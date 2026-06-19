// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// pdfview — native PDF viewer built on lightui (UI/X11) + nanopdf (rendering).
//
// Phase 2: open a document, rasterize pages via nanopdf into lvg surfaces, and
// display them with page navigation, zoom (fit-width / fit-page / steps), and
// scrolling. The Open dialog, sidebars, search and panels land in later phases.

extern "C" {
#include <lightui/lightui.h>
#include <lightui/window.h>
#include <lightui/event.h>
#include <lightui/font.h>
#include <lightvg/canvas.h>
#include <lightvg/surface.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pdf_document.hh"

#if PDFVIEW_HAVE_NFD
extern "C" {
#include <nfd.h>
}
#endif

namespace {

constexpr int kInitialWidth = 1100;
constexpr int kInitialHeight = 800;
constexpr int kToolbarH = 44;
constexpr int kPageMargin = 16;       // gap around the page in the content area
constexpr int kSidebarW = 300;        // sidebar width (physical px)
constexpr int kRowH = 24;             // outline row height
constexpr int kTabH = 30;             // sidebar tab-bar height
constexpr int kThumbW = 220;          // thumbnail render target width
constexpr int kThumbImgH = 200;       // thumbnail image area height per cell
constexpr int kThumbCellH = 234;      // full thumbnail cell height (img + label)
constexpr int kThumbMargin = 34;      // horizontal margin around thumbnails
constexpr float kMinZoom = 0.1f;
constexpr float kMaxZoom = 8.0f;
constexpr float kZoomStep = 1.2f;

// Dark UI chrome, matching the wasm viewer.
constexpr lvg_color_t kBgColor = LVG_COLOR_RGB(0x1E, 0x20, 0x24);
constexpr lvg_color_t kToolbarColor = LVG_COLOR_RGB(0x2B, 0x2F, 0x34);
constexpr lvg_color_t kToolbarLine = LVG_COLOR_RGB(0x16, 0x18, 0x1B);
constexpr lvg_color_t kTextColor = LVG_COLOR_RGB(0xD0, 0xD3, 0xD7);
constexpr lvg_color_t kTextDim = LVG_COLOR_RGB(0x80, 0x86, 0x8C);
constexpr lvg_color_t kPageShadow = LVG_COLOR_ARGB(0x55, 0x00, 0x00, 0x00);
constexpr lvg_color_t kSidebarBg = LVG_COLOR_RGB(0x25, 0x28, 0x2D);
constexpr lvg_color_t kSidebarSel = LVG_COLOR_RGB(0x33, 0x4E, 0x68);
constexpr lvg_color_t kSidebarText = LVG_COLOR_RGB(0xC2, 0xC6, 0xCB);
constexpr lvg_color_t kTabActive = LVG_COLOR_RGB(0x2B, 0x2F, 0x34);
constexpr lvg_color_t kTabInactive = LVG_COLOR_RGB(0x1E, 0x20, 0x24);
constexpr lvg_color_t kAccent = LVG_COLOR_RGB(0x4A, 0x9E, 0xFF);
constexpr lvg_color_t kMatchHi = LVG_COLOR_ARGB(0x60, 0xFF, 0xE0, 0x00);   // search hit
constexpr lvg_color_t kMatchCur = LVG_COLOR_ARGB(0x99, 0xFF, 0x8C, 0x00);  // focused hit
constexpr lvg_color_t kSelHi = LVG_COLOR_ARGB(0x55, 0x4A, 0x9E, 0xFF);     // selection

// X11 keysyms not given a LUI_KEY_* alias.
constexpr int kKeyPageUp = 0xFF55;
constexpr int kKeyPageDown = 0xFF56;

float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Viewer {
  pdfview::PdfDocument doc;
  lui_font_t* font = nullptr;
  std::string title;       // base filename for the toolbar
  int page = 0;            // 0-based current page
  float zoom = 1.0f;       // px per PDF point
  int scroll_x = 0;        // content scroll offset (px), >= 0
  int scroll_y = 0;
  bool fit_width = true;   // recompute zoom to fit content width each draw
  bool show_sidebar = true;
  int sidebar_mode = 0;    // 0 = Outline, 1 = Pages (thumbnails)
  int sidebar_scroll = 0;  // outline list scroll offset (px), >= 0
  int thumb_scroll = 0;    // thumbnail strip scroll offset (px), >= 0
  int mouse_x = 0;         // last cursor position in physical px
  int mouse_y = 0;

  // Search.
  bool search_active = false;   // typing in the search box
  std::string query;
  int match_page = -1;          // page the current matches belong to
  std::vector<nanopdf::TextQuad> match_quads;  // one bbox per match (page coords)
  int match_index = -1;         // currently focused match

  // Drag selection.
  bool selecting = false;
  int sel_x0 = 0, sel_y0 = 0, sel_x1 = 0, sel_y1 = 0;  // screen px
  int sel_page = -1;
  std::string sel_text;
  std::vector<nanopdf::TextQuad> sel_quads;  // selected segment bboxes (page coords)

  // Right-side panel: 0 = hidden, 1 = info, 2 = revisions.
  int right_panel = 0;
  int info_scroll = 0;

  // Revision history (Phase 6).
  bool revisions_loaded = false;
  std::vector<nanopdf::RevisionHistoryEntry> revisions;
  int rev_selected = -1;        // index into revisions, or -1 for current doc
  int rev_mode = 0;             // 0 = After/current, 1 = Before/rev, 2 = Diff
  pdfview::RenderedPage rev_page;   // cached snapshot render (Before)
  pdfview::RenderedPage diff_page;  // cached Before-vs-After diff image
  int rev_cache_key = -1;       // rev_selected this snapshot was rendered for
  int rev_cache_page = -1;
  float rev_cache_scale = 0.0f;

  // Current page placement (physical px), set by draw() for coord mapping.
  int page_px = 0, page_py = 0;
  float page_scale = 1.0f;

  char status[256] = {0};
};

// Truncate @s with an ellipsis so it fits within @maxw pixels.
std::string fit_text(lui_font_t* f, const std::string& s, int maxw) {
  if (!f || maxw <= 0) return std::string();
  if (lui_font_measure_text(f, s.c_str(), (int)s.size()) <= maxw) return s;
  const char* ell = "...";
  int ew = lui_font_measure_text(f, ell, 3);
  int budget = maxw - ew;
  if (budget <= 0) return std::string();
  int len = (int)s.size();
  while (len > 0) {
    --len;  // step back to a UTF-8 boundary
    while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) --len;
    if (lui_font_measure_text(f, s.c_str(), len) <= budget) break;
  }
  return s.substr(0, len) + ell;
}

void dump_surface_ppm(const lvg_surface_t* s, const char* path) {
  if (!s || !s->pixels || s->width <= 0 || s->height <= 0) return;
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", s->width, s->height);
  for (int y = 0; y < s->height; ++y) {
    const uint32_t* row = s->pixels + (size_t)y * s->stride;
    for (int x = 0; x < s->width; ++x) {
      uint32_t p = row[x];
      unsigned char rgb[3] = {(unsigned char)((p >> 16) & 0xFF),
                              (unsigned char)((p >> 8) & 0xFF),
                              (unsigned char)(p & 0xFF)};
      std::fwrite(rgb, 1, 3, f);
    }
  }
  std::fclose(f);
}

float fit_width_zoom(Viewer& v, int content_w) {
  double wpt = 0, hpt = 0;
  v.doc.page_size_points(v.page, &wpt, &hpt);
  if (wpt <= 0) return v.zoom;
  return clampf((float)((content_w - 2 * kPageMargin) / wpt), kMinZoom, kMaxZoom);
}

float fit_page_zoom(Viewer& v, int content_w, int content_h) {
  double wpt = 0, hpt = 0;
  v.doc.page_size_points(v.page, &wpt, &hpt);
  if (wpt <= 0 || hpt <= 0) return v.zoom;
  float zw = (float)((content_w - 2 * kPageMargin) / wpt);
  float zh = (float)((content_h - 2 * kPageMargin) / hpt);
  return clampf(std::min(zw, zh), kMinZoom, kMaxZoom);
}

void update_status(Viewer& v) {
  std::snprintf(v.status, sizeof(v.status), "%s    %d / %d    %d%%",
                v.title.empty() ? "(no document)" : v.title.c_str(),
                v.doc.loaded() ? v.page + 1 : 0, v.doc.page_count(),
                (int)std::lround(v.zoom * 100.0f / (96.0f / 72.0f)));
}

// The sidebar is shown for any loaded document (thumbnails always apply); the
// outline tab is only useful when bookmarks exist.
int sidebar_width(Viewer& v) {
  return (v.show_sidebar && v.doc.loaded()) ? kSidebarW : 0;
}
bool has_outline(Viewer& v) {
  return v.doc.loaded() && !v.doc.outline().empty();
}
// Y at which the sidebar panel content (below the tab bar) begins.
int sidebar_panel_y() { return kToolbarH + kTabH; }

void draw_outline_panel(Viewer& v, lvg_canvas_t* c, int panel_y, int H) {
  const int sb_w = kSidebarW;
  const std::vector<pdfview::Bookmark>& bm = v.doc.outline();
  int avail = std::max(0, H - panel_y);
  v.sidebar_scroll =
      clampi(v.sidebar_scroll, 0, std::max(0, (int)bm.size() * kRowH - avail));
  const int lh = v.font ? lui_font_line_height(v.font) : 14;
  for (size_t i = 0; i < bm.size(); ++i) {
    int ry = panel_y + (int)i * kRowH - v.sidebar_scroll;
    if (ry + kRowH <= panel_y || ry >= H) continue;
    const bool current = (bm[i].page == v.page);
    if (current) lvg_canvas_fill_rect(c, 0, ry, sb_w, kRowH, kSidebarSel);
    if (v.font) {
      const int tx = 12 + bm[i].depth * 14;
      const int ty = ry + (kRowH - lh) / 2;
      int num_w = 0;
      char num[16] = {0};
      if (bm[i].page >= 0) {
        std::snprintf(num, sizeof(num), "%d", bm[i].page + 1);
        num_w = lui_font_measure_text(v.font, num, (int)std::strlen(num));
        lui_canvas_draw_text(c, sb_w - num_w - 12, ty, num,
                             (int)std::strlen(num), v.font, kTextDim);
      }
      std::string t = fit_text(v.font, bm[i].title, sb_w - tx - num_w - 24);
      lui_canvas_draw_text(c, tx, ty, t.c_str(), (int)t.size(), v.font,
                           current ? kTextColor : kSidebarText);
    }
  }
}

void draw_thumb_panel(Viewer& v, lvg_canvas_t* c, int panel_y, int H) {
  const int sb_w = kSidebarW;
  const int n = v.doc.page_count();
  int avail = std::max(0, H - panel_y);
  v.thumb_scroll =
      clampi(v.thumb_scroll, 0, std::max(0, n * kThumbCellH - avail));
  const int area_w = sb_w - 2 * kThumbMargin;
  const int lh = v.font ? lui_font_line_height(v.font) : 12;
  for (int i = 0; i < n; ++i) {
    int cy = panel_y + i * kThumbCellH - v.thumb_scroll;
    if (cy + kThumbCellH <= panel_y || cy >= H) continue;  // cull off-screen
    const bool current = (i == v.page);
    const pdfview::RenderedPage* tp = v.doc.render_thumbnail(i, kThumbW);
    if (tp && tp->valid()) {
      float s = std::min((float)area_w / tp->width,
                         (float)kThumbImgH / tp->height);
      int dw = std::max(1, (int)(tp->width * s));
      int dh = std::max(1, (int)(tp->height * s));
      int dx = kThumbMargin + (area_w - dw) / 2;
      int dy = cy + (kThumbImgH - dh) / 2;
      lvg_canvas_fill_rect(c, dx, dy, dw, dh, LVG_COLOR_WHITE);
      lvg_canvas_draw_image(c, dx, dy, dw, dh, &tp->surface, nullptr,
                            LVG_IMAGE_FILTER_BILINEAR);
      lvg_color_t bc = current ? kAccent : kToolbarLine;
      int t = current ? 2 : 1;  // frame thickness
      lvg_canvas_fill_rect(c, dx - t, dy - t, dw + 2 * t, t, bc);
      lvg_canvas_fill_rect(c, dx - t, dy + dh, dw + 2 * t, t, bc);
      lvg_canvas_fill_rect(c, dx - t, dy - t, t, dh + 2 * t, bc);
      lvg_canvas_fill_rect(c, dx + dw, dy - t, t, dh + 2 * t, bc);
    }
    if (v.font) {
      char num[16];
      std::snprintf(num, sizeof(num), "%d", i + 1);
      int nw = lui_font_measure_text(v.font, num, (int)std::strlen(num));
      lui_canvas_draw_text(c, (sb_w - nw) / 2, cy + kThumbImgH + 6, num,
                           (int)std::strlen(num), v.font,
                           current ? kTextColor : kTextDim);
    }
  }
}

void draw_tab(Viewer& v, lvg_canvas_t* c, int x, int w, int y, const char* label,
              bool active, bool enabled) {
  lvg_canvas_fill_rect(c, x, y, w, kTabH, active ? kTabActive : kTabInactive);
  if (active) lvg_canvas_fill_rect(c, x, y, w, 2, kAccent);
  if (v.font) {
    int lh = lui_font_line_height(v.font);
    int tw = lui_font_measure_text(v.font, label, (int)std::strlen(label));
    lvg_color_t col = enabled ? (active ? kTextColor : kTextDim)
                              : LVG_COLOR_RGB(0x55, 0x59, 0x5E);
    lui_canvas_draw_text(c, x + (w - tw) / 2, y + (kTabH - lh) / 2, label,
                         (int)std::strlen(label), v.font, col);
  }
}

void draw_sidebar(Viewer& v, lvg_canvas_t* c, int H) {
  const int sb_w = kSidebarW;
  const int y0 = kToolbarH;
  const int h = std::max(0, H - kToolbarH);
  lvg_rect_t clip = lvg_rect_make(0, y0, sb_w, h);
  lvg_canvas_set_clip(c, &clip);
  lvg_canvas_fill_rect(c, 0, y0, sb_w, h, kSidebarBg);

  // Tab bar: Outline | Pages.
  const bool outline_ok = has_outline(v);
  const int half = sb_w / 2;
  draw_tab(v, c, 0, half, y0, "Outline", v.sidebar_mode == 0 && outline_ok,
           outline_ok);
  draw_tab(v, c, half, sb_w - half, y0, "Pages", v.sidebar_mode == 1, true);

  const int panel_y = sidebar_panel_y();
  lvg_rect_t pclip = lvg_rect_make(0, panel_y, sb_w, std::max(0, H - panel_y));
  lvg_canvas_set_clip(c, &pclip);
  if (v.sidebar_mode == 0 && outline_ok)
    draw_outline_panel(v, c, panel_y, H);
  else
    draw_thumb_panel(v, c, panel_y, H);
  lvg_canvas_reset_clip(c);

  lvg_canvas_fill_rect(c, 0, panel_y - 1, sb_w, 1, kToolbarLine);  // tab sep
  lvg_canvas_fill_rect(c, sb_w - 1, y0, 1, h, kToolbarLine);       // divider
}

// ---- Right panel: document info / signatures / forms, and revisions --------

constexpr int kRightW = 340;
constexpr int kRevRowH = 50;

int right_width(Viewer& v) {
  return (v.right_panel != 0 && v.doc.loaded()) ? kRightW : 0;
}

const char* field_type_name(nanopdf::FieldType t) {
  switch (t) {
    case nanopdf::FieldType::Button: return "Button";
    case nanopdf::FieldType::Text: return "Text";
    case nanopdf::FieldType::Choice: return "Choice";
    case nanopdf::FieldType::Signature: return "Sig";
  }
  return "Field";
}

std::string value_to_string(const nanopdf::Value& val) {
  switch (val.type) {
    case nanopdf::Value::STRING: return val.str;
    case nanopdf::Value::NAME: return val.name;
    case nanopdf::Value::NUMBER: {
      char b[32];
      std::snprintf(b, sizeof(b), "%g", val.number);
      return b;
    }
    case nanopdf::Value::BOOLEAN: return val.boolean ? "true" : "false";
    default: return "";
  }
}

struct InfoLine {
  std::string text;
  bool header;
};

std::vector<InfoLine> build_info_lines(Viewer& v) {
  std::vector<InfoLine> L;
  const nanopdf::Pdf* pdf = v.doc.pdf();
  if (!pdf) return L;
  pdf->ensure_metadata_loaded();
  pdf->ensure_acro_form_loaded();
  const auto& cat = pdf->catalog;
  char buf[256];

  L.push_back({"Document", true});
  L.push_back({"File: " + v.title, false});
  L.push_back({"Pages: " + std::to_string(v.doc.page_count()), false});
  double wpt = 0, hpt = 0;
  v.doc.page_size_points(v.page, &wpt, &hpt);
  std::snprintf(buf, sizeof(buf), "Page %d: %.0f x %.0f pt  (%.1f x %.1f in)",
                v.page + 1, wpt, hpt, wpt / 72.0, hpt / 72.0);
  L.push_back({buf, false});
  std::snprintf(buf, sizeof(buf), "PDF version: %d.%d", pdf->version_major,
                pdf->version_minor);
  L.push_back({buf, false});
  const auto& di = cat.document_info;
  using pdfview::decode_pdf_text;
  if (!di.title.empty()) L.push_back({"Title: " + decode_pdf_text(di.title), false});
  if (!di.author.empty())
    L.push_back({"Author: " + decode_pdf_text(di.author), false});
  if (!di.creator.empty())
    L.push_back({"Creator: " + decode_pdf_text(di.creator), false});
  if (!di.producer.empty())
    L.push_back({"Producer: " + decode_pdf_text(di.producer), false});
  if (!di.creation_date.empty())
    L.push_back({"Created: " + di.creation_date, false});

  L.push_back({"", false});
  L.push_back(
      {"Signatures (" + std::to_string(cat.signature_fields.size()) + ")",
       true});
  for (const auto& s : cat.signature_fields) {
    L.push_back({std::string(s.is_signed ? "[signed] " : "[unsigned] ") +
                     (s.name.empty() ? "(unnamed)" : s.name),
                 false});
    if (!s.signing_reason.empty())
      L.push_back({"  reason: " + s.signing_reason, false});
    if (!s.signing_date.empty())
      L.push_back({"  date: " + s.signing_date, false});
    if (s.is_signed) {
      nanopdf::SignatureValidationResult vr =
          nanopdf::validate_signature(*pdf, s);
      L.push_back({std::string("  integrity: ") +
                       (vr.integrity_valid ? "valid" : "INVALID"),
                   false});
      if (!vr.signer_name.empty())
        L.push_back({"  signer: " + vr.signer_name, false});
    }
  }

  L.push_back({"", false});
  L.push_back(
      {"Form fields (" + std::to_string(cat.form_fields.size()) + ")", true});
  for (const auto& f : cat.form_fields) {
    if (!f) continue;
    std::string nm = f->full_name.empty() ? f->partial_name : f->full_name;
    std::string val = value_to_string(f->field_value);
    std::string line = std::string(field_type_name(f->type)) + ": " + nm;
    if (!val.empty()) line += " = " + val;
    L.push_back({line, false});
  }
  return L;
}

void draw_info_panel(Viewer& v, lvg_canvas_t* c, int x0, int H) {
  const int w = kRightW;
  const int y0 = kToolbarH;
  std::vector<InfoLine> lines = build_info_lines(v);
  const int lh = v.font ? lui_font_line_height(v.font) : 16;
  const int row = lh + 8;
  int avail = std::max(0, H - y0);
  v.info_scroll =
      clampi(v.info_scroll, 0, std::max(0, (int)lines.size() * row - avail));
  if (!v.font) return;
  for (size_t i = 0; i < lines.size(); ++i) {
    int ry = y0 + 8 + (int)i * row - v.info_scroll;
    if (ry + row <= y0 || ry >= H) continue;
    const InfoLine& L = lines[i];
    if (L.text.empty()) continue;
    std::string t = fit_text(v.font, L.text, w - 24);
    lui_canvas_draw_text(c, x0 + 12, ry, t.c_str(), (int)t.size(), v.font,
                         L.header ? kAccent : kSidebarText);
  }
}

void draw_revisions_panel(Viewer& v, lvg_canvas_t* c, int x0, int H) {
  const int w = kRightW;
  const int y0 = kToolbarH;
  const int lh = v.font ? lui_font_line_height(v.font) : 14;
  int n = (int)v.revisions.size();
  int avail = std::max(0, H - y0);
  v.info_scroll =
      clampi(v.info_scroll, 0, std::max(0, (n + 1) * kRevRowH - avail));
  if (!v.font) return;
  // "Current document" pseudo-row at the top (rev_selected == -1).
  for (int i = -1; i < n; ++i) {
    int ry = y0 + (i + 1) * kRevRowH - v.info_scroll;
    if (ry + kRevRowH <= y0 || ry >= H) continue;
    bool sel = (v.rev_selected == i);
    if (sel) lvg_canvas_fill_rect(c, x0, ry, w, kRevRowH, kSidebarSel);
    char l1[160], l2[200];
    if (i < 0) {
      std::snprintf(l1, sizeof(l1), "Current document");
      l2[0] = '\0';
    } else {
      const auto& r = v.revisions[(size_t)i];
      std::snprintf(l1, sizeof(l1), "Revision %d  (%zu bytes)",
                    (int)r.revision_number, r.size_bytes);
      std::snprintf(l2, sizeof(l2), "+%zu  ~%zu  -%zu%s%s",
                    r.added_objects.size(), r.modified_objects.size(),
                    r.deleted_objects.size(),
                    r.signer_name.empty() ? "" : "  signed: ",
                    r.signer_name.c_str());
    }
    lui_canvas_draw_text(c, x0 + 12, ry + 6, l1, (int)std::strlen(l1), v.font,
                         sel ? kTextColor : kSidebarText);
    if (l2[0])
      lui_canvas_draw_text(c, x0 + 12, ry + 6 + lh + 2, l2,
                           (int)std::strlen(l2), v.font, kTextDim);
  }
}

void draw_right_panel(Viewer& v, lvg_canvas_t* c, int W, int H) {
  const int w = kRightW;
  const int x0 = W - w;
  const int y0 = kToolbarH;
  lvg_rect_t clip = lvg_rect_make(x0, y0, w, std::max(0, H - y0));
  lvg_canvas_set_clip(c, &clip);
  lvg_canvas_fill_rect(c, x0, y0, w, std::max(0, H - y0), kSidebarBg);
  if (v.right_panel == 1)
    draw_info_panel(v, c, x0, H);
  else if (v.right_panel == 2)
    draw_revisions_panel(v, c, x0, H);
  lvg_canvas_reset_clip(c);
  lvg_canvas_fill_rect(c, x0, y0, 1, std::max(0, H - y0), kToolbarLine);
}

// Build a Before-vs-After diff image: light gray = unchanged, green = added
// (ink only in After), red = removed (ink only in Before), blue = changed.
void build_diff(const pdfview::RenderedPage& after,
                const pdfview::RenderedPage& before, pdfview::RenderedPage* out) {
  int w = std::min(after.width, before.width);
  int h = std::min(after.height, before.height);
  out->page_index = after.page_index;
  out->scale = after.scale;
  out->width = w;
  out->height = h;
  if (w <= 0 || h <= 0) {
    out->argb.clear();
    return;
  }
  out->argb.assign((size_t)w * (size_t)h, 0);
  auto lum = [](uint32_t p) -> int {
    int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    return (r * 30 + g * 59 + b * 11) / 100;
  };
  for (int y = 0; y < h; ++y) {
    const uint32_t* ar = after.argb.data() + (size_t)y * after.width;
    const uint32_t* br = before.argb.data() + (size_t)y * before.width;
    uint32_t* o = out->argb.data() + (size_t)y * w;
    for (int x = 0; x < w; ++x) {
      int la = lum(ar[x]), lb = lum(br[x]);
      bool ai = la < 200, bi = lb < 200;  // "ink" = darker than near-white
      if (ai == bi && std::abs(la - lb) < 24) {
        int g = 200 + la * 55 / 255;  // unchanged -> light gray
        o[x] = LVG_COLOR_ARGB(0xFF, g, g, g);
      } else if (ai && !bi) {
        o[x] = LVG_COLOR_RGB(0x1C, 0x88, 0x30);  // added
      } else if (!ai && bi) {
        o[x] = LVG_COLOR_RGB(0xC8, 0x28, 0x28);  // removed
      } else {
        o[x] = LVG_COLOR_RGB(0x20, 0x68, 0xC8);  // changed
      }
    }
  }
  out->surface = lvg_surface_wrap(out->argb.data(), w, h, w);
}

// Render the Before (revision snapshot) of the current page and its diff vs the
// current render @after, caching by (revision, page, scale).
void ensure_rev_render(Viewer& v, const pdfview::RenderedPage* after) {
  if (v.rev_selected < 0 || v.rev_selected >= (int)v.revisions.size()) return;
  if (!after || !after->valid()) return;
  if (v.rev_cache_key == v.rev_selected && v.rev_cache_page == v.page &&
      std::fabs(v.rev_cache_scale - after->scale) < 1e-3f && v.rev_page.valid())
    return;  // cached
  size_t end = v.revisions[(size_t)v.rev_selected].end_offset;
  const std::vector<uint8_t>& data = v.doc.data();
  if (end == 0 || end > data.size()) return;
  nanopdf::Pdf rev_pdf;
  if (!nanopdf::parse_from_memory(data.data(), end, &rev_pdf)) return;
  rev_pdf.load_document_structure();
  if (v.page >= (int)rev_pdf.catalog.pages.size()) {
    v.rev_page.width = 0;
    return;
  }
  if (v.doc.render_external(rev_pdf, v.page, after->scale, &v.rev_page)) {
    build_diff(*after, v.rev_page, &v.diff_page);
    v.rev_cache_key = v.rev_selected;
    v.rev_cache_page = v.page;
    v.rev_cache_scale = after->scale;
  }
}

// Defined below; used by draw() for highlight placement.
void map_quad_to_screen(Viewer& v, const nanopdf::TextQuad& q, int* sx, int* sy,
                        int* sw, int* sh);

void draw(Viewer& v, lvg_surface_t* surf) {
  lvg_canvas_t c;
  lvg_canvas_init(&c, surf);
  lvg_canvas_clear(&c, kBgColor);

  const int W = surf->width;
  const int H = surf->height;
  const int content_x = sidebar_width(v);
  const int right_w = right_width(v);
  const int content_w = std::max(0, W - content_x - right_w);
  const int content_y = kToolbarH;
  const int content_h = std::max(0, H - kToolbarH);

  if (v.doc.loaded()) {
    if (v.fit_width) v.zoom = fit_width_zoom(v, content_w);
    const pdfview::RenderedPage* rp = v.doc.render(v.page, v.zoom);
    // Revision view: render the Before snapshot + diff when a revision is picked.
    bool rev_view = (v.right_panel == 2 && v.rev_selected >= 0);
    if (rev_view) ensure_rev_render(v, rp);
    const pdfview::RenderedPage* show = rp;
    if (rev_view && v.rev_mode == 1 && v.rev_page.valid())
      show = &v.rev_page;
    else if (rev_view && v.rev_mode == 2 && v.diff_page.valid())
      show = &v.diff_page;

    if (rp && rp->valid() && show && show->valid()) {
      const int pw = show->width;
      const int ph = show->height;

      const int max_sx = std::max(0, pw - content_w);
      const int max_sy = std::max(0, ph - content_h);
      v.scroll_x = clampi(v.scroll_x, 0, max_sx);
      v.scroll_y = clampi(v.scroll_y, 0, max_sy);

      const int px = (pw <= content_w) ? content_x + (content_w - pw) / 2
                                       : content_x - v.scroll_x;
      const int py = (ph <= content_h) ? content_y + (content_h - ph) / 2
                                       : content_y - v.scroll_y;
      v.page_px = px;
      v.page_py = py;
      v.page_scale = rp->scale;

      lvg_rect_t clip = lvg_rect_make(content_x, content_y, content_w, content_h);
      lvg_canvas_set_clip(&c, &clip);
      lvg_canvas_fill_rect(&c, px + 4, py + 4, pw, ph, kPageShadow);
      lvg_canvas_draw_image(&c, px, py, pw, ph, &show->surface, nullptr,
                            LVG_IMAGE_FILTER_BILINEAR);

      if (show == rp) {  // overlays apply to the normal (after) view only
        // Search-match highlights.
        if (v.match_page == v.page) {
          for (size_t i = 0; i < v.match_quads.size(); ++i) {
            int sx, sy, sw, sh;
            map_quad_to_screen(v, v.match_quads[i], &sx, &sy, &sw, &sh);
            lvg_canvas_fill_rect(&c, sx, sy, sw, sh,
                                 (int)i == v.match_index ? kMatchCur : kMatchHi);
          }
        }
        // Finalized text selection.
        if (v.sel_page == v.page) {
          for (const auto& q : v.sel_quads) {
            int sx, sy, sw, sh;
            map_quad_to_screen(v, q, &sx, &sy, &sw, &sh);
            lvg_canvas_fill_rect(&c, sx, sy, sw, sh, kSelHi);
          }
        }
        // Live drag rectangle.
        if (v.selecting) {
          int x0 = std::min(v.sel_x0, v.sel_x1);
          int y0 = std::min(v.sel_y0, v.sel_y1);
          int w = std::abs(v.sel_x1 - v.sel_x0);
          int h = std::abs(v.sel_y1 - v.sel_y0);
          lvg_canvas_fill_rect(&c, x0, y0, w, h, kSelHi);
        }
      }
      lvg_canvas_reset_clip(&c);
    }
  }

  if (right_w > 0) draw_right_panel(v, &c, W, H);

  if (content_x > 0) draw_sidebar(v, &c, H);

  // Toolbar (drawn last, spans full width).
  lvg_canvas_fill_rect(&c, 0, 0, W, kToolbarH, kToolbarColor);
  lvg_canvas_fill_rect(&c, 0, kToolbarH - 1, W, 1, kToolbarLine);
  if (v.font) {
    int lh = lui_font_line_height(v.font);
    int ty = (kToolbarH - lh) / 2;
    if (v.search_active || !v.query.empty()) {
      // Search box (left) + page/zoom status (right).
      char sb[320];
      int nmatch = (v.match_page >= 0) ? (int)v.match_quads.size() : 0;
      std::snprintf(sb, sizeof(sb), "Search: %s%s    [%s%d match%s]",
                    v.query.c_str(), v.search_active ? "_" : "",
                    nmatch ? "" : "no ", nmatch, nmatch == 1 ? "" : "es");
      lui_canvas_draw_text(&c, 14, ty, sb, (int)std::strlen(sb), v.font,
                           v.search_active ? kAccent : kTextColor);
    } else {
      update_status(v);
      lui_canvas_draw_text(&c, 14, ty, v.status, (int)std::strlen(v.status),
                           v.font, kTextColor);
    }
  }
  if (!v.doc.loaded() && v.font) {
    const char* hint = "Press 'o' to open a PDF, or pass a path on the CLI";
    lui_canvas_draw_text(&c, 24, content_y + 24, hint, (int)std::strlen(hint),
                         v.font, kTextDim);
  }
}

void go_to_page(Viewer& v, int page) {
  int n = v.doc.page_count();
  if (n <= 0) return;
  v.page = clampi(page, 0, n - 1);
  v.scroll_y = 0;
  v.scroll_x = 0;
}

int utf8_char_count(const std::string& s) {
  int n = 0;
  for (unsigned char ch : s)
    if ((ch & 0xC0) != 0x80) ++n;  // count non-continuation bytes
  return n;
}

// Map a text quad (PDF user space, y-up) to a screen rect using the current
// page placement recorded by draw().
void map_quad_to_screen(Viewer& v, const nanopdf::TextQuad& q, int* sx, int* sy,
                        int* sw, int* sh) {
  double left, bottom, right, top;
  v.doc.page_box(v.page, &left, &bottom, &right, &top);
  float s = v.page_scale;
  *sx = v.page_px + (int)((q.x - left) * s);
  *sy = v.page_py + (int)((top - (q.y + q.height)) * s);
  *sw = std::max(1, (int)(q.width * s));
  *sh = std::max(1, (int)(q.height * s));
}

// Inverse map: a screen point to PDF user-space coords on the current page.
void screen_to_page(Viewer& v, int sx, int sy, double* gx, double* gy) {
  double left, bottom, right, top;
  v.doc.page_box(v.page, &left, &bottom, &right, &top);
  float s = (v.page_scale > 0) ? v.page_scale : 1.0f;
  *gx = left + (sx - v.page_px) / s;
  *gy = top - (sy - v.page_py) / s;
}

// Collect one bounding box per match of @q on @page (PDF coords).
std::vector<nanopdf::TextQuad> collect_matches(Viewer& v, int page,
                                               const std::string& q) {
  std::vector<nanopdf::TextQuad> out;
  if (q.empty()) return out;
  const nanopdf::TextPage* tp = v.doc.text_page(page);
  if (!tp) return out;
  std::vector<int> starts = tp->find_text(q);
  int qlen = utf8_char_count(q);
  for (int s : starts) {
    nanopdf::TextSelectionResult sel =
        tp->select_text_range((size_t)s, (size_t)qlen);
    out.push_back(sel.bounds);
  }
  return out;
}

// Scroll the page so the focused match is roughly one-third down the viewport.
void scroll_to_current_match(Viewer& v, int content_h) {
  if (v.match_page != v.page || v.match_index < 0 ||
      v.match_index >= (int)v.match_quads.size())
    return;
  double left, bottom, right, top;
  v.doc.page_box(v.page, &left, &bottom, &right, &top);
  float s = (v.page_scale > 0) ? v.page_scale : v.zoom;
  const nanopdf::TextQuad& qd = v.match_quads[v.match_index];
  int img_y = (int)((top - (qd.y + qd.height)) * s);
  v.scroll_y = clampi(img_y - content_h / 3, 0, 1 << 30);  // draw re-clamps
}

// Search from @start_page in @dir (+1/-1) for the first page with a match.
void run_search(Viewer& v, int start_page, int dir, int content_h) {
  v.match_page = -1;
  v.match_quads.clear();
  v.match_index = -1;
  int n = v.doc.page_count();
  if (n <= 0 || v.query.empty()) return;
  for (int k = 0; k < n; ++k) {
    int p = (((start_page + dir * k) % n) + n) % n;
    std::vector<nanopdf::TextQuad> m = collect_matches(v, p, v.query);
    if (!m.empty()) {
      go_to_page(v, p);
      v.match_page = p;
      v.match_quads = std::move(m);
      v.match_index = (dir >= 0) ? 0 : (int)v.match_quads.size() - 1;
      scroll_to_current_match(v, content_h);
      return;
    }
  }
}

// Load a document and reset view state. @win may be null (headless modes).
bool load_document(Viewer& v, lui_window_t* win, const char* path) {
  if (!v.doc.load_file(path)) {
    std::fprintf(stderr, "pdfview: %s\n", v.doc.error().c_str());
    return false;
  }
  const char* slash = std::strrchr(path, '/');
  v.title = slash ? slash + 1 : path;
  v.page = 0;
  v.scroll_x = 0;
  v.scroll_y = 0;
  v.fit_width = true;
  v.sidebar_scroll = 0;
  v.thumb_scroll = 0;
  v.sidebar_mode = v.doc.outline().empty() ? 1 : 0;  // Pages if no bookmarks
  if (win) lui_window_set_title(win, v.title.c_str());
  std::printf("pdfview: loaded %s (%d pages)\n", path, v.doc.page_count());
  return true;
}

// Show a native Open dialog (xdg-desktop-portal) and load the chosen PDF.
void open_via_dialog(Viewer& v, lui_window_t* win) {
#if PDFVIEW_HAVE_NFD
  nfdu8char_t* out = nullptr;
  const nfdu8filteritem_t filters[1] = {{"PDF Document", "pdf"}};
  nfdresult_t r = NFD_OpenDialogU8(&out, filters, 1, nullptr);
  if (r == NFD_OKAY && out) {
    load_document(v, win, out);
    NFD_FreePathU8(out);
  } else if (r == NFD_ERROR) {
    std::fprintf(stderr, "pdfview: open dialog error: %s\n", NFD_GetError());
  }
#else
  (void)v;
  (void)win;
  std::fprintf(stderr, "pdfview: built without native file dialog\n");
#endif
}

}  // namespace

