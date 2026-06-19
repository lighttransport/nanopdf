/*
 * src/svg/svg_tiny.c — Minimal SVG-subset parser and renderer.
 *
 * Single-TU implementation. Parser is pull-style (no DOM): each <g>
 * pushes a transform/style frame, each <path> emits flattened-into-
 * world-space path commands. Bézier flattening is deferred until
 * render time so a viewport zoom can re-flatten without re-parsing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lightui/svg.h"
#include "internal/lui_grow.h"
#include "internal/lui_log.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Affine 2x3 matrix --------------------------------------------------- */

typedef struct {
    /* [a c e]
     * [b d f] */
    float a, b, c, d, e, f;
} mat2x3_t;

static const mat2x3_t MAT_IDENTITY = {1, 0, 0, 1, 0, 0};

static mat2x3_t mat_mul(mat2x3_t L, mat2x3_t R)
{
    mat2x3_t M;
    M.a = L.a * R.a + L.c * R.b;
    M.b = L.b * R.a + L.d * R.b;
    M.c = L.a * R.c + L.c * R.d;
    M.d = L.b * R.c + L.d * R.d;
    M.e = L.a * R.e + L.c * R.f + L.e;
    M.f = L.b * R.e + L.d * R.f + L.f;
    return M;
}

static void mat_apply(const mat2x3_t *M, float x, float y, float *ox, float *oy)
{
    *ox = M->a * x + M->c * y + M->e;
    *oy = M->b * x + M->d * y + M->f;
}

/* Average scale factor — used when projecting stroke-width through a
 * transform: tigers and the like uniformly scale, so the geometric mean
 * of |det| is a fine approximation.  */
static float mat_avg_scale(const mat2x3_t *M)
{
    float det = M->a * M->d - M->b * M->c;
    if (det < 0) det = -det;
    return sqrtf(det);
}

/* ---- Path command stream ------------------------------------------------- */

typedef enum {
    PCMD_MOVE  = 'M',
    PCMD_LINE  = 'L',
    PCMD_CUBIC = 'C',
    PCMD_QUAD  = 'Q',
    PCMD_CLOSE = 'Z',
} pcmd_kind_t;

typedef struct {
    /* Command kind + up to 3 control points (cubic uses all three;
     * quadratic uses 2; line/move use 1; close uses none). All coords
     * are pre-baked into world space at parse time. */
    uint8_t kind;
    float   x[3], y[3];
} path_cmd_t;

/* ---- Resolved style ------------------------------------------------------ */

typedef struct {
    bool          has_fill;        /* "none" → false; absent → inherits */
    bool          has_stroke;
    bool          fill_rule_evenodd;
    lvg_color_t   fill;
    lvg_color_t   stroke;
    float         stroke_width;
    float         opacity;         /* compounded down the tree */
    lvg_line_cap_t  cap;
    lvg_line_join_t join;
} style_t;

static const style_t STYLE_DEFAULT = {
    .has_fill          = true,                          /* SVG default fill = black */
    .has_stroke        = false,
    .fill_rule_evenodd = false,
    .fill              = LVG_COLOR_BLACK,
    .stroke            = LVG_COLOR_BLACK,
    .stroke_width      = 1.0f,
    .opacity           = 1.0f,
    .cap               = LVG_LINE_CAP_BUTT,
    .join              = LVG_LINE_JOIN_MITER,
};

/* ---- Shape (post-bake unit) --------------------------------------------- */

typedef struct {
    path_cmd_t *cmds;
    int         cmd_count;
    int         cmd_cap;

    /* Stroke width is stored in *world* units; the renderer projects it
     * to device space on the fly so zoom feels physical. */
    style_t     style;

    /* Cached flattened polylines. One sub-polyline per subpath (a M
     * starts a new subpath). Two parallel arrays in lockstep:
     *   pts[poly_offset[i] .. poly_offset[i+1]] = subpath i
     * Cache is invalidated when the requested device-space tolerance
     * shifts more than 25% from the cached one. */
    lvg_pointf_t *pts;
    int          *poly_offset;       /* count = subpath_count + 1 */
    int           subpath_count;
    int           pts_count;
    int           pts_cap;
    int           offset_cap;
    float         baked_tol_world;   /* sentinel <0 → not yet baked */
} shape_t;

/* ---- Document ------------------------------------------------------------ */

struct lui_svg_doc {
    shape_t *shapes;
    int      shape_count;
    int      shape_cap;

    lui_svg_viewbox_t viewbox;
    bool              has_viewbox;
    float             root_width, root_height;

    /* Render scratch — re-used across all shapes within one
     * lui_svg_render call so the per-frame alloc churn collapses
     * to one realloc-when-grown per buffer. Doc-bound (rather than
     * static) so destroy frees them and concurrent docs don't share
     * state. */
    lvg_pointf_t *scratch_f;
    int           scratch_f_cap;
    lvg_point_t  *scratch_i;
    int           scratch_i_cap;
};

/* ---- Small helpers ------------------------------------------------------- */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool slice_eq(const char *p, int n, const char *kw)
{
    int klen = (int)strlen(kw);
    if (n != klen) return false;
    return memcmp(p, kw, (size_t)n) == 0;
}

static bool slice_ieq(const char *p, int n, const char *kw)
{
    int klen = (int)strlen(kw);
    if (n != klen) return false;
    for (int i = 0; i < n; i++)
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)kw[i]))
            return false;
    return true;
}

/* parse_color: #rgb / #rrggbb / rgb(...) / "none" / "currentColor" / a
 * small named-colour table. Returns true and fills *out on success.
 * "none" yields *transparent = true. */
