/*
 * export_record.c — Command recording buffer for canvas export
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <lightui/export.h>

#include <stdlib.h>
#include <string.h>

/* ---- Internal helpers --------------------------------------------------- */

static void ensure_cmd(lui_recorder_t *rec)
{
    if (rec->cmd_count >= rec->cmd_cap) {
        int new_cap = rec->cmd_cap ? rec->cmd_cap * 2 : 64;
        lui_cmd_t *p = (lui_cmd_t *)realloc(rec->cmds,
                                             (size_t)new_cap * sizeof(lui_cmd_t));
        if (!p) return;
        rec->cmds = p;
        rec->cmd_cap = new_cap;
    }
}

static int alloc_data(lui_recorder_t *rec, int bytes)
{
    if (bytes <= 0) return 0;
    while (rec->data_used + bytes > rec->data_cap) {
        int new_cap = rec->data_cap ? rec->data_cap * 2 : 4096;
        uint8_t *p = (uint8_t *)realloc(rec->data, (size_t)new_cap);
        if (!p) return -1;
        rec->data = p;
        rec->data_cap = new_cap;
    }
    int offset = rec->data_used;
    rec->data_used += bytes;
    return offset;
}

static lui_cmd_t *push_cmd(lui_recorder_t *rec, lui_cmd_type_t type)
{
    ensure_cmd(rec);
    if (rec->cmd_count >= rec->cmd_cap) return NULL;
    lui_cmd_t *c = &rec->cmds[rec->cmd_count++];
    memset(c, 0, sizeof(*c));
    c->type = type;
    return c;
}

/* ---- Lifecycle ---------------------------------------------------------- */

lui_recorder_t *lui_recorder_create(int width, int height)
{
    lui_recorder_t *rec = (lui_recorder_t *)calloc(1, sizeof(lui_recorder_t));
    if (!rec) return NULL;
    rec->width = width;
    rec->height = height;
    return rec;
}

void lui_recorder_destroy(lui_recorder_t *rec)
{
    if (!rec) return;
    free(rec->cmds);
    free(rec->data);
    free(rec);
}

void lui_recorder_reset(lui_recorder_t *rec)
{
    if (!rec) return;
    rec->cmd_count = 0;
    rec->data_used = 0;
}

/* ---- Recording functions ------------------------------------------------ */

void lui_rec_clear(lui_recorder_t *rec, lvg_color_t color)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_CLEAR);
    if (c) c->color = color;
}

void lui_rec_set_clip(lui_recorder_t *rec, int x, int y, int w, int h)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_SET_CLIP);
    if (c) {
        c->d.clip.rect.x = x;
        c->d.clip.rect.y = y;
        c->d.clip.rect.width = w;
        c->d.clip.rect.height = h;
    }
}

void lui_rec_reset_clip(lui_recorder_t *rec)
{
    push_cmd(rec, LUI_CMD_RESET_CLIP);
}

void lui_rec_fill_rect(lui_recorder_t *rec,
                       int x, int y, int w, int h, lvg_color_t color)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_FILL_RECT);
    if (c) {
        c->color = color;
        c->d.rect.x = x; c->d.rect.y = y;
        c->d.rect.w = w; c->d.rect.h = h;
    }
}

void lui_rec_stroke_rect(lui_recorder_t *rec,
                         int x, int y, int w, int h,
                         lvg_color_t color, int stroke_width)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_STROKE_RECT);
    if (c) {
        c->color = color;
        c->stroke_width = stroke_width;
        c->d.rect.x = x; c->d.rect.y = y;
        c->d.rect.w = w; c->d.rect.h = h;
    }
}

void lui_rec_fill_circle(lui_recorder_t *rec,
                         int cx, int cy, int r, lvg_color_t color)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_FILL_CIRCLE);
    if (c) {
        c->color = color;
        c->d.circle.cx = cx; c->d.circle.cy = cy;
        c->d.circle.r = r;
    }
}

void lui_rec_stroke_circle(lui_recorder_t *rec,
                           int cx, int cy, int r,
                           lvg_color_t color, int stroke_width)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_STROKE_CIRCLE);
    if (c) {
        c->color = color;
        c->stroke_width = stroke_width;
        c->d.circle.cx = cx; c->d.circle.cy = cy;
        c->d.circle.r = r;
    }
}

void lui_rec_fill_ellipse(lui_recorder_t *rec,
                          int cx, int cy, int rx, int ry, lvg_color_t color)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_FILL_ELLIPSE);
    if (c) {
        c->color = color;
        c->d.ellipse.cx = cx; c->d.ellipse.cy = cy;
        c->d.ellipse.rx = rx; c->d.ellipse.ry = ry;
    }
}

