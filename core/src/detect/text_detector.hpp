#pragma once

// Text detector (SPG-06, lane:vision): PP-OCR DBNet on ONNX Runtime, run on
// fixed-size tiles of the readback frame.
//
//   frame (Readback) --tile_crops()--> tiles --render_model_input()--> model
//   input --DBNet--> probability map --db::decode()--> boxes (ModelInput)
//   --spg::convert()--> Source --clip, merge_across_tiles()--> Detections
//
// Every tile is a crop of at most tile_size x tile_size readback pixels,
// letterboxed top-left into a tile_size x tile_size input, so the model always
// sees one static shape (scale 1 for every tile of a frame at least one tile
// wide and tall; a smaller frame is scaled up uniformly to fill the input).
// Neighbouring tiles overlap by at least `tile_overlap` readback pixels, so a
// line no taller than the overlap lies whole inside some tile row, and a line
// cut by a vertical tile edge is seen in pieces that overlap and are merged.
//
// Every coordinate conversion goes through spg::convert() with the mapping
// that produced the tile. No OBS, no globals, no logging; nothing here touches
// the network or the disk except opening the model file.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "detect/db_postprocess.hpp"
#include "detect/frame_image.hpp"
#include "runtime/onnx_session.hpp"
#include "spg/detection.hpp"
#include "spg/frame.hpp"
#include "spg/geometry.hpp"

namespace spg::detect {

struct TextDetectorConfig {
  // Model input side in pixels; a multiple of 32 (DBNet downsamples by 32).
  // 640 matches the manifest's warmup shape.
  std::uint32_t tile_size = 640;
  // Minimum overlap between neighbouring tiles, in readback pixels. Lines up
  // to this tall are never cut by a horizontal tile edge. Less than tile_size.
  std::uint32_t tile_overlap = 64;
  // Boxes from different tiles are merged when they intersect and their
  // vertical overlap is at least this fraction of the shorter one (the same
  // line seen by two tiles). In (0, 1].
  float merge_line_overlap = 0.5F;
  db::Params db;
};

// Thrown for a model whose input or output is not a DBNet text detector's, or
// a config out of range. Messages carry tensor names and shapes only.
class TextDetectorError : public std::runtime_error {
 public:
  explicit TextDetectorError(const std::string& what) : std::runtime_error(what) {}
};

// Throws TextDetectorError naming the field when `config` is out of range.
void validate(const TextDetectorConfig& config);

// The tile crops (whole Readback pixels) covering a frame of size `readback`:
// a grid, row-major, each tile at most `tile` pixels on a side, first tile at
// the origin and last flush with the far edge, neighbours overlapping by at
// least `overlap` pixels (when there is more than one per axis). Throws
// ContractError for a zero size and TextDetectorError unless overlap < tile.
[[nodiscard]] std::vector<Rect> tile_crops(Size readback, std::uint32_t tile, std::uint32_t overlap);

// The mapping detect() uses for one tile: the crop letterboxed top-left into a
// `tile` x `tile` input.
[[nodiscard]] ModelInputMapping tile_input_mapping(const Rect& crop, std::uint32_t tile);

// Planar float tensor [1, 3, H, W] in B, G, R plane order, each value
// (v / 255 - 0.5) / 0.5, which is what PP-OCR det expects (RapidOCR
// DetPreProcess: OpenCV BGR, mean and std 0.5).
[[nodiscard]] std::vector<float> to_text_input_tensor(const BgrImage& image);

// A Source-space text box and the tile it came from.
struct TileDetection {
  Detection detection;
  std::size_t tile = 0;
};

// Converts one tile's candidates to Source-space text detections with
// spg::convert(), clips them to the source and drops any wholly outside it.
[[nodiscard]] std::vector<TileDetection> tile_to_source(std::span<const db::Candidate> candidates,
                                                        const SpaceMap& map, std::size_t tile);

// Links two boxes from different tiles when they intersect with positive area
// and overlap vertically by at least `line_overlap` of the shorter one (the
// same line seen by two tiles), and replaces every linked group by its union
// with the group's highest score. Boxes from the same tile are never linked
// directly; DBNet already separated them. Returns the boxes sorted top to
// bottom, then left to right.
[[nodiscard]] std::vector<Detection> merge_across_tiles(std::span<const TileDetection> detections,
                                                        float line_overlap);

class TextDetector {
 public:
  // Opens `model` with `options` (default providers, CPU fallback, one warmup
  // run). When options.warmup_shapes is empty it warms up with one tile
  // [1, 3, tile_size, tile_size]. Throws runtime::RuntimeError when the
  // session cannot be created and TextDetectorError when the model or config
  // is not usable.
  [[nodiscard]] static TextDetector open(runtime::Environment& env, const std::filesystem::path& model,
                                         const TextDetectorConfig& config = {},
                                         runtime::SessionOptions options = {});

  // Takes an open session. Throws TextDetectorError unless it has one float32
  // input [N, 3, H, W] and one float32 output [N, 1, H, W] whose dimensions
  // are dynamic or fit a tile_size x tile_size batch of one, and the config
  // is in range.
  explicit TextDetector(runtime::Session session, const TextDetectorConfig& config = {});

  // Detects text in `frame`, which holds Readback pixels: the whole source of
  // size `source` scaled to frame.size(). Pass source == frame.size() when the
  // frame is not scaled. Returns Source-space detections (kind TextRegion,
  // detector PpOcrDet) sorted top to bottom, then left to right; an empty
  // vector means no text was found. Safe to call from several threads at once.
  //
  // Throws on any failure so the caller can fail the frame closed:
  // ContractError for an invalid frame or zero-size source,
  // runtime::RuntimeError when inference fails, db::DecodeError when an output
  // has the wrong shape or holds a non-finite value.
  [[nodiscard]] std::vector<Detection> detect(const FrameView& frame, Size source) const;

  [[nodiscard]] const TextDetectorConfig& config() const noexcept { return config_; }
  [[nodiscard]] const runtime::Session& session() const noexcept { return session_; }

 private:
  runtime::Session session_;
  TextDetectorConfig config_;
};

}  // namespace spg::detect