static bool parse_color(const char *p, int n, lvg_color_t *out, bool *transparent)
{
    while (n > 0 && isspace((unsigned char)*p)) { p++; n--; }
    while (n > 0 && isspace((unsigned char)p[n - 1])) n--;

    *transparent = false;

    if (n == 0) return false;

    if (slice_ieq(p, n, "none")) {
        *transparent = true;
        return true;
    }
    if (slice_ieq(p, n, "currentColor")) {
        /* No <use>/<context-fill> support — treat as default black. */
        *out = LVG_COLOR_BLACK;
        return true;
    }

    if (*p == '#') {
        const char *ds = p + 1;
        int digits = n - 1;
        for (int i = 0; i < digits; i++)
            if (hex_digit(ds[i]) < 0) return false;

        if (digits == 6) {
            int r = (hex_digit(ds[0]) << 4) | hex_digit(ds[1]);
            int g = (hex_digit(ds[2]) << 4) | hex_digit(ds[3]);
            int b = (hex_digit(ds[4]) << 4) | hex_digit(ds[5]);
            *out = LVG_COLOR_RGB(r, g, b);
            return true;
        }
        if (digits == 3) {
            int r = hex_digit(ds[0]); r = (r << 4) | r;
            int g = hex_digit(ds[1]); g = (g << 4) | g;
            int b = hex_digit(ds[2]); b = (b << 4) | b;
            *out = LVG_COLOR_RGB(r, g, b);
            return true;
        }
        return false;
    }

    if (n >= 4 && p[0] == 'r' && p[1] == 'g' && p[2] == 'b' && p[3] == '(') {
        const char *q = p + 4;
        const char *end = p + n;
        char *eptr;
        long r = strtol(q, &eptr, 10); q = eptr;
        while (q < end && (*q == ',' || isspace((unsigned char)*q))) q++;
        long g = strtol(q, &eptr, 10); q = eptr;
        while (q < end && (*q == ',' || isspace((unsigned char)*q))) q++;
        long b = strtol(q, &eptr, 10);
        if (r < 0)   r = 0;
        if (r > 255) r = 255;
        if (g < 0)   g = 0;
        if (g > 255) g = 255;
        if (b < 0)   b = 0;
        if (b > 255) b = 255;
        *out = LVG_COLOR_RGB((int)r, (int)g, (int)b);
        return true;
    }

    static const struct { const char *name; lvg_color_t c; } named[] = {
        {"black",   LVG_COLOR_RGB(0x00, 0x00, 0x00)},
        {"white",   LVG_COLOR_RGB(0xFF, 0xFF, 0xFF)},
        {"red",     LVG_COLOR_RGB(0xFF, 0x00, 0x00)},
        {"green",   LVG_COLOR_RGB(0x00, 0x80, 0x00)},
        {"blue",    LVG_COLOR_RGB(0x00, 0x00, 0xFF)},
        {"yellow",  LVG_COLOR_RGB(0xFF, 0xFF, 0x00)},
        {"orange",  LVG_COLOR_RGB(0xFF, 0xA5, 0x00)},
        {"purple",  LVG_COLOR_RGB(0x80, 0x00, 0x80)},
        {"gray",    LVG_COLOR_RGB(0x80, 0x80, 0x80)},
        {"grey",    LVG_COLOR_RGB(0x80, 0x80, 0x80)},
        {"silver",  LVG_COLOR_RGB(0xC0, 0xC0, 0xC0)},
        {"maroon",  LVG_COLOR_RGB(0x80, 0x00, 0x00)},
        {"navy",    LVG_COLOR_RGB(0x00, 0x00, 0x80)},
        {"teal",    LVG_COLOR_RGB(0x00, 0x80, 0x80)},
        {"olive",   LVG_COLOR_RGB(0x80, 0x80, 0x00)},
        {"lime",    LVG_COLOR_RGB(0x00, 0xFF, 0x00)},
        {"aqua",    LVG_COLOR_RGB(0x00, 0xFF, 0xFF)},
        {"cyan",    LVG_COLOR_RGB(0x00, 0xFF, 0xFF)},
        {"magenta", LVG_COLOR_RGB(0xFF, 0x00, 0xFF)},
        {"fuchsia", LVG_COLOR_RGB(0xFF, 0x00, 0xFF)},
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (slice_ieq(p, n, named[i].name)) {
            *out = named[i].c;
            return true;
        }
    }
    return false;
}

/* Skip whitespace and an optional comma. */
static const char *skip_wsp_comma(const char *p, const char *end)
{
    while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
    return p;
}

static const char *skip_wsp(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

/* Parse a single SVG number. Tolerant of "1.2-3.4" boundaries. */
static const char *scan_number(const char *p, const char *end, float *out)
{
    p = skip_wsp_comma(p, end);
    if (p >= end) return NULL;

    const char *start = p;
    if (*p == '+' || *p == '-') p++;
    bool any_digit = false;
    while (p < end && isdigit((unsigned char)*p)) { p++; any_digit = true; }
    if (p < end && *p == '.') {
        p++;
        while (p < end && isdigit((unsigned char)*p)) { p++; any_digit = true; }
    }
    if (!any_digit) return NULL;
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        while (p < end && isdigit((unsigned char)*p)) p++;
    }

    char buf[64];
    int n = (int)(p - start);
    if (n >= (int)sizeof(buf)) return NULL;
    memcpy(buf, start, (size_t)n);
    buf[n] = 0;
    *out = strtof(buf, NULL);
    return p;
}

/* ---- Transform parsing --------------------------------------------------- */

static mat2x3_t parse_transform_attr(const char *p, int n)
{
    mat2x3_t M = MAT_IDENTITY;
    const char *end = p + n;

    while (p < end) {
        p = skip_wsp_comma(p, end);
        if (p >= end) break;

        const char *name = p;
        while (p < end && isalpha((unsigned char)*p)) p++;
        int name_len = (int)(p - name);

        p = skip_wsp(p, end);
        if (p >= end || *p != '(') { p++; continue; }
        p++;

        float args[6];
        int   nargs = 0;
        while (p < end && *p != ')' && nargs < 6) {
            float v;
            const char *nxt = scan_number(p, end, &v);
            if (!nxt) break;
            args[nargs++] = v;
            p = nxt;
        }
        while (p < end && *p != ')') p++;
        if (p < end) p++;

        mat2x3_t T = MAT_IDENTITY;
        if (slice_eq(name, name_len, "matrix") && nargs == 6) {
            T.a = args[0]; T.b = args[1];
            T.c = args[2]; T.d = args[3];
            T.e = args[4]; T.f = args[5];
        } else if (slice_eq(name, name_len, "translate") && nargs >= 1) {
            T.e = args[0];
            T.f = (nargs >= 2) ? args[1] : 0.0f;
        } else if (slice_eq(name, name_len, "scale") && nargs >= 1) {
            T.a = args[0];
            T.d = (nargs >= 2) ? args[1] : args[0];
        } else if (slice_eq(name, name_len, "rotate") && nargs >= 1) {
            float th = args[0] * (float)(3.14159265358979323846 / 180.0);
            float ct = cosf(th), st = sinf(th);
            if (nargs == 3) {
                /* Translate-rotate-translate around (cx, cy). */
                mat2x3_t T1 = {1, 0, 0, 1,  args[1],  args[2]};
                mat2x3_t TR = {ct, st, -st, ct, 0, 0};
                mat2x3_t T2 = {1, 0, 0, 1, -args[1], -args[2]};
                T = mat_mul(mat_mul(T1, TR), T2);
            } else {
                T.a = ct; T.b = st;
                T.c = -st; T.d = ct;
            }
        } else if (slice_eq(name, name_len, "skewX") && nargs >= 1) {
            T.c = tanf(args[0] * (float)(3.14159265358979323846 / 180.0));
        } else if (slice_eq(name, name_len, "skewY") && nargs >= 1) {
            T.b = tanf(args[0] * (float)(3.14159265358979323846 / 180.0));
        } else {
            continue;
        }
        M = mat_mul(M, T);
    }
    return M;
}

