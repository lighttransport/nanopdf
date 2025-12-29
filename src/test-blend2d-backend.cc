// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifdef NANOPDF_USE_BLEND2D

#include "blend2d-backend.hh"
#include "nanopdf.hh"

using namespace nanopdf;

int main(int argc, char** argv) {
  printf("Blend2D Backend Test\n");
  printf("====================\n\n");

  // Part 1: Basic drawing test
  printf("Part 1: Drawing basic shapes and text...\n");

  Blend2DBackend backend;
  if (!backend.initialize(800, 600)) {
    printf("ERROR: Failed to initialize Blend2D backend\n");
    return 1;
  }

  backend.begin_scene();

  // Draw some shapes
  backend.draw_rectangle(50, 50, 200, 150, 255, 0, 0, 255);      // Red rectangle
  backend.draw_circle(500, 200, 80, 0, 0, 255, 200);             // Blue circle (semi-transparent)
  backend.draw_line(100, 400, 700, 400, 3.0f, 0, 128, 0, 255);   // Green line

  // Draw some text
  backend.draw_text(300, 100, "Blend2D Test", 32.0f, 0, 0, 0, 255);

  backend.end_scene();

  if (backend.save_to_png("blend2d_test_shapes.png")) {
    printf("Saved shapes test to: blend2d_test_shapes.png\n");
  } else {
    printf("ERROR: Failed to save shapes test\n");
  }

  // Part 2: PDF rendering test (if PDF file provided)
  if (argc > 1) {
    printf("\nPart 2: Rendering PDF file: %s\n", argv[1]);

    // Load PDF file
    std::ifstream ifs(argv[1], std::ios::binary);
    if (!ifs) {
      printf("ERROR: Failed to open PDF file: %s\n", argv[1]);
      return 1;
    }

    std::vector<uint8_t> pdf_data((std::istreambuf_iterator<char>(ifs)),
                                  std::istreambuf_iterator<char>());
    ifs.close();

    Pdf pdf;
    if (!parse_from_memory(pdf_data.data(), pdf_data.size(), &pdf)) {
      printf("ERROR: Failed to parse PDF\n");
      return 1;
    }

    printf("PDF Info:\n");
    printf("  Version: %d.%d\n", pdf.version_major, pdf.version_minor);

    // Load document structure (includes catalog and pages)
    if (!pdf.load_document_structure()) {
      printf("ERROR: Failed to load document structure\n");
      return 1;
    }

    if (!pdf.catalog.pages.empty()) {
      printf("  Pages: %zu\n", pdf.catalog.pages.size());

      const Page& page = pdf.catalog.pages[0];
      float page_width = 612.0f;
      float page_height = 792.0f;

      if (page.media_box.size() >= 4) {
        page_width = static_cast<float>(page.media_box[2] - page.media_box[0]);
        page_height = static_cast<float>(page.media_box[3] - page.media_box[1]);
      }
      printf("  First page dimensions: %.0f x %.0f\n", page_width, page_height);

      // Create backend for PDF rendering
      Blend2DBackend pdf_backend;
      uint32_t render_width = 800;
      uint32_t render_height = static_cast<uint32_t>(render_width * page_height / page_width);

      if (!pdf_backend.initialize(render_width, render_height)) {
        printf("ERROR: Failed to initialize PDF backend\n");
        return 1;
      }

      auto result = pdf_backend.render_page(pdf, page);

      if (result.success) {
        if (pdf_backend.save_to_png("blend2d_pdf_page1.png")) {
          printf("Saved PDF page 1 to: blend2d_pdf_page1.png\n");
        }
      } else {
        printf("ERROR: Failed to render PDF: %s\n", result.error.c_str());
      }
    }
  }

  printf("\nTest completed successfully!\n");
  return 0;
}

#else

int main() {
  printf("Blend2D support not enabled. Build with -DNANOPDF_USE_BLEND2D=ON\n");
  return 0;
}

#endif // NANOPDF_USE_BLEND2D
