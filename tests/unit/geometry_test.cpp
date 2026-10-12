#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "spg/error.hpp"
#include "spg/geometry.hpp"

namespace {

using spg::Box;
using spg::Fit;
using spg::ModelInputMapping;
using spg::ReadbackMapping;
using spg::Rect;
using spg::Size;
using spg::Space;
using spg::SpaceMap;

constexpr std::array<Space, 3> kSpaces = {Space::Source, Space::Readback, Space::ModelInput};
constexpr std::array<Fit, 3> kFits = {Fit::Stretch, Fit::LetterboxCenter, Fit::LetterboxTopLeft};

bool near(float a, float b, double tol) {
  return std::abs(static_cast<double>(a) - static_cast<double>(b)) <= tol;
}

bool near(const Rect& a, const Rect& b, double tol) {
  return near(a.x, b.x, tol) && near(a.y, b.y, tol) && near(a.width, b.width, tol) &&
         near(a.height, b.height, tol);
}

// Largest dimension of a space under `map`, used to scale float tolerances.
double extent(Space space, const SpaceMap& map) {
  switch (space) {
    case Space::Source:
      return std::max(map.readback.source().width, map.readback.source().height);
    case Space::Readback:
      return std::max(map.readback.readback().width, map.readback.readback().height);
    case Space::ModelInput:
      return std::max(map.model_input->input().width, map.model_input->input().height);
  }
  return 0.0;
}

// Seeded generator of realistic mappings and boxes. Deterministic so a failure
// reproduces exactly; the case index is reported on failure.
class Cases {
 public:
  explicit Cases(std::uint32_t seed) : rng_(seed) {}

  std::uint32_t uniform(std::uint32_t lo, std::uint32_t hi) {
    return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng_);
  }
  float uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng_); }

  SpaceMap mapping() {
    const Size source{uniform(64U, 7680U), uniform(64U, 4320U)};
    const Size readback{uniform(32U, std::min(source.width, 1920U)),
                        uniform(32U, std::min(source.height, 1080U))};
    // A tile of at least an eighth of the readback frame, or the whole frame.
    Rect crop{0.0F, 0.0F, static_cast<float>(readback.width), static_cast<float>(readback.height)};
    if (uniform(0U, 1U) == 1U) {
      crop.width = uniform(crop.width / 8.0F, crop.width);
      crop.height = uniform(crop.height / 8.0F, crop.height);
      crop.x = uniform(0.0F, static_cast<float>(readback.width) - crop.width);
      crop.y = uniform(0.0F, static_cast<float>(readback.height) - crop.height);
    }
    const Size input{uniform(32U, 1280U), uniform(32U, 1280U)};
    const Fit fit = kFits.at(uniform(0U, 2U));
    return {ReadbackMapping(source, readback), ModelInputMapping(crop, input, fit)};
  }

  // A box mostly inside the space, sometimes partly outside it.
  Box box(Space space, const SpaceMap& map) {
    const auto e = static_cast<float>(extent(space, map));
    const float w = uniform(0.0F, e);
    const float h = uniform(0.0F, e);
    return {{uniform(-0.1F * e, e), uniform(-0.1F * e, e), w, h}, space};
  }

 private:
  std::mt19937 rng_;
};

constexpr int kPropertyCases = 5000;

// Units of `space` per Source pixel along one axis.
double units_per_source(Space space, const SpaceMap& map, bool x_axis) {
  const double rb = x_axis ? static_cast<double>(map.readback.readback().width) /
                                 static_cast<double>(map.readback.source().width)
                           : static_cast<double>(map.readback.readback().height) /
                                 static_cast<double>(map.readback.source().height);
  switch (space) {
    case Space::Source:
      return 1.0;
    case Space::Readback:
      return rb;
    case Space::ModelInput:
      return rb * (x_axis ? map.model_input->scale_x() : map.model_input->scale_y());
  }
  return 1.0;
}

// One float ulp at the larger magnitude of the span [origin, origin + length].
double span_ulp(float origin, float length) {
  const float m = std::max(std::abs(origin), std::abs(origin + length));
  return static_cast<double>(std::nextafter(m, std::numeric_limits<float>::infinity()) - m);
}

