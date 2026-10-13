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

#include "follow.hpp"
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

// Width and height each changed by at most a quarter from `from` to `to`.
bool same_size(const Rect& from, const Rect& to) noexcept {
  const auto close = [](float a, float b) {
    return std::abs(static_cast<double>(b) - static_cast<double>(a)) <= 0.25 * static_cast<double>(a);
  };
  return close(from.width, to.width) && close(from.height, to.height);
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
constexpr std::size_t kMaxFollowAfter = 64;

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
  if (config.max_rects_per_track < 1 || config.max_rects_per_track > kMaxRectsPerTrackLimit) {
    throw ContractError("tracker config: max_rects_per_track must be in [1, 64]");
  }
  if (config.follow_after > kMaxFollowAfter) {
    throw ContractError("tracker config: follow_after must be at most 64");
  }
  if (!std::isfinite(config.follow_speed_ratio) || config.follow_speed_ratio < 1.0F) {
    throw ContractError("tracker config: follow_speed_ratio must be finite and at least 1");
  }
  if (!std::isfinite(config.follow_min_move) || config.follow_min_move < 1.0F) {
    throw ContractError("tracker config: follow_min_move must be finite and at least 1");
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
    motions->assign(detections.size(), TrackMotion{Velocity{}, frame_time, false, 0});
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
    TrackState& state = tracks_[c.track];
    confirm(state, frame_time, detections[c.detection].box.rect);
    if (motions != nullptr) {
      (*motions)[c.detection] =
          state.has_velocity
              ? TrackMotion{state.track.velocity, state.first_seen, state.translating, state.track.id}
              : TrackMotion{Velocity{}, state.first_seen, false, state.track.id};
    }
  }
  // A cycle that did not find a track is a miss: the text may not be where
  // the track says. The run ends and nothing the track held so far is ever
  // followed, so a later confirmation cannot follow boxes across the miss.
  for (std::size_t ti = 0; ti < track_taken.size(); ++ti) {
    if (!track_taken[ti]) {
      TrackState& state = tracks_[ti];
      state.run = 0;
      for (Held& held : state.held) {
        held.followable = false;
      }
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
    state.anchor = detection.box.rect;
    state.anchor_time = frame_time;
    state.held.push_back(make_held(detection.box.rect, frame_time, Velocity{}, true));
    if (motions != nullptr) {
      (*motions)[di].track = state.track.id;
    }
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
    // Never followed: its place in the run was not measured.
    state.held.push_back(make_held(box, frame_time, track.velocity, false));
    return;
  }
  const double dt = seconds(frame_time - track.last_confirmed);
  const double measured_x = (center_x(box) - center_x(track.box.rect)) / dt;
  const double measured_y = (center_y(box) - center_y(track.box.rect)) / dt;
  measure_run(state, frame_time, box);
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
  track.box = Box{box, Space::Source};
  track.last_confirmed = frame_time;
  track.hold_until = frame_time + config_.hold;
  state.held.push_back(make_held(box, frame_time, track.velocity, true));
}

// Run state machine (docs/tracker.md, "Run state"), in check order:
//   size changed over a quarter        -> no run, re-anchor at `box`
//   moved under follow_min_move        -> unchanged, unless still for longer
//                                         than the hold: no run, re-anchor
//   measured, not a translation        -> no run (anchor moved to `box`)
//   consistent with the run            -> run + 1, widen [run_low, run_high]
//   any other translation              -> run 1, from the anchor's frame time
//                                         if there was no run, else from `box`
// Every measured case moves the anchor to `box`. A missed cycle and
// rescale() end the run outside this function (update(), rescale()).
void Tracker::measure_run(TrackState& state, Timestamp frame_time, const Rect& box) const noexcept {
  const Rect from = state.anchor;
  if (!same_size(from, box)) {
    // Another size: likely another line took the track. The run ends.
    state.run = 0;
    state.anchor = box;
    state.anchor_time = frame_time;
    return;
  }
  const double dx = center_x(box) - center_x(from);
  const double dy = center_y(box) - center_y(from);
  if (std::max(std::abs(dx), std::abs(dy)) < static_cast<double>(config_.follow_min_move)) {
    // Too close to the last measured box to tell motion from detection
    // jitter (two workers can scan frames a few ms apart): not measured yet.
    // A track that has not measurably moved for longer than the hold has
    // stopped: the run ends, and the next one starts from this box, so what
    // it held while still is never followed.
    if (frame_time - state.anchor_time > config_.hold) {
      state.run = 0;
      state.anchor = box;
      state.anchor_time = frame_time;
    }
    return;
  }
  const double dt = seconds(frame_time - state.anchor_time);
  const Velocity measured{to_velocity(dx / dt), to_velocity(dy / dt)};
  const Velocity previous = state.measured;
  state.measured = measured;
  state.anchor = box;
  const Timestamp from_time = state.anchor_time;
  state.anchor_time = frame_time;
  if (!translated(from, box, measured)) {
    state.run = 0;
    return;
  }
  if (state.run > 0) {
    const bool along_x = std::abs(measured.dx_per_s) >= std::abs(measured.dy_per_s);
    const bool before_x = std::abs(previous.dx_per_s) >= std::abs(previous.dy_per_s);
    const double now = static_cast<double>(along_x ? measured.dx_per_s : measured.dy_per_s);
    const double before = static_cast<double>(before_x ? previous.dx_per_s : previous.dy_per_s);
    const double ratio = static_cast<double>(config_.follow_speed_ratio);
    if (along_x == before_x && now * before > 0.0 && std::abs(now) <= ratio * std::abs(before) &&
        std::abs(before) <= ratio * std::abs(now)) {
      state.run = std::min(state.run + 1, kMaxFollowAfter);
      state.run_low = Velocity{std::min(state.run_low.dx_per_s, measured.dx_per_s),
                               std::min(state.run_low.dy_per_s, measured.dy_per_s)};
      state.run_high = Velocity{std::max(state.run_high.dx_per_s, measured.dx_per_s),
                                std::max(state.run_high.dy_per_s, measured.dy_per_s)};
      return;
    }
  }
  // A translation that does not continue the run starts a new one: from the
  // box it was measured from, except after a broken run. There that box
  // ended the old run moving another way and may be another line's, one this
  // box took the track over from, so the new run starts with this box.
  state.run_from = state.run > 0 ? frame_time : from_time;
  state.run = 1;
  state.run_low = measured;
  state.run_high = measured;
}

Tracker::Held Tracker::make_held(const Rect& box, Timestamp seen, Velocity velocity,
                                 bool followable) const noexcept {
  const double margin =
      static_cast<double>(config_.margin_px) +
      static_cast<double>(config_.margin_ratio) *
          std::min(static_cast<double>(box.width), static_cast<double>(box.height));
  return Held{box, seen, followable, velocity, velocity, margin, margin};
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

// Following, rule 5 of tracker.hpp's "Following" and docs/tracker.md
// ("Followed"). The property test's oracle `may_follow`
// (tests/unit/tracker_test.cpp) restates the same rule independently; change
// the three together (see track/follow.hpp).
const Tracker::Held* Tracker::follow_target(const TrackState& state, Timestamp t) const noexcept {
  if (!follow::active(config_.follow_after, state.run, t, state.track.last_confirmed)) {
    return nullptr;
  }
  const Held* newest = nullptr;
  for (const Held& held : state.held) {
    if (held.followable && held.seen >= state.run_from && held.seen <= t &&
        (newest == nullptr || held.seen > newest->seen)) {
      newest = &held;
    }
  }
  return newest;
}

// An older run detection is covered by `newest`'s mask instead of its own
// only if `newest`'s box grown by its margin contains it moved to `newest`'s
// frame time at every velocity between the run's lowest and highest
// measurement. A newer detection that found only part of the text (clipped
// within the size tolerance) so never releases a box that covered all of it:
// the part it missed lies outside it, and the measurement it biased only
// widens the range.
bool Tracker::followed(const TrackState& state, const Held* newest, const Held& held) noexcept {
  if (newest == nullptr || !held.followable || held.seen < state.run_from ||
      held.seen >= newest->seen) {
    return false;
  }
  return follow::contains_moved(
      follow::Target{newest->box, newest->seen, newest->margin_x, newest->margin_y}, held.box,
      held.seen, state.run_low, state.run_high);
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
    // Following (docs/tracker.md): a followed detection adds no rect on t;
    // the newest detection of the run at or before t covers it.
    const Held* newest = follow_target(state, t);
    const std::size_t start = out.size();
    out.resize(start + slots, Box{kNoRect, Space::Source});
    for (const Held& held : state.held) {
      if (t < held.seen || t > held.seen + config_.hold) {
        continue;
      }
      if (followed(state, newest, held)) {
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
    // Nothing from before the resize is followed, and the next measurement
    // starts a new run (from the track's box in the new coordinates, below).
    state.run = 0;
    for (Held& held : state.held) {
      held.followable = false;
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
    state.anchor = state.track.box.rect;
    state.anchor_time = state.track.last_confirmed;
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
