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

Region scaled(const Region& g, double sx, double sy) {
  return {g.x0 * sx, g.y0 * sy, g.x1 * sx, g.y1 * sy};
}

// Every rect of `before`, scaled by (sx, sy) in double, is contained in some
// rect of `after`: what was blurred before a resize stays blurred after it.
bool no_shrink_scaled(const std::vector<Box>& before, const std::vector<Box>& after, double sx,
                      double sy) {
  return std::all_of(before.begin(), before.end(), [&](const Box& b) {
    return covered(after, scaled(region_of(b.rect), sx, sy));
  });
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
  rejects([](TrackerConfig& c) { c.motion_gate_per_s = -1.0F; });
  rejects([](TrackerConfig& c) { c.motion_gate_per_s = INFINITY; });
  rejects([](TrackerConfig& c) { c.motion_gate_per_s = std::nanf(""); });
  rejects([](TrackerConfig& c) { c.motion_size_ratio = 0.99F; });
  rejects([](TrackerConfig& c) { c.motion_size_ratio = INFINITY; });
  rejects([](TrackerConfig& c) { c.motion_size_ratio = std::nanf(""); });
  rejects([](TrackerConfig& c) { c.velocity_smoothing = 0.0F; });
  rejects([](TrackerConfig& c) { c.velocity_smoothing = std::nanf(""); });
  rejects([](TrackerConfig& c) { c.max_rects_per_track = 0; });
  rejects([](TrackerConfig& c) { c.max_rects_per_track = 65; });

  // Zero hold and margin are accepted here (a floor is #28).
  TrackerConfig zero;
  zero.hold = Duration::zero();
  zero.margin_px = 0.0F;
  zero.margin_ratio = 0.0F;
  zero.motion_gate_per_s = 0.0F;  // the motion gate off
  zero.motion_size_ratio = 1.0F;
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

TEST_CASE("tracker: update reports the motion of the track each detection joined", "[tracker]") {
  ManualClock clock;
  Tracker tracker(clock, TrackerConfig{});
  std::vector<spg::TrackMotion> motions{{{9.0F, 9.0F}, at_ms(-5)}};
  // A new track has no motion measurement yet: zero, first seen now.
  tracker.update(at_ms(0), std::vector<Detection>{face(100.0F, 100.0F, 50.0F, 50.0F)}, &motions);
  REQUIRE(motions.size() == 1);
  CHECK(motions[0] == spg::TrackMotion{spg::Velocity{}, at_ms(0)});
  // The second confirmation measures 200 px/s right; a far box starts a track.
  tracker.update(at_ms(100),
                 std::vector<Detection>{face(600.0F, 600.0F, 50.0F, 50.0F), face(120.0F, 100.0F, 50.0F, 50.0F)},
                 &motions);
  REQUIRE(motions.size() == 2);
  CHECK(motions[0] == spg::TrackMotion{spg::Velocity{}, at_ms(100)});
  CHECK(std::abs(motions[1].velocity.dx_per_s - 200.0F) < 0.01F);
  CHECK(motions[1].velocity.dy_per_s == 0.0F);
  CHECK(motions[1].first_seen == at_ms(0));  // the track's first detection
  CHECK(motions[1].translating);             // both x edges moved right
  // A late detection of the moving track, from before its first one, moves
  // first_seen back but not the track.
  tracker.update(at_ms(-50), std::vector<Detection>{face(110.0F, 100.0F, 50.0F, 50.0F)}, &motions);
  REQUIRE(motions.size() == 1);
  CHECK(motions[0].first_seen == at_ms(-50));
  CHECK(std::abs(motions[0].velocity.dx_per_s - 200.0F) < 0.01F);
  // Without the out parameter nothing changes.
  tracker.update(at_ms(200), std::vector<Detection>{face(140.0F, 100.0F, 50.0F, 50.0F)});
  CHECK(std::abs(tracker.tracks()[0].velocity.dx_per_s - 200.0F) < 0.01F);
}

TEST_CASE("tracker: an older detection that arrives last measures the motion it saw", "[tracker]") {
  ManualClock clock;
  Tracker tracker(clock, TrackerConfig{});
  std::vector<spg::TrackMotion> motions;
  // Two cycles finish out of order: the newer one starts the track, then the
  // older one shows the box came from 6 px to the right 30 ms before.
  tracker.update(at_ms(280), std::vector<Detection>{face(900.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  REQUIRE(motions.size() == 1);
  CHECK(motions[0] == spg::TrackMotion{spg::Velocity{}, at_ms(280)});
  tracker.update(at_ms(250), std::vector<Detection>{face(906.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  REQUIRE(motions.size() == 1);
  CHECK(motions[0].first_seen == at_ms(250));
  CHECK(std::abs(motions[0].velocity.dx_per_s + 200.0F) < 0.01F);
  CHECK(motions[0].velocity.dy_per_s == 0.0F);
  CHECK(motions[0].translating);
  // The track's box stays the newest one.
  CHECK(tracker.tracks()[0].box.rect == Rect{900.0F, 100.0F, 200.0F, 20.0F});
  CHECK(tracker.tracks()[0].last_confirmed == at_ms(280));

  // A same-frame second result measures nothing.
  Tracker same(clock, TrackerConfig{});
  same.update(at_ms(280), std::vector<Detection>{face(900.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  same.update(at_ms(280), std::vector<Detection>{face(906.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  CHECK(motions[0] == spg::TrackMotion{spg::Velocity{}, at_ms(280)});

  // A track that already has a velocity keeps it, but an earliest detection
  // that shows the box sliding the same way marks it translating.
  Tracker measured(clock, TrackerConfig{});
  measured.update(at_ms(280), std::vector<Detection>{face(900.4F, 100.0F, 200.0F, 20.0F)}, &motions);
  measured.update(at_ms(300), std::vector<Detection>{face(900.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  REQUIRE(motions[0].velocity.dx_per_s < 0.0F);
  CHECK_FALSE(motions[0].translating);  // 0.4 px
  const spg::Velocity before = motions[0].velocity;
  measured.update(at_ms(250), std::vector<Detection>{face(906.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  CHECK(motions[0].velocity == before);
  CHECK(motions[0].first_seen == at_ms(250));
  CHECK(motions[0].translating);
}

TEST_CASE("tracker: a box that grows or shrinks in place is not translating", "[tracker]") {
  ManualClock clock;
  Tracker tracker(clock, TrackerConfig{});
  std::vector<spg::TrackMotion> motions;
  // Typing: the left edge stays and the box grows right, so the center moves
  // right at half the growth rate.
  tracker.update(at_ms(0), std::vector<Detection>{face(100.0F, 100.0F, 100.0F, 20.0F)}, &motions);
  tracker.update(at_ms(100), std::vector<Detection>{face(100.0F, 100.0F, 120.0F, 20.0F)}, &motions);
  REQUIRE(motions.size() == 1);
  CHECK(motions[0].velocity.dx_per_s > 0.0F);
  CHECK_FALSE(motions[0].translating);
  // Fading in: the box grows on both sides.
  tracker.update(at_ms(200), std::vector<Detection>{face(98.0F, 99.0F, 126.0F, 22.0F)}, &motions);
  CHECK_FALSE(motions[0].translating);
  // Then it moves up: both y edges move the way the larger velocity points.
  tracker.update(at_ms(300), std::vector<Detection>{face(98.0F, 90.0F, 126.0F, 22.0F)}, &motions);
  CHECK(motions[0].velocity.dy_per_s < 0.0F);
  CHECK(std::abs(motions[0].velocity.dy_per_s) > std::abs(motions[0].velocity.dx_per_s));
  CHECK(motions[0].translating);
  // Once translating, it stays so: coming to rest does not clear it.
  tracker.update(at_ms(400), std::vector<Detection>{face(98.0F, 90.0F, 126.0F, 22.0F)}, &motions);
  CHECK(motions[0].translating);

  // A shift of less than a pixel is not a translation.
  Tracker sub_pixel(clock, TrackerConfig{});
  sub_pixel.update(at_ms(0), std::vector<Detection>{face(98.0F, 90.0F, 126.0F, 22.0F)}, &motions);
  sub_pixel.update(at_ms(100), std::vector<Detection>{face(98.0F, 89.5F, 126.0F, 22.0F)}, &motions);
  CHECK(motions[0].velocity.dy_per_s < 0.0F);
  CHECK_FALSE(motions[0].translating);
}

TEST_CASE("tracker: three results landing in reverse order still show the slide", "[tracker]") {
  ManualClock clock;
  Tracker tracker(clock, TrackerConfig{});
  std::vector<spg::TrackMotion> motions;
  // A slide-in to the left, at rest by 280: the 280 result lands first, then
  // 270 (the same box: zero velocity), then 250, 6 px to the right.
  tracker.update(at_ms(280), std::vector<Detection>{face(900.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  tracker.update(at_ms(270), std::vector<Detection>{face(900.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  REQUIRE(motions.size() == 1);
  CHECK(motions[0].velocity == spg::Velocity{});
  CHECK_FALSE(motions[0].translating);
  tracker.update(at_ms(250), std::vector<Detection>{face(906.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  CHECK(motions[0].first_seen == at_ms(250));
  CHECK(std::abs(motions[0].velocity.dx_per_s + 200.0F) < 0.01F);
  CHECK(motions[0].translating);
  CHECK(tracker.tracks()[0].box.rect == Rect{900.0F, 100.0F, 200.0F, 20.0F});

  // The same with a velocity that points the other way (jitter at rest).
  Tracker jitter(clock, TrackerConfig{});
  jitter.update(at_ms(280), std::vector<Detection>{face(900.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  jitter.update(at_ms(270), std::vector<Detection>{face(899.6F, 100.0F, 200.0F, 20.0F)}, &motions);
  REQUIRE(motions[0].velocity.dx_per_s > 0.0F);
  jitter.update(at_ms(250), std::vector<Detection>{face(906.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  CHECK(motions[0].velocity.dx_per_s < 0.0F);
  CHECK(motions[0].translating);

  // A late result that is not the earliest, after a scan at rest: the track
  // is still translating.
  Tracker sticky(clock, TrackerConfig{});
  sticky.update(at_ms(250), std::vector<Detection>{face(906.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  sticky.update(at_ms(280), std::vector<Detection>{face(900.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  CHECK(motions[0].translating);
  sticky.update(at_ms(300), std::vector<Detection>{face(900.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  sticky.update(at_ms(260), std::vector<Detection>{face(902.0F, 100.0F, 200.0F, 20.0F)}, &motions);
  CHECK(motions[0].first_seen == at_ms(250));
  CHECK(motions[0].translating);
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

TEST_CASE("tracker: a small fast box keeps its track between sparse detections", "[tracker]") {
  // SPG-16b: a 35 x 48 px face at (490, 140) px/s, one detection cycle every
  // 200 ms (one worker), as the faces suite's seq-018 face moves. It moves
  // 98 px, 2.8 widths, between detections, so IoU never pairs them; the
  // motion gate does.
  const auto box_at = [](std::int64_t ms) {
    const auto t = static_cast<float>(ms);
    return Rect{100.0F + 0.49F * t, 300.0F + 0.14F * t, 35.0F, 48.0F};
  };
  const TrackerConfig config;
  ManualClock clock;
  Tracker tracker(clock, config);
  std::vector<spg::TrackMotion> motions;
  for (std::int64_t ms = 0; ms <= 800; ms += 200) {
    clock.set(at_ms(ms));
    const Rect b = box_at(ms);
    tracker.update(at_ms(ms), std::vector<Detection>{face(b.x, b.y, b.width, b.height)}, &motions);
    REQUIRE(motions.size() == 1);
    CHECK(motions[0].first_seen == at_ms(0));
    if (ms > 0) {
      CHECK(std::abs(motions[0].velocity.dx_per_s - 490.0F) < 1.0F);
      CHECK(std::abs(motions[0].velocity.dy_per_s - 140.0F) < 1.0F);
      CHECK(motions[0].translating);
    }
  }
  const std::vector<spg::Track> tracks = tracker.tracks();
  REQUIRE(tracks.size() == 1);
  CHECK(tracks[0].id == 1);
  // Every 60 fps frame from the first detection on is covered, with margin,
  // where the face really is: between sightings too.
  for (std::int64_t ms = 0; ms <= 800; ms += 16) {
    INFO("frame " << ms);
    CHECK(covered(tracker.masks(at_ms(ms)), required(box_at(ms), config)));
  }

  // Without the gate (main before SPG-16b) every cycle starts a track with no
  // velocity, and the face leaves its mask between cycles.
  TrackerConfig off = config;
  off.motion_gate_per_s = 0.0F;
  ManualClock clock_off;
  Tracker before(clock_off, off);
  for (std::int64_t ms = 0; ms <= 800; ms += 200) {
    clock_off.set(at_ms(ms));
    const Rect b = box_at(ms);
    before.update(at_ms(ms), std::vector<Detection>{face(b.x, b.y, b.width, b.height)});
  }
  CHECK(before.tracks().size() == 5);
  CHECK_FALSE(covered(before.masks(at_ms(100)), required(box_at(100), off)));
}

TEST_CASE("tracker: a slide-in seen at the edge and near rest is covered in between", "[tracker]") {
  // A toast eases out (cubic) from the right edge of a 1920 px frame to
  // x = 1400 over 200 ms from 170; cycles 200 ms apart see it clipped at the
  // edge (95 of 180 px wide),
  // then near rest (SPG-16b, text suite seq-006 with one worker). The
  // measured velocity puts it where constant motion would, behind where the
  // ease-out really has it; the hull of the two sightings covers it.
  const auto box_at = [](std::int64_t ms) {
    const double p = std::clamp(static_cast<double>(ms - 170) / 200.0, 0.0, 1.0);
    const double x = 1920.0 + (1400.0 - 1920.0) * (1.0 - std::pow(1.0 - p, 3.0));
    return Rect{static_cast<float>(x), 100.0F, static_cast<float>(std::min(180.0, 1920.0 - x)), 28.0F};
  };
  const TrackerConfig config;
  ManualClock clock;
  Tracker tracker(clock, config);
  std::vector<spg::TrackMotion> motions;
  for (const std::int64_t ms : {183, 383}) {
    clock.set(at_ms(ms));
    const Rect b = box_at(ms);
    tracker.update(at_ms(ms), std::vector<Detection>{text(b.x, b.y, b.width, b.height)}, &motions);
  }
  REQUIRE(tracker.tracks().size() == 1);
  REQUIRE(motions.size() == 1);
  CHECK(motions[0].first_seen == at_ms(183));
  CHECK(motions[0].velocity.dx_per_s < -1000.0F);
  CHECK(motions[0].translating);  // the scheduler trails it back to the edge
  for (std::int64_t ms = 183; ms <= 383; ms += 16) {
    INFO("frame " << ms);
    CHECK(covered(tracker.masks(at_ms(ms)), required(box_at(ms), config)));
  }
}

TEST_CASE("tracker: the motion gate bounds distance, size and kind", "[tracker]") {
  // A 40 px face at 1000; with a 12/s gate, 200 ms later it reaches
  // 12/s * 0.2 s = 2.4 box sizes from its center.
  TrackerConfig config;
  config.motion_gate_per_s = 12.0F;
  config.motion_size_ratio = 2.0F;
  const auto tracks_after = [&](const Detection& second, Duration gap = milliseconds{200}) {
    ManualClock clock;
    Tracker tracker(clock, config);
    tracker.update(at_ms(0), std::vector<Detection>{face(1000.0F, 500.0F, 40.0F, 40.0F)});
    clock.set(at_ms(0) + gap);
    tracker.update(at_ms(0) + gap, std::vector<Detection>{second});
    return tracker.tracks().size();
  };
  CHECK(tracks_after(face(1095.0F, 500.0F, 40.0F, 40.0F)) == 1);   // 2.375 sizes
  CHECK(tracks_after(face(1097.0F, 500.0F, 40.0F, 40.0F)) == 2);   // 2.425 sizes
  CHECK(tracks_after(face(1000.0F, 595.0F, 40.0F, 40.0F)) == 1);   // either axis
  CHECK(tracks_after(face(1060.0F, 560.0F, 40.0F, 40.0F)) == 1);   // 2.12 sizes
  CHECK(tracks_after(face(1070.0F, 570.0F, 40.0F, 40.0F)) == 2);   // 2.47 sizes
  // Half the time, half the reach.
  CHECK(tracks_after(face(1045.0F, 500.0F, 40.0F, 40.0F), milliseconds{100}) == 1);
  CHECK(tracks_after(face(1050.0F, 500.0F, 40.0F, 40.0F), milliseconds{100}) == 2);
  // Reach stops growing at max_prediction (500 ms: 6 sizes).
  CHECK(tracks_after(face(1235.0F, 500.0F, 40.0F, 40.0F), milliseconds{700}) == 1);
  CHECK(tracks_after(face(1250.0F, 500.0F, 40.0F, 40.0F), milliseconds{700}) == 2);
  // A box size is the larger extent of the two boxes on each axis.
  CHECK(tracks_after(face(1150.0F, 500.0F, 80.0F, 40.0F)) == 1);  // 4.25 old widths, 2.125 new
  // Width and height each within a factor of 2.
  CHECK(tracks_after(face(1060.0F, 500.0F, 80.0F, 40.0F)) == 1);
  CHECK(tracks_after(face(1060.0F, 500.0F, 81.0F, 40.0F)) == 2);
  CHECK(tracks_after(face(1060.0F, 500.0F, 40.0F, 19.0F)) == 2);
  // Same kind only; the gate is off at 0.
  CHECK(tracks_after(text(1060.0F, 500.0F, 40.0F, 40.0F)) == 2);
  TrackerConfig off = config;
  off.motion_gate_per_s = 0.0F;
  ManualClock clock;
  Tracker tracker(clock, off);
  tracker.update(at_ms(0), std::vector<Detection>{face(1000.0F, 500.0F, 40.0F, 40.0F)});
  tracker.update(at_ms(200), std::vector<Detection>{face(1060.0F, 500.0F, 40.0F, 40.0F)});
  CHECK(tracker.tracks().size() == 2);
}

TEST_CASE("tracker: the motion gate measures from where the track's velocity puts it", "[tracker]") {
  TrackerConfig config;
  config.motion_gate_per_s = 12.0F;  // 2.4 box sizes in 200 ms
  ManualClock clock;
  Tracker tracker(clock, config);
  // 450 px/s to the right (2.25 sizes in 200 ms, joined by the gate). 200 ms
  // later the box is 3.125 sizes from the last one, out of reach, but 0.875
  // from where the velocity puts it, and too far ahead for IoU.
  tracker.update(at_ms(0), std::vector<Detection>{face(0.0F, 500.0F, 40.0F, 40.0F)});
  tracker.update(at_ms(200), std::vector<Detection>{face(90.0F, 500.0F, 40.0F, 40.0F)});
  REQUIRE(tracker.tracks().size() == 1);
  tracker.update(at_ms(400), std::vector<Detection>{face(215.0F, 500.0F, 40.0F, 40.0F)});
  REQUIRE(tracker.tracks().size() == 1);
  CHECK(tracker.tracks()[0].velocity.dx_per_s == 537.5F);
  // An older result is measured back along the velocity: 0.4 sizes from
  // where it puts the box 100 ms earlier, 1.75 from the last box (reach 1.2).
  tracker.update(at_ms(300), std::vector<Detection>{face(145.0F, 500.0F, 40.0F, 40.0F)});
  CHECK(tracker.tracks().size() == 1);
}

TEST_CASE("tracker: IoU pairs come first and the gate pairs the closest", "[tracker]") {
  TrackerConfig config;
  config.motion_gate_per_s = 12.0F;  // 2.4 box sizes in 200 ms
  ManualClock clock;
  Tracker tracker(clock, config);
  // Tracks A (id 1) at 1100 and B (id 2) at 1180.
  tracker.update(at_ms(0), std::vector<Detection>{face(1100.0F, 500.0F, 40.0F, 40.0F),
                                                  face(1180.0F, 500.0F, 40.0F, 40.0F)});
  // 200 ms later: 1185 overlaps B (IoU 0.78) and is in A's reach (2.125
  // sizes); IoU keeps it on B. 1140 is 1 size from A and from B, and 1050
  // is 1.25 sizes from A: the gate gives A the closer one, and 1050 starts
  // a track.
  tracker.update(at_ms(200), std::vector<Detection>{face(1050.0F, 500.0F, 40.0F, 40.0F),
                                                    face(1185.0F, 500.0F, 40.0F, 40.0F),
                                                    face(1140.0F, 500.0F, 40.0F, 40.0F)});
  const std::vector<spg::Track> tracks = tracker.tracks();
  REQUIRE(tracks.size() == 3);
  CHECK(tracks[0].box.rect.x == 1140.0F);
  CHECK(tracks[1].box.rect.x == 1185.0F);
  CHECK(tracks[2].box.rect.x == 1050.0F);
  CHECK(tracks[2].id == 3);
}

TEST_CASE("tracker: a wrong motion pairing keeps every hold", "[tracker]") {
  // Face A is seen once; face B appears 200 ms later within A's reach and
  // joins A's track. Each box still holds, with its margin, for the whole
  // hold after the frame it was seen on: a pairing only moves masks along
  // the track's motion, it never ends one.
  const TrackerConfig config;
  ManualClock clock;
  Tracker tracker(clock, config);
  const Rect a{1000.0F, 500.0F, 40.0F, 40.0F};
  const Rect b{1000.0F, 580.0F, 40.0F, 40.0F};
  tracker.update(at_ms(0), std::vector<Detection>{face(a.x, a.y, a.width, a.height)});
  clock.set(at_ms(200));
  tracker.update(at_ms(200), std::vector<Detection>{face(b.x, b.y, b.width, b.height)});
  REQUIRE(tracker.tracks().size() == 1);
  for (std::int64_t ms = 0; ms <= 950; ms += 10) {
    INFO("frame " << ms);
    clock.set(at_ms(ms));
    const std::vector<Box> masks = tracker.masks(at_ms(ms));
    if (ms <= 750) {
      CHECK(covered(masks, required(a, config)));
    }
    if (ms >= 200) {
      CHECK(covered(masks, required(b, config)));
    }
  }
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

TEST_CASE("tracker: an upscale keeps the scaled margin and rounding", "[tracker]") {
  ManualClock clock;
  const TrackerConfig config;  // 4 px + 0.25 * min(w, h)
  Tracker tracker(clock, config);
  tracker.update(at_ms(0), std::vector<Detection>{face(0.0F, 0.0F, 40.0F, 40.0F)});
  const std::vector<Box> before = tracker.masks(at_ms(100));
  REQUIRE(before.size() == 1);
  CHECK(before[0].rect == Rect{-14.0F, -14.0F, 68.0F, 68.0F});

  // 2x: the old mask [-14, 54] scales to [-28, 108]. A fresh margin on the
  // scaled box (4 + 0.25 * 80 = 24, so [-24, 104]) would uncover 4 px a side.
  tracker.rescale(Size{960, 540}, Size{1920, 1080});
  const std::vector<Box> after = tracker.masks(at_ms(100));
  CHECK(no_shrink_scaled(before, after, 2.0, 2.0));
  CHECK(covered(after, {-28.0, -28.0, 108.0, 108.0}));
  CHECK(no_shrink(before, after));

  // A second, anisotropic resize keeps the twice-scaled masks too, and a
  // moving track keeps its scaled prediction.
  Tracker moving(clock, config);
  moving.update(at_ms(0), std::vector<Detection>{face(101.3F, 57.9F, 33.0F, 21.0F)});
  moving.update(at_ms(100), std::vector<Detection>{face(131.3F, 47.9F, 33.0F, 21.0F)});
  const std::vector<Box> m0 = moving.masks(at_ms(400));
  moving.rescale(Size{1280, 720}, Size{1920, 1080});
  const std::vector<Box> m1 = moving.masks(at_ms(400));
  CHECK(no_shrink_scaled(m0, m1, 1.5, 1.5));
  moving.rescale(Size{1920, 1080}, Size{2560, 1080});
  const std::vector<Box> m2 = moving.masks(at_ms(400));
  CHECK(no_shrink_scaled(m1, m2, 4.0 / 3.0, 1.0));
  CHECK(no_shrink_scaled(m0, m2, 2.0, 1.5));
}

TEST_CASE("tracker: a long hold stays within the rect cap and keeps coverage", "[tracker]") {
  // 60 s hold, detected at 10 Hz while moving 150 px/s: 600 held detections
  // whose masks do not contain each other.
  ManualClock clock;
  TrackerConfig config;
  config.hold = milliseconds{60'000};
  config.margin_px = 0.0F;
  config.margin_ratio = 0.0F;
  Tracker tracker(clock, config);
  std::vector<Detection> fed;
  for (int i = 0; i < 600; ++i) {
    clock.set(at_ms(100 * i));
    const Detection d = face(15.0F * static_cast<float>(i), 0.0F, 30.0F, 30.0F);
    tracker.update(at_ms(100 * i), std::vector<Detection>{d});
    fed.push_back(d);
  }
  REQUIRE(tracker.tracks().size() == 1);
  std::vector<Box> reused;
  for (const std::int64_t ms : {0, 3'000, 29'950, 59'900, 60'050, 119'900}) {
    INFO("t " << ms << " ms");
    const std::vector<Box> masks = tracker.masks(at_ms(ms));
    CHECK(masks.size() <= config.max_rects_per_track);
    require_source_boxes(masks);
    for (std::size_t i = 0; i < fed.size(); ++i) {
      const Timestamp seen = at_ms(100 * static_cast<std::int64_t>(i));
      if (at_ms(ms) >= seen && at_ms(ms) <= seen + config.hold) {
        CHECK(covered(masks, required(fed[i].box.rect, config)));
      }
    }
    // The buffer overload returns the same rects and reuses the buffer.
    tracker.masks(at_ms(ms), reused);
    CHECK(reused == masks);
  }

  // One rect per track when capped at 1.
  config.max_rects_per_track = 1;
  Tracker single(clock, config);
  single.update(at_ms(59'800), std::vector<Detection>{face(0.0F, 0.0F, 30.0F, 30.0F)});
  single.update(at_ms(59'900), std::vector<Detection>{face(15.0F, 0.0F, 30.0F, 30.0F)});
  const std::vector<Box> one = single.masks(at_ms(59'900));
  REQUIRE(one.size() == 1);
  CHECK(covered(one, {0.0, 0.0, 45.0, 30.0}));
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
    c.max_rects_per_track = static_cast<std::size_t>(uniform_int(1, 12));
    c.motion_gate_per_s = chance(0.2) ? 0.0F : static_cast<float>(uniform(0.0, 40.0));
    c.motion_size_ratio = static_cast<float>(uniform(1.0, 4.0));
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
// box, grown by the margin, and (after a resize) the scaled box grown by its
// own margin and the old region scaled with its margin. The scaled regions are
// computed in double from the original box so float rounding of the
// expectation cannot hide a shrink.
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
    out.push_back(scaled(required(fed.box, config), sx, sy));
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
    bool fed_fresh = false;  // a detection was delivered on its own frame
    int monotone_checks = 0;  // non-empty monotone-in-time comparisons

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
        REQUIRE(no_shrink_scaled(before, tracker.masks(probe), sx, sy));
        REQUIRE(no_shrink_scaled(before_now, tracker.masks(frame_time), sx, sy));
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
          fed_fresh = fed_fresh || when == frame_time;
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
        REQUIRE(masks.size() <= tracker.tracks().size() * config.max_rects_per_track);
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

      // Monotone in frame time, checked while the tracker still holds every
      // detection the window can see (nothing seen + hold >= oldest has been
      // pruned): from a t inside a retained detection's hold to a t2 before
      // any detection active at t expires, masks only grow.
      std::vector<std::size_t> live;
      for (std::size_t i = 0; i < fed.size(); ++i) {
        if (fed[i].seen + config.hold >= oldest) {
          live.push_back(i);
        }
      }
      for (int k = 0; k < 2 && !live.empty(); ++k) {
        const int pick = scene.uniform_int(0, static_cast<int>(live.size()) - 1);
        const Fed& anchor = fed[live[static_cast<std::size_t>(pick)]];
        const Timestamp lo = std::max(anchor.seen, oldest);
        const Timestamp t = lo + scene.within(anchor.seen + config.hold - lo);
        Timestamp limit = t + milliseconds{2000};
        for (const Fed& fd : fed) {
          if (fd.seen <= t && t <= fd.seen + config.hold) {
            limit = std::min(limit, fd.seen + config.hold);
          }
        }
        const Timestamp t2 = t + scene.within(limit - t);
        INFO("frame " << f << ", t " << spg::to_ns(t) << " -> " << spg::to_ns(t2));
        const std::vector<Box> at_t = tracker.masks(t);
        REQUIRE_FALSE(at_t.empty());
        REQUIRE(no_shrink(at_t, tracker.masks(t2)));
        ++monotone_checks;
      }
    }

    // A positive retention window saw at least one detection delivered on
    // its own frame, so the monotone check above compared real masks.
    if (fed_fresh && config.hold + config.retain >= milliseconds{120}) {
      REQUIRE(monotone_checks > 0);
    }
  }
}
