#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace spg {

// The coordinate space a box is expressed in. Every space is pixels with the
// origin at the top-left corner, x to the right and y down.
//   Source     full-resolution pixels of the filtered OBS source
//   Readback   the downscaled copy of the whole source that workers scan
//   ModelInput one model's input tensor: a crop of the readback frame (a tile
//              or the whole frame) stretched or letterboxed to the input size
// Only core/src/frame/geometry.cpp converts between spaces.
enum class Space : std::uint8_t { Source, Readback, ModelInput };

[[nodiscard]] std::string_view to_string(Space space) noexcept;

struct Size {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  friend bool operator==(const Size&, const Size&) = default;
};

// Axis-aligned rectangle. Valid when every field is finite and width and
// height are not negative.
struct Rect {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  friend bool operator==(const Rect&, const Rect&) = default;
};

// A rectangle tagged with its coordinate space.
struct Box {
  Rect rect;
  Space space = Space::Source;
  friend bool operator==(const Box&, const Box&) = default;
};

// Integer pixel rectangle, used where masks meet real pixels.
struct PixelRect {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

[[nodiscard]] bool is_valid(const Rect& rect) noexcept;
[[nodiscard]] float area(const Rect& rect) noexcept;
// Overlap of two rectangles; zero width and height when they do not overlap.
[[nodiscard]] Rect intersection(const Rect& a, const Rect& b) noexcept;
// Intersection over union in [0, 1]; 0 when both are empty.
[[nodiscard]] float iou(const Rect& a, const Rect& b) noexcept;

// Smallest pixel rectangle that fully contains `rect`, clipped to `bounds`.
// Rounds outward, so converting a mask to pixels never uncovers a pixel the
// float rectangle touched.
[[nodiscard]] PixelRect enclosing_pixels(const Rect& rect, Size bounds) noexcept;

// Source <-> Readback: the readback frame is the whole source scaled to
// `readback` (aspect ratio may change).
class ReadbackMapping {
 public:
  // Throws ContractError when either size has a zero dimension.
  ReadbackMapping(Size source, Size readback);
  [[nodiscard]] Size source() const noexcept { return source_; }
  [[nodiscard]] Size readback() const noexcept { return readback_; }

 private:
  Size source_;
  Size readback_;
};

// How a crop is resized into a model input.
enum class Fit : std::uint8_t {
  Stretch,           // independent x and y scale, no padding
  LetterboxCenter,   // uniform scale, content centered, padding split evenly
  LetterboxTopLeft,  // uniform scale, content at the origin, padding right/bottom
};

// Readback <-> ModelInput: `crop` (in Readback pixels) is resized into an
// input of size `input`. model = (readback - crop.origin) * scale + pad.
class ModelInputMapping {
 public:
  // Throws ContractError when the crop is invalid or empty or the input size
  // has a zero dimension.
  ModelInputMapping(Rect crop, Size input, Fit fit);
  [[nodiscard]] Rect crop() const noexcept { return crop_; }
  [[nodiscard]] Size input() const noexcept { return input_; }
  [[nodiscard]] Fit fit() const noexcept { return fit_; }
  [[nodiscard]] double scale_x() const noexcept { return scale_x_; }
  [[nodiscard]] double scale_y() const noexcept { return scale_y_; }
  [[nodiscard]] double pad_x() const noexcept { return pad_x_; }
  [[nodiscard]] double pad_y() const noexcept { return pad_y_; }

 private:
  Rect crop_;
  Size input_;
  Fit fit_;
  double scale_x_ = 1.0;
  double scale_y_ = 1.0;
  double pad_x_ = 0.0;
  double pad_y_ = 0.0;
};

// Every mapping in effect for one detector pass over one frame.
// `model_input` is empty when the pass does not involve a model input.
struct SpaceMap {
  ReadbackMapping readback;
  std::optional<ModelInputMapping> model_input;
};

// Converts `box` into `target` space. Computes in double and rounds outward
// when narrowing to float: the origin rounds down and the far edge
// (origin + length, summed in double as enclosing_pixels does) rounds up, so
// the result contains the double-precision image and a converted mask never
// loses an edge pixel. Does not clip: a box in letterbox padding maps outside
// the source and callers clip where they need to. Throws ContractError when
// the box is invalid, the result is outside the float range, or a ModelInput
// conversion is requested without a model input mapping.
[[nodiscard]] Box convert(const Box& box, Space target, const SpaceMap& map);

}  // namespace spg
