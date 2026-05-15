/*
 * src/vg/canvas_ops.h — internal pluggable-backend vtable for lui_canvas_t.
 *
 * This header is NOT part of the public lightui API. Backend implementations
 * (blend2d, thorvg, …) fill in a static const lui_canvas_ops_t and attach it
 * to a canvas via lui_canvas_t::ops.
 *
 * Design:
 *   - Every function pointer slot is OPTIONAL. A NULL slot means "fall back
 *     to the built-in software body", which lets backends ship partial
 *     coverage without blocking the demo.
 *   - The public lui_canvas_* dispatchers in src/vg/canvas.c check
 *     cv->ops-><slot>; if non-NULL they delegate, else they run the
 *     software body inline.
 *   - The software backend sets cv->ops = NULL entirely (zero-cost fast
 *     path — a single NULL-check branch).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIGHTUI_SRC_VG_CANVAS_OPS_H
#define LIGHTUI_SRC_VG_CANVAS_OPS_H

#include <lightui/vg/canvas.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lui_canvas_ops {
    /* ---- Lifecycle ------------------------------------------------------ */
    /* destroy: free backend_state. Called from lui_canvas_destroy. */
    void (*destroy)(lui_canvas_t *canvas);

    /* flush: finalise any deferred drawing into cv->surface->pixels.
     * Called by the user via lui_canvas_flush(). Called automatically at
     * the start of lui_canvas_destroy() for retained-mode backends. */
    void (*flush)(lui_canvas_t *canvas);

    /* set_clip: notify backend the clip rect changed. NULL = full surface. */
    void (*set_clip)(lui_canvas_t *canvas, const lui_rect_t *clip);

    /* ---- Primitives (NULL => software fallback) ------------------------- */
    void (*clear)(lui_canvas_t *, lui_color_t);

    void (*fill_rect)(lui_canvas_t *, int x, int y, int w, int h,
                      lui_color_t);
    void (*stroke_rect)(lui_canvas_t *, int x, int y, int w, int h,
                        lui_color_t, int stroke_width);

    void (*fill_circle)(lui_canvas_t *, int cx, int cy, int r, lui_color_t);
    void (*stroke_circle)(lui_canvas_t *, int cx, int cy, int r,
                          lui_color_t, int stroke_width);

    void (*fill_ellipse)(lui_canvas_t *, int cx, int cy, int rx, int ry,
                         lui_color_t);
    void (*stroke_ellipse)(lui_canvas_t *, int cx, int cy, int rx, int ry,
                           lui_color_t, int stroke_width);

    void (*fill_rounded_rect)(lui_canvas_t *, int x, int y, int w, int h,
                              int radius, lui_color_t);
    void (*stroke_rounded_rect)(lui_canvas_t *, int x, int y, int w, int h,
                                int radius, lui_color_t, int stroke_width);

    void (*fill_triangle)(lui_canvas_t *, int x0, int y0, int x1, int y1,
                          int x2, int y2, lui_color_t);
    void (*fill_polygon)(lui_canvas_t *, const lui_point_t *, int count,
                         lui_color_t);
    void (*stroke_polygon)(lui_canvas_t *, const lui_point_t *, int count,
                           lui_color_t, int stroke_width);
    void (*fill_polygon_ex)(lui_canvas_t *, const lui_point_t *, int count,
                            lui_color_t, lui_fill_rule_t);

    void (*draw_line)(lui_canvas_t *, int x0, int y0, int x1, int y1,
                      lui_color_t, int stroke_width);
    void (*draw_polyline)(lui_canvas_t *, const lui_point_t *, int count,
                          lui_color_t, int stroke_width);

    void (*draw_line_dashed)(lui_canvas_t *, int x0, int y0, int x1, int y1,
                             lui_color_t, int stroke_width,
                             const lui_dash_t *);
    void (*draw_polyline_dashed)(lui_canvas_t *, const lui_point_t *,
                                 int count, lui_color_t, int stroke_width,
                                 const lui_dash_t *);

    void (*draw_line_aa)(lui_canvas_t *, float x0, float y0,
                         float x1, float y1, lui_color_t);

    void (*draw_thick_polyline)(lui_canvas_t *, const lui_pointf_t *,
                                int count, lui_color_t, float width,
                                bool closed);
    void (*draw_styled_polyline)(lui_canvas_t *, const lui_pointf_t *,
                                 int count, lui_color_t, float width,
                                 bool closed,
                                 lui_line_cap_t, lui_line_join_t);

    void (*draw_arrow)(lui_canvas_t *, int x0, int y0, int x1, int y1,
                       lui_color_t, int stroke_width,
                       lui_arrow_style_t head, lui_arrow_style_t tail,
                       int head_size);

    void (*fill_rect_hatched)(lui_canvas_t *, int x, int y, int w, int h,
                              lui_color_t bg, lui_color_t fg,
                              lui_hatch_style_t, int spacing, int line_width);

    void (*draw_bezier_cubic)(lui_canvas_t *,
                              float x0, float y0, float x1, float y1,
                              float x2, float y2, float x3, float y3,
                              lui_color_t, int stroke_width);
    void (*draw_bezier_quad)(lui_canvas_t *,
                             float x0, float y0, float x1, float y1,
                             float x2, float y2,
                             lui_color_t, int stroke_width);

    void (*draw_arc)(lui_canvas_t *, float cx, float cy, float radius,
                     float start_angle, float sweep_angle,
                     lui_color_t, int stroke_width, lui_line_cap_t cap);
    void (*fill_pie)(lui_canvas_t *, float cx, float cy, float radius,
                     float start_angle, float sweep_angle, lui_color_t);

    void (*fill_rect_gradient)(lui_canvas_t *, int x, int y, int w, int h,
                               const lui_canvas_gradient_t *);
    void (*fill_circle_gradient)(lui_canvas_t *, int cx, int cy, int r,
                                 const lui_canvas_gradient_t *);

    void (*fill_rect_blended)(lui_canvas_t *, int x, int y, int w, int h,
                              lui_color_t, lui_canvas_blend_mode_t);
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIGHTUI_SRC_VG_CANVAS_OPS_H */
