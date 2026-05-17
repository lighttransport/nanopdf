#include "nanotest.hh"

#ifdef NANOPDF_USE_LIGHTVG

#include "lvg-compat.hh"

#include <cstdint>

namespace {

inline uint8_t channel(uint32_t argb, int shift) {
  return static_cast<uint8_t>((argb >> shift) & 0xff);
}

// Build a two-stop red-to-blue gradient so each sampled t maps to a unique
// recognisable colour.
lvg::Fill::ColorStop red_blue_stops[2] = {
    {0.0f, 255, 0, 0, 255},
    {1.0f, 0, 0, 255, 255},
};

}  // namespace

TEST_SUITE(focal_radial_gradient) {

TEST_CASE("concentric radial: centre is t=0") {
  auto* g = lvg::RadialGradient::gen();
  g->radial(50.0f, 50.0f, 50.0f, 50.0f, 50.0f, 0.0f);
  g->colorStops(red_blue_stops, 2);
  REQUIRE(!g->has_focal());

  uint32_t centre = lvg::sample_gradient_at(g, 50.0f, 50.0f);
  CHECK_EQ(static_cast<int>(channel(centre, 16)), 255);  // R
  CHECK_EQ(static_cast<int>(channel(centre, 0)),   0);   // B

  uint32_t edge = lvg::sample_gradient_at(g, 100.0f, 50.0f);
  CHECK_EQ(static_cast<int>(channel(edge, 16)), 0);
  CHECK_EQ(static_cast<int>(channel(edge, 0)),   255);

  delete g;
}

TEST_CASE("focal radial: t=0 at focal, t=1 at outer rim opposite focal") {
  // Focal at (20, 50), outer centre at (50, 50), outer radius 50. Both radii
  // along x = 50: focal contributes a zero-radius dot, outer rim hits x=100.
  auto* g = lvg::RadialGradient::gen();
  g->radial(50.0f, 50.0f, 50.0f, 20.0f, 50.0f, 0.0f);
  g->colorStops(red_blue_stops, 2);
  REQUIRE(g->has_focal());

  uint32_t at_focal = lvg::sample_gradient_at(g, 20.0f, 50.0f);
  CHECK_EQ(static_cast<int>(channel(at_focal, 16)), 255);  // pure R (t≈0)
  CHECK_EQ(static_cast<int>(channel(at_focal, 0)),   0);

  uint32_t at_outer = lvg::sample_gradient_at(g, 100.0f, 50.0f);
  CHECK_EQ(static_cast<int>(channel(at_outer, 16)), 0);    // pure B (t≈1)
  CHECK_EQ(static_cast<int>(channel(at_outer, 0)),   255);

  delete g;
}

TEST_CASE("focal radial: midpoint between focal and outer rim is t~0.5") {
  // Same setup as above; (60, 50) is roughly halfway between focal (20,50)
  // and outer rim at (100, 50) along the focal→centre axis. t should be
  // around 0.5 → mixed red/blue.
  auto* g = lvg::RadialGradient::gen();
  g->radial(50.0f, 50.0f, 50.0f, 20.0f, 50.0f, 0.0f);
  g->colorStops(red_blue_stops, 2);

  uint32_t mid = lvg::sample_gradient_at(g, 60.0f, 50.0f);
  // Roughly equal red and blue, certainly not pure red or pure blue.
  int r = channel(mid, 16);
  int b = channel(mid, 0);
  CHECK(r > 50);
  CHECK(r < 200);
  CHECK(b > 50);
  CHECK(b < 200);

  delete g;
}

TEST_CASE("focal radial: clamps to t=1 outside the cone") {
  // For a focal at the centre (degenerate concentric), points outside the
  // outer radius pad to the last stop. The same expectation holds for
  // points the focal-radial quadratic can't reach (discriminant < 0).
  auto* g = lvg::RadialGradient::gen();
  g->radial(50.0f, 50.0f, 50.0f, 20.0f, 50.0f, 0.0f);
  g->colorStops(red_blue_stops, 2);

  // Way outside the outer circle.
  uint32_t outside = lvg::sample_gradient_at(g, 500.0f, 500.0f);
  CHECK_EQ(static_cast<int>(channel(outside, 16)), 0);
  CHECK_EQ(static_cast<int>(channel(outside, 0)),   255);

  delete g;
}

TEST_CASE("focal radial with non-zero focal radius") {
  // Focal circle of radius 10 around (50,50); outer circle radius 50 around
  // same centre. This is a concentric two-circle gradient: the "centre" of
  // t=0 is the focal rim, not the focal point.
  auto* g = lvg::RadialGradient::gen();
  g->radial(50.0f, 50.0f, 50.0f, 50.0f, 50.0f, 10.0f);
  g->colorStops(red_blue_stops, 2);
  REQUIRE(g->has_focal());  // fr > eps

  // On the focal rim — should be t≈0, pure red.
  uint32_t on_focal_rim = lvg::sample_gradient_at(g, 60.0f, 50.0f);
  CHECK_EQ(static_cast<int>(channel(on_focal_rim, 16)), 255);
  CHECK_EQ(static_cast<int>(channel(on_focal_rim, 0)),   0);

  // On the outer rim — should be t≈1, pure blue.
  uint32_t on_outer_rim = lvg::sample_gradient_at(g, 100.0f, 50.0f);
  CHECK_EQ(static_cast<int>(channel(on_outer_rim, 16)), 0);
  CHECK_EQ(static_cast<int>(channel(on_outer_rim, 0)),   255);

  delete g;
}

}  // TEST_SUITE

#endif  // NANOPDF_USE_LIGHTVG
