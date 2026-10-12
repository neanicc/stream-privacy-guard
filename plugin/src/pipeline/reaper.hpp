#pragma once

// Where schedulers are destroyed (SPG-14), without OBS. A scheduler's
// destructor waits for its workers to leave their stage calls, so it must
// never run on the render thread, the video thread or the UI thread. The
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
#include <deque>
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
  // use). After shutdown(), or when the thread or the queue cannot be grown,
  // destroys it on the calling thread instead.
  void retire(std::unique_ptr<FrameScheduler> scheduler) noexcept;

  // Destroys everything queued, then joins the thread. Idempotent.
  void shutdown() noexcept;

  // Schedulers queued and not yet destroyed (including one being destroyed).
  [[nodiscard]] std::size_t pending() const;

 private:
  void run();

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::unique_ptr<FrameScheduler>> queue_;
  std::size_t destroying_ = 0;
  bool stop_ = false;
  std::thread thread_;
};

// The plugin's reaper, shut down by obs_module_unload.
[[nodiscard]] Reaper& module_reaper() noexcept;

}  // namespace spg::plugin
