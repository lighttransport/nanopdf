// Color space transformation unit tests
#include "nanotest.hh"
#include "color-transform.hh"

#include <cmath>
#include <vector>

using namespace nanopdf::color;

namespace {
bool float_equal(float a, float b, float epsilon = 0.001f) {
  return std::fabs(a - b) < epsilon;
}
} // namespace

TEST_SUITE("ColorTransform") {

TEST_CASE("gray_to_rgb black") {
  RGB black = gray_to_rgb(0.0f);
  CHECK(float_equal(black.r, 0.0f));
  CHECK(float_equal(black.g, 0.0f));
  CHECK(float_equal(black.b, 0.0f));
}

TEST_CASE("gray_to_rgb white") {
  RGB white = gray_to_rgb(1.0f);
  CHECK(float_equal(white.r, 1.0f));
  CHECK(float_equal(white.g, 1.0f));
  CHECK(float_equal(white.b, 1.0f));
}

TEST_CASE("gray_to_rgb 50% gray") {
  RGB gray = gray_to_rgb(0.5f);
  CHECK(float_equal(gray.r, 0.5f));
  CHECK(float_equal(gray.g, 0.5f));
  CHECK(float_equal(gray.b, 0.5f));
}

TEST_CASE("cmyk_to_rgb black K=1") {
  RGB black = cmyk_to_rgb(0.0f, 0.0f, 0.0f, 1.0f);
  CHECK(float_equal(black.r, 0.0f));
  CHECK(float_equal(black.g, 0.0f));
  CHECK(float_equal(black.b, 0.0f));
}

TEST_CASE("cmyk_to_rgb white") {
  RGB white = cmyk_to_rgb(0.0f, 0.0f, 0.0f, 0.0f);
  CHECK(float_equal(white.r, 1.0f));
  CHECK(float_equal(white.g, 1.0f));
  CHECK(float_equal(white.b, 1.0f));
}

TEST_CASE("cmyk_to_rgb pure cyan") {
  RGB cyan = cmyk_to_rgb(1.0f, 0.0f, 0.0f, 0.0f);
  CHECK(float_equal(cyan.r, 0.0f));
  CHECK(float_equal(cyan.g, 1.0f));
  CHECK(float_equal(cyan.b, 1.0f));
}

TEST_CASE("cmyk_to_rgb pure magenta") {
  RGB magenta = cmyk_to_rgb(0.0f, 1.0f, 0.0f, 0.0f);
  CHECK(float_equal(magenta.r, 1.0f));
  CHECK(float_equal(magenta.g, 0.0f));
  CHECK(float_equal(magenta.b, 1.0f));
}

TEST_CASE("cmyk_to_rgb pure yellow") {
  RGB yellow = cmyk_to_rgb(0.0f, 0.0f, 1.0f, 0.0f);
  CHECK(float_equal(yellow.r, 1.0f));
  CHECK(float_equal(yellow.g, 1.0f));
  CHECK(float_equal(yellow.b, 0.0f));
}

TEST_CASE("lab_to_xyz D65 white") {
  Lab white_lab(100.0f, 0.0f, 0.0f);
  std::vector<double> white_point;
  XYZ white_xyz = lab_to_xyz(white_lab, white_point);
  CHECK(float_equal(white_xyz.x, 0.9505f, 0.01f));
  CHECK(float_equal(white_xyz.y, 1.0f, 0.01f));
  CHECK(float_equal(white_xyz.z, 1.0889f, 0.01f));
}

TEST_CASE("xyz_to_rgb D65 white") {
  XYZ white_xyz(0.9505f, 1.0f, 1.0889f);
  RGB white_rgb = xyz_to_rgb(white_xyz);
  CHECK(float_equal(white_rgb.r, 1.0f, 0.01f));
  CHECK(float_equal(white_rgb.g, 1.0f, 0.01f));
  CHECK(float_equal(white_rgb.b, 1.0f, 0.01f));
}

TEST_CASE("lab_to_rgb white") {
  Lab white_lab(100.0f, 0.0f, 0.0f);
  std::vector<double> white_point;
  RGB white_rgb = lab_to_rgb(white_lab, white_point);
  CHECK(float_equal(white_rgb.r, 1.0f, 0.01f));
  CHECK(float_equal(white_rgb.g, 1.0f, 0.01f));
  CHECK(float_equal(white_rgb.b, 1.0f, 0.01f));
}

TEST_CASE("lab_to_rgb black") {
  Lab black_lab(0.0f, 0.0f, 0.0f);
  std::vector<double> white_point;
  RGB black_rgb = lab_to_rgb(black_lab, white_point);
  CHECK(float_equal(black_rgb.r, 0.0f, 0.01f));
  CHECK(float_equal(black_rgb.g, 0.0f, 0.01f));
  CHECK(float_equal(black_rgb.b, 0.0f, 0.01f));
}

TEST_CASE("ICC profile header parsing") {
  uint8_t header[128] = {0};
  header[0] = 0x00; header[1] = 0x00; header[2] = 0x00; header[3] = 0x80;
  header[16] = 0x52; header[17] = 0x47; header[18] = 0x42; header[19] = 0x20;

  IccProfileInfo info = parse_icc_profile_header(header, sizeof(header));
  REQUIRE(info.valid);
  CHECK_EQ(info.profile_size, uint32_t(128));
  CHECK_EQ(info.color_space, uint32_t(0x52474220));
  CHECK_EQ(info.num_components, 3);
}

TEST_CASE("ICC parametric curve avoids NaN for negative pow base") {
  IccParametricCurve curve;
  curve.type = 3;
  curve.g = 2.2f;
  curve.a = -2.0f;
  curve.b = 0.1f;
  curve.c = 0.5f;
  curve.d = 0.0f;

  float value = curve.apply(1.0f);
  CHECK(std::isfinite(value));
  CHECK_EQ(value, 0.0f);
}

TEST_CASE("ICC parametric type 4 falls back when pow base is negative") {
  IccParametricCurve curve;
  curve.type = 4;
  curve.g = 2.4f;
  curve.a = -1.5f;
  curve.b = 0.1f;
  curve.c = 0.25f;
  curve.d = 0.0f;
  curve.e = 0.2f;
  curve.f = 0.1f;

  float value = curve.apply(1.0f);
  CHECK(std::isfinite(value));
  CHECK_EQ(value, 0.2f);
}

} // TEST_SUITE
