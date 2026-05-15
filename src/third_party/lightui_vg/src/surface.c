/*
 * surface.c — Pixel-buffer surface allocation and writers
 *
 * Portability:
 *   - Core types + `lui_surface_wrap` have zero dependencies.
 *   - `lui_surface_create/destroy/resize` use the compile-time allocator
 *     macros below (default: malloc/free). Override with
 *         -DLUI_VG_MALLOC=my_alloc -DLUI_VG_FREE=my_free
 *     or via the `lui_surface_create_ex()` callback form.
 *   - PPM encoding uses a user-supplied write callback; no stdio required.
 *   - PNG encoding is optional (compile with `LUI_VG_ENABLE_PNG`), and
 *     pulls in the header-only `stb_image_write.h` plus its libm usage.
 *   - Path-based save helpers (`lui_surface_save_*`) are compiled only when
 *     `LUI_VG_NO_STDIO` is NOT defined.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <lightui/surface.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(LUI_VG_MALLOC) || !defined(LUI_VG_FREE)
#  include <stdlib.h>
#endif
#ifndef LUI_VG_MALLOC
#  define LUI_VG_MALLOC(sz) malloc(sz)
#endif
#ifndef LUI_VG_FREE
#  define LUI_VG_FREE(p)    free(p)
#endif

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

static void *lui__default_alloc(void *ud, size_t sz) {
    (void)ud; return LUI_VG_MALLOC(sz);
}
static void lui__default_free(void *ud, void *p) {
    (void)ud; LUI_VG_FREE(p);
}

lui_surface_t *lui_surface_create_ex(int width, int height,
                                     lui_vg_alloc_fn alloc_cb,
                                     lui_vg_free_fn  free_cb,
                                     void *userdata)
{
    if (width <= 0 || height <= 0) return NULL;
    /* Reject sizes that would overflow size_t when multiplied by 4 bytes/pixel.
     * Matters on 32-bit hosts; on 64-bit the guard is effectively free. */
    if ((uint64_t)width * (uint64_t)height > (uint64_t)(SIZE_MAX / sizeof(uint32_t)))
        return NULL;
    if (!alloc_cb) alloc_cb = lui__default_alloc;
    if (!free_cb)  free_cb  = lui__default_free;

    lui_surface_t *s = (lui_surface_t *)alloc_cb(userdata, sizeof(lui_surface_t));
    if (!s) return NULL;

    size_t bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
    uint32_t *px = (uint32_t *)alloc_cb(userdata, bytes);
    if (!px) {
        free_cb(userdata, s);
        return NULL;
    }
    memset(px, 0, bytes);

    s->pixels    = px;
    s->width     = width;
    s->height    = height;
    s->stride    = width;
    s->dpi_scale = 1.0f;
    return s;
}

void lui_surface_destroy_ex(lui_surface_t *surface,
                            lui_vg_free_fn free_cb, void *userdata)
{
    if (!surface) return;
    if (!free_cb) free_cb = lui__default_free;
    free_cb(userdata, surface->pixels);
    free_cb(userdata, surface);
}

lui_surface_t *lui_surface_create(int width, int height)
{
    return lui_surface_create_ex(width, height, NULL, NULL, NULL);
}

void lui_surface_destroy(lui_surface_t *surface)
{
    lui_surface_destroy_ex(surface, NULL, NULL);
}

lui_surface_t lui_surface_wrap(uint32_t *pixels,
                                int width, int height, int stride)
{
    lui_surface_t s;
    s.pixels    = pixels;
    s.width     = width;
    s.height    = height;
    s.stride    = stride;
    s.dpi_scale = 1.0f;
    return s;
}

bool lui_surface_resize(lui_surface_t *surface, int new_width, int new_height)
{
    if (!surface || new_width <= 0 || new_height <= 0)
        return false;
    if ((uint64_t)new_width * (uint64_t)new_height > (uint64_t)(SIZE_MAX / sizeof(uint32_t)))
        return false;

    size_t bytes = (size_t)new_width * (size_t)new_height * sizeof(uint32_t);
    uint32_t *new_pixels = (uint32_t *)LUI_VG_MALLOC(bytes);
    if (!new_pixels) return false;
    memset(new_pixels, 0, bytes);

    LUI_VG_FREE(surface->pixels);
    surface->pixels = new_pixels;
    surface->width  = new_width;
    surface->height = new_height;
    surface->stride = new_width;
    return true;
}

/* -------------------------------------------------------------------------
 * PPM writer (callback-based, no stdio)
 * ------------------------------------------------------------------------- */

