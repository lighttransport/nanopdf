// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// PDF to PNG rasterizer using nanopdf and ThorVG backend
//
// Usage: rasterize <input.pdf> <output.png> [options]
//
// Options:
//   -p, --page <n>     Page number to render (default: 1)
//   -w, --width <n>    Output width in pixels (default: 800)
//   -h, --height <n>   Output height in pixels (default: 600)
//   -s, --scale <f>    Scale factor (overrides width/height)
//   --dpi <n>          DPI for rendering (default: 72)
//   --all              Render all pages (creates multiple PNG files)
//   --verbose          Verbose output

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#ifdef NANOPDF_USE_THORVG
#include "../../src/thorvg-backend.hh"
#endif

#include "../../src/nanopdf.hh"

struct RasterizeOptions {
  std::string input_file;
  std::string output_file;
  int page_number = 1;  // 1-based page numbering
  int width = 800;
  int height = 600;
  float scale = 0.0f;  // 0 means auto-calculate
  float dpi = 72.0f;
  bool render_all_pages = false;
  bool verbose = false;
};

void print_usage(const char* program_name) {
  std::cout << "PDF to PNG Rasterizer using nanopdf and ThorVG\n";
  std::cout << "\n";
  std::cout << "Usage: " << program_name << " <input.pdf> <output.png> [options]\n";
  std::cout << "\n";
  std::cout << "Options:\n";
  std::cout << "  -p, --page <n>     Page number to render (default: 1)\n";
  std::cout << "  -w, --width <n>    Output width in pixels (default: 800)\n";
  std::cout << "  -h, --height <n>   Output height in pixels (default: 600)\n";
  std::cout << "  -s, --scale <f>    Scale factor (overrides width/height)\n";
  std::cout << "  --dpi <n>          DPI for rendering (default: 72)\n";
  std::cout << "  --all              Render all pages (creates multiple PNG files)\n";
  std::cout << "  --verbose          Verbose output\n";
  std::cout << "  --help             Show this help message\n";
  std::cout << "\n";
  std::cout << "Examples:\n";
  std::cout << "  " << program_name << " document.pdf output.png\n";
  std::cout << "  " << program_name << " document.pdf output.png -p 2 -w 1024 -h 768\n";
  std::cout << "  " << program_name << " document.pdf output.png --all --dpi 150\n";
  std::cout << "  " << program_name << " document.pdf output.png -s 2.0\n";
}

bool parse_arguments(int argc, char* argv[], RasterizeOptions& options) {
  if (argc < 3) {
    return false;
  }

  options.input_file = argv[1];
  options.output_file = argv[2];

  // Parse additional options
  for (int i = 3; i < argc; i++) {
    std::string arg = argv[i];

    if ((arg == "-p" || arg == "--page") && i + 1 < argc) {
      options.page_number = std::atoi(argv[++i]);
      if (options.page_number < 1) {
        std::cerr << "Error: Page number must be >= 1\n";
        return false;
      }
    } else if ((arg == "-w" || arg == "--width") && i + 1 < argc) {
      options.width = std::atoi(argv[++i]);
      if (options.width <= 0) {
        std::cerr << "Error: Width must be > 0\n";
        return false;
      }
    } else if ((arg == "-h" || arg == "--height") && i + 1 < argc) {
      options.height = std::atoi(argv[++i]);
      if (options.height <= 0) {
        std::cerr << "Error: Height must be > 0\n";
        return false;
      }
    } else if ((arg == "-s" || arg == "--scale") && i + 1 < argc) {
      options.scale = std::atof(argv[++i]);
      if (options.scale <= 0) {
        std::cerr << "Error: Scale must be > 0\n";
        return false;
      }
    } else if (arg == "--dpi" && i + 1 < argc) {
      options.dpi = std::atof(argv[++i]);
      if (options.dpi <= 0) {
        std::cerr << "Error: DPI must be > 0\n";
        return false;
      }
    } else if (arg == "--all") {
      options.render_all_pages = true;
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--help") {
      return false;
    } else {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return false;
    }
  }

  return true;
}

std::string generate_output_filename(const std::string& base_output, int page_num, int total_pages) {
  // If rendering all pages, append page number before extension
  if (total_pages > 1) {
    size_t dot_pos = base_output.rfind('.');
    if (dot_pos != std::string::npos) {
      std::stringstream ss;
      ss << base_output.substr(0, dot_pos);
      ss << "_page" << std::setfill('0') << std::setw(3) << page_num;
      ss << base_output.substr(dot_pos);
      return ss.str();
    } else {
      std::stringstream ss;
      ss << base_output << "_page" << std::setfill('0') << std::setw(3) << page_num;
      return ss.str();
    }
  }
  return base_output;
}

