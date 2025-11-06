#pragma once

#include "stress_common.h"
#include <atomic>
#include <memory>

namespace faas {
namespace stress {

// Abstract interface for stress test transports (TCP, Machnet, etc.)
class IStressTransport {
public:
  virtual ~IStressTransport() = default;

  // Initialize the transport (listen on port, etc.)
  virtual void Start() = 0;

  // Stop the transport and clean up resources
  virtual void Stop() = 0;

  // Enable sending requests (after warmup, for example)
  virtual void EnableSending() = 0;

  // Disable sending requests (before shutdown)
  virtual void DisableSending() = 0;

  // Mark warmup as complete (start collecting latency stats)
  virtual void SetWarmupDone() = 0;

  // Get current statistics snapshot
  virtual ConnectionStats GetStats() const = 0;

  // Get number of active connections
  virtual size_t GetConnectionCount() const = 0;

  // Check if at least one connection is established
  virtual bool HasConnections() const = 0;

  // Check if transport should stop
  virtual bool ShouldStop() const = 0;
  
  // Get actual measurement window times for accurate throughput calculation
  // Returns 0 if measurement hasn't started/ended yet
  virtual uint64_t GetMeasurementStartTime() const { return 0; }
  virtual uint64_t GetMeasurementEndTime() const { return 0; }
};

// Factory function to create appropriate transport
std::unique_ptr<IStressTransport> CreateTcpTransport(
    const std::string& listen_addr, int listen_port,
    std::atomic<uint32_t>* call_id_alloc, const TestConfig& config);

std::unique_ptr<IStressTransport> CreateMachnetTransport(
    const std::string& machnet_ip, int listen_port,
    std::atomic<uint32_t>* call_id_alloc, const TestConfig& config);

} // namespace stress
} // namespace faas
