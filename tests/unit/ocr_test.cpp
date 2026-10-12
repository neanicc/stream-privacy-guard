// Text recognizer stages that need no model: CTC decoding on synthetic
// probabilities, the dictionary, line splitting, read planning (views,
// rotation, chunks), the input tensor, the region crop, the config, and the
// per-track cache. Frames are drawn here; no string is real personal data.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../integration/synthetic_frames.hpp"
#include "ocr/crop_plan.hpp"
#include "ocr/ctc_decode.hpp"
#include "ocr/recognition_cache.hpp"
#include "ocr/text_recognizer.hpp"
#include "spg/error.hpp"

namespace {

using Catch::Approx;
using spg::PixelRect;
using spg::Size;
using spg::detect::BgrImage;
namespace ocr = spg::ocr;

// Alphabet "abc": classes blank, a, b, c, space.
ocr::Alphabet abc() {
  return ocr::Alphabet::from_dictionary("a\nb\nc\n");
}

// One row per step with probability `p` on class `best` and the rest spread
// evenly over the other classes.
std::vector<float> steps_of(const std::vector<std::size_t>& best, std::size_t classes, float p = 0.9F) {
  std::vector<float> probs(best.size() * classes, (1.0F - p) / static_cast<float>(classes - 1));
  for (std::size_t t = 0; t < best.size(); ++t) {
    probs[t * classes + best[t]] = p;
  }
  return probs;
}

std::string text_of(const ocr::CtcResult& result) {
  return std::string(result.text.reveal());
}

// An image of `size` filled with `fill`.
BgrImage image(Size size, std::uint8_t fill = 255) {
  return {size, std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) * size.height, fill)};
}

void set(BgrImage& img, std::uint32_t x, std::uint32_t y, std::uint8_t value) {
  const std::size_t i = 3 * (static_cast<std::size_t>(y) * img.size.width + x);
  img.pixels[i] = img.pixels[i + 1] = img.pixels[i + 2] = value;
}

// Draws a band of "glyphs": dark vertical strokes 2 px wide every 5 px across
// rows [y0, y1), which have strong horizontal edges like text.
void glyph_band(BgrImage& img, std::uint32_t y0, std::uint32_t y1) {
  for (std::uint32_t y = y0; y < y1; ++y) {
    for (std::uint32_t x = 2; x + 2 < img.size.width; x += 5) {
      set(img, x, y, 0);
      set(img, x + 1, y, 0);
    }
  }
}

}  // namespace

// --- CTC decoding ----------------------------------------------------------

TEST_CASE("Alphabet: blank first, dictionary lines, then a space", "[ocr][ctc]") {
  const ocr::Alphabet alphabet = abc();
  REQUIRE(alphabet.size() == 5);
  CHECK(alphabet.symbol(0).empty());
  CHECK(alphabet.symbol(1) == "a");
  CHECK(alphabet.symbol(3) == "c");
  CHECK(alphabet.symbol(4) == " ");
  // CRLF lines, no final newline, multi-byte symbols.
  const ocr::Alphabet crlf = ocr::Alphabet::from_dictionary("x\r\n\xC3\xA9\r\nz");
  REQUIRE(crlf.size() == 5);
  CHECK(crlf.symbol(2) == "\xC3\xA9");
  CHECK(crlf.symbol(3) == "z");
  CHECK_THROWS_AS(ocr::Alphabet::from_dictionary(""), ocr::DecodeError);
  CHECK_THROWS_AS(ocr::Alphabet::from_dictionary("a\n\nb\n"), ocr::DecodeError);
}

