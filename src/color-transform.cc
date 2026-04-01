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

bool checked_mul_size(size_t lhs, size_t rhs, size_t* out) {
  if (rhs != 0 && lhs > (std::numeric_limits<size_t>::max)() / rhs) {
    return false;
  }
  *out = lhs * rhs;
  return true;
}

float safe_powf(float base, float exponent, float fallback = 0.0f) {
  float integral_part = 0.0f;
  if (base < 0.0f && std::modf(exponent, &integral_part) != 0.0f) {
    return fallback;
  }
  if (base == 0.0f && exponent < 0.0f) {
    return fallback;
  }

  const float value = std::pow(base, exponent);
  return std::isfinite(value) ? value : fallback;
}

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

// Rec.2020 OETF (forward: linear → encoded)
inline float rec2020_oetf(float L) {
  const float alpha = 1.09929682680944f;
  const float beta = 0.018053968510807f;
  if (L < beta) {
    return 4.5f * L;
  }
  return alpha * std::pow(L, 0.45f) - (alpha - 1.0f);
}

// Rec.2020 inverse OETF (encoded → linear)
inline float rec2020_inv_oetf(float V) {
  const float alpha = 1.09929682680944f;
  const float beta_v = 4.5f * 0.018053968510807f;
  if (V < beta_v) {
    return V / 4.5f;
  }
  return std::pow((V + (alpha - 1.0f)) / alpha, 1.0f / 0.45f);
}

}  // namespace

// ============================================================================
// Color space definitions
// ============================================================================

// D65 white point: x=0.3127, y=0.3290
// D60 (ACES): x=0.32168, y=0.33767

static const RGBColorSpaceDef kColorSpaceDefs[] = {
  // sRGB / Rec.709 primaries, sRGB transfer
  {"sRGB",
   0.6400f, 0.3300f, 0.3000f, 0.6000f, 0.1500f, 0.0600f,
   0.3127f, 0.3290f,
   true, false, 2.2f, false},

  // Linear sRGB
  {"Linear sRGB",
   0.6400f, 0.3300f, 0.3000f, 0.6000f, 0.1500f, 0.0600f,
   0.3127f, 0.3290f,
   false, false, 1.0f, true},

  // Rec.709 (BT.1886 gamma 2.4)
  {"Rec. 709",
   0.6400f, 0.3300f, 0.3000f, 0.6000f, 0.1500f, 0.0600f,
   0.3127f, 0.3290f,
   false, false, 2.4f, false},

  // Linear Rec.709
  {"Linear Rec. 709",
   0.6400f, 0.3300f, 0.3000f, 0.6000f, 0.1500f, 0.0600f,
   0.3127f, 0.3290f,
   false, false, 1.0f, true},

  // Display P3 (DCI-P3 primaries, sRGB transfer, D65)
  {"Display P3",
   0.6800f, 0.3200f, 0.2650f, 0.6900f, 0.1500f, 0.0600f,
   0.3127f, 0.3290f,
   true, false, 2.2f, false},

  // Linear Display P3
  {"Linear Display P3",
   0.6800f, 0.3200f, 0.2650f, 0.6900f, 0.1500f, 0.0600f,
   0.3127f, 0.3290f,
   false, false, 1.0f, true},

  // Rec.2020 with BT.2020 transfer
  {"Rec. 2020",
   0.7080f, 0.2920f, 0.1700f, 0.7970f, 0.1310f, 0.0460f,
   0.3127f, 0.3290f,
   false, true, 2.2f, false},

  // Linear Rec.2020
  {"Linear Rec. 2020",
   0.7080f, 0.2920f, 0.1700f, 0.7970f, 0.1310f, 0.0460f,
   0.3127f, 0.3290f,
   false, false, 1.0f, true},

  // ACEScg (AP1 primaries, linear, D60)
  {"ACEScg",
   0.7130f, 0.2930f, 0.1650f, 0.8300f, 0.1280f, 0.0440f,
   0.32168f, 0.33767f,
   false, false, 1.0f, true},

  // ACES 2065-1 (AP0 primaries, linear, D60)
  {"ACES 2065-1",
   0.7347f, 0.2653f, 0.0000f, 1.0000f, 0.0001f, -0.0770f,
   0.32168f, 0.33767f,
   false, false, 1.0f, true},
};

