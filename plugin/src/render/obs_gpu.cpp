#include "render/obs_gpu.hpp"

#include <graphics/vec4.h>
#include <obs-module.h>

#include <algorithm>
#include <cmath>

#include "plugin-support.hpp"

namespace spg::plugin {

namespace {

constexpr const char* kSmooth = "Smooth";
constexpr const char* kBlocks = "Blocks";

// Debug overlay colours (0xAARRGGBB), one per Cover; docs/plugin-render.md.
constexpr std::uint32_t kOutline = 0xFF00E676;
std::uint32_t cover_colour(Cover cover) noexcept {
  switch (cover) {
    case Cover::None:
      return 0;
    case Cover::NotScanned:
      return 0xFFFF1744;  // red
    case Cover::LateRelease:
      return 0xFFFF9100;  // orange
    case Cover::BadTimestamp:
      return 0xFFD500F9;  // magenta
    case Cover::ApplyFailed:
      return 0xFF651FFF;  // violet
    case Cover::Starting:
    case Cover::NoDetection:
      return 0xFF9E9E9E;  // grey
    case Cover::SettingsChanged:
      return 0xFF2979FF;  // blue
    case Cover::NoTexture:
      return 0xFFFFFFFF;  // white
    case Cover::Stalled:
      return 0xFFFFEA00;  // yellow
    case Cover::Inconsistent:
      return 0xFF00E5FF;  // cyan
  }
  return 0xFFFFFFFF;
}

float f(std::uint32_t v) noexcept {
  return static_cast<float>(v);
}

// Starts rendering into `texrender` at `size`, cleared to transparent, with
// blending that replaces what is there (so averaged alpha is kept, never
// blended over a sharp copy).
bool begin_target(gs_texrender_t* texrender, Size size) {
  gs_texrender_reset(texrender);
  if (!gs_texrender_begin(texrender, size.width, size.height)) {
    return false;
  }
  vec4 clear{};
  vec4_zero(&clear);
  gs_clear(GS_CLEAR_COLOR, &clear, 0.0F, 0);
  gs_ortho(0.0F, f(size.width), 0.0F, f(size.height), -100.0F, 100.0F);
  return true;
}

gs_texrender_t* ensure(gs_texrender_t*& texrender) {
  if (texrender == nullptr) {
    texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
  }
  return texrender;
}

}  // namespace

ObsFrameGpu::ObsFrameGpu(obs_source_t* filter, std::size_t slots)
    : filter_(filter), slots_(slots, nullptr), slot_sizes_(slots), slot_serials_(slots, 0) {}

ObsFrameGpu::~ObsFrameGpu() {
  destroy();
}

bool ObsFrameGpu::load_effect() {
  if (effect_ != nullptr) {
    return true;
  }
  if (effect_failed_) {
    return false;
  }
  char* path = obs_module_file("effects/spg-mask.effect");
  if (path != nullptr) {
    effect_ = gs_effect_create_from_file(path, nullptr);
    bfree(path);
  }
  if (effect_ == nullptr) {
    effect_failed_ = true;
    log(LOG_ERROR, "mask effect failed to load: every frame is covered with a solid fill");
    return false;
  }
  return true;
}

gs_texture_t* ObsFrameGpu::slot_texture(std::size_t slot) const {
  return slot < slots_.size() && slots_[slot] != nullptr ? gs_texrender_get_texture(slots_[slot]) : nullptr;
}

SlotContents ObsFrameGpu::contents(std::size_t slot) const {
  SlotContents out;
  if (slot < slots_.size() && slot_texture(slot) != nullptr) {
    out.present = true;
    out.serial = slot_serials_[slot];
    out.size = slot_sizes_[slot];
  }
  return out;
}

bool ObsFrameGpu::capture(std::size_t slot, Size size, std::uint64_t serial) {
  if (slot >= slots_.size()) {
    return false;
  }
  // Until this capture completes the slot holds nothing a plan may draw.
  slot_serials_[slot] = 0;
  slot_sizes_[slot] = {};
  obs_source_t* target = obs_filter_get_target(filter_);
  obs_source_t* parent = obs_filter_get_parent(filter_);
  if (target == nullptr || parent == nullptr || ensure(slots_[slot]) == nullptr) {
    return false;
  }
  gs_texrender_reset(slots_[slot]);
  if (!gs_texrender_begin(slots_[slot], size.width, size.height)) {
    return false;
  }
  // As obs_source_process_filter_begin renders a filter's target.
  gs_blend_state_push();
  gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
  vec4 clear{};
  vec4_zero(&clear);
  gs_clear(GS_CLEAR_COLOR, &clear, 0.0F, 0);
  gs_ortho(0.0F, f(size.width), 0.0F, f(size.height), -100.0F, 100.0F);
  const std::uint32_t flags = obs_source_get_output_flags(parent);
  if (target == parent && (flags & OBS_SOURCE_CUSTOM_DRAW) == 0 && (flags & OBS_SOURCE_ASYNC) == 0) {
    obs_source_default_render(target);
  } else {
    obs_source_video_render(target);
  }
  gs_blend_state_pop();
  gs_texrender_end(slots_[slot]);
  slot_sizes_[slot] = size;
  slot_serials_[slot] = serial;
  return true;
}

bool ObsFrameGpu::stage(std::size_t slot, std::size_t surface, Size readback) {
  gs_texture_t* texture = slot_texture(slot);
  if (texture == nullptr || slot_serials_[slot] == 0 || surface >= surfaces_.size() || !load_effect()) {
    return false;
  }
  if (slot_sizes_[slot] != readback) {
    if (ensure(readback_) == nullptr) {
      return false;
    }
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    const bool begun = begin_target(readback_, readback);
    if (begun) {
      draw_texture(texture, kSmooth, 0.0F, 0.0F, f(readback.width), f(readback.height));
      gs_texrender_end(readback_);
    }
    gs_blend_state_pop();
    texture = begun ? gs_texrender_get_texture(readback_) : nullptr;
    if (texture == nullptr) {
      return false;
    }
  }
  if (surfaces_[surface] != nullptr && surface_sizes_[surface] != readback) {
    gs_stagesurface_destroy(surfaces_[surface]);
    surfaces_[surface] = nullptr;
  }
  if (surfaces_[surface] == nullptr) {
    surfaces_[surface] = gs_stagesurface_create(readback.width, readback.height, GS_BGRA);
    surface_sizes_[surface] = readback;
  }
  if (surfaces_[surface] == nullptr) {
    return false;
  }
  gs_stage_texture(surfaces_[surface], texture);
  return true;
}

bool ObsFrameGpu::map(std::size_t surface, MappedReadback& out) {
  if (surface >= surfaces_.size() || surfaces_[surface] == nullptr) {
    return false;
  }
  std::uint8_t* data = nullptr;
  std::uint32_t linesize = 0;
  if (!gs_stagesurface_map(surfaces_[surface], &data, &linesize)) {
    return false;
  }
  out.data = data;
  out.linesize = linesize;
  return true;
}

void ObsFrameGpu::unmap(std::size_t surface) {
  if (surface < surfaces_.size() && surfaces_[surface] != nullptr) {
    gs_stagesurface_unmap(surfaces_[surface]);
  }
}

void ObsFrameGpu::free_slot(std::size_t slot) {
  if (slot < slots_.size()) {
    gs_texrender_destroy(slots_[slot]);
    slots_[slot] = nullptr;
    slot_sizes_[slot] = {};
    slot_serials_[slot] = 0;
  }
}

void ObsFrameGpu::draw_texture(gs_texture_t* texture, const char* technique, float x, float y, float width,
                               float height) {
  gs_effect_set_texture(gs_effect_get_param_by_name(effect_, "image"), texture);
  const float scale_x = width / f(gs_texture_get_width(texture));
  const float scale_y = height / f(gs_texture_get_height(texture));
  while (gs_effect_loop(effect_, technique)) {
    gs_matrix_push();
    gs_matrix_translate3f(x, y, 0.0F);
    gs_matrix_scale3f(scale_x, scale_y, 1.0F);
    gs_draw_sprite(texture, 0, 0, 0);
    gs_matrix_pop();
  }
}

void ObsFrameGpu::draw_region(gs_texture_t* level_texture, Size level_size, const LevelRegion& region,
                              const char* technique, Size output) {
  // Level texels map onto the output by the same fractions they cover of
  // the frame, so the drawn region contains the mask rect (mask_region()).
  const float sx = f(output.width) / f(level_size.width);
  const float sy = f(output.height) / f(level_size.height);
  gs_effect_set_texture(gs_effect_get_param_by_name(effect_, "image"), level_texture);
  while (gs_effect_loop(effect_, technique)) {
    gs_matrix_push();
    gs_matrix_translate3f(f(region.x) * sx, f(region.y) * sy, 0.0F);
    gs_matrix_scale3f(sx, sy, 1.0F);
    gs_draw_sprite_subregion(level_texture, 0, region.x, region.y, region.width, region.height);
    gs_matrix_pop();
  }
}

void ObsFrameGpu::fill(float x, float y, float width, float height, std::uint32_t argb) {
  gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
  vec4 colour{};
  vec4_from_rgba(&colour, ((argb >> 16) & 0xFFU) | (argb & 0x0000FF00U) | ((argb & 0xFFU) << 16) |
                              (argb & 0xFF000000U));
  gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &colour);
  while (gs_effect_loop(solid, "Solid")) {
    gs_matrix_push();
    gs_matrix_translate3f(x, y, 0.0F);
    gs_matrix_scale3f(width, height, 1.0F);
    gs_draw_sprite(nullptr, 0, 1, 1);
    gs_matrix_pop();
  }
}

