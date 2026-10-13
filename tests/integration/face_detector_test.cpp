// spg::detect::FaceDetector end to end with the pinned YuNet model. Cases that
// need the model skip when scripts/fetch_models.py has not cached it; CI runs
// setup first, so they run there. Frames are synthetic: plain backgrounds and
// shapes for the empty cases, a drawn cartoon face for the positive ones.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include "detect/face_detector.hpp"
#include "runtime/onnx_session.hpp"
#include "spg/error.hpp"
#include "synthetic_frames.hpp"

namespace {

namespace fs = std::filesystem;
using spg::Size;
using spg::detect::FaceDetector;
using spg::detect::FaceDetectorConfig;
using spg::detect::FaceDetectorError;
using spg::runtime::Environment;
using spg::runtime::Provider;
using spg::runtime::SessionOptions;
using spg::test::Canvas;

Environment& shared_env() {
  static Environment env(Environment::LogLevel::Error);
  return env;
}

fs::path yunet_path() {
  const std::string path = SPG_YUNET_MODEL;
  return fs::path(std::u8string(path.begin(), path.end()));
}

SessionOptions cpu_only() {
  SessionOptions options;
  options.providers = {Provider::Cpu};
  options.cpu_fallback = false;
  return options;
}

// One detector per provider choice for the whole run; opening costs a warmup.
const FaceDetector& detector(bool accelerated = false) {
  static const FaceDetector cpu = FaceDetector::open(shared_env(), yunet_path(), {}, cpu_only());
  if (!accelerated) {
    return cpu;
  }
  static const FaceDetector fast = FaceDetector::open(shared_env(), yunet_path());
  return fast;
}

// A CPU detector with `config`, for cases that need tiles on every call (the
// default cadence spreads them over calls, so a shared detector's answer
// would depend on how many calls came before).
FaceDetector open_cpu(const FaceDetectorConfig& config) {
  return FaceDetector::open(shared_env(), yunet_path(), config, cpu_only());
}

FaceDetectorConfig every_tile() {
  FaceDetectorConfig config;
  config.tile_cadence = 1;
  return config;
}

#define SPG_REQUIRE_YUNET()                                                                   \
  do {                                                                                        \
    if (!fs::is_regular_file(yunet_path())) {                                                 \
      SKIP("yunet not in the cache (run scripts/fetch_models.py): " << yunet_path().string()); \
    }                                                                                         \
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

}  // namespace

TEST_CASE("FaceDetector rejects a bad config before opening anything", "[face][detector]") {
  FaceDetectorConfig config;
  config.score_threshold = 1.5F;
  CHECK_THROWS_AS(FaceDetector::open(shared_env(), "does-not-exist.onnx", config), FaceDetectorError);
  config = {};
  config.nms_iou_threshold = std::nanf("");
  CHECK_THROWS_AS(FaceDetector::open(shared_env(), "does-not-exist.onnx", config), FaceDetectorError);
  config = {};
  config.top_k = 0;
  CHECK_THROWS_AS(FaceDetector::open(shared_env(), "does-not-exist.onnx", config), FaceDetectorError);
  config = {};
  config.tile_cadence = 0;
  CHECK_THROWS_AS(FaceDetector::open(shared_env(), "does-not-exist.onnx", config), FaceDetectorError);
  config.tile_cadence = 9;
  CHECK_THROWS_AS(FaceDetector::open(shared_env(), "does-not-exist.onnx", config), FaceDetectorError);
}

TEST_CASE("FaceDetector reports a missing model and refuses a model that is not YuNet", "[face][detector]") {
  try {
    static_cast<void>(FaceDetector::open(shared_env(), fs::path(SPG_RUNTIME_FIXTURES) / "absent.onnx"));
    FAIL("open() accepted a missing model");
  } catch (const spg::runtime::RuntimeError& error) {
    CHECK(error.code() == spg::runtime::ErrorCode::ModelNotFound);
  }
  SessionOptions options = cpu_only();
  options.warmup_runs = 0;
  CHECK_THROWS_AS(FaceDetector::open(shared_env(), fs::path(SPG_RUNTIME_FIXTURES) / "scale_static.onnx", {}, options),
                  FaceDetectorError);
}

TEST_CASE("FaceDetector opens the pinned YuNet model", "[face][detector][model]") {
  SPG_REQUIRE_YUNET();
  CHECK(detector().input_size() == Size{640, 640});
  CHECK(detector().session().provider() == Provider::Cpu);
  CHECK(detector(true).input_size() == Size{640, 640});
}

TEST_CASE("FaceDetector rejects invalid frames and sources", "[face][detector][model]") {
  SPG_REQUIRE_YUNET();
  const auto frame = spg::test::to_frame(Canvas(64, 48), spg::PixelFormat::BGRA);
  CHECK_THROWS_AS(detector().detect(frame.view, {0, 48}), spg::ContractError);
  spg::FrameView broken = frame.view;
  broken.planes[0] = nullptr;
  CHECK_THROWS_AS(detector().detect(broken, {64, 48}), spg::ContractError);
}

TEST_CASE("FaceDetector finds no face in empty synthetic frames", "[face][detector][model]") {
  SPG_REQUIRE_YUNET();
  const Scene scene = GENERATE(Scene{{1920, 1080}, {960, 540}, spg::PixelFormat::BGRA},
                               Scene{{1280, 720}, {1280, 720}, spg::PixelFormat::NV12},
                               Scene{{641, 479}, {641, 479}, spg::PixelFormat::I420},
                               Scene{{1080, 1920}, {540, 960}, spg::PixelFormat::BGRA});
  const int pattern = GENERATE(range(0, 6));
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
    case 2:
      canvas = Canvas(rb.width, rb.height, {255, 255, 255});
      break;
    case 3:  // vertical bands of a gradient
      for (int i = 0; i < 16; ++i) {
        const auto v = static_cast<std::uint8_t>(i * 16);
        canvas.rect(w * i / 16.0, 0, w * (i + 1) / 16.0, h, {v, static_cast<std::uint8_t>(255 - v), 90});
      }
      break;
    case 4:  // UI-like panels and text-like bars
      canvas = Canvas(rb.width, rb.height, {45, 40, 38});
      canvas.rect(0.05 * w, 0.05 * h, 0.45 * w, 0.9 * h, {70, 64, 60});
      for (int line = 0; line < 12; ++line) {
        const double y = 0.1 * h + line * 0.06 * h;
        canvas.rect(0.08 * w, y, (0.2 + 0.02 * (line % 5)) * w, y + 0.02 * h, {220, 220, 220});
      }
      canvas.rect(0.55 * w, 0.1 * h, 0.95 * w, 0.4 * h, {200, 120, 40});
      break;
    default:  // plain geometric shapes
      canvas = Canvas(rb.width, rb.height, {30, 90, 30});
      canvas.ellipse(0.25 * w, 0.5 * h, 0.1 * w, 0.1 * w, {0, 200, 255});
      canvas.rect(0.5 * w, 0.2 * h, 0.7 * w, 0.8 * h, {200, 50, 50});
      canvas.ellipse(0.85 * w, 0.3 * h, 0.05 * w, 0.15 * h, {255, 255, 255});
      break;
  }
  CAPTURE(describe(scene), pattern);
  const auto frame = spg::test::to_frame(canvas, scene.format);
  CHECK(detector().detect(frame.view, scene.source).empty());
}

