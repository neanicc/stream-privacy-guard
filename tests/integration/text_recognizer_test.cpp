// spg::ocr::TextRecognizer end to end with the pinned PP-OCR recognizer, plus
// fixture models built at test time (make_runtime_fixtures.py) for batching
// and malformed output. Cases that need a pinned model skip when
// scripts/fetch_models.py has not cached it; CI runs setup first, so they run
// there. Crops are made-up strings drawn with OpenCV's Hershey fonts
// (synthetic_text.hpp); nothing is real personal data.
//
// Recognized text is compared inside helpers that return bool, so a failing
// check never prints what was read.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "detect/text_detector.hpp"
#include "ocr/recognition_cache.hpp"
#include "ocr/text_recognizer.hpp"
#include "runtime/onnx_session.hpp"
#include "spg/error.hpp"
#include "spg/policy.hpp"
#include "spg/policy_config.hpp"
#include "synthetic_frames.hpp"
#include "synthetic_text.hpp"

namespace {

namespace fs = std::filesystem;
namespace ocr = spg::ocr;
using spg::PixelFormat;
using spg::Rect;
using spg::Size;
using spg::ocr::Recognition;
using spg::ocr::TextRecognizer;
using spg::ocr::TextRecognizerConfig;
using spg::runtime::Environment;
using spg::runtime::Provider;
using spg::runtime::SessionOptions;
using spg::test::Bgr;
using spg::test::Canvas;
using spg::test::OwnedFrame;

Environment& shared_env() {
  static Environment env(Environment::LogLevel::Error);
  return env;
}

fs::path from_utf8(const std::string& path) {
  return fs::path(std::u8string(path.begin(), path.end()));
}

fs::path rec_path() {
  return from_utf8(SPG_PPOCR_REC_MODEL);
}

fs::path det_path() {
  return from_utf8(SPG_PPOCR_DET_MODEL);
}

fs::path fixture(const char* name) {
  return from_utf8(SPG_RUNTIME_FIXTURES) / name;
}

SessionOptions cpu_only() {
  SessionOptions options;
  options.providers = {Provider::Cpu};
  options.cpu_fallback = false;
  return options;
}

// The pinned recognizer on CPU, one per configuration for the whole run.
const TextRecognizer& recognizer() {
  static const TextRecognizer cpu = TextRecognizer::open(shared_env(), rec_path(), {}, cpu_only());
  return cpu;
}

const TextRecognizer& batched_recognizer() {
  static const TextRecognizer batched = [] {
    TextRecognizerConfig config;
    config.max_batch = 6;
    return TextRecognizer::open(shared_env(), rec_path(), config, cpu_only());
  }();
  return batched;
}

// The fixture models emit one step per input column.
TextRecognizerConfig fixture_config() {
  TextRecognizerConfig config;
  config.time_stride = 1;
  return config;
}

TextRecognizer open_fixture(const char* name, TextRecognizerConfig config = fixture_config()) {
  return TextRecognizer::open(shared_env(), fixture(name), config, cpu_only());
}

#define SPG_REQUIRE_MODEL(path)                                                                \
  do {                                                                                         \
    if (!fs::is_regular_file(path)) {                                                          \
      SKIP("model not in the cache (run scripts/fetch_models.py): " << (path).string());       \
    }                                                                                          \
  } while (false)

// A Source-space region around `ink` (Readback pixels), grown by `pad` times
// the ink's shorter side on every side, as the detector's unclip grows a box.
spg::Detection region(const Rect& ink, Size source, Size readback, double pad = 0.3) {
  const double grow = pad * std::min(ink.width, ink.height);
  const Rect padded{static_cast<float>(ink.x - grow), static_cast<float>(ink.y - grow),
                    static_cast<float>(ink.width + 2 * grow), static_cast<float>(ink.height + 2 * grow)};
  const spg::SpaceMap map{spg::ReadbackMapping(source, readback), std::nullopt};
  const spg::Box box = spg::convert({padded, spg::Space::Readback}, spg::Space::Source, map);
  return {box, spg::DetectionKind::TextRegion, 0.9F, spg::DetectorId::Synthetic};
}

Rect unite(const Rect& a, const Rect& b) {
  const float x0 = std::min(a.x, b.x);
  const float y0 = std::min(a.y, b.y);
  return {x0, y0, std::max(a.x + a.width, b.x + b.width) - x0, std::max(a.y + a.height, b.y + b.height) - y0};
}

// Lower case without spaces, so "Maple St" and "maplest" compare equal.
std::string squash(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) == 0) {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return out;
}

