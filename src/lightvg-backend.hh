// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#pragma once

#ifdef NANOPDF_USE_LIGHTVG

#include "lvg-compat.hh"
#include <array>
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
#include "type1-parser.hh"

namespace nanopdf {

// Color stop cache entry - declared outside class for use in static functions.
struct LightVGColorStopCacheEntry {
  std::vector<lvg::Fill::ColorStop> stops;
};

// Backwards-compat aliases: legacy callers use these names, the actual
// type lives in render-backend.hh now.
using LightVGRenderResult  = RenderResult;
using LightVGRenderOptions = RenderOptions;

class LightVGBackend : public RenderBackend {
public:
  LightVGBackend();
  ~LightVGBackend() override;

  // Initialize the lvg backend for a canvas of the given size.
  bool initialize(uint32_t width, uint32_t height) override;

  // Render a PDF page
  LightVGRenderResult render_page(const Pdf& pdf, const Page& page) override;
  void set_progress_callback(RenderProgressCallback callback,
                             size_t object_threshold = 100,
                             uint32_t percent_step = 1) override;
  void clear_progress_callback() override;
  void set_render_result_pixels_enabled(bool enabled) override {
    result_pixels_enabled_ = enabled;
  }
  void set_direct_bgra_output_enabled(bool enabled) override {
    direct_bgra_output_enabled_ = enabled;
  }

  BackendKind kind() const override { return BackendKind::LightVG; }

  // Direct drawing API for testing
  bool begin_scene();
  bool end_scene();

