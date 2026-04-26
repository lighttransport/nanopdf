#include "nanopdf_jbig2.h"
#include "nanopdf_image_decoder.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define NANOPDF_JBIG2_MAX_IMAGE_SIZE 0x40000000
#define NANOPDF_JBIG2_MAX_STREAM_SIZE 0x40000000u
#define NANOPDF_JBIG2_DEFAULT_A 0x8000u
#define NANOPDF_JBIG2_OOB 1
#define NANOPDF_JBIG2_SEGMENT_SYMBOL_DICTIONARY 0u
#define NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_TEXT_REGION 4u
#define NANOPDF_JBIG2_SEGMENT_IMMEDIATE_TEXT_REGION 6u
#define NANOPDF_JBIG2_SEGMENT_IMMEDIATE_LOSSLESS_TEXT_REGION 7u
#define NANOPDF_JBIG2_SEGMENT_PATTERN_DICTIONARY 16u
#define NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_HALFTONE_REGION 20u
#define NANOPDF_JBIG2_SEGMENT_IMMEDIATE_HALFTONE_REGION 22u
#define NANOPDF_JBIG2_SEGMENT_IMMEDIATE_LOSSLESS_HALFTONE_REGION 23u
#define NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_GENERIC_REGION 36u
#define NANOPDF_JBIG2_SEGMENT_IMMEDIATE_GENERIC_REGION 38u
#define NANOPDF_JBIG2_SEGMENT_IMMEDIATE_LOSSLESS_GENERIC_REGION 39u
#define NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_GENERIC_REFINEMENT_REGION 40u
#define NANOPDF_JBIG2_SEGMENT_IMMEDIATE_GENERIC_REFINEMENT_REGION 42u
#define NANOPDF_JBIG2_SEGMENT_IMMEDIATE_LOSSLESS_GENERIC_REFINEMENT_REGION 43u
#define NANOPDF_JBIG2_SEGMENT_PAGE_INFO 48u
#define NANOPDF_JBIG2_SEGMENT_END_OF_PAGE 49u
#define NANOPDF_JBIG2_SEGMENT_END_OF_STRIPE 50u
#define NANOPDF_JBIG2_SEGMENT_END_OF_FILE 51u
#define NANOPDF_JBIG2_SEGMENT_PROFILES 52u
#define NANOPDF_JBIG2_SEGMENT_TABLES 53u
#define NANOPDF_JBIG2_SEGMENT_EXTENSION 62u

static size_t jbig2_gb_context_count(uint8_t gb_template) {
  static const size_t context_counts[4] = {65536u, 8192u, 4096u, 1024u};
  return context_counts[gb_template < 4u ? gb_template : 0u];
}

static size_t jbig2_gr_context_count(uint8_t gr_template) {
  return gr_template == 0u ? 8192u : 1024u;
}

static int jbig2_text_region_is_empty_supported(
    const nanopdf_jbig2_text_region_header* header) {
  return header && header->num_instances == 0u;
}

typedef struct jbig2_segment_table {
  nanopdf_jbig2_segment* segments;
  size_t count;
  size_t capacity;
} jbig2_segment_table;

static void jbig2_segment_table_init(jbig2_segment_table* table) {
  if (table) {
    memset(table, 0, sizeof(*table));
  }
}

static void jbig2_segment_table_destroy(nanopdf_context* context, jbig2_segment_table* table) {
  size_t i;
  if (!table) {
    return;
  }
  for (i = 0; i < table->count; ++i) {
    nanopdf_jbig2_segment_destroy(context, &table->segments[i]);
  }
  if (context) {
    nanopdf__allocator_free(&context->allocator, table->segments);
  }
  memset(table, 0, sizeof(*table));
}

static int jbig2_segment_table_add(
    nanopdf_context* context,
    jbig2_segment_table* table,
    nanopdf_jbig2_segment* segment) {
  nanopdf_jbig2_segment* resized;
  size_t new_capacity;
  if (!context || !table || !segment) {
    return 0;
  }
  if (table->count >= table->capacity) {
    new_capacity = table->capacity == 0 ? 8u : table->capacity * 2u;
    resized = (nanopdf_jbig2_segment*)nanopdf__allocator_realloc(
        &context->allocator, table->segments, new_capacity * sizeof(resized[0]));
    if (!resized) {
      return 0;
    }
    memset(resized + table->capacity, 0, (new_capacity - table->capacity) * sizeof(resized[0]));
    table->segments = resized;
    table->capacity = new_capacity;
  }
  table->segments[table->count] = *segment;
  table->count++;
  nanopdf_jbig2_segment_init(segment);
  return 1;
}

static const nanopdf_jbig2_segment* jbig2_segment_table_find(
    const jbig2_segment_table* table,
    uint32_t number) {
  size_t i;
  if (!table) {
    return NULL;
  }
  for (i = 0; i < table->count; ++i) {
    if (table->segments[i].number == number) {
      return &table->segments[i];
    }
  }
  return NULL;
}

static size_t jbig2_segment_referenced_symbol_count(
    const jbig2_segment_table* table,
    const nanopdf_jbig2_segment* segment) {
  size_t count = 0;
  int32_t i;
  if (!table || !segment) {
    return 0;
  }
  for (i = 0; i < segment->referred_to_segment_count; ++i) {
    const nanopdf_jbig2_segment* ref =
        jbig2_segment_table_find(table, segment->referred_to_segment_numbers[i]);
    if (ref && ref->result_type == NANOPDF_JBIG2_SYMBOL_DICT_POINTER && ref->symbol_dict) {
      count += nanopdf_jbig2_symbol_dict_count(ref->symbol_dict);
    }
  }
  return count;
}

static int jbig2_segment_referenced_symbol_dicts_resolved(
    const jbig2_segment_table* table,
    const nanopdf_jbig2_segment* segment) {
  int32_t i;
  if (!table || !segment) {
    return 0;
  }
  for (i = 0; i < segment->referred_to_segment_count; ++i) {
    const nanopdf_jbig2_segment* ref =
        jbig2_segment_table_find(table, segment->referred_to_segment_numbers[i]);
    if (!ref || ref->result_type != NANOPDF_JBIG2_SYMBOL_DICT_POINTER || !ref->symbol_dict) {
      return 0;
    }
  }
  return 1;
}

static const nanopdf_jbig2_image* jbig2_segment_referenced_image(
    const jbig2_segment_table* table,
    const nanopdf_jbig2_segment* segment) {
  int32_t i;
  if (!table || !segment) {
    return NULL;
  }
  for (i = 0; i < segment->referred_to_segment_count; ++i) {
    const nanopdf_jbig2_segment* ref =
        jbig2_segment_table_find(table, segment->referred_to_segment_numbers[i]);
    if (ref && ref->result_type == NANOPDF_JBIG2_IMAGE_POINTER && ref->image) {
      return ref->image;
    }
  }
  return NULL;
}

static int jbig2_process_symbol_dict_segment(
    nanopdf_context* context,
    nanopdf_jbig2_bitstream* stream,
    jbig2_segment_table* segment_table,
    nanopdf_jbig2_segment* segment,
    int* unsupported_segment) {
  nanopdf_jbig2_symbol_dict_header symbol_header;
  if (!context || !stream || !segment_table || !segment || !unsupported_segment ||
      !nanopdf_jbig2_parse_symbol_dict_header(stream, &symbol_header)) {
    return 0;
  }
  if (symbol_header.num_exported_symbols != 0u || symbol_header.num_new_symbols != 0u) {
    *unsupported_segment = 1;
    return 1;
  }
  segment->symbol_dict = (nanopdf_jbig2_symbol_dict*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(segment->symbol_dict[0]));
  if (!segment->symbol_dict) {
    return 0;
  }
  nanopdf_jbig2_symbol_dict_init(segment->symbol_dict);
  segment->result_type = NANOPDF_JBIG2_SYMBOL_DICT_POINTER;
  segment->state = NANOPDF_JBIG2_SEGMENT_PARSE_COMPLETE;
  return jbig2_segment_table_add(context, segment_table, segment);
}

static int jbig2_process_pattern_dict_segment(
    nanopdf_context* context,
    nanopdf_jbig2_bitstream* stream,
    jbig2_segment_table* segment_table,
    nanopdf_jbig2_segment* segment) {
  nanopdf_jbig2_pattern_dict_header pattern_header;
  uint32_t data_end = 0;
  uint32_t count;
  if (!context || !stream || !segment_table || !segment ||
      !nanopdf_jbig2_parse_pattern_dict_header(stream, &pattern_header)) {
    return 0;
  }
  segment->pattern_dict = (nanopdf_jbig2_pattern_dict*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(segment->pattern_dict[0]));
  if (!segment->pattern_dict) {
    return 0;
  }
  nanopdf_jbig2_pattern_dict_init(segment->pattern_dict);
  nanopdf_jbig2_pattern_dict_set_dimensions(
      segment->pattern_dict, pattern_header.width, pattern_header.height);

  count = pattern_header.gray_max + 1u;
  if (segment->data_length != 0xffffffffu &&
      segment->data_offset <= UINT32_MAX - segment->data_length) {
    data_end = segment->data_offset + segment->data_length;
  }
  if (data_end > 0u && nanopdf_jbig2_bitstream_offset(stream) < data_end) {
    nanopdf_jbig2_grd_proc grd;
    nanopdf_jbig2_image collective;
    uint32_t collective_width;
    uint32_t i;
    int decoded = 0;
    if (pattern_header.width == 0u || pattern_header.height == 0u ||
        count == 0u || count > (uint32_t)INT32_MAX ||
        pattern_header.width > (uint32_t)INT32_MAX / count) {
      return 0;
    }
    collective_width = pattern_header.width * count;
    nanopdf_jbig2_grd_proc_init(&grd);
    grd.mmr = pattern_header.mmr;
    grd.gb_template = pattern_header.template_id;
    grd.width = collective_width;
    grd.height = pattern_header.height;
    if (!grd.mmr) {
      int template_bytes = grd.gb_template == 0u ? 8 : 2;
      int ati;
      for (ati = 0; ati < template_bytes; ++ati) {
        uint8_t value = 0;
        if (nanopdf_jbig2_bitstream_read_byte(stream, &value) != 0) {
          return 0;
        }
        grd.gbat[ati] = (int8_t)value;
      }
    }
    nanopdf_jbig2_image_init_external(&collective, 0, 0, 0, NULL);
    if (grd.mmr) {
      decoded = nanopdf_jbig2_grd_proc_decode_mmr(context, &grd, stream, &collective);
    } else {
      size_t context_count = jbig2_gb_context_count(grd.gb_template);
      nanopdf_jbig2_arith_ctx* gb_contexts =
          (nanopdf_jbig2_arith_ctx*)nanopdf__allocator_alloc(
              &context->allocator, sizeof(nanopdf_jbig2_arith_ctx) * context_count);
      if (gb_contexts) {
        size_t ci;
        nanopdf_jbig2_arith_decoder arith;
        for (ci = 0; ci < context_count; ++ci) {
          nanopdf_jbig2_arith_ctx_init(&gb_contexts[ci]);
        }
        nanopdf_jbig2_arith_decoder_init(&arith, stream);
        decoded = nanopdf_jbig2_grd_proc_decode_arith(
            context, &grd, &arith, gb_contexts, context_count, &collective);
        nanopdf__allocator_free(&context->allocator, gb_contexts);
      }
    }
    if (!decoded) {
      nanopdf_jbig2_image_destroy(context, &collective);
      return 0;
    }
    for (i = 0; i < count; ++i) {
      nanopdf_jbig2_image pattern;
      uint32_t y;
      nanopdf_jbig2_image_init_external(&pattern, 0, 0, 0, NULL);
      if (!nanopdf_jbig2_image_init(
              context,
              &pattern,
              (int32_t)pattern_header.width,
              (int32_t)pattern_header.height)) {
        nanopdf_jbig2_image_destroy(context, &collective);
        return 0;
      }
      for (y = 0; y < pattern_header.height; ++y) {
        uint32_t x;
        for (x = 0; x < pattern_header.width; ++x) {
          nanopdf_jbig2_image_set_pixel(
              &pattern,
              (int32_t)x,
              (int32_t)y,
              nanopdf_jbig2_image_get_pixel(
                  &collective,
                  (int32_t)(i * pattern_header.width + x),
                  (int32_t)y));
        }
      }
      if (!nanopdf_jbig2_pattern_dict_add(context, segment->pattern_dict, &pattern)) {
        nanopdf_jbig2_image_destroy(context, &pattern);
        nanopdf_jbig2_image_destroy(context, &collective);
        return 0;
      }
      nanopdf_jbig2_image_destroy(context, &pattern);
    }
    nanopdf_jbig2_image_destroy(context, &collective);
  }
  segment->result_type = NANOPDF_JBIG2_PATTERN_DICT_POINTER;
  segment->state = NANOPDF_JBIG2_SEGMENT_PARSE_COMPLETE;
  return jbig2_segment_table_add(context, segment_table, segment);
}

static int jbig2_process_table_segment(
    nanopdf_context* context,
    nanopdf_jbig2_bitstream* stream,
    jbig2_segment_table* segment_table,
    nanopdf_jbig2_segment* segment) {
  nanopdf_jbig2_huffman_table* table;
  if (!context || !stream || !segment_table || !segment) {
    return 0;
  }
  table = (nanopdf_jbig2_huffman_table*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(table[0]));
  if (!table) {
    return 0;
  }
  nanopdf_jbig2_huffman_table_init(table);
  if (!nanopdf_jbig2_huffman_table_init_encoded(context, table, stream)) {
    nanopdf_jbig2_huffman_table_destroy(context, table);
    nanopdf__allocator_free(&context->allocator, table);
    return 0;
  }
  segment->huffman_table = table;
  segment->result_type = NANOPDF_JBIG2_HUFFMAN_TABLE_POINTER;
  segment->state = NANOPDF_JBIG2_SEGMENT_PARSE_COMPLETE;
  return jbig2_segment_table_add(context, segment_table, segment);
}

static int jbig2_create_empty_text_region_image(
    nanopdf_context* context,
    const nanopdf_jbig2_text_region_header* text_header,
    nanopdf_jbig2_image* out_image) {
  if (!context || !text_header || !out_image ||
      !nanopdf_jbig2_image_init(
          context,
          out_image,
          (int32_t)text_header->region.width,
          (int32_t)text_header->region.height)) {
    return 0;
  }
  nanopdf_jbig2_image_fill(out_image, text_header->default_pixel ? 1 : 0);
  return 1;
}

static int jbig2_halftone_region_is_empty_supported(
    const nanopdf_jbig2_halftone_region_header* header) {
  return header && (header->grid_width == 0u || header->grid_height == 0u);
}

static int jbig2_create_empty_halftone_region_image(
    nanopdf_context* context,
    const nanopdf_jbig2_halftone_region_header* halftone_header,
    nanopdf_jbig2_image* out_image) {
  if (!context || !halftone_header || !out_image ||
      !nanopdf_jbig2_image_init(
          context,
          out_image,
          (int32_t)halftone_header->region.width,
          (int32_t)halftone_header->region.height)) {
    return 0;
  }
  nanopdf_jbig2_image_fill(out_image, halftone_header->default_pixel ? 1 : 0);
  return 1;
}

