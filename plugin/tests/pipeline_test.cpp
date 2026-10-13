// The filter's render pipeline (SPG-14) without OBS: a fake GPU records what
// the pipeline asks for, and a synchronous scheduler runs spg::Lookahead on a
// ManualClock with a fixed scan latency, so every case is deterministic. The
// GPU effect itself (plugin/data/effects/spg-mask.effect) needs a graphics
// context and is checked live (docs/live-check.md).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "alloc_fault.hpp"
#include "pipeline/builder.hpp"
#include "pipeline/frame_ring.hpp"
#include "pipeline/host.hpp"
#include "pipeline/mask_levels.hpp"
#include "pipeline/pipeline.hpp"
#include "pipeline/readback_pool.hpp"
#include "pipeline/reaper.hpp"
#include "schedule_fakes.hpp"
#include "spg/lookahead.hpp"
#include "spg/scan.hpp"

namespace {

using namespace std::chrono_literals;
using spg::Detection;
using spg::FallbackMode;
using spg::Rect;
using spg::Size;
using spg::Timestamp;
using spg::plugin::Cover;
using spg::plugin::DrawKind;
using spg::plugin::DrawPlan;
using spg::plugin::Pipeline;
using spg_test::at_ms;

constexpr Size k720{1280, 720};

// Records the GPU work; the readback pixels name the slot they came from.
class FakeGpu final : public spg::plugin::FrameGpu {
 public:
  bool capture(std::size_t slot, Size size, std::uint64_t serial) override {
    ++captures;
    slot_serial[slot] = 0;
    if (throw_capture) {
      throw std::runtime_error("device lost");
    }
    if (fail_capture) {
      return false;
    }
    slot_size[slot] = size;
    slot_serial[slot] = serial;
    return true;
  }
  [[nodiscard]] spg::plugin::SlotContents contents(std::size_t slot) const {
    spg::plugin::SlotContents out;
    if (const auto it = slot_serial.find(slot); it != slot_serial.end() && it->second != 0) {
      out.present = true;
      out.serial = it->second;
      out.size = slot_size.at(slot);
    }
    return out;
  }
  bool stage(std::size_t slot, std::size_t surface, Size readback) override {
    REQUIRE(surface < 2);
    REQUIRE_FALSE(mapped[surface]);
    staged[surface] = Staged{slot, readback, tick};
    stage_calls.push_back(surface);
    return true;
  }
  bool map(std::size_t surface, spg::plugin::MappedReadback& out) override {
    REQUIRE(surface < 2);
    const std::optional<Staged>& s = staged[surface];
    if (!s) {
      return false;
    }
    // A surface is mapped no sooner than the tick after it was staged.
    REQUIRE(s->tick < tick);
    const std::uint32_t linesize = s->size.width * 4 + 12;  // padded rows
    pixels.assign(static_cast<std::size_t>(linesize) * s->size.height, static_cast<std::uint8_t>(s->slot));
    out.data = pixels.data();
    out.linesize = linesize;
    mapped[surface] = true;
    return true;
  }
  void unmap(std::size_t surface) override {
    REQUIRE(mapped[surface]);
    mapped[surface] = false;
    staged[surface].reset();
  }
  void free_slot(std::size_t slot) override {
    freed.push_back(slot);
    slot_serial.erase(slot);
  }
  [[nodiscard]] std::uint32_t bits_per_pixel() const noexcept override { return 32; }  // BGRA

  struct Staged {
    std::size_t slot = 0;
    Size size;
    int tick = 0;
  };
  bool fail_capture = false;
  bool throw_capture = false;
  int tick = 0;
  int captures = 0;
  std::map<std::size_t, Size> slot_size;
  std::map<std::size_t, std::uint64_t> slot_serial;
  std::array<std::optional<Staged>, 2> staged;
  std::array<bool, 2> mapped{};
  std::vector<std::size_t> stage_calls;
  std::vector<std::size_t> freed;
  std::vector<std::uint8_t> pixels;
};

// spg::Lookahead driven inline: a job taken at clock time c completes at
// c + latency, when the pipeline next calls in.
class SyncScheduler final : public spg::plugin::FrameScheduler {
 public:
  SyncScheduler(const spg::ManualClock& clock, spg::SchedulerConfig config, spg::ScanStages stages,
                spg::Duration latency, std::shared_ptr<int> destroyed)
      : clock_(clock),
        lookahead_(clock, config),
        stages_(std::move(stages)),
        limits_(config.scan),
        latency_(latency),
        destroyed_(std::move(destroyed)) {}
  ~SyncScheduler() override {
    if (destroyed_) {
      ++*destroyed_;
    }
  }
  [[nodiscard]] bool wants_scan(Timestamp t) const override { return lookahead_.wants_scan(t); }
  spg::FrameId push(Timestamp t, Size source, std::shared_ptr<const spg::OwnedFrame> readback) override {
    run_ready();
    if (readback != nullptr) {
      scanned.push_back(readback->view().timestamp);
      // Tightly packed copy of the padded mapped rows.
      CHECK(readback->view().strides[0] == readback->view().width * 4);
    }
    const spg::FrameId id = lookahead_.push(t, source, std::move(readback));
    while (auto job = lookahead_.take_job()) {
      jobs_.push_back({*job, clock_.now() + latency_});
    }
    return id;
  }
  void release_due(std::vector<spg::ReleasedFrame>& out) override {
    run_ready();
    const std::size_t before = out.size();
    lookahead_.release_due(out);
    for (std::size_t i = before; i < out.size(); ++i) {
      released[out[i].frame_time] = out[i];
    }
  }
  void mark_discontinuity() override {
    ++discontinuities;
    lookahead_.mark_discontinuity();
  }
  [[nodiscard]] spg::SchedulerStats stats() const override { return lookahead_.stats(); }

  std::vector<Timestamp> scanned;
  std::map<Timestamp, spg::ReleasedFrame> released;
  int discontinuities = 0;

 private:
  void run_ready() {
    while (!jobs_.empty() && jobs_.front().ready <= clock_.now()) {
      const spg::ScanJob job = jobs_.front().job;
      jobs_.erase(jobs_.begin());
      lookahead_.complete(job, spg::run_scan(stages_, job, limits_));
    }
  }
  struct Pending {
    spg::ScanJob job;
    Timestamp ready;
  };
  const spg::ManualClock& clock_;
  spg::Lookahead lookahead_;
  spg::ScanStages stages_;
  spg::ScanLimits limits_;
  spg::Duration latency_;
  std::shared_ptr<int> destroyed_;
  std::vector<Pending> jobs_;
};

spg::SchedulerConfig face_config(FallbackMode fallback = FallbackMode::BlurSource) {
  spg::SchedulerConfig config;
  config.lookahead = 100ms;
  config.retro = 100ms;
  config.tracker.hold = 200ms;
  config.fallback = fallback;
  config.scan.categories = spg::CategorySet{};
  config.scan.categories.insert(spg::Category::Face);
  return config;
}

// A face at `box` (in 1280x720 Source pixels, scaled to the source size)
// while scene time is in [from, to).
std::shared_ptr<spg_test::ScriptedDetector> face_between(Rect box, std::int64_t from_ms, std::int64_t to_ms) {
  return std::make_shared<spg_test::ScriptedDetector>([=](Timestamp t, Size source) {
    std::vector<Detection> out;
    if (t >= at_ms(from_ms) && t < at_ms(to_ms)) {
      const float sx = static_cast<float>(source.width) / static_cast<float>(k720.width);
      const float sy = static_cast<float>(source.height) / static_cast<float>(k720.height);
      out.push_back(spg_test::face_at(box.x * sx, box.y * sy, box.width * sx, box.height * sy));
    }
    return out;
  });
}

bool contains(const Rect& outer, const Rect& inner) {
  return outer.x <= inner.x && outer.y <= inner.y && outer.x + outer.width >= inner.x + inner.width &&
         outer.y + outer.height >= inner.y + inner.height;
}

bool masks(const DrawPlan& plan, const Rect& box) {
  return std::any_of(plan.rects.begin(), plan.rects.end(),
                     [&](const spg::MaskRect& r) { return contains(r.box.rect, box); });
}

// Drives a Pipeline one tick at a time and checks the fail-closed rule on
// every plan: a frame is drawn with only its rects masked only when its
// scheduler released it clean (or it is that generation's re-emitted last
// clean frame).
struct Harness {
  explicit Harness(std::size_t ring = 64)
      : pipeline(gpu, ring, [this](std::unique_ptr<spg::plugin::FrameScheduler> s) { retired.push_back(std::move(s)); }) {}

  SyncScheduler& adopt(spg::SchedulerConfig config, std::shared_ptr<spg::Detector> faces,
                       spg::Duration latency = 30ms) {
    auto scheduler = std::make_unique<SyncScheduler>(clock, config, spg::ScanStages{std::move(faces), nullptr, nullptr, nullptr},
                                                     latency, destroyed);
    SyncScheduler& ref = *scheduler;
    spg::plugin::PipelineSpec spec;
    spec.scheduler = std::move(scheduler);
    spec.fallback = config.fallback;
    spec.hold = config.tracker.hold;
    spec.ring_budget_bytes = budget;
    pipeline.adopt(spec, clock.now());
    schedulers.push_back(&ref);
    return ref;
  }

  void adopt_none(FallbackMode fallback = FallbackMode::BlurSource) {
    spg::plugin::PipelineSpec spec;
    spec.fallback = fallback;
    spec.ring_budget_bytes = budget;
    pipeline.adopt(spec, clock.now());
  }

  const DrawPlan& step(std::int64_t ms, Size size = k720) {
    clock.set(at_ms(ms));
    ++gpu.tick;
    const int captures_before = gpu.captures;
    const DrawPlan& plan = pipeline.tick(at_ms(ms), size);
    if (gpu.captures > captures_before && !gpu.fail_capture && !gpu.throw_capture) {
      // The slot captured this tick is the one acquired last; find it by size
      // bookkeeping: the newest slot not shown before.
      for (const auto& [slot, s] : gpu.slot_size) {
        if (pipeline.ring().holders(slot) > 0 && !slot_time.contains(slot)) {
          slot_time[slot] = at_ms(ms);
        }
      }
    }
    check_fail_closed(plan, at_ms(ms));
    // Free slots lose their recorded time.
    for (auto it = slot_time.begin(); it != slot_time.end();) {
      it = pipeline.ring().holders(it->first) == 0 ? slot_time.erase(it) : std::next(it);
    }
    return plan;
  }

  [[nodiscard]] std::optional<Timestamp> shown_time(const DrawPlan& plan) const {
    const auto it = slot_time.find(plan.slot);
    return it == slot_time.end() ? std::nullopt : std::optional<Timestamp>(it->second);
  }

  void check_fail_closed(const DrawPlan& plan, Timestamp now) {
    if (plan.kind == DrawKind::Nothing || plan.kind == DrawKind::Solid) {
      return;
    }
    INFO("kind " << static_cast<int>(plan.kind) << " cover " << static_cast<int>(plan.cover) << " live " << plan.live
                 << " slot " << plan.slot << " now " << spg::to_ns(now));
    REQUIRE(pipeline.ring().holders(plan.slot) > 0);
    // The GPU side would draw exactly this plan: the slot still holds the
    // capture the plan names, at the plan's size.
    REQUIRE(spg::plugin::checked_kind(plan, gpu.contents(plan.slot)) == plan.kind);
    if (plan.kind == DrawKind::BlurWhole) {
      REQUIRE(plan.cover != Cover::None);
      return;
    }
    // Masked: the frame must have been released clean by some scheduler.
    const std::optional<Timestamp> t = shown_time(plan);
    REQUIRE(t.has_value());
    REQUIRE(*t <= now);
    bool clean = false;
    for (const SyncScheduler* s : schedulers) {
      if (const auto it = s->released.find(*t); it != s->released.end()) {
        clean = clean || it->second.action == spg::FallbackAction::None;
      }
    }
    REQUIRE(clean);
    REQUIRE((plan.cover == Cover::None) != plan.reemit);
  }

