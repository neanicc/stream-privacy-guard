#pragma once

// Pooled readback buffers (SPG-14), without OBS. The render thread copies a
// mapped stage surface into a buffer from this pool and hands it to the
// scheduler as an OwnedFrame; when the worker drops the frame, on its own
// thread, the buffer goes back to the pool instead of being freed. So a
// steady stream of readbacks allocates no pixel memory after the first few
// frames (the SPG-10 review asked for this: Scheduler::push used to allocate
// and zero a full readback on the render thread for every scan).

#include <cstddef>
#include <cstdint>
#include <memory>

#include "spg/geometry.hpp"
#include "spg/scan.hpp"
#include "spg/time.hpp"

namespace spg::plugin {

class ReadbackPool {
 public:
  // Keeps at most `max_idle` returned buffers; more are freed.
  explicit ReadbackPool(std::size_t max_idle = 3);
  ~ReadbackPool();
  ReadbackPool(const ReadbackPool&) = delete;
  ReadbackPool& operator=(const ReadbackPool&) = delete;

  // Copies `size.height` rows of 4 * size.width BGRA bytes from `data`
  // (`linesize` bytes apart) into a pooled buffer, tightly packed, and wraps
  // it as a readback with timestamp `t`. Empty when `data` is null, the size
  // is zero, `linesize` is shorter than a row, or memory runs out.
  [[nodiscard]] std::shared_ptr<const OwnedFrame> copy_bgra(const std::uint8_t* data, std::uint32_t linesize,
                                                            Size size, Timestamp t) noexcept;

  [[nodiscard]] std::size_t idle() const;
  // Buffers allocated so far (not reused from the pool).
  [[nodiscard]] std::uint64_t allocations() const;

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace spg::plugin
