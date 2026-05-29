// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#ifdef NANOPDF_USE_LIGHTVG

#include "lvg-compat.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace lvg {

namespace {

// Pack an 0xAARRGGBB lui_color from straight RGBA.
inline uint32_t pack_argb(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return (static_cast<uint32_t>(a) << 24) |
         (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) |
         static_cast<uint32_t>(b);
}

// Adaptive cubic Bezier flattening using midpoint subdivision. `depth` caps
// recursion; the flatness test is the chordal distance of control points
// from the chord.
void flatten_cubic(float x0, float y0, float x1, float y1, float x2, float y2,
                   float x3, float y3, std::vector<lui_pointf_t>& out,
                   int depth = 0) {
  // Flatness: max deviation of control points from line P0..P3.
  float dx = x3 - x0;
  float dy = y3 - y0;
  float len2 = dx * dx + dy * dy;
  float d1 = std::fabs((x1 - x0) * dy - (y1 - y0) * dx);
  float d2 = std::fabs((x2 - x0) * dy - (y2 - y0) * dx);
  float dev = (d1 + d2) * (d1 + d2);
  // If the squared deviation is small relative to chord squared (~0.5 px),
  // we're done.
  if (depth >= 6 || dev <= 0.25f * len2 + 0.25f) {
    out.push_back({x3, y3});
    return;
  }
  float x01 = (x0 + x1) * 0.5f, y01 = (y0 + y1) * 0.5f;
  float x12 = (x1 + x2) * 0.5f, y12 = (y1 + y2) * 0.5f;
  float x23 = (x2 + x3) * 0.5f, y23 = (y2 + y3) * 0.5f;
  float x012 = (x01 + x12) * 0.5f, y012 = (y01 + y12) * 0.5f;
  float x123 = (x12 + x23) * 0.5f, y123 = (y12 + y23) * 0.5f;
  float x0123 = (x012 + x123) * 0.5f, y0123 = (y012 + y123) * 0.5f;
  flatten_cubic(x0, y0, x01, y01, x012, y012, x0123, y0123, out, depth + 1);
  flatten_cubic(x0123, y0123, x123, y123, x23, y23, x3, y3, out, depth + 1);
}

// Convert a list of (cmd, point) into one-or-more closed contours, each a
// run of polygon vertices in `out_contours`.
struct Contour {
  std::vector<lui_pointf_t> pts;
};

void flatten_path(const std::vector<PathCommand>& cmds,
                  const std::vector<Point>& pts,
                  std::vector<Contour>& out) {
  Contour cur;
  cur.pts.reserve(std::min<size_t>(pts.size(), 64));
  out.reserve(out.size() + 1);
  size_t pi = 0;
  float lastx = 0, lasty = 0;
  float startx = 0, starty = 0;
  for (size_t ci = 0; ci < cmds.size(); ++ci) {
    switch (cmds[ci]) {
      case PathCommand::MoveTo: {
        if (pi >= pts.size()) return;
        if (!cur.pts.empty()) {
          out.push_back(std::move(cur));
          cur = Contour();
          cur.pts.reserve(std::min<size_t>(pts.size() - pi, 64));
        }
        const Point& p = pts[pi++];
        cur.pts.push_back({p.x, p.y});
        lastx = startx = p.x;
        lasty = starty = p.y;
        break;
      }
      case PathCommand::LineTo: {
        if (pi >= pts.size()) return;
        const Point& p = pts[pi++];
        cur.pts.push_back({p.x, p.y});
        lastx = p.x;
        lasty = p.y;
        break;
      }
      case PathCommand::CubicTo: {
        if (pi + 2 >= pts.size()) return;
        const Point& c1 = pts[pi++];
        const Point& c2 = pts[pi++];
        const Point& p3 = pts[pi++];
        flatten_cubic(lastx, lasty, c1.x, c1.y, c2.x, c2.y, p3.x, p3.y,
                      cur.pts);
        lastx = p3.x;
        lasty = p3.y;
        break;
      }
      case PathCommand::Close: {
        if (!cur.pts.empty()) {
          // Close the contour by appending the start point if not already
          // there.
          const lui_pointf_t& first = cur.pts.front();
          const lui_pointf_t& last  = cur.pts.back();
          if (first.x != last.x || first.y != last.y) {
            cur.pts.push_back(first);
          }
          out.push_back(std::move(cur));
          cur = Contour();
          cur.pts.reserve(std::min<size_t>(pts.size() - pi, 64));
        }
        lastx = startx;
        lasty = starty;
        break;
      }
    }
  }
  if (!cur.pts.empty()) {
    out.push_back(std::move(cur));
  }
}

lui_fill_rule_t to_lui_fill_rule(FillRule r) {
  return r == FillRule::EvenOdd ? LUI_FILL_RULE_EVENODD
                                : LUI_FILL_RULE_NONZERO;
}

lui_line_cap_t to_lui_cap(StrokeCap c) {
  switch (c) {
    case StrokeCap::Round:  return LUI_LINE_CAP_ROUND;
    case StrokeCap::Square: return LUI_LINE_CAP_SQUARE;
    case StrokeCap::Butt:   default: return LUI_LINE_CAP_BUTT;
  }
}

lui_line_join_t to_lui_join(StrokeJoin j) {
  switch (j) {
    case StrokeJoin::Round: return LUI_LINE_JOIN_ROUND;
    case StrokeJoin::Bevel: return LUI_LINE_JOIN_BEVEL;
    case StrokeJoin::Miter: default: return LUI_LINE_JOIN_MITER;
  }
}

lui_canvas_blend_mode_t to_lui_blend(BlendMethod m) {
  switch (m) {
    case BlendMethod::Multiply:   return LUI_CANVAS_BLEND_MULTIPLY;
    case BlendMethod::Screen:     return LUI_CANVAS_BLEND_SCREEN;
    case BlendMethod::Overlay:    return LUI_CANVAS_BLEND_OVERLAY;
    case BlendMethod::Darken:     return LUI_CANVAS_BLEND_DARKEN;
    case BlendMethod::Lighten:    return LUI_CANVAS_BLEND_LIGHTEN;
    case BlendMethod::Difference: return LUI_CANVAS_BLEND_DIFFERENCE;
    case BlendMethod::Exclusion:  return LUI_CANVAS_BLEND_EXCLUSION;
    case BlendMethod::Normal:
    default:                      return LUI_CANVAS_BLEND_SRC_OVER;
  }
}

bool blend_mode_supported(BlendMethod m) {
  switch (m) {
    case BlendMethod::Multiply:
    case BlendMethod::Screen:
    case BlendMethod::Overlay:
    case BlendMethod::Darken:
    case BlendMethod::Lighten:
    case BlendMethod::Difference:
    case BlendMethod::Exclusion:
      return true;
    default:
      return false;
  }
}

// Detect whether (cmds, pts) describe a single axis-aligned rectangle.
// Returns true and fills out_x/y/w/h on success.
bool detect_axis_aligned_rect(const std::vector<PathCommand>& cmds,
                              const std::vector<Point>& pts,
                              float& out_x, float& out_y,
                              float& out_w, float& out_h) {
  std::vector<Point> verts;
  size_t pi = 0;
  for (PathCommand c : cmds) {
    switch (c) {
      case PathCommand::MoveTo:
        if (!verts.empty()) return false;  // multi-contour rejected
        if (pi >= pts.size()) return false;
        verts.push_back(pts[pi++]);
        break;
      case PathCommand::LineTo:
        if (pi >= pts.size()) return false;
        verts.push_back(pts[pi++]);
        break;
      case PathCommand::CubicTo:
        return false;
      case PathCommand::Close:
        break;
    }
  }
  if (verts.size() < 4) return false;
  // Allow a redundant 5th vertex equal to the first (explicit close-as-line).
  if (verts.size() == 5) {
    if (verts[0].x != verts[4].x || verts[0].y != verts[4].y) return false;
  } else if (verts.size() != 4) {
    return false;
  }
  const Point& a = verts[0];
  const Point& b = verts[1];
  const Point& c = verts[2];
  const Point& d = verts[3];
  bool axisA = (a.y == b.y) && (b.x == c.x) && (c.y == d.y) && (d.x == a.x);
  bool axisB = (a.x == b.x) && (b.y == c.y) && (c.x == d.x) && (d.y == a.y);
  if (!axisA && !axisB) return false;
  float minx = std::min({a.x, b.x, c.x, d.x});
  float maxx = std::max({a.x, b.x, c.x, d.x});
  float miny = std::min({a.y, b.y, c.y, d.y});
  float maxy = std::max({a.y, b.y, c.y, d.y});
  out_x = minx;
  out_y = miny;
  out_w = maxx - minx;
  out_h = maxy - miny;
  return out_w > 0 && out_h > 0;
}

// Translate an lvg gradient to a lui_canvas_gradient_t. Returns false on
// empty/invalid input.
bool build_lui_gradient(Paint* grad, lui_canvas_gradient_t& out,
                        uint8_t opacity_mul) {
  if (!grad) return false;
  if (grad->kind() == Paint::kLinear) {
    auto* lg = static_cast<LinearGradient*>(grad);
    int count = std::min<int>(lg->stop_count(), LUI_CANVAS_GRADIENT_MAX_STOPS);
    if (count == 0) return false;
    out.type = LUI_CANVAS_GRADIENT_LINEAR;
    out.x0 = lg->x1(); out.y0 = lg->y1();
    out.x1 = lg->x2(); out.y1 = lg->y2();
    out.cx = 0; out.cy = 0; out.r = 0;
    out.stop_count = count;
    for (int i = 0; i < count; ++i) {
      const Fill::ColorStop& s = lg->stop_at(i);
      uint8_t a = static_cast<uint8_t>((s.a * opacity_mul) / 255);
      out.stops[i].position = s.offset;
      out.stops[i].color    = pack_argb(s.r, s.g, s.b, a);
    }
    return true;
  }
  if (grad->kind() == Paint::kRadial) {
    auto* rg = static_cast<RadialGradient*>(grad);
    // lui_canvas's radial gradient is concentric only (centre + radius).
    // Focal-point radials must go through the per-pixel sample_gradient_at
    // path; signal that here by declining the fast-path build.
    if (rg->has_focal()) return false;
    int count = std::min<int>(rg->stop_count(), LUI_CANVAS_GRADIENT_MAX_STOPS);
    if (count == 0) return false;
    out.type = LUI_CANVAS_GRADIENT_RADIAL;
    out.cx = rg->cx(); out.cy = rg->cy(); out.r = rg->r();
    out.x0 = 0; out.y0 = 0; out.x1 = 0; out.y1 = 0;
    out.stop_count = count;
    for (int i = 0; i < count; ++i) {
      const Fill::ColorStop& s = rg->stop_at(i);
      uint8_t a = static_cast<uint8_t>((s.a * opacity_mul) / 255);
      out.stops[i].position = s.offset;
      out.stops[i].color    = pack_argb(s.r, s.g, s.b, a);
    }
    return true;
  }
  return false;
}

// Separable blend functions per W3C Compositing and Blending Level 1.
// Inputs/outputs are 0-255 channel values (non-premultiplied "straight").
uint8_t blend_separable(uint8_t cb, uint8_t cs, BlendMethod m) {
  switch (m) {
    case BlendMethod::Multiply:   return static_cast<uint8_t>((cb * cs + 127) / 255);
    case BlendMethod::Screen:     return static_cast<uint8_t>(cb + cs - (cb * cs + 127) / 255);
    case BlendMethod::Overlay: {
      // Overlay(Cb, Cs) = HardLight(Cs, Cb) — i.e. branch on Cb.
      if (cb <= 127) return static_cast<uint8_t>((2 * cb * cs + 127) / 255);
      return static_cast<uint8_t>(255 - (2 * (255 - cb) * (255 - cs) + 127) / 255);
    }
    case BlendMethod::Darken:     return std::min(cb, cs);
    case BlendMethod::Lighten:    return std::max(cb, cs);
    case BlendMethod::Difference: return cb > cs ? static_cast<uint8_t>(cb - cs)
                                                  : static_cast<uint8_t>(cs - cb);
    case BlendMethod::Exclusion:  return static_cast<uint8_t>(cb + cs - (2 * cb * cs + 127) / 255);
    case BlendMethod::ColorDodge: {
      if (cs == 255) return 255;
      if (cb == 0)   return 0;
      int v = (cb * 255) / (255 - cs);
      return v > 255 ? static_cast<uint8_t>(255) : static_cast<uint8_t>(v);
    }
    case BlendMethod::ColorBurn: {
      if (cs == 0)   return 0;
      if (cb == 255) return 255;
      int v = ((255 - cb) * 255) / cs;
      return v > 255 ? static_cast<uint8_t>(0) : static_cast<uint8_t>(255 - v);
    }
    case BlendMethod::HardLight: {
      if (cs <= 127) return static_cast<uint8_t>((2 * cb * cs + 127) / 255);
      return static_cast<uint8_t>(255 - (2 * (255 - cb) * (255 - cs) + 127) / 255);
    }
    case BlendMethod::SoftLight: {
      float Cb = cb / 255.0f;
      float Cs = cs / 255.0f;
      float result;
      if (Cs <= 0.5f) {
        result = Cb - (1.0f - 2.0f * Cs) * Cb * (1.0f - Cb);
      } else {
        float d = Cb <= 0.25f
                      ? ((16.0f * Cb - 12.0f) * Cb + 4.0f) * Cb
                      : std::sqrt(Cb);
        result = Cb + (2.0f * Cs - 1.0f) * (d - Cb);
      }
      int v = static_cast<int>(result * 255.0f + 0.5f);
      if (v < 0)   v = 0;
      if (v > 255) v = 255;
      return static_cast<uint8_t>(v);
    }
    case BlendMethod::Normal:
    default:
      return cs;
  }
}

// ---- Non-separable (HSL) blend helpers ----

inline float luma(float r, float g, float b) {
  return 0.3f * r + 0.59f * g + 0.11f * b;
}

inline void clip_color(float& r, float& g, float& b) {
  float l = luma(r, g, b);
  float n = std::min({r, g, b});
  float x = std::max({r, g, b});
  if (n < 0.0f) {
    float k = l / (l - n);
    r = l + (r - l) * k;
    g = l + (g - l) * k;
    b = l + (b - l) * k;
  }
  if (x > 1.0f) {
    float k = (1.0f - l) / (x - l);
    r = l + (r - l) * k;
    g = l + (g - l) * k;
    b = l + (b - l) * k;
  }
}

inline void set_lum(float& r, float& g, float& b, float L) {
  float d = L - luma(r, g, b);
  r += d;
  g += d;
  b += d;
  clip_color(r, g, b);
}

inline float sat_of(float r, float g, float b) {
  return std::max({r, g, b}) - std::min({r, g, b});
}

// Set saturation of (r,g,b) to S, preserving the channel ordering.
inline void set_sat(float& r, float& g, float& b, float S) {
  float* chans[3] = {&r, &g, &b};
  // Sort references by channel value.
  if (*chans[0] > *chans[1]) std::swap(chans[0], chans[1]);
  if (*chans[1] > *chans[2]) std::swap(chans[1], chans[2]);
  if (*chans[0] > *chans[1]) std::swap(chans[0], chans[1]);
  // chans[0] = Cmin, chans[1] = Cmid, chans[2] = Cmax.
  if (*chans[2] > *chans[0]) {
    *chans[1] = ((*chans[1] - *chans[0]) * S) / (*chans[2] - *chans[0]);
    *chans[2] = S;
  } else {
    *chans[1] = 0.0f;
    *chans[2] = 0.0f;
  }
  *chans[0] = 0.0f;
}

// HSL blends. Returns the (r,g,b) result in [0,1] for a single pixel.
void blend_nonseparable(float Cb_r, float Cb_g, float Cb_b,
                        float Cs_r, float Cs_g, float Cs_b, BlendMethod m,
                        float& out_r, float& out_g, float& out_b) {
  switch (m) {
    case BlendMethod::Hue: {
      // SetLum(SetSat(Cs, Sat(Cb)), Lum(Cb))
      out_r = Cs_r; out_g = Cs_g; out_b = Cs_b;
      set_sat(out_r, out_g, out_b, sat_of(Cb_r, Cb_g, Cb_b));
      set_lum(out_r, out_g, out_b, luma(Cb_r, Cb_g, Cb_b));
      return;
    }
    case BlendMethod::Saturation: {
      // SetLum(SetSat(Cb, Sat(Cs)), Lum(Cb))
      out_r = Cb_r; out_g = Cb_g; out_b = Cb_b;
      set_sat(out_r, out_g, out_b, sat_of(Cs_r, Cs_g, Cs_b));
      set_lum(out_r, out_g, out_b, luma(Cb_r, Cb_g, Cb_b));
      return;
    }
    case BlendMethod::Color: {
      // SetLum(Cs, Lum(Cb))
      out_r = Cs_r; out_g = Cs_g; out_b = Cs_b;
      set_lum(out_r, out_g, out_b, luma(Cb_r, Cb_g, Cb_b));
      return;
    }
    case BlendMethod::Luminosity: {
      // SetLum(Cb, Lum(Cs))
      out_r = Cb_r; out_g = Cb_g; out_b = Cb_b;
      set_lum(out_r, out_g, out_b, luma(Cs_r, Cs_g, Cs_b));
      return;
    }
    default:
      out_r = Cs_r;
      out_g = Cs_g;
      out_b = Cs_b;
      return;
  }
}

inline bool is_nonseparable_blend(BlendMethod m) {
  return m == BlendMethod::Hue || m == BlendMethod::Saturation ||
         m == BlendMethod::Color || m == BlendMethod::Luminosity;
}

// Composite a source pixel (straight RGBA) onto a destination pixel (straight
// RGBA) with the chosen blend mode. Both are 0xAARRGGBB packed.
uint32_t composite_pixel(uint32_t dst, uint32_t src, BlendMethod m) {
  uint8_t sa = static_cast<uint8_t>((src >> 24) & 0xff);
  if (sa == 0) return dst;
  uint8_t sr = static_cast<uint8_t>((src >> 16) & 0xff);
  uint8_t sg = static_cast<uint8_t>((src >> 8) & 0xff);
  uint8_t sb = static_cast<uint8_t>(src & 0xff);
  uint8_t da = static_cast<uint8_t>((dst >> 24) & 0xff);
  uint8_t dr = static_cast<uint8_t>((dst >> 16) & 0xff);
  uint8_t dg = static_cast<uint8_t>((dst >> 8) & 0xff);
  uint8_t db = static_cast<uint8_t>(dst & 0xff);

  // Fast path: pure SRC_OVER (the common case). The W3C formula collapses
  // when B(Cb, Cs) = Cs, so co = Cs and we can skip the per-channel
  // blend_separable call entirely.
  if (m == BlendMethod::Normal) {
    if (sa == 255 || da == 0) {
      return (static_cast<uint32_t>(sa) << 24) |
             (static_cast<uint32_t>(sr) << 16) |
             (static_cast<uint32_t>(sg) << 8)  |
              static_cast<uint32_t>(sb);
    }
    uint16_t inv_sa = static_cast<uint16_t>(255 - sa);
    uint8_t a_out = static_cast<uint8_t>(sa + (da * inv_sa) / 255);
    if (a_out == 0) return 0;
    uint32_t num_r = sa * 255u * sr + inv_sa * da * dr;
    uint32_t num_g = sa * 255u * sg + inv_sa * da * dg;
    uint32_t num_b = sa * 255u * sb + inv_sa * da * db;
    uint32_t denom = 255u * a_out;
    uint8_t o_r = static_cast<uint8_t>(num_r / denom);
    uint8_t o_g = static_cast<uint8_t>(num_g / denom);
    uint8_t o_b = static_cast<uint8_t>(num_b / denom);
    return (static_cast<uint32_t>(a_out) << 24) |
           (static_cast<uint32_t>(o_r) << 16) |
           (static_cast<uint32_t>(o_g) << 8)  |
            static_cast<uint32_t>(o_b);
  }

  // Per W3C compositing: co = (1 - αb) × Cs + αb × B(Cb, Cs), then SRC_OVER.
  uint8_t Br, Bg, Bb;
  if (is_nonseparable_blend(m)) {
    float fbr = dr / 255.0f, fbg = dg / 255.0f, fbb = db / 255.0f;
    float fsr = sr / 255.0f, fsg = sg / 255.0f, fsb = sb / 255.0f;
    float orf, ogf, obf;
    blend_nonseparable(fbr, fbg, fbb, fsr, fsg, fsb, m, orf, ogf, obf);
    Br = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, orf * 255.0f + 0.5f)));
    Bg = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, ogf * 255.0f + 0.5f)));
    Bb = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, obf * 255.0f + 0.5f)));
  } else {
    Br = blend_separable(dr, sr, m);
    Bg = blend_separable(dg, sg, m);
    Bb = blend_separable(db, sb, m);
  }

  // co_channel = (255 - αb) × Cs + αb × B, then ÷ 255 to keep in [0,255].
  uint16_t inv_da = static_cast<uint16_t>(255 - da);
  uint8_t co_r = static_cast<uint8_t>((inv_da * sr + da * Br) / 255);
  uint8_t co_g = static_cast<uint8_t>((inv_da * sg + da * Bg) / 255);
  uint8_t co_b = static_cast<uint8_t>((inv_da * sb + da * Bb) / 255);

  // SRC_OVER with co as the source pre-multiplied? We work straight.
  // αo = αs + αb × (1 - αs)
  // Co = (αs × co + (1 - αs) × αb × Cb) / αo
  uint16_t inv_sa = static_cast<uint16_t>(255 - sa);
  uint8_t a_out = static_cast<uint8_t>(sa + (da * inv_sa) / 255);
  if (a_out == 0) return 0;
  // numerator channels in 16-bit space (uint will fit: 255*255 = 65025)
  uint32_t num_r = sa * static_cast<uint32_t>(co_r) +
                   (inv_sa * static_cast<uint32_t>(da) * dr) / 255;
  uint32_t num_g = sa * static_cast<uint32_t>(co_g) +
                   (inv_sa * static_cast<uint32_t>(da) * dg) / 255;
  uint32_t num_b = sa * static_cast<uint32_t>(co_b) +
                   (inv_sa * static_cast<uint32_t>(da) * db) / 255;
  uint8_t o_r = static_cast<uint8_t>(num_r / a_out);
  uint8_t o_g = static_cast<uint8_t>(num_g / a_out);
  uint8_t o_b = static_cast<uint8_t>(num_b / a_out);
  return (static_cast<uint32_t>(a_out) << 24) |
         (static_cast<uint32_t>(o_r) << 16) |
         (static_cast<uint32_t>(o_g) << 8) |
         static_cast<uint32_t>(o_b);
}

