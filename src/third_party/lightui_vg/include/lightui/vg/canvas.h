/*
 * lightui/canvas.h — 2D software-rendering drawing API
 *
 * A canvas is a lightweight drawing context backed by a lui_surface_t.
 * All operations are clipped to the current clip rectangle and alpha-blended
 * using the Porter-Duff SRC_OVER operator.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIGHTUI_CANVAS_H
#define LIGHTUI_CANVAS_H

#include "surface.h"
#include "vg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * lui_canvas_t — drawing context.
 *
 * Initialise on the stack with lui_canvas_init().  No heap allocation is
 * needed; the canvas does not own the surface.
 */
/* Forward declaration of the pluggable-backend vtable. The full definition
 * lives in the internal header src/vg/canvas_ops.h and is not part of the
 * public ABI. Callers treat @ops as an opaque pointer; NULL means "use the
 * built-in software rasterizer". */
struct lui_canvas_ops;
typedef struct lui_canvas_ops lui_canvas_ops_t;

/*
 * Fields prefixed with an underscore are private implementation details —
 * treat them as opaque. Use lui_canvas_get_surface() / lui_canvas_get_clip()
 * for read access; the drawing API mutates them internally.
 */
/* Software-backend AA algorithm. Affects polygon fills (incl. stroke-as-fill).
 * NORMAL = sub-scanline analytic AA (8 sub-rows; fast, good quality).
 * AGG    = AGG-style cell-based exact analytic AA (slower, higher quality at
 *          extreme zoom; matches blend2d/AGG output for thin strokes).
 * Backend-driven canvases (blend2d/thorvg) ignore this setting and use their
 * own rasterisers. */
typedef enum {
    LUI_CANVAS_AA_NORMAL = 0,
    LUI_CANVAS_AA_AGG    = 1,
} lui_canvas_aa_mode_t;

typedef struct {
    lui_surface_t *_surface;          /* backing pixel buffer (not owned)    */
    lui_rect_t     _clip;             /* current clip rect in surface coords */
    const lui_canvas_ops_t *_ops;     /* backend dispatch table (NULL=software) */
    void          *_backend_state;    /* backend-owned state (BLContext, …) */
    int            _aa_mode;          /* lui_canvas_aa_mode_t (software path) */
    float         *_aa_cov;           /* reusable AA coverage scratch (owned, kept zeroed) */
    int            _aa_cov_cap;       /* capacity of _aa_cov in floats          */
} lui_canvas_t;

/**
 * Accessors for the backing surface and current clip rectangle.
 * These replace direct `canvas->surface` / `canvas->clip` reads so the
 * struct body can evolve without breaking callers.
 */
static inline lui_surface_t *lui_canvas_get_surface(const lui_canvas_t *canvas)
{
    return canvas ? canvas->_surface : 0;
}

static inline lui_rect_t lui_canvas_get_clip(const lui_canvas_t *canvas)
{
    if (!canvas) { lui_rect_t r = {0, 0, 0, 0}; return r; }
    return canvas->_clip;
}

typedef enum {
    LUI_IMAGE_FILTER_NEAREST  = 0,
    LUI_IMAGE_FILTER_BILINEAR = 1,
    LUI_IMAGE_FILTER_LANCZOS3 = 2,
} lui_image_filter_t;

/* Selectable drawing backend for lui_canvas_init_backend().
 * BACKEND_SOFTWARE is always available. Others require the corresponding
 * library to be linked in; see lui_canvas_backend_available(). */
typedef enum {
    LUI_CANVAS_BACKEND_SOFTWARE = 0,
    LUI_CANVAS_BACKEND_BLEND2D  = 1,
    LUI_CANVAS_BACKEND_THORVG   = 2,
} lui_canvas_backend_t;

/* ---- Context ------------------------------------------------------------ */

/**
 * Initialise a canvas backed by @surface.
 * The initial clip is the full surface bounds.
 * Uses the built-in software rasterizer.
 */
void lui_canvas_init(lui_canvas_t *canvas, lui_surface_t *surface);

/**
 * Software-backend AA mode getter/setter. No effect on canvases backed by
 * a non-software backend (blend2d/thorvg own their AA). Default: NORMAL.
 */
