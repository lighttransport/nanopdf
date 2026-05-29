// stb_image_write implementation
// This file provides the implementation for stb_image_write.h.
//
// PNG's DEFLATE step is routed through the bundled miniz instead of
// stb_image_write's built-in compressor: miniz deflates several times faster,
// and stb's PNG IDAT is a raw zlib stream — exactly what mz_compress2 emits —
// so the written PNG stays a valid, losslessly-decodable file (only the
// compressed bytes differ; decoded pixels are identical).
//
// Profiling the LightVG rasterizer on a large text PDF showed ~89% of
// wall-clock spent in stb's zlib compressor when writing PNGs; this swap
// targets that hot path.

#include <stdlib.h>

#include "miniz.h"

// Signature required by stb_image_write's STBIW_ZLIB_COMPRESS hook. Returns a
// malloc'd zlib stream that stb frees with STBIW_FREE (== free by default).
static unsigned char* nanopdf_stbiw_zlib_compress(unsigned char* data,
                                                  int data_len, int* out_len,
                                                  int quality) {
  mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(data_len));
  unsigned char* out = static_cast<unsigned char*>(malloc(bound));
  if (!out) {
    *out_len = 0;
    return nullptr;
  }
  // stb passes the configured PNG compression level (stbi_write_png_compression_level)
  // as `quality`. Map it onto miniz's zlib-compatible [1,9] range.
  int level = quality;
  if (level < 1) level = MZ_DEFAULT_LEVEL;
  if (level > 9) level = 9;
  mz_ulong dst_len = bound;
  int rc = mz_compress2(out, &dst_len, data, static_cast<mz_ulong>(data_len),
                        level);
  if (rc != MZ_OK) {
    free(out);
    *out_len = 0;
    return nullptr;
  }
  *out_len = static_cast<int>(dst_len);
  return out;
}

#define STBIW_ZLIB_COMPRESS nanopdf_stbiw_zlib_compress
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
