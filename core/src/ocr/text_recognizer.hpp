#pragma once

// Text recognizer (SPG-07, lane:vision): PP-OCR CTC recognition on ONNX
// Runtime for the text regions the detector (SPG-06) finds.
//
//   Source-space TextRegion --spg::convert()--> Readback box
//   --enclosing_pixels()--> whole-pixel crop --plan_reads()--> views (whole,
//   per line, rotated) in chunks --render_model_input()--> 48 x W images
//   --batched model runs--> [N, W / 8, classes] --ctc_greedy_decode()-->
//   one Secret per region
//
// Preprocessing follows RapidOCR v3.9.2 TextRecognizer.resize_norm_img: each
// crop is scaled to the input height keeping its aspect ratio, normalized to
// (v / 255 - 0.5) / 0.5 in B, G, R plane order, and padded on the right with
// zeros (mid gray) to the batch width, at least min_width. Crops of a batch
// are sorted by width first, as RapidOCR sorts by aspect ratio.
//
// Recognized text is secret-adjacent: it is built in SecureText buffers and
// returned in spg::Secret, both protected memory that is zeroed when released
// (SPG-15d; what stays outside it is in docs/policy.md, "Recognized text in
// memory"). It is never logged and never appears in an error. The model's
// output probabilities are zeroed once decoded. No OBS, no globals, no logging;
// nothing here touches the network or the disk except opening the model file.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ocr/crop_plan.hpp"
#include "ocr/ctc_decode.hpp"
#include "runtime/onnx_session.hpp"
#include "spg/detection.hpp"
#include "spg/frame.hpp"
#include "spg/geometry.hpp"
#include "spg/secret.hpp"

namespace spg::ocr {

struct TextRecognizerConfig {
  // Model input height in pixels (PP-OCRv4 rec: 48).
  std::uint32_t input_height = 48;
  // The model emits one time step per this many input columns (PP-OCRv4 rec:
  // 8, measured on the pinned model: width 320 -> 40 steps). Input widths are
  // rounded up to a multiple of it.
  std::uint32_t time_stride = 8;
  // Narrowest model input; shorter crops are padded (RapidOCR: 320).
  std::uint32_t min_width = 320;
  // Widest model input (RapidOCR has no cap; a cap bounds one run's cost). A
  // view wider than this once scaled to input_height is read in chunks of
  // max_width that overlap by chunk_overlap of a chunk. With the default 0.5
  // any stretch of text up to half a chunk wide (1280 input px, about 50
  // characters) lies whole in some chunk, so a card number, an email address
  // or a watch-list entry is never read only in pieces. A longer stretch is
  // read in pieces of more than half a chunk each, which the policy's partial
  // reveal rule matches for watch-list entries.
  std::uint32_t max_width = 2560;
  float chunk_overlap = 0.5F;
  // Height / width ratio from which a region is also read turned 90 degrees.
  float tall_ratio = 1.5F;
  // Crops per model run. Runs group crops of similar width. 1 by default: on
  // the dev Mac (ORT 1.30.0 CPU, M4) the pinned model took about twice as long
  // per crop in batches of 2 to 8 as one at a time (4.5 ms vs 7.5 to 12 ms
  // per 48x320 crop; agent-log SPG-07).
  std::size_t max_batch = 1;
  // Model runs in flight at once in one recognize() call, in [1, 16]: the
  // calling thread plus up to parallel_runs - 1 it starts and joins before
  // returning. A 48x320 crop scales poorly over ONNX Runtime's threads (dev
  // Mac: 4.0 ms on 4 threads, 9.4 ms on 1), so a frame with many lines reads
  // faster with crops side by side (SPG-16: chat frames, about 25 regions).
  // Thread budget: each scheduler worker inside recognize() adds up to
  // parallel_runs - 1 threads, so the default two workers can have 8 runs
  // in flight on one session, all sharing that session's ONNX Runtime
  // intra-op pool (SessionOptions::intra_op_threads).
  std::size_t parallel_runs = 4;
  LineSplitParams lines;
};

// Thrown for a model that is not a CTC text recognizer (inputs, outputs or
// the 'character' metadata), or a config out of range. Messages carry tensor
// names, shapes and field names only.
class TextRecognizerError : public std::runtime_error {
 public:
  explicit TextRecognizerError(const std::string& what) : std::runtime_error(what) {}
};

// Throws TextRecognizerError naming the field when `config` is out of range.
void validate(const TextRecognizerConfig& config);

// The whole-pixel readback crop of a Source-space detection box: the box
// converted with spg::convert() (rounded outward) and then to enclosing pixels
// clipped to the frame. Empty (zero width or height) when the box lies
// outside the frame. Throws ContractError when the box is not in Source space
// or not valid.
[[nodiscard]] PixelRect readback_crop(const Box& box, const ReadbackMapping& readback);

// Planar float tensor [n, 3, input_height, width] in B, G, R plane order,
// each image's pixels (v / 255 - 0.5) / 0.5 at the left, zeros to the right.
// Every image must be input_height tall and at most `width` wide.
[[nodiscard]] std::vector<float> to_recognizer_tensor(std::span<const detect::BgrImage> images,
                                                      std::uint32_t input_height, std::uint32_t width);

// What the recognizer read in one region.
struct Recognition {
  // The texts of the region's views (whole region, each line, rotations),
  // joined by '\n', and the chunks of one view by ' ' in reading order. Chunks
  // overlap (see TextRecognizerConfig::max_width), so text in an overlap is
  // read twice and text at a seam may be cut there; nothing is de-duplicated.
  // Text no wider than the overlap is always whole in some chunk. Empty when
  // nothing was read. Never filtered by score.
  Secret text;
  // Mean probability of every decoded character, in [0, 1]; 0 when empty.
  float score = 0.0F;
};

class TextRecognizer {
 public:
  // Opens `model` with `options` (default providers, CPU fallback, one warmup
  // run). When options.warmup_shapes is empty it warms up with one crop
  // [1, 3, input_height, min_width]. Throws runtime::RuntimeError when the
  // session cannot be created and TextRecognizerError when the model or config
  // is not usable.
  [[nodiscard]] static TextRecognizer open(runtime::Environment& env, const std::filesystem::path& model,
                                           const TextRecognizerConfig& config = {},
                                           runtime::SessionOptions options = {});
  // The same from bytes already read and checked (runtime::ModelBytes).
  [[nodiscard]] static TextRecognizer open(runtime::Environment& env, const runtime::ModelBytes& model,
                                           const TextRecognizerConfig& config = {},
                                           runtime::SessionOptions options = {});