// Compute the axis-aligned bounding box of all contour points.
void contours_bbox(const std::vector<Contour>& contours, float& minx,
                   float& miny, float& maxx, float& maxy) {
  minx = miny = std::numeric_limits<float>::infinity();
  maxx = maxy = -std::numeric_limits<float>::infinity();
  for (const Contour& c : contours) {
    for (const lui_pointf_t& p : c.pts) {
      if (p.x < minx) minx = p.x;
      if (p.x > maxx) maxx = p.x;
      if (p.y < miny) miny = p.y;
      if (p.y > maxy) maxy = p.y;
    }
  }
}

// Slice a contour into sub-polylines according to a PDF-style dash pattern
// (alternating on/off lengths in canvas units) and starting `phase` offset.
// Sub-polylines preserve cap/join style when re-drawn through styled_polyline.
// Returns the original contour unchanged when the pattern is empty or has
// zero total length.
std::vector<Contour> apply_dash(const Contour& contour,
                                const std::vector<float>& pattern,
                                float phase) {
  std::vector<Contour> out;
  if (contour.pts.size() < 2) {
    if (!contour.pts.empty()) out.push_back(contour);
    return out;
  }
  float cycle = 0.0f;
  for (float v : pattern) cycle += v;
  if (pattern.empty() || cycle <= 1e-6f) {
    out.push_back(contour);
    return out;
  }

  // Normalise phase into [0, cycle).
  float pos = std::fmod(phase, cycle);
  if (pos < 0.0f) pos += cycle;

  // Locate the pattern entry the phase lands in and how much of it is left.
  size_t pat_idx = 0;
  bool is_on = true;  // PDF dash array always starts with an "on" entry
  float acc = 0.0f;
  while (pat_idx < pattern.size() && acc + pattern[pat_idx] <= pos) {
    acc += pattern[pat_idx];
    ++pat_idx;
    is_on = !is_on;
  }
  if (pat_idx >= pattern.size()) {
    // Shouldn't happen after normalisation; bail to solid.
    out.push_back(contour);
    return out;
  }
  float remaining_in_dash = pattern[pat_idx] - (pos - acc);

  Contour current;
  if (is_on) current.pts.push_back(contour.pts[0]);

  auto advance_pattern = [&]() {
    pat_idx = (pat_idx + 1) % pattern.size();
    is_on = !is_on;
    remaining_in_dash = pattern[pat_idx];
  };

  auto flush_current = [&]() {
    if (current.pts.size() >= 2) out.push_back(std::move(current));
    current = Contour{};
  };

  for (size_t i = 0; i + 1 < contour.pts.size(); ++i) {
    const lui_pointf_t& p0 = contour.pts[i];
    const lui_pointf_t& p1 = contour.pts[i + 1];
    float dx = p1.x - p0.x;
    float dy = p1.y - p0.y;
    float seg_len = std::sqrt(dx * dx + dy * dy);
    if (seg_len < 1e-6f) continue;
    float ux = dx / seg_len;
    float uy = dy / seg_len;
    float seg_remaining = seg_len;
    float seg_consumed = 0.0f;

    while (seg_remaining > 1e-6f) {
      float take = std::min(seg_remaining, remaining_in_dash);
      seg_consumed += take;
      lui_pointf_t pt{p0.x + ux * seg_consumed, p0.y + uy * seg_consumed};

      if (is_on) {
        current.pts.push_back(pt);
      }

      remaining_in_dash -= take;
      seg_remaining     -= take;

      if (remaining_in_dash <= 1e-6f) {
        if (is_on) flush_current();
        else current.pts.clear();
        advance_pattern();
        if (is_on && seg_remaining > 1e-6f) {
          current.pts.push_back(pt);
        }
      }
    }
  }

  if (is_on) flush_current();
  return out;
}

