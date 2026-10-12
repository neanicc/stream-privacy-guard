// Scheduler, lookahead and fail-closed release (SPG-10): seeded property
// tests. Each scene is a deterministic simulation on a ManualClock: frames at
// a jittered rate, static secrets that appear and disappear, cycles that start
// late, take random time, fail, stall forever or return after their frames
// were released, random cadence and queue limits, render hiccups, a resize in
// some scenes and a content switch in others. An oracle kept by the test, not
// the scheduler, checks every released frame.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "schedule_fakes.hpp"
#include "spg/lookahead.hpp"
#include "spg/mask.hpp"
#include "spg/scan.hpp"
#include "spg/time.hpp"

namespace {

using namespace std::chrono_literals;
using spg::Duration;
using spg::FallbackAction;
using spg::FallbackMode;
using spg::Rect;
using spg::ReleasedFrame;
using spg::SchedulerConfig;
using spg::Size;
using spg::Timestamp;
using spg_test::at_ms;

constexpr double kEps = 1e-3;

// A planted secret: a box in a 1000x1000 reference frame, scaled to the
// source size, visible on frame times [from, to].
struct Secret {
  double x, y, w, h;
  std::int64_t from_ms, to_ms;
};

struct DoubleBox {
  double x0, y0, x1, y1;
};

DoubleBox scaled(const Rect& r, Size from, Size to) {
  const double sx = static_cast<double>(to.width) / from.width;
  const double sy = static_cast<double>(to.height) / from.height;
  return {r.x * sx, r.y * sy, (static_cast<double>(r.x) + r.width) * sx,
          (static_cast<double>(r.y) + r.height) * sy};
}

bool plan_covers(const spg::MaskPlan& plan, const DoubleBox& b) {
  return std::any_of(plan.rects.begin(), plan.rects.end(), [&](const spg::MaskRect& m) {
    const Rect& r = m.box.rect;
    return r.x <= b.x0 + kEps && r.y <= b.y0 + kEps && static_cast<double>(r.x) + r.width >= b.x1 - kEps &&
           static_cast<double>(r.y) + r.height >= b.y1 - kEps;
  });
}

// What the oracle remembers about a completed cycle.
struct Completion {
  std::int64_t t_ms;
  Size size;
  int segment;
  bool ok;
  std::uint64_t order;  // completion sequence number
  std::vector<Rect> boxes;
};

struct PushedFrame {
  std::int64_t t_ms;
  Size size;
  int segment;
  bool bad;
};

struct Pending {
  spg::ScanJob job;
  std::int64_t done_ms;  // when the "worker" returns; -1 never
  bool ok;
};

// Totals over every scene, so the run shows each path was exercised.
struct Totals {
  std::uint64_t clean = 0, fallback = 0, reemits = 0, late = 0, bad = 0, early = 0, retro_checks = 0,
                hold_checks = 0, resized_scenes = 0;
};

struct Scene {
  explicit Scene(std::uint32_t seed) : rng(seed) {}

  std::int64_t pick(std::int64_t lo, std::int64_t hi) {
    return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
  }
  bool chance(double p) { return std::bernoulli_distribution(p)(rng); }
  std::size_t index(std::size_t n) {
    return static_cast<std::size_t>(pick(0, static_cast<std::int64_t>(n) - 1));
  }

