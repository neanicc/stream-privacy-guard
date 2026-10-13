// spg-cli scrub (SPG-18) without models: run_cli() with fake stages that read
// what a frame "shows" from its first pixel, on PNG frames written to the
// build tree. The planted strings are made up; both are sentinels that must
// never reach any output or the report.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "commands.hpp"
#include "image_io.hpp"
#include "inputs.hpp"
#include "schedule_fakes.hpp"
#include "scrub.hpp"
#include "spg/category.hpp"
#include "spg/compose.hpp"
#include "stages.hpp"

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kFirst = "SECRET-SENTINEL-QX7";
constexpr std::string_view kSecond = "Mirelda Vostrikan";

// What a frame shows, stored in the blue channel of its first pixel.
enum Shows : std::uint8_t {
  kNothing = 40,     // no text at all
  kFirstSharp = 10,  // planted #1, read exactly
  kGarbled = 20,     // a region the recognizer reads as noise (a blurred line)
  kSecondNear = 30,  // planted #2 with one OCR error
  kBoth = 50,        // two regions: #1 and #2
};

std::uint8_t shows(const spg::FrameView& view) {
  return view.planes[0][0];
}

class PixelDetector final : public spg::Detector {
 public:
  [[nodiscard]] std::vector<spg::Detection> detect(const spg::FrameView& readback, spg::Size) const override {
    switch (shows(readback)) {
      case kNothing:
        return {};
      case kBoth:
        return {spg_test::text_at(30, 20, 20, 8), spg_test::text_at(4, 36, 50, 10)};
      default:
        return {spg_test::text_at(30, 20, 20, 8)};
    }
  }
};

class PixelRecognizer final : public spg::TextRecognizer {
 public:
  explicit PixelRecognizer(bool short_by_one = false) : short_by_one_(short_by_one) {}
  [[nodiscard]] std::vector<spg::Secret> recognize(const spg::FrameView& readback, spg::Size,
                                                   std::span<const spg::Detection> regions) const override {
    std::vector<spg::Secret> out;
    const std::uint8_t what = shows(readback);
    for (std::size_t i = 0; i < regions.size(); ++i) {
      if (what == kFirstSharp || (what == kBoth && i == 0)) {
        out.emplace_back("chat: " + std::string(kFirst) + " joined");
      } else if (what == kSecondNear || (what == kBoth && i == 1)) {
        out.emplace_back(std::string("from Mireldo Vostrikan"));
      } else {
        out.emplace_back(std::string("ii1 ;. xqv ll"));
      }
    }
    if (short_by_one_ && !out.empty()) {
      out.pop_back();
    }
    return out;
  }

 private:
  bool short_by_one_;
};

struct Seen {
  bool thorough = false;
  bool watchlist_only = false;
  spg::FuzzyLimits fuzzy;
  std::size_t entries = 0;
};

spg::cli::CliHooks pixel_hooks(std::shared_ptr<Seen> seen = std::make_shared<Seen>(), bool short_by_one = false) {
  spg::cli::CliHooks hooks;
  hooks.open = [seen, short_by_one](const spg::PolicyConfig& policy, const spg::cli::StageOptions& options,
                                    const fs::path&) {
    seen->thorough = options.thorough;
    spg::CategorySet only;
    only.insert(spg::Category::Watchlist);
    seen->watchlist_only = policy.categories == only;
    seen->fuzzy = policy.fuzzy;
    seen->entries = policy.watchlist.size();
    spg::cli::OpenedStages opened;
    opened.stages.text = std::make_shared<PixelDetector>();
    opened.stages.recognizer = std::make_shared<PixelRecognizer>(short_by_one);
    opened.providers = "fake";
    return opened;
  };
  return hooks;
}

spg::BgraImage frame_showing(std::uint8_t what) {
  spg::BgraImage image{64, 48, std::vector<std::uint8_t>(64 * 48 * 4, 90)};
  for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
    image.pixels[i] = 255;
  }
  image.pixels[0] = what;
  return image;
}

fs::path scratch(const std::string& name) {
  const fs::path dir = fs::path(SPG_CLI_TEST_DIR) / ("scrub-" + name);
  fs::remove_all(dir);
  fs::create_directories(dir / "frames");
  return dir;
}

void write_frames(const fs::path& dir, const std::vector<std::uint8_t>& sequence) {
  for (std::size_t i = 0; i < sequence.size(); ++i) {
    std::ostringstream name;
    name << (i < 10 ? "00000" : "0000") << i << ".png";
    spg::cli::write_png(dir / "frames" / name.str(), frame_showing(sequence[i]));
  }
}

