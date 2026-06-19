/*
 * src/html.c -- HTML parser for lightui
 *
 * Single-pass tokenizer + tree builder producing a DOM-like tree of
 * lui_html_node_t nodes in a pre-allocated arena.  Supports the HTML
 * subset declared in <lightui/html.h>.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <lightui/html.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- Arena allocator ---------------------------------------------------- */

static lui_html_node_t *alloc_node(lui_html_doc_t *doc, lui_html_type_t type)
{
    if (doc->node_count >= doc->node_cap)
        return NULL;
    lui_html_node_t *n = &doc->nodes[doc->node_count++];
    memset(n, 0, sizeof(*n));
    n->type = type;
    return n;
}

/* ---- Tree helpers ------------------------------------------------------- */

static void append_child(lui_html_node_t *parent, lui_html_node_t *child)
{
    child->parent = parent;
    child->next_sibling = NULL;
    if (parent->last_child) {
        parent->last_child->next_sibling = child;
        parent->last_child = child;
    } else {
        parent->first_child = child;
        parent->last_child  = child;
    }
}

/* ---- Tag lookup table --------------------------------------------------- */

typedef struct {
    const char     *name;
    lui_html_type_t type;
    int             is_void;   /* self-closing: br, hr, img */
    int             is_block;  /* block-level element */
} tag_info_t;

static const tag_info_t tag_table[] = {
    /* Block elements */
    {"p",          LUI_HTML_P,          0, 1},
    {"h1",         LUI_HTML_H1,         0, 1},
    {"h2",         LUI_HTML_H2,         0, 1},
    {"h3",         LUI_HTML_H3,         0, 1},
    {"h4",         LUI_HTML_H4,         0, 1},
    {"h5",         LUI_HTML_H5,         0, 1},
    {"h6",         LUI_HTML_H6,         0, 1},
    {"div",        LUI_HTML_DIV,        0, 1},
    {"blockquote", LUI_HTML_BLOCKQUOTE, 0, 1},
    {"pre",        LUI_HTML_PRE,        0, 1},
    {"ul",         LUI_HTML_UL,         0, 1},
    {"ol",         LUI_HTML_OL,         0, 1},
    {"li",         LUI_HTML_LI,         0, 1},
    {"hr",         LUI_HTML_HR,         1, 1},
    {"table",      LUI_HTML_TABLE,      0, 1},
    {"tr",         LUI_HTML_TR,         0, 1},
    {"td",         LUI_HTML_TD,         0, 1},
    {"th",         LUI_HTML_TH,         0, 1},
    /* Void elements */
    {"br",         LUI_HTML_BR,         1, 0},
    {"img",        LUI_HTML_IMG,        1, 0},
    /* Inline elements */
    {"span",       LUI_HTML_SPAN,       0, 0},
    {"b",          LUI_HTML_B,          0, 0},
    {"strong",     LUI_HTML_B,          0, 0},
    {"i",          LUI_HTML_I,          0, 0},
    {"em",         LUI_HTML_I,          0, 0},
    {"u",          LUI_HTML_U,          0, 0},
    {"s",          LUI_HTML_S,          0, 0},
    {"strike",     LUI_HTML_S,          0, 0},
    {"code",       LUI_HTML_CODE,       0, 0},
    {"a",          LUI_HTML_A,          0, 0},
    {"sub",        LUI_HTML_SUB,        0, 0},
    {"sup",        LUI_HTML_SUP,        0, 0},
    {NULL,         LUI_HTML_TEXT,       0, 0},
};

static const tag_info_t *lookup_tag(const char *name, int name_len)
{
    for (int i = 0; tag_table[i].name; i++) {
        const char *tn = tag_table[i].name;
        int tl = (int)strlen(tn);
        if (tl != name_len) continue;
        int match = 1;
        for (int j = 0; j < tl; j++) {
            if (tolower((unsigned char)name[j]) != tn[j]) {
                match = 0;
                break;
            }
        }
        if (match) return &tag_table[i];
    }
    return NULL;
}

