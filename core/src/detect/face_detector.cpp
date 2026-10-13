#include "detect/face_detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "detect/source_clip.hpp"
#include "detect/tiling.hpp"
#include "spg/error.hpp"

namespace spg::detect {

namespace {

constexpr std::array<std::string_view, 3> kOutputKinds{"cls", "obj", "bbox"};
constexpr std::array<std::int64_t, 3> kOutputWidth{1, 1, 4};

std::string shape_text(std::span<const std::int64_t> shape) {
  std::string text = "[";
  for (std::size_t i = 0; i < shape.size(); ++i) {
    text += (i == 0 ? "" : ", ") + (shape[i] < 0 ? std::string("?") : std::to_string(shape[i]));
  }
  return text + "]";
}

void check_config(const FaceDetectorConfig& config) {
  const auto unit = [](float v) { return std::isfinite(v) && v >= 0.0F && v <= 1.0F; };
  if (!unit(config.score_threshold)) {
    throw FaceDetectorError("face detector: score_threshold must be in [0, 1]");
  }
  if (!unit(config.nms_iou_threshold)) {
    throw FaceDetectorError("face detector: nms_iou_threshold must be in [0, 1]");
  }
  if (config.top_k == 0) {
    throw FaceDetectorError("face detector: top_k must be at least 1");
  }
  if (config.tile_cadence < 1 || config.tile_cadence > 8) {
    throw FaceDetectorError("face detector: tile_cadence must be in [1, 8]");
  }
}

// The float length from `origin` that reaches at least `far`.
float outward_length(float origin, double far) {
  auto length = static_cast<float>(far - static_cast<double>(origin));
  while (static_cast<double>(origin) + static_cast<double>(length) < far) {
    length = std::nextafter(length, std::numeric_limits<float>::infinity());
  }
  return length;
}

void check_overlap(const FaceDetectorConfig& config, Size input) {
  if (config.tiles && config.tile_overlap >= std::min(input.width, input.height)) {
    throw FaceDetectorError("face detector: tile_overlap must be below the input's shorter side");
  }
}

// Joins the passes' boxes, best score first (ties by position, so the result
// does not depend on the order the passes ran in): a box overlapping a kept one
// by more than `iou_threshold` is merged into it as their hull. A tile can see
// only part of a face that a tile edge cuts; dropping the whole-frame box for
// that part would uncover the rest, so overlapping boxes only grow.
std::vector<Detection> merge_passes(std::vector<Detection> detections, float iou_threshold) {
  std::sort(detections.begin(), detections.end(), [](const Detection& a, const Detection& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return std::pair{a.box.rect.y, a.box.rect.x} < std::pair{b.box.rect.y, b.box.rect.x};
  });
  std::vector<Detection> kept;
  for (const Detection& d : detections) {
    const auto overlapping = std::find_if(kept.begin(), kept.end(), [&](const Detection& k) {
      return iou(k.box.rect, d.box.rect) > iou_threshold;
    });
    if (overlapping == kept.end()) {
      kept.push_back(d);
      continue;
    }
    Rect& r = overlapping->box.rect;
    const Rect& b = d.box.rect;
    const float x0 = std::min(r.x, b.x);
    const float y0 = std::min(r.y, b.y);
    const double x1 = std::max(static_cast<double>(r.x) + r.width, static_cast<double>(b.x) + b.width);
    const double y1 = std::max(static_cast<double>(r.y) + r.height, static_cast<double>(b.y) + b.height);
    r = Rect{x0, y0, outward_length(x0, x1), outward_length(y0, y1)};
  }
  return kept;
}

// The model's static input size, or FaceDetectorError.
Size check_input(const runtime::Session& session) {
  const auto& inputs = session.inputs();
  if (inputs.size() != 1 || !inputs[0].float32) {
    throw FaceDetectorError("face detector: expected one float32 input, model has " +
                            std::to_string(inputs.size()) + " input(s)");
  }
  const auto& shape = inputs[0].shape;
  constexpr std::int64_t kMaxSide = 1 << 14;
  const bool fits = shape.size() == 4 && shape[0] == 1 && shape[1] == 3 && shape[2] > 0 && shape[3] > 0 &&
                    shape[2] <= kMaxSide && shape[3] <= kMaxSide && shape[2] % yunet::kInputDivisor == 0 &&
                    shape[3] % yunet::kInputDivisor == 0;
  if (!fits) {
    throw FaceDetectorError("face detector: input '" + inputs[0].name + "' has shape " + shape_text(shape) +
                            ", expected static [1, 3, H, W] with H and W multiples of 32");
  }
  return {static_cast<std::uint32_t>(shape[3]), static_cast<std::uint32_t>(shape[2])};
}

}  // namespace

ModelInputMapping face_input_mapping(Size readback, Size input) {
  const Rect whole{0.0F, 0.0F, static_cast<float>(readback.width), static_cast<float>(readback.height)};
  return ModelInputMapping(whole, input, Fit::LetterboxTopLeft);
}

std::vector<float> to_input_tensor(const BgrImage& image) {
  const std::size_t plane = static_cast<std::size_t>(image.size.width) * image.size.height;
  if (image.pixels.size() != 3 * plane) {
    throw ContractError("face detector: image buffer does not match its size");
  }
  std::vector<float> tensor(3 * plane);
  for (std::size_t i = 0; i < plane; ++i) {
    for (std::size_t c = 0; c < 3; ++c) {
      tensor[c * plane + i] = static_cast<float>(image.pixels[3 * i + c]);
    }
  }
  return tensor;
}

std::vector<Detection> to_source_detections(std::span<const yunet::Candidate> candidates, const SpaceMap& map) {
  const Size source = map.readback.source();
  std::vector<Detection> detections;
  detections.reserve(candidates.size());
  for (const yunet::Candidate& candidate : candidates) {
    const Box box = convert(candidate.box, Space::Source, map);
    const std::optional<Rect> clipped = clip_to(box.rect, source);
    if (!clipped) {
      continue;
    }
    Detection detection{{*clipped, Space::Source}, DetectionKind::Face, std::clamp(candidate.score, 0.0F, 1.0F),
                        DetectorId::YuNet};
    validate(detection);
    detections.push_back(detection);
  }
  return detections;
}

std::array<yunet::StrideOutputs, yunet::kStrides.size()> stride_outputs(std::span<const runtime::Tensor> outputs,
                                                                        const OutputIndex& index, Size input) {
  std::array<yunet::StrideOutputs, yunet::kStrides.size()> strides;
  for (std::size_t s = 0; s < yunet::kStrides.size(); ++s) {
    const auto stride = static_cast<std::uint32_t>(yunet::kStrides[s]);
    const std::int64_t cells =
        static_cast<std::int64_t>(input.width / stride) * static_cast<std::int64_t>(input.height / stride);
    std::array<std::span<const float>, kOutputKinds.size()> data;
    for (std::size_t k = 0; k < kOutputKinds.size(); ++k) {
      const std::string name = std::string(kOutputKinds[k]) + "_" + std::to_string(stride);
      if (index[s][k] >= outputs.size()) {
        throw yunet::DecodeError("yunet: output '" + name + "' is missing from the run");
      }
      const runtime::Tensor& tensor = outputs[index[s][k]];
      const std::array<std::int64_t, 3> want{1, cells, kOutputWidth[k]};
      if (!std::equal(tensor.shape.begin(), tensor.shape.end(), want.begin(), want.end())) {
        throw yunet::DecodeError("yunet: output '" + name + "' has shape " + shape_text(tensor.shape) +
                                 ", expected " + shape_text(want));
      }
      data[k] = tensor.data;
    }
    strides[s] = {data[0], data[1], data[2]};
  }
  return strides;
}

FaceDetector FaceDetector::open(runtime::Environment& env, const std::filesystem::path& model,
                                const FaceDetectorConfig& config, const runtime::SessionOptions& options) {
  check_config(config);  // before paying for a session
  return FaceDetector(runtime::Session::open(env, model, options), config);
}

FaceDetector FaceDetector::open(runtime::Environment& env, const runtime::ModelBytes& model,
                                const FaceDetectorConfig& config, const runtime::SessionOptions& options) {
  check_config(config);  // before paying for a session
  return FaceDetector(runtime::Session::open(env, model, options), config);
}

FaceDetector::FaceDetector(runtime::Session session, const FaceDetectorConfig& config)
    : session_(std::move(session)), config_(config) {
  check_config(config_);
  input_ = check_input(session_);
  check_overlap(config_, input_);
  const auto& outputs = session_.outputs();
  for (std::size_t s = 0; s < yunet::kStrides.size(); ++s) {
    const auto stride = static_cast<std::int64_t>(yunet::kStrides[s]);
    const std::int64_t cells = (static_cast<std::int64_t>(input_.width) / stride) *
                               (static_cast<std::int64_t>(input_.height) / stride);
    for (std::size_t k = 0; k < kOutputKinds.size(); ++k) {
      const std::string name = std::string(kOutputKinds[k]) + "_" + std::to_string(stride);
      const auto found = std::find_if(outputs.begin(), outputs.end(),
                                      [&](const runtime::TensorInfo& info) { return info.name == name; });
      if (found == outputs.end()) {
        throw FaceDetectorError("face detector: model has no output '" + name + "'; is it YuNet?");
      }
      // Dynamic (-1) dimensions are allowed here; stride_outputs() checks the
      // real shapes on every run.
      const auto& shape = found->shape;
      const auto matches = [](std::int64_t dim, std::int64_t want) { return dim < 0 || dim == want; };
      if (!found->float32 || shape.size() != 3 || !matches(shape[0], 1) || !matches(shape[1], cells) ||
          !matches(shape[2], kOutputWidth[k])) {
        throw FaceDetectorError("face detector: output '" + name + "' has shape " + shape_text(shape) +
                                ", expected float32 [1, " + std::to_string(cells) + ", " +
                                std::to_string(kOutputWidth[k]) + "]");
      }
      output_index_[s][k] = static_cast<std::size_t>(found - outputs.begin());
    }
  }
}

std::vector<Detection> FaceDetector::detect(const FrameView& frame, Size source) const {
  validate(frame);
  const ReadbackMapping readback(source, frame.size());
  const std::array<std::int64_t, 4> shape{1, 3, static_cast<std::int64_t>(input_.height),
                                          static_cast<std::int64_t>(input_.width)};
  // One model run on the part of the readback `input_map` letterboxes into the input.
  const auto run = [&](const ModelInputMapping& input_map) {
    const SpaceMap map{readback, input_map};
    const std::vector<float> tensor = to_input_tensor(render_model_input(frame, *map.model_input));
    const std::array<runtime::TensorView, 1> inputs{{{tensor, shape}}};
    const std::vector<runtime::Tensor> outputs = session_.run(inputs);
    const auto strides = stride_outputs(outputs, output_index_, input_);
    std::vector<yunet::Candidate> candidates = yunet::decode(strides, input_, config_.score_threshold);
    candidates = yunet::suppress(std::move(candidates), config_.nms_iou_threshold, config_.top_k);
    return to_source_detections(candidates, map);
  };

  std::vector<Detection> found = run(face_input_mapping(frame.size(), input_));
  const Size readback_size = frame.size();
  if (!config_.tiles || (readback_size.width <= input_.width && readback_size.height <= input_.height)) {
    return found;
  }
  const std::uint32_t tile = std::min(input_.width, input_.height);
  const std::vector<Rect> crops = grid_crops(readback_size, tile, config_.tile_overlap);
  const std::uint64_t share = tile_calls_->fetch_add(1, std::memory_order_relaxed) % config_.tile_cadence;
  for (std::size_t i = 0; i < crops.size(); ++i) {
    if (i % config_.tile_cadence != share) {
      continue;
    }
    std::vector<Detection> more = run(ModelInputMapping(crops[i], input_, Fit::LetterboxTopLeft));
    found.insert(found.end(), more.begin(), more.end());
  }
  return merge_passes(std::move(found), config_.nms_iou_threshold);
}

}  // namespace spg::detect
