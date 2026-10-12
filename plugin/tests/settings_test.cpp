// Unit tests for the plugin's OBS-free settings code (SPG-15): clamping of
// values loaded from scene files, the PolicyConfig the settings become, the
// watch-list file format and its user-only storage, and secrecy. Every entry
// here is synthetic.

#include <catch2/catch_test_macros.hpp>

#include "settings/filter_settings.hpp"
#include "settings/watchlist_store.hpp"

#include <algorithm>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace spg;
using namespace spg::plugin;

// A unique directory under the system temp directory, removed afterwards.
class TempDir {
 public:
  TempDir() {
    std::random_device device;
    path_ = fs::temp_directory_path() / ("spg15-settings-test-" + std::to_string(device()) + std::to_string(device()));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;
  [[nodiscard]] const fs::path &path() const { return path_; }

 private:
  fs::path path_;
};

std::vector<Secret> secrets(std::initializer_list<const char *> items) {
  std::vector<Secret> out;
  for (const char *item : items) {
    out.emplace_back(item);
  }
  return out;
}

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path &path, const std::string &body) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
}

// True when only the owner has access, with exactly the `owner` bits. Windows
// has no POSIX modes (the per-user profile ACL applies), so it is not checked.
bool is_user_only([[maybe_unused]] const fs::path &path, [[maybe_unused]] fs::perms owner) {
#ifdef _WIN32
  return true;
#else
  const fs::perms perms = fs::status(path).permissions();
  constexpr fs::perms kOthers = fs::perms::group_all | fs::perms::others_all;
  return (perms & kOthers) == fs::perms::none && (perms & fs::perms::owner_all) == owner;
#endif
}

constexpr const char *kRef = "0123456789abcdef0123456789abcdef";

// Redirects std::cout, std::cerr and std::clog into a buffer until release()
// or destruction, so a failing REQUIRE cannot leave them dangling.
class CaptureStandardStreams {
 public:
  CaptureStandardStreams()
      : out_(std::cout.rdbuf(buffer_.rdbuf())), err_(std::cerr.rdbuf(buffer_.rdbuf())),
        log_(std::clog.rdbuf(buffer_.rdbuf())) {}
  ~CaptureStandardStreams() { restore(); }
  CaptureStandardStreams(const CaptureStandardStreams &) = delete;
  CaptureStandardStreams &operator=(const CaptureStandardStreams &) = delete;

  std::string release() {
    restore();
    return buffer_.str();
  }

 private:
  void restore() {
    if (out_ != nullptr) {
      std::cout.rdbuf(out_);
      std::cerr.rdbuf(err_);
      std::clog.rdbuf(log_);
      out_ = nullptr;
    }
  }

  std::ostringstream buffer_;
  std::streambuf *out_;
  std::streambuf *err_;
  std::streambuf *log_;
};

}  // namespace

TEST_CASE("lookahead and hold from a scene file are clamped to safe ranges", "[settings]") {
  CHECK(clamp_lookahead_ms(0) == kMinLookaheadMs);
  CHECK(clamp_lookahead_ms(-500) == kMinLookaheadMs);
  CHECK(clamp_lookahead_ms(LLONG_MIN) == kMinLookaheadMs);
  CHECK(clamp_lookahead_ms(kDefaultLookaheadMs) == kDefaultLookaheadMs);
  CHECK(clamp_lookahead_ms(99999) == kMaxLookaheadMs);
  CHECK(clamp_lookahead_ms(LLONG_MAX) == kMaxLookaheadMs);

  CHECK(clamp_hold_ms(0) == kMinHoldMs);
  CHECK(clamp_hold_ms(-1) == kMinHoldMs);
  CHECK(clamp_hold_ms(kDefaultHoldMs) == kDefaultHoldMs);
  CHECK(clamp_hold_ms(LLONG_MAX) == kMaxHoldMs);

  // A floor of zero would let a scene file switch the protection off.
  STATIC_REQUIRE(kMinLookaheadMs > 0);
  STATIC_REQUIRE(kMinHoldMs > 0);
  STATIC_REQUIRE(kMinLookaheadMs <= kDefaultLookaheadMs && kDefaultLookaheadMs <= kMaxLookaheadMs);
  STATIC_REQUIRE(kMinHoldMs <= kDefaultHoldMs && kDefaultHoldMs <= kMaxHoldMs);
}