void lui_rec_stroke_ellipse(lui_recorder_t *rec,
                            int cx, int cy, int rx, int ry,
                            lvg_color_t color, int stroke_width)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_STROKE_ELLIPSE);
    if (c) {
        c->color = color;
        c->stroke_width = stroke_width;
        c->d.ellipse.cx = cx; c->d.ellipse.cy = cy;
        c->d.ellipse.rx = rx; c->d.ellipse.ry = ry;
    }
}

void lui_rec_fill_rounded_rect(lui_recorder_t *rec,
                               int x, int y, int w, int h,
                               int radius, lvg_color_t color)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_FILL_ROUNDED_RECT);
    if (c) {
        c->color = color;
        c->d.rounded_rect.x = x; c->d.rounded_rect.y = y;
        c->d.rounded_rect.w = w; c->d.rounded_rect.h = h;
        c->d.rounded_rect.radius = radius;
    }
}

void lui_rec_stroke_rounded_rect(lui_recorder_t *rec,
                                 int x, int y, int w, int h,
                                 int radius, lvg_color_t color,
                                 int stroke_width)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_STROKE_ROUNDED_RECT);
    if (c) {
        c->color = color;
        c->stroke_width = stroke_width;
        c->d.rounded_rect.x = x; c->d.rounded_rect.y = y;
        c->d.rounded_rect.w = w; c->d.rounded_rect.h = h;
        c->d.rounded_rect.radius = radius;
    }
}

void lui_rec_fill_triangle(lui_recorder_t *rec,
                           int x0, int y0, int x1, int y1,
                           int x2, int y2, lvg_color_t color)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_FILL_TRIANGLE);
    if (c) {
        c->color = color;
        c->d.triangle.x0 = x0; c->d.triangle.y0 = y0;
        c->d.triangle.x1 = x1; c->d.triangle.y1 = y1;
        c->d.triangle.x2 = x2; c->d.triangle.y2 = y2;
    }
}

void lui_rec_fill_polygon(lui_recorder_t *rec,
                          const lvg_point_t *points, int count,
                          lvg_color_t color)
{
    if (!points || count < 3) return;
    int bytes = count * (int)sizeof(lvg_point_t);
    int off = alloc_data(rec, bytes);
    if (off < 0) return;
    memcpy(rec->data + off, points, (size_t)bytes);

    lui_cmd_t *c = push_cmd(rec, LUI_CMD_FILL_POLYGON);
    if (c) {
        c->color = color;
        c->d.polygon.data_offset = off;
        c->d.polygon.count = count;
    }
}

void lui_rec_stroke_polygon(lui_recorder_t *rec,
                            const lvg_point_t *points, int count,
                            lvg_color_t color, int stroke_width)
{
    if (!points || count < 3) return;
    int bytes = count * (int)sizeof(lvg_point_t);
    int off = alloc_data(rec, bytes);
    if (off < 0) return;
    memcpy(rec->data + off, points, (size_t)bytes);

    lui_cmd_t *c = push_cmd(rec, LUI_CMD_STROKE_POLYGON);
    if (c) {
        c->color = color;
        c->stroke_width = stroke_width;
        c->d.polygon.data_offset = off;
        c->d.polygon.count = count;
    }
}

void lui_rec_draw_line(lui_recorder_t *rec,
                       int x0, int y0, int x1, int y1,
                       lvg_color_t color, int stroke_width)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_DRAW_LINE);
    if (c) {
        c->color = color;
        c->stroke_width = stroke_width;
        c->d.line.x0 = x0; c->d.line.y0 = y0;
        c->d.line.x1 = x1; c->d.line.y1 = y1;
    }
}

void lui_rec_draw_polyline(lui_recorder_t *rec,
                           const lvg_point_t *points, int count,
                           lvg_color_t color, int stroke_width)
{
    if (!points || count < 2) return;
    int bytes = count * (int)sizeof(lvg_point_t);
    int off = alloc_data(rec, bytes);
    if (off < 0) return;
    memcpy(rec->data + off, points, (size_t)bytes);

    lui_cmd_t *c = push_cmd(rec, LUI_CMD_DRAW_POLYLINE);
    if (c) {
        c->color = color;
        c->stroke_width = stroke_width;
        c->d.polyline.data_offset = off;
        c->d.polyline.count = count;
    }
}

