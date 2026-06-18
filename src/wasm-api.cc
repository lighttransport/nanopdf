// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// WebAssembly API for nanopdf reading and writing
// This file provides C-compatible functions that can be called from JavaScript

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <cmath>

#include "nanopdf.hh"
#include "pdf-writer.hh"
#include "render-backend.hh"
#include "string-parse.hh"
#include "text-layout.hh"
#include "table-extraction.hh"

#ifdef NANOPDF_USE_BLEND2D
#include "blend2d-backend.hh"
#endif

#ifdef NANOPDF_USE_THORVG
#include "thorvg-backend.hh"
#endif

#ifdef NANOPDF_EMBED_FONTS
#include "embedded-fonts.hh"
#endif

#ifdef NANOPDF_EMBED_CJK_FONTS
#include "embedded-cjk-fonts.hh"
#endif

#include "font-provider.hh"

// ============================================================
// Global state for PDF reading
// ============================================================
static nanopdf::Pdf* g_pdf = nullptr;
static std::vector<uint8_t> g_pdf_data;
static std::string g_last_error;

// Rendering buffer
static std::vector<uint8_t> g_render_buffer;
static uint32_t g_render_width = 0;
static uint32_t g_render_height = 0;
static nanopdf::BackendKind g_render_backend =
#ifdef NANOPDF_USE_THORVG
    nanopdf::BackendKind::ThorVG;
#else
    nanopdf::BackendKind::LightVG;
#endif
;

// Text buffer
static std::string g_text_buffer;

// Export settings
static int g_export_render_scale = 2;  // Default 2x for good quality
static bool g_export_encryption_enabled = false;
static nanopdf::EncryptionConfig g_export_encryption_config;

// Font buffer (for embedded fonts)
static std::vector<uint8_t> g_font_buffer;

// ============================================================
// Global state for PDF writing / editing
// ============================================================
static nanopdf::PdfWriter* g_writer = nullptr;

// Document structure for editing
struct PageInfo {
  int source_doc_id;      // -1 for new pages, 0+ for loaded PDFs
  int source_page_index;  // Original page index in source
  double width, height;
  int rotation;           // 0, 90, 180, 270
  double crop_x, crop_y, crop_w, crop_h;  // Crop box (0,0,w,h = no crop)
  std::vector<uint8_t> thumbnail;  // Thumbnail RGBA data
  uint32_t thumb_width, thumb_height;
};

struct Annotation {
  int type;  // 0=text, 1=line, 2=rect, 3=oval, 4=highlight
  double x1, y1, x2, y2;
  double r, g, b, a;
  double line_width;
  std::string text;
  std::string font_name;
  double font_size;
  bool filled;
};

struct DocumentInfo {
  std::vector<uint8_t> data;
  nanopdf::Pdf* pdf;
  std::vector<PageInfo> pages;
};

static std::map<int, DocumentInfo> g_documents;  // Loaded source documents
static int g_next_doc_id = 0;

// Working document pages (for concatenation/editing)
static std::vector<PageInfo> g_working_pages;
static std::map<int, std::vector<Annotation>> g_page_annotations;  // Page index -> annotations

