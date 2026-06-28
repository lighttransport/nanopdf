// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// lvg-compat.hh — Compatibility shim that maps the subset of the ThorVG
// (`tvg::`) API used by thorvg-backend.cc to nanopdf's vendored lightvg
// (`lvg_canvas`) software rasterizer.
//
// The shim is private to lightvg-backend.cc; it lives in `namespace lvg` so
// it does not collide with the real `namespace tvg` from the actual ThorVG
// headers when both backends are compiled into the same library.
//
// Design notes
// ------------
// * Paint objects (Shape, Picture, Gradient, Scene) are heap-allocated by
//   `gen()`. Ownership transfers to a parent on `Scene::add()` /
//   `SwCanvas::add()`; the parent destroys children. `unref()` releases an
//   un-added paint.
// * `SwCanvas::target()` wraps a caller-owned pixel buffer (no copy).
// * `Scene` is a deferred command list. `SwCanvas::draw()` walks the scene
//   and replays each paint onto the underlying `lvg_canvas_t` immediately.
// * Cubic Beziers are flattened to polygon vertices for `lvg_canvas_*` fills
//   and strokes; flattening uses adaptive subdivision capped at depth 6.
// * Vector clip uses a rect fast path when possible, otherwise the clipper
//   path is rasterized to an alpha mask and intersected with the canvas clip.
// * Polygon gradient fills and gradient strokes are sampled per pixel when
//   lvg_canvas has no native primitive for the requested operation.

#pragma once

#ifdef NANOPDF_USE_LIGHTVG

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

extern "C" {
#include <lightvg/surface.h>
#include <lightvg/canvas.h>
}

namespace lvg {

// ---------------------------------------------------------------------------
// Enums (kept layout-compatible with ThorVG so PDF-level switch tables work).
// ---------------------------------------------------------------------------
enum class Result : uint8_t {
  Success = 0,
  InvalidArguments,
  InsufficientCondition,
  FailedAllocation,
  MemoryCorruption,
  NonSupport,
  Unknown,
};

enum class FillRule : uint8_t { NonZero = 0, EvenOdd };
enum class StrokeCap : uint8_t { Square = 0, Round, Butt };
enum class StrokeJoin : uint8_t { Bevel = 0, Round, Miter };
enum class FillSpread : uint8_t { Pad = 0, Reflect, Repeat };

enum class ColorSpace : uint8_t {
  ABGR8888 = 0,
  ARGB8888,
  ABGR8888S,
  ARGB8888S,
};

enum class BlendMethod : uint8_t {
  Normal = 0,
  Multiply,
  Screen,
  Overlay,
  Darken,
  Lighten,
  ColorDodge,
  ColorBurn,
  HardLight,
  SoftLight,
  Difference,
  Exclusion,
  Hue,
  Saturation,
  Color,
  Luminosity,
};

enum class PathCommand : uint8_t {
  Close = 0,
  MoveTo,
  LineTo,
  CubicTo,
};

// ---------------------------------------------------------------------------
// POD types
// ---------------------------------------------------------------------------
struct Point {
  float x;
  float y;
};

struct Matrix {
  float e11{1.0f}, e12{0.0f}, e13{0.0f};
  float e21{0.0f}, e22{1.0f}, e23{0.0f};
  float e31{0.0f}, e32{0.0f}, e33{1.0f};
};

namespace Fill {
struct ColorStop {
  float offset;
  uint8_t r, g, b, a;
};
}  // namespace Fill

// ---------------------------------------------------------------------------
// Forward decls
// ---------------------------------------------------------------------------
class Paint;
class Shape;
class Picture;
class Scene;
class LinearGradient;
class RadialGradient;
class SwCanvas;

// Canvas-resolution alpha bitmap used by Paint::softMask. Shared between
// paints via shared_ptr — the backend builds one per soft-mask state change
// and assigns it to each masked paint, so we avoid per-paint copies.
struct SoftMaskData {
  std::vector<uint8_t> data;  // grayscale 0..255
  uint32_t w{0};
  uint32_t h{0};
};

// ---------------------------------------------------------------------------
// Paint base
// ---------------------------------------------------------------------------
class Paint {
 public:
  virtual ~Paint() = default;

