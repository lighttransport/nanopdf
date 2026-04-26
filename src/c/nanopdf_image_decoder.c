#include "nanopdf_image_decoder.h"
#include "nanopdf_jbig2.h"
#include "nanopdf_jpx.h"

#include <limits.h>
#include <string.h>

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "../third_party/stb_image.h"

#define NANOPDF_FAX_MAX_IMAGE_DIMENSION 65535

static int nanopdf__jpx_indexed_data_total_size(
    const nanopdf_jpx_indexed_data* markers,
    size_t marker_count,
    size_t* total_size) {
  size_t i;
  if (!total_size) {
    return 0;
  }
  for (i = 0; i < marker_count; ++i) {
    if (markers[i].size > SIZE_MAX - *total_size) {
      return 0;
    }
    *total_size += markers[i].size;
  }
  return 1;
}

static void nanopdf__jpx_append_indexed_data_sorted(
    uint8_t* dest,
    size_t* offset,
    const nanopdf_jpx_indexed_data* markers,
    size_t marker_count) {
  uint16_t marker_index;
  if (!dest || !offset || !markers) {
    return;
  }
  for (marker_index = 0; marker_index <= 255u; ++marker_index) {
    size_t i;
    for (i = 0; i < marker_count; ++i) {
      if (markers[i].index == (uint8_t)marker_index && markers[i].size > 0u) {
        memcpy(dest + *offset, markers[i].data, markers[i].size);
        *offset += markers[i].size;
      }
    }
  }
}

static int nanopdf__jpx_append_bytes(
    nanopdf_context* context,
    uint8_t** buffer,
    size_t* size,
    const uint8_t* data,
    size_t data_size) {
  uint8_t* resized;
  if (!context || !buffer || !size || (!data && data_size > 0u)) {
    return 0;
  }
  if (data_size == 0u) {
    return 1;
  }
  if (*size > SIZE_MAX - data_size) {
    return 0;
  }
  resized = (uint8_t*)nanopdf__allocator_realloc(
      &context->allocator, *buffer, *size + data_size);
  if (!resized) {
    return 0;
  }
  memcpy(resized + *size, data, data_size);
  *buffer = resized;
  *size += data_size;
  return 1;
}

static int nanopdf__jpx_append_indexed_data_to_buffer(
    nanopdf_context* context,
    uint8_t** buffer,
    size_t* size,
    const nanopdf_jpx_indexed_data* markers,
    size_t marker_count) {
  size_t marker_data_size = 0;
  size_t offset;
  uint8_t* resized;
  if (!context || !buffer || !size) {
    return 0;
  }
  if (!nanopdf__jpx_indexed_data_total_size(markers, marker_count, &marker_data_size)) {
    return 0;
  }
  if (marker_data_size == 0u) {
    return 1;
  }
  if (*size > SIZE_MAX - marker_data_size) {
    return 0;
  }
  resized = (uint8_t*)nanopdf__allocator_realloc(
      &context->allocator, *buffer, *size + marker_data_size);
  if (!resized) {
    return 0;
  }
  *buffer = resized;
  offset = *size;
  nanopdf__jpx_append_indexed_data_sorted(
      *buffer, &offset, markers, marker_count);
  *size = offset;
  return 1;
}

static uint32_t nanopdf__jpx_read_be32(const uint8_t* data) {
  return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
         ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

static int nanopdf__jpx_build_sorted_indexed_data(
    nanopdf_context* context,
    const nanopdf_jpx_indexed_data* markers,
    size_t marker_count,
    uint8_t** out_data,
    size_t* out_size) {
  size_t total_size = 0;
  size_t offset = 0;
  uint8_t* data = NULL;
  if (!context || !out_data || !out_size) {
    return 0;
  }
  *out_data = NULL;
  *out_size = 0;
  if (!nanopdf__jpx_indexed_data_total_size(markers, marker_count, &total_size)) {
    return 0;
  }
  if (total_size == 0u) {
    return 1;
  }
  data = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, total_size);
  if (!data) {
    return 0;
  }
  nanopdf__jpx_append_indexed_data_sorted(data, &offset, markers, marker_count);
  *out_data = data;
  *out_size = offset;
  return 1;
}

static int nanopdf__jpx_append_ppm_tile_part_headers(
    nanopdf_context* context,
    uint8_t** buffer,
    size_t* size,
    const nanopdf_jpx_indexed_data* markers,
    size_t marker_count,
    size_t wanted_tile_part_sequence,
    int* fallback_appended) {
  uint8_t* ppm_data = NULL;
  size_t ppm_size = 0;
  size_t offset = 0;
  size_t block_index = 0;
  int structured_ppm = 0;
  int selected_block = 0;
  int result = 1;
  if (!context || !buffer || !size || !fallback_appended) {
    return 0;
  }
  if (!nanopdf__jpx_build_sorted_indexed_data(
          context, markers, marker_count, &ppm_data, &ppm_size)) {
    return 0;
  }
  while (offset + 4u <= ppm_size) {
    uint32_t block_size = nanopdf__jpx_read_be32(ppm_data + offset);
    offset += 4u;
    if ((size_t)block_size > ppm_size - offset) {
      structured_ppm = 0;
      break;
    }
    structured_ppm = 1;
    if (block_index == wanted_tile_part_sequence) {
      result = nanopdf__jpx_append_bytes(
          context, buffer, size, ppm_data + offset, (size_t)block_size);
      selected_block = 1;
      break;
    }
    offset += (size_t)block_size;
    block_index++;
  }
  if (result && !selected_block && (!structured_ppm || offset != ppm_size) &&
      !*fallback_appended) {
    result = nanopdf__jpx_append_bytes(context, buffer, size, ppm_data, ppm_size);
    *fallback_appended = result ? 1 : 0;
  }
  nanopdf__allocator_free(&context->allocator, ppm_data);
  return result;
}

static uint32_t nanopdf__jpx_ceil_div_u32(uint32_t value, uint32_t divisor) {
  if (divisor == 0u) {
    return 0u;
  }
  return value / divisor + (uint32_t)((value % divisor) != 0u);
}

static uint32_t nanopdf__jpx_tile_count_x(const nanopdf_jpx_header* header) {
  uint32_t end_x;
  if (!header || header->siz.tile_width == 0u ||
      header->siz.width <= header->siz.tile_x_offset) {
    return 0u;
  }
  end_x = header->siz.width - header->siz.tile_x_offset;
  return nanopdf__jpx_ceil_div_u32(end_x, header->siz.tile_width);
}

