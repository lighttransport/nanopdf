//
// Copyright 2025, nanopdf authors
// SPDX-License-Identifier: MIT
//

#include "mcp-json.hh"
#include "../string-parse.hh"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>

namespace nanopdf {
namespace mcp {

// ============================================================
// Helper functions
// ============================================================

std::string json_escape(const std::string& s) {
  std::string result;
  result.reserve(s.size() * 2);
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      default:
        if (c >= 0 && c < 32) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          result += buf;
        } else {
          result += c;
        }
        break;
    }
  }
  return result;
}

std::string unicode_to_utf8(uint32_t codepoint) {
  std::string result;
  if (codepoint < 0x80) {
    result += static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    result += static_cast<char>(0xC0 | (codepoint >> 6));
    result += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    result += static_cast<char>(0xE0 | (codepoint >> 12));
    result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    result += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    result += static_cast<char>(0xF0 | (codepoint >> 18));
    result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    result += static_cast<char>(0x80 | (codepoint & 0x3F));
  }
  return result;
}

// ============================================================
// JsonValue implementation
// ============================================================

JsonValue::JsonValue()
    : type_(JsonType::Null), bool_value_(false), number_value_(0.0) {}

JsonValue::JsonValue(JsonType type)
    : type_(type), bool_value_(false), number_value_(0.0) {}

JsonValue::JsonValue(bool value)
    : type_(JsonType::Boolean), bool_value_(value), number_value_(0.0) {}

JsonValue::JsonValue(int value)
    : type_(JsonType::Number), bool_value_(false), number_value_(static_cast<double>(value)) {}

JsonValue::JsonValue(double value)
    : type_(JsonType::Number), bool_value_(false), number_value_(value) {}

JsonValue::JsonValue(const char* value)
    : type_(JsonType::String), bool_value_(false), number_value_(0.0), string_value_(value) {}

JsonValue::JsonValue(const std::string& value)
    : type_(JsonType::String), bool_value_(false), number_value_(0.0), string_value_(value) {}

JsonValue::JsonValue(const JsonValue& other)
    : type_(other.type_),
      bool_value_(other.bool_value_),
      number_value_(other.number_value_),
      string_value_(other.string_value_),
      array_value_(other.array_value_),
      object_value_(other.object_value_) {}

JsonValue::JsonValue(JsonValue&& other) noexcept
    : type_(other.type_),
      bool_value_(other.bool_value_),
      number_value_(other.number_value_),
      string_value_(std::move(other.string_value_)),
      array_value_(std::move(other.array_value_)),
      object_value_(std::move(other.object_value_)) {}

JsonValue& JsonValue::operator=(const JsonValue& other) {
  if (this != &other) {
    type_ = other.type_;
    bool_value_ = other.bool_value_;
    number_value_ = other.number_value_;
    string_value_ = other.string_value_;
    array_value_ = other.array_value_;
    object_value_ = other.object_value_;
  }
  return *this;
}

JsonValue& JsonValue::operator=(JsonValue&& other) noexcept {
  if (this != &other) {
    type_ = other.type_;
    bool_value_ = other.bool_value_;
    number_value_ = other.number_value_;
    string_value_ = std::move(other.string_value_);
    array_value_ = std::move(other.array_value_);
    object_value_ = std::move(other.object_value_);
  }
  return *this;
}

bool JsonValue::has(const std::string& key) const {
  return object_value_.find(key) != object_value_.end();
}

const JsonValue& JsonValue::operator[](const std::string& key) const {
  static JsonValue null_value;
  auto it = object_value_.find(key);
  return it != object_value_.end() ? it->second : null_value;
}

JsonValue& JsonValue::operator[](const std::string& key) {
  return object_value_[key];
}

const JsonValue& JsonValue::operator[](size_t index) const {
  static JsonValue null_value;
  return index < array_value_.size() ? array_value_[index] : null_value;
}

JsonValue& JsonValue::operator[](size_t index) {
  return array_value_[index];
}

size_t JsonValue::size() const {
  if (type_ == JsonType::Array) {
    return array_value_.size();
  } else if (type_ == JsonType::Object) {
    return object_value_.size();
  }
  return 0;
}

void JsonValue::push_back(const JsonValue& value) {
  array_value_.push_back(value);
}

void JsonValue::push_back(JsonValue&& value) {
  array_value_.push_back(std::move(value));
}

JsonValue JsonValue::null() {
  return JsonValue(JsonType::Null);
}

JsonValue JsonValue::array() {
  return JsonValue(JsonType::Array);
}

