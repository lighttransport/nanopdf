/*
 * src/geomap/geomap_tiny.c — MVT (Mapbox Vector Tile) parser + 2.5D renderer.
 *
 * Single-TU implementation: protobuf wire decoder, MVT message decode
 * (Tile/Layer/Feature/Value), MVT geometry command stream decode, then
 * an oblique-projection ("2.5D") renderer with painter's-algorithm
 * occlusion.
 *
 * Tested against the GSI "optimal_bvmap-v1" layer set (BldA, RdCL,
 * RvrCL, WA, RailCL, SpcfArea, AdmArea, AdmBdry, Cntr, Anno) but
 * accepts any well-formed MVT input.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lightui/geomap.h"
#include "internal/lui_grow.h"
#include "internal/lui_log.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Protobuf wire decoder ---------------------------------------------- */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bool           err;
} pb_t;

static uint64_t pb_varint(pb_t *pb)
{
    uint64_t v = 0;
    int shift = 0;
    while (pb->p < pb->end && shift < 64) {
        uint8_t b = *pb->p++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
        shift += 7;
    }
    pb->err = true;
    return 0;
}

static uint32_t pb_fixed32(pb_t *pb)
{
    if (pb->end - pb->p < 4) { pb->err = true; return 0; }
    uint32_t v;
    memcpy(&v, pb->p, 4);
    pb->p += 4;
    return v;
}

static uint64_t pb_fixed64(pb_t *pb)
{
    if (pb->end - pb->p < 8) { pb->err = true; return 0; }
    uint64_t v;
    memcpy(&v, pb->p, 8);
    pb->p += 8;
    return v;
}

/* Skip an unknown field given its wire type. Returns false on error. */
static bool pb_skip(pb_t *pb, int wire)
{
    switch (wire) {
    case 0: pb_varint(pb); return !pb->err;
    case 1: pb_fixed64(pb); return !pb->err;
    case 2: {
        uint64_t ln = pb_varint(pb);
        if (pb->err || ln > (uint64_t)(pb->end - pb->p)) {
            pb->err = true; return false;
        }
        pb->p += ln;
        return true;
    }
    case 5: pb_fixed32(pb); return !pb->err;
    default: pb->err = true; return false;
    }
}

/* Read a length-delimited field's payload as a sub-pb. Returns false if
 * the payload is truncated. */
static bool pb_sub(pb_t *pb, pb_t *sub)
{
    uint64_t ln = pb_varint(pb);
    if (pb->err || ln > (uint64_t)(pb->end - pb->p)) {
        pb->err = true; return false;
    }
    sub->p   = pb->p;
    sub->end = pb->p + ln;
    sub->err = false;
    pb->p   += ln;
    return true;
}

/* ---- MVT geometry command stream ---------------------------------------- */

/* zigzag(n) = (n >> 1) ^ -(n & 1) */
static int32_t zz_decode(uint32_t v)
{
    return (int32_t)((v >> 1) ^ (uint32_t)-(int32_t)(v & 1));
}

/* ---- Internal feature representation ------------------------------------ */

typedef enum {
    GEOM_POINT      = 1,    /* MVT GeomType.POINT      */
    GEOM_LINESTRING = 2,    /* MVT GeomType.LINESTRING */
    GEOM_POLYGON    = 3,    /* MVT GeomType.POLYGON    */
} geom_type_t;

typedef struct {
    /* All vertices in tile-local coordinates. The renderer adds the
     * (tile_x * extent, tile_y * extent) offset on the fly so multiple
     * tiles compose into a single world. */
    float        *xy;        /* interleaved x,y; 2*pts_count floats   */
    int           pts_count;
    /* For LINESTRING / POLYGON: indices into xy[] of subpath
     * boundaries. ring_off[0] = 0, ring_off[ring_count] = pts_count. */
    int          *ring_off;
    int           ring_count;
    int           ring_cap;
} feat_geom_t;

typedef struct {
    /* Originating tile so the renderer can position the geometry. */
    int               tile_x, tile_y;
    lui_geomap_kind_t kind;
    geom_type_t       type;
    feat_geom_t       g;

    /* Tile-local bounding box of the geometry, computed once at parse
     * time. The renderer adds the tile offset to translate to world
     * coords for viewport culling. */
    float bb_xmin, bb_ymin, bb_xmax, bb_ymax;

    /* Resolved attributes (we copy out the few fields we use rather
     * than retaining the full key/value pair list). */
    int   level_order;       /* vt_lvorder if present, else 0          */
    float road_width;        /* vt_width  for RdCL, in tile units      */
    int   road_category;     /* vt_rdctg  (1..6 ish, see GSI docs)     */

    /* Cached metric used for painter-algorithm sort: the pivot world
     * y-coord for the feature (max y). Filled in by render. */
    float sort_y_max;
} feat_t;

/* Per-kind bucket: indices into doc->feats. Built lazily on first
 * render and invalidated by add_tile. Lets the renderer iterate
 * just the features of one kind instead of scanning all n_feats
 * five times per frame. */
#define LUI_GEOMAP_KIND_COUNT 8

typedef struct {
    int *idx;
    int  count;
    int  cap;
} kind_bucket_t;

struct lui_geomap_doc {
    feat_t *feats;
    int     n_feats;
    int     cap_feats;

    /* Doc-level bbox + zoom. First add_tile establishes z/extent;
     * subsequent tiles must match. */
    int z;
    int extent;
    int x_min, y_min, x_max, y_max;
    bool has_bounds;

    kind_bucket_t buckets[LUI_GEOMAP_KIND_COUNT];
    bool          buckets_valid;

    /* Render scratch — re-used across all features in one render. */
    lvg_pointf_t *scratch_f;
    int           scratch_f_cap;
    lvg_point_t  *scratch_i;
    int           scratch_i_cap;
    feat_t      **sort_ptrs;
    int           sort_cap;
};

/* ---- feat_geom_t allocators -------------------------------------------- */

/* Capacity is tracked locally in decode_geometry rather than on the
 * feat_geom_t — the feature is one-shot at parse time, so we don't
 * need to retain the cap after decode finishes. */
static bool g_push_pt_capped(feat_geom_t *g, int *cap, float x, float y)
{
    if (g->pts_count >= *cap) {
        int nc = *cap ? *cap * 2 : 16;
        while (nc <= g->pts_count) nc *= 2;
        float *p = (float *)realloc(g->xy, (size_t)nc * 2 * sizeof(float));
        if (!p) return false;
        g->xy = p;
        *cap = nc;
    }
    g->xy[2 * g->pts_count + 0] = x;
    g->xy[2 * g->pts_count + 1] = y;
    g->pts_count++;
    return true;
}

