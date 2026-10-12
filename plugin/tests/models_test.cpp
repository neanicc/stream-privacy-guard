// plugin.models: the plugin's detection models (SPG-14b) without OBS. Pinned
// file checks (SHA-256, size, search order), fail-closed stages when a model
// is missing or damaged, the loader thread, and, with the pinned models from
// $SPG_CACHE_DIR, that the stages the plugin hands its scheduler mask a drawn
// face and a made-up watch-list entry and leave an ordinary line alone. All
// text is made up; no image is committed.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "models/model_hub.hpp"
#include "models/model_set.hpp"
#include "models/sha256.hpp"
#include "spg/scan.hpp"
#include "synthetic_frames.hpp"
#include "synthetic_text.hpp"

namespace {

namespace fs = std::filesystem;
using spg::plugin::LoadResult;
using spg::plugin::ModelHub;
using spg::plugin::ModelReport;
using spg::plugin::ModelStatus;
using spg::test::Bgr;
using spg::test::Canvas;
using std::chrono::seconds;

std::string sha256_hex(std::string_view text, std::size_t chunk) {
  spg::plugin::Sha256 hash;
  for (std::size_t at = 0; at < text.size(); at += chunk) {
    const std::string_view part = text.substr(at, chunk);
    hash.update(std::span{reinterpret_cast<const std::uint8_t*>(part.data()), part.size()});
  }
  return spg::plugin::to_hex(hash.finish());
}

// A fresh, empty directory under the system temp directory.
fs::path temp_dir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / ("spg-models-test-" + std::string(name));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void write_file(const fs::path& path, std::uint64_t size, char fill) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  const std::string bytes(static_cast<std::size_t>(size), fill);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

const fs::path& cache_models() {
  static const fs::path dir{SPG_TEST_MODELS_DIR};
  return dir;
}

bool models_cached() {
  for (const spg::plugin::PinnedModel& pin : spg::plugin::pinned_models()) {
    if (!fs::is_regular_file(cache_models() / pin.relative)) {
      return false;
    }
  }
  return true;
}

// A 1280x720 readback (= source) with a face on the left and a dark panel of
// made-up lines on the right.
struct Scene {
  Canvas canvas{1280, 720, {200, 200, 200}};
  spg::Rect face;
  std::vector<spg::Rect> lines;
};

constexpr const char* kWatchlistLine = "chat: zorbly quanticor joined";
constexpr const char* kPlainLine = "Weather today is sunny";
constexpr const char* kWatchlistEntry = "Zorbly Quanticor";

Scene make_scene() {
  Scene scene;
  scene.canvas.face(300, 360, 160);
  scene.face = {300.0F - 80.0F, 360.0F - 104.0F, 160.0F, 208.0F};
  scene.canvas.rect(600, 40, 1260, 260, {32, 28, 24});
  int baseline = 100;
  for (const char* text : {kWatchlistLine, kPlainLine}) {
    scene.lines.push_back(spg::test::draw_text(scene.canvas, text, 630, baseline, 18.0, {230, 230, 230}));
    baseline += 80;
  }
  return scene;
}

spg::ScanJob job_for(const Scene& scene, std::shared_ptr<const spg::OwnedFrame> readback) {
  spg::ScanJob job;
  job.seq = 1;
  job.frame = 1;
  job.source = {scene.canvas.width(), scene.canvas.height()};
  job.readback = std::move(readback);
  return job;
}

spg::CategorySet only(spg::Category category) {
  spg::CategorySet set;
  set.insert(category);
  return set;
}

bool overlaps(const spg::Rect& a, const spg::Rect& b) {
  const spg::Rect both = spg::intersection(a, b);
  return both.width > 0.0F && both.height > 0.0F;
}

bool masked(const spg::ScanResult& result, const spg::Rect& ink, spg::DetectionKind kind) {
  for (const spg::Detection& d : result.detections) {
    if (d.kind == kind && d.box.space == spg::Space::Source && overlaps(d.box.rect, ink)) {
      return true;
    }
  }
  return false;
}

// Waits for `done` to become true, up to `timeout`.
bool wait_for(std::mutex& mutex, std::condition_variable& cv, const bool& done, std::chrono::seconds timeout) {
  std::unique_lock lock(mutex);
  return cv.wait_for(lock, timeout, [&] { return done; });
}

}  // namespace

