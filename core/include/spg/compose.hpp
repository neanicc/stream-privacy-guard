#pragma once

// CPU reference compositor (SPG-11): draws one released frame's masks, or its
// fail-closed fallback, onto Source-resolution BGRA pixels. spg-cli uses it
// for bench timing and debug output; the GPU blur effect (SPG-14) must hide at
// least as much as this does for the same plan. Rules: docs/cli.md.
//
// Pure C++: no OBS, no OpenCV, no globals, no logging, no disk.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "spg/frame.hpp"
#include "spg/geometry.hpp"
#include "spg/lookahead.hpp"
#include "spg/mask.hpp"
#include "spg/time.hpp"

namespace spg {

// Mask rects drawn per frame. The CPU compositor draws any number; the GPU
// effect draws a fixed array of this many in one pass, so every plan is fitted
// to it with fit_mask_rects() before it is drawn or scored. Eight tracks at
// TrackerConfig::max_rects_per_track (8) fit without merging.
inline constexpr std::size_t kMaskRectCapacity = 64;

// Packed BGRA pixels: row y starts at pixels[4 * width * y].
struct BgraImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] Size size() const noexcept { return {width, height}; }
  // A view of the pixels with timestamp `t`, valid while the image is alive
  // and not resized.
  [[nodiscard]] FrameView view(Timestamp t = {}) const noexcept;
};

// Throws ContractError unless the image has a non-zero size and exactly
// 4 * width * height bytes.
void validate(const BgraImage& image);

struct CompositorConfig {
  // Blur: a box blur run `blur_passes` times along each axis (close to a
  // Gaussian), with radius max(min_blur_radius, blur_radius_ratio * the
  // shorter side) of the rect, or of the whole frame for the fallback. Pixels
  // are only mixed with pixels of the same rect, so a radius of half the
  // shorter side smears it into bands.
  std::uint32_t min_blur_radius = 12;
  float blur_radius_ratio = 0.5F;
  std::uint32_t blur_passes = 3;
  // Pixelate: square blocks of max(min_pixel_block, pixel_block_ratio * the
  // shorter side), aligned to the rect's top-left corner, each set to its mean.
  std::uint32_t min_pixel_block = 8;
  float pixel_block_ratio = 0.25F;
  // Solid fill colour, B, G, R; a filled pixel is always opaque (alpha 255).
  // Blur and pixelate mix alpha like the colour channels, so a transparent
  // source's alpha shape does not survive a mask either.
  std::array<std::uint8_t, 3> fill_bgr{0, 0, 0};
};

// Throws ContractError naming the field when a value is out of range: a zero
// radius, pass count or block, or a ratio that is negative or not finite.
void validate(const CompositorConfig& config);

// At most `capacity` rects that together cover every input rect: each input
// rect lies inside some output rect, so fitting only adds blur. Rects inside
// another are dropped; while too many remain, the pair whose hull adds the
// least area beyond their union is replaced by that hull, rounded outward to
// whole pixels and drawn with the stronger style (SolidFill over Pixelate
// over Blur). Returns
// the input order otherwise. Throws ContractError for a zero capacity or a
// rect that is invalid or not in Source space.
[[nodiscard]] std::vector<MaskRect> fit_mask_rects(std::span<const MaskRect> rects,
                                                   std::size_t capacity = kMaskRectCapacity);

// Draws on `image`, which holds the released frame at Source resolution:
//   action None and plan.fallback false: each rect of plan.rects, rounded
//     outward to whole pixels and clipped to the image, with its style, in
//     order (overlaps are processed again, which only hides more);
//   BlurSource: the whole image blurred;
//   SolidFill: the whole image filled, opaque;
//   HoldLastScanned: the caller shows the re-emitted frame instead; drawing
//     it here blurs the whole image, as does a plan with fallback set but
//     action None. The compositor never shows a frame that failed closed.
// Throws ContractError when the image or config is invalid or a rect is not
// in Source space.
void compose(BgraImage& image, const MaskPlan& plan, FallbackAction action, const CompositorConfig& config = {});

}  // namespace spg
