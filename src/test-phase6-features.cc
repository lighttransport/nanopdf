#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include <stdexcept>

#include "nanopdf.hh"

using namespace nanopdf;

struct BitBuilder {
  std::vector<int> bits;

  void append(uint16_t code, uint8_t length) {
    for (int i = length - 1; i >= 0; --i) {
      bits.push_back((code >> i) & 1);
    }
  }

  std::vector<uint8_t> to_bytes() const {
    std::vector<uint8_t> bytes((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i) {
      if (bits[i]) {
        bytes[i / 8] |= static_cast<uint8_t>(1 << (7 - (i % 8)));
      }
    }
    return bytes;
  }
};

void append_white(BitBuilder& bb, int run) {
  struct Entry { int run; uint16_t code; uint8_t length; };
  static const Entry table[] = {
      {0, 0x35, 8},  {1, 0x07, 6},  {2, 0x07, 4},  {3, 0x08, 4},
      {4, 0x0B, 4},  {5, 0x0C, 4},  {6, 0x0E, 4},  {7, 0x0F, 4},
      {8, 0x13, 5},  {9, 0x14, 5},  {10, 0x07, 5}, {11, 0x08, 5},
      {12, 0x08, 6}, {13, 0x03, 6}, {14, 0x34, 6}, {15, 0x35, 6},
      {16, 0x2A, 6},
  };
  for (const auto& entry : table) {
    if (entry.run == run) {
      bb.append(entry.code, entry.length);
      return;
    }
  }
  throw std::runtime_error("Unsupported white run length");
}

void append_black(BitBuilder& bb, int run) {
  struct Entry { int run; uint16_t code; uint8_t length; };
  static const Entry table[] = {
      {0, 0x37, 10}, {1, 0x02, 3}, {2, 0x03, 2}, {3, 0x02, 2},
      {4, 0x03, 3},  {5, 0x03, 4}, {6, 0x02, 4}, {7, 0x03, 5},
      {8, 0x05, 6},  {9, 0x04, 6}, {10, 0x04, 7}, {11, 0x05, 7},
      {12, 0x07, 7}, {13, 0x04, 8}, {14, 0x07, 8}, {15, 0x18, 9},
      {16, 0x17, 10},
  };
  for (const auto& entry : table) {
    if (entry.run == run) {
      bb.append(entry.code, entry.length);
      return;
    }
  }
  throw std::runtime_error("Unsupported black run length");
}

std::vector<uint8_t> build_ccitt_sample() {
  BitBuilder bb;
  const uint16_t kEolCode = 0x001;  // 000000000001
  const uint8_t kEolLength = 12;

  // Line 1: 8 white pixels, 8 black pixels
  bb.append(kEolCode, kEolLength);
  append_white(bb, 8);
  append_black(bb, 8);

  // Line 2: 16 white pixels
  bb.append(kEolCode, kEolLength);
  append_white(bb, 16);

  // Final EOL to terminate block
  bb.append(kEolCode, kEolLength);

  // Pad with zeros to the next byte boundary
  while (bb.bits.size() % 8 != 0) {
    bb.bits.push_back(0);
  }

  return bb.to_bytes();
}

// Test CCITTFaxDecode filter
void test_ccittfax_decode() {
  std::cout << "Testing CCITTFaxDecode..." << std::endl;

  auto encoded = build_ccitt_sample();

  filters::DecodeParams params;
  params.k = 0;
  params.columns = 16;
  params.rows = 2;
  params.end_of_line = true;
  params.black_is_1 = false;

  DecodedStream result = filters::decode_ccittfax(encoded.data(), encoded.size(), params);
  assert(result.success);
  assert(result.data.size() == 4);
  assert(result.data[0] == 0xFF && result.data[1] == 0x00);
  assert(result.data[2] == 0xFF && result.data[3] == 0xFF);
  std::cout << "  CCITTFaxDecode (BlackIs1=0): PASS" << std::endl;

  params.black_is_1 = true;
  result = filters::decode_ccittfax(encoded.data(), encoded.size(), params);
  assert(result.success);
  assert(result.data.size() == 4);
  assert(result.data[0] == 0x00 && result.data[1] == 0xFF);
  assert(result.data[2] == 0x00 && result.data[3] == 0x00);
  std::cout << "  CCITTFaxDecode (BlackIs1=1): PASS" << std::endl;
}

// Test transparency and blending modes parsing
void test_transparency_blending() {
  std::cout << "Testing transparency and blending modes..." << std::endl;

  // Test blend mode parsing
  assert(parse_blend_mode("Normal") == BlendMode::Normal);
  assert(parse_blend_mode("Multiply") == BlendMode::Multiply);
  assert(parse_blend_mode("Screen") == BlendMode::Screen);
  assert(parse_blend_mode("Overlay") == BlendMode::Overlay);
  assert(parse_blend_mode("ColorDodge") == BlendMode::ColorDodge);
  assert(parse_blend_mode("Difference") == BlendMode::Difference);
  std::cout << "  Blend mode parsing: PASS" << std::endl;

  // Create a mock extended graphics state dictionary
  Dictionary gs_dict;

  // Add transparency parameters
  Value ca_val;
  ca_val.SetType(Value::NUMBER);
  ca_val.number = 0.5;
  gs_dict["ca"] = ca_val;

  Value CA_val;
  CA_val.SetType(Value::NUMBER);
  CA_val.number = 0.75;
  gs_dict["CA"] = CA_val;

  Value bm_val;
  bm_val.SetType(Value::NAME);
  bm_val.name = std::string("Multiply");
  gs_dict["BM"] = bm_val;

  // Parse the extended graphics state
  Pdf dummy_pdf;
  ExtendedGraphicsState state = parse_ext_gstate(dummy_pdf, gs_dict);

  assert(state.ca == 0.5);
  assert(state.CA == 0.75);
  assert(state.blend_mode == BlendMode::Multiply);
  std::cout << "  Extended graphics state parsing: PASS" << std::endl;

  // Test transparency group parsing
  Dictionary group_dict;

  Value s_val;
  s_val.SetType(Value::NAME);
  s_val.name = std::string("Transparency");
  group_dict["S"] = s_val;

  Value isolated_val;
  isolated_val.SetType(Value::BOOLEAN);
  isolated_val.boolean = true;
  group_dict["I"] = isolated_val;

  Value knockout_val;
  knockout_val.SetType(Value::BOOLEAN);
  knockout_val.boolean = false;
  group_dict["K"] = knockout_val;

  TransparencyGroup group = parse_transparency_group(dummy_pdf, group_dict);
  assert(group.isolated == true);
  assert(group.knockout == false);
  std::cout << "  Transparency group parsing: PASS" << std::endl;
}

// Test pattern and shading support
void test_patterns_shadings() {
  std::cout << "Testing patterns and shadings..." << std::endl;

  Pdf dummy_pdf;

  // Test tiling pattern
  Dictionary pattern_dict;

  Value pattern_type;
  pattern_type.SetType(Value::NUMBER);
  pattern_type.number = 1;  // Tiling pattern
  pattern_dict["PatternType"] = pattern_type;

  Value paint_type;
  paint_type.SetType(Value::NUMBER);
  paint_type.number = 1;  // Colored tiles
  pattern_dict["PaintType"] = paint_type;

  Value tiling_type;
  tiling_type.SetType(Value::NUMBER);
  tiling_type.number = 1;  // Constant spacing
  pattern_dict["TilingType"] = tiling_type;

  Value xstep;
  xstep.SetType(Value::NUMBER);
  xstep.number = 100;
  pattern_dict["XStep"] = xstep;

  Value ystep;
  ystep.SetType(Value::NUMBER);
  ystep.number = 100;
  pattern_dict["YStep"] = ystep;

  auto pattern = parse_pattern(dummy_pdf, pattern_dict);
  assert(pattern != nullptr);
  assert(pattern->type == PatternType::Tiling);
  assert(pattern->tiling != nullptr);
  assert(pattern->tiling->paint_type == TilingPaintType::ColoredTiles);
  assert(pattern->tiling->x_step == 100);
  assert(pattern->tiling->y_step == 100);
  std::cout << "  Tiling pattern parsing: PASS" << std::endl;

  // Test axial shading
  Dictionary shading_dict;

  Value shading_type;
  shading_type.SetType(Value::NUMBER);
  shading_type.number = 2;  // Axial shading
  shading_dict["ShadingType"] = shading_type;

  Value coords;
  coords.SetType(Value::ARRAY);
  Value x0, y0, x1, y1;
  x0.SetType(Value::NUMBER);
  y0.SetType(Value::NUMBER);
  x1.SetType(Value::NUMBER);
  y1.SetType(Value::NUMBER);
  x0.number = 0;
  y0.number = 0;
  x1.number = 100;
  y1.number = 100;
  coords.array.push_back(x0);
  coords.array.push_back(y0);
  coords.array.push_back(x1);
  coords.array.push_back(y1);
  shading_dict["Coords"] = coords;

  auto shading = parse_shading(dummy_pdf, shading_dict);
  assert(shading != nullptr);
  assert(shading->type == ShadingType::Axial);
  assert(shading->coords.size() == 4);
  assert(shading->coords[2] == 100);
  std::cout << "  Axial shading parsing: PASS" << std::endl;
}

// Test tagged PDF structure parsing
void test_tagged_pdf() {
  std::cout << "Testing tagged PDF structures..." << std::endl;

  // Test structure type parsing
  assert(parse_structure_type("Document") == StructureType::Document);
  assert(parse_structure_type("P") == StructureType::P);
  assert(parse_structure_type("H1") == StructureType::H1);
  assert(parse_structure_type("Table") == StructureType::Table);
  assert(parse_structure_type("TD") == StructureType::TD);
  assert(parse_structure_type("Figure") == StructureType::Figure);
  assert(parse_structure_type("Unknown") == StructureType::Unknown);
  std::cout << "  Structure type parsing: PASS" << std::endl;

  // Test role map
  Dictionary role_map_dict;

  Value heading_val;
  heading_val.SetType(Value::NAME);
  heading_val.name = std::string("H1");
  role_map_dict["Heading"] = heading_val;

  Value para_val;
  para_val.SetType(Value::NAME);
  para_val.name = std::string("P");
  role_map_dict["Paragraph"] = para_val;

  RoleMap role_map = parse_role_map(role_map_dict);
  assert(role_map.get_standard_type("Heading") == "H1");
  assert(role_map.get_standard_type("Paragraph") == "P");
  assert(role_map.get_standard_type("Unknown") == "Unknown");
  std::cout << "  Role map parsing: PASS" << std::endl;

  // Test structure element parsing
  Pdf dummy_pdf;
  Dictionary elem_dict;

  Value s_val;
  s_val.SetType(Value::NAME);
  s_val.name = std::string("P");
  elem_dict["S"] = s_val;

  Value id_val;
  id_val.SetType(Value::STRING);
  id_val.str = "para1";
  elem_dict["ID"] = id_val;

  Value title_val;
  title_val.SetType(Value::STRING);
  title_val.str = "First Paragraph";
  elem_dict["T"] = title_val;

  Value lang_val;
  lang_val.SetType(Value::STRING);
  lang_val.str = "en-US";
  elem_dict["Lang"] = lang_val;

  Value alt_val;
  alt_val.SetType(Value::STRING);
  alt_val.str = "Alternative text for paragraph";
  elem_dict["Alt"] = alt_val;

  Value elem_val;
  elem_val.SetType(Value::DICTIONARY);
  elem_val.dict = elem_dict;

  auto element = parse_structure_element(dummy_pdf, elem_val);
  assert(element != nullptr);
  assert(element->type == StructureType::P);
  assert(element->type_string == "P");
  assert(element->id == "para1");
  assert(element->title == "First Paragraph");
  assert(element->lang == "en-US");
  assert(element->alt_text == "Alternative text for paragraph");
  std::cout << "  Structure element parsing: PASS" << std::endl;

  // Test structure attributes
  Dictionary attr_dict;

  Value placement_val;
  placement_val.SetType(Value::NAME);
  placement_val.name = std::string("Block");
  attr_dict["Placement"] = placement_val;

  Value text_align_val;
  text_align_val.SetType(Value::NAME);
  text_align_val.name = std::string("Center");
  attr_dict["TextAlign"] = text_align_val;

  Value width_val;
  width_val.SetType(Value::NUMBER);
  width_val.number = 200;
  attr_dict["Width"] = width_val;

  Value height_val;
  height_val.SetType(Value::NUMBER);
  height_val.number = 50;
  attr_dict["Height"] = height_val;

  StructureElementAttributes attrs = parse_structure_attributes(dummy_pdf, attr_dict);
  assert(attrs.placement == "Block");
  assert(attrs.text_align == "Center");
  assert(attrs.width == 200);
  assert(attrs.height == 50);
  std::cout << "  Structure attributes parsing: PASS" << std::endl;

  // Test marked content properties
  Dictionary props_dict;

  Value mcid_val;
  mcid_val.SetType(Value::NUMBER);
  mcid_val.number = 42;
  props_dict["MCID"] = mcid_val;

  Value tag_val;
  tag_val.SetType(Value::NAME);
  tag_val.name = std::string("P");
  props_dict["Tag"] = tag_val;

  Value actual_text_val;
  actual_text_val.SetType(Value::STRING);
  actual_text_val.str = "Actual text content";
  props_dict["ActualText"] = actual_text_val;

  MarkedContentProperties props = parse_marked_content_props(props_dict);
  assert(props.mcid == 42);
  assert(props.tag == "P");
  assert(props.actual_text == "Actual text content");
  std::cout << "  Marked content properties parsing: PASS" << std::endl;

  // Test is_tagged_pdf function
  Pdf pdf;
  Value marked_val;
  marked_val.SetType(Value::BOOLEAN);
  marked_val.boolean = true;
  pdf.catalog.mark_info["Marked"] = marked_val;

  assert(is_tagged_pdf(pdf) == true);
  std::cout << "  Tagged PDF detection: PASS" << std::endl;
}

int main() {
  std::cout << "Phase 6 Features Test Suite" << std::endl;
  std::cout << "============================" << std::endl;

  test_ccittfax_decode();
  test_transparency_blending();
  test_patterns_shadings();
  test_tagged_pdf();

  std::cout << "\nAll Phase 6 tests completed!" << std::endl;
  return 0;
}