static int is_inline_type(lui_html_type_t t)
{
    switch (t) {
    case LUI_HTML_SPAN: case LUI_HTML_B: case LUI_HTML_I:
    case LUI_HTML_U: case LUI_HTML_S: case LUI_HTML_CODE:
    case LUI_HTML_A: case LUI_HTML_SUB: case LUI_HTML_SUP:
        return 1;
    default:
        return 0;
    }
}

/* ---- Entity decoding ---------------------------------------------------- */

/*
 * Decode HTML entities in-place during source copy.
 * Returns the decoded length.
 */
static int decode_entities(char *dst, const char *src, int len)
{
    const char *s = src;
    const char *end = src + len;
    char *d = dst;

    while (s < end) {
        if (*s != '&') {
            *d++ = *s++;
            continue;
        }

        const char *amp = s;
        s++; /* skip '&' */

        if (s >= end) {
            *d++ = '&';
            continue;
        }

        /* Numeric entity */
        if (*s == '#') {
            s++;
            if (s >= end) { *d++ = '&'; *d++ = '#'; continue; }

            unsigned long codepoint = 0;
            int valid = 0;

            if (*s == 'x' || *s == 'X') {
                /* Hex: &#xHHH; */
                s++;
                const char *start = s;
                while (s < end && isxdigit((unsigned char)*s)) s++;
                if (s > start && s < end && *s == ';') {
                    codepoint = strtoul(start, NULL, 16);
                    s++; /* skip ';' */
                    valid = 1;
                }
            } else {
                /* Decimal: &#NNN; */
                const char *start = s;
                while (s < end && isdigit((unsigned char)*s)) s++;
                if (s > start && s < end && *s == ';') {
                    codepoint = strtoul(start, NULL, 10);
                    s++; /* skip ';' */
                    valid = 1;
                }
            }

            if (valid && codepoint > 0 && codepoint <= 0x7F) {
                *d++ = (char)codepoint;
            } else if (valid && codepoint > 0x7F && codepoint <= 0x7FF) {
                *d++ = (char)(0xC0 | (codepoint >> 6));
                *d++ = (char)(0x80 | (codepoint & 0x3F));
            } else if (valid && codepoint > 0x7FF && codepoint <= 0xFFFF) {
                *d++ = (char)(0xE0 | (codepoint >> 12));
                *d++ = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                *d++ = (char)(0x80 | (codepoint & 0x3F));
            } else if (valid && codepoint > 0xFFFF && codepoint <= 0x10FFFF) {
                *d++ = (char)(0xF0 | (codepoint >> 18));
                *d++ = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                *d++ = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                *d++ = (char)(0x80 | (codepoint & 0x3F));
            } else {
                /* Invalid or zero -- keep original text */
                s = amp;
                *d++ = *s++;
            }
            continue;
        }

        /* Named entity */
        const char *name_start = s;
        while (s < end && *s != ';' && *s != '<' && *s != '&' &&
               !isspace((unsigned char)*s))
            s++;

        if (s < end && *s == ';') {
            int nlen = (int)(s - name_start);
            s++; /* skip ';' */

            if (nlen == 3 && memcmp(name_start, "amp", 3) == 0) {
                *d++ = '&';
            } else if (nlen == 2 && memcmp(name_start, "lt", 2) == 0) {
                *d++ = '<';
            } else if (nlen == 2 && memcmp(name_start, "gt", 2) == 0) {
                *d++ = '>';
            } else if (nlen == 4 && memcmp(name_start, "quot", 4) == 0) {
                *d++ = '"';
            } else if (nlen == 4 && memcmp(name_start, "nbsp", 4) == 0) {
                *d++ = ' ';
            } else {
                /* Unknown entity -- keep as-is */
                const char *orig = amp;
                while (orig < s) *d++ = *orig++;
            }
        } else {
            /* No semicolon found, not a valid entity */
            s = amp;
            *d++ = *s++;
        }
    }

    *d = '\0';
    return (int)(d - dst);
}

/* ---- Attribute extraction ----------------------------------------------- */

/*
 * Find attribute value in a tag body.
 * tag_body points to the first character after the tag name.
 * tag_end points to '>' or '/>' at end of tag.
 * Returns pointer into tag_body for the value, sets *val_len.
 * Returns NULL if not found.
 */
