// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Type1 font program parser implementation

#include "type1-parser.hh"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace nanopdf {

namespace {

// Standard encoding (Adobe Type 1 standard)
const char* kStandardEncoding[256] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    "space", "exclam", "quotedbl", "numbersign", "dollar", "percent",
    "ampersand", "quoteright", "parenleft", "parenright", "asterisk", "plus",
    "comma", "hyphen", "period", "slash", "zero", "one", "two", "three",
    "four", "five", "six", "seven", "eight", "nine", "colon", "semicolon",
    "less", "equal", "greater", "question", "at", "A", "B", "C", "D", "E",
    "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
    "T", "U", "V", "W", "X", "Y", "Z", "bracketleft", "backslash",
    "bracketright", "asciicircum", "underscore", "quoteleft", "a", "b", "c",
    "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q",
    "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar",
    "braceright", "asciitilde", nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, "exclamdown", "cent", "sterling",
    "fraction", "yen", "florin", "section", "currency", "quotesingle",
    "quotedblleft", "guillemotleft", "guilsinglleft", "guilsinglright", "fi",
    "fl", nullptr, "endash", "dagger", "daggerdbl", "periodcentered", nullptr,
    "paragraph", "bullet", "quotesinglbase", "quotedblbase", "quotedblright",
    "guillemotright", "ellipsis", "perthousand", nullptr, "questiondown",
    nullptr, "grave", "acute", "circumflex", "tilde", "macron", "breve",
    "dotaccent", "dieresis", nullptr, "ring", "cedilla", nullptr,
    "hungarumlaut", "ogonek", "caron", "emdash", nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, "AE", nullptr,
    "ordfeminine", nullptr, nullptr, nullptr, nullptr, "Lslash", "Oslash",
    "OE", "ordmasculine", nullptr, nullptr, nullptr, nullptr, nullptr, "ae",
    nullptr, nullptr, nullptr, "dotlessi", nullptr, nullptr, "lslash",
    "oslash", "oe", "germandbls", nullptr, nullptr, nullptr, nullptr
};

bool IsWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool IsDelimiter(char c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
         c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

}  // namespace

Type1Parser::Type1Parser() = default;
Type1Parser::~Type1Parser() = default;

bool Type1Parser::Parse(const uint8_t* data, size_t size, Type1FontData& result,
                        bool parse_charstrings) {
  if (!data || size == 0) {
    error_ = "No data provided";
    return false;
  }

  // Try to decode PFB format
  std::vector<uint8_t> decoded;
  const char* fontData;
  size_t fontSize;

  if (UndoPFB(data, size, decoded)) {
    fontData = reinterpret_cast<const char*>(decoded.data());
    fontSize = decoded.size();
  } else {
    // Assume PFA (ASCII) format
    fontData = reinterpret_cast<const char*>(data);
    fontSize = size;
  }

  // Parse the font dictionary
  if (!ParseFontDict(fontData, fontSize, result)) {
    return false;
  }

  // Parse encoding
  if (!ParseEncoding(fontData, fontSize, result)) {
    // Non-fatal - use standard encoding as fallback
    result.uses_standard_encoding = true;
    for (int i = 0; i < 256; ++i) {
      if (kStandardEncoding[i]) {
        result.encoding[i] = kStandardEncoding[i];
      }
    }
  }

  // Parse FontInfo (optional)
  ParseFontInfo(fontData, fontSize, result);

  // Parse Private dictionary (optional)
  ParsePrivate(fontData, fontSize, result);

  // Parse CharStrings if requested (optional)
  if (parse_charstrings) {
    ParseCharStrings(fontData, fontSize, result);
  }

  return true;
}

bool Type1Parser::UndoPFB(const uint8_t* data, size_t size,
                          std::vector<uint8_t>& output) {
  if (size < 6 || data[0] != 0x80) {
    return false;  // Not PFB format
  }

  output.clear();
  output.reserve(size);

  size_t pos = 0;
  while (pos + 6 <= size && data[pos] == 0x80) {
    uint8_t type = data[pos + 1];
    if (type < 1 || type > 2) {
      break;  // End marker (type 3) or invalid
    }

    // Read segment length (little-endian 32-bit)
    uint32_t segLen = static_cast<uint32_t>(data[pos + 2]) |
                      (static_cast<uint32_t>(data[pos + 3]) << 8) |
                      (static_cast<uint32_t>(data[pos + 4]) << 16) |
                      (static_cast<uint32_t>(data[pos + 5]) << 24);

    pos += 6;

    if (pos + segLen > size) {
      break;  // Truncated data
    }

    // Append segment data
    output.insert(output.end(), data + pos, data + pos + segLen);
    pos += segLen;
  }

  return !output.empty();
}

