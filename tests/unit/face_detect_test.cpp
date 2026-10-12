// Face detector stages that need no model: YuNet decoding and NMS on
// synthetic tensors, the letterbox mapping back to Source space, rendering a
// frame into the model input, and the input tensor layout.
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../integration/synthetic_frames.hpp"
#include "detect/face_detector.hpp"
#include "detect/frame_image.hpp"
#include "detect/yunet.hpp"
#include "spg/error.hpp"

namespace {

using Catch::Approx;
using spg::Box;
using spg::Rect;
using spg::Size;
using spg::Space;
using spg::detect::yunet::Candidate;
using spg::detect::yunet::DecodeError;
using spg::detect::yunet::StrideOutputs;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// Zero-filled YuNet outputs for an input of `size`, writable per cell.
struct FakeOutputs {
  explicit FakeOutputs(Size size) : input(size) {
    for (std::size_t s = 0; s < 3; ++s) {
      const auto stride = static_cast<std::uint32_t>(spg::detect::yunet::kStrides[s]);
      const std::size_t cells = static_cast<std::size_t>(size.width / stride) * (size.height / stride);
      cls[s].assign(cells, 0.0F);
      obj[s].assign(cells, 0.0F);
      bbox[s].assign(4 * cells, 0.0F);
    }
  }

  // Sets one cell of the stride at index `s` (0: 8, 1: 16, 2: 32).
  void set(std::size_t s, std::uint32_t row, std::uint32_t col, float c, float o, std::array<float, 4> box) {
    const auto stride = static_cast<std::uint32_t>(spg::detect::yunet::kStrides[s]);
    const std::size_t idx = static_cast<std::size_t>(row) * (input.width / stride) + col;
    cls[s][idx] = c;
    obj[s][idx] = o;
    for (std::size_t i = 0; i < 4; ++i) {
      bbox[s][4 * idx + i] = box[i];
    }
  }

  [[nodiscard]] std::array<StrideOutputs, 3> views() const {
    std::array<StrideOutputs, 3> out;
    for (std::size_t s = 0; s < 3; ++s) {
      out[s] = {cls[s], obj[s], bbox[s]};
    }
    return out;
  }

  [[nodiscard]] std::vector<Candidate> decode(float threshold) const {
    const auto v = views();
    return spg::detect::yunet::decode(v, input, threshold);
  }

  Size input;
  std::array<std::vector<float>, 3> cls;
  std::array<std::vector<float>, 3> obj;
  std::array<std::vector<float>, 3> bbox;
};

Candidate candidate(Rect rect, float score) { return {{rect, Space::ModelInput}, score}; }

// `outer` contains `inner` (edges compared exactly).
bool contains(const Rect& outer, const Rect& inner) {
  return outer.x <= inner.x && outer.y <= inner.y &&
         static_cast<double>(outer.x) + outer.width >= static_cast<double>(inner.x) + inner.width &&
         static_cast<double>(outer.y) + outer.height >= static_cast<double>(inner.y) + inner.height;
}

void check_rect(const Rect& actual, const Rect& expected, double margin = 1e-3) {
  CHECK(actual.x == Approx(expected.x).margin(margin));
  CHECK(actual.y == Approx(expected.y).margin(margin));
  CHECK(actual.width == Approx(expected.width).margin(margin));
  CHECK(actual.height == Approx(expected.height).margin(margin));
}

spg::SpaceMap face_map(Size source, Size readback, Size input = {640, 640}) {
  return {spg::ReadbackMapping(source, readback), spg::detect::face_input_mapping(readback, input)};
}

}  // namespace

// Decoding ---------------------------------------------------------------------

TEST_CASE("YuNet decode finds nothing in all-zero outputs", "[face][decode]") {
  const FakeOutputs outputs({640, 640});
  CHECK(outputs.decode(0.6F).empty());
  // Zero class and objectness score 0, which only a zero threshold keeps.
  CHECK(outputs.decode(0.0F).size() == 6400 + 1600 + 400);
}

TEST_CASE("YuNet decode places a cell's box in ModelInput space", "[face][decode]") {
  FakeOutputs outputs({64, 96});  // stride 16: 4 columns, 6 rows
  outputs.set(1, 3, 2, 0.81F, 1.0F, {0.5F, 0.25F, std::log(2.0F), std::log(3.0F)});
  const auto found = outputs.decode(0.5F);
  REQUIRE(found.size() == 1);
  CHECK(found[0].box.space == Space::ModelInput);
  CHECK(found[0].score == Approx(0.9F));
  // center ((2, 3) + (0.5, 0.25)) * 16 = (40, 52); size (2, 3) * 16 = (32, 48)
  check_rect(found[0].box.rect, {24.0F, 28.0F, 32.0F, 48.0F});
}