extern "C" {

// ============================================================
// Initialization
// ============================================================

EMSCRIPTEN_KEEPALIVE
int nanopdf_init() {
#ifdef NANOPDF_USE_THORVG
  tvg::Initializer::init(0);
#endif
  return 1;
}

EMSCRIPTEN_KEEPALIVE
void nanopdf_shutdown() {
  if (g_pdf) {
    delete g_pdf;
    g_pdf = nullptr;
  }
  g_pdf_data.clear();
  g_render_buffer.clear();

  // Clean up writer
  if (g_writer) {
    delete g_writer;
    g_writer = nullptr;
  }

  // Clean up loaded documents
  for (auto& kv : g_documents) {
    if (kv.second.pdf) {
      delete kv.second.pdf;
    }
  }
  g_documents.clear();
  g_working_pages.clear();
  g_page_annotations.clear();
  g_next_doc_id = 0;

#ifdef NANOPDF_USE_THORVG
  tvg::Initializer::term();
#endif
}

// ============================================================
// PDF Loading (for viewing)
// ============================================================

EMSCRIPTEN_KEEPALIVE
int nanopdf_load_pdf(const uint8_t* data, size_t size) {
  if (!data || size == 0) {
    g_last_error = "Invalid input data";
    return 0;
  }

  if (g_pdf) {
    delete g_pdf;
    g_pdf = nullptr;
  }

  g_pdf_data.assign(data, data + size);

  g_pdf = new nanopdf::Pdf();
  if (!nanopdf::parse_from_memory(g_pdf_data.data(), g_pdf_data.size(), g_pdf)) {
    g_last_error = "Failed to parse PDF";
    delete g_pdf;
    g_pdf = nullptr;
    return 0;
  }

  if (!g_pdf->load_document_structure()) {
    g_last_error = "Failed to parse document catalog";
    delete g_pdf;
    g_pdf = nullptr;
    return 0;
  }

  return static_cast<int>(g_pdf->catalog.pages.size());
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_get_page_count() {
  if (!g_pdf) return 0;
  return static_cast<int>(g_pdf->catalog.pages.size());
}

EMSCRIPTEN_KEEPALIVE
float nanopdf_get_page_width(int page_index) {
  if (!g_pdf || page_index < 0 ||
      page_index >= static_cast<int>(g_pdf->catalog.pages.size())) {
    return 0.0f;
  }

  const auto& page = g_pdf->catalog.pages[page_index];
  if (page.media_box.size() >= 4) {
    return static_cast<float>(page.media_box[2] - page.media_box[0]);
  }
  return 612.0f;
}

EMSCRIPTEN_KEEPALIVE
float nanopdf_get_page_height(int page_index) {
  if (!g_pdf || page_index < 0 ||
      page_index >= static_cast<int>(g_pdf->catalog.pages.size())) {
    return 0.0f;
  }

  const auto& page = g_pdf->catalog.pages[page_index];
  if (page.media_box.size() >= 4) {
    return static_cast<float>(page.media_box[3] - page.media_box[1]);
  }
  return 792.0f;
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_has_rendering() {
#if defined(NANOPDF_USE_LIGHTVG) || defined(NANOPDF_USE_THORVG) || defined(NANOPDF_USE_BLEND2D)
  return 1;
#else
  return 0;
#endif
}

static bool render_page_with_selected_backend(const nanopdf::Pdf& pdf,
                                              const nanopdf::Page& page,
                                              int width, int height,
                                              float dpi) {
  auto backend = nanopdf::make_backend(g_render_backend);
  if (!backend) {
    g_last_error = std::string("Rendering backend not available: ") +
                   nanopdf::backend_kind_name(g_render_backend);
    return false;
  }

  if (!backend->initialize(static_cast<uint32_t>(width),
                           static_cast<uint32_t>(height))) {
    g_last_error = std::string("Failed to initialize ") +
                   nanopdf::backend_kind_name(g_render_backend) + " backend";
    return false;
  }

  nanopdf::RenderOptions options;
  options.dpi = dpi > 0 ? dpi : 72.0f;

  auto result = backend->render_page(pdf, page, options);
  if (!result.success) {
    g_last_error = result.error;
    return false;
  }

  g_render_buffer = std::move(result.pixels);
  g_render_width = result.width;
  g_render_height = result.height;
  return true;
}

static bool backend_kind_from_wasm_id(int backend_id,
                                      nanopdf::BackendKind* out) {
  if (!out) return false;
  switch (backend_id) {
    case 0:
      *out = nanopdf::BackendKind::LightVG;
      return true;
    case 1:
      *out = nanopdf::BackendKind::ThorVG;
      return true;
    default:
      return false;
  }
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_render_backend_available(int backend_id) {
  nanopdf::BackendKind kind;
  if (!backend_kind_from_wasm_id(backend_id, &kind)) return 0;
  return nanopdf::backend_available(kind) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_get_render_backend() {
  switch (g_render_backend) {
    case nanopdf::BackendKind::LightVG: return 0;
    case nanopdf::BackendKind::ThorVG: return 1;
  }
  return -1;
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_set_render_backend(int backend_id) {
  nanopdf::BackendKind kind;
  if (!backend_kind_from_wasm_id(backend_id, &kind)) {
    g_last_error = "Unknown rendering backend";
    return 0;
  }
  if (!nanopdf::backend_available(kind)) {
    g_last_error = std::string("Rendering backend not available: ") +
                   nanopdf::backend_kind_name(kind);
    return 0;
  }
  g_render_backend = kind;
  return 1;
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_render_page(int page_index, int width, int height, float dpi) {
  if (!g_pdf || page_index < 0 ||
      page_index >= static_cast<int>(g_pdf->catalog.pages.size())) {
    g_last_error = "Invalid page index or no PDF loaded";
    return 0;
  }

  if (width <= 0 || height <= 0) {
    g_last_error = "Invalid dimensions";
    return 0;
  }

  const auto& page = g_pdf->catalog.pages[page_index];
  return render_page_with_selected_backend(*g_pdf, page, width, height, dpi) ? 1 : 0;
}

// Render a page as the document existed at a given incremental-update
// revision. `byte_len` is that revision's end offset (see
// nanopdf_get_revision_history -> revisions[].endOffset); the document is
// re-parsed from just the first `byte_len` bytes, which reconstructs the file
// state before any later incremental update. Output lands in the same render
// buffer as nanopdf_render_page. Used by the viewer's per-revision visual diff.
EMSCRIPTEN_KEEPALIVE
int nanopdf_render_revision_page(unsigned int byte_len, int page_index,
                                 int width, int height, float dpi) {
  if (!g_pdf || g_pdf_data.empty()) {
    g_last_error = "No PDF loaded";
    return 0;
  }
  if (byte_len == 0 || byte_len > g_pdf_data.size()) {
    g_last_error = "Invalid revision byte length";
    return 0;
  }
  if (width <= 0 || height <= 0) {
    g_last_error = "Invalid dimensions";
    return 0;
  }

  // Re-parse the truncated byte range as a standalone document snapshot.
  nanopdf::Pdf rev_pdf;
  if (!nanopdf::parse_from_memory(g_pdf_data.data(), byte_len, &rev_pdf) ||
      !rev_pdf.load_document_structure()) {
    g_last_error = "Failed to parse revision snapshot";
    return 0;
  }
  if (page_index < 0 ||
      page_index >= static_cast<int>(rev_pdf.catalog.pages.size())) {
    g_last_error = "Page not present in this revision";
    return 0;
  }

  const auto& page = rev_pdf.catalog.pages[page_index];
  return render_page_with_selected_backend(rev_pdf, page, width, height, dpi)
             ? 1
             : 0;
}

// Page count for a given revision snapshot (pages can be added across
// revisions, so the diff UI needs the per-revision count).
EMSCRIPTEN_KEEPALIVE
int nanopdf_get_revision_page_count(unsigned int byte_len) {
  if (!g_pdf || g_pdf_data.empty()) return 0;
  if (byte_len == 0 || byte_len > g_pdf_data.size()) return 0;
  nanopdf::Pdf rev_pdf;
  if (!nanopdf::parse_from_memory(g_pdf_data.data(), byte_len, &rev_pdf) ||
      !rev_pdf.load_document_structure()) {
    return 0;
  }
  return static_cast<int>(rev_pdf.catalog.pages.size());
}

EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_get_render_buffer() {
  if (g_render_buffer.empty()) return nullptr;
  return g_render_buffer.data();
}

EMSCRIPTEN_KEEPALIVE
size_t nanopdf_get_render_buffer_size() {
  return g_render_buffer.size();
}

EMSCRIPTEN_KEEPALIVE
uint32_t nanopdf_get_render_width() {
  return g_render_width;
}

EMSCRIPTEN_KEEPALIVE
uint32_t nanopdf_get_render_height() {
  return g_render_height;
}

EMSCRIPTEN_KEEPALIVE
const char* nanopdf_extract_text(int page_index) {
  if (!g_pdf || page_index < 0 ||
      page_index >= static_cast<int>(g_pdf->catalog.pages.size())) {
    g_text_buffer = "";
    return g_text_buffer.c_str();
  }

  const auto& page = g_pdf->catalog.pages[page_index];
  g_text_buffer = nanopdf::extract_text_from_page(*g_pdf, page);
  return g_text_buffer.c_str();
}

// Helper to escape JSON strings
static std::string json_escape(const std::string& s) {
  std::string result;
  result.reserve(s.size() * 2);
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
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

// Helper to convert unicode codepoint to UTF-8
static std::string unicode_to_utf8(uint32_t codepoint) {
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

static std::string quad_to_json(const nanopdf::TextQuad& q) {
  std::string json = "{";
  json += "\"x\":" + std::to_string(q.x) + ",";
  json += "\"y\":" + std::to_string(q.y) + ",";
  json += "\"width\":" + std::to_string(q.width) + ",";
  json += "\"height\":" + std::to_string(q.height) + ",";
  json += "\"points\":[";
  json += "{\"x\":" + std::to_string(q.x1) + ",\"y\":" + std::to_string(q.y1) + "},";
  json += "{\"x\":" + std::to_string(q.x2) + ",\"y\":" + std::to_string(q.y2) + "},";
  json += "{\"x\":" + std::to_string(q.x3) + ",\"y\":" + std::to_string(q.y3) + "},";
  json += "{\"x\":" + std::to_string(q.x4) + ",\"y\":" + std::to_string(q.y4) + "}";
  json += "]}";
  return json;
}

static std::string search_result_to_json(const nanopdf::TextSearchResult& match) {
  std::string json = "{";
  json += "\"page\":" + std::to_string(match.page_number);
  json += ",\"start\":" + std::to_string(match.char_index);
  json += ",\"end\":" + std::to_string(match.char_index + match.length);
  json += ",\"x\":" + std::to_string(match.x);
  json += ",\"y\":" + std::to_string(match.y);
  json += ",\"width\":" + std::to_string(match.width);
  json += ",\"height\":" + std::to_string(match.height);
  json += ",\"score\":" + std::to_string(match.score);
  json += ",\"fuzzy\":" + std::string(match.fuzzy ? "true" : "false");
  json += ",\"writingMode\":\"" + std::string(nanopdf::text_writing_mode_name(match.writing_mode)) + "\"";
  json += ",\"context\":\"" + json_escape(match.context) + "\"";
  json += ",\"quads\":[";
  for (size_t i = 0; i < match.quads.size(); ++i) {
    if (i > 0) json += ",";
    json += quad_to_json(match.quads[i]);
  }
  json += "]}";
  return json;
}

static std::string selection_to_json(const nanopdf::TextSelectionResult& selection) {
  std::string json = "{";
  json += "\"page\":" + std::to_string(selection.page_number);
  json += ",\"start\":" + std::to_string(selection.start);
  json += ",\"end\":" + std::to_string(selection.start + selection.length);
  json += ",\"length\":" + std::to_string(selection.length);
  json += ",\"text\":\"" + json_escape(selection.text) + "\"";
  json += ",\"bbox\":" + quad_to_json(selection.bounds);
  json += ",\"segments\":[";
  for (size_t i = 0; i < selection.segments.size(); ++i) {
    const auto& segment = selection.segments[i];
    if (i > 0) json += ",";
    json += "{";
    json += "\"start\":" + std::to_string(segment.start);
    json += ",\"end\":" + std::to_string(segment.start + segment.length);
    json += ",\"length\":" + std::to_string(segment.length);
    json += ",\"text\":\"" + json_escape(segment.text) + "\"";
    json += ",\"lineIndex\":" + std::to_string(segment.line_index);
    json += ",\"writingMode\":\"" + std::string(nanopdf::text_writing_mode_name(segment.writing_mode)) + "\"";
    json += ",\"quad\":" + quad_to_json(segment.quad);
    json += "}";
  }
  json += "]}";
  return json;
}

// Extract text with position info as JSON
// Returns: {"pageWidth":W,"pageHeight":H,"chars":[{"c":"X","x":0,"y":0,"w":10,"h":12,"fs":12,"fn":"Arial"},...],
//           "lines":[{"text":"...","x":0,"y":0,"w":100,"h":12,"baseline":10},...],
//           "words":[{"text":"...","x":0,"y":0,"w":50,"h":12},...]}
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_extract_text_layout(int page_index) {
  if (!g_pdf || page_index < 0 ||
      page_index >= static_cast<int>(g_pdf->catalog.pages.size())) {
    g_text_buffer = "{\"error\":\"Invalid page index\"}";
    return g_text_buffer.c_str();
  }

  const auto& page = g_pdf->catalog.pages[page_index];
  auto text_page = nanopdf::extract_text_layout(*g_pdf, page);

  if (!text_page) {
    g_text_buffer = "{\"error\":\"Failed to extract text layout\"}";
    return g_text_buffer.c_str();
  }

  // Build JSON response
  std::string json = "{";

  // Page dimensions
  json += "\"pageWidth\":" + std::to_string(text_page->page_width) + ",";
  json += "\"pageHeight\":" + std::to_string(text_page->page_height) + ",";
  json += "\"numColumns\":" + std::to_string(text_page->num_columns) + ",";

  // Characters with positions
  json += "\"chars\":[";
  for (size_t i = 0; i < text_page->chars.size(); ++i) {
    const auto& ch = text_page->chars[i];
    if (i > 0) json += ",";
    json += "{";
    json += "\"c\":\"" + json_escape(unicode_to_utf8(ch.unicode)) + "\",";
    json += "\"u\":" + std::to_string(ch.unicode) + ",";
    json += "\"x\":" + std::to_string(ch.x) + ",";
    json += "\"y\":" + std::to_string(ch.y) + ",";
    json += "\"w\":" + std::to_string(ch.width) + ",";
    json += "\"h\":" + std::to_string(ch.height) + ",";
    json += "\"fs\":" + std::to_string(ch.font_size) + ",";
    json += "\"fn\":\"" + json_escape(ch.font_name) + "\",";
    json += "\"rot\":" + std::to_string(ch.rotation) + ",";
    json += "\"writingMode\":\"" + std::string(nanopdf::text_writing_mode_name(ch.writing_mode)) + "\",";
    json += "\"quad\":" + quad_to_json(ch.quad) + ",";
    json += "\"charIndex\":" + std::to_string(ch.char_index) + ",";
    json += "\"li\":" + std::to_string(ch.line_index) + ",";
    json += "\"wi\":" + std::to_string(ch.word_index);
    json += "}";
  }
  json += "],";

  // Lines with bounding boxes
  json += "\"lines\":[";
  for (size_t i = 0; i < text_page->lines.size(); ++i) {
    const auto& line = text_page->lines[i];
    if (i > 0) json += ",";
    json += "{";
    json += "\"text\":\"" + json_escape(line.get_text()) + "\",";
    json += "\"x\":" + std::to_string(line.x) + ",";
    json += "\"y\":" + std::to_string(line.y) + ",";
    json += "\"w\":" + std::to_string(line.width) + ",";
    json += "\"h\":" + std::to_string(line.height) + ",";
    json += "\"baseline\":" + std::to_string(line.baseline) + ",";
    json += "\"rotation\":" + std::to_string(line.rotation) + ",";
    json += "\"writingMode\":\"" + std::string(nanopdf::text_writing_mode_name(line.writing_mode)) + "\",";
    json += "\"readingOrder\":" + std::to_string(line.reading_order) + ",";
    json += "\"isRtl\":" + std::string(line.is_rtl ? "true" : "false");
    json += "}";
  }
  json += "],";

  // Words with bounding boxes
  json += "\"words\":[";
  for (size_t i = 0; i < text_page->words.size(); ++i) {
    const auto& word = text_page->words[i];
    if (i > 0) json += ",";
    json += "{";
    json += "\"text\":\"" + json_escape(word.get_text()) + "\",";
    json += "\"x\":" + std::to_string(word.x) + ",";
    json += "\"y\":" + std::to_string(word.y) + ",";
    json += "\"w\":" + std::to_string(word.width) + ",";
    json += "\"h\":" + std::to_string(word.height) + ",";
    json += "\"writingMode\":\"" + std::string(nanopdf::text_writing_mode_name(word.writing_mode)) + "\",";
    json += "\"lineIndex\":" + std::to_string(word.line_index);
    json += "}";
  }
  json += "]";

  json += "}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Get text in a rectangular region
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_get_text_in_rect(int page_index, double x1, double y1, double x2, double y2) {
  if (!g_pdf || page_index < 0 ||
      page_index >= static_cast<int>(g_pdf->catalog.pages.size())) {
    g_text_buffer = "";
    return g_text_buffer.c_str();
  }

  const auto& page = g_pdf->catalog.pages[page_index];
  auto text_page = nanopdf::extract_text_layout(*g_pdf, page);

  if (!text_page) {
    g_text_buffer = "";
    return g_text_buffer.c_str();
  }

  g_text_buffer = text_page->get_text_in_rect(x1, y1, x2, y2);
  return g_text_buffer.c_str();
}

// Find text occurrences, returns JSON array of character indices
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_find_text(int page_index, const char* search_term) {
  if (!g_pdf || page_index < 0 || !search_term ||
      page_index >= static_cast<int>(g_pdf->catalog.pages.size())) {
    g_text_buffer = "[]";
    return g_text_buffer.c_str();
  }

  const auto& page = g_pdf->catalog.pages[page_index];
  auto results = nanopdf::search_text_on_page(*g_pdf, page, search_term, false);

  std::string json = "[";
  for (size_t i = 0; i < results.size(); ++i) {
    if (i > 0) json += ",";
    json += std::to_string(results[i].char_index);
  }
  json += "]";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Search text on selected pages. fuzzy=0 returns exact matches only.
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_search_text(const char* search_term, const char* page_indices,
                                int case_sensitive, int fuzzy, int max_results) {
  if (!g_pdf) {
    g_text_buffer = "{\"error\":\"No PDF loaded\"}";
    return g_text_buffer.c_str();
  }
  if (!search_term || strlen(search_term) == 0) {
    g_text_buffer = "{\"error\":\"Empty search term\"}";
    return g_text_buffer.c_str();
  }

  std::vector<int> pages;
  std::string indices_str(page_indices ? page_indices : "all");
  if (indices_str == "all") {
    for (size_t i = 0; i < g_pdf->catalog.pages.size(); ++i) {
      pages.push_back(static_cast<int>(i));
    }
  } else {
    size_t pos = 0;
    while ((pos = indices_str.find(',')) != std::string::npos) {
      int idx = nanopdf::stoi_or(indices_str.substr(0, pos).c_str());
      if (idx >= 0 && idx < static_cast<int>(g_pdf->catalog.pages.size())) pages.push_back(idx);
      indices_str.erase(0, pos + 1);
    }
    if (!indices_str.empty()) {
      int idx = nanopdf::stoi_or(indices_str.c_str());
      if (idx >= 0 && idx < static_cast<int>(g_pdf->catalog.pages.size())) pages.push_back(idx);
    }
  }

  std::string search(search_term);
  std::string json = "{\"searchTerm\":\"" + json_escape(search) + "\",\"results\":[";
  bool first = true;
  int total = 0;
  for (int page_idx : pages) {
    const auto& page = g_pdf->catalog.pages[page_idx];
    auto matches = nanopdf::search_text_on_page(*g_pdf, page, search, case_sensitive != 0);
    for (auto& match : matches) {
      if (!fuzzy && match.fuzzy) continue;
      if (!first) json += ",";
      first = false;
      match.page_number = static_cast<uint32_t>(page_idx);
      json += search_result_to_json(match);
      total++;
      if (max_results >= 0 && total >= max_results) break;
    }
    if (max_results >= 0 && total >= max_results) break;
  }
  json += "],\"totalMatches\":" + std::to_string(total) + "}";
  g_text_buffer = json;
  return g_text_buffer.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* nanopdf_select_text_range(int page_index, int start, int length) {
  if (!g_pdf || page_index < 0 ||
      page_index >= static_cast<int>(g_pdf->catalog.pages.size()) ||
      start < 0 || length < 0) {
    g_text_buffer = "{\"error\":\"Invalid range selection\"}";
    return g_text_buffer.c_str();
  }

  const auto& page = g_pdf->catalog.pages[page_index];
  auto text_page = nanopdf::extract_text_layout(*g_pdf, page);
  if (!text_page) {
    g_text_buffer = "{\"error\":\"Failed to extract text layout\"}";
    return g_text_buffer.c_str();
  }

  auto selection = text_page->select_text_range(static_cast<size_t>(start),
                                                static_cast<size_t>(length));
  selection.page_number = static_cast<uint32_t>(page_index);
  g_text_buffer = selection_to_json(selection);
  return g_text_buffer.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* nanopdf_select_text_rect(int page_index, double x1, double y1,
                                    double x2, double y2) {
  if (!g_pdf || page_index < 0 ||
      page_index >= static_cast<int>(g_pdf->catalog.pages.size())) {
    g_text_buffer = "{\"error\":\"Invalid rectangle selection\"}";
    return g_text_buffer.c_str();
  }

  const auto& page = g_pdf->catalog.pages[page_index];
  auto text_page = nanopdf::extract_text_layout(*g_pdf, page);
  if (!text_page) {
    g_text_buffer = "{\"error\":\"Failed to extract text layout\"}";
    return g_text_buffer.c_str();
  }

  auto selection = text_page->select_text_in_rect(x1, y1, x2, y2);
  selection.page_number = static_cast<uint32_t>(page_index);
  g_text_buffer = selection_to_json(selection);
  return g_text_buffer.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* nanopdf_get_last_error() {
  return g_last_error.c_str();
}

// ============================================================
// Document Management (for editing/concatenation)
// ============================================================

// Load a PDF document for editing, returns document ID
EMSCRIPTEN_KEEPALIVE
int nanopdf_doc_load(const uint8_t* data, size_t size) {
  if (!data || size == 0) {
    g_last_error = "Invalid input data";
    return -1;
  }

  DocumentInfo doc;
  doc.data.assign(data, data + size);
  doc.pdf = new nanopdf::Pdf();

  if (!nanopdf::parse_from_memory(doc.data.data(), doc.data.size(), doc.pdf)) {
    g_last_error = "Failed to parse PDF";
    delete doc.pdf;
    return -1;
  }

  if (!doc.pdf->load_document_structure()) {
    g_last_error = "Failed to parse document catalog";
    delete doc.pdf;
    return -1;
  }

  // Store page info
  for (size_t i = 0; i < doc.pdf->catalog.pages.size(); i++) {
    const auto& page = doc.pdf->catalog.pages[i];
    PageInfo pinfo;
    pinfo.source_doc_id = g_next_doc_id;
    pinfo.source_page_index = static_cast<int>(i);

    if (page.media_box.size() >= 4) {
      pinfo.width = page.media_box[2] - page.media_box[0];
      pinfo.height = page.media_box[3] - page.media_box[1];
    } else {
      pinfo.width = 612.0;
      pinfo.height = 792.0;
    }

    pinfo.rotation = 0;
    pinfo.crop_x = 0;
    pinfo.crop_y = 0;
    pinfo.crop_w = pinfo.width;
    pinfo.crop_h = pinfo.height;

    doc.pages.push_back(pinfo);
  }

  int doc_id = g_next_doc_id++;
  g_documents[doc_id] = std::move(doc);

  return doc_id;
}

// Get page count in a loaded document
EMSCRIPTEN_KEEPALIVE
int nanopdf_doc_get_page_count(int doc_id) {
  auto it = g_documents.find(doc_id);
  if (it == g_documents.end()) return 0;
  return static_cast<int>(it->second.pages.size());
}

// Get page width from a loaded document
EMSCRIPTEN_KEEPALIVE
float nanopdf_doc_get_page_width(int doc_id, int page_index) {
  auto it = g_documents.find(doc_id);
  if (it == g_documents.end()) return 0.0f;
  if (page_index < 0 || page_index >= static_cast<int>(it->second.pages.size())) return 0.0f;
  return static_cast<float>(it->second.pages[page_index].width);
}

// Get page height from a loaded document
EMSCRIPTEN_KEEPALIVE
float nanopdf_doc_get_page_height(int doc_id, int page_index) {
  auto it = g_documents.find(doc_id);
  if (it == g_documents.end()) return 0.0f;
  if (page_index < 0 || page_index >= static_cast<int>(it->second.pages.size())) return 0.0f;
  return static_cast<float>(it->second.pages[page_index].height);
}

// Close a loaded document
EMSCRIPTEN_KEEPALIVE
void nanopdf_doc_close(int doc_id) {
  auto it = g_documents.find(doc_id);
  if (it != g_documents.end()) {
    if (it->second.pdf) {
      delete it->second.pdf;
    }
    g_documents.erase(it);
  }
}

// Render a page from loaded document
EMSCRIPTEN_KEEPALIVE
int nanopdf_doc_render_page(int doc_id, int page_index, int width, int height, float dpi) {
  auto it = g_documents.find(doc_id);
  if (it == g_documents.end()) {
    g_last_error = "Document not found";
    return 0;
  }

  if (!it->second.pdf || page_index < 0 ||
      page_index >= static_cast<int>(it->second.pdf->catalog.pages.size())) {
    g_last_error = "Invalid page index";
    return 0;
  }

  if (width <= 0 || height <= 0) {
    g_last_error = "Invalid dimensions";
    return 0;
  }

  const auto& page = it->second.pdf->catalog.pages[page_index];
  return render_page_with_selected_backend(*it->second.pdf, page, width, height, dpi) ? 1 : 0;
}

// ============================================================
// Working Document Operations
// ============================================================

// Clear working document
EMSCRIPTEN_KEEPALIVE
void nanopdf_work_clear() {
  g_working_pages.clear();
  g_page_annotations.clear();
}

// Get working page count
EMSCRIPTEN_KEEPALIVE
int nanopdf_work_get_page_count() {
  return static_cast<int>(g_working_pages.size());
}

// Add page from loaded document to working document
EMSCRIPTEN_KEEPALIVE
int nanopdf_work_add_page(int doc_id, int page_index) {
  auto it = g_documents.find(doc_id);
  if (it == g_documents.end()) {
    g_last_error = "Document not found";
    return -1;
  }

  if (page_index < 0 || page_index >= static_cast<int>(it->second.pages.size())) {
    g_last_error = "Invalid page index";
    return -1;
  }

  PageInfo pinfo = it->second.pages[page_index];
  g_working_pages.push_back(pinfo);

  return static_cast<int>(g_working_pages.size() - 1);
}

// Add all pages from a document
EMSCRIPTEN_KEEPALIVE
int nanopdf_work_add_all_pages(int doc_id) {
  auto it = g_documents.find(doc_id);
  if (it == g_documents.end()) {
    g_last_error = "Document not found";
    return -1;
  }

  int first_index = static_cast<int>(g_working_pages.size());
  for (const auto& page : it->second.pages) {
    g_working_pages.push_back(page);
  }

  return first_index;
}

// Remove page from working document
EMSCRIPTEN_KEEPALIVE
int nanopdf_work_remove_page(int page_index) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    g_last_error = "Invalid page index";
    return 0;
  }

  g_working_pages.erase(g_working_pages.begin() + page_index);
  g_page_annotations.erase(page_index);

  // Shift annotation indices
  std::map<int, std::vector<Annotation>> new_annotations;
  for (auto& kv : g_page_annotations) {
    if (kv.first > page_index) {
      new_annotations[kv.first - 1] = std::move(kv.second);
    } else {
      new_annotations[kv.first] = std::move(kv.second);
    }
  }
  g_page_annotations = std::move(new_annotations);

  return 1;
}

// Move page in working document
EMSCRIPTEN_KEEPALIVE
int nanopdf_work_move_page(int from_index, int to_index) {
  if (from_index < 0 || from_index >= static_cast<int>(g_working_pages.size()) ||
      to_index < 0 || to_index >= static_cast<int>(g_working_pages.size())) {
    g_last_error = "Invalid page index";
    return 0;
  }

  if (from_index == to_index) return 1;

  PageInfo page = g_working_pages[from_index];
  g_working_pages.erase(g_working_pages.begin() + from_index);
  g_working_pages.insert(g_working_pages.begin() + to_index, page);

  // Also move annotations
  auto annot_it = g_page_annotations.find(from_index);
  std::vector<Annotation> annots;
  if (annot_it != g_page_annotations.end()) {
    annots = std::move(annot_it->second);
    g_page_annotations.erase(annot_it);
  }

  // Rebuild annotation map with shifted indices
  std::map<int, std::vector<Annotation>> new_annotations;
  for (auto& kv : g_page_annotations) {
    int new_idx = kv.first;
    if (from_index < to_index) {
      if (kv.first > from_index && kv.first <= to_index) {
        new_idx = kv.first - 1;
      }
    } else {
      if (kv.first >= to_index && kv.first < from_index) {
        new_idx = kv.first + 1;
      }
    }
    new_annotations[new_idx] = std::move(kv.second);
  }
  new_annotations[to_index] = std::move(annots);
  g_page_annotations = std::move(new_annotations);

  return 1;
}

// Get working page width
EMSCRIPTEN_KEEPALIVE
float nanopdf_work_get_page_width(int page_index) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    return 0.0f;
  }
  const auto& pinfo = g_working_pages[page_index];
  // Account for rotation
  if (pinfo.rotation == 90 || pinfo.rotation == 270) {
    return static_cast<float>(pinfo.crop_h);
  }
  return static_cast<float>(pinfo.crop_w);
}

// Get working page height
EMSCRIPTEN_KEEPALIVE
float nanopdf_work_get_page_height(int page_index) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    return 0.0f;
  }
  const auto& pinfo = g_working_pages[page_index];
  // Account for rotation
  if (pinfo.rotation == 90 || pinfo.rotation == 270) {
    return static_cast<float>(pinfo.crop_w);
  }
  return static_cast<float>(pinfo.crop_h);
}

// Get page rotation
EMSCRIPTEN_KEEPALIVE
int nanopdf_work_get_rotation(int page_index) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    return 0;
  }
  return g_working_pages[page_index].rotation;
}

// Rotate page (angle: 0, 90, 180, 270)
EMSCRIPTEN_KEEPALIVE
int nanopdf_work_rotate_page(int page_index, int angle) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    g_last_error = "Invalid page index";
    return 0;
  }

  // Normalize angle
  angle = ((angle % 360) + 360) % 360;
  if (angle != 0 && angle != 90 && angle != 180 && angle != 270) {
    g_last_error = "Rotation must be 0, 90, 180, or 270";
    return 0;
  }

  g_working_pages[page_index].rotation = angle;
  return 1;
}

// Crop page
EMSCRIPTEN_KEEPALIVE
int nanopdf_work_crop_page(int page_index, float x, float y, float w, float h) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    g_last_error = "Invalid page index";
    return 0;
  }

  auto& pinfo = g_working_pages[page_index];

  // Validate crop box
  if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
      x + w > pinfo.width || y + h > pinfo.height) {
    g_last_error = "Invalid crop box";
    return 0;
  }

  pinfo.crop_x = x;
  pinfo.crop_y = y;
  pinfo.crop_w = w;
  pinfo.crop_h = h;

  return 1;
}

// Reset crop
EMSCRIPTEN_KEEPALIVE
int nanopdf_work_reset_crop(int page_index) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    g_last_error = "Invalid page index";
    return 0;
  }

  auto& pinfo = g_working_pages[page_index];
  pinfo.crop_x = 0;
  pinfo.crop_y = 0;
  pinfo.crop_w = pinfo.width;
  pinfo.crop_h = pinfo.height;

  return 1;
}

