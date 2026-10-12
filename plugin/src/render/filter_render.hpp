#pragma once

// The filter's render side (SPG-14): builds scheduler generations when the
// settings change, and draws each frame from the PipelineHost's plan with
// libobs graphics. Rules: docs/plugin-render.md.
//
// Threads: publish() runs wherever the settings change (the video thread for
// `update`, the UI thread for the watch-list buttons) and builds the new
// scheduler there; render() runs on the graphics thread; status() may run on
// any thread.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

#include <obs.h>

#include "pipeline/host.hpp"
#include "pipeline/pipeline.hpp"
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
  explicit FilterRender(obs_source_t* filter);
  // Frees GPU resources; the host then retires the schedulers to the reaper.
  ~FilterRender();
  FilterRender(const FilterRender&) = delete;
  FilterRender& operator=(const FilterRender&) = delete;

  // Hands the render thread a scheduler for `policy` (none: every frame is
  // covered whole with `fallback`). Skips the rebuild when nothing changed.
  // On any failure every frame is covered with a solid fill until a later
  // publish succeeds.
  void publish(const std::optional<PolicyConfig>& policy, FallbackMode fallback) noexcept;
  void set_debug_overlay(bool on) noexcept { overlay_.store(on, std::memory_order_relaxed); }

  // The filter's video_render.
  void render() noexcept;

  [[nodiscard]] RenderStatus status() const noexcept { return host_.status(); }

 private:
  void log_transition(const DrawPlan& plan) noexcept;

  obs_source_t* filter_;
  ObsFrameGpu gpu_;
  PipelineHost host_;

  std::mutex publish_mutex_;  // publishers; guards the fields below
  std::optional<PolicyConfig> published_;
  FallbackMode published_fallback_ = FallbackMode::BlurSource;
  bool have_published_ = false;
  bool publish_failed_ = false;

  std::atomic<bool> overlay_{false};
  // Render thread only.
  Cover last_logged_cover_ = Cover::None;
  bool logged_once_ = false;
  std::uint64_t last_log_ns_ = 0;
};

}  // namespace spg::plugin
