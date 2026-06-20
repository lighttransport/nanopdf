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
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pdf_document.hh"
#include "pdf-writer.hh"
#include "pdf_debug.hh"
#include "pdf_sign.hh"

#if PDFVIEW_HAVE_NFD
extern "C" {
#include <nfd.h>
}
#endif

#if PDFVIEW_HAVE_MCP
extern "C" {
#include <lightui/mcp.h>
#include "lui_json.h"
}
#include <unistd.h>  // usleep
#endif

// stb_image_write encoders (compiled into libnanopdf.a). Declared here to avoid
// pulling in the implementation header; signatures match stb_image_write.h.
extern "C" {
int stbi_write_png(const char* filename, int w, int h, int comp,
                   const void* data, int stride_in_bytes);
int stbi_write_jpg(const char* filename, int w, int h, int comp,
                   const void* data, int quality);
}

namespace {

constexpr int kInitialWidth = 1100;   // logical window size (lightui scales it)
constexpr int kInitialHeight = 800;
// UI chrome metrics, in physical pixels. lightui hands us a physical-resolution
// surface (logical * dpi_scale), so on a HiDPI display these base (1x) values
// are scaled by the DPI factor via apply_ui_scale(), called at startup and
// whenever the window moves to a monitor with a different scale. The kXxxBase
// constants hold the 1x design values; the mutable kXxx hold the scaled values
// actually used when drawing.
constexpr int kToolbarHBase = 44;
constexpr int kPageMarginBase = 16;    // gap around the page in the content area
constexpr int kSidebarWBase = 300;     // sidebar width
constexpr int kRowHBase = 24;          // outline row height
constexpr int kTabHBase = 30;          // sidebar tab-bar height
constexpr int kThumbWBase = 220;       // thumbnail render target width
constexpr int kThumbImgHBase = 200;    // thumbnail image area height per cell
constexpr int kThumbCellHBase = 234;   // full thumbnail cell height (img+label)
constexpr int kThumbMarginBase = 34;   // horizontal margin around thumbnails
constexpr int kRightWBase = 340;       // right panel width
constexpr int kRevRowHBase = 50;       // revision-row height
int kToolbarH = kToolbarHBase;
int kPageMargin = kPageMarginBase;
int kSidebarW = kSidebarWBase;
int kRowH = kRowHBase;
int kTabH = kTabHBase;
int kThumbW = kThumbWBase;
int kThumbImgH = kThumbImgHBase;
int kThumbCellH = kThumbCellHBase;
int kThumbMargin = kThumbMarginBase;
int kRightW = kRightWBase;
int kRevRowH = kRevRowHBase;
// Current UI scale (physical px per 1x design px). Read by interaction code to
// scale hardcoded pixel deltas (scroll speed, drag threshold, shadow offset).
float g_ui_scale = 1.0f;
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
constexpr lvg_color_t kDbgHi = LVG_COLOR_ARGB(0x44, 0x2C, 0xE0, 0xC0);     // debug hit
constexpr lvg_color_t kDbgSel = LVG_COLOR_ARGB(0x66, 0xFF, 0x40, 0xC0);    // selected hit

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
  std::string doc_path;    // full path of the loaded document (for saving)
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

  // Go-to-page input.
  bool goto_active = false;
  std::string goto_buf;

  // Search.
  bool search_active = false;   // typing in the search box
  std::string query;
  int match_page = -1;          // page the current matches belong to
  std::vector<nanopdf::TextQuad> match_quads;  // one bbox per match (page coords)
  int match_index = -1;         // currently focused match

  // Text-markup annotations created this session (page + page-space quads).
  struct Markup {
    int page;
    std::vector<nanopdf::TextQuad> quads;
    nanopdf::MarkupType type;
  };
  std::vector<Markup> markups;

  // Sticky-note annotations (page + page-space anchor + text).
  struct Note {
    int page;
    double x, y;
    std::string text;
  };
  std::vector<Note> notes;
  bool note_placing = false;  // waiting for a click to drop a note
  bool note_typing = false;   // editing notes.back().text

  // Drag selection.
  bool selecting = false;
  int sel_x0 = 0, sel_y0 = 0, sel_x1 = 0, sel_y1 = 0;  // screen px
  int sel_page = -1;
  std::string sel_text;
  std::vector<nanopdf::TextQuad> sel_quads;  // selected segment bboxes (page coords)

  // Right-side panel: 0 = hidden, 1 = info, 2 = revisions, 3 = debug.
  int right_panel = 0;
  int info_scroll = 0;

  // Debug inspector (Phase: region -> PDF objects).
  bool debug_mode = false;                    // drag selects a region to inspect
  std::vector<pdfview::RegionHit> debug_hits;  // hits from the last inspect
  int debug_page = -1;                        // page the hits belong to
  int debug_sel = -1;                         // selected hit (drives dump)
  std::string debug_dump;                     // serialized object for the selection

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

  // View rotation in degrees clockwise (0/90/180/270), applied to all pages.
  int rotation = 0;
  std::vector<uint32_t> rot_argb;   // rotated copy of the current page
  lvg_surface_t rot_surface{};
  int rot_rotation = -1;
  const uint32_t* rot_src = nullptr;  // source buffer the cache was built from

  // Current page placement (physical px), set by draw() for coord mapping.
  int page_px = 0, page_py = 0;
  float page_scale = 1.0f;

  int screenshot_seq = 0;  // counter for auto-named screenshot files
  std::string toast;       // transient message (e.g. "Saved screenshot");
                           // shown in the toolbar until the next input

  // "Open file" button rect (physical px), set by draw() in the empty state so
  // MOUSE_DOWN can hit-test it. Zero-size when not shown.
  int open_btn_x = 0, open_btn_y = 0, open_btn_w = 0, open_btn_h = 0;

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

// Lower-cased file extension (without the dot), e.g. "out.PNG" -> "png".
std::string file_ext_lower(const std::string& path) {
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return std::string();
  std::string ext = path.substr(dot + 1);
  for (char& ch : ext) ch = (char)std::tolower((unsigned char)ch);
  return ext;
}

// Save a surface as PNG or JPG (chosen by @path extension; PNG default). The
// surface holds 0xAARRGGBB pixels with a row stride that may exceed width, so
// pack a tight RGB buffer first. Returns false on failure.
bool save_surface_image(const lvg_surface_t* s, const std::string& path) {
  if (!s || !s->pixels || s->width <= 0 || s->height <= 0) return false;
  const int w = s->width, h = s->height;
  std::vector<unsigned char> rgb((size_t)w * h * 3);
  for (int y = 0; y < h; ++y) {
    const uint32_t* row = s->pixels + (size_t)y * s->stride;
    unsigned char* dst = rgb.data() + (size_t)y * w * 3;
    for (int x = 0; x < w; ++x) {
      uint32_t p = row[x];
      dst[x * 3 + 0] = (unsigned char)((p >> 16) & 0xFF);
      dst[x * 3 + 1] = (unsigned char)((p >> 8) & 0xFF);
      dst[x * 3 + 2] = (unsigned char)(p & 0xFF);
    }
  }
  std::string ext = file_ext_lower(path);
  int ok;
  if (ext == "jpg" || ext == "jpeg")
    ok = stbi_write_jpg(path.c_str(), w, h, 3, rgb.data(), 90);
  else
    ok = stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3);
  return ok != 0;
}

// Scale all physical-pixel UI metrics by the display's DPI factor. Idempotent
// (always derived from the 1x base values), so it can be re-applied when the
// window moves to a monitor with a different scale. Headless 1x render paths
// never call it, so the metrics stay at their base values there.
void apply_ui_scale(float s) {
  if (s < 1.0f) s = 1.0f;
  g_ui_scale = s;
  auto sc = [&](int base) { return (int)(base * s + 0.5f); };
  kToolbarH = sc(kToolbarHBase);
  kPageMargin = sc(kPageMarginBase);
  kSidebarW = sc(kSidebarWBase);
  kRowH = sc(kRowHBase);
  kTabH = sc(kTabHBase);
  kThumbW = sc(kThumbWBase);
  kThumbImgH = sc(kThumbImgHBase);
  kThumbCellH = sc(kThumbCellHBase);
  kThumbMargin = sc(kThumbMarginBase);
  kRightW = sc(kRightWBase);
  kRevRowH = sc(kRevRowHBase);
}

