// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "pdf_document.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "font-provider.hh"

namespace pdfview {

namespace {

bool register_face(const std::string& path, const std::string& name,
                   nanopdf::FontCategory cat, int weight, bool italic) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    std::fclose(f);
    return false;
  }
  std::vector<uint8_t> buf((size_t)n);
  size_t got = std::fread(buf.data(), 1, (size_t)n, f);
  std::fclose(f);
  if (got != (size_t)n) return false;
  return nanopdf::FontProvider::instance().register_font_blob(
      name, cat, weight, italic, buf.data(), buf.size());
}

}  // namespace

int register_bundled_fonts(const std::string& dir) {
  using FC = nanopdf::FontCategory;
  struct Entry {
    const char* sub;
    const char* file;
    const char* name;
    FC cat;
    int weight;
    bool italic;
  };
  static const Entry table[] = {
      // Standard-14 substitutes (sans=Helvetica, serif=Times, mono=Courier).
      {"arimo", "Arimo-Regular.ttf", "Arimo", FC::kSans, 400, false},
      {"arimo", "Arimo-Bold.ttf", "Arimo", FC::kSans, 700, false},
      {"arimo", "Arimo-Italic.ttf", "Arimo", FC::kSans, 400, true},
      {"arimo", "Arimo-BoldItalic.ttf", "Arimo", FC::kSans, 700, true},
      {"tinos", "Tinos-Regular.ttf", "Tinos", FC::kSerif, 400, false},
      {"tinos", "Tinos-Bold.ttf", "Tinos", FC::kSerif, 700, false},
      {"tinos", "Tinos-Italic.ttf", "Tinos", FC::kSerif, 400, true},
      {"tinos", "Tinos-BoldItalic.ttf", "Tinos", FC::kSerif, 700, true},
      {"cousine", "Cousine-Regular.ttf", "Cousine", FC::kMono, 400, false},
      {"cousine", "Cousine-Bold.ttf", "Cousine", FC::kMono, 700, false},
      {"cousine", "Cousine-Italic.ttf", "Cousine", FC::kMono, 400, true},
      {"cousine", "Cousine-BoldItalic.ttf", "Cousine", FC::kMono, 700, true},
      // Symbol / math.
      {"stix", "STIXTwoMath-Regular.otf", "STIXTwoMath", FC::kSymbol, 400, false},
      {"noto-symbols", "NotoSansSymbols-Regular.ttf", "NotoSansSymbols",
       FC::kSymbol, 400, false},
      // CJK (Japanese). Regular + Bold cover most documents.
      {"noto-sans-jp", "NotoSansJP-Regular.otf", "NotoSansJP", FC::kCJKSans, 400,
       false},
      {"noto-sans-jp", "NotoSansJP-Medium.otf", "NotoSansJP", FC::kCJKSans, 500,
       false},
      {"noto-sans-jp", "NotoSansJP-Bold.otf", "NotoSansJP", FC::kCJKSans, 700,
       false},
      {"noto-serif-jp", "NotoSerifJP-Regular.otf", "NotoSerifJP", FC::kCJKSerif,
       400, false},
      {"noto-serif-jp", "NotoSerifJP-Bold.otf", "NotoSerifJP", FC::kCJKSerif, 700,
       false},
  };
  int count = 0;
  for (const Entry& e : table) {
    std::string path = dir + "/" + e.sub + "/" + e.file;
    if (register_face(path, e.name, e.cat, e.weight, e.italic)) ++count;
  }
  return count;
}


bool PdfDocument::load_file(const std::string& path) {
  loaded_ = false;
  error_.clear();
  cache_.clear();
  thumb_cache_.clear();
  outline_.clear();
  outline_built_ = false;
  text_page_.reset();
  text_page_index_ = -1;
  pdf_.reset();

  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    error_ = "cannot open file: " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    std::fclose(f);
    error_ = "empty or unreadable file: " + path;
    return false;
  }
  data_.resize((size_t)n);
  size_t got = std::fread(data_.data(), 1, (size_t)n, f);
  std::fclose(f);
  if (got != (size_t)n) {
    error_ = "short read on file: " + path;
    return false;
  }

  pdf_ = std::make_unique<nanopdf::Pdf>();
  if (!nanopdf::parse_from_memory(data_.data(), data_.size(), pdf_.get())) {
    error_ = "failed to parse PDF: " + path;
    pdf_.reset();
    return false;
  }
  pdf_->load_document_structure();

  if (!backend_) {
    backend_ = nanopdf::make_backend(nanopdf::BackendKind::LightVG);
    if (!backend_) {
      error_ = "LightVG backend not available in this nanopdf build";
      pdf_.reset();
      return false;
    }
  }

  path_ = path;
  loaded_ = true;
  return true;
}

