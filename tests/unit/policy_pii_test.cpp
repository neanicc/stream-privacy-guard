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
  CHECK(found(Category::PiiIp, "fe80::1ff:fe23:4567:890a"));
  CHECK(found(Category::PiiIp, "::ffff:192.0.2.128"));
  CHECK_FALSE(found(Category::PiiIp, "256.1.1.1"));
  CHECK_FALSE(found(Category::PiiIp, "1.2.3"));
  CHECK_FALSE(found(Category::PiiIp, "12:30:45"));
  CHECK_FALSE(found(Category::PiiIp, "aa:bb:cc:dd:ee:ff"));
  CHECK_FALSE(found(Category::PiiIp, "std::move"));
  CHECK_FALSE(found(Category::PiiIp, "1::2::3"));
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
  CHECK_FALSE(found(Category::PiiToken, "ghp_short"));
  CHECK_FALSE(found(Category::PiiToken, "task-SYNTHETIC0123456789abc"));
  CHECK_FALSE(found(Category::PiiToken, "eurasiaSYNTHETIC012345"));
  CHECK_FALSE(found(Category::PiiToken, "let's begin; keep your private key safe"));
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
