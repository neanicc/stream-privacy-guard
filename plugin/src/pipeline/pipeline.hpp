#pragma once

// The filter's per-frame logic (SPG-14), without OBS: capture into the
// lookahead ring, double-buffered readback for the scheduler, release, and
// what each output frame shows. plugin/src/render/ executes it with libobs
// graphics; plugin/tests/pipeline_test.cpp drives it with a fake GPU and a
// synchronous scheduler on a ManualClock. Rules: docs/plugin-render.md.
//
// Guarantees (each tested):
//   * Fail closed: a frame is drawn unmasked-except-for-its-rects only when
//     the scheduler released it with FallbackAction::None; every other frame,
//     every frame while no PolicyConfig is set, and every frame before the
//     first release is covered whole (blur, or solid fill when that is the
//     fallback or there is no texture to blur). Nothing draws the frame just
//     captured unless it is covered whole.
//   * Retroactive masking and hold come from the scheduler's MaskPlan, drawn
//     on the buffered texture of that frame.
//   * Readback: the frame staged on one tick is mapped on the next and pushed
//     with its own timestamp, so the scheduler credits the frame it shows.
//   * A 0x0 target or a resize never drops a held mask: the scheduler and its
//     buffered frames are kept, and buffered frames of the old size are drawn
//     scaled to the output with their own masks.
//   * A settings change starts a new scheduler; frames buffered by the old one
//     still air with the old one's masks, and frames of the new one within the
//     old hold of the switch are covered whole, since their held masks were in
//     the old tracker. Once a newer generation has aired a frame, an older
//     one's frames never air again (the picture never jumps back), and at
//     most kMaxGenerations schedulers are live.
//   * Schedulers are never destroyed here (their destructors wait for
//     workers); they go to the `retire` callback.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "pipeline/frame_ring.hpp"
#include "pipeline/readback_pool.hpp"
#include "spg/geometry.hpp"
#include "spg/lookahead.hpp"
#include "spg/mask.hpp"
#include "spg/scan.hpp"
#include "spg/scheduler.hpp"
#include "spg/time.hpp"

namespace spg::plugin {

// The scheduler as the pipeline drives it.
class FrameScheduler {
 public:
  FrameScheduler() = default;
  FrameScheduler(const FrameScheduler&) = delete;
  FrameScheduler& operator=(const FrameScheduler&) = delete;
  virtual ~FrameScheduler() = default;
  [[nodiscard]] virtual bool wants_scan(Timestamp t) const = 0;
  virtual FrameId push(Timestamp t, Size source, std::shared_ptr<const OwnedFrame> readback) = 0;
  virtual void release_due(std::vector<ReleasedFrame>& out) = 0;
  virtual void mark_discontinuity() = 0;
  [[nodiscard]] virtual SchedulerStats stats() const = 0;

 private:
  friend class Reaper;
  // The reaper's queue link, so retiring a scheduler never allocates.
  std::unique_ptr<FrameScheduler> reaper_next_;
};

// spg::Scheduler (worker threads) behind FrameScheduler.
class ThreadedScheduler final : public FrameScheduler {
 public:
  ThreadedScheduler(const Clock& clock, SchedulerConfig config, ScanStages stages)
      : scheduler_(clock, std::move(config), std::move(stages)) {}
  [[nodiscard]] bool wants_scan(Timestamp t) const override { return scheduler_.wants_scan(t); }
  FrameId push(Timestamp t, Size source, std::shared_ptr<const OwnedFrame> readback) override {
    return scheduler_.push(t, source, std::move(readback));
  }
  void release_due(std::vector<ReleasedFrame>& out) override { scheduler_.release_due(out); }
  void mark_discontinuity() override { scheduler_.mark_discontinuity(); }
  [[nodiscard]] SchedulerStats stats() const override { return scheduler_.stats(); }

