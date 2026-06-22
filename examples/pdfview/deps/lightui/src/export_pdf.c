/*
 * export_pdf.c — Minimal PDF serialization from recorded canvas commands
 *
 * Generates a valid PDF 1.4 file with no external dependencies.
 * Content is uncompressed for simplicity.
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

/* ---- Growing byte buffer ------------------------------------------------ */

typedef struct {
    uint8_t *data;
    int      len;
    int      cap;
    int      failed;  /* sticky: once realloc fails, all further writes no-op */
} bytebuf_t;

static void bb_init(bytebuf_t *bb)
{
    bb->data = NULL;
    bb->len = 0;
    bb->cap = 0;
    bb->failed = 0;
}

static void bb_ensure(bytebuf_t *bb, int need)
{
    if (bb->failed) return;
    if (need < 0 || bb->len > INT_MAX - need) { bb->failed = 1; return; }
    if (bb->len + need <= bb->cap) return;
    int new_cap = bb->cap ? bb->cap * 2 : 4096;
    while (new_cap < bb->len + need) {
        if (new_cap > INT_MAX / 2) { bb->failed = 1; return; }
        new_cap *= 2;
    }
    uint8_t *p = (uint8_t *)realloc(bb->data, (size_t)new_cap);
    if (!p) { bb->failed = 1; return; }
    bb->data = p;
    bb->cap = new_cap;
}

static void bb_append(bytebuf_t *bb, const void *data, int len)
{
    bb_ensure(bb, len);
    if (bb->failed) return;
    memcpy(bb->data + bb->len, data, (size_t)len);
    bb->len += len;
}

static void bb_printf(bytebuf_t *bb, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) bb_append(bb, buf, n);
}

/* ---- PDF object tracking ------------------------------------------------ */

#define MAX_OBJECTS 256

typedef struct {
    bytebuf_t buf;         /* final output */
    int       offsets[MAX_OBJECTS]; /* byte offset of each object */
    int       obj_count;
} pdf_writer_t;

static void pw_init(pdf_writer_t *pw)
{
    bb_init(&pw->buf);
    pw->obj_count = 0;
}

static int pw_new_obj(pdf_writer_t *pw)
{
    int id = pw->obj_count + 1; /* 1-based */
    if (pw->obj_count < MAX_OBJECTS)
        pw->offsets[pw->obj_count] = pw->buf.len;
    pw->obj_count++;
    bb_printf(&pw->buf, "%d 0 obj\n", id);
    return id;
}

static void pw_end_obj(pdf_writer_t *pw)
{
    bb_printf(&pw->buf, "endobj\n\n");
}

/* ---- PDF colour helpers ------------------------------------------------- */

static void pdf_set_fill(bytebuf_t *cs, lvg_color_t color)
{
    float r = LVG_COLOR_R(color) / 255.0f;
    float g = LVG_COLOR_G(color) / 255.0f;
    float b = LVG_COLOR_B(color) / 255.0f;
    bb_printf(cs, "%.3f %.3f %.3f rg\n", r, g, b);
}

static void pdf_set_stroke(bytebuf_t *cs, lvg_color_t color)
{
    float r = LVG_COLOR_R(color) / 255.0f;
    float g = LVG_COLOR_G(color) / 255.0f;
    float b = LVG_COLOR_B(color) / 255.0f;
    bb_printf(cs, "%.3f %.3f %.3f RG\n", r, g, b);
}

/* ---- Circle/ellipse as bezier curves ------------------------------------ */

static const float KAPPA = 0.5522847498f;

