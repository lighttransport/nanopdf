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

nanopdf_status nanopdf_writer_set_title(
    nanopdf_writer* writer,
    const char* title);
nanopdf_status nanopdf_writer_set_author(
    nanopdf_writer* writer,
    const char* author);
nanopdf_status nanopdf_writer_set_subject(
    nanopdf_writer* writer,
    const char* subject);
nanopdf_status nanopdf_writer_set_creator(
    nanopdf_writer* writer,
    const char* creator);
nanopdf_status nanopdf_writer_add_standard_font(
    nanopdf_writer* writer,
    nanopdf_standard_font font,
    char** out_font_name);
nanopdf_status nanopdf_writer_add_image_from_memory(
    nanopdf_writer* writer,
    const void* data,
    size_t size,
    nanopdf_image_compression compression,
    char** out_image_name);
nanopdf_status nanopdf_writer_add_image_from_file(
    nanopdf_writer* writer,
    const char* path,
    nanopdf_image_compression compression,
    char** out_image_name);

nanopdf_status nanopdf_writer_begin_page(
    nanopdf_writer* writer,
    double width,
    double height,
    nanopdf_page_builder** out_page);
nanopdf_status nanopdf_page_builder_close(nanopdf_page_builder* page);
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
nanopdf_status nanopdf_page_builder_draw_image(
    nanopdf_page_builder* page,
    const char* image_name,
    double x,
    double y,
    double width,
    double height);

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

nanopdf_status nanopdf_writer_write_file(
    nanopdf_writer* writer,
    const char* path);
nanopdf_status nanopdf_writer_write_memory(
    nanopdf_writer* writer,
    void** out_data,
    size_t* out_size);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_WRITE_H_ */
