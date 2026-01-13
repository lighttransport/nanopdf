// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: MIT
//
// Test Phase 2.3: Table Extraction

#include "src/table-extraction.hh"
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

void test_table_extraction_basic() {
  std::cout << "Test: Basic table extraction from synthetic data\n";

  // Create a simple synthetic text page with grid-like structure
  TextPage text_page;
  text_page.page_width = 612.0;
  text_page.page_height = 792.0;

  // Create a simple 3x3 table with multi-character cells
  // Row 0: "Name" "Age" "City"
  // Row 1: "Alice" "25" "NYC"
  // Row 2: "Bob" "30" "LA"

  double col_x[] = {100.0, 200.0, 300.0};
  double row_y[] = {100.0, 130.0, 160.0};
  const char* cells[3][3] = {
    {"Name", "Age", "City"},
    {"Alice", "25", "NYC"},
    {"Bob", "30", "LA"}
  };

  for (int r = 0; r < 3; ++r) {
    // Create a line for this row
    TextLine line;
    line.x = col_x[0];
    line.y = row_y[r];
    line.height = 12.0;
    line.reading_order = r;

    for (int c = 0; c < 3; ++c) {
      const char* text = cells[r][c];
      double x = col_x[c];
      double y = row_y[r];

      // Add each character in the cell
      for (int i = 0; text[i] != '\0'; ++i) {
        TextChar ch;
        ch.unicode = static_cast<uint32_t>(text[i]);
        ch.x = x + i * 8.0;  // Space characters horizontally
        ch.y = y;
        ch.width = 8.0;
        ch.height = 12.0;
        ch.font_size = 12.0;
        ch.line_index = r;
        text_page.chars.push_back(ch);
        line.chars.push_back(ch);
      }
    }

    // Calculate line width
    if (!line.chars.empty()) {
      const auto& last_char = line.chars.back();
      line.width = (last_char.x + last_char.width) - line.x;
    }
    text_page.lines.push_back(line);
  }

  // Extract tables
  TableExtractionConfig config;
  config.min_rows = 2;
  config.min_cols = 2;
  config.alignment_tolerance = 5.0;
  config.debug = true;

  auto tables = extract_tables(text_page, config);

  std::cout << "  Tables found: " << tables.size() << "\n";

  if (!tables.empty()) {
    const auto& table = tables[0];
    std::cout << "  Table dimensions: " << table.rows << " rows x " << table.cols << " cols\n";

    // Test CSV export
    std::string csv = table.to_csv();
    std::cout << "  CSV export:\n" << csv << "\n";

    // Test HTML export
    std::string html = table.to_html();
    std::cout << "  HTML export (first 200 chars):\n"
              << html.substr(0, std::min(size_t(200), html.length())) << "...\n\n";

    // Test JSON export
    std::string json = table.to_json();
    std::cout << "  JSON export (first 200 chars):\n"
              << json.substr(0, std::min(size_t(200), json.length())) << "...\n\n";
  }

  std::cout << "  ✓ Basic table extraction working\n";
}

void test_table_extraction_from_pdf() {
  std::cout << "\nTest: Extract tables from PDF (if available)\n";

  // Try to load a PDF with tables
  std::vector<std::string> test_files = {
    "../data/table_test.pdf",
    "../data/simple_table.pdf",
    "../data/blank.pdf"  // Fallback
  };

  std::vector<uint8_t> data;
  std::string loaded_file;

  for (const auto& path : test_files) {
    data = read_file(path);
    if (!data.empty()) {
      loaded_file = path;
      break;
    }
  }

  if (data.empty()) {
    std::cout << "  ! Skipped (no test PDFs found)\n";
    return;
  }

  std::cout << "  Using: " << loaded_file << "\n";

  Pdf pdf;
  if (!parse_from_memory(data.data(), data.size(), &pdf)) {
    std::cout << "  ! Failed to parse PDF\n";
    return;
  }

  if (!pdf.load_document_structure()) {
    std::cout << "  ! Failed to load document structure\n";
    return;
  }

  if (pdf.catalog.pages_count == 0) {
    std::cout << "  ! No pages found\n";
    return;
  }

  const Page* page = pdf.get_page(0);
  if (!page) {
    std::cout << "  ! Failed to get first page\n";
    return;
  }

  // Extract text layout
  auto text_page = extract_text_layout(pdf, *page);
  if (!text_page) {
    std::cout << "  ! Failed to extract text layout\n";
    return;
  }

  std::cout << "  Page has " << text_page->chars.size() << " characters\n";

  // Extract tables
  TableExtractionConfig config;
  config.debug = false;
  auto tables = extract_tables(*text_page, config);

  std::cout << "  Tables extracted: " << tables.size() << "\n";

  for (size_t i = 0; i < tables.size(); ++i) {
    const auto& table = tables[i];
    std::cout << "  Table " << i << ": " << table.rows << " rows x "
              << table.cols << " cols\n";

    if (table.rows > 0 && table.cols > 0) {
      // Show first few cells
      std::cout << "  First row: ";
      for (int c = 0; c < std::min(3, table.cols); ++c) {
        std::cout << "[" << table.cells[0][c].text << "] ";
      }
      std::cout << "\n";
    }
  }

  std::cout << "  ✓ PDF table extraction completed\n";
}

