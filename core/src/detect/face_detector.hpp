#pragma once

// Face detector (SPG-05, lane:vision): YuNet on ONNX Runtime.
//
//   frame (Readback pixels) --letterbox--> model input --YuNet--> candidates
//   (ModelInput) --NMS--> spg::convert() --> Detection boxes in Source space
//
// The whole readback frame is letterboxed (uniform scale, content at the top
// left, black padding right and bottom) into the model's fixed input, as
// OpenCV's FaceDetectorYN pads. Every coordinate conversion goes through
// spg::convert() with the mapping that produced the input. No OBS, no globals,
// no logging; nothing here touches the network or the disk except opening the
// model file.

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "detect/frame_image.hpp"
#include "detect/yunet.hpp"
#include "runtime/onnx_session.hpp"
#include "spg/detection.hpp"
#include "spg/frame.hpp"
#include "spg/geometry.hpp"

namespace spg::detect {

struct FaceDetectorConfig {
  // Minimum sqrt(class * objectness) score kept. OpenCV's demo uses 0.9; SPG
  // starts lower because a missed face airs (recall over precision). SPG-12
  // tunes it against the faces suite.
  float score_threshold = 0.6F;
  // Overlapping candidates with IoU above this collapse to the best one.
  float nms_iou_threshold = 0.3F;
  // At most this many candidates enter NMS (OpenCV's top_k default).
  std::size_t top_k = 5000;
};

// Thrown for a model whose inputs or outputs are not YuNet's, or a config out
// of range. Messages carry tensor names and shapes only.
class FaceDetectorError : public std::runtime_error {
 public:
  explicit FaceDetectorError(const std::string& what) : std::runtime_error(what) {}
};

// The mapping detect() uses: the whole readback frame letterboxed top-left
// into the model input.
[[nodiscard]] ModelInputMapping face_input_mapping(Size readback, Size input);

// Planar float tensor [1, 3, H, W] in B, G, R plane order with raw 0-255
// values, which is what YuNet expects (cv::dnn::blobFromImage defaults).
[[nodiscard]] std::vector<float> to_input_tensor(const BgrImage& image);

// Converts candidates to Source-space face detections with spg::convert(),
// clips them to the source, and drops any that fall wholly outside it (a box
// in letterbox padding). Keeps the input order.
[[nodiscard]] std::vector<Detection> to_source_detections(std::span<const yunet::Candidate> candidates,
                                                          const SpaceMap& map);

class FaceDetector {
 public:
  // Opens `model` with `options` (default providers, CPU fallback, one warmup
  // run). Throws runtime::RuntimeError when the session cannot be created and
  // FaceDetectorError when the model or config is not usable.
  [[nodiscard]] static FaceDetector open(runtime::Environment& env, const std::filesystem::path& model,
                                         const FaceDetectorConfig& config = {},
                                         const runtime::SessionOptions& options = {});

  // Takes an open session. Throws FaceDetectorError unless it has one float32
  // input of static shape [1, 3, H, W] (H and W multiples of 32) and YuNet's
  // cls_, obj_ and bbox_ outputs for strides 8, 16 and 32, and unless
  // the config is in range (thresholds in [0, 1], top_k at least 1).
  explicit FaceDetector(runtime::Session session, const FaceDetectorConfig& config = {});

  // Detects faces in `frame`, which holds Readback pixels: the whole source of
  // size `source` scaled to frame.size(). Pass source == frame.size() when the
  // frame is not scaled. Returns Source-space detections (kind Face, detector
  // YuNet) by descending score; an empty vector means no face scored above the
  // threshold. Safe to call from several threads at once.
  //
  // Throws on any failure so the caller can fail the frame closed:
  // ContractError for an invalid frame or zero-size source,
  // runtime::RuntimeError when inference fails, yunet::DecodeError when the
  // outputs are malformed or not finite.
  [[nodiscard]] std::vector<Detection> detect(const FrameView& frame, Size source) const;

  [[nodiscard]] Size input_size() const noexcept { return input_; }
  [[nodiscard]] const FaceDetectorConfig& config() const noexcept { return config_; }
  [[nodiscard]] const runtime::Session& session() const noexcept { return session_; }

 private:
  runtime::Session session_;
  FaceDetectorConfig config_;
  Size input_;
  // Index into session outputs of cls, obj, bbox for each stride.
  std::array<std::array<std::size_t, 3>, yunet::kStrides.size()> output_index_{};
};

}  // namespace spg::detect
