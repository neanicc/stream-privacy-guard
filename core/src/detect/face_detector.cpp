#include "detect/face_detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

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

// A Source-space box clipped to the source, or nothing when no part of it is
// inside. Edges are clipped in double and the far edge narrows outward, so a
// clipped box never loses a pixel the unclipped one covered inside the frame.
std::optional<Rect> clip_to(const Rect& rect, Size bounds) {
  const auto axis = [](float origin, float length, std::uint32_t limit) -> std::optional<std::pair<float, float>> {
    const double lo = std::max(static_cast<double>(origin), 0.0);
    const double hi = std::min(static_cast<double>(origin) + static_cast<double>(length), static_cast<double>(limit));
    if (!(hi > lo)) {
      return std::nullopt;
    }
    // lo is 0 or the original float origin, so it narrows exactly.
    const auto start = static_cast<float>(lo);
    auto span = static_cast<float>(hi - lo);
    while (static_cast<double>(start) + static_cast<double>(span) < hi) {
      span = std::nextafter(span, std::numeric_limits<float>::infinity());
    }
    return std::pair{start, span};
  };
  const auto x = axis(rect.x, rect.width, bounds.width);
  const auto y = axis(rect.y, rect.height, bounds.height);
  if (!x || !y) {
    return std::nullopt;
  }
  return Rect{x->first, y->first, x->second, y->second};
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

FaceDetector FaceDetector::open(runtime::Environment& env, const std::filesystem::path& model,
                                const FaceDetectorConfig& config, const runtime::SessionOptions& options) {
  check_config(config);  // before paying for a session
  return FaceDetector(runtime::Session::open(env, model, options), config);
}

FaceDetector::FaceDetector(runtime::Session session, const FaceDetectorConfig& config)
    : session_(std::move(session)), config_(config) {
  check_config(config_);
  input_ = check_input(session_);
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
      // Dynamic (-1) dimensions are allowed here; decode() checks real sizes.
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
  const SpaceMap map{ReadbackMapping(source, frame.size()), face_input_mapping(frame.size(), input_)};
  const std::vector<float> tensor = to_input_tensor(render_model_input(frame, *map.model_input));
  const std::array<std::int64_t, 4> shape{1, 3, static_cast<std::int64_t>(input_.height),
                                          static_cast<std::int64_t>(input_.width)};
  const std::array<runtime::TensorView, 1> inputs{{{tensor, shape}}};
  const std::vector<runtime::Tensor> outputs = session_.run(inputs);

  std::array<yunet::StrideOutputs, yunet::kStrides.size()> strides;
  for (std::size_t s = 0; s < strides.size(); ++s) {
    strides[s] = {outputs.at(output_index_[s][0]).data, outputs.at(output_index_[s][1]).data,
                  outputs.at(output_index_[s][2]).data};
  }
  std::vector<yunet::Candidate> candidates = yunet::decode(strides, input_, config_.score_threshold);
  candidates = yunet::suppress(std::move(candidates), config_.nms_iou_threshold, config_.top_k);
  return to_source_detections(candidates, map);
}

}  // namespace spg::detect
