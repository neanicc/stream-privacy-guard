// obs-spg: the OBS adapter for Stream Privacy Guard. It registers the filter;
// everything that decides what to blur lives in spg-core.

#include <obs-module.h>

#include "pipeline/reaper.hpp"
#include "plugin-support.hpp"
#include "spg-filter.hpp"
#include "spg/version.hpp"

#include <string_view>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-spg", "en-US")

MODULE_EXPORT const char *obs_module_name(void) {
  return "Stream Privacy Guard";
}

MODULE_EXPORT const char *obs_module_description(void) {
  return obs_module_text("SPG.Description");
}

bool obs_module_load(void) {
  obs_register_source(&spg::plugin::filter_info());
  const std::string_view core_version = spg::version();
  spg::plugin::log(LOG_INFO, "loaded version %s (spg-core %.*s); detection not built yet, filter fails closed",
                   spg::plugin::kPluginVersion, static_cast<int>(core_version.size()), core_version.data());
  return true;
}

void obs_module_unload(void) {
  // Scheduler workers run code from this module: wait until every retired
  // scheduler has joined its workers before the module can be unmapped.
  spg::plugin::module_reaper().shutdown();
  spg::plugin::log(LOG_INFO, "unloaded");
}