static uint32_t nanopdf__jpx_tile_count_y(const nanopdf_jpx_header* header) {
  uint32_t end_y;
  if (!header || header->siz.tile_height == 0u ||
      header->siz.height <= header->siz.tile_y_offset) {
    return 0u;
  }
  end_y = header->siz.height - header->siz.tile_y_offset;
  return nanopdf__jpx_ceil_div_u32(end_y, header->siz.tile_height);
}

static int nanopdf__jpx_tile_bounds(
    const nanopdf_jpx_header* header,
    uint32_t tile_index,
    uint32_t* out_x0,
    uint32_t* out_y0,
    uint32_t* out_x1,
    uint32_t* out_y1) {
  uint32_t tiles_x = nanopdf__jpx_tile_count_x(header);
  uint32_t tile_x;
  uint32_t tile_y;
  uint32_t x0;
  uint32_t y0;
  uint32_t x1;
  uint32_t y1;
  if (!header || !out_x0 || !out_y0 || !out_x1 || !out_y1 || tiles_x == 0u) {
    return 0;
  }
  tile_x = tile_index % tiles_x;
  tile_y = tile_index / tiles_x;
  if (header->siz.tile_width != 0u &&
      tile_x > (UINT32_MAX - header->siz.tile_x_offset) / header->siz.tile_width) {
    return 0;
  }
  if (header->siz.tile_height != 0u &&
      tile_y > (UINT32_MAX - header->siz.tile_y_offset) / header->siz.tile_height) {
    return 0;
  }
  x0 = header->siz.tile_x_offset + tile_x * header->siz.tile_width;
  y0 = header->siz.tile_y_offset + tile_y * header->siz.tile_height;
  if (x0 > UINT32_MAX - header->siz.tile_width ||
      y0 > UINT32_MAX - header->siz.tile_height) {
    return 0;
  }
  x1 = x0 + header->siz.tile_width;
  y1 = y0 + header->siz.tile_height;
  if (x0 < header->siz.x_offset) {
    x0 = header->siz.x_offset;
  }
  if (y0 < header->siz.y_offset) {
    y0 = header->siz.y_offset;
  }
  if (x1 > header->siz.width) {
    x1 = header->siz.width;
  }
  if (y1 > header->siz.height) {
    y1 = header->siz.height;
  }
  if (x1 <= x0 || y1 <= y0) {
    return 0;
  }
  *out_x0 = x0;
  *out_y0 = y0;
  *out_x1 = x1;
  *out_y1 = y1;
  return 1;
}

static uint32_t nanopdf__jpx_component_extent(
    const nanopdf_jpx_header* header,
    size_t component_index,
    uint32_t ref_start,
    uint32_t ref_end,
    int is_x) {
  uint32_t sep = 1u;
  uint32_t start;
  uint32_t end;
  if (!header || !header->siz.components ||
      component_index >= header->siz.num_components || ref_end <= ref_start) {
    return 0u;
  }
  sep = is_x ? header->siz.components[component_index].x_separation
             : header->siz.components[component_index].y_separation;
  if (sep == 0u) {
    sep = 1u;
  }
  start = nanopdf__jpx_ceil_div_u32(ref_start, sep);
  end = nanopdf__jpx_ceil_div_u32(ref_end, sep);
  return end > start ? end - start : 0u;
}

static int nanopdf__jpx_collect_tile_packet_stream(
    nanopdf_context* context,
    const uint8_t* codestream,
    size_t codestream_size,
    size_t first_tile_part_offset,
    nanopdf_jpx_header* header,
    uint32_t wanted_tile_index,
    uint8_t** out_packet_headers,
    size_t* out_packet_header_size,
    uint8_t** out_packet_bodies,
    size_t* out_packet_body_size) {
  size_t offset = first_tile_part_offset;
  size_t tile_part_sequence = 0;
  int ppm_fallback_appended = 0;
  if (!context || !codestream || !header || !out_packet_headers ||
      !out_packet_header_size || !out_packet_bodies || !out_packet_body_size) {
    return 0;
  }
  *out_packet_headers = NULL;
  *out_packet_header_size = 0;
  *out_packet_bodies = NULL;
  *out_packet_body_size = 0;
  while (offset + 12u <= codestream_size) {
    nanopdf_jpx_tile_part_header tile_part;
    size_t tile_data_offset = 0;
    size_t tile_data_size = 0;
    size_t next_offset;
    size_t old_ppt_marker_count = header->ppt_marker_count;
    if (!nanopdf_jpx_parse_tile_part_header(
            codestream, codestream_size, offset, &tile_part, &tile_data_offset,
            &tile_data_size)) {
      return 0;
    }
    if ((uint32_t)tile_part.tile_index == wanted_tile_index) {
      if (!nanopdf_jpx_parse_tile_part_markers(
              context, codestream, codestream_size, offset, header)) {
        return 0;
      }
      if (header->ppt_marker_count > old_ppt_marker_count) {
        if (!nanopdf__jpx_append_indexed_data_to_buffer(
                context,
                out_packet_headers,
                out_packet_header_size,
                header->ppt_markers + old_ppt_marker_count,
                header->ppt_marker_count - old_ppt_marker_count)) {
          return 0;
        }
      } else if (header->ppt_marker_count == 0u && header->ppm_marker_count > 0u) {
        if (!nanopdf__jpx_append_ppm_tile_part_headers(
                context,
                out_packet_headers,
                out_packet_header_size,
                header->ppm_markers,
                header->ppm_marker_count,
                tile_part_sequence,
                &ppm_fallback_appended)) {
          return 0;
        }
      }
      if (!nanopdf__jpx_append_bytes(
              context,
              out_packet_bodies,
              out_packet_body_size,
              codestream + tile_data_offset,
              tile_data_size)) {
        return 0;
      }
    }
    if (tile_part.tile_part_length > 0u) {
      next_offset = offset + (size_t)tile_part.tile_part_length;
    } else {
      next_offset = tile_data_offset + tile_data_size;
    }
    if (next_offset <= offset || next_offset + 2u > codestream_size ||
        codestream[next_offset] != 0xffu || codestream[next_offset + 1u] != 0x90u) {
      break;
    }
    tile_part_sequence++;
    offset = next_offset;
  }
  return *out_packet_body_size > 0u ||
         (*out_packet_header_size > 0u && header->ppm_marker_count > 0u);
}