  // Release an un-added paint. Adding to a Scene transfers ownership; do not
  // unref after add().
  void unref();

  Result blend(BlendMethod m) {
    blend_mode_ = m;
    return Result::Success;
  }

  // Clip this paint with a Shape. Axis-aligned rect clips use the canvas clip;
  // arbitrary clips are rasterized to a temporary alpha mask. The clipper is
  // owned by this paint after this call.
  Result clip(Shape* clipper);

  // Pre-multiplied opacity (0-255).
  Result opacity(uint8_t a) {
    opacity_ = a;
    return Result::Success;
  }

  // Attach a soft mask. When set, the Paint snapshots the canvas before
  // drawing and lerps the pre/post pixels back per-mask-pixel after, so the
  // mask varies opacity across the paint rather than applying a scalar.
  Result softMask(std::shared_ptr<const SoftMaskData> m) {
    soft_mask_ = std::move(m);
    return Result::Success;
  }

  // ThorVG: get axis-aligned bounding box. `transformed` ignored for now.
  Result bounds(float* x, float* y, float* w, float* h,
                bool transformed = false) const;

  // Type tag used for dispatch in Scene draw.
  enum Kind { kShape, kPicture, kScene, kLinear, kRadial };
  virtual Kind kind() const = 0;

  virtual void draw_on(lvg_canvas_t* canvas) = 0;

 protected:
  BlendMethod blend_mode_{BlendMethod::Normal};
  uint8_t     opacity_{255};

  // Optional clipper applied on draw_on.
  Shape* clipper_{nullptr};

  // Optional soft mask. When non-null, draw_on dispatches through
  // draw_with_soft_mask() in lvg-compat.cc.
  std::shared_ptr<const SoftMaskData> soft_mask_;
};

// ---------------------------------------------------------------------------
// Shape: path + fill/stroke style.
// ---------------------------------------------------------------------------
class Shape : public Paint {
 public:
  static Shape* gen();
  ~Shape() override;

  Result moveTo(float x, float y);
  Result lineTo(float x, float y);
  Result cubicTo(float c1x, float c1y, float c2x, float c2y, float ex, float ey);
  Result close();
  Result appendRect(float x, float y, float w, float h, float rx = 0,
                    float ry = 0);
  Result appendCircle(float cx, float cy, float rx, float ry);
  Result appendPath(const PathCommand* cmds, uint32_t cmd_cnt,
                    const Point* pts, uint32_t pt_cnt);

  Result fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
  // Take ownership of gradient. Mutually exclusive with solid fill().
  Result fill(LinearGradient* g);
  Result fill(RadialGradient* g);
  Result fillRule(FillRule rule) {
    fill_rule_ = rule;
    return Result::Success;
  }

  Result strokeWidth(float w) {
    stroke_width_ = w;
    has_stroke_   = w > 0.0f;
    return Result::Success;
  }
  Result strokeFill(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
  // Gradient strokeFill: takes ownership. Sampled per pixel on draw when
  // lvg_canvas has no gradient-stroke primitive.
  Result strokeFill(LinearGradient* g);
  Result strokeFill(RadialGradient* g);
  Result strokeCap(StrokeCap c) {
    stroke_cap_ = c;
    return Result::Success;
  }
  Result strokeJoin(StrokeJoin j) {
    stroke_join_ = j;
    return Result::Success;
  }
  Result strokeMiterlimit(float m) {
    stroke_miter_ = m;
    return Result::Success;
  }
  Result strokeDash(const float* pattern, uint32_t count, float phase = 0);

  Kind kind() const override { return kShape; }
  void draw_on(lvg_canvas_t* canvas) override;

  // Used by Scene/Picture to expose the path for clip approximation.
  void compute_bbox(float* x, float* y, float* w, float* h) const;

 private:
  Shape() = default;

  std::vector<PathCommand> cmds_;
  std::vector<Point>       pts_;
  Point                    pen_{0, 0};

  // Fill: either solid colour or gradient (mutually exclusive).
  uint8_t  fill_r_{0}, fill_g_{0}, fill_b_{0}, fill_a_{0};
  bool     has_solid_fill_{false};
  Paint*   fill_gradient_{nullptr};  // owned LinearGradient or RadialGradient

  // Stroke style.
  bool       has_stroke_{false};
  float      stroke_width_{1.0f};
  uint8_t    stroke_r_{0}, stroke_g_{0}, stroke_b_{0}, stroke_a_{0};
  bool       has_stroke_color_{false};
  Paint*     stroke_gradient_{nullptr};  // owned
  StrokeCap  stroke_cap_{StrokeCap::Butt};
  StrokeJoin stroke_join_{StrokeJoin::Miter};
  float      stroke_miter_{4.0f};
  std::vector<float> stroke_dash_;
  float      stroke_dash_phase_{0.0f};

  FillRule   fill_rule_{FillRule::NonZero};

  friend class Scene;
  friend class SwCanvas;
  friend class Picture;
};

// ---------------------------------------------------------------------------
// Picture: raster image + transform.
// ---------------------------------------------------------------------------
class Picture : public Paint {
 public:
  static Picture* gen();
  ~Picture() override;

