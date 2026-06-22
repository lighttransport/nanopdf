/*
 * export_svg.c — SVG serialization from recorded canvas commands
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <lightui/export.h>

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Growing string buffer ---------------------------------------------- */

typedef struct {
    char *data;
    int   len;
    int   cap;
    int   failed;  /* sticky: once realloc fails, all further writes no-op */
} strbuf_t;

static void sb_init(strbuf_t *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->failed = 0;
}

static void sb_ensure(strbuf_t *sb, int need)
{
    if (sb->failed) return;
    if (need < 0 || sb->len > INT_MAX - need) { sb->failed = 1; return; }
    if (sb->len + need <= sb->cap) return;
    int new_cap = sb->cap ? sb->cap * 2 : 4096;
    while (new_cap < sb->len + need) {
        if (new_cap > INT_MAX / 2) { sb->failed = 1; return; }
        new_cap *= 2;
    }
    char *p = (char *)realloc(sb->data, (size_t)new_cap);
    if (!p) { sb->failed = 1; return; }
    sb->data = p;
    sb->cap = new_cap;
}

static void sb_append(strbuf_t *sb, const char *s, int len)
{
    if (len < 0) len = (int)strlen(s);
    sb_ensure(sb, len);
    if (sb->failed) return;
    memcpy(sb->data + sb->len, s, (size_t)len);
    sb->len += len;
}

static void sb_printf(strbuf_t *sb, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) sb_append(sb, buf, n);
}

/* ---- Colour helpers ----------------------------------------------------- */

static void svg_fill(strbuf_t *sb, lvg_color_t c)
{
    int a = LVG_COLOR_A(c);
    int r = LVG_COLOR_R(c);
    int g = LVG_COLOR_G(c);
    int b = LVG_COLOR_B(c);
    if (a == 0) {
        sb_append(sb, "fill=\"none\"", -1);
    } else {
        sb_printf(sb, "fill=\"rgb(%d,%d,%d)\"", r, g, b);
        if (a < 255)
            sb_printf(sb, " fill-opacity=\"%.3f\"", a / 255.0);
    }
}

static void svg_stroke(strbuf_t *sb, lvg_color_t c, int w)
{
    int a = LVG_COLOR_A(c);
    int r = LVG_COLOR_R(c);
    int g = LVG_COLOR_G(c);
    int b = LVG_COLOR_B(c);
    sb_printf(sb, "stroke=\"rgb(%d,%d,%d)\" stroke-width=\"%d\"", r, g, b, w);
    if (a < 255)
        sb_printf(sb, " stroke-opacity=\"%.3f\"", a / 255.0);
}

/* ---- Base64 encoder for images ----------------------------------------- */

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(strbuf_t *sb, const uint8_t *data, int len)
{
    int i;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)data[i] << 16) |
                     ((uint32_t)data[i+1] << 8) |
                      (uint32_t)data[i+2];
        char out[4];
        out[0] = b64[(v >> 18) & 0x3F];
        out[1] = b64[(v >> 12) & 0x3F];
        out[2] = b64[(v >> 6) & 0x3F];
        out[3] = b64[v & 0x3F];
        sb_append(sb, out, 4);
    }
    if (i < len) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i+1] << 8;
        char out[4];
        out[0] = b64[(v >> 18) & 0x3F];
        out[1] = b64[(v >> 12) & 0x3F];
        out[2] = (i + 1 < len) ? b64[(v >> 6) & 0x3F] : '=';
        out[3] = '=';
        sb_append(sb, out, 4);
    }
}

/* ---- Minimal BMP encoder (for embedding images) ------------------------- */

