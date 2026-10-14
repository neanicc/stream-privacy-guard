#include "commands.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "driver.hpp"
#include "image_io.hpp"
#include "inputs.hpp"
#include "scrub.hpp"
#include "spg/category.hpp"
#include "spg/error.hpp"
#include "spg/json.hpp"
#include "spg/version.hpp"
#include "timing.hpp"

namespace spg::cli {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

class UsageError : public std::runtime_error {
 public:
  explicit UsageError(const std::string& what) : std::runtime_error(what) {}
};

constexpr std::string_view kUsage =
    "usage: spg-cli <command> [options]\n"
    "\n"
    "commands:\n"
    "  run     run the pipeline on one PNG sequence and write its mask plans\n"
    "            --frames DIR --policy FILE --out FILE [--fps N] [--timing FILE]\n"
    "            [--write-frames DIR]  debug: write every composed frame (off by default)\n"
    "  eval    run the pipeline on every sequence of a generated eval suite\n"
    "            --suite DIR --plans DIR [--timing FILE]\n"
    "            writes DIR/<sequence>/mask_plans.jsonl; eval/score.py scores them\n"
    "  bench   measure stage latency, detection latency and the fail-closed rate\n"
    "            (--frames DIR --policy FILE [--fps N] | --suite DIR [--resolution WxH])\n"
    "            [--realtime] [--out FILE | --plans DIR] [--report FILE]\n"
    "            simulated feed (default): cycles take their measured time, the\n"
    "            CPU compositor runs on every frame; --realtime: worker threads\n"
    "            on the wall clock, fed at fps\n"
    "  scrub   report the frames of a recorded session in which a planted string\n"
    "          is still readable, and exit 3 when there is any\n"
    "            --frames DIR --planted FILE [--fps N] [--report FILE]\n"
    "            [--sensitivity normal|high]  fuzzy matching, as the plugin's\n"
    "                                         setting (default high)\n"
    "            FILE holds one planted string per line; output names them by\n"
    "            number (#1 is the first) and never prints them\n"
    "\n"
    "pipeline options:\n"
    "  --cache DIR            models cache (default: $SPG_CACHE_DIR or ~/.cache/spg)\n"
    "  --cpu                  run the models on CPU only\n"
    "  --readback-scale S     scan a copy scaled by S in (0, 1] (default 1)\n"
    "  --workers N            detection workers (default 1, as the plugin runs); scrub:\n"
    "                         frames scanned at once (default 2)\n"
    "  --cycle-ms MS          run/eval: simulated time per detection cycle (default 200)\n"
    "  --scan-interval-ms MS  frame time between scans a worker may take (default: the\n"
    "                         scheduler's, as the plugin runs)\n"
    "  --threads N            ONNX Runtime threads per model (default: its own choice)\n"
    "  --recognizer-runs N    recognizer runs side by side per cycle, in [1, 16] (default 4)\n"
    "  --no-recognition-cache read every text region on every cycle (default: reuse the\n"
    "                         text of a region whose crop is pixel-identical; to measure it)\n"
    "\n"
    "  --version  print the version and exit\n"
    "  --help     print this help and exit\n";

// --name value pairs and --flag switches; anything else is a usage error.
class Options {
 public:
  Options(std::span<const std::string_view> args, const std::set<std::string_view>& valued,
          const std::set<std::string_view>& flags) {
    for (std::size_t i = 0; i < args.size(); ++i) {
      const std::string_view a = args[i];
      if (flags.contains(a)) {
        flags_.insert(std::string(a));
      } else if (valued.contains(a)) {
        if (i + 1 >= args.size()) {
          throw UsageError(std::string(a) + " needs a value");
        }
        if (!values_.emplace(std::string(a), std::string(args[i + 1])).second) {
          throw UsageError(std::string(a) + " given twice");
        }
        ++i;
      } else {
        throw UsageError("unknown option '" + std::string(a) + "'");
      }
    }
  }
  [[nodiscard]] bool flag(std::string_view name) const { return flags_.contains(std::string(name)); }
  [[nodiscard]] std::optional<std::string> get(std::string_view name) const {
    const auto it = values_.find(std::string(name));
    return it == values_.end() ? std::nullopt : std::optional<std::string>(it->second);
  }
  [[nodiscard]] std::string need(std::string_view name) const {
    std::optional<std::string> v = get(name);
    if (!v) {
      throw UsageError(std::string(name) + " is required");
    }
    return *v;
  }

