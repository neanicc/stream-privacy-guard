#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "spg/category.hpp"
#include "spg/error.hpp"
#include "spg/json.hpp"
#include "spg/policy_config.hpp"

using namespace std::chrono_literals;

namespace {

std::string read_fixture(std::string_view name) {
  const std::string path = std::string(SPG_FIXTURE_DIR) + "/contracts/" + std::string(name);
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// Synthetic watch-list sentinel. Any error message that contains part of it
// would show that input text can reach a log.
constexpr std::string_view kSentinel = "zq-SENTINEL-7f3a";

template <typename Parse>
std::string error_of(Parse parse, const std::string& json) {
  try {
    parse(json);
  } catch (const spg::ContractError& e) {
    return e.what();
  }
  FAIL("expected ContractError");
  return {};
}

const auto parse_config = [](const std::string& s) { return spg::parse_policy_config(s); };
const auto parse_truth = [](const std::string& s) { return spg::parse_ground_truth_frame(s); };
const auto parse_plan = [](const std::string& s) { return spg::parse_mask_plan(s); };

std::string with_sentinel(std::string_view tmpl) {
  std::string out(tmpl);
  const std::string marker = "@S@";
  for (auto pos = out.find(marker); pos != std::string::npos; pos = out.find(marker)) {
    out.replace(pos, marker.size(), kSentinel);
  }
  return out;
}

}  // namespace

TEST_CASE("json: policy config fixture parses", "[json][policy]") {
  const spg::PolicyConfig c = spg::parse_policy_config(read_fixture("policy_config.json"));
  CHECK(c.categories.contains(spg::Category::Face));
  CHECK(c.categories.contains(spg::Category::PiiCard));
  CHECK_FALSE(c.categories.contains(spg::Category::PiiPhone));
  REQUIRE(c.watchlist.size() == 2);
  CHECK(c.watchlist[0].reveal() == "SYNTHETIC NAME QX7");
  CHECK(c.fuzzy == spg::FuzzyLimits{1U, 0.15, 5U});
  CHECK(c.lookahead == 400ms);
  CHECK(c.retro == 400ms);  // defaults to the lookahead
  CHECK(c.hold == 1000ms);
  CHECK(c.margin_px == 6.0F);
  CHECK(c.margin_ratio == 0.5F);
  CHECK(c.fallback == spg::FallbackMode::SolidFill);
  CHECK(c.mask_style == spg::MaskStyle::Pixelate);
}

TEST_CASE("json: a minimal policy config takes the plan defaults", "[json][policy]") {
  CHECK(spg::parse_policy_config(R"({"schema": 1})") == spg::PolicyConfig{});
}

TEST_CASE("json: policy configs at the floors and caps parse", "[json][policy]") {
  const spg::PolicyConfig floors = spg::parse_policy_config(read_fixture("policy_config_floors.json"));
  spg::CategorySet token_only;
  token_only.insert(spg::Category::PiiToken);
  CHECK(floors.categories == token_only);
  CHECK(floors.lookahead == spg::kMinLookahead);
  CHECK(floors.retro == spg::kMinRetro);
  CHECK(floors.hold == spg::kMinHold);
  CHECK(floors.margin_px == static_cast<float>(spg::kMinMarginPx));
  CHECK(floors.margin_ratio == static_cast<float>(spg::kMinMarginRatio));

  const spg::PolicyConfig caps = spg::parse_policy_config(read_fixture("policy_config_caps.json"));
  CHECK(caps.categories == spg::CategorySet::all());
  CHECK(caps.lookahead == spg::kMaxLookahead);
  CHECK(caps.retro == spg::kMaxLookahead);
  CHECK(caps.hold == spg::kMaxHold);
  CHECK(caps.margin_px == static_cast<float>(spg::kMaxMarginPx));
  CHECK(caps.margin_ratio == static_cast<float>(spg::kMaxMarginRatio));
}

// Fail closed: a config that would turn detection, the lookahead, the
// retroactive window, the hold or the margins off is an error, not a quiet
// "off". Each case in the fixture must be rejected with an error that names
// its field.
TEST_CASE("json: policy configs that switch a protection off are rejected", "[json][policy]") {
  std::istringstream lines(read_fixture("policy_config_rejected.tsv"));
  std::size_t cases = 0;
  for (std::string line; std::getline(lines, line);) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::size_t tab = line.find('\t');
    REQUIRE(tab != std::string::npos);
    const std::string field = line.substr(0, tab);
    const std::string json = line.substr(tab + 1);
    const std::string message = error_of(parse_config, json);
    INFO(json << " -> " << message);
    CHECK(message.rfind("policy config: ", 0) == 0);
    CHECK(message.find(field) != std::string::npos);
    ++cases;
  }
  CHECK(cases == 20);

