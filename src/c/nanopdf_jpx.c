#include "nanopdf_jpx.h"

#include <string.h>

static const uint16_t k_jpx_qe_table[] = {
    0x5601, 0x3401, 0x1801, 0x0ac1, 0x0521, 0x0221, 0x5601, 0x5401,
    0x4801, 0x3801, 0x3001, 0x2401, 0x1c01, 0x1601, 0x5601, 0x5401,
    0x5101, 0x4801, 0x3801, 0x3401, 0x3001, 0x2801, 0x2401, 0x2201,
    0x1c01, 0x1801, 0x1601, 0x1401, 0x1201, 0x1101, 0x0ac1, 0x09c1,
    0x08a1, 0x0521, 0x0441, 0x02a1, 0x0221, 0x0141, 0x0111, 0x0085,
    0x0049, 0x0025, 0x0015, 0x0009, 0x0005, 0x0001, 0x5601};

static const uint8_t k_jpx_nmps_table[] = {
    1, 2, 3, 4, 5, 38, 7, 8, 9, 10, 11, 12, 13, 29, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 45, 46};

static const uint8_t k_jpx_nlps_table[] = {
    1, 6, 9, 12, 29, 33, 6, 14, 14, 14, 17, 18, 20, 21, 14, 14,
    15, 16, 17, 18, 19, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 46};

