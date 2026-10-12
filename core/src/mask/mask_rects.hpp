#pragma once

// Whole-pixel mask rect helpers for the scheduler (SPG-10). Every function
// here rounds outward, so a mask never loses a pixel to rounding. These are
// Source-to-Source operations (margins, a source resize); conversions between
// coordinate spaces stay in core/src/frame/geometry.cpp.

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

// Smallest whole-pixel rect containing both.
[[nodiscard]] Rect hull(const Rect& a, const Rect& b) noexcept;

// True when `outer` contains `inner` (compared in double).
[[nodiscard]] bool contains(const Rect& outer, const Rect& inner) noexcept;

}  // namespace spg::mask
