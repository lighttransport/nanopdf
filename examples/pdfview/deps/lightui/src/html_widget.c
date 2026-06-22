/*
 * html_widget.c — HTML display widget
 *
 * Owns a parsed HTML document, renders it via lui_text_layout_t,
 * and handles scrolling.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <lightui/html.h>
#include <lightvg/canvas.h>
#include <lightui/event.h>

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Rebuild layout from the parsed document if dirty. */
static void html_ensure_layout(lui_html_t *html, int width)
{
    if (!html->layout_dirty) return;

    html->layout.max_width = width;
    lui_html_render(&html->layout, &html->doc, &html->style, &html->render_state,
                    html->resolve_image, html->resolve_image_user);
    lui_text_layout_build(&html->layout);
    html->content_height = html->layout.total_height;
    html->layout_dirty = false;
}

/* Clamp scroll_y to valid range. */
static void html_clamp_scroll(lui_html_t *html, int view_h)
{
    int max_scroll = html->content_height - view_h;
    if (max_scroll < 0) max_scroll = 0;
    if (html->scroll_y < 0) html->scroll_y = 0;
    if (html->scroll_y > max_scroll) html->scroll_y = max_scroll;
}

/* -------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------- */

static int html_measure(const lui_widget_t *w, int *out_w, int *out_h,
                        void *user)
{
    (void)user;
    lui_html_t *html = (lui_html_t *)w;

    /* Rebuild layout at current max_width so we know the content size. */
    int width = html->layout.max_width > 0 ? html->layout.max_width : 300;
    html_ensure_layout(html, width);

    *out_w = html->layout.total_width;
    *out_h = html->layout.total_height;
    return 0;
}

static void html_draw(lui_widget_t *w, lvg_canvas_t *canvas)
{
    lui_html_t *html = (lui_html_t *)w;
    lvg_rect_t r = lui_widget_absolute_rect(w);
    if (lvg_rect_is_empty(&r)) return;

    /* Background */
    lvg_canvas_fill_rect(canvas, r.x, r.y, r.width, r.height, html->bg);

    /* Clip to widget bounds */
    lvg_rect_t old_clip = canvas->_clip;
    lvg_rect_t clip = lvg_rect_intersect(&old_clip, &r);
    canvas->_clip = clip;

    /* Rebuild layout if dirty (use widget width for wrapping) */
    html_ensure_layout(html, r.width);

    /* Clamp scroll */
    html_clamp_scroll(html, r.height);

    /* Draw block decorations (blockquote bars, code block bgs, HRs) */
    for (int i = 0; i < html->render_state.deco_count; i++) {
        const lui_html_deco_t *deco = &html->render_state.decos[i];

        /* Compute top/bottom from line indices */
        int deco_top = 0;
        int deco_bottom = 0;
        if (deco->line_start < html->layout.line_count)
            deco_top = html->layout.lines[deco->line_start].y;
        if (deco->line_end > 0 && deco->line_end <= html->layout.line_count) {
            const lui_line_t *last = &html->layout.lines[deco->line_end - 1];
            deco_bottom = last->y + last->height;
        }

        int dy = r.y - html->scroll_y;

        switch (deco->type) {
        case LUI_HTML_DECO_CODE_BLOCK_BG:
            lvg_canvas_fill_rect(canvas,
                                 r.x, deco_top + dy,
                                 r.width, deco_bottom - deco_top,
                                 deco->color);
            break;
        case LUI_HTML_DECO_BLOCKQUOTE_BAR:
            lvg_canvas_fill_rect(canvas,
                                 r.x + deco->indent, deco_top + dy,
                                 3, deco_bottom - deco_top,
                                 deco->color);
            break;
        case LUI_HTML_DECO_HR:
            {
                int hr_y = (deco_top + deco_bottom) / 2 + dy;
                lvg_canvas_fill_rect(canvas,
                                     r.x + 8, hr_y,
                                     r.width - 16, 1,
                                     deco->color);
            }
            break;
        case LUI_HTML_DECO_TABLE_BORDER:
        case LUI_HTML_DECO_TH_BG:
            lvg_canvas_fill_rect(canvas,
                                 r.x + deco->x, deco_top + dy,
                                 deco->width, deco_bottom - deco_top,
                                 deco->color);
            break;
        }
    }

    /* Draw text layout offset by scroll */
    lui_text_layout_draw(&html->layout, canvas,
                         r.x, r.y - html->scroll_y,
                         NULL, NULL);

    /* Scrollbar */
    if (html->content_height > r.height) {
        int sb_x = r.x + r.width - html->scrollbar_width;
        int track_h = r.height;
        int thumb_h = (r.height * r.height) / html->content_height;
        if (thumb_h < 20) thumb_h = 20;
        int max_scroll = html->content_height - r.height;
        int thumb_y = r.y;
        if (max_scroll > 0)
            thumb_y += (html->scroll_y * (track_h - thumb_h)) / max_scroll;
        lvg_canvas_fill_rounded_rect(canvas, sb_x, thumb_y,
                                     html->scrollbar_width, thumb_h,
                                     html->scrollbar_width / 2,
                                     html->scrollbar_color);
    }

    canvas->_clip = old_clip;
}