void lui_canvas_set_aa_mode(lui_canvas_t *canvas, lui_canvas_aa_mode_t mode);
lui_canvas_aa_mode_t lui_canvas_get_aa_mode(const lui_canvas_t *canvas);

/**
 * Initialise a canvas backed by @surface using the requested drawing backend.
 *
 * Returns true if the backend was initialised successfully. Returns false if
 * the backend was not compiled in or failed to initialise; in that case the
 * canvas is left initialised with the software backend as a fallback, so the
 * caller can proceed unconditionally.
 *
 * Must be paired with lui_canvas_destroy() when done.
 */
bool lui_canvas_init_backend(lui_canvas_t *canvas, lui_surface_t *surface,
                             lui_canvas_backend_t backend);

/**
 * Returns true if @backend is compiled into this build of lightui.
 */
bool lui_canvas_backend_available(lui_canvas_backend_t backend);

/**
 * Human-readable name for @backend ("software", "blend2d", "thorvg").
 */
const char *lui_canvas_backend_name(lui_canvas_backend_t backend);

/**
 * Flush any pending draw commands to the surface. Required by retained-mode
 * backends (thorvg); a no-op for the software backend.
 */
void lui_canvas_flush(lui_canvas_t *canvas);

/**
 * Tear down any backend-owned state held by @canvas. Safe to call on a
 * software-backed canvas. After this call the canvas must not be used.
 */
void lui_canvas_destroy(lui_canvas_t *canvas);

/**
 * Set the clip rectangle.  It is intersected with the surface bounds.
 * Passing NULL resets the clip to the full surface.
 */
void lui_canvas_set_clip(lui_canvas_t *canvas, const lui_rect_t *clip);

/** Reset the clip to the full surface bounds. */
void lui_canvas_reset_clip(lui_canvas_t *canvas);

/* ---- Fill ---------------------------------------------------------------- */

/** Fill the entire surface (respecting clip) with a solid colour. */
void lui_canvas_clear(lui_canvas_t *canvas, lui_color_t color);

/**
 * Fill an axis-aligned rectangle with @color (alpha-blended over existing
 * pixels using SRC_OVER).
 */
void lui_canvas_fill_rect(lui_canvas_t *canvas,
                           int x, int y, int w, int h,
                           lui_color_t color);

/**
 * Draw the outline of a rectangle with line thickness @stroke_width pixels.
 */
void lui_canvas_stroke_rect(lui_canvas_t *canvas,
                              int x, int y, int w, int h,
                              lui_color_t color, int stroke_width);

/**
 * Fill a circle centred at (@cx, @cy) with radius @r.
 */
void lui_canvas_fill_circle(lui_canvas_t *canvas,
                              int cx, int cy, int r,
                              lui_color_t color);

/**
 * Draw a straight line from (@x0,@y0) to (@x1,@y1) with the given colour and
 * stroke width (>= 1).
 */
void lui_canvas_draw_line(lui_canvas_t *canvas,
                           int x0, int y0, int x1, int y1,
                           lui_color_t color, int stroke_width);

/**
 * Draw connected line segments through @count points.
 * Requires @count >= 2.
 */
void lui_canvas_draw_polyline(lui_canvas_t *canvas,
                               const lui_point_t *points, int count,
                               lui_color_t color, int stroke_width);

/* ---- Circles & Ellipses ------------------------------------------------- */

/**
 * Draw the outline of a circle centred at (@cx, @cy) with radius @r.
 */
void lui_canvas_stroke_circle(lui_canvas_t *canvas,
                                int cx, int cy, int r,
                                lui_color_t color, int stroke_width);

/**
 * Fill an axis-aligned ellipse centred at (@cx, @cy) with radii @rx, @ry.
 */
void lui_canvas_fill_ellipse(lui_canvas_t *canvas,
                               int cx, int cy, int rx, int ry,
                               lui_color_t color);

/**
 * Draw the outline of an axis-aligned ellipse.
 */
void lui_canvas_stroke_ellipse(lui_canvas_t *canvas,
                                 int cx, int cy, int rx, int ry,
                                 lui_color_t color, int stroke_width);

