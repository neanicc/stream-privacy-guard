#pragma once

// The lookahead ring's slot bookkeeping (SPG-14), without OBS. A slot names
// one full-resolution texture on the GPU side; the ring only counts who still
// needs it: the scheduler's buffer, the frame on screen, and the last frame
// released clean (kept for HoldLastScanned). A slot with no holder is idle and
// is reused by the next capture, so textures are not recreated every frame.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace spg::plugin {

class FrameRing {
 public:
  // At most `capacity` slots are ever handed out at once.
  explicit FrameRing(std::size_t capacity);

  // A slot with one holder: the most recently idled one (its texture is warm),
  // else a never-used one. Empty when every slot is held.
  [[nodiscard]] std::optional<std::size_t> acquire();
  void retain(std::size_t slot);
  // Drops one holder; the slot is idle when none is left. Ignores a slot that
  // is not held, so a double release cannot free a slot still in use.
  void release(std::size_t slot) noexcept;

  [[nodiscard]] std::size_t capacity() const noexcept { return holders_.size(); }
  [[nodiscard]] std::size_t held() const noexcept { return held_; }
  [[nodiscard]] std::uint32_t holders(std::size_t slot) const noexcept;

  // Idle slots past `max_idle` (the least recently used first): the caller
  // frees their textures. They become never-used slots again.
  [[nodiscard]] std::vector<std::size_t> trim(std::size_t max_idle);

 private:
  std::vector<std::uint32_t> holders_;
  std::vector<std::size_t> idle_;   // warm, oldest first
  std::vector<std::size_t> fresh_;  // never used (or trimmed), next at the back
  std::size_t held_ = 0;
};

}  // namespace spg::plugin
