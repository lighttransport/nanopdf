#include "nanopdf.hh"

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace nanopdf;

namespace {

Value make_stream(const char* content) {
  Value val;
  val.SetType(Value::STREAM);
  val.stream.data.assign(content, content + std::strlen(content));
  return val;
}

void build_standard_encoding_page(Pdf& pdf, Page& page) {
  auto font = std::unique_ptr<BaseFont>(new BaseFont());
  font->subtype = "Type1";
  font->base_font = "Times-Roman";
  font->encoding = "StandardEncoding";

  font->encoding_differences[0x97] = "emdash";
  font->encoding_differences[0x9C] = "oe";
  font->encoding_differences[0xA1] = "exclamdown";
  font->encoding_differences[0xBF] = "questiondown";
  font->encoding_differences[0xDF] = "germandbls";
  font->encoding_differences[0xE0] = "agrave";
  font->encoding_differences[0xE1] = "aacute";
  font->encoding_differences[0xE6] = "ae";
  font->encoding_differences[0xE7] = "ccedilla";
  font->encoding_differences[0xE9] = "eacute";
  font->encoding_differences[0xF1] = "ntilde";
  font->encoding_differences[0xF3] = "oacute";
  font->encoding_differences[0xF6] = "odieresis";
  font->encoding_differences[0xFC] = "udieresis";

  page.fonts["F1"] = std::move(font);
  page.fonts_loaded = true;  // Prevent lazy loading from overwriting test font

  const char* content =
      "BT\n"
      "/F1 14 Tf\n"
      "16 TL\n"
      "72 720 Td\n"
      "<a1486f6c612c207365f16f722120bf43f36d6f20657374e13f> Tj\n"
      "T*\n"
      "<4c27e9636f6e6f6d6965206672616ee7616973652065737420656e206861757373652e> Tj\n"
      "T*\n"
      "<53747261df652c206772fcdf2c206772f6df2c206661e76164652c2064e96ae0207675> Tj\n"
      "T*\n"
      "<666920666c206666692066666c206c6967617475726573209720e6209c> Tj\n"
      "ET\n";

  page.contents.push_back(make_stream(content));
}

}  // namespace

void test_standard_encoding_text_extraction() {
  std::cout << "Testing StandardEncoding text extraction scenario..." << std::endl;

  Pdf pdf;
  Page page;
  build_standard_encoding_page(pdf, page);

  std::string text = extract_text_from_page(pdf, page);
  const std::string expected =
      u8"¡Hola, señor! ¿Cómo está?\n"
      u8"L'économie française est en hausse.\n"
      u8"Straße, grüß, größ, façade, déjà vu\n"
      u8"fi fl ffi ffl ligatures — æ œ\n";

  assert(text == expected);
  std::cout << "  Extracted text matches expectation: PASS" << std::endl;
}

int main() {
  std::cout << "=== Phase 2 Text Extraction Tests ===" << std::endl;
  test_standard_encoding_text_extraction();
  std::cout << "All text extraction tests passed!" << std::endl;
  return 0;
}