TEST_CASE("CTC greedy decode collapses repeats and drops blanks", "[ocr][ctc]") {
  const ocr::Alphabet alphabet = abc();
  const std::size_t c = alphabet.size();
  // a a _ a b b _ _ c space c: adjacent repeats collapse, a blank separates.
  const std::vector<std::size_t> path{1, 1, 0, 1, 2, 2, 0, 0, 3, 4, 3};
  const ocr::CtcResult result = ocr::ctc_greedy_decode(steps_of(path, c), path.size(), alphabet);
  CHECK(text_of(result) == "aabc c");
  CHECK(result.score == Approx(0.9F));

  SECTION("all blank reads as empty with score 0") {
    const std::vector<std::size_t> blank(6, 0);
    const ocr::CtcResult empty = ocr::ctc_greedy_decode(steps_of(blank, c), blank.size(), alphabet);
    CHECK(empty.text.empty());
    CHECK(empty.score == 0.0F);
  }
  SECTION("score is the mean probability of the kept steps") {
    std::vector<float> probs = steps_of({1, 0, 2}, c);
    probs[0 * c + 1] = 0.6F;
    probs[0 * c + 2] = 0.1F;
    probs[2 * c + 2] = 1.0F;
    const ocr::CtcResult r = ocr::ctc_greedy_decode(probs, 3, alphabet);
    CHECK(text_of(r) == "ab");
    CHECK(r.score == Approx(0.8F));
  }
  SECTION("the append form adds to existing text") {
    std::string text = "x";
    const ocr::CtcStats stats = ocr::ctc_greedy_decode(steps_of({3, 3, 1}, c), 3, alphabet, text);
    CHECK(text == "xca");
    CHECK(stats.characters == 2);
  }
}

TEST_CASE("CTC decode fails closed on malformed output without quoting it", "[ocr][ctc]") {
  // The alphabet's symbols spell a sentinel; no error may contain any of it.
  const ocr::Alphabet alphabet = ocr::Alphabet::from_dictionary("zq\nSENTINEL\n7f3a\n");
  const std::size_t c = alphabet.size();
  const std::vector<std::size_t> path{1, 2, 3, 1, 2};
  const auto message = [&](std::vector<float> probs, std::size_t steps) {
    std::string text = "unchanged";
    try {
      (void)ocr::ctc_greedy_decode(probs, steps, alphabet, text);
    } catch (const ocr::DecodeError& e) {
      CHECK(text == "unchanged");
      return std::string(e.what());
    }
    return std::string("no error");
  };
  const std::vector<float> good = steps_of(path, c);
  std::vector<std::string> errors;
  for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity(), -0.5F, 1.5F}) {
    for (const std::size_t at : {std::size_t{0}, good.size() / 2, good.size() - 1}) {
      std::vector<float> probs = good;
      probs[at] = bad;
      errors.push_back(message(probs, path.size()));
    }
  }
  errors.push_back(message(good, path.size() + 1));                       // wrong step count
  errors.push_back(message({good.begin(), good.end() - 1}, path.size()));  // one value short
  errors.push_back(message(good, 0));
  for (const std::string& error : errors) {
    INFO(error);
    CHECK(error.rfind("text recognizer: output", 0) == 0);
    for (const std::string_view fragment : {"zq", "SENTINEL", "7f3a"}) {
      CHECK(error.find(fragment) == std::string::npos);
    }
  }
  // Values within rounding of [0, 1] are probabilities.
  std::vector<float> rounded = good;
  rounded[0] = -1e-4F;
  rounded[c + 2] = 1.0F + 1e-4F;
  CHECK_NOTHROW(ocr::ctc_greedy_decode(rounded, path.size(), alphabet));
}

TEST_CASE("Recognized text is a Secret", "[ocr][ctc]") {
  static_assert(std::is_same_v<decltype(ocr::Recognition::text), spg::Secret>);
  static_assert(std::is_same_v<decltype(ocr::CtcResult::text), spg::Secret>);
  SUCCEED();
}

// --- Line splitting ----------------------------------------------------------

TEST_CASE("split_lines cuts stacked lines in the middle of each gap", "[ocr][lines]") {
  BgrImage crop = image({120, 40});
  glyph_band(crop, 3, 15);   // line 1: rows 3-14
  glyph_band(crop, 21, 34);  // line 2: rows 21-33
  const std::vector<ocr::RowRange> bands = ocr::split_lines(crop);
  REQUIRE(bands.size() == 2);
  CHECK(bands[0] == ocr::RowRange{0, 18});
  CHECK(bands[1] == ocr::RowRange{18, 40});

  SECTION("three lines with one-row gaps") {
    BgrImage three = image({60, 32});
    glyph_band(three, 0, 10);
    glyph_band(three, 11, 21);
    glyph_band(three, 22, 32);
    const std::vector<ocr::RowRange> b = ocr::split_lines(three);
    REQUIRE(b.size() == 3);
    CHECK(b[0] == ocr::RowRange{0, 10});
    CHECK(b[1] == ocr::RowRange{10, 21});
    CHECK(b[2] == ocr::RowRange{21, 32});
  }
}

