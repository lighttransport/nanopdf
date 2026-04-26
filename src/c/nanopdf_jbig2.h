#ifndef NANOPDF_JBIG2_H_
#define NANOPDF_JBIG2_H_

#include "nanopdf_c_internal.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nanopdf_jbig2_compose_op {
  NANOPDF_JBIG2_COMPOSE_OR = 0,
  NANOPDF_JBIG2_COMPOSE_AND = 1,
  NANOPDF_JBIG2_COMPOSE_XOR = 2,
  NANOPDF_JBIG2_COMPOSE_XNOR = 3,
  NANOPDF_JBIG2_COMPOSE_REPLACE = 4
} nanopdf_jbig2_compose_op;

typedef enum nanopdf_jbig2_segment_state {
  NANOPDF_JBIG2_SEGMENT_HEADER_UNPARSED = 0,
  NANOPDF_JBIG2_SEGMENT_DATA_UNPARSED = 1,
  NANOPDF_JBIG2_SEGMENT_PARSE_COMPLETE = 2,
  NANOPDF_JBIG2_SEGMENT_PAUSED = 3,
  NANOPDF_JBIG2_SEGMENT_ERROR = 4
} nanopdf_jbig2_segment_state;

typedef enum nanopdf_jbig2_result_type {
  NANOPDF_JBIG2_VOID_POINTER = 0,
  NANOPDF_JBIG2_IMAGE_POINTER = 1,
  NANOPDF_JBIG2_SYMBOL_DICT_POINTER = 2,
  NANOPDF_JBIG2_PATTERN_DICT_POINTER = 3,
  NANOPDF_JBIG2_HUFFMAN_TABLE_POINTER = 4
} nanopdf_jbig2_result_type;

typedef struct nanopdf_jbig2_rect {
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
} nanopdf_jbig2_rect;

typedef struct nanopdf_jbig2_region_info {
  uint32_t width;
  uint32_t height;
  uint32_t x;
  uint32_t y;
  uint8_t flags;
} nanopdf_jbig2_region_info;

typedef struct nanopdf_jbig2_page_info {
  uint32_t width;
  uint32_t height;
  uint32_t x_resolution;
  uint32_t y_resolution;
  uint8_t flags;
  uint16_t stripe_height;
  uint8_t default_pixel;
  uint8_t is_lossless;
} nanopdf_jbig2_page_info;

typedef struct nanopdf_jbig2_bitstream {
  const uint8_t* data;
  size_t size;
  uint32_t byte_idx;
  uint32_t bit_idx;
  uint64_t key;
} nanopdf_jbig2_bitstream;

typedef struct nanopdf_jbig2_image {
  uint8_t* data;
  uint8_t owns_data;
  int32_t width;
  int32_t height;
  int32_t stride;
} nanopdf_jbig2_image;

typedef struct nanopdf_jbig2_symbol_dict {
  nanopdf_jbig2_image* images;
  size_t count;
  size_t capacity;
} nanopdf_jbig2_symbol_dict;

typedef struct nanopdf_jbig2_pattern_dict {
  nanopdf_jbig2_image* patterns;
  size_t count;
  size_t capacity;
  uint32_t width;
  uint32_t height;
} nanopdf_jbig2_pattern_dict;

typedef struct nanopdf_jbig2_segment {
  uint32_t number;
  uint8_t type;
  uint8_t page_association_size;
  uint8_t deferred_non_retain;
  int32_t referred_to_segment_count;
  uint32_t* referred_to_segment_numbers;
  size_t referred_to_segment_capacity;
  uint32_t page_association;
  uint32_t data_length;
  uint32_t header_length;
  uint32_t data_offset;
  uint64_t key;
  nanopdf_jbig2_segment_state state;
  nanopdf_jbig2_result_type result_type;
  nanopdf_jbig2_symbol_dict* symbol_dict;
  nanopdf_jbig2_pattern_dict* pattern_dict;
  nanopdf_jbig2_image* image;
  void* huffman_table;
} nanopdf_jbig2_segment;

typedef struct nanopdf_jbig2_table_line {
  int16_t prefix_length;
  int16_t range_length;
  int32_t range_low;
} nanopdf_jbig2_table_line;

