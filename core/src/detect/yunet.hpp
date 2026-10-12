#pragma once

// YuNet face detector output decoding and non-maximum suppression
// (lane:vision). Pure functions over plain float spans, so they are tested
// with synthetic tensors and need no model.
//
// The model (OpenCV Zoo face_detection_yunet_2023mar) predicts, for every cell
// of a grid at strides 8, 16 and 32 over its input, a class score, an
// objectness score, a box (center offset in cells, log size in cells) and five
// landmarks. Decoding follows OpenCV's FaceDetectorYN::postProcess
// (modules/objdetect/src/face_detect.cpp, OpenCV 4.x). Landmarks are not used.

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "spg/geometry.hpp"

namespace spg::detect::yunet {

inline constexpr std::array<int, 3> kStrides{8, 16, 32};
// Input width and height must be multiples of the largest stride.
inline constexpr int kInputDivisor = 32;

// Thrown when model outputs do not have the layout YuNet produces, or contain
// non-finite values. Callers treat it like any detector failure: the frame
// fails closed. Messages carry sizes and output names only.
class DecodeError : public std::runtime_error {
 public:
  explicit DecodeError(const std::string& what) : std::runtime_error(what) {}
};

// The three outputs for one stride, each row-major over the grid
// (rows = input height / stride, cols = input width / stride).
struct StrideOutputs {
  std::span<const float> cls;   // rows * cols
  std::span<const float> obj;   // rows * cols
  std::span<const float> bbox;  // rows * cols * 4: dx, dy, log w, log h
};

// One face candidate before suppression.
struct Candidate {
  Box box;  // Space::ModelInput
  float score = 0.0F;
};

// Every grid cell whose score sqrt(clamp(cls) * clamp(obj)) is at least
// `score_threshold`, as a box in ModelInput space:
//   center = ((col, row) + (dx, dy)) * stride, size = exp(log w, log h) * stride.
// `outputs` holds strides 8, 16 and 32 in that order. Throws DecodeError when
// an output size does not match `input`, when a score is not finite, or when a
// kept candidate's box is not finite (a model that emits NaN or overflows must
// fail the frame closed, not drop a face).
[[nodiscard]] std::vector<Candidate> decode(std::span<const StrideOutputs, kStrides.size()> outputs, Size input,
                                            float score_threshold);

// Greedy non-maximum suppression, as cv::dnn::NMSBoxes: candidates sorted by
// descending score (ties keep decode order), at most `top_k` considered, and a
// candidate is dropped when its IoU with an already kept one is greater than
// `iou_threshold`. Returns the kept candidates by descending score.
[[nodiscard]] std::vector<Candidate> suppress(std::vector<Candidate> candidates, float iou_threshold,
                                              std::size_t top_k);

}  // namespace spg::detect::yunet
