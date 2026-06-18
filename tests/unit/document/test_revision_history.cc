// Revision history and signature correlation unit tests
#include "nanotest.hh"
#include "nanopdf.hh"
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

TEST_SUITE("RevisionHistory") {

TEST_CASE("Detects signed DocMDP revision history") {
  std::vector<uint8_t> data;
  REQUIRE(test::read_file(fixture_path("signature_docmdp.pdf"), data));

  Pdf pdf;
  REQUIRE(parse_from_memory(data.data(), data.size(), &pdf));
  REQUIRE(pdf.load_document_structure());
  REQUIRE(pdf.parse_signature_fields());

  RevisionHistory history = detect_revision_history(pdf);

  REQUIRE_EQ(history.revisions.size(), size_t(2));
  CHECK_FALSE(history.current_sha256.empty());

  const RevisionHistoryEntry& signed_revision = history.revisions[0];
  CHECK_EQ(signed_revision.revision_number, size_t(1));
  CHECK_EQ(signed_revision.associated_signature,
           std::string("DocMCP Test Signature"));
  CHECK_EQ(signed_revision.added_objects.size(), size_t(8));
  CHECK(signed_revision.modified_after_signature);

  const RevisionHistoryEntry& appended_revision = history.revisions[1];
  CHECK_EQ(appended_revision.revision_number, size_t(2));
  CHECK_EQ(appended_revision.prev_xref_offset, signed_revision.xref_offset);
  REQUIRE_EQ(appended_revision.added_objects.size(), size_t(1));
  CHECK_EQ(appended_revision.added_objects[0], uint32_t(9));
  CHECK(appended_revision.modified_after_signature);
  CHECK(appended_revision.has_docmdp);
  CHECK_EQ(appended_revision.mdp_permissions, 2);
  CHECK_FALSE(appended_revision.docmdp_allowed);
  CHECK_EQ(appended_revision.docmdp_status, std::string("disallowed"));
  REQUIRE_EQ(appended_revision.docmdp_violations.size(), size_t(1));
  CHECK(appended_revision.docmdp_violations[0].find("metadata") !=
        std::string::npos);
}

}  // TEST_SUITE
