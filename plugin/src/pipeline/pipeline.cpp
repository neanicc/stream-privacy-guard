#include "pipeline/pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace spg::plugin {

namespace {

Cover cover_for(FallbackReason reason) noexcept {
  switch (reason) {
    case FallbackReason::None:
    case FallbackReason::NotScanned:
      return Cover::NotScanned;
    case FallbackReason::LateRelease:
      return Cover::LateRelease;
    case FallbackReason::BadTimestamp:
      return Cover::BadTimestamp;
    case FallbackReason::ApplyFailed:
      return Cover::ApplyFailed;
  }
  return Cover::NotScanned;
}

bool uses_slot(const DrawPlan& plan) noexcept {
  return plan.kind == DrawKind::Masked || plan.kind == DrawKind::BlurWhole;
}

// Clears `plan` but keeps the capacity of its rect list.
void reset(DrawPlan& plan) noexcept {
  plan.kind = DrawKind::Nothing;
  plan.slot = 0;
  plan.serial = 0;
  plan.frame = {};
  plan.rects.clear();
  plan.cover = Cover::None;
  plan.reemit = false;
  plan.live = false;
}

bool drawable_rect(const MaskRect& rect) noexcept {
  return rect.box.space == Space::Source && is_valid(rect.box.rect);
}

}  // namespace

Size readback_size(Size source, Size max) noexcept {
  if (source.width == 0 || source.height == 0 || max.width == 0 || max.height == 0) {
    return {};
  }
  const double scale = std::min({1.0, static_cast<double>(max.width) / source.width,
                                 static_cast<double>(max.height) / source.height});
  const auto side = [scale](std::uint32_t v) {
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(v * scale)));
  };
  return {side(source.width), side(source.height)};
}

DrawKind checked_kind(const DrawPlan& plan, const SlotContents& slot) noexcept {
  if (!uses_slot(plan)) {
    return plan.kind;
  }
  if (!slot.present || slot.serial == 0 || slot.serial != plan.serial || slot.size != plan.frame ||
      plan.frame.width == 0 || plan.frame.height == 0) {
    return DrawKind::Solid;
  }
  if (plan.kind == DrawKind::Masked && !std::all_of(plan.rects.begin(), plan.rects.end(), drawable_rect)) {
    return DrawKind::BlurWhole;
  }
  return plan.kind;
}

Pipeline::Pipeline(FrameGpu& gpu, std::size_t ring_capacity, Retire retire, Size max_readback)
    : gpu_(&gpu), retire_(std::move(retire)), max_readback_(max_readback), ring_(ring_capacity) {
  gens_.emplace_back();
  gens_.back().id = next_generation_++;
}

Pipeline::~Pipeline() {
  for (Generation& gen : gens_) {
    retire(std::move(gen.scheduler));
  }
}

void Pipeline::retire(std::unique_ptr<FrameScheduler> scheduler) noexcept {
  if (scheduler == nullptr) {
    return;
  }
  try {
    retire_(std::move(scheduler));
  } catch (...) {
    // The callback could not take it; `scheduler` (if still held) is
    // destroyed here, which waits for its workers. Only on a failed
    // allocation.
  }
}

void Pipeline::adopt(PipelineSpec& spec, Timestamp now) {
  const Generation& previous = gens_.back();
  // Detections the old tracker still held (up to its hold past the last
  // scan, which is no later than now) are not in the new one.
  std::optional<Timestamp> covered_until;
  if (previous.scheduler != nullptr) {
    covered_until = now + previous.hold;
  }
  if (previous.covered_until && (!covered_until || *previous.covered_until > *covered_until)) {
    covered_until = previous.covered_until;
  }
  if (adopt_fault_) {
    adopt_fault_();
  }
  gens_.emplace_back();  // the only step that can throw; nothing changed yet
  Generation& next = gens_.back();
  next.id = next_generation_++;
  next.scheduler = std::move(spec.scheduler);
  next.fallback = spec.fallback;
  next.hold = spec.hold;
  next.covered_until = covered_until;
  staged_.reset();      // a readback of the old generation's frame
  forget_last_clean();  // re-emits name frames of one generation
  retire_drained();
}

