#include "watchlist_store.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>
#include <utility>

#include "spg/policy.hpp"
#include "spg/policy_config.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace spg::plugin {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kHeader = "spg-watchlist 1 # secret: do not share or attach this file";
constexpr std::size_t kRefDigits = 32;
// Encoded entries at most double in size; anything larger is not ours.
constexpr std::uintmax_t kMaxFileBytes = kHeader.size() + 1 + kMaxWatchlistEntries * (2 * kMaxEntryBytes + 1);

std::string random_hex(std::size_t digits) {
  static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::random_device device;
  std::string out;
  out.reserve(digits);
  while (out.size() < digits) {
    std::uint32_t word = device();
    for (int nibble = 0; nibble < 8 && out.size() < digits; ++nibble) {
      out.push_back(kHex[word & 0xFU]);
      word >>= 4U;
    }
  }
  return out;
}

// True when the entry is empty or blank by spg-core's definition (Unicode
// White_Space and zero-width characters only).
bool is_blank(const Secret& entry) {
  PolicyConfig probe;
  probe.watchlist.push_back(entry);
  try {
    validate(probe);
    return false;
  } catch (...) {
    return true;  // the only per-entry check in validate() is blankness
  }
}

bool has_letters_or_digits(const Secret& entry) {
  const Secret form = normalize_for_matching(entry.reveal());
  const std::string_view text = form.reveal();
  return std::any_of(text.begin(), text.end(), [](char c) { return c != ' '; });
}

#ifndef _WIN32
// Clears group and other permission bits; returns false when that fails.
bool make_user_only(const fs::path& path, fs::perms wanted) {
  std::error_code ec;
  const fs::file_status status = fs::symlink_status(path, ec);
  if (ec) {
    return false;
  }
  constexpr fs::perms kOthers = fs::perms::group_all | fs::perms::others_all;
  if ((status.permissions() & kOthers) == fs::perms::none) {
    return true;
  }
  fs::permissions(path, wanted, fs::perm_options::replace, ec);
  return !ec;
}
#endif

StoreError ensure_directory(const fs::path& directory) {
  std::error_code ec;
  fs::create_directories(directory, ec);
  if (ec || !fs::is_directory(fs::symlink_status(directory, ec))) {
    return StoreError::WriteFailed;
  }
#ifndef _WIN32
  if (!make_user_only(directory, fs::perms::owner_all)) {
    return StoreError::WriteFailed;
  }
#endif
  return StoreError::None;
}

// Writes `body` to a new file only the user can read and syncs it.
bool write_new_file(const fs::path& path, const std::string& body) {
#ifdef _WIN32
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  out.flush();
  return static_cast<bool>(out);
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return false;
  }
  const char *data = body.data();
  std::size_t left = body.size();
  bool ok = true;
  while (left > 0) {
    const ssize_t written = ::write(fd, data, left);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ok = false;
      break;
    }
    data += written;
    left -= static_cast<std::size_t>(written);
  }
  ok = ok && ::fsync(fd) == 0;
  ok = (::close(fd) == 0) && ok;
  return ok;
#endif
}

}  // namespace

std::string new_watchlist_ref() {
  return random_hex(kRefDigits);
}

