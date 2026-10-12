#pragma once

// The lookahead ring's VRAM budget (SPG-14c, plan section 7), without OBS.
// Every buffered frame is a full-resolution texture, so the budget bounds how
// long a lookahead the ring can hold: when the configured lookahead needs more
// textures than the budget holds at the source's size, the filter builds its
// scheduler with a shorter one (fit_lookahead), shows a warning and logs one
// line (BudgetNotice). Pipeline also enforces the budget on every capture: a
// frame that would take the ring past it gets no texture and is covered whole.
// Rules: docs/plugin-render.md.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "spg/geometry.hpp"
#include "spg/time.hpp"

namespace spg::plugin {

inline constexpr std::uint64_t kNoRingBudget = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint64_t kBytesPerMiB = 1024ULL * 1024ULL;

// Bytes of one ring texture of `size` in a format of `bits_per_pixel` (BGRA:
// 32): what the GPU side allocates for it, driver padding not counted.
[[nodiscard]] std::uint64_t texture_bytes(Size size, std::uint32_t bits_per_pixel) noexcept;

// Ring textures a generation needs besides the frames inside its lookahead
// window: the frame on screen, the last frame released clean (kept for
// HoldLastScanned), and one frame of timing slack (a tick that runs late).
// A second generation draining after a settings change is not included: its
// frames past the budget are refused and covered (Pipeline).
inline constexpr std::size_t kRingReserveSlots = 3;

// Textures one generation holds with `lookahead` at one frame per `interval`:
// the frames with a time in [t - lookahead, t] (the capture of this tick and
// the frame due now included) plus kRingReserveSlots.
[[nodiscard]] std::size_t ring_frames_needed(Duration lookahead, Duration interval) noexcept;

struct LookaheadFit {
  Duration requested{};  // the configured lookahead
  Duration lookahead{};  // what the scheduler gets: `requested`, or shorter
  bool shortened = false;
  Size frame;                     // the source size the fit was made for
  std::uint64_t frame_bytes = 0;  // one texture at that size (0: unknown)
  std::uint64_t budget_bytes = kNoRingBudget;
  std::size_t budget_frames = 0;  // textures the budget holds at frame_bytes
  friend bool operator==(const LookaheadFit&, const LookaheadFit&) = default;
};

// The longest lookahead, at most `requested`, whose frames fit in
// `budget_bytes` at `frame_bytes` per texture and one frame per `interval`,
// in whole milliseconds rounded down. Unknown inputs (a 0x0 source, no frame
// rate) leave `requested` as is; Pipeline still enforces the budget per
// capture. When not even the reserve fits the lookahead is 0: every frame is
// then released before a scan can finish and is covered (fail closed).
[[nodiscard]] LookaheadFit fit_lookahead(Duration requested, std::uint64_t budget_bytes, Size frame,
                                         std::uint64_t frame_bytes, Duration interval) noexcept;

// The warning the filter shows while the lookahead is shortened, and when to
// write its one log line.
class BudgetNotice {
 public:
  // Records the fit a generation was built with. True when the log should
  // say so: the lookahead was shortened, shortened to another value, or
  // restored to the configured one. A fit that changes nothing else (another
  // size with the same result) is not logged again.
  bool update(const LookaheadFit& fit) noexcept;
  [[nodiscard]] bool warning() const noexcept { return fit_.shortened; }
  [[nodiscard]] const LookaheadFit& fit() const noexcept { return fit_; }

 private:
  LookaheadFit fit_;
  bool logged_shortened_ = false;
};

// The log line for `fit`: sizes and durations only, never frame content.
[[nodiscard]] std::string describe(const LookaheadFit& fit);

// How often the filter's video tick retries a refit that failed.
inline constexpr std::uint64_t kRefitRetryNs = 1'000'000'000ULL;

// When the filter's video tick refits the lookahead (FilterRender::tick):
// once for each new source size or frame rate, and after a refit that failed
// (the source is then covered whole) again every kRefitRetryNs until one
// succeeds, so the cover does not outlast the failure. Video thread only.
class RefitSchedule {
 public:
  // A refit should be tried now. Never for a hidden (0x0) source.
  [[nodiscard]] bool due(Size source, Duration interval, std::uint64_t now_ns) const noexcept;
  // `source` and `interval` are fitted: not due again until either changes.
  void done(Size source, Duration interval) noexcept;
  // The refit failed: due again from now_ns + kRefitRetryNs.
  void failed(std::uint64_t now_ns) noexcept;

 private:
  Size checked_;
  Duration interval_{};
  std::uint64_t retry_at_ns_ = 0;
};

}  // namespace spg::plugin
