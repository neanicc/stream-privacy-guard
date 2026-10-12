// PolicyEngine: compiles a PolicyConfig and turns recognized text into a
// Verdict. Nothing here writes watch-list or recognized text anywhere: log
// lines and errors carry counts, indices and reason codes only.
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "spg/error.hpp"
#include "spg/policy.hpp"
#include "text.hpp"

namespace spg {

std::string describe(const Verdict& verdict) {
  std::string out;
  for (const Category c : kAllCategories) {
    if (verdict.reasons.contains(c)) {
      if (!out.empty()) {
        out += ',';
      }
      out += reason_code(c);
    }
  }
  return out;
}

PolicyEngine::PolicyEngine(const PolicyConfig& config, const LogSink& log)
    : categories_(config.categories), fuzzy_(config.fuzzy) {
  validate(config);
  std::size_t whole_word_only = 0;
  watchlist_.reserve(config.watchlist.size());
  for (std::size_t i = 0; i < config.watchlist.size(); ++i) {
    SecureU32String form = detail::matching_form(detail::fold_text(config.watchlist[i].reveal()));
    if (form.empty()) {
      throw ContractError("policy config: watchlist entry " + std::to_string(i) +
                          " has no letters or digits once normalized");
    }
    if (form.size() < fuzzy_.min_partial_chars) {
      ++whole_word_only;
    }
    watchlist_.push_back(std::move(form));
  }
  if (log) {
    const std::string enabled = describe(Verdict{categories_});
    log("policy: " + std::to_string(watchlist_.size()) + " watch-list entries, " +
        std::to_string(whole_word_only) + " match whole words only; enabled: " +
        (enabled.empty() ? std::string("none") : enabled));
    if (!watchlist_.empty() && !categories_.contains(Category::Watchlist)) {
      log("policy: the watchlist category is disabled; watch-list entries are ignored");
    }
  }
}

Verdict PolicyEngine::evaluate(const Secret& text) const {
  const SecureU32String folded = detail::fold_text(text.reveal());
  Verdict verdict{detail::find_pii_folded(folded, categories_)};
  if (categories_.contains(Category::Watchlist) && !watchlist_.empty()) {
    const SecureU32String form = detail::matching_form(folded);
    const SecureU32String reversed(form.rbegin(), form.rend());
    for (const SecureU32String& entry : watchlist_) {
      if (detail::watchlist_match(entry, form, reversed, fuzzy_)) {
        verdict.reasons.insert(Category::Watchlist);
        break;
      }
    }
  }
  return verdict;
}

}  // namespace spg
