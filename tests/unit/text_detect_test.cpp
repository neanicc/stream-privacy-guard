// Text detector stages that need no model: DBNet post-processing on synthetic
// probability maps, the tile layout, the tile mapping back to Source space,
// cross-tile merging and the input tensor layout.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "detect/db_postprocess.hpp"
#include "detect/source_clip.hpp"
#include "detect/text_detector.hpp"
#include "spg/error.hpp"

namespace {

using Catch::Approx;
using spg::Box;
using spg::Detection;
using spg::Rect;
using spg::Size;
using spg::Space;
using spg::detect::TileDetection;
namespace db = spg::detect::db;

// A probability map of `size`, zero except where painted.
struct Map {
  explicit Map(Size s) : size(s), prob(static_cast<std::size_t>(s.width) * s.height, 0.0F) {}

  void fill(std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h, float value) {
    for (std::uint32_t y = y0; y < y0 + h; ++y) {
      for (std::uint32_t x = x0; x < x0 + w; ++x) {
        prob[static_cast<std::size_t>(y) * size.width + x] = value;
      }
    }
  }

  Size size;
  std::vector<float> prob;
};

bool contains(const Rect& outer, const Rect& inner) {
  return outer.x <= inner.x && outer.y <= inner.y &&
         static_cast<double>(outer.x) + outer.width >= static_cast<double>(inner.x) + inner.width &&
         static_cast<double>(outer.y) + outer.height >= static_cast<double>(inner.y) + inner.height;
}

Detection text_box(Rect rect, float score = 0.9F) {
  return {{rect, Space::Source}, spg::DetectionKind::TextRegion, score, spg::DetectorId::PpOcrDet};
}

// A box from `tile` whose kernel spans the box (merging compares kernels).
TileDetection on_tile(Rect rect, std::size_t tile, float score = 0.9F) { return {text_box(rect, score), tile, rect}; }

}  // namespace

// --- DBNet post-processing ---------------------------------------------------

TEST_CASE("db::decode finds a filled block and grows it by the unclip distance", "[text][db]") {
  Map map({128, 64});
  map.fill(20, 10, 60, 12, 0.9F);  // a 60x12 "line"
  // Unclip distance 60 * 12 * ratio / (2 * 72): 8 for RapidOCR's 1.6, 10 for
  // SPG's default 2.0.
  const bool reference = GENERATE(true, false);
  db::Params params;
  if (reference) {
    params.unclip_ratio = 1.6F;
  }
  CHECK(db::Params{}.unclip_ratio == 2.0F);
  const float distance = reference ? 8.0F : 10.0F;
  const auto found = db::decode(map.prob, map.size, params);
  REQUIRE(found.size() == 1);
  const Box& box = found[0].box;
  CHECK(box.space == Space::ModelInput);
  // Dilation adds a pixel on the far edges (pixels 20..80, 10..22). The fitted
  // rectangle runs through the contour corner points: 60x12 centered at
  // (50, 16). Its score is the mean over pixels 20..80 x 10..22, which counts
  // the dilated zeros, as RapidOCR does: 0.9 * 720 / 793.
  CHECK(found[0].score == Approx(0.9 * 720.0 / 793.0).margin(1e-4));
  // Grown by `distance` on every side, plus one pixel on the far edges.
  CHECK(box.rect.x == Approx(20.0F - distance).margin(1e-3));
  CHECK(box.rect.y == Approx(10.0F - distance).margin(1e-3));
  CHECK(box.rect.width == Approx(61.0F + 2.0F * distance).margin(1e-3));
  CHECK(box.rect.height == Approx(13.0F + 2.0F * distance).margin(1e-3));
  CHECK(contains(box.rect, {20.0F, 10.0F, 60.0F, 12.0F}));
  // The kernel is the fitted rectangle before growth, far edges plus one.
  const Box& line = found[0].line;
  CHECK(line.space == Space::ModelInput);
  CHECK(line.rect.x == Approx(20.0F).margin(1e-3));
  CHECK(line.rect.y == Approx(10.0F).margin(1e-3));
  CHECK(line.rect.width == Approx(61.0F).margin(1e-3));
  CHECK(line.rect.height == Approx(13.0F).margin(1e-3));
}

