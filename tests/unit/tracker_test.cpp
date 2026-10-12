// Tracker and temporal hold (SPG-09): unit tests and seeded property tests.
// Every input is synthetic; time comes from an injected ManualClock.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "spg/detection.hpp"
#include "spg/error.hpp"
#include "spg/geometry.hpp"
#include "spg/policy_config.hpp"
#include "spg/time.hpp"
#include "spg/tracker.hpp"

namespace {

using std::chrono::milliseconds;
using spg::Box;
using spg::Detection;
using spg::DetectionKind;
using spg::Duration;
using spg::ManualClock;
using spg::Rect;
using spg::Size;
using spg::Space;
using spg::Timestamp;
using spg::Tracker;
using spg::TrackerConfig;

Timestamp at_ms(std::int64_t ms) {
  return spg::timestamp_from_ns(ms * 1'000'000);
}

Detection face(float x, float y, float w, float h) {
  return Detection{Box{Rect{x, y, w, h}, Space::Source}, DetectionKind::Face, 0.9F,
                   spg::DetectorId::Synthetic};
}

Detection text(float x, float y, float w, float h) {
  Detection d = face(x, y, w, h);
  d.kind = DetectionKind::TextRegion;
  return d;
}

// Required coverage of `box` under `config`, computed independently in double:
// every edge grown by margin_px + margin_ratio * min(width, height).
struct Region {
  double x0, y0, x1, y1;
};

Region required(const Rect& box, const TrackerConfig& config) {
  const double m = static_cast<double>(config.margin_px) +
                   static_cast<double>(config.margin_ratio) *
                       std::min(static_cast<double>(box.width), static_cast<double>(box.height));
  return {static_cast<double>(box.x) - m, static_cast<double>(box.y) - m,
          static_cast<double>(box.x) + static_cast<double>(box.width) + m,
          static_cast<double>(box.y) + static_cast<double>(box.height) + m};
}

bool rect_contains(const Rect& r, const Region& g) {
  return static_cast<double>(r.x) <= g.x0 && static_cast<double>(r.y) <= g.y0 &&
         static_cast<double>(r.x) + static_cast<double>(r.width) >= g.x1 &&
         static_cast<double>(r.y) + static_cast<double>(r.height) >= g.y1;
}

Region region_of(const Rect& r) {
  return {static_cast<double>(r.x), static_cast<double>(r.y),
          static_cast<double>(r.x) + static_cast<double>(r.width),
          static_cast<double>(r.y) + static_cast<double>(r.height)};
}

bool covered(const std::vector<Box>& masks, const Region& g) {
  return std::any_of(masks.begin(), masks.end(),
                     [&](const Box& b) { return rect_contains(b.rect, g); });
}

// Every rect of `before` is contained in some rect of `after`.
bool no_shrink(const std::vector<Box>& before, const std::vector<Box>& after) {
  return std::all_of(before.begin(), before.end(),
                     [&](const Box& b) { return covered(after, region_of(b.rect)); });
}

void require_source_boxes(const std::vector<Box>& masks) {
  for (const Box& b : masks) {
    REQUIRE(b.space == Space::Source);
    REQUIRE(spg::is_valid(b.rect));
    // Whole pixels.
    REQUIRE(std::floor(b.rect.x) == b.rect.x);
    REQUIRE(std::floor(b.rect.y) == b.rect.y);
    REQUIRE(std::floor(b.rect.width) == b.rect.width);
    REQUIRE(std::floor(b.rect.height) == b.rect.height);
  }
}

}  // namespace