TEST_CASE("split_lines keeps one band for one line, dots, underlines and blanks", "[ocr][lines]") {
  const std::vector<ocr::RowRange> whole{{0, 30}};
  BgrImage one = image({100, 30});
  glyph_band(one, 8, 22);
  CHECK(ocr::split_lines(one) == whole);

  BgrImage dotted = image({100, 30});  // i-dots above a line, underline below
  glyph_band(dotted, 4, 6);
  glyph_band(dotted, 9, 22);
  glyph_band(dotted, 25, 27);
  CHECK(ocr::split_lines(dotted) == whole);

  CHECK(ocr::split_lines(image({100, 30})) == whole);
  CHECK(ocr::split_lines(image({100, 30}, 0)) == whole);
  CHECK(ocr::split_lines(image({1, 30})) == whole);
  CHECK(ocr::split_lines(image({100, 2})) == std::vector<ocr::RowRange>{{0, 2}});

  BgrImage touching = image({100, 30});  // two lines with no gap stay one band
  glyph_band(touching, 2, 15);
  glyph_band(touching, 15, 28);
  CHECK(ocr::split_lines(touching) == whole);
}

TEST_CASE("split_lines bands always cover the crop", "[ocr][lines]") {
  std::mt19937 rng(7);
  for (int trial = 0; trial < 300; ++trial) {
    const auto h = std::uniform_int_distribution<std::uint32_t>(3, 120)(rng);
    BgrImage crop = image({40, h});
    std::uint32_t y = std::uniform_int_distribution<std::uint32_t>(0, 4)(rng);
    while (y < h) {
      const std::uint32_t end = std::min(h, y + std::uniform_int_distribution<std::uint32_t>(1, 14)(rng));
      glyph_band(crop, y, end);
      y = end + std::uniform_int_distribution<std::uint32_t>(0, 6)(rng);
    }
    const std::vector<ocr::RowRange> bands = ocr::split_lines(crop);
    REQUIRE_FALSE(bands.empty());
    CHECK(bands.front().begin == 0);
    CHECK(bands.back().end == h);
    for (std::size_t i = 0; i < bands.size(); ++i) {
      CHECK(bands[i].begin < bands[i].end);
      if (i > 0) {
        CHECK(bands[i].begin == bands[i - 1].end);
      }
    }
  }
}

TEST_CASE("split_lines validates its parameters", "[ocr][lines]") {
  CHECK_THROWS_AS(ocr::split_lines(image({10, 10}), {-0.1F, 0.5F}), std::invalid_argument);
  CHECK_THROWS_AS(ocr::split_lines(image({10, 10}), {1.0F, 0.5F}), std::invalid_argument);
  CHECK_THROWS_AS(ocr::split_lines(image({10, 10}), {0.1F, 1.5F}), std::invalid_argument);
  CHECK_THROWS_AS(ocr::split_lines(BgrImage{{10, 10}, {}}), std::invalid_argument);
}

// --- Rotation and read planning ------------------------------------------------

TEST_CASE("rotate turns an image a quarter turn each way", "[ocr][plan]") {
  BgrImage img = image({3, 2}, 0);
  for (std::uint32_t y = 0; y < 2; ++y) {
    for (std::uint32_t x = 0; x < 3; ++x) {
      set(img, x, y, static_cast<std::uint8_t>(10 * y + x));
    }
  }
  const auto at = [](const BgrImage& i, std::uint32_t x, std::uint32_t y) {
    return i.pixels[3 * (static_cast<std::size_t>(y) * i.size.width + x)];
  };
  const BgrImage ccw = ocr::rotate(img, ocr::Rotation::CounterClockwise);
  REQUIRE(ccw.size == Size{2, 3});
  // numpy rot90: the right column becomes the top row.
  CHECK(at(ccw, 0, 0) == 2);
  CHECK(at(ccw, 1, 0) == 12);
  CHECK(at(ccw, 0, 2) == 0);
  const BgrImage cw = ocr::rotate(img, ocr::Rotation::Clockwise);
  REQUIRE(cw.size == Size{2, 3});
  CHECK(at(cw, 0, 0) == 10);  // the bottom-left pixel moves to the top-left
  CHECK(at(cw, 1, 0) == 0);
  const BgrImage back = ocr::rotate(cw, ocr::Rotation::CounterClockwise);
  CHECK(back.size == img.size);
  CHECK(back.pixels == img.pixels);
  CHECK(ocr::rotate(img, ocr::Rotation::None).pixels == img.pixels);
}

