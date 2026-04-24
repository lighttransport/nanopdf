#ifndef NANOPDF_PARSE_H_
#define NANOPDF_PARSE_H_

#include "nanopdf_c.h"

#ifdef __cplusplus
extern "C" {
#endif

nanopdf_status nanopdf_document_open_memory(
    nanopdf_context* context,
    const void* data,
    size_t size,
    const nanopdf_parse_options* options,
    nanopdf_document** out_document);
void nanopdf_document_close(nanopdf_document* document);

uint32_t nanopdf_document_page_count(const nanopdf_document* document);
nanopdf_status nanopdf_document_get_page_info(
    const nanopdf_document* document,
    uint32_t page_index,
    nanopdf_page_info* out_page_info);
nanopdf_status nanopdf_document_copy_info_value(
    nanopdf_document* document,
    nanopdf_info_key key,
    char** out_value);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_PARSE_H_ */