static const uint8_t k_fax_black_run_ins[] = {
    0,    2,    0x02, 3,    0,    0x03, 2,    0,    2,    0x02, 1,    0,
    0x03, 4,    0,    2,    0x02, 6,    0,    0x03, 5,    0,    1,    0x03,
    7,    0,    2,    0x04, 9,    0,    0x05, 8,    0,    3,    0x04, 10,
    0,    0x05, 11,   0,    0x07, 12,   0,    2,    0x04, 13,   0,    0x07,
    14,   0,    1,    0x18, 15,   0,    5,    0x08, 18,   0,    0x0f, 64,
    0,    0x17, 16,   0,    0x18, 17,   0,    0x37, 0,    0,    10,   0x08,
    0x00, 0x07, 0x0c, 0x40, 0x07, 0x0d, 0x80, 0x07, 0x17, 24,   0,    0x18,
    25,   0,    0x28, 23,   0,    0x37, 22,   0,    0x67, 19,   0,    0x68,
    20,   0,    0x6c, 21,   0,    54,   0x12, 1984 % 256, 1984 / 256,
    0x13, 2048 % 256, 2048 / 256, 0x14, 2112 % 256, 2112 / 256,
    0x15, 2176 % 256, 2176 / 256, 0x16, 2240 % 256, 2240 / 256,
    0x17, 2304 % 256, 2304 / 256, 0x1c, 2368 % 256, 2368 / 256,
    0x1d, 2432 % 256, 2432 / 256, 0x1e, 2496 % 256, 2496 / 256,
    0x1f, 2560 % 256, 2560 / 256, 0x24, 52,   0,    0x27, 55,   0,
    0x28, 56,   0,    0x2b, 59,   0,    0x2c, 60,   0,    0x33, 320 % 256,
    320 / 256, 0x34, 384 % 256, 384 / 256, 0x35, 448 % 256, 448 / 256,
    0x37, 53,   0,    0x38, 54,   0,    0x52, 50,   0,    0x53, 51,   0,
    0x54, 44,   0,    0x55, 45,   0,    0x56, 46,   0,    0x57, 47,   0,
    0x58, 57,   0,    0x59, 58,   0,    0x5a, 61,   0,    0x5b, 256 % 256,
    256 / 256, 0x64, 48,   0,    0x65, 49,   0,    0x66, 62,   0,
    0x67, 63,   0,    0x68, 30,   0,    0x69, 31,   0,    0x6a, 32,   0,
    0x6b, 33,   0,    0x6c, 40,   0,    0x6d, 41,   0,    0xc8, 128,  0,
    0xc9, 192,  0,    0xca, 26,   0,    0xcb, 27,   0,    0xcc, 28,   0,
    0xcd, 29,   0,    0xd2, 34,   0,    0xd3, 35,   0,    0xd4, 36,   0,
    0xd5, 37,   0,    0xd6, 38,   0,    0xd7, 39,   0,    0xda, 42,   0,
    0xdb, 43,   0,    20,   0x4a, 640 % 256, 640 / 256, 0x4b, 704 % 256,
    704 / 256, 0x4c, 768 % 256, 768 / 256, 0x4d, 832 % 256, 832 / 256,
    0x52, 1280 % 256, 1280 / 256, 0x53, 1344 % 256, 1344 / 256,
    0x54, 1408 % 256, 1408 / 256, 0x55, 1472 % 256, 1472 / 256,
    0x5a, 1536 % 256, 1536 / 256, 0x5b, 1600 % 256, 1600 / 256,
    0x64, 1664 % 256, 1664 / 256, 0x65, 1728 % 256, 1728 / 256,
    0x6c, 512 % 256, 512 / 256, 0x6d, 576 % 256, 576 / 256,
    0x72, 896 % 256, 896 / 256, 0x73, 960 % 256, 960 / 256,
    0x74, 1024 % 256, 1024 / 256, 0x75, 1088 % 256, 1088 / 256,
    0x76, 1152 % 256, 1152 / 256, 0x77, 1216 % 256, 1216 / 256, 0xff};

static const uint8_t k_fax_white_run_ins[] = {
    0,    0,    0,    6,    0x07, 2,    0,    0x08, 3,    0,    0x0B, 4,
    0,    0x0C, 5,    0,    0x0E, 6,    0,    0x0F, 7,    0,    6,    0x07,
    10,   0,    0x08, 11,   0,    0x12, 128,  0,    0x13, 8,    0,    0x14,
    9,    0,    0x1b, 64,   0,    9,    0x03, 13,   0,    0x07, 1,    0,
    0x08, 12,   0,    0x17, 192,  0,    0x18, 1664 % 256, 1664 / 256,
    0x2a, 16,   0,    0x2B, 17,   0,    0x34, 14,   0,    0x35, 15,   0,
    12,   0x03, 22,   0,    0x04, 23,   0,    0x08, 20,   0,    0x0c, 19,
    0,    0x13, 26,   0,    0x17, 21,   0,    0x18, 28,   0,    0x24, 27,
    0,    0x27, 18,   0,    0x28, 24,   0,    0x2B, 25,   0,    0x37,
    256 % 256, 256 / 256, 42,   0x02, 29,   0,    0x03, 30,   0,    0x04,
    45,   0,    0x05, 46,   0,    0x0a, 47,   0,    0x0b, 48,   0,    0x12,
    33,   0,    0x13, 34,   0,    0x14, 35,   0,    0x15, 36,   0,    0x16,
    37,   0,    0x17, 38,   0,    0x1a, 31,   0,    0x1b, 32,   0,    0x24,
    53,   0,    0x25, 54,   0,    0x28, 39,   0,    0x29, 40,   0,    0x2a,
    41,   0,    0x2b, 42,   0,    0x2c, 43,   0,    0x2d, 44,   0,    0x32,
    61,   0,    0x33, 62,   0,    0x34, 63,   0,    0x35, 0,    0,    0x36,
    320 % 256, 320 / 256, 0x37, 384 % 256, 384 / 256, 0x4a, 59,   0,
    0x4b, 60,   0,    0x52, 49,   0,    0x53, 50,   0,    0x54, 51,   0,
    0x55, 52,   0,    0x58, 55,   0,    0x59, 56,   0,    0x5a, 57,   0,
    0x5b, 58,   0,    0x64, 448 % 256, 448 / 256, 0x65, 512 % 256,
    512 / 256, 0x67, 640 % 256, 640 / 256, 0x68, 576 % 256, 576 / 256,
    16,   0x98, 1472 % 256, 1472 / 256, 0x99, 1536 % 256, 1536 / 256,
    0x9a, 1600 % 256, 1600 / 256, 0x9b, 1728 % 256, 1728 / 256,
    0xcc, 704 % 256, 704 / 256, 0xcd, 768 % 256, 768 / 256,
    0xd2, 832 % 256, 832 / 256, 0xd3, 896 % 256, 896 / 256,
    0xd4, 960 % 256, 960 / 256, 0xd5, 1024 % 256, 1024 / 256,
    0xd6, 1088 % 256, 1088 / 256, 0xd7, 1152 % 256, 1152 / 256,
    0xd8, 1216 % 256, 1216 / 256, 0xd9, 1280 % 256, 1280 / 256,
    0xda, 1344 % 256, 1344 / 256, 0xdb, 1408 % 256, 1408 / 256,
    0,    3,    0x08, 1792 % 256, 1792 / 256, 0x0c, 1856 % 256,
    1856 / 256, 0x0d, 1920 % 256, 1920 / 256, 10,   0x12, 1984 % 256,
    1984 / 256, 0x13, 2048 % 256, 2048 / 256, 0x14, 2112 % 256,
    2112 / 256, 0x15, 2176 % 256, 2176 / 256, 0x16, 2240 % 256,
    2240 / 256, 0x17, 2304 % 256, 2304 / 256, 0x1c, 2368 % 256,
    2368 / 256, 0x1d, 2432 % 256, 2432 / 256, 0x1e, 2496 % 256,
    2496 / 256, 0x1f, 2560 % 256, 2560 / 256, 0xff};

