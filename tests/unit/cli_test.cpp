// spg-cli (SPG-11) without models: the simulated and realtime drivers with
// scripted stages, and the run, eval and bench commands through run_cli()
// with fake stages on PNG frames written to the build tree. The text
// "SECRET" in the watch-list and the recognizer output is a sentinel that
// must never reach any output.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "commands.hpp"
#include "driver.hpp"
#include "fail_alloc.hpp"
#include "image_io.hpp"
#include "inputs.hpp"
#include "schedule_fakes.hpp"
#include "spg/compose.hpp"
#include "spg/json.hpp"
#include "spg/policy.hpp"
#include "stages.hpp"
#include "timing.hpp"

namespace {

namespace fs = std::filesystem;
using spg::Timestamp;
using spg_test::ScriptedDetector;
using spg_test::ScriptedPolicy;
using spg_test::ScriptedRecognizer;

constexpr std::string_view kSentinel = "SECRET-SENTINEL-QX7";

spg::BgraImage gray_frame(std::uint32_t w, std::uint32_t h, std::uint8_t level) {
  spg::BgraImage image{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4, level)};
  for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
    image.pixels[i] = 255;
  }
  return image;
}

spg::cli::FrameSource memory_source(std::size_t n, std::uint32_t fps = 60, std::uint32_t w = 64,
                                    std::uint32_t h = 48) {
  return spg::cli::FrameSource{n, fps, [w, h](std::size_t i) {
                                 return gray_frame(w, h, static_cast<std::uint8_t>(16 + (i % 200)));
                               }};
}

// Faces only, so the scheduler needs no text stages.
spg::PolicyConfig faces_only() {
  spg::PolicyConfig policy;
  policy.categories = spg::CategorySet{};
  policy.categories.insert(spg::Category::Face);
  return policy;
}

std::int64_t ms_of(Timestamp t) {
  return spg::to_ns(t) / 1'000'000;
}

// A face at (10, 10, 8, 8) from scene time `from_ms` on.
std::shared_ptr<ScriptedDetector> face_from(std::int64_t from_ms) {
  return std::make_shared<ScriptedDetector>([from_ms](Timestamp t, spg::Size) {
    std::vector<spg::Detection> out;
    if (ms_of(t) >= from_ms) {
      out.push_back(spg_test::face_at(10, 10, 8, 8));
    }
    return out;
  });
}

bool covers(const spg::MaskPlan& plan, const spg::Rect& r) {
  return plan.fallback || std::any_of(plan.rects.begin(), plan.rects.end(), [&](const spg::MaskRect& m) {
           const spg::Rect& b = m.box.rect;
           return b.x <= r.x && b.y <= r.y && b.x + b.width >= r.x + r.width && b.y + b.height >= r.y + r.height;
         });
}

fs::path scratch(const std::string& name) {
  const fs::path dir = fs::path(SPG_CLI_TEST_DIR) / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::string slurp(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> lines_of(const fs::path& file) {
  std::istringstream in(slurp(file));
  std::vector<std::string> out;
  for (std::string line; std::getline(in, line);) {
    out.push_back(line);
  }
  return out;
}

// Writes `n` PNG frames of w x h into dir/frames.
void write_frames(const fs::path& dir, std::size_t n, std::uint32_t w, std::uint32_t h) {
  fs::create_directories(dir / "frames");
  for (std::size_t i = 0; i < n; ++i) {
    std::ostringstream name;
    name << (i < 10 ? "00000" : "0000") << i << ".png";
    spg::cli::write_png(dir / "frames" / name.str(), gray_frame(w, h, static_cast<std::uint8_t>(40 + i)));
  }
}

void write_text(const fs::path& file, const std::string& text) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << text;
}

// A two-sequence suite: 12 frames of 64x48 and 10 frames of 96x54.
fs::path write_suite(const std::string& name) {
  const fs::path dir = scratch(name);
  write_frames(dir / "seq-000", 12, 64, 48);
  write_frames(dir / "seq-001", 10, 96, 54);
  write_text(dir / "policy.json", R"({"schema": 1, "watchlist": [")" + std::string(kSentinel) + R"("]})");
  write_text(dir / "suite.json", R"({"schema": 1, "suite": "text", "fps": 60, "policy": "policy.json",
    "sequences": [{"name": "seq-000", "frames": 12, "width": 64, "height": 48},
                  {"name": "seq-001", "frames": 10, "width": 96, "height": 54}]})");
  return dir;
}

