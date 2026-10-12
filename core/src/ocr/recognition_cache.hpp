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
// Bounds: at most one entry per track (a store replaces it), at most
// `capacity` tracks, at most `max_entry_bytes` of pixels per entry and
// `max_bytes` over all entries. A store first drops every entry past max_age,
// then the least recently used entries until the bounds hold. A crop over a
// per-entry bound is not cached. A full cache only makes misses: every miss
// is read again, nothing is skipped.
//
// The cache holds recognized text (Secret) and crop pixels in memory only and
// is not synchronized: one worker owns it.

#include <cstddef>
#include <cstdint>
#include <list>
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
  // Tracks kept (one entry each); the least recently used goes first.
  std::size_t capacity = 1024;
  // Crop pixels kept for one entry; a larger crop is not cached (a 1920 x 700
  // readback crop is about 4 MiB).
  std::size_t max_entry_bytes = std::size_t{4} << 20;
  // Crop pixels kept over all entries; the least recently used goes first.
  std::size_t max_bytes = std::size_t{64} << 20;
};

// Throws ContractError naming the field when a value is out of range
// (negative max_age, or a zero capacity or byte bound).
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
  // otherwise. A hit makes the entry the most recently used.
  [[nodiscard]] std::optional<Recognition> find(TrackId track, const Fingerprint& print, Timestamp now);
  // Records what was read for `track` at frame time `now`, replacing the
  // track's entry. Drops expired entries, then least recently used ones until
  // the bounds hold. A crop that is empty or over a byte bound is not stored
  // (the track's old entry is still dropped).
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
    std::list<TrackId>::iterator use;  // position in lru_
  };
  [[nodiscard]] bool expired(const Entry& entry, Timestamp now) const;
  void drop(std::unordered_map<TrackId, Entry>::iterator it);

  RecognitionCacheConfig config_;
  std::unordered_map<TrackId, Entry> entries_;
  std::list<TrackId> lru_;  // most recently used first
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
