// spg::detect::TextDetector end to end with the pinned PP-OCR detector. Cases
// that need the model skip when scripts/fetch_models.py has not cached it; CI
// runs setup first, so they run there. Frames are generated here: plain and
// gradient backgrounds, UI-like panels, and made-up strings drawn with
// OpenCV's Hershey fonts (synthetic_text.hpp). Nothing is a real person's data.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include "detect/text_detector.hpp"
#include "runtime/onnx_session.hpp"
#include "spg/error.hpp"
#include "synthetic_frames.hpp"
#include "synthetic_text.hpp"

namespace {

namespace fs = std::filesystem;
using spg::Rect;
using spg::Size;
using spg::detect::TextDetector;
using spg::detect::TextDetectorConfig;
using spg::detect::TextDetectorError;
using spg::runtime::Environment;
using spg::runtime::Provider;
using spg::runtime::SessionOptions;
using spg::test::Bgr;
using spg::test::Canvas;

Environment& shared_env() {
  static Environment env(Environment::LogLevel::Error);
  return env;
}

fs::path ppocr_det_path() {
  const std::string path = SPG_PPOCR_DET_MODEL;
  return fs::path(std::u8string(path.begin(), path.end()));
}

SessionOptions cpu_only() {
  SessionOptions options;
  options.providers = {Provider::Cpu};
  options.cpu_fallback = false;
  return options;
}

// One detector per provider choice for the whole run; opening costs a warmup.
const TextDetector& detector(bool accelerated = false) {
  static const TextDetector cpu = TextDetector::open(shared_env(), ppocr_det_path(), {}, cpu_only());
  if (!accelerated) {
    return cpu;
  }
  static const TextDetector fast = TextDetector::open(shared_env(), ppocr_det_path());
  return fast;
}

#define SPG_REQUIRE_PPOCR_DET()                                                                       \
  do {                                                                                                \
    if (!fs::is_regular_file(ppocr_det_path())) {                                                     \
      SKIP("ppocr-det-en not in the cache (run scripts/fetch_models.py): " << ppocr_det_path().string()); \
    }                                                                                                 \
  } while (false)

struct Scene {
  Size source;
  Size readback;
  spg::PixelFormat format;
};

std::string describe(const Scene& s) {
  return std::to_string(s.source.width) + "x" + std::to_string(s.source.height) + " read back at " +
         std::to_string(s.readback.width) + "x" + std::to_string(s.readback.height) + " " +
         std::string(spg::to_string(s.format));
}

// A readback-pixel rect in Source pixels, through the one conversion module.
Rect to_source(const Rect& readback_rect, const Scene& scene) {
  const spg::SpaceMap map{spg::ReadbackMapping(scene.source, scene.readback), std::nullopt};
  return spg::convert({readback_rect, spg::Space::Readback}, spg::Space::Source, map).rect;
}

std::string boxes_text(const std::vector<spg::Detection>& found) {
  std::string text;
  for (const spg::Detection& d : found) {
    const Rect& r = d.box.rect;
    text += " [" + std::to_string(static_cast<int>(r.x)) + "," + std::to_string(static_cast<int>(r.y)) + " " +
            std::to_string(static_cast<int>(r.width)) + "x" + std::to_string(static_cast<int>(r.height)) + " " +
            std::to_string(d.score) + "]";
  }
  return text;
}

// Whether `box` covers `ink` once each of its edges moves out by `slack`
// times the ink height. DBNet boxes are a regrown shrunk kernel, so an edge
// can land a little inside anti-aliased ink; the mask margin downstream
// (PolicyConfig margin_px + margin_ratio) covers that much and more.
bool covers(const Rect& box, const Rect& ink, double slack) {
  const double pad = slack * ink.height;
  return box.x - pad <= ink.x && box.y - pad <= ink.y &&
         static_cast<double>(box.x) + box.width + pad >= static_cast<double>(ink.x) + ink.width &&
         static_cast<double>(box.y) + box.height + pad >= static_cast<double>(ink.y) + ink.height;
}

// The largest edge shortfall measured on these lines was about 2% of the ink
// height with unclip_ratio 2.0 (1.6 left up to 11%); see db_postprocess.hpp.
constexpr double kSlack = 0.05;

bool overlaps(const Rect& a, const Rect& b) {
  const Rect i = spg::intersection(a, b);
  return i.width > 0.0F && i.height > 0.0F;
}

// Made-up strings only: no names, addresses or numbers that belong to anyone.
const std::vector<std::string> kLines{"SPG SYNTHETIC LINE 0123", "hello stream 42", "QUICK BROWN FOX",
                                      "lorem ipsum dolor sit amet", "TEST 555-0100"};

}  // namespace

