#pragma once

// libobs graphics behind spg::plugin::FrameGpu (SPG-14): the lookahead ring
// as one gs_texrender per slot, the readback as a downscaling texrender plus
// two stage surfaces, and the drawing of a DrawPlan with
// effects/spg-mask.effect. Every call runs on the graphics thread inside the
// filter's video_render, except destroy(), which enters the graphics context.
// The decisions are in plugin/src/pipeline/ and unit-tested there; this file
// only executes them.

#include <array>
#include <cstddef>
#include <vector>

#include <obs.h>

#include "pipeline/mask_levels.hpp"
#include "pipeline/pipeline.hpp"

namespace spg::plugin {

class ObsFrameGpu final : public FrameGpu {
 public:
  ObsFrameGpu(obs_source_t* filter, std::size_t slots);
  ~ObsFrameGpu() override;  // destroy()

  bool capture(std::size_t slot, Size size, std::uint64_t serial) override;
  bool stage(std::size_t slot, std::size_t surface, Size readback) override;
  bool map(std::size_t surface, MappedReadback& out) override;
  void unmap(std::size_t surface) override;
  void free_slot(std::size_t slot) override;

  // Draws `plan` as the filter's output at `output` size. With `overlay`,
  // adds mask outlines and, for a covered frame, a corner square whose colour
  // names the Cover (docs/plugin-render.md); never text.
  void draw(const DrawPlan& plan, Size output, bool overlay);
  // An opaque fill the size of `output`, drawn straight to the target.
  void draw_solid(Size output);

  // Frees every GPU resource (enters the graphics context itself).
  void destroy() noexcept;

 private:
  bool load_effect();
  gs_texture_t* slot_texture(std::size_t slot) const;
  // What the slot holds now, for checked_kind().
  SlotContents contents(std::size_t slot) const;
  // Builds levels 1..level of the halving chain of `base`; returns the
  // texture of `level`, or null.
  gs_texture_t* build_chain(gs_texture_t* base, Size frame, std::uint32_t level);
  void draw_texture(gs_texture_t* texture, const char* technique, float x, float y, float width, float height);
  void draw_region(gs_texture_t* level_texture, Size level_size, const LevelRegion& region,
                   const char* technique, Size output);
  void fill(float x, float y, float width, float height, std::uint32_t argb);
  void compose(const DrawPlan& plan, DrawKind kind, Size output, bool overlay);

  obs_source_t* filter_;
  std::vector<gs_texrender_t*> slots_;
  std::vector<Size> slot_sizes_;
  std::vector<std::uint64_t> slot_serials_;  // 0: nothing valid captured
  gs_texrender_t* readback_ = nullptr;
  std::array<gs_stagesurf_t*, 2> surfaces_{};
  std::array<Size, 2> surface_sizes_{};
  // levels_[k - 1] holds level k. Fixed size (top_level() of any 32-bit size
  // is at most 32), so composing a frame never allocates and cannot throw
  // while a texrender and a blend state are open.
  std::array<gs_texrender_t*, 32> levels_{};
  gs_texrender_t* output_ = nullptr;
  gs_effect_t* effect_ = nullptr;
  bool effect_failed_ = false;
};

}  // namespace spg::plugin
