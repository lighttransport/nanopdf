// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: MIT
//
// Test Phase 4: Document Structure and Navigation

#include "src/nanopdf.hh"

#include <cassert>
#include <iostream>
#include <memory>

using namespace nanopdf;

void test_document_outline() {
  std::cout << "Test: Document outline parsing\n";

  // Create a minimal PDF structure
  Pdf pdf;
  pdf.root = 1;

  // Parse outline (should handle missing outline gracefully)
  parse_document_outline(pdf, pdf.catalog);

  std::cout << "  ✓ Handles missing outline\n";
}

void test_page_labels() {
  std::cout << "\nTest: Page labels\n";

  // Create PageLabels structure
  PageLabels labels;

  // Add label range: pages 0-9 with lowercase Roman numerals
  PageLabel range1;
  range1.style = PageLabelStyle::LowercaseRoman;
  range1.start_value = 1;
  labels.labels[0] = range1;

  // Add label range: pages 10+ with decimal Arabic
  PageLabel range2;
  range2.style = PageLabelStyle::DecimalArabic;
  range2.prefix = "Page ";
  range2.start_value = 1;
  labels.labels[10] = range2;

  // Test label generation
  std::string label0 = labels.get_label(0);
  assert(label0 == "i");
  std::cout << "  ✓ Page 0 label: " << label0 << "\n";

  std::string label5 = labels.get_label(5);
  assert(label5 == "vi");
  std::cout << "  ✓ Page 5 label: " << label5 << "\n";

  std::string label10 = labels.get_label(10);
  assert(label10 == "Page 1");
  std::cout << "  ✓ Page 10 label: " << label10 << "\n";

  std::string label15 = labels.get_label(15);
  assert(label15 == "Page 6");
  std::cout << "  ✓ Page 15 label: " << label15 << "\n";
}

void test_page_labels_parsing() {
  std::cout << "\nTest: Page labels parsing from PDF\n";

  Pdf pdf;
  pdf.root = 1;

  // Parse page labels (should handle missing labels gracefully)
  parse_page_labels(pdf, pdf.catalog);

  std::cout << "  ✓ Handles missing page labels\n";
}

void test_named_destinations() {
  std::cout << "\nTest: Named destinations\n";

  Pdf pdf;
  pdf.root = 1;

  // Parse named destinations (should handle missing gracefully)
  parse_named_destinations(pdf, pdf.catalog);

  std::cout << "  ✓ Handles missing destinations\n";
}

void test_document_info() {
  std::cout << "\nTest: Document info\n";

  Pdf pdf;
  pdf.root = 1;

  // Parse document info (will get data from trailer)
  parse_document_info(pdf, pdf.catalog);

  std::cout << "  ✓ Document info parsed\n";
}

void test_xmp_metadata() {
  std::cout << "\nTest: XMP metadata\n";

  Pdf pdf;
  pdf.root = 1;

  // Parse XMP metadata (should handle missing gracefully)
  parse_xmp_metadata(pdf, pdf.catalog);

  std::cout << "  ✓ Handles missing XMP metadata\n";
}

void test_outline_hierarchy() {
  std::cout << "\nTest: Outline hierarchy with children\n";

  // Create nested outline structure manually
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

  // Verify structure
  assert(root->title == "Part 1");
  assert(root->children.size() == 2);
  assert(root->children[0]->title == "Chapter 1");
  assert(root->children[1]->title == "Chapter 2");

  std::cout << "  ✓ Nested outline structure working\n";
  std::cout << "    Root: " << root->title << "\n";
  std::cout << "      Child 1: " << root->children[0]->title << "\n";
  std::cout << "      Child 2: " << root->children[1]->title << "\n";
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
  std::cout << "=== Phase 4: Document Structure and Navigation Tests ===\n\n";

  try {
    test_document_outline();
    test_page_labels();
    test_page_labels_parsing();
    test_named_destinations();
    test_document_info();
    test_xmp_metadata();
    test_outline_hierarchy();

    std::cout << "\n=== All Phase 4 tests passed! ===\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "\n!!! Test failed with exception: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "\n!!! Test failed with unknown exception\n";
    return 1;
  }
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