TEST_CASE("TextDetector rejects a bad config before opening anything", "[text][detector]") {
  TextDetectorConfig config;
  config.tile_size = 650;
  CHECK_THROWS_AS(TextDetector::open(shared_env(), "does-not-exist.onnx", config), TextDetectorError);
  config = {};
  config.db.box_threshold = -0.1F;
  CHECK_THROWS_AS(TextDetector::open(shared_env(), "does-not-exist.onnx", config), TextDetectorError);
}

TEST_CASE("TextDetector reports a missing model and refuses a model that is not DBNet", "[text][detector]") {
  try {
    static_cast<void>(TextDetector::open(shared_env(), fs::path(SPG_RUNTIME_FIXTURES) / "absent.onnx"));
    FAIL("open() accepted a missing model");
  } catch (const spg::runtime::RuntimeError& error) {
    CHECK(error.code() == spg::runtime::ErrorCode::ModelNotFound);
  }
  SessionOptions options = cpu_only();
  options.warmup_runs = 0;
  CHECK_THROWS_AS(TextDetector::open(shared_env(), fs::path(SPG_RUNTIME_FIXTURES) / "scale_static.onnx", {}, options),
                  TextDetectorError);
}

TEST_CASE("TextDetector opens the pinned PP-OCR detector", "[text][detector][model]") {
  SPG_REQUIRE_PPOCR_DET();
  CHECK(detector().session().provider() == Provider::Cpu);
  CHECK(detector().config().tile_size == 640);
  CHECK_NOTHROW(detector(true));
}

TEST_CASE("TextDetector rejects invalid frames and sources", "[text][detector][model]") {
  SPG_REQUIRE_PPOCR_DET();
  const auto frame = spg::test::to_frame(Canvas(64, 48), spg::PixelFormat::BGRA);
  CHECK_THROWS_AS(detector().detect(frame.view, {0, 48}), spg::ContractError);
  spg::FrameView broken = frame.view;
  broken.planes[0] = nullptr;
  CHECK_THROWS_AS(detector().detect(broken, {64, 48}), spg::ContractError);
}

TEST_CASE("TextDetector finds no text in frames without text", "[text][detector][model]") {
  SPG_REQUIRE_PPOCR_DET();
  const Scene scene = GENERATE(Scene{{1920, 1080}, {960, 540}, spg::PixelFormat::BGRA},
                               Scene{{1280, 720}, {1280, 720}, spg::PixelFormat::NV12},
                               Scene{{641, 479}, {641, 479}, spg::PixelFormat::I420});
  const int pattern = GENERATE(range(0, 4));
  const Size rb = scene.readback;
  Canvas canvas(rb.width, rb.height);
  const double w = rb.width;
  const double h = rb.height;
  switch (pattern) {
    case 0:  // black
      break;
    case 1:
      canvas = Canvas(rb.width, rb.height, {128, 128, 128});
      break;
    case 2:  // horizontal gradient
      for (std::uint32_t y = 0; y < rb.height; ++y) {
        for (std::uint32_t x = 0; x < rb.width; ++x) {
          const auto v = static_cast<std::uint8_t>(255 * x / rb.width);
          canvas.set(x, y, {v, v, v});
        }
      }
      break;
    default:  // UI-like panels: large flat rectangles, no glyphs
      canvas = Canvas(rb.width, rb.height, {30, 30, 30});
      canvas.rect(0.05 * w, 0.1 * h, 0.45 * w, 0.9 * h, {60, 60, 60});
      canvas.rect(0.55 * w, 0.1 * h, 0.95 * w, 0.4 * h, {200, 120, 40});
      canvas.ellipse(0.75 * w, 0.7 * h, 0.1 * w, 0.1 * h, {40, 160, 40});
      break;
  }
  const auto frame = spg::test::to_frame(canvas, scene.format);
  INFO(describe(scene) << ", pattern " << pattern);
  CHECK(detector().detect(frame.view, scene.source).empty());
}

