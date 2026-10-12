#include "spg/scan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
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
  const FrameView& frame = job.readback->view();
  ScanResult result;
  try {
    if (want_faces) {
      std::vector<Detection> faces;
      try {
        faces = stages.faces->detect(frame, job.source);
      } catch (...) {
        return failed(std::move(result), ScanFailure::FaceDetector);
      }
      if (!valid_output(faces, DetectionKind::Face)) {
        keep_valid(result.detections, faces, DetectionKind::Face);
        return failed(std::move(result), ScanFailure::InvalidOutput);
      }
      result.detections.insert(result.detections.end(), faces.begin(), faces.end());
    }

    if (want_text) {
      std::vector<Detection> regions;
      try {
        regions = stages.text->detect(frame, job.source);
      } catch (...) {
        return failed(std::move(result), ScanFailure::TextDetector);
      }
      if (!valid_output(regions, DetectionKind::TextRegion)) {
        keep_valid(result.detections, regions, DetectionKind::TextRegion);
        return failed(std::move(result), ScanFailure::InvalidOutput);
      }
      if (stages.recognizer == nullptr || stages.policy == nullptr) {
        // Nothing can clear these regions: mask them all (recall over precision).
        result.detections.insert(result.detections.end(), regions.begin(), regions.end());
      } else if (!regions.empty()) {
        // From here on a failure masks every region: none was cleared by a
        // text stage that finished.
        const auto mask_all = [&](ScanFailure failure) {
          result.detections.insert(result.detections.end(), regions.begin(), regions.end());
          return failed(std::move(result), failure);
        };
        std::vector<Secret> texts;
        try {
          texts = stages.recognizer->recognize(frame, job.source, regions);
        } catch (...) {
          return mask_all(ScanFailure::Recognizer);
        }
        if (texts.size() != regions.size()) {
          return mask_all(ScanFailure::InvalidOutput);
        }
        std::vector<bool> masked(regions.size(), false);
        for (std::size_t i = 0; i < regions.size(); ++i) {
          masked[i] = texts[i].size() > limits.max_text_bytes;
          if (!masked[i]) {
            try {
              masked[i] = stages.policy->evaluate(texts[i]).sensitive();
            } catch (...) {
              return mask_all(ScanFailure::Policy);
            }
          }
        }
        for (std::size_t i = 0; i < regions.size(); ++i) {
          if (masked[i]) {
            result.detections.push_back(regions[i]);
          }
        }
      }
    }
  } catch (...) {
    // Allocation failure while collecting the boxes a stage returned: some
    // are missing from `detections`, so the scheduler must fail closed
    // their whole window (retro and hold) rather than hold only what is here.
    result.detections_lost = true;
    return failed(std::move(result), ScanFailure::InvalidOutput);
  }
  result.complete = true;
  return result;
}

}  // namespace spg
