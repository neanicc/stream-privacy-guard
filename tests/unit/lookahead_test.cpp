// Scheduler, lookahead and fail-closed release (SPG-10): deterministic
// simulation tests. Time comes from a ManualClock; detection cycles run inline
// through run_scan() with scripted stages, so a test decides exactly when a
// cycle starts, stalls, throws, returns late or never returns.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fail_alloc.hpp"
#include "mask/mask_rects.hpp"
#include "schedule_fakes.hpp"
#include "spg/error.hpp"
#include "spg/lookahead.hpp"
#include "spg/mask.hpp"
#include "spg/scan.hpp"
#include "spg/time.hpp"

namespace {

using namespace std::chrono_literals;
using spg::Detection;
using spg::FallbackAction;
using spg::FallbackMode;
using spg::FallbackReason;
using spg::Lookahead;
using spg::ManualClock;
using spg::Rect;
using spg::ReleasedFrame;
using spg::SchedulerConfig;
using spg::Size;
using spg::Timestamp;
using spg_test::at_ms;
using spg_test::face_at;
using spg_test::text_at;

constexpr Size k720{1280, 720};
constexpr Size k1080{1920, 1080};

SchedulerConfig small_config() {
  SchedulerConfig config;
  config.lookahead = 100ms;
  config.retro = 100ms;
  config.tracker.hold = 200ms;
  config.tracker.margin_px = 2.0F;
  config.tracker.margin_ratio = 0.0F;
  config.queue_capacity = 4;
  return config;
}

bool covered_by(const spg::MaskPlan& plan, const Rect& box) {
  return std::any_of(plan.rects.begin(), plan.rects.end(),
                     [&](const spg::MaskRect& r) { return spg::mask::contains(r.box.rect, box); });
}

// `limits` without the categories whose detector `stages` lacks.
spg::ScanLimits served(spg::ScanLimits limits, const spg::ScanStages& stages) {
  for (const spg::Category c : spg::kAllCategories) {
    const bool face = c == spg::Category::Face;
    if ((face && stages.faces == nullptr) || (!face && stages.text == nullptr)) {
      limits.categories.erase(c);
    }
  }
  return limits;
}

// Drives a Lookahead frame by frame with inline detection cycles. A cycle
// scans only the categories its stages serve (a test that leaves out a
// detector disables its categories); the tests of an enabled category with
// no detector call run_scan() with the configured limits.
struct Sim {
  explicit Sim(SchedulerConfig config, spg::ScanStages s = {}) : la(clock, config), stages(std::move(s)) {}

  // Moves the clock to `t_ms` and pushes a frame stamped with it.
  spg::FrameId push(std::int64_t t_ms, bool scan, Size size = k720) {
    clock.set(at_ms(t_ms));
    std::shared_ptr<const spg::OwnedFrame> readback;
    if (scan) {
      readback = spg::OwnedFrame::copy(spg_test::TinyFrame(at_ms(t_ms)).view());
    }
    return la.push(at_ms(t_ms), size, readback);
  }
  // Takes the newest job and runs it, without completing it.
  std::pair<spg::ScanJob, spg::ScanResult> run() {
    std::optional<spg::ScanJob> job = la.take_job();
    REQUIRE(job.has_value());
    spg::ScanResult result = spg::run_scan(stages, *job, served(la.config().scan, stages));
    return {*job, result};
  }
  // Takes, runs and completes the newest job now.
  spg::ScanResult scan_now() {
    auto [job, result] = run();
    la.complete(job, result);
    return result;
  }
  std::vector<ReleasedFrame> release_at(std::int64_t t_ms) {
    clock.set(at_ms(t_ms));
    std::vector<ReleasedFrame> out;
    la.release_due(out);
    for (const ReleasedFrame& r : out) {
      spg::validate(r.plan);
      CHECK(r.plan.frame_time == r.frame_time);
      CHECK(r.plan.fallback == (r.action != FallbackAction::None));
    }
    return out;
  }

  ManualClock clock{at_ms(1000)};
  Lookahead la;
  spg::ScanStages stages;
};

std::shared_ptr<spg_test::ScriptedDetector> faces_always(Rect box) {
  return std::make_shared<spg_test::ScriptedDetector>(
      [box](Timestamp, Size) {
        return std::vector<Detection>{face_at(box.x, box.y, box.width, box.height)};
      });
}

std::shared_ptr<spg_test::ScriptedDetector> nothing() {
  return std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{}; });
}

}  // namespace

TEST_CASE("a frame scanned before its deadline is released with its masks", "[schedule]") {
  const Rect box{100, 100, 40, 40};
  Sim sim(small_config(), spg::ScanStages{faces_always(box), nullptr, nullptr, nullptr});
  const spg::FrameId id = sim.push(1000, true);
  sim.scan_now();
  CHECK(sim.release_at(1099).empty());  // never before t + lookahead
  const auto out = sim.release_at(1100);
  REQUIRE(out.size() == 1);
  CHECK(out[0].id == id);
  CHECK(out[0].action == FallbackAction::None);
  CHECK(out[0].reason == FallbackReason::None);
  CHECK(covered_by(out[0].plan, box));
  CHECK(sim.la.stats().scans_completed == 1);
}

TEST_CASE("an unscanned frame fails closed with the configured fallback", "[schedule]") {
  for (const auto& [mode, action] : {std::pair{FallbackMode::BlurSource, FallbackAction::BlurSource},
                                    std::pair{FallbackMode::SolidFill, FallbackAction::SolidFill},
                                    std::pair{FallbackMode::HoldLastScanned, FallbackAction::BlurSource}}) {
    SchedulerConfig config = small_config();
    config.fallback = mode;
    Sim sim(config);
    sim.push(1000, false);
    sim.push(1010, false);
    const auto out = sim.release_at(1200);
    REQUIRE(out.size() == 2);
    for (const ReleasedFrame& r : out) {
      CHECK(r.plan.fallback);
      CHECK(r.action == action);
      CHECK(r.reason == FallbackReason::NotScanned);
    }
    CHECK(sim.la.stats().frames_failed_closed == 2);
  }
}

TEST_CASE("a stalled cycle fails its frames closed and a late result still holds", "[schedule]") {
  const Rect box{10, 10, 20, 20};
  Sim sim(small_config(), spg::ScanStages{faces_always(box), nullptr, nullptr, nullptr});
  sim.push(1000, true);
  auto [job, result] = sim.run();  // the worker is now "inside" the cycle
  for (std::int64_t t = 1010; t <= 1100; t += 10) {
    sim.push(t, false);
  }
  // The cycle has not returned by frame 1000's deadline: it fails closed.
  auto out = sim.release_at(1100);
  REQUIRE(out.size() == 1);
  CHECK(out[0].reason == FallbackReason::NotScanned);
  CHECK(out[0].plan.fallback);

  // The late result is not wasted: it holds forward for later frames once a
  // fresh cycle covers them.
  sim.la.complete(job, result);
  sim.push(1110, true);
  sim.stages.faces = nothing();  // the face left; the held detection remains
  sim.scan_now();
  out = sim.release_at(1210);
  REQUIRE(out.size() == 11);  // frames 1010 .. 1110
  for (const ReleasedFrame& r : out) {
    CHECK(r.action == FallbackAction::None);
    CHECK(covered_by(r.plan, box));  // within hold (200 ms) of 1000
  }
}

