#include "pipeline/host.hpp"

#include <utility>

namespace spg::plugin {

PipelineHost::PipelineHost(FrameGpu& gpu, std::size_t ring_capacity, Pipeline::Retire retire, Size max_readback)
    : retire_(retire), pipeline_(std::make_unique<Pipeline>(gpu, ring_capacity, std::move(retire), max_readback)) {}

PipelineHost::~PipelineHost() {
  pipeline_.reset();  // retires its schedulers
  const auto drop = [this](std::unique_ptr<PipelineSpec>& spec) {
    if (spec != nullptr && spec->scheduler != nullptr) {
      try {
        retire_(std::move(spec->scheduler));
      } catch (...) {
      }
    }
    spec.reset();
  };
  drop(waiting_);
  const std::lock_guard lock(handoff_mutex_);
  drop(pending_);
}

void PipelineHost::offer(std::unique_ptr<PipelineSpec> spec) noexcept {
  std::unique_ptr<PipelineSpec> replaced;
  try {
    const std::lock_guard lock(handoff_mutex_);
    replaced = std::move(pending_);
    pending_ = std::move(spec);
    failed_.store(false);
    has_news_.store(true, std::memory_order_release);
  } catch (...) {
    // The mutex failed: nothing was handed over; cover until the next offer.
    fail();
  }
  if (replaced != nullptr && replaced->scheduler != nullptr) {
    try {
      retire_(std::move(replaced->scheduler));
    } catch (...) {
    }
  }
}

void PipelineHost::fail() noexcept {
  failed_.store(true);
  std::unique_ptr<PipelineSpec> replaced;
  try {
    const std::lock_guard lock(handoff_mutex_);
    replaced = std::move(pending_);  // built for settings now superseded
  } catch (...) {
    // Cannot lock: the failure is flagged all the same.
  }
  has_news_.store(true, std::memory_order_release);
  if (replaced != nullptr && replaced->scheduler != nullptr) {
    try {
      retire_(std::move(replaced->scheduler));
    } catch (...) {
    }
  }
}

const DrawPlan& PipelineHost::solid(Timestamp t, Size source, Cover cover) noexcept {
  solid_.kind = (source.width == 0 || source.height == 0) ? DrawKind::Nothing : DrawKind::Solid;
  solid_.slot = 0;
  solid_.serial = 0;
  solid_.frame = source;
  solid_.rects.clear();
  solid_.cover = cover;
  solid_.reemit = false;
  solid_.live = true;
  last_cover_.store(static_cast<std::uint8_t>(cover), std::memory_order_relaxed);
  failed_tick_ = t;
  return solid_;
}

const DrawPlan& PipelineHost::frame(Timestamp t, Size source) noexcept {
  if (has_news_.load(std::memory_order_acquire)) {
    try {
      std::unique_ptr<PipelineSpec> spec;
      {
        const std::lock_guard lock(handoff_mutex_);
        spec = std::move(pending_);
        cover_all_ = failed_.load();
        has_news_.store(false, std::memory_order_relaxed);
      }
      if (spec != nullptr) {
        if (waiting_ != nullptr && waiting_->scheduler != nullptr) {
          retire_(std::move(waiting_->scheduler));
        }
        waiting_ = std::move(spec);
      }
    } catch (...) {
      cover_all_ = true;  // could not read the hand-off: cover until it can
    }
  }
  if (cover_all_) {
    return solid(t, source, Cover::NoDetection);
  }
  if (failed_tick_ && *failed_tick_ == t) {
    return solid_;  // the same frame time rendered again: same answer
  }
  failed_tick_.reset();
  try {
    if (waiting_ != nullptr) {
      const bool scanning = waiting_->scheduler != nullptr;
      pipeline_->adopt(*waiting_, t);  // strong guarantee: on a throw waiting_ is kept
      waiting_.reset();
      scanning_.store(scanning, std::memory_order_relaxed);
    }
    const DrawPlan& plan = pipeline_->tick(t, source);
    const PipelineCounters& counters = pipeline_->counters();
    std::uint64_t covered = 0;
    for (std::size_t i = 1; i < kCoverCount; ++i) {
      covered += counters.shown[i];
    }
    clean_.store(counters.shown[0], std::memory_order_relaxed);
    covered_.store(covered, std::memory_order_relaxed);
    last_cover_.store(static_cast<std::uint8_t>(plan.cover), std::memory_order_relaxed);
    return plan;
  } catch (...) {
    // A new generation could not be adopted, or the tick failed: cover this
    // frame; the next frame time tries again.
    return solid(t, source, Cover::Inconsistent);
  }
}

RenderStatus PipelineHost::status() const noexcept {
  RenderStatus status;
  status.scanning = scanning_.load(std::memory_order_relaxed);
  status.clean = clean_.load(std::memory_order_relaxed);
  status.covered = covered_.load(std::memory_order_relaxed);
  status.last_cover = static_cast<Cover>(last_cover_.load(std::memory_order_relaxed));
  return status;
}

}  // namespace spg::plugin