 private:
  Scheduler scheduler_;
};

// A mapped stage surface: BGRA rows `linesize` bytes apart.
struct MappedReadback {
  const std::uint8_t* data = nullptr;
  std::uint32_t linesize = 0;
};

// The GPU work the pipeline asks for, in render order within one tick.
class FrameGpu {
 public:
  FrameGpu() = default;
  FrameGpu(const FrameGpu&) = delete;
  FrameGpu& operator=(const FrameGpu&) = delete;
  virtual ~FrameGpu() = default;
  // Renders the filter's target at `size` into ring slot `slot`'s texture
  // and records `serial` and `size` as what the slot now holds (see
  // SlotContents); on failure the slot must report itself empty.
  virtual bool capture(std::size_t slot, Size size, std::uint64_t serial) = 0;
  // Draws slot `slot` scaled to `readback` and stages it into stage surface
  // `surface` (0 or 1) for mapping on the next tick.
  virtual bool stage(std::size_t slot, std::size_t surface, Size readback) = 0;
  // Maps the surface staged on the previous tick.
  virtual bool map(std::size_t surface, MappedReadback& out) = 0;
  virtual void unmap(std::size_t surface) = 0;
  // Frees an idle slot's texture.
  virtual void free_slot(std::size_t slot) = 0;
};

enum class DrawKind : std::uint8_t {
  Nothing,    // a 0x0 target: there is nothing to draw on
  Masked,     // the slot's frame with each rect masked
  BlurWhole,  // the slot's frame blurred whole
  Solid,      // an opaque fill the size of the output
};

// Why a frame is covered (shown by the debug overlay and the properties).
enum class Cover : std::uint8_t {
  None,             // released clean; only its rects are masked
  NotScanned,       // scheduler: no complete cycle covered it in time
  LateRelease,      // scheduler: released after its deadline and grace
  BadTimestamp,     // scheduler: frame time not increasing or off the clock
  ApplyFailed,      // scheduler: a result could not be fully applied
  Starting,         // nothing released yet (the first lookahead after a start)
  NoDetection,      // no PolicyConfig (storage error, no categories, rejected
                    // settings) or no detection models (loading, missing, failed)
  SettingsChanged,  // within the old hold after a settings change
  NoTexture,        // the frame could not be kept (ring full or capture failed)
  Stalled,          // nothing released for longer than Pipeline::kMaxRepeat
  Inconsistent,     // a released frame did not match the frame buffered for it,
                    // or the render side could not build a plan this frame
};
inline constexpr std::size_t kCoverCount = 11;

struct DrawPlan {
  DrawKind kind = DrawKind::Nothing;
  std::size_t slot = 0;  // Masked and BlurWhole
  // The capture serial of the frame in `slot`: the GPU side draws the slot
  // only while it still holds exactly this capture (checked_kind()).
  std::uint64_t serial = 0;
  Size frame;            // the source size of the frame in `slot`
  std::vector<MaskRect> rects;  // Masked: Source space of `frame`
  Cover cover = Cover::None;
  // A HoldLastScanned re-emit: the last clean frame shown again as it aired.
  bool reemit = false;
  // Shows the frame just captured (always covered whole); replaced next tick.
  bool live = false;
};

// What a ring slot holds on the GPU side right now.
struct SlotContents {
  bool present = false;      // a texture exists and its last capture succeeded
  std::uint64_t serial = 0;  // the capture serial it holds
  Size size;                 // the size it was captured at
};

// What may actually be drawn for `plan`, decided from one snapshot of the
// slot it names: Solid whenever the slot is missing, holds another capture
// (recycled or never captured), or a different size than the plan expects;
// BlurWhole when a Masked plan lists a rect that is invalid or not in Source
// space (a partial mask is never drawn); otherwise plan.kind.
[[nodiscard]] DrawKind checked_kind(const DrawPlan& plan, const SlotContents& slot) noexcept;

// One scheduler generation, made off the render thread when the settings
// change (plugin/src/render/filter_render.cpp).
struct PipelineSpec {
  // Null when no PolicyConfig is set: every frame is covered whole.
  std::unique_ptr<FrameScheduler> scheduler;
  FallbackMode fallback = FallbackMode::BlurSource;
  // The scheduler's hold, so the next generation covers its first frames.
  Duration hold{};
};

struct PipelineCounters {
  std::uint64_t ticks = 0;
  std::uint64_t released = 0;
  std::uint64_t readbacks = 0;         // readbacks handed to a scheduler
  std::uint64_t capture_failures = 0;  // frames kept without a texture
  // Output frames drawn, by the Cover of the plan each tick ends with
  // (Cover::None: clean). Frames released and replaced within the same tick
  // never aired and are not counted; a plan repeated for a tick with no
  // release is not counted again.
  std::array<std::uint64_t, kCoverCount> shown{};
};

// Fits `source` into `max` keeping its aspect ratio, never scaling up; each
// side at least 1. The readback is the whole source at this size.
[[nodiscard]] Size readback_size(Size source, Size max) noexcept;

inline constexpr Size kMaxReadback{1920, 1080};

class Pipeline {
 public:
  using Retire = std::function<void(std::unique_ptr<FrameScheduler>)>;

  // Starts with no scheduler (every frame covered whole) until adopt().
  // `ring_capacity` bounds the textures held at once across generations.
  Pipeline(FrameGpu& gpu, std::size_t ring_capacity, Retire retire, Size max_readback = kMaxReadback);
  // Retires every scheduler. GPU resources belong to the caller.
  ~Pipeline();
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  // Switches to a new generation at frame time `now`. Strong guarantee: if it
  // throws (allocation), nothing changed and `spec` still owns its scheduler.
  void adopt(PipelineSpec& spec, Timestamp now);

