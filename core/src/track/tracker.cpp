// Tracker and temporal hold (SPG-09). See spg/tracker.hpp for the guarantees
// and docs/tracker.md for the reasoning behind them.
#include "spg/tracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <tuple>
#include <utility>

#include "spg/error.hpp"

namespace spg {

namespace {

constexpr double kFloatMax = std::numeric_limits<float>::max();
constexpr float kInf = std::numeric_limits<float>::infinity();

double seconds(Duration d) noexcept {
  return static_cast<double>(d.count()) / 1e9;
}

// Narrows [lo, hi] to a float origin and length whose span, summed in double
// as enclosing_pixels() does, contains it. Values beyond the float range are
// clamped to it, which only makes the rect larger than the frame.
std::pair<float, float> narrow_outward(double lo, double hi) noexcept {
  lo = std::clamp(lo, -kFloatMax, kFloatMax);
  hi = std::clamp(hi, lo, kFloatMax);
  float origin = static_cast<float>(lo);
  if (static_cast<double>(origin) > lo) {
    origin = std::nextafter(origin, -kInf);
  }
  const double span = std::min(hi - static_cast<double>(origin), kFloatMax);
  float length = static_cast<float>(span);
  while (static_cast<double>(origin) + static_cast<double>(length) < hi &&
         length < std::numeric_limits<float>::max()) {
    length = std::nextafter(length, kInf);
  }
  return {origin, length};
}

Rect narrow_rect(double x0, double y0, double x1, double y1) noexcept {
  const auto [x, w] = narrow_outward(x0, x1);
  const auto [y, h] = narrow_outward(y0, y1);
  return Rect{x, y, w, h};
}

// Mask rects are whole Source pixels: edges rounded outward to integers and
// kept within +-2^23, where every integer edge and every width up to 2^24 is
// exact in float. Exact edges make the mask monotone in its inputs (a larger
// hull never yields a rect that misses part of a smaller one by a rounding
// step) and match what enclosing_pixels() blurs. No source is near 2^23 px.
Rect pixel_rect(double x0, double y0, double x1, double y1) noexcept {
  constexpr double kLimit = 8388608.0;  // 2^23
  const double left = std::clamp(std::floor(x0), -kLimit, kLimit);
  const double top = std::clamp(std::floor(y0), -kLimit, kLimit);
  const double right = std::clamp(std::ceil(x1), left, kLimit);
  const double bottom = std::clamp(std::ceil(y1), top, kLimit);
  return Rect{static_cast<float>(left), static_cast<float>(top), static_cast<float>(right - left),
              static_cast<float>(bottom - top)};
}

bool contains(const Rect& outer, const Rect& inner) noexcept {
  const double ox1 = static_cast<double>(outer.x) + static_cast<double>(outer.width);
  const double oy1 = static_cast<double>(outer.y) + static_cast<double>(outer.height);
  const double ix1 = static_cast<double>(inner.x) + static_cast<double>(inner.width);
  const double iy1 = static_cast<double>(inner.y) + static_cast<double>(inner.height);
  return outer.x <= inner.x && outer.y <= inner.y && ox1 >= ix1 && oy1 >= iy1;
}

// `box` moved by `velocity` for `dt` seconds.
Rect moved(const Rect& box, Velocity velocity, double dt) noexcept {
  return narrow_rect(static_cast<double>(box.x) + static_cast<double>(velocity.dx_per_s) * dt,
                     static_cast<double>(box.y) + static_cast<double>(velocity.dy_per_s) * dt,
                     static_cast<double>(box.x) + static_cast<double>(box.width) +
                         static_cast<double>(velocity.dx_per_s) * dt,
                     static_cast<double>(box.y) + static_cast<double>(box.height) +
                         static_cast<double>(velocity.dy_per_s) * dt);
}

double center_x(const Rect& r) noexcept {
  return static_cast<double>(r.x) + static_cast<double>(r.width) / 2.0;
}
double center_y(const Rect& r) noexcept {
  return static_cast<double>(r.y) + static_cast<double>(r.height) / 2.0;
}

float to_velocity(double v) noexcept {
  return static_cast<float>(std::clamp(v, -kFloatMax, kFloatMax));
}

}  // namespace

TrackerConfig TrackerConfig::from_policy(const PolicyConfig& policy) {
  TrackerConfig config;
  config.hold = policy.hold;
  config.margin_px = policy.margin_px;
  config.margin_ratio = policy.margin_ratio;
  config.retain = policy.lookahead;
  return config;
}

void validate(const TrackerConfig& config) {
  if (config.hold < Duration::zero()) {
    throw ContractError("tracker config: hold must not be negative");
  }
  if (config.retain < Duration::zero()) {
    throw ContractError("tracker config: retain must not be negative");
  }
  if (config.max_prediction < Duration::zero()) {
    throw ContractError("tracker config: max_prediction must not be negative");
  }
  if (!std::isfinite(config.margin_px) || config.margin_px < 0.0F) {
    throw ContractError("tracker config: margin_px must be finite and not negative");
  }
  if (!std::isfinite(config.margin_ratio) || config.margin_ratio < 0.0F) {
    throw ContractError("tracker config: margin_ratio must be finite and not negative");
  }
  if (!(config.iou_threshold > 0.0F && config.iou_threshold <= 1.0F)) {
    throw ContractError("tracker config: iou_threshold must be in (0, 1]");
  }
  if (!(config.velocity_smoothing > 0.0F && config.velocity_smoothing <= 1.0F)) {
    throw ContractError("tracker config: velocity_smoothing must be in (0, 1]");
  }
}

Tracker::Tracker(const Clock& clock, TrackerConfig config) : clock_(&clock), config_(config) {
  validate(config_);
}

Rect Tracker::predicted(const Track& track, Timestamp t) const noexcept {
  const Duration ahead =
      std::clamp(t - track.last_confirmed, Duration::zero(), config_.max_prediction);
  return moved(track.box.rect, track.velocity, seconds(ahead));
}

void Tracker::update(Timestamp frame_time, std::span<const Detection> detections) {
  for (const Detection& detection : detections) {
    validate(detection);
  }

  // Candidate (track, detection) pairs of the same kind whose IoU clears the
  // threshold, against the track's predicted box or its last confirmed box.
  struct Candidate {
    float iou;
    std::size_t track;
    std::size_t detection;
  };
  std::vector<Candidate> candidates;
  for (std::size_t ti = 0; ti < tracks_.size(); ++ti) {
    const Track& track = tracks_[ti].track;
    if (frame_time > track.hold_until) {
      continue;  // lost before this frame; a detection now starts a new track
    }
    const Rect prediction = predicted(track, frame_time);
    for (std::size_t di = 0; di < detections.size(); ++di) {
      if (detections[di].kind != track.kind) {
        continue;
      }
      const Rect& box = detections[di].box.rect;
      const float score = std::max(iou(box, prediction), iou(box, track.box.rect));
      if (score >= config_.iou_threshold) {
        candidates.push_back({score, ti, di});
      }
    }
  }
  // Greedy one-to-one assignment, best IoU first; ties broken by index so the
  // result does not depend on sort stability.
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    return std::tie(b.iou, a.track, a.detection) < std::tie(a.iou, b.track, b.detection);
  });
  std::vector<bool> track_taken(tracks_.size(), false);
  std::vector<bool> detection_taken(detections.size(), false);
  for (const Candidate& c : candidates) {
    if (track_taken[c.track] || detection_taken[c.detection]) {
      continue;
    }
    track_taken[c.track] = true;
    detection_taken[c.detection] = true;
    confirm(tracks_[c.track], frame_time, detections[c.detection].box.rect);
  }

  for (std::size_t di = 0; di < detections.size(); ++di) {
    if (detection_taken[di]) {
      continue;
    }
    const Detection& detection = detections[di];
    TrackState state;
    state.track.id = next_id_++;
    state.track.box = Box{detection.box.rect, Space::Source};
    state.track.last_confirmed = frame_time;
    state.track.hold_until = frame_time + config_.hold;
    state.track.kind = detection.kind;
    state.held.push_back(Held{detection.box.rect, frame_time, {}, {}});
    tracks_.push_back(std::move(state));
  }

  prune();
}

