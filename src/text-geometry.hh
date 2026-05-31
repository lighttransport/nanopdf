// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: MIT

#ifndef NANOPDF_TEXT_GEOMETRY_HH_
#define NANOPDF_TEXT_GEOMETRY_HH_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nanopdf {

enum class TextWritingMode {
  Horizontal = 0,
  Vertical = 1
};

struct TextQuad {
  double x1{0.0}, y1{0.0};
  double x2{0.0}, y2{0.0};
  double x3{0.0}, y3{0.0};
  double x4{0.0}, y4{0.0};

  double x{0.0};
  double y{0.0};
  double width{0.0};
  double height{0.0};
};

struct TextSelectionSegment {
  size_t start{0};
  size_t length{0};
  std::string text;
  TextQuad quad;
  int line_index{-1};
  TextWritingMode writing_mode{TextWritingMode::Horizontal};
};

struct TextSelectionResult {
  uint32_t page_number{0};
  size_t start{0};
  size_t length{0};
  std::string text;
  TextQuad bounds;
  std::vector<TextSelectionSegment> segments;
};

inline const char* text_writing_mode_name(TextWritingMode mode) {
  return mode == TextWritingMode::Vertical ? "vertical" : "horizontal";
}

}  // namespace nanopdf

#endif  // NANOPDF_TEXT_GEOMETRY_HH_
