#pragma once

// spg-cli scrub (SPG-18): finds the frames of a recorded session in which a
// planted test string is still readable. Every frame goes through the text
// detector and the recognizer the pipeline uses, and every region's text is
// matched against each planted string with the policy engine's watch-list
// matcher, one engine per string, so a hit names the string by its index.
//
// Planted strings are secret: they and the recognized text stay in Secrets,
// and nothing here prints, logs or stores them. Results hold frame indices,
// boxes and planted-string indices only. No frame is written anywhere.
// Usage and rules: docs/cli.md, docs/live-check.md.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "driver.hpp"
#include "spg/geometry.hpp"
#include "spg/policy.hpp"
#include "spg/policy_config.hpp"
#include "spg/scan.hpp"
#include "spg/secret.hpp"
#include "timing.hpp"

namespace spg::cli {

// Planted strings from a UTF-8 text file, one per line (LF or CR LF). Lines
// holding only ASCII whitespace are skipped; #1 is the first other line. The
// file's bytes are wiped once split. Throws CliError naming the file, never a
// line's text, when it cannot be read or holds no string.
[[nodiscard]] std::vector<Secret> read_planted(const std::filesystem::path& file);

// The two fuzzy-matching levels the plugin's Sensitivity setting offers
// (plugin/src/settings/filter_settings.cpp): Normal is spg-core's
// FuzzyLimits defaults, High allows more edits and shorter partial reveals.
enum class ScrubSensitivity : std::uint8_t { Normal, High };
[[nodiscard]] FuzzyLimits scrub_fuzzy(ScrubSensitivity sensitivity) noexcept;

// Matches text against each planted string separately with a PolicyEngine
// limited to the watch-list category.
class PlantedMatcher {
 public:
  // Throws CliError naming the 1-based index of a string the engine rejects
  // (one with no letters or digits once normalized, which could never match).
  PlantedMatcher(const std::vector<Secret>& planted, FuzzyLimits fuzzy);
  // The 1-based indices of the planted strings `text` matches, ascending.
  [[nodiscard]] std::vector<std::size_t> match(const Secret& text) const;
  [[nodiscard]] std::size_t size() const noexcept { return engines_.size(); }

 private:
  std::vector<std::unique_ptr<const PolicyEngine>> engines_;
};

// One text region of one frame whose text matched planted strings.
struct ScrubLeak {
  std::size_t frame = 0;  // index in the sequence (frames sorted by file name)
  Box box;                // the detected region, Source pixels of the frame
  float score = 0.0F;     // the text detector's score
  std::vector<std::size_t> planted;  // 1-based, ascending
};

struct ScrubResult {
  std::size_t frames = 0;
  std::size_t regions = 0;       // text regions read over all frames
  std::vector<ScrubLeak> leaks;  // by frame, then in the detector's order
  // The frames with at least one leak, ascending, without repeats.
  [[nodiscard]] std::vector<std::size_t> leak_frames() const;
  // The frames in which planted string `index` (1-based) is readable.
  [[nodiscard]] std::vector<std::size_t> leak_frames_of(std::size_t index) const;
};

struct ScrubOptions {
  // Frames scanned at once, each on its own thread. At least 1.
  std::size_t workers = 2;
  // Called after each frame with the number of frames done so far, from the
  // worker that finished it, one call at a time.
  std::function<void(std::size_t done)> progress;
};

// Scans every frame of `source` at full resolution: stages.text, then
// stages.recognizer on every region, then `matcher` on each region's text.
// The stages must be safe to call from several threads, as the pipeline's
// are. Throws CliError when stages.text or stages.recognizer is null or the
// recognizer returns the wrong number of texts, and passes on any exception
// from loading a frame or from a stage: a frame that could not be checked is
// never counted as clean. Adds the matching time of each region to `times`
// as `policy`; decoding and the stages are timed by the caller's source and
// timed() wrappers.
[[nodiscard]] ScrubResult scrub(const FrameSource& source, const ScanStages& stages,
                                const PlantedMatcher& matcher, const ScrubOptions& options, StageTimes& times);

}  // namespace spg::cli