// Fake stages for the commands: a face everywhere, one text region read as
// the sentinel, and the real policy engine on the suite's policy.
spg::cli::CliHooks fake_hooks() {
  spg::cli::CliHooks hooks;
  hooks.open = [](const spg::PolicyConfig& policy, const spg::cli::StageOptions&, const fs::path&) {
    spg::cli::OpenedStages opened;
    opened.stages.faces = face_from(0);
    opened.stages.text = std::make_shared<ScriptedDetector>([](Timestamp, spg::Size) {
      return std::vector<spg::Detection>{spg_test::text_at(30, 20, 20, 8)};
    });
    opened.stages.recognizer =
        std::make_shared<ScriptedRecognizer>([](const spg::Detection&) { return std::string(kSentinel); });
    opened.stages.policy =
        std::make_shared<spg::EnginePolicy>(std::make_shared<const spg::PolicyEngine>(policy));
    opened.providers = "fake";
    return opened;
  };
  return hooks;
}

struct Ran {
  int code = 0;
  std::string out;
  std::string err;
};

Ran cli(std::vector<std::string> args, const spg::cli::CliHooks& hooks = fake_hooks()) {
  std::vector<std::string_view> views(args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  Ran r;
  r.code = spg::cli::run_cli(views, out, err, hooks);
  r.out = out.str();
  r.err = err.str();
  return r;
}

void no_sentinel(const std::string& text) {
  REQUIRE(text.find(kSentinel) == std::string::npos);
}

}  // namespace

TEST_CASE("simulated run writes one plan per frame with the suite's timestamps", "[cli]") {
  spg::cli::StageTimes times;
  const spg::cli::FrameSource source = memory_source(40);
  spg::ScanStages stages;
  stages.faces = face_from(0);
  const spg::cli::SequenceResult r =
      spg::cli::run_simulated(source, faces_only(), stages, spg::cli::DriverOptions{}, times);
  REQUIRE(r.plans.size() == 40);
  for (std::size_t i = 0; i < r.plans.size(); ++i) {
    CHECK(r.plans[i].frame_time == spg::cli::frame_time(i, 60));
    // Covered from the first frame, and the last frames do not fail closed:
    // padding keeps the feed going until every real frame is released.
    CHECK_FALSE(r.plans[i].fallback);
    CHECK(covers(r.plans[i], spg::Rect{10, 10, 8, 8}));
  }
  CHECK(r.fail_closed == 0);
  CHECK(r.padding_frames > 0);
  CHECK(r.stats.frames_pushed == 40 + r.padding_frames);
  CHECK(r.stats.frames_released >= 40);
  // Frame times are i * 1e9 / 60 rounded down, as the generators write them.
  CHECK(spg::to_ns(spg::cli::frame_time(1, 60)) == 16'666'666);
  CHECK(spg::to_ns(spg::cli::frame_time(3, 60)) == 50'000'000);
}

TEST_CASE("simulated run is deterministic with a fixed cycle", "[cli]") {
  spg::cli::StageTimes times;
  spg::ScanStages stages;
  stages.faces = face_from(200);
  spg::cli::DriverOptions options;
  options.cycle = std::chrono::milliseconds{70};
  options.workers = 3;
  const auto a = spg::cli::run_simulated(memory_source(50), faces_only(), stages, options, times);
  const auto b = spg::cli::run_simulated(memory_source(50), faces_only(), stages, options, times);
  CHECK(a.plans == b.plans);
  CHECK(a.latencies_ms == b.latencies_ms);
  // Every simulated cycle took exactly the fixed time after its frame was
  // queued, plus at most the wait for a free worker.
  for (const double ms : a.latencies_ms) {
    CHECK(ms >= 70.0);
  }
}

TEST_CASE("simulated run masks a face retroactively from its first frame", "[cli]") {
  spg::cli::StageTimes times;
  spg::ScanStages stages;
  // The face appears at 200 ms (frame 12); with 200 ms cycles the first scan
  // that sees it finishes later, and retro masking must still cover frame 12.
  stages.faces = face_from(200);
  const auto r = spg::cli::run_simulated(memory_source(60), faces_only(), stages, spg::cli::DriverOptions{}, times);
  for (std::size_t i = 12; i < 60; ++i) {
    INFO("frame " << i);
    CHECK(covers(r.plans[i], spg::Rect{10, 10, 8, 8}));
  }
  CHECK(r.fail_closed == 0);
}

