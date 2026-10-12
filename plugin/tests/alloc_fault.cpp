#include "alloc_fault.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::size_t> g_fail_from{SIZE_MAX};

}  // namespace

namespace spg_test {

void fail_allocations_from(std::size_t bytes) noexcept {
  g_fail_from.store(bytes, std::memory_order_relaxed);
}

void allow_allocations() noexcept {
  g_fail_from.store(SIZE_MAX, std::memory_order_relaxed);
}

}  // namespace spg_test

// Replaces the global (non-aligned) operator new and delete for the test
// binary. The array and nothrow forms forward to these by default.
void* operator new(std::size_t size) {
  if (size >= g_fail_from.load(std::memory_order_relaxed)) {
    throw std::bad_alloc();
  }
  if (void* p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc();
}

void operator delete(void* p) noexcept {
  std::free(p);
}

void operator delete(void* p, std::size_t /*size*/) noexcept {
  std::free(p);
}
