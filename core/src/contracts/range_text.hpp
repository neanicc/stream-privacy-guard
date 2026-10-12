#pragma once

// The "must be in [lo, hi]" rule text shared by validate() and the JSON
// parser, so both name a numeric limit the same way. Internal to spg-core.

#include <array>
#include <cstdio>
#include <string>

namespace spg::detail {

inline std::string range_text(double lo, double hi) {
  std::array<char, 64> text{};
  (void)std::snprintf(text.data(), text.size(), "must be in [%g, %g]", lo, hi);
  return text.data();
}

}  // namespace spg::detail
