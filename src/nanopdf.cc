#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstring.h"
#include "nanostring.h"
#else
#include <cstring>
#include <iostream>
#include <string>
#endif

#include "common-macros.inc"
#include "nanopdf.hh"

namespace nanopdf {

namespace {

// parseInt(32bit)
// 0 = success
// -1 = bad input
// -2 = overflow
// -3 = underflow
int parseInt(const std::string &s, int *out_result) {
  size_t n = s.size();
  const char *c = s.c_str();

  if ((c == nullptr) || (*c) == '\0') {
    return -1;
  }

  size_t idx = 0;
  bool negative = c[0] == '-';
  if ((c[0] == '+') | (c[0] == '-')) {
    idx = 1;
    if (n == 1) {
      // sign char only
      return -1;
    }
  }

  int64_t result = 0;

  // allow zero-padded digits(e.g. "003")
  while (idx < n) {
    if ((c[idx] >= '0') && (c[idx] <= '9')) {
      int digit = int(c[idx] - '0');
      result = result * 10 + digit;
    } else {
      // bad input
      return -1;
    }

    if (negative) {
      if ((-result) < (std::numeric_limits<int>::min)()) {
        return -3;
      }
    } else {
      if (result > (std::numeric_limits<int>::max)()) {
        return -2;
      }
    }

    idx++;
  }

  if (negative) {
    (*out_result) = -int(result);
  } else {
  }

  return 0;  // OK
}

int parseInt64(const std::string &s, int64_t *out_result) {
  size_t n = s.size();
  const char *c = s.c_str();

  if ((c == nullptr) || (*c) == '\0') {
    return -1;
  }

  size_t idx = 0;
  bool negative = c[0] == '-';
  if ((c[0] == '+') | (c[0] == '-')) {
    idx = 1;
    if (n == 1) {
      // sign char only
      return -1;
    }
  }

  int64_t result = 0;

  // allow zero-padded digits(e.g. "003")
  while (idx < n) {
    if ((c[idx] >= '0') && (c[idx] <= '9')) {
      if (idx > std::numeric_limits<int64_t>::digits10) {
        // input too long
        return -1;
      }
      int digit = int(c[idx] - '0');
      result = result * 10 + digit;
    } else {
      // bad input
      return -1;
    }

    if (negative) {
      if ((-result) < (std::numeric_limits<int64_t>::min)()) {
        return -3;
      }
    } else {
      if (result > (std::numeric_limits<int64_t>::max)()) {
        return -2;
      }
    }

    idx++;
  }

  if (negative) {
    (*out_result) = -result;
  } else {
  }

  return 0;  // OK
}

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

    // TODO: report true when eof()?
    return false;
  }

  const uint8_t *data() const { return binary_; }

  bool swap_endian() const { return swap_endian_; }

  uint64_t size() const { return length_; }

 private:
  const uint8_t *binary_;
  const uint64_t length_;
  bool swap_endian_;
  char pad_[7];
  mutable uint64_t idx_;
};

namespace detail {

struct Cursor {
  int row{0};
  int col{0};
};

class Parser {
 public:
  Parser(StreamReader &sr) : _sr(sr) {}

  bool Char1(char *c) {
    return _sr.read1(c);
  }

  bool SkipUntilNewline();
  bool Eof() const { return _sr.eof(); }

 private:
  StreamReader &_sr;
  Cursor _cursor;
};

bool Parser::SkipUntilNewline() {
  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }

    if (c == '\n') {
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

bool parse_header(StreamReader &sr, int &minor_version, std::string &err) {
  char buf[8];
  if (!sr.read(8, 8, reinterpret_cast<uint8_t *>(buf))) {
    return false;
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

}  // namespace detail

bool parse_from_memory(const uint8_t *addr, const size_t size) {
  if (size < 8) {
    // too short
    return false;
  }

  bool swap_endian = false;  // TODO
  StreamReader sr(addr, size, swap_endian);

  int minor_version{0};
  std::string err;
  if (!detail::parse_header(sr, minor_version, err)) {
    std::cerr << err << "\n";
    return false;
  }

  DCOUT("minor " << minor_version);
  std::cout << "minor\n";

  return true;
}

}  // namespace nanopdf
