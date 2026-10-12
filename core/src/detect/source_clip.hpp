#pragma once

// Clipping a converted Source-space box to the source (lane:vision), shared
// by every detector so they drop and narrow boxes the same way.

#include <optional>

#include "spg/geometry.hpp"

namespace spg::detect {

// `rect` clipped to [0, bounds), or nothing when no part of it is inside.
// Edges are clipped in double and the far edge narrows outward, so a clipped
// box never loses a pixel the unclipped one covered inside the frame.
[[nodiscard]] std::optional<Rect> clip_to(const Rect& rect, Size bounds);

}  // namespace spg::detect
