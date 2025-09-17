#include "nanopdf.hh"
#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace nanopdf;

// Test outline structures
void test_outline_structures() {
  std::cout << "Testing Outline structures..." << std::endl;

  // Test outline action enum values
  assert(static_cast<int>(OutlineAction::GoTo) == 0);
  assert(static_cast<int>(OutlineAction::GoToR) == 1);
  assert(static_cast<int>(OutlineAction::URI) == 2);
  assert(static_cast<int>(OutlineAction::Launch) == 3);

  // Test outline item
  OutlineItem item;
  assert(item.action_type == OutlineAction::GoTo);
  assert(item.dest_page == 0);
  assert(item.open == true);
  assert(item.count == 0);
  assert(!item.italic);
  assert(!item.bold);

  item.title = "Chapter 1";
  item.dest_page = 5;
  item.dest_position = {100, 500, 1.0};
  item.color = {1.0, 0.0, 0.0};  // Red
  item.italic = true;

  assert(item.title == "Chapter 1");
  assert(item.dest_page == 5);
  assert(item.dest_position.size() == 3);
  assert(item.color[0] == 1.0);
  assert(item.italic);

  std::cout << "  Outline structures: PASSED" << std::endl;
}

// Test outline hierarchy
void test_outline_hierarchy() {
  std::cout << "Testing Outline hierarchy..." << std::endl;

  auto root = std::unique_ptr<OutlineItem>(new OutlineItem());
  root->title = "Table of Contents";

  // Add children
  auto chapter1 = std::unique_ptr<OutlineItem>(new OutlineItem());
  chapter1->title = "Chapter 1";
  chapter1->dest_page = 1;

  auto chapter2 = std::unique_ptr<OutlineItem>(new OutlineItem());
  chapter2->title = "Chapter 2";
  chapter2->dest_page = 10;

  // Add subsections to chapter 1
  auto section1_1 = std::unique_ptr<OutlineItem>(new OutlineItem());
  section1_1->title = "Section 1.1";
  section1_1->dest_page = 2;

  auto section1_2 = std::unique_ptr<OutlineItem>(new OutlineItem());
  section1_2->title = "Section 1.2";
  section1_2->dest_page = 5;

  chapter1->children.push_back(std::move(section1_1));
  chapter1->children.push_back(std::move(section1_2));

  root->children.push_back(std::move(chapter1));
  root->children.push_back(std::move(chapter2));

  // Verify hierarchy
  assert(root->children.size() == 2);
  assert(root->children[0]->title == "Chapter 1");
  assert(root->children[0]->children.size() == 2);
  assert(root->children[0]->children[0]->title == "Section 1.1");
  assert(root->children[1]->title == "Chapter 2");

  std::cout << "  Outline hierarchy: PASSED" << std::endl;
}

// Test page labels
void test_page_labels() {
  std::cout << "Testing PageLabels..." << std::endl;

  // Test page label style enum
  assert(static_cast<int>(PageLabelStyle::None) == 0);
  assert(static_cast<int>(PageLabelStyle::DecimalArabic) == 1);
  assert(static_cast<int>(PageLabelStyle::UppercaseRoman) == 2);
  assert(static_cast<int>(PageLabelStyle::LowercaseRoman) == 3);
  assert(static_cast<int>(PageLabelStyle::UppercaseLetters) == 4);
  assert(static_cast<int>(PageLabelStyle::LowercaseLetters) == 5);

  // Create page labels
  PageLabels labels;

  // Cover pages: i, ii, iii (lowercase roman)
  PageLabel cover_label;
  cover_label.style = PageLabelStyle::LowercaseRoman;
  cover_label.start_value = 1;
  labels.labels[0] = cover_label;

  // Main content: 1, 2, 3... (decimal arabic with prefix)
  PageLabel main_label;
  main_label.style = PageLabelStyle::DecimalArabic;
  main_label.prefix = "Page ";
  main_label.start_value = 1;
  labels.labels[3] = main_label;

  // Appendix: A, B, C... (uppercase letters)
  PageLabel appendix_label;
  appendix_label.style = PageLabelStyle::UppercaseLetters;
  appendix_label.prefix = "Appendix ";
  appendix_label.start_value = 1;
  labels.labels[10] = appendix_label;

  // Test label generation
  assert(labels.get_label(0) == "i");
  assert(labels.get_label(1) == "ii");
  assert(labels.get_label(2) == "iii");
  assert(labels.get_label(3) == "Page 1");
  assert(labels.get_label(4) == "Page 2");
  assert(labels.get_label(9) == "Page 7");
  assert(labels.get_label(10) == "Appendix A");
  assert(labels.get_label(11) == "Appendix B");

  std::cout << "  PageLabels: PASSED" << std::endl;
}

