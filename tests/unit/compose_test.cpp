// CPU reference compositor (SPG-11): fit_mask_rects() never loses coverage,
// and compose() changes exactly the masked pixels (or the whole frame for a
// fallback) and destroys detail there.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "spg/compose.hpp"
#include "spg/error.hpp"
#include "spg/geometry.hpp"
#include "spg/lookahead.hpp"
#include "spg/mask.hpp"

namespace {

using spg::BgraImage;
using spg::FallbackAction;
using spg::MaskPlan;
using spg::MaskRect;
using spg::MaskStyle;

MaskRect rect(float x, float y, float w, float h, MaskStyle style = MaskStyle::Blur) {
  return MaskRect{spg::Box{spg::Rect{x, y, w, h}, spg::Space::Source}, style};
}

// A checkerboard of 1-pixel black and white cells: the most detail a frame
// can hold, and what text strokes look like to a blur. Alpha 200.
BgraImage checkerboard(std::uint32_t w, std::uint32_t h) {
  BgraImage image{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4)};
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::uint8_t v = ((x + y) % 2 == 0) ? 255 : 0;
      std::uint8_t* p = &image.pixels[(static_cast<std::size_t>(y) * w + x) * 4];
      p[0] = v;
      p[1] = v;
      p[2] = static_cast<std::uint8_t>(255 - v);
      p[3] = 200;
    }
  }
  return image;
}

const std::uint8_t* px(const BgraImage& image, std::uint32_t x, std::uint32_t y) {
  return &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
}

bool same_pixel(const BgraImage& a, const BgraImage& b, std::uint32_t x, std::uint32_t y) {
  return std::equal(px(a, x, y), px(a, x, y) + 4, px(b, x, y));
}

// Largest difference between horizontally or vertically adjacent pixels of
// channel 0 inside [x0, x1) x [y0, y1): 255 on the checkerboard.
int max_neighbour_step(const BgraImage& image, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                       std::uint32_t y1) {
  int step = 0;
  for (std::uint32_t y = y0; y < y1; ++y) {
    for (std::uint32_t x = x0; x < x1; ++x) {
      if (x + 1 < x1) {
        step = std::max(step, std::abs(px(image, x, y)[0] - px(image, x + 1, y)[0]));
      }
      if (y + 1 < y1) {
        step = std::max(step, std::abs(px(image, x, y)[0] - px(image, x, y + 1)[0]));
      }
    }
  }
  return step;
}

bool covered_by(const spg::Rect& inner, const std::vector<MaskRect>& outs) {
  return std::any_of(outs.begin(), outs.end(), [&](const MaskRect& o) {
    const spg::Rect& r = o.box.rect;
    return r.x <= inner.x && r.y <= inner.y && r.x + r.width >= inner.x + inner.width &&
           r.y + r.height >= inner.y + inner.height;
  });
}

}  // namespace

TEST_CASE("fit_mask_rects keeps short lists and drops contained rects", "[compose]") {
  const std::vector<MaskRect> in{rect(10, 10, 50, 20), rect(20, 12, 10, 5), rect(100, 100, 30, 30),
                                 rect(10, 10, 50, 20, MaskStyle::Pixelate)};
  const std::vector<MaskRect> out = spg::fit_mask_rects(in, 8);
  REQUIRE(out.size() == 2);
  CHECK(out[0].box.rect == in[0].box.rect);
  // The duplicate's stronger style is kept on the rect that stays.
  CHECK(out[0].style == MaskStyle::Pixelate);
  CHECK(out[1] == in[2]);
}

TEST_CASE("fit_mask_rects merges down to the capacity without losing coverage", "[compose]") {
  std::mt19937 rng(20261012);
  std::uniform_real_distribution<float> pos(0.0F, 1800.0F);
  std::uniform_real_distribution<float> size(1.0F, 200.0F);
  std::uniform_int_distribution<std::size_t> count(1, 300);
  std::uniform_int_distribution<std::size_t> cap(1, 70);
  for (int scene = 0; scene < 200; ++scene) {
    std::vector<MaskRect> in;
    const std::size_t n = count(rng);
    for (std::size_t i = 0; i < n; ++i) {
      in.push_back(rect(pos(rng), pos(rng), size(rng), size(rng), i % 7 == 0 ? MaskStyle::SolidFill : MaskStyle::Blur));
    }
    const std::size_t capacity = cap(rng);
    const std::vector<MaskRect> out = spg::fit_mask_rects(in, capacity);
    REQUIRE(out.size() <= capacity);
    REQUIRE(!out.empty());
    for (const MaskRect& r : in) {
      REQUIRE(covered_by(r.box.rect, out));
      // A rect drawn as solid fill is never drawn weaker after merging.
      if (r.style == MaskStyle::SolidFill) {
        REQUIRE(std::any_of(out.begin(), out.end(), [&](const MaskRect& o) {
          return o.style == MaskStyle::SolidFill && covered_by(r.box.rect, std::vector<MaskRect>{o});
        }));
      }
    }
  }
}