TEST_CASE("simulated run fails closed when cycles cannot finish in time", "[cli]") {
  spg::cli::StageTimes times;
  SECTION("a cycle slower than the lookahead and grace") {
    spg::ScanStages stages;
    stages.faces = face_from(0);
    spg::cli::DriverOptions options;
    options.cycle = std::chrono::milliseconds{1200};
    options.workers = 1;
    const auto r = spg::cli::run_simulated(memory_source(30), faces_only(), stages, options, times);
    CHECK(r.fail_closed == 30);
    for (const spg::MaskPlan& p : r.plans) {
      CHECK(p.fallback);
    }
  }
  SECTION("a detector that throws") {
    spg::ScanStages stages;
    stages.faces = std::make_shared<ScriptedDetector>([](Timestamp, spg::Size) { return std::vector<spg::Detection>{}; },
                                                      [](Timestamp) { throw std::runtime_error("model failed"); });
    const auto r = spg::cli::run_simulated(memory_source(20), faces_only(), stages, spg::cli::DriverOptions{}, times);
    CHECK(r.fail_closed == 20);
  }
  SECTION("a missing detector for an enabled category") {
    const auto r = spg::cli::run_simulated(memory_source(20), faces_only(), spg::ScanStages{},
                                           spg::cli::DriverOptions{}, times);
    CHECK(r.fail_closed == 20);
  }
}

TEST_CASE("plans are fitted to the compositor's rect capacity without losing coverage", "[cli]") {
  spg::cli::StageTimes times;
  spg::ScanStages stages;
  std::vector<spg::Detection> many;
  for (int i = 0; i < 150; ++i) {
    many.push_back(spg_test::face_at(static_cast<float>(i % 15) * 40.0F, static_cast<float>(i / 15) * 40.0F, 10, 10));
  }
  stages.faces = std::make_shared<ScriptedDetector>([many](Timestamp, spg::Size) { return many; });
  const auto r = spg::cli::run_simulated(memory_source(20, 60, 640, 400), faces_only(), stages,
                                         spg::cli::DriverOptions{}, times);
  for (const spg::MaskPlan& p : r.plans) {
    REQUIRE(p.rects.size() <= spg::kMaskRectCapacity);
    REQUIRE_FALSE(p.fallback);
    for (const spg::Detection& d : many) {
      REQUIRE(covers(p, d.box.rect));
    }
  }
}

TEST_CASE("the simulated compositor draws every released frame", "[cli]") {
  spg::cli::StageTimes times;
  spg::ScanStages stages;
  stages.faces = face_from(0);
  spg::cli::DriverOptions options;
  options.compose = true;
  std::vector<std::size_t> seen;
  options.on_composed = [&](std::size_t i, const spg::BgraImage& image) {
    seen.push_back(i);
    REQUIRE(image.width == 64);
  };
  const auto r = spg::cli::run_simulated(memory_source(25), faces_only(), stages, options, times);
  REQUIRE(seen.size() == 25);
  for (std::size_t i = 0; i < seen.size(); ++i) {
    CHECK(seen[i] == i);
  }
  CHECK(times.summary().at("compose").count == 25);
  CHECK(r.plans.size() == 25);
}

TEST_CASE("simulated run rejects bad options", "[cli]") {
  spg::cli::StageTimes times;
  spg::cli::DriverOptions options;
  options.readback_scale = 0.0;
  CHECK_THROWS_AS(spg::cli::run_simulated(memory_source(5), faces_only(), {}, options, times), spg::cli::CliError);
  options = {};
  options.workers = 0;
  CHECK_THROWS_AS(spg::cli::run_simulated(memory_source(5), faces_only(), {}, options, times), spg::cli::CliError);
  CHECK_THROWS_AS(spg::cli::run_simulated(memory_source(0), faces_only(), {}, {}, times), spg::cli::CliError);
}

TEST_CASE("realtime run feeds the scheduler on the wall clock", "[cli]") {
  spg::cli::StageTimes times;
  auto cycles = std::make_shared<spg::cli::CycleLog>();
  auto shared_times = std::make_shared<spg::cli::StageTimes>();
  spg::ScanStages raw;
  raw.faces = face_from(0);
  const spg::ScanStages stages = spg::cli::timed(raw, shared_times, cycles);
  const auto r = spg::cli::run_realtime(memory_source(30), faces_only(), stages, spg::cli::DriverOptions{}, times,
                                        *cycles);
  REQUIRE(r.plans.size() == 30);
  for (std::size_t i = 0; i < r.plans.size(); ++i) {
    // Plans carry the sequence's own timestamps, not the wall clock's.
    CHECK(r.plans[i].frame_time == spg::cli::frame_time(i, 60));
  }
  REQUIRE_FALSE(r.latencies_ms.empty());
  for (const double ms : r.latencies_ms) {
    CHECK(ms >= 0.0);
  }
  CHECK(shared_times->summary().at("face_detect").count == r.latencies_ms.size());
  CHECK(r.padding_frames > 0);
}

