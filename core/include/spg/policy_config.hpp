#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "spg/category.hpp"
#include "spg/mask.hpp"
#include "spg/secret.hpp"

namespace spg {

// What happens to a frame that is due for release before it was fully
// scanned. Blurring the whole source is the default; there is no fail-open
// mode. HoldLastScanned re-emits the last frame that was released after a
// complete scan, exactly as it was released (masks applied); it never emits
// the unscanned frame or a raw, unmasked copy of an earlier one. When there is
// no such frame for the current source size and content (the first frames
// after the filter starts, or after the source is resized or switched),
// HoldLastScanned applies BlurSource instead.
enum class FallbackMode : std::uint8_t { BlurSource, HoldLastScanned, SolidFill };

// JSON names: "blur_source", "hold_last_scanned", "solid_fill".
[[nodiscard]] std::string_view to_string(FallbackMode mode) noexcept;
[[nodiscard]] std::optional<FallbackMode> parse_fallback_mode(std::string_view name) noexcept;

// Bounds for fuzzy watch-list matching (SPG-08 implements the matcher). For a
// normalized entry of n characters the allowed edit distance is
// min(max_edits, floor(max_edit_ratio * n)). A partial reveal (a prefix or
// suffix of an entry) is considered only when it has at least
// min_partial_chars characters.
struct FuzzyLimits {
  std::uint32_t max_edits = 2;
  double max_edit_ratio = 0.2;
  std::uint32_t min_partial_chars = 6;
  friend bool operator==(const FuzzyLimits&, const FuzzyLimits&) = default;
};

// User-facing policy. Defaults follow docs/plan.md sections 4 and 12; the
// numeric matching and margin defaults are starting points for SPG-08/09 to
// tune against the eval.
struct PolicyConfig {
  CategorySet categories = CategorySet::all();
  std::vector<Secret> watchlist;
  FuzzyLimits fuzzy;
  std::chrono::milliseconds lookahead{500};
  std::chrono::milliseconds retro{500};  // retroactive window, <= lookahead
  std::chrono::milliseconds hold{750};   // minimum mask hold after last detection
  // Each mask edge grows by margin_px + margin_ratio * min(width, height),
  // in Source pixels.
  float margin_px = 4.0F;
  float margin_ratio = 0.25F;
  FallbackMode fallback = FallbackMode::BlurSource;
  MaskStyle mask_style = MaskStyle::Blur;
  friend bool operator==(const PolicyConfig&, const PolicyConfig&) = default;
};

// Limits enforced by validate() and the JSON parser.
inline constexpr std::chrono::milliseconds kMaxLookahead{5000};
inline constexpr std::chrono::milliseconds kMaxHold{60000};

// Throws ContractError naming the offending field (and the watch-list index,
// never its text) when a value is out of range or a watch-list entry is empty
// or blank: only Unicode White_Space characters and the zero-width characters
// U+200B..U+200D, U+2060 and U+FEFF.
void validate(const PolicyConfig& config);

}  // namespace spg
