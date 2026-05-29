#ifndef NANOPDF_WRITE_H_
#define NANOPDF_WRITE_H_

#include "nanopdf_c.h"

#ifdef __cplusplus
extern "C" {
#endif

nanopdf_status nanopdf_writer_create(
    nanopdf_context* context,
    nanopdf_writer** out_writer);
void nanopdf_writer_destroy(nanopdf_writer* writer);
nanopdf_status nanopdf_writer_load_existing_file(
    nanopdf_writer* writer,
    const char* path);
nanopdf_status nanopdf_writer_load_existing_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size);
nanopdf_status nanopdf_writer_import_pages_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    const int32_t* page_indices,
    size_t page_index_count,
    int32_t* out_imported_page_count);
nanopdf_status nanopdf_writer_has_existing_pdf(
    nanopdf_writer* writer,
    int* out_has_existing_pdf);
nanopdf_status nanopdf_writer_get_revision_count(
    nanopdf_writer* writer,
    int32_t* out_revision_count);
nanopdf_status nanopdf_writer_get_version(
    nanopdf_writer* writer,
    nanopdf_pdf_version* out_version);
nanopdf_status nanopdf_writer_get_page_count(
    nanopdf_writer* writer,
    int32_t* out_page_count);
nanopdf_status nanopdf_writer_set_page_label(
    nanopdf_writer* writer,
    uint32_t page_index,
    const nanopdf_page_label* label);
