#include "ocr/recognition_cache.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "detect/frame_image.hpp"
#include "spg/error.hpp"

namespace spg::ocr {

namespace {

constexpr double kCellsAcross = 6.0;  // thumbnail cells across the shorter side
constexpr double kMaxCells = 256.0;

std::uint32_t cells(std::int32_t length, std::int32_t shorter) {
  const double n = std::round(kCellsAcross * static_cast<double>(length) / static_cast<double>(shorter));
  return static_cast<std::uint32_t>(std::clamp(n, 1.0, kMaxCells));
}

bool within(std::uint32_t now, std::uint32_t before, float tolerance) {
  const double diff = std::abs(static_cast<double>(now) - static_cast<double>(before));
  return diff <= static_cast<double>(tolerance) * static_cast<double>(before);
}

}  // namespace

void validate(const RecognitionCacheConfig& config) {
  if (config.max_age < Duration::zero()) {
    throw ContractError("recognition cache: max_age must not be negative");
  }
  if (!(config.size_tolerance >= 0.0F && config.size_tolerance <= 1.0F)) {
    throw ContractError("recognition cache: size_tolerance must be in [0, 1]");
  }
  if (!(config.content_tolerance >= 0.0F && config.content_tolerance <= 255.0F)) {
    throw ContractError("recognition cache: content_tolerance must be in [0, 255]");
  }
  if (config.capacity < 1) {
    throw ContractError("recognition cache: capacity must be at least 1");
  }
}

Fingerprint fingerprint(const FrameView& frame, Size source, const Box& region) {
  validate(frame);
  const PixelRect crop = readback_crop(region, ReadbackMapping(source, frame.size()));
  Fingerprint print;
  if (crop.width <= 0 || crop.height <= 0) {
    return print;
  }
  const std::int32_t shorter = std::min(crop.width, crop.height);
  print.crop = {static_cast<std::uint32_t>(crop.width), static_cast<std::uint32_t>(crop.height)};
  print.grid = {cells(crop.width, shorter), cells(crop.height, shorter)};
  const Rect rect{static_cast<float>(crop.x), static_cast<float>(crop.y), static_cast<float>(crop.width),
                  static_cast<float>(crop.height)};
  const detect::BgrImage thumbnail =
      detect::render_model_input(frame, ModelInputMapping(rect, print.grid, Fit::Stretch));
  const std::size_t count = static_cast<std::size_t>(print.grid.width) * print.grid.height;
  print.luma.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint8_t* p = thumbnail.pixels.data() + 3 * i;
    print.luma[i] = static_cast<std::uint8_t>(std::lround(0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]));
  }
  return print;
}

bool same_content(const Fingerprint& now, const Fingerprint& before, const RecognitionCacheConfig& config) {
  if (now.luma.empty() || before.luma.empty() || now.grid != before.grid || now.luma.size() != before.luma.size()) {
    return false;
  }
  if (!within(now.crop.width, before.crop.width, config.size_tolerance) ||
      !within(now.crop.height, before.crop.height, config.size_tolerance)) {
    return false;
  }
  for (std::size_t i = 0; i < now.luma.size(); ++i) {
    if (std::abs(static_cast<int>(now.luma[i]) - static_cast<int>(before.luma[i])) >
        static_cast<int>(std::floor(config.content_tolerance))) {
      return false;
    }
  }
  return true;
}

RecognitionCache::RecognitionCache(const RecognitionCacheConfig& config) : config_(config) {
  validate(config_);
}

std::optional<Recognition> RecognitionCache::find(TrackId track, const Fingerprint& print, Timestamp now) const {
  const auto it = entries_.find(track);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  const Entry& entry = it->second;
  if (now < entry.read_at || now - entry.read_at > config_.max_age || !same_content(print, entry.print, config_)) {
    return std::nullopt;
  }
  return entry.recognition;
}

void RecognitionCache::store(TrackId track, Fingerprint print, Recognition recognition, Timestamp now) {
  entries_.insert_or_assign(track, Entry{std::move(print), std::move(recognition), now});
  while (entries_.size() > config_.capacity) {
    const auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
      return a.second.read_at < b.second.read_at;
    });
    entries_.erase(oldest);
  }
}

void RecognitionCache::erase(TrackId track) {
  entries_.erase(track);
}

void RecognitionCache::prune(Timestamp now) {
  std::erase_if(entries_, [&](const auto& item) {
    const Timestamp read_at = item.second.read_at;
    return now < read_at || now - read_at > config_.max_age;
  });
}

std::vector<TextHit> recognize_tracks(const TextRecognizer& recognizer, RecognitionCache& cache,
                                      const FrameView& frame, Size source, std::span<const TrackedRegion> regions,
                                      CacheStats* stats) {
  const Timestamp now = frame.timestamp;
  std::vector<Fingerprint> prints;
  std::vector<std::optional<Recognition>> found;
  std::vector<std::size_t> missing;
  std::vector<Detection> to_read;
  prints.reserve(regions.size());
  found.reserve(regions.size());
  for (std::size_t i = 0; i < regions.size(); ++i) {
    validate(regions[i].detection);
    prints.push_back(fingerprint(frame, source, regions[i].detection.box));
    found.push_back(cache.find(regions[i].track, prints.back(), now));
    if (!found.back()) {
      missing.push_back(i);
      to_read.push_back(regions[i].detection);
    }
  }
  std::vector<Recognition> read = recognizer.recognize(frame, source, to_read);  // throws before any store
  for (std::size_t k = 0; k < missing.size(); ++k) {
    const std::size_t i = missing[k];
    cache.store(regions[i].track, prints[i], read[k], now);
    found[i] = std::move(read[k]);
  }
  std::vector<TextHit> hits(regions.size());
  for (std::size_t i = 0; i < regions.size(); ++i) {
    hits[i].detection = regions[i].detection;
    hits[i].text = std::move(found[i]->text);
  }
  if (stats != nullptr) {
    stats->hits = regions.size() - missing.size();
    stats->misses = missing.size();
  }
  return hits;
}

}  // namespace spg::ocr