TEST_CASE("unknown names fall back to the safe defaults", "[settings]") {
  RawFilterSettings raw;
  raw.lookahead_ms = 0;
  raw.hold_ms = 0;
  raw.sensitivity = "lenient";
  raw.fallback = "show_unscanned";
  const FilterSettings clean = sanitize(raw);
  CHECK(clean.lookahead_ms == kMinLookaheadMs);
  CHECK(clean.hold_ms == kMinHoldMs);
  CHECK(clean.sensitivity == Sensitivity::Standard);
  CHECK(clean.fallback == FallbackMode::BlurSource);
  CHECK_FALSE(clean.debug_overlay);

  raw.sensitivity = "";
  raw.fallback = "";
  CHECK(sanitize(raw).fallback == FallbackMode::BlurSource);

  raw.sensitivity = "high";
  raw.fallback = "solid_fill";
  CHECK(sanitize(raw).sensitivity == Sensitivity::High);
  CHECK(sanitize(raw).fallback == FallbackMode::SolidFill);
  CHECK(sanitize(RawFilterSettings{}) == FilterSettings{});
}

TEST_CASE("sensitivity presets never match less than the spg-core defaults", "[settings]") {
  const FuzzyLimits standard = fuzzy_limits(Sensitivity::Standard);
  const FuzzyLimits high = fuzzy_limits(Sensitivity::High);
  CHECK(standard == FuzzyLimits{});
  CHECK(high.max_edits >= standard.max_edits);
  CHECK(high.max_edit_ratio >= standard.max_edit_ratio);
  CHECK(high.min_partial_chars <= standard.min_partial_chars);
  CHECK(parse_sensitivity(to_string(Sensitivity::High)) == Sensitivity::High);
  CHECK(parse_sensitivity(to_string(Sensitivity::Standard)) == Sensitivity::Standard);
  CHECK_FALSE(parse_sensitivity("low").has_value());
}

TEST_CASE("category settings keys", "[settings]") {
  CHECK(category_key(Category::Face) == "category_face");
  CHECK(category_key(Category::Watchlist) == "category_watchlist");
  CHECK(category_key(Category::PiiEmail) == "category_pii_email");
  CHECK(category_key(Category::PiiToken) == "category_pii_token");
}

TEST_CASE("settings become a valid PolicyConfig", "[settings]") {
  FilterSettings settings;
  settings.lookahead_ms = 800;
  settings.hold_ms = 1200;
  settings.sensitivity = Sensitivity::High;
  settings.fallback = FallbackMode::HoldLastScanned;
  std::optional<PolicyConfig> config;
  REQUIRE(make_policy_config(settings, secrets({"Zorblax Quentivar"}), config) == FilterIssue::None);
  REQUIRE(config.has_value());
  CHECK(config->lookahead == std::chrono::milliseconds{800});
  CHECK(config->retro == config->lookahead);
  CHECK(config->hold == std::chrono::milliseconds{1200});
  CHECK(config->fuzzy == fuzzy_limits(Sensitivity::High));
  CHECK(config->fallback == FallbackMode::HoldLastScanned);
  CHECK(config->categories == CategorySet::all());
  CHECK(config->watchlist == secrets({"Zorblax Quentivar"}));
  CHECK_NOTHROW(validate(*config));

  SECTION("values that skipped sanitize are still clamped") {
    settings.lookahead_ms = 0;
    settings.hold_ms = 0;
    REQUIRE(make_policy_config(settings, {}, config) == FilterIssue::None);
    CHECK(config->lookahead == std::chrono::milliseconds{kMinLookaheadMs});
    CHECK(config->hold == std::chrono::milliseconds{kMinHoldMs});
  }
  SECTION("every category off fails closed") {
    settings.categories = CategorySet{};
    CHECK(make_policy_config(settings, {}, config) == FilterIssue::NoCategories);
    CHECK_FALSE(config.has_value());
  }
  SECTION("an entry spg-core rejects fails closed") {
    CHECK(make_policy_config(settings, secrets({"!!!"}), config) == FilterIssue::InvalidConfig);
    CHECK_FALSE(config.has_value());
    CHECK(make_policy_config(settings, secrets({"  "}), config) == FilterIssue::InvalidConfig);
    CHECK_FALSE(config.has_value());
  }
}

