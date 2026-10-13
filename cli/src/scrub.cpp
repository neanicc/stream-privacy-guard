#include "scrub.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include "image_io.hpp"
#include "inputs.hpp"
#include "spg/category.hpp"
#include "spg/detection.hpp"
#include "spg/error.hpp"
#include "spg/frame.hpp"
#include "spg/secure_memory.hpp"

namespace spg::cli {

namespace {

bool blank(std::string_view line) noexcept {
  return std::all_of(line.begin(), line.end(),
                     [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; });
}

// Wipes a std::string's buffer before it is released.
class WipedString {
 public:
  WipedString() = default;
  WipedString(const WipedString&) = delete;
  WipedString& operator=(const WipedString&) = delete;
  ~WipedString() { secure_wipe(text.data(), text.size()); }
  std::string text;
};

}  // namespace

std::vector<Secret> read_planted(const std::filesystem::path& file) {
  WipedString content;
  {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      throw CliError("cannot open " + file.string());
    }
    content.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
      throw CliError("cannot read " + file.string());
    }
  }
  std::vector<Secret> planted;
  std::string_view rest = content.text;
  while (!rest.empty()) {
    const std::size_t end = rest.find('\n');
    std::string_view line = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (!blank(line)) {
      planted.push_back(Secret::copy_of(line));
    }
  }
  if (planted.empty()) {
    throw CliError(file.string() + " holds no planted string (one per line)");
  }
  return planted;
}

FuzzyLimits scrub_fuzzy(ScrubSensitivity sensitivity) noexcept {
  FuzzyLimits limits;  // spg-core defaults: 2 edits, ratio 0.2, partials from 6 characters
  if (sensitivity == ScrubSensitivity::High) {
    // The plugin's High sensitivity (fuzzy_limits() in
    // plugin/src/settings/filter_settings.cpp).
    limits.max_edits = 3;
    limits.max_edit_ratio = 0.3;
    limits.min_partial_chars = 5;
  }
  return limits;
}

PlantedMatcher::PlantedMatcher(const std::vector<Secret>& planted, FuzzyLimits fuzzy) {
  engines_.reserve(planted.size());
  for (std::size_t i = 0; i < planted.size(); ++i) {
    PolicyConfig config;
    config.categories = CategorySet{};
    config.categories.insert(Category::Watchlist);
    config.fuzzy = fuzzy;
    config.watchlist.push_back(planted[i]);
    try {
      engines_.push_back(std::make_unique<const PolicyEngine>(config));
    } catch (const ContractError&) {
      // The engine's message names its own entry index (always 0 here);
      // name the planted string's instead.
      throw CliError("planted string #" + std::to_string(i + 1) +
                     " cannot be matched: it is blank or has no letters or digits");
    }
  }
}

std::vector<std::size_t> PlantedMatcher::match(const Secret& text) const {
  std::vector<std::size_t> hits;
  for (std::size_t i = 0; i < engines_.size(); ++i) {
    if (engines_[i]->evaluate(text).sensitive()) {
      hits.push_back(i + 1);
    }
  }
  return hits;
}

std::vector<std::size_t> ScrubResult::leak_frames() const {
  std::vector<std::size_t> out;
  for (const ScrubLeak& leak : leaks) {
    if (out.empty() || out.back() != leak.frame) {
      out.push_back(leak.frame);
    }
  }
  return out;
}

std::vector<std::size_t> ScrubResult::leak_frames_of(std::size_t index) const {
  std::vector<std::size_t> out;
  for (const ScrubLeak& leak : leaks) {
    if (std::find(leak.planted.begin(), leak.planted.end(), index) != leak.planted.end() &&
        (out.empty() || out.back() != leak.frame)) {
      out.push_back(leak.frame);
    }
  }
  return out;
}

ScrubResult scrub(const FrameSource& source, const ScanStages& stages, const PlantedMatcher& matcher,
                  const ScrubOptions& options, StageTimes& times) {
  if (stages.text == nullptr || stages.recognizer == nullptr) {
    throw CliError("scrub needs the text detector and the recognizer");
  }
  if (options.workers == 0) {
    throw CliError("scrub needs at least one worker");
  }
  if (source.count == 0 || source.fps == 0 || !source.load) {
    throw CliError("scrub needs at least one frame and a frame rate");
  }

  struct FrameOutcome {
    std::size_t regions = 0;
    std::vector<ScrubLeak> leaks;
  };
  std::vector<FrameOutcome> outcomes(source.count);
  std::atomic<std::size_t> next{0};
  std::atomic<bool> failed{false};
  std::mutex mutex;  // guards first_error, done and the progress callback
  std::exception_ptr first_error;
  std::size_t done = 0;

  const auto scan_one = [&](std::size_t i) {
    const BgraImage image = source.load(i);
    validate(image);
    const FrameView view = image.view(frame_time(i, source.fps));
    const std::vector<Detection> regions = stages.text->detect(view, image.size());
    if (regions.empty()) {
      return;
    }
    const std::vector<Secret> texts = stages.recognizer->recognize(view, image.size(), regions);
    if (texts.size() != regions.size()) {
      throw CliError("the recognizer returned " + std::to_string(texts.size()) + " texts for " +
                     std::to_string(regions.size()) + " regions");
    }
    FrameOutcome& outcome = outcomes[i];
    outcome.regions = regions.size();
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t r = 0; r < regions.size(); ++r) {
      std::vector<std::size_t> hits = matcher.match(texts[r]);
      if (!hits.empty()) {
        outcome.leaks.push_back(ScrubLeak{i, regions[r].box, regions[r].score, std::move(hits)});
      }
    }
    times.add(kStagePolicy, elapsed_ms(start, std::chrono::steady_clock::now()));
  };

  const auto worker = [&] {
    while (!failed.load(std::memory_order_relaxed)) {
      const std::size_t i = next.fetch_add(1);
      if (i >= source.count) {
        return;
      }
      try {
        scan_one(i);
      } catch (...) {
        const std::lock_guard lock(mutex);
        if (!first_error) {
          first_error = std::current_exception();
        }
        failed.store(true, std::memory_order_relaxed);
        return;
      }
      const std::lock_guard lock(mutex);
      ++done;
      if (options.progress) {
        options.progress(done);
      }
    }
  };

  const std::size_t threads = std::min(options.workers, source.count);
  {
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    try {
      for (std::size_t t = 1; t < threads; ++t) {
        pool.emplace_back(worker);
      }
    } catch (...) {
      // A thread that could not start: the others and this one still drain
      // the queue, so every frame is scanned.
    }
    worker();
    for (std::thread& t : pool) {
      t.join();
    }
  }
  if (first_error) {
    std::rethrow_exception(first_error);
  }

  ScrubResult result;
  result.frames = source.count;
  for (FrameOutcome& outcome : outcomes) {
    result.regions += outcome.regions;
    for (ScrubLeak& leak : outcome.leaks) {
      result.leaks.push_back(std::move(leak));
    }
  }
  return result;
}

}  // namespace spg::cli