JsonValue JsonValue::object() {
  return JsonValue(JsonType::Object);
}

// ============================================================
// JsonParser implementation
// ============================================================

ParseResult JsonParser::parse(const std::string& json) {
  JsonParser parser(json);
  return parser.parse_value();
}

JsonParser::JsonParser(const std::string& json) : json_(json), pos_(0) {}

ParseResult JsonParser::parse_value() {
  skip_whitespace();

  if (eof()) {
    return ParseResult::fail("Unexpected end of input", pos_);
  }

  char c = peek();
  switch (c) {
    case 'n': return parse_null();
    case 't':
    case 'f': return parse_boolean();
    case '"': return parse_string();
    case '[': return parse_array();
    case '{': return parse_object();
    case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
      return parse_number();
    default:
      return ParseResult::fail(std::string("Unexpected character: ") + c, pos_);
  }
}

ParseResult JsonParser::parse_null() {
  if (json_.substr(pos_, 4) == "null") {
    pos_ += 4;
    return ParseResult::ok(JsonValue::null());
  }
  return ParseResult::fail("Expected 'null'", pos_);
}

ParseResult JsonParser::parse_boolean() {
  if (json_.substr(pos_, 4) == "true") {
    pos_ += 4;
    return ParseResult::ok(JsonValue(true));
  } else if (json_.substr(pos_, 5) == "false") {
    pos_ += 5;
    return ParseResult::ok(JsonValue(false));
  }
  return ParseResult::fail("Expected 'true' or 'false'", pos_);
}

ParseResult JsonParser::parse_number() {
  size_t start = pos_;

  // Optional minus
  if (peek() == '-') {
    next();
  }

  // Integer part
  if (peek() == '0') {
    next();
  } else if (peek() >= '1' && peek() <= '9') {
    while (!eof() && peek() >= '0' && peek() <= '9') {
      next();
    }
  } else {
    return ParseResult::fail("Invalid number", pos_);
  }

  // Fractional part
  if (!eof() && peek() == '.') {
    next();
    if (eof() || peek() < '0' || peek() > '9') {
      return ParseResult::fail("Invalid number: expected digit after '.'", pos_);
    }
    while (!eof() && peek() >= '0' && peek() <= '9') {
      next();
    }
  }

  // Exponent
  if (!eof() && (peek() == 'e' || peek() == 'E')) {
    next();
    if (!eof() && (peek() == '+' || peek() == '-')) {
      next();
    }
    if (eof() || peek() < '0' || peek() > '9') {
      return ParseResult::fail("Invalid number: expected digit in exponent", pos_);
    }
    while (!eof() && peek() >= '0' && peek() <= '9') {
      next();
    }
  }

  std::string num_str = json_.substr(start, pos_ - start);
  double value = stod_or(num_str);
  return ParseResult::ok(JsonValue(value));
}

ParseResult JsonParser::parse_string() {
  if (!expect('"')) {
    return ParseResult::fail("Expected '\"'", pos_);
  }

  std::string result;
  while (!eof() && peek() != '"') {
    char c = next();
    if (c == '\\') {
      if (eof()) {
        return ParseResult::fail("Unexpected end of string", pos_);
      }
      char escaped = next();
      switch (escaped) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u': {
          // Unicode escape: \uXXXX
          if (pos_ + 4 > json_.size()) {
            return ParseResult::fail("Invalid unicode escape", pos_);
          }
          std::string hex = json_.substr(pos_, 4);
          pos_ += 4;
          uint32_t codepoint = 0;
          if (!parse_hex_uint(hex, &codepoint)) {
            return ParseResult::fail("Invalid unicode escape hex", pos_);
          }
          result += unicode_to_utf8(codepoint);
          break;
        }
        default:
          return ParseResult::fail(std::string("Invalid escape sequence: \\") + escaped, pos_);
      }
    } else {
      result += c;
    }
  }

  if (!expect('"')) {
    return ParseResult::fail("Unterminated string", pos_);
  }

  return ParseResult::ok(JsonValue(result));
}

ParseResult JsonParser::parse_array() {
  if (!expect('[')) {
    return ParseResult::fail("Expected '['", pos_);
  }

  JsonValue arr = JsonValue::array();
  skip_whitespace();

  if (!eof() && peek() == ']') {
    next();
    return ParseResult::ok(std::move(arr));
  }

  while (true) {
    auto elem = parse_value();
    if (!elem.success) {
      return elem;
    }
    arr.push_back(std::move(elem.value));

    skip_whitespace();
    if (eof()) {
      return ParseResult::fail("Unexpected end of array", pos_);
    }

    char c = next();
    if (c == ']') {
      break;
    } else if (c == ',') {
      skip_whitespace();
      continue;
    } else {
      return ParseResult::fail(std::string("Expected ',' or ']', got: ") + c, pos_);
    }
  }

  return ParseResult::ok(std::move(arr));
}