typedef struct nanopdf_jbig2_huffman_table {
  uint8_t is_ok;
  uint8_t has_oob;
  size_t oob_index;
  int* codes;
  int* prefix_lengths;
  int* range_lengths;
  int* range_lows;
  size_t count;
} nanopdf_jbig2_huffman_table;

extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b1[];
extern const size_t nanopdf_jbig2_huffman_table_b1_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b2[];
extern const size_t nanopdf_jbig2_huffman_table_b2_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b3[];
extern const size_t nanopdf_jbig2_huffman_table_b3_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b4[];
extern const size_t nanopdf_jbig2_huffman_table_b4_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b5[];
extern const size_t nanopdf_jbig2_huffman_table_b5_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b6[];
extern const size_t nanopdf_jbig2_huffman_table_b6_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b7[];
extern const size_t nanopdf_jbig2_huffman_table_b7_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b8[];
extern const size_t nanopdf_jbig2_huffman_table_b8_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b9[];
extern const size_t nanopdf_jbig2_huffman_table_b9_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b10[];
extern const size_t nanopdf_jbig2_huffman_table_b10_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b11[];
extern const size_t nanopdf_jbig2_huffman_table_b11_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b12[];
extern const size_t nanopdf_jbig2_huffman_table_b12_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b13[];
extern const size_t nanopdf_jbig2_huffman_table_b13_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b14[];
extern const size_t nanopdf_jbig2_huffman_table_b14_size;
extern const nanopdf_jbig2_table_line nanopdf_jbig2_huffman_table_b15[];
extern const size_t nanopdf_jbig2_huffman_table_b15_size;

typedef struct nanopdf_jbig2_arith_ctx {
  uint8_t mps;
  uint8_t index;
} nanopdf_jbig2_arith_ctx;

typedef struct nanopdf_jbig2_arith_decoder {
  uint8_t complete;
  uint8_t b;
  uint32_t c;
  uint32_t a;
  uint32_t ct;
  nanopdf_jbig2_bitstream* stream;
} nanopdf_jbig2_arith_decoder;

typedef struct nanopdf_jbig2_arith_int_decoder {
  nanopdf_jbig2_arith_ctx iax[512];
} nanopdf_jbig2_arith_int_decoder;

typedef struct nanopdf_jbig2_arith_iaid_decoder {
  uint8_t symbol_code_len;
  nanopdf_jbig2_arith_ctx* iaid;
  size_t iaid_count;
} nanopdf_jbig2_arith_iaid_decoder;

typedef struct nanopdf_jbig2_grd_proc {
  uint8_t mmr;
  uint8_t tpgdon;
  uint8_t use_skip;
  uint8_t gb_template;
  uint32_t width;
  uint32_t height;
  nanopdf_jbig2_image* skip;
  int8_t gbat[8];
} nanopdf_jbig2_grd_proc;

typedef struct nanopdf_jbig2_symbol_dict_header {
  uint8_t huffman;
  uint8_t refagg;
  uint8_t template_id;
  uint8_t refinement_template;
  uint8_t huff_dh;
  uint8_t huff_dw;
  uint8_t huff_bitmap_size;
  uint8_t huff_agg_inst;
  uint32_t num_exported_symbols;
  uint32_t num_new_symbols;
  int8_t sdat[8];
  int8_t sdrat[4];
} nanopdf_jbig2_symbol_dict_header;

typedef struct nanopdf_jbig2_text_region_header {
  nanopdf_jbig2_region_info region;
  uint8_t huffman;
  uint8_t refine;
  uint8_t strip_count_log;
  uint8_t ref_corner;
  uint8_t transposed;
  uint8_t default_pixel;
  uint8_t refinement_template;
  nanopdf_jbig2_compose_op compose_op;
  int8_t ds_offset;
  uint32_t num_instances;
  uint8_t huff_fs;
  uint8_t huff_ds;
  uint8_t huff_dt;
  uint8_t huff_rdw;
  uint8_t huff_rdh;
  uint8_t huff_rdx;
  uint8_t huff_rdy;
  uint8_t huff_rsize;
  int8_t sbrat[4];
} nanopdf_jbig2_text_region_header;

typedef struct nanopdf_jbig2_pattern_dict_header {
  uint8_t mmr;
  uint8_t template_id;
  uint32_t width;
  uint32_t height;
  uint32_t gray_max;
} nanopdf_jbig2_pattern_dict_header;

