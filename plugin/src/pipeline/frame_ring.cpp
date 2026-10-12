#include "pipeline/frame_ring.hpp"

#include <algorithm>

namespace spg::plugin {

FrameRing::FrameRing(std::size_t capacity) : holders_(capacity, 0) {
  fresh_.reserve(capacity);
  idle_.reserve(capacity);
  for (std::size_t i = capacity; i > 0; --i) {
    fresh_.push_back(i - 1);  // slot 0 first
  }
}

std::optional<std::size_t> FrameRing::acquire() {
  std::size_t slot = 0;
  if (!idle_.empty()) {
    slot = idle_.back();
    idle_.pop_back();
  } else if (!fresh_.empty()) {
    slot = fresh_.back();
    fresh_.pop_back();
  } else {
    return std::nullopt;
  }
  holders_[slot] = 1;
  ++held_;
  return slot;
}

std::optional<std::size_t> FrameRing::peek() const noexcept {
  if (!idle_.empty()) {
    return idle_.back();
  }
  if (!fresh_.empty()) {
    return fresh_.back();
  }
  return std::nullopt;
}

void FrameRing::retain(std::size_t slot) {
  if (slot < holders_.size() && holders_[slot] > 0) {
    ++holders_[slot];
  }
}

void FrameRing::release(std::size_t slot) noexcept {
  if (slot >= holders_.size() || holders_[slot] == 0) {
    return;
  }
  if (--holders_[slot] == 0) {
    --held_;
    idle_.push_back(slot);  // capacity reserved: never reallocates
  }
}

std::uint32_t FrameRing::holders(std::size_t slot) const noexcept {
  return slot < holders_.size() ? holders_[slot] : 0;
}

std::vector<std::size_t> FrameRing::trim(std::size_t max_idle) {
  std::vector<std::size_t> freed;
  if (idle_.size() <= max_idle) {
    return freed;
  }
  const std::size_t count = idle_.size() - max_idle;
  freed.assign(idle_.begin(), idle_.begin() + static_cast<std::ptrdiff_t>(count));
  idle_.erase(idle_.begin(), idle_.begin() + static_cast<std::ptrdiff_t>(count));
  fresh_.insert(fresh_.end(), freed.begin(), freed.end());
  return freed;
}

}  // namespace spg::plugin