static uint32_t fax_src_bit_size(size_t src_size) {
  return src_size > (UINT_MAX / 8u) ? UINT_MAX : (uint32_t)(src_size * 8u);
}

static int fax_leading_one_pos(uint8_t data) {
  int i = 0;
  if (data == 0) {
    return 8;
  }
  for (i = 0; i < 8; ++i) {
    if (data & (uint8_t)(0x80u >> i)) {
      return i;
    }
  }
  return 8;
}

static int fax_next_bit(const uint8_t* src_buf, size_t src_size, uint32_t* bitpos) {
  uint32_t pos = 0;
  if (*bitpos / 8u >= src_size) {
    return 0;
  }
  pos = (*bitpos)++;
  return (src_buf[pos / 8u] & (uint8_t)(1u << (7u - (pos % 8u)))) != 0;
}

static int fax_find_bit(
    const uint8_t* data_buf,
    size_t data_size,
    int max_pos,
    int start_pos,
    int bit) {
  const uint8_t bit_xor = bit ? 0x00u : 0xffu;
  int bit_offset = 0;
  int max_byte = 0;
  int byte_pos = 0;

  if (start_pos < 0) {
    start_pos = 0;
  }
  if (start_pos >= max_pos) {
    return max_pos;
  }

  bit_offset = start_pos % 8;
  if (bit_offset) {
    byte_pos = start_pos / 8;
    if ((size_t)byte_pos >= data_size) {
      return max_pos;
    }
    {
      uint8_t data = (uint8_t)((data_buf[byte_pos] ^ bit_xor) & (0xffu >> bit_offset));
      if (data) {
        int result = byte_pos * 8 + fax_leading_one_pos(data);
        return result < max_pos ? result : max_pos;
      }
    }
    start_pos += 7;
  }

  max_byte = (max_pos + 7) / 8;
  byte_pos = start_pos / 8;
  while (byte_pos < max_byte && (size_t)byte_pos < data_size) {
    uint8_t data = (uint8_t)(data_buf[byte_pos] ^ bit_xor);
    if (data) {
      int result = byte_pos * 8 + fax_leading_one_pos(data);
      return result < max_pos ? result : max_pos;
    }
    ++byte_pos;
  }
  return max_pos;
}

static void fax_g4_find_b1_b2(
    const uint8_t* ref_buf,
    size_t ref_size,
    int columns,
    int a0,
    int a0color,
    int* b1,
    int* b2) {
  int first_bit = (a0 < 0) ? 1 :
      ((size_t)(a0 / 8) < ref_size &&
       (ref_buf[a0 / 8] & (uint8_t)(1u << (7u - (uint32_t)(a0 % 8)))) != 0);
  *b1 = fax_find_bit(ref_buf, ref_size, columns, a0 + 1, !first_bit);
  if (*b1 >= columns) {
    *b1 = columns;
    *b2 = columns;
    return;
  }
  if (first_bit == !a0color) {
    *b1 = fax_find_bit(ref_buf, ref_size, columns, *b1 + 1, first_bit);
    first_bit = !first_bit;
  }
  if (*b1 >= columns) {
    *b1 = columns;
    *b2 = columns;
    return;
  }
  *b2 = fax_find_bit(ref_buf, ref_size, columns, *b1 + 1, first_bit);
}

static void fax_fill_bits(
    uint8_t* dest_buf,
    size_t dest_size,
    int columns,
    int startpos,
    int endpos) {
  uint32_t first_byte = 0;
  uint32_t last_byte = 0;
  int i = 0;

  if (startpos < 0) {
    startpos = 0;
  }
  if (endpos < 0) {
    endpos = 0;
  }
  if (endpos > columns) {
    endpos = columns;
  }
  if (startpos >= endpos) {
    return;
  }

  first_byte = (uint32_t)(startpos / 8);
  last_byte = (uint32_t)((endpos - 1) / 8);
  if (first_byte >= dest_size) {
    return;
  }
  if (last_byte >= dest_size) {
    last_byte = (uint32_t)(dest_size - 1);
  }

  if (first_byte == last_byte) {
    for (i = startpos % 8; i <= (endpos - 1) % 8; ++i) {
      dest_buf[first_byte] = (uint8_t)(dest_buf[first_byte] - (uint8_t)(1u << (7 - i)));
    }
    return;
  }

  for (i = startpos % 8; i < 8; ++i) {
    dest_buf[first_byte] = (uint8_t)(dest_buf[first_byte] - (uint8_t)(1u << (7 - i)));
  }
  for (i = 0; i <= (endpos - 1) % 8; ++i) {
    dest_buf[last_byte] = (uint8_t)(dest_buf[last_byte] - (uint8_t)(1u << (7 - i)));
  }
  if (last_byte > first_byte + 1) {
    memset(dest_buf + first_byte + 1, 0, last_byte - first_byte - 1);
  }
}

