#include "nanopdf_image_decoder.h"

#include "../jbig2/JBig2_Context.hh"
#include "../jbig2/JBig2_Image.hh"
#include "../jpx-decoder.hh"

#include <cstring>
#include <limits>

namespace {

nanopdf_status set_status(
    nanopdf_context* context,
    nanopdf_status status,
    const char* message) {
  nanopdf__set_error(context, status, message);
  return status;
}

nanopdf_status copy_bridge_bytes(
    nanopdf_context* context,
    const uint8_t* data,
    size_t size,
    uint8_t** out_data,
    size_t* out_size) {
  uint8_t* output = nullptr;
  size_t alloc_size = size == 0 ? 1 : size;

  output = static_cast<uint8_t*>(
      nanopdf__allocator_alloc(&context->allocator, alloc_size));
  if (!output) {
    return set_status(
        context, NANOPDF_STATUS_OUT_OF_MEMORY, "failed to allocate decoded image output");
  }
  if (size > 0) {
    std::memcpy(output, data, size);
  }

  *out_data = output;
  *out_size = size;
  nanopdf__clear_error(context);
  return NANOPDF_STATUS_OK;
}

}  // namespace

extern "C" nanopdf_status nanopdf__decode_jpx(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    uint8_t** out_data,
    size_t* out_size) {
  nanopdf::jpx::JPXDecoder decoder;
  nanopdf::jpx::DecodeResult result;

  if (!context || !input || !out_data || !out_size) {
    return set_status(context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid JPXDecode arguments");
  }
  *out_data = nullptr;
  *out_size = 0;

  result = decoder.decode(input, input_size);
  if (!result.success) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, result.error.c_str());
  }
  if (result.pixels.empty()) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "JPXDecode: no image data decoded");
  }

  return copy_bridge_bytes(
      context, result.pixels.data(), result.pixels.size(), out_data, out_size);
}

extern "C" nanopdf_status nanopdf__decode_jbig2(
    nanopdf_context* context,
    const uint8_t* input,
    size_t input_size,
    const nanopdf_jbig2_params* params,
    uint8_t** out_data,
    size_t* out_size) {
  nanopdf::jbig2::JBig2DecodeResult result;
  int32_t stride = 0;
  size_t data_size = 0;

  if (!context || !input || !out_data || !out_size) {
    return set_status(context, NANOPDF_STATUS_INVALID_ARGUMENT, "invalid JBIG2Decode arguments");
  }
  *out_data = nullptr;
  *out_size = 0;

  const uint8_t* globals = params ? params->globals : nullptr;
  size_t globals_size = params ? params->globals_size : 0;
  nanopdf::jbig2::CJBig2_Context decoder(globals, globals_size, input, input_size, false);
  result = decoder.Decode();

  if (!result.success) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, result.error.c_str());
  }
  if (!result.image || !result.image->has_data()) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "JBIG2Decode: no image data decoded");
  }

  stride = result.image->stride();
  if (stride < 0 ||
      (stride > 0 &&
       static_cast<size_t>(result.height) >
           std::numeric_limits<size_t>::max() / static_cast<size_t>(stride))) {
    return set_status(context, NANOPDF_STATUS_MALFORMED, "JBIG2Decode output size overflow");
  }

  data_size = static_cast<size_t>(stride) * static_cast<size_t>(result.height);
  return copy_bridge_bytes(context, result.image->data(), data_size, out_data, out_size);
}
