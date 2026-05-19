// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Type1 font program parser for nanopdf
// Parses Adobe Type1 font programs to extract:
// - Font name
// - Encoding array (256 glyph names)
// - Font metrics (FontBBox, etc.)

#pragma once

#include "third_party/ttf_parse.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nanopdf {

// Parsed Type1 font data
struct Type1FontData {
  std::string font_name;
  std::array<double, 4> font_bbox{{0, 0, 0, 0}};  // [llx, lly, urx, ury]
  std::array<double, 6> font_matrix{{0.001, 0, 0, 0.001, 0, 0}};

  // Encoding array: 256 glyph names (empty string = .notdef)
  std::array<std::string, 256> encoding;

  // Whether StandardEncoding is used
  bool uses_standard_encoding{false};

  // Font metrics from FontInfo
  std::string full_name;
  std::string family_name;
  std::string weight;
  double italic_angle{0};
  bool is_fixed_pitch{false};
  double underline_position{0};
  double underline_thickness{0};

  // Character widths (glyph name -> width)
  std::map<std::string, int> char_widths;

  // Private dictionary values
  int blue_fuzz{1};
  std::vector<int> blue_values;
  std::vector<int> other_blues;
  std::vector<int> family_blues;
  std::vector<int> family_other_blues;
  double blue_scale{0.039625};
  int blue_shift{7};
  int std_hw{0};  // StdHW
  int std_vw{0};  // StdVW

  // CharStrings (glyph name -> decrypted charstring data)
  // Only populated if parse_charstrings is true
  std::map<std::string, std::vector<uint8_t>> char_strings;

  // Subrs (local subroutines, from Private dict)
  std::vector<std::vector<uint8_t>> subrs;

  // Number of lenIV random prefix bytes (default 4)
  int lenIV{4};
};

// Interpret a Type1 CharString for the given glyph name.
// Returns 0 on success, -1 on failure.
// Caller must free *out with ttf_outline_free().
int t1_glyph_outline(const Type1FontData& t1,
                     const std::string& glyph_name,
                     ttf_outline_t* out);

// Adobe Type 1 binary decryption (RFC 5531 / Type 1 Font Format).
// buffer/len is modified in-place. key is the starting key (55665 for eexec).
void t1_binary_decrypt(uint8_t* buffer, uint32_t len, uint16_t key);

// Adobe Type 1 binary encryption (in-place).
void t1_binary_encrypt(uint8_t* buffer, uint32_t len, uint16_t key);

// Type1 font parser
class Type1Parser {
 public:
  Type1Parser();
  ~Type1Parser();

  // Parse a Type1 font program
  // data: Raw font data (may be PFB or PFA format)
  // size: Size of data
  // result: Output parsed font data
  // parse_charstrings: If true, also parse CharStrings (slower)
  // Returns true on success
  bool Parse(const uint8_t* data, size_t size, Type1FontData& result,
             bool parse_charstrings = false);

  // Get last error message
  const std::string& GetError() const { return error_; }

 private:
  // Detect and decode PFB (Packed Font Binary) format
  // Returns true if PFB was detected and decoded
  bool UndoPFB(const uint8_t* data, size_t size, std::vector<uint8_t>& output);

  // Parse the font dictionary section
  bool ParseFontDict(const char* data, size_t size, Type1FontData& result);

  // Parse encoding from font data
  bool ParseEncoding(const char* data, size_t size, Type1FontData& result);

  // Parse FontInfo dictionary
  bool ParseFontInfo(const char* data, size_t size, Type1FontData& result);

  // Parse Private dictionary
  bool ParsePrivate(const char* data, size_t size, Type1FontData& result);

  // Parse CharStrings dictionary (optional)
  bool ParseCharStrings(const char* data, size_t size, Type1FontData& result);

  // Helper: Find a PostScript name in the data
  // Returns pointer to first char after the name, or nullptr if not found
  const char* FindName(const char* data, size_t size, const char* name);

  // Helper: Skip whitespace and comments
  const char* SkipWhitespaceAndComments(const char* p, const char* end);

  // Helper: Parse a PostScript token
  // Returns token and advances p
  std::string ParseToken(const char*& p, const char* end);

  // Helper: Parse an integer
  bool ParseInteger(const char*& p, const char* end, int& value);

  // Helper: Parse a real number
  bool ParseReal(const char*& p, const char* end, double& value);

  // Helper: Parse a string literal (in parentheses)
  bool ParseString(const char*& p, const char* end, std::string& value);

  // Helper: Parse a name (starting with /)
  bool ParseName(const char*& p, const char* end, std::string& value);

  // Helper: Parse an array of numbers
  bool ParseNumberArray(const char*& p, const char* end,
                        std::vector<double>& values);

  // Helper: Parse an array of integers
  bool ParseIntArray(const char*& p, const char* end,
                     std::vector<int>& values);

  std::string error_;
};

}  // namespace nanopdf
