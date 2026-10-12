#include "spg/scheduler.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace spg {

// State shared with the worker threads. A worker keeps it alive through its
// own shared_ptr, so a detached worker never outlives what it touches.
struct Scheduler::Shared {
  Shared(const Clock& clock, SchedulerConfig config, ScanStages s)
      : lookahead(clock, std::move(config)), stages(std::move(s)), limits(lookahead.config().scan) {}

  mutable std::mutex mutex;
  std::condition_variable work;  // a job was queued, or stop
  std::condition_variable idle;  // a worker exited
  Lookahead lookahead;
  const ScanStages stages;
  const ScanLimits limits;
  bool stop = false;
  std::size_t running = 0;
  std::vector<bool> exited;
};

Scheduler::Scheduler(const Clock& clock, SchedulerConfig config, ScanStages stages) {
  const std::size_t workers = config.workers;
  shared_ = std::make_shared<Shared>(clock, std::move(config), std::move(stages));
  shared_->exited.assign(workers, false);
  shared_->running = workers;
  threads_.reserve(workers);
  try {
    for (std::size_t i = 0; i < workers; ++i) {
      threads_.emplace_back(&Scheduler::run_worker, shared_, i);
    }
  } catch (...) {
    {
      const std::lock_guard lock(shared_->mutex);
      shared_->stop = true;
      shared_->running -= workers - threads_.size();
    }
    shared_->work.notify_all();
    for (std::thread& t : threads_) {
      t.join();
    }
    throw;
  }
}

Scheduler::~Scheduler() {
  std::unique_lock lock(shared_->mutex);
  shared_->stop = true;
  shared_->work.notify_all();
  shared_->idle.wait_for(lock, shared_->lookahead.config().shutdown_timeout,
                         [this] { return shared_->running == 0; });
  const std::vector<bool> exited = shared_->exited;
  lock.unlock();
  for (std::size_t i = 0; i < threads_.size(); ++i) {
    if (exited[i]) {
      threads_[i].join();
    } else {
      // Still inside a stage call that ignores deadlines. It holds its own
      // reference to the shared state and exits once the call returns.
      threads_[i].detach();
    }
  }
}

bool Scheduler::wants_scan(Timestamp t) const {
  const std::lock_guard lock(shared_->mutex);
  return shared_->lookahead.wants_scan(t);
}

FrameId Scheduler::push(Timestamp t, Size source, const FrameView* readback) {
  std::shared_ptr<const OwnedFrame> copy;
  if (readback != nullptr && wants_scan(t)) {
    try {
      copy = OwnedFrame::copy(*readback);
    } catch (...) {
      copy = nullptr;  // no scan; the frame fails closed unless another cycle covers it
    }
  }
  bool queued = false;
  FrameId id = 0;
  {
    const std::lock_guard lock(shared_->mutex);
    const std::uint64_t before = shared_->lookahead.stats().scans_queued;
    id = shared_->lookahead.push(t, source, std::move(copy));
    queued = shared_->lookahead.stats().scans_queued != before;
  }
  if (queued) {
    shared_->work.notify_one();
  }
  return id;
}

void Scheduler::release_due(std::vector<ReleasedFrame>& out) {
  const std::lock_guard lock(shared_->mutex);
  shared_->lookahead.release_due(out);
}

void Scheduler::mark_discontinuity() {
  const std::lock_guard lock(shared_->mutex);
  shared_->lookahead.mark_discontinuity();
}

SchedulerStats Scheduler::stats() const {
  const std::lock_guard lock(shared_->mutex);
  return shared_->lookahead.stats();
}

void Scheduler::run_worker(std::shared_ptr<Shared> shared, std::size_t index) {
  std::unique_lock lock(shared->mutex);
  while (true) {
    shared->work.wait(lock, [&] { return shared->stop || shared->lookahead.queued() > 0; });
    if (shared->stop) {
      break;
    }
    std::optional<ScanJob> job = shared->lookahead.take_job();
    if (!job.has_value()) {
      continue;
    }
    lock.unlock();
    // Inference runs without the lock; run_scan never throws.
    const ScanResult result = run_scan(shared->stages, *job, shared->limits);
    lock.lock();
    if (shared->stop) {
      break;
    }
    try {
      shared->lookahead.complete(*job, result);
    } catch (...) {
      // complete() rejects only a job it does not know; the frames it would
      // have covered fail closed.
    }
  }
  shared->exited[index] = true;
  --shared->running;
  lock.unlock();
  shared->idle.notify_all();
}

}  // namespace spg
