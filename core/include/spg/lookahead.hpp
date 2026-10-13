#pragma once

// The lookahead buffer and fail-closed release rule (SPG-10), as a
// single-threaded state machine. spg::Scheduler wraps it with worker threads;
// tests and offline tools drive it directly with a ManualClock. Rules and
// rationale: docs/scheduler.md.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "spg/category.hpp"
#include "spg/geometry.hpp"
#include "spg/mask.hpp"
#include "spg/policy_config.hpp"
#include "spg/scan.hpp"
#include "spg/time.hpp"
#include "spg/tracker.hpp"

namespace spg {

struct SchedulerConfig {
  // A frame is released once the clock reaches its timestamp + lookahead.
  Duration lookahead = std::chrono::milliseconds{500};
  // A completed cycle on frame time ts masks buffered frames from ts - retro
  // and counts as a complete scan for them.
  Duration retro = std::chrono::milliseconds{500};
  FallbackMode fallback = FallbackMode::BlurSource;
  MaskStyle mask_style = MaskStyle::Blur;
  // Hold and margin. `retain` is raised to at least lookahead + release_grace
  // so held detections outlive every frame that may still ask for them.
  TrackerConfig tracker;
  // Cadence: a frame is queued for scanning only when at least this much
  // frame time has passed since the last queued frame. 0 offers every frame.
  Duration scan_interval{0};
  // Bounded scan queue: when full, the oldest queued job is dropped.
  std::size_t queue_capacity = 1;
  // Worker threads (Scheduler only).
  std::size_t workers = 2;
  // When more frames than this are buffered, the oldest is released early,
  // under the same coverage rule.
  std::size_t max_buffered_frames = 600;
  // Retroactive rects kept per buffered frame; past this they merge into one
  // hull (more blur, never less).
  std::size_t max_retro_rects = 64;
  // scan.categories gates the stages (Face gates the face detector) and
  // nothing else; the policy limits text reasons itself.
  ScanLimits scan;
  // Frame timestamps must be within this of clock.now() when pushed, and must
  // increase; otherwise the clock and frames do not share one time base and
  // the frame fails closed.
  Duration clock_tolerance = std::chrono::seconds{1};
  // A frame released later than timestamp + lookahead + release_grace (the
  // caller stopped asking) fails closed: held detections it needed may have
  // been pruned.
  Duration release_grace = std::chrono::milliseconds{500};
  // Scheduler only: how long its destructor waits for workers before
  // detaching any still inside a stage call.
  Duration shutdown_timeout = std::chrono::seconds{2};

