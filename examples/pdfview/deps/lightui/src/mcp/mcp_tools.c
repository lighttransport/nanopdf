/*
 * mcp_tools.c — lightui MCP tool implementations
 *
 * Tools exposed to LLMs for inspecting and interacting with the UI:
 *   - inspect_tree:    dump widget tree structure
 *   - get_widget:      detailed info on a single widget
 *   - send_event:      inject a mouse/keyboard event
 *   - screenshot:      capture surface as base64 PPM
 *   - get_theme:       return current theme colours
 *   - set_theme:       switch between built-in themes
 *   - list_properties: list settable properties of a widget
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <lightui/mcp.h>
#include <lightui/layout.h>
#include <lightvg/surface.h>
#include <lightvg/canvas.h>
#include <lightui/theme.h>
#include <lightui/event.h>
#include <lightui/export.h>
#include <lightui/image_cmp.h>
#include <lightui/font.h>
#include "lui_json.h"
#include "lui_image_enc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Access MCP internals ----------------------------------------------- */

/* Must match the layout prefix of struct lui_mcp in mcp_server.c */
typedef struct {
    char *name;
    char *description;
    char *input_schema;
    lui_mcp_tool_handler_fn handler;
    void *user_data;
} lui_mcp_custom_tool_t;

#define LUI_MCP_MAX_CUSTOM_TOOLS 32

struct lui_mcp {
    int        transport;
    void      *ui_ctx;
    void      *surface;
    void      *theme;

    /* Stdio transport */
    char      *stdin_buf;
    int        stdin_len;
    int        stdin_cap;

    /* HTTP transport */
    void      *http;
    int        port;

    bool       initialized;

    /* Custom tool registry */
    lui_mcp_custom_tool_t custom_tools[LUI_MCP_MAX_CUSTOM_TOOLS];
    int                   num_custom_tools;

    /* Canvas debugging */
    void      *canvas;       /* lvg_canvas_t*    */
    void      *font;         /* lui_font_t*      */
    void      *recorder;     /* lui_recorder_t*  */

    #define LUI_MCP_MAX_SNAPSHOTS 8
    struct { char name[64]; void *surface; } snapshots[LUI_MCP_MAX_SNAPSHOTS];
    int        num_snapshots;
};

/* ---- Tool: inspect_tree ------------------------------------------------- */

static void write_widget_node(lui_json_buf_t *b, const lui_widget_t *w)
{
    lui_json_buf_object_begin(b);

    lui_json_buf_key(b, "id");
    lui_json_buf_int(b, w->id);

    lui_json_buf_key(b, "rect");
    lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "x");
        lui_json_buf_int(b, w->computed.x);
        lui_json_buf_key(b, "y");
        lui_json_buf_int(b, w->computed.y);
        lui_json_buf_key(b, "w");
        lui_json_buf_int(b, w->computed.width);
        lui_json_buf_key(b, "h");
        lui_json_buf_int(b, w->computed.height);
    lui_json_buf_object_end(b);

    lui_json_buf_key(b, "direction");
    lui_json_buf_string(b, w->direction == LUI_LAYOUT_ROW ? "row" :
                            w->direction == LUI_LAYOUT_COLUMN ? "column" : "stack");

    lui_json_buf_key(b, "flags");
    lui_json_buf_int(b, (int)w->flags);

    lui_json_buf_key(b, "has_draw");
    lui_json_buf_bool(b, w->draw != NULL);

    lui_json_buf_key(b, "has_event");
    lui_json_buf_bool(b, w->on_event != NULL);

    /* Children */
    if (w->first_child) {
        lui_json_buf_key(b, "children");
        lui_json_buf_array_begin(b);
        for (const lui_widget_t *c = w->first_child; c; c = c->next_sibling)
            write_widget_node(b, c);
        lui_json_buf_array_end(b);
    }

    lui_json_buf_object_end(b);
}

static void tool_inspect_tree(const lui_json_t *args, lui_json_buf_t *b,
                                lui_mcp_t *mcp)
{
    (void)args;
    lui_ui_ctx_t *ctx = (lui_ui_ctx_t *)mcp->ui_ctx;
    if (!ctx || !ctx->root) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type");
        lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text");
        lui_json_buf_string(b, "Error: no UI context set");
        lui_json_buf_object_end(b);
        return;
    }

    /* Build tree as JSON, then emit as text content */
    lui_json_buf_t *tree = lui_json_buf_new();
    write_widget_node(tree, ctx->root);
    int tree_len = 0;
    char *tree_json = lui_json_buf_finish(tree, &tree_len);

    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type");
    lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text");
    lui_json_buf_raw(b, tree_json, tree_len);
    lui_json_buf_object_end(b);

    free(tree_json);
}

/* ---- Tool: get_widget --------------------------------------------------- */

static const lui_widget_t *find_widget_by_id(const lui_widget_t *w, int id)
{
    if (!w) return NULL;
    if (w->id == id) return w;
    for (const lui_widget_t *c = w->first_child; c; c = c->next_sibling) {
        const lui_widget_t *found = find_widget_by_id(c, id);
        if (found) return found;
    }
    return NULL;
}

