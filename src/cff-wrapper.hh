// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// CFF (Compact Font Format) wrapper - stub header
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace cff_wrapper {

// Check if data is raw CFF (not wrapped in OpenType/OTF)
inline bool is_raw_cff(const uint8_t* data, size_t size) {
  // CFF starts with major version 1, minor version 0
  if (size < 4) return false;
  return data[0] == 1 && data[1] == 0;
}

// Build CID to GID mapping from CFF data (returns vector indexed by CID)
inline std::vector<uint16_t> build_cid_to_gid_map(
    const uint8_t* /*data*/, size_t /*size*/) {
  // Stub - returns empty vector (identity mapping assumed)
  return {};
}

// Wrap raw CFF data in a minimal OpenType container
inline std::vector<uint8_t> wrap_cff_in_opentype(
    const std::vector<uint8_t>& cff_data) {
  // Minimal OTF wrapper: offset table + 1 table record (CFF ) + CFF data
  // OTF offset table: version(4) + numTables(2) + searchRange(2) +
  //                   entrySelector(2) + rangeShift(2) = 12 bytes
  // Table record: tag(4) + checksum(4) + offset(4) + length(4) = 16 bytes
  // Total header: 28 bytes

  std::vector<uint8_t> otf;
  size_t header_size = 12 + 16;  // offset table + 1 table record
  otf.resize(header_size + cff_data.size(), 0);

  // Offset table - OTF version "OTTO"
  otf[0] = 'O'; otf[1] = 'T'; otf[2] = 'T'; otf[3] = 'O';
  // numTables = 1
  otf[4] = 0; otf[5] = 1;
  // searchRange = 16
  otf[6] = 0; otf[7] = 16;
  // entrySelector = 0
  otf[8] = 0; otf[9] = 0;
  // rangeShift = 0
  otf[10] = 0; otf[11] = 0;

  // Table record for "CFF "
  otf[12] = 'C'; otf[13] = 'F'; otf[14] = 'F'; otf[15] = ' ';
  // checksum = 0 (skip)
  // offset = header_size
  uint32_t offset = static_cast<uint32_t>(header_size);
  otf[20] = (offset >> 24) & 0xFF;
  otf[21] = (offset >> 16) & 0xFF;
  otf[22] = (offset >> 8) & 0xFF;
  otf[23] = offset & 0xFF;
  // length
  uint32_t length = static_cast<uint32_t>(cff_data.size());
  otf[24] = (length >> 24) & 0xFF;
  otf[25] = (length >> 16) & 0xFF;
  otf[26] = (length >> 8) & 0xFF;
  otf[27] = length & 0xFF;

  // Copy CFF data
  std::copy(cff_data.begin(), cff_data.end(), otf.begin() + header_size);

  return otf;
}

}  // namespace cff_wrapper