// Scale a 1x-design pixel delta (scroll amounts, thresholds) by the UI scale.
int ui_px(float base) { return (int)(base * g_ui_scale + 0.5f); }

constexpr int kUiFontBase = 16;  // UI font size at 1x (physical px = base*scale)
const char* const kUiFontPath = PDFVIEW_FONTS_DIR "/arimo/Arimo-Regular.ttf";

// Apply a UI scale: rescale metrics and (re)create the UI font at the matching
// size. Destroys @old on success; on font-load failure keeps @old. Returns the
// font to use.
lui_font_t* set_ui_scale(float s, lui_font_t* old) {
  if (s < 1.0f) s = 1.0f;
  apply_ui_scale(s);
  lui_font_t* f = lui_font_create(kUiFontPath, (int)(kUiFontBase * s + 0.5f));
  if (!f) return old;
  if (old) lui_font_destroy(old);
  return f;
}

// Rotate an ARGB buffer by @rot degrees clockwise (0/90/180/270) into @dst.
void rotate_argb(const uint32_t* src, int sw, int sh, int rot,
                 std::vector<uint32_t>& dst, int* dw, int* dh) {
  if (rot == 90 || rot == 270) { *dw = sh; *dh = sw; } else { *dw = sw; *dh = sh; }
  dst.assign((size_t)(*dw) * (*dh), 0);
  for (int y = 0; y < sh; ++y) {
    const uint32_t* srow = src + (size_t)y * sw;
    for (int x = 0; x < sw; ++x) {
      int ox, oy;
      switch (rot) {
        case 90:  ox = sh - 1 - y; oy = x; break;
        case 180: ox = sw - 1 - x; oy = sh - 1 - y; break;
        case 270: ox = y; oy = sw - 1 - x; break;
        default:  ox = x; oy = y; break;
      }
      dst[(size_t)oy * (*dw) + ox] = srow[x];
    }
  }
}

// Ensure v.rot_surface holds @rp rotated by v.rotation; returns the surface to
// display (the original when rotation == 0). Cached by (page, rotation, scale).
const lvg_surface_t* rotated_surface(Viewer& v, const pdfview::RenderedPage* rp) {
  if (v.rotation == 0 || !rp || !rp->valid()) return rp ? &rp->surface : nullptr;
  if (v.rot_src != rp->argb.data() || v.rot_rotation != v.rotation) {
    int dw = 0, dh = 0;
    rotate_argb(rp->argb.data(), rp->width, rp->height, v.rotation, v.rot_argb,
                &dw, &dh);
    v.rot_surface = lvg_surface_wrap(v.rot_argb.data(), dw, dh, dw);
    v.rot_rotation = v.rotation;
    v.rot_src = rp->argb.data();
  }
  return &v.rot_surface;
}

float fit_width_zoom(Viewer& v, int content_w) {
  double wpt = 0, hpt = 0;
  v.doc.page_size_points(v.page, &wpt, &hpt);
  if (v.rotation == 90 || v.rotation == 270) std::swap(wpt, hpt);  // displayed w
  if (wpt <= 0) return v.zoom;
  return clampf((float)((content_w - 2 * kPageMargin) / wpt), kMinZoom, kMaxZoom);
}

float fit_page_zoom(Viewer& v, int content_w, int content_h) {
  double wpt = 0, hpt = 0;
  v.doc.page_size_points(v.page, &wpt, &hpt);
  if (v.rotation == 90 || v.rotation == 270) std::swap(wpt, hpt);
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
  if (bm.empty()) {
    if (v.font) {
      const char* msg = "No bookmarks in this document";
      lui_canvas_draw_text(c, 12, panel_y + 10, msg, (int)std::strlen(msg),
                           v.font, kTextDim);
    }
    return;
  }
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

  // Tab bar: Outline | Pages. Both tabs are always clickable; the Outline panel
  // shows an empty-state when the document has no bookmarks.
  const int half = sb_w / 2;
  draw_tab(v, c, 0, half, y0, "Outline", v.sidebar_mode == 0, true);
  draw_tab(v, c, half, sb_w - half, y0, "Pages", v.sidebar_mode == 1, true);

  const int panel_y = sidebar_panel_y();
  lvg_rect_t pclip = lvg_rect_make(0, panel_y, sb_w, std::max(0, H - panel_y));
  lvg_canvas_set_clip(c, &pclip);
  if (v.sidebar_mode == 0)
    draw_outline_panel(v, c, panel_y, H);
  else
    draw_thumb_panel(v, c, panel_y, H);
  lvg_canvas_reset_clip(c);

  lvg_canvas_fill_rect(c, 0, panel_y - 1, sb_w, 1, kToolbarLine);  // tab sep
  lvg_canvas_fill_rect(c, sb_w - 1, y0, 1, h, kToolbarLine);       // divider
}

// ---- Right panel: document info / signatures / forms, and revisions --------

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
  // Populate catalog.signature_fields (non-const; mirrors validate_signature).
  const_cast<nanopdf::Pdf*>(pdf)->parse_signature_fields();
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
      // Cryptographic verification via OpenSSL (signature math + ByteRange +
      // embedded RFC 3161 timestamp), not just digest integrity.
      pdfview::VerifyResult vr = pdfview::verify_signature(
          v.doc.data(), s.byte_range, s.signature_contents);
      L.push_back({std::string("  signature: ") +
                       (!vr.checked ? "unverifiable"
                                    : vr.signature_valid ? "VALID" : "INVALID"),
                   false});
      if (vr.covers_document)
        L.push_back({"  covers: whole document", false});
      if (!vr.signer.empty())
        L.push_back({"  signer: " + vr.signer, false});
      if (!vr.digest_algorithm.empty())
        L.push_back({"  digest: " + vr.digest_algorithm, false});
      if (!vr.signing_time.empty())
        L.push_back({"  signed: " + vr.signing_time, false});
      if (vr.has_timestamp) {
        L.push_back({"  timestamp: " + vr.timestamp_time, false});
        if (!vr.timestamp_authority.empty())
          L.push_back({"  TSA: " + vr.timestamp_authority, false});
      }
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

