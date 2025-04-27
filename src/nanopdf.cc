#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstring.h"
#include "nanostring.h"
#else
#include <cstring>
#include <iostream>
#include <string>
#include <cmath>
#endif

#ifndef NANOPDF_USE_MINIZ
#include <zlib.h>
#endif

#include "common-macros.inc"
#include "nanopdf.hh"

namespace nanopdf {

namespace {

static inline void swap2(unsigned short *val) {
  unsigned short tmp = *val;
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[1];
  dst[1] = src[0];
}

static inline void swap4(uint32_t *val) {
  uint32_t tmp = *val;
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static inline void swap4(int *val) {
  int tmp = *val;
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static inline void swap8(uint64_t *val) {
  uint64_t tmp = (*val);
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];
}

static inline void swap8(int64_t *val) {
  int64_t tmp = (*val);
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];
}

} // namespace

namespace detail {

struct Cursor {
  int row{0};
  int col{0};
};

class Parser {
 public:
  explicit Parser(StreamReader &sr) : _sr(sr) {}
  
  bool consume_keyword(StreamReader &sr, const std::string &keyword) {
    for (char c : keyword) {
      if (sr.eof() || sr.get() != c) return false;
    }
    return true;
  }
  
  bool skip_whitespace(StreamReader &sr) {
    while (!sr.eof()) {
      char c = sr.peek();
      if (c != ' ' && c != '\r' && c != '\n' && c != '\t') break;
      sr.seek(sr.pos() + 1);
    }
    return true;
  }

  bool Char1(char *c) {
    if (_sr.eof()) return false;
    *c = _sr.get();
    return true;
  }

  bool SkipUntilNewline();
  bool Eof() const { return _sr.eof(); }

 private:
  StreamReader &_sr;
  Cursor _cursor;
};

bool Parser::SkipUntilNewline() {
  while (!_sr.eof()) {
    char c = _sr.get();

    if (c == '\n') {
      break;
    } else if (c == '\r') {
      // CRLF?
      if (_sr.pos() < (_sr.size() - 1)) {
        char d = _sr.get();
        if (d != '\n') {
          _sr.seek(_sr.pos() - 1);
        }
      }
      break;
    }
  }

  _cursor.row++;
  _cursor.col = 0;
  return true;
}

} // namespace detail

bool parse_stream(StreamReader& sr, detail::Parser& parser, Value* out_value) {
    if (!out_value || out_value->type != Value::DICTIONARY) return false;
    
    // Consume "stream" keyword
    if (!parser.consume_keyword(sr, "stream")) return false;
    
    // Expect either CR+LF or just LF
    char c = sr.get();
    if (c == '\r') {
        if (sr.get() != '\n') return false;
    } else if (c != '\n') {
        return false;
    }
    
    // Get stream length from dictionary
    size_t length = 0;
    auto it = out_value->dict.find("Length");
    if (it == out_value->dict.end() || it->second.type != Value::NUMBER) {
        return false;
    }
    length = static_cast<size_t>(it->second.number);
    
    // Read stream data
    out_value->type = Value::STREAM;
    out_value->stream.data.resize(length);
    if (!sr.read(out_value->stream.data.data(), length)) {
        return false;
    }
    
    // Move dictionary to stream
    out_value->stream.dict = std::move(out_value->dict);
    
    // Expect "endstream"
    if (!parser.skip_whitespace(sr)) return false;
    if (!parser.consume_keyword(sr, "endstream")) return false;
    
    return true;
}

bool parse_header(StreamReader &sr, int &minor_version, std::string &err) {
  char buf[8];
  for (int i = 0; i < 8; i++) {
    if (sr.eof()) return false;
    buf[i] = sr.get();
  }

  if ((buf[0] == '%') && (buf[1] == 'P') && (buf[2] == 'D') &&
      (buf[3] == 'F') && (buf[4] == '-') && (buf[5] == '1') &&
      (buf[6] == '.')) {
    // ok
  } else {
    err += "Invalid header.\n";
    return false;
  }

  if ((buf[7] >= '0') && (buf[7] < '8')) {
    minor_version = buf[7] - '0';
  } else {
    err += "Invalid or unsupported PDF minor version.\n";
    return false;
  }

  return true;
}

// Forward declarations
bool parse_object(StreamReader &sr, detail::Parser &parser, Value *value);
bool parse_dictionary(StreamReader &sr, detail::Parser &parser, Dictionary *dict);
bool parse_array(StreamReader &sr, detail::Parser &parser, std::vector<Value> *arr);
bool parse_name(StreamReader &sr, detail::Parser &parser, std::string *name);
bool parse_string(StreamReader &sr, detail::Parser &parser, std::vector<uint8_t> *str);
bool parse_number(StreamReader &sr, detail::Parser &parser, double *num);
bool parse_reference(StreamReader &sr, detail::Parser &parser, Value *value);
bool parse_stream(StreamReader &sr, detail::Parser &parser, Value *value);

bool parse_object(StreamReader &sr, detail::Parser &parser, Value *value) {
  if (!value) return false;

  // Skip whitespace
  char c;
  while (!sr.eof()) {
    c = sr.get();
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      sr.seek(sr.pos() - 1);
      break;
    }
  }

  c = sr.get();

