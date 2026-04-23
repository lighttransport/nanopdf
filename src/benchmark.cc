// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Benchmark test for nanopdf PDF parsing and rendering performance

#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "nanopdf.hh"
#include "string-parse.hh"

#ifdef NANOPDF_USE_THORVG
#include "thorvg-backend.hh"
#endif

#ifdef NANOPDF_USE_BLEND2D
#include "blend2d-backend.hh"
#endif

using namespace nanopdf;

// Timing utilities
struct TimingStats {
  double min_ms{0};
  double max_ms{0};
  double avg_ms{0};
  double std_ms{0};
  double total_ms{0};
  int iterations{0};
};

TimingStats calculate_stats(const std::vector<double>& times_ms) {
  TimingStats stats;
  if (times_ms.empty()) return stats;

  stats.iterations = static_cast<int>(times_ms.size());
  stats.min_ms = *std::min_element(times_ms.begin(), times_ms.end());
  stats.max_ms = *std::max_element(times_ms.begin(), times_ms.end());
  stats.total_ms = std::accumulate(times_ms.begin(), times_ms.end(), 0.0);
  stats.avg_ms = stats.total_ms / times_ms.size();

  // Calculate standard deviation
  double variance = 0.0;
  for (double t : times_ms) {
    variance += (t - stats.avg_ms) * (t - stats.avg_ms);
  }
  stats.std_ms = std::sqrt(variance / times_ms.size());

  return stats;
}

void print_stats(const std::string& name, const TimingStats& stats) {
  std::cout << "  " << std::left << std::setw(30) << name << ": "
            << std::fixed << std::setprecision(3)
            << "avg=" << std::setw(10) << stats.avg_ms << " ms, "
            << "min=" << std::setw(10) << stats.min_ms << " ms, "
            << "max=" << std::setw(10) << stats.max_ms << " ms, "
            << "std=" << std::setw(8) << stats.std_ms << " ms"
            << " (" << stats.iterations << " iterations)"
            << std::endl;
}

void print_separator() {
  std::cout << std::string(100, '-') << std::endl;
}

