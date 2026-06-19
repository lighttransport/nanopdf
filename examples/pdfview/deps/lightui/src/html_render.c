/*
 * html_render.c — HTML DOM tree → text layout renderer
 *
 * Walks a parsed lui_html_doc_t depth-first and emits styled spans into a
 * lui_text_layout_t.  Block-level decorations (blockquote bars, code block
 * backgrounds, horizontal rules, table borders) are recorded as decoration
 * entries for the widget layer to paint.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lightui/html.h"
#include "lightui/text_layout.h"
#include "lightui/types.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Dynamic-array push -------------------------------------------------- */

#define DA_PUSH(arr, count, cap, item) \
    do { \
        if ((count) >= (cap)) { \
            int new_cap = (cap) ? (cap) * 2 : 16; \
            void *p = realloc((arr), (size_t)new_cap * sizeof(*(arr))); \
            if (!p) break; \
            (arr) = p; (cap) = new_cap; \
        } \
        (arr)[(count)++] = (item); \
    } while (0)

/* ---- Style context carried through the walk ------------------------------ */

typedef struct {
    lvg_color_t fg;
    lvg_color_t bg;
    uint8_t     flags;         /* LUI_TEXT_* bitmask */
    int         indent;
    int         list_depth;
    int         list_counter;
    bool        in_pre;
} html_ctx_t;

/* ---- Render state passed around ------------------------------------------ */

typedef struct {
    lui_text_layout_t          *tl;
    const lui_html_style_t     *style;
    lui_html_render_state_t    *rs;
    lui_html_resolve_image_fn   resolve_image;
    void                       *resolve_user;
    html_ctx_t                  ctx;
    bool                        line_has_indent;
} html_render_t;

/* ---- Helpers ------------------------------------------------------------- */

static void emit_indent(html_render_t *r)
{
    if (r->line_has_indent)
        return;
    r->line_has_indent = true;

    int n = r->ctx.indent;
    if (n <= 0)
        return;

    static const char spaces[] =
        "                                                                ";
    while (n > 0) {
        int chunk = n;
        if (chunk > (int)(sizeof(spaces) - 1))
            chunk = (int)(sizeof(spaces) - 1);
        lui_text_layout_add_text(r->tl, spaces, chunk,
                                 r->ctx.fg, LVG_COLOR_TRANSPARENT,
                                 0, NULL);
        n -= chunk;
    }
}

static void emit_text(html_render_t *r, const char *utf8, int len)
{
    emit_indent(r);
    lui_text_layout_add_text(r->tl, utf8, len,
                             r->ctx.fg, r->ctx.bg,
                             r->ctx.flags, NULL);
}

static void emit_break(html_render_t *r)
{
    lui_text_layout_add_break(r->tl);
    r->line_has_indent = false;
}

/* ---- Inline style application -------------------------------------------- */

static void apply_inline_style(html_ctx_t *ctx, const lui_html_inline_style_t *s)
{
    if (s->color != LVG_COLOR_TRANSPARENT)
        ctx->fg = s->color;
    if (s->background_color != LVG_COLOR_TRANSPARENT)
        ctx->bg = s->background_color;
    if (s->font_weight == 2)
        ctx->flags |= LUI_TEXT_BOLD;
    if (s->font_style == 2)
        ctx->flags |= LUI_TEXT_ITALIC;
    if (s->text_decoration & 1)
        ctx->flags |= LUI_TEXT_UNDERLINE;
    if (s->text_decoration & 2)
        ctx->flags |= LUI_TEXT_STRIKETHROUGH;
}

/* ---- Node walkers -------------------------------------------------------- */

static void render_node(html_render_t *r, const lui_html_node_t *node);

static void render_children(html_render_t *r, const lui_html_node_t *node)
{
    for (const lui_html_node_t *c = node->first_child; c; c = c->next_sibling)
        render_node(r, c);
}

/* ---- Block elements ------------------------------------------------------ */

static void render_paragraph(html_render_t *r, const lui_html_node_t *node)
{
    render_children(r, node);
    emit_break(r);
    emit_break(r);
}

static void render_heading(html_render_t *r, const lui_html_node_t *node)
{
    html_ctx_t saved = r->ctx;

    r->ctx.fg = r->style->heading_color;
    r->ctx.flags |= LUI_TEXT_BOLD;

    render_children(r, node);

    r->ctx = saved;

    emit_break(r);
    emit_break(r);
}