  // Takes an open session. Throws TextRecognizerError unless the model has
  // one float32 input (shape undeclared, or [N, 3, H, W] with dynamic or
  // fitting dimensions: N dynamic or 1 with max_batch 1, H == input_height,
  // W dynamic), one float32 output (undeclared, or [N, T, C] with C dynamic or
  // equal to the alphabet size) and a 'character' metadata dictionary. The
  // output must be per-step probabilities (the graph ends in softmax): raw
  // logits make every recognize() call throw DecodeError, failing closed.
  explicit TextRecognizer(runtime::Session session, const TextRecognizerConfig& config = {});

  // Reads each region of `frame`, which holds Readback pixels: the whole
  // source of size `source` scaled to frame.size(). Regions are Source-space
  // detections (normally TextRegion). Returns one Recognition per region, in
  // order; a region outside the frame reads as empty. Safe to call from
  // several threads at once.
  //
  // Cost: one model run per max_batch read jobs; a region is at least one job
  // (one more per extra line, two more when tall, and more when a view is
  // chunked), up to parallel_runs of them at once on threads this call starts
  // and joins. No deadline or cancel check inside: call it on a worker thread
  // and fail the frame closed when it misses the lookahead deadline. The
  // result does not depend on parallel_runs.
  //
  // Throws on any failure so the caller can fail the frame closed:
  // ContractError for an invalid frame, source or region box,
  // runtime::RuntimeError when inference fails, DecodeError when an output
  // has the wrong shape or holds a value that is not a finite probability.
  [[nodiscard]] std::vector<Recognition> recognize(const FrameView& frame, Size source,
                                                   std::span<const Detection> regions) const;

  [[nodiscard]] const TextRecognizerConfig& config() const noexcept { return config_; }
  [[nodiscard]] const Alphabet& alphabet() const noexcept { return alphabet_; }
  [[nodiscard]] const runtime::Session& session() const noexcept { return session_; }

 private:
  runtime::Session session_;
  TextRecognizerConfig config_;
  Alphabet alphabet_;
};

}  // namespace spg::ocr