TEST_CASE("watch-list references are 32 lower-case hex digits", "[watchlist]") {
  const std::string a = new_watchlist_ref();
  const std::string b = new_watchlist_ref();
  CHECK(is_valid_watchlist_ref(a));
  CHECK(is_valid_watchlist_ref(b));
  CHECK(a != b);
  CHECK(is_valid_watchlist_ref(kRef));
  CHECK_FALSE(is_valid_watchlist_ref(""));
  CHECK_FALSE(is_valid_watchlist_ref("0123456789abcdef0123456789abcde"));    // 31
  CHECK_FALSE(is_valid_watchlist_ref("0123456789abcdef0123456789abcdef0"));  // 33
  CHECK_FALSE(is_valid_watchlist_ref("0123456789ABCDEF0123456789abcdef"));
  CHECK_FALSE(is_valid_watchlist_ref("../../../../../../../../etc/pass"));
  CHECK_FALSE(is_valid_watchlist_ref("0123456789abcdef0123456789abcde/"));
}

TEST_CASE("a reference is held by one live filter at a time", "[watchlist]") {
  WatchlistRefRegistry registry;
  const std::string ref = new_watchlist_ref();
  CHECK(registry.claim(ref));
  CHECK_FALSE(registry.claim(ref));  // a copied filter must fork
  registry.release(ref);
  CHECK(registry.claim(ref));  // free again once the holder is gone
  CHECK(registry.claim(new_watchlist_ref()));
  // Malformed references name no file: never held, never refused.
  CHECK(registry.claim("../escape"));
  CHECK(registry.claim("../escape"));
  registry.release("../escape");
  registry.release(new_watchlist_ref());  // releasing an unknown reference is harmless
}

TEST_CASE("new entries are checked before they are stored", "[watchlist]") {
  const std::vector<Secret> existing = secrets({"Zorblax Quentivar", "4821 Qx Maple Synthway"});
  CHECK(check_new_entry("Vexmoor Dalquist", existing) == EntryError::None);
  CHECK(check_new_entry("", existing) == EntryError::Empty);
  CHECK(check_new_entry(" \t ", existing) == EntryError::Empty);
  CHECK(check_new_entry("\xE2\x80\x8B\xE3\x80\x80", existing) == EntryError::Empty);  // U+200B U+3000
  CHECK(check_new_entry(std::string(kMaxEntryBytes + 1, 'q'), existing) == EntryError::TooLong);
  CHECK(check_new_entry(std::string(kMaxEntryBytes, 'q'), existing) == EntryError::None);
  CHECK(check_new_entry("!!! ---", existing) == EntryError::NoLettersOrDigits);
  CHECK(check_new_entry("zorblax  QUENTIVAR", existing) == EntryError::Duplicate);
  CHECK(check_new_entry("Zorblax, Quentivar!", existing) == EntryError::Duplicate);

  std::vector<Secret> full;
  for (std::size_t i = 0; i < kMaxWatchlistEntries; ++i) {
    full.emplace_back("entry" + std::to_string(i));
  }
  CHECK(check_new_entry("Vexmoor Dalquist", full) == EntryError::ListFull);
}

TEST_CASE("the watch-list file format round-trips any entry", "[watchlist]") {
  const std::vector<Secret> entries =
      secrets({"Zorblax Quentivar", "back\\slash \\n literal", "line\nbreak", "carriage\rreturn",
               "\xC3\x89lodie Qu\xC3\xA9tzal", "#not a comment", " leading and trailing "});
  const std::string body{encode_watchlist(entries)};
  CHECK(std::count(body.begin(), body.end(), '\n') == static_cast<std::ptrdiff_t>(entries.size() + 1));
  CHECK(body.find('\r') == std::string::npos);
  const auto decoded = decode_watchlist(body);
  REQUIRE(decoded.has_value());
  CHECK(*decoded == entries);

  const auto empty = decode_watchlist(encode_watchlist({}));
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
}

