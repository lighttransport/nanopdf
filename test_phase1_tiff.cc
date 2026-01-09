// Test file for TIFF decoder (Phase 1.3)
#include <fstream>
#include <iostream>
#include <vector>

#include "src/tiff-decoder.hh"

using namespace nanopdf::tiff;

// Create a minimal valid TIFF file in memory for testing
// This creates a tiny 2x2 grayscale uncompressed TIFF
std::vector<uint8_t> create_minimal_tiff() {
  std::vector<uint8_t> tiff_data;

  // TIFF Header (8 bytes)
  // Little-endian magic number
  tiff_data.push_back(0x49);  // 'I'
  tiff_data.push_back(0x49);  // 'I'
  // Version (42)
  tiff_data.push_back(0x2A);
  tiff_data.push_back(0x00);
  // Offset to first IFD (will be at offset 8)
  tiff_data.push_back(0x08);
  tiff_data.push_back(0x00);
  tiff_data.push_back(0x00);
  tiff_data.push_back(0x00);

  // IFD (Image File Directory) starts at offset 8
  // Number of directory entries
  tiff_data.push_back(0x0A);  // 10 entries
  tiff_data.push_back(0x00);

  // Helper lambda to add a TIFF tag entry
  auto add_tag = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
    // Tag
    tiff_data.push_back(tag & 0xFF);
    tiff_data.push_back((tag >> 8) & 0xFF);
    // Type
    tiff_data.push_back(type & 0xFF);
    tiff_data.push_back((type >> 8) & 0xFF);
    // Count
    tiff_data.push_back(count & 0xFF);
    tiff_data.push_back((count >> 8) & 0xFF);
    tiff_data.push_back((count >> 16) & 0xFF);
    tiff_data.push_back((count >> 24) & 0xFF);
    // Value/Offset
    tiff_data.push_back(value & 0xFF);
    tiff_data.push_back((value >> 8) & 0xFF);
    tiff_data.push_back((value >> 16) & 0xFF);
    tiff_data.push_back((value >> 24) & 0xFF);
  };

  // Tag 256 (0x100): ImageWidth = 2
  add_tag(0x0100, 3, 1, 2);

  // Tag 257 (0x101): ImageLength (height) = 2
  add_tag(0x0101, 3, 1, 2);

  // Tag 258 (0x102): BitsPerSample = 8
  add_tag(0x0102, 3, 1, 8);

  // Tag 259 (0x103): Compression = 1 (none)
  add_tag(0x0103, 3, 1, 1);

  // Tag 262 (0x106): PhotometricInterpretation = 1 (BlackIsZero)
  add_tag(0x0106, 3, 1, 1);

  // Tag 273 (0x111): StripOffsets (offset to pixel data)
  // Pixel data will start at offset 134 (header 8 + entries count 2 + 10*12 bytes + next IFD offset 4)
  add_tag(0x0111, 4, 1, 134);

  // Tag 277 (0x115): SamplesPerPixel = 1 (grayscale)
  add_tag(0x0115, 3, 1, 1);

  // Tag 278 (0x116): RowsPerStrip = 2 (all rows in one strip)
  add_tag(0x0116, 3, 1, 2);

  // Tag 279 (0x117): StripByteCounts = 4 (2x2 pixels, 1 byte each)
  add_tag(0x0117, 4, 1, 4);

  // Tag 282 (0x11A): XResolution (offset to rational)
  // XResolution will be at offset 138 (after 4 bytes of pixel data)
  add_tag(0x011A, 5, 1, 138);

  // Offset to next IFD (0 = no more IFDs)
  tiff_data.push_back(0x00);
  tiff_data.push_back(0x00);
  tiff_data.push_back(0x00);
  tiff_data.push_back(0x00);

  // Pixel data at offset 134: 2x2 grayscale image
  // [0, 85, 170, 255] = black, dark gray, light gray, white
  tiff_data.push_back(0);     // (0,0)
  tiff_data.push_back(85);    // (1,0)
  tiff_data.push_back(170);   // (0,1)
  tiff_data.push_back(255);   // (1,1)

  // XResolution rational value at offset 138 (72/1 = 72 DPI)
  tiff_data.push_back(72);
  tiff_data.push_back(0);
  tiff_data.push_back(0);
  tiff_data.push_back(0);
  tiff_data.push_back(1);
  tiff_data.push_back(0);
  tiff_data.push_back(0);
  tiff_data.push_back(0);

  return tiff_data;
}

