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

// ============================================================================
// Core color types
// ============================================================================

// RGB color in 0-1 range (may exceed for scene-referred / wide-gamut)
struct RGB {
  float r{0.0f};
  float g{0.0f};
  float b{0.0f};

  RGB() = default;
  RGB(float r_, float g_, float b_) : r(r_), g(g_), b(b_) {}
};

// CIE 1931 XYZ
struct XYZ {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};

  XYZ() = default;
  XYZ(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

// CIE L*a*b*
struct Lab {
  float L{0.0f};   // 0-100
  float a{0.0f};   // -128..127
  float b{0.0f};   // -128..127

  Lab() = default;
  Lab(float L_, float a_, float b_) : L(L_), a(a_), b(b_) {}
};

// ============================================================================
// RGB working / output color spaces
// ============================================================================

enum class RGBColorSpace {
  sRGB,             // IEC 61966-2-1: D65, sRGB piecewise transfer
  LinearSRGB,       // sRGB primaries, linear (gamma 1.0)
  Rec709,           // ITU-R BT.709: same primaries as sRGB, BT.1886 gamma 2.4
  LinearRec709,     // BT.709 primaries, linear
  DisplayP3,        // Apple Display P3: DCI-P3 primaries, sRGB transfer, D65
  LinearDisplayP3,  // Display P3 primaries, linear
  Rec2020,          // ITU-R BT.2020: BT.2020 transfer function
  LinearRec2020,    // BT.2020 primaries, linear
  ACEScg,           // ACES CG: linear, AP1 primaries, D60
  ACES2065_1,       // ACES 2065-1: linear, AP0 primaries, D60
};

// Static description of an RGB color space
struct RGBColorSpaceDef {
  const char* name;

  // CIE xy chromaticity of primaries and white point
  float rx, ry;   // Red
  float gx, gy;   // Green
  float bx, by;   // Blue
  float wx, wy;   // White

  // Transfer function selection
  bool srgb_transfer;  // Use sRGB piecewise EOTF/OETF
  bool rec2020_transfer; // Use Rec.2020 piecewise OETF
  float gamma;         // Simple power-law gamma (when neither piecewise TF)
  bool is_linear;      // Linear (gamma == 1, no transfer)
};

// Retrieve the definition of a known color space
const RGBColorSpaceDef& get_color_space_def(RGBColorSpace cs);

// Return the human-readable name
const char* color_space_name(RGBColorSpace cs);

// ============================================================================
// 3x3 matrix (row-major)
// ============================================================================

struct Matrix3x3 {
  float m[9]{1, 0, 0, 0, 1, 0, 0, 0, 1};

  Matrix3x3 operator*(const Matrix3x3& rhs) const;
  void apply(float x, float y, float z,
             float& ox, float& oy, float& oz) const;
};

// Build RGB→XYZ and XYZ→RGB matrices from chromaticity coordinates
Matrix3x3 rgb_to_xyz_matrix(const RGBColorSpaceDef& def);
Matrix3x3 xyz_to_rgb_matrix(const RGBColorSpaceDef& def);

// Combined matrix: src RGB → dst RGB (through XYZ, with chromatic adaptation)
Matrix3x3 rgb_to_rgb_matrix(RGBColorSpace src, RGBColorSpace dst);

// ============================================================================
// Transfer functions
// ============================================================================

// Forward: linear → encoded
float apply_transfer(float linear, RGBColorSpace cs);

// Inverse: encoded → linear
float apply_inverse_transfer(float encoded, RGBColorSpace cs);

// ============================================================================
// High-level conversions
// ============================================================================

// Convert RGB triple between any two supported color spaces
RGB convert_rgb(const RGB& src, RGBColorSpace from, RGBColorSpace to);

// XYZ → target RGB color space (applies matrix + transfer)
RGB xyz_to_rgb(const XYZ& xyz, RGBColorSpace target);

// Original sRGB-only overload (kept for compatibility)
RGB xyz_to_rgb(const XYZ& xyz);

// ============================================================================
// ICC profile support
// ============================================================================

constexpr uint32_t ICC_SIG_GRAY = 0x47524159;  // 'GRAY'
constexpr uint32_t ICC_SIG_RGB  = 0x52474220;  // 'RGB '
constexpr uint32_t ICC_SIG_CMYK = 0x434D594B;  // 'CMYK'
constexpr uint32_t ICC_SIG_LAB  = 0x4C616220;  // 'Lab '
constexpr uint32_t ICC_SIG_XYZ  = 0x58595A20;  // 'XYZ '

constexpr uint32_t ICC_TAG_rXYZ = 0x7258595A;
constexpr uint32_t ICC_TAG_gXYZ = 0x6758595A;
constexpr uint32_t ICC_TAG_bXYZ = 0x6258595A;
constexpr uint32_t ICC_TAG_rTRC = 0x72545243;
constexpr uint32_t ICC_TAG_gTRC = 0x67545243;
constexpr uint32_t ICC_TAG_bTRC = 0x62545243;
constexpr uint32_t ICC_TAG_kTRC = 0x6B545243;
constexpr uint32_t ICC_TAG_wtpt = 0x77747074;
constexpr uint32_t ICC_TAG_A2B0 = 0x41324230;
constexpr uint32_t ICC_TAG_A2B1 = 0x41324231;
constexpr uint32_t ICC_TAG_desc = 0x64657363;

struct IccParametricCurve {
  int type{0};
  float g{1.0f};
  float a{1.0f}, b{0.0f}, c{0.0f}, d{0.0f}, e{0.0f}, f{0.0f};
  float apply(float x) const;
};

struct IccTRC {
  bool valid{false};
  bool is_gamma{false};
  bool is_parametric{false};
  float gamma{1.0f};
  std::vector<float> curve;
  IccParametricCurve parametric;

  float apply(float v) const;
  float apply_inverse(float v) const;
};

struct IccProfileInfo {
  bool valid{false};
  std::string error;

  uint32_t profile_size{0};
  uint32_t cmm_type{0};
  uint32_t version{0};
  uint32_t device_class{0};
  uint32_t color_space{0};
  uint32_t pcs{0};
  int num_components{0};

  bool is_matrix_profile{false};
  float colorant_matrix[9]{0};
  IccTRC trc_r, trc_g, trc_b;
  IccTRC trc_gray;
  float white_point[3]{0.9505f, 1.0f, 1.0889f};

  bool has_clut{false};
  int clut_input_channels{0};
  int clut_output_channels{0};
  std::vector<int> clut_grid_points;
  std::vector<float> clut_data;
  std::vector<IccTRC> clut_input_curves;
  std::vector<IccTRC> clut_output_curves;
};

IccProfileInfo parse_icc_profile_header(const uint8_t* data, size_t size);
IccProfileInfo parse_icc_profile(const uint8_t* data, size_t size);

RGB iccbased_to_rgb(const float* components, int num_components,
                    const IccProfileInfo& profile);

// ============================================================================
// Basic color space conversions (no ICC needed)
// ============================================================================

RGB gray_to_rgb(float gray);
RGB cmyk_to_rgb(float c, float m, float y, float k);

RGB calgray_to_rgb(float gray,
                   const std::vector<double>& white_point,
                   const std::vector<double>& black_point,
                   const std::vector<double>& gamma);

RGB calrgb_to_rgb(float r, float g, float b,
                  const std::vector<double>& white_point,
                  const std::vector<double>& black_point,
                  const std::vector<double>& gamma,
                  const std::vector<double>& matrix);

XYZ lab_to_xyz(const Lab& lab, const std::vector<double>& white_point);
RGB lab_to_rgb(const Lab& lab, const std::vector<double>& white_point);

RGB iccbased_to_rgb_simple(const float* components, int num_components);

// ============================================================================
// Image-level transforms
// ============================================================================

struct TransformResult {
  bool success{false};
  std::string error;
  std::vector<uint8_t> data;  // 3 bytes per pixel (RGB)
};

TransformResult transform_to_rgb(const ColorSpace& color_space,
                                  const uint8_t* src_data,
                                  size_t src_size,
                                  int width, int height,
                                  int bits_per_component);

}  // namespace color
}  // namespace nanopdf
