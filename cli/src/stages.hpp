#pragma once

// The detection stages spg-cli runs (SPG-11): the pinned models opened from
// $SPG_CACHE_DIR, and wrappers that time every stage call. Recognized text
// stays inside the stages; nothing here logs or stores it.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "spg/policy_config.hpp"
#include "spg/scan.hpp"
#include "timing.hpp"

namespace spg::cli {

struct ModelPaths {
  std::filesystem::path faces;            // YuNet
  std::filesystem::path text_detector;    // PP-OCR det
  std::filesystem::path text_recognizer;  // PP-OCR rec
};

// $SPG_CACHE_DIR, else $HOME/.cache/spg, else %USERPROFILE%/.cache/spg, as
// scripts/ci/run.sh and the CMake build choose it.
[[nodiscard]] std::filesystem::path default_cache_dir();

// The models pinned in models/manifest.json, under `cache`/models/<name>/<version>/<file>.
[[nodiscard]] ModelPaths model_paths(const std::filesystem::path& cache);

struct StageOptions {
  // Execution providers: the platform default order with CPU fallback
  // (false), or CPU only (true).
  bool cpu_only = false;
  // ONNX Runtime intra-op threads per model; 0 keeps its default.
  int intra_op_threads = 0;
  // TextRecognizerConfig::parallel_runs: recognizer model runs side by side
  // in one recognize() call, in [1, 16]. The recognizer's default (checked
  // in stages_models.cpp), as the plugin runs with one worker.
  std::size_t recognizer_runs = 4;
  // Offline settings for `scrub` (SPG-18), which must not miss text the
  // pipeline would miss only to save time: tiles overlap by half the shorter
  // tile side, so a line that tall is never cut by a tile edge, and the text
  // detector keeps candidate boxes from a mean probability of 0.3 (the
  // binarization threshold) instead of 0.5. Off for run, eval and bench,
  // which must score what the plugin runs.
  bool thorough = false;
  // Put the content-keyed recognition cache (ocr::CachedRecognizer) in front
  // of the recognizer, as the plugin does. Off only to measure it
  // (--no-recognition-cache): a hit returns what a read would.
  bool recognition_cache = true;
};

// Text regions the recognition cache answered and regions it read.
struct RecognitionCounts {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
};

struct OpenedStages {
  ScanStages stages;
  // Which execution provider each opened model runs on, for the report.
  std::string providers;
  // The recognition cache's counts so far; empty without a cache.
  std::function<RecognitionCounts()> recognition_counts;
};

// Opens the models `policy` needs: YuNet when Face is enabled, the text
// detector and recognizer when any text category is, and the policy engine.
// A disabled category's detector stays null (docs/scheduler.md). Throws
// CliError when a model cannot be opened, or when this build has no models
// (configured with SPG_WITH_DEPS=OFF).
[[nodiscard]] OpenedStages open_stages(const ModelPaths& models, const PolicyConfig& policy,
                                       const StageOptions& options);

// `stages` with every non-null stage wrapped to add its call time to `times`
// (kStageFaces, kStageText, kStageRecognize, kStagePolicy) and, when `cycles`
// is set, the end of each call to the cycle of the readback it scanned (a
// policy call, which has no readback, to the cycle whose earlier stages ran
// on the same thread).
// Exceptions pass through unchanged, so run_scan() still fails the cycle.
[[nodiscard]] ScanStages timed(const ScanStages& stages, std::shared_ptr<StageTimes> times,
                               std::shared_ptr<CycleLog> cycles = nullptr);

}  // namespace spg::cli