TEST_CASE("YuNet decode decodes every stride", "[face][decode]") {
  FakeOutputs outputs({640, 640});
  outputs.set(0, 10, 20, 1.0F, 1.0F, {0.0F, 0.0F, 0.0F, 0.0F});
  outputs.set(1, 10, 20, 1.0F, 1.0F, {0.0F, 0.0F, 0.0F, 0.0F});
  outputs.set(2, 10, 15, 1.0F, 1.0F, {0.0F, 0.0F, 0.0F, 0.0F});
  const auto found = outputs.decode(0.5F);
  REQUIRE(found.size() == 3);
  check_rect(found[0].box.rect, {156.0F, 76.0F, 8.0F, 8.0F});
  check_rect(found[1].box.rect, {312.0F, 152.0F, 16.0F, 16.0F});
  check_rect(found[2].box.rect, {464.0F, 304.0F, 32.0F, 32.0F});
}

TEST_CASE("YuNet decode clamps scores and keeps the threshold inclusive", "[face][decode]") {
  FakeOutputs outputs({64, 64});
  outputs.set(0, 0, 0, 1.5F, 2.0F, {});     // clamps to 1
  outputs.set(0, 0, 1, 0.25F, 1.0F, {});    // exactly 0.5
  outputs.set(0, 0, 2, -3.0F, 1.0F, {});    // clamps to 0
  outputs.set(0, 0, 3, 0.2F, 1.0F, {});     // just under 0.5
  const auto found = outputs.decode(0.5F);
  REQUIRE(found.size() == 2);
  CHECK(found[0].score == 1.0F);
  CHECK(found[1].score == 0.5F);
}

TEST_CASE("YuNet decode fails closed on malformed outputs", "[face][decode]") {
  SECTION("a NaN score anywhere") {
    FakeOutputs outputs({64, 64});
    outputs.set(2, 1, 1, kNaN, 0.0F, {});
    CHECK_THROWS_AS(outputs.decode(0.6F), DecodeError);
  }
  SECTION("an infinite objectness") {
    FakeOutputs outputs({64, 64});
    outputs.set(1, 0, 0, 0.0F, kInf, {});
    CHECK_THROWS_AS(outputs.decode(0.6F), DecodeError);
  }
  SECTION("a kept candidate whose size overflows") {
    FakeOutputs outputs({64, 64});
    outputs.set(0, 2, 2, 1.0F, 1.0F, {0.0F, 0.0F, 1000.0F, 0.0F});
    CHECK_THROWS_AS(outputs.decode(0.6F), DecodeError);
  }
  SECTION("a kept candidate with a NaN offset") {
    FakeOutputs outputs({64, 64});
    outputs.set(0, 2, 2, 1.0F, 1.0F, {kNaN, 0.0F, 0.0F, 0.0F});
    CHECK_THROWS_AS(outputs.decode(0.6F), DecodeError);
  }
  SECTION("a kept candidate with an infinite offset") {
    FakeOutputs outputs({64, 64});
    outputs.set(0, 2, 2, 1.0F, 1.0F, {0.0F, -kInf, 0.0F, 0.0F});
    CHECK_THROWS_AS(outputs.decode(0.6F), DecodeError);
  }
  SECTION("a kept candidate with a -inf log size (exp gives zero)") {
    FakeOutputs outputs({64, 64});
    outputs.set(0, 2, 2, 1.0F, 1.0F, {0.0F, 0.0F, -kInf, 0.0F});
    CHECK_THROWS_AS(outputs.decode(0.6F), DecodeError);
    outputs.set(0, 2, 2, 1.0F, 1.0F, {0.0F, 0.0F, 0.0F, -kInf});
    CHECK_THROWS_AS(outputs.decode(0.6F), DecodeError);
  }
  SECTION("a kept candidate whose size underflows to zero") {
    FakeOutputs outputs({64, 64});
    outputs.set(0, 2, 2, 1.0F, 1.0F, {0.0F, 0.0F, -1000.0F, 0.0F});
    CHECK_THROWS_AS(outputs.decode(0.6F), DecodeError);
    // exp(-110) * 8 is about 1e-47: positive in double, zero as a float.
    outputs.set(0, 2, 2, 1.0F, 1.0F, {0.0F, 0.0F, 0.0F, -110.0F});
    CHECK_THROWS_AS(outputs.decode(0.6F), DecodeError);
  }
  SECTION("an output of the wrong size") {
    FakeOutputs outputs({64, 64});
    outputs.bbox[1].pop_back();
    CHECK_THROWS_AS(outputs.decode(0.6F), DecodeError);
  }
  SECTION("an input size that is not a multiple of 32") {
    const FakeOutputs outputs({64, 64});
    const auto views = outputs.views();
    CHECK_THROWS_AS(spg::detect::yunet::decode(views, {48, 64}, 0.6F), DecodeError);
    CHECK_THROWS_AS(spg::detect::yunet::decode(views, {0, 64}, 0.6F), DecodeError);
  }
}

