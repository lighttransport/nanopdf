#ifndef NANOPDF_JPX_H_
#define NANOPDF_JPX_H_

#include "nanopdf_c_internal.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  NANOPDF_JPX_SOC = 0xff4f,
  NANOPDF_JPX_SOT = 0xff90,
  NANOPDF_JPX_SOD = 0xff93,
  NANOPDF_JPX_EOC = 0xffd9,
  NANOPDF_JPX_SIZ = 0xff51,
  NANOPDF_JPX_COD = 0xff52,
  NANOPDF_JPX_COC = 0xff53,
  NANOPDF_JPX_QCD = 0xff5c,
  NANOPDF_JPX_QCC = 0xff5d,
  NANOPDF_JPX_RGN = 0xff5e,
  NANOPDF_JPX_POC = 0xff5f,
  NANOPDF_JPX_PPM = 0xff60,
  NANOPDF_JPX_PPT = 0xff61,
  NANOPDF_JPX_TLM = 0xff55,
  NANOPDF_JPX_PLM = 0xff57,
  NANOPDF_JPX_PLT = 0xff58,
  NANOPDF_JPX_CRG = 0xff63,
  NANOPDF_JPX_COM = 0xff64,
  NANOPDF_JPX_SOP = 0xff91,
  NANOPDF_JPX_EPH = 0xff92
};

typedef enum nanopdf_jpx_progression_order {
  NANOPDF_JPX_PROGRESSION_LRCP = 0,
  NANOPDF_JPX_PROGRESSION_RLCP = 1,
  NANOPDF_JPX_PROGRESSION_RPCL = 2,
  NANOPDF_JPX_PROGRESSION_PCRL = 3,
  NANOPDF_JPX_PROGRESSION_CPRL = 4
} nanopdf_jpx_progression_order;

typedef enum nanopdf_jpx_wavelet_type {
  NANOPDF_JPX_WAVELET_IRREVERSIBLE_9_7 = 0,
  NANOPDF_JPX_WAVELET_REVERSIBLE_5_3 = 1
} nanopdf_jpx_wavelet_type;

typedef struct nanopdf_jpx_bit_reader {
  const uint8_t* data;
  size_t size;
  size_t pos;
  int bit_pos;
} nanopdf_jpx_bit_reader;

typedef struct nanopdf_jpx_component {
  uint8_t bit_depth;
  uint8_t x_separation;
  uint8_t y_separation;
  uint8_t is_signed;
} nanopdf_jpx_component;

typedef struct nanopdf_jpx_siz_params {
  uint16_t rsiz;
  uint32_t width;
  uint32_t height;
  uint32_t x_offset;
  uint32_t y_offset;
  uint32_t tile_width;
  uint32_t tile_height;
  uint32_t tile_x_offset;
  uint32_t tile_y_offset;
  uint16_t num_components;
  nanopdf_jpx_component* components;
} nanopdf_jpx_siz_params;

typedef struct nanopdf_jpx_cod_params {
  uint8_t use_sop;
  uint8_t use_eph;
  nanopdf_jpx_progression_order progression_order;
  uint16_t num_layers;
  uint8_t mct;
  uint8_t num_decomp_levels;
  uint8_t codeblock_width;
  uint8_t codeblock_height;
  uint8_t codeblock_style;
  nanopdf_jpx_wavelet_type wavelet;
  uint8_t* precinct_sizes;
  size_t precinct_size_count;
} nanopdf_jpx_cod_params;

typedef struct nanopdf_jpx_qcd_step_size {
  uint16_t mantissa;
  uint8_t exponent;
} nanopdf_jpx_qcd_step_size;

typedef struct nanopdf_jpx_qcd_params {
  uint8_t quant_style;
  uint8_t num_guard_bits;
  nanopdf_jpx_qcd_step_size* step_sizes;
  size_t step_size_count;
} nanopdf_jpx_qcd_params;

typedef struct nanopdf_jpx_rgn_params {
  uint8_t style;
  uint8_t shift;
} nanopdf_jpx_rgn_params;

typedef struct nanopdf_jpx_poc_entry {
  uint8_t resolution_start;
  uint32_t component_start;
  uint16_t layer_end;
  uint8_t resolution_end;
  uint32_t component_end;
  nanopdf_jpx_progression_order progression_order;
} nanopdf_jpx_poc_entry;

