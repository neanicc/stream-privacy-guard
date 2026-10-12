#pragma once

// The "Privacy Guard" video filter: an OBS source of type FILTER.
//
// Render (SPG-14): each frame is captured into a lookahead ring of textures,
// read back downscaled for the scheduler's workers, and drawn once it is due,
// with its masks blurred, or covered whole when it was not fully scanned
// (render/filter_render.hpp, docs/plugin-render.md). It never draws an
// unscanned frame uncovered. Detection models are not loaded by this build
// yet, so every frame gets the fallback.
//
// Settings (SPG-15): the properties of plan section 7. Watch-list entries are
// never stored in the filter settings (which OBS saves in plain text in the
// scene collection); see settings/watchlist_store.hpp.

#include "settings/filter_settings.hpp"

struct obs_source_info;

namespace spg::plugin {

// Source type id. Saved in scene collections, so it must never change.
inline constexpr const char *kFilterId = "spg_privacy_guard_filter";

// Settings keys. Saved in scene collections, so they must never change.
inline constexpr const char *kStatusKey = "spg_status";
inline constexpr const char *kIssueKey = "spg_issue";
inline constexpr const char *kRenderStatusKey = "spg_render_status";
inline constexpr const char *kAudioKey = "spg_audio";
inline constexpr const char *kLookaheadKey = "lookahead_ms";
inline constexpr const char *kHoldKey = "hold_ms";
inline constexpr const char *kSensitivityKey = "sensitivity";
inline constexpr const char *kFallbackKey = "fallback";
inline constexpr const char *kDebugOverlayKey = "debug_overlay";
// Category checkboxes use category_key() ("category_face", ...).

// Watch-list. Only the reference and the entry count are saved; the entry
// typed into kWatchlistNewEntryKey (a password field) is moved into the
// watch-list file by the Add button and erased from the settings before OBS
// saves them.
inline constexpr const char *kWatchlistRefKey = "watchlist_ref";
inline constexpr const char *kWatchlistCountKey = "watchlist_count";
inline constexpr const char *kWatchlistInfoKey = "watchlist_info";
inline constexpr const char *kWatchlistNewEntryKey = "watchlist_new_entry";
inline constexpr const char *kWatchlistAddKey = "watchlist_add";
inline constexpr const char *kWatchlistSelectedKey = "watchlist_selected";
inline constexpr const char *kWatchlistRemoveKey = "watchlist_remove";
inline constexpr const char *kWatchlistClearKey = "watchlist_clear";

// Directory under the module config path that holds the watch-list files.
inline constexpr const char *kWatchlistDirName = "watchlists";

// Registration record for obs_register_source; valid for the module lifetime.
[[nodiscard]] const obs_source_info &filter_info() noexcept;

}  // namespace spg::plugin
