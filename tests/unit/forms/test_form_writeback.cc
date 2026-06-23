// Incremental form-field writeback regression test.
//
// Covers the fix for a stack-use-after-scope in find_field_obj_by_name
// (pdf-writer.cc): the resolved object that `field_val` pointed into was
// block-scoped, so for every field after the first the lookup read freed stack
// memory. It was benign on native (stale stack happened to be intact for the
// first field) but failed under WASM for later fields — so checkbox/choice
// writeback silently failed. This exercises text + checkbox + choice fields.
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

TEST_SUITE("FormWriteback") {

// All three field kinds must persist through an incremental update — not just
// the first field in the AcroForm /Fields array.
TEST_CASE("Incremental writeback of text, checkbox and choice fields") {
  std::vector<uint8_t> data;
  REQUIRE(test::read_file(fixture_path("form_text_check_choice.pdf"), data));

  PdfWriter writer;
  std::string err;
  REQUIRE(writer.load_existing(data, &err));

  CHECK(writer.set_field_value("fullname", "John"));
  CHECK(writer.set_field_checked("subscribe", true));
  CHECK(writer.set_field_choice("color", "Blue"));

  std::vector<uint8_t> out;
  auto result = writer.write_incremental(out);
  REQUIRE(result.success);
  REQUIRE_FALSE(out.empty());

  // Re-parse the incremental output and confirm every value landed.
  Pdf pdf;
  REQUIRE(parse_from_memory(out.data(), out.size(), &pdf));
  pdf.load_document_structure();
  pdf.ensure_acro_form_loaded();

  bool name_ok = false, subscribe_ok = false, color_ok = false;
  for (const auto& f : pdf.catalog.form_fields) {
    const std::string v = f->field_value.type == Value::STRING ? f->field_value.str
                        : f->field_value.type == Value::NAME ? f->field_value.name
                        : std::string();
    if (f->partial_name == "fullname" && v == "John") name_ok = true;
    if (f->partial_name == "subscribe" && (v == "Yes" || v == "/Yes")) subscribe_ok = true;
    if (f->partial_name == "color" && v == "Blue") color_ok = true;
  }
  CHECK(name_ok);
  CHECK(subscribe_ok);
  CHECK(color_ok);
}

}  // namespace -> TEST_SUITE