bool is_valid_watchlist_ref(std::string_view ref) noexcept {
  return ref.size() == kRefDigits && std::all_of(ref.begin(), ref.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

EntryError check_new_entry(std::string_view utf8, const std::vector<Secret>& existing) {
  const Secret entry{std::string(utf8)};
  if (is_blank(entry)) {
    return EntryError::Empty;
  }
  if (utf8.size() > kMaxEntryBytes) {
    return EntryError::TooLong;
  }
  if (!has_letters_or_digits(entry)) {
    return EntryError::NoLettersOrDigits;
  }
  const Secret form = normalize_for_matching(utf8);
  const bool duplicate = std::any_of(existing.begin(), existing.end(), [&form](const Secret& other) {
    return normalize_for_matching(other.reveal()) == form;
  });
  if (duplicate) {
    return EntryError::Duplicate;
  }
  if (existing.size() >= kMaxWatchlistEntries) {
    return EntryError::ListFull;
  }
  return EntryError::None;
}

std::string_view describe(StoreError error) noexcept {
  switch (error) {
    case StoreError::None:
      return "ok";
    case StoreError::NoConfigDir:
      return "no OBS config directory";
    case StoreError::BadReference:
      return "malformed watch-list reference in filter settings";
    case StoreError::Missing:
      return "watch-list file missing";
    case StoreError::Unreadable:
      return "watch-list file unreadable";
    case StoreError::Corrupt:
      return "watch-list file corrupt";
    case StoreError::WriteFailed:
      return "watch-list file could not be written";
  }
  return "unknown";
}

std::string encode_watchlist(const std::vector<Secret>& entries) {
  std::string out{kHeader};
  out.push_back('\n');
  for (const Secret& entry : entries) {
    for (const char c : entry.reveal()) {
      switch (c) {
        case '\\':
          out += "\\\\";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        default:
          out.push_back(c);
      }
    }
    out.push_back('\n');
  }
  return out;
}

std::optional<std::vector<Secret>> decode_watchlist(std::string_view body) {
  if (body.substr(0, kHeader.size()) != kHeader || body.size() == kHeader.size() || body[kHeader.size()] != '\n') {
    return std::nullopt;
  }
  body.remove_prefix(kHeader.size() + 1);
  std::vector<Secret> entries;
  while (!body.empty()) {
    const std::size_t end = body.find('\n');
    if (end == std::string_view::npos) {
      return std::nullopt;  // every entry line ends with '\n'; a cut-off file is corrupt
    }
    const std::string_view line = body.substr(0, end);
    body.remove_prefix(end + 1);
    std::string entry;
    for (std::size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (c == '\r') {
        return std::nullopt;
      }
      if (c != '\\') {
        entry.push_back(c);
        continue;
      }
      if (++i == line.size()) {
        return std::nullopt;
      }
      switch (line[i]) {
        case '\\':
          entry.push_back('\\');
          break;
        case 'n':
          entry.push_back('\n');
          break;
        case 'r':
          entry.push_back('\r');
          break;
        default:
          return std::nullopt;
      }
    }
    Secret secret{std::move(entry)};
    if (secret.size() > kMaxEntryBytes || entries.size() >= kMaxWatchlistEntries || is_blank(secret)) {
      return std::nullopt;
    }
    entries.push_back(std::move(secret));
  }
  return entries;
}

WatchlistStore::WatchlistStore(fs::path directory, std::string ref)
    : directory_(std::move(directory)), ref_(std::move(ref)) {
  if (check() == StoreError::None) {
    file_ = directory_ / (ref_ + ".txt");
  }
}

StoreError WatchlistStore::check() const noexcept {
  if (directory_.empty() || !directory_.is_absolute()) {
    return StoreError::NoConfigDir;
  }
  if (!is_valid_watchlist_ref(ref_)) {
    return StoreError::BadReference;
  }
  return StoreError::None;
}

WatchlistLoad WatchlistStore::load(long long expected_entries) const {
  WatchlistLoad result;
  result.error = check();
  if (result.error != StoreError::None) {
    return result;
  }
  std::error_code ec;
  const fs::file_status status = fs::symlink_status(file_, ec);
  if (status.type() == fs::file_type::not_found) {
    result.error = expected_entries > 0 ? StoreError::Missing : StoreError::None;
    return result;
  }
  if (ec || status.type() != fs::file_type::regular) {
    result.error = StoreError::Unreadable;
    return result;
  }
#ifndef _WIN32
  if (!make_user_only(directory_, fs::perms::owner_all) ||
      !make_user_only(file_, fs::perms::owner_read | fs::perms::owner_write)) {
    result.error = StoreError::Unreadable;
    return result;
  }
#endif
  const std::uintmax_t size = fs::file_size(file_, ec);
  if (ec) {
    result.error = StoreError::Unreadable;
    return result;
  }
  if (size > kMaxFileBytes) {
    result.error = StoreError::Corrupt;
    return result;
  }
  std::ifstream in(file_, std::ios::binary);
  if (!in) {
    result.error = StoreError::Unreadable;
    return result;
  }
  const std::string body{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (in.bad()) {
    result.error = StoreError::Unreadable;
    return result;
  }
  std::optional<std::vector<Secret>> entries = decode_watchlist(body);
  if (!entries) {
    result.error = StoreError::Corrupt;
    return result;
  }
  result.entries = std::move(*entries);
  return result;
}

StoreError WatchlistStore::save(const std::vector<Secret>& entries) const {
  if (const StoreError error = check(); error != StoreError::None) {
    return error;
  }
  if (entries.size() > kMaxWatchlistEntries) {
    return StoreError::WriteFailed;
  }
  if (const StoreError error = ensure_directory(directory_); error != StoreError::None) {
    return error;
  }
  fs::path temporary = file_;
  try {
    temporary += ".tmp-" + random_hex(8);
  } catch (...) {
    return StoreError::WriteFailed;
  }
  if (!write_new_file(temporary, encode_watchlist(entries))) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return StoreError::WriteFailed;
  }
  std::error_code ec;
  fs::rename(temporary, file_, ec);
  if (ec) {
    fs::remove(temporary, ec);
    return StoreError::WriteFailed;
  }
  return StoreError::None;
}

bool WatchlistRefRegistry::claim(const std::string &ref) {
  if (!is_valid_watchlist_ref(ref)) {
    return true;
  }
  const std::lock_guard lock{mutex_};
  return refs_.insert(ref).second;
}

void WatchlistRefRegistry::release(const std::string &ref) noexcept {
  const std::lock_guard lock{mutex_};
  const auto found = refs_.find(ref);
  if (found != refs_.end()) {
    refs_.erase(found);
  }
}

fs::path path_from_utf8(std::string_view utf8) {
  std::u8string converted;
  converted.reserve(utf8.size());
  std::transform(utf8.begin(), utf8.end(), std::back_inserter(converted),
                 [](char c) { return static_cast<char8_t>(c); });
  return fs::path(converted);
}

}  // namespace spg::plugin