// Test roman numeral conversion
void test_roman_numerals() {
  std::cout << "Testing Roman numerals..." << std::endl;

  PageLabels labels;
  PageLabel roman_label;

  // Test uppercase roman
  roman_label.style = PageLabelStyle::UppercaseRoman;
  roman_label.start_value = 1;
  labels.labels[0] = roman_label;

  assert(labels.get_label(0) == "I");
  assert(labels.get_label(3) == "IV");
  assert(labels.get_label(8) == "IX");
  assert(labels.get_label(39) == "XL");
  assert(labels.get_label(89) == "XC");
  assert(labels.get_label(399) == "CD");
  assert(labels.get_label(899) == "CM");
  assert(labels.get_label(1993) == "MCMXCIV");

  // Test lowercase roman
  roman_label.style = PageLabelStyle::LowercaseRoman;
  labels.labels[0] = roman_label;

  assert(labels.get_label(0) == "i");
  assert(labels.get_label(3) == "iv");
  assert(labels.get_label(8) == "ix");

  std::cout << "  Roman numerals: PASSED" << std::endl;
}

// Test letter numbering
void test_letter_numbering() {
  std::cout << "Testing Letter numbering..." << std::endl;

  PageLabels labels;
  PageLabel letter_label;

  // Test uppercase letters
  letter_label.style = PageLabelStyle::UppercaseLetters;
  letter_label.start_value = 1;
  labels.labels[0] = letter_label;

  assert(labels.get_label(0) == "A");
  assert(labels.get_label(25) == "Z");
  assert(labels.get_label(26) == "AA");
  assert(labels.get_label(27) == "AB");
  assert(labels.get_label(51) == "AZ");
  assert(labels.get_label(52) == "BA");

  // Test lowercase letters
  letter_label.style = PageLabelStyle::LowercaseLetters;
  labels.labels[0] = letter_label;

  assert(labels.get_label(0) == "a");
  assert(labels.get_label(25) == "z");
  assert(labels.get_label(26) == "aa");

  std::cout << "  Letter numbering: PASSED" << std::endl;
}

// Test named destinations
void test_named_destinations() {
  std::cout << "Testing NamedDestinations..." << std::endl;

  NamedDestination dest;
  assert(dest.page_number == 0);
  assert(dest.position.empty());
  assert(dest.fit_type.empty());

  dest.name = "chapter1";
  dest.page_number = 5;
  dest.position = {100, 200, 1.5};
  dest.fit_type = "FitR";

  assert(dest.name == "chapter1");
  assert(dest.page_number == 5);
  assert(dest.position.size() == 3);
  assert(dest.position[0] == 100);
  assert(dest.fit_type == "FitR");

  std::cout << "  NamedDestinations: PASSED" << std::endl;
}

// Test document info
void test_document_info() {
  std::cout << "Testing DocumentInfo..." << std::endl;

  DocumentInfo info;
  assert(info.title.empty());
  assert(info.author.empty());
  assert(info.custom.empty());

  info.title = "Test Document";
  info.author = "John Doe";
  info.subject = "PDF Testing";
  info.keywords = "test, pdf, document";
  info.creator = "Test Application";
  info.producer = "nanopdf";
  info.creation_date = "D:20240101120000";
  info.mod_date = "D:20240102130000";
  info.trapped = "False";

  info.custom["CustomField"] = "CustomValue";

  assert(info.title == "Test Document");
  assert(info.author == "John Doe");
  assert(info.subject == "PDF Testing");
  assert(info.keywords == "test, pdf, document");
  assert(info.creator == "Test Application");
  assert(info.producer == "nanopdf");
  assert(info.creation_date == "D:20240101120000");
  assert(info.mod_date == "D:20240102130000");
  assert(info.trapped == "False");
  assert(info.custom.size() == 1);
  assert(info.custom["CustomField"] == "CustomValue");

  std::cout << "  DocumentInfo: PASSED" << std::endl;
}

