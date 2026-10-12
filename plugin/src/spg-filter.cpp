#include "spg-filter.hpp"

#include <graphics/vec4.h>
#include <obs-module.h>

#include <new>

namespace spg::plugin {
namespace {

struct FilterState {
  obs_source_t *context = nullptr;
  // Stored for SPG-10/14; the shell does not buffer or detect yet.
  long long lookahead_ms = kDefaultLookaheadMs;
  long long hold_ms = kDefaultHoldMs;
};

const char *filter_get_name(void * /*type_data*/) {
  return obs_module_text("SPG.FilterName");
}

void filter_update(void *data, obs_data_t *settings) {
  auto *state = static_cast<FilterState *>(data);
  state->lookahead_ms = obs_data_get_int(settings, kLookaheadKey);
  state->hold_ms = obs_data_get_int(settings, kHoldKey);
}

void *filter_create(obs_data_t *settings, obs_source_t *source) {
  // Called from C: never let an exception escape. OBS treats nullptr as a failed create.
  auto *state = new (std::nothrow) FilterState{};
  if (state == nullptr) {
    return nullptr;
  }
  state->context = source;
  filter_update(state, settings);
  return state;
}

void filter_destroy(void *data) {
  delete static_cast<FilterState *>(data);
}

void filter_get_defaults(obs_data_t *settings) {
  obs_data_set_default_int(settings, kLookaheadKey, kDefaultLookaheadMs);
  obs_data_set_default_int(settings, kHoldKey, kDefaultHoldMs);
}

obs_properties_t *filter_get_properties(void * /*data*/) {
  obs_properties_t *props = obs_properties_create();
  obs_property_t *status =
      obs_properties_add_text(props, kStatusKey, obs_module_text("SPG.Status"), OBS_TEXT_INFO);
  obs_property_text_set_info_type(status, OBS_TEXT_INFO_WARNING);
  obs_property_t *lookahead = obs_properties_add_int_slider(
      props, kLookaheadKey, obs_module_text("SPG.Lookahead"), 100, 2000, 50);
  obs_property_int_set_suffix(lookahead, " ms");
  obs_property_t *hold =
      obs_properties_add_int_slider(props, kHoldKey, obs_module_text("SPG.Hold"), 0, 5000, 50);
  obs_property_int_set_suffix(hold, " ms");
  return props;
}

// Fail closed: draw an opaque fill the size of the source instead of the
// source itself. The target is never rendered, so no unscanned pixel reaches
// the preview, a recording or the stream.
void filter_video_render(void *data, gs_effect_t * /*effect*/) {
  const auto *state = static_cast<const FilterState *>(data);
  obs_source_t *target = obs_filter_get_target(state->context);
  const uint32_t width = target != nullptr ? obs_source_get_base_width(target) : 0;
  const uint32_t height = target != nullptr ? obs_source_get_base_height(target) : 0;
  if (width == 0 || height == 0) {
    return;  // nothing to cover; drawing nothing shows nothing
  }
  gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
  vec4 color{};
  vec4_set(&color, 0.0F, 0.0F, 0.0F, 1.0F);
  gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &color);
  while (gs_effect_loop(solid, "Solid")) {
    gs_draw_sprite(nullptr, 0, width, height);
  }
}

obs_source_info make_filter_info() noexcept {
  obs_source_info info{};
  info.id = kFilterId;
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_VIDEO;
  info.get_name = filter_get_name;
  info.create = filter_create;
  info.destroy = filter_destroy;
  info.update = filter_update;
  info.get_defaults = filter_get_defaults;
  info.get_properties = filter_get_properties;
  info.video_render = filter_video_render;
  return info;
}

}  // namespace

const obs_source_info &filter_info() noexcept {
  static const obs_source_info info = make_filter_info();
  return info;
}

}  // namespace spg::plugin
