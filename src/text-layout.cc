// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: MIT

#include "text-layout.hh"
#include "nanopdf.hh"
#include "string-parse.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
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

TextQuad make_quad(double x, double y, double width, double height) {
  TextQuad q;
  q.x = x;
  q.y = y;
  q.width = std::max(0.0, width);
  q.height = std::max(0.0, height);
  q.x1 = q.x;
  q.y1 = q.y;
  q.x2 = q.x + q.width;
  q.y2 = q.y;
  q.x3 = q.x + q.width;
  q.y3 = q.y + q.height;
  q.x4 = q.x;
  q.y4 = q.y + q.height;
  return q;
}

TextQuad merge_quads(const TextQuad& a, const TextQuad& b) {
  if (a.width <= 0.0 && a.height <= 0.0) return b;
  if (b.width <= 0.0 && b.height <= 0.0) return a;

  const double x1 = std::min(a.x, b.x);
  const double y1 = std::min(a.y, b.y);
  const double x2 = std::max(a.x + a.width, b.x + b.width);
  const double y2 = std::max(a.y + a.height, b.y + b.height);
  return make_quad(x1, y1, x2 - x1, y2 - y1);
}

bool quad_intersects_rect(const TextQuad& q, double x1, double y1, double x2, double y2) {
  return q.x < x2 && (q.x + q.width) > x1 &&
         q.y < y2 && (q.y + q.height) > y1;
}

std::string char_to_utf8(uint32_t unicode) {
  std::string result;
  if (unicode < 0x80) {
    result += static_cast<char>(unicode);
  } else if (unicode < 0x800) {
    result += static_cast<char>(0xC0 | (unicode >> 6));
    result += static_cast<char>(0x80 | (unicode & 0x3F));
  } else if (unicode < 0x10000) {
    result += static_cast<char>(0xE0 | (unicode >> 12));
    result += static_cast<char>(0x80 | ((unicode >> 6) & 0x3F));
    result += static_cast<char>(0x80 | (unicode & 0x3F));
  } else {
    result += static_cast<char>(0xF0 | (unicode >> 18));
    result += static_cast<char>(0x80 | ((unicode >> 12) & 0x3F));
    result += static_cast<char>(0x80 | ((unicode >> 6) & 0x3F));
    result += static_cast<char>(0x80 | (unicode & 0x3F));
  }
  return result;
}

void apply_word_bounds(TextWord* word) {
  if (!word || word->chars.empty()) return;

  TextQuad bounds;
  for (const auto& ch : word->chars) {
    bounds = merge_quads(bounds, ch.quad);
  }
  word->x = bounds.x;
  word->y = bounds.y;
  word->width = bounds.width;
  word->height = bounds.height;
}