static const char *find_attr(const char *tag_body, const char *tag_end,
                             const char *attr_name, int *val_len)
{
    int alen = (int)strlen(attr_name);
    const char *p = tag_body;

    while (p + alen < tag_end) {
        /* Skip whitespace */
        while (p < tag_end && isspace((unsigned char)*p)) p++;
        if (p >= tag_end) break;

        /* Check if this attribute name matches */
        int match = 1;
        for (int i = 0; i < alen; i++) {
            if (p + i >= tag_end ||
                tolower((unsigned char)p[i]) != tolower((unsigned char)attr_name[i])) {
                match = 0;
                break;
            }
        }

        if (match && p + alen < tag_end && p[alen] == '=') {
            p += alen + 1; /* skip name and '=' */
            if (p >= tag_end) return NULL;

            char quote = *p;
            if (quote == '"' || quote == '\'') {
                p++; /* skip opening quote */
                const char *val_start = p;
                while (p < tag_end && *p != quote) p++;
                *val_len = (int)(p - val_start);
                return val_start;
            } else {
                /* Unquoted attribute value */
                const char *val_start = p;
                while (p < tag_end && !isspace((unsigned char)*p) &&
                       *p != '>' && *p != '/') p++;
                *val_len = (int)(p - val_start);
                return val_start;
            }
        }

        /* Skip to next attribute (skip current attr name/value) */
        while (p < tag_end && !isspace((unsigned char)*p)) {
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (p < tag_end && *p != q) p++;
                if (p < tag_end) p++;
            } else {
                p++;
            }
        }
    }

    return NULL;
}

/* ---- Inline style parser ------------------------------------------------ */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Parse a CSS color value.  Supports:
 *   #RRGGBB, #RGB, rgb(R,G,B), named colors
 * Returns LVG_COLOR_TRANSPARENT on failure.
 */
static lvg_color_t parse_color(const char *p, const char *end)
{
    /* Skip whitespace */
    while (p < end && isspace((unsigned char)*p)) p++;

    if (p >= end) return LVG_COLOR_TRANSPARENT;

    /* #RRGGBB or #RGB */
    if (*p == '#') {
        p++;
        int digits = 0;
        const char *ds = p;
        while (p < end && isxdigit((unsigned char)*p)) { p++; digits++; }

        if (digits == 6) {
            int r = (hex_digit(ds[0]) << 4) | hex_digit(ds[1]);
            int g = (hex_digit(ds[2]) << 4) | hex_digit(ds[3]);
            int b = (hex_digit(ds[4]) << 4) | hex_digit(ds[5]);
            return LVG_COLOR_RGB(r, g, b);
        }
        if (digits == 3) {
            int r = hex_digit(ds[0]); r = (r << 4) | r;
            int g = hex_digit(ds[1]); g = (g << 4) | g;
            int b = hex_digit(ds[2]); b = (b << 4) | b;
            return LVG_COLOR_RGB(r, g, b);
        }
        return LVG_COLOR_TRANSPARENT;
    }

    /* rgb(R,G,B) */
    if (end - p >= 4 && p[0] == 'r' && p[1] == 'g' && p[2] == 'b' && p[3] == '(') {
        p += 4;
        int r = (int)strtol(p, (char **)&p, 10);
        while (p < end && (*p == ',' || isspace((unsigned char)*p))) p++;
        int g = (int)strtol(p, (char **)&p, 10);
        while (p < end && (*p == ',' || isspace((unsigned char)*p))) p++;
        int b = (int)strtol(p, (char **)&p, 10);
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (g < 0) g = 0;
        if (g > 255) g = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        return LVG_COLOR_RGB(r, g, b);
    }

    /* Named colors */
    int nlen = (int)(end - p);
    /* Trim trailing whitespace/semicolons */
    while (nlen > 0 && (isspace((unsigned char)p[nlen - 1]) ||
                        p[nlen - 1] == ';'))
        nlen--;

    struct { const char *name; int len; lvg_color_t color; } named[] = {
        {"black",   5, LVG_COLOR_RGB(0x00, 0x00, 0x00)},
        {"white",   5, LVG_COLOR_RGB(0xFF, 0xFF, 0xFF)},
        {"red",     3, LVG_COLOR_RGB(0xFF, 0x00, 0x00)},
        {"green",   5, LVG_COLOR_RGB(0x00, 0x80, 0x00)},
        {"blue",    4, LVG_COLOR_RGB(0x00, 0x00, 0xFF)},
        {"yellow",  6, LVG_COLOR_RGB(0xFF, 0xFF, 0x00)},
        {"cyan",    4, LVG_COLOR_RGB(0x00, 0xFF, 0xFF)},
        {"magenta", 7, LVG_COLOR_RGB(0xFF, 0x00, 0xFF)},
        {"orange",  6, LVG_COLOR_RGB(0xFF, 0xA5, 0x00)},
        {"purple",  6, LVG_COLOR_RGB(0x80, 0x00, 0x80)},
        {"gray",    4, LVG_COLOR_RGB(0x80, 0x80, 0x80)},
        {"grey",    4, LVG_COLOR_RGB(0x80, 0x80, 0x80)},
        {"silver",  6, LVG_COLOR_RGB(0xC0, 0xC0, 0xC0)},
        {"maroon",  6, LVG_COLOR_RGB(0x80, 0x00, 0x00)},
        {"navy",    4, LVG_COLOR_RGB(0x00, 0x00, 0x80)},
        {"teal",    4, LVG_COLOR_RGB(0x00, 0x80, 0x80)},
        {"olive",   5, LVG_COLOR_RGB(0x80, 0x80, 0x00)},
        {"lime",    4, LVG_COLOR_RGB(0x00, 0xFF, 0x00)},
        {"aqua",    4, LVG_COLOR_RGB(0x00, 0xFF, 0xFF)},
        {"fuchsia", 7, LVG_COLOR_RGB(0xFF, 0x00, 0xFF)},
    };

    for (int i = 0; i < (int)(sizeof(named) / sizeof(named[0])); i++) {
        if (nlen != named[i].len) continue;
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            if (tolower((unsigned char)p[j]) != named[i].name[j]) {
                match = 0;
                break;
            }
        }
        if (match) return named[i].color;
    }

    return LVG_COLOR_TRANSPARENT;
}