  // One output frame: `t` is the video frame time, `source` the target's size
  // now. A second call with the same `t` (the source rendered in another
  // view) changes nothing and returns the same plan.
  const DrawPlan& tick(Timestamp t, Size source);

  [[nodiscard]] const DrawPlan& plan() const noexcept { return display_; }
  [[nodiscard]] const PipelineCounters& counters() const noexcept { return counters_; }
  [[nodiscard]] std::size_t generations() const noexcept { return gens_.size(); }
  [[nodiscard]] const FrameRing& ring() const noexcept { return ring_; }
  // Test seam: runs inside adopt() at the step that can throw (allocation).
  void set_adopt_fault(std::function<void()> fault) { adopt_fault_ = std::move(fault); }
  // Idle textures kept for reuse beyond those in use.
  static constexpr std::size_t kMaxIdleSlots = 4;
  // A released frame stays on screen for at most this long with nothing new
  // released; then the live frame is covered whole (Cover::Stalled).
  static constexpr Duration kMaxRepeat = std::chrono::milliseconds{100};
  // Frame times further apart than this (the filter was disabled, or its
  // source not rendered) or going backwards mark a discontinuity: content may
  // have changed unseen, so no earlier scan or re-emit stands in for it.
  static constexpr Duration kMaxTickGap = std::chrono::milliseconds{200};
  static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);
  // Live scheduler generations: the current one and one still draining. A
  // further settings change retires the draining one unless it is on screen
  // (its frames then never air; the output is covered until the new one
  // releases), so repeated changes cannot pile up worker threads or slots.
  static constexpr std::size_t kMaxGenerations = 2;

 private:
  // A pushed frame as the pipeline recorded it; a release that does not
  // match it (time or size) is covered whole.
  struct Buffered {
    FrameId id = 0;
    std::size_t slot = kNoSlot;
    std::uint64_t serial = 0;
    Timestamp t{};
    Size size;
  };
  struct Generation {
    std::uint64_t id = 0;
    std::unique_ptr<FrameScheduler> scheduler;
    FallbackMode fallback = FallbackMode::BlurSource;
    Duration hold{};
    std::optional<Timestamp> covered_until;  // frames before this are covered (SettingsChanged)
    std::deque<Buffered> buffered;           // pushed, not yet released, oldest first
  };
  struct LastClean {
    std::uint64_t generation = 0;
    FrameId frame = 0;
    std::size_t slot = 0;
    std::uint64_t serial = 0;
    Size size;
    std::vector<MaskRect> rects;
  };
  struct Staged {
    std::size_t surface = 0;
    Timestamp t{};
    std::uint64_t generation = 0;
    Size size;
  };

  void push_frame(Generation& gen, Timestamp t, Size source, std::optional<std::size_t> slot);
  std::optional<Buffered> take_buffered(Generation& gen, FrameId id) noexcept;
  void release_buffered(Generation& gen, FrameId id) noexcept;
  void show_released(Generation& gen, const ReleasedFrame& frame);
  void show_live(std::optional<std::size_t> slot, Size source, FallbackMode fallback, Cover cover);
  void cover_whole(DrawPlan& next, std::size_t slot, std::uint64_t serial, Size size, FallbackMode fallback) const;
  void mark_discontinuity() noexcept;
  void set_display(DrawPlan& next);
  void forget_last_clean() noexcept;
  void retire_drained();
  void drop_buffered(Generation& gen) noexcept;
  void count_display() noexcept;
  void retire(std::unique_ptr<FrameScheduler> scheduler) noexcept;
  void trim_idle() noexcept;

  FrameGpu* gpu_;
  Retire retire_;
  Size max_readback_;
  FrameRing ring_;
  ReadbackPool pool_;
  std::deque<Generation> gens_;  // retiring ones first; back() is current
  std::uint64_t next_generation_ = 1;
  DrawPlan display_;
  DrawPlan next_;  // scratch, kept for its capacity
  std::optional<LastClean> last_clean_;
  std::optional<Staged> staged_;
  std::size_t next_surface_ = 0;
  std::optional<Timestamp> last_tick_;
  std::optional<Timestamp> last_release_;  // tick time of the last plan made from a release
  std::uint64_t next_serial_ = 1;
  std::uint64_t live_serial_ = 0;  // serial of this tick's capture
  bool target_empty_ = false;
  // The generation of the newest released frame put on screen; releases of
  // older generations are dropped unseen.
  std::uint64_t aired_generation_ = 0;
  bool display_set_ = false;  // set_display() ran during this tick
  std::vector<ReleasedFrame> released_;
  PipelineCounters counters_;
  std::function<void()> adopt_fault_;
};

}  // namespace spg::plugin
