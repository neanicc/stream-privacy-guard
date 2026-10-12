#pragma once

#include <chrono>
#include <cstdint>

namespace spg {

// Monotonic frame time with an arbitrary epoch, in nanoseconds (the unit OBS
// uses for frame timestamps). FrameClock only names the time base; time is
// read through an injected Clock, never a global.
struct FrameClock {
  using rep = std::int64_t;
  using period = std::nano;
  using duration = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<FrameClock, duration>;
  static constexpr bool is_steady = true;
};

using Duration = FrameClock::duration;
using Timestamp = FrameClock::time_point;

[[nodiscard]] constexpr Timestamp timestamp_from_ns(std::int64_t ns) noexcept {
  return Timestamp{Duration{ns}};
}
[[nodiscard]] constexpr std::int64_t to_ns(Timestamp t) noexcept {
  return t.time_since_epoch().count();
}

// Source of the current time. Core components take a Clock& so tests and
// simulations control time exactly.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;
  [[nodiscard]] virtual Timestamp now() const noexcept = 0;
};

// Wall-clock time from std::chrono::steady_clock.
class SteadyClock final : public Clock {
 public:
  [[nodiscard]] Timestamp now() const noexcept override;
};

// Time that moves only when told to. Not thread-safe; for tests and
// single-threaded simulations.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(Timestamp start = Timestamp{}) noexcept : now_(start) {}
  [[nodiscard]] Timestamp now() const noexcept override { return now_; }
  void set(Timestamp t) noexcept { now_ = t; }
  void advance(Duration d) noexcept { now_ += d; }

 private:
  Timestamp now_;
};

}  // namespace spg
