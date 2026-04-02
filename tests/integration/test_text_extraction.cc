// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: MIT
//
// Integration tests: Text extraction on real PDF files.

#include "nanotest.hh"
#include "nanopdf.hh"
#include "text-layout.hh"

#include <fstream>
#include <cctype>
#include <string>
#include <vector>

using namespace nanopdf;

namespace {

#ifdef NANOPDF_PROJECT_DIR
static const std::string kDataDir = std::string(NANOPDF_PROJECT_DIR) + "/data/";
#else
static const std::string kDataDir = "../data/";
#endif

std::vector<uint8_t> load_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
}

// Helper: parse PDF and load document structure.
// Returns false if file is missing or parse fails.
bool open_pdf(const std::string& rel_path, std::vector<uint8_t>& data,
              Pdf& pdf) {
  data = load_file(kDataDir + rel_path);
  if (data.empty()) return false;
  if (!parse_from_memory(data.data(), data.size(), &pdf)) return false;
  if (!pdf.load_document_structure()) return false;
  return true;
}

std::string to_lower_ascii(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

std::string pick_search_token(const std::string& text) {
  std::string current;
  for (char ch : text) {
    if (std::isalnum(static_cast<unsigned char>(ch))) {
      current.push_back(ch);
    } else {
      if (current.size() >= 4) {
        return current;
      }
      current.clear();
    }
  }

  if (current.size() >= 4) {
    return current;
  }

  return std::string();
}

std::string mutate_search_token(std::string token) {
  if (token.size() >= 5) {
    token.erase(1, 1);
    return token;
  }
  if (!token.empty()) {
    token[0] = (token[0] == 'x') ? 'y' : 'x';
  }
  return token;
}

}  // namespace

// ---------------------------------------------------------------------------
// TextExtraction suite
// ---------------------------------------------------------------------------