  spg::ManualClock clock{at_ms(1000)};
  FakeGpu gpu;
  std::vector<std::unique_ptr<spg::plugin::FrameScheduler>> retired;
  std::shared_ptr<int> destroyed = std::make_shared<int>(0);
  std::uint64_t budget = spg::plugin::kNoRingBudget;  // for the next adopt()
  Pipeline pipeline;
  std::vector<SyncScheduler*> schedulers;
  std::map<std::size_t, Timestamp> slot_time;
};

}  // namespace

TEST_CASE("without a policy every frame is covered whole", "[plugin][pipeline]") {
  Harness h;
  for (std::int64_t t = 1000; t < 1300; t += 10) {
    const DrawPlan& plan = h.step(t);
    CHECK(plan.kind == DrawKind::BlurWhole);
    CHECK(plan.cover == Cover::NoDetection);
    CHECK(plan.live);
    CHECK(plan.frame == k720);
  }
  h.adopt_none(FallbackMode::SolidFill);
  CHECK(h.step(1300).kind == DrawKind::Solid);
  CHECK(h.pipeline.ring().held() == 0);  // nothing is buffered without a scheduler
  CHECK(h.gpu.stage_calls.empty());       // and nothing is read back
}

TEST_CASE("frames air clean only once scanned, with retroactive and held masks", "[plugin][pipeline]") {
  Harness h;
  const Rect face{400, 200, 120, 160};
  h.adopt(face_config(), face_between(face, 1200, 1400));
  int starting = 0;
  int masked = 0;
  for (std::int64_t t = 1000; t < 1800; t += 10) {
    const DrawPlan& plan = h.step(t);
    if (t < 1100) {
      // Nothing is due yet: the live frame is covered whole.
      CHECK(plan.kind == DrawKind::BlurWhole);
      CHECK(plan.cover == Cover::Starting);
      ++starting;
      continue;
    }
    if (plan.kind != DrawKind::Masked) {
      continue;
    }
    ++masked;
    const Timestamp shown = *h.shown_time(plan);
    CHECK(shown <= at_ms(t - 100));  // aired after the lookahead
    // The face is masked from its first frame (retroactively) and for the
    // hold after its last detection.
    if (shown >= at_ms(1200) && shown < at_ms(1400 + 190)) {
      CHECK(masks(plan, face));
    }
  }
  CHECK(starting == 10);
  CHECK(masked > 50);
  // The first frame with the face was released clean and masked.
  const SyncScheduler& s = *h.schedulers.front();
  CHECK(s.released.at(at_ms(1200)).action == spg::FallbackAction::None);
}

TEST_CASE("a readback is pushed one tick late with the time of the frame it shows", "[plugin][pipeline]") {
  Harness h;
  SyncScheduler& s = h.adopt(face_config(), face_between({0, 0, 10, 10}, 0, 0));
  for (std::int64_t t = 1000; t < 1100; t += 10) {
    h.step(t);
  }
  // Staged every tick (cadence 0), alternating surfaces; each mapped next tick.
  REQUIRE(h.gpu.stage_calls.size() == 10);
  for (std::size_t i = 0; i < h.gpu.stage_calls.size(); ++i) {
    CHECK(h.gpu.stage_calls[i] == i % 2);
  }
  REQUIRE(s.scanned.size() == 9);
  for (std::size_t i = 0; i < s.scanned.size(); ++i) {
    CHECK(s.scanned[i] == at_ms(1000 + 10 * static_cast<std::int64_t>(i)));
  }
  CHECK(h.pipeline.counters().readbacks == 9);
}

TEST_CASE("readbacks are scaled to fit the cap and keep the aspect ratio", "[plugin][pipeline]") {
  using spg::plugin::readback_size;
  CHECK(readback_size({1280, 720}, {1920, 1080}) == Size{1280, 720});
  CHECK(readback_size({3840, 2160}, {1920, 1080}) == Size{1920, 1080});
  CHECK(readback_size({2560, 1600}, {1920, 1080}) == Size{1728, 1080});
  CHECK(readback_size({8000, 2}, {1920, 1080}) == Size{1920, 1});
  CHECK(readback_size({0, 720}, {1920, 1080}) == Size{});
}

TEST_CASE("unscanned frames get the configured fallback", "[plugin][pipeline]") {
  const auto failing = std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{}; }, [](Timestamp) { throw std::runtime_error("x"); });
  SECTION("blur the whole source") {
    Harness h;
    h.adopt(face_config(FallbackMode::BlurSource), failing);
    for (std::int64_t t = 1000; t < 1400; t += 10) {
      const DrawPlan& plan = h.step(t);
      CHECK(plan.kind == DrawKind::BlurWhole);
      CHECK(plan.cover == (t < 1100 ? Cover::Starting : Cover::NotScanned));
    }
  }
  SECTION("solid fill") {
    Harness h;
    h.adopt(face_config(FallbackMode::SolidFill), failing);
    for (std::int64_t t = 1000; t < 1400; t += 10) {
      CHECK(h.step(t).kind == DrawKind::Solid);
    }
  }
  SECTION("hold the last scanned frame") {
    Harness h;
    const Rect face{100, 100, 50, 50};
    // Works until scene time 1300, then every cycle fails.
    const auto flaky = std::make_shared<spg_test::ScriptedDetector>(
        [face](Timestamp, Size) { return std::vector<Detection>{spg_test::face_at(face.x, face.y, face.width, face.height)}; },
        [](Timestamp t) {
          if (t >= at_ms(1300)) {
            throw std::runtime_error("x");
          }
        });
    h.adopt(face_config(FallbackMode::HoldLastScanned), flaky);
    std::optional<std::size_t> held_slot;
    int reemits = 0;
    for (std::int64_t t = 1000; t < 1700; t += 10) {
      const DrawPlan& plan = h.step(t);
      if (plan.reemit) {
        ++reemits;
        CHECK(plan.kind == DrawKind::Masked);
        CHECK(plan.cover == Cover::NotScanned);
        CHECK(masks(plan, face));  // shown again exactly as it aired
        if (held_slot) {
          CHECK(plan.slot == *held_slot);  // the same last clean frame each time
        }
        held_slot = plan.slot;
        CHECK(h.pipeline.ring().holders(plan.slot) >= 1);
      }
    }
    CHECK(reemits > 20);
  }
}

TEST_CASE("a resize keeps held masks, scaled", "[plugin][pipeline]") {
  Harness h;
  const Rect face{400, 200, 120, 160};
  // The face is seen until 1300; the source doubles in size at 1250.
  h.adopt(face_config(), face_between(face, 1000, 1300));
  const Size big{2560, 1440};
  const Rect big_face{800, 400, 240, 320};
  for (std::int64_t t = 1000; t < 1700; t += 10) {
    const DrawPlan& plan = h.step(t, t < 1250 ? k720 : big);
    if (plan.kind != DrawKind::Masked) {
      continue;
    }
    const Timestamp shown = *h.shown_time(plan);
    if (shown < at_ms(1250)) {
      CHECK(plan.frame == k720);  // old-size frames keep their own size and masks
      CHECK(masks(plan, face));
    } else if (shown < at_ms(1300 + 190)) {
      CHECK(plan.frame == big);
      CHECK(masks(plan, big_face));  // held past the last detection, scaled
    }
  }
}

TEST_CASE("a 0x0 target draws nothing and drops no mask", "[plugin][pipeline]") {
  Harness h;
  const Rect face{400, 200, 120, 160};
  SyncScheduler& s = h.adopt(face_config(), face_between(face, 1000, 1290));
  for (std::int64_t t = 1000; t < 1300; t += 10) {
    h.step(t);
  }
  const std::size_t held = h.pipeline.ring().held();
  for (std::int64_t t = 1300; t < 1330; t += 10) {
    const DrawPlan& plan = h.step(t, Size{});
    CHECK(plan.kind == DrawKind::Nothing);
  }
  // Buffered frames that came due were dropped unseen; the rest are kept.
  CHECK(h.pipeline.ring().held() + 4 >= held);
  CHECK(h.pipeline.generations() == 1);
  int masked_held = 0;
  int masked_before = 0;
  for (std::int64_t t = 1330; t < 1700; t += 10) {
    const DrawPlan& plan = h.step(t);
    CHECK(plan.kind != DrawKind::Solid);  // every kept frame still has its texture
    if (plan.kind == DrawKind::Masked) {
      const Timestamp shown = *h.shown_time(plan);
      masked_before += shown < at_ms(1300) ? 1 : 0;
      if (shown < at_ms(1280 + 190)) {
        CHECK(masks(plan, face));
        ++masked_held;
      }
    }
  }
  CHECK(s.discontinuities == 1);
  CHECK(masked_before >= 5);  // frames buffered before the 0x0 ticks still air
  CHECK(masked_held > 0);
}

TEST_CASE("a settings change drains the old scheduler and covers the old hold", "[plugin][pipeline]") {
  Harness h;
  const Rect face{400, 200, 120, 160};
  h.adopt(face_config(), face_between(face, 1000, 1300));
  for (std::int64_t t = 1000; t < 1300; t += 10) {
    h.step(t);
  }
  h.adopt(face_config(), face_between(face, 1000, 1300));
  CHECK(h.pipeline.generations() == 2);
  int old_masked = 0;
  int covered = 0;
  for (std::int64_t t = 1300; t < 1800; t += 10) {
    const DrawPlan& plan = h.step(t);
    if (plan.kind == DrawKind::Masked && *h.shown_time(plan) < at_ms(1300)) {
      CHECK(masks(plan, face));  // old frames air with the old masks
      ++old_masked;
    }
    if (plan.cover == Cover::SettingsChanged) {
      CHECK(plan.kind == DrawKind::BlurWhole);
      ++covered;
    }
    if (plan.kind == DrawKind::Masked) {
      // No new-generation frame within the old hold airs with only its rects.
      // (The switch happened at clock 1290 with a 200 ms hold.)
      CHECK((*h.shown_time(plan) < at_ms(1300) || *h.shown_time(plan) >= at_ms(1490)));
    }
  }
  CHECK(old_masked >= 8);
  CHECK(covered > 0);
  // The drained scheduler went to the retire callback, not destroyed inline.
  CHECK(h.pipeline.generations() == 1);
  CHECK(h.retired.size() == 1);
  CHECK(*h.destroyed == 0);

  // Losing the policy covers the live frame at once.
  h.adopt_none();
  const DrawPlan& plan = h.step(1800);
  CHECK(plan.kind == DrawKind::BlurWhole);
  CHECK(plan.cover == Cover::NoDetection);
}

TEST_CASE("a second render of the same frame time changes nothing", "[plugin][pipeline]") {
  Harness h;
  SyncScheduler& s = h.adopt(face_config(), face_between({0, 0, 10, 10}, 0, 0));
  h.step(1000);
  const DrawPlan first = h.pipeline.plan();
  const DrawPlan& again = h.pipeline.tick(at_ms(1000), k720);
  CHECK(again.kind == first.kind);
  CHECK(again.slot == first.slot);
  CHECK(s.stats().frames_pushed == 1);
  CHECK(h.gpu.captures == 1);
}

