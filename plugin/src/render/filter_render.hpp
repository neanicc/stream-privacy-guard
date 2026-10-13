#pragma once

// The filter's render side (SPG-14): asks for scheduler generations when the
// settings change, and draws each frame from the PipelineHost's plan with
// libobs graphics. Rules: docs/plugin-render.md.
//
// Threads: OBS runs a filter's `update` and `video_tick` on the graphics
// thread, the thread that also runs `video_render`; the watch-list buttons
// run on the UI thread and the models' hand-over on the model loader thread.
// None of them builds a scheduler (SPG-14d): publish(), tick() and the
// models' hand-over only decide that a generation is needed and post its
// build to this filter's build thread (GenerationBuilder, TaskThread), which
// constructs the scheduler and its stages and offers the ready generation;
// render() swaps it in at the start of a frame. Models are only ever loaded
// on the loader thread: a build takes them ready-made from the ModelHub.
// status() may run on any thread.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

#include <obs.h>

#include "pipeline/builder.hpp"
#include "pipeline/host.hpp"
#include "pipeline/pipeline.hpp"
#include "pipeline/ring_budget.hpp"
#include "render/obs_gpu.hpp"
#include "spg/policy_config.hpp"

namespace spg::plugin {

// Schedulers are destroyed by the reaper, which joins their workers; this is
// how long a destructor waits for a worker inside a stage before detaching it.
// Long enough that it never detaches in practice, so obs_module_unload (which
// joins the reaper) never returns while a worker still runs module code.
inline constexpr std::chrono::hours kWorkerJoinTimeout{1};

// A fixed English name for logs ("not scanned in time"); never text from the frame.
[[nodiscard]] const char* cover_name(Cover cover) noexcept;

class FilterRender {
 public:
  // Starts loading the module's detection models if nothing has yet, and
  // rebuilds the scheduler when they are ready.
  explicit FilterRender(obs_source_t* filter);
  // Stops listening for the models, waits for a build in progress (the
  // models are already loaded, so it is short), frees GPU resources; the
  // host then retires the schedulers to the reaper.
  ~FilterRender();
  FilterRender(const FilterRender&) = delete;
  FilterRender& operator=(const FilterRender&) = delete;

  // Asks for a scheduler generation for `policy` with the module's detection
  // models, with the lookahead shortened to fit `vram_budget_mb` at the
  // source's current size (ring_budget.hpp), built on the build thread. No
  // policy, or no models (still loading, missing or failed): no scheduler,
  // and every frame is covered whole with the fallback. Skips the rebuild
  // when nothing changed. Until the new generation is ready every frame is
  // covered with a solid fill (Cover::SettingsChanged); if it cannot be
  // built, until a later publish succeeds.
  void publish(const std::optional<PolicyConfig>& policy, FallbackMode fallback, long long vram_budget_mb) noexcept;
  // The filter's video_tick (graphics thread): after a resize or a frame rate
  // change, asks for a new generation of the same settings if the lookahead
  // that fits the budget changed; the running one stays until it is ready.
  // A rebuild that fails covers everything and is asked for again every
  // kRefitRetryNs (RefitSchedule). Never waits for a build or for a publish.
  void tick() noexcept;
  void set_debug_overlay(bool on) noexcept { overlay_.store(on, std::memory_order_relaxed); }

  // The filter's video_render.
  void render() noexcept;

  [[nodiscard]] RenderStatus status() const noexcept;

 private:
  using Kind = GenerationBuilder::Kind;

  void log_transition(const DrawPlan& plan) noexcept;
  // Caller holds publish_mutex_. Asks for a generation of `kind` for these
  // settings unless the one running or being built already matches. `policy`
  // is the caller's copy (never a reference to requested_). Throws if the
  // request cannot be prepared; a request that cannot be posted has failed
  // (covered) and logged by the time this returns.
  void request_locked(Kind kind, std::optional<PolicyConfig> policy, FallbackMode fallback,
                      std::uint64_t budget_bytes);
  // Caller holds publish_mutex_. A request of `kind` could not be prepared.
  void request_failed_locked(Kind kind) noexcept;
  // Caller holds publish_mutex_ (the build thread, from GenerationBuilder).
  void built_locked(GenerationBuilder::Outcome outcome, const LookaheadFit& fit, bool with_models,
                    std::uint64_t models) noexcept;
  // The model loader thread: rebuild the requested settings with the models,
  // then ask OBS to refresh a properties view open on this filter.
  void models_changed() noexcept;
  // The source size and frame interval the budget is fitted to, now.
  [[nodiscard]] Size source_size() const noexcept;

  obs_source_t* filter_;
  ObsFrameGpu gpu_;
  PipelineHost host_;

  mutable std::mutex publish_mutex_;  // publishers and the build thread; guards the fields below
  // What the latest request was for (its generation is running, being
  // built, or failed: builds_ knows which).
  std::optional<PolicyConfig> requested_;
  FallbackMode requested_fallback_ = FallbackMode::BlurSource;
  std::uint64_t requested_budget_ = kNoRingBudget;
  Duration requested_lookahead_{};  // after the fit
  std::uint64_t requested_models_ = 0;  // ModelHub generation
  bool have_requested_ = false;
  BudgetNotice budget_notice_;  // the fit of the generation offered last
  std::uint64_t built_models_ = 0;  // ModelHub generation of the generation offered last
  std::uint64_t models_listener_ = 0;
  GenerationBuilder builds_;

  // Graphics thread only (tick()).
  RefitSchedule refits_;

  std::atomic<bool> overlay_{false};
  // Graphics thread, render() only.
  Cover last_logged_cover_ = Cover::None;
  bool logged_once_ = false;
  std::uint64_t last_log_ns_ = 0;

  // Last: started after everything a build touches, stopped first.
  TaskThread build_thread_;
};

}  // namespace spg::plugin