TEST_CASE("realtime run keeps a stalled frame's due time and fails it closed", "[cli]") {
  // Frame 40 takes 1.5 s to decode. The decoder runs 16 frames ahead, so it
  // starts on frame 40 only after the feed took frame 24, and the feed gets
  // frame 40 at least 1.5 s - 16 periods (about 1.23 s) after it was due. A
  // live source does not wait for the pipeline: the frame keeps its due
  // time, so it arrives past its lookahead, release grace and clock
  // tolerance (500 ms, 500 ms, 1 s) and must fail closed. Stamped on arrival
  // instead, it would be scanned as a fresh frame and the stall would only
  // slow the feed. Only lower bounds on time matter here, so a slow runner
  // cannot make the frame look on time.
  constexpr std::size_t kStalled = 40;
  spg::cli::FrameSource source{60, 60, [](std::size_t i) {
                                 if (i == kStalled) {
                                   std::this_thread::sleep_for(std::chrono::milliseconds{1500});
                                 }
                                 return gray_frame(64, 48, static_cast<std::uint8_t>(16 + i));
                               }};
  spg::cli::StageTimes times;
  auto cycles = std::make_shared<spg::cli::CycleLog>();
  spg::ScanStages raw;
  raw.faces = face_from(0);
  const spg::ScanStages stages = spg::cli::timed(raw, std::make_shared<spg::cli::StageTimes>(), cycles);
  const auto r = spg::cli::run_realtime(source, faces_only(), stages, spg::cli::DriverOptions{}, times, *cycles);
  REQUIRE(r.plans.size() == 60);
  CHECK(r.late_frames >= 1);
  CHECK(r.plans[kStalled].fallback);
  CHECK(r.fail_closed >= 1);
}

TEST_CASE("timed stages end a cycle after its policy calls", "[cli]") {
  auto cycles = std::make_shared<spg::cli::CycleLog>();
  spg::ScanStages raw;
  raw.text = std::make_shared<ScriptedDetector>([](Timestamp, spg::Size) {
    return std::vector<spg::Detection>{spg_test::text_at(4, 4, 20, 8)};
  });
  raw.recognizer = std::make_shared<ScriptedRecognizer>([](const spg::Detection&) { return std::string("x"); });
  raw.policy = std::make_shared<ScriptedPolicy>([] { std::this_thread::sleep_for(std::chrono::milliseconds{50}); });
  const spg::ScanStages stages = spg::cli::timed(raw, std::make_shared<spg::cli::StageTimes>(), cycles);

  const spg::BgraImage frame = gray_frame(64, 48, 100);
  const spg::FrameView view = frame.view(spg::SteadyClock{}.now());
  const std::vector<spg::Detection> regions = stages.text->detect(view, frame.size());
  const std::vector<spg::Secret> texts = stages.recognizer->recognize(view, frame.size(), regions);
  REQUIRE(texts.size() == 1);
  (void)stages.policy->evaluate(texts[0]);

  const std::vector<double> latencies = cycles->latencies_ms();
  REQUIRE(latencies.size() == 1);
  // The 50 ms the policy took is part of the cycle.
  CHECK(latencies[0] >= 50.0);
}

TEST_CASE("a policy call that throws still ends its cycle and counts its time", "[cli]") {
  auto cycles = std::make_shared<spg::cli::CycleLog>();
  auto times = std::make_shared<spg::cli::StageTimes>();
  spg::ScanStages raw;
  raw.text = std::make_shared<ScriptedDetector>([](Timestamp, spg::Size) {
    return std::vector<spg::Detection>{spg_test::text_at(4, 4, 20, 8)};
  });
  raw.recognizer = std::make_shared<ScriptedRecognizer>([](const spg::Detection&) { return std::string("x"); });
  raw.policy = std::make_shared<ScriptedPolicy>([] {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    throw std::runtime_error("policy failed");
  });
  const spg::ScanStages stages = spg::cli::timed(raw, times, cycles);

  const spg::BgraImage frame = gray_frame(64, 48, 100);
  const spg::FrameView view = frame.view(spg::SteadyClock{}.now());
  const std::vector<spg::Detection> regions = stages.text->detect(view, frame.size());
  const std::vector<spg::Secret> texts = stages.recognizer->recognize(view, frame.size(), regions);
  REQUIRE(texts.size() == 1);
  CHECK_THROWS_AS((void)stages.policy->evaluate(texts[0]), std::runtime_error);

  const std::vector<double> latencies = cycles->latencies_ms();
  REQUIRE(latencies.size() == 1);
  CHECK(latencies[0] >= 50.0);
  REQUIRE(times->summary().count("policy") == 1);
  CHECK(times->summary().at("policy").count == 1);
  CHECK(times->summary().at("policy").max_ms >= 50.0);
}