static void tool_get_widget(const lui_json_t *args, lui_json_buf_t *b,
                              lui_mcp_t *mcp)
{
    lui_ui_ctx_t *ctx = (lui_ui_ctx_t *)mcp->ui_ctx;
    int id = lui_json_int(lui_json_get(args, "id"));

    if (!ctx || !ctx->root) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: no UI context");
        lui_json_buf_object_end(b);
        return;
    }

    const lui_widget_t *w = find_widget_by_id(ctx->root, id);
    if (!w) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Error: widget id=%d not found", id);
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
        lui_json_buf_object_end(b);
        return;
    }

    /* Detailed widget info */
    lui_json_buf_t *info = lui_json_buf_new();
    lui_json_buf_object_begin(info);
        lui_json_buf_key(info, "id");        lui_json_buf_int(info, w->id);
        lui_json_buf_key(info, "computed");
        lui_json_buf_object_begin(info);
            lui_json_buf_key(info, "x"); lui_json_buf_int(info, w->computed.x);
            lui_json_buf_key(info, "y"); lui_json_buf_int(info, w->computed.y);
            lui_json_buf_key(info, "w"); lui_json_buf_int(info, w->computed.width);
            lui_json_buf_key(info, "h"); lui_json_buf_int(info, w->computed.height);
        lui_json_buf_object_end(info);
        lui_json_buf_key(info, "absolute");
        lvg_rect_t ar = lui_widget_absolute_rect(w);
        lui_json_buf_object_begin(info);
            lui_json_buf_key(info, "x"); lui_json_buf_int(info, ar.x);
            lui_json_buf_key(info, "y"); lui_json_buf_int(info, ar.y);
            lui_json_buf_key(info, "w"); lui_json_buf_int(info, ar.width);
            lui_json_buf_key(info, "h"); lui_json_buf_int(info, ar.height);
        lui_json_buf_object_end(info);
        lui_json_buf_key(info, "direction");
        lui_json_buf_string(info, w->direction == LUI_LAYOUT_ROW ? "row" :
                             w->direction == LUI_LAYOUT_COLUMN ? "column" : "stack");
        lui_json_buf_key(info, "width_mode");
        lui_json_buf_string(info, w->width.mode == LUI_SIZE_FIXED ? "fixed" :
                             w->width.mode == LUI_SIZE_HUG ? "hug" : "fill");
        lui_json_buf_key(info, "width_value"); lui_json_buf_int(info, w->width.value);
        lui_json_buf_key(info, "height_mode");
        lui_json_buf_string(info, w->height.mode == LUI_SIZE_FIXED ? "fixed" :
                             w->height.mode == LUI_SIZE_HUG ? "hug" : "fill");
        lui_json_buf_key(info, "height_value"); lui_json_buf_int(info, w->height.value);
        lui_json_buf_key(info, "padding");
        lui_json_buf_object_begin(info);
            lui_json_buf_key(info, "top");    lui_json_buf_int(info, w->padding.top);
            lui_json_buf_key(info, "right");  lui_json_buf_int(info, w->padding.right);
            lui_json_buf_key(info, "bottom"); lui_json_buf_int(info, w->padding.bottom);
            lui_json_buf_key(info, "left");   lui_json_buf_int(info, w->padding.left);
        lui_json_buf_object_end(info);
        lui_json_buf_key(info, "spacing");     lui_json_buf_int(info, w->spacing);
        lui_json_buf_key(info, "flags");       lui_json_buf_int(info, (int)w->flags);
        lui_json_buf_key(info, "child_count"); lui_json_buf_int(info, lui_widget_child_count(w));
        lui_json_buf_key(info, "has_focus");
        lui_json_buf_bool(info, ctx->focus == w);
    lui_json_buf_object_end(info);

    int info_len = 0;
    char *info_json = lui_json_buf_finish(info, &info_len);

    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_raw(b, info_json, info_len);
    lui_json_buf_object_end(b);

    free(info_json);
}

/* ---- Tool: send_event --------------------------------------------------- */

static void tool_send_event(const lui_json_t *args, lui_json_buf_t *b,
                              lui_mcp_t *mcp)
{
    lui_ui_ctx_t *ctx = (lui_ui_ctx_t *)mcp->ui_ctx;
    if (!ctx) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: no UI context");
        lui_json_buf_object_end(b);
        return;
    }

    const char *event_type = lui_json_string(lui_json_get(args, "type"));
    if (!event_type) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: missing event type");
        lui_json_buf_object_end(b);
        return;
    }

    lui_event_t ev;
    memset(&ev, 0, sizeof(ev));

    if (strcmp(event_type, "mouse_down") == 0) {
        ev.type = LUI_EVENT_MOUSE_DOWN;
        ev.data.mouse_button.x = lui_json_int(lui_json_get(args, "x"));
        ev.data.mouse_button.y = lui_json_int(lui_json_get(args, "y"));
        ev.data.mouse_button.button = LUI_MOUSE_LEFT;
        const lui_json_t *btn = lui_json_get(args, "button");
        if (btn) ev.data.mouse_button.button = lui_json_int(btn);
        const lui_json_t *clicks = lui_json_get(args, "clicks");
        ev.data.mouse_button.clicks = clicks ? lui_json_int(clicks) : 1;
    } else if (strcmp(event_type, "mouse_up") == 0) {
        ev.type = LUI_EVENT_MOUSE_UP;
        ev.data.mouse_button.x = lui_json_int(lui_json_get(args, "x"));
        ev.data.mouse_button.y = lui_json_int(lui_json_get(args, "y"));
        ev.data.mouse_button.button = LUI_MOUSE_LEFT;
        const lui_json_t *btn = lui_json_get(args, "button");
        if (btn) ev.data.mouse_button.button = lui_json_int(btn);
        const lui_json_t *clicks = lui_json_get(args, "clicks");
        ev.data.mouse_button.clicks = clicks ? lui_json_int(clicks) : 1;
    } else if (strcmp(event_type, "mouse_move") == 0) {
        ev.type = LUI_EVENT_MOUSE_MOVE;
        ev.data.mouse_move.x = lui_json_int(lui_json_get(args, "x"));
        ev.data.mouse_move.y = lui_json_int(lui_json_get(args, "y"));
        const lui_json_t *dx = lui_json_get(args, "dx");
        const lui_json_t *dy = lui_json_get(args, "dy");
        if (dx) ev.data.mouse_move.dx = lui_json_int(dx);
        if (dy) ev.data.mouse_move.dy = lui_json_int(dy);
    } else if (strcmp(event_type, "key_down") == 0) {
        ev.type = LUI_EVENT_KEY_DOWN;
        ev.data.key.key = lui_json_int(lui_json_get(args, "key"));
        const lui_json_t *mods_node = lui_json_get(args, "mods");
        if (mods_node) ev.data.key.mods = (uint32_t)lui_json_int(mods_node);
        const lui_json_t *repeat_node = lui_json_get(args, "repeat");
        if (repeat_node) ev.data.key.repeat = lui_json_bool(repeat_node);
    } else if (strcmp(event_type, "key_up") == 0) {
        ev.type = LUI_EVENT_KEY_UP;
        ev.data.key.key = lui_json_int(lui_json_get(args, "key"));
        const lui_json_t *mods_node = lui_json_get(args, "mods");
        if (mods_node) ev.data.key.mods = (uint32_t)lui_json_int(mods_node);
    } else if (strcmp(event_type, "text_input") == 0) {
        ev.type = LUI_EVENT_TEXT_INPUT;
        const char *text = lui_json_string(lui_json_get(args, "text"));
        if (text) {
            int tlen = (int)strlen(text);
            if (tlen > 7) tlen = 7;
            memcpy(ev.data.text.text, text, tlen);
            ev.data.text.text[tlen] = '\0';
        }
    } else if (strcmp(event_type, "scroll") == 0) {
        ev.type = LUI_EVENT_SCROLL;
        ev.data.scroll.x = lui_json_int(lui_json_get(args, "x"));
        ev.data.scroll.y = lui_json_int(lui_json_get(args, "y"));
        ev.data.scroll.delta_x = (float)lui_json_number(lui_json_get(args, "delta_x"));
        ev.data.scroll.delta_y = (float)lui_json_number(lui_json_get(args, "delta_y"));
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Error: unknown event type '%s'", event_type);
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
        lui_json_buf_object_end(b);
        return;
    }

    lui_widget_t *consumer = lui_ui_ctx_dispatch(ctx, &ev);

    char result[128];
    if (consumer)
        snprintf(result, sizeof(result), "Event consumed by widget id=%d", consumer->id);
    else
        snprintf(result, sizeof(result), "Event not consumed");

    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b, result);
    lui_json_buf_object_end(b);
}

