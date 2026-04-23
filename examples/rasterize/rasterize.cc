// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// PDF to PNG rasterizer using nanopdf and ThorVG backend
//
// Usage: rasterize <input.pdf> <output.png> [options]
//
// Options:
//   -p, --page <n>     Page number to render (default: 1)
//   --pages <spec>     Page selection (e.g. 1-3,7,10-12)
//   -w, --width <n>    Output width in pixels (default: 800)
//   -h, --height <n>   Output height in pixels (default: 600)
//   -s, --scale <f>    Scale factor (overrides width/height)
//   --dpi <n>          DPI for rendering (default: 150, same as pdftoppm)
//   -r, --rotate <n>   Rotation angle: 0, 90, 180, 270 (default: 0)
//   -g, --grayscale    Convert output to grayscale
//   --all              Render all pages (creates multiple PNG files)
//   --verbose          Verbose output
//   --log-level <n>    Log level: 0=none, 1=error, 2=warn, 3=info, 4=debug, 5=trace

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <set>

#ifdef NANOPDF_USE_THORVG
#include "../../src/thorvg-backend.hh"
#endif

#include "../../src/nanopdf.hh"
#include "../../src/nanopdf-log.hh"

// For saving processed images
extern "C" {
  int stbi_write_png(const char* filename, int w, int h, int comp,
                     const void* data, int stride_in_bytes);
}

struct RasterizeOptions {
  std::string input_file;
  std::string output_file;
  int page_number = 1;  // 1-based page numbering
  bool page_option_set = false;
  std::string pages_spec;
  bool pages_spec_set = false;
  int width = 800;
  int height = 600;
  float scale = 0.0f;  // 0 means auto-calculate
  // Default DPI matches pdftoppm's default so that output dimensions line up
  // with the common "poppler at 150 dpi" baseline used by visual-diff tooling.
  float dpi = 150.0f;
  bool width_set = false;
  bool height_set = false;
  int rotation = 0;    // 0, 90, 180, 270 degrees
  bool grayscale = false;
  bool render_all_pages = false;
  bool verbose = false;
  int log_level = 3;  // Default: Info
};