typedef struct nanopdf_jpx_crg_offset {
  uint16_t x;
  uint16_t y;
} nanopdf_jpx_crg_offset;

typedef struct nanopdf_jpx_comment {
  uint16_t registration;
  uint8_t* data;
  size_t size;
} nanopdf_jpx_comment;

typedef struct nanopdf_jpx_indexed_data {
  uint8_t index;
  uint8_t* data;
  size_t size;
} nanopdf_jpx_indexed_data;

typedef struct nanopdf_jpx_packet_length_marker {
  uint8_t index;
  uint32_t* lengths;
  size_t length_count;
} nanopdf_jpx_packet_length_marker;

typedef struct nanopdf_jpx_tlm_marker {
  uint8_t index;
  uint8_t style;
  uint8_t* data;
  size_t size;
} nanopdf_jpx_tlm_marker;

typedef struct nanopdf_jpx_tile_part_header {
  uint16_t tile_index;
  uint8_t tile_part_index;
  uint8_t num_tile_parts;
  uint32_t tile_part_length;
} nanopdf_jpx_tile_part_header;

typedef struct nanopdf_jpx_header {
  nanopdf_jpx_siz_params siz;
  nanopdf_jpx_cod_params cod;
  nanopdf_jpx_qcd_params qcd;
  nanopdf_jpx_cod_params* component_cod;
  uint8_t* has_component_cod;
  nanopdf_jpx_qcd_params* component_qcd;
  uint8_t* has_component_qcd;
  nanopdf_jpx_rgn_params* component_rgn;
  uint8_t* has_component_rgn;
  nanopdf_jpx_poc_entry* poc_entries;
  size_t poc_entry_count;
  nanopdf_jpx_crg_offset* crg_offsets;
  size_t crg_offset_count;
  nanopdf_jpx_comment* comments;
  size_t comment_count;
  nanopdf_jpx_tlm_marker* tlm_markers;
  size_t tlm_marker_count;
  nanopdf_jpx_packet_length_marker* plm_markers;
  size_t plm_marker_count;
  nanopdf_jpx_packet_length_marker* plt_markers;
  size_t plt_marker_count;
  nanopdf_jpx_indexed_data* ppm_markers;
  size_t ppm_marker_count;
  nanopdf_jpx_indexed_data* ppt_markers;
  size_t ppt_marker_count;
  size_t first_tile_part_offset;
} nanopdf_jpx_header;

typedef struct nanopdf_jpx_mq_decoder {
  const uint8_t* data;
  size_t size;
  size_t pos;
  uint32_t c;
  uint16_t a;
  int ct;
  uint8_t cx_states[19];
  uint8_t cx_mps[19];
} nanopdf_jpx_mq_decoder;

enum {
  NANOPDF_JPX_CODEBLOCK_STATE_SIG = 0x01,
  NANOPDF_JPX_CODEBLOCK_STATE_VISITED = 0x02,
  NANOPDF_JPX_CODEBLOCK_STATE_REFINED = 0x04
};

typedef struct nanopdf_jpx_tag_tree_node {
  int value;
  uint8_t known;
} nanopdf_jpx_tag_tree_node;

typedef struct nanopdf_jpx_tag_tree_level {
  int width;
  int height;
  nanopdf_jpx_tag_tree_node* nodes;
} nanopdf_jpx_tag_tree_level;

typedef struct nanopdf_jpx_tag_tree {
  int width;
  int height;
  size_t level_count;
  nanopdf_jpx_tag_tree_level* levels;
} nanopdf_jpx_tag_tree;

typedef struct nanopdf_jpx_codeblock {
  int x0;
  int y0;
  int width;
  int height;
  int num_passes;
  int zero_bit_planes;
  int num_len_bits;
  uint8_t included;
  const uint8_t* data;
  size_t data_size;
  int* pass_lengths;
  size_t pass_length_count;
  size_t pass_length_capacity;
  uint8_t packet_included;
  size_t packet_pass_length_start;
  uint8_t* owned_data;
  size_t owned_data_size;
  int32_t* coeffs;
  size_t coeff_count;
} nanopdf_jpx_codeblock;

