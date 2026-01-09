// Test file for color space transformation (Phase 1.2)
#include <iostream>
#include <iomanip>
#include <cmath>

#include "src/color-transform.hh"

using namespace nanopdf::color;

// Helper to compare floats with tolerance
bool float_equal(float a, float b, float epsilon = 0.001f) {
  return std::fabs(a - b) < epsilon;
}

bool test_gray_to_rgb() {
  std::cout << "Testing gray_to_rgb..." << std::endl;

  // Test black
  RGB black = gray_to_rgb(0.0f);
  if (!float_equal(black.r, 0.0f) || !float_equal(black.g, 0.0f) || !float_equal(black.b, 0.0f)) {
    std::cerr << "  FAILED: Black test" << std::endl;
    return false;
  }

  // Test white
  RGB white = gray_to_rgb(1.0f);
  if (!float_equal(white.r, 1.0f) || !float_equal(white.g, 1.0f) || !float_equal(white.b, 1.0f)) {
    std::cerr << "  FAILED: White test" << std::endl;
    return false;
  }

  // Test 50% gray
  RGB gray = gray_to_rgb(0.5f);
  if (!float_equal(gray.r, 0.5f) || !float_equal(gray.g, 0.5f) || !float_equal(gray.b, 0.5f)) {
    std::cerr << "  FAILED: 50% gray test" << std::endl;
    return false;
  }

  std::cout << "  PASSED" << std::endl;
  return true;
}

bool test_cmyk_to_rgb() {
  std::cout << "Testing cmyk_to_rgb..." << std::endl;

  // Test pure black (K=1)
  RGB black = cmyk_to_rgb(0.0f, 0.0f, 0.0f, 1.0f);
  if (!float_equal(black.r, 0.0f) || !float_equal(black.g, 0.0f) || !float_equal(black.b, 0.0f)) {
    std::cerr << "  FAILED: Black test (K=1)" << std::endl;
    std::cerr << "    Got RGB(" << black.r << ", " << black.g << ", " << black.b << ")" << std::endl;
    return false;
  }

  // Test white (no CMYK)
  RGB white = cmyk_to_rgb(0.0f, 0.0f, 0.0f, 0.0f);
  if (!float_equal(white.r, 1.0f) || !float_equal(white.g, 1.0f) || !float_equal(white.b, 1.0f)) {
    std::cerr << "  FAILED: White test (no CMYK)" << std::endl;
    std::cerr << "    Got RGB(" << white.r << ", " << white.g << ", " << white.b << ")" << std::endl;
    return false;
  }

  // Test pure cyan (C=1, M=0, Y=0, K=0)
  RGB cyan = cmyk_to_rgb(1.0f, 0.0f, 0.0f, 0.0f);
  if (!float_equal(cyan.r, 0.0f) || !float_equal(cyan.g, 1.0f) || !float_equal(cyan.b, 1.0f)) {
    std::cerr << "  FAILED: Cyan test" << std::endl;
    std::cerr << "    Got RGB(" << cyan.r << ", " << cyan.g << ", " << cyan.b << ")" << std::endl;
    return false;
  }

  // Test pure magenta (C=0, M=1, Y=0, K=0)
  RGB magenta = cmyk_to_rgb(0.0f, 1.0f, 0.0f, 0.0f);
  if (!float_equal(magenta.r, 1.0f) || !float_equal(magenta.g, 0.0f) || !float_equal(magenta.b, 1.0f)) {
    std::cerr << "  FAILED: Magenta test" << std::endl;
    std::cerr << "    Got RGB(" << magenta.r << ", " << magenta.g << ", " << magenta.b << ")" << std::endl;
    return false;
  }

  // Test pure yellow (C=0, M=0, Y=1, K=0)
  RGB yellow = cmyk_to_rgb(0.0f, 0.0f, 1.0f, 0.0f);
  if (!float_equal(yellow.r, 1.0f) || !float_equal(yellow.g, 1.0f) || !float_equal(yellow.b, 0.0f)) {
    std::cerr << "  FAILED: Yellow test" << std::endl;
    std::cerr << "    Got RGB(" << yellow.r << ", " << yellow.g << ", " << yellow.b << ")" << std::endl;
    return false;
  }

  std::cout << "  PASSED" << std::endl;
  return true;
}

bool test_lab_to_xyz() {
  std::cout << "Testing lab_to_xyz..." << std::endl;

  // Test D65 white point (L=100, a=0, b=0)
  Lab white_lab(100.0f, 0.0f, 0.0f);
  std::vector<double> white_point;  // Empty = use D65 default

  XYZ white_xyz = lab_to_xyz(white_lab, white_point);

  // D65 white should be approximately (0.9505, 1.0, 1.0889)
  if (!float_equal(white_xyz.x, 0.9505f, 0.01f) ||
      !float_equal(white_xyz.y, 1.0f, 0.01f) ||
      !float_equal(white_xyz.z, 1.0889f, 0.01f)) {
    std::cerr << "  FAILED: White point test" << std::endl;
    std::cerr << "    Got XYZ(" << white_xyz.x << ", " << white_xyz.y << ", " << white_xyz.z << ")" << std::endl;
    std::cerr << "    Expected ~XYZ(0.9505, 1.0, 1.0889)" << std::endl;
    return false;
  }

  std::cout << "  PASSED" << std::endl;
  return true;
}