/*
 * Parse the value of a style="" attribute into lui_html_inline_style_t.
 */
static void parse_inline_style(lui_html_inline_style_t *style,
                               const char *val, int val_len)
{
    const char *p = val;
    const char *end = val + val_len;

    while (p < end) {
        /* Skip whitespace */
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) break;

        /* Read property name */
        const char *prop_start = p;
        while (p < end && *p != ':' && *p != ';') p++;
        if (p >= end || *p != ':') { p++; continue; }

        int prop_len = (int)(p - prop_start);
        /* Trim trailing spaces from property name */
        while (prop_len > 0 && isspace((unsigned char)prop_start[prop_len - 1]))
            prop_len--;

        p++; /* skip ':' */

        /* Skip whitespace after colon */
        while (p < end && isspace((unsigned char)*p)) p++;

        /* Read value until ';' or end */
        const char *val_start = p;
        while (p < end && *p != ';') p++;
        int vlen = (int)(p - val_start);
        /* Trim trailing whitespace from value */
        while (vlen > 0 && isspace((unsigned char)val_start[vlen - 1]))
            vlen--;

        if (p < end && *p == ';') p++;

        /* Match properties (case-insensitive comparison for property names) */
        #define PROP_IS(s) (prop_len == (int)sizeof(s) - 1 && \
            strncmp(prop_start, s, (size_t)prop_len) == 0)
        #define VAL_IS(s) (vlen == (int)sizeof(s) - 1 && \
            strncmp(val_start, s, (size_t)vlen) == 0)

        if (PROP_IS("color")) {
            lvg_color_t c = parse_color(val_start, val_start + vlen);
            if (c != LVG_COLOR_TRANSPARENT) style->color = c;
        } else if (PROP_IS("background-color")) {
            lvg_color_t c = parse_color(val_start, val_start + vlen);
            if (c != LVG_COLOR_TRANSPARENT) style->background_color = c;
        } else if (PROP_IS("font-weight")) {
            if (VAL_IS("bold"))        style->font_weight = 2;
            else if (VAL_IS("normal")) style->font_weight = 1;
        } else if (PROP_IS("font-style")) {
            if (VAL_IS("italic"))      style->font_style = 2;
            else if (VAL_IS("normal")) style->font_style = 1;
        } else if (PROP_IS("text-decoration")) {
            if (VAL_IS("none"))           style->text_decoration = 0;
            else if (VAL_IS("underline"))    style->text_decoration = 1;
            else if (VAL_IS("line-through")) style->text_decoration = 2;
        } else if (PROP_IS("text-align")) {
            if (VAL_IS("left"))        style->text_align = 1;
            else if (VAL_IS("center")) style->text_align = 2;
            else if (VAL_IS("right"))  style->text_align = 3;
        }

        #undef PROP_IS
        #undef VAL_IS
    }
}