TEST_CASE("db::decode keeps separate lines separate", "[text][db]") {
  Map map({160, 96});
  map.fill(10, 10, 100, 10, 0.95F);
  map.fill(10, 50, 70, 10, 0.95F);
  map.fill(130, 70, 20, 20, 0.8F);
  auto found = db::decode(map.prob, map.size);
  REQUIRE(found.size() == 3);
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.box.rect.y < b.box.rect.y; });
  CHECK(contains(found[0].box.rect, {10, 10, 100, 10}));
  CHECK(contains(found[1].box.rect, {10, 50, 70, 10}));
  CHECK(contains(found[2].box.rect, {130, 70, 20, 20}));
  CHECK(found[0].box.rect.y + found[0].box.rect.height < 50.0F);
}

TEST_CASE("db::decode applies the binarization and box thresholds", "[text][db]") {
  Map map({96, 64});
  SECTION("below the binarization threshold nothing is text") {
    map.fill(10, 10, 60, 12, 0.3F);  // not greater than 0.3
    CHECK(db::decode(map.prob, map.size).empty());
  }
  SECTION("a weak region binarizes but scores under the box threshold") {
    map.fill(10, 10, 60, 12, 0.45F);
    CHECK(db::decode(map.prob, map.size).empty());
    db::Params params;
    params.box_threshold = 0.35F;
    CHECK(db::decode(map.prob, map.size, params).size() == 1);
  }
  SECTION("a box scoring exactly the threshold is kept") {
    map.fill(10, 10, 60, 12, 0.5F);
    db::Params params;
    params.dilate = false;  // dilation would mix zeros into the mean
    CHECK(db::decode(map.prob, map.size, params).size() == 1);
  }
  SECTION("specks under min_size are dropped") {
    map.fill(10, 10, 1, 1, 1.0F);
    map.fill(30, 30, 40, 1, 1.0F);  // 2 px tall after dilation
    CHECK(db::decode(map.prob, map.size).empty());
  }
}

TEST_CASE("db::decode covers a rotated line with its axis-aligned box", "[text][db]") {
  Map map({200, 200});
  // A 120x10 band at 30 degrees through (100, 100).
  const double angle = 30.0 * 3.14159265358979 / 180.0;
  float min_x = 1e9F, min_y = 1e9F, max_x = -1e9F, max_y = -1e9F;
  for (std::uint32_t y = 0; y < 200; ++y) {
    for (std::uint32_t x = 0; x < 200; ++x) {
      const double dx = x + 0.5 - 100.0;
      const double dy = y + 0.5 - 100.0;
      const double along = dx * std::cos(angle) + dy * std::sin(angle);
      const double across = -dx * std::sin(angle) + dy * std::cos(angle);
      if (std::abs(along) <= 60.0 && std::abs(across) <= 5.0) {
        map.prob[static_cast<std::size_t>(y) * 200 + x] = 0.9F;
        min_x = std::min(min_x, static_cast<float>(x));
        min_y = std::min(min_y, static_cast<float>(y));
        max_x = std::max(max_x, static_cast<float>(x + 1));
        max_y = std::max(max_y, static_cast<float>(y + 1));
      }
    }
  }
  const auto found = db::decode(map.prob, map.size);
  REQUIRE(found.size() == 1);
  CHECK(contains(found[0].box.rect, {min_x, min_y, max_x - min_x, max_y - min_y}));
}

TEST_CASE("db::decode keeps every candidate (no max_candidates cap)", "[text][db]") {
  Map map({640, 640});
  // 32 x 40 = 1280 separate 8x6 blocks, more than RapidOCR's 1000 cap.
  for (std::uint32_t row = 0; row < 40; ++row) {
    for (std::uint32_t col = 0; col < 32; ++col) {
      map.fill(col * 20 + 4, row * 16 + 4, 8, 6, 0.9F);
    }
  }
  CHECK(db::decode(map.prob, map.size).size() == 1280);
}

TEST_CASE("db::decode fails closed on malformed maps", "[text][db]") {
  Map map({64, 32});
  map.fill(10, 10, 30, 8, 0.9F);
  CHECK_THROWS_AS(db::decode(std::span<const float>(map.prob).first(100), map.size), db::DecodeError);
  CHECK_THROWS_AS(db::decode(map.prob, Size{0, 32}), db::DecodeError);
  for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
    std::vector<float> broken = map.prob;
    broken[5] = bad;  // far from the text: a NaN anywhere fails the frame
    CHECK_THROWS_AS(db::decode(broken, map.size), db::DecodeError);
  }
}

