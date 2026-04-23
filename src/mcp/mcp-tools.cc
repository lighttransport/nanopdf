//
// Copyright 2025, nanopdf authors
// SPDX-License-Identifier: MIT
//

#include "mcp-tools.hh"
#include "text-layout.hh"

#include <algorithm>
#include <cmath>
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

  return it->second(arguments);
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

  const auto& page = g_pdf_state.pdf->catalog.pages[page_num];
  std::vector<TextSearchResult> found =
      search_text_on_page(*g_pdf_state.pdf, page, query, false);

  JsonValue matches = JsonValue::array();
  for (const auto& hit : found) {
    JsonValue match = JsonValue::object();
    match["position"] = JsonValue(static_cast<int>(hit.char_index));
    match["context"] = JsonValue(hit.context);
    match["x"] = JsonValue(hit.x);
    match["y"] = JsonValue(hit.y);
    match["width"] = JsonValue(hit.width);
    match["height"] = JsonValue(hit.height);
    match["score"] = JsonValue(hit.score);
    match["fuzzy"] = JsonValue(hit.fuzzy);
    matches.push_back(match);
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

ToolResult query_region_tool(const JsonValue& args) {
  if (!g_pdf_state.pdf) {
    return ToolResult::error("No PDF loaded. Call load_pdf first.");
  }

  if (!args.has("page")) {
    return ToolResult::error("Missing required parameter: 'page'");
  }
  if (!args.has("x1") || !args.has("y1") || !args.has("x2") || !args.has("y2")) {
    return ToolResult::error("Missing required parameters: 'x1', 'y1', 'x2', 'y2'");
  }

  int page_num = args["page"].get_int();
  if (page_num < 0 || page_num >= static_cast<int>(g_pdf_state.pdf->catalog.pages.size())) {
    return ToolResult::error("Page index out of range: " + std::to_string(page_num));
  }

  double x1 = args["x1"].get_number();
  double y1 = args["y1"].get_number();
  double x2 = args["x2"].get_number();
  double y2 = args["y2"].get_number();

  // Normalize rectangle
  if (x1 > x2) std::swap(x1, x2);
  if (y1 > y2) std::swap(y1, y2);

  const auto& page = g_pdf_state.pdf->catalog.pages[page_num];

  // Extract text layout
  auto text_page = extract_text_layout(*g_pdf_state.pdf, page);
  if (!text_page) {
    return ToolResult::error("Failed to extract text layout");
  }

  // Collect characters in the region
  // Group by line for structured output
  struct CharInfo {
    uint32_t unicode;
    double x, y, w, h;
    double font_size;
    std::string font_name;
    double rotation;
    double matrix[6];
    int line_index;
  };

  std::vector<CharInfo> hits;
  for (const auto& ch : text_page->chars) {
    // Check if character center is within the query rectangle
    double cx = ch.x + ch.width * 0.5;
    double cy = ch.y + ch.height * 0.5;
    if (cx >= x1 && cx <= x2 && cy >= y1 && cy <= y2) {
      CharInfo ci;
      ci.unicode = ch.unicode;
      ci.x = ch.x;
      ci.y = ch.y;
      ci.w = ch.width;
      ci.h = ch.height;
      ci.font_size = ch.font_size;
      ci.font_name = ch.font_name;
      ci.rotation = ch.rotation;
      for (int i = 0; i < 6; i++) ci.matrix[i] = ch.matrix[i];
      ci.line_index = ch.line_index;
      hits.push_back(ci);
    }
  }

  // Build text spans grouped by line_index and font
  // Sort by line_index then x position
  std::sort(hits.begin(), hits.end(), [](const CharInfo& a, const CharInfo& b) {
    if (a.line_index != b.line_index) return a.line_index < b.line_index;
    return a.x < b.x;
  });

  // Group into text spans (contiguous chars with same line + font + rotation)
  JsonValue spans = JsonValue::array();
  size_t i = 0;
  while (i < hits.size()) {
    JsonValue span = JsonValue::object();
    std::string text;
    double span_x = hits[i].x;
    double span_y = hits[i].y;
    double span_x2 = hits[i].x + hits[i].w;
    double span_y2 = hits[i].y + hits[i].h;
    double rotation = hits[i].rotation;
    std::string font_name = hits[i].font_name;
    double font_size = hits[i].font_size;
    int line_idx = hits[i].line_index;

    // Collect contiguous chars in same line with same font/rotation
    while (i < hits.size() &&
           hits[i].line_index == line_idx &&
           hits[i].font_name == font_name &&
           std::abs(hits[i].rotation - rotation) < 0.1) {
      // Append character
      if (hits[i].unicode < 0x80) {
        text += static_cast<char>(hits[i].unicode);
      } else {
        text += unicode_to_utf8(hits[i].unicode);
      }
      // Expand bounding box
      if (hits[i].x < span_x) span_x = hits[i].x;
      if (hits[i].y < span_y) span_y = hits[i].y;
      if (hits[i].x + hits[i].w > span_x2) span_x2 = hits[i].x + hits[i].w;
      if (hits[i].y + hits[i].h > span_y2) span_y2 = hits[i].y + hits[i].h;
      font_size = hits[i].font_size;  // use last (they should be same)
      i++;
    }

    span["text"] = JsonValue(text);
    span["fontName"] = JsonValue(font_name);
    span["fontSize"] = JsonValue(font_size);
    span["rotation"] = JsonValue(rotation);

    // Determine text direction from rotation
    std::string direction;
    double rot = std::fmod(rotation, 360.0);
    if (rot < 0) rot += 360.0;
    if (rot < 5.0 || rot > 355.0) {
      direction = "ltr-horizontal";
    } else if (std::abs(rot - 90.0) < 5.0) {
      direction = "vertical-up";
    } else if (std::abs(rot - 180.0) < 5.0) {
      direction = "rtl-horizontal";
    } else if (std::abs(rot - 270.0) < 5.0) {
      direction = "vertical-down";
    } else {
      direction = "rotated-" + std::to_string(static_cast<int>(rot));
    }
    span["direction"] = JsonValue(direction);

    // Writing mode: check if font is Type0 with vertical metrics
    {
      auto& mutable_page = const_cast<Page&>(page);
      mutable_page.ensure_fonts_loaded(*g_pdf_state.pdf);
      const BaseFont* bf = mutable_page.get_font(font_name, *g_pdf_state.pdf);
      std::string writing_mode = "horizontal";
      if (bf) {
        const Type0Font* t0 = as_type0_font(bf);
        if (t0 && t0->has_vertical_metrics) {
          writing_mode = "vertical";
        }
      }
      span["writingMode"] = JsonValue(writing_mode);
    }

    // Bounding box [x, y, width, height]
    JsonValue bbox = JsonValue::object();
    bbox["x"] = JsonValue(span_x);
    bbox["y"] = JsonValue(span_y);
    bbox["width"] = JsonValue(span_x2 - span_x);
    bbox["height"] = JsonValue(span_y2 - span_y);
    span["bbox"] = bbox;

    if (line_idx >= 0) {
      span["lineIndex"] = JsonValue(line_idx);
    }

    spans.push_back(span);
  }

  // Also list image XObjects on the page (names + dimensions, no placement info)
  auto image_xobjects = parse_xobject_resources(*g_pdf_state.pdf, page.resources);
  JsonValue images = JsonValue::array();
  for (const auto& kv : image_xobjects) {
    JsonValue img = JsonValue::object();
    img["name"] = JsonValue(kv.first);
    img["width"] = JsonValue(kv.second.width);
    img["height"] = JsonValue(kv.second.height);
    if (kv.second.color_space.type == ColorSpaceType::DeviceGray)
      img["colorSpace"] = JsonValue("DeviceGray");
    else if (kv.second.color_space.type == ColorSpaceType::DeviceRGB)
      img["colorSpace"] = JsonValue("DeviceRGB");
    else if (kv.second.color_space.type == ColorSpaceType::DeviceCMYK)
      img["colorSpace"] = JsonValue("DeviceCMYK");
    else if (kv.second.color_space.type == ColorSpaceType::Indexed)
      img["colorSpace"] = JsonValue("Indexed");
    images.push_back(img);
  }

  // Build result
  JsonValue result = JsonValue::object();
  result["page"] = JsonValue(page_num);

  JsonValue query_rect = JsonValue::object();
  query_rect["x1"] = JsonValue(x1);
  query_rect["y1"] = JsonValue(y1);
  query_rect["x2"] = JsonValue(x2);
  query_rect["y2"] = JsonValue(y2);
  result["queryRect"] = query_rect;

  result["pageWidth"] = JsonValue(text_page->page_width);
  result["pageHeight"] = JsonValue(text_page->page_height);
  result["textSpans"] = spans;
  result["textSpanCount"] = JsonValue(static_cast<int>(spans.size()));
  result["charCount"] = JsonValue(static_cast<int>(hits.size()));
  result["pageImages"] = images;

  return ToolResult::ok(result);
}

// ============================================================
// get_page_structure tool
// ============================================================

ToolResult get_page_structure_tool(const JsonValue& args) {
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
  auto text_page = extract_text_layout(*g_pdf_state.pdf, page);
  if (!text_page) {
    return ToolResult::error("Failed to extract text layout");
  }

  JsonValue result = JsonValue::object();
  result["page"] = JsonValue(page_num);
  result["pageWidth"] = JsonValue(text_page->page_width);
  result["pageHeight"] = JsonValue(text_page->page_height);
  result["numColumns"] = JsonValue(text_page->num_columns);

  // Lines
  JsonValue lines_arr = JsonValue::array();
  for (size_t i = 0; i < text_page->lines.size(); i++) {
    const auto& line = text_page->lines[i];
    JsonValue lo = JsonValue::object();
    lo["text"] = JsonValue(line.get_text());
    JsonValue bbox = JsonValue::object();
    bbox["x"] = JsonValue(line.x);
    bbox["y"] = JsonValue(line.y);
    bbox["width"] = JsonValue(line.width);
    bbox["height"] = JsonValue(line.height);
    lo["bbox"] = bbox;
    lo["readingOrder"] = JsonValue(line.reading_order);
    lo["rotation"] = JsonValue(line.rotation);
    lo["isRtl"] = JsonValue(line.is_rtl);
    lo["baseline"] = JsonValue(line.baseline);
    lines_arr.push_back(lo);
  }
  result["lines"] = lines_arr;

  // Words
  JsonValue words_arr = JsonValue::array();
  for (const auto& word : text_page->words) {
    JsonValue wo = JsonValue::object();
    wo["text"] = JsonValue(word.get_text());
    JsonValue bbox = JsonValue::object();
    bbox["x"] = JsonValue(word.x);
    bbox["y"] = JsonValue(word.y);
    bbox["width"] = JsonValue(word.width);
    bbox["height"] = JsonValue(word.height);
    wo["bbox"] = bbox;
    wo["lineIndex"] = JsonValue(word.line_index);
    words_arr.push_back(wo);
  }
  result["words"] = words_arr;

  return ToolResult::ok(result);
}

// ============================================================
// query_annotations tool
// ============================================================

static std::string annotation_type_to_string(AnnotationType t) {
  switch (t) {
    case AnnotationType::Text: return "Text";
    case AnnotationType::Link: return "Link";
    case AnnotationType::FreeText: return "FreeText";
    case AnnotationType::Line: return "Line";
    case AnnotationType::Square: return "Square";
    case AnnotationType::Circle: return "Circle";
    case AnnotationType::Polygon: return "Polygon";
    case AnnotationType::PolyLine: return "PolyLine";
    case AnnotationType::Highlight: return "Highlight";
    case AnnotationType::Underline: return "Underline";
    case AnnotationType::Squiggly: return "Squiggly";
    case AnnotationType::StrikeOut: return "StrikeOut";
    case AnnotationType::Stamp: return "Stamp";
    case AnnotationType::Caret: return "Caret";
    case AnnotationType::Ink: return "Ink";
    case AnnotationType::Popup: return "Popup";
    case AnnotationType::FileAttachment: return "FileAttachment";
    case AnnotationType::Sound: return "Sound";
    case AnnotationType::Movie: return "Movie";
    case AnnotationType::Widget: return "Widget";
    case AnnotationType::Screen: return "Screen";
    case AnnotationType::PrinterMark: return "PrinterMark";
    case AnnotationType::TrapNet: return "TrapNet";
    case AnnotationType::Watermark: return "Watermark";
    case AnnotationType::ThreeD: return "3D";
    case AnnotationType::Redact: return "Redact";
    default: return "Unknown";
  }
}

static std::string action_type_to_string(LinkAnnotation::ActionType t) {
  switch (t) {
    case LinkAnnotation::GoTo: return "GoTo";
    case LinkAnnotation::GoToR: return "GoToR";
    case LinkAnnotation::Launch: return "Launch";
    case LinkAnnotation::URI: return "URI";
    case LinkAnnotation::Named: return "Named";
    case LinkAnnotation::JavaScript: return "JavaScript";
    default: return "Unknown";
  }
}

static std::string field_type_to_string(FieldType t) {
  switch (t) {
    case FieldType::Button: return "Button";
    case FieldType::Text: return "Text";
    case FieldType::Choice: return "Choice";
    case FieldType::Signature: return "Signature";
    default: return "Unknown";
  }
}

ToolResult query_annotations_tool(const JsonValue& args) {
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

  auto& page = g_pdf_state.pdf->catalog.pages[page_num];

  // Parse annotations if not already done
  if (page.annotations.empty() && page.object_number > 0) {
    auto resolved = resolve_reference(*g_pdf_state.pdf, page.object_number, 0);
    if (resolved.success && resolved.value.type == Value::DICTIONARY) {
      parse_page_annotations(*g_pdf_state.pdf, page, resolved.value.dict);
    }
  }

  // Optional region filter
  bool has_region = args.has("x1") && args.has("y1") && args.has("x2") && args.has("y2");
  double rx1 = 0, ry1 = 0, rx2 = 0, ry2 = 0;
  if (has_region) {
    rx1 = args["x1"].get_number();
    ry1 = args["y1"].get_number();
    rx2 = args["x2"].get_number();
    ry2 = args["y2"].get_number();
    if (rx1 > rx2) std::swap(rx1, rx2);
    if (ry1 > ry2) std::swap(ry1, ry2);
  }

  JsonValue annots_arr = JsonValue::array();
  for (const auto& ann : page.annotations) {
    if (!ann) continue;

    // Check region overlap if specified
    if (has_region && ann->rect.size() >= 4) {
      double ax1 = ann->rect[0], ay1 = ann->rect[1];
      double ax2 = ann->rect[2], ay2 = ann->rect[3];
      // No overlap check
      if (ax2 < rx1 || ax1 > rx2 || ay2 < ry1 || ay1 > ry2) continue;
    }

    JsonValue ao = JsonValue::object();
    ao["type"] = JsonValue(annotation_type_to_string(ann->type));

    if (ann->rect.size() >= 4) {
      JsonValue rect = JsonValue::object();
      rect["x1"] = JsonValue(ann->rect[0]);
      rect["y1"] = JsonValue(ann->rect[1]);
      rect["x2"] = JsonValue(ann->rect[2]);
      rect["y2"] = JsonValue(ann->rect[3]);
      ao["rect"] = rect;
    }

    if (!ann->contents.empty()) ao["contents"] = JsonValue(ann->contents);
    if (!ann->name.empty()) ao["name"] = JsonValue(ann->name);

    // Type-specific fields
    if (auto* link = dynamic_cast<LinkAnnotation*>(ann.get())) {
      ao["actionType"] = JsonValue(action_type_to_string(link->action_type));
      if (!link->uri.empty()) ao["uri"] = JsonValue(link->uri);
    } else if (auto* widget = dynamic_cast<WidgetAnnotation*>(ann.get())) {
      ao["fieldType"] = JsonValue(field_type_to_string(widget->field_type));
      if (!widget->field_name.empty()) ao["fieldName"] = JsonValue(widget->field_name);
      if (!widget->field_value.empty()) ao["fieldValue"] = JsonValue(widget->field_value);
    } else if (auto* ft = dynamic_cast<FreeTextAnnotation*>(ann.get())) {
      if (!ft->default_appearance.empty()) ao["defaultAppearance"] = JsonValue(ft->default_appearance);
    }

    annots_arr.push_back(ao);
  }

  JsonValue result = JsonValue::object();
  result["page"] = JsonValue(page_num);
  result["annotationCount"] = JsonValue(static_cast<int>(annots_arr.size()));
  result["annotations"] = annots_arr;
  return ToolResult::ok(result);
}

// ============================================================
// get_image_placements tool
// ============================================================

ToolResult get_image_placements_tool(const JsonValue& args) {
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

  // Parse image XObjects to know dimensions
  auto image_xobjects = parse_xobject_resources(*g_pdf_state.pdf, page.resources);

  // Decode content streams and parse for q/Q/cm/Do
  // CTM stack
  struct Matrix {
    double a, b, c, d, e, f;
    Matrix() : a(1), b(0), c(0), d(1), e(0), f(0) {}
    Matrix(double a_, double b_, double c_, double d_, double e_, double f_)
        : a(a_), b(b_), c(c_), d(d_), e(e_), f(f_) {}
    // Multiply: this = m * this (pre-multiply)
    Matrix concat(const Matrix& m) const {
      return Matrix(
          m.a * a + m.b * c,
          m.a * b + m.b * d,
          m.c * a + m.d * c,
          m.c * b + m.d * d,
          m.e * a + m.f * c + e,
          m.e * b + m.f * d + f);
    }
  };

  Matrix ctm;
  std::vector<Matrix> ctm_stack;
  std::vector<double> num_stack;

  struct ImagePlacement {
    std::string name;
    Matrix ctm;
  };
  std::vector<ImagePlacement> placements;

  // Get content stream data
  auto page_content = page.load_contents(*g_pdf_state.pdf);
  if (!page_content.success) {
    return ToolResult::error("Failed to load page content: " + page_content.error);
  }

  // Simple tokenizer over the content stream
  std::string stream(page_content.data.begin(), page_content.data.end());
  size_t pos = 0;
  size_t len = stream.size();

  auto skip_whitespace = [&]() {
    while (pos < len && (stream[pos] == ' ' || stream[pos] == '\n' ||
                         stream[pos] == '\r' || stream[pos] == '\t'))
      pos++;
  };

  std::function<bool(std::string&)> read_token = [&](std::string& token) -> bool {
    skip_whitespace();
    if (pos >= len) return false;
    token.clear();
    char ch = stream[pos];

    // Skip comments
    if (ch == '%') {
      while (pos < len && stream[pos] != '\n' && stream[pos] != '\r') pos++;
      return read_token(token);
    }

    // Skip strings (...)
    if (ch == '(') {
      int depth = 1;
      pos++;
      while (pos < len && depth > 0) {
        if (stream[pos] == '\\') { pos++; }
        else if (stream[pos] == '(') depth++;
        else if (stream[pos] == ')') depth--;
        pos++;
      }
      token = "(string)";
      return true;
    }

    // Skip hex strings <...>
    if (ch == '<' && pos + 1 < len && stream[pos + 1] != '<') {
      pos++;
      while (pos < len && stream[pos] != '>') pos++;
      if (pos < len) pos++;
      token = "<hex>";
      return true;
    }

    // Skip dict << ... >>
    if (ch == '<' && pos + 1 < len && stream[pos + 1] == '<') {
      pos += 2;
      int depth = 1;
      while (pos + 1 < len && depth > 0) {
        if (stream[pos] == '<' && stream[pos + 1] == '<') { depth++; pos += 2; }
        else if (stream[pos] == '>' && stream[pos + 1] == '>') { depth--; pos += 2; }
        else pos++;
      }
      token = "<<dict>>";
      return true;
    }

    // Skip arrays [...]
    if (ch == '[') {
      pos++;
      int depth = 1;
      while (pos < len && depth > 0) {
        if (stream[pos] == '[') depth++;
        else if (stream[pos] == ']') depth--;
        pos++;
      }
      token = "[array]";
      return true;
    }

    // Name /Xxx
    if (ch == '/') {
      pos++;
      while (pos < len && stream[pos] != ' ' && stream[pos] != '\n' &&
             stream[pos] != '\r' && stream[pos] != '\t' && stream[pos] != '/' &&
             stream[pos] != '[' && stream[pos] != '(' && stream[pos] != '<' &&
             stream[pos] != '{' && stream[pos] != '}') {
        token += stream[pos++];
      }
      token = "/" + token;
      return true;
    }

    // Number or operator
    size_t start = pos;
    while (pos < len && stream[pos] != ' ' && stream[pos] != '\n' &&
           stream[pos] != '\r' && stream[pos] != '\t' && stream[pos] != '/' &&
           stream[pos] != '[' && stream[pos] != '(' && stream[pos] != '<' &&
           stream[pos] != '{' && stream[pos] != '}') {
      pos++;
    }
    token = stream.substr(start, pos - start);
    return !token.empty();
  };

  std::string token;
  std::string last_name;  // last /Name seen (for Do operand)

  while (read_token(token)) {
    // Try parsing as number
    if (!token.empty() && (token[0] == '-' || token[0] == '+' || token[0] == '.' ||
                           (token[0] >= '0' && token[0] <= '9'))) {
      char* end = nullptr;
      double val = strtod(token.c_str(), &end);
      if (end && end != token.c_str()) {
        num_stack.push_back(val);
        continue;
      }
    }

    // Name
    if (!token.empty() && token[0] == '/') {
      last_name = token.substr(1);
      num_stack.clear();
      continue;
    }

    // Operators
    if (token == "q") {
      ctm_stack.push_back(ctm);
      num_stack.clear();
    } else if (token == "Q") {
      if (!ctm_stack.empty()) {
        ctm = ctm_stack.back();
        ctm_stack.pop_back();
      }
      num_stack.clear();
    } else if (token == "cm") {
      if (num_stack.size() >= 6) {
        size_t base = num_stack.size() - 6;
        Matrix m(num_stack[base], num_stack[base + 1], num_stack[base + 2],
                 num_stack[base + 3], num_stack[base + 4], num_stack[base + 5]);
        ctm = m.concat(ctm);
      }
      num_stack.clear();
    } else if (token == "Do") {
      if (!last_name.empty()) {
        ImagePlacement ip;
        ip.name = last_name;
        ip.ctm = ctm;
        placements.push_back(ip);
      }
      num_stack.clear();
    } else {
      num_stack.clear();
    }
  }

  // Build result
  JsonValue result = JsonValue::object();
  result["page"] = JsonValue(page_num);

  JsonValue arr = JsonValue::array();
  for (const auto& ip : placements) {
    // Only include if it's an image XObject
    auto it = image_xobjects.find(ip.name);
    if (it == image_xobjects.end()) continue;

    JsonValue obj = JsonValue::object();
    obj["name"] = JsonValue(ip.name);
    obj["imageWidth"] = JsonValue(it->second.width);
    obj["imageHeight"] = JsonValue(it->second.height);

    JsonValue ctm_arr = JsonValue::array();
    ctm_arr.push_back(JsonValue(ip.ctm.a));
    ctm_arr.push_back(JsonValue(ip.ctm.b));
    ctm_arr.push_back(JsonValue(ip.ctm.c));
    ctm_arr.push_back(JsonValue(ip.ctm.d));
    ctm_arr.push_back(JsonValue(ip.ctm.e));
    ctm_arr.push_back(JsonValue(ip.ctm.f));
    obj["ctm"] = ctm_arr;

    // Derived placement info
    obj["x"] = JsonValue(ip.ctm.e);
    obj["y"] = JsonValue(ip.ctm.f);
    obj["displayWidth"] = JsonValue(std::sqrt(ip.ctm.a * ip.ctm.a + ip.ctm.b * ip.ctm.b));
    obj["displayHeight"] = JsonValue(std::sqrt(ip.ctm.c * ip.ctm.c + ip.ctm.d * ip.ctm.d));

    arr.push_back(obj);
  }

  result["imagePlacements"] = arr;
  result["count"] = JsonValue(static_cast<int>(arr.size()));
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

  // query_region
  {
    std::map<std::string, JsonValue> props;
    props["page"] = make_number_property("Page number (0-indexed)");
    props["x1"] = make_number_property("Left X coordinate in PDF points");
    props["y1"] = make_number_property("Bottom Y coordinate in PDF points");
    props["x2"] = make_number_property("Right X coordinate in PDF points");
    props["y2"] = make_number_property("Top Y coordinate in PDF points");

    Tool tool("query_region",
              "Extract PDF object info (text spans with bbox, font, direction/rotation, "
              "line grouping) for a specified coordinate region. Returns structured data "
              "useful for image recognition post-processing: text direction metadata, "
              "bounding boxes, font info, and page image resources.",
              make_object_schema("Query region parameters", props,
                                 {"page", "x1", "y1", "x2", "y2"}));
    registry.register_tool(tool, query_region_tool);
  }

  // get_page_structure
  {
    std::map<std::string, JsonValue> props;
    props["page"] = make_number_property("Page number (0-indexed)");

    Tool tool("get_page_structure",
              "Get structured text layout (lines and words with bounding boxes, "
              "reading order, rotation, RTL detection) for a page. Lighter than "
              "extract_text_layout which returns per-character data.",
              make_object_schema("Get page structure parameters", props, {"page"}));
    registry.register_tool(tool, get_page_structure_tool);
  }

  // query_annotations
  {
    std::map<std::string, JsonValue> props;
    props["page"] = make_number_property("Page number (0-indexed)");
    props["x1"] = make_number_property("Optional left X coordinate to filter by region");
    props["y1"] = make_number_property("Optional bottom Y coordinate to filter by region");
    props["x2"] = make_number_property("Optional right X coordinate to filter by region");
    props["y2"] = make_number_property("Optional top Y coordinate to filter by region");

    Tool tool("query_annotations",
              "List annotations on a page, optionally filtered by region. Returns "
              "type, rect, contents, and type-specific fields (URI for links, "
              "field info for widgets, etc.).",
              make_object_schema("Query annotations parameters", props, {"page"}));
    registry.register_tool(tool, query_annotations_tool);
  }

  // get_image_placements
  {
    std::map<std::string, JsonValue> props;
    props["page"] = make_number_property("Page number (0-indexed)");

    Tool tool("get_image_placements",
              "Get placement info for all images on a page. Returns image name, "
              "native dimensions, CTM matrix, and derived display position/size.",
              make_object_schema("Get image placements parameters", props, {"page"}));
    registry.register_tool(tool, get_image_placements_tool);
  }
}

}  // namespace mcp
}  // namespace nanopdf
