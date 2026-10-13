#pragma once

// The detection models in the plugin (SPG-14b), without OBS: find the pinned
// YuNet, PP-OCR det and PP-OCR rec files, read each once into memory and
// check those bytes against its size and SHA-256 from models/manifest.json,
// open the same bytes through the spg-core runtime wrapper (warmup included;
// ONNX Runtime never reopens the file, so a file swapped after the check is
// never loaded), and turn them into the run_scan stages for a filter's
// PolicyConfig. Models are read from local files only: nothing here
// downloads anything (scripts/fetch_models.py fills the cache).
//
// Any file missing, of the wrong size or hash, or rejected by ONNX Runtime
// leaves the filter with no detectors, so it covers every frame whole (fail
// closed). Reports carry model names, paths and runtime messages, never frame
// content or text read from a frame.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "spg/policy_config.hpp"
#include "spg/scan.hpp"

namespace spg::plugin {

struct PinnedModel {
  const char* name;      // key in models/manifest.json
  const char* relative;  // <name>/<version>/<file>, under a models directory
  std::uint64_t size;    // bytes
  const char* sha256;    // lower-case hex
};

// YuNet (faces), PP-OCR det (text regions), PP-OCR rec (recognizer), in that
// order, as pinned in models/manifest.json when the build was configured.
[[nodiscard]] std::span<const PinnedModel> pinned_models() noexcept;

enum class ModelStatus : std::uint8_t {
  Idle,     // loading not started
  Loading,  // on the loader thread
  Ready,    // every model opened and warmed up
  Missing,  // a model file is in none of the directories searched
  Corrupt,  // a model file has the wrong size or hash, or ONNX Runtime rejected it
  Failed,   // anything else (no execution provider, out of memory, ...)
};

[[nodiscard]] const char* status_name(ModelStatus status) noexcept;

struct ModelReport {
  ModelStatus status = ModelStatus::Idle;
  std::string model;      // manifest name of the model at fault; empty otherwise
  std::string detail;     // for the log: path and reason, or the providers in use
};

// $SPG_CACHE_DIR/models, else ~/.cache/spg/models (%USERPROFILE% on Windows):
// where scripts/fetch_models.py puts them. Empty when neither is set.
[[nodiscard]] std::filesystem::path default_models_dir();

// A pinned model file as read and checked: its bytes are what gets opened.
struct ModelFile {
  std::filesystem::path path;
  std::vector<std::byte> bytes;
};

struct ModelFiles {
  ModelFile faces;
  ModelFile text;
  ModelFile recognizer;
};

// Finds each pinned model in the first of `dirs` that has its file (a
// packaged data directory first, then the cache), reads it (never more than
// its pinned size plus one byte) and checks the size and SHA-256 of what was
// read. status is Ready when all three match; otherwise Missing or Corrupt,
// naming the first model at fault. Throws only std::bad_alloc.
[[nodiscard]] ModelReport locate_models(std::span<const std::filesystem::path> dirs, ModelFiles& out);

// The opened models, shared by every filter's stages: each detector is safe
// to call from several worker threads at once.
class LoadedModels;

// Recognizer model runs in flight at once per filter, summed over its
// scheduler's workers: each worker inside recognize() runs up to
// TextRecognizerConfig::parallel_runs crops side by side, all on the one
// shared session and its ONNX Runtime intra-op pool. 4 is what one worker ran
// when the dev Mac met the realtime limits (agent-log SPG-16, SPG-16c); the
// old default of two workers with 4 runs each put 8 in flight (#85).
inline constexpr std::size_t kRecognizerRunBudget = 4;

// parallel_runs for a scheduler with `workers` workers: the budget split
// evenly, at least 1. So workers * parallel_runs <= kRecognizerRunBudget for
// up to kRecognizerRunBudget workers; past that each worker still reads one
// crop at a time.
[[nodiscard]] std::size_t recognizer_parallel_runs(std::size_t workers) noexcept;

// Scan workers in every filter's scheduler: the core default
// (SchedulerConfig::workers). The filter sets its scheduler's worker count
// from this, and load_models() sizes the recognizer from it, so the run
// budget holds whatever SchedulerConfig::from_policy() does with workers.
[[nodiscard]] std::size_t filter_scan_workers();

struct LoadOptions {
  // Platform default execution providers with CPU fallback, or CPU only.
  bool cpu_only = false;
};

struct LoadResult {
  std::shared_ptr<const LoadedModels> models;  // null unless report.status is Ready
  ModelReport report;
};

// locate_models(), then opens and warms up each model. The recognizer runs
// recognizer_parallel_runs(filter_scan_workers()) crops at once, sized for
// the workers every filter's scheduler gets. Slow (hundreds of milliseconds
// to seconds): never call it on the graphics or UI thread.
// Throws only std::bad_alloc.
[[nodiscard]] LoadResult load_models(std::span<const std::filesystem::path> dirs, const LoadOptions& options = {});

// The run_scan stages for `policy`: the face detector, the text detector and
// the recognizer from `models`, and a policy engine built from `policy`. With
// null `models` every detector is null, so a cycle with any category enabled
// fails with ScanFailure::MissingStage and frames get the fallback. Throws
// ContractError when the policy engine rejects `policy`.
[[nodiscard]] ScanStages make_stages(const std::shared_ptr<const LoadedModels>& models, const PolicyConfig& policy);

}  // namespace spg::plugin
