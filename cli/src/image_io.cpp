#include "image_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

namespace spg::cli {

namespace {

std::vector<unsigned char> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw CliError("cannot open " + path.string());
  }
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    throw CliError("cannot read " + path.string());
  }
  return bytes;
}

struct StbFree {
  void operator()(unsigned char* p) const noexcept { stbi_image_free(p); }
};

void append_bytes(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<unsigned char>*>(context);
  const auto* bytes = static_cast<const unsigned char*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

// Weights of the source cells [lo, hi) that one output cell covers, along
// one axis, for area averaging.
struct Span {
  std::size_t first = 0;
  std::vector<double> weights;
};

std::vector<Span> area_spans(std::uint32_t in, std::uint32_t out) {
  std::vector<Span> spans(out);
  const double scale = static_cast<double>(in) / static_cast<double>(out);
  for (std::uint32_t i = 0; i < out; ++i) {
    const double lo = static_cast<double>(i) * scale;
    const double hi = static_cast<double>(i + 1) * scale;
    const auto first = static_cast<std::size_t>(std::floor(lo));
    const auto last = std::min(static_cast<std::size_t>(std::ceil(hi)), static_cast<std::size_t>(in));
    Span& s = spans[i];
    s.first = first;
    for (std::size_t j = first; j < last; ++j) {
      const double cover = std::min(hi, static_cast<double>(j + 1)) - std::max(lo, static_cast<double>(j));
      s.weights.push_back(cover / (hi - lo));
    }
  }
  return spans;
}

}  // namespace

BgraImage read_png(const std::filesystem::path& path) {
  const std::vector<unsigned char> bytes = read_file(path);
  if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw CliError(path.string() + " is too large");
  }
  int w = 0;
  int h = 0;
  int channels = 0;
  const std::unique_ptr<unsigned char, StbFree> rgba(
      stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4));
  if (!rgba || w <= 0 || h <= 0) {
    throw CliError("cannot decode " + path.string() + " as an 8-bit PNG");
  }
  BgraImage image;
  image.width = static_cast<std::uint32_t>(w);
  image.height = static_cast<std::uint32_t>(h);
  const std::size_t n = static_cast<std::size_t>(image.width) * image.height;
  image.pixels.resize(n * 4);
  const unsigned char* src = rgba.get();
  for (std::size_t i = 0; i < n; ++i) {
    image.pixels[4 * i + 0] = src[4 * i + 2];
    image.pixels[4 * i + 1] = src[4 * i + 1];
    image.pixels[4 * i + 2] = src[4 * i + 0];
    image.pixels[4 * i + 3] = src[4 * i + 3];
  }
  return image;
}

void write_png(const std::filesystem::path& path, const BgraImage& image) {
  validate(image);
  const std::size_t n = static_cast<std::size_t>(image.width) * image.height;
  std::vector<unsigned char> rgba(n * 4);
  for (std::size_t i = 0; i < n; ++i) {
    rgba[4 * i + 0] = image.pixels[4 * i + 2];
    rgba[4 * i + 1] = image.pixels[4 * i + 1];
    rgba[4 * i + 2] = image.pixels[4 * i + 0];
    rgba[4 * i + 3] = image.pixels[4 * i + 3];
  }
  std::vector<unsigned char> encoded;
  const int ok = stbi_write_png_to_func(append_bytes, &encoded, static_cast<int>(image.width),
                                        static_cast<int>(image.height), 4, rgba.data(),
                                        static_cast<int>(image.width * 4));
  if (ok == 0) {
    throw CliError("cannot encode " + path.string());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
  if (!out) {
    throw CliError("cannot write " + path.string());
  }
}

Size readback_size(Size source, double scale) {
  const auto side = [scale](std::uint32_t v) {
    return static_cast<std::uint32_t>(std::max(1.0, std::round(static_cast<double>(v) * scale)));
  };
  return {side(source.width), side(source.height)};
}

BgraImage resize_area(const BgraImage& image, Size size) {
  validate(image);
  if (size == image.size()) {
    return image;
  }
  if (size.width == 0 || size.height == 0) {
    throw CliError("cannot resize a frame to a zero size");
  }
  const std::vector<Span> xs = area_spans(image.width, size.width);
  const std::vector<Span> ys = area_spans(image.height, size.height);
  // Horizontal pass into floats, then vertical into bytes.
  std::vector<double> rows(static_cast<std::size_t>(size.width) * image.height * 4);
  for (std::size_t y = 0; y < image.height; ++y) {
    const std::uint8_t* src = image.pixels.data() + y * image.width * 4;
    double* dst = rows.data() + y * size.width * 4;
    for (std::size_t x = 0; x < size.width; ++x) {
      const Span& s = xs[x];
      for (std::size_t k = 0; k < s.weights.size(); ++k) {
        for (std::size_t c = 0; c < 4; ++c) {
          dst[x * 4 + c] += s.weights[k] * src[(s.first + k) * 4 + c];
        }
      }
    }
  }
  BgraImage out;
  out.width = size.width;
  out.height = size.height;
  out.pixels.resize(static_cast<std::size_t>(size.width) * size.height * 4);
  for (std::size_t y = 0; y < size.height; ++y) {
    const Span& s = ys[y];
    for (std::size_t x = 0; x < size.width * 4; ++x) {
      double v = 0.0;
      for (std::size_t k = 0; k < s.weights.size(); ++k) {
        v += s.weights[k] * rows[(s.first + k) * size.width * 4 + x];
      }
      out.pixels[y * size.width * 4 + x] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
    }
  }
  return out;
}

}  // namespace spg::cli
