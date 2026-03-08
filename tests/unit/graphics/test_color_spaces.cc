/**
 * Color space conversion unit tests
 *
 * Tests the RGB working color space definitions, matrix construction,
 * transfer functions, and cross-space conversions for:
 * sRGB, Linear sRGB, Rec.709, Display P3, Rec.2020, ACEScg, ACES 2065-1
 */

#include "nanotest.hh"
#include "color-transform.hh"
#include <cmath>

using namespace nanopdf::color;

namespace {

bool near(float a, float b, float tol = 0.002f) {
  return std::abs(a - b) <= tol;
}

bool rgb_near(const RGB& a, const RGB& b, float tol = 0.005f) {
  return near(a.r, b.r, tol) && near(a.g, b.g, tol) && near(a.b, b.b, tol);
}

}  // namespace

// -------------------------------------------------------------------
// Color space definition tests
// -------------------------------------------------------------------

TEST_CASE("all color space names are non-null") {
  for (int i = 0; i <= static_cast<int>(RGBColorSpace::ACES2065_1); ++i) {
    auto cs = static_cast<RGBColorSpace>(i);
    const char* name = color_space_name(cs);
    CHECK(name != nullptr);
    CHECK(name[0] != '\0');
  }
}

TEST_CASE("sRGB primaries and transfer") {
  const auto& d = get_color_space_def(RGBColorSpace::sRGB);
  CHECK(near(d.rx, 0.64f));
  CHECK(near(d.ry, 0.33f));
  CHECK(near(d.wx, 0.3127f));
  CHECK(d.srgb_transfer);
  CHECK(!d.is_linear);
}

TEST_CASE("Display P3 primaries") {
  const auto& d = get_color_space_def(RGBColorSpace::DisplayP3);
  CHECK(near(d.rx, 0.68f));
  CHECK(near(d.gy, 0.69f));
  CHECK(d.srgb_transfer);
}

TEST_CASE("Rec.2020 primaries") {
  const auto& d = get_color_space_def(RGBColorSpace::Rec2020);
  CHECK(near(d.rx, 0.708f));
  CHECK(near(d.gy, 0.797f));
  CHECK(d.rec2020_transfer);
}

TEST_CASE("ACEScg is linear with D60 white") {
  const auto& d = get_color_space_def(RGBColorSpace::ACEScg);
  CHECK(d.is_linear);
  CHECK(near(d.wx, 0.32168f));
}

// -------------------------------------------------------------------
// Transfer function tests
// -------------------------------------------------------------------

TEST_CASE("sRGB transfer round-trip") {
  for (float v = 0.0f; v <= 1.0f; v += 0.05f) {
    float encoded = apply_transfer(v, RGBColorSpace::sRGB);
    float decoded = apply_inverse_transfer(encoded, RGBColorSpace::sRGB);
    CHECK(near(v, decoded, 0.001f));
  }
}

TEST_CASE("Rec.2020 transfer round-trip") {
  for (float v = 0.0f; v <= 1.0f; v += 0.05f) {
    float encoded = apply_transfer(v, RGBColorSpace::Rec2020);
    float decoded = apply_inverse_transfer(encoded, RGBColorSpace::Rec2020);
    CHECK(near(v, decoded, 0.001f));
  }
}

TEST_CASE("linear spaces are identity") {
  for (float v = 0.0f; v <= 1.0f; v += 0.1f) {
    CHECK(near(apply_transfer(v, RGBColorSpace::LinearSRGB), v));
    CHECK(near(apply_inverse_transfer(v, RGBColorSpace::LinearSRGB), v));
    CHECK(near(apply_transfer(v, RGBColorSpace::ACEScg), v));
    CHECK(near(apply_transfer(v, RGBColorSpace::ACES2065_1), v));
  }
}

TEST_CASE("Rec.709 gamma 2.4") {
  float encoded = apply_transfer(0.5f, RGBColorSpace::Rec709);
  CHECK(near(encoded, 0.7491f, 0.01f));
}

// -------------------------------------------------------------------
// Matrix construction tests
// -------------------------------------------------------------------

TEST_CASE("sRGB to XYZ white point") {
  Matrix3x3 mat = rgb_to_xyz_matrix(get_color_space_def(RGBColorSpace::LinearSRGB));
  float x, y, z;
  mat.apply(1, 1, 1, x, y, z);
  CHECK(near(x, 0.9505f, 0.01f));
  CHECK(near(y, 1.0f, 0.01f));
  CHECK(near(z, 1.089f, 0.01f));
}