std::size_t edit_distance(std::string_view a, std::string_view b) {
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) {
    row[j] = j;
  }
  for (std::size_t i = 1; i <= a.size(); ++i) {
    std::size_t diagonal = row[0];
    row[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t above = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
      diagonal = above;
    }
  }
  return row[b.size()];
}

// Whether some stretch of `read` is within `edits` edits of `want`, ignoring
// case and spaces. Hershey's 'a' reads as 'o' at small sizes; two edits is
// also the policy's default fuzzy budget (FuzzyLimits::max_edits).
bool reads_as(const spg::Secret& read, std::string_view want, std::size_t edits = 2) {
  const std::string text = squash(read.reveal());
  const std::string target = squash(want);
  const std::size_t shortest = target.size() > edits ? target.size() - edits : 1;
  for (std::size_t len = shortest; len <= target.size() + edits; ++len) {
    for (std::size_t start = 0; start + len <= text.size(); ++start) {
      if (edit_distance(std::string_view(text).substr(start, len), target) <= edits) {
        return true;
      }
    }
  }
  return false;
}

bool has_newline(const spg::Secret& text) {
  return text.reveal().find('\n') != std::string_view::npos;
}

struct Line {
  std::string text;
  Rect ink;  // Canvas (Readback) pixels
};

struct Scene {
  Canvas canvas;
  std::vector<Line> lines;
};

// A panel with made-up strings drawn one per line.
Scene panel(Size size, Bgr paper, Bgr ink, const std::vector<std::string>& texts, double height, int x = 20,
            double spacing = 2.2) {
  Scene scene{Canvas(size.width, size.height, paper), {}};
  double baseline = 2.0 * height;
  for (const std::string& text : texts) {
    const Rect box = spg::test::draw_text(scene.canvas, text, x, static_cast<int>(baseline), height, ink);
    scene.lines.push_back({text, box});
    baseline += spacing * height;
  }
  return scene;
}

std::vector<spg::Detection> regions_of(const Scene& scene, Size source) {
  std::vector<spg::Detection> regions;
  const Size readback{scene.canvas.width(), scene.canvas.height()};
  for (const Line& line : scene.lines) {
    regions.push_back(region(line.ink, source, readback));
  }
  return regions;
}

// The canvas turned a quarter turn (counter-clockwise: the right edge becomes
// the top) and where a rect of it lands.
Canvas turn(const Canvas& canvas, bool counter_clockwise) {
  const std::uint32_t w = canvas.width();
  const std::uint32_t h = canvas.height();
  Canvas out(h, w);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      out.set(counter_clockwise ? y : h - 1 - y, counter_clockwise ? w - 1 - x : x, canvas.at(x, y));
    }
  }
  return out;
}

Rect turn(const Rect& r, Size canvas, bool counter_clockwise) {
  return counter_clockwise ? Rect{r.y, static_cast<float>(canvas.width) - r.x - r.width, r.height, r.width}
                           : Rect{static_cast<float>(canvas.height) - r.y - r.height, r.x, r.height, r.width};
}

const std::vector<std::string> kTexts = {"Maple Street 4417", "card 4111 1111 1111 1111", "zorbly quanticor",
                                         "Call me at 555-0142", "hello@example.com", "Room 7B, door code 2958"};

// Redirects std::cout, std::cerr and std::clog into one buffer while alive.
class StreamCapture {
 public:
  StreamCapture()
      : out_(std::cout.rdbuf(buffer_.rdbuf())),
        err_(std::cerr.rdbuf(buffer_.rdbuf())),
        log_(std::clog.rdbuf(buffer_.rdbuf())) {}
  ~StreamCapture() {
    std::cout.rdbuf(out_);
    std::cerr.rdbuf(err_);
    std::clog.rdbuf(log_);
  }
  StreamCapture(const StreamCapture&) = delete;
  StreamCapture& operator=(const StreamCapture&) = delete;
  [[nodiscard]] std::string text() const { return buffer_.str(); }

