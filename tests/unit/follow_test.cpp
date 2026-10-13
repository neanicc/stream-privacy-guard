// Following's checks (SPG-12b, #75), tested directly. Tracker::masks()
// applies them; tracker_test.cpp covers the rule end to end. Every input is
// synthetic.
#include <chrono>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "spg/geometry.hpp"
#include "spg/time.hpp"
#include "track/follow.hpp"

namespace {

using spg::Rect;
using spg::Timestamp;
using spg::Velocity;
using spg::follow::active;
using spg::follow::contains_moved;
using spg::follow::Target;

Timestamp at_ms(std::int64_t ms) {
  return spg::timestamp_from_ns(ms * 1'000'000);
}

// A 200 x 40 line at y = 500 on 0 ms, scrolling up at 100 px/s: on 200 ms it
// is at y = 480.
const Rect kOlder{100.0F, 500.0F, 200.0F, 40.0F};
const Velocity kUp{0.0F, -100.0F};

Target newest_at(const Rect& box, double margin) {
  return Target{box, at_ms(200), margin, margin};
}

}  // namespace

TEST_CASE("follow: following is active only on a confirmed run still being updated",
          "[tracker][follow]") {
  const Timestamp confirmed = at_ms(400);
  // Confirmed moving (run >= follow_after), on or before the last confirmation.
  CHECK(active(2, 2, at_ms(400), confirmed));
  CHECK(active(2, 5, at_ms(250), confirmed));
  CHECK(active(1, 1, at_ms(0), confirmed));
  // Following off: never, however long the run.
  CHECK_FALSE(active(0, 0, at_ms(400), confirmed));
  CHECK_FALSE(active(0, 64, at_ms(400), confirmed));
  // Not confirmed moving yet.
  CHECK_FALSE(active(2, 0, at_ms(400), confirmed));
  CHECK_FALSE(active(2, 1, at_ms(400), confirmed));
  CHECK_FALSE(active(3, 2, at_ms(400), confirmed));
  // After the last confirmation: the track may be lost.
  CHECK_FALSE(active(2, 2, at_ms(401), confirmed));
  CHECK_FALSE(active(2, 64, at_ms(800), confirmed));
}

TEST_CASE("follow: the newest box must contain the older one moved along the run",
          "[tracker][follow]") {
  // Exactly where the older box moved to: contained, edges touching.
  const Rect moved{100.0F, 480.0F, 200.0F, 40.0F};
  CHECK(contains_moved(newest_at(moved, 0.0), kOlder, at_ms(0), kUp, kUp));
  // Unmoved (as with following's velocity ignored) it sticks out below.
  CHECK_FALSE(contains_moved(newest_at(moved, 0.0), kOlder, at_ms(0), Velocity{}, Velocity{}));
  // The same frame time: plain containment.
  CHECK(contains_moved(Target{kOlder, at_ms(0), 0.0, 0.0}, kOlder, at_ms(0), kUp, kUp));
  // A newest box one pixel off on any side no longer contains it with no
  // margin; a one-pixel margin absorbs it.
  for (const Rect& off : {Rect{101.0F, 480.0F, 200.0F, 40.0F}, Rect{99.0F, 480.0F, 200.0F, 40.0F},
                          Rect{100.0F, 481.0F, 200.0F, 40.0F}, Rect{100.0F, 479.0F, 200.0F, 40.0F}}) {
    INFO("newest at " << off.x << ", " << off.y);
    CHECK_FALSE(contains_moved(newest_at(off, 0.0), kOlder, at_ms(0), kUp, kUp));
    CHECK(contains_moved(newest_at(off, 1.0), kOlder, at_ms(0), kUp, kUp));
  }
}

TEST_CASE("follow: a newest box clipped on any side never contains the full older box",
          "[tracker][follow]") {
  // The detector found only part of the line on 200 ms: 20 px (a tenth of
  // the width) or 4 px (a tenth of the height) cut off one side. The older
  // full box moved along the run sticks out on that side unless the margin
  // covers the clip.
  struct Clip {
    const char* side;
    Rect newest;
    double clip;
  };
  const Clip clips[] = {
      {"left", {120.0F, 480.0F, 180.0F, 40.0F}, 20.0},
      {"right", {100.0F, 480.0F, 180.0F, 40.0F}, 20.0},
      {"top", {100.0F, 484.0F, 200.0F, 36.0F}, 4.0},
      {"bottom", {100.0F, 480.0F, 200.0F, 36.0F}, 4.0},
  };
  for (const Clip& c : clips) {
    INFO(c.side);
    CHECK_FALSE(contains_moved(newest_at(c.newest, 0.0), kOlder, at_ms(0), kUp, kUp));
    CHECK_FALSE(contains_moved(newest_at(c.newest, c.clip - 0.5), kOlder, at_ms(0), kUp, kUp));
    CHECK(contains_moved(newest_at(c.newest, c.clip), kOlder, at_ms(0), kUp, kUp));
  }
}

TEST_CASE("follow: the older box is swept over the run's whole velocity range",
          "[tracker][follow]") {
  // The run measured 50 to 150 px/s up. Over 200 ms the older box's top edge
  // may be anywhere from 470 to 490 and its bottom from 530 to 550: the
  // sweep spans y in [470, 530] (top at the high speed, bottom at the low
  // one).
  const Velocity fast{0.0F, -150.0F};
  const Velocity slow{0.0F, -50.0F};
  const Rect at_mean{100.0F, 480.0F, 200.0F, 40.0F};  // where 100 px/s puts it
  // The low end of the range (most negative) moves the leading edge, the
  // high end the trailing edge: a 10 px margin covers both.
  CHECK(contains_moved(newest_at(at_mean, 10.0), kOlder, at_ms(0), fast, slow));
  CHECK_FALSE(contains_moved(newest_at(at_mean, 9.0), kOlder, at_ms(0), fast, slow));
  // Either end alone is narrower than the range.
  CHECK(contains_moved(newest_at(Rect{100.0F, 470.0F, 200.0F, 40.0F}, 0.0), kOlder, at_ms(0), fast,
                       fast));
  CHECK(contains_moved(newest_at(Rect{100.0F, 490.0F, 200.0F, 40.0F}, 0.0), kOlder, at_ms(0), slow,
                       slow));
  CHECK_FALSE(contains_moved(newest_at(Rect{100.0F, 470.0F, 200.0F, 40.0F}, 0.0), kOlder, at_ms(0),
                             fast, slow));
  CHECK_FALSE(contains_moved(newest_at(Rect{100.0F, 490.0F, 200.0F, 40.0F}, 0.0), kOlder, at_ms(0),
                             fast, slow));
  // On x the same: low.dx moves the left edge, high.dx the right edge.
  const Rect wide{90.0F, 480.0F, 220.0F, 40.0F};
  CHECK(contains_moved(newest_at(wide, 0.0), kOlder, at_ms(0), Velocity{-50.0F, -100.0F},
                       Velocity{50.0F, -100.0F}));
  CHECK_FALSE(contains_moved(newest_at(wide, 0.0), kOlder, at_ms(0), Velocity{-55.0F, -100.0F},
                             Velocity{50.0F, -100.0F}));
  CHECK_FALSE(contains_moved(newest_at(wide, 0.0), kOlder, at_ms(0), Velocity{-50.0F, -100.0F},
                             Velocity{55.0F, -100.0F}));
}