 private:
  std::map<std::string, std::string> values_;
  std::set<std::string> flags_;
};

double parse_number(const std::string& text, std::string_view name) {
  try {
    std::size_t used = 0;
    const double v = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(v)) {
      throw UsageError(std::string(name) + " must be a number");
    }
    return v;
  } catch (const std::logic_error&) {
    throw UsageError(std::string(name) + " must be a number");
  }
}

std::uint32_t parse_count(const std::string& text, std::string_view name) {
  std::uint32_t v = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc{} || end != text.data() + text.size() || v == 0) {
    throw UsageError(std::string(name) + " must be a positive integer");
  }
  return v;
}

Size parse_resolution(const std::string& text) {
  const std::size_t x = text.find('x');
  if (x == std::string::npos) {
    throw UsageError("--resolution must look like 1920x1080");
  }
  return Size{parse_count(text.substr(0, x), "--resolution width"), parse_count(text.substr(x + 1), "--resolution height")};
}

const std::set<std::string_view> kPipelineValued = {"--cache",    "--readback-scale",   "--workers",
                                                    "--cycle-ms", "--scan-interval-ms", "--threads",
                                                    "--recognizer-runs"};

struct PipelineSetup {
  DriverOptions driver;
  StageOptions stages;
  fs::path cache;
};

PipelineSetup pipeline_setup(const Options& o, bool simulated_fixed) {
  PipelineSetup s;
  s.cache = o.get("--cache") ? fs::path(*o.get("--cache")) : default_cache_dir();
  s.stages.cpu_only = o.flag("--cpu");
  s.stages.recognition_cache = !o.flag("--no-recognition-cache");
  if (const auto v = o.get("--readback-scale")) {
    s.driver.readback_scale = parse_number(*v, "--readback-scale");
    if (!(s.driver.readback_scale > 0.0 && s.driver.readback_scale <= 1.0)) {
      throw UsageError("--readback-scale must be in (0, 1]");
    }
  }
  if (const auto v = o.get("--workers")) {
    s.driver.workers = parse_count(*v, "--workers");
  }
  if (const auto v = o.get("--cycle-ms")) {
    if (!simulated_fixed) {
      throw UsageError("--cycle-ms applies to run and eval only; bench measures each cycle");
    }
    const double ms = parse_number(*v, "--cycle-ms");
    if (ms < 0.0 || ms > 60000.0) {
      throw UsageError("--cycle-ms must be in [0, 60000]");
    }
    s.driver.cycle = std::chrono::duration_cast<Duration>(std::chrono::duration<double, std::milli>(ms));
  }
  if (!simulated_fixed) {
    s.driver.cycle.reset();
  }
  if (const auto v = o.get("--scan-interval-ms")) {
    const double ms = parse_number(*v, "--scan-interval-ms");
    if (ms < 0.0 || ms > 60000.0) {
      throw UsageError("--scan-interval-ms must be in [0, 60000]");
    }
    s.driver.scan_interval =
        std::chrono::duration_cast<Duration>(std::chrono::duration<double, std::milli>(ms));
  }
  if (const auto v = o.get("--threads")) {
    s.stages.intra_op_threads = static_cast<int>(std::min<std::uint32_t>(parse_count(*v, "--threads"), 256));
  }
  if (const auto v = o.get("--recognizer-runs")) {
    s.stages.recognizer_runs = parse_count(*v, "--recognizer-runs");
    if (s.stages.recognizer_runs > 16) {
      throw UsageError("--recognizer-runs must be in [1, 16]");
    }
  }
  return s;
}

OpenedStages open_for(const CliHooks& hooks, const PolicyConfig& policy, const PipelineSetup& setup) {
  if (hooks.open) {
    return hooks.open(policy, setup.stages, setup.cache);
  }
  return open_stages(model_paths(setup.cache), policy, setup.stages);
}

