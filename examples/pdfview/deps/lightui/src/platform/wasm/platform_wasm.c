/*
 * platform_wasm.c — WebAssembly / Emscripten platform backend
 *
 * Renders into an HTML5 <canvas> element via emscripten_set_canvas_element_size
 * and copies the pixel buffer using EM_ASM / emscripten_run_script patterns.
 *
 * Build with emcc:
 *   emcc -DLUI_PLATFORM_WASM -I../../include \
 *        platform_wasm.c ../../surface.c ../../canvas.c ../../lightui.c \
 *        -s WASM=1 -s USE_SDL=0 -o out.html
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef LUI_PLATFORM_WASM

#include "../platform_internal.h"

#include <emscripten.h>
#include <emscripten/html5.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Platform-specific window struct
 * ------------------------------------------------------------------------- */
typedef struct {
    lui_window_t  base;            /* MUST be first */
    const char   *canvas_id;       /* CSS selector, e.g. "#canvas"       */
    double        device_pixel_ratio;

#define WASM_EVT_QUEUE_SIZE 128
    lui_event_t   event_queue[WASM_EVT_QUEUE_SIZE];
    int           evt_head;
    int           evt_tail;
    int           last_mouse_x;
    int           last_mouse_y;
} lui_window_wasm_t;

static lui_window_wasm_t *g_window = NULL;  /* single-window model for WASM */

static void wasm_push_event(lui_window_wasm_t *ww, const lui_event_t *ev)
{
    int next = (ww->evt_tail + 1) % WASM_EVT_QUEUE_SIZE;
    if (next == ww->evt_head) return;
    ww->event_queue[ww->evt_tail] = *ev;
    ww->evt_tail = next;
}

/* -------------------------------------------------------------------------
 * Emscripten HTML5 callbacks
 * ------------------------------------------------------------------------- */

static EM_BOOL wasm_key_callback(int eventType,
                                   const EmscriptenKeyboardEvent *ke,
                                   void *userData)
{
    lui_window_wasm_t *ww = (lui_window_wasm_t *)userData;
    lui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type           = (eventType == EMSCRIPTEN_EVENT_KEYDOWN)
                         ? LUI_EVENT_KEY_DOWN : LUI_EVENT_KEY_UP;
    ev.data.key.key      = (int)ke->keyCode;
    ev.data.key.scancode = (uint32_t)ke->location;
    ev.data.key.repeat   = ke->repeat;
    if (ke->shiftKey)   ev.data.key.mods |= LUI_MOD_SHIFT;
    if (ke->ctrlKey)    ev.data.key.mods |= LUI_MOD_CTRL;
    if (ke->altKey)     ev.data.key.mods |= LUI_MOD_ALT;
    if (ke->metaKey)    ev.data.key.mods |= LUI_MOD_SUPER;
    wasm_push_event(ww, &ev);
    return EM_FALSE;  /* allow browser default */
}

static EM_BOOL wasm_mouse_move_callback(int eventType,
                                          const EmscriptenMouseEvent *me,
                                          void *userData)
{
    (void)eventType;
    lui_window_wasm_t *ww = (lui_window_wasm_t *)userData;
    /*
     * Emscripten reports CSS (logical) coords; scale to physical pixels
     * to match the surface/layout coordinate system.
     */
    float s = ww->base.dpi_scale;
    lui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type              = LUI_EVENT_MOUSE_MOVE;
    ev.data.mouse_move.x  = (int)(me->targetX * s);
    ev.data.mouse_move.y  = (int)(me->targetY * s);
    ev.data.mouse_move.dx = (int)(me->movementX * s);
    ev.data.mouse_move.dy = (int)(me->movementY * s);
    ww->last_mouse_x = ev.data.mouse_move.x;
    ww->last_mouse_y = ev.data.mouse_move.y;
    wasm_push_event(ww, &ev);
    return EM_FALSE;
}

static EM_BOOL wasm_mouse_button_callback(int eventType,
                                           const EmscriptenMouseEvent *me,
                                           void *userData)
{
    lui_window_wasm_t *ww = (lui_window_wasm_t *)userData;
    float s = ww->base.dpi_scale;
    lui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (eventType == EMSCRIPTEN_EVENT_MOUSEDOWN)
               ? LUI_EVENT_MOUSE_DOWN : LUI_EVENT_MOUSE_UP;
    ev.data.mouse_button.x = (int)(me->targetX * s);
    ev.data.mouse_button.y = (int)(me->targetY * s);
    ev.data.mouse_button.button =
        (me->button == 0) ? LUI_MOUSE_LEFT  :
        (me->button == 1) ? LUI_MOUSE_MIDDLE : LUI_MOUSE_RIGHT;
    ev.data.mouse_button.clicks = (int)me->detail;
    wasm_push_event(ww, &ev);
    return EM_FALSE;
}

