// spg-cli run end to end on the pinned models (SPG-11): PNG frames with
// made-up text and a drawn cartoon face, written at test time, through the
// real detectors, recognizer, policy, scheduler and compositor. Skips when a
// model is not in the cache. Nothing here is a photo or real personal data.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "commands.hpp"
#include "image_io.hpp"
#include "inputs.hpp"
#include "spg/compose.hpp"
#include "spg/json.hpp"
#include "stages.hpp"
#include "synthetic_text.hpp"

namespace {

namespace fs = std::filesystem;

// The watch-list entry doubles as the sentinel: it must never be printed.
constexpr std::string_view kEntry = "Zorbly Quanticor";

spg::BgraImage to_bgra(const spg::test::Canvas& canvas) {
  spg::BgraImage image{canvas.width(), canvas.height(),
                       std::vector<std::uint8_t>(static_cast<std::size_t>(canvas.width()) * canvas.height() * 4)};
  for (std::uint32_t y = 0; y < canvas.height(); ++y) {
    for (std::uint32_t x = 0; x < canvas.width(); ++x) {
      const spg::test::Bgr c = canvas.at(x, y);
      std::uint8_t* p = &image.pixels[(static_cast<std::size_t>(y) * canvas.width() + x) * 4];
      p[0] = c.b;
      p[1] = c.g;
      p[2] = c.r;
      p[3] = 255;
    }
  }
  return image;
}

std::string slurp(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool contains(const spg::MaskPlan& plan, const spg::Rect& r) {
  return std::any_of(plan.rects.begin(), plan.rects.end(), [&](const spg::MaskRect& m) {
    const spg::Rect& b = m.box.rect;
    return b.x <= r.x && b.y <= r.y && b.x + b.width >= r.x + r.width && b.y + b.height >= r.y + r.height;
  });
}

bool overlaps(const spg::MaskPlan& plan, const spg::Rect& r) {
  return std::any_of(plan.rects.begin(), plan.rects.end(), [&](const spg::MaskRect& m) {
    const spg::Rect i = spg::intersection(m.box.rect, r);
    return i.width > 0.0F && i.height > 0.0F;
  });
}

}  // namespace

TEST_CASE("spg-cli run masks a watch-list line, a card number and a face on the pinned models", "[cli][model]") {
  const spg::cli::ModelPaths models = spg::cli::model_paths(SPG_CACHE_DIR);
  for (const fs::path& p : {models.faces, models.text_detector, models.text_recognizer}) {
    if (!fs::is_regular_file(p)) {
      SKIP("model not in the cache (run scripts/fetch_models.py): " << p.string());
    }
  }
  const fs::path dir = fs::path(SPG_CLI_TEST_DIR) / "pipeline";
  fs::remove_all(dir);
  fs::create_directories(dir / "frames");

  // 36 frames at 60 fps of a 1280x720 dark chat panel. The watch-list line
  // and the card appear on frame 12 and stay; a decoy line is there all along.
  constexpr std::size_t kFrames = 36;
  constexpr std::size_t kAppear = 12;
  spg::Rect entry_ink;
  spg::Rect card_ink;
  spg::Rect decoy_ink;
  for (std::size_t i = 0; i < kFrames; ++i) {
    spg::test::Canvas canvas(1280, 720, {32, 28, 24});
    canvas.rect(40, 40, 900, 400, {48, 44, 40});
    decoy_ink = spg::test::draw_text(canvas, "Weather today is sunny", 80, 120, 20.0, {230, 230, 230});
    canvas.face(1080, 520, 160);
    if (i >= kAppear) {
      entry_ink = spg::test::draw_text(canvas, "chat: " + std::string(kEntry) + " joined", 80, 200, 20.0,
                                       {230, 230, 230});
      card_ink = spg::test::draw_text(canvas, "card 4111 1111 1111 1111", 80, 280, 20.0, {230, 230, 230});
    }
    std::ostringstream name;
    name << (i < 10 ? "00000" : "0000") << i << ".png";
    spg::cli::write_png(dir / "frames" / name.str(), to_bgra(canvas));
  }
  {
    std::ofstream policy(dir / "policy.json", std::ios::binary);
    policy << R"({"schema": 1, "watchlist": [")" << kEntry << R"("]})";
  }

  const std::vector<std::string> args = {"run",     "--frames", (dir / "frames").string(), "--policy",
                                         (dir / "policy.json").string(), "--out", (dir / "plans.jsonl").string(),
                                         "--timing", (dir / "timing.json").string(), "--cache", SPG_CACHE_DIR,
                                         "--cpu",   "--write-frames", (dir / "composed").string()};
  const std::vector<std::string_view> views(args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  const int code = spg::cli::run_cli(views, out, err);
  INFO(err.str());
  REQUIRE(code == 0);

  std::vector<spg::MaskPlan> plans;
  std::istringstream lines(slurp(dir / "plans.jsonl"));
  for (std::string line; std::getline(lines, line);) {
    plans.push_back(spg::parse_mask_plan(line));
  }
  REQUIRE(plans.size() == kFrames);
  for (std::size_t i = 0; i < kFrames; ++i) {
    INFO("frame " << i);
    CHECK_FALSE(plans[i].fallback);
    CHECK(plans[i].rects.size() <= spg::kMaskRectCapacity);
    // The face is on every frame.
    CHECK(overlaps(plans[i], spg::Rect{1000, 416, 160, 208}));
    if (i >= kAppear) {
      // Covered from the first frame it shows, by retroactive masking.
      CHECK(contains(plans[i], entry_ink));
      CHECK(contains(plans[i], card_ink));
    }
    // The decoy line is not sensitive.
    CHECK_FALSE(overlaps(plans[i], decoy_ink));
  }

  const nlohmann::json timing = nlohmann::json::parse(slurp(dir / "timing.json"));
  for (const char* stage : {"decode", "face_detect", "text_detect", "recognize", "policy", "cycle", "compose"}) {
    INFO(stage);
    CHECK(timing["stages"][stage]["count"].get<int>() > 0);
  }
  CHECK(spg::cli::list_frames(dir / "composed").size() == kFrames);
  for (const std::string& text : {out.str(), err.str(), slurp(dir / "plans.jsonl"), slurp(dir / "timing.json")}) {
    CHECK(text.find(kEntry) == std::string::npos);
    CHECK(text.find("Quanticor") == std::string::npos);
  }
}
