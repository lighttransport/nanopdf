// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: MIT

#include "text-layout.hh"
#include "nanopdf.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace nanopdf {

namespace {

// Calculate rotation angle from transformation matrix
double calculate_rotation(const double* matrix) {
  // Extract rotation from matrix [a b c d e f]
  double a = matrix[0];
  double b = matrix[1];
  double angle = std::atan2(b, a) * 180.0 / M_PI;
  return angle;
}

// Calculate distance between two points
double distance(double x1, double y1, double x2, double y2) {
  double dx = x2 - x1;
  double dy = y2 - y1;
  return std::sqrt(dx * dx + dy * dy);
}

// Check if two values are approximately equal
bool approx_equal(double a, double b, double tolerance) {
  return std::abs(a - b) <= tolerance;
}

// Sort characters by geometric position (top-to-bottom, left-to-right)
void sort_chars_geometric(std::vector<TextChar>& chars) {
  std::sort(chars.begin(), chars.end(), [](const TextChar& a, const TextChar& b) {
    // First by Y (top to bottom - note PDF coords have Y increasing upward)
    if (!approx_equal(a.y, b.y, 2.0)) {
      return a.y > b.y;  // Higher Y comes first (top of page)
    }
    // Then by X (left to right)
    return a.x < b.x;
  });
}

// Group characters into lines based on baseline alignment and horizontal proximity
std::vector<std::vector<int>> group_into_lines(
    const std::vector<TextChar>& chars,
    const TextLayoutOptions& options) {

  std::vector<std::vector<int>> lines;
  if (chars.empty()) return lines;

  std::vector<bool> assigned(chars.size(), false);

  // Process characters in geometric order
  for (size_t i = 0; i < chars.size(); ++i) {
    if (assigned[i]) continue;

    // Start a new line with this character
    std::vector<int> line;
    line.push_back(i);
    assigned[i] = true;

    const TextChar& seed = chars[i];
    double baseline = seed.y;
    double rotation = seed.rotation;

    // Calculate maximum horizontal gap (use char width as reference)
    double max_horizontal_gap = seed.width * 10.0;  // Allow up to 10 char widths gap

    // Find all horizontally adjacent characters on the same baseline
    bool changed = true;
    while (changed) {
      changed = false;

      for (size_t j = i + 1; j < chars.size(); ++j) {
        if (assigned[j]) continue;

        const TextChar& candidate = chars[j];

        // Check baseline alignment (within tolerance)
        if (!approx_equal(candidate.y, baseline, options.baseline_tolerance)) {
          continue;
        }

        // Check rotation matches (for rotated text)
        if (!approx_equal(candidate.rotation, rotation, 5.0)) {
          continue;
        }

        // Check horizontal proximity to any existing character in the line
        bool is_adjacent = false;
        for (int idx : line) {
          const TextChar& existing = chars[idx];
          double horizontal_gap = std::abs(candidate.x - (existing.x + existing.width));
          double reverse_gap = std::abs(existing.x - (candidate.x + candidate.width));
          double min_gap = std::min(horizontal_gap, reverse_gap);

          if (min_gap < max_horizontal_gap) {
            is_adjacent = true;
            break;
          }
        }

        if (is_adjacent) {
          line.push_back(j);
          assigned[j] = true;
          changed = true;
        }
      }
    }

    // Sort line characters left-to-right (or right-to-left for RTL)
    std::sort(line.begin(), line.end(), [&chars](int a, int b) {
      return chars[a].x < chars[b].x;
    });

    lines.push_back(line);
  }

  return lines;
}

// Detect reading order for lines (handles multi-column layouts)
void assign_reading_order(
    std::vector<TextLine>& lines,
    const TextLayoutOptions& options) {

  if (lines.empty()) return;

  // Detect columns if enabled
  int num_columns = 1;
  std::vector<double> column_boundaries;

  if (options.detect_columns && lines.size() > 1) {
    // Analyze horizontal gaps between lines
    std::map<double, int> x_positions;
    for (const auto& line : lines) {
      int bucket = static_cast<int>(line.x / 10.0);  // Group by 10-point buckets
      x_positions[bucket * 10.0]++;
    }

    // Find large gaps that might indicate column boundaries
    std::vector<double> x_coords;
    for (const auto& p : x_positions) {
      x_coords.push_back(p.first);
    }
    std::sort(x_coords.begin(), x_coords.end());

    for (size_t i = 1; i < x_coords.size(); ++i) {
      double gap = x_coords[i] - x_coords[i-1];
      if (gap > options.column_gap_threshold) {
        column_boundaries.push_back((x_coords[i-1] + x_coords[i]) / 2.0);
        num_columns++;
      }
    }
  }

  // Assign columns to lines
  std::vector<int> line_columns(lines.size(), 0);
  for (size_t i = 0; i < lines.size(); ++i) {
    int column = 0;
    for (double boundary : column_boundaries) {
      if (lines[i].x >= boundary) {
        column++;
      }
    }
    line_columns[i] = column;
  }

  // Sort lines by column, then by Y position (top to bottom)
  std::vector<size_t> indices(lines.size());
  for (size_t i = 0; i < indices.size(); ++i) {
    indices[i] = i;
  }

  std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
    // First by column
    if (line_columns[a] != line_columns[b]) {
      return line_columns[a] < line_columns[b];
    }
    // Then by Y position (higher Y = top of page comes first)
    return lines[a].y > lines[b].y;
  });

  // Assign reading order
  for (size_t i = 0; i < indices.size(); ++i) {
    lines[indices[i]].reading_order = static_cast<int>(i);
  }
}