static void emit_image_data_uri(strbuf_t *sb, const uint32_t *pixels,
                                int w, int h, int stride)
{
    /* BMP: 54-byte header + raw BGR rows (bottom-up, 4-byte aligned) */
    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int img_size = (row_bytes + pad) * h;
    int file_size = 54 + img_size;

    uint8_t *bmp = (uint8_t *)calloc(1, (size_t)file_size);
    if (!bmp) return;

    /* BMP file header */
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = (uint8_t)(file_size);
    bmp[3] = (uint8_t)(file_size >> 8);
    bmp[4] = (uint8_t)(file_size >> 16);
    bmp[5] = (uint8_t)(file_size >> 24);
    bmp[10] = 54; /* pixel data offset */

    /* DIB header (BITMAPINFOHEADER) */
    bmp[14] = 40; /* header size */
    bmp[18] = (uint8_t)(w);      bmp[19] = (uint8_t)(w >> 8);
    bmp[20] = (uint8_t)(w >> 16); bmp[21] = (uint8_t)(w >> 24);
    bmp[22] = (uint8_t)(h);      bmp[23] = (uint8_t)(h >> 8);
    bmp[24] = (uint8_t)(h >> 16); bmp[25] = (uint8_t)(h >> 24);
    bmp[26] = 1; /* planes */
    bmp[28] = 24; /* bpp */

    /* Pixel data (bottom-up) */
    for (int y = 0; y < h; y++) {
        const uint32_t *row = pixels + (h - 1 - y) * stride;
        uint8_t *dst = bmp + 54 + y * (row_bytes + pad);
        for (int x = 0; x < w; x++) {
            uint32_t px = row[x];
            dst[x * 3 + 0] = (uint8_t)(px & 0xFF);         /* B */
            dst[x * 3 + 1] = (uint8_t)((px >> 8) & 0xFF);  /* G */
            dst[x * 3 + 2] = (uint8_t)((px >> 16) & 0xFF); /* R */
        }
    }

    sb_append(sb, "data:image/bmp;base64,", -1);
    b64_encode(sb, bmp, file_size);
    free(bmp);
}

/* ---- Hatch pattern defs ------------------------------------------------- */

static int hatch_def_id_counter;

static int emit_hatch_def(strbuf_t *sb, lvg_hatch_style_t style,
                          lvg_color_t fg, int spacing, int line_width)
{
    int id = hatch_def_id_counter++;
    int r = LVG_COLOR_R(fg), g = LVG_COLOR_G(fg), b = LVG_COLOR_B(fg);
    int a = LVG_COLOR_A(fg);

    sb_printf(sb, "<pattern id=\"hatch%d\" patternUnits=\"userSpaceOnUse\" "
              "x=\"0\" y=\"0\" width=\"%d\" height=\"%d\">\n",
              id, spacing, spacing);

    char stroke_attr[128];
    int pos = snprintf(stroke_attr, sizeof(stroke_attr),
                       "stroke=\"rgb(%d,%d,%d)\" stroke-width=\"%d\"",
                       r, g, b, line_width);
    if (pos < 0 || pos >= (int)sizeof(stroke_attr)) {
        /* Refuse silent truncation — fall back to a minimal safe attr. */
        stroke_attr[0] = '\0';
        pos = 0;
    }
    if (a < 255) {
        int remain = (int)sizeof(stroke_attr) - pos;
        int n = snprintf(stroke_attr + pos, (size_t)remain,
                         " stroke-opacity=\"%.3f\"", a / 255.0);
        if (n < 0 || n >= remain) {
            /* Drop opacity rather than emit a truncated attribute. */
            stroke_attr[pos] = '\0';
        }
    }

    switch (style) {
    case LVG_HATCH_HORIZONTAL:
        sb_printf(sb, "  <line x1=\"0\" y1=\"%d\" x2=\"%d\" y2=\"%d\" %s/>\n",
                  spacing / 2, spacing, spacing / 2, stroke_attr);
        break;
    case LVG_HATCH_VERTICAL:
        sb_printf(sb, "  <line x1=\"%d\" y1=\"0\" x2=\"%d\" y2=\"%d\" %s/>\n",
                  spacing / 2, spacing / 2, spacing, stroke_attr);
        break;
    case LVG_HATCH_FORWARD_DIAG:
        sb_printf(sb, "  <line x1=\"0\" y1=\"%d\" x2=\"%d\" y2=\"0\" %s/>\n",
                  spacing, spacing, stroke_attr);
        break;
    case LVG_HATCH_BACK_DIAG:
        sb_printf(sb, "  <line x1=\"0\" y1=\"0\" x2=\"%d\" y2=\"%d\" %s/>\n",
                  spacing, spacing, stroke_attr);
        break;
    case LVG_HATCH_CROSS:
        sb_printf(sb, "  <line x1=\"0\" y1=\"%d\" x2=\"%d\" y2=\"%d\" %s/>\n",
                  spacing / 2, spacing, spacing / 2, stroke_attr);
        sb_printf(sb, "  <line x1=\"%d\" y1=\"0\" x2=\"%d\" y2=\"%d\" %s/>\n",
                  spacing / 2, spacing / 2, spacing, stroke_attr);
        break;
    case LVG_HATCH_DIAG_CROSS:
        sb_printf(sb, "  <line x1=\"0\" y1=\"%d\" x2=\"%d\" y2=\"0\" %s/>\n",
                  spacing, spacing, stroke_attr);
        sb_printf(sb, "  <line x1=\"0\" y1=\"0\" x2=\"%d\" y2=\"%d\" %s/>\n",
                  spacing, spacing, stroke_attr);
        break;
    }

    sb_append(sb, "</pattern>\n", -1);
    return id;
}