static bool g_push_ring(feat_geom_t *g, int offset)
{
    if (g->ring_count + 1 >= g->ring_cap) {
        int nc = g->ring_cap ? g->ring_cap * 2 : 4;
        while (nc <= g->ring_count + 1) nc *= 2;
        int *p = (int *)realloc(g->ring_off, (size_t)nc * sizeof(int));
        if (!p) return false;
        g->ring_off = p;
        g->ring_cap = nc;
    }
    g->ring_off[g->ring_count++] = offset;
    return true;
}

static void g_free(feat_geom_t *g)
{
    free(g->xy);
    free(g->ring_off);
    g->xy = NULL;
    g->ring_off = NULL;
    g->pts_count = g->ring_count = g->ring_cap = 0;
}

/* Compute the tile-local AABB from the decoded vertex stream. */
static void compute_feat_bbox(feat_t *f)
{
    if (f->g.pts_count == 0) {
        f->bb_xmin = f->bb_ymin = f->bb_xmax = f->bb_ymax = 0.0f;
        return;
    }
    float xn = f->g.xy[0], xm = xn;
    float yn = f->g.xy[1], ym = yn;
    for (int k = 1; k < f->g.pts_count; k++) {
        float x = f->g.xy[2 * k + 0];
        float y = f->g.xy[2 * k + 1];
        if (x < xn) xn = x; else if (x > xm) xm = x;
        if (y < yn) yn = y; else if (y > ym) ym = y;
    }
    f->bb_xmin = xn; f->bb_ymin = yn;
    f->bb_xmax = xm; f->bb_ymax = ym;
}

/* ---- Geometry command-stream decoder ------------------------------------ */

/*
 * Decode the MVT geometry command stream into feat_geom_t. The stream
 * is a packed uint32 array: each command is `(id & 7) | (count << 3)`,
 * followed by `count` sets of parameters (zigzag-encoded deltas).
 *
 * Commands:
 *   1 (MoveTo)    — count param-pairs: dx, dy
 *   2 (LineTo)    — count param-pairs: dx, dy
 *   7 (ClosePath) — no parameters
 *
 * We start a new ring on every MoveTo. A polygon's CCW outer ring vs
 * CW inner ring distinction is preserved (caller decides what to do
 * with it; the renderer uses NONZERO fill rule, so the winding is
 * what matters).
 */
static bool decode_geometry(feat_geom_t *g, const uint32_t *cmds, int n)
{
    int   xy_cap = 0;
    /* Accumulate in uint32_t so wraparound is defined behaviour for
     * pathological tiles whose summed deltas would overflow int32.
     * Cast back to int32 only at the float-conversion step. */
    uint32_t cx = 0, cy = 0;
    int   ring_start = 0;
    bool  have_pen = false;       /* false until first MoveTo */

    int i = 0;
    while (i < n) {
        uint32_t hdr = cmds[i++];
        int id = hdr & 7;
        /* hdr>>3 fits 29 bits (up to ~5.4e8); 2*cnt + i can overflow
         * a 32-bit int for pathological inputs. Compare against
         * remaining capacity instead so the arithmetic stays bounded. */
        uint32_t cnt = hdr >> 3;

        if (id == 1) {            /* MoveTo */
            if (cnt < 1) return false;
            if (cnt > (uint32_t)((n - i) / 2)) return false;
            for (uint32_t k = 0; k < cnt; k++) {
                cx += (uint32_t)zz_decode(cmds[i++]);
                cy += (uint32_t)zz_decode(cmds[i++]);
                if (k == 0) {
                    /* Start a new ring at this point. */
                    if (!g_push_ring(g, g->pts_count)) return false;
                    ring_start = g->pts_count;
                    have_pen = true;
                }
                if (!g_push_pt_capped(g, &xy_cap,
                        (float)(int32_t)cx, (float)(int32_t)cy))
                    return false;
            }
        } else if (id == 2) {     /* LineTo */
            if (cnt < 1) return false;
            if (!have_pen) return false;          /* spec: needs prior MoveTo */
            if (cnt > (uint32_t)((n - i) / 2)) return false;
            for (uint32_t k = 0; k < cnt; k++) {
                cx += (uint32_t)zz_decode(cmds[i++]);
                cy += (uint32_t)zz_decode(cmds[i++]);
                if (!g_push_pt_capped(g, &xy_cap,
                        (float)(int32_t)cx, (float)(int32_t)cy))
                    return false;
            }
        } else if (id == 7) {     /* ClosePath */
            if (cnt != 1) return false;           /* spec: count must be 1 */
            if (!have_pen) return false;
            /* Explicitly close by repeating the start point — helps
             * both fill and stroke renderers detect closed sub-rings. */
            if (g->pts_count > ring_start) {
                if (!g_push_pt_capped(g, &xy_cap,
                        g->xy[2 * ring_start + 0],
                        g->xy[2 * ring_start + 1])) return false;
            }
        } else {
            /* Unknown command — give up on this geometry. */
            return false;
        }
    }
    /* Sentinel ring offset for end-of-data. */
    if (!g_push_ring(g, g->pts_count)) return false;
    return true;
}

/* ---- Layer-name → kind map ---------------------------------------------- */

static lui_geomap_kind_t kind_from_layer(const char *name, int len)
{
    struct { const char *n; int len; lui_geomap_kind_t k; } table[] = {
        {"BldA",      4, LUI_GEOMAP_BUILDING},
        {"RdCL",      4, LUI_GEOMAP_ROAD},
        {"RailCL",    6, LUI_GEOMAP_RAIL},
        {"WA",        2, LUI_GEOMAP_WATER},
        {"RvrCL",     5, LUI_GEOMAP_WATER},
        {"SpcfArea",  8, LUI_GEOMAP_PARK},
        {"AdmArea",   7, LUI_GEOMAP_ADMIN},
        {"AdmBdry",   7, LUI_GEOMAP_ADMIN},
        {"Cntr",      4, LUI_GEOMAP_CONTOUR},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].len == len && memcmp(table[i].n, name, (size_t)len) == 0)
            return table[i].k;
    }
    return LUI_GEOMAP_OTHER;
}

/* ---- Layer / feature decode --------------------------------------------- */

/* MVT Value alternatives we care about. */
typedef struct {
    enum { VAL_NONE, VAL_INT, VAL_FLOAT, VAL_STRING } kind;
    union {
        int64_t i;
        float   f;
        struct { const char *p; int len; } s;
    } v;
} mvt_val_t;