// Segment lines into words based on spacing
std::vector<TextWord> segment_into_words(
    const std::vector<TextLine>& lines,
    const TextLayoutOptions& options) {

  std::vector<TextWord> words;

  for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
    const TextLine& line = lines[line_idx];
    if (line.chars.empty()) continue;

    // Calculate average character spacing for this line
    double total_spacing = 0.0;
    int spacing_count = 0;

    for (size_t i = 1; i < line.chars.size(); ++i) {
      double gap = line.chars[i].x - (line.chars[i-1].x + line.chars[i-1].width);
      if (gap >= 0) {  // Only count positive gaps
        total_spacing += gap;
        spacing_count++;
      }
    }

    double avg_spacing = spacing_count > 0 ? total_spacing / spacing_count : 2.0;
    double word_threshold = avg_spacing * options.word_spacing_threshold;

    // Segment into words
    TextWord current_word;
    current_word.line_index = static_cast<int>(line_idx);

    for (size_t i = 0; i < line.chars.size(); ++i) {
      const TextChar& ch = line.chars[i];

      // Check if we should start a new word
      if (!current_word.chars.empty()) {
        const TextChar& prev = current_word.chars.back();
        double gap = ch.x - (prev.x + prev.width);

        if (gap > word_threshold || ch.unicode == ' ') {
          // Finish current word
          if (!current_word.chars.empty()) {
            // Calculate word bounds
            current_word.x = current_word.chars.front().x;
            current_word.y = current_word.chars.front().y;
            const TextChar& last = current_word.chars.back();
            current_word.width = (last.x + last.width) - current_word.x;
            current_word.height = current_word.chars.front().height;

            words.push_back(current_word);
          }

          // Start new word
          current_word = TextWord();
          current_word.line_index = static_cast<int>(line_idx);

          // Skip spaces
          if (ch.unicode == ' ') {
            continue;
          }
        }
      }

      current_word.chars.push_back(ch);
    }

    // Add final word
    if (!current_word.chars.empty()) {
      current_word.x = current_word.chars.front().x;
      current_word.y = current_word.chars.front().y;
      const TextChar& last = current_word.chars.back();
      current_word.width = (last.x + last.width) - current_word.x;
      current_word.height = current_word.chars.front().height;

      words.push_back(current_word);
    }
  }

  return words;
}

}  // anonymous namespace

