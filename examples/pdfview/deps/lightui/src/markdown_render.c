/*
 * markdown_render.c — Markdown AST → text layout renderer
 *
 * Walks a parsed lui_md_doc_t depth-first and emits styled spans into a
 * lui_text_layout_t.  Block-level decorations (blockquote bars, code block
 * backgrounds, horizontal rules) are rendered as text spans so that the
 * existing text_layout engine handles all positioning.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lightui/markdown.h"
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
    uint8_t     flags;       /* LUI_TEXT_* bitmask */
    int         indent;      /* current left indent in chars */
    int         list_depth;
    int         list_counter;/* for ordered lists */
    bool        in_blockquote;
} md_ctx_t;

/* ---- Render state passed around ------------------------------------------ */

typedef struct {
    lui_text_layout_t       *tl;
    const lui_md_style_t    *style;
    lui_md_render_state_t   *rs;
    lui_md_resolve_image_fn  resolve_image;
    void                    *resolve_user;
    md_ctx_t                 ctx;
    bool                     line_has_indent; /* was indent emitted for current line? */
} md_render_t;

/* ---- Helpers ------------------------------------------------------------- */

static void emit_indent(md_render_t *r)
{
    if (r->line_has_indent)
        return;
    r->line_has_indent = true;

    int n = r->ctx.indent;
    if (n <= 0)
        return;

    /* Build a buffer of spaces for the indent. */
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

static void emit_text(md_render_t *r, const char *utf8, int len)
{
    emit_indent(r);
    lui_text_layout_add_text(r->tl, utf8, len,
                             r->ctx.fg, r->ctx.bg,
                             r->ctx.flags, NULL);
}

static void emit_break(md_render_t *r)
{
    lui_text_layout_add_break(r->tl);
    r->line_has_indent = false;  /* next line needs indent */
}


/* ---- Node walkers -------------------------------------------------------- */

static void render_node(md_render_t *r, const lui_md_node_t *node);

static void render_children(md_render_t *r, const lui_md_node_t *node)
{
    for (const lui_md_node_t *c = node->first_child; c; c = c->next_sibling)
        render_node(r, c);
}

static void render_paragraph(md_render_t *r, const lui_md_node_t *node)
{
    render_children(r, node);
    /* Paragraph spacing: two breaks (one for the line end, one for spacing). */
    emit_break(r);
    emit_break(r);
}

static void render_heading(md_render_t *r, const lui_md_node_t *node)
{
    md_ctx_t saved = r->ctx;

    r->ctx.fg = r->style->heading_color;
    r->ctx.flags |= LUI_TEXT_BOLD;

    /* Heading prefix (optional — emit "## " etc. for clarity) */
    render_children(r, node);

    r->ctx = saved;

    emit_break(r);
    emit_break(r);
}

static void render_blockquote(md_render_t *r, const lui_md_node_t *node)
{
    md_ctx_t saved = r->ctx;

    r->ctx.in_blockquote = true;
    r->ctx.fg = r->style->blockquote_fg;

    /* Increase indent and emit "│ " as visual bar at the start of each line.
       We add 2 chars for the bar ("│ ") plus the configured indent. */
    int bar_chars = 2; /* "│ " takes 2 character cells */
    r->ctx.indent += bar_chars;

    /* We override the indent emission to include the bar character.
       For simplicity, we'll emit the bar as part of the first content on
       each line by hooking into emit_indent.  Since our indent system uses
       plain spaces, we'll prepend the bar before children on each new line.

       Simplified approach: emit the bar + spaces as a text prefix before
       walking children, and after each break inside the blockquote. */

    /* Walk children.  Before each line's content, the indent mechanism will
       emit spaces.  We replace the first 2 spaces with "│ " by adjusting
       the indent and emitting the bar directly. */
    r->ctx.indent = saved.indent;  /* reset — we'll manage manually */

    /* Emit bar prefix for first line */
    emit_indent(r);
    lui_text_layout_add_text(r->tl, "\xe2\x94\x82 ", 4,  /* "│ " in UTF-8 */
                             r->style->blockquote_bar_color,
                             LVG_COLOR_TRANSPARENT, 0, NULL);

    /* Increase indent for nested content after the bar */
    r->ctx.indent = saved.indent + bar_chars;

    /* We need to intercept breaks to re-emit the bar prefix.
       Since we can't easily hook into the break mechanism, we handle this
       by walking children manually and inserting bar prefixes. */
    for (const lui_md_node_t *c = node->first_child; c; c = c->next_sibling) {
        render_node(r, c);
    }

    r->ctx = saved;
}

static void render_code_block(md_render_t *r, const lui_md_node_t *node)
{
    md_ctx_t saved = r->ctx;

    r->ctx.fg = r->style->code_fg;
    r->ctx.bg = r->style->code_bg;
    r->ctx.flags |= LUI_TEXT_CODE;

    if (node->text && node->text_len > 0) {
        /* Emit code block text line by line, with indent on each line. */
        const char *p = node->text;
        const char *end = p + node->text_len;

        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            int line_len = nl ? (int)(nl - p) : (int)(end - p);

            emit_indent(r);
            /* Emit a small padding prefix for visual separation */
            lui_text_layout_add_text(r->tl, "  ", 2,
                                     r->ctx.fg, r->ctx.bg,
                                     r->ctx.flags, NULL);
            if (line_len > 0) {
                lui_text_layout_add_text(r->tl, p, line_len,
                                         r->ctx.fg, r->ctx.bg,
                                         r->ctx.flags, NULL);
            }
            emit_break(r);

            p = nl ? nl + 1 : end;
        }
    }

    r->ctx = saved;

    emit_break(r);
}