 private:
  std::ostringstream buffer_;
  std::streambuf* out_;
  std::streambuf* err_;
  std::streambuf* log_;
};

constexpr std::string_view kSentinel = "zq-SENTINEL-7f3a";

// Fragments of the sentinel and of OCR-noisy readings of it.
bool leaks(std::string_view captured) {
  std::string text(captured);
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const std::string_view fragment : {"sentinel", "sentlnel", "zq-", "7f3a", "7f3o"}) {
    if (text.find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

constexpr spg::Timestamp at_ms(std::int64_t ms) {
  return spg::timestamp_from_ns(ms * 1'000'000);
}

}  // namespace

// --- The pinned recognizer ---------------------------------------------------

TEST_CASE("Reads made-up strings on UI-like panels", "[ocr][model]") {
  SPG_REQUIRE_MODEL(rec_path());
  struct Style {
    Bgr paper;
    Bgr ink;
  };
  const Style style = GENERATE(Style{{240, 240, 240}, {30, 30, 30}}, Style{{40, 32, 28}, {235, 235, 235}},
                               Style{{180, 120, 60}, {255, 255, 255}});
  const double height = GENERATE(10.0, 14.0, 22.0);
  const bool half = GENERATE(false, true);
  const PixelFormat format = GENERATE(PixelFormat::BGRA, PixelFormat::NV12, PixelFormat::I420);
  const Size readback{900, 420};
  const Size source = half ? Size{1800, 840} : readback;
  const Scene scene = panel(readback, style.paper, style.ink, kTexts, height);
  const OwnedFrame frame = spg::test::to_frame(scene.canvas, format);
  const std::vector<Recognition> read = recognizer().recognize(frame.view, source, regions_of(scene, source));
  REQUIRE(read.size() == kTexts.size());
  for (std::size_t i = 0; i < read.size(); ++i) {
    INFO("line " << i << ", cap height " << height << ", " << spg::to_string(format) << (half ? ", half" : ""));
    CHECK(reads_as(read[i].text, kTexts[i]));
    CHECK(read[i].score > 0.5F);
    CHECK(read[i].score <= 1.0F);
  }
}

TEST_CASE("Two lines merged into one box are read line by line", "[ocr][model]") {
  // SPG-06's cross-tile merge can join adjacent lines transitively into one
  // box (issue #8). CTC reads one line at a time, so the recognizer splits the
  // crop at the gap between lines and reads each band.
  SPG_REQUIRE_MODEL(rec_path());
  const double height = GENERATE(10.0, 16.0);
  const double spacing = GENERATE(1.6, 2.0);
  const std::vector<std::string> pairs[] = {{"Maple Street 4417", "card 4111 1111 1111 1111"},
                                            {"zorbly quanticor", "Room 7B, door code 2958"}};
  for (const std::vector<std::string>& texts : pairs) {
    const Scene scene = panel({700, 200}, {245, 245, 245}, {20, 20, 20}, texts, height, 20, spacing);
    const OwnedFrame frame = spg::test::to_frame(scene.canvas, PixelFormat::BGRA);
    const spg::Detection merged = region(unite(scene.lines[0].ink, scene.lines[1].ink), {700, 200}, {700, 200});
    const std::vector<Recognition> read =
        recognizer().recognize(frame.view, {700, 200}, std::vector<spg::Detection>{merged});
    REQUIRE(read.size() == 1);
    INFO("cap height " << height << ", spacing " << spacing);
    CHECK(reads_as(read[0].text, texts[0]));
    CHECK(reads_as(read[0].text, texts[1]));
    CHECK(has_newline(read[0].text));
  }
}

TEST_CASE("A long line is read in overlapping chunks", "[ocr][model]") {
  SPG_REQUIRE_MODEL(rec_path());
  // Narrower than the default so this line is chunked; half a chunk still
  // holds the card number (19 characters) at this size.
  TextRecognizerConfig config;
  config.max_width = GENERATE(960U, 1280U);
  const TextRecognizer chunked = TextRecognizer::open(shared_env(), rec_path(), config, cpu_only());
  const std::string text = "shipping note for zorbly quanticor at 1600 maple street, card 4111 1111 1111 1111";
  const Scene scene = panel({1400, 120}, {240, 240, 240}, {30, 30, 30}, {text}, 14.0);
  const OwnedFrame frame = spg::test::to_frame(scene.canvas, PixelFormat::BGRA);
  const std::vector<Recognition> read =
      chunked.recognize(frame.view, {1400, 120}, regions_of(scene, {1400, 120}));
  REQUIRE(read.size() == 1);
  INFO("max_width " << config.max_width);
  CHECK(has_newline(read[0].text) == false);
  CHECK(reads_as(read[0].text, "zorbly quanticor"));
  CHECK(reads_as(read[0].text, "1600 maple street"));
  CHECK(reads_as(read[0].text, "4111 1111 1111 1111"));

  spg::PolicyConfig policy;
  policy.watchlist.emplace_back("Zorbly Quanticor");
  const spg::Verdict verdict = spg::PolicyEngine(policy).evaluate(read[0].text);
  CHECK(verdict.reasons.contains(spg::Category::Watchlist));
  CHECK(verdict.reasons.contains(spg::Category::PiiCard));
  // The default max_width reads this line whole.
  const std::vector<Recognition> whole =
      recognizer().recognize(frame.view, {1400, 120}, regions_of(scene, {1400, 120}));
  CHECK(reads_as(whole[0].text, text, 4));
}

TEST_CASE("Vertical text is read turned either way", "[ocr][model]") {
  SPG_REQUIRE_MODEL(rec_path());
  const bool counter_clockwise = GENERATE(true, false);
  const Scene scene = panel({420, 60}, {240, 240, 240}, {30, 30, 30}, {"zorbly quanticor"}, 16.0);
  const Canvas turned = turn(scene.canvas, counter_clockwise);
  const Rect ink = turn(scene.lines[0].ink, {420, 60}, counter_clockwise);
  const OwnedFrame frame = spg::test::to_frame(turned, PixelFormat::BGRA);
  const Size size{turned.width(), turned.height()};
  const std::vector<Recognition> read =
      recognizer().recognize(frame.view, size, std::vector<spg::Detection>{region(ink, size, size)});
  REQUIRE(read.size() == 1);
  INFO((counter_clockwise ? "turned counter-clockwise" : "turned clockwise"));
  CHECK(reads_as(read[0].text, "zorbly quanticor"));
}

TEST_CASE("A long vertical line is read in chunks in reading order", "[ocr][model]") {
  // A max_width below the turned line's full width forces chunks in both
  // turned views; clockwise lists them bottom to top. Chunks in the wrong
  // order would put the end of the line before its start.
  SPG_REQUIRE_MODEL(rec_path());
  const bool counter_clockwise = GENERATE(true, false);
  TextRecognizerConfig config;
  config.max_width = 480;
  const TextRecognizer chunked = TextRecognizer::open(shared_env(), rec_path(), config, cpu_only());
  const std::string text = "shipping note for zorbly quanticor at 1600 maple street";
  const Scene scene = panel({1000, 60}, {240, 240, 240}, {30, 30, 30}, {text}, 14.0);
  const Canvas turned = turn(scene.canvas, counter_clockwise);
  const Rect ink = turn(scene.lines[0].ink, {1000, 60}, counter_clockwise);
  const OwnedFrame frame = spg::test::to_frame(turned, PixelFormat::BGRA);
  const Size size{turned.width(), turned.height()};
  const spg::Detection line = region(ink, size, size);
  // The turned views really are chunked (the padded box is 48 x ~800 turned).
  const double full_width = std::ceil(static_cast<double>(config.input_height) * line.box.rect.height /
                                      line.box.rect.width);
  REQUIRE(full_width > config.max_width);
  const std::vector<Recognition> read = chunked.recognize(frame.view, size, std::vector<spg::Detection>{line});
  REQUIRE(read.size() == 1);
  INFO((counter_clockwise ? "turned counter-clockwise" : "turned clockwise"));
  // One view (one '\n'-separated line) reads the first word before the last.
  // No chunk holds both, so this fails when the chunks are out of order.
  bool in_order = false;
  std::istringstream views{std::string(read[0].text.reveal())};
  for (std::string view; std::getline(views, view);) {
    const std::string squashed = squash(view);
    const std::size_t start = squashed.find("shipping");
    const std::size_t end = squashed.find("street");
    in_order = in_order || (start != std::string::npos && end != std::string::npos && start < end);
  }
  CHECK(in_order);
}

TEST_CASE("Batched runs read the same strings", "[ocr][model]") {
  SPG_REQUIRE_MODEL(rec_path());
  const Scene scene = panel({900, 420}, {240, 240, 240}, {30, 30, 30}, kTexts, 14.0);
  const OwnedFrame frame = spg::test::to_frame(scene.canvas, PixelFormat::BGRA);
  const std::vector<spg::Detection> regions = regions_of(scene, {900, 420});
  const std::vector<Recognition> batched = batched_recognizer().recognize(frame.view, {900, 420}, regions);
  REQUIRE(batched.size() == kTexts.size());
  for (std::size_t i = 0; i < kTexts.size(); ++i) {
    INFO("line " << i);
    CHECK(reads_as(batched[i].text, kTexts[i]));
  }
}

TEST_CASE("Detector boxes feed the recognizer and the policy flags the secrets", "[ocr][model]") {
  SPG_REQUIRE_MODEL(rec_path());
  SPG_REQUIRE_MODEL(det_path());
  const spg::detect::TextDetector detector =
      spg::detect::TextDetector::open(shared_env(), det_path(), {}, cpu_only());
  const std::vector<std::string> texts = {"chat: zorbly quanticor joined", "Weather today is sunny",
                                          "card 4111 1111 1111 1111"};
  const Scene scene = panel({1280, 720}, {32, 28, 24}, {230, 230, 230}, texts, 18.0, 60, 4.0);
  const OwnedFrame frame = spg::test::to_frame(scene.canvas, PixelFormat::NV12);
  const Size source{2560, 1440};  // readback at half scale
  const std::vector<spg::Detection> found = detector.detect(frame.view, source);
  REQUIRE(found.size() >= texts.size());
  const std::vector<Recognition> read = recognizer().recognize(frame.view, source, found);

  spg::PolicyConfig policy;
  policy.watchlist.emplace_back("Zorbly Quanticor");
  const spg::PolicyEngine engine(policy);
  // Which detection holds each drawn line, and what the policy says about it.
  const spg::SpaceMap map{spg::ReadbackMapping(source, {1280, 720}), std::nullopt};
  std::vector<spg::CategorySet> verdicts(texts.size());
  for (std::size_t d = 0; d < found.size(); ++d) {
    const spg::Verdict verdict = engine.evaluate(read[d].text);
    for (std::size_t i = 0; i < texts.size(); ++i) {
      const Rect ink = spg::convert({scene.lines[i].ink, spg::Space::Readback}, spg::Space::Source, map).rect;
      const Rect overlap = spg::intersection(found[d].box.rect, ink);
      if (overlap.width > 0.0F && overlap.height > 0.0F) {
        for (const spg::Category c : spg::kAllCategories) {
          if (verdict.reasons.contains(c)) {
            verdicts[i].insert(c);
          }
        }
      }
    }
  }
  CHECK(verdicts[0].contains(spg::Category::Watchlist));
  CHECK(verdicts[1].empty());
  CHECK(verdicts[2].contains(spg::Category::PiiCard));
}

TEST_CASE("Regions outside the frame read as empty; bad regions throw", "[ocr][model]") {
  SPG_REQUIRE_MODEL(rec_path());
  const Scene scene = panel({400, 100}, {240, 240, 240}, {30, 30, 30}, {"zorbly"}, 14.0);
  const OwnedFrame frame = spg::test::to_frame(scene.canvas, PixelFormat::BGRA);
  CHECK(recognizer().recognize(frame.view, {400, 100}, {}).empty());
  const spg::Detection outside{{{500.0F, 10.0F, 50.0F, 20.0F}, spg::Space::Source},
                               spg::DetectionKind::TextRegion, 0.5F, spg::DetectorId::Synthetic};
  const std::vector<Recognition> read =
      recognizer().recognize(frame.view, {400, 100}, std::vector<spg::Detection>{outside});
  REQUIRE(read.size() == 1);
  CHECK(read[0].text.empty());
  CHECK(read[0].score == 0.0F);
  spg::Detection readback_box = outside;
  readback_box.box.space = spg::Space::Readback;
  CHECK_THROWS_AS(recognizer().recognize(frame.view, {400, 100}, std::vector<spg::Detection>{readback_box}),
                  spg::ContractError);
  CHECK_THROWS_AS(recognizer().recognize(frame.view, {0, 100}, {}), spg::ContractError);
}

TEST_CASE("Concurrent recognize() calls on one recognizer agree", "[ocr][model]") {
  SPG_REQUIRE_MODEL(rec_path());
  const Scene scene = panel({900, 420}, {240, 240, 240}, {30, 30, 30}, kTexts, 14.0);
  const OwnedFrame frame = spg::test::to_frame(scene.canvas, PixelFormat::I420);
  const std::vector<spg::Detection> regions = regions_of(scene, {900, 420});
  const std::vector<Recognition> expected = recognizer().recognize(frame.view, {900, 420}, regions);
  std::vector<std::vector<Recognition>> results(4);
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < results.size(); ++t) {
    threads.emplace_back([&, t] { results[t] = recognizer().recognize(frame.view, {900, 420}, regions); });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (const std::vector<Recognition>& result : results) {
    REQUIRE(result.size() == expected.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
      CHECK(result[i].text == expected[i].text);
    }
  }
}

TEST_CASE("Tracks are read again only when new, changed or stale", "[ocr][model][cache]") {
  SPG_REQUIRE_MODEL(rec_path());
  const Size size{900, 300};
  const std::vector<std::string> before = {"Maple Street 4417", "card 4111 1111 1111 1111", "zorbly quanticor"};
  std::vector<std::string> after = before;
  after[1] = "card 4111 1111 1111 1112";  // one digit changes
  const Scene first = panel(size, {240, 240, 240}, {30, 30, 30}, before, 14.0);
  const Scene second = panel(size, {240, 240, 240}, {30, 30, 30}, after, 14.0);
  OwnedFrame frame1 = spg::test::to_frame(first.canvas, PixelFormat::BGRA);
  OwnedFrame frame2 = spg::test::to_frame(second.canvas, PixelFormat::BGRA);
  std::vector<ocr::TrackedRegion> tracks;
  for (std::size_t i = 0; i < first.lines.size(); ++i) {
    // The same box on both frames (the detector would return it again).
    tracks.push_back({100 + i, region(unite(first.lines[i].ink, second.lines[i].ink), size, size)});
  }

  ocr::RecognitionCache cache;
  ocr::CacheStats stats;
  frame1.view.timestamp = at_ms(0);
  std::vector<spg::TextHit> hits = ocr::recognize_tracks(recognizer(), cache, frame1.view, size, tracks, &stats);
  CHECK(stats.misses == 3);
  CHECK(stats.hits == 0);
  REQUIRE(hits.size() == 3);
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(reads_as(hits[i].text, before[i]));
    CHECK(hits[i].detection.box == tracks[i].detection.box);
    CHECK_FALSE(hits[i].verdict.sensitive());
  }
  const spg::Secret card_before = hits[1].text;

  frame1.view.timestamp = at_ms(100);  // unchanged: every track hits
  hits = ocr::recognize_tracks(recognizer(), cache, frame1.view, size, tracks, &stats);
  CHECK(stats.hits == 3);
  CHECK(hits[1].text == card_before);

  frame2.view.timestamp = at_ms(200);  // one digit changed: that track is read again
  hits = ocr::recognize_tracks(recognizer(), cache, frame2.view, size, tracks, &stats);
  CHECK(stats.misses == 1);
  CHECK(stats.hits == 2);
  CHECK(reads_as(hits[1].text, "4111 1111 1111 1112", 0));
  CHECK_FALSE(hits[1].text == card_before);

  frame2.view.timestamp = at_ms(1300);  // older than max_age: all read again
  hits = ocr::recognize_tracks(recognizer(), cache, frame2.view, size, tracks, &stats);
  CHECK(stats.misses == 3);

  SECTION("a failed read leaves the cache as it was") {
    const TextRecognizer broken = open_fixture("rec_sqrt.onnx");  // NaN on dark pixels
    const std::size_t entries = cache.size();
    frame1.view.timestamp = at_ms(1400);  // track 101 changed back: a miss
    CHECK_THROWS_AS(ocr::recognize_tracks(broken, cache, frame1.view, size, tracks), ocr::DecodeError);
    CHECK(cache.size() == entries);
    const ocr::Fingerprint print = ocr::fingerprint(frame2.view, size, tracks[1].detection.box);
    CHECK(cache.find(101, print, at_ms(1400)));
  }
}

