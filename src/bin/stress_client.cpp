#include "base/init.h"
#include "server/server_base.h"
#include "stress/stress_common.h"
#include "stress/stress_transport.h"

#include <absl/flags/flag.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <signal.h>
#include <thread>

ABSL_FLAG(std::string, listen_addr, "0.0.0.0",
          "Address to listen for engine connections");
ABSL_FLAG(int, listen_port, 10007, "Port for engine connections");
ABSL_FLAG(bool, use_machnet, false,
          "Use Machnet instead of TCP for engine connections");
ABSL_FLAG(std::string, machnet_ip, "",
          "Machnet IP address (required if use_machnet=true)");
ABSL_FLAG(int, func_id, 1, "Function ID to invoke");
ABSL_FLAG(int, method_id, 0, "Method ID (for gRPC, 0 for HTTP)");
ABSL_FLAG(int, duration_sec, 30, "Duration of stress test in seconds");
ABSL_FLAG(int, target_rps, 0,
          "Global target RPS across all connections (0 = unlimited)");
ABSL_FLAG(int, input_size, 64, "Size of input payload in bytes");
ABSL_FLAG(int, inflight_limit, 1000, "Max inflight requests per connection");
ABSL_FLAG(int, report_interval_sec, 1, "Interval for printing statistics");
ABSL_FLAG(int, warmup_sec, 5, "Warmup period before starting measurement");

namespace faas {
namespace stress {

class StressClient : public server::ServerBase {
public:
  StressClient() : next_call_id_(1) {}
  ~StressClient() {}

  void SetTransport(std::unique_ptr<IStressTransport> transport) {
    transport_ = std::move(transport);
  }

  void RunStressTest(const TestConfig& config) {
    LOG(INFO) << "=== Nightcore Stress Test ===";
    LOG(INFO) << "Function ID: " << config.func_id;
    LOG(INFO) << "Method ID: " << config.method_id;
    LOG(INFO) << "Input size: " << config.input_size << " bytes";
    LOG(INFO) << "Global target RPS: "
              << (config.target_rps == 0 ? "unlimited"
                                         : std::to_string(config.target_rps));
    LOG(INFO) << "Inflight limit per connection: " << config.inflight_limit;
    LOG(INFO) << "Warmup: " << config.warmup_sec
              << "s, Duration: " << config.duration_sec << "s";
    LOG(INFO) << "";
    LOG(INFO) << "========================================";

    // Wait for at least one engine connection
    LOG(INFO) << "Waiting for Engine to connect...";
    while (true) {
      if (transport_->HasConnections() || transport_->ShouldStop()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (transport_->ShouldStop()) {
      LOG(INFO) << "Interrupted before engine connected";
      return;
    }

    size_t num_connections = transport_->GetConnectionCount();
    LOG(INFO) << "Engine connected with " << num_connections
              << " connection(s)";
    LOG(INFO) << "Global target RPS: "
              << (config.target_rps == 0 ? "unlimited"
                                         : std::to_string(config.target_rps));
    LOG(INFO) << "";

    // Wait for workers to initialize before starting to send requests
    LOG(INFO) << "Waiting 3 seconds for workers to initialize...";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Warmup phase
    if (config.warmup_sec > 0) {
      LOG(INFO) << "Warmup phase: " << config.warmup_sec << " seconds...";
      transport_->EnableSending();
      std::this_thread::sleep_for(std::chrono::seconds(config.warmup_sec));
      transport_->SetWarmupDone();
      LOG(INFO) << "Warmup complete. Starting measurement...";
      LOG(INFO) << "";
    } else {
      transport_->SetWarmupDone();
      transport_->EnableSending();
    }

    uint64_t start_time = uv_hrtime() / 1000; // convert ns -> us

    // Test phase with periodic reporting
    for (int elapsed = 0;
         elapsed < config.duration_sec && !transport_->ShouldStop();
         elapsed += config.report_interval_sec) {
      std::this_thread::sleep_for(std::chrono::seconds(
          std::min(config.report_interval_sec, config.duration_sec - elapsed)));

      if (!transport_->ShouldStop()) {
        ConnectionStats stats = transport_->GetStats();
        PrintProgressStats(elapsed + config.report_interval_sec, stats);
      }
    }

    uint64_t end_time = uv_hrtime() / 1000; // convert ns -> us

    // Disable sending
    transport_->DisableSending();

    // Print final results
    LOG(INFO) << "";
    ConnectionStats final_stats = transport_->GetStats();
    const char* transport_name =
        absl::GetFlag(FLAGS_use_machnet) ? "Machnet" : "TCP";
    PrintFinalResults(final_stats, num_connections, start_time, end_time,
                      transport_name);
  }

private:
  std::unique_ptr<IStressTransport> transport_;
  std::atomic<uint32_t> next_call_id_;

  void StartInternal() override {
    if (transport_) {
      transport_->Start();
    }
  }

  void StopInternal() override {
    if (transport_) {
      transport_->Stop();
    }
  }
};

} // namespace stress
} // namespace faas

static std::atomic<faas::stress::StressClient*> g_stress_client(nullptr);

void SignalHandler(int signal) {
  faas::stress::StressClient* client = g_stress_client.exchange(nullptr);
  if (client != nullptr) {
    LOG(INFO) << "Received signal, stopping...";
    client->ScheduleStop();
  }
}

int main(int argc, char* argv[]) {
  signal(SIGINT, SignalHandler);
  faas::base::InitMain(argc, argv);

  // Build test configuration
  faas::stress::TestConfig config;
  config.func_id = absl::GetFlag(FLAGS_func_id);
  config.method_id = absl::GetFlag(FLAGS_method_id);
  config.input_size = absl::GetFlag(FLAGS_input_size);
  config.target_rps = absl::GetFlag(FLAGS_target_rps);
  config.inflight_limit = absl::GetFlag(FLAGS_inflight_limit);
  config.duration_sec = absl::GetFlag(FLAGS_duration_sec);
  config.warmup_sec = absl::GetFlag(FLAGS_warmup_sec);
  config.report_interval_sec = absl::GetFlag(FLAGS_report_interval_sec);

  auto client = std::make_unique<faas::stress::StressClient>();
  std::atomic<uint32_t> call_id_alloc(1);

  // Create appropriate transport
  bool use_machnet = absl::GetFlag(FLAGS_use_machnet);
  std::unique_ptr<faas::stress::IStressTransport> transport;

  if (use_machnet) {
    std::string machnet_ip = absl::GetFlag(FLAGS_machnet_ip);
    int listen_port = absl::GetFlag(FLAGS_listen_port);
    LOG(INFO) << "Using Machnet transport on " << machnet_ip << ":"
              << listen_port;
    transport = faas::stress::CreateMachnetTransport(machnet_ip, listen_port,
                                                      &call_id_alloc, config);
  } else {
    std::string listen_addr = absl::GetFlag(FLAGS_listen_addr);
    int listen_port = absl::GetFlag(FLAGS_listen_port);
    LOG(INFO) << "Using TCP transport on " << listen_addr << ":" << listen_port;
    transport = faas::stress::CreateTcpTransport(listen_addr, listen_port,
                                                  &call_id_alloc, config);
  }

  client->SetTransport(std::move(transport));
  g_stress_client.store(client.get());

  client->Start();

  // Run stress test directly in main thread
  std::this_thread::sleep_for(
      std::chrono::milliseconds(500)); // Let server start
  client->RunStressTest(config);
  client->ScheduleStop();

  client->WaitForFinish();

  return 0;
}
