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

// Read big-endian uint16
inline uint16_t read_be_u16(const uint8_t* data) {
  return (static_cast<uint16_t>(data[0]) << 8) |
         static_cast<uint16_t>(data[1]);
}

// Read s15Fixed16Number (signed 15.16 fixed point)
inline float read_s15Fixed16(const uint8_t* data) {
  int32_t raw = static_cast<int32_t>(read_be_u32(data));
  return static_cast<float>(raw) / 65536.0f;
}

// Read u8Fixed8Number (unsigned 8.8 fixed point)
inline float read_u8Fixed8(const uint8_t* data) {
  uint16_t raw = read_be_u16(data);
  return static_cast<float>(raw) / 256.0f;
}

}  // namespace

// IccTRC implementation
float IccTRC::apply(float v) const {
  if (!valid) return v;

  v = clamp01(v);

  if (is_gamma) {
    // Simple gamma
    return std::pow(v, gamma);
  } else if (!curve.empty()) {
    // Curve table lookup with linear interpolation
    float idx = v * static_cast<float>(curve.size() - 1);
    size_t i0 = static_cast<size_t>(idx);
    size_t i1 = std::min(i0 + 1, curve.size() - 1);
    float frac = idx - static_cast<float>(i0);
    return curve[i0] * (1.0f - frac) + curve[i1] * frac;
  }

  return v;
}

float IccTRC::apply_inverse(float v) const {
  if (!valid) return v;

  v = clamp01(v);

  if (is_gamma && gamma > 0.0f) {
    // Simple inverse gamma
    return std::pow(v, 1.0f / gamma);
  } else if (!curve.empty()) {
    // Binary search in curve table
    size_t lo = 0, hi = curve.size() - 1;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (curve[mid] < v) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return static_cast<float>(lo) / static_cast<float>(curve.size() - 1);
  }

  return v;
}

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

namespace {

// Helper to find and parse a tag in the ICC profile
struct IccTag {
  uint32_t signature;
  uint32_t offset;
  uint32_t size;
};

// Parse TRC (Tone Reproduction Curve) tag
IccTRC parse_trc_tag(const uint8_t* data, size_t data_size,
                     uint32_t offset, uint32_t size) {
  IccTRC trc;

  if (offset + size > data_size || size < 12) {
    return trc;
  }

  const uint8_t* tag_data = data + offset;

  // Tag type signature at offset 0-3
  uint32_t type_sig = read_be_u32(tag_data);

  // 'curv' type (0x63757276)
  if (type_sig == 0x63757276) {
    // Bytes 8-11: Number of entries
    uint32_t count = read_be_u32(tag_data + 8);

    if (count == 0) {
      // Identity curve (gamma = 1.0)
      trc.valid = true;
      trc.is_gamma = true;
      trc.gamma = 1.0f;
    } else if (count == 1) {
      // Single value = gamma encoded as u8Fixed8
      if (size >= 14) {
        trc.valid = true;
        trc.is_gamma = true;
        trc.gamma = read_u8Fixed8(tag_data + 12);
      }
    } else {
      // Curve table (16-bit values)
      if (size >= 12 + count * 2) {
        trc.valid = true;
        trc.is_gamma = false;
        trc.curve.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
          uint16_t val = read_be_u16(tag_data + 12 + i * 2);
          trc.curve[i] = static_cast<float>(val) / 65535.0f;
        }
      }
    }
  }
  // 'para' type (parametric curve) - 0x70617261
  else if (type_sig == 0x70617261) {
    if (size >= 16) {
      uint16_t func_type = read_be_u16(tag_data + 8);

      if (func_type == 0) {
        // Type 0: Y = X^g
        trc.valid = true;
        trc.is_gamma = true;
        trc.gamma = read_s15Fixed16(tag_data + 12);
      } else if (func_type == 1 && size >= 24) {
        // Type 1: Y = (aX+b)^g for X >= -b/a, else Y = 0
        // Approximate as simple gamma
        trc.valid = true;
        trc.is_gamma = true;
        trc.gamma = read_s15Fixed16(tag_data + 12);
      } else if (func_type == 2 && size >= 28) {
        // Type 2: sRGB-like
        trc.valid = true;
        trc.is_gamma = true;
        trc.gamma = read_s15Fixed16(tag_data + 12);
      } else if (func_type == 3 && size >= 32) {
        // Type 3: Y = (aX+b)^g + c for X >= d, else Y = cX
        trc.valid = true;
        trc.is_gamma = true;
        trc.gamma = read_s15Fixed16(tag_data + 12);
      } else if (func_type == 4 && size >= 36) {
        // Type 4: Y = (aX+b)^g + e for X >= d, else Y = cX + f
        trc.valid = true;
        trc.is_gamma = true;
        trc.gamma = read_s15Fixed16(tag_data + 12);
      }
    }
  }

