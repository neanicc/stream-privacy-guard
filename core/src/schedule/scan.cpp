#include "spg/scan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "spg/error.hpp"

namespace spg {

namespace {

constexpr Category kTextCategories[] = {Category::Watchlist, Category::PiiEmail,  Category::PiiPhone,
                                        Category::PiiCard,   Category::PiiIp,     Category::PiiAddress,
                                        Category::PiiToken};

bool any_text_category(CategorySet set) noexcept {
  return std::any_of(std::begin(kTextCategories), std::end(kTextCategories),
                     [set](Category c) { return set.contains(c); });
}

// Bytes in one row and rows in `plane` of a packed copy.
std::size_t row_bytes(PixelFormat format, std::size_t plane, std::uint32_t width) noexcept {
  const std::size_t w = width;
  const std::size_t half = (w + 1) / 2;
  switch (format) {
    case PixelFormat::BGRA:
      return 4 * w;
    case PixelFormat::NV12:
      return plane == 0 ? w : 2 * half;
    case PixelFormat::I420:
      return plane == 0 ? w : half;
  }
  return 0;
}
std::size_t rows(PixelFormat format, std::size_t plane, std::uint32_t height) noexcept {
  const std::size_t h = height;
  return (plane == 0 || format == PixelFormat::BGRA) ? h : (h + 1) / 2;
}

// Validates detector output; false fails the cycle.
bool valid_one(const Detection& d, DetectionKind kind) noexcept {
  try {
    validate(d);
    return d.kind == kind;
  } catch (...) {
    return false;
  }
}
bool valid_output(const std::vector<Detection>& detections, DetectionKind kind) noexcept {
  return std::all_of(detections.begin(), detections.end(),
                     [kind](const Detection& d) { return valid_one(d, kind); });
}

// Appends the detections of `found` that are valid on their own. Called for
// output that failed valid_output(): a malformed box is never held, but its
// valid siblings are still boxes to mask (recall over precision).
void keep_valid(std::vector<Detection>& out, const std::vector<Detection>& found, DetectionKind kind) {
  for (const Detection& d : found) {
    if (valid_one(d, kind)) {
      out.push_back(d);
    }
  }
}

// An incomplete cycle. `result.detections` keeps what the stages that ran
// found before the failure: the scheduler holds them without counting the
// cycle as a scan (docs/scheduler.md, "Partial cycles").
// Callers move `result` in, so returning it from run_scan() cannot throw.
ScanResult failed(ScanResult result, ScanFailure failure) noexcept {
  result.complete = false;
  result.failure = failure;
  return result;
}

ScanResult failed(ScanFailure failure) noexcept {
  ScanResult result;
  result.failure = failure;
  return result;
}

// What one lane of a cycle (faces, or text with its recognizer and policy)
// kept and how it ended. A lane stops at its own first failure.
struct Lane {
  std::vector<Detection> kept;
  ScanFailure failure = ScanFailure::None;
  bool lost = false;  // an allocation failed while collecting boxes
};

// The face lane. Never throws.
Lane face_lane(const Detector& detector, const ScanJob& job) noexcept {
  Lane lane;
  try {
    std::vector<Detection> faces;
    try {
      faces = detector.detect(job.readback->view(), job.source);
    } catch (...) {
      lane.failure = ScanFailure::FaceDetector;
      return lane;
    }
    if (!valid_output(faces, DetectionKind::Face)) {
      lane.failure = ScanFailure::InvalidOutput;
      keep_valid(lane.kept, faces, DetectionKind::Face);
      return lane;
    }
    lane.kept = std::move(faces);
  } catch (...) {
    lane.lost = true;
    lane.failure = ScanFailure::InvalidOutput;
  }
  return lane;
}

// The text lane, in two steps (detectors, then reading): detect() runs the
// text detector, read() the recognizer and the policy on the regions it
// found. Neither throws.
class TextLane {
 public:
  TextLane(const ScanStages& stages, const ScanJob& job, const ScanLimits& limits) noexcept
      : stages_(stages), job_(job), limits_(limits) {}

