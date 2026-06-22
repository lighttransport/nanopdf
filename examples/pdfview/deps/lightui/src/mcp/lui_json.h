/*
 * lui_json.h — Minimal JSON parser and builder (C99, no dependencies)
 *
 * Parser: recursive descent, returns a tree of lui_json_t nodes.
 * Builder: streaming API that writes into a growable buffer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LUI_JSON_H
#define LUI_JSON_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- JSON value types --------------------------------------------------- */

typedef enum {
    LUI_JSON_NULL,
    LUI_JSON_BOOL,
    LUI_JSON_NUMBER,
    LUI_JSON_STRING,
    LUI_JSON_ARRAY,
    LUI_JSON_OBJECT,
} lui_json_type_t;

/* ---- JSON node (parse result) ------------------------------------------- */

typedef struct lui_json     lui_json_t;
typedef struct lui_json_kv  lui_json_kv_t;

struct lui_json_kv {
    char          *key;      /* NULL for array elements       */
    lui_json_t    *value;
    lui_json_kv_t *next;
};

struct lui_json {
    lui_json_type_t type;
    union {
        double         number;
        char          *string;   /* heap-allocated, NUL-terminated */
        bool           boolean;
        struct {
            lui_json_kv_t *head;
            int            count;
        } children;              /* object or array */
    } v;
};

/* ---- Parser ------------------------------------------------------------- */

/** Parse a JSON string. Returns NULL on error. Caller must free with lui_json_free(). */
lui_json_t *lui_json_parse(const char *text, int len);

/** Free a parsed JSON tree. */
void lui_json_free(lui_json_t *node);

/* ---- Accessors ---------------------------------------------------------- */

static inline lui_json_type_t lui_json_type(const lui_json_t *n) {
    return n ? n->type : LUI_JSON_NULL;
}

/** Get string value (or NULL). */
const char *lui_json_string(const lui_json_t *n);

/** Get number value (or 0). */
double lui_json_number(const lui_json_t *n);

/** Get int value (or 0). */
int lui_json_int(const lui_json_t *n);

/** Get boolean value (or false). */
bool lui_json_bool(const lui_json_t *n);

/** Lookup object member by key. Returns NULL if not found or not an object. */
lui_json_t *lui_json_get(const lui_json_t *n, const char *key);

/** Get array element by index. Returns NULL if out of range or not an array. */
lui_json_t *lui_json_index(const lui_json_t *n, int index);

/** Get child count (object or array). */
int lui_json_length(const lui_json_t *n);

/* ---- Builder (streaming) ------------------------------------------------ */

typedef struct lui_json_buf lui_json_buf_t;

lui_json_buf_t *lui_json_buf_new(void);
void            lui_json_buf_free(lui_json_buf_t *buf);

/**
 * Finish building and return the JSON string.
 * Caller must free() the returned pointer. Sets *out_len if non-NULL.
 * The buf is consumed and freed.
 */
char *lui_json_buf_finish(lui_json_buf_t *buf, int *out_len);

void lui_json_buf_object_begin(lui_json_buf_t *b);
void lui_json_buf_object_end(lui_json_buf_t *b);
void lui_json_buf_array_begin(lui_json_buf_t *b);
void lui_json_buf_array_end(lui_json_buf_t *b);

/** Write an object key (must be followed by a value call). */
void lui_json_buf_key(lui_json_buf_t *b, const char *key);

void lui_json_buf_string(lui_json_buf_t *b, const char *value);
void lui_json_buf_int(lui_json_buf_t *b, int value);
void lui_json_buf_number(lui_json_buf_t *b, double value);
void lui_json_buf_bool(lui_json_buf_t *b, bool value);
void lui_json_buf_null(lui_json_buf_t *b);

/** Write a pre-formatted JSON string verbatim (no quoting). */
void lui_json_buf_raw(lui_json_buf_t *b, const char *raw, int len);

/**
 * Serialize a parsed JSON tree back to a JSON string.
 * Returns a malloc'd NUL-terminated string. Caller must free().
 * Sets *out_len to the string length (excluding NUL) if non-NULL.
 * Returns NULL on failure.
 */
char *lui_json_serialize(const lui_json_t *node, int *out_len);

#ifdef __cplusplus
}
#endif

#endif /* LUI_JSON_H */