typedef struct nanopdf_jbig2_generic_refinement_region_header {
  nanopdf_jbig2_region_info region;
  uint8_t template_id;
  uint8_t typical_prediction;
  int8_t grat[4];
} nanopdf_jbig2_generic_refinement_region_header;

typedef struct nanopdf_jbig2_halftone_region_header {
  nanopdf_jbig2_region_info region;
  uint8_t mmr;
  uint8_t template_id;
  uint8_t enable_skip;
  uint8_t default_pixel;
  nanopdf_jbig2_compose_op compose_op;
  uint32_t grid_width;
  uint32_t grid_height;
  int32_t grid_x;
  int32_t grid_y;
  int32_t grid_vector_x;
  int32_t grid_vector_y;
} nanopdf_jbig2_halftone_region_header;

void nanopdf_jbig2_bitstream_init(
    nanopdf_jbig2_bitstream* stream,
    const uint8_t* data,
    size_t size,
    uint64_t key);
int32_t nanopdf_jbig2_bitstream_read_bits_u32(
    nanopdf_jbig2_bitstream* stream,
    uint32_t bits,
    uint32_t* out_value);
int32_t nanopdf_jbig2_bitstream_read_bits_i32(
    nanopdf_jbig2_bitstream* stream,
    uint32_t bits,
    int32_t* out_value);
int32_t nanopdf_jbig2_bitstream_read_bit_u32(
    nanopdf_jbig2_bitstream* stream,
    uint32_t* out_value);
int32_t nanopdf_jbig2_bitstream_read_bit_bool(
    nanopdf_jbig2_bitstream* stream,
    uint8_t* out_value);
int32_t nanopdf_jbig2_bitstream_read_byte(
    nanopdf_jbig2_bitstream* stream,
    uint8_t* out_value);
int32_t nanopdf_jbig2_bitstream_read_u32(
    nanopdf_jbig2_bitstream* stream,
    uint32_t* out_value);
int32_t nanopdf_jbig2_bitstream_read_u16(
    nanopdf_jbig2_bitstream* stream,
    uint16_t* out_value);
void nanopdf_jbig2_bitstream_align_byte(nanopdf_jbig2_bitstream* stream);
uint8_t nanopdf_jbig2_bitstream_current_byte(const nanopdf_jbig2_bitstream* stream);
void nanopdf_jbig2_bitstream_inc_byte(nanopdf_jbig2_bitstream* stream);
uint8_t nanopdf_jbig2_bitstream_current_byte_arith(const nanopdf_jbig2_bitstream* stream);
uint8_t nanopdf_jbig2_bitstream_next_byte_arith(const nanopdf_jbig2_bitstream* stream);
uint32_t nanopdf_jbig2_bitstream_offset(const nanopdf_jbig2_bitstream* stream);
void nanopdf_jbig2_bitstream_set_offset(nanopdf_jbig2_bitstream* stream, uint32_t offset);
void nanopdf_jbig2_bitstream_add_offset(nanopdf_jbig2_bitstream* stream, uint32_t delta);
uint32_t nanopdf_jbig2_bitstream_bit_position(const nanopdf_jbig2_bitstream* stream);
void nanopdf_jbig2_bitstream_set_bit_position(nanopdf_jbig2_bitstream* stream, uint32_t bit_pos);
const uint8_t* nanopdf_jbig2_bitstream_pointer(const nanopdf_jbig2_bitstream* stream);
uint32_t nanopdf_jbig2_bitstream_bytes_left(const nanopdf_jbig2_bitstream* stream);
int nanopdf_jbig2_bitstream_in_bounds(const nanopdf_jbig2_bitstream* stream);

int nanopdf_jbig2_image_init(
    nanopdf_context* context,
    nanopdf_jbig2_image* image,
    int32_t width,
    int32_t height);
int nanopdf_jbig2_image_init_external(
    nanopdf_jbig2_image* image,
    int32_t width,
    int32_t height,
    int32_t stride,
    uint8_t* data);
void nanopdf_jbig2_image_destroy(nanopdf_context* context, nanopdf_jbig2_image* image);
int nanopdf_jbig2_image_copy(
    nanopdf_context* context,
    nanopdf_jbig2_image* out_image,
    const nanopdf_jbig2_image* source);
