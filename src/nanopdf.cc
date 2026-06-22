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

#ifdef NANOPDF_USE_MINIZ
#include "miniz.h"
#else
#include <zlib.h>
#endif

#ifdef NANOPDF_USE_LIBDEFLATE
#include "libdeflate/libdeflate.h"
#endif

#include "common-macros.inc"
#include "ccitt-decoder.hh"
#include "crypto.hh"
#include "jbig2/JBig2_Context.hh"
#include "jbig2/JBig2_Image.hh"
#include "jpx-decoder.hh"
#include "nanopdf-log.hh"
#include "nanopdf.hh"
#include "cff-parser.hh"
#include "stb_image.h"
#include "stream-reader.hh"
#include "text-layout.hh"
#include "type1-parser.hh"
#include "string-parse.hh"

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

  // Expect CR+LF, just LF, or just CR (many real-world PDFs use CR-only)
  char c;
  if (!sr.read1(&c)) return false;
  if (c == '\r') {
    // Peek at next byte: consume LF if present (CR+LF), otherwise CR-only
    char next;
    if (sr.read1(&next)) {
      if (next != '\n') {
        sr.seek_set(sr.tell() - 1);  // Put back non-LF byte
      }
    }
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
            // Remove only the EOL marker before endstream (per PDF spec 7.3.8.1)
            // Do NOT trim further - encrypted streams may legitimately end with \n/\r
            length = i - 1;
            if (length > 0 && static_cast<char>(data[length - 1]) == '\r' && before == '\n') {
              length--;  // Handle \r\n EOL
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

// Detect and parse linearization dictionary from the first indirect object
// in the file. Returns true if the PDF is linearized.
static bool parse_linearization_dict(const uint8_t* data, size_t data_size,
                                     LinearizationParams* out) {
  if (!data || data_size < 32 || !out) return false;

  // Skip header line (%PDF-x.y) and any binary comment line
  size_t pos = 0;
  // Skip first line
  while (pos < data_size && data[pos] != '\n' && data[pos] != '\r') ++pos;
  while (pos < data_size && (data[pos] == '\n' || data[pos] == '\r')) ++pos;
  // Skip optional binary comment line (starts with %)
  if (pos < data_size && data[pos] == '%') {
    while (pos < data_size && data[pos] != '\n' && data[pos] != '\r') ++pos;
    while (pos < data_size && (data[pos] == '\n' || data[pos] == '\r')) ++pos;
  }

  // Now we should be at the first indirect object: "N G obj"
  // Skip whitespace
  while (pos < data_size && (data[pos] == ' ' || data[pos] == '\t')) ++pos;

  // Parse object number and generation
  size_t obj_start = pos;
  while (pos < data_size && data[pos] >= '0' && data[pos] <= '9') ++pos;
  if (pos == obj_start || pos >= data_size) return false;

  // Skip space
  while (pos < data_size && data[pos] == ' ') ++pos;

  // Skip generation number
  while (pos < data_size && data[pos] >= '0' && data[pos] <= '9') ++pos;

  // Skip space
  while (pos < data_size && data[pos] == ' ') ++pos;

  // Check for "obj"
  if (pos + 3 > data_size) return false;
  if (data[pos] != 'o' || data[pos+1] != 'b' || data[pos+2] != 'j') return false;
  pos += 3;

  // Skip whitespace/newlines
  while (pos < data_size && (data[pos] == ' ' || data[pos] == '\n' ||
         data[pos] == '\r' || data[pos] == '\t')) ++pos;

  // Should start with "<<"
  if (pos + 2 > data_size || data[pos] != '<' || data[pos+1] != '<') return false;

  // Find the end of the dictionary (>>). Scan up to a reasonable limit.
  size_t dict_start = pos;
  size_t dict_end = 0;
  size_t scan_limit = std::min(data_size, dict_start + 2048);
  for (size_t i = dict_start + 2; i + 1 < scan_limit; ++i) {
    if (data[i] == '>' && data[i+1] == '>') {
      dict_end = i + 2;
      break;
    }
  }
  if (dict_end == 0) return false;

  // Extract the dictionary text
  std::string dict_text(reinterpret_cast<const char*>(data + dict_start),
                        dict_end - dict_start);

  // Check for /Linearized key
  if (dict_text.find("/Linearized") == std::string::npos) return false;

  // Helper to extract a numeric value for a key
  auto get_number = [&](const std::string& key) -> double {
    size_t kpos = dict_text.find(key);
    if (kpos == std::string::npos) return 0.0;
    kpos += key.size();
    // Skip whitespace
    while (kpos < dict_text.size() && (dict_text[kpos] == ' ' ||
           dict_text[kpos] == '\n' || dict_text[kpos] == '\r')) ++kpos;
    // Parse number
    size_t num_start = kpos;
    while (kpos < dict_text.size() && (dict_text[kpos] == '-' ||
           dict_text[kpos] == '+' || dict_text[kpos] == '.' ||
           (dict_text[kpos] >= '0' && dict_text[kpos] <= '9'))) ++kpos;
    if (kpos == num_start) return 0.0;
    return nanopdf::stod_or(dict_text.substr(num_start, kpos - num_start));
  };

  // Parse /H array: [offset length] or [offset1 length1 offset2 length2]
  auto parse_h_array = [&]() {
    size_t hpos = dict_text.find("/H");
    if (hpos == std::string::npos) return;
    hpos += 2;
    // Skip to '['
    while (hpos < dict_text.size() && dict_text[hpos] != '[') ++hpos;
    if (hpos >= dict_text.size()) return;
    ++hpos;  // skip '['

    std::vector<uint64_t> values;
    while (hpos < dict_text.size() && dict_text[hpos] != ']' && values.size() < 4) {
      while (hpos < dict_text.size() && (dict_text[hpos] == ' ' ||
             dict_text[hpos] == '\n' || dict_text[hpos] == '\r')) ++hpos;
      if (hpos >= dict_text.size() || dict_text[hpos] == ']') break;
      size_t nstart = hpos;
      while (hpos < dict_text.size() && dict_text[hpos] >= '0' &&
             dict_text[hpos] <= '9') ++hpos;
      if (hpos > nstart) {
        uint64_t v = 0;
        if (parse_uint64(dict_text.substr(nstart, hpos - nstart), &v)) {
          values.push_back(v);
        }
      }
    }
    if (values.size() >= 2) {
      out->hint_offset = values[0];
      out->hint_length = values[1];
    }
    if (values.size() >= 4) {
      out->hint_offset2 = values[2];
      out->hint_length2 = values[3];
    }
  };

  out->is_linearized = true;
  out->version = get_number("/Linearized");
  out->file_length = static_cast<uint64_t>(get_number("/L"));
  out->first_page_obj = static_cast<uint32_t>(get_number("/O"));
  out->first_page_end = static_cast<uint64_t>(get_number("/E"));
  out->num_pages = static_cast<uint32_t>(get_number("/N"));
  out->xref_offset = static_cast<uint64_t>(get_number("/T"));
  parse_h_array();

  return true;
}

ParseResult parse_pdf(const uint8_t *addr, const size_t size, Pdf *out_pdf) {
  if (!addr || !out_pdf) {
    return ParseResult::Fail(ErrorKind::IOError, "null input");
  }

  if (size < 8) {
    return ParseResult::Fail(ErrorKind::Malformed, "data too small");
  }

  // Find %PDF header — PDF spec allows up to 1024 bytes before it;
  // we scan up to 4096 to handle web-crawled files with prepended junk.
  size_t pdf_offset = 0;
  if (addr[0] == '%' && addr[1] == 'P' && addr[2] == 'D' && addr[3] == 'F' &&
      addr[4] == '-' && addr[6] == '.') {
    pdf_offset = 0;
  } else {
    size_t scan_limit = size < 4096 ? size : 4096;
    bool found = false;
    for (size_t i = 1; i + 7 < scan_limit; ++i) {
      if (addr[i] == '%' && addr[i+1] == 'P' && addr[i+2] == 'D' &&
          addr[i+3] == 'F' && addr[i+4] == '-' && addr[i+6] == '.') {
        pdf_offset = i;
        found = true;
        break;
      }
    }
    if (!found) {
      return ParseResult::Fail(ErrorKind::Malformed, "PDF header not found");
    }
  }

  // Adjust data pointer to start at %PDF header so xref offsets are correct
  out_pdf->data = addr + pdf_offset;
  out_pdf->data_size = size - pdf_offset;
  out_pdf->swap_endian = !is_system_little_endian();

  out_pdf->version_major = static_cast<int>(out_pdf->data[5] - '0');
  out_pdf->version_minor = static_cast<int>(out_pdf->data[7] - '0');

  if (!out_pdf->load_document_structure()) {
    // Check if failure is due to encryption
    if (out_pdf->encrypt != 0 && !out_pdf->security.authenticated) {
      return ParseResult::Fail(ErrorKind::Encrypted, "PDF is encrypted");
    }
    // Propagate structured error from load_document_structure if available
    if (out_pdf->last_error_kind != ErrorKind::None) {
      return ParseResult::Fail(out_pdf->last_error_kind, out_pdf->last_error);
    }
    // Check if xref parsing failed (no xref sections loaded)
    if (out_pdf->xref_sections.empty()) {
      return ParseResult::Fail(ErrorKind::Malformed, "failed to parse xref");
    }
    return ParseResult::Fail(ErrorKind::Internal,
                             "failed to load document structure");
  }

  // Check for encrypted PDF that requires authentication
  if (out_pdf->encrypt != 0 && !out_pdf->security.authenticated) {
    return ParseResult::Fail(ErrorKind::Encrypted, "PDF is encrypted");
  }

  return ParseResult::Ok();
}

bool parse_from_memory(const uint8_t *addr, const size_t size, Pdf *out_pdf) {
  return parse_pdf(addr, size, out_pdf).success;
}

// Scan for "endstream" to recover a stream's actual length
bool recover_stream_length(const uint8_t* data, size_t size,
                           size_t stream_start, size_t* out_length) {
  if (!data || !out_length || stream_start >= size) return false;

  const char* pattern = "endstream";
  const size_t pattern_len = 9;
  size_t search_end = size - pattern_len;

  for (size_t pos = stream_start; pos <= search_end; ++pos) {
    if (std::memcmp(data + pos, pattern, pattern_len) == 0) {
      // Verify it's preceded by whitespace/newline
      if (pos > stream_start) {
        uint8_t prev = data[pos - 1];
        if (prev == '\n' || prev == '\r') {
          *out_length = pos - stream_start;
          // Trim trailing CR/LF
          while (*out_length > 0 &&
                 (data[stream_start + *out_length - 1] == '\n' ||
                  data[stream_start + *out_length - 1] == '\r')) {
            (*out_length)--;
          }
          return true;
        }
      }
      *out_length = pos - stream_start;
      return true;
    }
  }
  return false;
}

// Repair xref by scanning the entire file for "N G obj" patterns
bool repair_xref(const uint8_t* data, size_t size, Pdf* out_pdf) {
  if (!data || size < 20 || !out_pdf) {
    NANOPDF_LOG_WARN("repair", "Invalid arguments for xref repair (data=%p, size=%zu)",
                     static_cast<const void*>(data), size);
    return false;
  }

  out_pdf->data = data;
  out_pdf->data_size = size;
  out_pdf->swap_endian = !is_system_little_endian();

  // Parse header
  if (size >= 8 && data[0] == '%' && data[1] == 'P' && data[2] == 'D' &&
      data[3] == 'F' && data[4] == '-' && data[6] == '.') {
    out_pdf->version_major = static_cast<int>(data[5] - '0');
    out_pdf->version_minor = static_cast<int>(data[7] - '0');
  }

  // Build a synthetic xref by scanning for "N G obj" patterns
  XRefSection section;
  section.start_object_id = 0;
  uint32_t max_obj = 0;

  const char* begin = reinterpret_cast<const char*>(data);
  const char* end = begin + size;
  const char* ptr = begin;

  while (ptr < end - 5) {
    // Skip non-digits
    if (!std::isdigit(static_cast<unsigned char>(*ptr))) {
      ++ptr;
      continue;
    }

    const char* start = ptr;

    // Parse object number
    uint32_t obj = 0;
    while (ptr < end && std::isdigit(static_cast<unsigned char>(*ptr))) {
      obj = obj * 10 + static_cast<uint32_t>(*ptr - '0');
      ++ptr;
      if (obj > 999999) { ptr = start + 1; goto next_iter; }
    }
    if (ptr >= end || *ptr != ' ') { ptr = start + 1; continue; }
    ++ptr;

    // Parse generation number
    if (ptr >= end || !std::isdigit(static_cast<unsigned char>(*ptr))) {
      ptr = start + 1; continue;
    }
    {
      uint32_t gen = 0;
      while (ptr < end && std::isdigit(static_cast<unsigned char>(*ptr))) {
        gen = gen * 10 + static_cast<uint32_t>(*ptr - '0');
        ++ptr;
        if (gen > 65535) { ptr = start + 1; goto next_iter; }
      }
      if (ptr >= end || *ptr != ' ') { ptr = start + 1; continue; }
      ++ptr;

      // Check for "obj"
      if (ptr + 3 <= end && ptr[0] == 'o' && ptr[1] == 'b' && ptr[2] == 'j') {
        // Found "N G obj" pattern
        uint64_t offset = static_cast<uint64_t>(start - begin);

        // Ensure xref vector is large enough
        if (obj >= section.xrefs.size()) {
          section.xrefs.resize(obj + 1);
        }

        XRef& xref = section.xrefs[obj];
        xref.offset = offset;
        xref.generation = static_cast<uint16_t>(gen);
        xref.use = true;

        if (obj > max_obj) max_obj = obj;

        ptr += 3;  // Skip "obj"
        continue;
      }
    }

    ptr = start + 1;
    continue;
next_iter:
    continue;
  }

  if (max_obj == 0) return false;

  section.num_objectsid = max_obj + 1;
  out_pdf->xref_sections.push_back(std::move(section));

  // Scan for trailer dictionary (search backwards)
  const char trailer_kw[] = "trailer";
  const size_t trailer_len = sizeof(trailer_kw) - 1;
  for (size_t pos = size - trailer_len; pos > 0; --pos) {
    if (std::memcmp(begin + pos, trailer_kw, trailer_len) == 0) {
      uint64_t dict_offset = static_cast<uint64_t>(pos + trailer_len);
      while (dict_offset < size && (data[dict_offset] == ' ' || data[dict_offset] == '\n' ||
             data[dict_offset] == '\r' || data[dict_offset] == '\t'))
        dict_offset++;

      if (dict_offset + 2 < size && data[dict_offset] == '<' && data[dict_offset + 1] == '<') {
        StreamReader sr(data, size, out_pdf->swap_endian);
        Parser parser(sr);
        if (sr.seek_set(dict_offset + 2)) {
          Value trailer_val;
          trailer_val.SetType(Value::DICTIONARY);
          if (parse_dictionary(sr, parser, &trailer_val.dict)) {
            // Extract trailer fields
            auto root_it = trailer_val.dict.find("Root");
            if (root_it != trailer_val.dict.end() &&
                root_it->second.type == Value::REFERENCE) {
              out_pdf->root = root_it->second.ref_object_number;
            }
            auto size_it = trailer_val.dict.find("Size");
            if (size_it != trailer_val.dict.end() &&
                size_it->second.type == Value::NUMBER) {
              out_pdf->size = static_cast<uint32_t>(size_it->second.number);
            }
            auto info_it = trailer_val.dict.find("Info");
            if (info_it != trailer_val.dict.end() &&
                info_it->second.type == Value::REFERENCE) {
              out_pdf->info = info_it->second.ref_object_number;
            }
            out_pdf->trailer = trailer_val.dict;
          }
        }
      }
      break;
    }
    if (pos == 0) break;
  }

  // If no trailer keyword was found (XRef-stream-only PDFs), search for /Root
  // in object dictionaries (XRef stream objects contain trailer-equivalent data)
  if (out_pdf->root == 0) {
    // Search for /Root in any object dictionary
    const char* root_kw = "/Root";
    const size_t root_len = 5;
    for (size_t pos = 0; pos + root_len < size; ++pos) {
      if (std::memcmp(begin + pos, root_kw, root_len) == 0) {
        // Found /Root — try to parse the value after it
        const char* p = begin + pos + root_len;
        // Skip whitespace
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
          p++;
        // Parse "N G R" reference
        if (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
          uint32_t obj = 0;
          while (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
            obj = obj * 10 + static_cast<uint32_t>(*p - '0');
            p++;
          }
          if (p < end && *p == ' ') {
            p++;
            // Skip generation number
            while (p < end && std::isdigit(static_cast<unsigned char>(*p))) p++;
            if (p < end && *p == ' ') p++;
            if (p < end && *p == 'R') {
              out_pdf->root = obj;
              break;
            }
          }
        }
      }
    }
  }

  // Last resort: search for /Type/Catalog or /Type /Catalog to find root
  if (out_pdf->root == 0) {
    const char* patterns[] = {"/Type/Catalog", "/Type /Catalog"};
    const size_t pat_lens[] = {13, 14};
    for (int pi = 0; pi < 2 && out_pdf->root == 0; ++pi) {
      for (size_t pos = 0; pos + pat_lens[pi] <= size; ++pos) {
        if (std::memcmp(begin + pos, patterns[pi], pat_lens[pi]) == 0) {
          // Find which object contains this offset
          const auto& xrefs = out_pdf->xref_sections.back().xrefs;
          for (size_t oi = 0; oi < xrefs.size(); ++oi) {
            if (!xrefs[oi].use) continue;
            uint64_t obj_off = xrefs[oi].offset;
            if (obj_off <= pos && pos - obj_off < 4096) {
              out_pdf->root = static_cast<uint32_t>(oi);
              break;
            }
          }
          if (out_pdf->root != 0) break;
        }
      }
    }
  }

  // Last-ditch: look for a dict containing /Pages (catalog-like object)
  if (out_pdf->root == 0 && !out_pdf->xref_sections.empty()) {
    const char* pages_kw = "/Pages";
    const size_t pages_len = 6;
    for (size_t pos = 0; pos + pages_len < size; ++pos) {
      if (std::memcmp(begin + pos, pages_kw, pages_len) != 0) continue;
      // Check that next char is whitespace or value (not a longer key like /PagesMode)
      char next = (pos + pages_len < size) ? begin[pos + pages_len] : ' ';
      if (next != ' ' && next != '\n' && next != '\r' && next != '\t' &&
          !std::isdigit(static_cast<unsigned char>(next)))
        continue;
      // Find which object contains this offset
      const auto& xrefs = out_pdf->xref_sections.back().xrefs;
      for (size_t oi = 0; oi < xrefs.size(); ++oi) {
        if (!xrefs[oi].use) continue;
        uint64_t obj_off = xrefs[oi].offset;
        if (obj_off <= pos && pos - obj_off < 4096) {
          out_pdf->root = static_cast<uint32_t>(oi);
          break;
        }
      }
      if (out_pdf->root != 0) break;
    }
  }

  if (out_pdf->root == 0) {
    NANOPDF_LOG_WARN("repair", "Repair failed: could not find /Root catalog object");
    return false;
  }

  // Try loading document structure
  return out_pdf->load_document_structure();
}

// parse_pdf with ParseOptions
ParseResult parse_pdf(const uint8_t *addr, const size_t size, Pdf *out_pdf,
                      const ParseOptions& options) {
  // Try normal parsing first
  ParseResult result = parse_pdf(addr, size, out_pdf);
  if (result.success) {
    return result;
  }

  // If auto_repair is enabled, try xref repair
  if (options.auto_repair) {
    // Reset the pdf struct (Pdf is not move-assignable due to std::mutex)
    out_pdf->~Pdf();
    new (out_pdf) Pdf{};
    if (repair_xref(addr, size, out_pdf)) {
      return ParseResult::Ok();
    }
    // Repair also failed; return the original error
  }

  return result;
}

// parse_from_memory with ParseOptions
bool parse_from_memory(const uint8_t *addr, const size_t size, Pdf *out_pdf,
                       const ParseOptions& options) {
  return parse_pdf(addr, size, out_pdf, options).success;
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

// Apply TIFF predictor (horizontal differencing)
bool apply_tiff_predictor(std::vector<uint8_t> &data, const DecodeParams &params,
                          std::string &error) {
  int bytes_per_pixel = (params.bits_per_component * params.colors + 7) / 8;
  int bytes_per_row =
      (params.bits_per_component * params.colors * params.columns + 7) / 8;

  if (data.empty() || bytes_per_row <= 0) {
    return true;  // Nothing to do
  }

  int row_count = static_cast<int>(data.size()) / bytes_per_row;
  if (row_count <= 0) {
    return true;
  }

  // For 8-bit components, apply horizontal differencing
  if (params.bits_per_component == 8) {
    for (int row = 0; row < row_count; row++) {
      size_t row_start = static_cast<size_t>(row) * bytes_per_row;
      // Start from the second pixel and accumulate differences
      for (int col = bytes_per_pixel; col < bytes_per_row; col++) {
        data[row_start + col] += data[row_start + col - bytes_per_pixel];
      }
    }
  }
  // For 16-bit components
  else if (params.bits_per_component == 16) {
    int samples_per_row = params.columns * params.colors;
    for (int row = 0; row < row_count; row++) {
      size_t row_start = static_cast<size_t>(row) * bytes_per_row;
      for (int sample = params.colors; sample < samples_per_row; sample++) {
        size_t offset = row_start + sample * 2;
        size_t prev_offset = row_start + (sample - params.colors) * 2;
        // Big-endian 16-bit
        uint16_t prev = (data[prev_offset] << 8) | data[prev_offset + 1];
        uint16_t curr = (data[offset] << 8) | data[offset + 1];
        uint16_t result = curr + prev;
        data[offset] = (result >> 8) & 0xFF;
        data[offset + 1] = result & 0xFF;
      }
    }
  }

  return true;
}

// Apply PNG predictor row filter
bool apply_predictor(std::vector<uint8_t> &data, const DecodeParams &params,
                     std::string &error) {
  // TIFF predictor (horizontal differencing)
  if (params.predictor == 2) {
    return apply_tiff_predictor(data, params, error);
  }

  if (params.predictor < 10 || params.predictor > 15) {
    return true;  // No predictor or unsupported (predictor 1 = no prediction)
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
      result.kind = ErrorKind::Malformed;
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
      result.kind = ErrorKind::Malformed;
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

#ifdef NANOPDF_USE_LIBDEFLATE
namespace {
// One reusable libdeflate decompressor per thread. libdeflate decompressors
// are cheap to reuse across calls but not safe to share between threads, so a
// thread_local instance fits decode_flate (which may run on multiple threads,
// one Pdf per thread). Freed at thread exit.
libdeflate_decompressor *get_tls_libdeflate_decompressor() {
  struct Holder {
    libdeflate_decompressor *d = nullptr;
    ~Holder() {
      if (d) libdeflate_free_decompressor(d);
    }
  };
  static thread_local Holder holder;
  if (!holder.d) holder.d = libdeflate_alloc_decompressor();
  return holder.d;
}

// Fast path: decode a zlib stream with libdeflate, growing the output buffer
// on INSUFFICIENT_SPACE. Returns true with the bytes in |out| on success;
// returns false (so the caller falls back to the lenient zlib path) on any
// libdeflate error, including streams that decompress past |max_size|.
bool libdeflate_inflate_zlib(const uint8_t *data, size_t size, size_t max_size,
                             std::vector<uint8_t> &out) {
  libdeflate_decompressor *d = get_tls_libdeflate_decompressor();
  if (!d) return false;
  size_t cap = size * 4;
  if (cap < 4096) cap = 4096;
  if (cap > max_size) cap = max_size;
  for (;;) {
    out.resize(cap);
    size_t actual = 0;
    libdeflate_result r =
        libdeflate_zlib_decompress(d, data, size, out.data(), cap, &actual);
    if (r == LIBDEFLATE_SUCCESS) {
      out.resize(actual);
      return true;
    }
    if (r == LIBDEFLATE_INSUFFICIENT_SPACE && cap < max_size) {
      size_t next = cap * 2;
      if (next > max_size) next = max_size;
      if (next == cap) return false;
      cap = next;
      continue;
    }
    return false;  // BAD_DATA, or output exceeds max_size -> use fallback
  }
}
}  // namespace
#endif  // NANOPDF_USE_LIBDEFLATE

DecodedStream decode_flate(const uint8_t *data, size_t size,
                           const DecodeParams &params) {
  DecodedStream result;
  std::vector<uint8_t> output;

  if (size == 0) {
    result.error = "FlateDecode: empty input data";
    result.kind = ErrorKind::Malformed;
    return result;
  }

#ifdef NANOPDF_USE_LIBDEFLATE
  // Fast path: libdeflate one-shot inflate. On any failure (malformed or
  // streaming-only data that the strict decoder rejects, or output over the
  // size cap) fall through to the lenient zlib streaming path below.
  if (libdeflate_inflate_zlib(data, size, kMaxDecodedSize, output)) {
    std::string predictor_error;
    if (!apply_predictor(output, params, predictor_error)) {
      result.error = "FlateDecode: " + predictor_error;
      result.kind = ErrorKind::Malformed;
      return result;
    }
    result.data = std::move(output);
    result.success = true;
    return result;
  }
  output.clear();
#endif

  // Decompress using zlib
  z_stream strm;
  memset(&strm, 0, sizeof(strm));
  strm.next_in = const_cast<Bytef *>(data);
  strm.avail_in = static_cast<uInt>(size);

  if (inflateInit(&strm) != Z_OK) {
    result.error = "FlateDecode: failed to initialize zlib decompressor";
    result.kind = ErrorKind::Malformed;
    return result;
  }

  int ret;
  do {
    // Check for excessive output size
    if (output.size() >= kMaxDecodedSize) {
      inflateEnd(&strm);
      result.error = "FlateDecode: output exceeds maximum size limit (" +
                     std::to_string(kMaxDecodedSize / (1024 * 1024)) + " MB)";
      result.kind = ErrorKind::Malformed;
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
      result.kind = ErrorKind::Malformed;
      return result;
    }
  } while (ret != Z_STREAM_END);

  inflateEnd(&strm);
  output.resize(strm.total_out);

  // Apply predictor if needed
  std::string predictor_error;
  if (!apply_predictor(output, params, predictor_error)) {
    result.error = "FlateDecode: " + predictor_error;
    result.kind = ErrorKind::Malformed;
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
    result.kind = ErrorKind::Malformed;
    return result;
  }

  // Decode data
  if (!decoder.decode(output)) {
    result.error = "LZWDecode: failed to decode data (invalid code sequence)";
    result.kind = ErrorKind::Malformed;
    return result;
  }

  // Apply predictor if needed
  std::string predictor_error;
  if (!apply_predictor(output, params, predictor_error)) {
    result.error = "LZWDecode: " + predictor_error;
    result.kind = ErrorKind::Malformed;
    return result;
  }

  result.data = std::move(output);
  result.success = true;
  return result;
}


DecodedStream decode_jbig2(const uint8_t *data, size_t size,
                           const DecodeParams &params) {
  DecodedStream result;

  // Get globals data if present
  const uint8_t* globalsData = nullptr;
  size_t globalsSize = 0;
  if (!params.jbig2_globals.empty()) {
    globalsData = params.jbig2_globals.data();
    globalsSize = params.jbig2_globals.size();
  }

  // Create JBIG2 decoding context
  jbig2::CJBig2_Context context(globalsData, globalsSize, data, size, false);

  // Decode the JBIG2 stream
  auto decodeResult = context.Decode();

  if (!decodeResult.success) {
    result.success = false;
    result.error = "JBIG2Decode: " + decodeResult.error;
    result.kind = ErrorKind::Malformed;
    return result;
  }

  if (!decodeResult.image || !decodeResult.image->has_data()) {
    result.success = false;
    result.error = "JBIG2Decode: No image data decoded";
    result.kind = ErrorKind::Malformed;
    return result;
  }

  // Convert 1-bit packed bitmap to byte array
  // JBIG2 images are 1-bit per pixel, packed into bytes
  uint32_t width = decodeResult.width;
  uint32_t height = decodeResult.height;
  int32_t stride = decodeResult.image->stride();
  const uint8_t* imgData = decodeResult.image->data();

  // Output as raw 1-bit packed data (same format as the image)
  size_t dataSize = static_cast<size_t>(stride) * height;
  result.data.resize(dataSize);
  std::memcpy(result.data.data(), imgData, dataSize);

  // Store image dimensions in result for later use
  result.width = static_cast<int>(width);
  result.height = static_cast<int>(height);
  result.bits_per_component = 1;
  result.success = true;

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
        result.kind = ErrorKind::Malformed;
        return result;
      }
      for (size_t i = 0; i < count; i++) {
        output.push_back(data[pos++]);
      }
    } else {
      // Repeat the next byte (257-length) times
      if (pos >= size) {
        result.error = "RunLengthDecode: Unexpected end of data";
        result.kind = ErrorKind::Malformed;
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
    result.kind = ErrorKind::Malformed;
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
    result.kind = ErrorKind::Malformed;
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
  std::string temp_dir;
#ifdef _WIN32
  const char* tmp = std::getenv("TEMP");
  if (!tmp) tmp = std::getenv("TMP");
  if (!tmp) tmp = ".";
  temp_dir = tmp;
#else
  temp_dir = "/tmp";
#endif
  std::string temp_template = temp_dir + "/nanopdf_ccitt_XXXXXX";
  std::vector<char> temp_buf(temp_template.begin(), temp_template.end());
  temp_buf.push_back('\0');
  char* temp_path = temp_buf.data();
  int fd = mkstemp(temp_path);
  if (fd < 0) {
    result.success = false;
    result.error = "Failed to create temporary file";
    result.kind = ErrorKind::Malformed;
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
    result.kind = ErrorKind::Malformed;
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
    result.kind = ErrorKind::Malformed;
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
    result.kind = ErrorKind::Malformed;
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
    result.kind = ErrorKind::Malformed;
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

  if (!data || size == 0) {
    result.error = "CCITTFaxDecode: Empty stream";
    result.kind = ErrorKind::Malformed;
    return result;
  }

#if defined(NANOPDF_USE_LIBTIFF)
  // Use libtiff for CCITT decoding if available
  NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Using libtiff decoder");
  return decode_ccittfax_libtiff(data, size, params);
#elif defined(NANOPDF_USE_BUILTIN_CCITT)
  // Built-in fallback decoder (less accurate, for compatibility)
  if (size >= 8) {
    NANOPDF_LOG_TRACE("CCITTFaxDecode", "First bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                      data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
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
        result.kind = ErrorKind::Malformed;
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
        result.kind = ErrorKind::Malformed;
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
            result.kind = ErrorKind::Malformed;
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
          result.kind = ErrorKind::Malformed;
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

  result.success = true;
  return result;
#else
  // Default: PDFium-based CCITT decoder (header-only, BSD licensed)
  NANOPDF_LOG_DEBUG("CCITTFaxDecode", "Using PDFium decoder");
  int width = params.columns > 0 ? params.columns : 1728;
  int height = params.rows;
  if (height <= 0) {
    height = static_cast<int>((size * 8 * 20) / width);
    if (height > 10000) height = 10000;
    if (height < 1) height = 1;
  }

  std::string error;
  if (!ccitt::decode_ccitt_fax(data, size, width, height, params.k,
                               params.end_of_line != 0,
                               params.encoded_byte_align != 0,
                               params.black_is_1 != 0,
                               result.data, &error)) {
    result.error = error.empty() ? "CCITTFaxDecode: Decode failed" : error;
    result.kind = ErrorKind::Malformed;
    return result;
  }
  result.success = true;
  return result;
#endif  // NANOPDF_USE_LIBTIFF || NANOPDF_USE_BUILTIN_CCITT
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
    // Crypt filter - data is passed through as-is here.
    // The actual decryption using the named crypt filter is handled at
    // the stream level in decode_stream() via the /Name param in DecodeParms.
    result.data.assign(data, data + size);
    result.success = true;
    return result;
  }

  result.error = "Unsupported filter type: " + filter_name;
  result.kind = ErrorKind::Unsupported;
  return result;
}

// Helper to generate cache key for decoded streams
static uint64_t make_decoded_stream_cache_key(uint32_t obj_num, uint16_t gen_num,
                                               int image_width = 0, int image_height = 0) {
  // Encode obj_num, gen_num, and optionally hash image dimensions
  uint64_t key = (static_cast<uint64_t>(obj_num) << 32) |
                 (static_cast<uint64_t>(gen_num) << 16);
  // Include image dimensions in key for image-specific decoding
  if (image_width > 0 || image_height > 0) {
    // Use simple hash for dimensions (lower 16 bits)
    key |= static_cast<uint16_t>((image_width * 31 + image_height) & 0xFFFF);
  }
  return key;
}

DecodedStream decode_stream(const Pdf &pdf, const Value &stream_obj,
                           uint32_t obj_num, uint16_t gen_num) {
  DecodedStream result;

  if (stream_obj.type != Value::STREAM) {
    result.error = "decode_stream: not a stream object";
    result.kind = ErrorKind::Malformed;
    return result;
  }

  // Check cache first (only if we have a valid object number)
  uint64_t cache_key = 0;
  if (obj_num > 0) {
    cache_key = make_decoded_stream_cache_key(obj_num, gen_num);
    {
      std::lock_guard<std::mutex> lock(pdf.cache_mutex);
      auto cache_it = pdf.decoded_stream_cache.find(cache_key);
      if (cache_it != pdf.decoded_stream_cache.end()) {
        // Cache hit - return cached result
        const auto& entry = cache_it->second;
        result.data = entry.data;
        result.success = entry.success;
        result.error = entry.error;
        result.width = entry.width;
        result.height = entry.height;
        result.bits_per_component = entry.bits_per_component;

        // Move to back of LRU order
        auto& order = pdf.decoded_stream_cache_order;
        auto it = std::find(order.begin(), order.end(), cache_key);
        if (it != order.end()) {
          order.erase(it);
          order.push_back(cache_key);
        }
        return result;
      }
    }
  }

  // Start with raw stream data
  std::vector<uint8_t> current_data = stream_obj.stream.data;

  // Check if filter chain includes a Crypt filter (handles encryption via /Name)
  bool has_crypt_filter = false;
  std::string crypt_filter_name;
  {
    auto flt_it = stream_obj.stream.dict.find("Filter");
    if (flt_it != stream_obj.stream.dict.end()) {
      auto check_crypt = [&](const std::string& fname, size_t idx) {
        if (fname == "Crypt") {
          has_crypt_filter = true;
          auto dp_it = stream_obj.stream.dict.find("DecodeParms");
          if (dp_it != stream_obj.stream.dict.end()) {
            const Value* dp_val = nullptr;
            if (dp_it->second.type == Value::DICTIONARY) {
              dp_val = &dp_it->second;
            } else if (dp_it->second.type == Value::ARRAY && idx < dp_it->second.array.size()) {
              dp_val = &dp_it->second.array[idx];
            }
            if (dp_val && dp_val->type == Value::DICTIONARY) {
              auto name_it = dp_val->dict.find("Name");
              if (name_it != dp_val->dict.end() && name_it->second.type == Value::NAME) {
                crypt_filter_name = name_it->second.name;
              }
            }
          }
          if (crypt_filter_name.empty()) crypt_filter_name = "Identity";
        }
      };
      if (flt_it->second.type == Value::NAME) {
        check_crypt(flt_it->second.name, 0);
      } else if (flt_it->second.type == Value::ARRAY) {
        for (size_t fi = 0; fi < flt_it->second.array.size(); ++fi) {
          if (flt_it->second.array[fi].type == Value::NAME)
            check_crypt(flt_it->second.array[fi].name, fi);
        }
      }
    }
  }

  // Decrypt stream data if PDF is encrypted
  // Note: Decryption must happen BEFORE decompression
  if (pdf.security.authenticated && !pdf.security.encryption_key.empty()) {
    auto type_it = stream_obj.stream.dict.find("Type");
    bool is_xref = (type_it != stream_obj.stream.dict.end() &&
                    type_it->second.type == Value::NAME &&
                    type_it->second.name == "XRef");
    bool skip_decrypt = is_xref || (has_crypt_filter && crypt_filter_name == "Identity");

    if (!skip_decrypt) {
      NANOPDF_LOG_TRACE("decode_stream", "Decrypting obj %u %u, before: %zu bytes",
                        obj_num, gen_num, current_data.size());
      if (current_data.size() >= 8) {
        NANOPDF_LOG_TRACE("decode_stream", "Pre-decrypt: %02x %02x %02x %02x %02x %02x %02x %02x",
                          current_data[0], current_data[1], current_data[2], current_data[3],
                          current_data[4], current_data[5], current_data[6], current_data[7]);
      }
      std::string effective_filter = has_crypt_filter ? crypt_filter_name : "";
      current_data = pdf.security.decrypt_stream(current_data, obj_num, gen_num, effective_filter);
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
        result.kind = ErrorKind::Malformed;
        return result;
      }
    }
  } else {
    result.error = "decode_stream: Filter must be a name or array, got type " +
                   std::to_string(static_cast<int>(filter_it->second.type));
    result.kind = ErrorKind::Malformed;
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
          result.kind = ErrorKind::Malformed;
          return result;
        }
      }
    } else if (params_it->second.type != Value::NULL_OBJ) {
      result.error = "decode_stream: DecodeParms must be dictionary, array, or null";
      result.kind = ErrorKind::Malformed;
      return result;
    }
  }

  // Pad params_list to match filter count (use defaults for missing)
  while (params_list.size() < filter_names.size()) {
    params_list.push_back(filters::DecodeParams{});
  }

  // Extract JBIG2Globals from DecodeParms for JBIG2Decode filters
  for (size_t i = 0; i < filter_names.size(); ++i) {
    if (filter_names[i] == "JBIG2Decode") {
      // Access raw DecodeParms to find JBIG2Globals indirect reference
      if (params_it != stream_obj.stream.dict.end()) {
        const Value *dp_entry = nullptr;
        if (params_it->second.type == Value::DICTIONARY) {
          dp_entry = &params_it->second;
        } else if (params_it->second.type == Value::ARRAY &&
                   i < params_it->second.array.size() &&
                   params_it->second.array[i].type == Value::DICTIONARY) {
          dp_entry = &params_it->second.array[i];
        }
        if (dp_entry) {
          auto globals_it = dp_entry->dict.find("JBIG2Globals");
          if (globals_it != dp_entry->dict.end() &&
              globals_it->second.type == Value::REFERENCE) {
            ResolvedObject resolved = resolve_reference(
                pdf, globals_it->second.ref_object_number,
                globals_it->second.ref_generation_number);
            if (resolved.success &&
                resolved.value.type == Value::STREAM) {
              DecodedStream globals_decoded = decode_stream(
                  pdf, resolved.value,
                  globals_it->second.ref_object_number,
                  globals_it->second.ref_generation_number);
              if (globals_decoded.success) {
                params_list[i].jbig2_globals =
                    std::move(globals_decoded.data);
              }
            }
          }
        }
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
      result.kind = step_result.kind;
      return result;
    }

    current_data = std::move(step_result.data);
  }

  result.data = std::move(current_data);
  result.success = true;

  // Store in cache (only if we have a valid object number)
  if (obj_num > 0 && cache_key != 0) {
    // Prepare entry outside the lock
    Pdf::DecodedStreamCacheEntry entry;
    entry.data = result.data;  // Copy for cache
    entry.success = result.success;
    entry.error = result.error;
    entry.width = result.width;
    entry.height = result.height;
    entry.bits_per_component = result.bits_per_component;

    std::lock_guard<std::mutex> lock(pdf.cache_mutex);
    // Evict oldest entries if at capacity
    while (pdf.decoded_stream_cache_order.size() >= pdf.decoded_stream_cache_capacity) {
      uint64_t evict = pdf.decoded_stream_cache_order.front();
      pdf.decoded_stream_cache_order.pop_front();
      pdf.decoded_stream_cache.erase(evict);
    }

    pdf.decoded_stream_cache[cache_key] = std::move(entry);
    pdf.decoded_stream_cache_order.push_back(cache_key);
  }

  return result;
}

// Overload with image dimensions for CCITT decoding
DecodedStream decode_stream(const Pdf &pdf, const Value &stream_obj,
                           uint32_t obj_num, uint16_t gen_num,
                           int image_width, int image_height,
                           bool cache_result) {
  DecodedStream result;

  if (stream_obj.type != Value::STREAM) {
    result.error = "decode_stream: not a stream object";
    result.kind = ErrorKind::Malformed;
    return result;
  }

  // Check cache first (only if we have a valid object number)
  uint64_t cache_key = 0;
  if (obj_num > 0 && cache_result) {
    cache_key = make_decoded_stream_cache_key(obj_num, gen_num, image_width, image_height);
    {
      std::lock_guard<std::mutex> lock(pdf.cache_mutex);
      auto cache_it = pdf.decoded_stream_cache.find(cache_key);
      if (cache_it != pdf.decoded_stream_cache.end()) {
        // Cache hit - return cached result
        const auto& entry = cache_it->second;
        result.data = entry.data;
        result.success = entry.success;
        result.error = entry.error;
        result.width = entry.width;
        result.height = entry.height;
        result.bits_per_component = entry.bits_per_component;

        // Move to back of LRU order
        auto& order = pdf.decoded_stream_cache_order;
        auto it = std::find(order.begin(), order.end(), cache_key);
        if (it != order.end()) {
          order.erase(it);
          order.push_back(cache_key);
        }
        return result;
      }
    }
  }

  // Start with raw stream data
  std::vector<uint8_t> current_data = stream_obj.stream.data;

  // Check if filter chain includes a Crypt filter (handles encryption via /Name)
  bool has_crypt_filter = false;
  std::string crypt_filter_name;
  {
    auto flt_it = stream_obj.stream.dict.find("Filter");
    if (flt_it != stream_obj.stream.dict.end()) {
      auto check_crypt = [&](const std::string& fname, size_t idx) {
        if (fname == "Crypt") {
          has_crypt_filter = true;
          auto dp_it = stream_obj.stream.dict.find("DecodeParms");
          if (dp_it != stream_obj.stream.dict.end()) {
            const Value* dp_val = nullptr;
            if (dp_it->second.type == Value::DICTIONARY) {
              dp_val = &dp_it->second;
            } else if (dp_it->second.type == Value::ARRAY && idx < dp_it->second.array.size()) {
              dp_val = &dp_it->second.array[idx];
            }
            if (dp_val && dp_val->type == Value::DICTIONARY) {
              auto name_it = dp_val->dict.find("Name");
              if (name_it != dp_val->dict.end() && name_it->second.type == Value::NAME) {
                crypt_filter_name = name_it->second.name;
              }
            }
          }
          if (crypt_filter_name.empty()) crypt_filter_name = "Identity";
        }
      };
      if (flt_it->second.type == Value::NAME) {
        check_crypt(flt_it->second.name, 0);
      } else if (flt_it->second.type == Value::ARRAY) {
        for (size_t fi = 0; fi < flt_it->second.array.size(); ++fi) {
          if (flt_it->second.array[fi].type == Value::NAME)
            check_crypt(flt_it->second.array[fi].name, fi);
        }
      }
    }
  }

  // Decrypt stream data if PDF is encrypted
  // Note: Decryption must happen BEFORE decompression
  if (pdf.security.authenticated && !pdf.security.encryption_key.empty()) {
    auto type_it = stream_obj.stream.dict.find("Type");
    bool is_xref = (type_it != stream_obj.stream.dict.end() &&
                    type_it->second.type == Value::NAME &&
                    type_it->second.name == "XRef");
    bool skip_decrypt = is_xref || (has_crypt_filter && crypt_filter_name == "Identity");

    if (!skip_decrypt) {
      NANOPDF_LOG_TRACE("decode_stream", "Decrypting obj %u %u, before: %zu bytes",
                        obj_num, gen_num, current_data.size());
      if (current_data.size() >= 8) {
        NANOPDF_LOG_TRACE("decode_stream", "Pre-decrypt: %02x %02x %02x %02x %02x %02x %02x %02x",
                          current_data[0], current_data[1], current_data[2], current_data[3],
                          current_data[4], current_data[5], current_data[6], current_data[7]);
      }
      std::string effective_filter = has_crypt_filter ? crypt_filter_name : "";
      current_data = pdf.security.decrypt_stream(current_data, obj_num, gen_num, effective_filter);
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
        result.kind = ErrorKind::Malformed;
        return result;
      }
    }
  } else {
    result.error = "decode_stream: Filter must be a name or array, got type " +
                   std::to_string(static_cast<int>(filter_it->second.type));
    result.kind = ErrorKind::Malformed;
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
          result.kind = ErrorKind::Malformed;
          return result;
        }
      }
    } else if (params_it->second.type != Value::NULL_OBJ) {
      result.error = "decode_stream: DecodeParms must be dictionary, array, or null";
      result.kind = ErrorKind::Malformed;
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

  // Extract JBIG2Globals from DecodeParms for JBIG2Decode filters
  for (size_t i = 0; i < filter_names.size(); ++i) {
    if (filter_names[i] == "JBIG2Decode") {
      if (params_it != stream_obj.stream.dict.end()) {
        const Value *dp_entry = nullptr;
        if (params_it->second.type == Value::DICTIONARY) {
          dp_entry = &params_it->second;
        } else if (params_it->second.type == Value::ARRAY &&
                   i < params_it->second.array.size() &&
                   params_it->second.array[i].type == Value::DICTIONARY) {
          dp_entry = &params_it->second.array[i];
        }
        if (dp_entry) {
          auto globals_it = dp_entry->dict.find("JBIG2Globals");
          if (globals_it != dp_entry->dict.end() &&
              globals_it->second.type == Value::REFERENCE) {
            ResolvedObject resolved = resolve_reference(
                pdf, globals_it->second.ref_object_number,
                globals_it->second.ref_generation_number);
            if (resolved.success &&
                resolved.value.type == Value::STREAM) {
              DecodedStream globals_decoded = decode_stream(
                  pdf, resolved.value,
                  globals_it->second.ref_object_number,
                  globals_it->second.ref_generation_number);
              if (globals_decoded.success) {
                params_list[i].jbig2_globals =
                    std::move(globals_decoded.data);
              }
            }
          }
        }
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
      result.kind = step_result.kind;
      return result;
    }

    current_data = std::move(step_result.data);
  }

  result.data = std::move(current_data);
  result.success = true;

  // Store in cache (only if we have a valid object number)
  if (obj_num > 0 && cache_key != 0 && cache_result) {
    // Prepare entry outside the lock
    Pdf::DecodedStreamCacheEntry entry;
    entry.data = result.data;  // Copy for cache
    entry.success = result.success;
    entry.error = result.error;
    entry.width = result.width;
    entry.height = result.height;
    entry.bits_per_component = result.bits_per_component;

    std::lock_guard<std::mutex> lock(pdf.cache_mutex);
    // Evict oldest entries if at capacity
    while (pdf.decoded_stream_cache_order.size() >= pdf.decoded_stream_cache_capacity) {
      uint64_t evict = pdf.decoded_stream_cache_order.front();
      pdf.decoded_stream_cache_order.pop_front();
      pdf.decoded_stream_cache.erase(evict);
    }

    pdf.decoded_stream_cache[cache_key] = std::move(entry);
    pdf.decoded_stream_cache_order.push_back(cache_key);
  }

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

namespace {

std::string revision_bytes_to_hex(const uint8_t* data, size_t len) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = kHex[(data[i] >> 4) & 0x0f];
    out[i * 2 + 1] = kHex[data[i] & 0x0f];
  }
  return out;
}

std::string revision_md5(const uint8_t* data, size_t len) {
  uint8_t digest[16];
  crypto::MD5::hash(data, len, digest);
  return revision_bytes_to_hex(digest, sizeof(digest));
}

std::string revision_sha256(const uint8_t* data, size_t len) {
  uint8_t digest[32];
  crypto::SHA256::hash(data, len, digest);
  return revision_bytes_to_hex(digest, sizeof(digest));
}

std::vector<size_t> find_revision_eof_markers(const uint8_t* data, size_t size) {
  std::vector<size_t> markers;
  const char eof_marker[] = "%%EOF";
  const size_t marker_len = sizeof(eof_marker) - 1;

  for (size_t i = 0; i + marker_len <= size; ++i) {
    if (std::memcmp(data + i, eof_marker, marker_len) == 0) {
      size_t end = i + marker_len;
      while (end < size && (data[end] == '\r' || data[end] == '\n')) {
        ++end;
      }
      markers.push_back(end);
      i = end;
    }
  }

  return markers;
}

size_t find_revision_startxref_before(const uint8_t* data, size_t eof_offset) {
  const char startxref[] = "startxref";
  const size_t startxref_len = sizeof(startxref) - 1;
  if (eof_offset < startxref_len) return 0;

  const size_t search_start = eof_offset > 1024 ? eof_offset - 1024 : 0;
  const size_t search_end = eof_offset - startxref_len;
  for (size_t i = search_end;; --i) {
    if (std::memcmp(data + i, startxref, startxref_len) == 0) {
      size_t pos = i + startxref_len;
      while (pos < eof_offset &&
             (data[pos] == ' ' || data[pos] == '\r' ||
              data[pos] == '\n' || data[pos] == '\t')) {
        ++pos;
      }

      size_t xref_offset = 0;
      while (pos < eof_offset && data[pos] >= '0' && data[pos] <= '9') {
        xref_offset = xref_offset * 10 + static_cast<size_t>(data[pos] - '0');
        ++pos;
      }
      return xref_offset;
    }
    if (i == search_start) break;
  }

  return 0;
}

std::map<uint32_t, std::pair<uint64_t, bool>> parse_revision_xref_entries(
    const Pdf& pdf, const uint8_t* data, size_t size, size_t xref_offset) {
  std::map<uint32_t, std::pair<uint64_t, bool>> entries;
  if (xref_offset >= size) return entries;

  const char xref_keyword[] = "xref";
  if (xref_offset + 4 <= size &&
      std::memcmp(data + xref_offset, xref_keyword, 4) == 0) {
    size_t pos = xref_offset + 4;
    while (pos < size && (data[pos] == ' ' || data[pos] == '\r' ||
                          data[pos] == '\n' || data[pos] == '\t')) {
      ++pos;
    }

    while (pos < size) {
      if (pos + 7 <= size && std::memcmp(data + pos, "trailer", 7) == 0) {
        break;
      }

      uint32_t start_obj = 0;
      while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        start_obj = start_obj * 10u + static_cast<uint32_t>(data[pos] - '0');
        ++pos;
      }
      while (pos < size && (data[pos] == ' ' || data[pos] == '\t')) ++pos;

      uint32_t count = 0;
      while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        count = count * 10u + static_cast<uint32_t>(data[pos] - '0');
        ++pos;
      }
      while (pos < size && data[pos] != '\n') ++pos;
      if (pos < size) ++pos;

      for (uint32_t i = 0; i < count && pos + 20 <= size; ++i) {
        uint64_t offset = 0;
        for (int j = 0; j < 10 && pos < size; ++j, ++pos) {
          if (data[pos] >= '0' && data[pos] <= '9') {
            offset = offset * 10 + static_cast<uint64_t>(data[pos] - '0');
          }
        }
        if (pos + 7 >= size) break;
        pos += 7;

        bool in_use = false;
        if (pos < size) {
          in_use = data[pos] == 'n';
          ++pos;
        }
        while (pos < size && data[pos] != '\n') ++pos;
        if (pos < size) ++pos;

        entries[start_obj + i] = std::make_pair(offset, in_use);
      }
    }
  } else if (data[xref_offset] >= '0' && data[xref_offset] <= '9') {
    size_t pos = xref_offset;
    uint32_t obj_num = 0;
    while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
      obj_num = obj_num * 10u + static_cast<uint32_t>(data[pos] - '0');
      ++pos;
    }
    while (pos < size && (data[pos] == ' ' || data[pos] == '\t')) ++pos;
    uint16_t gen_num = 0;
    while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
      gen_num = static_cast<uint16_t>(gen_num * 10u +
                                      static_cast<uint16_t>(data[pos] - '0'));
      ++pos;
    }

    ResolvedObject resolved = resolve_reference(pdf, obj_num, gen_num);
    if (!resolved.success || resolved.value.type != Value::STREAM) {
      return entries;
    }

    auto type_it = resolved.value.stream.dict.find("Type");
    if (type_it != resolved.value.stream.dict.end() &&
        (type_it->second.type != Value::NAME || type_it->second.name != "XRef")) {
      return entries;
    }

    auto w_it = resolved.value.stream.dict.find("W");
    if (w_it == resolved.value.stream.dict.end() ||
        w_it->second.type != Value::ARRAY ||
        w_it->second.array.size() != 3) {
      return entries;
    }

    int widths[3] = {0, 0, 0};
    for (size_t i = 0; i < 3; ++i) {
      const Value& item = w_it->second.array[i];
      if (item.type != Value::NUMBER) return entries;
      int width = static_cast<int>(item.number);
      if (width < 0 || width > 8) return entries;
      widths[i] = width;
    }

    auto size_it = resolved.value.stream.dict.find("Size");
    if (size_it == resolved.value.stream.dict.end() ||
        size_it->second.type != Value::NUMBER) {
      return entries;
    }
    uint64_t doc_size = static_cast<uint64_t>(size_it->second.number);

    std::vector<uint64_t> index_pairs;
    auto index_it = resolved.value.stream.dict.find("Index");
    if (index_it != resolved.value.stream.dict.end() &&
        index_it->second.type == Value::ARRAY) {
      const auto& arr = index_it->second.array;
      if (arr.size() % 2 != 0) return entries;
      for (const Value& v : arr) {
        if (v.type != Value::NUMBER) return entries;
        index_pairs.push_back(static_cast<uint64_t>(v.number));
      }
    } else {
      index_pairs = {0, doc_size};
    }

    DecodedStream decoded = decode_stream(pdf, resolved.value);
    if (!decoded.success) return entries;

    size_t spos = 0;
    const std::vector<uint8_t>& bytes = decoded.data;
    auto read_field = [&](int width) -> uint64_t {
      uint64_t value = 0;
      for (int i = 0; i < width; ++i) {
        if (spos >= bytes.size()) return uint64_t{0};
        value = (value << 8) | static_cast<uint64_t>(bytes[spos++]);
      }
      return value;
    };

    for (size_t i = 0; i + 1 < index_pairs.size(); i += 2) {
      uint64_t obj = index_pairs[i];
      uint64_t count = index_pairs[i + 1];
      if (count > doc_size) break;

      for (uint64_t j = 0; j < count; ++j, ++obj) {
        uint64_t type_field = widths[0] ? read_field(widths[0]) : 1;
        uint64_t field2 = widths[1] ? read_field(widths[1]) : 0;
        uint64_t field3 = widths[2] ? read_field(widths[2]) : 0;

        if (type_field == 0) {
          entries[static_cast<uint32_t>(obj)] = std::make_pair(field2, false);
        } else if (type_field == 1) {
          entries[static_cast<uint32_t>(obj)] = std::make_pair(field2, true);
        } else if (type_field == 2) {
          entries[static_cast<uint32_t>(obj)] =
              std::make_pair((field2 << 16) | field3, true);
        }
      }
    }
  }

  return entries;
}