TEST_SUITE("TextExtraction") {

TEST_CASE("extract_text_from_page on blank.pdf does not crash") {
  std::vector<uint8_t> data;
  Pdf pdf;
  SKIP_IF(!open_pdf("blank.pdf", data, pdf), "blank.pdf not available");
  REQUIRE(pdf.catalog.pages_count > 0);

  const Page* page = pdf.get_page(0);
  REQUIRE(page != nullptr);

  // blank.pdf may or may not contain text; the call must not crash.
  std::string text = extract_text_from_page(pdf, *page);
  // We accept any result (empty or not) -- just verify no crash.
  CHECK(true);

  std::cout << "  blank.pdf text length: " << text.size() << "\n";
}

TEST_CASE("extract_text_layout on blank.pdf returns valid TextPage") {
  std::vector<uint8_t> data;
  Pdf pdf;
  SKIP_IF(!open_pdf("blank.pdf", data, pdf), "blank.pdf not available");

  const Page* page = pdf.get_page(0);
  REQUIRE(page != nullptr);

  auto text_page = extract_text_layout(pdf, *page);
  REQUIRE(text_page != nullptr);

  // Page dimensions must be positive
  CHECK(text_page->page_width > 0);
  CHECK(text_page->page_height > 0);

  std::cout << "  blank.pdf page dimensions: " << text_page->page_width
            << " x " << text_page->page_height << "\n";
  std::cout << "  Characters: " << text_page->chars.size()
            << ", Lines: " << text_page->lines.size()
            << ", Words: " << text_page->words.size() << "\n";
}

TEST_CASE("Page dimensions are reasonable for all tracked PDFs") {
  const char* tracked_pdfs[] = {
      "blank.pdf",
      "test_blendmodes.pdf",
      "test_cmyk.pdf",
      "test_curves.pdf",
      "test_linestyles.pdf",
      "test_softmask.pdf",
      "test_transforms.pdf",
      "test_winding.pdf",
  };

  int checked = 0;
  for (const char* rel : tracked_pdfs) {
    std::vector<uint8_t> data;
    Pdf pdf;
    if (!open_pdf(rel, data, pdf)) continue;

    // get_page uses 1-based indexing
    for (uint32_t i = 1; i <= pdf.catalog.pages_count; ++i) {
      const Page* page = pdf.get_page(i);
      if (!page) continue;

      CHECK_EQ(page->media_box.size(), size_t(4));
      if (page->media_box.size() == 4) {
        double width = page->media_box[2] - page->media_box[0];
        double height = page->media_box[3] - page->media_box[1];
        CHECK(width > 0);
        CHECK(height > 0);
        // Typical PDF page widths are between 72 and 5000 points
        CHECK(width < 10000);
        CHECK(height < 10000);
        ++checked;
      }
    }
  }

  CHECK(checked > 0);
  std::cout << "  Checked " << checked << " page dimension(s)\n";
}

TEST_CASE("Text extraction on standardencoding PDFs does not crash") {
  const char* se_files[] = {
      "standardencoding/sample_standardencoding.pdf",
      "standardencoding/pdfa_font_encoding.pdf",
  };

  for (const char* rel : se_files) {
    std::vector<uint8_t> data;
    Pdf pdf;
    if (!open_pdf(rel, data, pdf)) {
      std::cout << "[nanotest] SKIP: " << rel << " not available\n";
      continue;
    }

    REQUIRE(pdf.catalog.pages_count >= 1);
    const Page* page = pdf.get_page(0);
    REQUIRE(page != nullptr);

    std::string text = extract_text_from_page(pdf, *page);
    // StandardEncoding PDFs should contain actual text content
    CHECK(text.size() > 0);
    std::cout << "  " << rel << " text length: " << text.size() << "\n";
  }
}

TEST_CASE("extract_text_layout on standardencoding PDFs produces lines") {
  std::vector<uint8_t> data;
  Pdf pdf;
  SKIP_IF(!open_pdf("standardencoding/sample_standardencoding.pdf", data, pdf),
          "sample_standardencoding.pdf not available");

  const Page* page = pdf.get_page(0);
  REQUIRE(page != nullptr);

  auto text_page = extract_text_layout(pdf, *page);
  REQUIRE(text_page != nullptr);

  CHECK(text_page->page_width > 0);
  CHECK(text_page->page_height > 0);

  // A StandardEncoding sample should have characters and lines
  CHECK(text_page->chars.size() > 0);
  CHECK(text_page->lines.size() > 0);

  std::string full_text = text_page->get_text();
  CHECK(full_text.size() > 0);
  std::cout << "  sample_standardencoding.pdf: " << text_page->chars.size()
            << " chars, " << text_page->lines.size() << " lines\n";
}

TEST_CASE("Text extraction on every page of multi-page PDFs") {
  const char* candidates[] = {
      "test_cmyk.pdf",
      "test_blendmodes.pdf",
      "blank.pdf",
  };

  for (const char* rel : candidates) {
    std::vector<uint8_t> data;
    Pdf pdf;
    if (!open_pdf(rel, data, pdf)) continue;

    // get_page uses 1-based indexing
    for (uint32_t i = 1; i <= pdf.catalog.pages_count; ++i) {
      const Page* page = pdf.get_page(i);
      if (!page) continue;

      // Must not crash on any page
      std::string text = extract_text_from_page(pdf, *page);
      // Result can be empty -- just ensure no crash
      CHECK(true);
    }

    std::cout << "  " << rel << ": extracted text from "
              << pdf.catalog.pages_count << " page(s)\n";
    break;  // One successful multi-page test is sufficient
  }
}

TEST_CASE("TextPage get_text returns consistent result") {
  std::vector<uint8_t> data;
  Pdf pdf;
  SKIP_IF(!open_pdf("standardencoding/sample_standardencoding.pdf", data, pdf),
          "sample_standardencoding.pdf not available");

  const Page* page = pdf.get_page(0);
  REQUIRE(page != nullptr);

  auto text_page = extract_text_layout(pdf, *page);
  REQUIRE(text_page != nullptr);

  // Calling get_text twice should give identical results
  std::string text1 = text_page->get_text();
  std::string text2 = text_page->get_text();
  CHECK_EQ(text1.size(), text2.size());
  CHECK(text1 == text2);
}

TEST_CASE("extract_text_from_page and extract_text_layout agree on presence") {
  std::vector<uint8_t> data;
  Pdf pdf;
  SKIP_IF(!open_pdf("standardencoding/sample_standardencoding.pdf", data, pdf),
          "sample_standardencoding.pdf not available");

  const Page* page = pdf.get_page(0);
  REQUIRE(page != nullptr);

  std::string simple_text = extract_text_from_page(pdf, *page);
  auto text_page = extract_text_layout(pdf, *page);
  REQUIRE(text_page != nullptr);

  // If one finds text, the other should too
  bool simple_has_text = !simple_text.empty();
  bool layout_has_text = !text_page->chars.empty();
  CHECK_EQ(simple_has_text, layout_has_text);
}

TEST_CASE("search_text_on_page finds a real token with geometry") {
  std::vector<uint8_t> data;
  Pdf pdf;
  SKIP_IF(!open_pdf("standardencoding/sample_standardencoding.pdf", data, pdf),
          "sample_standardencoding.pdf not available");

  const Page* page = pdf.get_page(0);
  REQUIRE(page != nullptr);

  std::string text = extract_text_from_page(pdf, *page);
  std::string token = pick_search_token(text);
  REQUIRE(!token.empty());

  std::vector<TextSearchResult> matches =
      search_text_on_page(pdf, *page, to_lower_ascii(token), false);

  CHECK(!matches.empty());
  if (!matches.empty()) {
    CHECK(!matches[0].context.empty());
    CHECK(matches[0].height >= 0.0);
    CHECK(matches[0].width >= 0.0);
  }
}

TEST_CASE("search_text_on_page supports fuzzy token matches") {
  std::vector<uint8_t> data;
  Pdf pdf;
  SKIP_IF(!open_pdf("standardencoding/sample_standardencoding.pdf", data, pdf),
          "sample_standardencoding.pdf not available");

  const Page* page = pdf.get_page(0);
  REQUIRE(page != nullptr);

  std::string text = extract_text_from_page(pdf, *page);
  std::string token = pick_search_token(text);
  REQUIRE(!token.empty());

  std::string mutated = mutate_search_token(to_lower_ascii(token));
  REQUIRE(mutated != to_lower_ascii(token));

  std::vector<TextSearchResult> matches =
      search_text_on_page(pdf, *page, mutated, false);

  CHECK(!matches.empty());
  if (!matches.empty()) {
    CHECK(matches[0].fuzzy);
    CHECK(matches[0].score < 1.0);
    CHECK(matches[0].score > 0.5);
  }
}

TEST_CASE("search_text respects max_results") {
  std::vector<uint8_t> data;
  Pdf pdf;
  SKIP_IF(!open_pdf("standardencoding/sample_standardencoding.pdf", data, pdf),
          "sample_standardencoding.pdf not available");

  const Page* page = pdf.get_page(0);
  REQUIRE(page != nullptr);

  std::string text = extract_text_from_page(pdf, *page);
  std::string token = pick_search_token(text);
  REQUIRE(!token.empty());

  std::vector<TextSearchResult> matches =
      search_text(pdf, to_lower_ascii(token), false, 1);

  CHECK_EQ(matches.size(), size_t(1));
  if (!matches.empty()) {
    CHECK(matches[0].page_number < pdf.catalog.pages.size());
  }
}

}  // TEST_SUITE("TextExtraction")

int main() { return nanotest::run_all_tests(); }