/* ---- Style / attribute parsing ------------------------------------------ */

static void apply_style_kv(style_t *s,
                           const char *key, int key_len,
                           const char *val, int val_len)
{
    while (val_len > 0 && isspace((unsigned char)*val)) { val++; val_len--; }
    while (val_len > 0 && isspace((unsigned char)val[val_len - 1])) val_len--;
    if (val_len == 0) return;

    if (slice_eq(key, key_len, "fill")) {
        lvg_color_t c; bool tr;
        if (parse_color(val, val_len, &c, &tr)) {
            if (tr) { s->has_fill = false; }
            else    { s->fill = c; s->has_fill = true; }
        }
    } else if (slice_eq(key, key_len, "stroke")) {
        lvg_color_t c; bool tr;
        if (parse_color(val, val_len, &c, &tr)) {
            if (tr) { s->has_stroke = false; }
            else    { s->stroke = c; s->has_stroke = true; }
        }
    } else if (slice_eq(key, key_len, "stroke-width")) {
        float v;
        if (scan_number(val, val + val_len, &v) && v >= 0)
            s->stroke_width = v;
    } else if (slice_eq(key, key_len, "fill-rule")) {
        s->fill_rule_evenodd = slice_eq(val, val_len, "evenodd");
    } else if (slice_eq(key, key_len, "stroke-linecap")) {
        if      (slice_eq(val, val_len, "round"))  s->cap = LVG_LINE_CAP_ROUND;
        else if (slice_eq(val, val_len, "square")) s->cap = LVG_LINE_CAP_SQUARE;
        else                                       s->cap = LVG_LINE_CAP_BUTT;
    } else if (slice_eq(key, key_len, "stroke-linejoin")) {
        if      (slice_eq(val, val_len, "round")) s->join = LVG_LINE_JOIN_ROUND;
        else if (slice_eq(val, val_len, "bevel")) s->join = LVG_LINE_JOIN_BEVEL;
        else                                      s->join = LVG_LINE_JOIN_MITER;
    } else if (slice_eq(key, key_len, "opacity") ||
               slice_eq(key, key_len, "fill-opacity") ||
               slice_eq(key, key_len, "stroke-opacity")) {
        float v;
        if (scan_number(val, val + val_len, &v)) {
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            /* Compound: an outer 0.5 + an inner 0.5 fold to 0.25.
             * SVG technically separates fill-opacity and opacity, but
             * the tiger doesn't use either, so a single channel
             * approximation is fine. */
            s->opacity *= v;
        }
    }
}

/* Walk style="k:v;k:v" and apply every key. */
static void parse_style_attr(style_t *s, const char *val, int val_len)
{
    const char *p = val;
    const char *end = val + val_len;

    while (p < end) {
        p = skip_wsp(p, end);
        if (p >= end) break;

        const char *kstart = p;
        while (p < end && *p != ':' && *p != ';') p++;
        if (p >= end || *p != ':') { if (p < end) p++; continue; }
        int klen = (int)(p - kstart);
        while (klen > 0 && isspace((unsigned char)kstart[klen - 1])) klen--;
        p++;

        p = skip_wsp(p, end);
        const char *vstart = p;
        while (p < end && *p != ';') p++;
        int vlen = (int)(p - vstart);
        if (p < end && *p == ';') p++;

        apply_style_kv(s, kstart, klen, vstart, vlen);
    }
}

/* ---- shape_t allocation helpers ----------------------------------------- */

static bool shape_grow_cmds(shape_t *sh, int need)
{
    if (sh->cmd_count + need <= sh->cmd_cap) return true;
    int cap = sh->cmd_cap ? sh->cmd_cap : 16;
    while (cap < sh->cmd_count + need) cap *= 2;
    path_cmd_t *p = (path_cmd_t *)realloc(sh->cmds, (size_t)cap * sizeof(*p));
    if (!p) return false;
    sh->cmds = p;
    sh->cmd_cap = cap;
    return true;
}

static bool shape_push_cmd(shape_t *sh, const path_cmd_t *c)
{
    if (!shape_grow_cmds(sh, 1)) return false;
    sh->cmds[sh->cmd_count++] = *c;
    return true;
}

static bool doc_grow_shapes(lui_svg_doc_t *doc)
{
    if (doc->shape_count < doc->shape_cap) return true;
    int cap = doc->shape_cap ? doc->shape_cap * 2 : 32;
    shape_t *p = (shape_t *)realloc(doc->shapes, (size_t)cap * sizeof(*p));
    if (!p) return false;
    doc->shapes = p;
    doc->shape_cap = cap;
    return true;
}

/* ---- Path data tokenizer ------------------------------------------------- */

/*
 * Parse <path d="..."> into the world-space command stream. Supports
 * M/m, L/l, H/h, V/v, C/c, S/s, Q/q, T/t, Z/z. A/a logs a warning and
 * draws a straight line to the endpoint (the tiger does not use arcs).
 */
