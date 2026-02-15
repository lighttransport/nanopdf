// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#pragma once

#include <string>
#include <vector>
#include <memory>

#include "text-layout.hh"

namespace nanopdf {

// Represents a single cell in a table
struct TableCell {
  std::string text;         // Text content of the cell
  int row{0};               // Row index (0-based)
  int col{0};               // Column index (0-based)
  int row_span{1};          // Number of rows this cell spans
  int col_span{1};          // Number of columns this cell spans
  double x{0.0};            // X coordinate (left edge)
  double y{0.0};            // Y coordinate (top edge)
  double width{0.0};        // Cell width
  double height{0.0};       // Cell height
  std::vector<TextChar> chars;  // Characters in this cell

  // Check if cell is empty
  bool is_empty() const { return text.empty(); }
};

// Represents a table extracted from a page
struct Table {
  std::vector<std::vector<TableCell>> cells;  // 2D array of cells [row][col]
  int rows{0};              // Number of rows
  int cols{0};              // Number of columns
  double x{0.0};            // Table bounding box X
  double y{0.0};            // Table bounding box Y
  double width{0.0};        // Table bounding box width
  double height{0.0};       // Table bounding box height

  // Get cell at position (returns nullptr if out of bounds)
  const TableCell* get_cell(int row, int col) const;
  TableCell* get_cell(int row, int col);

  // Export to different formats
  std::string to_csv() const;
  std::string to_html() const;
  std::string to_json() const;
  std::string to_markdown() const;

  // Get all text from table (row by row)
  std::string get_text() const;
};

// Configuration for table detection
struct TableExtractionConfig {
  // Alignment tolerance for detecting grid lines (in points)
  double alignment_tolerance{2.0};

  // Minimum number of rows to consider as a table
  int min_rows{2};

  // Minimum number of columns to consider as a table
  int min_cols{2};

  // Maximum gap between cells in same row/column (in points)
  double max_cell_gap{50.0};

  // Minimum characters per cell to not be considered empty
  int min_chars_per_cell{1};

  // Enable debug output
  bool debug{false};
};

// Main API: Extract tables from a text page
std::vector<Table> extract_tables(
    const TextPage& text_page,
    const TableExtractionConfig& config = TableExtractionConfig());

// Helper: Detect grid structure from text layout
struct GridStructure {
  std::vector<double> row_positions;    // Y positions of row boundaries
  std::vector<double> col_positions;    // X positions of column boundaries

  // Check if grid has enough rows/cols to be a table
  bool is_valid_table(int min_rows, int min_cols) const {
    return (row_positions.size() >= static_cast<size_t>(min_rows + 1)) &&
           (col_positions.size() >= static_cast<size_t>(min_cols + 1));
  }
};

// Detect grid structure from text lines
GridStructure detect_grid_structure(
    const TextPage& text_page,
    const TableExtractionConfig& config);

// Assign characters to grid cells
std::vector<std::vector<std::vector<TextChar>>> assign_chars_to_grid(
    const TextPage& text_page,
    const GridStructure& grid,
    const TableExtractionConfig& config);

// Build table from grid and assigned characters
Table build_table_from_grid(
    const GridStructure& grid,
    const std::vector<std::vector<std::vector<TextChar>>>& cell_chars);

}  // namespace nanopdf