TEST_CASE("db params are validated", "[text][db]") {
  Map map({32, 32});
  db::Params params;
  params.bin_threshold = 1.5F;
  CHECK_THROWS_AS(db::decode(map.prob, map.size, params), std::invalid_argument);
  params = {};
  params.box_threshold = std::nanf("");
  CHECK_THROWS_AS(db::decode(map.prob, map.size, params), std::invalid_argument);
  params = {};
  params.unclip_ratio = -1.0F;
  CHECK_THROWS_AS(db::decode(map.prob, map.size, params), std::invalid_argument);
  params = {};
  params.min_size = std::numeric_limits<float>::infinity();
  CHECK_THROWS_AS(db::decode(map.prob, map.size, params), std::invalid_argument);
}

// --- Tiles -----------------------------------------------------------------------

TEST_CASE("tile_crops covers the frame with overlapping whole-pixel tiles", "[text][tiles]") {
  std::mt19937 rng(606);
  std::uniform_int_distribution<std::uint32_t> side(1, 3000);
  std::uniform_int_distribution<std::uint32_t> tile_steps(2, 40);
  for (int trial = 0; trial < 1000; ++trial) {
    const Size frame{side(rng), side(rng)};
    const std::uint32_t tile = 32 * tile_steps(rng);
    const std::uint32_t overlap = std::uniform_int_distribution<std::uint32_t>(0, tile - 1)(rng);
    const auto crops = spg::detect::tile_crops(frame, tile, overlap);
    REQUIRE(!crops.empty());
    std::vector<float> xs;
    std::vector<float> ys;
    for (const Rect& c : crops) {
      REQUIRE(c.x == std::floor(c.x));
      REQUIRE(c.y == std::floor(c.y));
      REQUIRE(c.width == static_cast<float>(std::min(frame.width, tile)));
      REQUIRE(c.height == static_cast<float>(std::min(frame.height, tile)));
      REQUIRE(c.x >= 0.0F);
      REQUIRE(c.y >= 0.0F);
      REQUIRE(static_cast<double>(c.x) + c.width <= frame.width);
      REQUIRE(static_cast<double>(c.y) + c.height <= frame.height);
      xs.push_back(c.x);
      ys.push_back(c.y);
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    REQUIRE(xs.size() * ys.size() == crops.size());
    const auto check_axis = [&](const std::vector<float>& starts, std::uint32_t length) {
      REQUIRE(starts.front() == 0.0F);
      const float span = static_cast<float>(std::min(length, tile));
      REQUIRE(starts.back() + span == static_cast<float>(length));
      for (std::size_t i = 1; i < starts.size(); ++i) {
        // Each neighbour starts at least `overlap` before the previous ends.
        REQUIRE(starts[i - 1] + span - starts[i] >= static_cast<float>(overlap));
        REQUIRE(starts[i] > starts[i - 1]);
      }
      // Fewest tiles: one fewer could not keep the overlap.
      if (starts.size() > 1) {
        const double needed = (static_cast<double>(length) - overlap) / (static_cast<double>(tile) - overlap);
        REQUIRE(static_cast<double>(starts.size()) < needed + 1.0);
      }
    };
    check_axis(xs, frame.width);
    check_axis(ys, frame.height);
  }
}

TEST_CASE("tile_crops examples and errors", "[text][tiles]") {
  const auto crops = spg::detect::tile_crops({960, 540}, 640, 64);
  REQUIRE(crops.size() == 2);
  CHECK(crops[0] == Rect{0, 0, 640, 540});
  CHECK(crops[1] == Rect{320, 0, 640, 540});
  CHECK(spg::detect::tile_crops({640, 640}, 640, 64) == std::vector<Rect>{{0, 0, 640, 640}});
  CHECK(spg::detect::tile_crops({1920, 1080}, 640, 64).size() == 4 * 2);
  CHECK_THROWS_AS(spg::detect::tile_crops({0, 10}, 640, 64), spg::ContractError);
  CHECK_THROWS_AS(spg::detect::tile_crops({10, 10}, 640, 640), spg::detect::TextDetectorError);
}

TEST_CASE("tile_crops takes rectangular tiles", "[text][tiles]") {
  // The default text tile holds a whole 1080p readback.
  CHECK(spg::detect::tile_crops({1920, 1080}, Size{1920, 1088}, 64) == std::vector<Rect>{{0, 0, 1920, 1080}});
  CHECK(spg::detect::tile_crops({1280, 720}, Size{1920, 1088}, 64) == std::vector<Rect>{{0, 0, 1280, 720}});
  CHECK(spg::detect::tile_crops({3840, 2160}, Size{1920, 1088}, 64).size() == 3 * 3);
  CHECK_THROWS_AS(spg::detect::tile_crops({10, 10}, Size{1920, 64}, 64), spg::detect::TextDetectorError);
  // Each axis follows its own side, as square tiles of that side would.
  std::mt19937 rng(1616);
  std::uniform_int_distribution<std::uint32_t> side(1, 4000);
  std::uniform_int_distribution<std::uint32_t> tile_steps(2, 60);
  for (int trial = 0; trial < 500; ++trial) {
    const Size frame{side(rng), side(rng)};
    const Size tile{32 * tile_steps(rng), 32 * tile_steps(rng)};
    const std::uint32_t overlap =
        std::uniform_int_distribution<std::uint32_t>(0, std::min(tile.width, tile.height) - 1)(rng);
    const auto crops = spg::detect::tile_crops(frame, tile, overlap);
    const auto wide = spg::detect::tile_crops(frame, tile.width, overlap);
    const auto tall = spg::detect::tile_crops(frame, tile.height, overlap);
    std::vector<float> xs;
    std::vector<float> ys;
    for (const Rect& c : crops) {
      xs.push_back(c.x);
      ys.push_back(c.y);
      REQUIRE(c.width == static_cast<float>(std::min(frame.width, tile.width)));
      REQUIRE(c.height == static_cast<float>(std::min(frame.height, tile.height)));
    }
    const auto starts = [](const std::vector<Rect>& rects, bool x) {
      std::vector<float> out;
      for (const Rect& r : rects) {
        out.push_back(x ? r.x : r.y);
      }
      std::sort(out.begin(), out.end());
      out.erase(std::unique(out.begin(), out.end()), out.end());
      return out;
    };
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    REQUIRE(xs == starts(wide, true));
    REQUIRE(ys == starts(tall, false));
    REQUIRE(xs.size() * ys.size() == crops.size());
  }
}

TEST_CASE("The default text tile: one run up to 1080p, nine at scale 1 for 4K", "[text][tiles]") {
  const spg::detect::TextDetectorConfig config;
  const Size tile{config.tile_width, config.tile_height};
  for (const Size frame : {Size{1920, 1080}, Size{1280, 720}, Size{960, 540}, Size{640, 360}}) {
    INFO(frame.width << "x" << frame.height);
    CHECK(spg::detect::tile_crops(frame, tile, config.tile_overlap).size() == 1);
  }
  const auto crops = spg::detect::tile_crops({3840, 2160}, tile, config.tile_overlap);
  REQUIRE(crops.size() == 9);
  std::vector<float> xs;
  std::vector<float> ys;
  for (const Rect& c : crops) {
    CHECK(c.width == static_cast<float>(config.tile_width));
    CHECK(c.height == 1088.0F);
    const auto m = spg::detect::tile_input_mapping(c, tile, config.upscale_side);
    CHECK(m.scale_x() == 1.0);
    CHECK(m.scale_y() == 1.0);
    xs.push_back(c.x);
    ys.push_back(c.y);
  }
  const auto axis = [&](std::vector<float> starts, float span, float length) {
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    REQUIRE(starts.size() == 3);
    CHECK(starts.front() == 0.0F);
    CHECK(starts.back() + span == length);
    for (std::size_t i = 1; i < starts.size(); ++i) {
      CHECK(starts[i - 1] + span - starts[i] >= static_cast<float>(config.tile_overlap));
    }
  };
  axis(xs, 1920.0F, 3840.0F);
  axis(ys, 1088.0F, 2160.0F);
}

TEST_CASE("tile_input_mapping scales a crop up only to upscale_side", "[text][tiles]") {
  using spg::detect::tile_input_mapping;
  const Size tile{1920, 1088};
  CHECK(tile_input_mapping({0, 0, 1920, 1080}, tile, 640).scale_x() == 1.0);
  CHECK(tile_input_mapping({0, 0, 1280, 720}, tile, 640).scale_x() == 1.0);
  CHECK(tile_input_mapping({0, 0, 960, 540}, tile, 640).scale_x() == 1.0);
  CHECK(tile_input_mapping({0, 0, 320, 160}, tile, 640).scale_x() == 2.0);
  // Never past what the tile holds.
  CHECK(tile_input_mapping({0, 0, 100, 1000}, Size{640, 1088}, 8192).scale_x() == Approx(1.088));
  // Content stays at the origin; the rest is padding.
  const auto m = tile_input_mapping({0, 0, 960, 540}, tile, 640);
  CHECK(m.pad_x() == 0.0);
  CHECK(m.pad_y() == 0.0);
  // For a crop no larger than a square tile, the same scale as before SPG-16.
  std::mt19937 rng(1617);
  std::uniform_int_distribution<int> side(1, 640);
  for (int trial = 0; trial < 500; ++trial) {
    const Rect crop{0.0F, 0.0F, static_cast<float>(side(rng)), static_cast<float>(side(rng))};
    const auto old = tile_input_mapping(crop, 640U);
    const auto now = tile_input_mapping(crop, tile, 640);
    REQUIRE(now.scale_x() == Approx(old.scale_x()).epsilon(1e-12));
    REQUIRE(now.scale_y() == Approx(old.scale_y()).epsilon(1e-12));
  }
}

TEST_CASE("Tile boxes map to Source space through the tile's mapping", "[text][tiles]") {
  const Size source{1920, 1080};
  const Size readback{960, 540};
  const Rect crop{320, 0, 640, 540};
  const spg::SpaceMap map{spg::ReadbackMapping(source, readback), spg::detect::tile_input_mapping(crop, 640)};
  CHECK(map.model_input->scale_x() == 1.0);
  CHECK(map.model_input->pad_x() == 0.0);
  const std::vector<db::Candidate> candidates{
      {{{10, 20, 30, 8}, Space::ModelInput}, 0.8F, {{12, 22, 26, 4}, Space::ModelInput}},
      // partly in the letterbox padding
      {{{600, 520, 100, 100}, Space::ModelInput}, 0.7F, {{620, 540, 60, 60}, Space::ModelInput}},
      // wholly in the padding
      {{{10, 560, 30, 30}, Space::ModelInput}, 0.9F, {{12, 562, 26, 26}, Space::ModelInput}},
  };
  const auto found = spg::detect::tile_to_source(candidates, map, 3);
  REQUIRE(found.size() == 2);
  CHECK(found[0].tile == 3);
  CHECK(found[0].detection.kind == spg::DetectionKind::TextRegion);
  CHECK(found[0].detection.detector == spg::DetectorId::PpOcrDet);
  CHECK(found[0].detection.box == Box{{660, 40, 60, 16}, Space::Source});
  CHECK(found[0].line == Rect{664, 44, 52, 8});
  // Clipped to the source: x from 1840 to 1920, y from 1040 to 1080. The
  // kernel is converted but not clipped; it is only compared.
  CHECK(found[1].detection.box == Box{{1840, 1040, 80, 40}, Space::Source});
  CHECK(found[1].line == Rect{1880, 1080, 120, 120});
}

TEST_CASE("A frame smaller than a tile is scaled up uniformly", "[text][tiles]") {
  const auto crops = spg::detect::tile_crops({320, 160}, 640, 64);
  REQUIRE(crops.size() == 1);
  const auto mapping = spg::detect::tile_input_mapping(crops[0], 640);
  CHECK(mapping.scale_x() == 2.0);
  CHECK(mapping.scale_y() == 2.0);
  const spg::SpaceMap map{spg::ReadbackMapping({320, 160}, {320, 160}), mapping};
  const std::vector<db::Candidate> candidates{
      {{{100, 50, 40, 20}, Space::ModelInput}, 0.8F, {{104, 54, 32, 12}, Space::ModelInput}}};
  const auto found = spg::detect::tile_to_source(candidates, map, 0);
  REQUIRE(found.size() == 1);
  CHECK(found[0].detection.box == Box{{50, 25, 20, 10}, Space::Source});
  CHECK(found[0].line == Rect{52, 27, 16, 6});
}

// --- Merging ----------------------------------------------------------------------

TEST_CASE("merge_across_tiles unites one line seen by two tiles", "[text][merge]") {
  const std::vector<TileDetection> boxes{
      on_tile({100, 50, 560, 20}, 0, 0.7F),  // cut at the tile edge
      on_tile({600, 52, 300, 18}, 1, 0.9F),  // the rest, overlapping
      on_tile({100, 200, 80, 20}, 0, 0.8F),  // another line
  };
  const auto merged = spg::detect::merge_across_tiles(boxes, 0.5F);
  REQUIRE(merged.size() == 2);
  CHECK(merged[0].box.rect == Rect{100, 50, 800, 20});
  CHECK(merged[0].score == 0.9F);
  CHECK(merged[1].box.rect == Rect{100, 200, 80, 20});
}

TEST_CASE("merge_across_tiles leaves same-tile boxes and different lines apart", "[text][merge]") {
  SECTION("same tile, overlapping") {
    const std::vector<TileDetection> boxes{on_tile({0, 0, 50, 20}, 0), on_tile({40, 2, 50, 20}, 0)};
    CHECK(spg::detect::merge_across_tiles(boxes, 0.5F).size() == 2);
  }
  SECTION("different tiles, lines touching at their margins") {
    const std::vector<TileDetection> boxes{on_tile({0, 0, 50, 20}, 0), on_tile({40, 15, 50, 20}, 1)};
    CHECK(spg::detect::merge_across_tiles(boxes, 0.5F).size() == 2);
  }
  SECTION("different tiles, edges only touching") {
    const std::vector<TileDetection> boxes{on_tile({0, 0, 50, 20}, 0), on_tile({50, 0, 50, 20}, 1)};
    CHECK(spg::detect::merge_across_tiles(boxes, 0.5F).size() == 2);
  }
  SECTION("different tiles, grown boxes overlapping by half but kernels apart") {
    // Two adjacent lines: the grown boxes overlap vertically by 17 of 33 px,
    // the 13 px kernels not at all.
    const std::vector<TileDetection> boxes{
        {text_box({60, 90, 80, 33}), 0, {70, 100, 60, 13}},
        {text_box({60, 106, 80, 33}), 1, {70, 116, 60, 13}},
    };
    CHECK(spg::detect::merge_across_tiles(boxes, 0.5F).size() == 2);
  }
  SECTION("chains merge transitively") {
    const std::vector<TileDetection> boxes{on_tile({0, 0, 60, 20}, 0), on_tile({50, 0, 60, 20}, 1),
                                           on_tile({100, 0, 60, 20}, 0)};
    const auto merged = spg::detect::merge_across_tiles(boxes, 0.5F);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].box.rect == Rect{0, 0, 160, 20});
  }
}