TEST_CASE("YuNet decode keeps a tiny but positive box", "[face][decode]") {
  FakeOutputs outputs({64, 64});
  outputs.set(0, 2, 2, 1.0F, 1.0F, {0.0F, 0.0F, -10.0F, -10.0F});
  const auto found = outputs.decode(0.6F);
  REQUIRE(found.size() == 1);
  CHECK(found[0].box.rect.width > 0.0F);
  CHECK(found[0].box.rect.height > 0.0F);
}

TEST_CASE("YuNet decode ignores a garbage box below the threshold", "[face][decode]") {
  FakeOutputs outputs({64, 64});
  outputs.set(0, 2, 2, 0.1F, 0.1F, {kNaN, kInf, 1000.0F, kNaN});
  CHECK(outputs.decode(0.6F).empty());
}

// Session outputs to decode() input -------------------------------------------

namespace {

// Session outputs for a 64x64 input in YuNet's layout, cls/obj/bbox per stride
// in that order, so output (3 * s + k) is kind k of stride s.
std::vector<spg::runtime::Tensor> yunet_tensors() {
  std::vector<spg::runtime::Tensor> tensors;
  for (const int stride : spg::detect::yunet::kStrides) {
    const std::int64_t cells = (64 / stride) * (64 / stride);
    for (const std::int64_t width : {1, 1, 4}) {
      tensors.push_back({{1, cells, width}, std::vector<float>(static_cast<std::size_t>(cells * width), 0.0F)});
    }
  }
  return tensors;
}

spg::detect::OutputIndex in_order() {
  spg::detect::OutputIndex index{};
  for (std::size_t s = 0; s < index.size(); ++s) {
    for (std::size_t k = 0; k < 3; ++k) {
      index[s][k] = 3 * s + k;
    }
  }
  return index;
}

}  // namespace

TEST_CASE("Session outputs in YuNet's layout become decode() input", "[face][outputs]") {
  auto tensors = yunet_tensors();
  tensors[3 * 1 + 2].data[4 * 5 + 2] = 0.5F;  // stride 16, bbox of cell 5, log w
  const auto strides = spg::detect::stride_outputs(tensors, in_order(), {64, 64});
  CHECK(strides[0].cls.size() == 64);
  CHECK(strides[1].bbox.size() == 4 * 16);
  CHECK(strides[1].bbox[4 * 5 + 2] == 0.5F);
  CHECK(strides[2].obj.size() == 4);
  CHECK(spg::detect::yunet::decode(strides, {64, 64}, 0.6F).empty());
}

TEST_CASE("Session outputs with the right count but the wrong shape fail closed", "[face][outputs]") {
  auto tensors = yunet_tensors();
  SECTION("bbox transposed to [1, 4, cells]") {
    tensors[3 * 0 + 2].shape = {1, 4, 64};
  }
  SECTION("bbox flattened to [1, 4 * cells, 1]") {
    tensors[3 * 2 + 2].shape = {1, 16, 1};
  }
  SECTION("cls as [cells, 1, 1]") {
    tensors[3 * 1 + 0].shape = {16, 1, 1};
  }
  SECTION("obj without its batch dimension") {
    tensors[3 * 1 + 1].shape = {16, 1};
  }
  SECTION("bbox with an extra dimension") {
    tensors[3 * 2 + 2].shape = {1, 4, 4, 1};
  }
  CHECK_THROWS_AS(spg::detect::stride_outputs(tensors, in_order(), {64, 64}), DecodeError);
}

TEST_CASE("Session outputs missing a tensor fail closed", "[face][outputs]") {
  auto tensors = yunet_tensors();
  tensors.pop_back();
  CHECK_THROWS_AS(spg::detect::stride_outputs(tensors, in_order(), {64, 64}), DecodeError);
}

