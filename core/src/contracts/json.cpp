// JSON forms of the contracts (docs/contracts.md). Error messages are built
// only from field names, indices and byte offsets defined here. Library
// exception text is never forwarded because it can quote the input.
#include "spg/json.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "spg/error.hpp"

namespace spg {

namespace {

using Json = nlohmann::ordered_json;

constexpr std::int64_t kSchemaVersion = 1;
constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kUint32Max = std::numeric_limits<std::uint32_t>::max();
constexpr double kFloatMax = std::numeric_limits<float>::max();

constexpr std::string_view kInFloatRange = "must be within the float range";
constexpr std::string_view kNonNegativeFloat = "must not be negative and must be within the float range";

[[noreturn]] void fail(std::string_view doc, const std::string& message) {
  throw ContractError(std::string(doc) + ": " + message);
}

// Parses `text`, rejecting duplicate keys in any object.
Json parse_document(std::string_view text, std::string_view doc) {
  std::vector<std::unordered_set<std::string>> open_objects;
  bool duplicate = false;
  const Json::parser_callback_t track_keys = [&](int /*depth*/, Json::parse_event_t event,
                                                 Json& parsed) {
    switch (event) {
      case Json::parse_event_t::object_start:
        open_objects.emplace_back();
        break;
      case Json::parse_event_t::key:
        if (!open_objects.empty() && !open_objects.back().insert(parsed.get<std::string>()).second) {
          duplicate = true;
        }
        break;
      case Json::parse_event_t::object_end:
        if (!open_objects.empty()) {
          open_objects.pop_back();
        }
        break;
      default:
        break;
    }
    return true;
  };
  Json root;
  try {
    root = Json::parse(text.begin(), text.end(), track_keys);
  } catch (const Json::parse_error& e) {
    fail(doc, "invalid JSON at byte " + std::to_string(e.byte));
  } catch (const Json::exception&) {
    fail(doc, "invalid JSON");
  }
  if (duplicate) {
    fail(doc, "an object repeats a field name");
  }
  return root;
}

std::string serialize_document(const Json& root, std::string_view doc) {
  try {
    return root.dump();
  } catch (const Json::exception&) {
    fail(doc, "a string is not valid UTF-8");
  }
}

// Reads one JSON object, rejecting fields outside `allowed`.
class ObjectReader {
 public:
  ObjectReader(const Json& value, std::string_view doc, std::string path,
               std::initializer_list<std::string_view> allowed)
      : value_(value), doc_(doc), path_(std::move(path)) {
    if (!value_.is_object()) {
      fail(doc_, (path_.empty() ? std::string("document") : "'" + path_ + "'") +
                     " must be an object");
    }
    for (const auto& item : value_.items()) {
      bool known = false;
      for (const std::string_view name : allowed) {
        known = known || item.key() == name;
      }
      if (!known) {
        // The unknown name is not echoed: it is input and could be secret.
        std::string list;
        for (const std::string_view name : allowed) {
          list += (list.empty() ? "" : ", ") + std::string(name);
        }
        fail(doc_, "unknown field in " + (path_.empty() ? std::string("document") : "'" + path_ + "'") +
                       " (allowed: " + list + ")");
      }
    }
  }

  [[nodiscard]] std::string field(std::string_view key) const {
    return path_.empty() ? std::string(key) : path_ + "." + std::string(key);
  }
  [[noreturn]] void fail_field(std::string_view key, const std::string& message) const {
    fail(doc_, "'" + field(key) + "' " + message);
  }
  [[nodiscard]] const Json* find(std::string_view key) const {
    const auto it = value_.find(key);
    return it == value_.end() ? nullptr : &*it;
  }
  [[nodiscard]] const Json& require(std::string_view key) const {
    const Json* v = find(key);
    if (v == nullptr) {
      fail_field(key, "is required");
    }
    return *v;
  }

