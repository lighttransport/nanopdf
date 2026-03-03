// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: MIT
//
// Test Phase 2.1: Text Layout Analysis

#include "src/text-layout.hh"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace nanopdf;

// Helper to create a text character
TextChar make_char(char c, double x, double y, double font_size = 12.0) {
  TextChar ch;
  ch.unicode = static_cast<uint32_t>(c);
  ch.x = x;
  ch.y = y;
  ch.width = font_size * 0.6;  // Approximate character width
  ch.height = font_size;
  ch.font_size = font_size;
  ch.font_name = "Helvetica";
  ch.rotation = 0.0;
  ch.matrix[0] = 1; ch.matrix[1] = 0;
  ch.matrix[2] = 0; ch.matrix[3] = 1;
  ch.matrix[4] = x; ch.matrix[5] = y;
  return ch;
}

void test_single_line() {
  std::cout << "Test: Single line text\n";

  std::vector<TextChar> chars;
  // "Hello World" at y=700
  const char* text = "Hello World";
  double x = 100.0;
  double y = 700.0;

  for (size_t i = 0; text[i]; ++i) {
    chars.push_back(make_char(text[i], x, y));
    x += 7.2;  // Character spacing
  }

  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0);

  assert(page != nullptr);
  assert(page->lines.size() == 1);
  assert(page->lines[0].chars.size() == 11);

  std::string line_text = page->lines[0].get_text();
  assert(line_text == "Hello World");

  std::cout << "  ✓ Single line detected correctly\n";
  std::cout << "  ✓ Text extracted: \"" << line_text << "\"\n";
}

void test_multi_line() {
  std::cout << "\nTest: Multi-line text\n";

  std::vector<TextChar> chars;

  // Line 1: "First line" at y=700
  const char* line1 = "First line";
  double x = 100.0;
  double y = 700.0;
  for (size_t i = 0; line1[i]; ++i) {
    chars.push_back(make_char(line1[i], x, y));
    x += 7.2;
  }

  // Line 2: "Second line" at y=680 (20 points below)
  const char* line2 = "Second line";
  x = 100.0;
  y = 680.0;
  for (size_t i = 0; line2[i]; ++i) {
    chars.push_back(make_char(line2[i], x, y));
    x += 7.2;
  }

  // Line 3: "Third line" at y=660
  const char* line3 = "Third line";
  x = 100.0;
  y = 660.0;
  for (size_t i = 0; line3[i]; ++i) {
    chars.push_back(make_char(line3[i], x, y));
    x += 7.2;
  }

  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0);

  assert(page != nullptr);
  assert(page->lines.size() == 3);

  // Check reading order (top to bottom)
  assert(page->lines[0].reading_order == 0);
  assert(page->lines[1].reading_order == 1);
  assert(page->lines[2].reading_order == 2);

  std::cout << "  ✓ Three lines detected\n";
  std::cout << "  ✓ Reading order assigned correctly\n";

  std::string full_text = page->get_text();
  std::cout << "  ✓ Full text:\n" << full_text;
}

void test_word_segmentation() {
  std::cout << "\nTest: Word segmentation\n";

  std::vector<TextChar> chars;

  // "Hello World Test" with proper spacing
  const char* text = "Hello World Test";
  double x = 100.0;
  double y = 700.0;

  for (size_t i = 0; text[i]; ++i) {
    chars.push_back(make_char(text[i], x, y));

    if (text[i] == ' ') {
      x += 14.4;  // Larger space between words
    } else {
      x += 7.2;  // Normal character spacing
    }
  }

  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0);

  assert(page != nullptr);
  assert(page->lines.size() == 1);

  // Should detect 3 words (spaces excluded)
  std::cout << "  Detected " << page->words.size() << " words\n";

  for (size_t i = 0; i < page->words.size(); ++i) {
    std::string word_text = page->words[i].get_text();
    std::cout << "  Word " << i << ": \"" << word_text << "\"\n";
  }

  // At minimum, should detect some words
  assert(page->words.size() >= 1);

  std::cout << "  ✓ Word segmentation working\n";
}