static void render_list(md_render_t *r, const lui_md_node_t *node)
{
    md_ctx_t saved = r->ctx;

    r->ctx.list_depth++;
    r->ctx.indent += 2;  /* per-level indent */

    int counter = node->list_start > 0 ? node->list_start : 1;

    for (const lui_md_node_t *item = node->first_child; item;
         item = item->next_sibling) {
        if (item->type != LUI_MD_LIST_ITEM)
            continue;

        emit_indent(r);

        /* Emit bullet or number prefix */
        if (node->list_ordered) {
            char buf[16];
            int n = snprintf(buf, sizeof(buf), "%d. ", counter);
            lui_text_layout_add_text(r->tl, buf, n,
                                     r->ctx.fg, LVG_COLOR_TRANSPARENT,
                                     0, NULL);
            counter++;
        } else {
            /* UTF-8 bullet "• " */
            lui_text_layout_add_text(r->tl, "\xe2\x80\xa2 ", 4,
                                     r->ctx.fg, LVG_COLOR_TRANSPARENT,
                                     0, NULL);
        }

        /* Mark indent as already emitted for this first line */
        r->line_has_indent = true;

        /* Walk item children */
        for (const lui_md_node_t *c = item->first_child; c;
             c = c->next_sibling) {
            /* For paragraphs inside list items, just walk their inline
               content without double-spacing. */
            if (c->type == LUI_MD_PARAGRAPH) {
                render_children(r, c);
                emit_break(r);
            } else {
                render_node(r, c);
            }
        }
    }

    r->ctx = saved;

    emit_break(r);
}

static void render_thematic_break(md_render_t *r)
{
    static const char hr_text[] =
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80";
    /* 24 × "─" (U+2500), each 3 bytes = 72 bytes */

    emit_indent(r);
    lui_text_layout_add_text(r->tl, hr_text, (int)sizeof(hr_text) - 1,
                             r->style->hr_color, LVG_COLOR_TRANSPARENT,
                             0, NULL);
    emit_break(r);
    emit_break(r);
}

static void render_text(md_render_t *r, const lui_md_node_t *node)
{
    if (node->text && node->text_len > 0) {
        emit_text(r, node->text, node->text_len);
    }
}

static void render_emphasis(md_render_t *r, const lui_md_node_t *node)
{
    md_ctx_t saved = r->ctx;
    r->ctx.flags |= LUI_TEXT_ITALIC;
    render_children(r, node);
    r->ctx = saved;
}

static void render_strong(md_render_t *r, const lui_md_node_t *node)
{
    md_ctx_t saved = r->ctx;
    r->ctx.flags |= LUI_TEXT_BOLD;
    render_children(r, node);
    r->ctx = saved;
}

static void render_code_span(md_render_t *r, const lui_md_node_t *node)
{
    if (node->text && node->text_len > 0) {
        emit_indent(r);
        lui_text_layout_add_text(r->tl, node->text, node->text_len,
                                 r->style->code_fg, r->style->code_bg,
                                 r->ctx.flags | LUI_TEXT_CODE, NULL);
    }
}

