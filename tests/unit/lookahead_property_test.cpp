// Scheduler, lookahead and fail-closed release (SPG-10): seeded property
// tests. Each scene is a deterministic simulation on a ManualClock: frames at
// a jittered rate, static secrets that appear and disappear, cycles that start
// late, take random time, fail, stall forever or return after their frames
// were released, random cadence and queue limits, render hiccups, a resize in
// some scenes and a content switch in others, a source that flaps between two
// sizes faster than the size history, results that cannot be applied, and
// failed cycles that still return the boxes their finished stages found
// (SPG-10c: those are held and masked, but cover no frame). An oracle kept by
// the test, not the scheduler, checks every released frame and every frame
// re-emitted by hold-last-scanned (SPG-10b: every path that airs a frame
// passes the same gate).
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

// The scheduler remembers this many source sizes (docs/scheduler.md); a
// result from before more resizes than that cannot be scaled and applied.
constexpr int kSizeHistory = 64;

// What the oracle remembers about a completed cycle.
struct Completion {
  std::int64_t t_ms;
  Size size;
  int segment;
  bool ok;              // the cycle was complete
  bool held;            // its boxes go to the scheduler: complete, or failed with partial boxes
  bool applied;         // held, and they could be applied (not rejected, not too old)
  std::uint64_t order;  // completion sequence number
  std::vector<Rect> boxes;
};

struct PushedFrame {
  std::int64_t t_ms;
  Size size;
  int segment;
  int resizes;  // source size changes before this frame
  bool bad;
};

// A frame released clean, as it aired: what a re-emit shows again.
struct CleanFrame {
  spg::FrameId id;
  std::int64_t t_ms;
  Size size;
  int segment;
  spg::MaskPlan plan;
  std::uint64_t released_after;  // completions finished before its release
};

struct Pending {
  spg::ScanJob job;
  std::int64_t done_ms;  // when the "worker" returns; -1 never
  bool ok;
  bool partial;  // failed after some stages finished: returns some of the boxes
};

// Totals over every scene, so the run shows each path was exercised.
struct Totals {
  std::uint64_t clean = 0, fallback = 0, reemits = 0, late = 0, bad = 0, early = 0, retro_checks = 0,
                hold_checks = 0, resized_scenes = 0, lagged_scans = 0, not_scanned = 0, apply_failed = 0,
                unapplied_rejected = 0, unapplied_too_old = 0, reemit_checks = 0, reemits_refused = 0,
                switched_scenes = 0, flapping_scenes = 0, partial_cycles = 0, partial_unapplied = 0,
                partial_mask_checks = 0, partial_reemit_checks = 0;
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
  // A source that changes size on every frame for a while (a window being
  // dragged), more often than the size history holds.
  const int flap_at = s.chance(0.1) ? static_cast<int>(s.pick(5, 60)) : -1;
  const int flap_frames = kSizeHistory + 6;
  // Complete results the scheduler cannot apply (the tracker rejects a box),
  // the stand-in for an allocation or tracker failure in complete().
  const double reject_rate = s.chance(0.3) ? 0.08 : 0.0;
  // Readbacks that lag one frame, as a mapped stage surface does.
  const bool lagged_readbacks = s.chance(0.3);
  if (resize_at >= 0 && size_a != size_b) {
    ++totals.resized_scenes;
  }
  if (switch_at >= 0) {
    ++totals.switched_scenes;
  }
  if (flap_at >= 0 && size_a != size_b) {
    ++totals.flapping_scenes;
  }
  const double fail_rate = s.chance(0.5) ? 0.2 : 0.0;
  // Of the failed cycles, the share that fail after some stages finished
  // (faces found, then the text detector throws) and return those boxes.
  const double partial_rate = s.chance(0.6) ? 0.7 : 0.0;
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
  std::vector<CleanFrame> clean_frames;  // in release order
  spg::FrameId last_released = 0;
  std::size_t released_count = 0;
  int segment = 0;
  int resizes = 0;         // size changes so far in the scene
  int pushed_resizes = 0;  // of those, the ones the scheduler has been pushed
  Size size = size_a;
  std::int64_t now_ms = 1000;
  std::int64_t last_good_ms = -1;

