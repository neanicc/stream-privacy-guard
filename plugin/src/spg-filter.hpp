#pragma once

// The "Privacy Guard" video filter: an OBS source of type FILTER.
//
// Shell (SPG-13): there is no detection pipeline yet, so the filter fails
// closed. It never draws the unscanned source; it covers the whole source with
// an opaque fill (the plan's "solid fill" fallback). SPG-14 replaces this with
// readback, the lookahead buffer and the blur effect.

struct obs_source_info;

namespace spg::plugin {

// Source type id. Saved in scene collections, so it must never change.
inline constexpr const char *kFilterId = "spg_privacy_guard_filter";

// Settings keys (stub values until SPG-10/14 consume them).
inline constexpr const char *kStatusKey = "spg_status";
inline constexpr const char *kLookaheadKey = "lookahead_ms";
inline constexpr const char *kHoldKey = "hold_ms";

// Plan section 4 defaults.
inline constexpr long long kDefaultLookaheadMs = 500;
inline constexpr long long kDefaultHoldMs = 750;

// Registration record for obs_register_source; valid for the module lifetime.
[[nodiscard]] const obs_source_info &filter_info() noexcept;

}  // namespace spg::plugin
