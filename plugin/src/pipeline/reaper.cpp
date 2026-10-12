#include "pipeline/reaper.hpp"

#include <utility>

namespace spg::plugin {

Reaper::~Reaper() {
  shutdown();
}

void Reaper::set_start_fault(std::function<void()> fault) {
  const std::lock_guard lock(mutex_);
  start_fault_ = std::move(fault);
}

void Reaper::retire(std::unique_ptr<FrameScheduler> scheduler) noexcept {
  if (scheduler == nullptr) {
    return;
  }
  try {
    std::unique_lock lock(mutex_);
    if (!stop_) {
      // Linking allocates nothing, so this cannot fail and leave the
      // scheduler to be destroyed here.
      FrameScheduler* const raw = scheduler.get();
      if (tail_ == nullptr) {
        head_ = std::move(scheduler);
      } else {
        tail_->reaper_next_ = std::move(scheduler);
      }
      tail_ = raw;
      ++queued_;
      if (!thread_.joinable()) {
        try {
          if (start_fault_) {
            start_fault_();
          }
          thread_ = std::thread(&Reaper::run, this);
        } catch (...) {
          // Stays queued: the next retire() starts the thread, or shutdown()
          // destroys it.
        }
      }
      lock.unlock();
      wake_.notify_one();
      return;
    }
  } catch (...) {
    // The mutex failed: destroying it here is the only way left to stop its
    // workers before the module unloads.
  }
  scheduler.reset();
}

std::unique_ptr<FrameScheduler> Reaper::pop() noexcept {
  std::unique_ptr<FrameScheduler> out = std::move(head_);
  head_ = std::move(out->reaper_next_);
  if (head_ == nullptr) {
    tail_ = nullptr;
  }
  --queued_;
  return out;
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
  // Anything left (only if the thread never started) is destroyed here, one
  // at a time.
  while (true) {
    std::unique_ptr<FrameScheduler> next;
    {
      const std::lock_guard lock(mutex_);
      if (head_ == nullptr) {
        break;
      }
      next = pop();
      ++destroying_;
    }
    next.reset();
    const std::lock_guard lock(mutex_);
    --destroying_;
  }
}

std::size_t Reaper::pending() const {
  const std::lock_guard lock(mutex_);
  return queued_ + destroying_;
}

void Reaper::run() {
  std::unique_lock lock(mutex_);
  while (true) {
    wake_.wait(lock, [this] { return stop_ || head_ != nullptr; });
    if (head_ == nullptr) {
      return;  // stop_ and drained
    }
    std::unique_ptr<FrameScheduler> next = pop();
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