  const auto finish_due = [&](bool all_ready) {
    // Return workers that are done, in random order.
    std::shuffle(workers.begin(), workers.end(), s.rng);
    for (auto it = workers.begin(); it != workers.end();) {
      if (it->done_ms >= 0 && (all_ready || it->done_ms <= now_ms)) {
        spg::ScanResult result;
        result.complete = it->ok;
        if (it->ok || it->partial) {
          result.detections = detect(spg::to_ns(it->job.frame_time) / 1'000'000, it->job.source);
        }
        if (it->partial) {
          // The boxes of the stages that finished before the failure: a
          // random subset, possibly none.
          std::erase_if(result.detections, [&s](const spg::Detection&) { return s.chance(0.4); });
        }
        const PushedFrame& pf = pushed.at(it->job.frame);
        Completion c{pf.t_ms, pf.size, pf.segment, it->ok, false, false, ++order, {}};
        for (const spg::Detection& d : result.detections) {
          c.boxes.push_back(d.box.rect);
        }
        if ((it->ok || it->partial) && s.chance(reject_rate)) {
          // A score the tracker rejects (validate()); a resize cannot repair it.
          spg::Detection rejected = spg_test::face_at(1, 1, 4, 4);
          rejected.score = 2.0F;
          result.detections.push_back(rejected);
          c.held = true;
          ++(it->ok ? totals.unapplied_rejected : totals.partial_unapplied);
        } else {
          // The scheduler takes the boxes of a complete cycle, and of a
          // failed one that found any.
          c.held = it->ok || !result.detections.empty();
          c.applied = c.held;
          if (c.held && pushed_resizes - pf.resizes >= kSizeHistory) {
            c.applied = false;
            ++(it->ok ? totals.unapplied_too_old : totals.partial_unapplied);
          }
        }
        totals.partial_cycles += !it->ok && c.applied ? 1U : 0U;
        la.complete(it->job, result);
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
          return c.ok && c.applied && c.segment == f.segment && c.t_ms >= f.t_ms &&
                 c.t_ms <= f.t_ms + retro_ms;
        });
        REQUIRE(covered);
        clean_frames.push_back(CleanFrame{r.id, f.t_ms, f.size, f.segment, r.plan, order});
        ++totals.clean;
      } else {
        ++totals.fallback;
        totals.late += r.reason == spg::FallbackReason::LateRelease ? 1U : 0U;
        totals.bad += r.reason == spg::FallbackReason::BadTimestamp ? 1U : 0U;
        totals.not_scanned += r.reason == spg::FallbackReason::NotScanned ? 1U : 0U;
        totals.apply_failed += r.reason == spg::FallbackReason::ApplyFailed ? 1U : 0U;
        totals.reemits += r.action == FallbackAction::HoldLastScanned ? 1U : 0U;
        switch (config.fallback) {
          case FallbackMode::BlurSource:
            REQUIRE(r.action == FallbackAction::BlurSource);
            break;
          case FallbackMode::SolidFill:
            REQUIRE(r.action == FallbackAction::SolidFill);
            break;
          case FallbackMode::HoldLastScanned: {
            // The latest clean frame, of the same content, unless a result
            // that finished after it aired could have masked it: it is shown
            // again exactly as it aired, so it would air without those boxes.
            const CleanFrame* latest = clean_frames.empty() ? nullptr : &clean_frames.back();
            const bool spoiled =
                latest != nullptr &&
                std::any_of(completions.begin(), completions.end(), [&](const Completion& c) {
                  if (!c.held || c.order <= latest->released_after) {
                    return false;
                  }
                  if (!c.applied) {
                    return latest->t_ms >= c.t_ms - retro_ms;  // as for buffered frames
                  }
                  return !c.boxes.empty() && latest->t_ms >= c.t_ms - retro_ms &&
                         latest->t_ms <= c.t_ms + hold_ms;
                });
            if (r.action == FallbackAction::HoldLastScanned) {
              REQUIRE(latest != nullptr);
              REQUIRE(r.reemit == latest->id);
              REQUIRE(latest->segment == f.segment);
              // What airs is the re-emitted frame as it was released: it must
              // cover every box of every cycle finished by now (complete, or
              // failed with partial boxes) within the retro window after it or
              // the hold before it.
              for (const Completion& c : completions) {
                if (!c.held) {
                  continue;
                }
                const bool retro = c.t_ms >= latest->t_ms && c.t_ms <= latest->t_ms + retro_ms;
                const bool in_hold = c.t_ms <= latest->t_ms && latest->t_ms <= c.t_ms + hold_ms;
                if (!retro && !in_hold) {
                  continue;
                }
                for (const Rect& b : c.boxes) {
                  INFO("re-emitted frame at " << latest->t_ms << ", cycle at " << c.t_ms);
                  REQUIRE(plan_covers(latest->plan, scaled(b, c.size, latest->size)));
                  ++(c.ok ? totals.reemit_checks : totals.partial_reemit_checks);
                }
              }
              REQUIRE_FALSE(spoiled);
            } else {
              REQUIRE(r.action == FallbackAction::BlurSource);
              REQUIRE((latest == nullptr || latest->segment != f.segment || spoiled));
              totals.reemits_refused += latest != nullptr && latest->segment == f.segment ? 1U : 0U;
            }
            break;
          }
        }
      }
      if (now_ms < f.t_ms + lookahead_ms && !f.bad) {
        REQUIRE(la.stats().early_releases > 0);
        ++totals.early;
      }