/* ---- Tool: screenshot --------------------------------------------------- */

static void tool_screenshot(const lui_json_t *args, lui_json_buf_t *b,
                              lui_mcp_t *mcp)
{
    lvg_surface_t *surf = (lvg_surface_t *)mcp->surface;
    if (!surf || !surf->pixels) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: no surface");
        lui_json_buf_object_end(b);
        return;
    }

    /* Parse format (default: png) */
    const char *fmt_name = lui_json_string(lui_json_get(args, "format"));
    int fmt_int = lui_image_format_from_name(fmt_name);
    if (fmt_int < 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Error: unknown format '%s'", fmt_name);
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
        lui_json_buf_object_end(b);
        return;
    }
    lui_img_enc_fmt_t fmt = (lui_img_enc_fmt_t)fmt_int;

    /* Parse optional JPG quality */
    int quality = 90;
    const lui_json_t *q = lui_json_get(args, "quality");
    if (q) quality = lui_json_int(q);

    int w = surf->width, h = surf->height;

    /* Encode image */
    int enc_len = 0;
    unsigned char *enc = lui_image_encode(surf->pixels, w, h, surf->stride,
                                           fmt, quality, &enc_len);
    if (!enc) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: encoding failed");
        lui_json_buf_object_end(b);
        return;
    }

    /* Base64 encode */
    int b64_len = 0;
    char *b64 = lui_base64_encode(enc, enc_len, &b64_len);
    free(enc);

    if (!b64) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: base64 failed");
        lui_json_buf_object_end(b);
        return;
    }

    /* Text summary */
    const char *mime = lui_image_mime_type(fmt);
    char summary[128];
    snprintf(summary, sizeof(summary),
             "Surface %dx%d, format=%s, %d bytes encoded",
             w, h, fmt_name ? fmt_name : "png", enc_len);
    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b, summary);
    lui_json_buf_object_end(b);

    /* Image data */
    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type");     lui_json_buf_string(b, "image");
    lui_json_buf_key(b, "data");     lui_json_buf_string(b, b64);
    lui_json_buf_key(b, "mimeType"); lui_json_buf_string(b, mime);
    lui_json_buf_object_end(b);

    free(b64);
}

/* ---- Tool: get_theme / set_theme ---------------------------------------- */

static void write_color(lui_json_buf_t *b, const char *key, lvg_color_t c)
{
    char hex[10];
    snprintf(hex, sizeof(hex), "#%06X", (unsigned)(c & 0xFFFFFF));
    lui_json_buf_key(b, key);
    lui_json_buf_string(b, hex);
}

static void tool_get_theme(const lui_json_t *args, lui_json_buf_t *b,
                             lui_mcp_t *mcp)
{
    (void)args;
    lui_theme_t *theme = (lui_theme_t *)mcp->theme;
    if (!theme) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: no theme set");
        lui_json_buf_object_end(b);
        return;
    }

    lui_json_buf_t *t = lui_json_buf_new();
    lui_json_buf_object_begin(t);
        write_color(t, "window_bg",    theme->window_bg);
        write_color(t, "panel_bg",     theme->panel_bg);
        write_color(t, "panel_bg_alt", theme->panel_bg_alt);
        write_color(t, "text",         theme->text);
        write_color(t, "text_dim",     theme->text_dim);
        write_color(t, "accent",       theme->accent);
        write_color(t, "accent_active", theme->accent_active);
        write_color(t, "input_bg",     theme->input_bg);
        write_color(t, "input_text",   theme->input_text);
        write_color(t, "checkbox_bg",  theme->checkbox_bg);
        write_color(t, "slider_track", theme->slider_track);
        write_color(t, "slider_fill",  theme->slider_fill);
    lui_json_buf_object_end(t);

    int tlen = 0;
    char *tjson = lui_json_buf_finish(t, &tlen);

    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_raw(b, tjson, tlen);
    lui_json_buf_object_end(b);

    free(tjson);
}

static void tool_set_theme(const lui_json_t *args, lui_json_buf_t *b,
                             lui_mcp_t *mcp)
{
    lui_theme_t *theme = (lui_theme_t *)mcp->theme;
    if (!theme) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: no theme set");
        lui_json_buf_object_end(b);
        return;
    }

    const char *name = lui_json_string(lui_json_get(args, "name"));
    if (!name) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: missing theme name");
        lui_json_buf_object_end(b);
        return;
    }

    if (strcmp(name, "flat") == 0) {
        lui_theme_flat(theme);
    } else if (strcmp(name, "4dwm") == 0) {
        lui_theme_4dwm(theme);
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Error: unknown theme '%s'", name);
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
        lui_json_buf_object_end(b);
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "Theme set to '%s'. Re-apply to widgets to take effect.", name);
    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
    lui_json_buf_object_end(b);
}

/* ---- Tool: inspect_pixel ------------------------------------------------ */

static void tool_inspect_pixel(const lui_json_t *args, lui_json_buf_t *b,
                                lui_mcp_t *mcp)
{
    lvg_surface_t *surf = (lvg_surface_t *)mcp->surface;
    if (!surf || !surf->pixels) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: no surface");
        lui_json_buf_object_end(b);
        return;
    }

    int x = lui_json_int(lui_json_get(args, "x"));
    int y = lui_json_int(lui_json_get(args, "y"));

    if (x < 0 || x >= surf->width || y < 0 || y >= surf->height) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "Error: (%d,%d) out of bounds (surface %dx%d)", x, y,
                 surf->width, surf->height);
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
        lui_json_buf_object_end(b);
        return;
    }

    uint32_t c = surf->pixels[y * surf->stride + x];
    int a = (int)LVG_COLOR_A(c);
    int r = (int)LVG_COLOR_R(c);
    int g = (int)LVG_COLOR_G(c);
    int bv = (int)LVG_COLOR_B(c);

    char hex[10], hex_a[12];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, bv);
    snprintf(hex_a, sizeof(hex_a), "#%02X%02X%02X%02X", a, r, g, bv);

    char json[256];
    snprintf(json, sizeof(json),
             "Pixel (%d,%d): r=%d g=%d b=%d a=%d hex=%s argb=%s",
             x, y, r, g, bv, a, hex, hex_a);

    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b, json);
    lui_json_buf_object_end(b);
}

/* ---- Tool: inspect_region ----------------------------------------------- */