static void render_blockquote(html_render_t *r, const lui_html_node_t *node)
{
    html_ctx_t saved = r->ctx;

    r->ctx.fg = r->style->blockquote_fg;

    int bar_chars = 2; /* "| " */

    /* Emit bar prefix for first line */
    emit_indent(r);
    lui_text_layout_add_text(r->tl, "\xe2\x94\x82 ", 4,  /* "| " in UTF-8 */
                             r->style->blockquote_bar_color,
                             LVG_COLOR_TRANSPARENT, 0, NULL);

    r->ctx.indent = saved.indent + bar_chars;

    for (const lui_html_node_t *c = node->first_child; c; c = c->next_sibling)
        render_node(r, c);

    r->ctx = saved;
}

static void render_pre(html_render_t *r, const lui_html_node_t *node)
{
    html_ctx_t saved = r->ctx;

    r->ctx.in_pre = true;
    r->ctx.fg = r->style->code_fg;
    r->ctx.bg = r->style->code_bg;
    r->ctx.flags |= LUI_TEXT_CODE;

    /* Walk children — TEXT nodes will preserve whitespace */
    render_children(r, node);

    r->ctx = saved;

    emit_break(r);
}

static void render_code(html_render_t *r, const lui_html_node_t *node)
{
    if (r->ctx.in_pre) {
        /* Already styled by PRE — just render children */
        render_children(r, node);
        return;
    }

    /* Inline code */
    html_ctx_t saved = r->ctx;

    r->ctx.fg = r->style->code_fg;
    r->ctx.bg = r->style->code_bg;
    r->ctx.flags |= LUI_TEXT_CODE;

    render_children(r, node);

    r->ctx = saved;
}

static void render_ul(html_render_t *r, const lui_html_node_t *node)
{
    html_ctx_t saved = r->ctx;

    r->ctx.list_depth++;
    r->ctx.indent += 2;

    for (const lui_html_node_t *item = node->first_child; item;
         item = item->next_sibling) {
        if (item->type != LUI_HTML_LI)
            continue;

        emit_indent(r);

        /* Bullet prefix "* " */
        lui_text_layout_add_text(r->tl, "\xe2\x80\xa2 ", 4,
                                 r->ctx.fg, LVG_COLOR_TRANSPARENT,
                                 0, NULL);
        r->line_has_indent = true;

        /* Apply LI inline style */
        html_ctx_t li_saved = r->ctx;
        apply_inline_style(&r->ctx, &item->style);

        render_children(r, item);

        r->ctx = li_saved;
        emit_break(r);
    }

    r->ctx = saved;
    emit_break(r);
}

static void render_ol(html_render_t *r, const lui_html_node_t *node)
{
    html_ctx_t saved = r->ctx;

    r->ctx.list_depth++;
    r->ctx.indent += 2;

    int counter = node->ol_start > 0 ? node->ol_start : 1;

    for (const lui_html_node_t *item = node->first_child; item;
         item = item->next_sibling) {
        if (item->type != LUI_HTML_LI)
            continue;

        emit_indent(r);

        /* Numbered prefix "1. " */
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%d. ", counter);
        lui_text_layout_add_text(r->tl, buf, n,
                                 r->ctx.fg, LVG_COLOR_TRANSPARENT,
                                 0, NULL);
        counter++;
        r->line_has_indent = true;

        /* Apply LI inline style */
        html_ctx_t li_saved = r->ctx;
        apply_inline_style(&r->ctx, &item->style);

        render_children(r, item);

        r->ctx = li_saved;
        emit_break(r);
    }

    r->ctx = saved;
    emit_break(r);
}

static void render_hr(html_render_t *r)
{
    static const char hr_text[] =
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80";
    /* 24 x "─" (U+2500), each 3 bytes = 72 bytes */

    emit_indent(r);
    lui_text_layout_add_text(r->tl, hr_text, (int)sizeof(hr_text) - 1,
                             r->style->hr_color, LVG_COLOR_TRANSPARENT,
                             0, NULL);
    emit_break(r);
    emit_break(r);
}

/* ---- Table --------------------------------------------------------------- */

