// Text layout analysis unit tests
#include "nanotest.hh"
#include "text-layout.hh"

#include <cmath>
#include <vector>

using namespace nanopdf;

namespace {

TextChar make_char(char c, double x, double y, double font_size = 12.0) {
  TextChar ch;
  ch.unicode = static_cast<uint32_t>(c);
  ch.x = x;
  ch.y = y;
  ch.width = font_size * 0.6;
  ch.height = font_size;
  ch.font_size = font_size;
  ch.font_name = "Helvetica";
  ch.rotation = 0.0;
  ch.matrix[0] = 1; ch.matrix[1] = 0;
  ch.matrix[2] = 0; ch.matrix[3] = 1;
  ch.matrix[4] = x; ch.matrix[5] = y;
  return ch;
}

std::vector<TextChar> make_text_at(const char* text, double x, double y) {
  std::vector<TextChar> chars;
  for (size_t i = 0; text[i]; ++i) {
    chars.push_back(make_char(text[i], x, y));
    x += 7.2;
  }
  return chars;
}

} // namespace

TEST_SUITE("TextLayout") {

TEST_CASE("Single line text") {
  auto chars = make_text_at("Hello World", 100.0, 700.0);
  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0);

  REQUIRE(page != nullptr);
  CHECK_EQ(page->lines.size(), size_t(1));
  CHECK_EQ(page->lines[0].chars.size(), size_t(11));
  CHECK_EQ(page->lines[0].get_text(), std::string("Hello World"));
}

TEST_CASE("Multi-line text") {
  std::vector<TextChar> chars;
  auto l1 = make_text_at("First line", 100.0, 700.0);
  auto l2 = make_text_at("Second line", 100.0, 680.0);
  auto l3 = make_text_at("Third line", 100.0, 660.0);
  chars.insert(chars.end(), l1.begin(), l1.end());
  chars.insert(chars.end(), l2.begin(), l2.end());
  chars.insert(chars.end(), l3.begin(), l3.end());

  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0);

  REQUIRE(page != nullptr);
  CHECK_EQ(page->lines.size(), size_t(3));
  CHECK_EQ(page->lines[0].reading_order, 0);
  CHECK_EQ(page->lines[1].reading_order, 1);
  CHECK_EQ(page->lines[2].reading_order, 2);
}

TEST_CASE("Word segmentation") {
  std::vector<TextChar> chars;
  const char* text = "Hello World Test";
  double x = 100.0;
  for (size_t i = 0; text[i]; ++i) {
    chars.push_back(make_char(text[i], x, 700.0));
    x += (text[i] == ' ') ? 14.4 : 7.2;
  }

  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0);

  REQUIRE(page != nullptr);
  CHECK_EQ(page->lines.size(), size_t(1));
  CHECK(page->words.size() >= size_t(1));
}

TEST_CASE("Two-column layout") {
  std::vector<TextChar> chars;
  auto l1 = make_text_at("Left column", 100.0, 700.0);
  auto l2 = make_text_at("Line two", 100.0, 680.0);
  auto r1 = make_text_at("Right column", 350.0, 700.0);
  auto r2 = make_text_at("Line four", 350.0, 680.0);
  chars.insert(chars.end(), l1.begin(), l1.end());
  chars.insert(chars.end(), l2.begin(), l2.end());
  chars.insert(chars.end(), r1.begin(), r1.end());
  chars.insert(chars.end(), r2.begin(), r2.end());

  TextLayoutOptions options;
  options.column_gap_threshold = 200.0;
  options.detect_columns = true;

  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0, options);

  REQUIRE(page != nullptr);
  CHECK_EQ(page->lines.size(), size_t(4));
}

TEST_CASE("Baseline alignment tolerance") {
  std::vector<TextChar> chars;
  const char* text = "Baseline";
  double x = 100.0;
  for (size_t i = 0; text[i]; ++i) {
    double y = 700.0 + (i % 2 ? 0.5 : -0.5);
    chars.push_back(make_char(text[i], x, y));
    x += 7.2;
  }

  TextLayoutOptions options;
  options.baseline_tolerance = 2.0;

  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0, options);

  REQUIRE(page != nullptr);
  CHECK_EQ(page->lines.size(), size_t(1));
  CHECK_EQ(page->lines[0].get_text(), std::string("Baseline"));
}

TEST_CASE("Empty page") {
  std::vector<TextChar> chars;
  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0);

  REQUIRE(page != nullptr);
  CHECK(page->lines.empty());
  CHECK(page->words.empty());
  CHECK(page->get_text().empty());
}

TEST_CASE("Spatial query") {
  auto chars = make_text_at("Test", 100.0, 700.0);
  auto page = extract_text_layout_with_collector(chars, 612.0, 792.0);

  std::string found = page->get_text_in_rect(90.0, 690.0, 150.0, 710.0);
  CHECK_FALSE(found.empty());

  std::string not_found = page->get_text_in_rect(500.0, 500.0, 600.0, 600.0);
  CHECK(not_found.empty());
}

} // TEST_SUITE
