#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstring.h"
#include "nanostring.h"
#else
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#endif

#include <zlib.h>

#include "common-macros.inc"
#include "crypto.hh"
#include "jpx-decoder.hh"
#include "nanopdf-log.hh"
#include "nanopdf.hh"
#include "stb_image.h"
#include "stream-reader.hh"

namespace nanopdf {

namespace {
#include "adobe_glyph_list.inc"

bool is_system_little_endian() {
  const uint16_t value = 0x0102;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
  return bytes[0] == 0x02;
}

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
    (*out_result) = int(result);
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
    (*out_result) = result;
  }

  return 0;  // OK
}

}  // namespace

// namespace detail {

struct Cursor {
  int row{0};
  int col{0};
};

class Parser {
 public:
  explicit Parser(StreamReader &sr) : _sr(sr) {}

  bool consume_keyword(StreamReader &sr, const std::string &keyword) {
    for (char c : keyword) {
      char got;
      if (sr.eof() || !sr.read1(&got) || got != c) return false;
    }
    return true;
  }

  bool skip_whitespace(StreamReader &sr) {
    while (true) {
      char c;
      if (!Look(&c)) {
        return true;
      }

      if (c == '%') {
        // Comment. Consume '%' and skip until newline.
        _sr.seek_from_current(1);
        if (!SkipUntilNewline()) {
          return false;
        }
        continue;
      }

      if (c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' ||
          c == '\0') {
        _sr.seek_from_current(1);
        continue;
      }

      break;
    }
    return true;
  }

  bool Char1(char *c) { return _sr.read1(c); }

  bool SkipUntilNewline();
  bool Eof() const { return _sr.eof(); }

 private:
  bool Look(char *c) {
    char cval;
    if (!_sr.read1(&cval)) return false;

    _sr.seek_from_current(-1);

    (*c) = cval;
    return true;
  }

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
    } else if (c == '\r') {
      // CRLF?
      char next;
      if (Char1(&next) && next != '\n') {
        // Not CRLF, step back
        _sr.seek_from_current(-1);
      }
      break;
    }
  }

  _cursor.row++;
  _cursor.col = 0;
  return true;
}

//}  // namespace detail

bool parse_stream(StreamReader &sr, Parser &parser, Value *out_value) {
  if (!out_value || out_value->type != Value::DICTIONARY) return false;

  // Consume "stream" keyword
  if (!parser.consume_keyword(sr, "stream")) return false;

  // Expect either CR+LF or just LF
  char c;
  if (!sr.read1(&c)) return false;
  if (c == '\r') {
    char next;
    if (!sr.read1(&next) || next != '\n') return false;
  } else if (c != '\n') {
    return false;
  }

  // Get stream length from dictionary
  size_t length = 0;
  auto it = out_value->dict.find("Length");
  if (it == out_value->dict.end()) {
    return false;
  }
  if (it->second.type == Value::NUMBER) {
    length = static_cast<size_t>(it->second.number);
  } else if (it->second.type == Value::REFERENCE) {
    // Length is an indirect reference - we need to find the endstream keyword
    // to determine the actual length, as we can't resolve references here
    uint64_t stream_start = sr.tell();
    const uint8_t* data = sr.data() + stream_start;
    size_t remaining = static_cast<size_t>(sr.size() - stream_start);

    NANOPDF_LOG_DEBUG("parse_stream",
                       "Length is indirect ref %u %u, stream_start=%lu, remaining=%zu",
                       it->second.ref_object_number, it->second.ref_generation_number,
                       (unsigned long)stream_start, remaining);
    NANOPDF_LOG_TRACE("parse_stream", "First bytes: %s",
                      log::format_hex_bytes(data, remaining, 50).c_str());

    // Search for "endstream" keyword
    const char* endstream_kw = "endstream";
    const size_t endstream_len = 9;

    for (size_t i = 0; i + endstream_len <= remaining; i++) {
      if (std::memcmp(data + i, endstream_kw, endstream_len) == 0) {
        // Found endstream - check that it's preceded by whitespace
        if (i > 0) {
          char before = static_cast<char>(data[i - 1]);
          if (before == '\n' || before == '\r') {
            // Trim trailing whitespace from stream data
            length = i;
            while (length > 0) {
              char c = static_cast<char>(data[length - 1]);
              if (c == '\n' || c == '\r') {
                length--;
              } else {
                break;
              }
            }
            NANOPDF_LOG_DEBUG("parse_stream", "Found endstream at i=%zu, length=%zu", i, length);
            break;
          }
        }
      }
    }

    if (length == 0) {
      return false;  // Couldn't find endstream
    }
  } else {
    return false;
  }

  Dictionary dict_copy = std::move(out_value->dict);

  // Read stream data
  out_value->SetType(Value::STREAM);
  out_value->stream.data.resize(length);
  if (!sr.read(length, out_value->stream.data.data())) {
    return false;
  }

  // Move dictionary to stream
  out_value->stream.dict = std::move(dict_copy);

  // Expect "endstream"
  if (!parser.skip_whitespace(sr)) return false;
  if (!parser.consume_keyword(sr, "endstream")) return false;

  return true;
}

// Forward declarations
bool parse_object(StreamReader &sr, Parser &parser, Value *value);
bool parse_dictionary(StreamReader &sr, Parser &parser, Dictionary *dict);
bool parse_array(StreamReader &sr, Parser &parser, std::vector<Value> *arr);
bool parse_name(StreamReader &sr, Parser &parser, std::string *name);
bool parse_string(StreamReader &sr, Parser &parser, std::string *str);
bool parse_number(StreamReader &sr, Parser &parser, double *num);
bool parse_reference(StreamReader &sr, Parser &parser, Value *value);
bool parse_stream(StreamReader &sr, Parser &parser, Value *value);

inline bool is_whitespace_char(char c) {
  return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' ||
         c == '\0';
}

inline bool is_delimiter_char(char c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
         c == '{' || c == '}' || c == '/' || c == '%' || c == '\r' ||
         c == '\n' || c == '\t' || c == '\f' || c == ' ';
}

inline bool consume_keyword_rest(StreamReader &sr, const char *rest) {
  for (const char *p = rest; *p != '\0'; ++p) {
    uint8_t ch;
    if (!sr.read1(&ch) || static_cast<char>(ch) != *p) {
      return false;
    }
  }
  return true;
}

inline bool ensure_token_delimiter(StreamReader &sr) {
  uint8_t ch;
  if (!sr.read1(&ch)) {
    return true;  // EOF counts as delimiter
  }

  if (!is_delimiter_char(static_cast<char>(ch))) {
    sr.seek_from_current(-1);
    return false;
  }

  sr.seek_from_current(-1);
  return true;
}

inline int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  return -1;
}

bool parse_object(StreamReader &sr, Parser &parser, Value *value) {
  if (!value) return false;

  if (!parser.skip_whitespace(sr)) return false;

  uint8_t uchar;
  if (!sr.read1(&uchar)) return false;
  char c = static_cast<char>(uchar);

  if (c == '<') {
    uint8_t peek;
    if (!sr.read1(&peek)) return false;
    if (static_cast<char>(peek) == '<') {
      value->SetType(Value::DICTIONARY);
      if (!parse_dictionary(sr, parser, &value->dict)) return false;
      uint64_t pos_after_dict = sr.tell();
      if (!parser.skip_whitespace(sr)) return false;
      uint64_t stream_pos = sr.tell();
      uint8_t keyword_buf[6] = {0};
      uint64_t read = sr.read(6, keyword_buf);
      sr.seek_set(stream_pos);
      if (read == 6 && std::memcmp(keyword_buf, "stream", 6) == 0) {
        return parse_stream(sr, parser, value);
      }
      sr.seek_set(pos_after_dict);
      return true;
    } else {
      sr.seek_from_current(-2);
      value->SetType(Value::STRING);
      return parse_string(sr, parser, &value->str);
    }
  } else if (c == '(') {
    sr.seek_from_current(-1);
    value->SetType(Value::STRING);
    return parse_string(sr, parser, &value->str);
  } else if (c == '[') {
    value->SetType(Value::ARRAY);
    return parse_array(sr, parser, &value->array);
  } else if (c == '/') {
    value->SetType(Value::NAME);
    return parse_name(sr, parser, &value->name);
  } else if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
    sr.seek_from_current(-1);
    uint64_t saved_pos = sr.tell();
    if (parse_reference(sr, parser, value)) {
      return true;
    }
    sr.seek_set(saved_pos);
    value->SetType(Value::NUMBER);
    return parse_number(sr, parser, &value->number);
  } else if (c == 't') {
    if (!consume_keyword_rest(sr, "rue")) return false;
    if (!ensure_token_delimiter(sr)) return false;
    value->SetType(Value::BOOLEAN);
    value->boolean = true;
    return true;
  } else if (c == 'f') {
    if (!consume_keyword_rest(sr, "alse")) return false;
    if (!ensure_token_delimiter(sr)) return false;
    value->SetType(Value::BOOLEAN);
    value->boolean = false;
    return true;
  } else if (c == 'n') {
    if (!consume_keyword_rest(sr, "ull")) return false;
    if (!ensure_token_delimiter(sr)) return false;
    value->SetType(Value::NULL_OBJ);
    return true;
  }

  return false;
}

bool parse_dictionary(StreamReader &sr, Parser &parser, Dictionary *dict) {
  if (!dict) return false;

  while (true) {
    if (!parser.skip_whitespace(sr)) return false;

    uint8_t ch;
    if (!sr.read1(&ch)) return false;
    char c = static_cast<char>(ch);

    if (c == '>') {
      uint8_t next;
      if (!sr.read1(&next)) return false;
      if (static_cast<char>(next) != '>') {
        return false;
      }
      return true;
    }

    if (c == '%') {
      if (!parser.SkipUntilNewline()) return false;
      continue;
    }

    if (c != '/') {
      return false;
    }

    std::string key;
    if (!parse_name(sr, parser, &key)) return false;

    Value val;
    if (!parse_object(sr, parser, &val)) return false;

    (*dict)[key] = std::move(val);
  }
}

bool parse_array(StreamReader &sr, nanopdf::Parser &parser,
                 std::vector<Value> *arr) {
  if (!arr) return false;

  while (true) {
    if (!parser.skip_whitespace(sr)) return false;

    uint8_t ch;
    if (!sr.read1(&ch)) return false;
    char c = static_cast<char>(ch);

    if (c == ']') {
      return true;
    }

    if (c == '%') {
      if (!parser.SkipUntilNewline()) return false;
      continue;
    }

    sr.seek_from_current(-1);
    Value value;
    if (!parse_object(sr, parser, &value)) return false;
    arr->push_back(std::move(value));
  }
}

bool parse_reference(StreamReader &sr, Parser &parser, Value *value) {
  if (!value) return false;

  value->SetType(Value::REFERENCE);

  // Format: "12 0 R" - object number, generation number, R
  char buf[32];
  int i = 0;
  char c;

  // Read object number
  while (!sr.eof() && i < 31) {
    if (!sr.read1(&c)) return false;
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      if (i > 0) break;
      continue;
    }
    if (!(c >= '0' && c <= '9')) {
      sr.seek_from_current(-(i + 1));
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
    if (!sr.read1(&c)) return false;
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      if (i > 0) break;
      continue;
    }
    if (!(c >= '0' && c <= '9')) {
      sr.seek_from_current(-(i + 1));
      return false;
    }
    buf[i++] = c;
  }
  if (i == 0) return false;
  buf[i] = '\0';
  value->ref_generation_number =
      static_cast<uint16_t>(strtoul(buf, nullptr, 10));

  // Skip whitespace
  while (!sr.eof()) {
    if (!sr.read1(&c)) return false;
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      break;
    }
 }

  // Must be 'R'
  if (c != 'R') {
    sr.seek_from_current(-1);
    return false;
  }
  return true;
}

bool parse_indirect_object(StreamReader &sr, Parser &parser, Value *out_value) {
  if (!out_value) return false;

  if (!parser.skip_whitespace(sr)) return false;

  auto read_uint = [&](uint32_t *out) -> bool {
    *out = 0;
    bool read_digit = false;
    while (true) {
      uint8_t raw;
      if (!sr.read1(&raw)) {
        break;
      }
      char c = static_cast<char>(raw);
      if (is_whitespace_char(c)) {
        if (read_digit) {
          break;
        }
        continue;
      }
      if (!(c >= '0' && c <= '9')) {
        sr.seek_from_current(-1);
        break;
      }
      read_digit = true;
      *out = (*out * 10) + static_cast<uint32_t>(c - '0');
    }
    return read_digit;
  };

  uint32_t obj_num = 0;
  if (!read_uint(&obj_num)) {
    return false;
  }

  if (!parser.skip_whitespace(sr)) return false;

  uint32_t gen_num = 0;
  if (!read_uint(&gen_num)) {
    return false;
  }

  if (!parser.skip_whitespace(sr)) return false;

  if (!parser.consume_keyword(sr, "obj")) {
    return false;
  }

  Value value;
  if (!parse_object(sr, parser, &value)) {
    return false;
  }

  if (!parser.skip_whitespace(sr)) return false;
  if (!parser.consume_keyword(sr, "endobj")) {
    return false;
  }

  *out_value = std::move(value);
  out_value->ref_object_number = obj_num;
  out_value->ref_generation_number = static_cast<uint16_t>(gen_num);

  return true;
}

bool parse_from_memory(const uint8_t *addr, const size_t size, Pdf *out_pdf) {
  if (!addr || (size < 8) || !out_pdf) {
    return false;
  }

  out_pdf->data = addr;
  out_pdf->data_size = size;
  out_pdf->swap_endian = !is_system_little_endian();

  // Parse PDF version from header
  if ((addr[0] != '%') || (addr[1] != 'P') || (addr[2] != 'D') ||
      (addr[3] != 'F') || (addr[4] != '-') || (addr[6] != '.')) {
    return false;
  }

  out_pdf->version_major = static_cast<int>(addr[5] - '0');
  out_pdf->version_minor = static_cast<int>(addr[7] - '0');

  return out_pdf->load_document_structure();
}

namespace filters {

namespace {

// Maximum output size to prevent memory exhaustion (256 MB)
constexpr size_t kMaxDecodedSize = 256 * 1024 * 1024;

// Validate decode parameters for sanity
std::string validate_params(const DecodeParams &params) {
  if (params.colors < 1 || params.colors > 32) {
    return "Invalid Colors parameter: " + std::to_string(params.colors) +
           " (must be 1-32)";
  }
  if (params.bits_per_component < 1 || params.bits_per_component > 32) {
    return "Invalid BitsPerComponent parameter: " +
           std::to_string(params.bits_per_component) + " (must be 1-32)";
  }
  if (params.columns < 1 || params.columns > 65535) {
    return "Invalid Columns parameter: " + std::to_string(params.columns) +
           " (must be 1-65535)";
  }
  // Check for potential overflow in bytes_per_row calculation
  int64_t bits_per_row =
      static_cast<int64_t>(params.bits_per_component) * params.colors * params.columns;
  if (bits_per_row > static_cast<int64_t>(kMaxDecodedSize) * 8) {
    return "Row size too large: would require " +
           std::to_string((bits_per_row + 7) / 8) + " bytes per row";
  }
  return "";  // No error
}

// Apply PNG predictor row filter
bool apply_predictor(std::vector<uint8_t> &data, const DecodeParams &params,
                     std::string &error) {
  if (params.predictor < 10 || params.predictor > 15) {
    return true;  // No predictor or unsupported
  }

  // Validate parameters first
  std::string validation_error = validate_params(params);
  if (!validation_error.empty()) {
    error = "Predictor parameter validation failed: " + validation_error;
    return false;
  }

  int bytes_per_pixel = (params.bits_per_component * params.colors + 7) / 8;
  int bytes_per_row =
      (params.bits_per_component * params.colors * params.columns + 7) / 8;

  // Check for empty or insufficient data
  if (data.empty()) {
    error = "Predictor: empty input data";
    return false;
  }
  if (bytes_per_row <= 0) {
    error = "Predictor: invalid bytes_per_row calculation";
    return false;
  }

  int row_count = static_cast<int>(data.size() / (bytes_per_row + 1));
  if (row_count <= 0) {
    error = "Predictor: insufficient data for even one row (need " +
            std::to_string(bytes_per_row + 1) + " bytes, have " +
            std::to_string(data.size()) + ")";
    return false;
  }

  std::vector<uint8_t> output;
  output.reserve(row_count * bytes_per_row);

  for (int row = 0; row < row_count; row++) {
    size_t row_offset = static_cast<size_t>(row) * (bytes_per_row + 1);
    // Bounds check before accessing filter byte
    if (row_offset >= data.size()) {
      error = "Predictor: row " + std::to_string(row) +
              " offset out of bounds";
      return false;
    }
    int filter = data[row_offset];

    // Bounds check for row data access
    if (row_offset + 1 + bytes_per_row > data.size()) {
      error = "Predictor: insufficient data for row " + std::to_string(row) +
              " (need " + std::to_string(bytes_per_row) + " bytes)";
      return false;
    }
    const uint8_t *row_data = &data[row_offset + 1];

    // Previous row for 'up' filter
    const uint8_t *prev_row =
        row > 0 ? &output[(row - 1) * bytes_per_row] : nullptr;

    for (int col = 0; col < bytes_per_row; col++) {
      uint8_t left = col >= bytes_per_pixel
                         ? output[row * bytes_per_row + col - bytes_per_pixel]
                         : 0;
      uint8_t up = prev_row ? prev_row[col] : 0;
      uint8_t up_left = (col >= bytes_per_pixel && prev_row)
                            ? prev_row[col - bytes_per_pixel]
                            : 0;

      uint8_t val;
      switch (filter) {
        case 0:  // None
          val = row_data[col];
          break;
        case 1:  // Sub
          val = row_data[col] + left;
          break;
        case 2:  // Up
          val = row_data[col] + up;
          break;
        case 3:  // Average
          val = row_data[col] + ((left + up) >> 1);
          break;
        case 4:  // Paeth
        {
          int p = left + up - up_left;
          int pa = std::abs(p - left);
          int pb = std::abs(p - up);
          int pc = std::abs(p - up_left);
          if (pa <= pb && pa <= pc)
            val = row_data[col] + left;
          else if (pb <= pc)
            val = row_data[col] + up;
          else
            val = row_data[col] + up_left;
        } break;
        default:
          error = "Predictor: unknown filter type " + std::to_string(filter) +
                  " at row " + std::to_string(row);
          return false;
      }
      output.push_back(val);
    }
  }

  data = std::move(output);
  return true;
}

}  // namespace

// Forward declare LZWDecoder class
class LZWDecoder {
 public:
  LZWDecoder() = default;
  bool init(const uint8_t *data, size_t size, bool early_change);
  bool decode(std::vector<uint8_t> &output);

 private:
  const uint8_t *data_;
  size_t size_;
  size_t pos_;
  bool early_change_;
  int bit_pos_;
  uint32_t bit_buffer_;

  bool fill_buffer();
  int get_code(int code_size);
};

bool LZWDecoder::init(const uint8_t *data, size_t size, bool early_change) {
  data_ = data;
  size_ = size;
  pos_ = 0;
  early_change_ = early_change;
  bit_pos_ = 0;
  bit_buffer_ = 0;
  return true;
}

bool LZWDecoder::fill_buffer() {
  if (pos_ >= size_) return false;
  bit_buffer_ = (bit_buffer_ << 8) | data_[pos_++];
  bit_pos_ += 8;
  return true;
}

int LZWDecoder::get_code(int code_size) {
  while (bit_pos_ < code_size) {
    if (!fill_buffer()) return -1;
  }

  int code = bit_buffer_ >> (bit_pos_ - code_size);
  bit_pos_ -= code_size;
  bit_buffer_ &= (1 << bit_pos_) - 1;

  return code;
}

bool LZWDecoder::decode(std::vector<uint8_t> &output) {
  // Implement basic LZW decoding
  const int CLEAR_CODE = 256;
  const int EOD_CODE = 257;
  const int FIRST_CODE = 258;
  const int MAX_CODES = 4096;

  // Initialize the dictionary
  std::vector<std::vector<uint8_t>> dict(FIRST_CODE);
  for (int i = 0; i < 256; i++) {
    dict[i] = {static_cast<uint8_t>(i)};
  }

  int code_size = 9;
  int next_code = FIRST_CODE;

  // Process codes
  int old_code = -1;
  std::vector<uint8_t> string;

  while (true) {
    int code = get_code(code_size);
    if (code < 0) return false;

    if (code == EOD_CODE) {
      break;
    } else if (code == CLEAR_CODE) {
      // Reset the dictionary
      dict.resize(FIRST_CODE);
      next_code = FIRST_CODE;
      code_size = 9;
      old_code = -1;
    } else {
      if (code < next_code) {
        // Code exists in the dictionary
        string = dict[code];
      } else if (code == next_code && old_code != -1) {
        // Special case for code = next_code
        string = dict[old_code];
        string.push_back(string[0]);
      } else {
        return false;  // Invalid code
      }

      // Output the string
      output.insert(output.end(), string.begin(), string.end());

      // Add to the dictionary if we have a previous code
      if (old_code != -1) {
        std::vector<uint8_t> new_string = dict[old_code];
        new_string.push_back(string[0]);
        dict.push_back(new_string);
        next_code++;

        // Increase code size if needed
        if (next_code == (1 << code_size) && next_code < MAX_CODES) {
          code_size++;
        }
      }

      old_code = code;
    }
  }

  return true;
}

DecodeParams parse_decode_params(const Dictionary &params) {
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

  // CCITTFaxDecode parameters
  it = params.find("K");
  if (it != params.end() && it->second.type == Value::NUMBER) {
    result.k = static_cast<int>(it->second.number);
  }

  it = params.find("EndOfLine");
  if (it != params.end() && it->second.type == Value::BOOLEAN) {
    result.end_of_line = it->second.boolean;
  }

  it = params.find("EncodedByteAlign");
  if (it != params.end() && it->second.type == Value::BOOLEAN) {
    result.encoded_byte_align = it->second.boolean;
  }

  it = params.find("EndOfBlock");
  if (it != params.end() && it->second.type == Value::BOOLEAN) {
    result.end_of_block = it->second.boolean;
  }

  it = params.find("BlackIs1");
  if (it != params.end() && it->second.type == Value::BOOLEAN) {
    result.black_is_1 = it->second.boolean;
  }

  it = params.find("Rows");
  if (it != params.end() && it->second.type == Value::NUMBER) {
    result.rows = static_cast<int>(it->second.number);
  }

  it = params.find("DamagedRowsBeforeError");
  if (it != params.end() && it->second.type == Value::NUMBER) {
    result.damaged_rows_before_error = static_cast<int>(it->second.number);
  }

  return result;
}

DecodedStream decode_ascii85(const uint8_t *data, size_t size,
                             const DecodeParams &params) {
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

    // Check for end marker
    if (c == '~' && i < size && data[i] == '>') {
      break;
    }

    // Process regular ASCII85 character
    if (c < '!' || c > 'u') {
      result.error = "Invalid ASCII85 character";
      return result;
    }

    value = value * 85 + (c - '!');
    count++;

    if (count == 5) {
      // Output 4 bytes for every 5 ASCII85 characters
      output.push_back((value >> 24) & 0xFF);
      output.push_back((value >> 16) & 0xFF);
      output.push_back((value >> 8) & 0xFF);
      output.push_back(value & 0xFF);
      value = 0;
      count = 0;
    }
  }

  // Handle remaining bytes
  if (count > 0) {
    // Adjust the value for partial group
    for (int j = count; j < 5; j++) {
      value = value * 85 + 84;  // '!' + 84 = 'u'
    }

    // Output the appropriate number of bytes
    if (count > 1) output.push_back((value >> 24) & 0xFF);
    if (count > 2) output.push_back((value >> 16) & 0xFF);
    if (count > 3) output.push_back((value >> 8) & 0xFF);
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}

DecodedStream decode_asciihex(const uint8_t *data, size_t size,
                              const DecodeParams & /*params*/) {
  DecodedStream result;
  std::vector<uint8_t> output;
  output.reserve(size / 2);

  auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };

  int high_nibble = -1;

  for (size_t i = 0; i < size; ++i) {
    char c = static_cast<char>(data[i]);

    if (c == '>') {
      break;
    }

    if (c == '\0' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == ' ')
      continue;

    int value = hex_value(c);
    if (value < 0) {
      result.error = "ASCIIHexDecode: invalid hex digit";
      return result;
    }

    if (high_nibble < 0) {
      high_nibble = value;
    } else {
      uint8_t byte = static_cast<uint8_t>((high_nibble << 4) | value);
      output.push_back(byte);
      high_nibble = -1;
    }
  }

  if (high_nibble >= 0) {
    output.push_back(static_cast<uint8_t>(high_nibble << 4));
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}

DecodedStream decode_flate(const uint8_t *data, size_t size,
                           const DecodeParams &params) {
  DecodedStream result;
  std::vector<uint8_t> output;

  if (size == 0) {
    result.error = "FlateDecode: empty input data";
    return result;
  }

  // Decompress using zlib
  z_stream strm;
  memset(&strm, 0, sizeof(strm));
  strm.next_in = const_cast<Bytef *>(data);
  strm.avail_in = static_cast<uInt>(size);

  if (inflateInit(&strm) != Z_OK) {
    result.error = "FlateDecode: failed to initialize zlib decompressor";
    return result;
  }

  int ret;
  do {
    // Check for excessive output size
    if (output.size() >= kMaxDecodedSize) {
      inflateEnd(&strm);
      result.error = "FlateDecode: output exceeds maximum size limit (" +
                     std::to_string(kMaxDecodedSize / (1024 * 1024)) + " MB)";
      return result;
    }

    output.resize(output.size() + 4096);
    strm.next_out = output.data() + strm.total_out;
    strm.avail_out = static_cast<uInt>(output.size() - strm.total_out);

    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      std::string zlib_error;
      switch (ret) {
        case Z_DATA_ERROR:
          zlib_error = "corrupted or invalid deflate data";
          break;
        case Z_MEM_ERROR:
          zlib_error = "out of memory";
          break;
        case Z_BUF_ERROR:
          zlib_error = "buffer error (incomplete data)";
          break;
        case Z_STREAM_ERROR:
          zlib_error = "stream state error";
          break;
        default:
          zlib_error = "unknown error (code " + std::to_string(ret) + ")";
          break;
      }
      if (strm.msg) {
        zlib_error += std::string(": ") + strm.msg;
      }
      inflateEnd(&strm);
      result.error = "FlateDecode: " + zlib_error;
      return result;
    }
  } while (ret != Z_STREAM_END);

  inflateEnd(&strm);
  output.resize(strm.total_out);

  // Apply predictor if needed
  std::string predictor_error;
  if (!apply_predictor(output, params, predictor_error)) {
    result.error = "FlateDecode: " + predictor_error;
    return result;
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}

DecodedStream decode_lzw(const uint8_t *data, size_t size,
                         const DecodeParams &params) {
  DecodedStream result;
  std::vector<uint8_t> output;

  // Initialize LZW decoder
  LZWDecoder decoder;
  if (!decoder.init(data, size, params.early_change)) {
    result.error = "LZWDecode: failed to initialize decoder (empty or invalid data)";
    return result;
  }

  // Decode data
  if (!decoder.decode(output)) {
    result.error = "LZWDecode: failed to decode data (invalid code sequence)";
    return result;
  }

  // Apply predictor if needed
  std::string predictor_error;
  if (!apply_predictor(output, params, predictor_error)) {
    result.error = "LZWDecode: " + predictor_error;
    return result;
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}


DecodedStream decode_jbig2(const uint8_t * /*data*/, size_t /*size*/,
                           const DecodeParams & /*params*/) {
  DecodedStream result;
  result.success = false;
  result.error = "JBIG2Decode filter is not implemented.";
  return result;
}

// RunLengthDecode implementation
DecodedStream decode_runlength(const uint8_t *data, size_t size,
                               const DecodeParams & /*params*/) {
  DecodedStream result;
  std::vector<uint8_t> output;

  size_t pos = 0;
  while (pos < size) {
    uint8_t length = data[pos++];

    if (length == 128) {
      // EOD marker
      break;
    } else if (length < 128) {
      // Copy the next length+1 bytes literally
      size_t count = length + 1;
      if (pos + count > size) {
        result.error = "RunLengthDecode: Unexpected end of data";
        return result;
      }
      for (size_t i = 0; i < count; i++) {
        output.push_back(data[pos++]);
      }
    } else {
      // Repeat the next byte (257-length) times
      if (pos >= size) {
        result.error = "RunLengthDecode: Unexpected end of data";
        return result;
      }
      uint8_t byte = data[pos++];
      size_t count = 257 - length;
      for (size_t i = 0; i < count; i++) {
        output.push_back(byte);
      }
    }
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}

// DCTDecode (JPEG) implementation using stb_image
DecodedStream decode_dct(const uint8_t *data, size_t size,
                        const DecodeParams & /*params*/) {
  DecodedStream result;

  // Use stb_image to decode JPEG data
  int width, height, channels;
  unsigned char *decoded = stbi_load_from_memory(data, static_cast<int>(size),
                                                  &width, &height, &channels, 0);
  if (!decoded) {
    result.error = "DCTDecode: Failed to decode JPEG data";
    return result;
  }

  NANOPDF_LOG_DEBUG("DCTDecode", "Decoded JPEG: %dx%d, channels=%d, size=%zu",
                    width, height, channels, static_cast<size_t>(width) * height * channels);

  // Copy decoded data to result
  size_t decoded_size = static_cast<size_t>(width) * height * channels;
  result.data.assign(decoded, decoded + decoded_size);
  stbi_image_free(decoded);
  result.success = true;

  return result;
}

// JPXDecode (JPEG2000) implementation
DecodedStream decode_jpx(const uint8_t *data, size_t size,
                         const DecodeParams & /*params*/) {
  DecodedStream result;

  jpx::JPXDecoder decoder;
  auto decode_result = decoder.decode(data, size);

  if (!decode_result.success) {
    result.error = "JPXDecode: " + decode_result.error;
    return result;
  }

  result.data = std::move(decode_result.pixels);
  result.success = true;
  return result;
}

#ifdef NANOPDF_USE_LIBTIFF
// libtiff-based CCITTFaxDecode implementation
#include <tiffio.h>
#include <cstring>

// Memory buffer for in-memory TIFF operations
struct TIFFMemoryBuffer {
  std::vector<uint8_t> data;
  size_t pos;

  TIFFMemoryBuffer() : pos(0) {}
};

// I/O callbacks for in-memory TIFF
static tmsize_t tiff_mem_read(thandle_t handle, void* buf, tmsize_t size) {
  TIFFMemoryBuffer* mb = reinterpret_cast<TIFFMemoryBuffer*>(handle);
  size_t available = mb->data.size() - mb->pos;
  size_t to_read = std::min(static_cast<size_t>(size), available);
  if (to_read > 0) {
    std::memcpy(buf, mb->data.data() + mb->pos, to_read);
    mb->pos += to_read;
  }
  return static_cast<tmsize_t>(to_read);
}

static tmsize_t tiff_mem_write(thandle_t handle, void* buf, tmsize_t size) {
  TIFFMemoryBuffer* mb = reinterpret_cast<TIFFMemoryBuffer*>(handle);
  size_t new_size = mb->pos + size;
  if (new_size > mb->data.size()) {
    mb->data.resize(new_size);
  }
  std::memcpy(mb->data.data() + mb->pos, buf, size);
  mb->pos += size;
  return size;
}

static uint64_t tiff_mem_seek(thandle_t handle, uint64_t off, int whence) {
  TIFFMemoryBuffer* mb = reinterpret_cast<TIFFMemoryBuffer*>(handle);
  uint64_t new_pos = mb->pos;

  if (whence == SEEK_SET) {
    new_pos = off;
  } else if (whence == SEEK_CUR) {
    new_pos = mb->pos + off;
  } else if (whence == SEEK_END) {
    new_pos = mb->data.size() + off;
  }

  if (new_pos > mb->data.size()) {
    return static_cast<uint64_t>(-1);
  }
  mb->pos = static_cast<size_t>(new_pos);
  return new_pos;
}

static int tiff_mem_close(thandle_t) {
  return 0;
}

static uint64_t tiff_mem_size(thandle_t handle) {
  TIFFMemoryBuffer* mb = reinterpret_cast<TIFFMemoryBuffer*>(handle);
  return mb->data.size();
}

DecodedStream decode_ccittfax_libtiff(const uint8_t* data, size_t size,
                                      const DecodeParams& params) {
  DecodedStream result;

  int width = params.columns > 0 ? params.columns : 1728;
  int height = params.rows > 0 ? params.rows : 1;

  NANOPDF_LOG_TRACE("CCITTFaxDecode_libtiff", "Using libtiff: width=%d, height=%d, k=%d",
                    width, height, params.k);

  // Determine compression type based on k parameter
  // k < 0: Group 4 (COMPRESSION_CCITT_T6)
  // k = 0: Group 3 1D (COMPRESSION_CCITT_T4)
  // k > 0: Group 3 2D (COMPRESSION_CCITT_T4)
  uint16_t compression = (params.k < 0) ? COMPRESSION_CCITT_T6 : COMPRESSION_CCITT_T4;

  // Use temporary file approach (simpler and more reliable)
  char temp_path[] = "/tmp/nanopdf_ccitt_XXXXXX";
  int fd = mkstemp(temp_path);
  if (fd < 0) {
    result.success = false;
    result.error = "Failed to create temporary file";
    NANOPDF_LOG_ERROR("CCITTFaxDecode_libtiff", "%s", result.error.c_str());
    return result;
  }
  close(fd);

  // Write TIFF with raw CCITT data
  TIFF* tif = TIFFOpen(temp_path, "w");
  if (!tif) {
    unlink(temp_path);
    result.success = false;
    result.error = "Failed to create TIFF file";
    NANOPDF_LOG_ERROR("CCITTFaxDecode_libtiff", "%s", result.error.c_str());
    return result;
  }

  // Set required TIFF tags
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(width));
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(height));
  TIFFSetField(tif, TIFFTAG_COMPRESSION, compression);
  // PDF's CCITT uses different photometric than TIFF standard
  // Try MINISBLACK if black_is_1 is true, otherwise MINISWHITE
  uint16_t photometric = params.black_is_1 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_MINISWHITE;
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, photometric);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 1);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, static_cast<uint32_t>(height));
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);

  // For Group 3, set additional options
  if (compression == COMPRESSION_CCITT_T4) {
    uint32_t g3opts = 0;
    if (params.k != 0) {
      g3opts |= GROUP3OPT_2DENCODING;
    }
    TIFFSetField(tif, TIFFTAG_GROUP3OPTIONS, g3opts);
  }

  // Write the raw compressed data as a strip
  tmsize_t written = TIFFWriteRawStrip(tif, 0, const_cast<void*>(reinterpret_cast<const void*>(data)), size);

  if (written != static_cast<tmsize_t>(size)) {
    TIFFClose(tif);
    unlink(temp_path);
    result.success = false;
    result.error = "Failed to write CCITT data to TIFF strip";
    NANOPDF_LOG_ERROR("CCITTFaxDecode_libtiff", "%s", result.error.c_str());
    return result;
  }

  TIFFClose(tif);

  // Now open for reading
  tif = TIFFOpen(temp_path, "r");
  if (!tif) {
    unlink(temp_path);
    result.success = false;
    result.error = "Failed to reopen TIFF file for reading";
    NANOPDF_LOG_ERROR("CCITTFaxDecode_libtiff", "%s", result.error.c_str());
    return result;
  }

  // Read decoded data
  int bytes_per_row = (width + 7) / 8;
  size_t output_size = bytes_per_row * height;
  result.data.resize(output_size);

  // Read strip by strip (we have one strip containing all rows)
  tmsize_t read_bytes = TIFFReadEncodedStrip(tif, 0, result.data.data(), output_size);

  TIFFClose(tif);
  unlink(temp_path);

  if (read_bytes < 0) {
    result.success = false;
    result.error = "libtiff failed to decode CCITT data";
    NANOPDF_LOG_ERROR("CCITTFaxDecode_libtiff", "%s", result.error.c_str());
    return result;
  }

  result.data.resize(read_bytes);
  result.success = true;

  NANOPDF_LOG_INFO("CCITTFaxDecode_libtiff",
                   "Successfully decoded %d bytes (expected %zu)",
                   read_bytes, output_size);

  return result;
}
#endif

