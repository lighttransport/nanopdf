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

#ifdef NANOPDF_USE_BLEND2D
#include "blend2d-backend.hh"
#endif

#ifdef NANOPDF_USE_THORVG
#include "thorvg-backend.hh"
#endif

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

// Text buffer
static std::string g_text_buffer;

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
  nanopdf::Parser parser;
  if (!parser.parse(g_pdf_data.data(), g_pdf_data.size(), g_pdf)) {
    g_last_error = "Failed to parse PDF";
    delete g_pdf;
    g_pdf = nullptr;
    return 0;
  }

  if (!nanopdf::parse_document_catalog(*g_pdf)) {
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
#if defined(NANOPDF_USE_BLEND2D) || defined(NANOPDF_USE_THORVG)
  return 1;
#else
  return 0;
#endif
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

#ifdef NANOPDF_USE_BLEND2D
  nanopdf::Blend2DBackend backend;
  if (!backend.initialize(static_cast<uint32_t>(width),
                          static_cast<uint32_t>(height))) {
    g_last_error = "Failed to initialize Blend2D backend";
    return 0;
  }

  nanopdf::RenderOptions options;
  options.dpi = dpi > 0 ? dpi : 72.0f;

  auto result = backend.render_page(*g_pdf, page, options);
  if (!result.success) {
    g_last_error = result.error;
    return 0;
  }

  g_render_buffer = std::move(result.pixels);
  g_render_width = result.width;
  g_render_height = result.height;
  return 1;

#elif defined(NANOPDF_USE_THORVG)
  nanopdf::ThorVGBackend backend;
  if (!backend.initialize(static_cast<uint32_t>(width),
                          static_cast<uint32_t>(height))) {
    g_last_error = "Failed to initialize ThorVG backend";
    return 0;
  }

  nanopdf::ThorVGRenderOptions options;
  options.dpi = dpi > 0 ? dpi : 72.0f;

  auto result = backend.render_page(*g_pdf, page, options);
  if (!result.success) {
    g_last_error = result.error;
    return 0;
  }

  g_render_buffer = std::move(result.pixels);
  g_render_width = result.width;
  g_render_height = result.height;
  return 1;

#else
  (void)page;
  (void)dpi;
  g_last_error = "No rendering backend available";
  return 0;
#endif
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
  g_text_buffer = nanopdf::extract_text(*g_pdf, page);
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

  nanopdf::Parser parser;
  if (!parser.parse(doc.data.data(), doc.data.size(), doc.pdf)) {
    g_last_error = "Failed to parse PDF";
    delete doc.pdf;
    return -1;
  }

  if (!nanopdf::parse_document_catalog(*doc.pdf)) {
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

#ifdef NANOPDF_USE_THORVG
  nanopdf::ThorVGBackend backend;
  if (!backend.initialize(static_cast<uint32_t>(width),
                          static_cast<uint32_t>(height))) {
    g_last_error = "Failed to initialize ThorVG backend";
    return 0;
  }

  nanopdf::ThorVGRenderOptions options;
  options.dpi = dpi > 0 ? dpi : 72.0f;

  auto result = backend.render_page(*it->second.pdf, page, options);
  if (!result.success) {
    g_last_error = result.error;
    return 0;
  }

  g_render_buffer = std::move(result.pixels);
  g_render_width = result.width;
  g_render_height = result.height;
  return 1;
#else
  (void)page;
  (void)dpi;
  g_last_error = "No rendering backend available";
  return 0;
#endif
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

static std::vector<uint8_t> g_output_buffer;

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

  std::string std_font = g_writer->add_standard_font(nanopdf::StandardFont::Helvetica);

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

    g_writer->add_page(size, [&pinfo, &std_font, annots, page_w, page_h](nanopdf::PageBuilder& builder) {
      // Draw background (white)
      builder.save_state();
      builder.set_fill_color(1.0, 1.0, 1.0);
      builder.rectangle(0, 0, page_w, page_h);
      builder.fill();
      builder.restore_state();

      // TODO: If we have source PDF content, we'd render it here
      // For now we just create blank pages with annotations

      // Draw placeholder text if no source
      if (pinfo.source_doc_id >= 0) {
        builder.begin_text();
        builder.set_font(std_font, 12);
        builder.show_text_at(10, page_h - 20, "[Page content from source PDF]");
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