const RGBColorSpaceDef& get_color_space_def(RGBColorSpace cs) {
  return kColorSpaceDefs[static_cast<int>(cs)];
}

const char* color_space_name(RGBColorSpace cs) {
  return kColorSpaceDefs[static_cast<int>(cs)].name;
}

// ============================================================================
// Matrix3x3 implementation
// ============================================================================

Matrix3x3 Matrix3x3::operator*(const Matrix3x3& rhs) const {
  Matrix3x3 r;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      r.m[row * 3 + col] = m[row * 3 + 0] * rhs.m[0 * 3 + col] +
                            m[row * 3 + 1] * rhs.m[1 * 3 + col] +
                            m[row * 3 + 2] * rhs.m[2 * 3 + col];
    }
  }
  return r;
}

void Matrix3x3::apply(float x, float y, float z,
                       float& ox, float& oy, float& oz) const {
  ox = m[0] * x + m[1] * y + m[2] * z;
  oy = m[3] * x + m[4] * y + m[5] * z;
  oz = m[6] * x + m[7] * y + m[8] * z;
}

namespace {

// Invert a 3x3 matrix. Returns false if singular.
bool invert3x3(const float* src, float* dst) {
  float det = src[0] * (src[4] * src[8] - src[5] * src[7]) -
              src[1] * (src[3] * src[8] - src[5] * src[6]) +
              src[2] * (src[3] * src[7] - src[4] * src[6]);
  if (std::abs(det) < 1e-12f) return false;
  float inv_det = 1.0f / det;
  dst[0] = (src[4] * src[8] - src[5] * src[7]) * inv_det;
  dst[1] = (src[2] * src[7] - src[1] * src[8]) * inv_det;
  dst[2] = (src[1] * src[5] - src[2] * src[4]) * inv_det;
  dst[3] = (src[5] * src[6] - src[3] * src[8]) * inv_det;
  dst[4] = (src[0] * src[8] - src[2] * src[6]) * inv_det;
  dst[5] = (src[2] * src[3] - src[0] * src[5]) * inv_det;
  dst[6] = (src[3] * src[7] - src[4] * src[6]) * inv_det;
  dst[7] = (src[1] * src[6] - src[0] * src[7]) * inv_det;
  dst[8] = (src[0] * src[4] - src[1] * src[3]) * inv_det;
  return true;
}

// Bradford chromatic adaptation matrix D65→D60
// M_adapt = M_bradford * diag(cone_d60/cone_d65) * M_bradford_inv
// Pre-computed for D65 → D60 adaptation
constexpr float kBradford_D65_to_D60[9] = {
   1.01303f,  0.00610531f, -0.014971f,
   0.00769823f, 0.998165f,  -0.00503203f,
  -0.00284131f, 0.00468516f, 0.924507f
};
constexpr float kBradford_D60_to_D65[9] = {
   0.987224f,  -0.00611327f,  0.0159533f,
  -0.00759836f,  1.00186f,     0.00533002f,
   0.00307257f, -0.00509595f,  1.08168f
};

}  // namespace