TEST_CASE("Two adjacent lines seen by two tiles stay two boxes; a line across the seam is one",
          "[text][merge]") {
  // A 1216x640 frame read back unscaled: tiles at x = 0 and x = 576. Two 60x12
  // kernels 4 px apart sit in the tile overlap, so both tiles decode both; a
  // third line runs from x = 400 to 900, across the seam.
  const Size frame{1216, 640};
  const auto crops = spg::detect::tile_crops(frame, 640, 64);
  REQUIRE(crops.size() == 2);
  REQUIRE(crops[1].x == 576.0F);
  const spg::ReadbackMapping readback(frame, frame);
  std::vector<TileDetection> found;
  for (std::size_t t = 0; t < crops.size(); ++t) {
    Map map({640, 640});
    const auto offset = static_cast<std::uint32_t>(crops[t].x);
    map.fill(576 - offset, 100, 60, 12, 0.9F);
    map.fill(576 - offset, 116, 60, 12, 0.9F);
    const std::uint32_t seam_start = std::max<std::uint32_t>(400, offset);
    const std::uint32_t seam_end = std::min<std::uint32_t>(900, offset + 640);
    map.fill(seam_start - offset, 300, seam_end - seam_start, 12, 0.9F);
    const auto candidates = db::decode(map.prob, map.size);
    REQUIRE(candidates.size() == 3);
    const spg::SpaceMap space{readback, spg::detect::tile_input_mapping(crops[t], 640)};
    const auto boxes = spg::detect::tile_to_source(candidates, space, t);
    found.insert(found.end(), boxes.begin(), boxes.end());
  }
  const auto merged = spg::detect::merge_across_tiles(found, 0.5F);
  REQUIRE(merged.size() == 3);
  const Rect upper{576, 100, 60, 12};
  const Rect lower{576, 116, 60, 12};
  CHECK(contains(merged[0].box.rect, upper));
  CHECK_FALSE(contains(merged[0].box.rect, lower));
  CHECK(contains(merged[1].box.rect, lower));
  CHECK_FALSE(contains(merged[1].box.rect, upper));
  CHECK(contains(merged[2].box.rect, {400, 300, 500, 12}));
}