/* ---- Rounded rectangles ------------------------------------------------- */

/**
 * Fill a rectangle with rounded corners. @radius is the corner radius.
 */
void lui_canvas_fill_rounded_rect(lui_canvas_t *canvas,
                                    int x, int y, int w, int h,
                                    int radius, lui_color_t color);

/**
 * Draw the outline of a rounded rectangle.
 */
void lui_canvas_stroke_rounded_rect(lui_canvas_t *canvas,
                                      int x, int y, int w, int h,
                                      int radius, lui_color_t color,
                                      int stroke_width);

/* ---- Triangles & Polygons ----------------------------------------------- */

/**
 * Fill a triangle defined by three vertices.
 */
void lui_canvas_fill_triangle(lui_canvas_t *canvas,
                                int x0, int y0,
                                int x1, int y1,
                                int x2, int y2,
                                lui_color_t color);

/**
 * Fill a convex polygon defined by @count vertices (>= 3).
 * Results are undefined for concave polygons.
 */
void lui_canvas_fill_polygon(lui_canvas_t *canvas,
                               const lui_point_t *points, int count,
                               lui_color_t color);

/* ---- Stroke polygon ----------------------------------------------------- */

/**
 * Draw the outline of a polygon (closed path) defined by @count vertices.
 * The last vertex is automatically connected back to the first.
 */
void lui_canvas_stroke_polygon(lui_canvas_t *canvas,
                                 const lui_point_t *points, int count,
                                 lui_color_t color, int stroke_width);

/* ---- Dashed / dotted lines ---------------------------------------------- */

/**
 * Dash pattern for dashed/dotted lines.
 * Alternating on/off lengths in pixels.  E.g. {6, 3} = 6px on, 3px off.
 */
typedef struct {
    const int *pattern;   /* array of on/off lengths               */
    int        count;     /* number of entries in pattern (even)    */
    int        offset;    /* starting offset into the pattern       */
} lui_dash_t;

/** Pre-built dash patterns. */
extern const int lui_dash_solid[];       /* no dashes (empty) */
extern const int lui_dash_dashed[];      /* 6 on, 4 off      */
extern const int lui_dash_dotted[];      /* 2 on, 3 off      */
extern const int lui_dash_dashdot[];     /* 6 on, 3 off, 2 on, 3 off */

/**
 * Draw a dashed line from (x0,y0) to (x1,y1).
 * @dash  Dash pattern.  Pass NULL for a solid line.
 */
void lui_canvas_draw_line_dashed(lui_canvas_t *canvas,
                                   int x0, int y0, int x1, int y1,
                                   lui_color_t color, int stroke_width,
                                   const lui_dash_t *dash);

/**
 * Draw connected dashed line segments through @count points.
 */
void lui_canvas_draw_polyline_dashed(lui_canvas_t *canvas,
                                       const lui_point_t *points, int count,
                                       lui_color_t color, int stroke_width,
                                       const lui_dash_t *dash);

/* ---- Anti-aliased lines (Xiaolin Wu) ------------------------------------ */

/**
 * Draw an anti-aliased line using Xiaolin Wu's algorithm.
 * Produces smooth sub-pixel blended edges.  stroke_width is always 1.
 */
void lui_canvas_draw_line_aa(lui_canvas_t *canvas,
                               float x0, float y0, float x1, float y1,
                               lui_color_t color);

/**
 * Draw connected anti-aliased line segments (sub-pixel positions).
 */
void lui_canvas_draw_polyline_aa(lui_canvas_t *canvas,
                                   const float *xy_pairs, int point_count,
                                   lui_color_t color);

/* ---- Float-coordinate thick polyline ------------------------------------ */

/**
 * A 2D float point for sub-pixel coordinate drawing.
 */
typedef struct {
    float x, y;
} lui_pointf_t;

/**
 * Draw a thick polyline with constant pixel width at sub-pixel float coords.
 *
 * This is the key primitive for map/vector rendering: the line width stays
 * constant in screen pixels regardless of the world-to-screen transform,
 * so borders and wireframes don't scale with zoom.
 *
 * Joins between segments use miter joins (clamped to avoid spikes).
 * End caps are flat (butt caps).
 *
 * @points       Array of float (x,y) vertices.
 * @count        Number of vertices (>= 2).
 * @color        Line colour.
 * @width        Line width in pixels (float, >= 0.5).
 * @closed       If true, connect last vertex back to first.
 */
