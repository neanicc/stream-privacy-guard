#pragma once

// Per-track recognition cache (SPG-07, lane:vision). Plan section 6:
// recognition runs on new or changed text tracks only. A tracked text region
// is read again when it is new, when its box changed size, when its pixels
// changed, or when its cached text is older than max_age; otherwise the text
// read before is reused.
//
// "Changed pixels" compares a small luma thumbnail of the region (six cells
// across the line height, square cells) cell by cell, and any cell that moved
// by more than content_tolerance is a change. The readback is the source as
// rendered, before the encoder, so a still region repeats exactly and a
// changed character moves several cells by far more than that: a new digit
// in a chat line is read again on the next cycle. When unsure, the cache misses (recall over
// precision): a miss costs a model run, a stale hit can let a secret air.
//
// The cache holds recognized text (Secret) in memory only and is not
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
  // The readback crop may change width and height by at most this fraction
  // (and the thumbnail grid must match) for a hit.
  float size_tolerance = 0.1F;
  // Largest luma change of any thumbnail cell (0-255) still counted as the
  // same content.
  float content_tolerance = 6.0F;
  // Tracks kept; the oldest read is dropped first beyond this.
  std::size_t capacity = 1024;
};

// Throws ContractError naming the field when a value is out of range.
void validate(const RecognitionCacheConfig& config);

// What a region looked like when it was read.
struct Fingerprint {
  Size crop;  // readback crop size in pixels; zero when outside the frame
  Size grid;  // thumbnail size in cells
  std::vector<std::uint8_t> luma;  // grid.width * grid.height, row-major
  friend bool operator==(const Fingerprint&, const Fingerprint&) = default;
};

// The fingerprint of a Source-space region of `frame` (Readback pixels of a
// source of size `source`): its readback crop size and a luma thumbnail of
// square cells, six across the crop's shorter side (each side 1 to 256
// cells). Throws ContractError for an invalid frame or a box that is not a
// valid Source-space box.
[[nodiscard]] Fingerprint fingerprint(const FrameView& frame, Size source, const Box& region);

// Whether `now` and `before` show the same content under `config`.
[[nodiscard]] bool same_content(const Fingerprint& now, const Fingerprint& before,
                                const RecognitionCacheConfig& config);

class RecognitionCache {
 public:
  explicit RecognitionCache(const RecognitionCacheConfig& config = {});

  // The text read for `track` when it was read no more than max_age before
  // `now` (and not after it) from content `print` still matches.
  [[nodiscard]] std::optional<Recognition> find(TrackId track, const Fingerprint& print, Timestamp now) const;
  // Records what was read for `track` at frame time `now`.
  void store(TrackId track, Fingerprint print, Recognition recognition, Timestamp now);
  // Forgets a track (it ended).
  void erase(TrackId track);
  // Drops every entry find() could no longer return at `now`.
  void prune(Timestamp now);

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] const RecognitionCacheConfig& config() const noexcept { return config_; }

 private:
  struct Entry {
    Fingerprint print;
    Recognition recognition;
    Timestamp read_at{};
  };
  RecognitionCacheConfig config_;
  std::unordered_map<TrackId, Entry> entries_;
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