  [[nodiscard]] std::int64_t integer(std::string_view key, std::int64_t lo, std::int64_t hi) const {
    return integer_value(require(key), key, lo, hi);
  }
  [[nodiscard]] std::int64_t integer_or(std::string_view key, std::int64_t lo, std::int64_t hi,
                                        std::int64_t fallback) const {
    const Json* v = find(key);
    return v == nullptr ? fallback : integer_value(*v, key, lo, hi);
  }
  [[nodiscard]] double number(std::string_view key) const { return number_value(require(key), key); }
  [[nodiscard]] double number_or(std::string_view key, double fallback) const {
    const Json* v = find(key);
    return v == nullptr ? fallback : number_value(*v, key);
  }
  // Float fields are range-checked on the parsed double, before narrowing:
  // rounding could otherwise turn an invalid value into an allowed one
  // (1.00000001 -> 1, -1e-50 -> -0), and a double outside the float range
  // cannot be narrowed at all.
  [[nodiscard]] float float_number(std::string_view key, double lo, double hi,
                                   std::string_view rule) const {
    return narrow(number(key), key, lo, hi, rule);
  }
  [[nodiscard]] float float_number_or(std::string_view key, double lo, double hi,
                                      std::string_view rule, float fallback) const {
    const Json* v = find(key);
    return v == nullptr ? fallback : narrow(number_value(*v, key), key, lo, hi, rule);
  }
  [[nodiscard]] bool boolean(std::string_view key) const {
    const Json& v = require(key);
    if (!v.is_boolean()) {
      fail_field(key, "must be true or false");
    }
    return v.get<bool>();
  }
  [[nodiscard]] const std::string& string(std::string_view key) const {
    const Json& v = require(key);
    if (!v.is_string()) {
      fail_field(key, "must be a string");
    }
    return v.get_ref<const std::string&>();
  }
  [[nodiscard]] const Json& array(std::string_view key) const {
    const Json& v = require(key);
    if (!v.is_array()) {
      fail_field(key, "must be an array");
    }
    return v;
  }

  [[nodiscard]] std::string_view doc() const noexcept { return doc_; }

 private:
  [[nodiscard]] std::int64_t integer_value(const Json& v, std::string_view key, std::int64_t lo,
                                           std::int64_t hi) const {
    const std::string range = "must be an integer in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]";
    if (v.is_number_unsigned()) {
      const auto u = v.get<std::uint64_t>();
      if (u > static_cast<std::uint64_t>(hi)) {
        fail_field(key, range);
      }
      const auto s = static_cast<std::int64_t>(u);
      if (s < lo) {
        fail_field(key, range);
      }
      return s;
    }
    if (!v.is_number_integer()) {
      fail_field(key, range);
    }
    const auto s = v.get<std::int64_t>();
    if (s < lo || s > hi) {
      fail_field(key, range);
    }
    return s;
  }
  [[nodiscard]] double number_value(const Json& v, std::string_view key) const {
    if (!v.is_number()) {
      fail_field(key, "must be a number");
    }
    const auto d = v.get<double>();
    if (!std::isfinite(d)) {
      fail_field(key, "must be finite");
    }
    return d;
  }
  [[nodiscard]] float narrow(double d, std::string_view key, double lo, double hi,
                             std::string_view rule) const {
    if (d < lo || d > hi) {
      fail_field(key, std::string(rule));
    }
    return static_cast<float>(d);
  }

