#pragma once

// Filter settings as the plugin reads them from OBS (SPG-15), without OBS:
// clamping of values loaded from scene files, the sensitivity presets, and the
// spg-core PolicyConfig they become. Unit-tested on every OS
// (plugin/tests/settings_test.cpp); spg-filter.cpp is the OBS glue.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "spg/category.hpp"
#include "spg/policy_config.hpp"
#include "spg/secret.hpp"

namespace spg::plugin {

// Plan section 4 defaults and the ranges the properties offer. Values read
// from a scene file are clamped into these ranges, so a hand-edited or older
// file cannot switch a protection off: a lookahead or hold below the floor is
// raised to the floor. The ceilings bound VRAM (lookahead frames are kept as
// full-resolution textures, plan section 7) and are themselves safe values.
inline constexpr long long kDefaultLookaheadMs = 500;
inline constexpr long long kMinLookaheadMs = 100;
inline constexpr long long kMaxLookaheadMs = 2000;
inline constexpr long long kDefaultHoldMs = 750;
inline constexpr long long kMinHoldMs = 250;
inline constexpr long long kMaxHoldMs = 10000;
// VRAM the lookahead ring may hold, in MiB (plan section 7: "expose the
// limit"; SPG-14c). The default holds the default 500 ms lookahead at 1080p60
// (34 frames, about 270 MiB) with room to spare, and about 1000 ms there; a
// larger source or lookahead is shortened to fit, with a warning. A low value
// only shortens the lookahead (more frames covered), never weakens it.
inline constexpr long long kDefaultVramBudgetMb = 512;
inline constexpr long long kMinVramBudgetMb = 64;
inline constexpr long long kMaxVramBudgetMb = 16384;

static_assert(kMaxLookaheadMs <= kMaxLookahead.count(), "plugin lookahead must stay valid for spg-core");
static_assert(kMaxHoldMs <= kMaxHold.count(), "plugin hold must stay valid for spg-core");

[[nodiscard]] long long clamp_lookahead_ms(long long value) noexcept;
[[nodiscard]] long long clamp_hold_ms(long long value) noexcept;
[[nodiscard]] long long clamp_vram_budget_mb(long long value) noexcept;

// How eagerly watch-list entries match OCR text. Both presets favour recall;
// there is deliberately no preset below the spg-core defaults. The numbers are
// placeholders until SPG-12 tunes them against the eval.
enum class Sensitivity : std::uint8_t { Standard, High };

// Settings names: "standard", "high".
[[nodiscard]] std::string_view to_string(Sensitivity sensitivity) noexcept;
[[nodiscard]] std::optional<Sensitivity> parse_sensitivity(std::string_view name) noexcept;
[[nodiscard]] FuzzyLimits fuzzy_limits(Sensitivity sensitivity) noexcept;

// The settings key of a category's checkbox: "category_" + its JSON name with
// ':' replaced by '_' ("category_face", "category_pii_email").
[[nodiscard]] std::string category_key(Category category);

// Values exactly as read from OBS settings: unvalidated, possibly from an old
// or hand-edited scene file.
struct RawFilterSettings {
  long long lookahead_ms = kDefaultLookaheadMs;
  long long hold_ms = kDefaultHoldMs;
  long long vram_budget_mb = kDefaultVramBudgetMb;
  CategorySet categories = CategorySet::all();
  std::string_view sensitivity = "standard";
  std::string_view fallback = "blur_source";
  bool debug_overlay = false;
};

// Settings after clamping. Unknown names fall back to the defaults (standard
// sensitivity, blur_source fallback), never to a weaker choice.
struct FilterSettings {
  long long lookahead_ms = kDefaultLookaheadMs;
  long long hold_ms = kDefaultHoldMs;
  long long vram_budget_mb = kDefaultVramBudgetMb;
  CategorySet categories = CategorySet::all();
  Sensitivity sensitivity = Sensitivity::Standard;
  FallbackMode fallback = FallbackMode::BlurSource;
  // Boxes and reason codes only, never text (plan section 7). Off by default.
  bool debug_overlay = false;
  friend bool operator==(const FilterSettings&, const FilterSettings&) = default;
};

[[nodiscard]] FilterSettings sanitize(const RawFilterSettings& raw) noexcept;

// Why the filter cannot run detection and covers the whole source instead.
enum class FilterIssue : std::uint8_t {
  None,
  NoCategories,      // every category is switched off
  WatchlistStorage,  // the watch-list file is missing, unreadable or corrupt
  InvalidConfig,     // spg-core rejected the PolicyConfig
};

// The PolicyConfig for these settings and watch-list (retroactive window equal
// to the lookahead, plan section 4), checked with spg::validate and by
// compiling a PolicyEngine. Returns the issue that stops detection, if any;
// `config` is set only when the result is FilterIssue::None. Never throws and
// never puts entry text in a message.
[[nodiscard]] FilterIssue make_policy_config(const FilterSettings& settings, const std::vector<Secret>& watchlist,
                                             std::optional<PolicyConfig>& config);

}  // namespace spg::plugin