// Rasterize a sequence of contours to an 8-bit alpha mask of `bbox_w` × `bbox_h`.
// Contours are translated so (bbox_x, bbox_y) becomes (0, 0) before rasterizing.
std::vector<uint8_t> rasterize_contours_to_mask(
    const std::vector<Contour>& contours, int bbox_x, int bbox_y,
    int bbox_w, int bbox_h, FillRule rule) {
  std::vector<uint8_t> mask(static_cast<size_t>(bbox_w) * bbox_h, 0u);
  if (bbox_w <= 0 || bbox_h <= 0) return mask;
  std::vector<uint32_t> pixels(static_cast<size_t>(bbox_w) * bbox_h, 0u);
  lui_surface_t s = lui_surface_wrap(pixels.data(), bbox_w, bbox_h, bbox_w);
  lui_canvas_t  c;
  lui_canvas_init(&c, &s);
  for (Contour contour : contours) {
    if (contour.pts.size() < 3) continue;
    for (lui_pointf_t& p : contour.pts) {
      p.x -= static_cast<float>(bbox_x);
      p.y -= static_cast<float>(bbox_y);
    }
    lui_canvas_fill_polygonf_ex(&c, contour.pts.data(),
                                static_cast<int>(contour.pts.size()),
                                0xFFFFFFFFu, to_lui_fill_rule(rule));
  }
  lui_canvas_destroy(&c);
  for (size_t i = 0; i < mask.size(); ++i) {
    mask[i] = static_cast<uint8_t>((pixels[i] >> 24) & 0xff);
  }
  return mask;
}

// Fill polygon contours with a per-pixel source colour and arbitrary blend
// mode. Coverage from the polygon's alpha-AA rasterization is multiplied
// into the source alpha before compositing.
//
// @color_at returns the 0xAARRGGBB source colour at canvas pixel (x, y).
template <typename ColorFn>
void fill_polygon_with_source(lui_canvas_t* canvas,
                              const std::vector<Contour>& contours,
                              FillRule rule, ColorFn color_at,
                              BlendMethod blend_mode) {
  if (!canvas || contours.empty()) return;
  lui_surface_t* dst = lui_canvas_get_surface(canvas);
  if (!dst || !dst->pixels) return;

  // Compute bbox of all contours, clip to canvas clip rect.
  float minx, miny, maxx, maxy;
  contours_bbox(contours, minx, miny, maxx, maxy);
  lui_rect_t clip = lui_canvas_get_clip(canvas);
  int x0 = std::max(static_cast<int>(std::floor(minx)), clip.x);
  int y0 = std::max(static_cast<int>(std::floor(miny)), clip.y);
  int x1 = std::min(static_cast<int>(std::ceil(maxx)),  clip.x + clip.width);
  int y1 = std::min(static_cast<int>(std::ceil(maxy)),  clip.y + clip.height);
  if (x0 >= x1 || y0 >= y1) return;

  int bbox_w = x1 - x0;
  int bbox_h = y1 - y0;
  std::vector<uint8_t> mask =
      rasterize_contours_to_mask(contours, x0, y0, bbox_w, bbox_h, rule);

  for (int y = 0; y < bbox_h; ++y) {
    int canvas_y = y0 + y;
    uint32_t* dst_row = dst->pixels + canvas_y * dst->stride;
    const uint8_t* mask_row = mask.data() + static_cast<size_t>(y) * bbox_w;
    for (int x = 0; x < bbox_w; ++x) {
      uint8_t m = mask_row[x];
      if (m == 0) continue;
      int canvas_x = x0 + x;
      uint32_t src = color_at(canvas_x, canvas_y);
      // Multiply src alpha by coverage.
      uint32_t sa  = (src >> 24) & 0xff;
      if (sa == 0) continue;
      sa = (sa * m + 127) / 255;
      if (sa == 0) continue;
      uint32_t src_with_cov = (src & 0x00ffffffu) | (sa << 24);
      dst_row[canvas_x] =
          composite_pixel(dst_row[canvas_x], src_with_cov, blend_mode);
    }
  }
}

}  // namespace (close the file-local anonymous namespace so the next
   // function matches its lvg:: declaration in the header)

