#pragma once

// Following (SPG-12b, #75): the two checks Tracker::masks() makes before it
// lets a held detection go behind a newer one, apart from the tracker so they
// can be unit-tested directly (tests/unit/follow_test.cpp).
//
// Keep in sync. The rule has one statement, docs/tracker.md ("Following"),
// and two implementations that must change with it:
//   * this file and Tracker::follow_target() / Tracker::followed()
//     (core/src/track/tracker.cpp), which apply it;
//   * the property test's oracle `may_follow` (tests/unit/tracker_test.cpp),
//     which restates its order, resize, miss and run-length conditions
//     independently, from what the test delivered rather than from the
//     tracker's run state, and of contains_moved() the part that holds at any
//     velocity: the newest box grown by its margin is at least as large on
//     each axis as the older box grown by its own.

#include <cstddef>

#include "spg/detection.hpp"
#include "spg/geometry.hpp"
#include "spg/time.hpp"

namespace spg::follow {

// Following can apply to a track on frame time `t`: it is on (`follow_after`
// is not 0), the track's current run has at least `follow_after` consistent
// measurements (it is confirmed moving), and the track was confirmed at or
// after `t` (`last_confirmed`), so a later scan found it again. Otherwise
// every held detection of the track keeps its whole hold on `t`.
[[nodiscard]] bool active(std::size_t follow_after, std::size_t run, Timestamp t,
                          Timestamp last_confirmed) noexcept;

// A held detection as the containment check sees it: its box, the frame
// time it was seen on and its margin per axis, in Source pixels.
struct Detected {
  Rect box;
  Timestamp seen{};
  double margin_x = 0.0;
  double margin_y = 0.0;
};

// The containment check. `newest` is the newest detection of the run seen at
// or before the frame time asked about, the one older detections of the run
// are followed into. It returns whether `newest`'s box, grown by its margin,
// contains `older`'s box grown by `older`'s own margin and moved to
// newest.seen at every velocity between `low` and `high`, per axis (edges may
// touch; older.seen is at or before newest.seen). Growing the older box by
// its own margin means following never removes coverage that box's mask
// provided around the text: its margin moves with it into the newest mask.
// The whole range is used, not the latest measurement alone, so a newest box
// that found only part of the text (clipped on one side, which also biased
// the measurement taken to it) never contains the older full box.
[[nodiscard]] bool contains_moved(const Detected& newest, const Detected& older, Velocity low,
                                  Velocity high) noexcept;

}  // namespace spg::follow
