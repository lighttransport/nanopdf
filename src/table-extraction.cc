// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "table-extraction.hh"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <set>
#include <iostream>

namespace nanopdf {

// Helper: Check if two values are approximately equal within tolerance
static bool approx_equal(double a, double b, double tolerance) {
  return std::abs(a - b) <= tolerance;
}

// Helper: Find or create position in sorted list within tolerance
static size_t find_or_add_position(std::vector<double>& positions, double pos, double tolerance) {
  for (size_t i = 0; i < positions.size(); ++i) {
    if (approx_equal(positions[i], pos, tolerance)) {
      return i;
    }
    if (positions[i] > pos + tolerance) {
      // Insert before this position
      positions.insert(positions.begin() + i, pos);
      return i;
    }
  }
  // Add at end
  positions.push_back(pos);
  return positions.size() - 1;
}

// Detect grid structure from text layout by finding gaps between character groups
GridStructure detect_grid_structure(
    const TextPage& text_page,
    const TableExtractionConfig& config) {

  GridStructure grid;

  if (text_page.lines.empty() || text_page.chars.empty()) {
    return grid;
  }

  // Step 1: Detect row boundaries from lines
  // Each line represents a table row, so use line Y positions as row boundaries
  grid.row_positions.push_back(text_page.lines.front().y);
  for (size_t i = 0; i < text_page.lines.size(); ++i) {
    const auto& line = text_page.lines[i];
    // Add bottom boundary of this line
    double y_bottom = line.y + line.height;
    grid.row_positions.push_back(y_bottom);

    // If there's a next line, add its top boundary (may create gap for empty space)
    if (i + 1 < text_page.lines.size()) {
      const auto& next_line = text_page.lines[i + 1];
      if (!approx_equal(y_bottom, next_line.y, config.alignment_tolerance)) {
        // There's a gap - use it as boundary
        grid.row_positions.push_back(next_line.y);
      }
    }
  }

  // Step 2: Detect column boundaries by finding large horizontal gaps
  // Collect all character x positions sorted
  std::vector<std::pair<double, double>> char_spans;  // (x_start, x_end) for each char
  for (const auto& ch : text_page.chars) {
    char_spans.push_back({ch.x, ch.x + ch.width});
  }
  std::sort(char_spans.begin(), char_spans.end());

  // Find gaps larger than average character width (indicates column boundaries)
  double avg_char_width = 0.0;
  for (const auto& ch : text_page.chars) {
    avg_char_width += ch.width;
  }
  avg_char_width /= text_page.chars.size();
  double gap_threshold = avg_char_width * 2.0;  // Gap must be at least 2x char width

  grid.col_positions.push_back(char_spans.front().first);
  for (size_t i = 1; i < char_spans.size(); ++i) {
    double gap = char_spans[i].first - char_spans[i-1].second;
    if (gap > gap_threshold) {
      // Large gap detected - add column boundary
      double boundary = (char_spans[i-1].second + char_spans[i].first) / 2.0;
      if (grid.col_positions.empty() ||
          !approx_equal(grid.col_positions.back(), boundary, config.alignment_tolerance)) {
        grid.col_positions.push_back(boundary);
      }
    }
  }
  grid.col_positions.push_back(char_spans.back().second);

  if (config.debug) {
    std::cout << "Grid detection: " << grid.row_positions.size() - 1 << " rows, "
              << grid.col_positions.size() - 1 << " columns\n";
    std::cout << "Gap threshold: " << gap_threshold << " (avg char width: " << avg_char_width << ")\n";
  }

  return grid;
}

// Assign characters to grid cells
std::vector<std::vector<std::vector<TextChar>>> assign_chars_to_grid(
    const TextPage& text_page,
    const GridStructure& grid,
    const TableExtractionConfig& config) {

  int rows = static_cast<int>(grid.row_positions.size()) - 1;
  int cols = static_cast<int>(grid.col_positions.size()) - 1;

  if (rows <= 0 || cols <= 0) {
    return {};
  }

  // Initialize 2D array of character vectors
  std::vector<std::vector<std::vector<TextChar>>> cell_chars(
      rows, std::vector<std::vector<TextChar>>(cols));

  // Assign each character to a cell
  for (const auto& ch : text_page.chars) {
    // Find which cell this character belongs to
    int row = -1, col = -1;

    // Find row
    for (int r = 0; r < rows; ++r) {
      if (ch.y >= grid.row_positions[r] - config.alignment_tolerance &&
          ch.y <= grid.row_positions[r + 1] + config.alignment_tolerance) {
        row = r;
        break;
      }
    }

    // Find column
    for (int c = 0; c < cols; ++c) {
      if (ch.x >= grid.col_positions[c] - config.alignment_tolerance &&
          ch.x <= grid.col_positions[c + 1] + config.alignment_tolerance) {
        col = c;
        break;
      }
    }

    if (row >= 0 && col >= 0) {
      cell_chars[row][col].push_back(ch);
    }
  }

  return cell_chars;
}

// Build table from grid and assigned characters
Table build_table_from_grid(
    const GridStructure& grid,
    const std::vector<std::vector<std::vector<TextChar>>>& cell_chars) {

  Table table;
  table.rows = static_cast<int>(grid.row_positions.size()) - 1;
  table.cols = static_cast<int>(grid.col_positions.size()) - 1;

  if (table.rows <= 0 || table.cols <= 0) {
    return table;
  }

  // Calculate table bounding box
  table.x = grid.col_positions.front();
  table.y = grid.row_positions.front();
  table.width = grid.col_positions.back() - grid.col_positions.front();
  table.height = grid.row_positions.back() - grid.row_positions.front();

  // Initialize cells
  table.cells.resize(table.rows);
  for (int r = 0; r < table.rows; ++r) {
    table.cells[r].resize(table.cols);

    for (int c = 0; c < table.cols; ++c) {
      TableCell& cell = table.cells[r][c];
      cell.row = r;
      cell.col = c;
      cell.x = grid.col_positions[c];
      cell.y = grid.row_positions[r];
      cell.width = grid.col_positions[c + 1] - grid.col_positions[c];
      cell.height = grid.row_positions[r + 1] - grid.row_positions[r];

      // Build text from characters
      const auto& chars = cell_chars[r][c];
      cell.chars = chars;

      if (!chars.empty()) {
        std::ostringstream oss;
        for (const auto& ch : chars) {
          if (ch.unicode < 0x10000) {
            // BMP character - simple conversion
            if (ch.unicode >= 0x20 && ch.unicode < 0x7F) {
              oss << static_cast<char>(ch.unicode);
            } else if (ch.unicode >= 0x80) {
              // UTF-8 encoding for non-ASCII
              if (ch.unicode < 0x800) {
                oss << static_cast<char>(0xC0 | (ch.unicode >> 6));
                oss << static_cast<char>(0x80 | (ch.unicode & 0x3F));
              } else {
                oss << static_cast<char>(0xE0 | (ch.unicode >> 12));
                oss << static_cast<char>(0x80 | ((ch.unicode >> 6) & 0x3F));
                oss << static_cast<char>(0x80 | (ch.unicode & 0x3F));
              }
            }
          } else {
            // Supplementary plane - 4-byte UTF-8
            uint32_t u = ch.unicode;
            oss << static_cast<char>(0xF0 | (u >> 18));
            oss << static_cast<char>(0x80 | ((u >> 12) & 0x3F));
            oss << static_cast<char>(0x80 | ((u >> 6) & 0x3F));
            oss << static_cast<char>(0x80 | (u & 0x3F));
          }
        }
        cell.text = oss.str();
      }
    }
  }

  return table;
}

// Main API: Extract tables from a text page
std::vector<Table> extract_tables(
    const TextPage& text_page,
    const TableExtractionConfig& config) {

  std::vector<Table> tables;

  // Step 1: Detect grid structure
  GridStructure grid = detect_grid_structure(text_page, config);

  // Check if grid is valid for a table
  if (!grid.is_valid_table(config.min_rows, config.min_cols)) {
    if (config.debug) {
      std::cout << "Grid not valid for table extraction\n";
    }
    return tables;
  }

  // Step 2: Assign characters to grid cells
  auto cell_chars = assign_chars_to_grid(text_page, grid, config);

  // Step 3: Build table from grid
  Table table = build_table_from_grid(grid, cell_chars);

  if (table.rows >= config.min_rows && table.cols >= config.min_cols) {
    tables.push_back(table);
  }

  return tables;
}

// Table member functions

const TableCell* Table::get_cell(int row, int col) const {
  if (row < 0 || row >= rows || col < 0 || col >= cols) {
    return nullptr;
  }
  return &cells[row][col];
}

TableCell* Table::get_cell(int row, int col) {
  if (row < 0 || row >= rows || col < 0 || col >= cols) {
    return nullptr;
  }
  return &cells[row][col];
}

std::string Table::to_csv() const {
  std::ostringstream oss;

  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const auto& cell = cells[r][c];

      // Quote cell text and escape quotes
      oss << '"';
      for (char ch : cell.text) {
        if (ch == '"') {
          oss << "\"\"";  // Escape quotes by doubling
        } else {
          oss << ch;
        }
      }
      oss << '"';

      if (c < cols - 1) {
        oss << ',';
      }
    }
    oss << '\n';
  }

  return oss.str();
}

