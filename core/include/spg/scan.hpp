#pragma once

// One detection cycle over one readback frame (SPG-10): the stages a worker
// runs and the rule that turns their output into Source-space detections to
// mask. Rules and rationale: docs/scheduler.md.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "spg/category.hpp"
#include "spg/detection.hpp"
#include "spg/error.hpp"
#include "spg/frame.hpp"
#include "spg/geometry.hpp"
#include "spg/policy.hpp"
#include "spg/secret.hpp"
#include "spg/time.hpp"

namespace spg {

// A detector stage: Source-space detections for one readback frame, which
// holds the whole source of size `source` scaled to frame.size(). Throwing
// anything fails the cycle, so the frames it would have covered fail closed.
// Implementations must be safe to call from several worker threads at once.
class Detector {
 public:
  Detector() = default;
  Detector(const Detector&) = delete;
  Detector& operator=(const Detector&) = delete;
  virtual ~Detector() = default;
  [[nodiscard]] virtual std::vector<Detection> detect(const FrameView& readback, Size source) const = 0;
};

// Adapts any type with `std::vector<Detection> detect(const FrameView&, Size)
// const` (spg::detect::FaceDetector, spg::detect::TextDetector) without making
// spg-core depend on the detectors. Throws ContractError for a null `impl`.
template <class Impl>
class DetectorAdapter final : public Detector {
 public:
  explicit DetectorAdapter(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {
    if (impl_ == nullptr) {
      throw ContractError("scan: DetectorAdapter needs a detector");
    }
  }
  [[nodiscard]] std::vector<Detection> detect(const FrameView& readback, Size source) const override {
    return impl_->detect(readback, source);
  }

 private:
  std::shared_ptr<const Impl> impl_;
};

// Reads the text in each region: one Secret per region, in order. Throwing
// anything, or returning a different count, fails the cycle.
class TextRecognizer {
 public:
  TextRecognizer() = default;
  TextRecognizer(const TextRecognizer&) = delete;
  TextRecognizer& operator=(const TextRecognizer&) = delete;
  virtual ~TextRecognizer() = default;
  [[nodiscard]] virtual std::vector<Secret> recognize(const FrameView& readback, Size source,
                                                      std::span<const Detection> regions) const = 0;
};

// Decides whether recognized text is sensitive. Throwing fails the cycle.
class TextPolicy {
 public:
  TextPolicy() = default;
  TextPolicy(const TextPolicy&) = delete;
  TextPolicy& operator=(const TextPolicy&) = delete;
  virtual ~TextPolicy() = default;
  [[nodiscard]] virtual Verdict evaluate(const Secret& text) const = 0;
};

// The SPG-08 PolicyEngine as a TextPolicy. Throws ContractError for a null
// `engine`.
class EnginePolicy final : public TextPolicy {
 public:
  explicit EnginePolicy(std::shared_ptr<const PolicyEngine> engine) : engine_(std::move(engine)) {
    if (engine_ == nullptr) {
      throw ContractError("scan: EnginePolicy needs a policy engine");
    }
  }
  [[nodiscard]] Verdict evaluate(const Secret& text) const override { return engine_->evaluate(text); }

 private:
  std::shared_ptr<const PolicyEngine> engine_;
};

// What a worker runs:
//   faces       run when `categories` contains Face;
//   text        run when `categories` contains any text category;
//   recognizer  reads each text region; without one every region is masked;
//   policy      judges recognized text; without one every region is masked.
// A detector may be null only when its categories are disabled: a null
// detector for an enabled category cannot scan for it, so every cycle fails
// (ScanFailure::MissingStage) and frames get the fallback.
// Shared ownership keeps stages alive for a worker still inside a stage call
// when its scheduler shuts down.
struct ScanStages {
  std::shared_ptr<const Detector> faces;
  std::shared_ptr<const Detector> text;
  std::shared_ptr<const TextRecognizer> recognizer;
  std::shared_ptr<const TextPolicy> policy;
};

struct ScanLimits {
  CategorySet categories = CategorySet::all();
  // Recognized text longer than this many bytes is not given to the policy
  // (whose cost grows with text length and has no cap of its own); its
  // region is masked instead.
  std::size_t max_text_bytes = 512;
};

// An owned, tightly packed copy of a frame, so a worker can scan it after the
// render callback that produced it has returned.
class OwnedFrame {
 public:
  // Throws ContractError when validate(frame) fails.
  [[nodiscard]] static std::shared_ptr<const OwnedFrame> copy(const FrameView& frame);
  // Wraps pixels the caller already owns, without copying: `owner` keeps
  // every plane of `frame` alive and unchanged until the last reference to
  // the result is gone (on whatever thread drops it). For a pooled readback
  // buffer, `owner`'s deleter returns the buffer to the pool. Throws
  // ContractError when validate(frame) fails or `owner` is null.
  [[nodiscard]] static std::shared_ptr<const OwnedFrame> adopt(const FrameView& frame,
                                                               std::shared_ptr<const void> owner);
  // Valid for the lifetime of this object.
  [[nodiscard]] const FrameView& view() const noexcept { return view_; }

 private:
  OwnedFrame() = default;
  std::vector<std::uint8_t> bytes_;
  std::shared_ptr<const void> owner_;
  FrameView view_;
};

using FrameId = std::uint64_t;

// A queued detection cycle. `source` is the source size when the frame was
// pushed; detections come back in that size's Source pixels.
struct ScanJob {
  std::uint64_t seq = 0;
  FrameId frame = 0;
  Timestamp frame_time{};
  Size source;
  std::shared_ptr<const OwnedFrame> readback;
  std::uint64_t size_epoch = 0;  // which source size (changes on resize)
  std::uint64_t segment = 0;     // which content (changes on resize or discontinuity)
};

enum class ScanFailure : std::uint8_t {
  None,
  FaceDetector,
  TextDetector,
  Recognizer,
  Policy,
  InvalidOutput,
  MissingStage,  // an enabled category has no detector
};

struct ScanResult {
  // True only when every enabled category had its detector, and every stage
  // that ran finished and returned valid output. An incomplete cycle covers
  // no frame.
  bool complete = false;
  ScanFailure failure = ScanFailure::None;
  // Source-space boxes to mask, in the job's source size: faces, and text
  // regions that are sensitive, unread, over the text cap or unjudged.
  // An incomplete result (a partial cycle) keeps the valid boxes found before
  // the failure: faces from a face stage that finished, every text region
  // once the text detector returned (a failed recognizer or policy cleared
  // none), and the valid boxes of a stage whose output was malformed. The
  // scheduler holds them without counting the cycle as a scan.
  std::vector<Detection> detections;
  // True when a stage returned boxes that `detections` could not all keep
  // (an allocation failure while collecting them). Nothing can hold a box it
  // never received, so the scheduler fails closed every frame they could
  // have masked, as for a result it cannot apply. Implies !complete.
  bool detections_lost = false;
};

// Runs one cycle, stopping at the first failure. Never throws: any exception
// from a stage (InferenceFailed, DecodeError, ContractError, bad_alloc, or a
// non-std exception) or invalid output yields an incomplete result with the
// detections found before it; an allocation failure while collecting them
// sets detections_lost. Never logs, and keeps no text.
[[nodiscard]] ScanResult run_scan(const ScanStages& stages, const ScanJob& job,
                                  const ScanLimits& limits) noexcept;

}  // namespace spg
