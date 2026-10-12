#pragma once

// Watch-list storage (SPG-15, plan section 12 question 4, recommended default).
//
// OBS saves filter settings in plain text inside the scene collection JSON,
// which users share for support. Watch-list entries therefore never go into
// filter settings: each filter keeps only a reference (32 random hex digits)
// and an entry count there, and the entries live in their own file,
// <OBS config>/plugin_config/obs-spg/watchlists/<reference>.txt, readable and
// writable by the user only (0600 in a 0700 directory on macOS and Linux; on
// Windows the per-user %APPDATA% profile ACL). Nothing here logs; errors are
// fixed codes that carry no text, path or entry.
//
// Plain OBS-free C++ so it is unit-tested on every OS.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "spg/secret.hpp"
#include "spg/secure_memory.hpp"

namespace spg::plugin {

// Limits that keep a watch-list cheap to match (policy cost grows with entries
// x entry length x text length, docs/policy.md) and the file small.
inline constexpr std::size_t kMaxWatchlistEntries = 256;
inline constexpr std::size_t kMaxEntryBytes = 256;

// A new reference: 32 lower-case hex digits from std::random_device.
[[nodiscard]] std::string new_watchlist_ref();
// True for exactly 32 lower-case hex digits. Anything else (a hand-edited or
// hostile scene file, "../x") is rejected before it reaches a path.
[[nodiscard]] bool is_valid_watchlist_ref(std::string_view ref) noexcept;

// Why a new entry was not added. Messages for these never quote the entry.
enum class EntryError : std::uint8_t {
  None,
  Empty,              // blank once spaces and zero-width characters are ignored
  TooLong,            // more than kMaxEntryBytes bytes of UTF-8
  NoLettersOrDigits,  // nothing left to match once normalized
  Duplicate,          // same matching form as an existing entry
  ListFull,           // kMaxWatchlistEntries reached
};

// Checks a new entry against the list it would join, using spg-core's
// normalization (so "Jane  DOE" duplicates "jane doe").
[[nodiscard]] EntryError check_new_entry(std::string_view utf8, const std::vector<Secret>& existing);

enum class StoreError : std::uint8_t {
  None,
  NoConfigDir,   // OBS gave no absolute config directory
  BadReference,  // the reference in the filter settings is malformed
  Missing,       // the file is gone although the settings record entries
  Unreadable,    // I/O error, not a regular file, or permission denied
  Corrupt,       // not a v1 watch-list file, or over the limits
  WriteFailed,   // could not write, sync or replace the file
};

// A short, fixed English description for logs and the properties ("corrupt").
[[nodiscard]] std::string_view describe(StoreError error) noexcept;

// The v1 file body: a header line, then one entry per line with '\\', '\n'
// and '\r' escaped as "\\\\", "\\n" and "\\r". The output is secret, so it
// is built in protected memory.
[[nodiscard]] SecureString encode_watchlist(const std::vector<Secret>& entries);
// Strict inverse of encode_watchlist. Returns nullopt for any other content,
// including a blank entry or a list over the limits.
[[nodiscard]] std::optional<std::vector<Secret>> decode_watchlist(std::string_view body);

struct WatchlistLoad {
  StoreError error = StoreError::None;
  std::vector<Secret> entries;  // empty unless error == None
};

// The file of one filter's watch-list.
class WatchlistStore {
 public:
  // `directory` is the watchlists directory under the OBS module config path;
  // `ref` comes from the filter settings.
  WatchlistStore(std::filesystem::path directory, std::string ref);

  // Reads the watch-list straight into protected memory (no stream buffer).
  // A missing file is an empty list when the settings record no entries
  // (`expected_entries` <= 0, a new filter) and StoreError::Missing otherwise,
  // so a scene collection moved without its watch-list fails closed instead
  // of silently matching nothing. Tightens a file or directory that is
  // readable by others back to user-only.
  [[nodiscard]] WatchlistLoad load(long long expected_entries) const;

  // Replaces the file atomically: writes a user-only temporary file next to
  // it (unbuffered, from protected memory), syncs it and renames it over the
  // old one. Creates the directory (user-only) when needed.
  [[nodiscard]] StoreError save(const std::vector<Secret>& entries) const;

  // The file path, or empty when the directory or reference is invalid.
  [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

 private:
  [[nodiscard]] StoreError check() const noexcept;

  std::filesystem::path directory_;
  std::string ref_;
  std::filesystem::path file_;
};

// The references held by live filters in this process. A filter copied in OBS
// (Duplicate, Copy and Paste Filters) arrives with its original's reference;
// claim() refuses a reference that is already held, so the copy forks a file
// of its own instead of sharing one, and removing an entry from the copy can
// never remove it from the original. Thread-safe.
class WatchlistRefRegistry {
 public:
  // True when `ref` was free and is now held. Malformed references are never
  // held (they name no file) and always succeed.
  [[nodiscard]] bool claim(const std::string &ref);
  void release(const std::string &ref) noexcept;

 private:
  std::mutex mutex_;
  std::set<std::string, std::less<>> refs_;
};

// A UTF-8 path from OBS as a std::filesystem::path (on Windows a plain char
// string would be read in the ANSI code page).
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view utf8);

}  // namespace spg::plugin
