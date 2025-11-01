#pragma once

#include "base/common.h"
#include "common/protocol.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <absl/container/flat_hash_map.h>

namespace faas {
namespace stress {

// Per-connection statistics
struct ConnectionStats {
  uint64_t sent_count = 0;
  uint64_t completed_count = 0;
  uint64_t failed_count = 0;
  std::vector<int64_t> latencies_us;

  ConnectionStats() {
    latencies_us.reserve(1000000); // Pre-allocate for 1M samples
  }
};

// Test configuration parameters
struct TestConfig {
  int func_id;
  int method_id;
  int input_size;
  int target_rps;
  int inflight_limit;
  int duration_sec;
  int warmup_sec;
  int report_interval_sec;
};

// Entry in global inflight registry
struct InflightEntry {
  void* origin;  // Pointer to connection (type depends on transport)
  int64_t send_time;
  uint64_t index;
};

// Global registry for tracking inflight requests
// This is shared across all transports for simplicity
class GlobalInflightRegistry {
public:
  static GlobalInflightRegistry& Instance() {
    static GlobalInflightRegistry instance;
    return instance;
  }

  void Register(uint64_t full_call_id, void* origin, int64_t send_time,
                uint64_t index);
  bool Consume(uint64_t full_call_id, void** origin, int64_t* send_time,
               uint64_t* index);

private:
  GlobalInflightRegistry() = default;
  std::mutex mu_;
  absl::flat_hash_map<uint64_t, InflightEntry> map_;
};

// Utility functions for printing statistics
void PrintProgressStats(int elapsed_sec, const ConnectionStats& stats);
void PrintFinalResults(const ConnectionStats& stats, size_t num_connections,
                       uint64_t start_time_us, uint64_t end_time_us,
                       const char* transport_name);

// Merge multiple connection stats into one
ConnectionStats MergeStats(const std::vector<ConnectionStats>& stats_list);

} // namespace stress
} // namespace faas
