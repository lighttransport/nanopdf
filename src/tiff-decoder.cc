// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: Apache 2.0
//
// TIFF decoder implementation using tinydng

#include "tiff-decoder.hh"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "tiny_dng_loader.h"

namespace nanopdf {
namespace tiff {

TiffDecodeResult TiffDecoder::decode(const uint8_t* data, size_t size) {
  TiffDecodeResult result;

  if (!data || size == 0) {
    result.error = "Invalid input: null data or zero size";
    return result;
  }

  // Check for TIFF magic bytes
  // Little-endian: 0x49 0x49 (II)
  // Big-endian: 0x4D 0x4D (MM)
  if (size < 4) {
    result.error = "Data too small to be a valid TIFF file";
    return result;
  }

  bool is_tiff = (data[0] == 0x49 && data[1] == 0x49) ||  // Little-endian
                 (data[0] == 0x4D && data[1] == 0x4D);    // Big-endian

  if (!is_tiff) {
    result.error = "Not a TIFF file (invalid magic bytes)";
    return result;
  }

  // Use tinydng to decode
  std::vector<tinydng::FieldInfo> custom_fields;
  std::vector<tinydng::DNGImage> images;
  std::string warn;
  std::string err;

  // LoadDNGFromMemory expects char* but we have uint8_t*
  const char* mem = reinterpret_cast<const char*>(data);
  unsigned int mem_size = static_cast<unsigned int>(size);

  // Limit to 2GB for safety (tinydng uses unsigned int)
  if (size > 0x7FFFFFFF) {
    result.error = "TIFF file too large (>2GB)";
    return result;
  }

  bool success = tinydng::LoadDNGFromMemory(mem, mem_size, custom_fields,
                                            &images, &warn, &err);

  if (!success) {
    result.error = "Failed to decode TIFF: " + err;
    return result;
  }

  if (!warn.empty()) {
    // Log warning but continue
    std::cerr << "TIFF decoder warning: " << warn << std::endl;
  }

  if (images.empty()) {
    result.error = "No images found in TIFF file";
    return result;
  }

  // Use the first image (PDFs typically have single-image TIFFs)
  const tinydng::DNGImage& img = images[0];

  // Extract image properties
  result.width = img.width;
  result.height = img.height;
  result.components = img.samples_per_pixel;
  result.bits_per_component = img.bits_per_sample;
  result.compression = img.compression;
  result.planar_configuration = img.planar_configuration;

  // Copy pixel data
  result.pixels = img.data;

  // Validate decoded data size
  size_t expected_size = static_cast<size_t>(result.width) *
                        static_cast<size_t>(result.height) *
                        static_cast<size_t>(result.components) *
                        ((result.bits_per_component + 7) / 8);

  if (result.pixels.size() != expected_size) {
    // Some formats may have different sizes, just warn
    std::cerr << "TIFF decoder: pixel data size mismatch (expected "
              << expected_size << ", got " << result.pixels.size() << ")"
              << std::endl;
  }

  result.success = true;
  return result;
}

bool TiffDecoder::get_info(const uint8_t* data, size_t size,
                           int& width, int& height, int& components) {
  // For now, we need to decode to get info (tinydng doesn't have a header-only parse)
  // This could be optimized in the future
  TiffDecodeResult result = decode(data, size);

  if (!result.success) {
    return false;
  }

  width = result.width;
  height = result.height;
  components = result.components;
  return true;
}

}  // namespace tiff
}  // namespace nanopdf
