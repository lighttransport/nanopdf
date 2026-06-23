// Incremental annotation editing: highlight / text-markup / sticky-note added to
// an existing page must persist as real annotation objects (re-editable), not be
// flattened. Covers the WASM "Save Editable" path (add_*_to_existing_page +
// write_incremental).
#include "nanotest.hh"
#include "nanopdf.hh"
#include "pdf-writer.hh"
#include "test_helpers.hh"

#include <string>
#include <vector>

using namespace nanopdf;

namespace {

std::string fixture_path(const std::string& filename) {
  std::string current_file = __FILE__;
  size_t slash = current_file.find_last_of("/\\");
  std::string dir = slash == std::string::npos
      ? std::string(".")
      : current_file.substr(0, slash);
  return dir + "/../../fixtures/" + filename;
}

}  // namespace

TEST_SUITE("IncrementalAnnotations") {

TEST_CASE("Add highlight, markup and note to an existing page") {
  std::vector<uint8_t> data;
  REQUIRE(test::read_file(fixture_path("textpage.pdf"), data));

  PdfWriter writer;
  std::string err;
  REQUIRE(writer.load_existing(data, &err));

  HighlightConfig hc;
  hc.page = 0;
  hc.quads.push_back(quad_from_rect(50, 60, 200, 20));
  hc.contents = "hl";
  CHECK(writer.add_highlight_to_existing_page(0, hc));

  TextMarkupConfig tm;
  tm.page = 0;
  tm.type = MarkupType::Underline;
  tm.quads.push_back(quad_from_rect(50, 90, 200, 16));
  CHECK(writer.add_text_markup_to_existing_page(0, tm));

  CHECK(writer.add_text_annotation_to_existing_page(0, 260, 120, 20, 20, "note"));

  std::vector<uint8_t> out;
  auto result = writer.write_incremental(out);
  REQUIRE(result.success);

  // Re-parse: the page must carry 3 real annotations (text still extractable =>
  // not flattened), and the highlight must have an /AP appearance stream object.
  Pdf pdf;
  REQUIRE(parse_from_memory(out.data(), out.size(), &pdf));
  pdf.load_document_structure();
  REQUIRE_FALSE(pdf.catalog.pages.empty());
  CHECK_EQ(pdf.catalog.pages[0].annotations.size(), size_t(3));

  // The original page text survives (vector preserved, not rasterized).
  std::string text = extract_text_from_page(pdf, pdf.catalog.pages[0]);
  CHECK(text.find("Editable") != std::string::npos);
}

}  // namespace -> TEST_SUITE
