/*
 * src/markdown.c — Markdown parser for lightui
 *
 * Two-pass parser:
 *   Pass 1: Block structure (headings, code blocks, lists, etc.)
 *   Pass 2: Inline parsing (bold, italic, code spans, links, images)
 *
 * Uses a pre-allocated node arena.  Node count is bounded by source length
 * (each node needs at least 1 char of markup), so allocating len/2 + 16
 * nodes upfront avoids realloc and keeps pointers stable.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <lightui/markdown.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- Arena allocator ----------------------------------------------------- */

static lui_md_node_t *alloc_node(lui_md_doc_t *doc, lui_md_type_t type)
{
    if (doc->node_count >= doc->node_cap)
        return NULL;
    lui_md_node_t *n = &doc->nodes[doc->node_count++];
    memset(n, 0, sizeof(*n));
    n->type = type;
    return n;
}

/* ---- Tree helpers -------------------------------------------------------- */

static void append_child(lui_md_node_t *parent, lui_md_node_t *child)
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

/* ---- Line helpers -------------------------------------------------------- */

/* Advance past a single line, returning pointer to start of next line.
 * Sets *line_end to the end of content (before \n or \r\n). */
static const char *next_line(const char *p, const char *src_end,
                             const char **line_end)
{
    const char *le = p;
    while (le < src_end && *le != '\n' && *le != '\r') le++;
    *line_end = le;
    const char *next = le;
    if (next < src_end && *next == '\r') next++;
    if (next < src_end && *next == '\n') next++;
    return next;
}

static int is_blank_line(const char *start, const char *end)
{
    while (start < end) {
        if (*start != ' ' && *start != '\t') return 0;
        start++;
    }
    return 1;
}

/* Count leading '#' characters, return heading level (0 if not a heading). */
static int heading_level(const char *start, const char *end)
{
    int level = 0;
    const char *p = start;
    while (p < end && *p == '#' && level < 6) { level++; p++; }
    if (level == 0) return 0;
    /* Must be followed by space or end of line. */
    if (p < end && *p != ' ' && *p != '\t') return 0;
    return level;
}

/* Check for fenced code block opener: ``` or ~~~ (3+ chars). */
static int is_fence(const char *start, const char *end, char *fence_char)
{
    const char *p = start;
    /* Skip leading spaces (up to 3). */
    int spaces = 0;
    while (p < end && *p == ' ' && spaces < 3) { p++; spaces++; }
    if (p >= end) return 0;
    if (*p != '`' && *p != '~') return 0;
    char fc = *p;
    int count = 0;
    while (p < end && *p == fc) { count++; p++; }
    if (count < 3) return 0;
    *fence_char = fc;
    return count;
}

/* Check for thematic break: ---, ***, ___ (3+ of same char, optional spaces). */
static int is_thematic_break(const char *start, const char *end)
{
    const char *p = start;
    /* Skip leading spaces (up to 3). */
    int spaces = 0;
    while (p < end && *p == ' ' && spaces < 3) { p++; spaces++; }
    if (p >= end) return 0;
    char c = *p;
    if (c != '-' && c != '*' && c != '_') return 0;
    int count = 0;
    while (p < end) {
        if (*p == c) count++;
        else if (*p != ' ' && *p != '\t') return 0;
        p++;
    }
    return count >= 3;
}

/* Check for unordered list marker: "- ", "* ", "+ " */
static int is_unordered_list(const char *start, const char *end,
                             const char **content_start)
{
    const char *p = start;
    /* Skip up to 3 leading spaces. */
    int spaces = 0;
    while (p < end && *p == ' ' && spaces < 3) { p++; spaces++; }
    if (p >= end) return 0;
    if (*p != '-' && *p != '*' && *p != '+') return 0;
    p++;
    if (p >= end || *p != ' ') return 0;
    p++; /* skip the space */
    *content_start = p;
    return 1;
}

/* Check for ordered list marker: "1. ", "2. " etc. */
static int is_ordered_list(const char *start, const char *end,
                           int *start_num, const char **content_start)
{
    const char *p = start;
    int spaces = 0;
    while (p < end && *p == ' ' && spaces < 3) { p++; spaces++; }
    if (p >= end || !isdigit((unsigned char)*p)) return 0;
    int num = 0;
    while (p < end && isdigit((unsigned char)*p)) {
        num = num * 10 + (*p - '0');
        p++;
    }
    if (p >= end || *p != '.') return 0;
    p++;
    if (p >= end || *p != ' ') return 0;
    p++;
    *start_num = num;
    *content_start = p;
    return 1;
}