nanopdf_status nanopdf_writer_clear_page_labels(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_add_named_destination(
    nanopdf_writer* writer,
    const nanopdf_named_destination* destination);
nanopdf_status nanopdf_writer_clear_named_destinations(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_set_open_action_named_destination(
    nanopdf_writer* writer,
    const char* destination_name);
nanopdf_status nanopdf_writer_clear_open_action(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_set_page_layout(
    nanopdf_writer* writer,
    nanopdf_page_layout layout);
nanopdf_status nanopdf_writer_set_page_mode(
    nanopdf_writer* writer,
    nanopdf_page_mode mode);
nanopdf_status nanopdf_writer_set_viewer_preferences(
    nanopdf_writer* writer,
    const nanopdf_viewer_preferences* preferences);
nanopdf_status nanopdf_writer_clear_viewer_preferences(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_set_language(
    nanopdf_writer* writer,
    const char* language);
nanopdf_status nanopdf_writer_clear_language(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_set_xmp_metadata(
    nanopdf_writer* writer,
    const char* xml);
nanopdf_status nanopdf_writer_clear_xmp_metadata(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_add_output_intent(
    nanopdf_writer* writer,
    const nanopdf_output_intent* output_intent);
nanopdf_status nanopdf_writer_clear_output_intents(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_set_mark_info(
    nanopdf_writer* writer,
    const nanopdf_mark_info* mark_info);
nanopdf_status nanopdf_writer_clear_mark_info(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_set_trapped(
    nanopdf_writer* writer,
    nanopdf_trapped_state trapped);
nanopdf_status nanopdf_writer_clear_trapped(
    nanopdf_writer* writer);

nanopdf_status nanopdf_writer_set_title(
    nanopdf_writer* writer,
    const char* title);
nanopdf_status nanopdf_writer_set_author(
    nanopdf_writer* writer,
    const char* author);
nanopdf_status nanopdf_writer_set_subject(
    nanopdf_writer* writer,
    const char* subject);
nanopdf_status nanopdf_writer_set_keywords(
    nanopdf_writer* writer,
    const char* keywords);
nanopdf_status nanopdf_writer_set_creator(
    nanopdf_writer* writer,
    const char* creator);
nanopdf_status nanopdf_writer_set_producer(
    nanopdf_writer* writer,
    const char* producer);
nanopdf_status nanopdf_writer_set_custom_info(
    nanopdf_writer* writer,
    const char* key,
    const char* value);
nanopdf_status nanopdf_writer_clear_custom_info(
    nanopdf_writer* writer,
    const char* key);
nanopdf_status nanopdf_writer_set_creation_date(
    nanopdf_writer* writer,
    const char* creation_date);
nanopdf_status nanopdf_writer_set_modification_date(
    nanopdf_writer* writer,
    const char* modification_date);
nanopdf_status nanopdf_writer_set_version(
    nanopdf_writer* writer,
    nanopdf_pdf_version version);
nanopdf_status nanopdf_writer_set_document_id(
    nanopdf_writer* writer,
    const nanopdf_buffer_view* id1,
    const nanopdf_buffer_view* id2);
nanopdf_status nanopdf_writer_generate_document_id(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_add_standard_font(
    nanopdf_writer* writer,
    nanopdf_standard_font font,
    char** out_font_name);
nanopdf_status nanopdf_writer_add_truetype_font_from_file(
    nanopdf_writer* writer,
    const char* path,
    nanopdf_font_embedding embedding,
    char** out_font_name);
nanopdf_status nanopdf_writer_add_truetype_font_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    nanopdf_font_embedding embedding,
    char** out_font_name);
nanopdf_status nanopdf_writer_has_font_resource(
    nanopdf_writer* writer,
    const char* font_name,
    int* out_has_font);
nanopdf_status nanopdf_writer_get_font_metrics(
    nanopdf_writer* writer,
    const char* font_name,
    nanopdf_font_metrics_summary* out_metrics);
nanopdf_status nanopdf_writer_get_font_codepoint_width(
    nanopdf_writer* writer,
    const char* font_name,
    uint32_t codepoint,
    int32_t* out_width);
nanopdf_status nanopdf_writer_copy_font_postscript_name(
    nanopdf_writer* writer,
    const char* font_name,
    char** out_name);
nanopdf_status nanopdf_writer_copy_font_family_name(
    nanopdf_writer* writer,
    const char* font_name,
    char** out_name);
nanopdf_status nanopdf_writer_mark_font_text_used(
    nanopdf_writer* writer,
    const char* font_name,
    const char* text);
nanopdf_status nanopdf_writer_add_image_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    nanopdf_image_compression compression,
    char** out_image_name);
nanopdf_status nanopdf_writer_add_image_with_alpha_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    nanopdf_image_compression compression,
    char** out_image_name);
nanopdf_status nanopdf_writer_add_image_with_mask_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    const nanopdf_soft_mask_config* mask,
    nanopdf_image_compression compression,
    char** out_image_name);
nanopdf_status nanopdf_writer_add_image_from_file(
    nanopdf_writer* writer,
    const char* path,
    nanopdf_image_compression compression,
    char** out_image_name);
nanopdf_status nanopdf_writer_add_ccitt_image(
    nanopdf_writer* writer,
    const void* mono_data,
    uint32_t width,
    uint32_t height,
    char** out_image_name);
nanopdf_status nanopdf_writer_has_image_resource(
    nanopdf_writer* writer,
    const char* image_name,
    int* out_has_image);
nanopdf_status nanopdf_writer_add_image_page_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    double page_width,
    double page_height,
    double margin);
nanopdf_status nanopdf_writer_add_image_page_fit_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    double margin);
nanopdf_status nanopdf_writer_set_text_watermark(
    nanopdf_writer* writer,
    const char* text,
    const char* font_name,
    double font_size,
    double r,
    double g,
    double b,
    double alpha,
    double rotation_degrees,
    nanopdf_watermark_position position,
    nanopdf_watermark_layer layer);
nanopdf_status nanopdf_writer_set_image_watermark(
    nanopdf_writer* writer,
    const char* image_name,
    double alpha,
    double scale,
    nanopdf_watermark_position position,
    nanopdf_watermark_layer layer,
    double offset_x,
    double offset_y);
nanopdf_status nanopdf_writer_add_page_text_watermark(
    nanopdf_writer* writer,
    uint32_t page_index,
    const char* text,
    const char* font_name,
    double font_size,
    double r,
    double g,
    double b,
    double alpha,
    double rotation_degrees,
    nanopdf_watermark_position position,
    nanopdf_watermark_layer layer);
nanopdf_status nanopdf_writer_add_page_image_watermark(
    nanopdf_writer* writer,
    uint32_t page_index,
    const char* image_name,
    double alpha,
    double scale,
    nanopdf_watermark_position position,
    nanopdf_watermark_layer layer,
    double offset_x,
    double offset_y);
nanopdf_status nanopdf_writer_clear_watermark(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_set_header(
    nanopdf_writer* writer,
    const nanopdf_header_config* config);
nanopdf_status nanopdf_writer_set_footer(
    nanopdf_writer* writer,
    const nanopdf_footer_config* config);
nanopdf_status nanopdf_writer_set_page_header(
    nanopdf_writer* writer,
    uint32_t page_index,
    const nanopdf_header_config* config);
nanopdf_status nanopdf_writer_set_page_footer(
    nanopdf_writer* writer,
    uint32_t page_index,
    const nanopdf_footer_config* config);
nanopdf_status nanopdf_writer_clear_header(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_clear_footer(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_skip_header_footer(
    nanopdf_writer* writer,
    uint32_t page_index);
nanopdf_status nanopdf_writer_set_bates_numbering(
    nanopdf_writer* writer,
    const nanopdf_bates_config* config);
nanopdf_status nanopdf_writer_clear_bates_numbering(
    nanopdf_writer* writer);
nanopdf_status nanopdf_writer_skip_bates_number(
    nanopdf_writer* writer,
    uint32_t page_index);
nanopdf_status nanopdf_writer_add_layer(
    nanopdf_writer* writer,
    const char* name,
    int visible,
    char** out_layer_name);
nanopdf_status nanopdf_writer_add_layer_config(
    nanopdf_writer* writer,
    const nanopdf_layer_config* config,
    char** out_layer_name);
nanopdf_status nanopdf_writer_add_bookmark(
    nanopdf_writer* writer,
    const char* title,
    uint32_t page_index,
    double dest_y,
    int* out_bookmark_id);
nanopdf_status nanopdf_writer_add_bookmark_config(
    nanopdf_writer* writer,
    const nanopdf_bookmark_config* config,
    int* out_bookmark_id);
nanopdf_status nanopdf_writer_add_child_bookmark(
    nanopdf_writer* writer,
    int parent_bookmark_id,
    const char* title,
    uint32_t page_index,
    double dest_y,
    int* out_bookmark_id);
nanopdf_status nanopdf_writer_add_child_bookmark_config(
    nanopdf_writer* writer,
    int parent_bookmark_id,
    const nanopdf_bookmark_config* config,
    int* out_bookmark_id);
nanopdf_status nanopdf_writer_add_attachment(
    nanopdf_writer* writer,
    const nanopdf_attachment_config* config);
nanopdf_status nanopdf_writer_add_attachment_from_memory(
    nanopdf_writer* writer,
    const char* filename,
    const void* data,
    size_t size,
    const char* description);
nanopdf_status nanopdf_writer_add_attachment_from_file(
    nanopdf_writer* writer,
    const char* path,
    const char* description);
nanopdf_status nanopdf_writer_add_text_field(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* default_value);
nanopdf_status nanopdf_writer_add_checkbox(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    double x,
    double y,
    double size,
    int checked,
    const char* export_value);
nanopdf_status nanopdf_writer_add_dropdown(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* const* options,
    size_t option_count,
    int32_t selected_index,
    int editable);
nanopdf_status nanopdf_writer_add_radio_group(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    const nanopdf_radio_option* options,
    size_t option_count,
    int32_t selected_index);
nanopdf_status nanopdf_writer_add_listbox(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* const* options,
    size_t option_count,
    int32_t selected_index);
nanopdf_status nanopdf_writer_add_button(
    nanopdf_writer* writer,
    const char* name,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* caption);
nanopdf_status nanopdf_writer_add_signature_field(
    nanopdf_writer* writer,
    const nanopdf_signature_field_config* config);
void nanopdf_default_signature_field_config(
    nanopdf_signature_field_config* config);
nanopdf_status nanopdf_writer_set_signature_permissions(
    nanopdf_writer* writer,
    nanopdf_mdp_permissions permissions);
nanopdf_status nanopdf_writer_set_timestamp_config(
    nanopdf_writer* writer,
    const nanopdf_timestamp_config* config);
void nanopdf_default_timestamp_config(
    nanopdf_timestamp_config* config);
nanopdf_status nanopdf_writer_has_timestamp_config(
    nanopdf_writer* writer,
    int* out_has_timestamp);
nanopdf_status nanopdf_writer_get_signature_placeholder_count(
    nanopdf_writer* writer,
    size_t* out_count);
nanopdf_status nanopdf_writer_get_signature_placeholder(
    nanopdf_writer* writer,
    size_t index,
    nanopdf_signature_placeholder* out_placeholder);
nanopdf_status nanopdf_writer_add_linear_gradient(
    nanopdf_writer* writer,
    double x1,
    double y1,
    double x2,
    double y2,
    const nanopdf_color_stop* stops,
    size_t stop_count,
    int extend_start,
    int extend_end,
    char** out_gradient_name);
nanopdf_status nanopdf_writer_add_radial_gradient(
    nanopdf_writer* writer,
    double cx,
    double cy,
    double radius,
    const nanopdf_color_stop* stops,
    size_t stop_count,
    int extend_start,
    int extend_end,
    char** out_gradient_name);
nanopdf_status nanopdf_writer_text_layout_create(
    nanopdf_writer* writer,
    nanopdf_writer_text_layout** out_layout);
void nanopdf_writer_text_layout_destroy(
    nanopdf_writer_text_layout* layout);
nanopdf_status nanopdf_writer_text_layout_set_width(
    nanopdf_writer_text_layout* layout,
    double width);
nanopdf_status nanopdf_writer_text_layout_set_max_height(
    nanopdf_writer_text_layout* layout,
    double height);
nanopdf_status nanopdf_writer_text_layout_set_alignment(
    nanopdf_writer_text_layout* layout,
    nanopdf_writer_text_align alignment);
nanopdf_status nanopdf_writer_text_layout_set_style(
    nanopdf_writer_text_layout* layout,
    const nanopdf_writer_text_style* style);
nanopdf_status nanopdf_writer_text_layout_add_text(
    nanopdf_writer_text_layout* layout,
    const char* text);
nanopdf_status nanopdf_writer_text_layout_add_line_break(
    nanopdf_writer_text_layout* layout);
nanopdf_status nanopdf_writer_text_layout_add_paragraph_break(
    nanopdf_writer_text_layout* layout);
nanopdf_status nanopdf_writer_text_layout_get_height(
    const nanopdf_writer_text_layout* layout,
    double* out_height);
nanopdf_status nanopdf_writer_text_layout_get_line_count(
    const nanopdf_writer_text_layout* layout,
    int32_t* out_line_count);
nanopdf_status nanopdf_writer_text_layout_has_overflow(
    const nanopdf_writer_text_layout* layout,
    int* out_has_overflow);
nanopdf_status nanopdf_writer_table_create(
    nanopdf_context* context,
    nanopdf_writer_table** out_table);
void nanopdf_default_writer_table_cell_style(
    nanopdf_writer_table_cell_style* style);
void nanopdf_default_writer_table_cell(
    nanopdf_writer_table_cell* cell);
void nanopdf_default_writer_table_row(
    nanopdf_writer_table_row* row);
void nanopdf_writer_table_destroy(
    nanopdf_writer_table* table);
nanopdf_status nanopdf_writer_table_set_position(
    nanopdf_writer_table* table,
    double x,
    double y);
nanopdf_status nanopdf_writer_table_set_width(
    nanopdf_writer_table* table,
    double width);
nanopdf_status nanopdf_writer_table_set_column_widths(
    nanopdf_writer_table* table,
    const double* widths,
    size_t width_count);
nanopdf_status nanopdf_writer_table_set_font(
    nanopdf_writer_table* table,
    const char* font_name,
    double font_size);
nanopdf_status nanopdf_writer_table_set_text_color(
    nanopdf_writer_table* table,
    double r,
    double g,
    double b);
nanopdf_status nanopdf_writer_table_set_header_style(
    nanopdf_writer_table* table,
    const nanopdf_writer_table_cell_style* style);
nanopdf_status nanopdf_writer_table_set_border(
    nanopdf_writer_table* table,
    double width,
    double r,
    double g,
    double b);
nanopdf_status nanopdf_writer_table_set_outer_border(
    nanopdf_writer_table* table,
    int enabled);
nanopdf_status nanopdf_writer_table_set_inner_borders(
    nanopdf_writer_table* table,
    int enabled);
nanopdf_status nanopdf_writer_table_set_alternating_rows(
    nanopdf_writer_table* table,
    int enabled,
    double r,
    double g,
    double b);
nanopdf_status nanopdf_writer_table_add_header_row(
    nanopdf_writer_table* table,
    const char* const* cells,
    size_t cell_count);
nanopdf_status nanopdf_writer_table_add_header_row_cells(
    nanopdf_writer_table* table,
    const nanopdf_writer_table_cell* cells,
    size_t cell_count);
nanopdf_status nanopdf_writer_table_add_header_row_config(
    nanopdf_writer_table* table,
    const nanopdf_writer_table_row* row);
nanopdf_status nanopdf_writer_table_add_row(
    nanopdf_writer_table* table,
    const char* const* cells,
    size_t cell_count);
nanopdf_status nanopdf_writer_table_add_row_cells(
    nanopdf_writer_table* table,
    const nanopdf_writer_table_cell* cells,
    size_t cell_count);
nanopdf_status nanopdf_writer_table_add_row_config(
    nanopdf_writer_table* table,
    const nanopdf_writer_table_row* row);
nanopdf_status nanopdf_writer_table_calculate_height(
    const nanopdf_writer_table* table,
    double* out_height);
nanopdf_status nanopdf_writer_begin_template(
    nanopdf_writer* writer,
    double width,
    double height,
    nanopdf_page_builder** out_template);

nanopdf_status nanopdf_writer_begin_page(
    nanopdf_writer* writer,
    double width,
    double height,
    nanopdf_page_builder** out_page);
nanopdf_status nanopdf_page_builder_close(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_close_template(
    nanopdf_page_builder* page,
    char** out_template_name);
void nanopdf_page_builder_discard(nanopdf_page_builder* page);

nanopdf_status nanopdf_page_builder_save_state(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_restore_state(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_translate(
    nanopdf_page_builder* page,
    double tx,
    double ty);
nanopdf_status nanopdf_page_builder_scale(
    nanopdf_page_builder* page,
    double sx,
    double sy);
nanopdf_status nanopdf_page_builder_rotate(
    nanopdf_page_builder* page,
    double angle_degrees);
nanopdf_status nanopdf_page_builder_concat_matrix(
    nanopdf_page_builder* page,
    double a,
    double b,
    double c,
    double d,
    double e,
    double f);

nanopdf_status nanopdf_page_builder_set_stroke_color_rgb(
    nanopdf_page_builder* page,
    double r,
    double g,
    double b);
nanopdf_status nanopdf_page_builder_set_fill_color_rgb(
    nanopdf_page_builder* page,
    double r,
    double g,
    double b);
nanopdf_status nanopdf_page_builder_set_stroke_gray(
    nanopdf_page_builder* page,
    double gray);
nanopdf_status nanopdf_page_builder_set_fill_gray(
    nanopdf_page_builder* page,
    double gray);
nanopdf_status nanopdf_page_builder_set_fill_alpha(
    nanopdf_page_builder* page,
    double alpha);
nanopdf_status nanopdf_page_builder_set_stroke_alpha(
    nanopdf_page_builder* page,
    double alpha);
nanopdf_status nanopdf_page_builder_set_blend_mode(
    nanopdf_page_builder* page,
    nanopdf_blend_mode mode);
nanopdf_status nanopdf_page_builder_reset_transparency(
    nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_set_line_width(
    nanopdf_page_builder* page,
    double width);
nanopdf_status nanopdf_page_builder_set_line_cap(
    nanopdf_page_builder* page,
    int cap);
nanopdf_status nanopdf_page_builder_set_line_join(
    nanopdf_page_builder* page,
    int join);
nanopdf_status nanopdf_page_builder_set_dash_pattern(
    nanopdf_page_builder* page,
    const double* pattern,
    size_t pattern_count,
    double phase);

nanopdf_status nanopdf_page_builder_move_to(
    nanopdf_page_builder* page,
    double x,
    double y);
nanopdf_status nanopdf_page_builder_line_to(
    nanopdf_page_builder* page,
    double x,
    double y);
nanopdf_status nanopdf_page_builder_curve_to(
    nanopdf_page_builder* page,
    double x1,
    double y1,
    double x2,
    double y2,
    double x3,
    double y3);
nanopdf_status nanopdf_page_builder_close_path(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_stroke(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_fill(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_fill_even_odd(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_fill_stroke(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_clip(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_clip_even_odd(nanopdf_page_builder* page);

nanopdf_status nanopdf_page_builder_rectangle(
    nanopdf_page_builder* page,
    double x,
    double y,
    double width,
    double height);
nanopdf_status nanopdf_page_builder_line(
    nanopdf_page_builder* page,
    double x1,
    double y1,
    double x2,
    double y2);
nanopdf_status nanopdf_page_builder_circle(
    nanopdf_page_builder* page,
    double cx,
    double cy,
    double radius);
nanopdf_status nanopdf_page_builder_ellipse(
    nanopdf_page_builder* page,
    double cx,
    double cy,
    double rx,
    double ry);
nanopdf_status nanopdf_page_builder_arc(
    nanopdf_page_builder* page,
    double cx,
    double cy,
    double rx,
    double ry,
    double start_angle,
    double end_angle);
nanopdf_status nanopdf_page_builder_rounded_rect(
    nanopdf_page_builder* page,
    double x,
    double y,
    double width,
    double height,
    double radius);

nanopdf_status nanopdf_page_builder_append_raw_content(
    nanopdf_page_builder* page,
    const char* raw_content);
nanopdf_status nanopdf_page_builder_add_resource_ref(
    nanopdf_page_builder* page,
    const char* category,
    const char* name,
    nanopdf_object_ref ref);
nanopdf_status nanopdf_page_builder_begin_text(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_end_text(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_set_font(
    nanopdf_page_builder* page,
    const char* font_name,
    double font_size);
nanopdf_status nanopdf_page_builder_text_position(
    nanopdf_page_builder* page,
    double x,
    double y);
nanopdf_status nanopdf_page_builder_show_text(
    nanopdf_page_builder* page,
    const char* text);
nanopdf_status nanopdf_page_builder_show_text_at(
    nanopdf_page_builder* page,
    double x,
    double y,
    const char* text);
nanopdf_status nanopdf_page_builder_draw_text_layout(
    nanopdf_page_builder* page,
    const nanopdf_writer_text_layout* layout,
    double x,
    double y);
nanopdf_status nanopdf_page_builder_draw_table(
    nanopdf_page_builder* page,
    const nanopdf_writer_table* table);
nanopdf_status nanopdf_page_builder_draw_image(
    nanopdf_page_builder* page,
    const char* image_name,
    double x,
    double y,
    double width,
    double height);
nanopdf_status nanopdf_page_builder_draw_template(
    nanopdf_page_builder* page,
    const char* template_name,
    double x,
    double y,
    double scale_x,
    double scale_y);
nanopdf_status nanopdf_page_builder_add_link_uri(
    nanopdf_page_builder* page,
    double x,
    double y,
    double width,
    double height,
    const char* uri);
nanopdf_status nanopdf_page_builder_add_link_config(
    nanopdf_page_builder* page,
    const nanopdf_link_config* config);
nanopdf_status nanopdf_page_builder_add_link_goto(
    nanopdf_page_builder* page,
    double x,
    double y,
    double width,
    double height,
    uint32_t dest_page_index,
    double dest_y);
nanopdf_status nanopdf_page_builder_add_highlight(
    nanopdf_page_builder* page,
    double x,
    double y,
    double width,
    double height,
    double r,
    double g,
    double b,
    double alpha);
nanopdf_status nanopdf_page_builder_add_text_markup(
    nanopdf_page_builder* page,
    const nanopdf_text_markup_config* config);
nanopdf_status nanopdf_page_builder_begin_layer(
    nanopdf_page_builder* page,
    const char* layer_name);
nanopdf_status nanopdf_page_builder_end_layer(nanopdf_page_builder* page);
nanopdf_status nanopdf_page_builder_set_fill_gradient(
    nanopdf_page_builder* page,
    const char* gradient_name);
nanopdf_status nanopdf_page_builder_set_stroke_gradient(
    nanopdf_page_builder* page,
    const char* gradient_name);

nanopdf_status nanopdf_object_create_null(
    nanopdf_context* context,
    nanopdf_object** out_object);
nanopdf_status nanopdf_object_create_bool(
    nanopdf_context* context,
    int value,
    nanopdf_object** out_object);
nanopdf_status nanopdf_object_create_number(
    nanopdf_context* context,
    double value,
    nanopdf_object** out_object);
nanopdf_status nanopdf_object_create_string(
    nanopdf_context* context,
    const char* value,
    nanopdf_object** out_object);
nanopdf_status nanopdf_object_create_name(
    nanopdf_context* context,
    const char* value,
    nanopdf_object** out_object);
nanopdf_status nanopdf_object_create_array(
    nanopdf_context* context,
    nanopdf_object** out_object);
nanopdf_status nanopdf_object_create_dict(
    nanopdf_context* context,
    nanopdf_object** out_object);
nanopdf_status nanopdf_object_create_stream(
    nanopdf_context* context,
    nanopdf_object** out_object);
nanopdf_status nanopdf_object_create_ref(
    nanopdf_context* context,
    nanopdf_object_ref ref,
    nanopdf_object** out_object);
void nanopdf_object_destroy(nanopdf_object* object);

nanopdf_status nanopdf_object_array_append(
    nanopdf_object* array_object,
    const nanopdf_object* value);
nanopdf_status nanopdf_object_dict_set(
    nanopdf_object* dict_or_stream_object,
    const char* key,
    const nanopdf_object* value);
nanopdf_status nanopdf_object_stream_set_data(
    nanopdf_object* stream_object,
    const void* data,
    size_t size);

nanopdf_status nanopdf_writer_add_object(
    nanopdf_writer* writer,
    const nanopdf_object* object,
    nanopdf_object_ref* out_ref);
nanopdf_status nanopdf_writer_set_field_value(
    nanopdf_writer* writer,
    const char* field_name,
    const char* value);
nanopdf_status nanopdf_writer_set_field_checked(
    nanopdf_writer* writer,
    const char* field_name,
    int checked);
nanopdf_status nanopdf_writer_set_field_choice(
    nanopdf_writer* writer,
    const char* field_name,
    const char* value);
nanopdf_status nanopdf_writer_add_existing_text_annotation(
    nanopdf_writer* writer,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* contents);
nanopdf_status nanopdf_writer_add_existing_text_markup(
    nanopdf_writer* writer,
    uint32_t page_index,
    const nanopdf_text_markup_config* config);
nanopdf_status nanopdf_writer_add_existing_link_uri(
    nanopdf_writer* writer,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    const char* uri);
nanopdf_status nanopdf_writer_add_existing_link_config(
    nanopdf_writer* writer,
    uint32_t page_index,
    const nanopdf_link_config* config);
nanopdf_status nanopdf_writer_add_existing_link_goto(
    nanopdf_writer* writer,
    uint32_t page_index,
    double x,
    double y,
    double width,
    double height,
    uint32_t dest_page_index,
    double dest_y);
nanopdf_status nanopdf_writer_delete_existing_annotation(
    nanopdf_writer* writer,
    uint32_t page_index,
    uint32_t annotation_index);
nanopdf_status nanopdf_writer_write_file(
    nanopdf_writer* writer,
    const char* path);
nanopdf_status nanopdf_writer_write_memory(
    nanopdf_writer* writer,
    void** out_data,
    size_t* out_size);
nanopdf_status nanopdf_writer_write_incremental_file(
    nanopdf_writer* writer,
    const char* path);
nanopdf_status nanopdf_writer_write_incremental_memory(
    nanopdf_writer* writer,
    void** out_data,
    size_t* out_size);
nanopdf_status nanopdf_writer_write_incremental_for_signing_memory(
    nanopdf_writer* writer,
    size_t reserved_signature_size,
    void** out_data,
    size_t* out_size);
nanopdf_status nanopdf_writer_write_for_signing_memory(
    nanopdf_writer* writer,
    size_t reserved_signature_size,
    void** out_data,
    size_t* out_size);
void nanopdf_default_writer_encryption_config(
    nanopdf_writer_encryption_config* config);
nanopdf_status nanopdf_writer_set_encryption(
    nanopdf_writer* writer,
    const nanopdf_writer_encryption_config* config);
nanopdf_status nanopdf_writer_is_encrypted(
    nanopdf_writer* writer,
    int* out_is_encrypted);
nanopdf_status nanopdf_writer_get_encryption_algorithm(
    nanopdf_writer* writer,
    nanopdf_encryption_algorithm* out_algorithm);
nanopdf_status nanopdf_split_pdf_memory(
    nanopdf_context* context,
    const void* data,
    size_t size,
    const int32_t* page_indices,
    size_t page_index_count,
    void** out_data,
    size_t* out_size);
nanopdf_status nanopdf_merge_pdfs_memory(
    nanopdf_context* context,
    const nanopdf_buffer_view* inputs,
    size_t input_count,
    void** out_data,
    size_t* out_size);
nanopdf_status nanopdf_apply_redactions_memory(
    nanopdf_context* context,
    const void* data,
    size_t size,
    void** out_data,
    size_t* out_size);
nanopdf_status nanopdf_apply_precomputed_signature(
    nanopdf_context* context,
    const void* pdf_data,
    size_t pdf_size,
    const nanopdf_signature_placeholder* placeholder,
    const void* signature,
    size_t signature_size,
    void** out_signed_data,
    size_t* out_signed_size);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_WRITE_H_ */
