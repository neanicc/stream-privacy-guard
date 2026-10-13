#pragma once

// Drives the SPG-10 scheduler over a frame sequence (SPG-11). Two modes:
//
//   run_simulated  spg::Lookahead on a ManualClock, single-threaded. Frames
//                  arrive at `fps` in simulated time; `workers` simulated
//                  workers take the newest queued scan, and each cycle lasts
//                  `cycle` of simulated time (fixed, so the output depends
//                  only on the frames, the policy and the models), or the
//                  measured wall time of that cycle when `cycle` is unset.
//                  Used by `run` and `eval` (fixed) and `bench` (measured).
//   run_realtime   spg::Scheduler with worker threads on the wall clock, fed
//                  one frame every 1/fps seconds, as the plugin will feed it.
//                  Used by `bench --realtime`.
//
// Both keep feeding copies of the last frame after the sequence ends, until
// every real frame is released: a live source keeps producing frames, and
// without them the last `retro` of every sequence could never be covered by a
// later scan and would always fail closed. Plans are written for real frames
// only. Docs: docs/cli.md.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "spg/compose.hpp"
#include "spg/lookahead.hpp"
#include "spg/mask.hpp"
#include "spg/policy_config.hpp"
#include "spg/scan.hpp"
#include "spg/time.hpp"
#include "timing.hpp"

namespace spg::cli {

struct FrameSource {
  std::size_t count = 0;
  std::uint32_t fps = 60;
  // Frame i at Source resolution. May throw (a missing or corrupt file).
  std::function<BgraImage(std::size_t)> load;
};

struct DriverOptions {
  // Readback size as a fraction of the source, in (0, 1].
  double readback_scale = 1.0;
  // Worker threads (realtime) or simulated workers (simulated).
  std::size_t workers = 2;
  // Simulated cycle length; unset means the measured time of each cycle.
  std::optional<Duration> cycle = std::chrono::milliseconds{200};
  // Run the CPU compositor on every released frame (simulated mode only),
  // timing it as kStageCompose, and hand each composed real frame to
  // `on_composed` when set.
  bool compose = false;
  std::function<void(std::size_t index, const BgraImage& composed)> on_composed;
  CompositorConfig compositor;
};

// SchedulerConfig::from_policy(policy) with the driver's worker count and the
// mask limits taken from the compositor's rect capacity (kMaskRectCapacity).
[[nodiscard]] SchedulerConfig driver_config(const PolicyConfig& policy, const DriverOptions& options);

struct SequenceResult {
  // One plan per real frame, in frame order, with the frame's own timestamp
  // (frame_time(i, fps)) and rects fitted to kMaskRectCapacity.
  std::vector<MaskPlan> plans;
  std::size_t fail_closed = 0;
  std::size_t padding_frames = 0;
  // Realtime only: real frames pushed more than one frame period late
  // because decoding fell behind.
  std::size_t late_frames = 0;
  // Per detection cycle: its frame's timestamp to the cycle's end (simulated
  // or wall time).
  std::vector<double> latencies_ms;
  SchedulerStats stats;
};

// Throws CliError for bad options, and lets a frame-loading error through.
[[nodiscard]] SequenceResult run_simulated(const FrameSource& source, const PolicyConfig& policy,
                                           const ScanStages& stages, const DriverOptions& options,
                                           StageTimes& times);

// `stages` should be wrapped by timed() with `cycles`, whose cycle ends give
// the latencies. Throws CliError for bad options or a stuck release.
[[nodiscard]] SequenceResult run_realtime(const FrameSource& source, const PolicyConfig& policy,
                                          const ScanStages& stages, const DriverOptions& options,
                                          StageTimes& times, const CycleLog& cycles);

}  // namespace spg::cli
