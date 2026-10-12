#include "models/model_hub.hpp"

#include <algorithm>
#include <exception>

namespace spg::plugin {

ModelHub::~ModelHub() {
  shutdown();
}

void ModelHub::start() noexcept {
  const std::lock_guard lock(state_mutex_);
  if (started_ || shut_down_) {
    return;
  }
  started_ = true;
  report_.status = ModelStatus::Loading;
  ++generation_;
  try {
    thread_ = std::thread([this] { run(); });
  } catch (...) {
    report_.status = ModelStatus::Failed;
    report_.model.clear();
    report_.detail.clear();
    ++generation_;
  }
}

void ModelHub::run() noexcept {
  LoadResult result;
  try {
    result = loader_();
    if (result.report.status == ModelStatus::Ready && result.models == nullptr) {
      result.report.status = ModelStatus::Failed;  // never claim Ready without models
    }
  } catch (...) {
    result.models.reset();
    result.report.status = ModelStatus::Failed;
  }
  finish(std::move(result));
}

void ModelHub::finish(LoadResult result) noexcept {
  {
    const std::lock_guard lock(state_mutex_);
    if (result.report.status != ModelStatus::Ready) {
      result.models.reset();
    }
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
