#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstring.h"
#include "nanostring.h"
#else
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
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

#ifdef STB_IMAGE_IMPLEMENTATION
  // Use stb_image to decode JPEG data
  int width, height, channels;
  unsigned char *decoded = stbi_load_from_memory(data, static_cast<int>(size),
                                                  &width, &height, &channels, 0);
  if (!decoded) {
    result.error = "DCTDecode: Failed to decode JPEG data";
    return result;
  }

  // Copy decoded data to result
  size_t decoded_size = width * height * channels;
  result.data.assign(decoded, decoded + decoded_size);
  stbi_image_free(decoded);
  result.success = true;
#else
  // Fallback: just copy the JPEG data as-is for now
  result.data.assign(data, data + size);
  result.success = true;
#endif

  return result;
}

}  // namespace filters

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
  } else if (filter_name == "RunLengthDecode") {
    return filters::decode_runlength(stream_obj.stream.data.data(),
                                     stream_obj.stream.data.size(), params);
  } else if (filter_name == "DCTDecode") {
    return filters::decode_dct(stream_obj.stream.data.data(),
                              stream_obj.stream.data.size(), params);
  }

  result.error = "Unsupported filter type: " + filter_name;
  return result;
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
  
  return sig_field;
}

// Stub implementations for missing functions
bool parse_string(StreamReader& sr, Parser& parser, std::string* out_str) {
  // Stub implementation - needs proper PDF string parsing
  *out_str = "";
  return false;
}

bool parse_name(StreamReader& sr, Parser& parser, std::string* out_str) {
  // Stub implementation - needs proper PDF name parsing  
  *out_str = "";
  return false;
}

bool parse_number(StreamReader& sr, Parser& parser, double* out_number) {
  // Stub implementation - needs proper PDF number parsing
  *out_number = 0.0;
  return false;
}

ResolvedObject resolve_reference(const Pdf& pdf, uint32_t obj_num, uint16_t gen_num) {
  // Stub implementation - needs proper object resolution
  ResolvedObject result;
  result.success = false;
  result.error = "resolve_reference not yet implemented";
  return std::move(result);
}

// Stub implementations for Pdf member functions
bool Pdf::load_document_structure() {
  // Parse Phase 3 features
  parse_acro_form(*this, catalog);

  // Parse Phase 4 features
  parse_document_outline(*this, catalog);
  parse_page_labels(*this, catalog);
  parse_named_destinations(*this, catalog);
  parse_document_info(*this, catalog);
  parse_xmp_metadata(*this, catalog);

  return true;
}

const Page* Pdf::get_page(uint32_t page_number) const {
  // Stub implementation - needs proper page retrieval
  return nullptr;
}

PageContent Page::load_contents(const Pdf& pdf) const {
  // Stub implementation - needs proper page content loading
  PageContent content;
  content.success = false;
  content.error = "load_contents not yet implemented";
  return content;
}

// Stub implementations for Value class functions
void Value::clear() {
  // Stub implementation - needs proper value cleanup
  type = UNDEFINED;
}

Value& Value::operator=(const Value& other) {
  // Stub implementation - needs proper value assignment
  if (this != &other) {
    clear();
    type = other.type;
    // Copy other fields as needed
  }
  return *this;
}

Value& Value::operator=(Value&& other) noexcept {
  // Stub implementation - needs proper value move assignment
  if (this != &other) {
    clear();
    type = other.type;
    other.type = UNDEFINED;
    // Move other fields as needed
  }
  return *this;
}

// Color space parsing
ColorSpace parse_color_space(const Pdf& pdf, const Value& cs_value) {
  ColorSpace color_space;

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
      } else if (type_name == "Separation" || type_name == "DeviceN") {
        color_space.type = (type_name == "Separation") ?
                          ColorSpaceType::Separation : ColorSpaceType::DeviceN;
        // These require additional parsing of alternate color space and tint transform
      }
    }
  }

  return color_space;
}