TEST_CASE("a cycle that returns after the deadline does not rescue released frames", "[schedule]") {
  Sim sim(small_config(), spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.push(1000, true);
  auto [job, result] = sim.run();
  sim.push(1050, false);
  auto out = sim.release_at(1100);  // frame 1000 is due; its cycle is still running
  REQUIRE(out.size() == 1);
  CHECK(out[0].plan.fallback);
  sim.la.complete(job, result);  // returns late
  // Frame 1050 is covered only by cycles at or after 1050.
  out = sim.release_at(1150);
  REQUIRE(out.size() == 1);
  CHECK(out[0].reason == FallbackReason::NotScanned);
  CHECK(sim.la.stats().scans_completed == 1);
}

TEST_CASE("a slow policy is an overrun and fails the frame closed", "[schedule]") {
  Sim sim(small_config());
  auto policy = std::make_shared<spg_test::ScriptedPolicy>([&sim] { sim.clock.advance(150ms); });
  sim.stages = spg::ScanStages{
      nullptr, std::make_shared<spg_test::ScriptedDetector>([](Timestamp, Size) {
        return std::vector<Detection>{text_at(5, 5, 50, 10)};
      }),
      std::make_shared<spg_test::ScriptedRecognizer>([](const Detection&) { return std::string("hello"); }),
      policy};
  sim.push(1000, true);
  auto [job, result] = sim.run();  // the policy takes 150 ms of clock time
  CHECK(result.complete);
  std::vector<ReleasedFrame> out;
  sim.la.release_due(out);  // the render thread releases while the cycle finishes
  REQUIRE(out.size() == 1);
  CHECK(out[0].plan.fallback);
  sim.la.complete(job, result);
  CHECK(policy->calls == 1);
}

TEST_CASE("any exception from any stage fails the cycle", "[schedule]") {
  const auto text_regions = std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{text_at(5, 5, 50, 10)}; });
  const auto recognizer =
      std::make_shared<spg_test::ScriptedRecognizer>([](const Detection&) { return std::string("abc"); });
  const auto policy = std::make_shared<spg_test::ScriptedPolicy>();
  const std::vector<std::function<void(Timestamp)>> throwers = {
      [](Timestamp) { throw std::runtime_error("InferenceFailed: session run"); },
      [](Timestamp) { throw spg::ContractError("decode: output shape"); },
      [](Timestamp) { throw std::bad_alloc(); },
      [](Timestamp) { throw spg_test::NotAStdException{7}; },
  };
  for (const auto& thrower : throwers) {
    const auto bad = std::make_shared<spg_test::ScriptedDetector>(
        [](Timestamp, Size) { return std::vector<Detection>{}; }, thrower);
    {
      Sim sim(small_config(), spg::ScanStages{bad, nullptr, nullptr, nullptr});
      sim.push(1000, true);
      const spg::ScanResult r = sim.scan_now();
      CHECK_FALSE(r.complete);
      CHECK(r.failure == spg::ScanFailure::FaceDetector);
      const auto out = sim.release_at(1100);
      REQUIRE(out.size() == 1);
      CHECK(out[0].plan.fallback);
      CHECK(sim.la.stats().scans_failed == 1);
    }
    {
      Sim sim(small_config(), spg::ScanStages{nullptr, bad, nullptr, nullptr});
      sim.push(1000, true);
      CHECK(sim.scan_now().failure == spg::ScanFailure::TextDetector);
    }
  }
  // Recognizer and policy failures.
  struct ThrowingRecognizer final : spg::TextRecognizer {
    std::vector<spg::Secret> recognize(const spg::FrameView&, Size,
                                       std::span<const Detection>) const override {
      throw std::runtime_error("InferenceFailed");
    }
  };
  {
    Sim sim(small_config(),
            spg::ScanStages{nullptr, text_regions, std::make_shared<ThrowingRecognizer>(), policy});
    sim.push(1000, true);
    CHECK(sim.scan_now().failure == spg::ScanFailure::Recognizer);
  }
  {
    Sim sim(small_config(), spg::ScanStages{nullptr, text_regions, recognizer,
                                            std::make_shared<spg_test::ScriptedPolicy>(
                                                [] { throw spg_test::NotAStdException{1}; })});
    sim.push(1000, true);
    CHECK(sim.scan_now().failure == spg::ScanFailure::Policy);
  }
}

TEST_CASE("invalid stage output fails the cycle", "[schedule]") {
  const auto check_invalid = [](spg::ScanStages stages) {
    Sim sim(small_config(), std::move(stages));
    sim.push(1000, true);
    const spg::ScanResult r = sim.scan_now();
    CHECK_FALSE(r.complete);
    CHECK(r.failure == spg::ScanFailure::InvalidOutput);
    CHECK(sim.release_at(1100).at(0).plan.fallback);
  };
  // A face detector returning a text region, a negative box, a box in the
  // wrong space, a score out of range.
  Detection wrong_space = face_at(1, 1, 5, 5);
  wrong_space.box.space = spg::Space::Readback;
  Detection bad_score = face_at(1, 1, 5, 5);
  bad_score.score = 2.0F;
  for (const Detection& d : {text_at(1, 1, 5, 5), face_at(1, 1, -5, 5), wrong_space, bad_score}) {
    check_invalid(spg::ScanStages{
        std::make_shared<spg_test::ScriptedDetector>(
            [d](Timestamp, Size) { return std::vector<Detection>{d}; }),
        nullptr, nullptr, nullptr});
  }
  // A recognizer that returns the wrong number of texts.
  struct ShortRecognizer final : spg::TextRecognizer {
    std::vector<spg::Secret> recognize(const spg::FrameView&, Size,
                                       std::span<const Detection>) const override {
      return {};
    }
  };
  check_invalid(spg::ScanStages{nullptr,
                                std::make_shared<spg_test::ScriptedDetector>([](Timestamp, Size) {
                                  return std::vector<Detection>{text_at(1, 1, 5, 5)};
                                }),
                                std::make_shared<ShortRecognizer>(),
                                std::make_shared<spg_test::ScriptedPolicy>()});
}

TEST_CASE("an enabled category without its detector fails the cycle closed", "[schedule]") {
  Sim sim(small_config());  // every category enabled
  sim.push(1000, true);
  std::optional<spg::ScanJob> job = sim.la.take_job();
  REQUIRE(job.has_value());
  const spg::ScanLimits& all = sim.la.config().scan;
  const auto text = std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{}; });
  for (const spg::ScanStages& stages : {spg::ScanStages{}, spg::ScanStages{nothing(), nullptr, nullptr, nullptr},
                                        spg::ScanStages{nullptr, text, nullptr, nullptr}}) {
    const spg::ScanResult r = spg::run_scan(stages, *job, all);
    CHECK_FALSE(r.complete);
    CHECK(r.failure == spg::ScanFailure::MissingStage);
    CHECK(r.detections.empty());
  }
  CHECK(text->calls == 0);
  sim.la.complete(*job, spg::run_scan(spg::ScanStages{}, *job, all));
  const auto out = sim.release_at(1100);
  REQUIRE(out.size() == 1);
  CHECK(out[0].reason == FallbackReason::NotScanned);
  CHECK(sim.la.stats().scans_failed == 1);

  // A disabled category may lack its detector.
  spg::ScanLimits faces_only = all;
  faces_only.categories = spg::CategorySet{};
  faces_only.categories.insert(spg::Category::Face);
  CHECK(spg::run_scan(spg::ScanStages{nothing(), nullptr, nullptr, nullptr}, *job, faces_only).complete);
}

TEST_CASE("adapters reject a missing detector or policy engine", "[schedule]") {
  CHECK_THROWS_AS(spg::DetectorAdapter<spg_test::ScriptedDetector>(nullptr), spg::ContractError);
  CHECK_THROWS_AS(spg::EnginePolicy(nullptr), spg::ContractError);
  const spg::DetectorAdapter<spg_test::ScriptedDetector> adapter(nothing());
  const spg_test::TinyFrame frame(at_ms(0));
  CHECK(adapter.detect(frame.view(), k720).empty());
}

TEST_CASE("a lagging readback covers the frame it shows, not the frame pushed with it", "[schedule]") {
  const Rect box{100, 100, 40, 40};
  Sim sim(small_config(), spg::ScanStages{faces_always(box), nullptr, nullptr, nullptr});
  const spg::FrameId shown = sim.push(1000, false);
  // The stage surface mapped while pushing 1010 still holds frame 1000.
  sim.clock.set(at_ms(1010));
  const auto lagging = spg::OwnedFrame::copy(spg_test::TinyFrame(at_ms(1000)).view());
  sim.la.push(at_ms(1010), k720, lagging);
  {
    std::optional<spg::ScanJob> job = sim.la.take_job();
    REQUIRE(job.has_value());
    CHECK(job->frame == shown);
    CHECK(job->frame_time == at_ms(1000));
    sim.la.complete(*job, spg::run_scan(sim.stages, *job, served(sim.la.config().scan, sim.stages)));
  }
  // A readback of a frame that is not buffered (released, or never pushed) is
  // not scanned.
  sim.clock.set(at_ms(1020));
  sim.la.push(at_ms(1020), k720, spg::OwnedFrame::copy(spg_test::TinyFrame(at_ms(1015)).view()));
  CHECK(sim.la.stats().scans_queued == 1);
  const auto out = sim.release_at(1200);
  REQUIRE(out.size() == 3);
  CHECK(out[0].action == FallbackAction::None);
  CHECK(covered_by(out[0].plan, box));
  // The cycle on 1000 is before 1010 and 1020: it does not count as their scan.
  CHECK(out[1].reason == FallbackReason::NotScanned);
  CHECK(out[2].reason == FallbackReason::NotScanned);
}