void draw_debug_panel(Viewer& v, lvg_canvas_t* c, int x0, int H) {
  const int w = kRightW;
  const int y0 = kToolbarH;
  const int lh = v.font ? lui_font_line_height(v.font) : 14;
  if (!v.font) return;
  int y = y0 + 8 - v.info_scroll;
  // Header.
  const char* hdr = v.debug_hits.empty()
                        ? "Debug: drag a region on the page"
                        : "Debug: region hits (click to dump)";
  lui_canvas_draw_text(c, x0 + 12, y, hdr, (int)std::strlen(hdr), v.font,
                       kAccent);
  y += lh + 8;
  // Hit rows.
  for (size_t i = 0; i < v.debug_hits.size(); ++i) {
    int ry = y + (int)i * kRowH;
    if (ry + kRowH > y0 && ry < H) {
      if ((int)i == v.debug_sel)
        lvg_canvas_fill_rect(c, x0, ry - 4, w, kRowH, kSidebarSel);
      std::string t = fit_text(v.font, v.debug_hits[i].label, w - 24);
      lui_canvas_draw_text(c, x0 + 12, ry, t.c_str(), (int)t.size(), v.font,
                           (int)i == v.debug_sel ? kTextColor : kSidebarText);
    }
  }
  y += (int)v.debug_hits.size() * kRowH + 10;
  // Object dump for the selected hit.
  if (!v.debug_dump.empty()) {
    lvg_canvas_fill_rect(c, x0 + 8, y - 5, w - 16, 1, kToolbarLine);
    size_t start = 0;
    while (start <= v.debug_dump.size()) {
      size_t nl = v.debug_dump.find('\n', start);
      std::string line = v.debug_dump.substr(
          start, nl == std::string::npos ? std::string::npos : nl - start);
      if (y + lh > y0 && y < H) {
        std::string t = fit_text(v.font, line, w - 20);
        lui_canvas_draw_text(c, x0 + 10, y, t.c_str(), (int)t.size(), v.font,
                             kTextDim);
      }
      y += lh + 1;
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
  }
  // Clamp scroll to content height.
  int total = (y - (y0 + 8 - v.info_scroll));
  v.info_scroll = clampi(v.info_scroll, 0, std::max(0, total - (H - y0)));
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
  else if (v.right_panel == 3)
    draw_debug_panel(v, c, x0, H);
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
void pdf_to_screen(Viewer& v, double gx, double gy, int* sx, int* sy);

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
      const lvg_surface_t* disp = rotated_surface(v, show);
      const int pw = disp->width;   // displayed (post-rotation) dimensions
      const int ph = disp->height;

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
      const int shadow = ui_px(4);  // drop-shadow offset
      lvg_canvas_fill_rect(&c, px + shadow, py + shadow, pw, ph, kPageShadow);
      lvg_canvas_draw_image(&c, px, py, pw, ph, disp, nullptr,
                            LVG_IMAGE_FILTER_BILINEAR);

      if (show == rp) {  // overlays apply to the normal (after) view only
        // Markup annotations created this session.
        for (const auto& m : v.markups) {
          if (m.page != v.page) continue;
          for (const auto& q : m.quads) {
            int sx, sy, sw, sh;
            map_quad_to_screen(v, q, &sx, &sy, &sw, &sh);
            const int t = std::max(1, ui_px(2));
            switch (m.type) {
              case nanopdf::MarkupType::Underline:
                lvg_canvas_fill_rect(&c, sx, sy + sh - t, sw, t,
                                     LVG_COLOR_RGB(0xD8, 0x20, 0x20));
                break;
              case nanopdf::MarkupType::StrikeOut:
                lvg_canvas_fill_rect(&c, sx, sy + sh / 2, sw, t,
                                     LVG_COLOR_RGB(0xD8, 0x20, 0x20));
                break;
              default:  // Highlight / Squiggly
                lvg_canvas_fill_rect(&c, sx, sy, sw, sh,
                                     LVG_COLOR_ARGB(0x66, 0xFF, 0xE0, 0x00));
                break;
            }
          }
        }
        // Sticky-note markers.
        for (size_t i = 0; i < v.notes.size(); ++i) {
          if (v.notes[i].page != v.page) continue;
          int nx, ny;
          pdf_to_screen(v, v.notes[i].x, v.notes[i].y, &nx, &ny);
          const int sz = ui_px(16);
          const bool editing = v.note_typing && i == v.notes.size() - 1;
          lvg_canvas_fill_rect(&c, nx, ny - sz, sz, sz,
                               LVG_COLOR_RGB(0xFF, 0xC8, 0x40));
          lvg_canvas_fill_rect(&c, nx, ny - sz, sz, ui_px(2),
                               LVG_COLOR_RGB(0xB0, 0x80, 0x00));
          if (editing && v.font) {  // show the in-progress note text
            std::string t = v.notes[i].text + "_";
            lui_canvas_draw_text(&c, nx + sz + ui_px(4), ny - sz, t.c_str(),
                                 (int)t.size(), v.font, kAccent);
          }
        }
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
        // Debug region-inspect highlights.
        if (v.debug_page == v.page) {
          for (size_t i = 0; i < v.debug_hits.size(); ++i) {
            int sx, sy, sw, sh;
            map_quad_to_screen(v, v.debug_hits[i].rect, &sx, &sy, &sw, &sh);
            lvg_canvas_fill_rect(&c, sx, sy, sw, sh,
                                 (int)i == v.debug_sel ? kDbgSel : kDbgHi);
          }
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
    if (v.goto_active) {
      char gb[96];
      std::snprintf(gb, sizeof(gb), "Go to page: %s_   (1-%d)",
                    v.goto_buf.c_str(), v.doc.page_count());
      lui_canvas_draw_text(&c, 14, ty, gb, (int)std::strlen(gb), v.font, kAccent);
    } else if (!v.toast.empty()) {
      lui_canvas_draw_text(&c, 14, ty, v.toast.c_str(), (int)v.toast.size(),
                           v.font, kAccent);
    } else if (v.search_active || !v.query.empty()) {
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
    // Centered "Open file" button + drag-and-drop hint.
    const int lh = lui_font_line_height(v.font);
    const char* label = "Open PDF…";
    const int lw = lui_font_measure_text(v.font, label, (int)std::strlen(label));
    const int padx = ui_px(20), pady = ui_px(10);
    const int bw = lw + 2 * padx, bh = lh + 2 * pady;
    const int bx = content_x + (std::max(0, W - content_x) - bw) / 2;
    const int by = content_y + std::max(0, H - content_y) / 2 - bh;
    v.open_btn_x = bx;
    v.open_btn_y = by;
    v.open_btn_w = bw;
    v.open_btn_h = bh;
    const bool hover = v.mouse_x >= bx && v.mouse_x < bx + bw &&
                       v.mouse_y >= by && v.mouse_y < by + bh;
    lvg_canvas_fill_rect(&c, bx, by, bw, bh, hover ? kAccent : kSidebarSel);
    lui_canvas_draw_text(&c, bx + padx, by + pady, label, (int)std::strlen(label),
                         v.font, kTextColor);
    const char* hint = "or drag a PDF here  ·  press 'o' to browse";
    const int hw = lui_font_measure_text(v.font, hint, (int)std::strlen(hint));
    lui_canvas_draw_text(&c, content_x + (std::max(0, W - content_x) - hw) / 2,
                         by + bh + ui_px(16), hint, (int)std::strlen(hint),
                         v.font, kTextDim);
  } else {
    v.open_btn_w = v.open_btn_h = 0;  // not shown
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

// Map a PDF user-space point (y-up) to a screen pixel, honoring v.rotation.
void pdf_to_screen(Viewer& v, double gx, double gy, int* sx, int* sy) {
  double left, bottom, right, top;
  v.doc.page_box(v.page, &left, &bottom, &right, &top);
  float s = (v.page_scale > 0) ? v.page_scale : 1.0f;
  double ix = (gx - left) * s, iy = (top - gy) * s;  // unrotated image px
  double pw0 = (right - left) * s, ph0 = (top - bottom) * s;
  double dx, dy;
  switch (v.rotation) {
    case 90:  dx = ph0 - iy; dy = ix; break;
    case 180: dx = pw0 - ix; dy = ph0 - iy; break;
    case 270: dx = iy; dy = pw0 - ix; break;
    default:  dx = ix; dy = iy; break;
  }
  *sx = v.page_px + (int)dx;
  *sy = v.page_py + (int)dy;
}

// Map a text quad (PDF user space, y-up) to a screen rect (rotation-aware:
// the quad's corners are mapped and bounded).
void map_quad_to_screen(Viewer& v, const nanopdf::TextQuad& q, int* sx, int* sy,
                        int* sw, int* sh) {
  int cx[4], cy[4];
  pdf_to_screen(v, q.x, q.y, &cx[0], &cy[0]);
  pdf_to_screen(v, q.x + q.width, q.y, &cx[1], &cy[1]);
  pdf_to_screen(v, q.x, q.y + q.height, &cx[2], &cy[2]);
  pdf_to_screen(v, q.x + q.width, q.y + q.height, &cx[3], &cy[3]);
  int x0 = cx[0], y0 = cy[0], x1 = cx[0], y1 = cy[0];
  for (int i = 1; i < 4; ++i) {
    x0 = std::min(x0, cx[i]); y0 = std::min(y0, cy[i]);
    x1 = std::max(x1, cx[i]); y1 = std::max(y1, cy[i]);
  }
  *sx = x0;
  *sy = y0;
  *sw = std::max(1, x1 - x0);
  *sh = std::max(1, y1 - y0);
}

// Inverse map: a screen point to PDF user-space coords (rotation-aware).
void screen_to_page(Viewer& v, int sx, int sy, double* gx, double* gy) {
  double left, bottom, right, top;
  v.doc.page_box(v.page, &left, &bottom, &right, &top);
  float s = (v.page_scale > 0) ? v.page_scale : 1.0f;
  double dx = sx - v.page_px, dy = sy - v.page_py;
  double pw0 = (right - left) * s, ph0 = (top - bottom) * s;
  double ix, iy;
  switch (v.rotation) {
    case 90:  iy = ph0 - dx; ix = dy; break;
    case 180: ix = pw0 - dx; iy = ph0 - dy; break;
    case 270: iy = dx; ix = pw0 - dy; break;  // dx=iy, dy=pw0-ix
    default:  ix = dx; iy = dy; break;
  }
  *gx = left + ix / s;
  *gy = top - iy / s;
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

// Save a screenshot of the current frame @surf to an auto-named PNG in the
// working directory (e.g. "report.pdf_p03_1.png"). Updates v.status.
void take_screenshot(Viewer& v, const lvg_surface_t* surf) {
  if (!surf) return;
  std::string base = v.title.empty() ? "pdfview" : v.title;
  char name[320];
  std::snprintf(name, sizeof(name), "%s_p%02d_%d.png", base.c_str(),
                v.page + 1, ++v.screenshot_seq);
  if (save_surface_image(surf, name)) {
    v.toast = std::string("Saved screenshot: ") + name;
    std::printf("pdfview: saved screenshot %s (%dx%d)\n", name, surf->width,
                surf->height);
  } else {
    v.toast = std::string("Screenshot failed: ") + name;
    std::fprintf(stderr, "pdfview: screenshot failed: %s\n", name);
  }
}

// Write the document plus this session's markup annotations to a sibling
// "<name>.annotated.pdf" via a nanopdf incremental update. Sets v.toast.
void save_markups(Viewer& v) {
  if (v.markups.empty() && v.notes.empty()) {
    v.toast = "No annotations to save";
    return;
  }
  nanopdf::PdfWriter w;
  std::string err;
  if (!w.load_existing(v.doc.data(), &err)) {
    v.toast = "Save failed (load): " + err;
    return;
  }
  for (const auto& n : v.notes) {
    // Note anchor is the top-left; give it a small icon-sized rect.
    w.add_text_annotation_to_existing_page(n.page, n.x, n.y - 18, 18, 18,
                                           n.text);
  }
  for (const auto& m : v.markups) {
    std::vector<nanopdf::QuadPoints> quads;
    for (const auto& q : m.quads)
      quads.push_back(nanopdf::quad_from_rect(q.x, q.y, q.width, q.height));
    if (m.type == nanopdf::MarkupType::Highlight) {
      nanopdf::HighlightConfig cfg;
      cfg.page = m.page;
      cfg.quads = quads;
      cfg.color_preset = nanopdf::HighlightColor::Yellow;
      cfg.subject = "Highlight";
      w.add_highlight_to_existing_page(m.page, cfg);
    } else {
      nanopdf::TextMarkupConfig cfg;
      cfg.page = m.page;
      cfg.quads = quads;
      cfg.type = m.type;
      cfg.r = 0.85;
      cfg.g = 0.1;
      cfg.b = 0.1;  // red, like Preview underline/strikethrough
      w.add_text_markup_to_existing_page(m.page, cfg);
    }
  }
  std::string out = v.doc_path;
  size_t dot = out.find_last_of('.');
  out = (dot == std::string::npos ? out : out.substr(0, dot)) + ".annotated.pdf";
  nanopdf::WriteResult wr = w.write_incremental_to_file(out);
  if (wr.success) {
    const char* slash = std::strrchr(out.c_str(), '/');
    v.toast = std::string("Saved ") +
              std::to_string(v.markups.size() + v.notes.size()) +
              " annotation(s) -> " + (slash ? slash + 1 : out.c_str());
    std::printf("pdfview: wrote %s\n", out.c_str());
  } else {
    v.toast = "Save failed: " + wr.error;
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
  v.doc_path = path;
  v.markups.clear();
  v.notes.clear();
  v.note_placing = false;
  v.note_typing = false;
  v.page = 0;
  v.scroll_x = 0;
  v.scroll_y = 0;
  v.fit_width = true;
  v.rotation = 0;
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

#if PDFVIEW_HAVE_MCP
// ---- MCP server: let an LLM/agent drive + inspect the live viewer ----------
// Tool handlers run on the main thread during lui_mcp_poll(), so they mutate
// the Viewer directly and set *dirty to request a redraw.
struct McpCtx {
  Viewer* v = nullptr;
  bool* dirty = nullptr;
  int content_h = 700;  // approximate; draw() re-clamps scroll
};

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += (char)c;
        }
    }
  }
  return o;
}

char* mcp_text(const std::string& text) {
  std::string r = "[{\"type\":\"text\",\"text\":\"" + json_escape(text) + "\"}]";
  char* out = (char*)std::malloc(r.size() + 1);
  if (out) std::memcpy(out, r.c_str(), r.size() + 1);
  return out;
}

std::string hits_to_text(const std::vector<pdfview::RegionHit>& hits) {
  std::string t = std::to_string(hits.size()) + " hit(s):\n";
  char b[256];
  for (const auto& h : hits) {
    const char* k = h.kind == pdfview::RegionHit::Text    ? "text"
                    : h.kind == pdfview::RegionHit::Image  ? "image"
                                                           : "annot";
    std::snprintf(b, sizeof(b), "  [%s] %s  rect=%.0f,%.0f %.0fx%.0f obj=%d\n", k,
                  h.label.c_str(), h.rect.x, h.rect.y, h.rect.width,
                  h.rect.height, h.obj_num);
    t += b;
  }
  return t;
}

void tool_viewer_state(const char*, int, void* u, char** out, int* olen) {
  McpCtx* m = (McpCtx*)u;
  Viewer& v = *m->v;
  char buf[512];
  std::snprintf(
      buf, sizeof(buf),
      "doc: %s\npage: %d / %d\nzoom: %d%%\nsidebar: %s\npanel: %s\ndebug_mode: %s",
      v.title.empty() ? "(none)" : v.title.c_str(),
      v.doc.loaded() ? v.page + 1 : 0, v.doc.page_count(),
      (int)std::lround(v.zoom * 100.0f / (96.0f / 72.0f)),
      v.show_sidebar ? (v.sidebar_mode == 0 ? "outline" : "pages") : "hidden",
      v.right_panel == 1 ? "info" : v.right_panel == 2 ? "revisions"
                           : v.right_panel == 3 ? "debug" : "none",
      v.debug_mode ? "on" : "off");
  *out = mcp_text(buf);
  *olen = -1;
}

void tool_goto_page(const char* a, int al, void* u, char** out, int* olen) {
  McpCtx* m = (McpCtx*)u;
  lui_json_t* j = lui_json_parse(a, al);
  int page = lui_json_int(lui_json_get(j, "page"));
  lui_json_free(j);
  go_to_page(*m->v, page - 1);
  *m->dirty = true;
  *out = mcp_text("now on page " + std::to_string(m->v->page + 1));
  *olen = -1;
}

void tool_set_zoom(const char* a, int al, void* u, char** out, int* olen) {
  McpCtx* m = (McpCtx*)u;
  Viewer& v = *m->v;
  lui_json_t* j = lui_json_parse(a, al);
  const char* fit = lui_json_string(lui_json_get(j, "fit"));
  lui_json_t* z = lui_json_get(j, "zoom");
  if (fit && std::strcmp(fit, "width") == 0) {
    v.fit_width = true;
  } else if (fit && std::strcmp(fit, "page") == 0) {
    v.fit_width = false;
    v.zoom = fit_page_zoom(v, 760, m->content_h);
  } else if (fit && std::strcmp(fit, "100") == 0) {
    v.fit_width = false;
    v.zoom = 96.0f / 72.0f;
  } else if (z) {
    v.fit_width = false;
    v.zoom = clampf((float)(lui_json_number(z) / 100.0 * (96.0 / 72.0)), kMinZoom,
                    kMaxZoom);
  }
  lui_json_free(j);
  *m->dirty = true;
  *out = mcp_text("zoom set");
  *olen = -1;
}

void tool_open(const char* a, int al, void* u, char** out, int* olen) {
  McpCtx* m = (McpCtx*)u;
  lui_json_t* j = lui_json_parse(a, al);
  const char* path = lui_json_string(lui_json_get(j, "path"));
  std::string res;
  if (path && load_document(*m->v, nullptr, path)) {
    res = "opened " + std::string(path) + " (" +
          std::to_string(m->v->doc.page_count()) + " pages)";
    *m->dirty = true;
  } else {
    res = "failed to open: " + std::string(path ? path : "(no path)");
  }
  lui_json_free(j);
  *out = mcp_text(res);
  *olen = -1;
}

void tool_search(const char* a, int al, void* u, char** out, int* olen) {
  McpCtx* m = (McpCtx*)u;
  Viewer& v = *m->v;
  lui_json_t* j = lui_json_parse(a, al);
  const char* term = lui_json_string(lui_json_get(j, "term"));
  v.query = term ? term : "";
  lui_json_free(j);
  run_search(v, 0, +1, m->content_h);
  *m->dirty = true;
  std::string res =
      v.match_page >= 0
          ? std::to_string(v.match_quads.size()) + " match(es); jumped to page " +
                std::to_string(v.match_page + 1)
          : "no matches for \"" + v.query + "\"";
  *out = mcp_text(res);
  *olen = -1;
}

void tool_inspect_region(const char* a, int al, void* u, char** out, int* olen) {
  McpCtx* m = (McpCtx*)u;
  Viewer& v = *m->v;
  lui_json_t* j = lui_json_parse(a, al);
  lui_json_t* pj = lui_json_get(j, "page");
  int page = pj ? lui_json_int(pj) - 1 : v.page;
  double x1 = lui_json_number(lui_json_get(j, "x1"));
  double y1 = lui_json_number(lui_json_get(j, "y1"));
  double x2 = lui_json_number(lui_json_get(j, "x2"));
  double y2 = lui_json_number(lui_json_get(j, "y2"));
  lui_json_free(j);
  go_to_page(v, page);
  v.debug_hits = pdfview::inspect_region(v.doc, page, x1, y1, x2, y2);
  v.debug_page = page;
  v.debug_sel = -1;
  v.debug_dump.clear();
  v.debug_mode = true;
  v.right_panel = 3;
  *m->dirty = true;
  *out = mcp_text(hits_to_text(v.debug_hits));
  *olen = -1;
}

void tool_dump_object(const char* a, int al, void* u, char** out, int* olen) {
  McpCtx* m = (McpCtx*)u;
  lui_json_t* j = lui_json_parse(a, al);
  int obj = lui_json_int(lui_json_get(j, "obj"));
  lui_json_t* gj = lui_json_get(j, "gen");
  int gen = gj ? lui_json_int(gj) : 0;
  lui_json_free(j);
  *out = mcp_text(
      pdfview::dump_object(m->v->doc, (uint32_t)obj, (uint16_t)gen));
  *olen = -1;
}

void tool_highlight_object(const char* a, int al, void* u, char** out, int* olen) {
  McpCtx* m = (McpCtx*)u;
  Viewer& v = *m->v;
  lui_json_t* j = lui_json_parse(a, al);
  lui_json_t* pj = lui_json_get(j, "page");
  int page = pj ? lui_json_int(pj) - 1 : v.page;
  int obj = lui_json_int(lui_json_get(j, "obj"));
  lui_json_free(j);
  go_to_page(v, page);
  v.debug_hits = pdfview::find_object_placements(v.doc, page, (uint32_t)obj);
  v.debug_page = page;
  v.debug_sel = -1;
  v.debug_mode = true;
  v.right_panel = 3;
  *m->dirty = true;
  *out = mcp_text("object " + std::to_string(obj) + ": " +
                  hits_to_text(v.debug_hits));
  *olen = -1;
}

void tool_clear_highlight(const char*, int, void* u, char** out, int* olen) {
  McpCtx* m = (McpCtx*)u;
  m->v->debug_hits.clear();
  m->v->debug_page = -1;
  m->v->debug_sel = -1;
  m->v->debug_dump.clear();
  m->v->match_quads.clear();
  m->v->match_page = -1;
  *m->dirty = true;
  *out = mcp_text("cleared");
  *olen = -1;
}

void tool_sign(const char* a, int al, void* /*u*/, char** out, int* olen) {
  lui_json_t* j = lui_json_parse(a, al);
  auto str = [&](const char* k) -> std::string {
    const char* s = lui_json_string(lui_json_get(j, k));
    return s ? s : "";
  };
  pdfview::SignOptions o;
  std::string in = str("in"), outp = str("out");
  o.p12_path = str("p12");
  o.p12_password = str("pass");
  o.cert_pem_path = str("cert");
  o.key_pem_path = str("key");
  o.key_password = str("keypass");
  if (!str("reason").empty()) o.reason = str("reason");
  if (!str("location").empty()) o.location = str("location");
  if (!str("field").empty()) o.field_name = str("field");
  std::string tsa = str("tsa");
  if (!tsa.empty()) {
    std::string preset = pdfview::tsa_url_for(tsa);
    o.tsa_url = preset.empty() ? tsa : preset;
  }
  o.tsa_username = str("tsa_user");
  o.tsa_password = str("tsa_pass");
  lui_json_free(j);
  if (in.empty() || outp.empty()) {
    *out = mcp_text("error: 'in' and 'out' paths are required");
    *olen = -1;
    return;
  }
  pdfview::SignResult r = pdfview::sign_pdf_file(in, outp, o);
  std::string msg;
  if (r.ok) {
    msg = "signed by '" + r.signer_name + "' -> " + outp;
    if (r.timestamped) msg += "  (RFC 3161 timestamped via " +
                              r.timestamp_authority + ")";
  } else {
    msg = "sign failed: " + r.error;
  }
  *out = mcp_text(msg);
  *olen = -1;
}

lui_mcp_t* mcp_start(Viewer& v, bool* dirty, int port, lvg_surface_t* surface) {
  static McpCtx ctx;
  ctx.v = &v;
  ctx.dirty = dirty;
  lui_mcp_config_t cfg;
  cfg.transport = LUI_MCP_HTTP;
  cfg.host = "127.0.0.1";
  cfg.port = port;
  lui_mcp_t* mcp = lui_mcp_create(&cfg);
  if (!mcp) return nullptr;
  lui_mcp_set_context(mcp, nullptr, (struct lui_surface*)surface);
  auto reg = [&](const char* n, const char* d, const char* s,
                 lui_mcp_tool_handler_fn h) {
    lui_mcp_register_tool(mcp, n, d, s, h, &ctx);
  };
  const char* obj_only = "{\"type\":\"object\",\"properties\":{}}";
  reg("viewer_state", "Current viewer state (doc, page, zoom, panels).",
      obj_only, tool_viewer_state);
  reg("goto_page", "Navigate to a 1-based page number.",
      "{\"type\":\"object\",\"properties\":{\"page\":{\"type\":\"integer\"}},"
      "\"required\":[\"page\"]}",
      tool_goto_page);
  reg("set_zoom",
      "Set zoom: {zoom:percent} or {fit:\"width\"|\"page\"|\"100\"}.",
      "{\"type\":\"object\",\"properties\":{\"zoom\":{\"type\":\"number\"},"
      "\"fit\":{\"type\":\"string\"}}}",
      tool_set_zoom);
  reg("open", "Open a PDF by file path.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
      "\"required\":[\"path\"]}",
      tool_open);
  reg("search", "Search the document; jumps to + highlights the first match.",
      "{\"type\":\"object\",\"properties\":{\"term\":{\"type\":\"string\"}},"
      "\"required\":[\"term\"]}",
      tool_search);
  // Note: lightui registers built-in canvas tools (incl. "inspect_region"), so
  // the PDF debug tools are prefixed "pdf_" to avoid name collisions.
  reg("pdf_inspect_region",
      "Report PDF objects (text+font+obj, images, annotations) in a page-space "
      "rect and highlight them in the viewer.",
      "{\"type\":\"object\",\"properties\":{\"page\":{\"type\":\"integer\"},"
      "\"x1\":{\"type\":\"number\"},\"y1\":{\"type\":\"number\"},"
      "\"x2\":{\"type\":\"number\"},\"y2\":{\"type\":\"number\"}},"
      "\"required\":[\"x1\",\"y1\",\"x2\",\"y2\"]}",
      tool_inspect_region);
  reg("pdf_dump_object", "Pretty-print an arbitrary PDF object by number.",
      "{\"type\":\"object\",\"properties\":{\"obj\":{\"type\":\"integer\"},"
      "\"gen\":{\"type\":\"integer\"}},\"required\":[\"obj\"]}",
      tool_dump_object);
  reg("pdf_highlight_object",
      "Highlight where a PDF object (image XObject / font) is drawn on a page.",
      "{\"type\":\"object\",\"properties\":{\"page\":{\"type\":\"integer\"},"
      "\"obj\":{\"type\":\"integer\"}},\"required\":[\"obj\"]}",
      tool_highlight_object);
  reg("pdf_clear_highlight", "Clear all debug/search highlights.", obj_only,
      tool_clear_highlight);
  reg("pdf_sign",
      "Digitally sign a PDF (PKCS#7/CMS detached) with an optional RFC 3161 "
      "timestamp. Credentials: a PKCS#12 ({p12,pass}) or PEM ({cert,key,"
      "keypass}). TSA presets for 'tsa': digicert, globalsign, sectigo, "
      "freetsa (or a full URL).",
      "{\"type\":\"object\",\"properties\":{\"in\":{\"type\":\"string\"},"
      "\"out\":{\"type\":\"string\"},\"p12\":{\"type\":\"string\"},"
      "\"pass\":{\"type\":\"string\"},\"cert\":{\"type\":\"string\"},"
      "\"key\":{\"type\":\"string\"},\"keypass\":{\"type\":\"string\"},"
      "\"reason\":{\"type\":\"string\"},\"location\":{\"type\":\"string\"},"
      "\"field\":{\"type\":\"string\"},\"tsa\":{\"type\":\"string\"},"
      "\"tsa_user\":{\"type\":\"string\"},\"tsa_pass\":{\"type\":\"string\"}},"
      "\"required\":[\"in\",\"out\"]}",
      tool_sign);
  return mcp;
}
#endif  // PDFVIEW_HAVE_MCP

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: progress visible live
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

  // Headless page screenshot to PNG/JPG (format chosen by output extension).
  //   pdfview --screenshot <pdf> <out.png|jpg> [page1based] [scale]
  if (argc > 1 && std::strcmp(argv[1], "--screenshot") == 0) {
    if (argc < 4) {
      std::fprintf(stderr,
                   "usage: pdfview --screenshot <pdf> <out.png|jpg> "
                   "[page] [scale]\n");
      return 2;
    }
    int page1 = (argc > 4) ? std::atoi(argv[4]) : 1;
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
    if (!save_surface_image(&rp->surface, argv[3])) {
      std::fprintf(stderr, "write failed: %s\n", argv[3]);
      return 1;
    }
    std::fprintf(stderr, "screenshot page %d at scale %.2f -> %dx%d -> %s\n",
                 page1, scale, rp->width, rp->height, argv[3]);
    return 0;
  }

  // Headless digital signing.
  //   pdfview --sign <in.pdf> <out.pdf> (--p12 F --pass P | --cert C --key K
  //           [--keypass P]) [--reason R] [--location L] [--field N] [--visible]
  //           [--page N] [--tsa <preset|url>] [--tsa-user U] [--tsa-pass P]
  // TSA presets: digicert, globalsign, sectigo, freetsa, opentimestamps.
  if (argc > 1 && std::strcmp(argv[1], "--sign") == 0) {
    if (argc < 4) {
      std::fprintf(stderr,
                   "usage: pdfview --sign <in.pdf> <out.pdf> "
                   "(--p12 F --pass P | --cert C --key K) [--reason R] "
                   "[--location L] [--field N] [--visible] [--page N] "
                   "[--tsa <preset|url>] [--tsa-user U] [--tsa-pass P]\n"
                   "TSA presets: digicert globalsign sectigo freetsa "
                   "opentimestamps\n");
      return 2;
    }
    pdfview::SignOptions o;
    auto val = [&](int& i) -> const char* {
      return (i + 1 < argc) ? argv[++i] : "";
    };
    for (int i = 4; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--p12") o.p12_path = val(i);
      else if (a == "--pass") o.p12_password = val(i);
      else if (a == "--cert") o.cert_pem_path = val(i);
      else if (a == "--key") o.key_pem_path = val(i);
      else if (a == "--keypass") o.key_password = val(i);
      else if (a == "--reason") o.reason = val(i);
      else if (a == "--location") o.location = val(i);
      else if (a == "--field") o.field_name = val(i);
      else if (a == "--visible") o.visible = true;
      else if (a == "--page") o.page = std::atoi(val(i));
      else if (a == "--tsa") {
        std::string t = val(i);
        std::string preset = pdfview::tsa_url_for(t);
        o.tsa_url = preset.empty() ? t : preset;
      } else if (a == "--tsa-user") o.tsa_username = val(i);
      else if (a == "--tsa-pass") o.tsa_password = val(i);
    }
    pdfview::SignResult r = pdfview::sign_pdf_file(argv[2], argv[3], o);
    if (!r.ok) {
      std::fprintf(stderr, "sign failed: %s\n", r.error.c_str());
      return 1;
    }
    std::fprintf(stderr, "signed by '%s' -> %s%s\n", r.signer_name.c_str(),
                 argv[3],
                 r.timestamped ? "  (RFC 3161 timestamped)" : "");
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

  // Headless region inspector (debug data layer).
  //   pdfview --inspect <pdf> <page1based> <x1> <y1> <x2> <y2>   (page-space pts)
  if (argc > 1 && std::strcmp(argv[1], "--inspect") == 0) {
    if (argc < 8) {
      std::fprintf(stderr,
                   "usage: pdfview --inspect <pdf> <page> <x1> <y1> <x2> <y2>\n");
      return 2;
    }
    pdfview::PdfDocument doc;
    if (!doc.load_file(argv[2])) {
      std::fprintf(stderr, "load failed: %s\n", doc.error().c_str());
      return 1;
    }
    int page = std::atoi(argv[3]) - 1;
    double x1 = std::atof(argv[4]), y1 = std::atof(argv[5]);
    double x2 = std::atof(argv[6]), y2 = std::atof(argv[7]);
    std::vector<pdfview::RegionHit> hits =
        pdfview::inspect_region(doc, page, x1, y1, x2, y2);
    std::printf("region (%.0f,%.0f)-(%.0f,%.0f) on page %d: %zu hit(s)\n", x1, y1,
                x2, y2, page + 1, hits.size());
    for (const auto& h : hits) {
      const char* k = h.kind == pdfview::RegionHit::Text      ? "TEXT"
                      : h.kind == pdfview::RegionHit::Image    ? "IMG "
                                                               : "ANNO";
      std::printf("  [%s] %s  rect=%.0f,%.0f %.0fx%.0f%s%s\n", k,
                  h.label.c_str(), h.rect.x, h.rect.y, h.rect.width,
                  h.rect.height, h.detail.empty() ? "" : "  | ",
                  h.detail.c_str());
    }
    // Dump the first object-bearing hit.
    for (const auto& h : hits) {
      if (h.obj_num >= 0) {
        std::printf("\n--- dump object %d ---\n%s\n", h.obj_num,
                    pdfview::dump_object(doc, (uint32_t)h.obj_num,
                                         (uint16_t)h.gen_num)
                        .c_str());
        break;
      }
    }
    return 0;
  }

  // Headless debug-overlay check: inspect a region and compose the frame with
  // debug highlights + panel (first hit selected/dumped).
  //   pdfview --debugview <pdf> <page> <x1> <y1> <x2> <y2> <out.ppm>
  if (argc > 1 && std::strcmp(argv[1], "--debugview") == 0) {
    if (argc < 9) {
      std::fprintf(stderr,
                   "usage: pdfview --debugview <pdf> <page> <x1> <y1> <x2> <y2> "
                   "<out.ppm>\n");
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
    v.page = std::atoi(argv[3]) - 1;
    v.sidebar_mode = v.doc.outline().empty() ? 1 : 0;
    v.debug_mode = true;
    v.right_panel = 3;
    draw(v, s);  // establish placement
    v.debug_hits = pdfview::inspect_region(v.doc, v.page, std::atof(argv[4]),
                                           std::atof(argv[5]), std::atof(argv[6]),
                                           std::atof(argv[7]));
    v.debug_page = v.page;
    if (!v.debug_hits.empty()) {
      v.debug_sel = 0;
      const auto& h = v.debug_hits[0];
      if (h.obj_num >= 0)
        v.debug_dump = pdfview::dump_object(v.doc, (uint32_t)h.obj_num,
                                            (uint16_t)h.gen_num);
    }
    draw(v, s);
    dump_surface_ppm(s, argv[8]);
    std::fprintf(stderr, "debugview: %zu hit(s) -> %s\n", v.debug_hits.size(),
                 argv[8]);
    if (v.font) lui_font_destroy(v.font);
    lvg_surface_destroy(s);
    return 0;
  }

#if PDFVIEW_HAVE_MCP
  // Headless MCP server (no window): render to an offscreen surface and serve
  // the same tools over HTTP. Useful for LLM/VLM PDF inspection without a
  // display (the screenshot tool returns the offscreen-rendered frame).
  //   pdfview --mcp-serve <pdf> [port]
  if (argc > 1 && std::strcmp(argv[1], "--mcp-serve") == 0) {
    if (argc < 3) {
      std::fprintf(stderr, "usage: pdfview --mcp-serve <pdf> [port]\n");
      return 2;
    }
    int port = (argc > 3) ? std::atoi(argv[3]) : 3001;
    lvg_surface_t* s = lvg_surface_create(kInitialWidth, kInitialHeight);
    if (!s) return 1;
    Viewer v;
    v.font = lui_font_create(PDFVIEW_FONTS_DIR "/arimo/Arimo-Regular.ttf", 16);
    if (!load_document(v, nullptr, argv[2])) {
      std::fprintf(stderr, "load failed: %s\n", v.doc.error().c_str());
      return 1;
    }
    v.sidebar_mode = v.doc.outline().empty() ? 1 : 0;
    bool dirty = true;
    lui_mcp_t* mcp = mcp_start(v, &dirty, port, s);
    if (!mcp) {
      std::fprintf(stderr, "MCP server failed to start on port %d\n", port);
      return 1;
    }
    std::fprintf(stderr,
                 "pdfview: headless MCP server on http://127.0.0.1:%d "
                 "(Ctrl-C to stop)\n",
                 port);
    for (;;) {
      if (dirty) {
        draw(v, s);
        dirty = false;
      }
      lui_mcp_poll(mcp);
      usleep(8000);
    }
  }
#endif

  const char* initial_path = nullptr;
  bool mcp_enabled = false;
  int mcp_port = 3001;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--mcp") == 0) {
      mcp_enabled = true;
      if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
        mcp_port = std::atoi(argv[++i]);
    } else if (argv[i][0] != '-' && !initial_path) {
      initial_path = argv[i];
    }
  }
#if !PDFVIEW_HAVE_MCP
  if (mcp_enabled)
    std::fprintf(stderr, "pdfview: built without MCP (--mcp ignored)\n");
#endif

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

  // HiDPI: the surface is physical-resolution (logical * dpi_scale). Scale the
  // UI chrome metrics and the font to match so the UI isn't half-size on a 2x
  // display. Tracked in cur_scale so a monitor change (resize event) can
  // re-apply it.
  float cur_scale = 1.0f;
  if (lvg_surface_t* s0 = lui_window_get_surface(win)) {
    if (s0->dpi_scale > 1.0f) cur_scale = s0->dpi_scale;
    std::fprintf(stderr, "pdfview: dpi_scale=%.3f surface=%dx%d\n",
                 s0->dpi_scale, s0->width, s0->height);
  }

#if PDFVIEW_HAVE_NFD
  NFD_Init();
#endif

  Viewer viewer;
  viewer.font = set_ui_scale(cur_scale, nullptr);
  if (!viewer.font) {
    std::fprintf(stderr, "pdfview: warning: UI font not found at %s\n",
                 kUiFontPath);
  }

  if (initial_path) load_document(viewer, win, initial_path);

  bool running = true;
  bool dirty = true;

#if PDFVIEW_HAVE_MCP
  lui_mcp_t* mcp = nullptr;
  if (mcp_enabled) {
    mcp = mcp_start(viewer, &dirty, mcp_port, lui_window_get_surface(win));
    if (mcp)
      std::printf(
          "pdfview: MCP server (Streamable HTTP) on http://127.0.0.1:%d\n",
          mcp_port);
    else
      std::fprintf(stderr, "pdfview: MCP server failed to start\n");
  }
#endif

  while (running) {
    if (dirty) {
      lvg_surface_t* surface = lui_window_get_surface(win);
      if (!surface) break;
      draw(viewer, surface);
      lui_window_present(win);
      dirty = false;
    }

    lui_event_t event;
#if PDFVIEW_HAVE_MCP
    if (mcp) {
      // Non-blocking: service MCP, then drain input; idle-sleep when quiet so
      // tool calls stay responsive without busy-spinning.
      lui_mcp_poll(mcp);
      if (!lui_window_poll_event(win, &event)) {
        usleep(6000);
        continue;
      }
    } else
#endif
        if (!lui_window_wait_event(win, &event)) {
      break;
    }
    do {
      int W = 0, H = 0;
      lui_window_get_physical_size(win, &W, &H);
      // Note: mouse events from lightui are already in physical pixels (same
      // space as the surface and our layout), so they are used as-is — no
      // dpi rescaling.
      const int content_x = sidebar_width(viewer);
      const int right_w = right_width(viewer);
      const int content_w = std::max(1, W - content_x - right_w);
      const int content_h = std::max(1, H - kToolbarH);
      const int scroll_step = std::max(40, (int)(content_h * 0.9f));

      switch (event.type) {
        case LUI_EVENT_QUIT:
          running = false;
          break;
        case LUI_EVENT_WINDOW_RESIZE: {
          // Moving to a monitor with a different scale changes dpi_scale; if so,
          // re-apply UI metrics and rebuild the font at the new size.
          float ns = event.data.resize.dpi_scale;
          if (ns >= 1.0f && std::fabs(ns - cur_scale) > 0.01f) {
            cur_scale = ns;
            viewer.font = set_ui_scale(ns, viewer.font);
            // Thumbnails are cached by render scale, which changes with kThumbW,
            // so they re-render lazily at the new size — no explicit clear.
          }
          dirty = true;
          break;
        }
        case LUI_EVENT_FILE_DROP:
          // A PDF was dragged onto the window.
          load_document(viewer, win, event.data.drop.path);
          dirty = true;
          break;
        case LUI_EVENT_WINDOW_EXPOSE:
          dirty = true;
          break;
        case LUI_EVENT_KEY_DOWN: {
          int k = event.data.key.key;
          if (!viewer.toast.empty()) {  // any key dismisses a lingering toast
            viewer.toast.clear();
            dirty = true;
          }
          if (viewer.note_typing) {
            if (k == LUI_KEY_ESCAPE) {
              if (!viewer.notes.empty() && viewer.notes.back().text.empty())
                viewer.notes.pop_back();  // discard empty note
              viewer.note_typing = false;
            } else if (k == LUI_KEY_RETURN) {
              if (!viewer.notes.empty() && viewer.notes.back().text.empty())
                viewer.notes.pop_back();
              viewer.note_typing = false;
            } else if (k == LUI_KEY_BACKSPACE && !viewer.notes.empty() &&
                       !viewer.notes.back().text.empty()) {
              std::string& t = viewer.notes.back().text;
              size_t i = t.size();
              do { --i; } while (i > 0 && ((unsigned char)t[i] & 0xC0) == 0x80);
              t.erase(i);
            }
            dirty = true;
            break;
          }
          if (viewer.goto_active) {
            if (k == LUI_KEY_ESCAPE) {
              viewer.goto_active = false;
              viewer.goto_buf.clear();
            } else if (k == LUI_KEY_RETURN) {
              if (!viewer.goto_buf.empty())
                go_to_page(viewer, std::atoi(viewer.goto_buf.c_str()) - 1);
              viewer.goto_active = false;
              viewer.goto_buf.clear();
            } else if (k == LUI_KEY_BACKSPACE && !viewer.goto_buf.empty()) {
              viewer.goto_buf.pop_back();
            }
            dirty = true;
            break;  // consume keys while the goto box is focused
          }
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
          // Ctrl+C: copy the current text selection to the system clipboard.
          if ((event.data.key.mods & LUI_MOD_CTRL) && (k == 'c' || k == 'C')) {
            if (!viewer.sel_text.empty()) {
              lui_window_set_clipboard_text(win, viewer.sel_text.c_str());
              viewer.toast = "Copied " +
                             std::to_string(utf8_char_count(viewer.sel_text)) +
                             " chars to clipboard";
              dirty = true;
            }
            break;
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
          if (!viewer.toast.empty()) {  // any key dismisses a lingering toast
            viewer.toast.clear();
            dirty = true;
          }
          if (viewer.note_typing) {
            if ((unsigned char)ch >= 0x20 && !viewer.notes.empty() &&
                viewer.notes.back().text.size() < 240) {
              viewer.notes.back().text += event.data.text.text;
              dirty = true;
            }
            break;
          }
          if (viewer.goto_active) {
            if (ch >= '0' && ch <= '9' && viewer.goto_buf.size() < 7) {
              viewer.goto_buf += ch;
              dirty = true;
            }
            break;
          }
          if (viewer.search_active) {
            if ((unsigned char)ch >= 0x20) {  // printable (incl. UTF-8 lead)
              viewer.query += event.data.text.text;
              dirty = true;
            }
            break;
          }
          if (ch == 'p' || ch == 'P') {
            viewer.goto_active = true;
            viewer.goto_buf.clear();
            dirty = true;
          } else if (ch == '/') {
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
          } else if (ch == 'g' || ch == 'G') {
            viewer.debug_mode = !viewer.debug_mode;
            viewer.right_panel = viewer.debug_mode ? 3 : 0;
            viewer.info_scroll = 0;
            if (!viewer.debug_mode) {
              viewer.debug_hits.clear();
              viewer.debug_page = -1;
              viewer.debug_sel = -1;
              viewer.debug_dump.clear();
            }
            dirty = true;
          } else if (ch == 's' || ch == 'S') {
            // Screenshot: render a fresh frame, then save the window surface.
            lvg_surface_t* surface = lui_window_get_surface(win);
            if (surface) {
              draw(viewer, surface);
              take_screenshot(viewer, surface);
            }
            dirty = true;
          } else if (ch == ']') {
            viewer.rotation = (viewer.rotation + 90) % 360;  // rotate CW
            dirty = true;
          } else if (ch == '[') {
            viewer.rotation = (viewer.rotation + 270) % 360;  // rotate CCW
            dirty = true;
          } else if (ch == 'h' || ch == 'H' || ch == 'u' || ch == 'U' ||
                     ch == 'x' || ch == 'X') {
            // Mark up the current text selection (h=highlight, u=underline,
            // x=strikethrough).
            if (viewer.sel_page >= 0 && !viewer.sel_quads.empty()) {
              nanopdf::MarkupType ty =
                  (ch == 'u' || ch == 'U')   ? nanopdf::MarkupType::Underline
                  : (ch == 'x' || ch == 'X') ? nanopdf::MarkupType::StrikeOut
                                             : nanopdf::MarkupType::Highlight;
              viewer.markups.push_back({viewer.sel_page, viewer.sel_quads, ty});
              viewer.toast = "Annotated  ·  press 'w' to save annotated copy";
              viewer.sel_page = -1;
              viewer.sel_quads.clear();
              viewer.sel_text.clear();
            } else {
              viewer.toast = "Select text first (h=highlight u=underline "
                             "x=strikethrough)";
            }
            dirty = true;
          } else if (ch == 'm' || ch == 'M') {
            viewer.note_placing = true;
            viewer.toast = "Click on the page to place a note";
            dirty = true;
          } else if (ch == 'w' || ch == 'W') {
            save_markups(viewer);
            dirty = true;
          }
          break;
        }
        case LUI_EVENT_MOUSE_MOVE: {
          viewer.mouse_x = (int)event.data.mouse_move.x;
          viewer.mouse_y = (int)event.data.mouse_move.y;
          if (viewer.selecting) {
            viewer.sel_x1 = viewer.mouse_x;
            viewer.sel_y1 = viewer.mouse_y;
            dirty = true;
          } else if (!viewer.doc.loaded()) {
            dirty = true;  // refresh the Open-button hover state
          }
          break;
        }
        case LUI_EVENT_MOUSE_DOWN: {
          if (event.data.mouse_button.button != LUI_MOUSE_LEFT) break;
          int mx = (int)event.data.mouse_button.x;
          int my = (int)event.data.mouse_button.y;
          viewer.mouse_x = mx;
          viewer.mouse_y = my;
          const int panel_y = sidebar_panel_y();
          if (content_x > 0 && mx < content_x) {  // sidebar
            if (my >= kToolbarH && my < panel_y) {
              viewer.sidebar_mode = (mx < kSidebarW / 2) ? 0 : 1;
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
            } else if (viewer.right_panel == 3 && my >= kToolbarH) {
              int lh = viewer.font ? lui_font_line_height(viewer.font) : 14;
              int first = kToolbarH + 8 + lh + 8 - viewer.info_scroll;
              int idx = (my - first) / kRowH;
              if (idx >= 0 && idx < (int)viewer.debug_hits.size()) {
                viewer.debug_sel = idx;
                const pdfview::RegionHit& h = viewer.debug_hits[idx];
                if (h.obj_num >= 0)
                  viewer.debug_dump = pdfview::dump_object(
                      viewer.doc, (uint32_t)h.obj_num, (uint16_t)h.gen_num);
                else
                  viewer.debug_dump =
                      h.detail.empty() ? "(no PDF object)" : h.detail;
                dirty = true;
              }
            }
            break;
          }
          // Empty state: click the "Open PDF…" button.
          if (!viewer.doc.loaded() && viewer.open_btn_w > 0 &&
              mx >= viewer.open_btn_x &&
              mx < viewer.open_btn_x + viewer.open_btn_w &&
              my >= viewer.open_btn_y &&
              my < viewer.open_btn_y + viewer.open_btn_h) {
            open_via_dialog(viewer, win);
            dirty = true;
            break;
          }
          // Note placement: drop a sticky note where clicked, then type it.
          if (viewer.note_placing && my >= kToolbarH && mx >= content_x &&
              mx < W - right_w && viewer.doc.loaded()) {
            double gx, gy;
            screen_to_page(viewer, mx, my, &gx, &gy);
            viewer.notes.push_back({viewer.page, gx, gy, ""});
            viewer.note_placing = false;
            viewer.note_typing = true;
            viewer.toast.clear();
            dirty = true;
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
          int mx = (int)event.data.mouse_button.x;
          int my = (int)event.data.mouse_button.y;
          viewer.sel_x1 = mx;
          viewer.sel_y1 = my;
          // Tiny drag = click, not a selection.
          if (std::abs(viewer.sel_x1 - viewer.sel_x0) < ui_px(3) &&
              std::abs(viewer.sel_y1 - viewer.sel_y0) < ui_px(3)) {
            dirty = true;
            break;
          }
          if (viewer.debug_mode) {
            // Inspect the region: map to page space and find PDF objects there.
            double gx0, gy0, gx1, gy1;
            screen_to_page(viewer, viewer.sel_x0, viewer.sel_y0, &gx0, &gy0);
            screen_to_page(viewer, viewer.sel_x1, viewer.sel_y1, &gx1, &gy1);
            viewer.debug_hits =
                pdfview::inspect_region(viewer.doc, viewer.page, gx0, gy0, gx1, gy1);
            viewer.debug_page = viewer.page;
            viewer.debug_sel = -1;
            viewer.debug_dump.clear();
            viewer.right_panel = 3;
            viewer.info_scroll = 0;
            std::printf("pdfview: inspect region: %zu hit(s)\n",
                        viewer.debug_hits.size());
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
            if (!sel.text.empty()) {
              std::printf("pdfview: selected: %s\n", sel.text.c_str());
              viewer.toast = "Selected " +
                             std::to_string(utf8_char_count(sel.text)) +
                             " chars  ·  Ctrl+C to copy";
            }
          }
          dirty = true;
          break;
        }
        case LUI_EVENT_SCROLL: {
          // Route the wheel to whatever the cursor is over. Step sizes are in
          // physical px, so scale them with the UI to keep a consistent feel.
          if (right_w > 0 && viewer.mouse_x >= W - right_w) {
            viewer.info_scroll += (int)(event.data.scroll.delta_y * ui_px(60));
          } else if (content_x > 0 && viewer.mouse_x < content_x) {
            if (viewer.sidebar_mode == 0 && has_outline(viewer))
              viewer.sidebar_scroll += (int)(event.data.scroll.delta_y * ui_px(60));
            else
              viewer.thumb_scroll += (int)(event.data.scroll.delta_y * ui_px(120));
          } else {
            viewer.scroll_y += (int)(event.data.scroll.delta_y * ui_px(80));
            viewer.scroll_x += (int)(event.data.scroll.delta_x * ui_px(80));
          }
          dirty = true;
          break;
        }
        default:
          break;
      }
    } while (running && lui_window_poll_event(win, &event));
  }

#if PDFVIEW_HAVE_MCP
  if (mcp) lui_mcp_destroy(mcp);
#endif
  if (viewer.font) lui_font_destroy(viewer.font);
#if PDFVIEW_HAVE_NFD
  NFD_Quit();
#endif
  lui_window_destroy(win);
  lui_shutdown();
  return 0;
}
