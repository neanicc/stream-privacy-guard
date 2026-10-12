// Scheduler (SPG-10) with real worker threads. Frame time comes from an
// AtomicClock the test moves; real time is used only to wait, with generous
// timeouts, for workers to reach a state, so outcomes do not depend on
// thread timing. A render-facing call that waited on inference would hang
// these tests, so each such call runs under a real-time timeout.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mask/mask_rects.hpp"
#include "schedule_fakes.hpp"
#include "spg/error.hpp"
#include "spg/frame.hpp"
#include "spg/scan.hpp"
#include "spg/scheduler.hpp"

namespace {

using namespace std::chrono_literals;
using spg::Detection;
using spg::FallbackAction;
using spg::Rect;
using spg::ReleasedFrame;
using spg::SchedulerConfig;
using spg::Size;
using spg::Timestamp;
using spg_test::at_ms;

constexpr Size k720{1280, 720};

SchedulerConfig threaded_config() {
  SchedulerConfig config;
  config.lookahead = 100ms;
  config.retro = 100ms;
  config.tracker.hold = 200ms;
  config.workers = 2;
  config.shutdown_timeout = 5s;
  return config;
}

// Waits in real time until `done()` holds; false after `timeout`.
template <class Pred>
bool eventually(Pred done, std::chrono::milliseconds timeout = 10s) {
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (!done()) {
    if (std::chrono::steady_clock::now() > until) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// Runs `f` on another thread and requires it to return within `timeout`:
// the render thread must never wait on inference.
template <class F>
void returns_promptly(F f, std::chrono::milliseconds timeout = 5s) {
  auto done = std::async(std::launch::async, f);
  REQUIRE(done.wait_for(timeout) == std::future_status::ready);
  done.get();
}

bool covered_by(const spg::MaskPlan& plan, const Rect& box) {
  return std::any_of(plan.rects.begin(), plan.rects.end(),
                     [&](const spg::MaskRect& r) { return spg::mask::contains(r.box.rect, box); });
}

}  // namespace

TEST_CASE("workers scan pushed frames before their deadline", "[schedule][threads]") {
  const Rect box{40, 50, 60, 30};
  spg_test::AtomicClock clock(at_ms(1000));
  const auto faces = std::make_shared<spg_test::ScriptedDetector>([box](Timestamp, Size) {
    return std::vector<Detection>{spg_test::face_at(box.x, box.y, box.width, box.height)};
  });
  spg::Scheduler scheduler(clock, threaded_config(), spg::ScanStages{faces, nullptr, nullptr, nullptr});
  std::vector<ReleasedFrame> out;
  for (std::int64_t t = 1000; t < 1300; t += 10) {
    clock.set(at_ms(t));
    const spg_test::TinyFrame frame(at_ms(t));
    const std::uint64_t before = scheduler.stats().scans_completed;
    REQUIRE(scheduler.wants_scan(at_ms(t)));
    scheduler.push(at_ms(t), k720, &frame.view());
    REQUIRE(eventually([&] { return scheduler.stats().scans_completed > before; }));
    scheduler.release_due(out);
  }
  clock.set(at_ms(1400));
  scheduler.release_due(out);
  REQUIRE(out.size() == 30);
  for (const ReleasedFrame& r : out) {
    CHECK(r.action == FallbackAction::None);
    CHECK(covered_by(r.plan, box));
  }
  const spg::SchedulerStats stats = scheduler.stats();
  CHECK(stats.scans_completed == 30);
  CHECK(stats.frames_failed_closed == 0);
}

TEST_CASE("a stalled detector never blocks the render thread and its frames fail closed",
          "[schedule][threads]") {
  spg_test::AtomicClock clock(at_ms(1000));
  const auto gate = std::make_shared<spg_test::Gate>();
  const auto stalled = std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{}; }, nullptr, gate);
  SchedulerConfig config = threaded_config();
  config.workers = 1;
  config.shutdown_timeout = 50ms;
  auto scheduler =
      std::make_unique<spg::Scheduler>(clock, config, spg::ScanStages{stalled, nullptr, nullptr, nullptr});

  const spg_test::TinyFrame first(at_ms(1000));
  scheduler->push(at_ms(1000), k720, &first.view());
  REQUIRE(gate->wait_for_waiter(10s));  // the only worker is now stuck in detect()

  std::vector<ReleasedFrame> out;
  returns_promptly([&] {
    for (std::int64_t t = 1010; t <= 1500; t += 10) {
      clock.set(at_ms(t));
      const spg_test::TinyFrame frame(at_ms(t));
      (void)scheduler->wants_scan(at_ms(t));
      scheduler->push(at_ms(t), k720, &frame.view());
      scheduler->release_due(out);
      (void)scheduler->stats();
    }
  });
  REQUIRE(out.size() == 41);  // frames 1000 .. 1400 are due by 1500
  for (const ReleasedFrame& r : out) {
    CHECK(r.plan.fallback);
    CHECK(r.action == FallbackAction::BlurSource);
  }
  CHECK(scheduler->stats().scans_dropped > 0);  // the queue stayed bounded

  // Shutdown does not wait forever on the stalled stage.
  const auto start = std::chrono::steady_clock::now();
  scheduler.reset();
  CHECK(std::chrono::steady_clock::now() - start < 3s);
  // The detached worker owns the stages (and through them the gate), so it
  // returns and exits on its own; it never touches the clock again.
  gate->open();
}