/* Check for blockquote: "> " */
static int is_blockquote(const char *start, const char *end,
                         const char **content_start)
{
    const char *p = start;
    int spaces = 0;
    while (p < end && *p == ' ' && spaces < 3) { p++; spaces++; }
    if (p >= end || *p != '>') return 0;
    p++;
    if (p < end && *p == ' ') p++;
    *content_start = p;
    return 1;
}

/* ---- Pass 2: Inline parsing --------------------------------------------- */

/* Create a TEXT node with text pointing into doc->source. */
static lui_md_node_t *make_text_node(lui_md_doc_t *doc,
                                     const char *text, int len)
{
    if (len <= 0) return NULL;
    lui_md_node_t *n = alloc_node(doc, LUI_MD_TEXT);
    if (!n) return NULL;
    n->text     = text;
    n->text_len = len;
    return n;
}

static void parse_inlines(lui_md_doc_t *doc, lui_md_node_t *parent,
                          const char *text, int len);

/* Find closing delimiter matching 'delim' of length 'dlen' starting from p.
 * Returns pointer to the start of the closing delimiter, or NULL. */
static const char *find_closing(const char *p, const char *end,
                                char delim, int dlen)
{
    while (p + dlen <= end) {
        if (*p == '\\' && p + 1 < end) { p += 2; continue; }
        int match = 1;
        for (int i = 0; i < dlen; i++) {
            if (p[i] != delim) { match = 0; break; }
        }
        if (match) {
            /* Make sure it's not a longer run (e.g., *** when we want **). */
            if (dlen == 2 && p + 2 < end && p[2] == delim) { p++; continue; }
            if (dlen == 1 && p + 1 < end && p[1] == delim) { p++; continue; }
            return p;
        }
        p++;
    }
    return NULL;
}

