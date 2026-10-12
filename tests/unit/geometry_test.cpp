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
        REQUIRE(near(back.rect, original.rect, tol));
      }
    }
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
  CHECK(spg::to_string(Space::ModelInput) == "model_input");
}