// Build RGB→XYZ matrix from chromaticity coordinates and white point.
// Uses the standard algorithm: compute S = M_prim^-1 * W, then scale columns.
Matrix3x3 rgb_to_xyz_matrix(const RGBColorSpaceDef& def) {
  // Convert xy to XYZ (Y=1)
  auto xy_to_XYZ = [](float x, float y, float& X, float& Y, float& Z) {
    if (std::abs(y) < 1e-10f) { X = Y = Z = 0; return; }
    X = x / y;
    Y = 1.0f;
    Z = (1.0f - x - y) / y;
  };

  float rX, rY, rZ, gX, gY, gZ, bX, bY, bZ, wX, wY, wZ;
  xy_to_XYZ(def.rx, def.ry, rX, rY, rZ);
  xy_to_XYZ(def.gx, def.gy, gX, gY, gZ);
  xy_to_XYZ(def.bx, def.by, bX, bY, bZ);
  xy_to_XYZ(def.wx, def.wy, wX, wY, wZ);

  // M_prim = [rX gX bX; rY gY bY; rZ gZ bZ]
  float M[9] = {rX, gX, bX, rY, gY, bY, rZ, gZ, bZ};
  float M_inv[9];
  if (!invert3x3(M, M_inv)) {
    Matrix3x3 identity;
    return identity;
  }

  // S = M_inv * W
  float Sr = M_inv[0] * wX + M_inv[1] * wY + M_inv[2] * wZ;
  float Sg = M_inv[3] * wX + M_inv[4] * wY + M_inv[5] * wZ;
  float Sb = M_inv[6] * wX + M_inv[7] * wY + M_inv[8] * wZ;

  Matrix3x3 result;
  result.m[0] = Sr * rX; result.m[1] = Sg * gX; result.m[2] = Sb * bX;
  result.m[3] = Sr * rY; result.m[4] = Sg * gY; result.m[5] = Sb * bY;
  result.m[6] = Sr * rZ; result.m[7] = Sg * gZ; result.m[8] = Sb * bZ;
  return result;
}

Matrix3x3 xyz_to_rgb_matrix(const RGBColorSpaceDef& def) {
  Matrix3x3 fwd = rgb_to_xyz_matrix(def);
  Matrix3x3 inv;
  if (!invert3x3(fwd.m, inv.m)) {
    return Matrix3x3{};  // identity fallback
  }
  return inv;
}

Matrix3x3 rgb_to_rgb_matrix(RGBColorSpace src, RGBColorSpace dst) {
  const auto& src_def = get_color_space_def(src);
  const auto& dst_def = get_color_space_def(dst);

  Matrix3x3 src_to_xyz = rgb_to_xyz_matrix(src_def);
  Matrix3x3 xyz_to_dst = xyz_to_rgb_matrix(dst_def);

  // Check if chromatic adaptation is needed (different white points)
  float dx = src_def.wx - dst_def.wx;
  float dy = src_def.wy - dst_def.wy;
  bool need_adapt = (dx * dx + dy * dy) > 1e-8f;

  if (!need_adapt) {
    return xyz_to_dst * src_to_xyz;
  }

  // Use pre-computed Bradford matrices for D65↔D60 (ACES)
  bool src_d60 = (std::abs(src_def.wx - 0.32168f) < 0.001f);
  bool dst_d60 = (std::abs(dst_def.wx - 0.32168f) < 0.001f);

  if (src_d60 && !dst_d60) {
    // Source is D60, dest is D65: adapt D60→D65
    Matrix3x3 adapt;
    std::memcpy(adapt.m, kBradford_D60_to_D65, sizeof(adapt.m));
    return xyz_to_dst * adapt * src_to_xyz;
  } else if (!src_d60 && dst_d60) {
    // Source is D65, dest is D60: adapt D65→D60
    Matrix3x3 adapt;
    std::memcpy(adapt.m, kBradford_D65_to_D60, sizeof(adapt.m));
    return xyz_to_dst * adapt * src_to_xyz;
  }

  // General case: just go through XYZ without adaptation
  return xyz_to_dst * src_to_xyz;
}

// ============================================================================
// Transfer function implementations
// ============================================================================

float apply_transfer(float linear, RGBColorSpace cs) {
  const auto& def = get_color_space_def(cs);
  if (def.is_linear) return linear;
  if (def.srgb_transfer) return srgb_gamma(std::max(0.0f, linear));
  if (def.rec2020_transfer) return rec2020_oetf(std::max(0.0f, linear));
  // Simple power-law gamma
  if (linear <= 0.0f) return 0.0f;
  return std::pow(linear, 1.0f / def.gamma);
}

float apply_inverse_transfer(float encoded, RGBColorSpace cs) {
  const auto& def = get_color_space_def(cs);
  if (def.is_linear) return encoded;
  if (def.srgb_transfer) return srgb_inverse_gamma(std::max(0.0f, encoded));
  if (def.rec2020_transfer) return rec2020_inv_oetf(std::max(0.0f, encoded));
  if (encoded <= 0.0f) return 0.0f;
  return std::pow(encoded, def.gamma);
}

