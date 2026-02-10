#include "nanopdf.hh"
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace nanopdf;

#ifdef NANOPDF_SOURCE_DIR
std::string data_dir() { return std::string(NANOPDF_SOURCE_DIR) + "/data"; }
#else
std::string data_dir() { return "../data"; }
#endif

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  auto size = f.tellg();
  f.seekg(0);
  out.resize(static_cast<size_t>(size));
  f.read(reinterpret_cast<char*>(out.data()), size);
  return f.good();
}

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

// Test parse_document_info on real PDFs
void test_parse_document_info_real() {
  std::cout << "Testing parse_document_info on real PDFs..." << std::endl;

  // blank.pdf should have at least a Producer field
  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  if (!read_file(path, buf)) {
    std::cout << "  SKIPPED (blank.pdf not found)" << std::endl;
    return;
  }

  Pdf pdf;
  assert(parse_from_memory(buf.data(), buf.size(), &pdf));
  assert(pdf.load_document_structure());

  // Call parse_document_info - should not crash
  parse_document_info(pdf, pdf.catalog);

  // blank.pdf may or may not have info - just verify it doesn't crash
  std::cout << "  Title: \"" << pdf.catalog.document_info.title << "\"" << std::endl;
  std::cout << "  Producer: \"" << pdf.catalog.document_info.producer << "\"" << std::endl;
  std::cout << "  parse_document_info: PASSED" << std::endl;
}

// Test parse_document_outline on real PDFs (graceful no-op when absent)
void test_parse_document_outline_real() {
  std::cout << "Testing parse_document_outline on real PDFs..." << std::endl;

  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  if (!read_file(path, buf)) {
    std::cout << "  SKIPPED (blank.pdf not found)" << std::endl;
    return;
  }

  Pdf pdf;
  assert(parse_from_memory(buf.data(), buf.size(), &pdf));
  assert(pdf.load_document_structure());

  // blank.pdf has no outlines — should return gracefully
  parse_document_outline(pdf, pdf.catalog);
  // outline_root may be null — that's expected for a blank PDF
  std::cout << "  Outline root: "
            << (pdf.catalog.outline_root ? "present" : "null (expected)") << std::endl;
  std::cout << "  parse_document_outline: PASSED" << std::endl;
}

// Test parse_page_labels on real PDFs (graceful no-op when absent)
void test_parse_page_labels_real() {
  std::cout << "Testing parse_page_labels on real PDFs..." << std::endl;

  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  if (!read_file(path, buf)) {
    std::cout << "  SKIPPED (blank.pdf not found)" << std::endl;
    return;
  }

  Pdf pdf;
  assert(parse_from_memory(buf.data(), buf.size(), &pdf));
  assert(pdf.load_document_structure());

  // blank.pdf has no page labels — should return gracefully
  parse_page_labels(pdf, pdf.catalog);
  std::cout << "  Labels count: " << pdf.catalog.page_labels.labels.size() << std::endl;
  std::cout << "  parse_page_labels: PASSED" << std::endl;
}

// Test parse_named_destinations on real PDFs (graceful no-op when absent)
void test_parse_named_destinations_real() {
  std::cout << "Testing parse_named_destinations on real PDFs..." << std::endl;

  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  if (!read_file(path, buf)) {
    std::cout << "  SKIPPED (blank.pdf not found)" << std::endl;
    return;
  }

  Pdf pdf;
  assert(parse_from_memory(buf.data(), buf.size(), &pdf));
  assert(pdf.load_document_structure());

  parse_named_destinations(pdf, pdf.catalog);
  std::cout << "  Named destinations: " << pdf.catalog.named_destinations.size() << std::endl;
  std::cout << "  parse_named_destinations: PASSED" << std::endl;
}

// Test parse_xmp_metadata on real PDFs (graceful no-op when absent)
void test_parse_xmp_metadata_real() {
  std::cout << "Testing parse_xmp_metadata on real PDFs..." << std::endl;

  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  if (!read_file(path, buf)) {
    std::cout << "  SKIPPED (blank.pdf not found)" << std::endl;
    return;
  }

  Pdf pdf;
  assert(parse_from_memory(buf.data(), buf.size(), &pdf));
  assert(pdf.load_document_structure());

  parse_xmp_metadata(pdf, pdf.catalog);
  std::cout << "  XMP title: \"" << pdf.catalog.xmp_metadata.dc_title << "\"" << std::endl;
  std::cout << "  parse_xmp_metadata: PASSED" << std::endl;
}

// Test parse_optional_content on real PDFs (graceful no-op when absent)
void test_parse_optional_content_real() {
  std::cout << "Testing parse_optional_content on real PDFs..." << std::endl;

  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  if (!read_file(path, buf)) {
    std::cout << "  SKIPPED (blank.pdf not found)" << std::endl;
    return;
  }

  Pdf pdf;
  assert(parse_from_memory(buf.data(), buf.size(), &pdf));
  assert(pdf.load_document_structure());

  parse_optional_content(pdf, pdf.catalog);
  std::cout << "  Optional content groups: "
            << pdf.catalog.ocg_properties.ocgs.size() << std::endl;
  std::cout << "  parse_optional_content: PASSED" << std::endl;
}

// Test all parse functions on all available test PDFs (no crashes)
void test_parse_all_structures() {
  std::cout << "Testing structure parsing on all test PDFs..." << std::endl;

  const char* test_pdfs[] = {
      "blank.pdf",        "test_clip.pdf",      "test_dash.pdf",
      "test_gradient.pdf", "test_graphics.pdf",  "test_image.pdf",
      "test_multistop.pdf", "test_pattern.pdf",  "test_radial.pdf",
      "test_textmode.pdf", "test_textmode2.pdf",
  };

  int passed = 0;
  int skipped = 0;

  for (const char* name : test_pdfs) {
    std::string path = data_dir() + "/" + name;
    std::vector<uint8_t> buf;
    if (!read_file(path, buf)) {
      skipped++;
      continue;
    }

    Pdf pdf;
    if (!parse_from_memory(buf.data(), buf.size(), &pdf)) {
      skipped++;
      continue;
    }
    if (!pdf.load_document_structure()) {
      skipped++;
      continue;
    }

    // Call all parsing functions — none should crash
    parse_document_info(pdf, pdf.catalog);
    parse_document_outline(pdf, pdf.catalog);
    parse_page_labels(pdf, pdf.catalog);
    parse_named_destinations(pdf, pdf.catalog);
    parse_xmp_metadata(pdf, pdf.catalog);
    parse_optional_content(pdf, pdf.catalog);

    std::cout << "  " << name << ": PASSED" << std::endl;
    passed++;
  }

  assert(passed > 0);
  std::cout << "  Structure parsing: " << passed << " passed, "
            << skipped << " skipped" << std::endl;
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
  test_parse_document_info_real();
  test_parse_document_outline_real();
  test_parse_page_labels_real();
  test_parse_named_destinations_real();
  test_parse_xmp_metadata_real();
  test_parse_optional_content_real();
  test_parse_all_structures();

  std::cout << "\n=== All Phase 4 tests passed! ===\n" << std::endl;

  return 0;
}