TEST_CASE("TextDetector covers every synthetic line with one Source-space box", "[text][detector][model]") {
  SPG_REQUIRE_PPOCR_DET();
  const Scene scene = GENERATE(Scene{{1920, 1080}, {960, 540}, spg::PixelFormat::BGRA},
                               Scene{{1280, 720}, {1280, 720}, spg::PixelFormat::NV12},
                               Scene{{641, 479}, {641, 479}, spg::PixelFormat::I420},
                               Scene{{1080, 1920}, {540, 960}, spg::PixelFormat::BGRA},
                               Scene{{2560, 1440}, {1280, 720}, spg::PixelFormat::I420});
  const bool accelerated = GENERATE(false, true);
  const int style = GENERATE(range(0, 2));
  const Size rb = scene.readback;
  const Bgr background = style == 0 ? Bgr{30, 30, 30} : Bgr{245, 245, 245};
  const Bgr ink = style == 0 ? Bgr{235, 235, 235} : Bgr{20, 20, 20};
  Canvas canvas(rb.width, rb.height, background);

  // Lines spread down the frame, some starting near the left edge and the
  // first one long enough to cross a vertical tile edge on wide frames.
  std::vector<Rect> truth;
  const int rows = static_cast<int>(kLines.size());
  for (int i = 0; i < rows; ++i) {
    const double height = 14.0 + 3.0 * i;  // cap height, readback pixels
    const int baseline = static_cast<int>(rb.height) * (i + 1) / (rows + 1);
    const int x = 8 + static_cast<int>(rb.width / 9) * (i % 3);
    const Rect box = spg::test::draw_text(canvas, kLines[static_cast<std::size_t>(i)], x, baseline, height, ink);
    REQUIRE(box.width > 0.0F);
    // Keep lines the frame cuts off out of the truth: only fully drawn ones.
    if (static_cast<double>(box.x) + box.width < rb.width - 1) {
      truth.push_back(to_source(box, scene));
    }
  }
  REQUIRE(truth.size() >= 3);

  const auto frame = spg::test::to_frame(canvas, scene.format);
  const auto found = detector(accelerated).detect(frame.view, scene.source);
  INFO(describe(scene) << (accelerated ? ", default providers" : ", cpu") << ", style " << style << ", "
                       << found.size() << " boxes:" << boxes_text(found));
  for (const spg::Detection& d : found) {
    CHECK(d.kind == spg::DetectionKind::TextRegion);
    CHECK(d.detector == spg::DetectorId::PpOcrDet);
    CHECK(d.box.space == spg::Space::Source);
    CHECK(d.score >= detector().config().db.box_threshold);
    // Every box is on some line: no detections on the plain background.
    CHECK(std::any_of(truth.begin(), truth.end(), [&](const Rect& t) { return overlaps(d.box.rect, t); }));
  }
  for (std::size_t i = 0; i < truth.size(); ++i) {
    INFO("line " << i << " at " << truth[i].x << "," << truth[i].y << " size " << truth[i].width << "x"
                 << truth[i].height);
    CHECK(std::any_of(found.begin(), found.end(),
                      [&](const spg::Detection& d) { return covers(d.box.rect, truth[i], kSlack); }));
  }
}