int nanopdf_jbig2_image_valid_size(int32_t width, int32_t height);
int nanopdf_jbig2_image_get_pixel(const nanopdf_jbig2_image* image, int32_t x, int32_t y);
void nanopdf_jbig2_image_set_pixel(nanopdf_jbig2_image* image, int32_t x, int32_t y, int value);
const uint8_t* nanopdf_jbig2_image_get_line_const(const nanopdf_jbig2_image* image, int32_t y);
uint8_t* nanopdf_jbig2_image_get_line(nanopdf_jbig2_image* image, int32_t y);
void nanopdf_jbig2_image_copy_line(nanopdf_jbig2_image* image, int32_t dest_y, int32_t src_y);
void nanopdf_jbig2_image_fill(nanopdf_jbig2_image* image, int value);
int nanopdf_jbig2_image_expand(
    nanopdf_context* context,
    nanopdf_jbig2_image* image,
    int32_t height,
    int value);
int nanopdf_jbig2_image_compose_from(
    nanopdf_jbig2_image* dest,
    int64_t x,
    int64_t y,
    const nanopdf_jbig2_image* source,
    nanopdf_jbig2_compose_op op);

void nanopdf_jbig2_symbol_dict_init(nanopdf_jbig2_symbol_dict* dict);
void nanopdf_jbig2_symbol_dict_destroy(
    nanopdf_context* context,
    nanopdf_jbig2_symbol_dict* dict);
int nanopdf_jbig2_symbol_dict_add_image(
    nanopdf_context* context,
    nanopdf_jbig2_symbol_dict* dict,
    const nanopdf_jbig2_image* image);
int nanopdf_jbig2_symbol_dict_deep_copy(
    nanopdf_context* context,
    nanopdf_jbig2_symbol_dict* out_dict,
    const nanopdf_jbig2_symbol_dict* source);
size_t nanopdf_jbig2_symbol_dict_count(const nanopdf_jbig2_symbol_dict* dict);
nanopdf_jbig2_image* nanopdf_jbig2_symbol_dict_get(
    nanopdf_jbig2_symbol_dict* dict,
    size_t index);
const nanopdf_jbig2_image* nanopdf_jbig2_symbol_dict_get_const(
    const nanopdf_jbig2_symbol_dict* dict,
    size_t index);

void nanopdf_jbig2_pattern_dict_init(nanopdf_jbig2_pattern_dict* dict);
void nanopdf_jbig2_pattern_dict_destroy(
    nanopdf_context* context,
    nanopdf_jbig2_pattern_dict* dict);
int nanopdf_jbig2_pattern_dict_add(
    nanopdf_context* context,
    nanopdf_jbig2_pattern_dict* dict,
    const nanopdf_jbig2_image* pattern);
void nanopdf_jbig2_pattern_dict_set_dimensions(
    nanopdf_jbig2_pattern_dict* dict,
    uint32_t width,
    uint32_t height);
size_t nanopdf_jbig2_pattern_dict_count(const nanopdf_jbig2_pattern_dict* dict);
nanopdf_jbig2_image* nanopdf_jbig2_pattern_dict_get(
    nanopdf_jbig2_pattern_dict* dict,
    size_t index);
const nanopdf_jbig2_image* nanopdf_jbig2_pattern_dict_get_const(
    const nanopdf_jbig2_pattern_dict* dict,
    size_t index);

void nanopdf_jbig2_segment_init(nanopdf_jbig2_segment* segment);
void nanopdf_jbig2_segment_destroy(nanopdf_context* context, nanopdf_jbig2_segment* segment);
int nanopdf_jbig2_segment_set_referred_count(
    nanopdf_context* context,
    nanopdf_jbig2_segment* segment,
    int32_t count);
int nanopdf_jbig2_segment_parse_header(
    nanopdf_context* context,
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_segment* segment);

void nanopdf_jbig2_huffman_table_init(nanopdf_jbig2_huffman_table* table);
void nanopdf_jbig2_huffman_table_destroy(
    nanopdf_context* context,
    nanopdf_jbig2_huffman_table* table);
