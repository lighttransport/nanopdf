// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#pragma once

#ifdef NANOPDF_USE_BLEND2D

#include <blend2d.h>
#include <memory>
#include <vector>
#include <string>
#include <map>

// stb_truetype for glyph outline extraction
#include "stb_truetype.h"

#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstring.h"
#include "nanostring.h"
#include "nanovector.h"
using namespace nanostl;
#else
#include <cstring>
#include <string>
#include <vector>
#endif

#include "nanopdf.hh"

namespace nanopdf {

struct Blend2DRenderResult {
  bool success{false};
  std::string error;
  std::vector<uint8_t> pixels;  // RGBA8888 format
  uint32_t width{0};
  uint32_t height{0};
};

// Render options for output quality and format
struct RenderOptions {
  // Output format
  enum class Format {
    PNG,
    JPEG,
    BMP,
    TGA
  };
  Format format{Format::PNG};

  // JPEG quality (1-100, only used for JPEG format)
  int jpeg_quality{90};

  // PNG compression level (0-9, higher = more compression but slower)
  int png_compression{6};

  // DPI scaling factor (72 = 1x, 144 = 2x, etc.)
  float dpi{72.0f};

  // Anti-aliasing (for future use)
  bool antialias{true};

  // Background color (RGBA)
  uint8_t bg_r{255}, bg_g{255}, bg_b{255}, bg_a{255};
};

class Blend2DBackend {
public:
  Blend2DBackend();
  ~Blend2DBackend();

  // Initialize Blend2D with given canvas size
  bool initialize(uint32_t width, uint32_t height);

  // Render a PDF page
  Blend2DRenderResult render_page(const Pdf& pdf, const Page& page);

  // Direct drawing API for testing
  bool begin_scene();
  bool end_scene();

