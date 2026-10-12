#pragma once

// The filter's render side (SPG-14): owns the Pipeline and its GPU resources,
// takes new scheduler generations from the settings side, and draws each
// frame. Rules: docs/plugin-render.md.
//
// Threads: publish() runs wherever the settings change (the video thread for
// `update`, the UI thread for the watch-list buttons) and builds the new
// scheduler there; render() runs on the graphics thread and only swaps a
// pointer under a lock that publish() holds for that swap alone. status() may
// run on any thread.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include <obs.h>

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

struct RenderStatus {
  bool scanning = false;  // a scheduler is running
  std::uint64_t clean = 0;    // frames drawn with only their masks
  std::uint64_t covered = 0;  // frames drawn covered whole
  Cover last_cover = Cover::None;
};

class FilterRender {
 public:
  explicit FilterRender(obs_source_t* filter);
  // Retires the schedulers to the reaper and frees GPU resources.
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

  [[nodiscard]] RenderStatus status() const noexcept;

 private:
  void log_transition(const DrawPlan& plan) noexcept;

  obs_source_t* filter_;
  ObsFrameGpu gpu_;
  std::unique_ptr<Pipeline> pipeline_;  // render thread only

  std::mutex publish_mutex_;  // publishers; also guards published_
  std::optional<PolicyConfig> published_;
  FallbackMode published_fallback_ = FallbackMode::BlurSource;
  bool have_published_ = false;

  std::mutex handoff_mutex_;  // held only to swap `pending_`
  std::unique_ptr<PipelineSpec> pending_;
  std::atomic<bool> has_pending_{false};
  std::atomic<bool> cover_all_{false};
  std::atomic<bool> overlay_{false};

  std::atomic<bool> scanning_{false};
  std::atomic<std::uint64_t> clean_{0};
  std::atomic<std::uint64_t> covered_{0};
  std::atomic<std::uint8_t> last_cover_{0};
  Cover last_logged_cover_ = Cover::None;
  bool logged_once_ = false;
  std::uint64_t last_log_ns_ = 0;
};

}  // namespace spg::plugin
