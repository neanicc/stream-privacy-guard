#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "spg/geometry.hpp"
#include "spg/time.hpp"

namespace spg {

enum class MaskStyle : std::uint8_t { Blur, Pixelate, SolidFill };

// JSON names: "blur", "pixelate", "solid_fill".
[[nodiscard]] std::string_view to_string(MaskStyle style) noexcept;
[[nodiscard]] std::optional<MaskStyle> parse_mask_style(std::string_view name) noexcept;

struct MaskRect {
  Box box;  // Source space
  MaskStyle style = MaskStyle::Blur;
  friend bool operator==(const MaskRect&, const MaskRect&) = default;
};

// What to hide on the frame with timestamp `frame_time`. When `fallback` is
// set the pipeline failed closed for this frame and the configured
// FallbackMode covers the whole source; `rects` still lists what was found.
struct MaskPlan {
  Timestamp frame_time{};
  std::vector<MaskRect> rects;
  bool fallback = false;
  friend bool operator==(const MaskPlan&, const MaskPlan&) = default;
};

// Throws ContractError unless every rect is valid and in Source space.
void validate(const MaskPlan& plan);

}  // namespace spg