TEST_CASE("a failed allocation while timing a stage reaches the caller as an exception", "[cli]") {
  // Recording a stage's time allocates (a new stage name, a new cycle). A
  // failure there, on a call that returns or on one that throws, must reach
  // the caller, where run_scan() fails the cycle closed; recording from a
  // destructor would end the process instead.
  const spg::BgraImage frame = gray_frame(64, 48, 100);
  for (const bool policy_throws : {false, true}) {
    const auto stages_for_one_cycle = [policy_throws] {
      spg::ScanStages raw;
      raw.text = std::make_shared<ScriptedDetector>([](Timestamp, spg::Size) {
        return std::vector<spg::Detection>{spg_test::text_at(4, 4, 20, 8)};
      });
      raw.recognizer = std::make_shared<ScriptedRecognizer>([](const spg::Detection&) { return std::string("x"); });
      raw.policy = std::make_shared<ScriptedPolicy>([policy_throws] {
        if (policy_throws) {
          throw std::runtime_error("policy failed");
        }
      });
      // Fresh logs each time, so recording into them allocates.
      return spg::cli::timed(raw, std::make_shared<spg::cli::StageTimes>(), std::make_shared<spg::cli::CycleLog>());
    };
    const auto run_cycle = [&](const spg::ScanStages& stages) {
      const spg::FrameView view = frame.view(spg::SteadyClock{}.now());
      const std::vector<spg::Detection> regions = stages.text->detect(view, frame.size());
      for (const spg::Secret& text : stages.recognizer->recognize(view, frame.size(), regions)) {
        (void)stages.policy->evaluate(text);
      }
    };
    // One unfailed cycle first, so one-time setup (the secure heap behind
    // Secret) is done and the sweep sees only a cycle's allocations.
    if (policy_throws) {
      CHECK_THROWS_AS(run_cycle(stages_for_one_cycle()), std::runtime_error);
    } else {
      CHECK_NOTHROW(run_cycle(stages_for_one_cycle()));
    }
    for (std::uint64_t n = 0;; ++n) {
      REQUIRE(n < 10000);
      const spg::ScanStages stages = stages_for_one_cycle();
      bool threw = false;
      bool fired = false;
      {
        const spg_test::FailAllocations fail(n);
        try {
          run_cycle(stages);
        } catch (...) {
          threw = true;
        }
        fired = fail.fired();
      }
      INFO("policy throws " << policy_throws << ", allocation " << n);
      if (!fired) {
        CHECK(threw == policy_throws);
        CHECK(n > 0);
        break;
      }
      CHECK(threw);
    }
  }
}

TEST_CASE("realtime plans are fitted to the compositor's rect capacity after the feed", "[cli]") {
  spg::cli::StageTimes times;
  auto cycles = std::make_shared<spg::cli::CycleLog>();
  std::vector<spg::Detection> many;
  for (int i = 0; i < 150; ++i) {
    many.push_back(spg_test::face_at(static_cast<float>(i % 15) * 40.0F, static_cast<float>(i / 15) * 40.0F, 10, 10));
  }
  spg::ScanStages raw;
  raw.faces = std::make_shared<ScriptedDetector>([many](Timestamp, spg::Size) { return many; });
  const spg::ScanStages stages = spg::cli::timed(raw, std::make_shared<spg::cli::StageTimes>(), cycles);
  const auto r = spg::cli::run_realtime(memory_source(30, 60, 640, 400), faces_only(), stages,
                                        spg::cli::DriverOptions{}, times, *cycles);
  static_assert(150 > spg::kMaskRectCapacity, "a scanned plan must need fitting");
  REQUIRE(r.plans.size() == 30);
  // Whether a frame airs clean or fails closed depends on wall-clock timing
  // (a slow runner fails frames closed), but either way its plan lists the
  // rects of every cycle that reached it, and every plan is fitted. Every
  // cycle saw all 150 faces, so a plan with any rect had all 150 before
  // fitting. A plan with no rect was released before any cycle reached it.
  std::size_t fitted = 0;
  for (const spg::MaskPlan& p : r.plans) {
    REQUIRE(p.rects.size() <= spg::kMaskRectCapacity);
    if (p.rects.empty()) {
      continue;
    }
    ++fitted;
    for (const spg::Detection& d : many) {
      REQUIRE(covers(p, d.box.rect));
    }
  }
  // At least one plan had more raw rects than the capacity and went through
  // fitting. This needs one cycle to finish before the last real frame is
  // released, not any frame to make its deadline.
  REQUIRE(fitted > 0);
}

