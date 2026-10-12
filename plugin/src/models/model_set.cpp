#include "models/model_set.hpp"

#include <array>
#include <cstdlib>
#include <exception>
#include <new>
#include <system_error>
#include <utility>

#include "detect/face_detector.hpp"
#include "detect/text_detector.hpp"
#include "models/sha256.hpp"
#include "ocr/text_recognizer.hpp"
#include "runtime/onnx_session.hpp"
#include "spg/policy.hpp"

namespace spg::plugin {

class LoadedModels {
 public:
  LoadedModels(std::unique_ptr<runtime::Environment> env, detect::FaceDetector face_detector,
               detect::TextDetector text_detector, ocr::TextRecognizer text_recognizer)
      : env_(std::move(env)),
        faces(std::move(face_detector)),
        text(std::move(text_detector)),
        recognizer(std::move(text_recognizer)) {}

 private:
  // Declared first, so destroyed after every session opened with it.
  std::unique_ptr<runtime::Environment> env_;

 public:
  const detect::FaceDetector faces;
  const detect::TextDetector text;
  const ocr::TextRecognizer recognizer;
};

namespace {

// Generated from models/manifest.json by plugin/CMakeLists.txt.
constexpr std::array<PinnedModel, 3> kPinned = {{
#include "models/pinned_models.inc"
}};

// The SPG-07 recognizer as a scan stage: one Secret per region, in order.
// Every region is read on every cycle; the per-track recognition cache needs
// track ids, which run_scan does not have (costs time, never recall).
class ModelRecognizer final : public TextRecognizer {
 public:
  explicit ModelRecognizer(std::shared_ptr<const ocr::TextRecognizer> impl) : impl_(std::move(impl)) {}
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
  std::shared_ptr<const ocr::TextRecognizer> impl_;
};

#ifdef _WIN32
std::filesystem::path env_path(const wchar_t* name) {
  wchar_t* value = nullptr;
  std::size_t length = 0;
  std::filesystem::path out;
  if (_wdupenv_s(&value, &length, name) == 0 && value != nullptr) {
    out = value;
  }
  std::free(value);
  return out;
}
#define SPG_ENV(name) env_path(L##name)
#else
std::filesystem::path env_path(const char* name) {
  const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read-only
  return value != nullptr ? std::filesystem::path{value} : std::filesystem::path{};
}
#define SPG_ENV(name) env_path(name)
#endif

ModelReport report(ModelStatus status, const char* model, std::string detail) {
  return {status, model, std::move(detail)};
}

// UTF-8 on every OS (path::string() would use the ANSI code page on Windows).
std::string display(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string joined(std::span<const std::filesystem::path> dirs) {
  std::string out;
  for (const std::filesystem::path& dir : dirs) {
    if (!dir.empty()) {
      out += out.empty() ? "" : ", ";
      out += display(dir);
    }
  }
  return out.empty() ? "no directory" : out;
}

}  // namespace

std::span<const PinnedModel> pinned_models() noexcept {
  return kPinned;
}

const char* status_name(ModelStatus status) noexcept {
  switch (status) {
    case ModelStatus::Idle:
      return "not started";
    case ModelStatus::Loading:
      return "loading";
    case ModelStatus::Ready:
      return "ready";
    case ModelStatus::Missing:
      return "missing";
    case ModelStatus::Corrupt:
      return "corrupt";
    case ModelStatus::Failed:
      return "failed";
  }
  return "unknown";
}

std::filesystem::path default_models_dir() {
  if (std::filesystem::path cache = SPG_ENV("SPG_CACHE_DIR"); !cache.empty()) {
    return cache / "models";
  }
#ifdef _WIN32
  std::filesystem::path home = SPG_ENV("USERPROFILE");
#else
  std::filesystem::path home = SPG_ENV("HOME");
#endif
  if (home.empty()) {
    return {};
  }
  return home / ".cache" / "spg" / "models";
}

ModelReport locate_models(std::span<const std::filesystem::path> dirs, ModelPaths& out) {
  std::array<std::filesystem::path*, 3> slots = {&out.faces, &out.text, &out.recognizer};
  for (std::size_t i = 0; i < kPinned.size(); ++i) {
    const PinnedModel& pin = kPinned[i];
    std::filesystem::path found;
    for (const std::filesystem::path& dir : dirs) {
      if (dir.empty()) {
        continue;
      }
      std::error_code ec;
      std::filesystem::path candidate = dir / std::filesystem::path(pin.relative);
      if (std::filesystem::exists(candidate, ec)) {
        found = std::move(candidate);
        break;
      }
    }
    if (found.empty()) {
      return report(ModelStatus::Missing, pin.name,
                    std::string(pin.relative) + " is not in " + joined(dirs) + " (run scripts/fetch_models.py)");
    }
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::is_regular_file(found, ec) ? std::filesystem::file_size(found, ec) : 0;
    if (ec || size != pin.size) {
      return report(ModelStatus::Corrupt, pin.name,
                    display(found) + " has " + std::to_string(size) + " bytes, expected " + std::to_string(pin.size));
    }
    const std::optional<std::string> hash = sha256_file(found);
    if (!hash) {
      return report(ModelStatus::Corrupt, pin.name, display(found) + " cannot be read");
    }
    if (*hash != pin.sha256) {
      return report(ModelStatus::Corrupt, pin.name, display(found) + " does not match its pinned SHA-256");
    }
    *slots[i] = std::move(found);
  }
  return report(ModelStatus::Ready, "", "");
}

LoadResult load_models(std::span<const std::filesystem::path> dirs, const LoadOptions& options) {
  ModelPaths paths;
  LoadResult out;
  out.report = locate_models(dirs, paths);
  if (out.report.status != ModelStatus::Ready) {
    return out;
  }
  runtime::SessionOptions session;
  if (options.cpu_only) {
    session.providers.clear();
  }
  const char* opening = kPinned[0].name;
  try {
    auto env = std::make_unique<runtime::Environment>(runtime::Environment::LogLevel::Error);
    detect::FaceDetector faces = detect::FaceDetector::open(*env, paths.faces, {}, session);
    opening = kPinned[1].name;
    detect::TextDetector text = detect::TextDetector::open(*env, paths.text, {}, session);
    opening = kPinned[2].name;
    ocr::TextRecognizer recognizer = ocr::TextRecognizer::open(*env, paths.recognizer, {}, session);
    std::string providers = std::string("faces=") + std::string(runtime::to_string(faces.session().provider())) +
                            ", text=" + std::string(runtime::to_string(text.session().provider())) +
                            ", recognizer=" + std::string(runtime::to_string(recognizer.session().provider()));
    out.models = std::make_shared<const LoadedModels>(std::move(env), std::move(faces), std::move(text),
                                                      std::move(recognizer));
    out.report = report(ModelStatus::Ready, "", std::move(providers));
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const runtime::RuntimeError& e) {
    // The file matched its pin, so a load failure is the runtime's, not the file's.
    const ModelStatus status = e.code() == runtime::ErrorCode::ModelNotFound ? ModelStatus::Missing
                               : e.code() == runtime::ErrorCode::ModelLoadFailed ? ModelStatus::Corrupt
                                                                                 : ModelStatus::Failed;
    out.report = report(status, opening, e.what());
  } catch (const std::exception& e) {
    out.report = report(ModelStatus::Failed, opening, e.what());
  } catch (...) {
    out.report = report(ModelStatus::Failed, opening, "unknown error");
  }
  return out;
}

ScanStages make_stages(const std::shared_ptr<const LoadedModels>& models, const PolicyConfig& policy) {
  ScanStages stages;
  if (models != nullptr) {
    // Aliasing pointers: each stage keeps the whole model set (and its
    // runtime environment) alive while a worker may still call it.
    stages.faces = std::make_shared<DetectorAdapter<detect::FaceDetector>>(
        std::shared_ptr<const detect::FaceDetector>(models, &models->faces));
    stages.text = std::make_shared<DetectorAdapter<detect::TextDetector>>(
        std::shared_ptr<const detect::TextDetector>(models, &models->text));
    stages.recognizer =
        std::make_shared<ModelRecognizer>(std::shared_ptr<const ocr::TextRecognizer>(models, &models->recognizer));
  }
  // Errors name a watch-list entry's index, never its text.
  stages.policy = std::make_shared<EnginePolicy>(std::make_shared<const PolicyEngine>(policy));
  return stages;
}

}  // namespace spg::plugin
