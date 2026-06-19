/*
 * lui_json.c — Minimal JSON parser and builder
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lui_json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* =========================================================================
 * Parser
 * ========================================================================= */

typedef struct {
    const char *src;
    int         len;
    int         pos;
} parser_t;

static void p_skip_ws(parser_t *p)
{
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            p->pos++;
        else
            break;
    }
}

static char p_peek(parser_t *p)
{
    p_skip_ws(p);
    return (p->pos < p->len) ? p->src[p->pos] : '\0';
}

static bool p_match(parser_t *p, char c)
{
    p_skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == c) {
        p->pos++;
        return true;
    }
    return false;
}

static lui_json_t *p_alloc(lui_json_type_t type)
{
    lui_json_t *n = (lui_json_t *)calloc(1, sizeof(*n));
    if (n) n->type = type;
    return n;
}

/* Forward declaration */
static lui_json_t *p_value(parser_t *p);

static int p_hex4(parser_t *p)
{
    int val = 0;
    for (int i = 0; i < 4; i++) {
        if (p->pos >= p->len) return -1;
        char c = p->src[p->pos++];
        val <<= 4;
        if (c >= '0' && c <= '9')      val |= c - '0';
        else if (c >= 'a' && c <= 'f') val |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val |= c - 'A' + 10;
        else return -1;
    }
    return val;
}

