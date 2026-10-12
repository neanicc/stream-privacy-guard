// Sentinel test (plan section 8): a synthetic watch-list entry is pushed
// through every policy path - config errors, engine construction, matching,
// PII scanning, verdicts and their debug text - while the engine's log sink
// and the standard streams are captured. No captured byte may contain any
// recognizable part of the entry, raw or normalized.
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "spg/category.hpp"
#include "spg/detection.hpp"
#include "spg/error.hpp"
#include "spg/policy.hpp"
#include "spg/policy_config.hpp"
#include "spg/secret.hpp"

namespace {

constexpr std::string_view kSentinel = "zq-SENTINEL-7f3a";

// A verdict is a set of reason codes and has no room for text.
static_assert(std::is_trivially_copyable_v<spg::Verdict>);
static_assert(sizeof(spg::Verdict) == sizeof(spg::CategorySet));

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Fragments of the sentinel, of its normalized form ("zq sentlnel 7f3a") and of
// its OCR-noisy variants used below. Checked case-insensitively.
const std::vector<std::string> kForbidden = {"sentinel", "sentlnel", "5ent", "zq-", "zq s", "7f3a"};

bool leaks(std::string_view captured) {
  const std::string text = lower(captured);
  return std::any_of(kForbidden.begin(), kForbidden.end(),
                     [&](const std::string& f) { return text.find(f) != std::string::npos; });
}

// Redirects std::cout, std::cerr and std::clog into one buffer while alive.
class StreamCapture {
 public:
  StreamCapture()
      : out_(std::cout.rdbuf(buffer_.rdbuf())),
        err_(std::cerr.rdbuf(buffer_.rdbuf())),
        log_(std::clog.rdbuf(buffer_.rdbuf())) {}
  ~StreamCapture() {
    std::cout.rdbuf(out_);
    std::cerr.rdbuf(err_);
    std::clog.rdbuf(log_);
  }
  StreamCapture(const StreamCapture&) = delete;
  StreamCapture& operator=(const StreamCapture&) = delete;
  [[nodiscard]] std::string text() const { return buffer_.str(); }

 private:
  std::ostringstream buffer_;
  std::streambuf* out_;
  std::streambuf* err_;
  std::streambuf* log_;
};

// The ContractError message from building an engine, or "" when none is thrown.
std::string error_of(const spg::PolicyConfig& config, const spg::LogSink& sink) {
  try {
    const spg::PolicyEngine engine(config, sink);
  } catch (const spg::ContractError& e) {
    return e.what();
  }
  return {};
}

}  // namespace

TEST_CASE("policy sentinel: watch-list text never reaches logs, streams, errors or verdicts",
          "[policy][sentinel]") {
  spg::PolicyConfig config;
  config.watchlist.emplace_back(std::string(kSentinel));
  config.watchlist.emplace_back("Vex");  // a short entry, so the engine logs a count
  spg::PolicyConfig blank = config;      // errors name index 2, next to the sentinel
  blank.watchlist.emplace_back("\t \xE2\x80\x8B");
  spg::PolicyConfig unmatchable = config;
  unmatchable.watchlist.emplace_back("-- !! --");
  spg::PolicyConfig bad_ratio = config;
  bad_ratio.fuzzy.max_edit_ratio = 2.0;
  const std::vector<std::string> texts = {
      std::string(kSentinel),
      "chat: " + std::string(kSentinel) + " joined",
      "ZQ SENT1NEL 7F3A",  // OCR noise
      "zq-SENTIN",         // partial reveal
      "NEL-7f3a",          // suffix reveal
      std::string(kSentinel) + " 4111 1111 1111 1111 zq@example.com",
      "zq-SENTINEL-\xC3\x28-7f3a",       // malformed UTF-8
      "zq-SENTINEL-\xEF\xBC\x97" "f3a",  // fullwidth 7
  };

  std::vector<std::string> log_lines;
  const spg::LogSink sink = [&](std::string_view line) { log_lines.emplace_back(line); };
  std::vector<spg::Verdict> verdicts;
  std::vector<std::string> errors;
  std::string streamed;
  {
    // Catch2 reports through std::cout, so no assertion runs inside this block.
    const StreamCapture streams;
    const spg::PolicyEngine engine(config, sink);
    for (const std::string& text : texts) {
      spg::TextHit hit;
      hit.detection.kind = spg::DetectionKind::TextRegion;
      hit.text = spg::Secret(text);
      engine.apply(hit);
      verdicts.push_back(hit.verdict);
    }
    for (const spg::PolicyConfig* bad : {&blank, &unmatchable, &bad_ratio}) {
      errors.push_back(error_of(*bad, sink));
    }
    streamed = streams.text();
  }

  // Matching really saw the sentinel: every text is a watch-list hit.
  for (std::size_t i = 0; i < verdicts.size(); ++i) {
    INFO("text index " << i);
    CHECK(verdicts[i].reasons.contains(spg::Category::Watchlist));
    CHECK_FALSE(leaks(spg::describe(verdicts[i])));
  }
  REQUIRE_FALSE(log_lines.empty());  // the sink was exercised
  for (const std::string& line : log_lines) {
    INFO("log: " << line);
    CHECK_FALSE(leaks(line));
  }
  REQUIRE(errors.size() == 3);
  for (const std::string& error : errors) {
    INFO("error: " << error);
    CHECK((error.rfind("policy config: watchlist entry 2 ", 0) == 0 ||
           error.rfind("policy config: fuzzy.", 0) == 0));
    CHECK_FALSE(leaks(error));
  }
  CHECK(streamed.empty());
  // The check itself would catch a leak of the raw or normalized entry.
  CHECK(leaks(kSentinel));
  CHECK(leaks(spg::normalize_for_matching(kSentinel).reveal()));
}