size_t find_revision_prev_in_trailer(const Pdf& pdf, const uint8_t* data,
                                     size_t size, size_t xref_offset) {
  if (xref_offset >= size) return 0;

  if (data[xref_offset] >= '0' && data[xref_offset] <= '9') {
    size_t pos = xref_offset;
    uint32_t obj_num = 0;
    while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
      obj_num = obj_num * 10u + static_cast<uint32_t>(data[pos] - '0');
      ++pos;
    }
    while (pos < size && (data[pos] == ' ' || data[pos] == '\t')) ++pos;
    uint16_t gen_num = 0;
    while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
      gen_num = static_cast<uint16_t>(gen_num * 10u +
                                      static_cast<uint16_t>(data[pos] - '0'));
      ++pos;
    }

    ResolvedObject resolved = resolve_reference(pdf, obj_num, gen_num);
    if (resolved.success && resolved.value.type == Value::STREAM) {
      auto prev_it = resolved.value.stream.dict.find("Prev");
      if (prev_it != resolved.value.stream.dict.end() &&
          prev_it->second.type == Value::NUMBER) {
        return static_cast<size_t>(prev_it->second.number);
      }
    }
    return 0;
  }

  size_t trailer_pos = 0;
  for (size_t i = xref_offset; i + 7 < size; ++i) {
    if (std::memcmp(data + i, "trailer", 7) == 0) {
      trailer_pos = i;
      break;
    }
  }
  if (trailer_pos == 0) return 0;

  for (size_t i = trailer_pos; i + 5 < size && i < trailer_pos + 1024; ++i) {
    if (std::memcmp(data + i, "/Prev", 5) == 0) {
      size_t pos = i + 5;
      while (pos < size && (data[pos] == ' ' || data[pos] == '\r' ||
                            data[pos] == '\n' || data[pos] == '\t')) {
        ++pos;
      }

      size_t prev_offset = 0;
      while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        prev_offset = prev_offset * 10 + static_cast<size_t>(data[pos] - '0');
        ++pos;
      }
      return prev_offset;
    }

    if (data[i] == '>' && i + 1 < size && data[i + 1] == '>') break;
  }

  return 0;
}