      // Masks, for any frame released on time: every box a cycle found
      // (complete, or failed with partial boxes; SPG-10c) within the retro
      // window after the frame, or within the hold before it, is covered
      // (scaled across a resize).
      if (f.bad || now_ms > f.t_ms + lookahead_ms + grace_ms) {
        continue;
      }
      for (const Completion& c : completions) {
        if (!c.held) {
          continue;
        }
        const bool retro = c.t_ms >= f.t_ms && c.t_ms <= f.t_ms + retro_ms;
        const bool in_hold = c.t_ms <= f.t_ms && f.t_ms <= c.t_ms + hold_ms;
        if (!retro && !in_hold) {
          continue;
        }
        if (!c.applied && r.action != FallbackAction::None) {
          continue;  // in no plan; the fallback covers the whole frame
        }
        for (const Rect& b : c.boxes) {
          INFO("cycle at " << c.t_ms << " frame at " << f.t_ms << (c.ok ? "" : " (partial)"));
          REQUIRE(plan_covers(r.plan, scaled(b, c.size, f.size)));
          if (c.ok) {
            ++(retro && c.t_ms > f.t_ms ? totals.retro_checks : totals.hold_checks);
          } else {
            ++totals.partial_mask_checks;
          }
        }
      }
    }
  };

  for (int i = 0; i < frames; ++i) {
    const bool flapping = flap_at >= 0 && i >= flap_at && i < flap_at + flap_frames;
    // Frame interval with jitter, and an occasional render hiccup. A flapping
    // source sends frames fast, so cycles stay in flight across many resizes.
    now_ms += flapping ? s.pick(1, 3) : (s.chance(0.03) ? s.pick(100, 1500) : s.pick(8, 25));
    clock.set(at_ms(now_ms));
    // A resize is new content. The scheduler only sees the size it is
    // pushed, so a change counts once per frame and only if that size
    // differs from the last one: the resize landing on a flapping frame that
    // toggles straight back leaves the source as it was.
    const Size before = size;
    if (i == resize_at) {
      size = size_b;
    }
    if (flapping && size_a != size_b) {
      size = size == size_a ? size_b : size_a;
    }
    if (size != before) {
      ++segment;
      ++resizes;
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
    std::shared_ptr<const spg::OwnedFrame> readback;
    const std::int64_t shown_ms = lagged_readbacks && last_good_ms >= 0 ? last_good_ms : t_ms;
    if (s.chance(0.7)) {
      readback = spg::OwnedFrame::copy(spg_test::TinyFrame(at_ms(shown_ms)).view());
    }
    const std::uint64_t queued_before = la.stats().scans_queued;
    const spg::FrameId id = la.push(at_ms(t_ms), size, readback);
    if (la.stats().scans_queued != queued_before && shown_ms != t_ms) {
      ++totals.lagged_scans;  // queued for the earlier frame the readback shows
    }
    pushed[id] = PushedFrame{t_ms, size, segment, resizes, bad};
    pushed_resizes = resizes;
    if (!bad) {
      last_good_ms = t_ms;
    }
    // Idle workers take jobs, possibly several frames later.
    while (workers.size() < 3 && la.queued() > 0 && s.chance(0.6)) {
      std::optional<spg::ScanJob> job = la.take_job();
      REQUIRE(job.has_value());
      const bool stall = s.chance(stall_rate);
      const std::int64_t done_ms = stall ? -1 : now_ms + s.pick(0, max_work_ms);
      const bool ok = !s.chance(fail_rate);
      const bool partial = !ok && s.chance(partial_rate);
      workers.push_back(Pending{*job, done_ms, ok, partial});
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
  CHECK(totals.lagged_scans > 1000);
  // SPG-10b: every path that airs a frame, or refuses to, was taken.
  CHECK(totals.not_scanned > 1000);
  CHECK(totals.apply_failed > 100);
  CHECK(totals.unapplied_rejected > 100);
  CHECK(totals.unapplied_too_old > 0);
  CHECK(totals.reemit_checks > 100);
  CHECK(totals.reemits_refused > 0);
  CHECK(totals.switched_scenes > 100);
  CHECK(totals.flapping_scenes > 100);
  // SPG-10c: failed cycles with partial boxes, held and checked like any box.
  CHECK(totals.partial_cycles > 200);
  CHECK(totals.partial_unapplied > 10);
  CHECK(totals.partial_mask_checks > 1000);
  CHECK(totals.partial_reemit_checks > 100);
  UNSCOPED_INFO("clean " << totals.clean << " fallback " << totals.fallback << " reemits " << totals.reemits
                         << " late " << totals.late << " bad " << totals.bad << " early " << totals.early
                         << " retro " << totals.retro_checks << " hold " << totals.hold_checks
                         << " not_scanned " << totals.not_scanned << " apply_failed " << totals.apply_failed
                         << " unapplied " << totals.unapplied_rejected << "/" << totals.unapplied_too_old
                         << " reemit_checks " << totals.reemit_checks << " refused " << totals.reemits_refused
                         << " partial " << totals.partial_cycles << "/" << totals.partial_unapplied << " checks "
                         << totals.partial_mask_checks << "/" << totals.partial_reemit_checks);
  CHECK(totals.clean > 0);
}