// ============================================================================
// High-level conversion
// ============================================================================

RGB convert_rgb(const RGB& src, RGBColorSpace from, RGBColorSpace to) {
  if (from == to) return src;

  // 1. Linearize source
  float lr = apply_inverse_transfer(src.r, from);
  float lg = apply_inverse_transfer(src.g, from);
  float lb = apply_inverse_transfer(src.b, from);

  // 2. Apply combined matrix (src linear RGB → dst linear RGB)
  Matrix3x3 mat = rgb_to_rgb_matrix(from, to);
  float dr, dg, db;
  mat.apply(lr, lg, lb, dr, dg, db);

  // 3. Apply destination transfer function
  return RGB(apply_transfer(dr, to),
             apply_transfer(dg, to),
             apply_transfer(db, to));
}

RGB xyz_to_rgb(const XYZ& xyz, RGBColorSpace target) {
  Matrix3x3 mat = xyz_to_rgb_matrix(get_color_space_def(target));

  // Check if target white point differs from D65 (XYZ is D65-referenced)
  const auto& def = get_color_space_def(target);
  bool dst_d60 = (std::abs(def.wx - 0.32168f) < 0.001f);

  float x = xyz.x, y = xyz.y, z = xyz.z;

  if (dst_d60) {
    // Adapt D65 XYZ → D60 XYZ
    float ax, ay, az;
    Matrix3x3 adapt;
    std::memcpy(adapt.m, kBradford_D65_to_D60, sizeof(adapt.m));
    adapt.apply(x, y, z, ax, ay, az);
    x = ax; y = ay; z = az;
  }

  float r, g, b;
  mat.apply(x, y, z, r, g, b);

  return RGB(apply_transfer(r, target),
             apply_transfer(g, target),
             apply_transfer(b, target));
}

// IccParametricCurve implementation
float IccParametricCurve::apply(float x) const {
  x = std::max(0.0f, std::min(1.0f, x));
  switch (type) {
    case 0:
      // Y = X^g
      return safe_powf(x, g);
    case 1:
      // Y = (aX+b)^g  if X >= -b/a, else Y = 0
      if (a != 0.0f) {
        const float base = a * x + b;
        if (base >= 0.0f) return safe_powf(base, g);
      }
      return 0.0f;
    case 2:
      // Y = (aX+b)^g + c  if X >= -b/a, else Y = c
      if (a != 0.0f) {
        const float base = a * x + b;
        if (base >= 0.0f) return safe_powf(base, g) + c;
      }
      return c;
    case 3:
      // Y = (aX+b)^g  if X >= d, else Y = cX
      if (x >= d) {
        const float base = a * x + b;
        return (base >= 0.0f) ? safe_powf(base, g) : 0.0f;
      }
      return c * x;
    case 4:
      // Y = (aX+b)^g + e  if X >= d, else Y = cX + f
      if (x >= d) {
        const float base = a * x + b;
        return (base >= 0.0f) ? safe_powf(base, g) + e : e;
      }
      return c * x + f;
    default:
      return safe_powf(x, g);
  }
}