/* ---- Tokenizer + tree builder ------------------------------------------- */

/* Check if we are inside a <pre> ancestor */
static int is_inside_pre(lui_html_node_t **stack, int stack_depth)
{
    for (int i = 0; i < stack_depth; i++) {
        if (stack[i]->type == LUI_HTML_PRE) return 1;
    }
    return 0;
}

int lui_html_parse(lui_html_doc_t *doc, const char *src, int len)
{
    memset(doc, 0, sizeof(*doc));

    if (!src || len <= 0) {
        doc->node_cap = 2;
        doc->nodes = (lui_html_node_t *)calloc((size_t)doc->node_cap,
                                                sizeof(lui_html_node_t));
        if (!doc->nodes) return -1;
        doc->source = (char *)calloc(1, 1);
        if (!doc->source) { free(doc->nodes); return -1; }
        doc->source_len = 0;
        doc->root = alloc_node(doc, LUI_HTML_DOCUMENT);
        return 0;
    }

    /* Copy source with entity decoding */
    doc->source = (char *)malloc((size_t)len + 1);
    if (!doc->source) return -1;
    doc->source_len = decode_entities(doc->source, src, len);

    /* Pre-allocate arena */
    doc->node_cap = doc->source_len / 3 + 32;
    doc->nodes = (lui_html_node_t *)calloc((size_t)doc->node_cap,
                                            sizeof(lui_html_node_t));
    if (!doc->nodes) {
        free(doc->source);
        doc->source = NULL;
        return -1;
    }

    /* Root document node */
    lui_html_node_t *root = alloc_node(doc, LUI_HTML_DOCUMENT);
    doc->root = root;

    /* Element stack for open elements */
    #define MAX_DEPTH 64
    lui_html_node_t *stack[MAX_DEPTH];
    int stack_depth = 0;
    stack[stack_depth++] = root;

    const char *p = doc->source;
    const char *s_end = doc->source + doc->source_len;
    const char *text_start = p; /* start of current text run */

    /* Helper: current parent is top of stack */
    #define CURRENT_PARENT() (stack[stack_depth - 1])

    /* Helper: flush accumulated text as a TEXT node */
    #define FLUSH_TEXT() do {                                            \
        if (text_start < p) {                                           \
            int in_pre = is_inside_pre(stack, stack_depth);             \
            if (in_pre) {                                               \
                /* Keep text as-is inside <pre> */                      \
                int tlen = (int)(p - text_start);                       \
                if (tlen > 0) {                                         \
                    lui_html_node_t *tn = alloc_node(doc, LUI_HTML_TEXT); \
                    if (tn) {                                           \
                        tn->text = text_start;                          \
                        tn->text_len = tlen;                            \
                        append_child(CURRENT_PARENT(), tn);             \
                    }                                                   \
                }                                                       \
            } else {                                                    \
                /* Collapse whitespace */                               \
                const char *ts = text_start;                            \
                const char *te = p;                                     \
                /* We'll write collapsed text in-place in source */     \
                char *wp = (char *)ts;                                  \
                int in_ws = 0;                                          \
                int out_len = 0;                                        \
                for (const char *r = ts; r < te; r++) {                 \
                    if (*r == ' ' || *r == '\t' || *r == '\n' ||        \
                        *r == '\r') {                                   \
                        if (!in_ws) { wp[out_len++] = ' '; in_ws = 1; }\
                    } else {                                            \
                        wp[out_len++] = *r;                             \
                        in_ws = 0;                                      \
                    }                                                   \
                }                                                       \
                /* Skip text that is only whitespace */                 \
                if (out_len > 0 &&                                      \
                    !(out_len == 1 && wp[0] == ' ')) {                  \
                    lui_html_node_t *tn = alloc_node(doc, LUI_HTML_TEXT); \
                    if (tn) {                                           \
                        tn->text = wp;                                  \
                        tn->text_len = out_len;                         \
                        append_child(CURRENT_PARENT(), tn);             \
                    }                                                   \
                }                                                       \
            }                                                           \
        }                                                               \
        text_start = NULL;                                              \
    } while (0)

    while (p < s_end) {
        if (*p == '<') {
            /* Flush any pending text */
            FLUSH_TEXT();

            p++; /* skip '<' */
            if (p >= s_end) break;

            /* HTML comment: <!-- ... --> */
            if (p + 2 < s_end && p[0] == '!' && p[1] == '-' && p[2] == '-') {
                p += 3;
                /* Scan for --> */
                while (p + 2 < s_end) {
                    if (p[0] == '-' && p[1] == '-' && p[2] == '>') {
                        p += 3;
                        break;
                    }
                    p++;
                }
                text_start = p;
                continue;
            }

            /* DOCTYPE or other <! */
            if (*p == '!') {
                while (p < s_end && *p != '>') p++;
                if (p < s_end) p++; /* skip '>' */
                text_start = p;
                continue;
            }

            /* Close tag: </tagname> */
            if (*p == '/') {
                p++; /* skip '/' */
                /* Read tag name */
                const char *tag_start = p;
                while (p < s_end && *p != '>' && !isspace((unsigned char)*p))
                    p++;
                int tag_len = (int)(p - tag_start);

                /* Skip to '>' */
                while (p < s_end && *p != '>') p++;
                if (p < s_end) p++; /* skip '>' */

                if (tag_len > 0) {
                    const tag_info_t *info = lookup_tag(tag_start, tag_len);
                    if (info) {
                        /* Find matching open element on stack and close
                         * everything above it */
                        for (int i = stack_depth - 1; i > 0; i--) {
                            if (stack[i]->type == info->type) {
                                stack_depth = i;
                                break;
                            }
                        }
                    }
                }

                text_start = p;
                continue;
            }

            /* Open tag: <tagname ...> or self-closing <tagname ... /> */
            const char *tag_start = p;
            while (p < s_end && *p != '>' && *p != '/' &&
                   !isspace((unsigned char)*p))
                p++;
            int tag_len = (int)(p - tag_start);

            /* Body of tag (attributes) */
            const char *tag_body = p;

            /* Find end of tag */
            int self_closing = 0;
            while (p < s_end && *p != '>') {
                if (*p == '/' && p + 1 < s_end && p[1] == '>') {
                    self_closing = 1;
                    break;
                }
                p++;
            }
            const char *tag_end = p;

            if (p < s_end) {
                if (self_closing) p += 2; /* skip '/>' */
                else p++; /* skip '>' */
            }

            if (tag_len <= 0) {
                text_start = p;
                continue;
            }

            const tag_info_t *info = lookup_tag(tag_start, tag_len);
            if (!info) {
                /* Unknown tag -- skip it */
                text_start = p;
                continue;
            }

            /* Auto-close logic for certain elements */
            lui_html_type_t new_type = info->type;

            /* <li> auto-closes previous <li> */
            if (new_type == LUI_HTML_LI) {
                if (stack_depth > 1 &&
                    CURRENT_PARENT()->type == LUI_HTML_LI)
                    stack_depth--;
            }

            /* <td>/<th> auto-close previous <td>/<th> */
            if (new_type == LUI_HTML_TD || new_type == LUI_HTML_TH) {
                if (stack_depth > 1 &&
                    (CURRENT_PARENT()->type == LUI_HTML_TD ||
                     CURRENT_PARENT()->type == LUI_HTML_TH))
                    stack_depth--;
            }

            /* <tr> auto-closes previous <tr> */
            if (new_type == LUI_HTML_TR) {
                if (stack_depth > 1 &&
                    CURRENT_PARENT()->type == LUI_HTML_TR)
                    stack_depth--;
            }

            /* Block elements auto-close open inline elements */
            if (info->is_block) {
                while (stack_depth > 1 &&
                       is_inline_type(CURRENT_PARENT()->type))
                    stack_depth--;
            }

            /* Create the node */
            lui_html_node_t *node = alloc_node(doc, new_type);
            if (!node) {
                text_start = p;
                continue;
            }

            /* Parse attributes */
            int attr_len;
            const char *attr_val;

            /* href */
            attr_val = find_attr(tag_body, tag_end, "href", &attr_len);
            if (attr_val) {
                node->href = attr_val;
                node->href_len = attr_len;
            }

            /* src */
            attr_val = find_attr(tag_body, tag_end, "src", &attr_len);
            if (attr_val) {
                node->src_url = attr_val;
                node->src_url_len = attr_len;
            }

            /* alt */
            attr_val = find_attr(tag_body, tag_end, "alt", &attr_len);
            if (attr_val) {
                node->alt = attr_val;
                node->alt_len = attr_len;
            }

            /* start (for <ol>) */
            attr_val = find_attr(tag_body, tag_end, "start", &attr_len);
            if (attr_val) {
                node->ol_start = (int)strtol(attr_val, NULL, 10);
            }

            /* style */
            attr_val = find_attr(tag_body, tag_end, "style", &attr_len);
            if (attr_val) {
                parse_inline_style(&node->style, attr_val, attr_len);
            }

            /* Append to current parent */
            append_child(CURRENT_PARENT(), node);

            /* Push to stack unless void or self-closing */
            if (!info->is_void && !self_closing) {
                if (stack_depth < MAX_DEPTH) {
                    stack[stack_depth++] = node;
                }
            }

            text_start = p;
            continue;
        }

        /* Regular character -- part of text content */
        if (!text_start) text_start = p;
        p++;
    }

    /* Flush any remaining text */
    if (text_start && text_start < p) {
        FLUSH_TEXT();
    }

    #undef FLUSH_TEXT
    #undef CURRENT_PARENT
    #undef MAX_DEPTH

    return 0;
}