static mvt_val_t decode_value(pb_t *vp)
{
    mvt_val_t out = {0};
    while (vp->p < vp->end && !vp->err) {
        uint64_t tag = pb_varint(vp);
        int wire = tag & 7;
        int fnum = (int)(tag >> 3);
        if (fnum == 1 && wire == 2) {        /* string_value */
            uint64_t ln = pb_varint(vp);
            if (vp->err || ln > (uint64_t)(vp->end - vp->p)) { vp->err = true; break; }
            out.kind = VAL_STRING;
            out.v.s.p   = (const char *)vp->p;
            out.v.s.len = (int)ln;
            vp->p += ln;
        } else if (fnum == 2 && wire == 5) { /* float_value */
            uint32_t bits = pb_fixed32(vp);
            float f; memcpy(&f, &bits, 4);
            out.kind = VAL_FLOAT; out.v.f = f;
        } else if (fnum == 3 && wire == 1) { /* double_value */
            uint64_t bits = pb_fixed64(vp);
            double d; memcpy(&d, &bits, 8);
            out.kind = VAL_FLOAT; out.v.f = (float)d;
        } else if (fnum == 4 && wire == 0) { /* int_value */
            uint64_t v = pb_varint(vp);
            out.kind = VAL_INT; out.v.i = (int64_t)v;
        } else if (fnum == 5 && wire == 0) { /* uint_value */
            uint64_t v = pb_varint(vp);
            out.kind = VAL_INT; out.v.i = (int64_t)v;
        } else if (fnum == 6 && wire == 0) { /* sint_value */
            uint64_t v = pb_varint(vp);
            out.kind = VAL_INT;
            out.v.i = (int64_t)((v >> 1) ^ (uint64_t)-(int64_t)(v & 1));
        } else if (fnum == 7 && wire == 0) { /* bool_value */
            uint64_t v = pb_varint(vp);
            out.kind = VAL_INT; out.v.i = (int64_t)v;
        } else {
            pb_skip(vp, wire);
        }
    }
    return out;
}

/* Truncate a float to int, clamping NaN/Inf and out-of-range values
 * to 0. Plain (int)f is UB whenever f isn't representable as int —
 * a hostile tile could trigger that with a crafted Value field. */
static inline int float_to_int_safe(float f)
{
    /* The bound is conservative: 2^31 isn't exactly representable as
     * float, so use a value strictly below INT_MAX that *is* exactly
     * representable. NaN comparisons evaluate false, so NaN takes the
     * `0` path naturally. */
    if (!(f >= -2147483520.0f && f <= 2147483520.0f)) return 0;
    return (int)f;
}

/* Apply a single (key,value) pair to a feature being built. */
static void apply_tag(feat_t *f, const char *key, int klen, const mvt_val_t *v)
{
    if (klen == 10 && memcmp(key, "vt_lvorder", 10) == 0) {
        if (v->kind == VAL_INT)   f->level_order = (int)v->v.i;
        if (v->kind == VAL_FLOAT) f->level_order = float_to_int_safe(v->v.f);
    } else if (klen == 8 && memcmp(key, "vt_width", 8) == 0) {
        if (v->kind == VAL_INT)   f->road_width = (float)v->v.i;
        if (v->kind == VAL_FLOAT) f->road_width = isfinite(v->v.f) ? v->v.f : 0.0f;
    } else if (klen == 8 && memcmp(key, "vt_rdctg", 8) == 0) {
        if (v->kind == VAL_INT)   f->road_category = (int)v->v.i;
        if (v->kind == VAL_FLOAT) f->road_category = float_to_int_safe(v->v.f);
    }
    /* Other tags ignored (we don't retain them). */
}

/* ---- Doc grow helper ---------------------------------------------------- */

static feat_t *doc_alloc_feat(lui_geomap_doc_t *doc)
{
    if (doc->n_feats >= doc->cap_feats) {
        int nc = doc->cap_feats ? doc->cap_feats * 2 : 64;
        feat_t *p = (feat_t *)realloc(doc->feats, (size_t)nc * sizeof(*p));
        if (!p) return NULL;
        doc->feats = p;
        doc->cap_feats = nc;
    }
    feat_t *f = &doc->feats[doc->n_feats];
    memset(f, 0, sizeof(*f));
    return f;
}

/* ---- Layer parser ------------------------------------------------------- */

typedef struct { const char *p; int len; } str_ref_t;