// Benchmark: PDF Loading
TimingStats benchmark_pdf_loading(const std::string& pdf_path, int iterations) {
  std::vector<double> times;
  times.reserve(iterations);

  // Read file into memory once
  std::ifstream file(pdf_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    std::cerr << "Failed to open PDF file: " << pdf_path << std::endl;
    return {};
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<char> buffer(size);
  if (!file.read(buffer.data(), size)) {
    std::cerr << "Failed to read PDF file" << std::endl;
    return {};
  }
  file.close();

  for (int i = 0; i < iterations; i++) {
    auto start = std::chrono::high_resolution_clock::now();

    Pdf pdf;
    bool success = parse_from_memory(reinterpret_cast<const uint8_t*>(buffer.data()),
                                     buffer.size(), &pdf);

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();

    if (success) {
      times.push_back(elapsed_ms);
    }
  }

  return calculate_stats(times);
}

// Benchmark: Page Content Decoding
TimingStats benchmark_page_decoding(const Pdf& pdf, int page_idx, int iterations) {
  std::vector<double> times;
  times.reserve(iterations);

  pdf.ensure_pages_loaded();
  if (page_idx >= static_cast<int>(pdf.catalog.pages.size())) {
    return {};
  }

  const Page& page = pdf.catalog.pages[page_idx];

  for (int i = 0; i < iterations; i++) {
    auto start = std::chrono::high_resolution_clock::now();

    // Decode all content streams
    for (const auto& content : page.contents) {
      if (content.type == Value::STREAM) {
        auto decoded = decode_stream(pdf, content);
        (void)decoded;  // Prevent optimization
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    times.push_back(elapsed_ms);
  }

  return calculate_stats(times);
}

// Benchmark: Text Extraction
TimingStats benchmark_text_extraction(const Pdf& pdf, int page_idx, int iterations) {
  std::vector<double> times;
  times.reserve(iterations);

  pdf.ensure_pages_loaded();
  if (page_idx >= static_cast<int>(pdf.catalog.pages.size())) {
    return {};
  }

  const Page& page = pdf.catalog.pages[page_idx];

  for (int i = 0; i < iterations; i++) {
    auto start = std::chrono::high_resolution_clock::now();

    std::string text = extract_text_from_page(pdf, page);
    (void)text;  // Prevent optimization

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    times.push_back(elapsed_ms);
  }

  return calculate_stats(times);
}

#ifdef NANOPDF_USE_THORVG
// Benchmark: ThorVG Rendering
TimingStats benchmark_thorvg_rendering(const Pdf& pdf, int page_idx, int iterations,
                                        uint32_t width = 800, uint32_t height = 600) {
  std::vector<double> times;
  times.reserve(iterations);

  pdf.ensure_pages_loaded();
  if (page_idx >= static_cast<int>(pdf.catalog.pages.size())) {
    return {};
  }

  const Page& page = pdf.catalog.pages[page_idx];

  // Calculate dimensions from page MediaBox
  float page_width = 612.0f;   // Default letter size
  float page_height = 792.0f;
  if (page.media_box.size() >= 4) {
    page_width = static_cast<float>(page.media_box[2] - page.media_box[0]);
    page_height = static_cast<float>(page.media_box[3] - page.media_box[1]);
  }

  // Scale to fit
  float scale = std::min(width / page_width, height / page_height);
  uint32_t render_width = static_cast<uint32_t>(page_width * scale);
  uint32_t render_height = static_cast<uint32_t>(page_height * scale);

  for (int i = 0; i < iterations; i++) {
    ThorVGBackend backend;

    auto start = std::chrono::high_resolution_clock::now();

    if (backend.initialize(render_width, render_height)) {
      auto result = backend.render_page(pdf, page);
      (void)result;  // Prevent optimization
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    times.push_back(elapsed_ms);
  }

  return calculate_stats(times);
}
#endif

#ifdef NANOPDF_USE_BLEND2D
// Benchmark: Blend2D Rendering
TimingStats benchmark_blend2d_rendering(const Pdf& pdf, int page_idx, int iterations,
                                         uint32_t width = 800, uint32_t height = 600) {
  std::vector<double> times;
  times.reserve(iterations);

  pdf.ensure_pages_loaded();
  if (page_idx >= static_cast<int>(pdf.catalog.pages.size())) {
    return {};
  }

  const Page& page = pdf.catalog.pages[page_idx];

  // Calculate dimensions from page MediaBox
  float page_width = 612.0f;   // Default letter size
  float page_height = 792.0f;
  if (page.media_box.size() >= 4) {
    page_width = static_cast<float>(page.media_box[2] - page.media_box[0]);
    page_height = static_cast<float>(page.media_box[3] - page.media_box[1]);
  }

  // Scale to fit
  float scale = std::min(width / page_width, height / page_height);
  uint32_t render_width = static_cast<uint32_t>(page_width * scale);
  uint32_t render_height = static_cast<uint32_t>(page_height * scale);

  for (int i = 0; i < iterations; i++) {
    Blend2DBackend backend;

    auto start = std::chrono::high_resolution_clock::now();

    if (backend.initialize(render_width, render_height)) {
      auto result = backend.render_page(pdf, page);
      (void)result;  // Prevent optimization
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    times.push_back(elapsed_ms);
  }

  return calculate_stats(times);
}
#endif

void print_usage(const char* program) {
  std::cout << "Usage: " << program << " [options] <pdf_file>" << std::endl;
  std::cout << "\nOptions:" << std::endl;
  std::cout << "  -i, --iterations N    Number of iterations (default: 10)" << std::endl;
  std::cout << "  -w, --width N         Render width in pixels (default: 800)" << std::endl;
  std::cout << "  -h, --height N        Render height in pixels (default: 600)" << std::endl;
  std::cout << "  -p, --page N          Page index to benchmark (default: 0)" << std::endl;
  std::cout << "  --all-pages           Benchmark all pages" << std::endl;
  std::cout << "  --help                Show this help message" << std::endl;
}

int main(int argc, char** argv) {
  // Default parameters
  int iterations = 10;
  uint32_t width = 800;
  uint32_t height = 600;
  int page_idx = 0;
  bool all_pages = false;
  std::string pdf_path;

  // Parse arguments
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-i" || arg == "--iterations") {
      if (i + 1 < argc) iterations = nanopdf::stoi_or(argv[++i]);
    } else if (arg == "-w" || arg == "--width") {
      if (i + 1 < argc) width = static_cast<uint32_t>(nanopdf::stoi_or(argv[++i]));
    } else if (arg == "-h" || arg == "--height") {
      if (i + 1 < argc) height = static_cast<uint32_t>(nanopdf::stoi_or(argv[++i]));
    } else if (arg == "-p" || arg == "--page") {
      if (i + 1 < argc) page_idx = nanopdf::stoi_or(argv[++i]);
    } else if (arg == "--all-pages") {
      all_pages = true;
    } else if (arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg[0] != '-') {
      pdf_path = arg;
    }
  }

  if (pdf_path.empty()) {
    std::cerr << "Error: No PDF file specified" << std::endl;
    print_usage(argv[0]);
    return 1;
  }

  std::cout << "========================================" << std::endl;
  std::cout << "       nanopdf Benchmark Suite" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << std::endl;

  std::cout << "Configuration:" << std::endl;
  std::cout << "  PDF file:    " << pdf_path << std::endl;
  std::cout << "  Iterations:  " << iterations << std::endl;
  std::cout << "  Render size: " << width << "x" << height << std::endl;
  std::cout << "  Backends:    ";
#ifdef NANOPDF_USE_THORVG
  std::cout << "ThorVG ";
#endif
#ifdef NANOPDF_USE_BLEND2D
  std::cout << "Blend2D ";
#endif
#if !defined(NANOPDF_USE_THORVG) && !defined(NANOPDF_USE_BLEND2D)
  std::cout << "(none)";
#endif
  std::cout << std::endl;
  std::cout << std::endl;

  // Warmup: Load PDF once
  std::ifstream file(pdf_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    std::cerr << "Error: Failed to open PDF file: " << pdf_path << std::endl;
    return 1;
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<char> buffer(size);
  if (!file.read(buffer.data(), size)) {
    std::cerr << "Error: Failed to read PDF file" << std::endl;
    return 1;
  }
  file.close();

  Pdf pdf;
  if (!parse_from_memory(reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size(), &pdf)) {
    std::cerr << "Error: Failed to parse PDF file" << std::endl;
    return 1;
  }

  pdf.ensure_pages_loaded();
  int num_pages = static_cast<int>(pdf.catalog.pages.size());
  std::cout << "PDF Info:" << std::endl;
  std::cout << "  File size:   " << size << " bytes" << std::endl;
  std::cout << "  Pages:       " << num_pages << std::endl;
  std::cout << std::endl;

  // Determine which pages to benchmark
  std::vector<int> pages_to_benchmark;
  if (all_pages) {
    for (int i = 0; i < num_pages; i++) {
      pages_to_benchmark.push_back(i);
    }
  } else {
    if (page_idx >= num_pages) {
      std::cerr << "Error: Page index " << page_idx << " out of range (0-" << num_pages - 1 << ")" << std::endl;
      return 1;
    }
    pages_to_benchmark.push_back(page_idx);
  }

  print_separator();
  std::cout << "BENCHMARK: PDF Loading" << std::endl;
  print_separator();

  auto load_stats = benchmark_pdf_loading(pdf_path, iterations);
  print_stats("PDF parse_from_memory()", load_stats);
  std::cout << std::endl;

  // Per-page benchmarks
  for (int pidx : pages_to_benchmark) {
    print_separator();
    std::cout << "BENCHMARK: Page " << pidx << std::endl;
    print_separator();

    const Page& page = pdf.catalog.pages[pidx];

    // Print page info
    float page_width = 612.0f;
    float page_height = 792.0f;
    if (page.media_box.size() >= 4) {
      page_width = static_cast<float>(page.media_box[2] - page.media_box[0]);
      page_height = static_cast<float>(page.media_box[3] - page.media_box[1]);
    }
    std::cout << "  Page size: " << page_width << " x " << page_height << " pts" << std::endl;
    std::cout << "  Fonts: " << page.fonts.size() << std::endl;
    std::cout << "  Content streams: " << page.contents.size() << std::endl;
    std::cout << std::endl;

    // Content decoding benchmark
    auto decode_stats = benchmark_page_decoding(pdf, pidx, iterations);
    print_stats("Content stream decoding", decode_stats);

    // Text extraction benchmark
    auto text_stats = benchmark_text_extraction(pdf, pidx, iterations);
    print_stats("Text extraction", text_stats);

#ifdef NANOPDF_USE_THORVG
    // ThorVG rendering benchmark
    auto thorvg_stats = benchmark_thorvg_rendering(pdf, pidx, iterations, width, height);
    print_stats("ThorVG rendering", thorvg_stats);
#endif

#ifdef NANOPDF_USE_BLEND2D
    // Blend2D rendering benchmark
    auto blend2d_stats = benchmark_blend2d_rendering(pdf, pidx, iterations, width, height);
    print_stats("Blend2D rendering", blend2d_stats);
#endif

    std::cout << std::endl;
  }

  // Summary
  print_separator();
  std::cout << "SUMMARY" << std::endl;
  print_separator();

#if defined(NANOPDF_USE_THORVG) && defined(NANOPDF_USE_BLEND2D)
  if (!pages_to_benchmark.empty()) {
    // Compare backends
    double thorvg_total = 0, blend2d_total = 0;
    for (int pidx : pages_to_benchmark) {
      auto thorvg_stats = benchmark_thorvg_rendering(pdf, pidx, 1, width, height);
      auto blend2d_stats = benchmark_blend2d_rendering(pdf, pidx, 1, width, height);
      thorvg_total += thorvg_stats.avg_ms;
      blend2d_total += blend2d_stats.avg_ms;
    }

    std::cout << "  Backend comparison (single iteration per page):" << std::endl;
    std::cout << "    ThorVG total:  " << std::fixed << std::setprecision(3) << thorvg_total << " ms" << std::endl;
    std::cout << "    Blend2D total: " << std::fixed << std::setprecision(3) << blend2d_total << " ms" << std::endl;

    if (blend2d_total > 0 && thorvg_total > 0) {
      double ratio = thorvg_total / blend2d_total;
      if (ratio > 1.0) {
        std::cout << "    Blend2D is " << std::fixed << std::setprecision(2) << ratio << "x faster" << std::endl;
      } else {
        std::cout << "    ThorVG is " << std::fixed << std::setprecision(2) << (1.0 / ratio) << "x faster" << std::endl;
      }
    }
  }
#endif

  std::cout << std::endl;
  std::cout << "Benchmark complete." << std::endl;

  return 0;
}
