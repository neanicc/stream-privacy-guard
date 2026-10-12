#include "ocr/crop_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace spg::ocr {

namespace {

// Start offsets of chunks of `chunk` pixels along `length` pixels: the first
// at 0, the last flush with the end, neighbours overlapping by at least
// `overlap`. Requires chunk < length and overlap < chunk.
std::vector<std::uint32_t> chunk_starts(std::uint32_t length, std::uint32_t chunk, std::uint32_t overlap) {
  const std::uint64_t step = chunk - overlap;
  const std::uint64_t n = (static_cast<std::uint64_t>(length) - overlap + step - 1) / step;
  const std::uint64_t travel = length - chunk;
  std::vector<std::uint32_t> starts;
  starts.reserve(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n; ++i) {
    starts.push_back(static_cast<std::uint32_t>(i * travel / (n - 1)));
  }
  return starts;
}

std::uint32_t scaled_width(double length, double thickness, std::uint32_t height, std::uint32_t max_width) {
  const double width = std::ceil(static_cast<double>(height) * length / thickness);
  return static_cast<std::uint32_t>(std::clamp(width, 1.0, static_cast<double>(max_width)));
}

}  // namespace

void validate(const LineSplitParams& params) {
  if (!(params.gap_energy >= 0.0F && params.gap_energy < 1.0F)) {
    throw std::invalid_argument("line split: gap_energy must be in [0, 1)");
  }
  if (!(params.min_line_fraction >= 0.0F && params.min_line_fraction <= 1.0F)) {
    throw std::invalid_argument("line split: min_line_fraction must be in [0, 1]");
  }
}

std::vector<RowRange> split_lines(const detect::BgrImage& crop, const LineSplitParams& params) {
  validate(params);
  const std::uint32_t width = crop.size.width;
  const std::uint32_t height = crop.size.height;
  const std::vector<RowRange> whole{{0, height}};
  if (crop.pixels.size() != 3 * static_cast<std::size_t>(width) * height) {
    throw std::invalid_argument("line split: image buffer does not match its size");
  }
  if (width < 2 || height < 3) {
    return whole;
  }

  std::vector<double> energy(height, 0.0);
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint8_t* row = crop.pixels.data() + 3 * static_cast<std::size_t>(width) * y;
    double previous = 0.114 * row[0] + 0.587 * row[1] + 0.299 * row[2];
    for (std::uint32_t x = 1; x < width; ++x) {
      const std::uint8_t* p = row + 3 * static_cast<std::size_t>(x);
      const double luma = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
      energy[y] += std::abs(luma - previous);
      previous = luma;
    }
  }
  const double busiest = *std::max_element(energy.begin(), energy.end());
  if (!(busiest > 0.0)) {
    return whole;
  }
  const double threshold = static_cast<double>(params.gap_energy) * busiest;

  // Runs of text rows.
  std::vector<RowRange> runs;
  for (std::uint32_t y = 0; y < height; ++y) {
    if (energy[y] <= threshold) {
      continue;
    }
    if (!runs.empty() && runs.back().end == y) {
      runs.back().end = y + 1;
    } else {
      runs.push_back({y, y + 1});
    }
  }

  // Join runs too short to be a line to the neighbour across the smaller gap.
  const auto rows = [](const RowRange& r) { return r.end - r.begin; };
  while (runs.size() > 1) {
    const std::uint32_t tallest = rows(*std::max_element(
        runs.begin(), runs.end(), [&](const RowRange& a, const RowRange& b) { return rows(a) < rows(b); }));
    const auto shortest = std::min_element(runs.begin(), runs.end(),
                                           [&](const RowRange& a, const RowRange& b) { return rows(a) < rows(b); });
    if (static_cast<double>(rows(*shortest)) >= static_cast<double>(params.min_line_fraction) * tallest) {
      break;
    }
    const auto i = static_cast<std::size_t>(shortest - runs.begin());
    std::size_t other = 0;
    if (i == 0) {
      other = 1;
    } else if (i + 1 == runs.size()) {
      other = i - 1;
    } else {
      const std::uint32_t gap_above = runs[i].begin - runs[i - 1].end;
      const std::uint32_t gap_below = runs[i + 1].begin - runs[i].end;
      other = gap_above <= gap_below ? i - 1 : i + 1;
    }
    const std::size_t keep = std::min(i, other);
    runs[keep] = {std::min(runs[i].begin, runs[other].begin), std::max(runs[i].end, runs[other].end)};
    runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(std::max(i, other)));
  }
  if (runs.size() < 2) {
    return whole;
  }

  // Cut in the middle of each gap; the bands cover every row.
  std::vector<RowRange> bands;
  bands.reserve(runs.size());
  std::uint32_t begin = 0;
  for (std::size_t i = 1; i < runs.size(); ++i) {
    const std::uint32_t cut = runs[i - 1].end + (runs[i].begin - runs[i - 1].end) / 2;
    bands.push_back({begin, cut});
    begin = cut;
  }
  bands.push_back({begin, height});
  return bands;
}

