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

// One output frame at the canvas frame rate; zero while video is not set up
// (the budget fit then leaves the lookahead as configured).
Duration video_frame_interval() noexcept {
  obs_video_info ovi{};
  if (!obs_get_video_info(&ovi) || ovi.fps_num == 0 || ovi.fps_den == 0) {
    return {};
  }
  return Duration{1'000'000'000LL * ovi.fps_den / ovi.fps_num};
}

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

void FilterRender::publish(const std::optional<PolicyConfig>& policy, FallbackMode fallback,
                           long long vram_budget_mb) noexcept {
  try {
    const std::lock_guard lock(publish_mutex_);
    publish_locked(policy, fallback, static_cast<std::uint64_t>(clamp_vram_budget_mb(vram_budget_mb)) * kBytesPerMiB);
  } catch (...) {
    // No scheduler for these settings: cover everything until one is built.
    host_.fail();
    log(LOG_WARNING, "could not start detection for these settings: covering the whole source");
  }
}

void FilterRender::publish_locked(const std::optional<PolicyConfig>& policy, FallbackMode fallback,
                                  std::uint64_t budget_bytes) {
  Size source = source_size();
  if (source.width == 0 || source.height == 0) {
    source = budget_notice_.fit().frame;  // hidden for now: keep the last fit's size
  }
  const Duration requested =
      policy ? Duration{std::chrono::milliseconds(clamp_lookahead_ms(policy->lookahead.count()))} : Duration{};
  const LookaheadFit fit = fit_lookahead(requested, budget_bytes, source,
                                         texture_bytes(source, gpu_.bits_per_pixel()), video_frame_interval());
  if (have_published_ && !publish_failed_ && published_ == policy && published_fallback_ == fallback &&
      published_budget_ == budget_bytes && published_lookahead_ == fit.lookahead) {
    // The same generation (a debug overlay toggle, an unrelated update, a
    // resize that fits the same lookahead): only the status is refreshed.
    static_cast<void>(budget_notice_.update(fit));
    return;
  }
  // Until this publish succeeds, nothing may keep running the old policy.
  publish_failed_ = true;
  auto spec = std::make_unique<PipelineSpec>();
  spec->fallback = policy ? policy->fallback : fallback;
  spec->ring_budget_bytes = budget_bytes;
  if (policy) {
    SchedulerConfig config = SchedulerConfig::from_policy(*policy);
    // Clamped again where consumed (settings are clamped when read), then
    // shortened to what the ring can hold within the VRAM budget.
    config.lookahead = fit.lookahead;
    config.retro = std::min<Duration>(config.retro, config.lookahead);
    config.tracker.hold = std::chrono::milliseconds(clamp_hold_ms(policy->hold.count()));
    config.shutdown_timeout = kWorkerJoinTimeout;
    spec->hold = config.tracker.hold;
    spec->scheduler = std::make_unique<ThreadedScheduler>(obs_clock(), std::move(config), make_stages(*policy));
  }
  if (&policy != &published_) {
    published_ = policy;
  }
  published_fallback_ = fallback;
  published_budget_ = budget_bytes;
  published_lookahead_ = fit.lookahead;
  have_published_ = true;
  host_.offer(std::move(spec));
  publish_failed_ = false;
  if (budget_notice_.update(fit)) {
    // One line per change: shortened, shortened to another value, restored.
    try {
      log(fit.shortened ? LOG_WARNING : LOG_INFO, "%s", describe(fit).c_str());
    } catch (...) {
      // Allocation failure: the generation is already handed over; the
      // properties still show the warning.
    }
  }
}

void FilterRender::tick() noexcept {
  const Size source = source_size();
  const Duration interval = video_frame_interval();
  if (source.width == 0 || source.height == 0 || (source == ticked_source_ && interval == ticked_interval_)) {
    return;  // nothing new to fit (a hidden source keeps its fit)
  }
  try {
    const std::unique_lock lock(publish_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      return;  // a publish is running and fits the budget itself; check next tick
    }
    ticked_source_ = source;
    ticked_interval_ = interval;
    if (have_published_ && !publish_failed_) {
      publish_locked(published_, published_fallback_, published_budget_);
    }
  } catch (...) {
    host_.fail();
    log(LOG_WARNING, "could not start detection for these settings: covering the whole source");
  }
}

RenderStatus FilterRender::status() const noexcept {
  RenderStatus status = host_.status();
  try {
    const std::lock_guard lock(publish_mutex_);
    status.fit = budget_notice_.fit();
  } catch (...) {
    // The lock failed: the status shows no budget warning this time.
  }
  return status;
}

Size FilterRender::source_size() const noexcept {
  obs_source_t* target = obs_filter_get_target(filter_);
  return target != nullptr ? Size{obs_source_get_base_width(target), obs_source_get_base_height(target)} : Size{};
}

void FilterRender::render() noexcept {
  const Size source = source_size();
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