// Sample an lvg gradient at (canvas_x, canvas_y). Returns 0xAARRGGBB.
uint32_t sample_gradient_at(const Paint* grad, float x, float y,
                            uint8_t opacity_mul) {
  if (!grad) return 0;
  uint8_t r = 0, g = 0, b = 0, a = 0;
  if (grad->kind() == Paint::kLinear) {
    const auto* lg = static_cast<const LinearGradient*>(grad);
    float dx = lg->x2() - lg->x1();
    float dy = lg->y2() - lg->y1();
    float len2 = dx * dx + dy * dy;
    float t = 0.5f;
    if (len2 > 1e-8f) {
      t = ((x - lg->x1()) * dx + (y - lg->y1()) * dy) / len2;
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    lg->sample(t, r, g, b, a);
  } else if (grad->kind() == Paint::kRadial) {
    const auto* rg = static_cast<const RadialGradient*>(grad);
    float t;
    if (!rg->has_focal()) {
      // Concentric radial: t = distance from centre / radius.
      float dx = x - rg->cx();
      float dy = y - rg->cy();
      float d  = std::sqrt(dx * dx + dy * dy);
      t = rg->r() > 1e-8f ? d / rg->r() : 1.0f;
    } else {
      // PDF Shading Type 3 / SVG focal radial: the gradient is a family of
      // circles interpolated between focal (fx,fy,fr) at t=0 and centre
      // (cx,cy,r) at t=1. We solve for the t whose circle contains P,
      // picking the larger valid root (later in the gradient progression):
      //   Centre(t) = F + t*(C-F),  Radius(t) = Fr + t*(r-Fr)
      //   |P - Centre(t)|^2 = Radius(t)^2
      // → A*t^2 - 2*B*t + C = 0 with
      //   A = |D|^2 - Dr^2, B = PF·D + Fr*Dr, C = |PF|^2 - Fr^2
      float pfx = x - rg->fx();
      float pfy = y - rg->fy();
      float dx  = rg->cx() - rg->fx();
      float dy  = rg->cy() - rg->fy();
      float dr  = rg->r()  - rg->fr();
      float A = dx * dx + dy * dy - dr * dr;
      float B = pfx * dx + pfy * dy + rg->fr() * dr;
      float C = pfx * pfx + pfy * pfy - rg->fr() * rg->fr();
      if (std::fabs(A) < 1e-12f) {
        // Degenerate: focal and outer circles have equal radius; quadratic
        // collapses to -2*B*t + C = 0.
        t = std::fabs(B) > 1e-12f ? C / (2.0f * B) : 1.0f;
      } else {
        float disc = B * B - A * C;
        if (disc < 0.0f) {
          // P is outside the two-circle family. Pad to the gradient end.
          t = 1.0f;
        } else {
          float sq = std::sqrt(disc);
          // For A > 0 (typical: outer is larger than focal in expansion
          // direction), the larger root is the one we want. For A < 0, the
          // family is "shrinking" and signs flip; the heuristic of picking
          // the root with non-negative interpolated radius and larger t
          // covers both cases.
          float t1 = (B + sq) / A;
          float t2 = (B - sq) / A;
          float r1 = rg->fr() + t1 * dr;
          float r2 = rg->fr() + t2 * dr;
          bool r1_ok = (r1 >= 0.0f);
          bool r2_ok = (r2 >= 0.0f);
          if (r1_ok && r2_ok) t = std::max(t1, t2);
          else if (r1_ok)     t = t1;
          else if (r2_ok)     t = t2;
          else                t = 1.0f;
        }
      }
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    rg->sample(t, r, g, b, a);
  }
  if (opacity_mul < 255) a = static_cast<uint8_t>((a * opacity_mul) / 255);
  return pack_argb(r, g, b, a);
}

namespace {  // reopen for the remaining file-local helpers

// Rasterize an arbitrary path to an 8-bit alpha mask of `bbox_w` × `bbox_h`.
// Path is translated so (bbox_x, bbox_y) becomes (0, 0) before rasterizing.
std::vector<uint8_t> rasterize_path_to_mask(
    const std::vector<PathCommand>& cmds, const std::vector<Point>& pts,
    int bbox_x, int bbox_y, int bbox_w, int bbox_h, FillRule rule) {
  std::vector<uint8_t> mask(static_cast<size_t>(bbox_w) * bbox_h, 0u);
  if (bbox_w <= 0 || bbox_h <= 0) return mask;

  std::vector<uint32_t> pixels(static_cast<size_t>(bbox_w) * bbox_h, 0u);
  lui_surface_t s = lui_surface_wrap(pixels.data(), bbox_w, bbox_h, bbox_w);
  lui_canvas_t  c;
  lui_canvas_init(&c, &s);

  std::vector<Contour> contours;
  flatten_path(cmds, pts, contours);
  for (Contour& contour : contours) {
    if (contour.pts.size() < 3) continue;
    for (lui_pointf_t& p : contour.pts) {
      p.x -= static_cast<float>(bbox_x);
      p.y -= static_cast<float>(bbox_y);
    }
    lui_canvas_fill_polygonf_ex(&c, contour.pts.data(),
                                static_cast<int>(contour.pts.size()),
                                0xFFFFFFFFu, to_lui_fill_rule(rule));
  }
  lui_canvas_destroy(&c);

  // Extract alpha channel.
  for (size_t i = 0; i < mask.size(); ++i) {
    mask[i] = static_cast<uint8_t>((pixels[i] >> 24) & 0xff);
  }
  return mask;
}

}  // namespace

// =========================================================================
// Paint
// =========================================================================
void Paint::unref() { delete this; }

Result Paint::clip(Shape* clipper) {
  if (clipper_) delete clipper_;
  clipper_ = clipper;
  return Result::Success;
}

Result Paint::bounds(float* x, float* y, float* w, float* h,
                     bool /*transformed*/) const {
  if (kind() == kShape) {
    static_cast<const Shape*>(this)->compute_bbox(x, y, w, h);
    return Result::Success;
  }
  if (kind() == kPicture) {
    const Picture* pic = static_cast<const Picture*>(this);
    pic->compute_bbox(x, y, w, h);
    return Result::Success;
  }
  if (kind() == kScene) {
    const Scene* sc = static_cast<const Scene*>(this);
    sc->compute_bbox(x, y, w, h);
    return Result::Success;
  }
  if (x) *x = 0;
  if (y) *y = 0;
  if (w) *w = 0;
  if (h) *h = 0;
  return Result::Success;
}

// Snapshot-then-blend pattern for per-pixel soft masking. Captures pixels in
// [x0..x1) x [y0..y1) (clamped to canvas clip), runs `draw`, then lerps each
// pixel back using the mask: result = lerp(before, after, mask[px,py]).
// Untouched pixels lerp identity → no change. The bbox can intentionally be
// loose (e.g. full clip rect for scenes), since identity-lerping unmodified
// pixels is a no-op.
template <typename DrawFn>
static void draw_with_soft_mask(lui_canvas_t* canvas,
                                const SoftMaskData& mask,
                                int x0, int y0, int x1, int y1,
                                DrawFn draw) {
  lui_rect_t clip = lui_canvas_get_clip(canvas);
  x0 = std::max(x0, clip.x);
  y0 = std::max(y0, clip.y);
  x1 = std::min(x1, clip.x + clip.width);
  y1 = std::min(y1, clip.y + clip.height);
  if (x0 >= x1 || y0 >= y1) {
    draw(canvas);
    return;
  }
  lui_surface_t* surf = lui_canvas_get_surface(canvas);
  if (!surf || !surf->pixels) {
    draw(canvas);
    return;
  }
  int w = x1 - x0;
  int h = y1 - y0;
  std::vector<uint32_t> snapshot(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const uint32_t* src_row = surf->pixels + (y0 + y) * surf->stride + x0;
    std::memcpy(snapshot.data() + static_cast<size_t>(y) * w, src_row,
                static_cast<size_t>(w) * sizeof(uint32_t));
  }

  draw(canvas);

  for (int y = 0; y < h; ++y) {
    int my = y0 + y;
    if (my < 0 || static_cast<uint32_t>(my) >= mask.h) continue;
    const uint8_t* mask_row = mask.data.data() +
                              static_cast<size_t>(my) * mask.w;
    uint32_t* dst_row = surf->pixels + (y0 + y) * surf->stride + x0;
    const uint32_t* snap_row = snapshot.data() + static_cast<size_t>(y) * w;
    for (int x = 0; x < w; ++x) {
      int mx = x0 + x;
      if (mx < 0 || static_cast<uint32_t>(mx) >= mask.w) continue;
      uint8_t a = mask_row[mx];
      if (a == 255) continue;
      uint32_t before = snap_row[x];
      if (a == 0) { dst_row[x] = before; continue; }
      uint32_t after = dst_row[x];
      // Per-channel lerp: out = before + (after - before) * a / 255.
      uint16_t inv = static_cast<uint16_t>(255 - a);
      uint8_t ab = static_cast<uint8_t>((before >> 24) & 0xff);
      uint8_t rb = static_cast<uint8_t>((before >> 16) & 0xff);
      uint8_t gb = static_cast<uint8_t>((before >> 8)  & 0xff);
      uint8_t bb = static_cast<uint8_t>(before & 0xff);
      uint8_t aa = static_cast<uint8_t>((after >> 24) & 0xff);
      uint8_t ra = static_cast<uint8_t>((after >> 16) & 0xff);
      uint8_t ga = static_cast<uint8_t>((after >> 8)  & 0xff);
      uint8_t ba = static_cast<uint8_t>(after & 0xff);
      uint8_t ao = static_cast<uint8_t>((ab * inv + aa * a) / 255);
      uint8_t ro = static_cast<uint8_t>((rb * inv + ra * a) / 255);
      uint8_t go = static_cast<uint8_t>((gb * inv + ga * a) / 255);
      uint8_t bo = static_cast<uint8_t>((bb * inv + ba * a) / 255);
      dst_row[x] = (static_cast<uint32_t>(ao) << 24) |
                   (static_cast<uint32_t>(ro) << 16) |
                   (static_cast<uint32_t>(go) << 8) |
                    static_cast<uint32_t>(bo);
    }
  }
}

// =========================================================================
// Shape
// =========================================================================
Shape* Shape::gen() { return new Shape(); }

Shape::~Shape() {
  if (fill_gradient_)   delete fill_gradient_;
  if (stroke_gradient_) delete stroke_gradient_;
  if (clipper_)         delete clipper_;
}

Result Shape::moveTo(float x, float y) {
  cmds_.push_back(PathCommand::MoveTo);
  pts_.push_back({x, y});
  pen_ = {x, y};
  return Result::Success;
}

Result Shape::lineTo(float x, float y) {
  cmds_.push_back(PathCommand::LineTo);
  pts_.push_back({x, y});
  pen_ = {x, y};
  return Result::Success;
}

Result Shape::cubicTo(float c1x, float c1y, float c2x, float c2y, float ex,
                      float ey) {
  cmds_.push_back(PathCommand::CubicTo);
  pts_.push_back({c1x, c1y});
  pts_.push_back({c2x, c2y});
  pts_.push_back({ex, ey});
  pen_ = {ex, ey};
  return Result::Success;
}

Result Shape::close() {
  cmds_.push_back(PathCommand::Close);
  return Result::Success;
}

Result Shape::appendRect(float x, float y, float w, float h, float /*rx*/,
                         float /*ry*/) {
  cmds_.reserve(cmds_.size() + 5);
  pts_.reserve(pts_.size() + 4);
  moveTo(x, y);
  lineTo(x + w, y);
  lineTo(x + w, y + h);
  lineTo(x, y + h);
  close();
  return Result::Success;
}

Result Shape::appendCircle(float cx, float cy, float rx, float ry) {
  // Approximate an ellipse with 4 cubic Bezier segments (Kappa = 0.5522847).
  constexpr float K = 0.5522847498307933f;
  cmds_.reserve(cmds_.size() + 6);
  pts_.reserve(pts_.size() + 13);
  float kx = rx * K;
  float ky = ry * K;
  moveTo(cx, cy - ry);
  cubicTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
  cubicTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
  cubicTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
  cubicTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
  close();
  return Result::Success;
}

Result Shape::appendPath(const PathCommand* cmds, uint32_t cmd_cnt,
                         const Point* pts, uint32_t pt_cnt) {
  cmds_.reserve(cmds_.size() + cmd_cnt);
  for (uint32_t i = 0; i < cmd_cnt; ++i) cmds_.push_back(cmds[i]);
  pts_.reserve(pts_.size() + pt_cnt);
  for (uint32_t i = 0; i < pt_cnt; ++i) pts_.push_back(pts[i]);
  return Result::Success;
}

Result Shape::fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  fill_r_ = r;
  fill_g_ = g;
  fill_b_ = b;
  fill_a_ = a;
  has_solid_fill_ = true;
  if (fill_gradient_) {
    delete fill_gradient_;
    fill_gradient_ = nullptr;
  }
  return Result::Success;
}

Result Shape::fill(LinearGradient* g) {
  if (fill_gradient_) delete fill_gradient_;
  fill_gradient_ = g;
  has_solid_fill_ = false;
  return Result::Success;
}

Result Shape::fill(RadialGradient* g) {
  if (fill_gradient_) delete fill_gradient_;
  fill_gradient_ = g;
  has_solid_fill_ = false;
  return Result::Success;
}

Result Shape::strokeFill(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  stroke_r_ = r;
  stroke_g_ = g;
  stroke_b_ = b;
  stroke_a_ = a;
  has_stroke_color_ = true;
  if (stroke_gradient_) {
    delete stroke_gradient_;
    stroke_gradient_ = nullptr;
  }
  return Result::Success;
}

Result Shape::strokeFill(LinearGradient* g) {
  if (stroke_gradient_) delete stroke_gradient_;
  stroke_gradient_ = g;
  has_stroke_color_ = false;
  return Result::Success;
}

Result Shape::strokeFill(RadialGradient* g) {
  if (stroke_gradient_) delete stroke_gradient_;
  stroke_gradient_ = g;
  has_stroke_color_ = false;
  return Result::Success;
}

Result Shape::strokeDash(const float* pattern, uint32_t count, float phase) {
  stroke_dash_.assign(pattern, pattern + count);
  stroke_dash_phase_ = phase;
  return Result::Success;
}

void Shape::compute_bbox(float* x, float* y, float* w, float* h) const {
  if (pts_.empty()) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    if (h) *h = 0;
    return;
  }
  float minx = std::numeric_limits<float>::infinity();
  float maxx = -std::numeric_limits<float>::infinity();
  float miny = minx;
  float maxy = maxx;
  for (const Point& p : pts_) {
    if (p.x < minx) minx = p.x;
    if (p.x > maxx) maxx = p.x;
    if (p.y < miny) miny = p.y;
    if (p.y > maxy) maxy = p.y;
  }
  if (x) *x = minx;
  if (y) *y = miny;
  if (w) *w = maxx - minx;
  if (h) *h = maxy - miny;
}