  if (c == '<') {
    // Dictionary or hex string
    c = sr.get();
    if (c == '<') {
      // Dictionary
      value->type = Value::DICTIONARY;
      if (!parse_dictionary(sr, parser, &value->dict)) return false;
      
      // Check if this is a stream object
      return parse_stream(sr, parser, value);
    } else {
      // Hex string
      sr.seek(sr.pos() - 1);
      value->type = Value::STRING;
      return parse_string(sr, parser, &value->str);
    }
  } else if (c == '[') {
    // Array
    value->type = Value::ARRAY;
    return parse_array(sr, parser, &value->array);
  } else if (c == '/') {
    // Name
    value->type = Value::NAME;
    return parse_name(sr, parser, &value->name);
  } else if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
    // Could be a number or a reference
    sr.seek(sr.pos() - 1);
    
    // Try parsing as reference first
    uint64_t saved_pos = sr.pos();
    if (parse_reference(sr, parser, value)) {
      return true;
    }
    
    // Not a reference, try as number
    sr.seek(saved_pos);
    value->type = Value::NUMBER;
    return parse_number(sr, parser, &value->number);
  } else if (c == 't') {
    // Could be "true"
    value->type = Value::BOOLEAN;
    value->boolean = true;
    return true;
  } else if (c == 'f') {
    // Could be "false"
    value->type = Value::BOOLEAN;
    value->boolean = false;
    return true;
  }

  return false;
}

bool parse_dictionary(StreamReader &sr, detail::Parser &parser, Dictionary *dict) {
  if (!dict) return false;

  char c;
  while (!sr.eof()) {
    // Skip whitespace
    while (!sr.eof()) {
      c = sr.get();
      if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
        sr.seek(sr.pos() - 1);
        break;
      }
    }

    // Check for dictionary end
    c = sr.get();
    if (c == '>') {
      c = sr.get();
      if (c == '>') {
        return true;
      }
      return false;
    }
    sr.seek(sr.pos() - 1);

    // Must be a name
    c = sr.get();
    if (c != '/') return false;

    // Parse key (name)
    std::string key;
    if (!parse_name(sr, parser, &key)) return false;

    // Parse value
    Value value;
    if (!parse_object(sr, parser, &value)) return false;

    (*dict)[key] = value;
  }

  return false;
}

bool parse_array(StreamReader &sr, detail::Parser &parser, std::vector<Value> *arr) {
  if (!arr) return false;

  char c;
  while (!sr.eof()) {
    // Skip whitespace
    while (!sr.eof()) {
      c = sr.get();
      if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
        sr.seek(sr.pos() - 1);
        break;
      }
    }

    // Check for array end
    c = sr.get();
    if (c == ']') {
      return true;
    }
    sr.seek(sr.pos() - 1);

    // Parse value
    Value value;
    if (!parse_object(sr, parser, &value)) return false;
    arr->push_back(value);
  }

  return false;
}

bool parse_name(StreamReader &sr, detail::Parser &parser, std::string *name) {
  if (!name) return false;
  
  char c;
  while (!sr.eof()) {
    c = sr.get();
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t' || 
        c == '/' || c == '[' || c == '(' || c == '<' || c == '>' ||
        c == ']' || c == ')') {
      sr.seek(sr.pos() - 1);
      break;
    }
    name->push_back(c);
  }

  return true;
}

bool parse_hex_string(StreamReader &sr, detail::Parser &parser, std::vector<uint8_t> *str) {
  if (!str) return false;

  char c;
  std::vector<uint8_t> bytes;
  uint8_t current_byte = 0;
  bool first_nibble = true;

  while (!sr.eof()) {
    c = sr.get();
    if (c == '>') {
      if (!first_nibble) {
        bytes.push_back(current_byte);
      }
      break;
    }

    // Skip whitespace
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      continue;
    }

    // Convert hex char to value
    uint8_t nibble;
    if (c >= '0' && c <= '9') {
      nibble = c - '0';
    } else if (c >= 'A' && c <= 'F') {
      nibble = c - 'A' + 10;
    } else if (c >= 'a' && c <= 'f') {
      nibble = c - 'a' + 10;
    } else {
      return false; // Invalid hex character
    }

    if (first_nibble) {
      current_byte = nibble << 4;
      first_nibble = false;
    } else {
      current_byte |= nibble;
      bytes.push_back(current_byte);
      first_nibble = true;
    }
  }

  *str = std::move(bytes);
  return true;
}

bool parse_literal_string(StreamReader &sr, detail::Parser &parser, std::vector<uint8_t> *str) {
  if (!str) return false;

  char c;
  std::vector<uint8_t> bytes;
  int paren_depth = 1;  // We've already consumed the opening (

  while (!sr.eof()) {
    c = sr.get();

    if (c == '(') {
      paren_depth++;
      bytes.push_back(c);
    } else if (c == ')') {
      paren_depth--;
      if (paren_depth == 0) {
        break;
      }
      bytes.push_back(c);
    } else if (c == '\\') {
      // Handle escape sequences
      c = sr.get();

      switch (c) {
        case 'n': bytes.push_back('\n'); break;
        case 'r': bytes.push_back('\r'); break;
        case 't': bytes.push_back('\t'); break;
        case 'b': bytes.push_back('\b'); break;
        case 'f': bytes.push_back('\f'); break;
        case '(': bytes.push_back('('); break;
        case ')': bytes.push_back(')'); break;
        case '\\': bytes.push_back('\\'); break;
        default:
          if (c >= '0' && c <= '7') {
            // Octal escape sequence
            uint8_t val = c - '0';
            for (int i = 0; i < 2; i++) {
              c = sr.get();
              if (sr.eof()) break;
              if (c >= '0' && c <= '7') {
                val = (val << 3) | (c - '0');
              } else {
                sr.seek(sr.pos() - 1);
                break;
              }
            }
            bytes.push_back(val);
          } else {
            bytes.push_back(c);  // Regular character
          }
          break;
      }
    } else {
      bytes.push_back(c);
    }
  }

  *str = std::move(bytes);
  return true;
}

