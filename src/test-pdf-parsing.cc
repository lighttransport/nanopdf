#include "nanopdf.hh"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
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

// Test: Parse blank.pdf and verify basic structure
void test_parse_blank_pdf() {
  std::cout << "Testing blank.pdf parsing..." << std::endl;

  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  assert(read_file(path, buf));

  Pdf pdf;
  bool ok = parse_from_memory(buf.data(), buf.size(), &pdf);
  assert(ok);

  bool loaded = pdf.load_document_structure();
  assert(loaded);

  // blank.pdf should have at least 1 page
  assert(pdf.catalog.pages_count >= 1);
  assert(pdf.catalog.pages.size() >= 1);

  // First page should have a valid media box
  const Page& page = pdf.catalog.pages[0];
  assert(page.media_box.size() == 4);
  assert(page.media_box[2] > page.media_box[0]);  // width > 0
  assert(page.media_box[3] > page.media_box[1]);  // height > 0

  std::cout << "  Pages: " << pdf.catalog.pages_count << std::endl;
  std::cout << "  MediaBox: [" << page.media_box[0] << ", " << page.media_box[1]
            << ", " << page.media_box[2] << ", " << page.media_box[3] << "]"
            << std::endl;
  std::cout << "  blank.pdf: PASSED" << std::endl;
}

// Test: Parse all test PDFs without crashing
void test_parse_all_test_pdfs() {
  std::cout << "Testing parsing of all test PDFs..." << std::endl;

  const char* test_pdfs[] = {
      "blank.pdf",       "test_clip.pdf",    "test_dash.pdf",
      "test_gradient.pdf", "test_graphics.pdf", "test_image.pdf",
      "test_multistop.pdf", "test_pattern.pdf", "test_radial.pdf",
      "test_textmode.pdf", "test_textmode2.pdf",
  };

  int passed = 0;
  int skipped = 0;

  for (const char* name : test_pdfs) {
    std::string path = data_dir() + "/" + name;
    std::vector<uint8_t> buf;
    if (!read_file(path, buf)) {
      std::cout << "  " << name << ": SKIPPED (file not found)" << std::endl;
      skipped++;
      continue;
    }

    Pdf pdf;
    bool ok = parse_from_memory(buf.data(), buf.size(), &pdf);
    assert(ok);

    bool loaded = pdf.load_document_structure();
    assert(loaded);
    assert(pdf.catalog.pages_count >= 1);
    assert(pdf.catalog.pages.size() >= 1);

    // Every page should have a media box
    for (uint32_t i = 0; i < pdf.catalog.pages_count; i++) {
      assert(pdf.catalog.pages[i].media_box.size() == 4);
    }

    std::cout << "  " << name << " (" << pdf.catalog.pages_count << " pages): PASSED"
              << std::endl;
    passed++;
  }

  assert(passed > 0);  // At least blank.pdf must exist
  std::cout << "All test PDFs parsed: " << passed << " passed, " << skipped
            << " skipped" << std::endl << std::endl;
}

// Test: Page content streams can be loaded
void test_page_content_loading() {
  std::cout << "Testing page content stream loading..." << std::endl;

  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  assert(read_file(path, buf));

  Pdf pdf;
  assert(parse_from_memory(buf.data(), buf.size(), &pdf));
  assert(pdf.load_document_structure());

  // Load content for the first page
  const Page& page = pdf.catalog.pages[0];
  PageContent content = page.load_contents(pdf);

  // Content should be loadable (may be empty for blank page)
  std::cout << "  Content size: " << content.data.size() << " bytes" << std::endl;
  std::cout << "  Page content loading: PASSED" << std::endl << std::endl;
}

// Test: Malformed/truncated data should not crash
void test_malformed_input() {
  std::cout << "Testing malformed input handling..." << std::endl;

  // Test case 1: Empty data
  {
    Pdf pdf;
    bool ok = parse_from_memory(nullptr, 0, &pdf);
    assert(!ok);
    std::cout << "  Test 1 (empty data): PASSED" << std::endl;
  }

  // Test case 2: Not a PDF (random bytes)
  {
    uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03};
    Pdf pdf;
    bool ok = parse_from_memory(garbage, sizeof(garbage), &pdf);
    assert(!ok);
    std::cout << "  Test 2 (random bytes): PASSED" << std::endl;
  }

  // Test case 3: PDF header only, no content
  {
    const char* header = "%PDF-1.4\n";
    Pdf pdf;
    bool ok = parse_from_memory(reinterpret_cast<const uint8_t*>(header),
                                strlen(header), &pdf);
    assert(!ok);
    std::cout << "  Test 3 (header only): PASSED" << std::endl;
  }

  // Test case 4: Truncated PDF (first 32 bytes of a real PDF)
  {
    std::vector<uint8_t> buf;
    std::string path = data_dir() + "/blank.pdf";
    if (read_file(path, buf) && buf.size() > 32) {
      Pdf pdf;
      bool ok = parse_from_memory(buf.data(), 32, &pdf);
      assert(!ok);
      std::cout << "  Test 4 (truncated PDF): PASSED" << std::endl;
    } else {
      std::cout << "  Test 4 (truncated PDF): SKIPPED" << std::endl;
    }
  }

  std::cout << "Malformed input tests completed successfully!" << std::endl
            << std::endl;
}

// Test: resolve_reference with invalid object numbers
void test_invalid_references() {
  std::cout << "Testing invalid reference handling..." << std::endl;

  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  assert(read_file(path, buf));

  Pdf pdf;
  assert(parse_from_memory(buf.data(), buf.size(), &pdf));

  // Try to resolve a non-existent object
  ResolvedObject result = resolve_reference(pdf, 99999, 0);
  assert(!result.success);
  std::cout << "  Invalid object number: PASSED" << std::endl;

  std::cout << "Invalid reference tests completed successfully!" << std::endl
            << std::endl;
}

int main() {
  std::cout << "=== End-to-End PDF Parsing Tests ===" << std::endl << std::endl;

  test_parse_blank_pdf();
  test_parse_all_test_pdfs();
  test_page_content_loading();
  test_malformed_input();
  test_invalid_references();

  std::cout << "=== All PDF parsing tests passed! ===" << std::endl;
  return 0;
}