TEST_CASE("timing summaries use nearest-rank percentiles", "[cli]") {
  std::vector<double> samples;
  for (int i = 1; i <= 100; ++i) {
    samples.push_back(static_cast<double>(101 - i));
  }
  const spg::cli::StageSummary s = spg::cli::summarize(samples);
  CHECK(s.count == 100);
  CHECK(s.p50_ms == 50.0);
  CHECK(s.p95_ms == 95.0);
  CHECK(s.max_ms == 100.0);
  CHECK(s.mean_ms == 50.5);
  CHECK(spg::cli::summarize({}).count == 0);
}

TEST_CASE("PNG frames round-trip and scale by area", "[cli]") {
  const fs::path dir = scratch("png");
  spg::BgraImage image = gray_frame(6, 4, 0);
  for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
    image.pixels[i] = static_cast<std::uint8_t>(i);
    image.pixels[i + 1] = 7;
    image.pixels[i + 2] = 200;
  }
  spg::cli::write_png(dir / "a.png", image);
  const spg::BgraImage back = spg::cli::read_png(dir / "a.png");
  CHECK(back.width == 6);
  CHECK(back.height == 4);
  CHECK(back.pixels == image.pixels);
  CHECK_THROWS_AS(spg::cli::read_png(dir / "missing.png"), spg::cli::CliError);
  write_text(dir / "bad.png", "not a png");
  CHECK_THROWS_AS(spg::cli::read_png(dir / "bad.png"), spg::cli::CliError);

  const spg::BgraImage checker = [] {
    spg::BgraImage c = gray_frame(4, 4, 0);
    for (std::uint32_t y = 0; y < 4; ++y) {
      for (std::uint32_t x = 0; x < 4; ++x) {
        const std::uint8_t v = (x + y) % 2 == 0 ? 200 : 100;
        std::fill_n(&c.pixels[(y * 4 + x) * 4], 3, v);
      }
    }
    return c;
  }();
  const spg::BgraImage half = spg::cli::resize_area(checker, spg::cli::readback_size(checker.size(), 0.5));
  REQUIRE(half.width == 2);
  REQUIRE(half.height == 2);
  for (std::size_t i = 0; i < half.pixels.size(); i += 4) {
    CHECK(half.pixels[i] == 150);
    CHECK(half.pixels[i + 3] == 255);
  }
  CHECK(spg::cli::readback_size({1920, 1080}, 0.5) == spg::Size{960, 540});
  CHECK(spg::cli::readback_size({3, 3}, 0.01) == spg::Size{1, 1});
}

TEST_CASE("eval runs every sequence of a suite and writes plans and timing", "[cli]") {
  const fs::path suite = write_suite("suite-run");
  const fs::path plans = suite / "plans";
  const Ran r = cli({"eval", "--suite", suite.string(), "--plans", plans.string(), "--timing",
                     (plans / "timing.json").string(), "--cycle-ms", "50"});
  INFO(r.err);
  REQUIRE(r.code == 0);
  const std::vector<std::string> a = lines_of(plans / "seq-000" / "mask_plans.jsonl");
  const std::vector<std::string> b = lines_of(plans / "seq-001" / "mask_plans.jsonl");
  REQUIRE(a.size() == 12);
  REQUIRE(b.size() == 10);
  for (std::size_t i = 0; i < a.size(); ++i) {
    const spg::MaskPlan plan = spg::parse_mask_plan(a[i]);
    CHECK(plan.frame_time == spg::cli::frame_time(i, 60));
    CHECK_FALSE(plan.fallback);
    // The face, and the text region the policy flags (the sentinel is on the
    // watch-list).
    CHECK(covers(plan, spg::Rect{10, 10, 8, 8}));
    CHECK(covers(plan, spg::Rect{30, 20, 20, 8}));
  }
  const nlohmann::json timing = nlohmann::json::parse(slurp(plans / "timing.json"));
  CHECK(timing["command"] == "eval");
  CHECK(timing["frames"] == 22);
  CHECK(timing["fail_closed_frames"] == 0);
  CHECK(timing["cycle_ms"] == 50.0);
  CHECK(timing["sequences"].size() == 2);
  for (const char* stage : {"decode", "face_detect", "text_detect", "recognize", "policy", "cycle"}) {
    INFO(stage);
    CHECK(timing["stages"][stage]["count"].get<int>() > 0);
  }
  no_sentinel(r.out);
  no_sentinel(r.err);
  no_sentinel(slurp(plans / "timing.json"));
  no_sentinel(slurp(plans / "seq-000" / "mask_plans.jsonl"));
}

