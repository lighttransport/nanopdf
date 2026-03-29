//
// Copyright 2025, nanopdf authors
// SPDX-License-Identifier: MIT
//

#ifndef NANOPDF_MCP_JSON_HH_
#define NANOPDF_MCP_JSON_HH_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nanopdf {
namespace mcp {

// JSON value types
enum class JsonType {
  Null,
  Boolean,
  Number,
  String,
  Array,
  Object
};

// JSON value with tagged union pattern
class JsonValue {
 public:
  JsonValue();
  explicit JsonValue(JsonType type);

  // Constructors for specific types
  explicit JsonValue(bool value);
  explicit JsonValue(int value);
  explicit JsonValue(double value);
  explicit JsonValue(const char* value);
  explicit JsonValue(const std::string& value);

  // Copy and move
  JsonValue(const JsonValue& other);
  JsonValue(JsonValue&& other) noexcept;
  JsonValue& operator=(const JsonValue& other);
  JsonValue& operator=(JsonValue&& other) noexcept;

  ~JsonValue() = default;

  // Type queries
  JsonType type() const { return type_; }
  bool is_null() const { return type_ == JsonType::Null; }
  bool is_boolean() const { return type_ == JsonType::Boolean; }
  bool is_number() const { return type_ == JsonType::Number; }
  bool is_string() const { return type_ == JsonType::String; }
  bool is_array() const { return type_ == JsonType::Array; }
  bool is_object() const { return type_ == JsonType::Object; }

  // Value accessors (undefined behavior if type doesn't match)
  bool get_boolean() const { return bool_value_; }
  double get_number() const { return number_value_; }
  int get_int() const { return static_cast<int>(number_value_); }
  const std::string& get_string() const { return string_value_; }
  const std::vector<JsonValue>& get_array() const { return array_value_; }
  const std::map<std::string, JsonValue>& get_object() const { return object_value_; }

  // Mutable accessors
  std::vector<JsonValue>& get_array() { return array_value_; }
  std::map<std::string, JsonValue>& get_object() { return object_value_; }

  // Object helpers
  bool has(const std::string& key) const;
  const JsonValue& operator[](const std::string& key) const;
  JsonValue& operator[](const std::string& key);

  // Array helpers
  const JsonValue& operator[](size_t index) const;
  JsonValue& operator[](size_t index);
  size_t size() const;
  void push_back(const JsonValue& value);
  void push_back(JsonValue&& value);

  // Static factory methods
  static JsonValue null();
  static JsonValue array();
  static JsonValue object();

 private:
  JsonType type_;
  bool bool_value_;
  double number_value_;
  std::string string_value_;
  std::vector<JsonValue> array_value_;
  std::map<std::string, JsonValue> object_value_;
};

// Parse result
struct ParseResult {
  bool success;
  JsonValue value;
  std::string error;
  size_t error_position;

  ParseResult() : success(false), error_position(0) {}
  static ParseResult ok(const JsonValue& val) {
    ParseResult r;
    r.success = true;
    r.value = val;
    return r;
  }
  static ParseResult ok(JsonValue&& val) {
    ParseResult r;
    r.success = true;
    r.value = std::move(val);
    return r;
  }
  static ParseResult fail(const std::string& err, size_t pos = 0) {
    ParseResult r;
    r.success = false;
    r.error = err;
    r.error_position = pos;
    return r;
  }
};

// JSON parser (single-pass recursive descent)
class JsonParser {
 public:
  static ParseResult parse(const std::string& json);

 private:
  JsonParser(const std::string& json);

  ParseResult parse_value();
  ParseResult parse_null();
  ParseResult parse_boolean();
  ParseResult parse_number();
  ParseResult parse_string();
  ParseResult parse_array();
  ParseResult parse_object();

  void skip_whitespace();
  char peek() const;
  char next();
  bool expect(char c);
  bool eof() const;

  const std::string& json_;
  size_t pos_;
};

// JSON serializer
class JsonSerializer {
 public:
  static std::string serialize(const JsonValue& value, bool pretty = false);

 private:
  JsonSerializer(bool pretty);

  void serialize_value(const JsonValue& value, int indent);
  void serialize_string(const std::string& str);

  void indent(int level);
  void newline();

  std::string result_;
  bool pretty_;
};

// Helper functions (from wasm-api.cc)
std::string json_escape(const std::string& s);
std::string unicode_to_utf8(uint32_t codepoint);

}  // namespace mcp
}  // namespace nanopdf

#endif  // NANOPDF_MCP_JSON_HH_
