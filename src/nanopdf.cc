#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstring.h"
#include "nanostring.h"
#else
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#endif

#include <zlib.h>

#include "common-macros.inc"
#include "nanopdf.hh"
#include "stream-reader.hh"

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
    char c;
    while (Look(&c)) {
      if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
        break;
      }
      _sr.seek_from_current(1);
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
  if (it == out_value->dict.end() || it->second.type != Value::NUMBER) {
    return false;
  }
  length = static_cast<size_t>(it->second.number);

  // Read stream data
  out_value->type = Value::STREAM;
  out_value->stream.data.resize(length);
  if (!sr.read(length, out_value->stream.data.data())) {
    return false;
  }

  // Move dictionary to stream
  out_value->stream.dict = std::move(out_value->dict);

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
bool parse_string(StreamReader &sr, Parser &parser, std::vector<uint8_t> *str);
bool parse_number(StreamReader &sr, Parser &parser, double *num);
bool parse_reference(StreamReader &sr, Parser &parser, Value *value);
bool parse_stream(StreamReader &sr, Parser &parser, Value *value);

// Helper function to skip whitespace
inline bool skip_whitespace(StreamReader &sr) {
  char c;
  while (!sr.eof()) {
    if (!sr.read1(&c)) return false;
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      sr.seek_from_current(-1);
      break;
    }
  }
  return true;
}

