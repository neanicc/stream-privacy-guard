#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <vector>

#include "spg/detection.hpp"
#include "spg/geometry.hpp"
#include "spg/policy_config.hpp"
#include "spg/time.hpp"

namespace spg {

// Tuning for Tracker. Hold, margin and retention come from PolicyConfig via
// from_policy(); the association and prediction numbers are starting points
// for SPG-12 to tune against the eval.
struct TrackerConfig {
  // Minimum time a detection's box stays masked after the frame it was found
  // on.
  Duration hold = std::chrono::milliseconds{750};
  // Each mask edge grows by margin_px + margin_ratio * min(width, height) of
  // the detection box, in Source pixels.
  float margin_px = 4.0F;
  float margin_ratio = 0.25F;
  // How long after its hold expires a detection is kept for queries about
  // older frames. Frames wait in the lookahead buffer, so a mask can be asked
  // for up to `lookahead` behind the clock.
  Duration retain = std::chrono::milliseconds{500};
  // A detection joins an existing track of the same kind when its IoU with
  // the track's predicted box (or its last confirmed box) is at least this.
  float iou_threshold = 0.3F;
  // Weight of the newest motion measurement in the velocity estimate, in
  // (0, 1]. 1 uses the last measurement only.
  float velocity_smoothing = 0.5F;
  // Constant-velocity prediction runs at most this far past a detection; the
  // predicted box then stops moving (it still holds until hold expiry).
  Duration max_prediction = std::chrono::milliseconds{500};
  // Upper bound on the rects masks() returns per track. A track's held
  // detections are merged, by the frame time they were seen on, into at most
  // this many fixed time buckets of width ceil(hold / (n - 1)); each bucket
  // yields the hull of its masks. Merging only adds coverage. 1 merges a
  // track into one rect.
  std::size_t max_rects_per_track = 8;

  // Hold, margin and retention (lookahead) from `policy`; the rest default.
  [[nodiscard]] static TrackerConfig from_policy(const PolicyConfig& policy);

  friend bool operator==(const TrackerConfig&, const TrackerConfig&) = default;
};

// Throws ContractError naming the field when a value is out of range: a
// negative duration, a negative or non-finite margin, an IoU threshold
// outside (0, 1], a smoothing weight outside (0, 1] or a
// max_rects_per_track outside [1, 64]. A zero hold or margin
// is accepted here; whether to impose a floor is #28 (SPG-08b).
void validate(const TrackerConfig& config);

// What Tracker::update() reports for one detection: the track it joined,
// after the update.
struct TrackMotion {
  // The track's velocity estimate; zero for a detection that started a track
  // or joined one with no motion measurement yet.
  Velocity velocity;
  // Frame time of the earliest detection the track has had: when it was
  // first seen. A late detection that joins can move it earlier.
  Timestamp first_seen{};
  // The track's last motion measurement moved both edges of its box at least
  // one pixel the same way along the axis its velocity is larger on (x on a
  // tie), in the direction of the velocity: the box translated (it slid or
  // scrolled), not grew or shrank as text does while it fades in or is
  // typed, nor shifted by less than a mask pixel. False with no measurement
  // yet. A late detection older than all the track's others also counts its
  // move to the latest box (results that finished out of order).
  bool translating = false;

  friend bool operator==(const TrackMotion&, const TrackMotion&) = default;
};

// Follows detections across frames and answers which Source-space regions
// must be masked on the frame with a given timestamp.
//
// Guarantees, all in Source pixels and frame time:
//   * Hold: a detection with box b on frame time td is covered, grown by the
//     margin, on every frame time t in [td, td + hold]. Later detections,
//     tracking loss, missed cycles and resizes never remove that coverage.
//   * Prediction: on frame times after td the mask also covers b moved at the
//     track's estimated velocity for min(t - td, max_prediction), as the hull
//     of b and the moved box, so between detection cycles the mask only grows.
//   * Updates only add: masks(t) after update() contains every rect masks(t)
//     returned before, for any t the retention still covers.
//   * Resize: after rescale(from, to), masks(t) contains every rect masks(t)
//     returned before, both as it was and scaled to the new size (rounded
//     outward), so content blurred before a resize stays blurred after it.
//   * Bound: masks(t) returns at most max_rects_per_track rects per live
//     track, in O(held + max_rects_per_track^2) work per track.
//
// Time is frame time (FrameClock nanoseconds). The injected Clock is read only
// to drop detections whose hold ended more than `retain` ago, by comparing
// clock.now() with frame timestamps, so it must report the same FrameClock
// time base (same epoch and units) as the frame timestamps passed in. A clock
// ahead of frame time would drop held detections early (fail open); one behind
// it would keep them forever. The owner (the scheduler, SPG-10) must guarantee
// this. Not thread-safe: that owner serializes calls.
class Tracker {
 public:
  // Throws ContractError when `config` is invalid. `clock` must outlive the
  // tracker.
  Tracker(const Clock& clock, TrackerConfig config);