TEST_CASE("tracker: config comes from the policy and is validated", "[tracker]") {
  spg::PolicyConfig policy;
  policy.hold = milliseconds{900};
  policy.margin_px = 2.0F;
  policy.margin_ratio = 0.5F;
  policy.lookahead = milliseconds{300};
  policy.retro = milliseconds{300};
  const TrackerConfig config = TrackerConfig::from_policy(policy);
  CHECK(config.hold == milliseconds{900});
  CHECK(config.margin_px == 2.0F);
  CHECK(config.margin_ratio == 0.5F);
  CHECK(config.retain == milliseconds{300});
  CHECK_NOTHROW(spg::validate(config));

  // The default tracker config matches the default policy.
  CHECK(TrackerConfig::from_policy(spg::PolicyConfig{}) == TrackerConfig{});

  const auto rejects = [](auto mutate) {
    TrackerConfig c;
    mutate(c);
    CHECK_THROWS_AS(spg::validate(c), spg::ContractError);
    ManualClock clock;
    CHECK_THROWS_AS(Tracker(clock, c), spg::ContractError);
  };
  rejects([](TrackerConfig& c) { c.hold = milliseconds{-1}; });
  rejects([](TrackerConfig& c) { c.retain = milliseconds{-1}; });
  rejects([](TrackerConfig& c) { c.max_prediction = milliseconds{-1}; });
  rejects([](TrackerConfig& c) { c.margin_px = -1.0F; });
  rejects([](TrackerConfig& c) { c.margin_px = std::nanf(""); });
  rejects([](TrackerConfig& c) { c.margin_ratio = -0.1F; });
  rejects([](TrackerConfig& c) { c.margin_ratio = INFINITY; });
  rejects([](TrackerConfig& c) { c.iou_threshold = 0.0F; });
  rejects([](TrackerConfig& c) { c.iou_threshold = 1.5F; });
  rejects([](TrackerConfig& c) { c.velocity_smoothing = 0.0F; });
  rejects([](TrackerConfig& c) { c.velocity_smoothing = std::nanf(""); });

  // Zero hold and margin are accepted here (a floor is #28).
  TrackerConfig zero;
  zero.hold = Duration::zero();
  zero.margin_px = 0.0F;
  zero.margin_ratio = 0.0F;
  CHECK_NOTHROW(spg::validate(zero));
}

TEST_CASE("tracker: a detection holds with its margin for exactly the hold", "[tracker]") {
  ManualClock clock(at_ms(1000));
  const TrackerConfig config;  // 750 ms hold, 4 px + 0.25 * min(w, h)
  Tracker tracker(clock, config);
  const Detection d = face(100.0F, 200.0F, 40.0F, 80.0F);
  tracker.update(at_ms(1000), std::vector<Detection>{d});

  // Margin 4 + 0.25 * 40 = 14 px on every edge, nothing moving.
  const std::vector<Box> now = tracker.masks(at_ms(1000));
  REQUIRE(now.size() == 1);
  CHECK(now[0].rect == Rect{86.0F, 186.0F, 68.0F, 108.0F});
  require_source_boxes(now);

  CHECK(covered(tracker.masks(at_ms(1400)), required(d.box.rect, config)));
  CHECK(covered(tracker.masks(at_ms(1750)), required(d.box.rect, config)));
  // After the hold the mask is gone; before the frame it was found on there
  // is none either (retroactive masking is the scheduler's job, SPG-10).
  CHECK(tracker.masks(at_ms(1750) + Duration{1}).empty());
  CHECK(tracker.masks(at_ms(1000) - Duration{1}).empty());

  const std::vector<spg::Track> tracks = tracker.tracks();
  REQUIRE(tracks.size() == 1);
  CHECK(tracks[0].kind == DetectionKind::Face);
  CHECK(tracks[0].box == d.box);
  CHECK(tracks[0].last_confirmed == at_ms(1000));
  CHECK(tracks[0].hold_until == at_ms(1750));
}

