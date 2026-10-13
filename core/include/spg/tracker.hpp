#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
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
  // Motion gate (SPG-16b). With one detection cycle every ~200 ms a small
  // fast box moves more than its own size between cycles, so IoU misses it.
  // After IoU pairing, a detection left over may join a live track of its
  // kind left over when its center lies within
  // motion_gate_per_s * min(gap, max_prediction) box sizes of the track's
  // last center or of the center its velocity predicts; gap is the frame
  // time between them, and a box size is the larger of the two boxes'
  // widths along x and heights along y. Closest pairs first. 0 turns the
  // gate off.
  float motion_gate_per_s = 20.0F;
  // The gate also needs the detection's width and height each within this
  // factor of the track's last box, in [1, inf).
  float motion_size_ratio = 2.0F;
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
  // Follow confirmed moving tracks (SPG-12b). On a frame time that two
  // detections of a confirmed moving track bracket (see Tracker), its older
  // held boxes are masked at the track's measured position, each with its own
  // size and margin, instead of where they were seen. false holds every box
  // where it was seen, as before SPG-12b.
  bool follow_moving_tracks = true;

  // Hold, margin and retention (lookahead) from `policy`; the rest default.
  [[nodiscard]] static TrackerConfig from_policy(const PolicyConfig& policy);

  friend bool operator==(const TrackerConfig&, const TrackerConfig&) = default;
};

// Throws ContractError naming the field when a value is out of range: a
// negative duration, a negative or non-finite margin, an IoU threshold
// outside (0, 1], a negative or non-finite motion gate, a motion size ratio
// below 1 or not finite, a smoothing weight outside (0, 1] or a
// max_rects_per_track outside [1, 64]. A zero hold or margin
// is accepted here; whether to impose a floor is #28 (SPG-08b).
void validate(const TrackerConfig& config);

// What Tracker::update() reports for one detection: the motion of the track
// it joined, after the update. When the motion gate paired that track with a
// detection (SPG-16b), the pairing may be wrong: the detection may be a new
// object that only came near an old track. Then, once the detections from
// that gate pairing on have a motion measurement, those detections' motion is
// reported, as for the new track the gated detection would otherwise have
// started; until then, the whole track's. Either way the scheduler gets at
// least the motion it would have had without the gate.
struct TrackMotion {
  // The track's velocity estimate; zero for a detection that started a track
  // or joined one with no motion measurement yet.
  Velocity velocity;
  // Frame time of the earliest detection the reported motion has had: when
  // the track (or, as above, the run since its latest gate pairing) was
  // first seen. A late detection that joins can move it earlier.
  Timestamp first_seen{};
  // A motion measurement of the track has moved both edges of its box at
  // least one pixel the same way along the axis its velocity is larger on (x
  // on a tie), in the direction of the velocity: the box translated (it slid
  // or scrolled), not grew or shrank as text does while it fades in or is
  // typed, nor shifted by less than a mask pixel. Sticky: once set it stays
  // set for the life of the track, so results that land out of order see the
  // same answer. False with no measurement yet. A late detection older than
  // all the track's others also counts its move to the latest box.
  bool translating = false;
  // The track the detection joined (Track::id).
  TrackId track = 0;

  friend bool operator==(const TrackMotion&, const TrackMotion&) = default;
};