static void parse_inlines(lui_md_doc_t *doc, lui_md_node_t *parent,
                          const char *text, int len)
{
    const char *p   = text;
    const char *end = text + len;
    const char *run_start = p; /* start of current plain text run */

    while (p < end) {
        /* Hard break: two trailing spaces before newline. */
        if (p + 2 < end && p[0] == ' ' && p[1] == ' ') {
            const char *sp = p + 2;
            while (sp < end && *sp == ' ') sp++;
            if (sp < end && *sp == '\n') {
                /* Flush text before the spaces. */
                if (p > run_start) {
                    lui_md_node_t *tn = make_text_node(doc, run_start,
                                                       (int)(p - run_start));
                    if (tn) append_child(parent, tn);
                }
                lui_md_node_t *br = alloc_node(doc, LUI_MD_HARDBREAK);
                if (br) append_child(parent, br);
                p = sp + 1;
                run_start = p;
                continue;
            }
        }

        /* Soft break: single newline. */
        if (*p == '\n') {
            if (p > run_start) {
                lui_md_node_t *tn = make_text_node(doc, run_start,
                                                   (int)(p - run_start));
                if (tn) append_child(parent, tn);
            }
            lui_md_node_t *sb = alloc_node(doc, LUI_MD_SOFTBREAK);
            if (sb) append_child(parent, sb);
            p++;
            run_start = p;
            continue;
        }

        /* Backslash escape. */
        if (*p == '\\' && p + 1 < end) {
            /* Flush text before backslash. */
            if (p > run_start) {
                lui_md_node_t *tn = make_text_node(doc, run_start,
                                                   (int)(p - run_start));
                if (tn) append_child(parent, tn);
            }
            /* The escaped character becomes a text node. */
            lui_md_node_t *tn = make_text_node(doc, p + 1, 1);
            if (tn) append_child(parent, tn);
            p += 2;
            run_start = p;
            continue;
        }

        /* Code span: `code` */
        if (*p == '`') {
            const char *close = memchr(p + 1, '`', (size_t)(end - p - 1));
            if (close) {
                if (p > run_start) {
                    lui_md_node_t *tn = make_text_node(doc, run_start,
                                                       (int)(p - run_start));
                    if (tn) append_child(parent, tn);
                }
                lui_md_node_t *cs = alloc_node(doc, LUI_MD_CODE_SPAN);
                if (cs) {
                    cs->text     = p + 1;
                    cs->text_len = (int)(close - p - 1);
                    append_child(parent, cs);
                }
                p = close + 1;
                run_start = p;
                continue;
            }
        }

        /* Strong: **text** or __text__ */
        if ((p + 1 < end) &&
            ((*p == '*' && p[1] == '*') || (*p == '_' && p[1] == '_'))) {
            char delim = *p;
            const char *close = find_closing(p + 2, end, delim, 2);
            if (close) {
                if (p > run_start) {
                    lui_md_node_t *tn = make_text_node(doc, run_start,
                                                       (int)(p - run_start));
                    if (tn) append_child(parent, tn);
                }
                lui_md_node_t *strong = alloc_node(doc, LUI_MD_STRONG);
                if (strong) {
                    append_child(parent, strong);
                    parse_inlines(doc, strong, p + 2,
                                  (int)(close - p - 2));
                }
                p = close + 2;
                run_start = p;
                continue;
            }
        }

        /* Emphasis: *text* or _text_ */
        if (*p == '*' || *p == '_') {
            char delim = *p;
            /* Make sure it's not ** (handled above). */
            if (!(p + 1 < end && p[1] == delim)) {
                const char *close = find_closing(p + 1, end, delim, 1);
                if (close) {
                    if (p > run_start) {
                        lui_md_node_t *tn = make_text_node(doc, run_start,
                                                           (int)(p - run_start));
                        if (tn) append_child(parent, tn);
                    }
                    lui_md_node_t *em = alloc_node(doc, LUI_MD_EMPHASIS);
                    if (em) {
                        append_child(parent, em);
                        parse_inlines(doc, em, p + 1,
                                      (int)(close - p - 1));
                    }
                    p = close + 1;
                    run_start = p;
                    continue;
                }
            }
        }

        /* Image: ![alt](url) */
        if (*p == '!' && p + 1 < end && p[1] == '[') {
            const char *alt_start = p + 2;
            const char *rb = memchr(alt_start, ']',
                                    (size_t)(end - alt_start));
            if (rb && rb + 1 < end && rb[1] == '(') {
                const char *url_start = rb + 2;
                const char *rp = memchr(url_start, ')',
                                        (size_t)(end - url_start));
                if (rp) {
                    if (p > run_start) {
                        lui_md_node_t *tn = make_text_node(doc, run_start,
                                                           (int)(p - run_start));
                        if (tn) append_child(parent, tn);
                    }
                    lui_md_node_t *img = alloc_node(doc, LUI_MD_IMAGE);
                    if (img) {
                        img->text     = alt_start;
                        img->text_len = (int)(rb - alt_start);
                        img->url      = url_start;
                        img->url_len  = (int)(rp - url_start);
                        append_child(parent, img);
                    }
                    p = rp + 1;
                    run_start = p;
                    continue;
                }
            }
        }

        /* Link: [text](url) */
        if (*p == '[') {
            const char *rb = memchr(p + 1, ']', (size_t)(end - p - 1));
            if (rb && rb + 1 < end && rb[1] == '(') {
                const char *url_start = rb + 2;
                const char *rp = memchr(url_start, ')',
                                        (size_t)(end - url_start));
                if (rp) {
                    if (p > run_start) {
                        lui_md_node_t *tn = make_text_node(doc, run_start,
                                                           (int)(p - run_start));
                        if (tn) append_child(parent, tn);
                    }
                    lui_md_node_t *link = alloc_node(doc, LUI_MD_LINK);
                    if (link) {
                        link->url     = url_start;
                        link->url_len = (int)(rp - url_start);
                        append_child(parent, link);
                        parse_inlines(doc, link, p + 1,
                                      (int)(rb - p - 1));
                    }
                    p = rp + 1;
                    run_start = p;
                    continue;
                }
            }
        }

        /* Regular character — extend the current text run. */
        p++;
    }

    /* Flush remaining text. */
    if (p > run_start) {
        lui_md_node_t *tn = make_text_node(doc, run_start,
                                           (int)(p - run_start));
        if (tn) append_child(parent, tn);
    }
}

/* ---- Pass 1: Block structure -------------------------------------------- */