int main(int argc, char** argv) {
  // Register nanopdf's bundled substitute + CJK fonts so rendering resolves
  // Standard-14 and Japanese glyphs (shared fonts, no compile-time embedding).
  int nfonts = pdfview::register_bundled_fonts(PDFVIEW_FONTS_DIR);
  std::fprintf(stderr, "pdfview: registered %d bundled font faces\n", nfonts);

  // Headless render-path self-test (no window/X11).
  if (argc > 1 && std::strcmp(argv[1], "--selftest") == 0) {
    const char* out = (argc > 2) ? argv[2] : "/tmp/pdfview_selftest.ppm";
    lvg_surface_t* s = lvg_surface_create(kInitialWidth, kInitialHeight);
    if (!s) return 1;
    lvg_canvas_t c;
    lvg_canvas_init(&c, s);
    lvg_canvas_clear(&c, kBgColor);
    lvg_canvas_fill_rect(&c, 0, 0, s->width, kToolbarH, kToolbarColor);
    const lvg_pointf_t pts[] = {{100, 100}, {300, 100}, {300, 300}, {100, 300},
                                {160, 160}, {160, 240}, {240, 240}, {240, 160}};
    const int clens[] = {4, 4};
    lvg_canvas_fill_polygonsf_ex(&c, pts, 8, clens, 2,
                                 LVG_COLOR_RGB(0x4A, 0x9E, 0xFF),
                                 LVG_FILL_RULE_NONZERO);
    dump_surface_ppm(s, out);
    std::fprintf(stderr, "selftest: wrote %dx%d to %s\n", s->width, s->height,
                 out);
    lvg_surface_destroy(s);
    return 0;
  }

  // Headless single-page render check.
  //   pdfview --renderpage <pdf> <page1based> <out.ppm> [scale]
  if (argc > 1 && std::strcmp(argv[1], "--renderpage") == 0) {
    if (argc < 5) {
      std::fprintf(stderr,
                   "usage: pdfview --renderpage <pdf> <page> <out.ppm> "
                   "[scale]\n");
      return 2;
    }
    int page1 = std::atoi(argv[3]);
    float scale = (argc > 5) ? (float)std::atof(argv[5]) : 1.5f;
    pdfview::PdfDocument doc;
    if (!doc.load_file(argv[2])) {
      std::fprintf(stderr, "load failed: %s\n", doc.error().c_str());
      return 1;
    }
    const pdfview::RenderedPage* rp = doc.render(page1 - 1, scale);
    if (!rp || !rp->valid()) {
      std::fprintf(stderr, "render failed: %s\n", doc.error().c_str());
      return 1;
    }
    dump_surface_ppm(&rp->surface, argv[4]);
    std::fprintf(stderr, "rendered page %d at scale %.2f -> %dx%d -> %s\n",
                 page1, scale, rp->width, rp->height, argv[4]);
    return 0;
  }

  // Headless full-frame composition check: run the exact draw() the window loop
  // uses (fit-width zoom, page blit + clip, toolbar, text) onto an offscreen
  // surface and dump it.
  //   pdfview --compose <pdf> <out.ppm> [W H]
  if (argc > 1 && std::strcmp(argv[1], "--compose") == 0) {
    if (argc < 4) {
      std::fprintf(stderr, "usage: pdfview --compose <pdf> <out.ppm> [W H]\n");
      return 2;
    }
    int W = (argc > 5) ? std::atoi(argv[4]) : kInitialWidth;
    int H = (argc > 6) ? std::atoi(argv[5]) : kInitialHeight;
    if (W <= 0) W = kInitialWidth;
    if (H <= 0) H = kInitialHeight;
    lvg_surface_t* s = lvg_surface_create(W, H);
    if (!s) return 1;
    Viewer v;
    v.font = lui_font_create(PDFVIEW_FONTS_DIR "/arimo/Arimo-Regular.ttf", 16);
    if (!v.doc.load_file(argv[2])) {
      std::fprintf(stderr, "load failed: %s\n", v.doc.error().c_str());
      return 1;
    }
    const char* slash = std::strrchr(argv[2], '/');
    v.title = slash ? slash + 1 : argv[2];
    draw(v, s);
    dump_surface_ppm(s, argv[3]);
    std::fprintf(stderr, "composed %dx%d frame (page 1, fit-width) -> %s\n", W, H,
                 argv[3]);
    if (v.font) lui_font_destroy(v.font);
    lvg_surface_destroy(s);
    return 0;
  }

  // Headless search check: load, search for a term, and dump the first page
  // with matches (highlights drawn).  pdfview --search <pdf> <term> <out.ppm>
  if (argc > 1 && std::strcmp(argv[1], "--search") == 0) {
    if (argc < 5) {
      std::fprintf(stderr, "usage: pdfview --search <pdf> <term> <out.ppm>\n");
      return 2;
    }
    lvg_surface_t* s = lvg_surface_create(kInitialWidth, kInitialHeight);
    if (!s) return 1;
    Viewer v;
    v.font = lui_font_create(PDFVIEW_FONTS_DIR "/arimo/Arimo-Regular.ttf", 16);
    if (!v.doc.load_file(argv[2])) {
      std::fprintf(stderr, "load failed: %s\n", v.doc.error().c_str());
      return 1;
    }
    const char* slash = std::strrchr(argv[2], '/');
    v.title = slash ? slash + 1 : argv[2];
    v.sidebar_mode = v.doc.outline().empty() ? 1 : 0;
    v.query = argv[3];
    draw(v, s);  // establish page placement/scale for page 0
    run_search(v, 0, +1, kInitialHeight - kToolbarH);
    draw(v, s);  // re-draw the matched page with highlights
    dump_surface_ppm(s, argv[4]);
    int nmatch = (v.match_page >= 0) ? (int)v.match_quads.size() : 0;
    std::fprintf(stderr, "search '%s': %d match(es) on page %d -> %s\n", argv[3],
                 nmatch, v.match_page + 1, argv[4]);
    if (v.font) lui_font_destroy(v.font);
    lvg_surface_destroy(s);
    return 0;
  }

  // Headless info-panel check.  pdfview --info <pdf> <out.ppm>
  if (argc > 1 && std::strcmp(argv[1], "--info") == 0) {
    if (argc < 4) {
      std::fprintf(stderr, "usage: pdfview --info <pdf> <out.ppm>\n");
      return 2;
    }
    lvg_surface_t* s = lvg_surface_create(kInitialWidth, kInitialHeight);
    if (!s) return 1;
    Viewer v;
    v.font = lui_font_create(PDFVIEW_FONTS_DIR "/arimo/Arimo-Regular.ttf", 16);
    if (!v.doc.load_file(argv[2])) {
      std::fprintf(stderr, "load failed: %s\n", v.doc.error().c_str());
      return 1;
    }
    const char* slash = std::strrchr(argv[2], '/');
    v.title = slash ? slash + 1 : argv[2];
    v.sidebar_mode = v.doc.outline().empty() ? 1 : 0;
    v.right_panel = 1;
    draw(v, s);
    dump_surface_ppm(s, argv[3]);
    std::fprintf(stderr, "info panel -> %s\n", argv[3]);
    if (v.font) lui_font_destroy(v.font);
    lvg_surface_destroy(s);
    return 0;
  }

  // Headless revision-view check.  pdfview --rev <pdf> <out.ppm> [before|diff]
  if (argc > 1 && std::strcmp(argv[1], "--rev") == 0) {
    if (argc < 4) {
      std::fprintf(stderr, "usage: pdfview --rev <pdf> <out.ppm> [before|diff]\n");
      return 2;
    }
    lvg_surface_t* s = lvg_surface_create(kInitialWidth, kInitialHeight);
    if (!s) return 1;
    Viewer v;
    v.font = lui_font_create(PDFVIEW_FONTS_DIR "/arimo/Arimo-Regular.ttf", 16);
    if (!v.doc.load_file(argv[2])) {
      std::fprintf(stderr, "load failed: %s\n", v.doc.error().c_str());
      return 1;
    }
    const char* slash = std::strrchr(argv[2], '/');
    v.title = slash ? slash + 1 : argv[2];
    v.sidebar_mode = v.doc.outline().empty() ? 1 : 0;
    v.right_panel = 2;
    const nanopdf::Pdf* pdf = v.doc.pdf();
    if (pdf) v.revisions = nanopdf::detect_revision_history(*pdf).revisions;
    v.revisions_loaded = true;
    v.rev_selected = v.revisions.empty() ? -1 : 0;  // earliest revision
    v.rev_mode = 1;  // Before
    if (argc > 4 && std::strcmp(argv[4], "diff") == 0) v.rev_mode = 2;
    draw(v, s);
    dump_surface_ppm(s, argv[3]);
    std::fprintf(stderr, "revisions: %zu; showing rev %d mode %d -> %s\n",
                 v.revisions.size(), v.rev_selected, v.rev_mode, argv[3]);
    if (v.font) lui_font_destroy(v.font);
    lvg_surface_destroy(s);
    return 0;
  }

  const char* initial_path = (argc > 1) ? argv[1] : nullptr;

  if (!lui_init()) {
    std::fprintf(stderr, "pdfview: lui_init() failed\n");
    return 1;
  }

  lui_window_t* win = lui_window_create("nanopdf viewer", kInitialWidth,
                                        kInitialHeight,
                                        LUI_WINDOW_RESIZABLE | LUI_WINDOW_HDPI);
  if (!win) {
    std::fprintf(stderr, "pdfview: lui_window_create() failed\n");
    lui_shutdown();
    return 1;
  }
  lui_window_show(win);

#if PDFVIEW_HAVE_NFD
  NFD_Init();
#endif

  Viewer viewer;
  viewer.font = lui_font_create(PDFVIEW_FONTS_DIR "/arimo/Arimo-Regular.ttf", 16);
  if (!viewer.font) {
    std::fprintf(stderr, "pdfview: warning: UI font not found at %s\n",
                 PDFVIEW_FONTS_DIR "/arimo/Arimo-Regular.ttf");
  }

  if (initial_path) load_document(viewer, win, initial_path);

  bool running = true;
  bool dirty = true;
  while (running) {
    if (dirty) {
      lvg_surface_t* surface = lui_window_get_surface(win);
      if (!surface) break;
      draw(viewer, surface);
      lui_window_present(win);
      dirty = false;
    }

    lui_event_t event;
    if (!lui_window_wait_event(win, &event)) break;
    do {
      int W = 0, H = 0;
      lui_window_get_physical_size(win, &W, &H);
      int lw = 0, lh = 0;
      lui_window_get_size(win, &lw, &lh);
      const float dpi = (lw > 0) ? (float)W / (float)lw : 1.0f;
      const int content_x = sidebar_width(viewer);
      const int right_w = right_width(viewer);
      const int content_w = std::max(1, W - content_x - right_w);
      const int content_h = std::max(1, H - kToolbarH);
      const int scroll_step = std::max(40, (int)(content_h * 0.9f));

      switch (event.type) {
        case LUI_EVENT_QUIT:
          running = false;
          break;
        case LUI_EVENT_WINDOW_EXPOSE:
        case LUI_EVENT_WINDOW_RESIZE:
          dirty = true;
          break;
        case LUI_EVENT_KEY_DOWN: {
          int k = event.data.key.key;
          if (viewer.search_active) {
            // While typing a query, keys edit/commit the search.
            if (k == LUI_KEY_ESCAPE) {
              viewer.search_active = false;
              viewer.query.clear();
              viewer.match_page = -1;
              viewer.match_quads.clear();
              viewer.match_index = -1;
            } else if (k == LUI_KEY_RETURN) {
              viewer.search_active = false;
              run_search(viewer, viewer.page, +1, content_h);
            } else if (k == LUI_KEY_BACKSPACE && !viewer.query.empty()) {
              size_t i = viewer.query.size();
              do {
                --i;
              } while (i > 0 && ((unsigned char)viewer.query[i] & 0xC0) == 0x80);
              viewer.query.erase(i);
            }
            dirty = true;
            break;  // consume all keys while the search box is focused
          }
          if (k == LUI_KEY_ESCAPE) {
            // Clear search/selection first; quit only when nothing to clear.
            if (!viewer.query.empty() || viewer.sel_page >= 0) {
              viewer.query.clear();
              viewer.match_page = -1;
              viewer.match_quads.clear();
              viewer.match_index = -1;
              viewer.sel_page = -1;
              viewer.sel_quads.clear();
              viewer.sel_text.clear();
              dirty = true;
            } else {
              running = false;
            }
          } else if (k == LUI_KEY_LEFT || k == kKeyPageUp) {
            go_to_page(viewer, viewer.page - 1);
            dirty = true;
          } else if (k == LUI_KEY_RIGHT || k == kKeyPageDown) {
            go_to_page(viewer, viewer.page + 1);
            dirty = true;
          } else if (k == LUI_KEY_HOME) {
            go_to_page(viewer, 0);
            dirty = true;
          } else if (k == LUI_KEY_END) {
            go_to_page(viewer, viewer.doc.page_count() - 1);
            dirty = true;
          } else if (k == LUI_KEY_UP) {
            viewer.scroll_y -= scroll_step;
            dirty = true;
          } else if (k == LUI_KEY_DOWN) {
            viewer.scroll_y += scroll_step;
            dirty = true;
          }
          break;
        }
        case LUI_EVENT_TEXT_INPUT: {
          char ch = event.data.text.text[0];
          if (viewer.search_active) {
            if ((unsigned char)ch >= 0x20) {  // printable (incl. UTF-8 lead)
              viewer.query += event.data.text.text;
              dirty = true;
            }
            break;
          }
          if (ch == '/') {
            viewer.search_active = true;
            viewer.query.clear();
            viewer.match_page = -1;
            viewer.match_quads.clear();
            viewer.match_index = -1;
            dirty = true;
          } else if (ch == 'n') {
            if (viewer.match_page >= 0 &&
                viewer.match_index + 1 < (int)viewer.match_quads.size()) {
              viewer.match_index++;
              scroll_to_current_match(viewer, content_h);
            } else if (!viewer.query.empty()) {
              run_search(viewer, viewer.page + 1, +1, content_h);
            }
            dirty = true;
          } else if (ch == 'N') {
            if (viewer.match_page >= 0 && viewer.match_index > 0) {
              viewer.match_index--;
              scroll_to_current_match(viewer, content_h);
            } else if (!viewer.query.empty()) {
              run_search(viewer, viewer.page - 1, -1, content_h);
            }
            dirty = true;
          } else if (ch == '+' || ch == '=') {
            viewer.fit_width = false;
            viewer.zoom = clampf(viewer.zoom * kZoomStep, kMinZoom, kMaxZoom);
            dirty = true;
          } else if (ch == '-' || ch == '_') {
            viewer.fit_width = false;
            viewer.zoom = clampf(viewer.zoom / kZoomStep, kMinZoom, kMaxZoom);
            dirty = true;
          } else if (ch == '0') {
            viewer.fit_width = true;
            dirty = true;
          } else if (ch == '1') {
            viewer.fit_width = false;
            viewer.zoom = 96.0f / 72.0f;  // ~100% at 96 DPI
            dirty = true;
          } else if (ch == 'f' || ch == 'F') {
            viewer.fit_width = false;
            viewer.zoom = fit_page_zoom(viewer, content_w, content_h);
            dirty = true;
          } else if (ch == 'j') {
            go_to_page(viewer, viewer.page + 1);
            dirty = true;
          } else if (ch == 'k') {
            go_to_page(viewer, viewer.page - 1);
            dirty = true;
          } else if (ch == 'o' || ch == 'O') {
            open_via_dialog(viewer, win);
            dirty = true;
          } else if (ch == 'b' || ch == 'B') {
            viewer.show_sidebar = !viewer.show_sidebar;
            dirty = true;
          } else if (ch == 't' || ch == 'T') {
            viewer.show_sidebar = true;
            viewer.sidebar_mode = 1;  // Pages (thumbnails)
            dirty = true;
          } else if (ch == 'i' || ch == 'I') {
            viewer.right_panel = (viewer.right_panel == 1) ? 0 : 1;
            viewer.info_scroll = 0;
            dirty = true;
          } else if (ch == 'r' || ch == 'R') {
            viewer.right_panel = (viewer.right_panel == 2) ? 0 : 2;
            viewer.info_scroll = 0;
            if (viewer.right_panel == 2 && !viewer.revisions_loaded) {
              const nanopdf::Pdf* pdf = viewer.doc.pdf();
              if (pdf)
                viewer.revisions =
                    nanopdf::detect_revision_history(*pdf).revisions;
              viewer.revisions_loaded = true;
            }
            dirty = true;
          } else if (ch == 'd' || ch == 'D') {
            if (viewer.right_panel == 2 && viewer.rev_selected >= 0) {
              viewer.rev_mode = (viewer.rev_mode + 1) % 3;  // After/Before/Diff
              dirty = true;
            }
          }
          break;
        }
        case LUI_EVENT_MOUSE_MOVE: {
          viewer.mouse_x = (int)(event.data.mouse_move.x * dpi);
          viewer.mouse_y = (int)(event.data.mouse_move.y * dpi);
          if (viewer.selecting) {
            viewer.sel_x1 = viewer.mouse_x;
            viewer.sel_y1 = viewer.mouse_y;
            dirty = true;
          }
          break;
        }
        case LUI_EVENT_MOUSE_DOWN: {
          if (event.data.mouse_button.button != LUI_MOUSE_LEFT) break;
          int mx = (int)(event.data.mouse_button.x * dpi);
          int my = (int)(event.data.mouse_button.y * dpi);
          viewer.mouse_x = mx;
          viewer.mouse_y = my;
          const int panel_y = sidebar_panel_y();
          if (content_x > 0 && mx < content_x) {  // sidebar
            if (my >= kToolbarH && my < panel_y) {
              int mode = (mx < kSidebarW / 2) ? 0 : 1;
              if (mode == 0 && !has_outline(viewer)) mode = 1;
              viewer.sidebar_mode = mode;
              dirty = true;
            } else if (my >= panel_y) {
              if (viewer.sidebar_mode == 0 && has_outline(viewer)) {
                const std::vector<pdfview::Bookmark>& bm = viewer.doc.outline();
                int row = (my - panel_y + viewer.sidebar_scroll) / kRowH;
                if (row >= 0 && row < (int)bm.size() && bm[row].page >= 0) {
                  go_to_page(viewer, bm[row].page);
                  dirty = true;
                }
              } else {
                int row = (my - panel_y + viewer.thumb_scroll) / kThumbCellH;
                if (row >= 0 && row < viewer.doc.page_count()) {
                  go_to_page(viewer, row);
                  dirty = true;
                }
              }
            }
            break;
          }
          if (right_w > 0 && mx >= W - right_w) {  // right panel
            if (viewer.right_panel == 2 && my >= kToolbarH) {
              int rrow = (my - kToolbarH + viewer.info_scroll) / kRevRowH;
              int idx = rrow - 1;  // row 0 = "current document"
              if (idx >= -1 && idx < (int)viewer.revisions.size()) {
                viewer.rev_selected = idx;
                viewer.rev_mode = (idx < 0) ? 0 : 1;  // current=After, rev=Before
                dirty = true;
              }
            }
            break;
          }
          // Page area: begin a drag selection.
          if (my >= kToolbarH && mx >= content_x && mx < W - right_w &&
              viewer.doc.loaded()) {
            viewer.selecting = true;
            viewer.sel_x0 = viewer.sel_x1 = mx;
            viewer.sel_y0 = viewer.sel_y1 = my;
            viewer.sel_page = -1;
            viewer.sel_quads.clear();
            viewer.sel_text.clear();
          }
          break;
        }
        case LUI_EVENT_MOUSE_UP: {
          if (!viewer.selecting) break;
          viewer.selecting = false;
          int mx = (int)(event.data.mouse_button.x * dpi);
          int my = (int)(event.data.mouse_button.y * dpi);
          viewer.sel_x1 = mx;
          viewer.sel_y1 = my;
          // Tiny drag = click, not a selection.
          if (std::abs(viewer.sel_x1 - viewer.sel_x0) < 3 &&
              std::abs(viewer.sel_y1 - viewer.sel_y0) < 3) {
            dirty = true;
            break;
          }
          const nanopdf::TextPage* tp = viewer.doc.text_page(viewer.page);
          if (tp) {
            double gx0, gy0, gx1, gy1;
            screen_to_page(viewer, viewer.sel_x0, viewer.sel_y0, &gx0, &gy0);
            screen_to_page(viewer, viewer.sel_x1, viewer.sel_y1, &gx1, &gy1);
            nanopdf::TextSelectionResult sel =
                tp->select_text_in_rect(gx0, gy0, gx1, gy1);
            viewer.sel_text = sel.text;
            viewer.sel_page = viewer.page;
            viewer.sel_quads.clear();
            for (const auto& seg : sel.segments)
              viewer.sel_quads.push_back(seg.quad);
            if (!sel.text.empty())
              std::printf("pdfview: selected: %s\n", sel.text.c_str());
          }
          dirty = true;
          break;
        }
        case LUI_EVENT_SCROLL: {
          // Route the wheel to whatever the cursor is over.
          if (right_w > 0 && viewer.mouse_x >= W - right_w) {
            viewer.info_scroll += (int)(event.data.scroll.delta_y * 60.0f);
          } else if (content_x > 0 && viewer.mouse_x < content_x) {
            if (viewer.sidebar_mode == 0 && has_outline(viewer))
              viewer.sidebar_scroll += (int)(event.data.scroll.delta_y * 60.0f);
            else
              viewer.thumb_scroll += (int)(event.data.scroll.delta_y * 120.0f);
          } else {
            viewer.scroll_y += (int)(event.data.scroll.delta_y * 80.0f);
            viewer.scroll_x += (int)(event.data.scroll.delta_x * 80.0f);
          }
          dirty = true;
          break;
        }
        default:
          break;
      }
    } while (running && lui_window_poll_event(win, &event));
  }

  if (viewer.font) lui_font_destroy(viewer.font);
#if PDFVIEW_HAVE_NFD
  NFD_Quit();
#endif
  lui_window_destroy(win);
  lui_shutdown();
  return 0;
}
