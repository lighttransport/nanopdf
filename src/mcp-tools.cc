//
// Copyright 2025, nanopdf authors
// SPDX-License-Identifier: MIT
//

#include "mcp-tools.hh"
#include "text-layout.hh"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace nanopdf {
namespace mcp {

// ============================================================
// Global PDF state
// ============================================================

struct PdfState {
  std::unique_ptr<nanopdf::Pdf> pdf;
  std::vector<uint8_t> pdf_data;
  std::string last_error;
  std::string filename;

  void clear() {
    pdf.reset();
    pdf_data.clear();
    last_error.clear();
    filename.clear();
  }
};

static PdfState g_pdf_state;

// ============================================================
// Helper functions
// ============================================================

static const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
  std::string result;
  result.reserve((len + 2) / 3 * 4);

  for (size_t i = 0; i < len; i += 3) {
    uint32_t value = data[i] << 16;
    if (i + 1 < len) value |= data[i + 1] << 8;
    if (i + 2 < len) value |= data[i + 2];

    result += kBase64Chars[(value >> 18) & 0x3F];
    result += kBase64Chars[(value >> 12) & 0x3F];
    result += (i + 1 < len) ? kBase64Chars[(value >> 6) & 0x3F] : '=';
    result += (i + 2 < len) ? kBase64Chars[value & 0x3F] : '=';
  }

  return result;
}

std::vector<uint8_t> base64_decode(const std::string& encoded) {
  std::vector<uint8_t> result;
  result.reserve((encoded.size() / 4) * 3);

  auto decode_char = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };

  for (size_t i = 0; i < encoded.size(); i += 4) {
    int a = decode_char(encoded[i]);
    int b = (i + 1 < encoded.size()) ? decode_char(encoded[i + 1]) : 0;
    int c = (i + 2 < encoded.size()) ? decode_char(encoded[i + 2]) : 0;
    int d = (i + 3 < encoded.size()) ? decode_char(encoded[i + 3]) : 0;

    if (a < 0 || b < 0) break;

    result.push_back((a << 2) | (b >> 4));
    if (c >= 0 && encoded[i + 2] != '=') {
      result.push_back((b << 4) | (c >> 2));
    }
    if (d >= 0 && encoded[i + 3] != '=') {
      result.push_back((c << 6) | d);
    }
  }

  return result;
}

bool read_file(const std::string& filepath, std::vector<uint8_t>* out, std::string* error) {
  std::ifstream f(filepath.c_str(), std::ifstream::binary);
  if (!f) {
    if (error) {
      *error = "File open error: " + filepath;
    }
    return false;
  }

  f.seekg(0, f.end);
  size_t sz = static_cast<size_t>(f.tellg());
  f.seekg(0, f.beg);

  if (sz == 0) {
    if (error) {
      *error = "File is empty: " + filepath;
    }
    return false;
  }

  out->resize(sz);
  f.read(reinterpret_cast<char*>(&out->at(0)), static_cast<std::streamsize>(sz));

  if (!f) {
    if (error) {
      *error = "Failed to read file: " + filepath;
    }
    return false;
  }

  return true;
}

// ============================================================
// ToolRegistry implementation
// ============================================================

void ToolRegistry::register_tool(const Tool& tool, ToolFunction func) {
  tools_[tool.name] = tool;
  functions_[tool.name] = func;
}

std::vector<Tool> ToolRegistry::list_tools() const {
  std::vector<Tool> result;
  result.reserve(tools_.size());
  for (const auto& kv : tools_) {
    result.push_back(kv.second);
  }
  return result;
}

ToolResult ToolRegistry::call_tool(const std::string& name, const JsonValue& arguments) {
  auto it = functions_.find(name);
  if (it == functions_.end()) {
    return ToolResult::error("Tool not found: " + name);
  }

  try {
    return it->second(arguments);
  } catch (const std::exception& e) {
    return ToolResult::error(std::string("Tool execution error: ") + e.what());
  }
}

bool ToolRegistry::has_tool(const std::string& name) const {
  return tools_.find(name) != tools_.end();
}