static lvg_result_t parse_layer(lui_geomap_doc_t *doc,
                                int tile_x, int tile_y,
                                pb_t *layer)
{
    /* Two-pass: gather keys + values + extent + name first, then walk
     * features (keys/values are referenced by index from features). */
    str_ref_t  *keys = NULL;
    int         keys_n = 0, keys_cap = 0;
    mvt_val_t  *vals = NULL;
    int         vals_n = 0, vals_cap = 0;
    pb_t       *feat_subs = NULL;   /* deferred sub-pbs of features    */
    int         feat_n = 0, feat_cap = 0;
    /* Per-feature scratch — declared up front so the `done:` cleanup
     * frees a defined pointer even when an early `goto done` fires
     * before the feature loop runs. (free(uninitialised) would be UB.) */
    uint32_t   *cmds = NULL;
    int         cmd_cap = 0;
    uint32_t   *tags = NULL;
    int         tag_cap = 0;
    int         extent = 4096;
    str_ref_t   layer_name = {0, 0};
    lvg_result_t rc = LVG_OK;

    while (layer->p < layer->end && !layer->err) {
        uint64_t tag = pb_varint(layer);
        int wire = tag & 7;
        int fnum = (int)(tag >> 3);
        if (fnum == 1 && wire == 2) {        /* name */
            uint64_t ln = pb_varint(layer);
            if (layer->err || ln > (uint64_t)(layer->end - layer->p)) {
                rc = LVG_ERR_INVALID; goto done;
            }
            layer_name.p = (const char *)layer->p;
            layer_name.len = (int)ln;
            layer->p += ln;
        } else if (fnum == 2 && wire == 2) { /* features (deferred)    */
            if (feat_n >= feat_cap) {
                int nc = feat_cap ? feat_cap * 2 : 16;
                pb_t *p = (pb_t *)realloc(feat_subs, (size_t)nc * sizeof(pb_t));
                if (!p) { rc = LVG_ERR_NOMEM; goto done; }
                feat_subs = p; feat_cap = nc;
            }
            if (!pb_sub(layer, &feat_subs[feat_n])) {
                rc = LVG_ERR_INVALID; goto done;
            }
            feat_n++;
        } else if (fnum == 3 && wire == 2) { /* keys                   */
            if (keys_n >= keys_cap) {
                int nc = keys_cap ? keys_cap * 2 : 16;
                str_ref_t *p = (str_ref_t *)realloc(keys,
                    (size_t)nc * sizeof(*p));
                if (!p) { rc = LVG_ERR_NOMEM; goto done; }
                keys = p; keys_cap = nc;
            }
            uint64_t ln = pb_varint(layer);
            if (layer->err || ln > (uint64_t)(layer->end - layer->p)) {
                rc = LVG_ERR_INVALID; goto done;
            }
            keys[keys_n].p   = (const char *)layer->p;
            keys[keys_n].len = (int)ln;
            keys_n++;
            layer->p += ln;
        } else if (fnum == 4 && wire == 2) { /* values                 */
            if (vals_n >= vals_cap) {
                int nc = vals_cap ? vals_cap * 2 : 16;
                mvt_val_t *p = (mvt_val_t *)realloc(vals,
                    (size_t)nc * sizeof(*p));
                if (!p) { rc = LVG_ERR_NOMEM; goto done; }
                vals = p; vals_cap = nc;
            }
            pb_t vsub;
            if (!pb_sub(layer, &vsub)) { rc = LVG_ERR_INVALID; goto done; }
            vals[vals_n++] = decode_value(&vsub);
        } else if (fnum == 5 && wire == 0) { /* extent                 */
            /* MVT extent is uint32; reject 0 (would collapse the tile
             * to a point) and absurd values that signal a malformed
             * or hostile tile. 65536 covers all real-world choices
             * (4096 is universal in practice). */
            uint64_t e = pb_varint(layer);
            if (e == 0 || e > 65536) { rc = LVG_ERR_INVALID; goto done; }
            extent = (int)e;
        } else if (fnum == 15 && wire == 0) { /* version              */
            (void)pb_varint(layer);
        } else {
            if (!pb_skip(layer, wire)) { rc = LVG_ERR_INVALID; goto done; }
        }
    }
    if (layer->err) { rc = LVG_ERR_INVALID; goto done; }

    /* Layer-extent invariant: doc.extent is set on first feature. */
    if (doc->extent == 0) doc->extent = extent;
    /* Honour the per-tile extent only if it matches doc-level; mixing
     * extents across tiles is not supported (would need per-feature
     * scaling). Real-world tiles use 4096 universally. */
    if (extent != doc->extent) {
        LUI_LOG_WARN("geomap_tiny: skipping layer with extent %d "
                     "(doc extent is %d)", extent, doc->extent);
        rc = LVG_OK;
        goto done;
    }

    lui_geomap_kind_t kind = kind_from_layer(layer_name.p, layer_name.len);

    /* (cmds/tags scratch was hoisted to function-top so the cleanup
     * label can free a defined pointer; capacity still persists across
     * features so we don't pay malloc/free per feature.) */

    /* Decode each feature now that the symbol tables are populated. */
    for (int i = 0; i < feat_n; i++) {
        pb_t fp = feat_subs[i];

        feat_t *fout = doc_alloc_feat(doc);
        if (!fout) { rc = LVG_ERR_NOMEM; goto done; }
        fout->tile_x = tile_x;
        fout->tile_y = tile_y;
        fout->kind   = kind;
        fout->type   = GEOM_LINESTRING; /* default                     */
        fout->road_width = 1.0f;

        int cmd_n = 0;
        int tag_n = 0;

        while (fp.p < fp.end && !fp.err) {
            uint64_t ftag = pb_varint(&fp);
            int wire = ftag & 7;
            int fnum = (int)(ftag >> 3);
            if (fnum == 1 && wire == 0) {           /* id */
                pb_varint(&fp);
            } else if (fnum == 2 && wire == 2) {    /* tags (packed)   */
                pb_t sub;
                if (!pb_sub(&fp, &sub)) { fp.err = true; break; }
                while (sub.p < sub.end && !sub.err) {
                    if (tag_n >= tag_cap) {
                        int nc = tag_cap ? tag_cap * 2 : 16;
                        uint32_t *p = (uint32_t *)realloc(tags,
                            (size_t)nc * sizeof(*p));
                        if (!p) { fp.err = true; break; }
                        tags = p; tag_cap = nc;
                    }
                    tags[tag_n++] = (uint32_t)pb_varint(&sub);
                }
                if (sub.err) fp.err = true;
            } else if (fnum == 3 && wire == 0) {    /* type            */
                fout->type = (geom_type_t)pb_varint(&fp);
            } else if (fnum == 4 && wire == 2) {    /* geometry        */
                pb_t sub;
                if (!pb_sub(&fp, &sub)) { fp.err = true; break; }
                while (sub.p < sub.end && !sub.err) {
                    if (cmd_n >= cmd_cap) {
                        int nc = cmd_cap ? cmd_cap * 2 : 32;
                        uint32_t *p = (uint32_t *)realloc(cmds,
                            (size_t)nc * sizeof(*p));
                        if (!p) { fp.err = true; break; }
                        cmds = p; cmd_cap = nc;
                    }
                    cmds[cmd_n++] = (uint32_t)pb_varint(&sub);
                }
                if (sub.err) fp.err = true;
            } else {
                if (!pb_skip(&fp, wire)) { fp.err = true; break; }
            }
        }

        if (!fp.err && cmd_n > 0) {
            if (decode_geometry(&fout->g, cmds, cmd_n)) {
                /* Apply tag pairs (key_idx, val_idx). */
                for (int t = 0; t + 1 < tag_n; t += 2) {
                    uint32_t ki = tags[t];
                    uint32_t vi = tags[t + 1];
                    if (ki < (uint32_t)keys_n && vi < (uint32_t)vals_n) {
                        apply_tag(fout, keys[ki].p, keys[ki].len, &vals[vi]);
                    }
                }
                compute_feat_bbox(fout);
                doc->n_feats++;
            } else {
                g_free(&fout->g);
            }
        } else {
            g_free(&fout->g);
        }
    }

done:
    free(cmds);
    free(tags);
    free(keys);
    free(vals);
    free(feat_subs);
    return rc;
}

/* ---- Top-level Tile parser ---------------------------------------------- */

lui_geomap_doc_t *lui_geomap_create(void)
{
    lui_geomap_doc_t *d = (lui_geomap_doc_t *)calloc(1, sizeof(*d));
    return d;
}

void lui_geomap_destroy(lui_geomap_doc_t *doc)
{
    if (!doc) return;
    for (int i = 0; i < doc->n_feats; i++) g_free(&doc->feats[i].g);
    free(doc->feats);
    for (int k = 0; k < LUI_GEOMAP_KIND_COUNT; k++)
        free(doc->buckets[k].idx);
    free(doc->scratch_f);
    free(doc->scratch_i);
    free(doc->sort_ptrs);
    free(doc);
}

