#pragma once

#include "stress_transport.h"
#include "common/machnet_transport.h"
#include "common/protocol.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace faas {
namespace stress {

class MachnetStressTransport : public IStressTransport {
public:
  MachnetStressTransport(const std::string& machnet_ip, int listen_port,
                         std::atomic<uint32_t>* call_id_alloc,
                         const TestConfig& config);
  ~MachnetStressTransport() override;

  void Start() override;
  void Stop() override;
  void EnableSending() override;
  void DisableSending() override;
  void SetWarmupDone() override;
  ConnectionStats GetStats() const override;
  size_t GetConnectionCount() const override;
  bool HasConnections() const override;
  bool ShouldStop() const override { return should_stop_.load(); }
  
  // Get actual measurement window times for accurate throughput calculation
  uint64_t GetMeasurementStartTime() const override { return measurement_start_time_; }
  uint64_t GetMeasurementEndTime() const override { return measurement_end_time_; }

private:
  std::string machnet_ip_;
  int listen_port_;
  std::atomic<uint32_t>* call_id_alloc_;
  TestConfig config_;

  // Machnet support
  std::unique_ptr<machnet::MachnetListener> machnet_listener_;
  std::atomic<machnet::MachnetConnection*> machnet_single_conn_{nullptr};

  // Machnet stress test state - lock-free counters
  std::atomic<uint64_t> sent_count_{0};
  std::atomic<uint64_t> completed_count_{0};
  std::atomic<uint64_t> failed_count_{0};
  std::vector<int64_t> latencies_us_;
  std::mutex latencies_mu_; // Only for latencies vector

  std::atomic<bool> warmup_done_{false};
  std::atomic<bool> sending_enabled_{false};
  std::atomic<bool> should_stop_{false};

  // Open-loop scheduling for single connection (all in nanoseconds)
  uint64_t inter_send_ns_{0};
  uint64_t next_send_time_ns_{0};
  uint64_t test_start_time_ns_{0};
  uint64_t test_end_time_ns_{0};
  
  // Actual measurement window (stored in microseconds for reporting)
  std::atomic<uint64_t> measurement_start_time_{0};
  std::atomic<uint64_t> measurement_end_time_{0};

  // Tracking for request timestamps
  uint32_t base_call_id_start_{0};
  std::vector<int64_t> send_ts_by_index_;
  std::string input_buffer_;

  // Single event-loop thread
  std::thread loop_thread_;

  void OnNewMachnetConnection(machnet::MachnetConnection* connection);
  void OnRecvMachnetEngineMessage(machnet::MachnetConnection* connection,
                                   const protocol::GatewayMessage& message,
                                   std::span<const char> payload);

  void LoopThreadMain();
};

} // namespace stress
} // namespace faas