TEST_CASE("tracker: zero hold and margin cover only the detection frame", "[tracker]") {
  ManualClock clock;
  TrackerConfig config;
  config.hold = Duration::zero();
  config.margin_px = 0.0F;
  config.margin_ratio = 0.0F;
  Tracker tracker(clock, config);
  tracker.update(at_ms(0), std::vector<Detection>{face(10.0F, 20.0F, 30.0F, 40.0F)});
  const std::vector<Box> masks = tracker.masks(at_ms(0));
  REQUIRE(masks.size() == 1);
  CHECK(masks[0].rect == Rect{10.0F, 20.0F, 30.0F, 40.0F});
  CHECK(tracker.masks(at_ms(0) + Duration{1}).empty());
}

TEST_CASE("tracker: IoU association follows a moving box and estimates its velocity", "[tracker]") {
  ManualClock clock;
  Tracker tracker(clock, TrackerConfig{});
  // 300 px/s to the right, 150 px/s down, detected every 100 ms.
  for (int i = 0; i < 6; ++i) {
    clock.set(at_ms(100 * i));
    const float x = 200.0F + 30.0F * static_cast<float>(i);
    const float y = 100.0F + 15.0F * static_cast<float>(i);
    tracker.update(at_ms(100 * i), std::vector<Detection>{face(x, y, 120.0F, 120.0F)});
  }
  const std::vector<spg::Track> tracks = tracker.tracks();
  REQUIRE(tracks.size() == 1);
  CHECK(tracks[0].id == 1);
  CHECK(std::abs(tracks[0].velocity.dx_per_s - 300.0F) < 1.0F);
  CHECK(std::abs(tracks[0].velocity.dy_per_s - 150.0F) < 1.0F);
  CHECK(tracks[0].box.rect == Rect{350.0F, 175.0F, 120.0F, 120.0F});
}

TEST_CASE("tracker: constant-velocity prediction covers the box ahead, capped", "[tracker]") {
  ManualClock clock;
  TrackerConfig config;
  config.margin_px = 0.0F;
  config.margin_ratio = 0.0F;
  Tracker tracker(clock, config);
  tracker.update(at_ms(0), std::vector<Detection>{face(100.0F, 100.0F, 50.0F, 50.0F)});
  tracker.update(at_ms(100), std::vector<Detection>{face(120.0F, 100.0F, 50.0F, 50.0F)});
  REQUIRE(tracker.tracks().size() == 1);
  // Velocity 200 px/s. 200 ms after the last detection the box is predicted
  // 40 px further; the mask is the hull of the box and the predicted box.
  const std::vector<Box> later = tracker.masks(at_ms(300));
  CHECK(covered(later, {120.0, 100.0, 210.0, 150.0}));
  // The prediction stops at max_prediction (500 ms, 100 px) and holds there.
  const std::vector<Box> capped = tracker.masks(at_ms(850));
  CHECK(covered(capped, {120.0, 100.0, 270.0, 150.0}));
  CHECK_FALSE(covered(capped, {120.0, 100.0, 271.0, 150.0}));
  CHECK(tracker.masks(at_ms(851)).empty());
}

TEST_CASE("tracker: kinds and distant boxes keep separate tracks", "[tracker]") {
  ManualClock clock;
  Tracker tracker(clock, TrackerConfig{});
  tracker.update(at_ms(0), std::vector<Detection>{face(0.0F, 0.0F, 100.0F, 100.0F),
                                                  text(0.0F, 0.0F, 100.0F, 100.0F),
                                                  face(500.0F, 500.0F, 100.0F, 100.0F)});
  tracker.update(at_ms(50), std::vector<Detection>{text(5.0F, 0.0F, 100.0F, 100.0F),
                                                   face(505.0F, 500.0F, 100.0F, 100.0F),
                                                   face(2.0F, 0.0F, 100.0F, 100.0F)});
  const std::vector<spg::Track> tracks = tracker.tracks();
  REQUIRE(tracks.size() == 3);
  CHECK(tracks[0].kind == DetectionKind::Face);
  CHECK(tracks[0].box.rect.x == 2.0F);
  CHECK(tracks[1].kind == DetectionKind::TextRegion);
  CHECK(tracks[1].box.rect.x == 5.0F);
  CHECK(tracks[2].box.rect.x == 505.0F);
}

