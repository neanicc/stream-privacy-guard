// Normalization, fuzzy watch-list matching and the policy engine (SPG-08).
// Every watch-list string here is synthetic.
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "spg/category.hpp"
#include "spg/detection.hpp"
#include "spg/error.hpp"
#include "spg/policy.hpp"
#include "spg/policy_config.hpp"
#include "spg/secret.hpp"

namespace {

std::string norm(std::string_view text) { return std::string(spg::normalize_for_matching(text).reveal()); }

spg::PolicyConfig watchlist_config(const std::vector<std::string>& entries, spg::FuzzyLimits fuzzy = {}) {
  spg::PolicyConfig config;
  config.categories = spg::CategorySet{};
  config.categories.insert(spg::Category::Watchlist);
  config.fuzzy = fuzzy;
  for (const std::string& entry : entries) {
    config.watchlist.emplace_back(entry);
  }
  return config;
}

bool hits(const std::vector<std::string>& entries, std::string_view text, spg::FuzzyLimits fuzzy = {}) {
  const spg::PolicyEngine engine(watchlist_config(entries, fuzzy));
  const spg::Verdict verdict = engine.evaluate(spg::Secret(std::string(text)));
  CHECK((verdict.reasons == spg::CategorySet{} || describe(verdict) == "WATCHLIST"));
  return verdict.sensitive();
}

}  // namespace

TEST_CASE("policy: normalization folds case, width, styles and diacritics", "[policy][normalize]") {
  CHECK(norm("Zyxa Quorbel") == "zyxa quorbel");
  CHECK(norm("ＺＹＸＡ") == "zyxa");                       // fullwidth
  CHECK(norm("\xF0\x9D\x90\x99\xF0\x9D\x90\xB2xa") == "zyxa");  // mathematical bold Z, y
  CHECK(norm("Zÿxà") == "zyxa");                          // precomposed diacritics
  CHECK(norm("Zy\xCC\x81xa") == "zyxa");                  // combining acute accent
  CHECK(norm("Ærøskøbing") == norm("aeroskobing"));
  CHECK(norm("Straße") == norm("strasse"));
  CHECK(norm("\xEF\xAC\x81zz") == norm("fizz"));           // U+FB01 ligature
  CHECK(norm("Ⓩⓨⓧⓐ") == "zyxa");                          // circled letters
}

TEST_CASE("policy: normalization collapses separators and drops zero-width characters",
          "[policy][normalize]") {
  CHECK(norm("  Zyxa\t\n  Quorbel!! ") == "zyxa quorbel");
  CHECK(norm("Zyxa\xC2\xA0Quorbel") == "zyxa quorbel");   // no-break space
  CHECK(norm("Zyxa\xE3\x80\x80Quorbel") == "zyxa quorbel");  // ideographic space
  CHECK(norm("O\xE2\x80\x99Zyxa") == "o zyxa");             // typographic apostrophe
  CHECK(norm("Zy\xE2\x80\x8Bxa") == "zyxa");                // zero-width space inside a word
  CHECK(norm("Zy\xC2\xADxa") == "zyxa");                    // soft hyphen
  CHECK(norm("zy\xFFxa") == "zy xa");                       // malformed UTF-8 separates
  CHECK(norm("Zyxa\xD8\x8CQuorbel") == "zyxa quorbel");     // Arabic comma
  CHECK(norm("Zyxa\xE0\xA5\xA4Quorbel") == "zyxa quorbel");  // Devanagari danda
  CHECK(norm("Zyxa\xF0\x9F\x98\x80Quorbel") == "zyxa quorbel");  // emoji (a symbol)
  CHECK(norm("\xD0\x96yxa") == "\xD0\xB6yxa");                // a Cyrillic letter stays a letter
  CHECK(norm("--!!--").empty());
  CHECK(norm("").empty());
}

TEST_CASE("policy: normalization merges OCR confusions", "[policy][normalize]") {
  CHECK(norm("Q0RBEL") == norm("qorbel"));
  CHECK(norm("1") == norm("l"));
  CHECK(norm("I") == norm("l"));
  CHECK(norm("|") == norm("l"));
  CHECK(norm("5YNTH") == norm("synth"));
  CHECK(norm("8ORB") == norm("borb"));
  CHECK(norm("Ｑ０") == norm("qo"));  // fullwidth digit folds first
}

TEST_CASE("policy: normalization is idempotent", "[policy][normalize]") {
  for (const std::string_view text :
       {"Zyxa Quorbel", "ＺＹＸＡ ０１", "Ærø-Straße 12!", "4821 Qx Maple Synthway", "Ⅻ ½ ™"}) {
    const std::string once = norm(text);
    CHECK(norm(once) == once);
  }
}

TEST_CASE("policy: watch-list entries match through OCR noise", "[policy][watchlist]") {
  const std::vector<std::string> entries = {"Zyxa Quorbel"};
  CHECK(hits(entries, "gg zyxa quorbel wins"));
  CHECK(hits(entries, "ZYXA QU0RBE1"));     // confusions cost nothing
  CHECK(hits(entries, "Zyxa Quarbel"));     // one substitution
  CHECK(hits(entries, "Zyx Quorbel"));      // one deletion
  CHECK(hits(entries, "ZyxaQuorbel"));      // missing space
  CHECK(hits(entries, "xXzyxaquorbelXx"));  // inside a longer handle
  CHECK(hits(entries, "Zyxa, Quorbel."));
  CHECK_FALSE(hits(entries, "Zara Corbett"));  // decoy
  // 12 characters allow min(2, floor(0.2 * 12)) = 2 edits. Partial reveals are
  // off here: a 7-character prefix with one edit would match "Zyxo Quarbek".
  const spg::FuzzyLimits whole{2U, 0.2, 12U};
  CHECK(hits(entries, "Zyxo Quarbel", whole));
  CHECK_FALSE(hits(entries, "Zyxo Quarbek", whole));
  CHECK(hits(entries, "Zyxo Quarbek"));
  CHECK_FALSE(hits(entries, "nothing to see here"));
  CHECK_FALSE(hits(entries, ""));
}