bool Type1Parser::ParseFontDict(const char* data, size_t size,
                                Type1FontData& result) {
  const char* p = data;
  const char* end = data + size;

  // Find /FontName
  const char* fontNamePos = FindName(data, size, "/FontName");
  if (fontNamePos) {
    p = fontNamePos;
    p = SkipWhitespaceAndComments(p, end);
    if (p < end && *p == '/') {
      std::string name;
      if (ParseName(p, end, name)) {
        result.font_name = name;
      }
    }
  }

  // Find /FontBBox
  const char* bboxPos = FindName(data, size, "/FontBBox");
  if (bboxPos) {
    p = bboxPos;
    p = SkipWhitespaceAndComments(p, end);

    // Skip opening bracket or brace
    if (p < end && (*p == '[' || *p == '{')) {
      ++p;
    }

    std::vector<double> bbox;
    if (ParseNumberArray(p, end, bbox) && bbox.size() >= 4) {
      result.font_bbox[0] = bbox[0];
      result.font_bbox[1] = bbox[1];
      result.font_bbox[2] = bbox[2];
      result.font_bbox[3] = bbox[3];
    }
  }

  // Find /FontMatrix
  const char* matrixPos = FindName(data, size, "/FontMatrix");
  if (matrixPos) {
    p = matrixPos;
    p = SkipWhitespaceAndComments(p, end);

    if (p < end && (*p == '[' || *p == '{')) {
      ++p;
    }

    std::vector<double> matrix;
    if (ParseNumberArray(p, end, matrix) && matrix.size() >= 6) {
      for (int i = 0; i < 6; ++i) {
        result.font_matrix[i] = matrix[i];
      }
    }
  }

  return !result.font_name.empty();
}

bool Type1Parser::ParseEncoding(const char* data, size_t size,
                                Type1FontData& result) {
  const char* encPos = FindName(data, size, "/Encoding");
  if (!encPos) {
    return false;
  }

  const char* p = encPos;
  const char* end = data + size;
  p = SkipWhitespaceAndComments(p, end);

  // Check for "StandardEncoding def"
  std::string token = ParseToken(p, end);
  if (token == "StandardEncoding") {
    p = SkipWhitespaceAndComments(p, end);
    token = ParseToken(p, end);
    if (token == "def") {
      result.uses_standard_encoding = true;
      for (int i = 0; i < 256; ++i) {
        if (kStandardEncoding[i]) {
          result.encoding[i] = kStandardEncoding[i];
        }
      }
      return true;
    }
  }

  // Check for "256 array" encoding definition
  // Rewind and try again
  p = encPos;
  p = SkipWhitespaceAndComments(p, end);

  int arraySize = 0;
  if (ParseInteger(p, end, arraySize) && arraySize == 256) {
    p = SkipWhitespaceAndComments(p, end);
    token = ParseToken(p, end);
    if (token == "array") {
      // Parse encoding entries
      // Format: dup <code> /<glyphname> put
      while (p < end) {
        p = SkipWhitespaceAndComments(p, end);
        token = ParseToken(p, end);

        if (token == "def" || token == "readonly") {
          break;  // End of encoding
        }

        if (token == "dup") {
          p = SkipWhitespaceAndComments(p, end);

          // Parse code (may be decimal or octal with 8#)
          int code = -1;
          const char* codeStart = p;

          // Check for octal prefix "8#"
          if (p + 2 < end && p[0] == '8' && p[1] == '#') {
            p += 2;
            // Parse octal number
            int octalValue = 0;
            while (p < end && *p >= '0' && *p <= '7') {
              octalValue = octalValue * 8 + (*p - '0');
              ++p;
            }
            code = octalValue;
          } else {
            ParseInteger(p, end, code);
          }

          if (code < 0 || code > 255) {
            continue;
          }

          p = SkipWhitespaceAndComments(p, end);

          // Check for name directly attached to code (e.g., "33/exclam")
          if (p < end && *p == '/') {
            std::string glyphName;
            if (ParseName(p, end, glyphName)) {
              result.encoding[code] = glyphName;
            }
          } else {
            // Try parsing as separate token
            std::string glyphName;
            if (ParseName(p, end, glyphName)) {
              result.encoding[code] = glyphName;
            }
          }

          // Skip "put"
          p = SkipWhitespaceAndComments(p, end);
          token = ParseToken(p, end);
        }
      }
      return true;
    }
  }

  return false;
}

