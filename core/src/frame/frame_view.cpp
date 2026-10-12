#include <chrono>
#include <cstdint>
#include <string>

#include "spg/error.hpp"
#include "spg/frame.hpp"
#include "spg/time.hpp"

namespace spg {

namespace {

// Minimum bytes in one row of `plane`.
std::uint64_t min_row_bytes(PixelFormat format, std::size_t plane, std::uint32_t width) noexcept {
  const std::uint64_t w = width;
  const std::uint64_t half = (w + 1) / 2;
  switch (format) {
    case PixelFormat::BGRA:
      return 4 * w;
    case PixelFormat::NV12:
      return plane == 0 ? w : 2 * half;
    case PixelFormat::I420:
      return plane == 0 ? w : half;
  }
  return 0;
}

}  // namespace

std::string_view to_string(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::BGRA:
      return "BGRA";
    case PixelFormat::NV12:
      return "NV12";
    case PixelFormat::I420:
      return "I420";
  }
  return "unknown";
}

std::size_t plane_count(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::BGRA:
      return 1;
    case PixelFormat::NV12:
      return 2;
    case PixelFormat::I420:
      return 3;
  }
  return 0;
}

void validate(const FrameView& frame) {
  if (frame.width == 0 || frame.height == 0) {
    throw ContractError("frame: width and height must be non-zero");
  }
  const std::size_t planes = plane_count(frame.format);
  if (planes == 0) {
    throw ContractError("frame: unknown pixel format");
  }
  for (std::size_t p = 0; p < planes; ++p) {
    if (frame.planes[p] == nullptr) {
      throw ContractError("frame: plane " + std::to_string(p) + " is missing");
    }
    if (frame.strides[p] < min_row_bytes(frame.format, p, frame.width)) {
      throw ContractError("frame: stride of plane " + std::to_string(p) + " is shorter than one row");
    }
  }
}

Timestamp SteadyClock::now() const noexcept {
  const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
  return Timestamp{std::chrono::duration_cast<Duration>(since_epoch)};
}

}  // namespace spg
