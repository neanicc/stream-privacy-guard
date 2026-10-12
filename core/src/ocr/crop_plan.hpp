#pragma once

// Crop and rectification planning for the text recognizer (SPG-07,
// lane:vision): which readback-pixel rectangles of one text region the
// recognizer reads, in which orientation, and in how many pieces. Pure
// functions, tested without a model.
//
// The detector (SPG-06) hands over axis-aligned boxes, and its cross-tile
// merge can join two adjacent lines into one box. CTC recognition reads one
// line at a time, so a region is read as a set of views, all of them, because
// a view that reads nothing useful only costs time while a missing view can
// let a secret air (recall over precision):
//
//   - the whole region, upright;
//   - each text line, when split_lines() finds more than one;
//   - the whole region turned 90 degrees both ways, when it is at least
//     tall_ratio times as tall as wide (vertical text; RapidOCR turns such
//     crops one way only, and SPG has no direction classifier).
//
// A view wider than the recognizer's max_width once scaled to the input height
// is read in chunks that overlap, so a long line is not squeezed. The texts of
// a region's views are joined by the recognizer.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "detect/frame_image.hpp"
#include "spg/geometry.hpp"

namespace spg::ocr {

// Rows [begin, end) of a crop.
struct RowRange {
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  friend bool operator==(const RowRange&, const RowRange&) = default;
};

struct LineSplitParams {
  // A row is a gap between lines when its horizontal gradient energy (sum of
  // |luma(x + 1) - luma(x)|) is at most this fraction of the busiest row's.
  float gap_energy = 0.08F;
  // A run of text rows shorter than this fraction of the tallest run (the dot
  // of an 'i', an underline) is joined to its nearest neighbour instead of
  // becoming a line of its own.
  float min_line_fraction = 0.5F;
};

// Throws std::invalid_argument naming the field unless gap_energy is in
// [0, 1) and min_line_fraction in [0, 1].
void validate(const LineSplitParams& params);

// Splits a crop holding one or more stacked text lines into line bands by
// looking for rows with (almost) no horizontal edges between rows that have
// them. The bands are contiguous and cover every row of the crop: each cut is
// in the middle of a gap. Returns one band covering the whole crop when no
// split is found (one line, a blank crop, or lines that touch).
[[nodiscard]] std::vector<RowRange> split_lines(const detect::BgrImage& crop, const LineSplitParams& params = {});

// How a view is turned before recognition. Clockwise reads the region bottom
// to top; CounterClockwise (numpy rot90, RapidOCR's choice) reads it top to
// bottom.
enum class Rotation : std::uint8_t { None, Clockwise, CounterClockwise };

// Turns an image a quarter turn.
[[nodiscard]] detect::BgrImage rotate(const detect::BgrImage& image, Rotation rotation);

struct ViewParams {
  std::uint32_t input_height = 48;  // model input height
  std::uint32_t max_width = 2560;   // widest model input a view is scaled to
  float chunk_overlap = 0.5F;       // of the chunk length, in [0, 0.5]
  float tall_ratio = 1.5F;          // height / width that adds the rotated views
};

// One piece of a region the recognizer reads: a whole-pixel readback rect,
// its rotation, and the model input width it is scaled to (the height is
// input_height). `view` numbers the views of the region in plan order; the
// chunks of one view share it and are listed in reading order.
struct ReadJob {
  PixelRect rect;
  Rotation rotation = Rotation::None;
  std::uint32_t width = 0;
  std::size_t view = 0;
};

// Every job for a region whose readback crop is `region` (non-empty) and
// whose line bands (rows of the region) are `lines`. Each job's width is in
// [1, max_width]. Every pixel of the region is in at least one job of the
// whole-region view, and chunks of a view overlap by at least
// floor(chunk_overlap * chunk length) pixels along the reading direction.
[[nodiscard]] std::vector<ReadJob> plan_reads(const PixelRect& region, const std::vector<RowRange>& lines,
                                              const ViewParams& params);

}  // namespace spg::ocr
