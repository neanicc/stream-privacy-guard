#include "render/filter_render.hpp"

#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include "model_loading.hpp"
#include "models/model_set.hpp"
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
            [](std::unique_ptr<FrameScheduler> scheduler) { module_reaper().retire(std::move(scheduler)); }) {
  // Last, so a throw above leaves nothing subscribed. Loading runs on the
  // hub's thread; until it ends this filter publishes no scheduler.
  models_listener_ = module_models().subscribe([this] { models_changed(); });
  module_models().start();
}

FilterRender::~FilterRender() {
  // No models_changed() runs on this object once this returns.
  module_models().unsubscribe(models_listener_);
  // host_ is destroyed after this body (members in reverse order), retiring
  // its schedulers; the GPU resources go first, inside the graphics context.
  gpu_.destroy();
}

void FilterRender::publish(const std::optional<PolicyConfig>& policy, FallbackMode fallback,
                           long long vram_budget_mb) noexcept {
  try {
    const std::lock_guard lock(publish_mutex_);
    refit_failed_ = false;  // new settings: a failure now is not retried with the old ones
    publish_locked(policy, fallback, static_cast<std::uint64_t>(clamp_vram_budget_mb(vram_budget_mb)) * kBytesPerMiB);
  } catch (...) {
    // No scheduler for these settings: cover everything until one is built.
    host_.fail();
    log(LOG_WARNING, "could not start detection for these settings: covering the whole source");
  }
}

void FilterRender::models_changed() noexcept {
  bool first_failure = false;
  try {
    const std::lock_guard lock(publish_mutex_);
    // As in tick(): after a failed settings change there is nothing to
    // rebuild (the old settings must not run again).
    if (have_published_ && (!publish_failed_ || refit_failed_)) {
      try {
        publish_locked(published_, published_fallback_, published_budget_);
      } catch (...) {
        first_failure = !refit_failed_;
        refit_failed_ = true;  // tick() retries it after a resize; a publish clears it
        throw;
      }
    }
  } catch (...) {
    // No scheduler with the models: cover everything until one is built.
    host_.fail();
    if (first_failure) {
      log(LOG_WARNING, "could not start detection for these settings: covering the whole source");
    }
  }
  // OBS does not rebuild a properties view on its own, so one opened while
  // the models loaded would keep saying "loading". This only emits the
  // source's update_properties signal; the frontend reloads the view on its
  // own (UI) thread.
  obs_source_update_properties(filter_);
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
  // Already loaded on the hub's thread: nothing here opens or warms up a model.
  const ModelHub::Snapshot models = module_models().snapshot();
  if (have_published_ && !publish_failed_ && published_ == policy && published_fallback_ == fallback &&
      published_budget_ == budget_bytes && published_lookahead_ == fit.lookahead &&
      published_models_ == models.generation) {
    // The same generation (a debug overlay toggle, an unrelated update, a
    // resize that fits the same lookahead, the same models): only the status
    // is refreshed.
    static_cast<void>(budget_notice_.update(fit));
    return;
  }
  // Until this publish succeeds, nothing may keep running the old policy.
  publish_failed_ = true;
  auto spec = std::make_unique<PipelineSpec>();
  spec->fallback = policy ? policy->fallback : fallback;
  spec->ring_budget_bytes = budget_bytes;
  // Without models (still loading, missing, damaged) no scheduler is built:
  // every frame is covered whole (Cover::NoDetection), nothing is scanned.
  if (policy && models.models != nullptr) {
    SchedulerConfig config = SchedulerConfig::from_policy(*policy);
    // Clamped again where consumed (settings are clamped when read), then
    // shortened to what the ring can hold within the VRAM budget.
    config.lookahead = fit.lookahead;
    config.retro = std::min<Duration>(config.retro, config.lookahead);
    config.tracker.hold = std::chrono::milliseconds(clamp_hold_ms(policy->hold.count()));
    config.shutdown_timeout = kWorkerJoinTimeout;
    spec->hold = config.tracker.hold;
    spec->scheduler =
        std::make_unique<ThreadedScheduler>(obs_clock(), std::move(config), make_stages(models.models, *policy));
  }
  const bool first_with_models = spec->scheduler != nullptr && published_models_ != models.generation;
  if (&policy != &published_) {
    published_ = policy;
  }
  published_fallback_ = fallback;
  published_budget_ = budget_bytes;
  published_lookahead_ = fit.lookahead;
  published_models_ = models.generation;
  have_published_ = true;
  host_.offer(std::move(spec));
  publish_failed_ = false;
  refit_failed_ = false;
  if (first_with_models) {
    // The source name is the user's label for the filter, not frame content.
    const char* name = obs_source_get_name(filter_);
    log(LOG_INFO, "'%s': scanning with the detection models", name != nullptr ? name : "");
  }
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
  const std::uint64_t now = os_gettime_ns();
  if (!refits_.due(source, interval, now)) {
    return;
  }
  bool first_failure = false;
  try {
    const std::unique_lock lock(publish_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      return;  // a publish is running and fits the budget itself; check next tick
    }
    // After a failed settings change there is nothing to refit (the old
    // settings must not run again); a failed refit is retried.
    if (have_published_ && (!publish_failed_ || refit_failed_)) {
      try {
        publish_locked(published_, published_fallback_, published_budget_);
      } catch (...) {
        first_failure = !refit_failed_;
        refit_failed_ = true;  // cleared by a publish that succeeds
        throw;
      }
    }
    refits_.done(source, interval);
  } catch (...) {
    // Everything is covered until a refit or a settings change succeeds.
    host_.fail();
    refits_.failed(now);
    if (first_failure) {
      log(LOG_WARNING, "could not start detection for these settings: covering the whole source");
    }
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