void write_text(const fs::path& file, const std::string& text) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string slurp(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Both planted strings, with CR LF endings and blank lines between them.
void write_planted(const fs::path& file) {
  write_text(file, std::string(kFirst) + "\r\n\r\n   \n" + std::string(kSecond) + "\n");
}

struct Ran {
  int code = 0;
  std::string out;
  std::string err;
};

Ran scrub_cli(std::vector<std::string> args, const spg::cli::CliHooks& hooks = pixel_hooks()) {
  args.insert(args.begin(), "scrub");
  std::vector<std::string_view> views(args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  Ran r;
  r.code = spg::cli::run_cli(views, out, err, hooks);
  r.out = out.str();
  r.err = err.str();
  return r;
}

void never_planted(const std::string& text) {
  CHECK(text.find(kFirst) == std::string::npos);
  CHECK(text.find("SENTINEL") == std::string::npos);
  CHECK(text.find("Mirelda") == std::string::npos);
  CHECK(text.find("Vostrikan") == std::string::npos);
  CHECK(text.find("Mireldo") == std::string::npos);
}

const std::vector<std::uint8_t> kSequence = {kNothing, kFirstSharp, kFirstSharp, kGarbled, kGarbled,
                                             kSecondNear, kNothing, kBoth, kGarbled, kNothing};

}  // namespace

TEST_CASE("scrub reports every frame where a planted string is readable, by number only", "[cli][scrub]") {
  const fs::path dir = scratch("leaks");
  write_frames(dir, kSequence);
  write_planted(dir / "planted.txt");
  auto seen = std::make_shared<Seen>();
  const Ran r = scrub_cli({"--frames", (dir / "frames").string(), "--planted", (dir / "planted.txt").string(),
                           "--report", (dir / "report.json").string()},
                          pixel_hooks(seen));
  INFO(r.out << r.err);
  REQUIRE(r.code == spg::cli::kExitLeak);

  // The offline settings, the watch-list category only, both strings and the
  // High sensitivity limits by default.
  CHECK(seen->thorough);
  CHECK(seen->watchlist_only);
  CHECK(seen->entries == 2);
  CHECK(seen->fuzzy == spg::cli::scrub_fuzzy(spg::cli::ScrubSensitivity::High));

  CHECK(r.out.find("leak: frame 1 (000001.png, 0.017 s): planted #1 at x 30 y 20, 20x8\n") != std::string::npos);
  CHECK(r.out.find("leak: frame 2 (000002.png, 0.033 s): planted #1") != std::string::npos);
  // One OCR error still matches, as the policy engine would mask it.
  CHECK(r.out.find("leak: frame 5 (000005.png, 0.083 s): planted #2 at x 30 y 20") != std::string::npos);
  CHECK(r.out.find("leak: frame 7 (000007.png, 0.117 s): planted #1 at x 30 y 20") != std::string::npos);
  CHECK(r.out.find("leak: frame 7 (000007.png, 0.117 s): planted #2 at x 4 y 36, 50x10") != std::string::npos);
  // Garbled and empty frames are clean.
  for (const char* clean : {"frame 0 ", "frame 3 ", "frame 4 ", "frame 6 ", "frame 8 ", "frame 9 "}) {
    CHECK(r.out.find(std::string("leak: ") + clean) == std::string::npos);
  }
  CHECK(r.out.find("planted #1: readable in 3 frames, first 1 (0.017 s), last 7 (0.117 s)") != std::string::npos);
  CHECK(r.out.find("planted #2: readable in 2 frames, first 5") != std::string::npos);
  CHECK(r.out.find("LEAK: a planted string is readable in 4 of 10 frames") != std::string::npos);
  CHECK(r.out.find("10 frames, 8 text regions read, 2 planted strings, sensitivity high") != std::string::npos);

  const nlohmann::json report = nlohmann::json::parse(slurp(dir / "report.json"));
  CHECK(report["command"] == "scrub");
  CHECK(report["result"] == "leak");
  CHECK(report["frames"] == 10);
  CHECK(report["planted_strings"] == 2);
  CHECK(report["leak_frames"] == 4);
  CHECK(report["leak_frame_indices"] == nlohmann::json({1, 2, 5, 7}));
  REQUIRE(report["leaks"].size() == 5);
  const nlohmann::json& first = report["leaks"][0];
  CHECK(first["frame"] == 1);
  CHECK(first["file"] == "000001.png");
  CHECK(first["planted"] == nlohmann::json({1}));
  CHECK(first["box"]["x"] == 30.0);
  CHECK(first["box"]["width"] == 20.0);
  CHECK(report["planted"][0]["leak_frames"] == 3);
  CHECK(report["planted"][0]["first_frame"] == 1);
  CHECK(report["planted"][1]["last_frame"] == 7);
  CHECK(report["stages"]["decode"]["count"] == 10);
  CHECK(report["stages"]["text_detect"]["count"] == 10);

  never_planted(r.out);
  never_planted(r.err);
  never_planted(slurp(dir / "report.json"));
  // Nothing but the input frames and the report was written.
  CHECK(spg::cli::list_frames(dir / "frames").size() == kSequence.size());
  // frames/, planted.txt and report.json.
  CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator{}) == 3);
}