std::string Table::to_html() const {
  std::ostringstream oss;

  oss << "<table border=\"1\">\n";

  for (int r = 0; r < rows; ++r) {
    oss << "  <tr>\n";

    for (int c = 0; c < cols; ++c) {
      const auto& cell = cells[r][c];

      // Use <th> for first row (header), <td> for others
      const char* tag = (r == 0) ? "th" : "td";

      oss << "    <" << tag;
      if (cell.row_span > 1) {
        oss << " rowspan=\"" << cell.row_span << "\"";
      }
      if (cell.col_span > 1) {
        oss << " colspan=\"" << cell.col_span << "\"";
      }
      oss << ">";

      // Escape HTML entities
      for (char ch : cell.text) {
        if (ch == '<') oss << "&lt;";
        else if (ch == '>') oss << "&gt;";
        else if (ch == '&') oss << "&amp;";
        else if (ch == '"') oss << "&quot;";
        else oss << ch;
      }

      oss << "</" << tag << ">\n";
    }

    oss << "  </tr>\n";
  }

  oss << "</table>\n";

  return oss.str();
}

std::string Table::to_json() const {
  std::ostringstream oss;

  oss << "{\n";
  oss << "  \"rows\": " << rows << ",\n";
  oss << "  \"cols\": " << cols << ",\n";
  oss << "  \"bbox\": [" << x << ", " << y << ", " << width << ", " << height << "],\n";
  oss << "  \"cells\": [\n";

  for (int r = 0; r < rows; ++r) {
    oss << "    [\n";

    for (int c = 0; c < cols; ++c) {
      const auto& cell = cells[r][c];

      oss << "      {";
      oss << "\"text\": \"";

      // Escape JSON strings
      for (char ch : cell.text) {
        if (ch == '"') oss << "\\\"";
        else if (ch == '\\') oss << "\\\\";
        else if (ch == '\n') oss << "\\n";
        else if (ch == '\r') oss << "\\r";
        else if (ch == '\t') oss << "\\t";
        else oss << ch;
      }

      oss << "\", ";
      oss << "\"row\": " << cell.row << ", ";
      oss << "\"col\": " << cell.col << ", ";
      oss << "\"row_span\": " << cell.row_span << ", ";
      oss << "\"col_span\": " << cell.col_span;
      oss << "}";

      if (c < cols - 1) oss << ",";
      oss << "\n";
    }

    oss << "    ]";
    if (r < rows - 1) oss << ",";
    oss << "\n";
  }

  oss << "  ]\n";
  oss << "}\n";

  return oss.str();
}

std::string Table::get_text() const {
  std::ostringstream oss;

  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      oss << cells[r][c].text;
      if (c < cols - 1) {
        oss << '\t';  // Tab-separated within rows
      }
    }
    oss << '\n';
  }

  return oss.str();
}

}  // namespace nanopdf
