// Following's checks (SPG-12b). See follow.hpp and docs/tracker.md.
#include "follow.hpp"

namespace spg::follow {

bool active(std::size_t follow_after, std::size_t run, Timestamp t,
            Timestamp last_confirmed) noexcept {
  return follow_after > 0 && run >= follow_after && t <= last_confirmed;
}

bool contains_moved(const Detected& newest, const Detected& older, Velocity low,
                    Velocity high) noexcept {
  const double dt = static_cast<double>((newest.seen - older.seen).count()) / 1e9;
  const Rect& n = newest.box;
  const Rect& b = older.box;
  // `older`'s box grown by its own margin, swept over [low, high] for dt,
  // per axis.
  const double x0 =
      static_cast<double>(b.x) - older.margin_x + static_cast<double>(low.dx_per_s) * dt;
  const double y0 =
      static_cast<double>(b.y) - older.margin_y + static_cast<double>(low.dy_per_s) * dt;
  const double x1 = static_cast<double>(b.x) + static_cast<double>(b.width) + older.margin_x +
                    static_cast<double>(high.dx_per_s) * dt;
  const double y1 = static_cast<double>(b.y) + static_cast<double>(b.height) + older.margin_y +
                    static_cast<double>(high.dy_per_s) * dt;
  return static_cast<double>(n.x) - newest.margin_x <= x0 &&
         static_cast<double>(n.y) - newest.margin_y <= y0 &&
         x1 <= static_cast<double>(n.x) + static_cast<double>(n.width) + newest.margin_x &&
         y1 <= static_cast<double>(n.y) + static_cast<double>(n.height) + newest.margin_y;
}

}  // namespace spg::follow