TEST_CASE("merge_across_tiles matches the all-pairs result on dense frames", "[text][merge]") {
  // Reference: link every pair from different tiles whose boxes intersect and
  // whose kernels overlap vertically by half the shorter, then unite.
  const auto reference = [](const std::vector<TileDetection>& boxes) {
    std::vector<std::size_t> group(boxes.size());
    std::iota(group.begin(), group.end(), std::size_t{0});
    const auto find = [&](std::size_t i) {
      while (group[i] != i) {
        i = group[i];
      }
      return i;
    };
    for (std::size_t i = 0; i < boxes.size(); ++i) {
      for (std::size_t j = i + 1; j < boxes.size(); ++j) {
        const Rect hit = spg::intersection(boxes[i].detection.box.rect, boxes[j].detection.box.rect);
        const Rect& a = boxes[i].line;
        const Rect& b = boxes[j].line;
        const double over = std::min(static_cast<double>(a.y) + a.height, static_cast<double>(b.y) + b.height) -
                            std::max(static_cast<double>(a.y), static_cast<double>(b.y));
        if (boxes[i].tile != boxes[j].tile && hit.width > 0.0F && hit.height > 0.0F &&
            over >= 0.5 * std::min(a.height, b.height)) {
          group[find(j)] = find(i);
        }
      }
    }
    std::vector<std::vector<std::size_t>> members(boxes.size());
    for (std::size_t i = 0; i < boxes.size(); ++i) {
      members[find(i)].push_back(i);
    }
    std::vector<Rect> united;
    for (const auto& m : members) {
      if (m.empty()) {
        continue;
      }
      double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
      for (const std::size_t i : m) {
        const Rect& r = boxes[i].detection.box.rect;
        x0 = std::min(x0, static_cast<double>(r.x));
        y0 = std::min(y0, static_cast<double>(r.y));
        x1 = std::max(x1, static_cast<double>(r.x) + r.width);
        y1 = std::max(y1, static_cast<double>(r.y) + r.height);
      }
      united.push_back({static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0),
                        static_cast<float>(y1 - y0)});
    }
    return united;
  };
  const auto key = [](const Rect& r) { return std::tuple{r.y, r.x, r.height, r.width}; };
  std::mt19937 rng(3506);
  std::uniform_int_distribution<int> pos(0, 1900);
  std::uniform_int_distribution<int> row(0, 1060);
  std::uniform_int_distribution<int> width(8, 300);
  std::uniform_int_distribution<int> height(6, 40);
  std::uniform_int_distribution<std::size_t> tile(0, 7);
  for (int trial = 0; trial < 20; ++trial) {
    std::vector<TileDetection> boxes;
    for (int i = 0; i < 600; ++i) {
      const auto x = static_cast<float>(pos(rng));
      const auto y = static_cast<float>(row(rng));
      const auto h = static_cast<float>(height(rng));
      const Rect box{x, y, static_cast<float>(width(rng)), h};
      boxes.push_back({text_box(box), tile(rng), {x + 2.0F, y + h / 3.0F, box.width - 4.0F, h / 3.0F}});
    }
    const auto merged = spg::detect::merge_across_tiles(boxes, 0.5F);
    std::vector<Rect> got;
    for (const Detection& d : merged) {
      got.push_back(d.box.rect);
    }
    std::vector<Rect> want = reference(boxes);
    std::sort(got.begin(), got.end(), [&](const Rect& a, const Rect& b) { return key(a) < key(b); });
    std::sort(want.begin(), want.end(), [&](const Rect& a, const Rect& b) { return key(a) < key(b); });
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
      CHECK(got[i].x == want[i].x);
      CHECK(got[i].y == want[i].y);
      CHECK(got[i].width == Approx(want[i].width).margin(1e-3));
      CHECK(got[i].height == Approx(want[i].height).margin(1e-3));
    }
  }
}

