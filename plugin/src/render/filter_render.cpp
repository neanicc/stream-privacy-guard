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
    case Cover::Stalled:
      return "nothing released in time";
    case Cover::Inconsistent:
      return "inconsistent state";
  }
  return "unknown";
}

FilterRender::FilterRender(obs_source_t* filter)
    : filter_(filter),
      gpu_(filter, kRingCapacity),
      host_(gpu_, kRingCapacity,
            [](std::unique_ptr<FrameScheduler> scheduler) { module_reaper().retire(std::move(scheduler)); }) {}

FilterRender::~FilterRender() {
  // host_ is destroyed after this body (members in reverse order), retiring
  // its schedulers; the GPU resources go first, inside the graphics context.
  gpu_.destroy();
}

void FilterRender::publish(const std::optional<PolicyConfig>& policy, FallbackMode fallback) noexcept {
  try {
    const std::lock_guard lock(publish_mutex_);
    if (have_published_ && !publish_failed_ && published_ == policy && published_fallback_ == fallback) {
      return;  // the same policy (a debug overlay toggle, an unrelated update)
    }
    // Until this publish succeeds, nothing may keep running the old policy.
    publish_failed_ = true;
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
    host_.offer(std::move(spec));
    publish_failed_ = false;
  } catch (...) {
    // No scheduler for these settings: cover everything until one is built.
    host_.fail();
    log(LOG_WARNING, "could not start detection for these settings: covering the whole source");
  }
}

void FilterRender::render() noexcept {
  obs_source_t* target = obs_filter_get_target(filter_);
  const Size source = target != nullptr
                          ? Size{obs_source_get_base_width(target), obs_source_get_base_height(target)}
                          : Size{};
  // One plan per frame time, from one state; Solid when anything failed.
  const Timestamp t = timestamp_from_ns(static_cast<std::int64_t>(obs_get_video_frame_time()));
  const DrawPlan& plan = host_.frame(t, source);
  gpu_.draw(plan, source, overlay_.load(std::memory_order_relaxed));
  log_transition(plan);
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

}  // namespace spg::plugin
