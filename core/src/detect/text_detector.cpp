#include "detect/text_detector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <utility>

#include "detect/source_clip.hpp"
#include "spg/error.hpp"

namespace spg::detect {

namespace {

// DBNet downsamples by 32, so the input side must be a multiple of it.
constexpr std::uint32_t kInputDivisor = 32;
constexpr std::uint32_t kMaxTile = 1U << 13;

std::string shape_text(std::span<const std::int64_t> shape) {
  std::string text = "[";
  for (std::size_t i = 0; i < shape.size(); ++i) {
    text += (i == 0 ? "" : ", ") + (shape[i] < 0 ? std::string("?") : std::to_string(shape[i]));
  }
  return text + "]";
}

// Tile start positions along one axis of `length` pixels.
std::vector<std::uint32_t> axis_starts(std::uint32_t length, std::uint32_t tile, std::uint32_t overlap) {
  if (length <= tile) {
    return {0};
  }
  // Fewest tiles whose step (length - tile) / (n - 1) is at most tile - overlap.
  const std::uint64_t step = tile - overlap;
  const std::uint64_t n = (static_cast<std::uint64_t>(length) - overlap + step - 1) / step;
  const std::uint64_t travel = length - tile;
  std::vector<std::uint32_t> starts;
  starts.reserve(static_cast<std::size_t>(n));
  // Floored positions: consecutive gaps are at most ceil(travel / (n - 1)),
  // which is at most the integer tile - overlap.
  for (std::uint64_t i = 0; i < n; ++i) {
    starts.push_back(static_cast<std::uint32_t>(i * travel / (n - 1)));
  }
  return starts;
}

// Whether `dim` (possibly dynamic, -1) can take `want`.
bool fits(std::int64_t dim, std::int64_t want) {
  return dim < 0 || dim == want;
}

void check_model(const runtime::Session& session, std::uint32_t tile) {
  const auto side = static_cast<std::int64_t>(tile);
  const auto& inputs = session.inputs();
  if (inputs.size() != 1 || !inputs[0].float32) {
    throw TextDetectorError("text detector: expected one float32 input, model has " +
                            std::to_string(inputs.size()) + " input(s)");
  }
  const auto& in = inputs[0].shape;
  if (in.size() != 4 || !fits(in[0], 1) || in[1] != 3 || !fits(in[2], side) || !fits(in[3], side)) {
    throw TextDetectorError("text detector: input '" + inputs[0].name + "' has shape " + shape_text(in) +
                            ", expected [1, 3, " + std::to_string(tile) + ", " + std::to_string(tile) +
                            "] or dynamic");
  }
  const auto& outputs = session.outputs();
  if (outputs.size() != 1 || !outputs[0].float32) {
    throw TextDetectorError("text detector: expected one float32 output, model has " +
                            std::to_string(outputs.size()) + " output(s)");
  }
  const auto& out = outputs[0].shape;
  if (out.size() != 4 || !fits(out[0], 1) || !fits(out[1], 1) || !fits(out[2], side) || !fits(out[3], side)) {
    throw TextDetectorError("text detector: output '" + outputs[0].name + "' has shape " + shape_text(out) +
                            ", expected [1, 1, " + std::to_string(tile) + ", " + std::to_string(tile) +
                            "] or dynamic; is it a DBNet detector?");
  }
}

// Smallest rectangle holding both (computed in double, far edges rounded out).
Rect unite(const Rect& a, const Rect& b) {
  const float x0 = std::min(a.x, b.x);
  const float y0 = std::min(a.y, b.y);
  const double x1 = std::max(static_cast<double>(a.x) + a.width, static_cast<double>(b.x) + b.width);
  const double y1 = std::max(static_cast<double>(a.y) + a.height, static_cast<double>(b.y) + b.height);
  const auto span = [](float origin, double far) {
    auto length = static_cast<float>(far - static_cast<double>(origin));
    while (static_cast<double>(origin) + static_cast<double>(length) < far) {
      length = std::nextafter(length, std::numeric_limits<float>::infinity());
    }
    return length;
  };
  return {x0, y0, span(x0, x1), span(y0, y1)};
}

// Whether two boxes are the same line: the grown boxes intersect with positive
// area and the kernels overlap vertically by at least `line_overlap` of the
// shorter kernel.
bool same_line(const TileDetection& a, const TileDetection& b, float line_overlap) {
  const Rect overlap = intersection(a.detection.box.rect, b.detection.box.rect);
  if (!(overlap.width > 0.0F && overlap.height > 0.0F)) {
    return false;
  }
  const double top = std::max(static_cast<double>(a.line.y), static_cast<double>(b.line.y));
  const double bottom = std::min(static_cast<double>(a.line.y) + a.line.height,
                                 static_cast<double>(b.line.y) + b.line.height);
  const double shorter = std::min(a.line.height, b.line.height);
  return shorter > 0.0 && bottom - top >= static_cast<double>(line_overlap) * shorter;
}

}  // namespace

void validate(const TextDetectorConfig& config) {
  if (config.tile_size < kInputDivisor || config.tile_size > kMaxTile || config.tile_size % kInputDivisor != 0) {
    throw TextDetectorError("text detector: tile_size must be a multiple of 32 in [32, 8192]");
  }
  if (config.tile_overlap > config.tile_size / 2) {
    throw TextDetectorError("text detector: tile_overlap must be at most tile_size / 2");
  }
  if (!(std::isfinite(config.merge_line_overlap) && config.merge_line_overlap > 0.0F &&
        config.merge_line_overlap <= 1.0F)) {
    throw TextDetectorError("text detector: merge_line_overlap must be in (0, 1]");
  }
  try {
    db::validate(config.db);
  } catch (const std::invalid_argument& error) {
    throw TextDetectorError(std::string("text detector: ") + error.what());
  }
}

std::vector<Rect> tile_crops(Size readback, std::uint32_t tile, std::uint32_t overlap) {
  if (readback.width == 0 || readback.height == 0) {
    throw ContractError("text detector: readback size must be non-zero");
  }
  if (tile == 0 || overlap >= tile) {
    throw TextDetectorError("text detector: tile overlap must be less than the tile size");
  }
  const std::vector<std::uint32_t> xs = axis_starts(readback.width, tile, overlap);
  const std::vector<std::uint32_t> ys = axis_starts(readback.height, tile, overlap);
  const float w = static_cast<float>(std::min(readback.width, tile));
  const float h = static_cast<float>(std::min(readback.height, tile));
  std::vector<Rect> crops;
  crops.reserve(xs.size() * ys.size());
  for (const std::uint32_t y : ys) {
    for (const std::uint32_t x : xs) {
      crops.push_back({static_cast<float>(x), static_cast<float>(y), w, h});
    }
  }
  return crops;
}

ModelInputMapping tile_input_mapping(const Rect& crop, std::uint32_t tile) {
  return ModelInputMapping(crop, Size{tile, tile}, Fit::LetterboxTopLeft);
}

std::vector<float> to_text_input_tensor(const BgrImage& image) {
  const std::size_t plane = static_cast<std::size_t>(image.size.width) * image.size.height;
  if (image.pixels.size() != 3 * plane) {
    throw ContractError("text detector: image buffer does not match its size");
  }
  std::vector<float> tensor(3 * plane);
  for (std::size_t i = 0; i < plane; ++i) {
    for (std::size_t c = 0; c < 3; ++c) {
      tensor[c * plane + i] = static_cast<float>(image.pixels[3 * i + c]) / 127.5F - 1.0F;
    }
  }
  return tensor;
}

std::vector<TileDetection> tile_to_source(std::span<const db::Candidate> candidates, const SpaceMap& map,
                                          std::size_t tile) {
  const Size source = map.readback.source();
  std::vector<TileDetection> detections;
  detections.reserve(candidates.size());
  for (const db::Candidate& candidate : candidates) {
    const Box box = convert(candidate.box, Space::Source, map);
    const std::optional<Rect> clipped = clip_to(box.rect, source);
    if (!clipped) {
      continue;
    }
    Detection detection{{*clipped, Space::Source}, DetectionKind::TextRegion,
                        std::clamp(candidate.score, 0.0F, 1.0F), DetectorId::PpOcrDet};
    validate(detection);
    detections.push_back({detection, tile, convert(candidate.line, Space::Source, map).rect});
  }
  return detections;
}

std::vector<Detection> merge_across_tiles(std::span<const TileDetection> detections, float line_overlap) {
  const std::size_t n = detections.size();
  std::vector<std::size_t> parent(n);
  std::iota(parent.begin(), parent.end(), std::size_t{0});
  const auto root = [&](std::size_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  // Sweep in y: two boxes can intersect with positive area only when the lower
  // one starts above the upper one's bottom edge.
  const auto rect = [&](std::size_t i) -> const Rect& { return detections[i].detection.box.rect; };
  std::vector<std::size_t> by_top(n);
  std::iota(by_top.begin(), by_top.end(), std::size_t{0});
  std::sort(by_top.begin(), by_top.end(), [&](std::size_t a, std::size_t b) { return rect(a).y < rect(b).y; });
  for (std::size_t k = 0; k < n; ++k) {
    const std::size_t i = by_top[k];
    const double bottom = static_cast<double>(rect(i).y) + rect(i).height;
    for (std::size_t m = k + 1; m < n && static_cast<double>(rect(by_top[m]).y) < bottom; ++m) {
      const std::size_t j = by_top[m];
      if (detections[i].tile != detections[j].tile && same_line(detections[i], detections[j], line_overlap)) {
        parent[root(j)] = root(i);
      }
    }
  }
  std::vector<std::optional<Detection>> groups(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::optional<Detection>& group = groups[root(i)];
    const Detection& d = detections[i].detection;
    if (!group) {
      group = d;
    } else {
      group->box.rect = unite(group->box.rect, d.box.rect);
      group->score = std::max(group->score, d.score);
    }
  }
  std::vector<Detection> merged;
  for (const std::optional<Detection>& group : groups) {
    if (group) {
      merged.push_back(*group);
    }
  }
  std::sort(merged.begin(), merged.end(), [](const Detection& a, const Detection& b) {
    return std::pair{a.box.rect.y, a.box.rect.x} < std::pair{b.box.rect.y, b.box.rect.x};
  });
  return merged;
}

namespace {

// Validates `config` before paying for a session and gives a default warmup.
runtime::SessionOptions detector_options(const TextDetectorConfig& config, runtime::SessionOptions options) {
  validate(config);
  if (options.warmup_shapes.empty()) {
    const auto side = static_cast<std::int64_t>(config.tile_size);
    options.warmup_shapes = {{1, 3, side, side}};
  }
  return options;
}

}  // namespace

TextDetector TextDetector::open(runtime::Environment& env, const std::filesystem::path& model,
                                const TextDetectorConfig& config, runtime::SessionOptions options) {
  options = detector_options(config, std::move(options));
  return TextDetector(runtime::Session::open(env, model, options), config);
}

TextDetector TextDetector::open(runtime::Environment& env, const runtime::ModelBytes& model,
                                const TextDetectorConfig& config, runtime::SessionOptions options) {
  options = detector_options(config, std::move(options));
  return TextDetector(runtime::Session::open(env, model, options), config);
}

TextDetector::TextDetector(runtime::Session session, const TextDetectorConfig& config)
    : session_(std::move(session)), config_(config) {
  validate(config_);
  check_model(session_, config_.tile_size);
}

std::vector<Detection> TextDetector::detect(const FrameView& frame, Size source) const {
  validate(frame);
  const ReadbackMapping readback(source, frame.size());
  const std::uint32_t tile = config_.tile_size;
  const auto side = static_cast<std::int64_t>(tile);
  const std::array<std::int64_t, 4> shape{1, 3, side, side};
  const std::array<std::int64_t, 4> want{1, 1, side, side};

  std::vector<TileDetection> found;
  const std::vector<Rect> crops = tile_crops(frame.size(), tile, config_.tile_overlap);
  for (std::size_t t = 0; t < crops.size(); ++t) {
    const SpaceMap map{readback, tile_input_mapping(crops[t], tile)};
    const std::vector<float> tensor = to_text_input_tensor(render_model_input(frame, *map.model_input));
    const std::array<runtime::TensorView, 1> inputs{{{tensor, shape}}};
    const std::vector<runtime::Tensor> outputs = session_.run(inputs);
    if (outputs.size() != 1 ||
        !std::equal(outputs[0].shape.begin(), outputs[0].shape.end(), want.begin(), want.end())) {
      throw db::DecodeError("text detector: output has shape " +
                            (outputs.empty() ? std::string("(none)") : shape_text(outputs[0].shape)) +
                            ", expected " + shape_text(want));
    }
    const std::vector<db::Candidate> candidates = db::decode(outputs[0].data, Size{tile, tile}, config_.db);
    std::vector<TileDetection> boxes = tile_to_source(candidates, map, t);
    found.insert(found.end(), boxes.begin(), boxes.end());
  }
  return merge_across_tiles(found, config_.merge_line_overlap);
}

}  // namespace spg::detect