TEST_CASE("FaceDetector finds a drawn face and reports it in Source space", "[face][detector][model]") {
  SPG_REQUIRE_YUNET();
  struct Placed {
    Scene scene;
    double cx;  // face center and size, Source pixels
    double cy;
    double size;
  };
  const Placed placed = GENERATE(Placed{{{1920, 1080}, {960, 540}, spg::PixelFormat::BGRA}, 600, 500, 220},
                                 Placed{{{1920, 1080}, {960, 540}, spg::PixelFormat::NV12}, 1500, 300, 260},
                                 Placed{{{1920, 1080}, {1920, 1080}, spg::PixelFormat::I420}, 900, 700, 240},
                                 Placed{{{1280, 720}, {1280, 720}, spg::PixelFormat::BGRA}, 300, 360, 160},
                                 Placed{{{1080, 1920}, {540, 960}, spg::PixelFormat::BGRA}, 540, 1400, 300},
                                 // Readback squashes 16:9 to 4:3.
                                 Placed{{{1920, 1080}, {800, 600}, spg::PixelFormat::BGRA}, 1000, 540, 260});
  const Scene& scene = placed.scene;
  const bool accelerated = GENERATE(false, true);
  CAPTURE(describe(scene), placed.cx, placed.cy, placed.size, accelerated);

  // Draw in readback pixels; non-uniform readback scaling squashes the face too.
  const double sx = static_cast<double>(scene.readback.width) / scene.source.width;
  const double sy = static_cast<double>(scene.readback.height) / scene.source.height;
  Canvas canvas(scene.readback.width, scene.readback.height, {200, 200, 200});
  if (sx == sy) {
    canvas.face(placed.cx * sx, placed.cy * sy, placed.size * sx);
  } else {
    // Draw unsquashed into a source-sized canvas, then point-sample it down.
    Canvas full(scene.source.width, scene.source.height, {200, 200, 200});
    full.face(placed.cx, placed.cy, placed.size);
    for (std::uint32_t y = 0; y < canvas.height(); ++y) {
      for (std::uint32_t x = 0; x < canvas.width(); ++x) {
        canvas.set(x, y,
                   full.at(static_cast<std::uint32_t>((x + 0.5) / sx), static_cast<std::uint32_t>((y + 0.5) / sy)));
      }
    }
  }
  const auto frame = spg::test::to_frame(canvas, scene.format);
  const auto found = detector(accelerated).detect(frame.view, scene.source);

  REQUIRE(found.size() == 1);
  const spg::Detection& face = found[0];
  CHECK(face.box.space == spg::Space::Source);
  CHECK(face.kind == spg::DetectionKind::Face);
  CHECK(face.detector == spg::DetectorId::YuNet);
  CHECK(face.score >= detector().config().score_threshold);
  CHECK(face.score <= 1.0F);
  const spg::Rect& r = face.box.rect;
  CAPTURE(r.x, r.y, r.width, r.height, face.score);
  // The drawn skin oval spans 1.0 x 1.3 face sizes; YuNet's box hugs it.
  const double s = placed.size;
  CHECK(std::abs(r.x + r.width / 2.0 - placed.cx) < 0.15 * s);
  CHECK(std::abs(r.y + r.height / 2.0 - placed.cy) < 0.15 * s);
  CHECK(r.width > 0.6 * s);
  CHECK(r.width < 1.6 * s);
  CHECK(r.height > 0.8 * s);
  CHECK(r.height < 2.0 * s);
  // Both eyes are inside the box.
  CHECK(r.x < placed.cx - 0.3 * s);
  CHECK(r.x + r.width > placed.cx + 0.3 * s);
  CHECK(r.y < placed.cy - 0.18 * s);
}