/* Emit a full ellipse path (4 cubic Bezier segments) centered at (cx,cy). */
static void pdf_ellipse_path(bytebuf_t *cs, float cx, float cy,
                             float rx, float ry)
{
    float kx = rx * KAPPA;
    float ky = ry * KAPPA;

    bb_printf(cs, "%.2f %.2f m\n", cx + rx, cy);
    bb_printf(cs, "%.2f %.2f %.2f %.2f %.2f %.2f c\n",
              cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    bb_printf(cs, "%.2f %.2f %.2f %.2f %.2f %.2f c\n",
              cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    bb_printf(cs, "%.2f %.2f %.2f %.2f %.2f %.2f c\n",
              cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    bb_printf(cs, "%.2f %.2f %.2f %.2f %.2f %.2f c\n",
              cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
}

/* ---- Rounded rect path -------------------------------------------------- */

static void pdf_rounded_rect_path(bytebuf_t *cs, float x, float y,
                                  float w, float h, float r)
{
    float k = r * KAPPA;

    bb_printf(cs, "%.2f %.2f m\n", x + r, y);
    bb_printf(cs, "%.2f %.2f l\n", x + w - r, y);
    bb_printf(cs, "%.2f %.2f %.2f %.2f %.2f %.2f c\n",
              x + w - r + k, y, x + w, y + r - k, x + w, y + r);
    bb_printf(cs, "%.2f %.2f l\n", x + w, y + h - r);
    bb_printf(cs, "%.2f %.2f %.2f %.2f %.2f %.2f c\n",
              x + w, y + h - r + k, x + w - r + k, y + h, x + w - r, y + h);
    bb_printf(cs, "%.2f %.2f l\n", x + r, y + h);
    bb_printf(cs, "%.2f %.2f %.2f %.2f %.2f %.2f c\n",
              x + r - k, y + h, x, y + h - r + k, x, y + h - r);
    bb_printf(cs, "%.2f %.2f l\n", x, y + r);
    bb_printf(cs, "%.2f %.2f %.2f %.2f %.2f %.2f c\n",
              x, y + r - k, x + r - k, y, x + r, y);
    bb_printf(cs, "h\n");
}

/* ---- PDF text encoding -------------------------------------------------- */

/* Escape a string for PDF string literal (parentheses). */
static void pdf_escape_string(bytebuf_t *cs, const char *utf8, int len)
{
    bb_printf(cs, "(");
    for (int i = 0; i < len; i++) {
        char ch = utf8[i];
        if (ch == '(' || ch == ')' || ch == '\\')
            bb_printf(cs, "\\%c", ch);
        else if ((unsigned char)ch >= 0x20 && (unsigned char)ch < 0x7F)
            bb_append(cs, &ch, 1);
        else
            bb_printf(cs, "\\%03o", (unsigned char)ch);
    }
    bb_printf(cs, ")");
}

/* Map common font family names to PDF standard fonts. */
static const char *pdf_font_name(const char *family)
{
    if (!family) return "Helvetica";
    if (strstr(family, "mono") || strstr(family, "Mono") ||
        strstr(family, "code") || strstr(family, "Code") ||
        strstr(family, "Courier"))
        return "Courier";
    if (strstr(family, "serif") && !strstr(family, "sans"))
        return "Times-Roman";
    return "Helvetica";
}

/* ---- Image XObject ------------------------------------------------------ */

static int emit_image_xobject(pdf_writer_t *pw, const uint32_t *pixels,
                              int w, int h, int stride)
{
    /* Build RGB hex stream */
    bytebuf_t rgb;
    bb_init(&rgb);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = pixels[y * stride + x];
            uint8_t r = (uint8_t)((px >> 16) & 0xFF);
            uint8_t g = (uint8_t)((px >> 8) & 0xFF);
            uint8_t b = (uint8_t)(px & 0xFF);
            bb_printf(&rgb, "%02X%02X%02X", r, g, b);
        }
        bb_printf(&rgb, "\n");
    }

    int id = pw_new_obj(pw);
    bb_printf(&pw->buf, "<< /Type /XObject /Subtype /Image\n"
              "   /Width %d /Height %d\n"
              "   /ColorSpace /DeviceRGB\n"
              "   /BitsPerComponent 8\n"
              "   /Filter /ASCIIHexDecode\n"
              "   /Length %d\n>>\n"
              "stream\n", w, h, rgb.len);
    bb_append(&pw->buf, rgb.data, rgb.len);
    bb_printf(&pw->buf, "\nendstream\n");
    pw_end_obj(pw);

    free(rgb.data);
    return id;
}

/* ---- Generate content stream -------------------------------------------- */

static void generate_content(bytebuf_t *cs, const lui_recorder_t *rec,
                             pdf_writer_t *pw, int *image_ids, int *image_count)
{
    int page_h = rec->height;

    /* Flip coordinate system: origin top-left (canvas) -> bottom-left (PDF) */
    bb_printf(cs, "1 0 0 -1 0 %d cm\n", page_h);

    for (int i = 0; i < rec->cmd_count; i++) {
        const lui_cmd_t *c = &rec->cmds[i];

        switch (c->type) {
        case LUI_CMD_CLEAR:
            pdf_set_fill(cs, c->color);
            bb_printf(cs, "0 0 %d %d re f\n", rec->width, rec->height);
            break;

        case LUI_CMD_SET_CLIP:
            bb_printf(cs, "q\n");
            bb_printf(cs, "%d %d %d %d re W n\n",
                      c->d.clip.rect.x, c->d.clip.rect.y,
                      c->d.clip.rect.width, c->d.clip.rect.height);
            break;

        case LUI_CMD_RESET_CLIP:
            bb_printf(cs, "Q\n");
            break;

        case LUI_CMD_FILL_RECT:
            pdf_set_fill(cs, c->color);
            bb_printf(cs, "%d %d %d %d re f\n",
                      c->d.rect.x, c->d.rect.y,
                      c->d.rect.w, c->d.rect.h);
            break;

        case LUI_CMD_STROKE_RECT:
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "%d w\n", c->stroke_width);
            bb_printf(cs, "%d %d %d %d re S\n",
                      c->d.rect.x, c->d.rect.y,
                      c->d.rect.w, c->d.rect.h);
            break;

        case LUI_CMD_FILL_CIRCLE:
            pdf_set_fill(cs, c->color);
            pdf_ellipse_path(cs, (float)c->d.circle.cx, (float)c->d.circle.cy,
                             (float)c->d.circle.r, (float)c->d.circle.r);
            bb_printf(cs, "f\n");
            break;

        case LUI_CMD_STROKE_CIRCLE:
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "%d w\n", c->stroke_width);
            pdf_ellipse_path(cs, (float)c->d.circle.cx, (float)c->d.circle.cy,
                             (float)c->d.circle.r, (float)c->d.circle.r);
            bb_printf(cs, "S\n");
            break;

        case LUI_CMD_FILL_ELLIPSE:
            pdf_set_fill(cs, c->color);
            pdf_ellipse_path(cs, (float)c->d.ellipse.cx, (float)c->d.ellipse.cy,
                             (float)c->d.ellipse.rx, (float)c->d.ellipse.ry);
            bb_printf(cs, "f\n");
            break;

        case LUI_CMD_STROKE_ELLIPSE:
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "%d w\n", c->stroke_width);
            pdf_ellipse_path(cs, (float)c->d.ellipse.cx, (float)c->d.ellipse.cy,
                             (float)c->d.ellipse.rx, (float)c->d.ellipse.ry);
            bb_printf(cs, "S\n");
            break;

        case LUI_CMD_FILL_ROUNDED_RECT:
            pdf_set_fill(cs, c->color);
            pdf_rounded_rect_path(cs,
                                  (float)c->d.rounded_rect.x,
                                  (float)c->d.rounded_rect.y,
                                  (float)c->d.rounded_rect.w,
                                  (float)c->d.rounded_rect.h,
                                  (float)c->d.rounded_rect.radius);
            bb_printf(cs, "f\n");
            break;

        case LUI_CMD_STROKE_ROUNDED_RECT:
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "%d w\n", c->stroke_width);
            pdf_rounded_rect_path(cs,
                                  (float)c->d.rounded_rect.x,
                                  (float)c->d.rounded_rect.y,
                                  (float)c->d.rounded_rect.w,
                                  (float)c->d.rounded_rect.h,
                                  (float)c->d.rounded_rect.radius);
            bb_printf(cs, "S\n");
            break;

        case LUI_CMD_FILL_TRIANGLE:
            pdf_set_fill(cs, c->color);
            bb_printf(cs, "%d %d m %d %d l %d %d l h f\n",
                      c->d.triangle.x0, c->d.triangle.y0,
                      c->d.triangle.x1, c->d.triangle.y1,
                      c->d.triangle.x2, c->d.triangle.y2);
            break;

        case LUI_CMD_FILL_POLYGON: {
            const lvg_point_t *pts =
                (const lvg_point_t *)(rec->data + c->d.polygon.data_offset);
            int n = c->d.polygon.count;
            pdf_set_fill(cs, c->color);
            bb_printf(cs, "%d %d m\n", pts[0].x, pts[0].y);
            for (int j = 1; j < n; j++)
                bb_printf(cs, "%d %d l\n", pts[j].x, pts[j].y);
            bb_printf(cs, "h f\n");
        } break;

        case LUI_CMD_STROKE_POLYGON: {
            const lvg_point_t *pts =
                (const lvg_point_t *)(rec->data + c->d.polygon.data_offset);
            int n = c->d.polygon.count;
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "%d w\n", c->stroke_width);
            bb_printf(cs, "%d %d m\n", pts[0].x, pts[0].y);
            for (int j = 1; j < n; j++)
                bb_printf(cs, "%d %d l\n", pts[j].x, pts[j].y);
            bb_printf(cs, "h S\n");
        } break;

        case LUI_CMD_DRAW_LINE:
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "%d w\n", c->stroke_width);
            bb_printf(cs, "%d %d m %d %d l S\n",
                      c->d.line.x0, c->d.line.y0,
                      c->d.line.x1, c->d.line.y1);
            break;

        case LUI_CMD_DRAW_POLYLINE: {
            const lvg_point_t *pts =
                (const lvg_point_t *)(rec->data + c->d.polyline.data_offset);
            int n = c->d.polyline.count;
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "%d w\n", c->stroke_width);
            bb_printf(cs, "%d %d m\n", pts[0].x, pts[0].y);
            for (int j = 1; j < n; j++)
                bb_printf(cs, "%d %d l\n", pts[j].x, pts[j].y);
            bb_printf(cs, "S\n");
        } break;

        case LUI_CMD_DRAW_LINE_AA:
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "1 w\n");
            bb_printf(cs, "%.2f %.2f m %.2f %.2f l S\n",
                      c->d.line_aa.x0, c->d.line_aa.y0,
                      c->d.line_aa.x1, c->d.line_aa.y1);
            break;

        case LUI_CMD_DRAW_POLYLINE_AA: {
            const float *xy = (const float *)(rec->data + c->d.polyline_aa.data_offset);
            int n = c->d.polyline_aa.count;
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "1 w\n");
            bb_printf(cs, "%.2f %.2f m\n", xy[0], xy[1]);
            for (int j = 1; j < n; j++)
                bb_printf(cs, "%.2f %.2f l\n", xy[j * 2], xy[j * 2 + 1]);
            bb_printf(cs, "S\n");
        } break;

        case LUI_CMD_DRAW_LINE_DASHED: {
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "%d w\n", c->stroke_width);
            if (c->d.line_dashed.dash_count > 0) {
                const int *pat = (const int *)(rec->data + c->d.line_dashed.dash_offset);
                bb_printf(cs, "[");
                for (int j = 0; j < c->d.line_dashed.dash_count; j++)
                    bb_printf(cs, "%d ", pat[j]);
                bb_printf(cs, "] %d d\n", c->d.line_dashed.dash_phase);
            }
            bb_printf(cs, "%d %d m %d %d l S\n",
                      c->d.line_dashed.x0, c->d.line_dashed.y0,
                      c->d.line_dashed.x1, c->d.line_dashed.y1);
            /* Reset dash */
            bb_printf(cs, "[] 0 d\n");
        } break;

        case LUI_CMD_DRAW_POLYLINE_DASHED: {
            const lvg_point_t *pts =
                (const lvg_point_t *)(rec->data + c->d.polyline_dashed.pts_offset);
            int n = c->d.polyline_dashed.pts_count;
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "%d w\n", c->stroke_width);
            if (c->d.polyline_dashed.dash_count > 0) {
                const int *pat = (const int *)(rec->data + c->d.polyline_dashed.dash_offset);
                bb_printf(cs, "[");
                for (int j = 0; j < c->d.polyline_dashed.dash_count; j++)
                    bb_printf(cs, "%d ", pat[j]);
                bb_printf(cs, "] %d d\n", c->d.polyline_dashed.dash_phase);
            }
            bb_printf(cs, "%d %d m\n", pts[0].x, pts[0].y);
            for (int j = 1; j < n; j++)
                bb_printf(cs, "%d %d l\n", pts[j].x, pts[j].y);
            bb_printf(cs, "S\n");
            bb_printf(cs, "[] 0 d\n");
        } break;

        case LUI_CMD_DRAW_THICK_POLYLINE: {
            const lvg_pointf_t *pts =
                (const lvg_pointf_t *)(rec->data + c->d.thick_polyline.data_offset);
            int n = c->d.thick_polyline.count;
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "%.2f w\n", c->d.thick_polyline.width);
            bb_printf(cs, "1 j\n"); /* miter join */
            bb_printf(cs, "%.2f %.2f m\n", pts[0].x, pts[0].y);
            for (int j = 1; j < n; j++)
                bb_printf(cs, "%.2f %.2f l\n", pts[j].x, pts[j].y);
            if (c->d.thick_polyline.closed)
                bb_printf(cs, "h ");
            bb_printf(cs, "S\n");
        } break;

        case LUI_CMD_DRAW_ARROW: {
            /* Line body */
            pdf_set_stroke(cs, c->color);
            bb_printf(cs, "%d w\n", c->stroke_width);
            bb_printf(cs, "%d %d m %d %d l S\n",
                      c->d.arrow.x0, c->d.arrow.y0,
                      c->d.arrow.x1, c->d.arrow.y1);

            /* Arrowheads as filled triangles */
            float dx = (float)(c->d.arrow.x1 - c->d.arrow.x0);
            float dy = (float)(c->d.arrow.y1 - c->d.arrow.y0);
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.001f) {
                float ux = dx / len, uy = dy / len;
                float px = -uy, py = ux;
                int hs = c->d.arrow.head_size;
                float hw = hs * 0.4f;

                pdf_set_fill(cs, c->color);

                if (c->d.arrow.head != LVG_ARROW_NONE) {
                    float tx = (float)c->d.arrow.x1;
                    float ty = (float)c->d.arrow.y1;
                    float bx = tx - ux * hs;
                    float by = ty - uy * hs;
                    bb_printf(cs, "%.2f %.2f m %.2f %.2f l %.2f %.2f l h f\n",
                              tx, ty,
                              bx + px * hw, by + py * hw,
                              bx - px * hw, by - py * hw);
                }
                if (c->d.arrow.tail != LVG_ARROW_NONE) {
                    float tx = (float)c->d.arrow.x0;
                    float ty = (float)c->d.arrow.y0;
                    float bx = tx + ux * hs;
                    float by = ty + uy * hs;
                    bb_printf(cs, "%.2f %.2f m %.2f %.2f l %.2f %.2f l h f\n",
                              tx, ty,
                              bx + px * hw, by + py * hw,
                              bx - px * hw, by - py * hw);
                }
            }
        } break;

        case LUI_CMD_FILL_RECT_HATCHED: {
            /* Background fill */
            if (c->color != LVG_COLOR_TRANSPARENT) {
                pdf_set_fill(cs, c->color);
                bb_printf(cs, "%d %d %d %d re f\n",
                          c->d.hatch_rect.x, c->d.hatch_rect.y,
                          c->d.hatch_rect.w, c->d.hatch_rect.h);
            }
            /* Clip to rect and draw hatch lines */
            bb_printf(cs, "q\n");
            bb_printf(cs, "%d %d %d %d re W n\n",
                      c->d.hatch_rect.x, c->d.hatch_rect.y,
                      c->d.hatch_rect.w, c->d.hatch_rect.h);
            pdf_set_stroke(cs, c->color2);
            bb_printf(cs, "%d w\n", c->d.hatch_rect.line_width);

            int x = c->d.hatch_rect.x, y = c->d.hatch_rect.y;
            int w = c->d.hatch_rect.w, h = c->d.hatch_rect.h;
            int sp = c->d.hatch_rect.spacing;
            if (sp < 1) sp = 8;

            switch (c->d.hatch_rect.style) {
            case LVG_HATCH_HORIZONTAL:
                for (int yy = y; yy < y + h; yy += sp)
                    bb_printf(cs, "%d %d m %d %d l S\n", x, yy, x + w, yy);
                break;
            case LVG_HATCH_VERTICAL:
                for (int xx = x; xx < x + w; xx += sp)
                    bb_printf(cs, "%d %d m %d %d l S\n", xx, y, xx, y + h);
                break;
            case LVG_HATCH_FORWARD_DIAG:
                for (int d = -(h); d < w + h; d += sp)
                    bb_printf(cs, "%d %d m %d %d l S\n",
                              x + d, y + h, x + d + h, y);
                break;
            case LVG_HATCH_BACK_DIAG:
                for (int d = -(h); d < w + h; d += sp)
                    bb_printf(cs, "%d %d m %d %d l S\n",
                              x + d, y, x + d + h, y + h);
                break;
            case LVG_HATCH_CROSS:
                for (int yy = y; yy < y + h; yy += sp)
                    bb_printf(cs, "%d %d m %d %d l S\n", x, yy, x + w, yy);
                for (int xx = x; xx < x + w; xx += sp)
                    bb_printf(cs, "%d %d m %d %d l S\n", xx, y, xx, y + h);
                break;
            case LVG_HATCH_DIAG_CROSS:
                for (int d = -(h); d < w + h; d += sp) {
                    bb_printf(cs, "%d %d m %d %d l S\n",
                              x + d, y + h, x + d + h, y);
                    bb_printf(cs, "%d %d m %d %d l S\n",
                              x + d, y, x + d + h, y + h);
                }
                break;
            }
            bb_printf(cs, "Q\n");
        } break;

        case LUI_CMD_FILL_POLYGON_HATCHED: {
            const lvg_point_t *pts =
                (const lvg_point_t *)(rec->data + c->d.hatch_polygon.data_offset);
            int n = c->d.hatch_polygon.count;

            /* Background fill */
            if (c->color != LVG_COLOR_TRANSPARENT) {
                pdf_set_fill(cs, c->color);
                bb_printf(cs, "%d %d m\n", pts[0].x, pts[0].y);
                for (int j = 1; j < n; j++)
                    bb_printf(cs, "%d %d l\n", pts[j].x, pts[j].y);
                bb_printf(cs, "h f\n");
            }

            /* Clip to polygon and draw hatch lines */
            bb_printf(cs, "q\n");
            bb_printf(cs, "%d %d m\n", pts[0].x, pts[0].y);
            for (int j = 1; j < n; j++)
                bb_printf(cs, "%d %d l\n", pts[j].x, pts[j].y);
            bb_printf(cs, "h W n\n");

            pdf_set_stroke(cs, c->color2);
            bb_printf(cs, "%d w\n", c->d.hatch_polygon.line_width);

            /* Compute bounding box */
            int bx0 = pts[0].x, by0 = pts[0].y;
            int bx1 = pts[0].x, by1 = pts[0].y;
            for (int j = 1; j < n; j++) {
                if (pts[j].x < bx0) bx0 = pts[j].x;
                if (pts[j].y < by0) by0 = pts[j].y;
                if (pts[j].x > bx1) bx1 = pts[j].x;
                if (pts[j].y > by1) by1 = pts[j].y;
            }
            int bw = bx1 - bx0, bh = by1 - by0;
            int sp = c->d.hatch_polygon.spacing;
            if (sp < 1) sp = 8;

            switch (c->d.hatch_polygon.style) {
            case LVG_HATCH_HORIZONTAL:
                for (int yy = by0; yy <= by1; yy += sp)
                    bb_printf(cs, "%d %d m %d %d l S\n", bx0, yy, bx1, yy);
                break;
            case LVG_HATCH_VERTICAL:
                for (int xx = bx0; xx <= bx1; xx += sp)
                    bb_printf(cs, "%d %d m %d %d l S\n", xx, by0, xx, by1);
                break;
            default:
                for (int d = -(bh); d < bw + bh; d += sp)
                    bb_printf(cs, "%d %d m %d %d l S\n",
                              bx0 + d, by1, bx0 + d + bh, by0);
                break;
            }
            bb_printf(cs, "Q\n");
        } break;

        case LUI_CMD_DRAW_IMAGE: {
            const uint32_t *pixels =
                (const uint32_t *)(rec->data + c->d.image.data_offset);
            int img_id = emit_image_xobject(pw, pixels,
                                            c->d.image.img_w,
                                            c->d.image.img_h,
                                            c->d.image.img_stride);
            /* Place image with transform */
            bb_printf(cs, "q\n");
            bb_printf(cs, "%d 0 0 %d %d %d cm\n",
                      c->d.image.dst_w, c->d.image.dst_h,
                      c->d.image.dst_x, c->d.image.dst_y);
            bb_printf(cs, "/Img%d Do\n", img_id);
            bb_printf(cs, "Q\n");
            if (*image_count < MAX_OBJECTS) {
                image_ids[*image_count] = img_id;
                (*image_count)++;
            }
        } break;

        case LUI_CMD_DRAW_TEXT: {
            const char *text = (const char *)(rec->data + c->d.text.text_offset);
            pdf_set_fill(cs, c->color);
            bb_printf(cs, "BT\n");
            bb_printf(cs, "/F1 %.1f Tf\n", c->d.text.font_size);
            bb_printf(cs, "%d %d Td\n", c->d.text.x, c->d.text.y);
            pdf_escape_string(cs, text, c->d.text.text_len);
            bb_printf(cs, " Tj\n");
            bb_printf(cs, "ET\n");
        } break;
        }
    }
}