TEST_CASE("a frame without a texture is covered with a solid fill", "[plugin][pipeline]") {
  SECTION("capture fails") {
    Harness h;
    h.adopt(face_config(), face_between({0, 0, 10, 10}, 0, 0));
    for (std::int64_t t = 1000; t < 1300; t += 10) {
      h.gpu.fail_capture = (t == 1150);
      const DrawPlan& plan = h.step(t);
      if (t == 1250) {
        CHECK(plan.kind == DrawKind::Solid);
        CHECK(plan.cover == Cover::NoTexture);
      }
    }
    CHECK(h.pipeline.counters().capture_failures == 1);
  }
  SECTION("the ring is full") {
    Harness h(4);  // fewer slots than the lookahead buffers
    h.adopt(face_config(), face_between({0, 0, 10, 10}, 0, 0));
    int solid = 0;
    for (std::int64_t t = 1000; t < 1500; t += 10) {
      const DrawPlan& plan = h.step(t);
      CHECK(h.pipeline.ring().held() <= 4);
      solid += plan.kind == DrawKind::Solid ? 1 : 0;
    }
    CHECK(solid > 0);
    CHECK(h.pipeline.counters().capture_failures > 0);
  }
}

namespace {

const std::uint64_t k720Bytes = spg::plugin::texture_bytes(k720, 32);

// Faces from 1100 ms on; every third 100 ms window of scans fails, so clean
// frames, HoldLastScanned re-emits and covered frames all hold ring slots.
std::shared_ptr<spg_test::ScriptedDetector> flaky_face(Rect box) {
  return std::make_shared<spg_test::ScriptedDetector>(
      [box](Timestamp t, Size) {
        std::vector<Detection> out;
        if (t >= at_ms(1100)) {
          out.push_back(spg_test::face_at(box.x, box.y, box.width, box.height));
        }
        return out;
      },
      [](Timestamp t) {
        if ((spg::to_ns(t) / 100'000'000) % 3 == 0) {
          throw std::runtime_error("scan failed");
        }
      });
}

}  // namespace

TEST_CASE("a capture past the VRAM budget gets no texture and the frame is covered", "[plugin][pipeline][budget]") {
  Harness h;
  h.budget = 6 * k720Bytes;  // 100 ms at 10 ms ticks needs 14
  h.adopt(face_config(), face_between({0, 0, 10, 10}, 0, 0));
  int refused_shown = 0;
  for (std::int64_t t = 1000; t < 1500; t += 10) {
    const DrawPlan& plan = h.step(t);  // fail-closed rule checked on every tick
    REQUIRE(h.pipeline.ring_bytes() <= h.budget);
    REQUIRE(h.pipeline.ring().held() <= 6);
    if (plan.cover == Cover::NoTexture) {
      CHECK(plan.kind == DrawKind::Solid);  // never the frame, never sharp
      ++refused_shown;
    }
  }
  CHECK(refused_shown > 0);
  const spg::plugin::PipelineCounters& counters = h.pipeline.counters();
  CHECK(counters.over_budget > 0);
  CHECK(counters.capture_failures >= counters.over_budget);
}

TEST_CASE("at the VRAM budget an idle texture is reused, not freed and recreated", "[plugin][pipeline][budget]") {
  Harness h;
  h.budget = 6 * k720Bytes;  // 100 ms at 10 ms ticks needs 14: the ring runs at the cap
  h.adopt(face_config(), face_between({0, 0, 10, 10}, 0, 0));
  for (std::int64_t t = 1000; t < 1300; t += 10) {
    h.step(t);
  }
  REQUIRE(h.pipeline.ring_bytes() == h.budget);
  const std::size_t freed_before = h.gpu.freed.size();
  const int captures_before = h.gpu.captures;
  for (std::int64_t t = 1300; t < 1800; t += 10) {
    h.step(t);
    REQUIRE(h.pipeline.ring_bytes() <= h.budget);
  }
  CHECK(h.gpu.captures > captures_before);    // frames still get textures
  CHECK(h.gpu.freed.size() == freed_before);  // each into an idle slot's texture
}

TEST_CASE("at the VRAM budget a larger capture frees other idle textures before its own", "[plugin][pipeline][budget]") {
  constexpr Size kSmall{640, 360};  // a quarter of a 720p texture
  const std::uint64_t small_bytes = spg::plugin::texture_bytes(kSmall, 32);
  Harness h(16);
  h.budget = 8 * small_bytes;  // two 720p frames: the one on screen and the capture
  h.adopt(face_config(), face_between({0, 0, 10, 10}, 0, 0));
  for (std::int64_t t = 1000; t < 1300; t += 10) {
    h.step(t, kSmall);  // the lookahead fills the ring
  }
  h.adopt_none();  // the buffered frames drain and their textures go idle
  for (std::int64_t t = 1300; t < 1500; t += 10) {
    h.step(t, kSmall);
  }
  REQUIRE(h.pipeline.ring().held() <= 2);  // the frame on screen (and the one before it)
  REQUIRE(h.pipeline.ring_bytes() > 4 * small_bytes);  // with idle textures
  REQUIRE(h.pipeline.ring_bytes() <= h.budget);
  // A 720p frame does not fit beside every idle small texture: the other idle
  // ones are freed, and the warm one is grown in place, not freed and recreated.
  const std::uint64_t refused = h.pipeline.counters().over_budget;  // while the lookahead filled the ring
  const std::size_t freed_before = h.gpu.freed.size();
  const DrawPlan& first = h.step(1500);
  REQUIRE(first.kind == DrawKind::BlurWhole);  // the live frame, in the slot it was captured to
  const std::vector<std::size_t> freed(h.gpu.freed.begin() + static_cast<std::ptrdiff_t>(freed_before),
                                       h.gpu.freed.end());
  CHECK_FALSE(freed.empty());
  CHECK(std::find(freed.begin(), freed.end(), first.slot) == freed.end());
  REQUIRE(h.pipeline.ring_bytes() <= h.budget);
  CHECK(h.pipeline.counters().over_budget == refused);
  for (std::int64_t t = 1510; t < 1700; t += 10) {
    h.step(t);
    REQUIRE(h.pipeline.ring_bytes() <= h.budget);
  }
}

TEST_CASE("a budget that fits the lookahead never refuses a frame", "[plugin][pipeline][budget]") {
  // ring_frames_needed() against the real pipeline: buffered frames, the
  // capture, the frame on screen and the last clean frame (re-emitted by
  // HoldLastScanned while scans fail) all fit.
  for (const FallbackMode fallback : {FallbackMode::BlurSource, FallbackMode::HoldLastScanned}) {
    INFO("fallback " << static_cast<int>(fallback));
    Harness h;
    h.budget = spg::plugin::ring_frames_needed(100ms, 10ms) * k720Bytes;
    h.adopt(face_config(fallback), flaky_face({400, 200, 120, 160}));
    int reemits = 0;
    for (std::int64_t t = 1000; t < 3000; t += 10) {
      const DrawPlan& plan = h.step(t);
      REQUIRE(h.pipeline.ring_bytes() <= h.budget);
      reemits += plan.reemit ? 1 : 0;
    }
    CHECK(h.pipeline.counters().over_budget == 0);
    CHECK(h.pipeline.counters().capture_failures == 0);
    CHECK((fallback != FallbackMode::HoldLastScanned || reemits > 0));
  }
}