gs_texture_t* ObsFrameGpu::build_chain(gs_texture_t* base, Size frame, std::uint32_t level) {
  if (levels_.size() < level) {
    levels_.resize(level, nullptr);
  }
  gs_texture_t* below = base;
  for (std::uint32_t k = 1; k <= level && below != nullptr; ++k) {
    gs_texrender_t*& target = levels_[k - 1];
    const Size size = level_size(frame, k);
    if (ensure(target) == nullptr || !begin_target(target, size)) {
      return nullptr;
    }
    // Each texel samples the corner shared by the 2x2 texels below it, which
    // bilinear filtering averages: a box filter.
    draw_texture(below, kSmooth, 0.0F, 0.0F, f(size.width), f(size.height));
    gs_texrender_end(target);
    below = gs_texrender_get_texture(target);
  }
  return below;
}

void ObsFrameGpu::compose(const DrawPlan& plan, DrawKind kind, Size output, bool overlay) {
  const float ow = f(output.width);
  const float oh = f(output.height);
  gs_texture_t* frame = kind == DrawKind::Solid ? nullptr : slot_texture(plan.slot);
  if (kind == DrawKind::Solid || frame == nullptr) {
    fill(0.0F, 0.0F, ow, oh, 0xFF000000);
  } else if (kind == DrawKind::BlurWhole) {
    const std::uint32_t level = whole_level(plan.frame);
    gs_texture_t* coarse = build_chain(frame, plan.frame, level);
    if (coarse == nullptr || level == 0) {
      fill(0.0F, 0.0F, ow, oh, 0xFF000000);
    } else {
      draw_texture(coarse, kSmooth, 0.0F, 0.0F, ow, oh);
    }
  } else {
    // Masked: the frame, then each rect replaced from a coarse level.
    std::uint32_t top = 0;
    for (const MaskRect& rect : plan.rects) {
      if (rect.style != MaskStyle::SolidFill) {
        top = std::max(top, mask_level(rect.box.rect, plan.frame));
      }
    }
    draw_texture(frame, kSmooth, 0.0F, 0.0F, ow, oh);
    // Levels 1..top are rebuilt from this frame; if that fails, no level
    // texture is used (it could hold an older frame) and masks are filled.
    const bool chain = top > 0 && build_chain(frame, plan.frame, top) != nullptr;
    const float sx = ow / f(plan.frame.width);
    const float sy = oh / f(plan.frame.height);
    for (const MaskRect& rect : plan.rects) {
      const Rect& r = rect.box.rect;
      const std::uint32_t level = mask_level(r, plan.frame);
      // level <= top for every non-solid rect.
      gs_texture_t* coarse = chain && rect.style != MaskStyle::SolidFill && level > 0
                                 ? gs_texrender_get_texture(levels_[level - 1])
                                 : nullptr;
      const auto region = mask_region(r, plan.frame, level);
      if (!region) {
        continue;  // outside the frame
      }
      if (rect.style == MaskStyle::SolidFill || coarse == nullptr) {
        const Size texels = level_size(plan.frame, level);
        const float rx = ow / f(texels.width);
        const float ry = oh / f(texels.height);
        fill(f(region->x) * rx, f(region->y) * ry, f(region->width) * rx, f(region->height) * ry, 0xFF000000);
      } else {
        draw_region(coarse, level_size(plan.frame, level), *region,
                    rect.style == MaskStyle::Pixelate ? kBlocks : kSmooth, output);
      }
    }
    if (overlay) {
      const float line = std::max(2.0F, std::min(ow, oh) / 360.0F);
      for (const MaskRect& rect : plan.rects) {
        const Rect& r = rect.box.rect;
        const float x = r.x * sx;
        const float y = r.y * sy;
        const float w = r.width * sx;
        const float h = r.height * sy;
        fill(x, y, w, line, kOutline);
        fill(x, y + h - line, w, line, kOutline);
        fill(x, y, line, h, kOutline);
        fill(x + w - line, y, line, h, kOutline);
      }
    }
  }
  if (overlay && (plan.cover != Cover::None || kind != plan.kind)) {
    const float side = std::max(8.0F, std::min(ow, oh) / 24.0F);
    fill(0.0F, 0.0F, side, side, cover_colour(kind != plan.kind ? Cover::Inconsistent : plan.cover));
  }
}