static lvg_result_t ensure_buckets(lui_geomap_doc_t *doc)
{
    if (doc->buckets_valid) return LVG_OK;
    for (int k = 0; k < LUI_GEOMAP_KIND_COUNT; k++) doc->buckets[k].count = 0;
    for (int i = 0; i < doc->n_feats; i++) {
        int k = (int)doc->feats[i].kind;
        if (k < 0 || k >= LUI_GEOMAP_KIND_COUNT) k = 0;
        kind_bucket_t *b = &doc->buckets[k];
        if (!lui_grow_scratch((void **)&b->idx, &b->cap, b->count + 1,
                              sizeof(int)))
            return LVG_ERR_NOMEM;
        b->idx[b->count++] = i;
    }
    doc->buckets_valid = true;
    return LVG_OK;
}

/* Sniff the input for known-wrong formats so the caller gets an
 * actionable error message instead of a silent parse failure deep
 * in the protobuf walk. Returns LVG_ERR_INVALID with a logged
 * diagnostic if the input is gzip-compressed or a PMTiles archive
 * rather than a single MVT tile. */
static lvg_result_t reject_known_wrong_input(const uint8_t *b, size_t n)
{
    if (n < 2) return LVG_OK;  /* too small to sniff */

    /* Gzip RFC 1952: starts with 1F 8B. (Wire type 7 is reserved in
     * protobuf, so 1F as the first MVT byte is always malformed —
     * intercepting it here gives a useful message instead of a
     * generic parse failure.) */
    if (b[0] == 0x1F && b[1] == 0x8B) {
        LUI_LOG_WARN("geomap_tiny: input is gzip-compressed; "
                     "pre-decompress before passing "
                     "(e.g. curl --compressed)");
        return LVG_ERR_INVALID;
    }

    /* PMTiles archive: starts with the 7-byte ASCII string "PMTiles".
     * This parser handles individual MVT tiles; PMTiles unpacking
     * (header + directory + tile-by-offset) is out of scope. */
    static const char pm_magic[] = "PMTiles";
    enum { PM_MAGIC_LEN = sizeof(pm_magic) - 1 };
    if (n >= PM_MAGIC_LEN && memcmp(b, pm_magic, PM_MAGIC_LEN) == 0) {
        LUI_LOG_WARN("geomap_tiny: input is a PMTiles archive; "
                     "extract individual tiles before passing "
                     "(see https://github.com/protomaps/PMTiles)");
        return LVG_ERR_INVALID;
    }

    return LVG_OK;
}

lvg_result_t lui_geomap_add_tile(lui_geomap_doc_t *doc,
                                 int z, int x, int y,
                                 const void *pbf_data, size_t pbf_len)
{
    if (!doc || !pbf_data || pbf_len == 0) return LVG_ERR_INVALID;
    /* Cap tile size at 64 MiB. Real MVT tiles are <5 MiB; this leaves
     * headroom for unusual datasets while keeping every downstream
     * size and capacity calculation comfortably inside int32, so the
     * doubling-realloc paths in g_push_pt_capped / g_push_ring can't
     * be coerced into signed-overflow UB by an oversized tile. */
    if (pbf_len > (size_t)64 * 1024 * 1024) {
        LUI_LOG_WARN("geomap_tiny: tile too large (%zu bytes; max 64 MiB)",
                     pbf_len);
        return LVG_ERR_INVALID;
    }
    lvg_result_t sniff = reject_known_wrong_input(
        (const uint8_t *)pbf_data, pbf_len);
    if (sniff != LVG_OK) return sniff;

    /* Reject z-mismatch up front; the bounds update is deferred to
     * the success path so a failed parse doesn't leave the doc with
     * a permanently widened bbox. */
    if (doc->has_bounds && z != doc->z) {
        LUI_LOG_WARN("geomap_tiny: tile z=%d does not match doc z=%d",
                     z, doc->z);
        return LVG_ERR_INVALID;
    }

    /* Snapshot feature count so we only invalidate the per-kind index
     * if features actually got appended — a sniff/z/parse rejection
     * with zero features added leaves the cached index intact. */
    int n_feats_before = doc->n_feats;

    pb_t pb = { (const uint8_t *)pbf_data,
                (const uint8_t *)pbf_data + pbf_len, false };

    lvg_result_t parse_rc = LVG_OK;
    while (pb.p < pb.end && !pb.err) {
        uint64_t tag = pb_varint(&pb);
        int wire = tag & 7;
        int fnum = (int)(tag >> 3);
        if (fnum == 3 && wire == 2) {
            pb_t layer;
            if (!pb_sub(&pb, &layer)) { parse_rc = LVG_ERR_INVALID; break; }
            parse_rc = parse_layer(doc, x, y, &layer);
            if (parse_rc != LVG_OK) break;
        } else {
            if (!pb_skip(&pb, wire)) { parse_rc = LVG_ERR_INVALID; break; }
        }
    }
    if (parse_rc == LVG_OK && pb.err) parse_rc = LVG_ERR_INVALID;

    if (doc->n_feats != n_feats_before) doc->buckets_valid = false;
    if (parse_rc != LVG_OK) return parse_rc;

    /* Parse succeeded — commit bounds. */
    if (!doc->has_bounds) {
        doc->z = z;
        doc->x_min = doc->x_max = x;
        doc->y_min = doc->y_max = y;
        doc->has_bounds = true;
    } else {
        if (x < doc->x_min) doc->x_min = x;
        if (y < doc->y_min) doc->y_min = y;
        if (x > doc->x_max) doc->x_max = x;
        if (y > doc->y_max) doc->y_max = y;
    }
    return LVG_OK;
}

lvg_result_t lui_geomap_add_tile_file(lui_geomap_doc_t *doc,
                                      int z, int x, int y,
                                      const char *path)
{
    if (!path) return LVG_ERR_INVALID;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LUI_LOG_WARN("geomap_tiny: cannot open '%s'", path);
        return LVG_ERR_IO;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz <= 0) { fclose(fp); return LVG_ERR_INVALID; }
    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return LVG_ERR_NOMEM; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);

    lvg_result_t r = lui_geomap_add_tile(doc, z, x, y, buf, got);
    free(buf);
    return r;
}

lui_geomap_tile_bbox_t lui_geomap_get_tile_bbox(const lui_geomap_doc_t *doc)
{
    lui_geomap_tile_bbox_t bb = {0};
    if (doc && doc->has_bounds) {
        bb.z = doc->z;
        bb.x_min = doc->x_min; bb.y_min = doc->y_min;
        bb.x_max = doc->x_max; bb.y_max = doc->y_max;
        bb.extent = doc->extent ? doc->extent : 4096;
    }
    return bb;
}

