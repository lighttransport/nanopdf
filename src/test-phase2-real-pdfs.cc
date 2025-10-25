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

  auto find_body_offset = [&](uint32_t obj) -> uint64_t {
    std::string token = std::to_string(obj) + " 0 obj";
    std::string pdf_text(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    size_t pos = pdf_text.find(token);
    while (pos != std::string::npos) {
      bool boundary_before = pos == 0 || std::isspace(static_cast<unsigned char>(pdf_text[pos - 1]));
      size_t after = pos + token.size();
      bool boundary_after = after >= pdf_text.size() || std::isspace(static_cast<unsigned char>(pdf_text[after]));
      if (boundary_before && boundary_after) {
        size_t body = after;
        while (body < buffer.size() && std::isspace(static_cast<unsigned char>(buffer[body]))) {
          ++body;
        }
        return static_cast<uint64_t>(body);
      }
      pos = pdf_text.find(token, pos + 1);
    }
    return 0;
  };

  pdf.object_offsets.clear();
  for (uint32_t obj = 1; obj <= 5; ++obj) {
    uint64_t body = find_body_offset(obj);
    if (body == 0) {
      std::cerr << "Failed to locate object " << obj << " in sample PDF" << std::endl;
      std::exit(1);
    }
    uint64_t key = (static_cast<uint64_t>(obj) << 32);
    pdf.object_offsets[key] = body;
  }
  pdf.object_offsets_built = true;
  pdf.object_offsets_failed = false;
  pdf.size = 6;
  pdf.root = 1;

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