TEST_CASE("run writes one sequence's plans and can write debug frames", "[cli]") {
  const fs::path suite = write_suite("single-run");
  const fs::path out = suite / "out" / "plans.jsonl";
  SECTION("plans only") {
    const Ran r = cli({"run", "--frames", (suite / "seq-001" / "frames").string(), "--policy",
                       (suite / "policy.json").string(), "--out", out.string(), "--fps", "30"});
    INFO(r.err);
    REQUIRE(r.code == 0);
    const std::vector<std::string> lines = lines_of(out);
    REQUIRE(lines.size() == 10);
    CHECK(spg::parse_mask_plan(lines[1]).frame_time == spg::cli::frame_time(1, 30));
    CHECK_FALSE(fs::exists(suite / "debug"));
    no_sentinel(r.out + r.err);
  }
  SECTION("debug frames when asked") {
    const Ran r = cli({"run", "--frames", (suite / "seq-001" / "frames").string(), "--policy",
                       (suite / "policy.json").string(), "--out", out.string(), "--write-frames",
                       (suite / "debug").string()});
    INFO(r.err);
    REQUIRE(r.code == 0);
    CHECK(spg::cli::list_frames(suite / "debug").size() == 10);
    CHECK(r.err.find("debug") != std::string::npos);
  }
}

TEST_CASE("bench reports stage timing and the fail-closed rate", "[cli]") {
  const fs::path suite = write_suite("bench");
  SECTION("simulated feed with the compositor") {
    const Ran r = cli({"bench", "--suite", suite.string(), "--report", (suite / "bench.json").string()});
    INFO(r.err);
    REQUIRE(r.code == 0);
    const nlohmann::json report = nlohmann::json::parse(slurp(suite / "bench.json"));
    CHECK(report["mode"] == "simulated");
    CHECK(report["cycle_ms"].is_null());
    CHECK(report["frames"] == 22);
    CHECK(report["stages"]["compose"]["count"] == 22);
    CHECK(report["fail_closed_frame_rate"].get<double>() >= 0.0);
    CHECK(r.out.find("detection latency") != std::string::npos);
    no_sentinel(r.out + r.err + slurp(suite / "bench.json"));
  }
  SECTION("realtime feed on one resolution, with plans") {
    const Ran r = cli({"bench", "--realtime", "--suite", suite.string(), "--resolution", "96x54", "--plans",
                       (suite / "rt").string(), "--report", (suite / "rt.json").string()});
    INFO(r.err);
    REQUIRE(r.code == 0);
    const nlohmann::json report = nlohmann::json::parse(slurp(suite / "rt.json"));
    CHECK(report["mode"] == "realtime");
    CHECK(report["frames"] == 10);
    CHECK(report["workers"] == spg::SchedulerConfig{}.workers);
    CHECK(report["scan_interval_ms"] == 0.0);
    CHECK(report["recognizer_runs"] == spg::cli::StageOptions{}.recognizer_runs);
    REQUIRE(report["sequences"].size() == 1);
    CHECK(report["sequences"][0]["name"] == "seq-001");
    CHECK(report["latencies_ms"].size() > 0);
    CHECK(lines_of(suite / "rt" / "seq-001" / "mask_plans.jsonl").size() == 10);
    CHECK_FALSE(fs::exists(suite / "rt" / "seq-000"));
  }
  SECTION("scheduling knobs reach the report") {
    const Ran r = cli({"bench", "--realtime", "--suite", suite.string(), "--resolution", "96x54", "--workers", "1",
                       "--scan-interval-ms", "50", "--threads", "2", "--recognizer-runs", "2", "--report",
                       (suite / "knobs.json").string()});
    INFO(r.err);
    REQUIRE(r.code == 0);
    const nlohmann::json report = nlohmann::json::parse(slurp(suite / "knobs.json"));
    CHECK(report["workers"] == 1);
    CHECK(report["scan_interval_ms"] == 50.0);
    CHECK(report["threads"] == 2);
    CHECK(report["recognizer_runs"] == 2);
  }
  SECTION("a resolution no sequence has") {
    const Ran r = cli({"bench", "--suite", suite.string(), "--resolution", "1920x1080"});
    CHECK(r.code == 1);
  }
}

