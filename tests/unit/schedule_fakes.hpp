#pragma once

// Fakes for the scheduler tests (SPG-10): tiny synthetic frames, scripted
// detectors, recognizers and policies, and a clock several threads can read.
// Nothing here touches a model, the disk or the network.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "spg/detection.hpp"
#include "spg/frame.hpp"
#include "spg/geometry.hpp"
#include "spg/scan.hpp"
#include "spg/time.hpp"

namespace spg_test {

inline spg::Timestamp at_ms(std::int64_t ms) {
  return spg::timestamp_from_ns(ms * 1'000'000);
}

inline spg::Detection face_at(float x, float y, float w, float h) {
  return spg::Detection{spg::Box{spg::Rect{x, y, w, h}, spg::Space::Source}, spg::DetectionKind::Face,
                        0.9F, spg::DetectorId::Synthetic};
}

inline spg::Detection text_at(float x, float y, float w, float h) {
  spg::Detection d = face_at(x, y, w, h);
  d.kind = spg::DetectionKind::TextRegion;
  return d;
}

// A small BGRA readback frame. The fakes read the scene time from its
// timestamp, which is how a scripted detector knows what is on screen.
class TinyFrame {
 public:
  explicit TinyFrame(spg::Timestamp t) : pixels_(8 * 8 * 4, 0x40) {
    view_.planes[0] = pixels_.data();
    view_.strides[0] = 8 * 4;
    view_.width = 8;
    view_.height = 8;
    view_.format = spg::PixelFormat::BGRA;
    view_.timestamp = t;
  }
  [[nodiscard]] const spg::FrameView& view() const noexcept { return view_; }

 private:
  std::vector<std::uint8_t> pixels_;
  spg::FrameView view_;
};

// A clock tests can move while workers read it.
class AtomicClock final : public spg::Clock {
 public:
  explicit AtomicClock(spg::Timestamp start) : ns_(spg::to_ns(start)) {}
  [[nodiscard]] spg::Timestamp now() const noexcept override {
    return spg::timestamp_from_ns(ns_.load(std::memory_order_acquire));
  }
  void set(spg::Timestamp t) noexcept { ns_.store(spg::to_ns(t), std::memory_order_release); }

 private:
  std::atomic<std::int64_t> ns_;
};

// A gate a fake stage can wait on, so a test decides when a stalled stage
// returns.
class Gate {
 public:
  void open() {
    {
      const std::lock_guard lock(mutex_);
      open_ = true;
    }
    cv_.notify_all();
  }
  void wait() {
    std::unique_lock lock(mutex_);
    ++waiting_;
    cv_.notify_all();
    cv_.wait(lock, [this] { return open_; });
    --waiting_;
  }
  // True once some caller is blocked in wait(), within `timeout` of real time.
  bool wait_for_waiter(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return waiting_ > 0 || open_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
  int waiting_ = 0;
};

// An exception that is not a std::exception, as a third-party stage might
// throw.
struct NotAStdException {
  int code = 0;
};

// A detector whose output is scripted by scene time (the readback timestamp)
// and source size. `fail` decides when it throws instead; `gate`, when set,
// makes every call wait for it first.
class ScriptedDetector final : public spg::Detector {
 public:
  using Script = std::function<std::vector<spg::Detection>(spg::Timestamp, spg::Size)>;
  using Fail = std::function<void(spg::Timestamp)>;  // throws to fail

  // The detector shares ownership of `gate`, so a worker the scheduler
  // detached can still wait on it after the test's own reference is gone.
  explicit ScriptedDetector(Script script, Fail fail = {}, std::shared_ptr<Gate> gate = nullptr)
      : script_(std::move(script)), fail_(std::move(fail)), gate_(std::move(gate)) {}

  [[nodiscard]] std::vector<spg::Detection> detect(const spg::FrameView& readback,
                                                   spg::Size source) const override {
    calls.fetch_add(1);
    if (gate_ != nullptr) {
      gate_->wait();
    }
    if (fail_) {
      fail_(readback.timestamp);
    }
    return script_(readback.timestamp, source);
  }

  mutable std::atomic<int> calls{0};

 private:
  Script script_;
  Fail fail_;
  std::shared_ptr<Gate> gate_;
};

// Reads text by region: `text_for(region)`.
class ScriptedRecognizer final : public spg::TextRecognizer {
 public:
  using Script = std::function<std::string(const spg::Detection&)>;
  explicit ScriptedRecognizer(Script script) : script_(std::move(script)) {}
  [[nodiscard]] std::vector<spg::Secret> recognize(const spg::FrameView&, spg::Size,
                                                   std::span<const spg::Detection> regions) const override {
    calls.fetch_add(1);
    std::vector<spg::Secret> out;
    for (const spg::Detection& r : regions) {
      out.emplace_back(script_(r));
    }
    return out;
  }
  mutable std::atomic<int> calls{0};

 private:
  Script script_;
};

// Sensitive when the text contains "SECRET". `on_evaluate` runs first (to
// stall, throw or move a clock).
class ScriptedPolicy final : public spg::TextPolicy {
 public:
  explicit ScriptedPolicy(std::function<void()> on_evaluate = {}) : on_evaluate_(std::move(on_evaluate)) {}
  [[nodiscard]] spg::Verdict evaluate(const spg::Secret& text) const override {
    calls.fetch_add(1);
    if (on_evaluate_) {
      on_evaluate_();
    }
    spg::Verdict v;
    if (text.reveal().find("SECRET") != std::string::npos) {
      v.reasons.insert(spg::Category::Watchlist);
    }
    return v;
  }
  mutable std::atomic<int> calls{0};

 private:
  std::function<void()> on_evaluate_;
};

}  // namespace spg_test
