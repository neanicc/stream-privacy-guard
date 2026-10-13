#include "inputs.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "image_io.hpp"
#include "spg/error.hpp"
#include "spg/json.hpp"

namespace spg::cli {

namespace {

std::string read_text(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    throw CliError("cannot open " + file.string());
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    throw CliError("cannot read " + file.string());
  }
  return text;
}

std::uint32_t positive_u32(const nlohmann::json& value, const std::string& where) {
  if (!value.is_number_integer() || value.get<std::int64_t>() < 1 ||
      value.get<std::int64_t>() > std::numeric_limits<std::int32_t>::max()) {
    throw CliError(where + " must be a positive integer");
  }
  return static_cast<std::uint32_t>(value.get<std::int64_t>());
}

}  // namespace

std::vector<std::filesystem::path> list_frames(const std::filesystem::path& dir) {
  std::error_code ec;
  std::vector<std::filesystem::path> frames;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".png") {
      frames.push_back(it->path());
    }
  }
  if (ec) {
    throw CliError("cannot list " + dir.string() + ": " + ec.message());
  }
  if (frames.empty()) {
    throw CliError("no .png frames in " + dir.string());
  }
  std::sort(frames.begin(), frames.end(),
            [](const auto& a, const auto& b) { return a.filename().string() < b.filename().string(); });
  return frames;
}

PolicyConfig read_policy(const std::filesystem::path& file) {
  try {
    return parse_policy_config(read_text(file));
  } catch (const ContractError& e) {
    throw CliError(file.string() + ": " + e.what());
  }
}

Suite read_suite(const std::filesystem::path& dir) {
  const std::filesystem::path index_file = dir / "suite.json";
  nlohmann::json index;
  try {
    index = nlohmann::json::parse(read_text(index_file));
  } catch (const nlohmann::json::exception&) {
    throw CliError(index_file.string() + " is not valid JSON");
  }
  if (!index.is_object() || !index.contains("schema") || index["schema"] != 1 || !index.contains("sequences") ||
      !index["sequences"].is_array()) {
    throw CliError(index_file.string() + ": expected schema 1 with a sequence list");
  }
  Suite suite;
  suite.dir = dir;
  if (index.contains("suite") && index["suite"].is_string()) {
    suite.name = index["suite"].get<std::string>();
  }
  suite.fps = index.contains("fps") ? positive_u32(index["fps"], index_file.string() + ": fps") : 60;
  const std::string policy = index.contains("policy") && index["policy"].is_string()
                                 ? index["policy"].get<std::string>()
                                 : std::string("policy.json");
  suite.policy = read_policy(dir / policy);
  for (std::size_t i = 0; i < index["sequences"].size(); ++i) {
    const nlohmann::json& entry = index["sequences"][i];
    const std::string where = index_file.string() + ": sequences[" + std::to_string(i) + "]";
    if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() || !entry.contains("frames")) {
      throw CliError(where + " needs a name and a frame count");
    }
    FrameSequence seq;
    seq.name = entry["name"].get<std::string>();
    if (seq.name.empty() || seq.name.find_first_of("/\\") != std::string::npos || seq.name == "." ||
        seq.name == "..") {
      throw CliError(where + ": the name must be a plain directory name");
    }
    seq.fps = suite.fps;
    const std::uint32_t declared = positive_u32(entry["frames"], where + ".frames");
    if (entry.contains("width") && entry.contains("height")) {
      seq.size = Size{positive_u32(entry["width"], where + ".width"), positive_u32(entry["height"], where + ".height")};
    }
    seq.frames = list_frames(dir / seq.name / "frames");
    if (seq.frames.size() != declared) {
      throw CliError(where + ": " + std::to_string(seq.frames.size()) + " frame file(s), suite.json declares " +
                     std::to_string(declared));
    }
    suite.sequences.push_back(std::move(seq));
  }
  if (suite.sequences.empty()) {
    throw CliError(index_file.string() + " lists no sequences");
  }
  return suite;
}

Timestamp frame_time(std::size_t index, std::uint32_t fps) {
  return timestamp_from_ns(static_cast<std::int64_t>(index) * 1'000'000'000LL / static_cast<std::int64_t>(fps));
}

}  // namespace spg::cli
