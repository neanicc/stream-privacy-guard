// Fuzzy watch-list matching over token windows (docs/policy.md).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "text.hpp"

namespace spg::detail {
namespace {

// Allowed edit distance for n normalized characters:
// min(max_edits, floor(max_edit_ratio * n)). The epsilon keeps a product that
// should be whole (0.29 * 100) from flooring one below.
std::size_t allowed_edits(std::size_t n, const FuzzyLimits& limits) {
  const double by_ratio = std::floor(limits.max_edit_ratio * static_cast<double>(n) + 1e-9);
  return std::min<std::size_t>(limits.max_edits, static_cast<std::size_t>(by_ratio));
}

bool word_start(std::u32string_view text, std::size_t j) noexcept {
  return j == 0 || text[j - 1] == U' ';
}

bool word_end(std::u32string_view text, std::size_t j) noexcept {
  return j == text.size() || text[j] == U' ';
}

struct Window {
  std::size_t min_prefix;  // shortest prefix of the entry that counts
  bool start_at_word;      // the matched text must begin at a word start
  bool end_at_word;        // ... and end at a word end
};

// True when some prefix of `entry` of at least window.min_prefix characters
// is within allowed_edits(prefix length) of a substring of `text`. Uses the
// approximate substring recurrence (Sellers): row i holds, for each end
// position j, the fewest edits turning entry[0, i) into text ending at j.
bool prefix_within_edits(std::u32string_view entry, std::u32string_view text,
                         const FuzzyLimits& limits, const Window& window) {
  constexpr std::size_t kFar = std::numeric_limits<std::size_t>::max() / 4;
  const std::size_t m = text.size();
  std::vector<std::size_t> prev(m + 1);
  std::vector<std::size_t> cur(m + 1);
  for (std::size_t j = 0; j <= m; ++j) {
    prev[j] = !window.start_at_word || word_start(text, j) ? 0 : kFar;
  }
  for (std::size_t i = 1; i <= entry.size(); ++i) {
    cur[0] = prev[0] + 1;
    std::size_t row_min = cur[0];
    for (std::size_t j = 1; j <= m; ++j) {
      const std::size_t substitute = prev[j - 1] + (entry[i - 1] == text[j - 1] ? 0 : 1);
      cur[j] = std::min({substitute, prev[j] + 1, cur[j - 1] + 1});
      row_min = std::min(row_min, cur[j]);
    }
    if (i >= window.min_prefix) {
      const std::size_t allowed = allowed_edits(i, limits);
      for (std::size_t j = 0; j <= m && row_min <= allowed; ++j) {
        if (cur[j] <= allowed && (!window.end_at_word || word_end(text, j))) {
          return true;
        }
      }
    }
    // A row's minimum never decreases, so no longer prefix can match.
    if (row_min > limits.max_edits) {
      return false;
    }
    std::swap(prev, cur);
  }
  return false;
}

}  // namespace

bool watchlist_match(std::u32string_view entry, std::u32string_view text,
                     std::u32string_view reversed, const FuzzyLimits& limits) {
  const std::size_t n = entry.size();
  const std::size_t partial = limits.min_partial_chars;
  if (n == 0) {
    return false;
  }
  // Short entries match whole words only, so "al" cannot hit "always".
  if (n < partial) {
    return prefix_within_edits(entry, text, limits, {n, true, true});
  }
  // The whole entry anywhere, even inside a longer word ("xxjohnsmithxx").
  if (prefix_within_edits(entry, text, limits, {n, false, false})) {
    return true;
  }
  if (n == partial) {
    return false;
  }
  // A partial reveal: a prefix starting at a word start ("123 mapl"), or a
  // suffix ending at a word end (the prefix of the reversed entry).
  if (prefix_within_edits(entry, text, limits, {partial, true, false})) {
    return true;
  }
  const SecureU32String reversed_entry(entry.rbegin(), entry.rend());
  return prefix_within_edits(reversed_entry, reversed, limits, {partial, true, false});
}

}  // namespace spg::detail