int lui_md_parse(lui_md_doc_t *doc, const char *src, int len)
{
    memset(doc, 0, sizeof(*doc));

    if (!src || len <= 0) {
        /* Empty document. */
        doc->node_cap = 2;
        doc->nodes = (lui_md_node_t *)calloc((size_t)doc->node_cap,
                                              sizeof(lui_md_node_t));
        if (!doc->nodes) return -1;
        doc->source = (char *)calloc(1, 1);
        if (!doc->source) { free(doc->nodes); return -1; }
        doc->source_len = 0;
        doc->root = alloc_node(doc, LUI_MD_DOCUMENT);
        return 0;
    }

    /* Copy source. */
    doc->source = (char *)malloc((size_t)len + 1);
    if (!doc->source) return -1;
    memcpy(doc->source, src, (size_t)len);
    doc->source[len] = '\0';
    doc->source_len  = len;

    /* Pre-allocate arena. */
    doc->node_cap = len / 2 + 16;
    doc->nodes = (lui_md_node_t *)calloc((size_t)doc->node_cap,
                                          sizeof(lui_md_node_t));
    if (!doc->nodes) {
        free(doc->source);
        doc->source = NULL;
        return -1;
    }

    /* Root document node. */
    lui_md_node_t *root = alloc_node(doc, LUI_MD_DOCUMENT);
    doc->root = root;

    const char *p   = doc->source;
    const char *s_end = doc->source + len;

    /* Current paragraph accumulator. */
    const char *para_start = NULL;
    const char *para_end   = NULL;

    /* Flush accumulated paragraph text into a PARAGRAPH node with inlines. */
    #define FLUSH_PARA()                                                    \
        do {                                                                \
            if (para_start && para_end > para_start) {                      \
                lui_md_node_t *pn = alloc_node(doc, LUI_MD_PARAGRAPH);      \
                if (pn) {                                                   \
                    append_child(root, pn);                                 \
                    parse_inlines(doc, pn, para_start,                      \
                                  (int)(para_end - para_start));            \
                }                                                           \
            }                                                               \
            para_start = para_end = NULL;                                   \
        } while (0)

    while (p < s_end) {
        const char *line_end;
        const char *next = next_line(p, s_end, &line_end);

        /* Blank line ends current paragraph. */
        if (is_blank_line(p, line_end)) {
            FLUSH_PARA();
            p = next;
            continue;
        }

        /* Thematic break: ---, ***, ___. */
        if (is_thematic_break(p, line_end)) {
            FLUSH_PARA();
            lui_md_node_t *tb = alloc_node(doc, LUI_MD_THEMATIC_BREAK);
            if (tb) append_child(root, tb);
            p = next;
            continue;
        }

        /* Heading. */
        {
            int level = heading_level(p, line_end);
            if (level > 0) {
                FLUSH_PARA();
                lui_md_node_t *h = alloc_node(doc, LUI_MD_HEADING);
                if (h) {
                    h->heading_level = level;
                    append_child(root, h);
                    /* Content after "# ". */
                    const char *content = p + level;
                    if (content < line_end && *content == ' ') content++;
                    /* Strip trailing '#' and spaces. */
                    const char *ce = line_end;
                    while (ce > content && (ce[-1] == '#' || ce[-1] == ' '))
                        ce--;
                    parse_inlines(doc, h, content, (int)(ce - content));
                }
                p = next;
                continue;
            }
        }

        /* Fenced code block. */
        {
            char fence_char = 0;
            int fence_len = is_fence(p, line_end, &fence_char);
            if (fence_len > 0) {
                FLUSH_PARA();
                lui_md_node_t *cb = alloc_node(doc, LUI_MD_CODE_BLOCK);
                if (cb) {
                    append_child(root, cb);
                    /* Collect lines until closing fence. */
                    const char *code_start = next;
                    const char *code_end = code_start;
                    const char *scan = next;
                    while (scan < s_end) {
                        const char *cle;
                        const char *cnext = next_line(scan, s_end, &cle);
                        /* Check for closing fence. */
                        char fc2 = 0;
                        int fl2 = is_fence(scan, cle, &fc2);
                        if (fl2 >= fence_len && fc2 == fence_char) {
                            code_end = scan;
                            scan = cnext;
                            break;
                        }
                        code_end = cnext;
                        scan = cnext;
                    }
                    /* Remove trailing newline from code content. */
                    const char *ce = code_end;
                    if (ce > code_start && ce[-1] == '\n') ce--;
                    if (ce > code_start && ce[-1] == '\r') ce--;
                    cb->text     = code_start;
                    cb->text_len = (int)(ce - code_start);
                    p = scan;
                }
                continue;
            }
        }

        /* Blockquote. */
        {
            const char *bq_content;
            if (is_blockquote(p, line_end, &bq_content)) {
                FLUSH_PARA();
                lui_md_node_t *bq = alloc_node(doc, LUI_MD_BLOCKQUOTE);
                if (bq) {
                    append_child(root, bq);
                    /* Collect content from this and continuation lines. */
                    const char *content_start = bq_content;
                    int content_len = (int)(line_end - bq_content);

                    /* Scan continuation lines. */
                    const char *scan = next;
                    while (scan < s_end) {
                        const char *cle;
                        const char *cnext = next_line(scan, s_end, &cle);
                        const char *cont;
                        if (is_blockquote(scan, cle, &cont)) {
                            scan = cnext;
                        } else {
                            break;
                        }
                    }

                    /* For simplicity, just parse the first line's inline
                     * content.  Multi-line blockquotes get the combined
                     * text from first > marker to end of contiguous > lines. */
                    /* Re-gather all blockquote content. */
                    /* Build combined content: walk again from start. */
                    /* Actually, let's just parse the content between
                     * first bq_content and the end of contiguous bq lines. */
                    const char *s2 = next;
                    while (s2 < s_end) {
                        const char *cle2;
                        const char *cnext2 = next_line(s2, s_end, &cle2);
                        const char *cont2;
                        if (is_blockquote(s2, cle2, &cont2)) {
                            s2 = cnext2;
                        } else {
                            break;
                        }
                    }
                    (void)content_start;
                    (void)content_len;

                    /* Parse inline content. For multi-line blockquotes,
                     * we parse each line separately as a paragraph child. */
                    {
                        const char *qs = p;
                        while (qs < s2) {
                            const char *qle;
                            const char *qnext = next_line(qs, s_end, &qle);
                            const char *qcont;
                            if (is_blockquote(qs, qle, &qcont)) {
                                lui_md_node_t *bp = alloc_node(doc, LUI_MD_PARAGRAPH);
                                if (bp) {
                                    append_child(bq, bp);
                                    parse_inlines(doc, bp, qcont,
                                                  (int)(qle - qcont));
                                }
                            }
                            qs = qnext;
                        }
                    }
                    p = s2;
                }
                continue;
            }
        }

        /* Unordered list. */
        {
            const char *item_content;
            if (is_unordered_list(p, line_end, &item_content)) {
                FLUSH_PARA();
                lui_md_node_t *list = alloc_node(doc, LUI_MD_LIST);
                if (list) {
                    list->list_ordered = false;
                    list->list_start   = 0;
                    append_child(root, list);

                    /* First item. */
                    lui_md_node_t *item = alloc_node(doc, LUI_MD_LIST_ITEM);
                    if (item) {
                        append_child(list, item);
                        parse_inlines(doc, item, item_content,
                                      (int)(line_end - item_content));
                    }
                    p = next;

                    /* Continuation items. */
                    while (p < s_end) {
                        const char *cle;
                        const char *cnext = next_line(p, s_end, &cle);
                        const char *ic;
                        if (is_unordered_list(p, cle, &ic)) {
                            item = alloc_node(doc, LUI_MD_LIST_ITEM);
                            if (item) {
                                append_child(list, item);
                                parse_inlines(doc, item, ic,
                                              (int)(cle - ic));
                            }
                            p = cnext;
                        } else {
                            break;
                        }
                    }
                }
                continue;
            }
        }

        /* Ordered list. */
        {
            int start_num;
            const char *item_content;
            if (is_ordered_list(p, line_end, &start_num, &item_content)) {
                FLUSH_PARA();
                lui_md_node_t *list = alloc_node(doc, LUI_MD_LIST);
                if (list) {
                    list->list_ordered = true;
                    list->list_start   = start_num;
                    append_child(root, list);

                    lui_md_node_t *item = alloc_node(doc, LUI_MD_LIST_ITEM);
                    if (item) {
                        append_child(list, item);
                        parse_inlines(doc, item, item_content,
                                      (int)(line_end - item_content));
                    }
                    p = next;

                    while (p < s_end) {
                        const char *cle;
                        const char *cnext = next_line(p, s_end, &cle);
                        int sn2;
                        const char *ic2;
                        if (is_ordered_list(p, cle, &sn2, &ic2)) {
                            item = alloc_node(doc, LUI_MD_LIST_ITEM);
                            if (item) {
                                append_child(list, item);
                                parse_inlines(doc, item, ic2,
                                              (int)(cle - ic2));
                            }
                            p = cnext;
                        } else {
                            break;
                        }
                    }
                }
                continue;
            }
        }

        /* Paragraph text: accumulate lines. */
        if (!para_start) {
            para_start = p;
        }
        para_end = line_end;
        p = next;
    }

    FLUSH_PARA();
    #undef FLUSH_PARA

    return 0;
}

/* ---- Destroy ------------------------------------------------------------- */

void lui_md_destroy(lui_md_doc_t *doc)
{
    if (!doc) return;
    free(doc->nodes);
    free(doc->source);
    memset(doc, 0, sizeof(*doc));
}

/* ---- Style defaults ------------------------------------------------------ */

void lui_md_style_default(lui_md_style_t *style)
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

    style->blockquote_bar_width  = 3;
    style->blockquote_indent     = 16;
    style->list_indent           = 16;
    style->code_block_padding    = 6;
    style->paragraph_spacing     = 10;
    style->heading_spacing_above = 6;
    style->heading_spacing_below = 4;
}
