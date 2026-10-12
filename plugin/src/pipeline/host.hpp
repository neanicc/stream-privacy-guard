#pragma once

// The hand-off between the settings side and the render thread (SPG-14),
// without OBS. The settings side offers a ready scheduler generation (or
// reports that it could not build one); the render thread takes it at the
// start of a frame and asks for that frame's plan. Every frame's plan comes
// from one state: if anything is missing or fails (a failed build, an
// adoption or a tick that throws), the frame is covered with a solid fill,
// and a second render of the same frame time gets the same answer.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "pipeline/pipeline.hpp"

namespace spg::plugin {

struct RenderStatus {
  bool scanning = false;      // the current generation has a scheduler
  std::uint64_t clean = 0;    // plans drawn with only their masks
  std::uint64_t covered = 0;  // plans drawn covered whole
  Cover last_cover = Cover::None;
};

class PipelineHost {
 public:
  PipelineHost(FrameGpu& gpu, std::size_t ring_capacity, Pipeline::Retire retire, Size max_readback = kMaxReadback);
  ~PipelineHost();
  PipelineHost(const PipelineHost&) = delete;
  PipelineHost& operator=(const PipelineHost&) = delete;

  // Any thread. Hands over a generation for the next frame and clears a
  // failure; a generation offered earlier and not yet taken is retired.
  void offer(std::unique_ptr<PipelineSpec> spec) noexcept;
  // Any thread. No generation could be built for the current settings:
  // every frame is covered with a solid fill until the next offer().
  void fail() noexcept;

  // Render thread: the plan for frame time `t` with the target at `source`.
  [[nodiscard]] const DrawPlan& frame(Timestamp t, Size source) noexcept;

  [[nodiscard]] RenderStatus status() const noexcept;
  // Render thread, for tests.
  [[nodiscard]] const Pipeline& pipeline() const noexcept { return *pipeline_; }
  // Test seam: runs inside each adoption attempt where it can throw.
  void set_adopt_fault(std::function<void()> fault) { pipeline_->set_adopt_fault(std::move(fault)); }

 private:
  const DrawPlan& solid(Timestamp t, Size source, Cover cover) noexcept;

  Pipeline::Retire retire_;
  std::unique_ptr<Pipeline> pipeline_;

  std::mutex handoff_mutex_;  // held only to swap pending_
  std::unique_ptr<PipelineSpec> pending_;
  std::atomic<bool> failed_{false};
  std::atomic<bool> has_news_{false};

  // Render thread only.
  std::unique_ptr<PipelineSpec> waiting_;  // taken but not adopted yet
  bool cover_all_ = false;
  std::optional<Timestamp> failed_tick_;
  DrawPlan solid_;

  std::atomic<bool> scanning_{false};
  std::atomic<std::uint64_t> clean_{0};
  std::atomic<std::uint64_t> covered_{0};
  std::atomic<std::uint8_t> last_cover_{0};
};

}  // namespace spg::plugin