TEST_CASE("fit_mask_rects merges the closest rects first", "[compose]") {
  const std::vector<MaskRect> in{rect(0, 0, 10, 10), rect(12, 0, 10, 10), rect(500, 500, 10, 10)};
  const std::vector<MaskRect> out = spg::fit_mask_rects(in, 2);
  REQUIRE(out.size() == 2);
  CHECK(out[0].box.rect == spg::Rect{0, 0, 22, 10});
  CHECK(out[1] == in[2]);
}

TEST_CASE("fit_mask_rects rejects bad input", "[compose]") {
  const std::vector<MaskRect> ok{rect(0, 0, 1, 1)};
  CHECK_THROWS_AS(spg::fit_mask_rects(ok, 0), spg::ContractError);
  std::vector<MaskRect> readback{rect(0, 0, 1, 1)};
  readback[0].box.space = spg::Space::Readback;
  CHECK_THROWS_AS(spg::fit_mask_rects(readback, 4), spg::ContractError);
  CHECK_THROWS_AS(spg::fit_mask_rects(std::vector<MaskRect>{rect(0, 0, -1, 1)}, 4), spg::ContractError);
}

TEST_CASE("compose blurs inside each rect and nowhere else", "[compose]") {
  const BgraImage before = checkerboard(160, 90);
  BgraImage image = before;
  // Fractional edges round outward: x 20.5..70.25 touches columns 20..70.
  MaskPlan plan;
  plan.rects = {rect(20.5F, 10.0F, 49.75F, 30.0F), rect(150.0F, 80.0F, 40.0F, 40.0F)};
  spg::compose(image, plan, FallbackAction::None);

  for (std::uint32_t y = 0; y < 90; ++y) {
    for (std::uint32_t x = 0; x < 160; ++x) {
      const bool in_a = x >= 20 && x <= 70 && y >= 10 && y < 40;
      const bool in_b = x >= 150 && y >= 80;  // clipped to the frame
      if (!in_a && !in_b) {
        REQUIRE(same_pixel(image, before, x, y));
      }
      // Alpha is never touched.
      REQUIRE(px(image, x, y)[3] == 200);
    }
  }
  // The detail inside is gone: neighbours differ by a few levels at most.
  CHECK(max_neighbour_step(image, 20, 10, 71, 40) <= 8);
  CHECK(max_neighbour_step(image, 150, 80, 160, 90) <= 16);
  // Every pixel inside changed toward the mean.
  CHECK(std::abs(px(image, 40, 20)[0] - 127) <= 8);
}

TEST_CASE("compose pixelates into uniform blocks", "[compose]") {
  BgraImage image = checkerboard(64, 64);
  MaskPlan plan;
  plan.rects = {rect(0, 0, 32, 32, MaskStyle::Pixelate)};
  spg::compose(image, plan, FallbackAction::None);
  // Block of max(8, 0.25 * 32) = 8 px: every 8x8 block is one colour.
  for (std::uint32_t by = 0; by < 32; by += 8) {
    for (std::uint32_t bx = 0; bx < 32; bx += 8) {
      for (std::uint32_t y = by; y < by + 8; ++y) {
        for (std::uint32_t x = bx; x < bx + 8; ++x) {
          REQUIRE(std::equal(px(image, x, y), px(image, x, y) + 3, px(image, bx, by)));
        }
      }
    }
  }
  CHECK(max_neighbour_step(image, 0, 0, 32, 32) == 0);
}

TEST_CASE("compose fills solid rects with the configured colour", "[compose]") {
  BgraImage image = checkerboard(32, 32);
  MaskPlan plan;
  plan.rects = {rect(4, 4, 8, 8, MaskStyle::SolidFill)};
  spg::CompositorConfig config;
  config.fill_bgr = {1, 2, 3};
  spg::compose(image, plan, FallbackAction::None, config);
  for (std::uint32_t y = 4; y < 12; ++y) {
    for (std::uint32_t x = 4; x < 12; ++x) {
      CHECK(px(image, x, y)[0] == 1);
      CHECK(px(image, x, y)[1] == 2);
      CHECK(px(image, x, y)[2] == 3);
      CHECK(px(image, x, y)[3] == 200);
    }
  }
}

