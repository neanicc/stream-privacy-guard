#include "model_loading.hpp"

#include <obs-module.h>

#include <chrono>
#include <filesystem>
#include <vector>

#include "plugin-support.hpp"
#include "settings/watchlist_store.hpp"

namespace spg::plugin {

namespace {

// Runs on the hub's thread. Paths only; nothing here sees a frame.
LoadResult load_for_module() {
  std::vector<std::filesystem::path> dirs;
  if (char* packaged = obs_module_file("models"); packaged != nullptr) {
    dirs.push_back(path_from_utf8(packaged));
    bfree(packaged);
  }
  dirs.push_back(default_models_dir());
  return load_models(dirs);
}

// The hub calls this exactly once per load, whatever happened (the loader
// threw, its thread could not start): one line saying what loaded and on
// which execution providers, or why detection is off. Paths, model names,
// runtime messages and fixed reasons only.
void log_models(const ModelReport& report, std::chrono::milliseconds elapsed) {
  const auto ms = static_cast<long long>(elapsed.count());
  if (report.status == ModelStatus::Ready) {
    log(LOG_INFO, "detection models loaded in %lld ms (%s)", ms, report.detail.c_str());
  } else {
    log(LOG_WARNING, "detection models %s, covering every frame whole: %s: %s", status_name(report.status),
        report.model.empty() ? "-" : report.model.c_str(), report.detail.c_str());
  }
}

}  // namespace

ModelHub& module_models() noexcept {
  static ModelHub hub(load_for_module, log_models);
  return hub;
}

}  // namespace spg::plugin