// Non-maximum suppression -----------------------------------------------------

TEST_CASE("NMS keeps the best of overlapping candidates", "[face][nms]") {
  using spg::detect::yunet::suppress;
  CHECK(suppress({}, 0.3F, 10).empty());

  const auto kept = suppress({candidate({0, 0, 10, 10}, 0.7F), candidate({1, 0, 10, 10}, 0.9F),
                              candidate({100, 100, 10, 10}, 0.8F)},
                             0.3F, 10);
  REQUIRE(kept.size() == 2);
  CHECK(kept[0].score == 0.9F);
  CHECK(kept[0].box.rect.x == 1.0F);
  CHECK(kept[1].score == 0.8F);
}

TEST_CASE("NMS suppresses only above the IoU threshold", "[face][nms]") {
  using spg::detect::yunet::suppress;
  // IoU of these two is exactly 0.5.
  const std::vector<Candidate> pair{candidate({0, 0, 10, 10}, 0.9F), candidate({0, 0, 10, 5}, 0.8F)};
  CHECK(suppress(pair, 0.5F, 10).size() == 2);
  CHECK(suppress(pair, 0.49F, 10).size() == 1);
  CHECK(suppress(pair, 1.0F, 10).size() == 2);
}

TEST_CASE("NMS considers at most top_k candidates and breaks ties by order", "[face][nms]") {
  using spg::detect::yunet::suppress;
  std::vector<Candidate> many;
  for (int i = 0; i < 5; ++i) {
    many.push_back(candidate({static_cast<float>(100 * i), 0, 10, 10}, 0.8F));
  }
  const auto kept = suppress(many, 0.3F, 3);
  REQUIRE(kept.size() == 3);
  CHECK(kept[0].box.rect.x == 0.0F);
  CHECK(kept[1].box.rect.x == 100.0F);
  CHECK(kept[2].box.rect.x == 200.0F);
}

// Mapping to Source space -----------------------------------------------------

TEST_CASE("The face input mapping letterboxes the readback frame top-left", "[face][mapping]") {
  const auto landscape = spg::detect::face_input_mapping({960, 540}, {640, 640});
  CHECK(landscape.fit() == spg::Fit::LetterboxTopLeft);
  CHECK(landscape.scale_x() == Approx(2.0 / 3.0));
  CHECK(landscape.scale_y() == Approx(2.0 / 3.0));
  CHECK(landscape.pad_x() == 0.0);
  CHECK(landscape.pad_y() == 0.0);
  const auto portrait = spg::detect::face_input_mapping({540, 960}, {640, 640});
  CHECK(portrait.scale_x() == Approx(2.0 / 3.0));
  const auto small = spg::detect::face_input_mapping({320, 160}, {640, 640});
  CHECK(small.scale_x() == Approx(2.0));
  CHECK_THROWS_AS(spg::detect::face_input_mapping({0, 540}, {640, 640}), spg::ContractError);
}

TEST_CASE("Face candidates map to Source space through the readback and letterbox", "[face][mapping]") {
  struct Case {
    Size source;
    Size readback;
    Rect model;
    Rect expected;
  };
  const Case c = GENERATE(
      // 1080p read back at half size: model -> readback x1.5 -> source x2.
      Case{{1920, 1080}, {960, 540}, {100, 50, 40, 60}, {300, 150, 120, 180}},
      // Not downscaled: the model is 1/2 of a 1280x720 source.
      Case{{1280, 720}, {1280, 720}, {100, 50, 40, 60}, {200, 100, 80, 120}},
      // Readback squashes 16:9 to 4:3; the letterbox scale is 1 (480 rows fit).
      Case{{1920, 1080}, {640, 480}, {64, 48, 32, 24}, {192, 108, 96, 54}},
      // Portrait source: content is 360 columns wide.
      Case{{1080, 1920}, {540, 960}, {30, 600, 60, 30}, {90, 1800, 180, 90}});
  CAPTURE(c.source.width, c.source.height, c.readback.width, c.readback.height);
  const spg::SpaceMap map = face_map(c.source, c.readback);
  const std::array<Candidate, 1> in{candidate(c.model, 0.75F)};
  const auto out = spg::detect::to_source_detections(in, map);
  REQUIRE(out.size() == 1);
  CHECK(out[0].box.space == Space::Source);
  CHECK(out[0].kind == spg::DetectionKind::Face);
  CHECK(out[0].detector == spg::DetectorId::YuNet);
  CHECK(out[0].score == 0.75F);
  check_rect(out[0].box.rect, c.expected);
  // Rounding only ever grows the box (spg::convert rounds outward).
  CHECK(contains(out[0].box.rect, c.expected));
}