// CCITTFaxDecode implementation
DecodedStream decode_ccittfax(const uint8_t* data, size_t size,
                              const DecodeParams& params) {
  DecodedStream result;

  NANOPDF_LOG_DEBUG("CCITTFaxDecode", "params: k=%d, columns=%d, rows=%d, eol=%d, align=%d, "
                    "black_is_1=%d, damaged=%d, data=%zu bytes",
                    params.k, params.columns, params.rows, params.end_of_line,
                    params.encoded_byte_align, params.black_is_1,
                    params.damaged_rows_before_error, size);

#ifdef NANOPDF_USE_POPPLER_CCITT
  // Use Poppler's CCITT decoder if available (highest priority - most accurate)
  {
    NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Using Poppler decoder");
    extern std::vector<uint8_t> decode_ccitt_poppler_wrapper(
        const uint8_t* data, size_t size,
        int encoding, bool endOfLine, bool byteAlign,
        int columns, int rows, bool endOfBlock, bool blackIs1);

    auto decoded = decode_ccitt_poppler_wrapper(
        data, size,
        params.k,
        params.end_of_line,
        params.encoded_byte_align,
        params.columns,
        params.rows,
        params.end_of_block,
        params.black_is_1
    );
    result.data = std::move(decoded);
    result.success = true;
    NANOPDF_LOG_INFO("CCITTFaxDecode", "Poppler decoded %zu bytes", result.data.size());
    return result;
  }
#elif defined(NANOPDF_USE_LIBTIFF)
  // Use libtiff for CCITT decoding if available
  NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Using libtiff decoder");
  return decode_ccittfax_libtiff(data, size, params);
#endif

  if (size >= 8) {
    NANOPDF_LOG_TRACE("CCITTFaxDecode", "First bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                      data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
  }

  if (!data || size == 0) {
    result.error = "CCITTFaxDecode: Empty stream";
    return result;
  }

  struct BitReader {
    const uint8_t* data{nullptr};
    size_t size{0};
    size_t byte_pos{0};
    int bit_pos{7};

    BitReader(const uint8_t* d, size_t s) : data(d), size(s) {}

    struct State {
      size_t byte_pos;
      int bit_pos;
    };

    State save() const { return State{byte_pos, bit_pos}; }

    void restore(const State& state) {
      byte_pos = state.byte_pos;
      bit_pos = state.bit_pos;
      if (bit_pos < 0) bit_pos = 0;
      if (bit_pos > 7) bit_pos = 7;
      if (byte_pos > size) byte_pos = size;
    }

    int get_bit() {
      if (byte_pos >= size) {
        return -1;
      }
      int bit = (data[byte_pos] >> bit_pos) & 1;
      if (bit_pos == 0) {
        bit_pos = 7;
        byte_pos++;
      } else {
        bit_pos--;
      }
      return bit;
    }

    // Poppler-style: Look ahead n bits, returning partial data if EOF reached
    // Returns -1 if no bits available, otherwise returns available bits left-shifted
    int look_bits_partial(int n, int* bits_available) {
      if (byte_pos >= size) {
        if (bits_available) *bits_available = 0;
        return -1;
      }

      uint32_t result = 0;
      int count = 0;
      size_t saved_byte = byte_pos;
      int saved_bit = bit_pos;

      while (count < n) {
        if (byte_pos >= size) {
          // Near EOF - return whatever bits we have, left-shifted
          byte_pos = saved_byte;
          bit_pos = saved_bit;
          if (bits_available) *bits_available = count;
          if (count == 0) return -1;
          // Left-shift to fill the requested bit width
          return static_cast<int>(result << (n - count));
        }
        int bit = (data[byte_pos] >> bit_pos) & 1;
        result = (result << 1) | static_cast<uint32_t>(bit);
        count++;
        if (bit_pos == 0) {
          bit_pos = 7;
          byte_pos++;
        } else {
          bit_pos--;
        }
      }

      // Restore position (peek only)
      byte_pos = saved_byte;
      bit_pos = saved_bit;
      if (bits_available) *bits_available = count;
      return static_cast<int>(result);
    }

    int look_bits(int n) {
      int bits_available = 0;
      int value = look_bits_partial(n, &bits_available);
      if (bits_available == 0) {
        return -1;
      }
      return value;
    }

    void eat_bits(int n) {
      for (int i = 0; i < n; ++i) {
        if (get_bit() < 0) {
          break;
        }
      }
    }

    bool align_to_byte() {
      if (byte_pos >= size) {
        return false;
      }
      if (bit_pos != 7) {
        bit_pos = 7;
        ++byte_pos;
      }
      return byte_pos <= size;
    }

    bool skip_eol() {
      int zero_count = 0;
      while (byte_pos < size) {
        int bit = get_bit();
        if (bit < 0) {
          return false;
        }
        if (bit == 0) {
          zero_count++;
        } else {
          if (zero_count >= 11) {
            return true;
          }
          zero_count = 0;
        }
      }
      return false;
    }
  };

  struct CCITTCode {
    int16_t bits;
    int16_t run;
  };

  static constexpr int kCcittEol = -2;

  // Poppler CCITT tables (from ref/poppler/poppler/Stream-CCITT.h).
  static const CCITTCode kWhiteTab1[32] = {
      { -1, -1 }, // 00000
      { 12, kCcittEol }, // 00001
      { -1, -1 },       { -1, -1 }, // 0001x
      { -1, -1 },       { -1, -1 },   { -1, -1 }, { -1, -1 }, // 001xx
      { -1, -1 },       { -1, -1 },   { -1, -1 }, { -1, -1 }, // 010xx
      { -1, -1 },       { -1, -1 },   { -1, -1 }, { -1, -1 }, // 011xx
      { 11, 1792 },     { 11, 1792 }, // 1000x
      { 12, 1984 }, // 10010
      { 12, 2048 }, // 10011
      { 12, 2112 }, // 10100
      { 12, 2176 }, // 10101
      { 12, 2240 }, // 10110
      { 12, 2304 }, // 10111
      { 11, 1856 },     { 11, 1856 }, // 1100x
      { 11, 1920 },     { 11, 1920 }, // 1101x
      { 12, 2368 }, // 11100
      { 12, 2432 }, // 11101
      { 12, 2496 }, // 11110
      { 12, 2560 } // 11111
  };

  static const CCITTCode kWhiteTab2[512] = {
      { -1, -1 },  { -1, -1 },  { -1, -1 },  { -1, -1 }, // 0000000xx
      { 8, 29 },   { 8, 29 }, // 00000010x
      { 8, 30 },   { 8, 30 }, // 00000011x
      { 8, 45 },   { 8, 45 }, // 00000100x
      { 8, 46 },   { 8, 46 }, // 00000101x
      { 7, 22 },   { 7, 22 },   { 7, 22 },   { 7, 22 }, // 0000011xx
      { 7, 23 },   { 7, 23 },   { 7, 23 },   { 7, 23 }, // 0000100xx
      { 8, 47 },   { 8, 47 }, // 00001010x
      { 8, 48 },   { 8, 48 }, // 00001011x
      { 6, 13 },   { 6, 13 },   { 6, 13 },   { 6, 13 }, // 000011xxx
      { 6, 13 },   { 6, 13 },   { 6, 13 },   { 6, 13 },   { 7, 20 },   { 7, 20 },   { 7, 20 },   { 7, 20 }, // 0001000xx
      { 8, 33 },   { 8, 33 }, // 00010010x
      { 8, 34 },   { 8, 34 }, // 00010011x
      { 8, 35 },   { 8, 35 }, // 00010100x
      { 8, 36 },   { 8, 36 }, // 00010101x
      { 8, 37 },   { 8, 37 }, // 00010110x
      { 8, 38 },   { 8, 38 }, // 00010111x
      { 7, 19 },   { 7, 19 },   { 7, 19 },   { 7, 19 }, // 0001100xx
      { 8, 31 },   { 8, 31 }, // 00011010x
      { 8, 32 },   { 8, 32 }, // 00011011x
      { 6, 1 },    { 6, 1 },    { 6, 1 },    { 6, 1 }, // 000111xxx
      { 6, 1 },    { 6, 1 },    { 6, 1 },    { 6, 1 },    { 6, 12 },   { 6, 12 },   { 6, 12 },   { 6, 12 }, // 001000xxx
      { 6, 12 },   { 6, 12 },   { 6, 12 },   { 6, 12 },   { 8, 53 },   { 8, 53 }, // 00100100x
      { 8, 54 },   { 8, 54 }, // 00100101x
      { 7, 26 },   { 7, 26 },   { 7, 26 },   { 7, 26 }, // 0010011xx
      { 8, 39 },   { 8, 39 }, // 00101000x
      { 8, 40 },   { 8, 40 }, // 00101001x
      { 8, 41 },   { 8, 41 }, // 00101010x
      { 8, 42 },   { 8, 42 }, // 00101011x
      { 8, 43 },   { 8, 43 }, // 00101100x
      { 8, 44 },   { 8, 44 }, // 00101101x
      { 7, 21 },   { 7, 21 },   { 7, 21 },   { 7, 21 }, // 0010111xx
      { 7, 28 },   { 7, 28 },   { 7, 28 },   { 7, 28 }, // 0011000xx
      { 8, 61 },   { 8, 61 }, // 00110010x
      { 8, 62 },   { 8, 62 }, // 00110011x
      { 8, 63 },   { 8, 63 }, // 00110100x
      { 8, 0 },    { 8, 0 }, // 00110101x
      { 8, 320 },  { 8, 320 }, // 00110110x
      { 8, 384 },  { 8, 384 }, // 00110111x
      { 5, 10 },   { 5, 10 },   { 5, 10 },   { 5, 10 }, // 00111xxxx
      { 5, 10 },   { 5, 10 },   { 5, 10 },   { 5, 10 },   { 5, 10 },   { 5, 10 },   { 5, 10 },   { 5, 10 },   { 5, 10 },  { 5, 10 },  { 5, 10 },  { 5, 10 },  { 5, 11 },  { 5, 11 },  { 5, 11 },  { 5, 11 }, // 01000xxxx
      { 5, 11 },   { 5, 11 },   { 5, 11 },   { 5, 11 },   { 5, 11 },   { 5, 11 },   { 5, 11 },   { 5, 11 },   { 5, 11 },  { 5, 11 },  { 5, 11 },  { 5, 11 },  { 7, 27 },  { 7, 27 },  { 7, 27 },  { 7, 27 }, // 0100100xx
      { 8, 59 },   { 8, 59 }, // 01001010x
      { 8, 60 },   { 8, 60 }, // 01001011x
      { 9, 1472 }, // 010011000
      { 9, 1536 }, // 010011001
      { 9, 1600 }, // 010011010
      { 9, 1728 }, // 010011011
      { 7, 18 },   { 7, 18 },   { 7, 18 },   { 7, 18 }, // 0100111xx
      { 7, 24 },   { 7, 24 },   { 7, 24 },   { 7, 24 }, // 0101000xx
      { 8, 49 },   { 8, 49 }, // 01010010x
      { 8, 50 },   { 8, 50 }, // 01010011x
      { 8, 51 },   { 8, 51 }, // 01010100x
      { 8, 52 },   { 8, 52 }, // 01010101x
      { 7, 25 },   { 7, 25 },   { 7, 25 },   { 7, 25 }, // 0101011xx
      { 8, 55 },   { 8, 55 }, // 01011000x
      { 8, 56 },   { 8, 56 }, // 01011001x
      { 8, 57 },   { 8, 57 }, // 01011010x
      { 8, 58 },   { 8, 58 }, // 01011011x
      { 6, 192 },  { 6, 192 },  { 6, 192 },  { 6, 192 }, // 010111xxx
      { 6, 192 },  { 6, 192 },  { 6, 192 },  { 6, 192 },  { 6, 1664 }, { 6, 1664 }, { 6, 1664 }, { 6, 1664 }, // 011000xxx
      { 6, 1664 }, { 6, 1664 }, { 6, 1664 }, { 6, 1664 }, { 8, 448 },  { 8, 448 }, // 01100100x
      { 8, 512 },  { 8, 512 }, // 01100101x
      { 9, 704 }, // 011001100
      { 9, 768 }, // 011001101
      { 8, 640 },  { 8, 640 }, // 01100111x
      { 8, 576 },  { 8, 576 }, // 01101000x
      { 9, 832 }, // 011010010
      { 9, 896 }, // 011010011
      { 9, 960 }, // 011010100
      { 9, 1024 }, // 011010101
      { 9, 1088 }, // 011010110
      { 9, 1152 }, // 011010111
      { 9, 1216 }, // 011011000
      { 9, 1280 }, // 011011001
      { 9, 1344 }, // 011011010
      { 9, 1408 }, // 011011011
      { 7, 256 },  { 7, 256 },  { 7, 256 },  { 7, 256 }, // 0110111xx
      { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 }, // 0111xxxxx
      { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },   { 4, 2 },   { 4, 2 },   { 4, 2 },   { 4, 2 },   { 4, 2 },   { 4, 2 },   { 4, 2 },
      { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },    { 4, 2 },   { 4, 2 },   { 4, 2 },   { 4, 2 },   { 4, 3 },   { 4, 3 },   { 4, 3 },   { 4, 3 }, // 1000xxxxx
      { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },   { 4, 3 },   { 4, 3 },   { 4, 3 },   { 4, 3 },   { 4, 3 },   { 4, 3 },   { 4, 3 },
      { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },    { 4, 3 },   { 4, 3 },   { 4, 3 },   { 4, 3 },   { 5, 128 }, { 5, 128 }, { 5, 128 }, { 5, 128 }, // 10010xxxx
      { 5, 128 },  { 5, 128 },  { 5, 128 },  { 5, 128 },  { 5, 128 },  { 5, 128 },  { 5, 128 },  { 5, 128 },  { 5, 128 }, { 5, 128 }, { 5, 128 }, { 5, 128 }, { 5, 8 },   { 5, 8 },   { 5, 8 },   { 5, 8 }, // 10011xxxx
      { 5, 8 },    { 5, 8 },    { 5, 8 },    { 5, 8 },    { 5, 8 },    { 5, 8 },    { 5, 8 },    { 5, 8 },    { 5, 8 },   { 5, 8 },   { 5, 8 },   { 5, 8 },   { 5, 9 },   { 5, 9 },   { 5, 9 },   { 5, 9 }, // 10100xxxx
      { 5, 9 },    { 5, 9 },    { 5, 9 },    { 5, 9 },    { 5, 9 },    { 5, 9 },    { 5, 9 },    { 5, 9 },    { 5, 9 },   { 5, 9 },   { 5, 9 },   { 5, 9 },   { 6, 16 },  { 6, 16 },  { 6, 16 },  { 6, 16 }, // 101010xxx
      { 6, 16 },   { 6, 16 },   { 6, 16 },   { 6, 16 },   { 6, 17 },   { 6, 17 },   { 6, 17 },   { 6, 17 }, // 101011xxx
      { 6, 17 },   { 6, 17 },   { 6, 17 },   { 6, 17 },   { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 }, // 1011xxxxx
      { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },   { 4, 4 },   { 4, 4 },   { 4, 4 },   { 4, 4 },   { 4, 4 },   { 4, 4 },   { 4, 4 },
      { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },    { 4, 4 },   { 4, 4 },   { 4, 4 },   { 4, 4 },   { 4, 5 },   { 4, 5 },   { 4, 5 },   { 4, 5 }, // 1100xxxxx
      { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },   { 4, 5 },   { 4, 5 },   { 4, 5 },   { 4, 5 },   { 4, 5 },   { 4, 5 },   { 4, 5 },
      { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },    { 4, 5 },   { 4, 5 },   { 4, 5 },   { 4, 5 },   { 6, 14 },  { 6, 14 },  { 6, 14 },  { 6, 14 }, // 110100xxx
      { 6, 14 },   { 6, 14 },   { 6, 14 },   { 6, 14 },   { 6, 15 },   { 6, 15 },   { 6, 15 },   { 6, 15 }, // 110101xxx
      { 6, 15 },   { 6, 15 },   { 6, 15 },   { 6, 15 },   { 5, 64 },   { 5, 64 },   { 5, 64 },   { 5, 64 }, // 11011xxxx
      { 5, 64 },   { 5, 64 },   { 5, 64 },   { 5, 64 },   { 5, 64 },   { 5, 64 },   { 5, 64 },   { 5, 64 },   { 5, 64 },  { 5, 64 },  { 5, 64 },  { 5, 64 },  { 4, 6 },   { 4, 6 },   { 4, 6 },   { 4, 6 }, // 1110xxxxx
      { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },   { 4, 6 },   { 4, 6 },   { 4, 6 },   { 4, 6 },   { 4, 6 },   { 4, 6 },   { 4, 6 },
      { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },    { 4, 6 },   { 4, 6 },   { 4, 6 },   { 4, 6 },   { 4, 7 },   { 4, 7 },   { 4, 7 },   { 4, 7 }, // 1111xxxxx
      { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },   { 4, 7 },   { 4, 7 },   { 4, 7 },   { 4, 7 },   { 4, 7 },   { 4, 7 },   { 4, 7 },
      { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },    { 4, 7 },   { 4, 7 },   { 4, 7 },   { 4, 7 }
  };

  static const CCITTCode kBlackTab1[128] = { { -1, -1 },       { -1, -1 }, // 000000000000x
                                          { 12, kCcittEol }, { 12, kCcittEol }, // 000000000001x
                                          { -1, -1 },       { -1, -1 },       { -1, -1 },   { -1, -1 }, // 00000000001xx
                                          { -1, -1 },       { -1, -1 },       { -1, -1 },   { -1, -1 }, // 00000000010xx
                                          { -1, -1 },       { -1, -1 },       { -1, -1 },   { -1, -1 }, // 00000000011xx
                                          { -1, -1 },       { -1, -1 },       { -1, -1 },   { -1, -1 }, // 00000000100xx
                                          { -1, -1 },       { -1, -1 },       { -1, -1 },   { -1, -1 }, // 00000000101xx
                                          { -1, -1 },       { -1, -1 },       { -1, -1 },   { -1, -1 }, // 00000000110xx
                                          { -1, -1 },       { -1, -1 },       { -1, -1 },   { -1, -1 }, // 00000000111xx
                                          { 11, 1792 },     { 11, 1792 },     { 11, 1792 }, { 11, 1792 }, // 00000001000xx
                                          { 12, 1984 },     { 12, 1984 }, // 000000010010x
                                          { 12, 2048 },     { 12, 2048 }, // 000000010011x
                                          { 12, 2112 },     { 12, 2112 }, // 000000010100x
                                          { 12, 2176 },     { 12, 2176 }, // 000000010101x
                                          { 12, 2240 },     { 12, 2240 }, // 000000010110x
                                          { 12, 2304 },     { 12, 2304 }, // 000000010111x
                                          { 11, 1856 },     { 11, 1856 },     { 11, 1856 }, { 11, 1856 }, // 00000001100xx
                                          { 11, 1920 },     { 11, 1920 },     { 11, 1920 }, { 11, 1920 }, // 00000001101xx
                                          { 12, 2368 },     { 12, 2368 }, // 000000011100x
                                          { 12, 2432 },     { 12, 2432 }, // 000000011101x
                                          { 12, 2496 },     { 12, 2496 }, // 000000011110x
                                          { 12, 2560 },     { 12, 2560 }, // 000000011111x
                                          { 10, 18 },       { 10, 18 },       { 10, 18 },   { 10, 18 }, // 0000001000xxx
                                          { 10, 18 },       { 10, 18 },       { 10, 18 },   { 10, 18 },   { 12, 52 }, { 12, 52 }, // 000000100100x
                                          { 13, 640 }, // 0000001001010
                                          { 13, 704 }, // 0000001001011
                                          { 13, 768 }, // 0000001001100
                                          { 13, 832 }, // 0000001001101
                                          { 12, 55 },       { 12, 55 }, // 000000100111x
                                          { 12, 56 },       { 12, 56 }, // 000000101000x
                                          { 13, 1280 }, // 0000001010010
                                          { 13, 1344 }, // 0000001010011
                                          { 13, 1408 }, // 0000001010100
                                          { 13, 1472 }, // 0000001010101
                                          { 12, 59 },       { 12, 59 }, // 000000101011x
                                          { 12, 60 },       { 12, 60 }, // 000000101100x
                                          { 13, 1536 }, // 0000001011010
                                          { 13, 1600 }, // 0000001011011
                                          { 11, 24 },       { 11, 24 },       { 11, 24 },   { 11, 24 }, // 00000010111xx
                                          { 11, 25 },       { 11, 25 },       { 11, 25 },   { 11, 25 }, // 00000011000xx
                                          { 13, 1664 }, // 0000001100100
                                          { 13, 1728 }, // 0000001100101
                                          { 12, 320 },      { 12, 320 }, // 000000110011x
                                          { 12, 384 },      { 12, 384 }, // 000000110100x
                                          { 12, 448 },      { 12, 448 }, // 000000110101x
                                          { 13, 512 }, // 0000001101100
                                          { 13, 576 }, // 0000001101101
                                          { 12, 53 },       { 12, 53 }, // 000000110111x
                                          { 12, 54 },       { 12, 54 }, // 000000111000x
                                          { 13, 896 }, // 0000001110010
                                          { 13, 960 }, // 0000001110011
                                          { 13, 1024 }, // 0000001110100
                                          { 13, 1088 }, // 0000001110101
                                          { 13, 1152 }, // 0000001110110
                                          { 13, 1216 }, // 0000001110111
                                          { 10, 64 },       { 10, 64 },       { 10, 64 },   { 10, 64 }, // 0000001111xxx
                                          { 10, 64 },       { 10, 64 },       { 10, 64 },   { 10, 64 } };

  static const CCITTCode kBlackTab2[192] = {
    { 8, 13 },   { 8, 13 },  { 8, 13 },  { 8, 13 }, // 00000100xxxx
    { 8, 13 },   { 8, 13 },  { 8, 13 },  { 8, 13 },  { 8, 13 },   { 8, 13 }, { 8, 13 }, { 8, 13 }, { 8, 13 }, { 8, 13 }, { 8, 13 }, { 8, 13 }, { 11, 23 }, { 11, 23 }, // 00000101000x
    { 12, 50 }, // 000001010010
    { 12, 51 }, // 000001010011
    { 12, 44 }, // 000001010100
    { 12, 45 }, // 000001010101
    { 12, 46 }, // 000001010110
    { 12, 47 }, // 000001010111
    { 12, 57 }, // 000001011000
    { 12, 58 }, // 000001011001
    { 12, 61 }, // 000001011010
    { 12, 256 }, // 000001011011
    { 10, 16 },  { 10, 16 }, { 10, 16 }, { 10, 16 }, // 0000010111xx
    { 10, 17 },  { 10, 17 }, { 10, 17 }, { 10, 17 }, // 0000011000xx
    { 12, 48 }, // 000001100100
    { 12, 49 }, // 000001100101
    { 12, 62 }, // 000001100110
    { 12, 63 }, // 000001100111
    { 12, 30 }, // 000001101000
    { 12, 31 }, // 000001101001
    { 12, 32 }, // 000001101010
    { 12, 33 }, // 000001101011
    { 12, 40 }, // 000001101100
    { 12, 41 }, // 000001101101
    { 11, 22 },  { 11, 22 }, // 00000110111x
    { 8, 14 },   { 8, 14 },  { 8, 14 },  { 8, 14 }, // 00000111xxxx
    { 8, 14 },   { 8, 14 },  { 8, 14 },  { 8, 14 },  { 8, 14 },   { 8, 14 }, { 8, 14 }, { 8, 14 }, { 8, 14 }, { 8, 14 }, { 8, 14 }, { 8, 14 }, { 7, 10 },  { 7, 10 },  { 7, 10 }, { 7, 10 }, // 0000100xxxxx
    { 7, 10 },   { 7, 10 },  { 7, 10 },  { 7, 10 },  { 7, 10 },   { 7, 10 }, { 7, 10 }, { 7, 10 }, { 7, 10 }, { 7, 10 }, { 7, 10 }, { 7, 10 }, { 7, 10 },  { 7, 10 },  { 7, 10 }, { 7, 10 },
    { 7, 10 },   { 7, 10 },  { 7, 10 },  { 7, 10 },  { 7, 10 },   { 7, 10 }, { 7, 10 }, { 7, 10 }, { 7, 10 }, { 7, 10 }, { 7, 10 }, { 7, 10 }, { 7, 11 },  { 7, 11 },  { 7, 11 }, { 7, 11 }, // 0000101xxxxx
    { 7, 11 },   { 7, 11 },  { 7, 11 },  { 7, 11 },  { 7, 11 },   { 7, 11 }, { 7, 11 }, { 7, 11 }, { 7, 11 }, { 7, 11 }, { 7, 11 }, { 7, 11 }, { 7, 11 },  { 7, 11 },  { 7, 11 }, { 7, 11 },
    { 7, 11 },   { 7, 11 },  { 7, 11 },  { 7, 11 },  { 7, 11 },   { 7, 11 }, { 7, 11 }, { 7, 11 }, { 7, 11 }, { 7, 11 }, { 7, 11 }, { 7, 11 }, { 9, 15 },  { 9, 15 },  { 9, 15 }, { 9, 15 }, // 000011000xxx
    { 9, 15 },   { 9, 15 },  { 9, 15 },  { 9, 15 },  { 12, 128 }, // 000011001000
    { 12, 192 }, // 000011001001
    { 12, 26 }, // 000011001010
    { 12, 27 }, // 000011001011
    { 12, 28 }, // 000011001100
    { 12, 29 }, // 000011001101
    { 11, 19 },  { 11, 19 }, // 00001100111x
    { 11, 20 },  { 11, 20 }, // 00001101000x
    { 12, 34 }, // 000011010010
    { 12, 35 }, // 000011010011
    { 12, 36 }, // 000011010100
    { 12, 37 }, // 000011010101
    { 12, 38 }, // 000011010110
    { 12, 39 }, // 000011010111
    { 11, 21 },  { 11, 21 }, // 00001101100x
    { 12, 42 }, // 000011011010
    { 12, 43 }, // 000011011011
    { 10, 0 },   { 10, 0 },  { 10, 0 },  { 10, 0 }, // 0000110111xx
    { 7, 12 },   { 7, 12 },  { 7, 12 },  { 7, 12 }, // 0000111xxxxx
    { 7, 12 },   { 7, 12 },  { 7, 12 },  { 7, 12 },  { 7, 12 },   { 7, 12 }, { 7, 12 }, { 7, 12 }, { 7, 12 }, { 7, 12 }, { 7, 12 }, { 7, 12 }, { 7, 12 },  { 7, 12 },  { 7, 12 }, { 7, 12 },
    { 7, 12 },   { 7, 12 },  { 7, 12 },  { 7, 12 },  { 7, 12 },   { 7, 12 }, { 7, 12 }, { 7, 12 }, { 7, 12 }, { 7, 12 }, { 7, 12 }, { 7, 12 }
  };

  static const CCITTCode kBlackTab3[64] = { { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, // 0000xx
                                         { 6, 9 }, // 000100
                                         { 6, 8 }, // 000101
                                         { 5, 7 },   { 5, 7 }, // 00011x
                                         { 4, 6 },   { 4, 6 },   { 4, 6 },   { 4, 6 }, // 0010xx
                                         { 4, 5 },   { 4, 5 },   { 4, 5 },   { 4, 5 }, // 0011xx
                                         { 3, 1 },   { 3, 1 },   { 3, 1 },   { 3, 1 }, // 010xxx
                                         { 3, 1 },   { 3, 1 },   { 3, 1 },   { 3, 1 },   { 3, 4 }, { 3, 4 }, { 3, 4 }, { 3, 4 }, // 011xxx
                                         { 3, 4 },   { 3, 4 },   { 3, 4 },   { 3, 4 },   { 2, 3 }, { 2, 3 }, { 2, 3 }, { 2, 3 }, // 10xxxx
                                         { 2, 3 },   { 2, 3 },   { 2, 3 },   { 2, 3 },   { 2, 3 }, { 2, 3 }, { 2, 3 }, { 2, 3 }, { 2, 3 }, { 2, 3 }, { 2, 3 }, { 2, 3 }, { 2, 2 }, { 2, 2 }, { 2, 2 }, { 2, 2 }, // 11xxxx
                                         { 2, 2 },   { 2, 2 },   { 2, 2 },   { 2, 2 },   { 2, 2 }, { 2, 2 }, { 2, 2 }, { 2, 2 }, { 2, 2 }, { 2, 2 }, { 2, 2 }, { 2, 2 } };

  int decode_row_num = 0;

  auto get_white_code = [&](BitReader& reader) -> int {
    int code = 0;
    if (params.end_of_block) {
      code = reader.look_bits(12);
      if (code < 0) {
        return 1;
      }
      const CCITTCode* p = nullptr;
      if ((code >> 5) == 0) {
        p = &kWhiteTab1[code];
      } else {
        p = &kWhiteTab2[code >> 3];
      }
      if (p->bits > 0) {
        reader.eat_bits(p->bits);
        return p->run;
      }
    } else {
      for (int n = 1; n <= 9; ++n) {
        code = reader.look_bits(n);
        if (code < 0) {
          return 1;
        }
        if (n < 9) {
          code <<= (9 - n);
        }
        const CCITTCode& p = kWhiteTab2[code];
        if (p.bits == n) {
          reader.eat_bits(n);
          return p.run;
        }
      }
      for (int n = 11; n <= 12; ++n) {
        code = reader.look_bits(n);
        if (code < 0) {
          return 1;
        }
        if (n < 12) {
          code <<= (12 - n);
        }
        const CCITTCode& p = kWhiteTab1[code];
        if (p.bits == n) {
          reader.eat_bits(n);
          return p.run;
        }
      }
    }

    NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Bad white code (0x%x)", code);
    reader.eat_bits(1);
    return 1;
  };

  auto get_black_code = [&](BitReader& reader) -> int {
    int code = 0;
    if (params.end_of_block) {
      code = reader.look_bits(13);
      if (code < 0) {
        return 1;
      }
      const CCITTCode* p = nullptr;
      if ((code >> 7) == 0) {
        p = &kBlackTab1[code];
      } else if ((code >> 9) == 0 && (code >> 7) != 0) {
        p = &kBlackTab2[(code >> 1) - 64];
      } else {
        p = &kBlackTab3[code >> 7];
      }
      if (p->bits > 0) {
        reader.eat_bits(p->bits);
        return p->run;
      }
    } else {
      for (int n = 2; n <= 6; ++n) {
        code = reader.look_bits(n);
        if (code < 0) {
          return 1;
        }
        if (n < 6) {
          code <<= (6 - n);
        }
        const CCITTCode& p = kBlackTab3[code];
        if (p.bits == n) {
          reader.eat_bits(n);
          return p.run;
        }
      }
      for (int n = 7; n <= 12; ++n) {
        code = reader.look_bits(n);
        if (code < 0) {
          return 1;
        }
        if (n < 12) {
          code <<= (12 - n);
        }
        if (code >= 64) {
          const CCITTCode& p = kBlackTab2[code - 64];
          if (p.bits == n) {
            reader.eat_bits(n);
            return p.run;
          }
        }
      }
      for (int n = 10; n <= 13; ++n) {
        code = reader.look_bits(n);
        if (code < 0) {
          return 1;
        }
        if (n < 13) {
          code <<= (13 - n);
        }
        const CCITTCode& p = kBlackTab1[code];
        if (p.bits == n) {
          reader.eat_bits(n);
          return p.run;
        }
      }
    }

    NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Bad black code (0x%x)", code);
    reader.eat_bits(1);
    return 1;
  };

  auto decode_run = [&](BitReader& reader, bool white, int* run_length,
                        std::string* err) -> bool {
    int total = 0;
    int code = 0;
    do {
      code = white ? get_white_code(reader) : get_black_code(reader);
      total += code;
    } while (code >= 64);

    *run_length = total;
    (void)err;
    return true;
  };

  auto decode_row_1d = [&](BitReader& reader, std::vector<bool>& line,
                           bool allow_indicator_skip, int width,
                           std::string* err) -> bool {
    std::fill(line.begin(), line.end(), false);
    int pos = 0;
    bool is_white = true;
    bool skipped_indicator = false;

    while (pos < width) {
      BitReader::State state_before = reader.save();
      int run = 0;
      if (!decode_run(reader, is_white, &run, err)) {
        if (allow_indicator_skip && !skipped_indicator && pos == 0) {
          reader.restore(state_before);
          int indicator = reader.get_bit();
          if (indicator == 0 || indicator == 1) {
            skipped_indicator = true;
            continue;
          }
          reader.restore(state_before);
        }
        // Check if it's an EOF error - if so, fill rest of row with current color and succeed
        if (err && err->find("EOF") != std::string::npos) {
          NANOPDF_LOG_DEBUG("CCITTFaxDecode", "EOF in 1D decode at pos=%d, filling rest of row", pos);
          // Rest of row remains as initialized (white=false, black=true as needed)
          break;  // Exit loop, row is complete
        }
        return false;
      }

      // Poppler-style bounds clamping: clamp invalid positions to valid range
      if (run < 0) {
        NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Negative run length %d at pos=%d, clamping to 0", run, pos);
        run = 0;
      }
      if (pos + run > width) {
        NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Row overrun: pos=%d + run=%d > width=%d, clamping",
                          pos, run, width);
        run = width - pos;
      }
      if (pos < 0) {
        NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Negative position %d, clamping to 0", pos);
        pos = 0;
      }
      if (pos >= width) {
        NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Position %d >= width %d, breaking", pos, width);
        break;
      }
      if (!is_white) {
        for (int i = 0; i < run; ++i) {
          line[pos + i] = true;
        }
      }
      pos += run;
      is_white = !is_white;
    }
    return true;
  };


  struct TwoDimCodeEntry {
    int8_t bits;
    int8_t code;
  };

  static constexpr int kTwoDimPass = 0;
  static constexpr int kTwoDimHoriz = 1;
  static constexpr int kTwoDimVert0 = 2;
  static constexpr int kTwoDimVertR1 = 3;
  static constexpr int kTwoDimVertL1 = 4;
  static constexpr int kTwoDimVertR2 = 5;
  static constexpr int kTwoDimVertL2 = 6;
  static constexpr int kTwoDimVertR3 = 7;
  static constexpr int kTwoDimVertL3 = 8;

  static const TwoDimCodeEntry kTwoDimTab[128] = {
      { -1, -1 },          { -1, -1 }, // 000000x
      { 7, kTwoDimVertL3 }, { 7, kTwoDimVertR3 }, // 000001x
      { 6, kTwoDimVertL2 }, { 6, kTwoDimVertL2 }, // 000010x
      { 6, kTwoDimVertR2 }, { 6, kTwoDimVertR2 }, // 000011x
      { 4, kTwoDimPass },   { 4, kTwoDimPass }, // 0001xxx
      { 4, kTwoDimPass },   { 4, kTwoDimPass },   { 4, kTwoDimPass },   { 4, kTwoDimPass },   { 4, kTwoDimPass },   { 4, kTwoDimPass },   { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz }, // 001xxxx
      { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },
      { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },  { 3, kTwoDimHoriz },  { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, // 010xxxx
      { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 },
      { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, { 3, kTwoDimVertL1 }, { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, // 011xxxx
      { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 },
      { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, { 3, kTwoDimVertR1 }, { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 }, // 1xxxxxx
      { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },
      { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },
      { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },
      { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },
      { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },
      { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },
      { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },  { 1, kTwoDimVert0 },
  };

  // Return codes: 0=vertical, 1=horizontal, 2=pass, -1=error
  auto read_2d_code = [&](BitReader& reader, int* delta) -> int {
    int code = -1;
    if (params.end_of_block) {
      int bits = reader.look_bits(7);
      if (bits < 0) {
        return -1;
      }
      const TwoDimCodeEntry& entry = kTwoDimTab[bits];
      if (entry.bits > 0) {
        reader.eat_bits(entry.bits);
        code = entry.code;
      }
    } else {
      for (int n = 1; n <= 7; ++n) {
        int bits = reader.look_bits(n);
        if (bits < 0) {
          return -1;
        }
        if (n < 7) {
          bits <<= (7 - n);
        }
        const TwoDimCodeEntry& entry = kTwoDimTab[bits & 0x7f];
        if (entry.bits == n) {
          reader.eat_bits(n);
          code = entry.code;
          break;
        }
      }
    }

    if (code < 0) {
      return -1;
    }

    switch (code) {
      case 0:  // pass
        return 2;
      case 1:  // horizontal
        return 1;
      case 2:  // vert0
        if (delta) *delta = 0;
        return 0;
      case 3:  // vertR1
        if (delta) *delta = 1;
        return 0;
      case 4:  // vertL1
        if (delta) *delta = -1;
        return 0;
      case 5:  // vertR2
        if (delta) *delta = 2;
        return 0;
      case 6:  // vertL2
        if (delta) *delta = -2;
        return 0;
      case 7:  // vertR3
        if (delta) *delta = 3;
        return 0;
      case 8:  // vertL3
        if (delta) *delta = -3;
        return 0;
      default:
        return -1;
    }
  };

  bool hit_eofb = false;  // Flag to signal end of facsimile block

  // Poppler-style helper functions for array-based CCITT decoding.
  auto addPixels = [](int a1, int blackPixels, int* codingLine, int& a0i,
                      int columns) -> void {
    if (a1 > codingLine[a0i]) {
      if (a1 > columns) {
        NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Row overrun: a1=%d > columns=%d, clamping", a1, columns);
        a1 = columns;
      }
      // CRITICAL: Conditional increment based on color parity
      // Even a0i = white position, Odd a0i = black position
      // If color matches parity, overwrite; otherwise advance to next slot
      bool will_increment = ((a0i & 1) ^ blackPixels);
      if (will_increment) {
        ++a0i;
      }
      codingLine[a0i] = a1;
    }
  };

  auto addPixelsNeg = [](int a1, int blackPixels, int* codingLine, int& a0i,
                        int columns) -> void {
    if (a1 > codingLine[a0i]) {
      // Forward movement - same as addPixels
      if (a1 > columns) {
        NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Invalid a1=%d > columns=%d", a1, columns);
        a1 = columns;
      }
      if ((a0i & 1) ^ blackPixels) {
        ++a0i;
      }
      codingLine[a0i] = a1;
    } else if (a1 < codingLine[a0i]) {
      // Backward movement (negative vertical modes)
      if (a1 < 0) {
        NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Invalid negative a1=%d, setting to columns=%d", a1, columns);
        a1 = columns;  // CRITICAL: Poppler sets to columns, not 0!
      }
      // Scan backwards to find correct insertion point (match poppler)
      while (a0i > 0 && a1 <= codingLine[a0i - 1]) {
        --a0i;
      }
      codingLine[a0i] = a1;
    }
    // else: a1 == codingLine[a0i], no change
  };

  std::vector<int> prev_coding_line;
  bool prev_coding_line_valid = false;
  std::vector<int> coding_line_cache;
  bool coding_line_cache_valid = false;

  auto build_coding_line_from_bits = [](const std::vector<bool>& bits, int width,
                                        std::vector<int>& out) {
    out.resize(width + 2);
    int idx = 0;
    if (width > 0 && bits[0]) {
      out[idx++] = 0;
    }
    for (int i = 1; i < width; ++i) {
      if (bits[i] != bits[i - 1]) {
        out[idx++] = i;
      }
    }
    for (int i = idx; i < width + 2; ++i) {
      out[i] = width;
    }
  };

  auto decode_row_2d = [&](BitReader& reader, const std::vector<bool>& reference,
                           std::vector<bool>& line, int width,
                           std::string* err) -> bool {
    // Array-based CCITT decoding (poppler style) for 100% accuracy
    // Use fixed arrays for typical widths, heap for large images
    static constexpr int MAX_STATIC_WIDTH = 2402;
    int codingLine_static[MAX_STATIC_WIDTH];
    int refLine_static[MAX_STATIC_WIDTH];
    std::vector<int> codingLine_dynamic, refLine_dynamic;

    int* codingLine;
    int* refLine;

    if (width + 2 <= MAX_STATIC_WIDTH) {
      codingLine = codingLine_static;
      refLine = refLine_static;
    } else {
      codingLine_dynamic.resize(width + 2);
      refLine_dynamic.resize(width + 2);
      codingLine = codingLine_dynamic.data();
      refLine = refLine_dynamic.data();
    }

    // Convert reference line to transition array (match poppler codingLine)
    if (prev_coding_line_valid && static_cast<int>(prev_coding_line.size()) >= width + 2) {
      for (int i = 0; i < width + 2; ++i) {
        refLine[i] = prev_coding_line[i];
      }
    } else {
      int refIdx = 0;
      if (!reference.empty() && reference[0]) {
        refLine[refIdx++] = 0;
      }
      for (int i = 1; i < width; ++i) {
        if (reference[i] != reference[i - 1]) {
          refLine[refIdx++] = i;
        }
      }
      // Poppler fills ALL remaining positions with columns (width)
      for (int i = refIdx; i < width + 2; ++i) {
        refLine[i] = width;
      }
    }

    // Initialize state
    codingLine[0] = 0;
    int a0i = 0;
    int b1i = 0;
    int blackPixels = 0;

    // Main decode loop using array-based approach

    while (codingLine[a0i] < width) {
      BitReader::State mode_state = reader.save();
      int delta = 0;
      int mode = read_2d_code(reader, &delta);
      bool row_done = false;

      NANOPDF_LOG_TRACE("CCITTFaxDecode", "2D mode=%d delta=%d at byte=%zu bit=%d a0i=%d pos=%d",
                        mode, delta, mode_state.byte_pos, mode_state.bit_pos, a0i, codingLine[a0i]);
      // Error handling
      if (mode == -1) {
        if (reader.byte_pos >= reader.size) {
          NANOPDF_LOG_DEBUG("CCITTFaxDecode", "2D mode: out of data at pos=%d", codingLine[a0i]);
        } else {
          NANOPDF_LOG_DEBUG("CCITTFaxDecode", "2D mode: invalid code at pos=%d", codingLine[a0i]);
        }
        if (err) *err = "Invalid 2D code";
        addPixels(width, 0, codingLine, a0i, width);
        break;
      }

      if (mode == -2) {
        NANOPDF_LOG_TRACE("CCITTFaxDecode", "Skipping reserved extension at pos=%d", codingLine[a0i]);
        continue;
      }

      if (mode == 5) {
        NANOPDF_LOG_TRACE("CCITTFaxDecode", "Mode 5 (deprecated resync) at pos=%d", codingLine[a0i]);
        continue;
      }

      if (mode == 6) {
        NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Fill rest of row from pos=%d to width=%d", codingLine[a0i], width);
        codingLine[a0i] = width;
        break;
      }

      if (mode == 3) {
        // EOFB
        hit_eofb = true;
        break;
      }

      // Pass mode (mode == 2)
      if (mode == 2) {
        // Pass mode - match poppler exactly
        // refLine is filled up to width+2, so we only need to check against that
        if (b1i + 1 < width + 2) {
          addPixels(refLine[b1i + 1], blackPixels, codingLine, a0i, width);
          if (refLine[b1i + 1] < width) {
            b1i += 2;
          }
        } else {
          NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Pass mode: b1i+1=%d out of bounds (width+2=%d)", b1i+1, width+2);
          break;
        }
      }
      // Horizontal mode (mode == 1)
      // Horizontal mode (mode == 1) - CRITICAL for accuracy
      else if (mode == 1) {
        // Decode two runs: first in current color, then opposite
        // CRITICAL: Poppler checks codingLine[a0i] < columns before second run!
        for (int pass = 0; pass < 2; ++pass) {
          bool decoding_white = (blackPixels == 0);
          if (pass == 1) decoding_white = !decoding_white;
          int color = (pass == 0) ? blackPixels : (blackPixels ^ 1);

          int run = 0;
          if (!decode_run(reader, decoding_white, &run, err)) {
            addPixels(width, color, codingLine, a0i, width);
            row_done = true;
            break;
          }
          if (run < 0) run = 0;

          bool do_add = !(pass == 1 && codingLine[a0i] >= width);
          if (do_add) {
            // CRITICAL: addPixels uses current codingLine[a0i] which may be updated by first call
            int target = codingLine[a0i] + run;
            if (target > width) target = width;

            addPixels(target, color, codingLine, a0i, width);
          }
        }
        if (row_done) {
          break;
        }

        // Update b1i to skip past coded region (match poppler exactly)
        while (refLine[b1i] <= codingLine[a0i] && refLine[b1i] < width) {
          b1i += 2;
          if (b1i > width + 1) {
            NANOPDF_LOG_DEBUG("CCITTFaxDecode", "H-mode: b1i overflow");
            break;
          }
        }
        // NOTE: Do NOT toggle blackPixels here! H-mode decodes TWO runs (both colors),
        // so we end up at the same color state we started with.
      }
      // Uncompressed mode (mode == 4) - simplified for now
      else if (mode == 4) {
        NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Uncompressed mode at pos=%d (simplified handling)", codingLine[a0i]);

        int zero_count = 0;
        while (codingLine[a0i] < width) {
          int bit = reader.get_bit();
          if (bit < 0) {
            // EOF during uncompressed mode - just exit this mode, don't set hit_eofb
            NANOPDF_LOG_DEBUG("CCITTFaxDecode", "EOF in uncompressed mode at pos=%d, exiting mode", codingLine[a0i]);
            break;
          }

          if (bit == 0) {
            zero_count++;
            if (zero_count == 8) {
              // Could be exit sequence 00000001T - check next bit
              int exit_bit = reader.get_bit();
              if (exit_bit < 0) {
                // EOF while checking exit sequence - just exit this mode
                NANOPDF_LOG_DEBUG("CCITTFaxDecode", "EOF checking exit sequence at pos=%d, exiting mode", codingLine[a0i]);
                break;
              }
              if (exit_bit == 1) {
                // Exit sequence found - next bit is tag for following codeword
                // The tag bit determines if we return to 2D mode (0) or need special handling
                int tag = reader.get_bit();
                NANOPDF_LOG_TRACE("CCITTFaxDecode", "Exiting uncompressed mode at pos=%d, tag=%d", codingLine[a0i], tag);
                // Continue with normal 2D decoding
                // Note: the tag bit is already consumed, affecting next codeword interpretation
                break;
              } else {
                // Not exit sequence - continue processing
                zero_count = 0;
              }
            }
          } else {
            // Non-zero bit - reset counter
            zero_count = 0;
          }
        }

        // Uncompressed mode processing complete
        // For simplicity, fill rest of row if we didn't find exit
        if (codingLine[a0i] < width) {
          NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Uncompressed mode: filling rest of row from pos=%d", codingLine[a0i]);
          addPixels(width, blackPixels, codingLine, a0i, width);
        }
      }
      // Vertical mode (mode == 0) - match poppler exactly
      else if (mode == 0) {
        // Bounds check b1i before accessing refLine (filled up to width+2)
        if (b1i >= width + 2) {
          NANOPDF_LOG_DEBUG("CCITTFaxDecode", "V-mode: b1i=%d out of bounds (width+2=%d)", b1i, width+2);
          break;
        }

        // Poppler maintains invariant: refLine[b1i-1] <= codingLine[a0i] < refLine[b1i]
        // (left-edge exception: b1i can be 0), so use refLine[b1i] directly.
        int target = refLine[b1i] + delta;

        // Use addPixels for positive/zero delta, addPixelsNeg for negative delta
        if (delta < 0) {
          addPixelsNeg(target, blackPixels, codingLine, a0i, width);
        } else {
          addPixels(target, blackPixels, codingLine, a0i, width);
        }
        blackPixels ^= 1;

        // Update b1i to maintain invariant (different logic for negative deltas)
        if (codingLine[a0i] < width) {
          if (delta < 0) {
            // Negative vertical modes: special b1i update
            if (b1i > 0) {
              --b1i;
            } else {
              ++b1i;
            }
          } else {
            // Positive/zero vertical modes: simple increment
            ++b1i;
          }
          // Skip to next valid b1i (match poppler exactly)
          while (refLine[b1i] <= codingLine[a0i] && refLine[b1i] < width) {
            b1i += 2;
            if (b1i > width + 1) {
              NANOPDF_LOG_DEBUG("CCITTFaxDecode", "V-mode: b1i overflow");
              break;
            }
          }
        }
      }
    }

    // Convert codingLine array back to output line vector
    // Cache codingLine transitions for reuse as next refLine
    coding_line_cache.resize(width + 2);
    int cache_limit = std::min(a0i + 1, width + 2);
    for (int i = 0; i < cache_limit; ++i) {
      coding_line_cache[i] = codingLine[i];
    }
    for (int i = cache_limit; i < width + 2; ++i) {
      coding_line_cache[i] = width;
    }
    coding_line_cache_valid = true;

    std::fill(line.begin(), line.end(), false);

    // CRITICAL: Determine starting color (match poppler's logic at lines 2327-2330)
    // If codingLine[0] > 0, row starts with white pixels (a0i=0, even)
    // If codingLine[0] == 0, row starts with black pixels (a0i=1, odd)
    int start_idx = 0;
    bool isBlack = false;
    int prevPos = 0;

    if (codingLine[0] == 0 && a0i > 0) {
      // First transition is at position 0, meaning we start with black immediately
      start_idx = 1;
      isBlack = true;  // codingLine[0]=0 is a black-to-white transition
      prevPos = 0;
    } else if (codingLine[0] > 0) {
      // First transition at position > 0, start with white pixels
      start_idx = 0;
      isBlack = false;
      prevPos = 0;
    }

    for (int i = start_idx; i <= a0i && codingLine[i] <= width; ++i) {
      int pos = codingLine[i];
      if (pos > width) pos = width;

      if (isBlack) {
        for (int j = prevPos; j < pos; ++j) {
          line[j] = true;
        }
      }
      prevPos = pos;
      isBlack = !isBlack;
    }

    decode_row_num++;
    return true;
  };

  int width = params.columns > 0 ? params.columns : 1728;
  int bytes_per_row = (width + 7) / 8;
  int max_rows = params.rows;
  BitReader reader(data, size);

  std::vector<uint8_t> output;
  output.reserve(static_cast<size_t>(bytes_per_row) * (max_rows > 0 ? max_rows : 1));

  std::vector<bool> prev_line(width, false);
  std::vector<bool> line(width, false);
  std::vector<uint8_t> row_bytes(bytes_per_row, 0);

  int decoded_rows = 0;
  int damaged_rows = 0;

  // Loop until we've decoded the expected rows or hit EOFB
  // If max_rows is specified, we must decode that many rows even if we run out of data
  while ((max_rows > 0 && decoded_rows < max_rows) || (max_rows <= 0 && reader.byte_pos < size)) {
    if (params.end_of_line) {
      if (!reader.skip_eol()) {
        result.error = "CCITTFaxDecode: Missing EOL";
        return result;
      }
    } else if (params.encoded_byte_align) {
      reader.align_to_byte();
    }

    bool allow_indicator_skip = (params.k == 0);
    bool row_is_1d = false;
    if (params.k == 0) {
      row_is_1d = true;
    } else if (params.k > 0) {
      int mode = reader.get_bit();
      if (mode < 0) {
        result.error = "CCITTFaxDecode: Unexpected EOF while reading row mode";
        return result;
      }
      row_is_1d = (mode != 0);
    } else {
      row_is_1d = false;
    }

    std::string row_error;
    bool ok = false;
    NANOPDF_LOG_TRACE("CCITTFaxDecode", "Starting row %d at byte=%zu bit=%d",
                      decoded_rows, reader.byte_pos, reader.bit_pos);

    // If we're out of data but need more rows, repeat previous line
    if (reader.byte_pos >= size) {
      NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Out of data at row %d, repeating previous line", decoded_rows);
      line = prev_line;
      ok = true;
    } else {
      if (row_is_1d) {
        ok = decode_row_1d(reader, line, allow_indicator_skip, width, &row_error);
      } else {
        ok = decode_row_2d(reader, prev_line, line, width, &row_error);
      }
    }

    // Check if we hit EOFB during 2D decode
    if (hit_eofb) {
      NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Hit EOFB at row %d, stopping decode", decoded_rows);
      break;
    }

    if (!ok) {
      NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Row %d failed: %s (row_is_1d=%d)",
                        decoded_rows, row_error.c_str(), row_is_1d);

      // Poppler-style EOL recovery: if we have EOL markers, scan for next one
      if (params.end_of_line) {
        NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Attempting EOL resync at byte=%zu bit=%d",
                          reader.byte_pos, reader.bit_pos);
        bool found_eol = false;
        size_t scan_start = reader.byte_pos;
        // Scan bit-by-bit for next EOL marker (11 zeros followed by 1)
        int zero_count = 0;
        while (reader.byte_pos < size && reader.byte_pos < scan_start + 100) {
          int bit = reader.get_bit();
          if (bit < 0) break;
          if (bit == 0) {
            zero_count++;
          } else {
            if (zero_count >= 11) {
              found_eol = true;
              NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Found EOL after %zu bytes, resuming",
                                reader.byte_pos - scan_start);
              break;
            }
            zero_count = 0;
          }
        }
        if (found_eol) {
          // Successfully resynced - use previous line and continue
          ++damaged_rows;
          line = prev_line;
          // Continue to next iteration
        } else {
          // EOL scan failed - treat as damaged row or fail
          if (params.damaged_rows_before_error > 0 &&
              damaged_rows < params.damaged_rows_before_error) {
            ++damaged_rows;
            line = prev_line;
          } else {
            result.error = row_error.empty() ? "CCITTFaxDecode: Failed to decode row" : row_error;
            NANOPDF_LOG_ERROR("CCITTFaxDecode", "Decode failed at row %d: %s",
                              decoded_rows, result.error.c_str());
            return result;
          }
        }
      } else {
        // No EOL markers - use "just plow on" approach
        // For EOF errors during row decode, we should have already handled it gracefully
        // by filling the rest of the row. Don't treat it as a failure.
        if (row_error.find("EOF") != std::string::npos) {
          // EOF was encountered but row was completed, just log and continue
          NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Row %d completed with EOF handling", decoded_rows);
          damaged_rows = 0;  // Reset since we recovered
          // Don't break, continue to next row
        } else if (params.damaged_rows_before_error > 0 &&
                   damaged_rows < params.damaged_rows_before_error) {
          ++damaged_rows;
          line = prev_line;
        } else {
          result.error = row_error.empty() ? "CCITTFaxDecode: Failed to decode row" : row_error;
          NANOPDF_LOG_ERROR("CCITTFaxDecode", "Decode failed at row %d: %s",
                            decoded_rows, result.error.c_str());
          return result;
        }
      }
    } else {
      damaged_rows = 0;
    }

    if (ok) {
      if (!row_is_1d && coding_line_cache_valid &&
          static_cast<int>(coding_line_cache.size()) >= width + 2) {
        prev_coding_line = coding_line_cache;
      } else {
        build_coding_line_from_bits(line, width, prev_coding_line);
      }
      prev_coding_line_valid = true;
    }

    // Poppler-style: skip post-row zero padding when no EOL and no byte alignment.
    if (!params.end_of_line && !params.encoded_byte_align) {
      int code = reader.look_bits(12);
      while (code == 0) {
        if (reader.get_bit() < 0) {
          break;
        }
        code = reader.look_bits(12);
        if (code < 0) {
          break;
        }
      }
    }

    std::fill(row_bytes.begin(), row_bytes.end(), 0);
    for (int i = 0; i < width; ++i) {
      bool black = line[i];
      bool bit = params.black_is_1 ? black : !black;
      if (bit) {
        row_bytes[i / 8] |= static_cast<uint8_t>(1 << (7 - (i % 8)));
      }
    }

    output.insert(output.end(), row_bytes.begin(), row_bytes.end());
    prev_line = line;
    ++decoded_rows;
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}

}  // namespace filters

// Helper to apply a single filter
static DecodedStream apply_single_filter(const std::string &filter_name,
                                         const uint8_t *data, size_t size,
                                         const filters::DecodeParams &params) {
  DecodedStream result;

  if (filter_name == "FlateDecode" || filter_name == "Fl") {
    return filters::decode_flate(data, size, params);
  } else if (filter_name == "ASCII85Decode" || filter_name == "A85") {
    return filters::decode_ascii85(data, size, params);
  } else if (filter_name == "ASCIIHexDecode" || filter_name == "AHx") {
    return filters::decode_asciihex(data, size, params);
  } else if (filter_name == "LZWDecode" || filter_name == "LZW") {
    return filters::decode_lzw(data, size, params);
  } else if (filter_name == "JBIG2Decode") {
    return filters::decode_jbig2(data, size, params);
  } else if (filter_name == "RunLengthDecode" || filter_name == "RL") {
    return filters::decode_runlength(data, size, params);
  } else if (filter_name == "DCTDecode" || filter_name == "DCT") {
    return filters::decode_dct(data, size, params);
  } else if (filter_name == "CCITTFaxDecode" || filter_name == "CCF") {
    return filters::decode_ccittfax(data, size, params);
  } else if (filter_name == "JPXDecode") {
    return filters::decode_jpx(data, size, params);
  } else if (filter_name == "Crypt") {
    // Crypt filter is handled at a higher level; pass through
    result.data.assign(data, data + size);
    result.success = true;
    return result;
  }

  result.error = "Unsupported filter type: " + filter_name;
  return result;
}

DecodedStream decode_stream(const Pdf &pdf, const Value &stream_obj,
                           uint32_t obj_num, uint16_t gen_num) {
  DecodedStream result;

  if (stream_obj.type != Value::STREAM) {
    result.error = "decode_stream: not a stream object";
    return result;
  }

  // Start with raw stream data
  std::vector<uint8_t> current_data = stream_obj.stream.data;

  // Decrypt stream data if PDF is encrypted
  // Note: Decryption must happen BEFORE decompression
  if (pdf.security.authenticated && !pdf.security.encryption_key.empty()) {
    // Check if this is a Crypt filter that should not be decrypted
    // or if this is an XRef stream (XRef streams are not encrypted)
    auto type_it = stream_obj.stream.dict.find("Type");
    bool is_xref = (type_it != stream_obj.stream.dict.end() &&
                    type_it->second.type == Value::NAME &&
                    type_it->second.name == "XRef");

    if (!is_xref) {
      NANOPDF_LOG_TRACE("decode_stream", "Decrypting obj %u %u, before: %zu bytes",
                        obj_num, gen_num, current_data.size());
      if (current_data.size() >= 8) {
        NANOPDF_LOG_TRACE("decode_stream", "Pre-decrypt: %02x %02x %02x %02x %02x %02x %02x %02x",
                          current_data[0], current_data[1], current_data[2], current_data[3],
                          current_data[4], current_data[5], current_data[6], current_data[7]);
      }
      current_data = pdf.security.decrypt_stream(current_data, obj_num, gen_num);
      NANOPDF_LOG_TRACE("decode_stream", "After decrypt: %zu bytes", current_data.size());
      if (current_data.size() >= 8) {
        NANOPDF_LOG_TRACE("decode_stream", "Post-decrypt: %02x %02x %02x %02x %02x %02x %02x %02x",
                          current_data[0], current_data[1], current_data[2], current_data[3],
                          current_data[4], current_data[5], current_data[6], current_data[7]);
      }
    }
  }

  // Get filter type from stream dictionary
  auto filter_it = stream_obj.stream.dict.find("Filter");
  if (filter_it == stream_obj.stream.dict.end()) {
    // No filter, return (possibly decrypted) data
    result.data = std::move(current_data);
    result.success = true;
    return result;
  }

  // Build list of filters to apply (PDF applies filters in order)
  std::vector<std::string> filter_names;
  if (filter_it->second.type == Value::NAME) {
    filter_names.push_back(filter_it->second.name);
  } else if (filter_it->second.type == Value::ARRAY) {
    for (const auto &item : filter_it->second.array) {
      if (item.type == Value::NAME) {
        filter_names.push_back(item.name);
      } else {
        result.error = "decode_stream: Filter array contains non-name element";
        return result;
      }
    }
  } else {
    result.error = "decode_stream: Filter must be a name or array, got type " +
                   std::to_string(static_cast<int>(filter_it->second.type));
    return result;
  }

  // Build list of decode parameters (one per filter, or empty)
  std::vector<filters::DecodeParams> params_list;
  auto params_it = stream_obj.stream.dict.find("DecodeParms");
  if (params_it != stream_obj.stream.dict.end()) {
    if (params_it->second.type == Value::DICTIONARY) {
      // Single params dict applies to single filter
      params_list.push_back(filters::parse_decode_params(params_it->second.dict));
    } else if (params_it->second.type == Value::ARRAY) {
      // Array of params, one per filter
      for (const auto &item : params_it->second.array) {
        if (item.type == Value::DICTIONARY) {
          params_list.push_back(filters::parse_decode_params(item.dict));
        } else if (item.type == Value::NULL_OBJ) {
          // null means default params for this filter
          params_list.push_back(filters::DecodeParams{});
        } else {
          result.error = "decode_stream: DecodeParms array contains invalid element";
          return result;
        }
      }
    } else if (params_it->second.type != Value::NULL_OBJ) {
      result.error = "decode_stream: DecodeParms must be dictionary, array, or null";
      return result;
    }
  }

  // Pad params_list to match filter count (use defaults for missing)
  while (params_list.size() < filter_names.size()) {
    params_list.push_back(filters::DecodeParams{});
  }

  // Apply filters in sequence
  for (size_t i = 0; i < filter_names.size(); ++i) {
    const std::string &filter_name = filter_names[i];
    const filters::DecodeParams &params = params_list[i];

    DecodedStream step_result =
        apply_single_filter(filter_name, current_data.data(),
                            current_data.size(), params);

    if (!step_result.success) {
      if (filter_names.size() > 1) {
        result.error = "Filter chain step " + std::to_string(i + 1) + "/" +
                       std::to_string(filter_names.size()) + " (" +
                       filter_name + "): " + step_result.error;
      } else {
        result.error = step_result.error;
      }
      return result;
    }

    current_data = std::move(step_result.data);
  }

  result.data = std::move(current_data);
  result.success = true;
  return result;
}

// Overload with image dimensions for CCITT decoding
DecodedStream decode_stream(const Pdf &pdf, const Value &stream_obj,
                           uint32_t obj_num, uint16_t gen_num,
                           int image_width, int image_height) {
  DecodedStream result;

  if (stream_obj.type != Value::STREAM) {
    result.error = "decode_stream: not a stream object";
    return result;
  }

  // Start with raw stream data
  std::vector<uint8_t> current_data = stream_obj.stream.data;

  // Decrypt stream data if PDF is encrypted
  // Note: Decryption must happen BEFORE decompression
  if (pdf.security.authenticated && !pdf.security.encryption_key.empty()) {
    // Check if this is a Crypt filter that should not be decrypted
    // or if this is an XRef stream (XRef streams are not encrypted)
    auto type_it = stream_obj.stream.dict.find("Type");
    bool is_xref = (type_it != stream_obj.stream.dict.end() &&
                    type_it->second.type == Value::NAME &&
                    type_it->second.name == "XRef");

    if (!is_xref) {
      NANOPDF_LOG_TRACE("decode_stream", "Decrypting obj %u %u, before: %zu bytes",
                        obj_num, gen_num, current_data.size());
      if (current_data.size() >= 8) {
        NANOPDF_LOG_TRACE("decode_stream", "Pre-decrypt: %02x %02x %02x %02x %02x %02x %02x %02x",
                          current_data[0], current_data[1], current_data[2], current_data[3],
                          current_data[4], current_data[5], current_data[6], current_data[7]);
      }
      current_data = pdf.security.decrypt_stream(current_data, obj_num, gen_num);
      NANOPDF_LOG_TRACE("decode_stream", "After decrypt: %zu bytes", current_data.size());
      if (current_data.size() >= 8) {
        NANOPDF_LOG_TRACE("decode_stream", "Post-decrypt: %02x %02x %02x %02x %02x %02x %02x %02x",
                          current_data[0], current_data[1], current_data[2], current_data[3],
                          current_data[4], current_data[5], current_data[6], current_data[7]);
      }
    }
  }

  // Get filter type from stream dictionary
  auto filter_it = stream_obj.stream.dict.find("Filter");
  if (filter_it == stream_obj.stream.dict.end()) {
    // No filter, return (possibly decrypted) data
    result.data = std::move(current_data);
    result.success = true;
    return result;
  }

  // Build list of filters to apply (PDF applies filters in order)
  std::vector<std::string> filter_names;
  if (filter_it->second.type == Value::NAME) {
    filter_names.push_back(filter_it->second.name);
  } else if (filter_it->second.type == Value::ARRAY) {
    for (const auto &item : filter_it->second.array) {
      if (item.type == Value::NAME) {
        filter_names.push_back(item.name);
      } else {
        result.error = "decode_stream: Filter array contains non-name element";
        return result;
      }
    }
  } else {
    result.error = "decode_stream: Filter must be a name or array, got type " +
                   std::to_string(static_cast<int>(filter_it->second.type));
    return result;
  }

  // Build list of decode parameters (one per filter, or empty)
  std::vector<filters::DecodeParams> params_list;
  auto params_it = stream_obj.stream.dict.find("DecodeParms");
  if (params_it != stream_obj.stream.dict.end()) {
    if (params_it->second.type == Value::DICTIONARY) {
      // Single params dict applies to single filter
      params_list.push_back(filters::parse_decode_params(params_it->second.dict));
    } else if (params_it->second.type == Value::ARRAY) {
      // Array of params, one per filter
      for (const auto &item : params_it->second.array) {
        if (item.type == Value::DICTIONARY) {
          params_list.push_back(filters::parse_decode_params(item.dict));
        } else if (item.type == Value::NULL_OBJ) {
          // null means default params for this filter
          params_list.push_back(filters::DecodeParams{});
        } else {
          result.error = "decode_stream: DecodeParms array contains invalid element";
          return result;
        }
      }
    } else if (params_it->second.type != Value::NULL_OBJ) {
      result.error = "decode_stream: DecodeParms must be dictionary, array, or null";
      return result;
    }
  }

  // Pad params_list to match filter count (use defaults for missing)
  while (params_list.size() < filter_names.size()) {
    params_list.push_back(filters::DecodeParams{});
  }

  // Fix CCITT parameters if image dimensions are provided
  for (size_t i = 0; i < filter_names.size(); ++i) {
    const std::string &filter_name = filter_names[i];
    filters::DecodeParams &params = params_list[i];

    if ((filter_name == "CCITTFaxDecode" || filter_name == "CCF")) {
      // If rows not specified in DecodeParms but we have image height, use it
      if (params.rows == 0 && image_height > 0) {
        params.rows = image_height;
        NANOPDF_LOG_DEBUG("decode_stream",
                         "CCITT: Using image height %d for missing Rows param",
                         image_height);
      }
      // If columns not specified in DecodeParms but we have image width, use it
      if (params.columns == 0 && image_width > 0) {
        params.columns = image_width;
        NANOPDF_LOG_DEBUG("decode_stream",
                         "CCITT: Using image width %d for missing Columns param",
                         image_width);
      }
    }
  }

  // Apply filters in sequence
  for (size_t i = 0; i < filter_names.size(); ++i) {
    const std::string &filter_name = filter_names[i];
    const filters::DecodeParams &params = params_list[i];

    DecodedStream step_result =
        apply_single_filter(filter_name, current_data.data(),
                            current_data.size(), params);

    if (!step_result.success) {
      if (filter_names.size() > 1) {
        result.error = "Filter chain step " + std::to_string(i + 1) + "/" +
                       std::to_string(filter_names.size()) + " (" +
                       filter_name + "): " + step_result.error;
      } else {
        result.error = step_result.error;
      }
      return result;
    }

    current_data = std::move(step_result.data);
  }

  result.data = std::move(current_data);
  result.success = true;
  return result;
}

// Legacy version without object number - cannot decrypt
DecodedStream decode_stream(const Pdf &pdf, const Value &stream_obj) {
  // For backward compatibility, call with obj_num=0, gen_num=0
  // This means streams decoded via this function won't be decrypted
  // if the PDF is encrypted. Callers should use the overload with object numbers.
  return decode_stream(pdf, stream_obj, 0, 0);
}

bool parse_object_stream(StreamReader &sr, Parser &parser,
                         const Value &stream_obj,
                         std::vector<Value> &out_objects) {
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

    obj_offsets.push_back(
        std::make_pair(static_cast<int>(obj_num),
                       static_cast<uint64_t>(offset) + first_offset));
  }

  // Read the objects
  for (const auto &obj_info : obj_offsets) {
    sr.seek_set(obj_info.second);

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

//}  // namespace detail

bool parse_from_memory(const uint8_t *addr, const size_t size) {
  if (size < 8) {
    // too short
    return false;
  }

  const bool swap_endian = !is_system_little_endian();
  nanopdf::StreamReader sr(addr, size, swap_endian);

  int minor_version{0};
  std::string err;
  if (!parse_header(sr, minor_version, err)) {
    std::cerr << err;
    return false;
  }

  // Successfully parsed header
  return true;
}

// Signature field parsing implementation
bool Pdf::parse_signature_fields() {
  catalog.signature_fields.clear();
  
  if (catalog.acro_form.empty()) {
    return true; // No AcroForm, no signature fields
  }
  
  return parse_acro_form_fields(*this, catalog.acro_form, catalog.signature_fields);
}

bool Pdf::parse_acro_form_fields(const Pdf& pdf, const Dictionary& acro_form, 
                                 std::vector<SignatureField>& signature_fields) {
  // Look for Fields array in AcroForm
  auto fields_it = acro_form.find("Fields");
  if (fields_it == acro_form.end() || fields_it->second.type != Value::ARRAY) {
    return true; // No fields array
  }
  
  const auto& fields_array = fields_it->second.array;
  
  for (const auto& field_ref : fields_array) {
    if (field_ref.type != Value::REFERENCE) {
      continue;
    }
    
    // Resolve the field reference
    ResolvedObject resolved = resolve_reference(pdf, field_ref.ref_object_number, 
                                                field_ref.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      continue;
    }
    
    const Dictionary& field_dict = resolved.value.dict;
    
    // Check if this is a signature field
    auto ft_it = field_dict.find("FT");
    if (ft_it != field_dict.end() && ft_it->second.type == Value::NAME && 
        ft_it->second.name == "Sig") {
      
      SignatureField sig_field = parse_signature_field(pdf, field_dict);
      if (!sig_field.name.empty()) {
        signature_fields.push_back(std::move(sig_field));
      }
    }
  }
  
  return true;
}

namespace {

std::vector<uint8_t> compute_signature_digest(const Pdf& pdf,
                                              const std::vector<uint64_t>& byte_range) {
  std::vector<uint8_t> digest;

  if (!pdf.data || pdf.data_size == 0 || byte_range.size() < 2 ||
      (byte_range.size() % 2) != 0) {
    return digest;
  }

  nanopdf::crypto::SHA256 sha256;
  bool has_data = false;

  for (size_t i = 0; i < byte_range.size(); i += 2) {
    uint64_t offset = byte_range[i];
    uint64_t length = byte_range[i + 1];

    if (length == 0) {
      continue;
    }

    if (offset > pdf.data_size || length > pdf.data_size) {
      return {};
    }

    if (offset + length < offset || offset + length > pdf.data_size) {
      return {};
    }

    const uint8_t* segment_begin = pdf.data + static_cast<size_t>(offset);
    size_t segment_length = static_cast<size_t>(length);
    sha256.update(segment_begin, segment_length);
    has_data = true;
  }

  if (!has_data) {
    return digest;
  }

  sha256.finalize();
  digest.resize(nanopdf::crypto::SHA256::DIGEST_SIZE);
  sha256.get_digest(digest.data());
  return digest;
}

}  // namespace

SignatureField Pdf::parse_signature_field(const Pdf& pdf, const Dictionary& field_dict) {
  SignatureField sig_field;
  
  // Parse field name (T)
  auto t_it = field_dict.find("T");
  if (t_it != field_dict.end() && t_it->second.type == Value::STRING) {
    sig_field.name = t_it->second.str;
  }
  
  // Parse field rectangle (Rect)
  auto rect_it = field_dict.find("Rect");
  if (rect_it != field_dict.end() && rect_it->second.type == Value::ARRAY) {
    const auto& rect_array = rect_it->second.array;
    if (rect_array.size() == 4) {
      sig_field.rect.reserve(4);
      for (const auto& coord : rect_array) {
        if (coord.type == Value::NUMBER) {
          sig_field.rect.push_back(coord.number);
        }
      }
    }
  }
  
  // Parse page reference (P)
  auto p_it = field_dict.find("P");
  if (p_it != field_dict.end() && p_it->second.type == Value::REFERENCE) {
    sig_field.page_ref = p_it->second.ref_object_number;
  }
  
  // Parse field flags (F)
  auto f_it = field_dict.find("F");
  if (f_it != field_dict.end() && f_it->second.type == Value::NUMBER) {
    sig_field.flags = static_cast<uint32_t>(f_it->second.number);
  }
  
  // Parse signature dictionary (V)
  auto v_it = field_dict.find("V");
  if (v_it != field_dict.end()) {
    if (v_it->second.type == Value::DICTIONARY) {
      sig_field.signature_dict = v_it->second.dict;
      sig_field.is_signed = true;
      
      // Extract signature information
      auto reason_it = sig_field.signature_dict.find("Reason");
      if (reason_it != sig_field.signature_dict.end() && 
          reason_it->second.type == Value::STRING) {
        sig_field.signing_reason = reason_it->second.str;
      }
      
      auto location_it = sig_field.signature_dict.find("Location");
      if (location_it != sig_field.signature_dict.end() && 
          location_it->second.type == Value::STRING) {
        sig_field.signing_location = location_it->second.str;
      }
      
      auto contact_it = sig_field.signature_dict.find("ContactInfo");
      if (contact_it != sig_field.signature_dict.end() && 
          contact_it->second.type == Value::STRING) {
        sig_field.signing_contact_info = contact_it->second.str;
      }

      auto contents_it = sig_field.signature_dict.find("Contents");
      if (contents_it != sig_field.signature_dict.end()) {
        if (contents_it->second.type == Value::STRING) {
          sig_field.signature_contents.assign(contents_it->second.str.begin(),
                                              contents_it->second.str.end());
          sig_field.signature_present = true;
        } else if (contents_it->second.type == Value::STREAM) {
          sig_field.signature_contents = contents_it->second.stream.data;
          sig_field.signature_present = true;
        }
      }

      auto byte_range_it = sig_field.signature_dict.find("ByteRange");
      if (byte_range_it != sig_field.signature_dict.end() &&
          byte_range_it->second.type == Value::ARRAY) {
        const auto& arr = byte_range_it->second.array;
        std::vector<uint64_t> ranges;
        ranges.reserve(arr.size());
        for (const Value& v : arr) {
          if (v.type == Value::NUMBER) {
            ranges.push_back(static_cast<uint64_t>(v.number));
          }
        }
        if (!ranges.empty() && ranges.size() % 2 == 0) {
          sig_field.byte_range = std::move(ranges);
          sig_field.byte_range_valid = true;
          if (sig_field.signature_present) {
            sig_field.signed_data_digest = compute_signature_digest(pdf, sig_field.byte_range);
            sig_field.digest_algorithm = "SHA256";
          }
        }
      }
      
      auto date_it = sig_field.signature_dict.find("M");
      if (date_it != sig_field.signature_dict.end() && 
          date_it->second.type == Value::STRING) {
        sig_field.signing_date = date_it->second.str;
      }
    } else if (v_it->second.type == Value::REFERENCE) {
      // Resolve signature dictionary reference
      ResolvedObject resolved = resolve_reference(pdf, v_it->second.ref_object_number, 
                                                  v_it->second.ref_generation_number);
      if (resolved.success && resolved.value.type == Value::DICTIONARY) {
        sig_field.signature_dict = resolved.value.dict;
        sig_field.is_signed = true;

        auto contents_ref_it = sig_field.signature_dict.find("Contents");
        if (contents_ref_it != sig_field.signature_dict.end()) {
          if (contents_ref_it->second.type == Value::STRING) {
            sig_field.signature_contents.assign(contents_ref_it->second.str.begin(),
                                                contents_ref_it->second.str.end());
            sig_field.signature_present = true;
          } else if (contents_ref_it->second.type == Value::STREAM) {
            sig_field.signature_contents = contents_ref_it->second.stream.data;
            sig_field.signature_present = true;
          }
        }

        auto byte_range_ref_it = sig_field.signature_dict.find("ByteRange");
        if (byte_range_ref_it != sig_field.signature_dict.end() &&
            byte_range_ref_it->second.type == Value::ARRAY) {
          const auto& arr = byte_range_ref_it->second.array;
          std::vector<uint64_t> ranges;
          ranges.reserve(arr.size());
          for (const Value& v : arr) {
            if (v.type == Value::NUMBER) {
              ranges.push_back(static_cast<uint64_t>(v.number));
            }
          }
          if (!ranges.empty() && ranges.size() % 2 == 0) {
            sig_field.byte_range = std::move(ranges);
            sig_field.byte_range_valid = true;
            if (sig_field.signature_present) {
              sig_field.signed_data_digest = compute_signature_digest(pdf, sig_field.byte_range);
              sig_field.digest_algorithm = "SHA256";
            }
          }
        }
      }
    }
  }
  
  // Parse signature lock dictionary (Lock)
  auto lock_it = field_dict.find("Lock");
  if (lock_it != field_dict.end()) {
    if (lock_it->second.type == Value::DICTIONARY) {
      sig_field.lock_dict = lock_it->second.dict;
    } else if (lock_it->second.type == Value::REFERENCE) {
      ResolvedObject resolved = resolve_reference(pdf, lock_it->second.ref_object_number,
                                                  lock_it->second.ref_generation_number);
      if (resolved.success && resolved.value.type == Value::DICTIONARY) {
        sig_field.lock_dict = resolved.value.dict;
      }
    }
  }

  // Parse MDP (Modification Detection and Prevention) signature information
  if (sig_field.is_signed && !sig_field.signature_dict.empty()) {
    // Extract Filter (e.g., Adobe.PPKLite)
    auto filter_it = sig_field.signature_dict.find("Filter");
    if (filter_it != sig_field.signature_dict.end() && filter_it->second.type == Value::NAME) {
      sig_field.filter = filter_it->second.name;
    }

    // Extract SubFilter (e.g., adbe.pkcs7.detached, ETSI.CAdES.detached)
    auto subfilter_it = sig_field.signature_dict.find("SubFilter");
    if (subfilter_it != sig_field.signature_dict.end() && subfilter_it->second.type == Value::NAME) {
      sig_field.subfilter = subfilter_it->second.name;
    }

    // Parse Reference array for TransformMethod and TransformParams
    auto ref_it = sig_field.signature_dict.find("Reference");
    if (ref_it != sig_field.signature_dict.end() && ref_it->second.type == Value::ARRAY) {
      for (const auto& ref_entry : ref_it->second.array) {
        Dictionary ref_dict;
        if (ref_entry.type == Value::DICTIONARY) {
          ref_dict = ref_entry.dict;
        } else if (ref_entry.type == Value::REFERENCE) {
          ResolvedObject resolved = resolve_reference(pdf, ref_entry.ref_object_number,
                                                      ref_entry.ref_generation_number);
          if (resolved.success && resolved.value.type == Value::DICTIONARY) {
            ref_dict = resolved.value.dict;
          }
        }

        if (!ref_dict.empty()) {
          // Extract TransformMethod
          auto tm_it = ref_dict.find("TransformMethod");
          if (tm_it != ref_dict.end() && tm_it->second.type == Value::NAME) {
            sig_field.transform_method = tm_it->second.name;

            // Check if this is a DocMDP (certification) signature
            if (sig_field.transform_method == "DocMDP") {
              sig_field.is_certification_signature = true;
            }
          }

          // Extract TransformParams
          auto tp_it = ref_dict.find("TransformParams");
          if (tp_it != ref_dict.end()) {
            Dictionary tp_dict;
            if (tp_it->second.type == Value::DICTIONARY) {
              tp_dict = tp_it->second.dict;
            } else if (tp_it->second.type == Value::REFERENCE) {
              ResolvedObject resolved = resolve_reference(pdf, tp_it->second.ref_object_number,
                                                          tp_it->second.ref_generation_number);
              if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                tp_dict = resolved.value.dict;
              }
            }

            if (!tp_dict.empty()) {
              // Extract P (permissions) for DocMDP
              auto p_it = tp_dict.find("P");
              if (p_it != tp_dict.end() && p_it->second.type == Value::NUMBER) {
                sig_field.mdp_permissions = static_cast<int>(p_it->second.number);
              }

              // Extract locked fields for FieldMDP
              auto fields_it = tp_dict.find("Fields");
              if (fields_it != tp_dict.end() && fields_it->second.type == Value::ARRAY) {
                for (const auto& field_name : fields_it->second.array) {
                  if (field_name.type == Value::STRING) {
                    sig_field.locked_fields.push_back(field_name.str);
                  }
                }
              }
            }
          }
        }
      }
    }

    // Detect document timestamp signature (ETSI.RFC3161)
    if (sig_field.subfilter == "ETSI.RFC3161") {
      sig_field.is_document_timestamp = true;
      sig_field.has_timestamp = true;
    }

    // Check for embedded timestamp in signature contents
    // Timestamps are typically found as unsigned attributes in CMS/PKCS#7
    // The timestamp token contains OID 1.2.840.113549.1.9.16.2.14 (id-aa-timeStampToken)
    if (!sig_field.signature_contents.empty() && sig_field.signature_contents.size() > 100) {
      // Look for timestamp OID: 1.2.840.113549.1.9.16.2.14
      // DER encoded: 06 0B 2A 86 48 86 F7 0D 01 09 10 02 0E
      const uint8_t timestamp_oid[] = {0x06, 0x0B, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x10, 0x02, 0x0E};
      const size_t oid_len = sizeof(timestamp_oid);

      for (size_t i = 0; i + oid_len < sig_field.signature_contents.size(); ++i) {
        if (std::memcmp(sig_field.signature_contents.data() + i, timestamp_oid, oid_len) == 0) {
          sig_field.has_timestamp = true;
          break;
        }
      }

      // Also check for RFC 3161 content type OID: 1.2.840.113549.1.9.16.1.4
      // DER encoded: 06 0B 2A 86 48 86 F7 0D 01 09 10 01 04
      if (!sig_field.has_timestamp) {
        const uint8_t tst_content_oid[] = {0x06, 0x0B, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x10, 0x01, 0x04};
        for (size_t i = 0; i + sizeof(tst_content_oid) < sig_field.signature_contents.size(); ++i) {
          if (std::memcmp(sig_field.signature_contents.data() + i, tst_content_oid, sizeof(tst_content_oid)) == 0) {
            sig_field.has_timestamp = true;
            break;
          }
        }
      }
    }

    // Try to extract timestamp hash algorithm from signature contents
    // SHA-256 OID: 2.16.840.1.101.3.4.2.1 -> 06 09 60 86 48 01 65 03 04 02 01
    // SHA-384 OID: 2.16.840.1.101.3.4.2.2 -> 06 09 60 86 48 01 65 03 04 02 02
    // SHA-512 OID: 2.16.840.1.101.3.4.2.3 -> 06 09 60 86 48 01 65 03 04 02 03
    if (sig_field.has_timestamp && !sig_field.signature_contents.empty()) {
      const uint8_t sha256_oid[] = {0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01};
      const uint8_t sha384_oid[] = {0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02};
      const uint8_t sha512_oid[] = {0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03};
      const uint8_t sha1_oid[] = {0x06, 0x05, 0x2B, 0x0E, 0x03, 0x02, 0x1A};

      for (size_t i = 0; i + 11 < sig_field.signature_contents.size(); ++i) {
        if (std::memcmp(sig_field.signature_contents.data() + i, sha256_oid, sizeof(sha256_oid)) == 0) {
          sig_field.timestamp_hash_algorithm = "SHA-256";
          break;
        } else if (std::memcmp(sig_field.signature_contents.data() + i, sha384_oid, sizeof(sha384_oid)) == 0) {
          sig_field.timestamp_hash_algorithm = "SHA-384";
          break;
        } else if (std::memcmp(sig_field.signature_contents.data() + i, sha512_oid, sizeof(sha512_oid)) == 0) {
          sig_field.timestamp_hash_algorithm = "SHA-512";
          break;
        } else if (i + 7 < sig_field.signature_contents.size() &&
                   std::memcmp(sig_field.signature_contents.data() + i, sha1_oid, sizeof(sha1_oid)) == 0) {
          sig_field.timestamp_hash_algorithm = "SHA-1";
          break;
        }
      }
    }
  }

  return sig_field;
}

// Stub implementations for missing functions
bool parse_string(StreamReader& sr, Parser& parser, std::string* out_str) {
  (void)parser;
  if (!out_str) return false;

  out_str->clear();

  uint8_t first_ch;
  if (!sr.read1(&first_ch)) return false;
  char first = static_cast<char>(first_ch);

  if (first == '(') {
    std::string result;
    int depth = 1;

    while (true) {
      uint8_t raw;
      if (!sr.read1(&raw)) {
        return false;
      }
      char c = static_cast<char>(raw);

      if (c == '\\') {
        uint8_t escape_raw;
        if (!sr.read1(&escape_raw)) {
          return false;
        }
        char esc = static_cast<char>(escape_raw);

        if (esc == '\r' || esc == '\n') {
          // Line continuation. If CR followed by LF consume both.
          if (esc == '\r') {
            uint8_t next_raw;
            if (sr.read1(&next_raw)) {
              if (static_cast<char>(next_raw) != '\n') {
                sr.seek_from_current(-1);
              }
            }
          }
          continue;
        }

        switch (esc) {
          case 'n':
            result.push_back('\n');
            break;
          case 'r':
            result.push_back('\r');
            break;
          case 't':
            result.push_back('\t');
            break;
          case 'b':
            result.push_back('\b');
            break;
          case 'f':
            result.push_back('\f');
            break;
          case '(':
          case ')':
          case '\\':
            result.push_back(esc);
            break;
          default: {
            if (esc >= '0' && esc <= '7') {
              int octal = esc - '0';
              for (int i = 0; i < 2; ++i) {
                uint8_t next_raw;
                if (!sr.read1(&next_raw)) {
                  break;
                }
                char next = static_cast<char>(next_raw);
                if (next >= '0' && next <= '7') {
                  octal = (octal << 3) + (next - '0');
                } else {
                  sr.seek_from_current(-1);
                  break;
                }
              }
              result.push_back(static_cast<char>(octal & 0xFF));
            } else {
              // Unknown escape sequence; keep literal character.
              result.push_back(esc);
            }
            break;
          }
        }
        continue;
      }

      if (c == '(') {
        depth++;
        result.push_back(c);
      } else if (c == ')') {
        depth--;
        if (depth == 0) {
          break;
        }
        result.push_back(c);
      } else {
        result.push_back(c);
      }
    }

    *out_str = std::move(result);
    return true;
  }

  if (first == '<') {
    std::string bytes;
    std::string hex_buffer;

    while (true) {
      uint8_t raw;
      if (!sr.read1(&raw)) {
        return false;
      }
      char c = static_cast<char>(raw);

      if (c == '>') {
        break;
      }

      if (is_whitespace_char(c)) {
        continue;
      }

      int hv = hex_value(c);
      if (hv < 0) {
        return false;
      }
      hex_buffer.push_back(static_cast<char>(hv));
    }

    if (hex_buffer.size() % 2 == 1) {
      hex_buffer.push_back(0);  // Pad with zero nibble as per PDF spec
    }

    bytes.reserve(hex_buffer.size() / 2);
    for (size_t i = 0; i < hex_buffer.size(); i += 2) {
      int high = static_cast<unsigned char>(hex_buffer[i]);
      int low = static_cast<unsigned char>(hex_buffer[i + 1]);
      bytes.push_back(static_cast<char>((high << 4) | low));
    }

    *out_str = std::move(bytes);
    return true;
  }

  return false;
}

bool parse_name(StreamReader& sr, Parser& parser, std::string* out_str) {
  (void)parser;
  if (!out_str) return false;
  out_str->clear();

  while (true) {
    uint8_t raw;
    if (!sr.read1(&raw)) {
      break;
    }
    char c = static_cast<char>(raw);

    if (is_delimiter_char(c) || is_whitespace_char(c)) {
      sr.seek_from_current(-1);
      break;
    }

    if (c == '#') {
      uint8_t h1_raw, h2_raw;
      if (!sr.read1(&h1_raw) || !sr.read1(&h2_raw)) {
        return false;
      }
      int hi = hex_value(static_cast<char>(h1_raw));
      int lo = hex_value(static_cast<char>(h2_raw));
      if (hi < 0 || lo < 0) {
        return false;
      }
      out_str->push_back(static_cast<char>((hi << 4) | lo));
      continue;
    }

    out_str->push_back(c);
  }

  return true;
}

bool parse_number(StreamReader& sr, Parser& parser, double* out_number) {
  (void)parser;
  if (!out_number) return false;

  std::string token;
  bool has_digit = false;

  while (true) {
    uint8_t raw;
    if (!sr.read1(&raw)) {
      break;
    }
    char c = static_cast<char>(raw);

    if (is_delimiter_char(c)) {
      sr.seek_from_current(-1);
      break;
    }

    if (!is_whitespace_char(c)) {
      token.push_back(c);
      if (std::isdigit(static_cast<unsigned char>(c))) {
        has_digit = true;
      }
    } else {
      break;
    }
  }

  if (token.empty() || !has_digit) {
    return false;
  }

  try {
    *out_number = std::stod(token);
  } catch (const std::exception&) {
    return false;
  }

  return true;
}

namespace {

bool compute_object_body_offset(const Pdf& pdf, uint64_t header_offset,
                                uint32_t obj_num, uint16_t gen_num,
                                uint64_t* body_offset) {
  if (!pdf.data || !body_offset || header_offset >= pdf.data_size) {
    return false;
  }

  StreamReader sr(pdf.data, pdf.data_size, pdf.swap_endian);
  Parser parser(sr);

  if (!sr.seek_set(header_offset)) {
    return false;
  }

  auto read_uint32 = [&](uint32_t* out) -> bool {
    if (!out) return false;
    *out = 0;
    bool has_digit = false;

    while (true) {
      uint8_t raw;
      if (!sr.read1(&raw)) {
        return has_digit;
      }
      char c = static_cast<char>(raw);

      if (is_whitespace_char(c)) {
        if (has_digit) {
          return true;
        }
        continue;
      }

      if (!std::isdigit(static_cast<unsigned char>(c))) {
        if (!has_digit) {
          return false;
        }
        sr.seek_from_current(-1);
        return true;
      }

      has_digit = true;
      *out = (*out * 10) + static_cast<uint32_t>(c - '0');
    }
  };

  uint32_t parsed_obj = 0;
  if (!read_uint32(&parsed_obj) || parsed_obj != obj_num) {
    return false;
  }

  if (!parser.skip_whitespace(sr)) {
    return false;
  }

  uint32_t parsed_gen = 0;
  if (!read_uint32(&parsed_gen) || parsed_gen != gen_num) {
    return false;
  }

  if (!parser.skip_whitespace(sr)) {
    return false;
  }

  if (!parser.consume_keyword(sr, "obj")) {
    return false;
  }

  if (!parser.skip_whitespace(sr)) {
    return false;
  }

  *body_offset = sr.tell();
  return true;
}

bool find_indirect_object_body_offset(const Pdf& pdf, uint32_t obj_num,
                                      uint16_t gen_num, uint64_t* offset) {
  if (!pdf.data || pdf.data_size == 0 || !offset) {
    return false;
  }

  const uint8_t* data = pdf.data;
  const size_t size = pdf.data_size;
  const std::string token = std::to_string(obj_num) + " " +
                            std::to_string(gen_num) + " obj";
  const size_t token_len = token.size();

  if (token_len == 0 || token_len > size) {
    return false;
  }

  for (size_t pos = 0; pos + token_len <= size; ++pos) {
    if (data[pos] != static_cast<uint8_t>(token[0])) {
      continue;
    }

    bool matched = true;
    for (size_t i = 1; i < token_len; ++i) {
      if (data[pos + i] != static_cast<uint8_t>(token[i])) {
        matched = false;
        break;
      }
    }
    if (!matched) {
      continue;
    }

    if (pos > 0) {
      char before = static_cast<char>(data[pos - 1]);
      if (!is_whitespace_char(before) && !is_delimiter_char(before)) {
        continue;
      }
    }

    size_t after = pos + token_len;
    if (after < size) {
      char after_c = static_cast<char>(data[after]);
      if (!is_whitespace_char(after_c) && !is_delimiter_char(after_c)) {
        continue;
      }
    }

    while (after < size && is_whitespace_char(static_cast<char>(data[after]))) {
      after++;
    }

    uint64_t body_offset = 0;
    if (compute_object_body_offset(pdf, static_cast<uint64_t>(pos), obj_num,
                                   gen_num, &body_offset)) {
      *offset = body_offset;
      return true;
    }
  }

  return false;
}

}  // namespace

ResolvedObject resolve_reference(const Pdf& pdf, uint32_t obj_num, uint16_t gen_num) {
  return pdf.load_object(obj_num, gen_num);
}

ResolvedObject Pdf::load_object(uint32_t obj_num, uint16_t gen_num) const {
  ResolvedObject result;

  uint64_t key = (static_cast<uint64_t>(obj_num) << 32) |
                 static_cast<uint64_t>(gen_num);
  auto cached = object_cache.find(key);
  if (cached != object_cache.end()) {
    result.success = true;
    result.value = cached->second;
    return result;
  }

  if (!object_offsets_built && !object_offsets_failed) {
    if (build_object_offset_cache()) {
      object_offsets_built = true;
    } else {
      object_offsets_failed = true;
    }
  }
  uint64_t body_offset = 0;
  bool have_offset = false;

  if (object_offsets_built) {
    auto found = object_offsets.find(key);
    if (found != object_offsets.end()) {
      body_offset = found->second;
      have_offset = true;
    } else if (gen_num != 0) {
      uint64_t zero_key = static_cast<uint64_t>(obj_num) << 32;
      auto zero_found = object_offsets.find(zero_key);
      if (zero_found != object_offsets.end()) {
        body_offset = zero_found->second;
        have_offset = true;
      }
    }
  }

  if (!have_offset) {
    auto stream_it = object_stream_entries.find(key);
    if (stream_it == object_stream_entries.end() && gen_num != 0) {
      uint64_t zero_key = static_cast<uint64_t>(obj_num) << 32;
      stream_it = object_stream_entries.find(zero_key);
    }
    if (stream_it != object_stream_entries.end()) {
      Value from_stream;
      if (load_object_from_stream(obj_num, gen_num, stream_it->second.first,
                                  stream_it->second.second, &from_stream)) {
        object_cache[key] = from_stream;
        result.success = true;
        result.value = std::move(from_stream);
        return result;
      }
      result.success = false;
      result.error = "Failed to load object from object stream";
      return result;
    }

    if (!find_indirect_object_body_offset(*this, obj_num, gen_num, &body_offset)) {
      result.success = false;
      result.error = "Failed to locate object";
      return result;
    }
  }

  StreamReader sr(data, data_size, swap_endian);
  Parser parser(sr);

  if (!sr.seek_set(body_offset)) {
    result.success = false;
    result.error = "Invalid object offset";
    return result;
  }

  if (!parser.skip_whitespace(sr)) {
    result.success = false;
    result.error = "Failed to prepare object stream";
    return result;
  }

  if (!parse_object(sr, parser, &result.value)) {
    result.success = false;
    NANOPDF_LOG_WARN("load_object",
                     "Failed to parse object %u %u at body_offset=%lu have_offset=%d",
                     obj_num, gen_num, (unsigned long)body_offset, have_offset);
    if (body_offset < data_size) {
      NANOPDF_LOG_DEBUG("load_object", "Bytes at offset: %s",
                        log::format_hex_bytes(data + body_offset,
                                              data_size - body_offset, 40).c_str());
    }
    result.error = "Failed to parse object";
    return result;
  }

  if (!parser.skip_whitespace(sr)) {
    result.success = false;
    result.error = "Failed to parse object";
    return result;
  }

  if (!parser.consume_keyword(sr, "endobj")) {
    result.success = false;
    result.error = "Missing endobj for object";
    return result;
  }

  result.success = true;
  object_cache[key] = result.value;
  return result;
}

bool Pdf::build_object_offset_cache() const {
  object_offsets.clear();
  object_stream_entries.clear();
  object_stream_cache.clear();
  object_stream_cache_order.clear();

  if (!data || data_size == 0) {
    return false;
  }

  const char* begin = reinterpret_cast<const char*>(data);
  const char* end = begin + data_size;
  const char keyword[] = "startxref";
  const size_t keyword_len = sizeof(keyword) - 1;

  if (data_size < keyword_len) {
    return false;
  }

  const char* startxref_pos = nullptr;
  for (size_t pos = data_size - keyword_len;; --pos) {
    if (std::memcmp(begin + pos, keyword, keyword_len) == 0) {
      startxref_pos = begin + pos;
      break;
    }
    if (pos == 0) {
      break;
    }
  }

  if (!startxref_pos) {
    return false;
  }

  const char* cursor = startxref_pos + keyword_len;
  while (cursor < end && is_whitespace_char(*cursor)) {
    cursor++;
  }
  if (cursor >= end) {
    return false;
  }

  uint64_t initial_xref = 0;
  bool has_digit = false;
  while (cursor < end && std::isdigit(static_cast<unsigned char>(*cursor))) {
    has_digit = true;
    initial_xref = initial_xref * 10 + static_cast<uint64_t>(*cursor - '0');
    cursor++;
  }
  if (!has_digit || initial_xref >= data_size) {
    return false;
  }

  auto skip_ws_ptr = [&](const char*& p) {
    while (p < end && is_whitespace_char(*p)) {
      p++;
    }
  };

  auto parse_uint_ptr = [&](const char*& p, uint64_t& out) -> bool {
    skip_ws_ptr(p);
    if (p >= end || !std::isdigit(static_cast<unsigned char>(*p))) {
      return false;
    }
    out = 0;
    while (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
      out = out * 10 + static_cast<uint64_t>(*p - '0');
      p++;
    }
    return true;
  };

  auto store_entry = [&](uint32_t object_number, uint16_t generation,
                         uint64_t header_offset) {
    if (header_offset >= data_size) {
      return;
    }
    uint64_t body_offset = 0;
    if (compute_object_body_offset(*this, header_offset, object_number,
                                   generation, &body_offset)) {
      uint64_t key = (static_cast<uint64_t>(object_number) << 32) |
                     static_cast<uint64_t>(generation);
      object_offsets[key] = body_offset;
    }
  };

  auto update_trailer = [&](const Dictionary& dict) {
    Pdf* self = const_cast<Pdf*>(this);
    self->trailer = dict;

    auto size_it_local = dict.find("Size");
    if (size_it_local != dict.end() && size_it_local->second.type == Value::NUMBER) {
      self->size = static_cast<uint32_t>(size_it_local->second.number);
    }

    auto root_it = dict.find("Root");
    if (root_it != dict.end() && root_it->second.type == Value::REFERENCE) {
      self->root = root_it->second.ref_object_number;
    }

    auto info_it = dict.find("Info");
    if (info_it != dict.end() && info_it->second.type == Value::REFERENCE) {
      self->info = info_it->second.ref_object_number;
    }

    auto encrypt_it = dict.find("Encrypt");
    if (encrypt_it != dict.end() && encrypt_it->second.type == Value::REFERENCE) {
      self->encrypt = encrypt_it->second.ref_object_number;
    }

    auto prev_it_root = dict.find("Prev");
    if (prev_it_root != dict.end() && prev_it_root->second.type == Value::NUMBER) {
      self->prev = static_cast<uint32_t>(prev_it_root->second.number);
    }

    auto id_it = dict.find("ID");
    if (id_it != dict.end() && id_it->second.type == Value::ARRAY &&
        !id_it->second.array.empty() &&
        id_it->second.array[0].type == Value::STRING) {
      self->id = id_it->second.array[0].str;
    }
  };

  auto handle_trailer_offsets = [&](const Dictionary& dict,
                                   std::deque<uint64_t>& worklist) {
    auto push_offset = [&](const Value& v) {
      if (v.type == Value::NUMBER) {
        uint64_t off = static_cast<uint64_t>(v.number);
        if (off < data_size) {
          worklist.push_back(off);
        }
      }
    };

    update_trailer(dict);

    auto prev_it = dict.find("Prev");
    if (prev_it != dict.end()) {
      push_offset(prev_it->second);
    }

    auto xrefstm_it = dict.find("XRefStm");
    if (xrefstm_it != dict.end()) {
      push_offset(xrefstm_it->second);
    }
  };

  std::deque<uint64_t> worklist;
  std::unordered_set<uint64_t> visited_offsets;
  worklist.push_back(initial_xref);

  bool any_entries = false;

  while (!worklist.empty()) {
    uint64_t current_offset = worklist.front();
    worklist.pop_front();

    if (current_offset >= data_size) {
      continue;
    }
    if (!visited_offsets.insert(current_offset).second) {
      continue;
    }

    const char* ptr = begin + current_offset;

    auto parse_traditional_xref = [&](uint64_t offset) -> bool {
      const char* p = begin + offset;
      if (p + 4 > end || std::memcmp(p, "xref", 4) != 0) {
        return false;
      }
      p += 4;
      skip_ws_ptr(p);

      while (p < end) {
        if (p[0] == '%') {
          while (p < end && *p != '\n' && *p != '\r') {
            p++;
          }
          skip_ws_ptr(p);
          continue;
        }

        if (p + 7 <= end && std::memcmp(p, "trailer", 7) == 0) {
          p += 7;
          skip_ws_ptr(p);

        uint64_t dict_offset = static_cast<uint64_t>(p - begin);
        StreamReader sr(data, data_size, swap_endian);
        Parser parser(sr);
        if (!sr.seek_set(dict_offset)) {
          return false;
        }

        Value trailer_val;
        trailer_val.SetType(Value::DICTIONARY);
        uint8_t ch1 = 0, ch2 = 0;
        if (!sr.read1(&ch1) || !sr.read1(&ch2) || ch1 != '<' || ch2 != '<') {
          return false;
        }
        if (!parse_dictionary(sr, parser, &trailer_val.dict)) {
          return false;
        }

        handle_trailer_offsets(trailer_val.dict, worklist);
        any_entries = any_entries || !object_offsets.empty();
          return true;
        }

        uint64_t first_obj = 0;
        if (!parse_uint_ptr(p, first_obj)) {
          break;
        }

        uint64_t count = 0;
        if (!parse_uint_ptr(p, count) || count > 10000000ULL) {
          return false;
        }

        skip_ws_ptr(p);

        for (uint64_t i = 0; i < count; ++i) {
          if (p >= end) {
            return false;
          }

          char* endptr = nullptr;
          uint64_t entry_offset = std::strtoull(p, &endptr, 10);
          if (endptr == p || entry_offset >= data_size) {
            return false;
          }
          p = endptr;

          if (p >= end || *p != ' ') {
            return false;
          }
          p++;

          unsigned long entry_gen_ul = std::strtoul(p, &endptr, 10);
          if (endptr == p) {
            return false;
          }
          p = endptr;

          if (p >= end || *p != ' ') {
            return false;
          }
          p++;

          if (p >= end) {
            return false;
          }
          char entry_type = *p++;

          while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
          }
          if (p < end && *p == '\r') {
            p++;
          }
          if (p < end && *p == '\n') {
            p++;
          }

          if (entry_type == 'n') {
            store_entry(static_cast<uint32_t>(first_obj + i),
                        static_cast<uint16_t>(entry_gen_ul), entry_offset);
            any_entries = true;
          }
        }

        skip_ws_ptr(p);
      }

      return true;
    };

    auto parse_xref_stream = [&](uint64_t offset) -> bool {
      StreamReader sr(data, data_size, swap_endian);
      Parser parser(sr);
      if (!sr.seek_set(offset)) {
        return false;
      }

      Value xref_obj;
      if (!parse_indirect_object(sr, parser, &xref_obj)) {
        return false;
      }

      if (xref_obj.type != Value::STREAM) {
        return false;
      }

      auto type_it = xref_obj.stream.dict.find("Type");
      if (type_it != xref_obj.stream.dict.end()) {
        if (type_it->second.type != Value::NAME ||
            type_it->second.name != "XRef") {
          return false;
        }
      }

      auto w_it = xref_obj.stream.dict.find("W");
      if (w_it == xref_obj.stream.dict.end() ||
          w_it->second.type != Value::ARRAY ||
          w_it->second.array.size() != 3) {
        return false;
      }

      int w[3] = {0, 0, 0};
      for (size_t i = 0; i < 3; ++i) {
        const Value& item = w_it->second.array[i];
        if (item.type != Value::NUMBER) {
          return false;
        }
        int width = static_cast<int>(item.number);
        if (width < 0 || width > 8) {
          return false;
        }
        w[i] = width;
      }

      auto size_it = xref_obj.stream.dict.find("Size");
      if (size_it == xref_obj.stream.dict.end() ||
          size_it->second.type != Value::NUMBER) {
        return false;
      }
      uint64_t doc_size = static_cast<uint64_t>(size_it->second.number);

      std::vector<uint64_t> index_pairs;
      auto index_it = xref_obj.stream.dict.find("Index");
      if (index_it != xref_obj.stream.dict.end() &&
          index_it->second.type == Value::ARRAY) {
        const auto& arr = index_it->second.array;
        if (arr.size() % 2 != 0) {
          return false;
        }
        index_pairs.reserve(arr.size());
        for (const auto& v : arr) {
          if (v.type != Value::NUMBER) {
            return false;
          }
          index_pairs.push_back(static_cast<uint64_t>(v.number));
        }
      } else {
        index_pairs = {0, doc_size};
      }

      DecodedStream decoded = decode_stream(*this, xref_obj);
      if (!decoded.success) {
        return false;
      }

      size_t pos = 0;
      const auto& bytes = decoded.data;

      auto read_field = [&](int width) -> uint64_t {
        uint64_t value = 0;
        for (int i = 0; i < width; ++i) {
          value = (value << 8) | static_cast<uint64_t>(bytes[pos++]);
        }
        return value;
      };

      for (size_t i = 0; i + 1 < index_pairs.size(); i += 2) {
        uint64_t obj = index_pairs[i];
        uint64_t count = index_pairs[i + 1];

        if (count > doc_size) {
          return false;
        }

        for (uint64_t j = 0; j < count; ++j, ++obj) {
          if (pos > bytes.size()) {
            return false;
          }

          if (w[0] && pos + static_cast<size_t>(w[0]) > bytes.size()) {
            return false;
          }
          uint64_t type_field = w[0] ? read_field(w[0]) : 1;

          if (w[1] && pos + static_cast<size_t>(w[1]) > bytes.size()) {
            return false;
          }
          uint64_t field2 = w[1] ? read_field(w[1]) : 0;

          if (w[2] && pos + static_cast<size_t>(w[2]) > bytes.size()) {
            return false;
          }
          uint64_t field3 = w[2] ? read_field(w[2]) : 0;

          if (type_field == 1) {
            store_entry(static_cast<uint32_t>(obj),
                        static_cast<uint16_t>(field3), field2);
            any_entries = true;
          } else if (type_field == 2) {
            uint32_t stream_object = static_cast<uint32_t>(field2);
            uint32_t index = static_cast<uint32_t>(field3);
            uint64_t key = (static_cast<uint64_t>(obj) << 32);
            object_stream_entries[key] = {stream_object, index};
            any_entries = true;
          }
        }
      }

      handle_trailer_offsets(xref_obj.stream.dict, worklist);
      return true;
    };

    if (current_offset + 4 <= data_size &&
        std::memcmp(ptr, "xref", 4) == 0) {
      parse_traditional_xref(current_offset);
    } else {
      parse_xref_stream(current_offset);
    }
  }

  if (!any_entries && object_offsets.empty() && object_stream_entries.empty()) {
    // Fallback scanning for traditional PDFs without xref tables.
    auto update_trailer_fallback = [&]() {
      const char trailer_kw[] = "trailer";
      const size_t trailer_len = sizeof(trailer_kw) - 1;
      if (data_size < trailer_len) {
        return;
      }

      for (size_t pos = data_size - trailer_len;; --pos) {
        if (std::memcmp(begin + pos, trailer_kw, trailer_len) == 0) {
          uint64_t dict_offset = static_cast<uint64_t>((begin + pos + trailer_len) - begin);
          while (dict_offset < data_size && is_whitespace_char(static_cast<char>(data[dict_offset]))) {
            dict_offset++;
          }
          StreamReader sr(data, data_size, swap_endian);
          Parser parser(sr);
          if (!sr.seek_set(dict_offset)) {
            return;
          }
          uint8_t ch1 = 0, ch2 = 0;
          if (!sr.read1(&ch1) || !sr.read1(&ch2) || ch1 != '<' || ch2 != '<') {
            return;
          }
          Value trailer_val;
          trailer_val.SetType(Value::DICTIONARY);
          if (parse_dictionary(sr, parser, &trailer_val.dict)) {
            update_trailer(trailer_val.dict);
          }
          return;
        }
        if (pos == 0) {
          break;
        }
      }
    };

    update_trailer_fallback();

    const char* ptr = begin;
    while (ptr < end) {
      const char* start = ptr;
      if (!std::isdigit(static_cast<unsigned char>(*ptr))) {
        ++ptr;
        continue;
      }

      uint32_t obj = 0;
      while (ptr < end && std::isdigit(static_cast<unsigned char>(*ptr))) {
        obj = obj * 10 + static_cast<uint32_t>(*ptr - '0');
        ++ptr;
      }
      if (ptr >= end || !is_whitespace_char(*ptr)) {
        ptr = start + 1;
        continue;
      }

      ++ptr;  // skip whitespace before generation
      if (ptr >= end || !std::isdigit(static_cast<unsigned char>(*ptr))) {
        ptr = start + 1;
        continue;
      }

      uint32_t gen = 0;
      while (ptr < end && std::isdigit(static_cast<unsigned char>(*ptr))) {
        gen = gen * 10 + static_cast<uint32_t>(*ptr - '0');
        ++ptr;
      }
      if (ptr >= end || !is_whitespace_char(*ptr)) {
        ptr = start + 1;
        continue;
      }

      const char* keyword_ptr = ptr;
      ++ptr;
      if (keyword_ptr + 3 >= end) {
        ptr = start + 1;
        continue;
      }
      if (std::memcmp(keyword_ptr, " obj", 4) != 0) {
        ptr = start + 1;
        continue;
      }

      const char* body_ptr = keyword_ptr + 4;
      while (body_ptr < end && is_whitespace_char(*body_ptr)) {
        ++body_ptr;
      }
      if (body_ptr >= end) {
        ptr = start + 1;
        continue;
      }

      uint64_t key = (static_cast<uint64_t>(obj) << 32) |
                     static_cast<uint64_t>(gen);
      object_offsets[key] = static_cast<uint64_t>(body_ptr - begin);
      any_entries = true;
      ptr = body_ptr;
    }

    if (root == 0) {
      uint32_t catalog_obj = 0;
      for (const auto& entry : object_offsets) {
        uint32_t obj_num = static_cast<uint32_t>(entry.first >> 32);
        uint64_t body_offset = entry.second;
        if (body_offset >= data_size) {
          continue;
        }
        size_t remaining = static_cast<size_t>(data_size - body_offset);
        size_t sample_len = std::min<size_t>(remaining, 256);
        std::string snippet(reinterpret_cast<const char*>(data + body_offset), sample_len);
        if (snippet.find("/Type /Catalog") != std::string::npos) {
          catalog_obj = obj_num;
          break;
        }
      }
      if (catalog_obj != 0) {
        Pdf* self = const_cast<Pdf*>(this);
        self->root = catalog_obj;
      } else if (!object_offsets.empty()) {
        Pdf* self = const_cast<Pdf*>(this);
        self->root = static_cast<uint32_t>(object_offsets.begin()->first >> 32);
      }
    }

    if (size == 0 && !object_offsets.empty()) {
      uint32_t max_obj = 0;
      for (const auto& entry : object_offsets) {
        uint32_t obj_num = static_cast<uint32_t>(entry.first >> 32);
        if (obj_num > max_obj) {
          max_obj = obj_num;
        }
      }
      Pdf* self = const_cast<Pdf*>(this);
      self->size = max_obj + 1;
    }
  }

  return any_entries && (!object_offsets.empty() || !object_stream_entries.empty());
}

bool Pdf::load_object_from_stream(uint32_t object_number, uint16_t generation,
                                  uint32_t stream_object_number,
                                  uint32_t index, Value* out_value) const {
  if (!out_value) {
    return false;
  }

  // Compressed objects are always generation 0 per PDF spec.
  if (generation != 0) {
    return load_object_from_stream(object_number, 0, stream_object_number, index,
                                   out_value);
  }

  ResolvedObject stream_obj = load_object(stream_object_number, 0);
  if (!stream_obj.success || stream_obj.value.type != Value::STREAM) {
    return false;
  }

  const auto& dict = stream_obj.value.stream.dict;
  auto n_it = dict.find("N");
  auto first_it = dict.find("First");
  if (n_it == dict.end() || first_it == dict.end() ||
      n_it->second.type != Value::NUMBER ||
      first_it->second.type != Value::NUMBER) {
    return false;
  }

  uint32_t object_count = static_cast<uint32_t>(n_it->second.number);
  uint32_t first_offset = static_cast<uint32_t>(first_it->second.number);
  if (object_count == 0 || index >= object_count) {
    return false;
  }

  const std::vector<uint8_t>* data_ptr = nullptr;
  auto touch_cache_entry = [&](uint32_t key, bool inserted) {
    if (!inserted) {
      for (auto it = object_stream_cache_order.begin();
           it != object_stream_cache_order.end(); ++it) {
        if (*it == key) {
          object_stream_cache_order.erase(it);
          break;
        }
      }
    }
    object_stream_cache_order.push_back(key);
    while (object_stream_cache_order.size() > object_stream_cache_capacity) {
      uint32_t evict = object_stream_cache_order.front();
      object_stream_cache_order.pop_front();
      object_stream_cache.erase(evict);
    }
  };

  auto cache_it = object_stream_cache.find(stream_object_number);
  if (cache_it == object_stream_cache.end()) {
    // Use overload with object number for proper decryption
    DecodedStream decoded = decode_stream(*this, stream_obj.value,
                                          stream_object_number, 0);
    if (!decoded.success) {
      return false;
    }
    cache_it = object_stream_cache
                   .emplace(stream_object_number, std::move(decoded.data))
                   .first;
    touch_cache_entry(stream_object_number, true);
  } else {
    touch_cache_entry(stream_object_number, false);
  }
  data_ptr = &cache_it->second;
  const std::vector<uint8_t>& data = *data_ptr;
  if (first_offset > data.size()) {
    return false;
  }

  std::vector<std::pair<uint32_t, uint32_t>> entries;
  entries.reserve(object_count);

  size_t pos = 0;
  auto skip_ws = [&]() {
    while (pos < first_offset) {
      char c = static_cast<char>(data[pos]);
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
        break;
      }
      pos++;
    }
  };

  auto read_uint = [&](uint32_t* out) -> bool {
    skip_ws();
    if (pos >= first_offset) {
      return false;
    }
    uint32_t value = 0;
    bool has_digit = false;
    while (pos < first_offset) {
      char c = static_cast<char>(data[pos]);
      if (c < '0' || c > '9') {
        break;
      }
      has_digit = true;
      value = value * 10 + static_cast<uint32_t>(c - '0');
      pos++;
    }
    if (!has_digit) {
      return false;
    }
    *out = value;
    return true;
  };

  for (uint32_t i = 0; i < object_count; ++i) {
    uint32_t object_id = 0;
    uint32_t offset_val = 0;
    if (!read_uint(&object_id) || !read_uint(&offset_val)) {
      return false;
    }
    entries.emplace_back(object_id, offset_val);
  }

  if (index >= entries.size()) {
    return false;
  }

  uint32_t object_offset = entries[index].second;
  uint32_t object_id_from_stream = entries[index].first;
  if (object_id_from_stream != object_number) {
    // mismatch, treat as failure
    return false;
  }

  size_t content_start = static_cast<size_t>(first_offset) + object_offset;
  if (content_start >= data.size()) {
    return false;
  }

  size_t content_end = data.size();
  if (index + 1 < entries.size()) {
    size_t next_offset = static_cast<size_t>(first_offset) + entries[index + 1].second;
    if (next_offset <= data.size()) {
      content_end = next_offset;
    }
  }

  if (content_end <= content_start) {
    return false;
  }

  StreamReader sr(data.data() + content_start, content_end - content_start,
                  swap_endian);
  Parser parser(sr);

  Value parsed;
  if (!parse_object(sr, parser, &parsed)) {
    return false;
  }

  *out_value = std::move(parsed);
  return true;
}

