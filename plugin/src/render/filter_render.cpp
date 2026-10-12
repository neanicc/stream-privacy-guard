#include "render/filter_render.hpp"

#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include "pipeline/reaper.hpp"
#include "plugin-support.hpp"
#include "settings/filter_settings.hpp"
#include "spg/lookahead.hpp"

namespace spg::plugin {

namespace {

// OBS frame timestamps (obs_get_video_frame_time) and os_gettime_ns share one
// time base, which the scheduler requires of its clock. Thread-safe.
class ObsClock final : public Clock {
 public:
  [[nodiscard]] Timestamp now() const noexcept override {
    return timestamp_from_ns(static_cast<std::int64_t>(os_gettime_ns()));
  }
};

const Clock& obs_clock() noexcept {
  static const ObsClock clock;
  return clock;
}

// One scheduler generation buffers at most max_buffered_frames; one more can
// still be draining after a settings change, plus the frame on screen and
// the last clean frame.
constexpr std::size_t kRingCapacity = 2 * SchedulerConfig{}.max_buffered_frames + 2;

// The detection stages for `policy`. Not loaded by this build yet (models and
// ONNX Runtime in the plugin are a follow-up to SPG-14): with no detector for
// an enabled category every cycle fails (ScanFailure::MissingStage), so every
// frame is released with the fallback. Never fail open.
ScanStages make_stages(const PolicyConfig& /*policy*/) {
  return ScanStages{};
}

constexpr std::uint64_t kLogInterval = 10'000'000'000ULL;  // ns

}  // namespace

const char* cover_name(Cover cover) noexcept {
  switch (cover) {
    case Cover::None:
      return "none";
    case Cover::NotScanned:
      return "not scanned in time";
    case Cover::LateRelease:
      return "released late";
    case Cover::BadTimestamp:
      return "bad frame timestamp";
    case Cover::ApplyFailed:
      return "result could not be applied";
    case Cover::Starting:
      return "starting";
    case Cover::NoDetection:
      return "detection off";
    case Cover::SettingsChanged:
      return "settings changed";
    case Cover::NoTexture:
      return "frame not kept";
  }
  return "unknown";
}

FilterRender::FilterRender(obs_source_t* filter) : filter_(filter), gpu_(filter, kRingCapacity) {
  pipeline_ = std::make_unique<Pipeline>(
      gpu_, kRingCapacity, [](std::unique_ptr<FrameScheduler> scheduler) { module_reaper().retire(std::move(scheduler)); });
}

FilterRender::~FilterRender() {
  pipeline_.reset();  // retires its schedulers
  {
    const std::lock_guard lock(handoff_mutex_);
    if (pending_ != nullptr) {
      module_reaper().retire(std::move(pending_->scheduler));
    }
  }
  gpu_.destroy();
}

void FilterRender::publish(const std::optional<PolicyConfig>& policy, FallbackMode fallback) noexcept {
  try {
    const std::lock_guard lock(publish_mutex_);
    if (have_published_ && !cover_all_.load() && published_ == policy && published_fallback_ == fallback) {
      return;  // the same policy (a debug overlay toggle, an unrelated update)
    }
    auto spec = std::make_unique<PipelineSpec>();
    spec->fallback = policy ? policy->fallback : fallback;
    if (policy) {
      SchedulerConfig config = SchedulerConfig::from_policy(*policy);
      // Clamped again where consumed (settings are clamped when read).
      config.lookahead = std::chrono::milliseconds(clamp_lookahead_ms(policy->lookahead.count()));
      config.retro = std::min<Duration>(config.retro, config.lookahead);
      config.tracker.hold = std::chrono::milliseconds(clamp_hold_ms(policy->hold.count()));
      config.shutdown_timeout = kWorkerJoinTimeout;
      spec->hold = config.tracker.hold;
      spec->scheduler = std::make_unique<ThreadedScheduler>(obs_clock(), std::move(config), make_stages(*policy));
    }
    published_ = policy;
    published_fallback_ = fallback;
    have_published_ = true;
    std::unique_ptr<PipelineSpec> replaced;
    {
      const std::lock_guard swap(handoff_mutex_);
      replaced = std::move(pending_);
      pending_ = std::move(spec);
      has_pending_.store(true, std::memory_order_release);
    }
    cover_all_.store(false);
    if (replaced != nullptr) {
      module_reaper().retire(std::move(replaced->scheduler));
    }
  } catch (...) {
    // No scheduler for these settings: cover everything until one is built.
    cover_all_.store(true);
    log(LOG_WARNING, "could not start detection for these settings: covering the whole source");
  }
}

void FilterRender::render() noexcept {
  obs_source_t* target = obs_filter_get_target(filter_);
  const Size source = target != nullptr
                          ? Size{obs_source_get_base_width(target), obs_source_get_base_height(target)}
                          : Size{};
  if (cover_all_.load()) {
    gpu_.draw_solid(source);
    return;
  }
  try {
    const Timestamp t = timestamp_from_ns(static_cast<std::int64_t>(obs_get_video_frame_time()));
    if (has_pending_.load(std::memory_order_acquire)) {
      std::unique_ptr<PipelineSpec> spec;
      {
        const std::lock_guard swap(handoff_mutex_);
        spec = std::move(pending_);
        has_pending_.store(false, std::memory_order_relaxed);
      }
      if (spec != nullptr) {
        scanning_.store(spec->scheduler != nullptr);
        pipeline_->adopt(std::move(*spec), t);
      }
    }
    const DrawPlan& plan = pipeline_->tick(t, source);
    gpu_.draw(plan, source, overlay_.load(std::memory_order_relaxed));
    const PipelineCounters& counters = pipeline_->counters();
    std::uint64_t covered = 0;
    for (std::size_t i = 1; i < kCoverCount; ++i) {
      covered += counters.shown[i];
    }
    clean_.store(counters.shown[0], std::memory_order_relaxed);
    covered_.store(covered, std::memory_order_relaxed);
    last_cover_.store(static_cast<std::uint8_t>(plan.cover), std::memory_order_relaxed);
    log_transition(plan);
  } catch (...) {
    // Only an allocation can throw here: cover this frame.
    gpu_.draw_solid(source);
  }
}

void FilterRender::log_transition(const DrawPlan& plan) noexcept {
  if (plan.kind == DrawKind::Nothing) {
    return;
  }
  // One line per change of reason, at most one every kLogInterval: a reason
  // that changed meanwhile is logged once the interval has passed.
  if (logged_once_ && plan.cover == last_logged_cover_) {
    return;
  }
  const std::uint64_t now = os_gettime_ns();
  if (logged_once_ && now - last_log_ns_ < kLogInterval) {
    return;
  }
  if (plan.cover != Cover::None) {
    log(LOG_INFO, "covering the whole source: %s", cover_name(plan.cover));
  } else {
    log(LOG_INFO, "frames air with detected regions masked");
  }
  logged_once_ = true;
  last_logged_cover_ = plan.cover;
  last_log_ns_ = now;
}

RenderStatus FilterRender::status() const noexcept {
  RenderStatus status;
  status.scanning = scanning_.load(std::memory_order_relaxed);
  status.clean = clean_.load(std::memory_order_relaxed);
  status.covered = covered_.load(std::memory_order_relaxed);
  status.last_cover = static_cast<Cover>(last_cover_.load(std::memory_order_relaxed));
  return status;
}

}  // namespace spg::plugin