  const Json& value_;
  std::string_view doc_;
  std::string path_;
};

void require_schema(const ObjectReader& reader) {
  if (reader.integer("schema", 0, kInt64Max) != kSchemaVersion) {
    fail(reader.doc(), "unsupported schema version (expected " + std::to_string(kSchemaVersion) + ")");
  }
}

void require_source_space(const ObjectReader& reader) {
  if (reader.string("space") != to_string(Space::Source)) {
    reader.fail_field("space", "must be \"source\"");
  }
}

Rect read_rect(const Json& value, std::string_view doc, const std::string& path) {
  const ObjectReader r(value, doc, path, {"x", "y", "w", "h"});
  return {r.float_number("x", -kFloatMax, kFloatMax, kInFloatRange),
          r.float_number("y", -kFloatMax, kFloatMax, kInFloatRange),
          r.float_number("w", 0.0, kFloatMax, kNonNegativeFloat),
          r.float_number("h", 0.0, kFloatMax, kNonNegativeFloat)};
}

Json write_rect(const Rect& rect) {
  // Floats widen to double exactly, so parsing the output restores each value.
  return Json{{"x", rect.x}, {"y", rect.y}, {"w", rect.width}, {"h", rect.height}};
}

std::string item_path(std::string_view key, std::size_t index) {
  return std::string(key) + "[" + std::to_string(index) + "]";
}

}  // namespace

// Policy config ---------------------------------------------------------------

PolicyConfig parse_policy_config(std::string_view json) {
  constexpr std::string_view doc = "policy config";
  const Json root = parse_document(json, doc);
  const ObjectReader r(root, doc, "",
                       {"schema", "categories", "watchlist", "fuzzy", "lookahead_ms", "retro_ms",
                        "hold_ms", "margin_px", "margin_ratio", "fallback", "mask_style"});
  require_schema(r);

  PolicyConfig config;
  if (r.find("categories") != nullptr) {
    config.categories = CategorySet{};
    const Json& list = r.array("categories");
    for (std::size_t i = 0; i < list.size(); ++i) {
      const std::optional<Category> c =
          list[i].is_string() ? parse_category_name(list[i].get_ref<const std::string&>()) : std::nullopt;
      if (!c) {
        fail(doc, "'" + item_path("categories", i) + "' is not a known category");
      }
      config.categories.insert(*c);
    }
  }
  if (r.find("watchlist") != nullptr) {
    const Json& list = r.array("watchlist");
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (!list[i].is_string()) {
        fail(doc, "'" + item_path("watchlist", i) + "' must be a string");
      }
      config.watchlist.emplace_back(list[i].get<std::string>());
    }
  }
  if (const Json* fuzzy = r.find("fuzzy"); fuzzy != nullptr) {
    const ObjectReader f(*fuzzy, doc, "fuzzy", {"max_edits", "max_edit_ratio", "min_partial_chars"});
    config.fuzzy.max_edits = static_cast<std::uint32_t>(
        f.integer_or("max_edits", 0, kUint32Max, config.fuzzy.max_edits));
    config.fuzzy.max_edit_ratio = f.number_or("max_edit_ratio", config.fuzzy.max_edit_ratio);
    config.fuzzy.min_partial_chars = static_cast<std::uint32_t>(
        f.integer_or("min_partial_chars", 1, kUint32Max, config.fuzzy.min_partial_chars));
  }
  config.lookahead = std::chrono::milliseconds{
      r.integer_or("lookahead_ms", 0, kMaxLookahead.count(), config.lookahead.count())};
  // The retroactive window defaults to the whole lookahead (plan section 4).
  config.retro = std::chrono::milliseconds{
      r.integer_or("retro_ms", 0, kMaxLookahead.count(), config.lookahead.count())};
  config.hold =
      std::chrono::milliseconds{r.integer_or("hold_ms", 0, kMaxHold.count(), config.hold.count())};
  config.margin_px =
      r.float_number_or("margin_px", 0.0, kFloatMax, kNonNegativeFloat, config.margin_px);
  config.margin_ratio =
      r.float_number_or("margin_ratio", 0.0, kFloatMax, kNonNegativeFloat, config.margin_ratio);
  if (r.find("fallback") != nullptr) {
    const std::optional<FallbackMode> mode = parse_fallback_mode(r.string("fallback"));
    if (!mode) {
      r.fail_field("fallback", "must be \"blur_source\", \"hold_last_scanned\" or \"solid_fill\"");
    }
    config.fallback = *mode;
  }
  if (r.find("mask_style") != nullptr) {
    const std::optional<MaskStyle> style = parse_mask_style(r.string("mask_style"));
    if (!style) {
      r.fail_field("mask_style", "must be \"blur\", \"pixelate\" or \"solid_fill\"");
    }
    config.mask_style = *style;
  }
  validate(config);  // its messages carry the same document prefix
  return config;
}

std::string serialize_policy_config(const PolicyConfig& config) {
  validate(config);
  Json categories = Json::array();
  for (const Category c : kAllCategories) {
    if (config.categories.contains(c)) {
      categories.push_back(category_name(c));
    }
  }
  Json watchlist = Json::array();
  for (const Secret& entry : config.watchlist) {
    watchlist.push_back(entry.reveal());
  }
  Json root;
  root["schema"] = kSchemaVersion;
  root["categories"] = std::move(categories);
  root["watchlist"] = std::move(watchlist);
  root["fuzzy"] = Json{{"max_edits", config.fuzzy.max_edits},
                       {"max_edit_ratio", config.fuzzy.max_edit_ratio},
                       {"min_partial_chars", config.fuzzy.min_partial_chars}};
  root["lookahead_ms"] = config.lookahead.count();
  root["retro_ms"] = config.retro.count();
  root["hold_ms"] = config.hold.count();
  root["margin_px"] = config.margin_px;
  root["margin_ratio"] = config.margin_ratio;
  root["fallback"] = to_string(config.fallback);
  root["mask_style"] = to_string(config.mask_style);
  return serialize_document(root, "policy config");
}

