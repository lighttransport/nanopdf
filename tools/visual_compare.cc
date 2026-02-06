// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Standalone pixel comparison tool for visual regression testing.
// Compares two PNG images and reports RMSE, PSNR, max channel diff,
// and percentage of pixels exceeding a threshold.
//
// Exit codes: 0=PASS, 1=FAIL, 2=ERROR
//
// Usage:
//   visual_compare actual.png ref.png [--threshold N] [--max-diff-pct F]
//                                     [--save-diff diff.png]

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

struct CompareResult {
  bool valid{false};
  double rmse{0.0};
  double psnr{0.0};
  int max_channel_diff{0};
  double diff_pct{0.0};  // percentage of pixels exceeding threshold
  int width{0};
  int height{0};
};

CompareResult compare_images(const uint8_t* actual, int aw, int ah,
                             const uint8_t* ref, int rw, int rh,
                             int channels, int threshold) {
  CompareResult result;
  result.valid = true;
  int cmp_w = (aw < rw) ? aw : rw;
  int cmp_h = (ah < rh) ? ah : rh;
  result.width = cmp_w;
  result.height = cmp_h;

  size_t total_pixels = static_cast<size_t>(cmp_w) * cmp_h;
  int cmp_channels = (channels >= 3) ? 3 : channels;

  double sum_sq = 0.0;
  size_t exceeding = 0;

  for (int y = 0; y < cmp_h; ++y) {
    for (int x = 0; x < cmp_w; ++x) {
      size_t a_idx = static_cast<size_t>(y * aw + x);
      size_t r_idx = static_cast<size_t>(y * rw + x);
      int pixel_max_diff = 0;
      for (int c = 0; c < cmp_channels; ++c) {
        int diff = static_cast<int>(actual[a_idx * channels + c]) -
                   static_cast<int>(ref[r_idx * channels + c]);
        if (diff < 0) diff = -diff;
        double ddiff = static_cast<double>(diff);
        sum_sq += ddiff * ddiff;
        if (diff > pixel_max_diff) pixel_max_diff = diff;
        if (diff > result.max_channel_diff) result.max_channel_diff = diff;
      }
      if (pixel_max_diff > threshold) {
        ++exceeding;
      }
    }
  }

  double mse = sum_sq / (total_pixels * cmp_channels);
  result.rmse = std::sqrt(mse);
  if (mse > 0.0) {
    result.psnr = 10.0 * std::log10(255.0 * 255.0 / mse);
  } else {
    result.psnr = 999.0;  // identical
  }
  result.diff_pct = 100.0 * static_cast<double>(exceeding) / total_pixels;
  return result;
}

void save_diff_image(const uint8_t* actual, int aw, int ah,
                     const uint8_t* ref, int rw, int rh,
                     int channels, const std::string& path) {
  int cmp_w = (aw < rw) ? aw : rw;
  int cmp_h = (ah < rh) ? ah : rh;
  std::vector<uint8_t> diff(static_cast<size_t>(cmp_w) * cmp_h * 3);
  int cmp_channels = (channels >= 3) ? 3 : channels;

  for (int y = 0; y < cmp_h; ++y) {
    for (int x = 0; x < cmp_w; ++x) {
      size_t a_idx = static_cast<size_t>(y * aw + x);
      size_t r_idx = static_cast<size_t>(y * rw + x);
      size_t d_idx = static_cast<size_t>(y * cmp_w + x);
      for (int c = 0; c < 3; ++c) {
        if (c < cmp_channels) {
          int d = static_cast<int>(actual[a_idx * channels + c]) -
                  static_cast<int>(ref[r_idx * channels + c]);
          if (d < 0) d = -d;
          d *= 4;  // amplify
          if (d > 255) d = 255;
          diff[d_idx * 3 + c] = static_cast<uint8_t>(d);
        } else {
          diff[d_idx * 3 + c] = 0;
        }
      }
    }
  }

  stbi_write_png(path.c_str(), cmp_w, cmp_h, 3, diff.data(), cmp_w * 3);
}