TEST_CASE("Recognized text never reaches streams, errors or verdicts", "[ocr][model][sentinel]") {
  SPG_REQUIRE_MODEL(rec_path());
  const Size size{600, 120};
  const Scene scene = panel(size, {240, 240, 240}, {30, 30, 30}, {"chat " + std::string(kSentinel) + " joined"},
                            16.0);
  OwnedFrame frame = spg::test::to_frame(scene.canvas, PixelFormat::BGRA);
  const std::vector<ocr::TrackedRegion> tracks{{1, region(scene.lines[0].ink, size, size)}};
  spg::PolicyConfig policy;
  policy.watchlist.emplace_back(std::string(kSentinel));
  const spg::PolicyEngine engine(policy);
  const TextRecognizer broken = open_fixture("rec_sqrt.onnx");
  TextRecognizerConfig stride8 = fixture_config();
  stride8.time_stride = 8;
  const TextRecognizer wrong_steps = open_fixture("rec_softmax.onnx", stride8);

  std::vector<spg::TextHit> hits;
  std::vector<std::string> errors;
  std::string streamed;
  {
    // Catch2 reports through std::cout, so no assertion runs inside this block.
    const StreamCapture streams;
    ocr::RecognitionCache cache;
    hits = ocr::recognize_tracks(recognizer(), cache, frame.view, size, tracks);
    frame.view.timestamp = at_ms(50);
    hits = ocr::recognize_tracks(recognizer(), cache, frame.view, size, tracks);  // from the cache
    for (spg::TextHit& hit : hits) {
      engine.apply(hit);
    }
    for (const TextRecognizer* bad : {&broken, &wrong_steps}) {
      try {
        ocr::RecognitionCache empty;
        (void)ocr::recognize_tracks(*bad, empty, frame.view, size, tracks);
        errors.emplace_back("no error");
      } catch (const std::exception& e) {
        errors.emplace_back(e.what());
      }
    }
    streamed = streams.text();
  }
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].verdict.reasons.contains(spg::Category::Watchlist));  // the text was read
  CHECK_FALSE(leaks(spg::describe(hits[0].verdict)));
  CHECK(streamed.empty());
  REQUIRE(errors.size() == 2);
  for (const std::string& error : errors) {
    INFO("error: " << error);
    CHECK(error.rfind("text recognizer: output", 0) == 0);
    CHECK_FALSE(leaks(error));
  }
  CHECK(leaks(kSentinel));
  CHECK(leaks(hits[0].text.reveal()));  // the check would see a leak
}

