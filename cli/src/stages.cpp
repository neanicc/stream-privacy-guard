#include "stages.hpp"

#include <chrono>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

#include "spg/detection.hpp"
#include "spg/frame.hpp"
#include "spg/secret.hpp"
#include "spg/time.hpp"

namespace spg::cli {

namespace {

using steady = std::chrono::steady_clock;

// Records one call's duration and, for the cycle log, when it ended.
class CallTimer {
 public:
  CallTimer(std::string_view stage, StageTimes& times, CycleLog* cycles, Timestamp readback)
      : stage_(stage), times_(times), cycles_(cycles), readback_(readback), start_(steady::now()) {}
  CallTimer(const CallTimer&) = delete;
  CallTimer& operator=(const CallTimer&) = delete;
  ~CallTimer() {
    times_.add(stage_, elapsed_ms(start_, steady::now()));
    if (cycles_ != nullptr) {
      cycles_->note(readback_, SteadyClock{}.now());
    }
  }

 private:
  std::string_view stage_;
  StageTimes& times_;
  CycleLog* cycles_;
  Timestamp readback_;
  steady::time_point start_;
};

class TimedDetector final : public Detector {
 public:
  TimedDetector(std::shared_ptr<const Detector> inner, std::string_view stage, std::shared_ptr<StageTimes> times,
                std::shared_ptr<CycleLog> cycles)
      : inner_(std::move(inner)), stage_(stage), times_(std::move(times)), cycles_(std::move(cycles)) {}
  [[nodiscard]] std::vector<Detection> detect(const FrameView& readback, Size source) const override {
    const CallTimer timer(stage_, *times_, cycles_.get(), readback.timestamp);
    return inner_->detect(readback, source);
  }

 private:
  std::shared_ptr<const Detector> inner_;
  std::string_view stage_;
  std::shared_ptr<StageTimes> times_;
  std::shared_ptr<CycleLog> cycles_;
};

class TimedRecognizer final : public TextRecognizer {
 public:
  TimedRecognizer(std::shared_ptr<const TextRecognizer> inner, std::shared_ptr<StageTimes> times,
                  std::shared_ptr<CycleLog> cycles)
      : inner_(std::move(inner)), times_(std::move(times)), cycles_(std::move(cycles)) {}
  [[nodiscard]] std::vector<Secret> recognize(const FrameView& readback, Size source,
                                              std::span<const Detection> regions) const override {
    const CallTimer timer(kStageRecognize, *times_, cycles_.get(), readback.timestamp);
    return inner_->recognize(readback, source, regions);
  }

 private:
  std::shared_ptr<const TextRecognizer> inner_;
  std::shared_ptr<StageTimes> times_;
  std::shared_ptr<CycleLog> cycles_;
};

// The policy sees text, not the readback, so it times calls only; the
// recognizer call before it ends the cycle closely enough (the policy takes
// microseconds per line).
class TimedPolicy final : public TextPolicy {
 public:
  TimedPolicy(std::shared_ptr<const TextPolicy> inner, std::shared_ptr<StageTimes> times)
      : inner_(std::move(inner)), times_(std::move(times)) {}
  [[nodiscard]] Verdict evaluate(const Secret& text) const override {
    const auto start = steady::now();
    Verdict v = inner_->evaluate(text);
    times_->add(kStagePolicy, elapsed_ms(start, steady::now()));
    return v;
  }

 private:
  std::shared_ptr<const TextPolicy> inner_;
  std::shared_ptr<StageTimes> times_;
};

}  // namespace

std::filesystem::path default_cache_dir() {
  // std::getenv is fine here: the CLI reads these once on its main thread.
  if (const char* dir = std::getenv("SPG_CACHE_DIR"); dir != nullptr && *dir != '\0') {
    return std::filesystem::path(dir);
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / ".cache" / "spg";
  }
  if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
    return std::filesystem::path(profile) / ".cache" / "spg";
  }
  return std::filesystem::path(".cache") / "spg";
}

ModelPaths model_paths(const std::filesystem::path& cache) {
  // SPG_MODEL_* are models/manifest.json's <name>/<version>/<file>, set by
  // cli/CMakeLists.txt at configure time.
  const std::filesystem::path models = cache / "models";
  return ModelPaths{models / SPG_MODEL_YUNET, models / SPG_MODEL_PPOCR_DET, models / SPG_MODEL_PPOCR_REC};
}

ScanStages timed(const ScanStages& stages, std::shared_ptr<StageTimes> times, std::shared_ptr<CycleLog> cycles) {
  ScanStages out;
  if (stages.faces) {
    out.faces = std::make_shared<TimedDetector>(stages.faces, kStageFaces, times, cycles);
  }
  if (stages.text) {
    out.text = std::make_shared<TimedDetector>(stages.text, kStageText, times, cycles);
  }
  if (stages.recognizer) {
    out.recognizer = std::make_shared<TimedRecognizer>(stages.recognizer, times, cycles);
  }
  if (stages.policy) {
    out.policy = std::make_shared<TimedPolicy>(stages.policy, times);
  }
  return out;
}

}  // namespace spg::cli
