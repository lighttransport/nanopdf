// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// WebAssembly API for nanopdf rendering
// This file provides C-compatible functions that can be called from JavaScript

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

#include "nanopdf.hh"

#ifdef NANOPDF_USE_BLEND2D
#include "blend2d-backend.hh"
#endif

#ifdef NANOPDF_USE_THORVG
#include "thorvg-backend.hh"
#endif

// Global state for parsed PDF
static nanopdf::Pdf* g_pdf = nullptr;
static std::vector<uint8_t> g_pdf_data;
static std::string g_last_error;

// Rendering buffer
static std::vector<uint8_t> g_render_buffer;
static uint32_t g_render_width = 0;
static uint32_t g_render_height = 0;

extern "C" {

// Initialize the WASM module
EMSCRIPTEN_KEEPALIVE
int nanopdf_init() {
#ifdef NANOPDF_USE_THORVG
  // Initialize ThorVG
  tvg::Initializer::init(0);
#endif
  return 1;  // Success
}

// Shutdown the WASM module
EMSCRIPTEN_KEEPALIVE
void nanopdf_shutdown() {
  if (g_pdf) {
    delete g_pdf;
    g_pdf = nullptr;
  }
  g_pdf_data.clear();
  g_render_buffer.clear();
#ifdef NANOPDF_USE_THORVG
  tvg::Initializer::term();
#endif
}

// Load a PDF from memory
// Returns number of pages on success, 0 on failure
EMSCRIPTEN_KEEPALIVE
int nanopdf_load_pdf(const uint8_t* data, size_t size) {
  if (!data || size == 0) {
    g_last_error = "Invalid input data";
    return 0;
  }

  // Clean up previous PDF
  if (g_pdf) {
    delete g_pdf;
    g_pdf = nullptr;
  }

  // Copy data
  g_pdf_data.assign(data, data + size);

  // Parse PDF
  g_pdf = new nanopdf::Pdf();
  nanopdf::Parser parser;
  if (!parser.parse(g_pdf_data.data(), g_pdf_data.size(), g_pdf)) {
    g_last_error = "Failed to parse PDF";
    delete g_pdf;
    g_pdf = nullptr;
    return 0;
  }

  // Parse document structure
  if (!nanopdf::parse_document_catalog(*g_pdf)) {
    g_last_error = "Failed to parse document catalog";
    delete g_pdf;
    g_pdf = nullptr;
    return 0;
  }

  return static_cast<int>(g_pdf->catalog.pages.size());
}

// Get the number of pages in the loaded PDF
EMSCRIPTEN_KEEPALIVE
int nanopdf_get_page_count() {
  if (!g_pdf) return 0;
  return static_cast<int>(g_pdf->catalog.pages.size());
}

// Get page width in points (1/72 inch)
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
  return 612.0f;  // Default letter size width
}

// Get page height in points (1/72 inch)
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
  return 792.0f;  // Default letter size height
}

// Check if rendering is available
EMSCRIPTEN_KEEPALIVE
int nanopdf_has_rendering() {
#if defined(NANOPDF_USE_BLEND2D) || defined(NANOPDF_USE_THORVG)
  return 1;
#else
  return 0;
#endif
}

// Render a page to RGBA8888 buffer
// Returns 1 on success, 0 on failure
// The rendered buffer can be retrieved with nanopdf_get_render_buffer()
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
  (void)page;  // Suppress unused warning
  (void)dpi;
  g_last_error = "No rendering backend available. Build with NANOPDF_USE_THORVG=ON or NANOPDF_USE_BLEND2D=ON";
  return 0;
#endif
}

// Get pointer to the render buffer (RGBA8888)
EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_get_render_buffer() {
  if (g_render_buffer.empty()) return nullptr;
  return g_render_buffer.data();
}

// Get render buffer size in bytes
EMSCRIPTEN_KEEPALIVE
size_t nanopdf_get_render_buffer_size() {
  return g_render_buffer.size();
}

// Get rendered width
EMSCRIPTEN_KEEPALIVE
uint32_t nanopdf_get_render_width() {
  return g_render_width;
}

// Get rendered height
EMSCRIPTEN_KEEPALIVE
uint32_t nanopdf_get_render_height() {
  return g_render_height;
}

// Extract text from a page
// Returns pointer to null-terminated string (valid until next call)
static std::string g_text_buffer;

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

// Get the last error message
EMSCRIPTEN_KEEPALIVE
const char* nanopdf_get_last_error() {
  return g_last_error.c_str();
}

// Allocate memory (for passing data from JavaScript)
EMSCRIPTEN_KEEPALIVE
uint8_t* nanopdf_malloc(size_t size) {
  return static_cast<uint8_t*>(malloc(size));
}

// Free memory
EMSCRIPTEN_KEEPALIVE
void nanopdf_free(void* ptr) {
  free(ptr);
}

}  // extern "C"

#endif  // __EMSCRIPTEN__
