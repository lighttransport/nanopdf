/*
 * platform_wayland.c — Linux Wayland platform backend
 *
 * Uses wl_shm for CPU-side pixel buffers and the XDG-shell stable protocol
 * for window management.
 *
 * Build notes
 * -----------
 * The xdg-shell protocol C glue (xdg-shell-client-protocol.h / .c) must be
 * generated with wayland-scanner before building:
 *
 *   wayland-scanner client-header \
 *     /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
 *     xdg-shell-client-protocol.h
 *
 *   wayland-scanner private-code \
 *     /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
 *     xdg-shell-client-protocol.c
 *
 * The CMake / Meson / xmake build scripts do this automatically.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef LUI_PLATFORM_WAYLAND

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../platform_internal.h"
#include "../../internal/lui_log.h"

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * Global Wayland objects (shared across windows)
 * ------------------------------------------------------------------------- */
static struct wl_display    *g_display    = NULL;
static struct wl_registry   *g_registry   = NULL;
static struct wl_compositor *g_compositor = NULL;
static struct wl_shm        *g_shm        = NULL;
static struct xdg_wm_base   *g_xdg_wm_base = NULL;

/* -------------------------------------------------------------------------
 * Platform-specific window struct
 * ------------------------------------------------------------------------- */
typedef struct {
    lui_window_t           base;        /* MUST be first */

    struct wl_surface      *wl_surface;
    struct xdg_surface     *xdg_surface;
    struct xdg_toplevel    *xdg_toplevel;
    struct wl_shm_pool     *shm_pool;
    struct wl_buffer       *wl_buffer;

    /* Shared memory backing store */
    uint32_t               *shm_data;
    size_t                  shm_size;
    int                     shm_fd;

    bool                    configured;  /* xdg_surface configure received */

#define WAYLAND_EVT_QUEUE_SIZE 64
    lui_event_t             event_queue[WAYLAND_EVT_QUEUE_SIZE];
    int                     evt_head;
    int                     evt_tail;
} lui_window_wayland_t;

static void wayland_window_destroy(lui_window_t *win);

static void wl_push_event(lui_window_wayland_t *ww, const lui_event_t *ev)
{
    int next = (ww->evt_tail + 1) % WAYLAND_EVT_QUEUE_SIZE;
    if (next == ww->evt_head) return;
    ww->event_queue[ww->evt_tail] = *ev;
    ww->evt_tail = next;
}

/* -------------------------------------------------------------------------
 * wl_shm helper — create anonymous shared memory
 * ------------------------------------------------------------------------- */