/* ---- SVG generation ----------------------------------------------------- */

static void emit_svg(strbuf_t *sb, const lui_recorder_t *rec)
{
    hatch_def_id_counter = 0;
    int clip_id = 0;
    int clip_open = 0;

    sb_printf(sb, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<svg xmlns=\"http://www.w3.org/2000/svg\" "
              "width=\"%d\" height=\"%d\" "
              "viewBox=\"0 0 %d %d\">\n",
              rec->width, rec->height, rec->width, rec->height);

    /* We may need <defs> later — collect them in a separate buffer,
       then insert. For simplicity, open <defs> now, we'll prune if empty. */
    strbuf_t defs;
    sb_init(&defs);

    strbuf_t body;
    sb_init(&body);

    for (int i = 0; i < rec->cmd_count; i++) {
        const lui_cmd_t *c = &rec->cmds[i];

        switch (c->type) {
        case LUI_CMD_CLEAR:
            sb_printf(&body, "<rect x=\"0\" y=\"0\" width=\"%d\" height=\"%d\" ",
                      rec->width, rec->height);
            svg_fill(&body, c->color);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_SET_CLIP: {
            if (clip_open) sb_append(&body, "</g>\n", -1);
            sb_printf(&defs, "<clipPath id=\"clip%d\"><rect x=\"%d\" y=\"%d\" "
                      "width=\"%d\" height=\"%d\"/></clipPath>\n",
                      clip_id,
                      c->d.clip.rect.x, c->d.clip.rect.y,
                      c->d.clip.rect.width, c->d.clip.rect.height);
            sb_printf(&body, "<g clip-path=\"url(#clip%d)\">\n", clip_id);
            clip_id++;
            clip_open = 1;
        } break;

        case LUI_CMD_RESET_CLIP:
            if (clip_open) {
                sb_append(&body, "</g>\n", -1);
                clip_open = 0;
            }
            break;

        case LUI_CMD_FILL_RECT:
            sb_printf(&body, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" ",
                      c->d.rect.x, c->d.rect.y, c->d.rect.w, c->d.rect.h);
            svg_fill(&body, c->color);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_STROKE_RECT:
            sb_printf(&body, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
                      "fill=\"none\" ",
                      c->d.rect.x, c->d.rect.y, c->d.rect.w, c->d.rect.h);
            svg_stroke(&body, c->color, c->stroke_width);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_FILL_CIRCLE:
            sb_printf(&body, "<circle cx=\"%d\" cy=\"%d\" r=\"%d\" ",
                      c->d.circle.cx, c->d.circle.cy, c->d.circle.r);
            svg_fill(&body, c->color);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_STROKE_CIRCLE:
            sb_printf(&body, "<circle cx=\"%d\" cy=\"%d\" r=\"%d\" "
                      "fill=\"none\" ",
                      c->d.circle.cx, c->d.circle.cy, c->d.circle.r);
            svg_stroke(&body, c->color, c->stroke_width);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_FILL_ELLIPSE:
            sb_printf(&body, "<ellipse cx=\"%d\" cy=\"%d\" rx=\"%d\" ry=\"%d\" ",
                      c->d.ellipse.cx, c->d.ellipse.cy,
                      c->d.ellipse.rx, c->d.ellipse.ry);
            svg_fill(&body, c->color);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_STROKE_ELLIPSE:
            sb_printf(&body, "<ellipse cx=\"%d\" cy=\"%d\" rx=\"%d\" ry=\"%d\" "
                      "fill=\"none\" ",
                      c->d.ellipse.cx, c->d.ellipse.cy,
                      c->d.ellipse.rx, c->d.ellipse.ry);
            svg_stroke(&body, c->color, c->stroke_width);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_FILL_ROUNDED_RECT:
            sb_printf(&body, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
                      "rx=\"%d\" ry=\"%d\" ",
                      c->d.rounded_rect.x, c->d.rounded_rect.y,
                      c->d.rounded_rect.w, c->d.rounded_rect.h,
                      c->d.rounded_rect.radius, c->d.rounded_rect.radius);
            svg_fill(&body, c->color);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_STROKE_ROUNDED_RECT:
            sb_printf(&body, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
                      "rx=\"%d\" ry=\"%d\" fill=\"none\" ",
                      c->d.rounded_rect.x, c->d.rounded_rect.y,
                      c->d.rounded_rect.w, c->d.rounded_rect.h,
                      c->d.rounded_rect.radius, c->d.rounded_rect.radius);
            svg_stroke(&body, c->color, c->stroke_width);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_FILL_TRIANGLE:
            sb_printf(&body, "<polygon points=\"%d,%d %d,%d %d,%d\" ",
                      c->d.triangle.x0, c->d.triangle.y0,
                      c->d.triangle.x1, c->d.triangle.y1,
                      c->d.triangle.x2, c->d.triangle.y2);
            svg_fill(&body, c->color);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_FILL_POLYGON: {
            const lvg_point_t *pts =
                (const lvg_point_t *)(rec->data + c->d.polygon.data_offset);
            sb_append(&body, "<polygon points=\"", -1);
            for (int j = 0; j < c->d.polygon.count; j++) {
                if (j > 0) sb_append(&body, " ", 1);
                sb_printf(&body, "%d,%d", pts[j].x, pts[j].y);
            }
            sb_append(&body, "\" ", -1);
            svg_fill(&body, c->color);
            sb_append(&body, "/>\n", -1);
        } break;

        case LUI_CMD_STROKE_POLYGON: {
            const lvg_point_t *pts =
                (const lvg_point_t *)(rec->data + c->d.polygon.data_offset);
            sb_append(&body, "<polygon points=\"", -1);
            for (int j = 0; j < c->d.polygon.count; j++) {
                if (j > 0) sb_append(&body, " ", 1);
                sb_printf(&body, "%d,%d", pts[j].x, pts[j].y);
            }
            sb_append(&body, "\" fill=\"none\" ", -1);
            svg_stroke(&body, c->color, c->stroke_width);
            sb_append(&body, "/>\n", -1);
        } break;

        case LUI_CMD_DRAW_LINE:
            sb_printf(&body, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" ",
                      c->d.line.x0, c->d.line.y0,
                      c->d.line.x1, c->d.line.y1);
            svg_stroke(&body, c->color, c->stroke_width);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_DRAW_POLYLINE: {
            const lvg_point_t *pts =
                (const lvg_point_t *)(rec->data + c->d.polyline.data_offset);
            sb_append(&body, "<polyline points=\"", -1);
            for (int j = 0; j < c->d.polyline.count; j++) {
                if (j > 0) sb_append(&body, " ", 1);
                sb_printf(&body, "%d,%d", pts[j].x, pts[j].y);
            }
            sb_append(&body, "\" fill=\"none\" ", -1);
            svg_stroke(&body, c->color, c->stroke_width);
            sb_append(&body, "/>\n", -1);
        } break;

        case LUI_CMD_DRAW_LINE_AA:
            sb_printf(&body, "<line x1=\"%.2f\" y1=\"%.2f\" "
                      "x2=\"%.2f\" y2=\"%.2f\" ",
                      c->d.line_aa.x0, c->d.line_aa.y0,
                      c->d.line_aa.x1, c->d.line_aa.y1);
            svg_stroke(&body, c->color, 1);
            sb_append(&body, "/>\n", -1);
            break;

        case LUI_CMD_DRAW_POLYLINE_AA: {
            const float *xy = (const float *)(rec->data + c->d.polyline_aa.data_offset);
            sb_append(&body, "<polyline points=\"", -1);
            for (int j = 0; j < c->d.polyline_aa.count; j++) {
                if (j > 0) sb_append(&body, " ", 1);
                sb_printf(&body, "%.2f,%.2f", xy[j * 2], xy[j * 2 + 1]);
            }
            sb_append(&body, "\" fill=\"none\" ", -1);
            svg_stroke(&body, c->color, 1);
            sb_append(&body, "/>\n", -1);
        } break;

        case LUI_CMD_DRAW_LINE_DASHED: {
            sb_printf(&body, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" ",
                      c->d.line_dashed.x0, c->d.line_dashed.y0,
                      c->d.line_dashed.x1, c->d.line_dashed.y1);
            svg_stroke(&body, c->color, c->stroke_width);
            if (c->d.line_dashed.dash_count > 0) {
                const int *pat = (const int *)(rec->data + c->d.line_dashed.dash_offset);
                sb_append(&body, " stroke-dasharray=\"", -1);
                for (int j = 0; j < c->d.line_dashed.dash_count; j++) {
                    if (j > 0) sb_append(&body, ",", 1);
                    sb_printf(&body, "%d", pat[j]);
                }
                sb_append(&body, "\"", -1);
                if (c->d.line_dashed.dash_phase)
                    sb_printf(&body, " stroke-dashoffset=\"%d\"",
                              c->d.line_dashed.dash_phase);
            }
            sb_append(&body, "/>\n", -1);
        } break;

        case LUI_CMD_DRAW_POLYLINE_DASHED: {
            const lvg_point_t *pts =
                (const lvg_point_t *)(rec->data + c->d.polyline_dashed.pts_offset);
            sb_append(&body, "<polyline points=\"", -1);
            for (int j = 0; j < c->d.polyline_dashed.pts_count; j++) {
                if (j > 0) sb_append(&body, " ", 1);
                sb_printf(&body, "%d,%d", pts[j].x, pts[j].y);
            }
            sb_append(&body, "\" fill=\"none\" ", -1);
            svg_stroke(&body, c->color, c->stroke_width);
            if (c->d.polyline_dashed.dash_count > 0) {
                const int *pat = (const int *)(rec->data + c->d.polyline_dashed.dash_offset);
                sb_append(&body, " stroke-dasharray=\"", -1);
                for (int j = 0; j < c->d.polyline_dashed.dash_count; j++) {
                    if (j > 0) sb_append(&body, ",", 1);
                    sb_printf(&body, "%d", pat[j]);
                }
                sb_append(&body, "\"", -1);
                if (c->d.polyline_dashed.dash_phase)
                    sb_printf(&body, " stroke-dashoffset=\"%d\"",
                              c->d.polyline_dashed.dash_phase);
            }
            sb_append(&body, "/>\n", -1);
        } break;

        case LUI_CMD_DRAW_THICK_POLYLINE: {
            const lvg_pointf_t *pts =
                (const lvg_pointf_t *)(rec->data + c->d.thick_polyline.data_offset);
            int n = c->d.thick_polyline.count;
            if (c->d.thick_polyline.closed) {
                sb_append(&body, "<polygon points=\"", -1);
            } else {
                sb_append(&body, "<polyline points=\"", -1);
            }
            for (int j = 0; j < n; j++) {
                if (j > 0) sb_append(&body, " ", 1);
                sb_printf(&body, "%.2f,%.2f", pts[j].x, pts[j].y);
            }
            sb_printf(&body, "\" fill=\"none\" ");
            svg_stroke(&body, c->color, (int)(c->d.thick_polyline.width + 0.5f));
            sb_append(&body, " stroke-linejoin=\"miter\" stroke-linecap=\"butt\"", -1);
            sb_append(&body, "/>\n", -1);
        } break;

        case LUI_CMD_DRAW_ARROW: {
            /* Line body */
            sb_printf(&body, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" ",
                      c->d.arrow.x0, c->d.arrow.y0,
                      c->d.arrow.x1, c->d.arrow.y1);
            svg_stroke(&body, c->color, c->stroke_width);
            sb_append(&body, "/>\n", -1);

            /* Arrowheads as simple filled triangles */
            float dx = (float)(c->d.arrow.x1 - c->d.arrow.x0);
            float dy = (float)(c->d.arrow.y1 - c->d.arrow.y0);
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.001f) {
                float ux = dx / len, uy = dy / len;
                float px = -uy, py = ux;
                int hs = c->d.arrow.head_size;

                if (c->d.arrow.head != LVG_ARROW_NONE) {
                    float tx = (float)c->d.arrow.x1;
                    float ty = (float)c->d.arrow.y1;
                    float bx = tx - ux * hs;
                    float by = ty - uy * hs;
                    float hw = hs * 0.4f;
                    sb_printf(&body, "<polygon points=\"%.1f,%.1f %.1f,%.1f %.1f,%.1f\" ",
                              tx, ty,
                              bx + px * hw, by + py * hw,
                              bx - px * hw, by - py * hw);
                    svg_fill(&body, c->color);
                    sb_append(&body, "/>\n", -1);
                }
                if (c->d.arrow.tail != LVG_ARROW_NONE) {
                    float tx = (float)c->d.arrow.x0;
                    float ty = (float)c->d.arrow.y0;
                    float bx = tx + ux * hs;
                    float by = ty + uy * hs;
                    float hw = hs * 0.4f;
                    sb_printf(&body, "<polygon points=\"%.1f,%.1f %.1f,%.1f %.1f,%.1f\" ",
                              tx, ty,
                              bx + px * hw, by + py * hw,
                              bx - px * hw, by - py * hw);
                    svg_fill(&body, c->color);
                    sb_append(&body, "/>\n", -1);
                }
            }
        } break;

        case LUI_CMD_FILL_RECT_HATCHED: {
            /* Background */
            if (c->color != LVG_COLOR_TRANSPARENT) {
                sb_printf(&body, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" ",
                          c->d.hatch_rect.x, c->d.hatch_rect.y,
                          c->d.hatch_rect.w, c->d.hatch_rect.h);
                svg_fill(&body, c->color);
                sb_append(&body, "/>\n", -1);
            }
            /* Hatch pattern */
            int hid = emit_hatch_def(&defs, c->d.hatch_rect.style,
                                     c->color2,
                                     c->d.hatch_rect.spacing,
                                     c->d.hatch_rect.line_width);
            sb_printf(&body, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
                      "fill=\"url(#hatch%d)\"/>\n",
                      c->d.hatch_rect.x, c->d.hatch_rect.y,
                      c->d.hatch_rect.w, c->d.hatch_rect.h, hid);
        } break;

        case LUI_CMD_FILL_POLYGON_HATCHED: {
            const lvg_point_t *pts =
                (const lvg_point_t *)(rec->data + c->d.hatch_polygon.data_offset);
            int n = c->d.hatch_polygon.count;

            /* Build points string */
            strbuf_t pts_str;
            sb_init(&pts_str);
            for (int j = 0; j < n; j++) {
                if (j > 0) sb_append(&pts_str, " ", 1);
                sb_printf(&pts_str, "%d,%d", pts[j].x, pts[j].y);
            }

            /* Background */
            if (c->color != LVG_COLOR_TRANSPARENT) {
                sb_printf(&body, "<polygon points=\"%.*s\" ",
                          pts_str.len, pts_str.data);
                svg_fill(&body, c->color);
                sb_append(&body, "/>\n", -1);
            }
            /* Hatch */
            int hid = emit_hatch_def(&defs, c->d.hatch_polygon.style,
                                     c->color2,
                                     c->d.hatch_polygon.spacing,
                                     c->d.hatch_polygon.line_width);
            sb_printf(&body, "<polygon points=\"%.*s\" "
                      "fill=\"url(#hatch%d)\"/>\n",
                      pts_str.len, pts_str.data, hid);
            free(pts_str.data);
        } break;

        case LUI_CMD_DRAW_IMAGE: {
            const uint32_t *pixels =
                (const uint32_t *)(rec->data + c->d.image.data_offset);
            sb_printf(&body, "<image x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" href=\"",
                      c->d.image.dst_x, c->d.image.dst_y,
                      c->d.image.dst_w, c->d.image.dst_h);
            emit_image_data_uri(&body, pixels,
                                c->d.image.img_w, c->d.image.img_h,
                                c->d.image.img_stride);
            sb_append(&body, "\"/>\n", -1);
        } break;

        case LUI_CMD_DRAW_TEXT: {
            const char *text = (const char *)(rec->data + c->d.text.text_offset);
            const char *font = (const char *)(rec->data + c->d.text.font_offset);
            int r = LVG_COLOR_R(c->color);
            int g = LVG_COLOR_G(c->color);
            int b = LVG_COLOR_B(c->color);
            int a = LVG_COLOR_A(c->color);
            sb_printf(&body, "<text x=\"%d\" y=\"%d\" "
                      "font-family=\"%s\" font-size=\"%.1f\" "
                      "fill=\"rgb(%d,%d,%d)\"",
                      c->d.text.x, c->d.text.y,
                      font, c->d.text.font_size,
                      r, g, b);
            if (a < 255)
                sb_printf(&body, " fill-opacity=\"%.3f\"", a / 255.0);
            sb_append(&body, ">", 1);
            /* Escape XML special characters */
            for (int j = 0; j < c->d.text.text_len; j++) {
                char ch = text[j];
                if (ch == '<') sb_append(&body, "&lt;", -1);
                else if (ch == '>') sb_append(&body, "&gt;", -1);
                else if (ch == '&') sb_append(&body, "&amp;", -1);
                else if (ch == '"') sb_append(&body, "&quot;", -1);
                else sb_append(&body, &ch, 1);
            }
            sb_append(&body, "</text>\n", -1);
        } break;
        }
    }

    /* Close any open clip group */
    if (clip_open) sb_append(&body, "</g>\n", -1);

    /* Assemble: defs + body */
    if (defs.len > 0) {
        sb_append(sb, "<defs>\n", -1);
        sb_append(sb, defs.data, defs.len);
        sb_append(sb, "</defs>\n", -1);
    }
    sb_append(sb, body.data, body.len);
    sb_append(sb, "</svg>\n", -1);

    free(defs.data);
    free(body.data);
}

/* ---- Public API --------------------------------------------------------- */

char *lui_export_svg_string(const lui_recorder_t *rec, int *out_len)
{
    if (!rec) return NULL;
    strbuf_t sb;
    sb_init(&sb);
    emit_svg(&sb, rec);
    /* Null-terminate. If any allocation along the way failed, bail cleanly
     * instead of dereferencing a NULL sb.data. */
    sb_ensure(&sb, 1);
    if (sb.failed || !sb.data) {
        free(sb.data);
        if (out_len) *out_len = 0;
        return NULL;
    }
    sb.data[sb.len] = '\0';
    if (out_len) *out_len = sb.len;
    return sb.data;
}

int lui_export_svg(const lui_recorder_t *rec, const char *path)
{
    int len = 0;
    char *svg = lui_export_svg_string(rec, &len);
    if (!svg) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) { free(svg); return -1; }
    size_t written = fwrite(svg, 1, (size_t)len, f);
    fclose(f);
    free(svg);
    return (int)written == len ? 0 : -1;
}
