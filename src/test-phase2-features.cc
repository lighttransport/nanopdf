#include "nanopdf.hh"
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace nanopdf;

// Test text state and positioning
void test_text_state() {
  std::cout << "Testing TextState structure..." << std::endl;

  TextState state;

  // Test initial values
  assert(state.text_matrix[0] == 1.0);
  assert(state.text_matrix[1] == 0.0);
  assert(state.text_matrix[2] == 0.0);
  assert(state.text_matrix[3] == 1.0);
  assert(state.text_matrix[4] == 0.0);
  assert(state.text_matrix[5] == 0.0);
  assert(state.char_spacing == 0.0);
  assert(state.word_spacing == 0.0);
  assert(state.horizontal_scaling == 100.0);
  assert(state.render_mode == TextRenderingMode::Fill);
  std::cout << "  Initial state: PASSED" << std::endl;

  // Test reset
  state.char_spacing = 5.0;
  state.word_spacing = 10.0;
  state.font_size = 12.0;
  state.current_text = "test";
  state.reset();
  assert(state.char_spacing == 0.0);
  assert(state.word_spacing == 0.0);
  assert(state.font_size == 0.0);
  assert(state.current_text.empty());
  std::cout << "  Reset function: PASSED" << std::endl;

  // Test text matrix operations
  state.text_matrix[4] = 100.0;
  state.text_matrix[5] = 200.0;
  assert(state.text_matrix[4] == 100.0);
  assert(state.text_matrix[5] == 200.0);
  std::cout << "  Matrix operations: PASSED" << std::endl;

  std::cout << "TextState tests completed successfully!" << std::endl << std::endl;
}

// Test text rendering modes
void test_text_rendering_modes() {
  std::cout << "Testing TextRenderingMode enum..." << std::endl;

  assert(static_cast<int>(TextRenderingMode::Fill) == 0);
  assert(static_cast<int>(TextRenderingMode::Stroke) == 1);
  assert(static_cast<int>(TextRenderingMode::FillAndStroke) == 2);
  assert(static_cast<int>(TextRenderingMode::Invisible) == 3);
  assert(static_cast<int>(TextRenderingMode::FillAndClip) == 4);
  assert(static_cast<int>(TextRenderingMode::StrokeAndClip) == 5);
  assert(static_cast<int>(TextRenderingMode::FillStrokeAndClip) == 6);
  assert(static_cast<int>(TextRenderingMode::Clip) == 7);

  std::cout << "  All rendering modes correctly defined: PASSED" << std::endl;
  std::cout << "TextRenderingMode tests completed!" << std::endl << std::endl;
}

// Test CMap structure
void test_cmap() {
  std::cout << "Testing CMap structure..." << std::endl;

  CMap cmap;
  cmap.name = "TestCMap";
  cmap.registry = "Adobe";
  cmap.ordering = "Identity";
  cmap.supplement = 0;

  // Add some mappings
  cmap.code_to_unicode[65] = 0x0041;  // A
  cmap.code_to_unicode[66] = 0x0042;  // B

  // Add a range mapping
  cmap.range_mappings[std::make_pair(0x20, 0x7E)] = 0x0020;

  // Test direct mapping
  assert(cmap.map_code_to_unicode(65) == 0x0041);
  assert(cmap.map_code_to_unicode(66) == 0x0042);
  std::cout << "  Direct mapping: PASSED" << std::endl;

  // Test range mapping
  assert(cmap.map_code_to_unicode(0x20) == 0x0020);
  assert(cmap.map_code_to_unicode(0x21) == 0x0021);
  assert(cmap.map_code_to_unicode(0x7E) == 0x007E);
  std::cout << "  Range mapping: PASSED" << std::endl;

  // Test fallback (no mapping found)
  assert(cmap.map_code_to_unicode(255) == 255);
  std::cout << "  Fallback mapping: PASSED" << std::endl;

  std::cout << "CMap tests completed successfully!" << std::endl << std::endl;
}