TEST_CASE("merge_across_tiles sorts top to bottom, then left to right", "[text][merge]") {
  const std::vector<TileDetection> boxes{on_tile({300, 10, 10, 10}, 0), on_tile({5, 40, 10, 10}, 0),
                                         on_tile({100, 10, 10, 10}, 0)};
  const auto merged = spg::detect::merge_across_tiles(boxes, 0.5F);
  REQUIRE(merged.size() == 3);
  CHECK(merged[0].box.rect.x == 100.0F);
  CHECK(merged[1].box.rect.x == 300.0F);
  CHECK(merged[2].box.rect.y == 40.0F);
}

// --- Input tensor and config ---------------------------------------------------

TEST_CASE("Text input tensor is planar BGR scaled to [-1, 1]", "[text][tensor]") {
  spg::detect::BgrImage image{{2, 1}, {0, 128, 255, 255, 0, 51}};
  const auto tensor = spg::detect::to_text_input_tensor(image);
  REQUIRE(tensor.size() == 6);
  // Planes B, G, R; pixel 0 is (B 0, G 128, R 255), pixel 1 is (255, 0, 51).
  CHECK(tensor[0] == Approx(-1.0F));
  CHECK(tensor[1] == Approx(1.0F));
  CHECK(tensor[2] == Approx(128.0F / 127.5F - 1.0F));
  CHECK(tensor[3] == Approx(-1.0F));
  CHECK(tensor[4] == Approx(1.0F));
  CHECK(tensor[5] == Approx(51.0F / 127.5F - 1.0F));
  image.pixels.pop_back();
  CHECK_THROWS_AS(spg::detect::to_text_input_tensor(image), spg::ContractError);
}

