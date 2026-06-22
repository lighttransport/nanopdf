/*
 * src/fonts/font_internal.h — Shared internal definitions for font backends
 *
 * Extracted from font.c to allow the FreeType/HarfBuzz backend
 * (font_freetype.c) to share cache types, LUT externs, and the
 * extended lui_font_s struct.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIGHTUI_FONT_INTERNAL_H
#define LIGHTUI_FONT_INTERNAL_H

#include <lightui/font.h>
#include "ttf_parse.h"

#include <stdint.h>

/* =========================================================================
 * Compile-time constants
 * ========================================================================= */

#define LUI_CACHE_SIZE      512
#define LUI_CACHE_MASK      (LUI_CACHE_SIZE - 1)
#define LUI_CACHE_MAX_FILL  384

#define LUI_ATLAS_DIM       512
#define LUI_ATLAS_PAD       1

#define LUI_SLOT_EMPTY(s)   (~(s))

/* =========================================================================
 * sRGB <-> linear LUTs (defined in font.c, used by both backends)
 * ========================================================================= */

extern uint16_t lui__s2l[256];
extern uint8_t  lui__l2s[4097];
extern int      lui__lut_ready;

void lui__lut_init(void);

/* =========================================================================
 * Glyph cache types
 * ========================================================================= */

typedef struct {
    uint32_t  glyph_id;
    int       occupied;
    uint8_t  *atlas_ptr;
    int16_t   width, height;
    int16_t   bearing_x, bearing_y;
    int16_t   advance_x;
    int16_t   lru_prev, lru_next;
} lui_glyph_entry_t;

typedef struct {
    lui_glyph_entry_t slots[LUI_CACHE_SIZE];
    int               count;
    int               lru_head;
    int               lru_tail;
    uint8_t           atlas[LUI_ATLAS_DIM * LUI_ATLAS_DIM];
    int               shelf_x;
    int               shelf_y;
    int               shelf_h;
} lui_glyph_cache_t;

/* =========================================================================
 * Cache operations (defined in font.c)
 * ========================================================================= */

void              lui__cache_init(lui_glyph_cache_t *c);
void              lui__cache_flush(lui_glyph_cache_t *c);
lui_glyph_entry_t *lui__cache_lookup(lui_glyph_cache_t *c, uint32_t glyph_id);
lui_glyph_entry_t *lui__cache_insert(lui_glyph_cache_t *c,
                                      uint32_t glyph_id,
                                      const uint8_t *bitmap,
                                      int bm_w, int bm_h,
                                      int bearing_x, int bearing_y,
                                      int advance_x);

/* =========================================================================
 * Shaping result
 * ========================================================================= */

#define SHAPE_STACK_MAX 256

typedef struct {
    uint16_t *glyph_ids;
    int16_t  *advances;
    int       count;
    uint16_t  gid_stack[SHAPE_STACK_MAX];
    int16_t   adv_stack[SHAPE_STACK_MAX];
    int       heap_alloc;
} lui_shape_result_t;

/* =========================================================================
 * Font context — extended with dual-backend state
 * ========================================================================= */

struct lui_font_s {
    uint8_t          *font_data;
    size_t            font_data_len;
    ttf_font_t        ttf;
    float             scale;
    int               pixel_size;
    int               ascent;
    int               descent;
    int               line_height;

    /* Active backend */
    lui_font_backend_t active_backend;

    /* Custom backend cache (always present) */
    lui_glyph_cache_t  cache_custom;

#ifdef LUI_HAVE_FREETYPE
    /* FreeType/HarfBuzz backend state — opaque pointers to avoid header
     * pollution. Actual types (FT_Face, hb_font_t*) are cast in
     * font_freetype.c only. */
    void              *ft_face;      /* FT_Face */
    void              *ft_library;   /* FT_Library */
    void              *hb_font;      /* hb_font_t* */
    lui_glyph_cache_t  cache_ft;
    int                ft_ascent;
    int                ft_descent;
    int                ft_line_height;
#endif
};

/* =========================================================================
 * Blit function (shared by both backends, defined in font.c)
 * ========================================================================= */

void lui__blit_glyph(lui_canvas_t             *canvas,
                     int pen_x, int pen_y,
                     const lui_glyph_entry_t  *g,
                     uint8_t tr, uint8_t tg, uint8_t tb,
                     unsigned text_alpha);

/* =========================================================================
 * FreeType backend functions (defined in font_freetype.c)
 * ========================================================================= */

#ifdef LUI_HAVE_FREETYPE
int  lui__ft_init(lui_font_t *f);
void lui__ft_destroy(lui_font_t *f);
void lui__ft_shape(lui_font_t *f, const char *utf8, int len,
                   lui_shape_result_t *sr);
lui_glyph_entry_t *lui__ft_rasterize_and_cache(lui_font_t *f, uint16_t glyph_id);
#endif

#endif /* LIGHTUI_FONT_INTERNAL_H */