TEST_CASE("sRGB matrix round-trip") {
  Matrix3x3 fwd = rgb_to_xyz_matrix(get_color_space_def(RGBColorSpace::LinearSRGB));
  Matrix3x3 inv = xyz_to_rgb_matrix(get_color_space_def(RGBColorSpace::LinearSRGB));
  Matrix3x3 identity = inv * fwd;
  CHECK(near(identity.m[0], 1.0f, 0.001f));
  CHECK(near(identity.m[4], 1.0f, 0.001f));
  CHECK(near(identity.m[8], 1.0f, 0.001f));
  CHECK(near(identity.m[1], 0.0f, 0.001f));
}

TEST_CASE("Display P3 to XYZ white point") {
  Matrix3x3 mat = rgb_to_xyz_matrix(get_color_space_def(RGBColorSpace::LinearDisplayP3));
  float x, y, z;
  mat.apply(1, 1, 1, x, y, z);
  CHECK(near(x, 0.9505f, 0.01f));
  CHECK(near(y, 1.0f, 0.01f));
  CHECK(near(z, 1.089f, 0.01f));
}

// -------------------------------------------------------------------
// Cross-space conversion tests
// -------------------------------------------------------------------

TEST_CASE("sRGB to sRGB is identity") {
  RGB src(0.5f, 0.3f, 0.8f);
  RGB dst = convert_rgb(src, RGBColorSpace::sRGB, RGBColorSpace::sRGB);
  CHECK(rgb_near(src, dst, 0.001f));
}

TEST_CASE("sRGB white to P3 white") {
  RGB white(1.0f, 1.0f, 1.0f);
  RGB p3 = convert_rgb(white, RGBColorSpace::sRGB, RGBColorSpace::DisplayP3);
  CHECK(rgb_near(p3, RGB(1.0f, 1.0f, 1.0f), 0.01f));
}

TEST_CASE("sRGB red in P3 gamut") {
  RGB red(1.0f, 0.0f, 0.0f);
  RGB p3 = convert_rgb(red, RGBColorSpace::sRGB, RGBColorSpace::DisplayP3);
  CHECK(p3.r < 1.0f);
  CHECK(p3.r > 0.8f);
  CHECK(p3.g >= 0.0f);
  CHECK(p3.g < 0.25f);
}

TEST_CASE("sRGB to linear sRGB round-trip") {
  RGB src(0.5f, 0.7f, 0.2f);
  RGB linear = convert_rgb(src, RGBColorSpace::sRGB, RGBColorSpace::LinearSRGB);
  RGB back = convert_rgb(linear, RGBColorSpace::LinearSRGB, RGBColorSpace::sRGB);
  CHECK(rgb_near(src, back, 0.001f));
}

TEST_CASE("sRGB green in Rec.2020 gamut") {
  RGB green(0.0f, 1.0f, 0.0f);
  RGB r2020 = convert_rgb(green, RGBColorSpace::sRGB, RGBColorSpace::Rec2020);
  CHECK(r2020.g < 1.0f);
  CHECK(r2020.g > 0.5f);
}

TEST_CASE("sRGB to ACEScg gray") {
  RGB gray(0.4663f, 0.4663f, 0.4663f);
  RGB aces = convert_rgb(gray, RGBColorSpace::sRGB, RGBColorSpace::ACEScg);
  CHECK(near(aces.r, aces.g, 0.02f));
  CHECK(near(aces.g, aces.b, 0.02f));
  CHECK(aces.r > 0.1f);
  CHECK(aces.r < 0.3f);
}

TEST_CASE("XYZ to Display P3 white") {
  XYZ white_xyz(0.9505f, 1.0f, 1.089f);
  RGB p3 = xyz_to_rgb(white_xyz, RGBColorSpace::DisplayP3);
  CHECK(rgb_near(p3, RGB(1.0f, 1.0f, 1.0f), 0.02f));
}

TEST_CASE("XYZ to ACEScg white") {
  XYZ white_xyz(0.9505f, 1.0f, 1.089f);
  RGB acescg = xyz_to_rgb(white_xyz, RGBColorSpace::ACEScg);
  CHECK(near(acescg.r, acescg.g, 0.05f));
  CHECK(near(acescg.g, acescg.b, 0.05f));
  CHECK(acescg.r > 0.9f);
  CHECK(acescg.r < 1.1f);
}

int main() {
  return nanotest::run_all_tests();
}