TEST_CASE("tracker: a track lost past its hold restarts with a new id", "[tracker]") {
  ManualClock clock;
  Tracker tracker(clock, TrackerConfig{});
  tracker.update(at_ms(0), std::vector<Detection>{face(0.0F, 0.0F, 100.0F, 100.0F)});
  tracker.update(at_ms(751), std::vector<Detection>{face(0.0F, 0.0F, 100.0F, 100.0F)});
  const std::vector<spg::Track> tracks = tracker.tracks();
  REQUIRE(tracks.size() == 2);
  CHECK(tracks[0].id == 1);
  CHECK(tracks[1].id == 2);
}

TEST_CASE("tracker: a late detection holds but does not move its track", "[tracker]") {
  ManualClock clock;
  const TrackerConfig config;
  Tracker tracker(clock, config);
  tracker.update(at_ms(0), std::vector<Detection>{face(0.0F, 0.0F, 100.0F, 100.0F)});
  tracker.update(at_ms(200), std::vector<Detection>{face(20.0F, 0.0F, 100.0F, 100.0F)});
  const Detection late = face(10.0F, 0.0F, 100.0F, 100.0F);
  tracker.update(at_ms(100), std::vector<Detection>{late});
  const std::vector<spg::Track> tracks = tracker.tracks();
  REQUIRE(tracks.size() == 1);
  CHECK(tracks[0].box.rect.x == 20.0F);
  CHECK(tracks[0].last_confirmed == at_ms(200));
  CHECK(covered(tracker.masks(at_ms(100)), required(late.box.rect, config)));
  CHECK(covered(tracker.masks(at_ms(850)), required(late.box.rect, config)));
}

TEST_CASE("tracker: an invalid detection throws and changes nothing", "[tracker]") {
  ManualClock clock;
  Tracker tracker(clock, TrackerConfig{});
  Detection readback = face(0.0F, 0.0F, 10.0F, 10.0F);
  readback.box.space = Space::Readback;
  CHECK_THROWS_AS(tracker.update(at_ms(0), std::vector<Detection>{face(0.0F, 0.0F, 5.0F, 5.0F),
                                                                  readback}),
                  spg::ContractError);
  Detection bad_score = face(0.0F, 0.0F, 10.0F, 10.0F);
  bad_score.score = 2.0F;
  CHECK_THROWS_AS(tracker.update(at_ms(0), std::vector<Detection>{bad_score}), spg::ContractError);
  CHECK_THROWS_AS(tracker.update(at_ms(0), std::vector<Detection>{face(0.0F, 0.0F, -1.0F, 5.0F)}),
                  spg::ContractError);
  CHECK(tracker.tracks().empty());
  CHECK(tracker.masks(at_ms(0)).empty());
  CHECK_THROWS_AS(tracker.rescale(Size{0, 10}, Size{10, 10}), spg::ContractError);
}

TEST_CASE("tracker: the injected clock alone decides when held boxes are dropped", "[tracker]") {
  ManualClock clock(at_ms(0));
  TrackerConfig config;  // hold 750 ms, retain 500 ms
  Tracker tracker(clock, config);
  tracker.update(at_ms(0), std::vector<Detection>{face(0.0F, 0.0F, 10.0F, 10.0F)});

  // Still needed: a frame up to `retain` behind the clock may be queried.
  clock.set(at_ms(1250));
  tracker.prune();
  CHECK(tracker.tracks().size() == 1);
  CHECK_FALSE(tracker.masks(at_ms(750)).empty());

  clock.set(at_ms(1250) + Duration{1});
  tracker.update(at_ms(1250), std::vector<Detection>{});
  CHECK(tracker.tracks().empty());

  // A second tracker on a clock that has not moved keeps the same box.
  ManualClock frozen(at_ms(0));
  Tracker other(frozen, config);
  other.update(at_ms(0), std::vector<Detection>{face(0.0F, 0.0F, 10.0F, 10.0F)});
  other.prune();
  CHECK(other.tracks().size() == 1);
}

