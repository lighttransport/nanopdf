// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#pragma once

#ifdef NANOPDF_USE_THORVG

#include <thorvg.h>
#include <memory>
#include <vector>
#include <string>

#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstring.h"
#include "nanostring.h"
#include "nanovector.h"
using namespace nanostl;
#else
#include <cstring>
#include <string>
#include <vector>
#endif

#include "nanopdf.hh"

namespace nanopdf {

struct ThorVGRenderResult {
  bool success{false};
  std::string error;
  std::vector<uint8_t> pixels;  // RGBA8888 format
  uint32_t width{0};
  uint32_t height{0};
};

class ThorVGBackend {
public:
  ThorVGBackend();
  ~ThorVGBackend();

  // Initialize ThorVG with given canvas size
  bool initialize(uint32_t width, uint32_t height);

  // Render a PDF page
  ThorVGRenderResult render_page(const Pdf& pdf, const Page& page);

  // Direct drawing API for testing
  bool begin_scene();
  bool end_scene();

  // Shape drawing
  bool draw_rectangle(float x, float y, float width, float height,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
  bool draw_circle(float cx, float cy, float radius,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
  bool draw_path(const std::vector<tvg::PathCommand>& cmds, const std::vector<tvg::Point>& pts,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Text drawing (basic support)
  bool draw_text(float x, float y, const std::string& text, float size,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Line drawing
  bool draw_line(float x1, float y1, float x2, float y2, float stroke_width,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

  // Get rendered buffer
  ThorVGRenderResult get_buffer();

  // Save to PNG file
  bool save_to_png(const std::string& filename);

private:
  // Parse PDF content stream and convert to ThorVG shapes
  bool parse_pdf_content(const std::vector<uint8_t>& content_data);

  // Graphics state for PDF rendering
  struct GraphicsState {
    float current_x{0.0f};
    float current_y{0.0f};
    float text_x{0.0f};
    float text_y{0.0f};
    float font_size{12.0f};
    uint8_t fill_r{0}, fill_g{0}, fill_b{0}, fill_a{255};
    uint8_t stroke_r{0}, stroke_g{0}, stroke_b{0}, stroke_a{255};
    float stroke_width{1.0f};
    bool in_text_block{false};
    bool in_path{false};
    std::vector<tvg::PathCommand> path_commands;
    std::vector<tvg::Point> path_points;

    // Transformation matrix [a b c d e f]
    // Maps (x, y) -> (ax + cy + e, bx + dy + f)
    struct Matrix {
      float a{1.0f}, b{0.0f};  // Scale and rotation
      float c{0.0f}, d{1.0f};  // Scale and rotation
      float e{0.0f}, f{0.0f};  // Translation

      // Apply transformation to a point
      void transform(float& x, float& y) const {
        float nx = a * x + c * y + e;
        float ny = b * x + d * y + f;
        x = nx;
        y = ny;
      }

      // Concatenate with another matrix (multiply)
      Matrix operator*(const Matrix& other) const {
        Matrix result;
        result.a = a * other.a + b * other.c;
        result.b = a * other.b + b * other.d;
        result.c = c * other.a + d * other.c;
        result.d = c * other.b + d * other.d;
        result.e = e * other.a + f * other.c + other.e;
        result.f = e * other.b + f * other.d + other.f;
        return result;
      }
    } transform;
  };

  std::unique_ptr<tvg::SwCanvas> canvas_;
  std::unique_ptr<tvg::Scene> scene_;
  std::vector<uint32_t> buffer_;
  uint32_t width_{0};
  uint32_t height_{0};
  bool initialized_{false};
  GraphicsState state_;
};

}  // namespace nanopdf

#endif // NANOPDF_USE_THORVG