TEST_CASE("plan_reads: whole region, then each line, then both rotations when tall", "[ocr][plan]") {
  const ocr::ViewParams params;
  const PixelRect region{10, 20, 200, 40};
  SECTION("one line") {
    const std::vector<ocr::ReadJob> jobs = ocr::plan_reads(region, {{0, 40}}, params);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].rect == region);
    CHECK(jobs[0].rotation == ocr::Rotation::None);
    CHECK(jobs[0].width == 240);  // 48 * 200 / 40
  }
  SECTION("two lines add one view each") {
    const std::vector<ocr::ReadJob> jobs = ocr::plan_reads(region, {{0, 18}, {18, 40}}, params);
    REQUIRE(jobs.size() == 3);
    CHECK(jobs[1].rect == PixelRect{10, 20, 200, 18});
    CHECK(jobs[2].rect == PixelRect{10, 38, 200, 22});
    CHECK(jobs[1].view == 1);
    CHECK(jobs[2].view == 2);
    CHECK(jobs[1].width == 534);  // ceil(48 * 200 / 18)
  }
  SECTION("a tall region is also read turned both ways") {
    const PixelRect tall{0, 0, 20, 30};
    const std::vector<ocr::ReadJob> jobs = ocr::plan_reads(tall, {{0, 30}}, params);
    REQUIRE(jobs.size() == 3);
    CHECK(jobs[0].rotation == ocr::Rotation::None);
    CHECK(jobs[1].rotation == ocr::Rotation::CounterClockwise);
    CHECK(jobs[2].rotation == ocr::Rotation::Clockwise);
    CHECK(jobs[1].width == 72);  // 48 * 30 / 20
    CHECK(ocr::plan_reads({0, 0, 20, 29}, {{0, 29}}, params).size() == 1);
  }
  CHECK_THROWS_AS(ocr::plan_reads({0, 0, 0, 10}, {{0, 10}}, params), std::invalid_argument);
  CHECK_THROWS_AS(ocr::plan_reads(region, {{0, 20}, {20, 41}}, params), std::invalid_argument);
}

TEST_CASE("plan_reads chunks long views with overlap and covers every pixel", "[ocr][plan]") {
  std::mt19937 rng(11);
  for (int trial = 0; trial < 2000; ++trial) {
    ocr::ViewParams params;
    params.max_width = 32 * std::uniform_int_distribution<std::uint32_t>(2, 40)(rng);
    params.chunk_overlap = std::uniform_real_distribution<float>(0.0F, 0.5F)(rng);
    const auto w = std::uniform_int_distribution<std::int32_t>(1, 3000)(rng);
    const auto h = std::uniform_int_distribution<std::int32_t>(1, 300)(rng);
    const PixelRect region{std::uniform_int_distribution<std::int32_t>(0, 50)(rng), 5, w, h};
    const std::vector<ocr::ReadJob> jobs = ocr::plan_reads(region, {{0, static_cast<std::uint32_t>(h)}}, params);
    INFO("trial " << trial << " region " << w << "x" << h << " max_width " << params.max_width);
    REQUIRE_FALSE(jobs.empty());
    for (const bool upright : {true, false}) {
      std::vector<const ocr::ReadJob*> view;
      for (const ocr::ReadJob& job : jobs) {
        if ((job.rotation == ocr::Rotation::None) == upright &&
            (upright || job.rotation == ocr::Rotation::CounterClockwise)) {
          view.push_back(&job);
        }
      }
      if (view.empty()) {
        CHECK_FALSE(upright);
        continue;
      }
      const std::int32_t length = upright ? w : h;
      std::int32_t covered = 0;  // chunks are in reading order, so coverage grows
      for (std::size_t i = 0; i < view.size(); ++i) {
        const ocr::ReadJob& job = *view[i];
        CHECK(job.width >= 1);
        CHECK(job.width <= params.max_width);
        const std::int32_t start = upright ? job.rect.x - region.x : job.rect.y - region.y;
        const std::int32_t size = upright ? job.rect.width : job.rect.height;
        CHECK((upright ? job.rect.height : job.rect.width) == (upright ? h : w));
        CHECK(start <= covered);
        if (i > 0) {
          const auto overlap = static_cast<std::int32_t>(std::floor(static_cast<double>(params.chunk_overlap) * size));
          CHECK(covered - start >= std::min(overlap, size - 1));
        }
        covered = std::max(covered, start + size);
      }
      CHECK(covered == length);
    }
  }
}