int PdfDocument::page_count() const {
  return pdf_ ? static_cast<int>(pdf_->catalog.pages.size()) : 0;
}

namespace {

void append_utf8(std::string& out, unsigned cp) {
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

}  // namespace

// Decode a PDF text string (UTF-16BE with BOM, else PDFDocEncoding ~ Latin-1)
// to UTF-8 for display. Public so the viewer can decode metadata too.
std::string decode_pdf_text(const std::string& s) {
  if (s.size() >= 2 && (unsigned char)s[0] == 0xFE &&
      (unsigned char)s[1] == 0xFF) {
    std::string out;
    for (size_t i = 2; i + 1 < s.size(); i += 2) {
      unsigned cp = ((unsigned char)s[i] << 8) | (unsigned char)s[i + 1];
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < s.size()) {
        unsigned lo = ((unsigned char)s[i + 2] << 8) | (unsigned char)s[i + 3];
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 2;
        }
      }
      append_utf8(out, cp);
    }
    return out;
  }
  bool ascii = true;
  for (unsigned char c : s)
    if (c >= 0x80) { ascii = false; break; }
  if (ascii) return s;
  std::string out;
  for (unsigned char c : s) append_utf8(out, c);  // Latin-1 approximation
  return out;
}

namespace {

void flatten_outline(const nanopdf::OutlineItem* it, int depth,
                     std::vector<Bookmark>& out) {
  if (!it) return;
  if (!it->title.empty()) {
    Bookmark b;
    b.title = decode_pdf_text(it->title);
    b.depth = depth;
    b.page = (it->action_type == nanopdf::OutlineAction::GoTo)
                 ? (int)it->dest_page
                 : -1;
    out.push_back(std::move(b));
  }
  for (const auto& child : it->children)
    flatten_outline(child.get(), depth + 1, out);
}
}  // namespace

const std::vector<Bookmark>& PdfDocument::outline() {
  if (outline_built_) return outline_;
  outline_built_ = true;
  if (pdf_) {
    pdf_->ensure_outline_loaded();
    if (pdf_->catalog.outline_root)
      flatten_outline(pdf_->catalog.outline_root.get(), 0, outline_);
  }
  return outline_;
}

const nanopdf::TextPage* PdfDocument::text_page(int page_index) {
  if (!loaded_ || !pdf_) return nullptr;
  if (page_index < 0 || page_index >= page_count()) return nullptr;
  if (text_page_ && text_page_index_ == page_index) return text_page_.get();
  text_page_ = nanopdf::extract_text_layout(
      *pdf_, pdf_->catalog.pages[(size_t)page_index]);
  text_page_index_ = page_index;
  return text_page_.get();
}

void PdfDocument::page_box(int page_index, double* left, double* bottom,
                           double* right, double* top) const {
  double l = 0, b = 0, r = 612, t = 792;
  if (pdf_ && page_index >= 0 && page_index < page_count()) {
    const auto& pg = pdf_->catalog.pages[(size_t)page_index];
    if (pg.media_box.size() >= 4) {
      l = pg.media_box[0];
      b = pg.media_box[1];
      r = pg.media_box[2];
      t = pg.media_box[3];
    }
  }
  if (left) *left = l;
  if (bottom) *bottom = b;
  if (right) *right = r;
  if (top) *top = t;
}

void PdfDocument::page_size_points(int page_index, double* w_pt,
                                   double* h_pt) const {
  double w = 612.0, h = 792.0;  // US Letter fallback
  if (pdf_ && page_index >= 0 && page_index < page_count()) {
    const auto& page = pdf_->catalog.pages[(size_t)page_index];
    if (page.media_box.size() >= 4) {
      w = page.media_box[2] - page.media_box[0];
      h = page.media_box[3] - page.media_box[1];
    }
    // /Rotate of 90/270 swaps the visible aspect.
    int rot = ((int)std::lround(page.rotate) % 360 + 360) % 360;
    if (rot == 90 || rot == 270) std::swap(w, h);
  }
  if (w_pt) *w_pt = w;
  if (h_pt) *h_pt = h;
}

const RenderedPage* PdfDocument::render(int page_index, float scale) {
  return render_into(page_index, scale, cache_, kMaxCached);
}

