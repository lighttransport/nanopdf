#include "nanotest.hh"

#include <cstdint>
#include <string>
#include <vector>

#include "nanopdf.hh"

#if defined(NANOPDF_USE_THORVG)
#include "thorvg-backend.hh"
#endif

namespace {

nanopdf::Value make_stream_value(const std::string& content) {
  nanopdf::Value stream_value(nanopdf::Value::STREAM);
  stream_value.stream.data.assign(content.begin(), content.end());
  return stream_value;
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
      [&](const nanopdf::RenderProgressInfo& progress) {
        reported_percents.push_back(progress.percent);
        processed_objects.push_back(progress.processed_objects);
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
      [&](const nanopdf::RenderProgressInfo&) { callback_count++; });

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
      [&](const nanopdf::RenderProgressInfo&) { backend_callback_count++; },
      1, 1);

  nanopdf::ThorVGRenderOptions options;
  std::vector<uint32_t> reported_percents;
  options.progress_callback = [&](const nanopdf::RenderProgressInfo& progress) {
    reported_percents.push_back(progress.percent);
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
