// plugin.loads: start a headless libobs (no video, no UI), load the obs-spg
// module the way OBS does and check what it registers.
//
//   obs-spg-load-test <module binary> <module data directory>

#include <obs.h>

#include "spg-filter.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    ++failures;
  }
}

bool is_filter_type(const char *wanted) {
  const char *id = nullptr;
  for (size_t i = 0; obs_enum_filter_types(i, &id); ++i) {
    if (std::strcmp(id, wanted) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <module binary> <module data directory>\n", argv[0]);
    return 2;
  }
  namespace plugin = spg::plugin;

  if (!obs_startup("en-US", nullptr, nullptr)) {
    std::fprintf(stderr, "FAIL: obs_startup\n");
    return 1;
  }

  obs_module_t *module = nullptr;
  const int opened = obs_open_module(&module, argv[1], argv[2]);
  check(opened == MODULE_SUCCESS, "obs_open_module returns MODULE_SUCCESS");
  if (opened == MODULE_SUCCESS) {
    check(obs_init_module(module), "obs_init_module runs obs_module_load");

    check(is_filter_type(plugin::kFilterId), "filter is registered as a filter type");
    const uint32_t flags = obs_get_source_output_flags(plugin::kFilterId);
    check((flags & OBS_SOURCE_VIDEO) != 0, "filter declares video output");
    check((flags & OBS_SOURCE_ASYNC) == 0, "filter renders on the graphics thread (not async)");

    const char *name = obs_source_get_display_name(plugin::kFilterId);
    check(name != nullptr && std::string_view{name} == "Privacy Guard",
          "display name comes from data/locale/en-US.ini");

    obs_data_t *defaults = obs_get_source_defaults(plugin::kFilterId);
    check(defaults != nullptr, "defaults are available");
    if (defaults != nullptr) {
      check(obs_data_get_int(defaults, plugin::kLookaheadKey) == plugin::kDefaultLookaheadMs,
            "lookahead defaults to 500 ms");
      check(obs_data_get_int(defaults, plugin::kHoldKey) == plugin::kDefaultHoldMs,
            "hold defaults to 750 ms");
      obs_data_release(defaults);
    }

    obs_properties_t *props = obs_get_source_properties(plugin::kFilterId);
    check(props != nullptr, "properties are available");
    if (props != nullptr) {
      obs_property_t *status = obs_properties_get(props, plugin::kStatusKey);
      check(status != nullptr && obs_property_get_type(status) == OBS_PROPERTY_TEXT &&
                obs_property_text_type(status) == OBS_TEXT_INFO,
            "status notice is an info text property");
      obs_property_t *lookahead = obs_properties_get(props, plugin::kLookaheadKey);
      check(lookahead != nullptr && obs_property_get_type(lookahead) == OBS_PROPERTY_INT,
            "lookahead is an integer property");
      check(obs_properties_get(props, plugin::kHoldKey) != nullptr, "hold property exists");
      obs_properties_destroy(props);
    }
  }

  obs_shutdown();
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