// --- Tensor, crop and config --------------------------------------------------------

TEST_CASE("to_recognizer_tensor normalizes BGR planes and pads with zeros", "[ocr][tensor]") {
  BgrImage a = image({2, 2}, 0);
  a.pixels = {0, 255, 51, 255, 0, 102, 0, 0, 0, 255, 255, 255};
  const BgrImage b = image({1, 2}, 255);
  const std::vector<BgrImage> images{a, b};
  const std::vector<float> t = ocr::to_recognizer_tensor(images, 2, 3);
  REQUIRE(t.size() == 2 * 3 * 2 * 3);
  // Image 0, B plane, row 0: 0 -> -1, 255 -> 1, then padding 0.
  CHECK(t[0] == Approx(-1.0F));
  CHECK(t[1] == Approx(1.0F));
  CHECK(t[2] == 0.0F);
  CHECK(t[6 + 0] == Approx(1.0F));            // G plane, (0, 0)
  CHECK(t[12 + 0] == Approx(51 / 127.5 - 1));  // R plane, (0, 0)
  CHECK(t[18 + 0] == Approx(1.0F));            // image 1, B plane, (0, 0)
  CHECK(t[18 + 1] == 0.0F);
  CHECK_THROWS_AS(ocr::to_recognizer_tensor(images, 2, 1), spg::ContractError);
  CHECK_THROWS_AS(ocr::to_recognizer_tensor(images, 3, 3), spg::ContractError);
}

TEST_CASE("readback_crop converts through geometry and rounds outward", "[ocr][crop]") {
  const spg::ReadbackMapping half({1920, 1080}, {960, 540});
  CHECK(ocr::readback_crop({{101.0F, 51.0F, 40.0F, 11.0F}, spg::Space::Source}, half) == PixelRect{50, 25, 21, 6});
  CHECK(ocr::readback_crop({{1900.0F, 1070.0F, 100.0F, 100.0F}, spg::Space::Source}, half) ==
        PixelRect{950, 535, 10, 5});
  const PixelRect outside = ocr::readback_crop({{3000.0F, 10.0F, 5.0F, 5.0F}, spg::Space::Source}, half);
  CHECK((outside.width == 0 || outside.height == 0));
  CHECK_THROWS_AS(ocr::readback_crop({{0, 0, 5, 5}, spg::Space::Readback}, half), spg::ContractError);
  CHECK_THROWS_AS(ocr::readback_crop({{0, 0, -5, 5}, spg::Space::Source}, half), spg::ContractError);
}

TEST_CASE("TextRecognizerConfig validation names the field", "[ocr][config]") {
  CHECK_NOTHROW(ocr::validate(ocr::TextRecognizerConfig{}));
  const auto fails = [](auto mutate, std::string_view field) {
    ocr::TextRecognizerConfig config;
    mutate(config);
    try {
      ocr::validate(config);
    } catch (const ocr::TextRecognizerError& e) {
      return std::string(e.what()).find(field) != std::string::npos;
    }
    return false;
  };
  CHECK(fails([](auto& c) { c.input_height = 4; }, "input_height"));
  CHECK(fails([](auto& c) { c.time_stride = 0; }, "time_stride"));
  CHECK(fails([](auto& c) { c.min_width = 100; }, "min_width"));
  CHECK(fails([](auto& c) { c.max_width = 256; }, "max_width"));
  CHECK(fails([](auto& c) { c.max_width = 1284; }, "max_width"));
  CHECK(fails([](auto& c) { c.chunk_overlap = 0.6F; }, "chunk_overlap"));
  CHECK(fails([](auto& c) { c.chunk_overlap = std::numeric_limits<float>::quiet_NaN(); }, "chunk_overlap"));
  CHECK(fails([](auto& c) { c.tall_ratio = 0.5F; }, "tall_ratio"));
  CHECK(fails([](auto& c) { c.max_batch = 0; }, "max_batch"));
  CHECK(fails([](auto& c) { c.max_batch = 65; }, "max_batch"));
  CHECK(fails([](auto& c) { c.lines.gap_energy = 2.0F; }, "gap_energy"));
}