ToolRegistry& get_tool_registry() {
  static ToolRegistry registry;
  return registry;
}

// ============================================================
// JSON Schema helpers
// ============================================================

static JsonValue make_string_property(const std::string& description) {
  JsonValue prop = JsonValue::object();
  prop["type"] = JsonValue("string");
  prop["description"] = JsonValue(description);
  return prop;
}

static JsonValue make_number_property(const std::string& description) {
  JsonValue prop = JsonValue::object();
  prop["type"] = JsonValue("number");
  prop["description"] = JsonValue(description);
  return prop;
}

static JsonValue make_object_schema(const std::string& description,
                                     const std::map<std::string, JsonValue>& properties,
                                     const std::vector<std::string>& required = {}) {
  JsonValue schema = JsonValue::object();
  schema["type"] = JsonValue("object");
  schema["description"] = JsonValue(description);

  JsonValue props = JsonValue::object();
  for (const auto& kv : properties) {
    props[kv.first] = kv.second;
  }
  schema["properties"] = props;

  if (!required.empty()) {
    JsonValue req = JsonValue::array();
    for (const auto& r : required) {
      req.push_back(JsonValue(r));
    }
    schema["required"] = req;
  }

  return schema;
}

// ============================================================
// Tool implementations
// ============================================================

ToolResult load_pdf_tool(const JsonValue& args) {
  if (!args.has("path") && !args.has("data")) {
    return ToolResult::error("Missing required parameter: 'path' or 'data'");
  }

  std::vector<uint8_t> data;
  std::string filename;

  if (args.has("path")) {
    filename = args["path"].get_string();
    std::string error;
    if (!read_file(filename, &data, &error)) {
      return ToolResult::error("Failed to read file: " + error);
    }
  } else {
    // Base64 data
    std::string base64_data = args["data"].get_string();
    data = base64_decode(base64_data);
    filename = "<base64-data>";
  }

  // Parse PDF
  g_pdf_state.clear();
  g_pdf_state.pdf.reset(new nanopdf::Pdf());
  g_pdf_state.filename = filename;

  if (!nanopdf::parse_from_memory(data.data(), data.size(), g_pdf_state.pdf.get())) {
    g_pdf_state.clear();
    return ToolResult::error("Failed to parse PDF");
  }

  // Load document structure
  if (!g_pdf_state.pdf->load_document_structure()) {
    g_pdf_state.clear();
    return ToolResult::error("Failed to load document structure");
  }

  g_pdf_state.pdf_data = std::move(data);

  // Build result
  JsonValue result = JsonValue::object();
  result["success"] = JsonValue(true);
  result["pageCount"] = JsonValue(static_cast<int>(g_pdf_state.pdf->catalog.pages.size()));
  result["filename"] = JsonValue(filename);

  // Add metadata if available
  g_pdf_state.pdf->ensure_metadata_loaded();
  const auto& doc_info = g_pdf_state.pdf->catalog.document_info;
  if (!doc_info.title.empty() || !doc_info.author.empty()) {
    JsonValue metadata = JsonValue::object();
    if (!doc_info.title.empty()) metadata["Title"] = JsonValue(doc_info.title);
    if (!doc_info.author.empty()) metadata["Author"] = JsonValue(doc_info.author);
    if (!doc_info.subject.empty()) metadata["Subject"] = JsonValue(doc_info.subject);
    if (!doc_info.keywords.empty()) metadata["Keywords"] = JsonValue(doc_info.keywords);
    if (!doc_info.creator.empty()) metadata["Creator"] = JsonValue(doc_info.creator);
    if (!doc_info.producer.empty()) metadata["Producer"] = JsonValue(doc_info.producer);
    result["metadata"] = metadata;
  }

  return ToolResult::ok(result);
}

ToolResult get_page_count_tool(const JsonValue& args) {
  if (!g_pdf_state.pdf) {
    return ToolResult::error("No PDF loaded. Call load_pdf first.");
  }

  JsonValue result = JsonValue::object();
  result["count"] = JsonValue(static_cast<int>(g_pdf_state.pdf->catalog.pages.size()));
  return ToolResult::ok(result);
}

