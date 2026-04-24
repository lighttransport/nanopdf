#ifndef NANOPDF_FORMS_H_
#define NANOPDF_FORMS_H_

#include "nanopdf_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

size_t nanopdf_document_form_field_count(const nanopdf_document* document);
nanopdf_status nanopdf_document_get_form_field_info(
    nanopdf_document* document,
    size_t field_index,
    nanopdf_form_field_info* out_field_info);
nanopdf_status nanopdf_document_copy_form_field_value(
    nanopdf_document* document,
    size_t field_index,
    char** out_value);

nanopdf_status nanopdf_document_set_text_field(
    nanopdf_document* document,
    const char* field_name,
    const char* value);
nanopdf_status nanopdf_document_set_button_field(
    nanopdf_document* document,
    const char* field_name,
    int checked);
nanopdf_status nanopdf_document_set_choice_field_indices(
    nanopdf_document* document,
    const char* field_name,
    const int32_t* indices,
    size_t index_count);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_FORMS_H_ */