void Pipeline::mark_discontinuity() noexcept {
  for (Generation& gen : gens_) {
    if (gen.scheduler != nullptr) {
      try {
        gen.scheduler->mark_discontinuity();
      } catch (...) {
      }
    }
  }
  forget_last_clean();
}

const DrawPlan& Pipeline::tick(Timestamp t, Size source) {
  if (last_tick_ && *last_tick_ == t) {
    return display_;
  }
  if (last_tick_ && (t < *last_tick_ || t - *last_tick_ > kMaxTickGap)) {
    mark_discontinuity();
  }
  last_tick_ = t;
  ++counters_.ticks;
  trim_idle();
  Generation& current = gens_.back();

  if (source.width == 0 || source.height == 0) {
    // Nothing can be drawn on a 0x0 target. Frames that come due are dropped
    // unseen; schedulers, buffered frames and held masks are all kept.
    target_empty_ = true;
    staged_.reset();
    for (Generation& gen : gens_) {
      if (gen.scheduler == nullptr) {
        continue;
      }
      released_.clear();
      try {
        gen.scheduler->release_due(released_);
      } catch (...) {
      }
      for (const ReleasedFrame& frame : released_) {
        ++counters_.released;
        release_buffered(gen, frame.id);
      }
    }
    reset(next_);
    set_display(next_);
    last_release_.reset();
    retire_drained();
    return display_;
  }
  if (target_empty_) {
    // The content may have changed while nothing was rendered.
    target_empty_ = false;
    mark_discontinuity();
  }

  std::optional<std::size_t> slot = ring_.acquire();
  live_serial_ = 0;
  if (slot) {
    const std::uint64_t serial = next_serial_++;
    bool captured = false;
    try {
      captured = gpu_->capture(*slot, source, serial);
    } catch (...) {
      captured = false;
    }
    if (captured) {
      live_serial_ = serial;
    } else {
      ring_.release(*slot);
      slot.reset();
    }
  }
  if (!slot) {
    ++counters_.capture_failures;
  }

  if (current.scheduler != nullptr) {
    push_frame(current, t, source, slot);
  }

  bool released = false;
  for (Generation& gen : gens_) {
    if (gen.scheduler == nullptr) {
      continue;
    }
    released_.clear();
    try {
      gen.scheduler->release_due(released_);
    } catch (...) {
      // Allocation failure: the frames stay buffered and are released (late,
      // so covered) on a later tick.
    }
    for (const ReleasedFrame& frame : released_) {
      show_released(gen, frame);
      released = true;
    }
  }
  if (released) {
    last_release_ = t;
  }
  if (current.scheduler == nullptr) {
    show_live(slot, source, current.fallback, Cover::NoDetection);
  } else if (!released && (display_.kind == DrawKind::Nothing || display_.live)) {
    show_live(slot, source, current.fallback, Cover::Starting);
  } else if (!released && (!last_release_ || t - *last_release_ > kMaxRepeat)) {
    // Nothing new for too long: do not keep airing an old frame.
    show_live(slot, source, current.fallback, Cover::Stalled);
  }
  if (slot && current.scheduler == nullptr) {
    ring_.release(*slot);  // the display holds it if it shows it
  }
  retire_drained();
  return display_;
}