FrameSource png_source(const FrameSequence& seq, StageTimes& times) {
  FrameSource source;
  source.count = seq.frames.size();
  source.fps = seq.fps;
  source.load = [frames = seq.frames, expected = seq.size, &times](std::size_t i) {
    const auto start = std::chrono::steady_clock::now();
    BgraImage image = read_png(frames.at(i));
    times.add(kStageDecode, elapsed_ms(start, std::chrono::steady_clock::now()));
    if (expected.width != 0 && image.size() != expected) {
      throw CliError(frames.at(i).string() + " is " + std::to_string(image.width) + "x" +
                     std::to_string(image.height) + ", the suite declares " + std::to_string(expected.width) + "x" +
                     std::to_string(expected.height));
    }
    return image;
  };
  return source;
}

void write_plans(const fs::path& path, const std::vector<MaskPlan>& plans) {
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  for (const MaskPlan& plan : plans) {
    out << serialize_mask_plan(plan) << '\n';
  }
  if (!out) {
    throw CliError("cannot write " + path.string());
  }
}

void write_json(const fs::path& path, const json& doc) {
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << doc.dump(2) << '\n';
  if (!out) {
    throw CliError("cannot write " + path.string());
  }
}

json summary_json(const StageSummary& s) {
  return json{{"count", s.count}, {"total_ms", s.total_ms}, {"mean_ms", s.mean_ms},
              {"p50_ms", s.p50_ms}, {"p95_ms", s.p95_ms}, {"max_ms", s.max_ms}};
}

// Totals over the sequences of one command.
struct Totals {
  std::size_t frames = 0;
  std::size_t fail_closed = 0;
  std::size_t padding = 0;
  std::size_t late = 0;
  std::vector<double> latencies_ms;
  json sequences = json::array();
  json scheduler = json::object();

  void add(const std::string& name, const SequenceResult& r) {
    frames += r.plans.size();
    fail_closed += r.fail_closed;
    padding += r.padding_frames;
    late += r.late_frames;
    latencies_ms.insert(latencies_ms.end(), r.latencies_ms.begin(), r.latencies_ms.end());
    sequences.push_back(json{{"name", name},
                             {"frames", r.plans.size()},
                             {"fail_closed_frames", r.fail_closed},
                             {"padding_frames", r.padding_frames},
                             {"late_frames", r.late_frames}});
    const auto bump = [this](const char* key, std::uint64_t v) {
      scheduler[key] = scheduler.value(key, std::uint64_t{0}) + v;
    };
    bump("frames_pushed", r.stats.frames_pushed);
    bump("frames_released", r.stats.frames_released);
    bump("frames_failed_closed", r.stats.frames_failed_closed);
    bump("early_releases", r.stats.early_releases);
    bump("bad_timestamps", r.stats.bad_timestamps);
    bump("scans_queued", r.stats.scans_queued);
    bump("scans_dropped", r.stats.scans_dropped);
    bump("scans_started", r.stats.scans_started);
    bump("scans_completed", r.stats.scans_completed);
    bump("scans_failed", r.stats.scans_failed);
  }
};

json report_json(std::string_view command, std::string_view mode, std::uint32_t fps, const PipelineSetup& setup,
                 const OpenedStages& opened, const Totals& totals, const StageTimes& times) {
  json stages = json::object();
  for (const auto& [name, s] : times.summary()) {
    stages[name] = summary_json(s);
  }
  const double rate =
      totals.frames == 0 ? 0.0 : static_cast<double>(totals.fail_closed) / static_cast<double>(totals.frames);
  json doc{{"schema", 1},
           {"command", command},
           {"mode", mode},
           {"version", version()},
           {"fps", fps},
           {"workers", setup.driver.workers},
           {"readback_scale", setup.driver.readback_scale},
           {"scan_interval_ms", to_ms(driver_config(PolicyConfig{}, setup.driver).scan_interval)},
           {"threads", setup.stages.intra_op_threads},
           {"recognizer_runs", setup.stages.recognizer_runs},
           {"providers", opened.providers},
           {"frames", totals.frames},
           {"padding_frames", totals.padding},
           {"late_frames", totals.late},
           {"fail_closed_frames", totals.fail_closed},
           {"fail_closed_frame_rate", rate},
           {"detection_latency_ms", summary_json(summarize(totals.latencies_ms))},
           {"latencies_ms", totals.latencies_ms},
           {"stages", stages},
           {"scheduler", totals.scheduler},
           {"sequences", totals.sequences}};
  doc["cycle_ms"] = setup.driver.cycle ? json(to_ms(*setup.driver.cycle)) : json(nullptr);
  if (opened.recognition_counts) {
    const RecognitionCounts counts = opened.recognition_counts();
    doc["recognition_cache"] = json{{"hits", counts.hits}, {"misses", counts.misses}};
  } else {
    doc["recognition_cache"] = json(nullptr);
  }
  return doc;
}