static int jbig2_create_empty_refinement_region_image(
    nanopdf_context* context,
    const nanopdf_jbig2_generic_refinement_region_header* refinement_header,
    nanopdf_jbig2_image* out_image) {
  if (!context || !refinement_header || !out_image ||
      !nanopdf_jbig2_image_init(
          context,
          out_image,
          (int32_t)refinement_header->region.width,
          (int32_t)refinement_header->region.height)) {
    return 0;
  }
  nanopdf_jbig2_image_fill(out_image, 0);
  return 1;
}

static uint32_t jbig2_grrd_template0_context(
    const nanopdf_jbig2_generic_refinement_region_header* header,
    const nanopdf_jbig2_image* image,
    const nanopdf_jbig2_image* reference,
    int32_t x,
    int32_t y) {
  uint32_t context = 0;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x + 1, y + 1);
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x, y + 1) << 1;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x - 1, y + 1) << 2;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x + 1, y) << 3;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x, y) << 4;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x - 1, y) << 5;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x + 1, y - 1) << 6;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x, y - 1) << 7;
  context |=
      (uint32_t)nanopdf_jbig2_image_get_pixel(
          reference, x + header->grat[2], y + header->grat[3])
      << 8;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(image, x - 1, y) << 9;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(image, x + 1, y - 1) << 10;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(image, x, y - 1) << 11;
  context |=
      (uint32_t)nanopdf_jbig2_image_get_pixel(image, x + header->grat[0], y + header->grat[1])
      << 12;
  return context;
}

static uint32_t jbig2_grrd_template1_context(
    const nanopdf_jbig2_generic_refinement_region_header* header,
    const nanopdf_jbig2_image* image,
    const nanopdf_jbig2_image* reference,
    int32_t x,
    int32_t y) {
  uint32_t context = 0;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x + 1, y);
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x, y) << 1;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x - 1, y) << 2;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x + 1, y - 1) << 3;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(reference, x, y - 1) << 4;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(image, x - 1, y) << 5;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(image, x + 1, y - 1) << 6;
  context |= (uint32_t)nanopdf_jbig2_image_get_pixel(image, x, y - 1) << 7;
  context |=
      (uint32_t)nanopdf_jbig2_image_get_pixel(image, x + header->grat[0], y + header->grat[1])
      << 8;
  return context;
}

static int jbig2_decode_generic_refinement_region_arith(
    nanopdf_context* context,
    const nanopdf_jbig2_generic_refinement_region_header* header,
    const nanopdf_jbig2_image* reference,
    nanopdf_jbig2_arith_decoder* arith,
    nanopdf_jbig2_arith_ctx* gr_contexts,
    size_t gr_context_count,
    nanopdf_jbig2_image* out_image) {
  int ltp = 0;
  uint32_t y;
  if (!context || !header || !reference || !arith || !gr_contexts || !out_image ||
      !nanopdf_jbig2_image_valid_size(
          (int32_t)header->region.width, (int32_t)header->region.height)) {
    return 0;
  }
  if (!nanopdf_jbig2_image_init(
          context, out_image, (int32_t)header->region.width, (int32_t)header->region.height)) {
    return 0;
  }
  nanopdf_jbig2_image_fill(out_image, 0);
  for (y = 0; y < header->region.height; ++y) {
    uint32_t x;
    if (header->typical_prediction) {
      size_t ltp_context = header->template_id == 0u ? 0x0010u : 0x0008u;
      if (arith->complete || ltp_context >= gr_context_count) {
        nanopdf_jbig2_image_destroy(context, out_image);
        return 0;
      }
      ltp ^= nanopdf_jbig2_arith_decode(arith, &gr_contexts[ltp_context]);
    }
    for (x = 0; x < header->region.width; ++x) {
      int bit;
      if (ltp) {
        bit = nanopdf_jbig2_image_get_pixel(reference, (int32_t)x, (int32_t)y);
      } else {
        uint32_t context_index =
            header->template_id == 0u
                ? jbig2_grrd_template0_context(
                      header, out_image, reference, (int32_t)x, (int32_t)y)
                : jbig2_grrd_template1_context(
                      header, out_image, reference, (int32_t)x, (int32_t)y);
        if (arith->complete || context_index >= gr_context_count) {
          nanopdf_jbig2_image_destroy(context, out_image);
          return 0;
        }
        bit = nanopdf_jbig2_arith_decode(arith, &gr_contexts[context_index]);
      }
      nanopdf_jbig2_image_set_pixel(out_image, (int32_t)x, (int32_t)y, bit);
    }
  }
  return 1;
}

static int jbig2_decode_refinement_region_image(
    nanopdf_context* context,
    const nanopdf_jbig2_generic_refinement_region_header* header,
    const nanopdf_jbig2_image* reference,
    int is_empty_refinement,
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_image* out_image) {
  if (!context || !header || !stream || !out_image) {
    return 0;
  }
  if (is_empty_refinement) {
    return jbig2_create_empty_refinement_region_image(context, header, out_image);
  }
  if (!reference) {
    return 0;
  }
  {
    size_t context_count = jbig2_gr_context_count(header->template_id);
    nanopdf_jbig2_arith_ctx* gr_contexts =
        (nanopdf_jbig2_arith_ctx*)nanopdf__allocator_alloc(
            &context->allocator, sizeof(nanopdf_jbig2_arith_ctx) * context_count);
    int decoded = 0;
    if (gr_contexts) {
      size_t i;
      nanopdf_jbig2_arith_decoder arith;
      for (i = 0; i < context_count; ++i) {
        nanopdf_jbig2_arith_ctx_init(&gr_contexts[i]);
      }
      nanopdf_jbig2_arith_decoder_init(&arith, stream);
      decoded = jbig2_decode_generic_refinement_region_arith(
          context, header, reference, &arith, gr_contexts, context_count, out_image);
      nanopdf__allocator_free(&context->allocator, gr_contexts);
    }
    return decoded;
  }
}

typedef struct nanopdf_jbig2_qe {
  uint16_t qe;
  uint8_t nmps;
  uint8_t nlps;
  uint8_t switch_mps;
} nanopdf_jbig2_qe;

static const nanopdf_jbig2_qe k_qe_table[47] = {
    {0x5601, 1, 1, 1},    {0x3401, 2, 6, 0},   {0x1801, 3, 9, 0},
    {0x0AC1, 4, 12, 0},   {0x0521, 5, 29, 0},  {0x0221, 38, 33, 0},
    {0x5601, 7, 6, 1},    {0x5401, 8, 14, 0},  {0x4801, 9, 14, 0},
    {0x3801, 10, 14, 0},  {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0},
    {0x1C01, 13, 20, 0},  {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1},
    {0x5401, 16, 14, 0},  {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0},
    {0x3801, 19, 17, 0},  {0x3401, 20, 18, 0}, {0x3001, 21, 19, 0},
    {0x2801, 22, 19, 0},  {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0},
    {0x1C01, 25, 22, 0},  {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0},
    {0x1401, 28, 25, 0},  {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0},
    {0x0AC1, 31, 28, 0},  {0x09C1, 32, 29, 0}, {0x08A1, 33, 30, 0},
    {0x0521, 34, 31, 0},  {0x0441, 35, 32, 0}, {0x02A1, 36, 33, 0},
    {0x0221, 37, 34, 0},  {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0},
    {0x0085, 40, 37, 0},  {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0},
    {0x0015, 43, 40, 0},  {0x0009, 44, 41, 0}, {0x0005, 45, 42, 0},
    {0x0001, 45, 43, 0},  {0x5601, 46, 46, 0}};

typedef struct nanopdf_jbig2_arith_int_decode_data {
  int need_bits;
  int value;
} nanopdf_jbig2_arith_int_decode_data;

static const nanopdf_jbig2_arith_int_decode_data k_arith_int_decode_data[6] = {
    {2, 0}, {4, 4}, {6, 20}, {8, 84}, {12, 340}, {32, 4436}};

static const uint16_t k_grd_opt_constant1[3] = {0x9b25, 0x0795, 0x00e5};
static const uint16_t k_grd_opt_constant9[3] = {0x000c, 0x0009, 0x0007};
static const uint16_t k_grd_opt_constant10[3] = {0x0007, 0x000f, 0x0007};
static const uint16_t k_grd_opt_constant11[3] = {0x001f, 0x001f, 0x000f};
static const uint16_t k_grd_opt_constant12[3] = {0x000f, 0x0007, 0x0003};

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b1[] = {
    {1, 4, 0}, {2, 8, 16}, {3, 16, 272}, {3, 32, -1}};
const size_t nanopdf_jbig2_huffman_table_b1_size = 4;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b2[] = {
    {1, 0, 0}, {2, 0, 1}, {3, 0, 2}, {4, 3, 3},
    {5, 6, 11}, {6, 32, 75}, {6, 32, -1}};
const size_t nanopdf_jbig2_huffman_table_b2_size = 7;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b3[] = {
    {1, 0, -1}, {2, 0, -2}, {3, 0, -3}, {4, 3, -10}, {5, 6, -74},
    {5, 32, -1}};
const size_t nanopdf_jbig2_huffman_table_b3_size = 6;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b4[] = {
    {1, 0, 1}, {2, 0, 2}, {3, 0, 3}, {4, 3, 4},
    {5, 6, 12}, {5, 32, 76}, {6, 32, -1}};
const size_t nanopdf_jbig2_huffman_table_b4_size = 7;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b5[] = {
    {7, 8, -255}, {1, 0, 1}, {2, 0, 2}, {3, 0, 3},
    {4, 3, 4}, {5, 6, 12}, {6, 32, 76}, {7, 32, -1}};
const size_t nanopdf_jbig2_huffman_table_b5_size = 8;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b6[] = {
    {5, 10, -2048}, {4, 9, -1024}, {4, 8, -512}, {4, 7, -256},
    {5, 6, -128},   {5, 5, -64},   {4, 5, -32},  {2, 7, 0},
    {3, 7, 128},    {3, 8, 256},   {4, 9, 512},  {4, 10, 1024},
    {6, 32, -2049}, {6, 32, 2048}, {2, 32, -1}};
const size_t nanopdf_jbig2_huffman_table_b6_size = 15;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b7[] = {
    {4, 9, -1024}, {3, 8, -512}, {4, 7, -256}, {5, 6, -128},
    {5, 5, -64}, {4, 5, -32}, {4, 5, 0}, {5, 5, 32},
    {5, 6, 64}, {4, 7, 128}, {3, 8, 256}, {3, 9, 512},
    {3, 10, 1024}, {5, 32, -1025}, {5, 32, 2048}, {2, 32, -1}};
const size_t nanopdf_jbig2_huffman_table_b7_size = 16;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b8[] = {
    {8, 3, -15}, {9, 1, -7}, {8, 1, -5}, {9, 0, -3},
    {7, 0, -2}, {4, 0, -1}, {2, 1, 0}, {5, 0, 2},
    {6, 0, 3}, {3, 4, 4}, {6, 1, 20}, {4, 4, 22},
    {4, 5, 38}, {5, 6, 70}, {5, 7, 134}, {6, 7, 262},
    {7, 8, 390}, {6, 10, 646}, {9, 32, -16}, {9, 32, 1670},
    {2, 32, -1}};
const size_t nanopdf_jbig2_huffman_table_b8_size = 21;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b9[] = {
    {8, 4, -31}, {9, 2, -15}, {8, 2, -11}, {9, 1, -7},
    {7, 1, -5}, {4, 1, -3}, {3, 1, -1}, {3, 1, 1},
    {5, 1, 3}, {6, 1, 5}, {3, 5, 7}, {6, 2, 39},
    {4, 5, 43}, {4, 6, 75}, {5, 7, 139}, {5, 8, 267},
    {6, 8, 523}, {7, 9, 779}, {6, 11, 1291}, {9, 32, -32},
    {9, 32, 3339}, {2, 32, -1}};
const size_t nanopdf_jbig2_huffman_table_b9_size = 22;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b10[] = {
    {7, 4, -21}, {8, 0, -5}, {7, 0, -4}, {5, 0, -3},
    {2, 2, -2}, {5, 0, 2}, {6, 0, 3}, {7, 0, 4},
    {8, 0, 5}, {2, 6, 6}, {5, 5, 70}, {6, 5, 102},
    {6, 6, 134}, {6, 7, 198}, {6, 8, 326}, {6, 9, 582},
    {6, 10, 1094}, {7, 11, 2118}, {8, 32, -22}, {8, 32, 4166},
    {2, 32, -1}};
const size_t nanopdf_jbig2_huffman_table_b10_size = 21;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b11[] = {
    {1, 0, 1}, {2, 1, 2}, {4, 0, 4}, {4, 1, 5},
    {5, 1, 7}, {5, 2, 9}, {6, 2, 13}, {7, 2, 17},
    {7, 3, 21}, {7, 4, 29}, {7, 5, 45}, {7, 6, 77},
    {7, 32, 141}};
const size_t nanopdf_jbig2_huffman_table_b11_size = 13;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b12[] = {
    {1, 0, 1}, {2, 0, 2}, {3, 1, 3}, {5, 0, 5},
    {5, 1, 6}, {6, 1, 8}, {7, 0, 10}, {7, 1, 11},
    {7, 2, 13}, {7, 3, 17}, {7, 4, 25}, {8, 5, 41},
    {8, 32, 73}};
const size_t nanopdf_jbig2_huffman_table_b12_size = 13;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b13[] = {
    {1, 0, 1}, {3, 0, 2}, {4, 0, 3}, {5, 0, 4},
    {4, 1, 5}, {3, 3, 7}, {6, 1, 15}, {6, 2, 17},
    {6, 3, 21}, {6, 4, 29}, {6, 5, 45}, {7, 6, 77},
    {7, 32, 141}};
const size_t nanopdf_jbig2_huffman_table_b13_size = 13;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b14[] = {
    {3, 0, -2}, {3, 0, -1}, {1, 0, 0}, {3, 0, 1}, {3, 0, 2}};
const size_t nanopdf_jbig2_huffman_table_b14_size = 5;

const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b15[] = {
    {7, 4, -24}, {6, 2, -8}, {5, 1, -4}, {4, 0, -2},
    {3, 0, -1}, {1, 0, 0}, {3, 0, 1}, {4, 0, 2},
    {5, 1, 3}, {6, 2, 5}, {7, 4, 9}, {7, 32, -25},
    {7, 32, 25}};
const size_t nanopdf_jbig2_huffman_table_b15_size = 13;

static int32_t align_to_boundary32(int32_t value) {
  return (value + 31) & ~31;
}

static int bit_index_to_byte(int index) {
  return index / 8;
}

static uint32_t min_u32(uint32_t a, uint32_t b) {
  return a < b ? a : b;
}

static size_t min_size(size_t a, size_t b) {
  return a < b ? a : b;
}

static uint32_t bitstream_length_bits(const nanopdf_jbig2_bitstream* stream) {
  if (!stream) {
    return 0;
  }
  if (stream->size > UINT32_MAX / 8u) {
    return UINT32_MAX;
  }
  return (uint32_t)stream->size * 8u;
}

static void bitstream_advance_bit(nanopdf_jbig2_bitstream* stream) {
  if (stream->bit_idx == 7u) {
    ++stream->byte_idx;
    stream->bit_idx = 0;
  } else {
    ++stream->bit_idx;
  }
}