int lui_geomap_feature_count(const lui_geomap_doc_t *doc)
{
    return doc ? doc->n_feats : 0;
}

/* ---- View defaults ------------------------------------------------------ */

void lui_geomap_view_init_default(lui_geomap_view_t *v)
{
    if (!v) return;
    v->tx = 0; v->ty = 0;
    v->scale = 1.0f;
    /* Cabinet projection: receding direction 30° below horizontal
     * (to upper-left), foreshortened 0.5×. Each world unit of height
     * shifts (-sin30°, -cos30°) * 0.5 = (-0.25, -0.433) in world
     * units, which becomes pixels after the per-vertex scale fold. */
    v->lift_x = -0.25f;
    v->lift_y = -0.43f;
    v->story_h = 80.0f;
}

/* ---- Render ------------------------------------------------------------- */

/*
 * world-to-screen for an extruded vertex at world (wx, wy, h):
 *     sx = (wx + h * lift_x) * scale + tx
 *     sy = (wy + h * lift_y) * scale + ty
 *
 * lift_x/lift_y describe a cabinet projection — height extrudes
 * along an oblique screen direction. Default is a 30° receding
 * direction with 0.5× foreshortening (lift_x=-0.25, lift_y=-0.43);
 * (0, -0.5) gives a one-axis "vertical pillar" view; (0, 0) collapses
 * to top-down 2D.
 */

/* Tile units → world units (just the tile offset; renderer adds local
 * coords inside). */
static float wx_of(const lui_geomap_doc_t *doc, const feat_t *f, float lx)
{
    return (float)(f->tile_x - doc->x_min) * (float)doc->extent + lx;
}
static float wy_of(const lui_geomap_doc_t *doc, const feat_t *f, float ly)
{
    return (float)(f->tile_y - doc->y_min) * (float)doc->extent + ly;
}

static float screen_x(const lui_geomap_view_t *v, float wx, float h)
{
    return (wx + h * v->lift_x) * v->scale + v->tx;
}
static float screen_y(const lui_geomap_view_t *v, float wy, float h)
{
    return (wy + h * v->lift_y) * v->scale + v->ty;
}

/* Heuristic building height when the data lacks a real height tag.
 * GSI's vt_lvorder is the rendering layer order, not building height,
 * so we synthesise. The hash MUST use a stable identifier — earlier
 * we used the painter-sort index, which made buildings shift heights
 * during camera moves. Hash on tile coords + base-vertex sum so a
 * given footprint gets a fixed floor count regardless of view. */
static float bld_floors(const feat_t *f)
{
    uint32_t h = (uint32_t)f->tile_x * 374761393u
               + (uint32_t)f->tile_y * 668265263u;
    if (f->g.pts_count > 0) {
        /* Fold a few base coords into the hash so two adjacent
         * buildings in the same tile get different heights. */
        h ^= (uint32_t)(f->g.xy[0] * 1000.0f) * 2246822519u;
        h ^= (uint32_t)(f->g.xy[1] * 1000.0f) * 3266489917u;
    }
    /* 80% of buildings: 1..3 floors. 20%: 4..6 floors. */
    if ((h & 7) >= 6) return 4.0f + (float)((h >> 3) & 3);
    return 1.0f + (float)((h >> 3) % 3);
}

/* Color lerp lives in <lightvg/types.h>;
 * grow scratch helper in <internal/lui_grow.h>. */

/* Fill a polygon ring in screen space (with optional height for
 * extrusion). Uses the doc-level integer-point scratch. */
static void fill_ring_screen(lui_geomap_doc_t *doc,
                             lvg_canvas_t *canvas,
                             const lui_geomap_view_t *view,
                             const feat_t *f,
                             const float *xy, int n,
                             float height, lvg_color_t color)
{
    if (n < 3) return;
    if (!lui_grow_scratch((void **)&doc->scratch_i, &doc->scratch_i_cap,
                      n, sizeof(lvg_point_t))) return;
    for (int k = 0; k < n; k++) {
        float wx = wx_of(doc, f, xy[2 * k + 0]);
        float wy = wy_of(doc, f, xy[2 * k + 1]);
        doc->scratch_i[k].x = (int)lroundf(screen_x(view, wx, height));
        doc->scratch_i[k].y = (int)lroundf(screen_y(view, wy, height));
    }
    lvg_canvas_fill_polygon_ex(canvas, doc->scratch_i, n, color,
                               LVG_FILL_RULE_NONZERO);
}

/* Stroke a polyline ring in screen space. */
static void stroke_ring_screen(lui_geomap_doc_t *doc,
                               lvg_canvas_t *canvas,
                               const lui_geomap_view_t *view,
                               const feat_t *f,
                               const float *xy, int n,
                               float height,
                               lvg_color_t color, float width,
                               bool closed)
{
    if (n < 2) return;
    if (!lui_grow_scratch((void **)&doc->scratch_f, &doc->scratch_f_cap,
                      n, sizeof(lvg_pointf_t))) return;
    for (int k = 0; k < n; k++) {
        float wx = wx_of(doc, f, xy[2 * k + 0]);
        float wy = wy_of(doc, f, xy[2 * k + 1]);
        doc->scratch_f[k].x = screen_x(view, wx, height);
        doc->scratch_f[k].y = screen_y(view, wy, height);
    }
    if (width < 0.5f) width = 0.5f;
    lvg_canvas_draw_styled_polyline(canvas, doc->scratch_f, n, color,
                                    width, closed,
                                    LVG_LINE_CAP_ROUND, LVG_LINE_JOIN_ROUND);
}

/* Render one extruded building: walls (each base edge → trapezoid)
 * then top face. */