static int create_shm_fd(size_t size)
{
    int fd = -1;
#ifdef __linux__
    fd = memfd_create("lightui-shm", 0);
#endif
    if (fd < 0) {
        char name[32];
        snprintf(name, sizeof(name), "/lightui-shm-%d", (int)getpid());
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) shm_unlink(name);
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool wl_create_shm_buffer(lui_window_wayland_t *ww,
                                   int width, int height)
{
    size_t size = (size_t)(width * height * 4);

    /* Destroy existing */
    if (ww->wl_buffer) { wl_buffer_destroy(ww->wl_buffer); ww->wl_buffer = NULL; }
    if (ww->shm_pool)  { wl_shm_pool_destroy(ww->shm_pool); ww->shm_pool = NULL; }
    if (ww->shm_data && ww->shm_data != MAP_FAILED)
        munmap(ww->shm_data, ww->shm_size);
    if (ww->shm_fd >= 0) { close(ww->shm_fd); ww->shm_fd = -1; }

    ww->shm_fd = create_shm_fd(size);
    if (ww->shm_fd < 0) return false;

    ww->shm_data = (uint32_t *)mmap(NULL, size,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, ww->shm_fd, 0);
    if (ww->shm_data == MAP_FAILED) {
        close(ww->shm_fd);
        ww->shm_fd = -1;
        return false;
    }
    ww->shm_size = size;

    ww->shm_pool = wl_shm_create_pool(g_shm, ww->shm_fd, (int32_t)size);
    /* WL_SHM_FORMAT_ARGB8888 matches our 0xAARRGGBB pixel format */
    ww->wl_buffer = wl_shm_pool_create_buffer(
        ww->shm_pool,
        0,                    /* offset */
        width, height,
        width * 4,            /* stride in bytes */
        WL_SHM_FORMAT_ARGB8888
    );

    /* Point the surface struct at the shared buffer */
    ww->base.surface.pixels = ww->shm_data;
    ww->base.surface.width  = width;
    ww->base.surface.height = height;
    ww->base.surface.stride = width;
    return true;
}

/* -------------------------------------------------------------------------
 * xdg_wm_base ping/pong
 * ------------------------------------------------------------------------- */
static void xdg_wm_base_ping(void *data,
                               struct xdg_wm_base *xdg_wm_base,
                               uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}
static const struct xdg_wm_base_listener g_xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

/* -------------------------------------------------------------------------
 * xdg_surface configure
 * ------------------------------------------------------------------------- */
static void xdg_surface_configure(void *data,
                                   struct xdg_surface *xdg_surface,
                                   uint32_t serial)
{
    lui_window_wayland_t *ww = (lui_window_wayland_t *)data;
    xdg_surface_ack_configure(xdg_surface, serial);
    ww->configured = true;
}
static const struct xdg_surface_listener g_xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

/* -------------------------------------------------------------------------
 * xdg_toplevel events
 * ------------------------------------------------------------------------- */
static void xdg_toplevel_configure(void *data,
                                    struct xdg_toplevel *toplevel,
                                    int32_t width, int32_t height,
                                    struct wl_array *states)
{
    (void)toplevel; (void)states;
    lui_window_wayland_t *ww = (lui_window_wayland_t *)data;
    if (width > 0 && height > 0 &&
        (width != ww->base.logical_w || height != ww->base.logical_h)) {
        ww->base.logical_w = width;
        ww->base.logical_h = height;
        wl_create_shm_buffer(ww, width, height);

        lui_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type               = LUI_EVENT_WINDOW_RESIZE;
        ev.data.resize.width  = width;
        ev.data.resize.height = height;
        ev.data.resize.dpi_scale = ww->base.dpi_scale;
        wl_push_event(ww, &ev);
    }
}
static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    lui_window_wayland_t *ww = (lui_window_wayland_t *)data;
    ww->base.should_close = true;
    lui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LUI_EVENT_QUIT;
    wl_push_event(ww, &ev);
}
static const struct xdg_toplevel_listener g_xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close     = xdg_toplevel_close,
};

/* -------------------------------------------------------------------------
 * wl_registry
 * ------------------------------------------------------------------------- */