// IccTRC implementation
float IccTRC::apply(float v) const {
  if (!valid) return v;

  v = clamp01(v);

  if (is_parametric) {
    return clamp01(parametric.apply(v));
  } else if (is_gamma) {
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
      // Reserved bytes at 10-11, parameters start at 12

      trc.parametric.type = func_type;
      trc.parametric.g = read_s15Fixed16(tag_data + 12);

      if (func_type == 0) {
        // Type 0: Y = X^g  (1 param)
        trc.valid = true;
        trc.is_gamma = true;
        trc.gamma = trc.parametric.g;
      } else if (func_type == 1 && size >= 24) {
        // Type 1: Y = (aX+b)^g for X >= -b/a, else Y = 0  (3 params)
        trc.parametric.a = read_s15Fixed16(tag_data + 16);
        trc.parametric.b = read_s15Fixed16(tag_data + 20);
        trc.valid = true;
        trc.is_parametric = true;
      } else if (func_type == 2 && size >= 28) {
        // Type 2: Y = (aX+b)^g + c for X >= -b/a, else Y = c  (4 params)
        trc.parametric.a = read_s15Fixed16(tag_data + 16);
        trc.parametric.b = read_s15Fixed16(tag_data + 20);
        trc.parametric.c = read_s15Fixed16(tag_data + 24);
        trc.valid = true;
        trc.is_parametric = true;
      } else if (func_type == 3 && size >= 32) {
        // Type 3: Y = (aX+b)^g for X >= d, else Y = cX  (5 params)
        trc.parametric.a = read_s15Fixed16(tag_data + 16);
        trc.parametric.b = read_s15Fixed16(tag_data + 20);
        trc.parametric.c = read_s15Fixed16(tag_data + 24);
        trc.parametric.d = read_s15Fixed16(tag_data + 28);
        trc.valid = true;
        trc.is_parametric = true;
      } else if (func_type == 4 && size >= 36) {
        // Type 4: Y = (aX+b)^g + e for X >= d, else Y = cX + f  (7 params)
        trc.parametric.a = read_s15Fixed16(tag_data + 16);
        trc.parametric.b = read_s15Fixed16(tag_data + 20);
        trc.parametric.c = read_s15Fixed16(tag_data + 24);
        trc.parametric.d = read_s15Fixed16(tag_data + 28);
        trc.parametric.e = read_s15Fixed16(tag_data + 32);
        // f starts at offset 36 if present
        if (size >= 40)
          trc.parametric.f = read_s15Fixed16(tag_data + 36);
        trc.valid = true;
        trc.is_parametric = true;
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

// Parse mft2 (lut16Type) or mft1 (lut8Type) tag for CLUT-based transforms
// Returns true if CLUT was successfully parsed into the profile info
bool parse_clut_tag(const uint8_t* data, size_t data_size,
                    uint32_t offset, uint32_t tag_size,
                    IccProfileInfo* info) {
  if (offset + tag_size > data_size || tag_size < 52) return false;

  const uint8_t* t = data + offset;
  uint32_t type_sig = read_be_u32(t);

  // mft2 = 0x6D667432, mft1 = 0x6D667431
  bool is_16bit = (type_sig == 0x6D667432);
  bool is_8bit = (type_sig == 0x6D667431);
  if (!is_16bit && !is_8bit) return false;

  int input_channels = t[8];
  int output_channels = t[9];
  int grid_points = t[10];  // Same for all dimensions in lut8/lut16

  if (input_channels < 1 || input_channels > 4 ||
      output_channels < 1 || output_channels > 4 ||
      grid_points < 2) return false;

  info->clut_input_channels = input_channels;
  info->clut_output_channels = output_channels;
  info->clut_grid_points.assign(input_channels, grid_points);

  // Skip 3x3 matrix at bytes 12-47 (for PCS-to-PCS, not used for device-to-PCS)

  size_t pos = 48;
  if (is_16bit) {
    // lut16Type: bytes 48-49 = input table entries, 50-51 = output table entries
    int input_entries = read_be_u16(t + 48);
    int output_entries = read_be_u16(t + 50);
    pos = 52;

    // Input curves: input_channels * input_entries * 2 bytes
    info->clut_input_curves.resize(input_channels);
    for (int ch = 0; ch < input_channels; ++ch) {
      IccTRC& trc = info->clut_input_curves[ch];
      trc.valid = true;
      trc.curve.resize(input_entries);
      for (int i = 0; i < input_entries; ++i) {
        if (pos + 2 > tag_size) return false;
        trc.curve[i] = static_cast<float>(read_be_u16(t + pos)) / 65535.0f;
        pos += 2;
      }
    }

    // CLUT data: grid_points^input_channels * output_channels * 2 bytes
    size_t clut_entries = 1;
    for (int i = 0; i < input_channels; ++i) {
      if (!checked_mul_size(clut_entries, static_cast<size_t>(grid_points),
                            &clut_entries)) {
        return false;
      }
    }
    if (!checked_mul_size(clut_entries, static_cast<size_t>(output_channels),
                          &clut_entries)) {
      return false;
    }
    size_t clut_bytes = 0;
    if (!checked_mul_size(clut_entries, size_t(2), &clut_bytes) ||
        clut_bytes > (tag_size - pos)) {
      return false;
    }

    info->clut_data.resize(clut_entries);
    for (size_t i = 0; i < clut_entries; ++i) {
      if (pos + 2 > tag_size) return false;
      info->clut_data[i] = static_cast<float>(read_be_u16(t + pos)) / 65535.0f;
      pos += 2;
    }

    // Output curves: output_channels * output_entries * 2 bytes
    info->clut_output_curves.resize(output_channels);
    for (int ch = 0; ch < output_channels; ++ch) {
      IccTRC& trc = info->clut_output_curves[ch];
      trc.valid = true;
      trc.curve.resize(output_entries);
      for (int i = 0; i < output_entries; ++i) {
        if (pos + 2 > tag_size) return false;
        trc.curve[i] = static_cast<float>(read_be_u16(t + pos)) / 65535.0f;
        pos += 2;
      }
    }
  } else {
    // lut8Type: 256-entry tables, 1 byte per entry
    pos = 48;  // No extra counts in lut8
    int input_entries = 256;
    int output_entries = 256;

    info->clut_input_curves.resize(input_channels);
    for (int ch = 0; ch < input_channels; ++ch) {
      IccTRC& trc = info->clut_input_curves[ch];
      trc.valid = true;
      trc.curve.resize(input_entries);
      for (int i = 0; i < input_entries; ++i) {
        if (pos + 1 > tag_size) return false;
        trc.curve[i] = static_cast<float>(t[pos]) / 255.0f;
        pos += 1;
      }
    }

    size_t clut_entries = 1;
    for (int i = 0; i < input_channels; ++i) {
      if (!checked_mul_size(clut_entries, static_cast<size_t>(grid_points),
                            &clut_entries)) {
        return false;
      }
    }
    if (!checked_mul_size(clut_entries, static_cast<size_t>(output_channels),
                          &clut_entries) ||
        clut_entries > (tag_size - pos)) {
      return false;
    }

    info->clut_data.resize(clut_entries);
    for (size_t i = 0; i < clut_entries; ++i) {
      if (pos + 1 > tag_size) return false;
      info->clut_data[i] = static_cast<float>(t[pos]) / 255.0f;
      pos += 1;
    }

    info->clut_output_curves.resize(output_channels);
    for (int ch = 0; ch < output_channels; ++ch) {
      IccTRC& trc = info->clut_output_curves[ch];
      trc.valid = true;
      trc.curve.resize(output_entries);
      for (int i = 0; i < output_entries; ++i) {
        if (pos + 1 > tag_size) return false;
        trc.curve[i] = static_cast<float>(t[pos]) / 255.0f;
        pos += 1;
      }
    }
  }

  info->has_clut = true;
  return true;
}

// Multilinear interpolation in CLUT
// components: input values (normalized 0-1), n = input_channels
// Returns output values (output_channels floats)
void interpolate_clut(const IccProfileInfo& info, const float* components,
                      float* output) {
  int n_in = info.clut_input_channels;
  int n_out = info.clut_output_channels;
  const auto& grid = info.clut_grid_points;

  // Compute fractional grid positions and integer indices
  float frac[4] = {0};
  int idx[4] = {0};
  for (int i = 0; i < n_in && i < 4; ++i) {
    float v = std::max(0.0f, std::min(1.0f, components[i]));
    float pos = v * static_cast<float>(grid[i] - 1);
    idx[i] = std::min(static_cast<int>(pos), grid[i] - 2);
    frac[i] = pos - static_cast<float>(idx[i]);
  }

  // Compute strides for each dimension
  int stride[4] = {0};
  stride[0] = n_out;
  for (int i = 1; i < n_in; ++i) {
    stride[i] = stride[i - 1] * grid[i - 1];
  }

  // For CMYK (4D) or RGB (3D) - use multilinear interpolation
  // Number of corners = 2^n_in
  int num_corners = 1 << n_in;
  for (int c = 0; c < n_out; ++c) output[c] = 0.0f;

  for (int corner = 0; corner < num_corners; ++corner) {
    // Compute weight and base index for this corner
    float weight = 1.0f;
    int base = 0;
    for (int dim = 0; dim < n_in; ++dim) {
      int bit = (corner >> dim) & 1;
      base += (idx[dim] + bit) * stride[dim];
      weight *= bit ? frac[dim] : (1.0f - frac[dim]);
    }

    if (weight > 0.0f && base + n_out <= static_cast<int>(info.clut_data.size())) {
      for (int c = 0; c < n_out; ++c) {
        output[c] += weight * info.clut_data[base + c];
      }
    }
  }
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

  // For CMYK profiles (or RGB without matrix), try CLUT-based A2B0 tag
  if (info.color_space == ICC_SIG_CMYK ||
      (info.color_space == ICC_SIG_RGB && !info.is_matrix_profile)) {
    const IccTag* a2b0 = find_tag(ICC_TAG_A2B0);
    if (!a2b0) a2b0 = find_tag(ICC_TAG_A2B1);
    if (a2b0) {
      parse_clut_tag(data, size, a2b0->offset, a2b0->size, &info);
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
    if (profile.has_clut && profile.clut_output_channels >= 3) {
      // Apply input curves
      float adjusted[4];
      for (int i = 0; i < 4; ++i) {
        adjusted[i] = clamp01(components[i]);
        if (i < static_cast<int>(profile.clut_input_curves.size()) &&
            profile.clut_input_curves[i].valid) {
          adjusted[i] = profile.clut_input_curves[i].apply(adjusted[i]);
        }
      }

      // Interpolate CLUT
      float output[4] = {0};
      interpolate_clut(profile, adjusted, output);

      // Apply output curves
      for (int i = 0; i < profile.clut_output_channels && i < 4; ++i) {
        if (i < static_cast<int>(profile.clut_output_curves.size()) &&
            profile.clut_output_curves[i].valid) {
          output[i] = profile.clut_output_curves[i].apply(output[i]);
        }
      }

      // Output is in PCS (XYZ or Lab) - convert to sRGB
      if (profile.pcs == ICC_SIG_XYZ) {
        // CLUT output is XYZ (s15Fixed16-like, but normalized)
        // ICC PCS XYZ is encoded as X/1.0+32768/65535 range
        return xyz_to_rgb(XYZ(output[0] * 1.999969482421875f,
                               output[1] * 1.999969482421875f,
                               output[2] * 1.999969482421875f));
      } else if (profile.pcs == ICC_SIG_LAB) {
        Lab lab;
        lab.L = output[0] * 100.0f;
        lab.a = output[1] * 255.0f - 128.0f;
        lab.b = output[2] * 255.0f - 128.0f;
        std::vector<double> wp = {
            static_cast<double>(profile.white_point[0]),
            static_cast<double>(profile.white_point[1]),
            static_cast<double>(profile.white_point[2])};
        return lab_to_rgb(lab, wp);
      }
      // Assume output is already RGB
      return RGB(clamp01(output[0]), clamp01(output[1]), clamp01(output[2]));
    }
    // Fall back to simple CMYK conversion
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

  // Pre-parse ICC profile if available (once, before pixel loop)
  IccProfileInfo icc_profile;
  bool have_icc_profile = false;
  if (color_space.type == ColorSpaceType::ICCBased &&
      !color_space.icc_profile_data.empty()) {
    icc_profile = parse_icc_profile(color_space.icc_profile_data.data(),
                                    color_space.icc_profile_data.size());
    have_icc_profile = icc_profile.valid && icc_profile.is_matrix_profile;
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
          if (have_icc_profile) {
            rgb = iccbased_to_rgb(components, src_components, icc_profile);
          } else {
            rgb = iccbased_to_rgb_simple(components, src_components);
          }
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
          if (have_icc_profile) {
            rgb = iccbased_to_rgb(components, src_components, icc_profile);
          } else {
            rgb = iccbased_to_rgb_simple(components, src_components);
          }
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