static void parse_path_d(shape_t *sh, const mat2x3_t *xform,
                         const char *d, int n)
{
    const char *p = d;
    const char *end = d + n;

    char prev_cmd = 0;
    /* Current pen and last control points (in *user* space — pre-xform). */
    float cx = 0, cy = 0;
    float subpath_x = 0, subpath_y = 0;
    float last_cubic_cx = 0, last_cubic_cy = 0;
    float last_quad_cx  = 0, last_quad_cy  = 0;
    bool   has_last_cubic = false;
    bool   has_last_quad  = false;

    while (p < end) {
        p = skip_wsp_comma(p, end);
        if (p >= end) break;

        char cmd = *p;
        if (isalpha((unsigned char)cmd)) {
            p++;
        } else {
            /* Implicit-repeat: continue with the previous command. After
             * a moveto, repeats default to lineto (M→L, m→l). */
            if (prev_cmd == 'M') cmd = 'L';
            else if (prev_cmd == 'm') cmd = 'l';
            else if (prev_cmd == 0)   { return; }
            else                      cmd = prev_cmd;
        }

        bool rel = islower((unsigned char)cmd);
        char ucmd = (char)toupper((unsigned char)cmd);

        path_cmd_t out;
        out.kind = 0;

        switch (ucmd) {
        case 'M': {
            float x, y;
            const char *q = scan_number(p, end, &x);
            if (!q) return;
            q = scan_number(q, end, &y);
            if (!q) return;
            p = q;
            if (rel) { x += cx; y += cy; }
            cx = x; cy = y;
            subpath_x = cx; subpath_y = cy;
            out.kind = PCMD_MOVE;
            mat_apply(xform, cx, cy, &out.x[0], &out.y[0]);
            shape_push_cmd(sh, &out);
            has_last_cubic = has_last_quad = false;
            break;
        }
        case 'L': {
            float x, y;
            const char *q = scan_number(p, end, &x);
            if (!q) return;
            q = scan_number(q, end, &y);
            if (!q) return;
            p = q;
            if (rel) { x += cx; y += cy; }
            cx = x; cy = y;
            out.kind = PCMD_LINE;
            mat_apply(xform, cx, cy, &out.x[0], &out.y[0]);
            shape_push_cmd(sh, &out);
            has_last_cubic = has_last_quad = false;
            break;
        }
        case 'H': {
            float x;
            const char *q = scan_number(p, end, &x);
            if (!q) return;
            p = q;
            if (rel) x += cx;
            cx = x;
            out.kind = PCMD_LINE;
            mat_apply(xform, cx, cy, &out.x[0], &out.y[0]);
            shape_push_cmd(sh, &out);
            has_last_cubic = has_last_quad = false;
            break;
        }
        case 'V': {
            float y;
            const char *q = scan_number(p, end, &y);
            if (!q) return;
            p = q;
            if (rel) y += cy;
            cy = y;
            out.kind = PCMD_LINE;
            mat_apply(xform, cx, cy, &out.x[0], &out.y[0]);
            shape_push_cmd(sh, &out);
            has_last_cubic = has_last_quad = false;
            break;
        }
        case 'C': {
            float x1, y1, x2, y2, x, y;
            const char *q = p;
            q = scan_number(q, end, &x1); if (!q) return;
            q = scan_number(q, end, &y1); if (!q) return;
            q = scan_number(q, end, &x2); if (!q) return;
            q = scan_number(q, end, &y2); if (!q) return;
            q = scan_number(q, end, &x);  if (!q) return;
            q = scan_number(q, end, &y);  if (!q) return;
            p = q;
            if (rel) { x1+=cx; y1+=cy; x2+=cx; y2+=cy; x+=cx; y+=cy; }
            out.kind = PCMD_CUBIC;
            mat_apply(xform, x1, y1, &out.x[0], &out.y[0]);
            mat_apply(xform, x2, y2, &out.x[1], &out.y[1]);
            mat_apply(xform, x,  y,  &out.x[2], &out.y[2]);
            shape_push_cmd(sh, &out);
            last_cubic_cx = x2; last_cubic_cy = y2;
            cx = x; cy = y;
            has_last_cubic = true; has_last_quad = false;
            break;
        }
        case 'S': {
            float x2, y2, x, y;
            const char *q = p;
            q = scan_number(q, end, &x2); if (!q) return;
            q = scan_number(q, end, &y2); if (!q) return;
            q = scan_number(q, end, &x);  if (!q) return;
            q = scan_number(q, end, &y);  if (!q) return;
            p = q;
            if (rel) { x2+=cx; y2+=cy; x+=cx; y+=cy; }
            float x1, y1;
            if (has_last_cubic) {
                x1 = 2 * cx - last_cubic_cx;
                y1 = 2 * cy - last_cubic_cy;
            } else {
                x1 = cx; y1 = cy;
            }
            out.kind = PCMD_CUBIC;
            mat_apply(xform, x1, y1, &out.x[0], &out.y[0]);
            mat_apply(xform, x2, y2, &out.x[1], &out.y[1]);
            mat_apply(xform, x,  y,  &out.x[2], &out.y[2]);
            shape_push_cmd(sh, &out);
            last_cubic_cx = x2; last_cubic_cy = y2;
            cx = x; cy = y;
            has_last_cubic = true; has_last_quad = false;
            break;
        }
        case 'Q': {
            float x1, y1, x, y;
            const char *q = p;
            q = scan_number(q, end, &x1); if (!q) return;
            q = scan_number(q, end, &y1); if (!q) return;
            q = scan_number(q, end, &x);  if (!q) return;
            q = scan_number(q, end, &y);  if (!q) return;
            p = q;
            if (rel) { x1+=cx; y1+=cy; x+=cx; y+=cy; }
            out.kind = PCMD_QUAD;
            mat_apply(xform, x1, y1, &out.x[0], &out.y[0]);
            mat_apply(xform, x,  y,  &out.x[1], &out.y[1]);
            shape_push_cmd(sh, &out);
            last_quad_cx = x1; last_quad_cy = y1;
            cx = x; cy = y;
            has_last_quad = true; has_last_cubic = false;
            break;
        }
        case 'T': {
            float x, y;
            const char *q = p;
            q = scan_number(q, end, &x); if (!q) return;
            q = scan_number(q, end, &y); if (!q) return;
            p = q;
            if (rel) { x+=cx; y+=cy; }
            float x1, y1;
            if (has_last_quad) {
                x1 = 2 * cx - last_quad_cx;
                y1 = 2 * cy - last_quad_cy;
            } else {
                x1 = cx; y1 = cy;
            }
            out.kind = PCMD_QUAD;
            mat_apply(xform, x1, y1, &out.x[0], &out.y[0]);
            mat_apply(xform, x,  y,  &out.x[1], &out.y[1]);
            shape_push_cmd(sh, &out);
            last_quad_cx = x1; last_quad_cy = y1;
            cx = x; cy = y;
            has_last_quad = true; has_last_cubic = false;
            break;
        }
        case 'A': {
            /* Eat the 7 arc parameters and draw a straight line to the
             * endpoint. The tiger doesn't use elliptical arcs. */
            float discard, x, y;
            const char *q = p;
            for (int j = 0; j < 5; j++) {
                q = scan_number(q, end, &discard);
                if (!q) return;
            }
            q = scan_number(q, end, &x); if (!q) return;
            q = scan_number(q, end, &y); if (!q) return;
            p = q;
            if (rel) { x += cx; y += cy; }
            cx = x; cy = y;
            out.kind = PCMD_LINE;
            mat_apply(xform, cx, cy, &out.x[0], &out.y[0]);
            shape_push_cmd(sh, &out);
            has_last_cubic = has_last_quad = false;
            LUI_LOG_WARN("svg_tiny: elliptical arc 'A' not implemented; "
                         "approximated as line-to");
            break;
        }
        case 'Z': {
            cx = subpath_x; cy = subpath_y;
            out.kind = PCMD_CLOSE;
            shape_push_cmd(sh, &out);
            has_last_cubic = has_last_quad = false;
            break;
        }
        default:
            LUI_LOG_WARN("svg_tiny: unknown path command '%c'", cmd);
            return;
        }
        prev_cmd = cmd;
    }
}

