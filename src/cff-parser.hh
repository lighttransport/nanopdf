// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// CFF (Compact Font Format) Parser
// Parses CFF fonts for encoding and charset information

#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace nanopdf {
namespace cff {

// CFF predefined encoding indices
enum class EncodingType {
  Standard = 0,
  Expert = 1,
  Custom = 255
};

// CFF predefined charset indices
enum class CharsetType {
  ISOAdobe = 0,
  Expert = 1,
  ExpertSubset = 2,
  Custom = 255
};

// Parsed CFF data
struct CFFData {
  std::string font_name;
  std::string version;
  std::string notice;
  std::string copyright;
  std::string full_name;
  std::string family_name;
  std::string weight;

  // Encoding: code -> glyph index
  std::vector<int> encoding;

  // Charset: glyph index -> glyph name (SID resolved to string)
  std::vector<std::string> charset;

  // Number of glyphs
  int num_glyphs{0};

  // Is CID-keyed font
  bool is_cid{false};

  // Font matrix
  std::vector<double> font_matrix;

  // Font BBox
  std::vector<double> font_bbox;
};

// CFF Parser class
class CFFParser {
public:
  CFFParser() = default;

  // Parse CFF data from buffer
  // Returns true on success
  bool parse(const uint8_t* data, size_t size, CFFData& result);

private:
  // Read variable-length integer from DICT
  bool read_dict_operand(const uint8_t* data, size_t size, size_t& pos,
                         std::vector<double>& operands);

  // Read INDEX structure
  bool read_index(size_t& count, std::vector<size_t>& offsets);

  // Parse Top DICT
  bool parse_top_dict(const uint8_t* data, size_t size, CFFData& result);

  // Parse String INDEX
  bool parse_string_index();

  // Get SID string (SID < 391 are predefined)
  std::string get_sid_string(int sid);

  // Parse charset
  bool parse_charset(int format, int num_glyphs, CFFData& result);

  // Parse encoding
  bool parse_encoding(int format, CFFData& result);

  // Data members
  const uint8_t* data_{nullptr};
  size_t size_{0};
  size_t pos_{0};

  // Parsed sections
  std::vector<size_t> name_index_offsets_;
  std::vector<size_t> top_dict_index_offsets_;
  std::vector<size_t> string_index_offsets_;
  size_t charset_offset_{0};
  size_t encoding_offset_{0};
  size_t charstrings_offset_{0};

  // String INDEX data
  std::vector<std::string> strings_;
};

// Standard strings (SID 0-390)
const char* get_standard_string(int sid);

// Standard encoding
const char* standard_encoding_name(int code);

// Expert encoding
const char* expert_encoding_name(int code);

// ISO Adobe charset
const char* iso_adobe_charset_name(int glyph_index);

}  // namespace cff
}  // namespace nanopdf
