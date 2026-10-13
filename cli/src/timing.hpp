#pragma once

// Stage timing for spg-cli (SPG-11): wall-clock durations per pipeline stage
// and the end of each detection cycle. Thread-safe, since realtime workers
// record from several threads. Holds numbers only, never frame content.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "spg/time.hpp"

namespace spg::cli {

// Summary of one stage's samples. Percentiles are nearest-rank, as the eval
// scorer computes p95_detection_latency_ms.
struct StageSummary {
  std::size_t count = 0;
  double total_ms = 0.0;
  double mean_ms = 0.0;
  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double max_ms = 0.0;
};

[[nodiscard]] StageSummary summarize(std::vector<double> samples_ms);

// Milliseconds between two steady_clock points.
[[nodiscard]] double elapsed_ms(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to);
[[nodiscard]] double to_ms(Duration d);

// Stage names used by the drivers and the stage wrappers.
inline constexpr std::string_view kStageDecode = "decode";
inline constexpr std::string_view kStageFaces = "face_detect";
inline constexpr std::string_view kStageText = "text_detect";
inline constexpr std::string_view kStageRecognize = "recognize";
inline constexpr std::string_view kStagePolicy = "policy";
inline constexpr std::string_view kStageCycle = "cycle";
inline constexpr std::string_view kStageCompose = "compose";

class StageTimes {
 public:
  void add(std::string_view stage, double ms);
  [[nodiscard]] std::map<std::string, StageSummary> summary() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::vector<double>, std::less<>> samples_;
};

// When each detection cycle's last stage call ended, keyed by the timestamp
// of the readback it scanned (one cycle per readback).
class CycleLog {
 public:
  void note(Timestamp readback, Timestamp end);
  // end - readback per cycle, in ms, ordered by readback timestamp.
  [[nodiscard]] std::vector<double> latencies_ms() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::int64_t, Timestamp> ends_;
};

}  // namespace spg::cli