ToolResult extract_text_tool(const JsonValue& args) {
  if (!g_pdf_state.pdf) {
    return ToolResult::error("No PDF loaded. Call load_pdf first.");
  }

  int start_page = 0;
  int end_page = static_cast<int>(g_pdf_state.pdf->catalog.pages.size()) - 1;

  if (args.has("page")) {
    start_page = end_page = args["page"].get_int();
  } else {
    if (args.has("start_page")) {
      start_page = args["start_page"].get_int();
    }
    if (args.has("end_page")) {
      end_page = args["end_page"].get_int();
    }
  }

  // Validate range
  int max_page = static_cast<int>(g_pdf_state.pdf->catalog.pages.size()) - 1;
  if (start_page < 0 || start_page > max_page || end_page < 0 || end_page > max_page || start_page > end_page) {
    return ToolResult::error("Invalid page range: 0-" + std::to_string(max_page) + " expected, got " +
                             std::to_string(start_page) + "-" + std::to_string(end_page));
  }

  // Extract text
  std::string text;
  for (int i = start_page; i <= end_page; i++) {
    const auto& page = g_pdf_state.pdf->catalog.pages[i];

    std::string page_text = extract_text_from_page(*g_pdf_state.pdf, page);

    if (i > start_page) {
      text += "\n\n--- Page " + std::to_string(i) + " ---\n\n";
    }
    text += page_text;
  }

  JsonValue result = JsonValue::object();
  result["text"] = JsonValue(text);
  result["start_page"] = JsonValue(start_page);
  result["end_page"] = JsonValue(end_page);
  return ToolResult::ok(result);
}

ToolResult get_page_info_tool(const JsonValue& args) {
  if (!g_pdf_state.pdf) {
    return ToolResult::error("No PDF loaded. Call load_pdf first.");
  }

  if (!args.has("page")) {
    return ToolResult::error("Missing required parameter: 'page'");
  }

  int page_num = args["page"].get_int();
  if (page_num < 0 || page_num >= static_cast<int>(g_pdf_state.pdf->catalog.pages.size())) {
    return ToolResult::error("Page index out of range: " + std::to_string(page_num));
  }

  const auto& page = g_pdf_state.pdf->catalog.pages[page_num];

  // Calculate width and height from media_box [left, bottom, right, top]
  double width = 0.0;
  double height = 0.0;
  if (page.media_box.size() >= 4) {
    width = page.media_box[2] - page.media_box[0];
    height = page.media_box[3] - page.media_box[1];
  }

  JsonValue result = JsonValue::object();
  result["page"] = JsonValue(page_num);
  result["width"] = JsonValue(width);
  result["height"] = JsonValue(height);
  result["rotation"] = JsonValue(static_cast<int>(page.rotate));

  return ToolResult::ok(result);
}

ToolResult get_metadata_tool(const JsonValue& args) {
  if (!g_pdf_state.pdf) {
    return ToolResult::error("No PDF loaded. Call load_pdf first.");
  }

  JsonValue result = JsonValue::object();

  // Load metadata
  g_pdf_state.pdf->ensure_metadata_loaded();
  const auto& doc_info = g_pdf_state.pdf->catalog.document_info;

  // Add standard fields
  if (!doc_info.title.empty()) result["Title"] = JsonValue(doc_info.title);
  if (!doc_info.author.empty()) result["Author"] = JsonValue(doc_info.author);
  if (!doc_info.subject.empty()) result["Subject"] = JsonValue(doc_info.subject);
  if (!doc_info.keywords.empty()) result["Keywords"] = JsonValue(doc_info.keywords);
  if (!doc_info.creator.empty()) result["Creator"] = JsonValue(doc_info.creator);
  if (!doc_info.producer.empty()) result["Producer"] = JsonValue(doc_info.producer);
  if (!doc_info.creation_date.empty()) result["CreationDate"] = JsonValue(doc_info.creation_date);
  if (!doc_info.mod_date.empty()) result["ModDate"] = JsonValue(doc_info.mod_date);
  if (!doc_info.trapped.empty()) result["Trapped"] = JsonValue(doc_info.trapped);

  // Add custom fields
  for (const auto& kv : doc_info.custom) {
    result[kv.first] = JsonValue(kv.second);
  }

  return ToolResult::ok(result);
}