void Shape::draw_on(lui_canvas_t* canvas) {
  if (!canvas || cmds_.empty()) return;

  // Per-pixel soft mask: snapshot dest, draw without mask, lerp back through
  // the mask. Reset member to avoid infinite recursion in the lambda.
  if (soft_mask_) {
    auto mask = soft_mask_;
    soft_mask_.reset();
    float bx, by, bw, bh;
    compute_bbox(&bx, &by, &bw, &bh);
    int x0 = static_cast<int>(std::floor(bx));
    int y0 = static_cast<int>(std::floor(by));
    int x1 = static_cast<int>(std::ceil(bx + bw));
    int y1 = static_cast<int>(std::ceil(by + bh));
    draw_with_soft_mask(canvas, *mask, x0, y0, x1, y1,
                        [this](lui_canvas_t* c) { draw_on(c); });
    soft_mask_ = mask;
    return;
  }

  // -- Set up clipping. Three modes:
  //    0 = no clip change
  //    1 = exact rect clip (clipper is an axis-aligned rectangle)
  //    2 = mask-based clip (arbitrary clip path; rasterize to alpha mask,
  //        snapshot dest, draw, composite back)
  lui_rect_t saved_clip = lui_canvas_get_clip(canvas);
  int clip_mode = 0;
  lui_rect_t clip_bbox{0, 0, 0, 0};
  if (clipper_) {
    float crx, cry, crw, crh;
    if (detect_axis_aligned_rect(clipper_->cmds_, clipper_->pts_, crx, cry,
                                 crw, crh)) {
      int x0 = std::max(static_cast<int>(std::floor(crx)), saved_clip.x);
      int y0 = std::max(static_cast<int>(std::floor(cry)), saved_clip.y);
      int x1 = std::min(static_cast<int>(std::ceil(crx + crw)),
                        saved_clip.x + saved_clip.width);
      int y1 = std::min(static_cast<int>(std::ceil(cry + crh)),
                        saved_clip.y + saved_clip.height);
      clip_bbox.x = x0;
      clip_bbox.y = y0;
      clip_bbox.width  = std::max(0, x1 - x0);
      clip_bbox.height = std::max(0, y1 - y0);
      if (clip_bbox.width == 0 || clip_bbox.height == 0) return;
      lui_canvas_set_clip(canvas, &clip_bbox);
      clip_mode = 1;
    } else {
      float cx, cy, cw, ch;
      clipper_->compute_bbox(&cx, &cy, &cw, &ch);
      int x0 = std::max(static_cast<int>(std::floor(cx)), saved_clip.x);
      int y0 = std::max(static_cast<int>(std::floor(cy)), saved_clip.y);
      int x1 = std::min(static_cast<int>(std::ceil(cx + cw)),
                        saved_clip.x + saved_clip.width);
      int y1 = std::min(static_cast<int>(std::ceil(cy + ch)),
                        saved_clip.y + saved_clip.height);
      clip_bbox.x = x0;
      clip_bbox.y = y0;
      clip_bbox.width  = std::max(0, x1 - x0);
      clip_bbox.height = std::max(0, y1 - y0);
      if (clip_bbox.width == 0 || clip_bbox.height == 0) return;
      lui_canvas_set_clip(canvas, &clip_bbox);
      clip_mode = 2;
    }
  }

  // For mask-based clip: snapshot dest pixels, rasterize mask, draw normally,
  // composite back at the end.
  std::vector<uint32_t> snapshot;
  std::vector<uint8_t>  mask;
  if (clip_mode == 2) {
    lui_surface_t* dst = lui_canvas_get_surface(canvas);
    if (dst && dst->pixels) {
      snapshot.resize(static_cast<size_t>(clip_bbox.width) * clip_bbox.height);
      for (int y = 0; y < clip_bbox.height; ++y) {
        const uint32_t* row = dst->pixels +
            (clip_bbox.y + y) * dst->stride + clip_bbox.x;
        std::memcpy(snapshot.data() + static_cast<size_t>(y) * clip_bbox.width,
                    row, static_cast<size_t>(clip_bbox.width) * sizeof(uint32_t));
      }
    }
    mask = rasterize_path_to_mask(clipper_->cmds_, clipper_->pts_,
                                  clip_bbox.x, clip_bbox.y,
                                  clip_bbox.width, clip_bbox.height,
                                  clipper_->fill_rule_);
  }

  // -- Detect axis-aligned rect for the fast paths.
  float rx, ry, rw, rh;
  bool is_rect = detect_axis_aligned_rect(cmds_, pts_, rx, ry, rw, rh);

  // -- Resolve fill state.
  bool fill_active = (has_solid_fill_ && fill_a_ > 0) || fill_gradient_;
  bool fill_handled = false;

  if (fill_active && is_rect) {
    int ix = static_cast<int>(std::floor(rx));
    int iy = static_cast<int>(std::floor(ry));
    int iw = static_cast<int>(std::ceil(rx + rw)) - ix;
    int ih = static_cast<int>(std::ceil(ry + rh)) - iy;
    if (iw > 0 && ih > 0) {
      // Gradient fast path is Normal-blend-only. Non-Normal blends with a
      // gradient fill on a rect fall through to the polygon-custom path.
      if (fill_gradient_ && blend_mode_ == BlendMethod::Normal) {
        lui_canvas_gradient_t lg{};
        if (build_lui_gradient(fill_gradient_, lg, opacity_)) {
          lui_canvas_fill_rect_gradient(canvas, ix, iy, iw, ih, &lg);
          fill_handled = true;
        }
      } else if (has_solid_fill_) {
        uint8_t fa = fill_a_;
        if (opacity_ < 255) fa = static_cast<uint8_t>((fa * opacity_) / 255);
        if (fa > 0) {
          uint32_t color = pack_argb(fill_r_, fill_g_, fill_b_, fa);
          if (blend_mode_ == BlendMethod::Normal) {
            lui_canvas_fill_rect(canvas, ix, iy, iw, ih, color);
            fill_handled = true;
          } else if (blend_mode_supported(blend_mode_)) {
            lui_canvas_fill_rect_blended(canvas, ix, iy, iw, ih, color,
                                         to_lui_blend(blend_mode_));
            fill_handled = true;
          }
        }
      }
    }
  }

  // -- Generic (non-rect) fill path: flatten + polygon fill.
  std::vector<Contour> contours;  // shared by fill + stroke
  bool contours_built = false;

  if (fill_active && !fill_handled) {
    flatten_path(cmds_, pts_, contours);
    contours_built = true;

    // Pick the right rendering path:
    //   * gradient OR non-Normal blend → per-pixel custom path
    //   * solid + Normal               → lui_canvas_fill_polygonf_ex (fast)
    bool needs_custom = fill_gradient_ ||
                        (blend_mode_ != BlendMethod::Normal);

    if (needs_custom) {
      if (fill_gradient_) {
        Paint* g = fill_gradient_;
        uint8_t opc = opacity_;
        BlendMethod bm = blend_mode_;
        fill_polygon_with_source(
            canvas, contours, fill_rule_,
            [g, opc](int x, int y) {
              return sample_gradient_at(g, static_cast<float>(x) + 0.5f,
                                        static_cast<float>(y) + 0.5f, opc);
            },
            bm);
      } else if (has_solid_fill_) {
        uint8_t fa = fill_a_;
        if (opacity_ < 255) fa = static_cast<uint8_t>((fa * opacity_) / 255);
        if (fa > 0) {
          uint32_t color = pack_argb(fill_r_, fill_g_, fill_b_, fa);
          BlendMethod bm = blend_mode_;
          fill_polygon_with_source(
              canvas, contours, fill_rule_,
              [color](int /*x*/, int /*y*/) { return color; }, bm);
        }
      }
    } else if (has_solid_fill_) {
      uint8_t fa = fill_a_;
      if (opacity_ < 255) fa = static_cast<uint8_t>((fa * opacity_) / 255);
      if (fa > 0) {
        uint32_t color = pack_argb(fill_r_, fill_g_, fill_b_, fa);
        for (const Contour& c : contours) {
          if (c.pts.size() >= 3) {
            lui_canvas_fill_polygonf_ex(canvas, c.pts.data(),
                                        static_cast<int>(c.pts.size()), color,
                                        to_lui_fill_rule(fill_rule_));
          }
        }
      }
    }
  }

  // -- Stroke.
  if (has_stroke_ && (has_stroke_color_ || stroke_gradient_)) {
    if (!contours_built) {
      flatten_path(cmds_, pts_, contours);
      contours_built = true;
    }
    float sw = stroke_width_ > 0.5f ? stroke_width_ : 0.5f;
    auto to_cap  = to_lui_cap(stroke_cap_);
    auto to_join = to_lui_join(stroke_join_);

    if (stroke_gradient_) {
      // Per-pixel gradient stroke: rasterize the stroke into a temp ARGB
      // buffer (white-on-clear), then composite onto the canvas sampling
      // the gradient at each pixel's canvas position.
      lui_rect_t clip = lui_canvas_get_clip(canvas);
      float minx = std::numeric_limits<float>::infinity();
      float miny = std::numeric_limits<float>::infinity();
      float maxx = -std::numeric_limits<float>::infinity();
      float maxy = -std::numeric_limits<float>::infinity();
      for (const Contour& c : contours) {
        for (const lui_pointf_t& p : c.pts) {
          if (p.x < minx) minx = p.x;
          if (p.x > maxx) maxx = p.x;
          if (p.y < miny) miny = p.y;
          if (p.y > maxy) maxy = p.y;
        }
      }
      if (minx <= maxx && miny <= maxy) {
        float pad = sw * 0.5f + 1.5f;
        int x0 = std::max(static_cast<int>(std::floor(minx - pad)), clip.x);
        int y0 = std::max(static_cast<int>(std::floor(miny - pad)), clip.y);
        int x1 = std::min(static_cast<int>(std::ceil(maxx + pad)),
                          clip.x + clip.width);
        int y1 = std::min(static_cast<int>(std::ceil(maxy + pad)),
                          clip.y + clip.height);
        int tw = x1 - x0;
        int th = y1 - y0;
        if (tw > 0 && th > 0) {
          std::vector<uint32_t> tmp(static_cast<size_t>(tw) * th, 0u);
          lui_surface_t ts = lui_surface_wrap(tmp.data(), tw, th, tw);
          lui_canvas_t  tc;
          lui_canvas_init(&tc, &ts);
          for (Contour c : contours) {
            if (c.pts.size() < 2) continue;
            for (lui_pointf_t& p : c.pts) {
              p.x -= static_cast<float>(x0);
              p.y -= static_cast<float>(y0);
            }
            if (!stroke_dash_.empty()) {
              auto subs = apply_dash(c, stroke_dash_, stroke_dash_phase_);
              for (const Contour& sc : subs) {
                if (sc.pts.size() < 2) continue;
                lui_canvas_draw_styled_polyline(
                    &tc, sc.pts.data(), static_cast<int>(sc.pts.size()),
                    0xFFFFFFFFu, sw, /*closed=*/false, to_cap, to_join);
              }
              continue;
            }
            bool closed = false;
            if (c.pts.size() >= 3) {
              const lui_pointf_t& f = c.pts.front();
              const lui_pointf_t& l = c.pts.back();
              closed = (f.x == l.x && f.y == l.y);
            }
            lui_canvas_draw_styled_polyline(
                &tc, c.pts.data(), static_cast<int>(c.pts.size()),
                0xFFFFFFFFu, sw, closed, to_cap, to_join);
          }
          // Composite temp onto canvas with per-pixel gradient color.
          lui_surface_t* dst = lui_canvas_get_surface(canvas);
          if (dst && dst->pixels) {
            for (int y = 0; y < th; ++y) {
              uint32_t* dst_row = dst->pixels + (y0 + y) * dst->stride + x0;
              const uint32_t* src_row = tmp.data() +
                                        static_cast<size_t>(y) * tw;
              for (int x = 0; x < tw; ++x) {
                uint8_t cov = static_cast<uint8_t>((src_row[x] >> 24) & 0xff);
                if (cov == 0) continue;
                uint32_t grad_argb = sample_gradient_at(
                    stroke_gradient_,
                    static_cast<float>(x0 + x) + 0.5f,
                    static_cast<float>(y0 + y) + 0.5f, opacity_);
                uint8_t ga = static_cast<uint8_t>((grad_argb >> 24) & 0xff);
                if (ga == 0) continue;
                uint8_t fa = static_cast<uint8_t>((ga * cov) / 255);
                if (fa == 0) continue;
                uint32_t src = (grad_argb & 0x00ffffffu) |
                               (static_cast<uint32_t>(fa) << 24);
                dst_row[x] = composite_pixel(dst_row[x], src,
                                             blend_mode_);
              }
            }
          }
          lui_canvas_destroy(&tc);
        }
      }
    } else {
      uint8_t sr = stroke_r_, sg = stroke_g_, sb = stroke_b_, sa = stroke_a_;
      if (opacity_ < 255) sa = static_cast<uint8_t>((sa * opacity_) / 255);
      if (sa > 0) {
        uint32_t color = pack_argb(sr, sg, sb, sa);
        for (const Contour& c : contours) {
          if (c.pts.size() < 2) continue;
          // Dashed stroke: slice contour by the dash pattern, then draw each
          // "on" sub-polyline. Dashing always breaks contour closure.
          if (!stroke_dash_.empty()) {
            auto subs = apply_dash(c, stroke_dash_, stroke_dash_phase_);
            for (const Contour& sc : subs) {
              if (sc.pts.size() < 2) continue;
              lui_canvas_draw_styled_polyline(
                  canvas, sc.pts.data(), static_cast<int>(sc.pts.size()),
                  color, sw, /*closed=*/false, to_cap, to_join);
            }
            continue;
          }
          bool closed = false;
          if (c.pts.size() >= 3) {
            const lui_pointf_t& f = c.pts.front();
            const lui_pointf_t& l = c.pts.back();
            closed = (f.x == l.x && f.y == l.y);
          }
          lui_canvas_draw_styled_polyline(
              canvas, c.pts.data(), static_cast<int>(c.pts.size()), color,
              sw, closed, to_cap, to_join);
        }
      }
    }
  }

  // -- Mask-based clip: composite drawn pixels with snapshot via mask.
  if (clip_mode == 2 && !mask.empty() && !snapshot.empty()) {
    lui_surface_t* dst = lui_canvas_get_surface(canvas);
    if (dst && dst->pixels) {
      for (int y = 0; y < clip_bbox.height; ++y) {
        uint32_t* dst_row = dst->pixels +
            (clip_bbox.y + y) * dst->stride + clip_bbox.x;
        const uint32_t* snap_row = snapshot.data() +
            static_cast<size_t>(y) * clip_bbox.width;
        const uint8_t*  mask_row = mask.data() +
            static_cast<size_t>(y) * clip_bbox.width;
        for (int x = 0; x < clip_bbox.width; ++x) {
          uint8_t m = mask_row[x];
          if (m == 255) continue;
          if (m == 0) {
            dst_row[x] = snap_row[x];
            continue;
          }
          uint32_t snap = snap_row[x];
          uint32_t curr = dst_row[x];
          uint16_t inv = static_cast<uint16_t>(255 - m);
          uint8_t sa = static_cast<uint8_t>((snap >> 24) & 0xff);
          uint8_t sr = static_cast<uint8_t>((snap >> 16) & 0xff);
          uint8_t sg = static_cast<uint8_t>((snap >> 8) & 0xff);
          uint8_t sb = static_cast<uint8_t>(snap & 0xff);
          uint8_t ca = static_cast<uint8_t>((curr >> 24) & 0xff);
          uint8_t cr = static_cast<uint8_t>((curr >> 16) & 0xff);
          uint8_t cg = static_cast<uint8_t>((curr >> 8) & 0xff);
          uint8_t cb = static_cast<uint8_t>(curr & 0xff);
          uint8_t oa = static_cast<uint8_t>((ca * m + sa * inv) / 255);
          uint8_t orr = static_cast<uint8_t>((cr * m + sr * inv) / 255);
          uint8_t og = static_cast<uint8_t>((cg * m + sg * inv) / 255);
          uint8_t ob = static_cast<uint8_t>((cb * m + sb * inv) / 255);
          dst_row[x] = pack_argb(orr, og, ob, oa);
        }
      }
    }
  }

  if (clip_mode != 0) {
    lui_canvas_set_clip(canvas, &saved_clip);
  }
}