void lui_canvas_draw_thick_polyline(lui_canvas_t *canvas,
                                      const lui_pointf_t *points, int count,
                                      lui_color_t color, float width,
                                      bool closed);

/* ---- Arrow heads -------------------------------------------------------- */

typedef enum {
    LUI_ARROW_NONE     = 0,
    LUI_ARROW_FILLED   = 1,  /* solid filled triangle             */
    LUI_ARROW_OPEN     = 2,  /* open V-shape                      */
    LUI_ARROW_DIAMOND  = 3,  /* filled diamond                    */
} lui_arrow_style_t;

/**
 * Draw an arrow along a line from (x0,y0) to (x1,y1).
 *
 * @head_style  Arrow style at the (x1,y1) end.
 * @tail_style  Arrow style at the (x0,y0) end.
 * @head_size   Arrow head size in pixels (length from tip to base).
 */
void lui_canvas_draw_arrow(lui_canvas_t *canvas,
                             int x0, int y0, int x1, int y1,
                             lui_color_t color, int stroke_width,
                             lui_arrow_style_t head_style,
                             lui_arrow_style_t tail_style,
                             int head_size);

/* ---- Pattern / hatch fill ----------------------------------------------- */

typedef enum {
    LUI_HATCH_HORIZONTAL   = 0,  /* horizontal lines      */
    LUI_HATCH_VERTICAL     = 1,  /* vertical lines        */
    LUI_HATCH_FORWARD_DIAG = 2,  /* lines going /         */
    LUI_HATCH_BACK_DIAG    = 3,  /* lines going \         */
    LUI_HATCH_CROSS        = 4,  /* + grid                */
    LUI_HATCH_DIAG_CROSS   = 5,  /* x diagonal grid       */
} lui_hatch_style_t;

/**
 * Fill a rectangle with a hatching pattern.
 *
 * @bg_color   Background fill (use LUI_COLOR_TRANSPARENT for none).
 * @fg_color   Hatch line colour.
 * @style      Hatch style.
 * @spacing    Distance between hatch lines in pixels.
 * @line_width Width of each hatch line.
 */
void lui_canvas_fill_rect_hatched(lui_canvas_t *canvas,
                                    int x, int y, int w, int h,
                                    lui_color_t bg_color,
                                    lui_color_t fg_color,
                                    lui_hatch_style_t style,
                                    int spacing, int line_width);

/**
 * Fill a convex polygon with a hatching pattern.
 */
void lui_canvas_fill_polygon_hatched(lui_canvas_t *canvas,
                                       const lui_point_t *points, int count,
                                       lui_color_t bg_color,
                                       lui_color_t fg_color,
                                       lui_hatch_style_t style,
                                       int spacing, int line_width);

/* ---- Bezier curves ------------------------------------------------------ */

/**
 * Draw a cubic Bezier curve from (x0,y0) to (x3,y3) with control points
 * (x1,y1) and (x2,y2).  Uses De Casteljau subdivision for flattening.
 */
void lui_canvas_draw_bezier_cubic(lui_canvas_t *canvas,
                                     float x0, float y0,
                                     float x1, float y1,
                                     float x2, float y2,
                                     float x3, float y3,
                                     lui_color_t color, int stroke_width);

/**
 * Draw a quadratic Bezier curve from (x0,y0) to (x2,y2) with control
 * point (x1,y1).
 */
void lui_canvas_draw_bezier_quad(lui_canvas_t *canvas,
                                    float x0, float y0,
                                    float x1, float y1,
                                    float x2, float y2,
                                    lui_color_t color, int stroke_width);

/**
 * Fill a closed cubic Bezier shape.  Draws two cubic curves (top and bottom
 * halves) and fills between them using scanline rasterization.
 */
