#ifndef NANOPDF_BASIC_DOCUMENT_H_
#define NANOPDF_BASIC_DOCUMENT_H_

#include "nanopdf_c_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nanopdf_basic_content_ref {
  uint8_t kind;
  uint32_t object_number;
  uint16_t generation;
  uint8_t valid;
  uint8_t* decoded_data;
  size_t decoded_size;
} nanopdf_basic_content_ref;

typedef struct nanopdf_basic_page {
  double width;
  double height;
  double rotation;
  nanopdf_basic_content_ref* contents;
  size_t content_count;
  size_t content_capacity;
} nanopdf_basic_page;

typedef struct nanopdf_basic_xref_entry {
  size_t offset;
  uint16_t generation;
  uint8_t present;
  uint8_t in_use;
  uint8_t compressed;
  uint32_t object_stream_number;
  uint32_t object_stream_index;
} nanopdf_basic_xref_entry;

typedef struct nanopdf_basic_form_field {
  nanopdf_field_type type;
  char* partial_name;
  char* full_name;
  char* alternate_name;
  char* mapping_name;
  char* value;
  uint32_t flags;
} nanopdf_basic_form_field;

typedef enum nanopdf_basic_security_algorithm {
  NANOPDF_BASIC_SECURITY_NONE = 0,
  NANOPDF_BASIC_SECURITY_RC4_40 = 1,
  NANOPDF_BASIC_SECURITY_RC4_128 = 2,
  NANOPDF_BASIC_SECURITY_AES_128 = 3,
  NANOPDF_BASIC_SECURITY_AES_256 = 4
} nanopdf_basic_security_algorithm;

typedef struct nanopdf_basic_security {
  uint8_t active;
  uint8_t authenticated;
  uint8_t algorithm;
  uint8_t string_algorithm;
  uint8_t stream_algorithm;
  uint8_t encrypt_metadata;
  uint8_t key[32];
  uint8_t key_length;
  int32_t permissions;
  char* file_id;
} nanopdf_basic_security;

typedef struct nanopdf_basic_document {
  uint32_t version_major;
  uint32_t version_minor;
  const uint8_t* data;
  size_t data_size;
  nanopdf_basic_xref_entry* xrefs;
  size_t xref_count;
  nanopdf_basic_page* pages;
  size_t page_count;
  size_t page_capacity;
  char* title;
  char* author;
  char* subject;
  char* keywords;
  char* creator;
  char* producer;
  char* creation_date;
  char* mod_date;
  nanopdf_basic_form_field* form_fields;
  size_t form_field_count;
  size_t form_field_capacity;
  uint8_t forms_parsed;
  uint8_t recover_stream_length_enabled;
  nanopdf_basic_security security;
} nanopdf_basic_document;

void nanopdf_basic_document_init(nanopdf_basic_document* document);
void nanopdf_basic_document_destroy(
    const nanopdf_allocator* allocator,
    nanopdf_basic_document* document);
nanopdf_status nanopdf_basic_document_parse(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    const nanopdf_parse_options* options,
    nanopdf_basic_document* out_document);
nanopdf_status nanopdf_basic_document_extract_text(
    nanopdf_context* context,
    const nanopdf_basic_document* document,
    uint32_t page_index,
    char** out_text);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_BASIC_DOCUMENT_H_ */