// Test XMP metadata
void test_xmp_metadata() {
  std::cout << "Testing XMPMetadata..." << std::endl;

  XMPMetadata xmp;
  assert(xmp.raw_xml.empty());
  assert(xmp.dc_title.empty());

  // Test simple XML parsing
  std::string xml = R"(
    <?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>
    <x:xmpmeta xmlns:x="adobe:ns:meta/">
      <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
        <rdf:Description rdf:about="">
          <dc:title xmlns:dc="http://purl.org/dc/elements/1.1/">
            <rdf:Alt>
              <rdf:li xml:lang="x-default">Test Document Title</rdf:li>
            </rdf:Alt>
          </dc:title>
          <dc:creator xmlns:dc="http://purl.org/dc/elements/1.1/">
            <rdf:Seq>
              <rdf:li>John Smith</rdf:li>
            </rdf:Seq>
          </dc:creator>
          <xmp:CreateDate xmlns:xmp="http://ns.adobe.com/xap/1.0/">2024-01-01T12:00:00Z</xmp:CreateDate>
          <xmp:ModifyDate xmlns:xmp="http://ns.adobe.com/xap/1.0/">2024-01-02T13:00:00Z</xmp:ModifyDate>
          <pdf:Producer xmlns:pdf="http://ns.adobe.com/pdf/1.3/">Test Producer</pdf:Producer>
        </rdf:Description>
      </rdf:RDF>
    </x:xmpmeta>
    <?xpacket end="w"?>
  )";

  bool parsed = xmp.parse_xml(xml);
  assert(parsed);
  assert(xmp.raw_xml == xml);
  assert(xmp.dc_title == "Test Document Title");
  assert(xmp.dc_creator == "John Smith");
  assert(xmp.xmp_create_date == "2024-01-01T12:00:00Z");
  assert(xmp.xmp_modify_date == "2024-01-02T13:00:00Z");
  assert(xmp.pdf_producer == "Test Producer");

  std::cout << "  XMPMetadata: PASSED" << std::endl;
}

// Test document catalog with Phase 4 features
void test_document_catalog_phase4() {
  std::cout << "Testing DocumentCatalog Phase 4 features..." << std::endl;

  DocumentCatalog catalog;

  // Add outline
  catalog.outline_root = std::unique_ptr<OutlineItem>(new OutlineItem());
  catalog.outline_root->title = "Contents";

  // Add page labels
  PageLabel label;
  label.style = PageLabelStyle::DecimalArabic;
  catalog.page_labels.labels[0] = label;

  // Add named destination
  NamedDestination dest;
  dest.name = "intro";
  dest.page_number = 1;
  catalog.named_destinations["intro"] = dest;

  // Add document info
  catalog.document_info.title = "Test PDF";
  catalog.document_info.author = "Test Author";

  // Add XMP metadata
  catalog.xmp_metadata.dc_title = "Test PDF XMP";

  assert(catalog.outline_root != nullptr);
  assert(catalog.outline_root->title == "Contents");
  assert(!catalog.page_labels.labels.empty());
  assert(catalog.named_destinations.size() == 1);
  assert(catalog.named_destinations["intro"].page_number == 1);
  assert(catalog.document_info.title == "Test PDF");
  assert(catalog.xmp_metadata.dc_title == "Test PDF XMP");

  std::cout << "  DocumentCatalog Phase 4 features: PASSED" << std::endl;
}

int main() {
  std::cout << "=== Phase 4 Feature Tests ===\n" << std::endl;

  test_outline_structures();
  test_outline_hierarchy();
  test_page_labels();
  test_roman_numerals();
  test_letter_numbering();
  test_named_destinations();
  test_document_info();
  test_xmp_metadata();
  test_document_catalog_phase4();

  std::cout << "\n=== All Phase 4 tests passed! ===\n" << std::endl;
  std::cout << "Summary of implemented Phase 4 features:" << std::endl;
  std::cout << "  ✓ Document outlines (bookmarks) with hierarchy" << std::endl;
  std::cout << "  ✓ Outline actions (GoTo, GoToR, URI, Launch)" << std::endl;
  std::cout << "  ✓ Outline appearance (color, italic, bold)" << std::endl;
  std::cout << "  ✓ Page labels with custom numbering styles" << std::endl;
  std::cout << "  ✓ Roman numeral conversion (uppercase/lowercase)" << std::endl;
  std::cout << "  ✓ Letter-based page numbering" << std::endl;
  std::cout << "  ✓ Named destinations for internal links" << std::endl;
  std::cout << "  ✓ Document information dictionary" << std::endl;
  std::cout << "  ✓ Custom metadata fields" << std::endl;
  std::cout << "  ✓ XMP metadata parsing" << std::endl;
  std::cout << "  ✓ Integration with DocumentCatalog" << std::endl;

  return 0;
}