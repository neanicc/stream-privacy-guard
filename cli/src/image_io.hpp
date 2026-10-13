#pragma once

// PNG frames for spg-cli (SPG-11): read a frame as BGRA, write a debug frame,
// and scale a frame down to the readback size the workers scan. Errors name
// the file and sizes only.

#include <filesystem>
#include <stdexcept>
#include <string>

#include "spg/compose.hpp"
#include "spg/geometry.hpp"

namespace spg::cli {

// A failure the CLI reports and exits 1 on: a missing or unreadable input, a
// bad option value, an output it cannot write.
class CliError : public std::runtime_error {
 public:
  explicit CliError(const std::string& what) : std::runtime_error(what) {}
};

// Reads an 8-bit PNG (gray, gray and alpha, RGB or RGBA) as BGRA; alpha is
// 255 when the file has none. Throws CliError when the file cannot be read or
// decoded.
[[nodiscard]] BgraImage read_png(const std::filesystem::path& path);

// Writes `image` as an RGBA PNG. Throws CliError on failure.
void write_png(const std::filesystem::path& path, const BgraImage& image);

// The readback size for a source of size `source` at `scale` (in (0, 1]):
// each side scaled and rounded, at least 1 pixel.
[[nodiscard]] Size readback_size(Size source, double scale);

// `image` resized to `size` by area averaging (each output pixel is the mean
// of the source area it covers). Returns a copy when the sizes match.
[[nodiscard]] BgraImage resize_area(const BgraImage& image, Size size);

}  // namespace spg::cli