  // The serializer refuses to write such a config too.
  spg::PolicyConfig none;
  none.categories = spg::CategorySet{};
  CHECK_THROWS_AS(spg::serialize_policy_config(none), spg::ContractError);
  spg::PolicyConfig no_hold;
  no_hold.hold = 0ms;
  CHECK_THROWS_AS(spg::serialize_policy_config(no_hold), spg::ContractError);
}

TEST_CASE("json: policy configs round trip", "[json][policy][property]") {
  std::vector<spg::PolicyConfig> configs(4);
  configs[1].watchlist = {spg::Secret("SYNTHETIC NAME QX7"), spg::Secret("Ünïcode Ëntry QX7 \"quoted\"")};
  configs[1].categories = spg::CategorySet{};
  configs[1].categories.insert(spg::Category::PiiToken);
  configs[2].lookahead = 1200ms;
  configs[2].retro = 300ms;
  configs[2].hold = spg::kMinHold;
  configs[2].margin_px = 2.1F;
  configs[2].margin_ratio = 1.0F / 3.0F;
  configs[2].fuzzy = {0U, 0.0, 1U};
  configs[3].fallback = spg::FallbackMode::HoldLastScanned;
  configs[3].mask_style = spg::MaskStyle::SolidFill;
  for (const spg::PolicyConfig& config : configs) {
    const std::string text = spg::serialize_policy_config(config);
    CHECK(text.find('\n') == std::string::npos);
    CHECK(spg::parse_policy_config(text) == config);
  }
}

TEST_CASE("json: policy config parsing is strict", "[json][policy]") {
  const std::vector<std::string> bad = {
      R"({})",                                                  // no schema
      R"({"schema": 2})",                                       // unsupported schema
      R"({"schema": "1"})",                                     // wrong type
      R"([1])",                                                 // not an object
      R"({"schema": 1, "watchlist": ["a"], "watchlst": []})",   // misspelled field
      R"({"schema": 1, "fuzzy": {"max_edit": 1}})",             // unknown nested field
      R"({"schema": 1, "watchlist": [], "watchlist": ["a"]})",  // duplicate field
      R"({"schema": 1, "categories": ["pii:fax"]})",
      R"({"schema": 1, "categories": "face"})",
      R"({"schema": 1, "watchlist": [7]})",
      R"({"schema": 1, "watchlist": ["   "]})",
      R"({"schema": 1, "lookahead_ms": 1.5})",
      R"({"schema": 1, "lookahead_ms": -1})",
      R"({"schema": 1, "lookahead_ms": 99999})",
      R"({"schema": 1, "lookahead_ms": 200, "retro_ms": 300})",
      R"({"schema": 1, "hold_ms": 18446744073709551615})",
      R"({"schema": 1, "margin_px": -1})",
      // Range checks run on the parsed double, before narrowing to float
      // could round an invalid value into range or overflow.
      R"({"schema": 1, "margin_px": -1e-50})",
      R"({"schema": 1, "margin_ratio": -1e-50})",
      R"({"schema": 1, "margin_px": 1e300})",
      R"({"schema": 1, "margin_ratio": 1e39})",
      R"({"schema": 1, "watchlist": ["\u00a0"]})",
      R"({"schema": 1, "watchlist": ["\u2003\u200b\u3000"]})",
      R"({"schema": 1, "fuzzy": {"max_edit_ratio": 2}})",
      R"({"schema": 1, "fallback": "none"})",
      R"({"schema": 1, "mask_style": "glitter"})",
      R"({"schema": 1} trailing)",
      R"({"schema": 1)",
  };
  for (const std::string& json : bad) {
    INFO(json);
    CHECK_THROWS_AS(spg::parse_policy_config(json), spg::ContractError);
  }
}

