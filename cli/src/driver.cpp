#include "driver.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "image_io.hpp"
#include "inputs.hpp"
#include "spg/scheduler.hpp"

namespace spg::cli {

namespace {

using steady = std::chrono::steady_clock;

void check(const FrameSource& source, const DriverOptions& options) {
  if (source.count == 0 || !source.load) {
    throw CliError("no frames to run");
  }
  if (source.fps == 0) {
    throw CliError("fps must be at least 1");
  }
  if (!(options.readback_scale > 0.0 && options.readback_scale <= 1.0)) {
    throw CliError("readback scale must be in (0, 1]");
  }
  if (options.workers == 0) {
    throw CliError("workers must be at least 1");
  }
  if (options.cycle && *options.cycle < Duration::zero()) {
    throw CliError("cycle time must not be negative");
  }
}

// Ticks past the last frame before giving up on releasing every frame: the
// release rule needs at most lookahead + release grace, so this only trips
// on a bug.
std::size_t tick_limit(const FrameSource& source, const SchedulerConfig& config) {
  const double slack_s = std::chrono::duration<double>(config.lookahead + config.release_grace).count() + 5.0;
  return source.count + static_cast<std::size_t>(std::ceil(slack_s * source.fps));
}

BgraImage readback_of(const BgraImage& frame, const DriverOptions& options, StageTimes& times) {
  if (options.readback_scale >= 1.0) {
    return frame;
  }
  const auto start = steady::now();
  BgraImage out = resize_area(frame, readback_size(frame.size(), options.readback_scale));
  times.add("readback", elapsed_ms(start, steady::now()));
  return out;
}

// Collects released frames into the result.
class Collector {
 public:
  Collector(const FrameSource& source, SequenceResult& result) : source_(source), result_(result) {
    result_.plans.resize(source.count);
  }
  void expect(FrameId id, std::size_t index) { index_of_.emplace(id, index); }
  // The real frame index of a released frame, or nothing for padding.
  [[nodiscard]] std::optional<std::size_t> take(const ReleasedFrame& released) {
    const auto it = index_of_.find(released.id);
    if (it == index_of_.end()) {
      return std::nullopt;
    }
    const std::size_t index = it->second;
    index_of_.erase(it);
    MaskPlan& plan = result_.plans[index];
    plan.frame_time = frame_time(index, source_.fps);
    plan.fallback = released.plan.fallback;
    plan.rects = fit_mask_rects(released.plan.rects);
    result_.fail_closed += plan.fallback ? 1 : 0;
    ++released_;
    return index;
  }
  [[nodiscard]] bool finished() const noexcept { return released_ == source_.count; }

 private:
  const FrameSource& source_;
  SequenceResult& result_;
  std::unordered_map<FrameId, std::size_t> index_of_;
  std::size_t released_ = 0;
};

// The compositor stage for simulated runs: re-loads each released real frame
// and draws its plan; keeps the last clean frame for HoldLastScanned.
class Composer {
 public:
  Composer(const FrameSource& source, const DriverOptions& options, StageTimes& times)
      : source_(source), options_(options), times_(times) {}

  // `plan` is the released frame's fitted plan, as written out.
  void release(const ReleasedFrame& released, std::optional<std::size_t> index, const MaskPlan* plan) {
    // Padding frames are never shown.
    if (!options_.compose || !index || plan == nullptr) {
      return;
    }
    BgraImage image;
    if (released.action == FallbackAction::HoldLastScanned && has_last_clean_ && last_clean_id_ == released.reemit) {
      image = last_clean_;
    } else {
      image = source_.load(*index);
      const auto start = steady::now();
      compose(image, *plan, released.action, options_.compositor);
      times_.add(kStageCompose, elapsed_ms(start, steady::now()));
    }
    if (released.action == FallbackAction::None) {
      last_clean_id_ = released.id;
      last_clean_ = image;
      has_last_clean_ = true;
    }
    if (options_.on_composed) {
      options_.on_composed(*index, image);
    }
  }