// True when [b, b + bl] contains [a, a + al] up to `slack` and extends past
// it by at most `grow` on each side.
bool edges_within(float a, float al, float b, float bl, double slack, double grow) {
  const double a0 = a;
  const double a1 = a0 + static_cast<double>(al);
  const double b0 = b;
  const double b1 = b0 + static_cast<double>(bl);
  return b0 <= a0 + slack && b1 >= a1 - slack && a0 - b0 <= grow && b1 - a1 <= grow;
}

}  // namespace

TEST_CASE("geometry: round trips between every pair of spaces restore the box",
          "[geometry][property]") {
  Cases cases(0x5be0f02U);
  for (int i = 0; i < kPropertyCases; ++i) {
    const SpaceMap map = cases.mapping();
    for (const Space from : kSpaces) {
      const Box original = cases.box(from, map);
      for (const Space to : kSpaces) {
        const Box there = spg::convert(original, to, map);
        const Box back = spg::convert(there, from, map);
        const double tol = 1e-4 + 1e-5 * extent(from, map);
        INFO("case " << i << " from " << spg::to_string(from) << " via " << spg::to_string(to));
        REQUIRE(there.space == to);
        REQUIRE(back.space == from);
        // convert() rounds outward, so a round trip never shrinks the box (up
        // to double rounding) and grows each edge by at most `tol` plus the
        // two float ulps of `there` that narrowing may add, seen from `from`.
        const double sx = units_per_source(to, map, true) / units_per_source(from, map, true);
        const double sy = units_per_source(to, map, false) / units_per_source(from, map, false);
        const double slack_x = 1e-9 * (1.0 + 1.0 / sx);
        const double slack_y = 1e-9 * (1.0 + 1.0 / sy);
        const double grow_x = tol + 2.0 * span_ulp(there.rect.x, there.rect.width) / sx;
        const double grow_y = tol + 2.0 * span_ulp(there.rect.y, there.rect.height) / sy;
        REQUIRE(edges_within(original.rect.x, original.rect.width, back.rect.x, back.rect.width,
                             slack_x, grow_x));
        REQUIRE(edges_within(original.rect.y, original.rect.height, back.rect.y, back.rect.height,
                             slack_y, grow_y));
      }
    }
  }
}

TEST_CASE("geometry: a chain through every space never shrinks a Source box",
          "[geometry][property]") {
  Cases cases(0xc4a1eU);
  for (int i = 0; i < kPropertyCases; ++i) {
    const SpaceMap map = cases.mapping();
    const Box original = cases.box(Space::Source, map);
    const Box readback = spg::convert(original, Space::Readback, map);
    const Box model = spg::convert(readback, Space::ModelInput, map);
    const Box back = spg::convert(model, Space::Source, map);
    REQUIRE(back.space == Space::Source);
    // Each step rounds outward, so the chain contains the original up to
    // double rounding, magnified by the coarsest space it passed through.
    const double sx = std::min(units_per_source(Space::Readback, map, true),
                               units_per_source(Space::ModelInput, map, true));
    const double sy = std::min(units_per_source(Space::Readback, map, false),
                               units_per_source(Space::ModelInput, map, false));
    const double slack_x = 1e-9 * (1.0 + 1.0 / sx);
    const double slack_y = 1e-9 * (1.0 + 1.0 / sy);
    const double unbounded = std::numeric_limits<double>::infinity();
    INFO("case " << i << " fit " << static_cast<int>(map.model_input->fit()));
    REQUIRE(edges_within(original.rect.x, original.rect.width, back.rect.x, back.rect.width, slack_x,
                         unbounded));
    REQUIRE(edges_within(original.rect.y, original.rect.height, back.rect.y, back.rect.height,
                         slack_y, unbounded));
  }
}

TEST_CASE("geometry: direct conversion equals conversion through Readback",
          "[geometry][property]") {
  Cases cases(0x0c0ffeeU);
  for (int i = 0; i < kPropertyCases; ++i) {
    const SpaceMap map = cases.mapping();
    const Box source = cases.box(Space::Source, map);
    const Box direct = spg::convert(source, Space::ModelInput, map);
    const Box chained = spg::convert(spg::convert(source, Space::Readback, map), Space::ModelInput, map);
    INFO("case " << i);
    REQUIRE(near(direct.rect, chained.rect, 1e-4 + 1e-5 * 10.0 * extent(Space::ModelInput, map)));
  }
}