  void detect() noexcept {
    try {
      std::vector<Detection> found;
      try {
        found = stages_.text->detect(job_.readback->view(), job_.source);
      } catch (...) {
        lane_.failure = ScanFailure::TextDetector;
        return;
      }
      if (!valid_output(found, DetectionKind::TextRegion)) {
        lane_.failure = ScanFailure::InvalidOutput;
        keep_valid(lane_.kept, found, DetectionKind::TextRegion);
        return;
      }
      regions_ = std::move(found);
      detected_ = true;
    } catch (...) {
      lane_.lost = true;
      lane_.failure = ScanFailure::InvalidOutput;
    }
  }

  void read() noexcept {
    if (!detected_) {
      return;
    }
    try {
      if (stages_.recognizer == nullptr || stages_.policy == nullptr) {
        // Nothing can clear these regions: mask them all (recall over precision).
        lane_.kept = regions_;
        return;
      }
      if (regions_.empty()) {
        return;
      }
      // From here on a failure masks every region: none was cleared by a
      // text stage that finished.
      const FrameView& frame = job_.readback->view();
      std::vector<Secret> texts;
      try {
        texts = stages_.recognizer->recognize(frame, job_.source, regions_);
      } catch (...) {
        mask_all(ScanFailure::Recognizer);
        return;
      }
      if (texts.size() != regions_.size()) {
        mask_all(ScanFailure::InvalidOutput);
        return;
      }
      std::vector<bool> masked(regions_.size(), false);
      for (std::size_t i = 0; i < regions_.size(); ++i) {
        masked[i] = texts[i].size() > limits_.max_text_bytes;
        if (!masked[i]) {
          try {
            masked[i] = stages_.policy->evaluate(texts[i]).sensitive();
          } catch (...) {
            mask_all(ScanFailure::Policy);
            return;
          }
        }
      }
      for (std::size_t i = 0; i < regions_.size(); ++i) {
        if (masked[i]) {
          lane_.kept.push_back(regions_[i]);
        }
      }
    } catch (...) {
      // Some of the regions may be missing from `kept`.
      lane_.lost = true;
      lane_.failure = ScanFailure::InvalidOutput;
    }
  }

  [[nodiscard]] Lane& lane() noexcept { return lane_; }

 private:
  void mask_all(ScanFailure failure) {
    lane_.failure = failure;
    lane_.kept = regions_;  // may throw; read() records the loss
  }

  const ScanStages& stages_;
  const ScanJob& job_;
  const ScanLimits& limits_;
  std::vector<Detection> regions_;
  bool detected_ = false;
  Lane lane_;
};

// Runs the face lane on a thread of its own, joined by finish() or on
// destruction. When no thread was started, finish() runs the lane on the
// calling thread instead.
class FaceHelper {
 public:
  FaceHelper(const Detector& detector, const ScanJob& job) noexcept : detector_(detector), job_(job) {}
  FaceHelper(const FaceHelper&) = delete;
  FaceHelper& operator=(const FaceHelper&) = delete;
  ~FaceHelper() { join(); }

  // Starts the lane beside the caller; false when no thread could start.
  bool start() noexcept {
    try {
      thread_ = std::thread([this] { lane_ = face_lane(detector_, job_); });
      return true;
    } catch (...) {
      return false;
    }
  }
  // The lane's result: joins a started thread, or runs the lane here.
  Lane& finish() noexcept {
    if (thread_.joinable()) {
      join();
    } else {
      lane_ = face_lane(detector_, job_);
    }
    return lane_;
  }

 private:
  void join() noexcept {
    if (thread_.joinable()) {
      try {
        thread_.join();
      } catch (...) {
        // join() throws only for a thread that is not joinable or is this one.
      }
    }
  }

