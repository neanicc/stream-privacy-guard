#include "mask_rects.hpp"

#include <algorithm>
#include <cmath>

namespace spg::mask {

namespace {

constexpr double kLimit = 8388608.0;  // 2^23

double right(const Rect& r) noexcept {
  return static_cast<double>(r.x) + static_cast<double>(r.width);
}
double bottom(const Rect& r) noexcept {
  return static_cast<double>(r.y) + static_cast<double>(r.height);
}

}  // namespace

Rect pixel_rect(double x0, double y0, double x1, double y1) noexcept {
  // A NaN edge (impossible for valid inputs) becomes the whole float-exact
  // range rather than an empty rect: when unsure, cover more.
  const auto low = [](double v) {
    return std::isnan(v) ? -kLimit : std::clamp(std::floor(v), -kLimit, kLimit);
  };
  const double left = low(x0);
  const double top = low(y0);
  const double r = std::isnan(x1) ? kLimit : std::clamp(std::ceil(x1), left, kLimit);
  const double b = std::isnan(y1) ? kLimit : std::clamp(std::ceil(y1), top, kLimit);
  return Rect{static_cast<float>(left), static_cast<float>(top), static_cast<float>(r - left),
              static_cast<float>(b - top)};
}

Rect grow_by_margin(const Rect& box, float margin_px, float margin_ratio) noexcept {
  const double m = static_cast<double>(margin_px) +
                   static_cast<double>(margin_ratio) *
                       std::min(static_cast<double>(box.width), static_cast<double>(box.height));
  return pixel_rect(static_cast<double>(box.x) - m, static_cast<double>(box.y) - m, right(box) + m,
                    bottom(box) + m);
}

Rect moved(const Rect& box, Velocity velocity, double seconds) noexcept {
  const double dx = static_cast<double>(velocity.dx_per_s) * seconds;
  const double dy = static_cast<double>(velocity.dy_per_s) * seconds;
  return pixel_rect(static_cast<double>(box.x) + dx, static_cast<double>(box.y) + dy, right(box) + dx,
                    bottom(box) + dy);
}

Rect trail_to_edge(const Rect& box, Velocity velocity, Size source) noexcept {
  double x0 = static_cast<double>(box.x);
  double y0 = static_cast<double>(box.y);
  double x1 = right(box);
  double y1 = bottom(box);
  const double vx = static_cast<double>(velocity.dx_per_s);
  const double vy = static_cast<double>(velocity.dy_per_s);
  if (std::abs(vx) >= std::abs(vy)) {
    if (vx < 0.0) {
      x1 = std::max(x1, static_cast<double>(source.width));
    } else if (vx > 0.0) {
      x0 = std::min(x0, 0.0);
    }
  } else if (vy < 0.0) {
    y1 = std::max(y1, static_cast<double>(source.height));
  } else if (vy > 0.0) {
    y0 = std::min(y0, 0.0);
  }
  return pixel_rect(x0, y0, x1, y1);
}

Rect scale_between(const Rect& box, Size from, Size to) noexcept {
  if (from == to || from.width == 0 || from.height == 0 || to.width == 0 || to.height == 0) {
    return pixel_rect(static_cast<double>(box.x), static_cast<double>(box.y), right(box), bottom(box));
  }
  const double sx = static_cast<double>(to.width) / static_cast<double>(from.width);
  const double sy = static_cast<double>(to.height) / static_cast<double>(from.height);
  return pixel_rect(static_cast<double>(box.x) * sx, static_cast<double>(box.y) * sy, right(box) * sx,
                    bottom(box) * sy);
}

Rect hull(const Rect& a, const Rect& b) noexcept {
  return pixel_rect(std::min(static_cast<double>(a.x), static_cast<double>(b.x)),
                    std::min(static_cast<double>(a.y), static_cast<double>(b.y)),
                    std::max(right(a), right(b)), std::max(bottom(a), bottom(b)));
}

bool contains(const Rect& outer, const Rect& inner) noexcept {
  return outer.x <= inner.x && outer.y <= inner.y && right(outer) >= right(inner) &&
         bottom(outer) >= bottom(inner);
}

}  // namespace spg::mask