void lui_canvas_fill_bezier_cubic(lui_canvas_t *canvas,
                                     float x0, float y0,
                                     float x1, float y1,
                                     float x2, float y2,
                                     float x3, float y3,
                                     lui_color_t color);

/* ---- Line cap & join styles --------------------------------------------- */

typedef enum {
    LUI_LINE_CAP_BUTT   = 0,
    LUI_LINE_CAP_ROUND  = 1,
    LUI_LINE_CAP_SQUARE = 2,
} lui_line_cap_t;

typedef enum {
    LUI_LINE_JOIN_MITER = 0,
    LUI_LINE_JOIN_ROUND = 1,
    LUI_LINE_JOIN_BEVEL = 2,
} lui_line_join_t;

/* ---- Arcs --------------------------------------------------------------- */

/**
 * Draw a circular arc outline.
 *
 * @cx, @cy       Centre of the arc.
 * @radius        Arc radius.
 * @start_angle   Start angle in radians (0 = right, positive = counter-clockwise).
 * @sweep_angle   Sweep angle in radians (positive = counter-clockwise).
 * @color         Line colour.
 * @stroke_width  Line width in pixels.
 * @cap           Endpoint cap style.
 */
void lui_canvas_draw_arc(lui_canvas_t *canvas,
                            float cx, float cy, float radius,
                            float start_angle, float sweep_angle,
                            lui_color_t color, int stroke_width,
                            lui_line_cap_t cap);

/**
 * Fill a pie (closed sector).
 * The sector is formed by two radii and the arc between them.
 */
void lui_canvas_fill_pie(lui_canvas_t *canvas,
                            float cx, float cy, float radius,
                            float start_angle, float sweep_angle,
                            lui_color_t color);

/* ---- Gradient fills ----------------------------------------------------- */

/** Maximum number of colour stops in a canvas gradient. */
#define LUI_CANVAS_GRADIENT_MAX_STOPS  16

typedef enum {
    LUI_CANVAS_GRADIENT_LINEAR = 0,
    LUI_CANVAS_GRADIENT_RADIAL = 1,
} lui_canvas_gradient_type_t;

typedef struct {
    float      position;   /* 0.0 to 1.0 */
    lui_color_t color;
} lui_canvas_gradient_stop_t;

typedef struct {
    lui_canvas_gradient_type_t type;
    int                 stop_count;
    lui_canvas_gradient_stop_t stops[LUI_CANVAS_GRADIENT_MAX_STOPS];

    /* Linear: (x0,y0) → (x1,y1) */
    /* Radial: centre (cx,cy), radius r */
    float x0, y0, x1, y1;
    float cx, cy, r;
} lui_canvas_gradient_t;

/** Fill a rectangle with a gradient. */
void lui_canvas_fill_rect_gradient(lui_canvas_t *canvas,
                                      int x, int y, int w, int h,
                                      const lui_canvas_gradient_t *grad);

/** Fill a circle with a gradient. */
void lui_canvas_fill_circle_gradient(lui_canvas_t *canvas,
                                        int cx, int cy, int r,
                                        const lui_canvas_gradient_t *grad);

/* ---- Blend / composite modes -------------------------------------------- */

typedef enum {
    LUI_CANVAS_BLEND_SRC_OVER   = 0,   /* default Porter-Duff SRC_OVER         */
    LUI_CANVAS_BLEND_MULTIPLY   = 1,   /* result = src * dst                    */
    LUI_CANVAS_BLEND_SCREEN     = 2,   /* result = src + dst - src*dst          */
    LUI_CANVAS_BLEND_OVERLAY    = 3,   /* multiply or screen depending on dst   */
    LUI_CANVAS_BLEND_DARKEN     = 4,   /* min(src, dst)                         */
    LUI_CANVAS_BLEND_LIGHTEN    = 5,   /* max(src, dst)                         */
    LUI_CANVAS_BLEND_DIFFERENCE = 6,   /* |src - dst|                           */
    LUI_CANVAS_BLEND_EXCLUSION  = 7,   /* src + dst - 2*src*dst                 */
    LUI_CANVAS_BLEND_PLUS       = 8,   /* saturating add                        */
} lui_canvas_blend_mode_t;

/**
 * Fill a rectangle using a specified blend mode instead of default SRC_OVER.
 */