/* ---- PDF assembly ------------------------------------------------------- */

static void emit_pdf(pdf_writer_t *pw, const lui_recorder_t *rec)
{
    /* Header */
    bb_printf(&pw->buf, "%%PDF-1.4\n%%\xe2\xe3\xcf\xd3\n\n");

    /* Pre-generate content stream to know its length and collect image XObjects */
    bytebuf_t cs;
    bb_init(&cs);

    int image_ids[MAX_OBJECTS];
    int image_count = 0;

    generate_content(&cs, rec, pw, image_ids, &image_count);

    /* Determine font name from first text command */
    const char *pdf_font = "Helvetica";
    for (int i = 0; i < rec->cmd_count; i++) {
        if (rec->cmds[i].type == LUI_CMD_DRAW_TEXT) {
            const char *fam = (const char *)(rec->data +
                               rec->cmds[i].d.text.font_offset);
            pdf_font = pdf_font_name(fam);
            break;
        }
    }

    /* Obj 1: Catalog */
    int cat_id = pw_new_obj(pw);
    bb_printf(&pw->buf, "<< /Type /Catalog /Pages %d 0 R >>\n", cat_id + 1);
    pw_end_obj(pw);

    /* Obj 2: Pages */
    int pages_id = pw_new_obj(pw);
    bb_printf(&pw->buf, "<< /Type /Pages /Kids [%d 0 R] /Count 1 >>\n",
              pages_id + 1);
    pw_end_obj(pw);

    /* Obj 3: Page */
    int page_id = pw_new_obj(pw);
    bb_printf(&pw->buf, "<< /Type /Page /Parent %d 0 R\n"
              "   /MediaBox [0 0 %d %d]\n"
              "   /Contents %d 0 R\n"
              "   /Resources <<\n"
              "     /Font << /F1 %d 0 R >>\n",
              pages_id, rec->width, rec->height,
              page_id + 1, page_id + 2);

    if (image_count > 0) {
        bb_printf(&pw->buf, "     /XObject <<");
        for (int j = 0; j < image_count; j++)
            bb_printf(&pw->buf, " /Img%d %d 0 R", image_ids[j], image_ids[j]);
        bb_printf(&pw->buf, " >>\n");
    }

    bb_printf(&pw->buf, "   >>\n>>\n");
    pw_end_obj(pw);

    /* Obj 4: Content stream */
    pw_new_obj(pw);
    bb_printf(&pw->buf, "<< /Length %d >>\nstream\n", cs.len);
    bb_append(&pw->buf, cs.data, cs.len);
    bb_printf(&pw->buf, "\nendstream\n");
    pw_end_obj(pw);

    /* Obj 5: Font */
    pw_new_obj(pw);
    bb_printf(&pw->buf, "<< /Type /Font /Subtype /Type1 /BaseFont /%s >>\n",
              pdf_font);
    pw_end_obj(pw);

    /* Cross-reference table */
    int xref_offset = pw->buf.len;
    bb_printf(&pw->buf, "xref\n0 %d\n", pw->obj_count + 1);
    bb_printf(&pw->buf, "0000000000 65535 f \n");
    for (int j = 0; j < pw->obj_count; j++) {
        bb_printf(&pw->buf, "%010d 00000 n \n", pw->offsets[j]);
    }

    /* Trailer */
    bb_printf(&pw->buf, "trailer\n<< /Size %d /Root %d 0 R >>\n",
              pw->obj_count + 1, cat_id);
    bb_printf(&pw->buf, "startxref\n%d\n%%%%EOF\n", xref_offset);

    free(cs.data);
}

/* ---- Public API --------------------------------------------------------- */

uint8_t *lui_export_pdf_bytes(const lui_recorder_t *rec, int *out_len)
{
    if (!rec) return NULL;

    pdf_writer_t pw;
    pw_init(&pw);
    emit_pdf(&pw, rec);

    /* If any alloc failed along the way, the output is truncated — return
     * a clean failure rather than a malformed PDF blob. */
    if (pw.buf.failed) {
        free(pw.buf.data);
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (out_len) *out_len = pw.buf.len;
    return pw.buf.data;
}

int lui_export_pdf(const lui_recorder_t *rec, const char *path)
{
    int len = 0;
    uint8_t *pdf = lui_export_pdf_bytes(rec, &len);
    if (!pdf) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) { free(pdf); return -1; }
    size_t written = fwrite(pdf, 1, (size_t)len, f);
    fclose(f);
    free(pdf);
    return (int)written == len ? 0 : -1;
}