static EM_BOOL wasm_wheel_callback(int eventType,
                                    const EmscriptenWheelEvent *we,
                                    void *userData)
{
    (void)eventType;
    lui_window_wasm_t *ww = (lui_window_wasm_t *)userData;
    lui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type              = LUI_EVENT_SCROLL;
    ev.data.scroll.x      = ww->last_mouse_x;
    ev.data.scroll.y      = ww->last_mouse_y;
    ev.data.scroll.delta_x = (float)we->deltaX;
    ev.data.scroll.delta_y = (float)we->deltaY;
    wasm_push_event(ww, &ev);
    return EM_TRUE;  /* prevent page scroll */
}

static EM_BOOL wasm_resize_callback(int eventType,
                                     const EmscriptenUiEvent *ue,
                                     void *userData)
{
    (void)eventType; (void)ue;
    lui_window_wasm_t *ww = (lui_window_wasm_t *)userData;
    if (!ww) return EM_FALSE;

    double dpr = emscripten_get_device_pixel_ratio();
    int css_w, css_h;
    /* Re-read canvas CSS size */
    emscripten_get_canvas_element_size(ww->canvas_id, &css_w, &css_h);

    int phys_w = (int)(css_w * dpr);
    int phys_h = (int)(css_h * dpr);
    emscripten_set_canvas_element_size(ww->canvas_id, phys_w, phys_h);

    ww->base.logical_w    = css_w;
    ww->base.logical_h    = css_h;
    ww->base.dpi_scale    = (float)dpr;
    ww->device_pixel_ratio = dpr;
    lvg_surface_resize(&ww->base.surface, phys_w, phys_h);

    lui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type                = LUI_EVENT_WINDOW_RESIZE;
    ev.data.resize.width   = css_w;
    ev.data.resize.height  = css_h;
    ev.data.resize.dpi_scale = (float)dpr;
    wasm_push_event(ww, &ev);
    return EM_FALSE;
}

/* -------------------------------------------------------------------------
 * Platform ops implementation
 * ------------------------------------------------------------------------- */

static bool wasm_init(void)
{
    return true;
}

static void wasm_shutdown(void)
{
    g_window = NULL;
}

static lui_window_t *wasm_window_create(const char *title,
                                          int w, int h,
                                          uint32_t flags)
{
    (void)title; (void)flags;

    lui_window_wasm_t *ww =
        (lui_window_wasm_t *)calloc(1, sizeof(lui_window_wasm_t));
    if (!ww) return NULL;

    /* Default canvas selector; caller may override via canvas_id later */
    ww->canvas_id = "#canvas";

    double dpr = emscripten_get_device_pixel_ratio();
    ww->device_pixel_ratio = dpr;
    ww->base.logical_w   = w;
    ww->base.logical_h   = h;
    ww->base.dpi_scale   = (float)dpr;
    ww->base.should_close = false;

    int phys_w = (int)(w * dpr);
    int phys_h = (int)(h * dpr);

    emscripten_set_canvas_element_size(ww->canvas_id, phys_w, phys_h);
    if (!lvg_surface_resize(&ww->base.surface, phys_w, phys_h)) {
        free(ww);
        return NULL;
    }

    /* Register HTML5 event callbacks */
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,
                                    ww, EM_FALSE, wasm_key_callback);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,
                                   ww, EM_FALSE, wasm_key_callback);
    emscripten_set_mousemove_callback(ww->canvas_id,
                                       ww, EM_FALSE, wasm_mouse_move_callback);
    emscripten_set_mousedown_callback(ww->canvas_id,
                                       ww, EM_FALSE, wasm_mouse_button_callback);
    emscripten_set_mouseup_callback(ww->canvas_id,
                                     ww, EM_FALSE, wasm_mouse_button_callback);
    emscripten_set_wheel_callback(ww->canvas_id,
                                   ww, EM_FALSE, wasm_wheel_callback);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,
                                    ww, EM_FALSE, wasm_resize_callback);

    g_window = ww;
    return (lui_window_t *)ww;
}

static void wasm_window_destroy(lui_window_t *win)
{
    lui_window_wasm_t *ww = (lui_window_wasm_t *)win;
    if (!ww) return;
    free(ww->base.surface.pixels);
    ww->base.surface.pixels = NULL;
    if (g_window == ww) g_window = NULL;
    free(ww);
}

