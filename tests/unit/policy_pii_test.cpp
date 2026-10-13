// Structured PII validators (SPG-08). Every value here is synthetic: card
// numbers are published test numbers, phone numbers use 555-01xx, domains are
// reserved example domains and tokens spell SYNTHETIC.
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "spg/category.hpp"
#include "spg/policy.hpp"

namespace {

spg::CategorySet only(spg::Category c) {
  spg::CategorySet set;
  set.insert(c);
  return set;
}

bool found(spg::Category c, std::string_view text) { return spg::find_pii(text, only(c)).contains(c); }

}  // namespace

TEST_CASE("pii: Luhn checksum", "[policy][pii]") {
  CHECK(spg::luhn_valid("79927398713"));
  CHECK(spg::luhn_valid("4111111111111111"));
  CHECK_FALSE(spg::luhn_valid("79927398710"));
  CHECK_FALSE(spg::luhn_valid("4111111111111112"));
  CHECK_FALSE(spg::luhn_valid(""));
  CHECK_FALSE(spg::luhn_valid("4111 1111"));
  CHECK_FALSE(spg::luhn_valid("12a4"));
}

TEST_CASE("pii: card numbers need 13 to 19 digits and a valid checksum", "[policy][pii]") {
  using spg::Category;
  CHECK(found(Category::PiiCard, "card 4111 1111 1111 1111 exp 12/30"));
  CHECK(found(Category::PiiCard, "5555-5555-5555-4444"));
  CHECK(found(Category::PiiCard, "378282246310005"));                          // 15 digits
  CHECK(found(Category::PiiCard, "\xEF\xBC\x94" "111 1111 1111 1111"));       // fullwidth 4
  CHECK(found(Category::PiiCard, "4111 1111 1111 111l"));                      // OCR l for 1
  CHECK_FALSE(found(Category::PiiCard, "4111 1111 1111 1112"));                // bad checksum
  CHECK_FALSE(found(Category::PiiCard, "order 4111 1111"));                    // too short
  CHECK_FALSE(found(Category::PiiCard, "4111 ---- 1111 1111 1111"));           // groups too far apart
}

TEST_CASE("pii: phone numbers follow NANP or E.164", "[policy][pii]") {
  using spg::Category;
  CHECK(found(Category::PiiPhone, "call (212) 555-0123"));
  CHECK(found(Category::PiiPhone, "212.555.0199"));
  CHECK(found(Category::PiiPhone, "212 5550199"));
  CHECK(found(Category::PiiPhone, "2125550123"));
  CHECK(found(Category::PiiPhone, "1-800-555-0199"));
  CHECK(found(Category::PiiPhone, "+44 20 7946 0958"));
  CHECK(found(Category::PiiPhone, "+1 (212) 555-0123"));
  CHECK(found(Category::PiiPhone, "+ 44 20 7946 0958"));                // OCR space after '+'
  CHECK(found(Category::PiiPhone, "+(44) 20 7946 0958"));
  CHECK(found(Category::PiiPhone, "+ (44) 20 7946 0958"));
  CHECK(found(Category::PiiPhone, "212 555 O123"));                     // OCR O for 0
  CHECK(found(Category::PiiPhone, "ids 4821 then 212-555-0123 646-555-0199"));
  CHECK_FALSE(found(Category::PiiPhone, "(012) 555-0123"));             // area code starts 0
  CHECK_FALSE(found(Category::PiiPhone, "212-055-0123"));               // exchange starts 0
  CHECK_FALSE(found(Category::PiiPhone, "2026-10-12 12:30"));           // 10 digits, not 3-3-4
  CHECK_FALSE(found(Category::PiiPhone, "21-25-55-01-23"));
  CHECK_FALSE(found(Category::PiiPhone, "score 12345 67"));
  CHECK_FALSE(found(Category::PiiPhone, "+0 123 456 789"));
}