static void render_table(html_render_t *r, const lui_html_node_t *node)
{
    /* Walk TR children */
    for (const lui_html_node_t *tr = node->first_child; tr;
         tr = tr->next_sibling) {
        if (tr->type != LUI_HTML_TR)
            continue;

        /* Walk TD/TH children in this row */
        for (const lui_html_node_t *cell = tr->first_child; cell;
             cell = cell->next_sibling) {
            if (cell->type != LUI_HTML_TD && cell->type != LUI_HTML_TH)
                continue;

            emit_indent(r);

            /* Cell prefix */
            lui_text_layout_add_text(r->tl, "| ", 2,
                                     r->style->table_border_color,
                                     LVG_COLOR_TRANSPARENT, 0, NULL);

            html_ctx_t saved = r->ctx;

            /* Apply cell inline style */
            apply_inline_style(&r->ctx, &cell->style);

            if (cell->type == LUI_HTML_TH) {
                r->ctx.flags |= LUI_TEXT_BOLD;
            }

            render_children(r, cell);

            r->ctx = saved;

            /* Trailing space between cells */
            lui_text_layout_add_text(r->tl, " ", 1,
                                     r->ctx.fg, LVG_COLOR_TRANSPARENT,
                                     0, NULL);
        }

        emit_break(r);
    }

    emit_break(r);
}

/* ---- Inline elements ----------------------------------------------------- */

static void render_bold(html_render_t *r, const lui_html_node_t *node)
{
    html_ctx_t saved = r->ctx;
    r->ctx.flags |= LUI_TEXT_BOLD;
    render_children(r, node);
    r->ctx = saved;
}

static void render_italic(html_render_t *r, const lui_html_node_t *node)
{
    html_ctx_t saved = r->ctx;
    r->ctx.flags |= LUI_TEXT_ITALIC;
    render_children(r, node);
    r->ctx = saved;
}

static void render_underline(html_render_t *r, const lui_html_node_t *node)
{
    html_ctx_t saved = r->ctx;
    r->ctx.flags |= LUI_TEXT_UNDERLINE;
    render_children(r, node);
    r->ctx = saved;
}

static void render_strikethrough(html_render_t *r, const lui_html_node_t *node)
{
    html_ctx_t saved = r->ctx;
    r->ctx.flags |= LUI_TEXT_STRIKETHROUGH;
    render_children(r, node);
    r->ctx = saved;
}

static void render_link(html_render_t *r, const lui_html_node_t *node)
{
    html_ctx_t saved = r->ctx;
    r->ctx.fg = r->style->link_color;
    r->ctx.flags |= LUI_TEXT_UNDERLINE;
    render_children(r, node);
    r->ctx = saved;
}

static void render_image(html_render_t *r, const lui_html_node_t *node)
{
    const lvg_surface_t *surf = NULL;

    if (r->resolve_image && node->src_url && node->src_url_len > 0) {
        surf = r->resolve_image(node->src_url, node->src_url_len,
                                r->resolve_user);
    }

    if (surf) {
        emit_indent(r);
        lui_text_layout_add_image(r->tl, surf, 0, 0);
    } else {
        /* Fallback: emit alt text or "[img]" */
        emit_indent(r);
        if (node->alt && node->alt_len > 0) {
            lui_text_layout_add_text(r->tl, "[", 1,
                                     r->ctx.fg, LVG_COLOR_TRANSPARENT,
                                     r->ctx.flags, NULL);
            lui_text_layout_add_text(r->tl, node->alt, node->alt_len,
                                     r->ctx.fg, LVG_COLOR_TRANSPARENT,
                                     r->ctx.flags, NULL);
            lui_text_layout_add_text(r->tl, "]", 1,
                                     r->ctx.fg, LVG_COLOR_TRANSPARENT,
                                     r->ctx.flags, NULL);
        } else {
            lui_text_layout_add_text(r->tl, "[img]", 5,
                                     r->ctx.fg, LVG_COLOR_TRANSPARENT,
                                     r->ctx.flags, NULL);
        }
    }
}

static void render_text(html_render_t *r, const lui_html_node_t *node)
{
    if (!node->text || node->text_len <= 0)
        return;

    if (r->ctx.in_pre) {
        /* Preserve whitespace: emit line by line */
        const char *p = node->text;
        const char *end = p + node->text_len;

        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            int line_len = nl ? (int)(nl - p) : (int)(end - p);

            if (line_len > 0) {
                emit_indent(r);
                lui_text_layout_add_text(r->tl, p, line_len,
                                         r->ctx.fg, r->ctx.bg,
                                         r->ctx.flags, NULL);
            }

            if (nl) {
                emit_break(r);
                p = nl + 1;
            } else {
                p = end;
            }
        }
    } else {
        /* Normal text — whitespace already collapsed by parser */
        emit_text(r, node->text, node->text_len);
    }
}