std::vector<uint32_t> set_to_sorted_vector(const std::set<uint32_t>& values) {
  return std::vector<uint32_t>(values.begin(), values.end());
}

std::string classify_revision_object(const Pdf& pdf, uint32_t obj_num) {
  ResolvedObject resolved = resolve_reference(pdf, obj_num, 0);
  if (!resolved.success) return "unknown";

  const Dictionary* dict = nullptr;
  if (resolved.value.type == Value::DICTIONARY) {
    dict = &resolved.value.dict;
  } else if (resolved.value.type == Value::STREAM) {
    dict = &resolved.value.stream.dict;
  } else {
    return "other";
  }

  auto ft_it = dict->find("FT");
  if (ft_it != dict->end() && ft_it->second.type == Value::NAME) {
    if (ft_it->second.name == "Sig") return "signature";
    return "form";
  }

  auto type_it = dict->find("Type");
  if (type_it != dict->end() && type_it->second.type == Value::NAME) {
    if (type_it->second.name == "Sig") return "signature";
    if (type_it->second.name == "Annot") return "annotation";
    if (type_it->second.name == "Metadata" || type_it->second.name == "Catalog" ||
        type_it->second.name == "Page" || type_it->second.name == "Pages") {
      return "document";
    }
  }

  auto subtype_it = dict->find("Subtype");
  if (subtype_it != dict->end() && subtype_it->second.type == Value::NAME) {
    if (subtype_it->second.name == "Widget") return "form";
    if (!subtype_it->second.name.empty()) return "annotation";
  }

  if (dict->find("Producer") != dict->end() ||
      dict->find("ModDate") != dict->end() ||
      dict->find("CreationDate") != dict->end() ||
      dict->find("Title") != dict->end() ||
      dict->find("Author") != dict->end()) {
    return "metadata";
  }

  return "unknown";
}

bool object_class_allowed_by_docmdp(const std::string& cls, int permissions) {
  if (permissions == 1) return false;
  if (permissions == 2) return cls == "form" || cls == "signature";
  if (permissions == 3) {
    return cls == "form" || cls == "signature" || cls == "annotation";
  }
  return false;
}

std::string docmdp_permission_label(int permissions) {
  switch (permissions) {
    case 1: return "no_changes";
    case 2: return "form_fill_and_sign";
    case 3: return "form_fill_sign_annotate";
    default: return "unknown";
  }
}

void analyze_docmdp_revision(const Pdf& pdf, RevisionHistoryEntry& rev,
                             const SignatureField& cert_sig) {
  rev.has_docmdp = true;
  rev.mdp_permissions = cert_sig.mdp_permissions;
  rev.docmdp_allowed = true;
  rev.docmdp_status = "allowed";

  if (rev.added_objects.empty() && rev.modified_objects.empty() &&
      rev.deleted_objects.empty()) {
    rev.docmdp_status = "no_object_changes";
    return;
  }

  if (cert_sig.mdp_permissions <= 0) {
    rev.docmdp_allowed = false;
    rev.docmdp_status = "unknown_permissions";
    rev.docmdp_violations.push_back("DocMDP permission level is missing");
    return;
  }

  if (cert_sig.mdp_permissions == 1) {
    rev.docmdp_allowed = false;
    rev.docmdp_status = "disallowed";
    rev.docmdp_violations.push_back(
        "DocMDP P=1 allows no changes after certification");
    return;
  }

  auto check_obj = [&](uint32_t obj_num, const char* action) {
    std::string cls = classify_revision_object(pdf, obj_num);
    if (!object_class_allowed_by_docmdp(cls, cert_sig.mdp_permissions)) {
      rev.docmdp_allowed = false;
      std::ostringstream ss;
      ss << "object " << obj_num << " " << action << " classified as "
         << cls << " is not allowed by DocMDP P=" << cert_sig.mdp_permissions
         << " (" << docmdp_permission_label(cert_sig.mdp_permissions) << ")";
      rev.docmdp_violations.push_back(ss.str());
    }
  };

  for (uint32_t obj : rev.added_objects) check_obj(obj, "added");
  for (uint32_t obj : rev.modified_objects) check_obj(obj, "modified");
  for (uint32_t obj : rev.deleted_objects) {
    rev.docmdp_allowed = false;
    std::ostringstream ss;
    ss << "object " << obj << " deleted after certification";
    rev.docmdp_violations.push_back(ss.str());
  }

  if (!rev.docmdp_allowed) {
    rev.docmdp_status = "disallowed";
  }
}

}  // namespace

RevisionHistory detect_revision_history(const Pdf& pdf) {
  RevisionHistory history;
  const uint8_t* data = pdf.data;
  size_t size = pdf.data_size;
  if (!data || size == 0) return history;

  std::vector<size_t> eof_markers = find_revision_eof_markers(data, size);
  if (eof_markers.empty()) return history;

  size_t prev_end = 0;
  std::map<uint32_t, std::pair<uint64_t, bool>> cumulative_objects;

  for (size_t i = 0; i < eof_markers.size(); ++i) {
    RevisionHistoryEntry rev;
    rev.revision_number = i + 1;
    rev.start_offset = prev_end;
    rev.end_offset = eof_markers[i];
    rev.size_bytes = rev.end_offset - rev.start_offset;

    if (rev.end_offset > 0 && rev.end_offset <= size) {
      rev.md5_hash = revision_md5(data, rev.end_offset);
      rev.sha256_hash = revision_sha256(data, rev.end_offset);
    }

    if (eof_markers[i] >= 10) {
      rev.xref_offset = find_revision_startxref_before(data, eof_markers[i]);
    }

    if (rev.xref_offset > 0 && rev.xref_offset < size) {
      rev.prev_xref_offset =
          find_revision_prev_in_trailer(pdf, data, size, rev.xref_offset);
      auto entries = parse_revision_xref_entries(pdf, data, size, rev.xref_offset);

      std::set<uint32_t> added;
      std::set<uint32_t> modified;
      std::set<uint32_t> deleted;
      for (const auto& entry : entries) {
        uint32_t obj_num = entry.first;
        bool in_use = entry.second.second;
        auto prev_it = cumulative_objects.find(obj_num);
        if (prev_it == cumulative_objects.end()) {
          if (in_use && obj_num != 0) added.insert(obj_num);
        } else {
          bool was_in_use = prev_it->second.second;
          uint64_t old_offset = prev_it->second.first;
          uint64_t new_offset = entry.second.first;

          if (in_use && !was_in_use) {
            added.insert(obj_num);
          } else if (!in_use && was_in_use) {
            deleted.insert(obj_num);
          } else if (in_use && was_in_use && old_offset != new_offset) {
            modified.insert(obj_num);
          }
        }
        cumulative_objects[obj_num] = entry.second;
      }

      rev.added_objects = set_to_sorted_vector(added);
      rev.modified_objects = set_to_sorted_vector(modified);
      rev.deleted_objects = set_to_sorted_vector(deleted);
    }

    history.revisions.push_back(std::move(rev));
    prev_end = eof_markers[i];
  }

  if (!history.revisions.empty()) {
    history.current_md5 = history.revisions.back().md5_hash;
    history.current_sha256 = history.revisions.back().sha256_hash;
  }

  std::vector<SignatureField> signatures = pdf.catalog.signature_fields;
  if (signatures.empty()) {
    Pdf& mutable_pdf = const_cast<Pdf&>(pdf);
    mutable_pdf.parse_signature_fields();
    signatures = mutable_pdf.catalog.signature_fields;
  }

  const SignatureField* certification = nullptr;
  size_t certification_coverage_end = 0;
  for (const auto& sig : signatures) {
    if ((!sig.is_signed && !sig.signature_present) || sig.byte_range.size() != 4) {
      continue;
    }

    uint64_t coverage_end64 = sig.byte_range[2] + sig.byte_range[3];
    size_t coverage_end =
        coverage_end64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())
            ? size
            : static_cast<size_t>(coverage_end64);

    RevisionHistoryEntry* best = nullptr;
    for (auto& rev : history.revisions) {
      if (rev.end_offset >= coverage_end) {
        if (!best || rev.end_offset < best->end_offset) {
          best = &rev;
        }
      }
    }

    if (best) {
      best->associated_signature = sig.name;
      best->modified_after_signature = coverage_end < size;
      SignatureValidationResult validation = validate_signature(pdf, sig);
      if (!validation.signer_name.empty()) best->signer_name = validation.signer_name;
      if (!validation.signing_time.empty()) {
        best->signing_time = validation.signing_time;
      } else if (!sig.signing_date.empty()) {
        best->signing_time = sig.signing_date;
      }
    }

    if (sig.is_certification_signature &&
        (!certification || coverage_end > certification_coverage_end)) {
      certification = &sig;
      certification_coverage_end = coverage_end;
    }
  }

  if (certification) {
    for (auto& rev : history.revisions) {
      if (rev.start_offset >= certification_coverage_end) {
        rev.modified_after_signature = true;
        analyze_docmdp_revision(pdf, rev, *certification);
      }
    }
  }

  return history;
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

  *out_number = nanopdf::stod_or(token);

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

  // Check object_cache under lock
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto cached = object_cache.find(key);
    if (cached != object_cache.end()) {
      result.success = true;
      result.value = cached->second;
      return result;
    }
  }

  // Check if we need to build the offset cache.
  // Set object_offsets_failed optimistically to prevent concurrent builds,
  // then correct to object_offsets_built on success.
  {
    bool need_build = false;
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      if (!object_offsets_built && !object_offsets_failed) {
        need_build = true;
        object_offsets_failed = true;  // Prevent other threads from also building
      }
    }
    // build_object_offset_cache accesses caches internally (and may call
    // decode_stream), so we must not hold the lock across this call.
    if (need_build) {
      if (build_object_offset_cache()) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        object_offsets_built = true;
        object_offsets_failed = false;
      }
      // If build failed, object_offsets_failed remains true (set above)
    }
  }

  uint64_t body_offset = 0;
  bool have_offset = false;

  // Read from offset cache under lock
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
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
  }

  if (!have_offset) {
    // Check object_stream_entries under lock, copy out the entry info
    bool found_stream_entry = false;
    uint32_t stream_obj_num = 0;
    uint32_t stream_index = 0;
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      auto stream_it = object_stream_entries.find(key);
      if (stream_it == object_stream_entries.end() && gen_num != 0) {
        uint64_t zero_key = static_cast<uint64_t>(obj_num) << 32;
        stream_it = object_stream_entries.find(zero_key);
      }
      if (stream_it != object_stream_entries.end()) {
        found_stream_entry = true;
        stream_obj_num = stream_it->second.first;
        stream_index = stream_it->second.second;
      }
    }

    if (found_stream_entry) {
      Value from_stream;
      // load_object_from_stream may recursively call load_object, so no lock held here
      if (load_object_from_stream(obj_num, gen_num, stream_obj_num,
                                  stream_index, &from_stream)) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        object_cache[key] = from_stream;
        result.success = true;
        result.value = std::move(from_stream);
        return result;
      }
      result.success = false;
      result.error = "Failed to load object from object stream";
      result.kind = ErrorKind::Malformed;
      return result;
    }

    if (!find_indirect_object_body_offset(*this, obj_num, gen_num, &body_offset)) {
      result.success = false;
      result.error = "Failed to locate object";
      result.kind = ErrorKind::Malformed;
      NANOPDF_LOG_WARN("load_object", "Failed to locate object %u %u", obj_num, gen_num);
      return result;
    }
  }

  StreamReader sr(data, data_size, swap_endian);
  Parser parser(sr);

  if (!sr.seek_set(body_offset)) {
    result.success = false;
    result.error = "Invalid object offset";
    result.kind = ErrorKind::Malformed;
    return result;
  }

  if (!parser.skip_whitespace(sr)) {
    result.success = false;
    result.error = "Failed to prepare object stream";
    result.kind = ErrorKind::Malformed;
    return result;
  }

  if (!parse_object(sr, parser, &result.value)) {
    result.success = false;
    result.kind = ErrorKind::Malformed;
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
    result.kind = ErrorKind::Malformed;
    return result;
  }

  if (!parser.consume_keyword(sr, "endobj")) {
    result.success = false;
    result.error = "Missing endobj for object";
    result.kind = ErrorKind::Malformed;
    return result;
  }

  result.success = true;
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    object_cache[key] = result.value;
  }
  return result;
}