bool parse_string(StreamReader &sr, detail::Parser& parser, std::string* out_str) {
    // Skip initial '(' or '<'
    char c = sr.peek();
    if (c != '(' && c != '<') {
        return false;
    }
    sr.seek(sr.pos() + 1);  // Advance past the opening character

    std::string result;
    if (c == '(') {
        // Parse literal string
        int paren_depth = 1;
        bool escaped = false;

        while (paren_depth > 0 && !sr.eof()) {
            c = sr.get();
            if (escaped) {
                switch (c) {
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case '\\': result += '\\'; break;
                    case '(': result += '('; break;
                    case ')': result += ')'; break;
                    default: result += c;
                }
                escaped = false;
            } else {
                if (c == '\\') {
                    escaped = true;
                } else if (c == '(') {
                    paren_depth++;
                    result += c;
                } else if (c == ')') {
                    paren_depth--;
                    if (paren_depth > 0) {
                        result += c;
                    }
                } else {
                    result += c;
                }
            }
        }
    } else {
        // Parse hex string
        std::string hex;
        while (!sr.eof()) {
            c = sr.get();
            if (c == '>') break;
            if (std::isxdigit(c)) {
                hex += c;
            }
        }
        
        // Convert hex to bytes
        for (size_t i = 0; i < hex.length(); i += 2) {
            char hex_byte[3] = {hex[i], (i + 1 < hex.length()) ? hex[i + 1] : '0', 0};
            char byte = static_cast<char>(std::strtol(hex_byte, nullptr, 16));
            result += byte;
        }
    }

    if (out_str) {
        *out_str = std::move(result);
    }
    return true;
}

bool parse_number(StreamReader &sr, detail::Parser &parser, double *num) {
  if (!num) return false;

  char buf[32];
  int i = 0;
  char c;

  while (!sr.eof() && i < 31) {
    c = sr.peek();
    if (!(c >= '0' && c <= '9') && c != '+' && c != '-' && c != '.') {
      break;
    }
    sr.seek(sr.pos() + 1);  // Advance past the character
    buf[i++] = c;
  }
  buf[i] = '\0';

  *num = strtod(buf, nullptr);
  return true;
}

bool parse_reference(StreamReader &sr, detail::Parser &parser, Value *value) {
  if (!value) return false;

  // Format: "12 0 R" - object number, generation number, R
  char buf[32];
  int i = 0;
  char c;

  // Read object number
  while (!sr.eof() && i < 31) {
    c = sr.get();
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      if (i > 0) break;
      continue;
    }
    if (!(c >= '0' && c <= '9')) {
      sr.seek(sr.pos() - i - 1);
      return false;
    }
    buf[i++] = c;
  }
  if (i == 0) return false;
  buf[i] = '\0';
  value->ref_object_number = strtoul(buf, nullptr, 10);

  // Read generation number
  i = 0;
  while (!sr.eof() && i < 31) {
    c = sr.get();
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      if (i > 0) break;
      continue;
    }
    if (!(c >= '0' && c <= '9')) {
      sr.seek(sr.pos() - i - 1);
      return false;
    }
    buf[i++] = c;
  }
  if (i == 0) return false;
  buf[i] = '\0';
  value->ref_generation_number = static_cast<uint16_t>(strtoul(buf, nullptr, 10));

  // Skip whitespace
  while (!sr.eof()) {
    c = sr.get();
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      break;
    }
  }

  // Must be 'R'
  if (c != 'R') {
    sr.seek(sr.pos() - 1);
    return false;
  }

  value->type = Value::REFERENCE;
  return true;
}

bool parse_indirect_object(StreamReader& sr, detail::Parser& parser, Value* out_value) {
  if (!out_value) return false;

  char c;
  // Skip whitespace
  while (!sr.eof()) {
    c = sr.get();
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      sr.seek(sr.pos() - 1);
      break;
    }
  }

  // Read object number
  uint32_t obj_num = 0;
  while (!sr.eof()) {
    c = sr.get();
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      if (obj_num > 0) break;
      continue;
    }
    if (c < '0' || c > '9') return false;
    obj_num = obj_num * 10 + (c - '0');
  }

  // Read generation number
  uint16_t gen_num = 0;
  while (!sr.eof()) {
    c = sr.get();
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      if (gen_num > 0) break;
      continue;
    }
    if (c < '0' || c > '9') return false;
    gen_num = gen_num * 10 + (c - '0');
  }

  // Skip whitespace until "obj"
  while (!sr.eof()) {
    c = sr.get();
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      sr.seek(sr.pos() - 1);
      break;
    }
  }

  // Read "obj"
  std::string obj_str;
  for (int i = 0; i < 3; i++) {
    if (sr.eof()) return false;
    obj_str.push_back(sr.get());
  }
  if (obj_str != "obj") return false;

  // Parse the object value
  if (!parse_object(sr, parser, out_value)) return false;

  // Skip whitespace until "endobj"
  while (!sr.eof()) {
    c = sr.get();
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      sr.seek(sr.pos() - 1);
      break;
    }
  }

  // Read "endobj"
  std::string endobj_str;
  for (int i = 0; i < 6; i++) {
    if (sr.eof()) return false;
    endobj_str.push_back(sr.get());
  }
  if (endobj_str != "endobj") return false;

  return true;
}

