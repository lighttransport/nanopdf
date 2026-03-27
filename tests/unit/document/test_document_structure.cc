// Document structure and navigation unit tests
#include "nanotest.hh"
#include "nanopdf.hh"

#include <memory>

using namespace nanopdf;

TEST_SUITE("DocumentStructure") {

TEST_CASE("Document outline handles missing outline") {
  Pdf pdf;
  pdf.root = 1;
  parse_document_outline(pdf, pdf.catalog);
  CHECK(pdf.catalog.outline_root == nullptr);
}

TEST_CASE("Page labels generation") {
  PageLabels labels;

  // Pages 0-9: lowercase Roman numerals
  PageLabel range1;
  range1.style = PageLabelStyle::LowercaseRoman;
  range1.start_value = 1;
  labels.labels[0] = range1;

  // Pages 10+: decimal Arabic with prefix
  PageLabel range2;
  range2.style = PageLabelStyle::DecimalArabic;
  range2.prefix = "Page ";
  range2.start_value = 1;
  labels.labels[10] = range2;

  CHECK_EQ(labels.get_label(0), std::string("i"));
  CHECK_EQ(labels.get_label(5), std::string("vi"));
  CHECK_EQ(labels.get_label(10), std::string("Page 1"));
  CHECK_EQ(labels.get_label(15), std::string("Page 6"));
}

TEST_CASE("Page labels parsing handles missing labels") {
  Pdf pdf;
  pdf.root = 1;
  parse_page_labels(pdf, pdf.catalog);
  CHECK(pdf.catalog.page_labels.labels.empty());
}

TEST_CASE("Named destinations handles missing") {
  Pdf pdf;
  pdf.root = 1;
  parse_named_destinations(pdf, pdf.catalog);
  CHECK(pdf.catalog.named_destinations.empty());
}

TEST_CASE("Document info handles empty trailer") {
  Pdf pdf;
  pdf.root = 1;
  parse_document_info(pdf, pdf.catalog);
  CHECK(pdf.catalog.document_info.title.empty());
}

TEST_CASE("XMP metadata handles missing") {
  Pdf pdf;
  pdf.root = 1;
  parse_xmp_metadata(pdf, pdf.catalog);
  // Should not throw
  CHECK(true);
}

TEST_CASE("Outline hierarchy with children") {
  auto root = std::make_unique<OutlineItem>();
  root->title = "Part 1";
  root->action_type = OutlineAction::GoTo;

  auto child1 = std::make_unique<OutlineItem>();
  child1->title = "Chapter 1";
  child1->action_type = OutlineAction::GoTo;

  auto child2 = std::make_unique<OutlineItem>();
  child2->title = "Chapter 2";
  child2->action_type = OutlineAction::GoTo;

  root->children.push_back(std::move(child1));
  root->children.push_back(std::move(child2));

  CHECK_EQ(root->title, std::string("Part 1"));
  CHECK_EQ(root->children.size(), size_t(2));
  CHECK_EQ(root->children[0]->title, std::string("Chapter 1"));
  CHECK_EQ(root->children[1]->title, std::string("Chapter 2"));
}

} // TEST_SUITE