void print_usage(const char* program_name) {
  std::cout << "PDF to PNG Rasterizer using nanopdf and ThorVG\n";
  std::cout << "\n";
  std::cout << "Usage: " << program_name << " <input.pdf> <output.png> [options]\n";
  std::cout << "\n";
  std::cout << "Options:\n";
  std::cout << "  -p, --page <n>     Page number to render (default: 1)\n";
  std::cout << "  --pages <spec>     Page selection (e.g. 1-3,7,10-12)\n";
  std::cout << "  -w, --width <n>    Output width in pixels (default: 800)\n";
  std::cout << "  -h, --height <n>   Output height in pixels (default: 600)\n";
  std::cout << "  -s, --scale <f>    Scale factor (overrides width/height)\n";
  std::cout << "  --dpi <n>          DPI for rendering (default: 150, same as pdftoppm)\n";
  std::cout << "  -r, --rotate <n>   Rotation angle: 0, 90, 180, 270 (default: 0)\n";
  std::cout << "  -g, --grayscale    Convert output to grayscale\n";
  std::cout << "  --all              Render all pages (creates multiple PNG files)\n";
  std::cout << "  --verbose          Verbose output\n";
  std::cout << "  --log-level <n>    Log level: 0=none, 1=error, 2=warn, 3=info, 4=debug, 5=trace\n";
  std::cout << "  --help             Show this help message\n";
  std::cout << "\n";
  std::cout << "Examples:\n";
  std::cout << "  " << program_name << " document.pdf output.png\n";
  std::cout << "  " << program_name << " document.pdf output.png -p 2 -w 1024 -h 768\n";
  std::cout << "  " << program_name << " document.pdf output.png --pages 1-3,7,10\n";
  std::cout << "  " << program_name << " document.pdf output.png --all --dpi 150\n";
  std::cout << "  " << program_name << " document.pdf output.png -s 2.0\n";
  std::cout << "  " << program_name << " document.pdf output.png -r 90 --grayscale\n";
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
      options.page_option_set = true;
    } else if (arg == "--pages" && i + 1 < argc) {
      options.pages_spec = argv[++i];
      if (options.pages_spec.empty()) {
        std::cerr << "Error: --pages must not be empty\n";
        return false;
      }
      options.pages_spec_set = true;
    } else if ((arg == "-w" || arg == "--width") && i + 1 < argc) {
      options.width = std::atoi(argv[++i]);
      if (options.width <= 0) {
        std::cerr << "Error: Width must be > 0\n";
        return false;
      }
      options.width_set = true;
    } else if ((arg == "-h" || arg == "--height") && i + 1 < argc) {
      options.height = std::atoi(argv[++i]);
      if (options.height <= 0) {
        std::cerr << "Error: Height must be > 0\n";
        return false;
      }
      options.height_set = true;
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
    } else if ((arg == "-r" || arg == "--rotate") && i + 1 < argc) {
      options.rotation = std::atoi(argv[++i]);
      if (options.rotation != 0 && options.rotation != 90 &&
          options.rotation != 180 && options.rotation != 270) {
        std::cerr << "Error: Rotation must be 0, 90, 180, or 270\n";
        return false;
      }
    } else if (arg == "-g" || arg == "--grayscale") {
      options.grayscale = true;
    } else if (arg == "--all") {
      options.render_all_pages = true;
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--log-level" && i + 1 < argc) {
      options.log_level = std::atoi(argv[++i]);
      if (options.log_level < 0 || options.log_level > 5) {
        std::cerr << "Error: Log level must be 0-5\n";
        return false;
      }
    } else if (arg == "--help") {
      return false;
    } else {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return false;
    }
  }

  int selection_mode_count = 0;
  if (options.render_all_pages) selection_mode_count++;
  if (options.page_option_set) selection_mode_count++;
  if (options.pages_spec_set) selection_mode_count++;
  if (selection_mode_count > 1) {
    std::cerr << "Error: --all, --page, and --pages are mutually exclusive\n";
    return false;
  }

  return true;
}

static std::string trim_copy(const std::string& s) {
  size_t start = 0;
  while (start < s.size() &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    start++;
  }
  size_t end = s.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    end--;
  }
  return s.substr(start, end - start);
}

static bool parse_positive_int(const std::string& s, int& out) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  out = std::atoi(s.c_str());
  return out > 0;
}

static bool parse_pages_spec(const std::string& spec, std::vector<int>& pages,
                             std::string& error_message) {
  pages.clear();
  std::set<int> seen;

  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t comma = spec.find(',', pos);
    std::string token = (comma == std::string::npos)
                            ? spec.substr(pos)
                            : spec.substr(pos, comma - pos);
    token = trim_copy(token);
    if (token.empty()) {
      error_message = "empty token in --pages spec";
      return false;
    }

    size_t dash = token.find('-');
    if (dash == std::string::npos) {
      int page = 0;
      if (!parse_positive_int(token, page)) {
        error_message = "invalid page number: " + token;
        return false;
      }
      if (!seen.count(page)) {
        pages.push_back(page);
        seen.insert(page);
      }
    } else {
      if (token.find('-', dash + 1) != std::string::npos) {
        error_message = "invalid range token: " + token;
        return false;
      }
      std::string left = trim_copy(token.substr(0, dash));
      std::string right = trim_copy(token.substr(dash + 1));
      int start = 0;
      int end = 0;
      if (!parse_positive_int(left, start) || !parse_positive_int(right, end)) {
        error_message = "invalid range token: " + token;
        return false;
      }
      if (start > end) {
        error_message = "range start > end: " + token;
        return false;
      }
      for (int p = start; p <= end; p++) {
        if (!seen.count(p)) {
          pages.push_back(p);
          seen.insert(p);
        }
      }
    }

    if (comma == std::string::npos) break;
    pos = comma + 1;
  }

  if (pages.empty()) {
    error_message = "no pages selected";
    return false;
  }
  return true;
}

