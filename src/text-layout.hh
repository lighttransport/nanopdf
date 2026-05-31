// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: MIT

#ifndef NANOPDF_TEXT_LAYOUT_HH_
#define NANOPDF_TEXT_LAYOUT_HH_

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>

#include "text-geometry.hh"

namespace nanopdf {

// Forward declarations
struct Pdf;
struct Page;

// Represents a single character with position and metadata
struct TextChar {
  uint32_t unicode;        // Unicode code point
  double x, y;             // Position (bottom-left corner)
  double width, height;    // Bounding box dimensions
  double font_size;        // Font size in points
  std::string font_name;   // Font name
  double char_spacing;     // Character spacing at this position
  double word_spacing;     // Word spacing at this position
  int line_index;          // Which line this char belongs to (-1 if unassigned)
  int word_index;          // Which word this char belongs to (-1 if unassigned)

  // Transformation matrix [a b c d e f]
  double matrix[6];

  // Rotation angle in degrees (derived from matrix)
  double rotation;
  TextWritingMode writing_mode;  // Horizontal or vertical writing mode
  TextQuad quad;                 // Glyph bounds in page coordinates
  size_t char_index;             // Character index in reading-order text

  TextChar()
      : unicode(0), x(0), y(0), width(0), height(0),
        font_size(0), char_spacing(0), word_spacing(0),
        line_index(-1), word_index(-1), rotation(0),
        writing_mode(TextWritingMode::Horizontal), char_index(0) {
    matrix[0] = 1; matrix[1] = 0;
    matrix[2] = 0; matrix[3] = 1;
    matrix[4] = 0; matrix[5] = 0;
  }
};

// Represents a line of text
struct TextLine {
  std::vector<TextChar> chars;  // Characters in this line
  double x, y;                  // Line position (bottom-left)
  double width, height;         // Line bounding box
  int reading_order;            // Sequence in reading order (0, 1, 2...)
  bool is_rtl;                  // Right-to-left text (Arabic, Hebrew)
  double baseline;              // Y-coordinate of baseline
  double rotation;              // Line rotation angle in degrees
  TextWritingMode writing_mode; // Dominant writing mode

  TextLine()
      : x(0), y(0), width(0), height(0),
        reading_order(-1), is_rtl(false),
        baseline(0), rotation(0), writing_mode(TextWritingMode::Horizontal) {}

  // Get text content of this line
  std::string get_text() const;

  // Check if a character position belongs to this line
  bool contains_char(double char_x, double char_y, double tolerance = 2.0) const;
};

// Represents a word (contiguous characters)
struct TextWord {
  std::vector<TextChar> chars;  // Characters in this word
  double x, y;                  // Word position
  double width, height;         // Word bounding box
  int line_index;               // Which line this word belongs to
  TextWritingMode writing_mode; // Dominant writing mode

  TextWord()
      : x(0), y(0), width(0), height(0), line_index(-1),
        writing_mode(TextWritingMode::Horizontal) {}

  // Get text content of this word
  std::string get_text() const;
};

// Represents text layout for an entire page
struct TextPage {
  std::vector<TextChar> chars;   // All characters (unsorted)
  std::vector<TextLine> lines;   // Lines in reading order
  std::vector<TextWord> words;   // Words
  double page_width;
  double page_height;

  // Statistics
  int num_columns;               // Detected number of columns (1, 2, 3+)

  TextPage() : page_width(0), page_height(0), num_columns(1) {}

  // Get all text in reading order
  std::string get_text() const;

  // Get text as Markdown with heading/list/paragraph/table detection
  std::string to_markdown() const;

  // Spatial queries
  std::string get_text_in_rect(double x1, double y1, double x2, double y2) const;

  // Find text (returns character indices)
  std::vector<int> find_text(const std::string& search_term) const;

  // Text selection helpers. Ranges use positions in get_text() reading order.
  TextSelectionResult select_text_range(size_t start, size_t length) const;
  TextSelectionResult select_text_in_rect(double x1, double y1, double x2, double y2) const;

  // Character access
  int count_chars() const { return static_cast<int>(chars.size()); }
  const TextChar& get_char(int index) const { return chars[index]; }
};

// Text layout analysis options
struct TextLayoutOptions {
  // Line detection
  double baseline_tolerance;     // Tolerance for baseline alignment (default: 2.0)
  double line_spacing_threshold; // Threshold for detecting line breaks (default: 1.5x font size)

  // Word segmentation
  double word_spacing_threshold; // Multiple of avg char spacing (default: 2.0)

  // Column detection
  double column_gap_threshold;   // Min gap to detect columns (default: 20.0)
  bool detect_columns;           // Enable multi-column detection (default: true)

  // Reading order
  bool detect_rtl;               // Detect right-to-left text (default: true)

  TextLayoutOptions()
      : baseline_tolerance(2.0),
        line_spacing_threshold(1.5),
        word_spacing_threshold(2.0),
        column_gap_threshold(20.0),
        detect_columns(true),
        detect_rtl(true) {}
};

// Main API: Extract text layout from a page
std::unique_ptr<TextPage> extract_text_layout(
    const Pdf& pdf,
    const Page& page,
    const TextLayoutOptions& options = TextLayoutOptions());

// Advanced: Extract layout with custom character collector
std::unique_ptr<TextPage> extract_text_layout_with_collector(
    const std::vector<TextChar>& chars,
    double page_width,
    double page_height,
    const TextLayoutOptions& options = TextLayoutOptions());

}  // namespace nanopdf

#endif  // NANOPDF_TEXT_LAYOUT_HH_
