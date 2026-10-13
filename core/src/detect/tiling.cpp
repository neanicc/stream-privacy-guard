#include "detect/tiling.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "spg/error.hpp"

namespace spg::detect {

namespace {

// Tile start positions along one axis of `length` pixels.
std::vector<std::uint32_t> axis_starts(std::uint32_t length, std::uint32_t tile, std::uint32_t overlap) {
  if (length <= tile) {
    return {0};
  }
  // Fewest tiles whose step (length - tile) / (n - 1) is at most tile - overlap.
  const std::uint64_t step = tile - overlap;
  const std::uint64_t n = (static_cast<std::uint64_t>(length) - overlap + step - 1) / step;
  const std::uint64_t travel = length - tile;
  std::vector<std::uint32_t> starts;
  starts.reserve(static_cast<std::size_t>(n));
  // Floored positions: consecutive gaps are at most ceil(travel / (n - 1)),
  // which is at most the integer tile - overlap.
  for (std::uint64_t i = 0; i < n; ++i) {
    starts.push_back(static_cast<std::uint32_t>(i * travel / (n - 1)));
  }
  return starts;
}

}  // namespace

std::vector<Rect> grid_crops(Size readback, std::uint32_t tile, std::uint32_t overlap) {
  if (readback.width == 0 || readback.height == 0) {
    throw ContractError("tiling: readback size must be non-zero");
  }
  if (tile == 0 || overlap >= tile) {
    throw std::invalid_argument("tile overlap must be less than the tile size");
  }
  const std::vector<std::uint32_t> xs = axis_starts(readback.width, tile, overlap);
  const std::vector<std::uint32_t> ys = axis_starts(readback.height, tile, overlap);
  const float w = static_cast<float>(std::min(readback.width, tile));
  const float h = static_cast<float>(std::min(readback.height, tile));
  std::vector<Rect> crops;
  crops.reserve(xs.size() * ys.size());
  for (const std::uint32_t y : ys) {
    for (const std::uint32_t x : xs) {
      crops.push_back({static_cast<float>(x), static_cast<float>(y), w, h});
    }
  }
  return crops;
}

}  // namespace spg::detect