void nanopdf_jbig2_bitstream_init(
    nanopdf_jbig2_bitstream* stream,
    const uint8_t* data,
    size_t size,
    uint64_t key) {
  if (!stream) {
    return;
  }
  stream->data = data;
  stream->size = min_size(size, (size_t)NANOPDF_JBIG2_MAX_STREAM_SIZE);
  stream->byte_idx = 0;
  stream->bit_idx = 0;
  stream->key = key;
}

int nanopdf_jbig2_bitstream_in_bounds(const nanopdf_jbig2_bitstream* stream) {
  return stream && stream->byte_idx < stream->size;
}

int32_t nanopdf_jbig2_bitstream_read_bits_u32(
    nanopdf_jbig2_bitstream* stream,
    uint32_t bits,
    uint32_t* out_value) {
  uint32_t bit_pos;
  uint32_t read_bits;
  if (!stream || !out_value || !nanopdf_jbig2_bitstream_in_bounds(stream)) {
    return -1;
  }
  bit_pos = nanopdf_jbig2_bitstream_bit_position(stream);
  if (bit_pos > bitstream_length_bits(stream)) {
    return -1;
  }
  read_bits = bit_pos + bits <= bitstream_length_bits(stream)
                  ? bits
                  : bitstream_length_bits(stream) - bit_pos;
  *out_value = 0;
  while (read_bits > 0) {
    *out_value =
        (*out_value << 1) | ((stream->data[stream->byte_idx] >> (7u - stream->bit_idx)) & 1u);
    bitstream_advance_bit(stream);
    --read_bits;
  }
  return 0;
}

int32_t nanopdf_jbig2_bitstream_read_bits_i32(
    nanopdf_jbig2_bitstream* stream,
    uint32_t bits,
    int32_t* out_value) {
  uint32_t value = 0;
  int32_t status = nanopdf_jbig2_bitstream_read_bits_u32(stream, bits, &value);
  if (status == 0 && out_value) {
    *out_value = (int32_t)value;
  }
  return status;
}

int32_t nanopdf_jbig2_bitstream_read_bit_u32(
    nanopdf_jbig2_bitstream* stream,
    uint32_t* out_value) {
  if (!stream || !out_value || !nanopdf_jbig2_bitstream_in_bounds(stream)) {
    return -1;
  }
  *out_value = (stream->data[stream->byte_idx] >> (7u - stream->bit_idx)) & 1u;
  bitstream_advance_bit(stream);
  return 0;
}

int32_t nanopdf_jbig2_bitstream_read_bit_bool(
    nanopdf_jbig2_bitstream* stream,
    uint8_t* out_value) {
  uint32_t value = 0;
  int32_t status = nanopdf_jbig2_bitstream_read_bit_u32(stream, &value);
  if (status == 0 && out_value) {
    *out_value = value ? 1u : 0u;
  }
  return status;
}

int32_t nanopdf_jbig2_bitstream_read_byte(
    nanopdf_jbig2_bitstream* stream,
    uint8_t* out_value) {
  if (!stream || !out_value || !nanopdf_jbig2_bitstream_in_bounds(stream)) {
    return -1;
  }
  *out_value = stream->data[stream->byte_idx++];
  return 0;
}

int32_t nanopdf_jbig2_bitstream_read_u32(
    nanopdf_jbig2_bitstream* stream,
    uint32_t* out_value) {
  if (!stream || !out_value || stream->byte_idx + 3u >= stream->size) {
    return -1;
  }
  *out_value = ((uint32_t)stream->data[stream->byte_idx] << 24) |
               ((uint32_t)stream->data[stream->byte_idx + 1] << 16) |
               ((uint32_t)stream->data[stream->byte_idx + 2] << 8) |
               (uint32_t)stream->data[stream->byte_idx + 3];
  stream->byte_idx += 4;
  return 0;
}

int32_t nanopdf_jbig2_bitstream_read_u16(
    nanopdf_jbig2_bitstream* stream,
    uint16_t* out_value) {
  if (!stream || !out_value || stream->byte_idx + 1u >= stream->size) {
    return -1;
  }
  *out_value =
      (uint16_t)(((uint16_t)stream->data[stream->byte_idx] << 8) |
                 (uint16_t)stream->data[stream->byte_idx + 1]);
  stream->byte_idx += 2;
  return 0;
}

void nanopdf_jbig2_bitstream_align_byte(nanopdf_jbig2_bitstream* stream) {
  if (stream && stream->bit_idx != 0u) {
    nanopdf_jbig2_bitstream_add_offset(stream, 1);
    stream->bit_idx = 0;
  }
}

uint8_t nanopdf_jbig2_bitstream_current_byte(const nanopdf_jbig2_bitstream* stream) {
  return nanopdf_jbig2_bitstream_in_bounds(stream) ? stream->data[stream->byte_idx] : 0u;
}

void nanopdf_jbig2_bitstream_inc_byte(nanopdf_jbig2_bitstream* stream) {
  nanopdf_jbig2_bitstream_add_offset(stream, 1);
}

uint8_t nanopdf_jbig2_bitstream_current_byte_arith(const nanopdf_jbig2_bitstream* stream) {
  return nanopdf_jbig2_bitstream_in_bounds(stream) ? stream->data[stream->byte_idx] : 0xffu;
}

uint8_t nanopdf_jbig2_bitstream_next_byte_arith(const nanopdf_jbig2_bitstream* stream) {
  return stream && stream->byte_idx + 1u < stream->size ? stream->data[stream->byte_idx + 1] : 0xffu;
}

uint32_t nanopdf_jbig2_bitstream_offset(const nanopdf_jbig2_bitstream* stream) {
  return stream ? stream->byte_idx : 0;
}

void nanopdf_jbig2_bitstream_set_offset(nanopdf_jbig2_bitstream* stream, uint32_t offset) {
  if (!stream) {
    return;
  }
  stream->byte_idx = min_u32(offset, stream->size > UINT32_MAX ? UINT32_MAX : (uint32_t)stream->size);
}

void nanopdf_jbig2_bitstream_add_offset(nanopdf_jbig2_bitstream* stream, uint32_t delta) {
  if (!stream) {
    return;
  }
  if (delta > UINT32_MAX - stream->byte_idx) {
    stream->byte_idx = stream->size > UINT32_MAX ? UINT32_MAX : (uint32_t)stream->size;
    return;
  }
  nanopdf_jbig2_bitstream_set_offset(stream, stream->byte_idx + delta);
}

uint32_t nanopdf_jbig2_bitstream_bit_position(const nanopdf_jbig2_bitstream* stream) {
  return stream ? (stream->byte_idx << 3) + stream->bit_idx : 0;
}

void nanopdf_jbig2_bitstream_set_bit_position(nanopdf_jbig2_bitstream* stream, uint32_t bit_pos) {
  if (!stream) {
    return;
  }
  stream->byte_idx = bit_pos >> 3;
  stream->bit_idx = bit_pos & 7u;
}

const uint8_t* nanopdf_jbig2_bitstream_pointer(const nanopdf_jbig2_bitstream* stream) {
  return stream && stream->byte_idx <= stream->size ? stream->data + stream->byte_idx : NULL;
}

uint32_t nanopdf_jbig2_bitstream_bytes_left(const nanopdf_jbig2_bitstream* stream) {
  if (!stream || stream->byte_idx > stream->size) {
    return 0;
  }
  if (stream->size - stream->byte_idx > UINT32_MAX) {
    return UINT32_MAX;
  }
  return (uint32_t)(stream->size - stream->byte_idx);
}

static int image_line_offset(const nanopdf_jbig2_image* image, int32_t y, size_t* out_offset) {
  if (!image || !image->data || y < 0 || y >= image->height) {
    return 0;
  }
  *out_offset = (size_t)y * (size_t)image->stride;
  return 1;
}

int nanopdf_jbig2_image_valid_size(int32_t width, int32_t height) {
  return width > 0 && width <= NANOPDF_JBIG2_MAX_IMAGE_SIZE &&
         height > 0 && height <= NANOPDF_JBIG2_MAX_IMAGE_SIZE;
}

int nanopdf_jbig2_image_init(
    nanopdf_context* context,
    nanopdf_jbig2_image* image,
    int32_t width,
    int32_t height) {
  int32_t stride_pixels;
  size_t total_bytes;
  if (!context || !image) {
    return 0;
  }
  memset(image, 0, sizeof(*image));
  if (width <= 0 || height <= 0 || width > INT_MAX - 31) {
    return 0;
  }
  stride_pixels = align_to_boundary32(width);
  if (height > (INT_MAX - 31) / stride_pixels) {
    return 0;
  }
  image->width = width;
  image->height = height;
  image->stride = stride_pixels / 8;
  image->owns_data = 1;
  total_bytes = (size_t)image->stride * (size_t)image->height;
  image->data = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, total_bytes);
  if (!image->data) {
    memset(image, 0, sizeof(*image));
    return 0;
  }
  memset(image->data, 0, total_bytes);
  return 1;
}

int nanopdf_jbig2_image_init_external(
    nanopdf_jbig2_image* image,
    int32_t width,
    int32_t height,
    int32_t stride,
    uint8_t* data) {
  int32_t stride_pixels;
  if (!image) {
    return 0;
  }
  memset(image, 0, sizeof(*image));
  if (width < 0 || height < 0 || stride < 0 || stride % 4 != 0) {
    return 0;
  }
  stride_pixels = 8 * stride;
  if (stride_pixels < width || (stride_pixels > 0 && height > (INT_MAX - 31) / stride_pixels)) {
    return 0;
  }
  if (stride > 0 && height > 0 && !data) {
    return 0;
  }
  image->data = data;
  image->width = width;
  image->height = height;
  image->stride = stride;
  image->owns_data = 0;
  return 1;
}

void nanopdf_jbig2_image_destroy(nanopdf_context* context, nanopdf_jbig2_image* image) {
  if (!image) {
    return;
  }
  if (context && image->owns_data) {
    nanopdf__allocator_free(&context->allocator, image->data);
  }
  memset(image, 0, sizeof(*image));
}

int nanopdf_jbig2_image_copy(
    nanopdf_context* context,
    nanopdf_jbig2_image* out_image,
    const nanopdf_jbig2_image* source) {
  size_t total_bytes;
  if (!context || !out_image || !source || !source->data ||
      !nanopdf_jbig2_image_init(context, out_image, source->width, source->height)) {
    return 0;
  }
  total_bytes = (size_t)source->stride * (size_t)source->height;
  memcpy(out_image->data, source->data, total_bytes);
  return 1;
}

const uint8_t* nanopdf_jbig2_image_get_line_const(const nanopdf_jbig2_image* image, int32_t y) {
  size_t offset = 0;
  return image_line_offset(image, y, &offset) ? image->data + offset : NULL;
}

uint8_t* nanopdf_jbig2_image_get_line(nanopdf_jbig2_image* image, int32_t y) {
  size_t offset = 0;
  return image_line_offset(image, y, &offset) ? image->data + offset : NULL;
}

int nanopdf_jbig2_image_get_pixel(const nanopdf_jbig2_image* image, int32_t x, int32_t y) {
  const uint8_t* line;
  int32_t byte_index;
  if (!image || x < 0 || x >= image->width) {
    return 0;
  }
  line = nanopdf_jbig2_image_get_line_const(image, y);
  if (!line) {
    return 0;
  }
  byte_index = bit_index_to_byte(x);
  return (line[byte_index] >> (7 - (x & 7))) & 1;
}

void nanopdf_jbig2_image_set_pixel(nanopdf_jbig2_image* image, int32_t x, int32_t y, int value) {
  uint8_t* line;
  int32_t byte_index;
  uint8_t mask;
  if (!image || x < 0 || x >= image->width) {
    return;
  }
  line = nanopdf_jbig2_image_get_line(image, y);
  if (!line) {
    return;
  }
  byte_index = bit_index_to_byte(x);
  mask = (uint8_t)(1u << (7 - (x & 7)));
  if (value) {
    line[byte_index] |= mask;
  } else {
    line[byte_index] &= (uint8_t)~mask;
  }
}

void nanopdf_jbig2_image_copy_line(nanopdf_jbig2_image* image, int32_t dest_y, int32_t src_y) {
  uint8_t* dest = nanopdf_jbig2_image_get_line(image, dest_y);
  const uint8_t* src = nanopdf_jbig2_image_get_line_const(image, src_y);
  if (!dest || !image) {
    return;
  }
  if (!src) {
    memset(dest, 0, (size_t)image->stride);
    return;
  }
  memcpy(dest, src, (size_t)image->stride);
}

void nanopdf_jbig2_image_fill(nanopdf_jbig2_image* image, int value) {
  if (!image || !image->data || image->width <= 0 || image->height <= 0) {
    return;
  }
  memset(image->data, value ? 0xff : 0, (size_t)image->stride * (size_t)image->height);
}

int nanopdf_jbig2_image_expand(
    nanopdf_context* context,
    nanopdf_jbig2_image* image,
    int32_t height,
    int value) {
  size_t old_bytes;
  size_t new_bytes;
  uint8_t* resized;
  if (!context || !image || height <= image->height || height > NANOPDF_JBIG2_MAX_IMAGE_SIZE ||
      !image->owns_data) {
    return 0;
  }
  old_bytes = (size_t)image->stride * (size_t)image->height;
  new_bytes = (size_t)image->stride * (size_t)height;
  resized = (uint8_t*)nanopdf__allocator_realloc(&context->allocator, image->data, new_bytes);
  if (!resized) {
    return 0;
  }
  memset(resized + old_bytes, value ? 0xff : 0, new_bytes - old_bytes);
  image->data = resized;
  image->height = height;
  return 1;
}

static int compose_pixel(nanopdf_jbig2_compose_op op, int source, int dest) {
  switch (op) {
    case NANOPDF_JBIG2_COMPOSE_OR:
      return source | dest;
    case NANOPDF_JBIG2_COMPOSE_AND:
      return source & dest;
    case NANOPDF_JBIG2_COMPOSE_XOR:
      return source ^ dest;
    case NANOPDF_JBIG2_COMPOSE_XNOR:
      return !(source ^ dest);
    case NANOPDF_JBIG2_COMPOSE_REPLACE:
    default:
      return source;
  }
}

int nanopdf_jbig2_image_compose_from(
    nanopdf_jbig2_image* dest,
    int64_t x,
    int64_t y,
    const nanopdf_jbig2_image* source,
    nanopdf_jbig2_compose_op op) {
  int32_t src_left = 0;
  int32_t src_top = 0;
  int32_t src_right;
  int32_t src_bottom;
  int64_t dest_left;
  int64_t dest_top;
  int32_t copy_width;
  int32_t copy_height;
  int32_t row;
  int32_t col;
  if (!dest || !dest->data || !source || !source->data) {
    return 0;
  }
  src_right = source->width;
  src_bottom = source->height;
  dest_left = x;
  dest_top = y;
  if (dest_left < 0) {
    src_left -= (int32_t)dest_left;
    dest_left = 0;
  }
  if (dest_top < 0) {
    src_top -= (int32_t)dest_top;
    dest_top = 0;
  }
  copy_width = src_right - src_left;
  copy_height = src_bottom - src_top;
  if (dest_left + copy_width > dest->width) {
    copy_width = dest->width - (int32_t)dest_left;
  }
  if (dest_top + copy_height > dest->height) {
    copy_height = dest->height - (int32_t)dest_top;
  }
  if (copy_width <= 0 || copy_height <= 0) {
    return 1;
  }
  for (row = 0; row < copy_height; ++row) {
    for (col = 0; col < copy_width; ++col) {
      int source_pixel = nanopdf_jbig2_image_get_pixel(source, src_left + col, src_top + row);
      int dest_x = (int)(dest_left + col);
      int dest_y = (int)(dest_top + row);
      int dest_pixel = nanopdf_jbig2_image_get_pixel(dest, dest_x, dest_y);
      nanopdf_jbig2_image_set_pixel(dest, dest_x, dest_y, compose_pixel(op, source_pixel, dest_pixel));
    }
  }
  return 1;
}

