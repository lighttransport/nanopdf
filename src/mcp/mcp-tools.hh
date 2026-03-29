//
// Copyright 2025, nanopdf authors
// SPDX-License-Identifier: MIT
//

#ifndef NANOPDF_MCP_TOOLS_HH_
#define NANOPDF_MCP_TOOLS_HH_

#include "mcp-json.hh"
#include "nanopdf.hh"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nanopdf {
namespace mcp {

// Tool descriptor
struct Tool {
  std::string name;
  std::string description;
  JsonValue input_schema;  // JSON Schema for parameters

  Tool() = default;
  Tool(const std::string& n, const std::string& desc, const JsonValue& schema)
      : name(n), description(desc), input_schema(schema) {}
};

// Tool execution result
struct ToolResult {
  bool success;
  JsonValue content;  // Array of content items
  bool is_error;

  ToolResult() : success(false), is_error(false) {}

  static ToolResult ok(const JsonValue& result) {
    ToolResult r;
    r.success = true;
    r.is_error = false;

    // Wrap result in content array
    JsonValue content_item = JsonValue::object();
    content_item["type"] = JsonValue("text");
    content_item["text"] = result.is_string() ? result : JsonValue(JsonSerializer::serialize(result));

    r.content = JsonValue::array();
    r.content.push_back(content_item);
    return r;
  }

  static ToolResult error(const std::string& message) {
    ToolResult r;
    r.success = false;
    r.is_error = true;

    JsonValue content_item = JsonValue::object();
    content_item["type"] = JsonValue("text");
    content_item["text"] = JsonValue(message);

    r.content = JsonValue::array();
    r.content.push_back(content_item);
    return r;
  }
};

// Tool function signature
using ToolFunction = std::function<ToolResult(const JsonValue& arguments)>;

// Tool registry
class ToolRegistry {
 public:
  ToolRegistry() = default;
  ~ToolRegistry() = default;

  // Register a tool
  void register_tool(const Tool& tool, ToolFunction func);

  // Get all registered tools
  std::vector<Tool> list_tools() const;

  // Call a tool by name
  ToolResult call_tool(const std::string& name, const JsonValue& arguments);

  // Check if tool exists
  bool has_tool(const std::string& name) const;

 private:
  std::map<std::string, Tool> tools_;
  std::map<std::string, ToolFunction> functions_;
};

// Global tool registry
ToolRegistry& get_tool_registry();

// Register all PDF tools
void register_all_tools();

// Individual tool implementations
ToolResult load_pdf_tool(const JsonValue& args);
ToolResult get_page_count_tool(const JsonValue& args);
ToolResult extract_text_tool(const JsonValue& args);
ToolResult get_page_info_tool(const JsonValue& args);
ToolResult get_metadata_tool(const JsonValue& args);
ToolResult extract_text_layout_tool(const JsonValue& args);
ToolResult find_text_tool(const JsonValue& args);
ToolResult get_fonts_tool(const JsonValue& args);
ToolResult get_images_tool(const JsonValue& args);
ToolResult close_pdf_tool(const JsonValue& args);
ToolResult query_region_tool(const JsonValue& args);
ToolResult get_page_structure_tool(const JsonValue& args);
ToolResult query_annotations_tool(const JsonValue& args);
ToolResult get_image_placements_tool(const JsonValue& args);

// Helper functions
std::vector<uint8_t> base64_decode(const std::string& encoded);
std::string base64_encode(const uint8_t* data, size_t len);
bool read_file(const std::string& filepath, std::vector<uint8_t>* out, std::string* error);

}  // namespace mcp
}  // namespace nanopdf

#endif  // NANOPDF_MCP_TOOLS_HH_
