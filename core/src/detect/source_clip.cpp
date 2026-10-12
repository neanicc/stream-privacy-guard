#include "detect/source_clip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace spg::detect {

std::optional<Rect> clip_to(const Rect& rect, Size bounds) {
  const auto axis = [](float origin, float length, std::uint32_t limit) -> std::optional<std::pair<float, float>> {
    const double lo = std::max(static_cast<double>(origin), 0.0);
    const double hi = std::min(static_cast<double>(origin) + static_cast<double>(length), static_cast<double>(limit));
    if (!(hi > lo)) {
      return std::nullopt;
    }
    // lo is 0 or the original float origin, so it narrows exactly.
    const auto start = static_cast<float>(lo);
    auto span = static_cast<float>(hi - lo);
    while (static_cast<double>(start) + static_cast<double>(span) < hi) {
      span = std::nextafter(span, std::numeric_limits<float>::infinity());
    }
    return std::pair{start, span};
  };
  const auto x = axis(rect.x, rect.width, bounds.width);
  const auto y = axis(rect.y, rect.height, bounds.height);
  if (!x || !y) {
    return std::nullopt;
  }
  return Rect{x->first, y->first, x->second, y->second};
}

}  // namespace spg::detect
