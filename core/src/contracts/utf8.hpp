// UTF-8 helpers shared by contract validation and the policy engine. Internal
// to spg-core.
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace spg::detail {

// Code points that render as nothing in a settings field: the Unicode
// White_Space property plus the zero-width characters U+200B..U+200D, U+2060
// and U+FEFF.
[[nodiscard]] inline bool renders_blank(char32_t cp) noexcept {
  return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200D) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
         cp == 0x205F || cp == 0x2060 || cp == 0x3000 || cp == 0xFEFF;
}

// Decodes one UTF-8 sequence at the front of non-empty `text` and removes it.
// Returns nothing, and leaves `text` unchanged, for a malformed sequence.
[[nodiscard]] inline std::optional<char32_t> pop_utf8(std::string_view& text) noexcept {
  const auto lead = static_cast<unsigned char>(text.front());
  std::size_t length = 0;
  char32_t cp = 0;
  if (lead < 0x80) {
    length = 1;
    cp = lead;
  } else if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    cp = lead & 0x1FU;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    cp = lead & 0x0FU;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    cp = lead & 0x07U;
  } else {
    return std::nullopt;
  }
  if (text.size() < length) {
    return std::nullopt;
  }
  for (std::size_t i = 1; i < length; ++i) {
    const auto next = static_cast<unsigned char>(text[i]);
    if ((next & 0xC0U) != 0x80U) {
      return std::nullopt;
    }
    cp = (cp << 6U) | (next & 0x3FU);
  }
  // Reject overlong forms, surrogates and values past U+10FFFF.
  constexpr std::array<char32_t, 5> kMinForLength = {0, 0, 0x80, 0x800, 0x10000};
  if (cp < kMinForLength[length] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
    return std::nullopt;
  }
  text.remove_prefix(length);
  return cp;
}

// Appends the UTF-8 encoding of a valid code point to a std::string or any
// other basic_string of char (the policy engine's SecureString).
template <class String>
void append_utf8(String& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  }
}

}  // namespace spg::detail