ToolResult extract_text_layout_tool(const JsonValue& args) {
  if (!g_pdf_state.pdf) {
    return ToolResult::error("No PDF loaded. Call load_pdf first.");
  }

  if (!args.has("page")) {
    return ToolResult::error("Missing required parameter: 'page'");
  }

  int page_num = args["page"].get_int();
  if (page_num < 0 || page_num >= static_cast<int>(g_pdf_state.pdf->catalog.pages.size())) {
    return ToolResult::error("Page index out of range: " + std::to_string(page_num));
  }

  const auto& page = g_pdf_state.pdf->catalog.pages[page_num];

  // Extract text layout
  auto text_page = extract_text_layout(*g_pdf_state.pdf, page);
  if (!text_page) {
    return ToolResult::error("Failed to extract text layout");
  }

  JsonValue result = JsonValue::object();
  result["page"] = JsonValue(page_num);
  result["pageWidth"] = JsonValue(text_page->page_width);
  result["pageHeight"] = JsonValue(text_page->page_height);

  JsonValue chars_array = JsonValue::array();
  for (const auto& ch : text_page->chars) {
    JsonValue char_obj = JsonValue::object();
    // Convert unicode codepoint to UTF-8 string
    std::string char_str;
    if (ch.unicode < 0x80) {
      char_str += static_cast<char>(ch.unicode);
    } else {
      char_str = unicode_to_utf8(ch.unicode);
    }
    char_obj["c"] = JsonValue(char_str);
    char_obj["x"] = JsonValue(ch.x);
    char_obj["y"] = JsonValue(ch.y);
    char_obj["w"] = JsonValue(ch.width);
    char_obj["h"] = JsonValue(ch.height);
    char_obj["fontSize"] = JsonValue(ch.font_size);
    char_obj["fontName"] = JsonValue(ch.font_name);
    chars_array.push_back(char_obj);
  }
  result["chars"] = chars_array;

  return ToolResult::ok(result);
}

ToolResult find_text_tool(const JsonValue& args) {
  if (!g_pdf_state.pdf) {
    return ToolResult::error("No PDF loaded. Call load_pdf first.");
  }

  if (!args.has("page")) {
    return ToolResult::error("Missing required parameter: 'page'");
  }

  if (!args.has("query")) {
    return ToolResult::error("Missing required parameter: 'query'");
  }

  int page_num = args["page"].get_int();
  std::string query = args["query"].get_string();

  if (page_num < 0 || page_num >= static_cast<int>(g_pdf_state.pdf->catalog.pages.size())) {
    return ToolResult::error("Page index out of range: " + std::to_string(page_num));
  }

  // Extract text
  const auto& page = g_pdf_state.pdf->catalog.pages[page_num];
  std::string text = extract_text_from_page(*g_pdf_state.pdf, page);

  // Find all occurrences
  JsonValue matches = JsonValue::array();
  size_t pos = 0;
  while ((pos = text.find(query, pos)) != std::string::npos) {
    JsonValue match = JsonValue::object();
    match["position"] = JsonValue(static_cast<int>(pos));

    // Extract context (20 chars before and after)
    size_t context_start = (pos >= 20) ? pos - 20 : 0;
    size_t context_end = std::min(pos + query.length() + 20, text.length());
    std::string context = text.substr(context_start, context_end - context_start);
    match["context"] = JsonValue(context);

    matches.push_back(match);
    pos += query.length();
  }

  JsonValue result = JsonValue::object();
  result["page"] = JsonValue(page_num);
  result["query"] = JsonValue(query);
  result["matchCount"] = JsonValue(static_cast<int>(matches.size()));
  result["matches"] = matches;

  return ToolResult::ok(result);
}