TEST_CASE("Face boxes are clipped to the source and dropped in the padding", "[face][mapping]") {
  // 1280x720 source, not downscaled: model content is 640x360, padding below.
  const spg::SpaceMap map = face_map({1280, 720}, {1280, 720});
  const std::array<Candidate, 4> in{
      candidate({620, 340, 40, 40}, 0.9F),  // past the right and bottom edges
      candidate({-10, -20, 30, 40}, 0.8F),  // before the left and top edges
      candidate({100, 400, 40, 40}, 0.7F),  // wholly in the letterbox padding
      candidate({100, 100, 40, 40}, 0.6F),  // inside
  };
  const auto out = spg::detect::to_source_detections(in, map);
  REQUIRE(out.size() == 3);
  check_rect(out[0].box.rect, {1240, 680, 40, 40});
  check_rect(out[1].box.rect, {0, 0, 40, 40});
  check_rect(out[2].box.rect, {200, 200, 80, 80});
  CHECK(out[2].score == 0.6F);
}

TEST_CASE("Mapped face boxes always contain the exact image of the model box", "[face][mapping]") {
  // Fractional scales: 1366x768 read back at 683x384, so model px -> source px is 1366/640.
  const spg::SpaceMap map = face_map({1366, 768}, {683, 384});
  const double k = 1366.0 / 640.0;
  for (int i = 0; i < 200; ++i) {
    const Rect model{3.7F * static_cast<float>(i % 17) + 0.13F, 1.9F * static_cast<float>(i % 23) + 0.31F,
                     10.0F + 0.77F * static_cast<float>(i % 11), 12.0F + 1.13F * static_cast<float>(i % 7)};
    const std::array<Candidate, 1> in{candidate(model, 0.9F)};
    const auto out = spg::detect::to_source_detections(in, map);
    REQUIRE(out.size() == 1);
    const Rect& r = out[0].box.rect;
    CHECK(static_cast<double>(r.x) <= model.x * k);
    CHECK(static_cast<double>(r.y) <= model.y * k);
    CHECK(static_cast<double>(r.x) + r.width >= (static_cast<double>(model.x) + model.width) * k);
    CHECK(static_cast<double>(r.y) + r.height >= (static_cast<double>(model.y) + model.height) * k);
  }
}

// Rendering the model input ---------------------------------------------------

namespace {

spg::detect::BgrImage render(const spg::test::OwnedFrame& frame, Size input = {640, 640}) {
  return spg::detect::render_model_input(frame.view, spg::detect::face_input_mapping(frame.view.size(), input));
}

spg::test::Bgr pixel(const spg::detect::BgrImage& image, std::uint32_t x, std::uint32_t y) {
  const std::size_t i = 3 * (static_cast<std::size_t>(y) * image.size.width + x);
  return {image.pixels[i], image.pixels[i + 1], image.pixels[i + 2]};
}

void check_color(spg::test::Bgr actual, spg::test::Bgr expected, int tolerance) {
  CHECK(std::abs(int{actual.b} - int{expected.b}) <= tolerance);
  CHECK(std::abs(int{actual.g} - int{expected.g}) <= tolerance);
  CHECK(std::abs(int{actual.r} - int{expected.r}) <= tolerance);
}

}  // namespace

TEST_CASE("A frame renders into the top-left of the model input with black padding", "[face][render]") {
  const auto format = GENERATE(spg::PixelFormat::BGRA, spg::PixelFormat::NV12, spg::PixelFormat::I420);
  CAPTURE(spg::to_string(format));
  const spg::test::Bgr color{40, 120, 200};
  spg::test::Canvas canvas(1280, 720, color);
  canvas.rect(640, 360, 960, 540, {255, 255, 255});  // model [320, 480) x [180, 270)
  const auto frame = spg::test::to_frame(canvas, format);
  const auto image = render(frame);
  REQUIRE(image.size == Size{640, 640});
  REQUIRE(image.pixels.size() == 3U * 640U * 640U);
  // YUV round trips lose a little to rounding and chroma averaging.
  const int tolerance = format == spg::PixelFormat::BGRA ? 0 : 3;
  check_color(pixel(image, 10, 10), color, tolerance);
  check_color(pixel(image, 639, 359), color, tolerance);
  check_color(pixel(image, 400, 220), {255, 255, 255}, tolerance);
  check_color(pixel(image, 330, 175), color, tolerance);
  check_color(pixel(image, 0, 360), {0, 0, 0}, 0);
  check_color(pixel(image, 639, 639), {0, 0, 0}, 0);
}