TEST_CASE("text regions are masked unless recognized text is judged safe", "[schedule]") {
  const Rect safe{0, 0, 50, 10};
  const Rect secret{0, 20, 50, 10};
  const Rect overlong{0, 40, 50, 10};
  const auto regions = std::make_shared<spg_test::ScriptedDetector>([=](Timestamp, Size) {
    return std::vector<Detection>{text_at(safe.x, safe.y, safe.width, safe.height),
                                  text_at(secret.x, secret.y, secret.width, secret.height),
                                  text_at(overlong.x, overlong.y, overlong.width, overlong.height)};
  });
  const auto recognizer = std::make_shared<spg_test::ScriptedRecognizer>([](const Detection& d) {
    if (d.box.rect.y == 20.0F) {
      return std::string("my SECRET name");
    }
    if (d.box.rect.y == 40.0F) {
      return std::string(600, 'a') + "SECRET";  // over the text cap
    }
    return std::string("score 12");
  });
  const auto policy = std::make_shared<spg_test::ScriptedPolicy>();

  SECTION("with a recognizer and policy") {
    Sim sim(small_config(), spg::ScanStages{nullptr, regions, recognizer, policy});
    sim.push(1000, true);
    const spg::ScanResult r = sim.scan_now();
    REQUIRE(r.complete);
    REQUIRE(r.detections.size() == 2);
    CHECK(r.detections[0].box.rect == secret);
    CHECK(r.detections[1].box.rect == overlong);
    CHECK(policy->calls == 2);  // the overlong text never reached the policy
  }
  SECTION("without a recognizer every region is masked") {
    Sim sim(small_config(), spg::ScanStages{nullptr, regions, nullptr, policy});
    sim.push(1000, true);
    CHECK(sim.scan_now().detections.size() == 3);
    CHECK(policy->calls == 0);
  }
  SECTION("without a policy every region is masked") {
    Sim sim(small_config(), spg::ScanStages{nullptr, regions, recognizer, nullptr});
    sim.push(1000, true);
    CHECK(sim.scan_now().detections.size() == 3);
  }
  SECTION("a zero text cap masks every region") {
    SchedulerConfig config = small_config();
    config.scan.max_text_bytes = 0;
    Sim sim(config, spg::ScanStages{nullptr, regions, recognizer, policy});
    sim.push(1000, true);
    CHECK(sim.scan_now().detections.size() == 3);
    CHECK(policy->calls == 0);
  }
}

TEST_CASE("categories gate the stages", "[schedule]") {
  const auto faces = faces_always(Rect{0, 0, 10, 10});
  const auto text = std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{text_at(0, 0, 10, 10)}; });
  SchedulerConfig config = small_config();
  config.scan.categories = spg::CategorySet{};
  config.scan.categories.insert(spg::Category::PiiCard);
  Sim sim(config, spg::ScanStages{faces, text, nullptr, nullptr});
  sim.push(1000, true);
  CHECK(sim.scan_now().detections.size() == 1);
  CHECK(faces->calls == 0);
  CHECK(text->calls == 1);

  config.scan.categories = spg::CategorySet{};
  config.scan.categories.insert(spg::Category::Face);
  Sim faces_only(config, spg::ScanStages{faces, text, nullptr, nullptr});
  faces_only.push(1000, true);
  CHECK(faces_only.scan_now().detections.size() == 1);
  CHECK(text->calls == 1);
}

TEST_CASE("a detection masks earlier buffered frames within the retro window", "[schedule]") {
  // The secret appears at 1250 and is first scanned at 1300. Cycles at 1100
  // and 1200 saw nothing.
  const Rect box{300, 200, 80, 16};
  SchedulerConfig config = small_config();
  config.lookahead = 500ms;
  config.retro = 100ms;
  Sim sim(config, spg::ScanStages{std::make_shared<spg_test::ScriptedDetector>([box](Timestamp t, Size) {
                                    std::vector<Detection> out;
                                    if (t >= at_ms(1250)) {
                                      out.push_back(face_at(box.x, box.y, box.width, box.height));
                                    }
                                    return out;
                                  }),
                                  nullptr, nullptr, nullptr});
  for (std::int64_t t = 1100; t <= 1300; t += 10) {
    sim.push(t, t % 100 == 0);
    if (t % 100 == 0) {
      sim.scan_now();
    }
  }
  const auto out = sim.release_at(1800);
  REQUIRE(out.size() == 21);
  for (const ReleasedFrame& r : out) {
    const std::int64_t t = spg::to_ns(r.frame_time) / 1'000'000;
    CHECK(r.action == FallbackAction::None);
    if (t >= 1200) {
      // In [1300 - retro, 1300]: masked, including 1250 .. 1290 before the
      // detecting frame.
      CHECK(covered_by(r.plan, box));
    } else {
      CHECK_FALSE(covered_by(r.plan, box));  // outside the retro window
    }
  }
}

TEST_CASE("retroactive masks follow a moving track back to the frames before it was found", "[schedule]") {
  // A box slides in from the right at 1 px/ms (a toast or a scrolling line):
  // x = 700 - (t - 1150) from 1150 on. Cycles at 1200 and 1210 find it; the
  // second confirms its track with a velocity. On 1150 the box is 60 px right
  // of where the 1210 cycle saw it, beyond the margin, so a static
  // retroactive rect would leave it uncovered (SPG-12, text suite seq-001).
  const auto box_at = [](std::int64_t t) {
    return Rect{static_cast<float>(700 - (t - 1150)), 200.0F, 80.0F, 16.0F};
  };
  SchedulerConfig config = small_config();
  config.lookahead = 500ms;
  config.retro = 100ms;
  Sim sim(config, spg::ScanStages{std::make_shared<spg_test::ScriptedDetector>([&](Timestamp t, Size) {
                                    const std::int64_t ms = spg::to_ns(t) / 1'000'000;
                                    std::vector<Detection> out;
                                    if (ms >= 1150) {
                                      const Rect b = box_at(ms);
                                      out.push_back(face_at(b.x, b.y, b.width, b.height));
                                    }
                                    return out;
                                  }),
                                  nullptr, nullptr, nullptr});
  for (std::int64_t t = 1110; t <= 1300; t += 10) {
    const bool scan = t == 1200 || t == 1210 || t == 1250 || t == 1300;
    sim.push(t, scan);
    if (scan) {
      sim.scan_now();
    }
  }
  const auto out = sim.release_at(1800);
  REQUIRE(out.size() == 20);
  for (const ReleasedFrame& r : out) {
    const std::int64_t t = spg::to_ns(r.frame_time) / 1'000'000;
    CHECK(r.action == FallbackAction::None);
    if (t >= 1150) {
      INFO("frame " << t);
      CHECK(covered_by(r.plan, box_at(t)));
    }
    if (t >= 1200) {
      continue;
    }
    // The static rect where the 1210 cycle found it is still there too.
    CHECK(covered_by(r.plan, box_at(1210)));
  }
}

TEST_CASE("back-projection with a jittery velocity only adds rects and stays bounded", "[schedule]") {
  // A static box whose detections jitter 12 px left and right each cycle: the
  // track's velocity flips sign every 20 ms (an outlier-ridden estimate).
  // Back-projection may put rects in the wrong place, but every static rect
  // stays, and past max_retro_rects a frame's rects merge into one hull.
  const auto box_at = [](std::int64_t t) {
    return Rect{static_cast<float>((t / 20) % 2 == 0 ? 294 : 306), 200.0F, 80.0F, 16.0F};
  };
  SchedulerConfig config = small_config();
  config.lookahead = 500ms;
  config.retro = 100ms;
  config.max_retro_rects = 6;
  Sim sim(config, spg::ScanStages{std::make_shared<spg_test::ScriptedDetector>([&](Timestamp t, Size) {
                                    const Rect b = box_at(spg::to_ns(t) / 1'000'000);
                                    return std::vector<Detection>{face_at(b.x, b.y, b.width, b.height)};
                                  }),
                                  nullptr, nullptr, nullptr});
  for (std::int64_t t = 1100; t <= 1400; t += 10) {
    const bool scan = t % 20 == 0;
    sim.push(t, scan);
    if (scan) {
      sim.scan_now();
    }
  }
  const auto out = sim.release_at(2000);
  REQUIRE(out.size() == 31);
  for (const ReleasedFrame& r : out) {
    const std::int64_t t = spg::to_ns(r.frame_time) / 1'000'000;
    INFO("frame " << t);
    CHECK(r.action == FallbackAction::None);
    // Retro rects merge past their cap; the tracker adds at most its own cap.
    CHECK(r.plan.rects.size() <= config.max_retro_rects + config.tracker.max_rects_per_track);
    for (std::int64_t ts = t; ts <= t + 100 && ts <= 1400; ts += 10) {
      if (ts % 20 == 0) {
        CHECK(covered_by(r.plan, box_at(ts)));  // where every cycle in reach saw it
      }
    }
  }
}