TEST_CASE("compose fails closed over the whole frame", "[compose]") {
  const BgraImage before = checkerboard(120, 80);
  SECTION("BlurSource ignores the rects and blurs everything") {
    BgraImage image = before;
    MaskPlan plan;
    plan.fallback = true;
    plan.rects = {rect(0, 0, 4, 4)};
    spg::compose(image, plan, FallbackAction::BlurSource);
    CHECK(max_neighbour_step(image, 0, 0, 120, 80) <= 8);
  }
  SECTION("a failed-closed plan with no action still blurs everything") {
    BgraImage image = before;
    MaskPlan plan;
    plan.fallback = true;
    spg::compose(image, plan, FallbackAction::None);
    CHECK(max_neighbour_step(image, 0, 0, 120, 80) <= 8);
  }
  SECTION("HoldLastScanned drawn here blurs everything") {
    BgraImage image = before;
    MaskPlan plan;
    plan.fallback = true;
    spg::compose(image, plan, FallbackAction::HoldLastScanned);
    CHECK(max_neighbour_step(image, 0, 0, 120, 80) <= 8);
  }
  SECTION("SolidFill fills everything") {
    BgraImage image = before;
    MaskPlan plan;
    plan.fallback = true;
    spg::compose(image, plan, FallbackAction::SolidFill);
    CHECK(max_neighbour_step(image, 0, 0, 120, 80) == 0);
    CHECK(px(image, 60, 40)[0] == 0);
  }
}

TEST_CASE("compose leaves a frame with an empty plan untouched", "[compose]") {
  const BgraImage before = checkerboard(40, 30);
  BgraImage image = before;
  spg::compose(image, MaskPlan{}, FallbackAction::None);
  CHECK(image.pixels == before.pixels);
}

TEST_CASE("compose rejects invalid images, configs and rects", "[compose]") {
  BgraImage empty;
  CHECK_THROWS_AS(spg::compose(empty, MaskPlan{}, FallbackAction::None), spg::ContractError);
  BgraImage short_image{4, 4, std::vector<std::uint8_t>(10)};
  CHECK_THROWS_AS(spg::compose(short_image, MaskPlan{}, FallbackAction::None), spg::ContractError);
  BgraImage image = checkerboard(8, 8);
  spg::CompositorConfig config;
  config.blur_passes = 0;
  CHECK_THROWS_AS(spg::compose(image, MaskPlan{}, FallbackAction::None, config), spg::ContractError);
  config = {};
  config.blur_radius_ratio = std::nanf("");
  CHECK_THROWS_AS(spg::compose(image, MaskPlan{}, FallbackAction::None, config), spg::ContractError);
  MaskPlan plan;
  plan.rects = {rect(0, 0, 2, 2)};
  plan.rects[0].box.space = spg::Space::ModelInput;
  CHECK_THROWS_AS(spg::compose(image, plan, FallbackAction::None), spg::ContractError);
}

TEST_CASE("compose fills rects too thin to blur or pixelate", "[compose]") {
  // A 1x1 blur or pixelate block would return the pixel unchanged; a mask
  // must never leave what it covers as it was.
  spg::CompositorConfig config;
  config.fill_bgr = {1, 2, 3};
  for (const MaskStyle style : {MaskStyle::Blur, MaskStyle::Pixelate}) {
    const BgraImage before = checkerboard(16, 16);
    BgraImage image = before;
    MaskPlan plan;
    plan.rects = {rect(5, 5, 1, 1, style), rect(9, 2, 1, 6, style), rect(2, 12, 7, 1, style)};
    spg::compose(image, plan, FallbackAction::None, config);
    for (std::uint32_t y = 0; y < 16; ++y) {
      for (std::uint32_t x = 0; x < 16; ++x) {
        const bool masked = (x == 5 && y == 5) || (x == 9 && y >= 2 && y < 8) || (y == 12 && x >= 2 && x < 9);
        if (masked) {
          REQUIRE(px(image, x, y)[0] == 1);
          REQUIRE(px(image, x, y)[1] == 2);
          REQUIRE(px(image, x, y)[2] == 3);
        } else {
          REQUIRE(same_pixel(image, before, x, y));
        }
        REQUIRE(px(image, x, y)[3] == 200);
      }
    }
  }
}