bool Pdf::build_object_offset_cache() const {
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    object_offsets.clear();
    object_stream_entries.clear();
    object_stream_cache.clear();
    object_stream_cache_order.clear();
  }

  if (!data || data_size == 0) {
    NANOPDF_LOG_WARN("xref", "No data for xref offset cache");
    set_error(ErrorKind::Malformed, "no data for xref offset cache");
    return false;
  }

  const char* begin = reinterpret_cast<const char*>(data);
  const char* end = begin + data_size;
  const char keyword[] = "startxref";
  const size_t keyword_len = sizeof(keyword) - 1;

  if (data_size < keyword_len) {
    NANOPDF_LOG_WARN("xref", "PDF data too small for xref (%zu bytes)", data_size);
    set_error(ErrorKind::Malformed, "PDF data too small for xref");
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
    NANOPDF_LOG_WARN("xref", "startxref keyword not found");
    set_error(ErrorKind::Malformed, "startxref keyword not found");
    return false;
  }

  const char* cursor = startxref_pos + keyword_len;
  while (cursor < end && is_whitespace_char(*cursor)) {
    cursor++;
  }
  if (cursor >= end) {
    NANOPDF_LOG_WARN("xref", "Unexpected end of data after startxref");
    set_error(ErrorKind::Malformed, "unexpected end of data after startxref");
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
    NANOPDF_LOG_WARN("xref", "startxref offset beyond file size (offset=%llu, size=%zu)",
                     (unsigned long long)initial_xref, data_size);
    set_error(ErrorKind::Malformed, "startxref offset beyond file size");
    return false;
  }

  // Handle truncated linearized PDFs: when startxref is 0, the file is likely
  // a partial download of a linearized PDF. The first xref stream is located
  // near the beginning of the file (after the linearization dict object).
  // Scan for /Type /XRef to find it.
  if (initial_xref == 0) {
    // Check if this is a linearized PDF
    size_t scan_limit = data_size < 4096 ? data_size : 4096;
    bool is_linearized = false;
    for (size_t i = 0; i + 12 < scan_limit; ++i) {
      if (std::memcmp(begin + i, "/Linearized", 11) == 0) {
        is_linearized = true;
        break;
      }
    }

    if (is_linearized) {
      // Scan for the first xref stream object: look for /Type /XRef or
      // /Type/XRef within the first portion of the file
      size_t xref_scan_limit = data_size < 65536 ? data_size : 65536;
      for (size_t i = 0; i + 10 < xref_scan_limit; ++i) {
        if (std::memcmp(begin + i, "/Type", 5) != 0) continue;
        size_t j = i + 5;
        // Skip optional whitespace
        while (j < xref_scan_limit && (begin[j] == ' ' || begin[j] == '\t' ||
               begin[j] == '\n' || begin[j] == '\r'))
          j++;
        if (j + 4 >= xref_scan_limit) continue;
        if (std::memcmp(begin + j, "/XRef", 5) != 0) continue;

        // Found /Type /XRef — now find the "N G obj" that contains it by
        // scanning backwards for a digit sequence matching "N G obj"
        for (size_t k = i; k > 0; --k) {
          if (begin[k] == 'o' && k + 2 < data_size &&
              begin[k + 1] == 'b' && begin[k + 2] == 'j') {
            // Walk back to find the start of "N G obj"
            size_t obj_start = k;
            if (obj_start > 0 && begin[obj_start - 1] == ' ') {
              obj_start--;
              // Skip generation number backwards
              while (obj_start > 0 &&
                     std::isdigit(static_cast<unsigned char>(begin[obj_start - 1])))
                obj_start--;
              if (obj_start > 0 && begin[obj_start - 1] == ' ') {
                obj_start--;
                // Skip object number backwards
                while (obj_start > 0 &&
                       std::isdigit(static_cast<unsigned char>(begin[obj_start - 1])))
                  obj_start--;
              }
            }
            initial_xref = obj_start;
            break;
          }
        }
        break;
      }
    }
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
      std::lock_guard<std::mutex> lock(cache_mutex);
      // The xref chain is walked newest-first (startxref, then each /Prev), so
      // the first offset seen for an object is the most recent one. Incremental
      // updates redefine objects in later sections that appear EARLIER in this
      // walk; keep the first (newest) and never let an older /Prev section
      // overwrite it. Otherwise filled form fields, signatures and other
      // incremental edits silently revert to their pre-update objects.
      object_offsets.emplace(key, body_offset);
    }
  };

  auto update_trailer = [&](const Dictionary& dict) {
    Pdf* self = const_cast<Pdf*>(this);

    // Merge trailer dict: first-seen keys win (most recent trailer, from
    // startxref, is processed first and takes precedence per PDF spec 7.5.5).
    if (self->trailer.empty()) {
      self->trailer = dict;
    } else {
      for (const auto& kv : dict) {
        if (self->trailer.find(kv.first) == self->trailer.end()) {
          self->trailer[kv.first] = kv.second;
        }
      }
    }

    // Size: use the maximum seen across all trailers
    auto size_it_local = dict.find("Size");
    if (size_it_local != dict.end() && size_it_local->second.type == Value::NUMBER) {
      uint32_t new_size = static_cast<uint32_t>(size_it_local->second.number);
      if (new_size > self->size) {
        self->size = new_size;
      }
    }

    // Root, Info, Encrypt, ID: first-seen wins (most recent trailer)
    if (self->root == 0) {
      auto root_it = dict.find("Root");
      if (root_it != dict.end() && root_it->second.type == Value::REFERENCE) {
        self->root = root_it->second.ref_object_number;
      }
    }

    if (self->info == 0) {
      auto info_it = dict.find("Info");
      if (info_it != dict.end() && info_it->second.type == Value::REFERENCE) {
        self->info = info_it->second.ref_object_number;
      }
    }

    if (self->encrypt == 0) {
      auto encrypt_it = dict.find("Encrypt");
      if (encrypt_it != dict.end() && encrypt_it->second.type == Value::REFERENCE) {
        self->encrypt = encrypt_it->second.ref_object_number;
      }
    }

    auto prev_it_root = dict.find("Prev");
    if (prev_it_root != dict.end() && prev_it_root->second.type == Value::NUMBER) {
      self->prev = static_cast<uint32_t>(prev_it_root->second.number);
    }

    if (self->id.empty()) {
      auto id_it = dict.find("ID");
      if (id_it != dict.end() && id_it->second.type == Value::ARRAY &&
          !id_it->second.array.empty() &&
          id_it->second.array[0].type == Value::STRING) {
        self->id = id_it->second.array[0].str;
      }
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
        {
          std::lock_guard<std::mutex> lock(cache_mutex);
          any_entries = any_entries || !object_offsets.empty();
        }
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
            {
              std::lock_guard<std::mutex> lock(cache_mutex);
              object_stream_entries[key] = {stream_object, index};
            }
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

  bool need_fallback = false;
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    need_fallback = !any_entries && object_offsets.empty() && object_stream_entries.empty();
  }
  if (need_fallback) {
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
      {
        std::lock_guard<std::mutex> lock(cache_mutex);
        object_offsets[key] = static_cast<uint64_t>(body_ptr - begin);
      }
      any_entries = true;
      ptr = body_ptr;
    }

    if (root == 0) {
      std::lock_guard<std::mutex> lock(cache_mutex);
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

    if (size == 0) {
      std::lock_guard<std::mutex> lock(cache_mutex);
      if (!object_offsets.empty()) {
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
  }

  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    return any_entries && (!object_offsets.empty() || !object_stream_entries.empty());
  }
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
    NANOPDF_LOG_WARN("obj_stream", "Stream object %u not found or not a stream",
                     stream_object_number);
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

  // Lambda to touch LRU order for object_stream_cache (must be called with cache_mutex held)
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

  // Check object_stream_cache; if miss, decode outside the lock, then insert.
  // Copy the data locally so we don't hold references into the cache across
  // unlocked regions.
  std::vector<uint8_t> local_stream_data;
  {
    bool cache_hit = false;
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      auto cache_it = object_stream_cache.find(stream_object_number);
      if (cache_it != object_stream_cache.end()) {
        cache_hit = true;
        local_stream_data = cache_it->second;  // copy
        touch_cache_entry(stream_object_number, false);
      }
    }
    if (!cache_hit) {
      // decode_stream accesses decoded_stream_cache, so no lock held here
      DecodedStream decoded = decode_stream(*this, stream_obj.value,
                                            stream_object_number, 0);
      if (!decoded.success) {
        NANOPDF_LOG_WARN("obj_stream", "Failed to decode object stream %u: %s",
                         stream_object_number, decoded.error.c_str());
        return false;
      }
      local_stream_data = decoded.data;  // keep a local copy
      {
        std::lock_guard<std::mutex> lock(cache_mutex);
        object_stream_cache.emplace(stream_object_number, std::move(decoded.data));
        touch_cache_entry(stream_object_number, true);
      }
    }
  }
  const std::vector<uint8_t>& data = local_stream_data;
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
    NANOPDF_LOG_WARN("obj_stream",
                     "Object ID mismatch in stream %u: expected %u, got %u at index %u",
                     stream_object_number, object_number, object_id_from_stream, index);
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
  std::lock_guard<std::mutex> lock(cache_mutex);
  object_stream_cache_capacity = capacity;

  while (object_stream_cache_order.size() > object_stream_cache_capacity) {
    uint32_t evict = object_stream_cache_order.front();
    object_stream_cache_order.pop_front();
    object_stream_cache.erase(evict);
  }
}

void Pdf::set_decoded_stream_cache_capacity(size_t capacity) const {
  if (capacity == 0) {
    capacity = 1;
  }
  std::lock_guard<std::mutex> lock(cache_mutex);
  decoded_stream_cache_capacity = capacity;

  while (decoded_stream_cache_order.size() > decoded_stream_cache_capacity) {
    uint64_t evict = decoded_stream_cache_order.front();
    decoded_stream_cache_order.pop_front();
    decoded_stream_cache.erase(evict);
  }
}

void Pdf::clear_decoded_stream_cache() const {
  std::lock_guard<std::mutex> lock(cache_mutex);
  decoded_stream_cache.clear();
  decoded_stream_cache_order.clear();
}