/* ---- Pull XML scanner ---------------------------------------------------- */

typedef struct {
    const char *src;
    const char *end;
    const char *cur;
} xml_scanner_t;

typedef struct {
    /* For elt_start: tag is the element name, and attrs spans the
     * remainder of the open tag up to '>' or '/>' (caller scans
     * attributes via attr_next on demand). */
    const char *tag;
    int         tag_len;
    const char *attrs;
    int         attrs_len;
    bool        self_closing;
} xml_elt_t;

/* Find next '<' that opens a real element (skips comments, PIs, DOCTYPEs,
 * CDATA, and end-tags consumed by xml_elt_end). Returns false at EOF. */
static bool xml_next_elt(xml_scanner_t *s, xml_elt_t *out, bool *is_end)
{
    while (s->cur < s->end) {
        const char *p = s->cur;
        const char *lt = (const char *)memchr(p, '<', (size_t)(s->end - p));
        if (!lt) { s->cur = s->end; return false; }

        /* Comment <!-- ... --> */
        if (lt + 4 <= s->end && lt[1] == '!' && lt[2] == '-' && lt[3] == '-') {
            const char *q = lt + 4;
            while (q + 3 <= s->end && !(q[0] == '-' && q[1] == '-' && q[2] == '>')) q++;
            s->cur = (q + 3 <= s->end) ? q + 3 : s->end;
            continue;
        }
        /* PI / DOCTYPE / CDATA / generic <!...> — skip to next '>' */
        if (lt + 2 <= s->end && (lt[1] == '?' || lt[1] == '!')) {
            const char *q = lt + 2;
            while (q < s->end && *q != '>') q++;
            s->cur = (q < s->end) ? q + 1 : s->end;
            continue;
        }

        if (lt + 2 <= s->end && lt[1] == '/') {
            *is_end = true;
            const char *q = lt + 2;
            const char *name = q;
            while (q < s->end && *q != '>' && !isspace((unsigned char)*q)) q++;
            out->tag = name;
            out->tag_len = (int)(q - name);
            while (q < s->end && *q != '>') q++;
            s->cur = (q < s->end) ? q + 1 : s->end;
            out->attrs = NULL; out->attrs_len = 0;
            out->self_closing = false;
            return true;
        }

        /* Start tag */
        *is_end = false;
        const char *q = lt + 1;
        const char *name = q;
        while (q < s->end && *q != '>' && *q != '/' &&
               !isspace((unsigned char)*q)) q++;
        out->tag = name;
        out->tag_len = (int)(q - name);

        const char *attr_start = q;
        bool self_close = false;

        /* Walk to matching '>', tracking quotes so '>' inside an
         * attribute value doesn't mis-close. */
        while (q < s->end) {
            if (*q == '"' || *q == '\'') {
                char qc = *q++;
                while (q < s->end && *q != qc) q++;
                if (q < s->end) q++;
                continue;
            }
            if (*q == '/' && q + 1 < s->end && q[1] == '>') {
                self_close = true;
                q += 2;
                break;
            }
            if (*q == '>') { q++; break; }
            q++;
        }
        const char *attrs_end = q - (self_close ? 2 : 1);
        if (attrs_end < attr_start) attrs_end = attr_start;

        out->attrs = attr_start;
        out->attrs_len = (int)(attrs_end - attr_start);
        out->self_closing = self_close;
        s->cur = q;
        return true;
    }
    return false;
}

/* Pull next (key, value) attribute pair from an attribute-span slice. */
static bool attr_next(const char **pp, const char *end,
                      const char **k, int *klen,
                      const char **v, int *vlen)
{
    const char *p = *pp;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end) return false;

    const char *kstart = p;
    while (p < end && *p != '=' && !isspace((unsigned char)*p)) p++;
    *k = kstart;
    *klen = (int)(p - kstart);

    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '=') { *pp = p; *v = NULL; *vlen = 0; return *klen > 0; }
    p++;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end) { *pp = p; return false; }

    char qc = 0;
    if (*p == '"' || *p == '\'') { qc = *p++; }
    const char *vstart = p;
    if (qc) {
        while (p < end && *p != qc) p++;
        *v = vstart;
        *vlen = (int)(p - vstart);
        if (p < end) p++;
    } else {
        while (p < end && !isspace((unsigned char)*p)) p++;
        *v = vstart;
        *vlen = (int)(p - vstart);
    }
    *pp = p;
    return true;
}

/* ---- Style/attr resolution per element ---------------------------------- */

/* Walk attrs once, applying both presentation attrs (fill=...) and
 * style="...". Returns the per-element transform (caller composes onto
 * its parent). If @d_out is non-NULL and a `d` attribute is present,
 * writes its value slice into (*d_out, *d_len) — saves a second walk
 * for <path>. */
