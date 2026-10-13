#pragma once

// The tile grid shared by the detectors that run a fixed-size model over a
// larger readback frame (lane:vision): text (SPG-06) and faces (SPG-12).

#include <cstdint>
#include <vector>

#include "spg/geometry.hpp"

namespace spg::detect {

// The tile crops (whole Readback pixels) covering a frame of size `readback`:
// a grid, row-major, each tile at most `tile` pixels on a side, first tile at
// the origin and last flush with the far edge, neighbours overlapping by at
// least `overlap` pixels (when there is more than one per axis). Throws
// ContractError for a zero size and std::invalid_argument unless
// overlap < tile.
[[nodiscard]] std::vector<Rect> grid_crops(Size readback, std::uint32_t tile, std::uint32_t overlap);

}  // namespace spg::detect
