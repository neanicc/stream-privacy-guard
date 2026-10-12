#include "models/model_hub.hpp"

#include <algorithm>
#include <exception>
#include <new>

namespace spg::plugin {

ModelHub::~ModelHub() {
  shutdown();
}

namespace {

// Fixed reasons for failures the hub detects itself.
constexpr const char* kThreadFailed = "the model loader thread could not be started";
constexpr const char* kOutOfMemory = "out of memory while loading the models";
constexpr const char* kLoaderThrew = "the model loader failed with an unexpected error";
constexpr const char* kReadyWithoutModels = "the model loader reported ready without models";

}  // namespace

void ModelHub::start() noexcept {
  ModelReport failed;
  {
    const std::lock_guard lock(state_mutex_);
    if (started_ || shut_down_) {
      return;
    }
    started_ = true;
    report_.status = ModelStatus::Loading;
    ++generation_;
    try {
      thread_ = std::thread([this] { run(); });
      return;
    } catch (...) {
      report_.status = ModelStatus::Failed;
      report_.model.clear();
      report_.detail.clear();
      ++generation_;
    }
  }
  failed.status = ModelStatus::Failed;
  try {
    failed.detail = kThreadFailed;
  } catch (...) {
    // No memory for the reason: the status still says Failed.
  }
  report(failed, std::chrono::milliseconds{0});
}

void ModelHub::run() noexcept {
  const auto started = std::chrono::steady_clock::now();
  LoadResult result;
  const char* reason = nullptr;
  try {
    result = loader_();
    if (result.report.status == ModelStatus::Ready && result.models == nullptr) {
      reason = kReadyWithoutModels;  // never claim Ready without models
    }
  } catch (const std::bad_alloc&) {
    reason = kOutOfMemory;
  } catch (...) {
    reason = kLoaderThrew;
  }
  if (reason != nullptr) {
    result.models.reset();
    result.report.status = ModelStatus::Failed;
    result.report.model.clear();
    result.report.detail.clear();
    try {
      result.report.detail = reason;
    } catch (...) {
      // No memory for the reason: the status still says Failed.
    }
  }
  finish(std::move(result),
         std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started));
}

void ModelHub::report(const ModelReport& report, std::chrono::milliseconds elapsed) const noexcept {
  if (!reporter_) {
    return;
  }
  try {
    reporter_(report, elapsed);
  } catch (...) {
    // A reporter that fails costs the log line, nothing else.
  }
}

void ModelHub::finish(LoadResult result, std::chrono::milliseconds elapsed) noexcept {
  if (result.report.status != ModelStatus::Ready) {
    result.models.reset();
  }
  report(result.report, elapsed);
  {
    const std::lock_guard lock(state_mutex_);
    models_ = std::move(result.models);
    report_ = std::move(result.report);
    ++generation_;
  }
  // The new state is visible before any listener runs, so a filter that
  // publishes in between already sees it.
  const std::lock_guard lock(listeners_mutex_);
  for (const auto& entry : listeners_) {
    try {
      entry.second();
    } catch (...) {
      // A listener that fails leaves its filter covering every frame.
    }
  }
}

ModelHub::Snapshot ModelHub::snapshot() const {
  const std::lock_guard lock(state_mutex_);
  return {models_, report_, generation_};
}

ModelStatus ModelHub::status() const noexcept {
  const std::lock_guard lock(state_mutex_);
  return report_.status;
}

std::uint64_t ModelHub::subscribe(Listener listener) {
  const std::lock_guard lock(listeners_mutex_);
  const std::uint64_t id = next_listener_++;
  listeners_.emplace_back(id, std::move(listener));
  return id;
}

void ModelHub::unsubscribe(std::uint64_t id) noexcept {
  const std::lock_guard lock(listeners_mutex_);
  std::erase_if(listeners_, [id](const auto& entry) { return entry.first == id; });
}

void ModelHub::shutdown() noexcept {
  std::thread thread;
  {
    const std::lock_guard lock(state_mutex_);
    shut_down_ = true;
    thread = std::move(thread_);
  }
  if (thread.joinable()) {
    thread.join();
  }
  std::shared_ptr<const LoadedModels> models;
  {
    const std::lock_guard lock(state_mutex_);
    models = std::move(models_);
    if (report_.status == ModelStatus::Ready || report_.status == ModelStatus::Loading) {
      report_.status = ModelStatus::Idle;
      ++generation_;
    }
  }
  const std::lock_guard lock(listeners_mutex_);
  listeners_.clear();
}

}  // namespace spg::plugin
