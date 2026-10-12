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

void retire_to_reaper(std::unique_ptr<FrameScheduler> scheduler) noexcept {
  module_reaper().retire(std::move(scheduler));
}

// log() formats into a stack buffer and hands it to blogva (C), so it cannot
// throw: a call in a catch handler or a noexcept function cannot terminate
// OBS. Only an argument can allocate (describe()), and that call site
// catches.
static_assert(noexcept(log(LOG_WARNING, "%s", "")));

void log_not_started() noexcept {
  log(LOG_WARNING, "could not start detection for these settings: covering the whole source");
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
      host_(gpu_, kRingCapacity, retire_to_reaper),
      builds_(host_, retire_to_reaper, publish_mutex_,
              [this](GenerationBuilder::Task task) { build_thread_.post(std::move(task)); }) {
  // Last, so a throw above leaves nothing subscribed. Loading runs on the
  // hub's thread; until it ends this filter builds no scheduler.
  models_listener_ = module_models().subscribe([this] { models_changed(); });
  module_models().start();
}

FilterRender::~FilterRender() {
  // No models_changed() runs on this object once this returns.
  module_models().unsubscribe(models_listener_);
  // No build runs once this returns: one in progress finishes (and hands its
  // generation to host_), the ones not started are dropped.
  build_thread_.stop();
  // host_ is destroyed after this body (members in reverse order), retiring
  // its schedulers; the GPU resources go first, inside the graphics context.
  gpu_.destroy();
}

void FilterRender::publish(const std::optional<PolicyConfig>& policy, FallbackMode fallback,
                           long long vram_budget_mb) noexcept {
  const std::uint64_t budget = static_cast<std::uint64_t>(clamp_vram_budget_mb(vram_budget_mb)) * kBytesPerMiB;
  try {
    const std::lock_guard lock(publish_mutex_);
    try {
      // New settings: covered until their generation is built, and a failure
      // is not retried with the old ones.
      static_cast<void>(request_locked(Kind::Settings, policy, fallback, budget));
    } catch (...) {
      request_failed_locked(Kind::Settings);
    }
  } catch (...) {
    // The lock failed: nothing can be asked for. Cover everything; a build
    // in progress cannot lift it, since handing over needs the same lock.
    host_.fail();
    log_not_started();
  }
}

void FilterRender::models_changed() noexcept {
  try {
    const std::lock_guard lock(publish_mutex_);
    // As in tick(): after a failed settings change there is nothing to
    // rebuild (the old settings must not run again).
    if (have_requested_ && builds_.may_rebuild()) {
      try {
        // A copy: request_locked() assigns requested_.
        std::optional<PolicyConfig> policy = requested_;
        static_cast<void>(request_locked(Kind::Rebuild, std::move(policy), requested_fallback_, requested_budget_));
      } catch (...) {
        request_failed_locked(Kind::Rebuild);
      }
    }
  } catch (...) {
    // The lock failed: no scheduler with the models. Cover everything.
    host_.fail();
    log_not_started();
  }
  // OBS does not rebuild a properties view on its own, so one opened while
  // the models loaded would keep saying "loading". This only emits the
  // source's update_properties signal; the frontend reloads the view on its
  // own (UI) thread.
  obs_source_update_properties(filter_);
}

bool FilterRender::request_locked(Kind kind, std::optional<PolicyConfig> policy, FallbackMode fallback,
                                  std::uint64_t budget_bytes) {
  Size source = source_size();
  if (source.width == 0 || source.height == 0) {
    source = budget_notice_.fit().frame;  // hidden for now: keep the last fit's size
  }
  const Duration requested =
      policy ? Duration{std::chrono::milliseconds(clamp_lookahead_ms(policy->lookahead.count()))} : Duration{};
  const LookaheadFit fit = fit_lookahead(requested, budget_bytes, source,
                                         texture_bytes(source, gpu_.bits_per_pixel()), video_frame_interval());
  // Already loaded on the hub's thread: nothing here or in the build opens or
  // warms up a model.
  ModelHub::Snapshot models = module_models().snapshot();
  if (have_requested_ && !builds_.failed() && requested_ == policy && requested_fallback_ == fallback &&
      requested_budget_ == budget_bytes && requested_lookahead_ == fit.lookahead &&
      requested_models_ == models.generation) {
    // The same generation is running or being built (a debug overlay toggle,
    // an unrelated update, a resize that fits the same lookahead, the same
    // models): only the status is refreshed, and only once nothing is
    // pending (a pending build records and logs its own fit).
    if (!builds_.pending()) {
      static_cast<void>(budget_notice_.update(fit));
    }
    return true;
  }
  const bool with_models = policy.has_value() && models.models != nullptr;
  // Everything the build needs, by value: it runs on the build thread
  // without publish_mutex_.
  GenerationBuilder::Build build = [policy, fallback, budget_bytes, lookahead = fit.lookahead,
                                    loaded = models.models]() {
    auto spec = std::make_unique<PipelineSpec>();
    spec->fallback = policy ? policy->fallback : fallback;
    spec->ring_budget_bytes = budget_bytes;
    // Without models (still loading, missing, damaged) no scheduler is
    // built: every frame is covered whole (Cover::NoDetection), nothing is
    // scanned.
    if (policy && loaded != nullptr) {
      SchedulerConfig config = SchedulerConfig::from_policy(*policy);
      // Clamped again where consumed (settings are clamped when read), then
      // shortened to what the ring can hold within the VRAM budget.
      config.lookahead = lookahead;
      config.retro = std::min<Duration>(config.retro, config.lookahead);
      config.tracker.hold = std::chrono::milliseconds(clamp_hold_ms(policy->hold.count()));
      config.shutdown_timeout = kWorkerJoinTimeout;
      spec->hold = config.tracker.hold;
      spec->scheduler =
          std::make_unique<ThreadedScheduler>(obs_clock(), std::move(config), make_stages(loaded, *policy));
    }
    return spec;
  };
  GenerationBuilder::Done done = [this, fit, with_models, generation = models.generation](
                                     GenerationBuilder::Outcome outcome) {
    built_locked(outcome, fit, with_models, generation);
  };
  // `policy` is this call's own copy, so requested_ is never assigned to
  // itself (tick() and models_changed() pass a copy of it, not a reference).
  requested_ = std::move(policy);
  requested_fallback_ = fallback;
  requested_budget_ = budget_bytes;
  requested_lookahead_ = fit.lookahead;
  requested_models_ = models.generation;
  have_requested_ = true;
  return builds_.request(kind, std::move(build), std::move(done));
}

void FilterRender::request_failed_locked(Kind kind) noexcept {
  // Whatever is pending is never offered: it was asked for with settings
  // these replace (or, for a rebuild, the same ones, which failed now).
  if (builds_.fail(kind).first_failure) {
    log_not_started();
  }
}

void FilterRender::built_locked(GenerationBuilder::Outcome outcome, const LookaheadFit& fit, bool with_models,
                                std::uint64_t models) noexcept {
  if (!outcome.ok) {
    // Everything is covered until a rebuild or a settings change succeeds.
    if (outcome.first_failure) {
      log_not_started();
    }
    return;
  }
  const bool first_with_models = with_models && built_models_ != models;
  built_models_ = models;
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
  if (builds_.take_retry()) {
    refits_.failed(now);  // a rebuild failed on the build thread: ask again in kRefitRetryNs
  }
  if (!refits_.due(source, interval, now)) {
    return;
  }
  // Never waits: a publish holding the lock fits the budget itself, and the
  // build thread holds it only to hand a generation over. Check next tick.
  const std::unique_lock lock(publish_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }
  bool asked = true;
  // After a failed settings change there is nothing to refit (the old
  // settings must not run again); a failed rebuild is retried.
  if (have_requested_ && builds_.may_rebuild()) {
    try {
      // A copy: request_locked() assigns requested_.
      std::optional<PolicyConfig> policy = requested_;
      asked = request_locked(Kind::Rebuild, std::move(policy), requested_fallback_, requested_budget_);
    } catch (...) {
      request_failed_locked(Kind::Rebuild);
      asked = false;
    }
  }
  if (asked) {
    refits_.done(source, interval);
  } else {
    refits_.failed(now);  // covered meanwhile; asked again in kRefitRetryNs
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