void ObsFrameGpu::draw(const DrawPlan& plan, Size output, bool overlay) {
  // Decide from one snapshot of the slot: a plan whose texture was recycled,
  // never captured or captured at another size is not drawn from it.
  const DrawKind kind = checked_kind(plan, contents(plan.slot));
  if (kind == DrawKind::Nothing || output.width == 0 || output.height == 0) {
    return;
  }
  if (!load_effect() || ensure(output_) == nullptr) {
    draw_solid(output);
    return;
  }
  // Compose into our own texture with replace blending, so a mask region
  // replaces the frame under it (alpha included) instead of blending over a
  // sharp copy; then draw the result like any filter output.
  gs_blend_state_push();
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
  const bool begun = begin_target(output_, output);
  if (begun) {
    compose(plan, kind, output, overlay);
    gs_texrender_end(output_);
  }
  gs_blend_state_pop();
  gs_texture_t* composed = begun ? gs_texrender_get_texture(output_) : nullptr;
  if (composed == nullptr) {
    draw_solid(output);
    return;
  }
  gs_effect_t* base = obs_get_base_effect(OBS_EFFECT_DEFAULT);
  gs_effect_set_texture(gs_effect_get_param_by_name(base, "image"), composed);
  while (gs_effect_loop(base, "Draw")) {
    gs_draw_sprite(composed, 0, output.width, output.height);
  }
}

void ObsFrameGpu::draw_solid(Size output) {
  if (output.width == 0 || output.height == 0) {
    return;
  }
  gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
  vec4 colour{};
  vec4_set(&colour, 0.0F, 0.0F, 0.0F, 1.0F);
  gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &colour);
  while (gs_effect_loop(solid, "Solid")) {
    gs_draw_sprite(nullptr, 0, output.width, output.height);
  }
}

void ObsFrameGpu::destroy() noexcept {
  obs_enter_graphics();
  for (gs_texrender_t*& slot : slots_) {
    gs_texrender_destroy(slot);
    slot = nullptr;
  }
  for (gs_texrender_t*& level : levels_) {
    gs_texrender_destroy(level);
    level = nullptr;
  }
  gs_texrender_destroy(readback_);
  readback_ = nullptr;
  gs_texrender_destroy(output_);
  output_ = nullptr;
  for (gs_stagesurf_t*& surface : surfaces_) {
    gs_stagesurface_destroy(surface);
    surface = nullptr;
  }
  gs_effect_destroy(effect_);
  effect_ = nullptr;
  obs_leave_graphics();
}

}  // namespace spg::plugin
