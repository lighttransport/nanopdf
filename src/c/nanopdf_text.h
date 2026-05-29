#ifndef NANOPDF_TEXT_H_
#define NANOPDF_TEXT_H_

#include "nanopdf_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

nanopdf_status nanopdf_page_extract_text(
    nanopdf_document* document,
    uint32_t page_index,
    char** out_text);

nanopdf_status nanopdf_page_extract_text_layout(
    nanopdf_document* document,
    uint32_t page_index,
    const nanopdf_text_layout_options* options,
    nanopdf_text_layout** out_layout);
void nanopdf_text_layout_destroy(nanopdf_text_layout* layout);

size_t nanopdf_text_layout_char_count(const nanopdf_text_layout* layout);
size_t nanopdf_text_layout_line_count(const nanopdf_text_layout* layout);
size_t nanopdf_text_layout_word_count(const nanopdf_text_layout* layout);

nanopdf_status nanopdf_text_layout_get_char(
    const nanopdf_text_layout* layout,
    size_t index,
    nanopdf_text_char* out_char);
nanopdf_status nanopdf_text_layout_copy_text(
    nanopdf_text_layout* layout,
    char** out_text);
nanopdf_status nanopdf_text_layout_copy_text_in_rect(
    nanopdf_text_layout* layout,
    double x1,
    double y1,
    double x2,
    double y2,
    char** out_text);
nanopdf_status nanopdf_page_extract_tables(
    const nanopdf_document* document,
    uint32_t page_index,
    const nanopdf_table_extraction_options* options,
    nanopdf_table_result** out_result);
void nanopdf_table_result_destroy(nanopdf_table_result* result);
size_t nanopdf_table_result_table_count(const nanopdf_table_result* result);
nanopdf_status nanopdf_table_result_get_table_info(
    const nanopdf_table_result* result,
    size_t table_index,
    nanopdf_table_info* out_table);
size_t nanopdf_table_result_table_cell_count(
    const nanopdf_table_result* result,
    size_t table_index);
nanopdf_status nanopdf_table_result_get_table_cell(
    const nanopdf_table_result* result,
    size_t table_index,
    size_t cell_index,
    nanopdf_table_cell_info* out_cell);
nanopdf_status nanopdf_table_result_copy_table_text(
    nanopdf_table_result* result,
    size_t table_index,
    nanopdf_table_output_format format,
    char** out_text);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_TEXT_H_ */
