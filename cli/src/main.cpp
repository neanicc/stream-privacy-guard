// spg-cli entry point. `run`, `bench`, `eval` and `scrub` are planned (SPG-11,
// SPG-18); until then they exit non-zero so no script mistakes them for a pass.
#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>

#include "spg/version.hpp"

namespace {

constexpr int kUsageError = 2;

void print_usage(std::ostream& out) {
  out << "usage: spg-cli <command> [options]\n"
         "\n"
         "commands (not implemented yet):\n"
         "  run     run the pipeline on a PNG sequence and write mask plans (SPG-11)\n"
         "  bench   measure stage latency and fail-closed rate (SPG-11)\n"
         "  eval    run an eval suite end to end (SPG-11)\n"
         "  scrub   find planted strings still readable in a recording (SPG-18)\n"
         "\n"
         "options:\n"
         "  --version  print the version and exit\n"
         "  --help     print this help and exit\n";
}

}  // namespace

int main(int argc, char** argv) {
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));
  if (args.size() < 2) {
    print_usage(std::cerr);
    return kUsageError;
  }
  const std::string_view command = args[1];
  if (command == "--version") {
    std::cout << "spg-cli " << spg::version() << '\n';
    return 0;
  }
  if (command == "--help" || command == "-h") {
    print_usage(std::cout);
    return 0;
  }
  if (command == "run" || command == "bench" || command == "eval" || command == "scrub") {
    std::cerr << "spg-cli: '" << command << "' is not implemented yet\n";
    return kUsageError;
  }
  std::cerr << "spg-cli: unknown command '" << command << "'\n";
  print_usage(std::cerr);
  return kUsageError;
}
