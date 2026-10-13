// spg-cli scrub (SPG-18) on the pinned models: frames with made-up planted
// strings drawn at test time, some sharp and some masked the ways SPG masks
// them (the CPU compositor's blur and pixelate, an emulation of the plugin's
// GPU effect for masks and for the whole-source fallback). Scrub must flag
// exactly the sharp frames. Skips when a model is not in the cache. Nothing
// here is a photo or real personal data.
#include <algorithm>
#include <cmath>
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
#include "pipeline/mask_levels.hpp"
#include "spg/compose.hpp"
#include "spg/policy_config.hpp"
#include "stages.hpp"
#include "synthetic_text.hpp"

namespace {

namespace fs = std::filesystem;

// Planted strings; #1 doubles as the sentinel that must never be printed.
constexpr std::string_view kName = "Zorbly Quanticor";
constexpr std::string_view kStreet = "Velmora Truskett";
constexpr std::string_view kCard = "4111 1111 1111 1111";

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

// The mask the pipeline would draw over text whose ink is `ink`: grown by the
// policy's default margin. The detector's box is larger than the ink, so
// starting from the ink gives a smaller mask and finer cells than the plugin
// draws: a harder case.
spg::Rect mask_over(const spg::Rect& ink) {
  const spg::PolicyConfig policy{};
  const float grow = policy.margin_px + policy.margin_ratio * std::min(ink.width, ink.height);
  return {ink.x - grow, ink.y - grow, ink.width + 2 * grow, ink.height + 2 * grow};
}

// --- An emulation of plugin/data/effects/spg-mask.effect -------------------
// The plugin halves the frame repeatedly with bilinear sampling at each new
// texel's center (a 2x2 box filter on even sizes), then draws each mask region
// from one level over the same fraction of the output: Smooth samples it
// bilinearly (Blur), Blocks with point sampling (Pixelate). The level and the
// region come from the plugin's own mask_levels.cpp.

struct Level {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<float> bgra;
  [[nodiscard]] const float* at(std::int64_t x, std::int64_t y) const {
    x = std::clamp<std::int64_t>(x, 0, width - 1);
    y = std::clamp<std::int64_t>(y, 0, height - 1);
    return &bgra[(static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4];
  }
  // Bilinear, clamped, at normalized (u, v), as a linear_clamp sampler.
  void linear(double u, double v, float* out) const {
    const double tx = u * width - 0.5;
    const double ty = v * height - 0.5;
    const auto x0 = static_cast<std::int64_t>(std::floor(tx));
    const auto y0 = static_cast<std::int64_t>(std::floor(ty));
    const auto fx = static_cast<float>(tx - static_cast<double>(x0));
    const auto fy = static_cast<float>(ty - static_cast<double>(y0));
    for (int c = 0; c < 4; ++c) {
      const float top = at(x0, y0)[c] * (1 - fx) + at(x0 + 1, y0)[c] * fx;
      const float bottom = at(x0, y0 + 1)[c] * (1 - fx) + at(x0 + 1, y0 + 1)[c] * fx;
      out[c] = top * (1 - fy) + bottom * fy;
    }
  }
  void point(double u, double v, float* out) const {
    const auto* p = at(static_cast<std::int64_t>(u * width), static_cast<std::int64_t>(v * height));
    std::copy(p, p + 4, out);
  }
};

Level reduce(const spg::BgraImage& image, std::uint32_t level) {
  Level cur{image.width, image.height, std::vector<float>(image.pixels.begin(), image.pixels.end())};
  for (std::uint32_t k = 0; k < level; ++k) {
    const spg::Size size = spg::plugin::level_size(image.size(), k + 1);
    Level next{size.width, size.height, std::vector<float>(static_cast<std::size_t>(size.width) * size.height * 4)};
    for (std::uint32_t y = 0; y < size.height; ++y) {
      for (std::uint32_t x = 0; x < size.width; ++x) {
        cur.linear((x + 0.5) / size.width, (y + 0.5) / size.height,
                   &next.bgra[(static_cast<std::size_t>(y) * size.width + x) * 4]);
      }
    }
    cur = std::move(next);
  }
  return cur;
}

void put(spg::BgraImage& image, std::uint32_t x, std::uint32_t y, const float* bgra) {
  std::uint8_t* p = &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
  for (int c = 0; c < 4; ++c) {
    p[c] = static_cast<std::uint8_t>(std::lround(std::clamp(bgra[c], 0.0F, 255.0F)));
  }
}

// Draws `rect` (Source pixels) onto `image` as the GPU effect does: from the
// level mask_level() picks, built from the untouched frame `source` (the
// plugin reduces the frame once, before any mask is drawn), over the region
// mask_region() rounds it out to.
void gpu_mask(spg::BgraImage& image, const spg::BgraImage& source, const spg::Rect& rect, bool blocks) {
  const std::uint32_t level = spg::plugin::mask_level(rect, image.size());
  const auto region = spg::plugin::mask_region(rect, image.size(), level);
  REQUIRE(region.has_value());
  const Level texels = reduce(source, level);
  const double sx = static_cast<double>(image.width) / texels.width;
  const double sy = static_cast<double>(image.height) / texels.height;
  float px[4];
  for (std::uint32_t y = 0; y < image.height; ++y) {
    const double cy = y + 0.5;
    if (cy < region->y * sy || cy >= (region->y + region->height) * sy) {
      continue;
    }
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const double cx = x + 0.5;
      if (cx < region->x * sx || cx >= (region->x + region->width) * sx) {
        continue;
      }
      if (blocks) {
        texels.point(cx / image.width, cy / image.height, px);
      } else {
        texels.linear(cx / image.width, cy / image.height, px);
      }
      put(image, x, y, px);
    }
  }
}

// The whole-source fallback blur: whole_level(), drawn over the frame.
void gpu_whole(spg::BgraImage& image) {
  const Level texels = reduce(image, spg::plugin::whole_level(image.size()));
  float px[4];
  for (std::uint32_t y = 0; y < image.height; ++y) {
    for (std::uint32_t x = 0; x < image.width; ++x) {
      texels.linear((x + 0.5) / image.width, (y + 0.5) / image.height, px);
      put(image, x, y, px);
    }
  }
}

// --- Frames -------------------------------------------------------------------

enum class Cover { Sharp, CpuBlur, CpuPixelate, GpuBlur, GpuPixelate, CpuWhole, GpuWhole, NoPlanted, NameOnly };

struct Drawn {
  spg::BgraImage image;
  std::vector<spg::Rect> planted_ink;  // each planted line's ink
  spg::Rect decoy_ink;
};

Drawn draw_frame(Cover cover) {
  spg::test::Canvas canvas(1280, 720, {32, 28, 24});
  canvas.rect(40, 40, 1200, 660, {48, 44, 40});
  Drawn d;
  d.decoy_ink = spg::test::draw_text(canvas, "Weather today is sunny", 80, 120, 20.0, {230, 230, 230});
  if (cover != Cover::NoPlanted) {
    d.planted_ink.push_back(
        spg::test::draw_text(canvas, "chat: " + std::string(kName) + " joined", 80, 200, 20.0, {230, 230, 230}));
  }
  if (cover != Cover::NoPlanted && cover != Cover::NameOnly) {
    d.planted_ink.push_back(spg::test::draw_text(canvas, "card " + std::string(kCard), 80, 236, 20.0, {230, 230, 230}));
    d.planted_ink.push_back(spg::test::draw_text(canvas, std::string(kStreet), 80, 460, 56.0, {240, 240, 240}));
  }
  d.image = to_bgra(canvas);
  spg::MaskPlan plan;
  for (const spg::Rect& ink : d.planted_ink) {
    plan.rects.push_back(spg::MaskRect{spg::Box{mask_over(ink), spg::Space::Source},
                                       cover == Cover::CpuPixelate ? spg::MaskStyle::Pixelate : spg::MaskStyle::Blur});
  }
  switch (cover) {
    case Cover::CpuBlur:
    case Cover::CpuPixelate:
      spg::compose(d.image, plan, spg::FallbackAction::None);
      break;
    case Cover::GpuBlur:
    case Cover::GpuPixelate: {
      const spg::BgraImage source = d.image;
      for (const spg::MaskRect& m : plan.rects) {
        gpu_mask(d.image, source, m.box.rect, cover == Cover::GpuPixelate);
      }
      break;
    }
    case Cover::CpuWhole:
      plan.fallback = true;
      spg::compose(d.image, plan, spg::FallbackAction::BlurSource);
      break;
    case Cover::GpuWhole:
      gpu_whole(d.image);
      break;
    case Cover::Sharp:
    case Cover::NoPlanted:
    case Cover::NameOnly:
      break;
  }
  return d;
}

std::string slurp(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool overlaps(const nlohmann::json& box, const spg::Rect& r) {
  const spg::Rect b{box["x"].get<float>(), box["y"].get<float>(), box["width"].get<float>(),
                    box["height"].get<float>()};
  const spg::Rect i = spg::intersection(b, r);
  return i.width > 0.0F && i.height > 0.0F;
}

}  // namespace

TEST_CASE("scrub on the pinned models flags sharp planted strings and none under SPG's masks", "[cli][model][scrub]") {
  const spg::cli::ModelPaths models = spg::cli::model_paths(SPG_CACHE_DIR);
  for (const fs::path& p : {models.text_detector, models.text_recognizer}) {
    if (!fs::is_regular_file(p)) {
      SKIP("model not in the cache (run scripts/fetch_models.py): " << p.string());
    }
  }
  const fs::path dir = fs::path(SPG_CLI_TEST_DIR) / "scrub-pipeline";
  fs::remove_all(dir);
  fs::create_directories(dir / "frames");

  const std::vector<Cover> covers = {Cover::Sharp,    Cover::CpuBlur,  Cover::CpuPixelate, Cover::GpuBlur,
                                     Cover::GpuPixelate, Cover::CpuWhole, Cover::GpuWhole,    Cover::NoPlanted,
                                     Cover::NameOnly};
  std::vector<Drawn> drawn;
  for (std::size_t i = 0; i < covers.size(); ++i) {
    drawn.push_back(draw_frame(covers[i]));
    std::ostringstream name;
    name << "00000" << i << ".png";
    spg::cli::write_png(dir / "frames" / name.str(), drawn.back().image);
  }
  {
    std::ofstream planted(dir / "planted.txt", std::ios::binary);
    planted << kName << "\n" << kCard << "\n" << kStreet << "\n";
  }

  const std::vector<std::string> args = {"scrub",  "--frames", (dir / "frames").string(), "--planted",
                                         (dir / "planted.txt").string(), "--report", (dir / "report.json").string(),
                                         "--cache", SPG_CACHE_DIR, "--cpu"};
  const std::vector<std::string_view> views(args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  const int code = spg::cli::run_cli(views, out, err);
  INFO(out.str() << err.str());
  REQUIRE(code == spg::cli::kExitLeak);

  const nlohmann::json report = nlohmann::json::parse(slurp(dir / "report.json"));
  // The name and card masks overlap, as masks of neighbouring lines do.
  {
    const Drawn sharp = draw_frame(Cover::Sharp);
    const spg::Rect overlap = spg::intersection(mask_over(sharp.planted_ink[0]), mask_over(sharp.planted_ink[1]));
    REQUIRE(overlap.width > 0.0F);
    REQUIRE(overlap.height > 0.0F);
  }
  // Only the sharp frames leak: frame 0 (all three) and frame 8 (the name).
  CHECK(report["leak_frame_indices"] == nlohmann::json({0, 8}));
  CHECK(report["planted"][0]["leak_frames"] == 2);
  CHECK(report["planted"][1]["leak_frames"] == 1);
  CHECK(report["planted"][2]["leak_frames"] == 1);
  CHECK(out.str().find("planted #2: readable in 1 frame, first 0 (0.000 s)") != std::string::npos);
  for (const nlohmann::json& leak : report["leaks"]) {
    INFO(leak.dump());
    const auto frame = leak["frame"].get<std::size_t>();
    // Each leak box lies on a planted line, never on the decoy alone.
    const bool on_planted = std::any_of(drawn[frame].planted_ink.begin(), drawn[frame].planted_ink.end(),
                                        [&](const spg::Rect& ink) { return overlaps(leak["box"], ink); });
    CHECK(on_planted);
  }
  CHECK(report["text_regions"].get<int>() > 0);

  for (const std::string& text : {out.str(), err.str(), slurp(dir / "report.json")}) {
    CHECK(text.find(kName) == std::string::npos);
    CHECK(text.find("Quanticor") == std::string::npos);
    CHECK(text.find("Truskett") == std::string::npos);
    CHECK(text.find(kCard) == std::string::npos);
  }
}