TEST_CASE("geometry: conversions preserve containment", "[geometry][property]") {
  Cases cases(0xfeedU);
  for (int i = 0; i < kPropertyCases; ++i) {
    const SpaceMap map = cases.mapping();
    const Box outer = cases.box(Space::ModelInput, map);
    const Box inner{{outer.rect.x + outer.rect.width / 4.0F, outer.rect.y + outer.rect.height / 4.0F,
                     outer.rect.width / 2.0F, outer.rect.height / 2.0F},
                    Space::ModelInput};
    const Rect o = spg::convert(outer, Space::Source, map).rect;
    const Rect n = spg::convert(inner, Space::Source, map).rect;
    const double tol = 1e-4 + 1e-5 * extent(Space::Source, map);
    INFO("case " << i);
    REQUIRE(n.x + tol >= o.x);
    REQUIRE(n.y + tol >= o.y);
    REQUIRE(n.x + n.width <= o.x + o.width + tol);
    REQUIRE(n.y + n.height <= o.y + o.height + tol);
  }
}

// The exact image of [lo, hi] on one axis, computed in double straight from
// the mapping accessors rather than through convert().
struct Edges {
  double lo;
  double hi;
};

Edges exact_x(const Rect& r, Space from, Space to, const SpaceMap& map) {
  const double rb = static_cast<double>(map.readback.readback().width) /
                    static_cast<double>(map.readback.source().width);
  const ModelInputMapping& m = *map.model_input;
  const auto to_source = [&](double v) {
    switch (from) {
      case Space::Source:
        return v;
      case Space::Readback:
        return v / rb;
      case Space::ModelInput:
        return ((v - m.pad_x()) / m.scale_x() + static_cast<double>(m.crop().x)) / rb;
    }
    return v;
  };
  const auto from_source = [&](double v) {
    switch (to) {
      case Space::Source:
        return v;
      case Space::Readback:
        return v * rb;
      case Space::ModelInput:
        return (v * rb - static_cast<double>(m.crop().x)) * m.scale_x() + m.pad_x();
    }
    return v;
  };
  const double x = static_cast<double>(r.x);
  return {from_source(to_source(x)), from_source(to_source(x + static_cast<double>(r.width)))};
}

TEST_CASE("geometry: narrowing to float never moves a converted edge inward",
          "[geometry][property]") {
  Cases cases(0xed6e5U);
  for (int i = 0; i < kPropertyCases; ++i) {
    const SpaceMap map = cases.mapping();
    for (const Space from : kSpaces) {
      const Box original = cases.box(from, map);
      for (const Space to : kSpaces) {
        if (from == to) {
          continue;
        }
        const Rect r = spg::convert(original, to, map).rect;
        const Edges want = exact_x(original.rect, from, to, map);
        // The slack absorbs double rounding only. It is far below one float
        // ulp at pixel magnitudes, so an edge rounded inward still fails.
        const double slack = 1e-9;
        INFO("case " << i << " from " << spg::to_string(from) << " to " << spg::to_string(to));
        REQUIRE(static_cast<double>(r.x) <= want.lo + slack);
        REQUIRE(static_cast<double>(r.x) + static_cast<double>(r.width) >= want.hi - slack);
      }
    }
  }
}

TEST_CASE("geometry: a converted mask covers the edge pixel of its exact image", "[geometry]") {
  // Readback right edge 750.0000152587890625 maps to Source 1000.0000203450...
  // Rounding x and width to nearest float separately gives a right edge of
  // exactly 1000 and drops column 1000.
  const SpaceMap map{ReadbackMapping({1920U, 1080U}, {1440U, 810U}), std::nullopt};
  const Box readback{{512.0001220703125F, 0.0F, 237.9998931884765625F, 10.0F}, Space::Readback};
  const Box source = spg::convert(readback, Space::Source, map);
  CHECK(static_cast<double>(source.rect.x) + static_cast<double>(source.rect.width) >
        1000.0000203);
  const spg::PixelRect p = spg::enclosing_pixels(source.rect, map.readback.source());
  CHECK(p.x <= 682);
  CHECK(p.x + p.width >= 1001);
}