ResolvedObject resolve_reference(const Pdf& pdf, uint32_t obj_num, uint16_t gen_num) {
  ResolvedObject result;

  // Find the xref section containing this object
  const XRefSection* section = nullptr;
  size_t obj_index = 0;

  for (const auto& xs : pdf.xref_sections) {
    if (obj_num >= xs.start_object_id && 
        obj_num < (xs.start_object_id + xs.num_objectsid)) {
      section = &xs;
      obj_index = obj_num - xs.start_object_id;
      break;
    }
  }

  if (!section) {
    result.error = "Object number out of range";
    return result;
  }

  const XRef& xref = section->xrefs[obj_index];
  if (!xref.use || xref.generation != gen_num) {
    result.error = "Invalid generation number or free object";
    return result;
  }

  // Create stream reader positioned at object start
  StreamReader sr(pdf.data, pdf.data_size);
  sr.seek(xref.offset);

  detail::Parser parser(sr);
  if (!parse_indirect_object(sr, parser, &result.value)) {
    result.error = "Failed to parse indirect object";
    return result;
  }

  result.success = true;
  return result;
}

bool parse_from_memory(const uint8_t *addr, const size_t size, Pdf *out_pdf) {
    if (!addr || (size < 8) || !out_pdf) {
        return false;
    }

    out_pdf->data = addr;
    out_pdf->data_size = size;

    // Parse PDF version from header
    if ((addr[0] != '%') || (addr[1] != 'P') || (addr[2] != 'D') || (addr[3] != 'F') || 
        (addr[4] != '-') || (addr[6] != '.')) {
        return false;
    }

    out_pdf->version_major = static_cast<int>(addr[5] - '0');
    out_pdf->version_minor = static_cast<int>(addr[7] - '0');

    return out_pdf->load_document_structure();
}

