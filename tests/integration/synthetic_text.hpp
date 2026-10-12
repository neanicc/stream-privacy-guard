#pragma once

// Synthetic text for text detector tests: draws made-up strings onto a
// spg::test::Canvas with OpenCV's built-in Hershey vector fonts, so every
// fixture is generated at test time from the strings in the test source. No
// image is committed and no string is real personal data.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "spg/geometry.hpp"
#include "synthetic_frames.hpp"

namespace spg::test {

// Draws `text` with its baseline-left corner at (x, baseline) in Canvas
// pixels, cap height about `height` pixels, anti-aliased over the existing
// pixels. Returns the tight box of every pixel the ink touched (Canvas
// pixels), or an empty rect when nothing landed on the canvas.
inline Rect draw_text(Canvas& canvas, const std::string& text, int x, int baseline, double height, Bgr color,
                      int font = cv::FONT_HERSHEY_SIMPLEX) {
  // Hershey Simplex capitals are about 22 units tall at scale 1 (font size 30).
  const double scale = height / 22.0;
  const int thickness = std::max(1, static_cast<int>(std::lround(height / 10.0)));
  cv::Mat ink = cv::Mat::zeros(static_cast<int>(canvas.height()), static_cast<int>(canvas.width()), CV_8UC1);
  cv::putText(ink, text, cv::Point(x, baseline), font, scale, cv::Scalar(255), thickness, cv::LINE_AA);
  int x0 = ink.cols;
  int y0 = ink.rows;
  int x1 = -1;
  int y1 = -1;
  for (int row = 0; row < ink.rows; ++row) {
    const std::uint8_t* alpha = ink.ptr<std::uint8_t>(row);
    for (int col = 0; col < ink.cols; ++col) {
      if (alpha[col] == 0) {
        continue;
      }
      const auto px = static_cast<std::uint32_t>(col);
      const auto py = static_cast<std::uint32_t>(row);
      const Bgr under = canvas.at(px, py);
      const double a = alpha[col] / 255.0;
      const auto mix = [a](std::uint8_t fg, std::uint8_t bg) {
        return static_cast<std::uint8_t>(std::lround(a * fg + (1.0 - a) * bg));
      };
      canvas.set(px, py, {mix(color.b, under.b), mix(color.g, under.g), mix(color.r, under.r)});
      x0 = std::min(x0, col);
      y0 = std::min(y0, row);
      x1 = std::max(x1, col);
      y1 = std::max(y1, row);
    }
  }
  if (x1 < 0) {
    return {};
  }
  return {static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0 + 1),
          static_cast<float>(y1 - y0 + 1)};
}

}  // namespace spg::test