static void tool_inspect_region(const lui_json_t *args, lui_json_buf_t *b,
                                 lui_mcp_t *mcp)
{
    lvg_surface_t *surf = (lvg_surface_t *)mcp->surface;
    if (!surf || !surf->pixels) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: no surface");
        lui_json_buf_object_end(b);
        return;
    }

    int rx = lui_json_int(lui_json_get(args, "x"));
    int ry = lui_json_int(lui_json_get(args, "y"));
    int rw = lui_json_int(lui_json_get(args, "w"));
    int rh = lui_json_int(lui_json_get(args, "h"));

    /* Clamp to surface */
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > surf->width)  rw = surf->width - rx;
    if (ry + rh > surf->height) rh = surf->height - ry;
    if (rw <= 0 || rh <= 0) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: region empty or out of bounds");
        lui_json_buf_object_end(b);
        return;
    }

    /* Compute stats */
    int total = rw * rh;
    int min_r = 255, max_r = 0, min_g = 255, max_g = 0;
    int min_b = 255, max_b = 0, min_a = 255, max_a = 0;
    uint32_t dominant = 0;
    int dominant_count = 0;

    /* Simple dominant-color: track one most-frequent color */
    /* For efficiency, just track last-seen color with highest run */
    uint32_t prev_c = 0;
    int run = 0;

    for (int y = ry; y < ry + rh; y++) {
        for (int x = rx; x < rx + rw; x++) {
            uint32_t c = surf->pixels[y * surf->stride + x];
            int cr = (int)LVG_COLOR_R(c), cg = (int)LVG_COLOR_G(c);
            int cb = (int)LVG_COLOR_B(c), ca = (int)LVG_COLOR_A(c);
            if (cr < min_r) min_r = cr;
            if (cr > max_r) max_r = cr;
            if (cg < min_g) min_g = cg;
            if (cg > max_g) max_g = cg;
            if (cb < min_b) min_b = cb;
            if (cb > max_b) max_b = cb;
            if (ca < min_a) min_a = ca;
            if (ca > max_a) max_a = ca;

            if (c == prev_c) {
                run++;
            } else {
                if (run > dominant_count) { dominant = prev_c; dominant_count = run; }
                prev_c = c; run = 1;
            }
        }
    }
    if (run > dominant_count) { dominant = prev_c; dominant_count = run; }

    int is_uniform = (min_r == max_r && min_g == max_g &&
                      min_b == max_b && min_a == max_a);

    char dom_hex[10];
    snprintf(dom_hex, sizeof(dom_hex), "#%02X%02X%02X",
             (int)LVG_COLOR_R(dominant), (int)LVG_COLOR_G(dominant),
             (int)LVG_COLOR_B(dominant));

    /* Text summary */
    char summary[512];
    snprintf(summary, sizeof(summary),
             "Region (%d,%d %dx%d): %d pixels, uniform=%s, "
             "dominant=%s (%d/%d), R=[%d,%d] G=[%d,%d] B=[%d,%d] A=[%d,%d]",
             rx, ry, rw, rh, total, is_uniform ? "yes" : "no",
             dom_hex, dominant_count, total,
             min_r, max_r, min_g, max_g, min_b, max_b, min_a, max_a);

    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b, summary);
    lui_json_buf_object_end(b);

    /* Cropped image (optional, default true) */
    const lui_json_t *inc = lui_json_get(args, "include_image");
    int include_image = (!inc || lui_json_bool(inc));

    if (include_image) {
        /* Create temp pixel buffer for the crop */
        uint32_t *crop = (uint32_t *)malloc((size_t)(rw * rh) * sizeof(uint32_t));
        if (crop) {
            for (int y = 0; y < rh; y++)
                memcpy(crop + y * rw,
                       surf->pixels + (ry + y) * surf->stride + rx,
                       (size_t)rw * sizeof(uint32_t));

            int enc_len = 0;
            unsigned char *enc = lui_image_encode(crop, rw, rh, rw,
                                                   LUI_IMG_PNG, 0, &enc_len);
            free(crop);
            if (enc) {
                int b64_len = 0;
                char *b64 = lui_base64_encode(enc, enc_len, &b64_len);
                free(enc);
                if (b64) {
                    lui_json_buf_object_begin(b);
                    lui_json_buf_key(b, "type");     lui_json_buf_string(b, "image");
                    lui_json_buf_key(b, "data");     lui_json_buf_string(b, b64);
                    lui_json_buf_key(b, "mimeType"); lui_json_buf_string(b, "image/png");
                    lui_json_buf_object_end(b);
                    free(b64);
                }
            }
        }
    }
}

/* ---- Tool: canvas_state ------------------------------------------------- */

static void tool_canvas_state(const lui_json_t *args, lui_json_buf_t *b,
                               lui_mcp_t *mcp)
{
    (void)args;
    lvg_surface_t *surf = (lvg_surface_t *)mcp->surface;
    if (!surf) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: no surface");
        lui_json_buf_object_end(b);
        return;
    }

    lvg_canvas_t *cv = (lvg_canvas_t *)mcp->canvas;

    char info[512];
    if (cv) {
        snprintf(info, sizeof(info),
                 "Surface: %dx%d, stride=%d, dpi_scale=%.2f, "
                 "pixel_format=ARGB8888, "
                 "clip=(%d,%d %dx%d)",
                 surf->width, surf->height, surf->stride, surf->dpi_scale,
                 cv->_clip.x, cv->_clip.y, cv->_clip.width, cv->_clip.height);
    } else {
        snprintf(info, sizeof(info),
                 "Surface: %dx%d, stride=%d, dpi_scale=%.2f, "
                 "pixel_format=ARGB8888, clip=(no canvas set)",
                 surf->width, surf->height, surf->stride, surf->dpi_scale);
    }

    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b, info);
    lui_json_buf_object_end(b);
}

/* ---- Tool: measure_text ------------------------------------------------- */

static void tool_measure_text(const lui_json_t *args, lui_json_buf_t *b,
                               lui_mcp_t *mcp)
{
#ifdef LUI_HAVE_FONTS
    const char *text = lui_json_string(lui_json_get(args, "text"));
    int font_size = lui_json_int(lui_json_get(args, "font_size"));
    const char *font_path = lui_json_string(lui_json_get(args, "font_path"));

    if (!text || font_size <= 0) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: text and font_size required");
        lui_json_buf_object_end(b);
        return;
    }

    lui_font_t *font = NULL;
    int owns_font = 0;

    if (font_path && font_path[0]) {
        font = lui_font_create(font_path, font_size);
        owns_font = 1;
    } else {
        font = (lui_font_t *)mcp->font;
    }

    if (!font) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b,
            "Error: no font available (set font_path or call lui_mcp_set_font)");
        lui_json_buf_object_end(b);
        return;
    }

    int width = lui_font_measure_text(font, text, -1);
    int ascent = lui_font_ascent(font);
    int descent = lui_font_descent(font);
    int line_height = lui_font_line_height(font);

    char result[256];
    snprintf(result, sizeof(result),
             "Text \"%s\" at %dpx: width=%d, ascent=%d, descent=%d, "
             "line_height=%d, total_height=%d",
             text, font_size, width, ascent, descent, line_height,
             ascent + descent);

    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b, result);
    lui_json_buf_object_end(b);

    if (owns_font) lui_font_destroy(font);
