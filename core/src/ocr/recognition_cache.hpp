#pragma once

// Per-track recognition cache (SPG-07, lane:vision). Plan section 6:
// recognition runs on new or changed text tracks only.
//
// The cache stores recognized text, never a verdict: the policy evaluates the
// text again on every cycle. A cached text is reused for a track only when
// all of these hold, and the region is read again otherwise:
//   - the track has an entry, stored after a recognize() call that succeeded;
//   - the frame time is not before the read and at most max_age after it;
//   - the region's readback crop has exactly the same width and height;
//   - every byte of the crop's decoded BGR pixels is identical.
// The recognizer reads only the crop's pixels, so a hit returns what reading
// the region again would return (with max_batch 1; a larger batch only pads).
// Position may differ: the same pixels moved with their box (scrolling) are
// the same text. Any other change, however small (one pixel, a colour change
// with the same brightness, a resize of the source or of the box, a track id
// reused for other content), is a miss. Misses cost model runs, never recall.
//
// The cache holds recognized text (Secret) and crop pixels in memory only,
// bounded by `capacity` entries and `max_bytes` of pixels, and is not
// synchronized: one worker owns it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "ocr/text_recognizer.hpp"
#include "spg/detection.hpp"
#include "spg/frame.hpp"
#include "spg/geometry.hpp"
#include "spg/time.hpp"

namespace spg::ocr {

struct RecognitionCacheConfig {
  // A cached text is reused for at most this long (frame time) after it was
  // read, however still the region looks.
  Duration max_age = std::chrono::milliseconds{1000};
  // Tracks kept; the oldest read is dropped first beyond this.
  std::size_t capacity = 1024;
  // Crop pixels kept over all entries; the oldest read is dropped first
  // beyond this. A crop larger than the budget is not cached.
  std::size_t max_bytes = std::size_t{64} << 20;
};

// Throws ContractError naming the field when a value is out of range.
void validate(const RecognitionCacheConfig& config);

// What a region looked like when it was read: its whole readback crop.
struct Fingerprint {
  Size crop;  // readback crop size in pixels; zero when outside the frame
  std::vector<std::uint8_t> pixels;  // packed BGR, crop.width * crop.height * 3
  friend bool operator==(const Fingerprint&, const Fingerprint&) = default;
};

// The fingerprint of a Source-space region of `frame` (Readback pixels of a
// source of size `source`): the same readback crop the recognizer reads
// (readback_crop()), decoded to BGR at scale 1. Throws ContractError for an
// invalid frame or a box that is not a valid Source-space box.
[[nodiscard]] Fingerprint fingerprint(const FrameView& frame, Size source, const Box& region);

// Whether `now` and `before` are the same non-empty crop, pixel for pixel.
[[nodiscard]] bool same_content(const Fingerprint& now, const Fingerprint& before);

class RecognitionCache {
 public:
  explicit RecognitionCache(const RecognitionCacheConfig& config = {});

  // The text read for `track`, when it was read no more than max_age before
  // `now` (and not after it) from exactly the content `print` holds; nothing
  // otherwise.
  [[nodiscard]] std::optional<Recognition> find(TrackId track, const Fingerprint& print, Timestamp now) const;
  // Records what was read for `track` at frame time `now`.
  void store(TrackId track, Fingerprint print, Recognition recognition, Timestamp now);
  // Forgets a track (it ended).
  void erase(TrackId track);
  // Drops every entry find() could no longer return at `now`.
  void prune(Timestamp now);

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] const RecognitionCacheConfig& config() const noexcept { return config_; }

 private:
  struct Entry {
    Fingerprint print;
    Recognition recognition;
    Timestamp read_at{};
  };
  void drop_oldest();

  RecognitionCacheConfig config_;
  std::unordered_map<TrackId, Entry> entries_;
  std::size_t bytes_ = 0;
};

// A text region and the track it belongs to.
struct TrackedRegion {
  TrackId track = 0;
  Detection detection;  // Source space
};

// Counts from one recognize_tracks() call.
struct CacheStats {
  std::size_t hits = 0;
  std::size_t misses = 0;
};

// Recognizes `regions` of `frame` at frame time frame.timestamp: tracks the
// cache can answer reuse their text, the rest go to `recognizer` in one
// batched recognize() call, and the cache is updated only after it succeeds.
// Returns one TextHit per region, in order (verdict empty, for the policy).
// Throws like TextRecognizer::recognize(); the cache is unchanged then, so the
// frame fails closed and the next cycle reads the regions again.
[[nodiscard]] std::vector<TextHit> recognize_tracks(const TextRecognizer& recognizer, RecognitionCache& cache,
                                                    const FrameView& frame, Size source,
                                                    std::span<const TrackedRegion> regions,
                                                    CacheStats* stats = nullptr);

}  // namespace spg::ocr
