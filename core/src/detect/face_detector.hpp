#pragma once

// Face detector (SPG-05, lane:vision): YuNet on ONNX Runtime.
//
//   frame (Readback pixels) --letterbox--> model input --YuNet--> candidates
//   (ModelInput) --NMS--> spg::convert() --> Detection boxes in Source space
//
// The whole readback frame is letterboxed (uniform scale, content at the top
// left, black padding right and bottom) into the model's fixed input, as
// OpenCV's FaceDetectorYN pads. Every coordinate conversion goes through
// spg::convert() with the mapping that produced the input.
//
// One fixed input for the whole frame scales a 1080p source by 1/3, and on the
// faces suite that missed faces of 43 and 62 Source px outright (SPG-12). So
// a frame larger than the input is also cut into input-sized tiles at scale 1
// (grid_crops() in detect/tiling.hpp, shared with the text detector), each
// run through the model, and the boxes of every pass are joined in Source
// space: a box overlapping a better one by more than nms_iou_threshold merges
// into it as their hull. Faces larger than the tile overlap that a tile edge
// cuts are left to the whole frame pass, which sees them at a size YuNet
// handles.
//
// No OBS, no globals, no logging; nothing here touches the network or the disk
// except opening the model file.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
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
  // Also run scale-1 tiles when the frame is larger than the model input.
  bool tiles = true;
  // Tiles run round robin over this many detect() calls that need them: the
  // n-th such call (from 0) runs tile i when i % tile_cadence equals
  // n % tile_cadence, so every tile runs once in tile_cadence calls and the
  // whole-frame pass runs on every call. 1 runs every tile on every call.
  // In [1, 8].
  std::uint32_t tile_cadence = 1;
  // Minimum overlap between neighbouring tiles, in readback pixels: a face
  // up to this size is whole in some tile. Below the input's shorter side.
  std::uint32_t tile_overlap = 128;
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

// Where each stride's cls, obj and bbox tensors sit in a session's outputs.
using OutputIndex = std::array<std::array<std::size_t, 3>, yunet::kStrides.size()>;

// Views of one run's outputs in decode() order (strides 8, 16, 32). Throws
// yunet::DecodeError unless every tensor `index` names exists and has exactly
// the shape YuNet produces for `input`: [1, cells, 1] for cls and obj and
// [1, cells, 4] for bbox. The model may declare dynamic output dimensions, so
// this is what stops a tensor with the right value count in another layout
// (say [1, 4, cells]) from being read as boxes. The views borrow `outputs`.
[[nodiscard]] std::array<yunet::StrideOutputs, yunet::kStrides.size()> stride_outputs(
    std::span<const runtime::Tensor> outputs, const OutputIndex& index, Size input);

class FaceDetector {
 public:
  // Opens `model` with `options` (default providers, CPU fallback, one warmup
  // run). Throws runtime::RuntimeError when the session cannot be created and
  // FaceDetectorError when the model or config is not usable.
  [[nodiscard]] static FaceDetector open(runtime::Environment& env, const std::filesystem::path& model,
                                         const FaceDetectorConfig& config = {},
                                         const runtime::SessionOptions& options = {});
  // The same from bytes already read and checked (runtime::ModelBytes).
  [[nodiscard]] static FaceDetector open(runtime::Environment& env, const runtime::ModelBytes& model,
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
  // Cost: one model run for the whole frame plus, when `tiles` is set and the
  // frame is larger than the input, one per tile of this call's share (8
  // tiles for 1920x1080 at a 640x640 input and the default overlap, split
  // over tile_cadence calls), run one after another.
  //
  // Throws on any failure so the caller can fail the frame closed:
  // ContractError for an invalid frame or zero-size source,
  // runtime::RuntimeError when inference fails, yunet::DecodeError when the
  // outputs have the wrong shape or size or are not finite.
  [[nodiscard]] std::vector<Detection> detect(const FrameView& frame, Size source) const;

  [[nodiscard]] Size input_size() const noexcept { return input_; }
  [[nodiscard]] const FaceDetectorConfig& config() const noexcept { return config_; }
  [[nodiscard]] const runtime::Session& session() const noexcept { return session_; }

 private:
  runtime::Session session_;
  FaceDetectorConfig config_;
  Size input_;
  OutputIndex output_index_{};
  // detect() calls that ran tiles, for tile_cadence. Behind a pointer so the
  // detector stays movable.
  std::unique_ptr<std::atomic<std::uint64_t>> tile_calls_ = std::make_unique<std::atomic<std::uint64_t>>(0);
};

}  // namespace spg::detect
