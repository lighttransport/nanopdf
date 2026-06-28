#include "nanotest.hh"

#include <cstdint>
#include <string>
#include <vector>

#include "nanopdf.hh"

#if defined(NANOPDF_USE_THORVG)
#include "thorvg-backend.hh"
#endif

#if defined(NANOPDF_USE_LIGHTVG)
#include "lightvg-backend.hh"
#endif

namespace {

nanopdf::Value make_stream_value(const std::string& content) {
  nanopdf::Value stream_value(nanopdf::Value::STREAM);
  stream_value.stream.data.assign(content.begin(), content.end());
  return stream_value;
}

nanopdf::Value make_number_value(double n) {
  nanopdf::Value v(nanopdf::Value::NUMBER);
  v.number = n;
  return v;
}

nanopdf::Page make_rect_page(size_t rect_count) {
  nanopdf::Page page;
  page.page_number = 1;
  page.media_box = {0.0, 0.0, 64.0, 64.0};

  std::string content;
  content.reserve(rect_count * 32);
  for (size_t i = 0; i < rect_count; ++i) {
    content += "0 0 8 8 re f\n";
  }

  page.contents.push_back(make_stream_value(content));
  return page;
}

nanopdf::Page make_tiny_scale_pattern_page() {
  nanopdf::Page page;
  page.page_number = 1;
  page.media_box = {0.0, 0.0, 64.0, 64.0};

  // Use a colored tiling pattern with a very small matrix scale.
  // This exercises the internal tiled bitmap dimension path where
  // tiles_x * step_ix can truncate to 0 without clamping.
  nanopdf::Value pattern_stream(nanopdf::Value::STREAM);
  pattern_stream.stream.data.assign(
      {'0', ' ', '1', ' ', '0', ' ', 'r', 'g', '\n',
       '0', ' ', '0', ' ', '1', ' ', '1', ' ', 'r', 'e', ' ', 'f', '\n'});

  auto& dict = pattern_stream.stream.dict;
  dict["PatternType"] = make_number_value(1.0);
  dict["PaintType"] = make_number_value(1.0);   // Colored tiles
  dict["TilingType"] = make_number_value(1.0);

  nanopdf::Value bbox(nanopdf::Value::ARRAY);
  bbox.array.push_back(make_number_value(0.0));
  bbox.array.push_back(make_number_value(0.0));
  bbox.array.push_back(make_number_value(1.0));
  bbox.array.push_back(make_number_value(1.0));
  dict["BBox"] = bbox;

  dict["XStep"] = make_number_value(1.0);
  dict["YStep"] = make_number_value(1.0);

  nanopdf::Value matrix(nanopdf::Value::ARRAY);
  matrix.array.push_back(make_number_value(0.001));
  matrix.array.push_back(make_number_value(0.0));
  matrix.array.push_back(make_number_value(0.0));
  matrix.array.push_back(make_number_value(0.001));
  matrix.array.push_back(make_number_value(0.0));
  matrix.array.push_back(make_number_value(0.0));
  dict["Matrix"] = matrix;

  nanopdf::Value resources(nanopdf::Value::DICTIONARY);
  dict["Resources"] = resources;

  nanopdf::Value pattern_dict(nanopdf::Value::DICTIONARY);
  pattern_dict.dict["Ptiny"] = pattern_stream;
  page.resources["Pattern"] = pattern_dict;

  // Fill a medium rectangle so the pattern path is exercised.
  page.contents.push_back(make_stream_value(
      "/Pattern cs\n"
      "/Ptiny scn\n"
      "0 0 10 10 re\n"
      "f\n"));

  return page;
}

}  // namespace

TEST_CASE_IN_SUITE("backends", "ThorVG progress callback reports 1 percent steps") {
#if !defined(NANOPDF_USE_THORVG)
  SKIP_IF(true, "ThorVG backend is not enabled");
#else
  nanopdf::Pdf pdf;
  auto page = make_rect_page(100);

  nanopdf::ThorVGBackend backend;
  REQUIRE(backend.initialize(64, 64));

  std::vector<uint32_t> reported_percents;
  std::vector<size_t> processed_objects;
  backend.set_progress_callback(
      [&](const nanopdf::RenderProgressInfo& progress) -> bool {
        reported_percents.push_back(progress.percent);
        processed_objects.push_back(progress.processed_objects);
        return true;
      });

  auto result = backend.render_page(pdf, page);
  REQUIRE(result.success);

  REQUIRE_EQ(reported_percents.size(), size_t(101));
  CHECK_EQ(reported_percents.front(), uint32_t(0));
  CHECK_EQ(reported_percents.back(), uint32_t(100));
  CHECK_EQ(processed_objects.front(), size_t(0));
  CHECK_EQ(processed_objects.back(), size_t(100));

  for (size_t i = 0; i < reported_percents.size(); ++i) {
    CHECK_EQ(reported_percents[i], static_cast<uint32_t>(i));
  }

  backend.clear_progress_callback();
#endif
}

TEST_CASE_IN_SUITE("backends", "ThorVG progress callback respects threshold") {
#if !defined(NANOPDF_USE_THORVG)
  SKIP_IF(true, "ThorVG backend is not enabled");
#else
  nanopdf::Pdf pdf;
  auto page = make_rect_page(99);

  nanopdf::ThorVGBackend backend;
  REQUIRE(backend.initialize(64, 64));

  size_t callback_count = 0;
  backend.set_progress_callback(
      [&](const nanopdf::RenderProgressInfo&) { callback_count++; return true; });

  auto result = backend.render_page(pdf, page);
  REQUIRE(result.success);
  CHECK_EQ(callback_count, size_t(0));

  backend.clear_progress_callback();
#endif
}

