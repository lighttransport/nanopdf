// CFF wrapper unit tests
#include "nanotest.hh"
#include "cff-wrapper.hh"

#include <vector>

namespace {

void append_u16(std::vector<uint8_t>* data, uint16_t value) {
  data->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  data->push_back(static_cast<uint8_t>(value & 0xff));
}

void append_dict_int(std::vector<uint8_t>* data, int value) {
  if (value >= -107 && value <= 107) {
    data->push_back(static_cast<uint8_t>(value + 139));
    return;
  }

  data->push_back(28);
  append_u16(data, static_cast<uint16_t>(static_cast<int16_t>(value)));
}

void append_dict_short_int(std::vector<uint8_t>* data, int value) {
  data->push_back(28);
  append_u16(data, static_cast<uint16_t>(static_cast<int16_t>(value)));
}

std::vector<uint8_t> make_cid_cff_with_charset(
    const std::vector<uint8_t>& charset_data,
    uint16_t num_glyphs) {
  std::vector<uint8_t> dict_data;
  append_dict_int(&dict_data, 0);
  append_dict_int(&dict_data, 0);
  append_dict_int(&dict_data, 0);
  dict_data.push_back(12);
  dict_data.push_back(30);

  const int charset_offset = 28;
  const int charstrings_offset =
      charset_offset + static_cast<int>(charset_data.size());
  append_dict_short_int(&dict_data, charset_offset);
  dict_data.push_back(15);
  append_dict_short_int(&dict_data, charstrings_offset);
  dict_data.push_back(17);

  std::vector<uint8_t> data = {
      1, 0, 4, 1,  // Header
      0, 0,        // Name INDEX
      0, 1, 1, 1, static_cast<uint8_t>(dict_data.size() + 1),
  };
  data.insert(data.end(), dict_data.begin(), dict_data.end());
  data.push_back(0);
  data.push_back(0);  // String INDEX
  data.push_back(0);
  data.push_back(0);  // Global Subr INDEX
  data.insert(data.end(), charset_data.begin(), charset_data.end());
  append_u16(&data, num_glyphs);  // CharStrings INDEX count
  return data;
}

}  // namespace

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

TEST_CASE("format 1 charset overflow does not wrap low CIDs") {
  std::vector<uint8_t> charset = {1, 0xff, 0xfd, 4};
  auto data = make_cid_cff_with_charset(charset, 6);

  auto result = cff_wrapper::build_cid_to_gid_map(data.data(), data.size());
  REQUIRE_EQ(result.size(), size_t(65536));
  CHECK_EQ(result[0], uint16_t(0));
  CHECK_EQ(result[1], uint16_t(0));
  CHECK_EQ(result[65533], uint16_t(1));
  CHECK_EQ(result[65534], uint16_t(2));
  CHECK_EQ(result[65535], uint16_t(3));
}

TEST_CASE("format 2 charset overflow does not wrap low CIDs") {
  std::vector<uint8_t> charset = {2, 0xff, 0xfd, 0x00, 0x04};
  auto data = make_cid_cff_with_charset(charset, 6);

  auto result = cff_wrapper::build_cid_to_gid_map(data.data(), data.size());
  REQUIRE_EQ(result.size(), size_t(65536));
  CHECK_EQ(result[0], uint16_t(0));
  CHECK_EQ(result[1], uint16_t(0));
  CHECK_EQ(result[65533], uint16_t(1));
  CHECK_EQ(result[65534], uint16_t(2));
  CHECK_EQ(result[65535], uint16_t(3));
}

} // TEST_SUITE
