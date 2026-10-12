// Text forms shared by the policy engine's parts. Internal to spg-core; the
// rules are documented in docs/policy.md.
#pragma once

#include <string>
#include <string_view>

#include "spg/category.hpp"
#include "spg/policy_config.hpp"
#include "spg/secure_memory.hpp"

namespace spg::detail {

// Folds UTF-8 text for every policy check: compatibility forms, case and
// diacritics (fold_table.inc), zero-width characters and soft hyphens deleted,
// control characters and Unicode whitespace turned into one space, leading and
// trailing space trimmed. A malformed UTF-8 byte becomes U+FFFD. The text
// forms below are secret, so they live in SecureHeap (spg/secure_memory.hpp).
[[nodiscard]] SecureU32String fold_text(std::string_view utf8);

// True for a folded character that separates words in matching_form():
// anything in ASCII but a-z and 0-9, Unicode whitespace, punctuation and
// symbols of any script (separator_table.inc), Latin-1 and General
// Punctuation, CJK symbols and punctuation, and U+FFFD.
[[nodiscard]] bool is_separator(char32_t c) noexcept;

// Watch-list form of folded text: OCR confusions merged (0->o, 1/i/|->l,
// 5->s, 8->b), then each run of separators turned into one space, trimmed.
[[nodiscard]] SecureU32String matching_form(std::u32string_view folded);

[[nodiscard]] SecureString to_utf8(std::u32string_view text);

// True when the watch-list entry (in matching form) occurs in the text (in
// matching form, `reversed` being the same text reversed) under the fuzzy
// rules in docs/policy.md.
[[nodiscard]] bool watchlist_match(std::u32string_view entry, std::u32string_view text,
                                   std::u32string_view reversed, const FuzzyLimits& limits);

// The PII categories among `enabled` found in folded text.
[[nodiscard]] CategorySet find_pii_folded(std::u32string_view folded, CategorySet enabled);

}  // namespace spg::detail