void Tracker::confirm(TrackState& state, Timestamp frame_time, const Rect& box) {
  Track& track = state.track;
  if (frame_time <= track.last_confirmed) {
    // Late or same-frame detection: it holds, predicted at the track's
    // current velocity, but does not move the track.
    state.held.push_back(Held{box, frame_time, track.velocity, track.velocity});
    return;
  }
  const double dt = seconds(frame_time - track.last_confirmed);
  const double measured_x = (center_x(box) - center_x(track.box.rect)) / dt;
  const double measured_y = (center_y(box) - center_y(track.box.rect)) / dt;
  if (state.has_velocity) {
    const double a = static_cast<double>(config_.velocity_smoothing);
    track.velocity.dx_per_s =
        to_velocity(a * measured_x + (1.0 - a) * static_cast<double>(track.velocity.dx_per_s));
    track.velocity.dy_per_s =
        to_velocity(a * measured_y + (1.0 - a) * static_cast<double>(track.velocity.dy_per_s));
  } else {
    track.velocity = Velocity{to_velocity(measured_x), to_velocity(measured_y)};
    state.has_velocity = true;
  }
  track.box = Box{box, Space::Source};
  track.last_confirmed = frame_time;
  track.hold_until = frame_time + config_.hold;
  state.held.push_back(Held{box, frame_time, track.velocity, track.velocity});
}

