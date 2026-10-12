#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "spg/category.hpp"
#include "spg/geometry.hpp"
#include "spg/time.hpp"

namespace spg {

// One planted item on one eval frame. The same `id` names the same item on
// every frame it appears in. Ground truth never carries the item's text.
struct GroundTruthInstance {
  std::string id;
  // The sensitive category, or empty for a decoy (a near-miss that should not
  // be blurred). JSON kind: category_name(), or "decoy".
  std::optional<Category> category;
  Box box;                 // Source space
  float visibility = 1.0F;  // [0, 1]; 0 = present but not visible on this frame
  [[nodiscard]] bool is_decoy() const noexcept { return !category.has_value(); }
  friend bool operator==(const GroundTruthInstance&, const GroundTruthInstance&) = default;
};

struct GroundTruthFrame {
  std::uint64_t frame_index = 0;
  Timestamp timestamp{};
  Size source_size;
  std::vector<GroundTruthInstance> instances;
  friend bool operator==(const GroundTruthFrame&, const GroundTruthFrame&) = default;
};

// Throws ContractError for a zero source size, an empty or duplicate id, an
// invalid or non-Source box, or visibility outside [0, 1].
void validate(const GroundTruthFrame& frame);

}  // namespace spg