TEST_CASE("pii: email addresses", "[policy][pii]") {
  using spg::Category;
  CHECK(found(Category::PiiEmail, "mail zq.synth+tag@example.com now"));
  CHECK(found(Category::PiiEmail, "ZQ@EXAMPLE.ORG"));
  CHECK(found(Category::PiiEmail, "zq @ mail.example.net."));  // OCR spaces, sentence dot
  CHECK_FALSE(found(Category::PiiEmail, "user@localhost"));
  CHECK_FALSE(found(Category::PiiEmail, "zq@example.c"));
  CHECK_FALSE(found(Category::PiiEmail, "@example.com"));
  CHECK_FALSE(found(Category::PiiEmail, "zq@example..com"));
  CHECK_FALSE(found(Category::PiiEmail, "version 1.2@3.4"));
}

TEST_CASE("pii: IPv4 and IPv6 addresses", "[policy][pii]") {
  using spg::Category;
  CHECK(found(Category::PiiIp, "connect 192.0.2.44 now"));
  CHECK(found(Category::PiiIp, "198.51.100.7."));
  CHECK(found(Category::PiiIp, "2001:db8:0:0:0:0:2:1"));
  CHECK(found(Category::PiiIp, "ip:2001:db8::2:1"));
  CHECK(found(Category::PiiIp, "IPv6:2001:db8:0:0:0:0:2:1"));  // label's 6 is hex
  CHECK(found(Category::PiiIp, "addr:2001:db8:0:0:0:0:2:1"));
  CHECK(found(Category::PiiIp, "fe80::1ff:fe23:4567:890a"));
  CHECK(found(Category::PiiIp, "::ffff:192.0.2.128"));
  CHECK_FALSE(found(Category::PiiIp, "256.1.1.1"));
  CHECK_FALSE(found(Category::PiiIp, "1.2.3"));
  CHECK_FALSE(found(Category::PiiIp, "12:30:45"));
  CHECK_FALSE(found(Category::PiiIp, "aa:bb:cc:dd:ee:ff"));
  CHECK_FALSE(found(Category::PiiIp, "std::move"));
  CHECK_FALSE(found(Category::PiiIp, "1::2::3"));
  // OCR read "::" as one ':' (SPG-12, a HUD line on the text suite).
  CHECK(found(Category::PiiIp, "Server 2001:db8:3a09:2728"));
  CHECK(found(Category::PiiIp, "ip:2001:db8:cc6:bc63"));
  CHECK_FALSE(found(Category::PiiIp, "12:30:45:07"));          // no group of three or more
  CHECK_FALSE(found(Category::PiiIp, "2001:db8:3a09"));        // three groups
}

// The lost-"::" rule trades some over-blur for recall. These are clean
// on-screen strings it must leave alone, so the over-blur stays bounded to
// runs of 4 to 7 hex groups with two groups of 3 or more characters.
TEST_CASE("pii: the lost-gap IPv6 rule leaves clean text alone", "[policy][pii]") {
  using spg::Category;
  CHECK_FALSE(found(Category::PiiIp, "[12:30:45] chat is live"));     // chat timestamp
  CHECK_FALSE(found(Category::PiiIp, "split 01:23:45.678"));          // speedrun timer
  CHECK_FALSE(found(Category::PiiIp, "uptime 1:02:03:04"));           // d:hh:mm:ss
  CHECK_FALSE(found(Category::PiiIp, "mac 00:1a:2b:3c:4d:5e"));       // MAC address
  CHECK_FALSE(found(Category::PiiIp, "16:9 at 1080p, John 3:16"));    // ratios, verses
  CHECK_FALSE(found(Category::PiiIp, "score 1:100:2:3"));             // one long group
  CHECK_FALSE(found(Category::PiiIp, "bad:cafe:face"));               // three groups
  CHECK_FALSE(found(Category::PiiIp, "1:2:3:4:5:6:7:8:9:10"));        // ten groups
  CHECK_FALSE(found(Category::PiiIp, "aaaaa:bbbbb:ccccc:ddddd"));     // groups too long
  CHECK_FALSE(found(Category::PiiIp, "2026-10-12 12:30:45"));         // date and time
  // Accepted over-blur, by design (recall over precision): four hex groups,
  // two of them three long, read as an address that lost its "::".
  CHECK(found(Category::PiiIp, "12:345:678:90"));
}

