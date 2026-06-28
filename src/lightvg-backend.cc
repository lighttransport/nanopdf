// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#ifdef NANOPDF_USE_LIGHTVG

#include "lightvg-backend.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>

#include "nanopdf-log.hh"
#include "color-transform.hh"
#include "cff-wrapper.hh"
#include "cff-parser.hh"
#include "font-provider.hh"
#include "font-unicode-map.hh"
#include "shared-font-cache.hh"
#include "render-cache.hh"
#ifdef NANOPDF_USE_FPNGE
#include "fpnge/fpnge_dispatch.hh"
#endif
#include "pdf-function.hh"
#include "string-parse.hh"

#ifdef NANOPDF_EMBED_FONTS
#include "embedded-fonts.hh"
#endif

#ifdef NANOPDF_EMBED_CJK_FONTS
#include "embedded-cjk-fonts.hh"
#endif

// For PNG saving - implementation in stb_image_write_impl.cc
#include "stb_image_write.h"

// External C variable for PNG compression level
extern "C" int stbi_write_png_compression_level;

namespace nanopdf {

static constexpr size_t kMaxCachedArgbImageBytes = 1 * 1024 * 1024;
static constexpr uint64_t kMaxSoftMaskGroupPixels = 2ull * 1024ull * 1024ull;

// Glyph anti-aliasing gamma table: pow(i/255, 1.4) for i in [0,255].
// Tabulates the transcendental used on every glyph coverage sample; callers
// keep the surrounding `* a + 0.5f` / `* 255.0f + 0.5f` arithmetic so the
// result is identical to the previous std::pow() code.
static const std::array<float, 256>& glyph_gamma_lut() {
  static const std::array<float, 256> kLut = [] {
    std::array<float, 256> t{};
    for (int i = 0; i < 256; ++i) {
      t[i] = std::pow(static_cast<float>(i) / 255.0f, 1.4f);
    }
    return t;
  }();
  return kLut;
}

static uint32_t pack_argb32(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return (static_cast<uint32_t>(a) << 24) |
         (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) |
         static_cast<uint32_t>(b);
}

static uint32_t normalize_render_codepoint(uint32_t cp) {
  if (cp <= 0x10FFFFu) return cp;
  uint32_t hi = (cp >> 16) & 0xFFFFu;
  uint32_t lo = cp & 0xFFFFu;
  if (hi >= 0xD800u && hi <= 0xDBFFu &&
      lo >= 0xDC00u && lo <= 0xDFFFu) {
    return 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
  }
  if (hi == 'f' && lo == 'f') return 0xFB00u;
  if (hi == 'f' && lo == 'i') return 0xFB01u;
  if (hi == 'f' && lo == 'l') return 0xFB02u;
  if (hi >= 0x20u && hi <= 0x7Eu) return hi;
  return cp;
}

static std::vector<std::string> type1_glyph_name_candidates_for_unicode(uint32_t cp) {
  cp = normalize_render_codepoint(cp);
  std::vector<std::string> names;
  switch (cp) {
    case 0x0028:
      names = {"parenleft", "parenleftbig", "parenleftBig",
               "parenlefttp", "parenleftbt", "parenleftex"};
      break;
    case 0x0029:
      names = {"parenright", "parenrightbig", "parenrightBig",
               "parenrighttp", "parenrightbt", "parenrightex"};
      break;
    case 0x002B: names = {"plus"}; break;
    case 0x002C: names = {"comma"}; break;
    case 0x002E: names = {"period"}; break;
    case 0x002F: names = {"slash"}; break;
    case 0x003D: names = {"equal"}; break;
    case 0x005B:
      names = {"bracketleft", "bracketlefttp", "bracketleftbt",
               "bracketleftex", "bracketleftbig"};
      break;
    case 0x005D:
      names = {"bracketright", "bracketrighttp", "bracketrightbt",
               "bracketrightex", "bracketrightbig"};
      break;
    case 0x007B:
      names = {"braceleft", "bracelefttp", "braceleftmid", "braceleftbt",
               "braceleftex", "braceleftbig", "braceleftbigg"};
      break;
    case 0x007D:
      names = {"braceright", "bracerighttp", "bracerightmid", "bracerightbt",
               "bracerightex", "bracerightbig", "bracerightbigg"};
      break;
    case 0x007C: names = {"bar", "vert", "uni007C", "bar1", "barex"}; break;
    case 0x00B7: names = {"periodcentered"}; break;
    case 0x03A9: names = {"Omega", "uni03A9"}; break;
    case 0x2190: names = {"arrowleft"}; break;
    case 0x2192: names = {"arrowright"}; break;
    case 0x2207: names = {"nabla"}; break;
    case 0x2211: names = {"summationdisplay.1", "summationtext.1", "summation"}; break;
    case 0x2212: names = {"minus"}; break;
    case 0x221A: names = {"radical"}; break;
    case 0x2223: names = {"bar"}; break;
    case 0x2225: names = {"bardbl", "bardblex"}; break;
    case 0x222B: names = {"integraldisplay", "integraltext", "integral"}; break;
    case 0x2264: names = {"lessequal"}; break;
    case 0x2286: names = {"subsetorequal"}; break;
    case 0x2295: names = {"circleplus"}; break;
    case 0x230A: names = {"floorleft"}; break;
    case 0x230B: names = {"floorright"}; break;
    case 0x2308: names = {"ceilingleft"}; break;
    case 0x2309: names = {"ceilingright"}; break;
    case 0x27E8: names = {"angleleft"}; break;
    case 0x27E9: names = {"angleright"}; break;
    default: break;
  }
  if (cp >= 'A' && cp <= 'Z') names.push_back(std::string(1, static_cast<char>(cp)));
  if (cp >= 'a' && cp <= 'z') names.push_back(std::string(1, static_cast<char>(cp)));
  if (cp > 0xFFFFu && cp <= 0x10FFFFu) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "u%04X", cp);
    names.push_back(buf);
  }
  if (cp > 0 && cp <= 0xFFFFu) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "uni%04X", cp);
    names.push_back(buf);
  }
  return names;
}

static bool bgra_pixels_equal(const uint8_t* a, const uint8_t* b) {
  uint32_t pa, pb;
  std::memcpy(&pa, a, sizeof(pa));
  std::memcpy(&pb, b, sizeof(pb));
  return pa == pb;
}

struct BufferedFileWriter {
  explicit BufferedFileWriter(FILE* file) : file(file) {}

  ~BufferedFileWriter() { flush(); }

  bool write(const void* data, size_t size) {
    const uint8_t* src = static_cast<const uint8_t*>(data);
    while (size > 0) {
      size_t avail = sizeof(buffer) - used;
      if (avail == 0) {
        if (!flush()) return false;
        avail = sizeof(buffer);
      }
      size_t n = std::min(avail, size);
      std::memcpy(buffer + used, src, n);
      used += n;
      src += n;
      size -= n;
    }
    return true;
  }

  bool write1(uint8_t v) { return write(&v, 1); }
  bool write4(const uint8_t* v) { return write(v, 4); }

  bool flush() {
    if (used == 0) return ok;
    if (!ok || !file) return false;
    ok = std::fwrite(buffer, 1, used, file) == used;
    used = 0;
    return ok;
  }

  FILE* file{nullptr};
  uint8_t buffer[64 * 1024]{};
  size_t used{0};
  bool ok{true};
};

static bool write_tga_bgra_rle(const std::string& filename, int width,
                               int height, const uint8_t* bgra) {
  if (width <= 0 || height <= 0 || !bgra || width > 65535 ||
      height > 65535) {
    return false;
  }

  FILE* file = std::fopen(filename.c_str(), "wb");
  if (!file) return false;
  BufferedFileWriter writer(file);

  uint8_t header[18] = {
      0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      static_cast<uint8_t>(width & 0xff),
      static_cast<uint8_t>((width >> 8) & 0xff),
      static_cast<uint8_t>(height & 0xff),
      static_cast<uint8_t>((height >> 8) & 0xff),
      32, 8};

  bool ok = writer.write(header, sizeof(header));
  for (int y = height - 1; ok && y >= 0; --y) {
    const uint8_t* row =
        bgra + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
    for (int x = 0, len = 0; ok && x < width; x += len) {
      const uint8_t* begin = row + static_cast<size_t>(x) * 4;
      bool diff = true;
      len = 1;

      if (x < width - 1) {
        ++len;
        diff = !bgra_pixels_equal(begin, row + static_cast<size_t>(x + 1) * 4);
        if (diff) {
          const uint8_t* prev = begin;
          for (int k = x + 2; k < width && len < 128; ++k) {
            const uint8_t* px = row + static_cast<size_t>(k) * 4;
            if (!bgra_pixels_equal(prev, px)) {
              prev = px;
              ++len;
            } else {
              --len;
              break;
            }
          }
        } else {
          for (int k = x + 2; k < width && len < 128; ++k) {
            if (bgra_pixels_equal(begin, row + static_cast<size_t>(k) * 4)) {
              ++len;
            } else {
              break;
            }
          }
        }
      }

      if (diff) {
        ok = writer.write1(static_cast<uint8_t>(len - 1)) &&
             writer.write(begin, static_cast<size_t>(len) * 4);
      } else {
        ok = writer.write1(static_cast<uint8_t>(len - 129)) &&
             writer.write4(begin);
      }
    }
  }

  ok = writer.flush() && ok;
  ok = (std::fclose(file) == 0) && ok;
  writer.file = nullptr;
  return ok;
}

static uint64_t fnv1a64(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

struct LightVGRect {
  float x{0.0f};
  float y{0.0f};
  float w{0.0f};
  float h{0.0f};
};

// Emit ThorVG path commands for a ttf_outline_t (TrueType quadratic or CFF cubic).
// `emit_move`/`emit_line`/`emit_cubic` are called with canvas-space coordinates.
// Each glyph point (gx, gy) in font units is mapped to canvas by:
//   dx = gx*scale*cos_theta - gy*scale*sin_theta;
//   dy = gx*scale*sin_theta + gy*scale*cos_theta;  (in PDF y-up)
//   cx = x0 + dx;  cy = y0 - dy;                   (canvas is y-down)
// Contour-traversal mirrors lightui rasterize.c's decompose_outline so that
// quadratic-only and cubic-only glyphs both round-trip losslessly (quads are
// converted to cubics via the standard 2/3 control-point rule so ThorVG's
// cubicTo path is sufficient).
template <typename MoveFn, typename LineFn, typename CubicFn, typename CloseFn>
static void decompose_ttf_outline_to_path(const ttf_outline_t& outline,
                                          float scale, float x0, float y0,
                                          float cos_theta, float sin_theta,
                                          MoveFn emit_move, LineFn emit_line,
                                          CubicFn emit_cubic,
                                          CloseFn emit_close) {
  auto map = [&](float gx, float gy, float& cx, float& cy) {
    float fx = gx * scale;
    float fy = gy * scale;
    cx = x0 + fx * cos_theta - fy * sin_theta;
    cy = y0 - (fx * sin_theta + fy * cos_theta);
  };

  int pt_idx = 0;
  for (int c = 0; c < outline.num_contours; ++c) {
    int end = outline.contour_ends[c];
    int start = pt_idx;
    int npts = end - start + 1;
    if (npts < 2) { pt_idx = end + 1; continue; }

    auto on_curve = [&](int i) { return outline.points[i].on_curve; };
    auto raw_x = [&](int i) { return outline.points[i].x; };
    auto raw_y = [&](int i) { return outline.points[i].y; };

    float first_cx, first_cy;
    float cur_gx, cur_gy;  // current on-curve point in font units
    bool has_first = false;

    if (on_curve(start) == 1 || on_curve(start) == 2) {
      cur_gx = raw_x(start);
      cur_gy = raw_y(start);
      map(cur_gx, cur_gy, first_cx, first_cy);
      emit_move(first_cx, first_cy);
      has_first = true;
      pt_idx = start + 1;
    } else {
      // TrueType: first point off-curve. If last is on-curve, start there;
      // otherwise start from midpoint of first and last off-curve points.
      if (on_curve(end) == 1) {
        cur_gx = raw_x(end);
        cur_gy = raw_y(end);
      } else {
        cur_gx = 0.5f * (raw_x(start) + raw_x(end));
        cur_gy = 0.5f * (raw_y(start) + raw_y(end));
      }
      map(cur_gx, cur_gy, first_cx, first_cy);
      emit_move(first_cx, first_cy);
      has_first = true;
      pt_idx = start;  // keep the leading off-curve point for the next iteration
    }

    if (!has_first) { pt_idx = end + 1; continue; }

    while (pt_idx <= end) {
      int on = on_curve(pt_idx);
      if (on == 1) {
        float gx = raw_x(pt_idx), gy = raw_y(pt_idx);
        float cx, cy; map(gx, gy, cx, cy);
        emit_line(cx, cy);
        cur_gx = gx; cur_gy = gy;
        ++pt_idx;
      } else if (on == 0) {
        // Quadratic control point in TrueType; emit as cubic via 2/3 rule.
        float ctrl_gx = raw_x(pt_idx), ctrl_gy = raw_y(pt_idx);
        int next = pt_idx + 1;
        bool wrapped = next > end;
        if (wrapped) next = start;
        float end_gx, end_gy;
        if (on_curve(next) == 1) {
          end_gx = raw_x(next); end_gy = raw_y(next);
          pt_idx = wrapped ? end + 1 : next + 1;
        } else {
          // Implied on-curve midpoint between two off-curve points.
          end_gx = 0.5f * (ctrl_gx + raw_x(next));
          end_gy = 0.5f * (ctrl_gy + raw_y(next));
          pt_idx = wrapped ? end + 1 : pt_idx + 1;
        }
        float p0x, p0y, cpx, cpy, p1x, p1y;
        map(cur_gx, cur_gy, p0x, p0y);
        map(ctrl_gx, ctrl_gy, cpx, cpy);
        map(end_gx, end_gy, p1x, p1y);
        float cp1x = p0x + (2.0f / 3.0f) * (cpx - p0x);
        float cp1y = p0y + (2.0f / 3.0f) * (cpy - p0y);
        float cp2x = p1x + (2.0f / 3.0f) * (cpx - p1x);
        float cp2y = p1y + (2.0f / 3.0f) * (cpy - p1y);
        emit_cubic(cp1x, cp1y, cp2x, cp2y, p1x, p1y);
        cur_gx = end_gx; cur_gy = end_gy;
      } else if (on == 2) {
        // CFF cubic: two cubic control points followed by an on-curve endpoint.
        int n1 = pt_idx + 1, n2 = pt_idx + 2;
        if (n1 > end) n1 = start + (n1 - end - 1);
        if (n2 > end) n2 = start + (n2 - end - 1);
        float c1x, c1y, c2x, c2y, ex, ey;
        map(raw_x(pt_idx), raw_y(pt_idx), c1x, c1y);
        map(raw_x(n1), raw_y(n1), c2x, c2y);
        map(raw_x(n2), raw_y(n2), ex, ey);
        emit_cubic(c1x, c1y, c2x, c2y, ex, ey);
        cur_gx = raw_x(n2); cur_gy = raw_y(n2);
        pt_idx += 3;
        if (pt_idx > end + 1) pt_idx = end + 1;
      } else {
        ++pt_idx;
      }
    }
    emit_close();
    pt_idx = end + 1;
  }
}

static bool is_pdf_content_delimiter(char c) {
  return std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '[' ||
         c == ']' || c == '(' || c == ')' || c == '<' || c == '>' ||
         c == '{' || c == '}';
}

// Decode PDF literal string body (content inside the outer parentheses).
// Handles backslash escapes (\n, \r, \t, \b, \f, \(, \), \\) and octal
// escapes (\ddd up to 3 digits). Unknown escapes reduce to the next char.
// Line continuations (\ followed by newline) are removed.
static std::string decode_pdf_literal_string(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c != '\\') { out += c; continue; }
    if (i + 1 >= raw.size()) break;
    char n = raw[++i];
    switch (n) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case '(': out += '('; break;
      case ')': out += ')'; break;
      case '\\': out += '\\'; break;
      case '\n': break;  // line continuation
      case '\r':
        if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
        break;
      default:
        if (n >= '0' && n <= '7') {
          int val = n - '0';
          for (int d = 0; d < 2 && i + 1 < raw.size() && raw[i + 1] >= '0' &&
                          raw[i + 1] <= '7';
               ++d) {
            val = val * 8 + (raw[++i] - '0');
          }
          out += static_cast<char>(val & 0xFF);
        } else {
          out += n;  // unknown escape: emit literal character
        }
        break;
    }
  }
  return out;
}

static bool token_eq1(std::string_view token, char a) {
  return token.size() == 1 && token[0] == a;
}

static bool token_eq2(std::string_view token, char a, char b) {
  return token.size() == 2 && token[0] == a && token[1] == b;
}

static bool token_eq3(std::string_view token, char a, char b, char c) {
  return token.size() == 3 && token[0] == a && token[1] == b &&
         token[2] == c;
}

static bool is_render_progress_operator(std::string_view token) {
  if (token.empty() || token.size() > 2) return false;
  switch (token[0]) {
    case 'f':
      return token_eq1(token, 'f') || token_eq2(token, 'f', '*');
    case 'F':
    case 'S':
      return token_eq1(token, token[0]);
    case 's':
      return token_eq1(token, 's') || token_eq2(token, 's', 'h');
    case 'B':
      return token_eq1(token, 'B') || token_eq2(token, 'B', 'I') ||
             token_eq2(token, 'B', '*');
    case 'b':
      return token_eq1(token, 'b') || token_eq2(token, 'b', '*');
    case 'T':
      return token_eq2(token, 'T', 'j') || token_eq2(token, 'T', 'J');
    case '\'':
    case '"':
      return token_eq1(token, token[0]);
    case 'D':
      return token_eq2(token, 'D', 'o');
    default:
      return false;
  }
}

static void skip_inline_image_payload(std::string_view content, size_t& pos) {
  const size_t size = content.size();

  while (pos < size && std::isspace(static_cast<unsigned char>(content[pos]))) {
    pos++;
  }

  while (pos < size) {
    while (pos < size && std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
    if (pos >= size) break;

    if ((pos + 1) < size && content[pos] == 'I' && content[pos + 1] == 'D' &&
        ((pos + 2) >= size ||
         std::isspace(static_cast<unsigned char>(content[pos + 2])))) {
      pos += 2;
      if (pos < size &&
          std::isspace(static_cast<unsigned char>(content[pos]))) {
        pos++;
      }
      break;
    }

    while (pos < size &&
           !std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
  }

  while ((pos + 1) < size) {
    if (content[pos] == 'E' && content[pos + 1] == 'I' &&
        (pos == 0 ||
         std::isspace(static_cast<unsigned char>(content[pos - 1]))) &&
        ((pos + 2) >= size ||
         std::isspace(static_cast<unsigned char>(content[pos + 2])))) {
      pos += 2;
      return;
    }
    pos++;
  }
}

static size_t count_render_objects_impl(std::string_view content) {
  size_t count = 0;
  size_t pos = 0;

  while (pos < content.size()) {
    while (pos < content.size() &&
           std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
    if (pos >= content.size()) break;

    if (content[pos] == '%') {
      while (pos < content.size() && content[pos] != '\n' &&
             content[pos] != '\r') {
        pos++;
      }
      continue;
    }

    if (content[pos] == '(') {
      int depth = 1;
      pos++;
      while (pos < content.size() && depth > 0) {
        if (content[pos] == '\\' && (pos + 1) < content.size()) {
          pos += 2;
        } else if (content[pos] == '(') {
          depth++;
          pos++;
        } else if (content[pos] == ')') {
          depth--;
          pos++;
        } else {
          pos++;
        }
      }
      continue;
    }

    if (content[pos] == '<') {
      if ((pos + 1) < content.size() && content[pos + 1] == '<') {
        pos += 2;
      } else {
        pos++;
        while (pos < content.size() && content[pos] != '>') {
          pos++;
        }
        if (pos < content.size()) pos++;
      }
      continue;
    }

    if (content[pos] == '[' || content[pos] == ']' || content[pos] == '{' ||
        content[pos] == '}') {
      pos++;
      continue;
    }

    size_t start = pos;
    while (pos < content.size() && !is_pdf_content_delimiter(content[pos])) {
      pos++;
    }

    if (start == pos) {
      pos++;
      continue;
    }

    std::string_view token = content.substr(start, pos - start);
    if (!is_render_progress_operator(token)) {
      continue;
    }

    count++;
    if (token_eq2(token, 'B', 'I')) {
      skip_inline_image_payload(content, pos);
    }
  }

  return count;
}

// Computes extra progress weight for XObject Do operators beyond the
// baseline count of 1.  Returns 0 when the XObject is not found or is an
// unknown subtype.
//   Image: weight = max(1, w*h/10000) - 1
//   Form:  weight = form_operator_count - 1
static size_t compute_xobject_extra_weight(const std::string& xobj_name,
                                            const Pdf& pdf,
                                            const Dictionary& resources) {
  auto xobj_it = resources.find("XObject");
  if (xobj_it == resources.end()) return 0;
  const Value& xobj_dict = xobj_it->second;
  if (xobj_dict.type != Value::DICTIONARY) return 0;

  auto name_it = xobj_dict.dict.find(xobj_name);
  if (name_it == xobj_dict.dict.end()) return 0;

  Value entry = name_it->second;
  uint32_t obj_num = 0;
  uint16_t obj_gen = 0;
  if (entry.type == Value::REFERENCE) {
    obj_num = entry.ref_object_number;
    obj_gen = entry.ref_generation_number;
    auto res = resolve_reference(pdf, obj_num, obj_gen);
    if (!res.success) return 0;
    entry = std::move(res.value);
  }
  if (entry.type != Value::STREAM) return 0;

  auto sub_it = entry.stream.dict.find("Subtype");
  if (sub_it == entry.stream.dict.end() ||
      sub_it->second.type != Value::NAME)
    return 0;

  if (sub_it->second.name == "Image") {
    int w = 0, h = 0;
    auto w_it = entry.stream.dict.find("Width");
    auto h_it = entry.stream.dict.find("Height");
    if (w_it != entry.stream.dict.end() &&
        w_it->second.type == Value::NUMBER)
      w = static_cast<int>(w_it->second.number);
    if (h_it != entry.stream.dict.end() &&
        h_it->second.type == Value::NUMBER)
      h = static_cast<int>(h_it->second.number);
    if (w <= 0 || h <= 0) return 0;
    size_t weight =
        std::max<size_t>(1, (static_cast<size_t>(w) * h) / 10000);
    return weight - 1;  // Do already counted as 1
  }

  if (sub_it->second.name == "Form") {
    auto dec = decode_stream(pdf, entry, obj_num, obj_gen);
    if (!dec.success || dec.data.empty()) return 0;
    size_t n = count_render_objects_impl(std::string_view(
        reinterpret_cast<const char*>(dec.data.data()), dec.data.size()));
    return n - 1;  // Do already counted as 1
  }

  return 0;
}

// Scans content for "NAME Do" patterns and accumulates extra progress
// weight from Image/Form XObjects.  Uses page-level resources.
static size_t scan_do_extra_weight(std::string_view content,
                                    const Pdf& pdf,
                                    const Dictionary& resources) {
  size_t extra = 0;
  size_t pos = 0;
  std::string last_token;

  while (pos < content.size()) {
    while (pos < content.size() &&
           std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
    if (pos >= content.size()) break;

    if (content[pos] == '%') {
      while (pos < content.size() && content[pos] != '\n' &&
             content[pos] != '\r') {
        pos++;
      }
      continue;
    }

    if (content[pos] == '(') {
      int depth = 1;
      pos++;
      while (pos < content.size() && depth > 0) {
        if (content[pos] == '\\' && (pos + 1) < content.size()) {
          pos += 2;
        } else if (content[pos] == '(') {
          depth++;
          pos++;
        } else if (content[pos] == ')') {
          depth--;
          pos++;
        } else {
          pos++;
        }
      }
      continue;
    }

    if (content[pos] == '<') {
      if ((pos + 1) < content.size() && content[pos + 1] == '<') {
        pos += 2;
      } else {
        pos++;
        while (pos < content.size() && content[pos] != '>') pos++;
        if (pos < content.size()) pos++;
      }
      continue;
    }

    if (content[pos] == '[' || content[pos] == ']' ||
        content[pos] == '{' || content[pos] == '}') {
      pos++;
      continue;
    }

    size_t start = pos;
    while (pos < content.size() &&
           !is_pdf_content_delimiter(content[pos])) {
      pos++;
    }
    if (start == pos) {
      pos++;
      continue;
    }

    std::string_view token(content.data() + start, pos - start);
    if (token_eq2(token, 'D', 'o')) {
      if (!last_token.empty() && last_token[0] == '/') {
        extra += compute_xobject_extra_weight(last_token.substr(1),
                                               pdf, resources);
      }
    }
    last_token.assign(token.data(), token.size());
  }
  return extra;
}

static bool is_rectangular_path(const std::vector<lvg::PathCommand>& commands,
                                const std::vector<lvg::Point>& points,
                                LightVGRect& rect) {
  if (commands.size() != 5 || points.size() != 4) {
    return false;
  }

  if (commands[0] != lvg::PathCommand::MoveTo ||
      commands[1] != lvg::PathCommand::LineTo ||
      commands[2] != lvg::PathCommand::LineTo ||
      commands[3] != lvg::PathCommand::LineTo ||
      commands[4] != lvg::PathCommand::Close) {
    return false;
  }

  float min_x = points[0].x;
  float max_x = points[0].x;
  float min_y = points[0].y;
  float max_y = points[0].y;
  for (size_t i = 1; i < points.size(); i++) {
    min_x = std::min(min_x, points[i].x);
    max_x = std::max(max_x, points[i].x);
    min_y = std::min(min_y, points[i].y);
    max_y = std::max(max_y, points[i].y);
  }

  int corners_at_edges = 0;
  constexpr float kEps = 0.01f;
  for (const auto& point : points) {
    bool at_x_edge = (std::abs(point.x - min_x) < kEps) ||
                     (std::abs(point.x - max_x) < kEps);
    bool at_y_edge = (std::abs(point.y - min_y) < kEps) ||
                     (std::abs(point.y - max_y) < kEps);
    if (at_x_edge && at_y_edge) {
      corners_at_edges++;
    }
  }

  if (corners_at_edges != 4) {
    return false;
  }

  rect.x = min_x;
  rect.y = min_y;
  rect.w = max_x - min_x;
  rect.h = max_y - min_y;
  return rect.w > 0.0f && rect.h > 0.0f;
}

static void append_shape_geometry(lvg::Shape* shape,
                                  const std::vector<lvg::PathCommand>& commands,
                                  const std::vector<lvg::Point>& points) {
  LightVGRect rect;
  if (is_rectangular_path(commands, points, rect)) {
    shape->appendRect(rect.x, rect.y, rect.w, rect.h, 0, 0);
    return;
  }

  shape->appendPath(commands.data(), commands.size(), points.data(),
                    points.size());
}

// C++17 clamp wrapper
template<typename T>
constexpr T clamp14(const T& v, const T& lo, const T& hi) {
  return std::clamp(v, lo, hi);
}

// Improved CMYK to RGB conversion using Ghostscript-style under-color removal.
// The naive formula R=(1-C)*(1-K) loses shadow detail. The additive formula
// R=1-min(1,C+K) preserves more detail in dark regions.
// Input: c, m, y, k in [0, 1] range. Output: r, g, b as uint8_t.
static inline void cmyk_to_rgb(float c, float m, float y, float k,
                                uint8_t& r, uint8_t& g, uint8_t& b) {
  c = clamp14(c, 0.0f, 1.0f);
  m = clamp14(m, 0.0f, 1.0f);
  y = clamp14(y, 0.0f, 1.0f);
  k = clamp14(k, 0.0f, 1.0f);

  // Ghostscript-style: additive under-color removal
  float r_f = 1.0f - std::min(1.0f, c + k);
  float g_f = 1.0f - std::min(1.0f, m + k);
  float b_f = 1.0f - std::min(1.0f, y + k);

  r = static_cast<uint8_t>(r_f * 255.0f + 0.5f);
  g = static_cast<uint8_t>(g_f * 255.0f + 0.5f);
  b = static_cast<uint8_t>(b_f * 255.0f + 0.5f);
}

// WinAnsiEncoding to Unicode mapping table
static const uint16_t kWinAnsiEncoding[256] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
    0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF
};

// MacRomanEncoding to Unicode mapping table
static const uint16_t kMacRomanEncoding[256] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
    0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
};

// Adobe-Japan1 CID to Unicode mapping. Best-effort fallback when the PDF
// omits a ToUnicode CMap; authoritative coverage requires the Adobe-Japan1
// CMap resources. Regular closed-form ranges live in
// adobe_japan1_cid_to_unicode_regular; this function adds a hand-maintained
// table of common punctuation and kanji that show up in sample corpora.
static uint32_t adobe_japan1_cid_to_unicode(uint32_t cid) {
  if (uint32_t u = adobe_japan1_cid_to_unicode_regular(cid)) return u;

  // Common punctuation and symbols
  static const std::map<uint32_t, uint32_t> punct_map = {
    {631, 0x3001},  // 、 IDEOGRAPHIC COMMA
    {632, 0x3002},  // 。 IDEOGRAPHIC FULL STOP
    {633, 0xFF0C},  // ， FULLWIDTH COMMA
    {634, 0xFF0E},  // ． FULLWIDTH FULL STOP
    {635, 0x30FB},  // ・ KATAKANA MIDDLE DOT
    {636, 0xFF1A},  // ： FULLWIDTH COLON
    {637, 0xFF1B},  // ； FULLWIDTH SEMICOLON
    {638, 0x3059},  // す
    {639, 0xFF1F},  // ？ FULLWIDTH QUESTION MARK
    {640, 0xFF01},  // ！ FULLWIDTH EXCLAMATION MARK
    {674, 0x306E},  // の
    {675, 0x306F},  // は
    {676, 0x3070},  // ば
    {677, 0x3071},  // ぱ
    {678, 0x3072},  // ひ
    {720, 0x300C},  // 「 LEFT CORNER BRACKET
    {721, 0x300D},  // 」 RIGHT CORNER BRACKET
    {722, 0x300E},  // 『 LEFT WHITE CORNER BRACKET
    {723, 0x300F},  // 』 RIGHT WHITE CORNER BRACKET
    {724, 0xFF08},  // （ FULLWIDTH LEFT PARENTHESIS
    {725, 0xFF09},  // ） FULLWIDTH RIGHT PARENTHESIS
    {783, 0x30C8},  // ト
    {803, 0x30E9},  // ラ
  };

  auto punct_it = punct_map.find(cid);
  if (punct_it != punct_map.end()) {
    return punct_it->second;
  }

  // Common Kanji mappings (subset of most frequent characters)
  static const std::map<uint32_t, uint32_t> kanji_map = {
    // Numbers/counting
    {1866, 0x4E00}, // 一
    {1867, 0x4E8C}, // 二
    {1868, 0x4E09}, // 三
    {1869, 0x56DB}, // 四
    {1870, 0x4E94}, // 五
    {1871, 0x516D}, // 六
    {1872, 0x4E03}, // 七
    {1873, 0x516B}, // 八
    {1874, 0x4E5D}, // 九
    {1875, 0x5341}, // 十
    {1876, 0x767E}, // 百
    {1877, 0x5343}, // 千
    {1878, 0x4E07}, // 万
    {1879, 0x5104}, // 億
    // Common characters
    {1908, 0x5927}, // 大
    {1909, 0x5C0F}, // 小
    {1910, 0x4E2D}, // 中
    {1911, 0x9AD8}, // 高
    {1912, 0x4F4E}, // 低
    {1913, 0x65B0}, // 新
    {1914, 0x53E4}, // 古
    {1915, 0x826F}, // 良
    {1916, 0x60AA}, // 悪
    {1917, 0x591A}, // 多
    {1918, 0x5C11}, // 少
    {1919, 0x5168}, // 全
    {1920, 0x534A}, // 半
    {1921, 0x6700}, // 最
    {1922, 0x521D}, // 初
    {1923, 0x5F8C}, // 後
    {1924, 0x524D}, // 前
    {1925, 0x6B21}, // 次
    {1926, 0x4ECA}, // 今
    {1927, 0x6628}, // 昨
    {1928, 0x660E}, // 明
    {1929, 0x65E5}, // 日
    {1930, 0x6708}, // 月
    {1931, 0x5E74}, // 年
    {1932, 0x6642}, // 時
    {1933, 0x5206}, // 分
    {1934, 0x79D2}, // 秒
    {1935, 0x9031}, // 週
    {1936, 0x66DC}, // 曜
    {1937, 0x706B}, // 火
    {1938, 0x6C34}, // 水
    {1939, 0x6728}, // 木
    {1940, 0x91D1}, // 金
    {1941, 0x571F}, // 土
    {1942, 0x65E5}, // 日
    {1943, 0x6625}, // 春
    {1944, 0x590F}, // 夏
    {1945, 0x79CB}, // 秋
    {1946, 0x51AC}, // 冬
    {1947, 0x6771}, // 東
    {1948, 0x897F}, // 西
    {1949, 0x5357}, // 南
    {1950, 0x5317}, // 北
    {1951, 0x4E0A}, // 上
    {1952, 0x8A00}, // 言
    {1953, 0x4E0B}, // 下
    {1954, 0x5DE6}, // 左
    {1955, 0x53F3}, // 右
    {1956, 0x5185}, // 内
    {1957, 0x5916}, // 外
    // More common kanji
    {2028, 0x4EBA}, // 人
    {2029, 0x5B50}, // 子
    {2030, 0x5973}, // 女
    {2031, 0x7537}, // 男
    {2032, 0x7236}, // 父
    {2033, 0x6BCD}, // 母
    {2034, 0x5144}, // 兄
    {2035, 0x59C9}, // 姉
    {2036, 0x5F1F}, // 弟
    {2037, 0x59B9}, // 妹
    {2038, 0x592B}, // 夫
    {2039, 0x59BB}, // 妻
    {2040, 0x53CB}, // 友
    {2041, 0x5148}, // 先
    {2042, 0x751F}, // 生
    {2043, 0x5B66}, // 学
    {2044, 0x6821}, // 校
    {2045, 0x793E}, // 社
    {2046, 0x4F1A}, // 会
    {2047, 0x56FD}, // 国
    {2048, 0x5E02}, // 市
    {2049, 0x753A}, // 町
    {2050, 0x6751}, // 村
    {2051, 0x5C71}, // 山
    {2052, 0x5DDD}, // 川
    {2053, 0x6D77}, // 海
    {2054, 0x7A7A}, // 空
    {2055, 0x5730}, // 地
    {2056, 0x5929}, // 天
    {2057, 0x98A8}, // 風
    {2058, 0x96E8}, // 雨
    {2059, 0x96EA}, // 雪
    {2060, 0x82B1}, // 花
    {2061, 0x8349}, // 草
    {2062, 0x6728}, // 木
    {2063, 0x68EE}, // 森
    {2064, 0x6797}, // 林
    // Language/expression kanji
    {2248, 0x80FD}, // 能
    {2269, 0x660E}, // 明
    // More kanji
    {2890, 0x8A9E}, // 語
    {2956, 0x529B}, // 力
    {3592, 0x8A66}, // 試
    {3824, 0x65E5}, // 日
    {4011, 0x672C}, // 本
    {4782, 0x8A9E}, // 語
    // Additional common kanji
    {2100, 0x898B}, // 見
    {2101, 0x805E}, // 聞
    {2102, 0x8AAD}, // 読
    {2103, 0x66F8}, // 書
    {2104, 0x8A71}, // 話
    {2105, 0x601D}, // 思
    {2106, 0x8003}, // 考
    {2107, 0x77E5}, // 知
    {2108, 0x4F7F}, // 使
    {2109, 0x4F5C}, // 作
    {2110, 0x52D5}, // 動
    {2111, 0x6B62}, // 止
    {2112, 0x884C}, // 行
    {2113, 0x6765}, // 来
    {2114, 0x5E30}, // 帰
    {2115, 0x5165}, // 入
    {2116, 0x51FA}, // 出
    {2117, 0x7ACB}, // 立
    {2118, 0x5EA7}, // 座
    {2119, 0x98DF}, // 食
    {2120, 0x98F2}, // 飲
    {2121, 0x7720}, // 睡
    {2122, 0x8D77}, // 起
    {2123, 0x5BC4}, // 寄
    {2124, 0x4E57}, // 乗
    {2125, 0x964D}, // 降
    {2126, 0x8D70}, // 走
    {2127, 0x6B69}, // 歩
    {2128, 0x6CF3}, // 泳
    {2129, 0x98DB}, // 飛
    {2130, 0x843D}, // 落
    // Common verbs and adjectives
    {2150, 0x3042}, // あ (hiragana a - sometimes mapped here)
    {2151, 0x3044}, // い
    {2152, 0x3046}, // う
    {2153, 0x3048}, // え
    {2154, 0x304A}, // お
    // Additional important kanji
    {2197, 0x793A}, // 示
    {2198, 0x4F1D}, // 伝
    {2199, 0x5831}, // 報
    {2200, 0x77E5}, // 知
  };

  auto it = kanji_map.find(cid);
  if (it != kanji_map.end()) {
    return it->second;
  }

  // Not found - return 0 to indicate no mapping
  return 0;
}

// Common Adobe glyph name to Unicode mapping
// glyph_name_to_unicode is now in font-unicode-map.hh (shared, sorted-array+bsearch)

// ========================================================================
// Mesh Shading Support - Based on PDFium implementation
// Copyright 2016 The PDFium Authors
// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com
// ========================================================================

// BitStream reader for reading packed bits from mesh data
class BitStream {
public:
  BitStream(const uint8_t* data, size_t size) : data_(data), size_(size), bit_pos_(0) {}

  bool is_eof() const { return bit_pos_ >= size_ * 8; }
  size_t bits_remaining() const { return size_ * 8 - bit_pos_; }

  uint32_t get_bits(uint32_t num_bits) {
    if (num_bits == 0 || num_bits > 32) return 0;
    if (bit_pos_ + num_bits > size_ * 8) return 0;

    uint32_t result = 0;
    while (num_bits > 0) {
      size_t byte_pos = bit_pos_ / 8;
      size_t bit_offset = bit_pos_ % 8;
      size_t bits_in_byte = std::min(num_bits, 8 - static_cast<uint32_t>(bit_offset));

      uint8_t mask = static_cast<uint8_t>((1 << bits_in_byte) - 1);
      uint8_t value = (data_[byte_pos] >> (8 - bit_offset - bits_in_byte)) & mask;

      result = (result << bits_in_byte) | value;
      bit_pos_ += bits_in_byte;
      num_bits -= bits_in_byte;
    }
    return result;
  }

  void byte_align() {
    if (bit_pos_ % 8 != 0) {
      bit_pos_ = ((bit_pos_ / 8) + 1) * 8;
    }
  }

  void skip_bits(uint32_t num_bits) {
    bit_pos_ += num_bits;
  }

private:
  const uint8_t* data_;
  size_t size_;
  size_t bit_pos_;
};

// Mesh vertex with position and RGB color
struct MeshVertex {
  float x, y;
  float r, g, b;  // RGB in [0,1] range
};

// Decode coordinate from bit-packed value
static float decode_coord(uint32_t encoded, uint32_t bits_per_coord,
                          float min_val, float max_val) {
  if (bits_per_coord == 0) return min_val;
  uint32_t max_encoded = (bits_per_coord == 32) ? 0xFFFFFFFF : (1u << bits_per_coord) - 1;
  if (max_encoded == 0) return min_val;
  return min_val + (encoded / static_cast<float>(max_encoded)) * (max_val - min_val);
}

// Decode color component from bit-packed value
static float decode_component(uint32_t encoded, uint32_t bits_per_component,
                               float min_val, float max_val) {
  if (bits_per_component == 0) return min_val;
  uint32_t max_encoded = (1u << bits_per_component) - 1;
  if (max_encoded == 0) return min_val;
  return min_val + (encoded / static_cast<float>(max_encoded)) * (max_val - min_val);
}

// Draw a Gouraud-shaded triangle to a bitmap (scanline algorithm)
static void draw_gouraud_triangle(uint32_t* bitmap, int width, int height,
                                   const MeshVertex& v0, const MeshVertex& v1, const MeshVertex& v2) {
  // Find bounding box
  float min_y = std::min({v0.y, v1.y, v2.y});
  float max_y = std::max({v0.y, v1.y, v2.y});

  if (min_y == max_y) return;  // Degenerate triangle

  int min_yi = std::max(static_cast<int>(std::floor(min_y)), 0);
  int max_yi = std::min(static_cast<int>(std::ceil(max_y)), height - 1);

  // For each scanline
  for (int y = min_yi; y <= max_yi; ++y) {
    float fy = static_cast<float>(y);

    // Find intersections with triangle edges
    struct Intersection {
      float x;
      float r, g, b;
    };
    std::vector<Intersection> intersections;

    // Check each edge
    const MeshVertex* edges[3][2] = {{&v0, &v1}, {&v1, &v2}, {&v2, &v0}};

    for (int i = 0; i < 3; ++i) {
      const MeshVertex* a = edges[i][0];
      const MeshVertex* b = edges[i][1];

      if (a->y == b->y) continue;  // Horizontal edge

      // Check if scanline intersects this edge
      bool intersects = (a->y < b->y) ? (fy >= a->y && fy <= b->y) : (fy >= b->y && fy <= a->y);
      if (!intersects) continue;

      // Interpolate along edge
      float t = (fy - a->y) / (b->y - a->y);
      Intersection isect;
      isect.x = a->x + t * (b->x - a->x);
      isect.r = a->r + t * (b->r - a->r);
      isect.g = a->g + t * (b->g - a->g);
      isect.b = a->b + t * (b->b - a->b);
      intersections.push_back(isect);
    }

    if (intersections.size() != 2) continue;  // Should have exactly 2 intersections

    // Sort intersections by x
    if (intersections[0].x > intersections[1].x) {
      std::swap(intersections[0], intersections[1]);
    }

    int x_start = std::max(static_cast<int>(std::floor(intersections[0].x)), 0);
    int x_end = std::min(static_cast<int>(std::ceil(intersections[1].x)), width - 1);

    if (x_start >= x_end) continue;

    // Interpolate color across scanline
    float dx = intersections[1].x - intersections[0].x;
    if (dx == 0) continue;

    uint32_t* row = bitmap + y * width;
    for (int x = x_start; x <= x_end; ++x) {
      float t = (x - intersections[0].x) / dx;
      float r = intersections[0].r + t * (intersections[1].r - intersections[0].r);
      float g = intersections[0].g + t * (intersections[1].g - intersections[0].g);
      float b = intersections[0].b + t * (intersections[1].b - intersections[0].b);

      uint8_t ri = static_cast<uint8_t>(clamp14(r * 255.0f, 0.0f, 255.0f));
      uint8_t gi = static_cast<uint8_t>(clamp14(g * 255.0f, 0.0f, 255.0f));
      uint8_t bi = static_cast<uint8_t>(clamp14(b * 255.0f, 0.0f, 255.0f));

      row[x] = 0xFF000000 | (bi << 16) | (gi << 8) | ri;  // BGRA format
    }
  }
}

// Cubic Bezier patch for Coons/Tensor-product patch meshes
struct BezierPatch {
  float points[4][4][2];  // 4x4 grid of control points (x, y)
  float colors[4][3];     // 4 corner colors (r, g, b)

  bool is_small() const {
    // Check if patch is smaller than 2x2 pixels
    float min_x = points[0][0][0], max_x = points[0][0][0];
    float min_y = points[0][0][1], max_y = points[0][0][1];

    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        min_x = std::min(min_x, points[i][j][0]);
        max_x = std::max(max_x, points[i][j][0]);
        min_y = std::min(min_y, points[i][j][1]);
        max_y = std::max(max_y, points[i][j][1]);
      }
    }

    return (max_x - min_x < 2.0f) && (max_y - min_y < 2.0f);
  }

  // Subdivide patch vertically (split into top and bottom)
  void subdivide_vertical(BezierPatch& top, BezierPatch& bottom) const {
    for (int x = 0; x < 4; ++x) {
      // DeCasteljau subdivision for each row
      float p0[2] = {points[x][0][0], points[x][0][1]};
      float p1[2] = {points[x][1][0], points[x][1][1]};
      float p2[2] = {points[x][2][0], points[x][2][1]};
      float p3[2] = {points[x][3][0], points[x][3][1]};

      float q0[2] = {(p0[0] + p1[0]) * 0.5f, (p0[1] + p1[1]) * 0.5f};
      float q1[2] = {(p1[0] + p2[0]) * 0.5f, (p1[1] + p2[1]) * 0.5f};
      float q2[2] = {(p2[0] + p3[0]) * 0.5f, (p2[1] + p3[1]) * 0.5f};

      float r0[2] = {(q0[0] + q1[0]) * 0.5f, (q0[1] + q1[1]) * 0.5f};
      float r1[2] = {(q1[0] + q2[0]) * 0.5f, (q1[1] + q2[1]) * 0.5f};

      float s[2] = {(r0[0] + r1[0]) * 0.5f, (r0[1] + r1[1]) * 0.5f};

      top.points[x][0][0] = p0[0]; top.points[x][0][1] = p0[1];
      top.points[x][1][0] = q0[0]; top.points[x][1][1] = q0[1];
      top.points[x][2][0] = r0[0]; top.points[x][2][1] = r0[1];
      top.points[x][3][0] = s[0];  top.points[x][3][1] = s[1];

      bottom.points[x][0][0] = s[0];  bottom.points[x][0][1] = s[1];
      bottom.points[x][1][0] = r1[0]; bottom.points[x][1][1] = r1[1];
      bottom.points[x][2][0] = q2[0]; bottom.points[x][2][1] = q2[1];
      bottom.points[x][3][0] = p3[0]; bottom.points[x][3][1] = p3[1];
    }

    // Interpolate colors
    for (int i = 0; i < 3; ++i) {
      top.colors[0][i] = colors[0][i];
      top.colors[1][i] = (colors[0][i] + colors[1][i]) * 0.5f;
      top.colors[2][i] = (colors[0][i] + colors[1][i]) * 0.5f;
      top.colors[3][i] = colors[1][i];

      bottom.colors[0][i] = (colors[0][i] + colors[1][i]) * 0.5f;
      bottom.colors[1][i] = colors[1][i];
      bottom.colors[2][i] = colors[2][i];
      bottom.colors[3][i] = (colors[2][i] + colors[3][i]) * 0.5f;
    }
  }

  // Subdivide patch horizontally (split into left and right)
  void subdivide_horizontal(BezierPatch& left, BezierPatch& right) const {
    for (int y = 0; y < 4; ++y) {
      float p0[2] = {points[0][y][0], points[0][y][1]};
      float p1[2] = {points[1][y][0], points[1][y][1]};
      float p2[2] = {points[2][y][0], points[2][y][1]};
      float p3[2] = {points[3][y][0], points[3][y][1]};

      float q0[2] = {(p0[0] + p1[0]) * 0.5f, (p0[1] + p1[1]) * 0.5f};
      float q1[2] = {(p1[0] + p2[0]) * 0.5f, (p1[1] + p2[1]) * 0.5f};
      float q2[2] = {(p2[0] + p3[0]) * 0.5f, (p2[1] + p3[1]) * 0.5f};

      float r0[2] = {(q0[0] + q1[0]) * 0.5f, (q0[1] + q1[1]) * 0.5f};
      float r1[2] = {(q1[0] + q2[0]) * 0.5f, (q1[1] + q2[1]) * 0.5f};

      float s[2] = {(r0[0] + r1[0]) * 0.5f, (r0[1] + r1[1]) * 0.5f};

      left.points[0][y][0] = p0[0]; left.points[0][y][1] = p0[1];
      left.points[1][y][0] = q0[0]; left.points[1][y][1] = q0[1];
      left.points[2][y][0] = r0[0]; left.points[2][y][1] = r0[1];
      left.points[3][y][0] = s[0];  left.points[3][y][1] = s[1];

      right.points[0][y][0] = s[0];  right.points[0][y][1] = s[1];
      right.points[1][y][0] = r1[0]; right.points[1][y][1] = r1[1];
      right.points[2][y][0] = q2[0]; right.points[2][y][1] = q2[1];
      right.points[3][y][0] = p3[0]; right.points[3][y][1] = p3[1];
    }

    // Interpolate colors
    for (int i = 0; i < 3; ++i) {
      left.colors[0][i] = colors[0][i];
      left.colors[1][i] = (colors[0][i] + colors[3][i]) * 0.5f;
      left.colors[2][i] = (colors[0][i] + colors[3][i]) * 0.5f;
      left.colors[3][i] = colors[3][i];

      right.colors[0][i] = (colors[0][i] + colors[3][i]) * 0.5f;
      right.colors[1][i] = colors[3][i];
      right.colors[2][i] = colors[2][i];
      right.colors[3][i] = (colors[1][i] + colors[2][i]) * 0.5f;
    }
  }

  // Draw patch as filled polygon (for small patches)
  void draw_to_bitmap(uint32_t* bitmap, int width, int height) const {
    // Get average color
    float r = (colors[0][0] + colors[1][0] + colors[2][0] + colors[3][0]) * 0.25f;
    float g = (colors[0][1] + colors[1][1] + colors[2][1] + colors[3][1]) * 0.25f;
    float b = (colors[0][2] + colors[1][2] + colors[2][2] + colors[3][2]) * 0.25f;

    // Create triangles from patch boundary (corners)
    MeshVertex v0 = {points[0][0][0], points[0][0][1], colors[0][0], colors[0][1], colors[0][2]};
    MeshVertex v1 = {points[3][0][0], points[3][0][1], colors[3][0], colors[3][1], colors[3][2]};
    MeshVertex v2 = {points[3][3][0], points[3][3][1], colors[2][0], colors[2][1], colors[2][2]};
    MeshVertex v3 = {points[0][3][0], points[0][3][1], colors[1][0], colors[1][1], colors[1][2]};

    // Draw as two triangles
    draw_gouraud_triangle(bitmap, width, height, v0, v1, v2);
    draw_gouraud_triangle(bitmap, width, height, v0, v2, v3);
  }
};

// Recursively subdivide and draw a Bezier patch
static void draw_patch_recursive(uint32_t* bitmap, int width, int height,
                                  const BezierPatch& patch, int depth = 0) {
  const int max_depth = 8;  // Maximum subdivision depth

  if (patch.is_small() || depth >= max_depth) {
    // Small enough - draw it
    patch.draw_to_bitmap(bitmap, width, height);
    return;
  }

  // Subdivide into 4 patches
  BezierPatch top, bottom;
  patch.subdivide_vertical(top, bottom);

  BezierPatch top_left, top_right;
  top.subdivide_horizontal(top_left, top_right);

  BezierPatch bottom_left, bottom_right;
  bottom.subdivide_horizontal(bottom_left, bottom_right);

  // Recursively draw sub-patches
  draw_patch_recursive(bitmap, width, height, top_left, depth + 1);
  draw_patch_recursive(bitmap, width, height, top_right, depth + 1);
  draw_patch_recursive(bitmap, width, height, bottom_left, depth + 1);
  draw_patch_recursive(bitmap, width, height, bottom_right, depth + 1);
}

// ICC Profile parsing helpers
enum class ICCColorSpaceType {
  Unknown,
  Gray,
  RGB,
  CMYK,
  Lab
};

struct ICCProfileInfo {
  uint32_t size{0};
  uint32_t version{0};
  ICCColorSpaceType color_space{ICCColorSpaceType::Unknown};
  std::string description;
  bool is_srgb{false};
  bool is_adobe_rgb{false};
};

// Parse ICC profile header to extract basic info
static ICCProfileInfo parse_icc_profile(const std::vector<uint8_t>& data) {
  ICCProfileInfo info;

  if (data.size() < 128) {
    return info;  // ICC header is at least 128 bytes
  }

  // Profile size (bytes 0-3, big-endian)
  info.size = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];

  // Profile version (bytes 8-11)
  info.version = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];

  // Color space signature (bytes 16-19)
  uint32_t cs_sig = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];

  // 'RGB ' = 0x52474220, 'CMYK' = 0x434D594B, 'GRAY' = 0x47524159, 'Lab ' = 0x4C616220
  if (cs_sig == 0x52474220) {
    info.color_space = ICCColorSpaceType::RGB;
  } else if (cs_sig == 0x434D594B) {
    info.color_space = ICCColorSpaceType::CMYK;
  } else if (cs_sig == 0x47524159) {
    info.color_space = ICCColorSpaceType::Gray;
  } else if (cs_sig == 0x4C616220) {
    info.color_space = ICCColorSpaceType::Lab;
  }

  // Check for sRGB by looking at profile description tag
  // Tag table starts at byte 128, first 4 bytes = tag count
  if (data.size() > 132) {
    uint32_t tag_count = (data[128] << 24) | (data[129] << 16) | (data[130] << 8) | data[131];

    // Each tag entry is 12 bytes: signature(4) + offset(4) + size(4)
    for (uint32_t i = 0; i < tag_count && (132 + i * 12 + 11) < data.size(); i++) {
      size_t entry_offset = 132 + i * 12;
      uint32_t tag_sig = (data[entry_offset] << 24) | (data[entry_offset + 1] << 16) |
                         (data[entry_offset + 2] << 8) | data[entry_offset + 3];
      uint32_t tag_offset = (data[entry_offset + 4] << 24) | (data[entry_offset + 5] << 16) |
                            (data[entry_offset + 6] << 8) | data[entry_offset + 7];
      uint32_t tag_size = (data[entry_offset + 8] << 24) | (data[entry_offset + 9] << 16) |
                          (data[entry_offset + 10] << 8) | data[entry_offset + 11];

      // 'desc' = 0x64657363 - profile description
      if (tag_sig == 0x64657363 && tag_offset + tag_size <= data.size()) {
        // Read description (simplified - just check for keywords)
        std::string desc;
        for (size_t j = tag_offset; j < tag_offset + tag_size && j < data.size(); j++) {
          if (data[j] >= 32 && data[j] < 127) {
            desc += static_cast<char>(data[j]);
          }
        }
        info.description = desc;

        // Check for common profile names
        if (desc.find("sRGB") != std::string::npos ||
            desc.find("IEC 61966-2.1") != std::string::npos) {
          info.is_srgb = true;
        } else if (desc.find("Adobe RGB") != std::string::npos) {
          info.is_adobe_rgb = true;
        }
      }
    }
  }

  return info;
}

// Convert ICC-based pixel data to sRGB
// This is a simplified conversion that handles common cases
static void convert_icc_to_srgb(
    const std::vector<uint8_t>& src_data,
    std::vector<uint8_t>& dst_data,
    int width, int height,
    const ColorSpace& color_space) {

  ICCProfileInfo profile_info;
  if (!color_space.icc_profile_data.empty()) {
    profile_info = parse_icc_profile(color_space.icc_profile_data);
  }

  int num_components = color_space.num_components;
  if (num_components == 0) {
    // Guess from ICC profile
    switch (profile_info.color_space) {
      case ICCColorSpaceType::Gray: num_components = 1; break;
      case ICCColorSpaceType::CMYK: num_components = 4; break;
      case ICCColorSpaceType::Lab: num_components = 3; break;
      default: num_components = 3; break;
    }
  }

  dst_data.resize(width * height * 3);  // Output is always RGB

  if (num_components == 1) {
    // Grayscale - simple expansion
    for (int i = 0; i < width * height; i++) {
      uint8_t gray = (i < static_cast<int>(src_data.size())) ? src_data[i] : 0;
      dst_data[i * 3] = gray;
      dst_data[i * 3 + 1] = gray;
      dst_data[i * 3 + 2] = gray;
    }
  } else if (num_components == 3) {
    // RGB profile
    if (profile_info.is_srgb) {
      // sRGB - direct copy
      for (int i = 0; i < width * height; i++) {
        int src_idx = i * 3;
        if (src_idx + 2 < static_cast<int>(src_data.size())) {
          dst_data[i * 3] = src_data[src_idx];
          dst_data[i * 3 + 1] = src_data[src_idx + 1];
          dst_data[i * 3 + 2] = src_data[src_idx + 2];
        }
      }
    } else if (profile_info.is_adobe_rgb) {
      // Adobe RGB to sRGB approximate conversion
      // Adobe RGB has gamma 2.2 and wider gamut
      for (int i = 0; i < width * height; i++) {
        int src_idx = i * 3;
        if (src_idx + 2 < static_cast<int>(src_data.size())) {
          // Simplified conversion: apply gamut compression
          float r = src_data[src_idx] / 255.0f;
          float g = src_data[src_idx + 1] / 255.0f;
          float b = src_data[src_idx + 2] / 255.0f;

          // Adobe RGB to XYZ (D65)
          float x = 0.5767309f * r + 0.1855540f * g + 0.1881852f * b;
          float y = 0.2973769f * r + 0.6273491f * g + 0.0752741f * b;
          float z = 0.0270343f * r + 0.0706872f * g + 0.9911085f * b;

          // XYZ to sRGB
          float sr = 3.2404542f * x - 1.5371385f * y - 0.4985314f * z;
          float sg = -0.9692660f * x + 1.8760108f * y + 0.0415560f * z;
          float sb = 0.0556434f * x - 0.2040259f * y + 1.0572252f * z;

          // Clamp and convert to 8-bit
          dst_data[i * 3] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, sr)) * 255);
          dst_data[i * 3 + 1] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, sg)) * 255);
          dst_data[i * 3 + 2] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, sb)) * 255);
        }
      }
    } else {
      // Unknown RGB profile - try to use full ICC profile parsing
      color::IccProfileInfo full_profile;
      if (!color_space.icc_profile_data.empty()) {
        uint64_t icc_hash = 0xcbf29ce484222325ULL;
        for (uint8_t b : color_space.icc_profile_data) {
          icc_hash ^= b;
          icc_hash *= 0x1000000000000043ULL;
        }

        if (!SharedIccCache::instance().find(icc_hash, full_profile)) {
          full_profile = color::parse_icc_profile(
              color_space.icc_profile_data.data(),
              color_space.icc_profile_data.size());

          if (full_profile.valid) {
            SharedIccCache::instance().store(icc_hash, full_profile);
          }
        }
      }

      if (full_profile.valid && full_profile.is_matrix_profile) {
        // Use TRC curves and colorant matrix for accurate conversion
        for (int i = 0; i < width * height; i++) {
          int src_idx = i * 3;
          if (src_idx + 2 < static_cast<int>(src_data.size())) {
            float components[3] = {
                src_data[src_idx] / 255.0f,
                src_data[src_idx + 1] / 255.0f,
                src_data[src_idx + 2] / 255.0f
            };
            color::RGB rgb = color::iccbased_to_rgb(components, 3, full_profile);
            dst_data[i * 3] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, rgb.r)) * 255);
            dst_data[i * 3 + 1] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, rgb.g)) * 255);
            dst_data[i * 3 + 2] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, rgb.b)) * 255);
          }
        }
      } else {
        // Fallback - direct copy assuming sRGB-like
        for (int i = 0; i < width * height; i++) {
          int src_idx = i * 3;
          if (src_idx + 2 < static_cast<int>(src_data.size())) {
            dst_data[i * 3] = src_data[src_idx];
            dst_data[i * 3 + 1] = src_data[src_idx + 1];
            dst_data[i * 3 + 2] = src_data[src_idx + 2];
          }
        }
      }
    }
  } else if (num_components == 4) {
    // CMYK profile - convert to RGB
    for (int i = 0; i < width * height; i++) {
      int src_idx = i * 4;
      if (src_idx + 3 < static_cast<int>(src_data.size())) {
        cmyk_to_rgb(src_data[src_idx] / 255.0f, src_data[src_idx + 1] / 255.0f,
                    src_data[src_idx + 2] / 255.0f, src_data[src_idx + 3] / 255.0f,
                    dst_data[i * 3], dst_data[i * 3 + 1], dst_data[i * 3 + 2]);
      }
    }
  }
}

// Map character code to Unicode based on font encoding
// Uses shared glyph_name_to_unicode from font-unicode-map.hh
static uint32_t map_char_to_unicode(uint32_t char_code, const BaseFont* font) {
  return map_char_to_unicode_generic(
      char_code, font, kWinAnsiEncoding, kMacRomanEncoding,
      glyph_name_to_unicode,
      [](uint32_t cid) { return adobe_japan1_cid_to_unicode(cid); });
}

LightVGBackend::LightVGBackend() {
  if (lvg::Initializer::init(0) != lvg::Result::Success) {
    NANOPDF_LOG_ERROR("LightVG", "Failed to initialize lvg engine");
  }
}

LightVGBackend::~LightVGBackend() {
  if (canvas_) {
    delete canvas_;
    canvas_ = nullptr;
  }
  lvg::Initializer::term();
}

bool LightVGBackend::initialize(uint32_t width, uint32_t height) {
  if (initialized_ && canvas_ && width_ == width && height_ == height) {
    return true;
  }

  width_ = width;
  height_ = height;

  // Create buffer for rendering
  buffer_.resize(width * height);

  // Create software canvas (v1.0+ API returns raw pointer)
  if (!canvas_) {
    canvas_ = lvg::SwCanvas::gen();
  }
  if (!canvas_) {
    return false;
  }

  // Set target buffer (v1.0+ API uses lvg::ColorSpace enum)
  if (canvas_->target(reinterpret_cast<uint32_t*>(buffer_.data()),
                     width, width, height,
                     lvg::ColorSpace::ABGR8888) != lvg::Result::Success) {
    return false;
  }

  initialized_ = true;
  return true;
}

bool LightVGBackend::begin_scene() {
  if (!initialized_) {
    return false;
  }

  // Create a new scene (v1.0+ API returns raw pointer)
  scene_ = lvg::Scene::gen();
  if (!scene_) {
    return false;
  }

  return true;
}

bool LightVGBackend::end_scene() {
  if (!initialized_ || !scene_) {
    return false;
  }

  // Hand the scene over to the canvas; canvas owns it from here.
  if (canvas_->add(scene_) != lvg::Result::Success) {
    scene_ = nullptr;
    return false;
  }
  scene_ = nullptr;

  // Draw the canvas. Opaque page backgrounds can pre-fill the target buffer
  // and skip LightVG's transparent clear pass.
  const bool clear_canvas = clear_canvas_on_draw_;
  clear_canvas_on_draw_ = true;
  canvas_->setOutputSwizzle(!direct_bgra_output_enabled_);
  if (canvas_->draw(clear_canvas) != lvg::Result::Success) {
    canvas_->setOutputSwizzle(true);
    return false;
  }
  canvas_->setOutputSwizzle(true);

  // Sync drawing
  if (canvas_->sync() != lvg::Result::Success) {
    return false;
  }
  if (canvas_->remove() != lvg::Result::Success) {
    return false;
  }

  return true;
}

bool LightVGBackend::draw_rectangle(float x, float y, float width, float height,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = lvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Append rectangle
  shape->appendRect(x, y, width, height, 0, 0);

  // Set fill color
  shape->fill(r, g, b, a);

  // Hand the shape over to the scene; scene owns it from here.
  if (scene_->add(shape) != lvg::Result::Success) {
    return false;
  }

  return true;
}

bool LightVGBackend::draw_circle(float cx, float cy, float radius,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = lvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Append circle
  shape->appendCircle(cx, cy, radius, radius);

  // Set fill color
  shape->fill(r, g, b, a);

  // Add to scene (v1.0+ API takes raw Paint*, scene owns it after push)
  if (scene_->add(shape) != lvg::Result::Success) {
    return false;
  }

  return true;
}

bool LightVGBackend::draw_path(const std::vector<lvg::PathCommand>& cmds,
                             const std::vector<lvg::Point>& pts,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = lvg::Shape::gen();
  if (!shape) {
    return false;
  }

  append_shape_geometry(shape, cmds, pts);

  // Set fill color
  shape->fill(r, g, b, a);

  // Add to scene with clipping if active
  return push_with_clip(shape);
}

// Render a Type 3 font glyph by executing its CharProc content stream
bool LightVGBackend::render_type3_glyph(const Type3Font* type3_font, const std::string& glyph_name,
                                        float x, float y, float size,
                                        uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!type3_font || !current_pdf_ || glyph_name.empty()) {
    return false;
  }

  // Look up the glyph procedure in CharProcs
  auto proc_it = type3_font->char_procs.find(glyph_name);
  if (proc_it == type3_font->char_procs.end()) {
    // Glyph not found - draw tofu box placeholder
    draw_missing_glyph_placeholder(x, y, size, r, g, b, a);
    return true;
  }

  // Resolve the glyph procedure (it's usually a stream)
  Value proc_value = proc_it->second;
  if (proc_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_, proc_value.ref_object_number,
                                       proc_value.ref_generation_number);
    if (!resolved.success) {
      return false;
    }
    proc_value = resolved.value;
  }

  if (proc_value.type != Value::STREAM) {
    return false;
  }

  // Decode the glyph content stream
  auto decoded = decode_stream(*current_pdf_, proc_value);
  if (!decoded.success || decoded.data.empty()) {
    return false;
  }

  // Calculate transformation based on font matrix and size
  // Type 3 fonts use font_matrix to scale glyph space to text space
  // Default is [0.001, 0, 0, 0.001, 0, 0] which scales 1000 units to 1 point
  double fm_a = type3_font->font_matrix.size() >= 1 ? type3_font->font_matrix[0] : 0.001;
  double fm_b = type3_font->font_matrix.size() >= 2 ? type3_font->font_matrix[1] : 0;
  double fm_c = type3_font->font_matrix.size() >= 3 ? type3_font->font_matrix[2] : 0;
  double fm_d = type3_font->font_matrix.size() >= 4 ? type3_font->font_matrix[3] : 0.001;

  // Save current state
  GraphicsState saved_state = state_;

  // Set up graphics state for glyph rendering
  // Apply font matrix and size scaling
  float glyph_scale = size * static_cast<float>(fm_a) * state_.scale;

  // Offset to glyph position
  state_.transform.e = x;
  state_.transform.f = y;

  // Apply font matrix scaling
  state_.transform.a = glyph_scale;
  state_.transform.d = -glyph_scale;  // Flip Y for glyph space

  // Set fill color
  state_.fill_r = r;
  state_.fill_g = g;
  state_.fill_b = b;
  state_.fill_a = a;

  // Push Type 3 font resources
  if (!type3_font->resources.empty()) {
    form_resources_stack_.push_back(type3_font->resources);
  }

  // Parse and render the glyph content stream
  // Type 3 glyphs use d0/d1 operators for metrics, then standard path operators
  bool progress_enabled = progress_.enabled;
  progress_.enabled = false;
  parse_pdf_content(decoded.data);
  progress_.enabled = progress_enabled;

  // Pop resources
  if (!type3_font->resources.empty() && !form_resources_stack_.empty()) {
    form_resources_stack_.pop_back();
  }

  // Restore state
  state_ = saved_state;

  return true;
}

bool LightVGBackend::draw_type1_glyph_by_name(FontCache* font,
                                              const std::string& glyph_name,
                                              float x, float y, float size,
                                              uint8_t r, uint8_t g, uint8_t b,
                                              uint8_t a) {
  if (!scene_ || !font || !font->has_type1 || glyph_name.empty() ||
      glyph_name == ".notdef") {
    return false;
  }

  ttf_outline_t outline{};
  if (t1_glyph_outline(font->type1, glyph_name, &outline) != 0) {
    return false;
  }

  double units_per_em_d = 1000.0;
  if (font->type1.font_matrix[0] > 0.0) {
    units_per_em_d = 1.0 / font->type1.font_matrix[0];
  }
  float scale = size / static_cast<float>(units_per_em_d);

  float font_scale_tm = std::sqrt(state_.text_matrix.a * state_.text_matrix.a +
                                  state_.text_matrix.b * state_.text_matrix.b);
  float cos_theta = 1.0f, sin_theta = 0.0f;
  if (font_scale_tm > 0.01f) {
    cos_theta = state_.text_matrix.a / font_scale_tm;
    sin_theta = state_.text_matrix.b / font_scale_tm;
  }

  auto shape = lvg::Shape::gen();
  if (!shape) {
    ttf_outline_free(&outline);
    return false;
  }

  int render_mode = state_.text_render_mode;
  bool do_fill = (render_mode == 0 || render_mode == 2 ||
                  render_mode == 4 || render_mode == 6);
  bool do_stroke = (render_mode == 1 || render_mode == 2 ||
                    render_mode == 5 || render_mode == 6);
  bool invisible = (render_mode == 3);
  bool add_to_clip = (render_mode >= 4 && render_mode <= 7);

  auto emit_move = [&](float cx, float cy) {
    shape->moveTo(cx, cy);
    if (add_to_clip) {
      state_.text_clip_commands.push_back(lvg::PathCommand::MoveTo);
      state_.text_clip_points.push_back({cx, cy});
    }
  };
  auto emit_line = [&](float cx, float cy) {
    shape->lineTo(cx, cy);
    if (add_to_clip) {
      state_.text_clip_commands.push_back(lvg::PathCommand::LineTo);
      state_.text_clip_points.push_back({cx, cy});
    }
  };
  auto emit_cubic = [&](float c1x, float c1y, float c2x, float c2y,
                        float ex, float ey) {
    shape->cubicTo(c1x, c1y, c2x, c2y, ex, ey);
    if (add_to_clip) {
      state_.text_clip_commands.push_back(lvg::PathCommand::CubicTo);
      state_.text_clip_points.push_back({c1x, c1y});
      state_.text_clip_points.push_back({c2x, c2y});
      state_.text_clip_points.push_back({ex, ey});
    }
  };
  auto emit_close = [&]() {
    shape->close();
    if (add_to_clip) state_.text_clip_commands.push_back(lvg::PathCommand::Close);
  };

  if (add_to_clip) state_.text_clip_active = true;
  decompose_ttf_outline_to_path(outline, scale, x, y, cos_theta, sin_theta,
                                emit_move, emit_line, emit_cubic, emit_close);
  ttf_outline_free(&outline);

  if (render_mode == 7 || invisible) return true;

  if (do_fill) shape->fill(r, g, b, a);
  if (do_stroke) {
    float stroke_width = state_.stroke_width * state_.scale;
    if (state_.stroke_width == 0.0f) stroke_width = 1.0f;
    shape->strokeWidth(stroke_width);
    uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
    shape->strokeFill(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha);
    lvg::StrokeCap cap = lvg::StrokeCap::Butt;
    if (state_.line_cap == 1) cap = lvg::StrokeCap::Round;
    else if (state_.line_cap == 2) cap = lvg::StrokeCap::Square;
    shape->strokeCap(cap);
    lvg::StrokeJoin join = lvg::StrokeJoin::Miter;
    if (state_.line_join == 1) join = lvg::StrokeJoin::Round;
    else if (state_.line_join == 2) join = lvg::StrokeJoin::Bevel;
    shape->strokeJoin(join);
  }

  return push_with_clip(shape);
}

bool LightVGBackend::draw_text(float x, float y, const std::string& text, float size,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }
  if (state_.fill_opacity < 1.0f) {
    float opacity = std::max(0.0f, std::min(1.0f, state_.fill_opacity));
    a = static_cast<uint8_t>(a * opacity + 0.5f);
  }
  const float hscale = state_.horiz_scaling / 100.0f;

  // Compute text direction from text matrix for rotated text positioning
  // For normal text [8,0,0,8], cos_tm=1, sin_tm=0 (advance right)
  // For rotated [0,-8,8,0], cos_tm=0, sin_tm=-1 (advance down in canvas)
  float tm_scale_outer = std::sqrt(state_.text_matrix.a * state_.text_matrix.a +
                                   state_.text_matrix.b * state_.text_matrix.b);
  float cos_tm_outer = 1.0f, sin_tm_outer = 0.0f;
  if (tm_scale_outer > 0.01f) {
    cos_tm_outer = state_.text_matrix.a / tm_scale_outer;
    sin_tm_outer = state_.text_matrix.b / tm_scale_outer;
  }
  // The starting (x, y) has already been transformed through the CTM, but
  // glyph advance below uses cos_tm/sin_tm in canvas space. If the CTM
  // contains a mirror (negative determinant on the diagonal), the advance
  // direction needs to flip so glyphs march in the visually-correct order.
  if (state_.transform.a < 0.0f) cos_tm_outer = -cos_tm_outer;
  if (state_.transform.d < 0.0f) sin_tm_outer = -sin_tm_outer;

  // Check for Type 3 font (user-defined glyphs)
  auto* type3_font = as_type3_font(current_font_);
  if (type3_font) {
    // Type 3 fonts: render each glyph using its CharProc content stream
    float cursor_x = x;
    float cursor_y = y;

    for (size_t i = 0; i < text.length(); ++i) {
      uint8_t char_code = static_cast<uint8_t>(text[i]);

      // Get glyph name from encoding
      std::string glyph_name;
      if (char_code < type3_font->encoding.size()) {
        glyph_name = type3_font->encoding[char_code];
      }

      if (glyph_name.empty() || glyph_name == ".notdef") {
        // No glyph - draw tofu box placeholder and advance
        draw_missing_glyph_placeholder(cursor_x, cursor_y, size, r, g, b, a);
        float adv = size * 0.6f;
        cursor_x += (adv * hscale) * cos_tm_outer;
        cursor_y -= (adv * hscale) * sin_tm_outer;
        continue;
      }

      // Render the Type 3 glyph
      render_type3_glyph(type3_font, glyph_name, cursor_x, cursor_y, size, r, g, b, a);

      // Get advance width from Widths array if available
      float advance = size * 0.6f;  // Default advance
      if (char_code < type3_font->widths.size()) {
        // Type 3 widths are in glyph space, apply font matrix
        double fm_a = type3_font->font_matrix.size() >= 1 ? type3_font->font_matrix[0] : 0.001;
        advance = static_cast<float>(type3_font->widths[char_code] * fm_a * size);
      }

      cursor_x += (advance * hscale) * cos_tm_outer;
      cursor_y -= (advance * hscale) * sin_tm_outer;
    }
    return true;
  }

  // Check if we have a loaded font with glyph data
  FontCache* font = get_font(current_font_name_);

  if (font) {
    if (!font->has_type1 && !font->has_ttf_parse && font->font_info.data == nullptr) {
      font = nullptr;
    }
  }

  if (font) {
    // Render each character using glyph outlines
    float cursor_x = x;
    float scale = font->has_type1
        ? size * static_cast<float>(font->type1.font_matrix[0])
        : stbtt_ScaleForPixelHeight(&font->font_info, size);

    // Use outer text direction variables for cursor advance
    float cos_tm = cos_tm_outer;
    float sin_tm = sin_tm_outer;

    // Use pre-computed flags from Type0Font (set during PDF parse)
    auto* type0_font = as_type0_font(current_font_);
    bool is_two_byte_cid = type0_font ? type0_font->is_two_byte_cid : false;
    bool is_vertical = type0_font ? type0_font->is_vertical : false;

    // For vertical mode, track cursor_y separately
    float cursor_y = y;

    auto draw_unicode_sequence = [&](const std::vector<uint32_t>& sequence,
                                    float px, float py,
                                    float target_advance) -> bool {
      if (sequence.empty()) return false;

      std::vector<float> component_advances;
      component_advances.reserve(sequence.size());

      float natural_total = 0.0f;
      float em_scale = (font->ttf.units_per_em > 0)
          ? (size / static_cast<float>(font->ttf.units_per_em))
          : scale;

      for (uint32_t cp : sequence) {
        if (cp >= 0xFE00u && cp <= 0xFE0Fu) continue;
        float component_adv = size * 0.32f;
        if (font->has_ttf_parse) {
          uint16_t g = ttf_cmap_lookup(&font->ttf, cp);
          component_adv = glyph_advance_units(font, g) * em_scale;
        } else {
          int component_width = 0;
          int component_lsb = 0;
          stbtt_GetCodepointHMetrics(&font->font_info, static_cast<int>(cp),
                                    &component_width, &component_lsb);
          component_adv = component_width * em_scale;
        }
        if (component_adv <= 0.0f) component_adv = size * 0.32f;
        component_advances.push_back(component_adv);
        natural_total += component_adv;
      }

      float fit_scale = (natural_total > 0.001f && target_advance > 0.001f)
          ? std::min(1.0f, target_advance / natural_total)
          : 1.0f;

      bool drew_any = false;
      float sx = px;
      float sy = py;
      size_t advance_index = 0;
      for (uint32_t cp : sequence) {
        if (cp >= 0xFE00u && cp <= 0xFE0Fu) continue;
        drew_any = draw_glyph(static_cast<int>(cp), sx, sy, size,
                             r, g, b, a) || drew_any;
        float step = (advance_index < component_advances.size())
            ? component_advances[advance_index] * fit_scale
            : 0.0f;
        ++advance_index;
        sx += (step * hscale) * cos_tm;
        sy -= (step * hscale) * sin_tm;
      }
      return drew_any;
    };

    for (size_t i = 0; i < text.length(); ) {
      uint32_t char_code;
      size_t bytes_consumed = 1;

      if (type0_font && is_two_byte_cid && i + 1 < text.length()) {
        // Two-byte CID encoding: high byte first, then low byte
        uint8_t high_byte = static_cast<unsigned char>(text[i]);
        uint8_t low_byte = static_cast<unsigned char>(text[i + 1]);
        char_code = (static_cast<uint32_t>(high_byte) << 8) | low_byte;
        bytes_consumed = 2;
      } else {
        // Single-byte encoding
        char_code = static_cast<unsigned char>(text[i]);
      }

      // Type0/CID font dispatch: embedded fonts use glyph indices directly,
      // fallback fonts need Unicode codepoints for their real cmap tables.
      if (type0_font) {
        bool using_embedded = font->is_embedded;
        uint32_t mapped_unicode = 0;
        bool has_mapped_unicode =
            try_map_tounicode(char_code, current_font_, &mapped_unicode);
        const std::vector<uint32_t>* unicode_sequence = nullptr;
        bool has_unicode_sequence =
            try_map_tounicode_sequence(char_code, current_font_,
                                       &unicode_sequence);
        const bool has_rich_sequence =
            has_unicode_sequence && unicode_sequence && unicode_sequence->size() > 1;
        const uint32_t fallback_unicode =
            has_mapped_unicode ? mapped_unicode : map_char_to_unicode(char_code, current_font_);

        // Character spacing (Tc) is common to both single glyph and sequence
        // drawing and must be added to advance calculations for text positions.
        float tc_canvas = 0.0f;
        if (std::abs(state_.font_size) > 0.001f) {
          tc_canvas = state_.char_spacing * size / state_.font_size;
        }

        float sequence_advance = 0.0f;
        if (is_vertical) {
          sequence_advance = type0_font->default_width / 1000.0f * size + tc_canvas;
        } else {
          auto width_it = type0_font->cid_widths.find(char_code);
          if (width_it != type0_font->cid_widths.end()) {
            sequence_advance = width_it->second / 1000.0f * size + tc_canvas;
          } else if (!using_embedded && font->has_ttf_parse) {
            uint32_t lookup = char_code;
            if (!type0_font->to_unicode_cmap.code_to_unicode.empty()) {
              lookup = type0_font->to_unicode_cmap.map_code_to_unicode(char_code);
            }
            uint16_t fgid = ttf_cmap_lookup(&font->ttf, lookup);
            if (fgid == 0 && lookup != char_code) {
              fgid = ttf_cmap_lookup(&font->ttf, char_code);
            }
            uint16_t fadv_units = glyph_advance_units(font, fgid);
            uint16_t upem = font->ttf.units_per_em ? font->ttf.units_per_em : 1000;
            sequence_advance = (static_cast<float>(fadv_units) / upem) * size + tc_canvas;
          } else {
            sequence_advance = type0_font->default_width / 1000.0f * size + tc_canvas;
          }
        }

        // Compute glyph draw position
        // For vertical mode: uses cursor_y with v_x centering
        // For rotated text: cursor_y tracks vertical advance from rotation
        float draw_x = cursor_x;
        float draw_y = cursor_y;
        if (is_vertical) {
          // Apply vertical origin displacement (v_x, v_y)
          // In vertical writing, the text position is the vertical origin.
          // The glyph's horizontal origin (baseline) is displaced by -v from
          // the vertical origin. v_y shifts the baseline below the text position.
          auto vm_it = type0_font->cid_vertical_metrics.find(char_code);
          float v_x_offset, v_y_offset;
          if (vm_it != type0_font->cid_vertical_metrics.end()) {
            v_x_offset = static_cast<float>(vm_it->second.v_x) / 1000.0f * size;
            v_y_offset = static_cast<float>(vm_it->second.v_y) / 1000.0f * size;
          } else {
            // Default v_x = w0/2 (half the horizontal width)
            auto w_it = type0_font->cid_widths.find(char_code);
            int w0 = (w_it != type0_font->cid_widths.end())
                ? w_it->second : type0_font->default_width;
            v_x_offset = w0 / 2000.0f * size;
            v_y_offset = static_cast<float>(type0_font->default_v_y) / 1000.0f * size;
          }
          draw_x = cursor_x - v_x_offset;
          // In canvas coords (y-down), v_y moves baseline below the vertical origin
          draw_y = cursor_y + v_y_offset;
        }

        auto try_draw_gid = [&](uint32_t gid) -> bool {
          if (gid == 0) return false;
          if (font->has_ttf_parse && font->ttf.num_glyphs > 0 &&
              gid >= static_cast<uint32_t>(font->ttf.num_glyphs)) {
            return false;
          }
          if (font->initialized && font->font_info.numGlyphs > 0 &&
              font->font_info.numGlyphs != 0xFFFF &&
              gid >= static_cast<uint32_t>(font->font_info.numGlyphs)) {
            return false;
          }
          return draw_glyph_by_index(static_cast<int>(gid), draw_x, draw_y, size,
                                     r, g, b, a);
        };

        if (using_embedded) {
          bool drawn = false;
          if (!drawn && !type0_font->cid_to_gid_map.empty() &&
              char_code < type0_font->cid_to_gid_map.size()) {
            drawn = try_draw_gid(type0_font->cid_to_gid_map[char_code]);
          }
          if (!drawn && !font->cid_to_gid.empty() &&
              char_code < font->cid_to_gid.size()) {
            drawn = try_draw_gid(font->cid_to_gid[char_code]);
          }
          if (!drawn && is_identity_cmap(type0_font)) {
            drawn = try_draw_gid(char_code);
          }
          if (!drawn) {
            if (has_rich_sequence) {
              drawn = draw_unicode_sequence(*unicode_sequence, draw_x, draw_y,
                                            sequence_advance);
            }
            if (!drawn && has_mapped_unicode) {
              draw_glyph(static_cast<int>(mapped_unicode), draw_x, draw_y, size,
                         r, g, b, a);
              drawn = true;
            }
            if (!drawn) {
              draw_glyph(static_cast<int>(fallback_unicode), draw_x, draw_y, size,
                         r, g, b, a);
            }
          }
        } else {
          // Fallback font: always draw by Unicode semantics.
          bool drawn = false;
          if (has_rich_sequence) {
            drawn = draw_unicode_sequence(*unicode_sequence, draw_x, draw_y,
                                         sequence_advance);
          }
          if (!drawn) {
            draw_glyph(static_cast<int>(fallback_unicode), draw_x, draw_y, size,
                       r, g, b, a);
          }
        }

        if (is_vertical) {
          // Vertical mode: advance cursor_y downward using vertical metrics
          auto vm_it = type0_font->cid_vertical_metrics.find(char_code);
          double w1_y;
          if (vm_it != type0_font->cid_vertical_metrics.end()) {
            w1_y = vm_it->second.w1_y;
          } else {
            w1_y = type0_font->default_w1_y;  // default: -1000
          }
          // w1_y is negative (e.g., -1000); negate for canvas y-down coords
          // PDF spec: ty = w1/1000 * Tfs + Tc (Tc reduces downward displacement)
          // Since we work with magnitude (-w1_y), subtract tc_canvas
          cursor_y += static_cast<float>(-w1_y) / 1000.0f * size - tc_canvas;
        } else {
          // Horizontal mode: advance cursor in text direction using horizontal widths
          float adv = sequence_advance;
          cursor_x += (adv * hscale) * cos_tm;
          cursor_y -= (adv * hscale) * sin_tm;  // canvas y = -(PDF y), so subtract sin

          // Do not apply font kerning here: PDF content streams already encode
          // text positioning via Tj/TJ adjustments and text state tracking.
        }
        i += bytes_consumed;
        continue;
      }

      if (font->has_type1) {
        std::string glyph_name;
        uint32_t mapped_unicode = 0;
        bool has_mapped_unicode = false;
        if (current_font_) {
          if (try_map_tounicode(char_code, current_font_, &mapped_unicode)) {
            has_mapped_unicode = true;
            for (const std::string& mapped_name :
                 type1_glyph_name_candidates_for_unicode(mapped_unicode)) {
              if (!mapped_name.empty() &&
                  font->type1.char_strings.find(mapped_name) != font->type1.char_strings.end()) {
                glyph_name = mapped_name;
                break;
              }
            }
          }
        }
        if (current_font_) {
          auto diff_it = current_font_->encoding_differences.find(char_code);
          if (glyph_name.empty() &&
              diff_it != current_font_->encoding_differences.end()) {
            glyph_name = diff_it->second;
          }
        }
        if (glyph_name.empty() && char_code < font->type1.encoding.size()) {
          glyph_name = font->type1.encoding[char_code];
        }
        if (current_font_) {
          std::string base_lower = current_font_->base_font;
          for (char& c : base_lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          }
          if (base_lower.find("txex") != std::string::npos) {
            if (glyph_name == "parenleftBig") glyph_name = "parenleftbig";
            else if (glyph_name == "parenrightBig") glyph_name = "parenrightbig";
          }
        }
        if ((glyph_name.empty() || glyph_name == ".notdef" ||
             font->type1.char_strings.find(glyph_name) == font->type1.char_strings.end()) &&
            current_font_) {
          if (try_map_tounicode(char_code, current_font_, &mapped_unicode)) {
            has_mapped_unicode = true;
            for (const std::string& mapped_name :
                 type1_glyph_name_candidates_for_unicode(mapped_unicode)) {
              if (!mapped_name.empty() &&
                  font->type1.char_strings.find(mapped_name) != font->type1.char_strings.end()) {
                glyph_name = mapped_name;
                break;
              }
            }
          }
        }

        if (!glyph_name.empty() && glyph_name != ".notdef") {
          if (!draw_type1_glyph_by_name(font, glyph_name, cursor_x, cursor_y,
                                        size, r, g, b, a)) {
            uint32_t codepoint = glyph_name_to_unicode(glyph_name);
            if (codepoint == 0 && has_mapped_unicode) {
              codepoint = normalize_render_codepoint(mapped_unicode);
            }
            if (codepoint != 0 &&
                !try_draw_glyph_fallback(static_cast<int>(codepoint),
                                         cursor_x, cursor_y, size, r, g, b, a)) {
              draw_missing_glyph_placeholder(cursor_x, cursor_y, size, r, g, b, a);
            }
          }
        } else if (has_mapped_unicode) {
          uint32_t codepoint = normalize_render_codepoint(mapped_unicode);
          if (codepoint != 0 && codepoint <= 0x10FFFFu) {
            try_draw_glyph_fallback(static_cast<int>(codepoint), cursor_x, cursor_y,
                                    size, r, g, b, a);
          }
        }

        float adv = -1.0f;
        if (current_font_ && !current_font_->widths.empty()) {
          int first_char = current_font_->first_char;
          int last_char = current_font_->last_char;
          if (static_cast<int>(char_code) >= first_char &&
              static_cast<int>(char_code) <= last_char) {
            size_t idx = static_cast<size_t>(char_code - first_char);
            if (idx < current_font_->widths.size()) {
              adv = current_font_->widths[idx] / 1000.0f * size;
            }
          }
        }
        if (adv < 0.0f && !glyph_name.empty()) {
          auto width_it = font->type1.char_widths.find(glyph_name);
          if (width_it != font->type1.char_widths.end()) {
            adv = width_it->second / 1000.0f * size;
          }
        }
        if (adv < 0.0f) adv = size * 0.6f;

        float tc_canvas2 = 0.0f;
        if (std::abs(state_.font_size) > 0.001f) {
          tc_canvas2 = state_.char_spacing * size / state_.font_size;
        }
        float tw_canvas = 0.0f;
        if (char_code == 32 && std::abs(state_.font_size) > 0.001f) {
          tw_canvas = state_.word_spacing * size / state_.font_size;
        }
        adv += tc_canvas2 + tw_canvas;
        cursor_x += (adv * hscale) * cos_tm;
        cursor_y -= (adv * hscale) * sin_tm;
        i += bytes_consumed;
        continue;
      }

      // Non-Type0 fonts: use encoding tables and ttf_parse metrics.
      // ttf_parse (from lightui) covers both the legacy `kern` table and GPOS
      // PairPos (kern feature), matching what HarfBuzz does for Latin kerning.
      uint32_t codepoint = map_char_to_unicode(char_code, current_font_);
      const std::vector<uint32_t>* unicode_sequence = nullptr;
      bool has_unicode_sequence =
          try_map_tounicode_sequence(char_code, current_font_, &unicode_sequence);

      // Resolve glyph ID via ttf_parse cmap when available; fall back to stbtt.
      uint16_t gid = 0;
      int advance_width = 0;
      if (font->has_ttf_parse) {
        gid = ttf_cmap_lookup(&font->ttf, codepoint);
        advance_width = glyph_advance_units(font, gid);
      } else {
        int lsb;
        stbtt_GetCodepointHMetrics(&font->font_info,
                                   static_cast<int>(codepoint),
                                   &advance_width, &lsb);
      }

      // Add character spacing (Tc) in canvas space
      float tc_canvas2 = 0.0f;
      if (std::abs(state_.font_size) > 0.001f) {
        tc_canvas2 = state_.char_spacing * size / state_.font_size;
      }
      // Add word spacing (Tw) for single-byte space character (code 32)
      float tw_canvas = 0.0f;
      if (char_code == 32 && std::abs(state_.font_size) > 0.001f) {
        tw_canvas = state_.word_spacing * size / state_.font_size;
      }
      // PDF text space uses em units: advance_text = advance_font_units / units_per_em * Tfs.
      // Keep draw_text advance in canvas pixels but derived from the em box so it matches
      // TJ/Tj tracking in calculate_text_width (which is also em-based).
      float adv = -1.0f;
      if (current_font_ && !current_font_->widths.empty()) {
        int first_char = current_font_->first_char;
        int last_char = current_font_->last_char;
        if (static_cast<int>(char_code) >= first_char &&
            static_cast<int>(char_code) <= last_char) {
          size_t idx = static_cast<size_t>(char_code - first_char);
          if (idx < current_font_->widths.size()) {
            adv = current_font_->widths[idx] / 1000.0f * size;
          }
        }
      }
      if (adv < 0.0f) {
        float em_scale = (font->ttf.units_per_em > 0)
            ? (size / static_cast<float>(font->ttf.units_per_em))
            : scale;
        adv = advance_width * em_scale;
      }

      if (has_unicode_sequence && unicode_sequence) {
        std::vector<float> component_advances;
        component_advances.reserve(unicode_sequence->size());
        float natural_total = 0.0f;
        float em_scale = (font->ttf.units_per_em > 0)
            ? (size / static_cast<float>(font->ttf.units_per_em))
            : scale;
        for (uint32_t cp : *unicode_sequence) {
          if (cp >= 0xFE00u && cp <= 0xFE0Fu) continue;
          float component_adv = size * 0.3f;
          if (font->has_ttf_parse) {
            uint16_t component_gid = ttf_cmap_lookup(&font->ttf, cp);
            component_adv = glyph_advance_units(font, component_gid) * em_scale;
          } else {
            int component_width = 0, component_lsb = 0;
            stbtt_GetCodepointHMetrics(&font->font_info, static_cast<int>(cp),
                                       &component_width, &component_lsb);
            component_adv = component_width * em_scale;
          }
          if (component_adv <= 0.0f) component_adv = size * 0.3f;
          component_advances.push_back(component_adv);
          natural_total += component_adv;
        }
        float fit_scale = (natural_total > 0.001f && adv > 0.001f)
            ? std::min(1.0f, adv / natural_total)
            : 1.0f;
        float sequence_x = cursor_x;
        float sequence_y = cursor_y;
        size_t advance_index = 0;
        for (size_t si = 0; si < unicode_sequence->size(); ++si) {
          uint32_t cp = (*unicode_sequence)[si];
          if (cp >= 0xFE00u && cp <= 0xFE0Fu) continue;
          draw_glyph(static_cast<int>(cp), sequence_x,
                     sequence_y, size, r, g, b, a);
          float step = (advance_index < component_advances.size())
              ? component_advances[advance_index] * fit_scale
              : 0.0f;
          advance_index++;
          sequence_x += (step * hscale) * cos_tm;
          sequence_y -= (step * hscale) * sin_tm;
        }
      } else {
        draw_glyph(static_cast<int>(codepoint), cursor_x, cursor_y, size, r, g, b, a);
      }

      // Advance cursor in text direction
      adv += tc_canvas2 + tw_canvas;
      cursor_x += (adv * hscale) * cos_tm;
      cursor_y -= (adv * hscale) * sin_tm;

      // Do not apply font kerning here: PDF text placement is already explicit.

      i += bytes_consumed;
    }
  } else {
    // Fallback: draw per-character tofu box placeholders for text
    float char_width = size * 0.5f;
    for (size_t i = 0; i < text.length(); ++i) {
      draw_missing_glyph_placeholder(x + i * char_width * 1.2f * hscale,
                                     y, size, r, g, b, a);
    }
  }

  return true;
}

bool LightVGBackend::draw_line(float x1, float y1, float x2, float y2, float stroke_width,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = lvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Create line path
  shape->moveTo(x1, y1);
  shape->lineTo(x2, y2);

  // Set stroke (v1.0+ API uses separate methods)
  shape->strokeWidth(stroke_width);
  shape->strokeFill(r, g, b, a);
  shape->strokeCap(lvg::StrokeCap::Round);

  // Add to scene (v1.0+ API takes raw Paint*, scene owns it after push)
  if (scene_->add(shape) != lvg::Result::Success) {
    return false;
  }

  return true;
}

// Helper to load a TTF file from disk
static bool load_font_file(const std::string& path, std::vector<uint8_t>& data) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) return false;

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  data.resize(size);
  if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
    return false;
  }
  return true;
}

static bool looks_like_type1_font_program(const std::vector<uint8_t>& data) {
  if (data.size() >= 2 && data[0] == 0x80 && data[1] == 0x01) {
    return true;  // PFB ASCII segment marker.
  }
  const size_t n = std::min<size_t>(data.size(), 256);
  std::string_view head(reinterpret_cast<const char*>(data.data()), n);
  return head.find("%!PS-AdobeFont-") != std::string_view::npos ||
         head.find("%!FontType1-") != std::string_view::npos ||
         head.find("/FontType 1") != std::string_view::npos;
}

// Try to derive a styled (Bold / Italic / BoldItalic) variant path from a
// "regular" fallback path. The on-disk naming conventions vary by family —
// DejaVu uses -Bold / -Oblique, Liberation uses -Bold / -Italic, FreeFont
// uses BoldItalic with no separator, Noto matches DejaVu, etc. We probe
// several common patterns; the caller load_font_file() returns false for
// any path that doesn't exist on disk.
static std::vector<std::string> derive_styled_paths(const std::string& regular,
                                                    bool want_bold,
                                                    bool want_italic) {
  std::vector<std::string> out;
  if (!want_bold && !want_italic) {
    out.push_back(regular);
    return out;
  }

  size_t slash = regular.find_last_of("/\\");
  size_t name_start = (slash == std::string::npos) ? 0 : slash + 1;
  size_t dot = regular.find_last_of('.');
  if (dot == std::string::npos || dot < name_start) {
    out.push_back(regular);
    return out;
  }
  std::string dir  = regular.substr(0, name_start);
  std::string stem = regular.substr(name_start, dot - name_start);
  std::string ext  = regular.substr(dot);

  // Drop a trailing "-Regular" / "Regular" so substitution doesn't double up.
  for (const char* suf : {"-Regular", "Regular"}) {
    size_t slen = std::strlen(suf);
    if (stem.size() > slen && stem.compare(stem.size() - slen, slen, suf) == 0) {
      stem.erase(stem.size() - slen);
      break;
    }
  }

  // Candidate suffixes, ordered by preference (best match first).
  std::vector<const char*> suffixes;
  if (want_bold && want_italic) {
    suffixes = {"-BoldOblique", "-BoldItalic", "BoldOblique", "BoldItalic"};
  } else if (want_bold) {
    suffixes = {"-Bold", "Bold"};
  } else /* want_italic */ {
    suffixes = {"-Oblique", "-Italic", "Oblique", "Italic"};
  }

  for (const char* suf : suffixes) {
    out.push_back(dir + stem + suf + ext);
  }
  // If bold-italic fails, degrade to bold-only.
  if (want_bold && want_italic) {
    for (const char* suf : {"-Bold", "Bold"}) {
      out.push_back(dir + stem + suf + ext);
    }
  }
  // Always include the regular path as a final fallback.
  out.push_back(regular);
  return out;
}

// Helper to determine font category from name
// Helper to determine font category from name
// Returns: 0=sans-serif, 1=monospace, 2=serif, 3=symbol, 4=CJK
static int get_lightvg_font_category(const std::string& font_name) {
  // Convert to lowercase for comparison
  std::string lower_name;
  for (char c : font_name) {
    lower_name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  // Check for CJK fonts (Japanese, Chinese, Korean)
  if (lower_name.find("japan") != std::string::npos ||
      lower_name.find("mincho") != std::string::npos ||
      lower_name.find("gothic") != std::string::npos ||
      lower_name.find("meiryo") != std::string::npos ||
      lower_name.find("hiragino") != std::string::npos ||
      lower_name.find("msgothic") != std::string::npos ||
      lower_name.find("msmincho") != std::string::npos ||
      lower_name.find("futo") != std::string::npos ||
      lower_name.find("china") != std::string::npos ||
      lower_name.find("korea") != std::string::npos ||
      lower_name.find("simsun") != std::string::npos ||
      lower_name.find("simhei") != std::string::npos ||
      lower_name.find("heiti") != std::string::npos ||
      lower_name.find("songti") != std::string::npos ||
      // Specific Noto CJK font families (not all "noto" fonts are CJK)
      lower_name.find("notosanscjk") != std::string::npos ||
      lower_name.find("notoserifcjk") != std::string::npos ||
      lower_name.find("notosansjp") != std::string::npos ||
      lower_name.find("notoserifjp") != std::string::npos ||
      lower_name.find("notosanssc") != std::string::npos ||
      lower_name.find("notoserifsc") != std::string::npos ||
      lower_name.find("notosanstc") != std::string::npos ||
      lower_name.find("notoseriftc") != std::string::npos ||
      lower_name.find("notosanskr") != std::string::npos ||
      lower_name.find("notoserifkr") != std::string::npos ||
      lower_name.find("notosanshk") != std::string::npos ||
      lower_name.find("notoserifhk") != std::string::npos) {
    return 4;  // CJK
  }

  // Check for monospace/typewriter fonts
  if (lower_name.find("courier") != std::string::npos ||
      lower_name.find("inconsolata") != std::string::npos ||
      lower_name.find("mono") != std::string::npos ||
      lower_name.find("typewriter") != std::string::npos ||
      lower_name.find("consol") != std::string::npos ||
      lower_name.find("fixed") != std::string::npos) {
    return 1;  // Monospace
  }

  // Check for symbol/dingbat fonts
  if (lower_name.find("symbol") != std::string::npos ||
      lower_name.find("math") != std::string::npos ||
      lower_name.find("txmia") != std::string::npos ||
      lower_name.find("txsys") != std::string::npos ||
      lower_name.find("txsym") != std::string::npos ||
      lower_name.find("txex") != std::string::npos ||
      lower_name.find("dingbat") != std::string::npos ||
      lower_name.find("wingding") != std::string::npos ||
      lower_name.find("zapf") != std::string::npos) {
    return 3;  // Symbol
  }

  // Check for serif fonts
  if (lower_name.find("times") != std::string::npos ||
      lower_name.find("linlibertine") != std::string::npos ||
      lower_name.find("libertine") != std::string::npos ||
      lower_name.find("serif") != std::string::npos ||
      lower_name.find("roman") != std::string::npos ||
      lower_name.find("garamond") != std::string::npos ||
      lower_name.find("palatino") != std::string::npos ||
      lower_name.find("georgia") != std::string::npos ||
      lower_name.find("cambria") != std::string::npos) {
    return 2;  // Serif
  }

  if (lower_name.find("linbiolinum") != std::string::npos ||
      lower_name.find("biolinum") != std::string::npos ||
      lower_name.find("helvetica") != std::string::npos ||
      lower_name.find("arial") != std::string::npos ||
      lower_name.find("sans") != std::string::npos) {
    return 0;  // Sans-serif
  }

  // Default to sans-serif (most common)
  return 0;  // Sans-serif
}

// Check if font is CJK based on Type0Font ordering
static bool is_cjk_font(const BaseFont* font) {
  if (!font) return false;
  auto* type0_font = as_type0_font(font);
  if (type0_font) {
    // Check ordering for CJK collections
    const std::string& ordering = type0_font->ordering;
    if (ordering == "Japan1" || ordering == "CNS1" || ordering == "GB1" ||
        ordering == "Korea1" || ordering == "KR") {
      return true;
    }
    // Also check CMap name
    const std::string& cmap_name = type0_font->encoding_cmap.name;
    if (cmap_name.find("Japan") != std::string::npos ||
        cmap_name.find("CNS") != std::string::npos ||
        cmap_name.find("GB") != std::string::npos ||
        cmap_name.find("Korea") != std::string::npos ||
        cmap_name.find("UniJIS") != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Parse weight (400/700) and italic (true/false) from PDF font names.
// e.g. "Helvetica-Bold" → {700, false}
//      "Courier-BoldOblique" → {700, true}
//      "Times-Italic" → {400, true}
//      "Arial" → {400, false}
static std::pair<int, bool> parse_font_weight_style(const std::string& name) {
  std::string lower;
  for (char c : name) {
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  std::string family_name = lower;
  size_t subset_pos = family_name.find('+');
  if (subset_pos != std::string::npos) {
    family_name = family_name.substr(subset_pos + 1);
  }

  int weight = 400;
  bool italic = false;

  if (lower.find("bold") != std::string::npos) weight = 700;
  else if (lower.find("black") != std::string::npos ||
           lower.find("heavy") != std::string::npos) weight = 900;
  else if (lower.find("light") != std::string::npos) weight = 300;
  else if (lower.find("thin") != std::string::npos) weight = 100;
  else if (lower.find("medium") != std::string::npos) weight = 500;
  else if (lower.find("semibold") != std::string::npos ||
           lower.find("demibold") != std::string::npos) weight = 600;

  if (lower.find("italic") != std::string::npos ||
      lower.find("oblique") != std::string::npos) {
    italic = true;
  }
  if (family_name.find("linlibertine") != std::string::npos ||
      family_name.find("linbiolinum") != std::string::npos) {
    if (family_name.size() >= 2 &&
        family_name.compare(family_name.size() - 2, 2, "ti") == 0) {
      italic = true;
    }
    if (family_name.size() >= 2 &&
        family_name.compare(family_name.size() - 2, 2, "tb") == 0) {
      weight = 700;
    }
    if (family_name.size() >= 3 &&
        family_name.compare(family_name.size() - 3, 3, "tbi") == 0) {
      weight = 700;
      italic = true;
    }
  }

  return {weight, italic};
}

bool LightVGBackend::load_fallback_font(const std::string& font_name) {
  return load_fallback_font_with_hint(font_name, nullptr);
}

bool LightVGBackend::load_fallback_font_with_hint(const std::string& font_name, const BaseFont* font) {
  // Check shared fallback cache first to avoid repeated filesystem probes
  std::string cached_path;
  if (SharedFontFallbackCache::instance().find(font_name, cached_path)) {
    if (cached_path.empty()) {
      return false;
    }
    FontCache cache;
    if (load_font_file(cached_path, cache.font_data)) {
      int font_offset = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
      if (font_offset >= 0 &&
          stbtt_InitFont(&cache.font_info, cache.font_data.data(), font_offset)) {
        cache.initialized = true;
        font_cache_[font_name] = std::move(cache);
        return true;
      }
    }
  }

  // Use actual font name (e.g. "Helvetica") for category detection instead of
  // the PDF resource name (e.g. "F0") which carries no useful type information.
  const std::string& hint_name =
      (font && !font->base_font.empty()) ? font->base_font : font_name;

  // Determine font category for better substitution
  int category = get_lightvg_font_category(hint_name);

  // Override category to CJK if font structure indicates CJK
  if (is_cjk_font(font)) {
    category = 4;  // CJK
  }

  // Font paths organized by category
  // Category 0: Sans-serif, 1: Monospace, 2: Serif, 3: Symbol, 4: CJK
  static const char* sans_fonts[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/Library/Fonts/Arial.ttf",
    "C:\\Windows\\Fonts\\arial.ttf",
    nullptr
  };

  static const char* mono_fonts[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/System/Library/Fonts/Courier.ttc",
    "C:\\Windows\\Fonts\\cour.ttf",
    nullptr
  };

  static const char* serif_fonts[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSerif.ttf",
    "/usr/share/fonts/truetype/noto/NotoSerif-Regular.ttf",
    "/System/Library/Fonts/Times.ttc",
    "C:\\Windows\\Fonts\\times.ttf",
    nullptr
  };

  static const char* symbol_fonts[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/System/Library/Fonts/Symbol.ttf",
    nullptr
  };

  // CJK sans (Gothic) fonts
  static const char* cjk_sans_fonts[] = {
    // Bundled Noto Sans JP (relative to project root)
    "fonts/noto-sans-jp/NotoSansJP-Regular.otf",
    "../fonts/noto-sans-jp/NotoSansJP-Regular.otf",
    // System-installed Noto
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansJP-Regular.otf",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansJP-Regular.ttf",
    // Other Japanese Gothic fonts
    "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
    "/usr/share/fonts/truetype/takao-gothic/TakaoGothic.ttf",
    "/usr/share/fonts/opentype/ipafont-gothic/ipagp.ttf",
    "/usr/share/fonts/truetype/vlgothic/VL-Gothic-Regular.ttf",
    "/usr/share/fonts/truetype/sazanami/sazanami-gothic.ttf",
    // Chinese
    "/usr/share/fonts/opentype/noto/NotoSansSC-Regular.otf",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    // Korean
    "/usr/share/fonts/opentype/noto/NotoSansKR-Regular.otf",
    "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
    // macOS
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/Library/Fonts/Osaka.ttf",
    // Windows
    "C:\\Windows\\Fonts\\msgothic.ttc",
    "C:\\Windows\\Fonts\\meiryo.ttc",
    "C:\\Windows\\Fonts\\YuGothR.ttc",
    "C:\\Windows\\Fonts\\simsun.ttc",
    "C:\\Windows\\Fonts\\malgun.ttf",
    nullptr
  };

  // CJK serif (Mincho) fonts
  static const char* cjk_serif_fonts[] = {
    // Bundled Noto Serif JP (relative to project root)
    "fonts/noto-serif-jp/NotoSerifJP-Regular.otf",
    "../fonts/noto-serif-jp/NotoSerifJP-Regular.otf",
    // System-installed Noto Serif
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSerifJP-Regular.otf",
    "/usr/share/fonts/truetype/noto/NotoSerifCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSerifJP-Regular.ttf",
    // Japanese Mincho fonts
    "/usr/share/fonts/opentype/ipafont-mincho/ipamp.ttf",
    "/usr/share/fonts/truetype/sazanami/sazanami-mincho.ttf",
    // Fallback to sans CJK if no serif available
    "fonts/noto-sans-jp/NotoSansJP-Regular.otf",
    "../fonts/noto-sans-jp/NotoSansJP-Regular.otf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    // macOS
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/Library/Fonts/Osaka.ttf",
    // Windows
    "C:\\Windows\\Fonts\\msmincho.ttc",
    "C:\\Windows\\Fonts\\YuMincho.ttc",
    "C:\\Windows\\Fonts\\simsun.ttc",
    nullptr
  };

  static const char** font_lists[] = { sans_fonts, mono_fonts, serif_fonts, symbol_fonts, cjk_sans_fonts };

  // For CJK, select serif (Mincho) or sans (Gothic) list based on font name.
  // Use hint_name (PDF BaseFont like "YuMincho-Regular") rather than the short
  // PDF resource name ("C2_0"), which carries no type info.
  const char** font_list = font_lists[category];
  if (category == 4) {
    std::string lower;
    for (char c : hint_name)
      lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("mincho") != std::string::npos ||
        lower.find("ming") != std::string::npos ||
        lower.find("serif") != std::string::npos ||
        lower.find("songti") != std::string::npos) {
      font_list = cjk_serif_fonts;
    }
  }

  // Check font provider first (for runtime-registered fonts, all categories)
  {
    auto& provider = FontProvider::instance();
    FontCategory pcat;
    if (category == 4) {
      // CJK: select serif or sans based on font_list
      pcat = (font_list == cjk_serif_fonts)
          ? FontCategory::kCJKSerif : FontCategory::kCJKSans;
    } else {
      switch (category) {
        case 0: pcat = FontCategory::kSans; break;
        case 1: pcat = FontCategory::kMono; break;
        case 2: pcat = FontCategory::kSerif; break;
        case 3: pcat = FontCategory::kSymbol; break;
        default: pcat = FontCategory::kSans; break;
      }
    }
    // Use weight/style-aware matching instead of first-match
    auto ws = parse_font_weight_style(hint_name);
    const ProvidedFont* pf = provider.find_best_match(pcat, ws.first, ws.second);
    // For CJK, fall back to CJK sans if specific variant not found
    if (!pf && category == 4) pf = provider.find_best_match(
        FontCategory::kCJKSans, ws.first, ws.second);
    if (pf && !pf->data.empty()) {
      FontCache cache;
      cache.font_data = pf->data;
      int off = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
      if (off >= 0 && stbtt_InitFont(&cache.font_info, cache.font_data.data(), off)) {
        cache.initialized = true;
        font_cache_[font_name] = std::move(cache);
        NANOPDF_LOG_DEBUG("LightVG", "Using FontProvider font: %s for '%s'", pf->name.c_str(), font_name.c_str());
        return true;
      }
    }
  }

#ifdef NANOPDF_EMBED_FONTS
  // Check embedded Standard 14 fonts (non-CJK)
  // Try exact PDF name match first (e.g. "Helvetica" → "Arimo-Regular")
  {
    const auto* entry = embedded_fonts::get_pdf_standard_font(hint_name.c_str());
    // Also try with common name variations (e.g. "Helvetica,Bold" → "Helvetica-Bold")
    if (!entry && font && !font->base_font.empty()) {
      std::string normalized = font->base_font;
      // Some PDFs use comma-separated style (e.g. "Helvetica,Bold")
      for (auto& c : normalized) {
        if (c == ',') c = '-';
      }
      if (normalized != hint_name) {
        entry = embedded_fonts::get_pdf_standard_font(normalized.c_str());
      }
    }
    // If no exact match, pick a default by category
    if (!entry) {
      const char* fallback_name = nullptr;
      switch (category) {
        case 0: fallback_name = "Arimo-Regular"; break;      // sans
        case 1: fallback_name = "Cousine-Regular"; break;    // mono
        case 2: fallback_name = "Tinos-Regular"; break;      // serif
        case 3: fallback_name = "STIXTwoMath-Regular"; break; // symbol
        default: break;
      }
      if (fallback_name) {
        entry = embedded_fonts::find_font(fallback_name);
      }
    }
    if (entry) {
      FontCache cache;
      if (embedded_fonts::decompress_font(entry, cache.font_data)) {
        int off = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
        if (off >= 0 && stbtt_InitFont(&cache.font_info, cache.font_data.data(), off)) {
          if (ttf_font_init(&cache.ttf, cache.font_data.data(), cache.font_data.size()) == 0) {
            cache.has_ttf_parse = true;
          }
          cache.initialized = true;
          font_cache_[font_name] = std::move(cache);
          NANOPDF_LOG_DEBUG("LightVG", "Using embedded font: %s for '%s'",
                           entry->base_name, font_name.c_str());
          return true;
        }
      }
    }
  }
#endif

#ifdef NANOPDF_EMBED_CJK_FONTS
  // Check embedded CJK fonts
  if (category == 4) {
    const char* target = (font_list == cjk_serif_fonts)
        ? "NotoSerifJP-Regular" : "NotoSansJP-Regular";
    auto* entry = embedded_cjk_fonts::find_font(target);
    if (entry) {
      FontCache cache;
      if (embedded_cjk_fonts::decompress_font(entry, cache.font_data)) {
        int off = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
        if (off >= 0 && stbtt_InitFont(&cache.font_info, cache.font_data.data(), off)) {
          cache.initialized = true;
          if (ttf_font_init(&cache.ttf, cache.font_data.data(), cache.font_data.size()) == 0) {
            cache.has_ttf_parse = true;
          }
          font_cache_[font_name] = std::move(cache);
          NANOPDF_LOG_DEBUG("LightVG", "Using embedded CJK font: %s for '%s'", target, font_name.c_str());
          return true;
        }
      }
    }
  }
#endif

  // Weight/italic hint from the PDF font name (e.g. "Helvetica-Bold" → 700,
  // "Times-Italic" → italic). Used below to prefer styled file variants.
  auto fb_ws = parse_font_weight_style(hint_name);
  bool fb_want_bold   = fb_ws.first >= 600;
  bool fb_want_italic = fb_ws.second;

  // Try fonts in the matching category first. For each regular path, probe
  // styled variants (Bold / Italic / BoldItalic) on disk before the regular.
  for (const char** path = font_list; *path; ++path) {
    auto candidates = derive_styled_paths(*path, fb_want_bold, fb_want_italic);
    for (const std::string& cand : candidates) {
      FontCache cache;
      if (load_font_file(cand, cache.font_data)) {
        int font_offset = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
        if (font_offset >= 0 &&
            stbtt_InitFont(&cache.font_info, cache.font_data.data(), font_offset)) {
          cache.initialized = true;
          font_cache_[font_name] = std::move(cache);
          SharedFontFallbackCache::instance().store(font_name, cand);
          NANOPDF_LOG_DEBUG("LightVG", "Using fallback font: %s for '%s' (cat=%d, weight=%d%s)",
                            cand.c_str(), font_name.c_str(), category,
                            fb_ws.first, fb_want_italic ? ",italic" : "");
          return true;
        }
      }
    }
  }

  // If category-specific fonts not found, try sans-serif as ultimate fallback
  if (category != 0) {
    for (const char** path = sans_fonts; *path; ++path) {
      auto candidates = derive_styled_paths(*path, fb_want_bold, fb_want_italic);
      for (const std::string& cand : candidates) {
        FontCache cache;
        if (load_font_file(cand, cache.font_data)) {
          int font_offset = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
          if (font_offset >= 0 &&
              stbtt_InitFont(&cache.font_info, cache.font_data.data(), font_offset)) {
            cache.initialized = true;
            font_cache_[font_name] = std::move(cache);
            SharedFontFallbackCache::instance().store(font_name, cand);
            NANOPDF_LOG_DEBUG("LightVG", "Using fallback font: %s for '%s' (fallback, weight=%d%s)",
                              cand.c_str(), font_name.c_str(),
                              fb_ws.first, fb_want_italic ? ",italic" : "");
            return true;
          }
        }
      }
    }
  }

  SharedFontFallbackCache::instance().store(font_name, "");
  NANOPDF_LOG_WARN("LightVG", "Font '%s' - no fallback font found", font_name.c_str());
  return false;
}

static bool should_use_embedded_type1_for_lightvg(const BaseFont* font);

bool LightVGBackend::load_font(const Pdf& pdf, const std::string& font_name, const BaseFont* font) {
  if (font_cache_.count(font_name)) {
    return font_cache_[font_name].initialized;
  }

  // Shared font cache: avoid re-loading and re-decoding font data when the
  // same Pdf font is loaded by a different backend or a second page pass.
  {
    SharedFontEntry shared;
    if (SharedFontCache::instance().find(&pdf, font_name, shared)) {
      FontCache cache;
      cache.font_data = std::move(shared.font_data);
      cache.is_embedded = shared.is_embedded;
      cache.cid_to_gid = std::move(shared.cid_to_gid);

      int font_offset = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
      bool stbtt_ok = font_offset >= 0 &&
                      stbtt_InitFont(&cache.font_info, cache.font_data.data(), font_offset);

      if (shared.has_ttf_parse &&
          ttf_font_init(&cache.ttf, cache.font_data.data(), cache.font_data.size()) == 0) {
        cache.has_ttf_parse = true;
      }

      if (!stbtt_ok && !cache.has_ttf_parse) {
        return load_fallback_font_with_hint(font_name, font);
      }

      cache.initialized = true;
      font_cache_[font_name] = std::move(cache);
      return true;
    }
  }

  // Check if font has embedded data.
  // For Type0 fonts, the FontDescriptor is in the descendant CIDFont,
  // not on the Type0 font itself.
  const FontDescriptor* desc = font ? font->descriptor : nullptr;
  if (!desc) {
    auto* type0 = as_type0_font(font);
    if (type0 && type0->descendant_font && type0->descendant_font->descriptor) {
      desc = type0->descendant_font->descriptor;
    }
  }

  if (!font || !desc) {
    return load_fallback_font_with_hint(font_name, font);
  }
  if (desc->font_file.type == Value::UNDEFINED ||
      desc->font_file.type == Value::NULL_OBJ) {
    return load_fallback_font_with_hint(font_name, font);
  }

  // Resolve and decode font file stream. The obj/gen numbers must be passed through
  // to decode_stream so encrypted font streams can be decrypted (the per-object key
  // is derived from obj/gen).
  Value font_file_val = desc->font_file;
  uint32_t font_obj_num = desc->font_file.ref_object_number;
  uint16_t font_gen_num = desc->font_file.ref_generation_number;
  if (font_file_val.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, font_file_val.ref_object_number,
                                      font_file_val.ref_generation_number);
    if (!resolved.success) {
      return load_fallback_font_with_hint(font_name, font);
    }
    font_file_val = resolved.value;
  }

  if (font_file_val.type != Value::STREAM) {
    return load_fallback_font_with_hint(font_name, font);
  }

  auto decoded = decode_stream(pdf, font_file_val, font_obj_num, font_gen_num);
  if (!decoded.success || decoded.data.empty()) {
    return load_fallback_font_with_hint(font_name, font);
  }

  if ((desc->font_file_type == FontFileType::FontFile ||
       looks_like_type1_font_program(decoded.data)) &&
      should_use_embedded_type1_for_lightvg(font)) {
    Type1Parser parser;
    FontCache cache;
    if (!parser.Parse(decoded.data.data(), decoded.data.size(), cache.type1, true)) {
      NANOPDF_LOG_WARN("LightVG",
                       "Embedded Type1 font '%s' parse failed: %s; using fallback",
                       font_name.c_str(), parser.GetError().c_str());
      return load_fallback_font_with_hint(font_name, font);
    }
    if (cache.type1.char_strings.empty()) {
      NANOPDF_LOG_WARN("LightVG",
                       "Embedded Type1 font '%s' has no parsed CharStrings; using fallback",
                       font_name.c_str());
      return load_fallback_font_with_hint(font_name, font);
    }
    cache.font_data = std::move(decoded.data);
    cache.initialized = true;
    cache.is_embedded = true;
    cache.has_type1 = true;
    NANOPDF_LOG_INFO("LightVG", "Loaded embedded Type1 font '%s' (%zu glyphs)",
                     font_name.c_str(), cache.type1.char_strings.size());
    font_cache_[font_name] = std::move(cache);
    return true;
  }

  // Check for raw CFF data (stb_truetype can't parse raw CFF directly,
  // but DOES support OpenType-CFF with OTTO header natively)
  std::vector<uint16_t> cff_cid_to_gid;
  if (cff_wrapper::is_raw_cff(decoded.data.data(), decoded.data.size())) {
    std::vector<uint8_t> wrapped;
    cff::CFFData cff_data;
    cff::CFFParser cff_parser;
    bool cff_ok =
        cff_parser.parse(decoded.data.data(), decoded.data.size(), cff_data);
    if (cff_ok && !cff_data.is_cid && !cff_data.charset.empty()) {
      // Simple (non-CID) CFF: synthesize a fully loadable sfnt with a cmap
      // built from the charset glyph names, so the embedded glyphs render
      // directly instead of falling back to a substitute font.
      std::vector<std::pair<uint32_t, uint16_t>> uni_to_gid;
      uni_to_gid.reserve(cff_data.charset.size());
      for (size_t gid = 0; gid < cff_data.charset.size(); ++gid) {
        uint32_t u = glyph_name_to_unicode(cff_data.charset[gid]);
        if (u) uni_to_gid.emplace_back(u, static_cast<uint16_t>(gid));
      }
      // The CFF coordinate space is 1/FontMatrix[0] units/em (1000 by default,
      // but e.g. Courier-New subsets are 2048). ttf_parse/stbtt scale glyphs by
      // head.unitsPerEm and ignore the CFF FontMatrix, so derive upem from it.
      uint16_t upem = 1000;
      if (cff_data.font_matrix.size() >= 1 && cff_data.font_matrix[0] > 0.0) {
        double u = 1.0 / cff_data.font_matrix[0];
        if (u >= 16.0 && u <= 65535.0) upem = static_cast<uint16_t>(u + 0.5);
      }
      wrapped = cff_wrapper::build_simple_cff_opentype(
          decoded.data, std::move(uni_to_gid),
          static_cast<uint16_t>(cff_data.num_glyphs), upem);
    }
    if (wrapped.empty()) {
      // CID-keyed, parse failure, or no mappable names: minimal OTTO wrapper
      // plus a CID->GID map (legacy behavior).
      cff_cid_to_gid = cff_wrapper::build_cid_to_gid_map(
          decoded.data.data(), decoded.data.size());
      if (cff_ok && cff_data.is_cid && cff_data.num_glyphs > 0) {
        // CID CFF subsets usually have no cmap, but ttf_parse still needs the
        // sfnt metric tables before it can draw glyphs by CID-derived GID.
        uint16_t upem = 1000;
        if (cff_data.font_matrix.size() >= 1 &&
            cff_data.font_matrix[0] > 0.0) {
          double u = 1.0 / cff_data.font_matrix[0];
          if (u >= 16.0 && u <= 65535.0) {
            upem = static_cast<uint16_t>(u + 0.5);
          }
        }
        wrapped = cff_wrapper::build_simple_cff_opentype(
            decoded.data,
            std::vector<std::pair<uint32_t, uint16_t>>{{0, 0}},
            static_cast<uint16_t>(cff_data.num_glyphs), upem);
      }
      if (wrapped.empty()) {
        wrapped = cff_wrapper::wrap_cff_in_opentype(decoded.data);
      }
    }
    if (wrapped.empty()) {
      return load_fallback_font_with_hint(font_name, font);
    }
    decoded.data = std::move(wrapped);
    NANOPDF_LOG_INFO("LightVG", "Wrapped raw CFF font '%s' in OpenType container"
                     " (CID->GID map: %s)",
                     font_name.c_str(),
                     cff_cid_to_gid.empty() ? "identity" : "custom");
  }

  // Initialize stb_truetype with the font data
  FontCache cache;
  cache.font_data = std::move(decoded.data);

  // stb_truetype's InitFont requires a 'cmap' table. CID-keyed subset fonts
  // often omit it because the PDF's CIDToGIDMap drives glyph selection; in
  // that case we proceed with ttf_parse as the sole outline backend. Only
  // bail to a system fallback if ttf_parse also can't read the file.
  int font_offset = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
  bool stbtt_ok = font_offset >= 0 &&
                  stbtt_InitFont(&cache.font_info, cache.font_data.data(), font_offset);

  // Initialize ttf_parse for kerning lookup (GPOS and kern table support).
  // For CID subsets that fail stbtt_InitFont (no cmap table), ttf_parse is
  // the only outline reader we can use.
  if (ttf_font_init(&cache.ttf, cache.font_data.data(), cache.font_data.size()) == 0) {
    cache.has_ttf_parse = true;
    NANOPDF_LOG_DEBUG("LightVG", "ttf_parse: kerning available for '%s' (kern=%d, GPOS_count=%d)",
                      font_name.c_str(),
                      cache.ttf.tab_kern.len > 0 ? 1 : 0,
                      cache.ttf.gpos_kern_count);
  }

  if (!stbtt_ok && !cache.has_ttf_parse) {
    return load_fallback_font_with_hint(font_name, font);
  }

  cache.initialized = true;
  cache.is_embedded = true;
  cache.cid_to_gid = std::move(cff_cid_to_gid);
  NANOPDF_LOG_INFO("LightVG", "Loaded embedded font '%s' (%zu bytes)", font_name.c_str(), cache.font_data.size());

  // Share with other backends / page passes.
  SharedFontCache::instance().store(&pdf, font_name, {
    cache.font_data,  // copy
    true,             // is_embedded
    cache.has_ttf_parse,
    cache.cid_to_gid  // copy
  });

  font_cache_[font_name] = std::move(cache);
  return true;
}

LightVGBackend::FontCache* LightVGBackend::get_font(const std::string& font_name) {
  // Hot path: the same font is resolved many times per glyph. Short-circuit
  // on the last successful lookup to skip the std::map string-compare walk.
  if (memo_font_ptr_ && font_name == memo_font_name_) {
    return memo_font_ptr_;
  }
  auto it = font_cache_.find(font_name);
  if (it != font_cache_.end() && it->second.initialized) {
    // Assign a stable integer id on first resolution; used as the glyph-cache
    // key so per-glyph probes never hash the font name string.
    if (it->second.font_id == 0) {
      it->second.font_id = next_font_id_++;
    }
    memo_font_name_ = font_name;
    memo_font_ptr_ = &it->second;
    return &it->second;
  }
  return nullptr;
}

std::string LightVGBackend::font_cache_key(const BaseFont* font) const {
  if (!font) return std::string();
  // Type0 fonts carry the embedded program in the descendant CIDFont's
  // descriptor, not on the Type0 font itself.
  const FontDescriptor* desc = font->descriptor;
  if (!desc) {
    const Type0Font* t0 = as_type0_font(font);
    if (t0 && t0->descendant_font && t0->descendant_font->descriptor) {
      desc = t0->descendant_font->descriptor;
    }
  }
  if (desc && desc->font_file.type != Value::UNDEFINED &&
      desc->font_file.type != Value::NULL_OBJ) {
    // Embedded font: the font-file stream's object id is a per-document stable,
    // collision-free identity. (Indirect refs carry the object number; for the
    // rare direct/inline stream, fall through to the program length + name.)
    if (desc->font_file.ref_object_number != 0) {
      return "E" + std::to_string(desc->font_file.ref_object_number) + "_" +
             std::to_string(desc->font_file.ref_generation_number);
    }
    return "EL" + std::to_string(desc->font_file_length) + ":" + font->base_font;
  }
  // Non-embedded font: substitution is driven by the PostScript base name, so
  // the same base name maps to the same substitute — a safe shared identity.
  if (!font->base_font.empty()) return "F" + font->base_font;
  return std::string();
}

static bool should_use_embedded_type1_for_lightvg(const BaseFont* font) {
  if (!font) return false;
  std::string name = font->base_font;
  size_t subset = name.find('+');
  if (subset != std::string::npos) name = name.substr(subset + 1);
  std::string lower;
  for (char c : name) {
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower.find("math") != std::string::npos ||
         lower.find("txmia") != std::string::npos ||
         lower.find("txsys") != std::string::npos ||
         lower.find("txsym") != std::string::npos ||
         lower.find("txex") != std::string::npos;
}

uint16_t LightVGBackend::glyph_advance_units(FontCache* font, uint16_t gid) {
  if (!font || !font->has_ttf_parse) {
    return 0;
  }
  int n = font->ttf.num_glyphs;
  // Only memoize when the glyph id is within the known glyph count; otherwise
  // fall back to a direct lookup (defensive against malformed gids).
  if (n <= 0 || gid >= static_cast<uint16_t>(n)) {
    return ttf_hmtx_advance(&font->ttf, gid);
  }
  if (font->advance_cache.empty()) {
    font->advance_cache.assign(static_cast<size_t>(n), -1);
  }
  int16_t cached = font->advance_cache[gid];
  if (cached >= 0) {
    return static_cast<uint16_t>(cached);
  }
  uint16_t adv = ttf_hmtx_advance(&font->ttf, gid);
  // Advances comfortably fit in int16_t for any real font (units_per_em is
  // typically 1000/2048); clamp the sentinel range just in case.
  font->advance_cache[gid] =
      static_cast<int16_t>(adv > 0x7FFF ? 0x7FFF : adv);
  return adv;
}

float LightVGBackend::calculate_text_width(const std::string& text, float font_size) {
  // First try to use PDF font width information if available
  auto* type0_font = as_type0_font(current_font_);
  if (type0_font) {
    bool is_two_byte_cid = type0_font->is_two_byte_cid;

    float width = 0.0f;
    int num_chars = 0;
    for (size_t i = 0; i < text.length(); ) {
      uint32_t char_code;
      size_t bytes_consumed = 1;

      if (is_two_byte_cid && i + 1 < text.length()) {
        // Two-byte CID encoding
        uint8_t high_byte = static_cast<unsigned char>(text[i]);
        uint8_t low_byte = static_cast<unsigned char>(text[i + 1]);
        char_code = (static_cast<uint32_t>(high_byte) << 8) | low_byte;
        bytes_consumed = 2;
      } else {
        char_code = static_cast<unsigned char>(text[i]);
      }

      // Look up width in PDF font's width table
      auto width_it = type0_font->cid_widths.find(char_code);
      if (width_it != type0_font->cid_widths.end()) {
        // PDF widths are in 1/1000 of text space unit
        width += width_it->second / 1000.0f * font_size;
      } else {
        // Fallback: if using a substitute (non-embedded) font with ttf_parse,
        // use the substitute's actual hmtx advance instead of the 1-em DW.
        FontCache* fc = get_font(current_font_name_);
        bool used_ttf = false;
        if (fc && !fc->is_embedded && fc->has_ttf_parse) {
          uint32_t lookup = char_code;
          if (!type0_font->to_unicode_cmap.code_to_unicode.empty()) {
            lookup = type0_font->to_unicode_cmap.map_code_to_unicode(char_code);
          }
          uint16_t fgid = ttf_cmap_lookup(&fc->ttf, lookup);
          if (fgid == 0 && lookup != char_code) {
            fgid = ttf_cmap_lookup(&fc->ttf, char_code);
          }
          if (fgid != 0) {
            uint16_t fadv_units = glyph_advance_units(fc, fgid);
            uint16_t upem = fc->ttf.units_per_em ? fc->ttf.units_per_em : 1000;
            width += (static_cast<float>(fadv_units) / upem) * font_size;
            used_ttf = true;
          }
        }
        if (!used_ttf) {
          width += type0_font->default_width / 1000.0f * font_size;
        }
      }
      num_chars++;
      i += bytes_consumed;
    }
    // Add character spacing (Tc) for each character
    width += num_chars * state_.char_spacing;
    return width;
  }

  if (current_font_ && !current_font_->widths.empty()) {
    float width = 0.0f;
    int num_chars = 0;
    for (size_t i = 0; i < text.length(); i++) {
      uint32_t char_code = static_cast<unsigned char>(text[i]);
      int first_char = current_font_->first_char;
      int last_char = current_font_->last_char;

      if (static_cast<int>(char_code) >= first_char &&
          static_cast<int>(char_code) <= last_char) {
        size_t idx = char_code - first_char;
        if (idx < current_font_->widths.size()) {
          width += current_font_->widths[idx] / 1000.0f * font_size;
          if (char_code == 32) width += state_.word_spacing;
          num_chars++;
          continue;
        }
      }
      width += font_size * 0.5f;
      if (char_code == 32) width += state_.word_spacing;
      num_chars++;
    }
    width += num_chars * state_.char_spacing;
    return width;
  }

  // Fallback to stb_truetype metrics
  FontCache* font = get_font(current_font_name_);
  if (!font) {
    // Last resort: approximate width
    return text.length() * font_size * 0.5f;
  }

  float scale = stbtt_ScaleForPixelHeight(&font->font_info, font_size);
  float width = 0.0f;

  for (size_t i = 0; i < text.length(); i++) {
    uint32_t char_code = static_cast<unsigned char>(text[i]);

    // For Type0/CID fonts with CIDToGIDMap, use glyph index directly.
    if (type0_font && !type0_font->cid_to_gid_map.empty()) {
      if (char_code < type0_font->cid_to_gid_map.size()) {
        int gid = type0_font->cid_to_gid_map[char_code];
        int advance_width = 0;
        if (font->has_ttf_parse) {
          advance_width = glyph_advance_units(font, static_cast<uint16_t>(gid));
        } else {
          int lsb;
          stbtt_GetGlyphHMetrics(&font->font_info, gid, &advance_width, &lsb);
        }
        width += advance_width * scale;
        continue;
      }
    }

    // Map to Unicode and get metrics. Prefer ttf_parse (cmap + hmtx) over
    // stbtt; the two agree on advance widths but ttf_parse keeps the code
    // path consistent with draw_text. Kerning is intentionally NOT applied
    // here because Tm advance in PDF tracks unkerned widths — kerning is a
    // visual offset applied per-glyph inside draw_text only.
    uint32_t codepoint = map_char_to_unicode(char_code, current_font_);
    int advance_width = 0;
    if (font->has_ttf_parse) {
      uint16_t g = ttf_cmap_lookup(&font->ttf, codepoint);
      advance_width = glyph_advance_units(font, g);
    } else {
      int lsb;
      stbtt_GetCodepointHMetrics(&font->font_info,
                                 static_cast<int>(codepoint),
                                 &advance_width, &lsb);
    }
    width += advance_width * scale;

    // Add word spacing (Tw) for single-byte space (code 32)
    if (char_code == 32) width += state_.word_spacing * scale;
  }

  if (scale > 0) {
    float em_scale = (font->ttf.units_per_em > 0)
        ? (font_size / static_cast<float>(font->ttf.units_per_em))
        : scale;
    width = (width / scale) * em_scale;
  }
  width += static_cast<float>(text.length()) * state_.char_spacing;
  return width;
}

float LightVGBackend::calculate_text_advance_draw_model(const std::string& text,
                                                       float font_size) {
  // Mirror draw_text's per-glyph advance model so text rendering and text-matrix
  // advancement stay in lockstep for Tj/TJ.
  FontCache* font = get_font(current_font_name_);
  if (!font) {
    return calculate_text_width(text, font_size);
  }

  auto* type0_font = as_type0_font(current_font_);
  if (type0_font) {
    if (type0_font->is_vertical) {
      return calculate_vertical_advance(text, font_size);
    }

    float advance = 0.0f;
    for (size_t i = 0; i < text.length();) {
      uint32_t char_code;
      size_t bytes_consumed = 1;
      if (type0_font->is_two_byte_cid && i + 1 < text.length()) {
        uint8_t high_byte = static_cast<unsigned char>(text[i]);
        uint8_t low_byte = static_cast<unsigned char>(text[i + 1]);
        char_code = (static_cast<uint32_t>(high_byte) << 8) | low_byte;
        bytes_consumed = 2;
      } else {
        char_code = static_cast<unsigned char>(text[i]);
      }

      float adv = 0.0f;
      auto width_it = type0_font->cid_widths.find(char_code);
      if (width_it != type0_font->cid_widths.end()) {
        adv = width_it->second / 1000.0f * font_size;
      } else if (!font->is_embedded && font->has_ttf_parse) {
        uint32_t lookup = char_code;
        if (!type0_font->to_unicode_cmap.code_to_unicode.empty()) {
          lookup = type0_font->to_unicode_cmap.map_code_to_unicode(char_code);
        }
        uint16_t fgid = ttf_cmap_lookup(&font->ttf, lookup);
        if (fgid == 0 && lookup != char_code) {
          fgid = ttf_cmap_lookup(&font->ttf, char_code);
        }
        uint16_t fadv_units = glyph_advance_units(font, fgid);
        uint16_t upem = font->ttf.units_per_em ? font->ttf.units_per_em : 1000;
        adv = (static_cast<float>(fadv_units) / upem) * font_size;
      } else {
        adv = type0_font->default_width / 1000.0f * font_size;
      }
      adv += state_.char_spacing;
      advance += adv;
      i += bytes_consumed;
    }
    return advance;
  }

  float advance = 0.0f;
  for (size_t i = 0; i < text.length(); i++) {
    uint32_t char_code = static_cast<unsigned char>(text[i]);
    float adv = -1.0f;
    if (current_font_ && !current_font_->widths.empty()) {
      int first_char = current_font_->first_char;
      int last_char = current_font_->last_char;
      if (static_cast<int>(char_code) >= first_char &&
          static_cast<int>(char_code) <= last_char) {
        size_t idx = static_cast<size_t>(char_code - first_char);
        if (idx < current_font_->widths.size()) {
          adv = current_font_->widths[idx] / 1000.0f * font_size;
        }
      }
    }
    if (adv < 0.0f) {
      uint32_t codepoint = map_char_to_unicode(char_code, current_font_);
      int adv_units = 0;
      if (font->has_ttf_parse) {
        uint16_t gid = ttf_cmap_lookup(&font->ttf, codepoint);
        adv_units = glyph_advance_units(font, gid);
      } else {
        int lsb = 0;
        stbtt_GetCodepointHMetrics(&font->font_info, static_cast<int>(codepoint),
                                   &adv_units, &lsb);
      }
      float em_scale = (font->ttf.units_per_em > 0)
          ? (font_size / static_cast<float>(font->ttf.units_per_em))
          : stbtt_ScaleForPixelHeight(&font->font_info, font_size);
      adv = adv_units * em_scale;
    }
    adv += state_.char_spacing;
    if (char_code == 32) {
      adv += state_.word_spacing;
    }
    advance += adv;
  }
  return advance;
}

float LightVGBackend::calculate_vertical_advance(const std::string& text, float font_size) {
  auto* type0_font = as_type0_font(current_font_);
  if (!type0_font) return text.length() * font_size;  // fallback

  // Use pre-computed flag from Type0Font
  float advance = 0.0f;
  int num_chars = 0;
  for (size_t i = 0; i < text.length(); ) {
    uint32_t char_code;
    size_t bytes_consumed = 1;

    if (type0_font->is_two_byte_cid && i + 1 < text.length()) {
      uint8_t high_byte = static_cast<unsigned char>(text[i]);
      uint8_t low_byte = static_cast<unsigned char>(text[i + 1]);
      char_code = (static_cast<uint32_t>(high_byte) << 8) | low_byte;
      bytes_consumed = 2;
    } else {
      char_code = static_cast<unsigned char>(text[i]);
    }

    // Look up vertical advance width from W2/DW2
    auto vm_it = type0_font->cid_vertical_metrics.find(char_code);
    double w1_y = (vm_it != type0_font->cid_vertical_metrics.end())
        ? vm_it->second.w1_y : type0_font->default_w1_y;
    // w1_y is negative; return positive total advance magnitude
    advance += static_cast<float>(-w1_y) / 1000.0f * font_size;
    num_chars++;
    i += bytes_consumed;
  }
  // Subtract character spacing (Tc): PDF spec ty = w1/1000*Tfs + Tc.
  // Since w1 is negative and we return positive magnitude, Tc reduces it.
  advance -= num_chars * state_.char_spacing;
  if (advance < 0.0f) advance = 0.0f;
  return advance;
}

// Attach the current soft mask to a paint for per-pixel modulation.
// Cached shared_ptr so multiple paints with the same mask share one copy.
void LightVGBackend::apply_soft_mask_opacity(lvg::Paint* paint) {
  if (!paint || !state_.has_soft_mask || state_.soft_mask_data.empty()) return;
  if (state_.soft_mask_width == 0 || state_.soft_mask_height == 0) return;

  // Detect cache miss by size + dimensions + a few sentinel bytes. We
  // intentionally don't memcmp the whole mask each paint.
  bool rebuild = !current_soft_mask_ ||
                 current_soft_mask_->w != state_.soft_mask_width ||
                 current_soft_mask_->h != state_.soft_mask_height ||
                 current_soft_mask_->data.size() !=
                     state_.soft_mask_data.size();
  if (!rebuild && !state_.soft_mask_data.empty()) {
    size_t n = state_.soft_mask_data.size();
    if (current_soft_mask_->data.front() != state_.soft_mask_data.front() ||
        current_soft_mask_->data[n / 2] != state_.soft_mask_data[n / 2] ||
        current_soft_mask_->data.back() != state_.soft_mask_data.back()) {
      rebuild = true;
    }
  }
  if (rebuild) {
    auto sm = std::make_shared<lvg::SoftMaskData>();
    sm->w = state_.soft_mask_width;
    sm->h = state_.soft_mask_height;
    sm->data = state_.soft_mask_data;
    current_soft_mask_ = sm;
  }
  paint->softMask(current_soft_mask_);
}

bool LightVGBackend::push_with_clip(lvg::Shape* shape) {
  if (!scene_ || !shape) {
    return false;
  }

  // Apply blend mode
  if (state_.blend_mode != 0) {
    shape->blend(static_cast<lvg::BlendMethod>(state_.blend_mode));
  }

  // Apply soft mask opacity
  apply_soft_mask_opacity(shape);

  // If there's a clipping path, apply it
  if (state_.has_clip && !state_.clip_commands.empty()) {
    // Create a clipper shape from the clipping path
    auto clipper = lvg::Shape::gen();

    // The clip_points are already in canvas coordinates (Y-flip and scale were
    // applied when the path was constructed in parse_pdf_content)
    append_shape_geometry(clipper, state_.clip_commands, state_.clip_points);

    // Set fill rule for clipping
    if (state_.clip_even_odd) {
      clipper->fillRule(lvg::FillRule::EvenOdd);
    } else {
      clipper->fillRule(lvg::FillRule::NonZero);
    }

    // Apply clip to the shape (clip() takes ownership of clipper)
    if (shape->clip(clipper) != lvg::Result::Success) {
      // Clipping failed, release clipper and push shape without clip
      clipper->unref();
      scene_->add(shape);
      return true;
    }
  }

  scene_->add(shape);
  return true;
}

// Forward declaration for PDF function evaluator
static bool evaluate_pdf_function(const Pdf& pdf, const Value& function,
                                   const std::vector<double>& inputs,
                                   std::vector<double>& outputs);

struct BilinearSample {
  int i0{0};
  int i1{0};
  float w0{1.0f};
  float w1{0.0f};
};

static std::vector<BilinearSample> make_bilinear_samples(int src, int dst) {
  std::vector<BilinearSample> samples;
  if (src <= 0 || dst <= 0) return samples;
  samples.resize(static_cast<size_t>(dst));
  for (int i = 0; i < dst; ++i) {
    float src_i = (static_cast<float>(i) + 0.5f) * src / dst - 0.5f;
    int i0 = static_cast<int>(std::floor(src_i));
    float f = src_i - i0;
    if (i0 < 0) {
      i0 = 0;
      f = 0.0f;
    }
    samples[static_cast<size_t>(i)] = {i0, std::min(i0 + 1, src - 1),
                                       1.0f - f, f};
  }
  return samples;
}

static uint32_t clamp_to_byte(float v) {
  return static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, v + 0.5f)));
}

static void downsample_argb_bilinear(std::vector<uint32_t>& pixels,
                                     int src_w, int src_h,
                                     int dst_w, int dst_h) {
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
      (src_w == dst_w && src_h == dst_h)) {
    return;
  }

  std::vector<uint32_t> out(static_cast<size_t>(dst_w) *
                            static_cast<size_t>(dst_h));
  auto x_samples = make_bilinear_samples(src_w, dst_w);
  auto y_samples = make_bilinear_samples(src_h, dst_h);
  for (int y = 0; y < dst_h; ++y) {
    const auto& sy = y_samples[static_cast<size_t>(y)];
    const uint32_t* row0 =
        pixels.data() + static_cast<size_t>(sy.i0) * src_w;
    const uint32_t* row1 =
        pixels.data() + static_cast<size_t>(sy.i1) * src_w;
    uint32_t* dst_row = out.data() + static_cast<size_t>(y) * dst_w;
    for (int x = 0; x < dst_w; ++x) {
      const auto& sx = x_samples[static_cast<size_t>(x)];
      uint32_t p00 = row0[sx.i0];
      uint32_t p10 = row0[sx.i1];
      uint32_t p01 = row1[sx.i0];
      uint32_t p11 = row1[sx.i1];
      float w00 = sx.w0 * sy.w0;
      float w10 = sx.w1 * sy.w0;
      float w01 = sx.w0 * sy.w1;
      float w11 = sx.w1 * sy.w1;
      auto sample = [&](int shift) -> uint32_t {
        float v = static_cast<float>((p00 >> shift) & 0xffu) * w00 +
                  static_cast<float>((p10 >> shift) & 0xffu) * w10 +
                  static_cast<float>((p01 >> shift) & 0xffu) * w01 +
                  static_cast<float>((p11 >> shift) & 0xffu) * w11;
        return clamp_to_byte(v);
      };
      uint32_t a = sample(24);
      uint32_t r = sample(16);
      uint32_t g = sample(8);
      uint32_t b = sample(0);
      dst_row[x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  pixels = std::move(out);
}

bool LightVGBackend::draw_image(const ImageXObject& image, float x, float y, float width, float height,
                               uint8_t fill_r, uint8_t fill_g, uint8_t fill_b,
                               const GraphicsState::Matrix* image_ctm,
                               bool retain_converted_argb) {
  NANOPDF_LOG_DEBUG("LightVG", "draw_image: %dx%d at (%.1f,%.1f) size %.1fx%.1f, data=%zu bytes",
                    image.width, image.height, x, y, width, height, image.data.size());

  if (!scene_ || image.data.empty()) {
    NANOPDF_LOG_WARN("LightVG", "draw_image: skipped (scene=%p, data.empty=%d)",
                     (void*)scene_, image.data.empty() ? 1 : 0);
    return false;
  }
  last_image_argb_.clear();

  // Convert image data to ARGB8888 format for LightVG
  std::vector<uint32_t> argb_data;
  int img_width = image.width;
  int img_height = image.height;
  bool pixels_are_premultiplied = false;
  bool from_argb_cache = false;

  if (img_width <= 0 || img_height <= 0) {
    return false;
  }

  float draw_w_px = std::abs(width) * state_.scale;
  float draw_h_px = std::abs(height) * state_.scale;
  if (image_ctm) {
    draw_w_px = std::hypot(image_ctm->a, image_ctm->b) * state_.scale;
    draw_h_px = std::hypot(image_ctm->c, image_ctm->d) * state_.scale;
  }
  int prescale_w = static_cast<int>(std::ceil(draw_w_px * 2.0f));
  int prescale_h = static_cast<int>(std::ceil(draw_h_px * 2.0f));
  prescale_w = std::max(1, std::min(prescale_w, img_width));
  prescale_h = std::max(1, std::min(prescale_h, img_height));
  bool should_prescale =
      !image.image_mask &&
      prescale_w * 2 <= img_width &&
      prescale_h * 2 <= img_height;

  // Fast path: cached ARGB from render cache.
  if (image.color_space.type == ColorSpaceType::CacheARGB) {
    // data layout: argb_data as uint32_t packed into uint8_t vector
    argb_data.resize(static_cast<size_t>(img_width) * img_height);
    memcpy(argb_data.data(), image.data.data(), argb_data.size() * sizeof(uint32_t));
    from_argb_cache = true;
  } else {
  // Determine number of components based on color space
  int num_components = 3;  // Default RGB
  ColorSpaceType cs_type = image.color_space.type;

  if (cs_type == ColorSpaceType::DeviceGray || cs_type == ColorSpaceType::CalGray) {
    num_components = 1;
  } else if (cs_type == ColorSpaceType::DeviceCMYK) {
    num_components = 4;
  } else if (cs_type == ColorSpaceType::Indexed) {
    num_components = 1;  // Index into lookup table
  } else if (cs_type == ColorSpaceType::ICCBased) {
    num_components = image.color_space.num_components;
    if (num_components == 0) num_components = 3;  // Default
  }

  bool converted_direct = false;
  bool handled_image_soft_mask = false;
  if (should_prescale && num_components == 3 && image.bits_per_component == 8 &&
      image.data.size() >= static_cast<size_t>(img_width) *
                               static_cast<size_t>(img_height) * 3) {
    ImageXObject smask_img;
    bool has_smask = false;
    bool smask_supported = true;
    if (current_pdf_ && image.soft_mask.type != Value::UNDEFINED &&
        image.soft_mask.type != Value::NULL_OBJ) {
      Value smask_val = image.soft_mask;
      uint32_t sm_obj = smask_val.ref_object_number;
      uint16_t sm_gen = smask_val.ref_generation_number;
      if (smask_val.type == Value::REFERENCE) {
        auto resolved = resolve_reference(*current_pdf_, sm_obj, sm_gen);
        if (resolved.success) smask_val = resolved.value;
      }
      if (smask_val.type == Value::STREAM) {
        smask_img = parse_image_xobject(*current_pdf_, smask_val, sm_obj, sm_gen);
        has_smask = !smask_img.data.empty() && smask_img.width > 0 &&
                    smask_img.height > 0;
        smask_supported = has_smask &&
            (smask_img.bits_per_component == 8 ||
             smask_img.bits_per_component == 1);
      }
    }

    if (smask_supported) {
      argb_data.resize(static_cast<size_t>(prescale_w) *
                       static_cast<size_t>(prescale_h));
      auto rgb_x_samples = make_bilinear_samples(img_width, prescale_w);
      auto rgb_y_samples = make_bilinear_samples(img_height, prescale_h);
      std::vector<BilinearSample> mask_x_samples;
      std::vector<BilinearSample> mask_y_samples;
      if (has_smask && smask_img.bits_per_component == 8) {
        mask_x_samples = make_bilinear_samples(smask_img.width, prescale_w);
        mask_y_samples = make_bilinear_samples(smask_img.height, prescale_h);
      }
      for (int y = 0; y < prescale_h; ++y) {
        const auto& sy = rgb_y_samples[static_cast<size_t>(y)];
        const uint8_t* row0 =
            image.data.data() + static_cast<size_t>(sy.i0) * img_width * 3;
        const uint8_t* row1 =
            image.data.data() + static_cast<size_t>(sy.i1) * img_width * 3;
        uint32_t* dst_row = argb_data.data() + static_cast<size_t>(y) * prescale_w;
        for (int x = 0; x < prescale_w; ++x) {
          const auto& sx = rgb_x_samples[static_cast<size_t>(x)];
          const uint8_t* p00 = row0 + sx.i0 * 3;
          const uint8_t* p10 = row0 + sx.i1 * 3;
          const uint8_t* p01 = row1 + sx.i0 * 3;
          const uint8_t* p11 = row1 + sx.i1 * 3;
          float w00 = sx.w0 * sy.w0;
          float w10 = sx.w1 * sy.w0;
          float w01 = sx.w0 * sy.w1;
          float w11 = sx.w1 * sy.w1;
          uint32_t r = clamp_to_byte(p00[0] * w00 + p10[0] * w10 +
                                     p01[0] * w01 + p11[0] * w11);
          uint32_t g = clamp_to_byte(p00[1] * w00 + p10[1] * w10 +
                                     p01[1] * w01 + p11[1] * w11);
          uint32_t b = clamp_to_byte(p00[2] * w00 + p10[2] * w10 +
                                     p01[2] * w01 + p11[2] * w11);
          uint32_t a = 255;
          if (has_smask) {
            int sw = smask_img.width;
            int sh = smask_img.height;
            if (smask_img.bits_per_component == 8) {
              const auto& msx = mask_x_samples[static_cast<size_t>(x)];
              const auto& msy = mask_y_samples[static_cast<size_t>(y)];
              auto alpha_at = [&](int sx, int sy) -> float {
                size_t si = static_cast<size_t>(sy) * sw + sx;
                return si < smask_img.data.size()
                    ? static_cast<float>(smask_img.data[si])
                    : 255.0f;
              };
              float aw00 = msx.w0 * msy.w0;
              float aw10 = msx.w1 * msy.w0;
              float aw01 = msx.w0 * msy.w1;
              float aw11 = msx.w1 * msy.w1;
              a = clamp_to_byte(alpha_at(msx.i0, msy.i0) * aw00 +
                                alpha_at(msx.i1, msy.i0) * aw10 +
                                alpha_at(msx.i0, msy.i1) * aw01 +
                                alpha_at(msx.i1, msy.i1) * aw11);
            } else {
              int mask_sx = std::min(sw - 1, std::max(0, (sx.i0 * sw) / img_width));
              int mask_sy = std::min(sh - 1, std::max(0, (sy.i0 * sh) / img_height));
              int stride = (sw + 7) / 8;
              size_t bi = static_cast<size_t>(mask_sy) * stride + (mask_sx / 8);
              if (bi < smask_img.data.size()) {
                a = ((smask_img.data[bi] >> (7 - (mask_sx % 8))) & 1u) ? 255u : 0u;
              }
            }
            r = (r * a + 127) / 255;
            g = (g * a + 127) / 255;
            b = (b * a + 127) / 255;
          }
          dst_row[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
      }
      img_width = prescale_w;
      img_height = prescale_h;
      pixels_are_premultiplied = has_smask;
      handled_image_soft_mask = has_smask;
      converted_direct = true;
      retain_converted_argb = false;
    }
  }

  if (!converted_direct) {
    argb_data.resize(static_cast<size_t>(img_width) * img_height);

  // Handle image mask (1-bit data)
  if (image.image_mask) {
    int stride = (img_width + 7) / 8;  // Bytes per row

    // PDF spec: With default Decode [0 1], sample 0 = paint, sample 1 = masked.
    // With Decode [1 0], sample 1 = paint, sample 0 = masked.
    bool invert = false;
    if (image.decode.size() >= 2 && image.decode[0] > image.decode[1]) {
      invert = true;  // Decode [1 0]: bit=1 means paint
    }

    uint32_t paint_color =
        (0xFFu << 24) | (static_cast<uint32_t>(fill_r) << 16) |
        (static_cast<uint32_t>(fill_g) << 8) | static_cast<uint32_t>(fill_b);

    for (int row = 0; row < img_height; row++) {
      for (int col = 0; col < img_width; col++) {
        int byte_idx = row * stride + col / 8;
        int bit_idx = 7 - (col % 8);

        if (byte_idx < static_cast<int>(image.data.size())) {
          bool bit_set = (image.data[byte_idx] >> bit_idx) & 1;
          // Default Decode [0 1]: bit=0 → paint, bit=1 → transparent
          // Decode [1 0]: bit=1 → paint, bit=0 → transparent
          bool should_paint = invert ? bit_set : !bit_set;
          argb_data[row * img_width + col] = should_paint ? paint_color : 0x00000000;
        }
      }
    }
  }
  // Handle indexed color space
  else if (cs_type == ColorSpaceType::Indexed) {
    const auto& lookup = image.color_space.lookup_table;
    int base_components = 3;  // Assume RGB base
    if (image.color_space.base_color_space) {
      ColorSpaceType base_type = image.color_space.base_color_space->type;
      if (base_type == ColorSpaceType::DeviceGray || base_type == ColorSpaceType::CalGray) {
        base_components = 1;
      } else if (base_type == ColorSpaceType::DeviceCMYK) {
        base_components = 4;
      }
    }

    // If the lookup table couldn't be resolved, fall back to treating pixel bytes
    // as direct grayscale — visually degraded but avoids a solid block.
    if (lookup.empty()) {
      for (int i = 0; i < img_width * img_height && i < static_cast<int>(image.data.size()); i++) {
        uint8_t gray = image.data[i];
        argb_data[i] = (0xFF << 24) | (gray << 16) | (gray << 8) | gray;
      }
    } else {
      for (int i = 0; i < img_width * img_height && i < static_cast<int>(image.data.size()); i++) {
        uint8_t idx = image.data[i];
        int lookup_idx = idx * base_components;

        uint8_t r = 0, g = 0, b = 0;
        if (base_components == 1 && lookup_idx < static_cast<int>(lookup.size())) {
          r = g = b = lookup[lookup_idx];
        } else if (base_components >= 3 && lookup_idx + 2 < static_cast<int>(lookup.size())) {
          r = lookup[lookup_idx];
          g = lookup[lookup_idx + 1];
          b = lookup[lookup_idx + 2];
        }
        argb_data[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
      }
    }
  }
  // Handle ICCBased color space with profile conversion
  else if (cs_type == ColorSpaceType::ICCBased) {
    std::vector<uint8_t> rgb_data;
    convert_icc_to_srgb(image.data, rgb_data, img_width, img_height, image.color_space);

    for (int i = 0; i < img_width * img_height; i++) {
      int src_idx = i * 3;
      if (src_idx + 2 < static_cast<int>(rgb_data.size())) {
        uint8_t r = rgb_data[src_idx];
        uint8_t g = rgb_data[src_idx + 1];
        uint8_t b = rgb_data[src_idx + 2];
        argb_data[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
      }
    }
  }
  // Handle Separation color space (single spot color)
  else if (cs_type == ColorSpaceType::Separation) {
    const auto& tint_func = image.color_space.tint_function;

    // Determine alternate color space components
    int alt_components = 4;  // Default CMYK
    if (image.color_space.base_color_space) {
      ColorSpaceType alt_type = image.color_space.base_color_space->type;
      if (alt_type == ColorSpaceType::DeviceGray || alt_type == ColorSpaceType::CalGray) {
        alt_components = 1;
      } else if (alt_type == ColorSpaceType::DeviceRGB || alt_type == ColorSpaceType::CalRGB) {
        alt_components = 3;
      }
    }

    // Build or retrieve the 256-entry tint LUT
    uint32_t func_obj = (tint_func.type == Value::REFERENCE) ? tint_func.ref_object_number : 0;
    TintLutKey lut_key{func_obj, static_cast<uint32_t>(alt_components)};
    const uint32_t* lut_data = nullptr;
    TintLutEntry lut_entry;

    auto cache_it = tint_lut_cache_.find(lut_key);
    if (cache_it != tint_lut_cache_.end()) {
      lut_data = cache_it->second.lut.data();
    } else {
      for (int t = 0; t < 256; ++t) {
        double tint = t / 255.0;
        std::vector<double> outputs;
        uint8_t r = 0, g = 0, b = 0;

        if (tint_func.type != Value::UNDEFINED && current_pdf_ &&
            evaluate_pdf_function(*current_pdf_, tint_func, {tint}, outputs) &&
            !outputs.empty()) {
          if (alt_components == 1) {
            uint8_t gray = static_cast<uint8_t>(outputs[0] * 255);
            r = g = b = gray;
          } else if (alt_components == 3 && outputs.size() >= 3) {
            r = static_cast<uint8_t>(outputs[0] * 255);
            g = static_cast<uint8_t>(outputs[1] * 255);
            b = static_cast<uint8_t>(outputs[2] * 255);
          } else if (alt_components == 4 && outputs.size() >= 4) {
            cmyk_to_rgb(static_cast<float>(outputs[0]),
                        static_cast<float>(outputs[1]),
                        static_cast<float>(outputs[2]),
                        static_cast<float>(outputs[3]), r, g, b);
          }
        } else {
          uint8_t gray = static_cast<uint8_t>((1.0 - tint) * 255);
          r = g = b = gray;
        }
        lut_entry.lut[t] = (0xFF << 24) | (r << 16) | (g << 8) | b;
      }
      tint_lut_cache_[lut_key] = std::move(lut_entry);
      lut_data = tint_lut_cache_[lut_key].lut.data();
    }

    int pixel_count = std::min(img_width * img_height, static_cast<int>(image.data.size()));
    for (int i = 0; i < pixel_count; ++i) {
      argb_data[i] = lut_data[image.data[i]];
    }
  }
  // Handle DeviceN color space (multiple spot colors)
  else if (cs_type == ColorSpaceType::DeviceN) {
    int n_colorants = static_cast<int>(image.color_space.colorant_names.size());
    if (n_colorants == 0) n_colorants = num_components;
    const auto& tint_func = image.color_space.tint_function;

    // Determine alternate color space components
    int alt_components = 4;  // Default CMYK
    if (image.color_space.base_color_space) {
      ColorSpaceType alt_type = image.color_space.base_color_space->type;
      if (alt_type == ColorSpaceType::DeviceGray || alt_type == ColorSpaceType::CalGray) {
        alt_components = 1;
      } else if (alt_type == ColorSpaceType::DeviceRGB || alt_type == ColorSpaceType::CalRGB) {
        alt_components = 3;
      }
    }

    for (int i = 0; i < img_width * img_height; i++) {
      int src_idx = i * n_colorants;

      // Collect tint values
      std::vector<double> inputs;
      for (int j = 0; j < n_colorants; j++) {
        if (src_idx + j < static_cast<int>(image.data.size())) {
          inputs.push_back(image.data[src_idx + j] / 255.0);
        } else {
          inputs.push_back(0.0);
        }
      }

      // Evaluate tint function
      std::vector<double> outputs;
      uint8_t r = 0, g = 0, b = 0;

      if (tint_func.type != Value::UNDEFINED && current_pdf_ &&
          evaluate_pdf_function(*current_pdf_, tint_func, inputs, outputs) &&
          !outputs.empty()) {
        // Convert alternate color to RGB
        if (alt_components == 1) {
          uint8_t gray = static_cast<uint8_t>(outputs[0] * 255);
          r = g = b = gray;
        } else if (alt_components == 3 && outputs.size() >= 3) {
          r = static_cast<uint8_t>(outputs[0] * 255);
          g = static_cast<uint8_t>(outputs[1] * 255);
          b = static_cast<uint8_t>(outputs[2] * 255);
        } else if (alt_components == 4 && outputs.size() >= 4) {
          cmyk_to_rgb(static_cast<float>(outputs[0]),
                      static_cast<float>(outputs[1]),
                      static_cast<float>(outputs[2]),
                      static_cast<float>(outputs[3]), r, g, b);
        }
      } else {
        // Fallback: average tints as gray
        double avg = 0;
        for (double t : inputs) avg += t;
        avg /= (inputs.empty() ? 1.0 : inputs.size());
        uint8_t gray = static_cast<uint8_t>((1.0 - avg) * 255);
        r = g = b = gray;
      }

      argb_data[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
    }
  }
  // Handle grayscale
  else if (num_components == 1) {
    // Check if this is 1-bit per pixel (packed bits)
    if (image.bits_per_component == 1) {
      // Unpack 1-bit data: 8 pixels per byte
      int stride = (img_width + 7) / 8;  // Bytes per row
      for (int row = 0; row < img_height; row++) {
        for (int col = 0; col < img_width; col++) {
          int byte_idx = row * stride + col / 8;
          int bit_idx = 7 - (col % 8);  // MSB first

          if (byte_idx < static_cast<int>(image.data.size())) {
            bool bit_set = (image.data[byte_idx] >> bit_idx) & 1;
            // CCITT decoder outputs: 1=white, 0=black (when BlackIs1=false, which is typical)
            uint8_t gray = bit_set ? 0xFF : 0x00;
            argb_data[row * img_width + col] = (0xFF << 24) | (gray << 16) | (gray << 8) | gray;
          }
        }
      }
    } else {
      // 8-bit grayscale
      for (int i = 0; i < img_width * img_height && i < static_cast<int>(image.data.size()); i++) {
        uint8_t gray = image.data[i];
        argb_data[i] = (0xFF << 24) | (gray << 16) | (gray << 8) | gray;
      }
    }
  }
  // Handle RGB
  else if (num_components == 3) {
    for (int i = 0; i < img_width * img_height; i++) {
      int src_idx = i * 3;
      if (src_idx + 2 < static_cast<int>(image.data.size())) {
        uint8_t r = image.data[src_idx];
        uint8_t g = image.data[src_idx + 1];
        uint8_t b = image.data[src_idx + 2];
        argb_data[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
      }
    }
  }
  // Handle CMYK (convert to RGB)
  else if (num_components == 4) {
    for (int i = 0; i < img_width * img_height; i++) {
      int src_idx = i * 4;
      if (src_idx + 3 < static_cast<int>(image.data.size())) {
        uint8_t r, g, b_val;
        cmyk_to_rgb(image.data[src_idx] / 255.0f, image.data[src_idx + 1] / 255.0f,
                    image.data[src_idx + 2] / 255.0f, image.data[src_idx + 3] / 255.0f,
                    r, g, b_val);
        argb_data[i] = (0xFF << 24) | (r << 16) | (g << 8) | b_val;
      }
    }
  }
  }

  // Apply image's own Soft Mask (SMask) as per-pixel alpha.
  // SMask is a grayscale image; each sample becomes alpha for the corresponding
  // base-image pixel. When dimensions differ, nearest-neighbor sample.
  if (!handled_image_soft_mask &&
      current_pdf_ && image.soft_mask.type != Value::UNDEFINED &&
      image.soft_mask.type != Value::NULL_OBJ) {
    Value smask_val = image.soft_mask;
    uint32_t sm_obj = smask_val.ref_object_number;
    uint16_t sm_gen = smask_val.ref_generation_number;
    if (smask_val.type == Value::REFERENCE) {
      auto resolved = resolve_reference(*current_pdf_, sm_obj, sm_gen);
      if (resolved.success) smask_val = resolved.value;
    }
    if (smask_val.type == Value::STREAM) {
      ImageXObject smask_img = parse_image_xobject(*current_pdf_, smask_val, sm_obj, sm_gen);
      if (!smask_img.data.empty() && smask_img.width > 0 && smask_img.height > 0) {
        int sw = smask_img.width;
        int sh = smask_img.height;
        // We load the Picture with lvg::ColorSpace::ARGB8888 (premultiplied),
        // so we must premultiply each RGB channel by the alpha we're applying.
        // If we wrote straight alpha here, ThorVG's downscaling filter would
        // blend the interior RGB with the palette's "outside" color (typically
        // black for PDF masked images), producing a dark halo at letter edges.
        auto premult_pixel = [](uint32_t& argb, uint32_t alpha) {
          uint32_t r = (argb >> 16) & 0xFF;
          uint32_t g = (argb >> 8) & 0xFF;
          uint32_t b = argb & 0xFF;
          // Round-to-nearest to avoid a one-bit bias that darkens fringe pixels.
          r = (r * alpha + 127) / 255;
          g = (g * alpha + 127) / 255;
          b = (b * alpha + 127) / 255;
          argb = (alpha << 24) | (r << 16) | (g << 8) | b;
        };
        // Only 8-bit grayscale masks handled here; other bit depths fall through.
        if (smask_img.bits_per_component == 8) {
          pixels_are_premultiplied = true;
          for (int py = 0; py < img_height; ++py) {
            int sy = (py * sh) / img_height;
            for (int px = 0; px < img_width; ++px) {
              int sx = (px * sw) / img_width;
              size_t si = static_cast<size_t>(sy) * sw + sx;
              if (si < smask_img.data.size()) {
                uint32_t alpha = smask_img.data[si];
                premult_pixel(argb_data[py * img_width + px], alpha);
              }
            }
          }
        } else if (smask_img.bits_per_component == 1) {
          pixels_are_premultiplied = true;
          int stride = (sw + 7) / 8;
          for (int py = 0; py < img_height; ++py) {
            int sy = (py * sh) / img_height;
            for (int px = 0; px < img_width; ++px) {
              int sx = (px * sw) / img_width;
              size_t bi = static_cast<size_t>(sy) * stride + (sx / 8);
              if (bi < smask_img.data.size()) {
                uint8_t bit = (smask_img.data[bi] >> (7 - (sx % 8))) & 1u;
                uint32_t alpha = bit ? 0xFFu : 0x00u;
                premult_pixel(argb_data[py * img_width + px], alpha);
              }
            }
          }
        }
      }
    }
  }
  }  // else (non-cached ARGB path)

  if (!argb_data.empty() && !image.image_mask) {
    // Keep extra samples for downscale filtering quality, but do not keep
    // full-resolution image buffers alive when the page only uses a small
    // on-canvas footprint. This is important for papers with many 1080px
    // figure thumbnails drawn at tens of PDF points.
    int target_w = std::max(1, std::min(prescale_w, img_width));
    int target_h = std::max(1, std::min(prescale_h, img_height));
    if (target_w * 2 <= img_width && target_h * 2 <= img_height) {
      downsample_argb_bilinear(argb_data, img_width, img_height,
                               target_w, target_h);
      img_width = target_w;
      img_height = target_h;
      // A per-object cache key cannot distinguish different draw sizes.
      retain_converted_argb = false;
      from_argb_cache = false;
    }
  }

  // Create LightVG Picture from raw data.
  auto picture = lvg::Picture::gen();
  if (!picture) {
    return false;
  }

  lvg::Result result = lvg::Result::Unknown;
  if (!retain_converted_argb && !pixels_are_premultiplied) {
    result = picture->load_argb_owned(std::move(argb_data), img_width, img_height,
                                      false);
  } else {
    result = picture->load(argb_data.data(), img_width, img_height,
                           lvg::ColorSpace::ARGB8888, pixels_are_premultiplied);
  }
  if (result != lvg::Result::Success) {
    picture->unref();
    return false;
  }

  if (retain_converted_argb && !from_argb_cache) {
    if (pixels_are_premultiplied) {
      for (uint32_t& p : argb_data) {
        uint32_t a = (p >> 24) & 0xff;
        if (a > 0 && a < 255) {
          uint32_t r = std::min<uint32_t>(255, (((p >> 16) & 0xff) * 255) / a);
          uint32_t g = std::min<uint32_t>(255, (((p >> 8) & 0xff) * 255) / a);
          uint32_t b = std::min<uint32_t>(255, ((p & 0xff) * 255) / a);
          p = (a << 24) | (r << 16) | (g << 8) | b;
        }
      }
    }
    // Move the converted pixels to the side-channel cache buffer after
    // Picture::load() has copied them. This avoids a second full-image copy.
    last_image_argb_ = std::move(argb_data);
  }
  picture->interpolate(image.interpolate);

  // Apply transformation using matrix for non-uniform scaling.
  // PDF images map their unit square through the current CTM. Source pixel
  // row 0 is the top of that square, so source v maps to PDF y=(1 - v/h).
  lvg::Matrix m;
  if (image_ctm) {
    float inv_w = 1.0f / static_cast<float>(img_width);
    float inv_h = 1.0f / static_cast<float>(img_height);
    m.e11 = image_ctm->a * inv_w * state_.scale;
    m.e12 = -image_ctm->c * inv_h * state_.scale;
    m.e13 = (image_ctm->c + image_ctm->e) * state_.scale;
    m.e21 = -image_ctm->b * inv_w * state_.scale;
    m.e22 = image_ctm->d * inv_h * state_.scale;
    m.e23 = (state_.page_height - image_ctm->d - image_ctm->f) * state_.scale;
    NANOPDF_LOG_DEBUG("LightVG",
                      "draw_image CTM: [%.3f %.3f %.3f %.3f %.3f %.3f] -> "
                      "matrix=[%.3f %.3f %.3f; %.3f %.3f %.3f]",
                      image_ctm->a, image_ctm->b, image_ctm->c, image_ctm->d,
                      image_ctm->e, image_ctm->f, m.e11, m.e12, m.e13,
                      m.e21, m.e22, m.e23);
  } else {
    // PDF coordinate: (x, y) is bottom-left, we need to flip Y for screen coordinates.
    float scale_x = width / img_width;
    float scale_y = height / img_height;
    float canvas_x = x * state_.scale;
    float canvas_y = (state_.page_height - y - height) * state_.scale;

    NANOPDF_LOG_DEBUG("LightVG", "draw_image transform: page_h=%.1f, scale=%.3f, canvas=(%.1f,%.1f)",
                      state_.page_height, state_.scale, canvas_x, canvas_y);

    m.e11 = scale_x * state_.scale;
    m.e12 = 0.0f;
    m.e13 = canvas_x;
    m.e21 = 0.0f;
    m.e22 = scale_y * state_.scale;
    m.e23 = canvas_y;
  }
  m.e31 = 0.0f;
  m.e32 = 0.0f;
  m.e33 = 1.0f;
  picture->transform(m);

  // Apply blend mode
  if (state_.blend_mode != 0) {
    picture->blend(static_cast<lvg::BlendMethod>(state_.blend_mode));
  }
  if (state_.fill_opacity < 1.0f) {
    float opacity = std::max(0.0f, std::min(1.0f, state_.fill_opacity));
    picture->opacity(static_cast<uint8_t>(opacity * 255.0f + 0.5f));
  }
  if (state_.has_clip && !state_.clip_commands.empty()) {
    auto clipper = lvg::Shape::gen();
    if (clipper) {
      append_shape_geometry(clipper, state_.clip_commands, state_.clip_points);
      clipper->fillRule(state_.clip_even_odd ? lvg::FillRule::EvenOdd
                                             : lvg::FillRule::NonZero);
      if (picture->clip(clipper) != lvg::Result::Success) {
        clipper->unref();
      }
    }
  }

  // Push to scene
  apply_soft_mask_opacity(picture);
  auto push_result = scene_->add(picture);
  NANOPDF_LOG_DEBUG("LightVG", "draw_image: pushed to scene, result=%d", static_cast<int>(push_result));

  return true;
}

// Helper function to extract color stops from a PDF function
// Returns color stops for gradient fills. Uses cache when available.
static std::vector<lvg::Fill::ColorStop> extract_color_stops_from_function(
    const Pdf& pdf, const Value& function,
    std::unordered_map<uint32_t, LightVGColorStopCacheEntry>* cache = nullptr) {

  if (cache && function.type == Value::REFERENCE) {
    uint32_t func_obj = function.ref_object_number;
    auto it = cache->find(func_obj);
    if (it != cache->end()) {
      return it->second.stops;
    }
  }

  std::vector<lvg::Fill::ColorStop> stops;

  if (function.type != Value::DICTIONARY) {
    // Default black to white
    stops.push_back({0.0f, 0, 0, 0, 255});
    stops.push_back({1.0f, 255, 255, 255, 255});
    return stops;
  }

  auto func_type_it = function.dict.find("FunctionType");
  if (func_type_it == function.dict.end() || func_type_it->second.type != Value::NUMBER) {
    stops.push_back({0.0f, 0, 0, 0, 255});
    stops.push_back({1.0f, 255, 255, 255, 255});
    return stops;
  }

  int func_type = static_cast<int>(func_type_it->second.number);

  if (func_type == 2) {
    // Type 2: Exponential interpolation function
    // C0 = color at t=0, C1 = color at t=1
    auto c0_it = function.dict.find("C0");
    auto c1_it = function.dict.find("C1");

    lvg::Fill::ColorStop stop0 = {0.0f, 0, 0, 0, 255};
    lvg::Fill::ColorStop stop1 = {1.0f, 255, 255, 255, 255};

    if (c0_it != function.dict.end() && c0_it->second.type == Value::ARRAY &&
        c0_it->second.array.size() >= 3) {
      float r = c0_it->second.array[0].type == Value::NUMBER ?
                static_cast<float>(c0_it->second.array[0].number) : 0.0f;
      float g = c0_it->second.array[1].type == Value::NUMBER ?
                static_cast<float>(c0_it->second.array[1].number) : 0.0f;
      float b = c0_it->second.array[2].type == Value::NUMBER ?
                static_cast<float>(c0_it->second.array[2].number) : 0.0f;
      stop0 = {0.0f, static_cast<uint8_t>(r * 255),
               static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255), 255};
    }

    if (c1_it != function.dict.end() && c1_it->second.type == Value::ARRAY &&
        c1_it->second.array.size() >= 3) {
      float r = c1_it->second.array[0].type == Value::NUMBER ?
                static_cast<float>(c1_it->second.array[0].number) : 1.0f;
      float g = c1_it->second.array[1].type == Value::NUMBER ?
                static_cast<float>(c1_it->second.array[1].number) : 1.0f;
      float b = c1_it->second.array[2].type == Value::NUMBER ?
                static_cast<float>(c1_it->second.array[2].number) : 1.0f;
      stop1 = {1.0f, static_cast<uint8_t>(r * 255),
               static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255), 255};
    }

    stops.push_back(stop0);
    stops.push_back(stop1);
  }
  else if (func_type == 3) {
    // Type 3: Stitching function - combines multiple sub-functions
    // Functions = array of sub-functions
    // Bounds = array of boundary values between sub-functions
    // Encode = array mapping each subdomain to sub-function domain
    auto functions_it = function.dict.find("Functions");
    auto bounds_it = function.dict.find("Bounds");

    if (functions_it == function.dict.end() || functions_it->second.type != Value::ARRAY) {
      stops.push_back({0.0f, 0, 0, 0, 255});
      stops.push_back({1.0f, 255, 255, 255, 255});
      return stops;
    }

    const auto& functions = functions_it->second.array;

    // Build boundary positions: [0, bounds[0], bounds[1], ..., 1]
    std::vector<float> boundaries;
    boundaries.push_back(0.0f);

    if (bounds_it != function.dict.end() && bounds_it->second.type == Value::ARRAY) {
      for (const auto& b : bounds_it->second.array) {
        if (b.type == Value::NUMBER) {
          boundaries.push_back(static_cast<float>(b.number));
        }
      }
    }
    boundaries.push_back(1.0f);

    // For each sub-function, extract its start color
    for (size_t i = 0; i < functions.size() && i < boundaries.size(); i++) {
      Value sub_func = functions[i];

      // Resolve reference if needed
      if (sub_func.type == Value::REFERENCE) {
        auto resolved = resolve_reference(pdf, sub_func.ref_object_number,
                                          sub_func.ref_generation_number);
        if (resolved.success) {
          sub_func = resolved.value;
        }
      }

      if (sub_func.type == Value::DICTIONARY) {
        auto sub_c0_it = sub_func.dict.find("C0");
        if (sub_c0_it != sub_func.dict.end() && sub_c0_it->second.type == Value::ARRAY &&
            sub_c0_it->second.array.size() >= 3) {
          float r = sub_c0_it->second.array[0].type == Value::NUMBER ?
                    static_cast<float>(sub_c0_it->second.array[0].number) : 0.0f;
          float g = sub_c0_it->second.array[1].type == Value::NUMBER ?
                    static_cast<float>(sub_c0_it->second.array[1].number) : 0.0f;
          float b = sub_c0_it->second.array[2].type == Value::NUMBER ?
                    static_cast<float>(sub_c0_it->second.array[2].number) : 0.0f;
          stops.push_back({boundaries[i], static_cast<uint8_t>(r * 255),
                           static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255), 255});
        }
      }
    }

    // Add final color from last sub-function's C1
    if (!functions.empty()) {
      Value last_func = functions.back();
      if (last_func.type == Value::REFERENCE) {
        auto resolved = resolve_reference(pdf, last_func.ref_object_number,
                                          last_func.ref_generation_number);
        if (resolved.success) {
          last_func = resolved.value;
        }
      }

      if (last_func.type == Value::DICTIONARY) {
        auto c1_it = last_func.dict.find("C1");
        if (c1_it != last_func.dict.end() && c1_it->second.type == Value::ARRAY &&
            c1_it->second.array.size() >= 3) {
          float r = c1_it->second.array[0].type == Value::NUMBER ?
                    static_cast<float>(c1_it->second.array[0].number) : 1.0f;
          float g = c1_it->second.array[1].type == Value::NUMBER ?
                    static_cast<float>(c1_it->second.array[1].number) : 1.0f;
          float b = c1_it->second.array[2].type == Value::NUMBER ?
                    static_cast<float>(c1_it->second.array[2].number) : 1.0f;
          stops.push_back({1.0f, static_cast<uint8_t>(r * 255),
                           static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255), 255});
        }
      }
    }

    // Ensure we have at least 2 stops
    if (stops.size() < 2) {
      stops.clear();
      stops.push_back({0.0f, 0, 0, 0, 255});
      stops.push_back({1.0f, 255, 255, 255, 255});
    }
  }
  else {
  // Unsupported function type - default gradient
  stops.push_back({0.0f, 0, 0, 0, 255});
  stops.push_back({1.0f, 255, 255, 255, 255});
}

// Store in cache
if (cache && function.type == Value::REFERENCE) {
  uint32_t func_obj = function.ref_object_number;
  (*cache)[func_obj].stops = stops;
}

return stops;
}

// PDF function evaluator - evaluates a PDF function at given input values
// Returns output color values (typically RGB, each in range 0-1)
static bool evaluate_pdf_function(const Pdf& pdf, const Value& function,
                                   const std::vector<double>& inputs,
                                   std::vector<double>& outputs) {
  return pdfunc::evaluate(pdf, function, inputs, outputs);
}

// Rasterize function-based shading to a bitmap
static bool rasterize_function_shading(const Pdf& pdf, const Shading& shading,
                                        int width, int height, float scale,
                                        float page_height,
                                        std::vector<uint32_t>& pixels) {
  pixels.resize(width * height);

  // Get domain bounds
  double x_min = shading.domain.size() >= 1 ? shading.domain[0] : 0.0;
  double x_max = shading.domain.size() >= 2 ? shading.domain[1] : 1.0;
  double y_min = shading.domain.size() >= 3 ? shading.domain[2] : 0.0;
  double y_max = shading.domain.size() >= 4 ? shading.domain[3] : 1.0;

  // Get transformation matrix
  double a = shading.matrix.size() >= 1 ? shading.matrix[0] : 1.0;
  double b = shading.matrix.size() >= 2 ? shading.matrix[1] : 0.0;
  double c = shading.matrix.size() >= 3 ? shading.matrix[2] : 0.0;
  double d = shading.matrix.size() >= 4 ? shading.matrix[3] : 1.0;
  double e = shading.matrix.size() >= 5 ? shading.matrix[4] : 0.0;
  double f = shading.matrix.size() >= 6 ? shading.matrix[5] : 0.0;

  // Compute inverse matrix for device-to-domain transform
  double det = a * d - b * c;
  if (std::abs(det) < 1e-10) {
    // Degenerate matrix, fill with background or gray
    std::fill(pixels.begin(), pixels.end(), 0xFF808080);
    return true;
  }

  double inv_a = d / det;
  double inv_b = -b / det;
  double inv_c = -c / det;
  double inv_d = a / det;
  double inv_e = (c * f - d * e) / det;
  double inv_f = (b * e - a * f) / det;

  std::vector<double> inputs(2);
  std::vector<double> outputs;
  outputs.reserve(4);

  // Rasterize each pixel. Advance the inverse transform incrementally within
  // each row to avoid repeated divides and matrix multiplies in dense shadings.
  for (int py = 0; py < height; ++py) {
    double uy = (height - 1 - py) / scale;
    double dx = inv_c * uy + inv_e;
    double dy = inv_d * uy + inv_f;
    double dx_step = inv_a / scale;
    double dy_step = inv_b / scale;

    for (int px = 0; px < width; ++px) {
      // Check if in domain
      uint8_t r = 128, g = 128, b = 128, alpha = 255;

      if (dx >= x_min && dx <= x_max && dy >= y_min && dy <= y_max) {
        // Evaluate function at (dx, dy)
        inputs[0] = dx;
        inputs[1] = dy;
        outputs.clear();
        if (evaluate_pdf_function(pdf, shading.function, inputs, outputs)) {
          if (outputs.size() >= 3) {
            r = static_cast<uint8_t>(clamp14(outputs[0], 0.0, 1.0) * 255);
            g = static_cast<uint8_t>(clamp14(outputs[1], 0.0, 1.0) * 255);
            b = static_cast<uint8_t>(clamp14(outputs[2], 0.0, 1.0) * 255);
          } else if (outputs.size() == 1) {
            // Grayscale
            uint8_t gray = static_cast<uint8_t>(clamp14(outputs[0], 0.0, 1.0) * 255);
            r = g = b = gray;
          }
        }
      } else if (!shading.background.empty()) {
        // Use background color outside domain
        if (shading.background.size() >= 3) {
          r = static_cast<uint8_t>(clamp14(shading.background[0], 0.0, 1.0) * 255);
          g = static_cast<uint8_t>(clamp14(shading.background[1], 0.0, 1.0) * 255);
          b = static_cast<uint8_t>(clamp14(shading.background[2], 0.0, 1.0) * 255);
        }
      } else {
        // Transparent outside domain
        alpha = 0;
      }

      pixels[py * width + px] = (alpha << 24) | (r << 16) | (g << 8) | b;
      dx += dx_step;
      dy += dy_step;
    }
  }

  return true;
}

// Helper to lookup a resource, checking Form XObject resources first, then
// page resources. Returns a pointer that is valid until the next render_page
// (lookup_resolved_owned_ provides stable storage for resolved REFERENCEs;
// otherwise the pointer is into the original page/form resource dict).
const Value* LightVGBackend::lookup_resource(const std::string& resource_type,
                                             const std::string& name) const {
  auto resolve_into_dict = [this](const Value& v,
                                  const Value*& dict_out) -> bool {
    if (v.type == Value::DICTIONARY) {
      dict_out = &v;
      return true;
    }
    if (v.type == Value::REFERENCE && current_pdf_) {
      auto resolved = resolve_reference(*current_pdf_,
                                        v.ref_object_number,
                                        v.ref_generation_number);
      if (resolved.success && resolved.value.type == Value::DICTIONARY) {
        // Store resolved Value in stable storage (deque never invalidates).
        lookup_resolved_owned_.push_back(std::move(resolved.value));
        dict_out = &lookup_resolved_owned_.back();
        return true;
      }
    }
    return false;
  };

  // Check Form XObject resources stack (from top to bottom)
  for (auto it = form_resources_stack_.rbegin(); it != form_resources_stack_.rend(); ++it) {
    auto type_it = it->find(resource_type);
    if (type_it != it->end()) {
      const Value* dict = nullptr;
      if (resolve_into_dict(type_it->second, dict) && dict) {
        auto name_it = dict->dict.find(name);
        if (name_it != dict->dict.end()) {
          return &name_it->second;
        }
      }
    }
  }

  // Fall back to page resources
  if (current_page_) {
    auto type_it = current_page_->resources.find(resource_type);
    if (type_it != current_page_->resources.end()) {
      const Value* dict = nullptr;
      if (resolve_into_dict(type_it->second, dict) && dict) {
        auto name_it = dict->dict.find(name);
        if (name_it != dict->dict.end()) {
          return &name_it->second;
        }
      }
    }
  }

  return nullptr;
}

static lvg::BlendMethod blend_method_from_pdf_name(const std::string& name) {
  if (name == "Multiply") return lvg::BlendMethod::Multiply;
  if (name == "Screen") return lvg::BlendMethod::Screen;
  if (name == "Overlay") return lvg::BlendMethod::Overlay;
  if (name == "Darken") return lvg::BlendMethod::Darken;
  if (name == "Lighten") return lvg::BlendMethod::Lighten;
  if (name == "ColorDodge") return lvg::BlendMethod::ColorDodge;
  if (name == "ColorBurn") return lvg::BlendMethod::ColorBurn;
  if (name == "HardLight") return lvg::BlendMethod::HardLight;
  if (name == "SoftLight") return lvg::BlendMethod::SoftLight;
  if (name == "Difference") return lvg::BlendMethod::Difference;
  if (name == "Exclusion") return lvg::BlendMethod::Exclusion;
  if (name == "Hue") return lvg::BlendMethod::Hue;
  if (name == "Saturation") return lvg::BlendMethod::Saturation;
  if (name == "Color") return lvg::BlendMethod::Color;
  if (name == "Luminosity") return lvg::BlendMethod::Luminosity;
  return lvg::BlendMethod::Normal;
}

bool LightVGBackend::apply_extgstate(const std::string& name) {
  if (!current_pdf_) return false;
  std::string gs_name = name;
  if (!gs_name.empty() && gs_name[0] == '/') {
    gs_name = gs_name.substr(1);
  }
  const Value* gs_value = lookup_resource("ExtGState", gs_name);
  if (!gs_value) return false;

  Value resolved_value = *gs_value;
  if (resolved_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      resolved_value.ref_object_number,
                                      resolved_value.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    resolved_value = std::move(resolved.value);
  }
  if (resolved_value.type != Value::DICTIONARY) return false;
  return apply_extgstate_dict(resolved_value.dict);
}

bool LightVGBackend::apply_extgstate_dict(const Dictionary& gs_dict) {
  auto number = [&](const char* key, float* out) {
    auto it = gs_dict.find(key);
    if (it != gs_dict.end() && it->second.type == Value::NUMBER) {
      *out = static_cast<float>(it->second.number);
      return true;
    }
    return false;
  };
  auto boolean = [&](const char* key, bool* out) {
    auto it = gs_dict.find(key);
    if (it != gs_dict.end() && it->second.type == Value::BOOLEAN) {
      *out = it->second.boolean;
      return true;
    }
    return false;
  };

  number("ca", &state_.fill_opacity);
  state_.fill_opacity = clamp14(state_.fill_opacity, 0.0f, 1.0f);

  number("CA", &state_.stroke_opacity);
  state_.stroke_opacity = clamp14(state_.stroke_opacity, 0.0f, 1.0f);

  auto bm_it = gs_dict.find("BM");
  if (bm_it != gs_dict.end()) {
    std::string bm_name;
    if (bm_it->second.type == Value::NAME) {
      bm_name = bm_it->second.name;
    } else if (bm_it->second.type == Value::ARRAY) {
      for (const Value& item : bm_it->second.array) {
        if (item.type == Value::NAME) {
          bm_name = item.name;
          break;
        }
      }
    }
    state_.blend_mode = static_cast<int>(blend_method_from_pdf_name(bm_name));
  }

  number("LW", &state_.stroke_width);

  float tmp = 0.0f;
  if (number("LC", &tmp)) state_.line_cap = static_cast<int>(tmp);
  if (number("LJ", &tmp)) state_.line_join = static_cast<int>(tmp);
  number("ML", &state_.miter_limit);

  boolean("AIS", &state_.alpha_is_shape);
  boolean("TK", &state_.text_knockout);
  boolean("OP", &state_.overprint_stroke);
  boolean("op", &state_.overprint_fill);
  if (number("OPM", &tmp)) state_.overprint_mode = static_cast<int>(tmp);
  number("FL", &state_.flatness);
  boolean("SA", &state_.stroke_adjustment);

  auto ri_it = gs_dict.find("RI");
  if (ri_it != gs_dict.end() && ri_it->second.type == Value::NAME) {
    state_.rendering_intent = ri_it->second.name;
  }

  auto dash_it = gs_dict.find("D");
  if (dash_it != gs_dict.end() && dash_it->second.type == Value::ARRAY &&
      dash_it->second.array.size() >= 2) {
    const Value& dash_array = dash_it->second.array[0];
    const Value& dash_phase = dash_it->second.array[1];
    if (dash_array.type == Value::ARRAY) {
      state_.dash_pattern.clear();
      for (const Value& v : dash_array.array) {
        if (v.type == Value::NUMBER) {
          state_.dash_pattern.push_back(static_cast<float>(v.number));
        }
      }
    }
    if (dash_phase.type == Value::NUMBER) {
      state_.dash_phase = static_cast<float>(dash_phase.number);
    }
  }

  auto smask_it = gs_dict.find("SMask");
  if (smask_it != gs_dict.end()) {
    if (rendering_soft_mask_group_) {
      return true;
    }
    const Value& smask = smask_it->second;
    if (smask.type == Value::NAME && smask.name == "None") {
      state_.has_soft_mask = false;
      state_.soft_mask_type = 0;
      state_.soft_mask_data.clear();
      state_.soft_mask_width = 0;
      state_.soft_mask_height = 0;
      current_soft_mask_.reset();
    } else if (smask.type == Value::DICTIONARY || smask.type == Value::REFERENCE) {
      Dictionary smask_dict;
      if (smask.type == Value::DICTIONARY) {
        smask_dict = smask.dict;
      } else if (current_pdf_) {
        auto resolved = resolve_reference(*current_pdf_,
                                          smask.ref_object_number,
                                          smask.ref_generation_number);
        if (resolved.success && resolved.value.type == Value::DICTIONARY) {
          smask_dict = std::move(resolved.value.dict);
        }
      }
      if (!smask_dict.empty()) {
        int mask_type = 1;
        auto s_it = smask_dict.find("S");
        if (s_it != smask_dict.end() && s_it->second.type == Value::NAME &&
            s_it->second.name == "Luminosity") {
          mask_type = 2;
        }
        auto g_it = smask_dict.find("G");
        if (g_it != smask_dict.end()) {
          state_.has_soft_mask = true;
          state_.soft_mask_type = mask_type;
          if (render_soft_mask_group(g_it->second, mask_type)) {
            current_soft_mask_.reset();
          } else {
            state_.has_soft_mask = false;
          }
        }
      }
    }
  }

  return true;
}

bool LightVGBackend::draw_shading(const std::string& shading_name) {
  if (!scene_ || !current_pdf_ || !current_page_) {
    return false;
  }

  const Value* shading_value_ptr = lookup_resource("Shading", shading_name);
  if (!shading_value_ptr) return false;
  const Value& shading_value = *shading_value_ptr;

  // Resolve reference if needed
  Value resolved_shading = shading_value;
  if (shading_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_, shading_value.ref_object_number,
                                      shading_value.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    resolved_shading = resolved.value;
  }

  if (resolved_shading.type != Value::DICTIONARY) {
    return false;
  }

  // Parse shading
  auto shading = parse_shading(*current_pdf_, resolved_shading.dict);
  if (!shading) {
    return false;
  }

  // Create shape that covers the current clipping area (or entire page)
  auto shape = lvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Default to full page bounds
  float x = 0;
  float y = 0;
  float w = state_.page_width * state_.scale;
  float h = state_.page_height * state_.scale;

  // Use shading BBox if available
  if (shading->bbox.size() >= 4) {
    x = static_cast<float>(shading->bbox[0]) * state_.scale;
    y = (state_.page_height - static_cast<float>(shading->bbox[3])) * state_.scale;
    w = (static_cast<float>(shading->bbox[2]) - static_cast<float>(shading->bbox[0])) * state_.scale;
    h = (static_cast<float>(shading->bbox[3]) - static_cast<float>(shading->bbox[1])) * state_.scale;
  }

  shape->appendRect(x, y, w, h, 0, 0);

  // Create gradient based on shading type
  if (shading->type == ShadingType::Axial && shading->coords.size() >= 4) {
    // Linear gradient
    auto gradient = lvg::LinearGradient::gen();
    if (!gradient) {
      shape->unref();
      return false;
    }

    // Coords are [x0, y0, x1, y1]
    float x0 = static_cast<float>(shading->coords[0]) * state_.scale;
    float y0 = (state_.page_height - static_cast<float>(shading->coords[1])) * state_.scale;
    float x1 = static_cast<float>(shading->coords[2]) * state_.scale;
    float y1 = (state_.page_height - static_cast<float>(shading->coords[3])) * state_.scale;

    gradient->linear(x0, y0, x1, y1);

    // Extract color stops from function (supports Type 2 and Type 3 functions)
    auto colorStops = extract_color_stops_from_function(*current_pdf_, shading->function, &color_stop_cache_);
    gradient->colorStops(colorStops.data(), colorStops.size());

    // Set spread mode based on extend flags
    if (shading->extend.size() >= 2 && (shading->extend[0] || shading->extend[1])) {
      gradient->spread(lvg::FillSpread::Pad);
    }

    shape->fill(gradient);
  }
  else if (shading->type == ShadingType::Radial && shading->coords.size() >= 6) {
    // Radial gradient
    auto gradient = lvg::RadialGradient::gen();
    if (!gradient) {
      shape->unref();
      return false;
    }

    // Coords are [x0, y0, r0, x1, y1, r1]
    float x0 = static_cast<float>(shading->coords[0]) * state_.scale;
    float y0 = (state_.page_height - static_cast<float>(shading->coords[1])) * state_.scale;
    float r0 = static_cast<float>(shading->coords[2]) * state_.scale;
    float x1 = static_cast<float>(shading->coords[3]) * state_.scale;
    float y1 = (state_.page_height - static_cast<float>(shading->coords[4])) * state_.scale;
    float r1 = static_cast<float>(shading->coords[5]) * state_.scale;

    // ThorVG radial uses (cx, cy, r, fx, fy, fr) where:
    // - (cx, cy, r) is the outer circle
    // - (fx, fy, fr) is the focal/inner circle
    gradient->radial(x1, y1, r1, x0, y0, r0);

    // Extract color stops from function (supports Type 2 and Type 3 functions)
    auto colorStops = extract_color_stops_from_function(*current_pdf_, shading->function, &color_stop_cache_);
    gradient->colorStops(colorStops.data(), colorStops.size());

    if (shading->extend.size() >= 2 && (shading->extend[0] || shading->extend[1])) {
      gradient->spread(lvg::FillSpread::Pad);
    }

    shape->fill(gradient);
  }
  else if (shading->type == ShadingType::FunctionBased) {
    // Type 1: Function-based shading
    // Evaluate function at each point in domain and rasterize to bitmap

#if NANOPDF_DEBUG_PRINT
    printf("DEBUG: Type 1 (Function-based) shading requested\n");
    printf("DEBUG: Domain=[%g,%g,%g,%g], Matrix=[%g,%g,%g,%g,%g,%g]\n",
           shading->domain.size() >= 4 ? shading->domain[0] : 0,
           shading->domain.size() >= 4 ? shading->domain[1] : 1,
           shading->domain.size() >= 4 ? shading->domain[2] : 0,
           shading->domain.size() >= 4 ? shading->domain[3] : 1,
           shading->matrix.size() >= 6 ? shading->matrix[0] : 1,
           shading->matrix.size() >= 6 ? shading->matrix[1] : 0,
           shading->matrix.size() >= 6 ? shading->matrix[2] : 0,
           shading->matrix.size() >= 6 ? shading->matrix[3] : 1,
           shading->matrix.size() >= 6 ? shading->matrix[4] : 0,
           shading->matrix.size() >= 6 ? shading->matrix[5] : 0);
#endif

    // Rasterize the function-based shading to a bitmap
    int raster_w = static_cast<int>(w);
    int raster_h = static_cast<int>(h);
    if (raster_w <= 0) raster_w = 256;
    if (raster_h <= 0) raster_h = 256;
    if (max_function_shading_pixels_ > 0) {
      size_t pixels = static_cast<size_t>(raster_w) * static_cast<size_t>(raster_h);
      if (pixels > max_function_shading_pixels_) {
        double ratio = std::sqrt(static_cast<double>(max_function_shading_pixels_) /
                                 static_cast<double>(pixels));
        raster_w = std::max(1, static_cast<int>(std::floor(raster_w * ratio)));
        raster_h = std::max(1, static_cast<int>(std::floor(raster_h * ratio)));
      }
    }

    // Check render cache first
    uint32_t shading_obj = (shading_value.type == Value::REFERENCE) ? shading_value.ref_object_number : 0;
    std::string cache_key = "shading_func:" + std::to_string(shading_obj) + ":" +
                            std::to_string(raster_w) + "x" + std::to_string(raster_h);
    RenderCacheEntry cached;
    if (RenderCache::instance().find(cache_key, cached)) {
      auto picture = lvg::Picture::gen();
      if (picture) {
        auto result = picture->load(reinterpret_cast<const uint32_t*>(cached.data.data()),
                                     raster_w, raster_h, lvg::ColorSpace::ARGB8888, false);
        if (result == lvg::Result::Success) {
          lvg::Matrix m;
          m.e11 = w / static_cast<float>(raster_w); m.e12 = 0.0f; m.e13 = x;
          m.e21 = 0.0f; m.e22 = h / static_cast<float>(raster_h); m.e23 = y;
          m.e31 = 0.0f; m.e32 = 0.0f; m.e33 = 1.0f;
          picture->transform(m);
          apply_soft_mask_opacity(picture);
          scene_->add(picture);
          shape->unref();
          return true;
        }
      }
    }

    std::vector<uint32_t> pixels;
    if (rasterize_function_shading(*current_pdf_, *shading, raster_w, raster_h,
                                    state_.scale, state_.page_height, pixels)) {
      // Store in render cache
      RenderCacheEntry entry;
      entry.width = raster_w;
      entry.height = raster_h;
      entry.data.resize(pixels.size() * sizeof(uint32_t));
      std::copy(pixels.begin(), pixels.end(),
                reinterpret_cast<uint32_t*>(entry.data.data()));
      RenderCache::instance().store(cache_key, std::move(entry));

      // Create LightVG Picture from rasterized bitmap. The cache already has
      // its own copy, so transfer the live raster buffer into the picture.
      auto picture = lvg::Picture::gen();
      if (picture) {
        auto result = picture->load_argb_owned(std::move(pixels), raster_w,
                                               raster_h, false);
        if (result == lvg::Result::Success) {
          // Position the picture at the shape location
          lvg::Matrix m;
          m.e11 = w / static_cast<float>(raster_w); m.e12 = 0.0f; m.e13 = x;
          m.e21 = 0.0f; m.e22 = h / static_cast<float>(raster_h); m.e23 = y;
          m.e31 = 0.0f; m.e32 = 0.0f; m.e33 = 1.0f;
          picture->transform(m);

          apply_soft_mask_opacity(picture);
          scene_->add(picture);
          shape->unref();  // Don't need the shape anymore
          return true;
        } else {
          picture->unref();
        }
      }
    }

    // Fallback: use gradient approximation
#if NANOPDF_DEBUG_PRINT
    printf("DEBUG: Function rasterization failed, using gradient fallback\n");
#endif
    auto gradient = lvg::LinearGradient::gen();
    if (!gradient) {
      shape->unref();
      return false;
    }

    float x0 = 0, y0 = 0, x1 = w, y1 = 0;
    if (shading->domain.size() >= 4 && shading->matrix.size() >= 6) {
      float xmin = static_cast<float>(shading->domain[0]);
      float xmax = static_cast<float>(shading->domain[1]);
      float a = static_cast<float>(shading->matrix[0]);
      float e = static_cast<float>(shading->matrix[4]);
      x0 = (a * xmin + e) * state_.scale;
      x1 = (a * xmax + e) * state_.scale;
    }

    gradient->linear(x0, y0, x1, y1);
    auto colorStops = extract_color_stops_from_function(*current_pdf_, shading->function, &color_stop_cache_);
    gradient->colorStops(colorStops.data(), colorStops.size());
    shape->fill(gradient);
  }
  else if (shading->type == ShadingType::FreeFormTriangleMesh ||
           shading->type == ShadingType::LatticeFormTriangleMesh) {
    // Type 4: Free-form triangle mesh shading
    // Type 5: Lattice-form triangle mesh shading

#if NANOPDF_DEBUG_PRINT
    const char* type_name = (shading->type == ShadingType::FreeFormTriangleMesh) ?
                            "Type 4 (Free-form Triangle Mesh)" :
                            "Type 5 (Lattice-form Triangle Mesh)";
    printf("DEBUG: %s shading requested\n", type_name);
    printf("DEBUG: BitsPerCoordinate=%d, BitsPerComponent=%d, BitsPerFlag=%d\n",
           shading->bits_per_coordinate, shading->bits_per_component, shading->bits_per_flag);
    printf("DEBUG: Data stream size=%zu bytes\n", shading->data_stream.size());
#endif

    // Validate parameters
    if (shading->data_stream.empty() || shading->decode.size() < 4) {
#if NANOPDF_DEBUG_PRINT
      printf("DEBUG: Invalid mesh data - skipping\n");
#endif
      shape->unref();
      return false;
    }

    // Create a bitmap to rasterize the mesh
    int bmp_width = static_cast<int>(w);
    int bmp_height = static_cast<int>(h);
    if (bmp_width <= 0 || bmp_height <= 0 || bmp_width > 4096 || bmp_height > 4096) {
#if NANOPDF_DEBUG_PRINT
      printf("DEBUG: Invalid bitmap size - skipping\n");
#endif
      shape->unref();
      return false;
    }

    // Check render cache first
    uint32_t shading_obj = (shading_value.type == Value::REFERENCE) ? shading_value.ref_object_number : 0;
    std::string cache_key = "shading_mesh:" + std::to_string(shading_obj) + ":" +
                            std::to_string(static_cast<int>(shading->type)) + ":" +
                            std::to_string(bmp_width) + "x" + std::to_string(bmp_height);
    RenderCacheEntry cached;
    if (RenderCache::instance().find(cache_key, cached)) {
      auto picture = lvg::Picture::gen();
      if (picture) {
        if (picture->load(reinterpret_cast<const uint32_t*>(cached.data.data()),
                          bmp_width, bmp_height, lvg::ColorSpace::ARGB8888S, true) == lvg::Result::Success) {
          lvg::Matrix m;
          m.e11 = 1.0f; m.e12 = 0.0f; m.e13 = x;
          m.e21 = 0.0f; m.e22 = 1.0f; m.e23 = y;
          m.e31 = 0.0f; m.e32 = 0.0f; m.e33 = 1.0f;
          picture->transform(m);
          apply_soft_mask_opacity(picture);
          scene_->add(picture);
          shape->unref();
          return true;
        }
      }
    }

    std::vector<uint32_t> bitmap(bmp_width * bmp_height, 0x00000000);  // Transparent

    // Parse decode array
    float x_min = static_cast<float>(shading->decode[0]);
    float x_max = static_cast<float>(shading->decode[1]);
    float y_min = static_cast<float>(shading->decode[2]);
    float y_max = static_cast<float>(shading->decode[3]);

    // Color components (RGB for most color spaces)
    int num_components = 3;  // Assume RGB
    std::vector<float> color_min, color_max;
    for (size_t i = 4; i + 1 < shading->decode.size() && i < 4 + num_components * 2; i += 2) {
      color_min.push_back(static_cast<float>(shading->decode[i]));
      color_max.push_back(static_cast<float>(shading->decode[i + 1]));
    }

    // Setup bit stream
    BitStream bit_stream(shading->data_stream.data(), shading->data_stream.size());

    if (shading->type == ShadingType::FreeFormTriangleMesh) {
      // Type 4: Free-form triangle mesh with flags
      MeshVertex triangle[3];
      int vertex_count = 0;

      while (!bit_stream.is_eof()) {
        // Read flag
        uint32_t flag = 0;
        if (shading->bits_per_flag > 0 && bit_stream.bits_remaining() >= shading->bits_per_flag) {
          flag = bit_stream.get_bits(shading->bits_per_flag) & 0x03;
        } else {
          break;
        }

        // Read vertex
        if (bit_stream.bits_remaining() < shading->bits_per_coordinate * 2) break;
        uint32_t x_enc = bit_stream.get_bits(shading->bits_per_coordinate);
        uint32_t y_enc = bit_stream.get_bits(shading->bits_per_coordinate);

        MeshVertex vert;
        vert.x = x + decode_coord(x_enc, shading->bits_per_coordinate, x_min, x_max) * state_.scale;
        vert.y = y + (h - decode_coord(y_enc, shading->bits_per_coordinate, y_min, y_max)) * state_.scale;

        // Read color
        if (bit_stream.bits_remaining() < shading->bits_per_component * num_components) break;
        vert.r = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                  shading->bits_per_component,
                                  color_min.size() > 0 ? color_min[0] : 0.0f,
                                  color_max.size() > 0 ? color_max[0] : 1.0f);
        vert.g = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                  shading->bits_per_component,
                                  color_min.size() > 1 ? color_min[1] : 0.0f,
                                  color_max.size() > 1 ? color_max[1] : 1.0f);
        vert.b = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                  shading->bits_per_component,
                                  color_min.size() > 2 ? color_min[2] : 0.0f,
                                  color_max.size() > 2 ? color_max[2] : 1.0f);

        bit_stream.byte_align();

        // Build triangles based on flag
        if (flag == 0) {
          // Start new triangle
          triangle[0] = vert;
          vertex_count = 1;
        } else {
          // Continue triangle strip
          if (vertex_count < 3) {
            triangle[vertex_count++] = vert;
          }

          if (vertex_count == 3) {
            // Draw triangle
            draw_gouraud_triangle(bitmap.data(), bmp_width, bmp_height,
                                  triangle[0], triangle[1], triangle[2]);

            // Shift vertices for triangle strip
            if (flag == 1) {
              triangle[0] = triangle[1];
              triangle[1] = triangle[2];
            } else if (flag == 2) {
              triangle[1] = triangle[2];
            }
            triangle[2] = vert;
          }
        }
      }
    } else if (shading->type == ShadingType::LatticeFormTriangleMesh) {
      // Type 5: Lattice triangle mesh - requires VerticesPerRow
      // For now, skip as we'd need to parse the dictionary for this parameter
#if NANOPDF_DEBUG_PRINT
      printf("DEBUG: Lattice mesh - simplified rendering\n");
#endif
    }

    // Convert bitmap to LightVG Picture
    auto picture = lvg::Picture::gen();
    if (!picture) {
      shape->unref();
      return false;
    }

    // Convert BGRA to RGBA for ThorVG
    std::vector<uint32_t> rgba_data(bitmap.size());
    for (size_t i = 0; i < bitmap.size(); ++i) {
      uint32_t bgra = bitmap[i];
      uint32_t a = (bgra >> 24) & 0xFF;
      uint32_t r = bgra & 0xFF;
      uint32_t g = (bgra >> 8) & 0xFF;
      uint32_t b = (bgra >> 16) & 0xFF;
      rgba_data[i] = (a << 24) | (b << 16) | (g << 8) | r;
    }

    // Store in render cache
    RenderCacheEntry entry;
    entry.width = bmp_width;
    entry.height = bmp_height;
    entry.data.resize(rgba_data.size() * sizeof(uint32_t));
    std::copy(rgba_data.begin(), rgba_data.end(),
              reinterpret_cast<uint32_t*>(entry.data.data()));
    RenderCache::instance().store(cache_key, std::move(entry));

    // Load pixel data (ARGB8888S = un-premultiplied ARGB)
    if (picture->load(reinterpret_cast<uint32_t*>(rgba_data.data()),
                      bmp_width, bmp_height, lvg::ColorSpace::ARGB8888S, true) != lvg::Result::Success) {
      shape->unref();
      return false;
    }

    picture->translate(x, y);
    apply_soft_mask_opacity(picture);
    scene_->add(picture);
    shape->unref();  // Don't need shape wrapper
    return true;
  }
  else if (shading->type == ShadingType::CoonsPatchMesh ||
           shading->type == ShadingType::TensorProductPatchMesh) {
    // Type 6: Coons patch mesh shading (12 control points per patch)
    // Type 7: Tensor-product patch mesh shading (16 control points per patch)

    bool is_tensor = (shading->type == ShadingType::TensorProductPatchMesh);

#if NANOPDF_DEBUG_PRINT
    const char* type_name = is_tensor ? "Type 7 (Tensor-product Patch Mesh)" :
                                       "Type 6 (Coons Patch Mesh)";
    printf("DEBUG: %s shading requested\n", type_name);
    printf("DEBUG: BitsPerCoordinate=%d, BitsPerComponent=%d, BitsPerFlag=%d\n",
           shading->bits_per_coordinate, shading->bits_per_component, shading->bits_per_flag);
    printf("DEBUG: Data stream size=%zu bytes\n", shading->data_stream.size());
#endif

    // Validate parameters
    if (shading->data_stream.empty() || shading->decode.size() < 4) {
#if NANOPDF_DEBUG_PRINT
      printf("DEBUG: Invalid patch data - skipping\n");
#endif
      shape->unref();
      return false;
    }

    // Create a bitmap to rasterize patches
    int bmp_width = static_cast<int>(w);
    int bmp_height = static_cast<int>(h);
    if (bmp_width <= 0 || bmp_height <= 0 || bmp_width > 4096 || bmp_height > 4096) {
#if NANOPDF_DEBUG_PRINT
      printf("DEBUG: Invalid bitmap size - skipping\n");
#endif
      shape->unref();
      return false;
    }

    // Check render cache first
    uint32_t shading_obj = (shading_value.type == Value::REFERENCE) ? shading_value.ref_object_number : 0;
    std::string cache_key = "shading_patch:" + std::to_string(shading_obj) + ":" +
                            std::to_string(static_cast<int>(shading->type)) + ":" +
                            std::to_string(bmp_width) + "x" + std::to_string(bmp_height);
    RenderCacheEntry cached;
    if (RenderCache::instance().find(cache_key, cached)) {
      auto picture = lvg::Picture::gen();
      if (picture) {
        if (picture->load(reinterpret_cast<const uint32_t*>(cached.data.data()),
                          bmp_width, bmp_height, lvg::ColorSpace::ARGB8888S, true) == lvg::Result::Success) {
          lvg::Matrix m;
          m.e11 = 1.0f; m.e12 = 0.0f; m.e13 = x;
          m.e21 = 0.0f; m.e22 = 1.0f; m.e23 = y;
          m.e31 = 0.0f; m.e32 = 0.0f; m.e33 = 1.0f;
          picture->transform(m);
          apply_soft_mask_opacity(picture);
          scene_->add(picture);
          shape->unref();
          return true;
        }
      }
    }

    std::vector<uint32_t> bitmap(bmp_width * bmp_height, 0x00000000);  // Transparent

    // Parse decode array
    float x_min = static_cast<float>(shading->decode[0]);
    float x_max = static_cast<float>(shading->decode[1]);
    float y_min = static_cast<float>(shading->decode[2]);
    float y_max = static_cast<float>(shading->decode[3]);

    // Color components
    int num_components = 3;  // RGB
    std::vector<float> color_min, color_max;
    for (size_t i = 4; i + 1 < shading->decode.size() && i < 4 + num_components * 2; i += 2) {
      color_min.push_back(static_cast<float>(shading->decode[i]));
      color_max.push_back(static_cast<float>(shading->decode[i + 1]));
    }

    // Setup bit stream
    BitStream bit_stream(shading->data_stream.data(), shading->data_stream.size());

    // Track previous patch for edge sharing
    BezierPatch prev_patch;
    bool have_prev = false;

    while (!bit_stream.is_eof()) {
      // Read flag
      uint32_t flag = 0;
      if (shading->bits_per_flag > 0 && bit_stream.bits_remaining() >= shading->bits_per_flag) {
        flag = bit_stream.get_bits(shading->bits_per_flag) & 0x03;
      } else {
        break;
      }

      BezierPatch patch;

      // Number of control points to read depends on flag and type
      // flag=0: new patch, read all points
      // flag=1,2,3: shared edge, read fewer points
      int num_points_to_read = is_tensor ? 16 : 12;
      if (flag > 0 && have_prev) {
        // Shared edge - fewer points to read (simplified, full implementation complex)
        num_points_to_read = is_tensor ? 12 : 8;
      }

      // Read control points (simplified - Type 6/7 have complex point orderings)
      // For a proper implementation, need to handle Coons vs Tensor point layouts
      for (int i = 0; i < 4 && i < num_points_to_read / 4; ++i) {
        for (int j = 0; j < 4; ++j) {
          if (bit_stream.bits_remaining() < shading->bits_per_coordinate * 2) break;

          uint32_t x_enc = bit_stream.get_bits(shading->bits_per_coordinate);
          uint32_t y_enc = bit_stream.get_bits(shading->bits_per_coordinate);

          patch.points[i][j][0] = x + decode_coord(x_enc, shading->bits_per_coordinate,
                                                    x_min, x_max) * state_.scale;
          patch.points[i][j][1] = y + (h - decode_coord(y_enc, shading->bits_per_coordinate,
                                                         y_min, y_max)) * state_.scale;
        }
      }

      // Read 4 corner colors
      for (int corner = 0; corner < 4; ++corner) {
        if (bit_stream.bits_remaining() < shading->bits_per_component * num_components) break;

        patch.colors[corner][0] = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                                    shading->bits_per_component,
                                                    color_min.size() > 0 ? color_min[0] : 0.0f,
                                                    color_max.size() > 0 ? color_max[0] : 1.0f);
        patch.colors[corner][1] = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                                    shading->bits_per_component,
                                                    color_min.size() > 1 ? color_min[1] : 0.0f,
                                                    color_max.size() > 1 ? color_max[1] : 1.0f);
        patch.colors[corner][2] = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                                    shading->bits_per_component,
                                                    color_min.size() > 2 ? color_min[2] : 0.0f,
                                                    color_max.size() > 2 ? color_max[2] : 1.0f);
      }

      bit_stream.byte_align();

      // Draw patch using recursive subdivision
      draw_patch_recursive(bitmap.data(), bmp_width, bmp_height, patch);

      prev_patch = patch;
      have_prev = true;
    }

    // Convert bitmap to LightVG Picture
    auto picture = lvg::Picture::gen();
    if (!picture) {
      shape->unref();
      return false;
    }

    // Convert BGRA to RGBA
    std::vector<uint32_t> rgba_data(bitmap.size());
    for (size_t i = 0; i < bitmap.size(); ++i) {
      uint32_t bgra = bitmap[i];
      uint32_t a = (bgra >> 24) & 0xFF;
      uint32_t r = bgra & 0xFF;
      uint32_t g = (bgra >> 8) & 0xFF;
      uint32_t b = (bgra >> 16) & 0xFF;
      rgba_data[i] = (a << 24) | (b << 16) | (g << 8) | r;
    }

    // Store in render cache
    RenderCacheEntry entry;
    entry.width = bmp_width;
    entry.height = bmp_height;
    entry.data.resize(rgba_data.size() * sizeof(uint32_t));
    std::copy(rgba_data.begin(), rgba_data.end(),
              reinterpret_cast<uint32_t*>(entry.data.data()));
    RenderCache::instance().store(cache_key, std::move(entry));

    // Load pixel data (ARGB8888S = un-premultiplied ARGB)
    if (picture->load(reinterpret_cast<uint32_t*>(rgba_data.data()),
                      bmp_width, bmp_height, lvg::ColorSpace::ARGB8888S, true) != lvg::Result::Success) {
      shape->unref();
      return false;
    }

    picture->translate(x, y);
    apply_soft_mask_opacity(picture);
    scene_->add(picture);
    shape->unref();
    return true;
  }
  else {
    // Unsupported shading type - fill with gray
    shape->fill(128, 128, 128, 255);
  }

  return push_with_clip(shape);
}

bool LightVGBackend::parse_inline_image(const std::string& content, size_t& pos) {
  // Parse inline image dictionary (BI ... ID data EI)
  // pos should be right after "BI"

  std::map<std::string, std::string> dict;

  // Skip whitespace after BI
  while (pos < content.length() && std::isspace(static_cast<unsigned char>(content[pos]))) {
    pos++;
  }

  // Parse dictionary entries until we hit "ID"
  while (pos < content.length()) {
    // Skip whitespace
    while (pos < content.length() && std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
    if (pos >= content.length()) break;

    // Check for ID (marks start of image data)
    if (content[pos] == 'I' && pos + 1 < content.length() && content[pos + 1] == 'D') {
      pos += 2;  // Skip "ID"
      // Skip single whitespace after ID
      if (pos < content.length() && (content[pos] == ' ' || content[pos] == '\n' || content[pos] == '\r')) {
        pos++;
      }
      break;
    }

    // Parse key (should start with /)
    std::string key;
    if (content[pos] == '/') {
      pos++;  // Skip /
      while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos])) &&
             content[pos] != '/') {
        key += content[pos++];
      }
    } else {
      // Skip unknown token
      while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos]))) {
        pos++;
      }
      continue;
    }

    // Skip whitespace
    while (pos < content.length() && std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }

    // Parse value
    std::string value;
    if (pos < content.length()) {
      if (content[pos] == '/') {
        pos++;  // Skip /
        while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos])) &&
               content[pos] != '/') {
          value += content[pos++];
        }
      } else if (content[pos] == '[') {
        // Array value
        value += content[pos++];
        while (pos < content.length() && content[pos] != ']') {
          value += content[pos++];
        }
        if (pos < content.length()) value += content[pos++];
      } else {
        // Numeric or other value
        while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos])) &&
               content[pos] != '/') {
          value += content[pos++];
        }
      }
    }

    if (!key.empty()) {
      dict[key] = value;
    }
  }

  // Get image properties using PDF abbreviations
  int width = 0, height = 0, bpc = 8;
  std::string cs = "G";  // Default grayscale
  std::string filter;
  bool interpolate = false;

  // W or Width
  auto it = dict.find("W");
  if (it == dict.end()) it = dict.find("Width");
  if (it != dict.end()) width = nanopdf::stoi_or(it->second);

  // H or Height
  it = dict.find("H");
  if (it == dict.end()) it = dict.find("Height");
  if (it != dict.end()) height = nanopdf::stoi_or(it->second);

  // BPC or BitsPerComponent
  it = dict.find("BPC");
  if (it == dict.end()) it = dict.find("BitsPerComponent");
  if (it != dict.end()) bpc = nanopdf::stoi_or(it->second);

  // CS or ColorSpace
  it = dict.find("CS");
  if (it == dict.end()) it = dict.find("ColorSpace");
  if (it != dict.end()) cs = it->second;

  // F or Filter
  it = dict.find("F");
  if (it == dict.end()) it = dict.find("Filter");
  if (it != dict.end()) filter = it->second;

  // I or Interpolate
  it = dict.find("I");
  if (it == dict.end()) it = dict.find("Interpolate");
  if (it != dict.end()) {
    interpolate = (it->second == "true" || it->second == "True" ||
                   it->second == "1");
  }

  if (width <= 0 || height <= 0) {
    // Skip to EI and return
    while (pos < content.length()) {
      if (content[pos] == 'E' && pos + 1 < content.length() && content[pos + 1] == 'I') {
        pos += 2;
        return false;
      }
      pos++;
    }
    return false;
  }

  // Determine components based on color space
  int components = 1;
  if (cs == "RGB" || cs == "DeviceRGB") components = 3;
  else if (cs == "CMYK" || cs == "DeviceCMYK") components = 4;
  else if (cs == "G" || cs == "DeviceGray") components = 1;

  // Calculate expected data size
  int row_bytes = (width * components * bpc + 7) / 8;
  int expected_size = row_bytes * height;

  // Read raw image data until EI
  std::vector<uint8_t> raw_data;
  raw_data.reserve(expected_size);

  size_t data_start = pos;
  while (pos < content.length()) {
    // Look for EI preceded by whitespace
    if (pos > data_start &&
        (content[pos - 1] == ' ' || content[pos - 1] == '\n' || content[pos - 1] == '\r') &&
        content[pos] == 'E' && pos + 1 < content.length() && content[pos + 1] == 'I') {
      break;
    }
    raw_data.push_back(static_cast<uint8_t>(content[pos]));
    pos++;
  }

  // Skip EI
  if (pos < content.length() && content[pos] == 'E') {
    pos += 2;  // Skip "EI"
  }

  // Decode if filtered
  std::vector<uint8_t> decoded_data;
  if (filter == "AHx" || filter == "ASCIIHexDecode") {
    // ASCII Hex decode
    decoded_data.reserve(raw_data.size() / 2);
    for (size_t i = 0; i + 1 < raw_data.size(); i += 2) {
      char hex[3] = {static_cast<char>(raw_data[i]), static_cast<char>(raw_data[i + 1]), 0};
      decoded_data.push_back(static_cast<uint8_t>(strtol(hex, nullptr, 16)));
    }
  } else if (filter == "A85" || filter == "ASCII85Decode") {
    // ASCII85 decode
    decoded_data.reserve(raw_data.size() * 4 / 5);
    uint32_t value = 0;
    int count = 0;
    for (size_t i = 0; i < raw_data.size(); ++i) {
      uint8_t c = raw_data[i];
      // Skip whitespace
      if (c <= ' ') continue;
      // Handle 'z' (represents four zero bytes)
      if (c == 'z' && count == 0) {
        decoded_data.push_back(0);
        decoded_data.push_back(0);
        decoded_data.push_back(0);
        decoded_data.push_back(0);
        continue;
      }
      // Check for end marker ~>
      if (c == '~') break;
      // Validate character range
      if (c < '!' || c > 'u') continue;
      // Accumulate value
      value = value * 85 + (c - '!');
      count++;
      if (count == 5) {
        decoded_data.push_back((value >> 24) & 0xFF);
        decoded_data.push_back((value >> 16) & 0xFF);
        decoded_data.push_back((value >> 8) & 0xFF);
        decoded_data.push_back(value & 0xFF);
        value = 0;
        count = 0;
      }
    }
    // Handle remaining bytes (partial group)
    if (count > 0) {
      for (int j = count; j < 5; j++) {
        value = value * 85 + 84;  // Pad with 'u'
      }
      if (count > 1) decoded_data.push_back((value >> 24) & 0xFF);
      if (count > 2) decoded_data.push_back((value >> 16) & 0xFF);
      if (count > 3) decoded_data.push_back((value >> 8) & 0xFF);
    }
  } else if (filter == "Fl" || filter == "FlateDecode" || filter == "LZW" || filter == "LZWDecode") {
    // Need to decompress - use nanopdf's decode functions
    if (current_pdf_) {
      // Create a temporary stream value for decoding
      Value stream_val;
      stream_val.type = Value::STREAM;
      stream_val.stream.data = raw_data;
      if (filter == "Fl" || filter == "FlateDecode") {
        stream_val.stream.dict["Filter"] = Value();
        stream_val.stream.dict["Filter"].type = Value::NAME;
        stream_val.stream.dict["Filter"].name = "FlateDecode";
      } else {
        stream_val.stream.dict["Filter"] = Value();
        stream_val.stream.dict["Filter"].type = Value::NAME;
        stream_val.stream.dict["Filter"].name = "LZWDecode";
      }
      auto result = decode_stream(*current_pdf_, stream_val);
      if (result.success) {
        decoded_data = result.data;
      } else {
        decoded_data = raw_data;
      }
    } else {
      decoded_data = raw_data;
    }
  } else {
    // No filter or unknown, use raw data
    decoded_data = raw_data;
  }

  // Build ImageXObject
  ImageXObject image;
  image.width = width;
  image.height = height;
  image.bits_per_component = bpc;
  image.data = decoded_data;
  image.interpolate = interpolate;

  if (cs == "RGB" || cs == "DeviceRGB") {
    image.color_space.type = ColorSpaceType::DeviceRGB;
  } else if (cs == "CMYK" || cs == "DeviceCMYK") {
    image.color_space.type = ColorSpaceType::DeviceCMYK;
  } else {
    image.color_space.type = ColorSpaceType::DeviceGray;
  }

  draw_image(image, state_.transform.e, state_.transform.f,
             state_.transform.a, state_.transform.d,
             state_.fill_r, state_.fill_g, state_.fill_b,
             &state_.transform);

  return true;
}

bool LightVGBackend::apply_pattern_fill(lvg::Shape* shape, const std::string& pattern_name, bool is_stroke) {
  if (!shape || !current_pdf_ || !current_page_) {
    return false;
  }

  const Value* pattern_value = lookup_resource("Pattern", pattern_name);
  if (!pattern_value) {
    NANOPDF_LOG_DEBUG("LightVG", "Pattern '%s' not found", pattern_name.c_str());
    return false;
  }

  // The Value overload handles REFERENCE/DICTIONARY/STREAM triage and, for
  // Tiling patterns, decodes the stream body into pattern->tiling->content_stream.
  auto pattern = parse_pattern(*current_pdf_, *pattern_value, 0, 0);
  if (!pattern) {
    NANOPDF_LOG_DEBUG("LightVG", "Failed to parse pattern '%s'", pattern_name.c_str());
    return false;
  }

  NANOPDF_LOG_TRACE("LightVG", "Applying pattern '%s', type=%d", pattern_name.c_str(),
                    static_cast<int>(pattern->type));

  if (pattern->type == PatternType::Shading && pattern->shading) {
    // Shading pattern - apply as gradient fill
    auto shading = pattern->shading.get();

    if (shading->type == ShadingType::Axial && shading->coords.size() >= 4) {
      // Linear gradient
      auto gradient = lvg::LinearGradient::gen();
      if (!gradient) return false;

      // Apply pattern matrix if present
      float x0 = static_cast<float>(shading->coords[0]);
      float y0 = static_cast<float>(shading->coords[1]);
      float x1 = static_cast<float>(shading->coords[2]);
      float y1 = static_cast<float>(shading->coords[3]);

      // Transform coords through pattern matrix (if any)
      if (pattern->matrix.size() >= 6) {
        float a = static_cast<float>(pattern->matrix[0]);
        float b = static_cast<float>(pattern->matrix[1]);
        float c = static_cast<float>(pattern->matrix[2]);
        float d = static_cast<float>(pattern->matrix[3]);
        float e = static_cast<float>(pattern->matrix[4]);
        float f = static_cast<float>(pattern->matrix[5]);

        float new_x0 = a * x0 + c * y0 + e;
        float new_y0 = b * x0 + d * y0 + f;
        float new_x1 = a * x1 + c * y1 + e;
        float new_y1 = b * x1 + d * y1 + f;

        x0 = new_x0; y0 = new_y0;
        x1 = new_x1; y1 = new_y1;
      }

      // Apply page scale and Y-flip
      x0 *= state_.scale;
      y0 = (state_.page_height - y0) * state_.scale;
      x1 *= state_.scale;
      y1 = (state_.page_height - y1) * state_.scale;

      gradient->linear(x0, y0, x1, y1);

      // Extract color stops
      auto colorStops = extract_color_stops_from_function(*current_pdf_, shading->function, &color_stop_cache_);
      gradient->colorStops(colorStops.data(), colorStops.size());

      if (shading->extend.size() >= 2 && (shading->extend[0] || shading->extend[1])) {
        gradient->spread(lvg::FillSpread::Pad);
      }

      if (is_stroke) {
        shape->strokeFill(gradient);
      } else {
        shape->fill(gradient);
      }
      return true;
    }
    else if (shading->type == ShadingType::Radial && shading->coords.size() >= 6) {
      // Radial gradient
      auto gradient = lvg::RadialGradient::gen();
      if (!gradient) return false;

      float x0 = static_cast<float>(shading->coords[0]);
      float y0 = static_cast<float>(shading->coords[1]);
      float r0 = static_cast<float>(shading->coords[2]);
      float x1 = static_cast<float>(shading->coords[3]);
      float y1 = static_cast<float>(shading->coords[4]);
      float r1 = static_cast<float>(shading->coords[5]);

      // Transform through pattern matrix
      if (pattern->matrix.size() >= 6) {
        float a = static_cast<float>(pattern->matrix[0]);
        float b = static_cast<float>(pattern->matrix[1]);
        float c = static_cast<float>(pattern->matrix[2]);
        float d = static_cast<float>(pattern->matrix[3]);
        float e = static_cast<float>(pattern->matrix[4]);
        float f = static_cast<float>(pattern->matrix[5]);

        float new_x0 = a * x0 + c * y0 + e;
        float new_y0 = b * x0 + d * y0 + f;
        float new_x1 = a * x1 + c * y1 + e;
        float new_y1 = b * x1 + d * y1 + f;

        // Scale radius by average scale factor
        float scale_factor = std::sqrt(std::abs(a * d - b * c));
        r0 *= scale_factor;
        r1 *= scale_factor;

        x0 = new_x0; y0 = new_y0;
        x1 = new_x1; y1 = new_y1;
      }

      // Apply page scale and Y-flip
      x0 *= state_.scale;
      y0 = (state_.page_height - y0) * state_.scale;
      r0 *= state_.scale;
      x1 *= state_.scale;
      y1 = (state_.page_height - y1) * state_.scale;
      r1 *= state_.scale;

      gradient->radial(x1, y1, r1, x0, y0, r0);

      auto colorStops = extract_color_stops_from_function(*current_pdf_, shading->function, &color_stop_cache_);
      gradient->colorStops(colorStops.data(), colorStops.size());

      if (shading->extend.size() >= 2 && (shading->extend[0] || shading->extend[1])) {
        gradient->spread(lvg::FillSpread::Pad);
      }

      if (is_stroke) {
        shape->strokeFill(gradient);
      } else {
        shape->fill(gradient);
      }
      return true;
    }
  }
  else if (pattern->type == PatternType::Tiling && pattern->tiling) {
    // Tiling pattern - render tile and repeat
    return apply_tiling_pattern(shape, pattern->tiling.get(), pattern->matrix, is_stroke);
  }

  return false;
}

bool LightVGBackend::apply_tiling_pattern(lvg::Shape* shape, const TilingPattern* tiling,
                                          const std::vector<double>& matrix, bool is_stroke) {
  if (!shape || !tiling || !current_pdf_) {
    return false;
  }

#if NANOPDF_DEBUG_PRINT
  printf("DEBUG: Tiling pattern requested: paint_type=%d, bbox=[%g,%g,%g,%g], x_step=%g, y_step=%g\n",
         static_cast<int>(tiling->paint_type),
         tiling->bbox.size() >= 4 ? tiling->bbox[0] : 0,
         tiling->bbox.size() >= 4 ? tiling->bbox[1] : 0,
         tiling->bbox.size() >= 4 ? tiling->bbox[2] : 0,
         tiling->bbox.size() >= 4 ? tiling->bbox[3] : 0,
         tiling->x_step, tiling->y_step);
#endif

  // Get tile dimensions from BBox
  if (tiling->bbox.size() < 4 || tiling->content_stream.empty()) {
    // No content to render - use fallback color
    goto fallback;
  }

  {
    float tile_width = static_cast<float>(tiling->bbox[2] - tiling->bbox[0]);
    float tile_height = static_cast<float>(tiling->bbox[3] - tiling->bbox[1]);

    if (tile_width <= 0 || tile_height <= 0) {
      goto fallback;
    }

    // Pattern matrix maps pattern-cell coordinates to user space:
    //   user_x = a*pcx + c*pcy + e
    //   user_y = b*pcx + d*pcy + f
    // Decompose into per-axis scale magnitudes for tile resolution, and
    // separately carry (a,b,c,d) so rotated/sheared matrices place tiles
    // along the pattern's natural axes rather than along screen axes.
    float pm_a = 1.0f, pm_b = 0.0f, pm_c = 0.0f, pm_d = 1.0f;
    if (matrix.size() >= 6) {
      pm_a = static_cast<float>(matrix[0]);
      pm_b = static_cast<float>(matrix[1]);
      pm_c = static_cast<float>(matrix[2]);
      pm_d = static_cast<float>(matrix[3]);
    }
    float mat_sx = std::sqrt(pm_a * pm_a + pm_b * pm_b);
    float mat_sy = std::sqrt(pm_c * pm_c + pm_d * pm_d);
    if (mat_sx <= 0.0f) mat_sx = 1.0f;
    if (mat_sy <= 0.0f) mat_sy = 1.0f;
    const bool matrix_rotated =
        (std::abs(pm_b) > 1e-4f * mat_sx) ||
        (std::abs(pm_c) > 1e-4f * mat_sy);

    // Effective on-page tile size combines pattern matrix scale and page scale.
    float pattern_scale_x = state_.scale * mat_sx;
    float pattern_scale_y = state_.scale * mat_sy;

    // Render at matrix/page-derived scale and clamp only by global safety caps.
    float internal_scale_x = pattern_scale_x;
    float internal_scale_y = pattern_scale_y;
    int tile_px_w = static_cast<int>(std::ceil(tile_width * internal_scale_x));
    int tile_px_h = static_cast<int>(std::ceil(tile_height * internal_scale_y));

    // Cap tile buffer dimensions; when the natural size is too large we must
    // also drop internal_scale so the tile content still spans the full bbox
    // (otherwise rendering clips to the buffer and the on-canvas tile shows
    // only a fraction of the pattern cell).
    const int kMaxTileDim = 2048;
    if (tile_px_w > kMaxTileDim) {
      internal_scale_x = static_cast<float>(kMaxTileDim) / tile_width;
      tile_px_w = kMaxTileDim;
    }
    if (tile_px_h > kMaxTileDim) {
      internal_scale_y = static_cast<float>(kMaxTileDim) / tile_height;
      tile_px_h = kMaxTileDim;
    }
    if (tile_px_w < 1) tile_px_w = 1;
    if (tile_px_h < 1) tile_px_h = 1;

    // Try render cache first — same (content, dimensions, paint_type) produces
    // identical tile pixels regardless of which page or fill shape uses it.
    uint64_t content_hash = fnv1a64(tiling->content_stream.data(),
                                    tiling->content_stream.size());
    int paint_type_int = static_cast<int>(tiling->paint_type);
    // Include pattern pointer so different TilingPattern objects with identical
    // content but different resources get separate cache entries.
    uintptr_t ptr_key = reinterpret_cast<uintptr_t>(tiling);
    std::string cache_key = "tile:" + std::to_string(ptr_key) + ":" +
                            std::to_string(content_hash) + ":" +
                            std::to_string(tile_px_w) + "x" +
                            std::to_string(tile_px_h) + ":" +
                            std::to_string(paint_type_int);

    std::vector<uint32_t> rendered_tile;
    RenderCacheEntry cached;
    if (RenderCache::instance().find(cache_key, cached) &&
        cached.width == static_cast<uint32_t>(tile_px_w) &&
        cached.height == static_cast<uint32_t>(tile_px_h)) {
      // Cache hit — reconstruct pixel buffer from cached bytes.
      size_t n = static_cast<size_t>(tile_px_w) * static_cast<size_t>(tile_px_h);
      rendered_tile.resize(n);
      memcpy(rendered_tile.data(), cached.data.data(), n * sizeof(uint32_t));
    } else {
      // Cache miss — render the tile sub-scene.
      std::vector<uint32_t> tile_buffer(tile_px_w * tile_px_h, 0);

      // Initialize with transparent (for colored) or white (for uncolored)
      if (tiling->paint_type == TilingPaintType::ColoredTiles) {
        std::fill(tile_buffer.begin(), tile_buffer.end(), 0);
      } else {
        std::fill(tile_buffer.begin(), tile_buffer.end(), 0xFFFFFFFF);
      }

      lvg::SwCanvas* tile_canvas = lvg::SwCanvas::gen();
      if (!tile_canvas) {
        goto fallback;
      }

      if (tile_canvas->target(tile_buffer.data(),
                              tile_px_w, tile_px_w, tile_px_h,
                              lvg::ColorSpace::ABGR8888) != lvg::Result::Success) {
        delete tile_canvas;
        goto fallback;
      }

      lvg::Scene* tile_scene = lvg::Scene::gen();
      if (!tile_scene) {
        delete tile_canvas;
        goto fallback;
      }

      // Save current state
      lvg::SwCanvas* saved_canvas = canvas_;
      lvg::Scene* saved_scene = scene_;
      std::vector<uint32_t> saved_buffer = std::move(buffer_);
      uint32_t saved_width = width_;
      uint32_t saved_height = height_;
      GraphicsState saved_state = state_;

      // Switch to tile canvas
      canvas_ = tile_canvas;
      scene_ = tile_scene;
      buffer_ = std::move(tile_buffer);
      width_ = tile_px_w;
      height_ = tile_px_h;

      // Reset graphics state for tile rendering. Use the geometric mean of
      // per-axis scales so the pattern cell maps to the tile buffer without
      // having to handle per-axis scaling in the inner content renderer.
      state_ = GraphicsState();
      state_.page_width = tile_width;
      state_.page_height = tile_height;
      // Tile canvas scale matches the internal (oversampled) resolution so that
      // pattern content drawn inside has proper detail. When placed on the main
      // canvas the tile is downsampled via Picture transform.
      state_.scale = std::sqrt(internal_scale_x * internal_scale_y);

      // Push pattern resources if available
      if (!tiling->resources.empty()) {
        form_resources_stack_.push_back(tiling->resources);
      }

      // Parse and render the tile content
      bool progress_enabled = progress_.enabled;
      progress_.enabled = false;
      parse_pdf_content(tiling->content_stream);
      progress_.enabled = progress_enabled;

      // Pop pattern resources
      if (!tiling->resources.empty() && !form_resources_stack_.empty()) {
        form_resources_stack_.pop_back();
      }

      // Finalize tile rendering
      if (tile_canvas->add(tile_scene) == lvg::Result::Success) {
        tile_canvas->draw(true);
        tile_canvas->sync();
      }

      // Get the rendered tile pixels
      rendered_tile = std::move(buffer_);

      // Clean up tile canvas
      delete tile_canvas;

      // Restore original state
      canvas_ = saved_canvas;
      scene_ = saved_scene;
      buffer_ = std::move(saved_buffer);
      width_ = saved_width;
      height_ = saved_height;
      state_ = saved_state;

      // Store rendered tile in render cache for reuse across pages/shapes.
      if (!rendered_tile.empty()) {
        size_t px = static_cast<size_t>(tile_px_w) * static_cast<size_t>(tile_px_h);
        RenderCacheEntry entry;
        entry.width = static_cast<uint32_t>(tile_px_w);
        entry.height = static_cast<uint32_t>(tile_px_h);
        entry.data.resize(px * sizeof(uint32_t));
        memcpy(entry.data.data(), rendered_tile.data(), px * sizeof(uint32_t));
        RenderCache::instance().store(cache_key, std::move(entry));
      }
    }

    // Now create a tiled bitmap covering the target area
    // Get shape bounds
    float shape_x, shape_y, shape_w, shape_h;
    shape->bounds(&shape_x, &shape_y, &shape_w, &shape_h);

    // Step in canvas (final) space and in tile-internal space. The internal
    // step spans the tile's oversampled pixel dimensions; we compose the
    // tiled bitmap at that resolution and let ThorVG downsample during
    // Picture placement so sub-pixel features stay visible.
    float step_x = static_cast<float>(tiling->x_step) * pattern_scale_x;
    float step_y = static_cast<float>(tiling->y_step) * pattern_scale_y;
    if (step_x <= 0) step_x = tile_width * pattern_scale_x;
    if (step_y <= 0) step_y = tile_height * pattern_scale_y;
    float step_ix = static_cast<float>(tiling->x_step) * internal_scale_x;
    float step_iy = static_cast<float>(tiling->y_step) * internal_scale_y;
    if (step_ix <= 0) step_ix = tile_px_w;
    if (step_iy <= 0) step_iy = tile_px_h;

    int tiles_x = static_cast<int>(std::ceil(shape_w / step_x)) + 2;
    int tiles_y = static_cast<int>(std::ceil(shape_h / step_y)) + 2;

    // Limit total tiles. Patterns with small matrix scaling can legitimately
    // need many tiles across a large shape (e.g. 12px tile on a 400px region).
    tiles_x = clamp14(tiles_x, 1, 500);
    tiles_y = clamp14(tiles_y, 1, 500);

    // Compose at internal resolution; final Picture transform rescales.
    // Use ceil so fractional steps don't clip the last tile.
    int tiled_w = static_cast<int>(std::ceil(tiles_x * step_ix));
    int tiled_h = static_cast<int>(std::ceil(tiles_y * step_iy));
    if (tiled_w < 1) tiled_w = 1;
    if (tiled_h < 1) tiled_h = 1;

    // Create the tiled bitmap
    std::vector<uint32_t> tiled_pixels(tiled_w * tiled_h);

    // Get color for uncolored patterns
    uint8_t pattern_r = state_.fill_r;
    uint8_t pattern_g = state_.fill_g;
    uint8_t pattern_b = state_.fill_b;
    uint8_t pattern_a = static_cast<uint8_t>(state_.fill_opacity * 255);
    if (is_stroke) {
      pattern_r = state_.stroke_r;
      pattern_g = state_.stroke_g;
      pattern_b = state_.stroke_b;
      pattern_a = static_cast<uint8_t>(state_.stroke_opacity * 255);
    }

    // Rotated / sheared pattern matrix: tiles are placed along the (a,b)
    // and (c,d) pattern axes rather than along screen axes. We emit one
    // LightVG Picture per tile instance, each with its own rotation transform,
    // and let the caller-applied clip trim off tiles that spill outside the
    // fill shape. This branch preserves proper tile alignment for patterns
    // with non-zero skew (b or c); the flat-bitmap path below cannot.
    if (matrix_rotated) {
      // Convert pattern tile bitmap to a reusable RGBA buffer for Picture.
      std::vector<uint32_t> tile_rgba(tile_px_w * tile_px_h);
      for (int py = 0; py < tile_px_h; ++py) {
        for (int px = 0; px < tile_px_w; ++px) {
          // Flip rows for the same reason the flat path does (ThorVG tile
          // render is vertically inverted relative to page space).
          size_t src_idx = static_cast<size_t>(tile_px_h - 1 - py) * tile_px_w + px;
          uint32_t pixel = rendered_tile[src_idx];
          uint8_t b_val = static_cast<uint8_t>(pixel & 0xFF);
          uint8_t g_val = static_cast<uint8_t>((pixel >> 8) & 0xFF);
          uint8_t r_val = static_cast<uint8_t>((pixel >> 16) & 0xFF);
          uint8_t a_val = static_cast<uint8_t>((pixel >> 24) & 0xFF);
          if (tiling->paint_type == TilingPaintType::UncoloredTiles) {
            uint8_t intensity =
                static_cast<uint8_t>(255 - ((r_val + g_val + b_val) / 3));
            a_val = static_cast<uint8_t>((intensity * pattern_a) / 255);
            r_val = pattern_r;
            g_val = pattern_g;
            b_val = pattern_b;
          }
          tile_rgba[py * tile_px_w + px] =
              (static_cast<uint32_t>(a_val) << 24) |
              (static_cast<uint32_t>(r_val) << 16) |
              (static_cast<uint32_t>(g_val) << 8) | b_val;
        }
      }

      // Step vectors in canvas space. Canvas y is (page_height - user_y) *
      // scale, so the y component of a user-space delta flips sign.
      const float cs = state_.scale;
      const float step_u_cx = pm_a * static_cast<float>(tiling->x_step) * cs;
      const float step_u_cy = -pm_b * static_cast<float>(tiling->x_step) * cs;
      const float step_v_cx = pm_c * static_cast<float>(tiling->y_step) * cs;
      const float step_v_cy = -pm_d * static_cast<float>(tiling->y_step) * cs;

      // Rotation of the pattern x-axis in canvas. The scale part is already
      // baked into the tile bitmap dimensions (tile_px_w/h use mat_sx/mat_sy),
      // so we only need pure rotation from (pm_a, -pm_b).
      const float mag_u = std::sqrt(pm_a * pm_a + pm_b * pm_b);
      const float cos_a = mag_u > 0 ? pm_a / mag_u : 1.0f;
      const float sin_a = mag_u > 0 ? -pm_b / mag_u : 0.0f;

      // Determine an (i, j) range that safely covers the shape bbox. Invert
      // the step vectors (treating them as a 2x2 basis) to map each shape
      // corner into pattern-index space, then take the inclusive range.
      const float det = step_u_cx * step_v_cy - step_u_cy * step_v_cx;
      int i_min = -1, i_max = 1, j_min = -1, j_max = 1;
      if (std::abs(det) > 1e-6f) {
        auto inv_map = [&](float dx, float dy, float& oi, float& oj) {
          oi = (dx * step_v_cy - dy * step_v_cx) / det;
          oj = (-dx * step_u_cy + dy * step_u_cx) / det;
        };
        float corners_i[4], corners_j[4];
        const float cx0 = shape_x, cy0 = shape_y;
        const float cx1 = shape_x + shape_w, cy1 = shape_y + shape_h;
        inv_map(cx0 - shape_x, cy0 - shape_y, corners_i[0], corners_j[0]);
        inv_map(cx1 - shape_x, cy0 - shape_y, corners_i[1], corners_j[1]);
        inv_map(cx0 - shape_x, cy1 - shape_y, corners_i[2], corners_j[2]);
        inv_map(cx1 - shape_x, cy1 - shape_y, corners_i[3], corners_j[3]);
        float lo_i = corners_i[0], hi_i = corners_i[0];
        float lo_j = corners_j[0], hi_j = corners_j[0];
        for (int k = 1; k < 4; ++k) {
          lo_i = std::min(lo_i, corners_i[k]);
          hi_i = std::max(hi_i, corners_i[k]);
          lo_j = std::min(lo_j, corners_j[k]);
          hi_j = std::max(hi_j, corners_j[k]);
        }
        i_min = static_cast<int>(std::floor(lo_i)) - 1;
        i_max = static_cast<int>(std::ceil(hi_i)) + 1;
        j_min = static_cast<int>(std::floor(lo_j)) - 1;
        j_max = static_cast<int>(std::ceil(hi_j)) + 1;
      }

      // Cap total tiles to keep pathological patterns bounded.
      const int kMaxTiles = 4000;
      long long total =
          static_cast<long long>(i_max - i_min + 1) * (j_max - j_min + 1);
      if (total > kMaxTiles) {
        // Fall through to flat path rather than spending unbounded time here.
        goto flat_tile_path;
      }

      // Shape-space clip rectangle for the Picture group.
      lvg::Shape* clip_shape = lvg::Shape::gen();
      if (clip_shape) {
        clip_shape->appendRect(shape_x, shape_y, shape_w, shape_h, 0, 0);
      }

      lvg::Scene* tiles_scene = lvg::Scene::gen();
      if (!tiles_scene) {
        goto fallback;
      }

      for (int j = j_min; j <= j_max; ++j) {
        for (int i = i_min; i <= i_max; ++i) {
          float cx = shape_x + i * step_u_cx + j * step_v_cx;
          float cy = shape_y + i * step_u_cy + j * step_v_cy;
          // Conservative cull against shape bbox (allow one tile margin).
          if (cx + tile_px_w < shape_x - tile_px_w ||
              cy + tile_px_h < shape_y - tile_px_h ||
              cx > shape_x + shape_w + tile_px_w ||
              cy > shape_y + shape_h + tile_px_h) {
            continue;
          }
          auto picture = lvg::Picture::gen();
          if (!picture) continue;
          if (picture->load(tile_rgba.data(), tile_px_w, tile_px_h,
                            lvg::ColorSpace::ARGB8888, true) !=
              lvg::Result::Success) {
            continue;
          }
          // Transform local tile pixels by a pure rotation about the origin,
          // then translate to the target canvas position.
          lvg::Matrix m;
          m.e11 = cos_a;  m.e12 = -sin_a; m.e13 = cx;
          m.e21 = sin_a;  m.e22 = cos_a;  m.e23 = cy;
          m.e31 = 0.0f;   m.e32 = 0.0f;   m.e33 = 1.0f;
          picture->transform(m);
          tiles_scene->add(picture);
        }
      }

      if (clip_shape) {
        tiles_scene->clip(clip_shape);
      }
      apply_soft_mask_opacity(tiles_scene);
      scene_->add(tiles_scene);
      return true;
    }

  flat_tile_path:
    // Tile the pattern (axis-aligned fast path). Offsets use the internal
    // (oversampled) step so tile content preserves sub-pixel detail; a final
    // scale transform downsamples during Picture placement.
    //
    // Accumulate tile positions as floats and fill any sub-pixel gaps between
    // adjacent tiles by replicating the edge pixel, eliminating visible seams.
    {
      float oy_f = 0.0f;
      auto process_tile_pixel = [&](uint32_t raw_pixel, uint8_t& r, uint8_t& g,
                                      uint8_t& b, uint8_t& a) {
        r = static_cast<uint8_t>((raw_pixel >> 16) & 0xFF);
        g = static_cast<uint8_t>((raw_pixel >> 8) & 0xFF);
        b = static_cast<uint8_t>(raw_pixel & 0xFF);
        a = static_cast<uint8_t>((raw_pixel >> 24) & 0xFF);
        if (tiling->paint_type == TilingPaintType::UncoloredTiles) {
          uint8_t intensity = static_cast<uint8_t>((255 - ((r + g + b) / 3)));
          a = static_cast<uint8_t>((intensity * pattern_a) / 255);
          r = pattern_r; g = pattern_g; b = pattern_b;
        }
      };

      for (int ty = 0; ty < tiles_y; ++ty) {
        int oy = static_cast<int>(oy_f);
        float ox_f = 0.0f;
        for (int tx = 0; tx < tiles_x; ++tx) {
          int ox = static_cast<int>(ox_f);

          // Copy tile pixels into the tiled bitmap.
          for (int py = 0; py < tile_px_h && oy + py < tiled_h; ++py) {
            for (int px = 0; px < tile_px_w && ox + px < tiled_w; ++px) {
              // Tile render is vertically inverted relative to page space;
              // flip rows to preserve the source PDF pattern orientation.
              size_t src_idx =
                  static_cast<size_t>(tile_px_h - 1 - py) * tile_px_w + px;
              size_t dst_idx =
                  static_cast<size_t>(oy + py) * tiled_w + (ox + px);

              uint32_t pixel = rendered_tile[src_idx];
              uint8_t rv, gv, bv, av;
              process_tile_pixel(pixel, rv, gv, bv, av);
              tiled_pixels[dst_idx] =
                  (static_cast<uint32_t>(av) << 24) |
                  (static_cast<uint32_t>(rv) << 16) |
                  (static_cast<uint32_t>(gv) << 8) | bv;
            }
          }

          // Fill sub-pixel gap to next tile by replicating the edge column.
          float next_ox_f = ox_f + step_ix;
          int next_ox = static_cast<int>(next_ox_f);
          int gap_start = ox + tile_px_w;
          if (next_ox > gap_start) {
            int fill_end = (next_ox < tiled_w) ? next_ox : tiled_w;
            for (int gx = gap_start; gx < fill_end; ++gx) {
              for (int gy = 0; gy < tile_px_h && oy + gy < tiled_h; ++gy) {
                size_t src_idx =
                    static_cast<size_t>(tile_px_h - 1 - gy) * tile_px_w +
                    (tile_px_w - 1);
                size_t dst_idx = static_cast<size_t>(oy + gy) * tiled_w + gx;
                uint8_t rv, gv, bv, av;
                process_tile_pixel(rendered_tile[src_idx], rv, gv, bv, av);
                tiled_pixels[dst_idx] =
                    (static_cast<uint32_t>(av) << 24) |
                    (static_cast<uint32_t>(rv) << 16) |
                    (static_cast<uint32_t>(gv) << 8) | bv;
              }
            }
          }

          ox_f = next_ox_f;
        }
        oy_f += step_iy;
      }
    }

    // Create LightVG Picture from tiled bitmap
    auto picture = lvg::Picture::gen();
    if (picture) {
      auto result = picture->load(tiled_pixels.data(), tiled_w, tiled_h,
                                   lvg::ColorSpace::ARGB8888, true);
      if (result == lvg::Result::Success) {
        // Position at shape origin and downsample from internal to final scale.
        // The tiled bitmap is composed at internal_scale_x/y per pattern unit;
        // the final on-canvas footprint wants pattern_scale_x/y per unit, so
        // we apply the ratio as a scale transform. ThorVG handles filtering.
        float dsx = (internal_scale_x > 0) ? pattern_scale_x / internal_scale_x : 1.0f;
        float dsy = (internal_scale_y > 0) ? pattern_scale_y / internal_scale_y : 1.0f;
        lvg::Matrix m;
        m.e11 = dsx;  m.e12 = 0.0f; m.e13 = shape_x;
        m.e21 = 0.0f; m.e22 = dsy;  m.e23 = shape_y;
        m.e31 = 0.0f; m.e32 = 0.0f; m.e33 = 1.0f;
        picture->transform(m);

        // Clip to shape using the clip() method
        // Note: In newer ThorVG, clip() takes ownership of the shape
        lvg::Shape* clip_shape = lvg::Shape::gen();
        if (clip_shape) {
          // Copy path from original shape
          float sx, sy, sw, sh;
          shape->bounds(&sx, &sy, &sw, &sh);
          clip_shape->appendRect(sx, sy, sw, sh, 0, 0);
          picture->clip(clip_shape);
        }

        apply_soft_mask_opacity(picture);
        scene_->add(picture);
        return true;
      }
    }
  }

fallback:
  // Fallback: use solid color based on paint_type
  uint8_t r, g, b, a;

  if (tiling->paint_type == TilingPaintType::UncoloredTiles) {
    if (is_stroke) {
      r = state_.stroke_r;
      g = state_.stroke_g;
      b = state_.stroke_b;
      a = static_cast<uint8_t>(state_.stroke_opacity * 255);
    } else {
      r = state_.fill_r;
      g = state_.fill_g;
      b = state_.fill_b;
      a = static_cast<uint8_t>(state_.fill_opacity * 255);
    }
  } else {
    // Colored pattern placeholder
    r = 200;
    g = 150;
    b = 220;
    a = 255;
  }

  if (is_stroke) {
    shape->strokeFill(r, g, b, a);
    float stroke_width = state_.stroke_width * state_.scale;
    if (state_.stroke_width == 0.0f) stroke_width = 1.0f;
    shape->strokeWidth(stroke_width);
  } else {
    shape->fill(r, g, b, a);
  }

  return true;
}

bool LightVGBackend::draw_missing_glyph_placeholder(float x, float y, float size,
                                                    uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  auto shape = lvg::Shape::gen();
  if (!shape) return false;
  float glyph_width = size * 0.5f;
  float box_margin = size * 0.05f;
  shape->appendRect(x + box_margin, y - size + box_margin,
                    glyph_width - 2 * box_margin, size - 2 * box_margin, 0, 0);
  shape->fill(0, 0, 0, 0);  // transparent fill
  shape->strokeFill(r, g, b, static_cast<uint8_t>(a * 0.4f));
  shape->strokeWidth(size * 0.04f);  // thin stroke relative to font size
  push_with_clip(shape);
  return true;
}

// Try to render a missing glyph from fallback system fonts (e.g. for Unicode
// codepoints that the PDF-embedded font does not cover). Iterates over font
// categories registered in the FontProvider singleton, loads the first match,
// and delegates to draw_glyph().
bool LightVGBackend::try_draw_glyph_fallback(int codepoint, float x, float y,
                                             float size,
                                             uint8_t r, uint8_t g,
                                             uint8_t b, uint8_t a) {
  // Fallback font names by priority (well-known system / embedded fonts).
  // The first font in font_cache_ that has the codepoint wins.
  static const char* kFallbackNames[] = {
      "NotoSerifJP", "NotoSansJP",      // CJK (embedded if NANOPDF_EMBED_CJK_FONTS)
      "NotoSerif",   "NotoSans",         // Serif / Sans
      "Arimo",       "Tinos",            // Embedded fallbacks
      "Cousine",                          // Monospace
      "DejaVu Sans", "DejaVu Serif",
      "Liberation Sans", "Liberation Serif",
      "FreeSerif",   "FreeSans",
  };

  if (in_glyph_fallback_) return false;  // never recurse into another fallback
  in_glyph_fallback_ = true;
  std::string saved_font = current_font_name_;
  bool drawn = false;

  for (const char* fb_name : kFallbackNames) {
    // Skip if this is the same as the current font
    if (fb_name == saved_font) continue;

    // Use the cached font, or try to load it as a fallback.
    FontCache* fc = get_font(fb_name);
    if (!fc) {
      if (!load_fallback_font(fb_name)) continue;
      fc = get_font(fb_name);
      if (!fc) continue;
    }

    int gid = 0;
    if (fc->has_ttf_parse) {
      gid = ttf_cmap_lookup(&fc->ttf, static_cast<uint32_t>(codepoint));
    }
    if (gid == 0 && fc->initialized) {
      gid = stbtt_FindGlyphIndex(&fc->font_info, codepoint);
    }
    if (gid != 0) {
      current_font_name_ = fb_name;
      bool ok = draw_glyph(codepoint, x, y, size, r, g, b, a);
      current_font_name_ = saved_font;
      if (ok) { drawn = true; break; }
    }
  }

  current_font_name_ = saved_font;
  in_glyph_fallback_ = false;
  return drawn;
}

bool LightVGBackend::draw_glyph(int codepoint, float x, float y, float size,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }
  uint32_t raw_codepoint = static_cast<uint32_t>(codepoint);
  if (raw_codepoint > 0x10FFFFu) {
    uint32_t hi = (raw_codepoint >> 16) & 0xFFFFu;
    uint32_t lo = raw_codepoint & 0xFFFFu;
    bool surrogate_pair = hi >= 0xD800u && hi <= 0xDBFFu &&
                          lo >= 0xDC00u && lo <= 0xDFFFu;
    bool known_ligature = hi == 'f' && (lo == 'f' || lo == 'i' || lo == 'l');
    if (!surrogate_pair && !known_ligature &&
        hi >= 0x20u && hi <= 0x7Eu && lo >= 0x20u && lo <= 0x7Eu) {
      bool a_ok = draw_glyph(static_cast<int>(hi), x, y, size, r, g, b, a);
      bool b_ok = draw_glyph(static_cast<int>(lo), x + size * 0.52f, y, size, r, g, b, a);
      return a_ok || b_ok;
    }
  }
  codepoint = static_cast<int>(
      normalize_render_codepoint(static_cast<uint32_t>(codepoint)));

  auto draw_ligature_fallback = [&]() -> bool {
    const char* letters = nullptr;
    switch (codepoint) {
      case 0xFB00: letters = "ff"; break;
      case 0xFB01: letters = "fi"; break;
      case 0xFB02: letters = "fl"; break;
      case 0xFB03: letters = "ffi"; break;
      case 0xFB04: letters = "ffl"; break;
      default: break;
    }
    if (!letters) return false;
    bool drew_any = false;
    float dx = 0.0f;
    for (const char* p = letters; *p; ++p) {
      if (draw_glyph(static_cast<int>(*p), x + dx, y, size, r, g, b, a)) {
        drew_any = true;
      }
      dx += size * 0.28f;
    }
    return drew_any;
  };

  auto draw_bullet_fallback = [&]() -> bool {
    if (codepoint != 0x2022 && codepoint != 0x30FB) return false;
    float cx = x + size * 0.22f;
    float cy = y - size * 0.34f;
    float radius = std::max(0.8f, size * 0.06f);
    return draw_circle(cx, cy, radius, r, g, b, a);
  };
  auto draw_checkmark_fallback = [&]() -> bool {
    if (codepoint != 0x2713 && codepoint != 0x2714) return false;
    auto shape = lvg::Shape::gen();
    if (!shape) return false;
    float x0 = x + size * 0.10f;
    float y0 = y - size * 0.43f;
    float x1 = x + size * 0.22f;
    float y1 = y - size * 0.28f;
    float x2 = x + size * 0.46f;
    float y2 = y - size * 0.62f;
    shape->moveTo(x0, y0);
    shape->lineTo(x1, y1);
    shape->lineTo(x2, y2);
    shape->fill(0, 0, 0, 0);
    shape->strokeWidth(std::max(1.0f, size * 0.09f));
    shape->strokeCap(lvg::StrokeCap::Round);
    shape->strokeJoin(lvg::StrokeJoin::Round);
    shape->strokeFill(r, g, b, a);
    return push_with_clip(shape);
  };

  // Get current font
  FontCache* font = get_font(current_font_name_);
  if (!font) {
    if (draw_ligature_fallback()) return true;
    if (draw_bullet_fallback()) return true;
    if (draw_checkmark_fallback()) return true;
    // Fallback to tofu box placeholder
    draw_missing_glyph_placeholder(x, y, size, r, g, b, a);
    return true;
  }

  // Use bitmap rendering for fill-only, non-rotated text (common case)
  {
    bool fill_only = (state_.text_render_mode == 0);
    bool needs_precise_text =
        (std::abs(state_.char_spacing) > 1e-6f) ||
        (std::abs(state_.word_spacing) > 1e-6f);
    float font_scale_tm =
        std::sqrt(state_.text_matrix.a * state_.text_matrix.a +
                  state_.text_matrix.b * state_.text_matrix.b);
    float sin_theta_abs = 0.0f;
    if (font_scale_tm > 0.01f) {
      sin_theta_abs = std::abs(state_.text_matrix.b / font_scale_tm);
    }
    bool is_rotated = (sin_theta_abs > 0.01f);
    if (fill_only && !is_rotated && !needs_precise_text && !in_tj_text_draw_) {
      if (draw_glyph_bitmap(codepoint, x, y, size, r, g, b, a)) {
        return true;
      }
      // Fall through to outline path if bitmap rendering failed
    }
  }

  // Resolve glyph index.  We get it first (fast cmap lookup) so we can
  // short-circuit .notdef and check the outline cache before parsing.
  uint16_t gid = 0;
  if (font->has_ttf_parse) {
    gid = ttf_cmap_lookup(&font->ttf, static_cast<uint32_t>(codepoint));
  } else {
    gid = static_cast<uint16_t>(stbtt_FindGlyphIndex(&font->font_info, codepoint));
  }

  // Handle missing glyph early
  if (gid == 0) {
    if (codepoint == 0x20 || codepoint == 0x09 || codepoint == 0x0A ||
        codepoint == 0x0D || codepoint == 0x00A0) {
      return true;
    }
    if (draw_ligature_fallback()) return true;
    if (draw_bullet_fallback()) return true;
    if (draw_checkmark_fallback()) return true;
    if (try_draw_glyph_fallback(codepoint, x, y, size, r, g, b, a)) return true;
    draw_missing_glyph_placeholder(x, y, size, r, g, b, a);
    return true;
  }

  // --- Glyph outline vector cache ---
  // Caches deeply parsed ttf_outline_t data so that rotated / stroked / clipped
  // text reuses the outline without re-parsing sfnt tables (ttf_glyph_outline).
  GlyphOutlineKey cache_key{font->font_id, static_cast<int>(gid)};
  auto cache_it = glyph_outline_cache_.find(cache_key);
  bool outline_from_cache = (cache_it != glyph_outline_cache_.end());

  ttf_outline_t outline{};
  bool have_ttf_outline = false;
  stbtt_vertex* vertices = nullptr;
  int num_verts = 0;

  if (outline_from_cache) {
    // Wire a temporary ttf_outline_t pointing into the cached vectors.
    const auto& cached = cache_it->second;
    outline.points = const_cast<ttf_point_t*>(cached.points.data());
    outline.contour_ends = const_cast<int*>(cached.contour_ends.data());
    outline.num_points = static_cast<int>(cached.points.size());
    outline.num_contours = static_cast<int>(cached.contour_ends.size());
    outline.x_min = cached.x_min;
    outline.y_min = cached.y_min;
    outline.x_max = cached.x_max;
    outline.y_max = cached.y_max;
    have_ttf_outline = true;
  } else if (font->has_ttf_parse &&
             ttf_glyph_outline(&font->ttf, gid, &outline) == 0) {
    have_ttf_outline = true;

    // Deep-copy into cache
    GlyphOutlineEntry entry;
    entry.points.assign(outline.points, outline.points + outline.num_points);
    entry.contour_ends.assign(outline.contour_ends,
                              outline.contour_ends + outline.num_contours);
    entry.x_min = outline.x_min;
    entry.y_min = outline.y_min;
    entry.x_max = outline.x_max;
    entry.y_max = outline.y_max;
    if (glyph_outline_cache_.size() >= kMaxGlyphOutlineCacheEntries) {
      glyph_outline_cache_.erase(glyph_outline_cache_.begin());
    }
    glyph_outline_cache_[cache_key] = std::move(entry);
  }

  if (!have_ttf_outline) {
    // stbtt_GetGlyphShape returns the font's .notdef glyph when the
    // codepoint is not in the cmap — we already checked gid==0 above,
    // so this path only runs when ttf_parse is unavailable.
    num_verts = stbtt_GetGlyphShape(&font->font_info, gid, &vertices);
    if (num_verts == 0 || !vertices) {
      if (codepoint == 0x20 || codepoint == 0x09 || codepoint == 0x0A ||
          codepoint == 0x0D || codepoint == 0x00A0) {
        return true;
      }
      if (draw_ligature_fallback()) return true;
      if (draw_bullet_fallback()) return true;
      if (draw_checkmark_fallback()) return true;
      if (try_draw_glyph_fallback(codepoint, x, y, size, r, g, b, a)) return true;
      draw_missing_glyph_placeholder(x, y, size, r, g, b, a);
      return true;
    }
  }

  // Both code paths use the same em-based scale: size (canvas px) per em unit.
  // ttf_parse stores units_per_em; fall back to stbtt's ScaleForPixelHeight.
  float scale = 0.0f;
  if (have_ttf_outline && font->ttf.units_per_em > 0) {
    scale = size / static_cast<float>(font->ttf.units_per_em);
  } else {
    scale = stbtt_ScaleForPixelHeight(&font->font_info, size);
  }

  // Compute rotation from text matrix for rotated text (e.g., [0 -8 8 0])
  float font_scale_tm = std::sqrt(state_.text_matrix.a * state_.text_matrix.a +
                                  state_.text_matrix.b * state_.text_matrix.b);
  float cos_theta = 1.0f, sin_theta = 0.0f;
  if (font_scale_tm > 0.01f) {
    cos_theta = state_.text_matrix.a / font_scale_tm;
    sin_theta = state_.text_matrix.b / font_scale_tm;
  }

  auto shape = lvg::Shape::gen();
  if (!shape) {
    if (have_ttf_outline && !outline_from_cache) {
      ttf_outline_free(&outline);
    } else if (!have_ttf_outline && vertices) {
      stbtt_FreeShape(&font->font_info, vertices);
    }
    return false;
  }

  int render_mode = state_.text_render_mode;
  bool do_fill = (render_mode == 0 || render_mode == 2 || render_mode == 4 || render_mode == 6);
  bool do_stroke = (render_mode == 1 || render_mode == 2 || render_mode == 5 || render_mode == 6);
  bool invisible = (render_mode == 3);
  bool add_to_clip = (render_mode >= 4 && render_mode <= 7);

  // Emit path commands into both the shape and (when needed) the clip accumulator
  // in a single outline walk.
  auto emit_move = [&](float cx, float cy) {
    shape->moveTo(cx, cy);
    if (add_to_clip) {
      state_.text_clip_commands.push_back(lvg::PathCommand::MoveTo);
      state_.text_clip_points.push_back({cx, cy});
    }
  };
  auto emit_line = [&](float cx, float cy) {
    shape->lineTo(cx, cy);
    if (add_to_clip) {
      state_.text_clip_commands.push_back(lvg::PathCommand::LineTo);
      state_.text_clip_points.push_back({cx, cy});
    }
  };
  auto emit_cubic = [&](float c1x, float c1y, float c2x, float c2y, float ex, float ey) {
    shape->cubicTo(c1x, c1y, c2x, c2y, ex, ey);
    if (add_to_clip) {
      state_.text_clip_commands.push_back(lvg::PathCommand::CubicTo);
      state_.text_clip_points.push_back({c1x, c1y});
      state_.text_clip_points.push_back({c2x, c2y});
      state_.text_clip_points.push_back({ex, ey});
    }
  };
  auto emit_close = [&]() {
    shape->close();
    if (add_to_clip) {
      state_.text_clip_commands.push_back(lvg::PathCommand::Close);
    }
  };

  if (add_to_clip) state_.text_clip_active = true;

  if (have_ttf_outline) {
    decompose_ttf_outline_to_path(outline, scale, x, y, cos_theta, sin_theta,
                                  emit_move, emit_line, emit_cubic,
                                  emit_close);
  } else {
    // Legacy stbtt path — only reached when ttf_parse couldn't load the font.
    auto transform_vertex = [&](float gx_raw, float gy_raw, float& out_x, float& out_y) {
      float gx = gx_raw * scale;
      float gy = gy_raw * scale;
      out_x = x + gx * cos_theta - gy * sin_theta;
      out_y = y - (gx * sin_theta + gy * cos_theta);
    };
    float curr_x = x, curr_y = y;
    for (int i = 0; i < num_verts; i++) {
      stbtt_vertex* v = &vertices[i];
      float vx, vy;
      transform_vertex(v->x, v->y, vx, vy);
      switch (v->type) {
        case STBTT_vmove:
          if (i > 0) emit_close();
          emit_move(vx, vy);
          curr_x = vx; curr_y = vy; break;
        case STBTT_vline:
          emit_line(vx, vy);
          curr_x = vx; curr_y = vy; break;
        case STBTT_vcurve: {
          float cx, cy; transform_vertex(v->cx, v->cy, cx, cy);
          float cp1x = curr_x + (2.0f / 3.0f) * (cx - curr_x);
          float cp1y = curr_y + (2.0f / 3.0f) * (cy - curr_y);
          float cp2x = vx + (2.0f / 3.0f) * (cx - vx);
          float cp2y = vy + (2.0f / 3.0f) * (cy - vy);
          emit_cubic(cp1x, cp1y, cp2x, cp2y, vx, vy);
          curr_x = vx; curr_y = vy; break;
        }
        case STBTT_vcubic: {
          float cx1, cy1, cx2, cy2;
          transform_vertex(v->cx, v->cy, cx1, cy1);
          transform_vertex(v->cx1, v->cy1, cx2, cy2);
          emit_cubic(cx1, cy1, cx2, cy2, vx, vy);
          curr_x = vx; curr_y = vy; break;
        }
      }
    }
    emit_close();
  }

  if (have_ttf_outline && !outline_from_cache) {
    ttf_outline_free(&outline);
  } else if (!have_ttf_outline && vertices) {
    stbtt_FreeShape(&font->font_info, vertices);
  }

  // Mode 7: clip only - don't render visible shape
  if (render_mode == 7 || invisible) {
    return true;
  }

  if (do_fill) {
    shape->fill(r, g, b, a);
  }

  if (do_stroke) {
    float stroke_width = state_.stroke_width * state_.scale;
    if (state_.stroke_width == 0.0f) stroke_width = 1.0f;
    shape->strokeWidth(stroke_width);
    uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
    shape->strokeFill(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha);

    lvg::StrokeCap cap = lvg::StrokeCap::Butt;
    if (state_.line_cap == 1) cap = lvg::StrokeCap::Round;
    else if (state_.line_cap == 2) cap = lvg::StrokeCap::Square;
    shape->strokeCap(cap);

    lvg::StrokeJoin join = lvg::StrokeJoin::Miter;
    if (state_.line_join == 1) join = lvg::StrokeJoin::Round;
    else if (state_.line_join == 2) join = lvg::StrokeJoin::Bevel;
    shape->strokeJoin(join);
  }

  return push_with_clip(shape);
}

bool LightVGBackend::draw_glyph_by_index(int glyph_index, float x, float y, float size,
                                        uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  // Get current font
  FontCache* font = get_font(current_font_name_);
  if (!font) {
    // Fallback to tofu box placeholder
    draw_missing_glyph_placeholder(x, y, size, r, g, b, a);
    return true;
  }

  // Use bitmap rendering for fill-only, non-rotated text (common case)
  {
    bool fill_only = (state_.text_render_mode == 0);
    float font_scale_tm =
        std::sqrt(state_.text_matrix.a * state_.text_matrix.a +
                  state_.text_matrix.b * state_.text_matrix.b);
    float sin_theta_abs = 0.0f;
    if (font_scale_tm > 0.01f) {
      sin_theta_abs = std::abs(state_.text_matrix.b / font_scale_tm);
    }
    bool is_rotated = (sin_theta_abs > 0.01f);
    if (fill_only && !is_rotated && !in_tj_text_draw_) {
      if (draw_glyph_bitmap_by_index(glyph_index, x, y, size, r, g, b, a)) {
        return true;
      }
      // Fall through to outline path if bitmap rendering failed
    }
  }

  // --- Glyph outline vector cache ---
  GlyphOutlineKey cache_key{font->font_id, glyph_index};
  auto cache_it = glyph_outline_cache_.find(cache_key);
  bool outline_from_cache = (cache_it != glyph_outline_cache_.end());

  ttf_outline_t outline{};
  bool have_ttf_outline = false;
  stbtt_vertex* vertices = nullptr;
  int num_verts = 0;

  if (outline_from_cache) {
    const auto& cached = cache_it->second;
    outline.points = const_cast<ttf_point_t*>(cached.points.data());
    outline.contour_ends = const_cast<int*>(cached.contour_ends.data());
    outline.num_points = static_cast<int>(cached.points.size());
    outline.num_contours = static_cast<int>(cached.contour_ends.size());
    outline.x_min = cached.x_min;
    outline.y_min = cached.y_min;
    outline.x_max = cached.x_max;
    outline.y_max = cached.y_max;
    have_ttf_outline = true;
  } else if (font->has_ttf_parse &&
             ttf_glyph_outline(&font->ttf, static_cast<uint16_t>(glyph_index),
                               &outline) == 0) {
    have_ttf_outline = true;

    GlyphOutlineEntry entry;
    entry.points.assign(outline.points, outline.points + outline.num_points);
    entry.contour_ends.assign(outline.contour_ends,
                              outline.contour_ends + outline.num_contours);
    entry.x_min = outline.x_min;
    entry.y_min = outline.y_min;
    entry.x_max = outline.x_max;
    entry.y_max = outline.y_max;
    if (glyph_outline_cache_.size() >= kMaxGlyphOutlineCacheEntries) {
      glyph_outline_cache_.erase(glyph_outline_cache_.begin());
    }
    glyph_outline_cache_[cache_key] = std::move(entry);
  }

  if (!have_ttf_outline) {
    num_verts = stbtt_GetGlyphShape(&font->font_info, glyph_index, &vertices);
    if (num_verts == 0 || !vertices) {
      draw_missing_glyph_placeholder(x, y, size, r, g, b, a);
      return true;
    }
  }

  float scale = 0.0f;
  if (have_ttf_outline && font->ttf.units_per_em > 0) {
    scale = size / static_cast<float>(font->ttf.units_per_em);
  } else {
    scale = stbtt_ScaleForPixelHeight(&font->font_info, size);
  }

  float font_scale_tm = std::sqrt(state_.text_matrix.a * state_.text_matrix.a +
                                  state_.text_matrix.b * state_.text_matrix.b);
  float cos_theta = 1.0f, sin_theta = 0.0f;
  if (font_scale_tm > 0.01f) {
    cos_theta = state_.text_matrix.a / font_scale_tm;
    sin_theta = state_.text_matrix.b / font_scale_tm;
  }

  auto shape = lvg::Shape::gen();
  if (!shape) {
    if (have_ttf_outline && !outline_from_cache) {
      ttf_outline_free(&outline);
    } else if (!have_ttf_outline && vertices) {
      stbtt_FreeShape(&font->font_info, vertices);
    }
    return false;
  }

  int render_mode = state_.text_render_mode;
  bool do_fill = (render_mode == 0 || render_mode == 2 || render_mode == 4 || render_mode == 6);
  bool do_stroke = (render_mode == 1 || render_mode == 2 || render_mode == 5 || render_mode == 6);
  bool invisible = (render_mode == 3);
  bool add_to_clip = (render_mode >= 4 && render_mode <= 7);

  auto emit_move = [&](float cx, float cy) {
    shape->moveTo(cx, cy);
    if (add_to_clip) {
      state_.text_clip_commands.push_back(lvg::PathCommand::MoveTo);
      state_.text_clip_points.push_back({cx, cy});
    }
  };
  auto emit_line = [&](float cx, float cy) {
    shape->lineTo(cx, cy);
    if (add_to_clip) {
      state_.text_clip_commands.push_back(lvg::PathCommand::LineTo);
      state_.text_clip_points.push_back({cx, cy});
    }
  };
  auto emit_cubic = [&](float c1x, float c1y, float c2x, float c2y, float ex, float ey) {
    shape->cubicTo(c1x, c1y, c2x, c2y, ex, ey);
    if (add_to_clip) {
      state_.text_clip_commands.push_back(lvg::PathCommand::CubicTo);
      state_.text_clip_points.push_back({c1x, c1y});
      state_.text_clip_points.push_back({c2x, c2y});
      state_.text_clip_points.push_back({ex, ey});
    }
  };
  auto emit_close = [&]() {
    shape->close();
    if (add_to_clip) {
      state_.text_clip_commands.push_back(lvg::PathCommand::Close);
    }
  };

  if (add_to_clip) state_.text_clip_active = true;

  if (have_ttf_outline) {
    decompose_ttf_outline_to_path(outline, scale, x, y, cos_theta, sin_theta,
                                  emit_move, emit_line, emit_cubic,
                                  emit_close);
  } else {
    auto transform_vertex = [&](float gx_raw, float gy_raw, float& out_x, float& out_y) {
      float gx = gx_raw * scale;
      float gy = gy_raw * scale;
      out_x = x + gx * cos_theta - gy * sin_theta;
      out_y = y - (gx * sin_theta + gy * cos_theta);
    };
    float curr_x = x, curr_y = y;
    for (int i = 0; i < num_verts; i++) {
      stbtt_vertex* v = &vertices[i];
      float vx, vy;
      transform_vertex(v->x, v->y, vx, vy);
      switch (v->type) {
        case STBTT_vmove:
          if (i > 0) emit_close();
          emit_move(vx, vy); curr_x = vx; curr_y = vy; break;
        case STBTT_vline: emit_line(vx, vy); curr_x = vx; curr_y = vy; break;
        case STBTT_vcurve: {
          float cx, cy; transform_vertex(v->cx, v->cy, cx, cy);
          float cp1x = curr_x + (2.0f / 3.0f) * (cx - curr_x);
          float cp1y = curr_y + (2.0f / 3.0f) * (cy - curr_y);
          float cp2x = vx + (2.0f / 3.0f) * (cx - vx);
          float cp2y = vy + (2.0f / 3.0f) * (cy - vy);
          emit_cubic(cp1x, cp1y, cp2x, cp2y, vx, vy);
          curr_x = vx; curr_y = vy; break;
        }
        case STBTT_vcubic: {
          float cx1, cy1, cx2, cy2;
          transform_vertex(v->cx, v->cy, cx1, cy1);
          transform_vertex(v->cx1, v->cy1, cx2, cy2);
          emit_cubic(cx1, cy1, cx2, cy2, vx, vy);
          curr_x = vx; curr_y = vy; break;
        }
      }
    }
    emit_close();
  }

  if (have_ttf_outline && !outline_from_cache) {
    ttf_outline_free(&outline);
  } else if (!have_ttf_outline && vertices) {
    stbtt_FreeShape(&font->font_info, vertices);
  }

  if (render_mode == 7 || invisible) {
    return true;
  }

  if (do_fill) {
    shape->fill(r, g, b, a);
  }

  if (do_stroke) {
    float stroke_width = state_.stroke_width * state_.scale;
    if (state_.stroke_width == 0.0f) stroke_width = 1.0f;
    shape->strokeWidth(stroke_width);
    uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
    shape->strokeFill(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha);

    lvg::StrokeCap cap = lvg::StrokeCap::Butt;
    if (state_.line_cap == 1) cap = lvg::StrokeCap::Round;
    else if (state_.line_cap == 2) cap = lvg::StrokeCap::Square;
    shape->strokeCap(cap);

    lvg::StrokeJoin join = lvg::StrokeJoin::Miter;
    if (state_.line_join == 1) join = lvg::StrokeJoin::Round;
    else if (state_.line_join == 2) join = lvg::StrokeJoin::Bevel;
    shape->strokeJoin(join);
  }

  return push_with_clip(shape);
}

bool LightVGBackend::draw_glyph_bitmap(int codepoint, float x, float y,
                                      float size, uint8_t r, uint8_t g,
                                      uint8_t b, uint8_t a) {
  FontCache* font = get_font(current_font_name_);
  if (!font) return false;
  int glyph_index = 0;
  if (font->has_ttf_parse) {
    glyph_index = ttf_cmap_lookup(&font->ttf, static_cast<uint32_t>(codepoint));
  }
  if (glyph_index == 0) {
    glyph_index = stbtt_FindGlyphIndex(&font->font_info, codepoint);
  }
  if (glyph_index == 0) return false;
  return draw_glyph_bitmap_by_index(glyph_index, x, y, size, r, g, b, a);
}

bool LightVGBackend::draw_glyph_bitmap_by_index(int glyph_index, float x,
                                                float y, float size,
                                                uint8_t r, uint8_t g,
                                                uint8_t b, uint8_t a) {
  FontCache* font = get_font(current_font_name_);
  if (!font || !scene_) return false;

  // Quantize size for cache lookup (quarter-pixel granularity)
  uint16_t size_q = static_cast<uint16_t>(size * 4.0f + 0.5f);
  GlyphBitmapKey key{font->font_id, glyph_index, size_q, enable_lcd_};

  const GlyphBitmapEntry* entry = nullptr;
  auto cache_it = glyph_bitmap_cache_.find(key);

  if (cache_it != glyph_bitmap_cache_.end()) {
    entry = &cache_it->second;
  } else {
    // Evict a single entry when full (much better than clearing all 4096)
    if (glyph_bitmap_cache_.size() >= kMaxGlyphCacheEntries) {
      glyph_bitmap_cache_.erase(glyph_bitmap_cache_.begin());
    }

    // Oversample factor: 3x for LCD subpixel (3-wide samples per output
    // pixel), 2x for standard grayscale AA.
    float oversample = enable_lcd_ ? 3.0f : 2.0f;
    auto& e = glyph_bitmap_cache_[key];
    bool produced = false;

    if (font->has_ttf_parse) {
      ttf_outline_t outline{};
      if (ttf_glyph_outline(&font->ttf, static_cast<uint16_t>(glyph_index),
                            &outline) == 0) {
        float em_scale =
            (font->ttf.units_per_em > 0)
                ? ((size * oversample) /
                   static_cast<float>(font->ttf.units_per_em))
                : stbtt_ScaleForPixelHeight(&font->font_info,
                                            size * oversample);
        rast_bitmap_t bm{};
        if (rast_glyph(&outline, em_scale, &bm) == 0 && bm.pixels &&
            bm.width > 0 && bm.height > 0) {
          int w_ss = bm.width;
          int h_ss = bm.height;
          float xoff_ss = static_cast<float>(bm.bearing_x);
          float yoff_ss = -static_cast<float>(bm.bearing_y);

          int os = static_cast<int>(oversample);
          int w1x = (w_ss + os - 1) / os;
          int h1x = (h_ss + os - 1) / os;
          if (enable_lcd_) {
            // LCD: 3×3 oversampled → per-channel alpha.
            // Each output pixel gets 3 bytes (R/G/B coverage) from the
            // corresponding 3×3 block's vertical-averaged columns.
            e.bitmap.resize(static_cast<size_t>(w1x) * h1x * 3, 0);
            for (int iy = 0; iy < h1x; ++iy)
              for (int ix = 0; ix < w1x; ++ix) {
                float cols[3] = {0, 0, 0};
                int ccnt[3] = {0, 0, 0};
                for (int dy = 0; dy < 3; ++dy)
                  for (int dx = 0; dx < 3; ++dx) {
                    int sy = iy * 3 + dy, sx = ix * 3 + dx;
                    if (sy < h_ss && sx < w_ss) {
                      cols[dx] += bm.pixels[sy * w_ss + sx];
                      ccnt[dx]++;
                    }
                  }
                int bi = (iy * w1x + ix) * 3;
                for (int ch = 0; ch < 3; ++ch) {
                  float v = cols[ch] / std::max(1, ccnt[ch]);
                  float corrected = std::pow(v / 255.0f, 1.4f);
                  e.bitmap[bi + ch] =
                      static_cast<uint8_t>(corrected * 255.0f + 0.5f);
                }
              }
            e.is_lcd = true;
            e.lcd_scale = 3;
          } else {
            // Grayscale AA: box-filter 2x blocks
            e.bitmap.assign(static_cast<size_t>(w1x) * h1x, 0);
            for (int iy = 0; iy < h1x; ++iy)
              for (int ix = 0; ix < w1x; ++ix) {
                int sum = 0, count = 0;
                for (int dy = 0; dy < os; ++dy)
                  for (int dx = 0; dx < os; ++dx) {
                    int sy = iy * os + dy, sx = ix * os + dx;
                    if (sy < h_ss && sx < w_ss) {
                      sum += bm.pixels[sy * w_ss + sx];
                      ++count;
                    }
                  }
                e.bitmap[iy * w1x + ix] =
                    static_cast<uint8_t>(sum / count);
              }
          }
          e.width = w1x;
          e.height = h1x;
          e.xoff = xoff_ss / oversample;
          e.yoff = yoff_ss / oversample;
          free(bm.pixels);
          produced = true;
        } else if (bm.pixels) {
          free(bm.pixels);
        }
        ttf_outline_free(&outline);
      }
    }

    if (!produced) {
      float scale_ss =
          stbtt_ScaleForPixelHeight(&font->font_info, size * oversample);
      int w_ss, h_ss, xoff_ss, yoff_ss;
      unsigned char* bmp_ss = stbtt_GetGlyphBitmapSubpixel(
          &font->font_info, scale_ss, scale_ss, 0, 0, glyph_index, &w_ss,
          &h_ss, &xoff_ss, &yoff_ss);

      if (!bmp_ss || w_ss <= 0 || h_ss <= 0) {
        e.width = 0; e.height = 0; e.xoff = 0; e.yoff = 0;
        if (bmp_ss) stbtt_FreeBitmap(bmp_ss, font->font_info.userdata);
      } else {
        int os = static_cast<int>(oversample);
        int w1x = (w_ss + os - 1) / os;
        int h1x = (h_ss + os - 1) / os;
        if (enable_lcd_) {
          // LCD: 3×3 → per-channel alpha
          e.bitmap.resize(static_cast<size_t>(w1x) * h1x * 3, 0);
          for (int iy = 0; iy < h1x; iy++)
            for (int ix = 0; ix < w1x; ix++) {
              float cols[3] = {0, 0, 0};
              int ccnt[3] = {0, 0, 0};
              for (int dy = 0; dy < 3; dy++)
                for (int dx = 0; dx < 3; dx++) {
                  int sy = iy * 3 + dy, sx = ix * 3 + dx;
                  if (sy < h_ss && sx < w_ss) {
                    cols[dx] += bmp_ss[sy * w_ss + sx];
                    ccnt[dx]++;
                  }
                }
              int bi = (iy * w1x + ix) * 3;
              for (int ch = 0; ch < 3; ++ch) {
                float v = cols[ch] / std::max(1, ccnt[ch]);
                float corrected = std::pow(v / 255.0f, 1.4f);
                e.bitmap[bi + ch] =
                    static_cast<uint8_t>(corrected * 255.0f + 0.5f);
              }
            }
          e.is_lcd = true;
          e.lcd_scale = 3;
        } else {
          // Grayscale AA: box-filter 2x blocks
          e.bitmap.resize(static_cast<size_t>(w1x) * h1x);
          for (int iy = 0; iy < h1x; iy++)
            for (int ix = 0; ix < w1x; ix++) {
              int sum = 0, count = 0;
              for (int dy = 0; dy < os; dy++)
                for (int dx = 0; dx < os; dx++) {
                  int sy = iy * os + dy, sx = ix * os + dx;
                  if (sy < h_ss && sx < w_ss) {
                    sum += bmp_ss[sy * w_ss + sx];
                    count++;
                  }
                }
              e.bitmap[iy * w1x + ix] =
                  static_cast<uint8_t>(sum / count);
            }
        }
        e.width = w1x;
        e.height = h1x;
        e.xoff = static_cast<float>(xoff_ss) / oversample;
        e.yoff = static_cast<float>(yoff_ss) / oversample;
        stbtt_FreeBitmap(bmp_ss, font->font_info.userdata);
      }
    }
    entry = &e;
  }

  if (!entry || entry->width <= 0 || entry->height <= 0) {
    return true;  // empty glyph (e.g. space)
  }

  // Create ARGB8888 bitmap with glyph color + alpha from bitmap
  int gw = entry->width;
  int gh = entry->height;
  size_t npixels = static_cast<size_t>(gw) * gh;

  // Try render cache for pre-colored ARGB — skips per-pixel fill on hit.
  std::string glyph_cache_key;
  bool glyph_cached = false;
  glyph_cache_key = "glyph:" + current_font_name_ + ":" +
                    std::to_string(glyph_index) + ":" +
                    std::to_string(size_q) + ":" +
                    (enable_lcd_ ? "1" : "0") + ":" +
                    std::to_string(r) + ":" +
                    std::to_string(g) + ":" +
                    std::to_string(b) + ":" +
                    std::to_string(a);
  {
    RenderCacheEntry gce;
    if (RenderCache::instance().find(glyph_cache_key, gce) &&
        gce.width == static_cast<uint32_t>(gw) &&
        gce.height == static_cast<uint32_t>(gh)) {
      glyph_argb_buf_.resize(npixels);
      memcpy(glyph_argb_buf_.data(), gce.data.data(), npixels * sizeof(uint32_t));
      glyph_cached = true;
    }
  }
  if (!glyph_cached) {
    // Reuse pre-allocated buffer to avoid per-glyph heap allocation.
    glyph_argb_buf_.resize(npixels);
    if (entry->is_lcd) {
    // LCD subpixel: per-channel alpha (3 bytes/pixel, gamma-corrected)
    for (int i = 0; i < gw * gh; i++) {
      const uint8_t* pc = &entry->bitmap[i * 3];
      uint8_t alpha_r = static_cast<uint8_t>(
          (pc[0] / 255.0f) * a + 0.5f);
      uint8_t alpha_g = static_cast<uint8_t>(
          (pc[1] / 255.0f) * a + 0.5f);
      uint8_t alpha_b = static_cast<uint8_t>(
          (pc[2] / 255.0f) * a + 0.5f);
      uint8_t max_a = std::max({alpha_r, alpha_g, alpha_b});
      // Pre-multiplied ARGB: alpha = max per-channel, RGB modulated
      glyph_argb_buf_[static_cast<size_t>(i)] =
          (static_cast<uint32_t>(max_a) << 24) |
          (static_cast<uint32_t>(r) * alpha_r / 255 << 16) |
          (static_cast<uint32_t>(g) * alpha_g / 255 << 8) |
          (static_cast<uint32_t>(b) * alpha_b / 255);
    }
  } else {
    // Grayscale AA: single alpha per pixel, straight ARGB. The 1.4 gamma curve
    // comes from a 256-entry LUT (glyph_gamma_lut()), bit-identical to the
    // previous std::pow(byte/255, 1.4f) but without the per-pixel transcendental.
    const std::array<float, 256>& gamma = glyph_gamma_lut();
    for (int i = 0; i < gw * gh; i++) {
      uint8_t alpha = static_cast<uint8_t>(gamma[entry->bitmap[i]] * a + 0.5f);
      glyph_argb_buf_[static_cast<size_t>(i)] =
          (static_cast<uint32_t>(alpha) << 24) |
          (static_cast<uint32_t>(r) << 16) |
          (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }
  }

    // Store colored ARGB in render cache for future same-color hits.
    size_t argb_bytes = glyph_argb_buf_.size() * sizeof(uint32_t);
    RenderCacheEntry gce_out;
    gce_out.width = static_cast<uint32_t>(gw);
    gce_out.height = static_cast<uint32_t>(gh);
    gce_out.data.resize(argb_bytes);
    memcpy(gce_out.data.data(), glyph_argb_buf_.data(), argb_bytes);
    RenderCache::instance().store(glyph_cache_key, std::move(gce_out));
  }

  // Create LightVG Picture and position it. Picture copies the input pixels.
  auto picture = lvg::Picture::gen();
  if (!picture) {
    return false;
  }

  if (picture->load(glyph_argb_buf_.data(), gw, gh, lvg::ColorSpace::ARGB8888S, true) !=
      lvg::Result::Success) {
    picture->unref();
    return false;
  }

  // Position: glyph origin (x, y) + bitmap offset.
  // Honour the effective text-rendering matrix sign so horizontal/vertical
  // flips actually mirror the bitmap. PDF mirrors can come from either the
  // text matrix (Tm) or the CTM (cm). Bitmap path is only taken when the
  // text matrix is axis-aligned, so the effective x/y direction signs
  // come from CTM.a * tm.a and CTM.d * tm.d (cross terms tm.b/tm.c are
  // ~0 here).
  float eff_a = state_.transform.a * state_.text_matrix.a;
  float eff_d = state_.transform.d * state_.text_matrix.d;
  bool mirror_x = (eff_a < 0.0f);
  bool mirror_y = (eff_d < 0.0f);
  lvg::Matrix m;
  m.e11 = mirror_x ? -1.0f : 1.0f;
  m.e12 = 0.0f;
  m.e13 = mirror_x ? (x - entry->xoff) : (x + entry->xoff);
  m.e21 = 0.0f;
  m.e22 = mirror_y ? -1.0f : 1.0f;
  m.e23 = mirror_y ? (y - entry->yoff) : (y + entry->yoff);
  m.e31 = 0.0f;
  m.e32 = 0.0f;
  m.e33 = 1.0f;
  picture->transform(m);

  // Apply blend mode
  if (state_.blend_mode != 0) {
    picture->blend(static_cast<lvg::BlendMethod>(state_.blend_mode));
  }

  // Apply clipping if active
  if (state_.has_clip && !state_.clip_commands.empty()) {
    auto clipper = lvg::Shape::gen();
    append_shape_geometry(clipper, state_.clip_commands, state_.clip_points);
    if (state_.clip_even_odd) {
      clipper->fillRule(lvg::FillRule::EvenOdd);
    } else {
      clipper->fillRule(lvg::FillRule::NonZero);
    }
    if (picture->clip(clipper) != lvg::Result::Success) {
      clipper->unref();
    }
  }

  apply_soft_mask_opacity(picture);
  scene_->add(picture);
  return true;
}

LightVGRenderResult LightVGBackend::get_buffer() {
  LightVGRenderResult result;

  if (!initialized_) {
    result.error = "Backend not initialized";
    return result;
  }

  result.width = width_;
  result.height = height_;

  // Convert from ABGR8888 to RGBA8888
  result.pixels.resize(width_ * height_ * 4);
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  std::memcpy(result.pixels.data(), buffer_.data(), result.pixels.size());
#else
  for (size_t i = 0; i < buffer_.size(); ++i) {
    uint32_t pixel = buffer_[i];
    uint8_t a = (pixel >> 24) & 0xFF;
    uint8_t b = (pixel >> 16) & 0xFF;
    uint8_t g = (pixel >> 8) & 0xFF;
    uint8_t r = pixel & 0xFF;

    result.pixels[i * 4 + 0] = r;
    result.pixels[i * 4 + 1] = g;
    result.pixels[i * 4 + 2] = b;
    result.pixels[i * 4 + 3] = a;
  }
#endif

  result.success = true;
  return result;
}

LightVGBackend::CacheStats LightVGBackend::get_cache_stats() const {
  CacheStats stats;
  auto rc = RenderCache::instance().stats();
  stats.hits = rc.hits;
  stats.misses = rc.misses;
  stats.evictions = rc.evictions;
  stats.entries = rc.entries;
  stats.bytes_used = rc.bytes_used;
  stats.max_size = RenderCache::instance().max_size();
  return stats;
}

bool LightVGBackend::save_to_png(const std::string& filename) {
  LightVGRenderOptions options;
  options.format = LightVGRenderOptions::Format::PNG;
  return save_to_file(filename, options);
}

static bool save_rgba_pixels_to_file(const std::string& filename,
                                     const uint8_t* pixels,
                                     int width,
                                     int height,
                                     const RenderOptions& options) {
  if (!pixels || width <= 0 || height <= 0) return false;

  int ret = 0;
  int stride = width * 4;
  switch (options.format) {
    case RenderOptions::Format::PNG: {
#ifdef NANOPDF_USE_FPNGE
      if (nanopdf::fpnge_available()) {
        std::vector<uint8_t> png_bytes;
        if (nanopdf::fpnge_encode_png(pixels, width, height, /*channels=*/4,
                                      stride, options.png_compression,
                                      png_bytes)) {
          std::ofstream out(filename, std::ios::binary);
          if (out.write(reinterpret_cast<const char*>(png_bytes.data()),
                        static_cast<std::streamsize>(png_bytes.size()))) {
            ret = 1;
            break;
          }
        }
      }
#endif
      stbi_write_png_compression_level = options.png_compression;
      ret = stbi_write_png(filename.c_str(), width, height, 4,
                           pixels, stride);
      break;
    }
    case RenderOptions::Format::JPEG: {
      std::vector<uint8_t> rgb_pixels(static_cast<size_t>(width) * height * 3);
      for (int i = 0; i < width * height; i++) {
        rgb_pixels[i * 3 + 0] = pixels[i * 4 + 0];
        rgb_pixels[i * 3 + 1] = pixels[i * 4 + 1];
        rgb_pixels[i * 3 + 2] = pixels[i * 4 + 2];
      }
      ret = stbi_write_jpg(filename.c_str(), width, height, 3,
                           rgb_pixels.data(), options.jpeg_quality);
      break;
    }
    case RenderOptions::Format::BMP:
      ret = stbi_write_bmp(filename.c_str(), width, height, 4, pixels);
      break;
    case RenderOptions::Format::TGA:
      ret = stbi_write_tga(filename.c_str(), width, height, 4, pixels);
      break;
  }
  return ret != 0;
}

static std::vector<uint8_t> rotate_rgba_pixels_copy(const uint8_t* src,
                                                    int src_w,
                                                    int src_h,
                                                    int rotation,
                                                    int* out_w,
                                                    int* out_h) {
  if (!src || src_w <= 0 || src_h <= 0 || !out_w || !out_h) return {};
  rotation %= 360;
  if (rotation < 0) rotation += 360;
  if (rotation != 90 && rotation != 180 && rotation != 270) return {};

  const int dst_w = (rotation == 90 || rotation == 270) ? src_h : src_w;
  const int dst_h = (rotation == 90 || rotation == 270) ? src_w : src_h;
  std::vector<uint8_t> dst(static_cast<size_t>(dst_w) * dst_h * 4);
  for (int dy = 0; dy < dst_h; ++dy) {
    for (int dx = 0; dx < dst_w; ++dx) {
      int sx = dx;
      int sy = dy;
      if (rotation == 90) {
        sx = dy;
        sy = dst_w - 1 - dx;
      } else if (rotation == 180) {
        sx = src_w - 1 - dx;
        sy = src_h - 1 - dy;
      } else {
        sx = src_w - 1 - dy;
        sy = dx;
      }
      const uint8_t* sp = src + (static_cast<size_t>(sy) * src_w + sx) * 4;
      uint8_t* dp = dst.data() + (static_cast<size_t>(dy) * dst_w + dx) * 4;
      dp[0] = sp[0];
      dp[1] = sp[1];
      dp[2] = sp[2];
      dp[3] = sp[3];
    }
  }
  *out_w = dst_w;
  *out_h = dst_h;
  return dst;
}

bool LightVGBackend::save_to_file(const std::string& filename, const LightVGRenderOptions& options) {
  if (!initialized_) {
    return false;
  }

  int width = static_cast<int>(width_);
  int height = static_cast<int>(height_);
  const uint8_t* pixels = nullptr;
  std::vector<uint8_t> converted_pixels;

  if (options.format == LightVGRenderOptions::Format::TGA &&
      direct_bgra_output_enabled_) {
    return write_tga_bgra_rle(
        filename, width, height,
        reinterpret_cast<const uint8_t*>(buffer_.data()));
  }

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  pixels = reinterpret_cast<const uint8_t*>(buffer_.data());
#else
  auto result = get_buffer();
  if (!result.success) {
    return false;
  }
  converted_pixels = std::move(result.pixels);
  pixels = converted_pixels.data();
#endif

  return save_rgba_pixels_to_file(filename, pixels, width, height, options);
}

bool LightVGBackend::save_to_file_rotated(const std::string& filename,
                                          const LightVGRenderOptions& options,
                                          int rotation_degrees) {
  if (!initialized_) return false;

  int rotation = rotation_degrees % 360;
  if (rotation < 0) rotation += 360;
  if (rotation == 0) return save_to_file(filename, options);
  if (rotation != 90 && rotation != 180 && rotation != 270) return false;

  const int src_w = static_cast<int>(width_);
  const int src_h = static_cast<int>(height_);

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  const uint8_t* src = reinterpret_cast<const uint8_t*>(buffer_.data());
#else
  auto result = get_buffer();
  if (!result.success || result.pixels.empty()) return false;
  const uint8_t* src = result.pixels.data();
#endif

  int dst_w = 0;
  int dst_h = 0;
  std::vector<uint8_t> rotated =
      rotate_rgba_pixels_copy(src, src_w, src_h, rotation, &dst_w, &dst_h);
  if (rotated.empty()) return false;

  return save_rgba_pixels_to_file(filename, rotated.data(), dst_w, dst_h,
                                  options);
}

size_t LightVGBackend::count_render_objects(
    const std::vector<uint8_t>& content_data) {
  return count_render_objects_impl(
      std::string_view(reinterpret_cast<const char*>(content_data.data()),
                       content_data.size()));
}

void LightVGBackend::begin_progress(const RenderProgressCallback& callback,
                                   size_t total_objects,
                                   size_t object_threshold,
                                   uint32_t percent_step) {
  progress_ = RenderProgressState();
  if (!callback || total_objects == 0 || total_objects < object_threshold) {
    return;
  }

  progress_.callback = callback;
  progress_.total_objects = total_objects;
  progress_.percent_step = std::max<uint32_t>(1, percent_step);
  progress_.enabled = true;
  progress_.callback(RenderProgressInfo{0, progress_.total_objects, 0});
}

void LightVGBackend::advance_progress(size_t processed_objects) {
  if (!progress_.enabled || processed_objects == 0) {
    return;
  }

  progress_.processed_objects =
      std::min(progress_.processed_objects + processed_objects,
               progress_.total_objects);

  const auto current_percent = static_cast<uint32_t>(std::min<long double>(
      100.0L, (static_cast<long double>(progress_.processed_objects) * 100.0L) /
                  static_cast<long double>(progress_.total_objects)));

  for (uint32_t percent = progress_.last_percent + progress_.percent_step;
       percent <= current_percent && percent <= 100;
       percent += progress_.percent_step) {
    progress_.callback(
        RenderProgressInfo{progress_.processed_objects, progress_.total_objects,
                           percent});
    progress_.last_percent = percent;
  }
}

void LightVGBackend::finish_progress() {
  if (!progress_.enabled) {
    progress_ = RenderProgressState();
    return;
  }

  progress_.processed_objects = progress_.total_objects;
  if (progress_.last_percent < 100) {
    progress_.callback(RenderProgressInfo{progress_.processed_objects,
                                          progress_.total_objects, 100});
  }
  progress_ = RenderProgressState();
}

void LightVGBackend::set_progress_callback(RenderProgressCallback callback,
                                         size_t object_threshold,
                                         uint32_t percent_step) {
  progress_config_.callback = std::move(callback);
  progress_config_.object_threshold = object_threshold;
  progress_config_.percent_step = std::max<uint32_t>(1, percent_step);
}

void LightVGBackend::clear_progress_callback() {
  progress_config_ = RenderProgressConfig();
}

LightVGRenderResult LightVGBackend::render_page(const Pdf& pdf, const Page& page,
                                               const LightVGRenderOptions& options) {
  LightVGRenderResult result;

  // Reset per-page resource resolution storage so it doesn't grow unbounded
  // across multi-page renders.
  lookup_resolved_owned_.clear();
  lookup_font_owned_.clear();
  current_soft_mask_.reset();
  soft_mask_group_cache_.clear();

  // Get page dimensions
  float page_width = 612.0f;
  float page_height = 792.0f;

  if (page.media_box.size() >= 4) {
    page_width = static_cast<float>(page.media_box[2] - page.media_box[0]);
    page_height = static_cast<float>(page.media_box[3] - page.media_box[1]);
  }

  // Store antialias and LCD options
  antialias_ = options.antialias;
  enable_lcd_ = options.lcd_subpixel;
  max_function_shading_pixels_ = options.max_function_shading_pixels;
  if (!antialias_) {
    NANOPDF_LOG_DEBUG("LightVG", "antialias=false requested, but ThorVG's built-in AA cannot be disabled");
  }

  // Calculate scale based on DPI (72 DPI is standard PDF resolution)
  float dpi_scale = options.dpi / 72.0f;

  // Resize canvas if needed for DPI scaling (ceiling to ensure full page fits)
  uint32_t target_width = static_cast<uint32_t>(std::ceil(page_width * dpi_scale));
  uint32_t target_height = static_cast<uint32_t>(std::ceil(page_height * dpi_scale));

  if (target_width != width_ || target_height != height_) {
    if (!initialize(target_width, target_height)) {
      result.error = "Failed to resize canvas for DPI scaling";
      return result;
    }
  }

  if (!begin_scene()) {
    result.error = "Failed to begin scene";
    return result;
  }

  // Use exact DPI scale factor to avoid rounding errors from canvas dimensions
  float scale = dpi_scale;

  // Initialize graphics state
  state_ = GraphicsState();
  state_.page_width = page_width;
  state_.page_height = page_height;
  state_.scale = scale;

  current_pdf_ = &pdf;
  current_page_ = &page;
  page.ensure_fonts_loaded(pdf);

  // Preserve glyph bitmap cache across pages — same font/size/glyph
  // produces identical bitmaps, and the FIFO eviction keeps memory bounded.

  const bool has_explicit_progress = static_cast<bool>(options.progress_callback);
  const bool has_progress_callback =
      has_explicit_progress || static_cast<bool>(progress_config_.callback);

  std::vector<std::vector<uint8_t>> decoded_contents;
  decoded_contents.reserve(page.contents.size());
  size_t total_render_objects = 0;

  for (const auto& content_obj : page.contents) {
    Value resolved_obj = content_obj;
    uint32_t obj_num = 0;
    uint16_t gen_num = 0;

    if (content_obj.type == Value::REFERENCE) {
      obj_num = content_obj.ref_object_number;
      gen_num = content_obj.ref_generation_number;
      auto resolved = resolve_reference(pdf, obj_num, gen_num);
      if (!resolved.success) {
        continue;
      }
      resolved_obj = std::move(resolved.value);
    }

    if (resolved_obj.type != Value::STREAM) {
      continue;
    }

    auto decoded_result = decode_stream(pdf, resolved_obj, obj_num, gen_num);
    if (!decoded_result.success) {
      continue;
    }

    if (has_progress_callback) {
      total_render_objects += count_render_objects(decoded_result.data);
      total_render_objects += scan_do_extra_weight(
          std::string_view(
              reinterpret_cast<const char*>(decoded_result.data.data()),
              decoded_result.data.size()),
          pdf, page.resources);
    }
    decoded_contents.push_back(std::move(decoded_result.data));
  }

  begin_progress(has_explicit_progress ? options.progress_callback
                                       : progress_config_.callback,
                 total_render_objects,
                 has_explicit_progress ? options.progress_object_threshold
                                       : progress_config_.object_threshold,
                 has_explicit_progress ? options.progress_percent_step
                                       : progress_config_.percent_step);

  // Draw background. Opaque backgrounds are a full-canvas fill, so seed the
  // target buffer directly and skip both the transparent clear and a scene
  // rectangle. Translucent backgrounds keep the old compositing path.
  if (options.bg_a == 255) {
    std::fill(buffer_.begin(), buffer_.end(),
              pack_argb32(options.bg_r, options.bg_g, options.bg_b,
                          options.bg_a));
    clear_canvas_on_draw_ = false;
  } else {
    clear_canvas_on_draw_ = true;
    draw_rectangle(0, 0, width_, height_, options.bg_r, options.bg_g,
                   options.bg_b, options.bg_a);
  }

  // Process content streams — concatenate per PDF §7.8.2 (streams in a
  // /Contents array are treated as one; state persists, operators can span).
  state_ = GraphicsState();
  state_.page_width = page_width;
  state_.page_height = page_height;
  state_.scale = scale;
  if (!decoded_contents.empty()) {
    size_t total_size = 0;
    for (const auto& c : decoded_contents) total_size += c.size() + 1;
    std::vector<uint8_t> merged;
    merged.reserve(total_size);
    for (const auto& c : decoded_contents) {
      merged.insert(merged.end(), c.begin(), c.end());
      merged.push_back('\n');
    }
    parse_pdf_content(merged);
  }

  // Render page annotations (appearance streams + form-widget defaults).
  render_annotations(pdf, page, page_width, page_height, scale);

  if (!end_scene()) {
    result.error = "Failed to end scene";
    finish_progress();
    current_pdf_ = nullptr;
    current_page_ = nullptr;
    return result;
  }

  finish_progress();

  current_pdf_ = nullptr;
  current_page_ = nullptr;

  if (!result_pixels_enabled_) {
    result.success = true;
    result.width = width_;
    result.height = height_;
    return result;
  }

  // Apply page rotation if specified
  int rotation = static_cast<int>(page.rotate) % 360;
  if (rotation < 0) rotation += 360;
  if (rotation == 90 || rotation == 180 || rotation == 270) {
    int dst_w = 0;
    int dst_h = 0;
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    result.pixels = rotate_rgba_pixels_copy(
        reinterpret_cast<const uint8_t*>(buffer_.data()),
        static_cast<int>(width_), static_cast<int>(height_), rotation,
        &dst_w, &dst_h);
#else
    auto unrotated = get_buffer();
    if (unrotated.success && !unrotated.pixels.empty()) {
      result.pixels = rotate_rgba_pixels_copy(
          unrotated.pixels.data(), static_cast<int>(unrotated.width),
          static_cast<int>(unrotated.height), rotation, &dst_w, &dst_h);
    }
#endif
    result.success = !result.pixels.empty();
    result.width = static_cast<uint32_t>(dst_w);
    result.height = static_cast<uint32_t>(dst_h);
    if (!result.success) result.error = "Failed to rotate render buffer";
    return result;
  }

  return get_buffer();
}

LightVGRenderResult LightVGBackend::render_page(const Pdf& pdf, const Page& page) {
  LightVGRenderResult result;
  LightVGRenderOptions default_options;
  max_function_shading_pixels_ = default_options.max_function_shading_pixels;

  // Reset per-page resource resolution storage so it doesn't grow unbounded.
  lookup_resolved_owned_.clear();
  lookup_font_owned_.clear();
  current_soft_mask_.reset();
  soft_mask_group_cache_.clear();

  if (!initialized_) {
    result.error = "Backend not initialized";
    return result;
  }

  if (!begin_scene()) {
    result.error = "Failed to begin scene";
    return result;
  }

  // Get page dimensions from media_box [left, bottom, right, top]
  double page_width = 612.0;   // Default US Letter
  double page_height = 792.0;
  if (page.media_box.size() >= 4) {
    page_width = page.media_box[2] - page.media_box[0];
    page_height = page.media_box[3] - page.media_box[1];
  }

  // Calculate scale to fit the page into our canvas
  float scale_x = static_cast<float>(width_) / page_width;
  float scale_y = static_cast<float>(height_) / page_height;
  float scale = std::min(scale_x, scale_y);

  // Store PDF and Page references for font access
  current_pdf_ = &pdf;
  current_page_ = &page;

  // Ensure page fonts are loaded
  page.ensure_fonts_loaded(pdf);

  // Preserve glyph bitmap cache across pages — same font/size/glyph
  // produces identical bitmaps, and the FIFO eviction keeps memory bounded.

  const bool has_progress_callback = static_cast<bool>(progress_config_.callback);

  std::vector<std::vector<uint8_t>> decoded_contents;
  decoded_contents.reserve(page.contents.size());
  size_t total_render_objects = has_progress_callback ? page.annotations.size() : 0;

  for (const auto& content_obj : page.contents) {
    Value resolved_obj = content_obj;
    uint32_t obj_num = 0;
    uint16_t gen_num = 0;

    // Resolve reference if needed
    if (content_obj.type == Value::REFERENCE) {
      obj_num = content_obj.ref_object_number;
      gen_num = content_obj.ref_generation_number;
      auto resolved = resolve_reference(pdf, obj_num, gen_num);
      if (!resolved.success) {
        continue;
      }
      resolved_obj = std::move(resolved.value);
    }

    if (resolved_obj.type != Value::STREAM) {
      continue;
    }

    auto decoded_result = decode_stream(pdf, resolved_obj, obj_num, gen_num);
    if (!decoded_result.success) {
      continue;
    }

    if (has_progress_callback) {
      total_render_objects += count_render_objects(decoded_result.data);
      total_render_objects += scan_do_extra_weight(
          std::string_view(
              reinterpret_cast<const char*>(decoded_result.data.data()),
              decoded_result.data.size()),
          pdf, page.resources);
    }
    decoded_contents.push_back(std::move(decoded_result.data));
  }

  begin_progress(progress_config_.callback, total_render_objects,
                 progress_config_.object_threshold,
                 progress_config_.percent_step);

  // Draw white background by seeding the target buffer directly. This avoids
  // a transparent clear plus a full-canvas rectangle fill.
  std::fill(buffer_.begin(), buffer_.end(), pack_argb32(255, 255, 255, 255));
  clear_canvas_on_draw_ = false;

  // Parse and render page content.
  // Per PDF spec §7.8.2, when /Contents is an array, the streams must be
  // treated as a single stream (concatenated with a whitespace separator);
  // state is NOT reset between them, and operators may span boundaries.
  state_ = GraphicsState();
  state_.page_width = page_width;
  state_.page_height = page_height;
  state_.scale = scale;
  if (!decoded_contents.empty()) {
    size_t total_size = 0;
    for (const auto& c : decoded_contents) total_size += c.size() + 1;
    std::vector<uint8_t> merged;
    merged.reserve(total_size);
    for (const auto& c : decoded_contents) {
      merged.insert(merged.end(), c.begin(), c.end());
      merged.push_back('\n');
    }
    parse_pdf_content(merged);
  }

  // Render page annotations (appearance streams + form-widget defaults).
  render_annotations(pdf, page, static_cast<float>(page_width),
                     static_cast<float>(page_height), scale);

  if (!end_scene()) {
    result.error = "Failed to end scene";
    finish_progress();
    return result;
  }

  finish_progress();

  if (!result_pixels_enabled_) {
    result.success = true;
    result.width = width_;
    result.height = height_;
    return result;
  }

  return get_buffer();
}

void LightVGBackend::render_annotations(const Pdf& pdf, const Page& page,
                                        float page_width, float page_height,
                                        float scale) {
  for (const auto& annot : page.annotations) {
    if (!annot) continue;

    // Get annotation rect
    if (annot->rect.size() < 4) continue;
    float ax1 = static_cast<float>(annot->rect[0]) * scale;
    float ay1 = (page_height - static_cast<float>(annot->rect[3])) * scale;
    float ax2 = static_cast<float>(annot->rect[2]) * scale;
    float ay2 = (page_height - static_cast<float>(annot->rect[1])) * scale;
    float aw = ax2 - ax1;
    float ah = ay2 - ay1;

    if (aw <= 0 || ah <= 0) continue;

    // Check for appearance stream
    auto n_it = annot->appearance_streams.find("N");
    if (n_it != annot->appearance_streams.end()) {
      // Has normal appearance - render it as a Form XObject
      Value ap_stream = n_it->second;
      if (ap_stream.type == Value::REFERENCE) {
        auto resolved = resolve_reference(pdf, ap_stream.ref_object_number,
                                           ap_stream.ref_generation_number);
        if (resolved.success) {
          ap_stream = resolved.value;
        }
      }

      if (ap_stream.type == Value::STREAM) {
        auto decoded = decode_stream(pdf, ap_stream);
        if (decoded.success) {
          // The appearance is a Form XObject: its content runs in user space
          // (page conversion applies scale + Y-flip), with a CTM that maps the
          // form's /Matrix-transformed /BBox onto the annotation /Rect.
          GraphicsState saved_state = state_;
          state_ = GraphicsState();
          state_.page_width = page_width;
          state_.page_height = page_height;
          state_.scale = scale;

          GraphicsState::Matrix mtx;
          auto mtx_it = ap_stream.stream.dict.find("Matrix");
          if (mtx_it != ap_stream.stream.dict.end() &&
              mtx_it->second.type == Value::ARRAY &&
              mtx_it->second.array.size() >= 6) {
            mtx.a = static_cast<float>(mtx_it->second.array[0].number);
            mtx.b = static_cast<float>(mtx_it->second.array[1].number);
            mtx.c = static_cast<float>(mtx_it->second.array[2].number);
            mtx.d = static_cast<float>(mtx_it->second.array[3].number);
            mtx.e = static_cast<float>(mtx_it->second.array[4].number);
            mtx.f = static_cast<float>(mtx_it->second.array[5].number);
          }

          auto bbox_it = ap_stream.stream.dict.find("BBox");
          if (bbox_it != ap_stream.stream.dict.end() &&
              bbox_it->second.type == Value::ARRAY &&
              bbox_it->second.array.size() >= 4) {
            double bx0 = bbox_it->second.array[0].number;
            double by0 = bbox_it->second.array[1].number;
            double bx1 = bbox_it->second.array[2].number;
            double by1 = bbox_it->second.array[3].number;
            float cx[4] = {static_cast<float>(bx0), static_cast<float>(bx1),
                           static_cast<float>(bx1), static_cast<float>(bx0)};
            float cy[4] = {static_cast<float>(by0), static_cast<float>(by0),
                           static_cast<float>(by1), static_cast<float>(by1)};
            float tx0 = 1e30f, ty0 = 1e30f, tx1 = -1e30f, ty1 = -1e30f;
            for (int i = 0; i < 4; ++i) {
              float px = cx[i], py = cy[i];
              mtx.transform(px, py);
              tx0 = std::min(tx0, px); ty0 = std::min(ty0, py);
              tx1 = std::max(tx1, px); ty1 = std::max(ty1, py);
            }
            double rw = annot->rect[2] - annot->rect[0];
            double rh = annot->rect[3] - annot->rect[1];
            double bw = tx1 - tx0, bh = ty1 - ty0;
            GraphicsState::Matrix aa;
            aa.a = (bw != 0.0) ? static_cast<float>(rw / bw) : 1.0f;
            aa.d = (bh != 0.0) ? static_cast<float>(rh / bh) : 1.0f;
            aa.e = static_cast<float>(annot->rect[0]) - aa.a * tx0;
            aa.f = static_cast<float>(annot->rect[1]) - aa.d * ty0;
            state_.transform = mtx * aa;
          } else {
            state_.transform.e = static_cast<float>(annot->rect[0]);
            state_.transform.f = static_cast<float>(annot->rect[1]);
          }

          bool pushed_res = false;
          auto res_it = ap_stream.stream.dict.find("Resources");
          if (res_it != ap_stream.stream.dict.end()) {
            Value fr = res_it->second;
            if (fr.type == Value::REFERENCE) {
              auto r = resolve_reference(pdf, fr.ref_object_number, fr.ref_generation_number);
              if (r.success && r.value.type == Value::DICTIONARY) {
                form_resources_stack_.push_back(r.value.dict);
                pushed_res = true;
              }
            } else if (fr.type == Value::DICTIONARY) {
              form_resources_stack_.push_back(fr.dict);
              pushed_res = true;
            }
          }

          bool progress_enabled = progress_.enabled;
          progress_.enabled = false;
          parse_pdf_content(decoded.data);
          progress_.enabled = progress_enabled;

          if (pushed_res) form_resources_stack_.pop_back();
          state_ = saved_state;
          advance_progress();
        }
      }
      continue;
    }

    // No appearance stream - render default appearance based on type
    if (annot->type == AnnotationType::Widget) {
      bool rendered_annotation = false;
      auto* widget = static_cast<WidgetAnnotation*>(annot.get());

      // Draw widget border
      auto shape = lvg::Shape::gen();
      if (shape) {
        shape->appendRect(ax1, ay1, aw, ah, 0, 0);

        // Light gray background for form fields
        shape->fill(245, 245, 245, 255);
        shape->strokeWidth(1.0f);
        shape->strokeFill(128, 128, 128, 255);

        scene_->add(shape);
        rendered_annotation = true;
      }

      // Render field value for text fields
      if (widget->field_type == FieldType::Text && !widget->field_value.empty()) {
        // Draw text value (simple implementation)
        float text_size = std::min(ah * 0.7f, 12.0f * scale);
        float text_x = ax1 + 2 * scale;
        float text_y = ay1 + ah * 0.7f;
        draw_text(text_x, text_y, widget->field_value, text_size, 0, 0, 0, 255);
      }
      // Render checkmark for button fields (checkboxes)
      else if (widget->field_type == FieldType::Button) {
        // Check if the field is checked (value is not "Off")
        if (widget->field_value != "Off" && !widget->field_value.empty()) {
          // Draw checkmark
          auto check = lvg::Shape::gen();
          if (check) {
            float cx = ax1 + aw * 0.5f;
            float cy = ay1 + ah * 0.5f;
            float s = std::min(aw, ah) * 0.35f;

            // Simple checkmark path
            check->moveTo(cx - s, cy);
            check->lineTo(cx - s * 0.3f, cy + s * 0.6f);
            check->lineTo(cx + s, cy - s * 0.5f);

            check->strokeWidth(2.0f * scale);
            check->strokeFill(0, 0, 0, 255);
            check->strokeCap(lvg::StrokeCap::Round);
            check->strokeJoin(lvg::StrokeJoin::Round);

            scene_->add(check);
          }
        }
      }
      if (rendered_annotation) {
        advance_progress();
      }
    }
    // Other annotation types could be rendered here (links, highlights, etc.)
  }
}

bool LightVGBackend::parse_pdf_content(const std::vector<uint8_t>& content_data) {
  // Enhanced PDF content parser with more operators support

  std::string content(content_data.begin(), content_data.end());

  std::vector<std::string> operands;
  std::vector<GraphicsState> state_stack;  // For save/restore state
  std::vector<std::pair<std::string, const BaseFont*>> font_state_stack;
  auto effective_stroke_width_px = [&]() -> float {
    float sx = std::sqrt(state_.transform.a * state_.transform.a +
                         state_.transform.b * state_.transform.b);
    float sy = std::sqrt(state_.transform.c * state_.transform.c +
                         state_.transform.d * state_.transform.d);
    float ctm_scale = 0.5f * (sx + sy);
    if (ctm_scale < 1e-6f) ctm_scale = 1.0f;
    float stroke_width = state_.stroke_width * ctm_scale * state_.scale;
    // PDF hairline (0 w) maps to one device pixel.
    if (state_.stroke_width == 0.0f) stroke_width = 1.0f;
    return stroke_width;
  };
  operands.reserve(16);
  state_stack.reserve(16);
  font_state_stack.reserve(16);
  const size_t estimated_segments =
      std::max<size_t>(16, content_data.size() / 12);
  if (state_.path_commands.capacity() < estimated_segments) {
    state_.path_commands.reserve(estimated_segments);
  }
  if (state_.path_points.capacity() < estimated_segments) {
    state_.path_points.reserve(estimated_segments);
  }

  size_t pos = 0;
  while (pos < content.size()) {
    // Skip whitespace
    while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' ||
           content[pos] == '\r' || content[pos] == '\n')) {
      pos++;
    }
    if (pos >= content.size()) break;

    // Skip comments (% to end of line)
    if (content[pos] == '%') {
      while (pos < content.size() && content[pos] != '\r' && content[pos] != '\n') {
        pos++;
      }
      continue;
    }

    std::string token;

    // Skip stray '>' (e.g. leftover from malformed dict), but preserve valid '>' handled below
    if (content[pos] == '>') {
      pos++;
      continue;
    }

    // Check for dictionary <<...>> or hex string <...>
    if (content[pos] == '<') {
      if (pos + 1 < content.size() && content[pos + 1] == '<') {
        // Dictionary literal — scan with nesting until matching >>
        size_t start = pos;
        pos += 2;
        int depth = 1;
        while (pos < content.size() && depth > 0) {
          if (content[pos] == '<' && pos + 1 < content.size() && content[pos + 1] == '<') {
            depth++;
            pos += 2;
          } else if (content[pos] == '>' && pos + 1 < content.size() && content[pos + 1] == '>') {
            depth--;
            pos += 2;
          } else if (content[pos] == '(') {
            // Skip literal string inside dict
            int pdepth = 1;
            pos++;
            while (pos < content.size() && pdepth > 0) {
              if (content[pos] == '\\' && pos + 1 < content.size()) { pos += 2; continue; }
              if (content[pos] == '(') pdepth++;
              else if (content[pos] == ')') pdepth--;
              pos++;
            }
          } else {
            pos++;
          }
        }
        token = content.substr(start, pos - start);
      } else {
        // Hex string <...>
        size_t end = content.find('>', pos);
        if (end != std::string::npos) {
          token = content.substr(pos, end - pos + 1);
          pos = end + 1;
        } else {
          pos++;
          continue;
        }
      }
    }
    // Check for literal string (...)
    else if (content[pos] == '(') {
      int depth = 1;
      size_t start = pos;
      pos++;
      while (pos < content.size() && depth > 0) {
        // Honor escapes by skipping the char after a backslash. A naive
        // "previous char != backslash" test mis-handles an escaped backslash
        // "\\" before a ")" (e.g. the string "(\\)" for a path separator),
        // treating the ")" as escaped and swallowing the rest of the stream.
        if (content[pos] == '\\') {
          pos += 2;
          continue;
        }
        if (content[pos] == '(') {
          depth++;
        } else if (content[pos] == ')') {
          depth--;
        }
        pos++;
      }
      token = content.substr(start, pos - start);
    }
    // Check for array [...] - skip for now, just collect the bracket
    else if (content[pos] == '[') {
      token = "[";
      pos++;
    }
    else if (content[pos] == ']') {
      token = "]";
      pos++;
    }
    // Regular token
    else {
      size_t start = pos;
      while (pos < content.size() && content[pos] != ' ' && content[pos] != '\t' &&
             content[pos] != '\r' && content[pos] != '\n' && content[pos] != '<' &&
             content[pos] != '>' && content[pos] != '(' && content[pos] != '[' &&
             content[pos] != ']') {
        pos++;
      }
      token = content.substr(start, pos - start);
    }

    if (token.empty()) continue;

    // Check if token is an operator (starts with a letter or special chars)
    bool is_operator = false;
    if (!token.empty()) {
      char first = token[0];
      // Operators start with letters or are special like ' " *
      // But not if it looks like a hex string or literal string
      if (first != '<' && first != '(' && first != '[' && first != ']') {
        if ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
            first == '*' || first == '\'' || first == '"') {
          is_operator = true;
        }
      }
    }

    if (is_operator) {
      // Process operator with accumulated operands
      auto tok1 = [&](char a) { return token_eq1(token, a); };
      auto tok2 = [&](char a, char b) { return token_eq2(token, a, b); };
      auto tok3 = [&](char a, char b, char c) {
        return token_eq3(token, a, b, c);
      };

      // Path construction operators
      if (tok1('m')) {  // moveTo
        if (operands.size() >= 2) {
          float x = nanopdf::stof_or(operands[0]);
          float y = nanopdf::stof_or(operands[1]);

          // Apply CTM then scale to canvas coordinates with Y-flip
          state_.transform.transform(x, y);

          state_.current_x = x * state_.scale;
          state_.current_y = (state_.page_height - y) * state_.scale;
          state_.path_commands.push_back(lvg::PathCommand::MoveTo);
          state_.path_points.push_back({state_.current_x, state_.current_y});
          state_.in_path = true;
        }
      } else if (tok1('l')) {  // lineTo
        if (operands.size() >= 2) {
          float x = nanopdf::stof_or(operands[0]);
          float y = nanopdf::stof_or(operands[1]);

          // Apply CTM then scale to canvas coordinates with Y-flip
          state_.transform.transform(x, y);

          state_.current_x = x * state_.scale;
          state_.current_y = (state_.page_height - y) * state_.scale;
          state_.path_commands.push_back(lvg::PathCommand::LineTo);
          state_.path_points.push_back({state_.current_x, state_.current_y});
        }
      } else if (tok1('c')) {  // curveTo (cubic Bezier)
        if (operands.size() >= 6) {
          float x1 = nanopdf::stof_or(operands[0]);
          float y1 = nanopdf::stof_or(operands[1]);
          float x2 = nanopdf::stof_or(operands[2]);
          float y2 = nanopdf::stof_or(operands[3]);
          float x3 = nanopdf::stof_or(operands[4]);
          float y3 = nanopdf::stof_or(operands[5]);

          // Apply CTM then scale to canvas coordinates with Y-flip
          state_.transform.transform(x1, y1);
          state_.transform.transform(x2, y2);
          state_.transform.transform(x3, y3);

          x1 *= state_.scale; y1 = (state_.page_height - y1) * state_.scale;
          x2 *= state_.scale; y2 = (state_.page_height - y2) * state_.scale;
          x3 *= state_.scale; y3 = (state_.page_height - y3) * state_.scale;

          state_.path_commands.push_back(lvg::PathCommand::CubicTo);
          state_.path_points.push_back({x1, y1});
          state_.path_points.push_back({x2, y2});
          state_.path_points.push_back({x3, y3});
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (tok1('v')) {  // curveTo variant (first control point = current point)
        if (operands.size() >= 4) {
          float x2 = nanopdf::stof_or(operands[0]);
          float y2 = nanopdf::stof_or(operands[1]);
          float x3 = nanopdf::stof_or(operands[2]);
          float y3 = nanopdf::stof_or(operands[3]);

          // Apply CTM then scale to canvas coordinates with Y-flip
          state_.transform.transform(x2, y2);
          state_.transform.transform(x3, y3);

          x2 *= state_.scale; y2 = (state_.page_height - y2) * state_.scale;
          x3 *= state_.scale; y3 = (state_.page_height - y3) * state_.scale;

          state_.path_commands.push_back(lvg::PathCommand::CubicTo);
          state_.path_points.push_back({state_.current_x, state_.current_y});
          state_.path_points.push_back({x2, y2});
          state_.path_points.push_back({x3, y3});
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (tok1('y')) {  // curveTo variant (second control point = end point)
        if (operands.size() >= 4) {
          float x1 = nanopdf::stof_or(operands[0]);
          float y1 = nanopdf::stof_or(operands[1]);
          float x3 = nanopdf::stof_or(operands[2]);
          float y3 = nanopdf::stof_or(operands[3]);

          // Apply CTM then scale to canvas coordinates with Y-flip
          state_.transform.transform(x1, y1);
          state_.transform.transform(x3, y3);

          x1 *= state_.scale; y1 = (state_.page_height - y1) * state_.scale;
          x3 *= state_.scale; y3 = (state_.page_height - y3) * state_.scale;

          state_.path_commands.push_back(lvg::PathCommand::CubicTo);
          state_.path_points.push_back({x1, y1});
          state_.path_points.push_back({x3, y3});
          state_.path_points.push_back({x3, y3});
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (tok2('r', 'e')) {  // rectangle
        if (operands.size() >= 4) {
          float rx = nanopdf::stof_or(operands[0]);
          float ry = nanopdf::stof_or(operands[1]);
          float rw = nanopdf::stof_or(operands[2]);
          float rh = nanopdf::stof_or(operands[3]);

          // Build 4 corners in user space, transform through CTM, scale and Y-flip
          float corners[4][2] = {
            {rx, ry}, {rx + rw, ry}, {rx + rw, ry + rh}, {rx, ry + rh}
          };
          for (int i = 0; i < 4; i++) {
            state_.transform.transform(corners[i][0], corners[i][1]);
            corners[i][0] *= state_.scale;
            corners[i][1] = (state_.page_height - corners[i][1]) * state_.scale;
          }

          // Snap axis-aligned rectangles to pixel grid
          bool is_axis_aligned =
              (std::abs(corners[0][1] - corners[1][1]) < 0.5f) &&
              (std::abs(corners[1][0] - corners[2][0]) < 0.5f);
          if (is_axis_aligned) {
            for (int ci = 0; ci < 4; ci++) {
              corners[ci][0] = std::round(corners[ci][0]);
              corners[ci][1] = std::round(corners[ci][1]);
            }
          }

          // Add rectangle path from transformed corners
          state_.path_commands.push_back(lvg::PathCommand::MoveTo);
          state_.path_points.push_back({corners[0][0], corners[0][1]});
          state_.path_commands.push_back(lvg::PathCommand::LineTo);
          state_.path_points.push_back({corners[1][0], corners[1][1]});
          state_.path_commands.push_back(lvg::PathCommand::LineTo);
          state_.path_points.push_back({corners[2][0], corners[2][1]});
          state_.path_commands.push_back(lvg::PathCommand::LineTo);
          state_.path_points.push_back({corners[3][0], corners[3][1]});
          state_.path_commands.push_back(lvg::PathCommand::Close);
          state_.current_x = corners[0][0];
          state_.current_y = corners[0][1];
          state_.in_path = true;
        }
      } else if (tok1('h')) {  // Close path
        if (state_.in_path) {
          state_.path_commands.push_back(lvg::PathCommand::Close);
        }
      }
      // Path painting operators
      else if (tok1('f') || tok1('F') || tok2('f', '*')) {  // fill (various winding rules)
        if (!state_.path_commands.empty()) {
          auto shape = lvg::Shape::gen();
          if (shape) {
            append_shape_geometry(shape, state_.path_commands, state_.path_points);

            // Set fill rule: f*/F* use even-odd, f/F use non-zero
            if (tok2('f', '*')) {
              shape->fillRule(lvg::FillRule::EvenOdd);
            } else {
              shape->fillRule(lvg::FillRule::NonZero);
            }

            // Check for pattern fill
            if (!state_.fill_pattern.empty()) {
              apply_pattern_fill(shape, state_.fill_pattern, false);
            } else {
              // Apply solid fill with opacity
              uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);
              shape->fill(state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha);
            }
            push_with_clip(shape);
          }
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
          advance_progress();
        }
      } else if (tok1('s')) {  // close and stroke
        if (state_.in_path) {
          state_.path_commands.push_back(lvg::PathCommand::Close);
        }
        // Fall through to stroke
      }
      if (tok1('S') || tok1('s')) {  // stroke
        if (!state_.path_commands.empty()) {
          // Snap thin axis-aligned lines to pixel grid to avoid AA fringe
          float stroke_w_px = state_.stroke_width * state_.scale;
          if (stroke_w_px <= 2.0f && stroke_w_px > 0.0f) {
            bool snap_to_half =
                (static_cast<int>(std::round(stroke_w_px)) % 2 == 1);
            for (size_t i = 0; i + 1 < state_.path_points.size(); i++) {
              auto& p0 = state_.path_points[i];
              auto& p1 = state_.path_points[i + 1];
              // Only snap LineTo segments
              if (i + 1 < state_.path_commands.size() &&
                  state_.path_commands[i + 1] == lvg::PathCommand::LineTo) {
                float dx = std::abs(p1.x - p0.x);
                float dy = std::abs(p1.y - p0.y);
                if (dy < 0.5f && dx > 1.0f) {
                  // Horizontal line: snap Y coordinates
                  float snapped_y = snap_to_half ? std::floor(p0.y) + 0.5f
                                                 : std::round(p0.y);
                  p0.y = snapped_y;
                  p1.y = snapped_y;
                } else if (dx < 0.5f && dy > 1.0f) {
                  // Vertical line: snap X coordinates
                  float snapped_x = snap_to_half ? std::floor(p0.x) + 0.5f
                                                 : std::round(p0.x);
                  p0.x = snapped_x;
                  p1.x = snapped_x;
                }
              }
            }
          }
          // Create stroked shape
          auto shape = lvg::Shape::gen();
          if (shape) {
            append_shape_geometry(shape, state_.path_commands, state_.path_points);
            // Apply stroke style from graphics state
            shape->strokeWidth(effective_stroke_width_px());

            // Check for stroke pattern
            if (!state_.stroke_pattern.empty()) {
              apply_pattern_fill(shape, state_.stroke_pattern, true);
            } else {
              uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
              shape->strokeFill(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha);
            }
            // Set line cap: 0=butt, 1=round, 2=square
            lvg::StrokeCap cap = lvg::StrokeCap::Butt;
            if (state_.line_cap == 1) cap = lvg::StrokeCap::Round;
            else if (state_.line_cap == 2) cap = lvg::StrokeCap::Square;
            shape->strokeCap(cap);
            // Set line join: 0=miter, 1=round, 2=bevel
            lvg::StrokeJoin join = lvg::StrokeJoin::Miter;
            if (state_.line_join == 1) join = lvg::StrokeJoin::Round;
            else if (state_.line_join == 2) join = lvg::StrokeJoin::Bevel;
            shape->strokeJoin(join);
            shape->strokeMiterlimit(state_.miter_limit);
            // Apply dash pattern if set
            if (!state_.dash_pattern.empty()) {
              std::vector<float> scaled_dash;
              for (float d : state_.dash_pattern) {
                scaled_dash.push_back(d * state_.scale);
              }
              shape->strokeDash(scaled_dash.data(), scaled_dash.size(),
                               state_.dash_phase * state_.scale);
            }
            push_with_clip(shape);
          }
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
          advance_progress();
        }
      } else if (tok1('b') || tok2('b', '*')) {  // close, fill and stroke
        if (state_.in_path) {
          state_.path_commands.push_back(lvg::PathCommand::Close);
        }
        if (!state_.path_commands.empty()) {
          // Fill first
          {
            auto fill_shape = lvg::Shape::gen();
            if (fill_shape) {
              append_shape_geometry(fill_shape, state_.path_commands,
                                    state_.path_points);
              // Set fill rule
              if (tok2('b', '*')) {
                fill_shape->fillRule(lvg::FillRule::EvenOdd);
              } else {
                fill_shape->fillRule(lvg::FillRule::NonZero);
              }
              // Check for pattern fill
              if (!state_.fill_pattern.empty()) {
                apply_pattern_fill(fill_shape, state_.fill_pattern, false);
              } else {
                uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);
                fill_shape->fill(state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha);
              }
              push_with_clip(fill_shape);
            }
          }

          // Stroke with style
          auto shape = lvg::Shape::gen();
          if (shape) {
            append_shape_geometry(shape, state_.path_commands, state_.path_points);
            shape->strokeWidth(effective_stroke_width_px());
            // Check for stroke pattern
            if (!state_.stroke_pattern.empty()) {
              apply_pattern_fill(shape, state_.stroke_pattern, true);
            } else {
              uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
              shape->strokeFill(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha);
            }
            lvg::StrokeCap cap = lvg::StrokeCap::Butt;
            if (state_.line_cap == 1) cap = lvg::StrokeCap::Round;
            else if (state_.line_cap == 2) cap = lvg::StrokeCap::Square;
            shape->strokeCap(cap);
            lvg::StrokeJoin join = lvg::StrokeJoin::Miter;
            if (state_.line_join == 1) join = lvg::StrokeJoin::Round;
            else if (state_.line_join == 2) join = lvg::StrokeJoin::Bevel;
            shape->strokeJoin(join);
            shape->strokeMiterlimit(state_.miter_limit);
            // Apply dash pattern if set
            if (!state_.dash_pattern.empty()) {
              std::vector<float> scaled_dash;
              for (float d : state_.dash_pattern) {
                scaled_dash.push_back(d * state_.scale);
              }
              shape->strokeDash(scaled_dash.data(), scaled_dash.size(),
                               state_.dash_phase * state_.scale);
            }
            push_with_clip(shape);
          }
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
          advance_progress();
        }
      } else if (tok1('B') || tok2('B', '*')) {  // fill and stroke
        if (!state_.path_commands.empty()) {
          // Fill first
          {
            auto fill_shape = lvg::Shape::gen();
            if (fill_shape) {
              append_shape_geometry(fill_shape, state_.path_commands,
                                    state_.path_points);
              // Set fill rule
              if (tok2('B', '*')) {
                fill_shape->fillRule(lvg::FillRule::EvenOdd);
              } else {
                fill_shape->fillRule(lvg::FillRule::NonZero);
              }
              // Check for pattern fill
              if (!state_.fill_pattern.empty()) {
                apply_pattern_fill(fill_shape, state_.fill_pattern, false);
              } else {
                uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);
                fill_shape->fill(state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha);
              }
              push_with_clip(fill_shape);
            }
          }

          // Stroke with style
          auto shape = lvg::Shape::gen();
          if (shape) {
            append_shape_geometry(shape, state_.path_commands, state_.path_points);
            shape->strokeWidth(effective_stroke_width_px());
            // Check for stroke pattern
            if (!state_.stroke_pattern.empty()) {
              apply_pattern_fill(shape, state_.stroke_pattern, true);
            } else {
              uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
              shape->strokeFill(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha);
            }
            lvg::StrokeCap cap = lvg::StrokeCap::Butt;
            if (state_.line_cap == 1) cap = lvg::StrokeCap::Round;
            else if (state_.line_cap == 2) cap = lvg::StrokeCap::Square;
            shape->strokeCap(cap);
            lvg::StrokeJoin join = lvg::StrokeJoin::Miter;
            if (state_.line_join == 1) join = lvg::StrokeJoin::Round;
            else if (state_.line_join == 2) join = lvg::StrokeJoin::Bevel;
            shape->strokeJoin(join);
            shape->strokeMiterlimit(state_.miter_limit);
            // Apply dash pattern if set
            if (!state_.dash_pattern.empty()) {
              std::vector<float> scaled_dash;
              for (float d : state_.dash_pattern) {
                scaled_dash.push_back(d * state_.scale);
              }
              shape->strokeDash(scaled_dash.data(), scaled_dash.size(),
                               state_.dash_phase * state_.scale);
            }
            push_with_clip(shape);
          }
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
          advance_progress();
        }
      } else if (tok1('n')) {  // End path without fill or stroke
        state_.path_commands.clear();
        state_.path_points.clear();
        state_.in_path = false;
      }
      // Clipping path operators
      else if (tok1('W') || tok2('W', '*')) {
        // Set clipping path using current path
        // W = non-zero winding rule, W* = even-odd rule
        // The path will be used by subsequent paint operations
        if (!state_.path_commands.empty()) {
          // If we already have a clip, we need to intersect the new clip with it
          // For simplicity, we replace the clip (proper intersection is complex)
          state_.has_clip = true;
          state_.clip_even_odd = tok2('W', '*');
          state_.clip_commands = state_.path_commands;
          state_.clip_points = state_.path_points;
        }
      }
      // Color operators
      else if (tok2('r', 'g')) {  // Set RGB fill color
        if (operands.size() >= 3) {
          state_.fill_r = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
          state_.fill_g = static_cast<uint8_t>(nanopdf::stof_or(operands[1]) * 255);
          state_.fill_b = static_cast<uint8_t>(nanopdf::stof_or(operands[2]) * 255);
        }
      } else if (tok2('R', 'G')) {  // Set RGB stroke color
        if (operands.size() >= 3) {
          state_.stroke_r = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
          state_.stroke_g = static_cast<uint8_t>(nanopdf::stof_or(operands[1]) * 255);
          state_.stroke_b = static_cast<uint8_t>(nanopdf::stof_or(operands[2]) * 255);
        }
      } else if (tok1('g')) {  // Set gray fill color
        if (operands.size() >= 1) {
          uint8_t gray = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
          state_.fill_r = state_.fill_g = state_.fill_b = gray;
        }
      } else if (tok1('G')) {  // Set gray stroke color
        if (operands.size() >= 1) {
          uint8_t gray = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
          state_.stroke_r = state_.stroke_g = state_.stroke_b = gray;
        }
      } else if (tok1('k')) {  // Set CMYK fill color
        if (operands.size() >= 4) {
          cmyk_to_rgb(nanopdf::stof_or(operands[0]), nanopdf::stof_or(operands[1]),
                      nanopdf::stof_or(operands[2]), nanopdf::stof_or(operands[3]),
                      state_.fill_r, state_.fill_g, state_.fill_b);
        }
      } else if (tok1('K')) {  // Set CMYK stroke color
        if (operands.size() >= 4) {
          cmyk_to_rgb(nanopdf::stof_or(operands[0]), nanopdf::stof_or(operands[1]),
                      nanopdf::stof_or(operands[2]), nanopdf::stof_or(operands[3]),
                      state_.stroke_r, state_.stroke_g, state_.stroke_b);
        }
      }
      // Color space operators
      else if (tok2('c', 's')) {  // Set non-stroking color space
        if (operands.size() >= 1) {
          std::string cs_name = operands[0];
          if (!cs_name.empty() && cs_name[0] == '/') {
            cs_name = cs_name.substr(1);
          }
          state_.fill_color_space = cs_name;
          state_.fill_pattern.clear();  // Clear any previous pattern
          NANOPDF_LOG_TRACE("LightVG", "cs: set fill color space to '%s'", cs_name.c_str());
        }
      } else if (tok2('C', 'S')) {  // Set stroking color space
        if (operands.size() >= 1) {
          std::string cs_name = operands[0];
          if (!cs_name.empty() && cs_name[0] == '/') {
            cs_name = cs_name.substr(1);
          }
          state_.stroke_color_space = cs_name;
          state_.stroke_pattern.clear();  // Clear any previous pattern
          NANOPDF_LOG_TRACE("LightVG", "CS: set stroke color space to '%s'", cs_name.c_str());
        }
      } else if (tok2('s', 'c') || tok3('s', 'c', 'n')) {  // Set non-stroking color
        // Check if last operand is a pattern name (starts with /)
        bool is_pattern = false;
        if (!operands.empty()) {
          const std::string& last = operands.back();
          if (!last.empty() && last[0] == '/') {
            // This is a pattern name
            std::string pattern_name = last.substr(1);
            state_.fill_pattern = pattern_name;
            is_pattern = true;
            NANOPDF_LOG_TRACE("LightVG", "scn: set fill pattern to '%s'", pattern_name.c_str());
          }
        }

        if (!is_pattern) {
          state_.fill_pattern.clear();  // Not using a pattern
          // Color values depend on the current color space
          if (operands.size() >= 3) {
            state_.fill_r = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
            state_.fill_g = static_cast<uint8_t>(nanopdf::stof_or(operands[1]) * 255);
            state_.fill_b = static_cast<uint8_t>(nanopdf::stof_or(operands[2]) * 255);
          } else if (operands.size() >= 1) {
            // Grayscale
            uint8_t gray = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
            state_.fill_r = state_.fill_g = state_.fill_b = gray;
          }
        }
      } else if (tok2('S', 'C') || tok3('S', 'C', 'N')) {  // Set stroking color
        // Check if last operand is a pattern name (starts with /)
        bool is_pattern = false;
        if (!operands.empty()) {
          const std::string& last = operands.back();
          if (!last.empty() && last[0] == '/') {
            // This is a pattern name
            std::string pattern_name = last.substr(1);
            state_.stroke_pattern = pattern_name;
            is_pattern = true;
            NANOPDF_LOG_TRACE("LightVG", "SCN: set stroke pattern to '%s'", pattern_name.c_str());
          }
        }

        if (!is_pattern) {
          state_.stroke_pattern.clear();  // Not using a pattern
          // Color values depend on the current color space
          if (operands.size() >= 3) {
            state_.stroke_r = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
            state_.stroke_g = static_cast<uint8_t>(nanopdf::stof_or(operands[1]) * 255);
            state_.stroke_b = static_cast<uint8_t>(nanopdf::stof_or(operands[2]) * 255);
          } else if (operands.size() >= 1) {
            // Grayscale
            uint8_t gray = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
            state_.stroke_r = state_.stroke_g = state_.stroke_b = gray;
          }
        }
      }
      // Line style operators
      else if (tok1('w')) {  // Set line width
        if (operands.size() >= 1) {
          state_.stroke_width = nanopdf::stof_or(operands[0]);
        }
      } else if (tok1('J')) {  // Set line cap style
        if (operands.size() >= 1) {
          state_.line_cap = nanopdf::stoi_or(operands[0]);
        }
      } else if (tok1('j')) {  // Set line join style
        if (operands.size() >= 1) {
          state_.line_join = nanopdf::stoi_or(operands[0]);
        }
      } else if (tok1('M')) {  // Set miter limit
        if (operands.size() >= 1) {
          state_.miter_limit = nanopdf::stof_or(operands[0]);
        }
      } else if (tok1('d')) {  // Set dash pattern
        // Dash patterns: [array] phase
        // operands should contain: "[", values..., "]", phase
        state_.dash_pattern.clear();
        state_.dash_phase = 0.0f;

        bool in_array = false;
        for (size_t i = 0; i < operands.size(); i++) {
          if (operands[i] == "[") {
            in_array = true;
          } else if (operands[i] == "]") {
            in_array = false;
          } else if (in_array) {
            state_.dash_pattern.push_back(nanopdf::stof_or(operands[i]));
          } else if (!in_array && i == operands.size() - 1) {
            // Last operand after "]" is the phase
            state_.dash_phase = nanopdf::stof_or(operands[i]);
          }
        }
      } else if (tok2('r', 'i')) {  // Set rendering intent
        if (!operands.empty()) {
          state_.rendering_intent = operands[0];
        }
      } else if (tok1('i')) {  // Set flatness tolerance
        if (!operands.empty()) {
          state_.flatness = nanopdf::stof_or(operands[0]);
        }
      }
      // Graphics state operators
      else if (tok1('q')) {  // Save graphics state
        state_stack.push_back(state_);
        font_state_stack.emplace_back(current_font_name_, current_font_);
      } else if (tok1('Q')) {  // Restore graphics state
        if (!state_stack.empty()) {
          state_ = state_stack.back();
          state_stack.pop_back();
          current_soft_mask_.reset();
        }
        if (!font_state_stack.empty()) {
          current_font_name_ = font_state_stack.back().first;
          current_font_ = font_state_stack.back().second;
          font_state_stack.pop_back();
        }
      }
      // Graphics state dictionary (gs operator)
      else if (tok2('g', 's')) {
        if (!operands.empty()) apply_extgstate(operands[0]);
      }
      // Transformation matrix operators
      else if (tok2('c', 'm')) {  // Concatenate matrix
        if (operands.size() >= 6) {
          GraphicsState::Matrix m;
          m.a = nanopdf::stof_or(operands[0]);
          m.b = nanopdf::stof_or(operands[1]);
          m.c = nanopdf::stof_or(operands[2]);
          m.d = nanopdf::stof_or(operands[3]);
          m.e = nanopdf::stof_or(operands[4]);
          m.f = nanopdf::stof_or(operands[5]);
          // Per PDF spec, cm concatenates as: CTM' = M × CTM_current
          // M transforms from new user space to previous user space
          state_.transform = m * state_.transform;
        }
      }
      // Text operators
      else if (tok2('B', 'T')) {  // Begin text
        state_.in_text_block = true;
        state_.text_matrix.reset();
        state_.text_line_matrix.reset();
      } else if (tok2('E', 'T')) {  // End text
        state_.in_text_block = false;

        // If text clipping was active, apply accumulated text path to clipping
        if (state_.text_clip_active && !state_.text_clip_commands.empty()) {
          // Merge text clip path into main clip path
          state_.clip_commands.insert(state_.clip_commands.end(),
                                       state_.text_clip_commands.begin(),
                                       state_.text_clip_commands.end());
          state_.clip_points.insert(state_.clip_points.end(),
                                     state_.text_clip_points.begin(),
                                     state_.text_clip_points.end());
          state_.has_clip = true;

          // Clear text clip state
          state_.text_clip_commands.clear();
          state_.text_clip_points.clear();
          state_.text_clip_active = false;
        }
      } else if (tok2('T', 'd')) {  // Move text position
        if (operands.size() >= 2 && state_.in_text_block) {
          float tx = nanopdf::stof_or(operands[0]);
          float ty = nanopdf::stof_or(operands[1]);
          // Td translates from the text LINE matrix (not current text matrix)
          // per PDF spec: text_line_matrix = Translate(tx,ty) * text_line_matrix
          state_.text_line_matrix.e += tx * state_.text_line_matrix.a + ty * state_.text_line_matrix.c;
          state_.text_line_matrix.f += tx * state_.text_line_matrix.b + ty * state_.text_line_matrix.d;
          state_.text_matrix = state_.text_line_matrix;
        }
      } else if (tok2('T', 'D')) {  // Move text position and set leading
        if (operands.size() >= 2 && state_.in_text_block) {
          float tx = nanopdf::stof_or(operands[0]);
          float ty = nanopdf::stof_or(operands[1]);
          state_.text_leading = -ty;  // TL = -ty
          // Same as Td: translate from text line matrix
          state_.text_line_matrix.e += tx * state_.text_line_matrix.a + ty * state_.text_line_matrix.c;
          state_.text_line_matrix.f += tx * state_.text_line_matrix.b + ty * state_.text_line_matrix.d;
          state_.text_matrix = state_.text_line_matrix;
        }
      } else if (tok2('T', 'm')) {  // Set text matrix
        if (operands.size() >= 6 && state_.in_text_block) {
          // Text matrix [a b c d e f]
          state_.text_matrix.a = nanopdf::stof_or(operands[0]);
          state_.text_matrix.b = nanopdf::stof_or(operands[1]);
          state_.text_matrix.c = nanopdf::stof_or(operands[2]);
          state_.text_matrix.d = nanopdf::stof_or(operands[3]);
          state_.text_matrix.e = nanopdf::stof_or(operands[4]);
          state_.text_matrix.f = nanopdf::stof_or(operands[5]);
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (tok2('T', '*')) {  // Move to start of next line
        if (state_.in_text_block) {
          // Equivalent to: 0 -TL Td
          float leading = state_.text_leading;
          if (leading == 0) leading = state_.font_size;
          float ty = -leading;
          // Apply as Td: translate from text line matrix, scaled by matrix
          state_.text_line_matrix.e += ty * state_.text_line_matrix.c;
          state_.text_line_matrix.f += ty * state_.text_line_matrix.d;
          state_.text_matrix = state_.text_line_matrix;
        }
      } else if (tok2('T', 'L')) {  // Set text leading
        if (operands.size() >= 1) {
          state_.text_leading = nanopdf::stof_or(operands[0]);
        }
      } else if (tok2('T', 'c')) {  // Set character spacing
        if (operands.size() >= 1) {
          state_.char_spacing = nanopdf::stof_or(operands[0]);
        }
      } else if (tok2('T', 'w')) {  // Set word spacing
        if (operands.size() >= 1) {
          state_.word_spacing = nanopdf::stof_or(operands[0]);
        }
      } else if (tok2('T', 'z')) {  // Set horizontal scaling
        if (operands.size() >= 1) {
          state_.horiz_scaling = nanopdf::stof_or(operands[0]);
        }
      } else if (tok2('T', 's')) {  // Set text rise
        if (operands.size() >= 1) {
          state_.text_rise = nanopdf::stof_or(operands[0]);
        }
      } else if (tok2('T', 'r')) {  // Set text rendering mode
        // 0 = Fill, 1 = Stroke, 2 = Fill+Stroke, 3 = Invisible
        // 4 = Fill+Clip, 5 = Stroke+Clip, 6 = Fill+Stroke+Clip, 7 = Clip only
        if (operands.size() >= 1) {
          state_.text_render_mode = nanopdf::stoi_or(operands[0]);
          NANOPDF_LOG_TRACE("LightVG", "Tr: set text rendering mode to %d", state_.text_render_mode);
        }
      } else if (tok2('T', 'f')) {  // Set font and size
        if (operands.size() >= 2) {
          // operands[0] is font name (with leading /), operands[1] is size
          std::string font_name = operands[0];
          if (!font_name.empty() && font_name[0] == '/') {
            font_name = font_name.substr(1);  // Remove leading /
          }
          state_.font_size = nanopdf::stof_or(operands[operands.size() - 1]);

          // Try to load the font from page resources first, then from the
          // active Form/pattern resource stack.
          current_font_name_ = font_name;
          current_font_ = nullptr;
          if (current_pdf_ && current_page_) {
            auto font_it = current_page_->fonts.find(font_name);
            if (font_it != current_page_->fonts.end()) {
              current_font_ = font_it->second.get();
              // Key the font/glyph caches by stable per-document font identity
              // rather than the page-local resource name, which collides across
              // pages (the same name can name different embedded fonts).
              std::string key = font_cache_key(current_font_);
              current_font_name_ = key.empty() ? font_name : key;
              load_font(*current_pdf_, current_font_name_, current_font_);
            } else {
              const Value* font_value = lookup_resource("Font", font_name);
              Value resolved_font;
              if (font_value) {
                resolved_font = *font_value;
                if (resolved_font.type == Value::REFERENCE) {
                  auto resolved = resolve_reference(*current_pdf_,
                                                    resolved_font.ref_object_number,
                                                    resolved_font.ref_generation_number);
                  if (resolved.success) resolved_font = std::move(resolved.value);
                }
              }
              if (resolved_font.type == Value::DICTIONARY) {
                auto parsed_font = Pdf::parse_font(*current_pdf_, resolved_font);
                if (parsed_font) {
                  lookup_font_owned_.push_back(std::move(parsed_font));
                  current_font_ = lookup_font_owned_.back().get();
                  // Prefer stable font identity; fall back to a resource-scoped
                  // name only when the font has no identifiable program/name.
                  std::string key = font_cache_key(current_font_);
                  current_font_name_ =
                      key.empty() ? (font_name + "#resource" +
                                     std::to_string(lookup_font_owned_.size()))
                                  : key;
                  load_font(*current_pdf_, current_font_name_, current_font_);
                }
              }
              if (!current_font_) {
                // Font not in available resources — attempt fallback using name hint
                NANOPDF_LOG_DEBUG("LightVG", "Font '%s' not in resources, trying fallback", font_name.c_str());
                load_fallback_font_with_hint(font_name, nullptr);
              }
            }
          }
        }
      } else if (tok2('T', 'j')) {  // Show text
        if (operands.size() >= 1 && state_.in_text_block) {
          // Remove parentheses or angle brackets from text string
          std::string text = operands[0];
          bool was_literal = !text.empty() && text[0] == '(' && text.back() == ')';
          if (was_literal) {
            text = decode_pdf_literal_string(text.substr(1, text.length() - 2));
          } else if (!text.empty() && text[0] == '<' && text.back() == '>') {
            // Hex string - decode hex pairs to bytes
            std::string hex_str = text.substr(1, text.length() - 2);
            text.clear();
            if (hex_str.size() % 2 == 1) hex_str.push_back('0');
            for (size_t i = 0; i + 1 < hex_str.length(); i += 2) {
              char hex_byte[3] = {hex_str[i], hex_str[i+1], '\0'};
              int byte_val = static_cast<int>(strtol(hex_byte, nullptr, 16));
              text += static_cast<char>(byte_val);
            }
          }

          // Get text position from text matrix and apply CTM
          float text_x = state_.text_matrix.e +
                         state_.text_rise * state_.text_matrix.c;
          float text_y = state_.text_matrix.f +
                         state_.text_rise * state_.text_matrix.d;
          state_.transform.transform(text_x, text_y);

          // Transform from PDF coordinates to canvas coordinates
          float canvas_x = text_x * state_.scale;
          float canvas_y = (state_.page_height - text_y) * state_.scale;

          // Calculate effective font size (considering text matrix scale)
          // Use sqrt(a²+b²) to handle rotated text matrices (e.g., [0 -8 8 0])
          float font_scale = std::sqrt(state_.text_matrix.a * state_.text_matrix.a +
                                       state_.text_matrix.b * state_.text_matrix.b);
          if (font_scale < 0.01f) font_scale = std::abs(state_.text_matrix.d);
          if (font_scale < 0.01f) font_scale = 1.0f;
          // Also account for CTM scaling
          float ctm_scale = std::sqrt(state_.transform.a * state_.transform.a +
                                      state_.transform.b * state_.transform.b);
          if (ctm_scale < 0.01f) ctm_scale = 1.0f;
          float scaled_size = state_.font_size * font_scale * ctm_scale * state_.scale;

          draw_text(canvas_x, canvas_y, text, scaled_size,
                   state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a);

          // Use pre-computed vertical flag from Type0Font
          auto* type0_font_v = as_type0_font(current_font_);
          bool is_vertical_v = type0_font_v ? type0_font_v->is_vertical : false;

          if (is_vertical_v) {
            // Vertical: advance along y axis (c/d components of text matrix)
            float text_advance = calculate_vertical_advance(text, state_.font_size);
            state_.text_matrix.e += (-text_advance) * state_.text_matrix.c;
            state_.text_matrix.f += (-text_advance) * state_.text_matrix.d;
          } else {
            // Horizontal: advance along x axis (a/b components of text matrix)
            float text_advance =
                calculate_text_advance_draw_model(text, state_.font_size);
            text_advance *= state_.horiz_scaling / 100.0f;
            state_.text_matrix.e += text_advance * state_.text_matrix.a;
            state_.text_matrix.f += text_advance * state_.text_matrix.b;
          }
          advance_progress();
        }
      } else if (tok2('T', 'J')) {  // Show text with positioning array
        // TJ takes an array of strings and positioning adjustments
        if (state_.in_text_block) {
          // Use pre-computed vertical flag from Type0Font
          auto* type0_font_tj = as_type0_font(current_font_);
          bool is_vertical_tj = type0_font_tj ? type0_font_tj->is_vertical : false;

          for (const auto& item : operands) {
            if (!item.empty() && (item[0] == '(' || item[0] == '<')) {
              // Text string element
              std::string text = item;
              bool was_literal = (text[0] == '(' && text.back() == ')');
              if (was_literal) {
                text = decode_pdf_literal_string(text.substr(1, text.length() - 2));
              } else if (text[0] == '<' && text.back() == '>') {
                // Hex string - decode hex pairs to bytes
                std::string hex_str = text.substr(1, text.length() - 2);
                text.clear();
                // PDF spec: odd-length hex pads a trailing 0.
                if (hex_str.size() % 2 == 1) hex_str.push_back('0');
                for (size_t i = 0; i + 1 < hex_str.length(); i += 2) {
                  char hex_byte[3] = {hex_str[i], hex_str[i+1], '\0'};
                  int byte_val = static_cast<int>(strtol(hex_byte, nullptr, 16));
                  text += static_cast<char>(byte_val);
                }
              }
              if (!text.empty()) {
                float text_x = state_.text_matrix.e +
                               state_.text_rise * state_.text_matrix.c;
                float text_y = state_.text_matrix.f +
                               state_.text_rise * state_.text_matrix.d;
                state_.transform.transform(text_x, text_y);
                float canvas_x = text_x * state_.scale;
                float canvas_y = (state_.page_height - text_y) * state_.scale;
                float font_scale = std::sqrt(state_.text_matrix.a * state_.text_matrix.a +
                                             state_.text_matrix.b * state_.text_matrix.b);
                if (font_scale < 0.01f) font_scale = std::abs(state_.text_matrix.d);
                if (font_scale < 0.01f) font_scale = 1.0f;
                float ctm_scale = std::sqrt(state_.transform.a * state_.transform.a +
                                            state_.transform.b * state_.transform.b);
                if (ctm_scale < 0.01f) ctm_scale = 1.0f;
                float scaled_size = state_.font_size * font_scale * ctm_scale * state_.scale;

                in_tj_text_draw_ = true;
                draw_text(canvas_x, canvas_y, text, scaled_size,
                         state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a);
                in_tj_text_draw_ = false;

                if (is_vertical_tj) {
                  float text_advance = calculate_vertical_advance(text, state_.font_size);
                  state_.text_matrix.e += (-text_advance) * state_.text_matrix.c;
                  state_.text_matrix.f += (-text_advance) * state_.text_matrix.d;
                } else {
                  float text_advance =
                      calculate_text_advance_draw_model(text, state_.font_size);
                  text_advance *= state_.horiz_scaling / 100.0f;
                  state_.text_matrix.e += text_advance * state_.text_matrix.a;
                  state_.text_matrix.f += text_advance * state_.text_matrix.b;
                }
              }
            } else if (!item.empty() && item[0] != '[' && item[0] != ']') {
              // Numeric positioning adjustment
              // Positive values move left, negative move right (in thousandths of em)
              float adjustment = 0.0f;
              if (parse_float(item, &adjustment)) {
                if (is_vertical_tj) {
                  // Vertical: numeric adjustment displaces along y axis
                  float ty = -adjustment * state_.font_size / 1000.0f;
                  state_.text_matrix.e += ty * state_.text_matrix.c;
                  state_.text_matrix.f += ty * state_.text_matrix.d;
                } else {
                  // Horizontal: numeric adjustment displaces along x axis
                  float tx = -adjustment * state_.font_size / 1000.0f *
                             (state_.horiz_scaling / 100.0f);
                  state_.text_matrix.e += tx * state_.text_matrix.a;
                  state_.text_matrix.f += tx * state_.text_matrix.b;
                }
              }
            }
          }
          advance_progress();
        }
      } else if (tok1('\'') || tok1('"')) {  // Move to next line and show text
        if (operands.size() >= 1 && state_.in_text_block) {
          // Move to next line (T*)
          float ty = -state_.text_leading;
          if (ty == 0) ty = -state_.font_size;
          state_.text_matrix.e += ty * state_.text_matrix.c;
          state_.text_matrix.f += ty * state_.text_matrix.d;
          state_.text_line_matrix = state_.text_matrix;

          // For " operator, first two operands are word and char spacing
          size_t text_idx = operands.size() - 1;
          if (tok1('"') && operands.size() >= 3) {
            state_.word_spacing = nanopdf::stof_or(operands[0]);
            state_.char_spacing = nanopdf::stof_or(operands[1]);
          }

          std::string text = operands[text_idx];
          if (!text.empty() && text[0] == '(' && text.back() == ')') {
            text = decode_pdf_literal_string(text.substr(1, text.length() - 2));
          } else if (!text.empty() && text[0] == '<' && text.back() == '>') {
            std::string hex_str = text.substr(1, text.length() - 2);
            text.clear();
            for (size_t i = 0; i + 1 < hex_str.length(); i += 2) {
              char hex_byte[3] = {hex_str[i], hex_str[i+1], '\0'};
              int byte_val = static_cast<int>(strtol(hex_byte, nullptr, 16));
              text += static_cast<char>(byte_val);
            }
          }

          // Get text position from text matrix and apply CTM
          float text_x = state_.text_matrix.e +
                         state_.text_rise * state_.text_matrix.c;
          float text_y = state_.text_matrix.f +
                         state_.text_rise * state_.text_matrix.d;
          state_.transform.transform(text_x, text_y);
          float canvas_x = text_x * state_.scale;
          float canvas_y = (state_.page_height - text_y) * state_.scale;
          float font_scale = std::sqrt(state_.text_matrix.a * state_.text_matrix.a +
                                       state_.text_matrix.b * state_.text_matrix.b);
          if (font_scale < 0.01f) font_scale = std::abs(state_.text_matrix.d);
          if (font_scale < 0.01f) font_scale = 1.0f;
          // Also account for CTM scaling
          float ctm_scale = std::sqrt(state_.transform.a * state_.transform.a +
                                      state_.transform.b * state_.transform.b);
          if (ctm_scale < 0.01f) ctm_scale = 1.0f;
          float scaled_size = state_.font_size * font_scale * ctm_scale * state_.scale;

          draw_text(canvas_x, canvas_y, text, scaled_size,
                   state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a);

          // Use pre-computed vertical flag from Type0Font
          auto* type0_font_q = as_type0_font(current_font_);
          bool is_vertical_q = type0_font_q ? type0_font_q->is_vertical : false;

          if (is_vertical_q) {
            float text_advance = calculate_vertical_advance(text, state_.font_size);
            state_.text_matrix.e += (-text_advance) * state_.text_matrix.c;
            state_.text_matrix.f += (-text_advance) * state_.text_matrix.d;
          } else {
            float text_advance =
                calculate_text_advance_draw_model(text, state_.font_size);
            text_advance *= state_.horiz_scaling / 100.0f;
            state_.text_matrix.e += text_advance * state_.text_matrix.a;
            state_.text_matrix.f += text_advance * state_.text_matrix.b;
          }
          advance_progress();
        }
      }
      // Inline image (BI ... ID data EI)
      else if (tok2('B', 'I')) {
        parse_inline_image(content, pos);
        advance_progress();
        operands.clear();
        continue;  // Skip normal operand clearing
      }
      // XObject (Do operator for images/forms)
      else if (tok2('D', 'o')) {  // Paint XObject
        if (operands.size() >= 1 && current_pdf_ && current_page_) {
          bool rendered_xobject = false;
          std::string xobj_name = operands[0];
          // Remove leading '/' if present
          if (!xobj_name.empty() && xobj_name[0] == '/') {
            xobj_name = xobj_name.substr(1);
          }

          NANOPDF_LOG_DEBUG("LightVG", "Do operator: looking up XObject '%s'", xobj_name.c_str());

          // Resolve through the form/tiling resources stack first (so tile
          // patterns and Form XObjects see their own XObject dicts), then
          // fall back to page resources. lookup_resource handles both.
          const Value* entry_v = lookup_resource("XObject", xobj_name);
          if (entry_v) {
            Value xobj_value;
            uint32_t xobj_num = 0;
            uint16_t xobj_gen = 0;
            if (entry_v->type == Value::REFERENCE) {
              xobj_num = entry_v->ref_object_number;
              xobj_gen = entry_v->ref_generation_number;
              ResolvedObject resolved = resolve_reference(*current_pdf_, xobj_num, xobj_gen);
              if (resolved.success) xobj_value = std::move(resolved.value);
            } else {
              xobj_value = *entry_v;
            }

            if (xobj_value.type == Value::STREAM) {
              auto subtype_it = xobj_value.stream.dict.find("Subtype");
              if (subtype_it != xobj_value.stream.dict.end() &&
                  subtype_it->second.type == Value::NAME) {
                if (subtype_it->second.name == "Image") {
                  // Cache key: same (obj_num, obj_gen) always produces
                  // the same decoded + color-converted pixels.
                  std::string img_cache_key = "img:" + std::to_string(xobj_num) + ":" +
                                              std::to_string(xobj_gen);
                  RenderCacheEntry img_cached;
                  int img_w = 0, img_h = 0;
                  if (xobj_num != 0 &&
                      RenderCache::instance().find(img_cache_key, img_cached) &&
                      img_cached.width > 0 && img_cached.height > 0) {
                    // Cache hit: use pre-converted ARGB pixels directly.
                    ImageXObject image;
                    image.width = static_cast<int>(img_cached.width);
                    image.height = static_cast<int>(img_cached.height);
                    img_w = image.width;
                    img_h = image.height;
                    image.data = std::move(img_cached.data);
                    // Mark as cached so draw_image skips conversion.
                    image.color_space.type = ColorSpaceType::CacheARGB;
                    draw_image(image, state_.transform.e, state_.transform.f,
                               state_.transform.a, state_.transform.d,
                               state_.fill_r, state_.fill_g, state_.fill_b,
                               &state_.transform);
                  } else {
                    ImageParseOptions parse_options;
                    parse_options.keep_raw_data = false;
                    parse_options.cache_decoded_stream = false;
                    ImageXObject image = parse_image_xobject(*current_pdf_, xobj_value,
                                                             xobj_num, xobj_gen,
                                                             parse_options);
                    img_w = image.width;
                    img_h = image.height;
                    const size_t converted_bytes =
                        (img_w > 0 && img_h > 0)
                            ? static_cast<size_t>(img_w) *
                                  static_cast<size_t>(img_h) * sizeof(uint32_t)
                            : 0;
                    const bool cache_converted_argb =
                        xobj_num != 0 && converted_bytes > 0 &&
                        converted_bytes <= kMaxCachedArgbImageBytes;
                    draw_image(image, state_.transform.e, state_.transform.f,
                               state_.transform.a, state_.transform.d,
                               state_.fill_r, state_.fill_g, state_.fill_b,
                               &state_.transform, cache_converted_argb);
                    // Cache the result ARGB for future use. draw_image stores
                    // its result in last_image_argb_ when obj_num != 0.
                    if (cache_converted_argb && !last_image_argb_.empty()) {
                      size_t n = static_cast<size_t>(img_w) *
                                 static_cast<size_t>(img_h);
                      RenderCacheEntry entry;
                      entry.width = static_cast<uint32_t>(img_w);
                      entry.height = static_cast<uint32_t>(img_h);
                      entry.data.resize(n * sizeof(uint32_t));
                      memcpy(entry.data.data(), last_image_argb_.data(),
                             n * sizeof(uint32_t));
                      RenderCache::instance().store(img_cache_key, std::move(entry));
                      last_image_argb_.clear();
                    }
                  }
                  rendered_xobject = true;
                  // Advance progress proportional to image work
                  size_t img_weight = std::max<size_t>(1,
                      (static_cast<size_t>(img_w) * img_h) / 10000);
                  advance_progress(img_weight);
                } else if (subtype_it->second.name == "Form") {
                  auto decoded = decode_stream(*current_pdf_, xobj_value, xobj_num, xobj_gen);
                  if (decoded.success && !decoded.data.empty()) {
                    GraphicsState saved_state = state_;
                    auto resources_it = xobj_value.stream.dict.find("Resources");
                    bool has_form_resources = false;
                    if (resources_it != xobj_value.stream.dict.end()) {
                      Value form_resources = resources_it->second;
                      if (form_resources.type == Value::REFERENCE) {
                        auto resolved = resolve_reference(*current_pdf_,
                            form_resources.ref_object_number,
                            form_resources.ref_generation_number);
                        if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                          form_resources_stack_.push_back(resolved.value.dict);
                          has_form_resources = true;
                        }
                      } else if (form_resources.type == Value::DICTIONARY) {
                        form_resources_stack_.push_back(form_resources.dict);
                        has_form_resources = true;
                      }
                    }
                    auto matrix_it = xobj_value.stream.dict.find("Matrix");
                    if (matrix_it != xobj_value.stream.dict.end() &&
                        matrix_it->second.type == Value::ARRAY &&
                        matrix_it->second.array.size() >= 6) {
                      GraphicsState::Matrix form_matrix;
                      form_matrix.a = static_cast<float>(matrix_it->second.array[0].number);
                      form_matrix.b = static_cast<float>(matrix_it->second.array[1].number);
                      form_matrix.c = static_cast<float>(matrix_it->second.array[2].number);
                      form_matrix.d = static_cast<float>(matrix_it->second.array[3].number);
                      form_matrix.e = static_cast<float>(matrix_it->second.array[4].number);
                      form_matrix.f = static_cast<float>(matrix_it->second.array[5].number);
                      state_.transform = form_matrix * state_.transform;
                    }
                    // Form: sub-operators advance progress naturally,
                    // so no progress guard or form-level advance.
                    parse_pdf_content(decoded.data);
                    rendered_xobject = true;
                    if (has_form_resources) form_resources_stack_.pop_back();
                    state_ = saved_state;
                  }
                }
              }
            }
          }
          // Progress already advanced inside each XObject subtype handler
          // (Image advances by pixel-area weight; Form sub-operators advance naturally).
        }
      }
      // Marked content operators (structure only, no-op for rendering)
      else if (tok3('B', 'M', 'C')) {
        // Begin Marked Content - consume tag operand, no-op
        // operands[0] = tag name
      } else if (tok3('B', 'D', 'C')) {
        // Begin Marked Content with properties - consume operands, no-op
        // operands[0] = tag name, operands[1] = properties dict/name
      } else if (tok3('E', 'M', 'C')) {
        // End Marked Content - no operands, no-op
      } else if (tok2('M', 'P')) {
        // Marked Content Point - consume tag operand, no-op
      } else if (tok2('D', 'P')) {
        // Marked Content Point with properties - consume operands, no-op
      }
      // Shading operator (sh) - paint a shading pattern
      else if (tok2('s', 'h')) {
        if (operands.size() >= 1) {
          std::string shading_name = operands[0];
          // Remove leading '/' if present
          if (!shading_name.empty() && shading_name[0] == '/') {
            shading_name = shading_name.substr(1);
          }
          draw_shading(shading_name);
          advance_progress();
        }
      }

      operands.clear();
    } else {
      // Accumulate operand
      operands.push_back(token);
    }
  }

  return true;
}

bool LightVGBackend::render_soft_mask_group(const Value& group_xobject, int mask_type) {
  if (!current_pdf_) return false;

  std::string cache_key;
  if (group_xobject.type == Value::REFERENCE) {
    cache_key = "smg:" + std::to_string(mask_type) + ":" +
                std::to_string(group_xobject.ref_object_number) + ":" +
                std::to_string(group_xobject.ref_generation_number);
    auto cache_it = soft_mask_group_cache_.find(cache_key);
    if (cache_it != soft_mask_group_cache_.end()) {
      state_.soft_mask_width = cache_it->second.width;
      state_.soft_mask_height = cache_it->second.height;
      state_.soft_mask_data = cache_it->second.data;
      return true;
    }
  }

  // Get the XObject stream
  Value xobject_value = group_xobject;
  if (xobject_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      xobject_value.ref_object_number,
                                      xobject_value.ref_generation_number);
    if (!resolved.success) return false;
    xobject_value = resolved.value;
  }

  if (xobject_value.type != Value::STREAM) return false;

  // Check that it's a Form XObject
  auto subtype_it = xobject_value.stream.dict.find("Subtype");
  if (subtype_it == xobject_value.stream.dict.end() ||
      subtype_it->second.type != Value::NAME ||
      subtype_it->second.name != "Form") {
    return false;
  }

  // Get dimensions from BBox
  float bbox[4] = {0, 0, 100, 100};  // Default
  auto bbox_it = xobject_value.stream.dict.find("BBox");
  if (bbox_it != xobject_value.stream.dict.end() && bbox_it->second.type == Value::ARRAY) {
    const auto& arr = bbox_it->second.array;
    for (size_t i = 0; i < 4 && i < arr.size(); ++i) {
      if (arr[i].type == Value::NUMBER) {
        bbox[i] = static_cast<float>(arr[i].number);
      }
    }
  }

  uint32_t mask_width = static_cast<uint32_t>(std::abs(bbox[2] - bbox[0]) * state_.scale);
  uint32_t mask_height = static_cast<uint32_t>(std::abs(bbox[3] - bbox[1]) * state_.scale);

  if (mask_width == 0) mask_width = width_;
  if (mask_height == 0) mask_height = height_;
  uint64_t mask_pixels =
      static_cast<uint64_t>(mask_width) * static_cast<uint64_t>(mask_height);
  uint64_t max_mask_pixels =
      static_cast<uint64_t>(std::max<uint32_t>(1, width_)) *
      static_cast<uint64_t>(std::max<uint32_t>(1, height_));
  max_mask_pixels = std::min<uint64_t>(
      std::max<uint64_t>(max_mask_pixels, 1024u * 1024u),
      kMaxSoftMaskGroupPixels);
  if (mask_pixels > max_mask_pixels) {
    double downscale =
        std::sqrt(static_cast<double>(max_mask_pixels) /
                  static_cast<double>(mask_pixels));
    mask_width = std::max<uint32_t>(
        1, static_cast<uint32_t>(std::floor(mask_width * downscale)));
    mask_height = std::max<uint32_t>(
        1, static_cast<uint32_t>(std::floor(mask_height * downscale)));
  }

  // Decode the XObject stream content
  auto decoded = decode_stream(*current_pdf_, xobject_value);
  if (!decoded.success) {
    return false;
  }

  // Create temporary canvas and scene for rendering the soft mask
  std::vector<uint32_t> mask_buffer(mask_width * mask_height, 0);

  // Initialize mask buffer based on mask type
  if (mask_type == 2) {  // Luminosity - start with white (ARGB = 0xFFFFFFFF)
    std::fill(mask_buffer.begin(), mask_buffer.end(), 0xFFFFFFFF);
  }
  // For alpha mask (type 1), buffer is already zeroed (transparent)

  lvg::SwCanvas* mask_canvas = lvg::SwCanvas::gen();
  if (!mask_canvas) {
    // Fallback to opaque mask
    state_.soft_mask_width = mask_width;
    state_.soft_mask_height = mask_height;
    state_.soft_mask_data.resize(mask_width * mask_height, 255);
    return true;
  }

  if (mask_canvas->target(mask_buffer.data(),
                          mask_width, mask_width, mask_height,
                          lvg::ColorSpace::ABGR8888) != lvg::Result::Success) {
    delete mask_canvas;
    state_.soft_mask_width = mask_width;
    state_.soft_mask_height = mask_height;
    state_.soft_mask_data.resize(mask_width * mask_height, 255);
    return true;
  }

  // Create temporary scene
  lvg::Scene* mask_scene = lvg::Scene::gen();
  if (!mask_scene) {
    delete mask_canvas;
    state_.soft_mask_width = mask_width;
    state_.soft_mask_height = mask_height;
    state_.soft_mask_data.resize(mask_width * mask_height, 255);
    return true;
  }

  // Save current state
  lvg::SwCanvas* saved_canvas = canvas_;
  lvg::Scene* saved_scene = scene_;
  std::vector<uint32_t> saved_buffer = std::move(buffer_);
  uint32_t saved_width = width_;
  uint32_t saved_height = height_;
  GraphicsState saved_state = state_;

  // Switch to mask canvas
  canvas_ = mask_canvas;
  scene_ = mask_scene;
  buffer_ = std::move(mask_buffer);
  width_ = mask_width;
  height_ = mask_height;

  // Reset graphics state for mask rendering
  state_ = GraphicsState();
  state_.page_width = std::abs(bbox[2] - bbox[0]);
  state_.page_height = std::abs(bbox[3] - bbox[1]);
  state_.scale = static_cast<float>(mask_width) / state_.page_width;

  // Parse and render the XObject content to the mask canvas
  bool progress_enabled = progress_.enabled;
  progress_.enabled = false;
  bool saved_rendering_soft_mask_group = rendering_soft_mask_group_;
  rendering_soft_mask_group_ = true;
  parse_pdf_content(decoded.data);
  rendering_soft_mask_group_ = saved_rendering_soft_mask_group;
  progress_.enabled = progress_enabled;

  // Finalize rendering
  if (mask_canvas->add(mask_scene) == lvg::Result::Success) {
    mask_canvas->draw(true);
    mask_canvas->sync();
  }

  // Extract mask values from rendered buffer
  state_.soft_mask_width = mask_width;
  state_.soft_mask_height = mask_height;
  state_.soft_mask_data.resize(mask_width * mask_height);

  for (uint32_t y = 0; y < mask_height; ++y) {
    for (uint32_t x = 0; x < mask_width; ++x) {
      size_t idx = y * mask_width + x;

      // Extract BGRA components from ABGR8888 format pixel
      uint32_t pixel = buffer_[idx];
      uint8_t b = static_cast<uint8_t>(pixel & 0xFF);
      uint8_t g = static_cast<uint8_t>((pixel >> 8) & 0xFF);
      uint8_t r = static_cast<uint8_t>((pixel >> 16) & 0xFF);
      uint8_t a = static_cast<uint8_t>((pixel >> 24) & 0xFF);

      if (mask_type == 2) {  // Luminosity mask
        // Convert RGB to luminance using standard coefficients
        float luminance = 0.2126f * (r / 255.0f) + 0.7152f * (g / 255.0f) + 0.0722f * (b / 255.0f);
        state_.soft_mask_data[idx] = static_cast<uint8_t>(luminance * 255.0f);
      } else {  // Alpha mask
        state_.soft_mask_data[idx] = a;  // Alpha channel
      }
    }
  }

  // Clean up mask canvas (scene was pushed to canvas, so canvas owns it)
  delete mask_canvas;

  // Restore original state
  canvas_ = saved_canvas;
  scene_ = saved_scene;
  buffer_ = std::move(saved_buffer);
  width_ = saved_width;
  height_ = saved_height;

  // Restore graphics state but keep the extracted soft mask data
  auto soft_mask_data = std::move(state_.soft_mask_data);
  auto soft_mask_width = state_.soft_mask_width;
  auto soft_mask_height = state_.soft_mask_height;
  state_ = saved_state;
  state_.soft_mask_data = std::move(soft_mask_data);
  state_.soft_mask_width = soft_mask_width;
  state_.soft_mask_height = soft_mask_height;

  if (!cache_key.empty()) {
    SoftMaskGroupCacheEntry entry;
    entry.width = state_.soft_mask_width;
    entry.height = state_.soft_mask_height;
    entry.data = state_.soft_mask_data;
    soft_mask_group_cache_[cache_key] = std::move(entry);
  }

  return true;
}

}  // namespace nanopdf

#endif // NANOPDF_USE_LIGHTVG
