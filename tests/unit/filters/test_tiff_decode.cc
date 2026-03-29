// TIFF decoder unit tests
#include "nanotest.hh"
#include "tiff-decoder.hh"

#include <vector>

using namespace nanopdf::tiff;

namespace {

// Create a minimal valid TIFF file in memory (2x2 grayscale uncompressed)
std::vector<uint8_t> create_minimal_tiff() {
  std::vector<uint8_t> tiff_data;

  // TIFF Header: little-endian
  tiff_data.push_back(0x49); tiff_data.push_back(0x49);
  tiff_data.push_back(0x2A); tiff_data.push_back(0x00);
  tiff_data.push_back(0x08); tiff_data.push_back(0x00);
  tiff_data.push_back(0x00); tiff_data.push_back(0x00);

  // IFD: 10 entries
  tiff_data.push_back(0x0A); tiff_data.push_back(0x00);

  auto add_tag = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
    tiff_data.push_back(tag & 0xFF); tiff_data.push_back((tag >> 8) & 0xFF);
    tiff_data.push_back(type & 0xFF); tiff_data.push_back((type >> 8) & 0xFF);
    tiff_data.push_back(count & 0xFF); tiff_data.push_back((count >> 8) & 0xFF);
    tiff_data.push_back((count >> 16) & 0xFF); tiff_data.push_back((count >> 24) & 0xFF);
    tiff_data.push_back(value & 0xFF); tiff_data.push_back((value >> 8) & 0xFF);
    tiff_data.push_back((value >> 16) & 0xFF); tiff_data.push_back((value >> 24) & 0xFF);
  };

  add_tag(0x0100, 3, 1, 2);    // ImageWidth = 2
  add_tag(0x0101, 3, 1, 2);    // ImageLength = 2
  add_tag(0x0102, 3, 1, 8);    // BitsPerSample = 8
  add_tag(0x0103, 3, 1, 1);    // Compression = none
  add_tag(0x0106, 3, 1, 1);    // PhotometricInterpretation = BlackIsZero
  add_tag(0x0111, 4, 1, 134);  // StripOffsets
  add_tag(0x0115, 3, 1, 1);    // SamplesPerPixel = 1
  add_tag(0x0116, 3, 1, 2);    // RowsPerStrip = 2
  add_tag(0x0117, 4, 1, 4);    // StripByteCounts = 4
  add_tag(0x011A, 5, 1, 138);  // XResolution offset

  // Next IFD = 0
  tiff_data.push_back(0x00); tiff_data.push_back(0x00);
  tiff_data.push_back(0x00); tiff_data.push_back(0x00);

  // Pixel data at offset 134
  tiff_data.push_back(0); tiff_data.push_back(85);
  tiff_data.push_back(170); tiff_data.push_back(255);

  // XResolution rational (72/1) at offset 138
  tiff_data.push_back(72); tiff_data.push_back(0);
  tiff_data.push_back(0); tiff_data.push_back(0);
  tiff_data.push_back(1); tiff_data.push_back(0);
  tiff_data.push_back(0); tiff_data.push_back(0);

  return tiff_data;
}

} // namespace

TEST_SUITE("TiffDecode") {

TEST_CASE("Decode minimal 2x2 grayscale TIFF") {
  auto tiff_data = create_minimal_tiff();
  TiffDecoder decoder;
  TiffDecodeResult result = decoder.decode(tiff_data.data(), tiff_data.size());

  REQUIRE(result.success);
  CHECK_EQ(result.width, uint32_t(2));
  CHECK_EQ(result.height, uint32_t(2));
  CHECK_EQ(result.components, 1);
  CHECK_EQ(result.bits_per_component, 8);
  REQUIRE(result.pixels.size() == 4);
  CHECK_EQ(result.pixels[0], uint8_t(0));
  CHECK_EQ(result.pixels[1], uint8_t(85));
  CHECK_EQ(result.pixels[2], uint8_t(170));
  CHECK_EQ(result.pixels[3], uint8_t(255));
}

TEST_CASE("Reject null data") {
  TiffDecoder decoder;
  TiffDecodeResult result = decoder.decode(nullptr, 100);
  CHECK_FALSE(result.success);
}

TEST_CASE("Reject zero size") {
  TiffDecoder decoder;
  uint8_t dummy[10] = {0};
  TiffDecodeResult result = decoder.decode(dummy, 0);
  CHECK_FALSE(result.success);
}

TEST_CASE("Reject invalid magic bytes") {
  TiffDecoder decoder;
  uint8_t invalid[10] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  TiffDecodeResult result = decoder.decode(invalid, sizeof(invalid));
  CHECK_FALSE(result.success);
}

TEST_CASE("get_info returns correct dimensions") {
  auto tiff_data = create_minimal_tiff();
  TiffDecoder decoder;
  int width = 0, height = 0, components = 0;
  bool success = decoder.get_info(tiff_data.data(), tiff_data.size(), width, height, components);
  REQUIRE(success);
  CHECK_EQ(width, 2);
  CHECK_EQ(height, 2);
  CHECK_EQ(components, 1);
}

} // TEST_SUITE