static int grow_image_array(
    nanopdf_context* context,
    nanopdf_jbig2_image** images,
    size_t* capacity,
    size_t required) {
  nanopdf_jbig2_image* resized;
  size_t new_capacity;
  if (required <= *capacity) {
    return 1;
  }
  new_capacity = *capacity == 0 ? 4 : *capacity * 2;
  while (new_capacity < required) {
    new_capacity *= 2;
  }
  resized = (nanopdf_jbig2_image*)nanopdf__allocator_realloc(
      &context->allocator, *images, new_capacity * sizeof((*images)[0]));
  if (!resized) {
    return 0;
  }
  memset(resized + *capacity, 0, (new_capacity - *capacity) * sizeof(resized[0]));
  *images = resized;
  *capacity = new_capacity;
  return 1;
}

void nanopdf_jbig2_symbol_dict_init(nanopdf_jbig2_symbol_dict* dict) {
  if (dict) {
    memset(dict, 0, sizeof(*dict));
  }
}

void nanopdf_jbig2_symbol_dict_destroy(
    nanopdf_context* context,
    nanopdf_jbig2_symbol_dict* dict) {
  size_t i;
  if (!dict) {
    return;
  }
  for (i = 0; i < dict->count; ++i) {
    nanopdf_jbig2_image_destroy(context, &dict->images[i]);
  }
  if (context) {
    nanopdf__allocator_free(&context->allocator, dict->images);
  }
  memset(dict, 0, sizeof(*dict));
}

int nanopdf_jbig2_symbol_dict_add_image(
    nanopdf_context* context,
    nanopdf_jbig2_symbol_dict* dict,
    const nanopdf_jbig2_image* image) {
  if (!context || !dict || !image ||
      !grow_image_array(context, &dict->images, &dict->capacity, dict->count + 1) ||
      !nanopdf_jbig2_image_copy(context, &dict->images[dict->count], image)) {
    return 0;
  }
  dict->count++;
  return 1;
}

int nanopdf_jbig2_symbol_dict_deep_copy(
    nanopdf_context* context,
    nanopdf_jbig2_symbol_dict* out_dict,
    const nanopdf_jbig2_symbol_dict* source) {
  size_t i;
  if (!context || !out_dict || !source) {
    return 0;
  }
  nanopdf_jbig2_symbol_dict_init(out_dict);
  for (i = 0; i < source->count; ++i) {
    if (!nanopdf_jbig2_symbol_dict_add_image(context, out_dict, &source->images[i])) {
      nanopdf_jbig2_symbol_dict_destroy(context, out_dict);
      return 0;
    }
  }
  return 1;
}

size_t nanopdf_jbig2_symbol_dict_count(const nanopdf_jbig2_symbol_dict* dict) {
  return dict ? dict->count : 0;
}

nanopdf_jbig2_image* nanopdf_jbig2_symbol_dict_get(
    nanopdf_jbig2_symbol_dict* dict,
    size_t index) {
  return dict && index < dict->count ? &dict->images[index] : NULL;
}

const nanopdf_jbig2_image* nanopdf_jbig2_symbol_dict_get_const(
    const nanopdf_jbig2_symbol_dict* dict,
    size_t index) {
  return dict && index < dict->count ? &dict->images[index] : NULL;
}

void nanopdf_jbig2_pattern_dict_init(nanopdf_jbig2_pattern_dict* dict) {
  if (dict) {
    memset(dict, 0, sizeof(*dict));
  }
}

void nanopdf_jbig2_pattern_dict_destroy(
    nanopdf_context* context,
    nanopdf_jbig2_pattern_dict* dict) {
  size_t i;
  if (!dict) {
    return;
  }
  for (i = 0; i < dict->count; ++i) {
    nanopdf_jbig2_image_destroy(context, &dict->patterns[i]);
  }
  if (context) {
    nanopdf__allocator_free(&context->allocator, dict->patterns);
  }
  memset(dict, 0, sizeof(*dict));
}

int nanopdf_jbig2_pattern_dict_add(
    nanopdf_context* context,
    nanopdf_jbig2_pattern_dict* dict,
    const nanopdf_jbig2_image* pattern) {
  if (!context || !dict || !pattern ||
      !grow_image_array(context, &dict->patterns, &dict->capacity, dict->count + 1) ||
      !nanopdf_jbig2_image_copy(context, &dict->patterns[dict->count], pattern)) {
    return 0;
  }
  dict->count++;
  return 1;
}

void nanopdf_jbig2_pattern_dict_set_dimensions(
    nanopdf_jbig2_pattern_dict* dict,
    uint32_t width,
    uint32_t height) {
  if (!dict) {
    return;
  }
  dict->width = width;
  dict->height = height;
}

size_t nanopdf_jbig2_pattern_dict_count(const nanopdf_jbig2_pattern_dict* dict) {
  return dict ? dict->count : 0;
}

nanopdf_jbig2_image* nanopdf_jbig2_pattern_dict_get(
    nanopdf_jbig2_pattern_dict* dict,
    size_t index) {
  return dict && index < dict->count ? &dict->patterns[index] : NULL;
}

const nanopdf_jbig2_image* nanopdf_jbig2_pattern_dict_get_const(
    const nanopdf_jbig2_pattern_dict* dict,
    size_t index) {
  return dict && index < dict->count ? &dict->patterns[index] : NULL;
}

void nanopdf_jbig2_segment_init(nanopdf_jbig2_segment* segment) {
  if (!segment) {
    return;
  }
  memset(segment, 0, sizeof(*segment));
  segment->state = NANOPDF_JBIG2_SEGMENT_HEADER_UNPARSED;
  segment->result_type = NANOPDF_JBIG2_VOID_POINTER;
}

void nanopdf_jbig2_segment_destroy(nanopdf_context* context, nanopdf_jbig2_segment* segment) {
  if (!segment) {
    return;
  }
  if (context) {
    nanopdf__allocator_free(&context->allocator, segment->referred_to_segment_numbers);
    if (segment->symbol_dict) {
      nanopdf_jbig2_symbol_dict_destroy(context, segment->symbol_dict);
      nanopdf__allocator_free(&context->allocator, segment->symbol_dict);
    }
    if (segment->pattern_dict) {
      nanopdf_jbig2_pattern_dict_destroy(context, segment->pattern_dict);
      nanopdf__allocator_free(&context->allocator, segment->pattern_dict);
    }
    if (segment->image) {
      nanopdf_jbig2_image_destroy(context, segment->image);
      nanopdf__allocator_free(&context->allocator, segment->image);
    }
    if (segment->huffman_table) {
      nanopdf_jbig2_huffman_table_destroy(
          context, (nanopdf_jbig2_huffman_table*)segment->huffman_table);
      nanopdf__allocator_free(&context->allocator, segment->huffman_table);
    }
  }
  memset(segment, 0, sizeof(*segment));
}

int nanopdf_jbig2_segment_set_referred_count(
    nanopdf_context* context,
    nanopdf_jbig2_segment* segment,
    int32_t count) {
  uint32_t* numbers;
  if (!context || !segment || count < 0 || count > 64) {
    return 0;
  }
  numbers = NULL;
  if (count > 0) {
    numbers = (uint32_t*)nanopdf__allocator_alloc(
        &context->allocator, (size_t)count * sizeof(numbers[0]));
    if (!numbers) {
      return 0;
    }
    memset(numbers, 0, (size_t)count * sizeof(numbers[0]));
  }
  nanopdf__allocator_free(&context->allocator, segment->referred_to_segment_numbers);
  segment->referred_to_segment_numbers = numbers;
  segment->referred_to_segment_count = count;
  segment->referred_to_segment_capacity = (size_t)count;
  return 1;
}

int nanopdf_jbig2_segment_parse_header(
    nanopdf_context* context,
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_segment* segment) {
  uint8_t flags = 0;
  uint8_t ref_count_byte = 0;
  int32_t ref_count = 0;
  int ref_size;
  int32_t i;

  if (!context || !stream || !segment) {
    return 0;
  }
  nanopdf_jbig2_segment_destroy(context, segment);
  nanopdf_jbig2_segment_init(segment);

  if (nanopdf_jbig2_bitstream_read_u32(stream, &segment->number) != 0 ||
      nanopdf_jbig2_bitstream_read_byte(stream, &flags) != 0 ||
      nanopdf_jbig2_bitstream_read_byte(stream, &ref_count_byte) != 0) {
    segment->state = NANOPDF_JBIG2_SEGMENT_ERROR;
    return 0;
  }
  segment->type = (uint8_t)(flags & 0x3fu);
  segment->page_association_size = (uint8_t)((flags >> 6) & 1u);
  segment->deferred_non_retain = (uint8_t)((flags >> 7) & 1u);

  if ((ref_count_byte & 0xe0u) == 0xe0u) {
    uint32_t long_count = 0;
    uint32_t padding;
    if (nanopdf_jbig2_bitstream_read_u32(stream, &long_count) != 0) {
      segment->state = NANOPDF_JBIG2_SEGMENT_ERROR;
      return 0;
    }
    ref_count = (int32_t)(long_count & 0x1fffffffu);
    padding = ((uint32_t)ref_count + 9u) % 4u;
    if (padding > 0u) {
      nanopdf_jbig2_bitstream_add_offset(stream, 4u - padding);
    }
  } else {
    ref_count = (int32_t)((ref_count_byte >> 5) & 0x07u);
  }

  if (!nanopdf_jbig2_segment_set_referred_count(context, segment, ref_count)) {
    segment->state = NANOPDF_JBIG2_SEGMENT_ERROR;
    return 0;
  }

  ref_size = segment->number <= 256u ? 1 : (segment->number <= 65536u ? 2 : 4);
  for (i = 0; i < ref_count; ++i) {
    if (ref_size == 1) {
      uint8_t value = 0;
      if (nanopdf_jbig2_bitstream_read_byte(stream, &value) != 0) {
        segment->state = NANOPDF_JBIG2_SEGMENT_ERROR;
        return 0;
      }
      segment->referred_to_segment_numbers[i] = value;
    } else if (ref_size == 2) {
      uint16_t value = 0;
      if (nanopdf_jbig2_bitstream_read_u16(stream, &value) != 0) {
        segment->state = NANOPDF_JBIG2_SEGMENT_ERROR;
        return 0;
      }
      segment->referred_to_segment_numbers[i] = value;
    } else {
      if (nanopdf_jbig2_bitstream_read_u32(stream, &segment->referred_to_segment_numbers[i]) !=
          0) {
        segment->state = NANOPDF_JBIG2_SEGMENT_ERROR;
        return 0;
      }
    }
  }

  if (segment->page_association_size) {
    if (nanopdf_jbig2_bitstream_read_u32(stream, &segment->page_association) != 0) {
      segment->state = NANOPDF_JBIG2_SEGMENT_ERROR;
      return 0;
    }
  } else {
    uint8_t page_association = 0;
    if (nanopdf_jbig2_bitstream_read_byte(stream, &page_association) != 0) {
      segment->state = NANOPDF_JBIG2_SEGMENT_ERROR;
      return 0;
    }
    segment->page_association = page_association;
  }

  if (nanopdf_jbig2_bitstream_read_u32(stream, &segment->data_length) != 0) {
    segment->state = NANOPDF_JBIG2_SEGMENT_ERROR;
    return 0;
  }
  segment->data_offset = nanopdf_jbig2_bitstream_offset(stream);
  segment->header_length = segment->data_offset;
  if (segment->data_length != 0xffffffffu) {
    if (nanopdf_jbig2_bitstream_bytes_left(stream) < segment->data_length) {
      segment->state = NANOPDF_JBIG2_SEGMENT_ERROR;
      return 0;
    }
    nanopdf_jbig2_bitstream_add_offset(stream, segment->data_length);
  }
  segment->state = NANOPDF_JBIG2_SEGMENT_DATA_UNPARSED;
  return 1;
}

void nanopdf_jbig2_huffman_table_init(nanopdf_jbig2_huffman_table* table) {
  if (table) {
    memset(table, 0, sizeof(*table));
  }
}

void nanopdf_jbig2_huffman_table_destroy(
    nanopdf_context* context,
    nanopdf_jbig2_huffman_table* table) {
  if (!table) {
    return;
  }
  if (context) {
    nanopdf__allocator_free(&context->allocator, table->codes);
    nanopdf__allocator_free(&context->allocator, table->prefix_lengths);
    nanopdf__allocator_free(&context->allocator, table->range_lengths);
    nanopdf__allocator_free(&context->allocator, table->range_lows);
  }
  memset(table, 0, sizeof(*table));
}

static int huffman_alloc_arrays(
    nanopdf_context* context,
    nanopdf_jbig2_huffman_table* table,
    size_t count) {
  table->codes = (int*)nanopdf__allocator_alloc(&context->allocator, count * sizeof(table->codes[0]));
  table->prefix_lengths = (int*)nanopdf__allocator_alloc(
      &context->allocator, count * sizeof(table->prefix_lengths[0]));
  table->range_lengths = (int*)nanopdf__allocator_alloc(
      &context->allocator, count * sizeof(table->range_lengths[0]));
  table->range_lows = (int*)nanopdf__allocator_alloc(
      &context->allocator, count * sizeof(table->range_lows[0]));
  if (!table->codes || !table->prefix_lengths || !table->range_lengths || !table->range_lows) {
    nanopdf_jbig2_huffman_table_destroy(context, table);
    return 0;
  }
  memset(table->codes, 0, count * sizeof(table->codes[0]));
  memset(table->prefix_lengths, 0, count * sizeof(table->prefix_lengths[0]));
  memset(table->range_lengths, 0, count * sizeof(table->range_lengths[0]));
  memset(table->range_lows, 0, count * sizeof(table->range_lows[0]));
  table->count = count;
  return 1;
}

