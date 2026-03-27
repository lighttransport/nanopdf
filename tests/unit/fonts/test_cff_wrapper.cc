// CFF wrapper unit tests
#include "nanotest.hh"
#include "cff-wrapper.hh"

#include <vector>

TEST_SUITE("CffWrapper") {

TEST_CASE("is_raw_cff rejects null") {
  CHECK_FALSE(cff_wrapper::is_raw_cff(nullptr, 0));
}

TEST_CASE("is_raw_cff rejects too small") {
  uint8_t data[] = {1, 0};
  CHECK_FALSE(cff_wrapper::is_raw_cff(data, 2));
}

TEST_CASE("is_raw_cff accepts CFF v1.0 header") {
  uint8_t data[] = {1, 0, 4, 1}; // major=1, minor=0, hdrSize=4, offSize=1
  CHECK(cff_wrapper::is_raw_cff(data, 4));
}

TEST_CASE("is_raw_cff rejects non-CFF") {
  uint8_t data[] = {0, 1, 0, 0}; // Not CFF
  CHECK_FALSE(cff_wrapper::is_raw_cff(data, 4));
}

TEST_CASE("build_cid_to_gid_map returns empty for null input") {
  auto result = cff_wrapper::build_cid_to_gid_map(nullptr, 0);
  CHECK(result.empty());
}

TEST_CASE("build_cid_to_gid_map returns empty for non-CFF data") {
  uint8_t data[] = {0xFF, 0xFE, 0xFD, 0xFC};
  auto result = cff_wrapper::build_cid_to_gid_map(data, 4);
  CHECK(result.empty());
}

TEST_CASE("build_cid_to_gid_map returns empty for non-CID font") {
  // Minimal CFF header + empty Name INDEX (non-CID fonts return empty)
  // Header: major=1, minor=0, hdrSize=4, offSize=1
  // Name INDEX: count=1, offSize=1, offsets=[1,2], data='A'
  // Top DICT INDEX: count=1, offSize=1, offsets=[1,2], data=single byte (no ROS op)
  uint8_t data[] = {
    1, 0, 4, 1,          // CFF header
    0, 1, 1, 1, 2, 'A',  // Name INDEX: count=1, offSize=1, off=[1,2], data='A'
    0, 1, 1, 1, 2, 139,  // Top DICT INDEX: count=1, offSize=1, off=[1,2], data=operand 0
  };
  auto result = cff_wrapper::build_cid_to_gid_map(data, sizeof(data));
  // Non-CID font -> empty (caller uses identity mapping)
  CHECK(result.empty());
}

TEST_CASE("wrap_cff_in_opentype produces valid OTF header") {
  std::vector<uint8_t> cff = {1, 0, 4, 1}; // Minimal CFF
  auto otf = cff_wrapper::wrap_cff_in_opentype(cff);

  // Check OTF signature "OTTO"
  REQUIRE(otf.size() >= 28 + cff.size());
  CHECK_EQ(otf[0], uint8_t('O'));
  CHECK_EQ(otf[1], uint8_t('T'));
  CHECK_EQ(otf[2], uint8_t('T'));
  CHECK_EQ(otf[3], uint8_t('O'));

  // numTables = 1
  CHECK_EQ(otf[4], uint8_t(0));
  CHECK_EQ(otf[5], uint8_t(1));

  // Table tag "CFF "
  CHECK_EQ(otf[12], uint8_t('C'));
  CHECK_EQ(otf[13], uint8_t('F'));
  CHECK_EQ(otf[14], uint8_t('F'));
  CHECK_EQ(otf[15], uint8_t(' '));

  // CFF data is copied at offset 28
  CHECK_EQ(otf[28], uint8_t(1));
  CHECK_EQ(otf[29], uint8_t(0));
}

TEST_CASE("wrap_cff_in_opentype empty input") {
  std::vector<uint8_t> empty;
  auto otf = cff_wrapper::wrap_cff_in_opentype(empty);
  CHECK_EQ(otf.size(), size_t(28));
  CHECK_EQ(otf[0], uint8_t('O'));
}

} // TEST_SUITE