bool PdfDocument::render_external(const nanopdf::Pdf& doc, int page_index,
                                 float scale, RenderedPage* out) {
  if (!backend_ || !out) return false;
  if (page_index < 0 || page_index >= (int)doc.catalog.pages.size())
    return false;
  if (scale <= 0.0f) return false;
  const auto& page = doc.catalog.pages[(size_t)page_index];
  double raw_w = 612, raw_h = 792;
  if (page.media_box.size() >= 4) {
    raw_w = page.media_box[2] - page.media_box[0];
    raw_h = page.media_box[3] - page.media_box[1];
  }
  int out_w = std::max(1, (int)(raw_w * scale));
  int out_h = std::max(1, (int)(raw_h * scale));
  if (!backend_->initialize((uint32_t)out_w, (uint32_t)out_h)) return false;
  nanopdf::RenderResult result = backend_->render_page(doc, page);
  if (!result.success) return false;
  out->page_index = page_index;
  out->scale = scale;
  out->width = (int)result.width;
  out->height = (int)result.height;
  out->argb.assign((size_t)out->width * (size_t)out->height, 0);
  const uint8_t* src = result.pixels.data();
  const size_t npx = out->argb.size();
  if (result.pixels.size() >= npx * 4) {
    for (size_t i = 0; i < npx; ++i) {
      uint32_t r = src[i * 4 + 0], g = src[i * 4 + 1];
      uint32_t b = src[i * 4 + 2], a = src[i * 4 + 3];
      out->argb[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  out->surface =
      lvg_surface_wrap(out->argb.data(), out->width, out->height, out->width);
  return true;
}

const RenderedPage* PdfDocument::render_thumbnail(int page_index, int target_w) {
  double w_pt = 0, h_pt = 0;
  page_size_points(page_index, &w_pt, &h_pt);
  if (w_pt <= 0) return nullptr;
  float scale = (float)(target_w / w_pt);
  if (scale <= 0.0f) scale = 0.1f;
  return render_into(page_index, scale, thumb_cache_, kMaxThumbs);
}

const RenderedPage* PdfDocument::render_into(
    int page_index, float scale,
    std::vector<std::unique_ptr<RenderedPage>>& cache, size_t max_cached) {
  if (!loaded_ || !pdf_ || !backend_) return nullptr;
  if (page_index < 0 || page_index >= page_count()) return nullptr;
  if (scale <= 0.0f) return nullptr;

  // Cache hit?
  for (auto it = cache.begin(); it != cache.end(); ++it) {
    if ((*it)->page_index == page_index &&
        std::fabs((*it)->scale - scale) < 1e-4f) {
      // Move to back (most-recently-used) and return.
      std::unique_ptr<RenderedPage> hit = std::move(*it);
      cache.erase(it);
      const RenderedPage* p = hit.get();
      cache.push_back(std::move(hit));
      return p;
    }
  }

  double w_pt = 0, h_pt = 0;
  page_size_points(page_index, &w_pt, &h_pt);
  // page_size_points already swaps for rotation; the backend renders from the
  // raw media_box, so compute the target from the unrotated extents.
  const auto& page = pdf_->catalog.pages[(size_t)page_index];
  double raw_w = w_pt, raw_h = h_pt;
  if (page.media_box.size() >= 4) {
    raw_w = page.media_box[2] - page.media_box[0];
    raw_h = page.media_box[3] - page.media_box[1];
  }
  // Use ceil to keep the full PDF page covered when point-to-pixel conversion
  // lands between pixels, matching the DPI render backend and rasterize CLI.
  int out_w = std::max(1, (int)std::ceil(raw_w * scale));
  int out_h = std::max(1, (int)std::ceil(raw_h * scale));

  if (!backend_->initialize((uint32_t)out_w, (uint32_t)out_h)) {
    error_ = "backend initialize failed";
    return nullptr;
  }
  nanopdf::RenderResult result = backend_->render_page(*pdf_, page);
  if (!result.success) {
    error_ = "render failed: " + result.error;
    return nullptr;
  }

  auto rp = std::make_unique<RenderedPage>();
  rp->page_index = page_index;
  rp->scale = scale;
  rp->width = (int)result.width;
  rp->height = (int)result.height;
  rp->argb.resize((size_t)rp->width * (size_t)rp->height);

  // RenderResult.pixels is RGBA8888 (byte order R,G,B,A); lvg surfaces are
  // ARGB32 packed 0xAARRGGBB. Repack with the R/B swap.
  const uint8_t* src = result.pixels.data();
  const size_t npx = rp->argb.size();
  if (result.pixels.size() >= npx * 4) {
    for (size_t i = 0; i < npx; ++i) {
      uint32_t r = src[i * 4 + 0];
      uint32_t g = src[i * 4 + 1];
      uint32_t b = src[i * 4 + 2];
      uint32_t a = src[i * 4 + 3];
      rp->argb[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  rp->surface = lvg_surface_wrap(rp->argb.data(), rp->width, rp->height,
                                 rp->width);

  const RenderedPage* ret = rp.get();
  cache.push_back(std::move(rp));
  while (cache.size() > max_cached) cache.erase(cache.begin());
  return ret;
}

}  // namespace pdfview
