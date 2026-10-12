#include "pipeline/ring_budget.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>

namespace spg::plugin {

std::uint64_t texture_bytes(Size size, std::uint32_t bits_per_pixel) noexcept {
  return static_cast<std::uint64_t>(size.width) * size.height * bits_per_pixel / 8;
}

std::size_t ring_frames_needed(Duration lookahead, Duration interval) noexcept {
  if (interval <= Duration::zero()) {
    return kRingReserveSlots + 1;
  }
  const auto inside = lookahead > Duration::zero() ? static_cast<std::size_t>(lookahead / interval) : 0;
  return inside + 1 + kRingReserveSlots;
}

LookaheadFit fit_lookahead(Duration requested, std::uint64_t budget_bytes, Size frame, std::uint64_t frame_bytes,
                           Duration interval) noexcept {
  LookaheadFit fit;
  fit.requested = requested;
  fit.lookahead = requested;
  fit.frame = frame;
  fit.frame_bytes = frame_bytes;
  fit.budget_bytes = budget_bytes;
  if (frame_bytes == 0 || interval <= Duration::zero()) {
    return fit;  // unknown size or frame rate: Pipeline still caps each capture
  }
  fit.budget_frames = static_cast<std::size_t>(std::min<std::uint64_t>(
      budget_bytes / frame_bytes, std::numeric_limits<std::size_t>::max()));
  if (ring_frames_needed(requested, interval) <= fit.budget_frames) {
    return fit;
  }
  // The longest with floor(lookahead / interval) + 1 + reserve <= budget_frames.
  Duration longest{};
  if (fit.budget_frames > kRingReserveSlots) {
    longest = interval * static_cast<std::int64_t>(fit.budget_frames - kRingReserveSlots) - Duration{1};
  }
  longest = std::chrono::floor<std::chrono::milliseconds>(longest);
  fit.lookahead = std::min(requested, longest);
  fit.shortened = fit.lookahead < requested;
  return fit;
}

bool BudgetNotice::update(const LookaheadFit& fit) noexcept {
  bool log = false;
  if (fit.shortened) {
    log = !logged_shortened_ || fit.lookahead != fit_.lookahead;
  } else {
    log = logged_shortened_;  // restored
  }
  logged_shortened_ = fit.shortened;
  fit_ = fit;
  return log;
}

std::string describe(const LookaheadFit& fit) {
  const auto ms = [](Duration d) { return static_cast<long long>(std::chrono::floor<std::chrono::milliseconds>(d).count()); };
  const double frame_mib = static_cast<double>(fit.frame_bytes) / static_cast<double>(kBytesPerMiB);
  const std::uint64_t budget_mib = fit.budget_bytes / kBytesPerMiB;
  char line[256];
  if (fit.shortened) {
    std::snprintf(line, sizeof line,
                  "VRAM budget: lookahead shortened from %lld ms to %lld ms; the budget of %" PRIu64
                  " MiB holds %zu frames of %ux%u (%.1f MiB each)",
                  ms(fit.requested), ms(fit.lookahead), budget_mib, fit.budget_frames, fit.frame.width,
                  fit.frame.height, frame_mib);
  } else {
    std::snprintf(line, sizeof line,
                  "VRAM budget: the configured lookahead of %lld ms fits again (budget %" PRIu64
                  " MiB, frames of %ux%u)",
                  ms(fit.requested), budget_mib, fit.frame.width, fit.frame.height);
  }
  return line;
}

}  // namespace spg::plugin