TEST_CASE("json: errors never quote watch-list text", "[json][policy][privacy]") {
  const std::vector<std::string> templates = {
      R"({"schema": 1, "watchlist": ["@S@", ]})",          // syntax error after the entry
      R"({"schema": 1, "watchlist": ["@S@)",               // unterminated string
      R"({"schema": 1, "watchlist": [@S@]})",              // bare word
      R"({"schema": 1, "@S@": ["x"]})",                    // entry used as a field name
      R"({"schema": 1, "watchlist": ["x"], "@S@": 1, "@S@": 2})",
      R"({"schema": 1, "fuzzy": {"@S@": 1}})",
      R"({"schema": 1, "categories": ["@S@"]})",
      R"({"schema": 1, "fallback": "@S@"})",
      R"({"schema": 1, "mask_style": "@S@"})",
      R"({"schema": 1, "watchlist": ["@S@", 3]})",
      R"({"schema": 1, "watchlist": ["@S@"], "hold_ms": "@S@"})",
      R"({"schema": 1, "watchlist": ["@S@\u0000"]} @S@)",
  };
  for (const std::string& tmpl : templates) {
    const std::string json = with_sentinel(tmpl);
    const std::string message = error_of(parse_config, json);
    INFO("template: " << tmpl);
    CHECK(message.rfind("policy config: ", 0) == 0);
    CHECK(message.find("SENTINEL") == std::string::npos);
    CHECK(message.find("zq-") == std::string::npos);
    CHECK(message.find("7f3a") == std::string::npos);
  }
  // Invalid UTF-8 in an entry is rejected on input and on output without
  // quoting any byte of it.
  const std::string invalid_utf8 = "{\"schema\": 1, \"watchlist\": [\"zq-SENTINEL-\xC3\x28\"]}";
  CHECK(error_of(parse_config, invalid_utf8).find("SENTINEL") == std::string::npos);
  spg::PolicyConfig config;
  config.watchlist.emplace_back("zq-SENTINEL-\xC3\x28");
  try {
    (void)spg::serialize_policy_config(config);
    FAIL("expected ContractError");
  } catch (const spg::ContractError& e) {
    CHECK(std::string(e.what()).find("SENTINEL") == std::string::npos);
  }
}

TEST_CASE("json: ground truth fixture parses", "[json][ground-truth]") {
  const spg::GroundTruthFrame f = spg::parse_ground_truth_frame(read_fixture("ground_truth_frame.json"));
  CHECK(f.frame_index == 12U);
  CHECK(spg::to_ns(f.timestamp) == 200'000'000);
  CHECK(f.source_size == spg::Size{1920U, 1080U});
  REQUIRE(f.instances.size() == 3);
  CHECK(f.instances[0].id == "wl-0");
  CHECK(f.instances[0].category == spg::Category::Watchlist);
  CHECK(f.instances[0].box == spg::Box{{100.0F, 40.5F, 220.0F, 18.0F}, spg::Space::Source});
  CHECK(f.instances[1].category == spg::Category::PiiCard);
  CHECK(f.instances[1].visibility == 0.5F);
  CHECK(f.instances[2].is_decoy());
  CHECK(f.instances[2].visibility == 0.0F);
}

TEST_CASE("json: ground truth round trips", "[json][ground-truth][property]") {
  spg::GroundTruthFrame frame;
  frame.frame_index = 9'007'199'254'740'993ULL;  // above 2^53: must stay an exact integer
  frame.timestamp = spg::timestamp_from_ns(-16'666'667);
  frame.source_size = {1280U, 720U};
  std::uint32_t n = 0;
  for (const spg::Category c : spg::kAllCategories) {
    spg::GroundTruthInstance inst;
    inst.id = "i" + std::to_string(n);
    inst.category = c;
    const float v = static_cast<float>(n);
    inst.box = {{0.1F * v, 1.0F / (v + 3.0F), 1e-3F + v, 7.25F}, spg::Space::Source};
    inst.visibility = v / 8.0F;
    frame.instances.push_back(inst);
    ++n;
  }
  frame.instances.push_back({"decoy", std::nullopt, {{1.0F, 2.0F, 3.0F, 4.0F}, spg::Space::Source}, 1.0F});
  const std::string text = spg::serialize_ground_truth_frame(frame);
  CHECK(text.find("\"kind\":\"decoy\"") != std::string::npos);
  CHECK(spg::parse_ground_truth_frame(text) == frame);
}

