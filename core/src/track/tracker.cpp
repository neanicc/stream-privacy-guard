// Tracker and temporal hold (SPG-09). See spg/tracker.hpp for the guarantees
// and docs/tracker.md for the reasoning behind them.
#include "spg/tracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

// Both edges of the box moved at least one pixel from `from` to `to` the way
// `velocity` points, along the axis where it is larger (x on a tie).
bool translated(const Rect& from, const Rect& to, Velocity velocity) noexcept {
  const bool along_x = std::abs(velocity.dx_per_s) >= std::abs(velocity.dy_per_s);
  const double v = static_cast<double>(along_x ? velocity.dx_per_s : velocity.dy_per_s);
  const double from_lo = static_cast<double>(along_x ? from.x : from.y);
  const double from_hi = from_lo + static_cast<double>(along_x ? from.width : from.height);
  const double to_lo = static_cast<double>(along_x ? to.x : to.y);
  const double to_hi = to_lo + static_cast<double>(along_x ? to.width : to.height);
  const double lo = to_lo - from_lo;
  const double hi = to_hi - from_hi;
  return (v > 0.0 && lo >= 1.0 && hi >= 1.0) || (v < 0.0 && lo <= -1.0 && hi <= -1.0);
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

// The largest float at most `v` and the smallest float at least `v` (clamped
// to the float range), so a scaled velocity range never narrows by rounding.
float velocity_down(double v) noexcept {
  float f = to_velocity(v);
  if (static_cast<double>(f) > v) {
    f = std::nextafter(f, -kInf);
  }
  return f;
}
float velocity_up(double v) noexcept {
  float f = to_velocity(v);
  if (static_cast<double>(f) < v) {
    f = std::nextafter(f, kInf);
  }
  return f;
}

// Hull of two whole-pixel rects; exact, since their edges are integers.
Rect hull(const Rect& a, const Rect& b) noexcept {
  return pixel_rect(std::min(static_cast<double>(a.x), static_cast<double>(b.x)),
                    std::min(static_cast<double>(a.y), static_cast<double>(b.y)),
                    std::max(static_cast<double>(a.x) + static_cast<double>(a.width),
                             static_cast<double>(b.x) + static_cast<double>(b.width)),
                    std::max(static_cast<double>(a.y) + static_cast<double>(a.height),
                             static_cast<double>(b.y) + static_cast<double>(b.height)));
}

// An unused bucket slot in masks(); real mask rects never have a negative
// width.
constexpr Rect kNoRect{0.0F, 0.0F, -1.0F, -1.0F};

bool is_slot_empty(const Rect& r) noexcept {
  return r.width < 0.0F;
}

// Floor division for possibly negative nanosecond counts; `b` is positive.
std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
  std::int64_t q = a / b;
  if (a % b != 0 && a < 0) {
    --q;
  }
  return q;
}

constexpr std::size_t kMaxRectsPerTrackLimit = 64;

// The larger of two positive lengths over the smaller; infinite when either
// is not positive, so an empty box never passes a size gate.
double size_ratio(float a, float b) noexcept {
  const double lo = std::min(static_cast<double>(a), static_cast<double>(b));
  const double hi = std::max(static_cast<double>(a), static_cast<double>(b));
  return lo > 0.0 ? hi / lo : std::numeric_limits<double>::infinity();
}

