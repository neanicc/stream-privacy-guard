#pragma once

// Side-by-side model runs for the text recognizer (SPG-16, lane:vision):
// TextRecognizer::recognize() reads its batches through run_batches(). Header
// only so the unit tests can drive it without a model.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <thread>
#include <vector>

namespace spg::ocr {

// Calls run(b) once for every b in [0, count), on up to `lanes` threads: the
// caller and lanes - 1 it starts, each taking the next b until none is left.
// A lane that cannot start leaves its share to the others. The first
// exception stops the lanes from taking more and is rethrown (the lowest
// lane's, when several lanes threw) once every lane has returned, so no
// thread outlives the call. Plain std::thread: libc++ on macOS has no
// std::jthread, and MSVC's pulls in a runtime DLL the plugin does not ship.
template <class Run>
void run_batches(std::size_t count, std::size_t lanes, const Run& run) {
  lanes = std::min(lanes, count);
  if (lanes <= 1) {
    for (std::size_t b = 0; b < count; ++b) {
      run(b);
    }
    return;
  }
  std::atomic<std::size_t> next{0};
  std::vector<std::exception_ptr> errors(lanes);
  const auto lane = [&](std::size_t i) noexcept {
    try {
      for (std::size_t b = next.fetch_add(1); b < count; b = next.fetch_add(1)) {
        run(b);
      }
    } catch (...) {
      errors[i] = std::current_exception();
      next.store(count);
    }
  };
  {
    std::vector<std::thread> helpers;
    // Joins whatever started, on every way out of this block.
    struct JoinAll {
      std::vector<std::thread>& threads;
      ~JoinAll() {
        for (std::thread& t : threads) {
          if (t.joinable()) {
            t.join();
          }
        }
      }
    } const join_all{helpers};
    try {
      helpers.reserve(lanes - 1);
      for (std::size_t i = 1; i < lanes; ++i) {
        helpers.emplace_back(lane, i);
      }
    } catch (...) {
      // Fewer lanes: the caller's takes what the missing ones would have.
    }
    lane(0);
  }
  for (const std::exception_ptr& error : errors) {
    if (error) {
      std::rethrow_exception(error);
    }
  }
}

}  // namespace spg::ocr