int nanopdf_jbig2_huffman_table_init_standard(
    nanopdf_context* context,
    nanopdf_jbig2_huffman_table* table,
    const nanopdf_jbig2_table_line* lines,
    size_t line_count,
    int has_oob);
int nanopdf_jbig2_huffman_table_init_encoded(
    nanopdf_context* context,
    nanopdf_jbig2_huffman_table* table,
    nanopdf_jbig2_bitstream* stream);
int nanopdf_jbig2_huffman_table_decode(
    nanopdf_jbig2_huffman_table* table,
    nanopdf_jbig2_bitstream* stream,
    int32_t* out_value);

void nanopdf_jbig2_arith_ctx_init(nanopdf_jbig2_arith_ctx* context);
void nanopdf_jbig2_arith_decoder_init(
    nanopdf_jbig2_arith_decoder* decoder,
    nanopdf_jbig2_bitstream* stream);
int nanopdf_jbig2_arith_decode(
    nanopdf_jbig2_arith_decoder* decoder,
    nanopdf_jbig2_arith_ctx* context);

void nanopdf_jbig2_arith_int_decoder_init(nanopdf_jbig2_arith_int_decoder* decoder);
int nanopdf_jbig2_arith_int_decode(
    nanopdf_jbig2_arith_int_decoder* decoder,
    nanopdf_jbig2_arith_decoder* arith_decoder,
    int* out_value);
int nanopdf_jbig2_arith_iaid_decoder_init(
    nanopdf_context* context,
    nanopdf_jbig2_arith_iaid_decoder* decoder,
    uint8_t symbol_code_len);
void nanopdf_jbig2_arith_iaid_decoder_destroy(
    nanopdf_context* context,
    nanopdf_jbig2_arith_iaid_decoder* decoder);
void nanopdf_jbig2_arith_iaid_decode(
    nanopdf_jbig2_arith_iaid_decoder* decoder,
    nanopdf_jbig2_arith_decoder* arith_decoder,
    uint32_t* out_value);

void nanopdf_jbig2_grd_proc_init(nanopdf_jbig2_grd_proc* proc);
int nanopdf_jbig2_grd_proc_decode_arith(
    nanopdf_context* context,
    const nanopdf_jbig2_grd_proc* proc,
    nanopdf_jbig2_arith_decoder* arith_decoder,
    nanopdf_jbig2_arith_ctx* gb_contexts,
    size_t gb_context_count,
    nanopdf_jbig2_image* out_image);
int nanopdf_jbig2_grd_proc_decode_mmr(
    nanopdf_context* context,
    const nanopdf_jbig2_grd_proc* proc,
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_image* out_image);
int nanopdf_jbig2_parse_region_info(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_region_info* out_region);
int nanopdf_jbig2_parse_page_info(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_page_info* out_page_info);
int nanopdf_jbig2_parse_generic_region_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_region_info* out_region,
    nanopdf_jbig2_grd_proc* out_proc);
int nanopdf_jbig2_parse_symbol_dict_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_symbol_dict_header* out_header);
int nanopdf_jbig2_parse_text_region_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_text_region_header* out_header);
int nanopdf_jbig2_parse_pattern_dict_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_pattern_dict_header* out_header);
int nanopdf_jbig2_parse_generic_refinement_region_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_generic_refinement_region_header* out_header);
int nanopdf_jbig2_parse_halftone_region_header(
    nanopdf_jbig2_bitstream* stream,
    nanopdf_jbig2_halftone_region_header* out_header);
int nanopdf_jbig2_parse_end_of_stripe(
    nanopdf_jbig2_bitstream* stream,
    uint32_t* out_y_location);
int nanopdf_jbig2_page_init_from_info(
    nanopdf_context* context,
    const nanopdf_jbig2_page_info* page_info,
    nanopdf_jbig2_image* out_page);
int nanopdf_jbig2_page_compose_region(
    nanopdf_jbig2_image* page,
    const nanopdf_jbig2_region_info* region,
    const nanopdf_jbig2_image* image);
int nanopdf_jbig2_decode_page(
    nanopdf_context* context,
    const uint8_t* data,
    size_t data_size,
    const uint8_t* globals,
    size_t globals_size,
    nanopdf_jbig2_image* out_page);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_JBIG2_H_ */