// =========================================================================
// Picture
// =========================================================================
Picture* Picture::gen() { return new Picture(); }
Picture::~Picture() {
  if (clipper_) delete clipper_;
}

static bool pixels_are_opaque(const uint32_t* pixels, size_t count) {
  if (!pixels) return false;
  for (size_t i = 0; i < count; ++i) {
    if ((pixels[i] >> 24) != 255u) return false;
  }
  return true;
}

static void draw_opaque_nearest_image(lui_canvas_t* canvas,
                                      const uint32_t* pixels,
                                      int src_w, int src_h,
                                      int dst_x, int dst_y,
                                      int dst_w, int dst_h) {
  if (!canvas || !pixels || src_w <= 0 || src_h <= 0 ||
      dst_w <= 0 || dst_h <= 0) {
    return;
  }

  lui_rect_t clip = lui_canvas_get_clip(canvas);
  int x0 = std::max(dst_x, clip.x);
  int y0 = std::max(dst_y, clip.y);
  int x1 = std::min(dst_x + dst_w, clip.x + clip.width);
  int y1 = std::min(dst_y + dst_h, clip.y + clip.height);
  if (x0 >= x1 || y0 >= y1) return;

  lui_surface_t* dst = lui_canvas_get_surface(canvas);
  if (!dst || !dst->pixels) return;

  if (dst_w == src_w && dst_h == src_h) {
    const int copy_w = x1 - x0;
    for (int y = y0; y < y1; ++y) {
      const int sy = y - dst_y;
      const int sx = x0 - dst_x;
      std::memcpy(dst->pixels + static_cast<size_t>(y) * dst->stride + x0,
                  pixels + static_cast<size_t>(sy) * src_w + sx,
                  static_cast<size_t>(copy_w) * sizeof(uint32_t));
    }
    return;
  }

  const int64_t denom_x = static_cast<int64_t>(2) * dst_w;
  const int64_t denom_y = static_cast<int64_t>(2) * dst_h;
  for (int y = y0; y < y1; ++y) {
    int64_t sy_num =
        (static_cast<int64_t>(2) * (y - dst_y) + 1) * src_h;
    int sy = static_cast<int>(sy_num / denom_y);
    if (sy < 0) sy = 0;
    if (sy >= src_h) sy = src_h - 1;

    uint32_t* dst_row = dst->pixels + static_cast<size_t>(y) * dst->stride;
    const uint32_t* src_row = pixels + static_cast<size_t>(sy) * src_w;
    for (int x = x0; x < x1; ++x) {
      int64_t sx_num =
          (static_cast<int64_t>(2) * (x - dst_x) + 1) * src_w;
      int sx = static_cast<int>(sx_num / denom_x);
      if (sx < 0) sx = 0;
      if (sx >= src_w) sx = src_w - 1;
      dst_row[x] = src_row[sx];
    }
  }
}

Result Picture::load(const uint32_t* data, uint32_t w, uint32_t h, ColorSpace cs,
                     bool premultiplied) {
  if (!data || w == 0 || h == 0) return Result::InvalidArguments;
  width_  = w;
  height_ = h;

  if (!premultiplied &&
      (cs == ColorSpace::ARGB8888 || cs == ColorSpace::ARGB8888S)) {
    pixels_.assign(data, data + static_cast<size_t>(w) * h);
    opaque_ = pixels_are_opaque(pixels_.data(), pixels_.size());
    return Result::Success;
  }

  pixels_.resize(static_cast<size_t>(w) * h);

  // Convert source to lui_canvas's expected ARGB-packed
  // (0xAARRGGBB straight) layout.
  for (uint32_t i = 0; i < w * h; ++i) {
    uint32_t p = data[i];
    uint8_t a, r, g, b;
    switch (cs) {
      case ColorSpace::ABGR8888:
      case ColorSpace::ABGR8888S:
        // ABGR8888 packs A,B,G,R in nibbles 31..0:
        //   A = bits 31..24, B = 23..16, G = 15..8, R = 7..0
        a = static_cast<uint8_t>((p >> 24) & 0xff);
        b = static_cast<uint8_t>((p >> 16) & 0xff);
        g = static_cast<uint8_t>((p >> 8) & 0xff);
        r = static_cast<uint8_t>(p & 0xff);
        break;
      case ColorSpace::ARGB8888:
      case ColorSpace::ARGB8888S:
      default:
        a = static_cast<uint8_t>((p >> 24) & 0xff);
        r = static_cast<uint8_t>((p >> 16) & 0xff);
        g = static_cast<uint8_t>((p >> 8) & 0xff);
        b = static_cast<uint8_t>(p & 0xff);
        break;
    }
    // Undo pre-multiplication.
    if (premultiplied && a > 0 && a < 255) {
      r = static_cast<uint8_t>(std::min(255, (r * 255) / a));
      g = static_cast<uint8_t>(std::min(255, (g * 255) / a));
      b = static_cast<uint8_t>(std::min(255, (b * 255) / a));
    }
    pixels_[i] = pack_argb(r, g, b, a);
  }
  opaque_ = pixels_are_opaque(pixels_.data(), pixels_.size());
  return Result::Success;
}

Result Picture::load_argb_owned(std::vector<uint32_t>&& pixels, uint32_t w,
                                uint32_t h, bool premultiplied) {
  if (w == 0 || h == 0 || pixels.size() != static_cast<size_t>(w) * h) {
    return Result::InvalidArguments;
  }
  width_ = w;
  height_ = h;
  if (!premultiplied) {
    opaque_ = pixels_are_opaque(pixels.data(), pixels.size());
    pixels_ = std::move(pixels);
    return Result::Success;
  }
  return load(pixels.data(), w, h, ColorSpace::ARGB8888, true);
}

Result Picture::transform(const Matrix& m) {
  transform_ = m;
  return Result::Success;
}

Result Picture::translate(float x, float y) {
  transform_.e13 += x;
  transform_.e23 += y;
  return Result::Success;
}

void Picture::compute_bbox(float* x, float* y, float* w, float* h) const {
  if (!x || !y || !w || !h) return;
  if (width_ == 0 || height_ == 0) {
    *x = *y = *w = *h = 0;
    return;
  }
  const Matrix& m = transform_;
  float sw = static_cast<float>(width_);
  float sh = static_cast<float>(height_);
  auto fx = [&](float u, float v) { return m.e11 * u + m.e12 * v + m.e13; };
  auto fy = [&](float u, float v) { return m.e21 * u + m.e22 * v + m.e23; };
  float xs[4] = {fx(0, 0), fx(sw, 0), fx(sw, sh), fx(0, sh)};
  float ys[4] = {fy(0, 0), fy(sw, 0), fy(sw, sh), fy(0, sh)};
  float minx = std::min({xs[0], xs[1], xs[2], xs[3]});
  float maxx = std::max({xs[0], xs[1], xs[2], xs[3]});
  float miny = std::min({ys[0], ys[1], ys[2], ys[3]});
  float maxy = std::max({ys[0], ys[1], ys[2], ys[3]});
  *x = minx;
  *y = miny;
  *w = maxx - minx;
  *h = maxy - miny;
}