void print_stages(std::ostream& out, const StageTimes& times) {
  out << std::fixed << std::setprecision(1);
  out << std::left << std::setw(14) << "stage" << std::right << std::setw(8) << "calls" << std::setw(10) << "mean ms"
      << std::setw(10) << "p50 ms" << std::setw(10) << "p95 ms" << std::setw(10) << "max ms" << '\n';
  for (const auto& [name, s] : times.summary()) {
    out << std::left << std::setw(14) << name << std::right << std::setw(8) << s.count << std::setw(10) << s.mean_ms
        << std::setw(10) << s.p50_ms << std::setw(10) << s.p95_ms << std::setw(10) << s.max_ms << '\n';
  }
  out.unsetf(std::ios::floatfield);
}

void print_summary(std::ostream& out, std::string_view command, std::string_view mode, const OpenedStages& opened,
                   const Totals& totals, const StageTimes& times) {
  out << "spg-cli " << command << " (" << mode << "): " << totals.frames << " frames, " << totals.fail_closed
      << " fail-closed, " << totals.padding << " padding";
  if (totals.late > 0) {
    out << ", " << totals.late << " fed late";
  }
  out << '\n';
  if (!opened.providers.empty()) {
    out << "providers: " << opened.providers << '\n';
  }
  if (opened.recognition_counts) {
    const RecognitionCounts counts = opened.recognition_counts();
    out << "recognition cache: " << counts.hits << " regions reused, " << counts.misses << " read\n";
  }
  const StageSummary lat = summarize(totals.latencies_ms);
  out << std::fixed << std::setprecision(1);
  out << "detection latency: " << lat.count << " cycles, p50 " << lat.p50_ms << " ms, p95 " << lat.p95_ms
      << " ms, max " << lat.max_ms << " ms\n";
  out.unsetf(std::ios::floatfield);
  print_stages(out, times);
}

int cmd_run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err, const CliHooks& hooks) {
  std::set<std::string_view> valued = kPipelineValued;
  valued.insert({"--frames", "--policy", "--out", "--fps", "--timing", "--write-frames"});
  const Options o(args, valued, {"--cpu", "--no-recognition-cache"});
  PipelineSetup setup = pipeline_setup(o, true);
  FrameSequence seq;
  seq.name = "run";
  seq.frames = list_frames(o.need("--frames"));
  seq.fps = o.get("--fps") ? parse_count(*o.get("--fps"), "--fps") : 60;
  const fs::path out_file = o.need("--out");
  const PolicyConfig policy = read_policy(o.need("--policy"));
  if (const auto dir = o.get("--write-frames")) {
    // Debug only, never on by default: composed frames hold whatever the
    // source showed outside the masks.
    err << "spg-cli: debug: writing composed frames to " << *dir << '\n';
    fs::create_directories(*dir);
    setup.driver.compose = true;
    setup.driver.on_composed = [target = fs::path(*dir)](std::size_t index, const BgraImage& image) {
      std::ostringstream name;
      name << std::setw(6) << std::setfill('0') << index << ".png";
      write_png(target / name.str(), image);
    };
  }
  const OpenedStages opened = open_for(hooks, policy, setup);
  auto times = std::make_shared<StageTimes>();
  const ScanStages stages = timed(opened.stages, times);
  const SequenceResult result = run_simulated(png_source(seq, *times), policy, stages, setup.driver, *times);
  write_plans(out_file, result.plans);
  Totals totals;
  totals.add(seq.name, result);
  print_summary(out, "run", "simulated", opened, totals, *times);
  if (const auto timing = o.get("--timing")) {
    write_json(*timing, report_json("run", "simulated", seq.fps, setup, opened, totals, *times));
  }
  return kExitOk;
}

