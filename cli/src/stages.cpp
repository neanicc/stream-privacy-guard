#include "stages.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "spg/detection.hpp"
#include "spg/frame.hpp"
#include "spg/secret.hpp"
#include "spg/time.hpp"

namespace spg::cli {

namespace {

using steady = std::chrono::steady_clock;

// The readback timestamp of the cycle this thread is running. run_scan()
// calls a cycle's stages one after another on one worker thread, and every
// stage before the policy sees the readback, so the policy (which sees only
// text) uses this to end the same cycle in the log.
thread_local Timestamp t_cycle_readback{};
thread_local bool t_in_cycle = false;

// Runs call() and records its duration and, for the cycle log, when it
// ended: on the way out, so a call that throws (which fails the cycle) still
// counts its time and ends its cycle. `readback` names the cycle; unset means
// the cycle this thread is in, if any. Recording can throw (it allocates); it
// does so from a plain function, never a destructor, so a failed record is
// one more exception for run_scan() to fail the cycle on, never a terminate.
template <class Call>
auto timed_call(std::string_view stage, StageTimes& times, CycleLog* cycles, std::optional<Timestamp> readback,
                const Call& call) {
  const auto start = steady::now();
  const auto record = [&] {
    times.add(stage, elapsed_ms(start, steady::now()));
    if (cycles != nullptr && (readback || t_in_cycle)) {
      cycles->note(readback.value_or(t_cycle_readback), SteadyClock{}.now());
    }
  };
  auto result = [&] {
    try {
      return call();
    } catch (...) {
      record();
      throw;
    }
  }();
  record();
  return result;
}

class TimedDetector final : public Detector {
 public:
  TimedDetector(std::shared_ptr<const Detector> inner, std::string_view stage, std::shared_ptr<StageTimes> times,
                std::shared_ptr<CycleLog> cycles)
      : inner_(std::move(inner)), stage_(stage), times_(std::move(times)), cycles_(std::move(cycles)) {}
  [[nodiscard]] std::vector<Detection> detect(const FrameView& readback, Size source) const override {
    t_cycle_readback = readback.timestamp;
    t_in_cycle = true;
    return timed_call(stage_, *times_, cycles_.get(), readback.timestamp,
                      [&] { return inner_->detect(readback, source); });
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
    t_cycle_readback = readback.timestamp;
    t_in_cycle = true;
    return timed_call(kStageRecognize, *times_, cycles_.get(), readback.timestamp,
                      [&] { return inner_->recognize(readback, source, regions); });
  }

 private:
  std::shared_ptr<const TextRecognizer> inner_;
  std::shared_ptr<StageTimes> times_;
  std::shared_ptr<CycleLog> cycles_;
};

// The policy sees text, not the readback; it ends the cycle whose earlier
// stages ran on this thread (t_cycle_readback), so a large watch-list's
// policy time counts in the cycle's detection latency.
class TimedPolicy final : public TextPolicy {
 public:
  TimedPolicy(std::shared_ptr<const TextPolicy> inner, std::shared_ptr<StageTimes> times,
              std::shared_ptr<CycleLog> cycles)
      : inner_(std::move(inner)), times_(std::move(times)), cycles_(std::move(cycles)) {}
  [[nodiscard]] Verdict evaluate(const Secret& text) const override {
    return timed_call(kStagePolicy, *times_, cycles_.get(), std::nullopt, [&] { return inner_->evaluate(text); });
  }

 private:
  std::shared_ptr<const TextPolicy> inner_;
  std::shared_ptr<StageTimes> times_;
  std::shared_ptr<CycleLog> cycles_;
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
    out.policy = std::make_shared<TimedPolicy>(stages.policy, times, cycles);
  }
  return out;
}

}  // namespace spg::cli
