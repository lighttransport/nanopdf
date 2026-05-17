// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: MIT
//
// Integration tests: Parse real PDF files from data/ and verify end-to-end
// parsing behavior.

#include "nanotest.hh"
#include "nanopdf.hh"

#include <fstream>
#include <string>
#include <vector>

using namespace nanopdf;

namespace {

// Paths to project fixture directories, set via CMake define.
#ifdef NANOPDF_PROJECT_DIR
static const std::string kDataDir = std::string(NANOPDF_PROJECT_DIR) + "/data/";
static const std::string kVisualFixtureDir =
    std::string(NANOPDF_PROJECT_DIR) + "/tests/fixtures/visual/";
#else
// Fallback: assume build directory is one level below project root.
static const std::string kDataDir = "../data/";
static const std::string kVisualFixtureDir = "../tests/fixtures/visual/";
#endif

std::vector<uint8_t> load_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
}

std::vector<uint8_t> load_fixture_pdf(const std::string& rel_path) {
  auto data = load_file(kDataDir + rel_path);
  if (!data.empty()) return data;
  return load_file(kVisualFixtureDir + rel_path);
}

}  // namespace

// ---------------------------------------------------------------------------
// ParseRealPDFs suite
// ---------------------------------------------------------------------------

TEST_SUITE("ParseRealPDFs") {

TEST_CASE("Parse blank.pdf succeeds with valid structure") {
  auto data = load_fixture_pdf("blank.pdf");
  SKIP_IF(data.empty(), "blank.pdf not found");

  Pdf pdf;
  bool ok = parse_from_memory(data.data(), data.size(), &pdf);
  REQUIRE(ok);

  // Version should be reasonable (1.x or 2.x)
  CHECK(pdf.version_major >= 1);
  CHECK(pdf.version_major <= 2);
  CHECK(pdf.version_minor >= 0);
  CHECK(pdf.version_minor <= 9);

  // Data pointer should be set after successful parse
  CHECK(pdf.data != nullptr);
  CHECK(pdf.data_size > 0);
}

TEST_CASE("blank.pdf document structure loads with pages") {
  auto data = load_fixture_pdf("blank.pdf");
  SKIP_IF(data.empty(), "blank.pdf not found");

  Pdf pdf;
  REQUIRE(parse_from_memory(data.data(), data.size(), &pdf));
  REQUIRE(pdf.load_document_structure());

  // Catalog should be populated
  CHECK(pdf.catalog.object_number > 0);
  CHECK(pdf.catalog.pages_count > 0);
  CHECK(pdf.catalog.pages.size() == pdf.catalog.pages_count);

  // First page should be accessible
  const Page* page = pdf.get_page(0);
  REQUIRE(page != nullptr);

  // MediaBox should have 4 elements and reasonable dimensions
  CHECK_EQ(page->media_box.size(), size_t(4));
  if (page->media_box.size() == 4) {
    double width = page->media_box[2] - page->media_box[0];
    double height = page->media_box[3] - page->media_box[1];
    CHECK(width > 0);
    CHECK(height > 0);
  }
}

TEST_CASE("Parse test_cmyk.pdf succeeds with at least 1 page") {
  auto data = load_fixture_pdf("test_cmyk.pdf");
  SKIP_IF(data.empty(), "test_cmyk.pdf not found");

  Pdf pdf;
  REQUIRE(parse_from_memory(data.data(), data.size(), &pdf));
  REQUIRE(pdf.load_document_structure());

  CHECK(pdf.catalog.pages_count >= 1);
}

TEST_CASE("Parse test_curves.pdf succeeds and loads document structure") {
  auto data = load_fixture_pdf("test_curves.pdf");
  SKIP_IF(data.empty(), "test_curves.pdf not found");

  Pdf pdf;
  REQUIRE(parse_from_memory(data.data(), data.size(), &pdf));
  REQUIRE(pdf.load_document_structure());

  CHECK(pdf.catalog.pages_count >= 1);
  CHECK(pdf.catalog.pages.size() > 0);
}

TEST_CASE("Parse test_blendmodes.pdf without crashing") {
  auto data = load_fixture_pdf("test_blendmodes.pdf");
  SKIP_IF(data.empty(), "test_blendmodes.pdf not found");

  Pdf pdf;
  bool ok = parse_from_memory(data.data(), data.size(), &pdf);
  CHECK(ok);
}

TEST_CASE("Parse test_linestyles.pdf without crashing") {
  auto data = load_fixture_pdf("test_linestyles.pdf");
  SKIP_IF(data.empty(), "test_linestyles.pdf not found");

  Pdf pdf;
  bool ok = parse_from_memory(data.data(), data.size(), &pdf);
  CHECK(ok);
}

TEST_CASE("Parse test_softmask.pdf without crashing") {
  auto data = load_fixture_pdf("test_softmask.pdf");
  SKIP_IF(data.empty(), "test_softmask.pdf not found");

  Pdf pdf;
  bool ok = parse_from_memory(data.data(), data.size(), &pdf);
  CHECK(ok);
}

TEST_CASE("Parse test_transforms.pdf without crashing") {
  auto data = load_fixture_pdf("test_transforms.pdf");
  SKIP_IF(data.empty(), "test_transforms.pdf not found");

  Pdf pdf;
  bool ok = parse_from_memory(data.data(), data.size(), &pdf);
  CHECK(ok);
}

TEST_CASE("Parse test_winding.pdf without crashing") {
  auto data = load_fixture_pdf("test_winding.pdf");
  SKIP_IF(data.empty(), "test_winding.pdf not found");

  Pdf pdf;
  bool ok = parse_from_memory(data.data(), data.size(), &pdf);
  CHECK(ok);
}

TEST_CASE("Parse standardencoding PDFs without crashing") {
  const char* se_files[] = {
      "standardencoding/sample_standardencoding.pdf",
      "standardencoding/pdfa_font_encoding.pdf",
  };

  for (const char* rel : se_files) {
    auto data = load_fixture_pdf(rel);
    if (data.empty()) {
      std::cout << "[nanotest] SKIP: " << rel << " not found\n";
      continue;
    }

    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    CHECK(ok);

    if (ok) {
      bool loaded = pdf.load_document_structure();
      CHECK(loaded);
      if (loaded) {
        CHECK(pdf.catalog.pages_count >= 1);
      }
    }
  }
}

TEST_CASE("All tracked test PDFs parse successfully") {
  // Every small PDF fixture tracked in git should parse without error.
  const char* tracked_pdfs[] = {
      "blank.pdf",
      "test_blendmodes.pdf",
      "test_cmyk.pdf",
      "test_curves.pdf",
      "test_linestyles.pdf",
      "test_softmask.pdf",
      "test_transforms.pdf",
      "test_winding.pdf",
      "standardencoding/sample_standardencoding.pdf",
      "standardencoding/pdfa_font_encoding.pdf",
  };

  int parsed = 0;
  int skipped = 0;
  for (const char* rel : tracked_pdfs) {
    auto data = load_fixture_pdf(rel);
    if (data.empty()) {
      ++skipped;
      continue;
    }
    Pdf pdf;
    bool ok = parse_from_memory(data.data(), data.size(), &pdf);
    CHECK(ok);
    if (ok) ++parsed;
  }

  // At least some PDFs should have been parsed
  CHECK(parsed > 0);
  std::cout << "  Parsed " << parsed << " PDFs, skipped " << skipped << "\n";
}

TEST_CASE("parse_pdf returns ParseResult with success details") {
  auto data = load_fixture_pdf("blank.pdf");
  SKIP_IF(data.empty(), "blank.pdf not found");

  Pdf pdf;
  ParseResult result = parse_pdf(data.data(), data.size(), &pdf);
  REQUIRE(result.success);
  CHECK(result.error.empty());
  CHECK(result.kind == ErrorKind::None);
}

TEST_CASE("ParseOptions auto_repair=true works on valid PDFs") {
  auto data = load_fixture_pdf("blank.pdf");
  SKIP_IF(data.empty(), "blank.pdf not found");

  ParseOptions opts;
  opts.auto_repair = true;

  Pdf pdf;
  bool ok = parse_from_memory(data.data(), data.size(), &pdf, opts);
  REQUIRE(ok);
  REQUIRE(pdf.load_document_structure());
  CHECK(pdf.catalog.pages_count > 0);
}

TEST_CASE("ParseOptions recover_stream_length=true on valid PDFs") {
  auto data = load_fixture_pdf("test_curves.pdf");
  SKIP_IF(data.empty(), "test_curves.pdf not found");

  ParseOptions opts;
  opts.recover_stream_length = true;

  Pdf pdf;
  bool ok = parse_from_memory(data.data(), data.size(), &pdf, opts);
  REQUIRE(ok);
  REQUIRE(pdf.load_document_structure());
  CHECK(pdf.catalog.pages_count >= 1);
}

TEST_CASE("Parsing empty data fails gracefully") {
  Pdf pdf;
  bool ok = parse_from_memory(nullptr, 0, &pdf);
  CHECK_FALSE(ok);
}

TEST_CASE("Parsing garbage data fails gracefully") {
  std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE};
  Pdf pdf;
  bool ok = parse_from_memory(garbage.data(), garbage.size(), &pdf);
  CHECK_FALSE(ok);
}