int nanopdf_jbig2_huffman_table_init_standard(
    nanopdf_context* context,
    nanopdf_jbig2_huffman_table* table,
    const nanopdf_jbig2_table_line* lines,
    size_t line_count,
    int has_oob) {
  int max_prefix_length = 0;
  int* length_counts = NULL;
  int* first_codes = NULL;
  int* next_codes = NULL;
  int code = 0;
  size_t i;
  int length;
  if (!context || !table || !lines || line_count == 0) {
    return 0;
  }
  nanopdf_jbig2_huffman_table_destroy(context, table);
  for (i = 0; i < line_count; ++i) {
    if (lines[i].prefix_length > max_prefix_length) {
      max_prefix_length = lines[i].prefix_length;
    }
  }
  if (max_prefix_length <= 0 || max_prefix_length > 32) {
    return 0;
  }
  length_counts = (int*)nanopdf__allocator_alloc(
      &context->allocator, (size_t)(max_prefix_length + 1) * sizeof(length_counts[0]));
  first_codes = (int*)nanopdf__allocator_alloc(
      &context->allocator, (size_t)(max_prefix_length + 1) * sizeof(first_codes[0]));
  next_codes = (int*)nanopdf__allocator_alloc(
      &context->allocator, (size_t)(max_prefix_length + 1) * sizeof(next_codes[0]));
  if (!length_counts || !first_codes || !next_codes) {
    nanopdf__allocator_free(&context->allocator, length_counts);
    nanopdf__allocator_free(&context->allocator, first_codes);
    nanopdf__allocator_free(&context->allocator, next_codes);
    return 0;
  }
  memset(length_counts, 0, (size_t)(max_prefix_length + 1) * sizeof(length_counts[0]));
  memset(first_codes, 0, (size_t)(max_prefix_length + 1) * sizeof(first_codes[0]));
  memset(next_codes, 0, (size_t)(max_prefix_length + 1) * sizeof(next_codes[0]));
  for (i = 0; i < line_count; ++i) {
    if (lines[i].prefix_length > 0) {
      length_counts[lines[i].prefix_length]++;
    }
  }
  for (length = 1; length <= max_prefix_length; ++length) {
    code = (code + length_counts[length - 1]) << 1;
    first_codes[length] = code;
    next_codes[length] = code;
  }
  if (!huffman_alloc_arrays(context, table, line_count)) {
    nanopdf__allocator_free(&context->allocator, length_counts);
    nanopdf__allocator_free(&context->allocator, first_codes);
    nanopdf__allocator_free(&context->allocator, next_codes);
    return 0;
  }
  table->has_oob = has_oob ? 1u : 0u;
  for (i = 0; i < line_count; ++i) {
    int prefix_length = lines[i].prefix_length;
    if (prefix_length > 0 && prefix_length <= max_prefix_length) {
      table->codes[i] = next_codes[prefix_length]++;
    }
    table->prefix_lengths[i] = prefix_length;
    table->range_lengths[i] = lines[i].range_length;
    table->range_lows[i] = lines[i].range_low;
    if (has_oob && i == line_count - 1) {
      table->oob_index = i;
    }
  }
  table->is_ok = 1;
  nanopdf__allocator_free(&context->allocator, length_counts);
  nanopdf__allocator_free(&context->allocator, first_codes);
  nanopdf__allocator_free(&context->allocator, next_codes);
  return 1;
}

int nanopdf_jbig2_huffman_table_init_encoded(
    nanopdf_context* context,
    nanopdf_jbig2_huffman_table* table,
    nanopdf_jbig2_bitstream* stream) {
  uint32_t ht_oob = 0;
  uint32_t entry_count_u32 = 0;
  size_t entry_count;
  int max_prefix_length = 0;
  int* length_counts = NULL;
  int* first_codes = NULL;
  int* next_codes = NULL;
  int code = 0;
  size_t i;
  int length;
  if (!context || !table || !stream ||
      nanopdf_jbig2_bitstream_read_bit_u32(stream, &ht_oob) != 0 ||
      nanopdf_jbig2_bitstream_read_u32(stream, &entry_count_u32) != 0 ||
      entry_count_u32 == 0u || entry_count_u32 > 65535u) {
    return 0;
  }
  entry_count = (size_t)entry_count_u32;
  nanopdf_jbig2_huffman_table_destroy(context, table);
  if (!huffman_alloc_arrays(context, table, entry_count)) {
    return 0;
  }
  table->has_oob = ht_oob ? 1u : 0u;
  for (i = 0; i < entry_count; ++i) {
    int32_t prefix_length = 0;
    uint32_t bit = 0;
    uint32_t len_len = 0;
    int j;
    if (i > 0) {
      if (nanopdf_jbig2_bitstream_read_bit_u32(stream, &bit) != 0) {
        nanopdf_jbig2_huffman_table_destroy(context, table);
        return 0;
      }
      if (bit == 0u) {
        table->prefix_lengths[i] = table->prefix_lengths[i - 1u];
        continue;
      }
    }
    for (j = 0; j < 5; ++j) {
      if (nanopdf_jbig2_bitstream_read_bit_u32(stream, &bit) != 0) {
        nanopdf_jbig2_huffman_table_destroy(context, table);
        return 0;
      }
      if (bit == 0u) {
        len_len = (uint32_t)(j + 1);
        break;
      }
    }
    if (len_len == 0u) {
      len_len = 5u;
    }
    if (nanopdf_jbig2_bitstream_read_bits_i32(stream, len_len, &prefix_length) != 0 ||
        prefix_length < 0 || prefix_length > 32) {
      nanopdf_jbig2_huffman_table_destroy(context, table);
      return 0;
    }
    table->prefix_lengths[i] = prefix_length;
  }

  for (i = 0; i < entry_count; ++i) {
    int32_t range_length = 0;
    int32_t range_low = 0;
    if (nanopdf_jbig2_bitstream_read_bits_i32(stream, 5, &range_length) != 0 ||
        nanopdf_jbig2_bitstream_read_bits_i32(stream, 32, &range_low) != 0 ||
        range_length < 0 || range_length > 32) {
      nanopdf_jbig2_huffman_table_destroy(context, table);
      return 0;
    }
    table->range_lengths[i] = range_length;
    table->range_lows[i] = range_low;
  }

  for (i = 0; i < entry_count; ++i) {
    if (table->prefix_lengths[i] > max_prefix_length) {
      max_prefix_length = table->prefix_lengths[i];
    }
  }
  if (max_prefix_length <= 0 || max_prefix_length > 32) {
    nanopdf_jbig2_huffman_table_destroy(context, table);
    return 0;
  }
  length_counts = (int*)nanopdf__allocator_alloc(
      &context->allocator, (size_t)(max_prefix_length + 1) * sizeof(length_counts[0]));
  first_codes = (int*)nanopdf__allocator_alloc(
      &context->allocator, (size_t)(max_prefix_length + 1) * sizeof(first_codes[0]));
  next_codes = (int*)nanopdf__allocator_alloc(
      &context->allocator, (size_t)(max_prefix_length + 1) * sizeof(next_codes[0]));
  if (!length_counts || !first_codes || !next_codes) {
    nanopdf__allocator_free(&context->allocator, length_counts);
    nanopdf__allocator_free(&context->allocator, first_codes);
    nanopdf__allocator_free(&context->allocator, next_codes);
    nanopdf_jbig2_huffman_table_destroy(context, table);
    return 0;
  }
  memset(length_counts, 0, (size_t)(max_prefix_length + 1) * sizeof(length_counts[0]));
  memset(first_codes, 0, (size_t)(max_prefix_length + 1) * sizeof(first_codes[0]));
  memset(next_codes, 0, (size_t)(max_prefix_length + 1) * sizeof(next_codes[0]));
  for (i = 0; i < entry_count; ++i) {
    if (table->prefix_lengths[i] > 0) {
      length_counts[table->prefix_lengths[i]]++;
    }
  }
  for (length = 1; length <= max_prefix_length; ++length) {
    code = (code + length_counts[length - 1]) << 1;
    first_codes[length] = code;
    next_codes[length] = code;
  }
  for (i = 0; i < entry_count; ++i) {
    int prefix_length = table->prefix_lengths[i];
    if (prefix_length > 0 && prefix_length <= max_prefix_length) {
      table->codes[i] = next_codes[prefix_length]++;
    }
  }
  if (table->has_oob) {
    table->oob_index = entry_count - 1u;
  }
  table->is_ok = 1;
  nanopdf__allocator_free(&context->allocator, length_counts);
  nanopdf__allocator_free(&context->allocator, first_codes);
  nanopdf__allocator_free(&context->allocator, next_codes);
  return 1;
}

int nanopdf_jbig2_huffman_table_decode(
    nanopdf_jbig2_huffman_table* table,
    nanopdf_jbig2_bitstream* stream,
    int32_t* out_value) {
  int max_prefix_length = 0;
  uint32_t saved_offset;
  uint32_t saved_bit_pos;
  int32_t code = 0;
  int length;
  size_t i;
  if (!table || !stream || !out_value || !table->is_ok || table->count == 0) {
    if (out_value) {
      *out_value = 0;
    }
    return -1;
  }
  for (i = 0; i < table->count; ++i) {
    if (table->prefix_lengths[i] > max_prefix_length) {
      max_prefix_length = table->prefix_lengths[i];
    }
  }
  if (max_prefix_length == 0) {
    *out_value = 0;
    return -1;
  }
  saved_offset = nanopdf_jbig2_bitstream_offset(stream);
  saved_bit_pos = nanopdf_jbig2_bitstream_bit_position(stream);
  for (length = 1; length <= max_prefix_length; ++length) {
    uint32_t bit = 0;
    if (nanopdf_jbig2_bitstream_read_bit_u32(stream, &bit) != 0) {
      nanopdf_jbig2_bitstream_set_offset(stream, saved_offset);
      nanopdf_jbig2_bitstream_set_bit_position(stream, saved_bit_pos);
      *out_value = 0;
      return -1;
    }
    code = (code << 1) | (int32_t)bit;
    for (i = 0; i < table->count; ++i) {
      if (table->prefix_lengths[i] == length && table->codes[i] == code) {
        int32_t value;
        if (table->has_oob && i == table->oob_index) {
          *out_value = 0;
          return NANOPDF_JBIG2_OOB;
        }
        value = table->range_lows[i];
        if (table->range_lengths[i] > 0 && table->range_lengths[i] < 32) {
          int32_t extra_bits = 0;
          if (nanopdf_jbig2_bitstream_read_bits_i32(
                  stream, (uint32_t)table->range_lengths[i], &extra_bits) != 0) {
            *out_value = 0;
            return -1;
          }
          value += extra_bits;
        } else if (table->range_lengths[i] == 32) {
          int32_t extra_bits = 0;
          if (nanopdf_jbig2_bitstream_read_bits_i32(stream, 32, &extra_bits) != 0) {
            *out_value = 0;
            return -1;
          }
          value = table->range_lows[i] < 0 ? table->range_lows[i] - extra_bits - 1
                                           : table->range_lows[i] + extra_bits;
        }
        *out_value = value;
        return 0;
      }
    }
  }
  nanopdf_jbig2_bitstream_set_offset(stream, saved_offset);
  nanopdf_jbig2_bitstream_set_bit_position(stream, saved_bit_pos);
  *out_value = 0;
  return -1;
}

void nanopdf_jbig2_arith_ctx_init(nanopdf_jbig2_arith_ctx* context) {
  if (context) {
    context->mps = 0;
    context->index = 0;
  }
}

static int arith_decode_nlps(nanopdf_jbig2_arith_ctx* context, const nanopdf_jbig2_qe* qe) {
  int value = context->mps ? 0 : 1;
  if (qe->switch_mps) {
    context->mps = context->mps ? 0u : 1u;
  }
  context->index = qe->nlps;
  return value;
}

static int arith_decode_nmps(nanopdf_jbig2_arith_ctx* context, const nanopdf_jbig2_qe* qe) {
  context->index = qe->nmps;
  return context->mps ? 1 : 0;
}

static void arith_byte_in(nanopdf_jbig2_arith_decoder* decoder) {
  if (decoder->b == 0xffu) {
    uint8_t b1 = nanopdf_jbig2_bitstream_next_byte_arith(decoder->stream);
    if (b1 > 0x8fu) {
      decoder->ct = 8;
    } else {
      nanopdf_jbig2_bitstream_inc_byte(decoder->stream);
      decoder->b = b1;
      decoder->c = decoder->c + 0xfe00u - ((uint32_t)decoder->b << 9);
      decoder->ct = 7;
    }
  } else {
    nanopdf_jbig2_bitstream_inc_byte(decoder->stream);
    decoder->b = nanopdf_jbig2_bitstream_current_byte_arith(decoder->stream);
    decoder->c = decoder->c + 0xff00u - ((uint32_t)decoder->b << 8);
    decoder->ct = 8;
  }
  if (!nanopdf_jbig2_bitstream_in_bounds(decoder->stream)) {
    decoder->complete = 1;
  }
}

void nanopdf_jbig2_arith_decoder_init(
    nanopdf_jbig2_arith_decoder* decoder,
    nanopdf_jbig2_bitstream* stream) {
  if (!decoder) {
    return;
  }
  memset(decoder, 0, sizeof(*decoder));
  decoder->stream = stream;
  decoder->b = nanopdf_jbig2_bitstream_current_byte_arith(stream);
  decoder->c = ((uint32_t)(decoder->b ^ 0xffu)) << 16;
  arith_byte_in(decoder);
  decoder->c <<= 7;
  decoder->ct -= 7;
  decoder->a = NANOPDF_JBIG2_DEFAULT_A;
}

static void arith_read_value_a(nanopdf_jbig2_arith_decoder* decoder) {
  do {
    if (decoder->ct == 0) {
      arith_byte_in(decoder);
    }
    decoder->a <<= 1;
    decoder->c <<= 1;
    --decoder->ct;
  } while ((decoder->a & NANOPDF_JBIG2_DEFAULT_A) == 0);
}

int nanopdf_jbig2_arith_decode(
    nanopdf_jbig2_arith_decoder* decoder,
    nanopdf_jbig2_arith_ctx* context) {
  const nanopdf_jbig2_qe* qe;
  int value;
  if (!decoder || !context || context->index >= 47u) {
    return 0;
  }
  qe = &k_qe_table[context->index];
  decoder->a -= qe->qe;
  if ((decoder->c >> 16) < decoder->a) {
    if (decoder->a & NANOPDF_JBIG2_DEFAULT_A) {
      return context->mps ? 1 : 0;
    }
    value = decoder->a < qe->qe ? arith_decode_nlps(context, qe) : arith_decode_nmps(context, qe);
    arith_read_value_a(decoder);
    return value;
  }
  decoder->c -= decoder->a << 16;
  value = decoder->a < qe->qe ? arith_decode_nmps(context, qe) : arith_decode_nlps(context, qe);
  decoder->a = qe->qe;
  arith_read_value_a(decoder);
  return value;
}

void nanopdf_jbig2_arith_int_decoder_init(nanopdf_jbig2_arith_int_decoder* decoder) {
  size_t i;
  if (!decoder) {
    return;
  }
  for (i = 0; i < 512; ++i) {
    nanopdf_jbig2_arith_ctx_init(&decoder->iax[i]);
  }
}

static int shift_or(int value, int bit) {
  return (value << 1) | bit;
}

static size_t arith_int_recursive_decode(
    nanopdf_jbig2_arith_decoder* arith_decoder,
    nanopdf_jbig2_arith_ctx* contexts,
    int* prev,
    size_t depth) {
  int bit;
  if (depth == 5) {
    return 5;
  }
  bit = nanopdf_jbig2_arith_decode(arith_decoder, &contexts[*prev]);
  *prev = shift_or(*prev, bit);
  if (!bit) {
    return depth;
  }
  return arith_int_recursive_decode(arith_decoder, contexts, prev, depth + 1);
}

