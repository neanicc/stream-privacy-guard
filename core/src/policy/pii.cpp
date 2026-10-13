// Structured PII validators (docs/policy.md). All work on folded text, so
// letters are lower case and fullwidth or styled digits are ASCII.
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "spg/policy.hpp"
#include "text.hpp"

namespace spg {
namespace detail {
namespace {

using Text = std::u32string_view;

bool is_digit(char32_t c) noexcept { return c >= U'0' && c <= U'9'; }
bool is_lower(char32_t c) noexcept { return c >= U'a' && c <= U'z'; }
bool is_alnum(char32_t c) noexcept { return is_digit(c) || is_lower(c); }
bool is_hex(char32_t c) noexcept { return is_digit(c) || (c >= U'a' && c <= U'f'); }
// A letter or digit of any script (a non-ASCII character that is not a separator).
bool is_word(char32_t c) noexcept { return is_alnum(c) || (c >= 0x80 && !is_separator(c)); }

template <typename Char>
bool luhn(std::basic_string_view<Char> digits) noexcept {
  if (digits.empty()) {
    return false;
  }
  unsigned sum = 0;
  bool twice = false;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    const auto c = static_cast<char32_t>(static_cast<std::make_unsigned_t<Char>>(*it));
    if (!is_digit(c)) {
      return false;
    }
    unsigned d = static_cast<unsigned>(c - U'0');
    if (twice) {
      d = d * 2 > 9 ? d * 2 - 9 : d * 2;
    }
    sum += d;
    twice = !twice;
  }
  return sum % 10 == 0;
}

// OCR reads digits as look-alike letters. In the digit view, o reads as 0 and
// l, i or | as 1 when a neighbouring character is a digit.
SecureU32String digit_view(Text t) {
  SecureU32String d(t);
  for (std::size_t i = 0; i < t.size(); ++i) {
    const bool near_digit = (i > 0 && is_digit(t[i - 1])) || (i + 1 < t.size() && is_digit(t[i + 1]));
    if (!near_digit) {
      continue;
    }
    if (t[i] == U'o') {
      d[i] = U'0';
    } else if (t[i] == U'l' || t[i] == U'i' || t[i] == U'|') {
      d[i] = U'1';
    }
  }
  return d;
}

// E.164: '+', then 8 to 15 digits; a country code never starts with 0.
bool e164(Text s) noexcept { return s.size() >= 8 && s.size() <= 15 && s[0] != U'0'; }

// NANP: NXX-NXX-XXXX with area code and exchange starting with 2-9, optionally
// after a leading 1. `cuts` are the offsets where digit groups meet; they may
// only fall after the 1, the area code or the exchange, so a date and time
// ("2026-10-12 12") is not a phone number.
bool nanp(Text s, const std::vector<std::size_t>& cuts) noexcept {
  const std::size_t lead = s.size() == 11 && s[0] == U'1' ? 1 : 0;
  if (s.size() - lead != 10 || s[lead] < U'2' || s[lead + 3] < U'2') {
    return false;
  }
  return std::all_of(cuts.begin(), cuts.end(), [lead](std::size_t cut) {
    return (lead == 1 && cut == 1) || cut == lead + 3 || cut == lead + 6;
  });
}

// True when the digit group at `begin` follows a '+', allowing up to three
// spaces or opening parentheses between them: "+44", "+ 44", "+(44)", "+ (44)".
bool after_plus(Text d, std::size_t begin) noexcept {
  for (std::size_t k = begin; k > 0 && begin - k <= 3; --k) {
    if (d[k - 1] == U'+') {
      return true;
    }
    if (d[k - 1] != U' ' && d[k - 1] != U'(') {
      return false;
    }
  }
  return false;
}

bool number_punct(char32_t c) noexcept {
  return c == U' ' || c == U'-' || c == U'.' || c == U'(' || c == U')' || c == U'+';
}

// Phone and card numbers: digit groups joined by at most three of
// " -.()+". Every run of consecutive whole groups is checked, so two numbers
// on one line are both found and a group is never split.
void scan_numbers(Text d, CategorySet enabled, CategorySet& found) {
  struct Group {
    std::size_t begin;
    std::size_t end;
  };
  const std::size_t n = d.size();
  std::size_t i = 0;
  while (i < n) {
    if (!is_digit(d[i])) {
      ++i;
      continue;
    }
    std::vector<Group> groups;
    std::size_t j = i;
    while (true) {
      const std::size_t begin = j;
      while (j < n && is_digit(d[j])) {
        ++j;
      }
      groups.push_back({begin, j});
      std::size_t gap = j;
      while (gap < n && gap - j < 4 && number_punct(d[gap])) {
        ++gap;
      }
      if (gap == j || gap - j > 3 || gap == n || !is_digit(d[gap])) {
        break;
      }
      j = gap;
    }
    for (std::size_t a = 0; a < groups.size(); ++a) {
      const bool plus = after_plus(d, groups[a].begin);
      SecureU32String digits;
      std::vector<std::size_t> cuts;
      for (std::size_t b = a; b < groups.size() && digits.size() <= 19; ++b) {
        if (b > a) {
          cuts.push_back(digits.size());
        }
        digits.append(d.substr(groups[b].begin, groups[b].end - groups[b].begin));
        if (enabled.contains(Category::PiiPhone) && (plus ? e164(digits) : nanp(digits, cuts))) {
          found.insert(Category::PiiPhone);
        }
        if (enabled.contains(Category::PiiCard) && digits.size() >= 13 && digits.size() <= 19 &&
            luhn(Text(digits))) {
          found.insert(Category::PiiCard);
        }
      }
    }
    i = j;
  }
}

std::vector<Text> split(Text s, char32_t sep) {
  std::vector<Text> parts;
  std::size_t start = 0;
  for (std::size_t k = 0; k <= s.size(); ++k) {
    if (k == s.size() || s[k] == sep) {
      parts.push_back(s.substr(start, k - start));
      start = k + 1;
    }
  }
  return parts;
}

bool octet(Text p) noexcept {
  if (p.empty() || p.size() > 3 || !std::all_of(p.begin(), p.end(), is_digit)) {
    return false;
  }
  unsigned value = 0;
  for (const char32_t c : p) {
    value = value * 10 + static_cast<unsigned>(c - U'0');
  }
  return value <= 255;
}

// Any four consecutive dot-separated octets in a run of digits and dots.
bool has_ipv4(Text run) {
  const std::vector<Text> parts = split(run, U'.');
  std::size_t streak = 0;
  for (const Text p : parts) {
    streak = octet(p) ? streak + 1 : 0;
    if (streak == 4) {
      return true;
    }
  }
  return false;
}

// Counts the 16-bit groups of one side of an IPv6 address; false on a bad group.
bool ipv6_groups(Text side, bool may_end_ipv4, std::size_t& groups) {
  if (side.empty()) {
    return true;
  }
  const std::vector<Text> pieces = split(side, U':');
  for (std::size_t k = 0; k < pieces.size(); ++k) {
    const Text p = pieces[k];
    if (may_end_ipv4 && k + 1 == pieces.size() && p.find(U'.') != Text::npos) {
      const std::vector<Text> octets = split(p, U'.');
      if (octets.size() != 4 || !std::all_of(octets.begin(), octets.end(), octet)) {
        return false;
      }
      groups += 2;
    } else if (p.empty() || p.size() > 4 || !std::all_of(p.begin(), p.end(), is_hex)) {
      return false;
    } else {
      groups += 1;
    }
  }
  return true;
}

// RFC 4291 text forms: eight groups, or at least two groups around one "::".
bool ipv6(Text s) {
  const std::size_t gap = s.find(U"::");
  std::size_t groups = 0;
  if (gap == Text::npos) {
    return ipv6_groups(s, true, groups) && groups == 8;
  }
  if (s.find(U"::", gap + 1) != Text::npos) {
    return false;
  }
  return ipv6_groups(s.substr(0, gap), false, groups) && ipv6_groups(s.substr(gap + 2), true, groups) &&
         groups >= 2 && groups <= 7;
}

// Maximal runs of characters accepted by `in_run`.
template <typename Pred>
std::vector<Text> runs(Text t, Pred in_run) {
  std::vector<Text> out;
  std::size_t k = 0;
  while (k < t.size()) {
    if (!in_run(t[k])) {
      ++k;
      continue;
    }
    const std::size_t start = k;
    while (k < t.size() && in_run(t[k])) {
      ++k;
    }
    out.push_back(t.substr(start, k - start));
  }
  return out;
}

// OCR often reads the "::" of a compressed address as one ':'
// ("2001:db8:3a09::2728" as "2001:db8:3a09:2728"). Without "::", four to
// seven hex groups of one to four characters, at least two of them three or
// more long, count too; a time ("12:30:45") has three groups and a MAC
// address ("aa:bb:cc:dd:ee:ff") only two-character groups.
bool ipv6_lost_gap(Text run) {
  if (run.find(U"::") != Text::npos) {
    return false;
  }
  const std::vector<Text> groups = split(run, U':');
  if (groups.size() < 4 || groups.size() > 7) {
    return false;
  }
  std::size_t long_groups = 0;
  for (const Text g : groups) {
    if (g.empty() || g.size() > 4 || !std::all_of(g.begin(), g.end(), is_hex)) {
      return false;
    }
    if (g.size() >= 3) {
      ++long_groups;
    }
  }
  return long_groups >= 2;
}

bool ipv6_candidate(Text run) {
  return std::count(run.begin(), run.end(), U':') >= 2 && (ipv6(run) || ipv6_lost_gap(run));
}

bool has_ip(Text t, Text d) {
  // The digit view, then the text as read: in the digit view a '|' or l next
  // to an address ("ip|192.0.2.44|") reads as a 1 and spoils its end octets.
  const auto ipv4_in = [](Text text) {
    for (const Text run : runs(text, [](char32_t c) { return is_digit(c) || c == U'.'; })) {
      if (has_ipv4(run)) {
        return true;
      }
    }
    return false;
  };
  if (ipv4_in(d) || ipv4_in(t)) {
    return true;
  }
  for (Text run : runs(t, [](char32_t c) { return is_hex(c) || c == U':' || c == U'.'; })) {
    while (!run.empty() && run.back() == U'.') {
      run.remove_suffix(1);
    }
    if (run.size() > 1 && run.front() == U':' && run[1] != U':') {
      run.remove_prefix(1);  // a label such as "ip:" before the address
    }
    if (run.size() > 1 && run.back() == U':' && run[run.size() - 2] != U':') {
      run.remove_suffix(1);
    }
    if (ipv6_candidate(run)) {
      return true;
    }
    // The run may start with the hex tail of a label glued to the address
    // ("ipv6:2001:..." starts "6:2001:..."); try it without its first group.
    const std::size_t colon = run.find(U':');
    if (colon != Text::npos && colon + 1 < run.size() && run[colon + 1] != U':' &&
        ipv6_candidate(run.substr(colon + 1))) {
      return true;
    }
  }
  return false;
}

// local@domain.tld, allowing one OCR space on each side of '@'. The domain
// needs a dot, no empty label and a final label of at least two letters.
bool has_email(Text t) {
  const auto local_char = [](char32_t c) {
    return is_word(c) || c == U'.' || c == U'_' || c == U'%' || c == U'+' || c == U'-' || c == U'\'';
  };
  const auto domain_char = [](char32_t c) { return is_word(c) || c == U'.' || c == U'-'; };
  for (std::size_t at = 0; at < t.size(); ++at) {
    if (t[at] != U'@') {
      continue;
    }
    std::size_t left = at;
    if (left > 0 && t[left - 1] == U' ') {
      --left;
    }
    if (left == 0 || !local_char(t[left - 1])) {
      continue;
    }
    std::size_t right = at + 1;
    if (right < t.size() && t[right] == U' ') {
      ++right;
    }
    std::size_t end = right;
    while (end < t.size() && domain_char(t[end])) {
      ++end;
    }
    Text domain = t.substr(right, end - right);
    while (!domain.empty() && (domain.back() == U'.' || domain.back() == U'-')) {
      domain.remove_suffix(1);
    }
    const std::vector<Text> labels = split(domain, U'.');
    const Text tld = labels.back();
    if (labels.size() >= 2 &&
        std::none_of(labels.begin(), labels.end(), [](Text l) { return l.empty(); }) &&
        tld.size() >= 2 && std::none_of(tld.begin(), tld.end(), is_digit)) {
      return true;
    }
  }
  return false;
}

struct TokenFormat {
  Text prefix;
  std::size_t min_body;
};

// Known credential prefixes (folded to lower case) and the shortest body that
// counts: Twitch stream keys and OAuth tokens, GitHub, GitLab, Slack, Stripe,
// OpenAI and Anthropic ("sk-"), AWS access key ids, Google API keys, npm,
// Hugging Face, and JWTs.
constexpr TokenFormat kTokenFormats[] = {
    {U"live_", 20},  {U"oauth:", 20},      {U"ghp_", 20},     {U"gho_", 20},    {U"ghu_", 20},
    {U"ghs_", 20},   {U"ghr_", 20},        {U"github_pat_", 20}, {U"glpat-", 20}, {U"xoxb-", 10},
    {U"xoxp-", 10},  {U"xoxa-", 10},       {U"xoxr-", 10},    {U"xoxs-", 10},   {U"xoxe-", 10},
    {U"sk_live_", 16}, {U"rk_live_", 16},  {U"sk_test_", 16}, {U"rk_test_", 16}, {U"sk-", 20},
    {U"akia", 16},   {U"asia", 16},        {U"aiza", 30},     {U"npm_", 30},    {U"hf_", 30},
    {U"eyj", 20},
};

bool base64url(char32_t c) noexcept { return is_alnum(c) || c == U'_' || c == U'-'; }

// A JWT whose dot-separated parts are each shorter than the single-run rule:
// a base64url header starting "eyj" ('{"') of at least kJwtMinHeader
// characters, a dot, and a payload of at least kJwtMinPayload characters.
// Short real headers, {"typ":"JWT"} and {"alg":"none"}, encode to 18 and 19
// characters; an empty payload, {}, encodes to "e30".
constexpr std::size_t kJwtMinHeader = 18;
constexpr std::size_t kJwtMinPayload = 3;

bool has_jwt(Text t) {
  for (std::size_t at = t.find(U"eyj"); at != Text::npos; at = t.find(U"eyj", at + 1)) {
    if (at > 0 && (is_alnum(t[at - 1]) || t[at - 1] == U'_')) {
      continue;
    }
    std::size_t dot = at;
    while (dot < t.size() && base64url(t[dot])) {
      ++dot;
    }
    if (dot - at < kJwtMinHeader || dot == t.size() || t[dot] != U'.') {
      continue;
    }
    std::size_t end = dot + 1;
    while (end < t.size() && base64url(t[end])) {
      ++end;
    }
    if (end - dot - 1 >= kJwtMinPayload) {
      return true;
    }
  }
  return false;
}

// OCR splits a long unbroken string with a stray space now and then. A body
// may continue past up to kMaxTokenSplits single spaces when the runs on both
// sides have at least kMinTokenPiece characters; a body that needs a split
// must hold a digit, so words after a short prefix-like word ("live_stream
// starting tonight") do not count. Random keys almost always hold a digit.
// JWTs ("eyj") never split: a header alone is not a token (has_jwt()).
constexpr std::size_t kMaxTokenSplits = 2;
constexpr std::size_t kMinTokenPiece = 6;

std::size_t base64url_run(Text t, std::size_t begin) noexcept {
  std::size_t end = begin;
  while (end < t.size() && base64url(t[end])) {
    ++end;
  }
  return end - begin;
}

// Characters of the token body starting at `begin`, spaces not counted.
std::size_t token_body(Text t, std::size_t begin, bool may_split) {
  std::size_t run = base64url_run(t, begin);
  std::size_t total = run;
  bool digit = std::any_of(t.begin() + static_cast<std::ptrdiff_t>(begin),
                           t.begin() + static_cast<std::ptrdiff_t>(begin + run), is_digit);
  std::size_t joined = total;  // body length that already stands without a split
  for (std::size_t splits = 0; may_split && splits < kMaxTokenSplits && run >= kMinTokenPiece; ++splits) {
    const std::size_t space = begin + run;
    if (space >= t.size() || t[space] != U' ') {
      break;
    }
    const std::size_t next = base64url_run(t, space + 1);
    if (next < kMinTokenPiece) {
      break;
    }
    digit = digit || std::any_of(t.begin() + static_cast<std::ptrdiff_t>(space + 1),
                                 t.begin() + static_cast<std::ptrdiff_t>(space + 1 + next), is_digit);
    begin = space + 1;
    run = next;
    total += next;
  }
  return digit ? total : joined;
}

// Known prefixes, JWTs and PEM headers. OCR reads l or I as '|', so prefixes
// and bodies are read in a view where every '|' is an l, at the same offsets as
// the text; whether a prefix starts a word is decided on the text as read,
// where a '|' is a separator ("key|ghp_..."). So a '|' can be a misread letter
// and a separator on one line, every match on the text as read is still one
// here (prefixes hold no '|', and a body in the view is never shorter), and
// token_body() only ever lengthens a body.
bool has_token(Text raw) {
  SecureU32String repaired;
  Text view = raw;
  if (raw.find(U'|') != Text::npos) {
    repaired.assign(raw.begin(), raw.end());
    std::replace(repaired.begin(), repaired.end(), U'|', U'l');
    view = Text(repaired);
  }
  for (const TokenFormat& format : kTokenFormats) {
    for (std::size_t at = view.find(format.prefix); at != Text::npos; at = view.find(format.prefix, at + 1)) {
      if (at > 0 && (is_alnum(raw[at - 1]) || raw[at - 1] == U'_')) {
        continue;
      }
      if (token_body(view, at + format.prefix.size(), format.prefix != U"eyj") >= format.min_body) {
        return true;
      }
    }
  }
  if (has_jwt(raw) || has_jwt(view)) {
    return true;
  }
  // A PEM private key header ("-----BEGIN OPENSSH PRIVATE KEY-----").
  for (std::size_t at = raw.find(U"begin"); at != Text::npos; at = raw.find(U"begin", at + 1)) {
    const bool after_dash =
        at > 0 && (raw[at - 1] == U'-' || (at > 1 && raw[at - 1] == U' ' && raw[at - 2] == U'-'));
    if (after_dash && raw.find(U"private key", at) != Text::npos) {
      return true;
    }
  }
  return false;
}

constexpr Text kStreetSuffixes[] = {
    U"street", U"st",    U"avenue", U"ave",      U"av",    U"road",    U"rd",      U"boulevard",
    U"blvd",   U"lane",  U"ln",     U"drive",    U"dr",    U"court",   U"ct",      U"place",
    U"pl",     U"way",   U"terrace", U"ter",     U"circle", U"cir",    U"highway", U"hwy",
    U"parkway", U"pkwy", U"square", U"sq",       U"trail", U"trl",     U"crescent", U"cres",
    U"plaza",  U"plz",   U"alley",  U"crossing", U"grove", U"gardens", U"heights",  U"mews",
};

bool house_number(Text w) noexcept {
  std::size_t digits = 0;
  while (digits < w.size() && is_digit(w[digits])) {
    ++digits;
  }
  return digits >= 1 && digits <= 6 && (w.size() == digits || (w.size() == digits + 1 && is_lower(w.back())));
}

bool has_letter(Text w) noexcept {
  return std::any_of(w.begin(), w.end(), [](char32_t c) { return !is_digit(c); });
}

// A house number, one to four name words and a street suffix ("221b baker
// street"), or a PO box ("po box 12", "p o box 12").
bool has_address(Text t) {
  const std::vector<Text> w = runs(t, is_word);
  for (std::size_t i = 0; i < w.size(); ++i) {
    if (!house_number(w[i])) {
      continue;
    }
    if ((i >= 2 && w[i - 2] == U"po" && w[i - 1] == U"box") ||
        (i >= 3 && w[i - 3] == U"p" && w[i - 2] == U"o" && w[i - 1] == U"box")) {
      return true;
    }
    for (std::size_t k = i + 2; k < w.size() && k <= i + 5; ++k) {
      if (!has_letter(w[k - 1])) {
        break;
      }
      if (std::find(std::begin(kStreetSuffixes), std::end(kStreetSuffixes), w[k]) !=
          std::end(kStreetSuffixes)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

CategorySet find_pii_folded(std::u32string_view folded, CategorySet enabled) {
  CategorySet found;
  if (enabled.contains(Category::PiiPhone) || enabled.contains(Category::PiiCard) ||
      enabled.contains(Category::PiiIp)) {
    const SecureU32String digits = digit_view(folded);
    scan_numbers(digits, enabled, found);
    if (enabled.contains(Category::PiiIp) && has_ip(folded, digits)) {
      found.insert(Category::PiiIp);
    }
  }
  if (enabled.contains(Category::PiiEmail) && has_email(folded)) {
    found.insert(Category::PiiEmail);
  }
  if (enabled.contains(Category::PiiToken) && has_token(folded)) {
    found.insert(Category::PiiToken);
  }
  if (enabled.contains(Category::PiiAddress) && has_address(folded)) {
    found.insert(Category::PiiAddress);
  }
  return found;
}

}  // namespace detail

CategorySet find_pii(std::string_view utf8, CategorySet enabled) {
  return detail::find_pii_folded(detail::fold_text(utf8), enabled);
}

bool luhn_valid(std::string_view digits) noexcept { return detail::luhn(digits); }

}  // namespace spg
