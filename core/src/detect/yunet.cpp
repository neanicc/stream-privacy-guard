#include "detect/yunet.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace spg::detect::yunet {

std::vector<Candidate> decode(std::span<const StrideOutputs, kStrides.size()> outputs, Size input,
                              float score_threshold) {
  if (input.width == 0 || input.height == 0 || input.width % kInputDivisor != 0 ||
      input.height % kInputDivisor != 0) {
    throw DecodeError("yunet: input size must be a non-zero multiple of 32");
  }
  std::vector<Candidate> candidates;
  for (std::size_t s = 0; s < kStrides.size(); ++s) {
    const auto stride = static_cast<std::size_t>(kStrides[s]);
    const std::size_t cols = input.width / stride;
    const std::size_t rows = input.height / stride;
    const std::size_t cells = rows * cols;
    const StrideOutputs& out = outputs[s];
    if (out.cls.size() != cells || out.obj.size() != cells || out.bbox.size() != 4 * cells) {
      throw DecodeError("yunet: stride " + std::to_string(stride) + " outputs have " +
                        std::to_string(out.cls.size()) + "/" + std::to_string(out.obj.size()) + "/" +
                        std::to_string(out.bbox.size()) + " values, expected " + std::to_string(cells) + "/" +
                        std::to_string(cells) + "/" + std::to_string(4 * cells));
    }
    const double step = static_cast<double>(stride);
    for (std::size_t idx = 0; idx < cells; ++idx) {
      const float cls = out.cls[idx];
      const float obj = out.obj[idx];
      if (!std::isfinite(cls) || !std::isfinite(obj)) {
        throw DecodeError("yunet: non-finite score at stride " + std::to_string(stride));
      }
      const float score = std::sqrt(std::clamp(cls, 0.0F, 1.0F) * std::clamp(obj, 0.0F, 1.0F));
      if (score < score_threshold) {
        continue;
      }
      const std::span<const float> b = out.bbox.subspan(4 * idx, 4);
      const double cx = (static_cast<double>(idx % cols) + static_cast<double>(b[0])) * step;
      const double cy = (static_cast<double>(idx / cols) + static_cast<double>(b[1])) * step;
      const double w = std::exp(static_cast<double>(b[2])) * step;
      const double h = std::exp(static_cast<double>(b[3])) * step;
      const Rect rect{static_cast<float>(cx - w / 2.0), static_cast<float>(cy - h / 2.0), static_cast<float>(w),
                      static_cast<float>(h)};
      if (!is_valid(rect)) {
        throw DecodeError("yunet: non-finite box at stride " + std::to_string(stride));
      }
      candidates.push_back({{rect, Space::ModelInput}, score});
    }
  }
  return candidates;
}

std::vector<Candidate> suppress(std::vector<Candidate> candidates, float iou_threshold, std::size_t top_k) {
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
  if (candidates.size() > top_k) {
    candidates.resize(top_k);
  }
  std::vector<Candidate> kept;
  for (const Candidate& candidate : candidates) {
    const bool overlaps = std::any_of(kept.begin(), kept.end(), [&](const Candidate& k) {
      return iou(k.box.rect, candidate.box.rect) > iou_threshold;
    });
    if (!overlaps) {
      kept.push_back(candidate);
    }
  }
  return kept;
}

}  // namespace spg::detect::yunet
