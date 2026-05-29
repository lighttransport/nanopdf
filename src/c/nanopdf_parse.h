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
nanopdf_status nanopdf_document_copy_custom_info_value(
    nanopdf_document* document,
    const char* key,
    char** out_value);
nanopdf_status nanopdf_document_copy_language(
    const nanopdf_document* document,
    char** out_language);
nanopdf_status nanopdf_document_copy_xmp_metadata(
    const nanopdf_document* document,
    char** out_xml);
nanopdf_status nanopdf_document_copy_open_action_named_destination(
    const nanopdf_document* document,
    char** out_destination_name);
nanopdf_status nanopdf_document_get_page_layout(
    const nanopdf_document* document,
    nanopdf_page_layout* out_layout);
nanopdf_status nanopdf_document_get_page_mode(
    const nanopdf_document* document,
    nanopdf_page_mode* out_mode);
nanopdf_status nanopdf_document_get_viewer_preferences(
    const nanopdf_document* document,
    nanopdf_viewer_preferences* out_preferences);
nanopdf_status nanopdf_document_get_mark_info(
    const nanopdf_document* document,
    nanopdf_mark_info* out_mark_info);
uint32_t nanopdf_document_output_intent_count(const nanopdf_document* document);
nanopdf_status nanopdf_document_get_output_intent(
    const nanopdf_document* document,
    uint32_t index,
    nanopdf_output_intent* out_output_intent);
uint32_t nanopdf_document_page_label_count(const nanopdf_document* document);
nanopdf_status nanopdf_document_get_page_label(
    const nanopdf_document* document,
    uint32_t index,
    uint32_t* out_page_index,
    nanopdf_page_label* out_label);
uint32_t nanopdf_document_named_destination_count(const nanopdf_document* document);
nanopdf_status nanopdf_document_get_named_destination(
    const nanopdf_document* document,
    uint32_t index,
    nanopdf_named_destination* out_destination);
uint32_t nanopdf_document_bookmark_count(const nanopdf_document* document);
nanopdf_status nanopdf_document_get_bookmark(
    const nanopdf_document* document,
    uint32_t index,
    nanopdf_bookmark_info* out_bookmark);
uint32_t nanopdf_document_attachment_count(const nanopdf_document* document);
nanopdf_status nanopdf_document_get_attachment_info(
    const nanopdf_document* document,
    uint32_t index,
    nanopdf_attachment_info* out_attachment);
nanopdf_status nanopdf_document_copy_attachment_data(
    const nanopdf_document* document,
    uint32_t index,
    void** out_data,
    size_t* out_size);
uint32_t nanopdf_page_annotation_count(
    const nanopdf_document* document,
    uint32_t page_index);
nanopdf_status nanopdf_page_get_annotation(
    const nanopdf_document* document,
    uint32_t page_index,
    uint32_t annotation_index,
    nanopdf_annotation_info* out_annotation);
nanopdf_status nanopdf_document_validate_pdfa(
    const nanopdf_document* document,
    nanopdf_pdfa_report** out_report);
void nanopdf_pdfa_report_destroy(nanopdf_pdfa_report* report);
nanopdf_status nanopdf_pdfa_report_get_summary(
    const nanopdf_pdfa_report* report,
    nanopdf_pdfa_summary* out_summary);
size_t nanopdf_pdfa_report_violation_count(const nanopdf_pdfa_report* report);
nanopdf_status nanopdf_pdfa_report_get_violation(
    const nanopdf_pdfa_report* report,
    size_t index,
    nanopdf_pdfa_violation* out_violation);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_PARSE_H_ */
