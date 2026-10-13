#include "ocr/cached_recognizer.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ocr/ctc_decode.hpp"
#include "spg/error.hpp"

namespace spg::ocr {

TrackId content_key(const Fingerprint& print) noexcept {
  const std::string_view bytes(reinterpret_cast<const char*>(print.pixels.data()), print.pixels.size());
  std::uint64_t key = std::hash<std::string_view>{}(bytes);
  // Mix in the crop size: the same bytes in another shape are other content.
  const std::uint64_t size = (std::uint64_t{print.crop.width} << 32) | print.crop.height;
  key ^= size + 0x9e3779b97f4a7c15ULL + (key << 6) + (key >> 2);
  return key;
}

CachedRecognizer::CachedRecognizer(std::shared_ptr<const spg::TextRecognizer> inner,
                                   const RecognitionCacheConfig& config)
    : inner_(std::move(inner)), cache_(config) {
  if (inner_ == nullptr) {
    throw ContractError("cached recognizer: needs a recognizer");
  }
}

std::vector<Secret> CachedRecognizer::recognize(const FrameView& readback, Size source,
                                                std::span<const Detection> regions) const {
  const Timestamp now = readback.timestamp;
  std::vector<Fingerprint> prints;
  std::vector<TrackId> keys;
  prints.reserve(regions.size());
  keys.reserve(regions.size());
  for (const Detection& region : regions) {
    validate(region);
    prints.push_back(fingerprint(readback, source, region.box));
    keys.push_back(content_key(prints.back()));
  }

  std::vector<std::optional<Recognition>> found(regions.size());
  std::vector<std::size_t> missing;
  {
    const std::lock_guard lock(mutex_);
    cache_.prune(now);  // nothing past max_age stays in memory while hits keep coming
    for (std::size_t i = 0; i < regions.size(); ++i) {
      found[i] = cache_.find(keys[i], prints[i], now);
      if (!found[i]) {
        missing.push_back(i);
      }
    }
  }

  std::vector<Secret> read;
  if (!missing.empty()) {
    std::vector<Detection> to_read;
    to_read.reserve(missing.size());
    for (const std::size_t i : missing) {
      to_read.push_back(regions[i]);
    }
    read = inner_->recognize(readback, source, to_read);  // throws before any store
    if (read.size() != to_read.size()) {
      throw DecodeError("cached recognizer: the recognizer returned " + std::to_string(read.size()) +
                        " texts for " + std::to_string(to_read.size()) + " regions");
    }
    const std::lock_guard lock(mutex_);
    for (std::size_t k = 0; k < missing.size(); ++k) {
      const std::size_t i = missing[k];
      Recognition entry;
      entry.text = read[k];
      cache_.store(keys[i], std::move(prints[i]), std::move(entry), now);
    }
  }

  std::vector<Secret> out(regions.size());
  for (std::size_t i = 0, k = 0; i < regions.size(); ++i) {
    out[i] = found[i] ? std::move(found[i]->text) : std::move(read[k++]);
  }
  hits_.fetch_add(regions.size() - missing.size(), std::memory_order_relaxed);
  misses_.fetch_add(missing.size(), std::memory_order_relaxed);
  return out;
}

CacheTotals CachedRecognizer::totals() const noexcept {
  return CacheTotals{hits_.load(std::memory_order_relaxed), misses_.load(std::memory_order_relaxed)};
}

}  // namespace spg::ocr