TEST_CASE("anything but a well-formed v1 file is rejected", "[watchlist]") {
  const std::string header{encode_watchlist({})};
  CHECK_FALSE(decode_watchlist("").has_value());
  CHECK_FALSE(decode_watchlist("Zorblax Quentivar\n").has_value());
  CHECK_FALSE(decode_watchlist(header.substr(0, header.size() - 1)).has_value());  // no newline after header
  CHECK_FALSE(decode_watchlist("spg-watchlist 2\nZorblax\n").has_value());
  CHECK_FALSE(decode_watchlist(header + "Zorblax").has_value());                   // cut off
  CHECK_FALSE(decode_watchlist(header + "\n").has_value());                        // empty entry
  CHECK_FALSE(decode_watchlist(header + "   \n").has_value());                     // blank entry
  CHECK_FALSE(decode_watchlist(header + "Zor\\tblax\n").has_value());              // unknown escape
  CHECK_FALSE(decode_watchlist(header + "Zorblax\\\n").has_value());               // dangling escape
  CHECK_FALSE(decode_watchlist(header + "Zorblax\r\n").has_value());               // raw CR
  CHECK_FALSE(decode_watchlist(header + std::string(kMaxEntryBytes + 1, 'q') + "\n").has_value());
  CHECK(decode_watchlist(header + std::string(kMaxEntryBytes, 'q') + "\n").has_value());

  std::string many = header;
  for (std::size_t i = 0; i <= kMaxWatchlistEntries; ++i) {
    many += "entry" + std::to_string(i) + "\n";
  }
  CHECK_FALSE(decode_watchlist(many).has_value());
}

TEST_CASE("the store saves, replaces and loads a user-only file", "[watchlist]") {
  const TempDir temp;
  const fs::path dir = temp.path() / "obs-spg" / "watchlists";
  const WatchlistStore store{dir, kRef};
  REQUIRE(store.file() == dir / (std::string(kRef) + ".txt"));

  SECTION("a new filter starts with an empty list") {
    const WatchlistLoad loaded = store.load(0);
    CHECK(loaded.error == StoreError::None);
    CHECK(loaded.entries.empty());
    CHECK_FALSE(fs::exists(dir));  // loading never creates anything
  }
  SECTION("a missing file with recorded entries fails closed") {
    CHECK(store.load(3).error == StoreError::Missing);
  }
  SECTION("save then load") {
    // Each save leaves a user-only file in a user-only directory, checked
    // before any load (load also tightens permissions).
    const std::vector<Secret> first = secrets({"Zorblax Quentivar", "Vexmoor Dalquist"});
    REQUIRE(store.save(first) == StoreError::None);
    CHECK(is_user_only(store.file(), fs::perms::owner_read | fs::perms::owner_write));
    CHECK(is_user_only(dir, fs::perms::owner_all));
    WatchlistLoad loaded = store.load(2);
    CHECK(loaded.error == StoreError::None);
    CHECK(loaded.entries == first);

    const std::vector<Secret> second = secrets({"Vexmoor Dalquist"});
    REQUIRE(store.save(second) == StoreError::None);  // replaces the existing file
    CHECK(is_user_only(store.file(), fs::perms::owner_read | fs::perms::owner_write));
    loaded = store.load(2);  // a stale count is fine when the file is there
    CHECK(loaded.error == StoreError::None);
    CHECK(loaded.entries == second);

    // Only the watch-list file remains; no temporary file is left behind.
    std::vector<fs::path> files;
    for (const auto &item : fs::directory_iterator(dir)) {
      files.push_back(item.path());
    }
    CHECK(files == std::vector<fs::path>{store.file()});

#ifndef _WIN32
    // A file and directory someone made readable by others are tightened on
    // the next load.
    fs::permissions(store.file(), fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                                      fs::perms::others_read);
    fs::permissions(dir, fs::perms::owner_all | fs::perms::others_read | fs::perms::others_exec);
    CHECK(store.load(1).error == StoreError::None);
    CHECK(is_user_only(store.file(), fs::perms::owner_read | fs::perms::owner_write));
    CHECK(is_user_only(dir, fs::perms::owner_all));
#endif
  }
  SECTION("a damaged file is corrupt, not empty") {
    fs::create_directories(dir);
    write_file(store.file(), "Zorblax Quentivar\n");
    CHECK(store.load(0).error == StoreError::Corrupt);
    write_file(store.file(), std::string{encode_watchlist({})} + std::string(1 << 20, 'q') + "\n");
    CHECK(store.load(0).error == StoreError::Corrupt);
  }
  SECTION("a directory where the file should be is unreadable") {
    fs::create_directories(store.file());
    CHECK(store.load(0).error == StoreError::Unreadable);
    CHECK(store.save({}) == StoreError::WriteFailed);
  }
}