void print_usage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " actual.png ref.png [options]\n"
            << "\nOptions:\n"
            << "  --threshold N      Per-channel diff threshold (default: 5)\n"
            << "  --max-diff-pct F   Max allowed percentage of pixels exceeding "
               "threshold (default: 5.0)\n"
            << "  --save-diff PATH   Save amplified diff image to PATH\n"
            << "\nExit codes: 0=PASS, 1=FAIL, 2=ERROR\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    print_usage(argv[0]);
    return 2;
  }

  std::string actual_path = argv[1];
  std::string ref_path = argv[2];
  int threshold = 5;
  double max_diff_pct = 5.0;
  std::string diff_path;

  // Parse optional arguments
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
      threshold = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--max-diff-pct") == 0 && i + 1 < argc) {
      max_diff_pct = std::atof(argv[++i]);
    } else if (std::strcmp(argv[i], "--save-diff") == 0 && i + 1 < argc) {
      diff_path = argv[++i];
    } else {
      std::cerr << "Unknown option: " << argv[i] << std::endl;
      print_usage(argv[0]);
      return 2;
    }
  }

  // Load images
  int aw, ah, ac;
  uint8_t* actual = stbi_load(actual_path.c_str(), &aw, &ah, &ac, 0);
  if (!actual) {
    std::cerr << "ERROR: Failed to load actual image: " << actual_path
              << std::endl;
    return 2;
  }

  int rw, rh, rc;
  uint8_t* ref = stbi_load(ref_path.c_str(), &rw, &rh, &rc, 0);
  if (!ref) {
    std::cerr << "ERROR: Failed to load reference image: " << ref_path
              << std::endl;
    stbi_image_free(actual);
    return 2;
  }

  // Check dimensions match (allow ±2 pixel tolerance for DPI rounding)
  int dw = aw - rw; if (dw < 0) dw = -dw;
  int dh = ah - rh; if (dh < 0) dh = -dh;
  if (dw > 2 || dh > 2) {
    std::cerr << "ERROR: Dimension mismatch too large: actual=" << aw << "x"
              << ah << " ref=" << rw << "x" << rh << std::endl;
    stbi_image_free(actual);
    stbi_image_free(ref);
    return 2;
  }
  if (aw != rw || ah != rh) {
    std::cerr << "Note: Dimensions differ slightly (actual=" << aw << "x" << ah
              << " ref=" << rw << "x" << rh << "), comparing overlap"
              << std::endl;
  }

  // If channel counts differ, reload both as RGB (3 channels)
  int channels = ac;
  if (ac != rc) {
    stbi_image_free(actual);
    stbi_image_free(ref);
    actual = stbi_load(actual_path.c_str(), &aw, &ah, &ac, 3);
    ref = stbi_load(ref_path.c_str(), &rw, &rh, &rc, 3);
    if (!actual || !ref) {
      std::cerr << "ERROR: Failed to reload images as RGB" << std::endl;
      if (actual) stbi_image_free(actual);
      if (ref) stbi_image_free(ref);
      return 2;
    }
    channels = 3;
  }

  // Compare
  CompareResult result = compare_images(actual, aw, ah, ref, rw, rh,
                                        channels, threshold);

  // Save diff if requested
  if (!diff_path.empty()) {
    save_diff_image(actual, aw, ah, ref, rw, rh, channels, diff_path);
  }

  // Report results
  std::cout << "Image comparison: " << result.width << "x" << result.height
            << " ("
            << channels << " channels)\n"
            << "  RMSE:            " << result.rmse << "\n"
            << "  PSNR:            " << result.psnr << " dB\n"
            << "  Max channel diff: " << result.max_channel_diff << "\n"
            << "  Pixels > threshold (" << threshold << "): "
            << result.diff_pct << "%\n"
            << "  Max allowed diff%%: " << max_diff_pct << "%\n";

  stbi_image_free(actual);
  stbi_image_free(ref);

  // Pass/fail decision
  if (result.diff_pct > max_diff_pct) {
    std::cout << "RESULT: FAIL (diff " << result.diff_pct
              << "% > max " << max_diff_pct << "%)\n";
    return 1;
  }

  std::cout << "RESULT: PASS\n";
  return 0;
}