bool test_xyz_to_rgb() {
  std::cout << "Testing xyz_to_rgb..." << std::endl;

  // Test D65 white point
  XYZ white_xyz(0.9505f, 1.0f, 1.0889f);
  RGB white_rgb = xyz_to_rgb(white_xyz);

  // Should convert to RGB white (1, 1, 1)
  if (!float_equal(white_rgb.r, 1.0f, 0.01f) ||
      !float_equal(white_rgb.g, 1.0f, 0.01f) ||
      !float_equal(white_rgb.b, 1.0f, 0.01f)) {
    std::cerr << "  FAILED: White conversion test" << std::endl;
    std::cerr << "    Got RGB(" << white_rgb.r << ", " << white_rgb.g << ", " << white_rgb.b << ")" << std::endl;
    return false;
  }

  std::cout << "  PASSED" << std::endl;
  return true;
}

bool test_lab_to_rgb() {
  std::cout << "Testing lab_to_rgb (combined)..." << std::endl;

  // Test white
  Lab white_lab(100.0f, 0.0f, 0.0f);
  std::vector<double> white_point;
  RGB white_rgb = lab_to_rgb(white_lab, white_point);

  if (!float_equal(white_rgb.r, 1.0f, 0.01f) ||
      !float_equal(white_rgb.g, 1.0f, 0.01f) ||
      !float_equal(white_rgb.b, 1.0f, 0.01f)) {
    std::cerr << "  FAILED: White test" << std::endl;
    std::cerr << "    Got RGB(" << white_rgb.r << ", " << white_rgb.g << ", " << white_rgb.b << ")" << std::endl;
    return false;
  }

  // Test black
  Lab black_lab(0.0f, 0.0f, 0.0f);
  RGB black_rgb = lab_to_rgb(black_lab, white_point);

  if (!float_equal(black_rgb.r, 0.0f, 0.01f) ||
      !float_equal(black_rgb.g, 0.0f, 0.01f) ||
      !float_equal(black_rgb.b, 0.0f, 0.01f)) {
    std::cerr << "  FAILED: Black test" << std::endl;
    std::cerr << "    Got RGB(" << black_rgb.r << ", " << black_rgb.g << ", " << black_rgb.b << ")" << std::endl;
    return false;
  }

  std::cout << "  PASSED" << std::endl;
  return true;
}

bool test_icc_profile_header() {
  std::cout << "Testing ICC profile header parsing..." << std::endl;

  // Create a minimal fake ICC profile header
  uint8_t header[128] = {0};

  // Profile size (128 bytes) - big endian at offset 0
  header[0] = 0x00;
  header[1] = 0x00;
  header[2] = 0x00;
  header[3] = 0x80;  // 128

  // Color space 'RGB ' - big endian at offset 16
  header[16] = 0x52;  // 'R'
  header[17] = 0x47;  // 'G'
  header[18] = 0x42;  // 'B'
  header[19] = 0x20;  // ' '

  IccProfileInfo info = parse_icc_profile_header(header, sizeof(header));

  if (!info.valid) {
    std::cerr << "  FAILED: Profile not valid" << std::endl;
    if (!info.error.empty()) {
      std::cerr << "    Error: " << info.error << std::endl;
    }
    return false;
  }

  if (info.profile_size != 128) {
    std::cerr << "  FAILED: Profile size mismatch" << std::endl;
    std::cerr << "    Expected: 128, Got: " << info.profile_size << std::endl;
    return false;
  }

  if (info.color_space != 0x52474220) {  // 'RGB '
    std::cerr << "  FAILED: Color space mismatch" << std::endl;
    std::cerr << "    Expected: 0x52474220 (RGB), Got: 0x" << std::hex << info.color_space << std::dec << std::endl;
    return false;
  }

  if (info.num_components != 3) {
    std::cerr << "  FAILED: Component count mismatch" << std::endl;
    std::cerr << "    Expected: 3, Got: " << info.num_components << std::endl;
    return false;
  }

  std::cout << "  PASSED" << std::endl;
  return true;
}

int main() {
  std::cout << "=== nanopdf Color Transform Tests (Phase 1.2) ===" << std::endl << std::endl;

  int passed = 0;
  int total = 0;

  #define RUN_TEST(test) \
    total++; \
    if (test()) { \
      passed++; \
    } else { \
      std::cerr << "TEST FAILED: " #test << std::endl; \
    }

  RUN_TEST(test_gray_to_rgb);
  RUN_TEST(test_cmyk_to_rgb);
  RUN_TEST(test_lab_to_xyz);
  RUN_TEST(test_xyz_to_rgb);
  RUN_TEST(test_lab_to_rgb);
  RUN_TEST(test_icc_profile_header);

  std::cout << std::endl;
  std::cout << "=== Results ===" << std::endl;
  std::cout << "Passed: " << passed << "/" << total << std::endl;

  if (passed == total) {
    std::cout << "All tests PASSED!" << std::endl;
    return 0;
  } else {
    std::cout << "Some tests FAILED!" << std::endl;
    return 1;
  }
}