TEST_CASE("json: ground truth parsing is strict", "[json][ground-truth]") {
  const std::string base_head =
      R"({"schema":1,"space":"source","frame":0,"timestamp_ns":0,"width":64,"height":64,"instances":[)";
  const std::string ok_instance = R"({"id":"a","kind":"face","box":{"x":0,"y":0,"w":1,"h":1},"visibility":1})";
  CHECK_NOTHROW(spg::parse_ground_truth_frame(base_head + ok_instance + "]}"));
  const std::vector<std::string> bad = {
      R"({"schema":1,"space":"readback","frame":0,"timestamp_ns":0,"width":64,"height":64,"instances":[]})",
      R"({"schema":1,"space":"source","frame":-1,"timestamp_ns":0,"width":64,"height":64,"instances":[]})",
      R"({"schema":1,"space":"source","frame":0,"timestamp_ns":0,"width":0,"height":64,"instances":[]})",
      R"({"schema":1,"space":"source","frame":0,"width":64,"height":64,"instances":[]})",
      base_head + R"({"id":"a","kind":"pii:fax","box":{"x":0,"y":0,"w":1,"h":1},"visibility":1}]})",
      base_head + R"({"id":"a","kind":"face","box":{"x":0,"y":0,"w":-1,"h":1},"visibility":1}]})",
      base_head + R"({"id":"a","kind":"face","box":{"x":0,"y":0,"w":1},"visibility":1}]})",
      base_head + R"({"id":"a","kind":"face","box":{"x":0,"y":0,"w":1,"h":1}}]})",
      base_head + R"({"id":"a","kind":"face","box":{"x":0,"y":0,"w":1,"h":1},"visibility":2}]})",
      base_head + R"({"id":"a","kind":"face","box":{"x":0,"y":0,"w":1,"h":1},"visibility":1.00000001}]})",
      base_head + R"({"id":"a","kind":"face","box":{"x":0,"y":0,"w":1,"h":1},"visibility":-1e-50}]})",
      base_head + R"({"id":"a","kind":"face","box":{"x":0,"y":0,"w":-1e-50,"h":1},"visibility":1}]})",
      base_head + R"({"id":"a","kind":"face","box":{"x":0,"y":0,"w":1,"h":-1e-50},"visibility":1}]})",
      base_head + R"({"id":"a","kind":"face","box":{"x":1e300,"y":0,"w":1,"h":1},"visibility":1}]})",
      base_head + R"({"id":"a","kind":"face","box":{"x":0,"y":-1e39,"w":1,"h":1},"visibility":1}]})",
      base_head + R"({"id":"","kind":"face","box":{"x":0,"y":0,"w":1,"h":1},"visibility":1}]})",
      base_head + R"({"id":"a","kind":"face","box":{"x":0,"y":0,"w":1,"h":1},"visibility":1,"text":"x"}]})",
      base_head + ok_instance + "," + ok_instance + "]}",
  };
  for (const std::string& json : bad) {
    INFO(json);
    CHECK_THROWS_AS(spg::parse_ground_truth_frame(json), spg::ContractError);
  }
  CHECK(error_of(parse_truth, with_sentinel(base_head + R"({"id":"a","@S@":1}]})")).find("SENTINEL") ==
        std::string::npos);
}

TEST_CASE("json: mask plans round trip and parse strictly", "[json][mask]") {
  const spg::MaskPlan fixture = spg::parse_mask_plan(read_fixture("mask_plan.json"));
  CHECK(spg::to_ns(fixture.frame_time) == 200'000'000);
  CHECK_FALSE(fixture.fallback);
  REQUIRE(fixture.rects.size() == 1);
  CHECK(fixture.rects[0] ==
        spg::MaskRect{{{92.0F, 32.5F, 236.0F, 34.0F}, spg::Space::Source}, spg::MaskStyle::Blur});

  spg::MaskPlan plan;
  plan.frame_time = spg::timestamp_from_ns(123'456'789);
  plan.fallback = true;
  plan.rects = {{{{0.1F, 0.2F, 0.3F, 0.4F}, spg::Space::Source}, spg::MaskStyle::Pixelate},
                {{{1e6F, 3.0F, 0.0F, 0.0F}, spg::Space::Source}, spg::MaskStyle::SolidFill}};
  CHECK(spg::parse_mask_plan(spg::serialize_mask_plan(plan)) == plan);

  plan.rects[0].box.space = spg::Space::Readback;
  CHECK_THROWS_AS(spg::serialize_mask_plan(plan), spg::ContractError);
  CHECK_THROWS_AS(
      spg::parse_mask_plan(R"({"schema":1,"space":"source","timestamp_ns":0,"fallback":0,"rects":[]})"),
      spg::ContractError);
  CHECK_THROWS_AS(spg::parse_mask_plan(
                      R"({"schema":1,"space":"source","timestamp_ns":0,"fallback":false,"rects":[{"box":{"x":0,"y":0,"w":1,"h":1},"style":"x"}]})"),
                  spg::ContractError);
  CHECK_THROWS_AS(spg::parse_mask_plan(
                      R"({"schema":1,"space":"source","timestamp_ns":0,"fallback":false,"rects":[{"box":{"x":0,"y":0,"w":-1e-50,"h":1},"style":"blur"}]})"),
                  spg::ContractError);
  CHECK_THROWS_AS(spg::parse_mask_plan(
                      R"({"schema":1,"space":"source","timestamp_ns":0,"fallback":false,"rects":[{"box":{"x":0,"y":0,"w":1e39,"h":1},"style":"blur"}]})"),
                  spg::ContractError);
  // Boundary values that are exactly in range still parse.
  CHECK(spg::parse_mask_plan(
            R"({"schema":1,"space":"source","timestamp_ns":0,"fallback":false,"rects":[{"box":{"x":-0.0,"y":0,"w":0,"h":3.4028234663852886e38},"style":"blur"}]})")
            .rects.size() == 1);
  CHECK(error_of(parse_plan, with_sentinel(R"({"schema":1,"space":"@S@"})")).find("SENTINEL") ==
        std::string::npos);
}