/* ---- Main dispatcher ----------------------------------------------------- */

static void render_node(html_render_t *r, const lui_html_node_t *node)
{
    if (!node)
        return;

    /* Save context and apply inline style from style="" attribute */
    html_ctx_t style_saved = r->ctx;
    apply_inline_style(&r->ctx, &node->style);

    switch (node->type) {
    case LUI_HTML_DOCUMENT:
        render_children(r, node);
        break;

    /* Block elements */
    case LUI_HTML_P:
    case LUI_HTML_DIV:
        render_paragraph(r, node);
        break;

    case LUI_HTML_H1:
    case LUI_HTML_H2:
    case LUI_HTML_H3:
    case LUI_HTML_H4:
    case LUI_HTML_H5:
    case LUI_HTML_H6:
        render_heading(r, node);
        break;

    case LUI_HTML_BLOCKQUOTE:
        render_blockquote(r, node);
        break;

    case LUI_HTML_PRE:
        render_pre(r, node);
        break;

    case LUI_HTML_CODE:
        render_code(r, node);
        break;

    case LUI_HTML_UL:
        render_ul(r, node);
        break;

    case LUI_HTML_OL:
        render_ol(r, node);
        break;

    case LUI_HTML_LI:
        /* Handled by render_ul / render_ol directly */
        render_children(r, node);
        break;

    case LUI_HTML_HR:
        render_hr(r);
        break;

    case LUI_HTML_BR:
        emit_break(r);
        break;

    case LUI_HTML_TABLE:
        render_table(r, node);
        break;

    case LUI_HTML_TR:
        /* Handled by render_table */
        render_children(r, node);
        break;

    case LUI_HTML_TD:
        /* Handled by render_table */
        render_children(r, node);
        break;

    case LUI_HTML_TH:
        /* Handled by render_table */
        render_children(r, node);
        break;

    /* Inline elements */
    case LUI_HTML_B:
        render_bold(r, node);
        break;

    case LUI_HTML_I:
        render_italic(r, node);
        break;

    case LUI_HTML_U:
        render_underline(r, node);
        break;

    case LUI_HTML_S:
        render_strikethrough(r, node);
        break;

    case LUI_HTML_A:
        render_link(r, node);
        break;

    case LUI_HTML_IMG:
        render_image(r, node);
        break;

    case LUI_HTML_SPAN:
        /* Inline style already applied above; just render children */
        render_children(r, node);
        break;

    case LUI_HTML_SUB:
    case LUI_HTML_SUP:
        /* No baseline shift in v1 — render children inline */
        render_children(r, node);
        break;

    case LUI_HTML_TEXT:
        render_text(r, node);
        break;
    }

    /* Restore context (undo inline style application) */
    r->ctx = style_saved;
}

/* ---- Public API ---------------------------------------------------------- */

void lui_html_render(lui_text_layout_t *tl,
                     const lui_html_doc_t *doc,
                     const lui_html_style_t *style,
                     lui_html_render_state_t *rs,
                     lui_html_resolve_image_fn resolve_image,
                     void *resolve_user)
{
    if (!tl || !doc || !doc->root)
        return;

    /* Use default style if none provided. */
    lui_html_style_t default_style;
    if (!style) {
        lui_html_style_default(&default_style);
        style = &default_style;
    }

    /* Clear previous content. */
    lui_text_layout_clear(tl);
    if (rs) {
        rs->deco_count = 0;  /* keep allocation, reset count */
    }

    /* Set up render context. */
    html_render_t r;
    memset(&r, 0, sizeof(r));
    r.tl            = tl;
    r.style         = style;
    r.rs            = rs;
    r.resolve_image = resolve_image;
    r.resolve_user  = resolve_user;

    r.ctx.fg           = style->text_color;
    r.ctx.bg           = LVG_COLOR_TRANSPARENT;
    r.ctx.flags        = 0;
    r.ctx.indent       = 0;
    r.ctx.list_depth   = 0;
    r.ctx.list_counter = 0;
    r.ctx.in_pre       = false;

    r.line_has_indent = false;

    /* Walk the DOM tree. */
    render_node(&r, doc->root);
}

void lui_html_render_state_destroy(lui_html_render_state_t *rs)
{
    if (!rs)
        return;
    free(rs->decos);
    rs->decos      = NULL;
    rs->deco_count = 0;
    rs->deco_cap   = 0;
}

/* lui_html_style_default() is defined in html.c */
