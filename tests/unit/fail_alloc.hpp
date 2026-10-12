// Allocation-failure injection for spg-unit-tests. fail_alloc.cpp replaces
// the global operator new for this test binary; while a FailAllocations is
// alive, the n-th allocation (0-based) on the calling thread throws
// std::bad_alloc and every other allocation succeeds. Disarmed, operator new
// behaves as the default one.
#pragma once

#include <cstdint>

namespace spg_test {

class FailAllocations {
 public:
  explicit FailAllocations(std::uint64_t n) noexcept;
  ~FailAllocations();
  FailAllocations(const FailAllocations&) = delete;
  FailAllocations& operator=(const FailAllocations&) = delete;

  // Whether the n-th allocation was reached (and threw).
  [[nodiscard]] bool fired() const noexcept;
};

}  // namespace spg_test
