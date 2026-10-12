#include "spg/scan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
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
bool valid_output(const std::vector<Detection>& detections, DetectionKind kind) noexcept {
  try {
    for (const Detection& d : detections) {
      validate(d);
      if (d.kind != kind) {
        return false;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

ScanResult failed(ScanFailure failure) {
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

ScanResult run_scan(const ScanStages& stages, const ScanJob& job, const ScanLimits& limits) noexcept {
  if (job.readback == nullptr) {
    return failed(ScanFailure::InvalidOutput);
  }
  const FrameView& frame = job.readback->view();
  ScanResult result;
  try {
    if (stages.faces != nullptr && limits.categories.contains(Category::Face)) {
      std::vector<Detection> faces;
      try {
        faces = stages.faces->detect(frame, job.source);
      } catch (...) {
        return failed(ScanFailure::FaceDetector);
      }
      if (!valid_output(faces, DetectionKind::Face)) {
        return failed(ScanFailure::InvalidOutput);
      }
      result.detections.insert(result.detections.end(), faces.begin(), faces.end());
    }

    if (stages.text != nullptr && any_text_category(limits.categories)) {
      std::vector<Detection> regions;
      try {
        regions = stages.text->detect(frame, job.source);
      } catch (...) {
        return failed(ScanFailure::TextDetector);
      }
      if (!valid_output(regions, DetectionKind::TextRegion)) {
        return failed(ScanFailure::InvalidOutput);
      }
      if (stages.recognizer == nullptr || stages.policy == nullptr) {
        // Nothing can clear these regions: mask them all (recall over precision).
        result.detections.insert(result.detections.end(), regions.begin(), regions.end());
      } else if (!regions.empty()) {
        std::vector<Secret> texts;
        try {
          texts = stages.recognizer->recognize(frame, job.source, regions);
        } catch (...) {
          return failed(ScanFailure::Recognizer);
        }
        if (texts.size() != regions.size()) {
          return failed(ScanFailure::InvalidOutput);
        }
        for (std::size_t i = 0; i < regions.size(); ++i) {
          bool mask = texts[i].size() > limits.max_text_bytes;
          if (!mask) {
            try {
              mask = stages.policy->evaluate(texts[i]).sensitive();
            } catch (...) {
              return failed(ScanFailure::Policy);
            }
          }
          if (mask) {
            result.detections.push_back(regions[i]);
          }
        }
      }
    }
  } catch (...) {
    // Allocation failure while collecting results.
    return failed(ScanFailure::InvalidOutput);
  }
  result.complete = true;
  return result;
}

}  // namespace spg