// Image XObject parsing
ImageXObject parse_image_xobject(const Pdf& pdf, const Value& stream_value) {
  ImageXObject image;

  if (stream_value.type != Value::STREAM) {
    return image;  // Invalid image object
  }

  const Dictionary& dict = stream_value.stream.dict;

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

  // Decode the image data
  DecodedStream decoded = decode_stream(pdf, stream_value);
  if (decoded.success) {
    image.data = std::move(decoded.data);
  }

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
          images[entry.first] = parse_image_xobject(pdf, resolved_value);
        }
      }
    }
  }

  return images;
}

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
      text_state_.reset();
    } else if (op == "ET") {
      // End text block
      if (!text_state_.current_text.empty()) {
        extracted_text_ += text_state_.current_text;
        text_state_.current_text.clear();
      }
    } else if (op == "Td" && operands.size() >= 2) {
      // Move text position
      double tx = parse_number(operands[0]);
      double ty = parse_number(operands[1]);
      text_state_.line_matrix[4] += tx * text_state_.line_matrix[0] + ty * text_state_.line_matrix[2];
      text_state_.line_matrix[5] += tx * text_state_.line_matrix[1] + ty * text_state_.line_matrix[3];
      std::copy(text_state_.line_matrix, text_state_.line_matrix + 6, text_state_.text_matrix);
    } else if (op == "TD" && operands.size() >= 2) {
      // Move text position and set leading
      double tx = parse_number(operands[0]);
      double ty = parse_number(operands[1]);
      text_state_.leading = -ty;
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
      // Look up font in page resources
      auto font_it = page_.fonts.find(text_state_.font_name.substr(1)); // Remove leading /
      if (font_it != page_.fonts.end()) {
        text_state_.current_font = font_it->second.get();
      }
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

// Parse Type0 (CID) font
std::unique_ptr<BaseFont> parse_type0_font(const Pdf& pdf, const Dictionary& font_dict) {
  auto font = std::unique_ptr<Type0Font>(new Type0Font());

  // Parse BaseFont name
  auto base_it = font_dict.find("BaseFont");
  if (base_it != font_dict.end() && base_it->second.type == Value::NAME) {
    font->base_font = base_it->second.name;
  }

  // Parse Encoding (CMap)
  auto encoding_it = font_dict.find("Encoding");
  if (encoding_it != font_dict.end()) {
    if (encoding_it->second.type == Value::NAME) {
      font->encoding_cmap.name = encoding_it->second.name;
    } else if (encoding_it->second.type == Value::STREAM) {
      // Parse embedded CMap
      // This would require parsing the CMap stream format
    }
  }

  // Parse ToUnicode CMap
  auto tounicode_it = font_dict.find("ToUnicode");
  if (tounicode_it != font_dict.end() && tounicode_it->second.type == Value::STREAM) {
    // Parse ToUnicode CMap stream
    // This would decode character codes to Unicode
  }

  // Parse DescendantFonts array
  auto desc_it = font_dict.find("DescendantFonts");
  if (desc_it != font_dict.end() && desc_it->second.type == Value::ARRAY) {
    if (!desc_it->second.array.empty()) {
      const Value& desc_font_ref = desc_it->second.array[0];
      if (desc_font_ref.type == Value::REFERENCE) {
        ResolvedObject resolved = resolve_reference(pdf,
          desc_font_ref.ref_object_number,
          desc_font_ref.ref_generation_number);
        if (resolved.success && resolved.value.type == Value::DICTIONARY) {
          // Parse descendant font (CIDFont)
          auto subtype_it = resolved.value.dict.find("Subtype");
          if (subtype_it != resolved.value.dict.end() &&
              subtype_it->second.type == Value::NAME) {
            // Create appropriate descendant font type
            font->descendant_font.reset(new BaseFont());
            font->descendant_font->subtype = subtype_it->second.name;
          }
        }
      }
    }
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
  if (desc_val.type != Value::DICTIONARY) {
    return nullptr;
  }

  auto descriptor = std::unique_ptr<FontDescriptor>(new FontDescriptor());
  const Dictionary& dict = desc_val.dict;

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
    if (enc_it != font_dict.end() && enc_it->second.type == Value::NAME) {
      font->encoding = enc_it->second.name;
    }

    // Parse FontDescriptor
    auto desc_it = font_dict.find("FontDescriptor");
    if (desc_it != font_dict.end()) {
      font->descriptor = parse_font_descriptor(pdf, desc_it->second).release();
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

}  // namespace nanopdf