void lui_rec_draw_line_aa(lui_recorder_t *rec,
                          float x0, float y0, float x1, float y1,
                          lvg_color_t color)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_DRAW_LINE_AA);
    if (c) {
        c->color = color;
        c->d.line_aa.x0 = x0; c->d.line_aa.y0 = y0;
        c->d.line_aa.x1 = x1; c->d.line_aa.y1 = y1;
    }
}

void lui_rec_draw_polyline_aa(lui_recorder_t *rec,
                              const float *xy_pairs, int count,
                              lvg_color_t color)
{
    if (!xy_pairs || count < 2) return;
    int bytes = count * 2 * (int)sizeof(float);
    int off = alloc_data(rec, bytes);
    if (off < 0) return;
    memcpy(rec->data + off, xy_pairs, (size_t)bytes);

    lui_cmd_t *c = push_cmd(rec, LUI_CMD_DRAW_POLYLINE_AA);
    if (c) {
        c->color = color;
        c->d.polyline_aa.data_offset = off;
        c->d.polyline_aa.count = count;
    }
}

void lui_rec_draw_line_dashed(lui_recorder_t *rec,
                              int x0, int y0, int x1, int y1,
                              lvg_color_t color, int stroke_width,
                              const lvg_dash_t *dash)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_DRAW_LINE_DASHED);
    if (!c) return;
    c->color = color;
    c->stroke_width = stroke_width;
    c->d.line_dashed.x0 = x0; c->d.line_dashed.y0 = y0;
    c->d.line_dashed.x1 = x1; c->d.line_dashed.y1 = y1;
    c->d.line_dashed.dash_count = 0;
    c->d.line_dashed.dash_phase = 0;

    if (dash && dash->pattern && dash->count > 0) {
        int bytes = dash->count * (int)sizeof(int);
        int off = alloc_data(rec, bytes);
        if (off >= 0) {
            memcpy(rec->data + off, dash->pattern, (size_t)bytes);
            c->d.line_dashed.dash_offset = off;
            c->d.line_dashed.dash_count = dash->count;
            c->d.line_dashed.dash_phase = dash->offset;
        }
    }
}

void lui_rec_draw_polyline_dashed(lui_recorder_t *rec,
                                  const lvg_point_t *points, int count,
                                  lvg_color_t color, int stroke_width,
                                  const lvg_dash_t *dash)
{
    if (!points || count < 2) return;

    int pts_bytes = count * (int)sizeof(lvg_point_t);
    int pts_off = alloc_data(rec, pts_bytes);
    if (pts_off < 0) return;
    memcpy(rec->data + pts_off, points, (size_t)pts_bytes);

    lui_cmd_t *c = push_cmd(rec, LUI_CMD_DRAW_POLYLINE_DASHED);
    if (!c) return;
    c->color = color;
    c->stroke_width = stroke_width;
    c->d.polyline_dashed.pts_offset = pts_off;
    c->d.polyline_dashed.pts_count = count;
    c->d.polyline_dashed.dash_count = 0;
    c->d.polyline_dashed.dash_phase = 0;

    if (dash && dash->pattern && dash->count > 0) {
        int bytes = dash->count * (int)sizeof(int);
        int off = alloc_data(rec, bytes);
        if (off >= 0) {
            memcpy(rec->data + off, dash->pattern, (size_t)bytes);
            c->d.polyline_dashed.dash_offset = off;
            c->d.polyline_dashed.dash_count = dash->count;
            c->d.polyline_dashed.dash_phase = dash->offset;
        }
    }
}

void lui_rec_draw_thick_polyline(lui_recorder_t *rec,
                                 const lvg_pointf_t *points, int count,
                                 lvg_color_t color, float width, bool closed)
{
    if (!points || count < 2) return;
    int bytes = count * (int)sizeof(lvg_pointf_t);
    int off = alloc_data(rec, bytes);
    if (off < 0) return;
    memcpy(rec->data + off, points, (size_t)bytes);

    lui_cmd_t *c = push_cmd(rec, LUI_CMD_DRAW_THICK_POLYLINE);
    if (c) {
        c->color = color;
        c->d.thick_polyline.data_offset = off;
        c->d.thick_polyline.count = count;
        c->d.thick_polyline.width = width;
        c->d.thick_polyline.closed = closed;
    }
}

void lui_rec_draw_arrow(lui_recorder_t *rec,
                        int x0, int y0, int x1, int y1,
                        lvg_color_t color, int stroke_width,
                        lvg_arrow_style_t head, lvg_arrow_style_t tail,
                        int head_size)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_DRAW_ARROW);
    if (c) {
        c->color = color;
        c->stroke_width = stroke_width;
        c->d.arrow.x0 = x0; c->d.arrow.y0 = y0;
        c->d.arrow.x1 = x1; c->d.arrow.y1 = y1;
        c->d.arrow.head = head;
        c->d.arrow.tail = tail;
        c->d.arrow.head_size = head_size;
    }
}

