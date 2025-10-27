#include "nanopdf.hh"

#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

using namespace nanopdf;

namespace {

Value make_stream(const std::string& content) {
  Value value;
  value.SetType(Value::STREAM);
  value.stream.data.assign(content.begin(), content.end());
  return value;
}

BaseFont* add_standard_font(Page& page, const std::string& name) {
  auto font = std::unique_ptr<BaseFont>(new BaseFont());
  font->subtype = "Type1";
  font->encoding = "StandardEncoding";
  BaseFont* raw = font.get();
  page.fonts[name] = std::move(font);
  return raw;
}

void run_text_extraction_scenario(const std::string& program,
                                  const std::function<void(BaseFont*)>& font_setup,
                                  const std::string& expected) {
  Pdf pdf;
  pdf.catalog.pages.emplace_back();
  Page& page = pdf.catalog.pages.back();

  BaseFont* font = add_standard_font(page, "F1");
  if (font_setup) {
    font_setup(font);
  }

  page.contents.push_back(make_stream(program));

  std::string extracted = extract_text_from_page(pdf, page);
  assert(extracted == expected);
}

}  // namespace

void test_standard_encoding_difference() {
  std::cout << "Testing StandardEncoding Differences handling..." << std::endl;

  const std::string ops = "BT /F1 12 Tf (A) Tj ET";
  run_text_extraction_scenario(
      ops,
      [](BaseFont* font) {
        font->encoding_differences[65] = "Agrave";
      },
      std::string(u8"\u00C0\n"));

  std::cout << "  Encoding Differences remap glyphs: PASS" << std::endl;
}

void test_standard_encoding_lookup() {
  std::cout << "Testing StandardEncoding fallback glyph names..." << std::endl;

  const std::string ops = "BT /F1 12 Tf (\\241) Tj ET";  // octal 241 -> code 0xA1
  run_text_extraction_scenario(ops, nullptr, std::string(u8"\u00A1\n"));

  std::cout << "  StandardEncoding lookup maps 0xA1 -> \u00A1: PASS" << std::endl;
}

void test_multi_codepoint_difference() {
  std::cout << "Testing multi-codepoint glyph name mapping..." << std::endl;

  const std::string ops = "BT /F1 12 Tf (Z) Tj ET";
  run_text_extraction_scenario(
      ops,
      [](BaseFont* font) {
        font->encoding_differences[90] = "dalethiriq";  // Maps to U+05D3 U+05B4
      },
      std::string(u8"\u05D3\u05B4\n"));

  std::cout << "  Multi-codepoint Differences mapping: PASS" << std::endl;
}

int main() {
  std::cout << "Phase 2 StandardEncoding Regression Tests" << std::endl;
  std::cout << "=========================================" << std::endl;

  test_standard_encoding_difference();
  test_standard_encoding_lookup();
  test_multi_codepoint_difference();

  std::cout << "All StandardEncoding tests passed!" << std::endl;
  return 0;
}
