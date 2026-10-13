// Tracker and temporal hold (SPG-09): unit tests and seeded property tests.
// Every input is synthetic; time comes from an injected ManualClock.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
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
  CHECK(motions[0] == spg::TrackMotion{spg::Velocity{}, at_ms(0), false, 1});
  // The second confirmation measures 200 px/s right; a far box starts a track.
  tracker.update(at_ms(100),
                 std::vector<Detection>{face(600.0F, 600.0F, 50.0F, 50.0F), face(120.0F, 100.0F, 50.0F, 50.0F)},
                 &motions);
  REQUIRE(motions.size() == 2);
  CHECK(motions[0] == spg::TrackMotion{spg::Velocity{}, at_ms(100), false, 2});
  CHECK(motions[1].track == 1);
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
  CHECK(motions[0] == spg::TrackMotion{spg::Velocity{}, at_ms(280), false, 1});
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
  CHECK(motions[0] == spg::TrackMotion{spg::Velocity{}, at_ms(280), false, 1});

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
    // The gate pairs the sighting on 200 (IoU pairs the later ones, at the
    // predicted box). From 400 on, the detections since that pairing have a
    // velocity of their own, which is reported (see TrackMotion).
    CHECK(motions[0].first_seen == at_ms(ms <= 200 ? 0 : 200));
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

TEST_CASE("tracker: a wrong motion pairing keeps the new object's own motion", "[tracker]") {
  // Face A is seen once at x = 900. Face B, a different face sliding in
  // from the right edge, is seen at x = 1000 200 ms later, within A's reach
  // (2.5 box sizes), and then at x = 980. Without the gate B starts its own
  // track: first seen on 1200, then moving left at 100 px/s, which the
  // scheduler trails back to the right edge for the frames before 1200. The
  // gate pairs B with A's track (first seen on 1000, moving right), so
  // reporting that track's motion would lose the trail. Once B's own
  // detections have a measurement, they are reported instead, and the masks
  // still cover everything the gate-off tracker masks.
  ManualClock clock;
  TrackerConfig off;
  off.motion_gate_per_s = 0.0F;
  const TrackerConfig on;
  Tracker gated(clock, on);
  Tracker plain(clock, off);
  std::vector<spg::TrackMotion> gm;
  std::vector<spg::TrackMotion> pm;
  const auto both = [&](std::int64_t ms, float x) {
    clock.set(at_ms(ms));
    const std::vector<Detection> d{face(x, 300.0F, 40.0F, 40.0F)};
    gated.update(at_ms(ms), d, &gm);
    plain.update(at_ms(ms), d, &pm);
  };
  both(1000, 900.0F);
  both(1200, 1000.0F);
  REQUIRE(gated.tracks().size() == 1);
  REQUIRE(plain.tracks().size() == 2);
  // Right after the pairing B's own motion has no measurement yet; the
  // track's motion only adds to the static box the gate-off tracker reports.
  CHECK(gm[0].first_seen == at_ms(1000));
  CHECK(gm[0].velocity.dx_per_s == 500.0F);
  CHECK(pm[0] == spg::TrackMotion{spg::Velocity{}, at_ms(1200), false, 2});
  both(1400, 980.0F);
  CHECK(pm[0] == spg::TrackMotion{spg::Velocity{-100.0F, 0.0F}, at_ms(1200), true, 2});
  // The same motion as B's own track, on A's track id.
  CHECK(gm[0].track == 1);
  gm[0].track = pm[0].track;
  CHECK(gm[0] == pm[0]);
  for (std::int64_t ms = 1000; ms <= 2400; ms += 10) {
    INFO("frame " << ms);
    CHECK(no_shrink(plain.masks(at_ms(ms)), gated.masks(at_ms(ms))));
  }
}

