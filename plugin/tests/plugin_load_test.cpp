// plugin.loads: start a headless libobs (no video, no UI), load the obs-spg
// module the way OBS does and check what it registers. Then create filters
// and drive their settings and watch-list buttons the way the OBS properties
// view does, capturing every libobs log line, to check clamping, fail-closed
// storage errors, and that watch-list text never reaches the log or the saved
// scene collection data (SPG-15), and that typing into the masked field the
// way the OBS properties view does never leaves it in an undo snapshot
// (SPG-15b). All entries are synthetic.
//
//   obs-spg-load-test <module binary> <module data directory> <config directory>

#include <obs.h>

#include "settings/watchlist_store.hpp"
#include "spg-filter.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace plugin = spg::plugin;

int failures = 0;

// libobs calls the log handler from whichever thread logs, without a lock
// (util/base.c, blogva): obs_source_release() hands a source's destruction to
// libobs's destroy-task thread, which logs ("private source ... destroyed",
// gs_* errors in this headless run) while the main thread logs the next
// source's creation. The capture is therefore guarded; appending to it from
// two threads at once corrupted the heap and aborted the test (SPG-15c).
std::mutex log_mutex;
std::string captured_log;                // guarded by log_mutex
std::set<std::thread::id> log_threads;  // guarded by log_mutex

// Synthetic watch-list entries and fragments of them (raw and as spg-core
// folds them) that must never appear in a log or in saved settings. The
// digits are not a fragment: a log line can hold them by chance (an address).
constexpr const char *kSentinel = "Qorvexian Blathermoor 7319";
constexpr const char *kTypedOnly = "Wyndlequist Pellinore";
constexpr const char *kTypedInView = "Zanthorpe Quillibrand";
constexpr const char *kFragments[] = {"Qorvexian",   "qorvexian",   "qorvexlan", "Blathermoor", "lathermo",
                                      "Wyndlequist", "wyndlequlst", "Pellinore", "pellinore",   "Zanthorpe",
                                      "zanthorpe",   "Quillibrand", "quillibrand", "qulllbrand"};

void check(bool ok, const char *what) {
  std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    ++failures;
  }
}

