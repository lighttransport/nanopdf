// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// pdf_document — thin wrapper over nanopdf parsing + page rasterization for the
// native viewer. Renders a page (at a pixel size derived from page points x
// scale) into an lvg-ready ARGB32 surface, with a small per-(page,scale) cache.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <lightvg/surface.h>
}

#include "nanopdf.hh"
#include "render-backend.hh"
#include "text-layout.hh"

namespace pdfview {

// Decode a PDF text string (UTF-16BE with BOM, else PDFDocEncoding ~ Latin-1)
// to UTF-8 for display (bookmark titles, document metadata).
std::string decode_pdf_text(const std::string& s);

// Register nanopdf's bundled substitute (Arimo/Tinos/Cousine/STIX) and CJK
// (Noto Sans/Serif JP) fonts with nanopdf's FontProvider so the renderer can
// resolve Standard-14 and Japanese glyphs without compile-time embedding.
// @fonts_dir is nanopdf's fonts/ directory. Returns the number of faces added.
int register_bundled_fonts(const std::string& fonts_dir);

// One rasterized page held as an ARGB32 (0xAARRGGBB) buffer plus a wrapping
// lvg_surface_t view. Owns the pixel storage.
struct RenderedPage {
  int page_index = -1;
  float scale = 0.0f;            // px-per-point at which this was rendered
  int width = 0;                 // pixels
  int height = 0;
  std::vector<uint32_t> argb;    // width*height, row-major top-down
  lvg_surface_t surface{};       // view over argb (pixels = argb.data())

  bool valid() const { return width > 0 && height > 0 && !argb.empty(); }
};

// A flattened bookmark/outline entry for the sidebar.
struct Bookmark {
  std::string title;
  int depth = 0;   // nesting level (0 = top)
  int page = -1;   // 0-based GoTo destination, or -1 if not a GoTo
};

class PdfDocument {
 public:
  PdfDocument() = default;

  // Load a PDF from a file path. Returns false (with error()) on failure.
  bool load_file(const std::string& path);

  bool loaded() const { return loaded_; }
  const std::string& path() const { return path_; }
  const std::string& error() const { return error_; }

  // Raw parsed document + backing bytes (for info/signatures/forms and the
  // revision-history view). Valid only while loaded().
  const nanopdf::Pdf* pdf() const { return pdf_.get(); }
  const std::vector<uint8_t>& data() const { return data_; }

  // Render a page of an arbitrary already-parsed Pdf (used for revision
  // snapshots) into an lvg-ready ARGB surface owned by @out. Returns false on
  // failure. Does not touch the main cache.
  bool render_external(const nanopdf::Pdf& doc, int page_index, float scale,
                       RenderedPage* out);

  int page_count() const;

  // Page size in PDF points (user space), accounting for /Rotate.
  void page_size_points(int page_index, double* w_pt, double* h_pt) const;

  // Flattened document outline (bookmarks). Lazily built; empty if none.
  const std::vector<Bookmark>& outline();

  // Extracted text layout for a page (lazily built; one page cached). Used for
  // search and selection. Returns nullptr on failure.
  const nanopdf::TextPage* text_page(int page_index);

  // Page media box in PDF points: left/bottom/right/top. Used to map text
  // quads (PDF user space, y-up) to the rendered image (y-down).
  void page_box(int page_index, double* left, double* bottom, double* right,
                double* top) const;

  // Render @page_index at @scale (pixels per point). The returned pointer is
  // owned by an internal cache and stays valid until the next render of a
  // different (page,scale) evicts it. Returns nullptr on failure.
  const RenderedPage* render(int page_index, float scale);

  // Render a small thumbnail of @page_index roughly @target_w px wide. Kept in
  // a separate, larger cache so thumbnails don't evict the main view.
  const RenderedPage* render_thumbnail(int page_index, int target_w);

  void clear_cache() { cache_.clear(); thumb_cache_.clear(); }

 private:
  bool loaded_ = false;
  std::string path_;
  std::string error_;
  std::vector<uint8_t> data_;          // backing bytes (Pdf may reference them)
  std::unique_ptr<nanopdf::Pdf> pdf_;
  std::unique_ptr<nanopdf::RenderBackend> backend_;

  // Core render + LRU cache insert shared by render()/render_thumbnail().
  const RenderedPage* render_into(int page_index, float scale,
                                  std::vector<std::unique_ptr<RenderedPage>>& cache,
                                  size_t max_cached);

  // Simple LRU-ish cache, newest at back. Small (a few pages) to bound memory.
  std::vector<std::unique_ptr<RenderedPage>> cache_;
  static constexpr size_t kMaxCached = 6;

  std::vector<std::unique_ptr<RenderedPage>> thumb_cache_;
  static constexpr size_t kMaxThumbs = 80;

  std::vector<Bookmark> outline_;
  bool outline_built_ = false;

  std::unique_ptr<nanopdf::TextPage> text_page_;  // last-extracted page
  int text_page_index_ = -1;
};

}  // namespace pdfview