TEST_CASE("a frame counts as scanned only by a cycle within retro after it", "[schedule]") {
  SchedulerConfig config = small_config();
  config.lookahead = 500ms;
  config.retro = 100ms;
  Sim sim(config, spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.push(1000, true);
  sim.scan_now();
  sim.push(1050, false);  // next cycle is 150 ms later: beyond retro
  sim.push(1200, true);
  sim.scan_now();
  const auto out = sim.release_at(1700);
  REQUIRE(out.size() == 3);
  CHECK(out[0].action == FallbackAction::None);
  CHECK(out[1].reason == FallbackReason::NotScanned);  // a cycle before it does not count
  CHECK(out[2].action == FallbackAction::None);
}

TEST_CASE("a resize with a cycle in flight only grows masks", "[schedule]") {
  const Rect box720{100, 100, 50, 20};
  const Rect box1080{150, 150, 75, 30};  // the same content at 1080p
  SchedulerConfig config = small_config();
  config.lookahead = 500ms;
  config.retro = 500ms;
  config.tracker.hold = 750ms;
  Sim sim(config, spg::ScanStages{std::make_shared<spg_test::ScriptedDetector>([=](Timestamp, Size s) {
                                    const Rect b = s == k720 ? box720 : box1080;
                                    return std::vector<Detection>{face_at(b.x, b.y, b.width, b.height)};
                                  }),
                                  nullptr, nullptr, nullptr});
  sim.push(1000, false, k720);
  sim.push(1100, true, k720);
  auto [job, result] = sim.run();  // in flight at 720p
  sim.push(1110, false, k1080);    // the source is resized
  sim.push(1150, false, k1080);
  sim.la.complete(job, result);  // the 720p result lands after the resize
  sim.push(1200, true, k1080);
  sim.scan_now();
  const auto out = sim.release_at(1800);
  REQUIRE(out.size() == 5);
  // 720p frames: covered by the 720p cycle, masked in 720p pixels.
  CHECK(out[0].action == FallbackAction::None);
  CHECK(covered_by(out[0].plan, box720));
  CHECK(out[1].action == FallbackAction::None);
  CHECK(covered_by(out[1].plan, box720));
  // 1080p frames: the 720p cycle does not count as a scan of new-size content,
  // but its box, scaled, is still masked; the 1080p cycle covers them.
  for (std::size_t i = 2; i < 5; ++i) {
    CHECK(out[i].source == k1080);
    CHECK(out[i].action == FallbackAction::None);
    CHECK(covered_by(out[i].plan, box1080));
  }
}

TEST_CASE("a result that lands after a resize keeps its scaled margin", "[schedule]") {
  // 720p -> 1080p is exactly 1.5x. The 720p mask of {100,100,20,20} with a
  // 4 px margin is {96,96,28,28}; scaled, {144,144,42,42}, ending at 186.
  const Rect box720{100, 100, 20, 20};
  const Rect scaled_mask{144, 144, 42, 42};
  SchedulerConfig config = small_config();
  config.lookahead = 500ms;
  config.retro = 500ms;
  config.tracker.hold = 750ms;
  config.tracker.margin_px = 4.0F;
  config.tracker.margin_ratio = 0.0F;
  Sim sim(config, spg::ScanStages{std::make_shared<spg_test::ScriptedDetector>([=](Timestamp, Size s) {
                                    std::vector<Detection> out;
                                    if (s == k720) {
                                      out.push_back(face_at(box720.x, box720.y, box720.width, box720.height));
                                    }
                                    return out;  // gone at 1080p
                                  }),
                                  nullptr, nullptr, nullptr});
  sim.push(1000, false, k720);
  sim.push(1100, true, k720);
  auto [job, result] = sim.run();  // in flight at 720p
  sim.push(1110, false, k1080);
  sim.la.complete(job, result);  // lands after the resize
  sim.push(1200, true, k1080);
  sim.scan_now();  // an empty 1080p cycle covers 1110 and 1200
  const auto out = sim.release_at(1800);
  REQUIRE(out.size() == 4);
  for (std::size_t i = 2; i < 4; ++i) {
    INFO("frame " << i);
    CHECK(out[i].action == FallbackAction::None);
    CHECK(covered_by(out[i].plan, scaled_mask));
  }
  // The retroactive rects of 720p frames keep the unscaled mask too.
  CHECK(covered_by(out[0].plan, Rect{96, 96, 28, 28}));
}

TEST_CASE("a result that cannot be applied fails closed every frame it could mask", "[schedule]") {
  SchedulerConfig config = small_config();
  config.lookahead = 500ms;  // retro 100 ms, hold 200 ms
  Sim sim(config, spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.push(1000, true);
  sim.scan_now();  // an empty cycle covers 1000
  sim.push(1050, true);
  auto [job, result] = sim.run();
  // A complete result the tracker rejects (it validates every box first):
  // the stand-in for any failure while applying, such as bad_alloc. Its
  // valid box would have masked 1000 retroactively and later frames by hold.
  result.detections = {face_at(10, 10, 20, 20), face_at(10, 10, -5, 5)};
  sim.la.complete(job, result);
  CHECK(sim.la.stats().scans_failed == 1);
  CHECK(sim.la.stats().scans_completed == 1);
  sim.push(1100, true);  // within the hold after 1050
  sim.scan_now();
  sim.push(1300, true);  // past it
  sim.scan_now();
  const auto out = sim.release_at(1900);
  REQUIRE(out.size() == 4);
  CHECK(out[0].reason == FallbackReason::ApplyFailed);  // covered before, now missing a box
  CHECK(out[1].reason == FallbackReason::ApplyFailed);
  CHECK(out[2].reason == FallbackReason::ApplyFailed);
  CHECK(out[3].action == FallbackAction::None);
}

TEST_CASE("a size round trip starts new content that old cycles do not cover", "[schedule]") {
  Sim sim(small_config(), spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.push(1000, false, k720);
  sim.push(1010, false, k1080);
  sim.push(1050, true, k720);  // back to 720p: a third segment
  sim.scan_now();
  const auto out = sim.release_at(1200);
  REQUIRE(out.size() == 3);
  CHECK(out[0].reason == FallbackReason::NotScanned);  // same size, older segment
  CHECK(out[1].reason == FallbackReason::NotScanned);
  CHECK(out[2].action == FallbackAction::None);
}

TEST_CASE("frames whose time base disagrees with the clock fail closed", "[schedule]") {
  Sim sim(small_config(), spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.clock.set(at_ms(1000));
  const auto readback = spg::OwnedFrame::copy(spg_test::TinyFrame(at_ms(1000)).view());
  sim.la.push(at_ms(1000) + 5s, k720, readback);  // a different epoch
  sim.la.push(at_ms(1000) - 5s, k720, readback);
  sim.push(1000, true);
  sim.la.push(at_ms(1000), k720, readback);  // not increasing
  sim.la.push(at_ms(990), k720, readback);
  CHECK(sim.la.stats().bad_timestamps == 4);
  CHECK(sim.la.stats().scans_queued == 1);  // bad frames are never scanned
  sim.scan_now();
  const auto out = sim.release_at(1100);
  REQUIRE(out.size() == 5);
  CHECK(out[0].reason == FallbackReason::BadTimestamp);
  CHECK(out[1].reason == FallbackReason::BadTimestamp);
  CHECK(out[2].action == FallbackAction::None);
  CHECK(out[3].reason == FallbackReason::BadTimestamp);
  CHECK(out[4].reason == FallbackReason::BadTimestamp);
}

TEST_CASE("degenerate configs fail closed", "[schedule]") {
  SECTION("zero lookahead releases every frame at once, unscanned") {
    SchedulerConfig config = small_config();
    config.lookahead = 0ms;
    config.retro = 0ms;
    Sim sim(config, spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
    for (std::int64_t t = 1000; t < 1100; t += 10) {
      sim.push(t, true);
      const auto out = sim.release_at(t);
      REQUIRE(out.size() == 1);
      CHECK(out[0].plan.fallback);
      CHECK(out[0].reason == FallbackReason::NotScanned);
    }
  }
  SECTION("zero retro counts only a cycle on the frame itself") {
    SchedulerConfig config = small_config();
    config.retro = 0ms;
    Sim sim(config, spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
    sim.push(1000, false);
    sim.push(1001, true);
    sim.scan_now();
    const auto out = sim.release_at(1200);
    REQUIRE(out.size() == 2);
    CHECK(out[0].plan.fallback);
    CHECK(out[1].action == FallbackAction::None);
  }
  SECTION("zero hold still masks the detecting frame and earlier ones") {
    SchedulerConfig config = small_config();
    config.tracker.hold = 0ms;
    const Rect box{1, 2, 30, 40};
    Sim sim(config, spg::ScanStages{faces_always(box), nullptr, nullptr, nullptr});
    sim.push(1000, false);
    sim.push(1050, true);
    sim.scan_now();
    const auto out = sim.release_at(1200);
    REQUIRE(out.size() == 2);
    CHECK(covered_by(out[0].plan, box));
    CHECK(covered_by(out[1].plan, box));
  }
  SECTION("negative durations and zero capacities are rejected") {
    ManualClock clock;
    SchedulerConfig config;
    config.lookahead = -1ms;
    CHECK_THROWS_AS(Lookahead(clock, config), spg::ContractError);
    config = SchedulerConfig{};
    config.queue_capacity = 0;
    CHECK_THROWS_AS(Lookahead(clock, config), spg::ContractError);
    config = SchedulerConfig{};
    config.max_buffered_frames = 0;
    CHECK_THROWS_AS(Lookahead(clock, config), spg::ContractError);
    config = SchedulerConfig{};
    config.tracker.hold = -1ms;
    CHECK_THROWS_AS(Lookahead(clock, config), spg::ContractError);
  }
}

TEST_CASE("an overfull buffer releases its oldest frames early, under the same rule", "[schedule]") {
  SchedulerConfig config = small_config();
  config.max_buffered_frames = 3;
  Sim sim(config, spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.push(1000, false);
  sim.push(1010, true);
  sim.scan_now();
  sim.push(1020, false);
  sim.push(1030, false);
  sim.push(1040, false);
  const auto out = sim.release_at(1040);
  REQUIRE(out.size() == 2);
  CHECK(out[0].action == FallbackAction::None);  // covered by the cycle at 1010
  CHECK(out[1].action == FallbackAction::None);
  CHECK(sim.la.stats().early_releases == 2);
  CHECK(sim.la.buffered() == 3);
}

TEST_CASE("a frame released long after its deadline fails closed", "[schedule]") {
  SchedulerConfig config = small_config();
  config.release_grace = 50ms;
  Sim sim(config, spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.push(1000, true);
  sim.scan_now();
  sim.push(1001, true);
  sim.scan_now();
  auto out = sim.release_at(1100);
  REQUIRE(out.size() == 1);
  CHECK(out[0].action == FallbackAction::None);
  out = sim.release_at(1151);  // exactly lookahead + grace: still on time
  REQUIRE(out.size() == 1);
  CHECK(out[0].action == FallbackAction::None);
  sim.push(1152, true);
  sim.scan_now();
  out = sim.release_at(1303);  // frame 1152 is now 151 ms old
  REQUIRE(out.size() == 1);
  CHECK(out[0].reason == FallbackReason::LateRelease);
}

TEST_CASE("the scan queue is bounded and keeps the newest frames", "[schedule]") {
  SchedulerConfig config = small_config();
  config.queue_capacity = 2;
  Sim sim(config, spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  std::vector<spg::FrameId> ids;
  for (std::int64_t t = 1000; t < 1050; t += 10) {
    ids.push_back(sim.push(t, true));
  }
  CHECK(sim.la.queued() == 2);
  CHECK(sim.la.stats().scans_queued == 5);
  CHECK(sim.la.stats().scans_dropped == 3);
  auto job = sim.la.take_job();
  REQUIRE(job.has_value());
  CHECK(job->frame == ids[4]);  // newest first
  job = sim.la.take_job();
  REQUIRE(job.has_value());
  CHECK(job->frame == ids[3]);
  CHECK_FALSE(sim.la.take_job().has_value());
  CHECK(sim.la.in_flight() == 2);
}

TEST_CASE("the cadence spaces queued frames", "[schedule]") {
  SchedulerConfig config = small_config();
  config.scan_interval = 50ms;
  config.queue_capacity = 10;
  Sim sim(config);
  for (std::int64_t t = 1000; t < 1200; t += 10) {
    CHECK(sim.la.wants_scan(at_ms(t)) == ((t - 1000) % 50 == 0));
    sim.push(t, true);
  }
  CHECK(sim.la.stats().scans_queued == 4);  // 1000, 1050, 1100, 1150
}

TEST_CASE("hold-last-scanned re-emits the last clean frame of the same content", "[schedule]") {
  SchedulerConfig config = small_config();
  config.fallback = FallbackMode::HoldLastScanned;
  config.retro = 5ms;  // a cycle covers only its own frame here
  Sim sim(config, spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.push(1000, false);  // nothing clean yet: blur instead
  const spg::FrameId clean = sim.push(1010, true);
  sim.scan_now();
  sim.push(1020, false);
  sim.push(1030, false, k1080);  // resized: the clean frame is the wrong size
  auto out = sim.release_at(1200);
  REQUIRE(out.size() == 4);
  CHECK(out[0].action == FallbackAction::BlurSource);
  CHECK(out[0].reemit == 0);
  CHECK(out[1].action == FallbackAction::None);
  CHECK(out[2].action == FallbackAction::HoldLastScanned);
  CHECK(out[2].reemit == clean);
  CHECK(out[3].action == FallbackAction::BlurSource);

  const spg::FrameId clean2 = sim.push(1300, true, k1080);
  sim.scan_now();
  sim.push(1310, false, k1080);
  sim.la.mark_discontinuity();
  sim.push(1320, false, k1080);
  out = sim.release_at(1500);
  REQUIRE(out.size() == 3);
  CHECK(out[0].action == FallbackAction::None);
  CHECK(out[1].action == FallbackAction::HoldLastScanned);
  CHECK(out[1].reemit == clean2);
  CHECK(out[2].action == FallbackAction::BlurSource);  // content switched
}

// SPG-10b: a re-emit is a release too. A complete cycle that finishes after
// the clean frame aired, and whose boxes that frame would have needed (held
// over it, or retroactive), means the frame no longer passes the release
// gate: showing it again would air those boxes unmasked.
TEST_CASE("hold-last-scanned never re-emits a frame a later result would have masked", "[schedule]") {
  const Rect box{40, 40, 30, 30};
  SchedulerConfig config = small_config();  // lookahead 100, retro 100, hold 200
  config.fallback = FallbackMode::HoldLastScanned;
  // The detector sees the box only on frame 1000 (detector flicker, which the
  // hold exists for).
  Sim sim(config, spg::ScanStages{std::make_shared<spg_test::ScriptedDetector>([box](Timestamp t, Size) {
                                    std::vector<Detection> out;
                                    if (t == at_ms(1000)) {
                                      out.push_back(face_at(box.x, box.y, box.width, box.height));
                                    }
                                    return out;
                                  }),
                                  nullptr, nullptr, nullptr});

  SECTION("a result held over the clean frame") {
    sim.push(1000, true);
    auto [slow, found] = sim.run();  // finds the box, returns late
    REQUIRE(found.detections.size() == 1);
    const spg::FrameId clean = sim.push(1010, true);
    sim.scan_now();  // sees nothing; covers 1000 and 1010
    sim.push(1020, false);
    auto out = sim.release_at(1110);
    REQUIRE(out.size() == 2);
    REQUIRE(out[1].id == clean);
    REQUIRE(out[1].action == FallbackAction::None);
    CHECK_FALSE(covered_by(out[1].plan, box));  // the box was not known yet

    sim.la.complete(slow, found);  // the box at 1000 is held over 1010
    out = sim.release_at(1120);
    REQUIRE(out.size() == 1);
    CHECK(out[0].reason == FallbackReason::NotScanned);
    CHECK(out[0].action == FallbackAction::BlurSource);  // not frame 1010 again
    CHECK(out[0].reemit == 0);
  }

  SECTION("a result retroactive to the clean frame") {
    sim.stages.faces = std::make_shared<spg_test::ScriptedDetector>([box](Timestamp t, Size) {
      std::vector<Detection> out;
      if (t == at_ms(1050)) {
        out.push_back(face_at(box.x, box.y, box.width, box.height));
      }
      return out;
    });
    const spg::FrameId clean = sim.push(1000, true);
    sim.scan_now();  // sees nothing
    sim.push(1050, true);
    auto [slow, found] = sim.run();  // finds the box; 1000 is within retro of 1050
    REQUIRE(found.detections.size() == 1);
    sim.push(1060, false);
    auto out = sim.release_at(1150);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].id == clean);
    REQUIRE(out[0].action == FallbackAction::None);
    CHECK(out[1].action == FallbackAction::HoldLastScanned);  // nothing known against 1000 yet
    CHECK(out[1].reemit == clean);

    sim.la.complete(slow, found);  // would have masked 1000 retroactively
    out = sim.release_at(1160);
    REQUIRE(out.size() == 1);
    CHECK(out[0].reason == FallbackReason::NotScanned);
    CHECK(out[0].action == FallbackAction::BlurSource);
    CHECK(out[0].reemit == 0);
  }

  SECTION("an empty later result keeps the clean frame") {
    const spg::FrameId clean = sim.push(1010, true);
    sim.scan_now();
    sim.push(1020, true);
    auto [late, empty] = sim.run();  // sees nothing
    sim.push(1030, false);
    auto out = sim.release_at(1120);
    REQUIRE(out.size() == 2);
    CHECK(out[0].action == FallbackAction::None);
    CHECK(out[1].action == FallbackAction::HoldLastScanned);  // 1020's cycle is late
    sim.la.complete(late, empty);
    out = sim.release_at(1130);
    REQUIRE(out.size() == 1);
    CHECK(out[0].action == FallbackAction::HoldLastScanned);
    CHECK(out[0].reemit == clean);
  }
}

TEST_CASE("hold-last-scanned never re-emits a frame a failed apply could have masked", "[schedule]") {
  SchedulerConfig config = small_config();
  config.fallback = FallbackMode::HoldLastScanned;
  Sim sim(config, spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  const spg::FrameId clean = sim.push(1000, true);
  sim.scan_now();
  sim.push(1050, true);
  auto [job, result] = sim.run();
  auto out = sim.release_at(1100);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].id == clean);
  REQUIRE(out[0].action == FallbackAction::None);
  // A complete result the tracker rejects, as in the apply-failure test: its
  // valid box would have masked 1000 retroactively.
  result.detections = {face_at(10, 10, 20, 20), face_at(10, 10, -5, 5)};
  sim.la.complete(job, result);
  out = sim.release_at(1150);
  REQUIRE(out.size() == 1);
  CHECK(out[0].reason == FallbackReason::ApplyFailed);
  CHECK(out[0].action == FallbackAction::BlurSource);
  CHECK(out[0].reemit == 0);
}

// SPG-10b: a complete result from before more resizes than the size history
// holds cannot be scaled to the current size, so it cannot be applied. It must
// fail closed like any other result that cannot be applied, not vanish: its
// box is held over later frames that another cycle covers.
TEST_CASE("a complete result older than the size history fails closed", "[schedule]") {
  const Rect box720{100, 100, 20, 20};
  const Rect box1080{150, 150, 30, 30};  // box720 at 1.5x
  SchedulerConfig config = small_config();
  config.lookahead = 500ms;
  config.retro = 100ms;
  config.tracker.hold = 750ms;
  config.queue_capacity = 1;
  // Sees the box only on frame 1000 (detector flicker).
  Sim sim(config, spg::ScanStages{std::make_shared<spg_test::ScriptedDetector>([=](Timestamp t, Size) {
                                    std::vector<Detection> out;
                                    if (t == at_ms(1000)) {
                                      out.push_back(face_at(box720.x, box720.y, box720.width, box720.height));
                                    }
                                    return out;
                                  }),
                                  nullptr, nullptr, nullptr});
  sim.push(1000, true, k720);
  auto [slow, found] = sim.run();
  REQUIRE(found.detections.size() == 1);

  const auto resize_then_scan = [&sim](int resizes) {
    Size size = k720;
    std::int64_t t = 1000;
    for (int i = 0; i < resizes; ++i) {
      size = size == k720 ? k1080 : k720;
      t += 2;
      sim.push(t, false, size);
    }
    sim.push(t + 2, true, size);  // same size: same segment as the last resize
    sim.scan_now();               // sees nothing; covers both
    return size;
  };

  SECTION("63 resizes: the result is applied and held over later frames") {
    const Size last = resize_then_scan(63);
    REQUIRE(last == k1080);
    sim.la.complete(slow, found);
    CHECK(sim.la.stats().scans_completed == 2);
    const auto out = sim.release_at(1700);
    REQUIRE(out.size() == 65);
    for (std::size_t i = 63; i < 65; ++i) {
      INFO("frame " << i);
      CHECK(out[i].action == FallbackAction::None);
      CHECK(covered_by(out[i].plan, box1080));
    }
  }
  SECTION("64 resizes: the result cannot be scaled and its frames fail closed") {
    const Size last = resize_then_scan(64);
    REQUIRE(last == k720);
    sim.la.complete(slow, found);
    const auto out = sim.release_at(1700);
    REQUIRE(out.size() == 66);
    for (std::size_t i = 64; i < 66; ++i) {
      INFO("frame " << i);
      // On main these were released clean without the held box.
      CHECK(out[i].reason == FallbackReason::ApplyFailed);
      CHECK(out[i].plan.fallback);
    }
  }
}

// SPG-10c: a cycle that fails after some of its stages finished still found
// those boxes. They are held (temporal hold) and masked retroactively, so a
// later cycle whose face detector misses cannot air them inside the hold; the
// failed cycle still covers no frame.
TEST_CASE("detections from a failed cycle are held and masked retroactively", "[schedule]") {
  const Rect box{40, 40, 30, 30};
  SchedulerConfig config = small_config();  // retro 100, hold 200
  config.lookahead = 500ms;
  // The face detector sees the box only on frame 1040 (flicker), and the text
  // detector throws on that frame.
  const auto faces = std::make_shared<spg_test::ScriptedDetector>([box](Timestamp t, Size) {
    std::vector<Detection> out;
    if (t == at_ms(1040)) {
      out.push_back(face_at(box.x, box.y, box.width, box.height));
    }
    return out;
  });
  const auto text = std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{}; },
      [](Timestamp t) {
        if (t == at_ms(1040)) {
          throw std::runtime_error("InferenceFailed: session run");
        }
      });
  Sim sim(config, spg::ScanStages{faces, text, nullptr, nullptr});
  sim.push(1000, false);
  sim.push(1020, false);
  sim.push(1040, true);
  const spg::ScanResult partial = sim.scan_now();
  CHECK_FALSE(partial.complete);
  CHECK(partial.failure == spg::ScanFailure::TextDetector);
  CHECK(partial.detections.size() == 1);
  CHECK(sim.la.stats().scans_failed == 1);
  CHECK(sim.la.stats().scans_completed == 0);
  sim.push(1060, false);

  SECTION("later cycles that miss the face keep it masked within the hold") {
    sim.push(1080, false);
    sim.push(1100, true);
    CHECK(sim.scan_now().complete);  // sees nothing; covers 1000 .. 1100
    sim.push(1200, true);            // within the hold (1040 + 200)
    CHECK(sim.scan_now().complete);  // sees nothing
    const auto out = sim.release_at(1800);
    REQUIRE(out.size() == 7);
    for (const ReleasedFrame& r : out) {
      INFO("frame at " << spg::to_ns(r.frame_time) / 1'000'000);
      // On main these aired clean without the box: 1000 and 1020 before it
      // was found (retroactive), 1040 .. 1200 after it (hold).
      CHECK(r.action == FallbackAction::None);
      CHECK(covered_by(r.plan, box));
    }
    CHECK(sim.la.stats().scans_completed == 2);
  }
  SECTION("on its own it covers no frame") {
    const auto out = sim.release_at(1800);
    REQUIRE(out.size() == 4);
    for (const ReleasedFrame& r : out) {
      INFO("frame at " << spg::to_ns(r.frame_time) / 1'000'000);
      CHECK(r.reason == FallbackReason::NotScanned);
      CHECK(covered_by(r.plan, box));  // the fallback plan still lists it
    }
  }
}

TEST_CASE("a failed cycle keeps the valid boxes its stages found before the failure", "[schedule]") {
  const Rect face{10, 10, 20, 20};
  const Rect r1{0, 50, 60, 10};
  const Rect r2{0, 70, 60, 10};
  const auto faces = faces_always(face);
  const auto regions = std::make_shared<spg_test::ScriptedDetector>([=](Timestamp, Size) {
    return std::vector<Detection>{text_at(r1.x, r1.y, r1.width, r1.height), text_at(r2.x, r2.y, r2.width, r2.height)};
  });
  // Region r1 reads "fine", r2 reads "boom".
  const auto recognizer = std::make_shared<spg_test::ScriptedRecognizer>(
      [](const Detection& d) { return std::string(d.box.rect.y == 50.0F ? "fine" : "boom"); });
  // Clears "fine" and throws on "boom", after judging r1 safe.
  struct BoomPolicy final : spg::TextPolicy {
    spg::Verdict evaluate(const spg::Secret& text) const override {
      if (text.reveal() == "boom") {
        throw std::runtime_error("policy failed");
      }
      return {};
    }
  };
  struct ThrowingRecognizer final : spg::TextRecognizer {
    std::vector<spg::Secret> recognize(const spg::FrameView&, Size,
                                       std::span<const Detection>) const override {
      throw std::runtime_error("InferenceFailed");
    }
  };
  struct ShortRecognizer final : spg::TextRecognizer {
    std::vector<spg::Secret> recognize(const spg::FrameView&, Size,
                                       std::span<const Detection>) const override {
      return {};
    }
  };
  const auto throwing = [](Timestamp) { throw std::runtime_error("InferenceFailed"); };
  const auto empty = [](Timestamp, Size) { return std::vector<Detection>{}; };

  Sim sim(small_config());
  sim.push(1000, true);
  std::optional<spg::ScanJob> job = sim.la.take_job();
  REQUIRE(job.has_value());
  const spg::ScanLimits& all = sim.la.config().scan;
  const auto rects = [](const spg::ScanResult& r) {
    std::vector<Rect> out;
    for (const Detection& d : r.detections) {
      out.push_back(d.box.rect);
    }
    return out;
  };
  const auto expect = [&](const spg::ScanStages& stages, spg::ScanFailure failure, const std::vector<Rect>& kept) {
    const spg::ScanResult r = spg::run_scan(stages, *job, all);
    CHECK_FALSE(r.complete);
    CHECK(r.failure == failure);
    CHECK(rects(r) == kept);
    for (const Detection& d : r.detections) {
      CHECK_NOTHROW(spg::validate(d));
    }
  };

  // Faces found, then the text detector throws.
  expect(spg::ScanStages{faces, std::make_shared<spg_test::ScriptedDetector>(empty, throwing), nullptr, nullptr},
         spg::ScanFailure::TextDetector, {face});
  // Text regions found, then the recognizer throws or miscounts: no region
  // was cleared, so every region is kept.
  expect(spg::ScanStages{faces, regions, std::make_shared<ThrowingRecognizer>(),
                         std::make_shared<spg_test::ScriptedPolicy>()},
         spg::ScanFailure::Recognizer, {face, r1, r2});
  expect(spg::ScanStages{faces, regions, std::make_shared<ShortRecognizer>(),
                         std::make_shared<spg_test::ScriptedPolicy>()},
         spg::ScanFailure::InvalidOutput, {face, r1, r2});
  // The policy throws midway: r1, judged safe before it failed, is kept too.
  expect(spg::ScanStages{faces, regions, recognizer, std::make_shared<BoomPolicy>()}, spg::ScanFailure::Policy,
         {face, r1, r2});
  // The face detector throws first: nothing was found, and text never runs.
  const auto text_calls = regions->calls.load();
  expect(spg::ScanStages{std::make_shared<spg_test::ScriptedDetector>(empty, throwing), regions, nullptr, nullptr},
         spg::ScanFailure::FaceDetector, {});
  CHECK(regions->calls == text_calls);
  // Malformed output is never kept (non-finite, negative, wrong space or
  // kind, bad score); its valid siblings are.
  Detection nan_box = face_at(1, 1, 5, 5);
  nan_box.box.rect.x = std::nanf("");
  Detection wrong_space = face_at(1, 1, 5, 5);
  wrong_space.box.space = spg::Space::Readback;
  Detection bad_score = face_at(1, 1, 5, 5);
  bad_score.score = 2.0F;
  expect(spg::ScanStages{std::make_shared<spg_test::ScriptedDetector>([=](Timestamp, Size) {
                           return std::vector<Detection>{nan_box, face_at(face.x, face.y, face.width, face.height),
                                                         wrong_space, bad_score, text_at(1, 1, 5, 5),
                                                         face_at(1, 1, -5, 5)};
                         }),
                         regions, nullptr, nullptr},
         spg::ScanFailure::InvalidOutput, {face});
  CHECK(regions->calls == text_calls);
  expect(spg::ScanStages{faces, std::make_shared<spg_test::ScriptedDetector>([=](Timestamp, Size) {
                           return std::vector<Detection>{text_at(r1.x, r1.y, r1.width, r1.height),
                                                         text_at(0, 0, -1, 4), face_at(0, 0, 4, 4)};
                         }),
                         recognizer, std::make_shared<spg_test::ScriptedPolicy>()},
         spg::ScanFailure::InvalidOutput, {face, r1});
}

TEST_CASE("a failed cycle whose boxes cannot be applied fails closed every frame they could mask",
          "[schedule]") {
  SchedulerConfig config = small_config();
  config.lookahead = 500ms;  // retro 100 ms, hold 200 ms
  Sim sim(config, spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.push(1000, true);
  sim.scan_now();  // an empty cycle covers 1000
  sim.push(1050, true);
  auto [job, result] = sim.run();
  // A partial result the tracker rejects: its valid box would have masked
  // 1000 retroactively and later frames by hold.
  result.complete = false;
  result.failure = spg::ScanFailure::TextDetector;
  result.detections = {face_at(10, 10, 20, 20), face_at(10, 10, -5, 5)};
  sim.la.complete(job, result);
  CHECK(sim.la.stats().scans_failed == 1);
  CHECK(sim.la.stats().scans_completed == 1);
  sim.push(1100, true);  // within the hold after 1050
  sim.scan_now();
  sim.push(1300, true);  // past it
  sim.scan_now();
  const auto out = sim.release_at(1900);
  REQUIRE(out.size() == 4);
  CHECK(out[0].reason == FallbackReason::ApplyFailed);  // on main: clean, without the box
  CHECK(out[1].reason == FallbackReason::ApplyFailed);
  CHECK(out[2].reason == FallbackReason::ApplyFailed);
  CHECK(out[3].action == FallbackAction::None);
}

// Greptile on PR #58: an allocation failure while run_scan() collects the
// boxes a stage returned (the face insert, the policy's per-region flags,
// appending every region after a text stage failed, keeping the valid
// siblings of malformed output) used to return only what was collected so
// far, so the scheduler held less than the cycle saw.
TEST_CASE("a cycle that cannot keep every box its stages returned says so", "[schedule]") {
  const Rect face{10, 10, 20, 20};
  const Rect r1{0, 50, 60, 10};
  const Rect r2{0, 70, 60, 10};
  const auto regions = std::make_shared<spg_test::ScriptedDetector>([=](Timestamp, Size) {
    return std::vector<Detection>{text_at(r1.x, r1.y, r1.width, r1.height), text_at(r2.x, r2.y, r2.width, r2.height)};
  });
  // r1 reads "fine", r2 reads "SECRET" (sensitive to ScriptedPolicy).
  const auto recognizer = std::make_shared<spg_test::ScriptedRecognizer>(
      [](const Detection& d) { return std::string(d.box.rect.y == 50.0F ? "fine" : "SECRET"); });
  struct ThrowingRecognizer final : spg::TextRecognizer {
    std::vector<spg::Secret> recognize(const spg::FrameView&, Size,
                                       std::span<const Detection>) const override {
      throw std::runtime_error("InferenceFailed");
    }
  };
  const auto policy = std::make_shared<spg_test::ScriptedPolicy>();
  Sim sim(small_config());
  sim.push(1000, true);
  std::optional<spg::ScanJob> job = sim.la.take_job();
  REQUIRE(job.has_value());
  const spg::ScanLimits& all = sim.la.config().scan;
  const auto rects = [](const spg::ScanResult& r) {
    std::vector<Rect> out;
    for (const Detection& d : r.detections) {
      out.push_back(d.box.rect);
    }
    return out;
  };

  // The boxes a result keeps, by its failure (None: complete), when no box
  // was lost. A failure missing here is a test failure.
  using Kept = std::map<spg::ScanFailure, std::vector<Rect>>;
  // Fails each allocation of one cycle in turn, until one runs as the
  // unfailed cycle does; returns how many of them lost boxes.
  const auto sweep = [&](const spg::ScanStages& stages, const Kept& kept) {
    // One unfailed cycle first, so one-time setup (the secure heap behind
    // Secret) is done and the sweep sees only the allocations of a cycle.
    const spg::ScanResult unfailed = spg::run_scan(stages, *job, all);
    int lost = 0;
    for (std::uint64_t n = 0;; ++n) {
      REQUIRE(n < 10000);
      spg::ScanResult r;
      bool fired = false;
      {
        const spg_test::FailAllocations fail(n);
        r = spg::run_scan(stages, *job, all);
        fired = fail.fired();
      }
      INFO("allocation " << n);
      if (!fired) {
        CHECK(r.complete == unfailed.complete);
        CHECK(r.failure == unfailed.failure);
      }
      if (r.detections_lost) {
        // Collecting failed: never complete, always InvalidOutput.
        CHECK(fired);
        CHECK_FALSE(r.complete);
        CHECK(r.failure == spg::ScanFailure::InvalidOutput);
        ++lost;
        continue;
      }
      CHECK(r.complete == (r.failure == spg::ScanFailure::None));
      const auto it = kept.find(r.failure);
      REQUIRE(it != kept.end());
      CHECK(rects(r) == it->second);
      if (!fired) {
        return lost;
      }
    }
  };

  SECTION("a complete cycle: the face insert, the policy flags, the region append") {
    const Kept kept{{spg::ScanFailure::None, {face, r2}},
                    {spg::ScanFailure::FaceDetector, {}},
                    {spg::ScanFailure::TextDetector, {face}},
                    {spg::ScanFailure::Recognizer, {face, r1, r2}},
                    {spg::ScanFailure::Policy, {face, r1, r2}}};
    CHECK(sweep(spg::ScanStages{faces_always(face), regions, recognizer, policy}, kept) >= 3);
  }
  SECTION("the recognizer throws: appending every region") {
    const Kept kept{{spg::ScanFailure::FaceDetector, {}},
                    {spg::ScanFailure::TextDetector, {face}},
                    {spg::ScanFailure::Recognizer, {face, r1, r2}}};
    const spg::ScanStages stages{faces_always(face), regions, std::make_shared<ThrowingRecognizer>(), policy};
    CHECK(sweep(stages, kept) >= 2);
  }
  SECTION("malformed faces: keeping their valid siblings") {
    Detection nan_box = face_at(1, 1, 5, 5);
    nan_box.box.rect.x = std::nanf("");
    Detection wrong_space = face_at(1, 1, 5, 5);
    wrong_space.box.space = spg::Space::Readback;
    const auto faces = std::make_shared<spg_test::ScriptedDetector>([=](Timestamp, Size) {
      return std::vector<Detection>{nan_box, face_at(face.x, face.y, face.width, face.height), wrong_space};
    });
    const Kept kept{{spg::ScanFailure::FaceDetector, {}}, {spg::ScanFailure::InvalidOutput, {face}}};
    CHECK(sweep(spg::ScanStages{faces, regions, recognizer, policy}, kept) >= 1);
  }
}

TEST_CASE("a cycle that lost boxes it found fails closed every frame they could mask", "[schedule]") {
  SchedulerConfig config = small_config();
  config.lookahead = 500ms;  // retro 100 ms, hold 200 ms
  Sim sim(config, spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.push(1000, true);
  sim.scan_now();  // an empty cycle covers 1000
  sim.push(1050, true);
  auto [job, result] = sim.run();
  // What run_scan() returns when collecting the boxes fails: the face the
  // cycle found is missing from `detections`, and the result says so.
  result.complete = false;
  result.failure = spg::ScanFailure::InvalidOutput;
  result.detections.clear();
  result.detections_lost = true;
  sim.la.complete(job, result);
  CHECK(sim.la.stats().scans_failed == 1);
  CHECK(sim.la.stats().scans_completed == 1);
  sim.push(1100, true);  // within the hold after 1050
  sim.scan_now();
  sim.push(1300, true);  // past it
  sim.scan_now();
  const auto out = sim.release_at(1900);
  REQUIRE(out.size() == 4);
  // Without the flag, 1000 and 1100 aired clean: an empty incomplete result
  // changes nothing.
  CHECK(out[0].reason == FallbackReason::ApplyFailed);
  CHECK(out[1].reason == FallbackReason::ApplyFailed);
  CHECK(out[2].reason == FallbackReason::ApplyFailed);
  CHECK(out[3].action == FallbackAction::None);
}

TEST_CASE("hold-last-scanned never re-emits a frame a failed cycle's boxes would have masked", "[schedule]") {
  const Rect box{40, 40, 30, 30};
  SchedulerConfig config = small_config();  // lookahead 100, retro 100, hold 200
  config.fallback = FallbackMode::HoldLastScanned;
  const auto faces = std::make_shared<spg_test::ScriptedDetector>([box](Timestamp t, Size) {
    std::vector<Detection> out;
    if (t == at_ms(1000)) {
      out.push_back(face_at(box.x, box.y, box.width, box.height));
    }
    return out;
  });
  const auto text = std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{}; },
      [](Timestamp t) {
        if (t == at_ms(1000)) {
          throw spg_test::NotAStdException{3};
        }
      });
  Sim sim(config, spg::ScanStages{faces, text, nullptr, nullptr});
  sim.push(1000, true);
  auto [slow, partial] = sim.run();  // finds the box, then fails; returns late
  CHECK_FALSE(partial.complete);
  CHECK(partial.detections.size() == 1);
  const spg::FrameId clean = sim.push(1010, true);
  sim.scan_now();  // sees nothing; covers 1000 and 1010
  sim.push(1020, false);
  auto out = sim.release_at(1110);
  REQUIRE(out.size() == 2);
  REQUIRE(out[1].id == clean);
  REQUIRE(out[1].action == FallbackAction::None);

  sim.la.complete(slow, partial);  // the box at 1000 is held over 1010
  out = sim.release_at(1120);
  REQUIRE(out.size() == 1);
  CHECK(out[0].reason == FallbackReason::NotScanned);
  CHECK(out[0].action == FallbackAction::BlurSource);  // not frame 1010 again
  CHECK(out[0].reemit == 0);
}

TEST_CASE("a discontinuity stops older cycles from covering new frames", "[schedule]") {
  Sim sim(small_config(), spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.push(1000, false);
  sim.la.mark_discontinuity();
  sim.push(1010, true);
  sim.scan_now();
  const auto out = sim.release_at(1200);
  REQUIRE(out.size() == 2);
  CHECK(out[0].reason == FallbackReason::NotScanned);  // old content, new cycle
  CHECK(out[1].action == FallbackAction::None);
}

TEST_CASE("complete() rejects unknown and repeated jobs", "[schedule]") {
  Sim sim(small_config(), spg::ScanStages{nothing(), nullptr, nullptr, nullptr});
  sim.push(1000, true);
  auto [job, result] = sim.run();
  sim.la.complete(job, result);
  CHECK_THROWS_AS(sim.la.complete(job, result), spg::ContractError);
  spg::ScanJob unknown;
  unknown.seq = 999;
  CHECK_THROWS_AS(sim.la.complete(unknown, result), spg::ContractError);
}

TEST_CASE("plans carry the configured mask style", "[schedule]") {
  SchedulerConfig config = small_config();
  config.mask_style = spg::MaskStyle::Pixelate;
  Sim sim(config, spg::ScanStages{faces_always(Rect{1, 1, 4, 4}), nullptr, nullptr, nullptr});
  sim.push(1000, false);
  sim.push(1010, true);
  sim.scan_now();
  const auto out = sim.release_at(1200);
  REQUIRE(out.size() == 2);
  for (const ReleasedFrame& r : out) {
    REQUIRE_FALSE(r.plan.rects.empty());
    for (const spg::MaskRect& m : r.plan.rects) {
      CHECK(m.style == spg::MaskStyle::Pixelate);
    }
  }
}

TEST_CASE("from_policy carries the policy's timing, fallback and margins", "[schedule]") {
  spg::PolicyConfig policy;
  policy.lookahead = 800ms;
  policy.retro = 300ms;
  policy.hold = 1200ms;
  policy.fallback = FallbackMode::SolidFill;
  policy.mask_style = spg::MaskStyle::SolidFill;
  policy.margin_px = 7.0F;
  policy.categories = spg::CategorySet{};
  policy.categories.insert(spg::Category::Face);
  // A config spg::validate accepts (SPG-08b floors): no empty category set,
  // no hold below kMinHold.
  REQUIRE_NOTHROW(spg::validate(policy));
  const SchedulerConfig config = SchedulerConfig::from_policy(policy);
  CHECK(config.lookahead == 800ms);
  CHECK(config.retro == 300ms);
  CHECK(config.tracker.hold == 1200ms);
  CHECK(config.fallback == FallbackMode::SolidFill);
  CHECK(config.mask_style == spg::MaskStyle::SolidFill);
  CHECK(config.tracker.margin_px == 7.0F);
  CHECK(config.scan.categories == policy.categories);
}

TEST_CASE("recognized text and stage errors never reach output streams", "[schedule]") {
  const std::string sentinel = "Zq7 Sentinel Hollowbrook Lane";
  std::ostringstream captured;
  auto* const out_buf = std::cout.rdbuf(captured.rdbuf());
  auto* const err_buf = std::cerr.rdbuf(captured.rdbuf());
  auto* const log_buf = std::clog.rdbuf(captured.rdbuf());
  {
    const auto regions = std::make_shared<spg_test::ScriptedDetector>(
        [](Timestamp, Size) { return std::vector<Detection>{text_at(0, 0, 10, 10)}; });
    const auto recognizer =
        std::make_shared<spg_test::ScriptedRecognizer>([&](const Detection&) { return sentinel; });
    Sim sim(small_config(),
            spg::ScanStages{nullptr, regions, recognizer, std::make_shared<spg_test::ScriptedPolicy>()});
    sim.push(1000, true);
    CHECK(sim.scan_now().complete);
    const auto throwing = std::make_shared<spg_test::ScriptedDetector>(
        [](Timestamp, Size) { return std::vector<Detection>{}; },
        [&](Timestamp) { throw std::runtime_error(sentinel); });
    sim.stages = spg::ScanStages{nullptr, throwing, recognizer, nullptr};
    sim.push(1010, true);
    CHECK_FALSE(sim.scan_now().complete);
    (void)sim.release_at(2000);
  }
  std::cout.rdbuf(out_buf);
  std::cerr.rdbuf(err_buf);
  std::clog.rdbuf(log_buf);
  CHECK(captured.str().empty());
}

TEST_CASE("mask rect helpers round outward", "[schedule]") {
  using spg::mask::contains;
  const Rect r{10.25F, 20.75F, 5.5F, 3.1F};
  const Rect px = spg::mask::pixel_rect(10.25, 20.75, 15.75, 23.85);
  CHECK(px == Rect{10, 20, 6, 4});
  CHECK(contains(px, r));
  CHECK(spg::mask::grow_by_margin(Rect{10, 10, 20, 8}, 2.0F, 0.5F) == Rect{4, 4, 32, 20});  // 2 + 0.5 * 8
  // 1280x720 -> 1920x1080 is exactly 1.5x.
  CHECK(spg::mask::scale_between(Rect{100, 100, 50, 20}, k720, k1080) == Rect{150, 150, 75, 30});
  // Non-integer scale rounds outward.
  const Rect s = spg::mask::scale_between(Rect{1, 1, 1, 1}, Size{3, 3}, Size{4, 4});
  CHECK(contains(s, Rect{4.0F / 3.0F, 4.0F / 3.0F, 4.0F / 3.0F, 4.0F / 3.0F}));
  CHECK(spg::mask::hull(Rect{0, 0, 1, 1}, Rect{5, 5, 1, 1}) == Rect{0, 0, 6, 6});
  // NaN edges cover rather than vanish.
  const Rect nan_rect = spg::mask::pixel_rect(std::nan(""), 0, 5, 5);
  CHECK(nan_rect.width > 1e6F);
}
