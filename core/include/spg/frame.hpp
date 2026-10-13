#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "spg/geometry.hpp"
#include "spg/time.hpp"

namespace spg {

enum class PixelFormat : std::uint8_t {
  BGRA,  // one plane, 4 bytes per pixel
  NV12,  // Y plane, then interleaved UV plane at half width and height
  I420,  // Y, U and V planes; U and V at half width and height
};

[[nodiscard]] std::string_view to_string(PixelFormat format) noexcept;
[[nodiscard]] std::size_t plane_count(PixelFormat format) noexcept;

// Non-owning view of one video frame. It carries no Space tag: detectors get
// the downscaled readback copy (Readback pixels, size() is the readback size)
// with the Source size passed separately (docs/contracts.md, "Frames handed
// to detectors"). The pixels belong to the caller and must outlive every use
// of the view. Planar formats use one entry
// per plane; entries past plane_count(format) are ignored. Chroma planes of odd
// sizes round up.
struct FrameView {
  std::array<const std::uint8_t*, 3> planes{};
  std::array<std::uint32_t, 3> strides{};  // bytes per row, per plane
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  PixelFormat format = PixelFormat::BGRA;
  Timestamp timestamp{};

  [[nodiscard]] Size size() const noexcept { return {width, height}; }
};

// Throws ContractError when the view cannot be read safely: zero size, a
// missing plane, or a stride shorter than one row of that plane.
void validate(const FrameView& frame);

}  // namespace spg
