// The one module that converts boxes between coordinate spaces.
#include "spg/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "spg/error.hpp"

namespace spg {

namespace {

// Per-axis map from Source coordinates: v_space = v_source * scale + offset.
struct AxisMap {
  double scale = 1.0;
  double offset = 0.0;
};

struct SpaceAffine {
  AxisMap x;
  AxisMap y;
};

SpaceAffine affine_from_source(Space space, const SpaceMap& map) {
  const Size src = map.readback.source();
  const Size rb = map.readback.readback();
  const double rb_sx = static_cast<double>(rb.width) / static_cast<double>(src.width);
  const double rb_sy = static_cast<double>(rb.height) / static_cast<double>(src.height);
  switch (space) {
    case Space::Source:
      return {};
    case Space::Readback:
      return {{rb_sx, 0.0}, {rb_sy, 0.0}};
    case Space::ModelInput: {
      if (!map.model_input) {
        throw ContractError("geometry: ModelInput conversion needs a model input mapping");
      }
      const ModelInputMapping& m = *map.model_input;
      const Rect crop = m.crop();
      // model = (source * rb_s - crop) * scale + pad
      return {{rb_sx * m.scale_x(), m.pad_x() - static_cast<double>(crop.x) * m.scale_x()},
              {rb_sy * m.scale_y(), m.pad_y() - static_cast<double>(crop.y) * m.scale_y()}};
    }
  }
  throw ContractError("geometry: unknown space");
}

bool has_zero_dimension(Size size) noexcept {
  return size.width == 0 || size.height == 0;
}

// Narrows one axis [lo, hi] to a float origin and length whose span contains
// it, as enclosing_pixels() measures the span (origin + length in double).
// Rounding each field to nearest instead can move an edge inward by an ulp,
// which can uncover a pixel column that the exact box touches.
std::pair<float, float> narrow_outward(double lo, double hi) {
  constexpr double kFloatMax = std::numeric_limits<float>::max();
  if (!(std::abs(lo) <= kFloatMax && std::abs(hi) <= kFloatMax)) {
    throw ContractError("geometry: converted box is outside the float range");
  }
  constexpr float kInf = std::numeric_limits<float>::infinity();
  float origin = static_cast<float>(lo);
  if (static_cast<double>(origin) > lo) {
    origin = std::nextafter(origin, -kInf);
  }
  const double span = std::max(hi - static_cast<double>(origin), 0.0);
  if (span > kFloatMax) {
    throw ContractError("geometry: converted box is outside the float range");
  }
  float length = static_cast<float>(span);
  while (static_cast<double>(origin) + static_cast<double>(length) < hi) {
    length = std::nextafter(length, kInf);
  }
  if (!std::isfinite(origin) || !std::isfinite(length)) {
    throw ContractError("geometry: converted box is outside the float range");
  }
  return {origin, length};
}

}  // namespace

std::string_view to_string(Space space) noexcept {
  switch (space) {
    case Space::Source:
      return "source";
    case Space::Readback:
      return "readback";
    case Space::ModelInput:
      return "model_input";
  }
  return "unknown";
}

bool is_valid(const Rect& rect) noexcept {
  return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
         std::isfinite(rect.height) && rect.width >= 0.0F && rect.height >= 0.0F;
}

float area(const Rect& rect) noexcept {
  return rect.width * rect.height;
}

Rect intersection(const Rect& a, const Rect& b) noexcept {
  const float x0 = std::max(a.x, b.x);
  const float y0 = std::max(a.y, b.y);
  const float x1 = std::min(a.x + a.width, b.x + b.width);
  const float y1 = std::min(a.y + a.height, b.y + b.height);
  if (x1 <= x0 || y1 <= y0) {
    return {x0, y0, 0.0F, 0.0F};
  }
  return {x0, y0, x1 - x0, y1 - y0};
}

float iou(const Rect& a, const Rect& b) noexcept {
  const float inter = area(intersection(a, b));
  const float uni = area(a) + area(b) - inter;
  if (uni <= 0.0F) {
    return 0.0F;
  }
  return std::clamp(inter / uni, 0.0F, 1.0F);
}

PixelRect enclosing_pixels(const Rect& rect, Size bounds) noexcept {
  if (!is_valid(rect)) {
    return {};
  }
  // PixelRect is int32; clamp so a bounds dimension above INT32_MAX cannot
  // overflow the cast below.
  constexpr auto kInt32Max = static_cast<double>(std::numeric_limits<std::int32_t>::max());
  const double max_x = std::min(static_cast<double>(bounds.width), kInt32Max);
  const double max_y = std::min(static_cast<double>(bounds.height), kInt32Max);
  const double x0 = std::clamp(std::floor(static_cast<double>(rect.x)), 0.0, max_x);
  const double y0 = std::clamp(std::floor(static_cast<double>(rect.y)), 0.0, max_y);
  const double x1 =
      std::clamp(std::ceil(static_cast<double>(rect.x) + static_cast<double>(rect.width)), 0.0, max_x);
  const double y1 = std::clamp(
      std::ceil(static_cast<double>(rect.y) + static_cast<double>(rect.height)), 0.0, max_y);
  if (x1 <= x0 || y1 <= y0) {
    return {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), 0, 0};
  }
  return {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0),
          static_cast<std::int32_t>(x1 - x0), static_cast<std::int32_t>(y1 - y0)};
}

