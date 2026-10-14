#include "ocr/recognition_cache.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

#include "detect/frame_image.hpp"
#include "spg/error.hpp"
#include "spg/secure_memory.hpp"

namespace spg::ocr {

namespace {

// Zeroes every byte `pixels` owns, its unused capacity included, and leaves
// them all in it (size() == capacity()).
void zero_all(std::vector<std::uint8_t>& pixels) noexcept {
  pixels.resize(pixels.capacity());  // within capacity: never reallocates, never throws
  secure_wipe(pixels.data(), pixels.size());
}

}  // namespace

Fingerprint& Fingerprint::operator=(const Fingerprint& other) {
  if (this != &other) {
    wipe();
    crop = other.crop;
    pixels = other.pixels;  // reuses the zeroed buffer, or frees it for a larger one
  }
  return *this;
}

Fingerprint& Fingerprint::operator=(Fingerprint&& other) noexcept {
  if (this != &other) {
    wipe();
    crop = std::exchange(other.crop, Size{});
    pixels = std::move(other.pixels);  // frees the zeroed buffer
    other.pixels.clear();
  }
  return *this;
}

Fingerprint::~Fingerprint() {
  zero_all(pixels);
}

void Fingerprint::wipe() noexcept {
  zero_all(pixels);
  pixels.clear();
  crop = {};
}

void validate(const RecognitionCacheConfig& config) {
  if (config.max_age < Duration::zero()) {
    throw ContractError("recognition cache: max_age must not be negative");
  }
  if (config.capacity < 1) {
    throw ContractError("recognition cache: capacity must be at least 1");
  }
  if (config.max_entry_bytes < 1) {
    throw ContractError("recognition cache: max_entry_bytes must be at least 1");
  }
  if (config.max_bytes < 1) {
    throw ContractError("recognition cache: max_bytes must be at least 1");
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

RecognitionCache::~RecognitionCache() {
  for (auto& [track, entry] : entries_) {
    release(entry.print);
  }
}

void RecognitionCache::release(Fingerprint& print) noexcept {
  zero_all(print.pixels);
  if (observer_ && !print.pixels.empty()) {
    try {
      observer_(print.pixels.data(), print.pixels.size());
    } catch (...) {
      // A test hook must not break cleanup (release() is noexcept).
    }
  }
  print.pixels.clear();
  print.crop = {};
}

bool RecognitionCache::expired(const Entry& entry, Timestamp now) const {
  return now < entry.read_at || now - entry.read_at > config_.max_age;
}

void RecognitionCache::drop(std::unordered_map<TrackId, Entry>::iterator it) {
  bytes_ -= it->second.print.pixels.size();
  release(it->second.print);
  lru_.erase(it->second.use);
  entries_.erase(it);
}

std::optional<Recognition> RecognitionCache::find(TrackId track, const Fingerprint& print, Timestamp now) {
  const auto it = entries_.find(track);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  Entry& entry = it->second;
  if (expired(entry, now) || !same_content(print, entry.print)) {
    return std::nullopt;
  }
  lru_.splice(lru_.begin(), lru_, entry.use);
  return entry.recognition;
}

void RecognitionCache::store(TrackId track, Fingerprint print, Recognition recognition, Timestamp now) {
  erase(track);  // the old entry is never kept, even when the new one is not cached
  prune(now);
  const std::size_t size = print.pixels.size();
  if (size == 0 || size > config_.max_entry_bytes || size > config_.max_bytes) {
    release(print);
    return;
  }
  while (!entries_.empty() && (entries_.size() >= config_.capacity || bytes_ + size > config_.max_bytes)) {
    drop(entries_.find(lru_.back()));
  }
  // Each step either completes or leaves the cache as it was, so a failed
  // allocation never leaves a key in lru_ without an entry (which a later
  // eviction would look up) or bytes_ counting pixels nobody holds. A print
  // that is never stored is zeroed by its destructor.
  // erase(track) above means this always inserts.
  const auto it = entries_.emplace(track, Entry{std::move(print), std::move(recognition), now, {}}).first;
  try {
    lru_.push_front(track);
  } catch (...) {
    release(it->second.print);
    entries_.erase(it);
    throw;
  }
  it->second.use = lru_.begin();
  bytes_ += size;
}

void RecognitionCache::erase(TrackId track) {
  const auto it = entries_.find(track);
  if (it != entries_.end()) {
    drop(it);
  }
}

void RecognitionCache::prune(Timestamp now) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    const auto next = std::next(it);
    if (expired(it->second, now)) {
      drop(it);
    }
    it = next;
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
    cache.store(regions[i].track, std::move(prints[i]), read[k], now);
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
