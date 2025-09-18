// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#ifdef NANOPDF_USE_THORVG

#include "thorvg-backend.hh"
#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>

// For PNG saving
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace nanopdf {

ThorVGBackend::ThorVGBackend() {
  // Initialize ThorVG engine
  if (tvg::Initializer::init(tvg::CanvasEngine::Sw, 0) != tvg::Result::Success) {
    std::cerr << "Failed to initialize ThorVG" << std::endl;
  }
}

ThorVGBackend::~ThorVGBackend() {
  if (canvas_) {
    canvas_->clear();
  }
  // Terminate ThorVG engine
  tvg::Initializer::term(tvg::CanvasEngine::Sw);
}

bool ThorVGBackend::initialize(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;

  // Create buffer for rendering
  buffer_.resize(width * height);

  // Create software canvas
  canvas_ = tvg::SwCanvas::gen();
  if (!canvas_) {
    return false;
  }

  // Set target buffer
  if (canvas_->target(reinterpret_cast<uint32_t*>(buffer_.data()),
                     width, width, height,
                     tvg::SwCanvas::ABGR8888) != tvg::Result::Success) {
    return false;
  }

  initialized_ = true;
  return true;
}

bool ThorVGBackend::begin_scene() {
  if (!initialized_) {
    return false;
  }

  // Clear the canvas
  canvas_->clear();

  // Create a new scene
  scene_ = tvg::Scene::gen();
  if (!scene_) {
    return false;
  }

  return true;
}

bool ThorVGBackend::end_scene() {
  if (!initialized_ || !scene_) {
    return false;
  }

  // Push scene to canvas
  if (canvas_->push(std::move(scene_)) != tvg::Result::Success) {
    return false;
  }

  // Draw the canvas
  if (canvas_->draw() != tvg::Result::Success) {
    return false;
  }

  // Sync drawing
  if (canvas_->sync() != tvg::Result::Success) {
    return false;
  }

  return true;
}

bool ThorVGBackend::draw_rectangle(float x, float y, float width, float height,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = tvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Append rectangle
  if (shape->appendRect(x, y, width, height, 0, 0) != tvg::Result::Success) {
    return false;
  }

  // Set fill color
  if (shape->fill(r, g, b, a) != tvg::Result::Success) {
    return false;
  }

  // Add to scene
  if (scene_->push(std::move(shape)) != tvg::Result::Success) {
    return false;
  }

  return true;
}

bool ThorVGBackend::draw_circle(float cx, float cy, float radius,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = tvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Append circle
  if (shape->appendCircle(cx, cy, radius, radius) != tvg::Result::Success) {
    return false;
  }

  // Set fill color
  if (shape->fill(r, g, b, a) != tvg::Result::Success) {
    return false;
  }

  // Add to scene
  if (scene_->push(std::move(shape)) != tvg::Result::Success) {
    return false;
  }

  return true;
}

bool ThorVGBackend::draw_path(const std::vector<tvg::PathCommand>& cmds,
                             const std::vector<tvg::Point>& pts,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = tvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Append path
  if (shape->appendPath(cmds.data(), cmds.size(), pts.data(), pts.size()) != tvg::Result::Success) {
    return false;
  }

  // Set fill color
  if (shape->fill(r, g, b, a) != tvg::Result::Success) {
    return false;
  }

  // Add to scene
  if (scene_->push(std::move(shape)) != tvg::Result::Success) {
    return false;
  }

  return true;
}

bool ThorVGBackend::draw_text(float x, float y, const std::string& text, float size,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  // For now, we'll approximate text with rectangles
  // ThorVG doesn't have direct text support, so this would need
  // integration with a text rendering library like FreeType

  // Draw a simple placeholder rectangle for text
  float text_width = text.length() * size * 0.6f;
  float text_height = size;

  auto shape = tvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Simple text representation as a line
  if (shape->appendRect(x, y - text_height * 0.8f, text_width, 2, 0, 0) != tvg::Result::Success) {
    return false;
  }

  if (shape->fill(r, g, b, a) != tvg::Result::Success) {
    return false;
  }

  if (scene_->push(std::move(shape)) != tvg::Result::Success) {
    return false;
  }

  return true;
}

bool ThorVGBackend::draw_line(float x1, float y1, float x2, float y2, float stroke_width,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = tvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Create line path
  if (shape->moveTo(x1, y1) != tvg::Result::Success) {
    return false;
  }
  if (shape->lineTo(x2, y2) != tvg::Result::Success) {
    return false;
  }

  // Set stroke
  if (shape->stroke(stroke_width) != tvg::Result::Success) {
    return false;
  }
  if (shape->stroke(r, g, b, a) != tvg::Result::Success) {
    return false;
  }
  if (shape->stroke(tvg::StrokeCap::Round) != tvg::Result::Success) {
    return false;
  }

  // Add to scene
  if (scene_->push(std::move(shape)) != tvg::Result::Success) {
    return false;
  }

  return true;
}

ThorVGRenderResult ThorVGBackend::get_buffer() {
  ThorVGRenderResult result;

  if (!initialized_) {
    result.error = "Backend not initialized";
    return result;
  }

  result.width = width_;
  result.height = height_;

  // Convert from ABGR8888 to RGBA8888
  result.pixels.resize(width_ * height_ * 4);
  for (size_t i = 0; i < buffer_.size(); ++i) {
    uint32_t pixel = buffer_[i];
    uint8_t a = (pixel >> 24) & 0xFF;
    uint8_t b = (pixel >> 16) & 0xFF;
    uint8_t g = (pixel >> 8) & 0xFF;
    uint8_t r = pixel & 0xFF;

    result.pixels[i * 4 + 0] = r;
    result.pixels[i * 4 + 1] = g;
    result.pixels[i * 4 + 2] = b;
    result.pixels[i * 4 + 3] = a;
  }

  result.success = true;
  return result;
}

bool ThorVGBackend::save_to_png(const std::string& filename) {
  auto result = get_buffer();
  if (!result.success) {
    return false;
  }

  return stbi_write_png(filename.c_str(), result.width, result.height, 4,
                       result.pixels.data(), result.width * 4) != 0;
}

ThorVGRenderResult ThorVGBackend::render_page(const Pdf& pdf, const Page& page) {
  ThorVGRenderResult result;

  if (!initialized_) {
    result.error = "Backend not initialized";
    return result;
  }

  if (!begin_scene()) {
    result.error = "Failed to begin scene";
    return result;
  }

  // Get page dimensions
  double page_width = page.media_box.width();
  double page_height = page.media_box.height();

  // Calculate scale to fit the page into our canvas
  float scale_x = static_cast<float>(width_) / page_width;
  float scale_y = static_cast<float>(height_) / page_height;
  float scale = std::min(scale_x, scale_y);

  // Draw white background
  draw_rectangle(0, 0, width_, height_, 255, 255, 255, 255);

  // Parse and render page content
  for (const auto& content_obj : page.contents) {
    if (content_obj.is_stream()) {
      auto stream = content_obj.stream_value();
      auto decoded_result = pdf.decode_stream(stream);
      if (decoded_result.success) {
        state_ = GraphicsState();  // Reset state
        parse_pdf_content(decoded_result.data);
      }
    }
  }

  if (!end_scene()) {
    result.error = "Failed to end scene";
    return result;
  }

  return get_buffer();
}

bool ThorVGBackend::parse_pdf_content(const std::vector<uint8_t>& content_data) {
  // Enhanced PDF content parser with more operators support

  std::string content(content_data.begin(), content_data.end());
  std::istringstream stream(content);
  std::string token;

  std::vector<std::string> operands;
  std::vector<GraphicsState> state_stack;  // For save/restore state

  while (stream >> token) {
    // Check if token is an operator (starts with a letter or special chars)
    bool is_operator = false;
    if (!token.empty()) {
      char first = token[0];
      if ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
          first == '*' || first == '\'' || first == '"') {
        is_operator = true;
      }
    }

    if (is_operator) {
      // Process operator with accumulated operands

      // Path construction operators
      if (token == "m") {  // moveTo
        if (operands.size() >= 2) {
          float x = std::stof(operands[0]);
          float y = std::stof(operands[1]);

          // Apply transformation
          state_.transform.transform(x, y);

          state_.current_x = x;
          state_.current_y = height_ - y;  // Flip Y coordinate
          state_.path_commands.push_back(tvg::PathCommand::MoveTo);
          state_.path_points.push_back({state_.current_x, state_.current_y});
          state_.in_path = true;
        }
      } else if (token == "l") {  // lineTo
        if (operands.size() >= 2) {
          float x = std::stof(operands[0]);
          float y = std::stof(operands[1]);

          // Apply transformation
          state_.transform.transform(x, y);

          state_.current_x = x;
          state_.current_y = height_ - y;  // Flip Y coordinate
          state_.path_commands.push_back(tvg::PathCommand::LineTo);
          state_.path_points.push_back({state_.current_x, state_.current_y});
        }
      } else if (token == "c") {  // curveTo (cubic Bezier)
        if (operands.size() >= 6) {
          float x1 = std::stof(operands[0]);
          float y1 = std::stof(operands[1]);
          float x2 = std::stof(operands[2]);
          float y2 = std::stof(operands[3]);
          float x3 = std::stof(operands[4]);
          float y3 = std::stof(operands[5]);

          // Apply transformation
          state_.transform.transform(x1, y1);
          state_.transform.transform(x2, y2);
          state_.transform.transform(x3, y3);

          // Flip Y coordinates
          y1 = height_ - y1;
          y2 = height_ - y2;
          y3 = height_ - y3;

          state_.path_commands.push_back(tvg::PathCommand::CubicTo);
          state_.path_points.push_back({x1, y1});
          state_.path_points.push_back({x2, y2});
          state_.path_points.push_back({x3, y3});
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "v") {  // curveTo variant (first control point = current point)
        if (operands.size() >= 4) {
          float x2 = std::stof(operands[0]);
          float y2 = height_ - std::stof(operands[1]);
          float x3 = std::stof(operands[2]);
          float y3 = height_ - std::stof(operands[3]);
          state_.path_commands.push_back(tvg::PathCommand::CubicTo);
          state_.path_points.push_back({state_.current_x, state_.current_y});
          state_.path_points.push_back({x2, y2});
          state_.path_points.push_back({x3, y3});
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "y") {  // curveTo variant (second control point = end point)
        if (operands.size() >= 4) {
          float x1 = std::stof(operands[0]);
          float y1 = height_ - std::stof(operands[1]);
          float x3 = std::stof(operands[2]);
          float y3 = height_ - std::stof(operands[3]);
          state_.path_commands.push_back(tvg::PathCommand::CubicTo);
          state_.path_points.push_back({x1, y1});
          state_.path_points.push_back({x3, y3});
          state_.path_points.push_back({x3, y3});
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "re") {  // rectangle
        if (operands.size() >= 4) {
          float x = std::stof(operands[0]);
          float y = height_ - std::stof(operands[1]) - std::stof(operands[3]);  // Flip Y and adjust for height
          float w = std::stof(operands[2]);
          float h = std::stof(operands[3]);

          // Add rectangle to path
          state_.path_commands.push_back(tvg::PathCommand::MoveTo);
          state_.path_points.push_back({x, y});
          state_.path_commands.push_back(tvg::PathCommand::LineTo);
          state_.path_points.push_back({x + w, y});
          state_.path_commands.push_back(tvg::PathCommand::LineTo);
          state_.path_points.push_back({x + w, y + h});
          state_.path_commands.push_back(tvg::PathCommand::LineTo);
          state_.path_points.push_back({x, y + h});
          state_.path_commands.push_back(tvg::PathCommand::Close);
          state_.in_path = true;
        }
      } else if (token == "h") {  // Close path
        if (state_.in_path) {
          state_.path_commands.push_back(tvg::PathCommand::Close);
        }
      }
      // Path painting operators
      else if (token == "f" || token == "F" || token == "f*") {  // fill (various winding rules)
        if (!state_.path_commands.empty()) {
          draw_path(state_.path_commands, state_.path_points,
                   state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a);
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
        }
      } else if (token == "s") {  // close and stroke
        if (state_.in_path) {
          state_.path_commands.push_back(tvg::PathCommand::Close);
        }
        // Fall through to stroke
      }
      if (token == "S" || token == "s") {  // stroke
        if (!state_.path_commands.empty()) {
          // Create stroked shape
          auto shape = tvg::Shape::gen();
          if (shape) {
            if (shape->appendPath(state_.path_commands.data(), state_.path_commands.size(),
                                 state_.path_points.data(), state_.path_points.size()) == tvg::Result::Success) {
              shape->stroke(state_.stroke_width);
              shape->stroke(state_.stroke_r, state_.stroke_g, state_.stroke_b, state_.stroke_a);
              shape->stroke(tvg::StrokeCap::Round);
              shape->stroke(tvg::StrokeJoin::Round);
              scene_->push(std::move(shape));
            }
          }
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
        }
      } else if (token == "b" || token == "b*") {  // close, fill and stroke
        if (state_.in_path) {
          state_.path_commands.push_back(tvg::PathCommand::Close);
        }
        if (!state_.path_commands.empty()) {
          // Fill
          draw_path(state_.path_commands, state_.path_points,
                   state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a);

          // Stroke
          auto shape = tvg::Shape::gen();
          if (shape) {
            if (shape->appendPath(state_.path_commands.data(), state_.path_commands.size(),
                                 state_.path_points.data(), state_.path_points.size()) == tvg::Result::Success) {
              shape->stroke(state_.stroke_width);
              shape->stroke(state_.stroke_r, state_.stroke_g, state_.stroke_b, state_.stroke_a);
              shape->stroke(tvg::StrokeCap::Round);
              scene_->push(std::move(shape));
            }
          }
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
        }
      } else if (token == "B" || token == "B*") {  // fill and stroke
        if (!state_.path_commands.empty()) {
          // Fill
          draw_path(state_.path_commands, state_.path_points,
                   state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a);

          // Stroke
          auto shape = tvg::Shape::gen();
          if (shape) {
            if (shape->appendPath(state_.path_commands.data(), state_.path_commands.size(),
                                 state_.path_points.data(), state_.path_points.size()) == tvg::Result::Success) {
              shape->stroke(state_.stroke_width);
              shape->stroke(state_.stroke_r, state_.stroke_g, state_.stroke_b, state_.stroke_a);
              shape->stroke(tvg::StrokeCap::Round);
              scene_->push(std::move(shape));
            }
          }
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
        }
      } else if (token == "n") {  // End path without fill or stroke
        state_.path_commands.clear();
        state_.path_points.clear();
        state_.in_path = false;
      }
      // Color operators
      else if (token == "rg") {  // Set RGB fill color
        if (operands.size() >= 3) {
          state_.fill_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.fill_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
          state_.fill_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
        }
      } else if (token == "RG") {  // Set RGB stroke color
        if (operands.size() >= 3) {
          state_.stroke_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.stroke_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
          state_.stroke_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
        }
      } else if (token == "g") {  // Set gray fill color
        if (operands.size() >= 1) {
          uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.fill_r = state_.fill_g = state_.fill_b = gray;
        }
      } else if (token == "G") {  // Set gray stroke color
        if (operands.size() >= 1) {
          uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.stroke_r = state_.stroke_g = state_.stroke_b = gray;
        }
      } else if (token == "k") {  // Set CMYK fill color (simplified to RGB)
        if (operands.size() >= 4) {
          float c = std::stof(operands[0]);
          float m = std::stof(operands[1]);
          float y = std::stof(operands[2]);
          float k = std::stof(operands[3]);
          // Simple CMYK to RGB conversion
          state_.fill_r = static_cast<uint8_t>((1.0f - c) * (1.0f - k) * 255);
          state_.fill_g = static_cast<uint8_t>((1.0f - m) * (1.0f - k) * 255);
          state_.fill_b = static_cast<uint8_t>((1.0f - y) * (1.0f - k) * 255);
        }
      } else if (token == "K") {  // Set CMYK stroke color (simplified to RGB)
        if (operands.size() >= 4) {
          float c = std::stof(operands[0]);
          float m = std::stof(operands[1]);
          float y = std::stof(operands[2]);
          float k = std::stof(operands[3]);
          // Simple CMYK to RGB conversion
          state_.stroke_r = static_cast<uint8_t>((1.0f - c) * (1.0f - k) * 255);
          state_.stroke_g = static_cast<uint8_t>((1.0f - m) * (1.0f - k) * 255);
          state_.stroke_b = static_cast<uint8_t>((1.0f - y) * (1.0f - k) * 255);
        }
      }
      // Line style operators
      else if (token == "w") {  // Set line width
        if (operands.size() >= 1) {
          state_.stroke_width = std::stof(operands[0]);
        }
      } else if (token == "J") {  // Set line cap style
        // 0 = butt, 1 = round, 2 = square
        // ThorVG supports these through stroke() method
      } else if (token == "j") {  // Set line join style
        // 0 = miter, 1 = round, 2 = bevel
        // ThorVG supports these through stroke() method
      }
      // Graphics state operators
      else if (token == "q") {  // Save graphics state
        state_stack.push_back(state_);
      } else if (token == "Q") {  // Restore graphics state
        if (!state_stack.empty()) {
          state_ = state_stack.back();
          state_stack.pop_back();
        }
      }
      // Transformation matrix operators
      else if (token == "cm") {  // Concatenate matrix
        if (operands.size() >= 6) {
          GraphicsState::Matrix m;
          m.a = std::stof(operands[0]);
          m.b = std::stof(operands[1]);
          m.c = std::stof(operands[2]);
          m.d = std::stof(operands[3]);
          m.e = std::stof(operands[4]);
          m.f = std::stof(operands[5]);
          state_.transform = state_.transform * m;
        }
      }
      // Text operators (simplified handling)
      else if (token == "BT") {  // Begin text
        state_.in_text_block = true;
        state_.text_x = 0;
        state_.text_y = 0;
      } else if (token == "ET") {  // End text
        state_.in_text_block = false;
      } else if (token == "Td") {  // Move text position
        if (operands.size() >= 2 && state_.in_text_block) {
          state_.text_x += std::stof(operands[0]);
          state_.text_y += std::stof(operands[1]);
        }
      } else if (token == "Tf") {  // Set font and size
        if (operands.size() >= 2) {
          // operands[0] is font name, operands[1] is size
          state_.font_size = std::stof(operands[1]);
        }
      } else if (token == "Tj") {  // Show text
        if (operands.size() >= 1 && state_.in_text_block) {
          // Remove parentheses from text string if present
          std::string text = operands[0];
          if (!text.empty() && text[0] == '(' && text.back() == ')') {
            text = text.substr(1, text.length() - 2);
          }
          draw_text(state_.text_x, height_ - state_.text_y, text, state_.font_size,
                   state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a);
        }
      }

      operands.clear();
    } else {
      // Accumulate operand
      operands.push_back(token);
    }
  }

  return true;
}

}  // namespace nanopdf

#endif // NANOPDF_USE_THORVG