// Follows detections across frames and answers which Source-space regions
// must be masked on the frame with a given timestamp.
//
// Guarantees, all in Source pixels and frame time:
//   * Hold: a detection with box b on frame time td is covered, grown by the
//     margin, on every frame time t in [td, td + hold]. Later detections,
//     tracking loss, missed cycles and resizes never remove that coverage,
//     with the one exception below.
//   * Follow (SPG-12b, follow_moving_tracks): on a frame time t that two
//     sightings a and b of a confirmed moving track bracket (the latest and
//     earliest of its detections that moved it forward, at or before and at
//     or after t), a detection d of the track's current run seen before a is
//     masked at the track's measured position instead of where it was seen:
//     d's box moved by the measured move from d to a and from d to b (the
//     hull of both), grown by d's margin. The hull of a and b, grown by the
//     larger of their margins, is masked too, so text that moved one way
//     from a to b is covered at every frame time between them. A track is
//     confirmed moving after two steady translations the same way (each
//     move off what the last one's speed predicts by at most half of it and
//     half the box along it, or 3 px); its run ends when its box stops,
//     turns, changes speed, is paired by the motion gate or the source is
//     resized. Late detections, boxes held before the run (a run that leaves
//     a box seen at rest, or that a change of speed restarts, starts at the
//     box that restarted it) and boxes held before a gate pairing or resize
//     are never moved. On frame times after the track's last detection, every
//     box is held where it was seen.
//   * Prediction: on frame times after td the mask also covers b moved at the
//     track's estimated velocity for min(t - td, max_prediction), as the hull
//     of b and the moved box, so between detection cycles the mask only grows.
//   * Between sightings: when a detection on frame time t2 moves its track on
//     from its last box b1 on t1 < t2, the hull of b1 and the new box, with
//     the larger margin, is held from t1 like a detection (left out when the
//     new box lies inside b1, which b1's own hold already covers).
//   * Motion gate: a detection the gate pairs with a track is also measured
//     as the track it would otherwise have started. Its held masks predict
//     over both velocities, the bridges of both are held and TrackMotion
//     reports that motion once it has one, so a wrong pairing never removes
//     coverage the detection would have had without the gate.
//   * Updates only add: masks(t) after update() contains every rect masks(t)
//     returned before, for any t the retention still covers that is after
//     every track's last detection (before it, Follow may move boxes).
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
  // joins the best-matching live track of its kind (greedy by IoU, then the
  // tracks and detections left over greedy by the motion gate) or starts a
  // new track. Detections may arrive late or out of order; an older
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
    bool bridge = false;    // the hull between two sightings, not a detection
    bool late = false;      // a detection that did not move its track forward
    std::uint64_t seq = 0;  // order held in its track
  };
  // Whether a track is confirmed moving (SPG-12b). Measured from a checkpoint
  // box, so the short gap between two workers' paired cycles (a pixel or two)
  // neither counts nor breaks the run.
  struct Follow {
    Rect checkpoint;
    Timestamp checkpoint_time{};
    bool has_checkpoint = false;
    int translations = 0;      // steady, the same way, from run_start
    bool along_x = true;       // their axis
    bool positive = true;      // and direction along it
    double speed = 0.0;        // of the last one along that axis, px/s
    Timestamp run_start{};     // boxes seen before it never move
    // A box seen kMinBaseline or more after the checkpoint did not translate
    // from it: the text rested there, so a run that starts from this
    // checkpoint starts at its first translation instead.
    bool rested = false;
    std::uint64_t barrier = 0;  // boxes held before this seq never move
  };
  // What confirm() measures from a run of detections: the latest box (in
  // frame time) and its frame time, the earliest frame time, the velocity
  // estimate and whether the box translated (see TrackMotion).
  struct Motion {
    Rect box;
    Timestamp last{};
    Timestamp first_seen{};
    Velocity velocity;
    bool has_velocity = false;  // a motion measurement has been taken
    bool translating = false;   // see TrackMotion::translating
  };
  struct TrackState {
    Track track;  // box, velocity and last_confirmed follow `motion`
    std::vector<Held> held;
    Motion motion;  // of every detection the track took
    // Of the detections from the track's latest motion-gate pairing on: the
    // track that detection would have started without the gate. The same as
    // `motion` until the gate pairs the track.
    Motion since_gate;
    Follow follow;
    std::uint64_t next_seq = 0;
  };

  [[nodiscard]] Rect predicted(const Track& track, Timestamp t) const noexcept;
  // Adds a detection to `motion`; true when it moved the motion forward in
  // frame time (a newer box), false for a late or same-frame one.
  bool advance(Motion& motion, Timestamp frame_time, const Rect& box) const noexcept;
  // `gated`: the motion gate paired the detection with the track.
  void confirm(TrackState& state, Timestamp frame_time, const Rect& box, bool gated);
  // Holds the hull of `from` (seen on `since`) and `to` from `since`, unless
  // `to` lies inside `from`.
  void bridge(TrackState& state, const Rect& from, Timestamp since, const Rect& to) const;
  [[nodiscard]] Held make_held(const Rect& box, Timestamp seen, Velocity velocity) const noexcept;
  [[nodiscard]] Rect mask_rect(const Held& held, Timestamp t) const noexcept;
  // Appends `held` to the track with the next seq.
  static void hold(TrackState& state, Held held);
  // A forward detection moved the track to `box` on `frame_time`.
  static void follow_measure(Follow& follow, const Rect& box, Timestamp frame_time) noexcept;
  // Masks of one track on frame time `t`, merged into `slots` time buckets.
  void track_masks(const TrackState& state, Timestamp t, std::int64_t width, std::int64_t first,
                   std::span<Box> slots) const noexcept;

  const Clock* clock_;
  TrackerConfig config_;
  std::vector<TrackState> tracks_;
  TrackId next_id_ = 1;
};

}  // namespace spg
