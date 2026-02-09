#include "nanopdf.hh"
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace nanopdf;

// Always-active assertion (not disabled by NDEBUG)
#define REQUIRE(expr)                                                    \
  do {                                                                   \
    if (!(expr)) {                                                       \
      std::cerr << "FAIL: " #expr " at " __FILE__ ":" << __LINE__       \
                << std::endl;                                            \
      return false;                                                      \
    }                                                                    \
  } while (0)

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
bool test_parse_blank_pdf() {
  std::cout << "Testing blank.pdf parsing..." << std::endl;

  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  REQUIRE(read_file(path, buf));

  Pdf pdf;
  REQUIRE(parse_from_memory(buf.data(), buf.size(), &pdf));
  REQUIRE(pdf.load_document_structure());
  REQUIRE(pdf.catalog.pages_count >= 1);
  REQUIRE(pdf.catalog.pages.size() >= 1);

  const Page& page = pdf.catalog.pages[0];
  REQUIRE(page.media_box.size() == 4);
  REQUIRE(page.media_box[2] > page.media_box[0]);
  REQUIRE(page.media_box[3] > page.media_box[1]);

  std::cout << "  Pages: " << pdf.catalog.pages_count << std::endl;
  std::cout << "  MediaBox: [" << page.media_box[0] << ", " << page.media_box[1]
            << ", " << page.media_box[2] << ", " << page.media_box[3] << "]"
            << std::endl;
  std::cout << "  blank.pdf: PASSED" << std::endl;
  return true;
}

// Test: Parse all test PDFs without crashing
bool test_parse_all_test_pdfs() {
  std::cout << "Testing parsing of all test PDFs..." << std::endl;

  const char* test_pdfs[] = {
      "blank.pdf",        "test_clip.pdf",     "test_dash.pdf",
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
    REQUIRE(parse_from_memory(buf.data(), buf.size(), &pdf));
    REQUIRE(pdf.load_document_structure());
    REQUIRE(pdf.catalog.pages_count >= 1);
    REQUIRE(pdf.catalog.pages.size() >= 1);

    for (uint32_t i = 0; i < pdf.catalog.pages_count; i++) {
      REQUIRE(pdf.catalog.pages[i].media_box.size() == 4);
    }

    std::cout << "  " << name << " (" << pdf.catalog.pages_count
              << " pages): PASSED" << std::endl;
    passed++;
  }

  REQUIRE(passed > 0);
  std::cout << "All test PDFs parsed: " << passed << " passed, " << skipped
            << " skipped" << std::endl << std::endl;
  return true;
}

// Test: Page content streams can be loaded
bool test_page_content_loading() {
  std::cout << "Testing page content stream loading..." << std::endl;

  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  REQUIRE(read_file(path, buf));

  Pdf pdf;
  REQUIRE(parse_from_memory(buf.data(), buf.size(), &pdf));
  REQUIRE(pdf.load_document_structure());
  REQUIRE(pdf.catalog.pages.size() >= 1);

  const Page& page = pdf.catalog.pages[0];
  PageContent content = page.load_contents(pdf);

  std::cout << "  Content size: " << content.data.size() << " bytes" << std::endl;
  std::cout << "  Page content loading: PASSED" << std::endl << std::endl;
  return true;
}

// Test: Malformed/truncated data should not crash
bool test_malformed_input() {
  std::cout << "Testing malformed input handling..." << std::endl;

  // Test case 1: Empty data
  {
    Pdf pdf;
    REQUIRE(!parse_from_memory(nullptr, 0, &pdf));
    std::cout << "  Test 1 (empty data): PASSED" << std::endl;
  }

  // Test case 2: Not a PDF (random bytes)
  {
    uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03};
    Pdf pdf;
    REQUIRE(!parse_from_memory(garbage, sizeof(garbage), &pdf));
    std::cout << "  Test 2 (random bytes): PASSED" << std::endl;
  }

  // Test case 3: PDF header only, no content
  {
    const char* header = "%PDF-1.4\n";
    Pdf pdf;
    REQUIRE(!parse_from_memory(reinterpret_cast<const uint8_t*>(header),
                               strlen(header), &pdf));
    std::cout << "  Test 3 (header only): PASSED" << std::endl;
  }

  // Test case 4: Truncated PDF (first 32 bytes of a real PDF)
  {
    std::vector<uint8_t> buf;
    std::string path = data_dir() + "/blank.pdf";
    if (read_file(path, buf) && buf.size() > 32) {
      Pdf pdf;
      REQUIRE(!parse_from_memory(buf.data(), 32, &pdf));
      std::cout << "  Test 4 (truncated PDF): PASSED" << std::endl;
    } else {
      std::cout << "  Test 4 (truncated PDF): SKIPPED" << std::endl;
    }
  }

  std::cout << "Malformed input tests completed successfully!" << std::endl
            << std::endl;
  return true;
}

// Test: resolve_reference with invalid object numbers
bool test_invalid_references() {
  std::cout << "Testing invalid reference handling..." << std::endl;

  std::vector<uint8_t> buf;
  std::string path = data_dir() + "/blank.pdf";
  REQUIRE(read_file(path, buf));

  Pdf pdf;
  REQUIRE(parse_from_memory(buf.data(), buf.size(), &pdf));

  ResolvedObject result = resolve_reference(pdf, 99999, 0);
  REQUIRE(!result.success);
  std::cout << "  Invalid object number: PASSED" << std::endl;

  std::cout << "Invalid reference tests completed successfully!" << std::endl
            << std::endl;
  return true;
}

int main() {
  std::cout << "=== End-to-End PDF Parsing Tests ===" << std::endl << std::endl;

  int failures = 0;
  if (!test_parse_blank_pdf()) failures++;
  if (!test_parse_all_test_pdfs()) failures++;
  if (!test_page_content_loading()) failures++;
  if (!test_malformed_input()) failures++;
  if (!test_invalid_references()) failures++;

  if (failures > 0) {
    std::cerr << failures << " test(s) FAILED" << std::endl;
    return 1;
  }

  std::cout << "=== All PDF parsing tests passed! ===" << std::endl;
  return 0;
}