  // Adds the detections found on the frame with timestamp `frame_time`. Each
  // joins the best-matching live track of its kind (greedy by IoU) or starts
  // a new track. Detections may arrive late or out of order; an older
  // detection still holds but does not move its track (the earliest one can
  // give a track its first velocity; see TrackMotion). Validates every
  // detection first and throws ContractError (changing nothing) when one is
  // invalid or not in Source space. Then drops expired detections.
  //
  // When `motions` is not null it is resized to detections.size() and entry
  // i receives the motion of the track detection i joined, after the update
  // (see TrackMotion). The scheduler back-projects retroactive masks with it.
  void update(Timestamp frame_time, std::span<const Detection> detections,
              std::vector<TrackMotion>* motions = nullptr);

  // Source-space rects to mask on the frame with timestamp `t`: per track, the
  // hull of each time bucket's held masks (see max_rects_per_track). A rect
  // that another rect of the same track contains is left out. Pure: does not
  // read the clock or change state. Meant for the scheduler or a worker, not
  // the OBS render path, which only composes precomputed masks.
  [[nodiscard]] std::vector<Box> masks(Timestamp t) const;
  // As above, into `out` (cleared first), so a caller can reuse its buffer.
  void masks(Timestamp t, std::vector<Box>& out) const;

  // The source was resized from `from` to `to` pixels. Every held box becomes
  // the hull of its old and scaled box, its margin grows to cover the scaled
  // old margin plus the old outward rounding, and its velocity range widens to
  // cover the scaled range, so masks cover both the old and the scaled old
  // masks; tracks continue in the new coordinates. Throws ContractError when
  // either size has a zero dimension.
  void rescale(Size from, Size to);

  // Drops detections whose hold ended more than `retain` before clock.now(),
  // and tracks left with none. update() calls this.
  void prune();

  // Live tracks: last confirmed box, velocity, last confirmed time, hold
  // expiry and kind.
  [[nodiscard]] std::vector<Track> tracks() const;

  [[nodiscard]] const TrackerConfig& config() const noexcept { return config_; }

 private:
  // One detection held by a track: its box (Source), the frame time it was
  // seen on, the range of velocities to predict over (per axis; equal unless
  // a rescale widened it) and the margin per axis (fixed when it was seen,
  // grown only by a rescale).
  struct Held {
    Rect box;
    Timestamp seen{};
    Velocity low;
    Velocity high;
    double margin_x = 0.0;
    double margin_y = 0.0;
  };
  struct TrackState {
    Track track;
    std::vector<Held> held;
    bool has_velocity = false;  // a motion measurement has been taken
    Timestamp first_seen{};     // earliest frame time of a detection it took
    bool translating = false;   // see TrackMotion::translating
  };

  [[nodiscard]] Rect predicted(const Track& track, Timestamp t) const noexcept;
  void confirm(TrackState& state, Timestamp frame_time, const Rect& box);
  [[nodiscard]] Held make_held(const Rect& box, Timestamp seen, Velocity velocity) const noexcept;
  [[nodiscard]] Rect mask_rect(const Held& held, Timestamp t) const noexcept;

  const Clock* clock_;
  TrackerConfig config_;
  std::vector<TrackState> tracks_;
  TrackId next_id_ = 1;
};

}  // namespace spg