static int lui__emit(lui_vg_write_fn cb, void *ud, const void *buf, size_t n) {
    return (cb(ud, buf, n) == n) ? 0 : -1;
}

/* Minimal unsigned-int → decimal ASCII. */
static int lui__emit_uint(lui_vg_write_fn cb, void *ud, unsigned v) {
    char buf[16]; int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    else { char tmp[16]; int t = 0;
           while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
           while (t) buf[n++] = tmp[--t]; }
    return lui__emit(cb, ud, buf, (size_t)n);
}

int lui_surface_write_ppm(const lui_surface_t *surface,
                          lui_vg_write_fn write_cb, void *userdata)
{
    if (!surface || !write_cb) return -1;
    int w = surface->width, h = surface->height;

    /* Header: "P6\n<w> <h>\n255\n" */
    if (lui__emit(write_cb, userdata, "P6\n", 3) != 0) return -1;
    if (lui__emit_uint(write_cb, userdata, (unsigned)w) != 0) return -1;
    if (lui__emit(write_cb, userdata, " ", 1) != 0) return -1;
    if (lui__emit_uint(write_cb, userdata, (unsigned)h) != 0) return -1;
    if (lui__emit(write_cb, userdata, "\n255\n", 5) != 0) return -1;

    /* Chunked conversion keeps the stack buffer small (~3 KB) so this works
     * on constrained targets regardless of image width. */
    enum { CHUNK = 1024 };
    unsigned char rowbuf[CHUNK * 3];
    for (int y = 0; y < h; y++) {
        const uint32_t *row = surface->pixels + (size_t)y * surface->stride;
        int x = 0;
        while (x < w) {
            int n = (w - x) < CHUNK ? (w - x) : CHUNK;
            for (int i = 0; i < n; i++) {
                uint32_t px = row[x + i];
                rowbuf[i * 3 + 0] = (unsigned char)((px >> 16) & 0xFF);
                rowbuf[i * 3 + 1] = (unsigned char)((px >>  8) & 0xFF);
                rowbuf[i * 3 + 2] = (unsigned char)( px        & 0xFF);
            }
            if (lui__emit(write_cb, userdata, rowbuf, (size_t)n * 3) != 0)
                return -1;
            x += n;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * PNG / JPEG writers (optional; pull in stb_image_write + its libm usage)
 * ------------------------------------------------------------------------- */

#if defined(LUI_VG_ENABLE_PNG) || defined(LUI_VG_ENABLE_JPEG)

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../third_party/stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

typedef struct {
    lui_vg_write_fn cb;
    void *ud;
    int failed;
} lui__stbi_ctx;

static void lui__stbi_write_thunk(void *context, void *data, int size) {
    lui__stbi_ctx *c = (lui__stbi_ctx *)context;
    if (c->failed) return;
    if (c->cb(c->ud, data, (size_t)size) != (size_t)size) c->failed = 1;
}

/* Convert ARGB32 surface → packed RGBA (4 bytes/pixel) in a fresh buffer. */
static unsigned char *lui__surface_to_rgba(const lui_surface_t *surface)
{
    int w = surface->width, h = surface->height;
    if (w <= 0 || h <= 0) return NULL;
    if ((uint64_t)w * (uint64_t)h > (uint64_t)(SIZE_MAX / 4)) return NULL;
    unsigned char *rgba = (unsigned char *)LUI_VG_MALLOC((size_t)w * (size_t)h * 4);
    if (!rgba) return NULL;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = surface->pixels[y * surface->stride + x];
            int off = (y * w + x) * 4;
            rgba[off + 0] = (unsigned char)((px >> 16) & 0xFF);
            rgba[off + 1] = (unsigned char)((px >>  8) & 0xFF);
            rgba[off + 2] = (unsigned char)( px        & 0xFF);
            rgba[off + 3] = (unsigned char)((px >> 24) & 0xFF);
        }
    }
    return rgba;
}

/* Convert ARGB32 surface → packed RGB (3 bytes/pixel) — for JPEG which
 * doesn't support alpha. Alpha is flattened against opaque black. */
static unsigned char *lui__surface_to_rgb(const lui_surface_t *surface)
{
    int w = surface->width, h = surface->height;
    if (w <= 0 || h <= 0) return NULL;
    if ((uint64_t)w * (uint64_t)h > (uint64_t)(SIZE_MAX / 3)) return NULL;
    unsigned char *rgb = (unsigned char *)LUI_VG_MALLOC((size_t)w * (size_t)h * 3);
    if (!rgb) return NULL;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = surface->pixels[y * surface->stride + x];
            int off = (y * w + x) * 3;
            rgb[off + 0] = (unsigned char)((px >> 16) & 0xFF);
            rgb[off + 1] = (unsigned char)((px >>  8) & 0xFF);
            rgb[off + 2] = (unsigned char)( px        & 0xFF);
        }
    }
    return rgb;
}

