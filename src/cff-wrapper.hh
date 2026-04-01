// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// CFF (Compact Font Format) wrapper utilities
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace cff_wrapper {

// Check if data is raw CFF (not wrapped in OpenType/OTF)
inline bool is_raw_cff(const uint8_t* data, size_t size) {
  // CFF starts with major version 1, minor version 0
  if (size < 4) return false;
  return data[0] == 1 && data[1] == 0;
}

namespace detail {

// Read a big-endian uint16 from buffer
inline uint16_t read_u16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

// Read a big-endian uint32 from buffer
inline uint32_t read_u32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

// Skip a CFF INDEX structure, returning offset past it.
// An INDEX has: count(2) + offSize(1) + offset[count+1](offSize each) + data.
// Returns 0 on error.
inline size_t skip_index(const uint8_t* data, size_t size, size_t pos) {
  if (pos + 2 > size) return 0;
  uint16_t count = read_u16(data + pos);
  pos += 2;
  if (count == 0) return pos;
  if (pos >= size) return 0;
  uint8_t off_size = data[pos++];
  if (off_size < 1 || off_size > 4) return 0;
  size_t offsets_bytes = static_cast<size_t>(count + 1) * off_size;
  if (pos + offsets_bytes > size) return 0;
  // Read last offset to find end of data
  const uint8_t* last_off_ptr = data + pos + static_cast<size_t>(count) * off_size;
  uint32_t last_off = 0;
  for (uint8_t i = 0; i < off_size; ++i) {
    last_off = (last_off << 8) | last_off_ptr[i];
  }
  pos += offsets_bytes;
  // Offsets are 1-based, so data length is last_off - 1
  if (last_off == 0) return 0;
  pos += last_off - 1;
  if (pos > size) return 0;
  return pos;
}

// Parse a DICT to extract a specific integer operator value.
// CFF DICT: sequence of operands (numbers) followed by operator bytes.
// Returns true if found, sets *out_value.
inline bool dict_find_int(const uint8_t* dict_data, size_t dict_size,
                          uint16_t target_op, int32_t* out_value) {
  size_t pos = 0;
  std::vector<int32_t> operands;

  while (pos < dict_size) {
    uint8_t b0 = dict_data[pos];

    if (b0 >= 32 && b0 <= 246) {
      operands.push_back(static_cast<int32_t>(b0) - 139);
      pos++;
    } else if (b0 >= 247 && b0 <= 250) {
      if (pos + 1 >= dict_size) return false;
      operands.push_back((static_cast<int32_t>(b0) - 247) * 256 + dict_data[pos + 1] + 108);
      pos += 2;
    } else if (b0 >= 251 && b0 <= 254) {
      if (pos + 1 >= dict_size) return false;
      operands.push_back(-(static_cast<int32_t>(b0) - 251) * 256 - dict_data[pos + 1] - 108);
      pos += 2;
    } else if (b0 == 28) {
      if (pos + 2 >= dict_size) return false;
      int16_t val = static_cast<int16_t>((dict_data[pos + 1] << 8) | dict_data[pos + 2]);
      operands.push_back(val);
      pos += 3;
    } else if (b0 == 29) {
      if (pos + 4 >= dict_size) return false;
      int32_t val = static_cast<int32_t>(read_u32(dict_data + pos + 1));
      operands.push_back(val);
      pos += 5;
    } else if (b0 == 30) {
      // Real number - skip nibbles until end marker (0xf)
      pos++;
      while (pos < dict_size) {
        uint8_t b = dict_data[pos++];
        if ((b & 0x0f) == 0x0f || (b >> 4) == 0x0f) break;
      }
      operands.push_back(0); // Placeholder
    } else if (b0 == 12) {
      // Two-byte operator
      if (pos + 1 >= dict_size) return false;
      uint16_t op = static_cast<uint16_t>(0x0C00 | dict_data[pos + 1]);
      if (op == target_op && !operands.empty()) {
        *out_value = operands.back();
        return true;
      }
      operands.clear();
      pos += 2;
    } else if (b0 <= 21) {
      // Single-byte operator
      if (b0 == target_op && !operands.empty()) {
        *out_value = operands.back();
        return true;
      }
      operands.clear();
      pos++;
    } else {
      pos++; // Unknown, skip
    }
  }
  return false;
}

} // namespace detail

inline bool append_gid_cid_pair(std::vector<std::pair<uint16_t, uint16_t>>* pairs,
                                uint16_t gid,
                                uint16_t cid,
                                uint16_t* max_cid) {
  pairs->push_back({gid, cid});
  if (cid > *max_cid) *max_cid = cid;
  return true;
}

inline bool append_gid_cid_pair_checked(
    std::vector<std::pair<uint16_t, uint16_t>>* pairs,
    uint16_t gid,
    uint16_t first,
    uint32_t delta,
    uint16_t* max_cid) {
  if (delta > static_cast<uint32_t>((std::numeric_limits<uint16_t>::max)() - first)) {
    return false;
  }
  return append_gid_cid_pair(
      pairs, gid, static_cast<uint16_t>(first + delta), max_cid);
}

// Build CID to GID mapping from CFF data.
// For CID-keyed fonts, parses the charset to map CIDs to glyph indices.
// Returns a vector where result[cid] = gid.
// Returns empty vector if the font is not CID-keyed or on parse error
// (caller should fall back to identity mapping).
inline std::vector<uint16_t> build_cid_to_gid_map(
    const uint8_t* data, size_t size) {
  if (!data || size < 4) return {};
  if (!is_raw_cff(data, size)) return {};

  // CFF Header: major(1) + minor(1) + hdrSize(1) + offSize(1)
  if (data[2] < 4) return {};
  size_t pos = data[2]; // Skip header

  // Skip Name INDEX
  pos = detail::skip_index(data, size, pos);
  if (pos == 0) return {};

  // Read Top DICT INDEX (usually 1 entry)
  if (pos + 2 > size) return {};
  uint16_t dict_count = detail::read_u16(data + pos);
  if (dict_count == 0) return {};
  size_t dict_index_start = pos;
  pos += 2;
  if (pos >= size) return {};
  uint8_t dict_off_size = data[pos++];
  if (dict_off_size < 1 || dict_off_size > 4) return {};

  // Read first and second offsets to get dict data bounds
  auto read_off = [&](size_t idx) -> uint32_t {
    const uint8_t* p = data + pos + idx * dict_off_size;
    uint32_t val = 0;
    for (uint8_t i = 0; i < dict_off_size; ++i)
      val = (val << 8) | p[i];
    return val;
  };

  uint32_t off1 = read_off(0);
  uint32_t off2 = read_off(1);
  size_t offsets_end = pos + static_cast<size_t>(dict_count + 1) * dict_off_size;
  if (off1 == 0 || off2 <= off1) return {};
  size_t dict_data_start = offsets_end + off1 - 1;
  size_t dict_data_size = off2 - off1;
  if (dict_data_start + dict_data_size > size) return {};

  const uint8_t* dict_data = data + dict_data_start;

  // Check if CID-keyed: look for ROS operator (12 30 = 0x0C1E)
  int32_t dummy;
  bool is_cid = detail::dict_find_int(dict_data, dict_data_size, 0x0C1E, &dummy);
  if (!is_cid) return {}; // Not CID-keyed, identity mapping is correct

  // Find charset offset (operator 15)
  int32_t charset_offset = 0;
  if (!detail::dict_find_int(dict_data, dict_data_size, 15, &charset_offset))
    return {};
  if (charset_offset <= 0 || static_cast<size_t>(charset_offset) >= size)
    return {};

  // Find CharStrings INDEX to get num_glyphs
  int32_t charstrings_offset = 0;
  if (!detail::dict_find_int(dict_data, dict_data_size, 17, &charstrings_offset))
    return {};
  if (charstrings_offset <= 0 || static_cast<size_t>(charstrings_offset) + 2 > size)
    return {};
  uint16_t num_glyphs = detail::read_u16(data + charstrings_offset);
  if (num_glyphs == 0) return {};

  // Parse charset at charset_offset.
  // For CID fonts, charset maps GID -> CID.
  // We build the reverse: CID -> GID.
  size_t cpos = static_cast<size_t>(charset_offset);
  if (cpos >= size) return {};
  uint8_t format = data[cpos++];

  // Find max CID to size the result vector
  uint16_t max_cid = 0;
  std::vector<std::pair<uint16_t, uint16_t>> gid_cid_pairs; // (gid, cid)

  // GID 0 is always .notdef (CID 0)
  gid_cid_pairs.push_back({0, 0});

  if (format == 0) {
    // Format 0: array of SIDs/CIDs, one per glyph (starting from GID 1)
    for (uint16_t gid = 1; gid < num_glyphs; ++gid) {
      if (cpos + 2 > size) break;
      uint16_t cid = detail::read_u16(data + cpos);
      cpos += 2;
      gid_cid_pairs.push_back({gid, cid});
      if (cid > max_cid) max_cid = cid;
    }
  } else if (format == 1) {
    // Format 1: ranges of (first_cid, nLeft)
    uint16_t gid = 1;
    while (gid < num_glyphs && cpos + 3 <= size) {
      uint16_t first = detail::read_u16(data + cpos);
      uint8_t n_left = data[cpos + 2];
      cpos += 3;
      for (int j = 0; j <= n_left && gid < num_glyphs; ++j, ++gid) {
        if (!append_gid_cid_pair_checked(&gid_cid_pairs, gid, first,
                                         static_cast<uint32_t>(j), &max_cid)) {
          break;
        }
      }
    }
  } else if (format == 2) {
    // Format 2: ranges of (first_cid, nLeft as uint16)
    uint16_t gid = 1;
    while (gid < num_glyphs && cpos + 4 <= size) {
      uint16_t first = detail::read_u16(data + cpos);
      uint16_t n_left = detail::read_u16(data + cpos + 2);
      cpos += 4;
      for (int j = 0; j <= n_left && gid < num_glyphs; ++j, ++gid) {
        if (!append_gid_cid_pair_checked(&gid_cid_pairs, gid, first,
                                         static_cast<uint32_t>(j), &max_cid)) {
          break;
        }
      }
    }
  } else {
    return {}; // Unknown format
  }

  // Build CID -> GID map
  std::vector<uint16_t> cid_to_gid(max_cid + 1, 0);
  for (const auto& pair : gid_cid_pairs) {
    if (pair.second < cid_to_gid.size()) {
      cid_to_gid[pair.second] = pair.first;
    }
  }

  return cid_to_gid;
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
