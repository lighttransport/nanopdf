// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// PDF rasterizer using nanopdf
//
// Backends:
//   lightvg  — lightui VG software rasterizer (default, no external deps)
//   thorvg   — ThorVG (requires nanopdf built with -DNANOPDF_USE_THORVG=ON)
//
// Usage: rasterize <input.pdf> <output> [options]
//
// Options:
//   --backend <name>   Renderer: lightvg (default), thorvg, or list
//   -p, --page <n>     Page number to render (default: 1)
//   --pages <spec>     Page selection (e.g. 1-3,7,10-12)
//   -w, --width <n>    Output width in pixels (preserves aspect if height omitted)
//   -h, --height <n>   Output height in pixels (preserves aspect if width omitted)
//   -s, --scale <f>    Scale factor (overrides width/height)
//   --dpi <n>          DPI for rendering (default: 300)
//   -r, --rotate <n>   Rotation angle: 0, 90, 180, 270 (default: 0)
//   -g, --grayscale    Convert output to grayscale
//   --format <name>    Output format: png, jpg, bmp, or tga
//   --jpeg-quality <n> JPEG quality 1-100 (default: 90)
//   --png-compression <n> PNG compression level 0-9 (default: 1)
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
#include <cmath>
#include <iomanip>
#include <sstream>
#include <set>
#include <memory>

#include "../../src/render-backend.hh"
#include "../../src/nanopdf.hh"
#include "../../src/nanopdf-log.hh"

#include "../../src/third_party/stb_image_write.h"

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
  // Default DPI for clearer text in equations and small symbols.
  float dpi = 300.0f;
  bool width_set = false;
  bool height_set = false;
  int rotation = 0;    // 0, 90, 180, 270 degrees
  bool grayscale = false;
  nanopdf::RenderOptions::Format output_format = nanopdf::RenderOptions::Format::PNG;
  int jpeg_quality = 90;
  int png_compression = 1;
  bool fast_png = false;
  bool list_backends = false;
  bool render_all_pages = false;
  bool verbose = false;
  int log_level = 3;  // Default: Info
  nanopdf::BackendKind backend = nanopdf::BackendKind::LightVG;
};

void print_usage(const char* program_name) {
  std::cout << "PDF Rasterizer using nanopdf\n";
  std::cout << "\n";
  std::cout << "Usage: " << program_name << " <input.pdf> <output> [options]\n";
  std::cout << "\n";
  std::cout << "Options:\n";
  std::cout << "  --backend <name>   Renderer: lightvg (default), thorvg, or list\n";
  std::cout << "  -p, --page <n>     Page number to render (default: 1)\n";
  std::cout << "  --pages <spec>     Page selection (e.g. 1-3,7,10-12)\n";
  std::cout << "  -w, --width <n>    Output width in pixels (preserves aspect if height omitted)\n";
  std::cout << "  -h, --height <n>   Output height in pixels (preserves aspect if width omitted)\n";
  std::cout << "  -s, --scale <f>    Scale factor (overrides width/height)\n";
  std::cout << "  --dpi <n>          DPI for rendering (default: 300)\n";
  std::cout << "  -r, --rotate <n>   Rotation angle: 0, 90, 180, 270 (default: 0)\n";
  std::cout << "  -g, --grayscale    Convert output to grayscale\n";
  std::cout << "  --format <name>    Output format: png, jpg, bmp, or tga (default: png)\n";
  std::cout << "  --jpeg-quality <n> JPEG quality 1-100 (default: 90)\n";
  std::cout << "  --png-compression <n> PNG compression level 0-9 (default: 1)\n";
  std::cout << "  --fast-png         Fast fpnge PNG encoder (~5-10x faster, larger files)\n";
  std::cout << "  --all              Render all pages (creates multiple PNG files)\n";
  std::cout << "  --verbose          Verbose output\n";
  std::cout << "  --log-level <n>    Log level: 0=none, 1=error, 2=warn, 3=info, 4=debug, 5=trace\n";
  std::cout << "  --help             Show this help message\n";
  std::cout << "\n";
  std::cout << "Examples:\n";
  std::cout << "  " << program_name << " document.pdf output.png\n";
  std::cout << "  " << program_name << " document.pdf output.png -p 2 -w 1024 -h 768\n";
  std::cout << "  " << program_name << " document.pdf output.png --pages 1-3,7,10\n";
  std::cout << "  " << program_name << " document.pdf output.png --all --dpi 300\n";
  std::cout << "  " << program_name << " document.pdf output.png -s 2.0\n";
  std::cout << "  " << program_name << " document.pdf output.png -r 90 --grayscale\n";
  std::cout << "  " << program_name << " document.pdf output.jpg --format jpg --jpeg-quality 85\n";
}