#endif /* stb backend enabled */

#ifdef LUI_VG_ENABLE_PNG
int lui_surface_write_png(const lui_surface_t *surface,
                          lui_vg_write_fn write_cb, void *userdata)
{
    if (!surface || !write_cb) return -1;
    unsigned char *rgba = lui__surface_to_rgba(surface);
    if (!rgba) return -1;
    lui__stbi_ctx ctx = { write_cb, userdata, 0 };
    int ok = stbi_write_png_to_func(lui__stbi_write_thunk, &ctx,
                                    surface->width, surface->height,
                                    4, rgba, surface->width * 4);
    LUI_VG_FREE(rgba);
    return (ok && !ctx.failed) ? 0 : -1;
}
#else
int lui_surface_write_png(const lui_surface_t *surface,
                          lui_vg_write_fn write_cb, void *userdata)
{
    (void)surface; (void)write_cb; (void)userdata;
    return -1;
}
#endif /* LUI_VG_ENABLE_PNG */

#ifdef LUI_VG_ENABLE_JPEG
int lui_surface_write_jpeg(const lui_surface_t *surface,
                           lui_vg_write_fn write_cb, void *userdata,
                           int quality)
{
    if (!surface || !write_cb) return -1;
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;
    unsigned char *rgb = lui__surface_to_rgb(surface);
    if (!rgb) return -1;
    lui__stbi_ctx ctx = { write_cb, userdata, 0 };
    int ok = stbi_write_jpg_to_func(lui__stbi_write_thunk, &ctx,
                                    surface->width, surface->height,
                                    3, rgb, quality);
    LUI_VG_FREE(rgb);
    return (ok && !ctx.failed) ? 0 : -1;
}
#else
int lui_surface_write_jpeg(const lui_surface_t *surface,
                           lui_vg_write_fn write_cb, void *userdata,
                           int quality)
{
    (void)surface; (void)write_cb; (void)userdata; (void)quality;
    return -1;
}
#endif /* LUI_VG_ENABLE_JPEG */

/* -------------------------------------------------------------------------
 * Path-based save helpers (stdio)
 * ------------------------------------------------------------------------- */

#ifndef LUI_VG_NO_STDIO
#include <stdio.h>

static size_t lui__stdio_write(void *ud, const void *data, size_t len) {
    return fwrite(data, 1, len, (FILE *)ud);
}

int lui_surface_save_ppm(const lui_surface_t *surface, const char *path)
{
    if (!surface || !path) return -1;
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    int rc = lui_surface_write_ppm(surface, lui__stdio_write, fp);
    fclose(fp);
    return rc;
}

int lui_surface_save_png(const lui_surface_t *surface, const char *path)
{
    if (!surface || !path) return -1;
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    int rc = lui_surface_write_png(surface, lui__stdio_write, fp);
    fclose(fp);
    return rc;
}

int lui_surface_save_jpeg(const lui_surface_t *surface, const char *path,
                          int quality)
{
    if (!surface || !path) return -1;
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    int rc = lui_surface_write_jpeg(surface, lui__stdio_write, fp, quality);
    fclose(fp);
    return rc;
}

static int lui__ext_eq(const char *path, size_t len, const char *ext, size_t elen) {
    if (len < elen) return 0;
    const char *s = path + len - elen;
    for (size_t i = 0; i < elen; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        if (c != ext[i]) return 0;
    }
    return 1;
}

int lui_surface_save(const lui_surface_t *surface, const char *path)
{
    if (!surface || !path) return -1;
    size_t len = strlen(path);
    if (lui__ext_eq(path, len, ".png",  4)) return lui_surface_save_png(surface, path);
    if (lui__ext_eq(path, len, ".ppm",  4)) return lui_surface_save_ppm(surface, path);
    if (lui__ext_eq(path, len, ".jpg",  4)) return lui_surface_save_jpeg(surface, path, 90);
    if (lui__ext_eq(path, len, ".jpeg", 5)) return lui_surface_save_jpeg(surface, path, 90);
    return lui_surface_save_ppm(surface, path);
}

#endif /* !LUI_VG_NO_STDIO */