static mat2x3_t apply_attrs(const xml_elt_t *e, style_t *style,
                            const char **d_out, int *d_len)
{
    mat2x3_t local = MAT_IDENTITY;
    const char *p = e->attrs;
    const char *end = p + e->attrs_len;
    const char *k, *v;
    int klen, vlen;

    if (d_out) { *d_out = NULL; *d_len = 0; }

    while (attr_next(&p, end, &k, &klen, &v, &vlen)) {
        if (slice_eq(k, klen, "transform")) {
            local = parse_transform_attr(v, vlen);
        } else if (slice_eq(k, klen, "style")) {
            parse_style_attr(style, v, vlen);
        } else if (d_out && slice_eq(k, klen, "d")) {
            *d_out = v; *d_len = vlen;
        } else {
            apply_style_kv(style, k, klen, v, vlen);
        }
    }
    return local;
}

/* ---- viewBox / root ------------------------------------------------------ */

static void parse_root_attrs(lui_svg_doc_t *doc, const xml_elt_t *e)
{
    const char *p = e->attrs;
    const char *end = p + e->attrs_len;
    const char *k, *v;
    int klen, vlen;

    while (attr_next(&p, end, &k, &klen, &v, &vlen)) {
        if (slice_eq(k, klen, "viewBox")) {
            const char *q = v, *qe = v + vlen;
            float a, b, c, d;
            const char *r;
            r = scan_number(q, qe, &a); if (!r) continue; q = r;
            r = scan_number(q, qe, &b); if (!r) continue; q = r;
            r = scan_number(q, qe, &c); if (!r) continue; q = r;
            r = scan_number(q, qe, &d); if (!r) continue; q = r;
            doc->viewbox.min_x  = a;
            doc->viewbox.min_y  = b;
            doc->viewbox.width  = c;
            doc->viewbox.height = d;
            doc->has_viewbox = true;
        } else if (slice_eq(k, klen, "width")) {
            float w;
            if (scan_number(v, v + vlen, &w)) doc->root_width = w;
        } else if (slice_eq(k, klen, "height")) {
            float h;
            if (scan_number(v, v + vlen, &h)) doc->root_height = h;
        }
    }
}

/* ---- Bézier flattening --------------------------------------------------- */

static bool sh_grow_pts(shape_t *sh, int need)
{
    if (sh->pts_count + need <= sh->pts_cap) return true;
    int cap = sh->pts_cap ? sh->pts_cap : 32;
    while (cap < sh->pts_count + need) cap *= 2;
    lvg_pointf_t *p = (lvg_pointf_t *)realloc(sh->pts, (size_t)cap * sizeof(*p));
    if (!p) return false;
    sh->pts = p;
    sh->pts_cap = cap;
    return true;
}

static bool sh_grow_offsets(shape_t *sh, int need)
{
    if (sh->subpath_count + 1 + need <= sh->offset_cap) return true;
    int cap = sh->offset_cap ? sh->offset_cap : 8;
    while (cap < sh->subpath_count + 1 + need) cap *= 2;
    int *p = (int *)realloc(sh->poly_offset, (size_t)cap * sizeof(*p));
    if (!p) return false;
    sh->poly_offset = p;
    sh->offset_cap = cap;
    return true;
}

static void sh_push_pt(shape_t *sh, float x, float y)
{
    if (!sh_grow_pts(sh, 1)) return;
    sh->pts[sh->pts_count].x = x;
    sh->pts[sh->pts_count].y = y;
    sh->pts_count++;
}

/*
 * Adaptive De Casteljau subdivision of a cubic. Splits at t=0.5 until
 * both half-segments are flat enough (max control-point deviation
 * from the chord < tol). Recursion-capped at 16 levels.
 */
static void flatten_cubic(shape_t *sh,
                          float x0, float y0,
                          float x1, float y1,
                          float x2, float y2,
                          float x3, float y3,
                          float tol2, int depth)
{
    /* Cheap flatness test: distance of P1 and P2 from line P0-P3,
     * compared to tol². */
    float dx = x3 - x0, dy = y3 - y0;
    float len2 = dx * dx + dy * dy;
    float d1, d2;
    if (len2 < 1e-12f) {
        /* Endpoints coincide — bound by control-point spread. */
        d1 = (x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0);
        d2 = (x2 - x0) * (x2 - x0) + (y2 - y0) * (y2 - y0);
    } else {
        float c1 = (x1 - x0) * dy - (y1 - y0) * dx;
        float c2 = (x2 - x0) * dy - (y2 - y0) * dx;
        d1 = c1 * c1 / len2;
        d2 = c2 * c2 / len2;
    }
    if ((d1 <= tol2 && d2 <= tol2) || depth >= 16) {
        sh_push_pt(sh, x3, y3);
        return;
    }

    float x01 = 0.5f * (x0 + x1), y01 = 0.5f * (y0 + y1);
    float x12 = 0.5f * (x1 + x2), y12 = 0.5f * (y1 + y2);
    float x23 = 0.5f * (x2 + x3), y23 = 0.5f * (y2 + y3);
    float xA  = 0.5f * (x01 + x12), yA = 0.5f * (y01 + y12);
    float xB  = 0.5f * (x12 + x23), yB = 0.5f * (y12 + y23);
    float xC  = 0.5f * (xA + xB),   yC = 0.5f * (yA + yB);

    flatten_cubic(sh, x0, y0, x01, y01, xA, yA, xC, yC, tol2, depth + 1);
    flatten_cubic(sh, xC, yC, xB, yB, x23, y23, x3, y3, tol2, depth + 1);
}

static void flatten_quad(shape_t *sh,
                         float x0, float y0,
                         float x1, float y1,
                         float x2, float y2,
                         float tol2, int depth)
{
    float dx = x2 - x0, dy = y2 - y0;
    float len2 = dx * dx + dy * dy;
    float d;
    if (len2 < 1e-12f) {
        d = (x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0);
    } else {
        float c = (x1 - x0) * dy - (y1 - y0) * dx;
        d = c * c / len2;
    }
    if (d <= tol2 || depth >= 16) {
        sh_push_pt(sh, x2, y2);
        return;
    }
    float x01 = 0.5f * (x0 + x1), y01 = 0.5f * (y0 + y1);
    float x12 = 0.5f * (x1 + x2), y12 = 0.5f * (y1 + y2);
    float xC  = 0.5f * (x01 + x12), yC = 0.5f * (y01 + y12);
    flatten_quad(sh, x0, y0, x01, y01, xC, yC, tol2, depth + 1);
    flatten_quad(sh, xC, yC, x12, y12, x2, y2, tol2, depth + 1);
}