static void render_building(lui_geomap_doc_t *doc,
                            lvg_canvas_t *canvas,
                            const lui_geomap_view_t *view,
                            const feat_t *f,
                            float height,
                            lvg_color_t top, lvg_color_t wall,
                            lvg_color_t edge)
{
    /* MVT polygon convention: ring 0 is the exterior, subsequent rings
     * are interior holes. lvg_canvas_fill_polygon_ex doesn't support
     * multi-contour fills, so we render only the outer ring. (The
     * GSI BldA layer rarely uses holes; the visible artifact when we
     * did render them was small "comma" shapes from inner rings being
     * drawn as solid overlays.) ring_count includes the end-sentinel
     * pushed at decode time, so a polygon with one real ring has
     * ring_count == 2. */
    if (f->g.ring_count < 2) return;
    int beg = f->g.ring_off[0];
    int end = f->g.ring_off[1];
    int n = end - beg;
    if (n < 3) return;

    /* Wall quads: each base edge becomes a four-vertex trapezoid
     * (bottom-i, bottom-i+1, top-i+1, top-i). */
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float ax = wx_of(doc, f, f->g.xy[2 * (beg + i) + 0]);
        float ay = wy_of(doc, f, f->g.xy[2 * (beg + i) + 1]);
        float bx = wx_of(doc, f, f->g.xy[2 * (beg + j) + 0]);
        float by = wy_of(doc, f, f->g.xy[2 * (beg + j) + 1]);

        lvg_point_t quad[4];
        quad[0].x = (int)lroundf(screen_x(view, ax, 0.0f));
        quad[0].y = (int)lroundf(screen_y(view, ay, 0.0f));
        quad[1].x = (int)lroundf(screen_x(view, bx, 0.0f));
        quad[1].y = (int)lroundf(screen_y(view, by, 0.0f));
        quad[2].x = (int)lroundf(screen_x(view, bx, height));
        quad[2].y = (int)lroundf(screen_y(view, by, height));
        quad[3].x = (int)lroundf(screen_x(view, ax, height));
        quad[3].y = (int)lroundf(screen_y(view, ay, height));

        /* Wall shading: outward normal (assuming CCW winding) dotted
         * with a fixed upper-left light, mapped to brightness. */
        float ex = bx - ax, ey = by - ay;
        float nx = -ey,    ny =  ex;
        float nlen = sqrtf(nx * nx + ny * ny);
        float lit = 0.5f;
        if (nlen > 1e-6f) {
            /* Tile y grows downward, so "upper" light is -y. */
            float ldx = -0.7071f, ldy = -0.7071f;
            float d = (nx * ldx + ny * ldy) / nlen;
            /* Lower bound 0.45 keeps shadowed walls readable rather
             * than sooty. */
            lit = 0.45f + 0.55f * (0.5f * (d + 1.0f));
        }
        lvg_color_t wcol = lvg_color_lerp(LVG_COLOR_BLACK, wall, lit);
        lvg_canvas_fill_polygon_ex(canvas, quad, 4, wcol,
                                   LVG_FILL_RULE_NONZERO);
    }

    fill_ring_screen(doc, canvas, view, f,
                     &f->g.xy[2 * beg], n, height, top);

    /* Top outline. */
    if (!lui_grow_scratch((void **)&doc->scratch_f, &doc->scratch_f_cap,
                      n, sizeof(lvg_pointf_t))) return;
    for (int k = 0; k < n; k++) {
        float wx = wx_of(doc, f, f->g.xy[2 * (beg + k) + 0]);
        float wy = wy_of(doc, f, f->g.xy[2 * (beg + k) + 1]);
        doc->scratch_f[k].x = screen_x(view, wx, height);
        doc->scratch_f[k].y = screen_y(view, wy, height);
    }
    lvg_canvas_draw_styled_polyline(canvas, doc->scratch_f, n, edge,
                                    0.75f, true,
                                    LVG_LINE_CAP_ROUND, LVG_LINE_JOIN_MITER);
}

/* Painter's-algorithm sort key: max base-y of the footprint in
 * screen space. Higher screen-y = closer to the viewer, so it draws
 * last. This is correct for non-overlapping footprints; with dense
 * blocks along the receding axis a tall building's top can leak
 * onto a shorter neighbour. Per-quad sort or a depth buffer would
 * fix it; that's deferred until the artifacts get noticeable. */
static float compute_sort_y(const lui_geomap_doc_t *doc,
                            const lui_geomap_view_t *view,
                            const feat_t *f)
{
    /* screen_y(wy, 0) = wy*scale + ty is monotonic in wy, so just
     * find max wy and project once. */
    float wy_max = -1e30f;
    for (int k = 0; k < f->g.pts_count; k++) {
        float wy = wy_of(doc, f, f->g.xy[2 * k + 1]);
        if (wy > wy_max) wy_max = wy;
    }
    return screen_y(view, wy_max, 0.0f);
}

static int cmp_feat_y(const void *A, const void *B)
{
    const feat_t *a = *(const feat_t * const *)A;
    const feat_t *b = *(const feat_t * const *)B;
    if (a->sort_y_max < b->sort_y_max) return -1;
    if (a->sort_y_max > b->sort_y_max) return  1;
    return 0;
}

/* Cheap world-space AABB intersection for a feature. The viewport
 * rect is in *world* coords (caller pre-inverts the screen viewport).
 * Builds carry an extra height-lift bound on top of the footprint. */
static bool feat_in_view(const lui_geomap_doc_t *doc, const feat_t *f,
                         float vx_min, float vy_min,
                         float vx_max, float vy_max)
{
    float wx_off = (float)(f->tile_x - doc->x_min) * (float)doc->extent;
    float wy_off = (float)(f->tile_y - doc->y_min) * (float)doc->extent;
    float fx_min = wx_off + f->bb_xmin;
    float fy_min = wy_off + f->bb_ymin;
    float fx_max = wx_off + f->bb_xmax;
    float fy_max = wy_off + f->bb_ymax;
    return !(fx_max < vx_min || fx_min > vx_max ||
             fy_max < vy_min || fy_min > vy_max);
}