bool parse_object(StreamReader &sr, Parser &parser, Value *value) {
  if (!value) return false;

  // Skip whitespace
  if (!skip_whitespace(sr)) return false;

  char c;
  if (!sr.read1(&c)) return false;

  if (c == '<') {
    // Dictionary or hex string
    if (!sr.read1(&c)) return false;
    if (c == '<') {
      // Dictionary
      value->type = Value::DICTIONARY;
      if (!parse_dictionary(sr, parser, &value->dict)) return false;

      // Check if this is a stream object
      return parse_stream(sr, parser, value);
    } else {
      // Hex string
      sr.seek_from_current(-1);
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
    sr.seek_from_current(-1);

    // Try parsing as reference first
    uint64_t saved_pos = sr.tell();
    if (parse_reference(sr, parser, value)) {
      return true;
    }

    // Not a reference, try as number
    sr.seek_set(saved_pos);
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

bool parse_dictionary(StreamReader &sr, Parser &parser, Dictionary *dict) {
  if (!dict) return false;

  if (!skip_whitespace(sr)) return false;

  char c;
  // Check for dictionary end
  if (!sr.read1(&c)) return false;
  if (c == '>') {
    char next;
    if (!sr.read1(&next)) return false;
    if (next == '>') {
      return true;
    }
    return false;
  }
  sr.seek_from_current(-1);

  // Must be a name
  if (!sr.read1(&c)) return false;
  if (c != '/') return false;

  // Parse key (name)
  std::string key;
  if (!parse_name(sr, parser, &key)) return false;

  // Parse value
  Value value;
  if (!parse_object(sr, parser, &value)) return false;

  (*dict)[key] = value;

  return true;
}

bool parse_array(StreamReader &sr, nanopdf::Parser &parser,
                 std::vector<Value> *arr) {
  if (!arr) return false;

  if (!skip_whitespace(sr)) return false;

  char c;
  // Check for array end
  if (!sr.read1(&c)) return false;
  if (c == ']') {
    return true;
  }
  sr.seek_from_current(-1);

  // Parse value
  Value value;
  if (!parse_object(sr, parser, &value)) return false;
  arr->push_back(value);

  return true;
}

bool parse_reference(StreamReader &sr, Parser &parser, Value *value) {
  if (!value) return false;

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

  value->type = Value::REFERENCE;
  return true;
}

bool parse_indirect_object(StreamReader &sr, Parser &parser, Value *out_value) {
  if (!out_value) return false;

  if (!skip_whitespace(sr)) return false;

  // Read object number
  uint32_t obj_num = 0;
  while (!sr.eof()) {
    char c;
    if (!sr.read1(&c)) return false;
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      if (obj_num > 0) break;
      continue;
    }
    if (c < '0' || c > '9') return false;
    obj_num = obj_num * 10 + (c - '0');
  }

  // TODO: Continue parsing generation number and object content

  return true;
}

#if 0
bool parse_from_memory(const uint8_t *addr, const size_t size, Pdf *out_pdf) {
  if (!addr || (size < 8) || !out_pdf) {
    return false;
  }

  out_pdf->data = addr;
  out_pdf->data_size = size;

  // Parse PDF version from header
  if ((addr[0] != '%') || (addr[1] != 'P') || (addr[2] != 'D') ||
      (addr[3] != 'F') || (addr[4] != '-') || (addr[6] != '.')) {
    return false;
  }

  out_pdf->version_major = static_cast<int>(addr[5] - '0');
  out_pdf->version_minor = static_cast<int>(addr[7] - '0');

  return out_pdf->load_document_structure();
}
#endif

#if 0
namespace filters {

namespace {

// Apply PNG predictor row filter
bool apply_predictor(std::vector<uint8_t> &data, const DecodeParams &params) {
  if (params.predictor < 10 || params.predictor > 15) {
    return true;  // No predictor or unsupported
  }

  int bytes_per_pixel = (params.bits_per_component * params.colors + 7) / 8;
  int bytes_per_row =
      (params.bits_per_component * params.colors * params.columns + 7) / 8;
  int row_count = data.size() / (bytes_per_row + 1);

  std::vector<uint8_t> output;
  output.reserve(row_count * bytes_per_row);

  for (int row = 0; row < row_count; row++) {
    int filter = data[row * (bytes_per_row + 1)];
    const uint8_t *row_data = &data[row * (bytes_per_row + 1) + 1];

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

DecodedStream decode_flate(const uint8_t *data, size_t size,
                           const DecodeParams &params) {
  DecodedStream result;
  std::vector<uint8_t> output;

  // Decompress using zlib
  z_stream strm;
  memset(&strm, 0, sizeof(strm));
  strm.next_in = const_cast<Bytef *>(data);
  strm.avail_in = static_cast<uInt>(size);

  if (inflateInit(&strm) != Z_OK) {
    result.error = "Failed to initialize zlib";
    return result;
  }

  int ret;
  do {
    output.resize(output.size() + 4096);
    strm.next_out = output.data() + strm.total_out;
    strm.avail_out = static_cast<uInt>(output.size() - strm.total_out);

    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&strm);
      result.error = "Failed to decompress data";
      return result;
    }
  } while (ret != Z_STREAM_END);

  inflateEnd(&strm);
  output.resize(strm.total_out);

  // Apply predictor if needed
  if (!apply_predictor(output, params)) {
    result.error = "Failed to apply PNG predictor";
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
    result.error = "Failed to initialize LZW decoder";
    return result;
  }

  // Decode data
  if (!decoder.decode(output)) {
    result.error = "Failed to decode LZW data";
    return result;
  }

  // Apply predictor if needed
  if (!apply_predictor(output, params)) {
    result.error = "Failed to apply PNG predictor";
    return result;
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}

DecodedStream decode_runlength(const uint8_t *data, size_t size,
                               const DecodeParams &params) {
  DecodedStream result;
  std::vector<uint8_t> output;

  size_t i = 0;
  while (i < size) {
    int8_t length = static_cast<int8_t>(data[i++]);

    if (length == -128) {
      break;  // End of data
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

DecodedStream decode_jbig2(const uint8_t * /*data*/, size_t /*size*/,
                           const DecodeParams & /*params*/) {
  DecodedStream result;
  result.success = false;
  result.error = "JBIG2Decode filter is not implemented.";
  return result;
}

}  // namespace filters
#endif

#if 0
DecodedStream decode_stream(const Pdf &pdf, const Value &stream_obj) {
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
                                 stream_obj.stream.data.size(), params);
  } else if (filter_name == "ASCII85Decode") {
    return filters::decode_ascii85(stream_obj.stream.data.data(),
                                   stream_obj.stream.data.size(), params);
  } else if (filter_name == "LZWDecode") {
    return filters::decode_lzw(stream_obj.stream.data.data(),
                               stream_obj.stream.data.size(), params);
  } else if (filter_name == "JBIG2Decode") {
    return filters::decode_jbig2(stream_obj.stream.data.data(),
                                 stream_obj.stream.data.size(), params);
  }

  result.error = "Unsupported filter type: " + filter_name;
  return result;
}
#endif

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

  bool swap_endian = false;  // TODO: detect endianness if needed
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

}  // namespace nanopdf
