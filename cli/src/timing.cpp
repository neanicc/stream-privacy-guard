#include "timing.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace spg::cli {

namespace {

double nearest_rank(const std::vector<double>& sorted, double q) {
  const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(sorted.size())));
  return sorted[std::max<std::size_t>(rank, 1) - 1];
}

}  // namespace

StageSummary summarize(std::vector<double> samples_ms) {
  StageSummary s;
  if (samples_ms.empty()) {
    return s;
  }
  std::sort(samples_ms.begin(), samples_ms.end());
  s.count = samples_ms.size();
  s.total_ms = std::accumulate(samples_ms.begin(), samples_ms.end(), 0.0);
  s.mean_ms = s.total_ms / static_cast<double>(s.count);
  s.p50_ms = nearest_rank(samples_ms, 0.50);
  s.p95_ms = nearest_rank(samples_ms, 0.95);
  s.max_ms = samples_ms.back();
  return s;
}

double elapsed_ms(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

double to_ms(Duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

void StageTimes::add(std::string_view stage, double ms) {
  const std::lock_guard lock(mutex_);
  auto it = samples_.find(stage);
  if (it == samples_.end()) {
    it = samples_.emplace(std::string(stage), std::vector<double>{}).first;
  }
  it->second.push_back(ms);
}

std::map<std::string, StageSummary> StageTimes::summary() const {
  const std::lock_guard lock(mutex_);
  std::map<std::string, StageSummary> out;
  for (const auto& [stage, samples] : samples_) {
    out.emplace(stage, summarize(samples));
  }
  return out;
}

void CycleLog::note(Timestamp readback, Timestamp end) {
  const std::lock_guard lock(mutex_);
  auto [it, inserted] = ends_.emplace(to_ns(readback), end);
  if (!inserted && it->second < end) {
    it->second = end;
  }
}

std::vector<double> CycleLog::latencies_ms() const {
  const std::lock_guard lock(mutex_);
  std::vector<double> out;
  out.reserve(ends_.size());
  for (const auto& [readback_ns, end] : ends_) {
    out.push_back(to_ms(end - timestamp_from_ns(readback_ns)));
  }
  return out;
}

}  // namespace spg::cli
