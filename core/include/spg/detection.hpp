#pragma once

#include <cstdint>

#include "spg/category.hpp"
#include "spg/geometry.hpp"
#include "spg/secret.hpp"
#include "spg/time.hpp"

namespace spg {

enum class DetectionKind : std::uint8_t { Face, TextRegion };

// Which detector produced a detection. Synthetic is for tests and simulations.
enum class DetectorId : std::uint8_t { Synthetic, YuNet, PpOcrDet };

// One detector output, already converted to Source space.
struct Detection {
  Box box;  // Source space
  DetectionKind kind = DetectionKind::Face;
  float score = 0.0F;  // [0, 1]
  DetectorId detector = DetectorId::Synthetic;
};

// Throws ContractError unless the box is valid and in Source space and the
// score is in [0, 1].
void validate(const Detection& detection);

// Policy decision for one piece of text. Holds reason codes only, never the
// matched text or the watch-list entry it matched.
struct Verdict {
  CategorySet reasons;
  [[nodiscard]] bool sensitive() const noexcept { return !reasons.empty(); }
  friend bool operator==(const Verdict&, const Verdict&) = default;
};

// A recognized text region. `text` lives in memory only.
struct TextHit {
  Detection detection;  // kind TextRegion
  Secret text;
  Verdict verdict;
};

using TrackId = std::uint64_t;

// Box motion in Source pixels per second.
struct Velocity {
  float dx_per_s = 0.0F;
  float dy_per_s = 0.0F;
  friend bool operator==(const Velocity&, const Velocity&) = default;
};

// A detection followed across frames. The mask for a track stays on until at
// least `hold_until`; tracking loss may extend it, never shorten it.
struct Track {
  TrackId id = 0;
  Box box;  // Source space
  Velocity velocity;
  Timestamp last_confirmed{};
  Timestamp hold_until{};
  DetectionKind kind = DetectionKind::Face;
};

}  // namespace spg
