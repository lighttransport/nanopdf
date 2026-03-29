// Attachment extraction unit tests
#include "nanotest.hh"
#include "nanopdf.hh"
#include "pdf-attachments.hh"

using namespace nanopdf;

TEST_SUITE("Attachments") {

TEST_CASE("AttachmentExtractor on empty PDF") {
  Pdf pdf;
  pdf.root = 1;

  AttachmentExtractor extractor(pdf);
  CHECK_EQ(extractor.get_count(), 0);

  std::vector<std::string> names = extractor.list_names();
  CHECK(names.empty());
}

TEST_CASE("AttachmentExtractor get_attachment_by_name with no attachments") {
  Pdf pdf;
  pdf.root = 1;

  AttachmentExtractor extractor(pdf);
  FileAttachment att = extractor.get_attachment_by_name("nonexistent");
  CHECK_FALSE(att.success);
}

} // TEST_SUITE