std::string generate_output_filename(const std::string& base_output, int page_num,
                                     bool multi_page_output) {
  if (!multi_page_output) return base_output;

  size_t dot_pos = base_output.rfind('.');
  std::string stem = (dot_pos == std::string::npos) ? base_output
                                                     : base_output.substr(0, dot_pos);
  std::string ext = (dot_pos == std::string::npos) ? "" : base_output.substr(dot_pos);

  std::stringstream page_ss;
  page_ss << std::setfill('0') << std::setw(4) << page_num;
  std::string page_str = page_ss.str();

  // If user provided placeholder "0000" in filename, replace it.
  size_t placeholder_pos = stem.rfind("0000");
  if (placeholder_pos != std::string::npos) {
    stem.replace(placeholder_pos, 4, page_str);
    return stem + ext;
  }

  // Otherwise append "-0000"-style suffix.
  return stem + "-" + page_str + ext;
}

// Convert RGBA pixels to grayscale (in-place, keeps alpha)
void convert_to_grayscale(std::vector<uint8_t>& pixels) {
  for (size_t i = 0; i < pixels.size(); i += 4) {
    // Use luminosity formula: 0.299*R + 0.587*G + 0.114*B
    uint8_t r = pixels[i];
    uint8_t g = pixels[i + 1];
    uint8_t b = pixels[i + 2];
    uint8_t gray = static_cast<uint8_t>(0.299f * r + 0.587f * g + 0.114f * b);
    pixels[i] = gray;
    pixels[i + 1] = gray;
    pixels[i + 2] = gray;
    // Keep alpha unchanged
  }
}