Rect Tracker::mask_rect(const Held& held, Timestamp t) const noexcept {
  const double dt = seconds(std::clamp(t - held.seen, Duration::zero(), config_.max_prediction));
  const Rect& b = held.box;
  const double margin = static_cast<double>(config_.margin_px) +
                        static_cast<double>(config_.margin_ratio) *
                            std::min(static_cast<double>(b.width), static_cast<double>(b.height));
  // Hull of the box and every box moved by a velocity in [low, high] for dt.
  const double x0 = static_cast<double>(b.x) +
                    std::min(0.0, static_cast<double>(held.low.dx_per_s) * dt) - margin;
  const double y0 = static_cast<double>(b.y) +
                    std::min(0.0, static_cast<double>(held.low.dy_per_s) * dt) - margin;
  const double x1 = static_cast<double>(b.x) + static_cast<double>(b.width) +
                    std::max(0.0, static_cast<double>(held.high.dx_per_s) * dt) + margin;
  const double y1 = static_cast<double>(b.y) + static_cast<double>(b.height) +
                    std::max(0.0, static_cast<double>(held.high.dy_per_s) * dt) + margin;
  return pixel_rect(x0, y0, x1, y1);
}

std::vector<Box> Tracker::masks(Timestamp t) const {
  std::vector<Box> out;
  std::vector<Rect> rects;
  for (const TrackState& state : tracks_) {
    rects.clear();
    for (const Held& held : state.held) {
      if (t >= held.seen && t <= held.seen + config_.hold) {
        rects.push_back(mask_rect(held, t));
      }
    }
    // Leave out a rect another one contains; of two equal rects keep the
    // first. The union is unchanged.
    for (std::size_t i = 0; i < rects.size(); ++i) {
      bool redundant = false;
      for (std::size_t j = 0; j < rects.size() && !redundant; ++j) {
        if (i != j && contains(rects[j], rects[i]) && (j < i || !contains(rects[i], rects[j]))) {
          redundant = true;
        }
      }
      if (!redundant) {
        out.push_back(Box{rects[i], Space::Source});
      }
    }
  }
  return out;
}

void Tracker::rescale(Size from, Size to) {
  if (from.width == 0 || from.height == 0 || to.width == 0 || to.height == 0) {
    throw ContractError("tracker: rescale sizes must not have a zero dimension");
  }
  const double sx = static_cast<double>(to.width) / static_cast<double>(from.width);
  const double sy = static_cast<double>(to.height) / static_cast<double>(from.height);
  const auto scaled = [sx, sy](const Rect& r) {
    return narrow_rect(static_cast<double>(r.x) * sx, static_cast<double>(r.y) * sy,
                       (static_cast<double>(r.x) + static_cast<double>(r.width)) * sx,
                       (static_cast<double>(r.y) + static_cast<double>(r.height)) * sy);
  };
  const auto scaled_velocity = [sx, sy](Velocity v) {
    return Velocity{to_velocity(static_cast<double>(v.dx_per_s) * sx),
                    to_velocity(static_cast<double>(v.dy_per_s) * sy)};
  };
  for (TrackState& state : tracks_) {
    for (Held& held : state.held) {
      // One held box covering both the old and the scaled box, predicted
      // over both velocity ranges, so nothing it covered before is lost.
      const Rect s = scaled(held.box);
      const Rect& o = held.box;
      const double x0 = std::min(static_cast<double>(o.x), static_cast<double>(s.x));
      const double y0 = std::min(static_cast<double>(o.y), static_cast<double>(s.y));
      const double x1 = std::max(static_cast<double>(o.x) + static_cast<double>(o.width),
                                 static_cast<double>(s.x) + static_cast<double>(s.width));
      const double y1 = std::max(static_cast<double>(o.y) + static_cast<double>(o.height),
                                 static_cast<double>(s.y) + static_cast<double>(s.height));
      const Velocity low = scaled_velocity(held.low);
      const Velocity high = scaled_velocity(held.high);
      held.box = narrow_rect(x0, y0, x1, y1);
      held.low = Velocity{std::min(held.low.dx_per_s, low.dx_per_s),
                          std::min(held.low.dy_per_s, low.dy_per_s)};
      held.high = Velocity{std::max(held.high.dx_per_s, high.dx_per_s),
                           std::max(held.high.dy_per_s, high.dy_per_s)};
    }
    // The track itself continues in the new coordinates.
    state.track.box = Box{scaled(state.track.box.rect), Space::Source};
    state.track.velocity = scaled_velocity(state.track.velocity);
  }
}

void Tracker::prune() {
  const Timestamp now = clock_->now();
  for (TrackState& state : tracks_) {
    std::erase_if(state.held, [&](const Held& held) {
      return held.seen + config_.hold + config_.retain < now;
    });
  }
  std::erase_if(tracks_, [](const TrackState& state) { return state.held.empty(); });
}

std::vector<Track> Tracker::tracks() const {
  std::vector<Track> out;
  out.reserve(tracks_.size());
  for (const TrackState& state : tracks_) {
    out.push_back(state.track);
  }
  return out;
}

}  // namespace spg
