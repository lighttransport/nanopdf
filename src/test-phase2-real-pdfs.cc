#include "nanopdf.hh"

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace nanopdf;

namespace {

std::vector<uint8_t> read_binary(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  std::vector<uint8_t> buffer;
  if (!ifs) {
    return buffer;
  }

  ifs.seekg(0, std::ios::end);
  std::streamoff size = ifs.tellg();
  if (size <= 0) {
    return buffer;
  }
  buffer.resize(static_cast<size_t>(size));
  ifs.seekg(0, std::ios::beg);
  ifs.read(reinterpret_cast<char*>(buffer.data()), size);
  if (!ifs) {
    buffer.clear();
  }
  return buffer;
}

std::string data_directory() {
#ifdef NANOPDF_SOURCE_DIR
  return std::string(NANOPDF_SOURCE_DIR) + "/data/standardencoding";
#else
  return "../data/standardencoding";
#endif
}

}  // namespace

void test_pdfa_font_encoding_extracts_accented_text() {
  std::string path = data_directory() + "/sample_standardencoding.pdf";
  std::vector<uint8_t> buffer = read_binary(path);
  if (buffer.empty()) {
    std::cerr << "Failed to read sample PDF: " << path << std::endl;
    std::exit(1);
  }

  Pdf pdf;
  pdf.version_major = 1;
  pdf.version_minor = 3;
  pdf.data = buffer.data();
  pdf.data_size = buffer.size();
  if (!pdf.build_object_offset_cache()) {
    std::cerr << "Failed to build object offset cache" << std::endl;
    std::exit(1);
  }
  std::cout << "  Catalog root: " << pdf.root << " Size: " << pdf.size << std::endl;

  if (!pdf.load_document_structure()) {
    std::cerr << "Failed to load document structure for sample PDF" << std::endl;
    std::exit(1);
  }
  if (pdf.catalog.pages.empty()) {
    std::cerr << "Parsed PDF has no pages" << std::endl;
    std::exit(1);
  }

  const Page& page = pdf.catalog.pages.front();
  std::string text = extract_text_from_page(pdf, page);

  // Expect German sharp-s accented words to survive extraction.
  assert(text.find(u8"Straße") != std::string::npos);
  assert(text.find(u8"grüß") != std::string::npos);
  assert(text.find(u8"façade") != std::string::npos);

  // Ensure at least one font resource carries Differences overrides.
  bool has_differences = false;
  for (const auto& entry : page.fonts) {
    const BaseFont* font = entry.second.get();
    if (font && !font->encoding_differences.empty()) {
      has_differences = true;
      break;
    }
  }
  assert(has_differences);

  std::cout << "  Real PDF text extraction: PASS" << std::endl;
}

int main() {
  std::cout << "=== Phase 2 Real PDF Tests ===" << std::endl;
  test_pdfa_font_encoding_extracts_accented_text();
  std::cout << "All real PDF tests passed!" << std::endl;
  return 0;
}
