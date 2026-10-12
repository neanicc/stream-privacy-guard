#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "spg/category.hpp"
#include "spg/detection.hpp"
#include "spg/policy_config.hpp"
#include "spg/secret.hpp"
#include "spg/secure_memory.hpp"

// The text policy (SPG-08): decides whether recognized text is sensitive and
// why. Rules are in docs/policy.md.
namespace spg {

// Receives diagnostic lines. Core components write counts, indices and reason
// codes to it, never watch-list entries or recognized text. The plugin and the
// CLI connect it to their own logs.
using LogSink = std::function<void(std::string_view line)>;

// The form watch-list entries and recognized text are compared in: NFKC-style
// compatibility folding, case folding, diacritics removed, zero-width
// characters deleted, OCR confusions merged (0/o, 1/l/i/|, 5/s, 8/b) and every
// run of whitespace or punctuation turned into one space. The result is still
// secret text.
[[nodiscard]] Secret normalize_for_matching(std::string_view utf8);

// The structured PII categories among `enabled` that the UTF-8 text contains:
// emails, NANP or E.164 phone numbers, Luhn-valid card numbers, IPv4 and IPv6
// addresses, street addresses and known API token formats.
[[nodiscard]] CategorySet find_pii(std::string_view utf8, CategorySet enabled);

// True when `digits` is non-empty, holds only ASCII digits and passes the
// Luhn checksum.
[[nodiscard]] bool luhn_valid(std::string_view digits) noexcept;

// The verdict's reason codes joined by ',' in Category order, for the debug
// overlay and logs ("WATCHLIST,PII_CARD"); empty when nothing matched.
[[nodiscard]] std::string describe(const Verdict& verdict);

// Evaluates text against one PolicyConfig. Construction validates the config
// and compiles the watch-list; evaluate() is const and keeps no state, so one
// engine can serve several worker threads.
class PolicyEngine {
 public:
  // Throws ContractError when validate(config) fails or when a watch-list
  // entry has no letters or digits once normalized (it could never match).
  // Errors and log lines name the entry's index, never its text.
  explicit PolicyEngine(const PolicyConfig& config, const LogSink& log = {});

  // Reasons the text is sensitive, limited to the enabled categories. Never
  // includes Face. Any byte string is accepted; invalid UTF-8 reads as a
  // separator.
  [[nodiscard]] Verdict evaluate(const Secret& text) const;

  // Sets hit.verdict from hit.text.
  void apply(TextHit& hit) const { hit.verdict = evaluate(hit.text); }

 private:
  CategorySet categories_;
  FuzzyLimits fuzzy_;
  // Normalized entries, in protected memory like the Secrets they come from.
  std::vector<SecureU32String, SecureAllocator<SecureU32String>> watchlist_;
};

}  // namespace spg