#else
    (void)args; (void)mcp;
    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b,
        "Error: fonts not available (build with LUI_HAVE_FONTS)");
    lui_json_buf_object_end(b);
#endif
}

/* ---- Tool: take_snapshot ------------------------------------------------ */

static void tool_take_snapshot(const lui_json_t *args, lui_json_buf_t *b,
                                lui_mcp_t *mcp)
{
    lvg_surface_t *surf = (lvg_surface_t *)mcp->surface;
    if (!surf || !surf->pixels) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: no surface");
        lui_json_buf_object_end(b);
        return;
    }

    const char *name = lui_json_string(lui_json_get(args, "name"));
    if (!name || !name[0]) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: name required");
        lui_json_buf_object_end(b);
        return;
    }

    /* Find existing slot or allocate new */
    int slot = -1;
    for (int i = 0; i < mcp->num_snapshots; i++) {
        if (strcmp(mcp->snapshots[i].name, name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (mcp->num_snapshots >= LUI_MCP_MAX_SNAPSHOTS) {
            /* Evict oldest */
            lvg_surface_t *old = (lvg_surface_t *)mcp->snapshots[0].surface;
            if (old) lvg_surface_destroy(old);
            for (int i = 0; i < LUI_MCP_MAX_SNAPSHOTS - 1; i++)
                mcp->snapshots[i] = mcp->snapshots[i + 1];
            mcp->num_snapshots--;
        }
        slot = mcp->num_snapshots++;
    } else {
        /* Overwrite: free old surface */
        lvg_surface_t *old = (lvg_surface_t *)mcp->snapshots[slot].surface;
        if (old) lvg_surface_destroy(old);
    }

    /* Copy surface pixels */
    lvg_surface_t *snap = lvg_surface_create(surf->width, surf->height);
    if (snap) {
        for (int y = 0; y < surf->height; y++)
            memcpy(snap->pixels + y * snap->stride,
                   surf->pixels + y * surf->stride,
                   (size_t)surf->width * sizeof(uint32_t));
    }

    snprintf(mcp->snapshots[slot].name, 64, "%s", name);
    mcp->snapshots[slot].surface = snap;

    char msg[128];
    snprintf(msg, sizeof(msg), "Snapshot '%s' saved (%dx%d)",
             name, surf->width, surf->height);
    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
    lui_json_buf_object_end(b);
}

/* ---- Tool: diff_images -------------------------------------------------- */

static lvg_surface_t *mcp_resolve_snapshot(lui_mcp_t *mcp, const char *name)
{
    if (!name) return NULL;
    if (strcmp(name, "current") == 0)
        return (lvg_surface_t *)mcp->surface;
    for (int i = 0; i < mcp->num_snapshots; i++) {
        if (strcmp(mcp->snapshots[i].name, name) == 0)
            return (lvg_surface_t *)mcp->snapshots[i].surface;
    }
    return NULL;
}

static void tool_diff_images(const lui_json_t *args, lui_json_buf_t *b,
                              lui_mcp_t *mcp)
{
    const char *name_a = lui_json_string(lui_json_get(args, "a"));
    const char *name_b = lui_json_string(lui_json_get(args, "b"));

    lvg_surface_t *sa = mcp_resolve_snapshot(mcp, name_a);
    lvg_surface_t *sb = mcp_resolve_snapshot(mcp, name_b);

    if (!sa || !sb) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Error: snapshot '%s' or '%s' not found",
                 name_a ? name_a : "null", name_b ? name_b : "null");
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
        lui_json_buf_object_end(b);
        return;
    }

    lui_cmp_result_t result;
    if (lui_image_cmp(sa, sb, LUI_CMP_RGB, &result) != 0) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: dimension mismatch");
        lui_json_buf_object_end(b);
        return;
    }

    /* Text summary */
    char summary[512];
    snprintf(summary, sizeof(summary),
             "Diff '%s' vs '%s': SSIM=%.6f, RMSE=%.2f, PSNR=%.2f dB, "
             "diff_pixels=%d/%d (%.2f%%)",
             name_a, name_b, result.ssim, result.rmse, result.psnr_db,
             result.diff_pixels, result.total_pixels,
             100.0 * result.diff_pixels / result.total_pixels);

    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b, summary);
    lui_json_buf_object_end(b);

    /* Diff heatmap image */
    lvg_surface_t *diff = lui_image_cmp_diff_surface(sa, sb);
    if (diff) {
        int enc_len = 0;
        unsigned char *enc = lui_image_encode(diff->pixels, diff->width,
                                               diff->height, diff->stride,
                                               LUI_IMG_PNG, 0, &enc_len);
        lvg_surface_destroy(diff);
        if (enc) {
            int b64_len = 0;
            char *b64 = lui_base64_encode(enc, enc_len, &b64_len);
            free(enc);
            if (b64) {
                lui_json_buf_object_begin(b);
                lui_json_buf_key(b, "type");     lui_json_buf_string(b, "image");
                lui_json_buf_key(b, "data");     lui_json_buf_string(b, b64);
                lui_json_buf_key(b, "mimeType"); lui_json_buf_string(b, "image/png");
                lui_json_buf_object_end(b);
                free(b64);
            }
        }
    }
}

/* ---- Tool: record_drawing ----------------------------------------------- */

static const char *cmd_type_name(lui_cmd_type_t t)
{
    switch (t) {
    case LUI_CMD_CLEAR:               return "clear";
    case LUI_CMD_SET_CLIP:            return "set_clip";
    case LUI_CMD_RESET_CLIP:          return "reset_clip";
    case LUI_CMD_FILL_RECT:           return "fill_rect";
    case LUI_CMD_STROKE_RECT:         return "stroke_rect";
    case LUI_CMD_FILL_CIRCLE:         return "fill_circle";
    case LUI_CMD_STROKE_CIRCLE:       return "stroke_circle";
    case LUI_CMD_FILL_ELLIPSE:        return "fill_ellipse";
    case LUI_CMD_STROKE_ELLIPSE:      return "stroke_ellipse";
    case LUI_CMD_FILL_ROUNDED_RECT:   return "fill_rounded_rect";
    case LUI_CMD_STROKE_ROUNDED_RECT: return "stroke_rounded_rect";
    case LUI_CMD_FILL_TRIANGLE:       return "fill_triangle";
    case LUI_CMD_FILL_POLYGON:        return "fill_polygon";
    case LUI_CMD_STROKE_POLYGON:      return "stroke_polygon";
    case LUI_CMD_DRAW_LINE:           return "draw_line";
    case LUI_CMD_DRAW_POLYLINE:       return "draw_polyline";
    case LUI_CMD_DRAW_LINE_AA:        return "draw_line_aa";
    case LUI_CMD_DRAW_POLYLINE_AA:    return "draw_polyline_aa";
    case LUI_CMD_DRAW_LINE_DASHED:    return "draw_line_dashed";
    case LUI_CMD_DRAW_POLYLINE_DASHED:return "draw_polyline_dashed";
    case LUI_CMD_DRAW_THICK_POLYLINE: return "draw_thick_polyline";
    case LUI_CMD_DRAW_ARROW:          return "draw_arrow";
    case LUI_CMD_FILL_RECT_HATCHED:   return "fill_rect_hatched";
    case LUI_CMD_FILL_POLYGON_HATCHED:return "fill_polygon_hatched";
    case LUI_CMD_DRAW_IMAGE:          return "draw_image";
    case LUI_CMD_DRAW_TEXT:           return "draw_text";
    default:                          return "unknown";
    }
}