TEST_CASE("FaceDetector finds several faces and keeps them apart", "[face][detector][model]") {
  SPG_REQUIRE_YUNET();
  const Size source{1920, 1080};
  Canvas canvas(960, 540, {180, 190, 170});
  canvas.face(200, 200, 100);
  canvas.face(480, 300, 120);
  canvas.face(780, 180, 90);
  const auto frame = spg::test::to_frame(canvas, spg::PixelFormat::BGRA);
  const auto found = detector().detect(frame.view, source);
  REQUIRE(found.size() == 3);
  for (std::size_t i = 1; i < found.size(); ++i) {
    CHECK(found[i - 1].score >= found[i].score);
  }
  for (const auto& [cx, cy] : {std::pair{400.0, 400.0}, std::pair{960.0, 600.0}, std::pair{1560.0, 360.0}}) {
    const bool covered = std::any_of(found.begin(), found.end(), [&](const spg::Detection& d) {
      const spg::Rect& r = d.box.rect;
      return r.x < cx && cx < r.x + r.width && r.y < cy && cy < r.y + r.height;
    });
    CHECK(covered);
  }
}

TEST_CASE("FaceDetector finds small faces in a large frame through scale-1 tiles", "[face][detector][model]") {
  SPG_REQUIRE_YUNET();
  // On the faces suite the whole-frame pass (1920x1080 into 640x640, 1/3)
  // missed faces of 43 and 62 Source px (SPG-12); on these drawn faces it
  // finds 6 of the 18 (none at 40 px). x = 600 lies in the overlap of the
  // first two tile columns, so two tiles see that face whole.
  const Size source{1920, 1080};
  const double size = GENERATE(40.0, 56.0);
  const double cx = GENERATE(300.0, 600.0, 1700.0);
  const double cy = GENERATE(200.0, 560.0, 950.0);
  CAPTURE(size, cx, cy);
  Canvas canvas(source.width, source.height, {200, 200, 200});
  canvas.face(cx, cy, size);
  const auto frame = spg::test::to_frame(canvas, spg::PixelFormat::BGRA);
  static const FaceDetector tiled = open_cpu(every_tile());
  const auto found = tiled.detect(frame.view, source);
  // Exactly one box: the passes that see it agree after suppression.
  REQUIRE(found.size() == 1);
  const spg::Rect& r = found[0].box.rect;
  CAPTURE(r.x, r.y, r.width, r.height, found[0].score);
  CHECK(r.x < cx - 0.3 * size);
  CHECK(r.x + r.width > cx + 0.3 * size);
  CHECK(r.y < cy - 0.18 * size);
  CHECK(r.y + r.height > cy);
}