bool Pdf::load_document_structure() {
  clear_error();

  // Detect linearization (must be done before other parsing)
  parse_linearization_dict(data, data_size, &linearization);

  {
    bool need_build = false;
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      if (!object_offsets_built && !object_offsets_failed) {
        need_build = true;
        object_offsets_failed = true;  // Prevent concurrent builds
      }
    }
    if (need_build) {
      if (build_object_offset_cache()) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        object_offsets_built = true;
        object_offsets_failed = false;
      }
      // If build failed, object_offsets_failed remains true
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
    NANOPDF_LOG_ERROR("load", "No /Root entry in trailer");
    set_error(ErrorKind::Malformed, "no /Root entry in trailer");
    return false;
  }

  ResolvedObject root_obj = load_object(root, 0);
  if (!root_obj.success || root_obj.value.type != Value::DICTIONARY) {
    NANOPDF_LOG_ERROR("load", "Root catalog object %u is not a dictionary", root);
    set_error(ErrorKind::Malformed, "root catalog object is not a dictionary");
    return false;
  }

  const Dictionary& root_dict = root_obj.value.dict;
  auto version_it = root_dict.find("Version");
  if (version_it != root_dict.end() && version_it->second.type == Value::NAME) {
    catalog.version = version_it->second.name;
  }

  // Extract AcroForm dictionary from catalog
  auto acroform_it = root_dict.find("AcroForm");
  if (acroform_it != root_dict.end()) {
    if (acroform_it->second.type == Value::DICTIONARY) {
      catalog.acro_form = acroform_it->second.dict;
    } else if (acroform_it->second.type == Value::REFERENCE) {
      ResolvedObject af_obj = load_object(acroform_it->second.ref_object_number,
                                          acroform_it->second.ref_generation_number);
      if (af_obj.success && af_obj.value.type == Value::DICTIONARY) {
        catalog.acro_form = std::move(af_obj.value.dict);
      }
    }
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

    parse_page_annotations(*this, page, dict);

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
  parse_optional_content(*this, catalog);
  parse_output_intents(*this, catalog);
  parse_document_actions(*this, catalog);
  document_info_loaded = true;
  xmp_metadata_loaded = true;
  optional_content_loaded = true;
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

void Pdf::ensure_optional_content_loaded() const {
  if (!optional_content_loaded) {
    ensure_pages_loaded();
    parse_optional_content(*this, const_cast<DocumentCatalog&>(catalog));
    optional_content_loaded = true;
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

          // Parse lookup table. The entry can be a string literal, a stream,
          // or an indirect reference to either. Resolve references first.
          Value lookup_val = cs_value.array[3];
          if (lookup_val.type == Value::REFERENCE) {
            ResolvedObject resolved = resolve_reference(
                pdf, lookup_val.ref_object_number, lookup_val.ref_generation_number);
            if (resolved.success) {
              lookup_val = resolved.value;
            }
          }
          if (lookup_val.type == Value::STRING) {
            color_space.lookup_table.assign(lookup_val.str.begin(),
                                           lookup_val.str.end());
          } else if (lookup_val.type == Value::STREAM) {
            // Lookup table stream may be compressed; decode it.
            DecodedStream dec = decode_stream(
                pdf, lookup_val, cs_value.array[3].ref_object_number,
                cs_value.array[3].ref_generation_number);
            if (dec.success) {
              color_space.lookup_table = std::move(dec.data);
            } else {
              color_space.lookup_table = lookup_val.stream.data;
            }
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
  ImageParseOptions options;
  return parse_image_xobject(pdf, stream_value, obj_num, gen_num, options);
}

ImageXObject parse_image_xobject(const Pdf& pdf, const Value& stream_value,
                                 uint32_t obj_num, uint16_t gen_num,
                                 const ImageParseOptions& options) {
  ImageXObject image;

  if (stream_value.type != Value::STREAM) {
    return image;  // Invalid image object
  }

  const Dictionary& dict = stream_value.stream.dict;

  if (options.keep_raw_data) {
    image.raw_data = stream_value.stream.data;
  }

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
                                        image.width, image.height,
                                        options.cache_decoded_stream);
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

  // ActualText tracking: stack of (has_actual_text, actual_text_value)
  struct MarkedContentEntry {
    bool has_actual_text{false};
    std::string actual_text;
  };
  std::vector<MarkedContentEntry> mc_stack_;

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
      "CS", "cs", "SC", "SCN", "sc", "scn", "G", "g", "RG", "rg", "K", "k",
      "BMC", "BDC", "EMC", "Do"
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
      // Show text string - suppress if inside ActualText span
      if (!in_actual_text_span()) {
        std::string text = decode_text_string(operands[0]);
        text_state_.current_text += text;
      }
    } else if (op == "TJ" && operands.size() >= 1) {
      // Show text with individual positioning - suppress if inside ActualText span
      if (!in_actual_text_span()) {
        std::string array_str = operands[0];
        if (array_str.front() == '[' && array_str.back() == ']') {
          array_str = array_str.substr(1, array_str.size() - 2);
          std::vector<std::string> elements = parse_array_elements(array_str);
          for (const auto& elem : elements) {
            if (elem.front() == '(' || elem.front() == '<') {
              text_state_.current_text += decode_text_string(elem);
            }
          }
        }
      }
    } else if (op == "'" && operands.size() >= 1) {
      // Move to next line and show text
      execute_operator("T*", {});
      if (!in_actual_text_span()) execute_operator("Tj", operands);
    } else if (op == "\"" && operands.size() >= 3) {
      // Set word/char spacing, move to next line, show text
      text_state_.word_spacing = parse_number(operands[0]);
      text_state_.char_spacing = parse_number(operands[1]);
      execute_operator("T*", {});
      if (!in_actual_text_span()) execute_operator("Tj", {operands[2]});
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
    } else if (op == "BDC") {
      // Begin marked content with properties - check for ActualText
      MarkedContentEntry entry;
      // Look for ActualText in operands (inline properties dict)
      for (size_t i = 0; i < operands.size(); ++i) {
        if (operands[i] == "/ActualText" && i + 1 < operands.size()) {
          entry.has_actual_text = true;
          entry.actual_text = decode_pdf_string_raw(operands[i + 1]);
          // ActualText may be UTF-16BE encoded
          if (entry.actual_text.size() >= 2) {
            unsigned char b0 = static_cast<unsigned char>(entry.actual_text[0]);
            unsigned char b1 = static_cast<unsigned char>(entry.actual_text[1]);
            if (b0 == 0xFE && b1 == 0xFF) {
              entry.actual_text = decode_utf16be(entry.actual_text);
            }
          }
          break;
        }
      }
      mc_stack_.push_back(entry);
    } else if (op == "BMC") {
      mc_stack_.push_back(MarkedContentEntry{});
    } else if (op == "EMC") {
      if (!mc_stack_.empty()) {
        auto& entry = mc_stack_.back();
        if (entry.has_actual_text) {
          text_state_.current_text += entry.actual_text;
        }
        mc_stack_.pop_back();
      }
    } else if (op == "Do" && operands.size() >= 1) {
      // Invoke XObject - recurse into Form XObjects for text extraction
      std::string xobj_name = operands[0];
      if (xobj_name.size() > 1 && xobj_name[0] == '/') xobj_name = xobj_name.substr(1);
      auto xobj_it = page_.resources.find("XObject");
      if (xobj_it != page_.resources.end() && xobj_it->second.type == Value::DICTIONARY) {
        auto obj_it = xobj_it->second.dict.find(xobj_name);
        if (obj_it != xobj_it->second.dict.end()) {
          Value xobj_val = obj_it->second;
          if (xobj_val.type == Value::REFERENCE) {
            auto res = resolve_reference(pdf_, xobj_val.ref_object_number, xobj_val.ref_generation_number);
            if (res.success) xobj_val = res.value;
          }
          if (xobj_val.type == Value::STREAM) {
            auto subtype_it = xobj_val.stream.dict.find("Subtype");
            if (subtype_it != xobj_val.stream.dict.end() &&
                subtype_it->second.type == Value::NAME &&
                subtype_it->second.name == "Form") {
              // Load Form XObject's font resources into page font cache
              auto form_res_it = xobj_val.stream.dict.find("Resources");
              if (form_res_it != xobj_val.stream.dict.end()) {
                Value form_res = form_res_it->second;
                if (form_res.type == Value::REFERENCE) {
                  auto rr = resolve_reference(pdf_, form_res.ref_object_number,
                                               form_res.ref_generation_number);
                  if (rr.success) form_res = rr.value;
                }
                if (form_res.type == Value::DICTIONARY) {
                  auto font_res_it = form_res.dict.find("Font");
                  if (font_res_it != form_res.dict.end()) {
                    Value font_dict = font_res_it->second;
                    if (font_dict.type == Value::REFERENCE) {
                      auto fr = resolve_reference(pdf_, font_dict.ref_object_number,
                                                   font_dict.ref_generation_number);
                      if (fr.success) font_dict = fr.value;
                    }
                    if (font_dict.type == Value::DICTIONARY) {
                      for (const auto& fe : font_dict.dict) {
                        if (page_.fonts.find(fe.first) == page_.fonts.end()) {
                          Value fv = fe.second;
                          if (fv.type == Value::REFERENCE) {
                            auto fres = resolve_reference(pdf_, fv.ref_object_number,
                                                           fv.ref_generation_number);
                            if (fres.success) fv = fres.value;
                          }
                          auto parsed = Pdf::parse_font(pdf_, fv);
                          if (parsed) page_.fonts[fe.first] = std::move(parsed);
                        }
                      }
                    }
                  }
                }
              }
              auto decoded = decode_stream(pdf_, xobj_val);
              if (decoded.success && !decoded.data.empty()) {
                process_content_stream(decoded.data);
              }
            }
          }
        }
      }
    }
  }

  bool in_actual_text_span() const {
    for (auto it = mc_stack_.rbegin(); it != mc_stack_.rend(); ++it) {
      if (it->has_actual_text) return true;
    }
    return false;
  }

  double parse_number(const std::string& str) {
    return nanopdf::stod_or(str);
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
            int value = nanopdf::stou_base_or(octal, 8);
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
        result += static_cast<char>(nanopdf::stou_base_or(byte, 16));
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
      const Type0Font* type0 = as_type0_font(base_font);
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

    // Subset-font convention "G<hex>" / "g<hex>": <hex> is the 2- or 4-digit
    // hex character code (e.g. "G53" -> 0x53 'S'). Checked after the AGL table
    // so a real name like "Gamma" is never misread as hex.
    if ((name[0] == 'G' || name[0] == 'g') &&
        (name.size() == 3 || name.size() == 5)) {
      std::string hex = name.substr(1);
      char* end = nullptr;
      long value = std::strtol(hex.c_str(), &end, 16);
      if (end && *end == '\0' && value > 0) {
        append_utf8(static_cast<uint32_t>(value), utf8_out);
        return true;
      }
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
    if (cmap.name == "Identity-H" || cmap.name == "Identity-V") {
      return true;
    }
    // UTF16-based predefined CMaps: CID values are Unicode code points,
    // so they can be decoded the same way as Identity CMaps.
    if (cmap.name.find("-UTF16-") != std::string::npos ||
        cmap.name.find("-UCS2-") != std::string::npos) {
      return true;
    }
    return false;
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
  if (!out) return false;
  int32_t value = 0;
  if (!parse_int(token, &value)) return false;
  *out = value;
  return true;
}

bool parse_literal_number(const std::string& token, uint32_t* out) {
  if (!out) {
    return false;
  }
  size_t consumed = 0;
  uint32_t value = 0;
  if (!parse_uint_consumed(token, &value, &consumed)) return false;
  if (consumed != token.size()) return false;
  *out = value;
  return true;
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
  return parse_hex_uint(hex, out);
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

// Parse Type1 font program from FontFile stream and extract encoding
// This is called when no explicit encoding is provided in the PDF
void parse_type1_font_encoding(const Pdf& pdf, BaseFont* font) {
  if (!font || !font->descriptor) {
    return;
  }

  // Check if FontFile is present (Type1 font program)
  const Value& font_file = font->descriptor->font_file;
  if (font_file.type != Value::REFERENCE && font_file.type != Value::STREAM) {
    return;
  }

  // Resolve the font file stream
  Value resolved;
  if (font_file.type == Value::REFERENCE) {
    ResolvedObject res = resolve_reference(pdf, font_file.ref_object_number,
                                           font_file.ref_generation_number);
    if (!res.success || res.value.type != Value::STREAM) {
      return;
    }
    resolved = res.value;
  } else {
    resolved = font_file;
  }

  if (resolved.type != Value::STREAM) {
    return;
  }

  // Decode the stream
  DecodedStream decoded = decode_stream(const_cast<Pdf&>(pdf), resolved);
  if (!decoded.success || decoded.data.empty()) {
    return;
  }

  // Parse the Type1 font program
  Type1Parser parser;
  Type1FontData fontData;
  if (!parser.Parse(decoded.data.data(), decoded.data.size(), fontData, false)) {
    NANOPDF_LOG_DEBUG("Type1Parser failed: %s", parser.GetError().c_str());
    return;
  }

  // If font uses standard encoding and no explicit encoding was provided,
  // populate encoding differences from the Type1 program
  if (!fontData.uses_standard_encoding) {
    // Apply encoding from Type1 font program
    for (int i = 0; i < 256; ++i) {
      if (!fontData.encoding[i].empty() && fontData.encoding[i] != ".notdef") {
        font->encoding_differences[static_cast<uint32_t>(i)] = fontData.encoding[i];
      }
    }
  }

  // Update font metrics if not already set
  if (font->descriptor && font->descriptor->font_bbox.empty() &&
      (fontData.font_bbox[2] != 0 || fontData.font_bbox[3] != 0)) {
    font->descriptor->font_bbox.push_back(fontData.font_bbox[0]);
    font->descriptor->font_bbox.push_back(fontData.font_bbox[1]);
    font->descriptor->font_bbox.push_back(fontData.font_bbox[2]);
    font->descriptor->font_bbox.push_back(fontData.font_bbox[3]);
  }
}

// Parse CFF font program from FontFile3 stream and extract encoding/charset
// This is called when no explicit encoding is provided in the PDF for CFF fonts
void parse_cff_font_encoding(const Pdf& pdf, BaseFont* font) {
  if (!font || !font->descriptor) {
    return;
  }

  // Check if FontFile3 is present (CFF font program)
  if (font->descriptor->font_file_type != FontFileType::FontFile3) {
    return;
  }

  const Value& font_file = font->descriptor->font_file;
  if (font_file.type != Value::REFERENCE && font_file.type != Value::STREAM) {
    return;
  }

  // Resolve the font file stream
  Value resolved;
  if (font_file.type == Value::REFERENCE) {
    ResolvedObject res = resolve_reference(pdf, font_file.ref_object_number,
                                           font_file.ref_generation_number);
    if (!res.success || res.value.type != Value::STREAM) {
      return;
    }
    resolved = res.value;
  } else {
    resolved = font_file;
  }

  if (resolved.type != Value::STREAM) {
    return;
  }

  // Decode the stream
  DecodedStream decoded = decode_stream(const_cast<Pdf&>(pdf), resolved);
  if (!decoded.success || decoded.data.empty()) {
    return;
  }

  // Parse the CFF font program
  cff::CFFParser parser;
  cff::CFFData cffData;
  if (!parser.parse(decoded.data.data(), decoded.data.size(), cffData)) {
    NANOPDF_LOG_DEBUG("CFFParser failed for font %s", font->base_font.c_str());
    return;
  }

  NANOPDF_LOG_DEBUG("Parsed CFF font: %s, %zu glyphs, %zu charset entries",
                    cffData.font_name.c_str(), static_cast<size_t>(cffData.num_glyphs),
                    cffData.charset.size());

  // Build encoding mapping from CFF data
  // The CFF charset maps glyph index to glyph name
  // The CFF encoding maps code to glyph index
  if (!cffData.encoding.empty() && !cffData.charset.empty()) {
    for (int code = 0; code < 256 && code < static_cast<int>(cffData.encoding.size()); ++code) {
      int gid = cffData.encoding[code];
      if (gid > 0 && gid < static_cast<int>(cffData.charset.size())) {
        const std::string& glyph_name = cffData.charset[gid];
        if (!glyph_name.empty() && glyph_name != ".notdef") {
          font->encoding_differences[static_cast<uint32_t>(code)] = glyph_name;
        }
      }
    }
  }

  // Update font metrics if not already set
  if (font->descriptor && font->descriptor->font_bbox.empty() &&
      cffData.font_bbox.size() >= 4) {
    for (size_t i = 0; i < 4; ++i) {
      font->descriptor->font_bbox.push_back(cffData.font_bbox[i]);
    }
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

// Parse W2 (vertical metrics) array for CIDFonts
// Format: [CID [w1_y v_x v_y ...]] or [CID_first CID_last w1_y v_x v_y]
void parse_cid_vertical_width_array(const std::vector<Value>& array, Type0Font* font) {
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
      // Format: CID [w1_y v_x v_y w1_y v_x v_y ...]
      // Groups of 3 values for each CID starting from cid
      const auto& metrics_array = next.array;
      for (size_t j = 0; j + 2 < metrics_array.size(); j += 3) {
        if (metrics_array[j].type == Value::NUMBER &&
            metrics_array[j + 1].type == Value::NUMBER &&
            metrics_array[j + 2].type == Value::NUMBER) {
          Type0Font::VerticalMetrics vm;
          vm.w1_y = metrics_array[j].number;
          vm.v_x = metrics_array[j + 1].number;
          vm.v_y = metrics_array[j + 2].number;
          font->cid_vertical_metrics[cid + static_cast<uint32_t>(j / 3)] = vm;
        }
      }
      i++;
    } else if (next.type == Value::NUMBER) {
      // Format: CID_first CID_last w1_y v_x v_y
      uint32_t end_cid = static_cast<uint32_t>(next.number);
      i++;
      if (i + 2 >= array.size()) {
        break;
      }
      if (array[i].type == Value::NUMBER &&
          array[i + 1].type == Value::NUMBER &&
          array[i + 2].type == Value::NUMBER) {
        Type0Font::VerticalMetrics vm;
        vm.w1_y = array[i].number;
        vm.v_x = array[i + 1].number;
        vm.v_y = array[i + 2].number;
        for (uint32_t current = cid; current <= end_cid; ++current) {
          font->cid_vertical_metrics[current] = vm;
        }
      }
      i += 3;
    } else {
      i++;
    }
  }
  if (!font->cid_vertical_metrics.empty()) {
    font->has_vertical_metrics = true;
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
// Parse an embedded TrueType (FontFile2) cmap and build a GID->Unicode map by
// reversing the font's Unicode cmap subtable (formats 4 and 12). Used to make
// Identity-H CID fonts that lack a /ToUnicode CMap text-extractable.
static void build_gid_to_unicode_from_truetype(
    const uint8_t* d, size_t n, std::map<uint32_t, uint32_t>& gid2uni) {
  auto u16 = [](const uint8_t* p) -> uint16_t {
    return (uint16_t)((p[0] << 8) | p[1]);
  };
  auto u32 = [](const uint8_t* p) -> uint32_t {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
  };
  if (n < 12) return;
  uint16_t num_tables = u16(d + 4);
  uint32_t cmap_off = 0;
  for (uint16_t i = 0; i < num_tables; ++i) {
    size_t rec = 12 + (size_t)i * 16;
    if (rec + 16 > n) break;
    if (std::memcmp(d + rec, "cmap", 4) == 0) {
      cmap_off = u32(d + rec + 8);
      break;
    }
  }
  if (!cmap_off || cmap_off + 4 > n) return;
  uint16_t nsub = u16(d + cmap_off + 2);
  uint32_t best = 0;
  int best_score = -1;
  for (uint16_t i = 0; i < nsub; ++i) {
    size_t rec = cmap_off + 4 + (size_t)i * 8;
    if (rec + 8 > n) break;
    uint16_t plat = u16(d + rec), enc = u16(d + rec + 2);
    uint32_t off = u32(d + rec + 4);
    int score = -1;
    if (plat == 3 && enc == 10) score = 4;       // Windows UCS-4
    else if (plat == 3 && enc == 1) score = 3;   // Windows BMP
    else if (plat == 0) score = 2;               // Unicode
    else if (plat == 3 && enc == 0) score = 1;   // Symbol
    if (score > best_score) {
      best_score = score;
      best = cmap_off + off;
    }
  }
  if (!best || best + 4 > n) return;
  uint16_t format = u16(d + best);
  auto put = [&](uint32_t uni, uint32_t gid) {
    if (gid && gid2uni.find(gid) == gid2uni.end()) gid2uni[gid] = uni;
  };
  if (format == 4) {
    if (best + 14 > n) return;
    uint16_t segx2 = u16(d + best + 6);
    uint16_t segc = segx2 / 2;
    size_t end_o = best + 14;
    size_t start_o = end_o + segx2 + 2;
    size_t delta_o = start_o + segx2;
    size_t range_o = delta_o + segx2;
    for (uint16_t s = 0; s < segc; ++s) {
      if (range_o + (size_t)s * 2 + 2 > n) break;
      uint16_t end = u16(d + end_o + (size_t)s * 2);
      uint16_t start = u16(d + start_o + (size_t)s * 2);
      int16_t delta = (int16_t)u16(d + delta_o + (size_t)s * 2);
      uint16_t ro = u16(d + range_o + (size_t)s * 2);
      for (uint32_t c = start; c <= end && c != 0xFFFF; ++c) {
        uint16_t gid;
        if (ro == 0) {
          gid = (uint16_t)(c + delta);
        } else {
          size_t gi = range_o + (size_t)s * 2 + ro + (size_t)(c - start) * 2;
          if (gi + 2 > n) continue;
          uint16_t g = u16(d + gi);
          gid = g ? (uint16_t)(g + delta) : 0;
        }
        put(c, gid);
      }
    }
  } else if (format == 12) {
    if (best + 16 > n) return;
    uint32_t ngroups = u32(d + best + 12);
    for (uint32_t i = 0; i < ngroups; ++i) {
      size_t g = best + 16 + (size_t)i * 12;
      if (g + 12 > n) break;
      uint32_t sc = u32(d + g), ec = u32(d + g + 4), sg = u32(d + g + 8);
      if (ec < sc || ec - sc > 0x20000) continue;  // sanity cap
      for (uint32_t c = sc; c <= ec; ++c) put(c, sg + (c - sc));
    }
  }
}

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

  // Pre-compute two-byte CID and vertical flags from CMap name
  {
    const std::string& cn = font->encoding_cmap.name;
    if (cn.find("Identity") != std::string::npos ||
        cn.find("UTF16") != std::string::npos ||
        cn.find("UCS2") != std::string::npos ||
        cn.find("UniJIS") != std::string::npos ||
        cn.find("UniGB") != std::string::npos ||
        cn.find("UniKS") != std::string::npos ||
        cn.find("UniCNS") != std::string::npos) {
      font->is_two_byte_cid = true;
    }
    if (cn.size() >= 2 && cn.substr(cn.size() - 2) == "-V") {
      font->is_vertical = true;
    }
    if (!font->is_two_byte_cid) {
      const std::string& o = font->ordering;
      if (o == "Japan1" || o == "GB1" || o == "CNS1" || o == "Korea1") {
        font->is_two_byte_cid = true;
      }
    }
  }

  auto tounicode_it = font_dict.find("ToUnicode");
  if (tounicode_it != font_dict.end()) {
    parse_cmap_value(pdf, tounicode_it->second, &font->to_unicode_cmap);
  }

  auto desc_it = font_dict.find("DescendantFonts");
  Value desc_array_val;
  bool has_desc = false;
  if (desc_it != font_dict.end()) {
    desc_array_val = desc_it->second;
    if (desc_array_val.type == Value::REFERENCE) {
      ResolvedObject r = resolve_reference(pdf, desc_array_val.ref_object_number,
                                           desc_array_val.ref_generation_number);
      if (r.success) desc_array_val = r.value;
    }
    has_desc = desc_array_val.type == Value::ARRAY && !desc_array_val.array.empty();
  }
  if (has_desc) {
    Value descendant_value;
    if (resolve_indirect_value(pdf, desc_array_val.array[0], &descendant_value) &&
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

      // Several descendant CID-font entries can be indirect references.
      auto resolve = [&](Value v) -> Value {
        if (v.type == Value::REFERENCE) {
          ResolvedObject r = resolve_reference(pdf, v.ref_object_number,
                                               v.ref_generation_number);
          if (r.success) return r.value;
        }
        return v;
      };

      auto widths_it = cid_font_dict.find("Widths");
      if (widths_it != cid_font_dict.end()) {
        Value v = resolve(widths_it->second);
        if (v.type == Value::ARRAY) {
          descendant->widths.clear();
          for (const auto& width_val : v.array) {
            if (width_val.type == Value::NUMBER) {
              descendant->widths.push_back(static_cast<int>(width_val.number));
            }
          }
        }
      }

      auto cid_info_it = cid_font_dict.find("CIDSystemInfo");
      if (cid_info_it != cid_font_dict.end()) {
        Value v = resolve(cid_info_it->second);
        if (v.type == Value::DICTIONARY) {
          parse_cid_system_info(v.dict, font.get());
        }
      }

      auto dw_it = cid_font_dict.find("DW");
      if (dw_it != cid_font_dict.end()) {
        Value v = resolve(dw_it->second);
        if (v.type == Value::NUMBER) {
          font->default_width = static_cast<int>(v.number);
        }
      }

      auto w_it = cid_font_dict.find("W");
      if (w_it != cid_font_dict.end()) {
        Value v = resolve(w_it->second);
        if (v.type == Value::ARRAY) {
          font->cid_widths.clear();
          parse_cid_width_array(v.array, font.get());
        }
      }

      // Parse DW2 (default vertical metrics): [v_y w1_y]
      auto dw2_it = cid_font_dict.find("DW2");
      if (dw2_it != cid_font_dict.end() && dw2_it->second.type == Value::ARRAY) {
        const auto& dw2_array = dw2_it->second.array;
        if (dw2_array.size() >= 2 &&
            dw2_array[0].type == Value::NUMBER &&
            dw2_array[1].type == Value::NUMBER) {
          font->default_v_y = dw2_array[0].number;
          font->default_w1_y = dw2_array[1].number;
          font->has_vertical_metrics = true;
        }
      }

      // Parse W2 (vertical metrics per CID)
      auto w2_it = cid_font_dict.find("W2");
      if (w2_it != cid_font_dict.end() && w2_it->second.type == Value::ARRAY) {
        font->cid_vertical_metrics.clear();
        parse_cid_vertical_width_array(w2_it->second.array, font.get());
      }

      auto cid_to_gid_it = cid_font_dict.find("CIDToGIDMap");
      if (cid_to_gid_it != cid_font_dict.end()) {
        parse_cid_to_gid_map(pdf, cid_to_gid_it->second, font.get());
      }

      font->descendant_font = std::move(descendant);
    }
  }

  // Fallback for Identity-H/V CID fonts lacking /ToUnicode: reverse the embedded
  // TrueType cmap (GID->Unicode) so the text is extractable/searchable AND the
  // renderer can substitute the right glyph (instead of tofu) when needed.
  {
    const bool identity_enc = font->encoding_cmap.name.empty() ||
                              font->encoding_cmap.name == "Identity-H" ||
                              font->encoding_cmap.name == "Identity-V";
    if (font->to_unicode_cmap.code_to_unicode.empty() && identity_enc &&
        font->descendant_font && font->descendant_font->descriptor &&
        font->descendant_font->descriptor->font_file_type ==
            FontFileType::FontFile2) {
      const Value& ff = font->descendant_font->descriptor->font_file;
      Value resolved;
      uint32_t ff_obj = 0, ff_gen = 0;
      if (ff.type == Value::REFERENCE) {
        ff_obj = ff.ref_object_number;
        ff_gen = ff.ref_generation_number;
        ResolvedObject r = resolve_reference(pdf, ff.ref_object_number,
                                             ff.ref_generation_number);
        if (r.success) resolved = r.value;
      } else if (ff.type == Value::STREAM) {
        resolved = ff;
      }
      if (resolved.type == Value::STREAM) {
        // Pass the stream's object number so encrypted font programs decrypt
        // with the correct per-object key.
        DecodedStream dec =
            decode_stream(const_cast<Pdf&>(pdf), resolved, ff_obj, ff_gen);
        if (dec.success && !dec.data.empty()) {
          std::map<uint32_t, uint32_t> gid2uni;
          build_gid_to_unicode_from_truetype(dec.data.data(), dec.data.size(),
                                             gid2uni);
          if (!gid2uni.empty()) {
            auto& out = font->to_unicode_cmap.code_to_unicode;
            const auto& c2g = font->cid_to_gid_map;
            if (c2g.empty()) {
              for (const auto& kv : gid2uni) out[kv.first] = kv.second;  // CID==GID
            } else {
              for (uint32_t cid = 0; cid < c2g.size(); ++cid) {
                auto it = gid2uni.find(c2g[cid]);
                if (it != gid2uni.end()) out[cid] = it->second;
              }
            }
          }
        }
      }
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

  // Parse Widths — may be an indirect reference, resolve before use.
  auto widths_it = font_dict.find("Widths");
  if (widths_it != font_dict.end()) {
    Value widths_val = widths_it->second;
    if (widths_val.type == Value::REFERENCE) {
      ResolvedObject resolved = resolve_reference(pdf,
          widths_val.ref_object_number, widths_val.ref_generation_number);
      if (resolved.success) widths_val = resolved.value;
    }
    if (widths_val.type == Value::ARRAY) {
      for (const auto& val : widths_val.array) {
        if (val.type == Value::NUMBER) {
          font->widths.push_back(static_cast<int>(val.number));
        }
      }
    }
  }

  // Parse FirstChar and LastChar — also may be indirect references.
  auto resolve_int = [&](const std::string& key, int& out) {
    auto it = font_dict.find(key);
    if (it == font_dict.end()) return;
    Value v = it->second;
    if (v.type == Value::REFERENCE) {
      ResolvedObject r = resolve_reference(pdf, v.ref_object_number, v.ref_generation_number);
      if (r.success) v = r.value;
    }
    if (v.type == Value::NUMBER) out = static_cast<int>(v.number);
  };
  resolve_int("FirstChar", font->first_char);
  resolve_int("LastChar",  font->last_char);

  // Parse Encoding (critical for text extraction from Type3 fonts)
  auto enc_it = font_dict.find("Encoding");
  if (enc_it != font_dict.end()) {
    parse_simple_font_encoding(pdf, enc_it->second, font.get());
  }

  // Parse ToUnicode CMap
  auto tounicode_it = font_dict.find("ToUnicode");
  if (tounicode_it != font_dict.end()) {
    parse_cmap_value(pdf, tounicode_it->second, &font->to_unicode_cmap);
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
  if (fontfile_it != dict.end()) {
    descriptor->font_file = fontfile_it->second;
    descriptor->font_file_type = FontFileType::FontFile2;
  } else {
    fontfile_it = dict.find("FontFile3");  // CFF/OpenType
    if (fontfile_it != dict.end()) {
      descriptor->font_file = fontfile_it->second;
      descriptor->font_file_type = FontFileType::FontFile3;
    } else {
      fontfile_it = dict.find("FontFile");  // Type1
      if (fontfile_it != dict.end()) {
        descriptor->font_file = fontfile_it->second;
        descriptor->font_file_type = FontFileType::FontFile;
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
    if (enc_it != font_dict.end()) {
      parse_simple_font_encoding(pdf, enc_it->second, font.get());
    }

    // Parse FontDescriptor
    auto desc_it = font_dict.find("FontDescriptor");
    if (desc_it != font_dict.end()) {
      font->descriptor = parse_font_descriptor(pdf, desc_it->second).release();
    }

    // Parse Widths / FirstChar / LastChar. Each entry may be an indirect
    // reference that needs resolving (common in encrypted/compressed PDFs).
    auto widths_it = font_dict.find("Widths");
    if (widths_it != font_dict.end()) {
      Value widths_val = widths_it->second;
      if (widths_val.type == Value::REFERENCE) {
        ResolvedObject r = resolve_reference(pdf,
            widths_val.ref_object_number, widths_val.ref_generation_number);
        if (r.success) widths_val = r.value;
      }
      if (widths_val.type == Value::ARRAY) {
        for (const auto& val : widths_val.array) {
          if (val.type == Value::NUMBER) {
            font->widths.push_back(static_cast<int>(val.number));
          }
        }
      }
    }
    auto resolve_int = [&](const std::string& key, int& out) {
      auto it = font_dict.find(key);
      if (it == font_dict.end()) return;
      Value v = it->second;
      if (v.type == Value::REFERENCE) {
        ResolvedObject r = resolve_reference(pdf,
            v.ref_object_number, v.ref_generation_number);
        if (r.success) v = r.value;
      }
      if (v.type == Value::NUMBER) out = static_cast<int>(v.number);
    };
    resolve_int("FirstChar", font->first_char);
    resolve_int("LastChar",  font->last_char);

    // For Type1 fonts with embedded font program, try to extract encoding
    // ONLY when the PDF provides no encoding information of its own. Per
    // PDF spec 9.6.6.1, an explicit /Encoding (a base-encoding name such as
    // WinAnsiEncoding, or a dict with Differences) takes precedence over the
    // font program's built-in encoding. Overriding it here corrupts fonts
    // whose embedded CFF/Type1 program carries a re-ordered built-in encoding
    // (e.g. subset Courier where the CFF encoding is offset from WinAnsi).
    if (subtype == "Type1" && font->descriptor &&
        font->encoding_differences.empty() && font->encoding.empty()) {
      // Try CFF (FontFile3) first, then Type1 (FontFile)
      if (font->descriptor->font_file_type == FontFileType::FontFile3) {
        parse_cff_font_encoding(pdf, font.get());
      } else if (font->descriptor->font_file_type == FontFileType::FontFile) {
        parse_type1_font_encoding(pdf, font.get());
      }
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
      const Dictionary* annot_dict = &annot_val.dict;

      // Check for Redact subtype
      auto subtype_it = annot_dict->find("Subtype");
      std::string subtype;
      if (subtype_it != annot_dict->end() && subtype_it->second.type == Value::NAME) {
        subtype = subtype_it->second.name;
      }

      if (subtype == "Redact") {
        RedactionAnnotation redact;

        // Parse annotation rect
        auto rect_it = annot_dict->find("Rect");
        if (rect_it != annot_dict->end() && rect_it->second.type == Value::ARRAY) {
          for (const auto& val : rect_it->second.array) {
            if (val.type == Value::NUMBER) {
              redact.rect.push_back(val.number);
            }
          }
        }

        uint32_t ref_obj = 0;
        if (annot_ref.type == Value::REFERENCE) {
          ref_obj = annot_ref.ref_object_number;
        }
        redact.object_number = ref_obj;

        // Parse overlay color (/IC entry)
        auto ic_it = annot_dict->find("IC");
        if (ic_it != annot_dict->end() && ic_it->second.type == Value::ARRAY) {
          const auto& ic = ic_it->second.array;
          if (ic.size() >= 3) {
            redact.overlay_color_r = ic[0].number;
            redact.overlay_color_g = ic[1].number;
            redact.overlay_color_b = ic[2].number;
            redact.has_overlay_color = true;
          } else if (ic.empty()) {
            redact.has_overlay_color = false;
          }
        }

        // Parse overlay text (/OverlayText)
        auto ot_it = annot_dict->find("OverlayText");
        if (ot_it != annot_dict->end() && ot_it->second.type == Value::STRING) {
          redact.overlay_text = ot_it->second.str;
        }

        // Parse repeat flag (/Repeat)
        auto rp_it = annot_dict->find("Repeat");
        if (rp_it != annot_dict->end() && rp_it->second.type == Value::BOOLEAN) {
          redact.repeat = rp_it->second.boolean ? 1 : 0;
        }

        // Parse QuadPoints
        auto qp_it = annot_dict->find("QuadPoints");
        if (qp_it != annot_dict->end() && qp_it->second.type == Value::ARRAY) {
          const auto& qp = qp_it->second.array;
          // QuadPoints come in groups of 8 numbers (4 x,y pairs per quad)
          for (size_t qi = 0; qi + 7 < qp.size(); qi += 8) {
            std::vector<double> quad;
            for (int qj = 0; qj < 8; ++qj) {
              quad.push_back(qp[qi + qj].number);
            }
            redact.quad_points.push_back(quad);
          }
        }

        page.redaction_annotations.push_back(redact);
      } else {
        auto annotation = parse_annotation(pdf, annot_val);
        if (annotation) {
          annotation->page_ref = page.object_number;
          page.annotations.push_back(std::move(annotation));
        }
      }
    }
  }

  // Parse page-level /AA (Additional Actions)
  auto aa_it = page_dict.find("AA");
  if (aa_it != page_dict.end()) {
    const Value* aa_val = &aa_it->second;
    ResolvedObject aa_resolved;
    if (aa_val->type == Value::REFERENCE) {
      aa_resolved = resolve_reference(pdf, aa_val->ref_object_number,
                                      aa_val->ref_generation_number);
      if (aa_resolved.success) aa_val = &aa_resolved.value;
    }
    if (aa_val->type == Value::DICTIONARY) {
      page.additional_actions = parse_additional_actions(pdf, aa_val->dict);
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
        // First element is the destination page (indirect reference). Map it to
        // the 0-based page index by matching the page object number.
        if (arr[0].type == Value::REFERENCE) {
          uint32_t page_obj = arr[0].ref_object_number;
          for (size_t i = 0; i < pdf.catalog.pages.size(); ++i) {
            if (pdf.catalog.pages[i].object_number == page_obj) {
              item->dest_page = static_cast<uint32_t>(i);
              break;
            }
          }
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
          if (action_name == "GoTo") {
            // Go-to-page action: /D = [pageRef /Fit ...]. Map the page
            // reference to its 0-based index.
            item->action_type = OutlineAction::GoTo;
            auto d_it = action_dict.find("D");
            if (d_it != action_dict.end() &&
                d_it->second.type == Value::ARRAY) {
              const auto& darr = d_it->second.array;
              if (!darr.empty() && darr[0].type == Value::REFERENCE) {
                uint32_t page_obj = darr[0].ref_object_number;
                for (size_t i = 0; i < pdf.catalog.pages.size(); ++i) {
                  if (pdf.catalog.pages[i].object_number == page_obj) {
                    item->dest_page = static_cast<uint32_t>(i);
                    break;
                  }
                }
              }
            }
          } else if (action_name == "URI") {
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
  // The catalog.outlines member is not reliably populated for all documents
  // (e.g. when /Outlines is an indirect reference resolved lazily), so resolve
  // the outline dictionary straight from the catalog. Prefer the pre-parsed
  // member when present; otherwise fall back to resolving /Outlines.
  const Dictionary* outlines_dict = nullptr;
  Dictionary resolved_outlines;
  if (!catalog.outlines.empty()) {
    outlines_dict = &catalog.outlines;
  } else {
    auto catalog_obj = resolve_reference(pdf, pdf.root, 0);
    if (!catalog_obj.success || catalog_obj.value.type != Value::DICTIONARY) {
      return;
    }
    auto outlines_it = catalog_obj.value.dict.find("Outlines");
    if (outlines_it == catalog_obj.value.dict.end()) {
      return;
    }
    Value outlines_value = outlines_it->second;
    if (outlines_value.type == Value::REFERENCE) {
      auto resolved =
          resolve_reference(pdf, outlines_value.ref_object_number,
                            outlines_value.ref_generation_number);
      if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
        return;
      }
      outlines_value = resolved.value;
    }
    if (outlines_value.type != Value::DICTIONARY) {
      return;
    }
    resolved_outlines = std::move(outlines_value.dict);
    outlines_dict = &resolved_outlines;
  }

  auto type_it = outlines_dict->find("Type");
  if (type_it != outlines_dict->end() && type_it->second.type == Value::NAME &&
      type_it->second.name != "Outlines") {
    return;
  }

  // Parse the first top-level item, then walk its root-level siblings.
  auto first_it = outlines_dict->find("First");
  if (first_it == outlines_dict->end() ||
      first_it->second.type != Value::REFERENCE) {
    return;
  }
  auto first_obj = resolve_reference(pdf, first_it->second.ref_object_number,
                                     first_it->second.ref_generation_number);
  if (!first_obj.success || first_obj.value.type != Value::DICTIONARY) {
    return;
  }
  catalog.outline_root = parse_outline_item(pdf, first_obj.value.dict);

  const Dictionary* current = &first_obj.value.dict;
  std::vector<Dictionary> sibling_dicts;  // keep resolved dicts alive
  while (catalog.outline_root) {
    auto next_it = current->find("Next");
    if (next_it == current->end() || next_it->second.type != Value::REFERENCE) {
      break;
    }
    auto next_obj = resolve_reference(pdf, next_it->second.ref_object_number,
                                      next_it->second.ref_generation_number);
    if (!next_obj.success || next_obj.value.type != Value::DICTIONARY) {
      break;
    }
    sibling_dicts.push_back(std::move(next_obj.value.dict));
    const Dictionary& sib = sibling_dicts.back();
    auto sibling = parse_outline_item(pdf, sib);
    if (sibling) catalog.outline_root->children.push_back(std::move(sibling));
    current = &sib;
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

// Simple XMP XML parser helpers
namespace {

// Decode XML entities in a string
std::string decode_xml_entities(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '&') {
      if (s.compare(i, 5, "&amp;") == 0) { result += '&'; i += 4; }
      else if (s.compare(i, 4, "&lt;") == 0) { result += '<'; i += 3; }
      else if (s.compare(i, 4, "&gt;") == 0) { result += '>'; i += 3; }
      else if (s.compare(i, 6, "&quot;") == 0) { result += '"'; i += 5; }
      else if (s.compare(i, 6, "&apos;") == 0) { result += '\''; i += 5; }
      else { result += s[i]; }
    } else {
      result += s[i];
    }
  }
  return result;
}

// Trim whitespace from both ends
std::string trim_xmp(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t' ||
                               s[start] == '\n' || s[start] == '\r'))
    ++start;
  size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                          s[end - 1] == '\n' || s[end - 1] == '\r'))
    --end;
  return s.substr(start, end - start);
}

// Extract a simple XMP tag value. Handles both:
//   <tag>value</tag>
//   <tag><rdf:Alt><rdf:li ...>value</rdf:li></rdf:Alt></tag>
//   <tag><rdf:Seq><rdf:li>value</rdf:li></rdf:Seq></tag>
// Returns the first rdf:li value if present, otherwise direct content.
std::string extract_simple_tag(const std::string& xml, const std::string& open_tag,
                                const std::string& close_tag) {
  size_t pos = xml.find(open_tag);
  if (pos == std::string::npos) return "";
  size_t end = xml.find(close_tag, pos);
  if (end == std::string::npos) return "";

  size_t content_start = xml.find(">", pos) + 1;

  // Check for RDF container (Alt, Seq, Bag)
  size_t rdf_alt = xml.find("<rdf:Alt>", content_start);
  size_t rdf_seq = xml.find("<rdf:Seq>", content_start);
  size_t rdf_bag = xml.find("<rdf:Bag>", content_start);
  size_t container = std::string::npos;
  if (rdf_alt != std::string::npos && rdf_alt < end) container = rdf_alt;
  if (rdf_seq != std::string::npos && rdf_seq < end &&
      (container == std::string::npos || rdf_seq < container)) container = rdf_seq;
  if (rdf_bag != std::string::npos && rdf_bag < end &&
      (container == std::string::npos || rdf_bag < container)) container = rdf_bag;

  if (container != std::string::npos) {
    // Extract first rdf:li value
    size_t li_pos = xml.find("<rdf:li", container);
    if (li_pos != std::string::npos && li_pos < end) {
      li_pos = xml.find(">", li_pos) + 1;
      size_t li_end = xml.find("</rdf:li>", li_pos);
      if (li_end != std::string::npos && li_end <= end) {
        return decode_xml_entities(trim_xmp(xml.substr(li_pos, li_end - li_pos)));
      }
    }
    return "";
  }

  // Direct content
  size_t tag_end = xml.find("<", content_start);
  if (tag_end != std::string::npos && tag_end <= end) {
    return decode_xml_entities(trim_xmp(xml.substr(content_start, tag_end - content_start)));
  }
  return "";
}

// Extract all rdf:li values from an RDF container (Bag/Seq)
std::vector<std::string> extract_list_tag(const std::string& xml,
                                           const std::string& open_tag,
                                           const std::string& close_tag) {
  std::vector<std::string> result;
  size_t pos = xml.find(open_tag);
  if (pos == std::string::npos) return result;
  size_t end = xml.find(close_tag, pos);
  if (end == std::string::npos) return result;

  size_t search_pos = pos;
  while (true) {
    size_t li_pos = xml.find("<rdf:li", search_pos);
    if (li_pos == std::string::npos || li_pos >= end) break;
    li_pos = xml.find(">", li_pos) + 1;
    size_t li_end = xml.find("</rdf:li>", li_pos);
    if (li_end == std::string::npos || li_end > end) break;
    std::string val = decode_xml_entities(trim_xmp(xml.substr(li_pos, li_end - li_pos)));
    if (!val.empty()) result.push_back(std::move(val));
    search_pos = li_end + 9;  // length of "</rdf:li>"
  }
  return result;
}

}  // namespace

// Simple XMP XML parser
bool XMPMetadata::parse_xml(const std::string& xml) {
  raw_xml = xml;

  // Extract common fields using helper
  dc_title = extract_simple_tag(xml, "<dc:title", "</dc:title>");
  dc_creator = extract_simple_tag(xml, "<dc:creator", "</dc:creator>");
  dc_description = extract_simple_tag(xml, "<dc:description", "</dc:description>");
  dc_subject = extract_list_tag(xml, "<dc:subject", "</dc:subject>");

  xmp_create_date = extract_simple_tag(xml, "<xmp:CreateDate", "</xmp:CreateDate>");
  xmp_modify_date = extract_simple_tag(xml, "<xmp:ModifyDate", "</xmp:ModifyDate>");
  xmp_metadata_date = extract_simple_tag(xml, "<xmp:MetadataDate", "</xmp:MetadataDate>");
  xmp_creator_tool = extract_simple_tag(xml, "<xmp:CreatorTool", "</xmp:CreatorTool>");

  pdf_producer = extract_simple_tag(xml, "<pdf:Producer", "</pdf:Producer>");
  pdf_version = extract_simple_tag(xml, "<pdf:PDFVersion", "</pdf:PDFVersion>");

  xmpmm_document_id = extract_simple_tag(xml, "<xmpMM:DocumentID", "</xmpMM:DocumentID>");
  xmpmm_instance_id = extract_simple_tag(xml, "<xmpMM:InstanceID", "</xmpMM:InstanceID>");

  // PDF/A identification
  std::string part_str = extract_simple_tag(xml, "<pdfaid:part", "</pdfaid:part>");
  if (!part_str.empty()) {
    pdfa_part = std::atoi(part_str.c_str());
  }
  pdfa_conformance = extract_simple_tag(xml, "<pdfaid:conformance", "</pdfaid:conformance>");

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

// Parse a PDF action dictionary
Action parse_action(const Pdf& pdf, const Dictionary& action_dict) {
  Action action;

  auto s_it = action_dict.find("S");
  if (s_it == action_dict.end() || s_it->second.type != Value::NAME) return action;

  const std::string& action_name = s_it->second.name;

  if (action_name == "GoTo") {
    action.type = ActionType::GoTo;
    auto d_it = action_dict.find("D");
    if (d_it != action_dict.end()) {
      const Value* dest_val = &d_it->second;
      ResolvedObject dest_resolved;
      if (dest_val->type == Value::REFERENCE) {
        dest_resolved = resolve_reference(pdf, dest_val->ref_object_number,
                                          dest_val->ref_generation_number);
        if (dest_resolved.success) dest_val = &dest_resolved.value;
      }
      if (dest_val->type == Value::ARRAY && !dest_val->array.empty()) {
        // First element is page reference
        const auto& page_ref = dest_val->array[0];
        if (page_ref.type == Value::REFERENCE) {
          // Find page number from object number
          action.dest_page = page_ref.ref_object_number;
        } else if (page_ref.type == Value::NUMBER) {
          action.dest_page = static_cast<uint32_t>(page_ref.number);
        }
        // Second element is fit type
        if (dest_val->array.size() > 1 && dest_val->array[1].type == Value::NAME) {
          action.dest_fit_type = dest_val->array[1].name;
        }
        // Remaining elements are position parameters
        for (size_t i = 2; i < dest_val->array.size(); ++i) {
          if (dest_val->array[i].type == Value::NUMBER) {
            action.dest_position.push_back(dest_val->array[i].number);
          }
        }
      }
    }
  } else if (action_name == "GoToR") {
    action.type = ActionType::GoToR;
    auto f_it = action_dict.find("F");
    if (f_it != action_dict.end() && f_it->second.type == Value::STRING) {
      action.file = f_it->second.str;
    }
  } else if (action_name == "Launch") {
    action.type = ActionType::Launch;
    auto f_it = action_dict.find("F");
    if (f_it != action_dict.end() && f_it->second.type == Value::STRING) {
      action.file = f_it->second.str;
    }
  } else if (action_name == "URI") {
    action.type = ActionType::URI;
    auto uri_it = action_dict.find("URI");
    if (uri_it != action_dict.end() && uri_it->second.type == Value::STRING) {
      action.uri = uri_it->second.str;
    }
  } else if (action_name == "Named") {
    action.type = ActionType::Named;
    auto n_it = action_dict.find("N");
    if (n_it != action_dict.end() && n_it->second.type == Value::NAME) {
      action.named_action = n_it->second.name;
    }
  } else if (action_name == "JavaScript") {
    action.type = ActionType::JavaScript;
    auto js_it = action_dict.find("JS");
    if (js_it != action_dict.end()) {
      if (js_it->second.type == Value::STRING) {
        action.javascript = js_it->second.str;
      } else if (js_it->second.type == Value::STREAM) {
        auto decoded = decode_stream(pdf, js_it->second);
        if (decoded.success) {
          action.javascript.assign(decoded.data.begin(), decoded.data.end());
        }
      }
    }
  }

  return action;
}

Action parse_action(const Pdf& pdf, const Value& action_val) {
  if (action_val.type == Value::DICTIONARY) {
    return parse_action(pdf, action_val.dict);
  }
  if (action_val.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, action_val.ref_object_number,
                                      action_val.ref_generation_number);
    if (resolved.success && resolved.value.type == Value::DICTIONARY) {
      return parse_action(pdf, resolved.value.dict);
    }
  }
  return Action{};
}

// Parse Additional Actions (/AA) dictionary
AdditionalActions parse_additional_actions(const Pdf& pdf, const Dictionary& aa_dict) {
  AdditionalActions aa;

  auto parse_entry = [&](const std::string& key) -> Action {
    auto it = aa_dict.find(key);
    if (it == aa_dict.end()) return Action{};
    return parse_action(pdf, it->second);
  };

  // Page-level actions
  aa.on_open = parse_entry("O");
  aa.on_close = parse_entry("C");
  aa.on_visible = parse_entry("PV");
  aa.on_invisible = parse_entry("PI");

  // Document-level actions
  aa.will_close = parse_entry("WC");
  aa.will_save = parse_entry("WS");
  aa.did_save = parse_entry("DS");
  aa.will_print = parse_entry("WP");
  aa.did_print = parse_entry("DP");

  return aa;
}

// Parse document-level actions (/OpenAction and /AA)
void parse_document_actions(const Pdf& pdf, DocumentCatalog& catalog) {
  auto catalog_obj = resolve_reference(pdf, pdf.root, 0);
  if (!catalog_obj.success || catalog_obj.value.type != Value::DICTIONARY) return;

  const auto& cat_dict = catalog_obj.value.dict;

  // Parse /OpenAction
  auto oa_it = cat_dict.find("OpenAction");
  if (oa_it != cat_dict.end()) {
    if (oa_it->second.type == Value::DICTIONARY) {
      catalog.open_action = parse_action(pdf, oa_it->second.dict);
    } else if (oa_it->second.type == Value::REFERENCE) {
      catalog.open_action = parse_action(pdf, oa_it->second);
    } else if (oa_it->second.type == Value::ARRAY) {
      // Direct destination array (not an action dict)
      Action action;
      action.type = ActionType::GoTo;
      if (!oa_it->second.array.empty()) {
        const auto& page_ref = oa_it->second.array[0];
        if (page_ref.type == Value::REFERENCE)
          action.dest_page = page_ref.ref_object_number;
        else if (page_ref.type == Value::NUMBER)
          action.dest_page = static_cast<uint32_t>(page_ref.number);
        if (oa_it->second.array.size() > 1 && oa_it->second.array[1].type == Value::NAME)
          action.dest_fit_type = oa_it->second.array[1].name;
        for (size_t i = 2; i < oa_it->second.array.size(); ++i) {
          if (oa_it->second.array[i].type == Value::NUMBER)
            action.dest_position.push_back(oa_it->second.array[i].number);
        }
      }
      catalog.open_action = action;
    }
  }

  // Parse /AA (document-level additional actions)
  auto aa_it = cat_dict.find("AA");
  if (aa_it != cat_dict.end()) {
    const Value* aa_val = &aa_it->second;
    ResolvedObject aa_resolved;
    if (aa_val->type == Value::REFERENCE) {
      aa_resolved = resolve_reference(pdf, aa_val->ref_object_number,
                                      aa_val->ref_generation_number);
      if (aa_resolved.success) aa_val = &aa_resolved.value;
    }
    if (aa_val->type == Value::DICTIONARY) {
      catalog.additional_actions = parse_additional_actions(pdf, aa_val->dict);
    }
  }
}

// Parse /OutputIntents array from document catalog
void parse_output_intents(const Pdf& pdf, DocumentCatalog& catalog) {
  auto catalog_obj = resolve_reference(pdf, pdf.root, 0);
  if (!catalog_obj.success || catalog_obj.value.type != Value::DICTIONARY) {
    return;
  }

  const auto& catalog_dict = catalog_obj.value.dict;
  auto oi_it = catalog_dict.find("OutputIntents");
  if (oi_it == catalog_dict.end()) return;

  const Value* oi_val = &oi_it->second;
  // Resolve reference if needed
  ResolvedObject oi_resolved;
  if (oi_val->type == Value::REFERENCE) {
    oi_resolved = resolve_reference(pdf, oi_val->ref_object_number,
                                    oi_val->ref_generation_number);
    if (!oi_resolved.success) return;
    oi_val = &oi_resolved.value;
  }

  if (oi_val->type != Value::ARRAY) return;

  for (const auto& item : oi_val->array) {
    const Value* intent_val = &item;
    ResolvedObject intent_resolved;
    if (intent_val->type == Value::REFERENCE) {
      intent_resolved = resolve_reference(pdf, intent_val->ref_object_number,
                                          intent_val->ref_generation_number);
      if (!intent_resolved.success) continue;
      intent_val = &intent_resolved.value;
    }

    if (intent_val->type != Value::DICTIONARY) continue;
    const auto& d = intent_val->dict;

    OutputIntentInfo info;
    auto s_it = d.find("S");
    if (s_it != d.end() && s_it->second.type == Value::NAME)
      info.subtype = s_it->second.name;

    auto oc_it = d.find("OutputCondition");
    if (oc_it != d.end() && oc_it->second.type == Value::STRING)
      info.output_condition = oc_it->second.str;

    auto oci_it = d.find("OutputConditionIdentifier");
    if (oci_it != d.end() && oci_it->second.type == Value::STRING)
      info.output_condition_id = oci_it->second.str;

    auto rn_it = d.find("RegistryName");
    if (rn_it != d.end() && rn_it->second.type == Value::STRING)
      info.registry_name = rn_it->second.str;

    auto info_it = d.find("Info");
    if (info_it != d.end() && info_it->second.type == Value::STRING)
      info.info = info_it->second.str;

    // Resolve DestOutputProfile ICC stream
    auto dop_it = d.find("DestOutputProfile");
    if (dop_it != d.end() && dop_it->second.type == Value::REFERENCE) {
      auto dop_obj = resolve_reference(pdf, dop_it->second.ref_object_number,
                                       dop_it->second.ref_generation_number);
      if (dop_obj.success && dop_obj.value.type == Value::STREAM) {
        auto decoded = decode_stream(pdf, dop_obj.value);
        if (decoded.success) {
          info.dest_output_profile = std::move(decoded.data);
        }
      }
    }

    catalog.output_intents.push_back(std::move(info));
  }
}

// Optional Content Properties helper methods
bool OptionalContentProperties::is_ocg_visible(uint32_t obj_num) const {
  // Check if explicitly in off_list
  for (uint32_t off_ref : off_list) {
    if (off_ref == obj_num) return false;
  }

  // Check if explicitly in on_list
  for (uint32_t on_ref : on_list) {
    if (on_ref == obj_num) return true;
  }

  // Check base_state
  if (base_state == "OFF") return false;

  // Default to visible if not explicitly off and base_state is "ON" or empty
  return true;
}

const OptionalContentGroup* OptionalContentProperties::find_ocg(uint32_t obj_num) const {
  for (const auto& ocg : ocgs) {
    if (ocg.object_number == obj_num) {
      return &ocg;
    }
  }
  return nullptr;
}

void OptionalContentProperties::set_ocg_visibility(uint32_t obj_num, bool visible) {
  // Find and update the OCG
  for (auto& ocg : ocgs) {
    if (ocg.object_number == obj_num) {
      ocg.visible = visible;
      break;
    }
  }

  // Update on/off lists
  if (visible) {
    // Remove from off_list if present
    off_list.erase(
        std::remove(off_list.begin(), off_list.end(), obj_num),
        off_list.end());
    // Add to on_list if not present
    if (std::find(on_list.begin(), on_list.end(), obj_num) == on_list.end()) {
      on_list.push_back(obj_num);
    }
  } else {
    // Remove from on_list if present
    on_list.erase(
        std::remove(on_list.begin(), on_list.end(), obj_num),
        on_list.end());
    // Add to off_list if not present
    if (std::find(off_list.begin(), off_list.end(), obj_num) == off_list.end()) {
      off_list.push_back(obj_num);
    }
  }
}

// Parse Optional Content properties from document catalog
void parse_optional_content(const Pdf& pdf, DocumentCatalog& catalog) {
  // Look for OCProperties entry in document catalog
  auto catalog_obj = resolve_reference(pdf, pdf.root, 0);
  if (!catalog_obj.success || catalog_obj.value.type != Value::DICTIONARY) {
    return;
  }

  const auto& catalog_dict = catalog_obj.value.dict;
  auto ocprops_it = catalog_dict.find("OCProperties");
  if (ocprops_it == catalog_dict.end()) {
    return;  // No optional content in this document
  }

  Value ocprops_val = ocprops_it->second;
  if (ocprops_val.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, ocprops_val.ref_object_number,
                                       ocprops_val.ref_generation_number);
    if (!resolved.success) return;
    ocprops_val = resolved.value;
  }

  if (ocprops_val.type != Value::DICTIONARY) return;
  const auto& ocprops_dict = ocprops_val.dict;

  // Parse OCGs array - list of all OCG dictionaries
  auto ocgs_it = ocprops_dict.find("OCGs");
  if (ocgs_it != ocprops_dict.end()) {
    Value ocgs_val = ocgs_it->second;
    if (ocgs_val.type == Value::REFERENCE) {
      auto resolved = resolve_reference(pdf, ocgs_val.ref_object_number,
                                         ocgs_val.ref_generation_number);
      if (resolved.success) ocgs_val = resolved.value;
    }

    if (ocgs_val.type == Value::ARRAY) {
      for (const auto& ocg_ref : ocgs_val.array) {
        if (ocg_ref.type == Value::REFERENCE) {
          auto ocg_obj = resolve_reference(pdf, ocg_ref.ref_object_number,
                                            ocg_ref.ref_generation_number);
          if (ocg_obj.success && ocg_obj.value.type == Value::DICTIONARY) {
            OptionalContentGroup ocg;
            ocg.object_number = ocg_ref.ref_object_number;
            ocg.generation_number = ocg_ref.ref_generation_number;

            const auto& ocg_dict = ocg_obj.value.dict;

            // Get OCG name
            auto name_it = ocg_dict.find("Name");
            if (name_it != ocg_dict.end() && name_it->second.type == Value::STRING) {
              ocg.name = name_it->second.str;
            }

            // Get intent
            auto intent_it = ocg_dict.find("Intent");
            if (intent_it != ocg_dict.end()) {
              if (intent_it->second.type == Value::NAME) {
                ocg.intent = intent_it->second.name;
              } else if (intent_it->second.type == Value::ARRAY && !intent_it->second.array.empty()) {
                if (intent_it->second.array[0].type == Value::NAME) {
                  ocg.intent = intent_it->second.array[0].name;
                }
              }
            }

            // Parse Usage dictionary if present
            auto usage_it = ocg_dict.find("Usage");
            if (usage_it != ocg_dict.end() && usage_it->second.type == Value::DICTIONARY) {
              const auto& usage_dict = usage_it->second.dict;

              // Print usage
              auto print_it = usage_dict.find("Print");
              if (print_it != usage_dict.end() && print_it->second.type == Value::DICTIONARY) {
                auto state_it = print_it->second.dict.find("PrintState");
                if (state_it != print_it->second.dict.end() &&
                    state_it->second.type == Value::NAME &&
                    state_it->second.name == "OFF") {
                  ocg.print_never = true;
                }
              }

              // View usage
              auto view_it = usage_dict.find("View");
              if (view_it != usage_dict.end() && view_it->second.type == Value::DICTIONARY) {
                auto state_it = view_it->second.dict.find("ViewState");
                if (state_it != view_it->second.dict.end() &&
                    state_it->second.type == Value::NAME &&
                    state_it->second.name == "OFF") {
                  ocg.view_never = true;
                }
              }
            }

            catalog.ocg_properties.ocgs.push_back(ocg);
          }
        }
      }
    }
  }

  // Parse D (default configuration)
  auto d_it = ocprops_dict.find("D");
  if (d_it != ocprops_dict.end()) {
    Value d_val = d_it->second;
    if (d_val.type == Value::REFERENCE) {
      auto resolved = resolve_reference(pdf, d_val.ref_object_number,
                                         d_val.ref_generation_number);
      if (resolved.success) d_val = resolved.value;
    }

    if (d_val.type == Value::DICTIONARY) {
      const auto& d_dict = d_val.dict;

      // BaseState
      auto basestate_it = d_dict.find("BaseState");
      if (basestate_it != d_dict.end() && basestate_it->second.type == Value::NAME) {
        catalog.ocg_properties.base_state = basestate_it->second.name;
      } else {
        catalog.ocg_properties.base_state = "ON";  // Default
      }

      // ON array
      auto on_it = d_dict.find("ON");
      if (on_it != d_dict.end() && on_it->second.type == Value::ARRAY) {
        for (const auto& ref : on_it->second.array) {
          if (ref.type == Value::REFERENCE) {
            catalog.ocg_properties.on_list.push_back(ref.ref_object_number);
          }
        }
      }

      // OFF array
      auto off_it = d_dict.find("OFF");
      if (off_it != d_dict.end() && off_it->second.type == Value::ARRAY) {
        for (const auto& ref : off_it->second.array) {
          if (ref.type == Value::REFERENCE) {
            catalog.ocg_properties.off_list.push_back(ref.ref_object_number);
          }
        }
      }

      // Locked array
      auto locked_it = d_dict.find("Locked");
      if (locked_it != d_dict.end() && locked_it->second.type == Value::ARRAY) {
        for (const auto& ref : locked_it->second.array) {
          if (ref.type == Value::REFERENCE) {
            catalog.ocg_properties.locked.push_back(ref.ref_object_number);
            // Mark OCG as locked
            for (auto& ocg : catalog.ocg_properties.ocgs) {
              if (ocg.object_number == ref.ref_object_number) {
                ocg.locked = true;
                break;
              }
            }
          }
        }
      }

      // Order array (simplified - just extract OCG references)
      auto order_it = d_dict.find("Order");
      if (order_it != d_dict.end() && order_it->second.type == Value::ARRAY) {
        int creator_order = 0;
        for (const auto& item : order_it->second.array) {
          if (item.type == Value::REFERENCE) {
            catalog.ocg_properties.order.push_back(item.ref_object_number);
            // Set creator order for the OCG
            for (auto& ocg : catalog.ocg_properties.ocgs) {
              if (ocg.object_number == item.ref_object_number) {
                ocg.creator_order = creator_order++;
                break;
              }
            }
          }
        }
      }
    }
  }

  // Set initial visibility based on default configuration
  for (auto& ocg : catalog.ocg_properties.ocgs) {
    ocg.visible = catalog.ocg_properties.is_ocg_visible(ocg.object_number);
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

std::unique_ptr<Pattern> parse_pattern(const Pdf& pdf, const Value& value,
                                       uint32_t obj_num, uint16_t gen_num) {
  const Value* pv = &value;
  Value resolved_holder;
  if (pv->type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, pv->ref_object_number,
                                      pv->ref_generation_number);
    if (!resolved.success) return nullptr;
    if (obj_num == 0) {
      obj_num = pv->ref_object_number;
      gen_num = pv->ref_generation_number;
    }
    resolved_holder = std::move(resolved.value);
    pv = &resolved_holder;
  }

  const Dictionary* dict = nullptr;
  if (pv->type == Value::DICTIONARY) {
    dict = &pv->dict;
  } else if (pv->type == Value::STREAM) {
    dict = &pv->stream.dict;
  } else {
    return nullptr;
  }

  auto pattern = parse_pattern(pdf, *dict);
  if (!pattern) return nullptr;

  // Tiling patterns carry their drawing operators in the stream body; the
  // dictionary-only path can't see it, so decode and attach here.
  if (pattern->type == PatternType::Tiling && pattern->tiling &&
      pv->type == Value::STREAM) {
    auto decoded = decode_stream(pdf, *pv, obj_num, gen_num);
    if (decoded.success) {
      pattern->tiling->content_stream = std::move(decoded.data);
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

// =============================================================================
// Text Search API
// =============================================================================

namespace {

// Convert a string to lowercase for case-insensitive comparison
std::string to_lower_str(const std::string& s) {
  std::string result = s;
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));
  }
  return result;
}

// Get surrounding context text around a match position
std::string get_context(const std::string& text, size_t match_start,
                        size_t match_length, size_t context_chars = 30) {
  size_t ctx_start = match_start > context_chars ? match_start - context_chars : 0;
  size_t ctx_end = std::min(match_start + match_length + context_chars, text.size());
  return text.substr(ctx_start, ctx_end - ctx_start);
}

struct SearchCharRef {
  size_t line_idx{0};
  size_t char_in_line{0};
  bool is_text_char{false};
};

struct SearchablePageText {
  std::string text;
  std::vector<SearchCharRef> char_refs;
};

struct TrigramIndex {
  std::string text;
  std::unordered_map<uint32_t, std::vector<size_t>> postings;
};

struct SearchCandidate {
  size_t start{0};
  size_t length{0};
  double score{0.0};
  bool fuzzy{false};
};

void append_utf8(uint32_t unicode, std::string* out,
                 std::vector<SearchCharRef>* char_refs,
                 const SearchCharRef& ref) {
  if (unicode < 0x80) {
    out->push_back(static_cast<char>(unicode));
    char_refs->push_back(ref);
  } else if (unicode < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (unicode >> 6)));
    char_refs->push_back(ref);
    out->push_back(static_cast<char>(0x80 | (unicode & 0x3F)));
    char_refs->push_back(ref);
  } else if (unicode < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (unicode >> 12)));
    char_refs->push_back(ref);
    out->push_back(static_cast<char>(0x80 | ((unicode >> 6) & 0x3F)));
    char_refs->push_back(ref);
    out->push_back(static_cast<char>(0x80 | (unicode & 0x3F)));
    char_refs->push_back(ref);
  } else {
    out->push_back(static_cast<char>(0xF0 | (unicode >> 18)));
    char_refs->push_back(ref);
    out->push_back(static_cast<char>(0x80 | ((unicode >> 12) & 0x3F)));
    char_refs->push_back(ref);
    out->push_back(static_cast<char>(0x80 | ((unicode >> 6) & 0x3F)));
    char_refs->push_back(ref);
    out->push_back(static_cast<char>(0x80 | (unicode & 0x3F)));
    char_refs->push_back(ref);
  }
}

SearchablePageText build_searchable_page_text(const TextPage& text_page) {
  SearchablePageText result;

  std::vector<size_t> line_order(text_page.lines.size());
  for (size_t i = 0; i < line_order.size(); ++i) {
    line_order[i] = i;
  }

  std::sort(line_order.begin(), line_order.end(),
            [&](size_t a, size_t b) {
              return text_page.lines[a].reading_order <
                     text_page.lines[b].reading_order;
            });

  for (size_t li : line_order) {
    const TextLine& line = text_page.lines[li];
    for (size_t ci = 0; ci < line.chars.size(); ++ci) {
      append_utf8(line.chars[ci].unicode, &result.text, &result.char_refs,
                  SearchCharRef{li, ci, true});
    }

    result.text.push_back('\n');
    result.char_refs.push_back(SearchCharRef{li, line.chars.size(), false});
  }

  return result;
}

uint32_t make_trigram_key(unsigned char c0, unsigned char c1, unsigned char c2) {
  return (static_cast<uint32_t>(c0) << 16) |
         (static_cast<uint32_t>(c1) << 8) |
         static_cast<uint32_t>(c2);
}

std::vector<uint32_t> collect_trigrams(const std::string& text) {
  std::vector<uint32_t> trigrams;
  if (text.size() < 3) {
    return trigrams;
  }

  trigrams.reserve(text.size() - 2);
  for (size_t i = 0; i + 2 < text.size(); ++i) {
    trigrams.push_back(make_trigram_key(
        static_cast<unsigned char>(text[i]),
        static_cast<unsigned char>(text[i + 1]),
        static_cast<unsigned char>(text[i + 2])));
  }
  return trigrams;
}

TrigramIndex build_trigram_index(const std::string& text) {
  TrigramIndex index;
  index.text = text;

  if (text.size() < 3) {
    return index;
  }

  index.postings.reserve(text.size());
  for (size_t i = 0; i + 2 < text.size(); ++i) {
    const uint32_t key = make_trigram_key(
        static_cast<unsigned char>(text[i]),
        static_cast<unsigned char>(text[i + 1]),
        static_cast<unsigned char>(text[i + 2]));
    index.postings[key].push_back(i);
  }

  return index;
}

std::vector<size_t> find_query_positions(const TrigramIndex& index,
                                         const std::string& query) {
  std::vector<size_t> matches;

  if (query.empty() || index.text.size() < query.size()) {
    return matches;
  }

  if (query.size() < 3) {
    size_t pos = 0;
    while ((pos = index.text.find(query, pos)) != std::string::npos) {
      matches.push_back(pos);
      pos += 1;
    }
    return matches;
  }

  const uint32_t key = make_trigram_key(
      static_cast<unsigned char>(query[0]),
      static_cast<unsigned char>(query[1]),
      static_cast<unsigned char>(query[2]));
  auto it = index.postings.find(key);
  if (it == index.postings.end()) {
    return matches;
  }

  for (size_t pos : it->second) {
    if (pos + query.size() > index.text.size()) {
      continue;
    }
    if (index.text.compare(pos, query.size(), query) == 0) {
      matches.push_back(pos);
    }
  }

  return matches;
}

size_t levenshtein_distance_bounded(const std::string& a, const std::string& b,
                                    size_t max_distance) {
  if (a == b) {
    return 0;
  }

  const size_t a_size = a.size();
  const size_t b_size = b.size();
  const size_t len_gap = (a_size > b_size) ? (a_size - b_size) : (b_size - a_size);
  if (len_gap > max_distance) {
    return max_distance + 1;
  }

  std::vector<size_t> prev(b_size + 1);
  std::vector<size_t> curr(b_size + 1);
  for (size_t j = 0; j <= b_size; ++j) {
    prev[j] = j;
  }

  for (size_t i = 0; i < a_size; ++i) {
    curr[0] = i + 1;
    size_t row_best = curr[0];
    for (size_t j = 0; j < b_size; ++j) {
      const size_t cost = (a[i] == b[j]) ? 0 : 1;
      curr[j + 1] = std::min(std::min(curr[j] + 1, prev[j + 1] + 1), prev[j] + cost);
      row_best = std::min(row_best, curr[j + 1]);
    }

    if (row_best > max_distance) {
      return max_distance + 1;
    }

    prev.swap(curr);
  }

  return prev[b_size];
}

double trigram_similarity(const std::vector<uint32_t>& query_trigrams,
                          const std::string& candidate) {
  if (query_trigrams.empty()) {
    return candidate.empty() ? 1.0 : 0.0;
  }

  std::unordered_set<uint32_t> query_set(query_trigrams.begin(), query_trigrams.end());
  std::vector<uint32_t> candidate_trigrams = collect_trigrams(candidate);
  if (candidate_trigrams.empty()) {
    return 0.0;
  }

  std::unordered_set<uint32_t> candidate_set(candidate_trigrams.begin(), candidate_trigrams.end());
  size_t intersection = 0;
  for (uint32_t tri : query_set) {
    if (candidate_set.count(tri)) {
      intersection++;
    }
  }

  const size_t union_count = query_set.size() + candidate_set.size() - intersection;
  if (union_count == 0) {
    return 0.0;
  }

  return static_cast<double>(intersection) / static_cast<double>(union_count);
}

std::vector<SearchCandidate> find_fuzzy_positions(const TrigramIndex& index,
                                                  const std::string& query) {
  std::vector<SearchCandidate> matches;
  if (query.size() < 4 || index.text.empty()) {
    return matches;
  }

  const std::vector<uint32_t> query_trigrams = collect_trigrams(query);
  if (query_trigrams.empty()) {
    return matches;
  }

  std::unordered_map<size_t, int> vote_map;
  for (size_t qoff = 0; qoff + 2 < query.size(); ++qoff) {
    const uint32_t key = query_trigrams[qoff];
    auto it = index.postings.find(key);
    if (it == index.postings.end()) {
      continue;
    }

    for (size_t pos : it->second) {
      if (pos < qoff) {
        continue;
      }
      vote_map[pos - qoff] += 1;
    }
  }

  if (vote_map.empty()) {
    return matches;
  }

  std::vector<std::pair<size_t, int>> ranked(vote_map.begin(), vote_map.end());
  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) {
      return a.second > b.second;
    }
    return a.first < b.first;
  });

  const size_t max_edits = std::max<size_t>(1, query.size() / 4);
  const int min_votes = std::max<int>(1, static_cast<int>(query_trigrams.size() / 3));
  std::unordered_set<size_t> seen;

  for (size_t i = 0; i < ranked.size() && i < 64; ++i) {
    const size_t start = ranked[i].first;
    const int votes = ranked[i].second;
    if (votes < min_votes || start >= index.text.size()) {
      continue;
    }

    const size_t min_len = (query.size() > max_edits) ? (query.size() - max_edits) : 1;
    const size_t max_len = std::min(index.text.size() - start, query.size() + max_edits);
    for (size_t cand_len = min_len; cand_len <= max_len; ++cand_len) {
      const std::string candidate = index.text.substr(start, cand_len);
      const double tri_score = trigram_similarity(query_trigrams, candidate);
      if (tri_score < 0.25) {
        continue;
      }

      const size_t distance = levenshtein_distance_bounded(query, candidate, max_edits);
      if (distance > max_edits) {
        continue;
      }

      const double norm = static_cast<double>(std::max(query.size(), cand_len));
      const double edit_score = 1.0 - (static_cast<double>(distance) / norm);
      const double score = 0.55 * edit_score + 0.45 * tri_score;
      if (score < 0.55) {
        continue;
      }

      if (seen.insert(start).second) {
        matches.push_back(SearchCandidate{start, cand_len, score, true});
      }
      break;
    }
  }

  std::sort(matches.begin(), matches.end(), [](const SearchCandidate& a, const SearchCandidate& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.start < b.start;
  });

  return matches;
}

void populate_search_result_geometry(const SearchablePageText& searchable_page,
                                     const TextPage& text_page,
                                     size_t start, size_t match_length,
                                     TextSearchResult* result) {
  if (!result || start >= searchable_page.char_refs.size() || match_length == 0) {
    return;
  }

  const SearchCharRef& first_ref = searchable_page.char_refs[start];
  if (!first_ref.is_text_char ||
      first_ref.line_idx >= text_page.lines.size() ||
      first_ref.char_in_line >= text_page.lines[first_ref.line_idx].chars.size()) {
    return;
  }

  const TextChar& first_ch =
      text_page.lines[first_ref.line_idx].chars[first_ref.char_in_line];
  result->x = first_ch.x;
  result->y = first_ch.y;
  result->height = first_ch.height;
  result->writing_mode = first_ch.writing_mode;

  const size_t end_pos = start + match_length - 1;
  if (end_pos >= searchable_page.char_refs.size()) {
    return;
  }

  const SearchCharRef& last_ref = searchable_page.char_refs[end_pos];
  if (!last_ref.is_text_char ||
      last_ref.line_idx >= text_page.lines.size() ||
      last_ref.char_in_line >= text_page.lines[last_ref.line_idx].chars.size()) {
    return;
  }

  const TextChar& last_ch =
      text_page.lines[last_ref.line_idx].chars[last_ref.char_in_line];

  bool have_bounds = false;
  double min_x = 0.0;
  double min_y = 0.0;
  double max_x = 0.0;
  double max_y = 0.0;
  size_t current_line = std::numeric_limits<size_t>::max();
  TextQuad current_quad;
  bool have_quad = false;

  auto merge_char = [&](const TextChar& ch) {
    if (!have_bounds) {
      min_x = ch.quad.x;
      min_y = ch.quad.y;
      max_x = ch.quad.x + ch.quad.width;
      max_y = ch.quad.y + ch.quad.height;
      have_bounds = true;
    } else {
      min_x = std::min(min_x, ch.quad.x);
      min_y = std::min(min_y, ch.quad.y);
      max_x = std::max(max_x, ch.quad.x + ch.quad.width);
      max_y = std::max(max_y, ch.quad.y + ch.quad.height);
    }

    if (!have_quad || current_line != ch.line_index) {
      if (have_quad) {
        result->quads.push_back(current_quad);
      }
      current_quad = ch.quad;
      current_line = static_cast<size_t>(std::max(ch.line_index, 0));
      have_quad = true;
    } else {
      const double qx1 = std::min(current_quad.x, ch.quad.x);
      const double qy1 = std::min(current_quad.y, ch.quad.y);
      const double qx2 = std::max(current_quad.x + current_quad.width,
                                  ch.quad.x + ch.quad.width);
      const double qy2 = std::max(current_quad.y + current_quad.height,
                                  ch.quad.y + ch.quad.height);
      current_quad.x = qx1;
      current_quad.y = qy1;
      current_quad.width = qx2 - qx1;
      current_quad.height = qy2 - qy1;
      current_quad.x1 = qx1;
      current_quad.y1 = qy1;
      current_quad.x2 = qx2;
      current_quad.y2 = qy1;
      current_quad.x3 = qx2;
      current_quad.y3 = qy2;
      current_quad.x4 = qx1;
      current_quad.y4 = qy2;
    }
  };

  for (size_t pos = start; pos <= end_pos && pos < searchable_page.char_refs.size(); ++pos) {
    const SearchCharRef& ref = searchable_page.char_refs[pos];
    if (!ref.is_text_char ||
        ref.line_idx >= text_page.lines.size() ||
        ref.char_in_line >= text_page.lines[ref.line_idx].chars.size()) {
      continue;
    }
    merge_char(text_page.lines[ref.line_idx].chars[ref.char_in_line]);
  }
  if (have_quad) {
    result->quads.push_back(current_quad);
  }

  if (have_bounds) {
    result->x = min_x;
    result->y = min_y;
    result->width = max_x - min_x;
    result->height = max_y - min_y;
  } else {
    result->width = (last_ch.x + last_ch.width) - result->x;
    result->height = std::max(result->height, last_ch.height);
  }
}

std::vector<TextSearchResult> build_results_from_candidates(
    const std::vector<SearchCandidate>& candidates,
    const std::string& original_text,
    const SearchablePageText* searchable_page,
    const TextPage* text_page,
    const std::string& query) {
  std::vector<TextSearchResult> results;
  for (const auto& candidate : candidates) {
    TextSearchResult result;
    result.page_number = 0;
    result.char_index = candidate.start;
    result.length = candidate.length;
    result.context = get_context(original_text, candidate.start, candidate.length);
    result.score = candidate.score;
    result.fuzzy = candidate.fuzzy;

    if (searchable_page && text_page) {
      populate_search_result_geometry(*searchable_page, *text_page,
                                      candidate.start, candidate.length, &result);
    }

    results.push_back(std::move(result));
  }
  return results;
}

}  // anonymous namespace

std::vector<TextSearchResult> search_text_on_page(const Pdf& pdf,
                                                  const Page& page,
                                                  const std::string& query,
                                                  bool case_sensitive) {
  std::vector<TextSearchResult> results;

  if (query.empty()) {
    return results;
  }

  std::string page_text = extract_text_from_page(pdf, page);
  if (page_text.empty()) {
    return results;
  }

  std::string search_text_str = case_sensitive ? page_text : to_lower_str(page_text);
  std::string search_query = case_sensitive ? query : to_lower_str(query);

  std::unique_ptr<TextPage> text_page = extract_text_layout(pdf, page);

  if (text_page && !text_page->chars.empty()) {
    SearchablePageText searchable_page = build_searchable_page_text(*text_page);
    if (!searchable_page.char_refs.empty()) {
      std::string search_layout = case_sensitive
                                      ? searchable_page.text
                                      : to_lower_str(searchable_page.text);
      TrigramIndex index = build_trigram_index(search_layout);

      std::vector<SearchCandidate> exact_candidates;
      for (size_t pos : find_query_positions(index, search_query)) {
        exact_candidates.push_back(SearchCandidate{pos, search_query.size(), 1.0, false});
      }

      results = build_results_from_candidates(exact_candidates, searchable_page.text,
                                              &searchable_page, text_page.get(),
                                              query);
      if (!results.empty()) {
        return results;
      }

      std::vector<SearchCandidate> fuzzy_candidates =
          find_fuzzy_positions(index, search_query);
      results = build_results_from_candidates(fuzzy_candidates, searchable_page.text,
                                              &searchable_page, text_page.get(),
                                              query);
      if (!results.empty()) {
        return results;
      }
    }
  }

  std::vector<SearchCandidate> plain_exact_candidates;
  size_t pos = 0;
  while ((pos = search_text_str.find(search_query, pos)) != std::string::npos) {
    plain_exact_candidates.push_back(SearchCandidate{pos, search_query.size(), 1.0, false});
    pos += search_query.size();
  }

  results = build_results_from_candidates(plain_exact_candidates, page_text, nullptr, nullptr, query);
  if (!results.empty()) {
    return results;
  }

  TrigramIndex plain_index = build_trigram_index(search_text_str);
  std::vector<SearchCandidate> plain_fuzzy_candidates =
      find_fuzzy_positions(plain_index, search_query);
  return build_results_from_candidates(plain_fuzzy_candidates, page_text, nullptr, nullptr, query);
}

std::vector<TextSearchResult> search_text(const Pdf& pdf,
                                          const std::string& query,
                                          bool case_sensitive,
                                          int max_results) {
  std::vector<TextSearchResult> results;

  if (query.empty()) {
    return results;
  }

  for (size_t page_idx = 0; page_idx < pdf.catalog.pages.size();
       ++page_idx) {
    const Page& page = pdf.catalog.pages[page_idx];
    std::vector<TextSearchResult> page_results =
        search_text_on_page(pdf, page, query, case_sensitive);

    for (auto& r : page_results) {
      r.page_number = static_cast<uint32_t>(page_idx);
      results.push_back(std::move(r));

      if (max_results > 0 &&
          static_cast<int>(results.size()) >= max_results) {
        return results;
      }
    }
  }

  return results;
}

// ---------------------------------------------------------------------------
// Digital signature validation
// ---------------------------------------------------------------------------

namespace {

// ASN.1 DER tag constants
static const uint8_t kASN1_SEQUENCE = 0x30;
static const uint8_t kASN1_SET = 0x31;
static const uint8_t kASN1_OID = 0x06;
static const uint8_t kASN1_OCTET_STRING = 0x04;
static const uint8_t kASN1_UTF8_STRING = 0x0C;
static const uint8_t kASN1_PRINTABLE_STRING = 0x13;
static const uint8_t kASN1_IA5_STRING = 0x16;
static const uint8_t kASN1_T61_STRING = 0x14;
static const uint8_t kASN1_BMP_STRING = 0x1E;
static const uint8_t kASN1_UTC_TIME = 0x17;
static const uint8_t kASN1_GENERALIZED_TIME = 0x18;

// OID for PKCS#7 signedData (1.2.840.113549.1.7.2)
static const uint8_t kOID_SignedData[] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02};

// OID for messageDigest attribute (1.2.840.113549.1.9.4)
static const uint8_t kOID_MessageDigest[] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x04};