static void serialize_cmd(lui_json_buf_t *b, const lui_cmd_t *cmd,
                           const lui_recorder_t *rec)
{
    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, cmd_type_name(cmd->type));

    char hex[10];
    snprintf(hex, sizeof(hex), "#%06X", (unsigned)(cmd->color & 0xFFFFFF));
    lui_json_buf_key(b, "color"); lui_json_buf_string(b, hex);

    if (cmd->stroke_width > 0) {
        lui_json_buf_key(b, "stroke_width"); lui_json_buf_int(b, cmd->stroke_width);
    }

    switch (cmd->type) {
    case LUI_CMD_FILL_RECT: case LUI_CMD_STROKE_RECT: case LUI_CMD_FILL_RECT_HATCHED:
        lui_json_buf_key(b, "x"); lui_json_buf_int(b, cmd->d.rect.x);
        lui_json_buf_key(b, "y"); lui_json_buf_int(b, cmd->d.rect.y);
        lui_json_buf_key(b, "w"); lui_json_buf_int(b, cmd->d.rect.w);
        lui_json_buf_key(b, "h"); lui_json_buf_int(b, cmd->d.rect.h);
        break;
    case LUI_CMD_FILL_CIRCLE: case LUI_CMD_STROKE_CIRCLE:
        lui_json_buf_key(b, "cx"); lui_json_buf_int(b, cmd->d.circle.cx);
        lui_json_buf_key(b, "cy"); lui_json_buf_int(b, cmd->d.circle.cy);
        lui_json_buf_key(b, "r");  lui_json_buf_int(b, cmd->d.circle.r);
        break;
    case LUI_CMD_FILL_ELLIPSE: case LUI_CMD_STROKE_ELLIPSE:
        lui_json_buf_key(b, "cx"); lui_json_buf_int(b, cmd->d.ellipse.cx);
        lui_json_buf_key(b, "cy"); lui_json_buf_int(b, cmd->d.ellipse.cy);
        lui_json_buf_key(b, "rx"); lui_json_buf_int(b, cmd->d.ellipse.rx);
        lui_json_buf_key(b, "ry"); lui_json_buf_int(b, cmd->d.ellipse.ry);
        break;
    case LUI_CMD_FILL_ROUNDED_RECT: case LUI_CMD_STROKE_ROUNDED_RECT:
        lui_json_buf_key(b, "x"); lui_json_buf_int(b, cmd->d.rounded_rect.x);
        lui_json_buf_key(b, "y"); lui_json_buf_int(b, cmd->d.rounded_rect.y);
        lui_json_buf_key(b, "w"); lui_json_buf_int(b, cmd->d.rounded_rect.w);
        lui_json_buf_key(b, "h"); lui_json_buf_int(b, cmd->d.rounded_rect.h);
        lui_json_buf_key(b, "radius"); lui_json_buf_int(b, cmd->d.rounded_rect.radius);
        break;
    case LUI_CMD_FILL_TRIANGLE:
        lui_json_buf_key(b, "x0"); lui_json_buf_int(b, cmd->d.triangle.x0);
        lui_json_buf_key(b, "y0"); lui_json_buf_int(b, cmd->d.triangle.y0);
        lui_json_buf_key(b, "x1"); lui_json_buf_int(b, cmd->d.triangle.x1);
        lui_json_buf_key(b, "y1"); lui_json_buf_int(b, cmd->d.triangle.y1);
        lui_json_buf_key(b, "x2"); lui_json_buf_int(b, cmd->d.triangle.x2);
        lui_json_buf_key(b, "y2"); lui_json_buf_int(b, cmd->d.triangle.y2);
        break;
    case LUI_CMD_DRAW_LINE: case LUI_CMD_DRAW_LINE_DASHED:
        lui_json_buf_key(b, "x0"); lui_json_buf_int(b, cmd->d.line.x0);
        lui_json_buf_key(b, "y0"); lui_json_buf_int(b, cmd->d.line.y0);
        lui_json_buf_key(b, "x1"); lui_json_buf_int(b, cmd->d.line.x1);
        lui_json_buf_key(b, "y1"); lui_json_buf_int(b, cmd->d.line.y1);
        break;
    case LUI_CMD_DRAW_ARROW:
        lui_json_buf_key(b, "x0"); lui_json_buf_int(b, cmd->d.arrow.x0);
        lui_json_buf_key(b, "y0"); lui_json_buf_int(b, cmd->d.arrow.y0);
        lui_json_buf_key(b, "x1"); lui_json_buf_int(b, cmd->d.arrow.x1);
        lui_json_buf_key(b, "y1"); lui_json_buf_int(b, cmd->d.arrow.y1);
        lui_json_buf_key(b, "head_size"); lui_json_buf_int(b, cmd->d.arrow.head_size);
        break;
    case LUI_CMD_DRAW_TEXT:
        lui_json_buf_key(b, "x"); lui_json_buf_int(b, cmd->d.text.x);
        lui_json_buf_key(b, "y"); lui_json_buf_int(b, cmd->d.text.y);
        if (rec && cmd->d.text.text_offset >= 0 &&
            cmd->d.text.text_offset < rec->data_used) {
            lui_json_buf_key(b, "text");
            lui_json_buf_string(b, (const char *)(rec->data + cmd->d.text.text_offset));
        }
        break;
    case LUI_CMD_FILL_POLYGON: case LUI_CMD_STROKE_POLYGON:
        lui_json_buf_key(b, "vertex_count"); lui_json_buf_int(b, cmd->d.polygon.count);
        break;
    case LUI_CMD_DRAW_POLYLINE:
        lui_json_buf_key(b, "point_count"); lui_json_buf_int(b, cmd->d.polyline.count);
        break;
    case LUI_CMD_SET_CLIP:
        lui_json_buf_key(b, "x"); lui_json_buf_int(b, cmd->d.clip.rect.x);
        lui_json_buf_key(b, "y"); lui_json_buf_int(b, cmd->d.clip.rect.y);
        lui_json_buf_key(b, "w"); lui_json_buf_int(b, cmd->d.clip.rect.width);
        lui_json_buf_key(b, "h"); lui_json_buf_int(b, cmd->d.clip.rect.height);
        break;
    default:
        break;
    }

    lui_json_buf_object_end(b);
}

