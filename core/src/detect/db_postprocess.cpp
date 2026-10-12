#include "detect/db_postprocess.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace spg::detect::db {

namespace {

// Mean of `pred` inside the rotated rectangle `rect` (RapidOCR box_score_fast):
// the rectangle is filled into a mask over its clipped pixel bounding box.
float box_score(const cv::Mat& pred, const cv::RotatedRect& rect) {
  std::array<cv::Point2f, 4> corners;
  rect.points(corners.data());
  float min_x = corners[0].x;
  float max_x = corners[0].x;
  float min_y = corners[0].y;
  float max_y = corners[0].y;
  for (const cv::Point2f& p : corners) {
    min_x = std::min(min_x, p.x);
    max_x = std::max(max_x, p.x);
    min_y = std::min(min_y, p.y);
    max_y = std::max(max_y, p.y);
  }
  const auto clip = [](float v, int hi) { return std::clamp(static_cast<int>(v), 0, hi); };
  const int x0 = clip(std::floor(min_x), pred.cols - 1);
  const int x1 = clip(std::ceil(max_x), pred.cols - 1);
  const int y0 = clip(std::floor(min_y), pred.rows - 1);
  const int y1 = clip(std::ceil(max_y), pred.rows - 1);
  cv::Mat mask = cv::Mat::zeros(y1 - y0 + 1, x1 - x0 + 1, CV_8UC1);
  std::array<cv::Point, 4> poly;
  for (std::size_t i = 0; i < corners.size(); ++i) {
    // RapidOCR casts the shifted float corners to int32 (truncation).
    poly[i] = cv::Point(static_cast<int>(corners[i].x - static_cast<float>(x0)),
                        static_cast<int>(corners[i].y - static_cast<float>(y0)));
  }
  cv::fillConvexPoly(mask, poly.data(), static_cast<int>(poly.size()), cv::Scalar(1));
  const cv::Mat roi = pred(cv::Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
  return static_cast<float>(cv::mean(roi, mask)[0]);
}

}  // namespace

void validate(const Params& params) {
  const auto unit = [](float v) { return std::isfinite(v) && v >= 0.0F && v <= 1.0F; };
  const auto non_negative = [](float v) { return std::isfinite(v) && v >= 0.0F; };
  if (!unit(params.bin_threshold)) {
    throw std::invalid_argument("db: bin_threshold must be in [0, 1]");
  }
  if (!unit(params.box_threshold)) {
    throw std::invalid_argument("db: box_threshold must be in [0, 1]");
  }
  if (!non_negative(params.unclip_ratio)) {
    throw std::invalid_argument("db: unclip_ratio must be finite and not negative");
  }
  if (!non_negative(params.min_size)) {
    throw std::invalid_argument("db: min_size must be finite and not negative");
  }
}

std::vector<Candidate> decode(std::span<const float> prob, Size map, const Params& params) {
  validate(params);
  constexpr auto kIntMax = static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (map.width == 0 || map.height == 0 || map.width > kIntMax || map.height > kIntMax) {
    throw DecodeError("db: probability map size must be non-zero and fit an int");
  }
  const std::size_t cells = static_cast<std::size_t>(map.width) * map.height;
  if (prob.size() != cells) {
    throw DecodeError("db: probability map has " + std::to_string(prob.size()) + " values, expected " +
                      std::to_string(map.width) + "x" + std::to_string(map.height));
  }
  if (!std::all_of(prob.begin(), prob.end(), [](float v) { return std::isfinite(v); })) {
    throw DecodeError("db: probability map holds a non-finite value");
  }

  const int rows = static_cast<int>(map.height);
  const int cols = static_cast<int>(map.width);
  // OpenCV only reads `pred`; its constructor takes a non-const pointer.
  const cv::Mat pred(rows, cols, CV_32FC1, const_cast<float*>(prob.data()));
  cv::Mat bitmap;
  cv::compare(pred, cv::Scalar(params.bin_threshold), bitmap, cv::CMP_GT);
  if (params.dilate) {
    cv::dilate(bitmap, bitmap, cv::Mat::ones(2, 2, CV_8UC1));
  }
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(bitmap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);

  std::vector<Candidate> candidates;
  for (const std::vector<cv::Point>& contour : contours) {
    const cv::RotatedRect rect = cv::minAreaRect(contour);
    const float short_side = std::min(rect.size.width, rect.size.height);
    if (short_side < params.min_size) {
      continue;
    }
    const float score = box_score(pred, rect);
    if (!(score >= params.box_threshold)) {
      continue;
    }
    // Unclip: grow every side by area * ratio / perimeter, in double.
    const double w = rect.size.width;
    const double h = rect.size.height;
    const double distance = w * h * static_cast<double>(params.unclip_ratio) / (2.0 * (w + h));
    if (static_cast<double>(short_side) + 2.0 * distance < static_cast<double>(params.min_size) + 2.0) {
      continue;
    }
    const cv::RotatedRect grown(rect.center, cv::Size2f(static_cast<float>(w + 2.0 * distance),
                                                        static_cast<float>(h + 2.0 * distance)),
                                rect.angle);
    std::array<cv::Point2f, 4> corners;
    grown.points(corners.data());
    double x0 = corners[0].x;
    double x1 = corners[0].x;
    double y0 = corners[0].y;
    double y1 = corners[0].y;
    for (const cv::Point2f& p : corners) {
      x0 = std::min(x0, static_cast<double>(p.x));
      x1 = std::max(x1, static_cast<double>(p.x));
      y0 = std::min(y0, static_cast<double>(p.y));
      y1 = std::max(y1, static_cast<double>(p.y));
    }
    // Map index i covers [i, i + 1): the far edges gain one pixel. Round
    // outward when narrowing to float.
    const auto down = [](double v) {
      auto f = static_cast<float>(v);
      return static_cast<double>(f) > v ? std::nextafter(f, -std::numeric_limits<float>::infinity()) : f;
    };
    const float left = down(x0);
    const float top = down(y0);
    const auto span = [](float origin, double far) {
      auto length = static_cast<float>(far - static_cast<double>(origin));
      while (static_cast<double>(origin) + static_cast<double>(length) < far) {
        length = std::nextafter(length, std::numeric_limits<float>::infinity());
      }
      return length;
    };
    const Rect box{left, top, span(left, x1 + 1.0), span(top, y1 + 1.0)};
    candidates.push_back({{box, Space::ModelInput}, std::clamp(score, 0.0F, 1.0F)});
  }
  return candidates;
}

}  // namespace spg::detect::db