// --- Per-track cache ------------------------------------------------------------

namespace {

using spg::test::Bgr;
using spg::test::Canvas;

constexpr spg::Timestamp at_ms(std::int64_t ms) {
  return spg::timestamp_from_ns(ms * 1'000'000);
}

ocr::Recognition reading(std::string text) {
  return {spg::Secret(std::move(text)), 0.9F};
}

// A light panel with dark "glyph" strokes at the given columns.
Canvas strokes(const std::vector<std::uint32_t>& columns, std::uint8_t ink = 20, std::uint8_t paper = 230) {
  Canvas canvas(200, 60, {paper, paper, paper});
  for (const std::uint32_t x : columns) {
    canvas.rect(x, 20, x + 3, 36, {ink, ink, ink});
  }
  return canvas;
}

}  // namespace

namespace {

const spg::Box kBox{{20.0F, 18.0F, 80.0F, 20.0F}, spg::Space::Source};

ocr::Fingerprint print_of(const Canvas& canvas, const spg::Box& box = kBox,
                          spg::PixelFormat format = spg::PixelFormat::BGRA) {
  const spg::test::OwnedFrame frame = spg::test::to_frame(canvas, format);
  return ocr::fingerprint(frame.view, {canvas.width(), canvas.height()}, box);
}

}  // namespace

TEST_CASE("fingerprint: the exact readback crop the recognizer reads", "[ocr][cache]") {
  const ocr::Fingerprint print = print_of(strokes({30, 40, 50, 60}));
  CHECK(print.crop == Size{80, 20});
  CHECK(print.pixels.size() == 80 * 20 * 3);
  const ocr::Fingerprint outside = print_of(strokes({30}), {{500.0F, 18.0F, 80.0F, 20.0F}, spg::Space::Source});
  CHECK(outside.pixels.empty());
  CHECK_FALSE(ocr::same_content(outside, outside));
}

TEST_CASE("same_content: any change of the crop is new content", "[ocr][cache]") {
  const ocr::Fingerprint before = print_of(strokes({30, 40, 50, 60}));
  CHECK(ocr::same_content(print_of(strokes({30, 40, 50, 60})), before));
  // The same pixels moved with their box (scrolling) are the same content.
  CHECK(ocr::same_content(print_of(strokes({40, 50, 60, 70}), {{30.0F, 18.0F, 80.0F, 20.0F}, spg::Space::Source}),
                          before));
  SECTION("a different glyph") {
    CHECK_FALSE(ocr::same_content(print_of(strokes({30, 40, 53, 60})), before));
  }
  // The cases below passed the earlier thumbnail comparison (luma cells
  // within 6 levels, size within 10%) and would have reused stale text.
  SECTION("a colour change with the same brightness") {
    Canvas recoloured = strokes({30, 40, 50, 60});
    recoloured.rect(70, 20, 73, 36, {255, 230, 220});  // luma 230, as the paper
    CHECK_FALSE(ocr::same_content(print_of(recoloured), before));
  }
  SECTION("one pixel in a large crop") {
    const spg::Box large{{0.0F, 0.0F, 200.0F, 60.0F}, spg::Space::Source};
    Canvas dotted = strokes({30, 40, 50, 60});
    dotted.set(120, 50, {20, 20, 20});
    CHECK_FALSE(ocr::same_content(print_of(dotted, large), print_of(strokes({30, 40, 50, 60}), large)));
  }
  SECTION("faint text") {
    CHECK_FALSE(ocr::same_content(print_of(strokes({30, 40, 50, 60, 70}, 226, 230)),
                                  print_of(strokes({30, 40, 50, 60}, 226, 230))));
  }
  SECTION("a box one pixel wider") {
    CHECK_FALSE(ocr::same_content(print_of(strokes({30, 40, 50, 60}), {{20.0F, 18.0F, 81.0F, 20.0F},
                                                                         spg::Space::Source}),
                                  before));
  }
  SECTION("the same box after a source resize") {
    // Twice the source size, same readback frame: the Source box now covers
    // other readback pixels.
    const spg::test::OwnedFrame frame = spg::test::to_frame(strokes({30, 40, 50, 60}), spg::PixelFormat::BGRA);
    CHECK_FALSE(ocr::same_content(ocr::fingerprint(frame.view, {400, 120}, kBox), before));
  }
  SECTION("chroma only, in a subsampled format") {
    Canvas recoloured = strokes({30, 40, 50, 60});
    recoloured.rect(70, 20, 74, 36, {255, 230, 220});
    CHECK_FALSE(ocr::same_content(print_of(recoloured, kBox, spg::PixelFormat::NV12),
                                  print_of(strokes({30, 40, 50, 60}), kBox, spg::PixelFormat::NV12)));
  }
}

