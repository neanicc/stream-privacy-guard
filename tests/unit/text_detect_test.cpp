// Text detector stages that need no model: DBNet post-processing on synthetic
// probability maps, the tile layout, the tile mapping back to Source space,
// cross-tile merging and the input tensor layout.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
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

TEST_CASE("Tile boxes map to Source space through the tile's mapping", "[text][tiles]") {
  const Size source{1920, 1080};
  const Size readback{960, 540};
  const Rect crop{320, 0, 640, 540};
  const spg::SpaceMap map{spg::ReadbackMapping(source, readback), spg::detect::tile_input_mapping(crop, 640)};
  CHECK(map.model_input->scale_x() == 1.0);
  CHECK(map.model_input->pad_x() == 0.0);
  const std::vector<db::Candidate> candidates{
      {{{10, 20, 30, 8}, Space::ModelInput}, 0.8F},
      {{{600, 520, 100, 100}, Space::ModelInput}, 0.7F},  // partly in the letterbox padding
      {{{10, 560, 30, 30}, Space::ModelInput}, 0.9F},     // wholly in the padding
  };
  const auto found = spg::detect::tile_to_source(candidates, map, 3);
  REQUIRE(found.size() == 2);
  CHECK(found[0].tile == 3);
  CHECK(found[0].detection.kind == spg::DetectionKind::TextRegion);
  CHECK(found[0].detection.detector == spg::DetectorId::PpOcrDet);
  CHECK(found[0].detection.box == Box{{660, 40, 60, 16}, Space::Source});
  // Clipped to the source: x from 1840 to 1920, y from 1040 to 1080.
  CHECK(found[1].detection.box == Box{{1840, 1040, 80, 40}, Space::Source});
}

TEST_CASE("A frame smaller than a tile is scaled up uniformly", "[text][tiles]") {
  const auto crops = spg::detect::tile_crops({320, 160}, 640, 64);
  REQUIRE(crops.size() == 1);
  const auto mapping = spg::detect::tile_input_mapping(crops[0], 640);
  CHECK(mapping.scale_x() == 2.0);
  CHECK(mapping.scale_y() == 2.0);
  const spg::SpaceMap map{spg::ReadbackMapping({320, 160}, {320, 160}), mapping};
  const std::vector<db::Candidate> candidates{{{{100, 50, 40, 20}, Space::ModelInput}, 0.8F}};
  const auto found = spg::detect::tile_to_source(candidates, map, 0);
  REQUIRE(found.size() == 1);
  CHECK(found[0].detection.box == Box{{50, 25, 20, 10}, Space::Source});
}

// --- Merging ----------------------------------------------------------------------

TEST_CASE("merge_across_tiles unites one line seen by two tiles", "[text][merge]") {
  const std::vector<TileDetection> boxes{
      {text_box({100, 50, 560, 20}, 0.7F), 0},  // cut at the tile edge
      {text_box({600, 52, 300, 18}, 0.9F), 1},  // the rest, overlapping
      {text_box({100, 200, 80, 20}, 0.8F), 0},  // another line
  };
  const auto merged = spg::detect::merge_across_tiles(boxes, 0.5F);
  REQUIRE(merged.size() == 2);
  CHECK(merged[0].box.rect == Rect{100, 50, 800, 20});
  CHECK(merged[0].score == 0.9F);
  CHECK(merged[1].box.rect == Rect{100, 200, 80, 20});
}

TEST_CASE("merge_across_tiles leaves same-tile boxes and different lines apart", "[text][merge]") {
  SECTION("same tile, overlapping") {
    const std::vector<TileDetection> boxes{{text_box({0, 0, 50, 20}), 0}, {text_box({40, 2, 50, 20}), 0}};
    CHECK(spg::detect::merge_across_tiles(boxes, 0.5F).size() == 2);
  }
  SECTION("different tiles, lines touching at their margins") {
    const std::vector<TileDetection> boxes{{text_box({0, 0, 50, 20}), 0}, {text_box({40, 15, 50, 20}), 1}};
    CHECK(spg::detect::merge_across_tiles(boxes, 0.5F).size() == 2);
  }
  SECTION("different tiles, edges only touching") {
    const std::vector<TileDetection> boxes{{text_box({0, 0, 50, 20}), 0}, {text_box({50, 0, 50, 20}), 1}};
    CHECK(spg::detect::merge_across_tiles(boxes, 0.5F).size() == 2);
  }
  SECTION("chains merge transitively") {
    const std::vector<TileDetection> boxes{{text_box({0, 0, 60, 20}), 0},
                                           {text_box({50, 0, 60, 20}), 1},
                                           {text_box({100, 0, 60, 20}), 0}};
    const auto merged = spg::detect::merge_across_tiles(boxes, 0.5F);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].box.rect == Rect{0, 0, 160, 20});
  }
}

TEST_CASE("merge_across_tiles sorts top to bottom, then left to right", "[text][merge]") {
  const std::vector<TileDetection> boxes{{text_box({300, 10, 10, 10}), 0},
                                         {text_box({5, 40, 10, 10}), 0},
                                         {text_box({100, 10, 10, 10}), 0}};
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
  config.tile_size = 100;
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
  config = {};
  config.tile_size = 0;
  CHECK_THROWS_AS(spg::detect::validate(config), TextDetectorError);
  config = {};
  config.tile_overlap = 640;
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
