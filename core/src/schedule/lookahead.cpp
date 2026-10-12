#include "spg/lookahead.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../mask/mask_rects.hpp"
#include "spg/error.hpp"

namespace spg {

namespace {

// Source sizes remembered for rescaling late results. A job older than this
// many resizes is treated as incomplete.
constexpr std::size_t kMaxSizeHistory = 64;

const SchedulerConfig& validated(const SchedulerConfig& config) {
  validate(config);
  return config;
}

TrackerConfig tracker_config(const SchedulerConfig& config) {
  TrackerConfig tracker = config.tracker;
  tracker.retain = std::max(tracker.retain, config.lookahead + config.release_grace);
  return tracker;
}

}  // namespace

SchedulerConfig SchedulerConfig::from_policy(const PolicyConfig& policy) {
  SchedulerConfig config;
  config.lookahead = policy.lookahead;
  config.retro = policy.retro;
  config.fallback = policy.fallback;
  config.mask_style = policy.mask_style;
  config.categories = policy.categories;
  config.scan.categories = policy.categories;
  config.tracker = TrackerConfig::from_policy(policy);
  return config;
}

void validate(const SchedulerConfig& config) {
  const auto non_negative = [](Duration d, const char* field) {
    if (d < Duration::zero()) {
      throw ContractError(std::string("scheduler config: ") + field + " must not be negative");
    }
  };
  non_negative(config.lookahead, "lookahead");
  non_negative(config.retro, "retro");
  non_negative(config.scan_interval, "scan_interval");
  non_negative(config.clock_tolerance, "clock_tolerance");
  non_negative(config.release_grace, "release_grace");
  non_negative(config.shutdown_timeout, "shutdown_timeout");
  const auto positive = [](std::size_t n, const char* field) {
    if (n == 0) {
      throw ContractError(std::string("scheduler config: ") + field + " must be at least 1");
    }
  };
  positive(config.queue_capacity, "queue_capacity");
  positive(config.workers, "workers");
  positive(config.max_buffered_frames, "max_buffered_frames");
  positive(config.max_retro_rects, "max_retro_rects");
  validate(config.tracker);
}

Lookahead::Lookahead(const Clock& clock, SchedulerConfig config)
    : clock_(&clock), config_(validated(config)), tracker_(clock, tracker_config(config_)) {}

bool Lookahead::wants_scan(Timestamp t) const noexcept {
  return !last_queued_.has_value() || t - *last_queued_ >= config_.scan_interval;
}

FrameId Lookahead::push(Timestamp t, Size source, std::shared_ptr<const OwnedFrame> readback) {
  if (source.width == 0 || source.height == 0) {
    throw ContractError("lookahead: source size must not have a zero dimension");
  }
  const Timestamp now = clock_->now();
  // Frames and the clock must share one time base; the tracker prunes by
  // comparing them. A frame that breaks it is never scanned and fails closed.
  const bool bad = (have_frame_ && t <= last_pushed_) || t > now + config_.clock_tolerance ||
                   t < now - config_.clock_tolerance;

  if (sizes_.empty()) {
    sizes_.push_back(source);
  } else if (source != sizes_.back()) {
    tracker_.rescale(sizes_.back(), source);
    sizes_.push_back(source);
    if (sizes_.size() > kMaxSizeHistory) {
      sizes_.pop_front();
      ++first_epoch_;
    }
    ++segment_;
  }

  FrameRecord record;
  record.id = next_frame_++;
  record.t = t;
  record.due = bad ? now + config_.lookahead : t + config_.lookahead;
  record.source = source;
  record.size_epoch = first_epoch_ + sizes_.size() - 1;
  record.segment = segment_;
  record.bad_timestamp = bad;
  frames_.push_back(record);
  ++stats_.frames_pushed;

  if (bad) {
    ++stats_.bad_timestamps;
    return record.id;
  }
  have_frame_ = true;
  last_pushed_ = t;

  if (readback != nullptr && wants_scan(t)) {
    if (queue_.size() >= config_.queue_capacity) {
      queue_.pop_front();
      ++stats_.scans_dropped;
    }
    ScanJob job;
    job.seq = next_job_++;
    job.frame = record.id;
    job.frame_time = t;
    job.source = source;
    job.readback = std::move(readback);
    job.size_epoch = record.size_epoch;
    job.segment = record.segment;
    queue_.push_back(std::move(job));
    last_queued_ = t;
    ++stats_.scans_queued;
  }
  return record.id;
}

std::optional<ScanJob> Lookahead::take_job() {
  if (queue_.empty()) {
    return std::nullopt;
  }
  ScanJob job = std::move(queue_.back());
  queue_.pop_back();
  in_flight_.push_back(job.seq);
  ++stats_.scans_started;
  return job;
}

void Lookahead::complete(const ScanJob& job, const ScanResult& result) {
  const auto it = std::find(in_flight_.begin(), in_flight_.end(), job.seq);
  if (it == in_flight_.end()) {
    throw ContractError("lookahead: complete() for a job that is not in flight");
  }
  in_flight_.erase(it);
  if (!result.complete || job.size_epoch < first_epoch_) {
    ++stats_.scans_failed;
    return;
  }

  // The tracker works in the current source size. A result from before a
  // resize is held as the hull of its box scaled to every size since, so
  // frames of each size stay covered (masks only grow).
  const std::uint64_t current = first_epoch_ + sizes_.size() - 1;
  std::vector<Detection> tracked;
  tracked.reserve(result.detections.size());
  for (const Detection& d : result.detections) {
    Detection grown = d;
    for (std::uint64_t e = job.size_epoch + 1; e <= current; ++e) {
      grown.box.rect = mask::hull(grown.box.rect,
                                  mask::scale_between(d.box.rect, job.source, sizes_[e - first_epoch_]));
    }
    tracked.push_back(grown);
  }
  try {
    tracker_.update(job.frame_time, tracked);
  } catch (...) {
    ++stats_.scans_failed;
    return;
  }

  // Retroactive masking: the tracker only covers frames at or after the
  // frame a detection was seen on; earlier buffered frames get the boxes here.
  const Timestamp from = job.frame_time - config_.retro;
  for (FrameRecord& frame : frames_) {
    if (frame.bad_timestamp || frame.t < from || frame.t > job.frame_time) {
      continue;
    }
    if (frame.t < job.frame_time) {
      for (const Detection& d : result.detections) {
        Rect r = d.box.rect;
        if (frame.source != job.source) {
          r = mask::hull(r, mask::scale_between(r, job.source, frame.source));
        }
        add_retro(frame, mask::grow_by_margin(r, config_.tracker.margin_px, config_.tracker.margin_ratio));
      }
    }
    if (frame.segment == job.segment) {
      frame.covered = true;
    }
  }
  ++stats_.scans_completed;
}

void Lookahead::add_retro(FrameRecord& frame, const Rect& rect) const {
  for (const Rect& existing : frame.retro) {
    if (mask::contains(existing, rect)) {
      return;
    }
  }
  std::erase_if(frame.retro, [&rect](const Rect& existing) { return mask::contains(rect, existing); });
  frame.retro.push_back(rect);
  if (frame.retro.size() > config_.max_retro_rects) {
    Rect all = frame.retro.front();
    for (const Rect& r : frame.retro) {
      all = mask::hull(all, r);
    }
    frame.retro.assign(1, all);
  }
}

FallbackAction Lookahead::fallback_action(const FrameRecord& frame, FrameId& reemit) const noexcept {
  switch (config_.fallback) {
    case FallbackMode::SolidFill:
      return FallbackAction::SolidFill;
    case FallbackMode::HoldLastScanned:
      if (last_clean_.has_value() && last_clean_segment_ == frame.segment) {
        reemit = *last_clean_;
        return FallbackAction::HoldLastScanned;
      }
      return FallbackAction::BlurSource;
    case FallbackMode::BlurSource:
      break;
  }
  return FallbackAction::BlurSource;
}

void Lookahead::release_due(std::vector<ReleasedFrame>& out) {
  const Timestamp now = clock_->now();
  while (!frames_.empty()) {
    const FrameRecord& frame = frames_.front();
    const bool over = frames_.size() > config_.max_buffered_frames;
    if (!over && now < frame.due) {
      break;
    }
    ReleasedFrame released;
    released.id = frame.id;
    released.frame_time = frame.t;
    released.source = frame.source;
    released.plan.frame_time = frame.t;
    tracker_.masks(frame.t, masks_buffer_);
    released.plan.rects.reserve(masks_buffer_.size() + frame.retro.size());
    for (const Box& box : masks_buffer_) {
      released.plan.rects.push_back(MaskRect{box, config_.mask_style});
    }
    for (const Rect& rect : frame.retro) {
      released.plan.rects.push_back(MaskRect{Box{rect, Space::Source}, config_.mask_style});
    }

    if (frame.bad_timestamp) {
      released.reason = FallbackReason::BadTimestamp;
    } else if (!frame.covered) {
      released.reason = FallbackReason::NotScanned;
    } else if (now > frame.t + config_.lookahead + config_.release_grace) {
      released.reason = FallbackReason::LateRelease;
    }
    if (released.reason != FallbackReason::None) {
      released.plan.fallback = true;
      released.action = fallback_action(frame, released.reemit);
      ++stats_.frames_failed_closed;
    } else {
      last_clean_ = frame.id;
      last_clean_segment_ = frame.segment;
    }
    if (over) {
      ++stats_.early_releases;
    }
    ++stats_.frames_released;
    out.push_back(std::move(released));
    frames_.pop_front();
  }
}

void Lookahead::mark_discontinuity() noexcept {
  // Frames already buffered keep their segment; HoldLastScanned compares
  // segments, so a frame of the old content can still re-emit one.
  ++segment_;
}

}  // namespace spg
