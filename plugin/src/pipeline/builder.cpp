#include "pipeline/builder.hpp"

#include <stdexcept>
#include <utility>

namespace spg::plugin {

TaskThread::TaskThread() : thread_(&TaskThread::run, this) {
  id_ = thread_.get_id();
}

TaskThread::~TaskThread() {
  stop();
}

void TaskThread::post(Task task) {
  {
    const std::lock_guard lock(mutex_);
    if (stop_) {
      throw std::logic_error("task thread stopped");
    }
    tasks_.push_back(std::move(task));
  }
  wake_.notify_one();
}

void TaskThread::stop() noexcept {
  std::deque<Task> dropped;
  try {
    const std::lock_guard lock(mutex_);
    stop_ = true;
    dropped.swap(tasks_);
  } catch (...) {
    // The mutex failed: the thread cannot be told to stop, so it cannot be
    // joined either; leave it rather than wait forever.
    return;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    try {
      thread_.join();
    } catch (...) {
    }
  }
}

void TaskThread::run() noexcept {
  while (true) {
    Task task;
    try {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
      if (stop_) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    } catch (...) {
      return;  // the mutex failed: nothing more can be taken safely
    }
    try {
      task();
    } catch (...) {
      // A task must not throw; one that does is dropped.
    }
  }
}

GenerationBuilder::GenerationBuilder(PipelineHost& host, Pipeline::Retire retire, std::mutex& lock, Executor executor)
    : host_(&host), retire_(std::move(retire)), lock_(&lock), executor_(std::move(executor)) {}

bool GenerationBuilder::request(Kind kind, Build build, Done done) noexcept {
  const std::uint64_t ticket = ++latest_;
  pending_ = true;
  if (kind == Kind::Settings) {
    // Nothing may run the old settings from here on: covered until the
    // generation for these is offered.
    settings_built_ = false;
    failed_ = false;
    retry_ = false;
    host_->fail(Cover::SettingsChanged);
  }
  try {
    // `done` is copied, not moved, so it is still here if posting throws.
    executor_([this, ticket, job = std::move(build), done]() { run(ticket, job, done); });
    return true;
  } catch (...) {
    // Not posted (allocation, executor stopped): fails like a build.
  }
  const Outcome outcome = failure();
  if (done) {
    try {
      done(outcome);
    } catch (...) {
    }
  }
  return false;
}

GenerationBuilder::Outcome GenerationBuilder::fail(Kind kind) noexcept {
  ++latest_;  // whatever is pending is never offered
  if (kind == Kind::Settings) {
    settings_built_ = false;
    failed_ = false;
    retry_ = false;
  }
  return failure();
}

GenerationBuilder::Outcome GenerationBuilder::failure() noexcept {
  const bool first = !(failed_ && retry_);
  failed_ = true;
  retry_ = settings_built_;
  pending_ = false;
  host_->fail();
  if (retry_) {
    retry_due_.store(true);
  }
  return Outcome{false, first};
}

void GenerationBuilder::retire(std::unique_ptr<PipelineSpec> spec) noexcept {
  if (spec != nullptr && spec->scheduler != nullptr) {
    try {
      retire_(std::move(spec->scheduler));
    } catch (...) {
    }
  }
}

void GenerationBuilder::run(std::uint64_t ticket, const Build& build, const Done& done) noexcept {
  try {
    const std::lock_guard lock(*lock_);
    if (ticket != latest_) {
      return;  // superseded before it started: nothing to build
    }
  } catch (...) {
    // Cannot tell: build it; the hand-over below checks again.
  }
  // The expensive part, with no lock held: scheduler threads, stages.
  std::unique_ptr<PipelineSpec> spec;
  try {
    spec = build();
  } catch (...) {
    spec.reset();
  }
  try {
    const std::lock_guard lock(*lock_);
    if (ticket != latest_) {
      // Superseded while building: never offered.
      retire(std::move(spec));
      return;
    }
    Outcome outcome;
    if (spec != nullptr) {
      host_->offer(std::move(spec));
      pending_ = false;
      failed_ = false;
      retry_ = false;
      settings_built_ = true;
      outcome.ok = true;
    } else {
      outcome = failure();
    }
    if (done) {
      try {
        done(outcome);
      } catch (...) {
      }
    }
  } catch (...) {
    // The lock failed: nothing can be handed over safely. Cover, and drop
    // what was built.
    host_->fail();
    retire(std::move(spec));
  }
}

}  // namespace spg::plugin
