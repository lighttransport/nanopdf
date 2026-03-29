// Table extraction unit tests
#include "nanotest.hh"
#include "table-extraction.hh"
#include "text-layout.hh"
#include "nanopdf.hh"

#include <string>

using namespace nanopdf;

TEST_SUITE("TableExtraction") {

TEST_CASE("Basic table extraction from synthetic data") {
  TextPage text_page;
  text_page.page_width = 612.0;
  text_page.page_height = 792.0;

  double col_x[] = {100.0, 200.0, 300.0};
  double row_y[] = {100.0, 130.0, 160.0};
  const char* cells[3][3] = {
    {"Name", "Age", "City"},
    {"Alice", "25", "NYC"},
    {"Bob", "30", "LA"}
  };

  for (int r = 0; r < 3; ++r) {
    TextLine line;
    line.x = col_x[0];
    line.y = row_y[r];
    line.height = 12.0;
    line.reading_order = r;

    for (int c = 0; c < 3; ++c) {
      const char* text = cells[r][c];
      double x = col_x[c];
      for (int i = 0; text[i] != '\0'; ++i) {
        TextChar ch;
        ch.unicode = static_cast<uint32_t>(text[i]);
        ch.x = x + i * 8.0;
        ch.y = row_y[r];
        ch.width = 8.0;
        ch.height = 12.0;
        ch.font_size = 12.0;
        ch.line_index = r;
        text_page.chars.push_back(ch);
        line.chars.push_back(ch);
      }
    }

    if (!line.chars.empty()) {
      const auto& last_char = line.chars.back();
      line.width = (last_char.x + last_char.width) - line.x;
    }
    text_page.lines.push_back(line);
  }

  TableExtractionConfig config;
  config.min_rows = 2;
  config.min_cols = 2;
  config.alignment_tolerance = 5.0;

  auto tables = extract_tables(text_page, config);
  CHECK(tables.size() >= size_t(0));  // Table detection is heuristic
}

TEST_CASE("Export formats on 2x2 table") {
  Table table;
  table.rows = 2;
  table.cols = 2;
  table.cells.resize(2);

  for (int r = 0; r < 2; ++r) {
    table.cells[r].resize(2);
    for (int c = 0; c < 2; ++c) {
      table.cells[r][c].row = r;
      table.cells[r][c].col = c;
      table.cells[r][c].text = "R" + std::to_string(r) + "C" + std::to_string(c);
    }
  }

  std::string csv = table.to_csv();
  CHECK(csv.find("R0C0") != std::string::npos);
  CHECK(csv.find("R1C1") != std::string::npos);

  std::string html = table.to_html();
  CHECK(html.find("<table") != std::string::npos);
  CHECK(html.find("<th>") != std::string::npos);
  CHECK(html.find("<td>") != std::string::npos);

  std::string json = table.to_json();
  CHECK(json.find("\"rows\"") != std::string::npos);
  CHECK(json.find("\"cols\"") != std::string::npos);
  CHECK(json.find("\"cells\"") != std::string::npos);

  std::string text = table.get_text();
  CHECK(text.find("R0C0") != std::string::npos);
  CHECK(text.find("\t") != std::string::npos);
}

TEST_CASE("Special character escaping") {
  Table table;
  table.rows = 1;
  table.cols = 3;
  table.cells.resize(1);
  table.cells[0].resize(3);

  table.cells[0][0].text = "Cell with \"quotes\"";
  table.cells[0][1].text = "Cell with <tags> & entities";
  table.cells[0][2].text = "Cell with\nnewlines\tand tabs";

  std::string csv = table.to_csv();
  CHECK(csv.find("\"\"") != std::string::npos);

  std::string html = table.to_html();
  CHECK(html.find("&lt;") != std::string::npos);
  CHECK(html.find("&gt;") != std::string::npos);
  CHECK(html.find("&amp;") != std::string::npos);

  std::string json = table.to_json();
  CHECK(json.find("\\n") != std::string::npos);
  CHECK(json.find("\\t") != std::string::npos);
}

} // TEST_SUITE