TEST_CASE("scrub passes a recording where nothing planted is readable", "[cli][scrub]") {
  const fs::path dir = scratch("clean");
  write_frames(dir, {kNothing, kGarbled, kGarbled, kNothing});
  write_planted(dir / "planted.txt");
  const Ran r = scrub_cli({"--frames", (dir / "frames").string(), "--planted", (dir / "planted.txt").string(),
                           "--report", (dir / "report.json").string(), "--fps", "30", "--sensitivity", "normal"});
  INFO(r.out << r.err);
  REQUIRE(r.code == spg::cli::kExitOk);
  CHECK(r.out.find("PASS: no planted string is readable in any of the 4 frames") != std::string::npos);
  CHECK(r.out.find("planted #1: not readable in any frame") != std::string::npos);
  CHECK(r.out.find("leak:") == std::string::npos);
  const nlohmann::json report = nlohmann::json::parse(slurp(dir / "report.json"));
  CHECK(report["result"] == "pass");
  CHECK(report["leak_frames"] == 0);
  CHECK(report["sensitivity"] == "normal");
  CHECK(report["fps"] == 30);
  CHECK(report["planted"][1]["first_frame"].is_null());
}

TEST_CASE("scrub output does not depend on the number of workers", "[cli][scrub]") {
  const fs::path dir = scratch("workers");
  std::vector<std::uint8_t> long_sequence;
  for (int i = 0; i < 40; ++i) {
    long_sequence.push_back(kSequence[static_cast<std::size_t>(i) % kSequence.size()]);
  }
  write_frames(dir, long_sequence);
  write_planted(dir / "planted.txt");
  const auto leak_lines = [](const std::string& out) {
    std::istringstream in(out);
    std::string lines;
    for (std::string line; std::getline(in, line);) {
      if (line.rfind("leak: ", 0) == 0 || line.rfind("planted #", 0) == 0) {
        lines += line + "\n";
      }
    }
    return lines;
  };
  const Ran one = scrub_cli({"--frames", (dir / "frames").string(), "--planted", (dir / "planted.txt").string(),
                             "--workers", "1"});
  const Ran four = scrub_cli({"--frames", (dir / "frames").string(), "--planted", (dir / "planted.txt").string(),
                              "--workers", "4"});
  REQUIRE(one.code == spg::cli::kExitLeak);
  REQUIRE(four.code == spg::cli::kExitLeak);
  CHECK(leak_lines(one.out) == leak_lines(four.out));
  CHECK(leak_lines(one.out).find("leak: frame 37 (000037.png") != std::string::npos);
  // Progress lines go to stderr.
  CHECK(four.err.find("spg-cli scrub: 40/40 frames") != std::string::npos);
}

TEST_CASE("scrub fails rather than passing a frame it could not check", "[cli][scrub]") {
  const fs::path dir = scratch("fail");
  write_frames(dir, {kNothing, kGarbled, kFirstSharp});
  write_planted(dir / "planted.txt");
  const std::string frames = (dir / "frames").string();
  const std::string planted = (dir / "planted.txt").string();
  SECTION("an unreadable frame") {
    write_text(dir / "frames" / "000001.png", "not a png");
    const Ran r = scrub_cli({"--frames", frames, "--planted", planted});
    CHECK(r.code == spg::cli::kExitFailure);
    CHECK(r.err.find("000001.png") != std::string::npos);
    CHECK(r.out.find("PASS") == std::string::npos);
  }
  SECTION("a recognizer that returns too few texts") {
    const Ran r = scrub_cli({"--frames", frames, "--planted", planted}, pixel_hooks(std::make_shared<Seen>(), true));
    CHECK(r.code == spg::cli::kExitFailure);
    CHECK(r.err.find("texts for") != std::string::npos);
  }
  SECTION("missing models") {
    spg::cli::CliHooks hooks;
    hooks.open = [](const spg::PolicyConfig&, const spg::cli::StageOptions&, const fs::path&) -> spg::cli::OpenedStages {
      throw spg::cli::CliError("cannot open the pipeline: model not found");
    };
    const Ran r = scrub_cli({"--frames", frames, "--planted", planted}, hooks);
    CHECK(r.code == spg::cli::kExitFailure);
  }
}

