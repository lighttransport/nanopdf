// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: Apache 2.0
//
// TIFF decoder using tinydng library
// Wraps tinydng for decoding TIFF images embedded in PDFs

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nanopdf {
namespace tiff {

// TIFF decode result structure
struct TiffDecodeResult {
  bool success{false};
  std::string error;

  std::vector<uint8_t> pixels;  // Decoded pixel data
  int width{0};
  int height{0};
  int components{0};            // Samples per pixel (1=Gray, 3=RGB, 4=RGBA/CMYK)
  int bits_per_component{8};    // Bits per sample

  // Additional TIFF metadata
  int compression{0};           // Compression type
  int planar_configuration{1};  // 1=chunky, 2=planar
  int photometric{0};           // Photometric interpretation
};

// TIFF decoder class
class TiffDecoder {
 public:
  TiffDecoder() = default;
  ~TiffDecoder() = default;

  // Decode TIFF image from memory buffer
  // @param data Pointer to TIFF data
  // @param size Size of TIFF data in bytes
  // @return TiffDecodeResult with decoded image or error
  TiffDecodeResult decode(const uint8_t* data, size_t size);

  // Get TIFF image information without full decoding
  // @param data Pointer to TIFF data
  // @param size Size of TIFF data in bytes
  // @param[out] width Image width
  // @param[out] height Image height
  // @param[out] components Samples per pixel
  // @return true on success, false on failure
  bool get_info(const uint8_t* data, size_t size,
                int& width, int& height, int& components);
};

}  // namespace tiff
}  // namespace nanopdf
