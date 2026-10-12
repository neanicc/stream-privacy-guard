#include "ocr/recognition_cache.hpp"

#include <algorithm>
#include <utility>

#include "detect/frame_image.hpp"
#include "spg/error.hpp"

namespace spg::ocr {

void validate(const RecognitionCacheConfig& config) {
  if (config.max_age < Duration::zero()) {
    throw ContractError("recognition cache: max_age must not be negative");
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
  print.crop = {static_cast<std::uint32_t>(crop.width), static_cast<std::uint32_t>(crop.height)};
  const Rect rect{static_cast<float>(crop.x), static_cast<float>(crop.y), static_cast<float>(crop.width),
                  static_cast<float>(crop.height)};
  print.pixels = detect::render_model_input(frame, ModelInputMapping(rect, print.crop, Fit::Stretch)).pixels;
  return print;
}

bool same_content(const Fingerprint& now, const Fingerprint& before) {
  return !now.pixels.empty() && now.crop == before.crop && now.pixels == before.pixels;
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
  if (now < entry.read_at || now - entry.read_at > config_.max_age || !same_content(print, entry.print)) {
    return std::nullopt;
  }
  return entry.recognition;
}

void RecognitionCache::store(TrackId track, Fingerprint print, Recognition recognition, Timestamp now) {
  erase(track);  // the old entry is never kept, even when the new one is not cached
  if (print.pixels.empty() || print.pixels.size() > config_.max_bytes) {
    return;
  }
  bytes_ += print.pixels.size();
  entries_.emplace(track, Entry{std::move(print), std::move(recognition), now});
  while (entries_.size() > config_.capacity || bytes_ > config_.max_bytes) {
    drop_oldest();
  }
}

void RecognitionCache::drop_oldest() {
  const auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
    return a.second.read_at < b.second.read_at;
  });
  bytes_ -= oldest->second.print.pixels.size();
  entries_.erase(oldest);
}

void RecognitionCache::erase(TrackId track) {
  const auto it = entries_.find(track);
  if (it != entries_.end()) {
    bytes_ -= it->second.print.pixels.size();
    entries_.erase(it);
  }
}

void RecognitionCache::prune(Timestamp now) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    const Timestamp read_at = it->second.read_at;
    if (now < read_at || now - read_at > config_.max_age) {
      bytes_ -= it->second.print.pixels.size();
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
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