// Rotate RGBA pixels by 90, 180, or 270 degrees
std::vector<uint8_t> rotate_pixels(const std::vector<uint8_t>& src,
                                   int src_width, int src_height,
                                   int rotation,
                                   int& dst_width, int& dst_height) {
  if (rotation == 0) {
    dst_width = src_width;
    dst_height = src_height;
    return src;
  }

  if (rotation == 180) {
    dst_width = src_width;
    dst_height = src_height;
    std::vector<uint8_t> dst(src.size());
    for (int y = 0; y < src_height; ++y) {
      for (int x = 0; x < src_width; ++x) {
        int src_idx = (y * src_width + x) * 4;
        int dst_idx = ((src_height - 1 - y) * src_width + (src_width - 1 - x)) * 4;
        dst[dst_idx] = src[src_idx];
        dst[dst_idx + 1] = src[src_idx + 1];
        dst[dst_idx + 2] = src[src_idx + 2];
        dst[dst_idx + 3] = src[src_idx + 3];
      }
    }
    return dst;
  }

  // 90 or 270 degrees - swap dimensions
  dst_width = src_height;
  dst_height = src_width;
  std::vector<uint8_t> dst(src.size());

  for (int y = 0; y < src_height; ++y) {
    for (int x = 0; x < src_width; ++x) {
      int src_idx = (y * src_width + x) * 4;
      int dst_x, dst_y;
      if (rotation == 90) {
        dst_x = src_height - 1 - y;
        dst_y = x;
      } else {  // 270
        dst_x = y;
        dst_y = src_width - 1 - x;
      }
      int dst_idx = (dst_y * dst_width + dst_x) * 4;
      dst[dst_idx] = src[src_idx];
      dst[dst_idx + 1] = src[src_idx + 1];
      dst[dst_idx + 2] = src[src_idx + 2];
      dst[dst_idx + 3] = src[src_idx + 3];
    }
  }
  return dst;
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

  // Priority: --scale > explicit --width/--height > --dpi (defaults to 150).
  // PDF user space is 72 pt per inch; output px = page_pt * dpi / 72.
  if (options.scale > 0) {
    output_width = static_cast<int>(page_width * options.scale);
    output_height = static_cast<int>(page_height * options.scale);
  } else if (options.width_set || options.height_set) {
    float aspect_ratio = page_width / page_height;
    if (options.width_set && !options.height_set) {
      output_height = static_cast<int>(output_width / aspect_ratio);
    } else if (options.height_set && !options.width_set) {
      output_width = static_cast<int>(output_height * aspect_ratio);
    }
    // If both are set, honor them verbatim (may distort aspect).
  } else {
    float dpi_scale = options.dpi / 72.0f;
    output_width = static_cast<int>(page_width * dpi_scale);
    output_height = static_cast<int>(page_height * dpi_scale);
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

  if (options.verbose) {
    backend.set_progress_callback(
        [page_num](const nanopdf::RenderProgressInfo& progress) {
          std::cout << "\r  Rendering page " << page_num << ": "
                    << std::setw(3) << progress.percent << "%" << std::flush;
          if (progress.percent == 100) {
            std::cout << "\n";
          }
        });
  } else {
    backend.clear_progress_callback();
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

  // Apply post-processing (rotation and/or grayscale) if needed
  if (options.rotation != 0 || options.grayscale) {
    auto buffer = backend.get_buffer();
    if (buffer.pixels.empty()) {
      std::cerr << "Error: Failed to get render buffer\n";
      return false;
    }

    std::vector<uint8_t> pixels = std::move(buffer.pixels);
    int final_width = buffer.width;
    int final_height = buffer.height;

    // Apply grayscale conversion first (before rotation to work on smaller data if rotated)
    if (options.grayscale) {
      if (options.verbose) {
        std::cout << "  Converting to grayscale...\n";
      }
      convert_to_grayscale(pixels);
    }

    // Apply rotation
    if (options.rotation != 0) {
      if (options.verbose) {
        std::cout << "  Rotating " << options.rotation << " degrees...\n";
      }
      pixels = rotate_pixels(pixels, final_width, final_height,
                            options.rotation, final_width, final_height);
    }

    // Save processed image using stb_image_write
    if (stbi_write_png(output_file.c_str(), final_width, final_height, 4,
                       pixels.data(), final_width * 4)) {
      if (options.verbose) {
        std::cout << "  Saved to: " << output_file << "\n";
      }
      return true;
    } else {
      std::cerr << "Error: Failed to save PNG file: " << output_file << "\n";
      return false;
    }
  }

  // No post-processing needed, save directly
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

  // Set log level from command line option
  nanopdf::log::set_log_level(static_cast<nanopdf::log::Level>(options.log_level));

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
  } else if (options.pages_spec_set) {
    std::string parse_error;
    if (!parse_pages_spec(options.pages_spec, pages_to_render, parse_error)) {
      std::cerr << "Error: invalid --pages spec: " << parse_error << "\n";
      return 1;
    }
    for (int page_num : pages_to_render) {
      if (page_num > total_pages) {
        std::cerr << "Error: Page " << page_num << " does not exist. ";
        std::cerr << "PDF has " << total_pages << " page(s).\n";
        return 1;
      }
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
  bool multi_page_output = (pages_to_render.size() > 1);
  for (int page_num : pages_to_render) {
    const auto& page = pdf.catalog.pages[page_num - 1];  // Convert to 0-based index

    std::string output_file = generate_output_filename(
      options.output_file, page_num, multi_page_output
    );

    if (options.verbose) {
      std::cout << "\nProcessing page " << page_num << " of " << total_pages << ":\n";
      std::cout << "  Page contents count: " << page.contents.size() << "\n";
      for (size_t i = 0; i < page.contents.size(); i++) {
        std::cout << "    Content " << i << " type: " << page.contents[i].type << "\n";
      }
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
