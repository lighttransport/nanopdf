// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Abstract render-backend interface. Concrete backends (LightVGBackend,
// ThorVGBackend, ...) implement this so callers can switch between
// rasterizers at runtime.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nanopdf.hh"  // for Pdf, Page, RenderProgressCallback

namespace nanopdf {

// Pixel buffer returned by render_page / get_buffer.
// pixels is RGBA8888, row-major, top-down.
struct RenderResult {
  bool success{false};
  std::string error;
  std::vector<uint8_t> pixels;
  uint32_t width{0};
  uint32_t height{0};
};

// Output and rendering options shared by all backends.
struct RenderOptions {
  enum class Format { PNG, JPEG, BMP, TGA };
  Format format{Format::PNG};

  // JPEG quality 1-100 (JPEG only).
  int jpeg_quality{90};

  // PNG compression level 0-9.
  int png_compression{1};

  // DPI scaling factor (72 = 1x, 144 = 2x, ...).
  float dpi{72.0f};

  // Anti-aliasing hint. Backends may apply AA unconditionally.
  bool antialias{true};

  // LCD subpixel anti-aliasing for glyph bitmaps (LightVG only).
  // Renders glyphs at 3x horizontal resolution and encodes per-channel
  // coverage in ARGB for subpixel text on non-white backgrounds.
  bool lcd_subpixel{false};

  // Background fill colour (RGBA).
  uint8_t bg_r{255};
  uint8_t bg_g{255};
  uint8_t bg_b{255};
  uint8_t bg_a{255};

  // Optional progress callback for dense pages.
  RenderProgressCallback progress_callback;
  size_t progress_object_threshold{100};
  uint32_t progress_percent_step{1};

  // Maximum pixels used when LightVG has to rasterize a PDF Type 1
  // function-based shading into an intermediate bitmap. 0 means unlimited.
  size_t max_function_shading_pixels{1024 * 1024};
};

// Identifies a concrete backend implementation. New backends append to this
// enum; callers should treat unknown values as "use default".
enum class BackendKind {
  LightVG,  // lightui vg software rasterizer (default)
  ThorVG,   // ThorVG (requires NANOPDF_USE_THORVG)
};

// Abstract base. All public callers should program against this interface
// (and use make_backend() to construct one) so the rasterizer can be swapped
// without recompiling.
class RenderBackend {
 public:
  virtual ~RenderBackend() = default;

  virtual bool initialize(uint32_t width, uint32_t height) = 0;

  virtual RenderResult render_page(const Pdf& pdf, const Page& page) = 0;
  virtual RenderResult render_page(const Pdf& pdf, const Page& page,
                                   const RenderOptions& options) = 0;

  // Backends that keep an internal render target can skip populating
  // RenderResult::pixels when the caller will save directly through the
  // backend. The default preserves existing behavior.
  virtual void set_render_result_pixels_enabled(bool enabled) {
    (void)enabled;
  }

  // Allows a backend-specific direct-save path to keep its native BGRA byte
  // layout when the caller will immediately save to a format that accepts it.
  virtual void set_direct_bgra_output_enabled(bool enabled) {
    (void)enabled;
  }

  virtual void set_progress_callback(RenderProgressCallback callback,
                                     size_t object_threshold = 100,
                                     uint32_t percent_step = 1) = 0;
  virtual void clear_progress_callback() = 0;

  virtual RenderResult get_buffer() = 0;

  virtual bool save_to_png(const std::string& filename) = 0;
  virtual bool save_to_file(const std::string& filename,
                            const RenderOptions& options = RenderOptions()) = 0;
  virtual bool save_to_file_rotated(const std::string& filename,
                                    const RenderOptions& options,
                                    int rotation_degrees) {
    (void)filename;
    (void)options;
    (void)rotation_degrees;
    return false;
  }

  // Identifies which concrete backend this instance is.
  virtual BackendKind kind() const = 0;
};

// Factory. Returns a backend of the requested kind, or nullptr if the kind is
// not compiled in (e.g. ThorVG was requested but NANOPDF_USE_THORVG is OFF).
std::unique_ptr<RenderBackend> make_backend(BackendKind kind);

// Returns true if @kind is compiled into this build.
bool backend_available(BackendKind kind);

// Parses a backend name ("lightvg" / "thorvg", case-insensitive). Returns true
// on success.
bool parse_backend_kind(const std::string& name, BackendKind* out);

// Returns a stable short name for a kind ("lightvg" / "thorvg"). Used for
// help text and error messages.
const char* backend_kind_name(BackendKind kind);

}  // namespace nanopdf
