#ifndef NANOPDF_C_H_
#define NANOPDF_C_H_

#include "nanopdf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void nanopdf_default_context_options(nanopdf_context_options* options);
void nanopdf_default_parse_options(nanopdf_parse_options* options);
void nanopdf_default_text_layout_options(nanopdf_text_layout_options* options);
void nanopdf_default_table_extraction_options(
    nanopdf_table_extraction_options* options);

nanopdf_status nanopdf_context_create(
    const nanopdf_context_options* options,
    nanopdf_context** out_context);
void nanopdf_context_destroy(nanopdf_context* context);

nanopdf_status nanopdf_context_last_status(const nanopdf_context* context);
const char* nanopdf_context_last_error(const nanopdf_context* context);
const nanopdf_allocator* nanopdf_context_get_allocator(
    const nanopdf_context* context);

void nanopdf_free(nanopdf_context* context, void* ptr);

uint32_t nanopdf_version_major(void);
uint32_t nanopdf_version_minor(void);
uint32_t nanopdf_version_patch(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_C_H_ */