TEST_CASE("a bad reference or config directory never reaches the file system", "[watchlist]") {
  const TempDir temp;
  for (const char *ref : {"", "../escape", "0123456789ABCDEF0123456789ABCDEF"}) {
    const WatchlistStore store{temp.path() / "watchlists", ref};
    CHECK(store.file().empty());
    CHECK(store.load(0).error == StoreError::BadReference);
    CHECK(store.save(secrets({"Zorblax Quentivar"})) == StoreError::BadReference);
  }
  const WatchlistStore relative{fs::path("obs-spg") / "watchlists", kRef};
  CHECK(relative.load(0).error == StoreError::NoConfigDir);
  CHECK(relative.save({}) == StoreError::NoConfigDir);
  CHECK(fs::is_empty(temp.path()));
}

TEST_CASE("UTF-8 config paths keep their characters", "[watchlist]") {
  const fs::path path = path_from_utf8("/tmp/\xC3\x89lodie/obs");
  CHECK(path.u8string() == u8"/tmp/\u00C9lodie/obs");
}

// Sentinel: a synthetic entry goes through every store and settings path,
// including each failure, while std::cout, std::cerr and std::clog are
// captured. No output and no error description may contain any part of it.
TEST_CASE("watch-list text never reaches output or error descriptions", "[watchlist][sentinel]") {
  const std::string sentinel = "Qorvexian Blathermoor 7319";
  const std::vector<std::string> fragments = {"Qorvexian", "qorvexian", "QORVEXIAN", "Blathermoor",
                                              "blathermoor", "lathermo", "7319", "qorvexlan"};

  std::vector<std::string> messages;
  const TempDir temp;
  const fs::path dir = temp.path() / "watchlists";
  CaptureStandardStreams captured;
  {
    const WatchlistStore store{dir, kRef};
    const std::vector<Secret> entries = {Secret{sentinel}, Secret{"Vexmoor Dalquist"}};
    messages.emplace_back(describe(store.save(entries)));
    const WatchlistLoad loaded = store.load(2);
    messages.emplace_back(describe(loaded.error));
    CHECK(loaded.entries == entries);

    // Duplicate and too-long checks on the sentinel.
    CHECK(check_new_entry(sentinel, entries) == EntryError::Duplicate);
    CHECK(check_new_entry(sentinel + std::string(kMaxEntryBytes, 'x'), entries) == EntryError::TooLong);

    // Corrupt files that contain the sentinel.
    write_file(store.file(), sentinel + "\n");
    messages.emplace_back(describe(store.load(1).error));
    write_file(store.file(), std::string{encode_watchlist({})} + sentinel + "\\q\n");
    messages.emplace_back(describe(store.load(1).error));

    // Bad references, including one made from the sentinel.
    const WatchlistStore bad{dir, sentinel};
    messages.emplace_back(describe(bad.load(1).error));
    messages.emplace_back(describe(bad.save(entries)));

    // Settings paths: accepted, all categories off, and rejected by spg-core.
    FilterSettings settings;
    std::optional<PolicyConfig> config;
    CHECK(make_policy_config(settings, entries, config) == FilterIssue::None);
    settings.categories = CategorySet{};
    CHECK(make_policy_config(settings, entries, config) == FilterIssue::NoCategories);
    settings.categories = CategorySet::all();
    CHECK(make_policy_config(settings, {Secret{sentinel}, Secret{"%%%"}}, config) == FilterIssue::InvalidConfig);

    // The store does write the sentinel, and only to its own file.
    REQUIRE(store.save(entries) == StoreError::None);
    CHECK(read_file(store.file()).find(sentinel) != std::string::npos);
    std::size_t files = 0;
    for (const auto &item : fs::recursive_directory_iterator(temp.path())) {
      if (item.is_regular_file()) {
        ++files;
        CHECK(item.path() == store.file());
      }
    }
    CHECK(files == 1);
  }
  for (const StoreError error : {StoreError::None, StoreError::NoConfigDir, StoreError::BadReference,
                                 StoreError::Missing, StoreError::Unreadable, StoreError::Corrupt,
                                 StoreError::WriteFailed}) {
    messages.emplace_back(describe(error));
  }

  messages.push_back(captured.release());
  for (const std::string &message : messages) {
    for (const std::string &fragment : fragments) {
      INFO("fragment " << fragment.size() << " chars");
      CHECK(message.find(fragment) == std::string::npos);
    }
  }
}
