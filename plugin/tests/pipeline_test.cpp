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
#include <cstdint>
#include <cstdio>
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

#include <cstdlib>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "pipeline/frame_ring.hpp"
#include "pipeline/host.hpp"
#include "pipeline/mask_levels.hpp"
#include "pipeline/pipeline.hpp"
#include "pipeline/readback_pool.hpp"
#include "pipeline/reaper.hpp"
#include "schedule_fakes.hpp"
#include "spg/lookahead.hpp"
#include "spg/scan.hpp"

// Allocations of at least this many bytes throw std::bad_alloc while a test
// sets it; nothing else changes about operator new.
namespace {
std::atomic<std::size_t> g_fail_alloc_from{SIZE_MAX};
}  // namespace

void* operator new(std::size_t size) {
  if (size >= g_fail_alloc_from.load(std::memory_order_relaxed)) {
    throw std::bad_alloc();
  }
  if (void* p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t /*size*/) noexcept {
  std::free(p);
}

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
    pipeline.adopt(spec, clock.now());
    schedulers.push_back(&ref);
    return ref;
  }

  void adopt_none(FallbackMode fallback = FallbackMode::BlurSource) {
    spg::plugin::PipelineSpec spec;
    spec.fallback = fallback;
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
  g_fail_alloc_from.store(sizeof(spg::MaskRect) * 10'000);
  CHECK_THROWS_AS(rig.tick(1020), std::bad_alloc);
  g_fail_alloc_from.store(SIZE_MAX);
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
