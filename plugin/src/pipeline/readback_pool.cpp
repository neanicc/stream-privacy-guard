#include "pipeline/readback_pool.hpp"

#include <cstring>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace spg::plugin {

namespace {

struct Buffer {
  std::unique_ptr<std::uint8_t[]> bytes;
  std::size_t size = 0;
};

}  // namespace

// Shared with every buffer handed out, so a buffer dropped by a worker after
// the pool is gone is simply freed.
struct ReadbackPool::State {
  std::mutex mutex;
  std::vector<Buffer> idle;
  std::size_t max_idle = 0;
  std::uint64_t allocations = 0;
  bool closed = false;
};

ReadbackPool::ReadbackPool(std::size_t max_idle) : state_(std::make_shared<State>()) {
  state_->max_idle = max_idle;
  state_->idle.reserve(max_idle);
}

ReadbackPool::~ReadbackPool() {
  const std::lock_guard lock(state_->mutex);
  state_->closed = true;
  state_->idle.clear();
}

std::shared_ptr<const OwnedFrame> ReadbackPool::copy_bgra(const std::uint8_t* data, std::uint32_t linesize,
                                                          Size size, Timestamp t) noexcept {
  const std::size_t row = 4 * static_cast<std::size_t>(size.width);
  if (data == nullptr || size.width == 0 || size.height == 0 || linesize < row) {
    return nullptr;
  }
  const std::size_t bytes = row * size.height;
  try {
    Buffer buffer;
    {
      const std::lock_guard lock(state_->mutex);
      // Readbacks have one size until the source is resized; a buffer of
      // another size is dropped rather than kept for later.
      while (!state_->idle.empty()) {
        Buffer candidate = std::move(state_->idle.back());
        state_->idle.pop_back();
        if (candidate.size == bytes) {
          buffer = std::move(candidate);
          break;
        }
      }
    }
    if (buffer.bytes == nullptr) {
      buffer.bytes.reset(new std::uint8_t[bytes]);  // not zeroed: every byte is written below
      buffer.size = bytes;
      const std::lock_guard lock(state_->mutex);
      ++state_->allocations;
    }
    for (std::uint32_t y = 0; y < size.height; ++y) {
      std::memcpy(buffer.bytes.get() + row * y, data + static_cast<std::size_t>(linesize) * y, row);
    }
    FrameView view;
    view.planes[0] = buffer.bytes.get();
    view.strides[0] = static_cast<std::uint32_t>(row);
    view.width = size.width;
    view.height = size.height;
    view.format = PixelFormat::BGRA;
    view.timestamp = t;
    auto* raw = new Buffer(std::move(buffer));
    std::shared_ptr<Buffer> owner(raw, [state = state_](Buffer* returned) {
      std::unique_ptr<Buffer> held(returned);
      try {
        const std::lock_guard lock(state->mutex);
        if (!state->closed && state->idle.size() < state->max_idle) {
          state->idle.push_back(std::move(*held));  // capacity reserved: never reallocates
        }
      } catch (...) {
        // A mutex error: the buffer is freed instead.
      }
    });
    return OwnedFrame::adopt(view, std::move(owner));
  } catch (...) {
    return nullptr;  // no scan for this frame; it fails closed unless another cycle covers it
  }
}

std::size_t ReadbackPool::idle() const {
  const std::lock_guard lock(state_->mutex);
  return state_->idle.size();
}

std::uint64_t ReadbackPool::allocations() const {
  const std::lock_guard lock(state_->mutex);
  return state_->allocations;
}

}  // namespace spg::plugin
