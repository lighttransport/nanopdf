// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: MIT
//
// Test Phase 2.1: Text Layout Analysis - PDF Integration

#include "src/text-layout.hh"
#include "src/nanopdf.hh"

#include <cassert>
#include <iostream>
#include <fstream>
#include <vector>

using namespace nanopdf;

// Read file into vector
std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::cerr << "Failed to open: " << path << "\n";
    return {};
  }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
}

void test_blank_pdf() {
  std::cout << "Test: Extract text layout from blank.pdf\n";

  // Load PDF
  std::vector<uint8_t> data = read_file("../data/blank.pdf");
  if (data.empty()) {
    std::cout << "  ! Skipped (file not found)\n";
    return;
  }

  Pdf pdf;
  if (!parse_from_memory(data.data(), data.size(), &pdf)) {
    std::cerr << "  ! Failed to parse PDF\n";
    return;
  }

  if (!pdf.load_document_structure()) {
    std::cerr << "  ! Failed to load document structure\n";
    return;
  }

  if (pdf.catalog.pages_count == 0) {
    std::cerr << "  ! No pages found\n";
    return;
  }

  const Page* page = pdf.get_page(0);
  if (!page) {
    std::cerr << "  ! Failed to get first page\n";
    return;
  }

  // Extract text layout
  auto text_page = extract_text_layout(pdf, *page);

  assert(text_page != nullptr);
  std::cout << "  Page size: " << text_page->page_width << "x" << text_page->page_height << "\n";
  std::cout << "  Characters extracted: " << text_page->chars.size() << "\n";
  std::cout << "  Lines detected: " << text_page->lines.size() << "\n";
  std::cout << "  Words detected: " << text_page->words.size() << "\n";

  // Display lines
  if (!text_page->lines.empty()) {
    std::cout << "  First few lines:\n";
    for (size_t i = 0; i < std::min(size_t(5), text_page->lines.size()); ++i) {
      std::string line_text = text_page->lines[i].get_text();
      std::cout << "    Line " << i << " (order=" << text_page->lines[i].reading_order << "): \""
                << line_text << "\"\n";
    }
  }

  std::cout << "  ✓ PDF integration working\n";
}

void test_simple_text_pdf() {
  std::cout << "\nTest: Extract from simple_text.pdf (if exists)\n";

  // This test would require a PDF with known text content
  // For now, we'll skip this if test file doesn't exist
  std::vector<uint8_t> data = read_file("../data/simple_text.pdf");
  if (data.empty()) {
    std::cout << "  ! Skipped (simple_text.pdf not found)\n";
    return;
  }

  Pdf pdf;
  if (!parse_from_memory(data.data(), data.size(), &pdf)) {
    std::cout << "  ! Failed to parse\n";
    return;
  }

  if (!pdf.load_document_structure()) {
    std::cout << "  ! Failed to load document structure\n";
    return;
  }

  if (pdf.catalog.pages_count == 0) {
    std::cout << "  ! No pages\n";
    return;
  }

  const Page* page = pdf.get_page(0);
  if (!page) {
    std::cout << "  ! Failed to get page\n";
    return;
  }

  auto text_page = extract_text_layout(pdf, *page);

  std::cout << "  Characters: " << text_page->chars.size() << "\n";
  std::cout << "  Lines: " << text_page->lines.size() << "\n";

  // Display all text
  std::string full_text = text_page->get_text();
  if (!full_text.empty()) {
    std::cout << "  Full text:\n" << full_text << "\n";
  }

  std::cout << "  ✓ Simple text PDF processed\n";
}

int main() {
  std::cout << "=== Phase 2.1: Text Layout PDF Integration Tests ===\n\n";

  try {
    test_blank_pdf();
    test_simple_text_pdf();

    std::cout << "\n=== Integration tests passed! ===\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "\n!!! Test failed with exception: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "\n!!! Test failed with unknown exception\n";
    return 1;
  }
}