  return trc;
}

// Parse XYZ tag (returns XYZ values)
bool parse_xyz_tag(const uint8_t* data, size_t data_size,
                   uint32_t offset, uint32_t size,
                   float* x, float* y, float* z) {
  if (offset + size > data_size || size < 20) {
    return false;
  }

  const uint8_t* tag_data = data + offset;

  // Tag type signature at offset 0-3 should be 'XYZ ' (0x58595A20)
  uint32_t type_sig = read_be_u32(tag_data);
  if (type_sig != 0x58595A20) {
    return false;
  }

  // XYZ values at offsets 8, 12, 16 (s15Fixed16)
  *x = read_s15Fixed16(tag_data + 8);
  *y = read_s15Fixed16(tag_data + 12);
  *z = read_s15Fixed16(tag_data + 16);

  return true;
}

}  // namespace

IccProfileInfo parse_icc_profile(const uint8_t* data, size_t size) {
  // First parse the header
  IccProfileInfo info = parse_icc_profile_header(data, size);
  if (!info.valid) {
    return info;
  }

  // Need at least header + tag table
  if (size < 132) {
    return info;  // Return basic info without tag data
  }

  // Tag count at offset 128
  uint32_t tag_count = read_be_u32(data + 128);

  if (tag_count > 100 || size < 132 + tag_count * 12) {
    return info;  // Invalid or truncated tag table
  }

  // Build tag directory
  std::vector<IccTag> tags(tag_count);
  for (uint32_t i = 0; i < tag_count; ++i) {
    const uint8_t* entry = data + 132 + i * 12;
    tags[i].signature = read_be_u32(entry);
    tags[i].offset = read_be_u32(entry + 4);
    tags[i].size = read_be_u32(entry + 8);
  }

  // Helper to find a tag
  auto find_tag = [&](uint32_t sig) -> const IccTag* {
    for (const auto& tag : tags) {
      if (tag.signature == sig) {
        return &tag;
      }
    }
    return nullptr;
  };

  // Parse based on color space type
  if (info.color_space == ICC_SIG_RGB) {
    // Try to parse as matrix/TRC profile
    const IccTag* rXYZ = find_tag(ICC_TAG_rXYZ);
    const IccTag* gXYZ = find_tag(ICC_TAG_gXYZ);
    const IccTag* bXYZ = find_tag(ICC_TAG_bXYZ);
    const IccTag* rTRC = find_tag(ICC_TAG_rTRC);
    const IccTag* gTRC = find_tag(ICC_TAG_gTRC);
    const IccTag* bTRC = find_tag(ICC_TAG_bTRC);

    if (rXYZ && gXYZ && bXYZ && rTRC && gTRC && bTRC) {
      // Parse colorant matrix
      float rx, ry, rz, gx, gy, gz, bx, by, bz;
      bool have_matrix =
          parse_xyz_tag(data, size, rXYZ->offset, rXYZ->size, &rx, &ry, &rz) &&
          parse_xyz_tag(data, size, gXYZ->offset, gXYZ->size, &gx, &gy, &gz) &&
          parse_xyz_tag(data, size, bXYZ->offset, bXYZ->size, &bx, &by, &bz);

      if (have_matrix) {
        // Store as row-major matrix (XYZ rows, RGB columns)
        info.colorant_matrix[0] = rx;
        info.colorant_matrix[1] = gx;
        info.colorant_matrix[2] = bx;
        info.colorant_matrix[3] = ry;
        info.colorant_matrix[4] = gy;
        info.colorant_matrix[5] = by;
        info.colorant_matrix[6] = rz;
        info.colorant_matrix[7] = gz;
        info.colorant_matrix[8] = bz;
      }

      // Parse TRC curves
      info.trc_r = parse_trc_tag(data, size, rTRC->offset, rTRC->size);
      info.trc_g = parse_trc_tag(data, size, gTRC->offset, gTRC->size);
      info.trc_b = parse_trc_tag(data, size, bTRC->offset, bTRC->size);

      if (have_matrix && info.trc_r.valid && info.trc_g.valid && info.trc_b.valid) {
        info.is_matrix_profile = true;
      }
    }
  } else if (info.color_space == ICC_SIG_GRAY) {
    // Gray profile - just need kTRC
    const IccTag* kTRC = find_tag(ICC_TAG_kTRC);
    if (kTRC) {
      info.trc_gray = parse_trc_tag(data, size, kTRC->offset, kTRC->size);
      if (info.trc_gray.valid) {
        info.is_matrix_profile = true;  // Simple TRC profile
      }
    }
  }

  // Parse white point if available
  const IccTag* wtpt = find_tag(ICC_TAG_wtpt);
  if (wtpt) {
    parse_xyz_tag(data, size, wtpt->offset, wtpt->size,
                  &info.white_point[0], &info.white_point[1], &info.white_point[2]);
  }

  return info;
}