// --- Fixture models: batching and malformed output (no pinned model) -----------

TEST_CASE("Batching does not change what is read", "[ocr][fixture]") {
  // rec_softmax reads each input column on its own, so padding a crop to a
  // wider batch adds only padding columns; texts must match exactly.
  const TextRecognizer single = open_fixture("rec_softmax.onnx");
  TextRecognizerConfig config = fixture_config();
  config.max_batch = GENERATE(2U, 4U, 64U);
  const TextRecognizer batched = open_fixture("rec_softmax.onnx", config);
  // Bright ink on a dark panel: the fixture's most probable class in an ink
  // column is an ink pixel's red value, not the blank, so texts are not empty.
  const Scene scene = panel({900, 420}, {60, 60, 60}, {220, 230, 240}, kTexts, 12.0);
  const OwnedFrame frame = spg::test::to_frame(scene.canvas, PixelFormat::BGRA);
  std::vector<spg::Detection> regions = regions_of(scene, {900, 420});
  const Rect& first = scene.lines[0].ink;  // a tall sliver of ink: rotated views
  regions.push_back(region({first.x, first.y, 5.0F, first.height}, {900, 420}, {900, 420}));
  const std::vector<Recognition> one = single.recognize(frame.view, {900, 420}, regions);
  const std::vector<Recognition> many = batched.recognize(frame.view, {900, 420}, regions);
  REQUIRE(one.size() == regions.size());
  REQUIRE(many.size() == regions.size());
  for (std::size_t i = 0; i < regions.size(); ++i) {
    INFO("region " << i << ", max_batch " << config.max_batch);
    CHECK_FALSE(one[i].text.empty());
    CHECK(one[i].text == many[i].text);
  }
}