// ============================================================
// Annotations
// ============================================================

// Add text annotation
EMSCRIPTEN_KEEPALIVE
int nanopdf_annot_add_text(int page_index, float x, float y,
                           const char* text, const char* font_name, float font_size,
                           float r, float g, float b, float a) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    g_last_error = "Invalid page index";
    return -1;
  }

  Annotation annot;
  annot.type = 0;  // Text
  annot.x1 = x;
  annot.y1 = y;
  annot.x2 = 0;
  annot.y2 = 0;
  annot.r = r;
  annot.g = g;
  annot.b = b;
  annot.a = a;
  annot.text = text ? text : "";
  annot.font_name = font_name ? font_name : "Helvetica";
  annot.font_size = font_size > 0 ? font_size : 12;
  annot.line_width = 0;
  annot.filled = false;

  g_page_annotations[page_index].push_back(annot);
  return static_cast<int>(g_page_annotations[page_index].size() - 1);
}

// Add line annotation
EMSCRIPTEN_KEEPALIVE
int nanopdf_annot_add_line(int page_index, float x1, float y1, float x2, float y2,
                           float line_width, float r, float g, float b, float a) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    g_last_error = "Invalid page index";
    return -1;
  }

  Annotation annot;
  annot.type = 1;  // Line
  annot.x1 = x1;
  annot.y1 = y1;
  annot.x2 = x2;
  annot.y2 = y2;
  annot.r = r;
  annot.g = g;
  annot.b = b;
  annot.a = a;
  annot.line_width = line_width > 0 ? line_width : 1;
  annot.filled = false;

  g_page_annotations[page_index].push_back(annot);
  return static_cast<int>(g_page_annotations[page_index].size() - 1);
}

// Add rectangle annotation
EMSCRIPTEN_KEEPALIVE
int nanopdf_annot_add_rect(int page_index, float x, float y, float w, float h,
                           float line_width, float r, float g, float b, float a, int filled) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    g_last_error = "Invalid page index";
    return -1;
  }

  Annotation annot;
  annot.type = 2;  // Rectangle
  annot.x1 = x;
  annot.y1 = y;
  annot.x2 = w;  // Width
  annot.y2 = h;  // Height
  annot.r = r;
  annot.g = g;
  annot.b = b;
  annot.a = a;
  annot.line_width = line_width > 0 ? line_width : 1;
  annot.filled = (filled != 0);

  g_page_annotations[page_index].push_back(annot);
  return static_cast<int>(g_page_annotations[page_index].size() - 1);
}

// Add oval/ellipse annotation
EMSCRIPTEN_KEEPALIVE
int nanopdf_annot_add_oval(int page_index, float cx, float cy, float rx, float ry,
                           float line_width, float r, float g, float b, float a, int filled) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    g_last_error = "Invalid page index";
    return -1;
  }

  Annotation annot;
  annot.type = 3;  // Oval
  annot.x1 = cx;
  annot.y1 = cy;
  annot.x2 = rx;  // Radius X
  annot.y2 = ry;  // Radius Y
  annot.r = r;
  annot.g = g;
  annot.b = b;
  annot.a = a;
  annot.line_width = line_width > 0 ? line_width : 1;
  annot.filled = (filled != 0);

  g_page_annotations[page_index].push_back(annot);
  return static_cast<int>(g_page_annotations[page_index].size() - 1);
}