static int p_utf8_encode(int cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

static lui_json_t *p_string(parser_t *p)
{
    if (!p_match(p, '"')) return NULL;

    /* First pass: compute length */
    int start = p->pos;
    int slen = 0;
    int saved = p->pos;

    while (p->pos < p->len && p->src[p->pos] != '"') {
        if (p->src[p->pos] == '\\') {
            p->pos++;
            if (p->pos >= p->len) return NULL;
            if (p->src[p->pos] == 'u') {
                p->pos++;
                int cp = p_hex4(p);
                if (cp < 0) return NULL;
                char tmp[4];
                slen += p_utf8_encode(cp, tmp);
            } else {
                p->pos++;
                slen++;
            }
        } else {
            p->pos++;
            slen++;
        }
    }

    if (p->pos >= p->len) return NULL; /* unterminated */

    /* Allocate and second pass: copy */
    char *str = (char *)malloc(slen + 1);
    if (!str) return NULL;

    p->pos = saved;
    int wi = 0;
    while (p->src[p->pos] != '"') {
        if (p->src[p->pos] == '\\') {
            p->pos++;
            switch (p->src[p->pos]) {
            case '"':  str[wi++] = '"';  break;
            case '\\': str[wi++] = '\\'; break;
            case '/':  str[wi++] = '/';  break;
            case 'b':  str[wi++] = '\b'; break;
            case 'f':  str[wi++] = '\f'; break;
            case 'n':  str[wi++] = '\n'; break;
            case 'r':  str[wi++] = '\r'; break;
            case 't':  str[wi++] = '\t'; break;
            case 'u': {
                p->pos++;
                int cp = p_hex4(p);
                wi += p_utf8_encode(cp, str + wi);
                continue; /* p_hex4 already advanced pos */
            }
            default: str[wi++] = p->src[p->pos]; break;
            }
            p->pos++;
        } else {
            str[wi++] = p->src[p->pos++];
        }
    }
    str[wi] = '\0';
    p->pos++; /* skip closing '"' */

    lui_json_t *n = p_alloc(LUI_JSON_STRING);
    if (!n) { free(str); return NULL; }
    n->v.string = str;
    (void)start;
    return n;
}

static lui_json_t *p_number(parser_t *p)
{
    int start = p->pos;
    if (p->pos < p->len && p->src[p->pos] == '-') p->pos++;
    while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9')
        p->pos++;
    if (p->pos < p->len && p->src[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9')
            p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-'))
            p->pos++;
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9')
            p->pos++;
    }
    if (p->pos == start) return NULL;

    char buf[64];
    int numlen = p->pos - start;
    if (numlen >= (int)sizeof(buf)) numlen = (int)sizeof(buf) - 1;
    memcpy(buf, p->src + start, numlen);
    buf[numlen] = '\0';

    lui_json_t *n = p_alloc(LUI_JSON_NUMBER);
    if (!n) return NULL;
    n->v.number = strtod(buf, NULL);
    return n;
}

static bool p_literal(parser_t *p, const char *lit, int litlen)
{
    if (p->pos + litlen > p->len) return false;
    if (memcmp(p->src + p->pos, lit, litlen) != 0) return false;
    p->pos += litlen;
    return true;
}

static lui_json_t *p_object(parser_t *p)
{
    if (!p_match(p, '{')) return NULL;
    lui_json_t *n = p_alloc(LUI_JSON_OBJECT);
    if (!n) return NULL;

    lui_json_kv_t *tail = NULL;

    if (p_peek(p) != '}') {
        for (;;) {
            /* Key */
            lui_json_t *key_node = p_string(p);
            if (!key_node) goto fail;
            char *key = key_node->v.string;
            key_node->v.string = NULL;
            lui_json_free(key_node);

            if (!p_match(p, ':')) { free(key); goto fail; }

            /* Value */
            lui_json_t *val = p_value(p);
            if (!val) { free(key); goto fail; }

            lui_json_kv_t *kv = (lui_json_kv_t *)calloc(1, sizeof(*kv));
            if (!kv) { free(key); lui_json_free(val); goto fail; }
            kv->key = key;
            kv->value = val;

            if (tail) tail->next = kv;
            else      n->v.children.head = kv;
            tail = kv;
            n->v.children.count++;

            if (!p_match(p, ',')) break;
        }
    }
    if (!p_match(p, '}')) goto fail;
    return n;

fail:
    lui_json_free(n);
    return NULL;
}

static lui_json_t *p_array(parser_t *p)
{
    if (!p_match(p, '[')) return NULL;
    lui_json_t *n = p_alloc(LUI_JSON_ARRAY);
    if (!n) return NULL;

    lui_json_kv_t *tail = NULL;

    if (p_peek(p) != ']') {
        for (;;) {
            lui_json_t *val = p_value(p);
            if (!val) goto fail;

            lui_json_kv_t *kv = (lui_json_kv_t *)calloc(1, sizeof(*kv));
            if (!kv) { lui_json_free(val); goto fail; }
            kv->key = NULL;
            kv->value = val;

            if (tail) tail->next = kv;
            else      n->v.children.head = kv;
            tail = kv;
            n->v.children.count++;

            if (!p_match(p, ',')) break;
        }
    }
    if (!p_match(p, ']')) goto fail;
    return n;

fail:
    lui_json_free(n);
    return NULL;
}

static lui_json_t *p_value(parser_t *p)
{
    char c = p_peek(p);
    switch (c) {
    case '"': return p_string(p);
    case '{': return p_object(p);
    case '[': return p_array(p);
    case 't':
        if (p_literal(p, "true", 4)) {
            lui_json_t *n = p_alloc(LUI_JSON_BOOL);
            if (n) n->v.boolean = true;
            return n;
        }
        return NULL;
    case 'f':
        if (p_literal(p, "false", 5)) {
            lui_json_t *n = p_alloc(LUI_JSON_BOOL);
            /* boolean already false from calloc */
            return n;
        }
        return NULL;
    case 'n':
        if (p_literal(p, "null", 4))
            return p_alloc(LUI_JSON_NULL);
        return NULL;
    default:
        if (c == '-' || (c >= '0' && c <= '9'))
            return p_number(p);
        return NULL;
    }
}

/* ---- Public parser API -------------------------------------------------- */

lui_json_t *lui_json_parse(const char *text, int len)
{
    if (!text) return NULL;
    if (len < 0) len = (int)strlen(text);

    parser_t p;
    p.src = text;
    p.len = len;
    p.pos = 0;

    lui_json_t *root = p_value(&p);
    return root;
}

void lui_json_free(lui_json_t *node)
{
    if (!node) return;

    switch (node->type) {
    case LUI_JSON_STRING:
        free(node->v.string);
        break;
    case LUI_JSON_OBJECT:
    case LUI_JSON_ARRAY: {
        lui_json_kv_t *kv = node->v.children.head;
        while (kv) {
            lui_json_kv_t *next = kv->next;
            free(kv->key);
            lui_json_free(kv->value);
            free(kv);
            kv = next;
        }
        break;
    }
    default:
        break;
    }
    free(node);
}

/* ---- Accessors ---------------------------------------------------------- */

const char *lui_json_string(const lui_json_t *n)
{
    return (n && n->type == LUI_JSON_STRING) ? n->v.string : NULL;
}

double lui_json_number(const lui_json_t *n)
{
    return (n && n->type == LUI_JSON_NUMBER) ? n->v.number : 0.0;
}

int lui_json_int(const lui_json_t *n)
{
    return (n && n->type == LUI_JSON_NUMBER) ? (int)n->v.number : 0;
}

bool lui_json_bool(const lui_json_t *n)
{
    return (n && n->type == LUI_JSON_BOOL) ? n->v.boolean : false;
}

lui_json_t *lui_json_get(const lui_json_t *n, const char *key)
{
    if (!n || n->type != LUI_JSON_OBJECT || !key) return NULL;
    for (lui_json_kv_t *kv = n->v.children.head; kv; kv = kv->next) {
        if (kv->key && strcmp(kv->key, key) == 0)
            return kv->value;
    }
    return NULL;
}

lui_json_t *lui_json_index(const lui_json_t *n, int index)
{
    if (!n || n->type != LUI_JSON_ARRAY || index < 0) return NULL;
    lui_json_kv_t *kv = n->v.children.head;
    for (int i = 0; kv; kv = kv->next, i++) {
        if (i == index) return kv->value;
    }
    return NULL;
}

int lui_json_length(const lui_json_t *n)
{
    if (!n || (n->type != LUI_JSON_OBJECT && n->type != LUI_JSON_ARRAY))
        return 0;
    return n->v.children.count;
}

/* =========================================================================
 * Builder
 * ========================================================================= */

struct lui_json_buf {
    char *data;
    int   len;
    int   cap;
    int   depth;
    bool  need_comma[64];   /* per nesting level */
};

lui_json_buf_t *lui_json_buf_new(void)
{
    lui_json_buf_t *b = (lui_json_buf_t *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->cap = 256;
    b->data = (char *)malloc(b->cap);
    if (!b->data) { free(b); return NULL; }
    b->len = 0;
    b->depth = 0;
    return b;
}

void lui_json_buf_free(lui_json_buf_t *b)
{
    if (!b) return;
    free(b->data);
    free(b);
}

static void b_ensure(lui_json_buf_t *b, int extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    int new_cap = b->cap * 2;
    if (new_cap < b->len + extra + 1) new_cap = b->len + extra + 1;
    char *new_data = (char *)realloc(b->data, new_cap);
    if (!new_data) return;
    b->data = new_data;
    b->cap = new_cap;
}

static void b_put(lui_json_buf_t *b, const char *s, int n)
{
    b_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static void b_putc(lui_json_buf_t *b, char c)
{
    b_ensure(b, 1);
    b->data[b->len++] = c;
}

static void b_comma(lui_json_buf_t *b)
{
    if (b->depth > 0 && b->need_comma[b->depth])
        b_putc(b, ',');
    if (b->depth > 0)
        b->need_comma[b->depth] = true;
}

char *lui_json_buf_finish(lui_json_buf_t *b, int *out_len)
{
    if (!b) return NULL;
    b_putc(b, '\0');
    char *result = b->data;
    int len = b->len - 1; /* exclude NUL */
    b->data = NULL;
    if (out_len) *out_len = len;
    free(b);
    return result;
}

void lui_json_buf_object_begin(lui_json_buf_t *b)
{
    b_comma(b);
    b_putc(b, '{');
    if (b->depth < 63) b->depth++;
    b->need_comma[b->depth] = false;
}

void lui_json_buf_object_end(lui_json_buf_t *b)
{
    b_putc(b, '}');
    if (b->depth > 0) b->depth--;
}

void lui_json_buf_array_begin(lui_json_buf_t *b)
{
    b_comma(b);
    b_putc(b, '[');
    if (b->depth < 63) b->depth++;
    b->need_comma[b->depth] = false;
}

void lui_json_buf_array_end(lui_json_buf_t *b)
{
    b_putc(b, ']');
    if (b->depth > 0) b->depth--;
}

void lui_json_buf_key(lui_json_buf_t *b, const char *key)
{
    b_comma(b);
    b->need_comma[b->depth] = false; /* the value call will set it */
    b_putc(b, '"');
    /* Simple escape for key (keys are usually ASCII identifiers) */
    for (const char *s = key; *s; s++) {
        if (*s == '"' || *s == '\\') b_putc(b, '\\');
        b_putc(b, *s);
    }
    b_putc(b, '"');
    b_putc(b, ':');
}

void lui_json_buf_string(lui_json_buf_t *b, const char *value)
{
    b_comma(b);
    b_putc(b, '"');
    if (value) {
        for (const char *s = value; *s; s++) {
            unsigned char c = (unsigned char)*s;
            switch (c) {
            case '"':  b_put(b, "\\\"", 2); break;
            case '\\': b_put(b, "\\\\", 2); break;
            case '\b': b_put(b, "\\b", 2);  break;
            case '\f': b_put(b, "\\f", 2);  break;
            case '\n': b_put(b, "\\n", 2);  break;
            case '\r': b_put(b, "\\r", 2);  break;
            case '\t': b_put(b, "\\t", 2);  break;
            default:
                if (c < 0x20) {
                    char esc[7];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    b_put(b, esc, 6);
                } else {
                    b_putc(b, *s);
                }
                break;
            }
        }
    }
    b_putc(b, '"');
}

void lui_json_buf_int(lui_json_buf_t *b, int value)
{
    b_comma(b);
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%d", value);
    b_put(b, tmp, n);
}

void lui_json_buf_number(lui_json_buf_t *b, double value)
{
    b_comma(b);
    char tmp[48];
    int n = snprintf(tmp, sizeof(tmp), "%g", value);
    b_put(b, tmp, n);
}

void lui_json_buf_bool(lui_json_buf_t *b, bool value)
{
    b_comma(b);
    if (value)
        b_put(b, "true", 4);
    else
        b_put(b, "false", 5);
}

void lui_json_buf_null(lui_json_buf_t *b)
{
    b_comma(b);
    b_put(b, "null", 4);
}

void lui_json_buf_raw(lui_json_buf_t *b, const char *raw, int len)
{
    b_comma(b);
    if (len < 0) len = (int)strlen(raw);
    b_put(b, raw, len);
}

/* =========================================================================
 * Serializer (tree → JSON string)
 * ========================================================================= */

static void ser_node(lui_json_buf_t *b, const lui_json_t *n)
{
    if (!n) { lui_json_buf_null(b); return; }

    switch (n->type) {
    case LUI_JSON_NULL:
        lui_json_buf_null(b);
        break;
    case LUI_JSON_BOOL:
        lui_json_buf_bool(b, n->v.boolean);
        break;
    case LUI_JSON_NUMBER: {
        /* Use integer format when the value is an exact integer to avoid
         * precision loss from %g (e.g. 2097162 → "2.09716e+06"). */
        double v = n->v.number;
        if (v == (double)(int)v && v >= -2147483648.0 && v <= 2147483647.0)
            lui_json_buf_int(b, (int)v);
        else
            lui_json_buf_number(b, v);
        break;
    }
    case LUI_JSON_STRING:
        lui_json_buf_string(b, n->v.string);
        break;
    case LUI_JSON_ARRAY:
        lui_json_buf_array_begin(b);
        for (lui_json_kv_t *kv = n->v.children.head; kv; kv = kv->next)
            ser_node(b, kv->value);
        lui_json_buf_array_end(b);
        break;
    case LUI_JSON_OBJECT:
        lui_json_buf_object_begin(b);
        for (lui_json_kv_t *kv = n->v.children.head; kv; kv = kv->next) {
            if (kv->key) lui_json_buf_key(b, kv->key);
            ser_node(b, kv->value);
        }
        lui_json_buf_object_end(b);
        break;
    }
}

char *lui_json_serialize(const lui_json_t *node, int *out_len)
{
    if (!node) return NULL;
    lui_json_buf_t *b = lui_json_buf_new();
    if (!b) return NULL;
    ser_node(b, node);
    return lui_json_buf_finish(b, out_len);
}
