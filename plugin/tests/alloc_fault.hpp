#pragma once

// Allocation failure on demand for the pipeline tests: while set, every
// allocation of at least `bytes` through the global operator new throws
// std::bad_alloc. The replacement operators live in alloc_fault.cpp, a
// translation unit of their own, so no caller sees their bodies (GCC would
// otherwise pair an inlined free() with operator new and reject the build
// under -Werror=mismatched-new-delete).

#include <cstddef>

namespace spg_test {

void fail_allocations_from(std::size_t bytes) noexcept;
void allow_allocations() noexcept;

}  // namespace spg_test
