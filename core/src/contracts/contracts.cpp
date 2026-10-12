// Names and validation for the contract types in core/include/spg/.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_set>

#include "spg/category.hpp"
#include "spg/detection.hpp"
#include "spg/error.hpp"
#include "spg/ground_truth.hpp"
#include "spg/mask.hpp"
#include "spg/policy_config.hpp"

namespace spg {

namespace {

struct CategoryNames {
  Category category;
  std::string_view name;
  std::string_view code;
};

constexpr std::array<CategoryNames, kAllCategories.size()> kCategoryNames = {{
    {Category::Face, "face", "FACE"},
    {Category::Watchlist, "watchlist", "WATCHLIST"},
    {Category::PiiEmail, "pii:email", "PII_EMAIL"},
    {Category::PiiPhone, "pii:phone", "PII_PHONE"},
    {Category::PiiCard, "pii:card", "PII_CARD"},
    {Category::PiiIp, "pii:ip", "PII_IP"},
    {Category::PiiAddress, "pii:address", "PII_ADDRESS"},
    {Category::PiiToken, "pii:token", "PII_TOKEN"},
}};

const CategoryNames* find_category(Category category) noexcept {
  for (const CategoryNames& entry : kCategoryNames) {
    if (entry.category == category) {
      return &entry;
    }
  }
  return nullptr;
}

bool is_blank(std::string_view text) noexcept {
  return std::all_of(text.begin(), text.end(), [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  });
}

void require_source_box(const Box& box, const std::string& what) {
  if (!is_valid(box.rect)) {
    throw ContractError(what + ": box must be finite with non-negative size");
  }
  if (box.space != Space::Source) {
    throw ContractError(what + ": box must be in Source space");
  }
}

}  // namespace

std::string_view category_name(Category category) noexcept {
  const CategoryNames* entry = find_category(category);
  return entry != nullptr ? entry->name : "unknown";
}

std::optional<Category> parse_category_name(std::string_view name) noexcept {
  for (const CategoryNames& entry : kCategoryNames) {
    if (entry.name == name) {
      return entry.category;
    }
  }
  return std::nullopt;
}

std::string_view reason_code(Category category) noexcept {
  const CategoryNames* entry = find_category(category);
  return entry != nullptr ? entry->code : "UNKNOWN";
}

std::optional<Category> parse_reason_code(std::string_view code) noexcept {
  for (const CategoryNames& entry : kCategoryNames) {
    if (entry.code == code) {
      return entry.category;
    }
  }
  return std::nullopt;
}

std::string_view to_string(MaskStyle style) noexcept {
  switch (style) {
    case MaskStyle::Blur:
      return "blur";
    case MaskStyle::Pixelate:
      return "pixelate";
    case MaskStyle::SolidFill:
      return "solid_fill";
  }
  return "unknown";
}

std::optional<MaskStyle> parse_mask_style(std::string_view name) noexcept {
  for (const MaskStyle style : {MaskStyle::Blur, MaskStyle::Pixelate, MaskStyle::SolidFill}) {
    if (to_string(style) == name) {
      return style;
    }
  }
  return std::nullopt;
}

std::string_view to_string(FallbackMode mode) noexcept {
  switch (mode) {
    case FallbackMode::BlurSource:
      return "blur_source";
    case FallbackMode::HoldLastScanned:
      return "hold_last_scanned";
    case FallbackMode::SolidFill:
      return "solid_fill";
  }
  return "unknown";
}

std::optional<FallbackMode> parse_fallback_mode(std::string_view name) noexcept {
  for (const FallbackMode mode :
       {FallbackMode::BlurSource, FallbackMode::HoldLastScanned, FallbackMode::SolidFill}) {
    if (to_string(mode) == name) {
      return mode;
    }
  }
  return std::nullopt;
}

void validate(const Detection& detection) {
  require_source_box(detection.box, "detection");
  if (!std::isfinite(detection.score) || detection.score < 0.0F || detection.score > 1.0F) {
    throw ContractError("detection: score must be in [0, 1]");
  }
}

void validate(const MaskPlan& plan) {
  for (std::size_t i = 0; i < plan.rects.size(); ++i) {
    require_source_box(plan.rects[i].box, "mask plan: rect " + std::to_string(i));
  }
}

void validate(const PolicyConfig& config) {
  using std::chrono::milliseconds;
  if (config.lookahead < milliseconds{0} || config.lookahead > kMaxLookahead) {
    throw ContractError("policy config: lookahead_ms must be in [0, " +
                        std::to_string(kMaxLookahead.count()) + "]");
  }
  if (config.retro < milliseconds{0} || config.retro > config.lookahead) {
    throw ContractError("policy config: retro_ms must be in [0, lookahead_ms]");
  }
  if (config.hold < milliseconds{0} || config.hold > kMaxHold) {
    throw ContractError("policy config: hold_ms must be in [0, " + std::to_string(kMaxHold.count()) +
                        "]");
  }
  if (!std::isfinite(config.fuzzy.max_edit_ratio) || config.fuzzy.max_edit_ratio < 0.0 ||
      config.fuzzy.max_edit_ratio > 1.0) {
    throw ContractError("policy config: fuzzy.max_edit_ratio must be in [0, 1]");
  }
  if (config.fuzzy.min_partial_chars == 0) {
    throw ContractError("policy config: fuzzy.min_partial_chars must be at least 1");
  }
  if (!std::isfinite(config.margin_px) || config.margin_px < 0.0F) {
    throw ContractError("policy config: margin_px must be finite and not negative");
  }
  if (!std::isfinite(config.margin_ratio) || config.margin_ratio < 0.0F) {
    throw ContractError("policy config: margin_ratio must be finite and not negative");
  }
  for (std::size_t i = 0; i < config.watchlist.size(); ++i) {
    if (is_blank(config.watchlist[i].reveal())) {
      throw ContractError("policy config: watchlist entry " + std::to_string(i) + " is empty or blank");
    }
  }
}

void validate(const GroundTruthFrame& frame) {
  if (frame.source_size.width == 0 || frame.source_size.height == 0) {
    throw ContractError("ground truth: width and height must be non-zero");
  }
  std::unordered_set<std::string_view> ids;
  for (std::size_t i = 0; i < frame.instances.size(); ++i) {
    const GroundTruthInstance& inst = frame.instances[i];
    const std::string what = "ground truth: instance " + std::to_string(i);
    if (inst.id.empty()) {
      throw ContractError(what + ": id must not be empty");
    }
    if (!ids.insert(inst.id).second) {
      throw ContractError(what + ": id repeats an earlier instance");
    }
    require_source_box(inst.box, what);
    if (!std::isfinite(inst.visibility) || inst.visibility < 0.0F || inst.visibility > 1.0F) {
      throw ContractError(what + ": visibility must be in [0, 1]");
    }
  }
}

}  // namespace spg
