// Folding and the watch-list matching form (docs/policy.md).
#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "../contracts/utf8.hpp"
#include "spg/policy.hpp"
#include "text.hpp"

namespace spg {
namespace detail {
namespace {

struct FoldEntry {
  char32_t first;
  char32_t last;
  std::u32string_view out;
};

constexpr FoldEntry kFoldTable[] = {
#include "fold_table.inc"
};

struct CodeRange {
  char32_t first;
  char32_t last;
};

// Non-ASCII punctuation, symbols and separators of every script.
constexpr CodeRange kSeparatorTable[] = {
#include "separator_table.inc"
};

bool in_separator_table(char32_t cp) noexcept {
  const auto* it = std::upper_bound(std::begin(kSeparatorTable), std::end(kSeparatorTable), cp,
                                    [](char32_t value, const CodeRange& r) { return value < r.first; });
  return it != std::begin(kSeparatorTable) && cp <= std::prev(it)->last;
}

const FoldEntry* find_fold(char32_t cp) noexcept {
  const auto* it = std::upper_bound(std::begin(kFoldTable), std::end(kFoldTable), cp,
                                    [](char32_t value, const FoldEntry& e) { return value < e.first; });
  if (it == std::begin(kFoldTable)) {
    return nullptr;
  }
  --it;
  return cp <= it->last ? it : nullptr;
}

bool is_deleted(char32_t cp) noexcept {
  return cp == 0xAD || (cp >= 0x200B && cp <= 0x200D) || cp == 0x2060 || cp == 0xFEFF;
}

bool is_control(char32_t cp) noexcept { return cp < 0x20 || (cp >= 0x7F && cp < 0xA0); }

// Appends c, keeping at most one space in a row and none at the start.
void push(std::u32string& out, char32_t c) {
  if (c == U' ' && (out.empty() || out.back() == U' ')) {
    return;
  }
  out.push_back(c);
}

char32_t merge_confusion(char32_t c) noexcept {
  switch (c) {
    case U'0':
      return U'o';
    case U'1':
    case U'i':
    case U'|':
      return U'l';
    case U'5':
      return U's';
    case U'8':
      return U'b';
    default:
      return c;
  }
}

}  // namespace

std::u32string fold_text(std::string_view utf8) {
  std::u32string out;
  out.reserve(utf8.size());
  while (!utf8.empty()) {
    char32_t cp = 0xFFFD;
    if (const std::optional<char32_t> decoded = pop_utf8(utf8)) {
      cp = *decoded;
    } else {
      utf8.remove_prefix(1);
    }
    if (is_deleted(cp)) {
      continue;
    }
    if (renders_blank(cp) || is_control(cp)) {
      push(out, U' ');
    } else if (cp >= U'A' && cp <= U'Z') {
      out.push_back(cp - U'A' + U'a');
    } else if (const FoldEntry* entry = cp >= 0xA0 ? find_fold(cp) : nullptr) {
      if (entry->out.size() == 1) {
        push(out, entry->out.front() + (cp - entry->first));
      } else {
        for (const char32_t c : entry->out) {
          push(out, c);
        }
      }
    } else {
      out.push_back(cp);
    }
  }
  if (!out.empty() && out.back() == U' ') {
    out.pop_back();
  }
  return out;
}

bool is_separator(char32_t c) noexcept {
  if (c < 0x80) {
    return !((c >= U'a' && c <= U'z') || (c >= U'0' && c <= U'9'));
  }
  return renders_blank(c) || is_control(c) || (c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7 ||
         (c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x303F) || c == 0xFFFD ||
         in_separator_table(c);
}

std::u32string matching_form(std::u32string_view folded) {
  std::u32string out;
  out.reserve(folded.size());
  for (const char32_t raw : folded) {
    const char32_t c = merge_confusion(raw);
    push(out, is_separator(c) ? U' ' : c);
  }
  if (!out.empty() && out.back() == U' ') {
    out.pop_back();
  }
  return out;
}

std::string to_utf8(std::u32string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char32_t c : text) {
    append_utf8(out, c);
  }
  return out;
}

}  // namespace detail

Secret normalize_for_matching(std::string_view utf8) {
  return Secret(detail::to_utf8(detail::matching_form(detail::fold_text(utf8))));
}

}  // namespace spg
