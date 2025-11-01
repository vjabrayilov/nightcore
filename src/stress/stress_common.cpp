#include "stress_common.h"
#include "base/logging.h"
#include <algorithm>
#include <fmt/core.h>

namespace faas {
namespace stress {

void GlobalInflightRegistry::Register(uint64_t full_call_id, void* origin,
                                       int64_t send_time, uint64_t index) {
  std::lock_guard<std::mutex> lk(mu_);
  map_[full_call_id] = InflightEntry{origin, send_time, index};
}

bool GlobalInflightRegistry::Consume(uint64_t full_call_id, void** origin,
                                      int64_t* send_time, uint64_t* index) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = map_.find(full_call_id);
  if (it == map_.end()) {
    return false;
  }
  *origin = it->second.origin;
  *send_time = it->second.send_time;
  *index = it->second.index;
  map_.erase(it);
  return true;
}

void PrintProgressStats(int elapsed_sec, const ConnectionStats& stats) {
  LOG(INFO) << fmt::format(
      "[T+{}s] Sent: {} | Completed: {} | Failed: {} | Latencies: {}",
      elapsed_sec, stats.sent_count, stats.completed_count, stats.failed_count,
      stats.latencies_us.size());
}

void PrintFinalResults(const ConnectionStats& stats, size_t num_connections,
                       uint64_t start_time_us, uint64_t end_time_us,
                       const char* transport_name) {
  double duration_sec = (end_time_us - start_time_us) / 1e6;
  double throughput = stats.completed_count / duration_sec;

  LOG(INFO) << "========================================";
  LOG(INFO) << "=== Stress Test Results ===";
  LOG(INFO) << "========================================";
  LOG(INFO) << fmt::format("Transport:         {}", transport_name);
  LOG(INFO) << fmt::format("Connections:       {}", num_connections);
  LOG(INFO) << fmt::format("Duration:          {:.2f} seconds", duration_sec);
  LOG(INFO) << fmt::format("Total Sent:        {}", stats.sent_count);
  LOG(INFO) << fmt::format(
      "Total Completed:   {} ({:.2f}%)", stats.completed_count,
      100.0 * stats.completed_count / std::max<uint64_t>(1, stats.sent_count));
  LOG(INFO) << fmt::format(
      "Total Failed:      {} ({:.2f}%)", stats.failed_count,
      100.0 * stats.failed_count / std::max<uint64_t>(1, stats.sent_count));
  LOG(INFO) << "";
  LOG(INFO) << fmt::format("Throughput:        {:.1f} rps", throughput);
  LOG(INFO) << "";

  if (!stats.latencies_us.empty()) {
    std::vector<int64_t> sorted_latencies = stats.latencies_us;
    std::sort(sorted_latencies.begin(), sorted_latencies.end());

    size_t n = sorted_latencies.size();
    int64_t min_lat = sorted_latencies[0];
    int64_t p50_lat = sorted_latencies[n * 50 / 100];
    int64_t p90_lat = sorted_latencies[n * 90 / 100];
    int64_t p99_lat = sorted_latencies[n * 99 / 100];
    int64_t p999_lat = sorted_latencies[n * 999 / 1000];
    int64_t max_lat = sorted_latencies[n - 1];

    LOG(INFO) << "Latency (us):";
    LOG(INFO) << fmt::format("  Min:    {}", min_lat);
    LOG(INFO) << fmt::format("  p50:    {}", p50_lat);
    LOG(INFO) << fmt::format("  p90:    {}", p90_lat);
    LOG(INFO) << fmt::format("  p99:    {}", p99_lat);
    LOG(INFO) << fmt::format("  p99.9:  {}", p999_lat);
    LOG(INFO) << fmt::format("  Max:    {}", max_lat);
  } else {
    LOG(INFO) << "No latency data collected";
  }

  LOG(INFO) << "========================================";
}

ConnectionStats MergeStats(const std::vector<ConnectionStats>& stats_list) {
  ConnectionStats merged;
  for (const auto& stats : stats_list) {
    merged.sent_count += stats.sent_count;
    merged.completed_count += stats.completed_count;
    merged.failed_count += stats.failed_count;
    merged.latencies_us.insert(merged.latencies_us.end(),
                               stats.latencies_us.begin(),
                               stats.latencies_us.end());
  }
  return merged;
}

} // namespace stress
} // namespace faas