void Pipeline::push_frame(Generation& gen, Timestamp t, Size source, std::optional<std::size_t> slot) {
  // The readback staged on the previous tick shows the frame pushed then.
  std::shared_ptr<const OwnedFrame> readback;
  if (staged_ && staged_->generation == gen.id) {
    try {
      MappedReadback mapped;
      if (gpu_->map(staged_->surface, mapped)) {
        readback = pool_.copy_bgra(mapped.data, mapped.linesize, staged_->size, staged_->t);
        gpu_->unmap(staged_->surface);
      }
    } catch (...) {
      readback = nullptr;  // no scan; the frame fails closed unless another cycle covers it
    }
  }
  staged_.reset();
  if (readback != nullptr) {
    ++counters_.readbacks;
  }

  FrameId id = 0;
  try {
    id = gen.scheduler->push(t, source, std::move(readback));
    gen.buffered.push_back(Buffered{id, slot.value_or(kNoSlot), slot ? live_serial_ : 0, t, source});
  } catch (...) {
    // Not buffered (or buffered without its texture): when released it is
    // covered with a solid fill.
    if (slot) {
      ring_.release(*slot);
    }
    if (id != 0) {
      try {
        gen.buffered.push_back(Buffered{id, kNoSlot, 0, t, source});
      } catch (...) {
      }
    }
    return;
  }

  bool wanted = false;
  try {
    wanted = slot.has_value() && gen.scheduler->wants_scan(t);
  } catch (...) {
    wanted = false;
  }
  if (wanted) {
    const Size size = readback_size(source, max_readback_);
    const std::size_t surface = next_surface_;
    bool staged = false;
    try {
      staged = gpu_->stage(*slot, surface, size);
    } catch (...) {
      staged = false;
    }
    if (staged) {
      staged_ = Staged{surface, t, gen.id, size};
      next_surface_ = 1 - next_surface_;
    }
  }
}

std::optional<Pipeline::Buffered> Pipeline::take_buffered(Generation& gen, FrameId id) noexcept {
  // Frames are released in push order; anything before this one is gone.
  std::optional<Buffered> found;
  while (!gen.buffered.empty() && gen.buffered.front().id <= id) {
    const Buffered entry = gen.buffered.front();
    gen.buffered.pop_front();
    if (entry.id == id) {
      found = entry;
    } else if (entry.slot != kNoSlot) {
      ring_.release(entry.slot);
    }
  }
  return found;
}

void Pipeline::release_buffered(Generation& gen, FrameId id) noexcept {
  if (const std::optional<Buffered> entry = take_buffered(gen, id); entry && entry->slot != kNoSlot) {
    ring_.release(entry->slot);
  }
}

void Pipeline::show_released(Generation& gen, const ReleasedFrame& frame) {
  ++counters_.released;
  const std::optional<Buffered> entry = take_buffered(gen, frame.id);
  const std::size_t slot = entry ? entry->slot : kNoSlot;
  const std::uint64_t serial = entry ? entry->serial : 0;
  reset(next_);
  next_.frame = frame.source;
  // The release must describe the frame the pipeline buffered under that id;
  // anything else (unknown id, another time or size, or a clean action on a
  // plan marked fallback) is covered whole.
  const bool clean_action = frame.action == FallbackAction::None;
  const bool consistent = entry && entry->t == frame.frame_time && entry->size == frame.source &&
                          frame.plan.fallback != clean_action &&
                          (frame.reason == FallbackReason::None) == clean_action;
  const bool settings_changed = gen.covered_until && frame.frame_time < *gen.covered_until;
  if (consistent && frame.action == FallbackAction::None && !settings_changed) {
    if (slot == kNoSlot) {
      next_.kind = DrawKind::Solid;
      next_.cover = Cover::NoTexture;
    } else {
      next_.kind = DrawKind::Masked;
      next_.slot = slot;
      next_.serial = serial;
      next_.rects.assign(frame.plan.rects.begin(), frame.plan.rects.end());
      // Kept for HoldLastScanned.
      try {
        if (!last_clean_) {
          last_clean_.emplace();
        } else {
          ring_.release(last_clean_->slot);
        }
        ring_.retain(slot);
        last_clean_->generation = gen.id;
        last_clean_->frame = frame.id;
        last_clean_->slot = slot;
        last_clean_->serial = serial;
        last_clean_->size = frame.source;
        last_clean_->rects.assign(frame.plan.rects.begin(), frame.plan.rects.end());
      } catch (...) {
        forget_last_clean();
      }
    }
  } else {
    if (!consistent) {
      next_.cover = Cover::Inconsistent;
    } else if (settings_changed && frame.action == FallbackAction::None) {
      next_.cover = Cover::SettingsChanged;
    } else {
      next_.cover = cover_for(frame.reason);
    }
    if (consistent && frame.action == FallbackAction::HoldLastScanned && last_clean_ &&
        last_clean_->generation == gen.id && last_clean_->frame == frame.reemit) {
      next_.kind = DrawKind::Masked;
      next_.slot = last_clean_->slot;
      next_.serial = last_clean_->serial;
      next_.frame = last_clean_->size;
      next_.rects.assign(last_clean_->rects.begin(), last_clean_->rects.end());
      next_.reemit = true;
    } else if (frame.action == FallbackAction::SolidFill || gen.fallback == FallbackMode::SolidFill) {
      next_.kind = DrawKind::Solid;
    } else if (slot == kNoSlot || !consistent) {
      next_.kind = DrawKind::Solid;  // no texture known to show this frame
    } else {
      next_.kind = DrawKind::BlurWhole;
      next_.slot = slot;
      next_.serial = serial;
    }
  }
  if (slot == kNoSlot && next_.cover == Cover::None) {
    next_.cover = Cover::NoTexture;
  }
  set_display(next_);
  if (slot != kNoSlot) {
    ring_.release(slot);  // the buffer's hold
  }
}