int cmd_suite(std::span<const std::string_view> args, std::ostream& out, const CliHooks& hooks) {
  std::set<std::string_view> valued = kPipelineValued;
  valued.insert({"--suite", "--plans", "--timing"});
  const Options o(args, valued, {"--cpu", "--no-recognition-cache"});
  const PipelineSetup setup = pipeline_setup(o, true);
  const Suite suite = read_suite(o.need("--suite"));
  const fs::path plans = o.need("--plans");
  const OpenedStages opened = open_for(hooks, suite.policy, setup);
  auto times = std::make_shared<StageTimes>();
  const ScanStages stages = timed(opened.stages, times);
  Totals totals;
  for (const FrameSequence& seq : suite.sequences) {
    const SequenceResult result = run_simulated(png_source(seq, *times), suite.policy, stages, setup.driver, *times);
    write_plans(plans / seq.name / "mask_plans.jsonl", result.plans);
    totals.add(seq.name, result);
    out << seq.name << ": " << result.plans.size() << " frames, " << result.fail_closed << " fail-closed\n";
  }
  print_summary(out, "eval", "simulated", opened, totals, *times);
  if (const auto timing = o.get("--timing")) {
    write_json(*timing, report_json("eval", "simulated", suite.fps, setup, opened, totals, *times));
  }
  return kExitOk;
}

int cmd_bench(std::span<const std::string_view> args, std::ostream& out, const CliHooks& hooks) {
  std::set<std::string_view> valued = kPipelineValued;
  valued.insert({"--frames", "--policy", "--fps", "--suite", "--resolution", "--out", "--plans", "--report"});
  const Options o(args, valued, {"--cpu", "--realtime", "--no-recognition-cache"});
  const bool realtime = o.flag("--realtime");
  PipelineSetup setup = pipeline_setup(o, false);
  setup.driver.compose = !realtime;

  std::vector<FrameSequence> sequences;
  PolicyConfig policy;
  std::uint32_t fps = 60;
  if (o.get("--suite")) {
    if (o.get("--frames") || o.get("--policy") || o.get("--fps") || o.get("--out")) {
      throw UsageError("--suite takes the frames, policy and fps from suite.json (use --plans for output)");
    }
    Suite suite = read_suite(*o.get("--suite"));
    policy = suite.policy;
    fps = suite.fps;
    const std::optional<Size> only = o.get("--resolution") ? std::optional<Size>(parse_resolution(*o.get("--resolution")))
                                                           : std::nullopt;
    for (FrameSequence& seq : suite.sequences) {
      if (!only || seq.size == *only) {
        sequences.push_back(std::move(seq));
      }
    }
    if (sequences.empty()) {
      throw CliError("no sequence of the suite has the requested resolution");
    }
  } else {
    if (o.get("--plans") || o.get("--resolution")) {
      throw UsageError("--plans and --resolution need --suite");
    }
    FrameSequence seq;
    seq.name = "bench";
    seq.frames = list_frames(o.need("--frames"));
    seq.fps = o.get("--fps") ? parse_count(*o.get("--fps"), "--fps") : 60;
    fps = seq.fps;
    policy = read_policy(o.need("--policy"));
    sequences.push_back(std::move(seq));
  }

  const OpenedStages opened = open_for(hooks, policy, setup);
  auto times = std::make_shared<StageTimes>();
  Totals totals;
  for (const FrameSequence& seq : sequences) {
    SequenceResult result;
    if (realtime) {
      auto cycles = std::make_shared<CycleLog>();
      result = run_realtime(png_source(seq, *times), policy, timed(opened.stages, times, cycles), setup.driver,
                            *times, *cycles);
    } else {
      result = run_simulated(png_source(seq, *times), policy, timed(opened.stages, times), setup.driver, *times);
    }
    if (const auto file = o.get("--out")) {
      write_plans(*file, result.plans);
    }
    if (const auto dir = o.get("--plans")) {
      write_plans(fs::path(*dir) / seq.name / "mask_plans.jsonl", result.plans);
    }
    totals.add(seq.name, result);
  }
  const std::string_view mode = realtime ? "realtime" : "simulated";
  print_summary(out, "bench", mode, opened, totals, *times);
  if (const auto report = o.get("--report")) {
    write_json(*report, report_json("bench", mode, fps, setup, opened, totals, *times));
  }
  return kExitOk;
}

