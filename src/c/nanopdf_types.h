#ifndef NANOPDF_C_TYPES_H_
#define NANOPDF_C_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NANOPDF_C_VERSION_MAJOR 0u
#define NANOPDF_C_VERSION_MINOR 4u
#define NANOPDF_C_VERSION_PATCH 0u

typedef struct nanopdf_context nanopdf_context;
typedef struct nanopdf_document nanopdf_document;
typedef struct nanopdf_text_layout nanopdf_text_layout;
typedef struct nanopdf_writer nanopdf_writer;
typedef struct nanopdf_page_builder nanopdf_page_builder;
typedef struct nanopdf_object nanopdf_object;

typedef enum nanopdf_status {
  NANOPDF_STATUS_OK = 0,
  NANOPDF_STATUS_INVALID_ARGUMENT = 1,
  NANOPDF_STATUS_OUT_OF_MEMORY = 2,
  NANOPDF_STATUS_PARSE_ERROR = 3,
  NANOPDF_STATUS_MALFORMED = 4,
  NANOPDF_STATUS_UNSUPPORTED = 5,
  NANOPDF_STATUS_ENCRYPTED = 6,
  NANOPDF_STATUS_IO_ERROR = 7,
  NANOPDF_STATUS_INTERNAL_ERROR = 8,
  NANOPDF_STATUS_NOT_FOUND = 9
} nanopdf_status;

typedef enum nanopdf_info_key {
  NANOPDF_INFO_TITLE = 0,
  NANOPDF_INFO_AUTHOR = 1,
  NANOPDF_INFO_SUBJECT = 2,
  NANOPDF_INFO_KEYWORDS = 3,
  NANOPDF_INFO_CREATOR = 4,
  NANOPDF_INFO_PRODUCER = 5,
  NANOPDF_INFO_CREATION_DATE = 6,
  NANOPDF_INFO_MOD_DATE = 7
} nanopdf_info_key;

typedef enum nanopdf_field_type {
  NANOPDF_FIELD_TYPE_BUTTON = 0,
  NANOPDF_FIELD_TYPE_TEXT = 1,
  NANOPDF_FIELD_TYPE_CHOICE = 2,
  NANOPDF_FIELD_TYPE_SIGNATURE = 3
} nanopdf_field_type;

typedef enum nanopdf_standard_font {
  NANOPDF_STANDARD_FONT_HELVETICA = 0,
  NANOPDF_STANDARD_FONT_HELVETICA_BOLD = 1,
  NANOPDF_STANDARD_FONT_HELVETICA_OBLIQUE = 2,
  NANOPDF_STANDARD_FONT_HELVETICA_BOLD_OBLIQUE = 3,
  NANOPDF_STANDARD_FONT_TIMES_ROMAN = 4,
  NANOPDF_STANDARD_FONT_TIMES_BOLD = 5,
  NANOPDF_STANDARD_FONT_TIMES_ITALIC = 6,
  NANOPDF_STANDARD_FONT_TIMES_BOLD_ITALIC = 7,
  NANOPDF_STANDARD_FONT_COURIER = 8,
  NANOPDF_STANDARD_FONT_COURIER_BOLD = 9,
  NANOPDF_STANDARD_FONT_COURIER_OBLIQUE = 10,
  NANOPDF_STANDARD_FONT_COURIER_BOLD_OBLIQUE = 11,
  NANOPDF_STANDARD_FONT_SYMBOL = 12,
  NANOPDF_STANDARD_FONT_ZAPF_DINGBATS = 13
} nanopdf_standard_font;

typedef enum nanopdf_image_compression {
  NANOPDF_IMAGE_COMPRESSION_AUTO = 0,
  NANOPDF_IMAGE_COMPRESSION_FLATE = 1,
  NANOPDF_IMAGE_COMPRESSION_DCT = 2,
  NANOPDF_IMAGE_COMPRESSION_CCITT_FAX = 3
} nanopdf_image_compression;

typedef void* (*nanopdf_alloc_fn)(void* user_data, size_t size);
typedef void* (*nanopdf_realloc_fn)(void* user_data, void* ptr, size_t size);
typedef void (*nanopdf_free_fn)(void* user_data, void* ptr);

typedef struct nanopdf_allocator {
  void* user_data;
  nanopdf_alloc_fn alloc;
  nanopdf_realloc_fn realloc;
  nanopdf_free_fn free;
} nanopdf_allocator;

typedef struct nanopdf_context_options {
  size_t struct_size;
  nanopdf_allocator allocator;
} nanopdf_context_options;

typedef struct nanopdf_parse_options {
  uint8_t auto_repair;
  uint8_t recover_stream_length;
  size_t max_repair_scan;
  const char* password;
} nanopdf_parse_options;

typedef struct nanopdf_page_info {
  uint32_t page_index;
  double width;
  double height;
  double rotation;
} nanopdf_page_info;

typedef struct nanopdf_text_layout_options {
  double baseline_tolerance;
  double line_spacing_threshold;
  double word_spacing_threshold;
  double column_gap_threshold;
  uint8_t detect_columns;
  uint8_t detect_rtl;
} nanopdf_text_layout_options;

typedef struct nanopdf_text_char {
  uint32_t unicode;
  double x;
  double y;
  double width;
  double height;
  double font_size;
  const char* font_name;
  double char_spacing;
  double word_spacing;
  int32_t line_index;
  int32_t word_index;
  double matrix[6];
  double rotation;
} nanopdf_text_char;

typedef struct nanopdf_form_field_info {
  nanopdf_field_type type;
  const char* partial_name;
  const char* full_name;
  const char* alternate_name;
  const char* mapping_name;
  uint32_t flags;
} nanopdf_form_field_info;

typedef struct nanopdf_object_ref {
  uint32_t object_number;
  uint16_t generation;
  uint8_t valid;
} nanopdf_object_ref;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_C_TYPES_H_ */
