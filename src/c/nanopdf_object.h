#ifndef NANOPDF_OBJECT_H_
#define NANOPDF_OBJECT_H_

#include "nanopdf_basic_document.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nanopdf_basic_ref {
  uint32_t object_number;
  uint16_t generation;
  uint8_t valid;
} nanopdf_basic_ref;

typedef enum nanopdf_basic_object_type {
  NANOPDF_BASIC_OBJECT_NULL = 0,
  NANOPDF_BASIC_OBJECT_BOOL = 1,
  NANOPDF_BASIC_OBJECT_NUMBER = 2,
  NANOPDF_BASIC_OBJECT_STRING = 3,
  NANOPDF_BASIC_OBJECT_NAME = 4,
  NANOPDF_BASIC_OBJECT_ARRAY = 5,
  NANOPDF_BASIC_OBJECT_DICT = 6,
  NANOPDF_BASIC_OBJECT_STREAM = 7,
  NANOPDF_BASIC_OBJECT_REF = 8
} nanopdf_basic_object_type;

struct nanopdf_basic_object;

typedef struct nanopdf_basic_array {
  struct nanopdf_basic_object* items;
  size_t count;
  size_t capacity;
} nanopdf_basic_array;

typedef struct nanopdf_basic_dict_entry {
  char* key;
  struct nanopdf_basic_object* value;
} nanopdf_basic_dict_entry;

typedef struct nanopdf_basic_dict {
  nanopdf_basic_dict_entry* entries;
  size_t count;
  size_t capacity;
} nanopdf_basic_dict;

typedef struct nanopdf_basic_stream {
  nanopdf_basic_dict dict;
  uint8_t* data;
  size_t size;
} nanopdf_basic_stream;

typedef struct nanopdf_basic_object {
  nanopdf_basic_object_type type;
  size_t length;
  union {
    int boolean;
    double number;
    char* text;
    nanopdf_basic_array array;
    nanopdf_basic_dict dict;
    nanopdf_basic_stream stream;
    nanopdf_basic_ref ref;
  } as;
} nanopdf_basic_object;

void nanopdf_basic_object_init(nanopdf_basic_object* object);
void nanopdf_basic_object_destroy(
    const nanopdf_allocator* allocator,
    nanopdf_basic_object* object);

nanopdf_status nanopdf_basic_parse_indirect_object_at(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t offset,
    nanopdf_basic_object* out_object);

nanopdf_status nanopdf_basic_parse_object_from_buffer(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t start,
    nanopdf_basic_object* out_object);

nanopdf_status nanopdf_basic_load_object(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    nanopdf_basic_ref ref,
    nanopdf_basic_object* out_object);

const nanopdf_basic_object* nanopdf_basic_dict_get(
    const nanopdf_basic_dict* dict,
    const char* key);

const nanopdf_basic_dict* nanopdf_basic_object_as_dict(
    const nanopdf_basic_object* object);

nanopdf_status nanopdf_basic_decode_stream(
    nanopdf_context* context,
    const nanopdf_basic_object* object,
    uint8_t** out_data,
    size_t* out_size);

nanopdf_status nanopdf_basic_decode_stream_with_document(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    const nanopdf_basic_object* object,
    uint8_t** out_data,
    size_t* out_size);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_OBJECT_H_ */