// Add highlight annotation
EMSCRIPTEN_KEEPALIVE
int nanopdf_annot_add_highlight(int page_index, float x, float y, float w, float h,
                                float r, float g, float b, float a) {
  if (page_index < 0 || page_index >= static_cast<int>(g_working_pages.size())) {
    g_last_error = "Invalid page index";
    return -1;
  }

  Annotation annot;
  annot.type = 4;  // Highlight
  annot.x1 = x;
  annot.y1 = y;
  annot.x2 = w;
  annot.y2 = h;
  annot.r = r;
  annot.g = g;
  annot.b = b;
  annot.a = a;
  annot.line_width = 0;
  annot.filled = true;

  g_page_annotations[page_index].push_back(annot);
  return static_cast<int>(g_page_annotations[page_index].size() - 1);
}

// Remove annotation
EMSCRIPTEN_KEEPALIVE
int nanopdf_annot_remove(int page_index, int annot_index) {
  auto it = g_page_annotations.find(page_index);
  if (it == g_page_annotations.end()) {
    g_last_error = "No annotations on page";
    return 0;
  }

  if (annot_index < 0 || annot_index >= static_cast<int>(it->second.size())) {
    g_last_error = "Invalid annotation index";
    return 0;
  }

  it->second.erase(it->second.begin() + annot_index);
  return 1;
}

// Clear all annotations on a page
EMSCRIPTEN_KEEPALIVE
void nanopdf_annot_clear_page(int page_index) {
  g_page_annotations.erase(page_index);
}

// Get annotation count on a page
EMSCRIPTEN_KEEPALIVE
int nanopdf_annot_get_count(int page_index) {
  auto it = g_page_annotations.find(page_index);
  if (it == g_page_annotations.end()) return 0;
  return static_cast<int>(it->second.size());
}

// ============================================================
// PDF Export
// ============================================================