// Ground truth ----------------------------------------------------------------

GroundTruthFrame parse_ground_truth_frame(std::string_view json) {
  constexpr std::string_view doc = "ground truth";
  const Json root = parse_document(json, doc);
  const ObjectReader r(root, doc, "",
                       {"schema", "space", "frame", "timestamp_ns", "width", "height", "instances"});
  require_schema(r);
  require_source_space(r);

  GroundTruthFrame frame;
  frame.frame_index = static_cast<std::uint64_t>(r.integer("frame", 0, kInt64Max));
  frame.timestamp = timestamp_from_ns(r.integer("timestamp_ns", kInt64Min, kInt64Max));
  frame.source_size = {static_cast<std::uint32_t>(r.integer("width", 1, kUint32Max)),
                       static_cast<std::uint32_t>(r.integer("height", 1, kUint32Max))};
  const Json& list = r.array("instances");
  for (std::size_t i = 0; i < list.size(); ++i) {
    const std::string path = item_path("instances", i);
    const ObjectReader item(list[i], doc, path, {"id", "kind", "box", "visibility"});
    GroundTruthInstance inst;
    inst.id = item.string("id");
    const std::string& kind = item.string("kind");
    if (kind != "decoy") {
      inst.category = parse_category_name(kind);
      if (!inst.category) {
        item.fail_field("kind", "must be \"decoy\" or a category name");
      }
    }
    inst.box = {read_rect(item.require("box"), doc, path + ".box"), Space::Source};
    inst.visibility = item.float_number("visibility", 0.0, 1.0, "must be in [0, 1]");
    frame.instances.push_back(std::move(inst));
  }
  validate(frame);  // its messages carry the same document prefix
  return frame;
}

std::string serialize_ground_truth_frame(const GroundTruthFrame& frame) {
  validate(frame);
  Json instances = Json::array();
  for (const GroundTruthInstance& inst : frame.instances) {
    instances.push_back(Json{
        {"id", inst.id},
        {"kind", inst.category ? category_name(*inst.category) : std::string_view("decoy")},
        {"box", write_rect(inst.box.rect)},
        {"visibility", inst.visibility}});
  }
  Json root;
  root["schema"] = kSchemaVersion;
  root["space"] = to_string(Space::Source);
  root["frame"] = frame.frame_index;
  root["timestamp_ns"] = to_ns(frame.timestamp);
  root["width"] = frame.source_size.width;
  root["height"] = frame.source_size.height;
  root["instances"] = std::move(instances);
  return serialize_document(root, "ground truth");
}

// Mask plan -------------------------------------------------------------------

MaskPlan parse_mask_plan(std::string_view json) {
  constexpr std::string_view doc = "mask plan";
  const Json root = parse_document(json, doc);
  const ObjectReader r(root, doc, "", {"schema", "space", "timestamp_ns", "fallback", "rects"});
  require_schema(r);
  require_source_space(r);

  MaskPlan plan;
  plan.frame_time = timestamp_from_ns(r.integer("timestamp_ns", kInt64Min, kInt64Max));
  plan.fallback = r.boolean("fallback");
  const Json& list = r.array("rects");
  for (std::size_t i = 0; i < list.size(); ++i) {
    const std::string path = item_path("rects", i);
    const ObjectReader item(list[i], doc, path, {"box", "style"});
    MaskRect rect;
    rect.box = {read_rect(item.require("box"), doc, path + ".box"), Space::Source};
    const std::optional<MaskStyle> style = parse_mask_style(item.string("style"));
    if (!style) {
      item.fail_field("style", "must be \"blur\", \"pixelate\" or \"solid_fill\"");
    }
    rect.style = *style;
    plan.rects.push_back(rect);
  }
  validate(plan);  // its messages carry the same document prefix
  return plan;
}

std::string serialize_mask_plan(const MaskPlan& plan) {
  validate(plan);
  Json rects = Json::array();
  for (const MaskRect& rect : plan.rects) {
    rects.push_back(Json{{"box", write_rect(rect.box.rect)}, {"style", to_string(rect.style)}});
  }
  Json root;
  root["schema"] = kSchemaVersion;
  root["space"] = to_string(Space::Source);
  root["timestamp_ns"] = to_ns(plan.frame_time);
  root["fallback"] = plan.fallback;
  root["rects"] = std::move(rects);
  return serialize_document(root, "mask plan");
}

}  // namespace spg