TEST_CASE("geometry: enclosing_pixels covers every pixel the rect touches", "[geometry][property]") {
  Cases cases(0xb10bU);
  for (int i = 0; i < kPropertyCases; ++i) {
    const Size bounds{cases.uniform(1U, 4000U), cases.uniform(1U, 4000U)};
    const auto bw = static_cast<float>(bounds.width);
    const auto bh = static_cast<float>(bounds.height);
    const Rect r{cases.uniform(-0.2F * bw, bw), cases.uniform(-0.2F * bh, bh),
                 cases.uniform(0.0F, bw), cases.uniform(0.0F, bh)};
    const spg::PixelRect p = spg::enclosing_pixels(r, bounds);
    const Rect clipped = spg::intersection(r, {0.0F, 0.0F, bw, bh});
    INFO("case " << i);
    REQUIRE(p.x >= 0);
    REQUIRE(p.y >= 0);
    REQUIRE(p.x + p.width <= static_cast<std::int32_t>(bounds.width));
    REQUIRE(p.y + p.height <= static_cast<std::int32_t>(bounds.height));
    if (spg::area(clipped) > 0.0F) {
      REQUIRE(static_cast<float>(p.x) <= clipped.x);
      REQUIRE(static_cast<float>(p.y) <= clipped.y);
      REQUIRE(static_cast<float>(p.x + p.width) >= clipped.x + clipped.width);
      REQUIRE(static_cast<float>(p.y + p.height) >= clipped.y + clipped.height);
    }
  }
}

TEST_CASE("geometry: known conversions", "[geometry]") {
  const ReadbackMapping half({1920U, 1080U}, {960U, 540U});
  const Rect whole_readback{0.0F, 0.0F, 960.0F, 540.0F};

  SECTION("Source to Readback scales by the readback ratio") {
    const SpaceMap map{half, std::nullopt};
    const Box b = spg::convert({{100.0F, 50.0F, 200.0F, 100.0F}, Space::Source}, Space::Readback, map);
    CHECK(b == Box{{50.0F, 25.0F, 100.0F, 50.0F}, Space::Readback});
  }
  SECTION("centered letterbox pads the short axis evenly") {
    const SpaceMap map{half, ModelInputMapping(whole_readback, {640U, 640U}, Fit::LetterboxCenter)};
    const Box b = spg::convert({{0.0F, 0.0F, 1920.0F, 1080.0F}, Space::Source}, Space::ModelInput, map);
    CHECK(near(b.rect, {0.0F, 140.0F, 640.0F, 360.0F}, 1e-3));
    CHECK(std::abs(map.model_input->pad_x()) < 1e-9);
    CHECK(std::abs(map.model_input->pad_y() - 140.0) < 1e-9);
  }
  SECTION("top-left letterbox does not offset the content") {
    const SpaceMap map{half, ModelInputMapping(whole_readback, {640U, 640U}, Fit::LetterboxTopLeft)};
    const Box b = spg::convert({{0.0F, 0.0F, 1920.0F, 1080.0F}, Space::Source}, Space::ModelInput, map);
    CHECK(near(b.rect, {0.0F, 0.0F, 640.0F, 360.0F}, 1e-3));
  }
  SECTION("stretch scales each axis independently") {
    const SpaceMap map{half, ModelInputMapping(whole_readback, {320U, 320U}, Fit::Stretch)};
    const Box b = spg::convert({{960.0F, 540.0F, 960.0F, 540.0F}, Space::Source}, Space::ModelInput, map);
    CHECK(near(b.rect, {160.0F, 160.0F, 160.0F, 160.0F}, 1e-3));
  }
  SECTION("a tile maps its own origin to the model origin") {
    const SpaceMap map{half,
                       ModelInputMapping({480.0F, 270.0F, 480.0F, 270.0F}, {480U, 270U}, Fit::Stretch)};
    const Box b = spg::convert({{960.0F, 540.0F, 20.0F, 10.0F}, Space::Source}, Space::ModelInput, map);
    CHECK(near(b.rect, {0.0F, 0.0F, 10.0F, 5.0F}, 1e-4));
  }
  SECTION("letterbox padding maps outside the source and is not clipped") {
    const SpaceMap map{half, ModelInputMapping(whole_readback, {640U, 640U}, Fit::LetterboxCenter)};
    const Box b = spg::convert({{0.0F, 0.0F, 640.0F, 140.0F}, Space::ModelInput}, Space::Source, map);
    CHECK(b.rect.y < 0.0F);
  }
  SECTION("same-space conversion is the identity") {
    const SpaceMap map{half, std::nullopt};
    const Box b{{1.5F, 2.5F, 3.5F, 4.5F}, Space::Readback};
    CHECK(spg::convert(b, Space::Readback, map) == b);
  }
}