// Set export render quality (1-4, default 2)
// 1 = 72 DPI (fast, smaller files)
// 2 = 144 DPI (good quality, default)
// 3 = 216 DPI (high quality)
// 4 = 288 DPI (maximum quality, large files)
EMSCRIPTEN_KEEPALIVE
void nanopdf_export_set_quality(int scale) {
  if (scale < 1) scale = 1;
  if (scale > 4) scale = 4;
  g_export_render_scale = scale;
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_export_get_quality() {
  return g_export_render_scale;
}

// Configure password protection for writer-backed PDF export.
// algorithm: 1=RC4-40, 2=RC4-128, 3=AES-128, 4=AES-256.
EMSCRIPTEN_KEEPALIVE
int nanopdf_export_set_passwords(const char* user_password,
                                 const char* owner_password,
                                 int algorithm) {
  std::string user = user_password ? user_password : "";
  std::string owner = owner_password ? owner_password : "";
  if (user.empty() && owner.empty()) {
    g_export_encryption_enabled = false;
    g_export_encryption_config = nanopdf::EncryptionConfig();
    return 1;
  }

  if (owner.empty()) {
    owner = user;
  }
  if (owner.empty()) {
    g_last_error = "Owner password is required for encryption";
    return 0;
  }

  nanopdf::EncryptionConfig config;
  switch (algorithm) {
    case 1:
      config.algorithm = nanopdf::EncryptionAlgorithm::RC4_40;
      break;
    case 2:
      config.algorithm = nanopdf::EncryptionAlgorithm::RC4_128;
      break;
    case 4:
      config.algorithm = nanopdf::EncryptionAlgorithm::AES_256;
      break;
    case 3:
    default:
      config.algorithm = nanopdf::EncryptionAlgorithm::AES_128;
      break;
  }
  config.user_password = user;
  config.owner_password = owner;
  config.permissions = nanopdf::UserPermissions::view_only();
  config.permissions.allow_print = true;
  config.permissions.allow_copy = false;
  config.encrypt_metadata = true;

  g_export_encryption_config = config;
  g_export_encryption_enabled = true;
  return 1;
}

static std::vector<uint8_t> g_output_buffer;

// Helper: Render a source page and add it to the writer
// Returns image name or empty string on failure
static std::string render_source_page_to_image(
    nanopdf::PdfWriter* writer,
    const DocumentInfo& doc,
    int source_page_index,
    const PageInfo& pinfo,
    int render_scale = 2) {  // 2x resolution for quality

  if (!doc.pdf || source_page_index < 0 ||
      source_page_index >= static_cast<int>(doc.pdf->catalog.pages.size())) {
    return "";
  }

  const auto& page = doc.pdf->catalog.pages[source_page_index];

  // Calculate render dimensions
  int render_width = static_cast<int>(pinfo.width * render_scale);
  int render_height = static_cast<int>(pinfo.height * render_scale);

  if (render_width <= 0 || render_height <= 0) {
    return "";
  }

#ifdef NANOPDF_USE_THORVG
  nanopdf::ThorVGBackend backend;
  if (!backend.initialize(static_cast<uint32_t>(render_width),
                          static_cast<uint32_t>(render_height))) {
    return "";
  }

  nanopdf::ThorVGRenderOptions options;
  options.dpi = 72.0f * render_scale;

  auto result = backend.render_page(*doc.pdf, page, options);
  if (!result.success || result.pixels.empty()) {
    return "";
  }

  // Convert RGBA to RGB for PDF embedding (PDF doesn't handle RGBA well in XObjects)
  std::vector<uint8_t> rgb_data;
  rgb_data.reserve(result.width * result.height * 3);
  for (size_t i = 0; i < result.pixels.size(); i += 4) {
    // Pre-multiply alpha onto white background
    float alpha = result.pixels[i + 3] / 255.0f;
    rgb_data.push_back(static_cast<uint8_t>(result.pixels[i + 0] * alpha + 255 * (1 - alpha)));
    rgb_data.push_back(static_cast<uint8_t>(result.pixels[i + 1] * alpha + 255 * (1 - alpha)));
    rgb_data.push_back(static_cast<uint8_t>(result.pixels[i + 2] * alpha + 255 * (1 - alpha)));
  }

  // Create ImageData
  nanopdf::ImageData img;
  img.raw_data = std::move(rgb_data);
  img.width = static_cast<int>(result.width);
  img.height = static_cast<int>(result.height);
  img.channels = 3;
  img.format = nanopdf::ImageFormat::Unknown;

  // Add to writer and return name
  return writer->add_image(img, nanopdf::ImageCompression::DCT);

#elif defined(NANOPDF_USE_BLEND2D)
  nanopdf::Blend2DBackend backend;
  if (!backend.initialize(static_cast<uint32_t>(render_width),
                          static_cast<uint32_t>(render_height))) {
    return "";
  }

  nanopdf::RenderOptions options;
  options.dpi = 72.0f * render_scale;

  auto result = backend.render_page(*doc.pdf, page, options);
  if (!result.success || result.pixels.empty()) {
    return "";
  }

  // Convert RGBA to RGB
  std::vector<uint8_t> rgb_data;
  rgb_data.reserve(result.width * result.height * 3);
  for (size_t i = 0; i < result.pixels.size(); i += 4) {
    float alpha = result.pixels[i + 3] / 255.0f;
    rgb_data.push_back(static_cast<uint8_t>(result.pixels[i + 0] * alpha + 255 * (1 - alpha)));
    rgb_data.push_back(static_cast<uint8_t>(result.pixels[i + 1] * alpha + 255 * (1 - alpha)));
    rgb_data.push_back(static_cast<uint8_t>(result.pixels[i + 2] * alpha + 255 * (1 - alpha)));
  }

  nanopdf::ImageData img;
  img.raw_data = std::move(rgb_data);
  img.width = static_cast<int>(result.width);
  img.height = static_cast<int>(result.height);
  img.channels = 3;
  img.format = nanopdf::ImageFormat::Unknown;

  return writer->add_image(img, nanopdf::ImageCompression::DCT);

#else
  (void)writer;
  (void)doc;
  (void)source_page_index;
  (void)pinfo;
  (void)render_scale;
  return "";
#endif
}

// Export working document to PDF
EMSCRIPTEN_KEEPALIVE
int nanopdf_export_pdf() {
  if (g_working_pages.empty()) {
    g_last_error = "No pages to export";
    return 0;
  }

  if (g_writer) {
    delete g_writer;
  }
  g_writer = new nanopdf::PdfWriter();
  g_writer->set_title("nanopdf Document");
  g_writer->set_creator("nanopdf WASM");
  if (g_export_encryption_enabled) {
    g_writer->set_encryption(g_export_encryption_config);
    if (!g_writer->is_encrypted()) {
      g_last_error = "Failed to enable PDF encryption";
      return 0;
    }
  }

  std::string std_font = g_writer->add_standard_font(nanopdf::StandardFont::Helvetica);

  // Pre-render all source pages to images
  std::vector<std::string> page_images(g_working_pages.size());
  for (size_t i = 0; i < g_working_pages.size(); i++) {
    const auto& pinfo = g_working_pages[i];
    if (pinfo.source_doc_id >= 0) {
      auto doc_it = g_documents.find(pinfo.source_doc_id);
      if (doc_it != g_documents.end()) {
        page_images[i] = render_source_page_to_image(
            g_writer, doc_it->second, pinfo.source_page_index, pinfo,
            g_export_render_scale);
      }
    }
  }

  for (size_t i = 0; i < g_working_pages.size(); i++) {
    const auto& pinfo = g_working_pages[i];

    // Calculate page size accounting for rotation
    double page_w, page_h;
    if (pinfo.rotation == 90 || pinfo.rotation == 270) {
      page_w = pinfo.crop_h;
      page_h = pinfo.crop_w;
    } else {
      page_w = pinfo.crop_w;
      page_h = pinfo.crop_h;
    }

    nanopdf::PageSize size{page_w, page_h};

    // Get annotations for this page
    std::vector<Annotation>* annots = nullptr;
    auto annot_it = g_page_annotations.find(static_cast<int>(i));
    if (annot_it != g_page_annotations.end()) {
      annots = &annot_it->second;
    }

    // Capture image name for this page
    std::string img_name = page_images[i];
    int rotation = pinfo.rotation;

    g_writer->add_page(size, [&pinfo, &std_font, annots, page_w, page_h, img_name, rotation](nanopdf::PageBuilder& builder) {
      // Draw background (white)
      builder.save_state();
      builder.set_fill_color(1.0, 1.0, 1.0);
      builder.rectangle(0, 0, page_w, page_h);
      builder.fill();
      builder.restore_state();

      // Draw source page content as image
      if (!img_name.empty()) {
        builder.save_state();

        // Handle rotation
        if (rotation == 90) {
          builder.concat_matrix(0, 1, -1, 0, page_w, 0);
          builder.draw_image(img_name, 0, 0, page_h, page_w);
        } else if (rotation == 180) {
          builder.concat_matrix(-1, 0, 0, -1, page_w, page_h);
          builder.draw_image(img_name, 0, 0, page_w, page_h);
        } else if (rotation == 270) {
          builder.concat_matrix(0, -1, 1, 0, 0, page_h);
          builder.draw_image(img_name, 0, 0, page_h, page_w);
        } else {
          // No rotation
          builder.draw_image(img_name, 0, 0, page_w, page_h);
        }

        builder.restore_state();
      } else if (pinfo.source_doc_id >= 0) {
        // Fallback: show placeholder if rendering failed
        builder.begin_text();
        builder.set_font(std_font, 12);
        builder.show_text_at(10, page_h - 20, "[Page content - rendering unavailable]");
        builder.end_text();
      }

      // Draw annotations
      if (annots) {
        for (const auto& annot : *annots) {
          builder.save_state();

          if (annot.a < 1.0) {
            builder.set_fill_alpha(annot.a);
            builder.set_stroke_alpha(annot.a);
          }

          switch (annot.type) {
            case 0:  // Text
              builder.begin_text();
              builder.set_font(std_font, annot.font_size);
              builder.set_fill_color(annot.r, annot.g, annot.b);
              builder.show_text_at(annot.x1, annot.y1, annot.text);
              builder.end_text();
              break;

            case 1:  // Line
              builder.set_line_width(annot.line_width);
              builder.set_stroke_color(annot.r, annot.g, annot.b);
              builder.move_to(annot.x1, annot.y1);
              builder.line_to(annot.x2, annot.y2);
              builder.stroke();
              break;

            case 2:  // Rectangle
              builder.set_line_width(annot.line_width);
              if (annot.filled) {
                builder.set_fill_color(annot.r, annot.g, annot.b);
                builder.rectangle(annot.x1, annot.y1, annot.x2, annot.y2);
                builder.fill();
              } else {
                builder.set_stroke_color(annot.r, annot.g, annot.b);
                builder.rectangle(annot.x1, annot.y1, annot.x2, annot.y2);
                builder.stroke();
              }
              break;

            case 3:  // Oval
              builder.set_line_width(annot.line_width);
              if (annot.filled) {
                builder.set_fill_color(annot.r, annot.g, annot.b);
              } else {
                builder.set_stroke_color(annot.r, annot.g, annot.b);
              }
              // Approximate ellipse with bezier curves
              {
                double cx = annot.x1, cy = annot.y1;
                double rx = annot.x2, ry = annot.y2;
                double k = 0.5522847498;  // Magic number for circle approximation
                builder.move_to(cx + rx, cy);
                builder.curve_to(cx + rx, cy + ry*k, cx + rx*k, cy + ry, cx, cy + ry);
                builder.curve_to(cx - rx*k, cy + ry, cx - rx, cy + ry*k, cx - rx, cy);
                builder.curve_to(cx - rx, cy - ry*k, cx - rx*k, cy - ry, cx, cy - ry);
                builder.curve_to(cx + rx*k, cy - ry, cx + rx, cy - ry*k, cx + rx, cy);
                builder.close_path();
              }
              if (annot.filled) {
                builder.fill();
              } else {
                builder.stroke();
              }
              break;

            case 4:  // Highlight
              builder.set_fill_color(annot.r, annot.g, annot.b);
              builder.rectangle(annot.x1, annot.y1, annot.x2, annot.y2);
              builder.fill();
              break;
          }

          builder.restore_state();
        }
      }
    });
  }

  // Write to memory
  g_output_buffer.clear();
  auto result = g_writer->write_to_memory(g_output_buffer);

  if (!result.success) {
    g_last_error = result.error;
    return 0;
  }

  return 1;
}

// Get export buffer pointer
EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_export_get_buffer() {
  if (g_output_buffer.empty()) return nullptr;
  return g_output_buffer.data();
}

// Get export buffer size
EMSCRIPTEN_KEEPALIVE
size_t nanopdf_export_get_size() {
  return g_output_buffer.size();
}

// ============================================================
// Batch Processing API
// ============================================================

// Batch extract text from multiple pages
// page_indices: comma-separated list like "0,1,2" or "all" for all pages
// Returns JSON: {"pages":[{"page":0,"text":"..."},{"page":1,"text":"..."}]}
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_batch_extract_text(const char* page_indices) {
  if (!g_pdf) {
    g_text_buffer = "{\"error\":\"No PDF loaded\"}";
    return g_text_buffer.c_str();
  }

  std::vector<int> pages;
  std::string indices_str(page_indices ? page_indices : "all");

  if (indices_str == "all") {
    for (size_t i = 0; i < g_pdf->catalog.pages.size(); i++) {
      pages.push_back(static_cast<int>(i));
    }
  } else {
    // Parse comma-separated page indices
    size_t pos = 0;
    std::string token;
    while ((pos = indices_str.find(',')) != std::string::npos) {
      token = indices_str.substr(0, pos);
      int idx = nanopdf::stoi_or(token.c_str());
      if (idx >= 0 && idx < static_cast<int>(g_pdf->catalog.pages.size())) {
        pages.push_back(idx);
      }
      indices_str.erase(0, pos + 1);
    }
    if (!indices_str.empty()) {
      int idx = nanopdf::stoi_or(indices_str.c_str());
      if (idx >= 0 && idx < static_cast<int>(g_pdf->catalog.pages.size())) {
        pages.push_back(idx);
      }
    }
  }

  std::string json = "{\"pages\":[";
  bool first = true;
  for (int page_idx : pages) {
    if (!first) json += ",";
    first = false;

    const auto& page = g_pdf->catalog.pages[page_idx];
    std::string text = nanopdf::extract_text_from_page(*g_pdf, page);

    json += "{\"page\":" + std::to_string(page_idx);
    json += ",\"text\":\"" + json_escape(text) + "\"}";
  }
  json += "]}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Batch get page info for multiple pages
// Returns JSON with page dimensions, rotation, etc.
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_batch_get_page_info(const char* page_indices) {
  if (!g_pdf) {
    g_text_buffer = "{\"error\":\"No PDF loaded\"}";
    return g_text_buffer.c_str();
  }

  std::vector<int> pages;
  std::string indices_str(page_indices ? page_indices : "all");

  if (indices_str == "all") {
    for (size_t i = 0; i < g_pdf->catalog.pages.size(); i++) {
      pages.push_back(static_cast<int>(i));
    }
  } else {
    size_t pos = 0;
    std::string token;
    while ((pos = indices_str.find(',')) != std::string::npos) {
      token = indices_str.substr(0, pos);
      int idx = nanopdf::stoi_or(token.c_str());
      if (idx >= 0 && idx < static_cast<int>(g_pdf->catalog.pages.size())) {
        pages.push_back(idx);
      }
      indices_str.erase(0, pos + 1);
    }
    if (!indices_str.empty()) {
      int idx = nanopdf::stoi_or(indices_str.c_str());
      if (idx >= 0 && idx < static_cast<int>(g_pdf->catalog.pages.size())) {
        pages.push_back(idx);
      }
    }
  }

  std::string json = "{\"pages\":[";
  bool first = true;
  for (int page_idx : pages) {
    if (!first) json += ",";
    first = false;

    const auto& page = g_pdf->catalog.pages[page_idx];
    double width = (page.media_box.size() >= 4) ? (page.media_box[2] - page.media_box[0]) : 612.0;
    double height = (page.media_box.size() >= 4) ? (page.media_box[3] - page.media_box[1]) : 792.0;

    json += "{\"page\":" + std::to_string(page_idx);
    json += ",\"width\":" + std::to_string(width);
    json += ",\"height\":" + std::to_string(height);
    json += ",\"rotation\":" + std::to_string(page.rotate);

    // Crop box if different from media box
    if (page.crop_box.size() >= 4 &&
        (page.crop_box[0] != 0 || page.crop_box[1] != 0 ||
         page.crop_box[2] != width || page.crop_box[3] != height)) {
      json += ",\"cropBox\":{";
      json += "\"llx\":" + std::to_string(page.crop_box[0]) + ",";
      json += "\"lly\":" + std::to_string(page.crop_box[1]) + ",";
      json += "\"urx\":" + std::to_string(page.crop_box[2]) + ",";
      json += "\"ury\":" + std::to_string(page.crop_box[3]) + "}";
    }

    json += ",\"fontCount\":" + std::to_string(page.fonts.size());
    json += ",\"annotationCount\":" + std::to_string(page.annotations.size());
    json += "}";
  }
  json += "]}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Batch search text across multiple pages
// Returns JSON: {"results":[{"page":0,"matches":[{"start":10,"end":15,"context":"..."}]}]}
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_batch_find_text(const char* search_term, const char* page_indices) {
  if (!g_pdf) {
    g_text_buffer = "{\"error\":\"No PDF loaded\"}";
    return g_text_buffer.c_str();
  }

  if (!search_term || strlen(search_term) == 0) {
    g_text_buffer = "{\"error\":\"Empty search term\"}";
    return g_text_buffer.c_str();
  }

  std::vector<int> pages;
  std::string indices_str(page_indices ? page_indices : "all");

  if (indices_str == "all") {
    for (size_t i = 0; i < g_pdf->catalog.pages.size(); i++) {
      pages.push_back(static_cast<int>(i));
    }
  } else {
    size_t pos = 0;
    std::string token;
    while ((pos = indices_str.find(',')) != std::string::npos) {
      token = indices_str.substr(0, pos);
      int idx = nanopdf::stoi_or(token.c_str());
      if (idx >= 0 && idx < static_cast<int>(g_pdf->catalog.pages.size())) {
        pages.push_back(idx);
      }
      indices_str.erase(0, pos + 1);
    }
    if (!indices_str.empty()) {
      int idx = nanopdf::stoi_or(indices_str.c_str());
      if (idx >= 0 && idx < static_cast<int>(g_pdf->catalog.pages.size())) {
        pages.push_back(idx);
      }
    }
  }

  std::string search(search_term);
  std::string json = "{\"searchTerm\":\"" + json_escape(search) + "\",\"results\":[";
  bool first_page = true;
  int total_matches = 0;

  for (int page_idx : pages) {
    const auto& page = g_pdf->catalog.pages[page_idx];
    std::vector<nanopdf::TextSearchResult> matches =
        nanopdf::search_text_on_page(*g_pdf, page, search, false);

    if (!matches.empty()) {
      if (!first_page) json += ",";
      first_page = false;

      json += "{\"page\":" + std::to_string(page_idx);
      json += ",\"matchCount\":" + std::to_string(matches.size());
      json += ",\"matches\":[";

      bool first_match = true;
      for (const auto& match : matches) {
        if (!first_match) json += ",";
        first_match = false;
        auto match_copy = match;
        match_copy.page_number = static_cast<uint32_t>(page_idx);
        json += search_result_to_json(match_copy);
      }
      json += "]}";
      total_matches += static_cast<int>(matches.size());
    }
  }
  json += "],\"totalMatches\":" + std::to_string(total_matches) + "}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Batch extract text with layout from multiple pages
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_batch_extract_text_layout(const char* page_indices) {
  if (!g_pdf) {
    g_text_buffer = "{\"error\":\"No PDF loaded\"}";
    return g_text_buffer.c_str();
  }

  std::vector<int> pages;
  std::string indices_str(page_indices ? page_indices : "all");

  if (indices_str == "all") {
    for (size_t i = 0; i < g_pdf->catalog.pages.size(); i++) {
      pages.push_back(static_cast<int>(i));
    }
  } else {
    size_t pos = 0;
    std::string token;
    while ((pos = indices_str.find(',')) != std::string::npos) {
      token = indices_str.substr(0, pos);
      int idx = nanopdf::stoi_or(token.c_str());
      if (idx >= 0 && idx < static_cast<int>(g_pdf->catalog.pages.size())) {
        pages.push_back(idx);
      }
      indices_str.erase(0, pos + 1);
    }
    if (!indices_str.empty()) {
      int idx = nanopdf::stoi_or(indices_str.c_str());
      if (idx >= 0 && idx < static_cast<int>(g_pdf->catalog.pages.size())) {
        pages.push_back(idx);
      }
    }
  }

  std::string json = "{\"pages\":[";
  bool first_page = true;

  for (int page_idx : pages) {
    if (!first_page) json += ",";
    first_page = false;

    const auto& page = g_pdf->catalog.pages[page_idx];
    auto text_page = nanopdf::extract_text_layout(*g_pdf, page);

    json += "{\"page\":" + std::to_string(page_idx);

    if (!text_page) {
      json += ",\"error\":\"Failed to extract layout\"}";
      continue;
    }

    json += ",\"pageWidth\":" + std::to_string(text_page->page_width);
    json += ",\"pageHeight\":" + std::to_string(text_page->page_height);
    json += ",\"charCount\":" + std::to_string(text_page->chars.size());
    json += ",\"lineCount\":" + std::to_string(text_page->lines.size());
    json += ",\"wordCount\":" + std::to_string(text_page->words.size());

    // Include plain text
    json += ",\"text\":\"" + json_escape(text_page->get_text()) + "\"}";
  }
  json += "]}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// ============================================================
// Bookmark/Outline API
// ============================================================

// Helper to recursively serialize outline items to JSON
static void serialize_outline_item(const nanopdf::OutlineItem* item,
                                    std::string& json, bool first) {
  if (!first) json += ",";

  json += "{\"title\":\"" + json_escape(item->title) + "\"";

  // Action type
  switch (item->action_type) {
    case nanopdf::OutlineAction::GoTo:
      json += ",\"action\":\"goto\"";
      json += ",\"page\":" + std::to_string(item->dest_page);
      if (!item->dest_position.empty()) {
        json += ",\"position\":[";
        for (size_t i = 0; i < item->dest_position.size(); i++) {
          if (i > 0) json += ",";
          json += std::to_string(item->dest_position[i]);
        }
        json += "]";
      }
      break;
    case nanopdf::OutlineAction::URI:
      json += ",\"action\":\"uri\"";
      json += ",\"uri\":\"" + json_escape(item->uri) + "\"";
      break;
    case nanopdf::OutlineAction::GoToR:
      json += ",\"action\":\"gotor\"";
      json += ",\"file\":\"" + json_escape(item->file) + "\"";
      json += ",\"page\":" + std::to_string(item->dest_page);
      break;
    case nanopdf::OutlineAction::Launch:
      json += ",\"action\":\"launch\"";
      json += ",\"file\":\"" + json_escape(item->file) + "\"";
      break;
    default:
      json += ",\"action\":\"unknown\"";
      break;
  }

  // Style
  if (!item->color.empty() && item->color.size() >= 3) {
    json += ",\"color\":[";
    json += std::to_string(item->color[0]) + ",";
    json += std::to_string(item->color[1]) + ",";
    json += std::to_string(item->color[2]) + "]";
  }
  if (item->bold) json += ",\"bold\":true";
  if (item->italic) json += ",\"italic\":true";
  json += ",\"open\":" + std::string(item->open ? "true" : "false");

  // Children
  if (!item->children.empty()) {
    json += ",\"children\":[";
    bool child_first = true;
    for (const auto& child : item->children) {
      serialize_outline_item(child.get(), json, child_first);
      child_first = false;
    }
    json += "]";
  }

  json += "}";
}

// Get document outline/bookmarks as JSON
// Returns hierarchical structure of bookmarks
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_get_outline() {
  if (!g_pdf) {
    g_text_buffer = "{\"error\":\"No PDF loaded\"}";
    return g_text_buffer.c_str();
  }

  // Ensure outline is loaded
  g_pdf->ensure_outline_loaded();

  if (!g_pdf->catalog.outline_root) {
    g_text_buffer = "{\"outline\":null}";
    return g_text_buffer.c_str();
  }

  std::string json = "{\"outline\":[";
  bool first = true;

  // Serialize all top-level items
  for (const auto& child : g_pdf->catalog.outline_root->children) {
    serialize_outline_item(child.get(), json, first);
    first = false;
  }

  json += "]}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Get flat list of bookmarks (easier for simple navigation)
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_get_outline_flat() {
  if (!g_pdf) {
    g_text_buffer = "{\"error\":\"No PDF loaded\"}";
    return g_text_buffer.c_str();
  }

  g_pdf->ensure_outline_loaded();

  if (!g_pdf->catalog.outline_root) {
    g_text_buffer = "{\"bookmarks\":[]}";
    return g_text_buffer.c_str();
  }

  // Flatten outline with depth information
  struct FlatItem {
    const nanopdf::OutlineItem* item;
    int depth;
  };

  std::vector<FlatItem> flat;
  std::function<void(const nanopdf::OutlineItem*, int)> flatten =
      [&](const nanopdf::OutlineItem* item, int depth) {
        for (const auto& child : item->children) {
          flat.push_back({child.get(), depth});
          flatten(child.get(), depth + 1);
        }
      };

  flatten(g_pdf->catalog.outline_root.get(), 0);

  std::string json = "{\"bookmarks\":[";
  bool first = true;

  for (const auto& f : flat) {
    if (!first) json += ",";
    first = false;

    json += "{\"title\":\"" + json_escape(f.item->title) + "\"";
    json += ",\"depth\":" + std::to_string(f.depth);

    if (f.item->action_type == nanopdf::OutlineAction::GoTo) {
      json += ",\"page\":" + std::to_string(f.item->dest_page);
    } else if (f.item->action_type == nanopdf::OutlineAction::URI) {
      json += ",\"uri\":\"" + json_escape(f.item->uri) + "\"";
    }

    json += "}";
  }

  json += "],\"count\":" + std::to_string(flat.size()) + "}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Check if document has outline/bookmarks
EMSCRIPTEN_KEEPALIVE
int nanopdf_has_outline() {
  if (!g_pdf) return 0;
  g_pdf->ensure_outline_loaded();
  return (g_pdf->catalog.outline_root &&
          !g_pdf->catalog.outline_root->children.empty()) ? 1 : 0;
}

// Get bookmark count (total including nested)
EMSCRIPTEN_KEEPALIVE
int nanopdf_get_outline_count() {
  if (!g_pdf) return 0;
  g_pdf->ensure_outline_loaded();
  if (!g_pdf->catalog.outline_root) return 0;

  std::function<int(const nanopdf::OutlineItem*)> count_items =
      [&](const nanopdf::OutlineItem* item) -> int {
        int count = 0;
        for (const auto& child : item->children) {
          count += 1 + count_items(child.get());
        }
        return count;
      };

  return count_items(g_pdf->catalog.outline_root.get());
}

// ============================================================
// Form Field Listing API
// ============================================================

// Get list of all form fields in the document
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_get_form_fields() {
  if (!g_pdf) {
    g_text_buffer = "{\"error\":\"No PDF loaded\"}";
    return g_text_buffer.c_str();
  }

  g_pdf->ensure_acro_form_loaded();

  if (g_pdf->catalog.form_fields.empty()) {
    g_text_buffer = "{\"fields\":[]}";
    return g_text_buffer.c_str();
  }

  std::string json = "{\"fields\":[";
  bool first = true;

  for (const auto& field : g_pdf->catalog.form_fields) {
    if (!first) json += ",";
    first = false;

    json += "{\"name\":\"" + json_escape(field->partial_name) + "\"";
    json += ",\"fullName\":\"" + json_escape(field->full_name) + "\"";

    // Field type
    switch (field->type) {
      case nanopdf::FieldType::Text:
        json += ",\"type\":\"text\"";
        {
          const auto* tf = static_cast<const nanopdf::TextField*>(field.get());
          json += ",\"maxLength\":" + std::to_string(tf->max_length);
          // Check multiline/password via field flags
          bool multiline = (tf->flags & static_cast<uint32_t>(nanopdf::FormFieldFlags::Multiline)) != 0;
          bool password = (tf->flags & static_cast<uint32_t>(nanopdf::FormFieldFlags::Password)) != 0;
          json += ",\"multiline\":" + std::string(multiline ? "true" : "false");
          json += ",\"password\":" + std::string(password ? "true" : "false");
        }
        break;
      case nanopdf::FieldType::Button:
        json += ",\"type\":\"button\"";
        {
          const auto* bf = static_cast<const nanopdf::ButtonField*>(field.get());
          json += ",\"buttonType\":\"";
          switch (bf->button_type) {
            case nanopdf::ButtonField::PushButton: json += "push"; break;
            case nanopdf::ButtonField::CheckBox: json += "checkbox"; break;
            case nanopdf::ButtonField::RadioButton: json += "radio"; break;
          }
          json += "\"";
        }
        break;
      case nanopdf::FieldType::Choice:
        json += ",\"type\":\"choice\"";
        {
          const auto* cf = static_cast<const nanopdf::ChoiceField*>(field.get());
          json += ",\"options\":[";
          bool first_opt = true;
          for (const auto& opt : cf->options) {
            if (!first_opt) json += ",";
            first_opt = false;
            json += "\"" + json_escape(opt) + "\"";
          }
          json += "]";
          if (!cf->selected_indices.empty()) {
            json += ",\"selected\":[";
            bool first_sel = true;
            for (int idx : cf->selected_indices) {
              if (!first_sel) json += ",";
              first_sel = false;
              json += std::to_string(idx);
            }
            json += "]";
          }
        }
        break;
      case nanopdf::FieldType::Signature:
        json += ",\"type\":\"signature\"";
        break;
      default:
        json += ",\"type\":\"unknown\"";
        break;
    }

    // Common field properties via flags
    bool read_only = (field->flags & static_cast<uint32_t>(nanopdf::FormFieldFlags::ReadOnly)) != 0;
    bool required = (field->flags & static_cast<uint32_t>(nanopdf::FormFieldFlags::Required)) != 0;
    json += ",\"readOnly\":" + std::string(read_only ? "true" : "false");
    json += ",\"required\":" + std::string(required ? "true" : "false");

    // Widget info if available
    if (!field->widgets.empty()) {
      json += ",\"widgets\":[";
      bool first_w = true;
      for (const auto& widget : field->widgets) {
        if (!first_w) json += ",";
        first_w = false;
        json += "{";
        if (widget->rect.size() >= 4) {
          json += "\"rect\":{";
          json += "\"x\":" + std::to_string(widget->rect[0]) + ",";
          json += "\"y\":" + std::to_string(widget->rect[1]) + ",";
          json += "\"width\":" + std::to_string(widget->rect[2] - widget->rect[0]) + ",";
          json += "\"height\":" + std::to_string(widget->rect[3] - widget->rect[1]) + "}";
        }
        json += "}";
      }
      json += "]";
    }

    json += "}";
  }

  json += "],\"count\":" + std::to_string(g_pdf->catalog.form_fields.size()) + "}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Check if document has form fields
EMSCRIPTEN_KEEPALIVE
int nanopdf_has_form_fields() {
  if (!g_pdf) return 0;
  g_pdf->ensure_acro_form_loaded();
  return g_pdf->catalog.form_fields.empty() ? 0 : 1;
}

// Get form field count
EMSCRIPTEN_KEEPALIVE
int nanopdf_get_form_field_count() {
  if (!g_pdf) return 0;
  g_pdf->ensure_acro_form_loaded();
  return static_cast<int>(g_pdf->catalog.form_fields.size());
}

// ============================================================
// Embedded Fonts API
// ============================================================

#ifdef NANOPDF_EMBED_FONTS

// Check if embedded fonts are available
EMSCRIPTEN_KEEPALIVE
int nanopdf_fonts_available() {
  return 1;
}

// Get number of embedded fonts
EMSCRIPTEN_KEEPALIVE
int nanopdf_fonts_get_count() {
  return static_cast<int>(nanopdf::embedded_fonts::font_count);
}

// Get list of all embedded fonts as JSON
// Returns: {"fonts":[{"name":"Arimo-Regular","filename":"Arimo-Regular.ttf","originalSize":123,"compressedSize":456,"compressionRatio":0.409},...]}
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_fonts_list() {
  std::string json = "{\"fonts\":[";

  for (size_t i = 0; i < nanopdf::embedded_fonts::font_count; ++i) {
    const auto& font = nanopdf::embedded_fonts::font_registry[i];

    if (i > 0) json += ",";

    float ratio = 1.0f - static_cast<float>(font.compressed_size) / static_cast<float>(font.original_size);

    json += "{";
    json += "\"name\":\"" + json_escape(font.base_name) + "\",";
    json += "\"filename\":\"" + json_escape(font.filename) + "\",";
    json += "\"originalSize\":" + std::to_string(font.original_size) + ",";
    json += "\"compressedSize\":" + std::to_string(font.compressed_size) + ",";
    json += "\"compressionRatio\":" + std::to_string(ratio);
    json += "}";
  }

  json += "],\"count\":" + std::to_string(nanopdf::embedded_fonts::font_count) + "}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Get PDF Standard 14 font mapping as JSON
// Returns: {"mapping":[{"pdfName":"Helvetica","substituteName":"Arimo-Regular"},...]}
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_fonts_get_pdf_mapping() {
  std::string json = "{\"mapping\":[";

  for (size_t i = 0; i < nanopdf::embedded_fonts::pdf_mapping_count; ++i) {
    const auto& mapping = nanopdf::embedded_fonts::pdf_standard_14_mapping[i];

    if (i > 0) json += ",";

    json += "{";
    json += "\"pdfName\":\"" + json_escape(mapping.pdf_name) + "\",";
    json += "\"substituteName\":\"" + json_escape(mapping.substitute_name) + "\"";
    json += "}";
  }

  json += "],\"count\":" + std::to_string(nanopdf::embedded_fonts::pdf_mapping_count) + "}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Load a font by base name (e.g., "Arimo-Regular")
// Returns pointer to decompressed font data (valid until next font load)
// Returns nullptr on error
EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_fonts_load(const char* name) {
  if (!name) {
    g_last_error = "Font name is null";
    return nullptr;
  }

  const auto* font = nanopdf::embedded_fonts::find_font(name);
  if (!font) {
    g_last_error = std::string("Font not found: ") + name;
    return nullptr;
  }

  // Decompress font
  g_font_buffer.clear();
  if (!nanopdf::embedded_fonts::decompress_font(font, g_font_buffer)) {
    g_last_error = std::string("Failed to decompress font: ") + name;
    return nullptr;
  }

  return g_font_buffer.data();
}

// Load a PDF Standard 14 font (e.g., "Helvetica", "Times-Bold")
// Returns pointer to decompressed font data (valid until next font load)
// Returns nullptr on error
EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_fonts_load_pdf_standard(const char* pdf_name) {
  if (!pdf_name) {
    g_last_error = "PDF font name is null";
    return nullptr;
  }

  const auto* font = nanopdf::embedded_fonts::get_pdf_standard_font(pdf_name);
  if (!font) {
    g_last_error = std::string("PDF Standard 14 font not found: ") + pdf_name;
    return nullptr;
  }

  // Decompress font
  g_font_buffer.clear();
  if (!nanopdf::embedded_fonts::decompress_font(font, g_font_buffer)) {
    g_last_error = std::string("Failed to decompress font: ") + pdf_name;
    return nullptr;
  }

  return g_font_buffer.data();
}

// Get size of currently loaded font data
EMSCRIPTEN_KEEPALIVE
size_t nanopdf_fonts_get_loaded_size() {
  return g_font_buffer.size();
}

// Get font info by name as JSON
// Returns: {"found":true,"name":"...","filename":"...","originalSize":123,"compressedSize":456}
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_fonts_get_info(const char* name) {
  if (!name) {
    g_text_buffer = "{\"found\":false,\"error\":\"Font name is null\"}";
    return g_text_buffer.c_str();
  }

  const auto* font = nanopdf::embedded_fonts::find_font(name);
  if (!font) {
    g_text_buffer = "{\"found\":false,\"error\":\"Font not found\"}";
    return g_text_buffer.c_str();
  }

  float ratio = 1.0f - static_cast<float>(font->compressed_size) / static_cast<float>(font->original_size);

  std::string json = "{\"found\":true";
  json += ",\"name\":\"" + json_escape(font->base_name) + "\"";
  json += ",\"filename\":\"" + json_escape(font->filename) + "\"";
  json += ",\"originalSize\":" + std::to_string(font->original_size);
  json += ",\"compressedSize\":" + std::to_string(font->compressed_size);
  json += ",\"compressionRatio\":" + std::to_string(ratio);
  json += "}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Register ALL embedded fonts with FontProvider at init time.
// This eagerly decompresses every font so they're available for rendering.
// Returns the number of fonts successfully registered.
EMSCRIPTEN_KEEPALIVE
// Helper: detect weight and italic from font base name
// e.g. "Arimo-Bold" → weight=700, italic=false
//      "Tinos-BoldItalic" → weight=700, italic=true
//      "Cousine-Italic" → weight=400, italic=true
//      "Arimo-Regular" → weight=400, italic=false
static void detect_weight_italic(const char* name, int& weight, bool& italic) {
  weight = 400;
  italic = false;
  std::string s(name);
  // Check for Bold
  if (s.find("Bold") != std::string::npos) weight = 700;
  // Check for Light
  if (s.find("Light") != std::string::npos) weight = 300;
  // Check for Thin
  if (s.find("Thin") != std::string::npos) weight = 100;
  // Check for Medium
  if (s.find("Medium") != std::string::npos) weight = 500;
  // Check for SemiBold/DemiBold
  if (s.find("SemiBold") != std::string::npos ||
      s.find("DemiBold") != std::string::npos ||
      s.find("DemiLight") != std::string::npos) weight = 350;
  if (s.find("SemiBold") != std::string::npos) weight = 600;
  // Check for Black/Heavy
  if (s.find("Black") != std::string::npos ||
      s.find("Heavy") != std::string::npos) weight = 900;
  // Check for ExtraLight
  if (s.find("ExtraLight") != std::string::npos) weight = 200;
  // Check for Italic/Oblique
  if (s.find("Italic") != std::string::npos ||
      s.find("Oblique") != std::string::npos) italic = true;
}

int nanopdf_register_embedded_fonts() {
  int registered = 0;

  for (size_t i = 0; i < nanopdf::embedded_fonts::font_count; ++i) {
    const auto& entry = nanopdf::embedded_fonts::font_registry[i];

    // Determine FontCategory from font name prefix
    nanopdf::FontCategory cat;
    const char* name = entry.base_name;
    if (strncmp(name, "Arimo", 5) == 0) {
      cat = nanopdf::FontCategory::kSans;
    } else if (strncmp(name, "Cousine", 7) == 0) {
      cat = nanopdf::FontCategory::kMono;
    } else if (strncmp(name, "Tinos", 5) == 0) {
      cat = nanopdf::FontCategory::kSerif;
    } else if (strncmp(name, "NotoSans", 8) == 0) {
      cat = nanopdf::FontCategory::kSymbol;
    } else {
      cat = nanopdf::FontCategory::kSans;  // fallback
    }

    // Detect weight and italic from font name
    int weight;
    bool italic;
    detect_weight_italic(name, weight, italic);

    // Decompress font data
    std::vector<uint8_t> font_data;
    if (!nanopdf::embedded_fonts::decompress_font(&entry, font_data)) {
      continue;
    }

    // Register with FontProvider (including weight/italic)
    if (nanopdf::FontProvider::instance().register_font_blob(
            entry.base_name, cat, weight, italic,
            font_data.data(), font_data.size())) {
      ++registered;
    }
  }

  return registered;
}

#else  // !NANOPDF_EMBED_FONTS

// Stub functions when fonts are not embedded
EMSCRIPTEN_KEEPALIVE
int nanopdf_fonts_available() {
  return 0;
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_fonts_get_count() {
  return 0;
}

EMSCRIPTEN_KEEPALIVE
const char* nanopdf_fonts_list() {
  g_text_buffer = "{\"error\":\"Embedded fonts not available\",\"fonts\":[]}";
  return g_text_buffer.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* nanopdf_fonts_get_pdf_mapping() {
  g_text_buffer = "{\"error\":\"Embedded fonts not available\",\"mapping\":[]}";
  return g_text_buffer.c_str();
}

EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_fonts_load(const char* name) {
  (void)name;
  g_last_error = "Embedded fonts not available in this build";
  return nullptr;
}

EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_fonts_load_pdf_standard(const char* pdf_name) {
  (void)pdf_name;
  g_last_error = "Embedded fonts not available in this build";
  return nullptr;
}

EMSCRIPTEN_KEEPALIVE
size_t nanopdf_fonts_get_loaded_size() {
  return 0;
}

EMSCRIPTEN_KEEPALIVE
const char* nanopdf_fonts_get_info(const char* name) {
  (void)name;
  g_text_buffer = "{\"found\":false,\"error\":\"Embedded fonts not available\"}";
  return g_text_buffer.c_str();
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_register_embedded_fonts() {
  return 0;
}

#endif  // NANOPDF_EMBED_FONTS

// ============================================================
// Generic Font Registration API (always available)
// ============================================================

// Helper: convert category int to FontCategory enum
static nanopdf::FontCategory category_from_int(int category) {
  switch (category) {
    case 0: return nanopdf::FontCategory::kSans;
    case 1: return nanopdf::FontCategory::kMono;
    case 2: return nanopdf::FontCategory::kSerif;
    case 3: return nanopdf::FontCategory::kSymbol;
    case 4: return nanopdf::FontCategory::kCJKSans;
    case 5: return nanopdf::FontCategory::kCJKSerif;
    default: return nanopdf::FontCategory::kSans;
  }
}

// Helper: generate a unique runtime font name
static std::string make_runtime_font_name(int category, int weight, bool italic) {
  static const char* cat_prefixes[] = {
    "Sans", "Mono", "Serif", "Symbol", "CJKSans", "CJKSerif"
  };
  int idx = (category >= 0 && category <= 5) ? category : 0;
  std::string name = std::string(cat_prefixes[idx]) + "-w" + std::to_string(weight);
  if (italic) name += "-italic";
  return name;
}

// Register any font from a memory blob (legacy, weight=400, italic=false)
// category: 0=sans, 1=mono, 2=serif, 3=symbol, 4=cjk_sans, 5=cjk_serif
EMSCRIPTEN_KEEPALIVE
int nanopdf_register_font(const uint8_t* data, size_t size, int category) {
  if (!data || size == 0) return 0;
  nanopdf::FontCategory cat = category_from_int(category);
  std::string name = make_runtime_font_name(category, 400, false);
  return nanopdf::FontProvider::instance().register_font_blob(
      name, cat, 400, false, data, size) ? 1 : 0;
}

// Register a font with explicit weight and italic
// category: 0=sans, 1=mono, 2=serif, 3=symbol, 4=cjk_sans, 5=cjk_serif
// weight: CSS font-weight 100-900 (default 400)
// is_italic: 0=normal, 1=italic
EMSCRIPTEN_KEEPALIVE
int nanopdf_register_font_ex(const uint8_t* data, size_t size,
                              int category, int weight, int is_italic) {
  if (!data || size == 0) return 0;
  nanopdf::FontCategory cat = category_from_int(category);
  bool italic = (is_italic != 0);
  std::string name = make_runtime_font_name(category, weight, italic);
  return nanopdf::FontProvider::instance().register_font_blob(
      name, cat, weight, italic, data, size) ? 1 : 0;
}

// ============================================================
// CJK Font Registration API (always available, legacy wrapper)
// ============================================================

// Register a CJK font from a memory blob
// category: 4 = CJK sans, 5 = CJK serif
EMSCRIPTEN_KEEPALIVE
int nanopdf_register_cjk_font(const uint8_t* data, size_t size, int category) {
  if (!data || size == 0) return 0;
  nanopdf::FontCategory cat;
  if (category == 5) {
    cat = nanopdf::FontCategory::kCJKSerif;
  } else {
    cat = nanopdf::FontCategory::kCJKSans;
  }
  const char* name = (category == 5) ? "CJKSerif-Runtime" : "CJKSans-Runtime";
  return nanopdf::FontProvider::instance().register_font_blob(name, cat, data, size) ? 1 : 0;
}

// Check if CJK fonts are available (registered via FontProvider or embedded)
EMSCRIPTEN_KEEPALIVE
int nanopdf_cjk_fonts_ready() {
  if (nanopdf::FontProvider::instance().has_cjk_fonts()) {
    return 1;
  }
#ifdef NANOPDF_EMBED_CJK_FONTS
  return 1;
#else
  return 0;
#endif
}

#ifdef NANOPDF_EMBED_CJK_FONTS

// Check if embedded CJK fonts are compiled in
EMSCRIPTEN_KEEPALIVE
int nanopdf_cjk_embedded_fonts_available() {
  return 1;
}

// Get list of embedded CJK fonts as JSON
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_cjk_fonts_list() {
  std::string json = "{\"fonts\":[";

  for (size_t i = 0; i < nanopdf::embedded_cjk_fonts::font_count; ++i) {
    const auto& font = nanopdf::embedded_cjk_fonts::font_registry[i];
    if (i > 0) json += ",";
    json += "{\"name\":\"" + std::string(font.base_name) + "\"";
    json += ",\"filename\":\"" + std::string(font.filename) + "\"";
    json += ",\"originalSize\":" + std::to_string(font.original_size);
    json += ",\"compressedSize\":" + std::to_string(font.compressed_size);
    json += "}";
  }

  json += "],\"count\":" + std::to_string(nanopdf::embedded_cjk_fonts::font_count) + "}";
  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Load an embedded CJK font by name, returns pointer to decompressed data
EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_cjk_fonts_load(const char* name) {
  if (!name) return nullptr;
  const auto* font = nanopdf::embedded_cjk_fonts::find_font(name);
  if (!font) {
    g_last_error = std::string("CJK font not found: ") + name;
    return nullptr;
  }
  g_font_buffer.clear();
  if (!nanopdf::embedded_cjk_fonts::decompress_font(font, g_font_buffer)) {
    g_last_error = std::string("Failed to decompress CJK font: ") + name;
    return nullptr;
  }
  return g_font_buffer.data();
}

#else  // !NANOPDF_EMBED_CJK_FONTS

EMSCRIPTEN_KEEPALIVE
int nanopdf_cjk_embedded_fonts_available() {
  return 0;
}

EMSCRIPTEN_KEEPALIVE
const char* nanopdf_cjk_fonts_list() {
  g_text_buffer = "{\"fonts\":[],\"count\":0}";
  return g_text_buffer.c_str();
}

EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_cjk_fonts_load(const char* /*name*/) {
  g_last_error = "CJK fonts not embedded in this build";
  return nullptr;
}

#endif  // NANOPDF_EMBED_CJK_FONTS

// ============================================================
// Form Fill API (for existing PDFs)
// ============================================================

// Merge output buffer for form save / incremental writes
static std::vector<uint8_t> g_form_output;

EMSCRIPTEN_KEEPALIVE
int nanopdf_form_set_text(const char* field_name, const char* value) {
  if (!g_writer || !field_name || !value) {
    g_last_error = "Writer not initialized or invalid arguments";
    return 0;
  }
  return g_writer->set_field_value(field_name, value) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_form_set_checkbox(const char* field_name, int checked) {
  if (!g_writer || !field_name) {
    g_last_error = "Writer not initialized or invalid arguments";
    return 0;
  }
  return g_writer->set_field_checked(field_name, checked != 0) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_form_set_choice(const char* field_name, const char* value) {
  if (!g_writer || !field_name || !value) {
    g_last_error = "Writer not initialized or invalid arguments";
    return 0;
  }
  return g_writer->set_field_choice(field_name, value) ? 1 : 0;
}

// Load existing PDF into writer for form editing
EMSCRIPTEN_KEEPALIVE
int nanopdf_form_load(const uint8_t* data, size_t size) {
  if (!data || size == 0) {
    g_last_error = "Invalid input data";
    return 0;
  }

  if (g_writer) {
    delete g_writer;
  }
  g_writer = new nanopdf::PdfWriter();

  std::vector<uint8_t> pdf_data(data, data + size);
  std::string error;
  if (!g_writer->load_existing(pdf_data, &error)) {
    g_last_error = "Failed to load PDF for editing: " + error;
    delete g_writer;
    g_writer = nullptr;
    return 0;
  }
  return 1;
}

// Save form edits via incremental update
EMSCRIPTEN_KEEPALIVE
int nanopdf_form_save() {
  if (!g_writer) {
    g_last_error = "Writer not initialized";
    return 0;
  }

  g_form_output.clear();
  auto result = g_writer->write_incremental(g_form_output);
  if (!result.success) {
    g_last_error = "Form save failed: " + result.error;
    return 0;
  }
  return 1;
}

EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_form_get_buffer() {
  if (g_form_output.empty()) return nullptr;
  return g_form_output.data();
}

EMSCRIPTEN_KEEPALIVE
size_t nanopdf_form_get_size() {
  return g_form_output.size();
}

// ============================================================
// PDF Merge / Split API
// ============================================================

static std::vector<std::vector<uint8_t>> g_merge_inputs;
static std::vector<uint8_t> g_merge_output;

EMSCRIPTEN_KEEPALIVE
void nanopdf_merge_start() {
  g_merge_inputs.clear();
  g_merge_output.clear();
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_merge_add_pdf(const uint8_t* data, size_t size) {
  if (!data || size == 0) {
    g_last_error = "Invalid PDF data for merge";
    return 0;
  }
  g_merge_inputs.emplace_back(data, data + size);
  return static_cast<int>(g_merge_inputs.size());
}

EMSCRIPTEN_KEEPALIVE
int nanopdf_merge_finish() {
  if (g_merge_inputs.empty()) {
    g_last_error = "No PDFs added for merge";
    return 0;
  }

  g_merge_output.clear();
  auto result = nanopdf::PdfWriter::merge_pdfs(g_merge_inputs, g_merge_output);
  g_merge_inputs.clear();

  if (!result.success) {
    g_last_error = "Merge failed: " + result.error;
    return 0;
  }
  return 1;
}

EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_merge_get_buffer() {
  if (g_merge_output.empty()) return nullptr;
  return g_merge_output.data();
}

EMSCRIPTEN_KEEPALIVE
size_t nanopdf_merge_get_size() {
  return g_merge_output.size();
}

// Split pages from a loaded PDF into a new PDF
// page_indices_json: JSON array of 0-based page indices, e.g. "[0,2,5]"
EMSCRIPTEN_KEEPALIVE
int nanopdf_split_pages(const char* page_indices_json) {
  if (!g_pdf || !page_indices_json) {
    g_last_error = "No PDF loaded or invalid arguments";
    return 0;
  }

  // Simple JSON array parser for integers: [1,2,3]
  std::vector<int> indices;
  std::string json(page_indices_json);
  size_t pos = json.find('[');
  if (pos == std::string::npos) {
    g_last_error = "Invalid JSON array format";
    return 0;
  }
  pos++;
  while (pos < json.size() && json[pos] != ']') {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',')) pos++;
    if (pos < json.size() && json[pos] != ']') {
      int idx = 0;
      bool negative = false;
      if (json[pos] == '-') { negative = true; pos++; }
      while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        idx = idx * 10 + (json[pos] - '0');
        pos++;
      }
      if (negative) idx = -idx;
      indices.push_back(idx);
    }
  }

  if (indices.empty()) {
    g_last_error = "No page indices specified";
    return 0;
  }

  g_merge_output.clear();  // Reuse merge output buffer
  auto result = nanopdf::PdfWriter::split_pages(*g_pdf, indices, g_merge_output);
  if (!result.success) {
    g_last_error = "Split failed: " + result.error;
    return 0;
  }
  return 1;
}

// ============================================================
// Signature Validation API
// ============================================================

// Get all signature fields as JSON
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_get_signatures() {
  if (!g_pdf) {
    g_text_buffer = "{\"error\":\"No PDF loaded\"}";
    return g_text_buffer.c_str();
  }

  // Parse signature fields if not already done
  if (g_pdf->catalog.signature_fields.empty()) {
    g_pdf->parse_signature_fields();
  }

  std::string json = "{\"signatures\":[";
  for (size_t i = 0; i < g_pdf->catalog.signature_fields.size(); ++i) {
    const auto& sig = g_pdf->catalog.signature_fields[i];
    if (i > 0) json += ",";
    json += "{";
    json += "\"name\":\"" + json_escape(sig.name) + "\"";
    json += ",\"signed\":" + std::string(sig.is_signed ? "true" : "false");
    json += ",\"signaturePresent\":" + std::string(sig.signature_present ? "true" : "false");
    json += ",\"byteRangeValid\":" + std::string(sig.byte_range_valid ? "true" : "false");
    json += ",\"contentsLength\":" + std::to_string(sig.signature_contents.size());
    if (!sig.byte_range.empty()) {
      json += ",\"byteRange\":[";
      for (size_t j = 0; j < sig.byte_range.size(); ++j) {
        if (j > 0) json += ",";
        json += std::to_string(sig.byte_range[j]);
      }
      json += "]";
    }
    if (!sig.digest_algorithm.empty())
      json += ",\"digestAlgorithm\":\"" + json_escape(sig.digest_algorithm) + "\"";
    if (!sig.signing_reason.empty())
      json += ",\"reason\":\"" + json_escape(sig.signing_reason) + "\"";
    if (!sig.signing_location.empty())
      json += ",\"location\":\"" + json_escape(sig.signing_location) + "\"";
    if (!sig.signing_date.empty())
      json += ",\"date\":\"" + json_escape(sig.signing_date) + "\"";
    if (!sig.signing_contact_info.empty())
      json += ",\"contact\":\"" + json_escape(sig.signing_contact_info) + "\"";
    if (!sig.filter.empty())
      json += ",\"filter\":\"" + json_escape(sig.filter) + "\"";
    if (!sig.subfilter.empty())
      json += ",\"subFilter\":\"" + json_escape(sig.subfilter) + "\"";
    json += ",\"isCertification\":" + std::string(sig.is_certification_signature ? "true" : "false");
    json += ",\"mdpPermissions\":" + std::to_string(sig.mdp_permissions);
    if (!sig.transform_method.empty())
      json += ",\"transformMethod\":\"" + json_escape(sig.transform_method) + "\"";
    if (!sig.locked_fields.empty()) {
      json += ",\"lockedFields\":[";
      for (size_t j = 0; j < sig.locked_fields.size(); ++j) {
        if (j > 0) json += ",";
        json += "\"" + json_escape(sig.locked_fields[j]) + "\"";
      }
      json += "]";
    }
    json += ",\"hasTimestamp\":" + std::string(sig.has_timestamp ? "true" : "false");
    json += ",\"isDocumentTimestamp\":" + std::string(sig.is_document_timestamp ? "true" : "false");
    if (sig.has_timestamp)
      json += ",\"timestampType\":\"" + std::string(sig.is_document_timestamp ? "document_timestamp" : "embedded") + "\"";
    if (sig.has_timestamp && !sig.timestamp_date.empty())
      json += ",\"timestampDate\":\"" + json_escape(sig.timestamp_date) + "\"";
    if (sig.has_timestamp && !sig.timestamp_authority.empty())
      json += ",\"timestampAuthority\":\"" + json_escape(sig.timestamp_authority) + "\"";
    if (sig.has_timestamp && !sig.timestamp_hash_algorithm.empty())
      json += ",\"timestampHashAlgorithm\":\"" + json_escape(sig.timestamp_hash_algorithm) + "\"";
    if (sig.has_timestamp && !sig.timestamp_token.empty())
      json += ",\"timestampTokenLength\":" + std::to_string(sig.timestamp_token.size());
    if (sig.rect.size() >= 4) {
      json += ",\"rect\":[" + std::to_string(sig.rect[0]) + ","
              + std::to_string(sig.rect[1]) + ","
              + std::to_string(sig.rect[2]) + ","
              + std::to_string(sig.rect[3]) + "]";
    }
    json += "}";
  }
  json += "],\"count\":" + std::to_string(g_pdf->catalog.signature_fields.size()) + "}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Validate a specific signature by index
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_validate_signature(int sig_index) {
  if (!g_pdf) {
    g_text_buffer = "{\"error\":\"No PDF loaded\"}";
    return g_text_buffer.c_str();
  }

  if (g_pdf->catalog.signature_fields.empty()) {
    g_pdf->parse_signature_fields();
  }

  if (sig_index < 0 || sig_index >= static_cast<int>(g_pdf->catalog.signature_fields.size())) {
    g_text_buffer = "{\"error\":\"Invalid signature index\"}";
    return g_text_buffer.c_str();
  }

  const auto& sig = g_pdf->catalog.signature_fields[sig_index];
  auto result = nanopdf::validate_signature(*g_pdf, sig);

  std::string json = "{";
  json += "\"success\":" + std::string(result.success ? "true" : "false");
  json += ",\"integrityValid\":" + std::string(result.integrity_valid ? "true" : "false");
  json += ",\"signatureValid\":" + std::string(result.signature_valid ? "true" : "false");
  if (!result.signer_name.empty())
    json += ",\"signerName\":\"" + json_escape(result.signer_name) + "\"";
  if (!result.signing_time.empty())
    json += ",\"signingTime\":\"" + json_escape(result.signing_time) + "\"";
  if (!result.digest_algorithm.empty())
    json += ",\"digestAlgorithm\":\"" + json_escape(result.digest_algorithm) + "\"";
  if (!result.error.empty())
    json += ",\"error\":\"" + json_escape(result.error) + "\"";
  json += "}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// Get document revision history and DocMDP analysis as JSON.
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_get_revision_history() {
  if (!g_pdf) {
    g_text_buffer = "{\"error\":\"No PDF loaded\"}";
    return g_text_buffer.c_str();
  }

  nanopdf::RevisionHistory history = nanopdf::detect_revision_history(*g_pdf);

  auto append_object_array = [](std::string& json,
                                const char* name,
                                const std::vector<uint32_t>& objects) {
    json += ",\"" + std::string(name) + "\":[";
    for (size_t i = 0; i < objects.size(); ++i) {
      if (i > 0) json += ",";
      json += std::to_string(objects[i]);
    }
    json += "]";
  };

  std::string json = "{";
  json += "\"count\":" + std::to_string(history.revisions.size());
  if (!history.current_md5.empty())
    json += ",\"currentMd5\":\"" + json_escape(history.current_md5) + "\"";
  if (!history.current_sha256.empty())
    json += ",\"currentSha256\":\"" + json_escape(history.current_sha256) + "\"";

  json += ",\"revisions\":[";
  for (size_t i = 0; i < history.revisions.size(); ++i) {
    const auto& rev = history.revisions[i];
    if (i > 0) json += ",";
    json += "{";
    json += "\"revision\":" + std::to_string(rev.revision_number);
    json += ",\"startOffset\":" + std::to_string(rev.start_offset);
    json += ",\"endOffset\":" + std::to_string(rev.end_offset);
    json += ",\"sizeBytes\":" + std::to_string(rev.size_bytes);
    json += ",\"xrefOffset\":" + std::to_string(rev.xref_offset);
    json += ",\"prevXrefOffset\":" + std::to_string(rev.prev_xref_offset);
    if (!rev.md5_hash.empty())
      json += ",\"md5\":\"" + json_escape(rev.md5_hash) + "\"";
    if (!rev.sha256_hash.empty())
      json += ",\"sha256\":\"" + json_escape(rev.sha256_hash) + "\"";
    append_object_array(json, "addedObjects", rev.added_objects);
    append_object_array(json, "modifiedObjects", rev.modified_objects);
    append_object_array(json, "deletedObjects", rev.deleted_objects);
    if (!rev.associated_signature.empty())
      json += ",\"associatedSignature\":\"" + json_escape(rev.associated_signature) + "\"";
    if (!rev.signer_name.empty())
      json += ",\"signerName\":\"" + json_escape(rev.signer_name) + "\"";
    if (!rev.signing_time.empty())
      json += ",\"signingTime\":\"" + json_escape(rev.signing_time) + "\"";
    json += ",\"modifiedAfterSignature\":" + std::string(rev.modified_after_signature ? "true" : "false");
    json += ",\"hasDocMDP\":" + std::string(rev.has_docmdp ? "true" : "false");
    if (rev.has_docmdp) {
      json += ",\"mdpPermissions\":" + std::to_string(rev.mdp_permissions);
      json += ",\"docMDPAllowed\":" + std::string(rev.docmdp_allowed ? "true" : "false");
      json += ",\"docMDPStatus\":\"" + json_escape(rev.docmdp_status) + "\"";
      json += ",\"docMDPViolations\":[";
      for (size_t j = 0; j < rev.docmdp_violations.size(); ++j) {
        if (j > 0) json += ",";
        json += "\"" + json_escape(rev.docmdp_violations[j]) + "\"";
      }
      json += "]";
    }
    json += "}";
  }
  json += "]}";

  g_text_buffer = json;
  return g_text_buffer.c_str();
}

// ============================================================
// Markdown Conversion API
// ============================================================

EMSCRIPTEN_KEEPALIVE
const char* nanopdf_page_to_markdown(int page_index) {
  if (!g_pdf || page_index < 0 ||
      page_index >= static_cast<int>(g_pdf->catalog.pages.size())) {
    g_text_buffer = "";
    return g_text_buffer.c_str();
  }

  const auto& page = g_pdf->catalog.pages[page_index];
  auto text_page = nanopdf::extract_text_layout(*g_pdf, page);

  if (!text_page) {
    g_text_buffer = "";
    return g_text_buffer.c_str();
  }

  g_text_buffer = text_page->to_markdown();
  return g_text_buffer.c_str();
}

// ============================================================
// Memory Management
// ============================================================

EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_malloc(size_t size) {
  return static_cast<uint8_t*>(malloc(size));
}

EMSCRIPTEN_KEEPALIVE
void nanopdf_free(void* ptr) {
  free(ptr);
}

}  // extern "C"

#endif  // __EMSCRIPTEN__
