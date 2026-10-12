#pragma once

// The detection models in the plugin (SPG-14b), without OBS: find the pinned
// YuNet, PP-OCR det and PP-OCR rec files, check each against its size and
// SHA-256 from models/manifest.json, open them through the spg-core runtime
// wrapper (warmup included), and turn them into the run_scan stages for a
// filter's PolicyConfig. Models are read from local files only: nothing here
// downloads anything (scripts/fetch_models.py fills the cache).
//
// Any file missing, of the wrong size or hash, or rejected by ONNX Runtime
// leaves the filter with no detectors, so it covers every frame whole (fail
// closed). Reports carry model names, paths and runtime messages, never frame
// content or text read from a frame.

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

struct ModelPaths {
  std::filesystem::path faces;
  std::filesystem::path text;
  std::filesystem::path recognizer;
};

// Finds each pinned model in the first of `dirs` that has its file (a
// packaged data directory first, then the cache), then checks its size and
// SHA-256. status is Ready when all three match; otherwise Missing or Corrupt,
// naming the first model at fault.
[[nodiscard]] ModelReport locate_models(std::span<const std::filesystem::path> dirs, ModelPaths& out);

// The opened models, shared by every filter's stages: each detector is safe
// to call from several worker threads at once.
class LoadedModels;

struct LoadOptions {
  // Platform default execution providers with CPU fallback, or CPU only.
  bool cpu_only = false;
};

struct LoadResult {
  std::shared_ptr<const LoadedModels> models;  // null unless report.status is Ready
  ModelReport report;
};

// locate_models(), then opens and warms up each model. Slow (hundreds of
// milliseconds to seconds): never call it on the render, video or UI thread.
// Throws only std::bad_alloc.
[[nodiscard]] LoadResult load_models(std::span<const std::filesystem::path> dirs, const LoadOptions& options = {});

// The run_scan stages for `policy`: the face detector, the text detector and
// the recognizer from `models`, and a policy engine built from `policy`. With
// null `models` every detector is null, so a cycle with any category enabled
// fails with ScanFailure::MissingStage and frames get the fallback. Throws
// ContractError when the policy engine rejects `policy`.
[[nodiscard]] ScanStages make_stages(const std::shared_ptr<const LoadedModels>& models, const PolicyConfig& policy);

}  // namespace spg::plugin