int nanopdf_jbig2_arith_int_decode(
    nanopdf_jbig2_arith_int_decoder* decoder,
    nanopdf_jbig2_arith_decoder* arith_decoder,
    int* out_value) {
  int prev = 1;
  int sign;
  size_t decode_index;
  int temp = 0;
  int i;
  int base_value;
  int value;
  if (!decoder || !arith_decoder || !out_value) {
    return 0;
  }
  sign = nanopdf_jbig2_arith_decode(arith_decoder, &decoder->iax[prev]);
  prev = shift_or(prev, sign);
  decode_index = arith_int_recursive_decode(arith_decoder, decoder->iax, &prev, 0);
  for (i = 0; i < k_arith_int_decode_data[decode_index].need_bits; ++i) {
    int bit = nanopdf_jbig2_arith_decode(arith_decoder, &decoder->iax[prev]);
    prev = shift_or(prev, bit);
    if (prev >= 256) {
      prev = (prev & 511) | 256;
    }
    temp = shift_or(temp, bit);
  }
  base_value = k_arith_int_decode_data[decode_index].value;
  if (temp > 0 && base_value > INT_MAX - temp) {
    *out_value = 0;
    return 0;
  }
  value = base_value + temp;
  if (sign == 1 && value > 0) {
    value = -value;
  }
  *out_value = value;
  return sign != 1 || value != 0;
}

int nanopdf_jbig2_arith_iaid_decoder_init(
    nanopdf_context* context,
    nanopdf_jbig2_arith_iaid_decoder* decoder,
    uint8_t symbol_code_len) {
  size_t count;
  size_t i;
  if (!context || !decoder || symbol_code_len >= sizeof(size_t) * CHAR_BIT) {
    return 0;
  }
  memset(decoder, 0, sizeof(*decoder));
  count = (size_t)1u << symbol_code_len;
  decoder->iaid = (nanopdf_jbig2_arith_ctx*)nanopdf__allocator_alloc(
      &context->allocator, count * sizeof(decoder->iaid[0]));
  if (!decoder->iaid) {
    return 0;
  }
  decoder->symbol_code_len = symbol_code_len;
  decoder->iaid_count = count;
  for (i = 0; i < count; ++i) {
    nanopdf_jbig2_arith_ctx_init(&decoder->iaid[i]);
  }
  return 1;
}

void nanopdf_jbig2_arith_iaid_decoder_destroy(
    nanopdf_context* context,
    nanopdf_jbig2_arith_iaid_decoder* decoder) {
  if (!context || !decoder) {
    return;
  }
  nanopdf__allocator_free(&context->allocator, decoder->iaid);
  memset(decoder, 0, sizeof(*decoder));
}

void nanopdf_jbig2_arith_iaid_decode(
    nanopdf_jbig2_arith_iaid_decoder* decoder,
    nanopdf_jbig2_arith_decoder* arith_decoder,
    uint32_t* out_value) {
  int prev = 1;
  uint8_t i;
  if (!decoder || !arith_decoder || !out_value) {
    return;
  }
  for (i = 0; i < decoder->symbol_code_len; ++i) {
    int bit = nanopdf_jbig2_arith_decode(arith_decoder, &decoder->iaid[prev]);
    prev = shift_or(prev, bit);
  }
  *out_value = (uint32_t)(prev - (1 << decoder->symbol_code_len));
}

void nanopdf_jbig2_grd_proc_init(nanopdf_jbig2_grd_proc* proc) {
  if (!proc) {
    return;
  }
  memset(proc, 0, sizeof(*proc));
  proc->gbat[0] = 3;
  proc->gbat[1] = -1;
  proc->gbat[2] = -3;
  proc->gbat[3] = -1;
  proc->gbat[4] = 2;
  proc->gbat[5] = -2;
  proc->gbat[6] = -2;
  proc->gbat[7] = -2;
}

static int grd_decode_context(
    nanopdf_jbig2_arith_decoder* arith_decoder,
    nanopdf_jbig2_arith_ctx* contexts,
    size_t context_count,
    uint32_t context_index,
    int* out_bit) {
  if (!arith_decoder || !contexts || !out_bit || context_index >= context_count) {
    return 0;
  }
  if (arith_decoder->complete) {
    return 0;
  }
  *out_bit = nanopdf_jbig2_arith_decode(arith_decoder, &contexts[context_index]);
  return 1;
}

static int grd_decode_arith_template_unopt(
    const nanopdf_jbig2_grd_proc* proc,
    nanopdf_jbig2_arith_decoder* arith_decoder,
    nanopdf_jbig2_arith_ctx* contexts,
    size_t context_count,
    int unopt,
    nanopdf_jbig2_image* image) {
  int ltp = 0;
  uint8_t mod2 = (uint8_t)(unopt % 2);
  uint8_t div2 = (uint8_t)(unopt / 2);
  uint8_t shift = (uint8_t)(4 - unopt);
  uint32_t h;

  nanopdf_jbig2_image_fill(image, 0);
  for (h = 0; h < proc->height; ++h) {
    if (proc->tpgdon) {
      int bit = 0;
      if (!grd_decode_context(
              arith_decoder, contexts, context_count, k_grd_opt_constant1[unopt], &bit)) {
        return 0;
      }
      ltp ^= bit;
    }
    if (ltp) {
      nanopdf_jbig2_image_copy_line(image, (int32_t)h, (int32_t)h - 1);
      continue;
    }

    {
      uint32_t line1 =
          (uint32_t)nanopdf_jbig2_image_get_pixel(image, 1 + mod2, (int32_t)h - 2);
      uint32_t line2 =
          (uint32_t)nanopdf_jbig2_image_get_pixel(image, 2 - div2, (int32_t)h - 1);
      uint32_t line3 = 0;
      uint32_t w;

      line1 |= (uint32_t)nanopdf_jbig2_image_get_pixel(image, mod2, (int32_t)h - 2) << 1;
      if (unopt == 1) {
        line1 |= (uint32_t)nanopdf_jbig2_image_get_pixel(image, 0, (int32_t)h - 2) << 2;
      }
      line2 |=
          (uint32_t)nanopdf_jbig2_image_get_pixel(image, 1 - div2, (int32_t)h - 1) << 1;
      if (unopt < 2) {
        line2 |= (uint32_t)nanopdf_jbig2_image_get_pixel(image, 0, (int32_t)h - 1) << 2;
      }

      for (w = 0; w < proc->width; ++w) {
        int bit = 0;
        if (!proc->use_skip ||
            !proc->skip ||
            !nanopdf_jbig2_image_get_pixel(proc->skip, (int32_t)w, (int32_t)h)) {
          uint32_t context_index = line3;
          context_index |=
              (uint32_t)nanopdf_jbig2_image_get_pixel(
                  image, (int32_t)w + proc->gbat[0], (int32_t)h + proc->gbat[1])
              << shift;
          context_index |= line2 << (shift + 1);
          context_index |= line1 << k_grd_opt_constant9[unopt];
          if (unopt == 0) {
            context_index |=
                (uint32_t)nanopdf_jbig2_image_get_pixel(
                    image, (int32_t)w + proc->gbat[2], (int32_t)h + proc->gbat[3])
                << 10;
            context_index |=
                (uint32_t)nanopdf_jbig2_image_get_pixel(
                    image, (int32_t)w + proc->gbat[4], (int32_t)h + proc->gbat[5])
                << 11;
            context_index |=
                (uint32_t)nanopdf_jbig2_image_get_pixel(
                    image, (int32_t)w + proc->gbat[6], (int32_t)h + proc->gbat[7])
                << 15;
          }
          if (!grd_decode_context(arith_decoder, contexts, context_count, context_index, &bit)) {
            return 0;
          }
          if (bit) {
            nanopdf_jbig2_image_set_pixel(image, (int32_t)w, (int32_t)h, bit);
          }
        }
        line1 = ((line1 << 1) |
                 (uint32_t)nanopdf_jbig2_image_get_pixel(
                     image, (int32_t)w + 2 + mod2, (int32_t)h - 2)) &
                k_grd_opt_constant10[unopt];
        line2 = ((line2 << 1) |
                 (uint32_t)nanopdf_jbig2_image_get_pixel(
                     image, (int32_t)w + 3 - div2, (int32_t)h - 1)) &
                k_grd_opt_constant11[unopt];
        line3 = ((line3 << 1) | (uint32_t)bit) & k_grd_opt_constant12[unopt];
      }
    }
  }
  return 1;
}

static int grd_decode_arith_template3_unopt(
    const nanopdf_jbig2_grd_proc* proc,
    nanopdf_jbig2_arith_decoder* arith_decoder,
    nanopdf_jbig2_arith_ctx* contexts,
    size_t context_count,
    nanopdf_jbig2_image* image) {
  int ltp = 0;
  uint32_t h;

  nanopdf_jbig2_image_fill(image, 0);
  for (h = 0; h < proc->height; ++h) {
    if (proc->tpgdon) {
      int bit = 0;
      if (!grd_decode_context(arith_decoder, contexts, context_count, 0x0195u, &bit)) {
        return 0;
      }
      ltp ^= bit;
    }
    if (ltp) {
      nanopdf_jbig2_image_copy_line(image, (int32_t)h, (int32_t)h - 1);
      continue;
    }
    {
      uint32_t line1 =
          (uint32_t)nanopdf_jbig2_image_get_pixel(image, 1, (int32_t)h - 1);
      uint32_t line2 = 0;
      uint32_t w;
      line1 |= (uint32_t)nanopdf_jbig2_image_get_pixel(image, 0, (int32_t)h - 1) << 1;
      for (w = 0; w < proc->width; ++w) {
        int bit = 0;
        if (!proc->use_skip ||
            !proc->skip ||
            !nanopdf_jbig2_image_get_pixel(proc->skip, (int32_t)w, (int32_t)h)) {
          uint32_t context_index = line2;
          context_index |=
              (uint32_t)nanopdf_jbig2_image_get_pixel(
                  image, (int32_t)w + proc->gbat[0], (int32_t)h + proc->gbat[1])
              << 4;
          context_index |= line1 << 5;
          if (!grd_decode_context(arith_decoder, contexts, context_count, context_index, &bit)) {
            return 0;
          }
        }
        if (bit) {
          nanopdf_jbig2_image_set_pixel(image, (int32_t)w, (int32_t)h, bit);
        }
        line1 = ((line1 << 1) |
                 (uint32_t)nanopdf_jbig2_image_get_pixel(
                     image, (int32_t)w + 2, (int32_t)h - 1)) &
                0x1fu;
        line2 = ((line2 << 1) | (uint32_t)bit) & 0x0fu;
      }
    }
  }
  return 1;
}

int nanopdf_jbig2_grd_proc_decode_arith(
    nanopdf_context* context,
    const nanopdf_jbig2_grd_proc* proc,
    nanopdf_jbig2_arith_decoder* arith_decoder,
    nanopdf_jbig2_arith_ctx* gb_contexts,
    size_t gb_context_count,
    nanopdf_jbig2_image* out_image) {
  int ok = 0;
  if (!context || !proc || !arith_decoder || !gb_contexts || !out_image) {
    return 0;
  }
  if (!nanopdf_jbig2_image_valid_size((int32_t)proc->width, (int32_t)proc->height)) {
    return 0;
  }
  if (!nanopdf_jbig2_image_init(context, out_image, (int32_t)proc->width, (int32_t)proc->height)) {
    return 0;
  }

  switch (proc->gb_template) {
    case 0:
      ok = grd_decode_arith_template_unopt(
          proc, arith_decoder, gb_contexts, gb_context_count, 0, out_image);
      break;
    case 1:
      ok = grd_decode_arith_template_unopt(
          proc, arith_decoder, gb_contexts, gb_context_count, 1, out_image);
      break;
    case 2:
      ok = grd_decode_arith_template_unopt(
          proc, arith_decoder, gb_contexts, gb_context_count, 2, out_image);
      break;
    default:
      ok = grd_decode_arith_template3_unopt(
          proc, arith_decoder, gb_contexts, gb_context_count, out_image);
      break;
  }
  if (!ok) {
    nanopdf_jbig2_image_destroy(context, out_image);
  }
  return ok;
}

int nanopdf_jbig2_grd_proc_decode_mmr(
    nanopdf_context* context,
    const nanopdf_jbig2_grd_proc* proc,
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_image* out_image) {
  nanopdf_ccitt_params params;
  const uint8_t* source = NULL;
  uint32_t source_size = 0;
  uint8_t* decoded = NULL;
  size_t decoded_size = 0;
  int ccitt_pitch;
  int y;
  nanopdf_status status;

  if (!context || !proc || !stream || !out_image ||
      !nanopdf_jbig2_image_valid_size((int32_t)proc->width, (int32_t)proc->height)) {
    return 0;
  }
  nanopdf_jbig2_bitstream_align_byte(stream);
  source = nanopdf_jbig2_bitstream_pointer(stream);
  source_size = nanopdf_jbig2_bitstream_bytes_left(stream);
  if (!source || source_size == 0) {
    return 0;
  }

  memset(&params, 0, sizeof(params));
  params.columns = (int)proc->width;
  params.rows = (int)proc->height;
  params.k = -1;
  params.black_is_1 = 1;
  status = nanopdf__decode_ccitt(
      context, source, source_size, &params, &decoded, &decoded_size);
  if (status != NANOPDF_STATUS_OK || !decoded) {
    nanopdf__allocator_free(&context->allocator, decoded);
    return 0;
  }
  if (!nanopdf_jbig2_image_init(context, out_image, (int32_t)proc->width, (int32_t)proc->height)) {
    nanopdf__allocator_free(&context->allocator, decoded);
    return 0;
  }

  ccitt_pitch = ((int)proc->width + 7) / 8;
  if (decoded_size < (size_t)ccitt_pitch * (size_t)proc->height) {
    nanopdf_jbig2_image_destroy(context, out_image);
    nanopdf__allocator_free(&context->allocator, decoded);
    return 0;
  }
  for (y = 0; y < (int)proc->height; ++y) {
    uint8_t* dest = nanopdf_jbig2_image_get_line(out_image, y);
    if (!dest) {
      nanopdf_jbig2_image_destroy(context, out_image);
      nanopdf__allocator_free(&context->allocator, decoded);
      return 0;
    }
    memcpy(dest, decoded + (size_t)y * (size_t)ccitt_pitch, (size_t)ccitt_pitch);
    if (out_image->stride > ccitt_pitch) {
      memset(dest + ccitt_pitch, 0, (size_t)(out_image->stride - ccitt_pitch));
    }
  }
  nanopdf__allocator_free(&context->allocator, decoded);
  nanopdf_jbig2_bitstream_set_offset(stream, (uint32_t)stream->size);
  return 1;
}

int nanopdf_jbig2_parse_region_info(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_region_info* out_region) {
  if (!stream || !out_region) {
    return 0;
  }
  memset(out_region, 0, sizeof(*out_region));
  return nanopdf_jbig2_bitstream_read_u32(stream, &out_region->width) == 0 &&
         nanopdf_jbig2_bitstream_read_u32(stream, &out_region->height) == 0 &&
         nanopdf_jbig2_bitstream_read_u32(stream, &out_region->x) == 0 &&
         nanopdf_jbig2_bitstream_read_u32(stream, &out_region->y) == 0 &&
         nanopdf_jbig2_bitstream_read_byte(stream, &out_region->flags) == 0;
}