TEST_CASE("TextDetectorConfig is validated", "[text][config]") {
  using spg::detect::TextDetectorConfig;
  using spg::detect::TextDetectorError;
  CHECK_NOTHROW(spg::detect::validate(TextDetectorConfig{}));
  TextDetectorConfig config;
  config.tile_width = 100;
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
  config = {};
  config.tile_height = 0;
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
  config = {};
  config.tile_height = 1080;  // not a multiple of 32
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
  config = {};
  config.tile_width = 8192 + 32;
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
  // Overlap is capped at half the shorter tile side: 639 of a 640 tile would
  // mean 1281 x 441 model runs for one 1920x1080 frame.
  config = {};
  config.tile_width = 640;
  config.tile_height = 640;
  config.tile_overlap = 640;
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
  config.tile_overlap = 320;
  CHECK_NOTHROW(spg::detect::validate(config));
  config.tile_overlap = 321;
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
  config.tile_overlap = 639;
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
  // The default 1920 x 1088 tile: half of 1088.
  config = {};
  config.tile_overlap = 544;
  CHECK_NOTHROW(spg::detect::validate(config));
  config.tile_overlap = 545;
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
  config = {};
  config.merge_line_overlap = 0.0F;
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
  config = {};
  config.db.unclip_ratio = std::nanf("");
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
}

TEST_CASE("clip_to narrows to the frame and drops boxes outside it", "[text][clip]") {
  CHECK(spg::detect::clip_to({-5, -5, 20, 20}, {100, 100}) == Rect{0, 0, 15, 15});
  CHECK(spg::detect::clip_to({90, 90, 20, 20}, {100, 100}) == Rect{90, 90, 10, 10});
  CHECK_FALSE(spg::detect::clip_to({100, 0, 5, 5}, {100, 100}).has_value());
  CHECK_FALSE(spg::detect::clip_to({-10, 0, 10, 5}, {100, 100}).has_value());
}