TEST_CASE("geometry: invalid mappings and boxes are rejected", "[geometry]") {
  CHECK_THROWS_AS(ReadbackMapping({0U, 1080U}, {960U, 540U}), spg::ContractError);
  CHECK_THROWS_AS(ReadbackMapping({1920U, 1080U}, {960U, 0U}), spg::ContractError);
  CHECK_THROWS_AS(ModelInputMapping({0.0F, 0.0F, 0.0F, 10.0F}, {64U, 64U}, Fit::Stretch),
                  spg::ContractError);
  CHECK_THROWS_AS(ModelInputMapping({0.0F, 0.0F, 10.0F, 10.0F}, {64U, 0U}, Fit::Stretch),
                  spg::ContractError);

  const SpaceMap no_model{ReadbackMapping({1920U, 1080U}, {960U, 540U}), std::nullopt};
  const Box ok{{0.0F, 0.0F, 10.0F, 10.0F}, Space::Source};
  CHECK_THROWS_AS(spg::convert(ok, Space::ModelInput, no_model), spg::ContractError);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  CHECK_THROWS_AS(spg::convert({{nan, 0.0F, 1.0F, 1.0F}, Space::Source}, Space::Readback, no_model),
                  spg::ContractError);
  CHECK_THROWS_AS(spg::convert({{0.0F, 0.0F, -1.0F, 1.0F}, Space::Source}, Space::Readback, no_model),
                  spg::ContractError);
}

TEST_CASE("geometry: rect helpers", "[geometry]") {
  const Rect a{0.0F, 0.0F, 10.0F, 10.0F};
  CHECK(spg::area(a) == 100.0F);
  CHECK(spg::iou(a, a) == 1.0F);
  CHECK(spg::iou(a, {20.0F, 20.0F, 5.0F, 5.0F}) == 0.0F);
  CHECK(spg::iou({}, {}) == 0.0F);
  CHECK(spg::intersection(a, {5.0F, 5.0F, 10.0F, 10.0F}) == Rect{5.0F, 5.0F, 5.0F, 5.0F});
  CHECK(spg::area(spg::intersection(a, {10.0F, 0.0F, 5.0F, 5.0F})) == 0.0F);
  CHECK(std::abs(spg::iou(a, {5.0F, 0.0F, 10.0F, 10.0F}) - 1.0F / 3.0F) < 1e-6F);
  CHECK(spg::enclosing_pixels({10.2F, 5.7F, 3.1F, 0.2F}, {100U, 100U}) == spg::PixelRect{10, 5, 4, 1});
  CHECK(spg::enclosing_pixels({-5.0F, -5.0F, 10.0F, 10.0F}, {100U, 100U}) == spg::PixelRect{0, 0, 5, 5});
  CHECK(spg::enclosing_pixels({200.0F, 0.0F, 10.0F, 10.0F}, {100U, 100U}).width == 0);
  // Bounds above INT32_MAX clamp instead of overflowing the int32 result.
  constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
  CHECK(spg::enclosing_pixels({0.0F, 0.0F, 3e9F, 1.0F}, {4'000'000'000U, 1U}) ==
        spg::PixelRect{0, 0, kMax, 1});
  CHECK(spg::to_string(Space::ModelInput) == "model_input");
}