TEST_CASE("RecognitionCache: hits only for the same track, pixels and age", "[ocr][cache]") {
  ocr::RecognitionCacheConfig config;
  config.max_age = std::chrono::milliseconds{1000};
  ocr::RecognitionCache cache(config);
  const ocr::Fingerprint print = print_of(strokes({30, 40, 50, 60}));
  const ocr::Fingerprint other = print_of(strokes({30, 45, 50, 60}));

  CHECK_FALSE(cache.find(7, print, at_ms(0)));
  cache.store(7, print, reading("made-up text"), at_ms(100));
  REQUIRE(cache.find(7, print, at_ms(100)));
  CHECK(cache.find(7, print, at_ms(1100))->text == spg::Secret("made-up text"));
  CHECK_FALSE(cache.find(7, print, at_ms(1101)));  // too old
  CHECK_FALSE(cache.find(7, print, at_ms(99)));    // earlier than the read
  CHECK_FALSE(cache.find(8, print, at_ms(200)));   // another track
  CHECK_FALSE(cache.find(7, other, at_ms(200)));   // changed content (or a reused id)
  CHECK(cache.bytes() == print.pixels.size());

  cache.store(8, print, reading("b"), at_ms(500));
  cache.prune(at_ms(1200));  // track 7 is past max_age, 8 is not
  CHECK(cache.size() == 1);
  CHECK(cache.find(8, print, at_ms(1200)));
  cache.erase(8);
  CHECK(cache.size() == 0);
  CHECK(cache.bytes() == 0);

  SECTION("capacity drops the least recently used track") {
    config.capacity = 2;
    ocr::RecognitionCache tiny(config);
    tiny.store(1, print, reading("1"), at_ms(10));
    tiny.store(2, print, reading("2"), at_ms(20));
    CHECK(tiny.find(1, print, at_ms(25)));  // 1 is now the most recently used
    tiny.store(3, print, reading("3"), at_ms(30));
    CHECK(tiny.size() == 2);
    CHECK_FALSE(tiny.find(2, print, at_ms(30)));
    CHECK(tiny.find(1, print, at_ms(30)));
    CHECK(tiny.find(3, print, at_ms(30)));
  }
  SECTION("a store drops expired entries first") {
    cache.store(1, print, reading("1"), at_ms(2000));
    cache.store(2, print, reading("2"), at_ms(3500));  // 1 is past max_age
    CHECK(cache.size() == 1);
    CHECK(cache.bytes() == print.pixels.size());
  }
  SECTION("a crop over the per-entry bound is not cached") {
    config.max_entry_bytes = print.pixels.size() - 1;
    ocr::RecognitionCache tiny(config);
    tiny.store(1, print, reading("1"), at_ms(10));
    CHECK(tiny.size() == 0);
    CHECK_FALSE(tiny.find(1, print, at_ms(10)));
  }
  SECTION("the byte budget drops the oldest read") {
    config.max_bytes = 2 * print.pixels.size();
    ocr::RecognitionCache tiny(config);
    tiny.store(1, print, reading("1"), at_ms(10));
    tiny.store(2, print, reading("2"), at_ms(20));
    tiny.store(3, print, reading("3"), at_ms(30));
    CHECK(tiny.size() == 2);
    CHECK(tiny.bytes() == 2 * print.pixels.size());
    CHECK_FALSE(tiny.find(1, print, at_ms(30)));
  }
  SECTION("a crop over the budget is not cached and drops the track's old entry") {
    config.max_bytes = print.pixels.size();
    ocr::RecognitionCache tiny(config);
    tiny.store(1, print, reading("old"), at_ms(10));
    const ocr::Fingerprint big =
        print_of(strokes({30, 40, 50, 60}), {{0.0F, 0.0F, 200.0F, 60.0F}, spg::Space::Source});
    tiny.store(1, big, reading("new"), at_ms(20));
    CHECK(tiny.size() == 0);
    CHECK_FALSE(tiny.find(1, print, at_ms(20)));
    CHECK_FALSE(tiny.find(1, big, at_ms(20)));
  }
  SECTION("an empty crop is not cached") {
    cache.store(9, ocr::Fingerprint{}, reading(""), at_ms(10));
    CHECK_FALSE(cache.find(9, ocr::Fingerprint{}, at_ms(10)));
  }
  SECTION("config validation") {
    ocr::RecognitionCacheConfig bad;
    bad.max_age = std::chrono::milliseconds{-1};
    CHECK_THROWS_AS(ocr::RecognitionCache(bad), spg::ContractError);
    bad = {};
    bad.capacity = 0;
    CHECK_THROWS_AS(ocr::RecognitionCache(bad), spg::ContractError);
    bad = {};
    bad.max_entry_bytes = 0;
    CHECK_THROWS_AS(ocr::RecognitionCache(bad), spg::ContractError);
    bad = {};
    bad.max_bytes = 0;
    CHECK_THROWS_AS(ocr::RecognitionCache(bad), spg::ContractError);
  }
}

