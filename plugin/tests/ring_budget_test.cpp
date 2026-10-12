// The lookahead ring's VRAM budget math and its warning (SPG-14c), without
// OBS. How Pipeline enforces the budget per capture is in pipeline_test.cpp.
#include <chrono>
#include <cstdint>
#include <random>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "pipeline/ring_budget.hpp"

namespace {

using namespace std::chrono_literals;
using spg::Duration;
using spg::Size;
using spg::plugin::BudgetNotice;
using spg::plugin::fit_lookahead;
using spg::plugin::kBytesPerMiB;
using spg::plugin::kNoRingBudget;
using spg::plugin::kRingReserveSlots;
using spg::plugin::LookaheadFit;
using spg::plugin::ring_frames_needed;
using spg::plugin::texture_bytes;

constexpr Size k1080{1920, 1080};
constexpr Size k2160{3840, 2160};
constexpr Size k720{1280, 720};
constexpr Duration k60fps{1'000'000'000LL / 60};  // as the filter computes it: 16666666 ns
constexpr Duration k30fps{1'000'000'000LL / 30};
constexpr std::uint64_t k512MiB = 512 * kBytesPerMiB;

std::uint64_t bgra(Size size) {
  return texture_bytes(size, 32);
}

}  // namespace

TEST_CASE("a ring texture costs its size times the format's bytes per pixel", "[plugin][budget]") {
  CHECK(bgra(k1080) == 8'294'400);  // the plan's "about 8 MB per 1080p frame"
  CHECK(bgra(k2160) == 4 * bgra(k1080));
  CHECK(texture_bytes(k1080, 64) == 2 * bgra(k1080));  // a 16-bit-per-channel format
  CHECK(bgra({0, 1080}) == 0);
  CHECK(bgra({65535, 65535}) == 4ULL * 65535 * 65535);  // no 32-bit overflow
}

TEST_CASE("a lookahead needs its frames plus the reserve", "[plugin][budget]") {
  // Frames with a time in [t - 500 ms, t] at 60 fps, the capture of this
  // tick, the frame on screen and the last clean frame.
  CHECK(ring_frames_needed(500ms, k60fps) == 30 + 1 + kRingReserveSlots);
  CHECK(ring_frames_needed(100ms, 10ms) == 10 + 1 + kRingReserveSlots);
  CHECK(ring_frames_needed(0ms, k60fps) == 1 + kRingReserveSlots);
  CHECK(ring_frames_needed(2000ms, k60fps) == 120 + 1 + kRingReserveSlots);
}

TEST_CASE("the default budget holds the default lookahead at 1080p60 and shortens longer ones", "[plugin][budget]") {
  const LookaheadFit fits = fit_lookahead(500ms, k512MiB, k1080, bgra(k1080), k60fps);
  CHECK_FALSE(fits.shortened);
  CHECK(fits.lookahead == 500ms);
  CHECK(fits.budget_frames == 64);  // 512 MiB / 7.9 MiB

  const LookaheadFit longer = fit_lookahead(2000ms, k512MiB, k1080, bgra(k1080), k60fps);
  CHECK(longer.shortened);
  CHECK(longer.requested == 2000ms);
  CHECK(longer.lookahead == 1016ms);  // just under 61 frames of 16.67 ms, whole ms rounded down
  CHECK(ring_frames_needed(longer.lookahead, k60fps) <= longer.budget_frames);
  CHECK(longer.frame == k1080);
  CHECK(longer.frame_bytes == bgra(k1080));
}

TEST_CASE("a resize refits the lookahead to the actual texture size", "[plugin][budget]") {
  const LookaheadFit hd = fit_lookahead(500ms, k512MiB, k1080, bgra(k1080), k60fps);
  const LookaheadFit uhd = fit_lookahead(500ms, k512MiB, k2160, bgra(k2160), k60fps);
  const LookaheadFit small = fit_lookahead(2000ms, k512MiB, k720, bgra(k720), k30fps);
  CHECK_FALSE(hd.shortened);
  CHECK(uhd.shortened);
  CHECK(uhd.budget_frames == 16);
  CHECK(uhd.lookahead == 216ms);  // just under 13 frames of 16.67 ms
  CHECK_FALSE(small.shortened);  // 60 + 4 frames of 3.5 MiB fit in 145
  // A larger format at the same size holds fewer frames.
  const LookaheadFit wide = fit_lookahead(500ms, k512MiB, k1080, texture_bytes(k1080, 64), k60fps);
  CHECK(wide.budget_frames == 32);
  CHECK(wide.lookahead < hd.lookahead);
}

TEST_CASE("a budget below the reserve leaves no lookahead, so every frame fails closed", "[plugin][budget]") {
  const LookaheadFit none = fit_lookahead(500ms, 3 * bgra(k1080), k1080, bgra(k1080), k60fps);
  CHECK(none.shortened);
  CHECK(none.lookahead == 0ms);
  CHECK(none.budget_frames == 3);
  const LookaheadFit one = fit_lookahead(500ms, 4 * bgra(k1080), k1080, bgra(k1080), k60fps);
  CHECK(one.lookahead == 16ms);  // just under one frame: the due frame and the reserve
  const LookaheadFit two = fit_lookahead(500ms, 5 * bgra(k1080), k1080, bgra(k1080), k60fps);
  CHECK(two.lookahead == 33ms);
}

TEST_CASE("an unknown size or frame rate leaves the lookahead as configured", "[plugin][budget]") {
  // Pipeline still refuses any capture past the budget (pipeline_test.cpp).
  for (const LookaheadFit& fit : {fit_lookahead(500ms, k512MiB, {}, 0, k60fps),
                                  fit_lookahead(500ms, k512MiB, k2160, bgra(k2160), Duration{}),
                                  fit_lookahead(500ms, kNoRingBudget, k2160, bgra(k2160), k60fps)}) {
    CHECK_FALSE(fit.shortened);
    CHECK(fit.lookahead == 500ms);
  }
}

TEST_CASE("a fitted lookahead always fits and is the longest that does", "[plugin][budget]") {
  std::mt19937 rng(14);
  std::uniform_int_distribution<std::uint32_t> side(16, 7680);
  std::uniform_int_distribution<std::int64_t> ms(0, 2000);
  std::uniform_int_distribution<std::uint64_t> mib(64, 4096);
  std::uniform_int_distribution<std::int64_t> fps(10, 240);
  for (int i = 0; i < 5000; ++i) {
    const Size size{side(rng), side(rng)};
    const Duration requested = std::chrono::milliseconds(ms(rng));
    const std::uint64_t budget = mib(rng) * kBytesPerMiB;
    const Duration interval{1'000'000'000LL / fps(rng)};
    const LookaheadFit fit = fit_lookahead(requested, budget, size, bgra(size), interval);
    INFO("size " << size.width << "x" << size.height << " requested " << requested.count() << " budget "
                 << budget << " interval " << interval.count());
    REQUIRE(fit.lookahead <= requested);
    REQUIRE(fit.lookahead >= Duration::zero());
    REQUIRE(fit.shortened == (fit.lookahead < requested));
    REQUIRE(fit.lookahead % 1ms == Duration::zero());
    if (fit.budget_frames > kRingReserveSlots) {
      REQUIRE(ring_frames_needed(fit.lookahead, interval) <= fit.budget_frames);
    }
    if (fit.shortened) {
      // One more frame of lookahead would not fit.
      REQUIRE(ring_frames_needed(fit.lookahead + interval, interval) > fit.budget_frames);
    }
  }
}

TEST_CASE("the budget warning shows while shortened and logs once per change", "[plugin][budget]") {
  BudgetNotice notice;
  const LookaheadFit fits = fit_lookahead(500ms, k512MiB, k1080, bgra(k1080), k60fps);
  CHECK_FALSE(notice.update(fits));  // nothing to say
  CHECK_FALSE(notice.warning());

  const LookaheadFit uhd = fit_lookahead(500ms, k512MiB, k2160, bgra(k2160), k60fps);
  CHECK(notice.update(uhd));  // shortened: one log line
  CHECK(notice.warning());
  CHECK(notice.fit() == uhd);
  CHECK_FALSE(notice.update(uhd));  // the same fit again (another tick, an unrelated update)
  CHECK(notice.warning());

  const LookaheadFit uhd30 = fit_lookahead(500ms, k512MiB, k2160, bgra(k2160), k30fps);
  REQUIRE(uhd30.shortened);
  REQUIRE(uhd30.lookahead != uhd.lookahead);
  CHECK(notice.update(uhd30));  // shortened to another value
  CHECK(notice.warning());

  CHECK(notice.update(fits));  // restored: one line saying so
  CHECK_FALSE(notice.warning());
  CHECK_FALSE(notice.update(fits));
}

TEST_CASE("the budget log line names sizes and durations only", "[plugin][budget]") {
  const LookaheadFit shortened = fit_lookahead(2000ms, k512MiB, k1080, bgra(k1080), k60fps);
  const std::string line = describe(shortened);
  CHECK(line.find("lookahead shortened from 2000 ms to 1016 ms") != std::string::npos);
  CHECK(line.find("512 MiB holds 64 frames of 1920x1080 (7.9 MiB each)") != std::string::npos);
  const std::string restored = describe(fit_lookahead(500ms, k512MiB, k1080, bgra(k1080), k60fps));
  CHECK(restored.find("lookahead of 500 ms fits again") != std::string::npos);
}
