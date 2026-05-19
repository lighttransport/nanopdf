// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#pragma once

#ifdef NANOPDF_USE_THORVG

#include <thorvg.h>
#include <deque>
#include <memory>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>

// stb_truetype is kept only as a fallback for the rare font that
// ttf_parse rejects (malformed/unusual sfnt tables). All glyph metric,
// cmap, outline, and raster lookups go through ttf_parse + rasterize by default.
#include "stb_truetype.h"

// ttf_parse + rasterize — the primary TrueType/CFF stack (ported from lightui).
#include "ttf_parse.h"
#include "rasterize.h"

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
#include "render-backend.hh"

namespace nanopdf {

// Backwards-compat aliases: legacy callers use these names, the actual
// type lives in render-backend.hh now.
using ThorVGRenderResult  = RenderResult;
using ThorVGRenderOptions = RenderOptions;

class ThorVGBackend : public RenderBackend {
public:
  ThorVGBackend();
  ~ThorVGBackend() override;

  // Initialize ThorVG with given canvas size
  bool initialize(uint32_t width, uint32_t height) override;

  // Render a PDF page
  ThorVGRenderResult render_page(const Pdf& pdf, const Page& page) override;
  void set_progress_callback(RenderProgressCallback callback,
                             size_t object_threshold = 100,
                             uint32_t percent_step = 1) override;
  void clear_progress_callback() override;

  BackendKind kind() const override { return BackendKind::ThorVG; }

  // Direct drawing API for testing
  bool begin_scene();
  bool end_scene();

