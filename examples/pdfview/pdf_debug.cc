// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "pdf_debug.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

#include "nanopdf.hh"
#include "pdf_document.hh"

namespace pdfview {
namespace {

void append_cp(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

bool overlaps(double ax1, double ay1, double ax2, double ay2, double bx1,
              double by1, double bx2, double by2) {
  return ax1 < bx2 && ax2 > bx1 && ay1 < by2 && ay2 > by1;
}

// resources[category] (resolving an indirect dict) -> name -> indirect ref.
bool resource_obj(const nanopdf::Pdf& pdf, const nanopdf::Page& page,
                  const char* category, const std::string& name, uint32_t* obj,
                  uint16_t* gen) {
  auto cit = page.resources.find(category);
  if (cit == page.resources.end()) return false;
  nanopdf::Value cat = cit->second;
  if (cat.type == nanopdf::Value::REFERENCE) {
    auto r = nanopdf::resolve_reference(pdf, cat.ref_object_number,
                                        cat.ref_generation_number);
    if (r.success) cat = r.value;
  }
  if (cat.type != nanopdf::Value::DICTIONARY) return false;
  auto nit = cat.dict.find(name);
  if (nit == cat.dict.end() || nit->second.type != nanopdf::Value::REFERENCE)
    return false;
  *obj = nit->second.ref_object_number;
  *gen = nit->second.ref_generation_number;
  return true;
}

struct Mat {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  Mat concat(const Mat& m) const {
    return Mat{m.a * a + m.b * c,       m.a * b + m.b * d,
               m.c * a + m.d * c,       m.c * b + m.d * d,
               m.e * a + m.f * c + e,   m.e * b + m.f * d + f};
  }
};

struct ImgPlace {
  std::string name;
  double x, y, w, h;  // axis-aligned bbox in page space
};

// Tokenize the page content stream (q/Q/cm/Do) and return image placements as
// page-space bounding boxes. Mirrors the MCP get_image_placements tokenizer.
std::vector<ImgPlace> collect_image_placements(const nanopdf::Pdf& pdf,
                                               const nanopdf::Page& page) {
  std::vector<ImgPlace> out;
  nanopdf::PageContent pc = page.load_contents(pdf);
  if (!pc.success) return out;
  const std::string s(pc.data.begin(), pc.data.end());
  size_t pos = 0;
  const size_t len = s.size();

  auto skip_ws = [&]() {
    while (pos < len && (s[pos] == ' ' || s[pos] == '\n' || s[pos] == '\r' ||
                         s[pos] == '\t' || s[pos] == '\f'))
      pos++;
  };
  std::function<bool(std::string&)> read_token = [&](std::string& t) -> bool {
    skip_ws();
    if (pos >= len) return false;
    t.clear();
    char ch = s[pos];
    if (ch == '%') {
      while (pos < len && s[pos] != '\n' && s[pos] != '\r') pos++;
      return read_token(t);
    }
    if (ch == '(') {
      int depth = 1;
      pos++;
      while (pos < len && depth > 0) {
        if (s[pos] == '\\') pos++;
        else if (s[pos] == '(') depth++;
        else if (s[pos] == ')') depth--;
        pos++;
      }
      t = "(s)";
      return true;
    }
    if (ch == '<' && pos + 1 < len && s[pos + 1] == '<') {
      pos += 2;
      int depth = 1;
      while (pos + 1 < len && depth > 0) {
        if (s[pos] == '<' && s[pos + 1] == '<') { depth++; pos += 2; }
        else if (s[pos] == '>' && s[pos + 1] == '>') { depth--; pos += 2; }
        else pos++;
      }
      t = "<<>>";
      return true;
    }
    if (ch == '<') {
      pos++;
      while (pos < len && s[pos] != '>') pos++;
      if (pos < len) pos++;
      t = "<h>";
      return true;
    }
    if (ch == '[') {
      pos++;
      int depth = 1;
      while (pos < len && depth > 0) {
        if (s[pos] == '[') depth++;
        else if (s[pos] == ']') depth--;
        pos++;
      }
      t = "[a]";
      return true;
    }
    if (ch == '/') {
      pos++;
      while (pos < len && s[pos] != ' ' && s[pos] != '\n' && s[pos] != '\r' &&
             s[pos] != '\t' && s[pos] != '/' && s[pos] != '[' && s[pos] != '(' &&
             s[pos] != '<' && s[pos] != '{' && s[pos] != '}')
        t += s[pos++];
      t = "/" + t;
      return true;
    }
    size_t start = pos;
    while (pos < len && s[pos] != ' ' && s[pos] != '\n' && s[pos] != '\r' &&
           s[pos] != '\t' && s[pos] != '/' && s[pos] != '[' && s[pos] != '(' &&
           s[pos] != '<' && s[pos] != '{' && s[pos] != '}')
      pos++;
    t = s.substr(start, pos - start);
    return !t.empty();
  };

  Mat ctm;
  std::vector<Mat> stack;
  std::vector<double> nums;
  std::string tok, last_name;
  while (read_token(tok)) {
    if (!tok.empty() && (tok[0] == '-' || tok[0] == '+' || tok[0] == '.' ||
                         (tok[0] >= '0' && tok[0] <= '9'))) {
      char* end = nullptr;
      double v = std::strtod(tok.c_str(), &end);
      if (end && end != tok.c_str()) nums.push_back(v);
      continue;
    }
    if (!tok.empty() && tok[0] == '/') {
      last_name = tok.substr(1);
      nums.clear();
      continue;
    }
    if (tok == "q") {
      stack.push_back(ctm);
    } else if (tok == "Q") {
      if (!stack.empty()) { ctm = stack.back(); stack.pop_back(); }
    } else if (tok == "cm" && nums.size() >= 6) {
      size_t b = nums.size() - 6;
      ctm = Mat{nums[b], nums[b + 1], nums[b + 2], nums[b + 3], nums[b + 4],
                nums[b + 5]}.concat(ctm);
    } else if (tok == "Do" && !last_name.empty()) {
      double xs[4] = {ctm.e, ctm.a + ctm.e, ctm.c + ctm.e, ctm.a + ctm.c + ctm.e};
      double ys[4] = {ctm.f, ctm.b + ctm.f, ctm.d + ctm.f, ctm.b + ctm.d + ctm.f};
      double minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
      for (int i = 1; i < 4; ++i) {
        minx = std::min(minx, xs[i]);
        maxx = std::max(maxx, xs[i]);
        miny = std::min(miny, ys[i]);
        maxy = std::max(maxy, ys[i]);
      }
      out.push_back({last_name, minx, miny, maxx - minx, maxy - miny});
    }
    nums.clear();
  }
  return out;
}

const char* annot_type_name(nanopdf::AnnotationType t) {
  using A = nanopdf::AnnotationType;
  switch (t) {
    case A::Text: return "Text";
    case A::Link: return "Link";
    case A::FreeText: return "FreeText";
    case A::Line: return "Line";
    case A::Square: return "Square";
    case A::Circle: return "Circle";
    case A::Highlight: return "Highlight";
    case A::Underline: return "Underline";
    case A::StrikeOut: return "StrikeOut";
    case A::Stamp: return "Stamp";
    case A::Ink: return "Ink";
    case A::Popup: return "Popup";
    case A::Widget: return "Widget";
    case A::Redact: return "Redact";
    default: return "Annot";
  }
}

void serialize(const nanopdf::Value& v, std::string& out, int depth) {
  using V = nanopdf::Value;
  const std::string pad(depth * 2, ' ');
  if (depth > 6) {
    out += "...";
    return;
  }
  switch (v.type) {
    case V::BOOLEAN:
      out += v.boolean ? "true" : "false";
      break;
    case V::NUMBER: {
      char b[32];
      std::snprintf(b, sizeof(b), "%g", v.number);
      out += b;
      break;
    }
    case V::STRING: {
      std::string t = decode_pdf_text(v.str);
      if (t.size() > 80) t = t.substr(0, 80) + "...";
      out += "(" + t + ")";
      break;
    }
    case V::NAME:
      out += "/" + v.name;
      break;
    case V::REFERENCE:
      out += std::to_string(v.ref_object_number) + " " +
             std::to_string(v.ref_generation_number) + " R";
      break;
    case V::NULL_OBJ:
      out += "null";
      break;
    case V::ARRAY: {
      out += "[";
      for (size_t i = 0; i < v.array.size() && i < 32; ++i) {
        if (i) out += " ";
        serialize(v.array[i], out, depth + 1);
      }
      if (v.array.size() > 32) out += " ...";
      out += "]";
      break;
    }
    case V::DICTIONARY:
    case V::STREAM: {
      const nanopdf::Dictionary& d = (v.type == V::STREAM) ? v.stream.dict : v.dict;
      out += "<<\n";
      for (const auto& kv : d) {
        out += pad + "  /" + kv.first + " ";
        serialize(kv.second, out, depth + 1);
        out += "\n";
      }
      out += pad + ">>";
      if (v.type == V::STREAM)
        out += " stream(" + std::to_string(v.stream.data.size()) + " bytes)";
      break;
    }
    default:
      out += "<undefined>";
      break;
  }
}

}  // namespace

std::vector<RegionHit> inspect_region(PdfDocument& doc, int page_index,
                                      double x1, double y1, double x2,
                                      double y2) {
  std::vector<RegionHit> hits;
  const nanopdf::Pdf* pdf = doc.pdf();
  if (!pdf || page_index < 0 || page_index >= doc.page_count()) return hits;
  if (x1 > x2) std::swap(x1, x2);
  if (y1 > y2) std::swap(y1, y2);
  const nanopdf::Page& page = pdf->catalog.pages[(size_t)page_index];

  // Text runs grouped by font resource.
  const nanopdf::TextPage* tp = doc.text_page(page_index);
  if (tp) {
    struct Grp {
      std::string text;
      double minx = 0, miny = 0, maxx = 0, maxy = 0;
      bool init = false;
    };
    std::map<std::string, Grp> by_font;
    for (const auto& ch : tp->chars) {
      const auto& q = ch.quad;
      double cx = q.x + q.width * 0.5, cy = q.y + q.height * 0.5;
      if (cx < x1 || cx > x2 || cy < y1 || cy > y2) continue;
      Grp& g = by_font[ch.font_name];
      if (!g.init) {
        g.minx = q.x; g.miny = q.y; g.maxx = q.x + q.width; g.maxy = q.y + q.height;
        g.init = true;
      } else {
        g.minx = std::min(g.minx, q.x);
        g.miny = std::min(g.miny, q.y);
        g.maxx = std::max(g.maxx, q.x + q.width);
        g.maxy = std::max(g.maxy, q.y + q.height);
      }
      if (g.text.size() < 60 && ch.unicode >= 32) append_cp(g.text, ch.unicode);
    }
    for (auto& kv : by_font) {
      RegionHit h;
      h.kind = RegionHit::Text;
      uint32_t o = 0;
      uint16_t gn = 0;
      if (resource_obj(*pdf, page, "Font", kv.first, &o, &gn)) {
        h.obj_num = (int)o;
        h.gen_num = gn;
      }
      h.rect.x = kv.second.minx;
      h.rect.y = kv.second.miny;
      h.rect.width = kv.second.maxx - kv.second.minx;
      h.rect.height = kv.second.maxy - kv.second.miny;
      h.label = "text \"" + kv.second.text + "\"  font /" + kv.first +
                (h.obj_num >= 0 ? "  obj " + std::to_string(h.obj_num) : "");
      hits.push_back(std::move(h));
    }
  }

  // Image XObjects.
  for (const auto& p : collect_image_placements(*pdf, page)) {
    if (!overlaps(x1, y1, x2, y2, p.x, p.y, p.x + p.w, p.y + p.h)) continue;
    RegionHit h;
    h.kind = RegionHit::Image;
    h.rect.x = p.x; h.rect.y = p.y; h.rect.width = p.w; h.rect.height = p.h;
    uint32_t o = 0;
    uint16_t gn = 0;
    std::string objs;
    if (resource_obj(*pdf, page, "XObject", p.name, &o, &gn)) {
      h.obj_num = (int)o;
      h.gen_num = gn;
      objs = "  obj " + std::to_string(o);
    }
    h.label = "image /" + p.name + objs;
    char db[96];
    std::snprintf(db, sizeof(db), "placement %.0f,%.0f  %.0fx%.0f pt", p.x, p.y,
                  p.w, p.h);
    h.detail = db;
    hits.push_back(std::move(h));
  }

  // Annotations.
  for (const auto& a : page.annotations) {
    if (!a || a->rect.size() < 4) continue;
    double ax1 = a->rect[0], ay1 = a->rect[1], ax2 = a->rect[2], ay2 = a->rect[3];
    if (ax1 > ax2) std::swap(ax1, ax2);
    if (ay1 > ay2) std::swap(ay1, ay2);
    if (!overlaps(x1, y1, x2, y2, ax1, ay1, ax2, ay2)) continue;
    RegionHit h;
    h.kind = RegionHit::Annotation;
    h.rect.x = ax1; h.rect.y = ay1; h.rect.width = ax2 - ax1; h.rect.height = ay2 - ay1;
    h.label = std::string("annotation ") + annot_type_name(a->type);
    h.detail = a->contents;
    hits.push_back(std::move(h));
  }
  return hits;
}

std::string dump_object(PdfDocument& doc, uint32_t obj_num, uint16_t gen_num) {
  const nanopdf::Pdf* pdf = doc.pdf();
  if (!pdf) return "(no document)";
  nanopdf::ResolvedObject r = pdf->load_object(obj_num, gen_num);
  if (!r.success) {
    return std::to_string(obj_num) + " " + std::to_string(gen_num) +
           " R: " + (r.error.empty() ? "load failed" : r.error);
  }
  std::string out = std::to_string(obj_num) + " " + std::to_string(gen_num) +
                    " obj\n";
  serialize(r.value, out, 0);
  return out;
}

std::vector<RegionHit> find_object_placements(PdfDocument& doc, int page_index,
                                              uint32_t obj_num) {
  std::vector<RegionHit> hits;
  const nanopdf::Pdf* pdf = doc.pdf();
  if (!pdf || page_index < 0 || page_index >= doc.page_count()) return hits;
  const nanopdf::Page& page = pdf->catalog.pages[(size_t)page_index];

  // Image XObject placements pointing at this object.
  for (const auto& p : collect_image_placements(*pdf, page)) {
    uint32_t o = 0;
    uint16_t gn = 0;
    if (resource_obj(*pdf, page, "XObject", p.name, &o, &gn) && o == obj_num) {
      RegionHit h;
      h.kind = RegionHit::Image;
      h.obj_num = (int)obj_num;
      h.rect.x = p.x; h.rect.y = p.y; h.rect.width = p.w; h.rect.height = p.h;
      h.label = "image /" + p.name;
      hits.push_back(std::move(h));
    }
  }

  // Font object: highlight each text line drawn with that font.
  std::vector<std::string> font_names;
  {
    auto fit = page.resources.find("Font");
    nanopdf::Value fonts;
    if (fit != page.resources.end()) {
      fonts = fit->second;
      if (fonts.type == nanopdf::Value::REFERENCE) {
        auto rr = nanopdf::resolve_reference(*pdf, fonts.ref_object_number,
                                             fonts.ref_generation_number);
        if (rr.success) fonts = rr.value;
      }
      if (fonts.type == nanopdf::Value::DICTIONARY) {
        for (const auto& kv : fonts.dict) {
          if (kv.second.type == nanopdf::Value::REFERENCE &&
              kv.second.ref_object_number == obj_num)
            font_names.push_back(kv.first);
        }
      }
    }
  }
  if (!font_names.empty()) {
    const nanopdf::TextPage* tp = doc.text_page(page_index);
    if (tp) {
      std::map<int, RegionHit> by_line;  // one rect per text line
      int synth = -1;
      for (const auto& ch : tp->chars) {
        if (std::find(font_names.begin(), font_names.end(), ch.font_name) ==
            font_names.end())
          continue;
        int li = ch.line_index >= 0 ? ch.line_index : synth--;
        const auto& q = ch.quad;
        auto it = by_line.find(li);
        if (it == by_line.end()) {
          RegionHit h;
          h.kind = RegionHit::Text;
          h.obj_num = (int)obj_num;
          h.rect.x = q.x; h.rect.y = q.y; h.rect.width = q.width; h.rect.height = q.height;
          h.label = "text font /" + ch.font_name;
          by_line[li] = h;
        } else {
          RegionHit& h = it->second;
          double minx = std::min(h.rect.x, q.x);
          double miny = std::min(h.rect.y, q.y);
          double maxx = std::max(h.rect.x + h.rect.width, q.x + q.width);
          double maxy = std::max(h.rect.y + h.rect.height, q.y + q.height);
          h.rect.x = minx; h.rect.y = miny; h.rect.width = maxx - minx; h.rect.height = maxy - miny;
        }
      }
      for (auto& kv : by_line) hits.push_back(std::move(kv.second));
    }
  }
  return hits;
}

}  // namespace pdfview