bool Type1Parser::ParseFontInfo(const char* data, size_t size,
                                Type1FontData& result) {
  // Find /FontInfo begin ... end
  const char* infoPos = FindName(data, size, "/FontInfo");
  if (!infoPos) {
    return false;
  }

  // Find the "begin" after /FontInfo
  const char* p = infoPos;
  const char* end = data + size;

  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);
    std::string token = ParseToken(p, end);
    if (token == "begin") {
      break;
    }
    if (token.empty()) {
      return false;
    }
  }

  // Parse FontInfo entries until "end"
  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);
    std::string token = ParseToken(p, end);

    if (token == "end") {
      break;
    }

    if (token.empty()) {
      break;
    }

    // Check for known entries
    if (token[0] == '/') {
      std::string key = token.substr(1);
      p = SkipWhitespaceAndComments(p, end);

      if (key == "FullName" || key == "FamilyName" || key == "Weight") {
        std::string value;
        if (*p == '(') {
          ParseString(p, end, value);
        }
        if (key == "FullName") result.full_name = value;
        else if (key == "FamilyName") result.family_name = value;
        else if (key == "Weight") result.weight = value;
      } else if (key == "ItalicAngle") {
        ParseReal(p, end, result.italic_angle);
      } else if (key == "isFixedPitch") {
        std::string boolVal = ParseToken(p, end);
        result.is_fixed_pitch = (boolVal == "true");
      } else if (key == "UnderlinePosition") {
        ParseReal(p, end, result.underline_position);
      } else if (key == "UnderlineThickness") {
        ParseReal(p, end, result.underline_thickness);
      }
    }
  }

  return true;
}

bool Type1Parser::ParsePrivate(const char* data, size_t size,
                               Type1FontData& result) {
  // Find /Private
  const char* privPos = FindName(data, size, "/Private");
  if (!privPos) {
    return false;
  }

  const char* p = privPos;
  const char* end = data + size;

  // Skip to "begin"
  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);
    std::string token = ParseToken(p, end);
    if (token == "begin") {
      break;
    }
    if (token.empty()) {
      return false;
    }
  }

  // Parse Private dictionary entries
  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);
    std::string token = ParseToken(p, end);

    if (token == "end" || token == "currentdict" || token == "dup") {
      // End of Private or start of CharStrings
      break;
    }

    if (token.empty()) {
      break;
    }

    if (token[0] == '/') {
      std::string key = token.substr(1);
      p = SkipWhitespaceAndComments(p, end);

      if (key == "BlueValues" || key == "OtherBlues" ||
          key == "FamilyBlues" || key == "FamilyOtherBlues") {
        if (p < end && (*p == '[' || *p == '{')) {
          ++p;
        }
        std::vector<int> values;
        ParseIntArray(p, end, values);
        if (key == "BlueValues") result.blue_values = values;
        else if (key == "OtherBlues") result.other_blues = values;
        else if (key == "FamilyBlues") result.family_blues = values;
        else if (key == "FamilyOtherBlues") result.family_other_blues = values;
      } else if (key == "BlueFuzz") {
        ParseInteger(p, end, result.blue_fuzz);
      } else if (key == "BlueScale") {
        ParseReal(p, end, result.blue_scale);
      } else if (key == "BlueShift") {
        ParseInteger(p, end, result.blue_shift);
      } else if (key == "StdHW") {
        if (p < end && (*p == '[' || *p == '{')) {
          ++p;
        }
        ParseInteger(p, end, result.std_hw);
      } else if (key == "StdVW") {
        if (p < end && (*p == '[' || *p == '{')) {
          ++p;
        }
        ParseInteger(p, end, result.std_vw);
      }
    }
  }

  return true;
}

bool Type1Parser::ParseCharStrings(const char* data, size_t size,
                                   Type1FontData& result) {
  // CharStrings are typically encrypted and complex to parse
  // This is a simplified implementation that just identifies their location
  const char* csPos = FindName(data, size, "/CharStrings");
  if (!csPos) {
    return false;
  }

  // For now, just mark that CharStrings were found
  // Full parsing would require decrypting the eexec section
  return true;
}

const char* Type1Parser::FindName(const char* data, size_t size,
                                  const char* name) {
  size_t nameLen = std::strlen(name);
  const char* end = data + size - nameLen;

  for (const char* p = data; p < end; ++p) {
    if (std::strncmp(p, name, nameLen) == 0) {
      // Make sure it's a complete token
      char nextChar = p[nameLen];
      if (IsWhitespace(nextChar) || IsDelimiter(nextChar) || nextChar == '\0') {
        return p + nameLen;
      }
    }
  }

  return nullptr;
}

