#include "pipeline/mask_levels.hpp"

#include <algorithm>
#include <cmath>

namespace spg::plugin {

namespace {

std::uint32_t halve(std::uint32_t v) noexcept {
  return v / 2 + v % 2;
}

// Smallest level whose cells are at least `cell` frame pixels, capped.
std::uint32_t level_for_cell(double cell, Size frame) noexcept {
  const std::uint32_t top = top_level(frame);
  std::uint32_t level = 0;
  double side = 1.0;
  while (side < cell && level < top) {
    side *= 2.0;
    ++level;
  }
  return level;
}

}  // namespace

Size level_size(Size frame, std::uint32_t level) noexcept {
  Size size = frame;
  for (std::uint32_t i = 0; i < level && (size.width > 1 || size.height > 1); ++i) {
    size.width = std::max<std::uint32_t>(1, halve(size.width));
    size.height = std::max<std::uint32_t>(1, halve(size.height));
  }
  return size;
}

std::uint32_t top_level(Size frame) noexcept {
  std::uint32_t level = 0;
  Size size = frame;
  while (size.width > 1 || size.height > 1) {
    size.width = std::max<std::uint32_t>(1, halve(size.width));
    size.height = std::max<std::uint32_t>(1, halve(size.height));
    ++level;
  }
  return level;
}

std::uint32_t mask_level(const Rect& rect, Size frame) noexcept {
  const double shorter = std::isfinite(rect.width) && std::isfinite(rect.height)
                             ? std::max(0.0, static_cast<double>(std::min(rect.width, rect.height)))
                             : 0.0;
  return level_for_cell(std::max<double>(kMinMaskCell, shorter / kMaskCellDivisor), frame);
}

std::uint32_t whole_level(Size frame) noexcept {
  const double shorter = std::min(frame.width, frame.height);
  return level_for_cell(std::max<double>(kMinWholeCell, shorter / kWholeCellDivisor), frame);
}

std::optional<LevelRegion> mask_region(const Rect& rect, Size frame, std::uint32_t level) noexcept {
  if (!is_valid(rect) || frame.width == 0 || frame.height == 0) {
    return std::nullopt;
  }
  const Size texels = level_size(frame, level);
  // Normalized to the frame, then to whole texels, rounding outward.
  const auto span = [](double lo, double hi, std::uint32_t full, std::uint32_t cells, std::uint32_t& start,
                       std::uint32_t& length) {
    const double a = std::clamp(lo / full, 0.0, 1.0) * cells;
    const double b = std::clamp(hi / full, 0.0, 1.0) * cells;
    const double first = std::floor(a);
    const double last = std::ceil(b);
    if (!(hi > lo) || !(b > a) || last <= first) {
      return false;
    }
    start = static_cast<std::uint32_t>(first);
    length = static_cast<std::uint32_t>(last - first);
    return true;
  };
  LevelRegion region;
  const double x0 = rect.x;
  const double y0 = rect.y;
  if (!span(x0, x0 + rect.width, frame.width, texels.width, region.x, region.width) ||
      !span(y0, y0 + rect.height, frame.height, texels.height, region.y, region.height)) {
    return std::nullopt;
  }
  return region;
}

}  // namespace spg::plugin