// Sort characters by geometric position (top-to-bottom, left-to-right)
void sort_chars_geometric(std::vector<TextChar>& chars) {
  std::sort(chars.begin(), chars.end(), [](const TextChar& a, const TextChar& b) {
    if (a.writing_mode == TextWritingMode::Vertical &&
        b.writing_mode == TextWritingMode::Vertical) {
      if (!approx_equal(a.x, b.x, 2.0)) {
        return a.x > b.x;  // CJK vertical columns usually read right to left.
      }
      return a.y > b.y;
    }

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
    double baseline = seed.writing_mode == TextWritingMode::Vertical ? seed.x : seed.y;
    double rotation = seed.rotation;
    TextWritingMode writing_mode = seed.writing_mode;

    // Calculate maximum horizontal gap (use char width as reference)
    double max_gap = std::max(seed.width, seed.height) * 10.0;

    // Find all horizontally adjacent characters on the same baseline
    bool changed = true;
    while (changed) {
      changed = false;

      for (size_t j = i + 1; j < chars.size(); ++j) {
        if (assigned[j]) continue;

        const TextChar& candidate = chars[j];

        if (candidate.writing_mode != writing_mode) {
          continue;
        }

        // Check baseline/column alignment (within tolerance)
        double candidate_baseline =
            writing_mode == TextWritingMode::Vertical ? candidate.x : candidate.y;
        if (!approx_equal(candidate_baseline, baseline, options.baseline_tolerance)) {
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
          double forward_gap = 0.0;
          double reverse_gap = 0.0;
          if (writing_mode == TextWritingMode::Vertical) {
            forward_gap = std::abs(candidate.y - (existing.y - candidate.height));
            reverse_gap = std::abs(existing.y - (candidate.y - existing.height));
          } else {
            forward_gap = std::abs(candidate.x - (existing.x + existing.width));
            reverse_gap = std::abs(existing.x - (candidate.x + candidate.width));
          }
          double min_gap = std::min(forward_gap, reverse_gap);

          if (min_gap < max_gap) {
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

    // Sort line characters in reading order.
    std::sort(line.begin(), line.end(), [&chars](int a, int b) {
      if (chars[a].writing_mode == TextWritingMode::Vertical &&
          chars[b].writing_mode == TextWritingMode::Vertical) {
        return chars[a].y > chars[b].y;
      }
      return chars[a].x < chars[b].x;
    });

    lines.push_back(line);
  }

  return lines;
}

// Detect reading order for lines (handles multi-column layouts)
// Returns the detected number of columns.
int assign_reading_order(
    std::vector<TextLine>& lines,
    const TextLayoutOptions& options) {

  if (lines.empty()) return 1;

  // Detect columns if enabled
  int num_columns = 1;
  std::vector<double> column_boundaries;

  bool vertical_layout = false;
  for (const auto& line : lines) {
    if (line.writing_mode == TextWritingMode::Vertical) {
      vertical_layout = true;
      break;
    }
  }

  if (vertical_layout) {
    std::vector<size_t> indices(lines.size());
    for (size_t i = 0; i < indices.size(); ++i) {
      indices[i] = i;
    }

    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
      if (lines[a].writing_mode != lines[b].writing_mode) {
        return lines[a].writing_mode == TextWritingMode::Vertical;
      }
      if (lines[a].writing_mode == TextWritingMode::Vertical) {
        if (!approx_equal(lines[a].x, lines[b].x, options.column_gap_threshold)) {
          return lines[a].x > lines[b].x;
        }
        return lines[a].y > lines[b].y;
      }
      if (!approx_equal(lines[a].y, lines[b].y, options.baseline_tolerance)) {
        return lines[a].y > lines[b].y;
      }
      return lines[a].x < lines[b].x;
    });

    for (size_t i = 0; i < indices.size(); ++i) {
      lines[indices[i]].reading_order = static_cast<int>(i);
    }
    return static_cast<int>(indices.size());
  }

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

  return num_columns;
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
      double gap = line.writing_mode == TextWritingMode::Vertical
                       ? (line.chars[i - 1].y - (line.chars[i].y + line.chars[i].height))
                       : (line.chars[i].x - (line.chars[i-1].x + line.chars[i-1].width));
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
    current_word.writing_mode = line.writing_mode;

    for (size_t i = 0; i < line.chars.size(); ++i) {
      const TextChar& ch = line.chars[i];

      // Check if we should start a new word
      if (!current_word.chars.empty()) {
        const TextChar& prev = current_word.chars.back();
        double gap = line.writing_mode == TextWritingMode::Vertical
                         ? (prev.y - (ch.y + ch.height))
                         : (ch.x - (prev.x + prev.width));

        if (gap > word_threshold || ch.unicode == ' ') {
          // Finish current word
          if (!current_word.chars.empty()) {
            apply_word_bounds(&current_word);
            words.push_back(current_word);
          }

          // Start new word
          current_word = TextWord();
          current_word.line_index = static_cast<int>(line_idx);
          current_word.writing_mode = line.writing_mode;

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
      apply_word_bounds(&current_word);
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

std::string TextPage::to_markdown() const {
  if (lines.empty()) return "";

  // Sort lines by reading order
  std::vector<const TextLine*> sorted_lines;
  for (const auto& line : lines) {
    sorted_lines.push_back(&line);
  }
  std::sort(sorted_lines.begin(), sorted_lines.end(),
            [](const TextLine* a, const TextLine* b) {
              return a->reading_order < b->reading_order;
            });

  // Compute body font size as the mode (most common) font size
  std::map<int, int> size_counts;
  for (const auto& ch : chars) {
    if (ch.font_size > 0) {
      int size_key = static_cast<int>(ch.font_size * 10 + 0.5);  // 0.1pt resolution
      size_counts[size_key]++;
    }
  }
  double body_font_size = 12.0;
  int max_count = 0;
  for (const auto& sc : size_counts) {
    if (sc.second > max_count) {
      max_count = sc.second;
      body_font_size = sc.first / 10.0;
    }
  }

  // Compute average line height for paragraph break detection
  double avg_line_height = 0.0;
  int line_height_count = 0;
  for (size_t i = 1; i < sorted_lines.size(); ++i) {
    double gap = std::abs(sorted_lines[i - 1]->y - sorted_lines[i]->y);
    if (gap > 0.1 && gap < body_font_size * 5) {
      avg_line_height += gap;
      line_height_count++;
    }
  }
  if (line_height_count > 0) avg_line_height /= line_height_count;
  if (avg_line_height < 1.0) avg_line_height = body_font_size * 1.2;

  std::string result;
  bool prev_was_blank = false;

  for (size_t i = 0; i < sorted_lines.size(); ++i) {
    const TextLine* line = sorted_lines[i];
    std::string text = line->get_text();

    // Trim trailing whitespace
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
      text.pop_back();
    if (text.empty()) {
      if (!prev_was_blank) result += "\n";
      prev_was_blank = true;
      continue;
    }
    prev_was_blank = false;

    // Determine dominant font size for this line
    double line_font_size = body_font_size;
    if (!line->chars.empty()) {
      // Use the font size of the first non-space character
      for (const auto& ch : line->chars) {
        if (ch.font_size > 0 && ch.unicode > 32) {
          line_font_size = ch.font_size;
          break;
        }
      }
    }

    // Heading detection: font size ratio relative to body
    double ratio = line_font_size / body_font_size;
    if (ratio >= 1.8) {
      result += "# " + text + "\n\n";
      continue;
    } else if (ratio >= 1.4) {
      result += "## " + text + "\n\n";
      continue;
    } else if (ratio >= 1.15) {
      result += "### " + text + "\n\n";
      continue;
    }

    // List detection: check for bullet or number patterns
    bool is_list = false;
    if (text.size() >= 2) {
      // Bullet patterns: "- ", "* ", "o ", bullet char
      char first = text[0];
      if ((first == '-' || first == '*') && text[1] == ' ') {
        is_list = true;
      }
      // Number patterns: "1. ", "2) ", "(a) "
      if (!is_list && std::isdigit(static_cast<unsigned char>(first))) {
        size_t j = 1;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
        if (j < text.size() && (text[j] == '.' || text[j] == ')') &&
            j + 1 < text.size() && text[j + 1] == ' ') {
          is_list = true;
        }
      }
      // Unicode bullet (U+2022 = 0xE2 0x80 0xA2 in UTF-8)
      if (!is_list && text.size() >= 3 &&
          static_cast<uint8_t>(text[0]) == 0xE2 &&
          static_cast<uint8_t>(text[1]) == 0x80 &&
          static_cast<uint8_t>(text[2]) == 0xA2) {
        is_list = true;
        text = "- " + text.substr(3);
        // Trim leading space
        while (text.size() > 2 && text[2] == ' ') text.erase(2, 1);
      }
    }

    if (is_list) {
      result += text + "\n";
      continue;
    }

    // Paragraph break detection: gap > 1.5x average line height
    if (i > 0) {
      double gap = std::abs(sorted_lines[i - 1]->y - line->y);
      if (gap > avg_line_height * 1.5) {
        // Ensure previous content ends with double newline
        if (!result.empty() && result.back() != '\n') {
          result += "\n";
        }
        if (result.size() >= 2 && result[result.size() - 2] != '\n') {
          result += "\n";
        }
      }
    }

    // Regular text line - join words with space
    result += text + "\n";
  }

  return result;
}

std::string TextPage::get_text_in_rect(double x1, double y1, double x2, double y2) const {
  return select_text_in_rect(x1, y1, x2, y2).text;
}

std::vector<int> TextPage::find_text(const std::string& search_term) const {
  std::vector<int> results;

  std::string page_text = get_text();

  // Simple substring search
  size_t pos = 0;
  while ((pos = page_text.find(search_term, pos)) != std::string::npos) {
    results.push_back(static_cast<int>(pos));
    pos++;
  }

  return results;
}

TextSelectionResult TextPage::select_text_range(size_t start, size_t length) const {
  TextSelectionResult selection;
  selection.start = start;
  selection.length = length;
  if (length == 0) {
    return selection;
  }

  const size_t end = start + length;
  TextSelectionSegment current;
  bool have_current = false;

  std::vector<const TextLine*> sorted_lines;
  for (const auto& line : lines) sorted_lines.push_back(&line);
  std::sort(sorted_lines.begin(), sorted_lines.end(), [](const TextLine* a, const TextLine* b) {
    return a->reading_order < b->reading_order;
  });

  for (const auto* line_ptr : sorted_lines) {
    const TextLine& line = *line_ptr;
    for (const auto& ch : line.chars) {
      const std::string utf8 = char_to_utf8(ch.unicode);
      const size_t char_start = ch.char_index;
      const size_t char_end = ch.char_index + utf8.size();
      if (char_end <= start || char_start >= end) {
        continue;
      }

      selection.text += utf8;
      selection.bounds = merge_quads(selection.bounds, ch.quad);

      const bool same_segment =
          have_current &&
          current.line_index == ch.line_index &&
          current.writing_mode == ch.writing_mode;
      if (!same_segment) {
        if (have_current) {
          selection.segments.push_back(current);
        }
        current = TextSelectionSegment();
        current.start = char_start;
        current.line_index = ch.line_index;
        current.writing_mode = ch.writing_mode;
        current.quad = ch.quad;
        have_current = true;
      } else {
        current.quad = merge_quads(current.quad, ch.quad);
      }

      current.length = (char_end > current.start) ? (char_end - current.start) : current.length;
      current.text += utf8;
    }

    if (have_current && !current.text.empty()) {
      selection.segments.push_back(current);
      current = TextSelectionSegment();
      have_current = false;
    }
  }

  if (have_current) {
    selection.segments.push_back(current);
  }
  return selection;
}

TextSelectionResult TextPage::select_text_in_rect(double x1, double y1, double x2, double y2) const {
  if (x1 > x2) std::swap(x1, x2);
  if (y1 > y2) std::swap(y1, y2);

  TextSelectionResult selection;
  bool started = false;
  size_t last_end = 0;

  std::vector<const TextLine*> sorted_lines;
  for (const auto& line : lines) sorted_lines.push_back(&line);
  std::sort(sorted_lines.begin(), sorted_lines.end(), [](const TextLine* a, const TextLine* b) {
    return a->reading_order < b->reading_order;
  });

  for (const auto* line_ptr : sorted_lines) {
    const TextLine& line = *line_ptr;
    TextSelectionSegment segment;
    bool have_segment = false;

    for (const auto& ch : line.chars) {
      if (!quad_intersects_rect(ch.quad, x1, y1, x2, y2)) {
        continue;
      }

      const std::string utf8 = char_to_utf8(ch.unicode);
      if (!started) {
        selection.start = ch.char_index;
        started = true;
      }
      last_end = ch.char_index + utf8.size();
      selection.text += utf8;
      selection.bounds = merge_quads(selection.bounds, ch.quad);

      if (!have_segment) {
        segment.start = ch.char_index;
        segment.line_index = ch.line_index;
        segment.writing_mode = ch.writing_mode;
        segment.quad = ch.quad;
        have_segment = true;
      } else {
        segment.quad = merge_quads(segment.quad, ch.quad);
      }
      segment.length = last_end - segment.start;
      segment.text += utf8;
    }

    if (have_segment) {
      selection.segments.push_back(segment);
      selection.text += '\n';
      last_end += 1;
    }
  }

  if (!selection.text.empty() && selection.text.back() == '\n') {
    selection.text.pop_back();
  }
  if (started && last_end > selection.start) {
    selection.length = last_end - selection.start;
  }
  return selection;
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

  for (auto& ch : page->chars) {
    if (ch.quad.width <= 0.0 && ch.quad.height <= 0.0) {
      ch.quad = make_quad(ch.x, ch.y, ch.width, ch.height);
    }
  }

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
      TextQuad bounds;
      for (const auto& ch : line.chars) {
        bounds = merge_quads(bounds, ch.quad);
      }
      line.x = bounds.x;
      line.y = bounds.y;
      line.baseline = line.chars.front().writing_mode == TextWritingMode::Vertical
                          ? line.chars.front().x
                          : line.chars.front().y;
      line.rotation = line.chars.front().rotation;
      line.writing_mode = line.chars.front().writing_mode;
      line.width = bounds.width;
      line.height = bounds.height;

      page->lines.push_back(line);
    }
  }

  // Step 4: Assign reading order (handles multi-column)
  page->num_columns = assign_reading_order(page->lines, options);

  // Step 5: Segment into words
  page->words = segment_into_words(page->lines, options);

  // Step 6: Update char indices with line assignments.
  for (size_t line_idx = 0; line_idx < page->lines.size(); ++line_idx) {
    for (auto& ch : page->lines[line_idx].chars) {
      ch.line_index = static_cast<int>(line_idx);
    }
  }

  size_t text_index = 0;
  std::vector<size_t> line_order(page->lines.size());
  for (size_t i = 0; i < line_order.size(); ++i) {
    line_order[i] = i;
  }
  std::sort(line_order.begin(), line_order.end(), [&](size_t a, size_t b) {
    return page->lines[a].reading_order < page->lines[b].reading_order;
  });

  std::vector<TextChar> reading_chars;
  for (size_t li : line_order) {
    for (auto& ch : page->lines[li].chars) {
      ch.char_index = text_index;
      text_index += char_to_utf8(ch.unicode).size();
      reading_chars.push_back(ch);
    }
    text_index += 1;  // newline inserted by get_text()
  }
  page->chars = std::move(reading_chars);

  return page;
}

// Character collector for extracting positioned characters from content streams
class CharacterCollector {
public:
  CharacterCollector(const Pdf& pdf, const Page& page)
      : pdf_(pdf), page_(page) {}

  std::vector<TextChar> collect_characters() {
    PageContent content = page_.load_contents(pdf_);
    if (!content.success) {
      return chars_;
    }

    text_state_.reset();
    process_content_stream(content.data);
    return chars_;
  }

private:
  const Pdf& pdf_;
  const Page& page_;
  TextState text_state_;
  std::vector<TextChar> chars_;

  void process_content_stream(const std::vector<uint8_t>& data) {
    std::string content(data.begin(), data.end());
    std::vector<std::string> tokens = tokenize_content(content);
    std::vector<std::string> operand_stack;

    for (const auto& token : tokens) {
      if (is_operator(token)) {
        execute_operator(token, operand_stack);
        operand_stack.clear();
      } else {
        operand_stack.push_back(token);
      }
    }
  }

  std::vector<std::string> tokenize_content(const std::string& content) {
    std::vector<std::string> tokens;
    size_t pos = 0;

    while (pos < content.size()) {
      while (pos < content.size() && std::isspace(content[pos])) {
        pos++;
      }
      if (pos >= content.size()) break;

      if (content[pos] == '(') {
        std::string str = "(";
        pos++;
        int paren_depth = 1;
        while (pos < content.size() && paren_depth > 0) {
          if (content[pos] == '\\' && pos + 1 < content.size()) {
            str += content[pos++];
            str += content[pos++];
          } else if (content[pos] == '(') {
            paren_depth++;
            str += content[pos++];
          } else if (content[pos] == ')') {
            paren_depth--;
            str += content[pos++];
          } else {
            str += content[pos++];
          }
        }
        tokens.push_back(str);
      } else if (content[pos] == '<' && pos + 1 < content.size() && content[pos + 1] == '<') {
        tokens.push_back("<<");
        pos += 2;
      } else if (content[pos] == '>' && pos + 1 < content.size() && content[pos + 1] == '>') {
        tokens.push_back(">>");
        pos += 2;
      } else if (content[pos] == '<') {
        std::string hex = "<";
        pos++;
        while (pos < content.size() && content[pos] != '>') {
          hex += content[pos++];
        }
        if (pos < content.size()) {
          hex += content[pos++];
        }
        tokens.push_back(hex);
      } else if (content[pos] == '[' || content[pos] == ']') {
        tokens.push_back(std::string(1, content[pos]));
        pos++;
      } else if (content[pos] == '/') {
        std::string name = "/";
        pos++;
        while (pos < content.size() && !std::isspace(content[pos]) &&
               content[pos] != '/' && content[pos] != '[' && content[pos] != ']' &&
               content[pos] != '<' && content[pos] != '>' && content[pos] != '(') {
          name += content[pos++];
        }
        tokens.push_back(name);
      } else {
        std::string token;
        while (pos < content.size() && !std::isspace(content[pos]) &&
               content[pos] != '/' && content[pos] != '[' && content[pos] != ']' &&
               content[pos] != '<' && content[pos] != '>' && content[pos] != '(') {
          token += content[pos++];
        }
        tokens.push_back(token);
      }
    }
    return tokens;
  }

  bool is_operator(const std::string& token) {
    static const std::set<std::string> operators = {
      "BT", "ET", "Td", "TD", "Tm", "T*", "Tj", "TJ", "'", "\"",
      "Tc", "Tw", "Tz", "TL", "Tf", "Tr", "Ts",
      "q", "Q", "cm"
    };
    return operators.find(token) != operators.end();
  }

  void execute_operator(const std::string& op, const std::vector<std::string>& operands) {
    if (op == "BT") {
      text_state_.reset();
    } else if (op == "ET") {
      // End text block
    } else if (op == "Td" && operands.size() >= 2) {
      double tx = parse_number(operands[0]);
      double ty = parse_number(operands[1]);
      text_state_.line_matrix[4] += tx * text_state_.line_matrix[0] + ty * text_state_.line_matrix[2];
      text_state_.line_matrix[5] += tx * text_state_.line_matrix[1] + ty * text_state_.line_matrix[3];
      std::copy(text_state_.line_matrix, text_state_.line_matrix + 6, text_state_.text_matrix);
    } else if (op == "TD" && operands.size() >= 2) {
      double tx = parse_number(operands[0]);
      double ty = parse_number(operands[1]);
      text_state_.leading = -ty;
      text_state_.line_matrix[4] += tx * text_state_.line_matrix[0] + ty * text_state_.line_matrix[2];
      text_state_.line_matrix[5] += tx * text_state_.line_matrix[1] + ty * text_state_.line_matrix[3];
      std::copy(text_state_.line_matrix, text_state_.line_matrix + 6, text_state_.text_matrix);
    } else if (op == "Tm" && operands.size() >= 6) {
      for (int i = 0; i < 6; i++) {
        text_state_.text_matrix[i] = parse_number(operands[i]);
      }
      std::copy(text_state_.text_matrix, text_state_.text_matrix + 6, text_state_.line_matrix);
    } else if (op == "T*") {
      text_state_.line_matrix[5] -= text_state_.leading;
      std::copy(text_state_.line_matrix, text_state_.line_matrix + 6, text_state_.text_matrix);
    } else if (op == "Tj" && operands.size() >= 1) {
      extract_characters_from_string(operands[0]);
    } else if (op == "TJ" && operands.size() >= 1) {
      extract_characters_from_array(operands[0]);
    } else if (op == "'" && operands.size() >= 1) {
      execute_operator("T*", {});
      execute_operator("Tj", operands);
    } else if (op == "\"" && operands.size() >= 3) {
      text_state_.word_spacing = parse_number(operands[0]);
      text_state_.char_spacing = parse_number(operands[1]);
      execute_operator("T*", {});
      execute_operator("Tj", {operands[2]});
    } else if (op == "Tc" && operands.size() >= 1) {
      text_state_.char_spacing = parse_number(operands[0]);
    } else if (op == "Tw" && operands.size() >= 1) {
      text_state_.word_spacing = parse_number(operands[0]);
    } else if (op == "Tz" && operands.size() >= 1) {
      text_state_.horizontal_scaling = parse_number(operands[0]);
    } else if (op == "TL" && operands.size() >= 1) {
      text_state_.leading = parse_number(operands[0]);
    } else if (op == "Tf" && operands.size() >= 2) {
      text_state_.font_name = operands[0];
      text_state_.font_size = parse_number(operands[1]);
      std::string font_name = text_state_.font_name.substr(1);
      text_state_.current_font = page_.get_font(font_name, pdf_);
    } else if (op == "Tr" && operands.size() >= 1) {
      int mode = static_cast<int>(parse_number(operands[0]));
      if (mode >= 0 && mode <= 7) {
        text_state_.render_mode = static_cast<TextRenderingMode>(mode);
      }
    } else if (op == "Ts" && operands.size() >= 1) {
      text_state_.text_rise = parse_number(operands[0]);
    }
  }

  void extract_characters_from_string(const std::string& str) {
    std::string raw = decode_pdf_string_raw(str);

    for (size_t i = 0; i < raw.size(); ) {
      // Extract character code (single or multi-byte)
      uint32_t char_code = static_cast<uint8_t>(raw[i]);
      size_t code_len = 1;

      // For CID fonts, use 2-byte codes
      if (text_state_.current_font && text_state_.current_font->subtype == "Type0") {
        if (i + 1 < raw.size()) {
          char_code = (static_cast<uint8_t>(raw[i]) << 8) | static_cast<uint8_t>(raw[i + 1]);
          code_len = 2;
        }
      }

      // Map to Unicode
      uint32_t unicode = map_to_unicode(char_code);
      const Type0Font* type0_font = as_type0_font(text_state_.current_font);
      const bool is_vertical = type0_font && type0_font->is_vertical;

      // Create TextChar
      TextChar ch;
      ch.unicode = unicode;
      ch.font_size = text_state_.font_size;
      ch.font_name = text_state_.font_name.empty() ? "Unknown" : text_state_.font_name.substr(1);
      ch.char_spacing = text_state_.char_spacing;
      ch.word_spacing = text_state_.word_spacing;
      std::copy(text_state_.text_matrix, text_state_.text_matrix + 6, ch.matrix);
      ch.rotation = calculate_rotation(ch.matrix);
      ch.writing_mode = is_vertical ? TextWritingMode::Vertical : TextWritingMode::Horizontal;

      // Get character width from font
      double char_width = get_char_width(char_code);
      if (is_vertical) {
        double v_x = 0.0;
        double v_y = type0_font ? type0_font->default_v_y : 880.0;
        double w1_y = type0_font ? type0_font->default_w1_y : -1000.0;
        if (type0_font) {
          auto vm_it = type0_font->cid_vertical_metrics.find(char_code);
          if (vm_it != type0_font->cid_vertical_metrics.end()) {
            w1_y = vm_it->second.w1_y;
            v_x = vm_it->second.v_x;
            v_y = vm_it->second.v_y;
          }
        }

        const double glyph_w = text_state_.font_size;
        const double glyph_h = std::max(text_state_.font_size,
                                        std::abs(w1_y) * text_state_.font_size * 0.001);
        const double origin_x = text_state_.text_matrix[4] + v_x * text_state_.font_size * 0.001;
        const double origin_y = text_state_.text_matrix[5] + v_y * text_state_.font_size * 0.001;
        ch.x = origin_x - glyph_w * 0.5;
        ch.y = origin_y - glyph_h;
        ch.width = glyph_w;
        ch.height = glyph_h;
        ch.quad = make_quad(ch.x, ch.y, ch.width, ch.height);
      } else {
        ch.x = text_state_.text_matrix[4];
        ch.y = text_state_.text_matrix[5];
        ch.width = char_width * text_state_.font_size * 0.001;  // PDF units
        ch.height = text_state_.font_size;

        // Apply horizontal scaling
        ch.width *= text_state_.horizontal_scaling / 100.0;
        ch.quad = make_quad(ch.x, ch.y, ch.width, ch.height);
      }

      chars_.push_back(ch);

      // Advance text matrix
      if (is_vertical) {
        double w1_y = type0_font ? type0_font->default_w1_y : -1000.0;
        if (type0_font) {
          auto vm_it = type0_font->cid_vertical_metrics.find(char_code);
          if (vm_it != type0_font->cid_vertical_metrics.end()) {
            w1_y = vm_it->second.w1_y;
          }
        }
        double ty = w1_y * text_state_.font_size * 0.001;
        text_state_.text_matrix[4] += ty * text_state_.text_matrix[2];
        text_state_.text_matrix[5] += ty * text_state_.text_matrix[3];
      } else {
        double tx = (char_width * text_state_.font_size + text_state_.char_spacing) * text_state_.horizontal_scaling / 100.0;
        if (unicode == ' ') {
          tx += text_state_.word_spacing;
        }
        text_state_.text_matrix[4] += tx * text_state_.text_matrix[0] / 1000.0;
        text_state_.text_matrix[5] += tx * text_state_.text_matrix[1] / 1000.0;
      }

      i += code_len;
    }
  }

  void extract_characters_from_array(const std::string& array_str) {
    if (array_str.front() != '[' || array_str.back() != ']') return;

    std::string content = array_str.substr(1, array_str.size() - 2);
    std::vector<std::string> elements = parse_array_elements(content);

    for (const auto& elem : elements) {
      if (elem.front() == '(' || elem.front() == '<') {
        extract_characters_from_string(elem);
      } else {
        // Numeric adjustment - adjust text matrix
        double adjustment = parse_number(elem);
        const Type0Font* type0_font = as_type0_font(text_state_.current_font);
        if (type0_font && type0_font->is_vertical) {
          double ty = adjustment * text_state_.font_size / 1000.0;
          text_state_.text_matrix[4] += ty * text_state_.text_matrix[2] / 100.0;
          text_state_.text_matrix[5] += ty * text_state_.text_matrix[3] / 100.0;
        } else {
          double tx = -adjustment * text_state_.font_size * text_state_.horizontal_scaling / 100000.0;
          text_state_.text_matrix[4] += tx * text_state_.text_matrix[0];
          text_state_.text_matrix[5] += tx * text_state_.text_matrix[1];
        }
      }
    }
  }

  std::vector<std::string> parse_array_elements(const std::string& content) {
    std::vector<std::string> elements;
    size_t pos = 0;

    while (pos < content.size()) {
      while (pos < content.size() && std::isspace(content[pos])) pos++;
      if (pos >= content.size()) break;

      if (content[pos] == '(') {
        size_t start = pos++;
        int depth = 1;
        while (pos < content.size() && depth > 0) {
          if (content[pos] == '\\' && pos + 1 < content.size()) {
            pos += 2;
          } else if (content[pos] == '(') {
            depth++;
            pos++;
          } else if (content[pos] == ')') {
            depth--;
            pos++;
          } else {
            pos++;
          }
        }
        elements.push_back(content.substr(start, pos - start));
      } else if (content[pos] == '<') {
        size_t start = pos++;
        while (pos < content.size() && content[pos] != '>') pos++;
        if (pos < content.size()) pos++;
        elements.push_back(content.substr(start, pos - start));
      } else {
        size_t start = pos;
        while (pos < content.size() && !std::isspace(content[pos]) &&
               content[pos] != '(' && content[pos] != '<') {
          pos++;
        }
        elements.push_back(content.substr(start, pos - start));
      }
    }
    return elements;
  }

  uint32_t map_to_unicode(uint32_t char_code) {
    if (!text_state_.current_font) {
      return char_code < 128 ? char_code : '?';
    }

    const BaseFont* font = text_state_.current_font;

    // Try ToUnicode CMap first
    if (!font->to_unicode_cmap.code_to_unicode.empty()) {
      auto it = font->to_unicode_cmap.code_to_unicode.find(char_code);
      if (it != font->to_unicode_cmap.code_to_unicode.end()) {
        return it->second;
      }
    }

    // For simple fonts, use character code directly
    if (font->subtype != "Type0") {
      return char_code < 256 ? char_code : '?';
    }

    // For Type0 fonts with Identity-H/V, use CID as Unicode (approximation)
    return char_code;
  }

  double get_char_width(uint32_t char_code) {
    if (!text_state_.current_font) {
      return 500.0;  // Default width
    }

    const BaseFont* font = text_state_.current_font;

    // For Type0 fonts, check CID widths
    if (font->subtype == "Type0") {
      const Type0Font* type0 = as_type0_font(font);
      if (type0) {
        auto it = type0->cid_widths.find(char_code);
        if (it != type0->cid_widths.end()) {
          return static_cast<double>(it->second);
        }
        return static_cast<double>(type0->default_width);
      }
    }

    // Check widths array for simple fonts
    if (!font->widths.empty() && char_code >= font->first_char) {
      size_t index = char_code - font->first_char;
      if (index < font->widths.size()) {
        return static_cast<double>(font->widths[index]);
      }
    }

    return 500.0;  // Fallback
  }

  std::string decode_pdf_string_raw(const std::string& str) {
    if (str.empty()) return "";

    if (str[0] == '(') {
      std::string result = str.substr(1, str.size() - 2);
      size_t pos = 0;
      while ((pos = result.find("\\", pos)) != std::string::npos) {
        if (pos + 1 < result.size()) {
          char next = result[pos + 1];
          if (next == 'n') {
            result.replace(pos, 2, "\n");
          } else if (next == 'r') {
            result.replace(pos, 2, "\r");
          } else if (next == 't') {
            result.replace(pos, 2, "\t");
          } else if (next == '\\' || next == '(' || next == ')') {
            result.erase(pos, 1);
          } else if (next >= '0' && next <= '7') {
            std::string octal;
            size_t i = pos + 1;
            while (i < result.size() && i < pos + 4 && result[i] >= '0' && result[i] <= '7') {
              octal += result[i++];
            }
            int value = nanopdf::stou_base_or(octal, 8);
            result.replace(pos, i - pos, 1, static_cast<char>(value));
          } else {
            pos++;
          }
        } else {
          pos++;
        }
      }
      return result;
    } else if (str[0] == '<') {
      std::string hex = str.substr(1, str.size() - 2);
      std::string result;
      for (size_t i = 0; i < hex.size(); i += 2) {
        std::string byte = hex.substr(i, 2);
        if (byte.size() == 1) byte += "0";
        result += static_cast<char>(nanopdf::stou_base_or(byte, 16));
      }
      return result;
    }
    return str;
  }

  double parse_number(const std::string& str) {
    return nanopdf::stod_or(str);
  }
};

std::unique_ptr<TextPage> extract_text_layout(
    const Pdf& pdf,
    const Page& page,
    const TextLayoutOptions& options) {

  // Get page dimensions
  double width = 612.0;  // Default US Letter
  double height = 792.0;

  if (page.media_box.size() >= 4) {
    width = page.media_box[2] - page.media_box[0];
    height = page.media_box[3] - page.media_box[1];
  }

  // Collect characters from content streams
  CharacterCollector collector(pdf, page);
  std::vector<TextChar> chars = collector.collect_characters();

  return extract_text_layout_with_collector(chars, width, height, options);
}

}  // namespace nanopdf