// Seconds from the first frame, as "12.345".
std::string seconds_text(std::size_t frame, std::uint32_t fps) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(3) << static_cast<double>(frame) / static_cast<double>(fps);
  return text.str();
}

std::string planted_list(const std::vector<std::size_t>& planted) {
  std::string text;
  for (const std::size_t index : planted) {
    text += (text.empty() ? "#" : ", #") + std::to_string(index);
  }
  return text;
}

json box_json(const Box& box) {
  return json{{"x", box.rect.x}, {"y", box.rect.y}, {"width", box.rect.width}, {"height", box.rect.height}};
}

// Planted strings, recognized text and frames stay in memory: the output and
// the report hold frame indices and file names, times, boxes and the 1-based
// numbers of the planted strings.
int cmd_scrub(std::span<const std::string_view> args, std::ostream& out, std::ostream& err, const CliHooks& hooks) {
  const Options o(args, {"--frames", "--planted", "--fps", "--report", "--sensitivity", "--cache", "--workers"},
                  {"--cpu"});
  const fs::path frames_dir = o.need("--frames");
  const fs::path planted_file = o.need("--planted");
  FrameSequence seq;
  seq.name = "scrub";
  seq.fps = o.get("--fps") ? parse_count(*o.get("--fps"), "--fps") : 60;
  ScrubSensitivity sensitivity = ScrubSensitivity::High;
  if (const auto v = o.get("--sensitivity")) {
    if (*v == "normal") {
      sensitivity = ScrubSensitivity::Normal;
    } else if (*v != "high") {
      throw UsageError("--sensitivity must be normal or high");
    }
  }
  PipelineSetup setup;
  setup.cache = o.get("--cache") ? fs::path(*o.get("--cache")) : default_cache_dir();
  setup.stages.cpu_only = o.flag("--cpu");
  setup.stages.thorough = true;
  // Offline: frames scanned at once, not the realtime worker count.
  setup.driver.workers = ScrubOptions{}.workers;
  if (const auto v = o.get("--workers")) {
    setup.driver.workers = parse_count(*v, "--workers");
  }

  seq.frames = list_frames(frames_dir);
  const std::vector<Secret> planted = read_planted(planted_file);
  const FuzzyLimits fuzzy = scrub_fuzzy(sensitivity);
  const PlantedMatcher matcher(planted, fuzzy);
  PolicyConfig policy;
  policy.categories = CategorySet{};
  policy.categories.insert(Category::Watchlist);
  policy.watchlist = planted;
  policy.fuzzy = fuzzy;
  const OpenedStages opened = open_for(hooks, policy, setup);
  auto times = std::make_shared<StageTimes>();

  ScrubOptions options;
  options.workers = setup.driver.workers;
  const std::size_t total = seq.frames.size();
  options.progress = [&err, total](std::size_t done) {
    if (done % 100 == 0 || done == total) {
      err << "spg-cli scrub: " << done << "/" << total << " frames\n";
    }
  };
  const ScrubResult result = scrub(png_source(seq, *times), timed(opened.stages, times), matcher, options, *times);

  const std::string_view level = sensitivity == ScrubSensitivity::High ? "high" : "normal";
  json leaks = json::array();
  for (const ScrubLeak& leak : result.leaks) {
    const std::string file = seq.frames.at(leak.frame).filename().string();
    const Rect& r = leak.box.rect;
    out << "leak: frame " << leak.frame << " (" << file << ", " << seconds_text(leak.frame, seq.fps)
        << " s): planted " << planted_list(leak.planted) << " at x " << std::lround(r.x) << " y " << std::lround(r.y)
        << ", " << std::lround(r.width) << "x" << std::lround(r.height) << '\n';
    leaks.push_back(json{{"frame", leak.frame},
                         {"file", file},
                         {"time_ms", static_cast<double>(leak.frame) * 1000.0 / static_cast<double>(seq.fps)},
                         {"box", box_json(leak.box)},
                         {"score", leak.score},
                         {"planted", leak.planted}});
  }
  const std::vector<std::size_t> leak_frames = result.leak_frames();
  out << "spg-cli scrub: " << result.frames << " frames, " << result.regions << " text regions read, "
      << planted.size() << " planted strings, sensitivity " << level << '\n';
  if (!opened.providers.empty()) {
    out << "providers: " << opened.providers << '\n';
  }
  json per_planted = json::array();
  for (std::size_t index = 1; index <= planted.size(); ++index) {
    const std::vector<std::size_t> frames = result.leak_frames_of(index);
    out << "planted #" << index << ": ";
    if (frames.empty()) {
      out << "not readable in any frame\n";
    } else {
      out << "readable in " << frames.size() << (frames.size() == 1 ? " frame" : " frames") << ", first " << frames.front() << " ("
          << seconds_text(frames.front(), seq.fps) << " s), last " << frames.back() << " ("
          << seconds_text(frames.back(), seq.fps) << " s)\n";
    }
    per_planted.push_back(json{{"planted", index},
                               {"leak_frames", frames.size()},
                               {"first_frame", frames.empty() ? json(nullptr) : json(frames.front())},
                               {"last_frame", frames.empty() ? json(nullptr) : json(frames.back())}});
  }
  print_stages(out, *times);
  // A scrub that read no text at all checked nothing (a wrong folder, frames
  // at the wrong scale, a detector that sees nothing): never a pass.
  const bool nothing_read = result.regions == 0;
  if (nothing_read) {
    out << "NOTHING CHECKED: no text region was read in any of the " << result.frames
        << " frames, so this is not a pass; check the frames and run a positive control\n";
  } else if (leak_frames.empty()) {
    out << "PASS: no planted string was found readable in any of the " << result.frames
        << " frames (scrub finds only what its OCR reads; limits in docs/cli.md)\n";
  } else {
    out << "LEAK: a planted string is readable in " << leak_frames.size() << " of " << result.frames
        << " frames; look at each one\n";
  }
  if (const auto report = o.get("--report")) {
    json stages = json::object();
    for (const auto& [name, s] : times->summary()) {
      stages[name] = summary_json(s);
    }
    write_json(*report, json{{"schema", 1},
                             {"command", "scrub"},
                             {"version", version()},
                             {"result", !leak_frames.empty() ? "leak" : nothing_read ? "nothing_read" : "pass"},
                             {"fps", seq.fps},
                             {"frames", result.frames},
                             {"text_regions", result.regions},
                             {"planted_strings", planted.size()},
                             {"sensitivity", level},
                             {"workers", setup.driver.workers},
                             {"providers", opened.providers},
                             {"leak_frames", leak_frames.size()},
                             {"leak_frame_indices", leak_frames},
                             {"planted", per_planted},
                             {"leaks", leaks},
                             {"stages", stages}});
  }
  if (!leak_frames.empty()) {
    return kExitLeak;
  }
  return nothing_read ? kExitFailure : kExitOk;
}

}  // namespace

