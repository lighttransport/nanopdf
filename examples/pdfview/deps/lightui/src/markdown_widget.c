/*
 * markdown_widget.c — Markdown display widget
 *
 * Owns a parsed markdown document, renders it via lui_text_layout_t,
 * and handles scrolling.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <lightui/markdown.h>
#include <lightvg/canvas.h>
#include <lightui/event.h>

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Rebuild layout from the parsed document if dirty. */
static void md_ensure_layout(lui_markdown_t *md, int width)
{
    if (!md->layout_dirty) return;

    md->layout.max_width = width;
    lui_md_render(&md->layout, &md->doc, &md->style, &md->render_state,
                  md->resolve_image, md->resolve_image_user);
    lui_text_layout_build(&md->layout);
    md->content_height = md->layout.total_height;
    md->layout_dirty = false;
}

/* Clamp scroll_y to valid range. */
static void md_clamp_scroll(lui_markdown_t *md, int view_h)
{
    int max_scroll = md->content_height - view_h;
    if (max_scroll < 0) max_scroll = 0;
    if (md->scroll_y < 0) md->scroll_y = 0;
    if (md->scroll_y > max_scroll) md->scroll_y = max_scroll;
}

/* -------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------- */

static int md_measure(const lui_widget_t *w, int *out_w, int *out_h,
                      void *user)
{
    (void)user;
    lui_markdown_t *md = (lui_markdown_t *)w;

    /* Rebuild layout at current max_width so we know the content size. */
    int width = md->layout.max_width > 0 ? md->layout.max_width : 300;
    md_ensure_layout(md, width);

    *out_w = md->layout.total_width;
    *out_h = md->layout.total_height;
    return 0;
}

static void md_draw(lui_widget_t *w, lvg_canvas_t *canvas)
{
    lui_markdown_t *md = (lui_markdown_t *)w;
    lvg_rect_t r = lui_widget_absolute_rect(w);
    if (lvg_rect_is_empty(&r)) return;

    /* Background */
    lvg_canvas_fill_rect(canvas, r.x, r.y, r.width, r.height, md->bg);

    /* Clip to widget bounds */
    lvg_rect_t old_clip = canvas->_clip;
    lvg_rect_t clip = lvg_rect_intersect(&old_clip, &r);
    canvas->_clip = clip;

    /* Rebuild layout if dirty (use widget width for wrapping) */
    md_ensure_layout(md, r.width);

    /* Clamp scroll */
    md_clamp_scroll(md, r.height);

    /* Draw block decorations (blockquote bars, code block bgs, HRs) */
    for (int i = 0; i < md->render_state.deco_count; i++) {
        const lui_md_deco_t *deco = &md->render_state.decos[i];

        /* Compute top/bottom from line indices */
        int deco_top = 0;
        int deco_bottom = 0;
        if (deco->line_start < md->layout.line_count)
            deco_top = md->layout.lines[deco->line_start].y;
        if (deco->line_end > 0 && deco->line_end <= md->layout.line_count) {
            const lui_line_t *last = &md->layout.lines[deco->line_end - 1];
            deco_bottom = last->y + last->height;
        }

        int dy = r.y - md->scroll_y;

        switch (deco->type) {
        case LUI_MD_DECO_CODE_BLOCK_BG:
            lvg_canvas_fill_rect(canvas,
                                 r.x, deco_top + dy,
                                 r.width, deco_bottom - deco_top,
                                 deco->color);
            break;
        case LUI_MD_DECO_BLOCKQUOTE_BAR:
            lvg_canvas_fill_rect(canvas,
                                 r.x + deco->indent, deco_top + dy,
                                 3, deco_bottom - deco_top,
                                 deco->color);
            break;
        case LUI_MD_DECO_HR:
            {
                int hr_y = (deco_top + deco_bottom) / 2 + dy;
                lvg_canvas_fill_rect(canvas,
                                     r.x + 8, hr_y,
                                     r.width - 16, 1,
                                     deco->color);
            }
            break;
        }
    }

    /* Draw text layout offset by scroll */
    lui_text_layout_draw(&md->layout, canvas,
                         r.x, r.y - md->scroll_y,
                         NULL, NULL);

    /* Scrollbar */
    if (md->content_height > r.height) {
        int sb_x = r.x + r.width - md->scrollbar_width;
        int track_h = r.height;
        int thumb_h = (r.height * r.height) / md->content_height;
        if (thumb_h < 20) thumb_h = 20;
        int max_scroll = md->content_height - r.height;
        int thumb_y = r.y;
        if (max_scroll > 0)
            thumb_y += (md->scroll_y * (track_h - thumb_h)) / max_scroll;
        lvg_canvas_fill_rounded_rect(canvas, sb_x, thumb_y,
                                     md->scrollbar_width, thumb_h,
                                     md->scrollbar_width / 2,
                                     md->scrollbar_color);
    }

    canvas->_clip = old_clip;
}