typedef enum nanopdf_jpx_subband_type {
  NANOPDF_JPX_SUBBAND_LL = 0,
  NANOPDF_JPX_SUBBAND_HL = 1,
  NANOPDF_JPX_SUBBAND_LH = 2,
  NANOPDF_JPX_SUBBAND_HH = 3
} nanopdf_jpx_subband_type;

typedef struct nanopdf_jpx_subband {
  nanopdf_jpx_subband_type type;
  int x0;
  int y0;
  int width;
  int height;
  int res_level;
  int band_index;
  int num_xcb;
  int num_ycb;
  nanopdf_jpx_codeblock* codeblocks;
  size_t codeblock_count;
} nanopdf_jpx_subband;

typedef struct nanopdf_jpx_res_level {
  int level;
  int x0;
  int y0;
  int width;
  int height;
  int precinct_width;
  int precinct_height;
  int num_precincts_x;
  int num_precincts_y;
  nanopdf_jpx_subband* subbands;
  size_t subband_count;
} nanopdf_jpx_res_level;

typedef struct nanopdf_jpx_tile_component {
  int x0;
  int y0;
  int width;
  int height;
  nanopdf_jpx_res_level* res_levels;
  size_t res_level_count;
  int32_t* coeffs;
  size_t coeff_count;
} nanopdf_jpx_tile_component;

void nanopdf_jpx_bit_reader_init(
    nanopdf_jpx_bit_reader* reader,
    const uint8_t* data,
    size_t size);
uint32_t nanopdf_jpx_bit_reader_read_bits(nanopdf_jpx_bit_reader* reader, int bits);
int nanopdf_jpx_bit_reader_read_bit(nanopdf_jpx_bit_reader* reader);
void nanopdf_jpx_bit_reader_align_byte(nanopdf_jpx_bit_reader* reader);
int nanopdf_jpx_bit_reader_eof(const nanopdf_jpx_bit_reader* reader);
size_t nanopdf_jpx_bit_reader_position(const nanopdf_jpx_bit_reader* reader);
void nanopdf_jpx_bit_reader_seek(nanopdf_jpx_bit_reader* reader, size_t pos);

void nanopdf_jpx_header_init(nanopdf_jpx_header* header);
void nanopdf_jpx_header_destroy(nanopdf_context* context, nanopdf_jpx_header* header);
int nanopdf_jpx_extract_codestream(
    const uint8_t* data,
    size_t size,
    const uint8_t** out_data,
    size_t* out_size);
int nanopdf_jpx_parse_header(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    nanopdf_jpx_header* header);
int nanopdf_jpx_get_info(
    const uint8_t* data,
    size_t size,
    uint32_t* out_width,
    uint32_t* out_height,
    uint16_t* out_num_components,
    uint8_t* out_bit_depth);
uint32_t nanopdf_jpx_image_width(const nanopdf_jpx_header* header);
uint32_t nanopdf_jpx_image_height(const nanopdf_jpx_header* header);
uint32_t nanopdf_jpx_component_width(
    const nanopdf_jpx_header* header,
    size_t component_index);
uint32_t nanopdf_jpx_component_height(
    const nanopdf_jpx_header* header,
    size_t component_index);

void nanopdf_jpx_mq_decoder_init(
    nanopdf_jpx_mq_decoder* decoder,
    const uint8_t* data,
    size_t size);
void nanopdf_jpx_mq_decoder_reset(nanopdf_jpx_mq_decoder* decoder);
int nanopdf_jpx_mq_decode(nanopdf_jpx_mq_decoder* decoder, int context_index);

void nanopdf_jpx_tag_tree_init(nanopdf_jpx_tag_tree* tree);
void nanopdf_jpx_tag_tree_destroy(nanopdf_context* context, nanopdf_jpx_tag_tree* tree);
int nanopdf_jpx_tag_tree_build(
    nanopdf_context* context,
    nanopdf_jpx_tag_tree* tree,
    int width,
    int height);
int nanopdf_jpx_tag_tree_decode(
    nanopdf_jpx_tag_tree* tree,
    int x,
    int y,
    int threshold,
    nanopdf_jpx_mq_decoder* mq,
    int context_index);