void lui_rec_fill_rect_hatched(lui_recorder_t *rec,
                               int x, int y, int w, int h,
                               lvg_color_t bg_color, lvg_color_t fg_color,
                               lvg_hatch_style_t style,
                               int spacing, int line_width)
{
    lui_cmd_t *c = push_cmd(rec, LUI_CMD_FILL_RECT_HATCHED);
    if (c) {
        c->color = bg_color;
        c->color2 = fg_color;
        c->d.hatch_rect.x = x; c->d.hatch_rect.y = y;
        c->d.hatch_rect.w = w; c->d.hatch_rect.h = h;
        c->d.hatch_rect.style = style;
        c->d.hatch_rect.spacing = spacing;
        c->d.hatch_rect.line_width = line_width;
    }
}

void lui_rec_fill_polygon_hatched(lui_recorder_t *rec,
                                  const lvg_point_t *points, int count,
                                  lvg_color_t bg_color, lvg_color_t fg_color,
                                  lvg_hatch_style_t style,
                                  int spacing, int line_width)
{
    if (!points || count < 3) return;
    int bytes = count * (int)sizeof(lvg_point_t);
    int off = alloc_data(rec, bytes);
    if (off < 0) return;
    memcpy(rec->data + off, points, (size_t)bytes);

    lui_cmd_t *c = push_cmd(rec, LUI_CMD_FILL_POLYGON_HATCHED);
    if (c) {
        c->color = bg_color;
        c->color2 = fg_color;
        c->d.hatch_polygon.data_offset = off;
        c->d.hatch_polygon.count = count;
        c->d.hatch_polygon.style = style;
        c->d.hatch_polygon.spacing = spacing;
        c->d.hatch_polygon.line_width = line_width;
    }
}

void lui_rec_draw_image(lui_recorder_t *rec,
                        int dst_x, int dst_y, int dst_w, int dst_h,
                        const lvg_surface_t *src, const lvg_rect_t *src_rect)
{
    if (!src || !src->pixels) return;

    int sx = 0, sy = 0, sw = src->width, sh = src->height;
    if (src_rect) {
        sx = src_rect->x; sy = src_rect->y;
        sw = src_rect->width; sh = src_rect->height;
    }
    if (sw <= 0 || sh <= 0) return;

    /* Copy pixel data row by row (respecting stride) */
    int bytes = sw * sh * (int)sizeof(uint32_t);
    int off = alloc_data(rec, bytes);
    if (off < 0) return;

    uint32_t *dst = (uint32_t *)(rec->data + off);
    for (int row = 0; row < sh; row++) {
        memcpy(dst + row * sw,
               src->pixels + (sy + row) * src->stride + sx,
               (size_t)sw * sizeof(uint32_t));
    }

    lui_cmd_t *c = push_cmd(rec, LUI_CMD_DRAW_IMAGE);
    if (c) {
        c->d.image.dst_x = dst_x; c->d.image.dst_y = dst_y;
        c->d.image.dst_w = dst_w; c->d.image.dst_h = dst_h;
        c->d.image.img_w = sw;    c->d.image.img_h = sh;
        c->d.image.img_stride = sw;
        c->d.image.data_offset = off;
    }
}

void lui_rec_draw_text(lui_recorder_t *rec,
                       int x, int y,
                       const char *utf8, int len,
                       const char *font_family, float font_size,
                       lvg_color_t color)
{
    if (!utf8) return;
    if (len < 0) len = (int)strlen(utf8);

    /* Copy text */
    int text_off = alloc_data(rec, len + 1);
    if (text_off < 0) return;
    memcpy(rec->data + text_off, utf8, (size_t)len);
    rec->data[text_off + len] = '\0';

    /* Copy font family */
    const char *fam = font_family ? font_family : "sans-serif";
    int fam_len = (int)strlen(fam);
    int font_off = alloc_data(rec, fam_len + 1);
    if (font_off < 0) return;
    memcpy(rec->data + font_off, fam, (size_t)fam_len + 1);

    lui_cmd_t *c = push_cmd(rec, LUI_CMD_DRAW_TEXT);
    if (c) {
        c->color = color;
        c->d.text.x = x;
        c->d.text.y = y;
        c->d.text.text_offset = text_off;
        c->d.text.text_len = len;
        c->d.text.font_offset = font_off;
        c->d.text.font_size = font_size;
    }
}
