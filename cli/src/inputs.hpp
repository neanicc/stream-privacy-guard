#pragma once

// Inputs for spg-cli (SPG-11): a PNG frame sequence, a policy file, and a
// generated eval suite (docs/eval.md "Generated suites"). Errors name files,
// fields and counts only; the policy's watch-list is never printed.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "spg/geometry.hpp"
#include "spg/policy_config.hpp"
#include "spg/time.hpp"

namespace spg::cli {

struct FrameSequence {
  std::string name;
  std::vector<std::filesystem::path> frames;  // in frame order
  std::uint32_t fps = 60;
  Size size;  // as the suite declares it; zero when unknown
};

// The *.png files directly in `dir`, sorted by file name. Throws CliError when
// the directory cannot be listed or holds none.
[[nodiscard]] std::vector<std::filesystem::path> list_frames(const std::filesystem::path& dir);

// A schema-1 policy config file (docs/contracts.md). Throws CliError naming
// the file; the parser's message names fields and offsets, never text.
[[nodiscard]] PolicyConfig read_policy(const std::filesystem::path& file);

struct Suite {
  std::filesystem::path dir;
  std::string name;
  std::uint32_t fps = 60;
  PolicyConfig policy;
  std::vector<FrameSequence> sequences;
};

// Reads <dir>/suite.json, the policy file it names, and each sequence's frame
// list, which must hold exactly the frame count suite.json declares. Throws
// CliError otherwise.
[[nodiscard]] Suite read_suite(const std::filesystem::path& dir);

// Frame i's timestamp at `fps`: i * 1e9 / fps nanoseconds, rounded down, as
// the generators write it (eval/spg_eval/render.py).
[[nodiscard]] Timestamp frame_time(std::size_t index, std::uint32_t fps);

}  // namespace spg::cli