void print_available_backends() {
  std::cout << "Available backends:\n";
  std::cout << "  lightvg: "
            << (nanopdf::backend_available(nanopdf::BackendKind::LightVG)
                    ? "enabled" : "not compiled")
            << "\n";
  std::cout << "  thorvg:  "
            << (nanopdf::backend_available(nanopdf::BackendKind::ThorVG)
                    ? "enabled" : "not compiled")
            << "\n";
}

static bool parse_output_format(const std::string& name,
                                nanopdf::RenderOptions::Format& out) {
  std::string lower;
  lower.reserve(name.size());
  for (char c : name) {
    lower.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "png") {
    out = nanopdf::RenderOptions::Format::PNG;
    return true;
  }
  if (lower == "jpg" || lower == "jpeg") {
    out = nanopdf::RenderOptions::Format::JPEG;
    return true;
  }
  if (lower == "bmp") {
    out = nanopdf::RenderOptions::Format::BMP;
    return true;
  }
  if (lower == "tga") {
    out = nanopdf::RenderOptions::Format::TGA;
    return true;
  }
  return false;
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
    } else if (arg == "--format" && i + 1 < argc) {
      std::string format_name = argv[++i];
      if (!parse_output_format(format_name, options.output_format)) {
        std::cerr << "Error: Format must be png, jpg, jpeg, bmp, or tga\n";
        return false;
      }
    } else if (arg == "--jpeg-quality" && i + 1 < argc) {
      options.jpeg_quality = std::atoi(argv[++i]);
      if (options.jpeg_quality < 1 || options.jpeg_quality > 100) {
        std::cerr << "Error: JPEG quality must be 1-100\n";
        return false;
      }
    } else if (arg == "--png-compression" && i + 1 < argc) {
      options.png_compression = std::atoi(argv[++i]);
      if (options.png_compression < 0 || options.png_compression > 9) {
        std::cerr << "Error: PNG compression level must be 0-9\n";
        return false;
      }
    } else if (arg == "--fast-png") {
      options.fast_png = true;
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
    } else if (arg == "--backend" && i + 1 < argc) {
      std::string name = argv[++i];
      if (name == "list") {
        options.list_backends = true;
        continue;
      }
      if (!nanopdf::parse_backend_kind(name, &options.backend)) {
        std::cerr << "Error: Unknown backend: " << name
                  << " (expected: lightvg, thorvg, list)\n";
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

bool save_rgba_pixels(const std::string& filename,
                      const std::vector<uint8_t>& pixels,
                      int width, int height,
                      const RasterizeOptions& options) {
  if (width <= 0 || height <= 0 || pixels.empty()) return false;

  switch (options.output_format) {
    case nanopdf::RenderOptions::Format::PNG:
      stbi_write_png_compression_level = options.png_compression;
      return stbi_write_png(filename.c_str(), width, height, 4,
                            pixels.data(), width * 4) != 0;
    case nanopdf::RenderOptions::Format::JPEG: {
      std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
      for (int i = 0; i < width * height; ++i) {
        size_t rgb_idx = static_cast<size_t>(i) * 3;
        size_t rgba_idx = static_cast<size_t>(i) * 4;
        rgb[rgb_idx + 0] = pixels[rgba_idx + 0];
        rgb[rgb_idx + 1] = pixels[rgba_idx + 1];
        rgb[rgb_idx + 2] = pixels[rgba_idx + 2];
      }
      return stbi_write_jpg(filename.c_str(), width, height, 3,
                            rgb.data(), options.jpeg_quality) != 0;
    }
    case nanopdf::RenderOptions::Format::BMP:
      return stbi_write_bmp(filename.c_str(), width, height, 4,
                            pixels.data()) != 0;
    case nanopdf::RenderOptions::Format::TGA:
      return stbi_write_tga(filename.c_str(), width, height, 4,
                            pixels.data()) != 0;
  }
  return false;
}

static int normalized_page_rotation(const nanopdf::Page& page) {
  int rotation = static_cast<int>(page.rotate) % 360;
  if (rotation < 0) rotation += 360;
  if (rotation == 90 || rotation == 180 || rotation == 270) {
    return rotation;
  }
  return 0;
}

bool render_page(const nanopdf::Pdf& pdf, const nanopdf::Page& page, int page_num,
                const RasterizeOptions& options, const std::string& output_file) {
  // Get page dimensions from media_box [left, bottom, right, top]
  double page_width = 612.0;  // Default US Letter
  double page_height = 792.0;
  if (page.media_box.size() >= 4) {
    page_width = page.media_box[2] - page.media_box[0];
    page_height = page.media_box[3] - page.media_box[1];
  }

  const int page_rotation = normalized_page_rotation(page);
  const bool page_swaps_axes =
      (page_rotation == 90 || page_rotation == 270);
  const double visible_page_width = page_swaps_axes ? page_height : page_width;
  const double visible_page_height = page_swaps_axes ? page_width : page_height;

  if (options.verbose) {
    std::cout << "  Page " << page_num << " dimensions: "
              << page_width << " x " << page_height << " pts\n";
    if (page_rotation != 0) {
      std::cout << "  Page rotation: " << page_rotation
                << " degrees, visible dimensions: " << visible_page_width
                << " x " << visible_page_height << " pts\n";
    }
  }

  // Calculate final visible output dimensions. The backend renders into the
  // unrotated page coordinate system, then applies page /Rotate to pixels.
  int final_output_width = options.width;
  int final_output_height = options.height;

  // Priority: --scale > explicit --width/--height > --dpi (defaults to 150).
  // PDF user space is 72 pt per inch; output px = page_pt * dpi / 72.
  if (options.scale > 0) {
    final_output_width = static_cast<int>(std::ceil(visible_page_width * options.scale));
    final_output_height = static_cast<int>(std::ceil(visible_page_height * options.scale));
  } else if (options.width_set || options.height_set) {
    float aspect_ratio = static_cast<float>(visible_page_width / visible_page_height);
    if (options.width_set && !options.height_set) {
      final_output_height = static_cast<int>(std::ceil(final_output_width / aspect_ratio));
    } else if (options.height_set && !options.width_set) {
      final_output_width = static_cast<int>(std::ceil(final_output_height * aspect_ratio));
    }
    // If both are set, honor the canvas size verbatim.
  } else {
    float dpi_scale = options.dpi / 72.0f;
    final_output_width = static_cast<int>(std::ceil(visible_page_width * dpi_scale));
    final_output_height = static_cast<int>(std::ceil(visible_page_height * dpi_scale));
  }

  int output_width = final_output_width;
  int output_height = final_output_height;
  if (page_swaps_axes) {
    output_width = final_output_height;
    output_height = final_output_width;
  }

  if (options.verbose) {
    std::cout << "  Render canvas dimensions: " << output_width << " x "
              << output_height << " px\n";
    if (page_rotation != 0 || options.rotation != 0) {
      std::cout << "  Final output dimensions: ";
      int final_w = final_output_width;
      int final_h = final_output_height;
      if (options.rotation == 90 || options.rotation == 270) {
        std::swap(final_w, final_h);
      }
      std::cout << final_w << " x " << final_h << " px\n";
    }
  }

  // Create selected backend.
  auto backend = nanopdf::make_backend(options.backend);
  if (!backend) {
    std::cerr << "Error: Backend '" << nanopdf::backend_kind_name(options.backend)
              << "' is not compiled into this build of nanopdf.\n";
    if (options.backend == nanopdf::BackendKind::ThorVG) {
      std::cerr << "  Rebuild nanopdf with -DNANOPDF_USE_THORVG=ON to enable it.\n";
    } else if (options.backend == nanopdf::BackendKind::LightVG) {
      std::cerr << "  Rebuild nanopdf with -DNANOPDF_USE_LIGHTVG=ON to enable it.\n";
    }
    return false;
  }

  if (!backend->initialize(output_width, output_height)) {
    std::cerr << "Error: Failed to initialize "
              << nanopdf::backend_kind_name(options.backend) << " backend\n";
    return false;
  }

  if (options.verbose) {
    backend->set_progress_callback(
        [page_num](const nanopdf::RenderProgressInfo& progress) -> bool {
          std::cout << "\r  Rendering page " << page_num << ": "
                    << std::setw(3) << progress.percent << "%" << std::flush;
          if (progress.percent == 100) {
            std::cout << "\n";
          }
          return true;  // never interrupt from the CLI
        });
  } else {
    backend->clear_progress_callback();
  }

  // Render the page
  if (options.verbose) {
    std::cout << "  Rendering page " << page_num
              << " with backend '" << nanopdf::backend_kind_name(options.backend)
              << "'...\n";
  }

  int total_rotation = (page_rotation + options.rotation) % 360;
  if (total_rotation < 0) total_rotation += 360;
  const bool direct_backend_save =
      !options.grayscale &&
      (total_rotation == 0 || backend->kind() == nanopdf::BackendKind::LightVG);
  const bool direct_tga_save =
      direct_backend_save &&
      total_rotation == 0 &&
      options.output_format == nanopdf::RenderOptions::Format::TGA;
  backend->set_render_result_pixels_enabled(!direct_backend_save);
  backend->set_direct_bgra_output_enabled(direct_tga_save);
  auto result = backend->render_page(pdf, page);
  backend->set_render_result_pixels_enabled(true);
  if (!result.success) {
    backend->set_direct_bgra_output_enabled(false);
    std::cerr << "Error: Failed to render page " << page_num << ": " << result.error << "\n";
    return false;
  }

  if (direct_backend_save) {
    nanopdf::RenderOptions save_options;
    save_options.format = options.output_format;
    save_options.jpeg_quality = options.jpeg_quality;
    save_options.png_compression = options.png_compression;
    save_options.fast_png = options.fast_png;
    bool saved = false;
    if (total_rotation == 0) {
      saved = backend->save_to_file(output_file, save_options);
    } else {
      saved = backend->save_to_file_rotated(output_file, save_options,
                                            total_rotation);
    }
    backend->set_direct_bgra_output_enabled(false);
    if (saved) {
      if (options.verbose) {
        std::cout << "  Saved to: " << output_file << "\n";
      }
      return true;
    }
    std::cerr << "Error: Failed to save output file: " << output_file << "\n";
    return false;
  }

  // Apply post-processing (rotation and/or grayscale) if needed
  if (!direct_backend_save) {
    auto buffer = backend->get_buffer();
    if (buffer.pixels.empty()) {
      std::cerr << "Error: Failed to get render buffer\n";
      return false;
    }

    std::vector<uint8_t> pixels = std::move(buffer.pixels);
    int final_width = buffer.width;
    int final_height = buffer.height;

    // Apply the PDF page dictionary's /Rotate first. The no-options backend
    // render path draws the unrotated page into the canvas; the CLI owns the
    // visible-page orientation when it retrieves pixels for post-processing.
    if (page_rotation != 0) {
      if (options.verbose) {
        std::cout << "  Applying page rotation " << page_rotation << " degrees...\n";
      }
      pixels = rotate_pixels(pixels, final_width, final_height,
                             page_rotation, final_width, final_height);
    }

    // Apply grayscale conversion before any caller-requested extra rotation.
    if (options.grayscale) {
      if (options.verbose) {
        std::cout << "  Converting to grayscale...\n";
      }
      convert_to_grayscale(pixels);
    }

    // Apply caller-requested extra rotation.
    if (options.rotation != 0) {
      if (options.verbose) {
        std::cout << "  Rotating " << options.rotation << " degrees...\n";
      }
      pixels = rotate_pixels(pixels, final_width, final_height,
                            options.rotation, final_width, final_height);
    }

    if (save_rgba_pixels(output_file, pixels, final_width, final_height, options)) {
      if (options.verbose) {
        std::cout << "  Saved to: " << output_file << "\n";
      }
      return true;
    } else {
      std::cerr << "Error: Failed to save output file: " << output_file << "\n";
      return false;
    }
  }

  return false;
}

int main(int argc, char* argv[]) {
  RasterizeOptions options;

  if (argc == 2) {
    std::string arg = argv[1];
    if (arg == "--help" || arg == "-?") {
      print_usage(argv[0]);
      return 0;
    }
  }
  if (argc == 3 && std::string(argv[1]) == "--backend" &&
      std::string(argv[2]) == "list") {
    print_available_backends();
    return 0;
  }

  // Parse command line arguments
  if (!parse_arguments(argc, argv, options)) {
    print_usage(argv[0]);
    return 1;
  }
  if (options.list_backends) {
    print_available_backends();
    return 0;
  }

  // Set log level from command line option
  nanopdf::log::set_log_level(static_cast<nanopdf::log::Level>(options.log_level));

  // Verify that the requested backend is compiled in.
  if (!nanopdf::backend_available(options.backend)) {
    std::cerr << "Error: Backend '" << nanopdf::backend_kind_name(options.backend)
              << "' is not compiled into this build of nanopdf.\n";
    if (options.backend == nanopdf::BackendKind::ThorVG) {
      std::cerr << "  Rebuild nanopdf with -DNANOPDF_USE_THORVG=ON to enable it.\n";
    } else if (options.backend == nanopdf::BackendKind::LightVG) {
      std::cerr << "  Rebuild nanopdf with -DNANOPDF_USE_LIGHTVG=ON to enable it.\n";
    }
    return 1;
  }

  if (options.verbose) {
    std::cout << "PDF to PNG Rasterizer (backend: "
              << nanopdf::backend_kind_name(options.backend) << ")\n";
    std::cout << "Input: " << options.input_file << "\n";
    std::cout << "Output: " << options.output_file << "\n";
  }

  // Read PDF file
  std::ifstream ifs(options.input_file, std::ios::binary | std::ios::ate);
  if (!ifs) {
    std::cerr << "Error: Failed to open PDF file: " << options.input_file << "\n";
    return 1;
  }

  std::streamoff file_size = ifs.tellg();
  if (file_size < 0) {
    std::cerr << "Error: Failed to determine PDF file size: " << options.input_file << "\n";
    return 1;
  }

  std::vector<uint8_t> pdf_data(static_cast<size_t>(file_size));
  ifs.seekg(0, std::ios::beg);
  if (!pdf_data.empty()) {
    ifs.read(reinterpret_cast<char*>(pdf_data.data()),
             static_cast<std::streamsize>(pdf_data.size()));
    if (ifs.gcount() != static_cast<std::streamsize>(pdf_data.size())) {
      std::cerr << "Error: Failed to read PDF file: " << options.input_file << "\n";
      return 1;
    }
  }
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
