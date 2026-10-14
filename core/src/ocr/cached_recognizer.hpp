#pragma once

// A content-keyed recognition cache in front of a scan-stage recognizer
// (SPG-16e, lane:vision). Plan section 6: recognition runs on new or changed
// text only.
//
// run_scan() has no track ids, so the cache is keyed by what a region shows:
// its readback crop (ocr::fingerprint(), the pixels the recognizer reads).
// A region is answered from the cache only when an entry holds exactly the
// same crop size and every byte of the same decoded pixels, read no more than
// RecognitionCacheConfig::max_age before the frame and not after it. Anything
// else is read again in this cycle:
//   - text that changed in place (a character typed into a field), however
//     small the change: its pixels differ;
//   - text that moved and shows any other pixel than before (a scrolling line
//     over a gradient or a translucent panel): its pixels differ;
//   - text that moved with exactly its pixels (an opaque panel scrolling by
//     whole pixels) is the same crop and is answered from the cache, which is
//     what reading it again returns.
// The recognizer reads only a region's crop, one crop per model run, so a hit
// returns what a read would (RecognitionCache, "the recognizer reads only the
// crop's pixels"). A hash collision is caught by the pixel comparison and
// costs a read. Misses cost model runs, never recall.
//
// Memory and secrecy are RecognitionCache's: recognized text only in Secret,
// crop pixels zeroed whenever an entry or an unkept fingerprint is let go,
// bounded entries and bytes, memory only, nothing logged. The fingerprints a
// call takes before it looks up share those bounds: a crop over
// max_entry_bytes, or past max_bytes summed over the call's regions, gets no
// fingerprint and is simply read. Entries past max_age are dropped on the
// next recognize() call (or when the cache is destroyed), not on a timer: a
// cycle with no text region makes no call, so what was cached stays in
// memory (as Secret and zeroed pixels on release) until text is seen again
// or the stages are rebuilt.
//
// Thread-safe: workers share one instance. The cache is locked only to look
// up and to store, never while the inner recognizer runs.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "ocr/recognition_cache.hpp"
#include "spg/detection.hpp"
#include "spg/frame.hpp"
#include "spg/geometry.hpp"
#include "spg/scan.hpp"
#include "spg/secret.hpp"

namespace spg::ocr {

// The key a crop's content is filed under: a hash of its size and pixels.
// Equal crops always share a key; crops that share a key are still compared
// pixel for pixel before a hit.
[[nodiscard]] TrackId content_key(const Fingerprint& print) noexcept;

// Regions answered from the cache and regions read, since construction.
struct CacheTotals {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
};

class CachedRecognizer final : public spg::TextRecognizer {
 public:
  // Throws ContractError for a null `inner`, and like validate(config).
  explicit CachedRecognizer(std::shared_ptr<const spg::TextRecognizer> inner,
                            const RecognitionCacheConfig& config = {});

  // One Secret per region, in order, at frame time readback.timestamp: the
  // regions the cache can answer reuse their text, the rest go to the inner
  // recognizer in one call, and only after it succeeds is the cache updated.
  // Throws what the inner recognizer throws (the cache is then unchanged, so
  // the cycle fails and the next one reads the regions again),
  // ContractError for an invalid frame or region box, and DecodeError when
  // the inner recognizer returns a different count than it was given.
  [[nodiscard]] std::vector<Secret> recognize(const FrameView& readback, Size source,
                                              std::span<const Detection> regions) const override;

  [[nodiscard]] CacheTotals totals() const noexcept;

 private:
  std::shared_ptr<const spg::TextRecognizer> inner_;
  mutable std::mutex mutex_;
  mutable RecognitionCache cache_;
  mutable std::atomic<std::uint64_t> hits_{0};
  mutable std::atomic<std::uint64_t> misses_{0};
};

}  // namespace spg::ocr