bool test_minimal_tiff() {
  std::cout << "Testing minimal TIFF decoding..." << std::endl;

  std::vector<uint8_t> tiff_data = create_minimal_tiff();

  TiffDecoder decoder;
  TiffDecodeResult result = decoder.decode(tiff_data.data(), tiff_data.size());

  if (!result.success) {
    std::cerr << "  FAILED: " << result.error << std::endl;
    return false;
  }

  // Verify dimensions
  if (result.width != 2 || result.height != 2) {
    std::cerr << "  FAILED: Expected 2x2, got " << result.width << "x" << result.height << std::endl;
    return false;
  }

  // Verify components (grayscale)
  if (result.components != 1) {
    std::cerr << "  FAILED: Expected 1 component (grayscale), got " << result.components << std::endl;
    return false;
  }

  // Verify bits per component
  if (result.bits_per_component != 8) {
    std::cerr << "  FAILED: Expected 8 bits per component, got " << result.bits_per_component << std::endl;
    return false;
  }

  // Verify pixel data size
  if (result.pixels.size() != 4) {
    std::cerr << "  FAILED: Expected 4 bytes of pixel data, got " << result.pixels.size() << std::endl;
    return false;
  }

  // Verify pixel values
  if (result.pixels[0] != 0 || result.pixels[1] != 85 ||
      result.pixels[2] != 170 || result.pixels[3] != 255) {
    std::cerr << "  FAILED: Pixel values don't match" << std::endl;
    std::cerr << "    Expected: [0, 85, 170, 255]" << std::endl;
    std::cerr << "    Got: ["
              << static_cast<int>(result.pixels[0]) << ", "
              << static_cast<int>(result.pixels[1]) << ", "
              << static_cast<int>(result.pixels[2]) << ", "
              << static_cast<int>(result.pixels[3]) << "]" << std::endl;
    return false;
  }

  std::cout << "  PASSED (2x2 grayscale image decoded successfully)" << std::endl;
  return true;
}

bool test_invalid_input() {
  std::cout << "Testing invalid input handling..." << std::endl;

  TiffDecoder decoder;

  // Test null data
  TiffDecodeResult result1 = decoder.decode(nullptr, 100);
  if (result1.success) {
    std::cerr << "  FAILED: Should reject null data" << std::endl;
    return false;
  }

  // Test zero size
  uint8_t dummy[10] = {0};
  TiffDecodeResult result2 = decoder.decode(dummy, 0);
  if (result2.success) {
    std::cerr << "  FAILED: Should reject zero size" << std::endl;
    return false;
  }

  // Test invalid magic bytes
  uint8_t invalid_magic[10] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  TiffDecodeResult result3 = decoder.decode(invalid_magic, sizeof(invalid_magic));
  if (result3.success) {
    std::cerr << "  FAILED: Should reject invalid magic bytes" << std::endl;
    return false;
  }

  std::cout << "  PASSED (all invalid inputs rejected)" << std::endl;
  return true;
}

bool test_get_info() {
  std::cout << "Testing get_info..." << std::endl;

  std::vector<uint8_t> tiff_data = create_minimal_tiff();

  TiffDecoder decoder;
  int width = 0, height = 0, components = 0;
  bool success = decoder.get_info(tiff_data.data(), tiff_data.size(),
                                   width, height, components);

  if (!success) {
    std::cerr << "  FAILED: get_info returned false" << std::endl;
    return false;
  }

  if (width != 2 || height != 2 || components != 1) {
    std::cerr << "  FAILED: get_info returned wrong values" << std::endl;
    std::cerr << "    Expected: 2x2, 1 component" << std::endl;
    std::cerr << "    Got: " << width << "x" << height << ", " << components << " components" << std::endl;
    return false;
  }

  std::cout << "  PASSED" << std::endl;
  return true;
}

bool test_from_file(const char* filename) {
  std::cout << "Testing TIFF file: " << filename << "..." << std::endl;

  std::ifstream ifs(filename, std::ios::binary);
  if (!ifs) {
    std::cout << "  SKIPPED (file not found)" << std::endl;
    return true;  // Not a failure, just skip
  }

  // Read entire file
  ifs.seekg(0, std::ios::end);
  size_t file_size = ifs.tellg();
  ifs.seekg(0, std::ios::beg);

  std::vector<uint8_t> data(file_size);
  ifs.read(reinterpret_cast<char*>(data.data()), file_size);
  ifs.close();

  TiffDecoder decoder;
  TiffDecodeResult result = decoder.decode(data.data(), data.size());

  if (!result.success) {
    std::cerr << "  FAILED: " << result.error << std::endl;
    return false;
  }

  std::cout << "  PASSED (decoded " << result.width << "x" << result.height
            << ", " << result.components << " components, "
            << result.bits_per_component << " bpc)" << std::endl;
  return true;
}

int main(int argc, char** argv) {
  std::cout << "=== nanopdf TIFF Decoder Tests (Phase 1.3) ===" << std::endl << std::endl;

  int passed = 0;
  int total = 0;

  #define RUN_TEST(test) \
    total++; \
    if (test) { \
      passed++; \
    } else { \
      std::cerr << "TEST FAILED: " #test << std::endl; \
    }

  RUN_TEST(test_minimal_tiff());
  RUN_TEST(test_invalid_input());
  RUN_TEST(test_get_info());

  // Test real TIFF files if provided
  for (int i = 1; i < argc; ++i) {
    RUN_TEST(test_from_file(argv[i]));
  }

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