void Picture::draw_on(lui_canvas_t* canvas) {
  if (!canvas || pixels_.empty() || width_ == 0 || height_ == 0) return;

  if (soft_mask_) {
    auto mask = soft_mask_;
    soft_mask_.reset();
    float bx, by, bw, bh;
    compute_bbox(&bx, &by, &bw, &bh);
    int x0 = static_cast<int>(std::floor(bx));
    int y0 = static_cast<int>(std::floor(by));
    int x1 = static_cast<int>(std::ceil(bx + bw));
    int y1 = static_cast<int>(std::ceil(by + bh));
    draw_with_soft_mask(canvas, *mask, x0, y0, x1, y1,
                        [this](lui_canvas_t* c) { draw_on(c); });
    soft_mask_ = mask;
    return;
  }

  lui_rect_t saved_clip = lui_canvas_get_clip(canvas);
  int clip_mode = 0;
  lui_rect_t clip_bbox{0, 0, 0, 0};

  if (clipper_) {
    float crx, cry, crw, crh;
    if (detect_axis_aligned_rect(clipper_->cmds_, clipper_->pts_, crx, cry,
                                 crw, crh)) {
      int x0 = std::max(static_cast<int>(std::floor(crx)), saved_clip.x);
      int y0 = std::max(static_cast<int>(std::floor(cry)), saved_clip.y);
      int x1 = std::min(static_cast<int>(std::ceil(crx + crw)),
                        saved_clip.x + saved_clip.width);
      int y1 = std::min(static_cast<int>(std::ceil(cry + crh)),
                        saved_clip.y + saved_clip.height);
      clip_bbox.x = x0;
      clip_bbox.y = y0;
      clip_bbox.width = std::max(0, x1 - x0);
      clip_bbox.height = std::max(0, y1 - y0);
      if (clip_bbox.width == 0 || clip_bbox.height == 0) return;
      lui_canvas_set_clip(canvas, &clip_bbox);
      clip_mode = 1;
    } else {
      float cx, cy, cw, ch;
      clipper_->compute_bbox(&cx, &cy, &cw, &ch);
      int x0 = std::max(static_cast<int>(std::floor(cx)), saved_clip.x);
      int y0 = std::max(static_cast<int>(std::floor(cy)), saved_clip.y);
      int x1 = std::min(static_cast<int>(std::ceil(cx + cw)),
                        saved_clip.x + saved_clip.width);
      int y1 = std::min(static_cast<int>(std::ceil(cy + ch)),
                        saved_clip.y + saved_clip.height);
      clip_bbox.x = x0;
      clip_bbox.y = y0;
      clip_bbox.width = std::max(0, x1 - x0);
      clip_bbox.height = std::max(0, y1 - y0);
      if (clip_bbox.width == 0 || clip_bbox.height == 0) return;
      lui_canvas_set_clip(canvas, &clip_bbox);
      clip_mode = 2;
    }
  }

  std::vector<uint32_t> snapshot;
  std::vector<uint8_t> mask;
  if (clip_mode == 2) {
    lui_surface_t* dst = lui_canvas_get_surface(canvas);
    if (dst && dst->pixels) {
      snapshot.resize(static_cast<size_t>(clip_bbox.width) * clip_bbox.height);
      for (int y = 0; y < clip_bbox.height; ++y) {
        const uint32_t* row =
            dst->pixels + (clip_bbox.y + y) * dst->stride + clip_bbox.x;
        std::memcpy(snapshot.data() + static_cast<size_t>(y) * clip_bbox.width,
                    row, static_cast<size_t>(clip_bbox.width) * sizeof(uint32_t));
      }
    }
    mask = rasterize_path_to_mask(clipper_->cmds_, clipper_->pts_,
                                  clip_bbox.x, clip_bbox.y,
                                  clip_bbox.width, clip_bbox.height,
                                  clipper_->fill_rule_);
  }

  draw_pixels(canvas);

  if (clip_mode == 2 && !mask.empty() && !snapshot.empty()) {
    lui_surface_t* dst = lui_canvas_get_surface(canvas);
    if (dst && dst->pixels) {
      for (int y = 0; y < clip_bbox.height; ++y) {
        uint32_t* dst_row =
            dst->pixels + (clip_bbox.y + y) * dst->stride + clip_bbox.x;
        const uint32_t* snap_row =
            snapshot.data() + static_cast<size_t>(y) * clip_bbox.width;
        const uint8_t* mask_row =
            mask.data() + static_cast<size_t>(y) * clip_bbox.width;
        for (int x = 0; x < clip_bbox.width; ++x) {
          uint8_t m = mask_row[x];
          if (m == 255) continue;
          if (m == 0) {
            dst_row[x] = snap_row[x];
            continue;
          }
          uint32_t snap = snap_row[x];
          uint32_t curr = dst_row[x];
          uint16_t inv = static_cast<uint16_t>(255 - m);
          uint8_t sa = static_cast<uint8_t>((snap >> 24) & 0xff);
          uint8_t sr = static_cast<uint8_t>((snap >> 16) & 0xff);
          uint8_t sg = static_cast<uint8_t>((snap >> 8) & 0xff);
          uint8_t sb = static_cast<uint8_t>(snap & 0xff);
          uint8_t ca = static_cast<uint8_t>((curr >> 24) & 0xff);
          uint8_t cr = static_cast<uint8_t>((curr >> 16) & 0xff);
          uint8_t cg = static_cast<uint8_t>((curr >> 8) & 0xff);
          uint8_t cb = static_cast<uint8_t>(curr & 0xff);
          uint8_t oa = static_cast<uint8_t>((ca * m + sa * inv) / 255);
          uint8_t orr = static_cast<uint8_t>((cr * m + sr * inv) / 255);
          uint8_t og = static_cast<uint8_t>((cg * m + sg * inv) / 255);
          uint8_t ob = static_cast<uint8_t>((cb * m + sb * inv) / 255);
          dst_row[x] = pack_argb(orr, og, ob, oa);
        }
      }
    }
  }

  if (clip_mode != 0) {
    lui_canvas_set_clip(canvas, &saved_clip);
  }
}

void Picture::draw_pixels(lui_canvas_t* canvas) {
  if (!canvas || pixels_.empty() || width_ == 0 || height_ == 0) return;

  // Axis-aligned scale + translate? Use fast path through lui_canvas.
  // Note: lui_canvas_draw_image doesn't support mirroring, so negative
  // scale (PDF horizontal/vertical text flip), opacity, and custom blend
  // modes must take the per-pixel path.
  if (std::fabs(transform_.e12) < 1e-4f &&
      std::fabs(transform_.e21) < 1e-4f &&
      transform_.e11 > 0.0f &&
      transform_.e22 > 0.0f &&
      opacity_ == 255 &&
      blend_mode_ == BlendMethod::Normal) {
    lui_surface_t src = lui_surface_wrap(pixels_.data(),
                                         static_cast<int>(width_),
                                         static_cast<int>(height_),
                                         static_cast<int>(width_));
    float sx = transform_.e11;
    float sy = transform_.e22;
    int dst_w = static_cast<int>(std::round(sx * width_));
    int dst_h = static_cast<int>(std::round(sy * height_));
    if (dst_w <= 0 || dst_h <= 0) return;
    int dst_x = static_cast<int>(std::round(transform_.e13));
    int dst_y = static_cast<int>(std::round(transform_.e23));
    lui_image_filter_t filter =
        interpolate_ ? LUI_IMAGE_FILTER_BILINEAR : LUI_IMAGE_FILTER_NEAREST;
    if (opaque_ && filter == LUI_IMAGE_FILTER_NEAREST) {
      draw_opaque_nearest_image(canvas, pixels_.data(),
                                static_cast<int>(width_),
                                static_cast<int>(height_),
                                dst_x, dst_y, dst_w, dst_h);
      return;
    }
    lui_canvas_draw_image(canvas, dst_x, dst_y, dst_w, dst_h, &src, nullptr,
                          filter);
    return;
  }

  // Rotation/shear: per-pixel inverse transform sample.
  draw_transformed(canvas);
}

void Picture::draw_transformed(lui_canvas_t* canvas) {
  // Compute inverse of the 2x2 transform part.
  const Matrix& m = transform_;
  float det = m.e11 * m.e22 - m.e12 * m.e21;
  if (std::fabs(det) < 1e-8f) return;
  float inv_det = 1.0f / det;
  float i11 =  m.e22 * inv_det;
  float i12 = -m.e12 * inv_det;
  float i21 = -m.e21 * inv_det;
  float i22 =  m.e11 * inv_det;

  // Output bbox by transforming the four source corners.
  float sw = static_cast<float>(width_);
  float sh = static_cast<float>(height_);
  auto fwd_x = [&](float u, float v) { return m.e11 * u + m.e12 * v + m.e13; };
  auto fwd_y = [&](float u, float v) { return m.e21 * u + m.e22 * v + m.e23; };
  float xs[4] = {fwd_x(0, 0), fwd_x(sw, 0), fwd_x(sw, sh), fwd_x(0, sh)};
  float ys[4] = {fwd_y(0, 0), fwd_y(sw, 0), fwd_y(sw, sh), fwd_y(0, sh)};
  float minx = std::min({xs[0], xs[1], xs[2], xs[3]});
  float maxx = std::max({xs[0], xs[1], xs[2], xs[3]});
  float miny = std::min({ys[0], ys[1], ys[2], ys[3]});
  float maxy = std::max({ys[0], ys[1], ys[2], ys[3]});

  // Clip to canvas clip rect.
  lui_rect_t clip = lui_canvas_get_clip(canvas);
  int ix0 = std::max(static_cast<int>(std::floor(minx)), clip.x);
  int iy0 = std::max(static_cast<int>(std::floor(miny)), clip.y);
  int ix1 = std::min(static_cast<int>(std::ceil(maxx)),  clip.x + clip.width);
  int iy1 = std::min(static_cast<int>(std::ceil(maxy)),  clip.y + clip.height);
  if (ix0 >= ix1 || iy0 >= iy1) return;

  lui_surface_t* dst = lui_canvas_get_surface(canvas);
  if (!dst || !dst->pixels) return;

  int sw_int = static_cast<int>(width_);
  int sh_int = static_cast<int>(height_);

  for (int py = iy0; py < iy1; ++py) {
    uint32_t* dst_row = dst->pixels + py * dst->stride;
    for (int px = ix0; px < ix1; ++px) {
      // Sample at the pixel centre.
      float ox = (px + 0.5f) - m.e13;
      float oy = (py + 0.5f) - m.e23;
      float u = i11 * ox + i12 * oy;
      float v = i21 * ox + i22 * oy;
      if (u < 0.0f || u >= sw || v < 0.0f || v >= sh) continue;

      auto idx = [&](int x, int y) {
        return static_cast<size_t>(y) * sw_int + x;
      };

      uint32_t sample = 0;
      if (interpolate_) {
        int u0 = static_cast<int>(std::floor(u));
        int v0 = static_cast<int>(std::floor(v));
        float fu = u - u0;
        float fv = v - v0;
        int u1 = std::min(u0 + 1, sw_int - 1);
        int v1 = std::min(v0 + 1, sh_int - 1);
        if (u0 < 0) u0 = 0;
        if (v0 < 0) v0 = 0;

        uint32_t p00 = pixels_[idx(u0, v0)];
        uint32_t p10 = pixels_[idx(u1, v0)];
        uint32_t p01 = pixels_[idx(u0, v1)];
        uint32_t p11 = pixels_[idx(u1, v1)];

        auto comp = [&](int shift) -> uint8_t {
          float c00 = static_cast<float>((p00 >> shift) & 0xff);
          float c10 = static_cast<float>((p10 >> shift) & 0xff);
          float c01 = static_cast<float>((p01 >> shift) & 0xff);
          float c11 = static_cast<float>((p11 >> shift) & 0xff);
          float c0 = c00 * (1.0f - fu) + c10 * fu;
          float c1 = c01 * (1.0f - fu) + c11 * fu;
          float c  = c0 * (1.0f - fv) + c1 * fv;
          return static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, c + 0.5f)));
        };
        sample = pack_argb(comp(16), comp(8), comp(0), comp(24));
      } else {
        int sx = std::min(sw_int - 1, std::max(0, static_cast<int>(std::floor(u))));
        int sy = std::min(sh_int - 1, std::max(0, static_cast<int>(std::floor(v))));
        sample = pixels_[idx(sx, sy)];
      }

      uint8_t sa = static_cast<uint8_t>((sample >> 24) & 0xff);
      if (sa == 0) continue;
      if (opacity_ < 255) {
        sa = static_cast<uint8_t>((static_cast<uint16_t>(sa) * opacity_ + 127) / 255);
        if (sa == 0) continue;
      }
      uint8_t sr = static_cast<uint8_t>((sample >> 16) & 0xff);
      uint8_t sg = static_cast<uint8_t>((sample >> 8) & 0xff);
      uint8_t sb = static_cast<uint8_t>(sample & 0xff);

      uint32_t src = pack_argb(sr, sg, sb, sa);
      dst_row[px] = composite_pixel(dst_row[px], src, blend_mode_);
    }
  }
}

