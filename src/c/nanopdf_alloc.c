#include "nanopdf_c_internal.h"

#include <stdlib.h>

static void* nanopdf__libc_alloc(void* user_data, size_t size) {
  (void)user_data;
  return malloc(size);
}

static void* nanopdf__libc_realloc(void* user_data, void* ptr, size_t size) {
  (void)user_data;
  return realloc(ptr, size);
}

static void nanopdf__libc_free(void* user_data, void* ptr) {
  (void)user_data;
  free(ptr);
}

void nanopdf_default_context_options(nanopdf_context_options* options) {
  if (!options) {
    return;
  }

  options->struct_size = sizeof(*options);
  options->allocator.user_data = NULL;
  options->allocator.alloc = nanopdf__libc_alloc;
  options->allocator.realloc = nanopdf__libc_realloc;
  options->allocator.free = nanopdf__libc_free;
}

void nanopdf_default_parse_options(nanopdf_parse_options* options) {
  if (!options) {
    return;
  }

  options->auto_repair = 0;
  options->recover_stream_length = 0;
  options->max_repair_scan = 0;
  options->password = NULL;
}

void nanopdf_default_text_layout_options(nanopdf_text_layout_options* options) {
  if (!options) {
    return;
  }

  options->baseline_tolerance = 2.0;
  options->line_spacing_threshold = 1.5;
  options->word_spacing_threshold = 2.0;
  options->column_gap_threshold = 20.0;
  options->detect_columns = 1;
  options->detect_rtl = 1;
}

void nanopdf_default_table_extraction_options(
    nanopdf_table_extraction_options* options) {
  if (!options) {
    return;
  }

  options->alignment_tolerance = 2.0;
  options->min_rows = 2;
  options->min_cols = 2;
  options->max_cell_gap = 50.0;
  options->min_chars_per_cell = 1;
  options->debug = 0;
}

void* nanopdf__allocator_alloc(const nanopdf_allocator* allocator, size_t size) {
  if (!allocator || !allocator->alloc) {
    return NULL;
  }

  if (size == 0) {
    size = 1;
  }

  return allocator->alloc(allocator->user_data, size);
}

void* nanopdf__allocator_realloc(
    const nanopdf_allocator* allocator,
    void* ptr,
    size_t size) {
  if (!allocator || !allocator->realloc) {
    return NULL;
  }

  if (size == 0) {
    size = 1;
  }

  return allocator->realloc(allocator->user_data, ptr, size);
}

void nanopdf__allocator_free(const nanopdf_allocator* allocator, void* ptr) {
  if (!allocator || !allocator->free || !ptr) {
    return;
  }

  allocator->free(allocator->user_data, ptr);
}