ReadbackMapping::ReadbackMapping(Size source, Size readback) : source_(source), readback_(readback) {
  if (has_zero_dimension(source) || has_zero_dimension(readback)) {
    throw ContractError("geometry: readback mapping needs non-zero source and readback sizes");
  }
}

ModelInputMapping::ModelInputMapping(Rect crop, Size input, Fit fit, double max_scale)
    : ModelInputMapping(crop, input, fit) {
  if (fit == Fit::Stretch) {
    throw ContractError("geometry: a scale cap needs a letterbox fit");
  }
  if (!std::isfinite(max_scale) || max_scale <= 0.0) {
    throw ContractError("geometry: model input scale cap must be positive and finite");
  }
  if (scale_x_ <= max_scale) {
    return;
  }
  scale_x_ = max_scale;
  scale_y_ = max_scale;
  if (fit == Fit::LetterboxCenter) {
    pad_x_ = (static_cast<double>(input.width) - static_cast<double>(crop.width) * max_scale) / 2.0;
    pad_y_ = (static_cast<double>(input.height) - static_cast<double>(crop.height) * max_scale) / 2.0;
  }
}

ModelInputMapping::ModelInputMapping(Rect crop, Size input, Fit fit)
    : crop_(crop), input_(input), fit_(fit) {
  if (!is_valid(crop) || crop.width <= 0.0F || crop.height <= 0.0F) {
    throw ContractError("geometry: model input crop must be finite with positive size");
  }
  if (has_zero_dimension(input)) {
    throw ContractError("geometry: model input size must be non-zero");
  }
  const double in_w = static_cast<double>(input.width);
  const double in_h = static_cast<double>(input.height);
  const double crop_w = static_cast<double>(crop.width);
  const double crop_h = static_cast<double>(crop.height);
  scale_x_ = in_w / crop_w;
  scale_y_ = in_h / crop_h;
  if (fit == Fit::Stretch) {
    return;
  }
  const double s = std::min(scale_x_, scale_y_);
  scale_x_ = s;
  scale_y_ = s;
  if (fit == Fit::LetterboxCenter) {
    pad_x_ = (in_w - crop_w * s) / 2.0;
    pad_y_ = (in_h - crop_h * s) / 2.0;
  }
}

Box convert(const Box& box, Space target, const SpaceMap& map) {
  if (!is_valid(box.rect)) {
    throw ContractError("geometry: cannot convert an invalid box");
  }
  if (box.space == target) {
    return box;
  }
  const SpaceAffine from = affine_from_source(box.space, map);
  const SpaceAffine to = affine_from_source(target, map);
  const auto map_axis = [](double v, const AxisMap& a, const AxisMap& b) {
    return (v - a.offset) / a.scale * b.scale + b.offset;
  };
  const double x = map_axis(static_cast<double>(box.rect.x), from.x, to.x);
  const double y = map_axis(static_cast<double>(box.rect.y), from.y, to.y);
  const double w = static_cast<double>(box.rect.width) / from.x.scale * to.x.scale;
  const double h = static_cast<double>(box.rect.height) / from.y.scale * to.y.scale;
  const auto [rx, rw] = narrow_outward(x, x + w);
  const auto [ry, rh] = narrow_outward(y, y + h);
  return {{rx, ry, rw, rh}, target};
}

}  // namespace spg