ParseResult JsonParser::parse_object() {
  if (!expect('{')) {
    return ParseResult::fail("Expected '{'", pos_);
  }

  JsonValue obj = JsonValue::object();
  skip_whitespace();

  if (!eof() && peek() == '}') {
    next();
    return ParseResult::ok(std::move(obj));
  }

  while (true) {
    skip_whitespace();
    if (peek() != '"') {
      return ParseResult::fail("Expected string key", pos_);
    }

    auto key_result = parse_string();
    if (!key_result.success) {
      return key_result;
    }
    std::string key = key_result.value.get_string();

    skip_whitespace();
    if (!expect(':')) {
      return ParseResult::fail("Expected ':' after object key", pos_);
    }

    auto value = parse_value();
    if (!value.success) {
      return value;
    }

    obj[key] = std::move(value.value);

    skip_whitespace();
    if (eof()) {
      return ParseResult::fail("Unexpected end of object", pos_);
    }

    char c = next();
    if (c == '}') {
      break;
    } else if (c == ',') {
      continue;
    } else {
      return ParseResult::fail(std::string("Expected ',' or '}', got: ") + c, pos_);
    }
  }

  return ParseResult::ok(std::move(obj));
}

void JsonParser::skip_whitespace() {
  while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
    pos_++;
  }
}

char JsonParser::peek() const {
  return eof() ? '\0' : json_[pos_];
}

char JsonParser::next() {
  return eof() ? '\0' : json_[pos_++];
}

bool JsonParser::expect(char c) {
  if (peek() == c) {
    next();
    return true;
  }
  return false;
}

bool JsonParser::eof() const {
  return pos_ >= json_.size();
}

// ============================================================
// JsonSerializer implementation
// ============================================================

std::string JsonSerializer::serialize(const JsonValue& value, bool pretty) {
  JsonSerializer serializer(pretty);
  serializer.serialize_value(value, 0);
  return serializer.result_;
}

JsonSerializer::JsonSerializer(bool pretty) : pretty_(pretty) {}

void JsonSerializer::serialize_value(const JsonValue& value, int indent_level) {
  switch (value.type()) {
    case JsonType::Null:
      result_ += "null";
      break;

    case JsonType::Boolean:
      result_ += value.get_boolean() ? "true" : "false";
      break;

    case JsonType::Number: {
      double num = value.get_number();
      // Check if integer
      if (std::floor(num) == num && num >= -2147483648.0 && num <= 2147483647.0) {
        result_ += std::to_string(static_cast<int>(num));
      } else {
        // Use ostringstream for proper formatting
        std::ostringstream oss;
        oss << num;
        result_ += oss.str();
      }
      break;
    }

    case JsonType::String:
      serialize_string(value.get_string());
      break;

    case JsonType::Array: {
      const auto& arr = value.get_array();
      result_ += '[';
      if (!arr.empty()) {
        if (pretty_) newline();
        for (size_t i = 0; i < arr.size(); i++) {
          if (pretty_) indent(indent_level + 1);
          serialize_value(arr[i], indent_level + 1);
          if (i < arr.size() - 1) {
            result_ += ',';
          }
          if (pretty_) newline();
        }
        if (pretty_) indent(indent_level);
      }
      result_ += ']';
      break;
    }

    case JsonType::Object: {
      const auto& obj = value.get_object();
      result_ += '{';
      if (!obj.empty()) {
        if (pretty_) newline();
        size_t i = 0;
        for (const auto& kv : obj) {
          if (pretty_) indent(indent_level + 1);
          serialize_string(kv.first);
          result_ += pretty_ ? ": " : ":";
          serialize_value(kv.second, indent_level + 1);
          if (i < obj.size() - 1) {
            result_ += ',';
          }
          if (pretty_) newline();
          i++;
        }
        if (pretty_) indent(indent_level);
      }
      result_ += '}';
      break;
    }
  }
}

void JsonSerializer::serialize_string(const std::string& str) {
  result_ += '"';
  result_ += json_escape(str);
  result_ += '"';
}

void JsonSerializer::indent(int level) {
  for (int i = 0; i < level * 2; i++) {
    result_ += ' ';
  }
}

void JsonSerializer::newline() {
  result_ += '\n';
}

}  // namespace mcp
}  // namespace nanopdf