TEST_CASE("policy: partial reveals match from a word start or to a word end", "[policy][watchlist]") {
  const std::vector<std::string> entries = {"4821 Qx Maple Synthway"};
  CHECK(hits(entries, "my place is 4821 Qx Mapl"));       // typing in: a prefix
  CHECK(hits(entries, "4821 Qx"));                         // 7 characters >= min_partial_chars
  CHECK(hits(entries, "ple Synthway is home"));            // clipped on the left: a suffix
  CHECK(hits(entries, "4B2l Qx Mapel"));                   // a prefix with OCR noise
  CHECK_FALSE(hits(entries, "4821"));                      // shorter than min_partial_chars
  CHECK_FALSE(hits(entries, "Maple"));                     // neither a prefix nor a suffix
  spg::FuzzyLimits longer_partials;
  longer_partials.min_partial_chars = 10;
  CHECK_FALSE(hits(entries, "4821 Qx", longer_partials));
  CHECK(hits(entries, "4821 Qx Map", longer_partials));
}

TEST_CASE("policy: short entries match whole words only", "[policy][watchlist]") {
  const std::vector<std::string> entries = {"Vex"};
  CHECK(hits(entries, "the vex arrives"));
  CHECK(hits(entries, "VEX!"));
  CHECK(hits(entries, "zyxa vex quorbel"));
  CHECK(hits(entries, "vex\xD8\x8C"));              // Arabic comma ends the word
  CHECK(hits(entries, "\xE3\x80\x8Cvex\xE3\x80\x8D"));  // CJK corner brackets
  CHECK(hits(entries, "\xC2\xBFvex?"));             // inverted question mark
  CHECK_FALSE(hits(entries, "convex shapes"));
  CHECK_FALSE(hits(entries, "vexing"));
  CHECK_FALSE(hits(entries, "vax"));  // 3 characters allow no edits
}

TEST_CASE("policy: fuzzy limits come from the config", "[policy][watchlist]") {
  const std::vector<std::string> entries = {"Zyxa Quorbel"};
  const spg::FuzzyLimits exact{0U, 0.0, 12U};
  CHECK(hits(entries, "zyxa quorbel", exact));
  CHECK(hits(entries, "ZYXA QU0RBEL", exact));  // confusions are not edits
  CHECK_FALSE(hits(entries, "Zyxa Quarbel", exact));
  const spg::FuzzyLimits ratio_only{5U, 0.1, 12U};  // floor(0.1 * 12) = 1
  CHECK(hits(entries, "Zyxa Quarbel", ratio_only));
  CHECK_FALSE(hits(entries, "Zyxo Quarbel", ratio_only));
}

TEST_CASE("policy: any matching entry counts", "[policy][watchlist]") {
  const std::vector<std::string> entries = {"Zyxa Quorbel", "Synthville Qx Academy"};
  CHECK(hits(entries, "class of synthville qx academy"));
  CHECK_FALSE(hits(entries, "class of 2031"));
}

TEST_CASE("policy: engine reports every enabled reason and nothing else", "[policy][engine]") {
  spg::PolicyConfig config;
  config.watchlist.emplace_back("Zyxa Quorbel");
  const spg::PolicyEngine engine(config);
  const spg::Verdict verdict =
      engine.evaluate(spg::Secret("Zyxa Quorbel, card 4111 1111 1111 1111, mail zq@example.com"));
  CHECK(spg::describe(verdict) == "WATCHLIST,PII_EMAIL,PII_CARD");
  CHECK_FALSE(verdict.reasons.contains(spg::Category::Face));
  CHECK(spg::describe(spg::Verdict{}).empty());

  config.categories.erase(spg::Category::Watchlist);
  config.categories.erase(spg::Category::PiiCard);
  const spg::PolicyEngine fewer(config);
  CHECK(spg::describe(fewer.evaluate(spg::Secret("Zyxa Quorbel 4111 1111 1111 1111 zq@example.com"))) ==
        "PII_EMAIL");

  spg::TextHit hit;
  hit.detection.kind = spg::DetectionKind::TextRegion;
  hit.text = spg::Secret("call 212-555-0123");
  engine.apply(hit);
  CHECK(spg::describe(hit.verdict) == "PII_PHONE");
}

TEST_CASE("policy: engine rejects configs it cannot enforce", "[policy][engine]") {
  spg::PolicyConfig config;
  config.watchlist.emplace_back("Zyxa Quorbel");
  config.watchlist.emplace_back("-- !! --");
  CHECK_THROWS_AS(spg::PolicyEngine(config), spg::ContractError);
  config.watchlist.back() = spg::Secret(" \t ");
  CHECK_THROWS_AS(spg::PolicyEngine(config), spg::ContractError);
  config.watchlist.pop_back();
  config.fuzzy.min_partial_chars = 0;
  CHECK_THROWS_AS(spg::PolicyEngine(config), spg::ContractError);
}