 private:
  const FrameSource& source_;
  const DriverOptions& options_;
  StageTimes& times_;
  // Plain members rather than std::optional<std::pair<FrameId, BgraImage>>:
  // GCC 13 reports a false -Wmaybe-uninitialized on that optional's emplace.
  bool has_last_clean_ = false;
  FrameId last_clean_id_{};
  BgraImage last_clean_;
};

struct SimWorker {
  bool busy = false;
  Timestamp done{};
  ScanJob job;
  ScanResult result;
};

}  // namespace

SchedulerConfig driver_config(const PolicyConfig& policy, const DriverOptions& options) {
  SchedulerConfig config = SchedulerConfig::from_policy(policy);
  config.workers = options.workers;
  // Every plan is fitted to the compositor's rect capacity; the scheduler's
  // own per-frame and per-track bounds must not exceed it.
  config.max_retro_rects = std::min(config.max_retro_rects, kMaskRectCapacity);
  config.tracker.max_rects_per_track = std::min(config.tracker.max_rects_per_track, kMaskRectCapacity);
  return config;
}

SequenceResult run_simulated(const FrameSource& source, const PolicyConfig& policy, const ScanStages& stages,
                             const DriverOptions& options, StageTimes& times) {
  check(source, options);
  const SchedulerConfig config = driver_config(policy, options);
  ManualClock clock(frame_time(0, source.fps));
  Lookahead lookahead(clock, config);
  std::vector<SimWorker> workers(config.workers);
  SequenceResult result;
  Collector collector(source, result);
  Composer composer(source, options, times);

  const auto start_jobs = [&](Timestamp now) {
    for (SimWorker& w : workers) {
      if (w.busy) {
        continue;
      }
      std::optional<ScanJob> job = lookahead.take_job();
      if (!job) {
        return;
      }
      const auto started = steady::now();
      w.result = run_scan(stages, *job, config.scan);
      const auto took = steady::now() - started;
      times.add(kStageCycle, std::chrono::duration<double, std::milli>(took).count());
      w.done = now + (options.cycle ? *options.cycle : std::chrono::duration_cast<Duration>(took));
      w.job = std::move(*job);
      w.busy = true;
    }
  };
  // Applies every cycle that ends by `until`, in order of its end; each
  // freed worker takes the newest queued scan at that moment.
  const auto advance_to = [&](Timestamp until) {
    while (true) {
      SimWorker* next = nullptr;
      for (SimWorker& w : workers) {
        if (w.busy && w.done <= until && (next == nullptr || w.done < next->done)) {
          next = &w;
        }
      }
      if (next == nullptr) {
        return;
      }
      if (clock.now() < next->done) {
        clock.set(next->done);
      }
      lookahead.complete(next->job, next->result);
      result.latencies_ms.push_back(to_ms(next->done - next->job.frame_time));
      next->busy = false;
      next->job = ScanJob{};
      next->result = ScanResult{};
      start_jobs(clock.now());
    }
  };

  const std::size_t limit = tick_limit(source, config);
  std::shared_ptr<const OwnedFrame> last_readback;
  Size last_size;
  std::vector<ReleasedFrame> released;
  for (std::size_t k = 0; !collector.finished(); ++k) {
    if (k > limit) {
      throw CliError("frames were not released in time (internal error)");
    }
    const Timestamp t = frame_time(k, source.fps);
    advance_to(t);
    clock.set(t);
    std::shared_ptr<const OwnedFrame> readback;
    Size size;
    if (k < source.count) {
      const BgraImage frame = source.load(k);
      size = frame.size();
      last_size = size;
      // Kept for padding, which repeats the last frame's pixels.
      last_readback = OwnedFrame::copy(readback_of(frame, options, times).view(t));
      if (lookahead.wants_scan(t)) {
        readback = last_readback;
      }
    } else {
      size = last_size;
      ++result.padding_frames;
      if (lookahead.wants_scan(t)) {
        FrameView view = last_readback->view();
        view.timestamp = t;
        readback = OwnedFrame::copy(view);
      }
    }
    const FrameId id = lookahead.push(t, size, std::move(readback));
    if (k < source.count) {
      collector.expect(id, k);
    }
    start_jobs(t);
    advance_to(t);
    released.clear();
    lookahead.release_due(released);
    for (const ReleasedFrame& r : released) {
      const std::optional<std::size_t> index = collector.take(r);
      composer.release(r, index, index ? &result.plans[*index] : nullptr);
    }
  }
  result.stats = lookahead.stats();
  return result;
}

namespace {

struct Prefetched {
  BgraImage readback;
  Size source;
};

// Decodes frames ahead of the realtime feed on its own thread, at most
// `capacity` ahead, so PNG decoding does not set the feed's pace.
class Prefetcher {
 public:
  Prefetcher(const FrameSource& source, const DriverOptions& options, StageTimes& times, std::size_t capacity)
      : source_(source), options_(options), times_(times), capacity_(capacity) {
    thread_ = std::thread([this] { work(); });
  }
  Prefetcher(const Prefetcher&) = delete;
  Prefetcher& operator=(const Prefetcher&) = delete;
  ~Prefetcher() {
    {
      const std::lock_guard lock(mutex_);
      stop_ = true;
    }
    changed_.notify_all();
    thread_.join();
  }