  // Shape drawing
  bool draw_rectangle(float x, float y, float width, float height,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
  bool draw_circle(float cx, float cy, float radius,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
  bool draw_path(const std::vector<tvg::PathCommand>& cmds, const std::vector<tvg::Point>& pts,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Text drawing (basic support)
  bool draw_text(float x, float y, const std::string& text, float size,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Line drawing
  bool draw_line(float x1, float y1, float x2, float y2, float stroke_width,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Get rendered buffer
  ThorVGRenderResult get_buffer() override;

  // Save to PNG file
  bool save_to_png(const std::string& filename) override;

  // Save to file with options
  bool save_to_file(const std::string& filename,
                    const ThorVGRenderOptions& options = ThorVGRenderOptions()) override;

  // Render page with options (handles DPI scaling)
  ThorVGRenderResult render_page(const Pdf& pdf, const Page& page,
                                 const ThorVGRenderOptions& options) override;

private:
  // Parse PDF content stream and convert to ThorVG shapes
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
    int blend_mode{0};           // BM - blend mode (0=Normal)

    // Extended graphics state parameters
    bool alpha_is_shape{false};     // AIS - alpha source (false=opacity, true=shape)
    bool text_knockout{true};       // TK - text knockout (default true)
    bool overprint_stroke{false};   // OP - overprint for stroking
    bool overprint_fill{false};     // op - overprint for non-stroking
    int overprint_mode{0};          // OPM - overprint mode (0 or 1)
    float flatness{1.0f};           // FL - flatness tolerance
    bool stroke_adjustment{false};  // SA - stroke adjustment
    std::string rendering_intent{"RelativeColorimetric"};  // ri - rendering intent

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
    std::vector<tvg::PathCommand> path_commands;
    std::vector<tvg::Point> path_points;

    // Clipping path state
    bool has_clip{false};
    bool clip_even_odd{false};  // true for W* (even-odd), false for W (non-zero)
    std::vector<tvg::PathCommand> clip_commands;
    std::vector<tvg::Point> clip_points;

    // Page coordinate system info (set by render_page)
    float page_width{612.0f};
    float page_height{792.0f};
    float scale{1.0f};

    // Text state
    float text_leading{0.0f};        // TL - text leading (line spacing)
    float char_spacing{0.0f};        // Tc - character spacing
    float word_spacing{0.0f};        // Tw - word spacing
    float horiz_scaling{100.0f};     // Tz - horizontal scaling (percentage)
    float text_rise{0.0f};           // Ts - text rise (baseline shift)
    int text_render_mode{0};         // Tr - text rendering mode (0-7)

    // Text clipping path (accumulated during text block for modes 4-7)
    bool text_clip_active{false};    // True when accumulating text for clipping
    std::vector<tvg::PathCommand> text_clip_commands;
    std::vector<tvg::Point> text_clip_points;

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
    stbtt_fontinfo font_info{};      // Zero-init POD so any unread fields are deterministic
    bool initialized{false};
    bool is_embedded{false};  // true when loaded from PDF-embedded font stream
    std::vector<uint16_t> cid_to_gid;  // CFF charset CID→GID map (empty = identity)

    // ttf_parse context for kerning lookup (supports GPOS kerning)
    bool has_ttf_parse{false};
    ttf_font_t ttf{};                 // Zero-init POD; only read when has_ttf_parse is true
  };

  // Glyph bitmap cache key: font_name + glyph_id + quantized_size
  struct GlyphBitmapKey {
    std::string font_name;
    int glyph_id;
    uint16_t size_q;  // size * 4, quantized to quarter-pixel
    bool operator==(const GlyphBitmapKey& o) const {
      return font_name == o.font_name && glyph_id == o.glyph_id &&
             size_q == o.size_q;
    }
  };

  struct GlyphBitmapKeyHash {
    size_t operator()(const GlyphBitmapKey& k) const {
      size_t h = std::hash<std::string>{}(k.font_name);
      h ^= std::hash<int>{}(k.glyph_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= std::hash<uint16_t>{}(k.size_q) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct GlyphBitmapEntry {
    std::vector<uint8_t> bitmap;  // grayscale alpha
    int width{0}, height{0};
    float xoff{0.0f}, yoff{0.0f};  // offset from glyph origin (float for 2x precision)
  };

  // Draw an outlined "tofu" box placeholder for a missing glyph
  bool draw_missing_glyph_placeholder(float x, float y, float size,
                                      uint8_t r, uint8_t g, uint8_t b, uint8_t a);

  // Draw a single glyph using stb_truetype outlines (by Unicode codepoint)
  bool draw_glyph(int codepoint, float x, float y, float size,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a);

  // Draw a single glyph using stb_truetype outlines (by glyph index - for CID fonts)
  bool draw_glyph_by_index(int glyph_index, float x, float y, float size,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a);

  // Draw a glyph using stb_truetype bitmap rasterizer (higher quality for fill text)
  bool draw_glyph_bitmap(int codepoint, float x, float y, float size,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a);
  bool draw_glyph_bitmap_by_index(int glyph_index, float x, float y, float size,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a);

  // Render a Type 3 font glyph by executing its CharProc content stream
  bool render_type3_glyph(const Type3Font* type3_font, const std::string& glyph_name,
                          float x, float y, float size,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a);

  // Load font from PDF FontDescriptor
  bool load_font(const Pdf& pdf, const std::string& font_name, const BaseFont* font);

  // Load fallback system font
  bool load_fallback_font(const std::string& font_name);
  bool load_fallback_font_with_hint(const std::string& font_name, const BaseFont* font);

  // Get font cache entry
  FontCache* get_font(const std::string& font_name);

  // Calculate text width using font metrics (returns width in text space units)
  float calculate_text_width(const std::string& text, float font_size);
  // Calculate text advance using the same glyph-advance model as draw_text.
  float calculate_text_advance_draw_model(const std::string& text,
                                          float font_size);

  // Calculate vertical advance using W2/DW2 metrics (for vertical writing mode)
  float calculate_vertical_advance(const std::string& text, float font_size);

  // Draw an image XObject
  // fill_r/g/b: current non-stroking color, used to paint image mask pixels
  bool draw_image(const ImageXObject& image, float x, float y, float width, float height,
                  uint8_t fill_r = 0, uint8_t fill_g = 0, uint8_t fill_b = 0);

  // Draw a shading (gradient)
  bool draw_shading(const std::string& shading_name);

  // Apply pattern fill to a shape
  bool apply_pattern_fill(tvg::Shape* shape, const std::string& pattern_name, bool is_stroke);

  // Apply tiling pattern fill
  bool apply_tiling_pattern(tvg::Shape* shape, const TilingPattern* tiling,
                            const std::vector<double>& matrix, bool is_stroke);

  // Apply clipping path and push shape to scene
  bool push_with_clip(tvg::Shape* shape);

  // Apply soft mask opacity to a paint object (shape or picture)
  void apply_soft_mask_opacity(tvg::Paint* paint);

  // Parse and render inline image (BI/ID/EI operators)
  bool parse_inline_image(const std::string& content, size_t& pos);

  // Render a transparency group XObject to create a soft mask
  bool render_soft_mask_group(const Value& group_xobject, int mask_type);

  // Apply soft mask to current rendering (if active)
  void apply_soft_mask_to_context();

  // Per-pixel soft mask compositing on a rendered buffer
  void apply_soft_mask_to_pixels(uint8_t* pixels, uint32_t width,
                                  uint32_t height);

  void begin_progress(const RenderProgressCallback& callback,
                      size_t total_objects,
                      size_t object_threshold,
                      uint32_t percent_step);
  void advance_progress(size_t processed_objects = 1);
  void finish_progress();
  static size_t count_render_objects(const std::vector<uint8_t>& content_data);

  struct RenderProgressState {
    RenderProgressCallback callback;
    size_t total_objects{0};
    size_t processed_objects{0};
    uint32_t percent_step{1};
    uint32_t last_percent{0};
    bool enabled{false};
  };

  struct RenderProgressConfig {
    RenderProgressCallback callback;
    size_t object_threshold{100};
    uint32_t percent_step{1};
  };

  tvg::SwCanvas* canvas_{nullptr};
  tvg::Scene* scene_{nullptr};
  std::vector<uint32_t> buffer_;
  uint32_t width_{0};
  uint32_t height_{0};
  bool antialias_{true};
  bool initialized_{false};
  GraphicsState state_;

  // Font cache - maps font names to loaded font data
  std::map<std::string, FontCache> font_cache_;

  // Glyph bitmap cache for stb_truetype bitmap rasterizer
  std::unordered_map<GlyphBitmapKey, GlyphBitmapEntry, GlyphBitmapKeyHash>
      glyph_bitmap_cache_;
  static constexpr size_t kMaxGlyphCacheEntries = 4096;

  // Current font for text rendering
  std::string current_font_name_;
  const BaseFont* current_font_{nullptr};  // Pointer to current font for encoding lookup

  // Pointer to current PDF and Page for font access
  const Pdf* current_pdf_{nullptr};
  const Page* current_page_{nullptr};
  RenderProgressConfig progress_config_;
  RenderProgressState progress_;

  // Form XObject resource stack for nested Form XObjects
  // Each entry contains resources from a Form XObject
  std::vector<Dictionary> form_resources_stack_;

  // True while rendering text chunks originating from a TJ array.
  // Used to disable bitmap glyph snapping for high-precision spacing.
  bool in_tj_text_draw_{false};

  // Helper to lookup a resource, checking Form resources first, then page resources
  const Value* lookup_resource(const std::string& resource_type, const std::string& name) const;

  // Owns Values that lookup_resource() had to resolve from REFERENCE entries.
  // Returned pointers reference entries inside these owned dicts, so the
  // storage must outlive the caller's use of the pointer. Uses deque so
  // emplace_back never invalidates existing addresses. Cleared at the start
  // of each render_page().
  mutable std::deque<Value> lookup_resolved_owned_;

  // After draw_image() completes, holds the ARGB pixel buffer so the caller
  // can store it in the render cache. Only filled when the image came from a
  // named XObject (obj_num != 0). Cleared once consumed.
  std::vector<uint32_t> last_image_argb_;

  // Reusable ARGB buffer for glyph rendering.
  std::vector<uint32_t> glyph_argb_buf_;
};

}  // namespace nanopdf

#endif // NANOPDF_USE_THORVG
