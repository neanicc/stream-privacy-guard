#pragma once

#include <stdexcept>
#include <string>

namespace spg {

// Thrown when a contract is violated: malformed JSON, an out-of-range field,
// an invalid geometry mapping or a box in the wrong coordinate space.
// Messages name fields, indices and byte offsets only. They never contain
// values read from input, so a watch-list entry cannot reach a log through one.
class ContractError : public std::runtime_error {
 public:
  explicit ContractError(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace spg