int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err, const CliHooks& hooks) {
  if (args.empty()) {
    err << kUsage;
    return kExitUsage;
  }
  const std::string_view command = args[0];
  const std::span<const std::string_view> rest = args.subspan(1);
  if (command == "--version") {
    out << "spg-cli " << version() << '\n';
    return kExitOk;
  }
  if (command == "--help" || command == "-h") {
    out << kUsage;
    return kExitOk;
  }
  try {
    if (command == "run") {
      return cmd_run(rest, out, err, hooks);
    }
    if (command == "eval") {
      return cmd_suite(rest, out, hooks);
    }
    if (command == "bench") {
      return cmd_bench(rest, out, hooks);
    }
    if (command == "scrub") {
      return cmd_scrub(rest, out, err, hooks);
    }
    err << "spg-cli: unknown command '" << command << "'\n" << kUsage;
    return kExitUsage;
  } catch (const UsageError& e) {
    err << "spg-cli " << command << ": " << e.what() << "\n" << kUsage;
    return kExitUsage;
  } catch (const std::exception& e) {
    // CliError, ContractError (field names and offsets only), filesystem and
    // runtime errors: none carries recognized text or a watch-list entry.
    err << "spg-cli " << command << ": error: " << e.what() << '\n';
    return kExitFailure;
  }
}

}  // namespace spg::cli