  // Lookahead, retro, fallback, mask style, categories, hold and margin from
  // `policy`; the rest default.
  [[nodiscard]] static SchedulerConfig from_policy(const PolicyConfig& policy);
};

// Throws ContractError naming the field when a duration is negative, a
// capacity is zero, or the tracker config is invalid. A zero lookahead,
// retro or hold is accepted and fails closed rather than open (see
// docs/scheduler.md).
void validate(const SchedulerConfig& config);

enum class FallbackAction : std::uint8_t {
  None,             // the frame was fully scanned; apply plan.rects
  BlurSource,       // blur the whole source
  SolidFill,        // fill the whole source
  HoldLastScanned,  // show frame `reemit` again, exactly as it was released
};

enum class FallbackReason : std::uint8_t {
  None,
  NotScanned,    // no complete cycle covered the frame before its release
  LateRelease,   // released after timestamp + lookahead + release_grace
  BadTimestamp,  // not increasing, or outside clock_tolerance of the clock
  ApplyFailed,   // a complete cycle's boxes could not all be applied to it
};

struct ReleasedFrame {
  FrameId id = 0;
  Timestamp frame_time{};
  Size source;
  // Source-space rects for this frame (plan.fallback set when it failed
  // closed; rects still list what was found).
  MaskPlan plan;
  FallbackAction action = FallbackAction::None;
  FallbackReason reason = FallbackReason::None;
  // For HoldLastScanned: the last frame released with action None in the same
  // source size and content, which still passes the release gate: no complete
  // cycle that finished after it was released could have masked it. Never set
  // otherwise.
  FrameId reemit = 0;
};

struct SchedulerStats {
  std::uint64_t frames_pushed = 0;
  std::uint64_t frames_released = 0;
  std::uint64_t frames_failed_closed = 0;
  std::uint64_t early_releases = 0;  // buffer over max_buffered_frames
  std::uint64_t bad_timestamps = 0;
  std::uint64_t scans_queued = 0;
  std::uint64_t scans_dropped = 0;  // pushed out of a full queue
  std::uint64_t scans_started = 0;
  std::uint64_t scans_completed = 0;
  std::uint64_t scans_failed = 0;
  friend bool operator==(const SchedulerStats&, const SchedulerStats&) = default;
};

// Guarantees (each tested in tests/unit/lookahead_test.cpp):
//   * Fail closed: a frame is released with FallbackAction::None only when a
//     complete cycle on a frame of the same content, with frame time in
//     [t, t + retro], finished before the release, and the release is within
//     t + lookahead + release_grace. Any other frame gets the fallback.
//   * Retroactive masking: a complete cycle on frame time ts adds its boxes,
//     grown by the margin, to every buffered frame with time in
//     [ts - retro, ts) (scaled across a resize, hulled with the unscaled box).
//   * Hold: plan rects include Tracker::masks(t), so a detection stays masked
//     for the hold, except on frames where its confirmed moving track follows
//     it (a newer detection of the track pins the text; see Tracker,
//     "Following"). A resize only adds coverage, a late cycle only adds
//     coverage except by confirming such a track, and a box from before a
//     resize keeps its scaled margin and is never followed.
//   * Partial cycles: the detections of an incomplete result (found by the
//     stages that finished before a failure) are held and masked
//     retroactively like a complete cycle's, but the cycle covers no frame.
//   * A cycle whose detections cannot be fully applied (an allocation
//     or tracker failure, a result from before more resizes than the size
//     history holds, or a scan that lost some of the boxes it found) fails
//     closed every frame it could have masked: buffered
//     frames from ts - retro on, frames pushed up to ts + hold, and the frame
//     HoldLastScanned would show again.
//   * Frames are released once each, in push order, never before
//     t + lookahead unless the buffer is over max_buffered_frames.
//   * One gate: a released frame and a re-emitted one (HoldLastScanned) pass
//     the same check. A frame shown again is the frame as it was released, so
//     a complete cycle that finishes later and could have masked it (or whose
//     result cannot be applied) makes it ineligible; the fallback blurs.
//
// Not thread-safe. The Clock must share the frames' FrameClock time base.
class Lookahead {
 public:
  // Throws ContractError when validate(config) fails. `clock` must outlive
  // this object.
  Lookahead(const Clock& clock, SchedulerConfig config);

  // True when a frame at `t` would be queued for scanning (cadence). The
  // caller can skip the readback copy otherwise.
  [[nodiscard]] bool wants_scan(Timestamp t) const noexcept;

  // Buffers the frame with timestamp `t` and Source size `source`. A size
  // change starts a new segment and rescales held detections. Throws
  // ContractError for a zero source size (changing nothing).
  //
  // When `readback` is given and the cadence allows, queues a scan of it for
  // the buffered frame whose timestamp equals readback->view().timestamp:
  // the frame being pushed, or an earlier one when the readback lags (a
  // mapped stage surface holds the previous frame). A readback that matches
  // no buffered frame with a valid timestamp is not scanned, and nothing is
  // scanned when the pushed frame's own timestamp is bad.
  FrameId push(Timestamp t, Size source, std::shared_ptr<const OwnedFrame> readback = nullptr);