  std::mt19937 rng;
};

void run_scene(std::uint32_t seed, Totals& totals) {
  Scene s(seed);
  SchedulerConfig config;
  const std::int64_t lookahead_ms = std::vector<std::int64_t>{0, 30, 100, 250, 500}[s.index(5)];
  config.lookahead = std::chrono::milliseconds{lookahead_ms};
  const std::int64_t retro_ms = s.pick(0, lookahead_ms);
  config.retro = std::chrono::milliseconds{retro_ms};
  const std::int64_t hold_ms = std::vector<std::int64_t>{0, 100, 750}[s.index(3)];
  config.tracker.hold = std::chrono::milliseconds{hold_ms};
  config.tracker.margin_px = static_cast<float>(s.pick(0, 6));
  config.tracker.margin_ratio = s.chance(0.5) ? 0.25F : 0.0F;
  config.fallback = std::vector<FallbackMode>{FallbackMode::BlurSource, FallbackMode::HoldLastScanned,
                                              FallbackMode::SolidFill}[s.index(3)];
  config.queue_capacity = static_cast<std::size_t>(s.pick(1, 3));
  config.scan_interval = std::chrono::milliseconds{std::vector<std::int64_t>{0, 20, 50}[s.index(3)]};
  config.max_buffered_frames = static_cast<std::size_t>(s.pick(3, 80));
  config.max_retro_rects = static_cast<std::size_t>(s.pick(1, 8));
  config.release_grace = std::chrono::milliseconds{s.pick(0, 300)};
  const std::int64_t grace_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(config.release_grace).count();

  const auto dim = [&s](std::int64_t lo, std::int64_t hi) {
    return static_cast<std::uint32_t>(s.pick(lo, hi));
  };
  const Size size_a{dim(320, 1920), dim(240, 1080)};
  const Size size_b{dim(320, 2560), dim(240, 1440)};
  const int frames = 150;
  const int resize_at = s.chance(0.3) ? static_cast<int>(s.pick(10, frames - 10)) : -1;
  const int switch_at = s.chance(0.15) ? static_cast<int>(s.pick(10, frames - 10)) : -1;
  const bool bad_frames = s.chance(0.1);
  totals.resized_scenes += (resize_at >= 0 && size_a != size_b) ? 1 : 0;
  const double fail_rate = s.chance(0.5) ? 0.2 : 0.0;
  const double stall_rate = s.chance(0.3) ? 0.1 : 0.0;
  const std::int64_t max_work_ms = std::max<std::int64_t>(5, 2 * lookahead_ms);

  std::vector<Secret> secrets;
  const int n_secrets = static_cast<int>(s.pick(1, 4));
  for (int i = 0; i < n_secrets; ++i) {
    const std::int64_t from = 1000 + s.pick(0, 2000);
    secrets.push_back(Secret{static_cast<double>(s.pick(0, 900)), static_cast<double>(s.pick(0, 900)),
                             static_cast<double>(s.pick(4, 100)), static_cast<double>(s.pick(4, 60)), from,
                             from + s.pick(0, 1500)});
  }
  const auto detect = [&](std::int64_t t_ms, Size size) {
    std::vector<spg::Detection> out;
    for (const Secret& sec : secrets) {
      if (t_ms >= sec.from_ms && t_ms <= sec.to_ms) {
        const double sx = size.width / 1000.0;
        const double sy = size.height / 1000.0;
        out.push_back(spg_test::face_at(static_cast<float>(sec.x * sx), static_cast<float>(sec.y * sy),
                                        static_cast<float>(sec.w * sx), static_cast<float>(sec.h * sy)));
      }
    }
    return out;
  };

  spg::ManualClock clock{at_ms(1000)};
  spg::Lookahead la(clock, config);
  std::map<spg::FrameId, PushedFrame> pushed;
  std::vector<Pending> workers;
  std::vector<Completion> completions;
  std::uint64_t order = 0;
  std::map<spg::FrameId, int> clean_segment;  // frames released clean, by segment
  spg::FrameId last_released = 0;
  std::size_t released_count = 0;
  int segment = 0;
  Size size = size_a;
  std::int64_t now_ms = 1000;
  std::int64_t last_good_ms = -1;
  const auto readback = spg::OwnedFrame::copy(spg_test::TinyFrame(at_ms(0)).view());

  const auto finish_due = [&](bool all_ready) {
    // Return workers that are done, in random order.
    std::shuffle(workers.begin(), workers.end(), s.rng);
    for (auto it = workers.begin(); it != workers.end();) {
      if (it->done_ms >= 0 && (all_ready || it->done_ms <= now_ms)) {
        spg::ScanResult result;
        result.complete = it->ok;
        if (it->ok) {
          result.detections = detect(spg::to_ns(it->job.frame_time) / 1'000'000, it->job.source);
        }
        la.complete(it->job, result);
        const PushedFrame& pf = pushed.at(it->job.frame);
        Completion c{pf.t_ms, pf.size, pf.segment, it->ok, ++order, {}};
        for (const spg::Detection& d : result.detections) {
          c.boxes.push_back(d.box.rect);
        }
        completions.push_back(c);
        it = workers.erase(it);
      } else {
        ++it;
      }
    }
  };

  const auto release = [&] {
    std::vector<ReleasedFrame> out;
    la.release_due(out);
    for (const ReleasedFrame& r : out) {
      INFO("seed " << seed << " frame " << r.id);
      spg::validate(r.plan);
      REQUIRE(r.id > last_released);  // in push order, once each
      last_released = r.id;
      ++released_count;
      const PushedFrame& f = pushed.at(r.id);
      REQUIRE(r.plan.frame_time == at_ms(f.t_ms));
      REQUIRE(r.source == f.size);
      REQUIRE(r.plan.fallback == (r.action != FallbackAction::None));

      // Fail closed: released clean only with a complete cycle on the same
      // content within [t, t + retro], finished before now, on time, and
      // never before t + lookahead (unless the buffer was over its limit).
      if (r.action == FallbackAction::None) {
        REQUIRE_FALSE(f.bad);
        REQUIRE(now_ms <= f.t_ms + lookahead_ms + grace_ms);
        const bool covered = std::any_of(completions.begin(), completions.end(), [&](const Completion& c) {
          return c.ok && c.segment == f.segment && c.t_ms >= f.t_ms && c.t_ms <= f.t_ms + retro_ms;
        });
        REQUIRE(covered);
        clean_segment[r.id] = f.segment;
        ++totals.clean;
      } else {
        ++totals.fallback;
        totals.late += r.reason == spg::FallbackReason::LateRelease ? 1 : 0;
        totals.bad += r.reason == spg::FallbackReason::BadTimestamp ? 1 : 0;
        totals.reemits += r.action == FallbackAction::HoldLastScanned ? 1 : 0;
        switch (config.fallback) {
          case FallbackMode::BlurSource:
            REQUIRE(r.action == FallbackAction::BlurSource);
            break;
          case FallbackMode::SolidFill:
            REQUIRE(r.action == FallbackAction::SolidFill);
            break;
          case FallbackMode::HoldLastScanned:
            if (r.action == FallbackAction::HoldLastScanned) {
              // The latest clean frame, of the same content.
              REQUIRE_FALSE(clean_segment.empty());
              REQUIRE(r.reemit == clean_segment.rbegin()->first);
              REQUIRE(clean_segment.rbegin()->second == f.segment);
            } else {
              REQUIRE(r.action == FallbackAction::BlurSource);
              REQUIRE((clean_segment.empty() || clean_segment.rbegin()->second != f.segment));
            }
            break;
        }
      }
      if (now_ms < f.t_ms + lookahead_ms && !f.bad) {
        REQUIRE(la.stats().early_releases > 0);
        ++totals.early;
      }

      // Masks, for any frame released on time: every box a complete cycle
      // found within the retro window after the frame, or within the hold
      // before it, is covered (scaled across a resize).
      if (f.bad || now_ms > f.t_ms + lookahead_ms + grace_ms) {
        continue;
      }
      for (const Completion& c : completions) {
        if (!c.ok) {
          continue;
        }
        const bool retro = c.t_ms >= f.t_ms && c.t_ms <= f.t_ms + retro_ms;
        const bool held = c.t_ms <= f.t_ms && f.t_ms <= c.t_ms + hold_ms;
        if (!retro && !held) {
          continue;
        }
        for (const Rect& b : c.boxes) {
          INFO("cycle at " << c.t_ms << " frame at " << f.t_ms);
          REQUIRE(plan_covers(r.plan, scaled(b, c.size, f.size)));
          ++(retro && c.t_ms > f.t_ms ? totals.retro_checks : totals.hold_checks);
        }
      }
    }
  };

  for (int i = 0; i < frames; ++i) {
    // Frame interval with jitter, and an occasional render hiccup.
    now_ms += s.chance(0.03) ? s.pick(100, 1500) : s.pick(8, 25);
    clock.set(at_ms(now_ms));
    if (i == resize_at && size_b != size) {
      size = size_b;
      ++segment;  // a resize is new content
    }
    if (i == switch_at) {
      la.mark_discontinuity();
      ++segment;
    }
    finish_due(false);
    std::int64_t t_ms = now_ms;
    bool bad = false;
    if (bad_frames && s.chance(0.05)) {
      t_ms = s.chance(0.5) ? now_ms + 5000 : (last_good_ms >= 0 ? last_good_ms : now_ms - 5000);
      bad = true;
    }
    const bool scan = s.chance(0.7);
    const spg::FrameId id = la.push(at_ms(t_ms), size, scan ? readback : nullptr);
    pushed[id] = PushedFrame{t_ms, size, segment, bad};
    if (!bad) {
      last_good_ms = t_ms;
    }
    // Idle workers take jobs, possibly several frames later.
    while (workers.size() < 3 && la.queued() > 0 && s.chance(0.6)) {
      std::optional<spg::ScanJob> job = la.take_job();
      REQUIRE(job.has_value());
      const bool stall = s.chance(stall_rate);
      workers.push_back(Pending{*job, stall ? -1 : now_ms + s.pick(0, max_work_ms), !s.chance(fail_rate)});
    }
    finish_due(false);
    release();
  }
  // Drain: stalled cycles never return; everything else does, then every
  // frame is released.
  for (int k = 0; k < 4; ++k) {
    now_ms += lookahead_ms + 10;
    clock.set(at_ms(now_ms));
    finish_due(false);
    release();
  }
  finish_due(true);
  now_ms += 10000;
  clock.set(at_ms(now_ms));
  release();
  REQUIRE(released_count == pushed.size());
  REQUIRE(la.buffered() == 0);
}

}  // namespace

TEST_CASE("no frame is released without a complete scan or the fallback, and masks cover retro and hold",
          "[schedule][property]") {
  Totals totals;
  for (std::uint32_t seed = 1; seed <= 1500; ++seed) {
    run_scene(seed, totals);
  }
  // Every path the properties speak about was taken.
  CHECK(totals.clean > 10000);
  CHECK(totals.fallback > 10000);
  CHECK(totals.reemits > 100);
  CHECK(totals.late > 0);
  CHECK(totals.bad > 0);
  CHECK(totals.early > 0);
  CHECK(totals.retro_checks > 1000);
  CHECK(totals.hold_checks > 1000);
  CHECK(totals.resized_scenes > 100);
  UNSCOPED_INFO("clean " << totals.clean << " fallback " << totals.fallback << " reemits " << totals.reemits
                         << " late " << totals.late << " bad " << totals.bad << " early " << totals.early
                         << " retro " << totals.retro_checks << " hold " << totals.hold_checks);
  CHECK(totals.clean > 0);
}
