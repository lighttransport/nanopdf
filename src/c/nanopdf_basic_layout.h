#ifndef NANOPDF_BASIC_LAYOUT_H_
#define NANOPDF_BASIC_LAYOUT_H_

#include "nanopdf_c_internal.h"
#include "nanopdf_text.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nanopdf_basic_layout_char {
  uint32_t unicode;
  double x;
  double y;
  double width;
  double height;
  int32_t line_index;
  int32_t word_index;
} nanopdf_basic_layout_char;

typedef struct nanopdf_basic_layout {
  char* text;
  nanopdf_basic_layout_char* chars;
  size_t char_count;
  size_t char_capacity;
  size_t line_count;
  size_t word_count;
  double page_width;
  double page_height;
} nanopdf_basic_layout;

void nanopdf_basic_layout_init(nanopdf_basic_layout* layout);
void nanopdf_basic_layout_destroy(
    const nanopdf_allocator* allocator,
    nanopdf_basic_layout* layout);
nanopdf_status nanopdf_basic_layout_build(
    nanopdf_context* context,
    const char* text,
    double page_width,
    double page_height,
    nanopdf_basic_layout* out_layout);
nanopdf_status nanopdf_basic_layout_copy_text_in_rect(
    nanopdf_context* context,
    const nanopdf_basic_layout* layout,
    double x1,
    double y1,
    double x2,
    double y2,
    char** out_text);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_BASIC_LAYOUT_H_ */
