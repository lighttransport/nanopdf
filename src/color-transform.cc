// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: Apache 2.0
//
// Custom color space transformation implementation

#include "color-transform.hh"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "nanopdf.hh"

namespace nanopdf {
namespace color {

namespace {

// Clamp value to [0, 1] range
inline float clamp01(float v) {
  return std::max(0.0f, std::min(1.0f, v));
}

// Standard D65 white point for sRGB
constexpr float D65_X = 0.9505f;
constexpr float D65_Y = 1.0000f;
constexpr float D65_Z = 1.0889f;

// sRGB gamma correction
inline float srgb_gamma(float linear) {
  if (linear <= 0.0031308f) {
    return 12.92f * linear;
  } else {
    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
  }
}

// Inverse sRGB gamma
inline float srgb_inverse_gamma(float srgb) {
  if (srgb <= 0.04045f) {
    return srgb / 12.92f;
  } else {
    return std::pow((srgb + 0.055f) / 1.055f, 2.4f);
  }
}

// Read big-endian uint32
inline uint32_t read_be_u32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

}  // namespace

IccProfileInfo parse_icc_profile_header(const uint8_t* data, size_t size) {
  IccProfileInfo info;

  if (!data || size < 128) {
    info.error = "ICC profile too small (need at least 128 bytes for header)";
    return info;
  }

  // ICC profile header structure (first 128 bytes)
  // Bytes 0-3: Profile size
  info.profile_size = read_be_u32(data);

  if (info.profile_size > size) {
    info.error = "ICC profile size mismatch";
    return info;
  }

  // Bytes 4-7: CMM Type
  info.cmm_type = read_be_u32(data + 4);

  // Bytes 8-11: Version
  info.version = read_be_u32(data + 8);

  // Bytes 12-15: Device Class
  info.device_class = read_be_u32(data + 12);

  // Bytes 16-19: Color Space (input)
  info.color_space = read_be_u32(data + 16);

  // Bytes 20-23: Profile Connection Space (PCS)
  info.pcs = read_be_u32(data + 20);

  // Determine number of components from color space signature
  // 'GRAY' = 0x47524159, 'RGB ' = 0x52474220, 'CMYK' = 0x434D594B
  uint32_t cs = info.color_space;
  if (cs == 0x47524159) {  // GRAY
    info.num_components = 1;
  } else if (cs == 0x52474220) {  // RGB
    info.num_components = 3;
  } else if (cs == 0x434D594B) {  // CMYK
    info.num_components = 4;
  } else if (cs == 0x4C616220) {  // Lab
    info.num_components = 3;
  } else {
    // Unknown, try to infer from PCS or assume RGB
    info.num_components = 3;
  }

  info.valid = true;
  return info;
}

RGB gray_to_rgb(float gray) {
  float g = clamp01(gray);
  return RGB(g, g, g);
}

RGB cmyk_to_rgb(float c, float m, float y, float k) {
  // Simple CMYK to RGB conversion (not color-managed)
  // Formula: RGB = (1 - C) * (1 - K)
  float ik = 1.0f - clamp01(k);
  float r = (1.0f - clamp01(c)) * ik;
  float g = (1.0f - clamp01(m)) * ik;
  float b = (1.0f - clamp01(y)) * ik;

  return RGB(clamp01(r), clamp01(g), clamp01(b));
}

RGB calgray_to_rgb(float gray,
                   const std::vector<double>& white_point,
                   const std::vector<double>& black_point,
                   const std::vector<double>& gamma) {
  // CalGray: A = gray^Gamma
  float g = clamp01(gray);

  // Apply gamma
  if (!gamma.empty() && gamma[0] > 0.0) {
    g = std::pow(g, static_cast<float>(gamma[0]));
  }

  // Apply black point (simple linear adjustment)
  if (!black_point.empty() && black_point[0] > 0.0) {
    float bp = static_cast<float>(black_point[0]);
    g = std::max(0.0f, g - bp) / (1.0f - bp);
  }

  // White point adaptation could be added here (complex)
  // For simplicity, we assume D65 white point

  return RGB(g, g, g);
}

RGB calrgb_to_rgb(float r, float g, float b,
                  const std::vector<double>& white_point,
                  const std::vector<double>& black_point,
                  const std::vector<double>& gamma,
                  const std::vector<double>& matrix) {
  // Apply gamma to each component
  if (gamma.size() >= 3) {
    if (gamma[0] > 0.0) r = std::pow(clamp01(r), static_cast<float>(gamma[0]));
    if (gamma[1] > 0.0) g = std::pow(clamp01(g), static_cast<float>(gamma[1]));
    if (gamma[2] > 0.0) b = std::pow(clamp01(b), static_cast<float>(gamma[2]));
  }

  // Apply matrix transformation if provided
  if (matrix.size() >= 9) {
    float r_new = static_cast<float>(matrix[0]) * r +
                  static_cast<float>(matrix[1]) * g +
                  static_cast<float>(matrix[2]) * b;
    float g_new = static_cast<float>(matrix[3]) * r +
                  static_cast<float>(matrix[4]) * g +
                  static_cast<float>(matrix[5]) * b;
    float b_new = static_cast<float>(matrix[6]) * r +
                  static_cast<float>(matrix[7]) * g +
                  static_cast<float>(matrix[8]) * b;

    r = r_new;
    g = g_new;
    b = b_new;
  }

  return RGB(clamp01(r), clamp01(g), clamp01(b));
}

XYZ lab_to_xyz(const Lab& lab, const std::vector<double>& white_point) {
  // Lab to XYZ conversion
  // Uses CIE L*a*b* standard formulas

  // Get white point (default to D65 if not provided)
  float Xn = D65_X;
  float Yn = D65_Y;
  float Zn = D65_Z;

  if (white_point.size() >= 3) {
    Xn = static_cast<float>(white_point[0]);
    Yn = static_cast<float>(white_point[1]);
    Zn = static_cast<float>(white_point[2]);
  }

  // Calculate f(Y)
  float fy = (lab.L + 16.0f) / 116.0f;
  float fx = fy + (lab.a / 500.0f);
  float fz = fy - (lab.b / 200.0f);

  // Inverse f function
  auto f_inv = [](float t) -> float {
    const float delta = 6.0f / 29.0f;
    if (t > delta) {
      return t * t * t;
    } else {
      return 3.0f * delta * delta * (t - 4.0f / 29.0f);
    }
  };

  float x = Xn * f_inv(fx);
  float y = Yn * f_inv(fy);
  float z = Zn * f_inv(fz);

  return XYZ(x, y, z);
}

RGB xyz_to_rgb(const XYZ& xyz) {
  // XYZ to sRGB conversion matrix (D65 white point)
  // Source: http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html
  float r = 3.2404542f * xyz.x - 1.5371385f * xyz.y - 0.4985314f * xyz.z;
  float g = -0.9692660f * xyz.x + 1.8760108f * xyz.y + 0.0415560f * xyz.z;
  float b = 0.0556434f * xyz.x - 0.2040259f * xyz.y + 1.0572252f * xyz.z;

  // Apply sRGB gamma correction
  r = srgb_gamma(r);
  g = srgb_gamma(g);
  b = srgb_gamma(b);

  return RGB(clamp01(r), clamp01(g), clamp01(b));
}

RGB lab_to_rgb(const Lab& lab, const std::vector<double>& white_point) {
  XYZ xyz = lab_to_xyz(lab, white_point);
  return xyz_to_rgb(xyz);
}

RGB iccbased_to_rgb_simple(const float* components, int num_components) {
  // Simple approximation based on number of components
  if (num_components == 1) {
    // Gray
    return gray_to_rgb(components[0]);
  } else if (num_components == 3) {
    // Assume RGB
    return RGB(clamp01(components[0]),
               clamp01(components[1]),
               clamp01(components[2]));
  } else if (num_components == 4) {
    // CMYK
    return cmyk_to_rgb(components[0], components[1],
                       components[2], components[3]);
  } else {
    // Unknown, return gray
    return RGB(0.5f, 0.5f, 0.5f);
  }
}

TransformResult transform_to_rgb(const ColorSpace& color_space,
                                  const uint8_t* src_data,
                                  size_t src_size,
                                  int width,
                                  int height,
                                  int bits_per_component) {
  TransformResult result;

  if (!src_data || src_size == 0 || width <= 0 || height <= 0) {
    result.error = "Invalid input parameters";
    return result;
  }

  int num_pixels = width * height;
  result.data.resize(num_pixels * 3);  // RGB output (3 bytes per pixel)

  // Determine source components based on color space
  int src_components = 1;
  switch (color_space.type) {
    case ColorSpaceType::DeviceGray:
    case ColorSpaceType::CalGray:
      src_components = 1;
      break;
    case ColorSpaceType::DeviceRGB:
    case ColorSpaceType::CalRGB:
    case ColorSpaceType::Lab:
      src_components = 3;
      break;
    case ColorSpaceType::DeviceCMYK:
      src_components = 4;
      break;
    case ColorSpaceType::ICCBased:
      src_components = color_space.num_components;
      if (src_components <= 0) src_components = 3;
      break;
    default:
      result.error = "Unsupported color space for transformation";
      return result;
  }

  // Check if we have enough data
  size_t expected_size = num_pixels * src_components * ((bits_per_component + 7) / 8);
  if (src_size < expected_size) {
    result.error = "Source data too small for image dimensions";
    return result;
  }

  // Transform pixel by pixel
  for (int i = 0; i < num_pixels; ++i) {
    RGB rgb;

    // Extract source components
    if (bits_per_component == 8) {
      const uint8_t* src_pixel = src_data + i * src_components;
      float components[4] = {0.0f, 0.0f, 0.0f, 0.0f};

      for (int c = 0; c < src_components && c < 4; ++c) {
        components[c] = src_pixel[c] / 255.0f;
      }

      // Convert based on color space type
      switch (color_space.type) {
        case ColorSpaceType::DeviceGray:
          rgb = gray_to_rgb(components[0]);
          break;

        case ColorSpaceType::DeviceRGB:
          rgb = RGB(components[0], components[1], components[2]);
          break;

        case ColorSpaceType::DeviceCMYK:
          rgb = cmyk_to_rgb(components[0], components[1],
                           components[2], components[3]);
          break;

        case ColorSpaceType::CalGray:
          rgb = calgray_to_rgb(components[0], color_space.white_point,
                              color_space.black_point, color_space.gamma);
          break;

        case ColorSpaceType::CalRGB:
          rgb = calrgb_to_rgb(components[0], components[1], components[2],
                             color_space.white_point, color_space.black_point,
                             color_space.gamma, color_space.matrix);
          break;

        case ColorSpaceType::Lab: {
          // Lab values need to be decoded from PDF range
          // L*: 0-100, a*: -128 to 127, b*: -128 to 127
          Lab lab;
          lab.L = components[0] * 100.0f;
          lab.a = components[1] * 255.0f - 128.0f;
          lab.b = components[2] * 255.0f - 128.0f;
          rgb = lab_to_rgb(lab, color_space.white_point);
          break;
        }

        case ColorSpaceType::ICCBased:
          rgb = iccbased_to_rgb_simple(components, src_components);
          break;

        default:
          // Fallback to gray
          rgb = RGB(0.5f, 0.5f, 0.5f);
          break;
      }
    } else {
      // TODO: Handle other bit depths (1, 2, 4, 16 bits)
      result.error = "Only 8-bit color components supported currently";
      return result;
    }

    // Write RGB output
    uint8_t* dst_pixel = result.data.data() + i * 3;
    dst_pixel[0] = static_cast<uint8_t>(rgb.r * 255.0f + 0.5f);
    dst_pixel[1] = static_cast<uint8_t>(rgb.g * 255.0f + 0.5f);
    dst_pixel[2] = static_cast<uint8_t>(rgb.b * 255.0f + 0.5f);
  }

  result.success = true;
  return result;
}

}  // namespace color
}  // namespace nanopdf