// OID for signingTime attribute (1.2.840.113549.1.9.5)
static const uint8_t kOID_SigningTime[] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x05};

// OID for SHA-256 (2.16.840.1.101.3.4.2.1)
static const uint8_t kOID_SHA256[] = {
    0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01};

// OID for SHA-1 (1.3.14.3.2.26)
static const uint8_t kOID_SHA1[] = {0x2B, 0x0E, 0x03, 0x02, 0x1A};

// OID for SHA-384 (2.16.840.1.101.3.4.2.2)
static const uint8_t kOID_SHA384[] = {
    0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02};

// OID for SHA-512 (2.16.840.1.101.3.4.2.3)
static const uint8_t kOID_SHA512[] = {
    0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03};

// OID for MD5 (1.2.840.113549.2.5)
static const uint8_t kOID_MD5[] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x02, 0x05};

// OID for id-at-commonName (2.5.4.3)
static const uint8_t kOID_CommonName[] = {0x55, 0x04, 0x03};

// A minimal ASN.1 DER element
struct DERElement {
  uint8_t tag{0};
  const uint8_t* content{nullptr};  // pointer into the original data
  size_t content_length{0};
  const uint8_t* full_start{nullptr};  // pointer to start of TLV
  size_t full_length{0};               // total TLV length
  bool constructed{false};

