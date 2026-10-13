// open_stages() for builds with ONNX Runtime and OpenCV (SPG_WITH_DEPS=ON).
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "detect/face_detector.hpp"
#include "detect/text_detector.hpp"
#include "image_io.hpp"
#include "ocr/text_recognizer.hpp"
#include "runtime/onnx_session.hpp"
#include "spg/category.hpp"
#include "spg/policy.hpp"
#include "stages.hpp"

namespace spg::cli {

namespace {

// The SPG-07 recognizer as a scan stage: one Secret per region, in order.
// The per-track recognition cache needs track ids, which run_scan() does not
// have; every region is read on every cycle (costs time, never recall).
class OcrRecognizer final : public TextRecognizer {
 public:
  OcrRecognizer(std::shared_ptr<runtime::Environment> env, std::shared_ptr<const ocr::TextRecognizer> impl)
      : env_(std::move(env)), impl_(std::move(impl)) {}
  [[nodiscard]] std::vector<Secret> recognize(const FrameView& readback, Size source,
                                              std::span<const Detection> regions) const override {
    std::vector<ocr::Recognition> read = impl_->recognize(readback, source, regions);
    std::vector<Secret> out;
    out.reserve(read.size());
    for (ocr::Recognition& r : read) {
      out.push_back(std::move(r.text));
    }
    return out;
  }

 private:
  std::shared_ptr<runtime::Environment> env_;  // outlives the session
  std::shared_ptr<const ocr::TextRecognizer> impl_;
};

// Keeps the ONNX Runtime environment alive as long as a detector using it.
template <class Impl>
class OwningDetector final : public Detector {
 public:
  OwningDetector(std::shared_ptr<runtime::Environment> env, std::shared_ptr<const Impl> impl)
      : env_(std::move(env)), adapter_(std::move(impl)) {}
  [[nodiscard]] std::vector<Detection> detect(const FrameView& readback, Size source) const override {
    return adapter_.detect(readback, source);
  }

 private:
  std::shared_ptr<runtime::Environment> env_;
  DetectorAdapter<Impl> adapter_;
};

bool any_text(CategorySet categories) {
  for (const Category c : {Category::Watchlist, Category::PiiEmail, Category::PiiPhone, Category::PiiCard,
                           Category::PiiIp, Category::PiiAddress, Category::PiiToken}) {
    if (categories.contains(c)) {
      return true;
    }
  }
  return false;
}

std::string provider_name(const runtime::Session& session) {
  return std::string(runtime::to_string(session.provider()));
}

}  // namespace

OpenedStages open_stages(const ModelPaths& models, const PolicyConfig& policy, const StageOptions& options) {
  runtime::SessionOptions session;
  if (options.cpu_only) {
    session.providers.clear();
  }
  OpenedStages out;
  try {
    auto env = std::make_shared<runtime::Environment>(runtime::Environment::LogLevel::Error);
    if (policy.categories.contains(Category::Face)) {
      auto faces = std::make_shared<const detect::FaceDetector>(
          detect::FaceDetector::open(*env, models.faces, {}, session));
      out.providers += "faces=" + provider_name(faces->session()) + " ";
      out.stages.faces = std::make_shared<OwningDetector<detect::FaceDetector>>(env, faces);
    }
    if (any_text(policy.categories)) {
      detect::TextDetectorConfig text_config;
      if (options.thorough) {
        text_config.tile_overlap = text_config.tile_size / 2;
        text_config.db.box_threshold = text_config.db.bin_threshold;
      }
      auto text = std::make_shared<const detect::TextDetector>(
          detect::TextDetector::open(*env, models.text_detector, text_config, session));
      out.providers += "text_detect=" + provider_name(text->session()) + " ";
      out.stages.text = std::make_shared<OwningDetector<detect::TextDetector>>(env, text);
      auto rec = std::make_shared<const ocr::TextRecognizer>(
          ocr::TextRecognizer::open(*env, models.text_recognizer, {}, session));
      out.providers += "recognize=" + provider_name(rec->session());
      out.stages.recognizer = std::make_shared<OcrRecognizer>(env, rec);
    }
    // Errors name the entry's index, never its text.
    out.stages.policy = std::make_shared<EnginePolicy>(std::make_shared<const PolicyEngine>(policy));
  } catch (const CliError&) {
    throw;
  } catch (const std::exception& e) {
    throw CliError(std::string("cannot open the pipeline: ") + e.what());
  }
  return out;
}

}  // namespace spg::cli