  // The newest queued job, if any. Never throws: if the job cannot be
  // recorded as in flight it is dropped (counted in scans_dropped) and its
  // frames fail closed unless another cycle covers them.
  [[nodiscard]] std::optional<ScanJob> take_job() noexcept;

  // Applies a finished job. An incomplete result covers nothing, but its
  // detections are held and masked retroactively; a complete result, or an
  // incomplete one with detections, that cannot be applied fails closed what
  // it could have masked, as does one whose detections were lost
  // (ScanResult::detections_lost).
  // Throws ContractError for a job that was not taken or was already
  // completed.
  void complete(const ScanJob& job, const ScanResult& result);

  // Appends every frame due by clock.now() (and any over the buffer limit),
  // oldest first.
  void release_due(std::vector<ReleasedFrame>& out);

  // The source content changed without a resize (scene or source switch):
  // earlier cycles no longer cover new frames and HoldLastScanned has no frame
  // to show until one is fully scanned again.
  void mark_discontinuity() noexcept;

  [[nodiscard]] std::size_t buffered() const noexcept { return frames_.size(); }
  [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }
  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_.size(); }
  [[nodiscard]] const SchedulerStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const SchedulerConfig& config() const noexcept { return config_; }

 private:
  struct FrameRecord {
    FrameId id = 0;
    Timestamp t{};
    Timestamp due{};  // t + lookahead, or push time + lookahead for a bad timestamp
    Size source;
    std::uint64_t size_epoch = 0;
    std::uint64_t segment = 0;
    bool covered = false;
    bool scanned = false;  // a complete cycle ran on this frame itself
    bool bad_timestamp = false;
    bool apply_failed = false;
    std::vector<Rect> retro;
  };

  [[nodiscard]] const FrameRecord* find_frame(Timestamp t) const noexcept;
  void apply(const ScanJob& job, const ScanResult& result);
  // A result that cannot be applied (complete, or partial with detections)
  // or that lost detections: fails closed every frame it could have masked
  // (buffered from ts - retro on, pushed up to ts + hold, and the frame
  // HoldLastScanned would re-emit).
  void fail_closed(const ScanJob& job) noexcept;
  void add_retro(FrameRecord& frame, const Rect& rect) const;
  // The release gate (docs/scheduler.md, "Release rule"), the only place that
  // decides a frame may air clean: None, or why it gets the fallback. `at` is
  // when its masks are composed: now for a release, the original release for
  // a re-emit (its masks were fixed then).
  [[nodiscard]] FallbackReason gate(const FrameRecord& frame, Timestamp at) const noexcept;
  [[nodiscard]] FallbackAction fallback_action(const FrameRecord& frame, FrameId& reemit) const noexcept;

  const Clock* clock_;
  SchedulerConfig config_;
  Tracker tracker_;
  std::deque<FrameRecord> frames_;
  std::deque<ScanJob> queue_;
  std::vector<std::uint64_t> in_flight_;
  // Source sizes by epoch, oldest first; first_epoch_ names sizes_.front().
  std::deque<Size> sizes_;
  std::uint64_t first_epoch_ = 0;
  std::uint64_t segment_ = 0;
  bool have_frame_ = false;
  Timestamp last_pushed_{};
  std::optional<Timestamp> last_queued_;
  // Frames pushed with t at or before this fail closed (ApplyFailed): a
  // result that could not be applied may have been held up to here.
  std::optional<Timestamp> apply_failed_until_;
  FrameId next_frame_ = 1;
  std::uint64_t next_job_ = 1;
  // Last frame released with FallbackAction::None, for HoldLastScanned, and
  // when it was released. Later results mark it (apply_failed) when they
  // could have masked it, so it fails the gate and is not shown again.
  std::optional<FrameRecord> last_clean_;
  Timestamp last_clean_at_{};
  SchedulerStats stats_;
  std::vector<Box> masks_buffer_;
};

}  // namespace spg
