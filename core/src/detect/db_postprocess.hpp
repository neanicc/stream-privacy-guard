#pragma once

// DBNet (Differentiable Binarization) post-processing for the PP-OCR text
// detector (lane:vision). A pure function over a plain float span, tested with
// synthetic probability maps; no model needed.
//
// The detector outputs one text probability per input pixel. Following
// RapidOCR v3.9.2 DBPostProcess (python/rapidocr/ch_ppocr_det/utils.py, the
// packaging plan section 6 names): binarize at `bin_threshold`, dilate with a
// 2x2 kernel, take every contour (RETR_LIST), fit a minimum-area rectangle,
// drop it when its short side is under `min_size`, score it by the mean
// probability inside it ("fast" mode), drop it when the score is under
// `box_threshold`, grow it by area * unclip_ratio / perimeter on every side,
// and drop it when the grown short side is under min_size + 2.
//
// SPG differences, all in the direction of covering more:
// - Boxes are axis-aligned: the bounding box of the grown rectangle, plus one
//   pixel on the far edges because a map index i covers [i, i + 1).
// - No max_candidates cap. RapidOCR silently keeps the first 1000 contours;
//   dropping text without failing the frame would let it air.
// - Any non-finite probability throws DecodeError so the frame fails closed.

#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "spg/geometry.hpp"

namespace spg::detect::db {

// Thrown when the probability map does not match the expected size or holds a
// non-finite value. The caller fails the frame closed. Messages carry sizes
// only, never map contents.
class DecodeError : public std::runtime_error {
 public:
  explicit DecodeError(const std::string& what) : std::runtime_error(what) {}
};

// RapidOCR v3.9.2 defaults (config.yaml, Det section) except unclip_ratio.
// SPG-12 tunes them on the text suite.
struct Params {
  // A pixel is text when its probability is greater than this.
  float bin_threshold = 0.3F;
  // A box is kept when its mean probability is at least this.
  float box_threshold = 0.5F;
  // Growth distance = rect area * unclip_ratio / rect perimeter. RapidOCR
  // uses 1.6; SPG starts at 2.0 for recall: on the synthetic lines in
  // tests/integration/text_detector_test.cpp, 1.6 left a box edge up to 11%
  // of the ink height inside the ink (68 of 100 lines), 2.0 at most 2%
  // (20 lines).
  float unclip_ratio = 2.0F;
  // Minimum short side in map pixels before growth (min_size + 2 after).
  float min_size = 3.0F;
  // Dilate the binary map with a 2x2 kernel before finding contours.
  bool dilate = true;
};

// Throws std::invalid_argument naming the field unless every threshold is in
// [0, 1] and unclip_ratio and min_size are finite and not negative.
void validate(const Params& params);

struct Candidate {
  Box box;  // Space::ModelInput, axis-aligned; may reach past the map edge
  float score = 0.0F;  // mean probability inside the fitted rectangle, [0, 1]
  // Space::ModelInput: the fitted rectangle's axis-aligned bounds before
  // growth (the text kernel). Growth can make the boxes of two adjacent lines
  // overlap; their kernels do not, so merging compares these.
  Box line;
};

// Text boxes in `prob`, a row-major map of `map.width * map.height`
// probabilities covering the model input pixel for pixel. Returns candidates
// in contour order. Throws DecodeError when the span size does not match `map`
// or any value is not finite, and std::invalid_argument for invalid params.
[[nodiscard]] std::vector<Candidate> decode(std::span<const float> prob, Size map, const Params& params = {});

}  // namespace spg::detect::db