static int fax_get_run(
    const uint8_t* ins_array,
    const uint8_t* src_buf,
    size_t src_size,
    uint32_t* bitpos) {
  uint32_t bitsize = fax_src_bit_size(src_size);
  uint32_t code = 0;
  int ins_off = 0;

  for (;;) {
    uint8_t ins = ins_array[ins_off++];
    int next_off = 0;
    if (ins == 0xffu || *bitpos >= bitsize) {
      return -1;
    }

    code <<= 1u;
    if (src_buf[*bitpos / 8u] & (uint8_t)(1u << (7u - (*bitpos % 8u)))) {
      ++code;
    }
    ++(*bitpos);

    next_off = ins_off + ins * 3;
    for (; ins_off < next_off; ins_off += 3) {
      if (ins_array[ins_off] == code) {
        return ins_array[ins_off + 1] + ins_array[ins_off + 2] * 256;
      }
    }
  }
}

static void fax_g4_get_row(
    const uint8_t* src_buf,
    size_t src_size,
    uint32_t* bitpos,
    uint8_t* dest_buf,
    size_t dest_size,
    const uint8_t* ref_buf,
    size_t ref_size,
    int columns) {
  int a0 = -1;
  int a0color = 1;
  uint32_t bitsize = fax_src_bit_size(src_size);

  for (;;) {
    int a1 = 0;
    int a2 = 0;
    int b1 = 0;
    int b2 = 0;
    int v_delta = 0;

    if (*bitpos >= bitsize) {
      return;
    }
    fax_g4_find_b1_b2(ref_buf, ref_size, columns, a0, a0color, &b1, &b2);

    if (!fax_next_bit(src_buf, src_size, bitpos)) {
      int bit1 = 0;
      int bit2 = 0;
      if (*bitpos >= bitsize) {
        return;
      }
      bit1 = fax_next_bit(src_buf, src_size, bitpos);
      if (*bitpos >= bitsize) {
        return;
      }
      bit2 = fax_next_bit(src_buf, src_size, bitpos);
      if (bit1) {
        v_delta = bit2 ? 1 : -1;
      } else if (bit2) {
        int run_len1 = 0;
        int run_len2 = 0;
        for (;;) {
          int run = fax_get_run(
              a0color ? k_fax_white_run_ins : k_fax_black_run_ins,
              src_buf,
              src_size,
              bitpos);
          if (run < 0) {
            break;
          }
          run_len1 += run;
          if (run < 64) {
            break;
          }
        }
        if (a0 < 0) {
          ++run_len1;
        }
        a1 = a0 + run_len1;
        if (!a0color) {
          fax_fill_bits(dest_buf, dest_size, columns, a0, a1);
        }

        for (;;) {
          int run = fax_get_run(
              a0color ? k_fax_black_run_ins : k_fax_white_run_ins,
              src_buf,
              src_size,
              bitpos);
          if (run < 0) {
            break;
          }
          run_len2 += run;
          if (run < 64) {
            break;
          }
        }
        a2 = a1 + run_len2;
        if (a0color) {
          fax_fill_bits(dest_buf, dest_size, columns, a1, a2);
        }
        a0 = a2;
        if (a0 < columns) {
          continue;
        }
        return;
      } else {
        int next_bit1 = 0;
        int next_bit2 = 0;
        if (*bitpos >= bitsize) {
          return;
        }
        if (fax_next_bit(src_buf, src_size, bitpos)) {
          if (!a0color) {
            fax_fill_bits(dest_buf, dest_size, columns, a0, b2);
          }
          if (b2 >= columns) {
            return;
          }
          a0 = b2;
          continue;
        }
        if (*bitpos >= bitsize) {
          return;
        }
        next_bit1 = fax_next_bit(src_buf, src_size, bitpos);
        if (*bitpos >= bitsize) {
          return;
        }
        next_bit2 = fax_next_bit(src_buf, src_size, bitpos);
        if (next_bit1) {
          v_delta = next_bit2 ? 2 : -2;
        } else if (next_bit2) {
          if (*bitpos >= bitsize) {
            return;
          }
          v_delta = fax_next_bit(src_buf, src_size, bitpos) ? 3 : -3;
        } else {
          if (*bitpos >= bitsize) {
            return;
          }
          if (fax_next_bit(src_buf, src_size, bitpos)) {
            *bitpos += 3u;
            continue;
          }
          *bitpos += 5u;
          return;
        }
      }
    }

    a1 = b1 + v_delta;
    if (!a0color) {
      fax_fill_bits(dest_buf, dest_size, columns, a0, a1);
    }
    if (a1 >= columns || a0 >= a1) {
      return;
    }
    a0 = a1;
    a0color = !a0color;
  }
}

static void fax_skip_eol(const uint8_t* src_buf, size_t src_size, uint32_t* bitpos) {
  uint32_t bitsize = fax_src_bit_size(src_size);
  uint32_t startbit = *bitpos;
  while (*bitpos < bitsize) {
    if (!fax_next_bit(src_buf, src_size, bitpos)) {
      continue;
    }
    if (*bitpos - startbit <= 11u) {
      *bitpos = startbit;
    }
    return;
  }
}

static void fax_get_1d_line(
    const uint8_t* src_buf,
    size_t src_size,
    uint32_t* bitpos,
    uint8_t* dest_buf,
    size_t dest_size,
    int columns) {
  uint32_t bitsize = fax_src_bit_size(src_size);
  int color = 1;
  int startpos = 0;

  for (;;) {
    int run_len = 0;
    if (*bitpos >= bitsize) {
      return;
    }
    for (;;) {
      int run = fax_get_run(
          color ? k_fax_white_run_ins : k_fax_black_run_ins,
          src_buf,
          src_size,
          bitpos);
      if (run < 0) {
        while (*bitpos < bitsize) {
          if (fax_next_bit(src_buf, src_size, bitpos)) {
            return;
          }
        }
        return;
      }
      run_len += run;
      if (run < 64) {
        break;
      }
    }
    if (!color) {
      fax_fill_bits(dest_buf, dest_size, columns, startpos, startpos + run_len);
    }
    startpos += run_len;
    if (startpos >= columns) {
      break;
    }
    color = !color;
  }
}

static void fax_invert_buffer(uint8_t* buf, size_t size) {
  size_t i = 0;
  for (i = 0; i < size; ++i) {
    buf[i] = (uint8_t)~buf[i];
  }
}