  // Loads a buffer of ARGB/ABGR pixels. Picture takes a copy (we own it).
  Result load(const uint32_t* data, uint32_t w, uint32_t h, ColorSpace cs,
              bool premultiplied = false);
  Result load_argb_owned(std::vector<uint32_t>&& pixels, uint32_t w, uint32_t h,
                         bool premultiplied = false);

  Result transform(const Matrix& m);
  Result translate(float x, float y);
  Result interpolate(bool enabled) {
    interpolate_ = enabled;
    return Result::Success;
  }

  Kind kind() const override { return kPicture; }
  void draw_on(lvg_canvas_t* canvas) override;

  // Forward-transformed bbox of the source image rectangle.
  void compute_bbox(float* x, float* y, float* w, float* h) const;

 private:
  Picture() = default;

  void draw_pixels(lvg_canvas_t* canvas);

  // Slow path: per-pixel inverse-transform sample for rotated/sheared,
  // mirrored, blended, or opacity-adjusted pictures.
  void draw_transformed(lvg_canvas_t* canvas);

  std::vector<uint32_t> pixels_;  // ARGB-packed for lvg_surface
  uint32_t width_{0};
  uint32_t height_{0};
  Matrix   transform_{1, 0, 0, 0, 1, 0, 0, 0, 1};
  bool     interpolate_{false};
  bool     opaque_{false};
};

// ---------------------------------------------------------------------------
// LinearGradient / RadialGradient
// ---------------------------------------------------------------------------
class LinearGradient : public Paint {
 public:
  static LinearGradient* gen();

  Result linear(float x1, float y1, float x2, float y2) {
    x1_ = x1; y1_ = y1; x2_ = x2; y2_ = y2;
    return Result::Success;
  }
  Result colorStops(const Fill::ColorStop* stops, uint32_t count);
  Result spread(FillSpread s) {
    spread_ = s;
    return Result::Success;
  }

  Kind kind() const override { return kLinear; }
  void draw_on(lvg_canvas_t* /*canvas*/) override {}

  // Sample colour at gradient parameter t in [0,1]; clamps according to
  // spread mode (Pad only honoured; Repeat/Reflect treated as Pad).
  void sample(float t, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;

  float x1() const { return x1_; }
  float y1() const { return y1_; }
  float x2() const { return x2_; }
  float y2() const { return y2_; }
  bool empty() const { return stops_.empty(); }

  int stop_count() const { return static_cast<int>(stops_.size()); }
  const Fill::ColorStop& stop_at(int i) const { return stops_[i]; }
  FillSpread spread_mode() const { return spread_; }

 private:
  LinearGradient() = default;

  std::vector<Fill::ColorStop> stops_;
  FillSpread spread_{FillSpread::Pad};
  float x1_{0}, y1_{0}, x2_{0}, y2_{0};
};

class RadialGradient : public Paint {
 public:
  static RadialGradient* gen();

