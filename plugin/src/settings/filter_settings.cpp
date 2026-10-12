#include "filter_settings.hpp"

#include <algorithm>
#include <chrono>

#include "spg/policy.hpp"

namespace spg::plugin {

long long clamp_lookahead_ms(long long value) noexcept {
  return std::clamp(value, kMinLookaheadMs, kMaxLookaheadMs);
}

long long clamp_hold_ms(long long value) noexcept {
  return std::clamp(value, kMinHoldMs, kMaxHoldMs);
}

long long clamp_vram_budget_mb(long long value) noexcept {
  return std::clamp(value, kMinVramBudgetMb, kMaxVramBudgetMb);
}

std::string_view to_string(Sensitivity sensitivity) noexcept {
  switch (sensitivity) {
    case Sensitivity::High:
      return "high";
    case Sensitivity::Standard:
      break;
  }
  return "standard";
}

std::optional<Sensitivity> parse_sensitivity(std::string_view name) noexcept {
  if (name == "standard") {
    return Sensitivity::Standard;
  }
  if (name == "high") {
    return Sensitivity::High;
  }
  return std::nullopt;
}

FuzzyLimits fuzzy_limits(Sensitivity sensitivity) noexcept {
  FuzzyLimits limits;  // spg-core defaults: 2 edits, ratio 0.2, partials from 6 characters
  if (sensitivity == Sensitivity::High) {
    limits.max_edits = 3;
    limits.max_edit_ratio = 0.3;
    limits.min_partial_chars = 5;
  }
  return limits;
}

std::string category_key(Category category) {
  std::string key = "category_";
  key += category_name(category);
  std::replace(key.begin(), key.end(), ':', '_');
  return key;
}

FilterSettings sanitize(const RawFilterSettings& raw) noexcept {
  FilterSettings out;
  out.lookahead_ms = clamp_lookahead_ms(raw.lookahead_ms);
  out.hold_ms = clamp_hold_ms(raw.hold_ms);
  out.vram_budget_mb = clamp_vram_budget_mb(raw.vram_budget_mb);
  out.categories = raw.categories;
  out.sensitivity = parse_sensitivity(raw.sensitivity).value_or(Sensitivity::Standard);
  out.fallback = parse_fallback_mode(raw.fallback).value_or(FallbackMode::BlurSource);
  out.debug_overlay = raw.debug_overlay;
  return out;
}

FilterIssue make_policy_config(const FilterSettings& settings, const std::vector<Secret>& watchlist,
                               std::optional<PolicyConfig>& config) {
  config.reset();
  if (settings.categories.empty()) {
    return FilterIssue::NoCategories;
  }
  try {
    PolicyConfig candidate;
    candidate.categories = settings.categories;
    candidate.watchlist = watchlist;
    candidate.fuzzy = fuzzy_limits(settings.sensitivity);
    candidate.lookahead = std::chrono::milliseconds{clamp_lookahead_ms(settings.lookahead_ms)};
    candidate.retro = candidate.lookahead;
    candidate.hold = std::chrono::milliseconds{clamp_hold_ms(settings.hold_ms)};
    candidate.fallback = settings.fallback;
    validate(candidate);
    // Compiling the engine rejects entries that could never match; its
    // errors name an index only, and no log sink is attached here.
    const PolicyEngine engine{candidate};
    static_cast<void>(engine);
    config = std::move(candidate);
    return FilterIssue::None;
  } catch (...) {
    // Any failure (ContractError, allocation) leaves detection off, which the
    // filter treats as fail closed. The message is dropped unread.
    return FilterIssue::InvalidConfig;
  }
}

}  // namespace spg::plugin