TEST_CASE("tracker: a late motion pairing bridges from the new object's own box", "[tracker]") {
  // Results land out of order. A face is seen at x = 900 on 1000 and 1400
  // (its track's last box is on 1400). A late result for 1200 has a face at
  // x = 1000: a new face near the old track, which the gate pairs with it.
  // The next result, on 1600, has the new face at x = 1010. Without the gate
  // the new face's own track bridges 1000 to 1010 from 1200; with it, that
  // bridge is still held although the track's last box is A's.
  ManualClock clock;
  TrackerConfig off;
  off.motion_gate_per_s = 0.0F;
  const TrackerConfig on;
  Tracker gated(clock, on);
  Tracker plain(clock, off);
  const auto both = [&](std::int64_t ms, float x) {
    clock.set(at_ms(std::max<std::int64_t>(ms, 1400)));
    const std::vector<Detection> d{face(x, 300.0F, 40.0F, 40.0F)};
    gated.update(at_ms(ms), d);
    plain.update(at_ms(ms), d);
  };
  both(1000, 900.0F);
  both(1400, 900.0F);
  both(1200, 1000.0F);
  REQUIRE(gated.tracks().size() == 1);
  REQUIRE(plain.tracks().size() == 2);
  both(1600, 1010.0F);
  for (std::int64_t ms = 1000; ms <= 2400; ms += 10) {
    INFO("frame " << ms);
    CHECK(no_shrink(plain.masks(at_ms(ms)), gated.masks(at_ms(ms))));
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
  // whose masks do not contain each other. Every box held where it was seen
  // (follow off); the followed track is checked below.
  ManualClock clock;
  TrackerConfig config;
  config.hold = milliseconds{60'000};
  config.margin_px = 0.0F;
  config.margin_ratio = 0.0F;
  config.follow_moving_tracks = false;
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

  // Followed (SPG-12b): the same track stays within the cap. Up to its last
  // detection every box is covered moved to where the track went (between
  // the sightings a and b around t) and the positions it left are released;
  // after it, every box is held where it was seen.
  TrackerConfig followed = config;
  followed.follow_moving_tracks = true;
  Tracker follower(clock, followed);
  for (std::size_t i = 0; i < fed.size(); ++i) {
    follower.update(at_ms(100 * static_cast<std::int64_t>(i)), std::vector<Detection>{fed[i]});
  }
  const auto shifted = [&](std::size_t i, std::size_t to) {
    const Region g = required(fed[i].box.rect, followed);
    const double dx = static_cast<double>(fed[to].box.rect.x) - static_cast<double>(fed[i].box.rect.x);
    return Region{g.x0 + dx, g.y0, g.x1 + dx, g.y1};
  };
  for (const std::int64_t ms : {0, 3'000, 29'950, 59'900, 60'050, 119'900}) {
    INFO("followed, t " << ms << " ms");
    const std::vector<Box> masks = follower.masks(at_ms(ms));
    CHECK(masks.size() <= followed.max_rects_per_track);
    require_source_boxes(masks);
    const std::size_t a = static_cast<std::size_t>(std::min<std::int64_t>(ms / 100, 599));
    const std::size_t b = std::min<std::size_t>(ms % 100 == 0 ? a : a + 1, 599);
    for (std::size_t i = 0; i < fed.size(); ++i) {
      const Timestamp seen = at_ms(100 * static_cast<std::int64_t>(i));
      if (at_ms(ms) < seen || at_ms(ms) > seen + followed.hold) {
        continue;
      }
      if (ms > 59'900 || i == a) {
        CHECK(covered(masks, required(fed[i].box.rect, followed)));
      } else {
        CHECK(covered(masks, shifted(i, a)));
        CHECK(covered(masks, shifted(i, b)));
      }
    }
  }
  // At 29.95 s the first box's position, 4,485 px behind the text, is released.
  CHECK_FALSE(covered(follower.masks(at_ms(29'950)), required(fed[0].box.rect, followed)));

  // One rect per track when capped at 1.
  config.max_rects_per_track = 1;
  Tracker single(clock, config);
  single.update(at_ms(59'800), std::vector<Detection>{face(0.0F, 0.0F, 30.0F, 30.0F)});
  single.update(at_ms(59'900), std::vector<Detection>{face(15.0F, 0.0F, 30.0F, 30.0F)});
  const std::vector<Box> one = single.masks(at_ms(59'900));
  REQUIRE(one.size() == 1);
  CHECK(covered(one, {0.0, 0.0, 45.0, 30.0}));
}

// --- Follow (SPG-12b) -------------------------------------------------------
//
// A line of text whose top edge is at top(ms), detected the way the eval's
// scheduler sees it: two workers' paired cycles on frames 0 and 1 of every
// 12 (60 fps), each result landing 200 ms after its frame, and every frame
// masked when it leaves the 500 ms lookahead, with what has landed by then.

namespace {

struct LineRun {
  std::vector<Rect> truth;                // per frame
  std::vector<std::vector<Box>> masks;    // per frame, at its release
  std::vector<Detection> fed;             // in frame order
  std::vector<std::int64_t> fed_ms;
  std::vector<spg::TrackMotion> motions;  // per fed detection
};

template <typename Top, typename Jitter>
LineRun run_line(const TrackerConfig& config, int frames, int detect_frames, Top top, Jitter jitter) {
  constexpr float kX = 100.0F;
  constexpr float kW = 300.0F;
  constexpr float kH = 30.0F;
  const auto ms_of = [](int f) { return static_cast<std::int64_t>(std::llround(f * 1000.0 / 60.0)); };
  ManualClock clock;
  Tracker tracker(clock, config);
  LineRun run;
  std::vector<int> pending;  // detection frames whose result has not landed
  for (int f = 0; f < detect_frames; ++f) {
    if (f % 12 == 0 || f % 12 == 1) {
      pending.push_back(f);
    }
  }
  std::size_t next = 0;
  for (int f = 0; f < frames; ++f) {
    const std::int64_t release = ms_of(f) + 500;
    while (next < pending.size() && ms_of(pending[next]) + 200 <= release) {
      const int d = pending[next++];
      clock.set(at_ms(ms_of(d) + 200));
      const auto [dy, dh] = jitter(d);
      const Detection det = text(kX, static_cast<float>(top(ms_of(d)) + dy), kW, kH + static_cast<float>(dh));
      std::vector<spg::TrackMotion> motions;
      tracker.update(at_ms(ms_of(d)), std::vector<Detection>{det}, &motions);
      run.fed.push_back(det);
      run.fed_ms.push_back(ms_of(d));
      run.motions.push_back(motions.at(0));
    }
    clock.set(at_ms(release));
    run.truth.push_back(Rect{kX, static_cast<float>(top(ms_of(f))), kW, kH});
    run.masks.push_back(tracker.masks(at_ms(ms_of(f))));
  }
  REQUIRE(tracker.tracks().size() == 1);
  return run;
}

// A pixel of box jitter on most cycles, in a fixed pattern.
std::pair<double, double> small_jitter(int f) {
  constexpr double kDy[] = {0.0, 1.0, -1.0, 0.5, -0.5, 1.5};
  constexpr double kDh[] = {0.0, -1.0, 1.0, 0.0, 2.0, -0.5};
  const auto k = static_cast<std::size_t>(f % 6);
  return {kDy[k], kDh[k]};
}

}  // namespace

TEST_CASE("tracker: a scrolling line is covered where it is on every frame", "[tracker]") {
  // A chat line scrolls up at 90 px/s, 34 px between lines, for 2 s. The
  // next line down sits where this one was 0.38 s before.
  const auto top = [](std::int64_t ms) { return 600.0 - 0.09 * static_cast<double>(ms); };
  TrackerConfig on;
  TrackerConfig off;
  off.follow_moving_tracks = false;
  const LineRun followed = run_line(on, 150, 121, top, small_jitter);
  const LineRun held = run_line(off, 150, 121, top, small_jitter);
  const int last = 120;  // the last detection frame
  int below_followed = 0;
  int below_held = 0;
  for (std::size_t f = 0; f < followed.truth.size(); ++f) {
    INFO("frame " << f);
    const Rect& line = followed.truth[f];
    CHECK(covered(followed.masks[f], region_of(line)));
    CHECK(covered(held.masks[f], region_of(line)));
    if (static_cast<int>(f) > last) {
      // After the last detection every box holds where it was seen.
      for (std::size_t i = 0; i < followed.fed.size(); ++i) {
        const Timestamp seen = at_ms(followed.fed_ms[i]);
        const Timestamp t = at_ms(static_cast<std::int64_t>(std::llround(static_cast<double>(f) * 1000.0 / 60.0)));
        if (t >= seen && t <= seen + on.hold) {
          CHECK(covered(followed.masks[f], required(followed.fed[i].box.rect, on)));
        }
      }
      continue;
    }
    const Region next_line{line.x, line.y + 34.0, line.x + line.width, line.y + 34.0 + line.height};
    below_followed += covered(followed.masks[f], next_line) ? 1 : 0;
    below_held += covered(held.masks[f], next_line) ? 1 : 0;
  }
  // Held where they were seen, the boxes blur the next line on most frames;
  // followed, on none.
  INFO("next line blurred on " << below_held << " frames held, " << below_followed << " followed");
  CHECK(below_held > 60);
  CHECK(below_followed == 0);
}

TEST_CASE("tracker: a stopped track holds every box where it was seen", "[tracker]") {
  // The line scrolls for 1 s, then stops and stays detected for 1 s more.
  // While it scrolls it is followed; once it has stopped it is no longer
  // confirmed moving, and on every frame every box is covered where it was
  // seen for the whole hold. Retention is long so the whole history can be
  // asked about at the end.
  const auto top = [](std::int64_t ms) {
    return 600.0 - 0.09 * static_cast<double>(std::min<std::int64_t>(ms, 1000));
  };
  TrackerConfig config;
  config.retain = std::chrono::seconds{10};
  ManualClock clock;
  Tracker tracker(clock, config);
  std::vector<std::pair<std::int64_t, Detection>> fed;
  for (int f = 0; f < 121; ++f) {
    if (f % 12 != 0 && f % 12 != 1) {
      continue;
    }
    const std::int64_t ms = std::llround(f * 1000.0 / 60.0);
    const auto [dy, dh] = small_jitter(f);
    const Detection d = text(100.0F, static_cast<float>(top(ms) + dy), 300.0F, 30.0F + static_cast<float>(dh));
    clock.set(at_ms(ms + 200));
    tracker.update(at_ms(ms), std::vector<Detection>{d});
    fed.emplace_back(ms, d);
    if (ms == 1000) {
      // Still scrolling: the first box's position is released.
      CHECK_FALSE(covered(tracker.masks(at_ms(500)), required(fed.front().second.box.rect, config)));
    }
  }
  for (std::int64_t ms = 0; ms <= 2'750; ms += 10) {
    const std::vector<Box> masks = tracker.masks(at_ms(ms));
    for (const auto& [seen, d] : fed) {
      if (ms >= seen && ms <= seen + 750) {
        INFO("t " << ms << " ms, box seen at " << seen << " ms");
        CHECK(covered(masks, required(d.box.rect, config)));
      }
    }
  }
}

TEST_CASE("tracker: a wrong motion estimate never uncovers the text", "[tracker]") {
  TrackerConfig config;
  config.velocity_smoothing = 1.0F;  // the latest measurement only
  SECTION("paired cycles whose second box jitters against the scroll") {
    // The second result of each pair reads the line 3 px low: over 17 ms that
    // measures the line moving down at about 90 px/s while it scrolls up.
    const auto top = [](std::int64_t ms) { return 600.0 - 0.09 * static_cast<double>(ms); };
    const auto against = [](int f) { return std::pair<double, double>{f % 12 == 1 ? 3.0 : 0.0, 0.0}; };
    const LineRun run = run_line(config, 150, 121, top, against);
    bool wrong_way = false;
    for (const spg::TrackMotion& m : run.motions) {
      wrong_way = wrong_way || m.velocity.dy_per_s > 0.0F;
    }
    CHECK(wrong_way);
    for (std::size_t f = 0; f < run.truth.size(); ++f) {
      INFO("frame " << f);
      CHECK(covered(run.masks[f], region_of(run.truth[f])));
    }
  }
  SECTION("an ease-out slide the measured velocity lags") {
    // 400 px up in 1.5 s, fastest first (cubic ease-out), then at rest.
    const auto top = [](std::int64_t ms) {
      const double u = std::min(1.0, static_cast<double>(ms) / 1500.0);
      return 800.0 - 400.0 * (1.0 - std::pow(1.0 - u, 3.0));
    };
    const LineRun run = run_line(config, 150, 121, top, small_jitter);
    for (std::size_t f = 0; f < run.truth.size(); ++f) {
      INFO("frame " << f);
      CHECK(covered(run.masks[f], region_of(run.truth[f])));
    }
  }
}

namespace {

// Text boxes fed in frame-time order the way the scheduler feeds them: each
// result lands 200 ms after its frame, and a frame (every 10 ms from 0 to
// end_ms) is masked when it leaves the 500 ms lookahead, with what has landed
// by then.
struct Replay {
  std::vector<std::vector<Box>> released;  // per frame, at its release
  std::vector<spg::TrackMotion> motions;   // per fed box
};

// Each cycle is a frame time and the boxes found on it.
Replay replay_lines(const TrackerConfig& config,
                    const std::vector<std::pair<std::int64_t, std::vector<Rect>>>& cycles,
                    std::int64_t end_ms) {
  ManualClock clock;
  Tracker tracker(clock, config);
  Replay out;
  std::size_t next = 0;
  for (std::int64_t ms = 0; ms <= end_ms; ms += 10) {
    const std::int64_t release = ms + 500;
    while (next < cycles.size() && cycles[next].first + 200 <= release) {
      const auto& [seen, boxes] = cycles[next++];
      clock.set(at_ms(seen + 200));
      std::vector<Detection> detections;
      for (const Rect& b : boxes) {
        detections.push_back(text(b.x, b.y, b.width, b.height));
      }
      std::vector<spg::TrackMotion> motions;
      tracker.update(at_ms(seen), detections, &motions);
      out.motions.insert(out.motions.end(), motions.begin(), motions.end());
    }
    clock.set(at_ms(release));
    out.released.push_back(tracker.masks(at_ms(ms)));
  }
  return out;
}

// One box per cycle.
Replay replay(const TrackerConfig& config, const std::vector<std::pair<std::int64_t, Rect>>& cycles,
              std::int64_t end_ms) {
  std::vector<std::pair<std::int64_t, std::vector<Rect>>> lines;
  for (const auto& [seen, box] : cycles) {
    lines.push_back({seen, {box}});
  }
  return replay_lines(config, lines, end_ms);
}

}  // namespace

TEST_CASE("tracker: a static line paired into a moving run stays covered where it was seen", "[tracker]") {
  // SPG-12b staff review: a static watch-listed line shares a track with a
  // line that scrolls. Its boxes must hold where they were seen, or the run
  // must break, so the static line is covered on every frame of its hold as
  // with follow off.
  const TrackerConfig config;
  TrackerConfig off;
  off.follow_moving_tracks = false;
  SECTION("a moving line joins the static line's track") {
    // S rests at y = 500, found on 0, 100 and 200 ms, then missed (the hold
    // exists for misses). M scrolls up at 40 px/s; it was over S on 0 ms
    // and is first found 12 px above it on 300 ms, where IoU (0.43) pairs it
    // into S's track. Measured from S's first box, M's moves look like one
    // steady scroll from 0 ms, and the track is confirmed moving on 400 ms.
    // S rested on 100 and 200 ms, so its boxes are not part of that run.
    const Rect s{100.0F, 500.0F, 300.0F, 30.0F};
    const auto m = [](std::int64_t ms) {
      return Rect{100.0F, 500.0F - 0.04F * static_cast<float>(ms), 300.0F, 30.0F};
    };
    std::vector<std::pair<std::int64_t, Rect>> cycles{{0, s}, {100, s}, {200, s}};
    for (std::int64_t ms = 300; ms <= 1500; ms += 100) {
      cycles.emplace_back(ms, m(ms));
    }
    const Replay followed = replay(config, cycles, 1500);
    const Replay held = replay(off, cycles, 1500);
    for (const spg::TrackMotion& motion : followed.motions) {
      CHECK(motion.track == 1);  // one track: M joined S's
    }
    for (std::size_t f = 0; f < followed.released.size(); ++f) {
      const auto ms = static_cast<std::int64_t>(10 * f);
      INFO("frame " << ms << " ms");
      if (ms <= 950) {  // S's last box, seen on 200 ms, holds until 950 ms
        CHECK(covered(held.released[f], region_of(s)));
        CHECK(covered(followed.released[f], region_of(s)));
      }
      if (ms >= 300) {
        CHECK(covered(followed.released[f], region_of(m(ms))));
      }
    }
    // M's own run is followed: on 1200 ms its box from 600 ms has moved on
    // with it, where held it stays.
    CHECK(covered(held.released[120], required(m(600), config)));
    CHECK_FALSE(covered(followed.released[120], required(m(600), config)));
  }
  SECTION("the static line joins a moving line's track") {
    // M scrolls up at 40 px/s from y = 600, found every 100 ms until 500 ms,
    // then missed. S rests at y = 576, where M's run puts it on 600 ms, and
    // is found from 600 to 1000 ms: it joins M's track and its first box
    // even counts as a steady move. Each of S's boxes moves only by S's own
    // measured move (none), and the run ends when S has not moved for
    // 250 ms.
    const auto m = [](std::int64_t ms) {
      return Rect{100.0F, 600.0F - 0.04F * static_cast<float>(ms), 300.0F, 30.0F};
    };
    const Rect s{100.0F, 576.0F, 300.0F, 30.0F};
    std::vector<std::pair<std::int64_t, Rect>> cycles;
    for (std::int64_t ms = 0; ms <= 500; ms += 100) {
      cycles.emplace_back(ms, m(ms));
    }
    for (std::int64_t ms = 600; ms <= 1000; ms += 100) {
      cycles.emplace_back(ms, s);
    }
    const Replay followed = replay(config, cycles, 1750);
    for (const spg::TrackMotion& motion : followed.motions) {
      CHECK(motion.track == 1);  // one track: S joined M's
    }
    for (std::size_t f = 60; f < followed.released.size(); ++f) {
      INFO("frame " << 10 * f << " ms");
      CHECK(covered(followed.released[f], region_of(s)));
    }
  }
}

TEST_CASE("tracker: a sighting gap past the hold never covers less than held where seen", "[tracker]") {
  // SPG-12b staff review: a run cannot follow a track across a gap longer
  // than the hold. A line slides right at 20 px/s, found every 100 ms until
  // 500 ms, then after a gap, then every 100 ms until 2000 ms. A box found
  // more than `hold` after the track's last one starts a new track, so a
  // confirmed run never spans such a gap, and on every frame the followed
  // tracker covers the line wherever the tracker that holds every box where
  // it was seen does.
  const auto line = [](std::int64_t ms) {
    return Rect{100.0F + 0.02F * static_cast<float>(ms), 500.0F, 300.0F, 30.0F};
  };
  TrackerConfig off;
  off.follow_moving_tracks = false;
  for (const std::int64_t gap : {750, 760}) {
    INFO("gap " << gap << " ms");
    std::vector<std::pair<std::int64_t, Rect>> cycles;
    for (std::int64_t ms = 0; ms <= 500; ms += 100) {
      cycles.emplace_back(ms, line(ms));
    }
    for (std::int64_t ms = 500 + gap; ms <= 2000; ms += 100) {
      cycles.emplace_back(ms, line(ms));
    }
    const Replay followed = replay(TrackerConfig{}, cycles, 2750);
    const Replay held = replay(off, cycles, 2750);
    // Within the hold the run goes on (one track); past it a new track starts.
    CHECK(followed.motions.at(6).track == (gap <= 750 ? 1U : 2U));
    int frames_held = 0;
    for (std::size_t f = 0; f < followed.released.size(); ++f) {
      const auto ms = static_cast<std::int64_t>(10 * f);
      INFO("frame " << ms << " ms");
      if (covered(held.released[f], region_of(line(ms)))) {
        ++frames_held;
        CHECK(covered(followed.released[f], region_of(line(ms))));
      }
      if (gap <= 750 && ms <= 2000) {
        // One run bridges the gap: covered on every frame up to the last box.
        CHECK(covered(followed.released[f], region_of(line(ms))));
      }
    }
    CHECK(frames_held > 150);
  }
}

TEST_CASE("tracker: a resize mid-run holds the boxes from before it where they were seen", "[tracker]") {
  // SPG-12b staff review: a line scrolls up at 60 px/s and its track is
  // confirmed moving when the source is resized from 1920x1080 to 1280x720.
  // Every box from before the resize is held where it was seen, as it was
  // and scaled, for its whole hold; the run starts again from the boxes
  // found in the new size.
  TrackerConfig config;
  config.retain = std::chrono::seconds{10};
  ManualClock clock;
  Tracker tracker(clock, config);
  const auto line = [](std::int64_t ms, double scale) {
    return Rect{static_cast<float>(100.0 * scale),
                static_cast<float>((600.0 - 0.06 * static_cast<double>(ms)) * scale),
                static_cast<float>(300.0 * scale), static_cast<float>(30.0 * scale)};
  };
  const auto feed = [&](std::int64_t ms, const Rect& box) {
    clock.set(at_ms(ms + 200));
    tracker.update(at_ms(ms), std::vector<Detection>{text(box.x, box.y, box.width, box.height)});
  };
  std::vector<std::pair<std::int64_t, Rect>> before;
  for (std::int64_t ms = 0; ms <= 500; ms += 100) {
    feed(ms, line(ms, 1.0));
    before.emplace_back(ms, line(ms, 1.0));
  }
  // Confirmed moving: on 450 ms the first box has moved on with the line.
  CHECK_FALSE(covered(tracker.masks(at_ms(450)), required(line(0, 1.0), config)));
  clock.set(at_ms(700));
  tracker.rescale(Size{1920, 1080}, Size{1280, 720});
  constexpr double kScale = 2.0 / 3.0;
  for (std::int64_t ms = 600; ms <= 2000; ms += 100) {
    feed(ms, line(ms, kScale));
  }
  REQUIRE(tracker.tracks().size() == 1);
  for (std::int64_t ms = 0; ms <= 2750; ms += 10) {
    INFO("frame " << ms << " ms");
    const std::vector<Box> masks = tracker.masks(at_ms(ms));
    for (const auto& [seen, box] : before) {
      if (ms >= seen && ms <= seen + 750) {
        CHECK(covered(masks, required(box, config)));
        CHECK(covered(masks, scaled(required(box, config), kScale, kScale)));
      }
    }
    if (ms >= 600 && ms <= 2000) {
      CHECK(covered(masks, region_of(line(ms, kScale))));
    }
  }
  // The run goes on in the new size: on 1800 ms the box from 1100 ms has
  // moved on with the line.
  CHECK_FALSE(covered(tracker.masks(at_ms(1800)), required(line(1100, kScale), config)));
}

TEST_CASE("tracker: a neighbouring line that takes over a fast track leaves the text's boxes held",
          "[tracker]") {
  // SPG-12b staff review: line S scrolls up at 150 to 300 px/s, 34 px above
  // line N. S is found until 600 ms, then missed; after a gap only N is found
  // (and, in one variant, S again from 200 ms later). Past max_prediction the
  // track's prediction stops, so N can win S's track by IoU with it, and N's
  // move, 34 px short of what the run predicts, can still be within half of
  // a long gap's move. The run must break there: S's boxes hold where they
  // were seen for their whole hold, and on every frame where holding every
  // box where it was seen covers S, following covers it too.
  constexpr std::int64_t kLastS = 600;
  constexpr std::int64_t kEnd = 2000;
  int scenes = 0;
  int takeovers = 0;
  int compared = 0;
  int uncovered = 0;
  int moved = 0;
  std::string first_failure;
  for (const float smoothing : {0.5F, 1.0F}) {
    TrackerConfig on;
    on.velocity_smoothing = smoothing;
    TrackerConfig off = on;
    off.follow_moving_tracks = false;
    for (const double speed : {150.0, 200.0, 250.0, 300.0}) {
      const auto s = [&](std::int64_t ms) {
        return Rect{100.0F, static_cast<float>(900.0 - speed * static_cast<double>(ms) / 1000.0), 300.0F, 30.0F};
      };
      // One worker every 100 ms, or two workers' paired cycles every 200 ms.
      for (const bool paired : {false, true}) {
        std::vector<std::int64_t> times;
        for (std::int64_t base = 0; base <= kEnd; base += paired ? 200 : 100) {
          times.push_back(base);
          if (paired) {
            times.push_back(base + 17);
          }
        }
        for (std::int64_t gap = 100; gap <= 700; gap += 25) {
          for (const bool s_returns : {false, true}) {
            // No jitter, small_jitter, or every other box read 3 px low.
            for (const int jitter : {0, 1, 2}) {
              std::vector<std::pair<std::int64_t, std::vector<Rect>>> cycles;
              std::vector<std::pair<std::int64_t, Rect>> s_fed;
              std::int64_t takeover_at = -1;
              std::size_t takeover_index = 0;  // N's first box among all fed
              std::size_t fed = 0;
              for (std::size_t k = 0; k < times.size(); ++k) {
                const std::int64_t ms = times[k];
                std::pair<double, double> j{0.0, 0.0};
                if (jitter == 1) {
                  j = small_jitter(static_cast<int>(k));
                } else if (jitter == 2) {
                  j = {k % 2 == 1 ? 3.0 : 0.0, 0.0};
                }
                Rect sr = s(ms);
                sr.y += static_cast<float>(j.first);
                sr.height += static_cast<float>(j.second);
                Rect nr = sr;
                nr.y += 34.0F;
                if (ms <= kLastS) {
                  cycles.push_back({ms, {sr}});
                  s_fed.emplace_back(ms, sr);
                  fed += 1;
                } else if (ms >= kLastS + gap) {
                  if (takeover_at < 0) {
                    takeover_at = ms;
                    takeover_index = fed;
                  }
                  cycles.push_back({ms, s_returns && ms >= takeover_at + 200 ? std::vector<Rect>{nr, sr}
                                                                             : std::vector<Rect>{nr}});
                  fed += cycles.back().second.size();
                }
              }
              const Replay followed = replay_lines(on, cycles, kEnd + 750);
              const Replay held = replay_lines(off, cycles, kEnd + 750);
              ++scenes;
              takeovers += followed.motions.at(takeover_index).track == 1 ? 1 : 0;
              for (std::size_t f = 0; f < followed.released.size(); ++f) {
                const auto ms = static_cast<std::int64_t>(10 * f);
                const auto fail = [&](int& count, const char* what) {
                  if (count++ == 0 && first_failure.empty()) {
                    first_failure = std::string(what) + ": smoothing " + std::to_string(smoothing) + ", " +
                                    std::to_string(static_cast<int>(speed)) + " px/s, paired " +
                                    std::to_string(paired) + ", gap " + std::to_string(gap) + " ms, S returns " +
                                    std::to_string(s_returns) + ", jitter " + std::to_string(jitter) + ", frame " +
                                    std::to_string(ms) + " ms";
                  }
                };
                if (ms > kLastS) {
                  for (const auto& [seen, box] : s_fed) {
                    if (ms <= seen + 750 && !covered(followed.released[f], required(box, on))) {
                      fail(moved, "S box not held where seen");
                    }
                  }
                }
                if (covered(held.released[f], region_of(s(ms)))) {
                  ++compared;
                  if (!covered(followed.released[f], region_of(s(ms)))) {
                    fail(uncovered, "S uncovered where held covers it");
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  INFO("scenes " << scenes << ", N took S's track in " << takeovers << ", frames compared " << compared);
  INFO(first_failure);
  CHECK(moved == 0);
  CHECK(uncovered == 0);
  CHECK(takeovers > scenes / 2);
  CHECK(compared > 100'000);
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
  spg::TrackId track = 0;  // the track it joined
  bool forward = false;    // newer than every detection of its track before it
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

namespace {

constexpr std::uint32_t kScenes = 2000;

struct SceneCounts {
  int monotone_checks = 0;  // non-empty monotone-in-time comparisons
  int released = 0;         // held boxes followed away from where they were seen
};

Region hull_of(const Region& a, const Region& b) {
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

// `g` (around `from`) moved by the move of the box center from `from` to `to`.
Region moved_with(const Region& g, const Rect& from, const Rect& to) {
  const double dx = (static_cast<double>(to.x) + static_cast<double>(to.width) / 2.0) -
                    (static_cast<double>(from.x) + static_cast<double>(from.width) / 2.0);
  const double dy = (static_cast<double>(to.y) + static_cast<double>(to.height) / 2.0) -
                    (static_cast<double>(from.y) + static_cast<double>(from.height) / 2.0);
  return {g.x0 + dx, g.y0 + dy, g.x1 + dx, g.y1 + dy};
}

// One seeded scene. With `follow` off (follow_moving_tracks false) every box
// must hold where it was seen and masks never shrink, as before SPG-12b.
// With it on, a box not covered where it was seen must be a release the
// Follow guarantee allows (spg/tracker.hpp): a detection that moved its track
// forward, seen before that track's latest such sighting a at or before t,
// with another b at or after t, both in the current size. Then the hull of a
// and b and the box moved to a and to b, each with its margin, must be
// covered. Masks only grow on frame times after every detection.
void run_scene(std::uint32_t seed, bool follow, SceneCounts& counts) {
  constexpr std::int64_t kFrameNs = 16'666'667;  // 60 fps
  {
    INFO("seed " << seed << (follow ? ", follow" : ""));
    Scene scene(seed);
    TrackerConfig config = scene.config();
    config.follow_moving_tracks = follow;
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
    std::vector<spg::TrackMotion> motions;
    Timestamp newest = Timestamp::min();  // the latest frame time fed
    // Records a delivered batch with the track each detection joined.
    const auto record = [&](Timestamp when, const std::vector<Detection>& batch) {
      for (std::size_t j = 0; j < batch.size(); ++j) {
        bool forward = true;
        for (const Fed& fd : fed) {
          forward = forward && !(fd.track == motions[j].track && fd.seen >= when);
        }
        fed.push_back(Fed{batch[j].box.rect, when, false, motions[j].track, forward});
        newest = std::max(newest, when);
      }
    };

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
          tracker.update(when, batch, &motions);
          record(when, batch);
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
        // Following, a frame time up to a track's last detection may have had
        // boxes moved to where it went; the resize ends the run and holds
        // them where they were seen again (checked with the hold below).
        if (!follow || probe > newest) {
          REQUIRE(no_shrink(before, tracker.masks(probe)));
          REQUIRE(no_shrink_scaled(before, tracker.masks(probe), sx, sy));
        }
        REQUIRE(frame_time > newest);
        REQUIRE(no_shrink(before_now, tracker.masks(frame_time)));
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
        tracker.update(when, batch, &motions);
        record(when, batch);
        fed_fresh = fed_fresh || (!batch.empty() && when == frame_time);
        for (std::size_t k = 0; k < probes.size(); ++k) {
          if (follow && probes[k] <= newest) {
            continue;  // a followed box may move up to its track's last detection
          }
          INFO("update at frame " << f << ", probe " << k);
          REQUIRE(no_shrink(before[k], tracker.masks(probes[k])));
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
          INFO("frame " << f << ", detection " << i);
          const std::vector<Region> regions = expected_regions(fed[i], sx, sy, config);
          const bool in_place =
              std::all_of(regions.begin(), regions.end(), [&](const Region& g) { return covered(masks, g); });
          if (!follow || in_place) {
            for (const Region& g : regions) {
              REQUIRE(covered(masks, g));
            }
            continue;
          }
          // Released: only the Follow guarantee's case, covered where the
          // track went.
          const Fed& d = fed[i];
          const Fed* a = nullptr;
          const Fed* b = nullptr;
          for (const Fed& fd : fed) {
            if (fd.track != d.track || !fd.forward) {
              continue;
            }
            if (fd.seen <= t && (a == nullptr || fd.seen > a->seen)) {
              a = &fd;
            }
            if (fd.seen >= t && (b == nullptr || fd.seen < b->seen)) {
              b = &fd;
            }
          }
          REQUIRE(d.forward);
          REQUIRE_FALSE(d.before_resize);
          REQUIRE(a != nullptr);
          REQUIRE(b != nullptr);
          REQUIRE_FALSE(a->before_resize);
          REQUIRE(d.seen < a->seen);
          REQUIRE(t <= a->seen + config.hold);
          const Region own = required(d.box, config);
          REQUIRE(covered(masks, hull_of(required(a->box, config), required(b->box, config))));
          REQUIRE(covered(masks, moved_with(own, d.box, a->box)));
          REQUIRE(covered(masks, moved_with(own, d.box, b->box)));
          ++counts.released;
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
        if (follow && t <= newest) {
          continue;  // a followed box may move up to its track's last detection
        }
        INFO("frame " << f << ", t " << spg::to_ns(t) << " -> " << spg::to_ns(t2));
        const std::vector<Box> at_t = tracker.masks(t);
        REQUIRE_FALSE(at_t.empty());
        REQUIRE(no_shrink(at_t, tracker.masks(t2)));
        ++monotone_checks;
      }
    }

    // A positive retention window saw at least one detection delivered on
    // its own frame, so the monotone check above compared real masks. With
    // follow on, the checks are counted over all scenes instead.
    if (!follow && fed_fresh && config.hold + config.retain >= milliseconds{120}) {
      REQUIRE(monotone_checks > 0);
    }
    counts.monotone_checks += monotone_checks;
  }
}

}  // namespace

TEST_CASE("tracker: masks never shrink before hold expiry (property)", "[tracker][property]") {
  for (std::uint32_t seed = 1; seed <= kScenes; ++seed) {
    SceneCounts counts;
    run_scene(seed, false, counts);
  }
}

TEST_CASE("tracker: a followed track stays covered where it went (property)", "[tracker][property]") {
  SceneCounts counts;
  for (std::uint32_t seed = 1; seed <= kScenes; ++seed) {
    run_scene(seed, true, counts);
  }
  // The scenes exercise both: boxes followed away from where they were seen,
  // and masks compared after every detection.
  INFO("released " << counts.released << ", monotone checks " << counts.monotone_checks);
  CHECK(counts.released > 0);
  CHECK(counts.monotone_checks > 0);
}