void Pdf::set_object_stream_cache_capacity(size_t capacity) const {
  if (capacity == 0) {
    capacity = 1;
  }
  object_stream_cache_capacity = capacity;

  while (object_stream_cache_order.size() > object_stream_cache_capacity) {
    uint32_t evict = object_stream_cache_order.front();
    object_stream_cache_order.pop_front();
    object_stream_cache.erase(evict);
  }
}

bool Pdf::load_document_structure() {
  if (!object_offsets_built && !object_offsets_failed) {
    if (build_object_offset_cache()) {
      object_offsets_built = true;
    } else {
      object_offsets_failed = true;
    }
  }

  auto ensure_field_from_trailer = [&](const char* key, uint32_t* out) {
    if (*out != 0) {
      return;
    }
    auto it = trailer.find(key);
    if (it != trailer.end() && it->second.type == Value::REFERENCE) {
      *out = it->second.ref_object_number;
    }
  };

  if (size == 0) {
    auto it = trailer.find("Size");
    if (it != trailer.end() && it->second.type == Value::NUMBER) {
      size = static_cast<uint32_t>(it->second.number);
    }
  }

  ensure_field_from_trailer("Root", &root);
  ensure_field_from_trailer("Info", &info);
  ensure_field_from_trailer("Encrypt", &encrypt);

  // Initialize security handler if document is encrypted
  if (encrypt != 0) {
    security = create_security_handler(*this);
  }

  catalog.object_number = root;
  catalog.pages.clear();
  catalog.pages_count = 0;

  if (root == 0) {
    return false;
  }

  ResolvedObject root_obj = load_object(root, 0);
  if (!root_obj.success || root_obj.value.type != Value::DICTIONARY) {
    return false;
  }

  const Dictionary& root_dict = root_obj.value.dict;
  auto version_it = root_dict.find("Version");
  if (version_it != root_dict.end() && version_it->second.type == Value::NAME) {
    catalog.version = version_it->second.name;
  }

  struct InheritedProps {
    Dictionary resources;
    bool has_resources{false};
    std::vector<double> media_box;
    bool has_media_box{false};
    std::vector<double> crop_box;
    bool has_crop_box{false};
  };

  auto extract_box = [&](const Value& value, std::vector<double>* out) -> bool {
    if (value.type != Value::ARRAY || value.array.size() < 4) {
      return false;
    }
    std::vector<double> box;
    for (size_t i = 0; i < 4; ++i) {
      const Value& entry = value.array[i];
      if (entry.type != Value::NUMBER) {
        return false;
      }
      box.push_back(entry.number);
    }
    *out = std::move(box);
    return true;
  };

  auto resolve_dictionary_value = [&](const Value& value, Dictionary* out) -> bool {
    if (value.type == Value::DICTIONARY) {
      *out = value.dict;
      return true;
    }
    if (value.type == Value::REFERENCE) {
      ResolvedObject resolved = load_object(value.ref_object_number,
                                            value.ref_generation_number);
      if (resolved.success && resolved.value.type == Value::DICTIONARY) {
        *out = resolved.value.dict;
        return true;
      }
    }
    return false;
  };

  auto apply_inheritance = [&](const Dictionary& dict,
                               const InheritedProps& parent) {
    InheritedProps props = parent;

    auto res_it = dict.find("Resources");
    if (res_it != dict.end()) {
      Dictionary resolved;
      if (resolve_dictionary_value(res_it->second, &resolved)) {
        props.resources = std::move(resolved);
        props.has_resources = true;
      }
    }

    auto media_it = dict.find("MediaBox");
    if (media_it != dict.end()) {
      std::vector<double> box;
      if (extract_box(media_it->second, &box)) {
        props.media_box = std::move(box);
        props.has_media_box = true;
      }
    }

    auto crop_it = dict.find("CropBox");
    if (crop_it != dict.end()) {
      std::vector<double> box;
      if (extract_box(crop_it->second, &box)) {
        props.crop_box = std::move(box);
        props.has_crop_box = true;
      }
    }

    return props;
  };

  auto append_page = [&](const Dictionary& dict, uint32_t object_number,
                         const InheritedProps& inherited) {
    Page page;
    page.object_number = object_number;
    page.page_number = static_cast<uint32_t>(catalog.pages.size() + 1);

    Dictionary resources;
    bool has_resources = false;
    auto res_it = dict.find("Resources");
    if (res_it != dict.end()) {
      if (resolve_dictionary_value(res_it->second, &resources)) {
        has_resources = true;
      }
    }
    if (!has_resources && inherited.has_resources) {
      resources = inherited.resources;
      has_resources = true;
    }
    if (has_resources) {
      page.resources = std::move(resources);
      // Fonts are now loaded lazily via ensure_fonts_loaded()
      page.fonts_loaded = false;
    } else {
      page.fonts.clear();
      page.fonts_loaded = true;  // No resources = no fonts to load
    }

    auto media_it = dict.find("MediaBox");
    if (media_it != dict.end()) {
      std::vector<double> box;
      if (extract_box(media_it->second, &box)) {
        page.media_box = std::move(box);
      }
    } else if (inherited.has_media_box) {
      page.media_box = inherited.media_box;
    }

    auto crop_it = dict.find("CropBox");
    if (crop_it != dict.end()) {
      std::vector<double> box;
      if (extract_box(crop_it->second, &box)) {
        page.crop_box = std::move(box);
      }
    } else if (inherited.has_crop_box) {
      page.crop_box = inherited.crop_box;
    }

    auto rotate_it = dict.find("Rotate");
    if (rotate_it != dict.end() && rotate_it->second.type == Value::NUMBER) {
      page.rotate = rotate_it->second.number;
    }

    auto contents_it = dict.find("Contents");
    if (contents_it != dict.end()) {
      if (contents_it->second.type == Value::ARRAY) {
        page.contents = contents_it->second.array;
      } else {
        page.contents.clear();
        page.contents.push_back(contents_it->second);
      }
    }

    catalog.pages.push_back(std::move(page));
  };

  std::unordered_set<uint64_t> visited;
  std::function<void(uint32_t, const InheritedProps&)> walk_pages;
  std::function<void(const Dictionary&, uint32_t, const InheritedProps&)> process_dict;

  process_dict = [&](const Dictionary& dict, uint32_t object_number,
                     const InheritedProps& parent_props) {
    auto type_it = dict.find("Type");
    std::string type_name;
    if (type_it != dict.end() && type_it->second.type == Value::NAME) {
      type_name = type_it->second.name;
    }

    InheritedProps props = apply_inheritance(dict, parent_props);

    if (type_name == "Pages") {
      auto kids_it = dict.find("Kids");
      if (kids_it != dict.end() && kids_it->second.type == Value::ARRAY) {
        for (const auto& kid : kids_it->second.array) {
          if (kid.type == Value::REFERENCE) {
            walk_pages(kid.ref_object_number, props);
          } else if (kid.type == Value::DICTIONARY) {
            process_dict(kid.dict, 0, props);
          }
        }
      }
    } else if (type_name == "Page" || type_name.empty()) {
      append_page(dict, object_number, props);
    }
  };

  walk_pages = [&](uint32_t object_number, const InheritedProps& parent_props) {
    uint64_t key = (static_cast<uint64_t>(object_number) << 32);
    if (!visited.insert(key).second) {
      return;
    }

    ResolvedObject obj = load_object(object_number, 0);
    if (!obj.success || obj.value.type != Value::DICTIONARY) {
      return;
    }

    process_dict(obj.value.dict, object_number, parent_props);
  };

  InheritedProps root_props;
  root_props = apply_inheritance(root_dict, root_props);

  auto pages_it = root_dict.find("Pages");
  if (pages_it != root_dict.end()) {
    if (pages_it->second.type == Value::REFERENCE) {
      walk_pages(pages_it->second.ref_object_number, root_props);
    } else if (pages_it->second.type == Value::DICTIONARY) {
      process_dict(pages_it->second.dict, 0, root_props);
    }
  }

  catalog.pages_count = static_cast<uint32_t>(catalog.pages.size());
  pages_loaded = true;

  // Metadata is now loaded lazily via ensure_*_loaded() methods
  // Keeping eager loading for backward compatibility, but can be disabled
  // by setting NANOPDF_LAZY_METADATA=1 at compile time
#ifndef NANOPDF_LAZY_METADATA
  parse_acro_form(*this, catalog);
  acro_form_loaded = true;
  parse_document_outline(*this, catalog);
  outline_loaded = true;
  parse_page_labels(*this, catalog);
  page_labels_loaded = true;
  parse_named_destinations(*this, catalog);
  named_destinations_loaded = true;
  parse_document_info(*this, catalog);
  parse_xmp_metadata(*this, catalog);
  document_info_loaded = true;
  xmp_metadata_loaded = true;
#endif
  return true;
}

