#include "spg-filter.hpp"

#include <graphics/vec4.h>
#include <obs-module.h>

#include "plugin-support.hpp"
#include "render/filter_render.hpp"
#include "settings/watchlist_store.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace spg::plugin {
namespace {

// The outcome of the last watch-list button, shown in the properties. Fixed
// texts only: a notice never quotes an entry.
enum class Notice : std::uint8_t {
  None,
  Added,
  Removed,
  Cleared,
  EntryEmpty,
  EntryTooLong,
  EntryNoLettersOrDigits,
  EntryDuplicate,
  ListFull,
  StorageBroken,  // edits refused until the user removes all entries
  SaveFailed,
};

struct FilterState {
  obs_source_t *context = nullptr;
  std::filesystem::path watchlist_dir;  // fixed at create
  // Readback, lookahead ring, scheduler and blur (SPG-14). Set at create;
  // null only if that failed, and then every frame gets an opaque fill.
  std::unique_ptr<FilterRender> render;

  std::mutex mutex;  // guards everything below
  FilterSettings settings;
  std::string ref;
  StoreError store_error = StoreError::NoConfigDir;
  std::vector<Secret> watchlist;
  // Set only when detection could run with these settings; handed to
  // `render`, which covers the whole source while it is empty.
  std::optional<PolicyConfig> policy;
  FilterIssue issue = FilterIssue::WatchlistStorage;
  // True once `policy` and `issue` match the fields above; an update that
  // changes none of them skips the rebuild.
  bool policy_current = false;
  Notice notice = Notice::None;
  // What the user has typed into the masked entry field. The field's modified
  // callback moves each keystroke's text here and out of the settings, so the
  // OBS undo stack, which snapshots the settings, never holds it.
  Secret pending_entry;
};

const char *text(const char *key) {
  return obs_module_text(key);
}

const char *issue_name(FilterIssue issue) {
  switch (issue) {
    case FilterIssue::None:
      return "none";
    case FilterIssue::NoCategories:
      return "every category is off";
    case FilterIssue::WatchlistStorage:
      return "watch-list storage error";
    case FilterIssue::InvalidConfig:
      return "settings rejected by spg-core";
  }
  return "unknown";
}

std::filesystem::path watchlist_directory() {
  char *path = obs_module_config_path(kWatchlistDirName);
  std::filesystem::path out = path != nullptr ? path_from_utf8(path) : std::filesystem::path{};
  bfree(path);
  return out;  // a relative path (no OBS config directory) is refused by the store
}

// Writes `value` only when it differs, so defaults stay defaults.
void set_int_if_changed(obs_data_t *settings, const char *key, long long value) {
  if (obs_data_get_int(settings, key) != value) {
    obs_data_set_int(settings, key, value);
  }
}

void set_string_if_changed(obs_data_t *settings, const char *key, std::string_view value) {
  if (std::string_view{obs_data_get_string(settings, key)} != value) {
    obs_data_set_string(settings, key, std::string(value).c_str());
  }
}

// Reads the settings, clamps them and writes the clamped values back, so a
// hand-edited or older scene file is corrected and the properties show what
// is in effect.
FilterSettings read_settings(obs_data_t *settings) {
  RawFilterSettings raw;
  raw.lookahead_ms = obs_data_get_int(settings, kLookaheadKey);
  raw.hold_ms = obs_data_get_int(settings, kHoldKey);
  raw.categories = CategorySet{};
  for (const Category category : kAllCategories) {
    if (obs_data_get_bool(settings, category_key(category).c_str())) {
      raw.categories.insert(category);
    }
  }
  raw.sensitivity = obs_data_get_string(settings, kSensitivityKey);
  raw.fallback = obs_data_get_string(settings, kFallbackKey);
  raw.debug_overlay = obs_data_get_bool(settings, kDebugOverlayKey);

  const FilterSettings clean = sanitize(raw);
  set_int_if_changed(settings, kLookaheadKey, clean.lookahead_ms);
  set_int_if_changed(settings, kHoldKey, clean.hold_ms);
  set_string_if_changed(settings, kSensitivityKey, to_string(clean.sensitivity));
  set_string_if_changed(settings, kFallbackKey, spg::to_string(clean.fallback));
  return clean;
}

// Moves the masked entry field out of the settings: copies it into a Secret,
// overwrites the stored string in place (libobs reuses an item's storage for a
// value of the same length, so the typed bytes do not linger in the freed
// item) and erases the key.
Secret take_masked_entry(obs_data_t *settings) noexcept {
  if (!obs_data_has_user_value(settings, kWatchlistNewEntryKey)) {
    return {};
  }
  const char *typed = obs_data_get_string(settings, kWatchlistNewEntryKey);
  Secret entry;
  try {
    entry = Secret::copy_of(typed);
    if (const std::size_t length = std::strlen(typed); length > 0) {
      obs_data_set_string(settings, kWatchlistNewEntryKey, std::string(length, '*').c_str());
    }
  } catch (...) {
    // Allocation failure: the field is still erased below.
  }
  obs_data_erase(settings, kWatchlistNewEntryKey);
  return entry;
}

void log_store_error(StoreError error) {
  const std::string_view what = describe(error);
  log(LOG_WARNING, "watch-list: %.*s", static_cast<int>(what.size()), what.data());
}

// A watch-list as last read or written for one reference. Copied out of the
// state so that file I/O never runs while the mutex is held: OBS runs a video
// filter's update on the video thread (deferred to the next tick), and the
// watch-list buttons write and sync the file on the UI thread.
struct WatchlistSnapshot {
  std::string ref;
  StoreError error = StoreError::None;
  std::vector<Secret> entries;
};

WatchlistSnapshot snapshot(FilterState &state) {
  const std::lock_guard lock{state.mutex};
  return {state.ref, state.store_error, state.watchlist};
}

// Hands the current policy (or none) to the render side. Caller holds
// state.mutex; the render thread never takes it.
void publish_render_locked(FilterState &state) noexcept {
  if (state.render != nullptr) {
    state.render->set_debug_overlay(state.settings.debug_overlay);
    state.render->publish(state.policy, state.settings.fallback);
  }
}

// Caller holds state.mutex.
void rebuild_policy_locked(FilterState &state) {
  state.policy_current = false;
  const FilterIssue before = state.issue;
  if (state.store_error != StoreError::None) {
    state.policy.reset();
    state.issue = FilterIssue::WatchlistStorage;
  } else {
    state.issue = make_policy_config(state.settings, state.watchlist, state.policy);
  }
  if (state.issue != before && state.issue != FilterIssue::None) {
    log(LOG_WARNING, "detection off, covering the whole source: %s", issue_name(state.issue));
  }
  state.policy_current = true;
  publish_render_locked(state);
}

WatchlistRefRegistry &ref_registry() {
  static WatchlistRefRegistry registry;
  return registry;
}

// The watch-list reference in the settings, created for a new filter.
std::string settings_ref(obs_data_t *settings) {
  std::string ref = obs_data_get_string(settings, kWatchlistRefKey);
  if (ref.empty()) {
    try {
      ref = new_watchlist_ref();
    } catch (...) {
      ref.clear();  // stays invalid: BadReference, fail closed
    }
    obs_data_set_string(settings, kWatchlistRefKey, ref.c_str());
  }
  return ref;
}

// Writes `next` and, when that succeeds, makes it the filter's watch-list.
// `replace_ref` installs it even under a new reference (Remove all entries)
// and releases the old one. Returns false when the write failed, or when an
// update switched the filter to another watch-list in the meantime (the list
// written is then not this filter's, so nothing is installed).
bool save_watchlist(FilterState &state, obs_data_t *settings, WatchlistSnapshot next, bool replace_ref) {
  const StoreError error = WatchlistStore{state.watchlist_dir, next.ref}.save(next.entries);
  if (error != StoreError::None) {
    log_store_error(error);
    return false;
  }
  const std::size_t count = next.entries.size();
  {
    const std::lock_guard lock{state.mutex};
    if (!replace_ref && state.ref != next.ref) {
      return false;
    }
    if (state.ref != next.ref) {
      ref_registry().release(state.ref);
    }
    state.ref = std::move(next.ref);
    state.watchlist = std::move(next.entries);
    state.store_error = StoreError::None;
  }
  obs_data_set_int(settings, kWatchlistCountKey, static_cast<long long>(count));
  log(LOG_INFO, "watch-list saved: %zu entries", count);
  return true;
}

Notice notice_for(EntryError error) {
  switch (error) {
    case EntryError::None:
      return Notice::None;
    case EntryError::Empty:
      return Notice::EntryEmpty;
    case EntryError::TooLong:
      return Notice::EntryTooLong;
    case EntryError::NoLettersOrDigits:
      return Notice::EntryNoLettersOrDigits;
    case EntryError::Duplicate:
      return Notice::EntryDuplicate;
    case EntryError::ListFull:
      return Notice::ListFull;
  }
  return Notice::None;
}

const char *filter_get_name(void * /*type_data*/) {
  return text("SPG.FilterName");
}

// Reads the watch-list for `ref` and claims `ref` for this filter. A reference
// another live filter holds means this filter is a copy: it gets a new
// reference and its own copy of the file (written in `ref`'s place in the
// settings). On return `ref` is claimed; after an exception nothing new is
// held. Runs without the state mutex.
WatchlistLoad load_or_fork(const FilterState &state, obs_data_t *settings, std::string &ref) {
  const long long expected = obs_data_get_int(settings, kWatchlistCountKey);
  WatchlistLoad loaded = WatchlistStore{state.watchlist_dir, ref}.load(expected);
  if (ref_registry().claim(ref)) {
    return loaded;
  }
  std::string fork = new_watchlist_ref();
  static_cast<void>(ref_registry().claim(fork));  // fresh: never held
  try {
    if (loaded.error == StoreError::None) {
      loaded.error = WatchlistStore{state.watchlist_dir, fork}.save(loaded.entries);
      if (loaded.error != StoreError::None) {
        loaded.entries.clear();
      }
    }
    if (loaded.error != StoreError::None) {
      // A storage error carries over to the copy, which fails closed like its
      // original. The copy has no file of its own, so record one as expected:
      // a reload then reports it missing instead of reading the absent file as
      // a new, empty watch-list. Remove all entries is the way back.
      obs_data_set_int(settings, kWatchlistCountKey, std::max(expected, 1LL));
    }
    obs_data_set_string(settings, kWatchlistRefKey, fork.c_str());
    log(LOG_INFO, "copied filter: watch-list forked to its own file");
  } catch (...) {
    ref_registry().release(fork);
    throw;
  }
  ref = std::move(fork);
  return loaded;
}

void filter_update(void *data, obs_data_t *settings) {
  auto *state = static_cast<FilterState *>(data);
  std::string ref;
  bool claimed = false;  // `ref` is held for this filter but not installed yet
  try {
    const FilterSettings clean = read_settings(settings);
    ref = settings_ref(settings);
    bool reload = false;
    {
      const std::lock_guard lock{state->mutex};
      reload = ref.empty() || ref != state->ref;
    }
    // Only a new filter or a changed reference reads the file, outside the lock.
    std::optional<WatchlistLoad> loaded;
    if (reload) {
      loaded = load_or_fork(*state, settings, ref);
      claimed = true;
      if (loaded->error == StoreError::None) {
        set_int_if_changed(settings, kWatchlistCountKey, static_cast<long long>(loaded->entries.size()));
        log(LOG_INFO, "watch-list loaded: %zu entries", loaded->entries.size());
      } else {
        log_store_error(loaded->error);
      }
    }
    const std::lock_guard lock{state->mutex};
    if (loaded) {
      if (state->ref != ref) {
        ref_registry().release(state->ref);
      }
      claimed = false;
      state->ref = std::move(ref);
      state->store_error = loaded->error;
      state->watchlist = std::move(loaded->entries);
    } else if (state->policy_current && clean == state->settings) {
      return;  // same settings and watch-list: the policy is already built
    }
    state->settings = clean;
    rebuild_policy_locked(*state);
  } catch (...) {
    // Allocation failure: drop the policy so detection stays off (fail closed),
    // and give back a reference this update claimed but did not install.
    if (claimed) {
      ref_registry().release(ref);
    }
    const std::lock_guard lock{state->mutex};
    state->policy.reset();
    state->issue = FilterIssue::InvalidConfig;
    state->policy_current = false;
    publish_render_locked(*state);
  }
}

void *filter_create(obs_data_t *settings, obs_source_t *source) {
  // Called from C: never let an exception escape. OBS treats nullptr as a failed create.
  auto *state = new (std::nothrow) FilterState{};
  if (state == nullptr) {
    return nullptr;
  }
  state->context = source;
  try {
    state->watchlist_dir = watchlist_directory();
  } catch (...) {
    state->watchlist_dir.clear();  // NoConfigDir: fail closed
  }
  try {
    state->render = std::make_unique<FilterRender>(source);
  } catch (...) {
    state->render.reset();  // video_render draws an opaque fill
  }
  filter_update(state, settings);
  return state;
}

void filter_destroy(void *data) {
  auto *state = static_cast<FilterState *>(data);
  ref_registry().release(state->ref);
  delete state;
}

// OBS calls this just before it serializes the settings into the scene
// collection (and before the filters dialog snapshots a deleted filter for
// undo): an entry typed but never added must not be saved there. The
// properties view never leaves it in the settings; a script might.
void filter_save(void *data, obs_data_t *settings) {
  static_cast<void>(take_masked_entry(settings));
  auto *state = static_cast<FilterState *>(data);
  const std::lock_guard lock{state->mutex};
  if (state->store_error == StoreError::None) {
    set_int_if_changed(settings, kWatchlistCountKey, static_cast<long long>(state->watchlist.size()));
  }
}

void filter_get_defaults(obs_data_t *settings) {
  try {
    for (const Category category : kAllCategories) {
      obs_data_set_default_bool(settings, category_key(category).c_str(), true);
    }
  } catch (...) {
    // Allocation failure: a missing default reads as false, so that category
    // is off; the remaining defaults still apply.
  }
  obs_data_set_default_string(settings, kSensitivityKey, to_string(Sensitivity::Standard).data());
  obs_data_set_default_int(settings, kLookaheadKey, kDefaultLookaheadMs);
  obs_data_set_default_int(settings, kHoldKey, kDefaultHoldMs);
  obs_data_set_default_string(settings, kFallbackKey, spg::to_string(FallbackMode::BlurSource).data());
  obs_data_set_default_bool(settings, kDebugOverlayKey, false);
  obs_data_set_default_int(settings, kWatchlistCountKey, 0);
  obs_data_set_default_int(settings, kWatchlistSelectedKey, 0);
}

// --- Watch-list buttons (UI thread). Each returns true so OBS rebuilds the
// properties and the masked entry field shows empty again.

// Records the outcome of a button and re-derives the policy.
void finish_action(FilterState &state, Notice notice) {
  const std::lock_guard lock{state.mutex};
  state.notice = notice;
  rebuild_policy_locked(state);
}

// The masked field's modified callback, run by the properties view right after
// each keystroke is written to the settings. OBS 32's view records an undo
// step 500 ms after the last change, from the settings as they are then
// (shared/properties-view/properties-view.cpp, WidgetInfo::ControlChanged), so
// the text has left the settings before any snapshot is taken. libobs also
// calls it whenever the properties are built (obs_source_properties applies
// the settings), so a rebuilt view, whose masked field is empty, resets
// pending_entry: Add never adds text the field no longer shows, and a script
// that set the field before a build still gets it added. Returns false:
// rebuilding the properties would clear the field while the user types.
bool masked_entry_modified(void *data, obs_properties_t * /*props*/, obs_property_t * /*property*/,
                           obs_data_t *settings) {
  Secret typed = take_masked_entry(settings);
  if (auto *state = static_cast<FilterState *>(data); state != nullptr) {
    try {
      const std::lock_guard lock{state->mutex};
      state->pending_entry = std::move(typed);
    } catch (...) {
      // Called from C: the text is already out of the settings and is wiped
      // when `typed` goes out of scope.
    }
  }
  return false;
}

bool watchlist_add_clicked(obs_properties_t * /*props*/, obs_property_t * /*property*/, void *data) {
  auto *state = static_cast<FilterState *>(data);
  if (state == nullptr) {
    return false;
  }
  obs_data_t *settings = obs_source_get_settings(state->context);
  try {
    // The text is in pending_entry, or still in the settings when a script
    // set the field and pressed Add on properties built before that. A field
    // the settings hold as empty means "nothing to add", not "use what was
    // typed earlier".
    const bool in_settings = obs_data_has_user_value(settings, kWatchlistNewEntryKey);
    Secret entry = take_masked_entry(settings);
    {
      const std::lock_guard lock{state->mutex};
      if (!in_settings) {
        entry = std::move(state->pending_entry);
      }
      state->pending_entry = Secret{};
    }
    WatchlistSnapshot next = snapshot(*state);
    Notice notice = Notice::None;
    if (next.error != StoreError::None) {
      notice = Notice::StorageBroken;
    } else if (const EntryError error = check_new_entry(entry.reveal(), next.entries); error != EntryError::None) {
      notice = notice_for(error);
    } else {
      next.entries.push_back(std::move(entry));
      notice = save_watchlist(*state, settings, std::move(next), false) ? Notice::Added : Notice::SaveFailed;
    }
    finish_action(*state, notice);
  } catch (...) {
    static_cast<void>(take_masked_entry(settings));
  }
  obs_data_release(settings);
  return true;
}

bool watchlist_remove_clicked(obs_properties_t * /*props*/, obs_property_t * /*property*/, void *data) {
  auto *state = static_cast<FilterState *>(data);
  if (state == nullptr) {
    return false;
  }
  obs_data_t *settings = obs_source_get_settings(state->context);
  try {
    const long long index = obs_data_get_int(settings, kWatchlistSelectedKey);
    WatchlistSnapshot next = snapshot(*state);
    Notice notice = Notice::None;
    if (next.error != StoreError::None) {
      notice = Notice::StorageBroken;
    } else if (index >= 0 && static_cast<unsigned long long>(index) < next.entries.size()) {
      next.entries.erase(next.entries.begin() + static_cast<std::ptrdiff_t>(index));
      notice = save_watchlist(*state, settings, std::move(next), false) ? Notice::Removed : Notice::SaveFailed;
      obs_data_set_int(settings, kWatchlistSelectedKey, 0);
    }
    finish_action(*state, notice);
  } catch (...) {
    // Nothing changed in memory; the file is either untouched or replaced whole.
  }
  obs_data_release(settings);
  return true;
}

// Also the way out of a storage error: writes an empty watch-list over the
// old file, or under a new reference when the old one is malformed or its
// file cannot be replaced (a directory or a foreign file in its place).
bool watchlist_clear_clicked(obs_properties_t * /*props*/, obs_property_t * /*property*/, void *data) {
  auto *state = static_cast<FilterState *>(data);
  if (state == nullptr) {
    return false;
  }
  obs_data_t *settings = obs_source_get_settings(state->context);
  try {
    WatchlistSnapshot next = snapshot(*state);
    const bool broken = next.error != StoreError::None;
    next.entries.clear();
    bool saved = is_valid_watchlist_ref(next.ref) && save_watchlist(*state, settings, next, true);
    if (!saved && broken) {
      next.ref = new_watchlist_ref();
      static_cast<void>(ref_registry().claim(next.ref));  // fresh: never held
      try {
        saved = save_watchlist(*state, settings, next, true);
      } catch (...) {
        ref_registry().release(next.ref);
        throw;
      }
      if (!saved) {
        ref_registry().release(next.ref);
      }
    }
    if (saved) {
      obs_data_set_string(settings, kWatchlistRefKey, next.ref.c_str());
    }
    obs_data_set_int(settings, kWatchlistSelectedKey, 0);
    finish_action(*state, saved ? Notice::Cleared : Notice::SaveFailed);
  } catch (...) {
    // Leaves the previous state, including any storage error (fail closed).
  }
  obs_data_release(settings);
  return true;
}

const char *notice_text(Notice notice) {
  switch (notice) {
    case Notice::None:
      return nullptr;
    case Notice::Added:
      return text("SPG.Watchlist.Added");
    case Notice::Removed:
      return text("SPG.Watchlist.Removed");
    case Notice::Cleared:
      return text("SPG.Watchlist.Cleared");
    case Notice::EntryEmpty:
      return text("SPG.Watchlist.EntryEmpty");
    case Notice::EntryTooLong:
      return text("SPG.Watchlist.EntryTooLong");
    case Notice::EntryNoLettersOrDigits:
      return text("SPG.Watchlist.EntryNoLetters");
    case Notice::EntryDuplicate:
      return text("SPG.Watchlist.EntryDuplicate");
    case Notice::ListFull:
      return text("SPG.Watchlist.ListFull");
    case Notice::StorageBroken:
      return text("SPG.Watchlist.StorageBroken");
    case Notice::SaveFailed:
      return text("SPG.Watchlist.SaveFailed");
  }
  return nullptr;
}

bool notice_is_error(Notice notice) {
  return notice != Notice::None && notice != Notice::Added && notice != Notice::Removed && notice != Notice::Cleared;
}

void add_info(obs_properties_t *props, const char *key, const char *description, obs_text_info_type type) {
  obs_property_t *info = obs_properties_add_text(props, key, description, OBS_TEXT_INFO);
  obs_property_text_set_info_type(info, type);
}

void add_watchlist_group(obs_properties_t *props, FilterState *state) {
  obs_properties_t *group = obs_properties_create();

  std::string summary;
  bool broken = false;
  std::size_t count = 0;
  Notice notice = Notice::None;
  if (state != nullptr) {
    const std::lock_guard lock{state->mutex};
    broken = state->store_error != StoreError::None;
    count = state->watchlist.size();
    notice = state->notice;
  }
  if (broken) {
    summary = text("SPG.Watchlist.Broken");
  } else {
    summary = std::to_string(count) + " " + text(count == 1 ? "SPG.Watchlist.CountOne" : "SPG.Watchlist.Count") + " " +
              text("SPG.Watchlist.StoredSeparately");
  }
  obs_property_t *info = obs_properties_add_text(group, kWatchlistInfoKey, summary.c_str(), OBS_TEXT_INFO);
  obs_property_text_set_info_type(info, broken ? OBS_TEXT_INFO_ERROR : OBS_TEXT_INFO_NORMAL);
  if (const char *message = notice_text(notice); message != nullptr) {
    add_info(group, "watchlist_notice", message,
             notice_is_error(notice) ? OBS_TEXT_INFO_WARNING : OBS_TEXT_INFO_NORMAL);
  }

  // Masked entry: OBS draws a password field with a show/hide toggle. Its text
  // moves to pending_entry as it is typed (masked_entry_modified).
  obs_property_t *entry =
      obs_properties_add_text(group, kWatchlistNewEntryKey, text("SPG.Watchlist.NewEntry"), OBS_TEXT_PASSWORD);
  obs_property_set_long_description(entry, text("SPG.Watchlist.NewEntryHelp"));
  obs_property_set_modified_callback2(entry, masked_entry_modified, state);
  obs_property_t *add =
      obs_properties_add_button(group, kWatchlistAddKey, text("SPG.Watchlist.Add"), watchlist_add_clicked);
  obs_property_set_enabled(add, !broken);

  // Entries are listed by number only; their text is never shown again.
  obs_property_t *list = obs_properties_add_list(group, kWatchlistSelectedKey, text("SPG.Watchlist.Entries"),
                                                 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  for (std::size_t i = 0; i < count; ++i) {
    const std::string label = std::string(text("SPG.Watchlist.Entry")) + " " + std::to_string(i + 1);
    obs_property_list_add_int(list, label.c_str(), static_cast<long long>(i));
  }
  obs_property_set_enabled(list, !broken && count > 0);
  obs_property_t *remove =
      obs_properties_add_button(group, kWatchlistRemoveKey, text("SPG.Watchlist.Remove"), watchlist_remove_clicked);
  obs_property_set_enabled(remove, !broken && count > 0);
  obs_property_t *clear =
      obs_properties_add_button(group, kWatchlistClearKey, text("SPG.Watchlist.Clear"), watchlist_clear_clicked);
  obs_property_set_long_description(clear, text("SPG.Watchlist.ClearHelp"));

  obs_properties_add_group(props, "watchlist_group", text("SPG.Watchlist"), OBS_GROUP_NORMAL, group);
}

// What has aired so far: counts and a fixed reason name, never frame content.
void add_render_status(obs_properties_t *props, const FilterState &state) {
  if (state.render == nullptr) {
    return;
  }
  const RenderStatus status = state.render->status();
  std::string line = std::string(text("SPG.Render.Aired")) + " " + std::to_string(status.clean) + " " +
                     text("SPG.Render.Masked") + ", " + std::to_string(status.covered) + " " +
                     text("SPG.Render.Covered");
  if (status.covered > 0) {
    line += std::string(" ") + text("SPG.Render.Last") + " " + cover_name(status.last_cover) + ".";
  }
  add_info(props, kRenderStatusKey, line.c_str(), status.covered > 0 ? OBS_TEXT_INFO_WARNING : OBS_TEXT_INFO_NORMAL);
}

obs_properties_t *filter_get_properties(void *data) {
  auto *state = static_cast<FilterState *>(data);
  obs_properties_t *props = obs_properties_create();
  add_info(props, kStatusKey, text("SPG.Status"), OBS_TEXT_INFO_WARNING);
  if (state != nullptr) {
    FilterIssue issue = FilterIssue::None;
    {
      const std::lock_guard lock{state->mutex};
      issue = state->issue;
    }
    if (issue == FilterIssue::NoCategories) {
      add_info(props, kIssueKey, text("SPG.Issue.NoCategories"), OBS_TEXT_INFO_ERROR);
    } else if (issue == FilterIssue::InvalidConfig) {
      add_info(props, kIssueKey, text("SPG.Issue.InvalidConfig"), OBS_TEXT_INFO_ERROR);
    }
    try {
      add_render_status(props, *state);
    } catch (...) {
      // Allocation failure: no status line.
    }
  }

  try {
    add_watchlist_group(props, state);
  } catch (...) {
    add_info(props, kWatchlistInfoKey, text("SPG.Watchlist.Broken"), OBS_TEXT_INFO_ERROR);
  }

  obs_properties_t *categories = obs_properties_create();
  try {
    for (const Category category : kAllCategories) {
      const std::string key = category_key(category);
      const std::string label_key = "SPG.Category." + key;
      obs_properties_add_bool(categories, key.c_str(), text(label_key.c_str()));
    }
  } catch (...) {
    // Allocation failure: the group shows fewer checkboxes; settings are unchanged.
  }
  obs_properties_add_group(props, "categories_group", text("SPG.Categories"), OBS_GROUP_NORMAL, categories);

  obs_property_t *sensitivity = obs_properties_add_list(props, kSensitivityKey, text("SPG.Sensitivity"),
                                                        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(sensitivity, text("SPG.Sensitivity.Standard"), to_string(Sensitivity::Standard).data());
  obs_property_list_add_string(sensitivity, text("SPG.Sensitivity.High"), to_string(Sensitivity::High).data());
  obs_property_set_long_description(sensitivity, text("SPG.Sensitivity.Help"));

  obs_property_t *lookahead = obs_properties_add_int_slider(props, kLookaheadKey, text("SPG.Lookahead"),
                                                            static_cast<int>(kMinLookaheadMs),
                                                            static_cast<int>(kMaxLookaheadMs), 50);
  obs_property_int_set_suffix(lookahead, " ms");
  obs_property_set_long_description(lookahead, text("SPG.Lookahead.Help"));
  // The filter delays video only; a filter cannot delay audio (SPG-13 spike).
  add_info(props, kAudioKey, text("SPG.Audio"), OBS_TEXT_INFO_WARNING);
  obs_property_t *hold = obs_properties_add_int_slider(props, kHoldKey, text("SPG.Hold"), static_cast<int>(kMinHoldMs),
                                                       static_cast<int>(kMaxHoldMs), 50);
  obs_property_int_set_suffix(hold, " ms");

  obs_property_t *fallback = obs_properties_add_list(props, kFallbackKey, text("SPG.Fallback"), OBS_COMBO_TYPE_LIST,
                                                     OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(fallback, text("SPG.Fallback.BlurSource"),
                               spg::to_string(FallbackMode::BlurSource).data());
  obs_property_list_add_string(fallback, text("SPG.Fallback.HoldLastScanned"),
                               spg::to_string(FallbackMode::HoldLastScanned).data());
  obs_property_list_add_string(fallback, text("SPG.Fallback.SolidFill"),
                               spg::to_string(FallbackMode::SolidFill).data());
  obs_property_set_long_description(fallback, text("SPG.Fallback.Help"));

  obs_properties_add_bool(props, kDebugOverlayKey, text("SPG.DebugOverlay"));
  return props;
}

// The render side draws each frame (docs/plugin-render.md). Without it, fail
// closed: an opaque fill the size of the source, and the target is never
// drawn, so no unscanned pixel reaches the preview, a recording or the stream.
void filter_video_render(void *data, gs_effect_t * /*effect*/) {
  const auto *state = static_cast<const FilterState *>(data);
  if (state->render != nullptr) {
    state->render->render();
    return;
  }
  obs_source_t *target = obs_filter_get_target(state->context);
  const uint32_t width = target != nullptr ? obs_source_get_base_width(target) : 0;
  const uint32_t height = target != nullptr ? obs_source_get_base_height(target) : 0;
  if (width == 0 || height == 0) {
    return;  // nothing to cover; drawing nothing shows nothing
  }
  gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
  vec4 color{};
  vec4_set(&color, 0.0F, 0.0F, 0.0F, 1.0F);
  gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &color);
  while (gs_effect_loop(solid, "Solid")) {
    gs_draw_sprite(nullptr, 0, width, height);
  }
}

obs_source_info make_filter_info() noexcept {
  obs_source_info info{};
  info.id = kFilterId;
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_VIDEO;
  info.get_name = filter_get_name;
  info.create = filter_create;
  info.destroy = filter_destroy;
  info.update = filter_update;
  info.save = filter_save;
  info.get_defaults = filter_get_defaults;
  info.get_properties = filter_get_properties;
  info.video_render = filter_video_render;
  return info;
}

}  // namespace

const obs_source_info &filter_info() noexcept {
  static const obs_source_info info = make_filter_info();
  return info;
}

}  // namespace spg::plugin