TEST_CASE("FaceDetector runs every tile once in tile_cadence calls", "[face][detector][model]") {
  SPG_REQUIRE_YUNET();
  // The same small faces as above, which only the tiles find: with the
  // tiles spread over `cadence` calls, a face is found in at least one of
  // any `cadence` consecutive calls.
  const Size source{1920, 1080};
  const std::uint32_t cadence = GENERATE(2U, 3U);
  const double size = GENERATE(40.0, 56.0);
  const double cx = GENERATE(300.0, 600.0, 1700.0);
  const double cy = GENERATE(200.0, 560.0, 950.0);
  CAPTURE(cadence, size, cx, cy);
  Canvas canvas(source.width, source.height, {200, 200, 200});
  canvas.face(cx, cy, size);
  const auto frame = spg::test::to_frame(canvas, spg::PixelFormat::BGRA);
  FaceDetectorConfig config;
  config.tile_cadence = cadence;
  const FaceDetector spread = open_cpu(config);
  std::string seen;  // one character per call: '+' found, '-' not
  for (std::uint32_t call = 0; call < 2 * cadence; ++call) {
    seen += spread.detect(frame.view, source).empty() ? '-' : '+';
  }
  CAPTURE(seen);
  for (std::size_t first = 0; first + cadence <= seen.size(); ++first) {
    CHECK(seen.find('+', first) < first + cadence);
  }
  // Every call keeps the whole-frame pass: a face it sees is found each time.
  Canvas big(source.width, source.height, {200, 200, 200});
  big.face(960, 540, 300);
  const auto big_frame = spg::test::to_frame(big, spg::PixelFormat::BGRA);
  for (std::uint32_t call = 0; call < cadence; ++call) {
    CHECK(spread.detect(big_frame.view, source).size() == 1);
  }
}

TEST_CASE("FaceDetector rejects a tile overlap that is not below the input side", "[face][detector][model]") {
  SPG_REQUIRE_YUNET();
  FaceDetectorConfig config;
  config.tile_overlap = 640;
  CHECK_THROWS_AS(FaceDetector::open(shared_env(), yunet_path(), config, cpu_only()), FaceDetectorError);
  config.tiles = false;  // the overlap only matters with tiles
  CHECK_NOTHROW(FaceDetector::open(shared_env(), yunet_path(), config, cpu_only()));
}

TEST_CASE("FaceDetector gives the same answer from several threads", "[face][detector][model]") {
  SPG_REQUIRE_YUNET();
  Canvas canvas(960, 540, {200, 200, 200});
  canvas.face(480, 270, 120);
  const auto frame = spg::test::to_frame(canvas, spg::PixelFormat::BGRA);
  // Every tile on every call, so each call computes the same answer.
  static const FaceDetector shared = open_cpu(every_tile());
  const auto expected = shared.detect(frame.view, {1920, 1080});
  REQUIRE(expected.size() == 1);
  std::vector<std::vector<spg::Detection>> results(4);
  std::vector<std::thread> threads;
  for (auto& result : results) {
    threads.emplace_back([&] { result = shared.detect(frame.view, {1920, 1080}); });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  for (const auto& result : results) {
    REQUIRE(result.size() == 1);
    CHECK(result[0].box == expected[0].box);
    CHECK(result[0].score == expected[0].score);
  }
}