const Page* Pdf::get_page(uint32_t page_number) const {
  if (catalog.pages.empty()) {
    return nullptr;
  }

  if (page_number == 0) {
    return &catalog.pages.front();
  }

  if (page_number <= catalog.pages.size()) {
    return &catalog.pages[page_number - 1];
  }

  return nullptr;
}

// Lazy loading implementation methods
void Pdf::ensure_pages_loaded() const {
  if (!pages_loaded) {
    // Pages are loaded during load_document_structure
    // This method exists for explicit control
    const_cast<Pdf*>(this)->load_document_structure();
  }
}

void Pdf::ensure_acro_form_loaded() const {
  if (!acro_form_loaded) {
    ensure_pages_loaded();
    parse_acro_form(*this, const_cast<DocumentCatalog&>(catalog));
    acro_form_loaded = true;
  }
}

void Pdf::ensure_outline_loaded() const {
  if (!outline_loaded) {
    ensure_pages_loaded();
    parse_document_outline(*this, const_cast<DocumentCatalog&>(catalog));
    outline_loaded = true;
  }
}

void Pdf::ensure_page_labels_loaded() const {
  if (!page_labels_loaded) {
    ensure_pages_loaded();
    parse_page_labels(*this, const_cast<DocumentCatalog&>(catalog));
    page_labels_loaded = true;
  }
}

