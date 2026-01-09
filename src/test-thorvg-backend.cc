// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>

#ifdef NANOPDF_USE_THORVG
#include "thorvg-backend.hh"
#include "nanopdf.hh"

int main(int argc, char** argv) {
  std::cout << "ThorVG Backend Test" << std::endl;
  std::cout << "==================" << std::endl;

  // Initialize ThorVG backend
  nanopdf::ThorVGBackend backend;

  // Create a 800x600 canvas
  uint32_t width = 800;
  uint32_t height = 600;

  if (!backend.initialize(width, height)) {
    std::cerr << "Failed to initialize ThorVG backend" << std::endl;
    return 1;
  }

  // Part 1: Test basic shapes and text
  std::cout << "\nPart 1: Drawing basic shapes and text..." << std::endl;

  if (!backend.begin_scene()) {
    std::cerr << "Failed to begin scene" << std::endl;
    return 1;
  }

  // Draw white background
  backend.draw_rectangle(0, 0, width, height, 255, 255, 255, 255);

  // Draw some shapes
  // Red rectangle
  backend.draw_rectangle(50, 50, 200, 150, 255, 0, 0, 200);

  // Green circle
  backend.draw_circle(400, 150, 75, 0, 255, 0, 200);

  // Blue rectangle
  backend.draw_rectangle(550, 50, 150, 150, 0, 0, 255, 200);

  // Draw some lines
  backend.draw_line(50, 250, 750, 250, 3.0f, 0, 0, 0, 255);
  backend.draw_line(50, 300, 750, 300, 5.0f, 128, 128, 128, 255);

  // Draw a triangle using path
  std::vector<tvg::PathCommand> tri_cmds = {
    tvg::PathCommand::MoveTo,
    tvg::PathCommand::LineTo,
    tvg::PathCommand::LineTo,
    tvg::PathCommand::Close
  };

  std::vector<tvg::Point> tri_pts = {
    {200.0f, 350.0f},
    {300.0f, 350.0f},
    {250.0f, 450.0f}
  };

  backend.draw_path(tri_cmds, tri_pts, 255, 128, 0, 255);

  // Draw a star using path
  std::vector<tvg::PathCommand> star_cmds;
  std::vector<tvg::Point> star_pts;

  float cx = 500.0f;
  float cy = 400.0f;
  float outer_radius = 60.0f;
  float inner_radius = 25.0f;
  int points = 5;

  star_cmds.push_back(tvg::PathCommand::MoveTo);
  star_pts.push_back({cx + outer_radius, cy});

  for (int i = 1; i < points * 2; ++i) {
    float angle = M_PI * i / points;
    float radius = (i % 2 == 0) ? outer_radius : inner_radius;
    float x = cx + radius * cos(angle);
    float y = cy + radius * sin(angle);
    star_cmds.push_back(tvg::PathCommand::LineTo);
    star_pts.push_back({x, y});
  }

  star_cmds.push_back(tvg::PathCommand::Close);
  backend.draw_path(star_cmds, star_pts, 255, 255, 0, 255);

  // Draw some text (simplified representation)
  backend.draw_text(50, 550, "ThorVG Backend Test", 24, 0, 0, 0, 255);
  backend.draw_text(50, 520, "Shapes and Graphics", 18, 64, 64, 64, 255);

  if (!backend.end_scene()) {
    std::cerr << "Failed to end scene" << std::endl;
    return 1;
  }

  // Save to PNG
  std::string output_file = "thorvg_test_shapes.png";
  if (backend.save_to_png(output_file)) {
    std::cout << "Saved shapes test to: " << output_file << std::endl;
  } else {
    std::cerr << "Failed to save PNG" << std::endl;
  }

  // Part 2: Test PDF rendering if a PDF file is provided
  if (argc > 1) {
    std::string pdf_filename = argv[1];
    std::cout << "\nPart 2: Rendering PDF file: " << pdf_filename << std::endl;

    // Load PDF
    std::ifstream ifs(pdf_filename, std::ios::binary);
    if (!ifs) {
      std::cerr << "Failed to open PDF file: " << pdf_filename << std::endl;
      return 1;
    }

    std::vector<uint8_t> pdf_data((std::istreambuf_iterator<char>(ifs)),
                                  std::istreambuf_iterator<char>());
    ifs.close();

    nanopdf::Pdf pdf;
    if (!nanopdf::parse_from_memory(pdf_data.data(), pdf_data.size(), &pdf)) {
      std::cerr << "Failed to parse PDF" << std::endl;
      return 1;
    }

    if (!pdf.load_document_structure()) {
      std::cerr << "Failed to load document structure" << std::endl;
      return 1;
    }

    std::cout << "PDF Info:" << std::endl;
    std::cout << "  Version: " << pdf.version_major << "." << pdf.version_minor << std::endl;

    // Get document catalog
    const nanopdf::DocumentCatalog& catalog = pdf.catalog;

    std::cout << "  Pages: " << catalog.pages.size() << std::endl;

    // Render first page if available
    if (!catalog.pages.empty()) {
      const auto& page = catalog.pages[0];

      // Get page dimensions from media_box [left, bottom, right, top]
      double page_width = 612.0;   // Default letter size
      double page_height = 792.0;
      if (page.media_box.size() >= 4) {
        page_width = page.media_box[2] - page.media_box[0];
        page_height = page.media_box[3] - page.media_box[1];
      }

      std::cout << "  First page dimensions: " << page_width << " x " << page_height << std::endl;

      // Render the page
      auto render_result = backend.render_page(pdf, page);

      if (render_result.success) {
        // Save rendered page to PNG
        std::string pdf_output = "thorvg_pdf_page1.png";
        if (backend.save_to_png(pdf_output)) {
          std::cout << "Saved PDF page 1 to: " << pdf_output << std::endl;
        } else {
          std::cerr << "Failed to save PDF page PNG" << std::endl;
        }
      } else {
        std::cerr << "Failed to render PDF page: " << render_result.error << std::endl;
      }
    }
  } else {
    std::cout << "\nNote: Provide a PDF file as argument to test PDF rendering" << std::endl;
    std::cout << "Usage: " << argv[0] << " <pdf_file>" << std::endl;
  }

  // Part 3: Create a more complex drawing
  std::cout << "\nPart 3: Creating complex drawing..." << std::endl;

  if (!backend.begin_scene()) {
    std::cerr << "Failed to begin scene" << std::endl;
    return 1;
  }

  // Draw gradient-like effect using multiple rectangles
  backend.draw_rectangle(0, 0, width, height, 240, 240, 240, 255);

  // Draw concentric circles with varying opacity
  for (int i = 10; i >= 1; --i) {
    float radius = i * 30.0f;
    uint8_t alpha = 25 * (11 - i);
    backend.draw_circle(width/2, height/2, radius, 100, 150, 255, alpha);
  }

  // Draw a grid pattern
  for (int x = 0; x <= width; x += 50) {
    backend.draw_line(x, 0, x, height, 0.5f, 200, 200, 200, 128);
  }
  for (int y = 0; y <= height; y += 50) {
    backend.draw_line(0, y, width, y, 0.5f, 200, 200, 200, 128);
  }

  // Draw bezier curves
  std::vector<tvg::PathCommand> curve_cmds = {
    tvg::PathCommand::MoveTo,
    tvg::PathCommand::CubicTo,
    tvg::PathCommand::CubicTo
  };

  std::vector<tvg::Point> curve_pts = {
    {100.0f, 300.0f},
    {200.0f, 100.0f}, {300.0f, 500.0f}, {400.0f, 300.0f},
    {500.0f, 100.0f}, {600.0f, 500.0f}, {700.0f, 300.0f}
  };

  backend.draw_path(curve_cmds, curve_pts, 255, 0, 128, 200);

  if (!backend.end_scene()) {
    std::cerr << "Failed to end scene" << std::endl;
    return 1;
  }

  // Save complex drawing
  std::string complex_output = "thorvg_test_complex.png";
  if (backend.save_to_png(complex_output)) {
    std::cout << "Saved complex drawing to: " << complex_output << std::endl;
  } else {
    std::cerr << "Failed to save complex PNG" << std::endl;
  }

  std::cout << "\nTest completed successfully!" << std::endl;
  return 0;
}

#else

int main() {
  std::cerr << "ThorVG support not enabled. Please rebuild with -DNANOPDF_USE_THORVG=ON" << std::endl;
  return 1;
}

#endif // NANOPDF_USE_THORVG