/* ---- Destroy ------------------------------------------------------------ */

void lui_html_doc_destroy(lui_html_doc_t *doc)
{
    if (!doc) return;
    free(doc->nodes);
    free(doc->source);
    memset(doc, 0, sizeof(*doc));
}

/* ---- Style defaults ----------------------------------------------------- */

void lui_html_style_default(lui_html_style_t *style)
{
    if (!style) return;
    memset(style, 0, sizeof(*style));

    style->text_color           = LVG_COLOR_RGB(0xE0, 0xE0, 0xE0);
    style->heading_color        = LVG_COLOR_RGB(0xFF, 0xFF, 0xFF);
    style->link_color           = LVG_COLOR_RGB(0x6E, 0xB5, 0xFF);
    style->code_fg              = LVG_COLOR_RGB(0xE0, 0xD0, 0xB0);
    style->code_bg              = LVG_COLOR_ARGB(0xFF, 0x2A, 0x2A, 0x2A);
    style->blockquote_fg        = LVG_COLOR_RGB(0xA0, 0xA0, 0xA0);
    style->blockquote_bar_color = LVG_COLOR_RGB(0x50, 0x50, 0x50);
    style->hr_color             = LVG_COLOR_RGB(0x40, 0x40, 0x40);
    style->table_border_color   = LVG_COLOR_RGB(0x50, 0x50, 0x50);
    style->th_bg                = LVG_COLOR_ARGB(0xFF, 0x30, 0x30, 0x30);

    style->blockquote_bar_width  = 3;
    style->blockquote_indent     = 16;
    style->list_indent           = 16;
    style->code_block_padding    = 6;
    style->paragraph_spacing     = 10;
    style->heading_spacing_above = 6;
    style->heading_spacing_below = 4;
    style->table_cell_padding    = 4;
    style->table_border_width    = 1;
}