static void tool_record_drawing(const lui_json_t *args, lui_json_buf_t *b,
                                 lui_mcp_t *mcp)
{
    const char *action = lui_json_string(lui_json_get(args, "action"));
    if (!action) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: action required");
        lui_json_buf_object_end(b);
        return;
    }

    lui_recorder_t *rec = (lui_recorder_t *)mcp->recorder;

    if (strcmp(action, "start") == 0) {
        lvg_surface_t *surf = (lvg_surface_t *)mcp->surface;
        int w = surf ? surf->width : 800;
        int h = surf ? surf->height : 600;
        if (rec) lui_recorder_destroy(rec);
        rec = lui_recorder_create(w, h);
        mcp->recorder = rec;

        char msg[64];
        snprintf(msg, sizeof(msg), "Recording started (%dx%d)", w, h);
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
        lui_json_buf_object_end(b);
    } else if (strcmp(action, "stop") == 0) {
        int count = rec ? rec->cmd_count : 0;
        if (rec) { lui_recorder_destroy(rec); mcp->recorder = NULL; }
        char msg[64];
        snprintf(msg, sizeof(msg), "Recording stopped (%d commands)", count);
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
        lui_json_buf_object_end(b);
    } else if (strcmp(action, "clear") == 0) {
        if (rec) lui_recorder_reset(rec);
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Recording cleared");
        lui_json_buf_object_end(b);
    } else if (strcmp(action, "status") == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Recording %s, %d commands",
                 rec ? "active" : "inactive", rec ? rec->cmd_count : 0);
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
        lui_json_buf_object_end(b);
    } else if (strcmp(action, "list") == 0) {
        if (!rec || rec->cmd_count == 0) {
            lui_json_buf_object_begin(b);
            lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
            lui_json_buf_key(b, "text"); lui_json_buf_string(b,
                rec ? "No commands recorded" : "No active recording");
            lui_json_buf_object_end(b);
            return;
        }

        /* Serialize commands as JSON text */
        lui_json_buf_t *cmds = lui_json_buf_new();
        lui_json_buf_array_begin(cmds);
        for (int i = 0; i < rec->cmd_count; i++)
            serialize_cmd(cmds, &rec->cmds[i], rec);
        lui_json_buf_array_end(cmds);

        int json_len = 0;
        char *json_str = lui_json_buf_finish(cmds, &json_len);
        if (json_str) {
            char header[64];
            snprintf(header, sizeof(header), "%d drawing commands:", rec->cmd_count);
            lui_json_buf_object_begin(b);
            lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
            lui_json_buf_key(b, "text"); lui_json_buf_string(b, header);
            lui_json_buf_object_end(b);

            lui_json_buf_object_begin(b);
            lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
            lui_json_buf_key(b, "text"); lui_json_buf_string(b, json_str);
            lui_json_buf_object_end(b);
            free(json_str);
        }
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Error: unknown action '%s'", action);
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, msg);
        lui_json_buf_object_end(b);
    }
}

/* ---- Tool: export_svg --------------------------------------------------- */

static void tool_export_svg(const lui_json_t *args, lui_json_buf_t *b,
                             lui_mcp_t *mcp)
{
    (void)args;
    lui_recorder_t *rec = (lui_recorder_t *)mcp->recorder;
    if (!rec || rec->cmd_count == 0) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b,
            rec ? "No commands to export" : "No active recording");
        lui_json_buf_object_end(b);
        return;
    }

    int svg_len = 0;
    char *svg = lui_export_svg_string(rec, &svg_len);
    if (!svg) {
        lui_json_buf_object_begin(b);
        lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
        lui_json_buf_key(b, "text"); lui_json_buf_string(b, "Error: SVG export failed");
        lui_json_buf_object_end(b);
        return;
    }

    char header[64];
    snprintf(header, sizeof(header), "SVG export: %d commands, %d bytes",
             rec->cmd_count, svg_len);
    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b, header);
    lui_json_buf_object_end(b);

    lui_json_buf_object_begin(b);
    lui_json_buf_key(b, "type"); lui_json_buf_string(b, "text");
    lui_json_buf_key(b, "text"); lui_json_buf_string(b, svg);
    lui_json_buf_object_end(b);

    free(svg);
}

/* ---- Tool registry ------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema;  /* JSON string for inputSchema */
    void (*handler)(const lui_json_t *args, lui_json_buf_t *b, lui_mcp_t *mcp);
} tool_def_t;

