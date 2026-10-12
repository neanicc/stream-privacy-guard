#pragma once

// Scheduler (SPG-10): the Lookahead state machine plus a pool of worker
// threads that run detection cycles. The render-facing calls (wants_scan,
// push, release_due, mark_discontinuity, stats) never wait on inference:
// workers hold the lock only to take a job and to apply a result, never while
// a stage runs. Rules and rationale: docs/scheduler.md.

#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "spg/frame.hpp"
#include "spg/geometry.hpp"
#include "spg/lookahead.hpp"
#include "spg/scan.hpp"
#include "spg/time.hpp"

namespace spg {

class Scheduler {
 public:
  // Starts config.workers threads. `clock` must outlive the scheduler, must
  // be safe to read from several threads (SteadyClock is; ManualClock is
  // not), and must share the frames' FrameClock time base. Throws
  // ContractError when validate(config) fails.
  Scheduler(const Clock& clock, SchedulerConfig config, ScanStages stages);

  // Stops the workers. Waits up to config.shutdown_timeout for workers inside
  // a stage call, then detaches them: a detached worker owns the stages and
  // its job through shared pointers and exits without touching the scheduler
  // or the clock once its stage call returns.
  ~Scheduler();

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  // True when a frame at `t` would be queued for scanning, so the caller can
  // skip reading back the frame otherwise.
  [[nodiscard]] bool wants_scan(Timestamp t) const;

  // Buffers a frame (see Lookahead::push). When `readback` is given and the
  // cadence allows, copies it (outside the lock) and queues a scan for the
  // buffered frame whose timestamp equals readback->timestamp (this frame,
  // or an earlier one when the readback lags); an invalid readback, a failed
  // copy or a readback of no buffered frame just skips the scan. Throws
  // ContractError for a zero source size.
  FrameId push(Timestamp t, Size source, const FrameView* readback = nullptr);

  // Appends every frame due now, oldest first (see Lookahead::release_due).
  void release_due(std::vector<ReleasedFrame>& out);

  void mark_discontinuity();

  [[nodiscard]] SchedulerStats stats() const;

 private:
  struct Shared;
  static void run_worker(std::shared_ptr<Shared> shared, std::size_t index);

  std::shared_ptr<Shared> shared_;
  std::vector<std::thread> threads_;
};

}  // namespace spg