static void registry_global(void *data,
                              struct wl_registry *registry,
                              uint32_t name,
                              const char *interface,
                              uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        g_compositor = (struct wl_compositor *)wl_registry_bind(
            registry, name, &wl_compositor_interface,
            (version < 4) ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        g_shm = (struct wl_shm *)wl_registry_bind(
            registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        g_xdg_wm_base = (struct xdg_wm_base *)wl_registry_bind(
            registry, name, &xdg_wm_base_interface,
            (version < 2) ? version : 2);
        xdg_wm_base_add_listener(g_xdg_wm_base,
                                   &g_xdg_wm_base_listener, NULL);
    }
}
static void registry_global_remove(void *data,
                                    struct wl_registry *registry,
                                    uint32_t name)
{
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener g_registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* -------------------------------------------------------------------------
 * Platform ops implementation
 * ------------------------------------------------------------------------- */

static bool wayland_init(void)
{
    g_display = wl_display_connect(NULL);
    if (!g_display) {
        LUI_LOG_ERR("wayland: cannot connect to Wayland display");
        return false;
    }
    g_registry = wl_display_get_registry(g_display);
    wl_registry_add_listener(g_registry, &g_registry_listener, NULL);
    wl_display_roundtrip(g_display);
    wl_display_roundtrip(g_display);  /* second round to bind listeners */

    if (!g_compositor || !g_shm || !g_xdg_wm_base) {
        LUI_LOG_ERR("wayland: required Wayland interfaces missing");
        return false;
    }
    return true;
}

static void wayland_shutdown(void)
{
    if (g_xdg_wm_base) { xdg_wm_base_destroy(g_xdg_wm_base); g_xdg_wm_base = NULL; }
    if (g_shm)          { wl_shm_destroy(g_shm);               g_shm = NULL; }
    if (g_compositor)   { wl_compositor_destroy(g_compositor);  g_compositor = NULL; }
    if (g_registry)     { wl_registry_destroy(g_registry);      g_registry = NULL; }
    if (g_display)      { wl_display_disconnect(g_display);      g_display = NULL; }
}

static lui_window_t *wayland_window_create(const char *title,
                                            int w, int h,
                                            uint32_t flags)
{
    (void)flags;
    lui_window_wayland_t *ww =
        (lui_window_wayland_t *)calloc(1, sizeof(lui_window_wayland_t));
    if (!ww) return NULL;

    ww->base.logical_w   = w;
    ww->base.logical_h   = h;
    ww->base.dpi_scale   = 1.0f;  /* TODO: wl_output fractional scale */
    ww->base.should_close = false;
    ww->shm_fd = -1;

    ww->wl_surface = wl_compositor_create_surface(g_compositor);
    if (!ww->wl_surface) { free(ww); return NULL; }

    ww->xdg_surface = xdg_wm_base_get_xdg_surface(g_xdg_wm_base,
                                                     ww->wl_surface);
    xdg_surface_add_listener(ww->xdg_surface, &g_xdg_surface_listener, ww);

    ww->xdg_toplevel = xdg_surface_get_toplevel(ww->xdg_surface);
    xdg_toplevel_add_listener(ww->xdg_toplevel, &g_xdg_toplevel_listener, ww);

    if (title) xdg_toplevel_set_title(ww->xdg_toplevel, title);
    if (!(flags & LUI_WINDOW_RESIZABLE))
        xdg_toplevel_set_max_size(ww->xdg_toplevel, w, h);

    wl_surface_commit(ww->wl_surface);
    wl_display_roundtrip(g_display);   /* receive configure */

    if (!wl_create_shm_buffer(ww, w, h)) {
        /* wayland_window_destroy will release all resources and free ww */
        wayland_window_destroy((lui_window_t *)ww);
        return NULL;
    }

    return (lui_window_t *)ww;
}

static void wayland_window_destroy(lui_window_t *win)
{
    lui_window_wayland_t *ww = (lui_window_wayland_t *)win;
    if (!ww) return;

    if (ww->wl_buffer)    { wl_buffer_destroy(ww->wl_buffer);           ww->wl_buffer = NULL; }
    if (ww->shm_pool)     { wl_shm_pool_destroy(ww->shm_pool);          ww->shm_pool = NULL; }
    if (ww->shm_data && ww->shm_data != MAP_FAILED)
        munmap(ww->shm_data, ww->shm_size);
    if (ww->shm_fd >= 0)  { close(ww->shm_fd); ww->shm_fd = -1; }
    if (ww->xdg_toplevel) { xdg_toplevel_destroy(ww->xdg_toplevel);     ww->xdg_toplevel = NULL; }
    if (ww->xdg_surface)  { xdg_surface_destroy(ww->xdg_surface);       ww->xdg_surface = NULL; }
    if (ww->wl_surface)   { wl_surface_destroy(ww->wl_surface);         ww->wl_surface = NULL; }

    /* Pixel buffer is the shm mapping — not separately free()'d */
    ww->base.surface.pixels = NULL;
    free(ww);
}

static void wayland_window_show(lui_window_t *win)
{
    lui_window_wayland_t *ww = (lui_window_wayland_t *)win;
    wl_surface_commit(ww->wl_surface);
    wl_display_flush(g_display);
}

static void wayland_window_hide(lui_window_t *win)
{
    /* Wayland has no direct "hide" concept; attach a NULL buffer */
    lui_window_wayland_t *ww = (lui_window_wayland_t *)win;
    wl_surface_attach(ww->wl_surface, NULL, 0, 0);
    wl_surface_commit(ww->wl_surface);
    wl_display_flush(g_display);
}

static void wayland_window_set_title(lui_window_t *win, const char *title)
{
    lui_window_wayland_t *ww = (lui_window_wayland_t *)win;
    if (title) xdg_toplevel_set_title(ww->xdg_toplevel, title);
}

static void wayland_window_get_size(const lui_window_t *win, int *w, int *h)
{
    if (w) *w = win->logical_w;
    if (h) *h = win->logical_h;
}

static void wayland_window_get_physical_size(const lui_window_t *win,
                                              int *w, int *h)
{
    if (w) *w = win->surface.width;
    if (h) *h = win->surface.height;
}

static lvg_surface_t *wayland_window_get_surface(lui_window_t *win)
{
    return &win->surface;
}

static void wayland_window_present(lui_window_t *win)
{
    lui_window_wayland_t *ww = (lui_window_wayland_t *)win;
    wl_surface_attach(ww->wl_surface, ww->wl_buffer, 0, 0);
    wl_surface_damage_buffer(ww->wl_surface, 0, 0,
                              ww->base.surface.width,
                              ww->base.surface.height);
    wl_surface_commit(ww->wl_surface);
    wl_display_flush(g_display);
}

static void wayland_window_present_rect(lui_window_t *win, const lvg_rect_t *dirty)
{
    lui_window_wayland_t *ww = (lui_window_wayland_t *)win;
    if (!dirty || dirty->width <= 0 || dirty->height <= 0) {
        wayland_window_present(win);
        return;
    }

    int x0 = dirty->x;
    int y0 = dirty->y;
    int x1 = dirty->x + dirty->width;
    int y1 = dirty->y + dirty->height;
    int phys_w = ww->base.surface.width;
    int phys_h = ww->base.surface.height;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > phys_w) x1 = phys_w;
    if (y1 > phys_h) y1 = phys_h;
    if (x1 <= x0 || y1 <= y0)
        return;

    wl_surface_attach(ww->wl_surface, ww->wl_buffer, 0, 0);
    wl_surface_damage_buffer(ww->wl_surface, x0, y0, x1 - x0, y1 - y0);
    wl_surface_commit(ww->wl_surface);
    wl_display_flush(g_display);
}

static bool wayland_window_poll_event(lui_window_t *win, lui_event_t *ev)
{
    lui_window_wayland_t *ww = (lui_window_wayland_t *)win;
    wl_display_dispatch_pending(g_display);
    if (ww->evt_head == ww->evt_tail) return false;
    *ev = ww->event_queue[ww->evt_head];
    ww->evt_head = (ww->evt_head + 1) % WAYLAND_EVT_QUEUE_SIZE;
    return true;
}

static bool wayland_window_wait_event(lui_window_t *win, lui_event_t *ev)
{
    lui_window_wayland_t *ww = (lui_window_wayland_t *)win;
    for (;;) {
        wl_display_dispatch(g_display);
        if (ww->evt_head != ww->evt_tail) {
            *ev = ww->event_queue[ww->evt_head];
            ww->evt_head = (ww->evt_head + 1) % WAYLAND_EVT_QUEUE_SIZE;
            return true;
        }
    }
}

/* -------------------------------------------------------------------------
 * Platform ops table
 * ------------------------------------------------------------------------- */
const lui_platform_ops_t lui_platform_ops = {
    .init                    = wayland_init,
    .shutdown                = wayland_shutdown,
    .window_create           = wayland_window_create,
    .window_destroy          = wayland_window_destroy,
    .window_show             = wayland_window_show,
    .window_hide             = wayland_window_hide,
    .window_set_title        = wayland_window_set_title,
    .window_get_size         = wayland_window_get_size,
    .window_get_physical_size = wayland_window_get_physical_size,
    .window_get_surface      = wayland_window_get_surface,
    .window_present          = wayland_window_present,
    .window_present_rect     = wayland_window_present_rect,
    .window_poll_event       = wayland_window_poll_event,
    .window_wait_event       = wayland_window_wait_event,
};

#endif /* LUI_PLATFORM_WAYLAND */