TEST_CASE("SHA-256 matches the FIPS 180-4 examples in any chunking", "[models][sha256]") {
  const std::string abc = "abc";
  const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  const std::string million(1'000'000, 'a');
  for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{63}, std::size_t{64}, std::size_t{1000}}) {
    CAPTURE(chunk);
    CHECK(sha256_hex("", chunk) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex(abc, chunk) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(two_blocks, chunk) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  }
  CHECK(sha256_hex(million, 4096) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  const std::vector<std::byte> bytes = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  CHECK(spg::plugin::sha256_hex(bytes) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(spg::plugin::sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("The pinned models are the manifest's three", "[models]") {
  const auto pins = spg::plugin::pinned_models();
  REQUIRE(pins.size() == 3);
  CHECK(std::string_view{pins[0].name} == "yunet");
  CHECK(std::string_view{pins[1].name} == "ppocr-det-en");
  CHECK(std::string_view{pins[2].name} == "ppocr-rec-en");
  for (const spg::plugin::PinnedModel& pin : pins) {
    CAPTURE(pin.name);
    CHECK(std::string_view{pin.relative}.starts_with(std::string(pin.name) + "/"));
    CHECK(std::string_view{pin.relative}.ends_with(".onnx"));
    CHECK(std::string_view{pin.sha256}.size() == 64);
    CHECK(pin.size > 0);
  }
}

TEST_CASE("Missing models are reported and the stages fail closed", "[models]") {
  const fs::path empty = temp_dir("missing");
  const std::vector<fs::path> dirs = {fs::path{}, empty};
  spg::plugin::ModelFiles paths;
  const ModelReport located = spg::plugin::locate_models(dirs, paths);
  CHECK(located.status == ModelStatus::Missing);
  CHECK(located.model == "yunet");
  CHECK(located.detail.find("yunet/") != std::string::npos);

  const LoadResult loaded = spg::plugin::load_models(dirs, {true});
  CHECK(loaded.models == nullptr);
  CHECK(loaded.report.status == ModelStatus::Missing);

  // What a filter would scan with: the policy engine, no detectors. Every
  // cycle with any category enabled is incomplete, so frames get the fallback.
  spg::PolicyConfig policy;
  policy.watchlist.emplace_back(kWatchlistEntry);
  const spg::ScanStages stages = spg::plugin::make_stages(nullptr, policy);
  CHECK(stages.faces == nullptr);
  CHECK(stages.text == nullptr);
  CHECK(stages.recognizer == nullptr);
  CHECK(stages.policy != nullptr);
  const Scene scene = make_scene();
  const auto frame = spg::test::to_frame(scene.canvas, spg::PixelFormat::BGRA);
  const spg::ScanJob job = job_for(scene, spg::OwnedFrame::copy(frame.view));
  for (const spg::CategorySet categories :
       {spg::CategorySet::all(), only(spg::Category::Face), only(spg::Category::Watchlist)}) {
    spg::ScanLimits limits;
    limits.categories = categories;
    const spg::ScanResult result = spg::run_scan(stages, job, limits);
    CHECK_FALSE(result.complete);
    CHECK(result.failure == spg::ScanFailure::MissingStage);
  }
}

TEST_CASE("Damaged model files are reported and never opened", "[models]") {
  const auto pins = spg::plugin::pinned_models();
  const fs::path first = temp_dir("damaged-first");
  const fs::path second = temp_dir("damaged-second");
  const std::vector<fs::path> dirs = {first, second};
  spg::plugin::ModelFiles paths;

  SECTION("wrong size, found in the second directory") {
    write_file(second / pins[0].relative, pins[0].size - 1, 'x');
    const ModelReport report = spg::plugin::locate_models(dirs, paths);
    CHECK(report.status == ModelStatus::Corrupt);
    CHECK(report.model == "yunet");
    CHECK(report.detail.find("damaged-second") != std::string::npos);
  }
  SECTION("larger than its pin") {
    write_file(first / pins[0].relative, pins[0].size + 4096, 'x');
    const ModelReport report = spg::plugin::locate_models(dirs, paths);
    CHECK(report.status == ModelStatus::Corrupt);
    CHECK(report.model == "yunet");
    CHECK(report.detail.find("more than") != std::string::npos);
    CHECK(paths.faces.bytes.empty());
  }
  SECTION("right size, wrong bytes") {
    write_file(first / pins[0].relative, pins[0].size, '\0');
    const LoadResult loaded = spg::plugin::load_models(dirs, {true});
    CHECK(loaded.models == nullptr);
    CHECK(loaded.report.status == ModelStatus::Corrupt);
    CHECK(loaded.report.model == "yunet");
    CHECK(loaded.report.detail.find("SHA-256") != std::string::npos);
  }
  SECTION("the first directory holding a file wins, even when damaged") {
    if (!models_cached()) {
      SKIP("pinned models not in the cache (run scripts/fetch_models.py)");
    }
    write_file(first / pins[2].relative, pins[2].size, 'x');
    const ModelReport report = spg::plugin::locate_models(std::vector<fs::path>{first, cache_models()}, paths);
    CHECK(report.status == ModelStatus::Corrupt);
    CHECK(report.model == "ppocr-rec-en");
  }
  SECTION("a directory in a model's place") {
    fs::create_directories(first / pins[1].relative);
    if (models_cached()) {
      const ModelReport report = spg::plugin::locate_models(std::vector<fs::path>{first, cache_models()}, paths);
      CHECK(report.status == ModelStatus::Corrupt);
      CHECK(report.model == "ppocr-det-en");
    }
  }
}

TEST_CASE("The verified bytes are kept for opening, so the file is not read again", "[models][model]") {
  if (!models_cached()) {
    SKIP("pinned models not in the cache (run scripts/fetch_models.py)");
  }
  const auto pins = spg::plugin::pinned_models();
  spg::plugin::ModelFiles files;
  const ModelReport report = spg::plugin::locate_models(std::vector<fs::path>{cache_models()}, files);
  REQUIRE(report.status == ModelStatus::Ready);
  const std::array<const spg::plugin::ModelFile*, 3> slots = {&files.faces, &files.text, &files.recognizer};
  for (std::size_t i = 0; i < pins.size(); ++i) {
    CAPTURE(pins[i].name);
    CHECK(slots[i]->path == cache_models() / pins[i].relative);
    CHECK(slots[i]->bytes.size() == pins[i].size);
    CHECK(spg::plugin::sha256_hex(slots[i]->bytes) == pins[i].sha256);
  }
}

TEST_CASE("ModelHub loads on its own thread and tells every listener once", "[models][hub]") {
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<int> loads{0};
  std::thread::id loader_thread;  // read after shutdown() joins the loader
  ModelHub hub([&] {
    loader_thread = std::this_thread::get_id();
    ++loads;
    released.wait();
    LoadResult result;
    result.report = {ModelStatus::Missing, "yunet", "test"};
    return result;
  });
  CHECK(hub.status() == ModelStatus::Idle);

  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  int calls = 0;
  int dropped_calls = 0;
  std::thread::id listener_thread;
  const std::uint64_t id = hub.subscribe([&] {
    const std::lock_guard lock(mutex);
    ++calls;
    listener_thread = std::this_thread::get_id();
    done = true;
    cv.notify_all();
  });
  const std::uint64_t dropped = hub.subscribe([&] { ++dropped_calls; });
  CHECK(id != dropped);
  hub.unsubscribe(dropped);

  const std::uint64_t before = hub.snapshot().generation;
  hub.start();  // returns while the loader is blocked
  hub.start();
  CHECK(hub.status() == ModelStatus::Loading);
  CHECK(hub.snapshot().generation != before);
  CHECK(hub.snapshot().models == nullptr);
  release.set_value();
  REQUIRE(wait_for(mutex, cv, done, seconds{30}));

  const ModelHub::Snapshot snapshot = hub.snapshot();
  CHECK(snapshot.report.status == ModelStatus::Missing);
  CHECK(snapshot.report.model == "yunet");
  CHECK(snapshot.models == nullptr);
  hub.shutdown();
  CHECK(loads == 1);
  CHECK(calls == 1);
  CHECK(dropped_calls == 0);
  CHECK(listener_thread == loader_thread);
  CHECK(listener_thread != std::this_thread::get_id());
  hub.start();  // after shutdown: nothing
  CHECK(loads == 1);
}

TEST_CASE("ModelHub never reports Ready without models, and survives a throwing loader", "[models][hub]") {
  const bool throws = GENERATE(false, true);
  CAPTURE(throws);
  ModelHub hub([throws]() -> LoadResult {
    if (throws) {
      throw std::runtime_error("loader failed");
    }
    LoadResult result;
    result.report.status = ModelStatus::Ready;  // but no models
    return result;
  });
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  const std::uint64_t id = hub.subscribe([&] {
    const std::lock_guard lock(mutex);
    done = true;
    cv.notify_all();
  });
  hub.start();
  REQUIRE(wait_for(mutex, cv, done, seconds{30}));
  CHECK(hub.status() == ModelStatus::Failed);
  CHECK(hub.snapshot().models == nullptr);
  hub.unsubscribe(id);
}

TEST_CASE("ModelHub shutdown waits for a load in progress", "[models][hub]") {
  std::atomic<bool> finished{false};
  std::promise<void> entered;
  ModelHub hub([&] {
    entered.set_value();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    finished = true;
    return LoadResult{};
  });
  hub.start();
  entered.get_future().wait();
  hub.shutdown();
  CHECK(finished);
  hub.shutdown();  // idempotent
}

TEST_CASE("The pinned models load off the caller's thread and the stages mask a face and a watch-list entry",
          "[models][model]") {
  if (!models_cached()) {
    SKIP("pinned models not in the cache (run scripts/fetch_models.py): " << cache_models().string());
  }
  const std::vector<fs::path> dirs = {temp_dir("packaged-empty"), cache_models()};
  ModelHub hub([&] { return spg::plugin::load_models(dirs, {true}); });
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  std::thread::id listener_thread;
  const std::uint64_t id = hub.subscribe([&] {
    const std::lock_guard lock(mutex);
    listener_thread = std::this_thread::get_id();
    done = true;
    cv.notify_all();
  });
  hub.start();
  REQUIRE(wait_for(mutex, cv, done, seconds{240}));
  hub.unsubscribe(id);
  CHECK(listener_thread != std::this_thread::get_id());
  const ModelHub::Snapshot loaded = hub.snapshot();
  INFO(loaded.report.detail);
  REQUIRE(loaded.report.status == ModelStatus::Ready);
  REQUIRE(loaded.models != nullptr);
  CHECK(loaded.report.detail == "faces=cpu, text=cpu, recognizer=cpu");

  const Scene scene = make_scene();
  REQUIRE(scene.lines.size() == 2);
  const auto frame = spg::test::to_frame(scene.canvas, spg::PixelFormat::BGRA);
  const spg::ScanJob job = job_for(scene, spg::OwnedFrame::copy(frame.view));

  spg::PolicyConfig policy;
  policy.watchlist.emplace_back(kWatchlistEntry);
  const spg::ScanStages stages = spg::plugin::make_stages(loaded.models, policy);
  REQUIRE(stages.faces != nullptr);
  REQUIRE(stages.text != nullptr);
  REQUIRE(stages.recognizer != nullptr);
  REQUIRE(stages.policy != nullptr);
  const spg::ScanResult result = spg::run_scan(stages, job, spg::ScanLimits{});
  REQUIRE(result.complete);
  CHECK(result.failure == spg::ScanFailure::None);
  CHECK(masked(result, scene.face, spg::DetectionKind::Face));
  CHECK(masked(result, scene.lines[0], spg::DetectionKind::TextRegion));
  CHECK_FALSE(masked(result, scene.lines[1], spg::DetectionKind::TextRegion));

  // The policy comes from the filter's PolicyConfig: without the entry the
  // same line is read and left alone.
  const spg::ScanStages no_entry = spg::plugin::make_stages(loaded.models, spg::PolicyConfig{});
  const spg::ScanResult plain = spg::run_scan(no_entry, job, spg::ScanLimits{});
  REQUIRE(plain.complete);
  CHECK(masked(plain, scene.face, spg::DetectionKind::Face));
  CHECK_FALSE(masked(plain, scene.lines[0], spg::DetectionKind::TextRegion));

  // Stages keep the models alive after the hub lets go (module unload while
  // a scheduler still holds them).
  hub.shutdown();
  CHECK(hub.snapshot().models == nullptr);
  const spg::ScanResult after = spg::run_scan(stages, job, spg::ScanLimits{});
  CHECK(after.complete);
  CHECK(masked(after, scene.lines[0], spg::DetectionKind::TextRegion));
}