static void render_link(md_render_t *r, const lui_md_node_t *node)
{
    md_ctx_t saved = r->ctx;
    r->ctx.fg = r->style->link_color;
    r->ctx.flags |= LUI_TEXT_UNDERLINE;
    render_children(r, node);
    r->ctx = saved;
}

static void render_image(md_render_t *r, const lui_md_node_t *node)
{
    const lvg_surface_t *surf = NULL;

    if (r->resolve_image && node->url && node->url_len > 0) {
        surf = r->resolve_image(node->url, node->url_len, r->resolve_user);
    }

    if (surf) {
        emit_indent(r);
        lui_text_layout_add_image(r->tl, surf, 0, 0);
    } else {
        /* Fallback: emit alt text or placeholder */
        emit_indent(r);
        if (node->first_child && node->first_child->text &&
            node->first_child->text_len > 0) {
            lui_text_layout_add_text(r->tl, "[", 1,
                                     r->ctx.fg, LVG_COLOR_TRANSPARENT,
                                     r->ctx.flags, NULL);
            lui_text_layout_add_text(r->tl,
                                     node->first_child->text,
                                     node->first_child->text_len,
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

static void render_softbreak(md_render_t *r)
{
    emit_text(r, " ", 1);
}

static void render_hardbreak(md_render_t *r)
{
    emit_break(r);
}

/* ---- Main dispatcher ----------------------------------------------------- */

static void render_node(md_render_t *r, const lui_md_node_t *node)
{
    if (!node)
        return;

    switch (node->type) {
    case LUI_MD_DOCUMENT:
        render_children(r, node);
        break;
    case LUI_MD_PARAGRAPH:
        render_paragraph(r, node);
        break;
    case LUI_MD_HEADING:
        render_heading(r, node);
        break;
    case LUI_MD_BLOCKQUOTE:
        render_blockquote(r, node);
        break;
    case LUI_MD_CODE_BLOCK:
        render_code_block(r, node);
        break;
    case LUI_MD_LIST:
        render_list(r, node);
        break;
    case LUI_MD_LIST_ITEM:
        /* Handled directly by render_list */
        render_children(r, node);
        break;
    case LUI_MD_THEMATIC_BREAK:
        render_thematic_break(r);
        break;
    case LUI_MD_TEXT:
        render_text(r, node);
        break;
    case LUI_MD_EMPHASIS:
        render_emphasis(r, node);
        break;
    case LUI_MD_STRONG:
        render_strong(r, node);
        break;
    case LUI_MD_CODE_SPAN:
        render_code_span(r, node);
        break;
    case LUI_MD_LINK:
        render_link(r, node);
        break;
    case LUI_MD_IMAGE:
        render_image(r, node);
        break;
    case LUI_MD_SOFTBREAK:
        render_softbreak(r);
        break;
    case LUI_MD_HARDBREAK:
        render_hardbreak(r);
        break;
    }
}

/* ---- Public API ---------------------------------------------------------- */

void lui_md_render(lui_text_layout_t *tl,
                   const lui_md_doc_t *doc,
                   const lui_md_style_t *style,
                   lui_md_render_state_t *rs,
                   lui_md_resolve_image_fn resolve_image,
                   void *resolve_user)
{
    if (!tl || !doc || !doc->root)
        return;

    /* Use default style if none provided. */
    lui_md_style_t default_style;
    if (!style) {
        lui_md_style_default(&default_style);
        style = &default_style;
    }

    /* Clear previous content. */
    lui_text_layout_clear(tl);
    if (rs) {
        rs->deco_count = 0;  /* keep allocation, reset count */
    }

    /* Set up render context. */
    md_render_t r;
    memset(&r, 0, sizeof(r));
    r.tl            = tl;
    r.style         = style;
    r.rs            = rs;
    r.resolve_image = resolve_image;
    r.resolve_user  = resolve_user;

    r.ctx.fg            = style->text_color;
    r.ctx.bg            = LVG_COLOR_TRANSPARENT;
    r.ctx.flags         = 0;
    r.ctx.indent        = 0;
    r.ctx.list_depth    = 0;
    r.ctx.list_counter  = 0;
    r.ctx.in_blockquote = false;

    r.line_has_indent = false;

    /* Walk the AST. */
    render_node(&r, doc->root);
}

void lui_md_render_state_destroy(lui_md_render_state_t *rs)
{
    if (!rs)
        return;
    free(rs->decos);
    rs->decos      = NULL;
    rs->deco_count = 0;
    rs->deco_cap   = 0;
}

/* lui_md_style_default() is defined in markdown.c */