RGB iccbased_to_rgb(const float* components, int num_components,
                    const IccProfileInfo& profile) {
  if (!profile.valid) {
    return iccbased_to_rgb_simple(components, num_components);
  }

  // Handle based on profile color space
  if (profile.color_space == ICC_SIG_GRAY && num_components >= 1) {
    float gray = clamp01(components[0]);

    // Apply gray TRC if available
    if (profile.trc_gray.valid) {
      gray = profile.trc_gray.apply(gray);
    }

    return RGB(gray, gray, gray);
  }

  if (profile.color_space == ICC_SIG_RGB && num_components >= 3) {
    float r = clamp01(components[0]);
    float g = clamp01(components[1]);
    float b = clamp01(components[2]);

    if (profile.is_matrix_profile) {
      // Apply TRC curves (linearize)
      if (profile.trc_r.valid) r = profile.trc_r.apply(r);
      if (profile.trc_g.valid) g = profile.trc_g.apply(g);
      if (profile.trc_b.valid) b = profile.trc_b.apply(b);

      // Apply colorant matrix to get XYZ
      float x = profile.colorant_matrix[0] * r +
                profile.colorant_matrix[1] * g +
                profile.colorant_matrix[2] * b;
      float y = profile.colorant_matrix[3] * r +
                profile.colorant_matrix[4] * g +
                profile.colorant_matrix[5] * b;
      float z = profile.colorant_matrix[6] * r +
                profile.colorant_matrix[7] * g +
                profile.colorant_matrix[8] * b;

      // Convert XYZ to sRGB
      return xyz_to_rgb(XYZ(x, y, z));
    }

    // Non-matrix profile - return as-is (assuming sRGB)
    return RGB(r, g, b);
  }

  if (profile.color_space == ICC_SIG_CMYK && num_components >= 4) {
    // CMYK profiles require CLUT - fall back to simple conversion
    return cmyk_to_rgb(components[0], components[1],
                       components[2], components[3]);
  }

  if (profile.color_space == ICC_SIG_LAB && num_components >= 3) {
    // Lab profile
    Lab lab;
    lab.L = components[0] * 100.0f;
    lab.a = components[1] * 255.0f - 128.0f;
    lab.b = components[2] * 255.0f - 128.0f;

    std::vector<double> wp = {
        static_cast<double>(profile.white_point[0]),
        static_cast<double>(profile.white_point[1]),
        static_cast<double>(profile.white_point[2])};

    return lab_to_rgb(lab, wp);
  }

  // Fallback to simple conversion
  return iccbased_to_rgb_simple(components, num_components);
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
    } else if (bits_per_component == 16) {
      // 16-bit components (big-endian)
      const uint8_t* src_pixel = src_data + i * src_components * 2;
      float components[4] = {0.0f, 0.0f, 0.0f, 0.0f};

      for (int c = 0; c < src_components && c < 4; ++c) {
        uint16_t val = (static_cast<uint16_t>(src_pixel[c * 2]) << 8) |
                       static_cast<uint16_t>(src_pixel[c * 2 + 1]);
        components[c] = static_cast<float>(val) / 65535.0f;
      }

      // Convert based on color space type (same as 8-bit)
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
          rgb = RGB(0.5f, 0.5f, 0.5f);
          break;
      }
    } else if (bits_per_component == 1 || bits_per_component == 2 ||
               bits_per_component == 4) {
      // Sub-byte components (packed pixels)
      // For simplicity, we need to handle row-by-row with padding
      // This path handles single-component (gray) images primarily

      int mask = (1 << bits_per_component) - 1;
      float max_val = static_cast<float>(mask);

      // Calculate position in packed data
      int row = i / width;
      int col = i % width;
      int bytes_per_row = (width * src_components * bits_per_component + 7) / 8;
      int bit_offset = (col * src_components * bits_per_component) % 8;
      int byte_idx = row * bytes_per_row + (col * src_components * bits_per_component) / 8;

      if (byte_idx >= static_cast<int>(src_size)) {
        rgb = RGB(0.5f, 0.5f, 0.5f);
      } else {
        float components[4] = {0.0f, 0.0f, 0.0f, 0.0f};

        for (int c = 0; c < src_components && c < 4; ++c) {
          int c_bit_offset = bit_offset + c * bits_per_component;
          int c_byte_idx = byte_idx + c_bit_offset / 8;
          int c_shift = 8 - bits_per_component - (c_bit_offset % 8);

          if (c_byte_idx < static_cast<int>(src_size) && c_shift >= 0) {
            uint8_t val = (src_data[c_byte_idx] >> c_shift) & mask;
            components[c] = static_cast<float>(val) / max_val;
          }
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
          default:
            rgb = gray_to_rgb(components[0]);
            break;
        }
      }
    } else {
      result.error = "Unsupported bit depth";
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