// Distance from (cx, cy) to the center of `box` in box sizes: per axis, the
// offset over the larger of `box`'s and `last`'s extent on that axis.
double size_distance(const Rect& box, const Rect& last, double cx, double cy) noexcept {
  const double sx = std::max(static_cast<double>(box.width), static_cast<double>(last.width));
  const double sy = std::max(static_cast<double>(box.height), static_cast<double>(last.height));
  return std::hypot((center_x(box) - cx) / sx, (center_y(box) - cy) / sy);
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
  if (!std::isfinite(config.motion_gate_per_s) || config.motion_gate_per_s < 0.0F) {
    throw ContractError("tracker config: motion_gate_per_s must be finite and not negative");
  }
  if (!std::isfinite(config.motion_size_ratio) || config.motion_size_ratio < 1.0F) {
    throw ContractError("tracker config: motion_size_ratio must be finite and at least 1");
  }
  if (!(config.velocity_smoothing > 0.0F && config.velocity_smoothing <= 1.0F)) {
    throw ContractError("tracker config: velocity_smoothing must be in (0, 1]");
  }
  if (config.max_rects_per_track < 1 || config.max_rects_per_track > kMaxRectsPerTrackLimit) {
    throw ContractError("tracker config: max_rects_per_track must be in [1, 64]");
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

void Tracker::update(Timestamp frame_time, std::span<const Detection> detections,
                     std::vector<TrackMotion>* motions) {
  for (const Detection& detection : detections) {
    validate(detection);
  }
  if (motions != nullptr) {
    motions->assign(detections.size(), TrackMotion{Velocity{}, frame_time});
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
  const auto join = [&](std::size_t ti, std::size_t di) {
    track_taken[ti] = true;
    detection_taken[di] = true;
    TrackState& state = tracks_[ti];
    confirm(state, frame_time, detections[di].box.rect);
    if (motions != nullptr) {
      (*motions)[di] = state.has_velocity
                           ? TrackMotion{state.track.velocity, state.first_seen, state.translating}
                           : TrackMotion{Velocity{}, state.first_seen, false};
    }
  };
  for (const Candidate& c : candidates) {
    if (!track_taken[c.track] && !detection_taken[c.detection]) {
      join(c.track, c.detection);
    }
  }

  // Motion gate (SPG-16b): the tracks and detections IoU left unpaired, when
  // the detection is where the track could have moved in the frame time
  // between them (see TrackerConfig::motion_gate_per_s). It runs after IoU so
  // every pair IoU makes is kept. A detection joining the wrong track only
  // changes that track's motion: every detection is held from its own frame
  // whatever track it joins, so no hold is shortened.
  struct MotionCandidate {
    double cost;  // box sizes from the nearer of the last and predicted center
    std::size_t track;
    std::size_t detection;
  };
  std::vector<MotionCandidate> moving;
  if (config_.motion_gate_per_s > 0.0F) {
    const double ratio = static_cast<double>(config_.motion_size_ratio);
    for (std::size_t ti = 0; ti < tracks_.size(); ++ti) {
      const TrackState& state = tracks_[ti];
      const Track& track = state.track;
      if (track_taken[ti] || frame_time > track.hold_until) {
        continue;
      }
      const Duration offset = frame_time - track.last_confirmed;
      const Duration gap = std::min(offset < Duration::zero() ? -offset : offset, config_.max_prediction);
      const double reach = static_cast<double>(config_.motion_gate_per_s) * seconds(gap);
      if (!(reach > 0.0)) {
        continue;  // same frame time: only IoU pairs it
      }
      const Rect& last = track.box.rect;
      // Where the track's velocity puts it at frame_time, forward or back,
      // capped as prediction is.
      const double ahead = seconds(std::clamp(offset, -config_.max_prediction, config_.max_prediction));
      const double px = center_x(last) + static_cast<double>(track.velocity.dx_per_s) * ahead;
      const double py = center_y(last) + static_cast<double>(track.velocity.dy_per_s) * ahead;
      for (std::size_t di = 0; di < detections.size(); ++di) {
        const Rect& box = detections[di].box.rect;
        if (detection_taken[di] || detections[di].kind != track.kind ||
            !(size_ratio(box.width, last.width) <= ratio && size_ratio(box.height, last.height) <= ratio)) {
          continue;
        }
        const double cost = std::min(size_distance(box, last, center_x(last), center_y(last)),
                                     size_distance(box, last, px, py));
        if (cost <= reach) {
          moving.push_back({cost, ti, di});
        }
      }
    }
  }
  std::sort(moving.begin(), moving.end(), [](const MotionCandidate& a, const MotionCandidate& b) {
    return std::tie(a.cost, a.track, a.detection) < std::tie(b.cost, b.track, b.detection);
  });
  for (const MotionCandidate& c : moving) {
    if (!track_taken[c.track] && !detection_taken[c.detection]) {
      join(c.track, c.detection);
    }
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
    state.first_seen = frame_time;
    state.held.push_back(make_held(detection.box.rect, frame_time, Velocity{}));
    tracks_.push_back(std::move(state));
  }

  prune();
}

void Tracker::confirm(TrackState& state, Timestamp frame_time, const Rect& box) {
  Track& track = state.track;
  const bool earliest = frame_time < state.first_seen;
  state.first_seen = std::min(state.first_seen, frame_time);
  if (frame_time <= track.last_confirmed) {
    // Late or same-frame detection: it holds, predicted at the track's
    // current velocity, but does not move the track.
    if (earliest) {
      // Older than every detection the track has taken (cycles that finish
      // out of order): its move to the latest box is motion no measurement
      // has seen. The track takes that velocity when it has none, or when
      // the move translates the box but the velocity it has does not point
      // that way (measured near rest by results that landed first). The box
      // does not move.
      const double dt = seconds(track.last_confirmed - frame_time);
      const Velocity measured{to_velocity((center_x(track.box.rect) - center_x(box)) / dt),
                              to_velocity((center_y(track.box.rect) - center_y(box)) / dt)};
      if (!state.has_velocity || (!translated(box, track.box.rect, track.velocity) &&
                                  translated(box, track.box.rect, measured))) {
        track.velocity = measured;
        state.has_velocity = true;
      }
      state.translating = state.translating || translated(box, track.box.rect, track.velocity);
    }
    state.held.push_back(make_held(box, frame_time, track.velocity));
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
  // Sticky: a later measurement at rest does not clear it, so a result that
  // lands late is judged the same whatever arrived before it.
  state.translating = state.translating || translated(track.box.rect, box, track.velocity);
  // Between two sightings the object went from the last box to this one.
  // Velocity covers that path only as far as constant motion would: a slide
  // that eases out is further along than the measurement says, and a track
  // seen once has no velocity at all. So the hull of both boxes, with the
  // larger of their margins, is held from the last sighting. With one
  // detection cycle every ~200 ms this covers the frames between sightings
  // of a box that moves more than its size (SPG-16b).
  const Rect& last = track.box.rect;
  Held bridge = make_held(
      narrow_rect(std::min(static_cast<double>(last.x), static_cast<double>(box.x)),
                  std::min(static_cast<double>(last.y), static_cast<double>(box.y)),
                  std::max(static_cast<double>(last.x) + static_cast<double>(last.width),
                           static_cast<double>(box.x) + static_cast<double>(box.width)),
                  std::max(static_cast<double>(last.y) + static_cast<double>(last.height),
                           static_cast<double>(box.y) + static_cast<double>(box.height))),
      track.last_confirmed, Velocity{});
  const Held last_held = make_held(last, track.last_confirmed, Velocity{});
  const Held this_held = make_held(box, frame_time, Velocity{});
  bridge.margin_x = bridge.margin_y = std::max(last_held.margin_x, this_held.margin_x);
  state.held.push_back(bridge);
  track.box = Box{box, Space::Source};
  track.last_confirmed = frame_time;
  track.hold_until = frame_time + config_.hold;
  state.held.push_back(make_held(box, frame_time, track.velocity));
}

Tracker::Held Tracker::make_held(const Rect& box, Timestamp seen,
                                 Velocity velocity) const noexcept {
  const double margin =
      static_cast<double>(config_.margin_px) +
      static_cast<double>(config_.margin_ratio) *
          std::min(static_cast<double>(box.width), static_cast<double>(box.height));
  return Held{box, seen, velocity, velocity, margin, margin};
}

Rect Tracker::mask_rect(const Held& held, Timestamp t) const noexcept {
  const double dt = seconds(std::clamp(t - held.seen, Duration::zero(), config_.max_prediction));
  const Rect& b = held.box;
  // Hull of the box and every box moved by a velocity in [low, high] for dt.
  const double x0 = static_cast<double>(b.x) +
                    std::min(0.0, static_cast<double>(held.low.dx_per_s) * dt) - held.margin_x;
  const double y0 = static_cast<double>(b.y) +
                    std::min(0.0, static_cast<double>(held.low.dy_per_s) * dt) - held.margin_y;
  const double x1 = static_cast<double>(b.x) + static_cast<double>(b.width) +
                    std::max(0.0, static_cast<double>(held.high.dx_per_s) * dt) + held.margin_x;
  const double y1 = static_cast<double>(b.y) + static_cast<double>(b.height) +
                    std::max(0.0, static_cast<double>(held.high.dy_per_s) * dt) + held.margin_y;
  return pixel_rect(x0, y0, x1, y1);
}

std::vector<Box> Tracker::masks(Timestamp t) const {
  std::vector<Box> out;
  masks(t, out);
  return out;
}

void Tracker::masks(Timestamp t, std::vector<Box>& out) const {
  out.clear();
  // Fixed time buckets: a detection seen at `seen` falls in bucket
  // floor(seen / width). Detections active at t were seen in [t - hold, t],
  // which spans at most ceil(hold / width) + 1 <= slots buckets. Bucket
  // membership never changes, so a bucket's hull only grows as detections
  // are added or t advances, like the rects it merges.
  const std::size_t slots = config_.max_rects_per_track;
  std::int64_t width = 1;
  if (slots > 1) {
    const auto n = static_cast<std::int64_t>(slots - 1);
    width = std::max<std::int64_t>(1, (config_.hold.count() + n - 1) / n);
  }
  const std::int64_t first =
      slots > 1 ? floor_div((t - config_.hold).time_since_epoch().count(), width) : 0;

  for (const TrackState& state : tracks_) {
    const std::size_t start = out.size();
    out.resize(start + slots, Box{kNoRect, Space::Source});
    for (const Held& held : state.held) {
      if (t < held.seen || t > held.seen + config_.hold) {
        continue;
      }
      const std::int64_t bucket =
          slots > 1 ? floor_div(held.seen.time_since_epoch().count(), width) - first : 0;
      // Always in [0, slots) by the bound above; the clamp only guards the
      // buffer (merging into a neighbor would still keep the coverage).
      const std::size_t slot = std::min(static_cast<std::size_t>(std::max<std::int64_t>(0, bucket)),
                                        slots - 1);
      Rect& r = out[start + slot].rect;
      const Rect m = mask_rect(held, t);
      r = is_slot_empty(r) ? m : hull(r, m);
    }
    // Leave out a rect another one contains; of two equal rects keep the
    // first. Each dropped rect lies inside one still kept, so the union is
    // unchanged.
    for (std::size_t i = start; i < start + slots; ++i) {
      Rect& ri = out[i].rect;
      if (is_slot_empty(ri)) {
        continue;
      }
      for (std::size_t j = start; j < start + slots; ++j) {
        const Rect& rj = out[j].rect;
        if (i != j && !is_slot_empty(rj) && contains(rj, ri) && (j < i || !contains(ri, rj))) {
          ri = kNoRect;
          break;
        }
      }
    }
    std::size_t kept = start;
    for (std::size_t i = start; i < start + slots; ++i) {
      if (!is_slot_empty(out[i].rect)) {
        out[kept++] = out[i];
      }
    }
    out.resize(kept);
  }
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
  const double margin_px = static_cast<double>(config_.margin_px);
  const double margin_ratio = static_cast<double>(config_.margin_ratio);
  for (TrackState& state : tracks_) {
    for (Held& held : state.held) {
      // One held box covering both the old and the scaled box, predicted
      // over both velocity ranges, so nothing it covered before is lost.
      // The old mask, scaled, is the scaled box grown by the scaled margin
      // plus less than one scaled pixel of outward rounding; the margin grows
      // to cover that, and to at least the margin of the merged box.
      const Rect s = scaled(held.box);
      const Rect& o = held.box;
      const double x0 = std::min(static_cast<double>(o.x), static_cast<double>(s.x));
      const double y0 = std::min(static_cast<double>(o.y), static_cast<double>(s.y));
      const double x1 = std::max(static_cast<double>(o.x) + static_cast<double>(o.width),
                                 static_cast<double>(s.x) + static_cast<double>(s.width));
      const double y1 = std::max(static_cast<double>(o.y) + static_cast<double>(o.height),
                                 static_cast<double>(s.y) + static_cast<double>(s.height));
      held.box = narrow_rect(x0, y0, x1, y1);
      const double fresh =
          margin_px + margin_ratio * std::min(static_cast<double>(held.box.width),
                                              static_cast<double>(held.box.height));
      held.margin_x = std::max({held.margin_x, sx * (held.margin_x + 1.0), fresh});
      held.margin_y = std::max({held.margin_y, sy * (held.margin_y + 1.0), fresh});
      // Scaled velocities rounded outward, so the scaled prediction range
      // never narrows by a float step.
      held.low = Velocity{
          std::min(held.low.dx_per_s, velocity_down(static_cast<double>(held.low.dx_per_s) * sx)),
          std::min(held.low.dy_per_s, velocity_down(static_cast<double>(held.low.dy_per_s) * sy))};
      held.high = Velocity{
          std::max(held.high.dx_per_s, velocity_up(static_cast<double>(held.high.dx_per_s) * sx)),
          std::max(held.high.dy_per_s, velocity_up(static_cast<double>(held.high.dy_per_s) * sy))};
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