static int md_event(lui_widget_t *w, const lui_event_t *event)
{
    lui_markdown_t *md = (lui_markdown_t *)w;

    if (event->type == LUI_EVENT_SCROLL) {
        lvg_rect_t r = lui_widget_absolute_rect(w);
        if (lvg_rect_contains_point(&r,
                event->data.scroll.x, event->data.scroll.y)) {
            int delta = (int)(event->data.scroll.delta_y * -30.0f);
            md->scroll_y += delta;
            md_clamp_scroll(md, r.height);
            return 1;
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void lui_markdown_init(lui_markdown_t *md, lui_font_t *font)
{
    if (!md) return;

    memset(md, 0, sizeof(*md));

    lui_widget_init(&md->widget);
    md->widget.width    = lvg_size_fill(1);
    md->widget.height   = lvg_size_fill(1);
    md->widget.measure  = md_measure;
    md->widget.draw     = md_draw;
    md->widget.on_event = md_event;
    md->widget.flags    = LUI_WIDGET_DRAWS_CHILDREN;

    md->font = font;
    lui_text_layout_init(&md->layout, font, 0);

    lui_md_style_default(&md->style);

    md->layout_dirty   = false;
    md->scroll_y       = 0;
    md->content_height = 0;

    md->bg              = LVG_COLOR_RGB(0x1E, 0x1E, 0x2E);
    md->scrollbar_color = LVG_COLOR_ARGB(0x80, 0x80, 0x84, 0x8A);
    md->scrollbar_width = 6;

    md->resolve_image      = NULL;
    md->resolve_image_user = NULL;
    md->on_link_click      = NULL;
    md->on_link_user       = NULL;

    md->link_regions       = NULL;
    md->link_region_count  = 0;
    md->link_region_cap    = 0;

    md->render_state.decos      = NULL;
    md->render_state.deco_count = 0;
    md->render_state.deco_cap   = 0;
}

void lui_markdown_destroy(lui_markdown_t *md)
{
    if (!md) return;

    lui_md_destroy(&md->doc);
    lui_text_layout_destroy(&md->layout);
    lui_md_render_state_destroy(&md->render_state);

    if (md->link_regions) {
        free(md->link_regions);
        md->link_regions      = NULL;
        md->link_region_count = 0;
        md->link_region_cap   = 0;
    }
}

void lui_markdown_set_text(lui_markdown_t *md, const char *src, int len)
{
    if (!md) return;

    /* Free previous document */
    lui_md_destroy(&md->doc);
    lui_text_layout_clear(&md->layout);

    md->link_region_count = 0;

    if (src && len != 0) {
        if (len < 0) len = (int)strlen(src);
        lui_md_parse(&md->doc, src, len);
    }

    md->layout_dirty = true;
}

void lui_markdown_scroll_to(lui_markdown_t *md, int y)
{
    if (!md) return;
    md->scroll_y = y;

    /* Clamp using current widget bounds */
    lvg_rect_t r = lui_widget_absolute_rect(&md->widget);
    int view_h = r.height > 0 ? r.height : 300;
    md_clamp_scroll(md, view_h);
}
