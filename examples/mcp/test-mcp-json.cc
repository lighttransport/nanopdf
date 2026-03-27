//
// Copyright 2025, nanopdf authors
// SPDX-License-Identifier: MIT
//
// Simple test for MCP JSON parser

#include "../../src/mcp/mcp-json.hh"

#include <cassert>
#include <iostream>

using namespace nanopdf::mcp;

void test_parse_primitives() {
  std::cout << "Testing primitive parsing..." << std::endl;

  auto null_result = JsonParser::parse("null");
  assert(null_result.success);
  assert(null_result.value.is_null());

  auto bool_result = JsonParser::parse("true");
  assert(bool_result.success);
  assert(bool_result.value.is_boolean());
  assert(bool_result.value.get_boolean() == true);

  auto num_result = JsonParser::parse("42");
  assert(num_result.success);
  assert(num_result.value.is_number());
  assert(num_result.value.get_int() == 42);

  auto str_result = JsonParser::parse("\"hello\"");
  assert(str_result.success);
  assert(str_result.value.is_string());
  assert(str_result.value.get_string() == "hello");

  std::cout << "✓ Primitive parsing tests passed" << std::endl;
}

void test_parse_array() {
  std::cout << "Testing array parsing..." << std::endl;

  auto result = JsonParser::parse("[1, 2, 3]");
  assert(result.success);
  assert(result.value.is_array());
  assert(result.value.size() == 3);
  assert(result.value[0].get_int() == 1);
  assert(result.value[1].get_int() == 2);
  assert(result.value[2].get_int() == 3);

  std::cout << "✓ Array parsing tests passed" << std::endl;
}

void test_parse_object() {
  std::cout << "Testing object parsing..." << std::endl;

  auto result = JsonParser::parse("{\"name\": \"test\", \"value\": 42}");
  assert(result.success);
  assert(result.value.is_object());
  assert(result.value.has("name"));
  assert(result.value.has("value"));
  assert(result.value["name"].get_string() == "test");
  assert(result.value["value"].get_int() == 42);

  std::cout << "✓ Object parsing tests passed" << std::endl;
}

void test_serialize() {
  std::cout << "Testing serialization..." << std::endl;

  JsonValue obj = JsonValue::object();
  obj["name"] = JsonValue("test");
  obj["value"] = JsonValue(42);
  obj["flag"] = JsonValue(true);

  std::string json = JsonSerializer::serialize(obj);
  std::cout << "Serialized: " << json << std::endl;

  // Parse it back
  auto result = JsonParser::parse(json);
  assert(result.success);
  assert(result.value["name"].get_string() == "test");
  assert(result.value["value"].get_int() == 42);
  assert(result.value["flag"].get_boolean() == true);

  std::cout << "✓ Serialization tests passed" << std::endl;
}

void test_round_trip() {
  std::cout << "Testing round-trip..." << std::endl;

  const char* json = R"({
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/list",
    "params": {}
  })";

  auto parse_result = JsonParser::parse(json);
  assert(parse_result.success);

  std::string serialized = JsonSerializer::serialize(parse_result.value);
  auto reparse_result = JsonParser::parse(serialized);
  assert(reparse_result.success);

  assert(reparse_result.value["jsonrpc"].get_string() == "2.0");
  assert(reparse_result.value["id"].get_int() == 1);
  assert(reparse_result.value["method"].get_string() == "tools/list");

  std::cout << "✓ Round-trip tests passed" << std::endl;
}

void test_unicode() {
  std::cout << "Testing Unicode..." << std::endl;

  // UTF-8 string
  auto result = JsonParser::parse("\"Hello 世界 🌍\"");
  assert(result.success);
  assert(result.value.is_string());
  std::cout << "UTF-8: " << result.value.get_string() << std::endl;

  // Unicode escape
  auto escape_result = JsonParser::parse("\"\\u0048\\u0065\\u006c\\u006c\\u006f\"");
  assert(escape_result.success);
  assert(escape_result.value.get_string() == "Hello");

  std::cout << "✓ Unicode tests passed" << std::endl;
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
  std::cout << "Running MCP JSON tests..." << std::endl << std::endl;

  try {
    test_parse_primitives();
    test_parse_array();
    test_parse_object();
    test_serialize();
    test_round_trip();
    test_unicode();

    std::cout << std::endl << "All tests passed! ✓" << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed with exception: " << e.what() << std::endl;
    return 1;
  }
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
