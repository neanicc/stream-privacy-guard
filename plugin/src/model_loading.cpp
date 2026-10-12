#include "model_loading.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <filesystem>
#include <vector>

#include "plugin-support.hpp"
#include "settings/watchlist_store.hpp"

namespace spg::plugin {

namespace {

// Runs on the hub's thread. Logs exactly one line: what loaded and on which
// execution providers, or why detection is off. Paths and runtime messages
// only; nothing here sees a frame.
LoadResult load_for_module() {
  const std::uint64_t started = os_gettime_ns();
  std::vector<std::filesystem::path> dirs;
  if (char* packaged = obs_module_file("models"); packaged != nullptr) {
    dirs.push_back(path_from_utf8(packaged));
    bfree(packaged);
  }
  dirs.push_back(default_models_dir());
  LoadResult result = load_models(dirs);
  const auto ms = static_cast<long long>((os_gettime_ns() - started) / 1'000'000ULL);
  if (result.report.status == ModelStatus::Ready) {
    log(LOG_INFO, "detection models loaded in %lld ms (%s)", ms, result.report.detail.c_str());
  } else {
    log(LOG_WARNING, "detection models %s, covering every frame whole: %s: %s", status_name(result.report.status),
        result.report.model.c_str(), result.report.detail.c_str());
  }
  return result;
}

}  // namespace

ModelHub& module_models() noexcept {
  static ModelHub hub(load_for_module);
  return hub;
}

}  // namespace spg::plugin