TEST_CASE("TextDetector covers small lines at readback scale", "[text][detector][model]") {
  SPG_REQUIRE_PPOCR_DET();
  // Readback cap heights 6 to 12 px (ink 9 to 15 px tall). A probe during
  // SPG-06 found every line from cap 5 up and only some at cap 4; the plugin's
  // readback scale decides how small Source text gets here (SPG-12, SPG-14).
  const double cap = GENERATE(6.0, 8.0, 10.0, 12.0);
  const int style = GENERATE(range(0, 2));
  Canvas canvas(960, 540, style == 0 ? Bgr{30, 30, 30} : Bgr{245, 245, 245});
  const Bgr ink = style == 0 ? Bgr{235, 235, 235} : Bgr{20, 20, 20};
  std::vector<Rect> truth;
  for (std::size_t i = 0; i < kLines.size(); ++i) {
    truth.push_back(spg::test::draw_text(canvas, kLines[i], 20, 60 + 90 * static_cast<int>(i), cap, ink));
  }
  const auto frame = spg::test::to_frame(canvas, spg::PixelFormat::BGRA);
  const auto found = detector().detect(frame.view, {960, 540});
  INFO("cap " << cap << ", style " << style << ", " << found.size() << " boxes:" << boxes_text(found));
  CHECK(found.size() == truth.size());
  for (const Rect& line : truth) {
    CHECK(std::any_of(found.begin(), found.end(),
                      [&](const spg::Detection& d) { return covers(d.box.rect, line, kSlack); }));
  }
}

TEST_CASE("TextDetector merges a line that crosses a tile edge", "[text][detector][model]") {
  SPG_REQUIRE_PPOCR_DET();
  // 1600 px wide readback: tiles start at x = 0, 480 and 960 (overlap 160).
  const Scene scene{{1600, 400}, {1600, 400}, spg::PixelFormat::BGRA};
  Canvas canvas(1600, 400, {30, 30, 30});
  const Rect line = spg::test::draw_text(canvas, "SPG SYNTHETIC TILE EDGE CROSSING LINE 0123456789", 40, 200, 30.0,
                                         {235, 235, 235});
  REQUIRE(line.x < 480.0F);
  REQUIRE(static_cast<double>(line.x) + line.width > 1120.0);  // past the first two tiles
  const auto frame = spg::test::to_frame(canvas, scene.format);
  const auto found = detector().detect(frame.view, scene.source);
  INFO(found.size() << " boxes:" << boxes_text(found));
  REQUIRE(!found.empty());
  CHECK(std::any_of(found.begin(), found.end(),
                    [&](const spg::Detection& d) { return covers(d.box.rect, line, kSlack); }));
}

TEST_CASE("TextDetector gives the same answer from several threads", "[text][detector][model]") {
  SPG_REQUIRE_PPOCR_DET();
  Canvas canvas(960, 540, {245, 245, 245});
  for (int i = 0; i < 4; ++i) {
    static_cast<void>(spg::test::draw_text(canvas, kLines[static_cast<std::size_t>(i)], 20 + 150 * i,
                                           80 + 110 * i, 18.0, {20, 20, 20}));
  }
  const auto frame = spg::test::to_frame(canvas, spg::PixelFormat::BGRA);
  const Size source{1920, 1080};
  const auto expected = detector().detect(frame.view, source);
  REQUIRE(expected.size() >= 4);
  std::vector<std::vector<spg::Detection>> results(4);
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < results.size(); ++t) {
    threads.emplace_back([&, t] { results[t] = detector().detect(frame.view, source); });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (const auto& result : results) {
    REQUIRE(result.size() == expected.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
      CHECK(result[i].box == expected[i].box);
      CHECK(result[i].score == expected[i].score);
    }
  }
}
