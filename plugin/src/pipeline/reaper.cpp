#include "pipeline/reaper.hpp"

#include <utility>

namespace spg::plugin {

Reaper::~Reaper() {
  shutdown();
}

void Reaper::retire(std::unique_ptr<FrameScheduler> scheduler) noexcept {
  if (scheduler == nullptr) {
    return;
  }
  try {
    std::unique_lock lock(mutex_);
    if (!stop_) {
      if (!thread_.joinable()) {
        thread_ = std::thread(&Reaper::run, this);
      }
      queue_.push_back(std::move(scheduler));
      lock.unlock();
      wake_.notify_one();
      return;
    }
  } catch (...) {
    // No thread or no room in the queue: destroyed below.
  }
  scheduler.reset();
}

void Reaper::shutdown() noexcept {
  std::thread thread;
  {
    const std::lock_guard lock(mutex_);
    stop_ = true;
    thread = std::move(thread_);
  }
  wake_.notify_all();
  if (thread.joinable()) {
    thread.join();
  }
  // Anything left (only if the thread never started) is destroyed here.
  std::deque<std::unique_ptr<FrameScheduler>> rest;
  {
    const std::lock_guard lock(mutex_);
    rest.swap(queue_);
  }
}

std::size_t Reaper::pending() const {
  const std::lock_guard lock(mutex_);
  return queue_.size() + destroying_;
}

void Reaper::run() {
  std::unique_lock lock(mutex_);
  while (true) {
    wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
    if (queue_.empty()) {
      return;  // stop_ and drained
    }
    std::unique_ptr<FrameScheduler> next = std::move(queue_.front());
    queue_.pop_front();
    ++destroying_;
    lock.unlock();
    next.reset();  // waits for that scheduler's workers
    lock.lock();
    --destroying_;
  }
}

Reaper& module_reaper() noexcept {
  static Reaper reaper;
  return reaper;
}

}  // namespace spg::plugin