bool contains_secret(std::string_view text) {
  for (const char *fragment : kFragments) {
    if (text.find(fragment) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

// Captures every libobs log line, the plugin's included, and echoes it. Runs
// on any libobs thread.
void capture_log(int /*level*/, const char *format, va_list args, void * /*param*/) {
  char line[4096];
  va_list copy;
  va_copy(copy, args);
  const int written = std::vsnprintf(line, sizeof(line), format, copy);
  va_end(copy);
  if (written >= 0) {
    const std::lock_guard lock{log_mutex};
    captured_log += line;
    captured_log += '\n';
    log_threads.insert(std::this_thread::get_id());
    std::printf("log: %s\n", line);
  }
}

bool is_filter_type(const char *wanted) {
  const char *id = nullptr;
  for (size_t i = 0; obs_enum_filter_types(i, &id); ++i) {
    if (std::strcmp(id, wanted) == 0) {
      return true;
    }
  }
  return false;
}

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Clicks a button the way the properties view does: with the source as object.
bool click(obs_source_t *source, const char *key) {
  obs_properties_t *props = obs_source_properties(source);
  const bool refreshed = obs_property_button_clicked(obs_properties_get(props, key), source);
  obs_properties_destroy(props);
  return refreshed;
}

obs_text_info_type watchlist_info_type(obs_source_t *source) {
  obs_properties_t *props = obs_source_properties(source);
  const obs_text_info_type type = obs_property_text_info_type(obs_properties_get(props, plugin::kWatchlistInfoKey));
  obs_properties_destroy(props);
  return type;
}

bool add_enabled(obs_source_t *source) {
  obs_properties_t *props = obs_source_properties(source);
  const bool enabled = obs_property_enabled(obs_properties_get(props, plugin::kWatchlistAddKey));
  obs_properties_destroy(props);
  return enabled;
}

std::string saved_json(obs_source_t *source) {
  obs_data_t *saved = obs_save_source(source);
  std::string json = obs_data_get_json(saved);
  obs_data_release(saved);
  return json;
}

// Values from a scene file are clamped and written back; a new filter gets a
// watch-list reference.
void check_clamping() {
  obs_data_t *settings = obs_data_create();
  obs_data_set_int(settings, plugin::kLookaheadKey, 0);
  obs_data_set_int(settings, plugin::kHoldKey, 9999999);
  obs_data_set_string(settings, plugin::kFallbackKey, "show_unscanned");
  obs_data_set_string(settings, plugin::kSensitivityKey, "lenient");
  obs_source_t *source = obs_source_create(plugin::kFilterId, "spg-clamp", settings, nullptr);
  obs_data_release(settings);
  check(source != nullptr, "filter source is created");
  if (source == nullptr) {
    return;
  }
  obs_data_t *applied = obs_source_get_settings(source);
  check(obs_data_get_int(applied, plugin::kLookaheadKey) == plugin::kMinLookaheadMs,
        "lookahead 0 from a scene file is raised to the floor");
  check(obs_data_get_int(applied, plugin::kHoldKey) == plugin::kMaxHoldMs, "an oversized hold is capped");
  check(std::string_view{obs_data_get_string(applied, plugin::kFallbackKey)} == "blur_source",
        "an unknown fallback becomes blur_source");
  check(std::string_view{obs_data_get_string(applied, plugin::kSensitivityKey)} == "standard",
        "an unknown sensitivity becomes standard");
  check(plugin::is_valid_watchlist_ref(obs_data_get_string(applied, plugin::kWatchlistRefKey)),
        "a new filter gets a watch-list reference");
  obs_data_release(applied);
  obs_source_release(source);

  // obs_source_update defers a video filter's update to the next video tick,
  // which a headless libobs never runs, so each case is a new filter.
  settings = obs_data_create();
  obs_data_set_int(settings, plugin::kLookaheadKey, 999999);
  obs_data_set_int(settings, plugin::kHoldKey, -5);
  source = obs_source_create(plugin::kFilterId, "spg-clamp-2", settings, nullptr);
  obs_data_release(settings);
  check(source != nullptr, "second filter source is created");
  if (source == nullptr) {
    return;
  }
  applied = obs_source_get_settings(source);
  check(obs_data_get_int(applied, plugin::kLookaheadKey) == plugin::kMaxLookaheadMs,
        "an oversized lookahead is capped");
  check(obs_data_get_int(applied, plugin::kHoldKey) == plugin::kMinHoldMs, "a negative hold is raised to the floor");
  obs_data_release(applied);
  obs_source_release(source);
}

void check_watchlist(const fs::path &watchlist_dir) {
  obs_source_t *source = obs_source_create(plugin::kFilterId, "spg-watchlist", nullptr, nullptr);
  check(source != nullptr, "filter source is created without settings");
  if (source == nullptr) {
    return;
  }
  obs_data_t *settings = obs_source_get_settings(source);
  const std::string ref = obs_data_get_string(settings, plugin::kWatchlistRefKey);
  const fs::path file = watchlist_dir / (ref + ".txt");
  check(watchlist_info_type(source) == OBS_TEXT_INFO_NORMAL, "an empty watch-list is not an error");

  // Type an entry into the masked field and press Add.
  obs_data_set_string(settings, plugin::kWatchlistNewEntryKey, kSentinel);
  check(click(source, plugin::kWatchlistAddKey), "Add asks OBS to refresh the properties");
  check(!obs_data_has_user_value(settings, plugin::kWatchlistNewEntryKey), "Add clears the masked field");
  check(obs_data_get_int(settings, plugin::kWatchlistCountKey) == 1, "the settings record one entry");
  check(read_file(file).find(kSentinel) != std::string::npos, "the entry is in the watch-list file");
#ifndef _WIN32
  constexpr fs::perms kOthers = fs::perms::group_all | fs::perms::others_all;
  check((fs::status(file).permissions() & kOthers) == fs::perms::none, "the watch-list file is user-only");
#endif

  obs_data_set_string(settings, plugin::kWatchlistNewEntryKey, "qorvexian  BLATHERMOOR 7319");
  click(source, plugin::kWatchlistAddKey);
  check(obs_data_get_int(settings, plugin::kWatchlistCountKey) == 1, "a duplicate entry is not added");

  // An entry typed but never added must not be saved in the scene collection.
  obs_data_set_string(settings, plugin::kWatchlistNewEntryKey, kTypedOnly);
  const std::string json = saved_json(source);
  check(!contains_secret(json), "saved scene collection data holds no watch-list text");
  check(json.find(ref) != std::string::npos, "saved scene collection data holds the reference");
  check(!contains_secret(obs_data_get_json(settings)), "saving erased the unsent masked entry");

  // The entry list shows numbers only.
  obs_properties_t *props = obs_source_properties(source);
  obs_property_t *list = obs_properties_get(props, plugin::kWatchlistSelectedKey);
  check(obs_property_list_item_count(list) == 1, "the entry list has one item");
  check(!contains_secret(obs_property_list_item_name(list, 0)), "the entry list shows no entry text");
  obs_properties_destroy(props);

  obs_data_set_int(settings, plugin::kWatchlistSelectedKey, 0);
  click(source, plugin::kWatchlistRemoveKey);
  check(obs_data_get_int(settings, plugin::kWatchlistCountKey) == 0, "Remove deletes the entry");
  check(read_file(file).find(kSentinel) == std::string::npos, "the removed entry is gone from the file");

  // Add it back so the file has an entry.
  obs_data_set_string(settings, plugin::kWatchlistNewEntryKey, kSentinel);
  click(source, plugin::kWatchlistAddKey);

  // A duplicated filter forks its own watch-list file: emptying the copy
  // leaves the original's entries alone.
  obs_source_t *copy = obs_source_duplicate(source, "spg-watchlist-copy", false);
  check(copy != nullptr, "the filter can be duplicated");
  if (copy != nullptr) {
    obs_data_t *copy_settings = obs_source_get_settings(copy);
    const std::string copy_ref = obs_data_get_string(copy_settings, plugin::kWatchlistRefKey);
    check(plugin::is_valid_watchlist_ref(copy_ref) && copy_ref != ref, "the copy gets its own reference");
    check(obs_data_get_int(copy_settings, plugin::kWatchlistCountKey) == 1, "the copy starts with the same entries");
    obs_data_release(copy_settings);
    click(copy, plugin::kWatchlistClearKey);
    check(read_file(file).find(kSentinel) != std::string::npos, "emptying the copy keeps the original's file");
    obs_source_release(copy);
  }

  // Reload from saved scene collection data.
  obs_data_t *saved = obs_save_source(source);
  obs_data_release(settings);
  obs_source_release(source);

  obs_source_t *loaded = obs_load_source(saved);
  obs_data_release(saved);
  check(loaded != nullptr, "the filter loads back from saved data");
  if (loaded != nullptr) {
    check(watchlist_info_type(loaded) == OBS_TEXT_INFO_NORMAL, "the reloaded filter finds its watch-list");
    obs_source_release(loaded);
  }
}

// The OBS 32.1.2 properties view, for one edit of a field
// (shared/properties-view/properties-view.cpp, WidgetInfo::ControlChanged):
// when a burst of edits starts it snapshots the settings, then it writes the
// widget's whole value to the settings, calls the visual update (for filters,
// obs_source_update) and then the property's modified callback. Returns true
// when the view would rebuild the properties.
bool view_edit(obs_source_t *source, obs_properties_t *props, obs_data_t *settings, const char *key,
               obs_data_t *&burst_snapshot, void (*write)(obs_data_t *, const char *, const char *),
               const char *value) {
  if (burst_snapshot == nullptr) {
    burst_snapshot = obs_data_create();
    obs_data_apply(burst_snapshot, settings);
  }
  write(settings, key, value);
  obs_source_update(source, settings);
  return obs_property_modified(obs_properties_get(props, key), settings);
}

void write_string(obs_data_t *settings, const char *key, const char *value) {
  obs_data_set_string(settings, key, value);
}

void write_int(obs_data_t *settings, const char *key, const char *value) {
  obs_data_set_int(settings, key, std::stoll(value));
}

// Settings keys the filter owns. A snapshot holding anything else (the masked
// entry field) fails the check.
bool only_known_keys(obs_data_t *data) {
  std::vector<std::string> known = {plugin::kWatchlistRefKey, plugin::kWatchlistCountKey, plugin::kWatchlistSelectedKey,
                                    plugin::kLookaheadKey,    plugin::kHoldKey,           plugin::kSensitivityKey,
                                    plugin::kFallbackKey,     plugin::kDebugOverlayKey};
  for (const spg::Category category : spg::kAllCategories) {
    known.push_back(plugin::category_key(category));
  }
  bool ok = true;
  for (obs_data_item_t *item = obs_data_first(data); item != nullptr; obs_data_item_next(&item)) {
    ok = ok && std::find(known.begin(), known.end(), obs_data_item_get_name(item)) != known.end();
  }
  return ok;
}

// 500 ms after the last edit of a burst the view calls the filters dialog's
// FilterChangeUndoRedo (frontend/dialogs/OBSBasicFilters.cpp) with the
// snapshot and the live settings; it stores both as JSON in the undo stack.
void view_record_undo(obs_data_t *settings, obs_data_t *&burst_snapshot, std::vector<std::string> &undo_stack) {
  check(only_known_keys(burst_snapshot) && only_known_keys(settings),
        "undo snapshots hold only the filter's own settings (watch-list reference and count, no entry)");
  undo_stack.emplace_back(obs_data_get_json(burst_snapshot));
  undo_stack.emplace_back(obs_data_get_json(settings));
  obs_data_release(burst_snapshot);
  burst_snapshot = nullptr;
}

// Typing into the masked field in the OBS properties view never leaves the
// text in the settings, so neither the undo stack nor a save can hold it, and
// Add still stores what was typed.
void check_undo_snapshots(const fs::path &watchlist_dir) {
  obs_source_t *source = obs_source_create(plugin::kFilterId, "spg-undo", nullptr, nullptr);
  check(source != nullptr, "filter source for the undo checks is created");
  if (source == nullptr) {
    return;
  }
  obs_data_t *settings = obs_source_get_settings(source);
  const std::string ref = obs_data_get_string(settings, plugin::kWatchlistRefKey);
  const fs::path file = watchlist_dir / (ref + ".txt");
  std::vector<std::string> undo_stack;
  obs_data_t *burst = nullptr;

  // The view builds its properties once, then the user types one key at a time.
  obs_properties_t *props = obs_source_properties(source);
  const std::string typed = kTypedInView;
  bool refreshed = false;
  bool prefix_in_settings = false;
  for (std::size_t n = 1; n <= typed.size(); ++n) {
    const std::string prefix = typed.substr(0, n);
    refreshed = view_edit(source, props, settings, plugin::kWatchlistNewEntryKey, burst, write_string,
                          prefix.c_str()) ||
                refreshed;
    prefix_in_settings = prefix_in_settings || (n >= 3 && std::string_view{obs_data_get_json(settings)}.find(
                                                              prefix) != std::string_view::npos);
  }
  check(!refreshed, "typing in the masked field does not rebuild the properties");
  check(!prefix_in_settings, "no typed prefix is left in the settings after its keystroke");
  check(!obs_data_has_user_value(settings, plugin::kWatchlistNewEntryKey), "the masked field is not in the settings");
  view_record_undo(settings, burst, undo_stack);

  // A slider moved in the same dialog records its own undo step.
  view_edit(source, props, settings, plugin::kLookaheadKey, burst, write_int, "700");
  view_record_undo(settings, burst, undo_stack);

  // Add, pressed in the same view, stores what was typed.
  check(obs_property_button_clicked(obs_properties_get(props, plugin::kWatchlistAddKey), source),
        "Add asks the view to refresh");
  obs_properties_destroy(props);
  check(obs_data_get_int(settings, plugin::kWatchlistCountKey) == 1, "Add stores the entry typed in the view");
  check(read_file(file).find(kTypedInView) != std::string::npos, "the typed entry is in the watch-list file");

  // Text typed and then dropped by a rebuilt view (the field shows empty) is
  // never added.
  props = obs_source_properties(source);
  view_edit(source, props, settings, plugin::kWatchlistNewEntryKey, burst, write_string, kTypedOnly);
  view_record_undo(settings, burst, undo_stack);
  obs_properties_destroy(props);
  props = obs_source_properties(source);
  obs_property_button_clicked(obs_properties_get(props, plugin::kWatchlistAddKey), source);
  obs_properties_destroy(props);
  check(obs_data_get_int(settings, plugin::kWatchlistCountKey) == 1, "text the rebuilt field no longer shows is not added");
  check(read_file(file).find(kTypedOnly) == std::string::npos, "the dropped text is not in the watch-list file");

  // Text typed in the view, then the field set to empty in the settings (a
  // script) before Add: Add sees an empty entry and adds nothing.
  props = obs_source_properties(source);
  view_edit(source, props, settings, plugin::kWatchlistNewEntryKey, burst, write_string, kTypedOnly);
  view_record_undo(settings, burst, undo_stack);
  obs_data_set_string(settings, plugin::kWatchlistNewEntryKey, "");
  obs_property_button_clicked(obs_properties_get(props, plugin::kWatchlistAddKey), source);
  obs_properties_destroy(props);
  check(obs_data_get_int(settings, plugin::kWatchlistCountKey) == 1, "an emptied field adds nothing");
  check(read_file(file).find(kTypedOnly) == std::string::npos, "text typed before the field was emptied is not added");
  check(!obs_data_has_user_value(settings, plugin::kWatchlistNewEntryKey), "Add clears the emptied field");

  bool secret_in_undo = false;
  bool ref_in_undo = true;
  for (const std::string &json : undo_stack) {
    secret_in_undo = secret_in_undo || contains_secret(json);
    ref_in_undo = ref_in_undo && json.find(ref) != std::string::npos;
  }
  check(!undo_stack.empty() && !secret_in_undo, "no undo snapshot holds watch-list text");
  check(ref_in_undo, "every undo snapshot holds the watch-list reference");
  check(!contains_secret(saved_json(source)), "saved scene collection data holds no typed text");

  obs_data_release(settings);
  obs_source_release(source);
}

// A watch-list that cannot be read turns detection off and shows an error;
// "Remove all entries" is the way back.
void check_storage_errors() {
  obs_data_t *settings = obs_data_create();
  obs_data_set_string(settings, plugin::kWatchlistRefKey, plugin::new_watchlist_ref().c_str());
  obs_data_set_int(settings, plugin::kWatchlistCountKey, 2);  // a scene file moved without its watch-list
  obs_source_t *missing = obs_source_create(plugin::kFilterId, "spg-missing", settings, nullptr);
  obs_data_release(settings);
  check(missing != nullptr, "a filter with a missing watch-list is created");
  if (missing != nullptr) {
    check(watchlist_info_type(missing) == OBS_TEXT_INFO_ERROR, "a missing watch-list is shown as an error");
    check(!add_enabled(missing), "Add is disabled while the watch-list is broken");
    click(missing, plugin::kWatchlistClearKey);
    check(watchlist_info_type(missing) == OBS_TEXT_INFO_NORMAL, "Remove all entries starts a new watch-list");
    check(add_enabled(missing), "Add works again");
    obs_source_release(missing);
  }

  settings = obs_data_create();
  obs_data_set_string(settings, plugin::kWatchlistRefKey, "../../escape");
  obs_source_t *bad = obs_source_create(plugin::kFilterId, "spg-bad-ref", settings, nullptr);
  obs_data_release(settings);
  check(bad != nullptr, "a filter with a malformed reference is created");
  if (bad != nullptr) {
    check(watchlist_info_type(bad) == OBS_TEXT_INFO_ERROR, "a malformed reference is shown as an error");
    click(bad, plugin::kWatchlistClearKey);
    obs_data_t *fixed = obs_source_get_settings(bad);
    check(plugin::is_valid_watchlist_ref(obs_data_get_string(fixed, plugin::kWatchlistRefKey)),
          "Remove all entries replaces a malformed reference");
    obs_data_release(fixed);
    obs_source_release(bad);
  }
}

std::string settings_ref_of(obs_source_t *source) {
  obs_data_t *settings = obs_source_get_settings(source);
  std::string ref = obs_data_get_string(settings, plugin::kWatchlistRefKey);
  obs_data_release(settings);
  return ref;
}

obs_source_t *filter_with_ref(const char *name, const std::string &ref) {
  obs_data_t *settings = obs_data_create();
  obs_data_set_string(settings, plugin::kWatchlistRefKey, ref.c_str());
  obs_source_t *source = obs_source_create(plugin::kFilterId, name, settings, nullptr);
  obs_data_release(settings);
  return source;
}

// A copy of a filter whose watch-list cannot be read stays broken after a
// reload, even when the scene file records no entries (an older backup): its
// missing file must not read as a new, empty watch-list.
void check_broken_copy(const fs::path &watchlist_dir) {
  const std::string ref = plugin::new_watchlist_ref();
  {
    std::ofstream out(watchlist_dir / (ref + ".txt"), std::ios::binary);
    out << "not a watch-list\n";
  }
  obs_source_t *original = filter_with_ref("spg-corrupt", ref);  // watchlist_count stays 0
  check(original != nullptr, "a filter with a corrupt watch-list is created");
  if (original == nullptr) {
    return;
  }
  check(watchlist_info_type(original) == OBS_TEXT_INFO_ERROR, "a corrupt watch-list is shown as an error");
  obs_source_t *copy = obs_source_duplicate(original, "spg-corrupt-copy", false);
  check(copy != nullptr, "a filter with a corrupt watch-list can be duplicated");
  if (copy != nullptr) {
    check(watchlist_info_type(copy) == OBS_TEXT_INFO_ERROR, "the copy of a corrupt watch-list is an error");
    obs_data_t *saved = obs_save_source(copy);
    obs_source_release(copy);
    obs_source_t *reloaded = obs_load_source(saved);
    obs_data_release(saved);
    check(reloaded != nullptr, "the copy loads back from saved data");
    if (reloaded != nullptr) {
      check(watchlist_info_type(reloaded) == OBS_TEXT_INFO_ERROR, "the reloaded copy still fails closed");
      check(!add_enabled(reloaded), "Add stays disabled on the reloaded copy");
      click(reloaded, plugin::kWatchlistClearKey);
      check(watchlist_info_type(reloaded) == OBS_TEXT_INFO_NORMAL, "Remove all entries recovers the copy");
      obs_source_release(reloaded);
    }
  }
  obs_source_release(original);
}

// "Remove all entries" recovers even when the file cannot be replaced: a
// directory in its place gets the filter a new reference, and the old one is
// released.
void check_unreplaceable_file(const fs::path &watchlist_dir) {
  const std::string ref = plugin::new_watchlist_ref();
  fs::create_directories(watchlist_dir / (ref + ".txt"));
  obs_source_t *source = filter_with_ref("spg-dir-in-place", ref);
  check(source != nullptr, "a filter whose watch-list path is a directory is created");
  if (source == nullptr) {
    return;
  }
  check(watchlist_info_type(source) == OBS_TEXT_INFO_ERROR, "a directory in the file's place is an error");
  click(source, plugin::kWatchlistClearKey);
  check(watchlist_info_type(source) == OBS_TEXT_INFO_NORMAL, "Remove all entries recovers under a new reference");
  check(add_enabled(source), "Add works again after recovery");
  const std::string fresh = settings_ref_of(source);
  check(plugin::is_valid_watchlist_ref(fresh) && fresh != ref, "the settings hold the new reference");

  // The old reference was released: a filter that names it is not forked.
  obs_source_t *other = filter_with_ref("spg-dir-in-place-2", ref);
  check(other != nullptr && settings_ref_of(other) == ref, "Remove all entries releases the old reference");
  obs_source_release(other);
  obs_source_release(source);
}

void run_filter_checks(obs_module_t *module) {
  char *dir = obs_module_get_config_path(module, plugin::kWatchlistDirName);
  const fs::path watchlist_dir = plugin::path_from_utf8(dir);
  bfree(dir);
  check_clamping();
  check_watchlist(watchlist_dir);
  check_undo_snapshots(watchlist_dir);
  check_storage_errors();
  check_broken_copy(watchlist_dir);
  check_unreplaceable_file(watchlist_dir);

  // Nothing outside the watch-list directory holds watch-list text.
  std::error_code ec;
  for (const auto &item : fs::recursive_directory_iterator(watchlist_dir.parent_path().parent_path(), ec)) {
    if (item.is_regular_file() && item.path().parent_path() != watchlist_dir) {
      check(!contains_secret(read_file(item.path())), "no watch-list text outside the watch-list files");
    }
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <module binary> <module data directory> <config directory>\n", argv[0]);
    return 2;
  }
  const fs::path config_dir = plugin::path_from_utf8(argv[3]);
  std::error_code ec;
  fs::remove_all(config_dir, ec);
  fs::create_directories(config_dir);

  base_set_log_handler(capture_log, nullptr);
  if (!obs_startup("en-US", argv[3], nullptr)) {
    std::fprintf(stderr, "FAIL: obs_startup\n");
    return 1;
  }

  obs_module_t *module = nullptr;
  const int opened = obs_open_module(&module, argv[1], argv[2]);
  check(opened == MODULE_SUCCESS, "obs_open_module returns MODULE_SUCCESS");
  if (opened == MODULE_SUCCESS) {
    check(obs_init_module(module), "obs_init_module runs obs_module_load");

    check(is_filter_type(plugin::kFilterId), "filter is registered as a filter type");
    const uint32_t flags = obs_get_source_output_flags(plugin::kFilterId);
    check((flags & OBS_SOURCE_VIDEO) != 0, "filter declares video output");
    check((flags & OBS_SOURCE_ASYNC) == 0, "filter renders on the graphics thread (not async)");

    const char *name = obs_source_get_display_name(plugin::kFilterId);
    check(name != nullptr && std::string_view{name} == "Privacy Guard",
          "display name comes from data/locale/en-US.ini");

    obs_data_t *defaults = obs_get_source_defaults(plugin::kFilterId);
    check(defaults != nullptr, "defaults are available");
    if (defaults != nullptr) {
      check(obs_data_get_int(defaults, plugin::kLookaheadKey) == plugin::kDefaultLookaheadMs,
            "lookahead defaults to 500 ms");
      check(obs_data_get_int(defaults, plugin::kHoldKey) == plugin::kDefaultHoldMs, "hold defaults to 750 ms");
      check(std::string_view{obs_data_get_string(defaults, plugin::kFallbackKey)} == "blur_source",
            "fallback defaults to blur_source");
      check(!obs_data_get_bool(defaults, plugin::kDebugOverlayKey), "debug overlay defaults to off");
      for (const spg::Category category : spg::kAllCategories) {
        check(obs_data_get_bool(defaults, plugin::category_key(category).c_str()), "every category defaults to on");
      }
      obs_data_release(defaults);
    }

    obs_properties_t *props = obs_get_source_properties(plugin::kFilterId);
    check(props != nullptr, "properties are available");
    if (props != nullptr) {
      obs_property_t *status = obs_properties_get(props, plugin::kStatusKey);
      check(status != nullptr && obs_property_get_type(status) == OBS_PROPERTY_TEXT &&
                obs_property_text_type(status) == OBS_TEXT_INFO,
            "status notice is an info text property");
      obs_property_t *lookahead = obs_properties_get(props, plugin::kLookaheadKey);
      check(lookahead != nullptr && obs_property_get_type(lookahead) == OBS_PROPERTY_INT,
            "lookahead is an integer property");
      check(obs_property_int_min(lookahead) == plugin::kMinLookaheadMs &&
                obs_property_int_max(lookahead) == plugin::kMaxLookaheadMs,
            "lookahead slider matches the clamp range");
      obs_property_t *hold = obs_properties_get(props, plugin::kHoldKey);
      check(hold != nullptr && obs_property_int_min(hold) == plugin::kMinHoldMs &&
                obs_property_int_max(hold) == plugin::kMaxHoldMs,
            "hold slider matches the clamp range");
      obs_property_t *entry = obs_properties_get(props, plugin::kWatchlistNewEntryKey);
      check(entry != nullptr && obs_property_get_type(entry) == OBS_PROPERTY_TEXT &&
                obs_property_text_type(entry) == OBS_TEXT_PASSWORD,
            "watch-list entry is a masked (password) field");
      for (const spg::Category category : spg::kAllCategories) {
        check(obs_properties_get(props, plugin::category_key(category).c_str()) != nullptr,
              "every category has a checkbox");
      }
      for (const char *key : {plugin::kSensitivityKey, plugin::kFallbackKey, plugin::kDebugOverlayKey,
                              plugin::kWatchlistAddKey, plugin::kWatchlistSelectedKey, plugin::kWatchlistRemoveKey,
                              plugin::kWatchlistClearKey}) {
        check(obs_properties_get(props, key) != nullptr, key);
      }
      obs_properties_destroy(props);
    }

    run_filter_checks(module);
  }

  // obs_shutdown joins the destroy-task thread, so every filter's destroy and
  // its log lines have run by the time it returns.
  obs_shutdown();
  base_set_log_handler(nullptr, nullptr);

  std::string log_text;
  std::size_t logging_threads = 0;
  {
    const std::lock_guard lock{log_mutex};
    log_text = captured_log;
    logging_threads = log_threads.size();
  }
  check(!contains_secret(log_text), "no watch-list text in the libobs log");
  check(log_text.find("[obs-spg] watch-list saved") != std::string::npos,
        "the log capture saw the plugin's own lines");
  // Why the capture is locked: sources are destroyed, and log, on libobs's
  // destroy-task thread while the main thread keeps logging.
  check(logging_threads > 1, "libobs logs from more than one thread (deferred source destruction)");
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
