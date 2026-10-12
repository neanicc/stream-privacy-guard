#include <array>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "spg/category.hpp"
#include "spg/detection.hpp"
#include "spg/error.hpp"
#include "spg/frame.hpp"
#include "spg/ground_truth.hpp"
#include "spg/mask.hpp"
#include "spg/policy_config.hpp"
#include "spg/secret.hpp"
#include "spg/time.hpp"

using namespace std::chrono_literals;

// Secret text cannot be streamed, formatted or implicitly turned into a string.
template <typename T>
concept Streamable = requires(std::ostream& os, const T& value) { os << value; };
static_assert(!Streamable<spg::Secret>);
static_assert(!std::is_convertible_v<spg::Secret, std::string>);
static_assert(!std::is_convertible_v<spg::Secret, std::string_view>);
static_assert(!std::is_convertible_v<std::string, spg::Secret>);

TEST_CASE("contracts: category names and reason codes round trip", "[contracts]") {
  for (const spg::Category c : spg::kAllCategories) {
    CHECK(spg::parse_category_name(spg::category_name(c)) == c);
    CHECK(spg::parse_reason_code(spg::reason_code(c)) == c);
  }
  CHECK(spg::category_name(spg::Category::PiiCard) == "pii:card");
  CHECK(spg::reason_code(spg::Category::PiiCard) == "PII_CARD");
  CHECK(spg::reason_code(spg::Category::Watchlist) == "WATCHLIST");
  CHECK_FALSE(spg::parse_category_name("decoy").has_value());
  CHECK_FALSE(spg::parse_reason_code("pii:card").has_value());
}

TEST_CASE("contracts: CategorySet", "[contracts]") {
  spg::CategorySet set;
  CHECK(set.empty());
  set.insert(spg::Category::PiiIp);
  CHECK(set.contains(spg::Category::PiiIp));
  CHECK_FALSE(set.contains(spg::Category::Face));
  set.erase(spg::Category::PiiIp);
  CHECK(set.empty());
  for (const spg::Category c : spg::kAllCategories) {
    CHECK(spg::CategorySet::all().contains(c));
  }
  spg::Verdict verdict;
  CHECK_FALSE(verdict.sensitive());
  verdict.reasons.insert(spg::Category::Watchlist);
  CHECK(verdict.sensitive());
}

TEST_CASE("contracts: clocks", "[contracts]") {
  spg::ManualClock clock(spg::timestamp_from_ns(1000));
  CHECK(spg::to_ns(clock.now()) == 1000);
  clock.advance(5ms);
  CHECK(spg::to_ns(clock.now()) == 5'001'000);
  clock.set(spg::timestamp_from_ns(-7));
  CHECK(spg::to_ns(clock.now()) == -7);

  const spg::SteadyClock steady;
  const spg::Clock& injected = steady;
  const spg::Timestamp a = injected.now();
  CHECK(injected.now() >= a);
}

TEST_CASE("contracts: FrameView validation", "[contracts][frame]") {
  std::vector<std::uint8_t> pixels(64 * 64 * 4);
  spg::FrameView frame;
  frame.planes[0] = pixels.data();
  frame.strides[0] = 64 * 4;
  frame.width = 64;
  frame.height = 64;
  frame.format = spg::PixelFormat::BGRA;
  CHECK_NOTHROW(spg::validate(frame));
  CHECK(frame.size() == spg::Size{64U, 64U});

  SECTION("short stride") {
    frame.strides[0] = 64 * 4 - 1;
    CHECK_THROWS_AS(spg::validate(frame), spg::ContractError);
  }
  SECTION("zero size") {
    frame.height = 0;
    CHECK_THROWS_AS(spg::validate(frame), spg::ContractError);
  }
  SECTION("NV12 needs its interleaved chroma plane") {
    frame.format = spg::PixelFormat::NV12;
    frame.strides[0] = 64;
    CHECK_THROWS_AS(spg::validate(frame), spg::ContractError);
    frame.planes[1] = pixels.data();
    frame.strides[1] = 64;
    CHECK_NOTHROW(spg::validate(frame));
  }
  SECTION("I420 chroma rows round up for odd widths") {
    frame.format = spg::PixelFormat::I420;
    frame.width = 63;
    frame.strides = {63U, 31U, 32U};
    frame.planes = {pixels.data(), pixels.data(), pixels.data()};
    CHECK_THROWS_AS(spg::validate(frame), spg::ContractError);
    frame.strides[1] = 32;
    CHECK_NOTHROW(spg::validate(frame));
  }
  CHECK(spg::plane_count(spg::PixelFormat::I420) == 3);
  CHECK(spg::to_string(spg::PixelFormat::NV12) == "NV12");
}

TEST_CASE("contracts: detections must be in Source space with a valid score", "[contracts]") {
  spg::Detection d{{{1.0F, 2.0F, 3.0F, 4.0F}, spg::Space::Source}, spg::DetectionKind::TextRegion,
                   0.5F, spg::DetectorId::PpOcrDet};
  CHECK_NOTHROW(spg::validate(d));
  d.box.space = spg::Space::Readback;
  CHECK_THROWS_AS(spg::validate(d), spg::ContractError);
  d.box.space = spg::Space::Source;
  d.score = 1.5F;
  CHECK_THROWS_AS(spg::validate(d), spg::ContractError);
}

TEST_CASE("contracts: mask plans hold Source-space rects only", "[contracts]") {
  spg::MaskPlan plan;
  plan.rects.push_back({{{0.0F, 0.0F, 5.0F, 5.0F}, spg::Space::Source}, spg::MaskStyle::Pixelate});
  CHECK_NOTHROW(spg::validate(plan));
  plan.rects.push_back({{{0.0F, 0.0F, 5.0F, 5.0F}, spg::Space::ModelInput}, spg::MaskStyle::Blur});
  CHECK_THROWS_AS(spg::validate(plan), spg::ContractError);
}

TEST_CASE("contracts: policy config defaults follow the plan and validate", "[contracts][policy]") {
  const spg::PolicyConfig config;
  CHECK_NOTHROW(spg::validate(config));
  CHECK(config.lookahead == 500ms);
  CHECK(config.retro == config.lookahead);
  CHECK(config.hold == 750ms);
  CHECK(config.fallback == spg::FallbackMode::BlurSource);
  CHECK(config.categories == spg::CategorySet::all());

  spg::PolicyConfig bad = config;
  bad.retro = config.lookahead + 1ms;
  CHECK_THROWS_AS(spg::validate(bad), spg::ContractError);
  bad = config;
  bad.watchlist.emplace_back(" \t ");
  CHECK_THROWS_AS(spg::validate(bad), spg::ContractError);
  bad = config;
  bad.fuzzy.max_edit_ratio = 1.5;
  CHECK_THROWS_AS(spg::validate(bad), spg::ContractError);
}

TEST_CASE("contracts: ground truth ids are unique per frame", "[contracts][ground-truth]") {
  spg::GroundTruthFrame frame;
  frame.source_size = {1280U, 720U};
  spg::GroundTruthInstance inst;
  inst.id = "a";
  inst.category = spg::Category::Watchlist;
  inst.box = {{1.0F, 1.0F, 10.0F, 10.0F}, spg::Space::Source};
  frame.instances = {inst, inst};
  CHECK_THROWS_AS(spg::validate(frame), spg::ContractError);
  frame.instances[1].id = "b";
  CHECK_NOTHROW(spg::validate(frame));
  frame.instances[1].visibility = -0.1F;
  CHECK_THROWS_AS(spg::validate(frame), spg::ContractError);
}
