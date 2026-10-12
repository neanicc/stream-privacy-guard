#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace spg {

// What makes content sensitive. One enum serves three contracts: categories a
// user enables in PolicyConfig, reason codes in a policy verdict, and
// sensitive instance kinds in eval ground truth.
enum class Category : std::uint8_t {
  Face,
  Watchlist,
  PiiEmail,
  PiiPhone,
  PiiCard,
  PiiIp,
  PiiAddress,
  PiiToken,
};

inline constexpr std::array<Category, 8> kAllCategories = {
    Category::Face,  Category::Watchlist, Category::PiiEmail,   Category::PiiPhone,
    Category::PiiCard, Category::PiiIp,   Category::PiiAddress, Category::PiiToken,
};

// Lower-case name used in config and ground truth JSON: "face", "watchlist",
// "pii:email", "pii:phone", "pii:card", "pii:ip", "pii:address", "pii:token".
[[nodiscard]] std::string_view category_name(Category category) noexcept;
[[nodiscard]] std::optional<Category> parse_category_name(std::string_view name) noexcept;

// Reason code carried by a verdict and shown by the debug overlay: "FACE",
// "WATCHLIST", "PII_EMAIL", "PII_PHONE", "PII_CARD", "PII_IP", "PII_ADDRESS",
// "PII_TOKEN". A reason code never includes the matched text.
[[nodiscard]] std::string_view reason_code(Category category) noexcept;
[[nodiscard]] std::optional<Category> parse_reason_code(std::string_view code) noexcept;

// A set of categories, stored as a bitmask.
class CategorySet {
 public:
  constexpr CategorySet() noexcept = default;
  [[nodiscard]] static constexpr CategorySet all() noexcept {
    CategorySet set;
    for (const Category c : kAllCategories) {
      set.insert(c);
    }
    return set;
  }

  constexpr void insert(Category c) noexcept { bits_ = static_cast<std::uint16_t>(bits_ | bit(c)); }
  constexpr void erase(Category c) noexcept {
    bits_ = static_cast<std::uint16_t>(bits_ & ~bit(c));
  }
  [[nodiscard]] constexpr bool contains(Category c) const noexcept { return (bits_ & bit(c)) != 0; }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }

  friend constexpr bool operator==(CategorySet, CategorySet) noexcept = default;

 private:
  [[nodiscard]] static constexpr std::uint16_t bit(Category c) noexcept {
    return static_cast<std::uint16_t>(1U << static_cast<unsigned>(c));
  }
  std::uint16_t bits_ = 0;
};

}  // namespace spg