// Test Type0 font structure
void test_type0_font() {
  std::cout << "Testing Type0Font structure..." << std::endl;

  Type0Font font;
  assert(font.subtype == "Type0");

  font.registry = "Adobe";
  font.ordering = "Identity";
  font.supplement = 0;
  font.base_font = "HeiseiKakuGo-W5";

  // Add CID to GID mapping
  font.cid_to_gid_map.resize(256, 0);
  font.cid_to_gid_map[100] = 150;

  assert(font.registry == "Adobe");
  assert(font.ordering == "Identity");
  assert(font.cid_to_gid_map.size() == 256);
  assert(font.cid_to_gid_map[100] == 150);

  std::cout << "  Type0Font structure: PASSED" << std::endl;
  std::cout << "Type0Font tests completed!" << std::endl << std::endl;
}

// Test Type3 font structure
void test_type3_font() {
  std::cout << "Testing Type3Font structure..." << std::endl;

  Type3Font font;
  assert(font.subtype == "Type3");

  // Test font matrix default values
  assert(font.font_matrix.size() == 6);
  assert(font.font_matrix[0] == 0.001);
  assert(font.font_matrix[3] == 0.001);

  // Set font bbox
  font.font_bbox = {0, 0, 1000, 1000};
  assert(font.font_bbox.size() == 4);
  assert(font.font_bbox[2] == 1000);

  std::cout << "  Type3Font structure: PASSED" << std::endl;
  std::cout << "Type3Font tests completed!" << std::endl << std::endl;
}

// Test font substitution
void test_font_substitution() {
  std::cout << "Testing FontSubstitution..." << std::endl;

  FontSubstitution sub;
  sub.original_name = "Times-Roman";
  sub.substitute_name = "serif";
  sub.substitute_path = "/usr/share/fonts/liberation/LiberationSerif-Regular.ttf";
  sub.is_system_font = true;

  assert(sub.original_name == "Times-Roman");
  assert(sub.substitute_name == "serif");
  assert(sub.is_system_font == true);

  std::cout << "  FontSubstitution structure: PASSED" << std::endl;
  std::cout << "FontSubstitution tests completed!" << std::endl << std::endl;
}

// Test text extraction helpers
namespace {

Value make_stream(const char* content) {
  Value val;
  val.SetType(Value::STREAM);
  val.stream.data.assign(content, content + std::strlen(content));
  return val;
}

void setup_simple_font(Page& page, const std::string& font_name) {
  auto font = std::unique_ptr<BaseFont>(new BaseFont());
  font->subtype = "Type1";
  font->base_font = "Helvetica";
  font->encoding = "WinAnsiEncoding";
  page.fonts[font_name] = std::move(font);
  page.fonts_loaded = true;
}

}  // namespace

// Test basic text extraction with Tj operator
void test_text_extraction_tj() {
  std::cout << "Testing text extraction (Tj operator)..." << std::endl;

  Pdf pdf;
  Page page;
  setup_simple_font(page, "F1");

  page.contents.push_back(make_stream(
      "BT\n"
      "/F1 12 Tf\n"
      "72 720 Td\n"
      "(Hello World) Tj\n"
      "ET\n"));

  std::string text = extract_text_from_page(pdf, page);
  assert(text.find("Hello World") != std::string::npos);
  std::cout << "  Tj operator: PASSED" << std::endl;

  std::cout << "Tj text extraction tests completed!" << std::endl << std::endl;
}

// Test text extraction with multiple Tj calls
void test_text_extraction_multiple_tj() {
  std::cout << "Testing text extraction (multiple Tj calls)..." << std::endl;

  Pdf pdf;
  Page page;
  setup_simple_font(page, "F1");

  page.contents.push_back(make_stream(
      "BT\n"
      "/F1 12 Tf\n"
      "72 720 Td\n"
      "(Hello ) Tj\n"
      "(World) Tj\n"
      "ET\n"));

  std::string text = extract_text_from_page(pdf, page);
  assert(text.find("Hello") != std::string::npos);
  assert(text.find("World") != std::string::npos);
  std::cout << "  Multiple Tj: PASSED" << std::endl;

  std::cout << "Multiple Tj text extraction tests completed!" << std::endl << std::endl;
}