TEST_CASE("a lower budget in a new generation keeps the frames already buffered", "[plugin][pipeline][budget]") {
  Harness h;
  const Rect face{400, 200, 120, 160};
  h.adopt(face_config(), face_between(face, 1000, 10'000));
  for (std::int64_t t = 1000; t < 1200; t += 10) {
    h.step(t);
  }
  REQUIRE(h.pipeline.ring().held() > 4);
  // Settings change: a budget below what is held now and a shorter lookahead.
  h.budget = 4 * k720Bytes;
  spg::SchedulerConfig shorter = face_config();
  shorter.lookahead = 50ms;
  shorter.retro = 50ms;
  h.adopt(shorter, face_between(face, 1000, 10'000));
  int old_frames_aired = 0;
  for (std::int64_t t = 1200; t < 1600; t += 10) {
    const DrawPlan& plan = h.step(t);
    if (plan.kind == DrawKind::Masked && *h.shown_time(plan) < at_ms(1200)) {
      CHECK(masks(plan, face));  // the old generation's frame, with its own masks
      ++old_frames_aired;
    }
  }
  CHECK(old_frames_aired > 0);  // buffered frames were not dropped by the budget
  CHECK(h.pipeline.counters().over_budget > 0);
  CHECK(h.pipeline.ring_bytes() <= h.budget);  // once the old frames are gone
}

TEST_CASE("after a resize the refitted lookahead fits the budget again", "[plugin][pipeline][budget]") {
  constexpr Size k1080{1920, 1080};
  Harness h;
  h.budget = spg::plugin::ring_frames_needed(100ms, 10ms) * k720Bytes;
  const Rect face{400, 200, 120, 160};
  h.adopt(face_config(), face_between(face, 1000, 10'000), 5ms);
  for (std::int64_t t = 1000; t < 1300; t += 10) {
    h.step(t);
  }
  CHECK(h.pipeline.counters().over_budget == 0);
  // The source grows: captures past the budget are refused (covered) until
  // the filter's video tick publishes a generation with the refitted lookahead.
  for (std::int64_t t = 1300; t < 1400; t += 10) {
    h.step(t, k1080);
    REQUIRE(h.pipeline.ring_bytes() <= h.budget);
  }
  CHECK(h.pipeline.counters().over_budget > 0);
  const spg::plugin::LookaheadFit fit = spg::plugin::fit_lookahead(
      100ms, h.budget, k1080, spg::plugin::texture_bytes(k1080, h.gpu.bits_per_pixel()), 10ms);
  REQUIRE(fit.shortened);
  spg::SchedulerConfig refit = face_config();
  refit.lookahead = fit.lookahead;
  refit.retro = std::min(refit.retro, fit.lookahead);
  h.adopt(refit, face_between(face, 1000, 10'000), 5ms);
  std::uint64_t refused = 0;
  int masked = 0;
  for (std::int64_t t = 1400; t < 2400; t += 10) {
    const DrawPlan& plan = h.step(t, k1080);
    REQUIRE(h.pipeline.ring_bytes() <= h.budget);
    if (t == 1700) {
      refused = h.pipeline.counters().over_budget;  // the old generation has drained
    }
    masked += plan.kind == DrawKind::Masked ? 1 : 0;
  }
  CHECK(h.pipeline.counters().over_budget == refused);  // steady state: nothing refused
  CHECK(masked > 50);  // frames scanned within the shorter lookahead air with their masks
}

TEST_CASE("random stalls and failures never air an unscanned frame", "[plugin][pipeline]") {
  // Harness::check_fail_closed runs on every tick.
  for (std::uint32_t seed = 1; seed <= 40; ++seed) {
    std::mt19937 rng(seed);
    Harness h(32);
    const Rect face{300, 300, 90, 90};
    const auto detector = std::make_shared<spg_test::ScriptedDetector>(
        [face](Timestamp, Size s) {
          const float max_w = static_cast<float>(s.width) / 4.0F;
          const float max_h = static_cast<float>(s.height) / 4.0F;
          return std::vector<Detection>{
              spg_test::face_at(face.x, face.y, std::min(face.width, max_w), std::min(face.height, max_h))};
        },
        [seed](Timestamp t) {
          if ((spg::to_ns(t) / 1'000'000 + seed) % 7 == 0) {
            throw std::runtime_error("x");
          }
        });
    const FallbackMode modes[] = {FallbackMode::BlurSource, FallbackMode::HoldLastScanned, FallbackMode::SolidFill};
    const FallbackMode mode = modes[seed % 3];
    const auto latency = [&rng] { return std::chrono::milliseconds(10 + static_cast<std::int64_t>(rng() % 150)); };
    h.adopt(face_config(mode), detector, latency());
    std::int64_t t = 1000;
    for (int i = 0; i < 150; ++i) {
      t += 5 + static_cast<std::int64_t>(rng() % 30);
      const auto roll = static_cast<std::uint32_t>(rng() % 100);
      Size size = k720;
      if (roll < 3) {
        size = Size{};
      } else if (roll < 6) {
        size = Size{1920, 1080};
      }
      h.gpu.fail_capture = roll >= 95;
      if (roll == 50) {
        h.adopt(face_config(mode), detector, latency());
      }
      const DrawPlan& plan = h.step(t, size);
      if (plan.kind == DrawKind::Masked && plan.cover == Cover::None) {
        CHECK(!plan.rects.empty());
      }
      CHECK(h.pipeline.ring().held() <= 32);
    }
  }
}

// --- State drift: the render side must never draw from state that no longer
// matches what the scheduler and the GPU hold.

namespace {

// A scheduler whose releases the test writes by hand.
class ScriptedScheduler final : public spg::plugin::FrameScheduler {
 public:
  struct Pushed {
    spg::FrameId id = 0;
    Timestamp t{};
    Size source;
  };
  [[nodiscard]] bool wants_scan(Timestamp) const override { return false; }
  spg::FrameId push(Timestamp t, Size source, std::shared_ptr<const spg::OwnedFrame>) override {
    pushed.push_back(Pushed{++next_id, t, source});
    return next_id;
  }
  void release_due(std::vector<spg::ReleasedFrame>& out) override {
    // Moved, not copied: a test can fail one allocation in the pipeline.
    out.insert(out.end(), std::make_move_iterator(queue.begin()), std::make_move_iterator(queue.end()));
    queue.clear();
  }
  void mark_discontinuity() override { ++discontinuities; }
  [[nodiscard]] spg::SchedulerStats stats() const override { return {}; }

  std::vector<Pushed> pushed;
  std::vector<spg::ReleasedFrame> queue;
  int discontinuities = 0;
  spg::FrameId next_id = 0;
};

// A clean release of `p` with one mask.
spg::ReleasedFrame clean_release(const ScriptedScheduler::Pushed& p) {
  spg::ReleasedFrame r;
  r.id = p.id;
  r.frame_time = p.t;
  r.source = p.source;
  r.plan.frame_time = p.t;
  r.plan.rects.push_back(spg::MaskRect{spg::Box{Rect{10, 10, 50, 50}, spg::Space::Source}, spg::MaskStyle::Blur});
  return r;
}

struct ScriptedRig {
  ScriptedRig() {
    auto owned = std::make_unique<ScriptedScheduler>();
    scheduler = owned.get();
    spg::plugin::PipelineSpec spec;
    spec.scheduler = std::move(owned);
    spec.fallback = FallbackMode::HoldLastScanned;
    pipeline.adopt(spec, at_ms(1000));
  }
  const DrawPlan& tick(std::int64_t ms, Size size = k720) { return pipeline.tick(at_ms(ms), size); }

  FakeGpu gpu;
  std::vector<std::unique_ptr<spg::plugin::FrameScheduler>> retired;
  Pipeline pipeline{gpu, 16, [this](std::unique_ptr<spg::plugin::FrameScheduler> s) { retired.push_back(std::move(s)); }};
  ScriptedScheduler* scheduler = nullptr;
};

// Adopts a new ScriptedScheduler generation at `ms`.
ScriptedScheduler* adopt_scripted(Pipeline& pipeline, std::int64_t ms) {
  auto owned = std::make_unique<ScriptedScheduler>();
  ScriptedScheduler* out = owned.get();
  spg::plugin::PipelineSpec spec;
  spec.scheduler = std::move(owned);
  spec.fallback = FallbackMode::HoldLastScanned;
  pipeline.adopt(spec, at_ms(ms));
  return out;
}

}  // namespace

TEST_CASE("the draw decision rechecks the slot snapshot", "[plugin][pipeline][drift]") {
  using spg::plugin::checked_kind;
  using spg::plugin::SlotContents;
  DrawPlan plan;
  plan.kind = DrawKind::Masked;
  plan.slot = 3;
  plan.serial = 42;
  plan.frame = k720;
  plan.rects.push_back(spg::MaskRect{spg::Box{Rect{1, 2, 30, 40}, spg::Space::Source}, spg::MaskStyle::Blur});
  const SlotContents good{true, 42, k720};
  CHECK(checked_kind(plan, good) == DrawKind::Masked);
  CHECK(checked_kind(plan, SlotContents{}) == DrawKind::Solid);                  // texture missing
  CHECK(checked_kind(plan, SlotContents{true, 43, k720}) == DrawKind::Solid);    // recycled for another frame
  CHECK(checked_kind(plan, SlotContents{true, 0, k720}) == DrawKind::Solid);     // capture failed
  CHECK(checked_kind(plan, SlotContents{true, 42, {640, 360}}) == DrawKind::Solid);  // another size
  DrawPlan blurred = plan;
  blurred.kind = DrawKind::BlurWhole;
  CHECK(checked_kind(blurred, SlotContents{true, 7, k720}) == DrawKind::Solid);
  // A mask that cannot be drawn: the frame is blurred whole, never partially masked.
  DrawPlan bad = plan;
  bad.rects.push_back(spg::MaskRect{spg::Box{Rect{std::nanf(""), 0, 5, 5}, spg::Space::Source}, spg::MaskStyle::Blur});
  CHECK(checked_kind(bad, good) == DrawKind::BlurWhole);
  DrawPlan wrong_space = plan;
  wrong_space.rects[0].box.space = spg::Space::Readback;
  CHECK(checked_kind(wrong_space, good) == DrawKind::BlurWhole);
  DrawPlan solid;
  solid.kind = DrawKind::Solid;
  CHECK(checked_kind(solid, SlotContents{}) == DrawKind::Solid);
}

TEST_CASE("a recaptured slot is never drawn as the frame a plan names", "[plugin][pipeline][drift]") {
  ScriptedRig rig;
  rig.tick(1000);
  rig.scheduler->queue.push_back(clean_release(rig.scheduler->pushed[0]));
  const DrawPlan plan = rig.tick(1010);
  REQUIRE(plan.kind == DrawKind::Masked);
  CHECK(spg::plugin::checked_kind(plan, rig.gpu.contents(plan.slot)) == DrawKind::Masked);
  // The slot's texture is overwritten by another capture behind the plan's back.
  rig.gpu.slot_serial[plan.slot] = 999;
  CHECK(spg::plugin::checked_kind(plan, rig.gpu.contents(plan.slot)) == DrawKind::Solid);
  rig.gpu.slot_serial.erase(plan.slot);
  CHECK(spg::plugin::checked_kind(plan, rig.gpu.contents(plan.slot)) == DrawKind::Solid);
}

TEST_CASE("a release that does not match its buffered frame is covered", "[plugin][pipeline][drift]") {
  ScriptedRig rig;
  rig.tick(1000);
  rig.tick(1010);
  const ScriptedScheduler::Pushed first = rig.scheduler->pushed[0];
  spg::ReleasedFrame r = clean_release(first);
  SECTION("clean action on a plan marked fallback") { r.plan.fallback = true; }
  SECTION("clean action with a fallback reason") { r.reason = spg::FallbackReason::NotScanned; }
  SECTION("another frame time") { r.frame_time = at_ms(1005); }
  SECTION("another source size") { r.source = Size{640, 360}; }
  SECTION("an id never pushed") { r.id = 77; }
  rig.scheduler->queue.push_back(r);
  const DrawPlan& plan = rig.tick(1020);
  CHECK(plan.kind == DrawKind::Solid);
  CHECK(plan.cover == Cover::Inconsistent);
}

TEST_CASE("an old frame is not repeated past the repeat limit", "[plugin][pipeline][drift]") {
  ScriptedRig rig;
  rig.tick(1000);
  rig.scheduler->queue.push_back(clean_release(rig.scheduler->pushed[0]));
  CHECK(rig.tick(1010).kind == DrawKind::Masked);
  // The scheduler stops releasing (a stall); the clean frame repeats briefly,
  // then the live frame is covered whole.
  std::int64_t t = 1020;
  for (; t <= 1110; t += 10) {
    CHECK(rig.tick(t).kind == DrawKind::Masked);
  }
  const DrawPlan& plan = rig.tick(t);
  CHECK(plan.kind == DrawKind::BlurWhole);
  CHECK(plan.cover == Cover::Stalled);
  CHECK(plan.live);
}

TEST_CASE("a gap or a backwards step in frame time is a discontinuity", "[plugin][pipeline][drift]") {
  ScriptedRig rig;
  rig.tick(1000);
  rig.scheduler->queue.push_back(clean_release(rig.scheduler->pushed[0]));
  REQUIRE(rig.tick(1010).kind == DrawKind::Masked);
  // The filter was disabled (no renders) for 300 ms.
  rig.tick(1310);
  CHECK(rig.scheduler->discontinuities == 1);
  // A HoldLastScanned release naming the frame from before the gap is not
  // re-emitted: content may have changed while nothing was rendered.
  spg::ReleasedFrame hold;
  const ScriptedScheduler::Pushed& p = rig.scheduler->pushed[1];
  hold.id = p.id;
  hold.frame_time = p.t;
  hold.source = p.source;
  hold.plan.fallback = true;
  hold.action = spg::FallbackAction::HoldLastScanned;
  hold.reason = spg::FallbackReason::NotScanned;
  hold.reemit = rig.scheduler->pushed[0].id;
  rig.scheduler->queue.push_back(hold);
  const DrawPlan& plan = rig.tick(1320);
  CHECK_FALSE(plan.reemit);
  CHECK(plan.kind == DrawKind::BlurWhole);
  rig.tick(1300);  // backwards
  CHECK(rig.scheduler->discontinuities == 2);
}

TEST_CASE("a GPU call that throws covers the frame and leaks nothing", "[plugin][pipeline][drift]") {
  ScriptedRig rig;
  rig.gpu.throw_capture = true;
  const DrawPlan& plan = rig.tick(1000);
  CHECK(plan.kind == DrawKind::Solid);
  CHECK(rig.pipeline.ring().held() == 0);
  rig.gpu.throw_capture = false;
  rig.tick(1010);
  rig.scheduler->queue.push_back(clean_release(rig.scheduler->pushed[0]));  // the frame without a texture
  const DrawPlan& next = rig.tick(1020);
  CHECK(next.kind == DrawKind::Solid);
  CHECK(next.cover == Cover::NoTexture);
}

TEST_CASE("an older generation never airs once a newer one has", "[plugin][pipeline][drift]") {
  ScriptedRig rig;
  rig.tick(1000);
  rig.tick(1010);
  ScriptedScheduler* old = rig.scheduler;
  // The new settings have a shorter lookahead: the new generation's first
  // frame comes due on the same tick as the old one's first.
  ScriptedScheduler* fresh = adopt_scripted(rig.pipeline, 1015);
  rig.tick(1020);
  old->queue.push_back(clean_release(old->pushed[0]));
  fresh->queue.push_back(clean_release(fresh->pushed[0]));
  const DrawPlan aired = rig.tick(1030);
  REQUIRE(aired.kind == DrawKind::Masked);
  // A skipped render leaves the new generation nothing due on the next tick
  // while the old one's next (older) frame comes due: it must not air.
  old->queue.push_back(clean_release(old->pushed[1]));
  const DrawPlan& next = rig.tick(1040);
  CHECK(next.kind == DrawKind::Masked);
  CHECK(next.slot == aired.slot);
  CHECK(next.serial == aired.serial);
  // The old generation is retired at once, its buffered frame's slot freed.
  CHECK(rig.pipeline.generations() == 1);
  REQUIRE(rig.retired.size() == 1);
  CHECK(rig.retired[0].get() == old);
}

TEST_CASE("repeated settings changes keep at most two generations live", "[plugin][pipeline][drift]") {
  SECTION("nothing aired yet: the newest two are kept") {
    ScriptedRig rig;
    rig.tick(1000);  // every generation buffers a frame and none drains
    for (std::int64_t i = 0; i < 5; ++i) {
      adopt_scripted(rig.pipeline, 1005 + 10 * i);
      CHECK(rig.pipeline.generations() <= Pipeline::kMaxGenerations);
      rig.tick(1010 + 10 * i);
      CHECK(rig.pipeline.generations() <= Pipeline::kMaxGenerations);
    }
    CHECK(rig.retired.size() == 4);
    // Held slots: the two live generations' buffered frames (one is on screen).
    CHECK(rig.pipeline.ring().held() == 2);
  }
  SECTION("the generation on screen is kept, the one between is retired") {
    ScriptedRig rig;
    rig.tick(1000);
    rig.tick(1010);
    ScriptedScheduler* first = rig.scheduler;
    first->queue.push_back(clean_release(first->pushed[0]));
    REQUIRE(rig.tick(1020).kind == DrawKind::Masked);
    ScriptedScheduler* second = adopt_scripted(rig.pipeline, 1025);
    rig.tick(1030);
    adopt_scripted(rig.pipeline, 1035);
    CHECK(rig.pipeline.generations() == 2);
    REQUIRE(rig.retired.size() == 1);
    CHECK(rig.retired[0].get() == second);
    // The first generation still airs its buffered frames.
    first->queue.push_back(clean_release(first->pushed[1]));
    const DrawPlan& plan = rig.tick(1040);
    CHECK(plan.kind == DrawKind::Masked);
    CHECK(plan.cover == Cover::None);
  }
}

TEST_CASE("frames released and replaced within one tick are not counted as aired", "[plugin][pipeline]") {
  ScriptedRig rig;
  rig.tick(1000);
  rig.tick(1010);
  rig.tick(1020);
  const auto& shown = rig.pipeline.counters().shown;
  CHECK(shown[static_cast<std::size_t>(Cover::Starting)] == 3);
  // Rendering paused: three frames come due on one tick; one output frame airs.
  for (const ScriptedScheduler::Pushed& p : rig.scheduler->pushed) {
    rig.scheduler->queue.push_back(clean_release(p));
  }
  REQUIRE(rig.tick(1030).kind == DrawKind::Masked);
  CHECK(shown[static_cast<std::size_t>(Cover::None)] == 1);
  CHECK(rig.pipeline.counters().released == 3);
  // A tick that only repeats the plan is not counted.
  rig.tick(1040);
  CHECK(shown[static_cast<std::size_t>(Cover::None)] == 1);
}

TEST_CASE("a release whose masks cannot be copied leaks no slot", "[plugin][pipeline][drift]") {
  ScriptedRig rig;
  rig.tick(1000);
  rig.tick(1010);
  REQUIRE(rig.pipeline.ring().held() == 2);  // two buffered frames, one on screen
  spg::ReleasedFrame r = clean_release(rig.scheduler->pushed[0]);
  const spg::MaskRect one = r.plan.rects.front();
  r.plan.rects.assign(20'000, one);
  rig.scheduler->queue.push_back(std::move(r));
  spg_test::fail_allocations_from(sizeof(spg::MaskRect) * 10'000);
  CHECK_THROWS_AS(rig.tick(1020), std::bad_alloc);
  spg_test::allow_allocations();
  // The released frame's slot went back to the ring; frames 2 and 3 are held.
  CHECK(rig.pipeline.ring().held() == 2);
  rig.scheduler->queue.push_back(clean_release(rig.scheduler->pushed[1]));
  CHECK(rig.tick(1030).kind == DrawKind::Masked);
}

// --- The hand-off between the settings side and the render thread.

namespace {

struct HostRig {
  HostRig()
      : host(gpu, 32, [this](std::unique_ptr<spg::plugin::FrameScheduler> s) {
          const std::lock_guard lock(mutex);
          retired.push_back(std::move(s));
        }) {}

  std::unique_ptr<spg::plugin::PipelineSpec> spec(std::shared_ptr<spg::Detector> faces,
                                                  FallbackMode fallback = FallbackMode::BlurSource) {
    auto out = std::make_unique<spg::plugin::PipelineSpec>();
    spg::SchedulerConfig config = face_config(fallback);
    out->scheduler = std::make_unique<SyncScheduler>(clock, config, spg::ScanStages{std::move(faces), nullptr, nullptr, nullptr},
                                                     30ms, nullptr);
    out->fallback = fallback;
    out->hold = config.tracker.hold;
    return out;
  }
  static std::unique_ptr<spg::plugin::PipelineSpec> none(FallbackMode fallback = FallbackMode::BlurSource) {
    auto out = std::make_unique<spg::plugin::PipelineSpec>();
    out->fallback = fallback;
    return out;
  }
  const DrawPlan& frame(std::int64_t ms, Size size = k720) {
    clock.set(at_ms(ms));
    ++gpu.tick;
    return host.frame(at_ms(ms), size);
  }

  spg::ManualClock clock{at_ms(1000)};
  FakeGpu gpu;
  std::mutex mutex;
  std::vector<std::unique_ptr<spg::plugin::FrameScheduler>> retired;
  spg::plugin::PipelineHost host;
};

}  // namespace

TEST_CASE("the host covers until a generation is adopted and when a build fails", "[plugin][pipeline][drift]") {
  HostRig rig;
  const Rect face{400, 200, 120, 160};
  // Before any settings arrive: covered whole.
  CHECK(rig.frame(1000).cover == Cover::NoDetection);
  rig.host.offer(rig.spec(face_between(face, 0, 10'000)));
  int masked = 0;
  for (std::int64_t t = 1010; t < 1300; t += 10) {
    masked += rig.frame(t).kind == DrawKind::Masked ? 1 : 0;
  }
  CHECK(masked > 10);
  // A failed build for new settings: every frame is solid, even though the
  // old scheduler could still release clean frames, and a second render of
  // the same frame time gets the same answer.
  rig.host.fail();
  for (std::int64_t t = 1300; t < 1400; t += 10) {
    CHECK(rig.frame(t).kind == DrawKind::Solid);
    CHECK(rig.host.frame(at_ms(t), k720).kind == DrawKind::Solid);
  }
  // The policy becomes empty (a storage error): covered whole at once.
  rig.host.offer(HostRig::none());
  for (std::int64_t t = 1400; t < 1700; t += 10) {
    const DrawPlan& plan = rig.frame(t);
    CHECK(plan.kind == DrawKind::BlurWhole);
    CHECK(plan.cover == Cover::NoDetection);
  }
}

TEST_CASE("a generation that cannot be adopted is retried, never skipped", "[plugin][pipeline][drift]") {
  HostRig rig;
  const Rect face{400, 200, 120, 160};
  rig.host.offer(rig.spec(face_between(face, 0, 10'000)));
  for (std::int64_t t = 1000; t < 1200; t += 10) {
    rig.frame(t);
  }
  REQUIRE(rig.host.status().scanning);
  // The next settings remove the policy, but adopting them fails once.
  int failures = 1;
  rig.host.set_adopt_fault([&failures] {
    if (failures-- > 0) {
      throw std::bad_alloc();
    }
  });
  rig.host.offer(HostRig::none());
  const DrawPlan& failed = rig.frame(1200);
  CHECK(failed.kind == DrawKind::Solid);  // not the old policy's next frame
  CHECK(failed.cover == Cover::Inconsistent);
  CHECK(rig.host.frame(at_ms(1200), k720).kind == DrawKind::Solid);
  const DrawPlan& adopted = rig.frame(1210);
  CHECK(adopted.cover == Cover::NoDetection);
  CHECK(adopted.kind == DrawKind::BlurWhole);
  CHECK_FALSE(rig.host.status().scanning);

  // The same with a generation that has a scheduler: after a failed attempt
  // the next frame adopts it whole, scheduler included.
  failures = 1;
  const auto failing = std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{}; }, [](Timestamp) { throw std::runtime_error("x"); });
  rig.host.offer(rig.spec(failing, FallbackMode::SolidFill));
  CHECK(rig.frame(1220).kind == DrawKind::Solid);
  rig.frame(1230);
  CHECK(rig.host.status().scanning);
  for (std::int64_t t = 1240; t < 1500; t += 10) {
    const DrawPlan& plan = rig.frame(t);
    if (t >= 1340) {
      CHECK(plan.kind == DrawKind::Solid);
      CHECK(plan.cover == Cover::NotScanned);  // released by the new scheduler
    }
  }
}

TEST_CASE("a generation offered twice before a frame retires the first", "[plugin][pipeline][drift]") {
  HostRig rig;
  rig.host.offer(rig.spec(face_between({0, 0, 10, 10}, 0, 0)));
  rig.host.offer(HostRig::none(FallbackMode::SolidFill));
  CHECK(rig.retired.size() == 1);
  CHECK(rig.frame(1000).kind == DrawKind::Solid);  // the second (solid fill, no policy)
}

TEST_CASE("the host reports frames refused by the VRAM budget", "[plugin][pipeline][budget]") {
  HostRig rig;
  auto spec = rig.spec(face_between({0, 0, 10, 10}, 0, 0));
  spec->ring_budget_bytes = k720Bytes - 1;  // not even one frame
  rig.host.offer(std::move(spec));
  for (std::int64_t t = 1000; t < 1300; t += 10) {
    const DrawPlan& plan = rig.frame(t);
    CHECK(plan.kind != DrawKind::Masked);  // nothing fits: every frame is covered
  }
  const spg::plugin::RenderStatus status = rig.host.status();
  CHECK(status.over_budget > 0);
  CHECK(status.clean == 0);
  CHECK(rig.host.pipeline().ring_bytes() == 0);
}

TEST_CASE("settings published while frames render never draw an unscanned frame", "[plugin][pipeline][drift]") {
  HostRig rig;
  const auto failing = std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{}; }, [](Timestamp) { throw std::runtime_error("x"); });
  std::vector<std::unique_ptr<spg::plugin::PipelineSpec>> specs;
  for (int i = 0; i < 200; ++i) {
    specs.push_back(i % 3 == 0 ? HostRig::none() : rig.spec(failing));
  }
  std::atomic<bool> done{false};
  std::thread publisher([&] {
    for (auto& s : specs) {
      rig.host.offer(std::move(s));
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    rig.host.fail();
    done.store(true);
  });
  std::int64_t t = 1000;
  // Every cycle fails, so nothing may ever be drawn with only its masks.
  while (!done.load()) {
    t += 10;
    CHECK(rig.frame(t).kind != DrawKind::Masked);
  }
  publisher.join();
  CHECK(rig.frame(t + 10).kind == DrawKind::Solid);
}

// --- Building generations off the thread that asks for them (SPG-14d).

namespace {

using spg::plugin::GenerationBuilder;
using Kind = GenerationBuilder::Kind;
using SpecPtr = std::unique_ptr<spg::plugin::PipelineSpec>;

// A builder on HostRig's host whose executor only queues: the test decides
// when (and on which thread) each build runs.
struct BuildRig : HostRig {
  BuildRig()
      : builder(
            host,
            [this](std::unique_ptr<spg::plugin::FrameScheduler> s) {
              const std::lock_guard guard(mutex);
              retired.push_back(std::move(s));
            },
            lock,
            [this](GenerationBuilder::Task task) {
              if (refuse_posts) {
                throw std::bad_alloc();
              }
              tasks.push_back(std::move(task));
            }) {}

  bool request(Kind kind, std::function<SpecPtr()> build) {
    const std::lock_guard guard(lock);
    return builder.request(
        kind,
        [this, build] {
          ++builds;
          return build();
        },
        [this](GenerationBuilder::Outcome outcome) { outcomes.push_back(outcome); });
  }
  // Runs the queued builds, as the build thread would.
  void run_builds() {
    std::vector<GenerationBuilder::Task> now;
    now.swap(tasks);
    for (auto& task : now) {
      task();
    }
  }
  template <typename F>
  auto locked(F f) {
    const std::lock_guard guard(lock);
    return f(builder);
  }
  std::function<SpecPtr()> faces(std::shared_ptr<spg::Detector> detector) {
    return [this, detector] { return spec(detector); };
  }

  std::mutex lock;  // the caller's lock (FilterRender::publish_mutex_)
  std::vector<GenerationBuilder::Task> tasks;
  bool refuse_posts = false;
  int builds = 0;
  std::vector<GenerationBuilder::Outcome> outcomes;
  GenerationBuilder builder;
};

const Rect kFace{400, 200, 120, 160};

int count_masked(BuildRig& rig, std::int64_t from, std::int64_t to) {
  int masked = 0;
  for (std::int64_t t = from; t < to; t += 10) {
    masked += rig.frame(t).kind == DrawKind::Masked ? 1 : 0;
  }
  return masked;
}

}  // namespace

TEST_CASE("a settings change covers until its generation is built", "[plugin][pipeline][build]") {
  BuildRig rig;
  REQUIRE(rig.request(Kind::Settings, rig.faces(face_between(kFace, 0, 10'000))));
  CHECK(rig.builds == 0);  // asking builds nothing on the caller
  const DrawPlan& waiting = rig.frame(1000);
  CHECK(waiting.kind == DrawKind::Solid);
  CHECK(waiting.cover == Cover::SettingsChanged);
  rig.run_builds();
  REQUIRE(rig.builds == 1);
  REQUIRE(rig.outcomes.size() == 1);
  CHECK(rig.outcomes[0].ok);
  CHECK(count_masked(rig, 1010, 1300) > 10);

  // New settings while the running generation still masks the face: every
  // frame is covered from the request until the new generation is ready,
  // since the old one scans with the old settings.
  REQUIRE(rig.request(Kind::Settings, rig.faces(face_between(kFace, 0, 10'000))));
  for (std::int64_t t = 1300; t < 1400; t += 10) {
    const DrawPlan& plan = rig.frame(t);
    CHECK(plan.kind == DrawKind::Solid);
    CHECK(plan.cover == Cover::SettingsChanged);
    CHECK(rig.host.frame(at_ms(t), k720).kind == DrawKind::Solid);  // a second view: the same answer
  }
  rig.run_builds();
  CHECK(rig.builds == 2);
  CHECK(rig.outcomes.back().ok);
  CHECK(count_masked(rig, 1400, 2000) > 10);
  CHECK(rig.host.status().scanning);
  CHECK(rig.host.pipeline().generations() <= Pipeline::kMaxGenerations);
  CHECK_FALSE(rig.locked([](GenerationBuilder& b) { return b.pending() || b.failed(); }));
}

TEST_CASE("a rebuild keeps the running generation until the new one is ready", "[plugin][pipeline][build]") {
  BuildRig rig;
  REQUIRE(rig.request(Kind::Settings, rig.faces(face_between(kFace, 0, 10'000))));
  rig.run_builds();
  REQUIRE(count_masked(rig, 1000, 1300) > 10);
  // A refit or the models becoming ready: the same settings, so the running
  // generation keeps scanning while the new one is built.
  REQUIRE(rig.request(Kind::Rebuild, rig.faces(face_between(kFace, 0, 10'000))));
  CHECK(rig.locked([](GenerationBuilder& b) { return b.pending(); }));
  for (std::int64_t t = 1300; t < 1400; t += 10) {
    const DrawPlan& plan = rig.frame(t);
    CHECK(plan.kind == DrawKind::Masked);
    CHECK(plan.cover == Cover::None);
  }
  rig.run_builds();
  CHECK(rig.builds == 2);
  CHECK(rig.outcomes.back().ok);
  // The switch follows the usual rules (old hold covered), then masks again.
  CHECK(count_masked(rig, 1400, 2000) > 10);
  CHECK(rig.host.pipeline().generations() <= Pipeline::kMaxGenerations);
}

TEST_CASE("only the latest request is ever handed over", "[plugin][pipeline][build]") {
  BuildRig rig;
  int built_a = 0;
  rig.request(Kind::Settings, [&] {
    ++built_a;
    return rig.spec(face_between(kFace, 0, 10'000));
  });
  rig.request(Kind::Settings, rig.faces(face_between(kFace, 0, 10'000)));
  rig.run_builds();
  CHECK(built_a == 0);  // superseded before it started: never built
  CHECK(rig.builds == 1);
  CHECK(rig.outcomes.size() == 1);

  // Superseded while it builds (a publish arrives meanwhile): its scheduler
  // is retired, never offered, and the cover stays until the newer one.
  rig.request(Kind::Settings, [&] {
    rig.request(Kind::Settings, rig.faces(face_between(kFace, 0, 10'000)));
    return rig.spec(face_between(kFace, 0, 10'000));
  });
  const std::size_t retired_before = rig.retired.size();
  const std::size_t outcomes_before = rig.outcomes.size();
  rig.run_builds();
  CHECK(rig.retired.size() == retired_before + 1);
  CHECK(rig.outcomes.size() == outcomes_before);  // nothing to report for it
  CHECK(rig.frame(1000).cover == Cover::SettingsChanged);
  rig.run_builds();
  CHECK(rig.outcomes.size() == outcomes_before + 1);
  CHECK(count_masked(rig, 1010, 1400) > 10);
}

TEST_CASE("requests waiting for the build thread are replaced, not queued", "[plugin][pipeline][build]") {
  BuildRig rig;
  std::weak_ptr<int> first_alive;
  {
    // Stands for the first request's copy of the settings (the watch-list).
    const auto settings = std::make_shared<int>(1);
    first_alive = settings;
    rig.request(Kind::Settings, [&rig, settings] { return rig.spec(face_between(kFace, 0, 10'000)); });
  }
  CHECK_FALSE(first_alive.expired());  // waiting for the build thread
  CHECK(rig.tasks.size() == 1);
  for (int i = 0; i < 50; ++i) {
    rig.request(i % 2 == 0 ? Kind::Settings : Kind::Rebuild, rig.faces(face_between(kFace, 0, 10'000)));
  }
  CHECK(first_alive.expired());  // superseded: freed at once, never built
  CHECK(rig.tasks.size() == 1);  // one task, for the latest request
  rig.run_builds();
  CHECK(rig.builds == 1);
  REQUIRE(rig.outcomes.size() == 1);
  CHECK(rig.outcomes[0].ok);

  // Requests made while a build runs post one more task between them.
  rig.request(Kind::Settings, [&rig] {
    for (int i = 0; i < 20; ++i) {
      rig.request(Kind::Settings, rig.faces(face_between(kFace, 0, 10'000)));
    }
    return rig.spec(face_between(kFace, 0, 10'000));
  });
  rig.run_builds();
  CHECK(rig.tasks.size() == 1);
  CHECK(rig.outcomes.size() == 1);  // the running build was superseded: retired, not offered
  rig.run_builds();
  CHECK(rig.builds == 3);
  CHECK(rig.outcomes.size() == 2);
  CHECK(rig.outcomes.back().ok);
  CHECK(count_masked(rig, 1000, 1400) > 10);
  CHECK(rig.host.pipeline().generations() <= Pipeline::kMaxGenerations);
}

TEST_CASE("a failed build covers, and only a rebuild of settings built once is retried", "[plugin][pipeline][build]") {
  BuildRig rig;
  const auto throwing = [] () -> SpecPtr { throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again)); };
  // A settings change whose generation cannot be built: covered, not retried.
  rig.request(Kind::Settings, throwing);
  rig.run_builds();
  REQUIRE(rig.outcomes.size() == 1);
  CHECK_FALSE(rig.outcomes[0].ok);
  CHECK(rig.outcomes[0].first_failure);
  CHECK(rig.frame(1000).kind == DrawKind::Solid);
  CHECK(rig.frame(1010).cover == Cover::NoDetection);
  CHECK(rig.locked([](GenerationBuilder& b) { return b.failed() && !b.may_rebuild(); }));
  CHECK_FALSE(rig.builder.take_retry());

  // The next settings build: scanning again.
  rig.request(Kind::Settings, rig.faces(face_between(kFace, 0, 10'000)));
  rig.run_builds();
  CHECK(rig.outcomes.back().ok);
  CHECK(count_masked(rig, 1020, 1300) > 10);

  // A rebuild of those settings fails: covered at once, and worth retrying.
  rig.request(Kind::Rebuild, throwing);
  rig.run_builds();
  CHECK_FALSE(rig.outcomes.back().ok);
  CHECK(rig.outcomes.back().first_failure);
  for (std::int64_t t = 1300; t < 1400; t += 10) {
    CHECK(rig.frame(t).kind == DrawKind::Solid);
  }
  CHECK(rig.locked([](GenerationBuilder& b) { return b.failed() && b.may_rebuild(); }));
  CHECK(rig.builder.take_retry());
  CHECK_FALSE(rig.builder.take_retry());  // once per failure

  // Retried and failing again: still covered, not reported as new.
  rig.request(Kind::Rebuild, throwing);
  CHECK(rig.frame(1400).kind == DrawKind::Solid);  // still covered while it is pending
  rig.run_builds();
  CHECK_FALSE(rig.outcomes.back().ok);
  CHECK_FALSE(rig.outcomes.back().first_failure);
  CHECK(rig.builder.take_retry());

  // Retried and built: scanning again.
  rig.request(Kind::Rebuild, rig.faces(face_between(kFace, 0, 10'000)));
  rig.run_builds();
  CHECK(rig.outcomes.back().ok);
  CHECK(count_masked(rig, 1410, 2000) > 10);
  CHECK_FALSE(rig.locked([](GenerationBuilder& b) { return b.failed(); }));
}

TEST_CASE("a build that cannot be posted or prepared fails closed", "[plugin][pipeline][build]") {
  BuildRig rig;
  rig.request(Kind::Settings, rig.faces(face_between(kFace, 0, 10'000)));
  rig.run_builds();
  REQUIRE(count_masked(rig, 1000, 1300) > 10);
  // The executor refuses (allocation): failed at once, done reported.
  rig.refuse_posts = true;
  CHECK_FALSE(rig.request(Kind::Settings, rig.faces(face_between(kFace, 0, 10'000))));
  rig.refuse_posts = false;
  REQUIRE(rig.outcomes.size() == 2);
  CHECK_FALSE(rig.outcomes[1].ok);
  CHECK(rig.frame(1300).kind == DrawKind::Solid);
  CHECK(rig.builds == 1);

  // The caller could not prepare its request: a pending build for earlier
  // settings must not be offered after it.
  rig.request(Kind::Settings, rig.faces(face_between(kFace, 0, 10'000)));
  const GenerationBuilder::Outcome failed = rig.locked([](GenerationBuilder& b) { return b.fail(Kind::Settings); });
  CHECK_FALSE(failed.ok);
  rig.run_builds();
  CHECK(rig.builds == 1);  // never built
  for (std::int64_t t = 1310; t < 1500; t += 10) {
    const DrawPlan& plan = rig.frame(t);
    CHECK(plan.kind == DrawKind::Solid);
    CHECK(plan.cover == Cover::NoDetection);
  }
}

TEST_CASE("the task thread runs tasks in order and drops the rest on stop", "[plugin][pipeline][build]") {
  spg::plugin::TaskThread thread;
  std::mutex m;
  std::condition_variable cv;
  std::vector<int> order;
  std::vector<std::thread::id> where;
  bool gate = false;
  thread.post([&] {
    std::unique_lock guard(m);
    cv.wait_for(guard, 5s, [&] { return gate; });
    order.push_back(1);
    where.push_back(std::this_thread::get_id());
  });
  for (int i = 2; i <= 4; ++i) {
    thread.post([&, i] {
      const std::lock_guard guard(m);
      order.push_back(i);
      where.push_back(std::this_thread::get_id());
    });
  }
  {
    const std::lock_guard guard(m);
    gate = true;
  }
  cv.notify_all();
  const auto until = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < until) {
    {
      const std::lock_guard guard(m);
      if (order.size() == 4) {
        break;
      }
    }
    std::this_thread::sleep_for(1ms);
  }
  {
    const std::lock_guard guard(m);
    CHECK(order == std::vector<int>{1, 2, 3, 4});
    for (const std::thread::id id : where) {
      CHECK(id == thread.id());
      CHECK(id != std::this_thread::get_id());
    }
  }
  // A task still running when stop() is called finishes; queued ones never run.
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
  std::atomic<int> ran{0};
  thread.post([&] {
    started.store(true);
    while (!release.load()) {
      std::this_thread::sleep_for(1ms);
    }
    ++ran;
  });
  thread.post([&] { ran += 100; });
  const auto start_by = std::chrono::steady_clock::now() + 5s;
  while (!started.load() && std::chrono::steady_clock::now() < start_by) {
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(started.load());
  std::thread stopper([&] { thread.stop(); });
  // Release the running task only once stop() has taken the queue: post()
  // refuses from then on (a no-op posted before that is dropped or runs).
  const auto stop_by = std::chrono::steady_clock::now() + 5s;
  bool stopping = false;
  while (!stopping && std::chrono::steady_clock::now() < stop_by) {
    try {
      thread.post([] {});
      std::this_thread::sleep_for(1ms);
    } catch (const std::logic_error&) {
      stopping = true;
    }
  }
  CHECK(stopping);
  release.store(true);
  stopper.join();
  CHECK(ran.load() == 1);
  CHECK_THROWS(thread.post([] {}));
}

TEST_CASE("generations are built on the build thread, never on the caller", "[plugin][pipeline][build]") {
  HostRig rig;
  std::mutex lock;
  // Built here, handed out by the build: the rig's clock is not thread-safe.
  // The next case constructs a real scheduler inside the build.
  auto ready = std::make_shared<SpecPtr>(rig.spec(face_between(kFace, 0, 10'000)));
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool gate = false;
  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
  std::thread::id built_on;
  bool ok = false;
  spg::plugin::TaskThread thread;
  GenerationBuilder builder(
      rig.host, [&rig](std::unique_ptr<spg::plugin::FrameScheduler> s) {
        const std::lock_guard guard(rig.mutex);
        rig.retired.push_back(std::move(s));
      },
      lock, [&thread](GenerationBuilder::Task task) { thread.post(std::move(task)); });
  {
    const std::lock_guard guard(lock);
    REQUIRE(builder.request(
        Kind::Settings,
        [&, ready] {
          built_on = std::this_thread::get_id();
          started.store(true);
          // Held until the test has rendered frames while the build runs.
          std::unique_lock held(gate_mutex);
          gate_cv.wait_for(held, 10s, [&] { return gate; });
          return std::move(*ready);
        },
        [&](GenerationBuilder::Outcome outcome) {
          ok = outcome.ok;  // with `lock` held, on the build thread
          finished.store(true);
        }));
  }
  const auto start_by = std::chrono::steady_clock::now() + 5s;
  while (!started.load() && std::chrono::steady_clock::now() < start_by) {
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(started.load());
  // The build is running and cannot finish: every frame is covered.
  std::int64_t t = 1000;
  for (; t < 1300; t += 10) {
    const DrawPlan& plan = rig.frame(t);
    CHECK(plan.kind == DrawKind::Solid);
    CHECK(plan.cover == Cover::SettingsChanged);
  }
  CHECK_FALSE(finished.load());
  {
    const std::lock_guard guard(gate_mutex);
    gate = true;
  }
  gate_cv.notify_all();
  const auto done_by = std::chrono::steady_clock::now() + 5s;
  while (!finished.load() && std::chrono::steady_clock::now() < done_by) {
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(finished.load());
  {
    const std::lock_guard guard(lock);
    CHECK(ok);
    CHECK(built_on == thread.id());
    CHECK(built_on != std::this_thread::get_id());
  }
  int masked = 0;
  for (const std::int64_t end = t + 300; t < end; t += 10) {
    masked += rig.frame(t).kind == DrawKind::Masked ? 1 : 0;
  }
  CHECK(masked > 10);
  thread.stop();
}

namespace {

// A stage that records the thread that constructed it.
class ThreadTaggedDetector final : public spg::Detector {
 public:
  ThreadTaggedDetector() : made_on(std::this_thread::get_id()) {}
  [[nodiscard]] std::vector<Detection> detect(const spg::FrameView& /*readback*/, Size /*source*/) const override {
    return {};
  }
  const std::thread::id made_on;
};

}  // namespace

TEST_CASE("the scheduler and its stages are constructed on the build thread", "[plugin][pipeline][build]") {
  static const spg::SteadyClock clock;  // thread-safe, unlike the rig's; outlives the scheduler
  HostRig rig;
  std::mutex lock;
  std::atomic<bool> finished{false};
  bool ok = false;
  std::shared_ptr<const ThreadTaggedDetector> stage;
  std::thread::id scheduler_made_on;
  spg::plugin::TaskThread thread;
  GenerationBuilder builder(
      rig.host, [&rig](std::unique_ptr<spg::plugin::FrameScheduler> s) {
        const std::lock_guard guard(rig.mutex);
        rig.retired.push_back(std::move(s));
      },
      lock, [&thread](GenerationBuilder::Task task) { thread.post(std::move(task)); });
  {
    const std::lock_guard guard(lock);
    // As FilterRender's build does: the stages and the ThreadedScheduler
    // (worker threads) are made inside the build.
    REQUIRE(builder.request(
        Kind::Settings,
        [&] {
          auto detector = std::make_shared<const ThreadTaggedDetector>();
          auto spec = std::make_unique<spg::plugin::PipelineSpec>();
          const spg::SchedulerConfig config = face_config();
          spec->fallback = FallbackMode::BlurSource;
          spec->hold = config.tracker.hold;
          spec->scheduler = std::make_unique<spg::plugin::ThreadedScheduler>(
              clock, config, spg::ScanStages{detector, nullptr, nullptr, nullptr});
          scheduler_made_on = std::this_thread::get_id();
          stage = std::move(detector);
          return spec;
        },
        [&](GenerationBuilder::Outcome outcome) {
          ok = outcome.ok;
          finished.store(true);
        }));
  }
  const auto done_by = std::chrono::steady_clock::now() + 10s;
  while (!finished.load() && std::chrono::steady_clock::now() < done_by) {
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(finished.load());
  {
    const std::lock_guard guard(lock);
    CHECK(ok);
    REQUIRE(stage != nullptr);
    CHECK(stage->made_on == thread.id());
    CHECK(stage->made_on != std::this_thread::get_id());
    CHECK(scheduler_made_on == thread.id());
  }
  // The render side only swaps the ready generation in.
  CHECK_FALSE(rig.host.status().scanning);
  static_cast<void>(rig.frame(1000));
  CHECK(rig.host.status().scanning);
  thread.stop();
}

TEST_CASE("builds racing with frames never air an unscanned frame", "[plugin][pipeline][build]") {
  HostRig rig;
  std::mutex lock;
  const auto failing = std::make_shared<spg_test::ScriptedDetector>(
      [](Timestamp, Size) { return std::vector<Detection>{}; }, [](Timestamp) { throw std::runtime_error("x"); });
  // Built here: the rig's clock is not thread-safe.
  std::vector<std::shared_ptr<SpecPtr>> specs;
  for (int i = 0; i < 150; ++i) {
    specs.push_back(std::make_shared<SpecPtr>(i % 3 == 0 ? HostRig::none() : rig.spec(failing)));
  }
  spg::plugin::TaskThread thread;
  GenerationBuilder builder(
      rig.host, [&rig](std::unique_ptr<spg::plugin::FrameScheduler> s) {
        const std::lock_guard guard(rig.mutex);
        rig.retired.push_back(std::move(s));
      },
      lock, [&thread](GenerationBuilder::Task task) { thread.post(std::move(task)); });
  std::atomic<bool> done{false};
  std::thread publisher([&] {
    for (std::size_t i = 0; i < specs.size(); ++i) {
      const auto spec = specs[i];
      const bool fails = i % 7 == 3;
      {
        const std::lock_guard guard(lock);
        static_cast<void>(builder.request(i % 2 == 0 ? Kind::Settings : Kind::Rebuild,
                                          [spec, fails]() -> SpecPtr {
                                            if (fails) {
                                              throw std::bad_alloc();
                                            }
                                            return std::move(*spec);
                                          },
                                          {}));
      }
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    done.store(true);
  });
  std::int64_t t = 1000;
  // Every cycle fails, so nothing may ever be drawn with only its masks,
  // and builds never leave more than two schedulers live.
  while (!done.load()) {
    t += 10;
    CHECK(rig.frame(t).kind != DrawKind::Masked);
    CHECK(rig.host.pipeline().generations() <= Pipeline::kMaxGenerations);
  }
  publisher.join();
  thread.stop();
  for (int i = 0; i < 20; ++i) {
    t += 10;
    CHECK(rig.frame(t).kind != DrawKind::Masked);
    CHECK(rig.host.pipeline().generations() <= Pipeline::kMaxGenerations);
  }
}

TEST_CASE("the frame ring counts holders and trims idle textures", "[plugin][pipeline]") {
  spg::plugin::FrameRing ring(3);
  const auto a = ring.acquire();
  const auto b = ring.acquire();
  const auto c = ring.acquire();
  REQUIRE((a && b && c));
  CHECK_FALSE(ring.acquire().has_value());
  ring.retain(*a);
  ring.release(*a);
  CHECK(ring.holders(*a) == 1);
  ring.release(*a);
  ring.release(*a);  // a double release is ignored
  CHECK(ring.holders(*a) == 0);
  CHECK(ring.held() == 2);
  CHECK(ring.peek() == a);
  CHECK(ring.acquire() == a);  // the warm slot is reused first
  ring.release(*a);
  ring.release(*b);
  ring.release(*c);
  const std::vector<std::size_t> freed = ring.trim(1);
  CHECK(freed.size() == 2);
  CHECK(ring.trim(1).empty());
  CHECK(ring.held() == 0);
}

TEST_CASE("readback buffers are copied tightly and reused", "[plugin][pipeline]") {
  spg::plugin::ReadbackPool pool(2);
  std::vector<std::uint8_t> mapped(20 * 3, 0);
  for (std::size_t i = 0; i < mapped.size(); ++i) {
    mapped[i] = static_cast<std::uint8_t>(i);
  }
  auto first = pool.copy_bgra(mapped.data(), 20, Size{4, 3}, at_ms(5));
  REQUIRE(first != nullptr);
  CHECK(first->view().strides[0] == 16);
  CHECK(first->view().timestamp == at_ms(5));
  CHECK(first->view().planes[0][16] == 20);  // row 1 starts at the next linesize
  const std::uint8_t* storage = first->view().planes[0];
  first.reset();
  CHECK(pool.idle() == 1);
  auto second = pool.copy_bgra(mapped.data(), 20, Size{4, 3}, at_ms(6));
  CHECK(second->view().planes[0] == storage);  // reused
  CHECK(pool.allocations() == 1);
  // A different size does not reuse a buffer of the old size.
  second.reset();
  auto third = pool.copy_bgra(mapped.data(), 20, Size{2, 3}, at_ms(7));
  CHECK(pool.allocations() == 2);
  CHECK(pool.copy_bgra(nullptr, 20, Size{4, 3}, at_ms(8)) == nullptr);
  CHECK(pool.copy_bgra(mapped.data(), 8, Size{4, 3}, at_ms(8)) == nullptr);
  CHECK(pool.copy_bgra(mapped.data(), 20, Size{}, at_ms(8)) == nullptr);
  // A frame may outlive its pool (a worker still scanning it).
  std::shared_ptr<const spg::OwnedFrame> survivor;
  {
    spg::plugin::ReadbackPool local(1);
    survivor = local.copy_bgra(mapped.data(), 20, Size{4, 3}, at_ms(9));
  }
  CHECK(survivor->view().planes[0][16] == 20);
}

TEST_CASE("mask regions cover their rect at the chosen level", "[plugin][pipeline]") {
  using spg::plugin::level_size;
  using spg::plugin::mask_level;
  using spg::plugin::mask_region;
  using spg::plugin::top_level;
  using spg::plugin::whole_level;
  CHECK(level_size({1920, 1080}, 1) == Size{960, 540});
  CHECK(level_size({1921, 1081}, 1) == Size{961, 541});
  CHECK(level_size({5, 3}, 3) == Size{1, 1});
  CHECK(top_level({1920, 1080}) == 11);
  CHECK(top_level({1, 1}) == 0);
  CHECK(whole_level({1920, 1080}) == 7);   // cells of 128 px: >= 1080 / 16
  CHECK(whole_level({1280, 720}) == 6);    // >= 45 px
  CHECK(mask_level({0, 0, 30, 12}, {1920, 1080}) == 3);  // the 8 px floor
  CHECK(mask_level({0, 0, 300, 120}, {1920, 1080}) == 6);  // >= 40 px
  CHECK_FALSE(mask_region({-50, 10, 10, 10}, {100, 100}, 2).has_value());
  CHECK_FALSE(mask_region({10, 10, 0, 10}, {100, 100}, 2).has_value());

  std::mt19937 rng(7);
  for (int i = 0; i < 2000; ++i) {
    const auto next = [&rng](std::uint32_t n) { return static_cast<std::uint32_t>(rng() % n); };
    const Size frame{1 + next(4000), 1 + next(2500)};
    const float x = static_cast<float>(static_cast<int>(next(frame.width + 200)) - 100);
    const float y = static_cast<float>(static_cast<int>(next(frame.height + 200)) - 100);
    const Rect rect{x, y, 1.0F + static_cast<float>(next(600)), 1.0F + static_cast<float>(next(600))};
    const std::uint32_t level = mask_level(rect, frame);
    CHECK(level <= top_level(frame));
    const auto region = mask_region(rect, frame, level);
    const double cx0 = std::clamp<double>(rect.x, 0, frame.width);
    const double cx1 = std::clamp<double>(rect.x + rect.width, 0, frame.width);
    const double cy0 = std::clamp<double>(rect.y, 0, frame.height);
    const double cy1 = std::clamp<double>(rect.y + rect.height, 0, frame.height);
    if (cx1 <= cx0 || cy1 <= cy0) {
      CHECK_FALSE(region.has_value());
      continue;
    }
    REQUIRE(region.has_value());
    const Size texels = level_size(frame, level);
    // Normalized, the drawn region contains the clipped rect.
    CHECK(static_cast<double>(region->x) / texels.width <= cx0 / frame.width + 1e-9);
    CHECK(static_cast<double>(region->x + region->width) / texels.width >= cx1 / frame.width - 1e-9);
    CHECK(static_cast<double>(region->y) / texels.height <= cy0 / frame.height + 1e-9);
    CHECK(static_cast<double>(region->y + region->height) / texels.height >= cy1 / frame.height - 1e-9);
    CHECK(region->x + region->width <= texels.width);
    CHECK(region->y + region->height <= texels.height);
  }
}

TEST_CASE("the reaper destroys schedulers off the calling thread and joins on shutdown", "[plugin][pipeline]") {
  // A destructor that waits for workers: here, until the test opens `gate`.
  static std::atomic<bool> gate{false};
  struct Probe final : spg::plugin::FrameScheduler {
    explicit Probe(std::thread::id* where) : where_(where) {}
    ~Probe() override {
      const auto until = std::chrono::steady_clock::now() + 5s;
      while (!gate.load() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(1ms);
      }
      *where_ = std::this_thread::get_id();
    }
    [[nodiscard]] bool wants_scan(Timestamp) const override { return false; }
    spg::FrameId push(Timestamp, Size, std::shared_ptr<const spg::OwnedFrame>) override { return 0; }
    void release_due(std::vector<spg::ReleasedFrame>&) override {}
    void mark_discontinuity() override {}
    [[nodiscard]] spg::SchedulerStats stats() const override { return {}; }
    std::thread::id* where_;
  };
  std::array<std::thread::id, 3> where{};
  spg::plugin::Reaper reaper;
  const auto start = std::chrono::steady_clock::now();
  reaper.retire(std::make_unique<Probe>(&where[0]));
  reaper.retire(std::make_unique<Probe>(&where[1]));
  CHECK(std::chrono::steady_clock::now() - start < 2s);  // retire does not wait for the gate
  CHECK(reaper.pending() == 2);
  gate.store(true);
  reaper.shutdown();
  CHECK(reaper.pending() == 0);
  CHECK(where[0] != std::thread::id{});
  CHECK(where[0] != std::this_thread::get_id());
  CHECK(where[1] == where[0]);
  // After shutdown, a retired scheduler is destroyed on the caller.
  reaper.retire(std::make_unique<Probe>(&where[2]));
  CHECK(where[2] == std::this_thread::get_id());
}

TEST_CASE("the reaper keeps a scheduler queued when its thread cannot start", "[plugin][pipeline]") {
  struct Probe final : spg::plugin::FrameScheduler {
    explicit Probe(std::thread::id* where) : where_(where) {}
    ~Probe() override { *where_ = std::this_thread::get_id(); }
    [[nodiscard]] bool wants_scan(Timestamp) const override { return false; }
    spg::FrameId push(Timestamp, Size, std::shared_ptr<const spg::OwnedFrame>) override { return 0; }
    void release_due(std::vector<spg::ReleasedFrame>&) override {}
    void mark_discontinuity() override {}
    [[nodiscard]] spg::SchedulerStats stats() const override { return {}; }
    std::thread::id* where_;
  };
  SECTION("the next retire starts it") {
    std::array<std::thread::id, 2> where{};
    spg::plugin::Reaper reaper;
    int failures = 1;
    reaper.set_start_fault([&failures] {
      if (failures-- > 0) {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
      }
    });
    reaper.retire(std::make_unique<Probe>(&where[0]));
    CHECK(where[0] == std::thread::id{});  // not destroyed on the caller
    CHECK(reaper.pending() == 1);
    reaper.retire(std::make_unique<Probe>(&where[1]));
    reaper.shutdown();
    CHECK(reaper.pending() == 0);
    CHECK(where[0] != std::thread::id{});
    CHECK(where[0] != std::this_thread::get_id());
    CHECK(where[1] == where[0]);
  }
  SECTION("shutdown destroys what never left") {
    std::thread::id where{};
    spg::plugin::Reaper reaper;
    reaper.set_start_fault([] { throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again)); });
    reaper.retire(std::make_unique<Probe>(&where));
    CHECK(where == std::thread::id{});
    reaper.shutdown();
    CHECK(where == std::this_thread::get_id());
    CHECK(reaper.pending() == 0);
  }
}

// Not run by ctest ([.] hides it): render-thread cost of the scheduler calls
// with real worker threads, a 1920x1080 readback copied into the pool every
// frame at 60 fps, and a detector that takes 15 ms and returns 40 boxes (so
// workers hold the lock to apply results while the render thread calls in).
// Run: obs-spg-pipeline-tests "[bench]". Numbers are for docs/agent-log.md.
TEST_CASE("render-thread cost of push and release_due", "[.][bench]") {
  const auto busy = std::make_shared<spg_test::ScriptedDetector>([](Timestamp, Size) {
    std::this_thread::sleep_for(15ms);
    std::vector<Detection> out;
    for (int i = 0; i < 40; ++i) {
      out.push_back(spg_test::face_at(static_cast<float>(40 * i), static_cast<float>(20 * i), 60, 60));
    }
    return out;
  });
  spg::SteadyClock clock;
  spg::SchedulerConfig config = face_config();
  config.lookahead = 500ms;
  config.retro = 500ms;
  config.tracker.hold = 750ms;
  spg::plugin::ThreadedScheduler scheduler(clock, config, spg::ScanStages{busy, nullptr, nullptr, nullptr});
  spg::plugin::ReadbackPool pool;
  const Size readback{1920, 1080};
  std::vector<std::uint8_t> mapped(static_cast<std::size_t>(readback.width) * 4 * readback.height, 0x55);
  std::vector<double> push_us;
  std::vector<double> release_us;
  std::vector<spg::ReleasedFrame> out;
  Timestamp previous{};
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 600; ++i) {
    std::this_thread::sleep_until(start + i * std::chrono::microseconds(16'667));
    const Timestamp t = clock.now();
    const auto a = std::chrono::steady_clock::now();
    std::shared_ptr<const spg::OwnedFrame> frame;
    if (i > 0 && scheduler.wants_scan(previous)) {
      frame = pool.copy_bgra(mapped.data(), readback.width * 4, readback, previous);
    }
    scheduler.push(t, Size{1920, 1080}, std::move(frame));
    const auto b = std::chrono::steady_clock::now();
    out.clear();
    scheduler.release_due(out);
    const auto c = std::chrono::steady_clock::now();
    push_us.push_back(std::chrono::duration<double, std::micro>(b - a).count());
    release_us.push_back(std::chrono::duration<double, std::micro>(c - b).count());
    previous = t;
  }
  const auto report = [](const char* name, std::vector<double> v) {
    std::sort(v.begin(), v.end());
    std::printf("%s: p50 %.1f us, p99 %.1f us, max %.1f us (%zu calls)\n", name, v[v.size() / 2],
                v[v.size() * 99 / 100], v.back(), v.size());
  };
  report("readback copy + push", push_us);
  report("release_due", release_us);
  std::printf("pool allocations: %llu, scans completed: %llu\n",
              static_cast<unsigned long long>(pool.allocations()),
              static_cast<unsigned long long>(scheduler.stats().scans_completed));
  CHECK(pool.allocations() <= 4);
}