  // Waits until the queue is full or every frame is decoded.
  void wait_ready() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return queue_.size() >= capacity_ || finished_; });
  }

  Prefetched pop() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return !queue_.empty() || finished_; });
    if (queue_.empty()) {
      if (error_) {
        std::rethrow_exception(error_);
      }
      throw CliError("frame feed ended early (internal error)");
    }
    Prefetched item = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    changed_.notify_all();
    return item;
  }

 private:
  void work() {
    try {
      for (std::size_t i = 0; i < source_.count; ++i) {
        {
          std::unique_lock lock(mutex_);
          changed_.wait(lock, [this] { return queue_.size() < capacity_ || stop_; });
          if (stop_) {
            break;
          }
        }
        const BgraImage frame = source_.load(i);
        Prefetched item{readback_of(frame, options_, times_), frame.size()};
        {
          const std::lock_guard lock(mutex_);
          queue_.push_back(std::move(item));
        }
        changed_.notify_all();
      }
    } catch (...) {
      const std::lock_guard lock(mutex_);
      error_ = std::current_exception();
    }
    {
      const std::lock_guard lock(mutex_);
      finished_ = true;
    }
    changed_.notify_all();
  }

  const FrameSource& source_;
  const DriverOptions& options_;
  StageTimes& times_;
  const std::size_t capacity_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Prefetched> queue_;
  std::exception_ptr error_;
  bool finished_ = false;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace

SequenceResult run_realtime(const FrameSource& source, const PolicyConfig& policy, const ScanStages& stages,
                            const DriverOptions& options, StageTimes& times, const CycleLog& cycles) {
  check(source, options);
  if (options.compose) {
    throw CliError("the realtime feed does not run the CPU compositor");
  }
  const SchedulerConfig config = driver_config(policy, options);
  SteadyClock clock;
  SequenceResult result;
  Collector collector(source, result);
  const auto period = std::chrono::nanoseconds(1'000'000'000LL / static_cast<std::int64_t>(source.fps));
  const std::size_t limit = tick_limit(source, config);
  {
    Prefetcher prefetch(source, options, times, 16);
    prefetch.wait_ready();
    Scheduler scheduler(clock, config, stages);
    const steady::time_point wall_start = steady::now() + std::chrono::milliseconds{5};
    Prefetched last;
    Timestamp previous{};
    std::vector<ReleasedFrame> released;
    for (std::size_t k = 0; !collector.finished(); ++k) {
      if (k > limit) {
        throw CliError("frames were not released in time (internal error)");
      }
      const steady::time_point due = wall_start + period * static_cast<std::int64_t>(k);
      std::this_thread::sleep_until(due);
      if (k < source.count) {
        last = prefetch.pop();
        if (steady::now() - due > period) {
          ++result.late_frames;
        }
      } else {
        ++result.padding_frames;
      }
      Timestamp t = clock.now();
      if (t <= previous) {
        t = previous + Duration{1};
      }
      previous = t;
      const FrameView view = last.readback.view(t);
      const FrameId id =
          scheduler.wants_scan(t) ? scheduler.push(t, last.source, &view) : scheduler.push(t, last.source);
      if (k < source.count) {
        collector.expect(id, k);
      }
      released.clear();
      scheduler.release_due(released);
      for (const ReleasedFrame& r : released) {
        (void)collector.take(r);
      }
    }
    result.stats = scheduler.stats();
  }
  result.latencies_ms = cycles.latencies_ms();
  return result;
}

}  // namespace spg::cli