TEST_CASE("scrub rejects bad planted files and options without printing a string", "[cli][scrub]") {
  const fs::path dir = scratch("usage");
  write_frames(dir, {kNothing});
  const std::string frames = (dir / "frames").string();
  SECTION("usage errors") {
    write_planted(dir / "planted.txt");
    const std::string planted = (dir / "planted.txt").string();
    CHECK(scrub_cli({}).code == spg::cli::kExitUsage);
    CHECK(scrub_cli({"--frames", frames}).code == spg::cli::kExitUsage);
    CHECK(scrub_cli({"--planted", planted}).code == spg::cli::kExitUsage);
    CHECK(scrub_cli({"--frames", frames, "--planted", planted, "--sensitivity", "max"}).code == spg::cli::kExitUsage);
    CHECK(scrub_cli({"--frames", frames, "--planted", planted, "--readback-scale", "0.5"}).code ==
          spg::cli::kExitUsage);
    CHECK(scrub_cli({"--frames", frames, "--planted", planted, "--cycle-ms", "10"}).code == spg::cli::kExitUsage);
    CHECK(scrub_cli({"--frames", frames, "--planted", planted, "--workers", "0"}).code == spg::cli::kExitUsage);
    CHECK(scrub_cli({"--frames", frames, "--planted", planted, "--fps", "0"}).code == spg::cli::kExitUsage);
  }
  SECTION("a missing frame folder or planted file") {
    write_planted(dir / "planted.txt");
    CHECK(scrub_cli({"--frames", (dir / "nope").string(), "--planted", (dir / "planted.txt").string()}).code ==
          spg::cli::kExitFailure);
    CHECK(scrub_cli({"--frames", frames, "--planted", (dir / "nope.txt").string()}).code == spg::cli::kExitFailure);
  }
  SECTION("a planted file with only blank lines") {
    write_text(dir / "blank.txt", "\n  \r\n\t\n");
    const Ran r = scrub_cli({"--frames", frames, "--planted", (dir / "blank.txt").string()});
    CHECK(r.code == spg::cli::kExitFailure);
    CHECK(r.err.find("no planted string") != std::string::npos);
  }
  SECTION("a planted string that could never match is named by number") {
    write_text(dir / "bad.txt", std::string(kSecond) + "\n" + std::string(kFirst) + "\n--- !!! ---\n");
    const Ran r = scrub_cli({"--frames", frames, "--planted", (dir / "bad.txt").string()});
    CHECK(r.code == spg::cli::kExitFailure);
    CHECK(r.err.find("planted string #3") != std::string::npos);
    CHECK(r.err.find("---") == std::string::npos);
    never_planted(r.out + r.err);
  }
}

TEST_CASE("planted files split into lines and the matcher numbers its strings", "[cli][scrub]") {
  const fs::path dir = scratch("planted");
  write_text(dir / "p.txt", "  alpha one  \r\n\r\nbeta two\nlast line without newline");
  const std::vector<spg::Secret> planted = spg::cli::read_planted(dir / "p.txt");
  REQUIRE(planted.size() == 3);
  CHECK(planted[0].reveal() == "  alpha one  ");
  CHECK(planted[1].reveal() == "beta two");
  CHECK(planted[2].reveal() == "last line without newline");

  const spg::cli::PlantedMatcher matcher(planted, spg::cli::scrub_fuzzy(spg::cli::ScrubSensitivity::Normal));
  CHECK(matcher.size() == 3);
  CHECK(matcher.match(spg::Secret(std::string("Beta Two!"))) == std::vector<std::size_t>{2});
  CHECK(matcher.match(spg::Secret(std::string("alpha one / beta tw0"))) == std::vector<std::size_t>{1, 2});
  CHECK(matcher.match(spg::Secret(std::string("gamma"))).empty());

  const spg::FuzzyLimits high = spg::cli::scrub_fuzzy(spg::cli::ScrubSensitivity::High);
  CHECK(high.max_edits == 3);
  CHECK(high.min_partial_chars == 5);
  CHECK(spg::cli::scrub_fuzzy(spg::cli::ScrubSensitivity::Normal) == spg::FuzzyLimits{});
}
