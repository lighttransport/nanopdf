/*
 * lightui/vg/vg_types.h — Primitive types used by the VG rasterizer
 *
 * Self-contained: depends only on <stdint.h> / <stdbool.h>.
 * Suitable for reuse outside lightui (drop src/vg/ + include/lightui/vg/ in
 * another project and this header is the only dependency).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIGHTUI_VG_TYPES_H
#define LIGHTUI_VG_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Color — packed 32-bit ARGB (0xAARRGGBB)
 * ------------------------------------------------------------------------- */
typedef uint32_t lui_color_t;

/* Construction */
#define LUI_COLOR_ARGB(a, r, g, b) \
    (((uint32_t)((a) & 0xFF) << 24) | \
     ((uint32_t)((r) & 0xFF) << 16) | \
     ((uint32_t)((g) & 0xFF) <<  8) | \
      (uint32_t)((b) & 0xFF))
#define LUI_COLOR_RGB(r, g, b)  LUI_COLOR_ARGB(0xFF, (r), (g), (b))

/* Component extraction */
#define LUI_COLOR_A(c)  (((c) >> 24) & 0xFF)
#define LUI_COLOR_R(c)  (((c) >> 16) & 0xFF)
#define LUI_COLOR_G(c)  (((c) >>  8) & 0xFF)
#define LUI_COLOR_B(c)  (((c)      ) & 0xFF)

/* Linear interpolation between two ARGB colours by weight @t in [0,1].
 * Out-of-range t is clamped to the endpoint; NaN t falls back to @a. */
static inline lui_color_t lui_color_lerp(lui_color_t a, lui_color_t b, float t)
{
    if (!(t > 0.0f)) return a;        /* covers t<=0 and t==NaN */
    if (t >= 1.0f)   return b;
    int ra = (int)LUI_COLOR_R(a), ga = (int)LUI_COLOR_G(a),
        ba = (int)LUI_COLOR_B(a), aa = (int)LUI_COLOR_A(a);
    int rb = (int)LUI_COLOR_R(b), gb = (int)LUI_COLOR_G(b),
        bb = (int)LUI_COLOR_B(b), ab = (int)LUI_COLOR_A(b);
    int r  = ra + (int)((rb - ra) * t);
    int g  = ga + (int)((gb - ga) * t);
    int bl = ba + (int)((bb - ba) * t);
    int al = aa + (int)((ab - aa) * t);
    return LUI_COLOR_ARGB(al, r, g, bl);
}

/* Named colours */
#define LUI_COLOR_TRANSPARENT  LUI_COLOR_ARGB(0x00, 0x00, 0x00, 0x00)
#define LUI_COLOR_BLACK        LUI_COLOR_RGB(0x00, 0x00, 0x00)
#define LUI_COLOR_WHITE        LUI_COLOR_RGB(0xFF, 0xFF, 0xFF)
#define LUI_COLOR_RED          LUI_COLOR_RGB(0xFF, 0x00, 0x00)
#define LUI_COLOR_GREEN        LUI_COLOR_RGB(0x00, 0xFF, 0x00)
#define LUI_COLOR_BLUE         LUI_COLOR_RGB(0x00, 0x00, 0xFF)
#define LUI_COLOR_YELLOW       LUI_COLOR_RGB(0xFF, 0xFF, 0x00)
#define LUI_COLOR_CYAN         LUI_COLOR_RGB(0x00, 0xFF, 0xFF)
#define LUI_COLOR_MAGENTA      LUI_COLOR_RGB(0xFF, 0x00, 0xFF)
#define LUI_COLOR_GRAY         LUI_COLOR_RGB(0x80, 0x80, 0x80)
#define LUI_COLOR_DARK_GRAY    LUI_COLOR_RGB(0x40, 0x40, 0x40)
#define LUI_COLOR_LIGHT_GRAY   LUI_COLOR_RGB(0xC0, 0xC0, 0xC0)

/* -------------------------------------------------------------------------
 * lui_result_t — status code for APIs that can fail
 *
 * Convention (rolled out opportunistically; existing int-returning
 * functions keep their 0/-1 protocol):
 *   - Constructors return NULL on failure.
 *   - "Do X, might fail" returns lui_result_t (LUI_OK == 0 on success).
 *   - "Query state / did X happen" stays bool.
 * ------------------------------------------------------------------------- */
typedef enum {
    LUI_OK               =  0,
    LUI_ERR_INVALID      = -1,   /* NULL / out-of-range / wrong-kind arg  */
    LUI_ERR_NOMEM        = -2,   /* allocation failure                    */
    LUI_ERR_UNSUPPORTED  = -3,   /* feature disabled at compile time      */
    LUI_ERR_IO           = -4,   /* write/read callback reported failure  */
} lui_result_t;

/* -------------------------------------------------------------------------
 * lui_point_t — 2D integer point
 * ------------------------------------------------------------------------- */
typedef struct {
    int x, y;
} lui_point_t;

/* -------------------------------------------------------------------------
 * lui_size_t — 2D integer size (non-negative)
 * ------------------------------------------------------------------------- */
typedef struct {
    int width, height;
} lui_size_t;

/* -------------------------------------------------------------------------
 * lui_rect_t — 2D integer rectangle (origin + size)
 * ------------------------------------------------------------------------- */
typedef struct {
    int x, y;
    int width, height;
} lui_rect_t;

/* Rectangle helpers (static inline so header-only with no LTO cost) */
static inline lui_rect_t lui_rect_make(int x, int y, int w, int h) {
    lui_rect_t r;
    r.x = x; r.y = y; r.width = w; r.height = h;
    return r;
}

static inline bool lui_rect_is_empty(const lui_rect_t *r) {
    return r->width <= 0 || r->height <= 0;
}

static inline bool lui_rect_contains_point(const lui_rect_t *r, int x, int y) {
    return x >= r->x && y >= r->y &&
           x < r->x + r->width && y < r->y + r->height;
}

static inline bool lui_rect_overlaps(const lui_rect_t *a, const lui_rect_t *b) {
    return a->x < b->x + b->width  && a->x + a->width  > b->x &&
           a->y < b->y + b->height && a->y + a->height > b->y;
}

static inline lui_rect_t lui_rect_intersect(const lui_rect_t *a,
                                             const lui_rect_t *b) {
    lui_rect_t r;
    int x1 = a->x > b->x ? a->x : b->x;
    int y1 = a->y > b->y ? a->y : b->y;
    int x2 = (a->x + a->width)  < (b->x + b->width)  ? (a->x + a->width)  : (b->x + b->width);
    int y2 = (a->y + a->height) < (b->y + b->height) ? (a->y + a->height) : (b->y + b->height);
    r.x = x1; r.y = y1;
    r.width  = x2 > x1 ? x2 - x1 : 0;
    r.height = y2 > y1 ? y2 - y1 : 0;
    return r;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIGHTUI_VG_TYPES_H */
