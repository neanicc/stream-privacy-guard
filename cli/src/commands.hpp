#pragma once

// spg-cli commands: run, eval and bench (SPG-11) and scrub (SPG-18). main()
// calls run_cli(); the tests call it too, with fake stages. Output holds
// counts, timings, frame indices and boxes only, never recognized text,
// watch-list entries or planted strings. Usage: docs/cli.md.

#include <functional>
#include <ostream>
#include <span>
#include <string_view>

#include "spg/policy_config.hpp"
#include "stages.hpp"

namespace spg::cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitUsage = 2;
// scrub only: the scan finished and found a planted string readable in at
// least one frame.
inline constexpr int kExitLeak = 3;

struct CliHooks {
  // Opens the stages for a policy; open_stages() on the pinned models when
  // unset. Tests pass fakes.
  std::function<OpenedStages(const PolicyConfig& policy, const StageOptions& options,
                             const std::filesystem::path& cache)>
      open;
};

// `args` excludes the program name. Returns the process exit code: 0 on
// success, 1 when the command failed, 2 for a usage error, 3 when scrub found
// a leak.
[[nodiscard]] int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
                          const CliHooks& hooks = {});

}  // namespace spg::cli