  bool is_sequence() const { return tag == kASN1_SEQUENCE; }
  bool is_set() const { return tag == kASN1_SET; }
  bool is_context(uint8_t n) const { return tag == (0xA0 | n); }
  bool is_oid() const { return tag == kASN1_OID; }
  bool is_octet_string() const { return tag == kASN1_OCTET_STRING; }

  bool is_string_type() const {
    return tag == kASN1_UTF8_STRING || tag == kASN1_PRINTABLE_STRING ||
           tag == kASN1_IA5_STRING || tag == kASN1_T61_STRING ||
           tag == kASN1_BMP_STRING;
  }

  bool is_time_type() const {
    return tag == kASN1_UTC_TIME || tag == kASN1_GENERALIZED_TIME;
  }

  // Extract string value from a string-type element
  std::string to_string() const {
    if (!content || content_length == 0) return "";
    if (tag == kASN1_BMP_STRING) {
      // BMP string is UCS-2 big-endian; convert to ASCII (lossy)
      std::string result;
      for (size_t i = 0; i + 1 < content_length; i += 2) {
        uint16_t ch = (static_cast<uint16_t>(content[i]) << 8) | content[i + 1];
        if (ch < 0x80) {
          result.push_back(static_cast<char>(ch));
        } else {
          result.push_back('?');
        }
      }
      return result;
    }
    return std::string(reinterpret_cast<const char*>(content), content_length);
  }

  // Check if OID matches
  bool oid_equals(const uint8_t* oid, size_t oid_len) const {
    if (tag != kASN1_OID) return false;
    if (content_length != oid_len) return false;
    return std::memcmp(content, oid, oid_len) == 0;
  }
};

// Parse one DER element from the given position. Returns false on error.
// On success, advances `pos` past the element.
bool parse_der_element(const uint8_t* data, size_t data_len, size_t& pos,
                       DERElement& out) {
  if (pos >= data_len) return false;

  out.full_start = data + pos;
  out.tag = data[pos];
  out.constructed = (out.tag & 0x20) != 0;
  pos++;

  // Handle high-tag-number form (tag number >= 31)
  if ((out.tag & 0x1F) == 0x1F) {
    // Skip subsequent tag bytes (each has bit 7 set except the last)
    while (pos < data_len && (data[pos] & 0x80)) {
      pos++;
    }
    if (pos >= data_len) return false;
    pos++;  // skip last tag byte
  }

  // Parse length
  if (pos >= data_len) return false;
  uint8_t len_byte = data[pos];
  pos++;

  size_t length = 0;
  if (len_byte < 0x80) {
    length = len_byte;
  } else if (len_byte == 0x80) {
    // Indefinite length - not supported in DER, but handle gracefully
    return false;
  } else {
    size_t num_bytes = len_byte & 0x7F;
    if (num_bytes > 4 || pos + num_bytes > data_len) return false;
    for (size_t i = 0; i < num_bytes; i++) {
      length = (length << 8) | data[pos];
      pos++;
    }
  }

  if (pos + length > data_len) return false;

  out.content = data + pos;
  out.content_length = length;
  out.full_length = static_cast<size_t>((data + pos + length) - out.full_start);
  pos += length;
  return true;
}

// Parse all children elements inside a constructed element
bool parse_der_children(const DERElement& parent,
                        std::vector<DERElement>& children) {
  if (!parent.constructed || !parent.content) return false;
  size_t pos = 0;
  while (pos < parent.content_length) {
    DERElement child;
    if (!parse_der_element(parent.content, parent.content_length, pos,
                           child)) {
      return false;
    }
    children.push_back(child);
  }
  return true;
}

// Identify the digest algorithm from an AlgorithmIdentifier SEQUENCE
std::string identify_digest_algorithm(const DERElement& algo_id) {
  if (!algo_id.is_sequence()) return "";
  std::vector<DERElement> children;
  if (!parse_der_children(algo_id, children) || children.empty()) return "";
  if (!children[0].is_oid()) return "";

  if (children[0].oid_equals(kOID_SHA256, sizeof(kOID_SHA256)))
    return "SHA-256";
  if (children[0].oid_equals(kOID_SHA1, sizeof(kOID_SHA1))) return "SHA-1";
  if (children[0].oid_equals(kOID_SHA384, sizeof(kOID_SHA384)))
    return "SHA-384";
  if (children[0].oid_equals(kOID_SHA512, sizeof(kOID_SHA512)))
    return "SHA-512";
  if (children[0].oid_equals(kOID_MD5, sizeof(kOID_MD5))) return "MD5";

  return "Unknown";
}