TEST_CASE("A full cache falls back to reading every track again", "[ocr][fixture][cache]") {
  // Six tracks and room for two: every call still returns a text for every
  // track, equal to an uncached read, and at most two of them come from the
  // cache. A full cache costs model runs, never a skipped region.
  const TextRecognizer reader = open_fixture("rec_softmax.onnx");
  const Scene scene = panel({900, 420}, {60, 60, 60}, {220, 230, 240}, kTexts, 12.0);
  OwnedFrame frame = spg::test::to_frame(scene.canvas, PixelFormat::BGRA);
  const std::vector<spg::Detection> regions = regions_of(scene, {900, 420});
  const std::vector<Recognition> fresh = reader.recognize(frame.view, {900, 420}, regions);
  std::vector<ocr::TrackedRegion> tracks;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    tracks.push_back({i, regions[i]});
  }
  ocr::RecognitionCacheConfig config;
  config.capacity = 2;
  ocr::RecognitionCache cache(config);
  for (int call = 0; call < 4; ++call) {
    frame.view.timestamp = at_ms(10 * call);
    ocr::CacheStats stats;
    const std::vector<spg::TextHit> hits = ocr::recognize_tracks(reader, cache, frame.view, {900, 420}, tracks,
                                                                 &stats);
    INFO("call " << call);
    REQUIRE(hits.size() == regions.size());
    CHECK(stats.hits + stats.misses == regions.size());
    CHECK(stats.hits <= 2);
    CHECK(stats.misses >= regions.size() - 2);
    CHECK(cache.size() <= 2);
    for (std::size_t i = 0; i < hits.size(); ++i) {
      CHECK_FALSE(hits[i].text.empty());
      CHECK(hits[i].text == fresh[i].text);
    }
  }
}