int nanopdf_jbig2_parse_page_info(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_page_info* out_page_info) {
  uint16_t stripe_info = 0;
  if (!stream || !out_page_info) {
    return 0;
  }
  memset(out_page_info, 0, sizeof(*out_page_info));
  if (nanopdf_jbig2_bitstream_read_u32(stream, &out_page_info->width) != 0 ||
      nanopdf_jbig2_bitstream_read_u32(stream, &out_page_info->height) != 0 ||
      nanopdf_jbig2_bitstream_read_u32(stream, &out_page_info->x_resolution) != 0 ||
      nanopdf_jbig2_bitstream_read_u32(stream, &out_page_info->y_resolution) != 0 ||
      nanopdf_jbig2_bitstream_read_byte(stream, &out_page_info->flags) != 0 ||
      nanopdf_jbig2_bitstream_read_u16(stream, &stripe_info) != 0) {
    return 0;
  }
  out_page_info->default_pixel = (uint8_t)((out_page_info->flags & 0x04u) != 0u);
  out_page_info->is_lossless = (uint8_t)((out_page_info->flags & 0x01u) != 0u);
  out_page_info->stripe_height = (uint16_t)(stripe_info & 0x7fffu);
  if (out_page_info->stripe_height == 0u && out_page_info->height <= 0xffffu) {
    out_page_info->stripe_height = (uint16_t)out_page_info->height;
  }
  return 1;
}

int nanopdf_jbig2_parse_generic_region_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_region_info* out_region,
    nanopdf_jbig2_grd_proc* out_proc) {
  uint8_t flags = 0;
  int template_bytes;
  int i;
  if (!stream || !out_region || !out_proc) {
    return 0;
  }
  if (!nanopdf_jbig2_parse_region_info(stream, out_region) ||
      nanopdf_jbig2_bitstream_read_byte(stream, &flags) != 0) {
    return 0;
  }
  nanopdf_jbig2_grd_proc_init(out_proc);
  out_proc->mmr = (uint8_t)((flags & 0x01u) != 0u);
  out_proc->gb_template = (uint8_t)((flags >> 1) & 0x03u);
  out_proc->tpgdon = (uint8_t)((flags & 0x08u) != 0u);
  out_proc->use_skip = 0;
  out_proc->skip = NULL;
  out_proc->width = out_region->width;
  out_proc->height = out_region->height;
  if (out_proc->mmr) {
    return 1;
  }
  template_bytes = out_proc->gb_template == 0 ? 8 : 2;
  for (i = 0; i < template_bytes; ++i) {
    uint8_t value = 0;
    if (nanopdf_jbig2_bitstream_read_byte(stream, &value) != 0) {
      return 0;
    }
    out_proc->gbat[i] = (int8_t)value;
  }
  return 1;
}

int nanopdf_jbig2_parse_symbol_dict_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_symbol_dict_header* out_header) {
  uint16_t flags = 0;
  int at_count;
  int i;
  if (!stream || !out_header) {
    return 0;
  }
  memset(out_header, 0, sizeof(*out_header));
  if (nanopdf_jbig2_bitstream_read_u16(stream, &flags) != 0) {
    return 0;
  }
  out_header->huffman = (uint8_t)((flags & 0x0001u) != 0u);
  out_header->refagg = (uint8_t)((flags & 0x0002u) != 0u);
  out_header->template_id = (uint8_t)((flags >> 10) & 0x03u);
  out_header->refinement_template = (uint8_t)((flags >> 12) & 0x01u);
  out_header->huff_dh = (uint8_t)((flags >> 2) & 0x03u);
  out_header->huff_dw = (uint8_t)((flags >> 4) & 0x03u);
  out_header->huff_bitmap_size = (uint8_t)((flags >> 6) & 0x01u);
  out_header->huff_agg_inst = (uint8_t)((flags >> 7) & 0x01u);

  if (!out_header->huffman) {
    at_count = out_header->template_id == 0u ? 8 : 2;
    for (i = 0; i < at_count; ++i) {
      uint8_t value = 0;
      if (nanopdf_jbig2_bitstream_read_byte(stream, &value) != 0) {
        return 0;
      }
      out_header->sdat[i] = (int8_t)value;
    }
  }
  if (out_header->refagg && out_header->refinement_template == 0u) {
    for (i = 0; i < 4; ++i) {
      uint8_t value = 0;
      if (nanopdf_jbig2_bitstream_read_byte(stream, &value) != 0) {
        return 0;
      }
      out_header->sdrat[i] = (int8_t)value;
    }
  }
  return nanopdf_jbig2_bitstream_read_u32(stream, &out_header->num_exported_symbols) == 0 &&
         nanopdf_jbig2_bitstream_read_u32(stream, &out_header->num_new_symbols) == 0;
}

int nanopdf_jbig2_parse_text_region_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_text_region_header* out_header) {
  uint16_t flags = 0;
  int i;
  if (!stream || !out_header) {
    return 0;
  }
  memset(out_header, 0, sizeof(*out_header));
  if (!nanopdf_jbig2_parse_region_info(stream, &out_header->region) ||
      nanopdf_jbig2_bitstream_read_u16(stream, &flags) != 0) {
    return 0;
  }
  out_header->huffman = (uint8_t)((flags & 0x0001u) != 0u);
  out_header->refine = (uint8_t)((flags & 0x0002u) != 0u);
  out_header->strip_count_log = (uint8_t)((flags >> 2) & 0x03u);
  out_header->ref_corner = (uint8_t)((flags >> 4) & 0x03u);
  out_header->transposed = (uint8_t)((flags & 0x0040u) != 0u);
  out_header->compose_op = (nanopdf_jbig2_compose_op)((flags >> 7) & 0x03u);
  out_header->default_pixel = (uint8_t)((flags >> 9) & 0x01u);
  out_header->ds_offset = (int8_t)((flags >> 10) & 0x1fu);
  if (out_header->ds_offset & 0x10) {
    out_header->ds_offset = (int8_t)(out_header->ds_offset | (int8_t)0xe0);
  }
  out_header->refinement_template = (uint8_t)((flags >> 15) & 0x01u);

  if (out_header->huffman) {
    uint16_t huff_flags = 0;
    if (nanopdf_jbig2_bitstream_read_u16(stream, &huff_flags) != 0) {
      return 0;
    }
    out_header->huff_fs = (uint8_t)(huff_flags & 0x03u);
    out_header->huff_ds = (uint8_t)((huff_flags >> 2) & 0x03u);
    out_header->huff_dt = (uint8_t)((huff_flags >> 4) & 0x03u);
    out_header->huff_rdw = (uint8_t)((huff_flags >> 6) & 0x03u);
    out_header->huff_rdh = (uint8_t)((huff_flags >> 8) & 0x03u);
    out_header->huff_rdx = (uint8_t)((huff_flags >> 10) & 0x03u);
    out_header->huff_rdy = (uint8_t)((huff_flags >> 12) & 0x03u);
    out_header->huff_rsize = (uint8_t)((huff_flags >> 14) & 0x01u);
  }
  if (out_header->refine && out_header->refinement_template == 0u) {
    for (i = 0; i < 4; ++i) {
      uint8_t value = 0;
      if (nanopdf_jbig2_bitstream_read_byte(stream, &value) != 0) {
        return 0;
      }
      out_header->sbrat[i] = (int8_t)value;
    }
  }
  return nanopdf_jbig2_bitstream_read_u32(stream, &out_header->num_instances) == 0;
}

int nanopdf_jbig2_parse_pattern_dict_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_pattern_dict_header* out_header) {
  uint8_t flags = 0;
  uint8_t width = 0;
  uint8_t height = 0;
  if (!stream || !out_header) {
    return 0;
  }
  memset(out_header, 0, sizeof(*out_header));
  if (nanopdf_jbig2_bitstream_read_byte(stream, &flags) != 0 ||
      nanopdf_jbig2_bitstream_read_byte(stream, &width) != 0 ||
      nanopdf_jbig2_bitstream_read_byte(stream, &height) != 0 ||
      nanopdf_jbig2_bitstream_read_u32(stream, &out_header->gray_max) != 0) {
    return 0;
  }
  out_header->mmr = (uint8_t)((flags & 0x01u) != 0u);
  out_header->template_id = (uint8_t)((flags >> 1) & 0x03u);
  out_header->width = width;
  out_header->height = height;
  return 1;
}

int nanopdf_jbig2_parse_generic_refinement_region_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_generic_refinement_region_header* out_header) {
  uint8_t flags = 0;
  int i;
  if (!stream || !out_header) {
    return 0;
  }
  memset(out_header, 0, sizeof(*out_header));
  for (i = 0; i < 4; ++i) {
    out_header->grat[i] = -1;
  }
  if (!nanopdf_jbig2_parse_region_info(stream, &out_header->region) ||
      nanopdf_jbig2_bitstream_read_byte(stream, &flags) != 0) {
    return 0;
  }
  out_header->template_id = (uint8_t)(flags & 0x01u);
  out_header->typical_prediction = (uint8_t)((flags & 0x02u) != 0u);
  if (out_header->template_id == 0u) {
    for (i = 0; i < 4; ++i) {
      uint8_t value = 0;
      if (nanopdf_jbig2_bitstream_read_byte(stream, &value) != 0) {
        return 0;
      }
      out_header->grat[i] = (int8_t)value;
    }
  }
  return 1;
}

int nanopdf_jbig2_parse_halftone_region_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_halftone_region_header* out_header) {
  uint8_t flags = 0;
  uint32_t value = 0;
  if (!stream || !out_header) {
    return 0;
  }
  memset(out_header, 0, sizeof(*out_header));
  if (!nanopdf_jbig2_parse_region_info(stream, &out_header->region) ||
      nanopdf_jbig2_bitstream_read_byte(stream, &flags) != 0 ||
      nanopdf_jbig2_bitstream_read_u32(stream, &out_header->grid_width) != 0 ||
      nanopdf_jbig2_bitstream_read_u32(stream, &out_header->grid_height) != 0 ||
      nanopdf_jbig2_bitstream_read_u32(stream, &value) != 0) {
    return 0;
  }
  out_header->grid_x = (int32_t)value;
  if (nanopdf_jbig2_bitstream_read_u32(stream, &value) != 0) {
    return 0;
  }
  out_header->grid_y = (int32_t)value;
  if (nanopdf_jbig2_bitstream_read_u32(stream, &value) != 0) {
    return 0;
  }
  out_header->grid_vector_x = (int32_t)value;
  if (nanopdf_jbig2_bitstream_read_u32(stream, &value) != 0) {
    return 0;
  }
  out_header->grid_vector_y = (int32_t)value;
  out_header->mmr = (uint8_t)((flags & 0x01u) != 0u);
  out_header->template_id = (uint8_t)((flags >> 1) & 0x03u);
  out_header->enable_skip = (uint8_t)((flags & 0x08u) != 0u);
  out_header->compose_op = (nanopdf_jbig2_compose_op)((flags >> 4) & 0x07u);
  out_header->default_pixel = (uint8_t)((flags & 0x80u) != 0u);
  return 1;
}

int nanopdf_jbig2_parse_end_of_stripe(
    nanopdf_jbig2_bitstream* stream,
    uint32_t* out_y_location) {
  if (!stream || !out_y_location) {
    return 0;
  }
  return nanopdf_jbig2_bitstream_read_u32(stream, out_y_location) == 0;
}

int nanopdf_jbig2_page_init_from_info(
    nanopdf_context* context,
    const nanopdf_jbig2_page_info* page_info,
    nanopdf_jbig2_image* out_page) {
  uint32_t height;
  if (!context || !page_info || !out_page) {
    return 0;
  }
  height = page_info->height == 0xffffffffu ? page_info->stripe_height : page_info->height;
  if (height == 0) {
    return 0;
  }
  if (!nanopdf_jbig2_image_init(
          context, out_page, (int32_t)page_info->width, (int32_t)height)) {
    return 0;
  }
  nanopdf_jbig2_image_fill(out_page, page_info->default_pixel ? 1 : 0);
  return 1;
}

int nanopdf_jbig2_page_compose_region(
    nanopdf_jbig2_image* page,
    const nanopdf_jbig2_region_info* region,
    const nanopdf_jbig2_image* image) {
  nanopdf_jbig2_compose_op op;
  if (!page || !region || !image) {
    return 0;
  }
  op = (nanopdf_jbig2_compose_op)(region->flags & 0x07u);
  return nanopdf_jbig2_image_compose_from(
      page, (int64_t)region->x, (int64_t)region->y, image, op);
}