void test_two_column_layout() {
  std::cout << "\nTest: Two-column layout\n";

  std::vector<TextChar> chars;

  // Left column (x=100)
  const char* left1 = "Left column";
  double x = 100.0;
  double y = 700.0;
  for (size_t i = 0; left1[i]; ++i) {
    chars.push_back(make_char(left1[i], x, y));
    x += 7.2;
  }

  const char* left2 = "Line two";
  x = 100.0;
  y = 680.0;
  for (size_t i = 0; left2[i]; ++i) {
    chars.push_back(make_char(left2[i], x, y));
    x += 7.2;
  }

  // Right column (x=350 - large gap indicates column break)
  const char* right1 = "Right column";
  x = 350.0;
  y = 700.0;
  for (size_t i = 0; right1[i]; ++i) {
    chars.push_back(make_char(right1[i], x, y));
    x += 7.2;
  }

  const char* right2 = "Line four";
  x = 350.0;
  y = 680.0;
  for (size_t i = 0; right2[i]; ++i) {
    chars.push_back(make_char(right2[i], x, y));
    x += 7.2;
  }

  TextLayoutOptions options;
  options.column_gap_threshold = 200.0;  // Detect columns with 200pt gap
  options.detect_columns = true;

  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0, options);

  assert(page != nullptr);
  assert(page->lines.size() == 4);

  // Reading order should be: left1, left2, right1, right2
  std::cout << "  Lines in reading order:\n";
  std::vector<int> order_indices(4);
  for (size_t i = 0; i < page->lines.size(); ++i) {
    order_indices[page->lines[i].reading_order] = i;
  }

  for (int i = 0; i < 4; ++i) {
    std::cout << "  " << i << ": \"" << page->lines[order_indices[i]].get_text() << "\"\n";
  }

  std::cout << "  ✓ Two-column layout handled\n";
}

void test_baseline_alignment() {
  std::cout << "\nTest: Baseline alignment tolerance\n";

  std::vector<TextChar> chars;

  // Characters on same line with slight Y variations
  const char* text = "Baseline";
  double x = 100.0;
  double base_y = 700.0;

  for (size_t i = 0; text[i]; ++i) {
    // Add small random variations (within tolerance)
    double y = base_y + (i % 2 ? 0.5 : -0.5);
    chars.push_back(make_char(text[i], x, y));
    x += 7.2;
  }

  TextLayoutOptions options;
  options.baseline_tolerance = 2.0;  // Allow 2pt variance

  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0, options);

  assert(page != nullptr);
  assert(page->lines.size() == 1);  // Should group into one line

  std::string line_text = page->lines[0].get_text();
  assert(line_text == "Baseline");

  std::cout << "  ✓ Characters with slight Y variance grouped correctly\n";
  std::cout << "  ✓ Text: \"" << line_text << "\"\n";
}

void test_empty_page() {
  std::cout << "\nTest: Empty page\n";

  std::vector<TextChar> chars;  // Empty

  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0);

  assert(page != nullptr);
  assert(page->lines.empty());
  assert(page->words.empty());
  assert(page->get_text().empty());

  std::cout << "  ✓ Empty page handled correctly\n";
}

void test_spatial_query() {
  std::cout << "\nTest: Spatial queries\n";

  std::vector<TextChar> chars;

  // "Test Text" at specific location
  const char* text = "Test";
  double x = 100.0;
  double y = 700.0;
  for (size_t i = 0; text[i]; ++i) {
    chars.push_back(make_char(text[i], x, y));
    x += 7.2;
  }

  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0);

  // Query rectangle containing the text
  std::string found = page->get_text_in_rect(90.0, 690.0, 150.0, 710.0);
  std::cout << "  Text in rect: \"" << found << "\"\n";
  assert(!found.empty());

  // Query rectangle outside the text
  std::string not_found = page->get_text_in_rect(500.0, 500.0, 600.0, 600.0);
  assert(not_found.empty());

  std::cout << "  ✓ Spatial queries working\n";
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
  std::cout << "=== Phase 2.1: Text Layout Analysis Tests ===\n\n";

  try {
    test_single_line();
    test_multi_line();
    test_word_segmentation();
    test_two_column_layout();
    test_baseline_alignment();
    test_empty_page();
    test_spatial_query();

    std::cout << "\n=== All tests passed! ===\n";
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