  // Shape drawing
  bool draw_rectangle(float x, float y, float width, float height,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
  bool draw_circle(float cx, float cy, float radius,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
  bool draw_path(const BLPath& path,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Text drawing (basic support)
  bool draw_text(float x, float y, const std::string& text, float size,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Line drawing
  bool draw_line(float x1, float y1, float x2, float y2, float stroke_width,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Get rendered buffer
  Blend2DRenderResult get_buffer();

  // Save to PNG file
  bool save_to_png(const std::string& filename);

  // Save to file with options
  bool save_to_file(const std::string& filename, const RenderOptions& options = RenderOptions());

  // Render page with options (handles DPI scaling)
  Blend2DRenderResult render_page(const Pdf& pdf, const Page& page, const RenderOptions& options);
  
  // Calculate text width using font metrics (returns width in text space units)
  float calculate_text_width(const std::string& text, float font_size);

  void set_current_font(const std::string& font_name, const BaseFont* font);

private:
  // Parse PDF content stream and convert to Blend2D shapes
  bool parse_pdf_content(const std::vector<uint8_t>& content_data);

  // Graphics state for PDF rendering
  struct GraphicsState {
    float current_x{0.0f};
    float current_y{0.0f};
    float font_size{12.0f};
    uint8_t fill_r{0}, fill_g{0}, fill_b{0}, fill_a{255};
    uint8_t stroke_r{0}, stroke_g{0}, stroke_b{0}, stroke_a{255};
    float stroke_width{1.0f};
    int line_cap{0};      // 0=butt, 1=round, 2=square
    int line_join{0};     // 0=miter, 1=round, 2=bevel
    float miter_limit{10.0f};
    std::vector<float> dash_pattern;  // d - dash pattern array
    float dash_phase{0.0f};           // d - dash phase (offset)
    float fill_opacity{1.0f};    // ca - fill alpha (0-1)
    float stroke_opacity{1.0f};  // CA - stroke alpha (0-1)
    int blend_mode{0};           // BM - blend mode (0=Normal, see BLCompOp)

    // Extended graphics state parameters
    bool alpha_is_shape{false};     // AIS - alpha source (false=opacity, true=shape)
    bool text_knockout{true};       // TK - text knockout (default true)
    bool overprint_stroke{false};   // OP - overprint for stroking
    bool overprint_fill{false};     // op - overprint for non-stroking
    int overprint_mode{0};          // OPM - overprint mode (0 or 1)
    float flatness{1.0f};           // FL - flatness tolerance
    bool stroke_adjustment{false};  // SA - stroke adjustment

    // Soft mask support
    bool has_soft_mask{false};
    int soft_mask_type{0};          // 0=None, 1=Alpha, 2=Luminosity
    std::vector<uint8_t> soft_mask_data;  // Soft mask pixel data (if any)
    uint32_t soft_mask_width{0};
    uint32_t soft_mask_height{0};

    // Pattern support
    std::string fill_color_space;    // Current non-stroking color space name
    std::string stroke_color_space;  // Current stroking color space name
    std::string fill_pattern;        // Current fill pattern name (if cs = Pattern)
    std::string stroke_pattern;      // Current stroke pattern name (if CS = Pattern)

    bool in_text_block{false};
    bool in_path{false};
    BLPath current_path;

    // Clipping path state
    bool has_clip{false};
    bool clip_even_odd{false};  // true for W* (even-odd), false for W (non-zero)
    BLPath clip_path;

    // Page coordinate system info (set by render_page)
    float page_width{612.0f};
    float page_height{792.0f};
    float scale{1.0f};

    // Text state
    float text_leading{0.0f};        // TL - text leading (line spacing)
    float char_spacing{0.0f};        // Tc - character spacing
    float word_spacing{0.0f};        // Tw - word spacing
    float horiz_scaling{100.0f};     // Tz - horizontal scaling (percentage)
    int text_rise{0};                // Ts - text rise (baseline shift)
    int text_render_mode{0};         // Tr - text rendering mode (0-7)

    // Transformation matrix [a b c d e f]
    // Maps (x, y) -> (ax + cy + e, bx + dy + f)
    struct Matrix {
      float a{1.0f}, b{0.0f};  // Scale and rotation
      float c{0.0f}, d{1.0f};  // Scale and rotation
      float e{0.0f}, f{0.0f};  // Translation

      // Apply transformation to a point
      void transform(float& x, float& y) const {
        float nx = a * x + c * y + e;
        float ny = b * x + d * y + f;
        x = nx;
        y = ny;
      }

      // Concatenate with another matrix (multiply)
      Matrix operator*(const Matrix& other) const {
        Matrix result;
        result.a = a * other.a + b * other.c;
        result.b = a * other.b + b * other.d;
        result.c = c * other.a + d * other.c;
        result.d = c * other.b + d * other.d;
        result.e = e * other.a + f * other.c + other.e;
        result.f = e * other.b + f * other.d + other.f;
        return result;
      }

      // Reset to identity
      void reset() {
        a = 1.0f; b = 0.0f;
        c = 0.0f; d = 1.0f;
        e = 0.0f; f = 0.0f;
      }
    } transform;

    // Text matrix (Tm) - separate from CTM
    Matrix text_matrix;
    // Line matrix - tracks start of current line
    Matrix text_line_matrix;
  };

  // Font cache entry
  struct FontCache {
    std::vector<uint8_t> font_data;  // Raw font file data
    stbtt_fontinfo font_info;
    bool initialized{false};
  };

  // Draw a single glyph using stb_truetype outlines (by Unicode codepoint)
  bool draw_glyph(int codepoint, float x, float y, float size,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a);

  // Draw a single glyph using stb_truetype outlines (by glyph index - for CID fonts)
  bool draw_glyph_by_index(int glyph_index, float x, float y, float size,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a);

  // Render text string with CMap-aware encoding (handles CJK/CID fonts)
  // Returns the total width rendered in pixels
  float render_text_string(const std::string& text, float x, float y, float font_size,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a);

  // Load font from PDF FontDescriptor
  bool load_font(const Pdf& pdf, const std::string& font_name, const BaseFont* font);

  // Load fallback system font
  bool load_fallback_font(const std::string& font_name);
  bool load_fallback_font_with_hint(const std::string& font_name, const BaseFont* font);

  // Get font cache entry
  FontCache* get_font(const std::string& font_name);

  // Draw an image XObject
  bool draw_image(const ImageXObject& image, float x, float y, float width, float height);

  // Draw a shading (gradient)
  bool draw_shading(const std::string& shading_name);

  // Apply pattern fill to a shape
  bool apply_pattern_fill(BLPath& path, const std::string& pattern_name, bool is_stroke);

  // Push shape with clipping
  bool push_with_clip(BLPath& path, bool fill, bool stroke);

  // Parse and render inline image (BI/ID/EI operators)
  bool parse_inline_image(const std::string& content, size_t& pos);

  // Render a transparency group XObject to create a soft mask
  bool render_soft_mask_group(const Value& group_xobject, int mask_type);

  // Apply soft mask to current rendering (if active)
  void apply_soft_mask_to_context();

  BLImage image_;
  BLContext ctx_;
  uint32_t width_{0};
  uint32_t height_{0};
  bool initialized_{false};
  GraphicsState state_;

  // Font cache - maps font names to loaded font data
  std::map<std::string, FontCache> font_cache_;

  // Current font for text rendering
  std::string current_font_name_;
  const BaseFont* current_font_{nullptr};  // Pointer to current font for encoding lookup

  // Pointer to current PDF and Page for font access
  const Pdf* current_pdf_{nullptr};
  const Page* current_page_{nullptr};

  // Form XObject resource stack for nested Form XObjects
  // Each entry contains resources from a Form XObject
  std::vector<Dictionary> form_resources_stack_;

  // Helper to lookup a resource, checking Form resources first, then page resources
  const Value* lookup_resource(const std::string& resource_type, const std::string& name) const;
};

}  // namespace nanopdf

#endif // NANOPDF_USE_BLEND2D