nanopdf_status nanopdf__decode_dct(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    uint8_t** out_data,
    size_t* out_size) {
  int width = 0;
  int height = 0;
  int channels = 0;
  size_t decoded_size = 0;
  unsigned char* decoded = NULL;

  if (!context || !input || input_size == 0 || !out_data || !out_size) {
    nanopdf__set_error(
        context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid DCTDecode arguments");
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }

  *out_data = NULL;
  *out_size = 0;
  if (input_size > (size_t)INT_MAX) {
    nanopdf__set_error(
        context, NANOPDF_STATUS_MALFORMED, "DCTDecode input too large");
    return NANOPDF_STATUS_MALFORMED;
  }

  decoded = stbi_load_from_memory(
      input, (int)input_size, &width, &height, &channels, 0);
  if (!decoded || width <= 0 || height <= 0 || channels <= 0) {
    if (decoded) {
      stbi_image_free(decoded);
    }
    nanopdf__set_error(
        context, NANOPDF_STATUS_MALFORMED, "DCTDecode: failed to decode JPEG data");
    return NANOPDF_STATUS_MALFORMED;
  }

  if ((size_t)width > (SIZE_MAX / (size_t)height) ||
      ((size_t)width * (size_t)height) > (SIZE_MAX / (size_t)channels)) {
    stbi_image_free(decoded);
    nanopdf__set_error(
        context, NANOPDF_STATUS_MALFORMED, "DCTDecode output size overflow");
    return NANOPDF_STATUS_MALFORMED;
  }
  decoded_size = (size_t)width * (size_t)height * (size_t)channels;

  *out_data = (uint8_t*)nanopdf__allocator_alloc(
      &context->allocator, decoded_size);
  if (!*out_data) {
    stbi_image_free(decoded);
    nanopdf__set_error(
        context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate DCTDecode output");
    return NANOPDF_STATUS_OUT_OF_MEMORY;
  }

  memcpy(*out_data, decoded, decoded_size);
  *out_size = decoded_size;
  stbi_image_free(decoded);
  nanopdf__clear_error(context);
  return NANOPDF_STATUS_OK;
}

nanopdf_status nanopdf__decode_ccitt(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    const nanopdf_ccitt_params* params,
    uint8_t** out_data,
    size_t* out_size) {
  int columns = 0;
  int rows = 0;
  int pitch = 0;
  size_t output_size = 0;
  uint8_t* output = NULL;
  uint8_t* ref_buf = NULL;
  uint8_t* scanline_buf = NULL;
  uint32_t bitpos = 0;
  int row = 0;

  if (!context || !input || input_size == 0 || !params || !out_data || !out_size) {
    nanopdf__set_error(
        context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid CCITTFaxDecode arguments");
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }

  *out_data = NULL;
  *out_size = 0;
  columns = params->columns;
  rows = params->rows;
  if (columns <= 0 || columns > NANOPDF_FAX_MAX_IMAGE_DIMENSION ||
      rows <= 0 || rows > NANOPDF_FAX_MAX_IMAGE_DIMENSION) {
    nanopdf__set_error(
        context, NANOPDF_STATUS_MALFORMED, "CCITTFaxDecode invalid image dimensions");
    return NANOPDF_STATUS_MALFORMED;
  }

  pitch = (columns + 7) / 8;
  if ((size_t)pitch > SIZE_MAX / (size_t)rows) {
    nanopdf__set_error(
        context, NANOPDF_STATUS_MALFORMED, "CCITTFaxDecode output size overflow");
    return NANOPDF_STATUS_MALFORMED;
  }
  output_size = (size_t)pitch * (size_t)rows;

  output = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, output_size);
  ref_buf = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, (size_t)pitch);
  scanline_buf = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, (size_t)pitch);
  if (!output || !ref_buf || !scanline_buf) {
    nanopdf__allocator_free(&context->allocator, scanline_buf);
    nanopdf__allocator_free(&context->allocator, ref_buf);
    nanopdf__allocator_free(&context->allocator, output);
    nanopdf__set_error(
        context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate CCITTFaxDecode output");
    return NANOPDF_STATUS_OUT_OF_MEMORY;
  }

  memset(output, 0xff, output_size);
  memset(ref_buf, 0xff, (size_t)pitch);
  for (row = 0; row < rows; ++row) {
    uint32_t bitsize = fax_src_bit_size(input_size);
    fax_skip_eol(input, input_size, &bitpos);
    if (bitpos >= bitsize) {
      break;
    }

    memset(scanline_buf, 0xff, (size_t)pitch);
    if (params->k < 0) {
      fax_g4_get_row(
          input, input_size, &bitpos, scanline_buf, (size_t)pitch, ref_buf, (size_t)pitch, columns);
      memcpy(ref_buf, scanline_buf, (size_t)pitch);
    } else if (params->k == 0) {
      fax_get_1d_line(input, input_size, &bitpos, scanline_buf, (size_t)pitch, columns);
    } else {
      if (fax_next_bit(input, input_size, &bitpos)) {
        fax_get_1d_line(input, input_size, &bitpos, scanline_buf, (size_t)pitch, columns);
      } else {
        fax_g4_get_row(
            input,
            input_size,
            &bitpos,
            scanline_buf,
            (size_t)pitch,
            ref_buf,
            (size_t)pitch,
            columns);
      }
      memcpy(ref_buf, scanline_buf, (size_t)pitch);
    }

    if (params->end_of_line) {
      fax_skip_eol(input, input_size, &bitpos);
    }
    if (params->encoded_byte_align && bitpos < bitsize) {
      uint32_t bitpos0 = bitpos;
      uint32_t bitpos1 = (bitpos + 7u) & ~7u;
      int should_align = 1;
      while (should_align && bitpos0 < bitpos1) {
        int bit = (input[bitpos0 / 8u] & (uint8_t)(1u << (7u - (bitpos0 % 8u)))) ? 1 : 0;
        if (bit != 0) {
          should_align = 0;
        } else {
          ++bitpos0;
        }
      }
      if (should_align) {
        bitpos = bitpos1;
      }
    }
    if (params->black_is_1) {
      fax_invert_buffer(scanline_buf, (size_t)pitch);
    }
    memcpy(output + (size_t)row * (size_t)pitch, scanline_buf, (size_t)pitch);
  }

  nanopdf__allocator_free(&context->allocator, scanline_buf);
  nanopdf__allocator_free(&context->allocator, ref_buf);
  *out_data = output;
  *out_size = output_size;
  nanopdf__clear_error(context);
  return NANOPDF_STATUS_OK;
}

#ifndef NANOPDF_C_USE_CPP_IMAGE_BRIDGE
nanopdf_status nanopdf__decode_jpx(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    uint8_t** out_data,
    size_t* out_size) {
  nanopdf_jpx_header header;
  nanopdf_jpx_tile_component* tile_components = NULL;
  const uint8_t* codestream = NULL;
  size_t codestream_size = 0;
  size_t tile_component_count = 0;
  size_t i;
  uint8_t* pixels = NULL;
  size_t output_size = 0;
  uint32_t image_width = 0;
  uint32_t image_height = 0;
  uint32_t tile_count_x = 0;
  uint32_t tile_count_y = 0;
  uint32_t tile_count = 0;
  uint32_t tile_index;

  if (out_data) {
    *out_data = NULL;
  }
  if (out_size) {
    *out_size = 0;
  }
  if (!context || !input || input_size == 0 || !out_data || !out_size) {
    if (context) {
      nanopdf__set_error(
          context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid JPXDecode arguments");
    }
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }

  nanopdf_jpx_header_init(&header);
  if (!nanopdf_jpx_extract_codestream(input, input_size, &codestream, &codestream_size) ||
      !nanopdf_jpx_parse_header(context, codestream, codestream_size, &header)) {
    nanopdf_jpx_header_destroy(context, &header);
    nanopdf__set_error(context, NANOPDF_STATUS_MALFORMED, "JPXDecode malformed header");
    return NANOPDF_STATUS_MALFORMED;
  }
  image_width = nanopdf_jpx_image_width(&header);
  image_height = nanopdf_jpx_image_height(&header);
  tile_count_x = nanopdf__jpx_tile_count_x(&header);
  tile_count_y = nanopdf__jpx_tile_count_y(&header);
  tile_component_count = header.siz.num_components;
  if (header.first_tile_part_offset > 0 && tile_component_count > 0u &&
      image_width > 0u && image_height > 0u && tile_count_x > 0u &&
      tile_count_y > 0u && tile_count_y <= UINT32_MAX / tile_count_x) {
    tile_count = tile_count_x * tile_count_y;
    if ((size_t)image_width > SIZE_MAX / (size_t)image_height ||
        tile_component_count >
            SIZE_MAX / ((size_t)image_width * (size_t)image_height)) {
      nanopdf_jpx_header_destroy(context, &header);
      nanopdf__set_error(context, NANOPDF_STATUS_OUT_OF_MEMORY, "JPXDecode output too large");
      return NANOPDF_STATUS_OUT_OF_MEMORY;
    }
    output_size = (size_t)image_width * (size_t)image_height * tile_component_count;
    pixels = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, output_size);
    if (!pixels) {
      nanopdf_jpx_header_destroy(context, &header);
      nanopdf__set_error(context, NANOPDF_STATUS_OUT_OF_MEMORY, "JPXDecode output allocation failed");
      return NANOPDF_STATUS_OUT_OF_MEMORY;
    }
    memset(pixels, 0, output_size);
    for (tile_index = 0; tile_index < tile_count; ++tile_index) {
      nanopdf_jpx_header tile_header;
      uint8_t* packet_headers = NULL;
      size_t packet_header_size = 0;
      uint8_t* packet_bodies = NULL;
      size_t packet_body_size = 0;
      uint8_t* tile_pixels = NULL;
      size_t tile_pixels_size = 0;
      uint32_t tile_x0 = 0;
      uint32_t tile_y0 = 0;
      uint32_t tile_x1 = 0;
      uint32_t tile_y1 = 0;
      uint32_t tile_width;
      uint32_t tile_height;
      int built_all = 1;
      nanopdf_jpx_header_init(&tile_header);
      if (!nanopdf__jpx_tile_bounds(
              &header, tile_index, &tile_x0, &tile_y0, &tile_x1, &tile_y1)) {
        nanopdf_jpx_header_destroy(context, &tile_header);
        continue;
      }
      if (!nanopdf_jpx_parse_header(context, codestream, codestream_size, &tile_header)) {
        nanopdf__allocator_free(&context->allocator, pixels);
        nanopdf_jpx_header_destroy(context, &tile_header);
        nanopdf_jpx_header_destroy(context, &header);
        nanopdf__set_error(context, NANOPDF_STATUS_MALFORMED, "JPXDecode malformed header");
        return NANOPDF_STATUS_MALFORMED;
      }
      if (!nanopdf__jpx_collect_tile_packet_stream(
              context,
              codestream,
              codestream_size,
              tile_header.first_tile_part_offset,
              &tile_header,
              tile_index,
              &packet_headers,
              &packet_header_size,
              &packet_bodies,
              &packet_body_size)) {
        nanopdf__allocator_free(&context->allocator, packet_headers);
        nanopdf__allocator_free(&context->allocator, packet_bodies);
        nanopdf__allocator_free(&context->allocator, pixels);
        nanopdf_jpx_header_destroy(context, &tile_header);
        nanopdf_jpx_header_destroy(context, &header);
        nanopdf__set_error(
            context, NANOPDF_STATUS_UNSUPPORTED, "JPXDecode missing tile data");
        return NANOPDF_STATUS_UNSUPPORTED;
      }
      tile_width = tile_x1 - tile_x0;
      tile_height = tile_y1 - tile_y0;
      tile_header.siz.width = tile_x1;
      tile_header.siz.height = tile_y1;
      tile_header.siz.x_offset = tile_x0;
      tile_header.siz.y_offset = tile_y0;
      tile_header.siz.tile_width = tile_width;
      tile_header.siz.tile_height = tile_height;
      tile_header.siz.tile_x_offset = tile_x0;
      tile_header.siz.tile_y_offset = tile_y0;
      if (tile_component_count > SIZE_MAX / sizeof(tile_components[0])) {
        nanopdf__allocator_free(&context->allocator, packet_headers);
        nanopdf__allocator_free(&context->allocator, packet_bodies);
        nanopdf__allocator_free(&context->allocator, pixels);
        nanopdf_jpx_header_destroy(context, &tile_header);
        nanopdf_jpx_header_destroy(context, &header);
        nanopdf__set_error(
            context, NANOPDF_STATUS_OUT_OF_MEMORY, "JPXDecode tile component allocation failed");
        return NANOPDF_STATUS_OUT_OF_MEMORY;
      }
      tile_components = (nanopdf_jpx_tile_component*)nanopdf__allocator_alloc(
          &context->allocator, tile_component_count * sizeof(tile_components[0]));
      if (!tile_components) {
        nanopdf__allocator_free(&context->allocator, packet_headers);
        nanopdf__allocator_free(&context->allocator, packet_bodies);
        nanopdf__allocator_free(&context->allocator, pixels);
        nanopdf_jpx_header_destroy(context, &tile_header);
        nanopdf_jpx_header_destroy(context, &header);
        nanopdf__set_error(
            context, NANOPDF_STATUS_OUT_OF_MEMORY, "JPXDecode tile component allocation failed");
        return NANOPDF_STATUS_OUT_OF_MEMORY;
      }
      for (i = 0; i < tile_component_count; ++i) {
        nanopdf_jpx_tile_component_init(&tile_components[i]);
      }
      for (i = 0; i < tile_component_count; ++i) {
        const nanopdf_jpx_cod_params* component_cod = &tile_header.cod;
        uint32_t component_width;
        uint32_t component_height;
        if (tile_header.has_component_cod && tile_header.component_cod &&
            tile_header.has_component_cod[i]) {
          component_cod = &tile_header.component_cod[i];
        }
        component_width =
            nanopdf__jpx_component_extent(&tile_header, i, tile_x0, tile_x1, 1);
        component_height =
            nanopdf__jpx_component_extent(&tile_header, i, tile_y0, tile_y1, 0);
        if (component_width == 0u || component_height == 0u ||
            !nanopdf_jpx_build_tile_component(
                context,
                &tile_components[i],
                component_cod,
                0,
                0,
                (int)component_width,
                (int)component_height)) {
          built_all = 0;
          break;
        }
      }
      if (built_all &&
          (packet_header_size > 0u
               ? nanopdf_jpx_decode_tile_data_with_packet_headers(
                     context,
                     packet_headers,
                     packet_header_size,
                     packet_bodies,
                     packet_body_size,
                     &tile_header,
                     tile_components,
                     tile_component_count)
               : nanopdf_jpx_decode_tile_data_simple(
                     context,
                     packet_bodies,
                     packet_body_size,
                     &tile_header,
                     tile_components,
                     tile_component_count)) &&
          nanopdf_jpx_reconstruct_pixels(
              context,
              &tile_header,
              tile_components,
              tile_component_count,
              &tile_pixels,
              &tile_pixels_size)) {
        uint32_t y;
        for (y = 0; y < tile_height; ++y) {
          size_t dst = ((size_t)(tile_y0 - header.siz.y_offset + y) *
                            (size_t)image_width +
                        (size_t)(tile_x0 - header.siz.x_offset)) *
                       tile_component_count;
          size_t src = (size_t)y * (size_t)tile_width * tile_component_count;
          size_t row_size = (size_t)tile_width * tile_component_count;
          if (src + row_size <= tile_pixels_size && dst + row_size <= output_size) {
            memcpy(pixels + dst, tile_pixels + src, row_size);
          }
        }
      } else {
        built_all = 0;
      }
      for (i = 0; i < tile_component_count; ++i) {
        nanopdf_jpx_tile_component_destroy(context, &tile_components[i]);
      }
      nanopdf__allocator_free(&context->allocator, tile_components);
      nanopdf__allocator_free(&context->allocator, packet_headers);
      nanopdf__allocator_free(&context->allocator, packet_bodies);
      nanopdf__allocator_free(&context->allocator, tile_pixels);
      nanopdf_jpx_header_destroy(context, &tile_header);
      tile_components = NULL;
      if (!built_all) {
        nanopdf__allocator_free(&context->allocator, pixels);
        nanopdf_jpx_header_destroy(context, &header);
        nanopdf__set_error(
            context,
            NANOPDF_STATUS_UNSUPPORTED,
            "JPXDecode native C pixel reconstruction incomplete");
        return NANOPDF_STATUS_UNSUPPORTED;
      }
    }
    nanopdf_jpx_header_destroy(context, &header);
    *out_data = pixels;
    *out_size = output_size;
    nanopdf__clear_error(context);
    return NANOPDF_STATUS_OK;
  }
  nanopdf_jpx_header_destroy(context, &header);
  nanopdf__set_error(
      context,
      NANOPDF_STATUS_UNSUPPORTED,
      "JPXDecode native C pixel reconstruction incomplete");
  return NANOPDF_STATUS_UNSUPPORTED;
}

nanopdf_status nanopdf__decode_jbig2(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    const nanopdf_jbig2_params* params,
    uint8_t** out_data,
    size_t* out_size) {
  nanopdf_jbig2_image page;
  int decode_result;

  nanopdf_jbig2_image_init_external(&page, 0, 0, 0, NULL);
  if (out_data) {
    *out_data = NULL;
  }
  if (out_size) {
    *out_size = 0;
  }
  if (!context || !input || input_size == 0 || !out_data || !out_size) {
    if (context) {
      nanopdf__set_error(
          context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid JBIG2Decode arguments");
    }
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }

  decode_result = nanopdf_jbig2_decode_page(
      context,
      input,
      input_size,
      params ? params->globals : NULL,
      params ? params->globals_size : 0,
      &page);
  if (decode_result < 0) {
    nanopdf__set_error(context, NANOPDF_STATUS_MALFORMED, "JBIG2Decode malformed stream");
    return NANOPDF_STATUS_MALFORMED;
  }
  if (decode_result > 0) {
    size_t data_size = (size_t)page.stride * (size_t)page.height;
    uint8_t* output = (uint8_t*)nanopdf__allocator_alloc(&context->allocator, data_size);
    if (!output) {
      nanopdf_jbig2_image_destroy(context, &page);
      nanopdf__set_error(context, NANOPDF_STATUS_OUT_OF_MEMORY, "JBIG2Decode output allocation failed");
      return NANOPDF_STATUS_OUT_OF_MEMORY;
    }
    memcpy(output, page.data, data_size);
    nanopdf_jbig2_image_destroy(context, &page);
    *out_data = output;
    *out_size = data_size;
    nanopdf__clear_error(context);
    return NANOPDF_STATUS_OK;
  }
  nanopdf_jbig2_image_destroy(context, &page);
  nanopdf__set_error(
      context,
      NANOPDF_STATUS_UNSUPPORTED,
      "JBIG2Decode native C segment processing incomplete");
  return NANOPDF_STATUS_UNSUPPORTED;
}
#endif