static const tool_def_t tools[] = {
    {
        "inspect_tree",
        "Dump the full widget tree structure with IDs, rects, directions, and flags.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_inspect_tree,
    },
    {
        "get_widget",
        "Get detailed properties of a widget by its integer ID.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"description\":\"Widget ID\"}},\"required\":[\"id\"]}",
        tool_get_widget,
    },
    {
        "send_event",
        "Inject a UI event (mouse_down, mouse_up, mouse_move, key_down, key_up, text_input, scroll).",
        "{\"type\":\"object\",\"properties\":{"
            "\"type\":{\"type\":\"string\",\"description\":\"Event type\"},"
            "\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},"
            "\"button\":{\"type\":\"integer\",\"description\":\"Mouse button (1=left,2=middle,3=right)\"},"
            "\"clicks\":{\"type\":\"integer\",\"description\":\"Click count (1=single,2=double)\"},"
            "\"key\":{\"type\":\"integer\",\"description\":\"Key code (LUI_KEY_* constants or ASCII)\"},"
            "\"mods\":{\"type\":\"integer\",\"description\":\"Modifier bitmask: 1=Shift,2=Ctrl,4=Alt,8=Super\"},"
            "\"repeat\":{\"type\":\"boolean\",\"description\":\"Key repeat flag\"},"
            "\"text\":{\"type\":\"string\",\"description\":\"Text for text_input events\"},"
            "\"dx\":{\"type\":\"integer\",\"description\":\"Mouse move delta X\"},"
            "\"dy\":{\"type\":\"integer\",\"description\":\"Mouse move delta Y\"},"
            "\"delta_x\":{\"type\":\"number\"},\"delta_y\":{\"type\":\"number\"}"
        "},\"required\":[\"type\"]}",
        tool_send_event,
    },
    {
        "screenshot",
        "Capture the current surface as a base64-encoded image. Formats: png (default), jpg, bmp, qoi, ppm.",
        "{\"type\":\"object\",\"properties\":{"
            "\"format\":{\"type\":\"string\",\"description\":\"Image format: png, jpg, bmp, qoi, ppm\"},"
            "\"quality\":{\"type\":\"integer\",\"description\":\"JPG quality 1-100 (default 90)\"}"
        "}}",
        tool_screenshot,
    },
    {
        "get_theme",
        "Get the current theme colour values.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_get_theme,
    },
    {
        "set_theme",
        "Switch to a built-in theme by name ('flat' or '4dwm'). Widgets must be re-themed after.",
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Theme name\"}},\"required\":[\"name\"]}",
        tool_set_theme,
    },
    /* ---- Canvas debugging tools ---- */
    {
        "inspect_pixel",
        "Query the exact RGBA color at a pixel coordinate. Returns channel values and hex color.",
        "{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"type\":\"integer\",\"description\":\"X coordinate\"},"
            "\"y\":{\"type\":\"integer\",\"description\":\"Y coordinate\"}"
        "},\"required\":[\"x\",\"y\"]}",
        tool_inspect_pixel,
    },
    {
        "inspect_region",
        "Get statistics and a cropped image for a rectangular region. Returns dominant color, channel ranges, uniformity, and a base64 PNG crop.",
        "{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},"
            "\"w\":{\"type\":\"integer\"},\"h\":{\"type\":\"integer\"},"
            "\"include_image\":{\"type\":\"boolean\",\"description\":\"Include cropped PNG (default true)\"}"
        "},\"required\":[\"x\",\"y\",\"w\",\"h\"]}",
        tool_inspect_region,
    },
    {
        "canvas_state",
        "Query canvas dimensions, DPI scale, pixel format, and clip rectangle.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_canvas_state,
    },
    {
        "measure_text",
        "Measure text dimensions in pixels without rendering. Returns width, ascent, descent, line height.",
        "{\"type\":\"object\",\"properties\":{"
            "\"text\":{\"type\":\"string\",\"description\":\"UTF-8 text to measure\"},"
            "\"font_size\":{\"type\":\"integer\",\"description\":\"Font size in pixels\"},"
            "\"font_path\":{\"type\":\"string\",\"description\":\"Path to TTF/OTF file (uses default if omitted)\"}"
        "},\"required\":[\"text\",\"font_size\"]}",
        tool_measure_text,
    },
    {
        "take_snapshot",
        "Save a named copy of the current surface state for later comparison with diff_images.",
        "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Snapshot name (max 63 chars)\"}"
        "},\"required\":[\"name\"]}",
        tool_take_snapshot,
    },
    {
        "diff_images",
        "Compare two surface snapshots using SSIM/RMSE/PSNR. Returns metrics and a diff heatmap image. Use 'current' for the live surface.",
        "{\"type\":\"object\",\"properties\":{"
            "\"a\":{\"type\":\"string\",\"description\":\"Snapshot name or 'current'\"},"
            "\"b\":{\"type\":\"string\",\"description\":\"Snapshot name or 'current'\"}"
        "},\"required\":[\"a\",\"b\"]}",
        tool_diff_images,
    },
    {
        "record_drawing",
        "Control draw command recording. Actions: 'start' (begin recording), 'stop' (end and discard), 'list' (show recorded commands as JSON), 'clear' (reset), 'status' (check state). Use lui_mcp_get_recorder() in app code to dual-write draw commands.",
        "{\"type\":\"object\",\"properties\":{"
            "\"action\":{\"type\":\"string\",\"description\":\"start, stop, list, clear, or status\"}"
        "},\"required\":[\"action\"]}",
        tool_record_drawing,
    },
    {
        "export_svg",
        "Export recorded drawing commands as an SVG string. Requires an active recording with commands.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_export_svg,
    },
};

#define TOOL_COUNT ((int)(sizeof(tools) / sizeof(tools[0])))

/* ---- External interface (called from mcp_server.c) ---------------------- */

void lui_mcp_tools_list(lui_json_buf_t *out, lui_mcp_t *mcp)
{
    /* Built-in tools */
    for (int i = 0; i < TOOL_COUNT; i++) {
        lui_json_buf_object_begin(out);
        lui_json_buf_key(out, "name");
        lui_json_buf_string(out, tools[i].name);
        lui_json_buf_key(out, "description");
        lui_json_buf_string(out, tools[i].description);
        lui_json_buf_key(out, "inputSchema");
        lui_json_buf_raw(out, tools[i].input_schema, -1);
        lui_json_buf_object_end(out);
    }

    /* Custom tools */
    if (mcp) {
        for (int i = 0; i < mcp->num_custom_tools; i++) {
            lui_mcp_custom_tool_t *ct = &mcp->custom_tools[i];
            lui_json_buf_object_begin(out);
            lui_json_buf_key(out, "name");
            lui_json_buf_string(out, ct->name);
            lui_json_buf_key(out, "description");
            lui_json_buf_string(out, ct->description);
            lui_json_buf_key(out, "inputSchema");
            lui_json_buf_raw(out, ct->input_schema, -1);
            lui_json_buf_object_end(out);
        }
    }
}

void lui_mcp_tools_call(const char *name, const lui_json_t *args,
                          lui_json_buf_t *out, lui_mcp_t *mcp)
{
    /* Built-in tools */
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (strcmp(name, tools[i].name) == 0) {
            tools[i].handler(args, out, mcp);
            return;
        }
    }

    /* Custom tools */
    if (mcp) {
        for (int i = 0; i < mcp->num_custom_tools; i++) {
            lui_mcp_custom_tool_t *ct = &mcp->custom_tools[i];
            if (strcmp(name, ct->name) == 0) {
                /* Serialize args to JSON string for the callback */
                char *args_json = NULL;
                int args_len = 0;
                if (args) {
                    args_json = lui_json_serialize(args, &args_len);
                }
                if (!args_json) {
                    args_json = (char *)malloc(3);
                    if (args_json) { memcpy(args_json, "{}", 3); }
                    args_len = 2;
                }

                char *result_json = NULL;
                int result_len = 0;
                ct->handler(args_json, args_len, ct->user_data,
                            &result_json, &result_len);
                free(args_json);

                /* Inject the returned content array elements */
                if (result_json) {
                    if (result_len < 0) result_len = (int)strlen(result_json);
                    /* The handler returns a JSON array "[...]".
                     * Strip the outer brackets and inject the elements. */
                    if (result_len >= 2 &&
                        result_json[0] == '[' &&
                        result_json[result_len - 1] == ']') {
                        lui_json_buf_raw(out, result_json + 1, result_len - 2);
                    } else {
                        lui_json_buf_raw(out, result_json, result_len);
                    }
                    free(result_json);
                } else {
                    /* Handler returned nothing — emit empty text */
                    lui_json_buf_object_begin(out);
                    lui_json_buf_key(out, "type");
                    lui_json_buf_string(out, "text");
                    lui_json_buf_key(out, "text");
                    lui_json_buf_string(out, "(no output)");
                    lui_json_buf_object_end(out);
                }
                return;
            }
        }
    }

    /* Unknown tool */
    lui_json_buf_object_begin(out);
    lui_json_buf_key(out, "type"); lui_json_buf_string(out, "text");
    lui_json_buf_key(out, "text");
    char msg[64];
    snprintf(msg, sizeof(msg), "Error: unknown tool '%s'", name);
    lui_json_buf_string(out, msg);
    lui_json_buf_object_end(out);
}