int nanopdf_jbig2_decode_page(
    nanopdf_context* context,
    const uint8_t* data,
    size_t data_size,
    const uint8_t* globals,
    size_t globals_size,
    nanopdf_jbig2_image* out_page) {
  nanopdf_jbig2_bitstream stream;
  nanopdf_jbig2_bitstream globals_stream;
  nanopdf_jbig2_image page;
  jbig2_segment_table segment_table;
  int has_page = 0;
  int parsed_any = 0;
  int unsupported_segment = 0;

  if (!context || !data || data_size == 0 || !out_page) {
    return -1;
  }
  nanopdf_jbig2_image_init_external(&page, 0, 0, 0, NULL);
  jbig2_segment_table_init(&segment_table);
  nanopdf_jbig2_image_destroy(context, out_page);
  nanopdf_jbig2_image_init_external(out_page, 0, 0, 0, NULL);

  if (globals && globals_size > 0) {
    nanopdf_jbig2_bitstream_init(&globals_stream, globals, globals_size, 0);
    while (nanopdf_jbig2_bitstream_bytes_left(&globals_stream) > 0) {
      nanopdf_jbig2_segment segment;
      uint32_t next_offset;
      nanopdf_jbig2_segment_init(&segment);
      if (!nanopdf_jbig2_segment_parse_header(context, &globals_stream, &segment)) {
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
      parsed_any = 1;
      next_offset = nanopdf_jbig2_bitstream_offset(&globals_stream);
      nanopdf_jbig2_bitstream_set_offset(&globals_stream, segment.data_offset);
      if (segment.type == NANOPDF_JBIG2_SEGMENT_SYMBOL_DICTIONARY &&
          !jbig2_process_symbol_dict_segment(
              context, &globals_stream, &segment_table, &segment, &unsupported_segment)) {
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      } else if (segment.type == NANOPDF_JBIG2_SEGMENT_PATTERN_DICTIONARY &&
                 !jbig2_process_pattern_dict_segment(
                     context, &globals_stream, &segment_table, &segment)) {
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      } else if (segment.type == NANOPDF_JBIG2_SEGMENT_TABLES &&
                 !jbig2_process_table_segment(
                     context, &globals_stream, &segment_table, &segment)) {
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      } else if (segment.type == NANOPDF_JBIG2_SEGMENT_END_OF_FILE ||
                 segment.type == NANOPDF_JBIG2_SEGMENT_PROFILES ||
                 segment.type == NANOPDF_JBIG2_SEGMENT_EXTENSION) {
        segment.state = NANOPDF_JBIG2_SEGMENT_PARSE_COMPLETE;
      } else if (segment.type != NANOPDF_JBIG2_SEGMENT_SYMBOL_DICTIONARY &&
                 segment.type != NANOPDF_JBIG2_SEGMENT_PATTERN_DICTIONARY &&
                 segment.type != NANOPDF_JBIG2_SEGMENT_TABLES) {
        unsupported_segment = 1;
      }
      nanopdf_jbig2_bitstream_set_offset(&globals_stream, next_offset);
      nanopdf_jbig2_segment_destroy(context, &segment);
    }
  }

  nanopdf_jbig2_bitstream_init(&stream, data, data_size, 0);
  while (nanopdf_jbig2_bitstream_bytes_left(&stream) > 0) {
    nanopdf_jbig2_segment segment;
    uint32_t next_offset;
    nanopdf_jbig2_segment_init(&segment);
    if (!nanopdf_jbig2_segment_parse_header(context, &stream, &segment)) {
      nanopdf_jbig2_segment_destroy(context, &segment);
      nanopdf_jbig2_image_destroy(context, &page);
      jbig2_segment_table_destroy(context, &segment_table);
      return -1;
    }
    parsed_any = 1;
    next_offset = nanopdf_jbig2_bitstream_offset(&stream);
    nanopdf_jbig2_bitstream_set_offset(&stream, segment.data_offset);

    if (segment.type == NANOPDF_JBIG2_SEGMENT_PAGE_INFO) {
      nanopdf_jbig2_page_info page_info;
      if (!nanopdf_jbig2_parse_page_info(&stream, &page_info)) {
        nanopdf_jbig2_segment_destroy(context, &segment);
        nanopdf_jbig2_image_destroy(context, &page);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
      nanopdf_jbig2_image_destroy(context, &page);
      if (!nanopdf_jbig2_page_init_from_info(context, &page_info, &page)) {
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
      has_page = 1;
    } else if (segment.type == NANOPDF_JBIG2_SEGMENT_SYMBOL_DICTIONARY) {
      if (!jbig2_process_symbol_dict_segment(
              context, &stream, &segment_table, &segment, &unsupported_segment)) {
        nanopdf_jbig2_image_destroy(context, &page);
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
    } else if (segment.type == NANOPDF_JBIG2_SEGMENT_PATTERN_DICTIONARY) {
      if (!jbig2_process_pattern_dict_segment(context, &stream, &segment_table, &segment)) {
        nanopdf_jbig2_image_destroy(context, &page);
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
    } else if (segment.type == NANOPDF_JBIG2_SEGMENT_TABLES) {
      if (!jbig2_process_table_segment(context, &stream, &segment_table, &segment)) {
        nanopdf_jbig2_image_destroy(context, &page);
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
    } else if (segment.type == NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_TEXT_REGION ||
               segment.type == NANOPDF_JBIG2_SEGMENT_IMMEDIATE_TEXT_REGION ||
               segment.type == NANOPDF_JBIG2_SEGMENT_IMMEDIATE_LOSSLESS_TEXT_REGION) {
      nanopdf_jbig2_text_region_header text_header;
      if (!nanopdf_jbig2_parse_text_region_header(&stream, &text_header)) {
        nanopdf_jbig2_image_destroy(context, &page);
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
      (void)jbig2_segment_referenced_symbol_count(&segment_table, &segment);
      if ((segment.referred_to_segment_count > 0 &&
           !jbig2_segment_referenced_symbol_dicts_resolved(&segment_table, &segment)) ||
          !jbig2_text_region_is_empty_supported(&text_header)) {
        unsupported_segment = 1;
      } else if (segment.type == NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_TEXT_REGION) {
        segment.image = (nanopdf_jbig2_image*)nanopdf__allocator_alloc(
            &context->allocator, sizeof(segment.image[0]));
        if (!segment.image) {
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        nanopdf_jbig2_image_init_external(segment.image, 0, 0, 0, NULL);
        if (!jbig2_create_empty_text_region_image(context, &text_header, segment.image)) {
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        segment.result_type = NANOPDF_JBIG2_IMAGE_POINTER;
        segment.state = NANOPDF_JBIG2_SEGMENT_PARSE_COMPLETE;
        if (!jbig2_segment_table_add(context, &segment_table, &segment)) {
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
      } else if (segment.type == NANOPDF_JBIG2_SEGMENT_IMMEDIATE_TEXT_REGION ||
                 segment.type == NANOPDF_JBIG2_SEGMENT_IMMEDIATE_LOSSLESS_TEXT_REGION) {
        nanopdf_jbig2_image text_image;
        nanopdf_jbig2_image_init_external(&text_image, 0, 0, 0, NULL);
        if (!has_page ||
            !jbig2_create_empty_text_region_image(context, &text_header, &text_image)) {
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        text_header.region.flags = (uint8_t)text_header.compose_op;
        if (!nanopdf_jbig2_page_compose_region(&page, &text_header.region, &text_image)) {
          nanopdf_jbig2_image_destroy(context, &text_image);
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        nanopdf_jbig2_image_destroy(context, &text_image);
      }
    } else if (segment.type == NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_GENERIC_REGION ||
               segment.type == NANOPDF_JBIG2_SEGMENT_IMMEDIATE_GENERIC_REGION ||
               segment.type == NANOPDF_JBIG2_SEGMENT_IMMEDIATE_LOSSLESS_GENERIC_REGION) {
      nanopdf_jbig2_region_info region;
      nanopdf_jbig2_grd_proc grd;
      nanopdf_jbig2_image region_image;
      int decoded = 0;
      nanopdf_jbig2_image_init_external(&region_image, 0, 0, 0, NULL);
      if (!nanopdf_jbig2_parse_generic_region_header(&stream, &region, &grd)) {
        nanopdf_jbig2_image_destroy(context, &page);
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
      if (grd.mmr) {
        decoded = nanopdf_jbig2_grd_proc_decode_mmr(context, &grd, &stream, &region_image);
      } else {
        size_t context_count = jbig2_gb_context_count(grd.gb_template);
        nanopdf_jbig2_arith_ctx* gb_contexts =
            (nanopdf_jbig2_arith_ctx*)nanopdf__allocator_alloc(
                &context->allocator, sizeof(nanopdf_jbig2_arith_ctx) * context_count);
        if (gb_contexts) {
          size_t i;
          nanopdf_jbig2_arith_decoder arith;
          for (i = 0; i < context_count; ++i) {
            nanopdf_jbig2_arith_ctx_init(&gb_contexts[i]);
          }
          nanopdf_jbig2_arith_decoder_init(&arith, &stream);
          decoded = nanopdf_jbig2_grd_proc_decode_arith(
              context, &grd, &arith, gb_contexts, context_count, &region_image);
          nanopdf__allocator_free(&context->allocator, gb_contexts);
        }
      }
      if (!decoded) {
        nanopdf_jbig2_image_destroy(context, &region_image);
        nanopdf_jbig2_image_destroy(context, &page);
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
      if (segment.type == NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_GENERIC_REGION) {
        segment.image = (nanopdf_jbig2_image*)nanopdf__allocator_alloc(
            &context->allocator, sizeof(segment.image[0]));
        if (!segment.image) {
          nanopdf_jbig2_image_destroy(context, &region_image);
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        *segment.image = region_image;
        nanopdf_jbig2_image_init_external(&region_image, 0, 0, 0, NULL);
        segment.result_type = NANOPDF_JBIG2_IMAGE_POINTER;
        segment.state = NANOPDF_JBIG2_SEGMENT_PARSE_COMPLETE;
        if (!jbig2_segment_table_add(context, &segment_table, &segment)) {
          nanopdf_jbig2_image_destroy(context, &region_image);
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
      } else {
        if (!has_page || !nanopdf_jbig2_page_compose_region(&page, &region, &region_image)) {
          nanopdf_jbig2_image_destroy(context, &region_image);
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        nanopdf_jbig2_image_destroy(context, &region_image);
      }
    } else if (segment.type == NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_GENERIC_REFINEMENT_REGION ||
               segment.type == NANOPDF_JBIG2_SEGMENT_IMMEDIATE_GENERIC_REFINEMENT_REGION ||
               segment.type ==
                   NANOPDF_JBIG2_SEGMENT_IMMEDIATE_LOSSLESS_GENERIC_REFINEMENT_REGION) {
      nanopdf_jbig2_generic_refinement_region_header refinement_header;
      int is_empty_refinement = 0;
      if (!nanopdf_jbig2_parse_generic_refinement_region_header(&stream, &refinement_header)) {
        nanopdf_jbig2_image_destroy(context, &page);
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
      if (segment.data_length != 0xffffffffu &&
          segment.data_offset <= UINT32_MAX - segment.data_length &&
          nanopdf_jbig2_bitstream_offset(&stream) >= segment.data_offset + segment.data_length) {
        is_empty_refinement = 1;
      }
      if (segment.type == NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_GENERIC_REFINEMENT_REGION) {
        const nanopdf_jbig2_image* reference_image =
            jbig2_segment_referenced_image(&segment_table, &segment);
        if (!reference_image && has_page) {
          reference_image = &page;
        }
        if (!is_empty_refinement && !reference_image) {
          unsupported_segment = 1;
        } else {
          segment.image = (nanopdf_jbig2_image*)nanopdf__allocator_alloc(
              &context->allocator, sizeof(segment.image[0]));
          if (!segment.image) {
            nanopdf_jbig2_image_destroy(context, &page);
            nanopdf_jbig2_segment_destroy(context, &segment);
            jbig2_segment_table_destroy(context, &segment_table);
            return -1;
          }
          nanopdf_jbig2_image_init_external(segment.image, 0, 0, 0, NULL);
          if (!jbig2_decode_refinement_region_image(
                  context,
                  &refinement_header,
                  reference_image,
                  is_empty_refinement,
                  &stream,
                  segment.image)) {
            nanopdf__allocator_free(&context->allocator, segment.image);
            segment.image = NULL;
            unsupported_segment = 1;
          } else {
            segment.result_type = NANOPDF_JBIG2_IMAGE_POINTER;
            segment.state = NANOPDF_JBIG2_SEGMENT_PARSE_COMPLETE;
            if (!jbig2_segment_table_add(context, &segment_table, &segment)) {
              nanopdf_jbig2_image_destroy(context, &page);
              nanopdf_jbig2_segment_destroy(context, &segment);
              jbig2_segment_table_destroy(context, &segment_table);
              return -1;
            }
          }
        }
      } else {
        const nanopdf_jbig2_image* reference_image =
            jbig2_segment_referenced_image(&segment_table, &segment);
        nanopdf_jbig2_image refinement_image;
        nanopdf_jbig2_image_init_external(&refinement_image, 0, 0, 0, NULL);
        if (!reference_image && has_page) {
          reference_image = &page;
        }
        if (!has_page || !reference_image) {
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        if (!jbig2_decode_refinement_region_image(
                context,
                &refinement_header,
                reference_image,
                is_empty_refinement,
                &stream,
                &refinement_image)) {
          nanopdf_jbig2_image_destroy(context, &refinement_image);
          unsupported_segment = 1;
        }
        if (refinement_image.data &&
            !nanopdf_jbig2_page_compose_region(
                &page, &refinement_header.region, &refinement_image)) {
          nanopdf_jbig2_image_destroy(context, &refinement_image);
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        nanopdf_jbig2_image_destroy(context, &refinement_image);
      }
    } else if (segment.type == NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_HALFTONE_REGION ||
               segment.type == NANOPDF_JBIG2_SEGMENT_IMMEDIATE_HALFTONE_REGION ||
               segment.type == NANOPDF_JBIG2_SEGMENT_IMMEDIATE_LOSSLESS_HALFTONE_REGION) {
      nanopdf_jbig2_halftone_region_header halftone_header;
      if (!nanopdf_jbig2_parse_halftone_region_header(&stream, &halftone_header)) {
        nanopdf_jbig2_image_destroy(context, &page);
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
      if (!jbig2_halftone_region_is_empty_supported(&halftone_header)) {
        unsupported_segment = 1;
      } else if (segment.type == NANOPDF_JBIG2_SEGMENT_INTERMEDIATE_HALFTONE_REGION) {
        segment.image = (nanopdf_jbig2_image*)nanopdf__allocator_alloc(
            &context->allocator, sizeof(segment.image[0]));
        if (!segment.image) {
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        nanopdf_jbig2_image_init_external(segment.image, 0, 0, 0, NULL);
        if (!jbig2_create_empty_halftone_region_image(context, &halftone_header, segment.image)) {
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        segment.result_type = NANOPDF_JBIG2_IMAGE_POINTER;
        segment.state = NANOPDF_JBIG2_SEGMENT_PARSE_COMPLETE;
        if (!jbig2_segment_table_add(context, &segment_table, &segment)) {
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
      } else {
        nanopdf_jbig2_image halftone_image;
        nanopdf_jbig2_image_init_external(&halftone_image, 0, 0, 0, NULL);
        if (!has_page ||
            !jbig2_create_empty_halftone_region_image(
                context, &halftone_header, &halftone_image)) {
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        halftone_header.region.flags = (uint8_t)halftone_header.compose_op;
        if (!nanopdf_jbig2_page_compose_region(
                &page, &halftone_header.region, &halftone_image)) {
          nanopdf_jbig2_image_destroy(context, &halftone_image);
          nanopdf_jbig2_image_destroy(context, &page);
          nanopdf_jbig2_segment_destroy(context, &segment);
          jbig2_segment_table_destroy(context, &segment_table);
          return -1;
        }
        nanopdf_jbig2_image_destroy(context, &halftone_image);
      }
    } else if (segment.type == NANOPDF_JBIG2_SEGMENT_END_OF_PAGE ||
               segment.type == NANOPDF_JBIG2_SEGMENT_END_OF_FILE) {
      nanopdf_jbig2_segment_destroy(context, &segment);
      break;
    } else if (segment.type == NANOPDF_JBIG2_SEGMENT_END_OF_STRIPE) {
      uint32_t y_location = 0;
      if (!has_page || !nanopdf_jbig2_parse_end_of_stripe(&stream, &y_location)) {
        nanopdf_jbig2_image_destroy(context, &page);
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
      if (y_location >= (uint32_t)INT32_MAX) {
        nanopdf_jbig2_image_destroy(context, &page);
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
      if ((int32_t)(y_location + 1u) > page.height &&
          !nanopdf_jbig2_image_expand(context, &page, (int32_t)(y_location + 1u), 0)) {
        nanopdf_jbig2_image_destroy(context, &page);
        nanopdf_jbig2_segment_destroy(context, &segment);
        jbig2_segment_table_destroy(context, &segment_table);
        return -1;
      }
    } else if (segment.type == NANOPDF_JBIG2_SEGMENT_PROFILES ||
               segment.type == NANOPDF_JBIG2_SEGMENT_EXTENSION) {
      segment.state = NANOPDF_JBIG2_SEGMENT_PARSE_COMPLETE;
    } else {
      unsupported_segment = 1;
    }

    nanopdf_jbig2_bitstream_set_offset(&stream, next_offset);
    nanopdf_jbig2_segment_destroy(context, &segment);
  }

  if (!parsed_any) {
    nanopdf_jbig2_image_destroy(context, &page);
    jbig2_segment_table_destroy(context, &segment_table);
    return -1;
  }
  if (unsupported_segment || !has_page || !page.data || page.stride <= 0 || page.height <= 0) {
    nanopdf_jbig2_image_destroy(context, &page);
    jbig2_segment_table_destroy(context, &segment_table);
    return 0;
  }

  *out_page = page;
  jbig2_segment_table_destroy(context, &segment_table);
  return 1;
}
