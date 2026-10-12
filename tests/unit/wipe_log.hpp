#pragma once

// Records what SecureHeap::process() wipes while a test runs (SPG-15d), so a
// test can show that a component's temporaries were protected blocks and were
// zeroed when released.

#include <algorithm>
#include <cstddef>
#include <vector>

#include "spg/secure_memory.hpp"

namespace spg::test {

// Each wiped block: where it was, how long, and whether every byte was zero
// when the observer ran.
struct WipeLog {
  struct Entry {
    const void* block;
    std::size_t bytes;
    bool all_zero;
  };
  std::vector<Entry> entries;

  [[nodiscard]] SecureHeap::WipeObserver observer() {
    return [this](const unsigned char* block, std::size_t bytes) {
      entries.push_back({block, bytes, std::all_of(block, block + bytes, [](unsigned char b) { return b == 0; })});
    };
  }
  // The most recent wipe of `block`, or nullptr.
  [[nodiscard]] const Entry* find(const void* block) const {
    const auto it =
        std::find_if(entries.rbegin(), entries.rend(), [block](const Entry& e) { return e.block == block; });
    return it == entries.rend() ? nullptr : &*it;
  }
  [[nodiscard]] bool all_zero() const {
    return std::all_of(entries.begin(), entries.end(), [](const Entry& e) { return e.all_zero; });
  }
};

// Installs a wipe observer on a heap for one scope.
class ObserveWipes {
 public:
  ObserveWipes(SecureHeap& heap, WipeLog& log) : heap_(heap) { heap_.set_wipe_observer(log.observer()); }
  ~ObserveWipes() { heap_.set_wipe_observer(nullptr); }
  ObserveWipes(const ObserveWipes&) = delete;
  ObserveWipes& operator=(const ObserveWipes&) = delete;

 private:
  SecureHeap& heap_;
};

}  // namespace spg::test