lvg_result_t lui_geomap_render(const lui_geomap_doc_t *doc,
                               lvg_canvas_t *canvas,
                               const lui_geomap_view_t *view)
{
    if (!doc || !canvas || !view) return LVG_ERR_INVALID;
    /* Reject non-finite view fields up front. NaN <= 0 is false, so a
     * naive check would let NaN pour through every comparison and
     * produce degenerate output. */
    if (!isfinite(view->scale)  || view->scale <= 0)  return LVG_ERR_INVALID;
    if (!isfinite(view->tx)     || !isfinite(view->ty))   return LVG_ERR_INVALID;
    if (!isfinite(view->lift_x) || !isfinite(view->lift_y)) return LVG_ERR_INVALID;
    if (!isfinite(view->story_h))                      return LVG_ERR_INVALID;
    if (doc->n_feats == 0)        return LVG_OK;

    lui_geomap_doc_t *mdoc = (lui_geomap_doc_t *)doc;
    lvg_result_t bucket_rc = ensure_buckets(mdoc);
    if (bucket_rc != LVG_OK) return bucket_rc;

    /* Viewport AABB in world coords. Inverse of the base (z=0)
     * projection: world = (screen - t) / scale. We pad downward
     * (-y) by a generous building-height bound so extruded tops of
     * buildings whose footprints sit just below the viewport still
     * render. The 6-floor cap matches bld_floors's max. */
    lvg_surface_t *surf = lvg_canvas_get_surface(canvas);
    int sw = surf ? surf->width  : 0;
    int sh = surf ? surf->height : 0;
    float vx_min = -view->tx / view->scale;
    float vy_min = -view->ty / view->scale;
    float vx_max = ((float)sw - view->tx) / view->scale;
    float vy_max = ((float)sh - view->ty) / view->scale;
    /* Extend the world-rect by what a 6-story building's screen-y
     * lift inverts to. */
    float lift_pad_x = view->story_h * 6.0f * fabsf(view->lift_x);
    float lift_pad_y = view->story_h * 6.0f * fabsf(view->lift_y);
    vx_min -= lift_pad_x; vx_max += lift_pad_x;
    vy_min -= lift_pad_y; vy_max += lift_pad_y;

    const kind_bucket_t *b_water = &mdoc->buckets[LUI_GEOMAP_WATER];
    const kind_bucket_t *b_park  = &mdoc->buckets[LUI_GEOMAP_PARK];
    const kind_bucket_t *b_road  = &mdoc->buckets[LUI_GEOMAP_ROAD];
    const kind_bucket_t *b_rail  = &mdoc->buckets[LUI_GEOMAP_RAIL];
    const kind_bucket_t *b_bld   = &mdoc->buckets[LUI_GEOMAP_BUILDING];

    /* ---- Water polygons + river centerlines ---------------------- */
    for (int j = 0; j < b_water->count; j++) {
        const feat_t *f = &doc->feats[b_water->idx[j]];
        if (!feat_in_view(doc, f, vx_min, vy_min, vx_max, vy_max)) continue;
        for (int r = 0; r + 1 < f->g.ring_count; r++) {
            int beg = f->g.ring_off[r];
            int n   = f->g.ring_off[r + 1] - beg;
            if (f->type == GEOM_POLYGON) {
                fill_ring_screen(mdoc, canvas, view, f,
                                 &f->g.xy[2 * beg], n, 0.0f,
                                 LVG_COLOR_RGB(0xB6, 0xD3, 0xEA));
            } else if (f->type == GEOM_LINESTRING) {
                stroke_ring_screen(mdoc, canvas, view, f,
                                   &f->g.xy[2 * beg], n, 0.0f,
                                   LVG_COLOR_RGB(0x88, 0xB0, 0xD0),
                                   2.0f, false);
            }
        }
    }

    /* ---- Parks --------------------------------------------------- */
    for (int j = 0; j < b_park->count; j++) {
        const feat_t *f = &doc->feats[b_park->idx[j]];
        if (!feat_in_view(doc, f, vx_min, vy_min, vx_max, vy_max)) continue;
        for (int r = 0; r + 1 < f->g.ring_count; r++) {
            int beg = f->g.ring_off[r];
            int n   = f->g.ring_off[r + 1] - beg;
            fill_ring_screen(mdoc, canvas, view, f,
                             &f->g.xy[2 * beg], n, 0.0f,
                             LVG_COLOR_RGB(0xCB, 0xE3, 0xC5));
        }
    }

    /* ---- Roads --------------------------------------------------- */
    for (int j = 0; j < b_road->count; j++) {
        const feat_t *f = &doc->feats[b_road->idx[j]];
        /* Per-category boost: trunk roads (cat 1-2) wider than
         * residential (3-4) which are wider than footways (5-6).
         * Drop minor categories whose un-clamped width falls below
         * the legibility floor — culls footpath clutter at small
         * scales. */
        int cat = (f->road_category > 0 && f->road_category <= 6)
                  ? f->road_category : 4;
        float cat_boost = (7.0f - (float)cat) * 0.5f;
        float raw = (f->road_width > 0 ? f->road_width : 2.0f)
                    * view->scale * 0.0010f * cat_boost;
        if (cat >= 5 && raw < 1.5f) continue;
        if (!feat_in_view(doc, f, vx_min, vy_min, vx_max, vy_max)) continue;
        float w = raw;
        if (w < 1.5f)  w = 1.5f;
        if (w > 14.0f) w = 14.0f;
        for (int r = 0; r + 1 < f->g.ring_count; r++) {
            int beg = f->g.ring_off[r];
            int n   = f->g.ring_off[r + 1] - beg;
            /* Casing then center. */
            stroke_ring_screen(mdoc, canvas, view, f,
                               &f->g.xy[2 * beg], n, 0.0f,
                               LVG_COLOR_RGB(0x8E, 0x8E, 0x8E),
                               w + 1.5f, false);
            stroke_ring_screen(mdoc, canvas, view, f,
                               &f->g.xy[2 * beg], n, 0.0f,
                               LVG_COLOR_RGB(0xFD, 0xFD, 0xFD),
                               w, false);
        }
    }

    /* ---- Rails on top of roads ----------------------------------- */
    for (int j = 0; j < b_rail->count; j++) {
        const feat_t *f = &doc->feats[b_rail->idx[j]];
        if (!feat_in_view(doc, f, vx_min, vy_min, vx_max, vy_max)) continue;
        for (int r = 0; r + 1 < f->g.ring_count; r++) {
            int beg = f->g.ring_off[r];
            int n   = f->g.ring_off[r + 1] - beg;
            stroke_ring_screen(mdoc, canvas, view, f,
                               &f->g.xy[2 * beg], n, 0.0f,
                               LVG_COLOR_RGB(0x55, 0x55, 0x55),
                               2.5f, false);
        }
    }

    /* ---- Buildings, painter-sorted (visible subset only) ---------- */
    if (b_bld->count > 0) {
        if (!lui_grow_scratch((void **)&mdoc->sort_ptrs, &mdoc->sort_cap,
                              b_bld->count, sizeof(feat_t *)))
            return LVG_ERR_NOMEM;

        int n_visible = 0;
        for (int j = 0; j < b_bld->count; j++) {
            feat_t *f = &mdoc->feats[b_bld->idx[j]];
            if (!feat_in_view(doc, f, vx_min, vy_min, vx_max, vy_max))
                continue;
            f->sort_y_max = compute_sort_y(doc, view, f);
            mdoc->sort_ptrs[n_visible++] = f;
        }
        qsort(mdoc->sort_ptrs, (size_t)n_visible, sizeof(feat_t *),
              cmp_feat_y);

        for (int i = 0; i < n_visible; i++) {
            feat_t *f = mdoc->sort_ptrs[i];
            float floors = bld_floors(f);
            float h = floors * view->story_h;
            render_building(mdoc, canvas, view, f, h,
                            /* top  */ LVG_COLOR_RGB(0xEE, 0xE2, 0xC8),
                            /* wall */ LVG_COLOR_RGB(0xE0, 0xCB, 0xA8),
                            /* edge */ LVG_COLOR_RGB(0x70, 0x5A, 0x3A));
        }
    }

    return LVG_OK;
}