TEST_CASE("a throwing stage fails its frame closed and the worker keeps serving", "[schedule][threads]") {
  spg_test::AtomicClock clock(at_ms(1000));
  const Rect box{5, 5, 10, 10};
  const auto flaky = std::make_shared<spg_test::ScriptedDetector>(
      [box](Timestamp, Size) {
        return std::vector<Detection>{spg_test::face_at(box.x, box.y, box.width, box.height)};
      },
      [](Timestamp t) {
        if (t == at_ms(1000)) {
          throw std::runtime_error("InferenceFailed");
        }
        if (t == at_ms(1200)) {
          throw spg_test::NotAStdException{3};
        }
      });
  SchedulerConfig config = threaded_config();
  config.retro = 0ms;  // each frame needs its own cycle
  config.workers = 1;
  spg::Scheduler scheduler(clock, config, spg::ScanStages{flaky, nullptr, nullptr, nullptr});
  std::vector<ReleasedFrame> out;
  for (const std::int64_t t : {1000, 1100, 1200, 1300}) {
    clock.set(at_ms(t));
    const spg_test::TinyFrame frame(at_ms(t));
    const spg::SchedulerStats before = scheduler.stats();
    scheduler.push(at_ms(t), k720, &frame.view());
    REQUIRE(eventually([&] {
      const spg::SchedulerStats now = scheduler.stats();
      return now.scans_completed + now.scans_failed > before.scans_completed + before.scans_failed;
    }));
  }
  clock.set(at_ms(1400));
  scheduler.release_due(out);
  REQUIRE(out.size() == 4);
  CHECK(out[0].plan.fallback);
  CHECK(out[1].action == FallbackAction::None);
  CHECK(out[2].plan.fallback);
  CHECK(out[3].action == FallbackAction::None);
  CHECK(scheduler.stats().scans_failed == 2);
}

TEST_CASE("a policy that overruns the deadline fails the frame closed", "[schedule][threads]") {
  spg_test::AtomicClock clock(at_ms(1000));
  const auto gate = std::make_shared<spg_test::Gate>();
  const auto regions = std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{spg_test::text_at(0, 0, 40, 10)}; });
  const auto recognizer =
      std::make_shared<spg_test::ScriptedRecognizer>([](const Detection&) { return std::string("SECRET"); });
  const auto slow = std::make_shared<spg_test::ScriptedPolicy>([gate] { gate->wait(); });
  SchedulerConfig config = threaded_config();
  config.workers = 1;
  spg::Scheduler scheduler(clock, config, spg::ScanStages{nullptr, regions, recognizer, slow});
  const spg_test::TinyFrame frame(at_ms(1000));
  scheduler.push(at_ms(1000), k720, &frame.view());
  REQUIRE(gate->wait_for_waiter(10s));
  std::vector<ReleasedFrame> out;
  clock.set(at_ms(1100));
  returns_promptly([&] { scheduler.release_due(out); });
  REQUIRE(out.size() == 1);
  CHECK(out[0].plan.fallback);
  gate->open();
  REQUIRE(eventually([&] { return scheduler.stats().scans_completed == 1; }));
}

TEST_CASE("the scheduler rejects bad configs and sizes", "[schedule][threads]") {
  spg_test::AtomicClock clock(at_ms(1000));
  SchedulerConfig config = threaded_config();
  config.workers = 0;
  CHECK_THROWS_AS(spg::Scheduler(clock, config, spg::ScanStages{}), spg::ContractError);
  spg::Scheduler scheduler(clock, threaded_config(), spg::ScanStages{});
  CHECK_THROWS_AS(scheduler.push(at_ms(1000), Size{0, 720}), spg::ContractError);
  // An invalid readback skips the scan; the frame still buffers and fails closed.
  spg::FrameView broken;
  broken.width = 8;
  broken.height = 8;
  scheduler.push(at_ms(1000), k720, &broken);
  CHECK(scheduler.stats().scans_queued == 0);
  std::vector<ReleasedFrame> out;
  clock.set(at_ms(1100));
  scheduler.release_due(out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].plan.fallback);
}

TEST_CASE("an owned frame copies every plane tightly", "[schedule]") {
  // A 5x3 I420 frame with padded strides.
  std::vector<std::uint8_t> y(8 * 3, 1), u(4 * 2, 2), v(4 * 2, 3);
  for (std::size_t i = 0; i < y.size(); ++i) {
    y[i] = static_cast<std::uint8_t>(i);
  }
  spg::FrameView view;
  view.planes = {y.data(), u.data(), v.data()};
  view.strides = {8, 4, 4};
  view.width = 5;
  view.height = 3;
  view.format = spg::PixelFormat::I420;
  view.timestamp = at_ms(7);
  const auto owned = spg::OwnedFrame::copy(view);
  const spg::FrameView& c = owned->view();
  CHECK(c.width == 5);
  CHECK(c.height == 3);
  CHECK(c.timestamp == at_ms(7));
  CHECK(c.strides[0] == 5);
  CHECK(c.strides[1] == 3);
  CHECK(c.strides[2] == 3);
  CHECK(c.planes[0][5] == 8);  // row 1, column 0 of the source
  CHECK(c.planes[1][0] == 2);
  CHECK(c.planes[2][5] == 3);
  spg::validate(c);
  spg::FrameView bad = view;
  bad.strides[0] = 2;
  CHECK_THROWS_AS(spg::OwnedFrame::copy(bad), spg::ContractError);
}