TEST_CASE_IN_SUITE("backends",
                   "ThorVG progress callback skips empty pages even with zero threshold") {
#if !defined(NANOPDF_USE_THORVG)
  SKIP_IF(true, "ThorVG backend is not enabled");
#else
  nanopdf::Pdf pdf;
  auto page = make_rect_page(0);

  nanopdf::ThorVGBackend backend;
  REQUIRE(backend.initialize(64, 64));

  size_t callback_count = 0;
  backend.set_progress_callback(
      [&](const nanopdf::RenderProgressInfo&) { callback_count++; return true; }, 0, 1);

  auto result = backend.render_page(pdf, page);
  REQUIRE(result.success);
  CHECK_EQ(callback_count, size_t(0));

  backend.clear_progress_callback();
#endif
}

TEST_CASE_IN_SUITE("backends",
                   "ThorVG per-render progress options override backend callback") {
#if !defined(NANOPDF_USE_THORVG)
  SKIP_IF(true, "ThorVG backend is not enabled");
#else
  nanopdf::Pdf pdf;
  auto page = make_rect_page(25);

  nanopdf::ThorVGBackend backend;
  REQUIRE(backend.initialize(64, 64));

  size_t backend_callback_count = 0;
  backend.set_progress_callback(
      [&](const nanopdf::RenderProgressInfo&) { backend_callback_count++; return true; },
      1, 1);

  nanopdf::ThorVGRenderOptions options;
  std::vector<uint32_t> reported_percents;
  options.progress_callback = [&](const nanopdf::RenderProgressInfo& progress) -> bool {
    reported_percents.push_back(progress.percent);
    return true;
  };
  options.progress_object_threshold = 1;
  options.progress_percent_step = 10;

  auto result = backend.render_page(pdf, page, options);
  REQUIRE(result.success);

  CHECK_EQ(backend_callback_count, size_t(0));
  REQUIRE_EQ(reported_percents.size(), size_t(11));
  for (size_t i = 0; i < reported_percents.size(); ++i) {
    CHECK_EQ(reported_percents[i], static_cast<uint32_t>(i * 10));
  }

  backend.clear_progress_callback();
#endif
}

TEST_CASE_IN_SUITE("backends",
                   "ThorVG tiny-scale tiling pattern renders without fallback placeholder") {
#if !defined(NANOPDF_USE_THORVG)
  SKIP_IF(true, "ThorVG backend is not enabled");
#else
  nanopdf::Pdf pdf;
  auto page = make_tiny_scale_pattern_page();

  nanopdf::ThorVGBackend backend;
  REQUIRE(backend.initialize(64, 64));

  auto result = backend.render_page(pdf, page);
  REQUIRE(result.success);
  REQUIRE(!result.pixels.empty());

  // Fallback placeholder for colored patterns uses purple-ish (200,150,220).
  // Assert we do not paint placeholder purple after tiny-scale composition.
  bool found_placeholder_purple = false;
  const size_t pixel_count = result.pixels.size() / 4;
  for (size_t i = 0; i < pixel_count; ++i) {
    uint8_t r = result.pixels[i * 4 + 0];
    uint8_t g = result.pixels[i * 4 + 1];
    uint8_t b = result.pixels[i * 4 + 2];
    if (r >= 190 && r <= 210 &&
        g >= 140 && g <= 160 &&
        b >= 210 && b <= 230) {
      found_placeholder_purple = true;
      break;
    }
  }
  CHECK(!found_placeholder_purple);
#endif
}

TEST_CASE_IN_SUITE("backends", "LightVG progress callback can interrupt a render") {
#if !defined(NANOPDF_USE_LIGHTVG)
  SKIP_IF(true, "LightVG backend is not enabled");
#else
  nanopdf::Pdf pdf;
  auto page = make_rect_page(200);  // 200 fill objects -> plenty of progress

  nanopdf::LightVGBackend backend;
  REQUIRE(backend.initialize(64, 64));

  uint32_t last_percent = 0;
  // Cancel once we pass the halfway mark.
  backend.set_progress_callback(
      [&](const nanopdf::RenderProgressInfo& p) -> bool {
        last_percent = p.percent;
        return p.percent < 50;
      },
      /*object_threshold=*/1, /*percent_step=*/1);

  auto result = backend.render_page(pdf, page);
  CHECK(result.interrupted);
  CHECK(!result.success);
  CHECK(last_percent >= 50);    // we ran at least to the cancel point
  CHECK(last_percent < 100);    // but did NOT finish the page

  backend.clear_progress_callback();
#endif
}

TEST_CASE_IN_SUITE("backends", "LightVG render completes when callback never cancels") {
#if !defined(NANOPDF_USE_LIGHTVG)
  SKIP_IF(true, "LightVG backend is not enabled");
#else
  nanopdf::Pdf pdf;
  auto page = make_rect_page(200);

  nanopdf::LightVGBackend backend;
  REQUIRE(backend.initialize(64, 64));

  uint32_t last_percent = 0;
  backend.set_progress_callback(
      [&](const nanopdf::RenderProgressInfo& p) -> bool {
        last_percent = p.percent;
        return true;
      },
      /*object_threshold=*/1, /*percent_step=*/1);

  auto result = backend.render_page(pdf, page);
  CHECK(result.success);
  CHECK(!result.interrupted);
  CHECK_EQ(last_percent, uint32_t(100));

  backend.clear_progress_callback();
#endif
}