/* Bake all shape commands → sh->pts + sh->poly_offset. tol_world is
 * the chord-distance threshold in world units (caller converts from
 * device-space tolerance via /scale). */
static void shape_bake(shape_t *sh, float tol_world)
{
    sh->pts_count = 0;
    sh->subpath_count = 0;
    sh_grow_offsets(sh, 0);

    float tol2 = tol_world * tol_world;
    float cx = 0, cy = 0;
    float sub_x = 0, sub_y = 0;
    bool  has_pen = false;

    for (int i = 0; i < sh->cmd_count; i++) {
        const path_cmd_t *c = &sh->cmds[i];
        switch (c->kind) {
        case PCMD_MOVE:
            /* Close out the previous subpath. */
            if (has_pen) {
                if (!sh_grow_offsets(sh, 1)) return;
                sh->poly_offset[sh->subpath_count + 1] = sh->pts_count;
                sh->subpath_count++;
            }
            if (!sh_grow_offsets(sh, 1)) return;
            sh->poly_offset[sh->subpath_count] = sh->pts_count;
            cx = c->x[0]; cy = c->y[0];
            sub_x = cx; sub_y = cy;
            sh_push_pt(sh, cx, cy);
            has_pen = true;
            break;
        case PCMD_LINE:
            if (!has_pen) {
                if (!sh_grow_offsets(sh, 1)) return;
                sh->poly_offset[sh->subpath_count] = sh->pts_count;
                cx = c->x[0]; cy = c->y[0];
                sub_x = cx; sub_y = cy;
                sh_push_pt(sh, cx, cy);
                has_pen = true;
                break;
            }
            cx = c->x[0]; cy = c->y[0];
            sh_push_pt(sh, cx, cy);
            break;
        case PCMD_CUBIC:
            if (!has_pen) break;
            flatten_cubic(sh, cx, cy,
                          c->x[0], c->y[0],
                          c->x[1], c->y[1],
                          c->x[2], c->y[2],
                          tol2, 0);
            cx = c->x[2]; cy = c->y[2];
            break;
        case PCMD_QUAD:
            if (!has_pen) break;
            flatten_quad(sh, cx, cy,
                         c->x[0], c->y[0],
                         c->x[1], c->y[1],
                         tol2, 0);
            cx = c->x[1]; cy = c->y[1];
            break;
        case PCMD_CLOSE:
            if (has_pen) {
                if (sh->pts_count > 0 &&
                    (sh->pts[sh->pts_count - 1].x != sub_x ||
                     sh->pts[sh->pts_count - 1].y != sub_y))
                {
                    sh_push_pt(sh, sub_x, sub_y);
                }
                cx = sub_x; cy = sub_y;
                if (!sh_grow_offsets(sh, 1)) return;
                sh->poly_offset[sh->subpath_count + 1] = sh->pts_count;
                sh->subpath_count++;
                has_pen = false;
            }
            break;
        }
    }
    if (has_pen) {
        if (!sh_grow_offsets(sh, 1)) return;
        sh->poly_offset[sh->subpath_count + 1] = sh->pts_count;
        sh->subpath_count++;
    }
    sh->baked_tol_world = tol_world;
}

/* ---- Render -------------------------------------------------------------- */

static lvg_color_t apply_opacity(lvg_color_t c, float opacity)
{
    if (opacity >= 0.999f) return c;
    if (opacity < 0)       opacity = 0;
    int a = (int)((c >> 24) & 0xFF);
    a = (int)(a * opacity + 0.5f);
    if (a > 255) a = 255;
    if (a < 0)   a = 0;
    return (c & 0x00FFFFFFu) | ((lvg_color_t)a << 24);
}

/*
 * Render @sh using world-space points (sh->pts) projected through the
 * viewport (scale, tx, ty). @screen and @ipts are doc-owned scratch
 * buffers (sized for sh->pts_count); allocated/grown by the caller.
 */
static void render_shape(const shape_t *sh, lvg_canvas_t *canvas,
                         float tx, float ty, float scale,
                         lvg_pointf_t *screen, lvg_point_t *ipts)
{
    if (sh->subpath_count <= 0 || sh->pts_count < 2) return;

    for (int k = 0; k < sh->pts_count; k++) {
        screen[k].x = sh->pts[k].x * scale + tx;
        screen[k].y = sh->pts[k].y * scale + ty;
    }

    if (sh->style.has_fill) {
        lvg_color_t fc = apply_opacity(sh->style.fill, sh->style.opacity);
        (void)ipts;  /* float-input fill — software path uses analytic AA */
        for (int s = 0; s < sh->subpath_count; s++) {
            int beg = sh->poly_offset[s];
            int eend = sh->poly_offset[s + 1];
            int n = eend - beg;
            if (n < 3) continue;
            lvg_canvas_fill_polygonf_ex(canvas, &screen[beg], n, fc,
                sh->style.fill_rule_evenodd ? LVG_FILL_RULE_EVENODD
                                            : LVG_FILL_RULE_NONZERO);
        }
    }

    if (sh->style.has_stroke && sh->style.stroke_width > 0.0f) {
        lvg_color_t sc = apply_opacity(sh->style.stroke, sh->style.opacity);
        float w = sh->style.stroke_width * scale;
        if (w < 0.5f) w = 0.5f;
        for (int s = 0; s < sh->subpath_count; s++) {
            int beg = sh->poly_offset[s];
            int eend = sh->poly_offset[s + 1];
            int n = eend - beg;
            if (n < 2) continue;
            bool closed = (sh->pts[beg].x == sh->pts[eend - 1].x &&
                           sh->pts[beg].y == sh->pts[eend - 1].y);
            lvg_canvas_draw_styled_polyline(canvas, &screen[beg], n,
                                            sc, w, closed,
                                            sh->style.cap, sh->style.join);
        }
    }
}

/* ---- High-level loader --------------------------------------------------- */

/* Parse stack frame: each frame holds the composed transform and the
 * resolved style up to that point. <g> pushes; </g> pops. The depth
 * cap is generous — the tiger nests just two levels (svg → g). */
#define STACK_MAX 64

typedef struct {
    mat2x3_t xform;
    style_t  style;
} frame_t;

