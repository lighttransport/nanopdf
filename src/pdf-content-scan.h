// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#ifndef NANOPDF_PDF_CONTENT_SCAN_H
#define NANOPDF_PDF_CONTENT_SCAN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum npdf_content_token_kind {
  NPDF_CONTENT_TOKEN_EOF = 0,
  NPDF_CONTENT_TOKEN_ATOM = 1,
  NPDF_CONTENT_TOKEN_LITERAL_STRING = 2,
  NPDF_CONTENT_TOKEN_HEX_STRING = 3,
  NPDF_CONTENT_TOKEN_DICTIONARY = 4,
  NPDF_CONTENT_TOKEN_ARRAY_START = 5,
  NPDF_CONTENT_TOKEN_ARRAY_END = 6
} npdf_content_token_kind;

typedef struct npdf_content_token {
  const char* ptr;
  size_t len;
  npdf_content_token_kind kind;
} npdf_content_token;

typedef struct npdf_content_scanner {
  const char* data;
  size_t size;
  size_t pos;
} npdf_content_scanner;

typedef enum npdf_inline_image_color_space {
  NPDF_INLINE_IMAGE_GRAY = 0,
  NPDF_INLINE_IMAGE_RGB = 1,
  NPDF_INLINE_IMAGE_CMYK = 2
} npdf_inline_image_color_space;

typedef enum npdf_inline_image_filter {
  NPDF_INLINE_IMAGE_FILTER_NONE = 0,
  NPDF_INLINE_IMAGE_FILTER_ASCII_HEX = 1,
  NPDF_INLINE_IMAGE_FILTER_ASCII85 = 2,
  NPDF_INLINE_IMAGE_FILTER_FLATE = 3,
  NPDF_INLINE_IMAGE_FILTER_LZW = 4,
  NPDF_INLINE_IMAGE_FILTER_UNKNOWN = 5
} npdf_inline_image_filter;

typedef struct npdf_inline_image_info {
  int width;
  int height;
  int bits_per_component;
  int interpolate;
  int components;
  npdf_inline_image_color_space color_space;
  npdf_inline_image_filter filter;
  size_t expected_data_size;
  size_t data_start;
  size_t data_end;
  size_t end_pos;
} npdf_inline_image_info;

void npdf_content_scanner_init(npdf_content_scanner* scanner,
                               const void* data,
                               size_t size);

int npdf_content_next_token(npdf_content_scanner* scanner,
                            npdf_content_token* token);

int npdf_parse_double_span(const char* ptr, size_t len, double* out);
int npdf_parse_float_span(const char* ptr, size_t len, float* out);

int npdf_parse_inline_image_info(const char* data,
                                 size_t size,
                                 size_t start_pos,
                                 npdf_inline_image_info* out);

#ifdef __cplusplus
}
#endif

#endif  // NANOPDF_PDF_CONTENT_SCAN_H