  const Detector& detector_;
  const ScanJob& job_;
  Lane lane_;
  std::thread thread_;
};

}  // namespace

std::shared_ptr<const OwnedFrame> OwnedFrame::copy(const FrameView& frame) {
  validate(frame);
  const std::size_t planes = plane_count(frame.format);
  std::size_t total = 0;
  for (std::size_t p = 0; p < planes; ++p) {
    total += row_bytes(frame.format, p, frame.width) * rows(frame.format, p, frame.height);
  }
  std::shared_ptr<OwnedFrame> owned(new OwnedFrame());
  owned->bytes_.resize(total);
  owned->view_ = frame;
  std::size_t offset = 0;
  for (std::size_t p = 0; p < planes; ++p) {
    const std::size_t bytes = row_bytes(frame.format, p, frame.width);
    const std::size_t count = rows(frame.format, p, frame.height);
    std::uint8_t* dst = owned->bytes_.data() + offset;
    for (std::size_t r = 0; r < count; ++r) {
      std::memcpy(dst + r * bytes, frame.planes[p] + r * frame.strides[p], bytes);
    }
    owned->view_.planes[p] = dst;
    owned->view_.strides[p] = static_cast<std::uint32_t>(bytes);
    offset += bytes * count;
  }
  return owned;
}

std::shared_ptr<const OwnedFrame> OwnedFrame::adopt(const FrameView& frame, std::shared_ptr<const void> owner) {
  validate(frame);
  if (owner == nullptr) {
    throw ContractError("scan: OwnedFrame::adopt needs an owner");
  }
  std::shared_ptr<OwnedFrame> owned(new OwnedFrame());
  owned->owner_ = std::move(owner);
  owned->view_ = frame;
  return owned;
}

ScanResult run_scan(const ScanStages& stages, const ScanJob& job, const ScanLimits& limits) noexcept {
  if (job.readback == nullptr) {
    return failed(ScanFailure::InvalidOutput);
  }
  const bool want_faces = limits.categories.contains(Category::Face);
  const bool want_text = any_text_category(limits.categories);
  // A category that is enabled but cannot be scanned is not a clean scan.
  if ((want_faces && stages.faces == nullptr) || (want_text && stages.text == nullptr)) {
    return failed(ScanFailure::MissingStage);
  }
  // Two lanes, each stopping at its own first failure: faces, and text
  // (detector, recognizer, policy). With both enabled the face detector runs
  // on a helper thread beside the text detector, so the cycle waits for the
  // slower of the two rather than their sum (SPG-16e).
  Lane faces;
  std::optional<TextLane> text;
  {
    std::optional<FaceHelper> helper;
    if (want_faces) {
      helper.emplace(*stages.faces, job);
      if (want_text) {
        (void)helper->start();  // when it cannot start, finish() runs it here
      }
    }
    if (want_text) {
      text.emplace(stages, job, limits);
      text->detect();
    }
    if (helper) {
      faces = std::move(helper->finish());
    }
  }
  // Reading only matters when the face lane finished (see below).
  if (text && faces.failure == ScanFailure::None) {
    text->read();
  }

  // The result is what running the stages one after another returns: a
  // failed face lane ends the cycle with its own boxes, as if text never ran
  // (docs/scheduler.md, "Partial cycles"); otherwise the faces and what the
  // text lane kept, named by the text lane's failure.
  ScanResult result;
  Lane none;
  Lane& texts = text && faces.failure == ScanFailure::None ? text->lane() : none;
  const ScanFailure failure = faces.failure != ScanFailure::None ? faces.failure : texts.failure;
  try {
    result.detections = std::move(faces.kept);
    result.detections.insert(result.detections.end(), texts.kept.begin(), texts.kept.end());
  } catch (...) {
    // Allocation failure while collecting the boxes the lanes returned: some
    // are missing from `detections`, so the scheduler must fail closed
    // their whole window (retro and hold) rather than hold only what is here.
    result.detections_lost = true;
    return failed(std::move(result), ScanFailure::InvalidOutput);
  }
  if (faces.lost || texts.lost) {
    result.detections_lost = true;
    return failed(std::move(result), ScanFailure::InvalidOutput);
  }
  if (failure != ScanFailure::None) {
    return failed(std::move(result), failure);
  }
  result.complete = true;
  return result;
}

}  // namespace spg
