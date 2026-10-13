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
#include "detect/tiling.hpp"
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

// Whether `dim` (possibly dynamic, -1) can take `want`.
bool fits(std::int64_t dim, std::int64_t want) {
  return dim < 0 || dim == want;
}

void check_model(const runtime::Session& session, Size tile) {
  const auto rows = static_cast<std::int64_t>(tile.height);
  const auto cols = static_cast<std::int64_t>(tile.width);
  const auto& inputs = session.inputs();
  if (inputs.size() != 1 || !inputs[0].float32) {
    throw TextDetectorError("text detector: expected one float32 input, model has " +
                            std::to_string(inputs.size()) + " input(s)");
  }
  const auto& in = inputs[0].shape;
  if (in.size() != 4 || !fits(in[0], 1) || in[1] != 3 || !fits(in[2], rows) || !fits(in[3], cols)) {
    throw TextDetectorError("text detector: input '" + inputs[0].name + "' has shape " + shape_text(in) +
                            ", expected [1, 3, " + std::to_string(rows) + ", " + std::to_string(cols) +
                            "] or dynamic");
  }
  const auto& outputs = session.outputs();
  if (outputs.size() != 1 || !outputs[0].float32) {
    throw TextDetectorError("text detector: expected one float32 output, model has " +
                            std::to_string(outputs.size()) + " output(s)");
  }
  const auto& out = outputs[0].shape;
  if (out.size() != 4 || !fits(out[0], 1) || !fits(out[1], 1) || !fits(out[2], rows) || !fits(out[3], cols)) {
    throw TextDetectorError("text detector: output '" + outputs[0].name + "' has shape " + shape_text(out) +
                            ", expected [1, 1, " + std::to_string(rows) + ", " + std::to_string(cols) +
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
  const auto side_ok = [](std::uint32_t side) {
    return side >= kInputDivisor && side <= kMaxTile && side % kInputDivisor == 0;
  };
  if (!side_ok(config.tile_width)) {
    throw TextDetectorError("text detector: tile_width must be a multiple of 32 in [32, 8192]");
  }
  if (!side_ok(config.tile_height)) {
    throw TextDetectorError("text detector: tile_height must be a multiple of 32 in [32, 8192]");
  }
  if (config.tile_overlap > std::min(config.tile_width, config.tile_height) / 2) {
    throw TextDetectorError("text detector: tile_overlap must be at most half the shorter tile side");
  }
  if (config.upscale_side < 1 || config.upscale_side > kMaxTile) {
    throw TextDetectorError("text detector: upscale_side must be in [1, 8192]");
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
  return tile_crops(readback, Size{tile, tile}, overlap);
}

std::vector<Rect> tile_crops(Size readback, Size tile, std::uint32_t overlap) {
  try {
    return grid_crops(readback, tile, overlap);
  } catch (const std::invalid_argument& error) {
    throw TextDetectorError(std::string("text detector: ") + error.what());
  }
}

ModelInputMapping tile_input_mapping(const Rect& crop, std::uint32_t tile) {
  return ModelInputMapping(crop, Size{tile, tile}, Fit::LetterboxTopLeft);
}

ModelInputMapping tile_input_mapping(const Rect& crop, Size tile, std::uint32_t upscale_side) {
  const double longer = std::max(static_cast<double>(crop.width), static_cast<double>(crop.height));
  const double up = longer > 0.0 ? static_cast<double>(upscale_side) / longer : 1.0;
  return ModelInputMapping(crop, tile, Fit::LetterboxTopLeft, std::max(1.0, up));
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

// Validates `config` before paying for a session, fixes the input to the one
// tile shape detect() runs, and gives a default warmup. Every run is one
// tile, so fixing the shape costs nothing, and it is what lets Core ML take
// the graph: with the PP-OCR model's dynamic input it ran almost wholly on CPU
// (SPG-16: about 50 ms per 640 x 640 tile on the dev Mac, 18 ms fixed).
runtime::SessionOptions detector_options(const TextDetectorConfig& config, runtime::SessionOptions options) {
  validate(config);
  const std::vector<std::int64_t> tile{1, 3, static_cast<std::int64_t>(config.tile_height),
                                       static_cast<std::int64_t>(config.tile_width)};
  if (options.fixed_shapes.empty()) {
    options.fixed_shapes = {tile};
  }
  if (options.warmup_shapes.empty()) {
    options.warmup_shapes = {tile};
  }
  return options;
}

// Session::open for the detector: a model whose input cannot take the tile
// shape (InvalidInput while fixing or warming it up) is not a DBNet detector
// for this config, which open() reports as TextDetectorError.
template <class Model>
runtime::Session open_session(runtime::Environment& env, const Model& model, const runtime::SessionOptions& options) {
  try {
    return runtime::Session::open(env, model, options);
  } catch (const runtime::RuntimeError& error) {
    if (error.code() == runtime::ErrorCode::InvalidInput) {
      throw TextDetectorError(std::string("text detector: the model does not take the tile shape: ") + error.what());
    }
    throw;
  }
}

}  // namespace

TextDetector TextDetector::open(runtime::Environment& env, const std::filesystem::path& model,
                                const TextDetectorConfig& config, runtime::SessionOptions options) {
  options = detector_options(config, std::move(options));
  return TextDetector(open_session(env, model, options), config);
}

TextDetector TextDetector::open(runtime::Environment& env, const runtime::ModelBytes& model,
                                const TextDetectorConfig& config, runtime::SessionOptions options) {
  options = detector_options(config, std::move(options));
  return TextDetector(open_session(env, model, options), config);
}

TextDetector::TextDetector(runtime::Session session, const TextDetectorConfig& config)
    : session_(std::move(session)), config_(config) {
  validate(config_);
  check_model(session_, Size{config_.tile_width, config_.tile_height});
}

std::vector<Detection> TextDetector::detect(const FrameView& frame, Size source) const {
  validate(frame);
  const ReadbackMapping readback(source, frame.size());
  const Size tile{config_.tile_width, config_.tile_height};
  const auto rows = static_cast<std::int64_t>(tile.height);
  const auto cols = static_cast<std::int64_t>(tile.width);
  const std::array<std::int64_t, 4> shape{1, 3, rows, cols};
  const std::array<std::int64_t, 4> want{1, 1, rows, cols};

  std::vector<TileDetection> found;
  const std::vector<Rect> crops = tile_crops(frame.size(), tile, config_.tile_overlap);
  for (std::size_t t = 0; t < crops.size(); ++t) {
    const SpaceMap map{readback, tile_input_mapping(crops[t], tile, config_.upscale_side)};
    const std::vector<float> tensor = to_text_input_tensor(render_model_input(frame, *map.model_input));
    const std::array<runtime::TensorView, 1> inputs{{{tensor, shape}}};
    const std::vector<runtime::Tensor> outputs = session_.run(inputs);
    if (outputs.size() != 1 ||
        !std::equal(outputs[0].shape.begin(), outputs[0].shape.end(), want.begin(), want.end())) {
      throw db::DecodeError("text detector: output has shape " +
                            (outputs.empty() ? std::string("(none)") : shape_text(outputs[0].shape)) +
                            ", expected " + shape_text(want));
    }
    const std::vector<db::Candidate> candidates = db::decode(outputs[0].data, tile, config_.db);
    std::vector<TileDetection> boxes = tile_to_source(candidates, map, t);
    found.insert(found.end(), boxes.begin(), boxes.end());
  }
  return merge_across_tiles(found, config_.merge_line_overlap);
}

}  // namespace spg::detect