TEST_CASE("Rendering reads rows by stride, never the padding bytes", "[face][render]") {
  // to_frame() fills row padding with 0xFF; a black frame must stay black.
  const auto format = GENERATE(spg::PixelFormat::BGRA, spg::PixelFormat::NV12, spg::PixelFormat::I420);
  CAPTURE(spg::to_string(format));
  const auto frame = spg::test::to_frame(spg::test::Canvas(333, 211), format);
  const auto image = render(frame);
  for (std::size_t i = 0; i < image.pixels.size(); ++i) {
    if (image.pixels[i] > 1) {
      FAIL("byte " << i << " is " << int{image.pixels[i]});
    }
  }
}

TEST_CASE("Rendering handles odd sizes and upscaling", "[face][render]") {
  const auto format = GENERATE(spg::PixelFormat::BGRA, spg::PixelFormat::NV12, spg::PixelFormat::I420);
  CAPTURE(spg::to_string(format));
  const spg::test::Bgr color{90, 160, 30};
  const auto frame = spg::test::to_frame(spg::test::Canvas(33, 17, color), format);
  const auto image = render(frame);
  // scale = 640 / 33; content is 640 x round(17 * 640 / 33) = 640 x 330.
  const int tolerance = format == spg::PixelFormat::BGRA ? 0 : 3;
  check_color(pixel(image, 639, 329), color, tolerance);
  check_color(pixel(image, 0, 330), {0, 0, 0}, 0);
}

TEST_CASE("Rendering rejects invalid frames and crops", "[face][render]") {
  const auto frame = spg::test::to_frame(spg::test::Canvas(64, 48), spg::PixelFormat::BGRA);
  using spg::ContractError;
  using spg::ModelInputMapping;
  using spg::detect::render_model_input;
  CHECK_THROWS_AS(render_model_input(frame.view, ModelInputMapping({0.5F, 0, 32, 32}, {64, 64}, spg::Fit::Stretch)),
                  ContractError);
  CHECK_THROWS_AS(render_model_input(frame.view, ModelInputMapping({40, 0, 32, 32}, {64, 64}, spg::Fit::Stretch)),
                  ContractError);
  // A tile crop inside the frame is fine.
  CHECK(render_model_input(frame.view, ModelInputMapping({32, 16, 32, 32}, {64, 64}, spg::Fit::Stretch)).size ==
        Size{64, 64});

  spg::FrameView missing = frame.view;
  missing.planes[0] = nullptr;
  CHECK_THROWS_AS(render(spg::test::OwnedFrame{missing}), ContractError);
  spg::FrameView short_stride = frame.view;
  short_stride.strides[0] = 4 * 64 - 1;
  CHECK_THROWS_AS(render(spg::test::OwnedFrame{short_stride}), ContractError);
}

TEST_CASE("A synthetic frame moves with its planes and cannot be copied", "[face][render]") {
  static_assert(!std::is_copy_constructible_v<spg::test::OwnedFrame>);
  static_assert(!std::is_copy_assignable_v<spg::test::OwnedFrame>);
  static_assert(std::is_nothrow_move_constructible_v<spg::test::OwnedFrame>);
  const auto format = GENERATE(spg::PixelFormat::BGRA, spg::PixelFormat::NV12, spg::PixelFormat::I420);
  CAPTURE(spg::to_string(format));
  const spg::test::Bgr color{40, 120, 200};
  std::optional<spg::test::OwnedFrame> original(spg::test::to_frame(spg::test::Canvas(64, 48, color), format));
  const spg::test::OwnedFrame moved = std::move(*original);
  original.reset();  // the moved-to view must not point into the original
  const int tolerance = format == spg::PixelFormat::BGRA ? 0 : 3;
  check_color(pixel(render(moved), 5, 5), color, tolerance);
}

TEST_CASE("The input tensor is planar B, G, R with raw byte values", "[face][tensor]") {
  spg::detect::BgrImage image{{2, 1}, {1, 2, 3, 4, 5, 6}};
  CHECK(spg::detect::to_input_tensor(image) == std::vector<float>{1, 4, 2, 5, 3, 6});
  image.pixels.pop_back();
  CHECK_THROWS_AS(spg::detect::to_input_tensor(image), spg::ContractError);
}