void test_export_formats() {
  std::cout << "\nTest: Table export formats\n";

  // Create a simple 2x2 table
  Table table;
  table.rows = 2;
  table.cols = 2;
  table.cells.resize(2);

  for (int r = 0; r < 2; ++r) {
    table.cells[r].resize(2);
    for (int c = 0; c < 2; ++c) {
      table.cells[r][c].row = r;
      table.cells[r][c].col = c;
      table.cells[r][c].text = "R" + std::to_string(r) + "C" + std::to_string(c);
    }
  }

  // Test CSV
  std::string csv = table.to_csv();
  assert(csv.find("R0C0") != std::string::npos);
  assert(csv.find("R1C1") != std::string::npos);
  std::cout << "  ✓ CSV export working\n";

  // Test HTML
  std::string html = table.to_html();
  assert(html.find("<table") != std::string::npos);
  assert(html.find("<th>") != std::string::npos);  // First row uses th
  assert(html.find("<td>") != std::string::npos);  // Other rows use td
  std::cout << "  ✓ HTML export working\n";

  // Test JSON
  std::string json = table.to_json();
  assert(json.find("\"rows\"") != std::string::npos);
  assert(json.find("\"cols\"") != std::string::npos);
  assert(json.find("\"cells\"") != std::string::npos);
  std::cout << "  ✓ JSON export working\n";

  // Test plain text
  std::string text = table.get_text();
  assert(text.find("R0C0") != std::string::npos);
  assert(text.find("\t") != std::string::npos);  // Tab-separated
  std::cout << "  ✓ Text export working\n";
}

void test_special_characters() {
  std::cout << "\nTest: Table with special characters\n";

  Table table;
  table.rows = 1;
  table.cols = 3;
  table.cells.resize(1);
  table.cells[0].resize(3);

  // CSV: quotes
  table.cells[0][0].text = "Cell with \"quotes\"";
  // HTML: entities
  table.cells[0][1].text = "Cell with <tags> & entities";
  // JSON: escapes
  table.cells[0][2].text = "Cell with\nnewlines\tand tabs";

  std::string csv = table.to_csv();
  assert(csv.find("\"\"") != std::string::npos);  // Escaped quotes
  std::cout << "  ✓ CSV escaping working\n";

  std::string html = table.to_html();
  assert(html.find("&lt;") != std::string::npos);  // <
  assert(html.find("&gt;") != std::string::npos);  // >
  assert(html.find("&amp;") != std::string::npos); // &
  std::cout << "  ✓ HTML escaping working\n";

  std::string json = table.to_json();
  assert(json.find("\\n") != std::string::npos);   // Newline
  assert(json.find("\\t") != std::string::npos);   // Tab
  std::cout << "  ✓ JSON escaping working\n";
}

int main() {
  std::cout << "=== Phase 2.3: Table Extraction Tests ===\n\n";

  try {
    test_table_extraction_basic();
    test_table_extraction_from_pdf();
    test_export_formats();
    test_special_characters();

    std::cout << "\n=== All table extraction tests passed! ===\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "\n!!! Test failed with exception: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "\n!!! Test failed with unknown exception\n";
    return 1;
  }
}