TEST_CASE("tracker: a resize keeps the old masks and adds scaled ones", "[tracker]") {
  ManualClock clock;
  const TrackerConfig config;
  Tracker tracker(clock, config);
  const Detection d = face(400.0F, 300.0F, 60.0F, 60.0F);
  tracker.update(at_ms(0), std::vector<Detection>{d});
  const std::vector<Box> before = tracker.masks(at_ms(100));
  tracker.rescale(Size{1920, 1080}, Size{1280, 720});
  const std::vector<Box> after = tracker.masks(at_ms(100));
  CHECK(no_shrink(before, after));
  CHECK(covered(after, required(Rect{400.0F * 2.0F / 3.0F, 200.0F, 40.0F, 40.0F}, config)));
  const std::vector<spg::Track> tracks = tracker.tracks();
  REQUIRE(tracks.size() == 1);
  // The track continues in the new coordinates.
  tracker.update(at_ms(100), std::vector<Detection>{face(268.0F, 200.0F, 40.0F, 40.0F)});
  CHECK(tracker.tracks().size() == 1);
}

// --- Property tests --------------------------------------------------------
//
// Seeded random scenes: several objects of both kinds moving at constant or
// changing velocity, detected on irregular cycles with jitter, missed cycles
// (tracking loss), late deliveries, frames that are never queried (dropped)
// and sometimes one source resize. Deterministic per seed; the seed is
// reported on failure.

namespace {

struct Fed {
  Rect box;
  Timestamp seen;
  bool before_resize = false;
};

struct Object {
  double x, y, w, h, vx, vy;
  DetectionKind kind;
};

class Scene {
 public:
  explicit Scene(std::uint32_t seed) : rng_(seed) {}

  double uniform(double lo, double hi) {
    return std::uniform_real_distribution<double>(lo, hi)(rng_);
  }
  // A random duration in [0, span].
  Duration within(Duration span) {
    const double fraction = uniform(0.0, 1.0);
    return Duration{static_cast<std::int64_t>(fraction * static_cast<double>(span.count()))};
  }
  int uniform_int(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }
  bool chance(double p) { return uniform(0.0, 1.0) < p; }

  TrackerConfig config() {
    TrackerConfig c;
    c.hold = milliseconds{uniform_int(0, 4) == 0 ? 0 : uniform_int(100, 1500)};
    c.margin_px = static_cast<float>(uniform(0.0, 8.0));
    c.margin_ratio = static_cast<float>(uniform(0.0, 0.5));
    c.retain = milliseconds{uniform_int(0, 1000)};
    c.iou_threshold = static_cast<float>(uniform(0.1, 0.7));
    c.velocity_smoothing = static_cast<float>(uniform(0.2, 1.0));
    c.max_prediction = milliseconds{uniform_int(0, 800)};
    return c;
  }

  std::vector<Object> objects() {
    std::vector<Object> out(static_cast<std::size_t>(uniform_int(1, 5)));
    for (Object& o : out) {
      o.w = uniform(8.0, 300.0);
      o.h = uniform(8.0, 300.0);
      o.x = uniform(-50.0, 1900.0);
      o.y = uniform(-50.0, 1050.0);
      o.vx = chance(0.3) ? 0.0 : uniform(-900.0, 900.0);
      o.vy = chance(0.3) ? 0.0 : uniform(-600.0, 600.0);
      o.kind = chance(0.5) ? DetectionKind::Face : DetectionKind::TextRegion;
    }
    return out;
  }

