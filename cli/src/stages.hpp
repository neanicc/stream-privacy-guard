#pragma once

// The detection stages spg-cli runs (SPG-11): the pinned models opened from
// $SPG_CACHE_DIR, and wrappers that time every stage call. Recognized text
// stays inside the stages; nothing here logs or stores it.

#include <filesystem>
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
};

struct OpenedStages {
  ScanStages stages;
  // Which execution provider each opened model runs on, for the report.
  std::string providers;
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