TEST_CASE("RecognitionCache stays within its bounds under churn", "[ocr][cache]") {
  // Many tracks, crops of many sizes, repeated ids, hits and time jumps: after
  // every operation the cache holds at most `capacity` tracks, at most
  // `max_bytes` of pixels (exactly the sum of its entries), and no entry over
  // `max_entry_bytes`; every hit is the latest store for that track and fresh.
  ocr::RecognitionCacheConfig config;
  config.capacity = 40;
  config.max_entry_bytes = 3 * 120 * 30;
  config.max_bytes = 3 * 120 * 30 * 10;
  ocr::RecognitionCache cache(config);
  std::mt19937 rng(5);
  std::vector<ocr::Fingerprint> prints;
  for (std::uint32_t w = 10; w <= 150; w += 20) {
    for (std::uint32_t h = 10; h <= 40; h += 15) {
      prints.push_back({{w, h}, std::vector<std::uint8_t>(3 * static_cast<std::size_t>(w) * h,
                                                          static_cast<std::uint8_t>(w + h))});
    }
  }
  std::vector<std::optional<std::pair<std::size_t, std::int64_t>>> latest(200);  // print index, time
  std::int64_t now = 0;
  for (int step = 0; step < 20000; ++step) {
    now += std::uniform_int_distribution<std::int64_t>(0, 30)(rng);
    const auto track = std::uniform_int_distribution<std::size_t>(0, latest.size() - 1)(rng);
    const auto p = std::uniform_int_distribution<std::size_t>(0, prints.size() - 1)(rng);
    if (std::uniform_int_distribution<int>(0, 2)(rng) == 0) {
      cache.store(track, prints[p], reading("t"), at_ms(now));
      latest[track] = std::pair{p, now};
    } else if (cache.find(track, prints[p], at_ms(now))) {
      REQUIRE(latest[track]);
      CHECK(latest[track]->first == p);
      CHECK(now - latest[track]->second <= 1000);
      CHECK(prints[p].pixels.size() <= config.max_entry_bytes);
    }
    REQUIRE(cache.size() <= config.capacity);
    REQUIRE(cache.bytes() <= config.max_bytes);
  }
  CHECK(cache.size() > 0);
}
