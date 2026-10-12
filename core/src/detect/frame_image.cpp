#include "detect/frame_image.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "spg/error.hpp"

namespace spg::detect {

namespace {

constexpr double kIntMax = static_cast<double>(std::numeric_limits<int>::max());

struct PixelBox {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

bool is_whole(float value) noexcept {
  return std::isfinite(value) && std::floor(value) == value;
}

// The crop as integer pixels, or ContractError when it has a fractional edge
// or reaches outside the frame. Sub-pixel crops would need resampling that the
// mapping does not describe.
PixelBox whole_pixel_crop(const Rect& crop, Size frame) {
  if (!is_whole(crop.x) || !is_whole(crop.y) || !is_whole(crop.width) || !is_whole(crop.height)) {
    throw ContractError("frame image: model input crop must have whole-pixel edges");
  }
  const double x0 = crop.x;
  const double y0 = crop.y;
  const double x1 = x0 + static_cast<double>(crop.width);
  const double y1 = y0 + static_cast<double>(crop.height);
  if (x0 < 0.0 || y0 < 0.0 || x1 > static_cast<double>(frame.width) || y1 > static_cast<double>(frame.height)) {
    throw ContractError("frame image: model input crop reaches outside the frame");
  }
  return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(crop.width), static_cast<int>(crop.height)};
}

std::uint8_t saturate(double value) noexcept {
  return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

// BT.709 limited range (Y 16-235, Cb/Cr 16-240) to 8-bit BGR.
void yuv_to_bgr(int y, int u, int v, std::uint8_t* bgr) noexcept {
  const double c = 1.164384 * static_cast<double>(y - 16);
  const double d = static_cast<double>(u - 128);
  const double e = static_cast<double>(v - 128);
  bgr[0] = saturate(c + 2.112402 * d);
  bgr[1] = saturate(c - 0.213249 * d - 0.532909 * e);
  bgr[2] = saturate(c + 1.792741 * e);
}

// Decodes `box` of an NV12 or I420 frame into a packed BGR image.
cv::Mat decode_yuv(const FrameView& frame, const PixelBox& box) {
  cv::Mat out(box.height, box.width, CV_8UC3);
  const bool nv12 = frame.format == PixelFormat::NV12;
  for (int row = 0; row < box.height; ++row) {
    const auto y = static_cast<std::size_t>(box.y + row);
    const std::uint8_t* luma = frame.planes[0] + y * frame.strides[0];
    const std::uint8_t* chroma_u = frame.planes[1] + (y / 2) * frame.strides[1];
    const std::uint8_t* chroma_v = nv12 ? chroma_u + 1 : frame.planes[2] + (y / 2) * frame.strides[2];
    std::uint8_t* dst = out.ptr<std::uint8_t>(row);
    for (int col = 0; col < box.width; ++col) {
      const auto x = static_cast<std::size_t>(box.x + col);
      const std::size_t c = nv12 ? 2 * (x / 2) : x / 2;
      yuv_to_bgr(luma[x], chroma_u[c], chroma_v[c], dst + 3 * static_cast<std::size_t>(col));
    }
  }
  return out;
}

}  // namespace

BgrImage render_model_input(const FrameView& frame, const ModelInputMapping& mapping) {
  validate(frame);
  const Size input = mapping.input();
  if (static_cast<double>(frame.width) > kIntMax || static_cast<double>(frame.height) > kIntMax ||
      static_cast<double>(input.width) > kIntMax || static_cast<double>(input.height) > kIntMax) {
    throw ContractError("frame image: frame or model input is too large");
  }
  const PixelBox crop = whole_pixel_crop(mapping.crop(), frame.size());
  const int input_w = static_cast<int>(input.width);
  const int input_h = static_cast<int>(input.height);

  // Where the crop lands in the model input: [pad, pad + crop * scale).
  const auto place = [](double pad, double length, int limit) {
    const double lo = std::clamp(std::round(pad), 0.0, static_cast<double>(limit));
    const double hi = std::clamp(std::round(pad + length), lo, static_cast<double>(limit));
    return std::pair<int, int>{static_cast<int>(lo), static_cast<int>(hi - lo)};
  };
  const auto [left, content_w] = place(mapping.pad_x(), crop.width * mapping.scale_x(), input_w);
  const auto [top, content_h] = place(mapping.pad_y(), crop.height * mapping.scale_y(), input_h);

  BgrImage image;
  image.size = input;
  image.pixels.assign(3 * static_cast<std::size_t>(input.width) * input.height, 0);
  if (content_w == 0 || content_h == 0) {
    return image;
  }
  cv::Mat canvas(input_h, input_w, CV_8UC3, image.pixels.data());
  // A header into `canvas`: OpenCV writes in place because size and type match.
  cv::Mat content = canvas(cv::Rect(left, top, content_w, content_h));
  const cv::Size target(content_w, content_h);
  const int interpolation =
      (content_w <= crop.width && content_h <= crop.height) ? cv::INTER_AREA : cv::INTER_LINEAR;

  if (frame.format == PixelFormat::BGRA) {
    // OpenCV only reads `source`; its constructor takes a non-const pointer.
    const cv::Mat source(static_cast<int>(frame.height), static_cast<int>(frame.width), CV_8UC4,
                         const_cast<std::uint8_t*>(frame.planes[0]), frame.strides[0]);
    cv::Mat scaled;
    cv::resize(source(cv::Rect(crop.x, crop.y, crop.width, crop.height)), scaled, target, 0.0, 0.0,
               interpolation);
    cv::cvtColor(scaled, content, cv::COLOR_BGRA2BGR);
  } else {
    cv::resize(decode_yuv(frame, crop), content, target, 0.0, 0.0, interpolation);
  }
  if (content.data != canvas.ptr<std::uint8_t>(top) + 3 * static_cast<std::size_t>(left)) {
    throw ContractError("frame image: resize did not write into the model input");  // cannot happen
  }
  return image;
}

}  // namespace spg::detect