bool render_page(const nanopdf::Pdf& pdf, const nanopdf::Page& page, int page_num,
                const RasterizeOptions& options, const std::string& output_file) {
#ifdef NANOPDF_USE_THORVG
  // Get page dimensions from media_box [left, bottom, right, top]
  double page_width = 612.0;  // Default US Letter
  double page_height = 792.0;
  if (page.media_box.size() >= 4) {
    page_width = page.media_box[2] - page.media_box[0];
    page_height = page.media_box[3] - page.media_box[1];
  }

  if (options.verbose) {
    std::cout << "  Page " << page_num << " dimensions: "
              << page_width << " x " << page_height << " pts\n";
  }

  // Calculate output dimensions
  int output_width = options.width;
  int output_height = options.height;

  if (options.scale > 0) {
    // Use scale factor
    output_width = static_cast<int>(page_width * options.scale);
    output_height = static_cast<int>(page_height * options.scale);
  } else if (options.dpi != 72.0f) {
    // Calculate based on DPI (PDF uses 72 DPI by default)
    float dpi_scale = options.dpi / 72.0f;
    output_width = static_cast<int>(page_width * dpi_scale);
    output_height = static_cast<int>(page_height * dpi_scale);
  } else {
    // Maintain aspect ratio if only one dimension is different from default
    float aspect_ratio = page_width / page_height;
    if (output_width == 800 && output_height != 600) {
      output_width = static_cast<int>(output_height * aspect_ratio);
    } else if (output_height == 600 && output_width != 800) {
      output_height = static_cast<int>(output_width / aspect_ratio);
    }
  }

  if (options.verbose) {
    std::cout << "  Output dimensions: " << output_width << " x " << output_height << " px\n";
  }

  // Initialize ThorVG backend
  nanopdf::ThorVGBackend backend;
  if (!backend.initialize(output_width, output_height)) {
    std::cerr << "Error: Failed to initialize ThorVG backend\n";
    return false;
  }

  // Render the page
  if (options.verbose) {
    std::cout << "  Rendering page " << page_num << "...\n";
  }

  auto result = backend.render_page(pdf, page);
  if (!result.success) {
    std::cerr << "Error: Failed to render page " << page_num << ": " << result.error << "\n";
    return false;
  }

  // Save to PNG
  if (backend.save_to_png(output_file)) {
    if (options.verbose) {
      std::cout << "  Saved to: " << output_file << "\n";
    }
    return true;
  } else {
    std::cerr << "Error: Failed to save PNG file: " << output_file << "\n";
    return false;
  }

#else
  std::cerr << "Error: ThorVG support not enabled. Please rebuild nanopdf with -DNANOPDF_USE_THORVG=ON\n";
  return false;
#endif
}

int main(int argc, char* argv[]) {
  RasterizeOptions options;

  // Parse command line arguments
  if (!parse_arguments(argc, argv, options)) {
    print_usage(argv[0]);
    return 1;
  }

  // Check for ThorVG support
#ifndef NANOPDF_USE_THORVG
  std::cerr << "Error: This program requires ThorVG support.\n";
  std::cerr << "Please rebuild nanopdf with -DNANOPDF_USE_THORVG=ON\n";
  return 1;
#endif

  if (options.verbose) {
    std::cout << "PDF to PNG Rasterizer\n";
    std::cout << "Input: " << options.input_file << "\n";
    std::cout << "Output: " << options.output_file << "\n";
  }

  // Read PDF file
  std::ifstream ifs(options.input_file, std::ios::binary);
  if (!ifs) {
    std::cerr << "Error: Failed to open PDF file: " << options.input_file << "\n";
    return 1;
  }

  std::vector<uint8_t> pdf_data((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
  ifs.close();

  if (options.verbose) {
    std::cout << "PDF file size: " << pdf_data.size() << " bytes\n";
  }

  // Parse PDF
  nanopdf::Pdf pdf;
  if (!nanopdf::parse_from_memory(pdf_data.data(), pdf_data.size(), &pdf)) {
    std::cerr << "Error: Failed to parse PDF file\n";
    return 1;
  }

  if (options.verbose) {
    std::cout << "PDF version: " << pdf.version_major << "." << pdf.version_minor << "\n";
  }

  // The catalog is already parsed and available in pdf.catalog
  int total_pages = static_cast<int>(pdf.catalog.pages.size());
  if (options.verbose) {
    std::cout << "Total pages: " << total_pages << "\n";
  }

  if (total_pages == 0) {
    std::cerr << "Error: PDF has no pages\n";
    return 1;
  }

  // Determine which pages to render
  std::vector<int> pages_to_render;
  if (options.render_all_pages) {
    for (int i = 1; i <= total_pages; i++) {
      pages_to_render.push_back(i);
    }
  } else {
    if (options.page_number > total_pages) {
      std::cerr << "Error: Page " << options.page_number << " does not exist. ";
      std::cerr << "PDF has " << total_pages << " page(s).\n";
      return 1;
    }
    pages_to_render.push_back(options.page_number);
  }

  // Render pages
  int success_count = 0;
  for (int page_num : pages_to_render) {
    const auto& page = pdf.catalog.pages[page_num - 1];  // Convert to 0-based index

    std::string output_file = generate_output_filename(
      options.output_file, page_num,
      options.render_all_pages ? total_pages : 1
    );

    if (options.verbose) {
      std::cout << "\nProcessing page " << page_num << " of " << total_pages << ":\n";
    }

    if (render_page(pdf, page, page_num, options, output_file)) {
      success_count++;
      if (!options.verbose) {
        std::cout << "Rendered page " << page_num << " -> " << output_file << "\n";
      }
    }
  }

  // Report results
  if (options.verbose) {
    std::cout << "\nSummary:\n";
    std::cout << "  Successfully rendered " << success_count << " of "
              << pages_to_render.size() << " page(s)\n";
  }

  return (success_count == static_cast<int>(pages_to_render.size())) ? 0 : 1;
}