void Pdf::ensure_named_destinations_loaded() const {
  if (!named_destinations_loaded) {
    ensure_pages_loaded();
    parse_named_destinations(*this, const_cast<DocumentCatalog&>(catalog));
    named_destinations_loaded = true;
  }
}

void Pdf::ensure_metadata_loaded() const {
  if (!document_info_loaded) {
    ensure_pages_loaded();
    parse_document_info(*this, const_cast<DocumentCatalog&>(catalog));
    document_info_loaded = true;
  }
  if (!xmp_metadata_loaded) {
    parse_xmp_metadata(*this, const_cast<DocumentCatalog&>(catalog));
    xmp_metadata_loaded = true;
  }
}

// Page lazy font loading
void Page::ensure_fonts_loaded(const Pdf& pdf) const {
  if (!fonts_loaded) {
    Pdf::parse_font_resources(pdf, const_cast<Page&>(*this), resources);
    fonts_loaded = true;
  }
}

const BaseFont* Page::get_font(const std::string& name, const Pdf& pdf) const {
  ensure_fonts_loaded(pdf);
  auto it = fonts.find(name);
  return it != fonts.end() ? it->second.get() : nullptr;
}

PageContent Page::load_contents(const Pdf& pdf) const {
  PageContent content;

  if (contents.empty()) {
    content.success = true;
    return content;
  }

  std::vector<uint8_t> aggregate;
  std::vector<std::unique_ptr<Value>> owned_values;
  // Stack holds (Value*, object_number, generation_number) for decryption
  std::vector<std::tuple<const Value*, uint32_t, uint16_t>> stack;

  stack.reserve(contents.size());
  for (auto it = contents.rbegin(); it != contents.rend(); ++it) {
    stack.push_back(std::make_tuple(&*it, 0u, static_cast<uint16_t>(0)));
  }

  while (!stack.empty()) {
    const Value* node = std::get<0>(stack.back());
    uint32_t obj_num = std::get<1>(stack.back());
    uint16_t gen_num = std::get<2>(stack.back());
    stack.pop_back();

    switch (node->type) {
      case Value::REFERENCE: {
        ResolvedObject resolved =
            resolve_reference(pdf, node->ref_object_number,
                              node->ref_generation_number);
        if (!resolved.success) {
          content.error = resolved.error;
          content.success = false;
          return content;
        }
        owned_values.push_back(std::unique_ptr<Value>(new Value(std::move(resolved.value))));
        // Track the object number for decryption
        stack.push_back(std::make_tuple(owned_values.back().get(),
                                        node->ref_object_number,
                                        node->ref_generation_number));
        break;
      }

      case Value::ARRAY: {
        for (auto it = node->array.rbegin(); it != node->array.rend(); ++it) {
          stack.push_back(std::make_tuple(&*it, obj_num, gen_num));
        }
        break;
      }

      case Value::STREAM: {
        DecodedStream decoded = decode_stream(pdf, *node, obj_num, gen_num);
        if (!decoded.success) {
          content.error = decoded.error;
          content.success = false;
          return content;
        }

        if (!aggregate.empty() && !decoded.data.empty() &&
            aggregate.back() != '\n') {
          aggregate.push_back('\n');
        }
        aggregate.insert(aggregate.end(), decoded.data.begin(), decoded.data.end());
        break;
      }

      case Value::STRING: {
        if (!aggregate.empty() && !node->str.empty() &&
            aggregate.back() != '\n') {
          aggregate.push_back('\n');
        }
        aggregate.insert(aggregate.end(), node->str.begin(), node->str.end());
        break;
      }

      case Value::NULL_OBJ:
        break;

      default:
        content.error = "Unsupported content type";
        content.success = false;
        return content;
    }
  }

  content.data = std::move(aggregate);
  content.success = true;
  return content;
}

void Value::clear() {
  switch (type) {
    case STRING:
      str.clear();
      break;
    case NAME:
      name.clear();
      break;
    case ARRAY:
      array.clear();
      break;
    case DICTIONARY:
      dict.clear();
      break;
    case STREAM:
      stream = StreamValue{};
      break;
    case BOOLEAN:
      boolean = false;
      break;
    case NUMBER:
      number = 0.0;
      break;
    case REFERENCE:
    case NULL_OBJ:
    case UNDEFINED:
      break;
  }

  boolean = false;
  number = 0.0;
  ref_object_number = 0;
  ref_generation_number = 0;
  type = UNDEFINED;
}

void Value::SetType(Type new_type) {
  if (type != new_type) {
    clear();
    type = new_type;
  }

  switch (new_type) {
    case BOOLEAN:
      boolean = false;
      break;
    case NUMBER:
      number = 0.0;
      break;
    case STRING:
      str.clear();
      break;
    case NAME:
      name.clear();
      break;
    case ARRAY:
      array.clear();
      break;
    case DICTIONARY:
      dict.clear();
      break;
    case STREAM:
      stream = StreamValue{};
      break;
    case REFERENCE:
    case NULL_OBJ:
    case UNDEFINED:
      break;
  }

  if (new_type != BOOLEAN) {
    boolean = false;
  }
  if (new_type != NUMBER) {
    number = 0.0;
  }
  if (new_type == REFERENCE) {
    ref_object_number = 0;
    ref_generation_number = 0;
  } else {
    ref_object_number = 0;
    ref_generation_number = 0;
  }
}

// Color space parsing
ColorSpace parse_color_space(const Pdf& pdf, const Value& cs_value) {
  ColorSpace color_space;

  if (cs_value.type == Value::REFERENCE) {
    ResolvedObject resolved = resolve_reference(pdf, cs_value.ref_object_number,
                                                cs_value.ref_generation_number);
    if (resolved.success) {
      return parse_color_space(pdf, resolved.value);
    }
    return color_space;
  }

  if (cs_value.type == Value::NAME) {
    // Simple color space name
    const std::string& name = cs_value.name;
    if (name == "DeviceGray" || name == "G") {
      color_space.type = ColorSpaceType::DeviceGray;
    } else if (name == "DeviceRGB" || name == "RGB") {
      color_space.type = ColorSpaceType::DeviceRGB;
    } else if (name == "DeviceCMYK" || name == "CMYK") {
      color_space.type = ColorSpaceType::DeviceCMYK;
    } else if (name == "Pattern") {
      color_space.type = ColorSpaceType::Pattern;
    } else {
      // Unknown or resource name
      color_space.type = ColorSpaceType::DeviceGray;
    }
    color_space.name = name;
  } else if (cs_value.type == Value::ARRAY && !cs_value.array.empty()) {
    // Array color space specification
    const Value& type_val = cs_value.array[0];
    if (type_val.type == Value::NAME) {
      const std::string& type_name = type_val.name;

      if (type_name == "CalGray") {
        color_space.type = ColorSpaceType::CalGray;
        if (cs_value.array.size() > 1 && cs_value.array[1].type == Value::DICTIONARY) {
          const Dictionary& params = cs_value.array[1].dict;
          // Parse WhitePoint
          auto wp_it = params.find("WhitePoint");
          if (wp_it != params.end() && wp_it->second.type == Value::ARRAY) {
            for (const auto& v : wp_it->second.array) {
              if (v.type == Value::NUMBER) {
                color_space.white_point.push_back(v.number);
              }
            }
          }
          // Parse Gamma
          auto gamma_it = params.find("Gamma");
          if (gamma_it != params.end() && gamma_it->second.type == Value::NUMBER) {
            color_space.gamma.push_back(gamma_it->second.number);
          }
        }
      } else if (type_name == "CalRGB") {
        color_space.type = ColorSpaceType::CalRGB;
        if (cs_value.array.size() > 1 && cs_value.array[1].type == Value::DICTIONARY) {
          const Dictionary& params = cs_value.array[1].dict;
          // Parse WhitePoint
          auto wp_it = params.find("WhitePoint");
          if (wp_it != params.end() && wp_it->second.type == Value::ARRAY) {
            for (const auto& v : wp_it->second.array) {
              if (v.type == Value::NUMBER) {
                color_space.white_point.push_back(v.number);
              }
            }
          }
          // Parse Gamma
          auto gamma_it = params.find("Gamma");
          if (gamma_it != params.end() && gamma_it->second.type == Value::ARRAY) {
            for (const auto& v : gamma_it->second.array) {
              if (v.type == Value::NUMBER) {
                color_space.gamma.push_back(v.number);
              }
            }
          }
          // Parse Matrix
          auto matrix_it = params.find("Matrix");
          if (matrix_it != params.end() && matrix_it->second.type == Value::ARRAY) {
            for (const auto& v : matrix_it->second.array) {
              if (v.type == Value::NUMBER) {
                color_space.matrix.push_back(v.number);
              }
            }
          }
        }
      } else if (type_name == "ICCBased") {
        color_space.type = ColorSpaceType::ICCBased;
        if (cs_value.array.size() > 1 && cs_value.array[1].type == Value::STREAM) {
          // Get number of components from stream dictionary
          auto n_it = cs_value.array[1].stream.dict.find("N");
          if (n_it != cs_value.array[1].stream.dict.end() && n_it->second.type == Value::NUMBER) {
            color_space.num_components = static_cast<int>(n_it->second.number);
          }
          // Store ICC profile data
          color_space.icc_profile_data = cs_value.array[1].stream.data;
        }
      } else if (type_name == "Indexed") {
        color_space.type = ColorSpaceType::Indexed;
        if (cs_value.array.size() > 3) {
          // Parse base color space
          color_space.base_color_space.reset(new ColorSpace(
              parse_color_space(pdf, cs_value.array[1])));

          // Parse hival
          if (cs_value.array[2].type == Value::NUMBER) {
            color_space.hival = static_cast<int>(cs_value.array[2].number);
          }

          // Parse lookup table
          if (cs_value.array[3].type == Value::STRING) {
            color_space.lookup_table.assign(cs_value.array[3].str.begin(),
                                           cs_value.array[3].str.end());
          } else if (cs_value.array[3].type == Value::STREAM) {
            color_space.lookup_table = cs_value.array[3].stream.data;
          }
        }
      } else if (type_name == "Separation") {
        color_space.type = ColorSpaceType::Separation;
        if (cs_value.array.size() > 1 && cs_value.array[1].type == Value::NAME) {
          color_space.name = cs_value.array[1].name;
        }
        if (cs_value.array.size() > 2) {
          color_space.base_color_space.reset(new ColorSpace(
              parse_color_space(pdf, cs_value.array[2])));
        }
        if (cs_value.array.size() > 3) {
          color_space.tint_function = cs_value.array[3];
        }
      } else if (type_name == "DeviceN") {
        color_space.type = ColorSpaceType::DeviceN;
        color_space.colorant_names.clear();
        if (cs_value.array.size() > 1 && cs_value.array[1].type == Value::ARRAY) {
          for (const auto& name_val : cs_value.array[1].array) {
            if (name_val.type == Value::NAME) {
              color_space.colorant_names.push_back(name_val.name);
            }
          }
        }
        if (cs_value.array.size() > 2) {
          color_space.base_color_space.reset(new ColorSpace(
              parse_color_space(pdf, cs_value.array[2])));
        }
        if (cs_value.array.size() > 3) {
          color_space.tint_function = cs_value.array[3];
        }
      }
    }
  }

  return color_space;
}

// Image XObject parsing
ImageXObject parse_image_xobject(const Pdf& pdf, const Value& stream_value,
                                 uint32_t obj_num, uint16_t gen_num) {
  ImageXObject image;

  if (stream_value.type != Value::STREAM) {
    return image;  // Invalid image object
  }

  const Dictionary& dict = stream_value.stream.dict;

  image.raw_data = stream_value.stream.data;

  // Parse required entries
  auto width_it = dict.find("Width");
  if (width_it != dict.end() && width_it->second.type == Value::NUMBER) {
    image.width = static_cast<int>(width_it->second.number);
  }

  auto height_it = dict.find("Height");
  if (height_it != dict.end() && height_it->second.type == Value::NUMBER) {
    image.height = static_cast<int>(height_it->second.number);
  }

  // Parse BitsPerComponent (not required for ImageMask)
  auto bpc_it = dict.find("BitsPerComponent");
  if (bpc_it != dict.end() && bpc_it->second.type == Value::NUMBER) {
    image.bits_per_component = static_cast<int>(bpc_it->second.number);
  }

  // Parse ImageMask
  auto mask_it = dict.find("ImageMask");
  if (mask_it != dict.end() && mask_it->second.type == Value::BOOLEAN) {
    image.image_mask = mask_it->second.boolean;
  }

  // Parse ColorSpace
  if (!image.image_mask) {
    auto cs_it = dict.find("ColorSpace");
    if (cs_it != dict.end()) {
      image.color_space = parse_color_space(pdf, cs_it->second);
    }
  }

  // Parse Decode array
  auto decode_it = dict.find("Decode");
  if (decode_it != dict.end() && decode_it->second.type == Value::ARRAY) {
    for (const auto& v : decode_it->second.array) {
      if (v.type == Value::NUMBER) {
        image.decode.push_back(v.number);
      }
    }
  }

  // Parse Interpolate
  auto interp_it = dict.find("Interpolate");
  if (interp_it != dict.end() && interp_it->second.type == Value::BOOLEAN) {
    image.interpolate = interp_it->second.boolean;
  }

  auto smask_entry = dict.find("SMask");
  if (smask_entry != dict.end()) {
    image.soft_mask = smask_entry->second;
  }

  // Track applied filter (single filter support for now)
  auto filter_it = dict.find("Filter");
  if (filter_it != dict.end()) {
    if (filter_it->second.type == Value::NAME) {
      image.filter = filter_it->second.name;
    } else if (filter_it->second.type == Value::ARRAY) {
      for (const auto& item : filter_it->second.array) {
        if (item.type == Value::NAME) {
          image.filter = item.name;
          break;
        }
      }
    }
  }

  // Decode the image data (use obj_num/gen_num for decryption if needed)
  // Pass image dimensions to handle CCITT images with missing Rows/Columns params
  DecodedStream decoded = decode_stream(pdf, stream_value, obj_num, gen_num,
                                        image.width, image.height);
  if (decoded.success) {
    image.data = std::move(decoded.data);
  }

  NANOPDF_LOG_DEBUG("parse_image", "Image %ux%u, bpc=%d, cs_type=%d, filter=%s, data=%zu bytes",
                    static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height),
                    image.bits_per_component, static_cast<int>(image.color_space.type),
                    image.filter.c_str(), image.data.size());

  return image;
}

// Helper function to extract images from resources
std::map<std::string, ImageXObject> parse_xobject_resources(const Pdf& pdf, const Dictionary& resources) {
  std::map<std::string, ImageXObject> images;

  auto xobj_it = resources.find("XObject");
  if (xobj_it != resources.end() && xobj_it->second.type == Value::DICTIONARY) {
    for (const auto& entry : xobj_it->second.dict) {
      const Value& xobj_value = entry.second;

      // Resolve reference if needed
      Value resolved_value;
      if (xobj_value.type == Value::REFERENCE) {
        ResolvedObject resolved = resolve_reference(pdf, xobj_value.ref_object_number,
                                                    xobj_value.ref_generation_number);
        if (resolved.success) {
          resolved_value = std::move(resolved.value);
        }
      } else {
        resolved_value = xobj_value;
      }

      // Check if it's an image
      if (resolved_value.type == Value::STREAM) {
        auto subtype_it = resolved_value.stream.dict.find("Subtype");
        if (subtype_it != resolved_value.stream.dict.end() &&
            subtype_it->second.type == Value::NAME &&
            subtype_it->second.name == "Image") {
          // Pass object numbers for proper decryption in encrypted PDFs
          uint32_t obj_num = (xobj_value.type == Value::REFERENCE)
                             ? xobj_value.ref_object_number : 0;
          uint16_t gen_num = (xobj_value.type == Value::REFERENCE)
                             ? xobj_value.ref_generation_number : 0;
          images[entry.first] = parse_image_xobject(pdf, resolved_value, obj_num, gen_num);
        }
      }
    }
  }

  return images;
}

namespace {

static const uint16_t kWinAnsiEncoding[256] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
    0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF
};

static const uint16_t kMacRomanEncoding[256] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
    0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
};

static const char* kStandardEncodingNames[256] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, "exclamdown", "cent", "sterling", "fraction", "yen", "florin", "section",
    "currency", "quotesingle", "quotedblleft", "guillemotleft", "guilsinglleft", "guilsinglright", "fi", "fl",
    nullptr, "endash", "dagger", "daggerdbl", "periodcentered", nullptr, "paragraph", "bullet",
    "quotesinglbase", "quotedblbase", "quotedblright", "guillemotright", "ellipsis", "perthousand", nullptr, "questiondown",
    nullptr, "grave", "acute", "circumflex", "tilde", "macron", "breve", "dotaccent",
    "dieresis", nullptr, "ring", "cedilla", nullptr, "hungarumlaut", "ogonek", "caron",
    "emdash", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, "AE", nullptr, "ordfeminine", nullptr, nullptr, nullptr, nullptr,
    "Lslash", "Oslash", "OE", "ordmasculine", nullptr, nullptr, nullptr, nullptr,
    nullptr, "ae", nullptr, nullptr, nullptr, "dotlessi", nullptr, nullptr,
    "lslash", "oslash", "oe", "germandbls", nullptr, nullptr, nullptr, nullptr,
};

}  // namespace

// Text extraction implementation
class TextExtractor {
public:
  TextExtractor(const Pdf& pdf, const Page& page)
    : pdf_(pdf), page_(page) {}

  std::string extract_text() {
    PageContent content = page_.load_contents(pdf_);
    if (!content.success) {
      return "";
    }

    text_state_.reset();
    extracted_text_.clear();

    process_content_stream(content.data);

    return extracted_text_;
  }

private:
  const Pdf& pdf_;
  const Page& page_;
  TextState text_state_;
  std::string extracted_text_;

  void process_content_stream(const std::vector<uint8_t>& data) {
    std::string content(data.begin(), data.end());
    std::vector<std::string> tokens = tokenize_content(content);
    std::vector<std::string> operand_stack;

    for (const auto& token : tokens) {
      if (is_operator(token)) {
        execute_operator(token, operand_stack);
        operand_stack.clear();
      } else {
        operand_stack.push_back(token);
      }
    }
  }

  std::vector<std::string> tokenize_content(const std::string& content) {
    std::vector<std::string> tokens;
    size_t pos = 0;

    while (pos < content.size()) {
      // Skip whitespace
      while (pos < content.size() && std::isspace(content[pos])) {
        pos++;
      }

      if (pos >= content.size()) break;

      // Parse token
      if (content[pos] == '(') {
        // String literal
        std::string str = "(";
        pos++;
        int paren_depth = 1;
        while (pos < content.size() && paren_depth > 0) {
          if (content[pos] == '\\' && pos + 1 < content.size()) {
            str += content[pos++];
            str += content[pos++];
          } else if (content[pos] == '(') {
            paren_depth++;
            str += content[pos++];
          } else if (content[pos] == ')') {
            paren_depth--;
            str += content[pos++];
          } else {
            str += content[pos++];
          }
        }
        tokens.push_back(str);
      } else if (content[pos] == '<' && pos + 1 < content.size() && content[pos + 1] == '<') {
        // Dictionary start
        tokens.push_back("<<");
        pos += 2;
      } else if (content[pos] == '>' && pos + 1 < content.size() && content[pos + 1] == '>') {
        // Dictionary end
        tokens.push_back(">>");
        pos += 2;
      } else if (content[pos] == '<') {
        // Hex string
        std::string hex = "<";
        pos++;
        while (pos < content.size() && content[pos] != '>') {
          hex += content[pos++];
        }
        if (pos < content.size()) {
          hex += content[pos++];
        }
        tokens.push_back(hex);
      } else if (content[pos] == '[' || content[pos] == ']') {
        // Array delimiter
        tokens.push_back(std::string(1, content[pos]));
        pos++;
      } else if (content[pos] == '/') {
        // Name
        std::string name = "/";
        pos++;
        while (pos < content.size() && !std::isspace(content[pos]) &&
               content[pos] != '/' && content[pos] != '[' && content[pos] != ']' &&
               content[pos] != '<' && content[pos] != '>' && content[pos] != '(') {
          name += content[pos++];
        }
        tokens.push_back(name);
      } else {
        // Number or operator
        std::string token;
        while (pos < content.size() && !std::isspace(content[pos]) &&
               content[pos] != '/' && content[pos] != '[' && content[pos] != ']' &&
               content[pos] != '<' && content[pos] != '>' && content[pos] != '(') {
          token += content[pos++];
        }
        tokens.push_back(token);
      }
    }

    return tokens;
  }

  bool is_operator(const std::string& token) {
    static const std::set<std::string> operators = {
      "BT", "ET", "Td", "TD", "Tm", "T*", "Tj", "TJ", "'", "\"",
      "Tc", "Tw", "Tz", "TL", "Tf", "Tr", "Ts",
      "q", "Q", "cm", "w", "J", "j", "M", "d", "ri", "i", "gs",
      "m", "l", "c", "v", "y", "h", "re", "S", "s", "f", "F", "f*",
      "B", "B*", "b", "b*", "n", "W", "W*",
      "CS", "cs", "SC", "SCN", "sc", "scn", "G", "g", "RG", "rg", "K", "k"
    };
    return operators.find(token) != operators.end();
  }

