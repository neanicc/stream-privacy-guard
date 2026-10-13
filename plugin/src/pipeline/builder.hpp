#pragma once

// Builds scheduler generations off the thread that asks for them (SPG-14d),
// without OBS. In OBS a filter's `update` and `video_tick` run on the
// graphics thread, the thread that also draws every frame, so building a
// ThreadedScheduler there (worker threads, stages around the loaded models)
// would stall rendering. The caller only decides that a generation is
// needed; the build runs on an executor (TaskThread in the plugin, a manual
// queue in tests) and the ready generation is offered to the PipelineHost,
// which the render thread swaps in at the start of a frame.
//
// Fail closed while a build is pending or after it fails:
//   * A settings change covers every frame (solid, Cover::SettingsChanged)
//     from the moment it is requested until its generation is offered: the
//     generation still running scans with the old settings, which may lack a
//     watch-list entry or a category just added.
//   * A rebuild of the same settings (a VRAM-budget refit, the models
//     becoming ready) leaves the running generation in place until the new
//     one is offered: it runs the same policy, and Pipeline covers whatever
//     it cannot buffer.
//   * Only the latest request is ever offered. One superseded before it
//     starts is never built, and its copy of the settings is dropped at
//     once (requests wait in one slot, and at most one task is posted for
//     it); one superseded while building is retired.
//   * A failed build covers every frame (solid, Cover::NoDetection) until a
//     later request is offered. A rebuild that fails is worth retrying
//     (take_retry()) only when the current settings were built once; a
//     settings change that never produced a generation is not retried, and
//     covers until the next change.
// Schedulers are never destroyed here: superseded ones go to `retire`.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "pipeline/host.hpp"
#include "pipeline/pipeline.hpp"

namespace spg::plugin {

// One thread that runs posted tasks in order. The plugin gives each filter
// one, started when the filter is created, so asking for a build never starts
// a thread on the graphics thread.
class TaskThread {
 public:
  using Task = std::function<void()>;

  // Starts the thread; throws (std::system_error) if it cannot.
  TaskThread();
  ~TaskThread();  // stop()
  TaskThread(const TaskThread&) = delete;
  TaskThread& operator=(const TaskThread&) = delete;

  // Any thread. Queues `task`; throws on allocation failure or after stop().
  // A task must not throw (one that does is dropped).
  void post(Task task);
  // Drops the tasks not started, waits for the one running, joins. Never
  // from a task. Idempotent.
  void stop() noexcept;
  [[nodiscard]] std::thread::id id() const noexcept { return id_; }

 private:
  void run() noexcept;

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Task> tasks_;
  bool stop_ = false;
  std::thread thread_;
  std::thread::id id_;
};

class GenerationBuilder {
 public:
  using Task = std::function<void()>;
  // Runs `task` later on another thread; throws if it cannot (the request
  // then fails like a build).
  using Executor = std::function<void(Task)>;
  // Builds the generation, on the executor's thread, without the lock.
  using Build = std::function<std::unique_ptr<PipelineSpec>()>;

  enum class Kind : std::uint8_t {
    Settings,  // new settings: covers until ready; a failure is not retried
    Rebuild,   // the same settings again (refit, models): the old generation runs meanwhile
  };

  struct Outcome {
    bool ok = false;             // the generation was offered to the host
    bool first_failure = false;  // !ok, and not a retried rebuild failing again (log once)
  };
  // Runs with the lock held, once per request that is not superseded: on the
  // executor's thread after its build, or on request()'s caller when the
  // build could not be posted. Must not call request() or fail().
  using Done = std::function<void(Outcome)>;

  // `lock` is the caller's mutex: request(), fail() and the queries below
  // are called with it held, and the executor's thread holds it while it
  // hands a generation to `host` and calls Done, so a request can never
  // race with the hand-over of the one it supersedes.
  GenerationBuilder(PipelineHost& host, Pipeline::Retire retire, std::mutex& lock, Executor executor);
  GenerationBuilder(const GenerationBuilder&) = delete;
  GenerationBuilder& operator=(const GenerationBuilder&) = delete;

  // Lock held. Supersedes every earlier request (one not started yet is
  // dropped here) and queues `build`, posting a task to the executor unless
  // one is already queued. False if it could not be posted: then it failed
  // (covered) and `done` has run.
  bool request(Kind kind, Build build, Done done) noexcept;
  // Lock held. The caller could not even prepare a request of `kind`:
  // supersedes every earlier one and fails as a build would.
  Outcome fail(Kind kind) noexcept;

  // Lock held. A request is posted and has not ended.
  [[nodiscard]] bool pending() const noexcept { return pending_; }
  // Lock held. The latest request failed and nothing newer is pending.
  [[nodiscard]] bool failed() const noexcept { return failed_ && !pending_; }
  // Lock held. A Rebuild of the current settings may be requested: nothing
  // failed, or the failure was a rebuild of settings built once.
  [[nodiscard]] bool may_rebuild() const noexcept { return !failed_ || retry_; }
  // Any thread. True once after each failure worth retrying (FilterRender's
  // video tick then waits kRefitRetryNs before it asks again).
  [[nodiscard]] bool take_retry() noexcept { return retry_due_.exchange(false); }

 private:
  // The executor's thread: builds the request waiting in the slot, if any.
  void run() noexcept;
  // Lock held: covers, records the failure.
  Outcome failure() noexcept;
  // Lock held: empties the slot (the request in it is never built).
  void drop_next() noexcept;
  void retire(std::unique_ptr<PipelineSpec> spec) noexcept;

  PipelineHost* host_;
  Pipeline::Retire retire_;
  std::mutex* lock_;
  Executor executor_;

  // Guarded by *lock_.
  // The latest request not started yet; queued_ while a task is posted for it.
  Build next_build_;
  Done next_done_;
  bool queued_ = false;
  std::uint64_t latest_ = 0;
  bool pending_ = false;
  bool failed_ = false;
  bool retry_ = false;           // failed_ is a rebuild failure worth retrying
  bool settings_built_ = false;  // the current settings produced a generation
  std::atomic<bool> retry_due_{false};
};

}  // namespace spg::plugin