TEST_CASE("Multiple pages accessible on multi-page PDF") {
  // test_cmyk.pdf or any multi-page PDF
  const char* candidates[] = {
      "test_cmyk.pdf",
      "test_blendmodes.pdf",
      "blank.pdf",
  };

  for (const char* rel : candidates) {
    auto data = load_fixture_pdf(rel);
    if (data.empty()) continue;

    Pdf pdf;
    if (!parse_from_memory(data.data(), data.size(), &pdf)) continue;
    if (!pdf.load_document_structure()) continue;

    // Verify every page is accessible (get_page uses 1-based indexing)
    for (uint32_t i = 1; i <= pdf.catalog.pages_count; ++i) {
      const Page* page = pdf.get_page(i);
      CHECK(page != nullptr);
      if (page) {
        CHECK_EQ(page->media_box.size(), size_t(4));
      }
    }

    // Out-of-bounds page should return nullptr.
    // get_page uses 1-based indexing (0 is special for first page),
    // so pages_count+1 is always out of range.
    const Page* oob = pdf.get_page(pdf.catalog.pages_count + 1);
    CHECK(oob == nullptr);

    std::cout << "  " << rel << ": " << pdf.catalog.pages_count << " page(s)\n";
    break;  // One successful test is enough
  }
}

}  // TEST_SUITE("ParseRealPDFs")

int main() { return nanotest::run_all_tests(); }