  // Shape drawing
  bool draw_rectangle(float x, float y, float width, float height,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
  bool draw_circle(float cx, float cy, float radius,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
  bool draw_path(const std::vector<lvg::PathCommand>& cmds, const std::vector<lvg::Point>& pts,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Text drawing (basic support)
  bool draw_text(float x, float y, const std::string& text, float size,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Line drawing
  bool draw_line(float x1, float y1, float x2, float y2, float stroke_width,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Get rendered buffer
  LightVGRenderResult get_buffer() override;

  // Save to PNG file
  bool save_to_png(const std::string& filename) override;

  // Save to file with options
  bool save_to_file(const std::string& filename,
                    const LightVGRenderOptions& options = LightVGRenderOptions()) override;

  // Render page with options (handles DPI scaling)
  LightVGRenderResult render_page(const Pdf& pdf, const Page& page,
                                 const LightVGRenderOptions& options) override;

  // Render cache statistics for profiling.
  struct CacheStats {
    size_t hits{0};
    size_t misses{0};
    size_t evictions{0};
    size_t entries{0};
    size_t bytes_used{0};
    size_t max_size{0};
  };
  CacheStats get_cache_stats() const;

private:
  // Parse PDF content stream and emit lvg paints into the active scene.
  bool parse_pdf_content(const std::vector<uint8_t>& content_data);

  // Render a page's annotations (appearance streams + form-widget defaults)
  // into the active scene. Shared by both render_page() overloads.
  void render_annotations(const Pdf& pdf, const Page& page, float page_width,
                          float page_height, float scale);

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
    std::vector<lvg::PathCommand> path_commands;
    std::vector<lvg::Point> path_points;

    // Clipping path state
    bool has_clip{false};
    bool clip_even_odd{false};  // true for W* (even-odd), false for W (non-zero)
    std::vector<lvg::PathCommand> clip_commands;
    std::vector<lvg::Point> clip_points;

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
    std::vector<lvg::PathCommand> text_clip_commands;
    std::vector<lvg::Point> text_clip_points;

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
    bool has_type1{false};
    Type1FontData type1;

    // Small integer id assigned at load time, used as the glyph-cache key
    // instead of the font name so per-glyph cache probes avoid string hashing.
    int font_id{0};

    // Lazily-filled glyph advance memo (font units, indexed by glyph id).
    // Sentinel -1 = not yet computed. Dedupes repeated ttf_hmtx_advance walks
    // across the measure and draw passes for hot glyphs.
    std::vector<int16_t> advance_cache;
  };

  // Glyph outline cache key: font_id + glyph_id.
  // Caches decomposed vector outlines so rotated/stroked/clipped text
  // reuses the outline without re-parsing the sfnt tables. Keyed on the
  // interned integer font id (not the name) to keep per-glyph probes cheap.
  struct GlyphOutlineKey {
    int font_id;
    int glyph_id;
    bool operator==(const GlyphOutlineKey& o) const {
      return font_id == o.font_id && glyph_id == o.glyph_id;
    }
  };

  struct GlyphOutlineKeyHash {
    size_t operator()(const GlyphOutlineKey& k) const {
      size_t h = std::hash<int>{}(k.font_id);
      h ^= std::hash<int>{}(k.glyph_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  // Stores a deep-copied ttf_outline_t for the glyph outline cache.
  struct GlyphOutlineEntry {
    std::vector<ttf_point_t> points;
    std::vector<int> contour_ends;
    int16_t x_min{0}, y_min{0}, x_max{0}, y_max{0};
  };

  // Glyph bitmap cache key: font_id + glyph_id + quantized_size + lcd flag
  struct GlyphBitmapKey {
    int font_id;
    int glyph_id;
    uint16_t size_q;  // size * 4, quantized to quarter-pixel
    bool lcd{false};  // LCD subpixel mode
    bool operator==(const GlyphBitmapKey& o) const {
      return font_id == o.font_id && glyph_id == o.glyph_id &&
             size_q == o.size_q && lcd == o.lcd;
    }
  };

  struct GlyphBitmapKeyHash {
    size_t operator()(const GlyphBitmapKey& k) const {
      size_t h = std::hash<int>{}(k.font_id);
      h ^= std::hash<int>{}(k.glyph_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= std::hash<uint16_t>{}(k.size_q) + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= std::hash<bool>{}(k.lcd) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct GlyphBitmapEntry {
    std::vector<uint8_t> bitmap;  // grayscale alpha (or 3x oversampled for LCD)
    int width{0}, height{0};
    float xoff{0.0f}, yoff{0.0f};  // offset from glyph origin (float for 2x precision)
    bool is_lcd{false};  // true when bitmap holds 3x oversampled LCD data
    int lcd_scale{1};    // oversample factor (1 for grayscale, 3 for LCD)
  };

  // Draw an outlined "tofu" box placeholder for a missing glyph
  bool draw_missing_glyph_placeholder(float x, float y, float size,
                                      uint8_t r, uint8_t g, uint8_t b, uint8_t a);

  // Try to render a missing glyph using fallback fonts (FontProvider).
  // Returns true if any fallback font successfully rendered the glyph.
  bool try_draw_glyph_fallback(int codepoint, float x, float y, float size,
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
  bool draw_type1_glyph_by_name(FontCache* font, const std::string& glyph_name,
                                float x, float y, float size,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a);

  // Load font from PDF FontDescriptor
  bool load_font(const Pdf& pdf, const std::string& font_name, const BaseFont* font);

  // Load fallback system font
  bool load_fallback_font(const std::string& font_name);
  bool load_fallback_font_with_hint(const std::string& font_name, const BaseFont* font);

  // Get font cache entry
  FontCache* get_font(const std::string& font_name);

  // Stable per-document identity key for a font, used as the cache key instead
  // of the page-local resource name (which collides across pages: the same
  // name like "TT3" can refer to different embedded fonts on different pages).
  // Returns "E<obj>_<gen>" for embedded fonts (the font-file stream object id),
  // "F<base_font>" for non-embedded fonts, or "" when no stable identity is
  // available (caller then falls back to the resource name).
  std::string font_cache_key(const BaseFont* font) const;

  // Memoized horizontal advance (font units) for a glyph, backed by
  // FontCache::advance_cache. Falls back to ttf_hmtx_advance on a miss.
  uint16_t glyph_advance_units(FontCache* font, uint16_t gid);

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
                  uint8_t fill_r = 0, uint8_t fill_g = 0, uint8_t fill_b = 0,
                  const GraphicsState::Matrix* image_ctm = nullptr,
                  bool retain_converted_argb = false);

  // Draw a shading (gradient)
  bool draw_shading(const std::string& shading_name);

  // Apply pattern fill to a shape
  bool apply_pattern_fill(lvg::Shape* shape, const std::string& pattern_name, bool is_stroke);

  // Apply tiling pattern fill
  bool apply_tiling_pattern(lvg::Shape* shape, const TilingPattern* tiling,
                            const std::vector<double>& matrix, bool is_stroke);

  // Apply clipping path and push shape to scene
  bool push_with_clip(lvg::Shape* shape);

  // Apply soft mask opacity to a paint object (shape or picture)
  void apply_soft_mask_opacity(lvg::Paint* paint);

  // Parse and render inline image (BI/ID/EI operators)
  bool parse_inline_image(const std::string& content, size_t& pos);

  // Render a transparency group XObject to create a soft mask
  bool render_soft_mask_group(const Value& group_xobject, int mask_type);

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

  lvg::SwCanvas* canvas_{nullptr};
  lvg::Scene* scene_{nullptr};
  std::vector<uint32_t> buffer_;
  uint32_t width_{0};
  uint32_t height_{0};
  bool antialias_{true};
  bool enable_lcd_{false};
  bool initialized_{false};
  GraphicsState state_;

  // Font cache - maps font names to loaded font data
  std::map<std::string, FontCache> font_cache_;

  // Sequential id source for FontCache::font_id (0 is reserved/unset).
  int next_font_id_{1};

  // Single-slot memo for get_font(): the text hot path resolves the current
  // font several times per glyph, so cache the last successful {name -> ptr}
  // lookup. std::map node addresses are stable across inserts, so the pointer
  // stays valid as later fonts load. Only successful lookups are memoized.
  std::string memo_font_name_;
  FontCache* memo_font_ptr_{nullptr};

  // Glyph outline vector cache for rotated/stroked/clipped text.
  // Avoids re-parsing sfnt tables and re-decomposing outlines per frame.
  std::unordered_map<GlyphOutlineKey, GlyphOutlineEntry, GlyphOutlineKeyHash>
      glyph_outline_cache_;
  static constexpr size_t kMaxGlyphOutlineCacheEntries = 2048;

  // Glyph bitmap cache for stb_truetype bitmap rasterizer
  // Single-entry eviction when full replaces the old full-clear strategy.
  std::unordered_map<GlyphBitmapKey, GlyphBitmapEntry, GlyphBitmapKeyHash>
      glyph_bitmap_cache_;
  static constexpr size_t kMaxGlyphCacheEntries = 4096;

  // Current font for text rendering
  std::string current_font_name_;
  const BaseFont* current_font_{nullptr};  // Pointer to current font for encoding lookup
  // Guards try_draw_glyph_fallback against re-entry: a fallback font that has
  // the codepoint in its cmap but whose outline can't be extracted could
  // otherwise oscillate between fallback fonts.
  bool in_glyph_fallback_{false};

  // Pointer to current PDF and Page for font access
  const Pdf* current_pdf_{nullptr};
  const Page* current_page_{nullptr};
  RenderProgressConfig progress_config_;
  RenderProgressState progress_;
  bool result_pixels_enabled_{true};
  bool clear_canvas_on_draw_{true};
  bool direct_bgra_output_enabled_{false};
  bool rendering_soft_mask_group_{false};

  // Form XObject resource stack for nested Form XObjects
  // Each entry contains resources from a Form XObject
  std::vector<Dictionary> form_resources_stack_;

  // True while rendering text chunks originating from a TJ array.
  // Used to disable bitmap glyph snapping for high-precision spacing.
  bool in_tj_text_draw_{false};

  // Helper to lookup a resource, checking Form resources first, then page resources
  const Value* lookup_resource(const std::string& resource_type, const std::string& name) const;

  bool apply_extgstate(const std::string& name);
  bool apply_extgstate_dict(const Dictionary& gs_dict);

  // Owns Values that lookup_resource() had to resolve from REFERENCE entries.
  // Returned pointers reference entries inside these owned dicts, so the
  // storage must outlive the caller's use of the pointer. Uses deque so
  // emplace_back never invalidates existing addresses. Cleared at the start
  // of each render_page().
  mutable std::deque<Value> lookup_resolved_owned_;

  // Fonts parsed from nested resource dictionaries that are not present in
  // Page::fonts. Cleared at the start of each render_page().
  std::deque<std::unique_ptr<BaseFont>> lookup_font_owned_;

  // Shared snapshot of the active soft mask. Built lazily in
  // apply_soft_mask_opacity() so each masked paint shares one allocation.
  // Cleared via cache-key mismatch when the underlying soft_mask_data changes.
  std::shared_ptr<const lvg::SoftMaskData> current_soft_mask_;

  // After draw_image() completes, holds the ARGB pixel buffer so the caller
  // can store it in the render cache. Only filled when the image came from a
  // named XObject (obj_num != 0). Cleared once consumed.
  std::vector<uint32_t> last_image_argb_;

  // Current render option for function-based shadings.
  size_t max_function_shading_pixels_{1024 * 1024};

  // Reusable ARGB buffer for glyph rendering. Avoids per-glyph allocation
  // in draw_glyph_bitmap_by_index when the render cache misses.
  std::vector<uint32_t> glyph_argb_buf_;

  // Separation tint function LUT cache (256-entry precomputed ARGB LUT).
  struct TintLutKey {
    uint32_t func_obj_num;
    uint32_t alt_cs;
    bool operator==(const TintLutKey& o) const {
      return func_obj_num == o.func_obj_num && alt_cs == o.alt_cs;
    }
  };
  struct TintLutKeyHash {
    size_t operator()(const TintLutKey& k) const {
      size_t h = std::hash<uint32_t>{}(k.func_obj_num);
      h ^= std::hash<uint32_t>{}(k.alt_cs) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct TintLutEntry {
    std::array<uint32_t, 256> lut;
  };
  std::unordered_map<TintLutKey, TintLutEntry, TintLutKeyHash> tint_lut_cache_;

  // Color stop cache for gradient shadings.
  std::unordered_map<uint32_t, LightVGColorStopCacheEntry> color_stop_cache_;
};

}  // namespace nanopdf

#endif // NANOPDF_USE_LIGHTVG
