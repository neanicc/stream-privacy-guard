#pragma once

// Turns a FrameView into the 8-bit BGR image a model sees (lane:vision).
//
// The frame passed in holds Readback pixels: the whole source, possibly scaled
// down by the plugin. render_model_input() resizes one crop of it into a model
// input exactly as a ModelInputMapping describes, so a box the model reports
// maps back through spg::convert() with no second, hand-written conversion.
// Consumers do not see OpenCV; it is a private dependency of spg-detect.

#include <cstdint>
#include <vector>

#include "spg/frame.hpp"
#include "spg/geometry.hpp"

namespace spg::detect {

// Packed 8-bit BGR image: row y starts at pixels[3 * width * y].
struct BgrImage {
  Size size;
  std::vector<std::uint8_t> pixels;
};

// Renders `mapping.crop()` of `frame` into an image of `mapping.input()` size.
// The crop is placed at the mapping's padding offset and scaled by its scale;
// the rest of the image (letterbox padding) is black, as OpenCV's YuNet
// preprocessing pads. Content size and offset round to the nearest pixel, so a
// model coordinate maps back to within half a model pixel of the true one.
// Downscaling uses area averaging (no aliasing on 2-4x readback ratios);
// upscaling is bilinear. Alpha is ignored.
//
// NV12 and I420 are decoded as BT.709 limited range, the OBS default for
// those formats. Odd frame sizes are supported (chroma rounds up).
//
// Throws ContractError when the frame is invalid (see spg::validate) or the
// crop is not a whole-pixel rectangle inside the frame.
[[nodiscard]] BgrImage render_model_input(const FrameView& frame, const ModelInputMapping& mapping);

}  // namespace spg::detect
