#pragma once

// Where the GPU blur samples from (SPG-14), without OBS. The blur effect
// builds a chain of box-filtered halvings of the frame (level 0 is the frame,
// level k is level k-1 halved, rounding up, down to 1x1), so each texel of
// level k is the average of about 2^k x 2^k frame pixels. A mask is drawn
// from one level, chosen so that its cells are large compared with what it
// hides: smooth (bilinear) for Blur, blocky (point) for Pixelate. Every
// function here is pure so the covering rules are unit-tested; the OBS side
// only executes them (plugin/src/render/).

#include <cstdint>
#include <optional>

#include "spg/geometry.hpp"

namespace spg::plugin {

// Smallest cell, in frame pixels, a mask is drawn with.
inline constexpr std::uint32_t kMinMaskCell = 8;
// A mask's cells are at least a third of its shorter side, so the text line
// or face it covers (the mask is the detection grown by the tracker's
// margin) spans no more than about two cells across.
inline constexpr std::uint32_t kMaskCellDivisor = 3;
// The whole-source fallback blur: cells of at least 1/16 of the shorter
// side, and at least 16 pixels.
inline constexpr std::uint32_t kMinWholeCell = 16;
inline constexpr std::uint32_t kWholeCellDivisor = 16;

// Size of level `level` of the chain for a frame of `frame` (each halving
// rounds up, so no frame pixel is dropped). Level 0 is `frame`.
[[nodiscard]] Size level_size(Size frame, std::uint32_t level) noexcept;
// The level at which the chain reaches 1x1 (0 for a 1x1 or empty frame).
[[nodiscard]] std::uint32_t top_level(Size frame) noexcept;

// The level a mask over `rect` (Source pixels of `frame`) samples, capped at
// top_level(frame).
[[nodiscard]] std::uint32_t mask_level(const Rect& rect, Size frame) noexcept;
// The level the whole-source fallback blur samples, capped at top_level(frame).
[[nodiscard]] std::uint32_t whole_level(Size frame) noexcept;

// A region of whole texels of one level.
struct LevelRegion {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  friend bool operator==(const LevelRegion&, const LevelRegion&) = default;
};

// The texels of `level` that cover `rect` clipped to the frame, rounded
// outward: drawn over the same fraction of the output, the region contains
// every point of the clipped rect. Empty when the rect is invalid, has no
// area, or lies outside the frame.
[[nodiscard]] std::optional<LevelRegion> mask_region(const Rect& rect, Size frame, std::uint32_t level) noexcept;

}  // namespace spg::plugin