  void execute_operator(const std::string& op, const std::vector<std::string>& operands) {
    if (op == "BT") {
      // Begin text block
      ensure_line_break();
      flush_text();
      text_state_.reset();
    } else if (op == "ET") {
      // End text block
      ensure_line_break();
      flush_text();
    } else if (op == "Td" && operands.size() >= 2) {
      // Move text position
      double tx = parse_number(operands[0]);
      double ty = parse_number(operands[1]);
      if (ty < 0.0) {
        ensure_line_break();
      }
      text_state_.line_matrix[4] += tx * text_state_.line_matrix[0] + ty * text_state_.line_matrix[2];
      text_state_.line_matrix[5] += tx * text_state_.line_matrix[1] + ty * text_state_.line_matrix[3];
      std::copy(text_state_.line_matrix, text_state_.line_matrix + 6, text_state_.text_matrix);
    } else if (op == "TD" && operands.size() >= 2) {
      // Move text position and set leading
      double tx = parse_number(operands[0]);
      double ty = parse_number(operands[1]);
      text_state_.leading = -ty;
      if (ty < 0.0) {
        ensure_line_break();
      }
      text_state_.line_matrix[4] += tx * text_state_.line_matrix[0] + ty * text_state_.line_matrix[2];
      text_state_.line_matrix[5] += tx * text_state_.line_matrix[1] + ty * text_state_.line_matrix[3];
      std::copy(text_state_.line_matrix, text_state_.line_matrix + 6, text_state_.text_matrix);
    } else if (op == "Tm" && operands.size() >= 6) {
      // Set text matrix
      for (int i = 0; i < 6; i++) {
        text_state_.text_matrix[i] = parse_number(operands[i]);
      }
      std::copy(text_state_.text_matrix, text_state_.text_matrix + 6, text_state_.line_matrix);
    } else if (op == "T*") {
      // Move to next line
      ensure_line_break();
      text_state_.line_matrix[4] += 0;
      text_state_.line_matrix[5] -= text_state_.leading;
      std::copy(text_state_.line_matrix, text_state_.line_matrix + 6, text_state_.text_matrix);
    } else if (op == "Tj" && operands.size() >= 1) {
      // Show text string
      std::string text = decode_text_string(operands[0]);
      text_state_.current_text += text;
    } else if (op == "TJ" && operands.size() >= 1) {
      // Show text with individual positioning
      std::string array_str = operands[0];
      if (array_str.front() == '[' && array_str.back() == ']') {
        array_str = array_str.substr(1, array_str.size() - 2);
        // Parse array elements
        std::vector<std::string> elements = parse_array_elements(array_str);
        for (const auto& elem : elements) {
          if (elem.front() == '(' || elem.front() == '<') {
            text_state_.current_text += decode_text_string(elem);
          }
          // Numbers adjust spacing, but we'll ignore for simple extraction
        }
      }
    } else if (op == "'" && operands.size() >= 1) {
      // Move to next line and show text
      execute_operator("T*", {});
      execute_operator("Tj", operands);
    } else if (op == "\"" && operands.size() >= 3) {
      // Set word/char spacing, move to next line, show text
      text_state_.word_spacing = parse_number(operands[0]);
      text_state_.char_spacing = parse_number(operands[1]);
      execute_operator("T*", {});
      execute_operator("Tj", {operands[2]});
    } else if (op == "Tc" && operands.size() >= 1) {
      text_state_.char_spacing = parse_number(operands[0]);
    } else if (op == "Tw" && operands.size() >= 1) {
      text_state_.word_spacing = parse_number(operands[0]);
    } else if (op == "Tz" && operands.size() >= 1) {
      text_state_.horizontal_scaling = parse_number(operands[0]);
    } else if (op == "TL" && operands.size() >= 1) {
      text_state_.leading = parse_number(operands[0]);
    } else if (op == "Tf" && operands.size() >= 2) {
      text_state_.font_name = operands[0];
      text_state_.font_size = parse_number(operands[1]);
      // Look up font in page resources (lazy load if needed)
      std::string font_name = text_state_.font_name.substr(1);  // Remove leading /
      text_state_.current_font = page_.get_font(font_name, pdf_);
    } else if (op == "Tr" && operands.size() >= 1) {
      int mode = static_cast<int>(parse_number(operands[0]));
      if (mode >= 0 && mode <= 7) {
        text_state_.render_mode = static_cast<TextRenderingMode>(mode);
      }
    } else if (op == "Ts" && operands.size() >= 1) {
      text_state_.text_rise = parse_number(operands[0]);
    }
  }

  double parse_number(const std::string& str) {
    try {
      return std::stod(str);
    } catch (...) {
      return 0.0;
    }
  }

  std::string decode_text_string(const std::string& str) {
    std::string raw = decode_pdf_string_raw(str);
    return map_text_with_font(raw);
  }

  std::string decode_pdf_string_raw(const std::string& str) {
    if (str.empty()) return "";

    if (str[0] == '(') {
      // Literal string
      std::string result = str.substr(1, str.size() - 2);
      // Decode escape sequences
      size_t pos = 0;
      while ((pos = result.find("\\", pos)) != std::string::npos) {
        if (pos + 1 < result.size()) {
          char next = result[pos + 1];
          if (next == 'n') {
            result.replace(pos, 2, "\n");
          } else if (next == 'r') {
            result.replace(pos, 2, "\r");
          } else if (next == 't') {
            result.replace(pos, 2, "\t");
          } else if (next == '\\' || next == '(' || next == ')') {
            result.erase(pos, 1);
          } else if (next >= '0' && next <= '7') {
            // Octal sequence
            std::string octal;
            size_t i = pos + 1;
            while (i < result.size() && i < pos + 4 &&
                   result[i] >= '0' && result[i] <= '7') {
              octal += result[i++];
            }
            int value = std::stoi(octal, nullptr, 8);
            result.replace(pos, i - pos, 1, static_cast<char>(value));
          } else {
            pos++;
          }
        } else {
          pos++;
        }
      }
      return result;
    } else if (str[0] == '<') {
      // Hex string
      std::string hex = str.substr(1, str.size() - 2);
      std::string result;
      for (size_t i = 0; i < hex.size(); i += 2) {
        std::string byte = hex.substr(i, 2);
        if (byte.size() == 1) byte += "0";
        result += static_cast<char>(std::stoi(byte, nullptr, 16));
      }
      return result;
    }

    return str;
  }

  std::string map_text_with_font(const std::string& raw) {
    if (!text_state_.current_font) {
      return raw;
    }

    const BaseFont* base_font = text_state_.current_font;
    if (!base_font->to_unicode_cmap.code_to_unicode.empty()) {
      return map_with_cmap(base_font->to_unicode_cmap, raw);
    }

    if (base_font->subtype != "Type0" &&
        (base_font->encoding.empty() || base_font->encoding == "StandardEncoding")) {
      return map_with_standard_encoding(base_font, raw);
    }

    if (base_font->subtype == "Type0") {
      const Type0Font* type0 = dynamic_cast<const Type0Font*>(base_font);
      if (type0) {
        if (!type0->encoding_cmap.code_to_unicode.empty()) {
          return map_with_cmap(type0->encoding_cmap, raw);
        }
        if (uses_identity_cmap(type0->encoding_cmap) ||
            uses_identity_cmap(base_font->to_unicode_cmap)) {
          return decode_utf16be(raw);
        }
      }
    }

    if (base_font->encoding == "WinAnsiEncoding") {
      return map_with_single_byte_encoding(base_font, kWinAnsiEncoding, raw);
    }
    if (base_font->encoding == "MacRomanEncoding") {
      return map_with_single_byte_encoding(base_font, kMacRomanEncoding, raw);
    }

    return raw;
  }

  void ensure_line_break() {
    if (!text_state_.current_text.empty()) {
      if (text_state_.current_text.back() != '\n') {
        text_state_.current_text.push_back('\n');
      }
    } else if (!extracted_text_.empty() && extracted_text_.back() != '\n') {
      extracted_text_.push_back('\n');
    }
  }

  void flush_text() {
    if (!text_state_.current_text.empty()) {
      extracted_text_ += text_state_.current_text;
      text_state_.current_text.clear();
    }
  }

  bool apply_encoding_difference(const BaseFont* font, uint8_t byte,
                                 std::string* out) {
    if (!font || !out || font->encoding_differences.empty()) {
      return false;
    }

    auto diff_it = font->encoding_differences.find(static_cast<uint32_t>(byte));
    if (diff_it == font->encoding_differences.end()) {
      return false;
    }

    std::string glyph_utf8;
    if (glyph_name_to_unicode(diff_it->second, &glyph_utf8)) {
      out->append(glyph_utf8);
      return true;
    }

    return false;
  }

  std::string map_with_single_byte_encoding(const BaseFont* font, const uint16_t* table,
                                            const std::string& raw) {
    if (!table) {
      return raw;
    }

    std::string result;
    result.reserve(raw.size());

    for (unsigned char byte : raw) {
      if (apply_encoding_difference(font, byte, &result)) {
        continue;
      }

      uint16_t code = table[byte];
      if (code == 0) {
        if (byte == '\n' || byte == '\r' || byte == '\t' || byte == ' ') {
          result.push_back(static_cast<char>(byte));
        } else if (byte >= 32) {
          result.push_back(static_cast<char>(byte));
        } else {
          result.push_back('?');
        }
        continue;
      }

      append_utf8(code, &result);
    }

    return result;
  }

  std::string map_with_standard_encoding(const BaseFont* font, const std::string& raw) {
    std::string result;
    result.reserve(raw.size());

    for (unsigned char byte : raw) {
      if (apply_encoding_difference(font, byte, &result)) {
        continue;
      }

      if (byte >= 32 && byte <= 126) {
        result.push_back(static_cast<char>(byte));
        continue;
      }

      if (byte == '\n' || byte == '\r' || byte == '\t' || byte == ' ') {
        result.push_back(static_cast<char>(byte));
        continue;
      }

      const char* name = kStandardEncodingNames[byte];
      if (name) {
        std::string glyph_utf8;
        if (glyph_name_to_unicode(name, &glyph_utf8)) {
          result.append(glyph_utf8);
          continue;
        }
      }

      result.push_back('?');
    }

    return result;
  }

  std::string map_with_cmap(const CMap& cmap, const std::string& raw) {
    if (cmap.code_to_unicode.empty()) {
      return raw;
    }

    std::string result;
    size_t pos = 0;
    while (pos < raw.size()) {
      bool matched = false;
      size_t remaining = raw.size() - pos;
      size_t max_len = std::min<size_t>(4, remaining);
      for (size_t length = max_len; length > 0; --length) {
        uint32_t code = 0;
        for (size_t i = 0; i < length; ++i) {
          code = (code << 8) | static_cast<unsigned char>(raw[pos + i]);
        }
        auto it = cmap.code_to_unicode.find(code);
        if (it != cmap.code_to_unicode.end()) {
          append_utf8(it->second, &result);
          pos += length;
          matched = true;
          break;
        }
      }

      if (!matched) {
        unsigned char byte = static_cast<unsigned char>(raw[pos]);
        if (byte >= 32 || byte == '\n' || byte == '\r' || byte == '\t') {
          result += static_cast<char>(byte);
        } else {
          result += '?';
        }
        pos += 1;
      }
    }

    return result;
  }

  void append_utf8(uint32_t codepoint, std::string* out) {
    if (!out) {
      return;
    }

    if (codepoint <= 0x7F) {
      out->push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
      out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
      if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
        return;
      }
      out->push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
      out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
      out->push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
      out->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
      out->push_back('?');
    }
  }

  bool glyph_name_to_unicode(const std::string& name, std::string* utf8_out) {
    if (!utf8_out || name.empty() || name == ".notdef") {
      return false;
    }

    utf8_out->clear();

    // Handle names like "uniXXXX" or "uniXXXXYYYY"
    if (name.size() >= 7 && name.compare(0, 3, "uni") == 0) {
      size_t pos = 3;
      if (((name.size() - pos) % 4) != 0) {
        return false;
      }
      while (pos + 4 <= name.size()) {
        std::string hex = name.substr(pos, 4);
        char* end = nullptr;
        long value = std::strtol(hex.c_str(), &end, 16);
        if (!(end && *end == '\0')) {
          utf8_out->clear();
          return false;
        }
        append_utf8(static_cast<uint32_t>(value), utf8_out);
        pos += 4;
      }
      return !utf8_out->empty();
    }

    if (name.size() >= 5 && name.front() == 'u') {
      std::string hex = name.substr(1);
      char* end = nullptr;
      long value = std::strtol(hex.c_str(), &end, 16);
      if (end && *end == '\0') {
        append_utf8(static_cast<uint32_t>(value), utf8_out);
        return true;
      }
    }

    if (name.size() == 1) {
      unsigned char ch = static_cast<unsigned char>(name[0]);
      if (ch >= 32) {
        utf8_out->push_back(static_cast<char>(ch));
        return true;
      }
    }

    const auto& table = glyph_name_map();
    auto it = table.find(name);
    if (it != table.end()) {
      *utf8_out = it->second;
      return true;
    }

    return false;
  }
  const std::unordered_map<std::string, std::string>& glyph_name_map() {
    static std::unordered_map<std::string, std::string> map;
    if (map.empty()) {
      for (size_t i = 0; i < kAdobeGlyphListCount; ++i) {
        map.emplace(kAdobeGlyphList[i].name, std::string(kAdobeGlyphList[i].utf8));
      }
    }
    return map;
  }

  bool uses_identity_cmap(const CMap& cmap) const {
    if (cmap.name.empty()) {
      return false;
    }
    return cmap.name == "Identity-H" || cmap.name == "Identity-V";
  }

  std::string decode_utf16be(const std::string& raw) {
    if (raw.size() < 2) {
      return raw;
    }

    std::string result;
    size_t pos = 0;

    if (raw.size() >= 2) {
      unsigned char b0 = static_cast<unsigned char>(raw[0]);
      unsigned char b1 = static_cast<unsigned char>(raw[1]);
      if (b0 == 0xFE && b1 == 0xFF) {
        pos = 2;
      }
    }

    while (pos + 1 < raw.size()) {
      uint16_t high = static_cast<uint8_t>(raw[pos]);
      uint16_t low = static_cast<uint8_t>(raw[pos + 1]);
      uint16_t code = static_cast<uint16_t>((high << 8) | low);
      pos += 2;

      if (code >= 0xD800 && code <= 0xDBFF) {
        if (pos + 1 >= raw.size()) {
          break;
        }
        uint16_t low_high = static_cast<uint8_t>(raw[pos]);
        uint16_t low_low = static_cast<uint8_t>(raw[pos + 1]);
        uint16_t low_code = static_cast<uint16_t>((low_high << 8) | low_low);
        pos += 2;
        if (low_code >= 0xDC00 && low_code <= 0xDFFF) {
          uint32_t full = 0x10000 +
                          (((static_cast<uint32_t>(code) - 0xD800) << 10) |
                           (static_cast<uint32_t>(low_code) - 0xDC00));
          append_utf8(full, &result);
        }
        continue;
      }

      append_utf8(code, &result);
    }

    return result;
  }



  std::vector<std::string> parse_array_elements(const std::string& array_content) {
    std::vector<std::string> elements;
    size_t pos = 0;

    while (pos < array_content.size()) {
      // Skip whitespace
      while (pos < array_content.size() && std::isspace(array_content[pos])) {
        pos++;
      }

      if (pos >= array_content.size()) break;

      if (array_content[pos] == '(') {
        // String literal
        size_t start = pos;
        pos++;
        int depth = 1;
        while (pos < array_content.size() && depth > 0) {
          if (array_content[pos] == '\\' && pos + 1 < array_content.size()) {
            pos += 2;
          } else if (array_content[pos] == '(') {
            depth++;
            pos++;
          } else if (array_content[pos] == ')') {
            depth--;
            pos++;
          } else {
            pos++;
          }
        }
        elements.push_back(array_content.substr(start, pos - start));
      } else if (array_content[pos] == '<') {
        // Hex string
        size_t start = pos;
        pos++;
        while (pos < array_content.size() && array_content[pos] != '>') {
          pos++;
        }
        if (pos < array_content.size()) pos++;
        elements.push_back(array_content.substr(start, pos - start));
      } else {
        // Number
        size_t start = pos;
        while (pos < array_content.size() && !std::isspace(array_content[pos]) &&
               array_content[pos] != '(' && array_content[pos] != '<') {
          pos++;
        }
        if (pos > start) {
          elements.push_back(array_content.substr(start, pos - start));
        }
      }
    }

    return elements;
  }

  // Need to include <set> for std::set
};

bool Pdf::parse_font_resources(const Pdf& pdf, Page& page,
                               const Dictionary& resources) {
  return pdf.parse_font_resources(page, resources);
}

bool Pdf::parse_font_resources(Page& page, const Dictionary& resources) const {
  // Reset fonts map before populating
  page.fonts.clear();

  auto font_it = resources.find("Font");
  if (font_it == resources.end()) {
    return true;
  }

  auto resolve_dict = [&](const Value& value, Dictionary* out) -> bool {
    if (value.type == Value::DICTIONARY) {
      *out = value.dict;
      return true;
    }
    if (value.type == Value::REFERENCE) {
      ResolvedObject resolved =
          load_object(value.ref_object_number, value.ref_generation_number);
      if (resolved.success && resolved.value.type == Value::DICTIONARY) {
        *out = resolved.value.dict;
        return true;
      }
    }
    return false;
  };

  Dictionary font_resource_dict;
  if (!resolve_dict(font_it->second, &font_resource_dict)) {
    return false;
  }

  for (const auto& entry : font_resource_dict) {
    const Value& font_val = entry.second;
    Value resolved_font = font_val;
    if (resolved_font.type == Value::REFERENCE) {
      ResolvedObject resolved =
          load_object(resolved_font.ref_object_number,
                      resolved_font.ref_generation_number);
      if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
        continue;
      }
      resolved_font = resolved.value;
    }

    if (resolved_font.type != Value::DICTIONARY) {
      continue;
    }

    std::unique_ptr<BaseFont> font = parse_font(resolved_font);
    if (font) {
      page.fonts[entry.first] = std::move(font);
    }
  }

  return true;
}

std::unique_ptr<BaseFont> Pdf::parse_font(const Value& font_val) const {
  return parse_font(*this, font_val);
}

std::unique_ptr<FontDescriptor> Pdf::parse_font_descriptor(const Value& font_val) const {
  return parse_font_descriptor(*this, font_val);
}

namespace {

std::string trim_cmap_line(const std::string& line) {
  size_t start = 0;
  size_t end = line.size();
  while (start < end && std::isspace(static_cast<unsigned char>(line[start]))) {
    start++;
  }
  while (end > start && std::isspace(static_cast<unsigned char>(line[end - 1]))) {
    end--;
  }
  return line.substr(start, end - start);
}

std::vector<std::string> tokenize_cmap_line(const std::string& line) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < line.size()) {
    unsigned char ch = static_cast<unsigned char>(line[i]);
    if (std::isspace(ch)) {
      i++;
      continue;
    }
    if (line[i] == '%') {
      break;
    }
    if (line[i] == '<') {
      size_t j = i + 1;
      while (j < line.size() && line[j] != '>') {
        j++;
      }
      if (j < line.size()) {
        j++;
      }
      tokens.push_back(line.substr(i, j - i));
      i = j;
      continue;
    }
    if (line[i] == '[') {
      size_t j = i + 1;
      int depth = 1;
      while (j < line.size() && depth > 0) {
        if (line[j] == '[') {
          depth++;
        } else if (line[j] == ']') {
          depth--;
        }
        j++;
      }
      tokens.push_back(line.substr(i, j - i));
      i = j;
      continue;
    }
    size_t j = i + 1;
    while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) {
      j++;
    }
    tokens.push_back(line.substr(i, j - i));
    i = j;
  }
  return tokens;
}