  // Modern ThorVG signature; focal-point variant not honoured (treated as
  // centre).
  Result radial(float cx, float cy, float r, float fx = 0, float fy = 0,
                float fr = 0);
  Result colorStops(const Fill::ColorStop* stops, uint32_t count);
  Result spread(FillSpread s) {
    spread_ = s;
    return Result::Success;
  }

  Kind kind() const override { return kRadial; }
  void draw_on(lvg_canvas_t* /*canvas*/) override {}

  void sample(float t, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;

  float cx() const { return cx_; }
  float cy() const { return cy_; }
  float r() const { return r_; }
  float fx() const { return fx_; }
  float fy() const { return fy_; }
  float fr() const { return fr_; }
  // PDF Shading Type 3 uses a focal circle (fx,fy,fr) distinct from the
  // outer circle (cx,cy,r). When the focal coincides with the centre and
  // its radius is zero we fall back to the simple d/r parameterisation.
  bool has_focal() const {
    constexpr float eps = 1e-4f;
    return std::fabs(fx_ - cx_) > eps || std::fabs(fy_ - cy_) > eps ||
           std::fabs(fr_)       > eps;
  }
  bool empty() const { return stops_.empty(); }

  int stop_count() const { return static_cast<int>(stops_.size()); }
  const Fill::ColorStop& stop_at(int i) const { return stops_[i]; }
  FillSpread spread_mode() const { return spread_; }

 private:
  RadialGradient() = default;

  std::vector<Fill::ColorStop> stops_;
  FillSpread spread_{FillSpread::Pad};
  float cx_{0}, cy_{0}, r_{0};
  float fx_{0}, fy_{0}, fr_{0};
};

// ---------------------------------------------------------------------------
// Scene: ordered list of paints.
// ---------------------------------------------------------------------------
class Scene : public Paint {
 public:
  static Scene* gen();
  ~Scene() override;

  Result add(Paint* p);

  Kind kind() const override { return kScene; }
  void draw_on(lvg_canvas_t* canvas) override;

  // Reset state without destroying.
  void clear();

  // Axis-aligned union of all child paints' bounding boxes.
  void compute_bbox(float* x, float* y, float* w, float* h) const;

 private:
  Scene() = default;

  std::vector<Paint*> paints_;
};

// ---------------------------------------------------------------------------
// SwCanvas: wraps a caller-owned ABGR/ARGB pixel buffer.
// ---------------------------------------------------------------------------
class SwCanvas {
 public:
  static SwCanvas* gen();
  ~SwCanvas();

  // Caller owns the buffer. `stride` is in pixels.
  Result target(uint32_t* buffer, uint32_t stride, uint32_t width,
                uint32_t height, ColorSpace cs);

  Result add(Paint* p);
  Result remove(Paint* p = nullptr);
  Result draw(bool clear);
  Result sync();
  void setOutputSwizzle(bool enabled) { output_swizzle_enabled_ = enabled; }

  // Returns the wrapped surface (for direct pixel access).
  lvg_surface_t* surface() { return &surface_; }
  ColorSpace colorspace() const { return cs_; }

 private:
  SwCanvas() = default;

  std::vector<Paint*> paints_;
  lvg_surface_t surface_{};
  lvg_canvas_t  canvas_{};
  bool          have_canvas_{false};
  ColorSpace    cs_{ColorSpace::ABGR8888};
  bool          output_swizzle_enabled_{true};
};

// ---------------------------------------------------------------------------
// Initializer (no-op for lvg_canvas — kept for API parity with ThorVG).
// ---------------------------------------------------------------------------
class Initializer {
 public:
  static Result init(int /*threads*/) { return Result::Success; }
  static Result term() { return Result::Success; }
};

// Sample a Linear/RadialGradient at canvas position (x, y). Returns 0xAARRGGBB
// in straight-alpha form, with the gradient's alpha multiplied by opacity_mul.
// Exposed for unit tests; also used internally by fill_polygon_with_source
// and the gradient-stroke compositor.
uint32_t sample_gradient_at(const Paint* grad, float x, float y,
                            uint8_t opacity_mul = 255);

}  // namespace lvg

#endif  // NANOPDF_USE_LIGHTVG