 private:
  std::mt19937 rng_;
};

// Coverage the hold guarantees for `fed` in the current coordinates: its own
// box, grown by the margin, and (after a resize) the scaled box too. The
// scaled region is computed in double from the original box so float rounding
// of the expectation cannot hide a shrink.
std::vector<Region> expected_regions(const Fed& fed, double sx, double sy,
                                     const TrackerConfig& config) {
  std::vector<Region> out{required(fed.box, config)};
  if (fed.before_resize) {
    const Rect& b = fed.box;
    const double x0 = static_cast<double>(b.x) * sx;
    const double y0 = static_cast<double>(b.y) * sy;
    const double x1 = (static_cast<double>(b.x) + static_cast<double>(b.width)) * sx;
    const double y1 = (static_cast<double>(b.y) + static_cast<double>(b.height)) * sy;
    const double m = static_cast<double>(config.margin_px) +
                     static_cast<double>(config.margin_ratio) * std::min(x1 - x0, y1 - y0);
    out.push_back({x0 - m, y0 - m, x1 + m, y1 + m});
  }
  return out;
}

}  // namespace

TEST_CASE("tracker: masks never shrink before hold expiry (property)", "[tracker][property]") {
  constexpr std::uint32_t kScenes = 2000;
  constexpr std::int64_t kFrameNs = 16'666'667;  // 60 fps
  for (std::uint32_t seed = 1; seed <= kScenes; ++seed) {
    INFO("seed " << seed);
    Scene scene(seed);
    const TrackerConfig config = scene.config();
    std::vector<Object> objects = scene.objects();
    ManualClock clock;
    Tracker tracker(clock, config);
    std::vector<Fed> fed;
    std::vector<std::pair<Timestamp, std::vector<Detection>>> late;

    const int frames = scene.uniform_int(60, 200);
    const int resize_frame = scene.chance(0.3) ? scene.uniform_int(10, frames - 10) : -1;
    double sx = 1.0;
    double sy = 1.0;
    int next_cycle = 0;

    for (int f = 0; f < frames; ++f) {
      const Timestamp frame_time = spg::timestamp_from_ns(kFrameNs * f);
      // Objects move; some change direction.
      for (Object& o : objects) {
        o.x += o.vx * static_cast<double>(kFrameNs) / 1e9;
        o.y += o.vy * static_cast<double>(kFrameNs) / 1e9;
        if (scene.chance(0.01)) {
          o.vx = scene.uniform(-900.0, 900.0);
          o.vy = scene.uniform(-600.0, 600.0);
        }
      }
      if (f == resize_frame) {
        // Results still in flight are in the old coordinates; deliver them
        // before the resize (the scheduler owns what happens otherwise).
        for (const auto& [when, batch] : late) {
          tracker.update(when, batch);
          for (const Detection& d : batch) {
            fed.push_back(Fed{d.box.rect, when});
          }
        }
        late.clear();
        // Snapshot masks a lookahead's worth of frames back, resize, compare.
        const Timestamp probe = clock.now() - config.retain;
        const std::vector<Box> before = tracker.masks(probe);
        const std::vector<Box> before_now = tracker.masks(frame_time);
        sx = scene.uniform(0.4, 2.0);
        sy = scene.chance(0.5) ? sx : scene.uniform(0.4, 2.0);
        tracker.rescale(Size{1920, 1080},
                        Size{static_cast<std::uint32_t>(std::lround(1920.0 * sx)),
                             static_cast<std::uint32_t>(std::lround(1080.0 * sy))});
        sx = static_cast<double>(std::lround(1920.0 * sx)) / 1920.0;
        sy = static_cast<double>(std::lround(1080.0 * sy)) / 1080.0;
        REQUIRE(no_shrink(before, tracker.masks(probe)));
        REQUIRE(no_shrink(before_now, tracker.masks(frame_time)));
        for (Object& o : objects) {
          o.x *= sx;
          o.w *= sx;
          o.vx *= sx;
          o.y *= sy;
          o.h *= sy;
          o.vy *= sy;
        }
        // Detections fed so far must now also be covered in the new
        // coordinates; expected_regions() scales them.
        for (Fed& fd : fed) {
          fd.before_resize = true;
        }
      }
      if (f < next_cycle) {
        continue;
      }
      next_cycle = f + scene.uniform_int(1, 12);

      // One detection cycle: each object found with some probability (a
      // miss is tracking loss), with jitter; occasionally delivered late.
      std::vector<Detection> detections;
      for (const Object& o : objects) {
        if (!scene.chance(0.75)) {
          continue;
        }
        const double j = 3.0;
        Detection d;
        d.box = Box{Rect{static_cast<float>(o.x + scene.uniform(-j, j)),
                         static_cast<float>(o.y + scene.uniform(-j, j)),
                         static_cast<float>(std::max(1.0, o.w + scene.uniform(-j, j))),
                         static_cast<float>(std::max(1.0, o.h + scene.uniform(-j, j)))},
                    Space::Source};
        d.kind = o.kind;
        d.score = 0.8F;
        detections.push_back(d);
      }
      const bool deliver_late = scene.chance(0.15);
      std::vector<std::pair<Timestamp, std::vector<Detection>>> batches;
      if (deliver_late) {
        late.emplace_back(frame_time, detections);
      } else {
        batches.emplace_back(frame_time, detections);
        if (!late.empty() && scene.chance(0.5)) {
          batches.push_back(late.front());
          late.erase(late.begin());
        }
      }

      for (const auto& [when, batch] : batches) {
        // Detection latency: the clock is ahead of the frame time.
        clock.set(std::max(clock.now(), frame_time + milliseconds{scene.uniform_int(5, 120)}));
        // Updates only add: snapshot masks for frames still in the buffer.
        std::vector<Timestamp> probes;
        for (int k = 0; k < 4; ++k) {
          probes.push_back(clock.now() - config.retain + scene.within(config.retain + config.hold));
        }
        std::vector<std::vector<Box>> before;
        for (Timestamp p : probes) {
          before.push_back(tracker.masks(p));
        }
        tracker.update(when, batch);
        for (std::size_t k = 0; k < probes.size(); ++k) {
          INFO("update at frame " << f << ", probe " << k);
          REQUIRE(no_shrink(before[k], tracker.masks(probes[k])));
        }
        for (const Detection& d : batch) {
          fed.push_back(Fed{d.box.rect, when});
        }
      }

      // Hold: every detection is covered, grown by the margin, on every frame
      // time in [seen, seen + hold] that is still within the retention.
      // Frames are sampled (some are never queried, like dropped frames).
      const Timestamp oldest = clock.now() - config.retain;
      for (int k = 0; k < 6; ++k) {
        const Timestamp t = oldest + scene.within(config.retain + config.hold);
        const std::vector<Box> masks = tracker.masks(t);
        require_source_boxes(masks);
        for (std::size_t i = 0; i < fed.size(); ++i) {
          if (t < fed[i].seen || t > fed[i].seen + config.hold) {
            continue;
          }
          for (const Region& g : expected_regions(fed[i], sx, sy, config)) {
            INFO("frame " << f << ", detection " << i);
            REQUIRE(covered(masks, g));
          }
        }
      }
    }

    // After the scene: freeze retention and check monotonic growth along
    // frame time between hold expiries of the detections active at t.
    clock.set(Timestamp{});
    for (int k = 0; k < 60; ++k) {
      const Timestamp t = Timestamp{} + scene.within(Duration{kFrameNs * frames});
      Timestamp limit = t + milliseconds{2000};
      for (const Fed& fd : fed) {
        if (fd.seen <= t && t <= fd.seen + config.hold) {
          limit = std::min(limit, fd.seen + config.hold);
        }
      }
      const Timestamp t2 = t + scene.within(limit - t);
      INFO("t " << spg::to_ns(t) << " -> " << spg::to_ns(t2));
      REQUIRE(no_shrink(tracker.masks(t), tracker.masks(t2)));
    }
  }
}