static void wasm_window_show(lui_window_t *win)   { (void)win; }
static void wasm_window_hide(lui_window_t *win)   { (void)win; }
static void wasm_window_set_title(lui_window_t *win, const char *title)
{
    (void)win;
    if (title) EM_ASM({ document.title = UTF8ToString($0); }, title);
}

static void wasm_window_get_size(const lui_window_t *win, int *w, int *h)
{
    if (w) *w = win->logical_w;
    if (h) *h = win->logical_h;
}

static void wasm_window_get_physical_size(const lui_window_t *win,
                                           int *w, int *h)
{
    if (w) *w = win->surface.width;
    if (h) *h = win->surface.height;
}

static lvg_surface_t *wasm_window_get_surface(lui_window_t *win)
{
    return &win->surface;
}

static void wasm_window_present(lui_window_t *win)
{
    lui_window_wasm_t *ww = (lui_window_wasm_t *)win;
    int phys_w = ww->base.surface.width;
    int phys_h = ww->base.surface.height;

    /*
     * Copy the ARGB pixel buffer to the HTML5 canvas via ImageData.
     * Note: HTML5 ImageData uses RGBA byte order, but our buffer is packed as
     * 0xAARRGGBB (host-endian uint32).  On little-endian hosts (all common
     * WASM targets) this means bytes in memory are [BB, GG, RR, AA], so we
     * need a channel swap to [RR, GG, BB, AA] = RGBA.
     *
     * The swapped copy is done in JS for efficiency using a Uint8ClampedArray
     * view and swapping R↔B channels.
     */
    EM_ASM({
        var phys_w = $0, phys_h = $1;
        var src_ptr = $2;
        var canvas_id = UTF8ToString($3);
        var canvas = document.querySelector(canvas_id);
        if (!canvas) return;
        var ctx = canvas.getContext('2d');
        var imgData = ctx.createImageData(phys_w, phys_h);
        var src = new Uint8Array(HEAPU8.buffer, src_ptr, phys_w * phys_h * 4);
        var dst = imgData.data;
        for (var i = 0; i < phys_w * phys_h; i++) {
            var b = i * 4;
            /* ARGB (0xAARRGGBB, little-endian) → bytes [BB,GG,RR,AA] */
            /* Swap to RGBA: [RR,GG,BB,AA]                              */
            dst[b + 0] = src[b + 2];  /* R */
            dst[b + 1] = src[b + 1];  /* G */
            dst[b + 2] = src[b + 0];  /* B */
            dst[b + 3] = src[b + 3];  /* A */
        }
        ctx.putImageData(imgData, 0, 0);
    }, phys_w, phys_h, (int)ww->base.surface.pixels, ww->canvas_id);
}

static bool wasm_window_poll_event(lui_window_t *win, lui_event_t *ev)
{
    lui_window_wasm_t *ww = (lui_window_wasm_t *)win;
    emscripten_sleep(0);  /* yield so JS events can be processed */
    if (ww->evt_head == ww->evt_tail) return false;
    *ev = ww->event_queue[ww->evt_head];
    ww->evt_head = (ww->evt_head + 1) % WASM_EVT_QUEUE_SIZE;
    return true;
}

static bool wasm_window_wait_event(lui_window_t *win, lui_event_t *ev)
{
    lui_window_wasm_t *ww = (lui_window_wasm_t *)win;
    while (ww->evt_head == ww->evt_tail)
        emscripten_sleep(8);
    *ev = ww->event_queue[ww->evt_head];
    ww->evt_head = (ww->evt_head + 1) % WASM_EVT_QUEUE_SIZE;
    return true;
}

/* -------------------------------------------------------------------------
 * Platform ops table
 * ------------------------------------------------------------------------- */
const lui_platform_ops_t lui_platform_ops = {
    .init                    = wasm_init,
    .shutdown                = wasm_shutdown,
    .window_create           = wasm_window_create,
    .window_destroy          = wasm_window_destroy,
    .window_show             = wasm_window_show,
    .window_hide             = wasm_window_hide,
    .window_set_title        = wasm_window_set_title,
    .window_get_size         = wasm_window_get_size,
    .window_get_physical_size = wasm_window_get_physical_size,
    .window_get_surface      = wasm_window_get_surface,
    .window_present          = wasm_window_present,
    .window_poll_event       = wasm_window_poll_event,
    .window_wait_event       = wasm_window_wait_event,
};

#endif /* LUI_PLATFORM_WASM */