const char* Type1Parser::SkipWhitespaceAndComments(const char* p,
                                                    const char* end) {
  while (p < end) {
    if (IsWhitespace(*p)) {
      ++p;
    } else if (*p == '%') {
      // Skip comment to end of line
      while (p < end && *p != '\n' && *p != '\r') {
        ++p;
      }
    } else {
      break;
    }
  }
  return p;
}

std::string Type1Parser::ParseToken(const char*& p, const char* end) {
  p = SkipWhitespaceAndComments(p, end);
  if (p >= end) {
    return "";
  }

  std::string token;

  if (*p == '/') {
    // Name token
    token += *p++;
    while (p < end && !IsWhitespace(*p) && !IsDelimiter(*p)) {
      token += *p++;
    }
  } else if (*p == '(') {
    // String - skip for now, handled separately
    return "";
  } else if (*p == '<' || *p == '>' || *p == '[' || *p == ']' ||
             *p == '{' || *p == '}') {
    token += *p++;
  } else {
    // Regular token
    while (p < end && !IsWhitespace(*p) && !IsDelimiter(*p)) {
      token += *p++;
    }
  }

  return token;
}

bool Type1Parser::ParseInteger(const char*& p, const char* end, int& value) {
  p = SkipWhitespaceAndComments(p, end);
  if (p >= end) {
    return false;
  }

  const char* start = p;
  bool negative = false;

  if (*p == '-') {
    negative = true;
    ++p;
  } else if (*p == '+') {
    ++p;
  }

  if (p >= end || !std::isdigit(static_cast<unsigned char>(*p))) {
    p = start;
    return false;
  }

  int result = 0;
  while (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
    result = result * 10 + (*p - '0');
    ++p;
  }

  value = negative ? -result : result;
  return true;
}

bool Type1Parser::ParseReal(const char*& p, const char* end, double& value) {
  p = SkipWhitespaceAndComments(p, end);
  if (p >= end) {
    return false;
  }

  const char* start = p;
  char* endPtr = nullptr;
  value = std::strtod(p, &endPtr);

  if (endPtr == p) {
    return false;
  }

  p = endPtr;
  return true;
}

bool Type1Parser::ParseString(const char*& p, const char* end,
                              std::string& value) {
  p = SkipWhitespaceAndComments(p, end);
  if (p >= end || *p != '(') {
    return false;
  }

  ++p;  // Skip opening paren
  value.clear();
  int depth = 1;

  while (p < end && depth > 0) {
    if (*p == '\\' && p + 1 < end) {
      ++p;  // Skip backslash
      switch (*p) {
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case '(': value += '('; break;
        case ')': value += ')'; break;
        case '\\': value += '\\'; break;
        default:
          if (*p >= '0' && *p <= '7') {
            // Octal escape
            int octal = *p - '0';
            if (p + 1 < end && p[1] >= '0' && p[1] <= '7') {
              ++p;
              octal = octal * 8 + (*p - '0');
              if (p + 1 < end && p[1] >= '0' && p[1] <= '7') {
                ++p;
                octal = octal * 8 + (*p - '0');
              }
            }
            value += static_cast<char>(octal);
          } else {
            value += *p;
          }
          break;
      }
    } else if (*p == '(') {
      ++depth;
      value += *p;
    } else if (*p == ')') {
      --depth;
      if (depth > 0) {
        value += *p;
      }
    } else {
      value += *p;
    }
    ++p;
  }

  return depth == 0;
}

bool Type1Parser::ParseName(const char*& p, const char* end,
                            std::string& value) {
  p = SkipWhitespaceAndComments(p, end);
  if (p >= end || *p != '/') {
    return false;
  }

  ++p;  // Skip the slash
  value.clear();

  while (p < end && !IsWhitespace(*p) && !IsDelimiter(*p)) {
    value += *p++;
  }

  return !value.empty();
}

bool Type1Parser::ParseNumberArray(const char*& p, const char* end,
                                   std::vector<double>& values) {
  values.clear();

  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);

    if (*p == ']' || *p == '}') {
      ++p;
      break;
    }

    double value;
    if (ParseReal(p, end, value)) {
      values.push_back(value);
    } else {
      break;
    }
  }

  return !values.empty();
}

bool Type1Parser::ParseIntArray(const char*& p, const char* end,
                                std::vector<int>& values) {
  values.clear();

  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);

    if (*p == ']' || *p == '}') {
      ++p;
      break;
    }

    int value;
    if (ParseInteger(p, end, value)) {
      values.push_back(value);
    } else {
      break;
    }
  }

  return !values.empty();
}

}  // namespace nanopdf
