#pragma once

#include <string>
#include <string_view>

#include "spg/ground_truth.hpp"
#include "spg/mask.hpp"
#include "spg/policy_config.hpp"

// JSON forms of the contracts, documented in docs/contracts.md.
//
// Parsing is strict: the document must carry "schema": 1, unknown or duplicate
// fields are errors (a misspelled "watchlist" must not silently disable
// matching), and every value is range-checked by the matching validate().
// Errors are ContractError with messages that name fields and byte offsets
// only, never input text.
//
// Serializers write a single line of UTF-8 without a trailing newline, so a
// sequence of frames can be stored as JSON Lines.
namespace spg {

[[nodiscard]] PolicyConfig parse_policy_config(std::string_view json);
// The output contains the watch-list. Write it only to the user's own config
// file; never log it or attach it to an artifact.
[[nodiscard]] std::string serialize_policy_config(const PolicyConfig& config);

[[nodiscard]] GroundTruthFrame parse_ground_truth_frame(std::string_view json);
[[nodiscard]] std::string serialize_ground_truth_frame(const GroundTruthFrame& frame);

[[nodiscard]] MaskPlan parse_mask_plan(std::string_view json);
[[nodiscard]] std::string serialize_mask_plan(const MaskPlan& plan);

}  // namespace spg
