// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: Apache 2.0
//
// Custom color space transformation implementation
// Lightweight alternative to LCMS2 for common PDF color spaces

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nanopdf {

// Forward declarations
struct ColorSpace;

namespace color {

// RGB color in 0-1 range
struct RGB {
  float r{0.0f};
  float g{0.0f};
  float b{0.0f};

  RGB() = default;
  RGB(float r_, float g_, float b_) : r(r_), g(g_), b(b_) {}
};

// XYZ color (CIE 1931)
struct XYZ {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};

  XYZ() = default;
  XYZ(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

// Lab color (CIE L*a*b*)
struct Lab {
  float L{0.0f};  // Lightness: 0-100
  float a{0.0f};  // Green-Red: -128 to 127
  float b{0.0f};  // Blue-Yellow: -128 to 127

  Lab() = default;
  Lab(float L_, float a_, float b_) : L(L_), a(a_), b(b_) {}
};

// ICC color space signatures
constexpr uint32_t ICC_SIG_GRAY = 0x47524159;  // 'GRAY'
constexpr uint32_t ICC_SIG_RGB = 0x52474220;   // 'RGB '
constexpr uint32_t ICC_SIG_CMYK = 0x434D594B;  // 'CMYK'
constexpr uint32_t ICC_SIG_LAB = 0x4C616220;   // 'Lab '
constexpr uint32_t ICC_SIG_XYZ = 0x58595A20;   // 'XYZ '

// ICC tag signatures
constexpr uint32_t ICC_TAG_rXYZ = 0x7258595A;  // Red colorant XYZ
constexpr uint32_t ICC_TAG_gXYZ = 0x6758595A;  // Green colorant XYZ
constexpr uint32_t ICC_TAG_bXYZ = 0x6258595A;  // Blue colorant XYZ
constexpr uint32_t ICC_TAG_rTRC = 0x72545243;  // Red TRC (gamma)
constexpr uint32_t ICC_TAG_gTRC = 0x67545243;  // Green TRC (gamma)
constexpr uint32_t ICC_TAG_bTRC = 0x62545243;  // Blue TRC (gamma)
constexpr uint32_t ICC_TAG_kTRC = 0x6B545243;  // Gray TRC
constexpr uint32_t ICC_TAG_wtpt = 0x77747074;  // White point

// TRC (Tone Reproduction Curve) representation
struct IccTRC {
  bool valid{false};
  bool is_gamma{false};    // True if simple gamma, false if curve table
  float gamma{1.0f};       // Simple gamma value
  std::vector<float> curve;  // Curve table (normalized 0-1)

  // Apply TRC to a value
  float apply(float v) const;
  // Apply inverse TRC
  float apply_inverse(float v) const;
};

// Simple ICC profile header parsing (minimal)
struct IccProfileInfo {
  bool valid{false};
  std::string error;

  // Profile information
  uint32_t profile_size{0};
  uint32_t cmm_type{0};  // Color Management Module
  uint32_t version{0};   // Major.minor.bugfix
  uint32_t device_class{0};
  uint32_t color_space{0};  // Input color space
  uint32_t pcs{0};          // Profile Connection Space (usually XYZ or Lab)

  // Number of color components (1=Gray, 3=RGB, 4=CMYK)
  int num_components{0};

  // Parsed profile data (for matrix/TRC profiles)
  bool is_matrix_profile{false};  // True if matrix/TRC based

  // Colorant matrix (3x3) - converts profile RGB to XYZ
  // Row-major: [rX, gX, bX, rY, gY, bY, rZ, gZ, bZ]
  float colorant_matrix[9]{0};

  // TRC curves for each channel
  IccTRC trc_r, trc_g, trc_b;  // For RGB profiles
  IccTRC trc_gray;              // For Gray profiles

  // White point
  float white_point[3]{0.9505f, 1.0f, 1.0889f};  // Default D65
};

// Parse ICC profile header (first 128 bytes)
IccProfileInfo parse_icc_profile_header(const uint8_t* data, size_t size);

// Parse full ICC profile including tags (TRC curves, colorant matrix)
IccProfileInfo parse_icc_profile(const uint8_t* data, size_t size);

// Convert color using parsed ICC profile
RGB iccbased_to_rgb(const float* components, int num_components,
                    const IccProfileInfo& profile);

// Basic color space conversions (no ICC profile needed)

// Gray to RGB (simple scaling)
RGB gray_to_rgb(float gray);

// CMYK to RGB (simple conversion, no ICC)
RGB cmyk_to_rgb(float c, float m, float y, float k);

// CalGray to RGB (with gamma and white point)
RGB calgray_to_rgb(float gray,
                   const std::vector<double>& white_point,
                   const std::vector<double>& black_point,
                   const std::vector<double>& gamma);

// CalRGB to RGB (with gamma, white point, and matrix)
RGB calrgb_to_rgb(float r, float g, float b,
                  const std::vector<double>& white_point,
                  const std::vector<double>& black_point,
                  const std::vector<double>& gamma,
                  const std::vector<double>& matrix);

// Lab to XYZ
XYZ lab_to_xyz(const Lab& lab, const std::vector<double>& white_point);

// XYZ to RGB (using sRGB D65 white point)
RGB xyz_to_rgb(const XYZ& xyz);

// Lab to RGB (combined conversion)
RGB lab_to_rgb(const Lab& lab, const std::vector<double>& white_point);

// ICCBased to RGB (simple approximation based on number of components)
// This is a fallback when we don't want to parse full ICC profiles
RGB iccbased_to_rgb_simple(const float* components, int num_components);

// Transform entire scanline (in-place or to separate buffer)
struct TransformResult {
  bool success{false};
  std::string error;
  std::vector<uint8_t> data;  // RGB data (3 bytes per pixel)
};

// Transform image data from color space to RGB
TransformResult transform_to_rgb(const ColorSpace& color_space,
                                  const uint8_t* src_data,
                                  size_t src_size,
                                  int width,
                                  int height,
                                  int bits_per_component);

}  // namespace color
}  // namespace nanopdf