ToolResult get_fonts_tool(const JsonValue& args) {
  if (!g_pdf_state.pdf) {
    return ToolResult::error("No PDF loaded. Call load_pdf first.");
  }

  JsonValue fonts = JsonValue::array();

  if (args.has("page")) {
    // Fonts for specific page
    int page_num = args["page"].get_int();
    if (page_num < 0 || page_num >= static_cast<int>(g_pdf_state.pdf->catalog.pages.size())) {
      return ToolResult::error("Page index out of range: " + std::to_string(page_num));
    }

    auto& page = g_pdf_state.pdf->catalog.pages[page_num];
    page.ensure_fonts_loaded(*g_pdf_state.pdf);

    for (const auto& kv : page.fonts) {
      if (!kv.second) continue;  // Skip null fonts
      JsonValue font = JsonValue::object();
      font["name"] = JsonValue(kv.first);
      font["subtype"] = JsonValue(kv.second->subtype);
      if (!kv.second->base_font.empty()) {
        font["baseFont"] = JsonValue(kv.second->base_font);
      }
      // Check if font has embedded data
      bool embedded = (kv.second->descriptor && kv.second->descriptor->font_file_type != FontFileType::None);
      font["embedded"] = JsonValue(embedded);
      fonts.push_back(font);
    }
  } else {
    // All fonts in document
    std::map<std::string, bool> seen_fonts;
    for (size_t i = 0; i < g_pdf_state.pdf->catalog.pages.size(); i++) {
      auto& page = g_pdf_state.pdf->catalog.pages[i];
      page.ensure_fonts_loaded(*g_pdf_state.pdf);

      for (const auto& kv : page.fonts) {
        if (!kv.second) continue;  // Skip null fonts
        if (seen_fonts.find(kv.first) == seen_fonts.end()) {
          seen_fonts[kv.first] = true;
          JsonValue font = JsonValue::object();
          font["name"] = JsonValue(kv.first);
          font["subtype"] = JsonValue(kv.second->subtype);
          if (!kv.second->base_font.empty()) {
            font["baseFont"] = JsonValue(kv.second->base_font);
          }
          // Check if font has embedded data
          bool embedded = (kv.second->descriptor && kv.second->descriptor->font_file_type != FontFileType::None);
          font["embedded"] = JsonValue(embedded);
          fonts.push_back(font);
        }
      }
    }
  }

  JsonValue result = JsonValue::object();
  result["fonts"] = fonts;
  return ToolResult::ok(result);
}

ToolResult get_images_tool(const JsonValue& args) {
  if (!g_pdf_state.pdf) {
    return ToolResult::error("No PDF loaded. Call load_pdf first.");
  }

  if (!args.has("page")) {
    return ToolResult::error("Missing required parameter: 'page'");
  }

  int page_num = args["page"].get_int();
  if (page_num < 0 || page_num >= static_cast<int>(g_pdf_state.pdf->catalog.pages.size())) {
    return ToolResult::error("Page index out of range: " + std::to_string(page_num));
  }

  const auto& page = g_pdf_state.pdf->catalog.pages[page_num];

  // Parse image XObjects from page resources
  auto image_xobjects = parse_xobject_resources(*g_pdf_state.pdf, page.resources);

  JsonValue images = JsonValue::array();
  for (const auto& kv : image_xobjects) {
    JsonValue img = JsonValue::object();
    img["name"] = JsonValue(kv.first);
    img["width"] = JsonValue(kv.second.width);
    img["height"] = JsonValue(kv.second.height);

    // Color space name
    if (kv.second.color_space.type == ColorSpaceType::DeviceGray) {
      img["colorSpace"] = JsonValue("DeviceGray");
    } else if (kv.second.color_space.type == ColorSpaceType::DeviceRGB) {
      img["colorSpace"] = JsonValue("DeviceRGB");
    } else if (kv.second.color_space.type == ColorSpaceType::DeviceCMYK) {
      img["colorSpace"] = JsonValue("DeviceCMYK");
    } else if (kv.second.color_space.type == ColorSpaceType::Indexed) {
      img["colorSpace"] = JsonValue("Indexed");
    } else {
      img["colorSpace"] = JsonValue("Unknown");
    }

    images.push_back(img);
  }

  JsonValue result = JsonValue::object();
  result["page"] = JsonValue(page_num);
  result["images"] = images;
  return ToolResult::ok(result);
}