TEST_CASE("Malformed recognizer output fails the frame closed", "[ocr][fixture]") {
  const Scene scene = panel({400, 100}, {240, 240, 240}, {30, 30, 30}, {"zorbly"}, 14.0);
  const OwnedFrame frame = spg::test::to_frame(scene.canvas, PixelFormat::BGRA);
  const std::vector<spg::Detection> regions = regions_of(scene, {400, 100});
  SECTION("NaN in the output") {
    CHECK_THROWS_AS(open_fixture("rec_sqrt.onnx").recognize(frame.view, {400, 100}, regions), ocr::DecodeError);
  }
  SECTION("time steps that do not match the stride") {
    TextRecognizerConfig config = fixture_config();
    config.time_stride = 8;
    CHECK_THROWS_AS(open_fixture("rec_softmax.onnx", config).recognize(frame.view, {400, 100}, regions),
                    ocr::DecodeError);
  }
  SECTION("well-formed output decodes") {
    CHECK_NOTHROW(open_fixture("rec_softmax.onnx").recognize(frame.view, {400, 100}, regions));
  }
}

TEST_CASE("Models that are not CTC recognizers are refused when opened", "[ocr][fixture]") {
  CHECK_THROWS_AS(open_fixture("rec_no_dictionary.onnx"), ocr::TextRecognizerError);
  // ONNX Runtime infers 144 output classes; the dictionary makes 12.
  CHECK_THROWS_AS(open_fixture("rec_wrong_classes.onnx"), ocr::TextRecognizerError);
  // Neither has a dictionary, and their shapes are not a recognizer's.
  SessionOptions options = cpu_only();
  CHECK_THROWS_AS(TextRecognizer(spg::runtime::Session::open(shared_env(), fixture("scale_static.onnx"), options)),
                  ocr::TextRecognizerError);
  options.warmup_shapes = {{1, 3, 48, 320}};
  CHECK_THROWS_AS(TextRecognizer(spg::runtime::Session::open(shared_env(), fixture("text_sqrt.onnx"), options)),
                  ocr::TextRecognizerError);
  TextRecognizerConfig bad = fixture_config();
  bad.max_batch = 0;
  CHECK_THROWS_AS(open_fixture("rec_softmax.onnx", bad), ocr::TextRecognizerError);
}