namespace filters {

namespace {

// Apply PNG predictor row filter
bool apply_predictor(std::vector<uint8_t>& data, const DecodeParams& params) {
  if (params.predictor < 10 || params.predictor > 15) {
    return true; // No predictor or unsupported
  }

  int bytes_per_pixel = (params.bits_per_component * params.colors + 7) / 8;
  int bytes_per_row = (params.bits_per_component * params.colors * params.columns + 7) / 8;
  int row_count = data.size() / (bytes_per_row + 1);

  std::vector<uint8_t> output;
  output.reserve(row_count * bytes_per_row);

  for (int row = 0; row < row_count; row++) {
    int filter = data[row * (bytes_per_row + 1)];
    const uint8_t* row_data = &data[row * (bytes_per_row + 1) + 1];
    
    // Previous row for 'up' filter
    const uint8_t* prev_row = row > 0 ? &output[(row - 1) * bytes_per_row] : nullptr;

    for (int col = 0; col < bytes_per_row; col++) {
      uint8_t left = col >= bytes_per_pixel ? output[row * bytes_per_row + col - bytes_per_pixel] : 0;
      uint8_t up = prev_row ? prev_row[col] : 0;
      uint8_t up_left = (col >= bytes_per_pixel && prev_row) ? prev_row[col - bytes_per_pixel] : 0;

      uint8_t val;
      switch (filter) {
        case 0: // None
          val = row_data[col];
          break;
        case 1: // Sub
          val = row_data[col] + left;
          break;
        case 2: // Up
          val = row_data[col] + up;
          break;
        case 3: // Average
          val = row_data[col] + ((left + up) >> 1);
          break;
        case 4: // Paeth
          {
            int p = left + up - up_left;
            int pa = std::abs(p - left);
            int pb = std::abs(p - up);
            int pc = std::abs(p - up_left);
            if (pa <= pb && pa <= pc) val = row_data[col] + left;
            else if (pb <= pc) val = row_data[col] + up;
            else val = row_data[col] + up_left;
          }
          break;
        default:
          return false;
      }
      output.push_back(val);
    }
  }

  data = std::move(output);
  return true;
}

// Base-85 digits
constexpr uint8_t ascii85_digits[86] = {
  '!', '"', '#', '$', '%', '&', '\'', '(', ')', '*',
  '+', ',', '-', '.', '/', '0', '1', '2', '3', '4',
  '5', '6', '7', '8', '9', ':', ';', '<', '=', '>',
  '?', '@', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
  'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R',
  'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '[', '\\',
  ']', '^', '_', '`', 'a', 'b', 'c', 'd', 'e', 'f',
  'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
  'q', 'r', 's', 't', 'u'
};

} // namespace

DecodeParams parse_decode_params(const Dictionary& params) {
  DecodeParams result;
  
  auto it = params.find("Predictor");
  if (it != params.end() && it->second.type == Value::NUMBER) {
    result.predictor = static_cast<int>(it->second.number);
  }

  it = params.find("Colors");
  if (it != params.end() && it->second.type == Value::NUMBER) {
    result.colors = static_cast<int>(it->second.number);
  }

  it = params.find("BitsPerComponent");
  if (it != params.end() && it->second.type == Value::NUMBER) {
    result.bits_per_component = static_cast<int>(it->second.number);
  }

  it = params.find("Columns");
  if (it != params.end() && it->second.type == Value::NUMBER) {
    result.columns = static_cast<int>(it->second.number);
  }

  it = params.find("EarlyChange");
  if (it != params.end() && it->second.type == Value::NUMBER) {
    result.early_change = static_cast<int>(it->second.number) != 0;
  }

  return result;
}

DecodedStream decode_ascii85(const uint8_t* data, size_t size, const DecodeParams& params) {
  DecodedStream result;
  std::vector<uint8_t> output;
  output.reserve(size * 4 / 5);  // Approximate size

  size_t i = 0;
  uint32_t value = 0;
  int count = 0;

  while (i < size) {
    uint8_t c = data[i++];

    // Skip whitespace
    if (c <= ' ') continue;

    // Handle special cases
    if (c == 'z' && count == 0) {
      output.push_back(0);
      output.push_back(0);
      output.push_back(0);
      output.push_back(0);
      continue;
    }

    if (c == '~' && i < size && data[i] == '>') {
      // End marker
      if (count > 0) {
        value *= pow(85, 5 - count);
        for (int j = 0; j < count - 1; j++) {
          output.push_back((value >> (24 - j * 8)) & 0xFF);
        }
      }
      break;
    }

    // Find digit value
    int digit = -1;
    for (int j = 0; j < 85; j++) {
      if (ascii85_digits[j] == c) {
        digit = j;
        break;
      }
    }

    if (digit < 0) {
      result.error = "Invalid ASCII85 character";
      return result;
    }

    value = value * 85 + digit;
    count++;

    if (count == 5) {
      output.push_back((value >> 24) & 0xFF);
      output.push_back((value >> 16) & 0xFF);
      output.push_back((value >> 8) & 0xFF);
      output.push_back(value & 0xFF);
      value = 0;
      count = 0;
    }
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}

DecodedStream decode_lzw(const uint8_t* data, size_t size, const DecodeParams& params) {
  DecodedStream result;
  std::vector<uint8_t> output;
  output.reserve(size * 2);  // Initial estimate

  // Initialize LZW dictionary
  struct DictEntry {
    std::vector<uint8_t> bytes;
    int prefix_index{-1};
    uint8_t append_char{0};
  };
  std::vector<DictEntry> dict;
  dict.reserve(4096);

  // Initialize with single byte entries
  for (int i = 0; i < 256; i++) {
    dict.push_back({{static_cast<uint8_t>(i)}, -1, static_cast<uint8_t>(i)});
  }

  // Clear code and end code
  int clear_code = 256;
  int end_code = 257;
  int next_code = 258;
  int curr_code_size = 9;
  uint32_t bit_buffer = 0;
  int bits_in_buffer = 0;
  int dict_limit = 512;

  size_t i = 0;
  int old_code = -1;

  auto get_next_code = [&]() -> int {
    while (bits_in_buffer < curr_code_size) {
      if (i >= size) return end_code;
      bit_buffer = (bit_buffer << 8) | data[i++];
      bits_in_buffer += 8;
    }

    int code = (bit_buffer >> (bits_in_buffer - curr_code_size)) & ((1 << curr_code_size) - 1);
    bits_in_buffer -= curr_code_size;
    bit_buffer &= (1 << bits_in_buffer) - 1;
    return code;
  };

  while (true) {
    int code = get_next_code();
    if (code == end_code) break;
    
    if (code == clear_code) {
      // Reset dictionary
      dict.resize(258);
      next_code = 258;
      curr_code_size = 9;
      dict_limit = 512;
      old_code = -1;
      continue;
    }

    if (code >= dict.size()) {
      if (old_code < 0) {
        result.error = "Invalid LZW data";
        return result;
      }

      // Special case for code not yet in dictionary
      const auto& entry = dict[old_code];
      output.insert(output.end(), entry.bytes.begin(), entry.bytes.end());
      output.push_back(entry.bytes[0]);
    } else {
      // Output the decoded bytes
      const auto& entry = dict[code];
      output.insert(output.end(), entry.bytes.begin(), entry.bytes.end());
    }

    // Add new code to dictionary
    if (old_code >= 0 && next_code < 4096) {
      const auto& old_entry = dict[old_code];
      const auto& curr_entry = dict[code];
      dict.push_back({old_entry.bytes, old_code, curr_entry.bytes[0]});
      
      if (++next_code == dict_limit) {
        if (curr_code_size < 12) {
          curr_code_size++;
          dict_limit <<= 1;
        }
      }
    }

    old_code = code;
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}

DecodedStream decode_flate(const uint8_t* data, size_t size, const DecodeParams& params) {
  DecodedStream result;

#ifdef NANOPDF_USE_MINIZ
  // TODO: Add miniz implementation
  result.error = "Flate decode not implemented";
  return result;
#else
  // Use zlib
  z_stream strm;
  memset(&strm, 0, sizeof(strm));
  
  if (inflateInit(&strm) != Z_OK) {
    result.error = "Failed to initialize zlib";
    return result;
  }

  strm.next_in = const_cast<uint8_t*>(data);
  strm.avail_in = size;

  std::vector<uint8_t> output;
  output.resize(size * 2);  // Initial estimate
  
  strm.next_out = output.data();
  strm.avail_out = output.size();

  while (true) {
    int ret = inflate(&strm, Z_NO_FLUSH);
    if (ret == Z_STREAM_END) {
      break;
    }
    if (ret != Z_OK) {
      inflateEnd(&strm);
      result.error = "Flate decode failed";
      return result;
    }

    if (strm.avail_out == 0) {
      // Need more output space
      size_t curr_size = output.size();
      output.resize(curr_size * 2);
      strm.next_out = output.data() + curr_size;
      strm.avail_out = curr_size;
    }
  }

  output.resize(strm.total_out);
  inflateEnd(&strm);

  // Handle predictor if present
  if (params.predictor > 1) {
    if (!apply_predictor(output, params)) {
      result.error = "Predictor processing failed";
      return result;
    }
  }

  result.data = std::move(output);
  result.success = true;
  return result;
#endif
}

DecodedStream decode_ascii_hex(const uint8_t* data, size_t size, const DecodeParams& params) {
  DecodedStream result;
  std::vector<uint8_t> output;
  output.reserve(size / 2);  // Hex encoding uses 2 chars per byte

  size_t i = 0;
  uint8_t current_byte = 0;
  bool first_nibble = true;

  auto hex_to_int = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };

  while (i < size) {
    char c = data[i++];

    // Skip whitespace
    if (c <= ' ') continue;

    // Check for end marker
    if (c == '>') {
      if (!first_nibble) {
        // Handle last byte with missing nibble (treat as 0)
        output.push_back(current_byte);
      }
      break;
    }

    int value = hex_to_int(c);
    if (value < 0) {
      result.error = "Invalid hex character in ASCIIHexDecode stream";
      return result;
    }

    if (first_nibble) {
      current_byte = value << 4;
      first_nibble = false;
    } else {
      current_byte |= value;
      output.push_back(current_byte);
      first_nibble = true;
    }
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}

DecodedStream decode_runlength(const uint8_t* data, size_t size, const DecodeParams& params) {
  DecodedStream result;
  std::vector<uint8_t> output;
  output.reserve(size * 2);  // Initial estimate

  size_t i = 0;
  while (i < size) {
    uint8_t length = data[i++];
    if (length == 0x80) {  // EOD marker
      break;
    }
    
    if (length < 0x80) {  // Copy next (length + 1) bytes
      uint32_t count = length + 1;
      if (i + count > size) {
        result.error = "RunLengthDecode: Data truncated";
        return result;
      }
      output.insert(output.end(), data + i, data + i + count);
      i += count;
    } else {  // Repeat next byte (257 - length) times
      if (i >= size) {
        result.error = "RunLengthDecode: Data truncated";
        return result;
      }
      uint32_t count = 257 - length;
      uint8_t byte = data[i++];
      output.insert(output.end(), count, byte);
    }
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}

} // namespace filters

DecodedStream decode_stream(const Pdf& pdf, const Value& stream_obj) {
    DecodedStream result;
    if (stream_obj.type != Value::STREAM) {
        result.error = "Not a stream object";
        return result;
    }

    // Get filter type from stream dictionary
    auto filter_it = stream_obj.stream.dict.find("Filter");
    if (filter_it == stream_obj.stream.dict.end()) {
        // No filter, return raw data
        result.data = stream_obj.stream.data;
        result.success = true;
        return result;
    }

    // Get decode parameters if present
    filters::DecodeParams params;
    auto params_it = stream_obj.stream.dict.find("DecodeParms");
    if (params_it != stream_obj.stream.dict.end()) {
        params = filters::parse_decode_params(params_it->second.dict);
    }

    // Handle different filter types
    std::string filter_name = filter_it->second.name;
    if (filter_name == "FlateDecode") {
        return filters::decode_flate(stream_obj.stream.data.data(), 
                                   stream_obj.stream.data.size(), 
                                   params);
    } else if (filter_name == "ASCII85Decode") {
        return filters::decode_ascii85(stream_obj.stream.data.data(),
                                     stream_obj.stream.data.size(),
                                     params);
    } else if (filter_name == "LZWDecode") {
        return filters::decode_lzw(stream_obj.stream.data.data(),
                                 stream_obj.stream.data.size(),
                                 params);
    }

    result.error = "Unsupported filter type: " + filter_name;
    return result;
}

bool parse_object_stream(StreamReader& sr, detail::Parser& parser, const Value& stream_obj, std::vector<Value>& out_objects) {
  if (stream_obj.type != Value::STREAM) {
    return false;
  }

  // Get N (number of objects) and First (offset to stream data)
  auto it = stream_obj.stream.dict.find("N");
  if (it == stream_obj.stream.dict.end() || it->second.type != Value::NUMBER) {
    return false;
  }
  int num_objects = static_cast<int>(it->second.number);

  it = stream_obj.stream.dict.find("First");
  if (it == stream_obj.stream.dict.end() || it->second.type != Value::NUMBER) {
    return false;
  }
  uint64_t first_offset = static_cast<uint64_t>(it->second.number);

  // Read object numbers and offsets
  std::vector<std::pair<int, uint64_t>> obj_offsets;
  obj_offsets.reserve(num_objects);

  for (int i = 0; i < num_objects; i++) {
    parser.skip_whitespace(sr);

    // Read object number and offset
    double obj_num, offset;
    if (!parse_number(sr, parser, &obj_num)) {
      return false;
    }

    parser.skip_whitespace(sr);

    if (!parse_number(sr, parser, &offset)) {
      return false;
    }

    obj_offsets.push_back(std::make_pair(
        static_cast<int>(obj_num),
        static_cast<uint64_t>(offset) + first_offset));
  }

  // Read the objects
  for (const auto& obj_info : obj_offsets) {
    sr.seek(obj_info.second);

    Value obj;
    if (!parse_object(sr, parser, &obj)) {
      return false;
    }

    out_objects.push_back(std::move(obj));
  }

  return true;
}

bool Pdf::load_document_structure() {
  // Load catalog dictionary
  ResolvedObject result = load_object(root, 0);
  if (!result.success) {
    return false;
  }

  if (result.value.type != Value::DICTIONARY) {
    return false;
  }

  catalog.object_number = root;

  // Get catalog version if present
  auto it = result.value.dict.find("Version");
  if (it != result.value.dict.end() && it->second.type == Value::NAME) {
    catalog.version = it->second.name;
  }

  // Load pages tree
  it = result.value.dict.find("Pages");
  if (it == result.value.dict.end() || it->second.type != Value::REFERENCE) {
    return false;
  }

  // Get pages root object
  uint32_t pages_root = it->second.ref_object_number;
  uint16_t pages_gen = it->second.ref_generation_number;

  result = load_object(pages_root, pages_gen);
  if (!result.success || result.value.type != Value::DICTIONARY) {
    return false;
  }

  // Parse pages tree
  struct PageTreeWalker {
    const Pdf* pdf;
    DocumentCatalog* catalog;
    ResolvedObject* result;

    bool traverse_pages(const Value& node) {
      // Check node type
      auto type_it = node.dict.find("Type");
      if (type_it == node.dict.end() || type_it->second.type != Value::NAME) {
        return false;
      }

      if (type_it->second.name == "Pages") {
        // Internal node - process children
        auto kids_it = node.dict.find("Kids");
        if (kids_it == node.dict.end() || kids_it->second.type != Value::ARRAY) {
          return false;
        }

        // Load each child
        for (const auto& kid : kids_it->second.array) {
          if (kid.type != Value::REFERENCE) {
            return false;
          }

          *result = pdf->load_object(kid.ref_object_number, kid.ref_generation_number);
          if (!result->success) {
            return false;
          }

          if (!traverse_pages(result->value)) {
            return false;
          }
        }
        return true;

      } else if (type_it->second.name == "Page") {
        // Leaf node - create page object
        Page page;
        page.object_number = node.ref_object_number;
        page.page_number = catalog->pages.size();

        // Get page resources
        auto res_it = node.dict.find("Resources");
        if (res_it != node.dict.end()) {
          if (res_it->second.type == Value::DICTIONARY) {
            page.resources = res_it->second.dict;
          } else if (res_it->second.type == Value::REFERENCE) {
            *result = pdf->load_object(res_it->second.ref_object_number, 
                                     res_it->second.ref_generation_number);
            if (result->success && result->value.type == Value::DICTIONARY) {
              page.resources = result->value.dict;
            }
          }
        }

        catalog->pages.push_back(std::move(page));
        return true;
      }

      return false;
    }
  };

  PageTreeWalker walker{this, &catalog, &result};
  if (!walker.traverse_pages(result.value)) {
    return false;
  }

  catalog.pages_count = catalog.pages.size();
  return true;
}

ResolvedObject Pdf::load_object(uint32_t obj_num, uint16_t gen_num) const {
    // First try resolving directly
    ResolvedObject result = resolve_reference(*this, obj_num, gen_num);
    if (!result.success) {
        return result;
    }

    // Check if this is an object stream (Type = ObjStm)
    if (result.value.type == Value::STREAM) {
        auto type_it = result.value.stream.dict.find("Type");
        if (type_it != result.value.stream.dict.end() && 
            type_it->second.type == Value::NAME &&
            type_it->second.name == "ObjStm") {
            
            // Decode the stream first
            DecodedStream decoded = decode_stream(*this, result.value);
            if (!decoded.success) {
                result.success = false;
                result.error = "Failed to decode object stream";
                return result;
            }

            // Create stream reader for decoded data
            StreamReader sr(decoded.data.data(), decoded.data.size());
            detail::Parser parser(sr);
            
            // Parse the object stream
            std::vector<Value> stream_objects;
            if (!parse_object_stream(sr, parser, result.value, stream_objects)) {
                result.success = false;
                result.error = "Failed to parse object stream";
                return result;
            }

            // Objects in stream always have generation number 0
            if (gen_num != 0) {
                result.success = false;
                result.error = "Invalid generation number for object stream object";
                return result;
            }

            result.value = std::move(stream_objects[obj_num]);
        }
    }

    return result;
}

const Page* Pdf::get_page(uint32_t page_number) const {
  if (page_number >= catalog.pages.size()) {
    return nullptr;
  }
  return &catalog.pages[page_number];
}

PageContent Page::load_contents(const Pdf& pdf) const {
  PageContent result;
  std::vector<uint8_t> combined_data;

  // Load each content stream
  for (const auto& content : contents) {
    if (content.type == Value::REFERENCE) {
      // Load referenced content stream
      ResolvedObject resolved = pdf.load_object(content.ref_object_number, 
                                              content.ref_generation_number);
      if (!resolved.success) {
        result.error = "Failed to load content stream: " + resolved.error;
        return result;
      }

      if (resolved.value.type != Value::STREAM) {
        result.error = "Content object is not a stream";
        return result;
      }

      // Decode the stream
      DecodedStream decoded = decode_stream(pdf, resolved.value);
      if (!decoded.success) {
        result.error = "Failed to decode content stream: " + decoded.error;
        return result;
      }

      // Append decoded data
      combined_data.insert(combined_data.end(), 
                         decoded.data.begin(), 
                         decoded.data.end());

    } else if (content.type == Value::STREAM) {
      // Direct stream object
      DecodedStream decoded = decode_stream(pdf, content);
      if (!decoded.success) {
        result.error = "Failed to decode content stream: " + decoded.error;
        return result;
      }

      combined_data.insert(combined_data.end(), 
                         decoded.data.begin(), 
                         decoded.data.end());
    }
  }

  result.data = std::move(combined_data);
  result.success = true;
  return result;
}

bool Pdf::parse_font_resources(Page& page, const Dictionary& resources) {
  auto font_it = resources.find("Font");
  if (font_it == resources.end() || font_it->second.type != Value::DICTIONARY) {
    return true; // No fonts is okay
  }

  for (const auto& entry : font_it->second.dict) {
    const std::string& font_name = entry.first;
    const Value& font_ref = entry.second;
    
    auto font = parse_font(font_ref);
    if (font) {
      page.fonts[font_name] = std::move(font);
    }
  }

  return true;
}

std::unique_ptr<BaseFont> Pdf::parse_font(const Value& font_val) {
  Value resolved_font = std::move(font_val);
  if (resolved_font.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*this, resolved_font.ref_object_number, resolved_font.ref_generation_number);
    if (!resolved.success) {
      return nullptr;
    }
    resolved_font = std::move(resolved.value);
  }
  
  if (resolved_font.type != Value::DICTIONARY) {
    return nullptr;
  }
  
  auto font = std::make_unique<BaseFont>();
  
  auto it = resolved_font.dict.find("Subtype");
  if (it == resolved_font.dict.end() || it->second.type != Value::NAME) {
    return nullptr;
  }
  font->subtype = it->second.name;
  
  it = resolved_font.dict.find("BaseFont");
  if (it != resolved_font.dict.end() && it->second.type == Value::NAME) {
    font->base_font = it->second.name;
  }
  
  it = resolved_font.dict.find("Encoding");
  if (it != resolved_font.dict.end() && it->second.type == Value::NAME) {
    font->encoding = it->second.name;
  }
  
  it = resolved_font.dict.find("FontDescriptor");
  if (it != resolved_font.dict.end()) {
    auto descriptor = parse_font_descriptor(it->second);
    if (descriptor) {
      font->descriptor = descriptor.release();
    }
  }
  
  return font;
}

std::unique_ptr<FontDescriptor> Pdf::parse_font_descriptor(const Value& font_dict) {
  Value resolved_dict = std::move(font_dict);
  if (resolved_dict.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*this, resolved_dict.ref_object_number, resolved_dict.ref_generation_number);
    if (!resolved.success) {
      return nullptr;
    }
    resolved_dict = std::move(resolved.value);
  }
  
  if (resolved_dict.type != Value::DICTIONARY) {
    return nullptr;
  }
  
  auto descriptor = std::make_unique<FontDescriptor>();
  
  auto it = resolved_dict.dict.find("FontName");
  if (it != resolved_dict.dict.end() && it->second.type == Value::NAME) {
    descriptor->font_name = it->second.name;
  }
  
  it = resolved_dict.dict.find("FontFamily");
  if (it != resolved_dict.dict.end() && it->second.type == Value::NAME) {
    descriptor->font_family = it->second.name;
  }
  
  it = resolved_dict.dict.find("FontFile");
  if (it != resolved_dict.dict.end()) {
    descriptor->font_file = std::move(it->second);
  } else {
    it = resolved_dict.dict.find("FontFile2");
    if (it != resolved_dict.dict.end()) {
      descriptor->font_file = std::move(it->second);
    } else {
      it = resolved_dict.dict.find("FontFile3");
      if (it != resolved_dict.dict.end()) {
        descriptor->font_file = std::move(it->second);
      }
    }
  }
  
  return descriptor;
}

void Value::destroy() {
    switch (type) {
        case STRING:
            str.~basic_string();
            break;
        case NAME:
            name.~basic_string();
            break;
        case ARRAY:
            array.~vector();
            break;
        case DICTIONARY:
            dict.~map();
            break;
        case STREAM:
            stream.~StreamValue();
            break;
        default:
            break;
    }
    type = UNDEFINED;
}

void Value::clear() {
    destroy();
}

Value& Value::operator=(const Value& other) {
    if (this != &other) {
        clear();
        type = other.type;
        
        switch (type) {
            case BOOLEAN:
                boolean = other.boolean;
                break;
            case NUMBER:
                number = other.number;
                break;
            case STRING:
                new (&str) std::string(other.str);
                break;
            case NAME:
                new (&name) std::string(other.name);
                break;
            case ARRAY:
                new (&array) std::vector<Value>(other.array);
                break;
            case DICTIONARY:
                new (&dict) Dictionary(other.dict);
                break;
            case STREAM:
                new (&stream) StreamValue(other.stream);
                break;
            case REFERENCE:
                ref_object_number = other.ref_object_number;
                ref_generation_number = other.ref_generation_number;
                break;
            default:
                break;
        }
    }
    return *this;
}

Value& Value::operator=(Value&& other) noexcept {
    if (this != &other) {
        clear();
        type = other.type;
        
        switch (type) {
            case BOOLEAN:
                boolean = other.boolean;
                break;
            case NUMBER:
                number = other.number;
                break;
            case STRING:
                new (&str) std::string(std::move(other.str));
                break;
            case NAME:
                new (&name) std::string(std::move(other.name));
                break;
            case ARRAY:
                new (&array) std::vector<Value>(std::move(other.array));
                break;
            case DICTIONARY:
                new (&dict) Dictionary(std::move(other.dict));
                break;
            case STREAM:
                new (&stream) StreamValue(std::move(other.stream));
                break;
            case REFERENCE:
                ref_object_number = other.ref_object_number;
                ref_generation_number = other.ref_generation_number;
                break;
            default:
                break;
        }
        other.type = UNDEFINED;
    }
    return *this;
}

} // namespace nanopdf
