#pragma once

// Synthetic frames for detector tests: a BGR canvas with simple shape drawing
// and conversion to the FrameView formats the core accepts. Nothing here comes
// from a photo; the "face" is a drawn cartoon (ellipses), so no real person's
// likeness is used or committed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "spg/frame.hpp"

namespace spg::test {

struct Bgr {
  std::uint8_t b = 0;
  std::uint8_t g = 0;
  std::uint8_t r = 0;
};

class Canvas {
 public:
  Canvas(std::uint32_t width, std::uint32_t height, Bgr fill = {})
      : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height, fill) {}

  [[nodiscard]] std::uint32_t width() const { return width_; }
  [[nodiscard]] std::uint32_t height() const { return height_; }
  [[nodiscard]] Bgr at(std::uint32_t x, std::uint32_t y) const { return pixels_[index(x, y)]; }
  void set(std::uint32_t x, std::uint32_t y, Bgr color) { pixels_[index(x, y)] = color; }

  void rect(double x0, double y0, double x1, double y1, Bgr color) {
    for_each_pixel([&](double px, double py) { return px >= x0 && px < x1 && py >= y0 && py < y1; }, color);
  }

  void ellipse(double cx, double cy, double rx, double ry, Bgr color) {
    for_each_pixel(
        [&](double px, double py) {
          const double dx = (px + 0.5 - cx) / rx;
          const double dy = (py + 0.5 - cy) / ry;
          return dx * dx + dy * dy <= 1.0;
        },
        color);
  }

  // A front-facing cartoon face of nominal size `s` (face width) centered at
  // (cx, cy): hair, skin oval, eyes with pupils, brows, nose and mouth.
  void face(double cx, double cy, double s) {
    const Bgr hair{40, 50, 70};
    ellipse(cx, cy - 0.15 * s, 0.62 * s, 0.75 * s, hair);
    ellipse(cx, cy, 0.5 * s, 0.65 * s, {150, 180, 225});
    for (const double side : {-0.2, 0.2}) {
      ellipse(cx + side * s, cy - 0.12 * s, 0.11 * s, 0.06 * s, {255, 255, 255});
      ellipse(cx + side * s, cy - 0.12 * s, 0.05 * s, 0.05 * s, {60, 40, 30});
      ellipse(cx + side * s, cy - 0.27 * s, 0.13 * s, 0.025 * s, hair);
    }
    ellipse(cx, cy + 0.08 * s, 0.05 * s, 0.1 * s, {120, 150, 200});
    ellipse(cx, cy + 0.32 * s, 0.18 * s, 0.05 * s, {80, 80, 170});
  }

 private:
  [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y) const {
    return static_cast<std::size_t>(y) * width_ + x;
  }

  template <typename Inside>
  void for_each_pixel(Inside inside, Bgr color) {
    for (std::uint32_t y = 0; y < height_; ++y) {
      for (std::uint32_t x = 0; x < width_; ++x) {
        if (inside(static_cast<double>(x), static_cast<double>(y))) {
          set(x, y, color);
        }
      }
    }
  }

  std::uint32_t width_;
  std::uint32_t height_;
  std::vector<Bgr> pixels_;
};

// Owns the planes a FrameView points into. Rows carry `kPad` extra bytes
// filled with 0xFF, so a reader that ignores the stride sees white garbage.
struct OwnedFrame {
  static constexpr std::uint32_t kPad = 13;
  std::vector<std::vector<std::uint8_t>> planes;
  FrameView view;
};

namespace detail {

inline std::uint8_t clamp_byte(double v) {
  return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
}

// BT.709 limited range, the inverse of what spg::detect decodes.
struct Yuv {
  double y;
  double u;
  double v;
};
inline Yuv to_yuv(Bgr c) {
  const double luma = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
  return {16.0 + 219.0 * luma / 255.0, 128.0 + 224.0 * (c.b - luma) / (1.8556 * 255.0),
          128.0 + 224.0 * (c.r - luma) / (1.5748 * 255.0)};
}

}  // namespace detail

inline OwnedFrame to_frame(const Canvas& canvas, PixelFormat format) {
  const std::uint32_t w = canvas.width();
  const std::uint32_t h = canvas.height();
  const std::uint32_t cw = (w + 1) / 2;
  const std::uint32_t ch = (h + 1) / 2;
  OwnedFrame frame;
  frame.view.width = w;
  frame.view.height = h;
  frame.view.format = format;
  const auto add_plane = [&](std::uint32_t row_bytes, std::uint32_t rows) {
    const std::uint32_t stride = row_bytes + OwnedFrame::kPad;
    frame.planes.emplace_back(static_cast<std::size_t>(stride) * rows, std::uint8_t{0xFF});
    frame.view.strides[frame.planes.size() - 1] = stride;
    return stride;
  };

  if (format == PixelFormat::BGRA) {
    const std::uint32_t stride = add_plane(4 * w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w; ++x) {
        const Bgr c = canvas.at(x, y);
        std::uint8_t* p = frame.planes[0].data() + static_cast<std::size_t>(y) * stride + 4 * x;
        p[0] = c.b;
        p[1] = c.g;
        p[2] = c.r;
        p[3] = 255;
      }
    }
  } else {
    const std::uint32_t y_stride = add_plane(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w; ++x) {
        frame.planes[0][static_cast<std::size_t>(y) * y_stride + x] = detail::clamp_byte(detail::to_yuv(canvas.at(x, y)).y);
      }
    }
    const bool nv12 = format == PixelFormat::NV12;
    const std::uint32_t u_stride = add_plane(nv12 ? 2 * cw : cw, ch);
    const std::uint32_t v_stride = nv12 ? 0 : add_plane(cw, ch);
    for (std::uint32_t cy = 0; cy < ch; ++cy) {
      for (std::uint32_t cx = 0; cx < cw; ++cx) {
        // Average the (up to) 2x2 block this chroma sample covers.
        double u = 0.0;
        double v = 0.0;
        int n = 0;
        for (std::uint32_t y = 2 * cy; y < std::min(2 * cy + 2, h); ++y) {
          for (std::uint32_t x = 2 * cx; x < std::min(2 * cx + 2, w); ++x) {
            const detail::Yuv yuv = detail::to_yuv(canvas.at(x, y));
            u += yuv.u;
            v += yuv.v;
            ++n;
          }
        }
        const std::size_t row = cy;
        if (nv12) {
          frame.planes[1][row * u_stride + 2 * cx] = detail::clamp_byte(u / n);
          frame.planes[1][row * u_stride + 2 * cx + 1] = detail::clamp_byte(v / n);
        } else {
          frame.planes[1][row * u_stride + cx] = detail::clamp_byte(u / n);
          frame.planes[2][row * v_stride + cx] = detail::clamp_byte(v / n);
        }
      }
    }
  }
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    frame.view.planes[p] = frame.planes[p].data();
  }
  return frame;
}

}  // namespace spg::test