detect::BgrImage rotate(const detect::BgrImage& image, Rotation rotation) {
  if (rotation == Rotation::None) {
    return image;
  }
  const std::uint32_t w = image.size.width;
  const std::uint32_t h = image.size.height;
  if (image.pixels.size() != 3 * static_cast<std::size_t>(w) * h) {
    throw std::invalid_argument("rotate: image buffer does not match its size");
  }
  detect::BgrImage out;
  out.size = {h, w};
  out.pixels.resize(image.pixels.size());
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      // Counter-clockwise: (x, y) -> (y, w - 1 - x). Clockwise: (h - 1 - y, x).
      const std::uint32_t nx = rotation == Rotation::CounterClockwise ? y : h - 1 - y;
      const std::uint32_t ny = rotation == Rotation::CounterClockwise ? w - 1 - x : x;
      const std::size_t from = 3 * (static_cast<std::size_t>(y) * w + x);
      const std::size_t to = 3 * (static_cast<std::size_t>(ny) * h + nx);
      std::copy_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(from), 3,
                  out.pixels.begin() + static_cast<std::ptrdiff_t>(to));
    }
  }
  return out;
}

std::vector<ReadJob> plan_reads(const PixelRect& region, const std::vector<RowRange>& lines,
                                const ViewParams& params) {
  if (region.width <= 0 || region.height <= 0) {
    throw std::invalid_argument("plan reads: region must not be empty");
  }
  const auto w = static_cast<std::uint32_t>(region.width);
  const auto h = static_cast<std::uint32_t>(region.height);

  std::vector<std::pair<PixelRect, Rotation>> views{{region, Rotation::None}};
  if (lines.size() >= 2) {
    for (const RowRange& line : lines) {
      if (line.begin >= line.end || line.end > h) {
        throw std::invalid_argument("plan reads: line band outside the region");
      }
      views.push_back({{region.x, region.y + static_cast<std::int32_t>(line.begin), region.width,
                        static_cast<std::int32_t>(line.end - line.begin)},
                       Rotation::None});
    }
  }
  if (static_cast<double>(h) >= static_cast<double>(params.tall_ratio) * w) {
    views.push_back({region, Rotation::CounterClockwise});
    views.push_back({region, Rotation::Clockwise});
  }

  std::vector<ReadJob> jobs;
  for (std::size_t v = 0; v < views.size(); ++v) {
    const auto& [rect, rotation] = views[v];
    const bool upright = rotation == Rotation::None;
    const auto length = static_cast<std::uint32_t>(upright ? rect.width : rect.height);
    const auto thickness = static_cast<std::uint32_t>(upright ? rect.height : rect.width);
    const double full = std::ceil(static_cast<double>(params.input_height) * length / thickness);
    if (full <= static_cast<double>(params.max_width)) {
      jobs.push_back({rect, rotation, scaled_width(length, thickness, params.input_height, params.max_width), v});
      continue;
    }
    const auto chunk = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::floor(static_cast<double>(params.max_width) * thickness /
                                                 params.input_height)));
    const auto overlap = std::min<std::uint32_t>(
        chunk - 1, static_cast<std::uint32_t>(std::floor(static_cast<double>(params.chunk_overlap) * chunk)));
    std::vector<std::uint32_t> starts = chunk_starts(length, chunk, overlap);
    if (rotation == Rotation::Clockwise) {
      std::reverse(starts.begin(), starts.end());  // reads bottom to top
    }
    const std::uint32_t width = scaled_width(chunk, thickness, params.input_height, params.max_width);
    for (const std::uint32_t s : starts) {
      PixelRect piece = rect;
      if (upright) {
        piece.x += static_cast<std::int32_t>(s);
        piece.width = static_cast<std::int32_t>(chunk);
      } else {
        piece.y += static_cast<std::int32_t>(s);
        piece.height = static_cast<std::int32_t>(chunk);
      }
      jobs.push_back({piece, rotation, width, v});
    }
  }
  return jobs;
}

}  // namespace spg::ocr
