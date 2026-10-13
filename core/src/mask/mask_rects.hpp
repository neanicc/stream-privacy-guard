#pragma once

// Whole-pixel mask rect helpers for the scheduler (SPG-10). Every function
// here rounds outward, so a mask never loses a pixel to rounding. These are
// Source-to-Source operations (margins, a source resize); conversions between
// coordinate spaces stay in core/src/frame/geometry.cpp.

#include "spg/detection.hpp"
#include "spg/geometry.hpp"

namespace spg::mask {

// The smallest whole-pixel rect containing [x0, x1] x [y0, y1], with edges
// clamped to +-2^23 (where every integer edge is exact in float).
[[nodiscard]] Rect pixel_rect(double x0, double y0, double x1, double y1) noexcept;

// `box` grown on every edge by margin_px + margin_ratio * min(width, height),
// rounded outward to whole pixels. The same margin the tracker applies.
[[nodiscard]] Rect grow_by_margin(const Rect& box, float margin_px, float margin_ratio) noexcept;

// `box` in a source of size `from`, scaled to a source of size `to` and
// rounded outward to whole pixels. Returns `box` rounded outward when the
// sizes are equal or either has a zero dimension.
[[nodiscard]] Rect scale_between(const Rect& box, Size from, Size to) noexcept;

// `box` moved by `velocity` (Source pixels per second) for `seconds`, which
// may be negative, rounded outward to whole pixels.
[[nodiscard]] Rect moved(const Rect& box, Velocity velocity, double seconds) noexcept;

// `box` stretched back along its motion to the edge of a `source`-sized frame:
// along the axis where `velocity` is larger (x on a tie), the edge the box
// moves away from is extended to the frame edge it came from (the right edge
// for leftward motion, and so on). Everything `box` covered stays covered.
// Returns `box` rounded outward when `velocity` is zero. Where a box that is
// seen moving for the first time was on the frames before: a toast or panel
// sliding in, which may have moved faster than the velocity measured now.
[[nodiscard]] Rect trail_to_edge(const Rect& box, Velocity velocity, Size source) noexcept;

// `rect` grown by `dx` on its left and right edges and `dy` on its top and
// bottom, rounded outward to whole pixels. A negative or NaN amount grows by
// zero (a mask never shrinks here).
[[nodiscard]] Rect spread(const Rect& rect, double dx, double dy) noexcept;

// Smallest whole-pixel rect containing both.
[[nodiscard]] Rect hull(const Rect& a, const Rect& b) noexcept;

// True when `outer` contains `inner` (compared in double).
[[nodiscard]] bool contains(const Rect& outer, const Rect& inner) noexcept;

}  // namespace spg::mask