static lui_svg_doc_t *parse_xml(const char *src, size_t len)
{
    lui_svg_doc_t *doc = (lui_svg_doc_t *)calloc(1, sizeof(*doc));
    if (!doc) return NULL;

    xml_scanner_t s = {src, src + len, src};
    frame_t stack[STACK_MAX];
    int sp = 0;
    stack[0].xform = MAT_IDENTITY;
    stack[0].style = STYLE_DEFAULT;

    bool seen_root = false;

    while (s.cur < s.end) {
        xml_elt_t e;
        bool is_end;
        if (!xml_next_elt(&s, &e, &is_end)) break;

        if (is_end) {
            if (slice_eq(e.tag, e.tag_len, "g") && sp > 0) {
                sp--;
            }
            /* </svg> just ends parse — but be lenient about EOF
             * without explicit close. */
            continue;
        }

        if (slice_eq(e.tag, e.tag_len, "svg")) {
            if (!seen_root) {
                parse_root_attrs(doc, &e);
                seen_root = true;
            }
            continue;
        }

        if (slice_eq(e.tag, e.tag_len, "g")) {
            mat2x3_t parent_xform = stack[sp].xform;
            bool pushed = false;
            if (sp + 1 >= STACK_MAX) {
                LUI_LOG_WARN("svg_tiny: <g> nesting > %d, flattening", STACK_MAX);
            } else {
                sp++;
                stack[sp] = stack[sp - 1];
                pushed = true;
            }
            mat2x3_t local = apply_attrs(&e, &stack[sp].style, NULL, NULL);
            stack[sp].xform = mat_mul(parent_xform, local);
            /* Self-closing <g/> pops what it pushed (if anything). */
            if (e.self_closing && pushed) sp--;
            continue;
        }

        if (slice_eq(e.tag, e.tag_len, "path")) {
            frame_t fr = stack[sp];
            const char *d_val; int d_len;
            mat2x3_t local = apply_attrs(&e, &fr.style, &d_val, &d_len);
            mat2x3_t world = mat_mul(fr.xform, local);
            if (!d_val) continue;

            if (!doc_grow_shapes(doc)) goto oom;
            shape_t *sh = &doc->shapes[doc->shape_count];
            memset(sh, 0, sizeof(*sh));
            sh->style = fr.style;
            sh->baked_tol_world = -1.0f;
            /* Project stroke-width through the transform to keep
             * widths physical in world space. */
            sh->style.stroke_width = fr.style.stroke_width *
                                     mat_avg_scale(&world);

            parse_path_d(sh, &world, d_val, d_len);
            if (sh->cmd_count > 0) doc->shape_count++;
            else free(sh->cmds);

            continue;
        }

        /* Unsupported element — keep walking but skip its body. We do
         * NOT push a frame, so its </tag> is naturally a no-op. */
    }

    return doc;

oom:
    lui_svg_destroy(doc);
    return NULL;
}

lui_svg_doc_t *lui_svg_load_mem(const char *xml, size_t len)
{
    if (!xml || len == 0) return NULL;
    return parse_xml(xml, len);
}

lui_svg_doc_t *lui_svg_load_file(const char *path)
{
    if (!path) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LUI_LOG_WARN("svg_tiny: cannot open '%s'", path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    rewind(fp);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = 0;

    lui_svg_doc_t *doc = parse_xml(buf, got);
    free(buf);
    return doc;
}

static void shape_free_contents(shape_t *sh)
{
    free(sh->cmds);
    free(sh->pts);
    free(sh->poly_offset);
    sh->cmds = NULL;
    sh->pts = NULL;
    sh->poly_offset = NULL;
}

void lui_svg_destroy(lui_svg_doc_t *doc)
{
    if (!doc) return;
    for (int i = 0; i < doc->shape_count; i++)
        shape_free_contents(&doc->shapes[i]);
    free(doc->shapes);
    free(doc->scratch_f);
    free(doc->scratch_i);
    free(doc);
}

lui_svg_viewbox_t lui_svg_get_viewbox(const lui_svg_doc_t *doc)
{
    lui_svg_viewbox_t vb = {0, 0, 0, 0};
    if (!doc) return vb;
    if (doc->has_viewbox) return doc->viewbox;
    vb.width  = doc->root_width;
    vb.height = doc->root_height;
    return vb;
}

int lui_svg_shape_count(const lui_svg_doc_t *doc)
{
    return doc ? doc->shape_count : 0;
}

lvg_result_t lui_svg_render(const lui_svg_doc_t *doc,
                            lvg_canvas_t *canvas,
                            float tx, float ty, float scale,
                            float tol_dev_px)
{
    if (!doc || !canvas) return LVG_ERR_INVALID;
    if (scale <= 0)      return LVG_ERR_INVALID;
    if (tol_dev_px <= 0) tol_dev_px = 0.5f;

    /* Doc shapes carry a logical bake cache; we mutate it through this
     * single, contained const-cast. The doc remains "logically const"
     * to callers — repeated renders return identical pixels. */
    lui_svg_doc_t *mdoc = (lui_svg_doc_t *)doc;

    /* Device-space tolerance → world-space tolerance: a chord that's
     * <= tol_dev_px on screen is <= tol_dev_px/scale in world units. */
    float tol_world = tol_dev_px / scale;

    for (int i = 0; i < doc->shape_count; i++) {
        shape_t *sh = &mdoc->shapes[i];

        /* Re-bake when stale or the cached tolerance falls outside a
         * generous octave. The window is wide so a continuous-zoom
         * drag (~10% per scroll event) doesn't re-flatten on every
         * tick — flatness perception is logarithmic anyway. */
        bool need_bake = (sh->baked_tol_world < 0);
        if (!need_bake) {
            float ratio = tol_world / sh->baked_tol_world;
            if (ratio < 0.5f || ratio > 2.0f) need_bake = true;
        }
        if (need_bake) shape_bake(sh, tol_world);

        if (sh->subpath_count <= 0 || sh->pts_count < 2) continue;

        if (!lui_grow_scratch((void **)&mdoc->scratch_f, &mdoc->scratch_f_cap,
                          sh->pts_count, sizeof(lvg_pointf_t)))
            return LVG_ERR_NOMEM;
        if (!lui_grow_scratch((void **)&mdoc->scratch_i, &mdoc->scratch_i_cap,
                          sh->pts_count, sizeof(lvg_point_t)))
            return LVG_ERR_NOMEM;

        render_shape(sh, canvas, tx, ty, scale,
                     mdoc->scratch_f, mdoc->scratch_i);
    }
    return LVG_OK;
}