void Pipeline::cover_whole(DrawPlan& next, std::size_t slot, std::uint64_t serial, Size size,
                           FallbackMode fallback) const {
  next.frame = size;
  if (fallback == FallbackMode::SolidFill || slot == kNoSlot) {
    next.kind = DrawKind::Solid;
  } else {
    next.kind = DrawKind::BlurWhole;  // HoldLastScanned has nothing to hold yet
    next.slot = slot;
    next.serial = serial;
  }
}

void Pipeline::show_live(std::optional<std::size_t> slot, Size source, FallbackMode fallback, Cover cover) {
  reset(next_);
  cover_whole(next_, slot.value_or(kNoSlot), slot ? live_serial_ : 0, source, fallback);
  next_.cover = slot ? cover : Cover::NoTexture;
  next_.live = true;
  set_display(next_);
}

void Pipeline::set_display(DrawPlan& next) {
  if (uses_slot(next)) {
    ring_.retain(next.slot);
  }
  if (uses_slot(display_)) {
    ring_.release(display_.slot);
  }
  display_.kind = next.kind;
  display_.slot = next.slot;
  display_.serial = next.serial;
  display_.frame = next.frame;
  display_.cover = next.cover;
  display_.reemit = next.reemit;
  display_.live = next.live;
  try {
    display_.rects.assign(next.rects.begin(), next.rects.end());
  } catch (...) {
    // Cannot list the masks: cover the frame whole instead.
    display_.rects.clear();
    if (display_.kind == DrawKind::Masked) {
      display_.kind = DrawKind::BlurWhole;
      display_.cover = Cover::Inconsistent;
      display_.reemit = false;
    }
  }
  if (display_.kind != DrawKind::Nothing) {
    ++counters_.shown[static_cast<std::size_t>(display_.cover)];
  }
}

void Pipeline::forget_last_clean() noexcept {
  if (last_clean_) {
    ring_.release(last_clean_->slot);
    last_clean_.reset();
  }
}

void Pipeline::retire_drained() {
  // Every generation but the current one leaves once its frames have aired.
  while (gens_.size() > 1 && gens_.front().buffered.empty()) {
    retire(std::move(gens_.front().scheduler));
    gens_.pop_front();
  }
}

void Pipeline::trim_idle() noexcept {
  try {
    for (const std::size_t slot : ring_.trim(kMaxIdleSlots)) {
      gpu_->free_slot(slot);
    }
  } catch (...) {
    // Allocation failure: the idle textures stay until a later tick.
  }
}

}  // namespace spg::plugin