static const uint8_t k_jpx_switch_table[] = {
    1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static const float k_jpx_lift_97_alpha = -1.586134342f;
static const float k_jpx_lift_97_beta = -0.052980118f;
static const float k_jpx_lift_97_gamma = 0.882911075f;
static const float k_jpx_lift_97_delta = 0.443506852f;
static const float k_jpx_lift_97_k = 1.230174105f;

static uint16_t nanopdf_jpx_read_be16_at(const uint8_t* data) {
  return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t nanopdf_jpx_read_be32_at(const uint8_t* data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static int nanopdf_jpx_ceildiv_i32(int a, int b) {
  if (b <= 0) {
    return 0;
  }
  return (a + b - 1) / b;
}

typedef struct nanopdf_jpx_packet_header_reader {
  const uint8_t* data;
  size_t size;
  size_t pos;
  int bit_pos;
  uint8_t current_byte;
  int max_bits;
  int overflow;
} nanopdf_jpx_packet_header_reader;

static void jpx_packet_header_reader_init(
    nanopdf_jpx_packet_header_reader* reader,
    const uint8_t* data,
    size_t size,
    size_t offset) {
  reader->data = data;
  reader->size = size;
  reader->pos = offset;
  reader->bit_pos = 8;
  reader->current_byte = 0;
  reader->max_bits = 8;
  reader->overflow = 0;
}

static int jpx_packet_header_read_bit(nanopdf_jpx_packet_header_reader* reader) {
  int bit;
  if (reader->bit_pos >= reader->max_bits) {
    if (reader->pos >= reader->size) {
      reader->overflow = 1;
      return 0;
    }
    reader->current_byte = reader->data[reader->pos++];
    reader->bit_pos = 0;
    reader->max_bits =
        (reader->pos >= 2u && reader->data[reader->pos - 2u] == 0xffu) ? 7 : 8;
  }
  bit = (reader->current_byte >> (reader->max_bits - 1 - reader->bit_pos)) & 1;
  reader->bit_pos++;
  return bit;
}

static int jpx_packet_header_read_bits(nanopdf_jpx_packet_header_reader* reader, int bits) {
  int value = 0;
  int i;
  for (i = 0; i < bits; ++i) {
    value = (value << 1) | jpx_packet_header_read_bit(reader);
  }
  return value;
}

static void jpx_packet_header_align(nanopdf_jpx_packet_header_reader* reader) {
  if (reader->bit_pos > 0 && reader->bit_pos < reader->max_bits) {
    reader->bit_pos = reader->max_bits;
  }
}

void nanopdf_jpx_bit_reader_init(
    nanopdf_jpx_bit_reader* reader,
    const uint8_t* data,
    size_t size) {
  if (!reader) {
    return;
  }
  reader->data = data;
  reader->size = size;
  reader->pos = 0;
  reader->bit_pos = 0;
}

uint32_t nanopdf_jpx_bit_reader_read_bits(nanopdf_jpx_bit_reader* reader, int bits) {
  uint32_t value = 0;
  int i;
  if (!reader || bits <= 0 || bits > 32) {
    return 0;
  }
  for (i = 0; i < bits; ++i) {
    value = (value << 1) | (uint32_t)nanopdf_jpx_bit_reader_read_bit(reader);
  }
  return value;
}

int nanopdf_jpx_bit_reader_read_bit(nanopdf_jpx_bit_reader* reader) {
  int bit;
  if (!reader || reader->pos >= reader->size) {
    return 0;
  }
  bit = (reader->data[reader->pos] >> (7 - reader->bit_pos)) & 1;
  reader->bit_pos++;
  if (reader->bit_pos >= 8) {
    reader->bit_pos = 0;
    reader->pos++;
    if (reader->pos > 0 && reader->data[reader->pos - 1] == 0xff &&
        reader->pos < reader->size && (reader->data[reader->pos] & 0x80) == 0) {
      reader->bit_pos = 1;
    }
  }
  return bit;
}

void nanopdf_jpx_bit_reader_align_byte(nanopdf_jpx_bit_reader* reader) {
  if (!reader) {
    return;
  }
  if (reader->bit_pos > 0) {
    reader->bit_pos = 0;
    reader->pos++;
  }
}

int nanopdf_jpx_bit_reader_eof(const nanopdf_jpx_bit_reader* reader) {
  return !reader || reader->pos >= reader->size;
}

size_t nanopdf_jpx_bit_reader_position(const nanopdf_jpx_bit_reader* reader) {
  return reader ? reader->pos : 0;
}

void nanopdf_jpx_bit_reader_seek(nanopdf_jpx_bit_reader* reader, size_t pos) {
  if (!reader) {
    return;
  }
  reader->pos = pos > reader->size ? reader->size : pos;
  reader->bit_pos = 0;
}

void nanopdf_jpx_header_init(nanopdf_jpx_header* header) {
  if (!header) {
    return;
  }
  memset(header, 0, sizeof(*header));
  header->cod.progression_order = NANOPDF_JPX_PROGRESSION_LRCP;
  header->cod.num_layers = 1;
  header->cod.num_decomp_levels = 5;
  header->cod.codeblock_width = 4;
  header->cod.codeblock_height = 4;
  header->cod.wavelet = NANOPDF_JPX_WAVELET_REVERSIBLE_5_3;
}

static void jpx_cod_params_destroy(nanopdf_context* context, nanopdf_jpx_cod_params* cod) {
  if (!context || !cod) {
    return;
  }
  nanopdf__allocator_free(&context->allocator, cod->precinct_sizes);
  memset(cod, 0, sizeof(*cod));
}

static void jpx_qcd_params_destroy(nanopdf_context* context, nanopdf_jpx_qcd_params* qcd) {
  if (!context || !qcd) {
    return;
  }
  nanopdf__allocator_free(&context->allocator, qcd->step_sizes);
  memset(qcd, 0, sizeof(*qcd));
}

static void jpx_indexed_data_array_destroy(
    nanopdf_context* context,
    nanopdf_jpx_indexed_data* values,
    size_t count) {
  size_t i;
  if (!context || !values) {
    return;
  }
  for (i = 0; i < count; ++i) {
    nanopdf__allocator_free(&context->allocator, values[i].data);
  }
}

static void jpx_packet_length_array_destroy(
    nanopdf_context* context,
    nanopdf_jpx_packet_length_marker* values,
    size_t count) {
  size_t i;
  if (!context || !values) {
    return;
  }
  for (i = 0; i < count; ++i) {
    nanopdf__allocator_free(&context->allocator, values[i].lengths);
  }
}

static void jpx_tlm_array_destroy(
    nanopdf_context* context,
    nanopdf_jpx_tlm_marker* values,
    size_t count) {
  size_t i;
  if (!context || !values) {
    return;
  }
  for (i = 0; i < count; ++i) {
    nanopdf__allocator_free(&context->allocator, values[i].data);
  }
}

void nanopdf_jpx_header_destroy(nanopdf_context* context, nanopdf_jpx_header* header) {
  size_t i;
  if (!header) {
    return;
  }
  nanopdf__allocator_free(&context->allocator, header->siz.components);
  jpx_cod_params_destroy(context, &header->cod);
  jpx_qcd_params_destroy(context, &header->qcd);
  if (header->component_cod) {
    for (i = 0; i < header->siz.num_components; ++i) {
      jpx_cod_params_destroy(context, &header->component_cod[i]);
    }
  }
  if (header->component_qcd) {
    for (i = 0; i < header->siz.num_components; ++i) {
      jpx_qcd_params_destroy(context, &header->component_qcd[i]);
    }
  }
  nanopdf__allocator_free(&context->allocator, header->component_cod);
  nanopdf__allocator_free(&context->allocator, header->has_component_cod);
  nanopdf__allocator_free(&context->allocator, header->component_qcd);
  nanopdf__allocator_free(&context->allocator, header->has_component_qcd);
  nanopdf__allocator_free(&context->allocator, header->component_rgn);
  nanopdf__allocator_free(&context->allocator, header->has_component_rgn);
  nanopdf__allocator_free(&context->allocator, header->poc_entries);
  nanopdf__allocator_free(&context->allocator, header->crg_offsets);
  if (header->comments) {
    for (i = 0; i < header->comment_count; ++i) {
      nanopdf__allocator_free(&context->allocator, header->comments[i].data);
    }
  }
  nanopdf__allocator_free(&context->allocator, header->comments);
  jpx_tlm_array_destroy(context, header->tlm_markers, header->tlm_marker_count);
  nanopdf__allocator_free(&context->allocator, header->tlm_markers);
  jpx_packet_length_array_destroy(context, header->plm_markers, header->plm_marker_count);
  nanopdf__allocator_free(&context->allocator, header->plm_markers);
  jpx_packet_length_array_destroy(context, header->plt_markers, header->plt_marker_count);
  nanopdf__allocator_free(&context->allocator, header->plt_markers);
  jpx_indexed_data_array_destroy(context, header->ppm_markers, header->ppm_marker_count);
  nanopdf__allocator_free(&context->allocator, header->ppm_markers);
  jpx_indexed_data_array_destroy(context, header->ppt_markers, header->ppt_marker_count);
  nanopdf__allocator_free(&context->allocator, header->ppt_markers);
  nanopdf_jpx_header_init(header);
}

int nanopdf_jpx_extract_codestream(
    const uint8_t* data,
    size_t size,
    const uint8_t** out_data,
    size_t* out_size) {
  size_t pos = 0;
  if (!data || !out_data || !out_size || size < 2) {
    return 0;
  }
  *out_data = data;
  *out_size = size;

  if (size >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00) {
    while (pos + 8 <= size) {
      uint32_t box_len = nanopdf_jpx_read_be32_at(data + pos);
      uint32_t box_type = nanopdf_jpx_read_be32_at(data + pos + 4);
      size_t header_size = 8;
      uint64_t actual_len = box_len;

      if (box_len == 1) {
        if (pos + 16 > size) {
          return 0;
        }
        actual_len = ((uint64_t)nanopdf_jpx_read_be32_at(data + pos + 8) << 32) |
                     (uint64_t)nanopdf_jpx_read_be32_at(data + pos + 12);
        header_size = 16;
      } else if (box_len == 0) {
        actual_len = (uint64_t)(size - pos);
      }

      if (actual_len < header_size || actual_len > (uint64_t)(size - pos)) {
        return 0;
      }
      if (box_type == 0x6a703263u) {
        *out_data = data + pos + header_size;
        *out_size = (size_t)(actual_len - header_size);
        return 1;
      }
      pos += (size_t)actual_len;
    }
  }

  return data[0] == 0xff && data[1] == 0x4f;
}

static int nanopdf_jpx_marker_has_segment_length(uint16_t marker) {
  return marker != NANOPDF_JPX_SOC && marker != NANOPDF_JPX_SOT &&
         marker != NANOPDF_JPX_SOD && marker != NANOPDF_JPX_EOC &&
         marker != NANOPDF_JPX_SOP && marker != NANOPDF_JPX_EPH;
}

static uint32_t nanopdf_jpx_ceil_div_u32(uint32_t value, uint32_t divisor) {
  if (divisor == 0u) {
    return 0u;
  }
  return value / divisor + (uint32_t)((value % divisor) != 0u);
}

uint32_t nanopdf_jpx_image_width(const nanopdf_jpx_header* header) {
  if (!header || header->siz.width <= header->siz.x_offset) {
    return 0u;
  }
  return header->siz.width - header->siz.x_offset;
}

uint32_t nanopdf_jpx_image_height(const nanopdf_jpx_header* header) {
  if (!header || header->siz.height <= header->siz.y_offset) {
    return 0u;
  }
  return header->siz.height - header->siz.y_offset;
}

uint32_t nanopdf_jpx_component_width(
    const nanopdf_jpx_header* header,
    size_t component_index) {
  uint32_t x_sep = 1u;
  uint32_t component_end;
  uint32_t component_start;
  if (!header || component_index >= header->siz.num_components ||
      !header->siz.components || header->siz.width <= header->siz.x_offset) {
    return 0u;
  }
  x_sep = header->siz.components[component_index].x_separation;
  if (x_sep == 0u) {
    x_sep = 1u;
  }
  component_start = nanopdf_jpx_ceil_div_u32(header->siz.x_offset, x_sep);
  component_end = nanopdf_jpx_ceil_div_u32(header->siz.width, x_sep);
  return component_end > component_start ? component_end - component_start : 0u;
}

uint32_t nanopdf_jpx_component_height(
    const nanopdf_jpx_header* header,
    size_t component_index) {
  uint32_t y_sep = 1u;
  uint32_t component_end;
  uint32_t component_start;
  if (!header || component_index >= header->siz.num_components ||
      !header->siz.components || header->siz.height <= header->siz.y_offset) {
    return 0u;
  }
  y_sep = header->siz.components[component_index].y_separation;
  if (y_sep == 0u) {
    y_sep = 1u;
  }
  component_start = nanopdf_jpx_ceil_div_u32(header->siz.y_offset, y_sep);
  component_end = nanopdf_jpx_ceil_div_u32(header->siz.height, y_sep);
  return component_end > component_start ? component_end - component_start : 0u;
}

static int nanopdf_jpx_parse_siz(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_header* header) {
  uint16_t component_count;
  size_t i;
  if (length < 38) {
    return 0;
  }
  header->siz.rsiz = (uint16_t)nanopdf_jpx_bit_reader_read_bits(reader, 16);
  header->siz.width = nanopdf_jpx_bit_reader_read_bits(reader, 32);
  header->siz.height = nanopdf_jpx_bit_reader_read_bits(reader, 32);
  header->siz.x_offset = nanopdf_jpx_bit_reader_read_bits(reader, 32);
  header->siz.y_offset = nanopdf_jpx_bit_reader_read_bits(reader, 32);
  header->siz.tile_width = nanopdf_jpx_bit_reader_read_bits(reader, 32);
  header->siz.tile_height = nanopdf_jpx_bit_reader_read_bits(reader, 32);
  header->siz.tile_x_offset = nanopdf_jpx_bit_reader_read_bits(reader, 32);
  header->siz.tile_y_offset = nanopdf_jpx_bit_reader_read_bits(reader, 32);
  component_count = (uint16_t)nanopdf_jpx_bit_reader_read_bits(reader, 16);
  if (component_count == 0 || length != (uint16_t)(38u + 3u * component_count)) {
    return 0;
  }
  nanopdf__allocator_free(&context->allocator, header->siz.components);
  header->siz.components = NULL;
  header->siz.num_components = 0;
  header->siz.components = (nanopdf_jpx_component*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(nanopdf_jpx_component) * component_count);
  header->component_cod = (nanopdf_jpx_cod_params*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(nanopdf_jpx_cod_params) * component_count);
  header->has_component_cod =
      (uint8_t*)nanopdf__allocator_alloc(&context->allocator, component_count);
  header->component_qcd = (nanopdf_jpx_qcd_params*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(nanopdf_jpx_qcd_params) * component_count);
  header->has_component_qcd =
      (uint8_t*)nanopdf__allocator_alloc(&context->allocator, component_count);
  header->component_rgn = (nanopdf_jpx_rgn_params*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(nanopdf_jpx_rgn_params) * component_count);
  header->has_component_rgn =
      (uint8_t*)nanopdf__allocator_alloc(&context->allocator, component_count);
  if (!header->siz.components || !header->component_cod || !header->has_component_cod ||
      !header->component_qcd || !header->has_component_qcd ||
      !header->component_rgn || !header->has_component_rgn) {
    return 0;
  }
  memset(header->siz.components, 0, sizeof(nanopdf_jpx_component) * component_count);
  memset(header->component_cod, 0, sizeof(nanopdf_jpx_cod_params) * component_count);
  memset(header->has_component_cod, 0, component_count);
  memset(header->component_qcd, 0, sizeof(nanopdf_jpx_qcd_params) * component_count);
  memset(header->has_component_qcd, 0, component_count);
  memset(header->component_rgn, 0, sizeof(nanopdf_jpx_rgn_params) * component_count);
  memset(header->has_component_rgn, 0, component_count);
  header->siz.num_components = component_count;
  for (i = 0; i < component_count; ++i) {
    uint8_t ssiz = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
    header->siz.components[i].is_signed = (uint8_t)((ssiz & 0x80u) != 0u);
    header->siz.components[i].bit_depth = (uint8_t)((ssiz & 0x7fu) + 1u);
    header->siz.components[i].x_separation =
        (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
    header->siz.components[i].y_separation =
        (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  }
  return header->siz.width > header->siz.x_offset &&
         header->siz.height > header->siz.y_offset;
}

static int nanopdf_jpx_parse_cod(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_header* header) {
  uint8_t scod;
  size_t precinct_count;
  size_t i;
  if (length < 12) {
    return 0;
  }
  scod = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  header->cod.use_sop = (uint8_t)((scod & 0x02u) != 0u);
  header->cod.use_eph = (uint8_t)((scod & 0x04u) != 0u);
  header->cod.progression_order =
      (nanopdf_jpx_progression_order)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  header->cod.num_layers = (uint16_t)nanopdf_jpx_bit_reader_read_bits(reader, 16);
  header->cod.mct = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  header->cod.num_decomp_levels = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  header->cod.codeblock_width = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  header->cod.codeblock_height = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  header->cod.codeblock_style = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  header->cod.wavelet =
      nanopdf_jpx_bit_reader_read_bits(reader, 8) == 1u
          ? NANOPDF_JPX_WAVELET_REVERSIBLE_5_3
          : NANOPDF_JPX_WAVELET_IRREVERSIBLE_9_7;

  nanopdf__allocator_free(&context->allocator, header->cod.precinct_sizes);
  header->cod.precinct_sizes = NULL;
  header->cod.precinct_size_count = 0;

  if ((scod & 0x01u) != 0u) {
    precinct_count = (size_t)header->cod.num_decomp_levels + 1u;
    if (length != (uint16_t)(12u + precinct_count)) {
      return 0;
    }
    header->cod.precinct_sizes = (uint8_t*)nanopdf__allocator_alloc(
        &context->allocator, precinct_count);
    if (!header->cod.precinct_sizes) {
      return 0;
    }
    header->cod.precinct_size_count = precinct_count;
    for (i = 0; i < precinct_count; ++i) {
      header->cod.precinct_sizes[i] = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
    }
  } else if (length != 12) {
    return 0;
  }

  return header->cod.num_layers > 0;
}

static int nanopdf_jpx_parse_coc(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_header* header) {
  uint32_t component_index;
  uint8_t scoc;
  size_t component_index_size;
  size_t precinct_count;
  size_t i;
  nanopdf_jpx_cod_params* cod;
  if (!context || !reader || !header || header->siz.num_components == 0) {
    return 0;
  }
  component_index_size = header->siz.num_components < 257u ? 1u : 2u;
  if (length < 2u + component_index_size + 6u) {
    return 0;
  }
  component_index = component_index_size == 1u
                        ? nanopdf_jpx_bit_reader_read_bits(reader, 8)
                        : nanopdf_jpx_bit_reader_read_bits(reader, 16);
  if (component_index >= header->siz.num_components || !header->component_cod ||
      !header->has_component_cod) {
    return 0;
  }
  cod = &header->component_cod[component_index];
  jpx_cod_params_destroy(context, cod);
  *cod = header->cod;
  cod->precinct_sizes = NULL;
  cod->precinct_size_count = 0;

  scoc = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  cod->num_decomp_levels = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  cod->codeblock_width = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  cod->codeblock_height = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  cod->codeblock_style = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  cod->wavelet =
      nanopdf_jpx_bit_reader_read_bits(reader, 8) == 1u
          ? NANOPDF_JPX_WAVELET_REVERSIBLE_5_3
          : NANOPDF_JPX_WAVELET_IRREVERSIBLE_9_7;
  if ((scoc & 0x01u) != 0u) {
    precinct_count = (size_t)cod->num_decomp_levels + 1u;
    if (length != (uint16_t)(2u + component_index_size + 6u + precinct_count)) {
      jpx_cod_params_destroy(context, cod);
      return 0;
    }
    cod->precinct_sizes = (uint8_t*)nanopdf__allocator_alloc(
        &context->allocator, precinct_count);
    if (!cod->precinct_sizes) {
      jpx_cod_params_destroy(context, cod);
      return 0;
    }
    cod->precinct_size_count = precinct_count;
    for (i = 0; i < precinct_count; ++i) {
      cod->precinct_sizes[i] = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
    }
  } else if (length != (uint16_t)(2u + component_index_size + 6u)) {
    jpx_cod_params_destroy(context, cod);
    return 0;
  }
  header->has_component_cod[component_index] = 1u;
  return 1;
}

static int jpx_parse_qcd_payload(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t payload_length,
    nanopdf_jpx_qcd_params* qcd) {
  uint8_t sqcd;
  int remaining;
  size_t step_count;
  size_t i;
  if (!context || !reader || !qcd || payload_length < 1) {
    return 0;
  }
  sqcd = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  qcd->quant_style = (uint8_t)(sqcd & 0x1fu);
  qcd->num_guard_bits = (uint8_t)((sqcd >> 5) & 0x07u);
  remaining = (int)payload_length - 1;

  nanopdf__allocator_free(&context->allocator, qcd->step_sizes);
  qcd->step_sizes = NULL;
  qcd->step_size_count = 0;

  if (qcd->quant_style == 0) {
    step_count = (size_t)remaining;
    qcd->step_sizes = (nanopdf_jpx_qcd_step_size*)nanopdf__allocator_alloc(
        &context->allocator, sizeof(nanopdf_jpx_qcd_step_size) * step_count);
    if (step_count > 0 && !qcd->step_sizes) {
      return 0;
    }
    qcd->step_size_count = step_count;
    for (i = 0; i < step_count; ++i) {
      uint8_t value = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
      qcd->step_sizes[i].exponent = (uint8_t)(value >> 3);
      qcd->step_sizes[i].mantissa = 0;
    }
  } else {
    if ((remaining & 1) != 0) {
      return 0;
    }
    step_count = qcd->quant_style == 1 ? 1u : (size_t)remaining / 2u;
    if (remaining == 0) {
      return 0;
    }
    qcd->step_sizes = (nanopdf_jpx_qcd_step_size*)nanopdf__allocator_alloc(
        &context->allocator, sizeof(nanopdf_jpx_qcd_step_size) * step_count);
    if (!qcd->step_sizes) {
      return 0;
    }
    qcd->step_size_count = step_count;
    for (i = 0; i < step_count; ++i) {
      uint16_t value = (uint16_t)nanopdf_jpx_bit_reader_read_bits(reader, 16);
      qcd->step_sizes[i].exponent = (uint8_t)((value >> 11) & 0x1fu);
      qcd->step_sizes[i].mantissa = (uint16_t)(value & 0x07ffu);
    }
    if (qcd->quant_style == 1 && remaining > 2) {
      nanopdf_jpx_bit_reader_seek(reader, nanopdf_jpx_bit_reader_position(reader) + (size_t)remaining - 2u);
    }
  }
  return 1;
}

static int nanopdf_jpx_parse_qcd(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_header* header) {
  if (length < 3) {
    return 0;
  }
  return jpx_parse_qcd_payload(context, reader, (uint16_t)(length - 2u), &header->qcd);
}

static int nanopdf_jpx_parse_qcc(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_header* header) {
  uint32_t component_index;
  size_t component_index_size;
  if (!context || !reader || !header || header->siz.num_components == 0) {
    return 0;
  }
  component_index_size = header->siz.num_components < 257u ? 1u : 2u;
  if (length < 2u + component_index_size + 1u) {
    return 0;
  }
  component_index = component_index_size == 1u
                        ? nanopdf_jpx_bit_reader_read_bits(reader, 8)
                        : nanopdf_jpx_bit_reader_read_bits(reader, 16);
  if (component_index >= header->siz.num_components || !header->component_qcd ||
      !header->has_component_qcd) {
    return 0;
  }
  if (!jpx_parse_qcd_payload(
          context,
          reader,
          (uint16_t)(length - 2u - component_index_size),
          &header->component_qcd[component_index])) {
    return 0;
  }
  header->has_component_qcd[component_index] = 1u;
  return 1;
}

static int nanopdf_jpx_parse_rgn(
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_header* header) {
  uint32_t component_index;
  size_t component_index_size;
  if (!reader || !header || header->siz.num_components == 0) {
    return 0;
  }
  component_index_size = header->siz.num_components < 257u ? 1u : 2u;
  if (length != (uint16_t)(2u + component_index_size + 2u)) {
    return 0;
  }
  component_index = component_index_size == 1u
                        ? nanopdf_jpx_bit_reader_read_bits(reader, 8)
                        : nanopdf_jpx_bit_reader_read_bits(reader, 16);
  if (component_index >= header->siz.num_components || !header->component_rgn ||
      !header->has_component_rgn) {
    return 0;
  }
  header->component_rgn[component_index].style =
      (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  header->component_rgn[component_index].shift =
      (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  header->has_component_rgn[component_index] = 1u;
  return 1;
}

static int nanopdf_jpx_parse_poc(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_header* header) {
  size_t component_index_size;
  size_t record_size;
  size_t payload_size;
  size_t entry_count;
  size_t i;
  nanopdf_jpx_poc_entry* entries;
  if (!context || !reader || !header || header->siz.num_components == 0 || length < 2u) {
    return 0;
  }
  component_index_size = header->siz.num_components < 257u ? 1u : 2u;
  record_size = 1u + component_index_size + 2u + 1u + component_index_size + 1u;
  payload_size = (size_t)length - 2u;
  if (record_size == 0u || payload_size == 0u || (payload_size % record_size) != 0u) {
    return 0;
  }
  entry_count = payload_size / record_size;
  entries = (nanopdf_jpx_poc_entry*)nanopdf__allocator_realloc(
      &context->allocator,
      header->poc_entries,
      sizeof(nanopdf_jpx_poc_entry) * (header->poc_entry_count + entry_count));
  if (!entries) {
    return 0;
  }
  header->poc_entries = entries;
  memset(
      header->poc_entries + header->poc_entry_count,
      0,
      sizeof(nanopdf_jpx_poc_entry) * entry_count);
  for (i = 0; i < entry_count; ++i) {
    nanopdf_jpx_poc_entry* entry = &header->poc_entries[header->poc_entry_count + i];
    entry->resolution_start = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
    entry->component_start =
        component_index_size == 1u ? nanopdf_jpx_bit_reader_read_bits(reader, 8)
                                   : nanopdf_jpx_bit_reader_read_bits(reader, 16);
    entry->layer_end = (uint16_t)nanopdf_jpx_bit_reader_read_bits(reader, 16);
    entry->resolution_end = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
    entry->component_end =
        component_index_size == 1u ? nanopdf_jpx_bit_reader_read_bits(reader, 8)
                                   : nanopdf_jpx_bit_reader_read_bits(reader, 16);
    entry->progression_order =
        (nanopdf_jpx_progression_order)nanopdf_jpx_bit_reader_read_bits(reader, 8);
    if (entry->component_start >= header->siz.num_components ||
        entry->component_end > header->siz.num_components ||
        entry->component_start >= entry->component_end ||
        entry->layer_end == 0u ||
        entry->progression_order > NANOPDF_JPX_PROGRESSION_CPRL) {
      return 0;
    }
  }
  header->poc_entry_count += entry_count;
  return 1;
}

static int nanopdf_jpx_parse_crg(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_header* header) {
  size_t expected_length;
  size_t i;
  if (!context || !reader || !header || header->siz.num_components == 0) {
    return 0;
  }
  expected_length = 2u + 4u * (size_t)header->siz.num_components;
  if ((size_t)length != expected_length) {
    return 0;
  }
  nanopdf__allocator_free(&context->allocator, header->crg_offsets);
  header->crg_offsets = (nanopdf_jpx_crg_offset*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(nanopdf_jpx_crg_offset) * header->siz.num_components);
  if (!header->crg_offsets) {
    header->crg_offset_count = 0;
    return 0;
  }
  header->crg_offset_count = header->siz.num_components;
  for (i = 0; i < header->crg_offset_count; ++i) {
    header->crg_offsets[i].x = (uint16_t)nanopdf_jpx_bit_reader_read_bits(reader, 16);
    header->crg_offsets[i].y = (uint16_t)nanopdf_jpx_bit_reader_read_bits(reader, 16);
  }
  return 1;
}

static int nanopdf_jpx_parse_com(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_header* header) {
  nanopdf_jpx_comment* comments;
  nanopdf_jpx_comment* comment;
  size_t payload_size;
  size_t i;
  if (!context || !reader || !header || length < 4u) {
    return 0;
  }
  comments = (nanopdf_jpx_comment*)nanopdf__allocator_realloc(
      &context->allocator,
      header->comments,
      sizeof(nanopdf_jpx_comment) * (header->comment_count + 1u));
  if (!comments) {
    return 0;
  }
  header->comments = comments;
  comment = &header->comments[header->comment_count];
  memset(comment, 0, sizeof(*comment));
  comment->registration = (uint16_t)nanopdf_jpx_bit_reader_read_bits(reader, 16);
  payload_size = (size_t)length - 4u;
  if (payload_size > 0u) {
    comment->data = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, payload_size);
    if (!comment->data) {
      return 0;
    }
    comment->size = payload_size;
    for (i = 0; i < payload_size; ++i) {
      comment->data[i] = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
    }
  }
  header->comment_count++;
  return 1;
}

static int nanopdf_jpx_parse_indexed_data(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_indexed_data** out_values,
    size_t* out_count) {
  nanopdf_jpx_indexed_data* values;
  nanopdf_jpx_indexed_data* value;
  size_t payload_size;
  size_t i;
  if (!context || !reader || !out_values || !out_count || length < 3u) {
    return 0;
  }
  values = (nanopdf_jpx_indexed_data*)nanopdf__allocator_realloc(
      &context->allocator,
      *out_values,
      sizeof(nanopdf_jpx_indexed_data) * (*out_count + 1u));
  if (!values) {
    return 0;
  }
  *out_values = values;
  value = &(*out_values)[*out_count];
  memset(value, 0, sizeof(*value));
  value->index = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  payload_size = (size_t)length - 3u;
  if (payload_size > 0u) {
    value->data = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, payload_size);
    if (!value->data) {
      return 0;
    }
    value->size = payload_size;
    for (i = 0; i < payload_size; ++i) {
      value->data[i] = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
    }
  }
  (*out_count)++;
  return 1;
}

static int nanopdf_jpx_parse_packet_lengths(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_packet_length_marker** out_values,
    size_t* out_count) {
  nanopdf_jpx_packet_length_marker* values;
  nanopdf_jpx_packet_length_marker* marker;
  size_t payload_size;
  size_t consumed = 0;
  if (!context || !reader || !out_values || !out_count || length < 3u) {
    return 0;
  }
  values = (nanopdf_jpx_packet_length_marker*)nanopdf__allocator_realloc(
      &context->allocator,
      *out_values,
      sizeof(nanopdf_jpx_packet_length_marker) * (*out_count + 1u));
  if (!values) {
    return 0;
  }
  *out_values = values;
  marker = &(*out_values)[*out_count];
  memset(marker, 0, sizeof(*marker));
  marker->index = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  payload_size = (size_t)length - 3u;
  while (consumed < payload_size) {
    uint32_t value = 0;
    uint8_t byte_value = 0;
    int complete = 0;
    do {
      if (consumed >= payload_size) {
        goto fail;
      }
      byte_value = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
      consumed++;
      if (value > (UINT32_MAX >> 7)) {
        goto fail;
      }
      value = (value << 7) | (uint32_t)(byte_value & 0x7fu);
      complete = (byte_value & 0x80u) == 0u;
    } while (!complete);
    {
      uint32_t* lengths = (uint32_t*)nanopdf__allocator_realloc(
          &context->allocator,
          marker->lengths,
          sizeof(uint32_t) * (marker->length_count + 1u));
      if (!lengths) {
        goto fail;
      }
      marker->lengths = lengths;
      marker->lengths[marker->length_count] = value;
      marker->length_count++;
    }
  }
  (*out_count)++;
  return 1;

fail:
  nanopdf__allocator_free(&context->allocator, marker->lengths);
  memset(marker, 0, sizeof(*marker));
  return 0;
}

static int nanopdf_jpx_parse_tlm(
    nanopdf_context* context,
    nanopdf_jpx_bit_reader* reader,
    uint16_t length,
    nanopdf_jpx_header* header) {
  nanopdf_jpx_tlm_marker* values;
  nanopdf_jpx_tlm_marker* marker;
  size_t payload_size;
  size_t i;
  if (!context || !reader || !header || length < 4u) {
    return 0;
  }
  values = (nanopdf_jpx_tlm_marker*)nanopdf__allocator_realloc(
      &context->allocator,
      header->tlm_markers,
      sizeof(nanopdf_jpx_tlm_marker) * (header->tlm_marker_count + 1u));
  if (!values) {
    return 0;
  }
  header->tlm_markers = values;
  marker = &header->tlm_markers[header->tlm_marker_count];
  memset(marker, 0, sizeof(*marker));
  marker->index = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  marker->style = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
  payload_size = (size_t)length - 4u;
  if (payload_size > 0u) {
    marker->data = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, payload_size);
    if (!marker->data) {
      return 0;
    }
    marker->size = payload_size;
    for (i = 0; i < payload_size; ++i) {
      marker->data[i] = (uint8_t)nanopdf_jpx_bit_reader_read_bits(reader, 8);
    }
  }
  header->tlm_marker_count++;
  return 1;
}

int nanopdf_jpx_parse_header(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    nanopdf_jpx_header* header) {
  const uint8_t* codestream = NULL;
  size_t codestream_size = 0;
  nanopdf_jpx_bit_reader reader;
  size_t pos;
  int saw_siz = 0;

  if (!context || !data || !header) {
    return 0;
  }
  nanopdf_jpx_header_destroy(context, header);

  if (!nanopdf_jpx_extract_codestream(data, size, &codestream, &codestream_size) ||
      codestream_size < 2 || codestream[0] != 0xff || codestream[1] != 0x4f) {
    return 0;
  }

  if (nanopdf_jpx_read_be16_at(codestream) != NANOPDF_JPX_SOC) {
    return 0;
  }

  pos = 2u;
  while (pos + 2u <= codestream_size) {
    size_t marker_pos = pos;
    uint16_t marker;
    uint16_t length;
    size_t payload_start;
    size_t next_marker_pos;

    marker = nanopdf_jpx_read_be16_at(codestream + pos);
    pos += 2u;
    if (marker == NANOPDF_JPX_SOT) {
      header->first_tile_part_offset = marker_pos;
      return saw_siz;
    }
    if ((marker & 0xff00u) != 0xff00u || !nanopdf_jpx_marker_has_segment_length(marker)) {
      return 0;
    }
    if (pos + 2u > codestream_size) {
      return 0;
    }
    length = nanopdf_jpx_read_be16_at(codestream + pos);
    pos += 2u;
    if (length < 2) {
      return 0;
    }
    payload_start = pos;
    next_marker_pos = payload_start + (size_t)length - 2u;
    if (next_marker_pos > codestream_size) {
      return 0;
    }
    nanopdf_jpx_bit_reader_init(&reader, codestream + payload_start, (size_t)length - 2u);

    switch (marker) {
      case NANOPDF_JPX_SIZ:
        if (!nanopdf_jpx_parse_siz(context, &reader, length, header)) {
          return 0;
        }
        saw_siz = 1;
        break;
      case NANOPDF_JPX_COD:
        if (!nanopdf_jpx_parse_cod(context, &reader, length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_COC:
        if (!nanopdf_jpx_parse_coc(context, &reader, length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_QCD:
        if (!nanopdf_jpx_parse_qcd(context, &reader, length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_QCC:
        if (!nanopdf_jpx_parse_qcc(context, &reader, length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_RGN:
        if (!nanopdf_jpx_parse_rgn(&reader, length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_POC:
        if (!nanopdf_jpx_parse_poc(context, &reader, length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_CRG:
        if (!nanopdf_jpx_parse_crg(context, &reader, length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_COM:
        if (!nanopdf_jpx_parse_com(context, &reader, length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_TLM:
        if (!nanopdf_jpx_parse_tlm(context, &reader, length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_PLM:
        if (!nanopdf_jpx_parse_packet_lengths(
                context, &reader, length, &header->plm_markers, &header->plm_marker_count)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_PLT:
        if (!nanopdf_jpx_parse_packet_lengths(
                context, &reader, length, &header->plt_markers, &header->plt_marker_count)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_PPM:
        if (!nanopdf_jpx_parse_indexed_data(
                context, &reader, length, &header->ppm_markers, &header->ppm_marker_count)) {
          return 0;
        }
        break;
      default:
        break;
    }
    pos = next_marker_pos;
  }

  return saw_siz;
}

int nanopdf_jpx_get_info(
    const uint8_t* data,
    size_t size,
    uint32_t* out_width,
    uint32_t* out_height,
    uint16_t* out_num_components,
    uint8_t* out_bit_depth) {
  const uint8_t* codestream = NULL;
  size_t codestream_size = 0;
  size_t pos;

  if (!data || !out_width || !out_height || !out_num_components || !out_bit_depth) {
    return 0;
  }
  if (!nanopdf_jpx_extract_codestream(data, size, &codestream, &codestream_size) ||
      codestream_size < 40u || codestream[0] != 0xff || codestream[1] != 0x4f) {
    return 0;
  }
  pos = 2;
  while (pos + 4u <= codestream_size) {
    uint16_t marker = nanopdf_jpx_read_be16_at(codestream + pos);
    uint16_t length;
    if (marker == NANOPDF_JPX_SIZ) {
      if (pos + 40u > codestream_size) {
        return 0;
      }
      length = nanopdf_jpx_read_be16_at(codestream + pos + 2u);
      if (length < 43u || pos + 2u + (size_t)length > codestream_size) {
        return 0;
      }
      uint32_t xsiz = nanopdf_jpx_read_be32_at(codestream + pos + 6u);
      uint32_t ysiz = nanopdf_jpx_read_be32_at(codestream + pos + 10u);
      uint32_t xosiz = nanopdf_jpx_read_be32_at(codestream + pos + 14u);
      uint32_t yosiz = nanopdf_jpx_read_be32_at(codestream + pos + 18u);
      if (xsiz <= xosiz || ysiz <= yosiz) {
        return 0;
      }
      *out_width = xsiz - xosiz;
      *out_height = ysiz - yosiz;
      *out_num_components = nanopdf_jpx_read_be16_at(codestream + pos + 38u);
      if (*out_num_components == 0 ||
          length != (uint16_t)(38u + 3u * *out_num_components)) {
        return 0;
      }
      *out_bit_depth = (uint8_t)((codestream[pos + 40u] & 0x7fu) + 1u);
      return *out_width > 0 && *out_height > 0;
    }
    if ((marker & 0xff00u) != 0xff00u || !nanopdf_jpx_marker_has_segment_length(marker)) {
      return 0;
    }
    length = nanopdf_jpx_read_be16_at(codestream + pos + 2u);
    if (length < 2u || pos + 2u + (size_t)length > codestream_size) {
      return 0;
    }
    pos += 2u + (size_t)length;
  }
  return 0;
}

void nanopdf_jpx_mq_decoder_reset(nanopdf_jpx_mq_decoder* decoder) {
  if (!decoder) {
    return;
  }
  memset(decoder->cx_states, 0, sizeof(decoder->cx_states));
  memset(decoder->cx_mps, 0, sizeof(decoder->cx_mps));
  decoder->cx_states[17] = 3;
  decoder->cx_states[18] = 46;
}

static void nanopdf_jpx_mq_bytein(nanopdf_jpx_mq_decoder* decoder) {
  uint8_t b;
  if (decoder->pos >= decoder->size) {
    decoder->c += 0xffu;
    decoder->ct = 8;
    return;
  }
  b = decoder->data[decoder->pos];
  if (decoder->pos > 0 && decoder->data[decoder->pos - 1u] == 0xffu) {
    if (b > 0x8fu) {
      decoder->c += 0xffu;
      decoder->ct = 8;
    } else {
      decoder->pos++;
      decoder->c += (uint32_t)b << 9;
      decoder->ct = 7;
    }
  } else {
    decoder->pos++;
    decoder->c += (uint32_t)b << 8;
    decoder->ct = 8;
  }
}

void nanopdf_jpx_mq_decoder_init(
    nanopdf_jpx_mq_decoder* decoder,
    const uint8_t* data,
    size_t size) {
  if (!decoder) {
    return;
  }
  memset(decoder, 0, sizeof(*decoder));
  decoder->data = data;
  decoder->size = size;
  nanopdf_jpx_mq_decoder_reset(decoder);
  if (!data || size == 0) {
    decoder->a = 0x8000u;
    return;
  }
  decoder->c = (uint32_t)data[0] << 16;
  if (size > 1u) {
    decoder->c |= (uint32_t)data[1] << 8;
  }
  decoder->pos = 2u;
  nanopdf_jpx_mq_bytein(decoder);
  decoder->c <<= 7;
  decoder->ct -= 7;
  decoder->a = 0x8000u;
}

static void nanopdf_jpx_mq_renormd(nanopdf_jpx_mq_decoder* decoder) {
  do {
    if (decoder->ct == 0) {
      nanopdf_jpx_mq_bytein(decoder);
    }
    decoder->a = (uint16_t)(decoder->a << 1);
    decoder->c <<= 1;
    decoder->ct--;
  } while ((decoder->a & 0x8000u) == 0u);
}

static int nanopdf_jpx_mq_mps_exchange(nanopdf_jpx_mq_decoder* decoder, int context_index) {
  int decoded;
  uint16_t qe = k_jpx_qe_table[decoder->cx_states[context_index]];
  if (decoder->a < qe) {
    decoded = 1 - decoder->cx_mps[context_index];
    if (k_jpx_switch_table[decoder->cx_states[context_index]]) {
      decoder->cx_mps[context_index] = (uint8_t)(1 - decoder->cx_mps[context_index]);
    }
    decoder->cx_states[context_index] = k_jpx_nlps_table[decoder->cx_states[context_index]];
  } else {
    decoded = decoder->cx_mps[context_index];
    decoder->cx_states[context_index] = k_jpx_nmps_table[decoder->cx_states[context_index]];
  }
  nanopdf_jpx_mq_renormd(decoder);
  return decoded;
}

static int nanopdf_jpx_mq_lps_exchange(nanopdf_jpx_mq_decoder* decoder, int context_index) {
  int decoded;
  uint16_t qe = k_jpx_qe_table[decoder->cx_states[context_index]];
  if (decoder->a < qe) {
    decoder->a = qe;
    decoded = decoder->cx_mps[context_index];
    decoder->cx_states[context_index] = k_jpx_nmps_table[decoder->cx_states[context_index]];
  } else {
    decoder->a = qe;
    decoded = 1 - decoder->cx_mps[context_index];
    if (k_jpx_switch_table[decoder->cx_states[context_index]]) {
      decoder->cx_mps[context_index] = (uint8_t)(1 - decoder->cx_mps[context_index]);
    }
    decoder->cx_states[context_index] = k_jpx_nlps_table[decoder->cx_states[context_index]];
  }
  nanopdf_jpx_mq_renormd(decoder);
  return decoded;
}

int nanopdf_jpx_mq_decode(nanopdf_jpx_mq_decoder* decoder, int context_index) {
  uint16_t qe;
  if (!decoder || context_index < 0 || context_index >= 19) {
    return 0;
  }
  qe = k_jpx_qe_table[decoder->cx_states[context_index]];
  decoder->a = (uint16_t)(decoder->a - qe);
  if ((decoder->c >> 16) < decoder->a) {
    if ((decoder->a & 0x8000u) == 0u) {
      return nanopdf_jpx_mq_mps_exchange(decoder, context_index);
    }
    return decoder->cx_mps[context_index];
  }
  decoder->c -= (uint32_t)decoder->a << 16;
  return nanopdf_jpx_mq_lps_exchange(decoder, context_index);
}

void nanopdf_jpx_tag_tree_init(nanopdf_jpx_tag_tree* tree) {
  if (!tree) {
    return;
  }
  memset(tree, 0, sizeof(*tree));
}

void nanopdf_jpx_tag_tree_destroy(nanopdf_context* context, nanopdf_jpx_tag_tree* tree) {
  size_t i;
  if (!context || !tree) {
    return;
  }
  for (i = 0; i < tree->level_count; ++i) {
    nanopdf__allocator_free(&context->allocator, tree->levels[i].nodes);
  }
  nanopdf__allocator_free(&context->allocator, tree->levels);
  nanopdf_jpx_tag_tree_init(tree);
}

int nanopdf_jpx_tag_tree_build(
    nanopdf_context* context,
    nanopdf_jpx_tag_tree* tree,
    int width,
    int height) {
  int level_width;
  int level_height;
  size_t level_count = 0;
  size_t level_index;

  if (!context || !tree || width <= 0 || height <= 0) {
    return 0;
  }
  nanopdf_jpx_tag_tree_destroy(context, tree);

  level_width = width;
  level_height = height;
  while (level_width > 0 && level_height > 0) {
    level_count++;
    if (level_width == 1 && level_height == 1) {
      break;
    }
    level_width = nanopdf_jpx_ceildiv_i32(level_width, 2);
    level_height = nanopdf_jpx_ceildiv_i32(level_height, 2);
  }

  tree->levels = (nanopdf_jpx_tag_tree_level*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(nanopdf_jpx_tag_tree_level) * level_count);
  if (!tree->levels) {
    return 0;
  }
  memset(tree->levels, 0, sizeof(nanopdf_jpx_tag_tree_level) * level_count);
  tree->width = width;
  tree->height = height;
  tree->level_count = level_count;

  level_width = width;
  level_height = height;
  for (level_index = 0; level_index < level_count; ++level_index) {
    size_t node_count = (size_t)level_width * (size_t)level_height;
    tree->levels[level_index].width = level_width;
    tree->levels[level_index].height = level_height;
    tree->levels[level_index].nodes =
        (nanopdf_jpx_tag_tree_node*)nanopdf__allocator_alloc(
            &context->allocator, sizeof(nanopdf_jpx_tag_tree_node) * node_count);
    if (!tree->levels[level_index].nodes) {
      nanopdf_jpx_tag_tree_destroy(context, tree);
      return 0;
    }
    memset(tree->levels[level_index].nodes, 0,
           sizeof(nanopdf_jpx_tag_tree_node) * node_count);
    if (level_width == 1 && level_height == 1) {
      break;
    }
    level_width = nanopdf_jpx_ceildiv_i32(level_width, 2);
    level_height = nanopdf_jpx_ceildiv_i32(level_height, 2);
  }
  return 1;
}

int nanopdf_jpx_tag_tree_decode(
    nanopdf_jpx_tag_tree* tree,
    int x,
    int y,
    int threshold,
    nanopdf_jpx_mq_decoder* mq,
    int context_index) {
  int min_value = 0;
  size_t level_index;

  if (!tree || !mq || x < 0 || y < 0 || x >= tree->width || y >= tree->height ||
      tree->level_count == 0) {
    return 0;
  }

  level_index = tree->level_count;
  while (level_index > 0) {
    nanopdf_jpx_tag_tree_level* level;
    nanopdf_jpx_tag_tree_node* node;
    int level_x;
    int level_y;
    size_t node_index;

    level_index--;
    level = &tree->levels[level_index];
    level_x = x >> (int)level_index;
    level_y = y >> (int)level_index;
    if (level_x < 0 || level_y < 0 || level_x >= level->width ||
        level_y >= level->height) {
      return 0;
    }
    node_index = (size_t)level_y * (size_t)level->width + (size_t)level_x;
    node = &level->nodes[node_index];

    if (node->known) {
      if (node->value > min_value) {
        min_value = node->value;
      }
      continue;
    }
    if (node->value < min_value) {
      node->value = min_value;
    }
    while (node->value < threshold) {
      int bit = nanopdf_jpx_mq_decode(mq, context_index);
      if (bit) {
        node->known = 1;
        break;
      }
      node->value++;
    }
    if (!node->known && node->value >= threshold) {
      min_value = node->value;
      if (level_index == 0) {
        return 0;
      }
      continue;
    }
    min_value = node->value;
  }

  return tree->levels[0].nodes[(size_t)y * (size_t)tree->width + (size_t)x].known &&
         tree->levels[0].nodes[(size_t)y * (size_t)tree->width + (size_t)x].value <=
             threshold;
}

int nanopdf_jpx_tag_tree_value(const nanopdf_jpx_tag_tree* tree, int x, int y) {
  if (!tree || tree->level_count == 0 || x < 0 || y < 0 || x >= tree->width ||
      y >= tree->height) {
    return 0;
  }
  return tree->levels[0].nodes[(size_t)y * (size_t)tree->width + (size_t)x].value;
}

int nanopdf_jpx_get_significance_context(
    const uint8_t* state,
    int x,
    int y,
    int width,
    int height,
    int subband_type) {
  int h0 = 0;
  int h1 = 0;
  int v0 = 0;
  int v1 = 0;
  int d = 0;
  int h;
  int v;

  if (!state || width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width ||
      y >= height) {
    return 0;
  }
  if (x > 0 && (state[(size_t)y * (size_t)width + (size_t)(x - 1)] &
                NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    h0 = 1;
  }
  if (x < width - 1 &&
      (state[(size_t)y * (size_t)width + (size_t)(x + 1)] &
       NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    h1 = 1;
  }
  if (y > 0 && (state[(size_t)(y - 1) * (size_t)width + (size_t)x] &
                NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    v0 = 1;
  }
  if (y < height - 1 &&
      (state[(size_t)(y + 1) * (size_t)width + (size_t)x] &
       NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    v1 = 1;
  }
  if (x > 0 && y > 0 &&
      (state[(size_t)(y - 1) * (size_t)width + (size_t)(x - 1)] &
       NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    d++;
  }
  if (x < width - 1 && y > 0 &&
      (state[(size_t)(y - 1) * (size_t)width + (size_t)(x + 1)] &
       NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    d++;
  }
  if (x > 0 && y < height - 1 &&
      (state[(size_t)(y + 1) * (size_t)width + (size_t)(x - 1)] &
       NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    d++;
  }
  if (x < width - 1 && y < height - 1 &&
      (state[(size_t)(y + 1) * (size_t)width + (size_t)(x + 1)] &
       NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    d++;
  }

  h = h0 + h1;
  v = v0 + v1;
  if (subband_type == 0) {
    if (h == 2) {
      return 8;
    }
    if (h == 1) {
      if (v >= 1) {
        return 7;
      }
      if (d >= 1) {
        return 6;
      }
      return 5;
    }
    if (v == 2) {
      return 4;
    }
    if (v == 1) {
      if (d >= 1) {
        return 3;
      }
      return 2;
    }
    return d >= 1 ? 1 : 0;
  }
  if (subband_type == 1) {
    if (v == 2) {
      return 8;
    }
    if (v == 1) {
      if (h >= 1) {
        return 7;
      }
      if (d >= 1) {
        return 6;
      }
      return 5;
    }
    if (h == 2) {
      return 4;
    }
    if (h == 1) {
      if (d >= 1) {
        return 3;
      }
      return 2;
    }
    return d >= 1 ? 1 : 0;
  }
  {
    int hv = h + v;
    if (d >= 3) {
      return 8;
    }
    if (d == 2) {
      return hv >= 1 ? 7 : 6;
    }
    if (d == 1) {
      if (hv >= 2) {
        return 7;
      }
      if (hv == 1) {
        return 6;
      }
      return 5;
    }
    if (hv >= 2) {
      return 4;
    }
    if (hv == 1) {
      return 3;
    }
  }
  return 0;
}

int nanopdf_jpx_get_sign_context(
    const uint8_t* state,
    const int32_t* coeffs,
    int x,
    int y,
    int width,
    int height,
    int* out_sign_flip) {
  int h_contrib = 0;
  int v_contrib = 0;

  if (out_sign_flip) {
    *out_sign_flip = 0;
  }
  if (!state || !coeffs || !out_sign_flip || width <= 0 || height <= 0 || x < 0 ||
      y < 0 || x >= width || y >= height) {
    return 9;
  }

  if (x > 0 && (state[(size_t)y * (size_t)width + (size_t)(x - 1)] &
                NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    h_contrib += coeffs[(size_t)y * (size_t)width + (size_t)(x - 1)] >= 0 ? 1 : -1;
  }
  if (x < width - 1 &&
      (state[(size_t)y * (size_t)width + (size_t)(x + 1)] &
       NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    h_contrib += coeffs[(size_t)y * (size_t)width + (size_t)(x + 1)] >= 0 ? 1 : -1;
  }
  if (y > 0 && (state[(size_t)(y - 1) * (size_t)width + (size_t)x] &
                NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    v_contrib += coeffs[(size_t)(y - 1) * (size_t)width + (size_t)x] >= 0 ? 1 : -1;
  }
  if (y < height - 1 &&
      (state[(size_t)(y + 1) * (size_t)width + (size_t)x] &
       NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
    v_contrib += coeffs[(size_t)(y + 1) * (size_t)width + (size_t)x] >= 0 ? 1 : -1;
  }

  if (h_contrib > 0) {
    if (v_contrib > 0) {
      return 13;
    }
    if (v_contrib == 0) {
      return 12;
    }
    return 11;
  }
  if (h_contrib == 0) {
    if (v_contrib > 0) {
      return 10;
    }
    if (v_contrib == 0) {
      return 9;
    }
    *out_sign_flip = 1;
    return 10;
  }
  if (v_contrib > 0) {
    *out_sign_flip = 1;
    return 11;
  }
  if (v_contrib == 0) {
    *out_sign_flip = 1;
    return 12;
  }
  *out_sign_flip = 1;
  return 13;
}

int nanopdf_jpx_get_magnitude_refinement_context(
    const uint8_t* state,
    int x,
    int y,
    int width,
    int height) {
  int dy;
  int first_refinement;
  if (!state || width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width ||
      y >= height) {
    return 14;
  }
  first_refinement =
      (state[(size_t)y * (size_t)width + (size_t)x] &
       NANOPDF_JPX_CODEBLOCK_STATE_REFINED) == 0;
  if (!first_refinement) {
    return 16;
  }
  for (dy = -1; dy <= 1; ++dy) {
    int dx;
    for (dx = -1; dx <= 1; ++dx) {
      int nx = x + dx;
      int ny = y + dy;
      if (dx == 0 && dy == 0) {
        continue;
      }
      if (nx >= 0 && nx < width && ny >= 0 && ny < height &&
          (state[(size_t)ny * (size_t)width + (size_t)nx] &
           NANOPDF_JPX_CODEBLOCK_STATE_SIG)) {
        return 15;
      }
    }
  }
  return 14;
}

int nanopdf_jpx_inverse_dwt_53(
    nanopdf_context* context,
    int32_t* data,
    int width,
    int height,
    int levels) {
  int32_t* temp = NULL;
  int32_t* interleaved = NULL;
  int max_dim;
  int level;

  if (!context || !data || width <= 0 || height <= 0) {
    return 0;
  }
  if (levels <= 0 || width <= 1 || height <= 1) {
    return 1;
  }
  max_dim = width > height ? width : height;
  temp = (int32_t*)nanopdf__allocator_alloc(&context->allocator, sizeof(int32_t) * (size_t)max_dim);
  interleaved =
      (int32_t*)nanopdf__allocator_alloc(&context->allocator, sizeof(int32_t) * (size_t)max_dim);
  if (!temp || !interleaved) {
    nanopdf__allocator_free(&context->allocator, temp);
    nanopdf__allocator_free(&context->allocator, interleaved);
    return 0;
  }

  for (level = levels - 1; level >= 0; --level) {
    int w = nanopdf_jpx_ceildiv_i32(width, 1 << level);
    int h = nanopdf_jpx_ceildiv_i32(height, 1 << level);
    int half_w = nanopdf_jpx_ceildiv_i32(w, 2);
    int half_h = nanopdf_jpx_ceildiv_i32(h, 2);
    int x;
    int y;

    if (w <= 1 && h <= 1) {
      continue;
    }
    if (h > 1) {
      int h_high = h - half_h;
      for (x = 0; x < w; ++x) {
        int n;
        for (y = 0; y < h; ++y) {
          temp[y] = data[(size_t)y * (size_t)width + (size_t)x];
        }
        if (h_high > 0) {
          temp[0] -= (temp[half_h] + temp[half_h] + 2) >> 2;
          for (n = 1; n < half_h; ++n) {
            int d_prev = n - 1 < h_high ? temp[half_h + n - 1] : temp[half_h + h_high - 1];
            int d_curr = n < h_high ? temp[half_h + n] : temp[half_h + h_high - 1];
            temp[n] -= (d_prev + d_curr + 2) >> 2;
          }
        }
        for (n = 0; n < h_high; ++n) {
          int s_curr = temp[n];
          int s_next = n + 1 < half_h ? temp[n + 1] : temp[half_h - 1];
          temp[half_h + n] += (s_curr + s_next) >> 1;
        }
        for (n = 0; n < half_h; ++n) {
          interleaved[2 * n] = temp[n];
        }
        for (n = 0; n < h_high; ++n) {
          interleaved[2 * n + 1] = temp[half_h + n];
        }
        for (y = 0; y < h; ++y) {
          data[(size_t)y * (size_t)width + (size_t)x] = interleaved[y];
        }
      }
    }
    if (w > 1) {
      int w_high = w - half_w;
      for (y = 0; y < h; ++y) {
        int n;
        for (x = 0; x < w; ++x) {
          temp[x] = data[(size_t)y * (size_t)width + (size_t)x];
        }
        if (w_high > 0) {
          temp[0] -= (temp[half_w] + temp[half_w] + 2) >> 2;
          for (n = 1; n < half_w; ++n) {
            int d_prev = n - 1 < w_high ? temp[half_w + n - 1] : temp[half_w + w_high - 1];
            int d_curr = n < w_high ? temp[half_w + n] : temp[half_w + w_high - 1];
            temp[n] -= (d_prev + d_curr + 2) >> 2;
          }
        }
        for (n = 0; n < w_high; ++n) {
          int s_curr = temp[n];
          int s_next = n + 1 < half_w ? temp[n + 1] : temp[half_w - 1];
          temp[half_w + n] += (s_curr + s_next) >> 1;
        }
        for (n = 0; n < half_w; ++n) {
          interleaved[2 * n] = temp[n];
        }
        for (n = 0; n < w_high; ++n) {
          interleaved[2 * n + 1] = temp[half_w + n];
        }
        for (x = 0; x < w; ++x) {
          data[(size_t)y * (size_t)width + (size_t)x] = interleaved[x];
        }
      }
    }
  }

  nanopdf__allocator_free(&context->allocator, temp);
  nanopdf__allocator_free(&context->allocator, interleaved);
  return 1;
}

int nanopdf_jpx_inverse_dwt_97(
    nanopdf_context* context,
    float* data,
    int width,
    int height,
    int levels) {
  float* temp = NULL;
  float* interleaved = NULL;
  int max_dim;
  int level;

  if (!context || !data || width <= 0 || height <= 0) {
    return 0;
  }
  if (levels <= 0 || width <= 1 || height <= 1) {
    return 1;
  }
  max_dim = width > height ? width : height;
  temp = (float*)nanopdf__allocator_alloc(&context->allocator, sizeof(float) * (size_t)max_dim);
  interleaved = (float*)nanopdf__allocator_alloc(&context->allocator, sizeof(float) * (size_t)max_dim);
  if (!temp || !interleaved) {
    nanopdf__allocator_free(&context->allocator, temp);
    nanopdf__allocator_free(&context->allocator, interleaved);
    return 0;
  }

  for (level = levels - 1; level >= 0; --level) {
    int w = nanopdf_jpx_ceildiv_i32(width, 1 << level);
    int h = nanopdf_jpx_ceildiv_i32(height, 1 << level);
    int half_w = nanopdf_jpx_ceildiv_i32(w, 2);
    int half_h = nanopdf_jpx_ceildiv_i32(h, 2);
    int x;
    int y;

    if (w <= 1 && h <= 1) {
      continue;
    }
    if (h > 1) {
      int h_high = h - half_h;
      for (x = 0; x < w; ++x) {
        int n;
        for (y = 0; y < h; ++y) {
          temp[y] = data[(size_t)y * (size_t)width + (size_t)x];
        }
        for (n = 0; n < half_h; ++n) {
          temp[n] /= k_jpx_lift_97_k;
        }
        for (n = 0; n < h_high; ++n) {
          temp[half_h + n] *= k_jpx_lift_97_k;
        }
        for (n = 0; n < half_h; ++n) {
          float d0 = n < h_high ? temp[half_h + n] : temp[half_h + h_high - 1];
          float d1 = n > 0 ? temp[half_h + n - 1] : d0;
          temp[n] -= k_jpx_lift_97_delta * (d1 + d0);
        }
        for (n = 0; n < h_high; ++n) {
          float s0 = temp[n];
          float s1 = n + 1 < half_h ? temp[n + 1] : temp[half_h - 1];
          temp[half_h + n] -= k_jpx_lift_97_gamma * (s0 + s1);
        }
        for (n = 0; n < half_h; ++n) {
          float d0 = n < h_high ? temp[half_h + n] : temp[half_h + h_high - 1];
          float d1 = n > 0 ? temp[half_h + n - 1] : d0;
          temp[n] -= k_jpx_lift_97_beta * (d1 + d0);
        }
        for (n = 0; n < h_high; ++n) {
          float s0 = temp[n];
          float s1 = n + 1 < half_h ? temp[n + 1] : temp[half_h - 1];
          temp[half_h + n] -= k_jpx_lift_97_alpha * (s0 + s1);
        }
        for (n = 0; n < half_h; ++n) {
          interleaved[2 * n] = temp[n];
        }
        for (n = 0; n < h_high; ++n) {
          interleaved[2 * n + 1] = temp[half_h + n];
        }
        for (y = 0; y < h; ++y) {
          data[(size_t)y * (size_t)width + (size_t)x] = interleaved[y];
        }
      }
    }
    if (w > 1) {
      int w_high = w - half_w;
      for (y = 0; y < h; ++y) {
        int n;
        for (x = 0; x < w; ++x) {
          temp[x] = data[(size_t)y * (size_t)width + (size_t)x];
        }
        for (n = 0; n < half_w; ++n) {
          temp[n] /= k_jpx_lift_97_k;
        }
        for (n = 0; n < w_high; ++n) {
          temp[half_w + n] *= k_jpx_lift_97_k;
        }
        for (n = 0; n < half_w; ++n) {
          float d0 = n < w_high ? temp[half_w + n] : temp[half_w + w_high - 1];
          float d1 = n > 0 ? temp[half_w + n - 1] : d0;
          temp[n] -= k_jpx_lift_97_delta * (d1 + d0);
        }
        for (n = 0; n < w_high; ++n) {
          float s0 = temp[n];
          float s1 = n + 1 < half_w ? temp[n + 1] : temp[half_w - 1];
          temp[half_w + n] -= k_jpx_lift_97_gamma * (s0 + s1);
        }
        for (n = 0; n < half_w; ++n) {
          float d0 = n < w_high ? temp[half_w + n] : temp[half_w + w_high - 1];
          float d1 = n > 0 ? temp[half_w + n - 1] : d0;
          temp[n] -= k_jpx_lift_97_beta * (d1 + d0);
        }
        for (n = 0; n < w_high; ++n) {
          float s0 = temp[n];
          float s1 = n + 1 < half_w ? temp[n + 1] : temp[half_w - 1];
          temp[half_w + n] -= k_jpx_lift_97_alpha * (s0 + s1);
        }
        for (n = 0; n < half_w; ++n) {
          interleaved[2 * n] = temp[n];
        }
        for (n = 0; n < w_high; ++n) {
          interleaved[2 * n + 1] = temp[half_w + n];
        }
        for (x = 0; x < w; ++x) {
          data[(size_t)y * (size_t)width + (size_t)x] = interleaved[x];
        }
      }
    }
  }

  nanopdf__allocator_free(&context->allocator, temp);
  nanopdf__allocator_free(&context->allocator, interleaved);
  return 1;
}

void nanopdf_jpx_apply_inverse_mct(
    int32_t** components,
    size_t num_components,
    size_t num_pixels,
    nanopdf_jpx_wavelet_type wavelet) {
  size_t i;
  if (!components || num_components < 3 || !components[0] || !components[1] ||
      !components[2]) {
    return;
  }
  for (i = 0; i < num_pixels; ++i) {
    int32_t y = components[0][i];
    int32_t cb = components[1][i];
    int32_t cr = components[2][i];
    if (wavelet == NANOPDF_JPX_WAVELET_REVERSIBLE_5_3) {
      int32_t g = y - ((cb + cr) >> 2);
      components[0][i] = cr + g;
      components[1][i] = g;
      components[2][i] = cb + g;
    } else {
      float yf = (float)y;
      float cbf = (float)cb;
      float crf = (float)cr;
      float r = yf + 1.402f * crf;
      float g = yf - 0.34413f * cbf - 0.71414f * crf;
      float b = yf + 1.772f * cbf;
      components[0][i] = (int32_t)(r >= 0.0f ? r + 0.5f : r - 0.5f);
      components[1][i] = (int32_t)(g >= 0.0f ? g + 0.5f : g - 0.5f);
      components[2][i] = (int32_t)(b >= 0.0f ? b + 0.5f : b - 0.5f);
    }
  }
}

void nanopdf_jpx_coeffs_to_pixels(
    const int32_t* const* components,
    size_t num_components,
    const uint8_t* bit_depths,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels) {
  size_t num_pixels;
  size_t i;
  size_t c;
  if (!components || !pixels || num_components == 0) {
    return;
  }
  num_pixels = (size_t)width * (size_t)height;
  for (i = 0; i < num_pixels; ++i) {
    for (c = 0; c < num_components; ++c) {
      int32_t value = components[c] ? components[c][i] : 0;
      uint8_t bit_depth = bit_depths ? bit_depths[c] : 8u;
      if (bit_depth > 8u) {
        value >>= bit_depth - 8u;
      } else if (bit_depth < 8u) {
        value <<= 8u - bit_depth;
      }
      if (value < 0) {
        value = 0;
      }
      if (value > 255) {
        value = 255;
      }
      pixels[i * num_components + c] = (uint8_t)value;
    }
  }
}

static uint8_t jpx_coeff_to_u8(int32_t value, uint8_t bit_depth) {
  if (bit_depth > 8u) {
    value >>= bit_depth - 8u;
  } else if (bit_depth < 8u) {
    value <<= 8u - bit_depth;
  }
  if (value < 0) {
    value = 0;
  }
  if (value > 255) {
    value = 255;
  }
  return (uint8_t)value;
}

static void jpx_sampled_components_to_pixels(
    const nanopdf_jpx_header* header,
    const nanopdf_jpx_tile_component* tile_components,
    size_t tile_component_count,
    const uint8_t* bit_depths,
    uint8_t* pixels) {
  size_t width = nanopdf_jpx_image_width(header);
  size_t height = nanopdf_jpx_image_height(header);
  size_t y;
  for (y = 0; y < height; ++y) {
    size_t x;
    for (x = 0; x < width; ++x) {
      size_t c;
      for (c = 0; c < tile_component_count; ++c) {
        const nanopdf_jpx_tile_component* component = &tile_components[c];
        uint8_t x_sep = 1u;
        uint8_t y_sep = 1u;
        size_t src_x;
        size_t src_y;
        int32_t value = 0;
        if (c < header->siz.num_components) {
          x_sep = header->siz.components[c].x_separation;
          y_sep = header->siz.components[c].y_separation;
        }
        if (x_sep == 0u) {
          x_sep = 1u;
        }
        if (y_sep == 0u) {
          y_sep = 1u;
        }
        src_x = nanopdf_jpx_ceil_div_u32(
                    (uint32_t)(header->siz.x_offset + (uint32_t)x), x_sep) -
                nanopdf_jpx_ceil_div_u32(header->siz.x_offset, x_sep);
        src_y = nanopdf_jpx_ceil_div_u32(
                    (uint32_t)(header->siz.y_offset + (uint32_t)y), y_sep) -
                nanopdf_jpx_ceil_div_u32(header->siz.y_offset, y_sep);
        if (component->width > 0 && component->height > 0 &&
            component->coeffs && component->coeff_count > 0) {
          if (src_x >= (size_t)component->width) {
            src_x = (size_t)component->width - 1u;
          }
          if (src_y >= (size_t)component->height) {
            src_y = (size_t)component->height - 1u;
          }
          value = component->coeffs[src_y * (size_t)component->width + src_x];
        }
        pixels[(y * width + x) * tile_component_count + c] =
            jpx_coeff_to_u8(value, bit_depths ? bit_depths[c] : 8u);
      }
    }
  }
}

void nanopdf_jpx_codeblock_init(nanopdf_jpx_codeblock* codeblock) {
  if (!codeblock) {
    return;
  }
  memset(codeblock, 0, sizeof(*codeblock));
  codeblock->num_len_bits = 3;
}

void nanopdf_jpx_codeblock_destroy(nanopdf_context* context, nanopdf_jpx_codeblock* codeblock) {
  if (!context || !codeblock) {
    return;
  }
  nanopdf__allocator_free(&context->allocator, codeblock->pass_lengths);
  nanopdf__allocator_free(&context->allocator, codeblock->owned_data);
  nanopdf__allocator_free(&context->allocator, codeblock->coeffs);
  nanopdf_jpx_codeblock_init(codeblock);
}

static int jpx_codeblock_add_pass_length(
    nanopdf_context* context,
    nanopdf_jpx_codeblock* codeblock,
    int pass_length) {
  int* lengths;
  size_t new_capacity;
  if (!context || !codeblock || pass_length < 0) {
    return 0;
  }
  if (codeblock->pass_length_count == codeblock->pass_length_capacity) {
    new_capacity = codeblock->pass_length_capacity == 0 ? 4u : codeblock->pass_length_capacity * 2u;
    lengths = (int*)nanopdf__allocator_realloc(
        &context->allocator, codeblock->pass_lengths, sizeof(int) * new_capacity);
    if (!lengths) {
      return 0;
    }
    codeblock->pass_lengths = lengths;
    codeblock->pass_length_capacity = new_capacity;
  }
  codeblock->pass_lengths[codeblock->pass_length_count++] = pass_length;
  return 1;
}

static int jpx_codeblock_append_data(
    nanopdf_context* context,
    nanopdf_jpx_codeblock* codeblock,
    const uint8_t* data,
    size_t data_size) {
  uint8_t* storage;
  if (!context || !codeblock || (!data && data_size > 0u)) {
    return 0;
  }
  if (data_size == 0u) {
    return 1;
  }
  if (!codeblock->data && codeblock->data_size == 0u) {
    codeblock->owned_data_size = 0u;
  }
  if (codeblock->owned_data_size > SIZE_MAX - data_size) {
    return 0;
  }
  storage = (uint8_t*)nanopdf__allocator_realloc(
      &context->allocator,
      codeblock->owned_data,
      codeblock->owned_data_size + data_size);
  if (!storage) {
    return 0;
  }
  memcpy(storage + codeblock->owned_data_size, data, data_size);
  codeblock->owned_data = storage;
  codeblock->owned_data_size += data_size;
  codeblock->data = codeblock->owned_data;
  codeblock->data_size = codeblock->owned_data_size;
  return 1;
}

static void jpx_significance_propagation_pass(
    nanopdf_jpx_mq_decoder* mq,
    int32_t* coeffs,
    uint8_t* state,
    int width,
    int height,
    int bit_plane,
    int subband_type) {
  int x;
  for (x = 0; x < width; ++x) {
    int y0;
    for (y0 = 0; y0 < height; y0 += 4) {
      int y_end = y0 + 4 < height ? y0 + 4 : height;
      int y;
      for (y = y0; y < y_end; ++y) {
        size_t idx = (size_t)y * (size_t)width + (size_t)x;
        int ctx;
        int sig;
        if ((state[idx] & NANOPDF_JPX_CODEBLOCK_STATE_SIG) ||
            (state[idx] & NANOPDF_JPX_CODEBLOCK_STATE_VISITED)) {
          continue;
        }
        ctx = nanopdf_jpx_get_significance_context(state, x, y, width, height, subband_type);
        if (ctx == 0) {
          continue;
        }
        state[idx] |= NANOPDF_JPX_CODEBLOCK_STATE_VISITED;
        sig = nanopdf_jpx_mq_decode(mq, ctx);
        if (sig) {
          int sign_flip = 0;
          int sign_ctx = nanopdf_jpx_get_sign_context(
              state, coeffs, x, y, width, height, &sign_flip);
          int sign_bit = nanopdf_jpx_mq_decode(mq, sign_ctx) ^ sign_flip;
          int32_t magnitude = (int32_t)1 << bit_plane;
          coeffs[idx] = sign_bit ? -magnitude : magnitude;
          state[idx] |= NANOPDF_JPX_CODEBLOCK_STATE_SIG;
        }
      }
    }
  }
}

static void jpx_magnitude_refinement_pass(
    nanopdf_jpx_mq_decoder* mq,
    int32_t* coeffs,
    uint8_t* state,
    int width,
    int height,
    int bit_plane) {
  int x;
  for (x = 0; x < width; ++x) {
    int y0;
    for (y0 = 0; y0 < height; y0 += 4) {
      int y_end = y0 + 4 < height ? y0 + 4 : height;
      int y;
      for (y = y0; y < y_end; ++y) {
        size_t idx = (size_t)y * (size_t)width + (size_t)x;
        int ctx;
        int bit;
        int32_t abs_value;
        if (!(state[idx] & NANOPDF_JPX_CODEBLOCK_STATE_SIG) ||
            (state[idx] & NANOPDF_JPX_CODEBLOCK_STATE_VISITED)) {
          continue;
        }
        state[idx] |= NANOPDF_JPX_CODEBLOCK_STATE_VISITED;
        ctx = nanopdf_jpx_get_magnitude_refinement_context(state, x, y, width, height);
        bit = nanopdf_jpx_mq_decode(mq, ctx);
        abs_value = coeffs[idx] < 0 ? -coeffs[idx] : coeffs[idx];
        abs_value |= (int32_t)bit << bit_plane;
        coeffs[idx] = coeffs[idx] < 0 ? -abs_value : abs_value;
        state[idx] |= NANOPDF_JPX_CODEBLOCK_STATE_REFINED;
      }
    }
  }
}

static void jpx_cleanup_pass(
    nanopdf_jpx_mq_decoder* mq,
    int32_t* coeffs,
    uint8_t* state,
    int width,
    int height,
    int bit_plane,
    int subband_type) {
  int x;
  for (x = 0; x < width; ++x) {
    int y0;
    for (y0 = 0; y0 < height; y0 += 4) {
      int y_end = y0 + 4 < height ? y0 + 4 : height;
      int stripe_height = y_end - y0;
      int y = y0;
      int yy;
      if (stripe_height == 4) {
        int can_run = 1;
        for (yy = y0; yy < y_end; ++yy) {
          size_t idx = (size_t)yy * (size_t)width + (size_t)x;
          if ((state[idx] & NANOPDF_JPX_CODEBLOCK_STATE_SIG) ||
              (state[idx] & NANOPDF_JPX_CODEBLOCK_STATE_VISITED) ||
              nanopdf_jpx_get_significance_context(state, x, yy, width, height, subband_type) !=
                  0) {
            can_run = 0;
            break;
          }
        }
        if (can_run) {
          int run_bit = nanopdf_jpx_mq_decode(mq, 17);
          if (!run_bit) {
            for (yy = y0; yy < y_end; ++yy) {
              state[(size_t)yy * (size_t)width + (size_t)x] &=
                  (uint8_t)~NANOPDF_JPX_CODEBLOCK_STATE_VISITED;
            }
            continue;
          }
          y = y0 + ((nanopdf_jpx_mq_decode(mq, 18) << 1) | nanopdf_jpx_mq_decode(mq, 18));
          if (y < y_end) {
            size_t idx = (size_t)y * (size_t)width + (size_t)x;
            int sign_flip = 0;
            int sign_ctx = nanopdf_jpx_get_sign_context(
                state, coeffs, x, y, width, height, &sign_flip);
            int sign_bit = nanopdf_jpx_mq_decode(mq, sign_ctx) ^ sign_flip;
            int32_t magnitude = (int32_t)1 << bit_plane;
            coeffs[idx] = sign_bit ? -magnitude : magnitude;
            state[idx] |= NANOPDF_JPX_CODEBLOCK_STATE_SIG;
          }
          y++;
        }
      }
      for (; y < y_end; ++y) {
        size_t idx = (size_t)y * (size_t)width + (size_t)x;
        int ctx;
        int sig;
        if ((state[idx] & NANOPDF_JPX_CODEBLOCK_STATE_SIG) ||
            (state[idx] & NANOPDF_JPX_CODEBLOCK_STATE_VISITED)) {
          state[idx] &= (uint8_t)~NANOPDF_JPX_CODEBLOCK_STATE_VISITED;
          continue;
        }
        ctx = nanopdf_jpx_get_significance_context(state, x, y, width, height, subband_type);
        sig = nanopdf_jpx_mq_decode(mq, ctx);
        if (sig) {
          int sign_flip = 0;
          int sign_ctx = nanopdf_jpx_get_sign_context(
              state, coeffs, x, y, width, height, &sign_flip);
          int sign_bit = nanopdf_jpx_mq_decode(mq, sign_ctx) ^ sign_flip;
          int32_t magnitude = (int32_t)1 << bit_plane;
          coeffs[idx] = sign_bit ? -magnitude : magnitude;
          state[idx] |= NANOPDF_JPX_CODEBLOCK_STATE_SIG;
        }
        state[idx] &= (uint8_t)~NANOPDF_JPX_CODEBLOCK_STATE_VISITED;
      }
      for (yy = y0; yy < y_end; ++yy) {
        state[(size_t)yy * (size_t)width + (size_t)x] &=
            (uint8_t)~NANOPDF_JPX_CODEBLOCK_STATE_VISITED;
      }
    }
  }
}

int nanopdf_jpx_decode_codeblock(
    nanopdf_context* context,
    nanopdf_jpx_codeblock* codeblock,
    uint8_t bit_depth,
    uint8_t num_guard_bits,
    int subband_type) {
  uint8_t* state = NULL;
  nanopdf_jpx_mq_decoder mq;
  int num_bit_planes;
  int first_bit_plane;
  int current_bit_plane;
  int pass_idx = 0;
  int pass_type = 2;
  size_t coeff_count;

  if (!context || !codeblock || codeblock->width <= 0 || codeblock->height <= 0) {
    return 0;
  }
  if (!codeblock->data || codeblock->data_size == 0 || codeblock->num_passes <= 0) {
    return 1;
  }
  coeff_count = (size_t)codeblock->width * (size_t)codeblock->height;
  nanopdf__allocator_free(&context->allocator, codeblock->coeffs);
  codeblock->coeffs =
      (int32_t*)nanopdf__allocator_alloc(&context->allocator, sizeof(int32_t) * coeff_count);
  state = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, coeff_count);
  if (!codeblock->coeffs || !state) {
    nanopdf__allocator_free(&context->allocator, state);
    nanopdf__allocator_free(&context->allocator, codeblock->coeffs);
    codeblock->coeffs = NULL;
    codeblock->coeff_count = 0;
    return 0;
  }
  memset(codeblock->coeffs, 0, sizeof(int32_t) * coeff_count);
  memset(state, 0, coeff_count);
  codeblock->coeff_count = coeff_count;

  nanopdf_jpx_mq_decoder_init(&mq, codeblock->data, codeblock->data_size);
  num_bit_planes = (int)bit_depth + (int)num_guard_bits - 1;
  first_bit_plane = num_bit_planes - codeblock->zero_bit_planes;
  if (first_bit_plane < 0) {
    first_bit_plane = 0;
  }
  current_bit_plane = first_bit_plane;
  while (pass_idx < codeblock->num_passes && current_bit_plane >= 0) {
    if (pass_type == 0) {
      jpx_significance_propagation_pass(
          &mq, codeblock->coeffs, state, codeblock->width, codeblock->height,
          current_bit_plane, subband_type);
    } else if (pass_type == 1) {
      jpx_magnitude_refinement_pass(
          &mq, codeblock->coeffs, state, codeblock->width, codeblock->height,
          current_bit_plane);
    } else {
      jpx_cleanup_pass(
          &mq, codeblock->coeffs, state, codeblock->width, codeblock->height,
          current_bit_plane, subband_type);
    }
    pass_idx++;
    pass_type++;
    if (pass_type > 2) {
      pass_type = 0;
      current_bit_plane--;
    }
  }

  nanopdf__allocator_free(&context->allocator, state);
  return 1;
}

void nanopdf_jpx_tile_component_init(nanopdf_jpx_tile_component* tile_component) {
  if (!tile_component) {
    return;
  }
  memset(tile_component, 0, sizeof(*tile_component));
}

void nanopdf_jpx_tile_component_destroy(
    nanopdf_context* context,
    nanopdf_jpx_tile_component* tile_component) {
  size_t r;
  if (!context || !tile_component) {
    return;
  }
  for (r = 0; r < tile_component->res_level_count; ++r) {
    size_t s;
    nanopdf_jpx_res_level* res = &tile_component->res_levels[r];
    for (s = 0; s < res->subband_count; ++s) {
      size_t c;
      nanopdf_jpx_subband* subband = &res->subbands[s];
      for (c = 0; c < subband->codeblock_count; ++c) {
        nanopdf_jpx_codeblock_destroy(context, &subband->codeblocks[c]);
      }
      nanopdf__allocator_free(&context->allocator, subband->codeblocks);
    }
    nanopdf__allocator_free(&context->allocator, res->subbands);
  }
  nanopdf__allocator_free(&context->allocator, tile_component->res_levels);
  nanopdf__allocator_free(&context->allocator, tile_component->coeffs);
  nanopdf_jpx_tile_component_init(tile_component);
}

static int jpx_subband_alloc_codeblocks(
    nanopdf_context* context,
    nanopdf_jpx_subband* subband,
    int codeblock_width,
    int codeblock_height) {
  int cby;
  if (!context || !subband || subband->width <= 0 || subband->height <= 0 ||
      codeblock_width <= 0 || codeblock_height <= 0) {
    return 1;
  }
  subband->num_xcb = nanopdf_jpx_ceildiv_i32(subband->width, codeblock_width);
  subband->num_ycb = nanopdf_jpx_ceildiv_i32(subband->height, codeblock_height);
  subband->codeblock_count = (size_t)subband->num_xcb * (size_t)subband->num_ycb;
  subband->codeblocks = (nanopdf_jpx_codeblock*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(nanopdf_jpx_codeblock) * subband->codeblock_count);
  if (!subband->codeblocks) {
    subband->codeblock_count = 0;
    return 0;
  }
  for (cby = 0; cby < subband->num_ycb; ++cby) {
    int cbx;
    for (cbx = 0; cbx < subband->num_xcb; ++cbx) {
      nanopdf_jpx_codeblock* block =
          &subband->codeblocks[(size_t)cby * (size_t)subband->num_xcb + (size_t)cbx];
      nanopdf_jpx_codeblock_init(block);
      block->x0 = cbx * codeblock_width;
      block->y0 = cby * codeblock_height;
      block->width = codeblock_width < subband->width - block->x0
                         ? codeblock_width
                         : subband->width - block->x0;
      block->height = codeblock_height < subband->height - block->y0
                          ? codeblock_height
                          : subband->height - block->y0;
    }
  }
  return 1;
}

int nanopdf_jpx_build_tile_component(
    nanopdf_context* context,
    nanopdf_jpx_tile_component* tile_component,
    const nanopdf_jpx_cod_params* cod,
    int tile_x0,
    int tile_y0,
    int tile_x1,
    int tile_y1) {
  int num_levels;
  int cb_width;
  int cb_height;
  int r;
  if (!context || !tile_component || !cod || tile_x1 <= tile_x0 || tile_y1 <= tile_y0 ||
      cod->codeblock_width > 30 || cod->codeblock_height > 30) {
    return 0;
  }
  nanopdf_jpx_tile_component_destroy(context, tile_component);
  num_levels = (int)cod->num_decomp_levels;
  cb_width = 1 << ((int)cod->codeblock_width + 2);
  cb_height = 1 << ((int)cod->codeblock_height + 2);
  tile_component->x0 = tile_x0;
  tile_component->y0 = tile_y0;
  tile_component->width = tile_x1 - tile_x0;
  tile_component->height = tile_y1 - tile_y0;
  tile_component->coeff_count =
      (size_t)tile_component->width * (size_t)tile_component->height;
  tile_component->coeffs = (int32_t*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(int32_t) * tile_component->coeff_count);
  tile_component->res_level_count = (size_t)num_levels + 1u;
  tile_component->res_levels = (nanopdf_jpx_res_level*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(nanopdf_jpx_res_level) * tile_component->res_level_count);
  if (!tile_component->coeffs || !tile_component->res_levels) {
    nanopdf_jpx_tile_component_destroy(context, tile_component);
    return 0;
  }
  memset(tile_component->coeffs, 0, sizeof(int32_t) * tile_component->coeff_count);
  memset(
      tile_component->res_levels,
      0,
      sizeof(nanopdf_jpx_res_level) * tile_component->res_level_count);

  for (r = 0; r <= num_levels; ++r) {
    int div = 1 << (num_levels - r);
    nanopdf_jpx_res_level* res = &tile_component->res_levels[r];
    res->level = r;
    res->width = nanopdf_jpx_ceildiv_i32(tile_component->width, div);
    res->height = nanopdf_jpx_ceildiv_i32(tile_component->height, div);
    res->x0 = nanopdf_jpx_ceildiv_i32(tile_x0, div);
    res->y0 = nanopdf_jpx_ceildiv_i32(tile_y0, div);
    if (cod->precinct_sizes && (size_t)r < cod->precinct_size_count) {
      uint8_t ps = cod->precinct_sizes[r];
      res->precinct_width = 1 << (ps & 0x0f);
      res->precinct_height = 1 << ((ps >> 4) & 0x0f);
    } else {
      res->precinct_width = 1 << 15;
      res->precinct_height = 1 << 15;
    }
    if (res->width > 0 && res->height > 0) {
      res->num_precincts_x = nanopdf_jpx_ceildiv_i32(res->width, res->precinct_width);
      res->num_precincts_y = nanopdf_jpx_ceildiv_i32(res->height, res->precinct_height);
    }
    res->subband_count = r == 0 ? 1u : 3u;
    res->subbands = (nanopdf_jpx_subband*)nanopdf__allocator_alloc(
        &context->allocator, sizeof(nanopdf_jpx_subband) * res->subband_count);
    if (!res->subbands) {
      nanopdf_jpx_tile_component_destroy(context, tile_component);
      return 0;
    }
    memset(res->subbands, 0, sizeof(nanopdf_jpx_subband) * res->subband_count);
    if (r == 0) {
      nanopdf_jpx_subband* sb = &res->subbands[0];
      sb->type = NANOPDF_JPX_SUBBAND_LL;
      sb->res_level = 0;
      sb->band_index = 0;
      sb->x0 = res->x0;
      sb->y0 = res->y0;
      sb->width = res->width;
      sb->height = res->height;
      if (!jpx_subband_alloc_codeblocks(context, sb, cb_width, cb_height)) {
        nanopdf_jpx_tile_component_destroy(context, tile_component);
        return 0;
      }
    } else {
      int prev_div = 1 << (num_levels - r + 1);
      int ll_w = nanopdf_jpx_ceildiv_i32(tile_component->width, prev_div);
      int ll_h = nanopdf_jpx_ceildiv_i32(tile_component->height, prev_div);
      size_t b;
      for (b = 0; b < 3u; ++b) {
        nanopdf_jpx_subband* sb = &res->subbands[b];
        sb->type = b == 0 ? NANOPDF_JPX_SUBBAND_HL
                          : (b == 1 ? NANOPDF_JPX_SUBBAND_LH : NANOPDF_JPX_SUBBAND_HH);
        sb->res_level = r;
        sb->band_index = 1 + (r - 1) * 3 + (int)b;
        if (b == 0) {
          sb->width = res->width - ll_w;
          sb->height = ll_h;
        } else if (b == 1) {
          sb->width = ll_w;
          sb->height = res->height - ll_h;
        } else {
          sb->width = res->width - ll_w;
          sb->height = res->height - ll_h;
        }
        if (sb->width < 0) {
          sb->width = 0;
        }
        if (sb->height < 0) {
          sb->height = 0;
        }
        if (!jpx_subband_alloc_codeblocks(context, sb, cb_width, cb_height)) {
          nanopdf_jpx_tile_component_destroy(context, tile_component);
          return 0;
        }
      }
    }
  }
  return 1;
}

int nanopdf_jpx_parse_tile_part_header(
    const uint8_t* codestream,
    size_t codestream_size,
    size_t offset,
    nanopdf_jpx_tile_part_header* out_header,
    size_t* out_data_offset,
    size_t* out_data_size) {
  uint16_t marker;
  uint16_t lsot;
  uint32_t psot;
  size_t data_offset;
  size_t data_size = 0;
  size_t marker_offset;
  if (!codestream || !out_header || offset + 12u > codestream_size) {
    return 0;
  }
  marker = nanopdf_jpx_read_be16_at(codestream + offset);
  if (marker != NANOPDF_JPX_SOT) {
    return 0;
  }
  lsot = nanopdf_jpx_read_be16_at(codestream + offset + 2u);
  if (lsot < 10u || offset + 2u + (size_t)lsot > codestream_size) {
    return 0;
  }
  psot = nanopdf_jpx_read_be32_at(codestream + offset + 6u);
  memset(out_header, 0, sizeof(*out_header));
  out_header->tile_index = nanopdf_jpx_read_be16_at(codestream + offset + 4u);
  out_header->tile_part_length = psot;
  out_header->tile_part_index = codestream[offset + 10u];
  out_header->num_tile_parts = codestream[offset + 11u];
  data_offset = offset + 2u + (size_t)lsot;
  marker_offset = data_offset;
  while (marker_offset + 2u <= codestream_size) {
    uint16_t marker_value = nanopdf_jpx_read_be16_at(codestream + marker_offset);
    if (marker_value == NANOPDF_JPX_SOD) {
      data_offset = marker_offset + 2u;
      break;
    }
    if (marker_value == NANOPDF_JPX_EOC || marker_value == NANOPDF_JPX_SOT) {
      break;
    }
    if ((marker_value & 0xff00u) != 0xff00u) {
      break;
    }
    if (!nanopdf_jpx_marker_has_segment_length(marker_value)) {
      return 0;
    }
    if (marker_offset + 4u > codestream_size) {
      return 0;
    }
    {
      uint16_t marker_length = nanopdf_jpx_read_be16_at(codestream + marker_offset + 2u);
      if (marker_length < 2u ||
          marker_offset + 2u + (size_t)marker_length > codestream_size) {
        return 0;
      }
      marker_offset += 2u + (size_t)marker_length;
    }
  }
  if (psot == 0u) {
    data_size = data_offset <= codestream_size ? codestream_size - data_offset : 0u;
    {
      size_t scan = data_offset;
      while (scan + 1u < codestream_size) {
        uint16_t scan_marker = nanopdf_jpx_read_be16_at(codestream + scan);
        if (scan_marker == NANOPDF_JPX_EOC || scan_marker == NANOPDF_JPX_SOT) {
          data_size = scan - data_offset;
          break;
        }
        ++scan;
      }
    }
  } else if ((size_t)psot >= data_offset - offset &&
             offset + (size_t)psot <= codestream_size) {
    data_size = (size_t)psot - (data_offset - offset);
  } else if (psot != 0u) {
    return 0;
  }
  if (out_data_offset) {
    *out_data_offset = data_offset;
  }
  if (out_data_size) {
    *out_data_size = data_size;
  }
  return 1;
}

int nanopdf_jpx_parse_tile_part_markers(
    nanopdf_context* context,
    const uint8_t* codestream,
    size_t codestream_size,
    size_t offset,
    nanopdf_jpx_header* header) {
  uint16_t marker;
  uint16_t lsot;
  size_t marker_offset;
  if (!context || !codestream || !header || offset + 12u > codestream_size) {
    return 0;
  }
  marker = nanopdf_jpx_read_be16_at(codestream + offset);
  if (marker != NANOPDF_JPX_SOT) {
    return 0;
  }
  lsot = nanopdf_jpx_read_be16_at(codestream + offset + 2u);
  if (lsot < 10u || offset + 2u + (size_t)lsot > codestream_size) {
    return 0;
  }
  marker_offset = offset + 2u + (size_t)lsot;
  while (marker_offset + 2u <= codestream_size) {
    uint16_t marker_value = nanopdf_jpx_read_be16_at(codestream + marker_offset);
    uint16_t marker_length;
    nanopdf_jpx_bit_reader reader;
    if (marker_value == NANOPDF_JPX_SOD) {
      return 1;
    }
    if (marker_value == NANOPDF_JPX_EOC || marker_value == NANOPDF_JPX_SOT) {
      return 1;
    }
    if ((marker_value & 0xff00u) != 0xff00u) {
      return 1;
    }
    if (!nanopdf_jpx_marker_has_segment_length(marker_value) ||
        marker_offset + 4u > codestream_size) {
      return 0;
    }
    marker_length = nanopdf_jpx_read_be16_at(codestream + marker_offset + 2u);
    if (marker_length < 2u ||
        marker_offset + 2u + (size_t)marker_length > codestream_size) {
      return 0;
    }
    nanopdf_jpx_bit_reader_init(
        &reader, codestream + marker_offset + 4u, (size_t)marker_length - 2u);
    switch (marker_value) {
      case NANOPDF_JPX_COD:
        if (!nanopdf_jpx_parse_cod(context, &reader, marker_length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_COC:
        if (!nanopdf_jpx_parse_coc(context, &reader, marker_length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_QCD:
        if (!nanopdf_jpx_parse_qcd(context, &reader, marker_length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_QCC:
        if (!nanopdf_jpx_parse_qcc(context, &reader, marker_length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_RGN:
        if (!nanopdf_jpx_parse_rgn(&reader, marker_length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_POC:
        if (!nanopdf_jpx_parse_poc(context, &reader, marker_length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_COM:
        if (!nanopdf_jpx_parse_com(context, &reader, marker_length, header)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_PPT:
        if (!nanopdf_jpx_parse_indexed_data(
                context, &reader, marker_length, &header->ppt_markers,
                &header->ppt_marker_count)) {
          return 0;
        }
        break;
      case NANOPDF_JPX_PLT:
        if (!nanopdf_jpx_parse_packet_lengths(
                context, &reader, marker_length, &header->plt_markers,
                &header->plt_marker_count)) {
          return 0;
        }
        break;
      default:
        break;
    }
    marker_offset += 2u + (size_t)marker_length;
  }
  return 1;
}

int nanopdf_jpx_parse_packet_simple(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t* offset,
    const nanopdf_jpx_cod_params* cod,
    nanopdf_jpx_subband* subbands,
    size_t subband_count) {
  nanopdf_jpx_packet_header_reader reader;
  size_t si;
  if (!context || !data || !offset || !cod || !subbands || *offset >= size) {
    return 0;
  }
  for (si = 0; si < subband_count; ++si) {
    nanopdf_jpx_subband* subband = &subbands[si];
    size_t cbi;
    for (cbi = 0; cbi < subband->codeblock_count; ++cbi) {
      nanopdf_jpx_codeblock* block = &subband->codeblocks[cbi];
      block->packet_included = 0u;
      block->packet_pass_length_start = block->pass_length_count;
    }
  }
  if (cod->use_sop && *offset + 6u <= size &&
      data[*offset] == 0xffu && data[*offset + 1u] == 0x91u) {
    *offset += 6u;
  }

  jpx_packet_header_reader_init(&reader, data, size, *offset);
  if (!jpx_packet_header_read_bit(&reader)) {
    if (reader.overflow) {
      return 0;
    }
    jpx_packet_header_align(&reader);
    *offset = reader.pos;
    if (cod->use_eph && *offset + 2u <= size &&
        data[*offset] == 0xffu && data[*offset + 1u] == 0x92u) {
      *offset += 2u;
    }
    return 1;
  }

  for (si = 0; si < subband_count; ++si) {
    nanopdf_jpx_subband* subband = &subbands[si];
    size_t cbi;
    for (cbi = 0; cbi < subband->codeblock_count; ++cbi) {
      nanopdf_jpx_codeblock* block = &subband->codeblocks[cbi];
      int include_bit;
      int num_new_passes = 0;
      int first_bit;
      int delta_len_bits = 0;
      int data_length;

      if (!block->included) {
        include_bit = jpx_packet_header_read_bit(&reader);
        if (!include_bit) {
          continue;
        }
        block->included = 1;
        block->zero_bit_planes = 0;
        while (!jpx_packet_header_read_bit(&reader)) {
          block->zero_bit_planes++;
          if (block->zero_bit_planes > 30) {
            break;
          }
        }
      } else {
        include_bit = jpx_packet_header_read_bit(&reader);
        if (!include_bit) {
          continue;
        }
      }

      first_bit = jpx_packet_header_read_bit(&reader);
      if (first_bit == 0) {
        num_new_passes = 1;
      } else {
        int second_bit = jpx_packet_header_read_bit(&reader);
        if (second_bit == 0) {
          num_new_passes = 2;
        } else {
          int value = jpx_packet_header_read_bits(&reader, 2);
          if (value < 3) {
            num_new_passes = 3 + value;
          } else {
            value = jpx_packet_header_read_bits(&reader, 5);
            num_new_passes = value < 31 ? 6 + value
                                        : 37 + jpx_packet_header_read_bits(&reader, 7);
          }
        }
      }
      while (jpx_packet_header_read_bit(&reader)) {
        delta_len_bits++;
        if (delta_len_bits > 10) {
          break;
        }
      }
      block->num_len_bits += delta_len_bits;
      data_length = jpx_packet_header_read_bits(&reader, block->num_len_bits);
      if (!jpx_codeblock_add_pass_length(context, block, data_length)) {
        return 0;
      }
      block->packet_included = 1u;
      block->num_passes += num_new_passes;
    }
  }

  if (reader.overflow) {
    return 0;
  }
  jpx_packet_header_align(&reader);
  *offset = reader.pos;
  if (cod->use_eph && *offset + 2u <= size &&
      data[*offset] == 0xffu && data[*offset + 1u] == 0x92u) {
    *offset += 2u;
  }

  for (si = 0; si < subband_count; ++si) {
    nanopdf_jpx_subband* subband = &subbands[si];
    size_t cbi;
    for (cbi = 0; cbi < subband->codeblock_count; ++cbi) {
      nanopdf_jpx_codeblock* block = &subband->codeblocks[cbi];
      size_t length_index;
      if (!block->packet_included ||
          block->packet_pass_length_start >= block->pass_length_count) {
        continue;
      }
      for (length_index = block->packet_pass_length_start;
           length_index < block->pass_length_count;
           ++length_index) {
        int length = block->pass_lengths[length_index];
        if (length <= 0) {
          continue;
        }
        if (*offset >= size) {
          return 0;
        }
        if ((size_t)length > size - *offset) {
          return 0;
        }
        if (!jpx_codeblock_append_data(context, block, data + *offset, (size_t)length)) {
          return 0;
        }
        *offset += (size_t)length;
      }
    }
  }
  return 1;
}

static int jpx_parse_packet_separate_header(
    nanopdf_context* context,
    const uint8_t* packet_headers,
    size_t packet_header_size,
    size_t* packet_header_offset,
    const uint8_t* packet_bodies,
    size_t packet_body_size,
    size_t* packet_body_offset,
    const nanopdf_jpx_cod_params* cod,
    nanopdf_jpx_subband* subbands,
    size_t subband_count) {
  nanopdf_jpx_packet_header_reader reader;
  size_t si;
  if (!context || !packet_headers || !packet_header_offset || !packet_bodies ||
      !packet_body_offset || !cod || !subbands || *packet_header_offset >= packet_header_size) {
    return 0;
  }
  for (si = 0; si < subband_count; ++si) {
    nanopdf_jpx_subband* subband = &subbands[si];
    size_t cbi;
    for (cbi = 0; cbi < subband->codeblock_count; ++cbi) {
      nanopdf_jpx_codeblock* block = &subband->codeblocks[cbi];
      block->packet_included = 0u;
      block->packet_pass_length_start = block->pass_length_count;
    }
  }
  if (cod->use_sop && *packet_header_offset + 6u <= packet_header_size &&
      packet_headers[*packet_header_offset] == 0xffu &&
      packet_headers[*packet_header_offset + 1u] == 0x91u) {
    *packet_header_offset += 6u;
  }

  jpx_packet_header_reader_init(
      &reader, packet_headers, packet_header_size, *packet_header_offset);
  if (!jpx_packet_header_read_bit(&reader)) {
    if (reader.overflow) {
      return 0;
    }
    jpx_packet_header_align(&reader);
    *packet_header_offset = reader.pos;
    if (cod->use_eph && *packet_header_offset + 2u <= packet_header_size &&
        packet_headers[*packet_header_offset] == 0xffu &&
        packet_headers[*packet_header_offset + 1u] == 0x92u) {
      *packet_header_offset += 2u;
    }
    return 1;
  }

  for (si = 0; si < subband_count; ++si) {
    nanopdf_jpx_subband* subband = &subbands[si];
    size_t cbi;
    for (cbi = 0; cbi < subband->codeblock_count; ++cbi) {
      nanopdf_jpx_codeblock* block = &subband->codeblocks[cbi];
      int include_bit;
      int num_new_passes = 0;
      int first_bit;
      int delta_len_bits = 0;
      int data_length;

      if (!block->included) {
        include_bit = jpx_packet_header_read_bit(&reader);
        if (!include_bit) {
          continue;
        }
        block->included = 1;
        block->zero_bit_planes = 0;
        while (!jpx_packet_header_read_bit(&reader)) {
          block->zero_bit_planes++;
          if (block->zero_bit_planes > 30) {
            break;
          }
        }
      } else {
        include_bit = jpx_packet_header_read_bit(&reader);
        if (!include_bit) {
          continue;
        }
      }

      first_bit = jpx_packet_header_read_bit(&reader);
      if (first_bit == 0) {
        num_new_passes = 1;
      } else {
        int second_bit = jpx_packet_header_read_bit(&reader);
        if (second_bit == 0) {
          num_new_passes = 2;
        } else {
          int value = jpx_packet_header_read_bits(&reader, 2);
          if (value < 3) {
            num_new_passes = 3 + value;
          } else {
            value = jpx_packet_header_read_bits(&reader, 5);
            num_new_passes = value < 31 ? 6 + value
                                        : 37 + jpx_packet_header_read_bits(&reader, 7);
          }
        }
      }
      while (jpx_packet_header_read_bit(&reader)) {
        delta_len_bits++;
        if (delta_len_bits > 10) {
          break;
        }
      }
      block->num_len_bits += delta_len_bits;
      data_length = jpx_packet_header_read_bits(&reader, block->num_len_bits);
      if (!jpx_codeblock_add_pass_length(context, block, data_length)) {
        return 0;
      }
      block->packet_included = 1u;
      block->num_passes += num_new_passes;
    }
  }

  if (reader.overflow) {
    return 0;
  }
  jpx_packet_header_align(&reader);
  *packet_header_offset = reader.pos;
  if (cod->use_eph && *packet_header_offset + 2u <= packet_header_size &&
      packet_headers[*packet_header_offset] == 0xffu &&
      packet_headers[*packet_header_offset + 1u] == 0x92u) {
    *packet_header_offset += 2u;
  }

  for (si = 0; si < subband_count; ++si) {
    nanopdf_jpx_subband* subband = &subbands[si];
    size_t cbi;
    for (cbi = 0; cbi < subband->codeblock_count; ++cbi) {
      nanopdf_jpx_codeblock* block = &subband->codeblocks[cbi];
      size_t length_index;
      if (!block->packet_included ||
          block->packet_pass_length_start >= block->pass_length_count) {
        continue;
      }
      for (length_index = block->packet_pass_length_start;
           length_index < block->pass_length_count;
           ++length_index) {
        int length = block->pass_lengths[length_index];
        if (length <= 0) {
          continue;
        }
        if (*packet_body_offset >= packet_body_size) {
          return 0;
        }
        if ((size_t)length > packet_body_size - *packet_body_offset) {
          return 0;
        }
        if (!jpx_codeblock_append_data(
                context,
                block,
                packet_bodies + *packet_body_offset,
                (size_t)length)) {
          return 0;
        }
        *packet_body_offset += (size_t)length;
      }
    }
  }
  return 1;
}

static void jpx_subband_offset(
    const nanopdf_jpx_tile_component* tile_component,
    const nanopdf_jpx_subband* subband,
    uint8_t num_decomp_levels,
    int* out_x,
    int* out_y) {
  int offset_x = 0;
  int offset_y = 0;
  if (tile_component && subband && subband->res_level > 0) {
    int div = 1 << ((int)num_decomp_levels - subband->res_level + 1);
    int ll_w = nanopdf_jpx_ceildiv_i32(tile_component->width, div);
    int ll_h = nanopdf_jpx_ceildiv_i32(tile_component->height, div);
    if (subband->type == NANOPDF_JPX_SUBBAND_HL) {
      offset_x = ll_w;
    } else if (subband->type == NANOPDF_JPX_SUBBAND_LH) {
      offset_y = ll_h;
    } else if (subband->type == NANOPDF_JPX_SUBBAND_HH) {
      offset_x = ll_w;
      offset_y = ll_h;
    }
  }
  if (out_x) {
    *out_x = offset_x;
  }
  if (out_y) {
    *out_y = offset_y;
  }
}

int nanopdf_jpx_place_codeblock_coeffs(
    nanopdf_jpx_tile_component* tile_component,
    const nanopdf_jpx_subband* subband,
    const nanopdf_jpx_codeblock* codeblock,
    uint8_t num_decomp_levels) {
  int offset_x = 0;
  int offset_y = 0;
  int y;
  if (!tile_component || !subband || !codeblock || !tile_component->coeffs ||
      !codeblock->coeffs) {
    return 0;
  }
  jpx_subband_offset(tile_component, subband, num_decomp_levels, &offset_x, &offset_y);
  for (y = 0; y < codeblock->height; ++y) {
    int x;
    for (x = 0; x < codeblock->width; ++x) {
      int dst_x = offset_x + codeblock->x0 + x;
      int dst_y = offset_y + codeblock->y0 + y;
      if (dst_x >= 0 && dst_y >= 0 && dst_x < tile_component->width &&
          dst_y < tile_component->height) {
        tile_component->coeffs[(size_t)dst_y * (size_t)tile_component->width + (size_t)dst_x] =
            codeblock->coeffs[(size_t)y * (size_t)codeblock->width + (size_t)x];
      }
    }
  }
  return 1;
}

static int32_t jpx_round_float_to_i32(float value) {
  return (int32_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static float jpx_pow2_i32(int exponent) {
  float value = 1.0f;
  if (exponent >= 0) {
    while (exponent-- > 0) {
      value *= 2.0f;
    }
  } else {
    while (exponent++ < 0) {
      value *= 0.5f;
    }
  }
  return value;
}

void nanopdf_jpx_dequantize_subband(
    nanopdf_jpx_tile_component* tile_component,
    const nanopdf_jpx_subband* subband,
    const nanopdf_jpx_qcd_params* qcd,
    uint8_t bit_depth,
    uint8_t num_decomp_levels) {
  int offset_x = 0;
  int offset_y = 0;
  float step_size;
  int exponent;
  int mantissa;
  int y;
  if (!tile_component || !subband || !qcd || !tile_component->coeffs ||
      qcd->quant_style == 0) {
    return;
  }
  if (qcd->quant_style == 1) {
    if (qcd->step_size_count == 0) {
      return;
    }
    exponent = qcd->step_sizes[0].exponent;
    mantissa = qcd->step_sizes[0].mantissa;
  } else {
    if (subband->band_index < 0 || (size_t)subband->band_index >= qcd->step_size_count) {
      return;
    }
    exponent = qcd->step_sizes[subband->band_index].exponent;
    mantissa = qcd->step_sizes[subband->band_index].mantissa;
  }
  step_size = (1.0f + (float)mantissa / 2048.0f) * jpx_pow2_i32(exponent - (int)bit_depth);
  jpx_subband_offset(tile_component, subband, num_decomp_levels, &offset_x, &offset_y);
  for (y = 0; y < subband->height; ++y) {
    int x;
    for (x = 0; x < subband->width; ++x) {
      int dst_x = offset_x + x;
      int dst_y = offset_y + y;
      if (dst_x >= 0 && dst_y >= 0 && dst_x < tile_component->width &&
          dst_y < tile_component->height) {
        size_t idx = (size_t)dst_y * (size_t)tile_component->width + (size_t)dst_x;
        tile_component->coeffs[idx] =
            jpx_round_float_to_i32((float)tile_component->coeffs[idx] * step_size);
      }
    }
  }
}

static int jpx_subband_decode_type(const nanopdf_jpx_subband* subband) {
  if (!subband) {
    return 2;
  }
  if (subband->type == NANOPDF_JPX_SUBBAND_HL) {
    return 0;
  }
  if (subband->type == NANOPDF_JPX_SUBBAND_LH) {
    return 1;
  }
  return 2;
}

static int jpx_decode_tile_component_codeblocks(
    nanopdf_context* context,
    const nanopdf_jpx_header* header,
    nanopdf_jpx_tile_component* tile_component,
    size_t component_index) {
  size_t ri;
  uint8_t bit_depth = 8;
  const nanopdf_jpx_cod_params* cod = &header->cod;
  const nanopdf_jpx_qcd_params* qcd = &header->qcd;
  if (header->siz.components && component_index < header->siz.num_components) {
    bit_depth = header->siz.components[component_index].bit_depth;
  }
  if (header->has_component_cod && header->component_cod &&
      component_index < header->siz.num_components &&
      header->has_component_cod[component_index]) {
    cod = &header->component_cod[component_index];
  }
  if (header->has_component_qcd && header->component_qcd &&
      component_index < header->siz.num_components &&
      header->has_component_qcd[component_index]) {
    qcd = &header->component_qcd[component_index];
  }
  for (ri = 0; ri < tile_component->res_level_count; ++ri) {
    nanopdf_jpx_res_level* res = &tile_component->res_levels[ri];
    size_t si;
    for (si = 0; si < res->subband_count; ++si) {
      nanopdf_jpx_subband* subband = &res->subbands[si];
      size_t cbi;
      int subband_type = jpx_subband_decode_type(subband);
      for (cbi = 0; cbi < subband->codeblock_count; ++cbi) {
        nanopdf_jpx_codeblock* block = &subband->codeblocks[cbi];
        if (!block->included || !block->data || block->data_size == 0) {
          continue;
        }
        if (!nanopdf_jpx_decode_codeblock(
                context, block, bit_depth, qcd->num_guard_bits, subband_type)) {
          return 0;
        }
        if (!nanopdf_jpx_place_codeblock_coeffs(
                tile_component, subband, block, cod->num_decomp_levels)) {
          return 0;
        }
      }
      nanopdf_jpx_dequantize_subband(
          tile_component, subband, qcd, bit_depth, cod->num_decomp_levels);
    }
  }
  return 1;
}

static int jpx_parse_packet_for_component_resolution(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t* offset,
    const uint8_t* packet_headers,
    size_t packet_header_size,
    size_t* packet_header_offset,
    const uint8_t* packet_bodies,
    size_t packet_body_size,
    size_t* packet_body_offset,
    const nanopdf_jpx_header* header,
    nanopdf_jpx_tile_component* tile_components,
    size_t tile_component_count,
    size_t component_index,
    int res_index,
    int separate_headers) {
  nanopdf_jpx_tile_component* component;
  const nanopdf_jpx_cod_params* cod = &header->cod;
  if (component_index >= tile_component_count) {
    return 1;
  }
  if (header->has_component_cod && header->component_cod &&
      component_index < header->siz.num_components &&
      header->has_component_cod[component_index]) {
    cod = &header->component_cod[component_index];
  }
  component = &tile_components[component_index];
  if (res_index < 0 || (size_t)res_index >= component->res_level_count) {
    return 1;
  }
  if (separate_headers) {
    return jpx_parse_packet_separate_header(
        context,
        packet_headers,
        packet_header_size,
        packet_header_offset,
        packet_bodies,
        packet_body_size,
        packet_body_offset,
        cod,
        component->res_levels[res_index].subbands,
        component->res_levels[res_index].subband_count);
  }
  return nanopdf_jpx_parse_packet_simple(
      context,
      data,
      size,
      offset,
      cod,
      component->res_levels[res_index].subbands,
      component->res_levels[res_index].subband_count);
}

static int jpx_packet_source_has_more(
    int separate_headers,
    const size_t* offset,
    size_t size,
    const size_t* packet_header_offset,
    size_t packet_header_size) {
  if (separate_headers) {
    return packet_header_offset && *packet_header_offset < packet_header_size;
  }
  return offset && *offset < size;
}

static int jpx_parse_packet_progression_range(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t* offset,
    const uint8_t* packet_headers,
    size_t packet_header_size,
    size_t* packet_header_offset,
    const uint8_t* packet_bodies,
    size_t packet_body_size,
    size_t* packet_body_offset,
    const nanopdf_jpx_header* header,
    nanopdf_jpx_tile_component* tile_components,
    size_t tile_component_count,
    nanopdf_jpx_progression_order progression_order,
    int layer_start,
    int layer_end,
    int res_start,
    int res_end,
    size_t component_start,
    size_t component_end,
    int separate_headers) {
  int layer;
  if (!context || !data || !offset || !header || !tile_components ||
      tile_component_count == 0 || layer_start < 0 || layer_end < layer_start ||
      res_start < 0 || res_end < res_start || component_end < component_start) {
    return 0;
  }
  if (component_end > tile_component_count) {
    component_end = tile_component_count;
  }
  if (progression_order == NANOPDF_JPX_PROGRESSION_RLCP) {
    int res_index;
    for (res_index = res_start;
         res_index < res_end &&
             jpx_packet_source_has_more(
                 separate_headers, offset, size, packet_header_offset, packet_header_size);
         ++res_index) {
      for (layer = layer_start;
           layer < layer_end &&
               jpx_packet_source_has_more(
                   separate_headers, offset, size, packet_header_offset, packet_header_size);
           ++layer) {
        size_t component_index;
        for (component_index = component_start;
             component_index < component_end &&
                 jpx_packet_source_has_more(
                     separate_headers, offset, size, packet_header_offset, packet_header_size);
             ++component_index) {
          if (!jpx_parse_packet_for_component_resolution(
                  context,
                  data,
                  size,
                  offset,
                  packet_headers,
                  packet_header_size,
                  packet_header_offset,
                  packet_bodies,
                  packet_body_size,
                  packet_body_offset,
                  header,
                  tile_components,
                  tile_component_count,
                  component_index,
                  res_index,
                  separate_headers)) {
            return 0;
          }
        }
      }
    }
  } else if (progression_order == NANOPDF_JPX_PROGRESSION_RPCL ||
             progression_order == NANOPDF_JPX_PROGRESSION_PCRL) {
    int res_index;
    for (res_index = res_start;
         res_index < res_end &&
             jpx_packet_source_has_more(
                 separate_headers, offset, size, packet_header_offset, packet_header_size);
         ++res_index) {
      size_t component_index;
      for (component_index = component_start;
           component_index < component_end &&
               jpx_packet_source_has_more(
                   separate_headers, offset, size, packet_header_offset, packet_header_size);
           ++component_index) {
        for (layer = layer_start;
             layer < layer_end &&
                 jpx_packet_source_has_more(
                     separate_headers, offset, size, packet_header_offset, packet_header_size);
             ++layer) {
          if (!jpx_parse_packet_for_component_resolution(
                  context,
                  data,
                  size,
                  offset,
                  packet_headers,
                  packet_header_size,
                  packet_header_offset,
                  packet_bodies,
                  packet_body_size,
                  packet_body_offset,
                  header,
                  tile_components,
                  tile_component_count,
                  component_index,
                  res_index,
                  separate_headers)) {
            return 0;
          }
        }
      }
    }
  } else if (progression_order == NANOPDF_JPX_PROGRESSION_CPRL) {
    size_t component_index;
    for (component_index = component_start;
         component_index < component_end &&
             jpx_packet_source_has_more(
                 separate_headers, offset, size, packet_header_offset, packet_header_size);
         ++component_index) {
      int res_index;
      for (res_index = res_start;
           res_index < res_end &&
               jpx_packet_source_has_more(
                   separate_headers, offset, size, packet_header_offset, packet_header_size);
           ++res_index) {
        for (layer = layer_start;
             layer < layer_end &&
                 jpx_packet_source_has_more(
                     separate_headers, offset, size, packet_header_offset, packet_header_size);
             ++layer) {
          if (!jpx_parse_packet_for_component_resolution(
                  context,
                  data,
                  size,
                  offset,
                  packet_headers,
                  packet_header_size,
                  packet_header_offset,
                  packet_bodies,
                  packet_body_size,
                  packet_body_offset,
                  header,
                  tile_components,
                  tile_component_count,
                  component_index,
                  res_index,
                  separate_headers)) {
            return 0;
          }
        }
      }
    }
  } else {
    for (layer = layer_start;
         layer < layer_end &&
             jpx_packet_source_has_more(
                 separate_headers, offset, size, packet_header_offset, packet_header_size);
         ++layer) {
      int res_index;
      for (res_index = res_start;
           res_index < res_end &&
               jpx_packet_source_has_more(
                   separate_headers, offset, size, packet_header_offset, packet_header_size);
           ++res_index) {
        size_t component_index;
        for (component_index = component_start;
             component_index < component_end &&
                 jpx_packet_source_has_more(
                     separate_headers, offset, size, packet_header_offset, packet_header_size);
             ++component_index) {
          if (!jpx_parse_packet_for_component_resolution(
                  context,
                  data,
                  size,
                  offset,
                  packet_headers,
                  packet_header_size,
                  packet_header_offset,
                  packet_bodies,
                  packet_body_size,
                  packet_body_offset,
                  header,
                  tile_components,
                  tile_component_count,
                  component_index,
                  res_index,
                  separate_headers)) {
            return 0;
          }
        }
      }
    }
  }
  return 1;
}

static int jpx_decode_tile_data_common(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    const uint8_t* packet_headers,
    size_t packet_header_size,
    const uint8_t* packet_bodies,
    size_t packet_body_size,
    int separate_headers,
    const nanopdf_jpx_header* header,
    nanopdf_jpx_tile_component* tile_components,
    size_t tile_component_count) {
  size_t offset = 0;
  size_t packet_header_offset = 0;
  size_t packet_body_offset = 0;
  int layers;
  int res_count;
  if (!context || !header || !tile_components || tile_component_count == 0 ||
      (!separate_headers && !data) ||
      (separate_headers && (!packet_headers || !packet_bodies))) {
    return 0;
  }
  layers = header->cod.num_layers > 0 ? header->cod.num_layers : 1;
  res_count = (int)header->cod.num_decomp_levels + 1;
  if (header->poc_entries && header->poc_entry_count > 0u) {
    size_t i;
    for (i = 0;
         i < header->poc_entry_count &&
             jpx_packet_source_has_more(
                 separate_headers,
                 &offset,
                 size,
                 &packet_header_offset,
                 packet_header_size);
         ++i) {
      const nanopdf_jpx_poc_entry* entry = &header->poc_entries[i];
      int layer_end = entry->layer_end < (uint16_t)layers ? entry->layer_end : layers;
      int res_end = entry->resolution_end < (uint8_t)res_count
                        ? entry->resolution_end
                        : res_count;
      size_t component_end = entry->component_end < (uint32_t)tile_component_count
                                 ? (size_t)entry->component_end
                                 : tile_component_count;
      if ((int)entry->resolution_start >= res_end ||
          (size_t)entry->component_start >= component_end ||
          layer_end <= 0) {
        continue;
      }
      if (!jpx_parse_packet_progression_range(
              context,
              data,
              size,
              &offset,
              packet_headers,
              packet_header_size,
              &packet_header_offset,
              packet_bodies,
              packet_body_size,
              &packet_body_offset,
              header,
              tile_components,
              tile_component_count,
              entry->progression_order,
              0,
              layer_end,
              entry->resolution_start,
              res_end,
              (size_t)entry->component_start,
              component_end,
              separate_headers)) {
        return 0;
      }
    }
  } else if (!jpx_parse_packet_progression_range(
                 context,
                 data,
                 size,
                 &offset,
                 packet_headers,
                 packet_header_size,
                 &packet_header_offset,
                 packet_bodies,
                 packet_body_size,
                 &packet_body_offset,
                 header,
                 tile_components,
                 tile_component_count,
                 header->cod.progression_order,
                 0,
                 layers,
                 0,
                 res_count,
                 0,
                 tile_component_count,
                 separate_headers)) {
    return 0;
  }

  {
    size_t component_index;
    for (component_index = 0; component_index < tile_component_count; ++component_index) {
      if (!jpx_decode_tile_component_codeblocks(
              context, header, &tile_components[component_index], component_index)) {
        return 0;
      }
    }
  }
  return 1;
}

int nanopdf_jpx_decode_tile_data_simple(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    const nanopdf_jpx_header* header,
    nanopdf_jpx_tile_component* tile_components,
    size_t tile_component_count) {
  return jpx_decode_tile_data_common(
      context,
      data,
      size,
      NULL,
      0,
      NULL,
      0,
      0,
      header,
      tile_components,
      tile_component_count);
}

int nanopdf_jpx_decode_tile_data_with_packet_headers(
    nanopdf_context* context,
    const uint8_t* packet_headers,
    size_t packet_header_size,
    const uint8_t* packet_bodies,
    size_t packet_body_size,
    const nanopdf_jpx_header* header,
    nanopdf_jpx_tile_component* tile_components,
    size_t tile_component_count) {
  return jpx_decode_tile_data_common(
      context,
      packet_bodies,
      packet_body_size,
      packet_headers,
      packet_header_size,
      packet_bodies,
      packet_body_size,
      1,
      header,
      tile_components,
      tile_component_count);
}

int nanopdf_jpx_reconstruct_pixels(
    nanopdf_context* context,
    const nanopdf_jpx_header* header,
    nanopdf_jpx_tile_component* tile_components,
    size_t tile_component_count,
    uint8_t** out_pixels,
    size_t* out_size) {
  int32_t** component_ptrs = NULL;
  uint8_t* bit_depths = NULL;
  uint8_t* pixels = NULL;
  size_t width;
  size_t height;
  size_t num_pixels;
  size_t output_size;
  size_t i;

  if (out_pixels) {
    *out_pixels = NULL;
  }
  if (out_size) {
    *out_size = 0;
  }
  if (!context || !header || !tile_components || tile_component_count == 0 ||
      !out_pixels || !out_size || nanopdf_jpx_image_width(header) == 0 ||
      nanopdf_jpx_image_height(header) == 0) {
    return 0;
  }
  width = nanopdf_jpx_image_width(header);
  height = nanopdf_jpx_image_height(header);
  if (width > SIZE_MAX / height) {
    return 0;
  }
  num_pixels = width * height;
  if (tile_component_count > SIZE_MAX / num_pixels) {
    return 0;
  }
  output_size = num_pixels * tile_component_count;

  component_ptrs = (int32_t**)nanopdf__allocator_alloc(
      &context->allocator, tile_component_count * sizeof(component_ptrs[0]));
  bit_depths = (uint8_t*)nanopdf__allocator_alloc(
      &context->allocator, tile_component_count * sizeof(bit_depths[0]));
  pixels = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, output_size);
  if (!component_ptrs || !bit_depths || !pixels) {
    nanopdf__allocator_free(&context->allocator, component_ptrs);
    nanopdf__allocator_free(&context->allocator, bit_depths);
    nanopdf__allocator_free(&context->allocator, pixels);
    return 0;
  }

  for (i = 0; i < tile_component_count; ++i) {
    nanopdf_jpx_tile_component* component = &tile_components[i];
    const nanopdf_jpx_cod_params* cod = &header->cod;
    size_t component_width =
        component->width > 0 ? (size_t)component->width : width;
    size_t component_height =
        component->height > 0 ? (size_t)component->height : height;
    size_t component_pixels;
    if (component_width == 0 || component_height == 0 ||
        component_width > SIZE_MAX / component_height) {
      nanopdf__allocator_free(&context->allocator, component_ptrs);
      nanopdf__allocator_free(&context->allocator, bit_depths);
      nanopdf__allocator_free(&context->allocator, pixels);
      return 0;
    }
    component_pixels = component_width * component_height;
    if (!component->coeffs || component->coeff_count < component_pixels) {
      nanopdf__allocator_free(&context->allocator, component_ptrs);
      nanopdf__allocator_free(&context->allocator, bit_depths);
      nanopdf__allocator_free(&context->allocator, pixels);
      return 0;
    }
    if (header->has_component_cod && header->component_cod &&
        i < header->siz.num_components && header->has_component_cod[i]) {
      cod = &header->component_cod[i];
    }
    if (cod->num_decomp_levels > 0) {
      if (cod->wavelet == NANOPDF_JPX_WAVELET_REVERSIBLE_5_3) {
        if (!nanopdf_jpx_inverse_dwt_53(
                context,
                component->coeffs,
                (int)component_width,
                (int)component_height,
                cod->num_decomp_levels)) {
          nanopdf__allocator_free(&context->allocator, component_ptrs);
          nanopdf__allocator_free(&context->allocator, bit_depths);
          nanopdf__allocator_free(&context->allocator, pixels);
          return 0;
        }
      } else {
        float* float_coeffs = (float*)nanopdf__allocator_alloc(
            &context->allocator, component_pixels * sizeof(float_coeffs[0]));
        size_t j;
        if (!float_coeffs) {
          nanopdf__allocator_free(&context->allocator, component_ptrs);
          nanopdf__allocator_free(&context->allocator, bit_depths);
          nanopdf__allocator_free(&context->allocator, pixels);
          return 0;
        }
        for (j = 0; j < component_pixels; ++j) {
          float_coeffs[j] = (float)component->coeffs[j];
        }
        if (!nanopdf_jpx_inverse_dwt_97(
                context,
                float_coeffs,
                (int)component_width,
                (int)component_height,
                cod->num_decomp_levels)) {
          nanopdf__allocator_free(&context->allocator, float_coeffs);
          nanopdf__allocator_free(&context->allocator, component_ptrs);
          nanopdf__allocator_free(&context->allocator, bit_depths);
          nanopdf__allocator_free(&context->allocator, pixels);
          return 0;
        }
        for (j = 0; j < component_pixels; ++j) {
          component->coeffs[j] = (int32_t)(float_coeffs[j] >= 0.0f ?
              float_coeffs[j] + 0.5f : float_coeffs[j] - 0.5f);
        }
        nanopdf__allocator_free(&context->allocator, float_coeffs);
      }
    }
    component_ptrs[i] = component->coeffs;
    bit_depths[i] = i < header->siz.num_components ?
        header->siz.components[i].bit_depth : 8u;
  }

  if (header->cod.mct && tile_component_count >= 3) {
    for (i = 0; i < 3u; ++i) {
      if ((tile_components[i].width > 0 && (size_t)tile_components[i].width != width) ||
          (tile_components[i].height > 0 && (size_t)tile_components[i].height != height)) {
        nanopdf__allocator_free(&context->allocator, component_ptrs);
        nanopdf__allocator_free(&context->allocator, bit_depths);
        nanopdf__allocator_free(&context->allocator, pixels);
        return 0;
      }
    }
    nanopdf_jpx_apply_inverse_mct(
        component_ptrs,
        tile_component_count,
        num_pixels,
        header->cod.wavelet);
  }
  jpx_sampled_components_to_pixels(
      header, tile_components, tile_component_count, bit_depths, pixels);

  nanopdf__allocator_free(&context->allocator, component_ptrs);
  nanopdf__allocator_free(&context->allocator, bit_depths);
  *out_pixels = pixels;
  *out_size = output_size;
  return 1;
}