TEST_CASE("pii: street addresses and PO boxes", "[policy][pii]") {
  using spg::Category;
  CHECK(found(Category::PiiAddress, "221B Synth Street"));
  CHECK(found(Category::PiiAddress, "at 4821 Qx Maple Ln. today"));
  CHECK(found(Category::PiiAddress, "77 North Synthwood Blvd"));
  CHECK(found(Category::PiiAddress, "PO Box 4821"));
  CHECK(found(Category::PiiAddress, "P.O. Box 12"));
  CHECK_FALSE(found(Category::PiiAddress, "level 42 boss fight"));
  CHECK_FALSE(found(Category::PiiAddress, "Street 5"));
  CHECK_FALSE(found(Category::PiiAddress, "4821 Street"));  // no street name
  CHECK_FALSE(found(Category::PiiAddress, "1 2 3 Road"));
}

TEST_CASE("pii: credential and stream key formats", "[policy][pii]") {
  using spg::Category;
  CHECK(found(Category::PiiToken, "key: live_123456789_SYNTHETICabcdefghij"));
  CHECK(found(Category::PiiToken, "ghp_SYNTHETIC0123456789SYNTHETIC0123"));
  CHECK(found(Category::PiiToken, "AKIASYNTHETIC0123456"));
  CHECK(found(Category::PiiToken, "xoxb-0000-SYNTHETIC"));
  CHECK(found(Category::PiiToken, "sk-ant-SYNTHETIC0123456789abc"));
  CHECK(found(Category::PiiToken, "-----BEGIN OPENSSH PRIVATE KEY-----"));
  // A JWT whose header ({"alg":"HS256"}) and payload ({"sub":"zq"}) are each
  // shorter than the single-run rule; with and without the signature.
  CHECK(found(Category::PiiToken, "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ6cSJ9.SYNTHETICsig"));
  CHECK(found(Category::PiiToken, "token=eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ6cSJ9."));
  CHECK(found(Category::PiiToken, "eyJ0eXAiOiJKV1QifQ.e30.SYNTHETIC"));  // {"typ":"JWT"}, {}
  CHECK_FALSE(found(Category::PiiToken, "ghp_short"));
  CHECK_FALSE(found(Category::PiiToken, "task-SYNTHETIC0123456789abc"));
  CHECK_FALSE(found(Category::PiiToken, "eurasiaSYNTHETIC012345"));
  CHECK_FALSE(found(Category::PiiToken, "let's begin; keep your private key safe"));
  CHECK_FALSE(found(Category::PiiToken, "eyJhbGciOiJIUzI1NiJ9 then a space"));  // header alone
  CHECK_FALSE(found(Category::PiiToken, "eyJhbGciOiJIUzI1NiJ9.ab"));            // payload too short
  CHECK_FALSE(found(Category::PiiToken, "Eyjafjallajokull.is"));                 // too short for a header
}

TEST_CASE("pii: tokens survive OCR misreads", "[policy][pii]") {
  using spg::Category;
  // A small-font token on the text suite was read with I as '|' and with a
  // stray space (SPG-12); the '|' ended the body after 16 characters.
  CHECK(found(Category::PiiToken, "ghp_SYNTHETIC0123456|SYNTHETIC0123456789"));
  CHECK(found(Category::PiiToken, "|ive_123456789_SYNTHETICabcdefghij"));  // l of live_ read as '|'
  // A '|' as a separator and a '|' as a misread letter on one line.
  CHECK(found(Category::PiiToken, "key|ghp_SYNTHETIC0123456|SYNTHETIC0123456789"));
  CHECK(found(Category::PiiToken, "key||ive_123456789_SYNTHETICabcdefghij|"));
  CHECK_FALSE(found(Category::PiiToken, "x|ive_123456789_SYNTHETICabcdefghij"));  // "xlive_" starts no word
  CHECK(found(Category::PiiToken, "ghp_SYNTHETIC0 123456SYNTHETIC0123456789"));
  CHECK(found(Category::PiiToken, "ghp_SYNTH3TIC xyzw0123 SYNTHETIC"));  // needs both splits
  // A split needs runs of at least six characters on both sides and a digit
  // somewhere in the body, at most two splits join, and a JWT never splits.
  CHECK_FALSE(found(Category::PiiToken, "ghp_SYNTH3TIC xyzw SYNTHETICabcdef"));
  CHECK_FALSE(found(Category::PiiToken, "eyJhbGciOiJIUzI1NiJ9 tomorrow"));
  CHECK_FALSE(found(Category::PiiToken, "live_streaming starting tomorrow evening"));
  CHECK_FALSE(found(Category::PiiToken, "ghp_SYNTH3 abcdef ghijkl mnopqrstuv"));
}