bool parse_integer_token(const std::string& token, int* out) {
  if (!out) {
    return false;
  }
  try {
    size_t consumed = 0;
    int value = std::stoi(token, &consumed, 10);
    if (consumed != token.size()) {
      return false;
    }
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_literal_number(const std::string& token, uint32_t* out) {
  if (!out) {
    return false;
  }
  try {
    size_t consumed = 0;
    uint32_t value = static_cast<uint32_t>(std::stoul(token, &consumed, 10));
    if (consumed != token.size()) {
      return false;
    }
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_hex_string_token(const std::string& token, uint32_t* out) {
  if (!out || token.size() < 2 || token.front() != '<') {
    return false;
  }
  size_t end_pos = token.find('>');
  if (end_pos == std::string::npos) {
    return false;
  }
  std::string hex;
  for (size_t i = 1; i < end_pos; ++i) {
    char c = token[i];
    if (!std::isspace(static_cast<unsigned char>(c))) {
      hex += c;
    }
  }
  if (hex.empty()) {
    *out = 0;
    return true;
  }
  try {
    *out = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<uint32_t> parse_hex_array_token(const std::string& token) {
  std::vector<uint32_t> values;
  size_t i = 0;
  while (i < token.size()) {
    if (token[i] == '<') {
      size_t j = token.find('>', i);
      if (j == std::string::npos) {
        break;
      }
      uint32_t value = 0;
      if (parse_hex_string_token(token.substr(i, j - i + 1), &value)) {
        values.push_back(value);
      }
      i = j + 1;
    } else {
      i++;
    }
  }
  return values;
}

bool resolve_indirect_value(const Pdf& pdf, const Value& value, Value* out) {
  if (!out) {
    return false;
  }
  if (value.type == Value::REFERENCE) {
    ResolvedObject resolved =
        resolve_reference(pdf, value.ref_object_number, value.ref_generation_number);
    if (!resolved.success) {
      return false;
    }
    *out = resolved.value;
    return true;
  }
  *out = value;
  return true;
}

void parse_encoding_differences(const std::vector<Value>& diffs, BaseFont* font) {
  if (!font) {
    return;
  }

  uint32_t current_code = 0;
  bool have_code = false;

  for (const auto& entry : diffs) {
    if (entry.type == Value::NUMBER) {
      current_code = static_cast<uint32_t>(entry.number);
      have_code = true;
      continue;
    }
    if (!have_code) {
      continue;
    }

    if (entry.type == Value::NAME) {
      font->encoding_differences[current_code] = entry.name;
      current_code++;
      continue;
    }

    if (entry.type == Value::STRING) {
      font->encoding_differences[current_code] = entry.str;
      current_code++;
      continue;
    }
  }
}

void parse_simple_font_encoding(const Pdf& pdf, const Value& encoding_value, BaseFont* font) {
  if (!font) {
    return;
  }

  Value resolved;
  if (!resolve_indirect_value(pdf, encoding_value, &resolved)) {
    return;
  }

  if (resolved.type == Value::NAME) {
    font->encoding = resolved.name;
    font->encoding_differences.clear();
    return;
  }

  if (resolved.type != Value::DICTIONARY) {
    return;
  }

  const Dictionary& dict = resolved.dict;

  auto base_it = dict.find("BaseEncoding");
  if (base_it != dict.end() && base_it->second.type == Value::NAME) {
    font->encoding = base_it->second.name;
  } else if (font->encoding.empty()) {
    font->encoding = "StandardEncoding";
  }

  auto diff_it = dict.find("Differences");
  if (diff_it != dict.end() && diff_it->second.type == Value::ARRAY) {
    font->encoding_differences.clear();
    parse_encoding_differences(diff_it->second.array, font);
  }
}

void extract_cid_system_info(const Dictionary& dict, std::string* registry,
                             std::string* ordering, int* supplement) {
  auto registry_it = dict.find("Registry");
  if (registry && registry_it != dict.end()) {
    if (registry_it->second.type == Value::STRING) {
      *registry = registry_it->second.str;
    } else if (registry_it->second.type == Value::NAME) {
      *registry = registry_it->second.name;
    }
  }
  auto ordering_it = dict.find("Ordering");
  if (ordering && ordering_it != dict.end()) {
    if (ordering_it->second.type == Value::STRING) {
      *ordering = ordering_it->second.str;
    } else if (ordering_it->second.type == Value::NAME) {
      *ordering = ordering_it->second.name;
    }
  }
  auto supplement_it = dict.find("Supplement");
  if (supplement && supplement_it != dict.end() &&
      supplement_it->second.type == Value::NUMBER) {
    *supplement = static_cast<int>(supplement_it->second.number);
  }
}

void parse_cid_system_info(const Dictionary& dict, Type0Font* font) {
  if (!font) {
    return;
  }
  std::string registry = font->registry;
  std::string ordering = font->ordering;
  int supplement = font->supplement;
  extract_cid_system_info(dict, &registry, &ordering, &supplement);
  if (!registry.empty()) {
    font->registry = registry;
    font->encoding_cmap.registry = registry;
  }
  if (!ordering.empty()) {
    font->ordering = ordering;
    font->encoding_cmap.ordering = ordering;
  }
  font->supplement = supplement;
  font->encoding_cmap.supplement = supplement;
}

void parse_cid_width_array(const std::vector<Value>& array, Type0Font* font) {
  if (!font) {
    return;
  }
  size_t i = 0;
  while (i < array.size()) {
    if (array[i].type != Value::NUMBER) {
      i++;
      continue;
    }
    uint32_t cid = static_cast<uint32_t>(array[i].number);
    i++;
    if (i >= array.size()) {
      break;
    }
    const Value& next = array[i];
    if (next.type == Value::ARRAY) {
      const auto& widths_array = next.array;
      for (size_t j = 0; j < widths_array.size(); ++j) {
        if (widths_array[j].type == Value::NUMBER) {
          font->cid_widths[cid + static_cast<uint32_t>(j)] =
              static_cast<int>(widths_array[j].number);
        }
      }
      i++;
    } else if (next.type == Value::NUMBER) {
      uint32_t end_cid = static_cast<uint32_t>(next.number);
      i++;
      if (i >= array.size()) {
        break;
      }
      if (array[i].type == Value::NUMBER) {
        int width = static_cast<int>(array[i].number);
        for (uint32_t current = cid; current <= end_cid; ++current) {
          font->cid_widths[current] = width;
        }
      }
      i++;
    } else {
      i++;
    }
  }
}

void parse_cid_to_gid_map(const Pdf& pdf, const Value& value, Type0Font* font) {
  if (!font) {
    return;
  }
  Value resolved;
  if (!resolve_indirect_value(pdf, value, &resolved)) {
    return;
  }
  if (resolved.type == Value::NAME) {
    font->cid_to_gid_map.clear();
    return;
  }
  if (resolved.type != Value::STREAM) {
    return;
  }
  DecodedStream decoded = decode_stream(pdf, resolved);
  if (!decoded.success) {
    return;
  }
  const auto& data = decoded.data;
  font->cid_to_gid_map.clear();
  for (size_t i = 0; i + 1 < data.size(); i += 2) {
    uint16_t gid = static_cast<uint16_t>((static_cast<uint16_t>(data[i]) << 8) |
                                         static_cast<uint16_t>(data[i + 1]));
    font->cid_to_gid_map.push_back(gid);
  }
}

std::string extract_parenthesized_value(const std::string& line,
                                        const std::string& key) {
  auto key_pos = line.find(key);
  if (key_pos == std::string::npos) {
    return std::string();
  }
  auto start = line.find('(', key_pos);
  auto end = line.find(')', start + 1);
  if (start == std::string::npos || end == std::string::npos || end <= start) {
    return std::string();
  }
  return line.substr(start + 1, end - start - 1);
}

bool parse_cmap_content(const std::string& content, CMap* cmap) {
  if (!cmap) {
    return false;
  }
  std::istringstream input(content);
  auto read_data_line = [&](std::string* out_line) -> bool {
    std::string raw;
    while (std::getline(input, raw)) {
      std::string trimmed = trim_cmap_line(raw);
      if (trimmed.empty()) {
        continue;
      }
      if (trimmed[0] == '%') {
        continue;
      }
      *out_line = trimmed;
      return true;
    }
    return false;
  };

  std::string line;
  while (read_data_line(&line)) {
    auto tokens = tokenize_cmap_line(line);
    if (tokens.empty()) {
      continue;
    }

    if (tokens.back() == "begincodespacerange") {
      int count = 0;
      if (!parse_integer_token(tokens.front(), &count)) {
        continue;
      }
      int processed = 0;
      while (processed < count) {
        std::string entry_line;
        if (!read_data_line(&entry_line)) {
          break;
        }
        auto entry_tokens = tokenize_cmap_line(entry_line);
        if (entry_tokens.size() < 2) {
          continue;
        }
        processed++;
      }
      continue;
    }

    if (tokens.back() == "beginbfchar") {
      int count = 0;
      if (!parse_integer_token(tokens.front(), &count)) {
        continue;
      }
      int processed = 0;
      while (processed < count) {
        std::string entry_line;
        if (!read_data_line(&entry_line)) {
          break;
        }
        auto entry_tokens = tokenize_cmap_line(entry_line);
        if (entry_tokens.size() < 2) {
          continue;
        }
        uint32_t src = 0;
        if (!parse_hex_string_token(entry_tokens[0], &src)) {
          continue;
        }
        if (entry_tokens[1].front() == '<') {
          uint32_t dst = 0;
          if (parse_hex_string_token(entry_tokens[1], &dst)) {
            cmap->code_to_unicode[src] = dst;
          }
        }
        processed++;
      }
      continue;
    }

    if (tokens.back() == "beginbfrange") {
      int count = 0;
      if (!parse_integer_token(tokens.front(), &count)) {
        continue;
      }
      int processed = 0;
      while (processed < count) {
        std::string entry_line;
        if (!read_data_line(&entry_line)) {
          break;
        }
        auto entry_tokens = tokenize_cmap_line(entry_line);
        if (entry_tokens.size() < 3) {
          continue;
        }
        uint32_t start_code = 0;
        uint32_t end_code = 0;
        if (!parse_hex_string_token(entry_tokens[0], &start_code) ||
            !parse_hex_string_token(entry_tokens[1], &end_code)) {
          continue;
        }
        if (entry_tokens[2].front() == '[') {
          auto values = parse_hex_array_token(entry_tokens[2]);
          for (size_t idx = 0; idx < values.size(); ++idx) {
            cmap->code_to_unicode[start_code + static_cast<uint32_t>(idx)] =
                values[idx];
          }
        } else if (entry_tokens[2].front() == '<') {
          uint32_t dst = 0;
          if (parse_hex_string_token(entry_tokens[2], &dst)) {
            for (uint32_t code = start_code; code <= end_code; ++code) {
              cmap->code_to_unicode[code] = dst + (code - start_code);
            }
          }
        }
        processed++;
      }
      continue;
    }

    if (tokens.back() == "begincidchar") {
      int count = 0;
      if (!parse_integer_token(tokens.front(), &count)) {
        continue;
      }
      int processed = 0;
      while (processed < count) {
        std::string entry_line;
        if (!read_data_line(&entry_line)) {
          break;
        }
        auto entry_tokens = tokenize_cmap_line(entry_line);
        if (entry_tokens.size() < 2) {
          continue;
        }
        uint32_t src = 0;
        if (!parse_hex_string_token(entry_tokens[0], &src)) {
          continue;
        }
        uint32_t dst = 0;
        if (entry_tokens[1].front() == '<') {
          if (parse_hex_string_token(entry_tokens[1], &dst)) {
            cmap->code_to_unicode[src] = dst;
          }
        } else if (parse_literal_number(entry_tokens[1], &dst)) {
          cmap->code_to_unicode[src] = dst;
        }
        processed++;
      }
      continue;
    }

    if (tokens.back() == "begincidrange") {
      int count = 0;
      if (!parse_integer_token(tokens.front(), &count)) {
        continue;
      }
      int processed = 0;
      while (processed < count) {
        std::string entry_line;
        if (!read_data_line(&entry_line)) {
          break;
        }
        auto entry_tokens = tokenize_cmap_line(entry_line);
        if (entry_tokens.size() < 3) {
          continue;
        }
        uint32_t start_code = 0;
        uint32_t end_code = 0;
        if (!parse_hex_string_token(entry_tokens[0], &start_code) ||
            !parse_hex_string_token(entry_tokens[1], &end_code)) {
          continue;
        }
        uint32_t dst = 0;
        if (entry_tokens[2].front() == '<') {
          if (parse_hex_string_token(entry_tokens[2], &dst)) {
            for (uint32_t code = start_code; code <= end_code; ++code) {
              cmap->code_to_unicode[code] = dst + (code - start_code);
            }
          }
        } else if (parse_literal_number(entry_tokens[2], &dst)) {
          for (uint32_t code = start_code; code <= end_code; ++code) {
            cmap->code_to_unicode[code] = dst + (code - start_code);
          }
        }
        processed++;
      }
      continue;
    }

    if (tokens.size() >= 2 && tokens[0] == "/CMapName") {
      const std::string& name_token = tokens[1];
      if (!name_token.empty() && name_token.front() == '/') {
        cmap->name = name_token.substr(1);
      }
      continue;
    }

    if (line.find("/Registry") != std::string::npos) {
      std::string value = extract_parenthesized_value(line, "/Registry");
      if (!value.empty()) {
        cmap->registry = value;
      }
      continue;
    }

    if (line.find("/Ordering") != std::string::npos) {
      std::string value = extract_parenthesized_value(line, "/Ordering");
      if (!value.empty()) {
        cmap->ordering = value;
      }
      continue;
    }

    if (line.find("/Supplement") != std::string::npos) {
      auto supp_tokens = tokenize_cmap_line(line);
      for (size_t idx = 0; idx + 1 < supp_tokens.size(); ++idx) {
        if (supp_tokens[idx] == "/Supplement") {
          int supp = 0;
          if (parse_integer_token(supp_tokens[idx + 1], &supp)) {
            cmap->supplement = supp;
          }
          break;
        }
      }
      continue;
    }
  }
  return true;
}

bool parse_cmap_value(const Pdf& pdf, const Value& value, CMap* cmap) {
  if (!cmap) {
    return false;
  }
  Value resolved;
  if (!resolve_indirect_value(pdf, value, &resolved)) {
    return false;
  }
  if (resolved.type == Value::NAME) {
    cmap->name = resolved.name;
    return true;
  }
  if (resolved.type == Value::STREAM) {
    DecodedStream decoded = decode_stream(pdf, resolved);
    if (!decoded.success) {
      return false;
    }
    std::string content(decoded.data.begin(), decoded.data.end());
    return parse_cmap_content(content, cmap);
  }
  if (resolved.type == Value::DICTIONARY) {
    auto name_it = resolved.dict.find("CMapName");
    if (name_it != resolved.dict.end()) {
      if (name_it->second.type == Value::NAME) {
        cmap->name = name_it->second.name;
      } else if (name_it->second.type == Value::STRING) {
        cmap->name = name_it->second.str;
      }
    }
    auto sysinfo_it = resolved.dict.find("CIDSystemInfo");
    if (sysinfo_it != resolved.dict.end() && sysinfo_it->second.type == Value::DICTIONARY) {
      extract_cid_system_info(sysinfo_it->second.dict, &cmap->registry,
                              &cmap->ordering, &cmap->supplement);
    }
    auto use_it = resolved.dict.find("UseCMap");
    if (use_it != resolved.dict.end()) {
      parse_cmap_value(pdf, use_it->second, cmap);
    }
    return true;
  }
  return false;
}

}  // namespace

// Parse Type0 (CID) font
std::unique_ptr<BaseFont> parse_type0_font(const Pdf& pdf, const Dictionary& font_dict) {
  auto font = std::unique_ptr<Type0Font>(new Type0Font());

  auto base_it = font_dict.find("BaseFont");
  if (base_it != font_dict.end() && base_it->second.type == Value::NAME) {
    font->base_font = base_it->second.name;
  }

  auto encoding_it = font_dict.find("Encoding");
  if (encoding_it != font_dict.end()) {
    parse_cmap_value(pdf, encoding_it->second, &font->encoding_cmap);
  }

  auto tounicode_it = font_dict.find("ToUnicode");
  if (tounicode_it != font_dict.end()) {
    parse_cmap_value(pdf, tounicode_it->second, &font->to_unicode_cmap);
  }

  auto desc_it = font_dict.find("DescendantFonts");
  if (desc_it != font_dict.end() && desc_it->second.type == Value::ARRAY &&
      !desc_it->second.array.empty()) {
    Value descendant_value;
    if (resolve_indirect_value(pdf, desc_it->second.array[0], &descendant_value) &&
        descendant_value.type == Value::DICTIONARY) {
      const Dictionary& cid_font_dict = descendant_value.dict;

      auto descendant = std::unique_ptr<BaseFont>(new BaseFont());
      auto subtype_it = cid_font_dict.find("Subtype");
      if (subtype_it != cid_font_dict.end() && subtype_it->second.type == Value::NAME) {
        descendant->subtype = subtype_it->second.name;
      }

      auto descendant_base_it = cid_font_dict.find("BaseFont");
      if (descendant_base_it != cid_font_dict.end() &&
          descendant_base_it->second.type == Value::NAME) {
        descendant->base_font = descendant_base_it->second.name;
      }

      auto descendant_encoding_it = cid_font_dict.find("Encoding");
      if (descendant_encoding_it != cid_font_dict.end() &&
          descendant_encoding_it->second.type == Value::NAME) {
        descendant->encoding = descendant_encoding_it->second.name;
      }

      auto descriptor_it = cid_font_dict.find("FontDescriptor");
      if (descriptor_it != cid_font_dict.end()) {
        auto descriptor = Pdf::parse_font_descriptor(pdf, descriptor_it->second);
        if (descriptor) {
          descendant->descriptor = descriptor.release();
        }
      }

      auto first_it = cid_font_dict.find("FirstChar");
      if (first_it != cid_font_dict.end() && first_it->second.type == Value::NUMBER) {
        descendant->first_char = static_cast<int>(first_it->second.number);
      }

      auto last_it = cid_font_dict.find("LastChar");
      if (last_it != cid_font_dict.end() && last_it->second.type == Value::NUMBER) {
        descendant->last_char = static_cast<int>(last_it->second.number);
      }

      auto widths_it = cid_font_dict.find("Widths");
      if (widths_it != cid_font_dict.end() && widths_it->second.type == Value::ARRAY) {
        descendant->widths.clear();
        for (const auto& width_val : widths_it->second.array) {
          if (width_val.type == Value::NUMBER) {
            descendant->widths.push_back(static_cast<int>(width_val.number));
          }
        }
      }

      auto cid_info_it = cid_font_dict.find("CIDSystemInfo");
      if (cid_info_it != cid_font_dict.end() && cid_info_it->second.type == Value::DICTIONARY) {
        parse_cid_system_info(cid_info_it->second.dict, font.get());
      }

      auto dw_it = cid_font_dict.find("DW");
      if (dw_it != cid_font_dict.end() && dw_it->second.type == Value::NUMBER) {
        font->default_width = static_cast<int>(dw_it->second.number);
      }

      auto w_it = cid_font_dict.find("W");
      if (w_it != cid_font_dict.end() && w_it->second.type == Value::ARRAY) {
        font->cid_widths.clear();
        parse_cid_width_array(w_it->second.array, font.get());
      }

      auto cid_to_gid_it = cid_font_dict.find("CIDToGIDMap");
      if (cid_to_gid_it != cid_font_dict.end()) {
        parse_cid_to_gid_map(pdf, cid_to_gid_it->second, font.get());
      }

      font->descendant_font = std::move(descendant);
    }
  }

  if (font->registry.empty() && !font->encoding_cmap.registry.empty()) {
    font->registry = font->encoding_cmap.registry;
  }
  if (font->ordering.empty() && !font->encoding_cmap.ordering.empty()) {
    font->ordering = font->encoding_cmap.ordering;
  }
  if (font->supplement == 0 && font->encoding_cmap.supplement != 0) {
    font->supplement = font->encoding_cmap.supplement;
  }

  return font;
}

// Parse Type3 font
std::unique_ptr<BaseFont> parse_type3_font(const Pdf& pdf, const Dictionary& font_dict) {
  auto font = std::unique_ptr<Type3Font>(new Type3Font());

  // Parse font name
  auto name_it = font_dict.find("Name");
  if (name_it != font_dict.end() && name_it->second.type == Value::NAME) {
    font->base_font = name_it->second.name;
  }

  // Parse FontBBox
  auto bbox_it = font_dict.find("FontBBox");
  if (bbox_it != font_dict.end() && bbox_it->second.type == Value::ARRAY) {
    for (const auto& val : bbox_it->second.array) {
      if (val.type == Value::NUMBER) {
        font->font_bbox.push_back(val.number);
      }
    }
  }

  // Parse FontMatrix
  auto matrix_it = font_dict.find("FontMatrix");
  if (matrix_it != font_dict.end() && matrix_it->second.type == Value::ARRAY) {
    font->font_matrix.clear();
    for (const auto& val : matrix_it->second.array) {
      if (val.type == Value::NUMBER) {
        font->font_matrix.push_back(val.number);
      }
    }
  }

  // Parse CharProcs dictionary
  auto procs_it = font_dict.find("CharProcs");
  if (procs_it != font_dict.end() && procs_it->second.type == Value::DICTIONARY) {
    font->char_procs = procs_it->second.dict;
  }

  // Parse Resources
  auto res_it = font_dict.find("Resources");
  if (res_it != font_dict.end() && res_it->second.type == Value::DICTIONARY) {
    font->resources = res_it->second.dict;
  }

  // Parse Widths
  auto widths_it = font_dict.find("Widths");
  if (widths_it != font_dict.end() && widths_it->second.type == Value::ARRAY) {
    for (const auto& val : widths_it->second.array) {
      if (val.type == Value::NUMBER) {
        font->widths.push_back(static_cast<int>(val.number));
      }
    }
  }

  // Parse FirstChar and LastChar
  auto first_it = font_dict.find("FirstChar");
  if (first_it != font_dict.end() && first_it->second.type == Value::NUMBER) {
    font->first_char = static_cast<int>(first_it->second.number);
  }

  auto last_it = font_dict.find("LastChar");
  if (last_it != font_dict.end() && last_it->second.type == Value::NUMBER) {
    font->last_char = static_cast<int>(last_it->second.number);
  }

  return font;
}

// Font substitution manager
class FontSubstitutionManager {
public:
  static FontSubstitutionManager& instance() {
    static FontSubstitutionManager instance;
    return instance;
  }

  void add_substitution(const std::string& original, const std::string& substitute) {
    FontSubstitution sub;
    sub.original_name = original;
    sub.substitute_name = substitute;
    sub.substitute_path = "";
    sub.is_system_font = false;
    substitutions_[original] = sub;
  }

  void add_standard_14_fonts() {
    // PDF standard 14 fonts
    add_substitution("Times-Roman", "serif");
    add_substitution("Times-Bold", "serif bold");
    add_substitution("Times-Italic", "serif italic");
    add_substitution("Times-BoldItalic", "serif bold italic");
    add_substitution("Helvetica", "sans-serif");
    add_substitution("Helvetica-Bold", "sans-serif bold");
    add_substitution("Helvetica-Oblique", "sans-serif italic");
    add_substitution("Helvetica-BoldOblique", "sans-serif bold italic");
    add_substitution("Courier", "monospace");
    add_substitution("Courier-Bold", "monospace bold");
    add_substitution("Courier-Oblique", "monospace italic");
    add_substitution("Courier-BoldOblique", "monospace bold italic");
    add_substitution("Symbol", "Symbol");
    add_substitution("ZapfDingbats", "ZapfDingbats");
  }

  FontSubstitution find_substitute(const std::string& font_name) {
    auto it = substitutions_.find(font_name);
    if (it != substitutions_.end()) {
      return it->second;
    }

    // Try partial match
    for (const auto& pair : substitutions_) {
      if (font_name.find(pair.first) != std::string::npos) {
        return pair.second;
      }
    }

    // Default fallback
    FontSubstitution fallback;
    fallback.original_name = font_name;
    fallback.substitute_name = "sans-serif";
    fallback.substitute_path = "";
    fallback.is_system_font = false;
    return fallback;
  }

private:
  FontSubstitutionManager() {
    add_standard_14_fonts();
  }

  std::map<std::string, FontSubstitution> substitutions_;
};

// Parse font descriptor
std::unique_ptr<FontDescriptor> Pdf::parse_font_descriptor(const Pdf& pdf, const Value& desc_val) {
  // Resolve reference if needed
  Value resolved_val = desc_val;
  if (desc_val.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, desc_val.ref_object_number, desc_val.ref_generation_number);
    if (!resolved.success) {
      return nullptr;
    }
    resolved_val = resolved.value;
  }

  if (resolved_val.type != Value::DICTIONARY) {
    return nullptr;
  }

  auto descriptor = std::unique_ptr<FontDescriptor>(new FontDescriptor());
  const Dictionary& dict = resolved_val.dict;

  // Parse font name
  auto name_it = dict.find("FontName");
  if (name_it != dict.end() && name_it->second.type == Value::NAME) {
    descriptor->font_name = name_it->second.name;
  }

  // Parse font family
  auto family_it = dict.find("FontFamily");
  if (family_it != dict.end() && family_it->second.type == Value::STRING) {
    descriptor->font_family = family_it->second.str;
  }

  // Parse metrics
  auto ascent_it = dict.find("Ascent");
  if (ascent_it != dict.end() && ascent_it->second.type == Value::NUMBER) {
    descriptor->ascent = ascent_it->second.number;
  }

  auto descent_it = dict.find("Descent");
  if (descent_it != dict.end() && descent_it->second.type == Value::NUMBER) {
    descriptor->descent = descent_it->second.number;
  }

  auto capheight_it = dict.find("CapHeight");
  if (capheight_it != dict.end() && capheight_it->second.type == Value::NUMBER) {
    descriptor->cap_height = capheight_it->second.number;
  }

  auto flags_it = dict.find("Flags");
  if (flags_it != dict.end() && flags_it->second.type == Value::NUMBER) {
    descriptor->flags = static_cast<int>(flags_it->second.number);
  }

  // Parse FontBBox
  auto bbox_it = dict.find("FontBBox");
  if (bbox_it != dict.end() && bbox_it->second.type == Value::ARRAY) {
    for (const auto& val : bbox_it->second.array) {
      if (val.type == Value::NUMBER) {
        descriptor->font_bbox.push_back(val.number);
      }
    }
  }

  // Parse embedded font file (FontFile, FontFile2, FontFile3)
  // FontFile = Type1 font program
  // FontFile2 = TrueType font program
  // FontFile3 = CFF font program (Type1C, CIDFontType0C, OpenType)
  auto fontfile_it = dict.find("FontFile2");  // TrueType
  if (fontfile_it == dict.end()) {
    fontfile_it = dict.find("FontFile3");  // CFF/OpenType
  }
  if (fontfile_it == dict.end()) {
    fontfile_it = dict.find("FontFile");  // Type1
  }
  if (fontfile_it != dict.end()) {
    // Store the reference/stream for later decoding
    descriptor->font_file = fontfile_it->second;
  }

  return descriptor;
}

// Extended font parsing to include Type0 and Type3
std::unique_ptr<BaseFont> Pdf::parse_font(const Pdf& pdf, const Value& font_val) {
  if (font_val.type != Value::DICTIONARY) {
    return nullptr;
  }

  const Dictionary& font_dict = font_val.dict;

  // Check font subtype
  auto subtype_it = font_dict.find("Subtype");
  if (subtype_it == font_dict.end() || subtype_it->second.type != Value::NAME) {
    return nullptr;
  }

  const std::string& subtype = subtype_it->second.name;

  if (subtype == "Type0") {
    return parse_type0_font(pdf, font_dict);
  } else if (subtype == "Type3") {
    return parse_type3_font(pdf, font_dict);
  } else {
    // Use existing parsing for Type1, TrueType, etc.
    auto font = std::unique_ptr<BaseFont>(new BaseFont());
    font->subtype = subtype;

    // Parse BaseFont
    auto base_it = font_dict.find("BaseFont");
    if (base_it != font_dict.end() && base_it->second.type == Value::NAME) {
      font->base_font = base_it->second.name;
    }

    // Parse Encoding
    auto enc_it = font_dict.find("Encoding");
    if (enc_it != font_dict.end()) {
      parse_simple_font_encoding(pdf, enc_it->second, font.get());
    }

    // Parse FontDescriptor
    auto desc_it = font_dict.find("FontDescriptor");
    if (desc_it != font_dict.end()) {
      font->descriptor = parse_font_descriptor(pdf, desc_it->second).release();
    }

    auto tounicode_it = font_dict.find("ToUnicode");
    if (tounicode_it != font_dict.end()) {
      parse_cmap_value(pdf, tounicode_it->second, &font->to_unicode_cmap);
    }

    return font;
  }
}

// Public text extraction function
std::string extract_text_from_page(const Pdf& pdf, const Page& page) {
  TextExtractor extractor(pdf, page);
  return extractor.extract_text();
}

// Annotation parsing implementation
std::unique_ptr<Annotation> parse_annotation(const Pdf& pdf, const Value& annot_val) {
  if (annot_val.type != Value::DICTIONARY) {
    return nullptr;
  }

  const Dictionary& dict = annot_val.dict;

  // Get annotation type
  auto type_it = dict.find("Subtype");
  if (type_it == dict.end() || type_it->second.type != Value::NAME) {
    return nullptr;
  }

  const std::string& type_name = type_it->second.name;
  std::unique_ptr<Annotation> annot;

  // Create appropriate annotation type
  if (type_name == "Text") {
    auto text_annot = std::unique_ptr<TextAnnotation>(new TextAnnotation());

    // Parse text-specific properties
    auto icon_it = dict.find("Name");
    if (icon_it != dict.end() && icon_it->second.type == Value::NAME) {
      text_annot->icon = icon_it->second.name;
    }

    auto open_it = dict.find("Open");
    if (open_it != dict.end() && open_it->second.type == Value::BOOLEAN) {
      text_annot->open = open_it->second.boolean;
    }

    auto state_it = dict.find("State");
    if (state_it != dict.end() && state_it->second.type == Value::NAME) {
      text_annot->state = state_it->second.name;
    }

    annot = std::move(text_annot);
  } else if (type_name == "Link") {
    auto link_annot = std::unique_ptr<LinkAnnotation>(new LinkAnnotation());

    // Parse action
    auto action_it = dict.find("A");
    if (action_it != dict.end() && action_it->second.type == Value::DICTIONARY) {
      const Dictionary& action_dict = action_it->second.dict;
      auto s_it = action_dict.find("S");
      if (s_it != action_dict.end() && s_it->second.type == Value::NAME) {
        const std::string& action_type = s_it->second.name;
        if (action_type == "URI") {
          link_annot->action_type = LinkAnnotation::URI;
          auto uri_it = action_dict.find("URI");
          if (uri_it != action_dict.end() && uri_it->second.type == Value::STRING) {
            link_annot->uri = uri_it->second.str;
          }
        } else if (action_type == "GoTo") {
          link_annot->action_type = LinkAnnotation::GoTo;
          auto dest_it = action_dict.find("D");
          if (dest_it != action_dict.end()) {
            link_annot->destination = dest_it->second.dict;
          }
        }
      }
    }

    annot = std::move(link_annot);
  } else if (type_name == "Highlight" || type_name == "Underline" ||
             type_name == "Squiggly" || type_name == "StrikeOut") {
    auto markup = std::unique_ptr<MarkupAnnotation>(new MarkupAnnotation(
      type_name == "Highlight" ? AnnotationType::Highlight :
      type_name == "Underline" ? AnnotationType::Underline :
      type_name == "Squiggly" ? AnnotationType::Squiggly :
      AnnotationType::StrikeOut
    ));

    // Parse quad points
    auto quad_it = dict.find("QuadPoints");
    if (quad_it != dict.end() && quad_it->second.type == Value::ARRAY) {
      std::vector<double> points;
      for (const auto& val : quad_it->second.array) {
        if (val.type == Value::NUMBER) {
          points.push_back(val.number);
        }
        if (points.size() == 8) {
          markup->quad_points.push_back(points);
          points.clear();
        }
      }
    }

    auto opacity_it = dict.find("CA");
    if (opacity_it != dict.end() && opacity_it->second.type == Value::NUMBER) {
      markup->opacity = opacity_it->second.number;
    }

    annot = std::move(markup);
  } else if (type_name == "FreeText") {
    auto freetext = std::unique_ptr<FreeTextAnnotation>(new FreeTextAnnotation());

    auto da_it = dict.find("DA");
    if (da_it != dict.end() && da_it->second.type == Value::STRING) {
      freetext->default_appearance = da_it->second.str;
    }

    auto q_it = dict.find("Q");
    if (q_it != dict.end() && q_it->second.type == Value::NUMBER) {
      freetext->quadding = static_cast<int>(q_it->second.number);
    }

    annot = std::move(freetext);
  } else if (type_name == "Widget") {
    auto widget = std::unique_ptr<WidgetAnnotation>(new WidgetAnnotation());

    // Parse field type and properties
    auto ft_it = dict.find("FT");
    if (ft_it != dict.end() && ft_it->second.type == Value::NAME) {
      const std::string& field_type = ft_it->second.name;
      if (field_type == "Tx") {
        widget->field_type = FieldType::Text;
      } else if (field_type == "Btn") {
        widget->field_type = FieldType::Button;
      } else if (field_type == "Ch") {
        widget->field_type = FieldType::Choice;
      } else if (field_type == "Sig") {
        widget->field_type = FieldType::Signature;
      }
    }

    auto t_it = dict.find("T");
    if (t_it != dict.end() && t_it->second.type == Value::STRING) {
      widget->field_name = t_it->second.str;
    }

    auto v_it = dict.find("V");
    if (v_it != dict.end()) {
      if (v_it->second.type == Value::STRING) {
        widget->field_value = v_it->second.str;
      }
    }

    auto dv_it = dict.find("DV");
    if (dv_it != dict.end() && dv_it->second.type == Value::STRING) {
      widget->default_value = dv_it->second.str;
    }

    auto ff_it = dict.find("Ff");
    if (ff_it != dict.end() && ff_it->second.type == Value::NUMBER) {
      widget->field_flags = static_cast<uint32_t>(ff_it->second.number);
    }

    annot = std::move(widget);
  } else {
    // Generic annotation for unsupported types
    annot = std::unique_ptr<Annotation>(new Annotation());
    annot->type = AnnotationType::Text;  // Default
  }

  // Parse common properties
  auto rect_it = dict.find("Rect");
  if (rect_it != dict.end() && rect_it->second.type == Value::ARRAY) {
    for (const auto& val : rect_it->second.array) {
      if (val.type == Value::NUMBER) {
        annot->rect.push_back(val.number);
      }
    }
  }

  auto contents_it = dict.find("Contents");
  if (contents_it != dict.end() && contents_it->second.type == Value::STRING) {
    annot->contents = contents_it->second.str;
  }

  auto nm_it = dict.find("NM");
  if (nm_it != dict.end() && nm_it->second.type == Value::STRING) {
    annot->name = nm_it->second.str;
  }

  auto f_it = dict.find("F");
  if (f_it != dict.end() && f_it->second.type == Value::NUMBER) {
    annot->flags = static_cast<uint32_t>(f_it->second.number);
  }

  auto c_it = dict.find("C");
  if (c_it != dict.end() && c_it->second.type == Value::ARRAY) {
    for (const auto& val : c_it->second.array) {
      if (val.type == Value::NUMBER) {
        annot->color.push_back(val.number);
      }
    }
  }

  // Parse border
  auto border_it = dict.find("Border");
  if (border_it != dict.end() && border_it->second.type == Value::ARRAY) {
    const auto& border_array = border_it->second.array;
    if (border_array.size() >= 3) {
      if (border_array[2].type == Value::NUMBER) {
        annot->border.width = border_array[2].number;
      }
      if (border_array.size() > 3 && border_array[3].type == Value::ARRAY) {
        for (const auto& val : border_array[3].array) {
          if (val.type == Value::NUMBER) {
            annot->border.dash_pattern.push_back(val.number);
          }
        }
      }
    }
  }

  return annot;
}

// Parse annotations for a page
void parse_page_annotations(const Pdf& pdf, Page& page, const Dictionary& page_dict) {
  auto annots_it = page_dict.find("Annots");
  if (annots_it == page_dict.end() || annots_it->second.type != Value::ARRAY) {
    return;
  }

  for (const auto& annot_ref : annots_it->second.array) {
    Value annot_val;
    if (annot_ref.type == Value::REFERENCE) {
      ResolvedObject resolved = resolve_reference(pdf,
        annot_ref.ref_object_number,
        annot_ref.ref_generation_number);
      if (resolved.success) {
        annot_val = std::move(resolved.value);
      }
    } else {
      annot_val = annot_ref;
    }

    if (annot_val.type == Value::DICTIONARY) {
      auto annotation = parse_annotation(pdf, annot_val);
      if (annotation) {
        annotation->page_ref = page.object_number;
        page.annotations.push_back(std::move(annotation));
      }
    }
  }
}

// Form field parsing
std::unique_ptr<FormField> parse_form_field(const Pdf& pdf, const Dictionary& field_dict) {
  // Get field type
  auto ft_it = field_dict.find("FT");
  if (ft_it == field_dict.end() || ft_it->second.type != Value::NAME) {
    return nullptr;
  }

  const std::string& field_type = ft_it->second.name;
  std::unique_ptr<FormField> field;

  if (field_type == "Tx") {
    auto text_field = std::unique_ptr<TextField>(new TextField());

    auto maxlen_it = field_dict.find("MaxLen");
    if (maxlen_it != field_dict.end() && maxlen_it->second.type == Value::NUMBER) {
      text_field->max_length = static_cast<int>(maxlen_it->second.number);
    }

    auto da_it = field_dict.find("DA");
    if (da_it != field_dict.end() && da_it->second.type == Value::STRING) {
      text_field->default_appearance = da_it->second.str;
    }

    auto q_it = field_dict.find("Q");
    if (q_it != field_dict.end() && q_it->second.type == Value::NUMBER) {
      text_field->quadding = static_cast<int>(q_it->second.number);
    }

    field = std::move(text_field);
  } else if (field_type == "Btn") {
    auto button_field = std::unique_ptr<ButtonField>(new ButtonField());

    auto ff_it = field_dict.find("Ff");
    uint32_t flags = 0;
    if (ff_it != field_dict.end() && ff_it->second.type == Value::NUMBER) {
      flags = static_cast<uint32_t>(ff_it->second.number);
    }

    // Determine button type based on flags
    if (flags & static_cast<uint32_t>(FormFieldFlags::Pushbutton)) {
      button_field->button_type = ButtonField::PushButton;
    } else if (flags & static_cast<uint32_t>(FormFieldFlags::Radio)) {
      button_field->button_type = ButtonField::RadioButton;
    } else {
      button_field->button_type = ButtonField::CheckBox;
    }

    field = std::move(button_field);
  } else if (field_type == "Ch") {
    auto choice_field = std::unique_ptr<ChoiceField>(new ChoiceField());

    auto opt_it = field_dict.find("Opt");
    if (opt_it != field_dict.end() && opt_it->second.type == Value::ARRAY) {
      for (const auto& opt : opt_it->second.array) {
        if (opt.type == Value::STRING) {
          choice_field->options.push_back(opt.str);
        } else if (opt.type == Value::ARRAY && opt.array.size() >= 2) {
          // Export value and display text
          if (opt.array[1].type == Value::STRING) {
            choice_field->options.push_back(opt.array[1].str);
          }
        }
      }
    }

    field = std::move(choice_field);
  } else if (field_type == "Sig") {
    field = std::unique_ptr<FormField>(new FormField(FieldType::Signature));
  } else {
    return nullptr;
  }

  // Parse common field properties
  auto t_it = field_dict.find("T");
  if (t_it != field_dict.end() && t_it->second.type == Value::STRING) {
    field->partial_name = t_it->second.str;
    field->full_name = field->partial_name;  // Will be updated with parent hierarchy
  }

  auto tu_it = field_dict.find("TU");
  if (tu_it != field_dict.end() && tu_it->second.type == Value::STRING) {
    field->alternate_name = tu_it->second.str;
  }

  auto tm_it = field_dict.find("TM");
  if (tm_it != field_dict.end() && tm_it->second.type == Value::STRING) {
    field->mapping_name = tm_it->second.str;
  }

  auto ff_it = field_dict.find("Ff");
  if (ff_it != field_dict.end() && ff_it->second.type == Value::NUMBER) {
    field->flags = static_cast<uint32_t>(ff_it->second.number);
  }

  auto v_it = field_dict.find("V");
  if (v_it != field_dict.end()) {
    field->field_value = v_it->second;
  }

  auto dv_it = field_dict.find("DV");
  if (dv_it != field_dict.end()) {
    field->default_value = dv_it->second;
  }

  return field;
}

// Parse AcroForm to extract all form fields
void parse_acro_form(const Pdf& pdf, DocumentCatalog& catalog) {
  if (catalog.acro_form.empty()) {
    return;
  }

  auto fields_it = catalog.acro_form.find("Fields");
  if (fields_it == catalog.acro_form.end() || fields_it->second.type != Value::ARRAY) {
    return;
  }

  for (const auto& field_ref : fields_it->second.array) {
    Value field_val;
    if (field_ref.type == Value::REFERENCE) {
      ResolvedObject resolved = resolve_reference(pdf,
        field_ref.ref_object_number,
        field_ref.ref_generation_number);
      if (resolved.success) {
        field_val = std::move(resolved.value);
      }
    } else {
      field_val = field_ref;
    }

    if (field_val.type == Value::DICTIONARY) {
      auto field = parse_form_field(pdf, field_val.dict);
      if (field) {
        catalog.form_fields.push_back(std::move(field));
      }
    }
  }
}

// Generate appearance stream for annotation
std::string generate_annotation_appearance(const Annotation& annot) {
  std::stringstream ss;

  if (annot.type == AnnotationType::Text) {
    // Simple text annotation appearance
    ss << "q\n";
    ss << "BT\n";
    ss << "/Helv 12 Tf\n";
    ss << "0 0 Td\n";
    ss << "(" << annot.contents << ") Tj\n";
    ss << "ET\n";
    ss << "Q\n";
  } else if (annot.type == AnnotationType::Highlight) {
    // Highlight appearance
    ss << "q\n";
    if (annot.color.size() >= 3) {
      ss << annot.color[0] << " " << annot.color[1] << " " << annot.color[2] << " rg\n";
    } else {
      ss << "1 1 0 rg\n";  // Default yellow
    }

    const MarkupAnnotation* markup = static_cast<const MarkupAnnotation*>(&annot);
    if (markup && !markup->quad_points.empty()) {
      for (const auto& quad : markup->quad_points) {
        if (quad.size() >= 8) {
          ss << quad[0] << " " << quad[1] << " m\n";
          ss << quad[2] << " " << quad[3] << " l\n";
          ss << quad[4] << " " << quad[5] << " l\n";
          ss << quad[6] << " " << quad[7] << " l\n";
          ss << "h\n";
          ss << "f\n";
        }
      }
    }
    ss << "Q\n";
  }

  return ss.str();
}

// Phase 4: Document Structure Implementation

// Parse a single outline item (bookmark) and its children
std::unique_ptr<OutlineItem> parse_outline_item(const Pdf& pdf, const Dictionary& outline_dict) {
  auto item = std::unique_ptr<OutlineItem>(new OutlineItem());

  // Get title
  auto title_it = outline_dict.find("Title");
  if (title_it != outline_dict.end()) {
    if (title_it->second.type == Value::STRING) {
      item->title = title_it->second.str;
    }
  }

  // Get destination or action
  auto dest_it = outline_dict.find("Dest");
  auto action_it = outline_dict.find("A");

  if (dest_it != outline_dict.end()) {
    // Direct destination
    item->action_type = OutlineAction::GoTo;

    if (dest_it->second.type == Value::ARRAY) {
      const auto& arr = dest_it->second.array;
      if (!arr.empty()) {
        // First element is page reference
        if (arr[0].type == Value::REFERENCE) {
          // Would need to look up page number from object reference
          // For now, use object number as approximation
          item->dest_page = arr[0].ref_object_number;
        }

        // Rest are position parameters
        for (size_t i = 1; i < arr.size(); i++) {
          if (arr[i].type == Value::NUMBER) {
            item->dest_position.push_back(arr[i].number);
          }
        }
      }
    }
  } else if (action_it != outline_dict.end()) {
    // Action dictionary
    if (action_it->second.type == Value::DICTIONARY) {
      const auto& action_dict = action_it->second.dict;
      auto s_it = action_dict.find("S");
      if (s_it != action_dict.end()) {
        if (s_it->second.type == Value::NAME) {
          const std::string& action_name = s_it->second.name;
          if (action_name == "URI") {
            item->action_type = OutlineAction::URI;
            auto uri_it = action_dict.find("URI");
            if (uri_it != action_dict.end()) {
              if (uri_it->second.type == Value::STRING) {
                item->uri = uri_it->second.str;
              }
            }
          } else if (action_name == "GoToR") {
            item->action_type = OutlineAction::GoToR;
            auto f_it = action_dict.find("F");
            if (f_it != action_dict.end()) {
              if (f_it->second.type == Value::STRING) {
                item->file = f_it->second.str;
              }
            }
          } else if (action_name == "Launch") {
            item->action_type = OutlineAction::Launch;
            auto f_it = action_dict.find("F");
            if (f_it != action_dict.end()) {
              if (f_it->second.type == Value::STRING) {
                item->file = f_it->second.str;
              }
            }
          }
        }
      }
    }
  }

  // Get appearance
  auto c_it = outline_dict.find("C");
  if (c_it != outline_dict.end()) {
    if (c_it->second.type == Value::ARRAY) {
      for (const auto& v : c_it->second.array) {
        if (v.type == Value::NUMBER) {
          item->color.push_back(v.number);
        }
      }
    }
  }

  auto f_it = outline_dict.find("F");
  if (f_it != outline_dict.end()) {
    if (f_it->second.type == Value::NUMBER) {
      int flags = static_cast<int>(f_it->second.number);
      item->italic = (flags & 0x01) != 0;
      item->bold = (flags & 0x02) != 0;
    }
  }

  // Get count and open state
  auto count_it = outline_dict.find("Count");
  if (count_it != outline_dict.end()) {
    if (count_it->second.type == Value::NUMBER) {
      item->count = static_cast<int>(count_it->second.number);
      item->open = item->count >= 0;
      if (item->count < 0) {
        item->count = -item->count;
      }
    }
  }

  // Parse children recursively
  auto first_it = outline_dict.find("First");
  if (first_it != outline_dict.end()) {
    if (first_it->second.type == Value::REFERENCE) {
      auto first_obj = resolve_reference(pdf, first_it->second.ref_object_number,
                                         first_it->second.ref_generation_number);
      if (first_obj.success && first_obj.value.type == Value::DICTIONARY) {
        Dictionary curr_dict = first_obj.value.dict;
        bool has_more = true;

        // Process all siblings
        while (has_more) {
          auto child = parse_outline_item(pdf, curr_dict);
          if (child) {
            item->children.push_back(std::move(child));
          }

          // Get next sibling
          auto next_it = curr_dict.find("Next");
          if (next_it != curr_dict.end() && next_it->second.type == Value::REFERENCE) {
            auto next_obj = resolve_reference(pdf, next_it->second.ref_object_number,
                                             next_it->second.ref_generation_number);
            if (next_obj.success && next_obj.value.type == Value::DICTIONARY) {
              curr_dict = next_obj.value.dict;
            } else {
              has_more = false;
            }
          } else {
            has_more = false;
          }
        }
      }
    }
  }

  return item;
}

// Parse document outline (bookmarks)
void parse_document_outline(const Pdf& pdf, DocumentCatalog& catalog) {
  if (catalog.outlines.empty()) {
    return;
  }

  auto type_it = catalog.outlines.find("Type");
  if (type_it != catalog.outlines.end()) {
    if (type_it->second.type == Value::NAME) {
      if (type_it->second.name != "Outlines") {
        return;
      }
    }
  }

  // Get first outline item
  auto first_it = catalog.outlines.find("First");
  if (first_it != catalog.outlines.end()) {
    if (first_it->second.type == Value::REFERENCE) {
      auto first_obj = resolve_reference(pdf, first_it->second.ref_object_number,
                                         first_it->second.ref_generation_number);
      if (first_obj.success && first_obj.value.type == Value::DICTIONARY) {
        catalog.outline_root = parse_outline_item(pdf, first_obj.value.dict);
      }
    }
  }
}

// Get label string for a page
std::string PageLabels::get_label(uint32_t page_index) const {
  std::string label;

  // Find the appropriate label definition
  PageLabel current_label;
  uint32_t label_start = 0;

  for (const auto& entry : labels) {
    if (entry.first <= page_index) {
      current_label = entry.second;
      label_start = entry.first;
    } else {
      break;
    }
  }

  // Apply prefix
  label = current_label.prefix;

  // Calculate number
  uint32_t number = page_index - label_start + current_label.start_value;

  // Apply style
  switch (current_label.style) {
    case PageLabelStyle::None:
      // No numbering
      break;

    case PageLabelStyle::DecimalArabic:
      label += std::to_string(number);
      break;

    case PageLabelStyle::UppercaseRoman: {
      // Simple Roman numeral conversion (up to 3999)
      const char* thousands[] = {"", "M", "MM", "MMM"};
      const char* hundreds[] = {"", "C", "CC", "CCC", "CD", "D", "DC", "DCC", "DCCC", "CM"};
      const char* tens[] = {"", "X", "XX", "XXX", "XL", "L", "LX", "LXX", "LXXX", "XC"};
      const char* ones[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX"};

      if (number <= 3999) {
        label += thousands[number / 1000];
        label += hundreds[(number % 1000) / 100];
        label += tens[(number % 100) / 10];
        label += ones[number % 10];
      }
      break;
    }

    case PageLabelStyle::LowercaseRoman: {
      // Same as uppercase but convert to lowercase
      std::string upper_label = label;
      // Calculate uppercase roman
      const char* thousands[] = {"", "m", "mm", "mmm"};
      const char* hundreds[] = {"", "c", "cc", "ccc", "cd", "d", "dc", "dcc", "dccc", "cm"};
      const char* tens[] = {"", "x", "xx", "xxx", "xl", "l", "lx", "lxx", "lxxx", "xc"};
      const char* ones[] = {"", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix"};

      if (number <= 3999) {
        label += thousands[number / 1000];
        label += hundreds[(number % 1000) / 100];
        label += tens[(number % 100) / 10];
        label += ones[number % 10];
      }
      break;
    }

    case PageLabelStyle::UppercaseLetters:
      if (number > 0 && number <= 26) {
        label += char('A' + number - 1);
      } else if (number > 26) {
        // AA, AB, etc.
        int n = number - 1;
        std::string letters;
        while (n >= 0) {
          letters = char('A' + (n % 26)) + letters;
          n = n / 26 - 1;
        }
        label += letters;
      }
      break;

    case PageLabelStyle::LowercaseLetters:
      if (number > 0 && number <= 26) {
        label += char('a' + number - 1);
      } else if (number > 26) {
        // aa, ab, etc.
        int n = number - 1;
        std::string letters;
        while (n >= 0) {
          letters = char('a' + (n % 26)) + letters;
          n = n / 26 - 1;
        }
        label += letters;
      }
      break;
  }

  return label;
}

// Parse page labels
void parse_page_labels(const Pdf& pdf, DocumentCatalog& catalog) {
  // Page labels are in the Names dictionary under PageLabels
  auto page_labels_it = catalog.names.find("PageLabels");
  if (page_labels_it == catalog.names.end()) {
    return;
  }

  if (page_labels_it->second.type == Value::REFERENCE) {
    auto labels_obj = resolve_reference(pdf, page_labels_it->second.ref_object_number,
                                        page_labels_it->second.ref_generation_number);
    if (!labels_obj.success) {
      return;
    }

    if (labels_obj.value.type == Value::DICTIONARY) {
      const auto& labels_dict = labels_obj.value.dict;
      // Get the number tree
      auto nums_it = labels_dict.find("Nums");
      if (nums_it != labels_dict.end()) {
        if (nums_it->second.type == Value::ARRAY) {
          const auto& nums_array = nums_it->second.array;
          // Process pairs of [page_index, label_dict]
          for (size_t i = 0; i + 1 < nums_array.size(); i += 2) {
            if (nums_array[i].type == Value::NUMBER &&
                nums_array[i + 1].type == Value::DICTIONARY) {
              PageLabel label;
              uint32_t page_index = static_cast<uint32_t>(nums_array[i].number);
              const auto& label_dict = nums_array[i + 1].dict;

              // Parse label style
              auto type_it = label_dict.find("Type");
              if (type_it != label_dict.end()) {
                if (type_it->second.type == Value::NAME) {
                  const std::string& style_name = type_it->second.name;
                  if (style_name == "D") {
                    label.style = PageLabelStyle::DecimalArabic;
                  } else if (style_name == "r") {
                    label.style = PageLabelStyle::LowercaseRoman;
                  } else if (style_name == "R") {
                    label.style = PageLabelStyle::UppercaseRoman;
                  } else if (style_name == "a") {
                    label.style = PageLabelStyle::LowercaseLetters;
                  } else if (style_name == "A") {
                    label.style = PageLabelStyle::UppercaseLetters;
                  } else {
                    label.style = PageLabelStyle::None;
                  }
                }
              }

              // Parse prefix
              auto p_it = label_dict.find("P");
              if (p_it != label_dict.end()) {
                if (p_it->second.type == Value::STRING) {
                  label.prefix = p_it->second.str;
                }
              }

              // Parse start value
              auto st_it = label_dict.find("St");
              if (st_it != label_dict.end()) {
                if (st_it->second.type == Value::NUMBER) {
                  label.start_value = static_cast<uint32_t>(st_it->second.number);
                }
              }

              catalog.page_labels.labels[page_index] = label;
            }
          }
        }
      }
    }
  }
}

// Parse named destinations
void parse_named_destinations(const Pdf& pdf, DocumentCatalog& catalog) {
  // Named destinations are in the Names dictionary under Dests
  auto dests_it = catalog.names.find("Dests");
  if (dests_it == catalog.names.end()) {
    return;
  }

  if (dests_it->second.type == Value::REFERENCE) {
    auto dests_obj = resolve_reference(pdf, dests_it->second.ref_object_number,
                                       dests_it->second.ref_generation_number);
    if (!dests_obj.success) {
      return;
    }

    if (dests_obj.value.type == Value::DICTIONARY) {
      const auto& dests_dict = dests_obj.value.dict;
      // Process name tree
      auto names_it = dests_dict.find("Names");
      if (names_it != dests_dict.end()) {
        if (names_it->second.type == Value::ARRAY) {
          const auto& names_array = names_it->second.array;
          // Process pairs of [name, destination]
          for (size_t i = 0; i + 1 < names_array.size(); i += 2) {
            if (names_array[i].type == Value::STRING) {
              NamedDestination dest;
              dest.name = names_array[i].str;

              // Parse destination
              if (names_array[i + 1].type == Value::ARRAY) {
                const auto& dest_array = names_array[i + 1].array;
                if (!dest_array.empty()) {
                  // First element is page reference
                  if (dest_array[0].type == Value::REFERENCE) {
                    // Would need to look up page number from object reference
                    dest.page_number = dest_array[0].ref_object_number;
                  }

                  // Second element is fit type
                  if (dest_array.size() > 1) {
                    if (dest_array[1].type == Value::NAME) {
                      dest.fit_type = dest_array[1].name;
                    }
                  }

                  // Rest are position parameters
                  for (size_t j = 2; j < dest_array.size(); j++) {
                    if (dest_array[j].type == Value::NUMBER) {
                      dest.position.push_back(dest_array[j].number);
                    }
                  }
                }
              }

              catalog.named_destinations[dest.name] = dest;
            }
          }
        }
      }
    }
  }
}

// Parse document information dictionary
void parse_document_info(const Pdf& pdf, DocumentCatalog& catalog) {
  if (pdf.info == 0) {
    return;
  }

  auto info_obj = resolve_reference(pdf, pdf.info, 0);
  if (!info_obj.success) {
    return;
  }

  if (info_obj.value.type == Value::DICTIONARY) {
    const auto& info_dict = info_obj.value.dict;
    // Standard fields
    auto title_it = info_dict.find("Title");
    if (title_it != info_dict.end()) {
      if (title_it->second.type == Value::STRING) {
        catalog.document_info.title = title_it->second.str;
      }
    }

    auto author_it = info_dict.find("Author");
    if (author_it != info_dict.end()) {
      if (author_it->second.type == Value::STRING) {
        catalog.document_info.author = author_it->second.str;
      }
    }

    auto subject_it = info_dict.find("Subject");
    if (subject_it != info_dict.end()) {
      if (subject_it->second.type == Value::STRING) {
        catalog.document_info.subject = subject_it->second.str;
      }
    }

    auto keywords_it = info_dict.find("Keywords");
    if (keywords_it != info_dict.end()) {
      if (keywords_it->second.type == Value::STRING) {
        catalog.document_info.keywords = keywords_it->second.str;
      }
    }

    auto creator_it = info_dict.find("Creator");
    if (creator_it != info_dict.end()) {
      if (creator_it->second.type == Value::STRING) {
        catalog.document_info.creator = creator_it->second.str;
      }
    }

    auto producer_it = info_dict.find("Producer");
    if (producer_it != info_dict.end()) {
      if (producer_it->second.type == Value::STRING) {
        catalog.document_info.producer = producer_it->second.str;
      }
    }

    auto creation_date_it = info_dict.find("CreationDate");
    if (creation_date_it != info_dict.end()) {
      if (creation_date_it->second.type == Value::STRING) {
        catalog.document_info.creation_date = creation_date_it->second.str;
      }
    }

    auto mod_date_it = info_dict.find("ModDate");
    if (mod_date_it != info_dict.end()) {
      if (mod_date_it->second.type == Value::STRING) {
        catalog.document_info.mod_date = mod_date_it->second.str;
      }
    }

    auto trapped_it = info_dict.find("Trapped");
    if (trapped_it != info_dict.end()) {
      if (trapped_it->second.type == Value::NAME) {
        catalog.document_info.trapped = trapped_it->second.name;
      }
    }

    // Process any custom metadata fields
    for (const auto& entry : info_dict) {
      if (entry.first != "Title" && entry.first != "Author" &&
          entry.first != "Subject" && entry.first != "Keywords" &&
          entry.first != "Creator" && entry.first != "Producer" &&
          entry.first != "CreationDate" && entry.first != "ModDate" &&
          entry.first != "Trapped") {
        if (entry.second.type == Value::STRING) {
          catalog.document_info.custom[entry.first] = entry.second.str;
        }
      }
    }
  }
}

// Simple XMP XML parser
bool XMPMetadata::parse_xml(const std::string& xml) {
  raw_xml = xml;

  // Very basic XML parsing - production code should use a proper XML parser
  // Look for common XMP fields

  // Find dc:title
  size_t pos = xml.find("<dc:title");
  if (pos != std::string::npos) {
    size_t end = xml.find("</dc:title>", pos);
    if (end != std::string::npos) {
      pos = xml.find(">", pos) + 1; // Skip to after the tag
      // Skip any nested tags
      size_t nested = xml.find("<rdf:Alt>", pos);
      if (nested != std::string::npos && nested < end) {
        pos = xml.find("<rdf:li", nested);
        if (pos != std::string::npos && pos < end) {
          pos = xml.find(">", pos) + 1;
          size_t li_end = xml.find("</rdf:li>", pos);
          if (li_end != std::string::npos) {
            dc_title = xml.substr(pos, li_end - pos);
          }
        }
      }
    }
  }

  // Find dc:creator
  pos = xml.find("<dc:creator");
  if (pos != std::string::npos) {
    size_t end = xml.find("</dc:creator>", pos);
    if (end != std::string::npos) {
      pos = xml.find(">", pos) + 1; // Skip to after the tag
      size_t nested = xml.find("<rdf:Seq>", pos);
      if (nested != std::string::npos && nested < end) {
        pos = xml.find("<rdf:li", nested);
        if (pos != std::string::npos && pos < end) {
          pos = xml.find(">", pos) + 1;
          size_t li_end = xml.find("</rdf:li>", pos);
          if (li_end != std::string::npos) {
            dc_creator = xml.substr(pos, li_end - pos);
          }
        }
      }
    }
  }

  // Find xmp:CreateDate
  pos = xml.find("xmp:CreateDate");
  if (pos != std::string::npos) {
    pos = xml.find(">", pos) + 1;
    size_t end = xml.find("<", pos);
    if (end != std::string::npos) {
      xmp_create_date = xml.substr(pos, end - pos);
    }
  }

  // Find xmp:ModifyDate
  pos = xml.find("xmp:ModifyDate");
  if (pos != std::string::npos) {
    pos = xml.find(">", pos) + 1;
    size_t end = xml.find("<", pos);
    if (end != std::string::npos) {
      xmp_modify_date = xml.substr(pos, end - pos);
    }
  }

  // Find pdf:Producer
  pos = xml.find("pdf:Producer");
  if (pos != std::string::npos) {
    pos = xml.find(">", pos) + 1;
    size_t end = xml.find("<", pos);
    if (end != std::string::npos) {
      pdf_producer = xml.substr(pos, end - pos);
    }
  }

  return true;
}

// Parse XMP metadata
void parse_xmp_metadata(const Pdf& pdf, DocumentCatalog& catalog) {
  // Look for Metadata entry in document catalog
  auto catalog_obj = resolve_reference(pdf, pdf.root, 0);
  if (!catalog_obj.success) {
    return;
  }

  if (catalog_obj.value.type == Value::DICTIONARY) {
    const auto& catalog_dict = catalog_obj.value.dict;
    auto metadata_it = catalog_dict.find("Metadata");
    if (metadata_it != catalog_dict.end()) {
      if (metadata_it->second.type == Value::REFERENCE) {
        auto metadata_obj = resolve_reference(pdf, metadata_it->second.ref_object_number,
                                              metadata_it->second.ref_generation_number);
        if (metadata_obj.success) {
          if (metadata_obj.value.type == Value::STREAM) {
            // Decode the stream
            auto decoded = decode_stream(pdf, metadata_obj.value);
            if (decoded.success) {
              std::string xml_data(decoded.data.begin(), decoded.data.end());
              catalog.xmp_metadata.parse_xml(xml_data);
            }
          }
        }
      }
    }
  }
}

// Phase 6.2: Advanced Graphics Implementation

// Parse extended graphics state dictionary
ExtendedGraphicsState parse_ext_gstate(const Pdf& pdf, const Dictionary& gs_dict) {
  ExtendedGraphicsState state;

  // Line parameters
  auto it = gs_dict.find("LW");
  if (it != gs_dict.end() && it->second.type == Value::NUMBER) {
    state.line_width = it->second.number;
  }

  it = gs_dict.find("LC");
  if (it != gs_dict.end() && it->second.type == Value::NUMBER) {
    state.line_cap = static_cast<int>(it->second.number);
  }

  it = gs_dict.find("LJ");
  if (it != gs_dict.end() && it->second.type == Value::NUMBER) {
    state.line_join = static_cast<int>(it->second.number);
  }

  it = gs_dict.find("ML");
  if (it != gs_dict.end() && it->second.type == Value::NUMBER) {
    state.miter_limit = it->second.number;
  }

  it = gs_dict.find("D");
  if (it != gs_dict.end() && it->second.type == Value::ARRAY) {
    if (it->second.array.size() >= 2) {
      // First element is dash pattern array
      if (it->second.array[0].type == Value::ARRAY) {
        for (const auto& val : it->second.array[0].array) {
          if (val.type == Value::NUMBER) {
            state.dash_pattern.push_back(val.number);
          }
        }
      }
      // Second element is dash phase
      if (it->second.array[1].type == Value::NUMBER) {
        state.dash_phase = it->second.array[1].number;
      }
    }
  }

  // Transparency parameters
  it = gs_dict.find("ca");
  if (it != gs_dict.end() && it->second.type == Value::NUMBER) {
    state.ca = it->second.number;
  }

  it = gs_dict.find("CA");
  if (it != gs_dict.end() && it->second.type == Value::NUMBER) {
    state.CA = it->second.number;
  }

  it = gs_dict.find("BM");
  if (it != gs_dict.end()) {
    if (it->second.type == Value::NAME) {
      state.blend_mode = parse_blend_mode(it->second.name);
    } else if (it->second.type == Value::ARRAY && !it->second.array.empty()) {
      // Multiple blend modes, use first one
      if (it->second.array[0].type == Value::NAME) {
        state.blend_mode = parse_blend_mode(it->second.array[0].name);
      }
    }
  }

  it = gs_dict.find("AIS");
  if (it != gs_dict.end() && it->second.type == Value::BOOLEAN) {
    state.alpha_is_shape = it->second.boolean;
  }

  it = gs_dict.find("TK");
  if (it != gs_dict.end() && it->second.type == Value::BOOLEAN) {
    state.knockout_group = it->second.boolean;
  }

  // Soft mask
  it = gs_dict.find("SMask");
  if (it != gs_dict.end()) {
    if (it->second.type == Value::NAME && it->second.name == "None") {
      state.soft_mask_type = SoftMaskType::None;
      state.soft_mask_value = Value();
    } else {
      state.soft_mask_value = it->second;
      if (it->second.type == Value::DICTIONARY) {
        state.soft_mask_dict = it->second.dict;
        auto type_it = it->second.dict.find("S");
        if (type_it != it->second.dict.end() && type_it->second.type == Value::NAME) {
          if (type_it->second.name == "Alpha") {
            state.soft_mask_type = SoftMaskType::Alpha;
          } else if (type_it->second.name == "Luminosity") {
            state.soft_mask_type = SoftMaskType::Luminosity;
          }
        }
      }
    }
  }
  else {
    state.soft_mask_value = Value();
  }

  // Rendering intent
  it = gs_dict.find("RI");
  if (it != gs_dict.end() && it->second.type == Value::NAME) {
    state.rendering_intent = it->second.name;
  }

  // Overprint control
  it = gs_dict.find("OP");
  if (it != gs_dict.end() && it->second.type == Value::BOOLEAN) {
    state.overprint_stroking = it->second.boolean;
  }

  it = gs_dict.find("op");
  if (it != gs_dict.end() && it->second.type == Value::BOOLEAN) {
    state.overprint_nonstroking = it->second.boolean;
  }

  it = gs_dict.find("OPM");
  if (it != gs_dict.end() && it->second.type == Value::NUMBER) {
    state.overprint_mode = static_cast<int>(it->second.number);
  }

  return state;
}

// Parse blend mode from name
BlendMode parse_blend_mode(const std::string& mode_name) {
  static const std::map<std::string, BlendMode> blend_modes = {
    {"Normal", BlendMode::Normal},
    {"Compatible", BlendMode::Normal},  // Alias
    {"Multiply", BlendMode::Multiply},
    {"Screen", BlendMode::Screen},
    {"Overlay", BlendMode::Overlay},
    {"Darken", BlendMode::Darken},
    {"Lighten", BlendMode::Lighten},
    {"ColorDodge", BlendMode::ColorDodge},
    {"ColorBurn", BlendMode::ColorBurn},
    {"HardLight", BlendMode::HardLight},
    {"SoftLight", BlendMode::SoftLight},
    {"Difference", BlendMode::Difference},
    {"Exclusion", BlendMode::Exclusion},
    {"Hue", BlendMode::Hue},
    {"Saturation", BlendMode::Saturation},
    {"Color", BlendMode::Color},
    {"Luminosity", BlendMode::Luminosity}
  };

  auto it = blend_modes.find(mode_name);
  return (it != blend_modes.end()) ? it->second : BlendMode::Normal;
}

// Parse pattern dictionary
std::unique_ptr<Pattern> parse_pattern(const Pdf& pdf, const Dictionary& pattern_dict) {
  auto pattern = std::unique_ptr<Pattern>(new Pattern());

  // Get pattern type
  auto type_it = pattern_dict.find("PatternType");
  if (type_it == pattern_dict.end() || type_it->second.type != Value::NUMBER) {
    return nullptr;
  }

  int pattern_type = static_cast<int>(type_it->second.number);
  pattern->type = static_cast<PatternType>(pattern_type);

  // Parse matrix if present
  auto matrix_it = pattern_dict.find("Matrix");
  if (matrix_it != pattern_dict.end() && matrix_it->second.type == Value::ARRAY) {
    pattern->matrix.clear();
    for (const auto& val : matrix_it->second.array) {
      if (val.type == Value::NUMBER) {
        pattern->matrix.push_back(val.number);
      }
    }
  }

  if (pattern->type == PatternType::Tiling) {
    // Parse tiling pattern
    pattern->tiling.reset(new TilingPattern());

    auto paint_it = pattern_dict.find("PaintType");
    if (paint_it != pattern_dict.end() && paint_it->second.type == Value::NUMBER) {
      pattern->tiling->paint_type = static_cast<TilingPaintType>(
        static_cast<int>(paint_it->second.number));
    }

    auto tiling_it = pattern_dict.find("TilingType");
    if (tiling_it != pattern_dict.end() && tiling_it->second.type == Value::NUMBER) {
      pattern->tiling->tiling_type = static_cast<int>(tiling_it->second.number);
    }

    auto bbox_it = pattern_dict.find("BBox");
    if (bbox_it != pattern_dict.end() && bbox_it->second.type == Value::ARRAY) {
      for (const auto& val : bbox_it->second.array) {
        if (val.type == Value::NUMBER) {
          pattern->tiling->bbox.push_back(val.number);
        }
      }
    }

    auto xstep_it = pattern_dict.find("XStep");
    if (xstep_it != pattern_dict.end() && xstep_it->second.type == Value::NUMBER) {
      pattern->tiling->x_step = xstep_it->second.number;
    }

    auto ystep_it = pattern_dict.find("YStep");
    if (ystep_it != pattern_dict.end() && ystep_it->second.type == Value::NUMBER) {
      pattern->tiling->y_step = ystep_it->second.number;
    }

    auto res_it = pattern_dict.find("Resources");
    if (res_it != pattern_dict.end() && res_it->second.type == Value::DICTIONARY) {
      pattern->tiling->resources = res_it->second.dict;
    }

  } else if (pattern->type == PatternType::Shading) {
    // Parse shading pattern
    auto shading_it = pattern_dict.find("Shading");
    if (shading_it != pattern_dict.end()) {
      if (shading_it->second.type == Value::DICTIONARY) {
        pattern->shading = parse_shading(pdf, shading_it->second.dict);
      } else if (shading_it->second.type == Value::REFERENCE) {
        // Resolve shading reference
        ResolvedObject resolved = resolve_reference(pdf,
          shading_it->second.ref_object_number,
          shading_it->second.ref_generation_number);
        if (resolved.success && resolved.value.type == Value::DICTIONARY) {
          pattern->shading = parse_shading(pdf, resolved.value.dict);
        }
      }
    }
  }

  return pattern;
}

// Parse shading dictionary
std::unique_ptr<Shading> parse_shading(const Pdf& pdf, const Dictionary& shading_dict) {
  auto shading = std::unique_ptr<Shading>(new Shading());

  // Get shading type
  auto type_it = shading_dict.find("ShadingType");
  if (type_it == shading_dict.end() || type_it->second.type != Value::NUMBER) {
    return nullptr;
  }

  shading->type = static_cast<ShadingType>(static_cast<int>(type_it->second.number));

  // Parse color space
  auto cs_it = shading_dict.find("ColorSpace");
  if (cs_it != shading_dict.end()) {
    shading->color_space.reset(new ColorSpace(parse_color_space(pdf, cs_it->second)));
  }

  // Parse bounding box
  auto bbox_it = shading_dict.find("BBox");
  if (bbox_it != shading_dict.end() && bbox_it->second.type == Value::ARRAY) {
    for (const auto& val : bbox_it->second.array) {
      if (val.type == Value::NUMBER) {
        shading->bbox.push_back(val.number);
      }
    }
  }

  // Parse background color
  auto bg_it = shading_dict.find("Background");
  if (bg_it != shading_dict.end() && bg_it->second.type == Value::ARRAY) {
    for (const auto& val : bg_it->second.array) {
      if (val.type == Value::NUMBER) {
        shading->background.push_back(val.number);
      }
    }
  }

  // Parse anti-aliasing flag
  auto aa_it = shading_dict.find("AntiAlias");
  if (aa_it != shading_dict.end() && aa_it->second.type == Value::BOOLEAN) {
    shading->anti_alias = aa_it->second.boolean;
  }

  // Parse type-specific parameters
  switch (shading->type) {
    case ShadingType::FunctionBased:
      {
        auto domain_it = shading_dict.find("Domain");
        if (domain_it != shading_dict.end() && domain_it->second.type == Value::ARRAY) {
          shading->domain.clear();
          for (const auto& val : domain_it->second.array) {
            if (val.type == Value::NUMBER) {
              shading->domain.push_back(val.number);
            }
          }
        }

        auto matrix_it = shading_dict.find("Matrix");
        if (matrix_it != shading_dict.end() && matrix_it->second.type == Value::ARRAY) {
          shading->matrix.clear();
          for (const auto& val : matrix_it->second.array) {
            if (val.type == Value::NUMBER) {
              shading->matrix.push_back(val.number);
            }
          }
        }

        auto func_it = shading_dict.find("Function");
        if (func_it != shading_dict.end()) {
          shading->function = func_it->second;
        }
      }
      break;

    case ShadingType::Axial:
    case ShadingType::Radial:
      {
        auto coords_it = shading_dict.find("Coords");
        if (coords_it != shading_dict.end() && coords_it->second.type == Value::ARRAY) {
          for (const auto& val : coords_it->second.array) {
            if (val.type == Value::NUMBER) {
              shading->coords.push_back(val.number);
            }
          }
        }

        auto extend_it = shading_dict.find("Extend");
        if (extend_it != shading_dict.end() && extend_it->second.type == Value::ARRAY) {
          shading->extend.clear();
          for (const auto& val : extend_it->second.array) {
            if (val.type == Value::BOOLEAN) {
              shading->extend.push_back(val.boolean);
            }
          }
        }

        auto func_it = shading_dict.find("Function");
        if (func_it != shading_dict.end()) {
          shading->function = func_it->second;
        }
      }
      break;

    default:
      // Mesh-based shadings would require more complex parsing
      break;
  }

  return shading;
}

// Parse transparency group dictionary
TransparencyGroup parse_transparency_group(const Pdf& pdf, const Dictionary& group_dict) {
  TransparencyGroup group;

  // Check if it's actually a transparency group
  auto s_it = group_dict.find("S");
  if (s_it == group_dict.end() || s_it->second.type != Value::NAME ||
      s_it->second.name != "Transparency") {
    return group;
  }

  // Parse color space
  auto cs_it = group_dict.find("CS");
  if (cs_it != group_dict.end()) {
    group.color_space.reset(new ColorSpace(parse_color_space(pdf, cs_it->second)));
  }

  // Parse isolated flag
  auto iso_it = group_dict.find("I");
  if (iso_it != group_dict.end() && iso_it->second.type == Value::BOOLEAN) {
    group.isolated = iso_it->second.boolean;
  }

  // Parse knockout flag
  auto ko_it = group_dict.find("K");
  if (ko_it != group_dict.end() && ko_it->second.type == Value::BOOLEAN) {
    group.knockout = ko_it->second.boolean;
  }

  return group;
}

// Phase 6.3: Tagged PDF Implementation

// Check if a PDF is tagged
bool is_tagged_pdf(const Pdf& pdf) {
  // A PDF is tagged if it has a MarkInfo dictionary with Marked = true
  auto marked_it = pdf.catalog.mark_info.find("Marked");
  if (marked_it != pdf.catalog.mark_info.end() &&
      marked_it->second.type == Value::BOOLEAN) {
    return marked_it->second.boolean;
  }
  return false;
}

// Parse structure tree from catalog
StructureTreeRoot parse_structure_tree(const Pdf& pdf, const Dictionary& struct_tree_dict) {
  StructureTreeRoot tree_root;

  // Parse role map
  auto role_map_it = struct_tree_dict.find("RoleMap");
  if (role_map_it != struct_tree_dict.end() &&
      role_map_it->second.type == Value::DICTIONARY) {
    tree_root.role_map = parse_role_map(role_map_it->second.dict);
  }

  // Parse class map
  auto class_map_it = struct_tree_dict.find("ClassMap");
  if (class_map_it != struct_tree_dict.end() &&
      class_map_it->second.type == Value::DICTIONARY) {
    for (const auto& pair : class_map_it->second.dict) {
      tree_root.class_map.push_back(pair.first);
    }
  }

  // Parse parent tree (number tree)
  auto parent_tree_it = struct_tree_dict.find("ParentTree");
  if (parent_tree_it != struct_tree_dict.end()) {
    if (parent_tree_it->second.type == Value::DICTIONARY) {
      tree_root.parent_tree = parent_tree_it->second.dict;
    } else if (parent_tree_it->second.type == Value::REFERENCE) {
      ResolvedObject resolved = resolve_reference(pdf,
        parent_tree_it->second.ref_object_number,
        parent_tree_it->second.ref_generation_number);
      if (resolved.success && resolved.value.type == Value::DICTIONARY) {
        tree_root.parent_tree = resolved.value.dict;
      }
    }
  }

  // Parse ID tree
  auto id_tree_it = struct_tree_dict.find("IDTree");
  if (id_tree_it != struct_tree_dict.end() &&
      id_tree_it->second.type == Value::NAME) {
    tree_root.id_tree = id_tree_it->second.name;
  }

  // Parse structure tree root element (K entry)
  auto k_it = struct_tree_dict.find("K");
  if (k_it != struct_tree_dict.end()) {
    tree_root.root_element = parse_structure_element(pdf, k_it->second);
  }

  return tree_root;
}

// Parse structure type from string
StructureType parse_structure_type(const std::string& type_name) {
  static const std::map<std::string, StructureType> type_map = {
    // Grouping elements
    {"Document", StructureType::Document},
    {"Part", StructureType::Part},
    {"Art", StructureType::Art},
    {"Sect", StructureType::Sect},
    {"Div", StructureType::Div},
    {"BlockQuote", StructureType::BlockQuote},
    {"Caption", StructureType::Caption},
    {"TOC", StructureType::TOC},
    {"TOCI", StructureType::TOCI},
    {"Index", StructureType::Index},
    {"NonStruct", StructureType::NonStruct},
    {"Private", StructureType::Private},

    // Block-level elements
    {"H", StructureType::H},
    {"H1", StructureType::H1},
    {"H2", StructureType::H2},
    {"H3", StructureType::H3},
    {"H4", StructureType::H4},
    {"H5", StructureType::H5},
    {"H6", StructureType::H6},
    {"P", StructureType::P},
    {"L", StructureType::L},
    {"LI", StructureType::LI},
    {"Lbl", StructureType::Lbl},
    {"LBody", StructureType::LBody},

    // Table elements
    {"Table", StructureType::Table},
    {"TR", StructureType::TR},
    {"TH", StructureType::TH},
    {"TD", StructureType::TD},
    {"THead", StructureType::THead},
    {"TBody", StructureType::TBody},
    {"TFoot", StructureType::TFoot},

    // Inline elements
    {"Span", StructureType::Span},
    {"Quote", StructureType::Quote},
    {"Note", StructureType::Note},
    {"Reference", StructureType::Reference},
    {"BibEntry", StructureType::BibEntry},
    {"Code", StructureType::Code},
    {"Link", StructureType::Link},
    {"Annot", StructureType::Annot},

    // Illustration elements
    {"Figure", StructureType::Figure},
    {"Formula", StructureType::Formula},
    {"Form", StructureType::Form},

    // Ruby and Warichu elements
    {"Ruby", StructureType::Ruby},
    {"RB", StructureType::RB},
    {"RT", StructureType::RT},
    {"RP", StructureType::RP},
    {"Warichu", StructureType::Warichu},
    {"WT", StructureType::WT},
    {"WP", StructureType::WP}
  };

  auto it = type_map.find(type_name);
  return (it != type_map.end()) ? it->second : StructureType::Unknown;
}

// Parse structure element
std::unique_ptr<StructureElement> parse_structure_element(const Pdf& pdf, const Value& elem_val) {
  if (elem_val.type == Value::REFERENCE) {
    // Resolve reference
    ResolvedObject resolved = resolve_reference(pdf,
      elem_val.ref_object_number,
      elem_val.ref_generation_number);
    if (resolved.success) {
      return parse_structure_element(pdf, resolved.value);
    }
    return nullptr;
  }

  if (elem_val.type != Value::DICTIONARY) {
    return nullptr;
  }

  auto element = std::unique_ptr<StructureElement>(new StructureElement());
  const Dictionary& elem_dict = elem_val.dict;

  // Parse type (S entry)
  auto s_it = elem_dict.find("S");
  if (s_it != elem_dict.end() && s_it->second.type == Value::NAME) {
    element->type_string = s_it->second.name;
    element->type = parse_structure_type(s_it->second.name);
  }

  // Parse ID
  auto id_it = elem_dict.find("ID");
  if (id_it != elem_dict.end()) {
    if (id_it->second.type == Value::STRING) {
      element->id = id_it->second.str;
    } else if (id_it->second.type == Value::NAME) {
      element->id = id_it->second.name;
    }
  }

  // Parse title (T)
  auto t_it = elem_dict.find("T");
  if (t_it != elem_dict.end() && t_it->second.type == Value::STRING) {
    element->title = t_it->second.str;
  }

  // Parse language
  auto lang_it = elem_dict.find("Lang");
  if (lang_it != elem_dict.end() && lang_it->second.type == Value::STRING) {
    element->lang = lang_it->second.str;
  }

  // Parse alternative text
  auto alt_it = elem_dict.find("Alt");
  if (alt_it != elem_dict.end() && alt_it->second.type == Value::STRING) {
    element->alt_text = alt_it->second.str;
  }

  // Parse actual text
  auto actual_it = elem_dict.find("ActualText");
  if (actual_it != elem_dict.end() && actual_it->second.type == Value::STRING) {
    element->actual_text = actual_it->second.str;
  }

  // Parse expansion text (E)
  auto e_it = elem_dict.find("E");
  if (e_it != elem_dict.end() && e_it->second.type == Value::STRING) {
    element->expansion = e_it->second.str;
  }

  // Parse attributes (A)
  auto a_it = elem_dict.find("A");
  if (a_it != elem_dict.end()) {
    if (a_it->second.type == Value::DICTIONARY) {
      element->attributes = parse_structure_attributes(pdf, a_it->second.dict);
    } else if (a_it->second.type == Value::ARRAY) {
      // Multiple attribute dictionaries
      for (const auto& attr_val : a_it->second.array) {
        if (attr_val.type == Value::DICTIONARY) {
          // Merge attributes (simplified - just use first for now)
          element->attributes = parse_structure_attributes(pdf, attr_val.dict);
          break;
        }
      }
    }
  }

  // Parse class names (C)
  auto c_it = elem_dict.find("C");
  if (c_it != elem_dict.end()) {
    if (c_it->second.type == Value::NAME) {
      element->classes.push_back(c_it->second.name);
    } else if (c_it->second.type == Value::ARRAY) {
      for (const auto& class_val : c_it->second.array) {
        if (class_val.type == Value::NAME) {
          element->classes.push_back(class_val.name);
        }
      }
    }
  }

  // Parse page reference (Pg)
  auto pg_it = elem_dict.find("Pg");
  if (pg_it != elem_dict.end() && pg_it->second.type == Value::REFERENCE) {
    element->page_ref = pg_it->second.ref_object_number;
    element->page_gen = pg_it->second.ref_generation_number;
  }

  // Parse kids/content (K)
  auto k_it = elem_dict.find("K");
  if (k_it != elem_dict.end()) {
    if (k_it->second.type == Value::NUMBER) {
      // Single MCID
      MarkedContentID mcid;
      mcid.mcid = static_cast<int>(k_it->second.number);
      mcid.page_ref = element->page_ref;
      mcid.page_gen = element->page_gen;
      element->marked_content.push_back(mcid);
    } else if (k_it->second.type == Value::DICTIONARY) {
      // Single child element or marked content reference
      auto child = parse_structure_element(pdf, k_it->second);
      if (child) {
        child->parent = element.get();
        element->children.push_back(std::move(child));
      }
    } else if (k_it->second.type == Value::ARRAY) {
      // Multiple children
      for (const auto& child_val : k_it->second.array) {
        if (child_val.type == Value::NUMBER) {
          // MCID
          MarkedContentID mcid;
          mcid.mcid = static_cast<int>(child_val.number);
          mcid.page_ref = element->page_ref;
          mcid.page_gen = element->page_gen;
          element->marked_content.push_back(mcid);
        } else if (child_val.type == Value::DICTIONARY) {
          // Check if it's a marked content reference or object reference
          auto type_it = child_val.dict.find("Type");
          if (type_it != child_val.dict.end() &&
              type_it->second.type == Value::NAME) {
            if (type_it->second.name == "MCR") {
              // Marked content reference
              auto mcid_it = child_val.dict.find("MCID");
              if (mcid_it != child_val.dict.end() &&
                  mcid_it->second.type == Value::NUMBER) {
                MarkedContentID mcid;
                mcid.mcid = static_cast<int>(mcid_it->second.number);

                // Check for page reference
                auto pg_it = child_val.dict.find("Pg");
                if (pg_it != child_val.dict.end() &&
                    pg_it->second.type == Value::REFERENCE) {
                  mcid.page_ref = pg_it->second.ref_object_number;
                  mcid.page_gen = pg_it->second.ref_generation_number;
                } else {
                  mcid.page_ref = element->page_ref;
                  mcid.page_gen = element->page_gen;
                }
                element->marked_content.push_back(mcid);
              }
            } else if (type_it->second.name == "OBJR") {
              // Object reference
              auto obj_it = child_val.dict.find("Obj");
              if (obj_it != child_val.dict.end() &&
                  obj_it->second.type == Value::REFERENCE) {
                ObjectReference objref;
                objref.obj_ref = obj_it->second.ref_object_number;
                objref.obj_gen = obj_it->second.ref_generation_number;
                element->object_refs.push_back(objref);
              }
            }
          } else {
            // Child structure element
            auto child = parse_structure_element(pdf, child_val);
            if (child) {
              child->parent = element.get();
              element->children.push_back(std::move(child));
            }
          }
        } else {
          // Try to parse as child element
          auto child = parse_structure_element(pdf, child_val);
          if (child) {
            child->parent = element.get();
            element->children.push_back(std::move(child));
          }
        }
      }
    }
  }

  return element;
}

// Parse structure element attributes
StructureElementAttributes parse_structure_attributes(const Pdf& pdf, const Dictionary& attr_dict) {
  StructureElementAttributes attrs;

  // Parse owner
  auto owner_it = attr_dict.find("O");
  if (owner_it != attr_dict.end() && owner_it->second.type == Value::NAME) {
    attrs.owner = owner_it->second.name;
  }

  // Layout attributes
  auto placement_it = attr_dict.find("Placement");
  if (placement_it != attr_dict.end() && placement_it->second.type == Value::NAME) {
    attrs.placement = placement_it->second.name;
  }

  auto writing_mode_it = attr_dict.find("WritingMode");
  if (writing_mode_it != attr_dict.end() && writing_mode_it->second.type == Value::NAME) {
    attrs.writing_mode = writing_mode_it->second.name;
  }

  auto text_align_it = attr_dict.find("TextAlign");
  if (text_align_it != attr_dict.end() && text_align_it->second.type == Value::NAME) {
    attrs.text_align = text_align_it->second.name;
  }

  auto bbox_it = attr_dict.find("BBox");
  if (bbox_it != attr_dict.end() && bbox_it->second.type == Value::ARRAY) {
    for (const auto& val : bbox_it->second.array) {
      if (val.type == Value::NUMBER) {
        attrs.bbox.push_back(val.number);
      }
    }
  }

  auto width_it = attr_dict.find("Width");
  if (width_it != attr_dict.end() && width_it->second.type == Value::NUMBER) {
    attrs.width = width_it->second.number;
  }

  auto height_it = attr_dict.find("Height");
  if (height_it != attr_dict.end() && height_it->second.type == Value::NUMBER) {
    attrs.height = height_it->second.number;
  }

  // Table attributes
  auto rowspan_it = attr_dict.find("RowSpan");
  if (rowspan_it != attr_dict.end() && rowspan_it->second.type == Value::NUMBER) {
    attrs.row_span = static_cast<int>(rowspan_it->second.number);
  }

  auto colspan_it = attr_dict.find("ColSpan");
  if (colspan_it != attr_dict.end() && colspan_it->second.type == Value::NUMBER) {
    attrs.col_span = static_cast<int>(colspan_it->second.number);
  }

  auto headers_it = attr_dict.find("Headers");
  if (headers_it != attr_dict.end() && headers_it->second.type == Value::ARRAY) {
    for (const auto& val : headers_it->second.array) {
      if (val.type == Value::STRING) {
        attrs.headers.push_back(val.str);
      } else if (val.type == Value::NAME) {
        attrs.headers.push_back(val.name);
      }
    }
  }

  auto scope_it = attr_dict.find("Scope");
  if (scope_it != attr_dict.end() && scope_it->second.type == Value::NAME) {
    attrs.scope = scope_it->second.name;
  }

  auto summary_it = attr_dict.find("Summary");
  if (summary_it != attr_dict.end() && summary_it->second.type == Value::STRING) {
    attrs.summary = summary_it->second.str;
  }

  // List attributes
  auto list_num_it = attr_dict.find("ListNumbering");
  if (list_num_it != attr_dict.end() && list_num_it->second.type == Value::NAME) {
    attrs.list_numbering = list_num_it->second.name;
  }

  return attrs;
}

// Parse role map
RoleMap parse_role_map(const Dictionary& role_map_dict) {
  RoleMap role_map;

  for (const auto& pair : role_map_dict) {
    if (pair.second.type == Value::NAME) {
      role_map.mappings[pair.first] = pair.second.name;
    }
  }

  return role_map;
}

// Parse marked content properties
MarkedContentProperties parse_marked_content_props(const Dictionary& props_dict) {
  MarkedContentProperties props;

  auto mcid_it = props_dict.find("MCID");
  if (mcid_it != props_dict.end() && mcid_it->second.type == Value::NUMBER) {
    props.mcid = static_cast<int>(mcid_it->second.number);
  }

  auto tag_it = props_dict.find("Tag");
  if (tag_it != props_dict.end() && tag_it->second.type == Value::NAME) {
    props.tag = tag_it->second.name;
  }

  auto lang_it = props_dict.find("Lang");
  if (lang_it != props_dict.end() && lang_it->second.type == Value::STRING) {
    props.lang = lang_it->second.str;
  }

  auto alt_it = props_dict.find("Alt");
  if (alt_it != props_dict.end() && alt_it->second.type == Value::STRING) {
    props.alt_text = alt_it->second.str;
  }

  auto actual_it = props_dict.find("ActualText");
  if (actual_it != props_dict.end() && actual_it->second.type == Value::STRING) {
    props.actual_text = actual_it->second.str;
  }

  auto exp_it = props_dict.find("E");
  if (exp_it != props_dict.end() && exp_it->second.type == Value::STRING) {
    props.expansion = exp_it->second.str;
  }

  props.properties = props_dict;

  return props;
}

// Find structure element by MCID
StructureElement* find_structure_element_by_mcid(StructureElement* root, int mcid, uint32_t page_ref) {
  if (!root) return nullptr;

  // Check if this element contains the MCID
  for (const auto& mc : root->marked_content) {
    if (mc.mcid == mcid && mc.page_ref == page_ref) {
      return root;
    }
  }

  // Search children recursively
  for (const auto& child : root->children) {
    auto result = find_structure_element_by_mcid(child.get(), mcid, page_ref);
    if (result) {
      return result;
    }
  }

  return nullptr;
}

}  // namespace nanopdf
