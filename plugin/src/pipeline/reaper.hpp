#pragma once

// Where schedulers are destroyed (SPG-14), without OBS. A scheduler's
// destructor waits for its workers to leave their stage calls, so it must
// never run on the graphics thread (render, video tick) or the UI thread. The
// reaper destroys retired schedulers on its own thread, in order.
//
// Module unload: the plugin's workers run code from the plugin module
// (spg-core is linked into it), so obs_module_unload calls shutdown(), which
// destroys everything queued and joins the reaper thread. The plugin builds
// its schedulers with a shutdown timeout long enough that a destructor joins
// its workers rather than detaching them, so once shutdown() returns no
// scheduler thread is left running module code.

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "pipeline/pipeline.hpp"

namespace spg::plugin {

class Reaper {
 public:
  Reaper() = default;
  ~Reaper();  // shutdown()
  Reaper(const Reaper&) = delete;
  Reaper& operator=(const Reaper&) = delete;

  // Queues `scheduler` for destruction on the reaper thread (started on first
  // use). Queueing never allocates (the queue is linked through the
  // schedulers), so the render thread can retire one at any time without
  // waiting for its workers. If the thread cannot be started, the scheduler
  // stays queued: the next retire() tries again and shutdown() destroys it.
  // Only after shutdown() (module unload) is it destroyed on the caller.
  void retire(std::unique_ptr<FrameScheduler> scheduler) noexcept;

  // Destroys everything queued, then joins the thread. Idempotent.
  void shutdown() noexcept;

  // Schedulers queued and not yet destroyed (including one being destroyed).
  [[nodiscard]] std::size_t pending() const;

  // Test seam: runs where the thread is started, and may throw as starting it
  // can.
  void set_start_fault(std::function<void()> fault);

 private:
  void run();
  // Unlinks the oldest queued scheduler; mutex_ held, queue not empty.
  std::unique_ptr<FrameScheduler> pop() noexcept;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::unique_ptr<FrameScheduler> head_;  // oldest; each links the next
  FrameScheduler* tail_ = nullptr;
  std::size_t queued_ = 0;
  std::size_t destroying_ = 0;
  bool stop_ = false;
  std::thread thread_;
  std::function<void()> start_fault_;
};

// The plugin's reaper, shut down by obs_module_unload.
[[nodiscard]] Reaper& module_reaper() noexcept;

}  // namespace spg::plugin