TEST_CASE("pii: the OCR-tolerant rules keep every match the strict rules had", "[policy][pii]") {
  using spg::Category;
  // Pins what the rules before SPG-12 detected, so a change that narrows them
  // fails here. Each known prefix with a body of exactly its minimum length,
  // as is, with words after it, with a '|' or a space before it, and in a
  // table cell ("key|<token>|"). Bodies are synthetic.
  struct Format {
    const char* prefix;
    std::size_t min_body;
  };
  constexpr Format kFormats[] = {
      {"live_", 20},  {"oauth:", 20},   {"ghp_", 20},     {"gho_", 20},     {"ghu_", 20},  {"ghs_", 20},
      {"ghr_", 20},   {"github_pat_", 20}, {"glpat-", 20}, {"xoxb-", 10},   {"xoxp-", 10}, {"xoxa-", 10},
      {"xoxr-", 10},  {"xoxs-", 10},    {"xoxe-", 10},    {"sk_live_", 16}, {"rk_live_", 16},
      {"sk_test_", 16}, {"rk_test_", 16}, {"sk-", 20},     {"AKIA", 16},     {"ASIA", 16},  {"AIza", 30},
      {"npm_", 30},   {"hf_", 30},      {"eyJ", 20},
  };
  const std::string body_chars = "SYNTH0123456789abcdefghijklmnopqrstuvwxyz";
  for (const Format& f : kFormats) {
    const std::string token = std::string(f.prefix) + body_chars.substr(0, f.min_body);
    CAPTURE(token);
    CHECK(found(Category::PiiToken, token));
    CHECK(found(Category::PiiToken, token + " then plain words follow"));  // digit-free piece after a space
    CHECK(found(Category::PiiToken, token + " 0123 more"));
    CHECK(found(Category::PiiToken, "key|" + token + "|"));
    CHECK(found(Category::PiiToken, "|" + token));
    CHECK(found(Category::PiiToken, "key: " + token));
    // One character short stays below the rule, as before.
    CHECK_FALSE(found(Category::PiiToken, std::string(f.prefix) + body_chars.substr(0, f.min_body - 1)));
  }
  // IPv4 and IPv6 forms the strict rules found, with a '|' or a space around.
  for (const char* ip : {"192.0.2.44", "2001:db8:0:0:0:0:2:1", "2001:db8::2:1", "fe80::1ff:fe23:4567:890a",
                         "::ffff:192.0.2.128", "2001:0db8:85a3:0000:0000:8a2e:0370:7334", "2001:db8::"}) {
    CAPTURE(ip);
    CHECK(found(Category::PiiIp, ip));
    CHECK(found(Category::PiiIp, std::string("server|") + ip + "|"));
    CHECK(found(Category::PiiIp, std::string("ip ") + ip + " up"));
  }
}

TEST_CASE("pii: only enabled categories are reported", "[policy][pii]") {
  const std::string text = "4111 1111 1111 1111 zq@example.com 192.0.2.44";
  CHECK(spg::find_pii(text, spg::CategorySet{}).empty());
  const spg::CategorySet all = spg::find_pii(text, spg::CategorySet::all());
  CHECK(all.contains(spg::Category::PiiCard));
  CHECK(all.contains(spg::Category::PiiEmail));
  CHECK(all.contains(spg::Category::PiiIp));
  CHECK_FALSE(all.contains(spg::Category::Face));
  CHECK_FALSE(all.contains(spg::Category::Watchlist));
  CHECK(spg::find_pii(text, only(spg::Category::PiiPhone)).empty());
}
