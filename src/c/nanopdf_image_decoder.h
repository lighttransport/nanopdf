#ifndef NANOPDF_IMAGE_DECODER_H_
#define NANOPDF_IMAGE_DECODER_H_

#include "nanopdf_c_internal.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nanopdf_ccitt_params {
  int columns;
  int rows;
  int k;
  int end_of_line;
  int encoded_byte_align;
  int black_is_1;
} nanopdf_ccitt_params;

typedef struct nanopdf_jbig2_params {
  const uint8_t* globals;
  size_t globals_size;
} nanopdf_jbig2_params;

nanopdf_status nanopdf__decode_dct(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    uint8_t** out_data,
    size_t* out_size);

nanopdf_status nanopdf__decode_ccitt(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    const nanopdf_ccitt_params* params,
    uint8_t** out_data,
    size_t* out_size);

nanopdf_status nanopdf__decode_jpx(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    uint8_t** out_data,
    size_t* out_size);

nanopdf_status nanopdf__decode_jbig2(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    const nanopdf_jbig2_params* params,
    uint8_t** out_data,
    size_t* out_size);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NANOPDF_IMAGE_DECODER_H_ */