// TextLine methods
std::string TextLine::get_text() const {
  std::string result;
  for (const auto& ch : chars) {
    if (ch.unicode < 0x80) {
      result += static_cast<char>(ch.unicode);
    } else {
      // Simple UTF-8 encoding for basic multilingual plane
      if (ch.unicode < 0x800) {
        result += static_cast<char>(0xC0 | (ch.unicode >> 6));
        result += static_cast<char>(0x80 | (ch.unicode & 0x3F));
      } else if (ch.unicode < 0x10000) {
        result += static_cast<char>(0xE0 | (ch.unicode >> 12));
        result += static_cast<char>(0x80 | ((ch.unicode >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (ch.unicode & 0x3F));
      } else {
        result += static_cast<char>(0xF0 | (ch.unicode >> 18));
        result += static_cast<char>(0x80 | ((ch.unicode >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((ch.unicode >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (ch.unicode & 0x3F));
      }
    }
  }
  return result;
}

bool TextLine::contains_char(double char_x, double char_y, double tolerance) const {
  return char_x >= (x - tolerance) && char_x <= (x + width + tolerance) &&
         char_y >= (y - tolerance) && char_y <= (y + height + tolerance);
}

// TextWord methods
std::string TextWord::get_text() const {
  std::string result;
  for (const auto& ch : chars) {
    if (ch.unicode < 0x80) {
      result += static_cast<char>(ch.unicode);
    } else {
      // Simple UTF-8 encoding
      if (ch.unicode < 0x800) {
        result += static_cast<char>(0xC0 | (ch.unicode >> 6));
        result += static_cast<char>(0x80 | (ch.unicode & 0x3F));
      } else if (ch.unicode < 0x10000) {
        result += static_cast<char>(0xE0 | (ch.unicode >> 12));
        result += static_cast<char>(0x80 | ((ch.unicode >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (ch.unicode & 0x3F));
      } else {
        result += static_cast<char>(0xF0 | (ch.unicode >> 18));
        result += static_cast<char>(0x80 | ((ch.unicode >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((ch.unicode >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (ch.unicode & 0x3F));
      }
    }
  }
  return result;
}

// TextPage methods
std::string TextPage::get_text() const {
  std::string result;

  // Sort lines by reading order
  std::vector<const TextLine*> sorted_lines;
  for (const auto& line : lines) {
    sorted_lines.push_back(&line);
  }
  std::sort(sorted_lines.begin(), sorted_lines.end(),
            [](const TextLine* a, const TextLine* b) {
              return a->reading_order < b->reading_order;
            });

  // Concatenate line text
  for (const auto* line : sorted_lines) {
    result += line->get_text();
    result += '\n';
  }

  return result;
}

std::string TextPage::get_text_in_rect(double x1, double y1, double x2, double y2) const {
  std::string result;

  // Ensure x1 <= x2 and y1 <= y2
  if (x1 > x2) std::swap(x1, x2);
  if (y1 > y2) std::swap(y1, y2);

  for (const auto& ch : chars) {
    if (ch.x >= x1 && ch.x <= x2 && ch.y >= y1 && ch.y <= y2) {
      if (ch.unicode < 0x80) {
        result += static_cast<char>(ch.unicode);
      }
    }
  }

  return result;
}

std::vector<int> TextPage::find_text(const std::string& search_term) const {
  std::vector<int> results;

  std::string page_text;
  for (const auto& ch : chars) {
    if (ch.unicode < 0x80) {
      page_text += static_cast<char>(ch.unicode);
    }
  }

  // Simple substring search
  size_t pos = 0;
  while ((pos = page_text.find(search_term, pos)) != std::string::npos) {
    results.push_back(static_cast<int>(pos));
    pos++;
  }

  return results;
}

// Main API implementation
std::unique_ptr<TextPage> extract_text_layout_with_collector(
    const std::vector<TextChar>& input_chars,
    double page_width,
    double page_height,
    const TextLayoutOptions& options) {

  auto page = std::unique_ptr<TextPage>(new TextPage());
  page->chars = input_chars;
  page->page_width = page_width;
  page->page_height = page_height;

  if (page->chars.empty()) {
    return page;
  }

  // Step 1: Sort characters geometrically
  std::vector<TextChar> sorted_chars = page->chars;
  sort_chars_geometric(sorted_chars);

  // Step 2: Group into lines
  auto line_groups = group_into_lines(sorted_chars, options);

  // Step 3: Create TextLine objects
  for (const auto& group : line_groups) {
    if (group.empty()) continue;

    TextLine line;

    for (int idx : group) {
      line.chars.push_back(sorted_chars[idx]);
    }

    if (!line.chars.empty()) {
      // Calculate line bounds
      line.x = line.chars.front().x;
      line.y = line.chars.front().y;
      line.baseline = line.y;
      line.rotation = line.chars.front().rotation;

      double max_x = line.x;
      double max_height = 0.0;

      for (const auto& ch : line.chars) {
        max_x = std::max(max_x, ch.x + ch.width);
        max_height = std::max(max_height, ch.height);
      }

      line.width = max_x - line.x;
      line.height = max_height;

      page->lines.push_back(line);
    }
  }

  // Step 4: Assign reading order (handles multi-column)
  assign_reading_order(page->lines, options);

  // Detect number of columns
  std::set<int> reading_orders;
  for (const auto& line : page->lines) {
    reading_orders.insert(line.reading_order);
  }
  page->num_columns = 1;  // Simplified for now

  // Step 5: Segment into words
  page->words = segment_into_words(page->lines, options);

  // Step 6: Update char indices with line and word assignments
  for (size_t line_idx = 0; line_idx < page->lines.size(); ++line_idx) {
    for (auto& ch : page->lines[line_idx].chars) {
      ch.line_index = static_cast<int>(line_idx);
    }
  }

  for (size_t word_idx = 0; word_idx < page->words.size(); ++word_idx) {
    for (auto& ch : page->words[word_idx].chars) {
      ch.word_index = static_cast<int>(word_idx);
    }
  }

  return page;
}

std::unique_ptr<TextPage> extract_text_layout(
    const Pdf& pdf,
    const Page& page,
    const TextLayoutOptions& options) {

  // TODO: Implement character collection from content streams
  // This requires integrating with the existing text extraction code
  // For now, return empty page

  std::vector<TextChar> chars;
  double width = 612.0;  // Default US Letter
  double height = 792.0;

  if (page.media_box.size() >= 4) {
    width = page.media_box[2] - page.media_box[0];
    height = page.media_box[3] - page.media_box[1];
  }

  return extract_text_layout_with_collector(chars, width, height, options);
}

}  // namespace nanopdf
