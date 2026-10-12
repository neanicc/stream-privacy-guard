#include "spg/compose.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "../mask/mask_rects.hpp"
#include "spg/error.hpp"

namespace spg {

namespace {

// Blur and pixelate mix all four channels, alpha included: a source with
// transparency (black text on a transparent background) shows its content in
// the alpha channel alone, so leaving alpha intact would leave that text
// readable over whatever is behind the source. Solid fill is opaque.
constexpr std::size_t kChannels = 4;  // B, G, R, A
constexpr std::size_t kBytesPerPixel = 4;
constexpr std::size_t kAlpha = 3;
constexpr std::uint8_t kOpaque = 255;

std::uint32_t scaled_size(std::uint32_t minimum, float ratio, std::uint32_t side) {
  const double scaled = std::ceil(static_cast<double>(ratio) * static_cast<double>(side));
  const double capped = std::min(scaled, static_cast<double>(std::numeric_limits<std::uint32_t>::max()));
  return std::max(minimum, static_cast<std::uint32_t>(capped));
}

// One pass of a box blur over `count` values, with the window clamped to the
// line's ends (edge values repeat). Reads `src`, writes `dst`.
void box_pass(const std::vector<std::uint32_t>& src, std::vector<std::uint32_t>& dst, std::size_t count,
              std::size_t radius) {
  const std::size_t last = count - 1;
  const std::size_t window = 2 * radius + 1;
  std::uint64_t sum = static_cast<std::uint64_t>(radius + 1) * src[0];
  for (std::size_t j = 1; j <= radius; ++j) {
    sum += src[std::min(j, last)];
  }
  for (std::size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<std::uint32_t>((sum + window / 2) / window);
    const std::size_t add = std::min(i + radius + 1, last);
    const std::size_t sub = i >= radius ? i - radius : 0;
    sum += src[add];
    sum -= src[sub];
  }
}

// Blurs `count` pixels starting at `first`, `step` bytes apart, channel by
// channel.
void blur_line(std::uint8_t* first, std::size_t count, std::size_t step, std::size_t radius, std::uint32_t passes,
               std::vector<std::uint32_t>& a, std::vector<std::uint32_t>& b) {
  if (count < 2) {
    return;
  }
  a.resize(count);
  b.resize(count);
  for (std::size_t c = 0; c < kChannels; ++c) {
    for (std::size_t i = 0; i < count; ++i) {
      a[i] = first[i * step + c];
    }
    for (std::uint32_t p = 0; p < passes; ++p) {
      box_pass(a, b, count, radius);
      a.swap(b);
    }
    for (std::size_t i = 0; i < count; ++i) {
      first[i * step + c] = static_cast<std::uint8_t>(std::min<std::uint32_t>(a[i], 255));
    }
  }
}

std::uint8_t* pixel_at(BgraImage& image, std::size_t x, std::size_t y) {
  return image.pixels.data() + (y * image.width + x) * kBytesPerPixel;
}

void fill(BgraImage& image, const PixelRect& r, const CompositorConfig& config) {
  for (auto y = static_cast<std::size_t>(r.y); y < static_cast<std::size_t>(r.y + r.height); ++y) {
    for (auto x = static_cast<std::size_t>(r.x); x < static_cast<std::size_t>(r.x + r.width); ++x) {
      std::uint8_t* p = pixel_at(image, x, y);
      for (std::size_t c = 0; c < config.fill_bgr.size(); ++c) {
        p[c] = config.fill_bgr[c];
      }
      p[kAlpha] = kOpaque;
    }
  }
}

// A rect under 2 px on a side has nothing to average across (a 1x1 blur or
// pixelate block returns the pixel unchanged), so it is filled instead: a
// mask must always hide what it covers.
bool too_thin(const PixelRect& r) noexcept { return r.width < 2 || r.height < 2; }

void blur(BgraImage& image, const PixelRect& r, const CompositorConfig& config) {
  if (too_thin(r)) {
    fill(image, r, config);
    return;
  }
  const auto w = static_cast<std::size_t>(r.width);
  const auto h = static_cast<std::size_t>(r.height);
  const auto x0 = static_cast<std::size_t>(r.x);
  const auto y0 = static_cast<std::size_t>(r.y);
  const std::size_t radius = scaled_size(config.min_blur_radius, config.blur_radius_ratio,
                                         static_cast<std::uint32_t>(std::min(w, h)));
  std::vector<std::uint32_t> a;
  std::vector<std::uint32_t> b;
  for (std::size_t y = y0; y < y0 + h; ++y) {
    blur_line(pixel_at(image, x0, y), w, kBytesPerPixel, radius, config.blur_passes, a, b);
  }
  const std::size_t row = static_cast<std::size_t>(image.width) * kBytesPerPixel;
  for (std::size_t x = x0; x < x0 + w; ++x) {
    blur_line(pixel_at(image, x, y0), h, row, radius, config.blur_passes, a, b);
  }
}

void pixelate(BgraImage& image, const PixelRect& r, const CompositorConfig& config) {
  if (too_thin(r)) {
    fill(image, r, config);
    return;
  }
  const auto w = static_cast<std::size_t>(r.width);
  const auto h = static_cast<std::size_t>(r.height);
  const auto x0 = static_cast<std::size_t>(r.x);
  const auto y0 = static_cast<std::size_t>(r.y);
  const std::size_t block = scaled_size(config.min_pixel_block, config.pixel_block_ratio,
                                        static_cast<std::uint32_t>(std::min(w, h)));
  for (std::size_t by = y0; by < y0 + h; by += block) {
    const std::size_t ey = std::min(by + block, y0 + h);
    for (std::size_t bx = x0; bx < x0 + w; bx += block) {
      const std::size_t ex = std::min(bx + block, x0 + w);
      std::uint64_t sums[kChannels] = {};
      for (std::size_t y = by; y < ey; ++y) {
        for (std::size_t x = bx; x < ex; ++x) {
          const std::uint8_t* p = pixel_at(image, x, y);
          for (std::size_t c = 0; c < kChannels; ++c) {
            sums[c] += p[c];
          }
        }
      }
      const std::uint64_t n = (ey - by) * (ex - bx);
      std::uint8_t mean[kChannels];
      for (std::size_t c = 0; c < kChannels; ++c) {
        mean[c] = static_cast<std::uint8_t>((sums[c] + n / 2) / n);
      }
      for (std::size_t y = by; y < ey; ++y) {
        for (std::size_t x = bx; x < ex; ++x) {
          std::uint8_t* p = pixel_at(image, x, y);
          for (std::size_t c = 0; c < kChannels; ++c) {
            p[c] = mean[c];
          }
        }
      }
    }
  }
}

void draw(BgraImage& image, const PixelRect& r, MaskStyle style, const CompositorConfig& config) {
  if (r.width <= 0 || r.height <= 0) {
    return;
  }
  switch (style) {
    case MaskStyle::Pixelate:
      pixelate(image, r, config);
      return;
    case MaskStyle::SolidFill:
      fill(image, r, config);
      return;
    case MaskStyle::Blur:
      break;
  }
  // Blur, and any value outside the enum: hide rather than skip.
  blur(image, r, config);
}

int strength(MaskStyle style) noexcept {
  switch (style) {
    case MaskStyle::Blur:
      return 0;
    case MaskStyle::Pixelate:
      return 1;
    case MaskStyle::SolidFill:
      return 2;
  }
  return 2;
}

double area_of(const Rect& r) noexcept {
  return static_cast<double>(r.width) * static_cast<double>(r.height);
}

// Area covered by both rects (0 when they do not overlap).
double overlap_area(const Rect& a, const Rect& b) noexcept {
  const double w = std::min(static_cast<double>(a.x) + a.width, static_cast<double>(b.x) + b.width) -
                   std::max(static_cast<double>(a.x), static_cast<double>(b.x));
  const double h = std::min(static_cast<double>(a.y) + a.height, static_cast<double>(b.y) + b.height) -
                   std::max(static_cast<double>(a.y), static_cast<double>(b.y));
  return w > 0.0 && h > 0.0 ? w * h : 0.0;
}

void check_source(const MaskRect& rect) {
  if (rect.box.space != Space::Source || !is_valid(rect.box.rect)) {
    throw ContractError("compositor: mask rects must be valid boxes in Source space");
  }
}

// Removes every rect another rect contains (the first of two equal rects
// stays).
void drop_contained(std::vector<MaskRect>& rects) {
  std::vector<bool> gone(rects.size(), false);
  for (std::size_t i = 0; i < rects.size(); ++i) {
    for (std::size_t j = 0; j < rects.size() && !gone[i]; ++j) {
      if (i == j || gone[j] || !mask::contains(rects[j].box.rect, rects[i].box.rect)) {
        continue;
      }
      const bool equal = mask::contains(rects[i].box.rect, rects[j].box.rect);
      // Of two equal rects keep the earlier one, with the stronger style.
      if (equal && i < j) {
        continue;
      }
      if (strength(rects[i].style) > strength(rects[j].style)) {
        rects[j].style = rects[i].style;
      }
      gone[i] = true;
    }
  }
  std::size_t out = 0;
  for (std::size_t i = 0; i < rects.size(); ++i) {
    if (!gone[i]) {
      rects[out++] = rects[i];
    }
  }
  rects.resize(out);
}

}  // namespace

FrameView BgraImage::view(Timestamp t) const noexcept {
  FrameView v;
  v.planes[0] = pixels.data();
  v.strides[0] = width * static_cast<std::uint32_t>(kBytesPerPixel);
  v.width = width;
  v.height = height;
  v.format = PixelFormat::BGRA;
  v.timestamp = t;
  return v;
}

void validate(const BgraImage& image) {
  if (image.width == 0 || image.height == 0) {
    throw ContractError("compositor: image has a zero dimension");
  }
  const std::size_t expected = static_cast<std::size_t>(image.width) * image.height * kBytesPerPixel;
  if (image.pixels.size() != expected) {
    throw ContractError("compositor: image holds " + std::to_string(image.pixels.size()) + " bytes, expected " +
                        std::to_string(expected));
  }
}

void validate(const CompositorConfig& config) {
  if (config.min_blur_radius == 0) {
    throw ContractError("compositor config: min_blur_radius must be at least 1");
  }
  if (config.blur_passes == 0) {
    throw ContractError("compositor config: blur_passes must be at least 1");
  }
  if (config.min_pixel_block == 0) {
    throw ContractError("compositor config: min_pixel_block must be at least 1");
  }
  if (!std::isfinite(config.blur_radius_ratio) || config.blur_radius_ratio < 0.0F) {
    throw ContractError("compositor config: blur_radius_ratio must be finite and not negative");
  }
  if (!std::isfinite(config.pixel_block_ratio) || config.pixel_block_ratio < 0.0F) {
    throw ContractError("compositor config: pixel_block_ratio must be finite and not negative");
  }
}

std::vector<MaskRect> fit_mask_rects(std::span<const MaskRect> rects, std::size_t capacity) {
  if (capacity == 0) {
    throw ContractError("compositor: rect capacity must be at least 1");
  }
  for (const MaskRect& rect : rects) {
    check_source(rect);
  }
  std::vector<MaskRect> out(rects.begin(), rects.end());
  drop_contained(out);
  while (out.size() > capacity) {
    // The pair whose hull adds the least area beyond what the two rects
    // already cover together (their union, not the sum of their areas).
    std::size_t best_i = 0;
    std::size_t best_j = 1;
    double best_cost = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < out.size(); ++i) {
      for (std::size_t j = i + 1; j < out.size(); ++j) {
        const Rect h = mask::hull(out[i].box.rect, out[j].box.rect);
        const Rect& a = out[i].box.rect;
        const Rect& b = out[j].box.rect;
        const double cost = area_of(h) - (area_of(a) + area_of(b) - overlap_area(a, b));
        if (cost < best_cost) {
          best_cost = cost;
          best_i = i;
          best_j = j;
        }
      }
    }
    MaskRect merged = out[best_i];
    merged.box.rect = mask::hull(out[best_i].box.rect, out[best_j].box.rect);
    if (strength(out[best_j].style) > strength(merged.style)) {
      merged.style = out[best_j].style;
    }
    out[best_i] = merged;
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(best_j));
    drop_contained(out);
  }
  return out;
}

void compose(BgraImage& image, const MaskPlan& plan, FallbackAction action, const CompositorConfig& config) {
  validate(image);
  validate(config);
  for (const MaskRect& rect : plan.rects) {
    check_source(rect);
  }
  const PixelRect whole = enclosing_pixels(
      Rect{0.0F, 0.0F, static_cast<float>(image.width), static_cast<float>(image.height)}, image.size());
  if (action == FallbackAction::SolidFill) {
    fill(image, whole, config);
    return;
  }
  if (action != FallbackAction::None || plan.fallback) {
    // BlurSource; HoldLastScanned drawn here (the caller should have shown
    // the re-emitted frame); a failed-closed plan with no action.
    blur(image, whole, config);
    return;
  }
  for (const MaskRect& rect : plan.rects) {
    draw(image, enclosing_pixels(rect.box.rect, image.size()), rect.style, config);
  }
}

}  // namespace spg