void lui_canvas_fill_rect_blended(lui_canvas_t *canvas,
                                     int x, int y, int w, int h,
                                     lui_color_t color,
                                     lui_canvas_blend_mode_t mode);

/**
 * Draw a thick polyline with configurable cap and join styles.
 */
void lui_canvas_draw_styled_polyline(lui_canvas_t *canvas,
                                        const lui_pointf_t *points, int count,
                                        lui_color_t color, float width,
                                        bool closed,
                                        lui_line_cap_t cap,
                                        lui_line_join_t join);

/* ---- Fill rules --------------------------------------------------------- */

typedef enum {
    LUI_FILL_RULE_NONZERO = 0,
    LUI_FILL_RULE_EVENODD = 1,
} lui_fill_rule_t;

/**
 * Fill an arbitrary (possibly concave/self-intersecting) polygon using
 * the specified fill rule.
 *
 * @points  Array of vertices.
 * @count   Number of vertices (>= 3).
 * @color   Fill colour.
 * @rule    Fill rule (winding or even-odd).
 */
void lui_canvas_fill_polygon_ex(lui_canvas_t *canvas,
                                   const lui_point_t *points, int count,
                                   lui_color_t color,
                                   lui_fill_rule_t rule);

/**
 * Float-precision analogue of lui_canvas_fill_polygon_ex(). The software
 * rasteriser uses sub-scanline analytic-coverage AA (smooth edges; matches
 * blend2d/thorvg quality). Backends that only expose an integer-input
 * fill receive the polygon rounded to int.
 */
void lui_canvas_fill_polygonf_ex(lui_canvas_t *canvas,
                                    const lui_pointf_t *points, int count,
                                    lui_color_t color,
                                    lui_fill_rule_t rule);

/**
 * Fill multiple closed sub-contours packed back-to-back in @points
 * (@contour_lengths[k] vertices each, summing to @count) as a SINGLE shape:
 * every contour's edges feed one rasteriser pass so the fill rule is applied
 * across all of them. This is what carves out holes (glyph counters, donut
 * shapes); filling each contour separately would fill the holes solid.
 *
 * @points           Concatenated vertices of all contours.
 * @count             Total vertex count (>= 3).
 * @contour_lengths  Per-contour vertex counts (summing to @count). NULL = one
 *                    contour spanning all @count points.
 * @n_contours       Number of entries in @contour_lengths.
 */
void lui_canvas_fill_polygonsf_ex(lui_canvas_t *canvas,
                                     const lui_pointf_t *points, int count,
                                     const int *contour_lengths, int n_contours,
                                     lui_color_t color,
                                     lui_fill_rule_t rule);

/* ---- Blit / composite --------------------------------------------------- */

/**
 * Copy pixels from @src into this canvas (no blending — destination alpha is
 * overwritten).
 *
 * @src_rect  Region of @src to copy; pass NULL for the full @src surface.
 * @dst_x/@dst_y  Top-left destination in canvas coordinates.
 */
void lui_canvas_blit(lui_canvas_t *canvas, int dst_x, int dst_y,
                      const lui_surface_t *src, const lui_rect_t *src_rect);

/**
 * Blend @src into this canvas using per-pixel SRC_OVER alpha compositing.
 *
 * @src_rect  Region of @src to blend; pass NULL for the full @src surface.
 * @dst_x/@dst_y  Top-left destination in canvas coordinates.
 */
void lui_canvas_blend(lui_canvas_t *canvas, int dst_x, int dst_y,
                       const lui_surface_t *src, const lui_rect_t *src_rect);

/**
 * Draw and optionally scale an image into the canvas using SRC_OVER.
 *
 * @dst_w/@dst_h  Destination size in canvas coordinates.
 * @src_rect      Source region within @src; pass NULL for the full image.
 * @filter        Sampling filter used when scaling.
 */
void lui_canvas_draw_image(lui_canvas_t *canvas,
                            int dst_x, int dst_y, int dst_w, int dst_h,
                            const lui_surface_t *src, const lui_rect_t *src_rect,
                            lui_image_filter_t filter);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIGHTUI_CANVAS_H */