TEST_CASE("usage errors exit 2 and failures exit 1", "[cli]") {
  const fs::path suite = write_suite("usage");
  CHECK(cli({}).code == 2);
  CHECK(cli({"frobnicate"}).code == 2);
  CHECK(cli({"scrub"}).code == 2);
  CHECK(cli({"run"}).code == 2);
  CHECK(cli({"eval"}).code == 2);
  CHECK(cli({"bench"}).code == 2);
  CHECK(cli({"eval", "--suite", suite.string(), "--plans", "x", "--bogus"}).code == 2);
  CHECK(cli({"eval", "--suite", suite.string(), "--plans", "x", "--workers", "0"}).code == 2);
  CHECK(cli({"eval", "--suite", suite.string(), "--plans", "x", "--readback-scale", "1.5"}).code == 2);
  CHECK(cli({"eval", "--suite", suite.string(), "--plans", "x", "--suite", "y"}).code == 2);
  CHECK(cli({"bench", "--suite", suite.string(), "--cycle-ms", "10"}).code == 2);
  CHECK(cli({"bench", "--suite", suite.string(), "--scan-interval-ms", "-1"}).code == 2);
  CHECK(cli({"bench", "--suite", suite.string(), "--scan-interval-ms", "soon"}).code == 2);
  CHECK(cli({"bench", "--suite", suite.string(), "--threads", "0"}).code == 2);
  CHECK(cli({"bench", "--suite", suite.string(), "--recognizer-runs", "0"}).code == 2);
  CHECK(cli({"bench", "--suite", suite.string(), "--recognizer-runs", "17"}).code == 2);
  CHECK(cli({"eval", "--suite", (suite / "nope").string(), "--plans", "x"}).code == 1);
  CHECK(cli({"--version"}).out.rfind("spg-cli ", 0) == 0);
  CHECK(cli({"--help"}).out.find("usage: spg-cli") != std::string::npos);
}

TEST_CASE("suite errors name files and counts, never the watch-list", "[cli]") {
  const fs::path suite = write_suite("bad-suite");
  SECTION("frame count mismatch") {
    fs::remove(suite / "seq-000" / "frames" / "000003.png");
    const Ran r = cli({"eval", "--suite", suite.string(), "--plans", (suite / "p").string()});
    CHECK(r.code == 1);
    CHECK(r.err.find("declares 12") != std::string::npos);
  }
  SECTION("a frame of the wrong size") {
    spg::cli::write_png(suite / "seq-000" / "frames" / "000002.png", gray_frame(10, 10, 1));
    const Ran r = cli({"eval", "--suite", suite.string(), "--plans", (suite / "p").string()});
    CHECK(r.code == 1);
    CHECK(r.err.find("64x48") != std::string::npos);
  }
  SECTION("an invalid policy holding the sentinel") {
    write_text(suite / "policy.json", R"({"schema": 1, "watchlist": [")" + std::string(kSentinel) +
                                          R"("], "lookahead_ms": "x"})");
    const Ran r = cli({"eval", "--suite", suite.string(), "--plans", (suite / "p").string()});
    CHECK(r.code == 1);
    no_sentinel(r.out + r.err);
  }
  SECTION("a policy below a floor writes no plans (SPG-08b)") {
    // A hold or category set that would switch a protection off is refused,
    // never run with that protection missing.
    for (const char* rule : {R"("hold_ms": 0)", R"("hold_ms": 250)", R"("categories": [])", R"("margin_px": 0)"}) {
      write_text(suite / "policy.json",
                 R"({"schema": 1, "watchlist": [")" + std::string(kSentinel) + R"("], )" + rule + "}");
      const fs::path plans = suite / "p-floor";
      const Ran r = cli({"eval", "--suite", suite.string(), "--plans", plans.string()});
      CHECK(r.code == 1);
      CHECK(r.err.find("policy.json") != std::string::npos);
      CHECK_FALSE(fs::exists(plans));
      no_sentinel(r.out + r.err);
    }
  }
}