ToolResult close_pdf_tool(const JsonValue& args) {
  g_pdf_state.clear();

  JsonValue result = JsonValue::object();
  result["success"] = JsonValue(true);
  return ToolResult::ok(result);
}

// ============================================================
// Register all tools
// ============================================================

void register_all_tools() {
  auto& registry = get_tool_registry();

  // load_pdf
  {
    std::map<std::string, JsonValue> props;
    props["path"] = make_string_property("Path to PDF file (mutually exclusive with 'data')");
    props["data"] = make_string_property("Base64-encoded PDF data (mutually exclusive with 'path')");

    Tool tool("load_pdf",
              "Load a PDF document from file path or base64 data",
              make_object_schema("Load PDF parameters", props));
    registry.register_tool(tool, load_pdf_tool);
  }

  // get_page_count
  {
    Tool tool("get_page_count",
              "Get the total number of pages in the loaded PDF",
              make_object_schema("Get page count parameters", {}));
    registry.register_tool(tool, get_page_count_tool);
  }

  // extract_text
  {
    std::map<std::string, JsonValue> props;
    props["page"] = make_number_property("Single page to extract (0-indexed)");
    props["start_page"] = make_number_property("Start page for range extraction (0-indexed)");
    props["end_page"] = make_number_property("End page for range extraction (0-indexed)");

    Tool tool("extract_text",
              "Extract text content from page(s)",
              make_object_schema("Extract text parameters", props));
    registry.register_tool(tool, extract_text_tool);
  }

  // get_page_info
  {
    std::map<std::string, JsonValue> props;
    props["page"] = make_number_property("Page number (0-indexed)");

    Tool tool("get_page_info",
              "Get page dimensions and metadata",
              make_object_schema("Get page info parameters", props, {"page"}));
    registry.register_tool(tool, get_page_info_tool);
  }

  // get_metadata
  {
    Tool tool("get_metadata",
              "Get PDF document metadata (title, author, etc.)",
              make_object_schema("Get metadata parameters", {}));
    registry.register_tool(tool, get_metadata_tool);
  }

  // extract_text_layout
  {
    std::map<std::string, JsonValue> props;
    props["page"] = make_number_property("Page number (0-indexed)");

    Tool tool("extract_text_layout",
              "Extract text with position and layout information",
              make_object_schema("Extract text layout parameters", props, {"page"}));
    registry.register_tool(tool, extract_text_layout_tool);
  }

  // find_text
  {
    std::map<std::string, JsonValue> props;
    props["page"] = make_number_property("Page number to search (0-indexed)");
    props["query"] = make_string_property("Text to search for");

    Tool tool("find_text",
              "Search for text in a specific page",
              make_object_schema("Find text parameters", props, {"page", "query"}));
    registry.register_tool(tool, find_text_tool);
  }

  // get_fonts
  {
    std::map<std::string, JsonValue> props;
    props["page"] = make_number_property("Page number (0-indexed). If omitted, returns all fonts in document");

    Tool tool("get_fonts",
              "List fonts used in the document or a specific page",
              make_object_schema("Get fonts parameters", props));
    registry.register_tool(tool, get_fonts_tool);
  }

  // get_images
  {
    std::map<std::string, JsonValue> props;
    props["page"] = make_number_property("Page number (0-indexed)");

    Tool tool("get_images",
              "List images in a specific page",
              make_object_schema("Get images parameters", props, {"page"}));
    registry.register_tool(tool, get_images_tool);
  }

  // close_pdf
  {
    Tool tool("close_pdf",
              "Close the currently loaded PDF and free resources",
              make_object_schema("Close PDF parameters", {}));
    registry.register_tool(tool, close_pdf_tool);
  }
}

}  // namespace mcp
}  // namespace nanopdf