// Test text extraction with multiple lines (T* operator)
void test_text_extraction_multiline() {
  std::cout << "Testing text extraction (multiline T*)..." << std::endl;

  Pdf pdf;
  Page page;
  setup_simple_font(page, "F1");

  page.contents.push_back(make_stream(
      "BT\n"
      "/F1 12 Tf\n"
      "14 TL\n"
      "72 720 Td\n"
      "(Line One) Tj\n"
      "T*\n"
      "(Line Two) Tj\n"
      "ET\n"));

  std::string text = extract_text_from_page(pdf, page);
  assert(text.find("Line One") != std::string::npos);
  assert(text.find("Line Two") != std::string::npos);
  std::cout << "  Multiline T*: PASSED" << std::endl;

  std::cout << "Multiline text extraction tests completed!" << std::endl << std::endl;
}

// Test text extraction with ' and " operators
void test_text_extraction_quote_operators() {
  std::cout << "Testing text extraction (' and \" operators)..." << std::endl;

  Pdf pdf;
  Page page;
  setup_simple_font(page, "F1");

  // ' operator: move to next line and show text
  page.contents.push_back(make_stream(
      "BT\n"
      "/F1 12 Tf\n"
      "14 TL\n"
      "72 720 Td\n"
      "(First) Tj\n"
      "(Second) '\n"
      "ET\n"));

  std::string text = extract_text_from_page(pdf, page);
  assert(text.find("First") != std::string::npos);
  assert(text.find("Second") != std::string::npos);
  std::cout << "  Quote operator ('): PASSED" << std::endl;

  std::cout << "Quote operator text extraction tests completed!" << std::endl
            << std::endl;
}

// Test text extraction with Td positioning
void test_text_extraction_positioning() {
  std::cout << "Testing text extraction (Td/TD positioning)..." << std::endl;

  Pdf pdf;
  Page page;
  setup_simple_font(page, "F1");

  page.contents.push_back(make_stream(
      "BT\n"
      "/F1 12 Tf\n"
      "72 720 Td\n"
      "(Part A) Tj\n"
      "200 0 Td\n"
      "(Part B) Tj\n"
      "ET\n"));

  std::string text = extract_text_from_page(pdf, page);
  assert(text.find("Part A") != std::string::npos);
  assert(text.find("Part B") != std::string::npos);
  std::cout << "  Td positioning: PASSED" << std::endl;

  std::cout << "Positioning text extraction tests completed!" << std::endl
            << std::endl;
}

// Test hex string text extraction
void test_text_extraction_hex_string() {
  std::cout << "Testing text extraction (hex strings)..." << std::endl;

  Pdf pdf;
  Page page;
  setup_simple_font(page, "F1");

  // "Hi" in hex = 4869
  page.contents.push_back(make_stream(
      "BT\n"
      "/F1 12 Tf\n"
      "72 720 Td\n"
      "<4869> Tj\n"
      "ET\n"));

  std::string text = extract_text_from_page(pdf, page);
  assert(text.find("Hi") != std::string::npos);
  std::cout << "  Hex string: PASSED" << std::endl;

  std::cout << "Hex string text extraction tests completed!" << std::endl
            << std::endl;
}

// Test empty text block
void test_text_extraction_empty() {
  std::cout << "Testing text extraction (empty/no text)..." << std::endl;

  Pdf pdf;
  Page page;

  // No content at all
  std::string text = extract_text_from_page(pdf, page);
  // Should not crash, may return empty
  std::cout << "  No content: PASSED (got " << text.size() << " chars)" << std::endl;

  // Empty BT/ET block
  Page page2;
  setup_simple_font(page2, "F1");
  page2.contents.push_back(make_stream("BT\nET\n"));
  std::string text2 = extract_text_from_page(pdf, page2);
  std::cout << "  Empty BT/ET: PASSED" << std::endl;

  std::cout << "Empty text extraction tests completed!" << std::endl << std::endl;
}

int main() {
  std::cout << "=== Phase 2 Feature Tests ===" << std::endl << std::endl;

  test_text_state();
  test_text_rendering_modes();
  test_cmap();
  test_type0_font();
  test_type3_font();
  test_font_substitution();
  test_text_extraction_tj();
  test_text_extraction_multiple_tj();
  test_text_extraction_multiline();
  test_text_extraction_quote_operators();
  test_text_extraction_positioning();
  test_text_extraction_hex_string();
  test_text_extraction_empty();

  std::cout << "=== All Phase 2 tests passed! ===" << std::endl;

  return 0;
}