int nanopdf_jpx_tag_tree_value(const nanopdf_jpx_tag_tree* tree, int x, int y);

int nanopdf_jpx_get_significance_context(
    const uint8_t* state,
    int x,
    int y,
    int width,
    int height,
    int subband_type);
int nanopdf_jpx_get_sign_context(
    const uint8_t* state,
    const int32_t* coeffs,
    int x,
    int y,
    int width,
    int height,
    int* out_sign_flip);
int nanopdf_jpx_get_magnitude_refinement_context(
    const uint8_t* state,
    int x,
    int y,
    int width,
    int height);
int nanopdf_jpx_inverse_dwt_53(
    nanopdf_context* context,
    int32_t* data,
    int width,
    int height,
    int levels);
int nanopdf_jpx_inverse_dwt_97(
    nanopdf_context* context,
    float* data,
    int width,
    int height,
    int levels);
void nanopdf_jpx_apply_inverse_mct(
    int32_t** components,
    size_t num_components,
    size_t num_pixels,
    nanopdf_jpx_wavelet_type wavelet);
void nanopdf_jpx_coeffs_to_pixels(
    const int32_t* const* components,
    size_t num_components,
    const uint8_t* bit_depths,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels);
void nanopdf_jpx_codeblock_init(nanopdf_jpx_codeblock* codeblock);
void nanopdf_jpx_codeblock_destroy(nanopdf_context* context, nanopdf_jpx_codeblock* codeblock);
int nanopdf_jpx_decode_codeblock(
    nanopdf_context* context,
    nanopdf_jpx_codeblock* codeblock,
    uint8_t bit_depth,
    uint8_t num_guard_bits,
    int subband_type);
void nanopdf_jpx_tile_component_init(nanopdf_jpx_tile_component* tile_component);
void nanopdf_jpx_tile_component_destroy(
    nanopdf_context* context,
    nanopdf_jpx_tile_component* tile_component);
int nanopdf_jpx_build_tile_component(
    nanopdf_context* context,
    nanopdf_jpx_tile_component* tile_component,
    const nanopdf_jpx_cod_params* cod,
    int tile_x0,
    int tile_y0,
    int tile_x1,
    int tile_y1);
int nanopdf_jpx_parse_tile_part_header(
    const uint8_t* codestream,
    size_t codestream_size,
    size_t offset,
    nanopdf_jpx_tile_part_header* out_header,
    size_t* out_data_offset,
    size_t* out_data_size);
int nanopdf_jpx_parse_tile_part_markers(
    nanopdf_context* context,
    const uint8_t* codestream,
    size_t codestream_size,
    size_t offset,
    nanopdf_jpx_header* header);
int nanopdf_jpx_parse_packet_simple(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    size_t* offset,
    const nanopdf_jpx_cod_params* cod,
    nanopdf_jpx_subband* subbands,
    size_t subband_count);
int nanopdf_jpx_place_codeblock_coeffs(
    nanopdf_jpx_tile_component* tile_component,
    const nanopdf_jpx_subband* subband,
    const nanopdf_jpx_codeblock* codeblock,
    uint8_t num_decomp_levels);
void nanopdf_jpx_dequantize_subband(
    nanopdf_jpx_tile_component* tile_component,
    const nanopdf_jpx_subband* subband,
    const nanopdf_jpx_qcd_params* qcd,
    uint8_t bit_depth,
    uint8_t num_decomp_levels);
int nanopdf_jpx_decode_tile_data_simple(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    const nanopdf_jpx_header* header,
    nanopdf_jpx_tile_component* tile_components,
    size_t tile_component_count);
int nanopdf_jpx_decode_tile_data_with_packet_headers(
    nanopdf_context* context,
    const uint8_t* packet_headers,
    size_t packet_header_size,
    const uint8_t* packet_bodies,
    size_t packet_body_size,
    const nanopdf_jpx_header* header,
    nanopdf_jpx_tile_component* tile_components,
    size_t tile_component_count);
int nanopdf_jpx_reconstruct_pixels(
    nanopdf_context* context,
    const nanopdf_jpx_header* header,
    nanopdf_jpx_tile_component* tile_components,
    size_t tile_component_count,
    uint8_t** out_pixels,
    size_t* out_size);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_JPX_H_ */