// =========================================================================
// LinearGradient / RadialGradient
// =========================================================================
LinearGradient* LinearGradient::gen() { return new LinearGradient(); }

Result LinearGradient::colorStops(const Fill::ColorStop* stops,
                                  uint32_t count) {
  stops_.assign(stops, stops + count);
  return Result::Success;
}

void LinearGradient::sample(float t, uint8_t& r, uint8_t& g, uint8_t& b,
                            uint8_t& a) const {
  if (stops_.empty()) {
    r = g = b = 0;
    a = 0;
    return;
  }
  if (t <= stops_.front().offset) {
    r = stops_.front().r;
    g = stops_.front().g;
    b = stops_.front().b;
    a = stops_.front().a;
    return;
  }
  if (t >= stops_.back().offset) {
    r = stops_.back().r;
    g = stops_.back().g;
    b = stops_.back().b;
    a = stops_.back().a;
    return;
  }
  for (size_t i = 1; i < stops_.size(); ++i) {
    if (t <= stops_[i].offset) {
      float t0 = stops_[i - 1].offset;
      float t1 = stops_[i].offset;
      float u  = (t - t0) / std::max(1e-6f, t1 - t0);
      r = static_cast<uint8_t>(stops_[i - 1].r + u * (stops_[i].r - stops_[i - 1].r));
      g = static_cast<uint8_t>(stops_[i - 1].g + u * (stops_[i].g - stops_[i - 1].g));
      b = static_cast<uint8_t>(stops_[i - 1].b + u * (stops_[i].b - stops_[i - 1].b));
      a = static_cast<uint8_t>(stops_[i - 1].a + u * (stops_[i].a - stops_[i - 1].a));
      return;
    }
  }
  r = stops_.back().r;
  g = stops_.back().g;
  b = stops_.back().b;
  a = stops_.back().a;
}

RadialGradient* RadialGradient::gen() { return new RadialGradient(); }

Result RadialGradient::radial(float cx, float cy, float r, float fx,
                              float fy, float fr) {
  cx_ = cx;
  cy_ = cy;
  r_  = r;
  fx_ = fx;
  fy_ = fy;
  fr_ = fr;
  return Result::Success;
}

Result RadialGradient::colorStops(const Fill::ColorStop* stops,
                                  uint32_t count) {
  stops_.assign(stops, stops + count);
  return Result::Success;
}

void RadialGradient::sample(float t, uint8_t& r, uint8_t& g, uint8_t& b,
                            uint8_t& a) const {
  // Same logic as linear (already factored on offset).
  if (stops_.empty()) {
    r = g = b = 0;
    a = 0;
    return;
  }
  if (t <= stops_.front().offset) {
    r = stops_.front().r;
    g = stops_.front().g;
    b = stops_.front().b;
    a = stops_.front().a;
    return;
  }
  if (t >= stops_.back().offset) {
    r = stops_.back().r;
    g = stops_.back().g;
    b = stops_.back().b;
    a = stops_.back().a;
    return;
  }
  for (size_t i = 1; i < stops_.size(); ++i) {
    if (t <= stops_[i].offset) {
      float t0 = stops_[i - 1].offset;
      float t1 = stops_[i].offset;
      float u  = (t - t0) / std::max(1e-6f, t1 - t0);
      r = static_cast<uint8_t>(stops_[i - 1].r + u * (stops_[i].r - stops_[i - 1].r));
      g = static_cast<uint8_t>(stops_[i - 1].g + u * (stops_[i].g - stops_[i - 1].g));
      b = static_cast<uint8_t>(stops_[i - 1].b + u * (stops_[i].b - stops_[i - 1].b));
      a = static_cast<uint8_t>(stops_[i - 1].a + u * (stops_[i].a - stops_[i - 1].a));
      return;
    }
  }
  r = stops_.back().r;
  g = stops_.back().g;
  b = stops_.back().b;
  a = stops_.back().a;
}

// =========================================================================
// Scene
// =========================================================================
Scene* Scene::gen() { return new Scene(); }

Scene::~Scene() {
  for (Paint* p : paints_) delete p;
}

Result Scene::add(Paint* p) {
  if (!p) return Result::InvalidArguments;
  paints_.push_back(p);
  return Result::Success;
}

void Scene::clear() {
  for (Paint* p : paints_) delete p;
  paints_.clear();
}

void Scene::compute_bbox(float* x, float* y, float* w, float* h) const {
  if (!x || !y || !w || !h) return;
  if (paints_.empty()) {
    *x = *y = *w = *h = 0;
    return;
  }
  float minx =  std::numeric_limits<float>::infinity();
  float miny =  std::numeric_limits<float>::infinity();
  float maxx = -std::numeric_limits<float>::infinity();
  float maxy = -std::numeric_limits<float>::infinity();
  for (const Paint* p : paints_) {
    if (!p) continue;
    float bx, by, bw, bh;
    p->bounds(&bx, &by, &bw, &bh);
    if (bw <= 0.0f || bh <= 0.0f) continue;
    if (bx < minx) minx = bx;
    if (by < miny) miny = by;
    if (bx + bw > maxx) maxx = bx + bw;
    if (by + bh > maxy) maxy = by + bh;
  }
  if (minx == std::numeric_limits<float>::infinity()) {
    *x = *y = *w = *h = 0;
    return;
  }
  *x = minx;
  *y = miny;
  *w = maxx - minx;
  *h = maxy - miny;
}

void Scene::draw_on(lui_canvas_t* canvas) {
  if (!canvas) return;

  if (soft_mask_) {
    auto mask = soft_mask_;
    soft_mask_.reset();
    // Tight scene bbox (union of child paint bounds). Falls back to the
    // canvas clip if the scene has no children with non-degenerate bounds.
    float bx, by, bw, bh;
    compute_bbox(&bx, &by, &bw, &bh);
    int x0, y0, x1, y1;
    if (bw > 0.0f && bh > 0.0f) {
      x0 = static_cast<int>(std::floor(bx));
      y0 = static_cast<int>(std::floor(by));
      x1 = static_cast<int>(std::ceil(bx + bw));
      y1 = static_cast<int>(std::ceil(by + bh));
    } else {
      lui_rect_t clip = lui_canvas_get_clip(canvas);
      x0 = clip.x;
      y0 = clip.y;
      x1 = clip.x + clip.width;
      y1 = clip.y + clip.height;
    }
    draw_with_soft_mask(canvas, *mask, x0, y0, x1, y1,
                        [this](lui_canvas_t* c) { draw_on(c); });
    soft_mask_ = mask;
    return;
  }
  // Apply scene-level clip (bbox of clipper) the same way Shape does.
  lui_rect_t saved_clip = lui_canvas_get_clip(canvas);
  bool clip_changed = false;
  if (clipper_) {
    float cx, cy, cw, ch;
    clipper_->compute_bbox(&cx, &cy, &cw, &ch);
    int x0 = std::max(static_cast<int>(std::floor(cx)), saved_clip.x);
    int y0 = std::max(static_cast<int>(std::floor(cy)), saved_clip.y);
    int x1 = std::min(static_cast<int>(std::ceil(cx + cw)),
                      saved_clip.x + saved_clip.width);
    int y1 = std::min(static_cast<int>(std::ceil(cy + ch)),
                      saved_clip.y + saved_clip.height);
    lui_rect_t cr;
    cr.x = x0;
    cr.y = y0;
    cr.width  = std::max(0, x1 - x0);
    cr.height = std::max(0, y1 - y0);
    lui_canvas_set_clip(canvas, &cr);
    clip_changed = true;
  }
  for (Paint* p : paints_) p->draw_on(canvas);
  if (clip_changed) {
    lui_canvas_set_clip(canvas, &saved_clip);
  }
}

// =========================================================================
// SwCanvas
// =========================================================================
SwCanvas* SwCanvas::gen() { return new SwCanvas(); }

SwCanvas::~SwCanvas() {
  for (Paint* p : paints_) delete p;
  if (have_canvas_) {
    lui_canvas_destroy(&canvas_);
  }
}

Result SwCanvas::target(uint32_t* buffer, uint32_t stride, uint32_t width,
                        uint32_t height, ColorSpace cs) {
  if (!buffer || width == 0 || height == 0) {
    return Result::InvalidArguments;
  }
  if (have_canvas_) {
    lui_canvas_destroy(&canvas_);
    have_canvas_ = false;
  }
  surface_ = lui_surface_wrap(buffer, static_cast<int>(width),
                              static_cast<int>(height),
                              static_cast<int>(stride));
  lui_canvas_init(&canvas_, &surface_);
  have_canvas_ = true;
  cs_ = cs;
  return Result::Success;
}

Result SwCanvas::add(Paint* p) {
  if (!p) return Result::InvalidArguments;
  paints_.push_back(p);
  return Result::Success;
}

Result SwCanvas::draw(bool clear_first) {
  if (!have_canvas_) return Result::InsufficientCondition;
  if (clear_first) {
    // Transparent black; the caller (backend) fills its own background.
    lui_canvas_clear(&canvas_, 0u);
  }
  for (Paint* p : paints_) p->draw_on(&canvas_);

  // After draw, swizzle the surface in-place if the requested colorspace
  // expects ABGR byte order (R,G,B,A in memory) — lui_canvas writes ARGB
  // (B,G,R,A in memory). PDF backends pass ABGR8888 and read pixels back
  // expecting R,G,B,A byte order; swap R and B per pixel.
  if (output_swizzle_enabled_ &&
      (cs_ == ColorSpace::ABGR8888 || cs_ == ColorSpace::ABGR8888S)) {
    uint32_t* pix = surface_.pixels;
    int n = surface_.height * surface_.stride;
    for (int i = 0; i < n; ++i) {
      uint32_t p = pix[i];
      // Repack ARGB word 0xAARRGGBB as ABGR word 0xAABBGGRR.
      pix[i] = (p & 0xff00ff00u) | ((p & 0x00ff0000u) >> 16) |
               ((p & 0x000000ffu) << 16);
    }
  }
  return Result::Success;
}

Result SwCanvas::sync() { return Result::Success; }

}  // namespace lvg

#endif  // NANOPDF_USE_LIGHTVG
