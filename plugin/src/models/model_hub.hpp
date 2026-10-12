#pragma once

// Loads the detection models once per module, on a thread of its own
// (SPG-14b), without OBS. Opening and warming up the models takes from
// hundreds of milliseconds to seconds, so it never runs on the graphics
// thread (render, video tick, update) or the UI thread: start() only starts
// the thread, and a filter keeps covering every frame whole until the models
// are ready. When loading ends (ready or not) every subscribed listener is
// called on the loader thread, so each filter can ask for a scheduler with
// the models (built on the filter's build thread, SPG-14d). The reporter is told the
// outcome exactly once, with a reason even when the loader threw or its
// thread could not be started, so the log always says why detection is off.
//
// One attempt per module load: after a failure the status stays until OBS
// restarts (the properties say so).

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "models/model_set.hpp"

namespace spg::plugin {

class ModelHub {
 public:
  using Loader = std::function<LoadResult()>;
  using Listener = std::function<void()>;
  // Called once per start() with the final report and how long loading took:
  // on the loader thread before the state changes, or on start()'s caller
  // when the thread cannot be started. Failures the hub detects itself carry
  // a fixed reason in report.detail.
  using Reporter = std::function<void(const ModelReport& report, std::chrono::milliseconds elapsed)>;

  explicit ModelHub(Loader loader, Reporter reporter = {})
      : loader_(std::move(loader)), reporter_(std::move(reporter)) {}
  ~ModelHub();  // shutdown()
  ModelHub(const ModelHub&) = delete;
  ModelHub& operator=(const ModelHub&) = delete;

  // Any thread; returns at once. The first call starts the loader thread;
  // later calls, and calls after shutdown(), do nothing. If the thread cannot
  // be started the status becomes Failed.
  void start() noexcept;

  struct Snapshot {
    std::shared_ptr<const LoadedModels> models;  // null unless Ready
    ModelReport report;
    // Changes whenever the models or the status change.
    std::uint64_t generation = 0;
  };
  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] ModelStatus status() const noexcept;

  // Called on the loader thread when loading ends. Returns an id for
  // unsubscribe(). A listener must not call subscribe() or unsubscribe().
  [[nodiscard]] std::uint64_t subscribe(Listener listener);
  // After it returns the listener is never called again; waits for a call
  // to it in progress.
  void unsubscribe(std::uint64_t id) noexcept;

  // Waits for the loader thread (loading cannot be interrupted), then drops
  // the hub's models and listeners. Stages built from the models keep them
  // alive until they are destroyed. Idempotent.
  void shutdown() noexcept;

 private:
  void run() noexcept;
  void finish(LoadResult result, std::chrono::milliseconds elapsed) noexcept;
  void report(const ModelReport& report, std::chrono::milliseconds elapsed) const noexcept;

  Loader loader_;
  Reporter reporter_;

  mutable std::mutex state_mutex_;  // guards the fields below
  std::shared_ptr<const LoadedModels> models_;
  ModelReport report_;
  std::uint64_t generation_ = 0;
  bool started_ = false;
  bool shut_down_ = false;
  std::thread thread_;

  std::mutex listeners_mutex_;  // held while listeners run
  std::vector<std::pair<std::uint64_t, Listener>> listeners_;
  std::uint64_t next_listener_ = 1;
};

}  // namespace spg::plugin
