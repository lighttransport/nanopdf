#ifndef NANOPDF_C_INTERNAL_H_
#define NANOPDF_C_INTERNAL_H_

#include "nanopdf_c.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nanopdf_context {
  nanopdf_allocator allocator;
  char* last_error;
  size_t last_error_capacity;
  nanopdf_status last_status;
};

void* nanopdf__allocator_alloc(const nanopdf_allocator* allocator, size_t size);
void* nanopdf__allocator_realloc(
    const nanopdf_allocator* allocator,
    void* ptr,
    size_t size);
void nanopdf__allocator_free(const nanopdf_allocator* allocator, void* ptr);

void nanopdf__clear_error(nanopdf_context* context);
void nanopdf__set_error(
    nanopdf_context* context,
    nanopdf_status status,
    const char* message);
nanopdf_status nanopdf__copy_owned_string(
    nanopdf_context* context,
    const char* value,
    char** out_value);
char* nanopdf__strdup(
    const nanopdf_allocator* allocator,
    const char* value);
void nanopdf__document_destroy_cpp_bridge(nanopdf_document* document);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_C_INTERNAL_H_ */
