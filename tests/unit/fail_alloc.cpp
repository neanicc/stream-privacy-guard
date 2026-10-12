#include "fail_alloc.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

// Per thread, so worker threads of other tests are never affected.
thread_local bool armed = false;
thread_local bool fired_here = false;
thread_local std::uint64_t remaining = 0;

}  // namespace

// The replaceable global allocation functions. The array and nothrow forms
// forward here by default, and the default deallocation functions call
// std::free, so only these need replacing.
void* operator new(std::size_t size) {
  if (armed) {
    if (remaining == 0) {
      armed = false;
      fired_here = true;
      throw std::bad_alloc();
    }
    --remaining;
  }
  if (void* p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }

void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace spg_test {

FailAllocations::FailAllocations(std::uint64_t n) noexcept {
  remaining = n;
  fired_here = false;
  armed = true;
}

FailAllocations::~FailAllocations() { armed = false; }

bool FailAllocations::fired() const noexcept { return fired_here; }

}  // namespace spg_test