// Extract Common Name from an X.500 Name (SEQUENCE of SETs of
// AttributeTypeAndValue)
std::string extract_common_name(const DERElement& name_seq) {
  if (!name_seq.is_sequence()) return "";

  std::vector<DERElement> rdns;
  if (!parse_der_children(name_seq, rdns)) return "";

  for (const auto& rdn : rdns) {
    if (!rdn.is_set()) continue;
    std::vector<DERElement> atvs;
    if (!parse_der_children(rdn, atvs)) continue;

    for (const auto& atv : atvs) {
      if (!atv.is_sequence()) continue;
      std::vector<DERElement> atv_children;
      if (!parse_der_children(atv, atv_children) || atv_children.size() < 2)
        continue;

      if (atv_children[0].oid_equals(kOID_CommonName,
                                     sizeof(kOID_CommonName))) {
        return atv_children[1].to_string();
      }
    }
  }
  return "";
}

// Parse UTCTime or GeneralizedTime to a readable string
std::string parse_asn1_time(const DERElement& time_elem) {
  if (!time_elem.is_time_type()) return "";
  std::string raw = time_elem.to_string();
  if (raw.empty()) return "";

  // UTCTime: YYMMDDHHmmSSZ
  // GeneralizedTime: YYYYMMDDHHmmSSZ
  if (time_elem.tag == kASN1_UTC_TIME && raw.size() >= 12) {
    int yy = std::atoi(raw.substr(0, 2).c_str());
    int year = (yy >= 50) ? 1900 + yy : 2000 + yy;
    return std::to_string(year) + "-" + raw.substr(2, 2) + "-" +
           raw.substr(4, 2) + " " + raw.substr(6, 2) + ":" +
           raw.substr(8, 2) + ":" + raw.substr(10, 2) + " UTC";
  }

  if (time_elem.tag == kASN1_GENERALIZED_TIME && raw.size() >= 14) {
    return raw.substr(0, 4) + "-" + raw.substr(4, 2) + "-" +
           raw.substr(6, 2) + " " + raw.substr(8, 2) + ":" +
           raw.substr(10, 2) + ":" + raw.substr(12, 2) + " UTC";
  }

  return raw;
}

// Search for a specific OID-keyed attribute in a SET OF Attribute
// Returns the value element if found.
bool find_attribute_value(const DERElement& attrs_elem, const uint8_t* oid,
                          size_t oid_len, DERElement& out_value) {
  // attrs_elem should be context [0] or SET wrapping attributes
  std::vector<DERElement> attrs;
  if (!parse_der_children(attrs_elem, attrs)) return false;

  for (const auto& attr : attrs) {
    // Each Attribute is a SEQUENCE { OID, SET OF values }
    if (!attr.is_sequence()) continue;
    std::vector<DERElement> attr_children;
    if (!parse_der_children(attr, attr_children) || attr_children.size() < 2)
      continue;

    if (!attr_children[0].is_oid()) continue;
    if (!attr_children[0].oid_equals(oid, oid_len)) continue;

    // The value is in a SET
    if (!attr_children[1].is_set()) continue;
    std::vector<DERElement> value_set;
    if (!parse_der_children(attr_children[1], value_set) ||
        value_set.empty())
      continue;

    out_value = value_set[0];
    return true;
  }
  return false;
}

// Extract the raw OCTET STRING bytes of the messageDigest attribute
bool extract_message_digest(const DERElement& attrs_elem,
                            std::vector<uint8_t>& digest) {
  DERElement value_elem;
  if (!find_attribute_value(attrs_elem, kOID_MessageDigest,
                            sizeof(kOID_MessageDigest), value_elem)) {
    return false;
  }

  if (value_elem.is_octet_string() && value_elem.content &&
      value_elem.content_length > 0) {
    digest.assign(value_elem.content,
                  value_elem.content + value_elem.content_length);
    return true;
  }
  return false;
}

// Extract the signing time string
bool extract_signing_time(const DERElement& attrs_elem,
                          std::string& signing_time) {
  DERElement value_elem;
  if (!find_attribute_value(attrs_elem, kOID_SigningTime,
                            sizeof(kOID_SigningTime), value_elem)) {
    return false;
  }

  if (value_elem.is_time_type()) {
    signing_time = parse_asn1_time(value_elem);
    return true;
  }
  return false;
}

}  // anonymous namespace

SignatureValidationResult validate_signature(const Pdf& pdf,
                                             const SignatureField& field) {
  SignatureValidationResult result;

  // --- Basic precondition checks ---
  if (!field.signature_present) {
    result.error = "Signature field is not signed";
    return result;
  }

  if (!field.byte_range_valid || field.byte_range.empty()) {
    result.error = "ByteRange is missing or invalid";
    return result;
  }

  if (field.signature_contents.empty()) {
    result.error = "Signature Contents is empty";
    return result;
  }

  if (!pdf.data || pdf.data_size == 0) {
    result.error = "PDF data is not available";
    return result;
  }

  // --- Step (a): Compute SHA-256 hash of byte-range-covered data ---
  // Reuse the same logic as compute_signature_digest.
  nanopdf::crypto::SHA256 sha256;
  bool has_data = false;

  for (size_t i = 0; i < field.byte_range.size(); i += 2) {
    uint64_t offset = field.byte_range[i];
    uint64_t length = field.byte_range[i + 1];

    if (length == 0) continue;
    if (offset > pdf.data_size || length > pdf.data_size) {
      result.error = "ByteRange offset/length exceeds PDF data size";
      return result;
    }
    if (offset + length < offset || offset + length > pdf.data_size) {
      result.error = "ByteRange overflow";
      return result;
    }

    sha256.update(pdf.data + static_cast<size_t>(offset),
                  static_cast<size_t>(length));
    has_data = true;
  }

  if (!has_data) {
    result.error = "ByteRange covers no data";
    return result;
  }

  sha256.finalize();
  std::vector<uint8_t> computed_digest(nanopdf::crypto::SHA256::DIGEST_SIZE);
  sha256.get_digest(computed_digest.data());

  // --- Step (b): Parse the PKCS#7/CMS SignedData from Contents ---
  const uint8_t* pkcs7_data = field.signature_contents.data();
  size_t pkcs7_size = field.signature_contents.size();

  // The outermost structure: ContentInfo ::= SEQUENCE { contentType, content }
  size_t pos = 0;
  DERElement content_info;
  if (!parse_der_element(pkcs7_data, pkcs7_size, pos, content_info) ||
      !content_info.is_sequence()) {
    result.error = "Failed to parse PKCS#7 ContentInfo outer SEQUENCE";
    return result;
  }

  std::vector<DERElement> ci_children;
  if (!parse_der_children(content_info, ci_children) ||
      ci_children.size() < 2) {
    result.error = "PKCS#7 ContentInfo has too few elements";
    return result;
  }

  // ci_children[0] should be OID for signedData
  if (!ci_children[0].oid_equals(kOID_SignedData, sizeof(kOID_SignedData))) {
    result.error = "ContentInfo is not signedData";
    return result;
  }

  // ci_children[1] should be [0] EXPLICIT wrapping the SignedData SEQUENCE
  if (!ci_children[1].is_context(0)) {
    result.error = "Missing [0] context wrapper for SignedData";
    return result;
  }

  // Inside the [0] context, parse the SignedData SEQUENCE
  std::vector<DERElement> ctx0_children;
  if (!parse_der_children(ci_children[1], ctx0_children) ||
      ctx0_children.empty() || !ctx0_children[0].is_sequence()) {
    result.error = "Failed to parse SignedData SEQUENCE";
    return result;
  }

  const DERElement& signed_data = ctx0_children[0];
  // SignedData ::= SEQUENCE {
  //   version          INTEGER,
  //   digestAlgorithms SET OF AlgorithmIdentifier,
  //   encapContentInfo EncapsulatedContentInfo,
  //   certificates     [0] IMPLICIT SET OF Certificate  OPTIONAL,
  //   crls             [1] IMPLICIT SET OF CRL          OPTIONAL,
  //   signerInfos      SET OF SignerInfo
  // }
  std::vector<DERElement> sd_children;
  if (!parse_der_children(signed_data, sd_children) ||
      sd_children.size() < 4) {
    result.error = "SignedData has too few elements";
    return result;
  }

  // sd_children[0] = version (INTEGER)
  // sd_children[1] = digestAlgorithms (SET)
  // sd_children[2] = encapContentInfo (SEQUENCE)
  // Then optional [0] certificates, optional [1] crls
  // Then signerInfos (SET)

  // Extract digest algorithm from digestAlgorithms SET
  if (sd_children[1].is_set()) {
    std::vector<DERElement> digest_algos;
    if (parse_der_children(sd_children[1], digest_algos) &&
        !digest_algos.empty()) {
      result.digest_algorithm = identify_digest_algorithm(digest_algos[0]);
    }
  }

  // Find certificates [0] and signerInfos (the last SET in the sequence)
  const DERElement* certs_elem = nullptr;
  const DERElement* signer_infos_elem = nullptr;

  for (size_t i = 3; i < sd_children.size(); i++) {
    if (sd_children[i].is_context(0)) {
      certs_elem = &sd_children[i];
    } else if (sd_children[i].is_set()) {
      // The last SET we encounter is signerInfos
      signer_infos_elem = &sd_children[i];
    }
  }

  // --- Extract signer's Common Name from the first certificate ---
  if (certs_elem) {
    std::vector<DERElement> certs;
    if (parse_der_children(*certs_elem, certs) && !certs.empty()) {
      // First certificate: SEQUENCE { tbsCertificate, signatureAlgorithm,
      // signatureValue }
      const DERElement& cert = certs[0];
      if (cert.is_sequence()) {
        std::vector<DERElement> cert_children;
        if (parse_der_children(cert, cert_children) && !cert_children.empty()) {
          // tbsCertificate is the first SEQUENCE
          const DERElement& tbs = cert_children[0];
          if (tbs.is_sequence()) {
            std::vector<DERElement> tbs_children;
            if (parse_der_children(tbs, tbs_children)) {
              // TBSCertificate: version [0], serial, signature, issuer, validity,
              // subject, ...
              // Subject is typically at index 5 (after version, serial,
              // sigAlgo, issuer, validity)
              // But version [0] is optional -- if present, subject is at idx 5;
              // if absent, at idx 4.
              size_t subject_idx = 5;
              if (tbs_children.size() > 0 && tbs_children[0].is_context(0)) {
                subject_idx = 5;
              } else {
                subject_idx = 4;
              }
              if (subject_idx < tbs_children.size()) {
                result.signer_name =
                    extract_common_name(tbs_children[subject_idx]);
              }
            }
          }
        }
      }
    }
  }

  // --- Parse SignerInfo to extract authenticated attributes ---
  if (!signer_infos_elem) {
    result.error = "No signerInfos found in SignedData";
    return result;
  }

  std::vector<DERElement> signer_infos;
  if (!parse_der_children(*signer_infos_elem, signer_infos) ||
      signer_infos.empty()) {
    result.error = "signerInfos SET is empty";
    return result;
  }

  // Take the first SignerInfo
  // SignerInfo ::= SEQUENCE {
  //   version                  INTEGER,
  //   sid                      SignerIdentifier,
  //   digestAlgorithm          AlgorithmIdentifier,
  //   authenticatedAttributes  [0] IMPLICIT SET OF Attribute  OPTIONAL,
  //   digestEncryptionAlgorithm AlgorithmIdentifier,
  //   encryptedDigest          OCTET STRING,
  //   unauthenticatedAttributes [1] IMPLICIT SET OF Attribute  OPTIONAL
  // }
  const DERElement& signer_info = signer_infos[0];
  if (!signer_info.is_sequence()) {
    result.error = "SignerInfo is not a SEQUENCE";
    return result;
  }

  std::vector<DERElement> si_children;
  if (!parse_der_children(signer_info, si_children) ||
      si_children.size() < 4) {
    result.error = "SignerInfo has too few elements";
    return result;
  }

  // si_children[0] = version (INTEGER)
  // si_children[1] = sid (SEQUENCE for issuerAndSerialNumber or [0] for
  //                  subjectKeyIdentifier)
  // si_children[2] = digestAlgorithm (SEQUENCE)
  // Then possibly [0] authenticatedAttributes
  // Then digestEncryptionAlgorithm, encryptedDigest, ...

  // Override digest_algorithm from SignerInfo's digestAlgorithm if empty
  if (result.digest_algorithm.empty() || result.digest_algorithm == "Unknown") {
    if (si_children.size() > 2 && si_children[2].is_sequence()) {
      result.digest_algorithm = identify_digest_algorithm(si_children[2]);
    }
  }

  // Find authenticatedAttributes [0]
  const DERElement* auth_attrs = nullptr;
  for (size_t i = 3; i < si_children.size(); i++) {
    if (si_children[i].is_context(0)) {
      auth_attrs = &si_children[i];
      break;
    }
  }

  if (!auth_attrs) {
    // No authenticated attributes -- in this case the message digest is the
    // direct hash of the content.  We can still check integrity but cannot
    // extract signing time.
    //
    // Without authenticated attributes the encryptedDigest directly signs the
    // content hash.  We only verify that the computed digest matches
    // signed_data_digest stored in the field (already computed at parse time).
    if (!field.signed_data_digest.empty() &&
        field.signed_data_digest == computed_digest) {
      result.integrity_valid = true;
      result.signature_valid = true;
      result.success = true;
    } else {
      result.error = "No authenticated attributes and digest mismatch";
    }
    return result;
  }

  // --- Extract message digest from authenticated attributes ---
  std::vector<uint8_t> pkcs7_digest;
  if (!extract_message_digest(*auth_attrs, pkcs7_digest)) {
    result.error = "Failed to find messageDigest attribute in SignerInfo";
    return result;
  }

  // --- Extract signing time ---
  extract_signing_time(*auth_attrs, result.signing_time);

  // --- Step (c): Compare the digests ---
  // The messageDigest attribute should contain the hash of the byte-range data.
  // We need to compare using the appropriate algorithm.
  //
  // The computed_digest above is always SHA-256. If the signature uses a
  // different algorithm, we need to recompute.

  std::vector<uint8_t> final_computed_digest;
  if (result.digest_algorithm == "SHA-256" ||
      result.digest_algorithm.empty()) {
    final_computed_digest = computed_digest;
  } else if (result.digest_algorithm == "SHA-1") {
    // Recompute with SHA-1
    nanopdf::crypto::SHA1 sha1;
    for (size_t i = 0; i < field.byte_range.size(); i += 2) {
      uint64_t offset = field.byte_range[i];
      uint64_t length = field.byte_range[i + 1];
      if (length == 0) continue;
      sha1.update(pdf.data + static_cast<size_t>(offset),
                  static_cast<size_t>(length));
    }
    sha1.finalize();
    final_computed_digest.resize(nanopdf::crypto::SHA1::DIGEST_SIZE);
    sha1.get_digest(final_computed_digest.data());
  } else if (result.digest_algorithm == "MD5") {
    // Recompute with MD5
    nanopdf::crypto::MD5 md5;
    for (size_t i = 0; i < field.byte_range.size(); i += 2) {
      uint64_t offset = field.byte_range[i];
      uint64_t length = field.byte_range[i + 1];
      if (length == 0) continue;
      md5.update(pdf.data + static_cast<size_t>(offset),
                 static_cast<size_t>(length));
    }
    md5.finalize();
    final_computed_digest.resize(nanopdf::crypto::MD5::DIGEST_SIZE);
    md5.get_digest(final_computed_digest.data());
  } else {
    result.error = "Unsupported digest algorithm: " + result.digest_algorithm;
    result.signature_valid = true;  // Structure parsed OK
    return result;
  }

  // Compare
  if (pkcs7_digest.size() == final_computed_digest.size() &&
      std::memcmp(pkcs7_digest.data(), final_computed_digest.data(),
                  pkcs7_digest.size()) == 0) {
    result.integrity_valid = true;
  } else {
    result.error = "Message digest mismatch: ByteRange hash does not match "
                   "messageDigest attribute in PKCS#7";
  }

  // If integrity is valid, mark signature_valid to indicate the PKCS#7
  // structure is well-formed.  We do NOT verify the RSA/ECDSA signature
  // over the authenticated attributes.
  result.signature_valid = result.integrity_valid;
  result.success = result.integrity_valid;

  return result;
}

// ---------------------------------------------------------------------------
// PDF/A Conformance Validation
// ---------------------------------------------------------------------------

namespace {

// The standard 14 PDF fonts that may be unembedded in some profiles.
// PDF/A-1 technically requires all fonts to be embedded, but we still
// identify them so the violation detail is informative.
static const char* kStandard14Fonts[] = {
    "Courier",
    "Courier-Bold",
    "Courier-Oblique",
    "Courier-BoldOblique",
    "Helvetica",
    "Helvetica-Bold",
    "Helvetica-Oblique",
    "Helvetica-BoldOblique",
    "Times-Roman",
    "Times-Bold",
    "Times-Italic",
    "Times-BoldItalic",
    "Symbol",
    "ZapfDingbats",
};

static bool is_standard_14_font(const std::string& name) {
  for (const auto* std_name : kStandard14Fonts) {
    if (name == std_name) return true;
  }
  return false;
}

}  // anonymous namespace

PdfAValidationResult validate_pdfa(const Pdf& pdf) {
  PdfAValidationResult result;

  // 1. Check XMP metadata with PDF/A identification
  const XMPMetadata& xmp = pdf.catalog.xmp_metadata;
  if (!xmp.is_pdfa()) {
    PdfAViolation v;
    v.rule = PdfAViolation::Rule::MissingXMPMetadata;
    v.message = "XMP metadata does not contain PDF/A identification";
    v.detail = "pdfaid:part is missing or zero";
    result.violations.push_back(std::move(v));
    // Without a claimed level we cannot validate further, but we still
    // run the remaining checks so callers get a full report.
  }

  result.claimed_level = xmp.pdfa_level();

  // 2. Check OutputIntent with GTS_PDFA1 subtype (required for PDF/A-1;
  //    PDF/A-2/3 accept GTS_PDFA1 as well).
  {
    bool has_pdfa_output_intent = false;
    for (const auto& oi : pdf.catalog.output_intents) {
      if (oi.subtype == "GTS_PDFA1") {
        has_pdfa_output_intent = true;
        break;
      }
    }
    if (!has_pdfa_output_intent) {
      PdfAViolation v;
      v.rule = PdfAViolation::Rule::MissingOutputIntent;
      v.message = "No OutputIntent with subtype GTS_PDFA1 found";
      v.detail = "output_intents count: " +
                 std::to_string(pdf.catalog.output_intents.size());
      result.violations.push_back(std::move(v));
    }
  }

  // 3. Check that all fonts are embedded
  for (size_t pi = 0; pi < pdf.catalog.pages.size(); ++pi) {
    const Page& page = pdf.catalog.pages[pi];
    for (const auto& kv : page.fonts) {
      const std::string& font_name = kv.first;
      const BaseFont* font = kv.second.get();
      if (!font) continue;

      bool embedded = false;
      if (font->descriptor) {
        // A font is considered embedded when the descriptor carries a
        // non-None font file type (FontFile, FontFile2, or FontFile3).
        if (font->descriptor->font_file_type != FontFileType::None) {
          embedded = true;
        }
      }

      if (!embedded) {
        const std::string& base = font->base_font.empty() ? font_name
                                                           : font->base_font;
        PdfAViolation v;
        v.rule = PdfAViolation::Rule::FontNotEmbedded;
        if (is_standard_14_font(base)) {
          v.message =
              "Standard 14 font is not embedded (required by PDF/A)";
        } else {
          v.message = "Font is not embedded";
        }
        v.detail = "font=" + base + " page=" + std::to_string(pi + 1);
        result.violations.push_back(std::move(v));
      }
    }
  }

  // 4. No encryption allowed
  if (pdf.encrypt != 0) {
    PdfAViolation v;
    v.rule = PdfAViolation::Rule::EncryptionPresent;
    v.message = "PDF/A documents must not be encrypted";
    v.detail = "encrypt object number: " + std::to_string(pdf.encrypt);
    result.violations.push_back(std::move(v));
  }

  // 5. Document info present (title required for conformance level A)
  {
    const DocumentInfo& info = pdf.catalog.document_info;
    if (info.title.empty()) {
      PdfAViolation v;
      v.rule = PdfAViolation::Rule::MissingDocumentInfo;
      v.message = "Document title is empty (required for PDF/A level A)";
      result.violations.push_back(std::move(v));
    }
  }

  // 6. Transparency check for PDF/A-1 (parts 2/3 allow transparency)
  if (xmp.pdfa_part == 1 || xmp.pdfa_part == 0) {
    for (size_t pi = 0; pi < pdf.catalog.pages.size(); ++pi) {
      const Page& page = pdf.catalog.pages[pi];
      const Dictionary& res = page.resources;
      auto ext_it = res.find("ExtGState");
      if (ext_it == res.end()) continue;

      // Resolve the ExtGState dictionary
      Dictionary ext_dict;
      if (ext_it->second.type == Value::DICTIONARY) {
        ext_dict = ext_it->second.dict;
      } else if (ext_it->second.type == Value::REFERENCE) {
        ResolvedObject resolved =
            resolve_reference(pdf, ext_it->second.ref_object_number,
                              ext_it->second.ref_generation_number);
        if (resolved.success && resolved.value.type == Value::DICTIONARY) {
          ext_dict = resolved.value.dict;
        }
      }

      for (const auto& gs_entry : ext_dict) {
        Dictionary gs_dict;
        if (gs_entry.second.type == Value::DICTIONARY) {
          gs_dict = gs_entry.second.dict;
        } else if (gs_entry.second.type == Value::REFERENCE) {
          ResolvedObject resolved =
              resolve_reference(pdf, gs_entry.second.ref_object_number,
                                gs_entry.second.ref_generation_number);
          if (resolved.success &&
              resolved.value.type == Value::DICTIONARY) {
            gs_dict = resolved.value.dict;
          }
        }
        if (gs_dict.empty()) continue;

        // Check for stroking alpha (CA) < 1.0
        auto ca_it = gs_dict.find("CA");
        if (ca_it != gs_dict.end() && ca_it->second.type == Value::NUMBER) {
          if (ca_it->second.number < 1.0) {
            PdfAViolation v;
            v.rule = PdfAViolation::Rule::TransparencyUsed;
            v.message =
                "Transparency (non-stroking alpha CA) used on page";
            v.detail = "page=" + std::to_string(pi + 1) +
                       " CA=" + std::to_string(ca_it->second.number);
            result.violations.push_back(std::move(v));
          }
        }

        // Check for non-stroking alpha (ca) < 1.0
        auto ca_lower_it = gs_dict.find("ca");
        if (ca_lower_it != gs_dict.end() &&
            ca_lower_it->second.type == Value::NUMBER) {
          if (ca_lower_it->second.number < 1.0) {
            PdfAViolation v;
            v.rule = PdfAViolation::Rule::TransparencyUsed;
            v.message =
                "Transparency (stroking alpha ca) used on page";
            v.detail = "page=" + std::to_string(pi + 1) +
                       " ca=" + std::to_string(ca_lower_it->second.number);
            result.violations.push_back(std::move(v));
          }
        }

        // Check for SMask
        auto smask_it = gs_dict.find("SMask");
        if (smask_it != gs_dict.end()) {
          // SMask can be /None (a name) which is acceptable
          bool has_smask = true;
          if (smask_it->second.type == Value::NAME &&
              smask_it->second.name == "None") {
            has_smask = false;
          }
          if (has_smask) {
            PdfAViolation v;
            v.rule = PdfAViolation::Rule::TransparencyUsed;
            v.message = "Soft mask (SMask) used on page";
            v.detail = "page=" + std::to_string(pi + 1);
            result.violations.push_back(std::move(v));
          }
        }
      }
    }
  }

  // Determine overall validity: valid only if no violations and the PDF
  // actually claims to be PDF/A.
  result.valid = result.violations.empty() && xmp.is_pdfa();

  return result;
}

}  // namespace nanopdf