static int html_event(lui_widget_t *w, const lui_event_t *event)
{
    lui_html_t *html = (lui_html_t *)w;

    if (event->type == LUI_EVENT_SCROLL) {
        lvg_rect_t r = lui_widget_absolute_rect(w);
        if (lvg_rect_contains_point(&r,
                event->data.scroll.x, event->data.scroll.y)) {
            int delta = (int)(event->data.scroll.delta_y * -30.0f);
            html->scroll_y += delta;
            html_clamp_scroll(html, r.height);
            return 1;
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void lui_html_init(lui_html_t *html, lui_font_t *font)
{
    if (!html) return;

    memset(html, 0, sizeof(*html));

    lui_widget_init(&html->widget);
    html->widget.width    = lvg_size_fill(1);
    html->widget.height   = lvg_size_fill(1);
    html->widget.measure  = html_measure;
    html->widget.draw     = html_draw;
    html->widget.on_event = html_event;
    html->widget.flags    = LUI_WIDGET_DRAWS_CHILDREN;

    html->font = font;
    lui_text_layout_init(&html->layout, font, 0);

    lui_html_style_default(&html->style);

    html->layout_dirty   = false;
    html->scroll_y       = 0;
    html->content_height = 0;

    html->bg              = LVG_COLOR_RGB(0x1E, 0x1E, 0x2E);
    html->scrollbar_color = LVG_COLOR_ARGB(0x80, 0x80, 0x84, 0x8A);
    html->scrollbar_width = 6;

    html->resolve_image      = NULL;
    html->resolve_image_user = NULL;
    html->on_link_click      = NULL;
    html->on_link_user       = NULL;

    html->link_regions       = NULL;
    html->link_region_count  = 0;
    html->link_region_cap    = 0;

    html->render_state.decos      = NULL;
    html->render_state.deco_count = 0;
    html->render_state.deco_cap   = 0;
}

void lui_html_destroy(lui_html_t *html)
{
    if (!html) return;

    lui_html_doc_destroy(&html->doc);
    lui_text_layout_destroy(&html->layout);
    lui_html_render_state_destroy(&html->render_state);

    if (html->link_regions) {
        free(html->link_regions);
        html->link_regions      = NULL;
        html->link_region_count = 0;
        html->link_region_cap   = 0;
    }
}

void lui_html_set_text(lui_html_t *html, const char *src, int len)
{
    if (!html) return;

    /* Free previous document */
    lui_html_doc_destroy(&html->doc);
    lui_text_layout_clear(&html->layout);

    html->link_region_count = 0;

    if (src && len != 0) {
        if (len < 0) len = (int)strlen(src);
        lui_html_parse(&html->doc, src, len);
    }

    html->layout_dirty = true;
}

void lui_html_scroll_to(lui_html_t *html, int y)
{
    if (!html) return;
    html->scroll_y = y;

    /* Clamp using current widget bounds */
    lvg_rect_t r = lui_widget_absolute_rect(&html->widget);
    int view_h = r.height > 0 ? r.height : 300;
    html_clamp_scroll(html, view_h);
}
