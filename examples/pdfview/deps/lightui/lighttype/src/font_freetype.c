/*
 * src/fonts/font_freetype.c — FreeType/HarfBuzz font backend
 *
 * Compiled only when LTT_HAVE_FREETYPE is defined.
 * Provides init/destroy/shape/rasterize using FreeType for glyph
 * rasterization and HarfBuzz for text shaping.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef LTT_HAVE_FREETYPE

#include "font_internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>

#include <stdlib.h>
#include <string.h>

/* ---- Init / Destroy ----------------------------------------------------- */

int ltt__ft_init(ltt_font_t *f)
{
    FT_Library lib;
    if (FT_Init_FreeType(&lib) != 0)
        return -1;

    FT_Face face;
    if (FT_New_Memory_Face(lib, f->font_data, (FT_Long)f->font_data_len,
                           0, &face) != 0) {
        FT_Done_FreeType(lib);
        return -1;
    }

    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)f->pixel_size);

    /* Create HarfBuzz font from FreeType face */
    hb_font_t *hb_font = hb_ft_font_create(face, NULL);
    if (!hb_font) {
        FT_Done_Face(face);
        FT_Done_FreeType(lib);
        return -1;
    }

    f->ft_library = lib;
    f->ft_face = face;
    f->hb_font = hb_font;

    /* Compute metrics from FreeType */
    f->ft_ascent = (int)(face->size->metrics.ascender >> 6);
    f->ft_descent = (int)(-(face->size->metrics.descender >> 6));
    if (f->ft_descent < 0) f->ft_descent = -f->ft_descent;

    f->ft_line_height = (int)(face->size->metrics.height >> 6);
    if (f->ft_line_height <= 0)
        f->ft_line_height = f->ft_ascent + f->ft_descent;

    ltt__cache_init(&f->cache_ft);
    return 0;
}

void ltt__ft_destroy(ltt_font_t *f)
{
    if (f->hb_font) {
        hb_font_destroy((hb_font_t *)f->hb_font);
        f->hb_font = NULL;
    }
    if (f->ft_face) {
        FT_Done_Face((FT_Face)f->ft_face);
        f->ft_face = NULL;
    }
    if (f->ft_library) {
        FT_Done_FreeType((FT_Library)f->ft_library);
        f->ft_library = NULL;
    }
}

/* ---- Shaping via HarfBuzz ---------------------------------------------- */

void ltt__ft_shape(ltt_font_t *f, const char *utf8, int len,
                   ltt_shape_result_t *sr)
{
    sr->count = 0;
    sr->heap_alloc = 0;
    sr->glyph_ids = sr->gid_stack;
    sr->advances = sr->adv_stack;

    hb_buffer_t *buf = hb_buffer_create();
    if (!buf) return;

    hb_buffer_add_utf8(buf, utf8, len, 0, len);
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(buf);

    hb_shape((hb_font_t *)f->hb_font, buf, NULL, 0);

    unsigned int glyph_count = 0;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &glyph_count);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &glyph_count);

    if ((int)glyph_count > SHAPE_STACK_MAX) {
        sr->glyph_ids = (uint16_t *)malloc(glyph_count * sizeof(uint16_t));
        sr->advances = (int16_t *)malloc(glyph_count * sizeof(int16_t));
        if (!sr->glyph_ids || !sr->advances) {
            free(sr->glyph_ids);
            free(sr->advances);
            sr->glyph_ids = sr->gid_stack;
            sr->advances = sr->adv_stack;
            hb_buffer_destroy(buf);
            return;
        }
        sr->heap_alloc = 1;
    }

    for (unsigned int i = 0; i < glyph_count; i++) {
        sr->glyph_ids[i] = (uint16_t)info[i].codepoint;
        /* HarfBuzz positions are in 26.6 fixed point */
        sr->advances[i] = (int16_t)(pos[i].x_advance >> 6);
    }
    sr->count = (int)glyph_count;

    hb_buffer_destroy(buf);
}

/* ---- Rasterize and cache via FreeType ----------------------------------- */

ltt_glyph_entry_t *ltt__ft_rasterize_and_cache(ltt_font_t *f, uint16_t glyph_id)
{
    FT_Face face = (FT_Face)f->ft_face;

    /* Disable hinting so that FreeType's glyph bbox and bearing values
     * match the Custom backend's unhinted rasterizer.  With hinting the
     * outline is grid-fitted, shifting bearing_y by ±1 px systematically. */
    if (FT_Load_Glyph(face, glyph_id,
                       FT_LOAD_RENDER | FT_LOAD_NO_HINTING) != 0) {
        /* No glyph — cache empty entry */
        FT_Load_Glyph(face, glyph_id, FT_LOAD_NO_HINTING);
        int adv = (int)(face->glyph->advance.x >> 6);
        uint8_t dummy = 0;
        return ltt__cache_insert(&f->cache_ft, (uint32_t)glyph_id,
                                  &dummy, 0, 0, 0, 0, adv);
    }

    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap *bm = &slot->bitmap;

    int bm_w = (int)bm->width;
    int bm_h = (int)bm->rows;
    int adv = (int)(slot->advance.x >> 6);

    if (bm_w == 0 || bm_h == 0) {
        uint8_t dummy = 0;
        return ltt__cache_insert(&f->cache_ft, (uint32_t)glyph_id,
                                  &dummy, 0, 0, 0, 0, adv);
    }

    /* FreeType bitmap may have a different pitch than width, so convert */
    uint8_t *pixels = (uint8_t *)malloc((size_t)(bm_w * bm_h));
    if (!pixels) return NULL;

    for (int row = 0; row < bm_h; row++) {
        memcpy(pixels + row * bm_w,
               bm->buffer + row * bm->pitch,
               (size_t)bm_w);
    }

    ltt_glyph_entry_t *e = ltt__cache_insert(&f->cache_ft, (uint32_t)glyph_id,
                                              pixels, bm_w, bm_h,
                                              slot->bitmap_left,
                                              slot->bitmap_top,
                                              adv);
    free(pixels);
    return e;
}

#endif /* LTT_HAVE_FREETYPE */
