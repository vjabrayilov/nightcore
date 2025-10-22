#include "base/init.h"
#include "common/protocol.h"
#include "common/time.h"
#include "common/uv.h"
#include "server/server_base.h"
#include "utils/appendable_buffer.h"
#include "common/machnet_transport.h"

#include <absl/flags/flag.h>
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

using protocol::FuncCall;
using protocol::GatewayMessage;
using protocol::GetFuncCallFromMessage;
using protocol::IsEngineHandshakeMessage;
using protocol::IsFuncCallCompleteMessage;
using protocol::IsFuncCallFailedMessage;
using protocol::NewDispatchFuncCallGatewayMessage;
using protocol::NewFuncCall;

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

class StressClient;
class EngineConnection;

namespace {
struct InflightEntry {
  EngineConnection *origin;
  int64_t send_time;
  uint64_t index;
};

struct GlobalInflightRegistry {
  std::mutex mu;
  absl::flat_hash_map<uint64_t, InflightEntry> map;
} g_inflight_registry;

inline void RegisterInflight(uint64_t full_call_id, EngineConnection *origin,
                             int64_t send_time, uint64_t index) {
  std::lock_guard<std::mutex> lk(g_inflight_registry.mu);
  g_inflight_registry.map[full_call_id] =
      InflightEntry{origin, send_time, index};
}

inline bool ConsumeInflight(uint64_t full_call_id, EngineConnection **origin,
                            int64_t *send_time, uint64_t *index) {
  std::lock_guard<std::mutex> lk(g_inflight_registry.mu);
  auto it = g_inflight_registry.map.find(full_call_id);
  if (it == g_inflight_registry.map.end()) {
    return false;
  }
  *origin = it->second.origin;
  *send_time = it->second.send_time;
  *index = it->second.index;
  g_inflight_registry.map.erase(it);
  return true;
}
} // anonymous namespace

// Connection to Engine - handles both sending and receiving in event loop
class EngineConnection {
public:
  EngineConnection(std::atomic<uint32_t> *call_id_alloc, uint16_t node_id,
                   uint16_t conn_id, int func_id, int method_id, int input_size,
                   int target_rps, int inflight_limit)
      : call_id_alloc_(call_id_alloc), node_id_(node_id), conn_id_(conn_id),
        func_id_(func_id), method_id_(method_id), input_size_(input_size),
        target_rps_(target_rps), inflight_limit_(inflight_limit),
        state_(kCreated), loop_(nullptr), warmup_done_(false),
        test_enabled_(false), should_stop_(false) {

    input_buffer_.resize(input_size_, 'x');
  }

  ~EngineConnection() { DCHECK(state_ == kCreated || state_ == kClosed); }

  void StartFromFd(int fd) {
    LOG(INFO) << "Starting EngineConnection thread";
    DCHECK(state_ == kCreated);
    owned_fd_ = fd;
    thread_ = std::thread(&EngineConnection::ThreadMain, this);
  }

  void EnableSending() { test_enabled_.store(true, std::memory_order_release); }

  void DisableSending() {
    test_enabled_.store(false, std::memory_order_release);
  }

  void SetTargetRps(int target_rps) { target_rps_ = target_rps; }

  void SetWarmupDone() { warmup_done_.store(true, std::memory_order_release); }

  uint16_t node_id() const { return node_id_; }
  uint16_t conn_id() const { return conn_id_; }
  const ConnectionStats &stats() const { return stats_; }

  void Close() {
    if (state_ == kClosed)
      return;
    test_enabled_.store(false, std::memory_order_release);
    should_stop_.store(true, std::memory_order_release);
    if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) {
      thread_.join();
    }
    state_ = kClosed;
  }

  void OnInflightCompletedFromAnyThread(bool success, int64_t send_time,
                                        uint64_t index, int64_t recv_time) {
    inflight_count_.fetch_sub(1, std::memory_order_acq_rel);
    {
      std::lock_guard<std::mutex> lk(stats_mu_);
      if (success) {
        stats_.completed_count++;
        if (warmup_done_.load(std::memory_order_acquire)) {
          int64_t latency_us = recv_time - send_time;
          stats_.latencies_us.push_back(latency_us);
        }
      } else {
        stats_.failed_count++;
      }
    }
  }

  ConnectionStats SnapshotStats() const {
    std::lock_guard<std::mutex> lk(
        const_cast<EngineConnection *>(this)->stats_mu_);
    return stats_;
  }

private:
  enum State { kCreated, kRunning, kClosed };

  std::atomic<uint32_t> *call_id_alloc_;
  uint16_t node_id_;
  uint16_t conn_id_;
  int func_id_;
  int method_id_;
  int input_size_;
  int target_rps_;
  int inflight_limit_;

  State state_;
  uv_loop_t *loop_;
  uv_tcp_t handle_;
  utils::AppendableBuffer read_buffer_;

  std::atomic<bool> warmup_done_;
  std::atomic<bool> test_enabled_;
  std::atomic<bool> should_stop_;
  std::string input_buffer_;

  ConnectionStats stats_;
  absl::flat_hash_map<uint64_t, int64_t>
      inflight_requests_; // call_id -> send_timestamp
  absl::flat_hash_map<uint64_t, uint64_t>
      inflight_indices_; // call_id -> send_index

  std::mutex stats_mu_;
  std::atomic<size_t> inflight_count_{0};

  std::thread thread_;
  int owned_fd_ = -1;
  int64_t pacing_window_start_us_ = 0;
  uint32_t sent_in_window_ = 0;
  uint64_t sent_message_index_ = 0;

  static void AllocBuffer(uv_handle_t *handle, size_t suggested_size,
                          uv_buf_t *buf) {
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
  }

  static void OnRecvData(uv_stream_t *stream, ssize_t nread,
                         const uv_buf_t *buf) {
    auto *conn = reinterpret_cast<EngineConnection *>(stream->data);
    std::unique_ptr<char[]> buffer_guard(buf->base);

    if (nread < 0) {
      // Close this connection on error/EOF so loop can exit cleanly
      conn->Close();
      return;
    }
    if (nread == 0)
      return;

    conn->read_buffer_.AppendData(buf->base, nread);
    conn->ProcessMessages();
  }

  void ThreadMain() {
    uv_loop_t owned_loop;
    UV_CHECK_OK(uv_loop_init(&owned_loop));
    loop_ = &owned_loop;

    UV_CHECK_OK(uv_tcp_init(loop_, &handle_));
    handle_.data = this;
    UV_CHECK_OK(uv_tcp_open(&handle_, owned_fd_));
    UV_DCHECK_OK(uv_tcp_nodelay(&handle_, 1));
    UV_DCHECK_OK(uv_tcp_keepalive(&handle_, 1, 1));
    UV_DCHECK_OK(
        uv_read_start(UV_AS_STREAM(&handle_), AllocBuffer, OnRecvData));

    state_ = kRunning;
    pacing_window_start_us_ = GetMonotonicMicroTimestamp();
    sent_in_window_ = 0;

    while (!should_stop_.load(std::memory_order_acquire)) {
      if (test_enabled_.load(std::memory_order_acquire) && state_ == kRunning) {
        // Refresh pacing window (1s)
        int64_t now = GetMonotonicMicroTimestamp();
        if (now - pacing_window_start_us_ >= 1000000) {
          pacing_window_start_us_ = now;
          sent_in_window_ = 0;
        }

        size_t capacity_left = 0;
        if (inflight_limit_ > 0) {
          size_t current_inflight =
              inflight_count_.load(std::memory_order_acquire);
          if (current_inflight < static_cast<size_t>(inflight_limit_)) {
            capacity_left =
                static_cast<size_t>(inflight_limit_) - current_inflight;
          } else {
            capacity_left = 0;
          }
        } else {
          capacity_left = std::numeric_limits<size_t>::max();
        }

        size_t quota_left = std::numeric_limits<size_t>::max();
        if (target_rps_ > 0) {
          if (sent_in_window_ >= static_cast<uint32_t>(target_rps_)) {
            quota_left = 0;
          } else {
            quota_left = static_cast<size_t>(target_rps_ - sent_in_window_);
          }
        }

        size_t to_send = std::min(capacity_left, quota_left);
        for (size_t i = 0; i < to_send && state_ == kRunning; i++) {
          int64_t send_ts = GetMonotonicMicroTimestamp();
          SendRequest(send_ts);
          if (target_rps_ > 0) {
            sent_in_window_++;
          }
        }
      }

      // Pump the loop to receive completions and write callbacks
      uv_run(loop_, UV_RUN_NOWAIT);
      // Busy spin by design (no sleeps)
    }

    // Begin shutdown on connection thread
    uv_read_stop(UV_AS_STREAM(&handle_));
    uv_close(UV_AS_HANDLE(&handle_), nullptr);
    while (uv_run(loop_, UV_RUN_NOWAIT) != 0) {
    }
    UV_CHECK_OK(uv_loop_close(loop_));
    state_ = kClosed;
  }

  void SendRequest(int64_t send_time) {
    // Use client_id=0 for external requests (like Gateway does)
    uint16_t client_id = 0;
    uint32_t call_id = call_id_alloc_->fetch_add(1, std::memory_order_relaxed);

    FuncCall func_call = NewFuncCall(func_id_, client_id, call_id);
    if (method_id_ > 0) {
      func_call.method_id = method_id_;
    }

    GatewayMessage message = NewDispatchFuncCallGatewayMessage(func_call);
    message.payload_size = input_size_;

    uint64_t current_index = ++sent_message_index_;
    SendMessage(message,
                std::span<const char>(input_buffer_.data(), input_size_));

    RegisterInflight(func_call.full_call_id, this, send_time, current_index);
    inflight_count_.fetch_add(1, std::memory_order_acq_rel);
    stats_.sent_count++;
  }

  void SendMessage(const GatewayMessage &message,
                   std::span<const char> payload) {
    if (state_ != kRunning)
      return;

    size_t total_size = sizeof(GatewayMessage) + payload.size();
    char *buffer = new char[total_size];
    memcpy(buffer, &message, sizeof(GatewayMessage));
    if (!payload.empty()) {
      memcpy(buffer + sizeof(GatewayMessage), payload.data(), payload.size());
    }

    uv_buf_t buf = uv_buf_init(buffer, total_size);
    uv_write_t *req = new uv_write_t();
    req->data = buffer;

    int status = uv_write(req, UV_AS_STREAM(&handle_), &buf, 1, OnSendComplete);
    if (status != 0) {
      delete[] buffer;
      delete req;
    }
  }

  static void OnSendComplete(uv_write_t *req, int status) {
    auto *buffer = reinterpret_cast<char *>(req->data);
    delete[] buffer;
    delete req;
  }

  void ProcessMessages() {
    while (state_ == kRunning &&
           read_buffer_.length() >= sizeof(GatewayMessage)) {
      GatewayMessage *message =
          reinterpret_cast<GatewayMessage *>(read_buffer_.data());
      size_t full_size =
          sizeof(GatewayMessage) + std::max<size_t>(0, message->payload_size);

      if (read_buffer_.length() >= full_size) {
        std::span<const char> payload(read_buffer_.data() +
                                          sizeof(GatewayMessage),
                                      full_size - sizeof(GatewayMessage));
        HandleResponse(*message, payload);
        read_buffer_.ConsumeFront(full_size);
      } else {
        break;
      }
    }
  }

  void HandleResponse(const GatewayMessage &message,
                      std::span<const char> payload) {
    if (state_ != kRunning)
      return;

    int64_t recv_time = GetMonotonicMicroTimestamp();

    if (IsFuncCallCompleteMessage(message) ||
        IsFuncCallFailedMessage(message)) {
      FuncCall func_call = GetFuncCallFromMessage(message);

      EngineConnection *origin = nullptr;
      int64_t send_time = 0;
      uint64_t recv_index = 0;
      if (!ConsumeInflight(func_call.full_call_id, &origin, &send_time,
                           &recv_index)) {
        return; // Unknown response
      }

      if (IsFuncCallCompleteMessage(message)) {
        origin->OnInflightCompletedFromAnyThread(/*success=*/true, send_time,
                                                 recv_index, recv_time);
      } else if (IsFuncCallFailedMessage(message)) {
        origin->OnInflightCompletedFromAnyThread(/*success=*/false, send_time,
                                                 recv_index, recv_time);
      }
    }
  }
};

// Main stress client
class StressClient : public server::ServerBase {
public:
  StressClient()
      : listen_backlog_(64), use_machnet_(false), should_stop_(false),
        test_started_(false), start_time_(0), end_time_(0), next_call_id_(1) {}

  ~StressClient() {}

  void set_listen_addr(std::string_view addr) {
    listen_addr_ = std::string(addr);
  }
  void set_listen_port(int port) { listen_port_ = port; }
  void set_use_machnet(bool value) { use_machnet_ = value; }
  void set_machnet_ip(std::string_view ip) { machnet_ip_ = std::string(ip); }

  uint32_t NextCallId() {
    return next_call_id_.fetch_add(1, std::memory_order_relaxed);
  }

  void RunStressTest() {
    int warmup_sec = absl::GetFlag(FLAGS_warmup_sec);
    int duration_sec = absl::GetFlag(FLAGS_duration_sec);
    int report_interval = absl::GetFlag(FLAGS_report_interval_sec);
    int func_id = absl::GetFlag(FLAGS_func_id);
    int method_id = absl::GetFlag(FLAGS_method_id);
    int input_size = absl::GetFlag(FLAGS_input_size);
    int target_rps = absl::GetFlag(FLAGS_target_rps);
    int inflight_limit = absl::GetFlag(FLAGS_inflight_limit);

    // Store test params for when connections arrive
    func_id_ = func_id;
    method_id_ = method_id;
    input_size_ = input_size;
    target_rps_ = target_rps;
    inflight_limit_ = inflight_limit;

    LOG(INFO) << "=== Nightcore Stress Test ===";
    LOG(INFO) << "Function ID: " << func_id;
    LOG(INFO) << "Method ID: " << method_id;
    LOG(INFO) << "Input size: " << input_size << " bytes";
    LOG(INFO) << "Global target RPS: "
              << (target_rps == 0 ? "unlimited" : std::to_string(target_rps));
    LOG(INFO) << "Inflight limit per connection: " << inflight_limit;
    LOG(INFO) << "Warmup: " << warmup_sec << "s, Duration: " << duration_sec
              << "s";
    LOG(INFO) << "";
    LOG(INFO) << "========================================";

    // Wait for at least one engine connection (TCP or Machnet)
    LOG(INFO) << "Waiting for Engine to connect...";
    while (true) {
      bool has_connections = false;
      if (use_machnet_) {
        std::lock_guard<std::mutex> lk(machnet_connections_mu_);
        has_connections = !machnet_connections_.empty();
      } else {
        has_connections = !connections_.empty();
      }

      if (has_connections || should_stop_.load()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (should_stop_.load()) {
      LOG(INFO) << "Interrupted before engine connected";
      return;
    }

    size_t num_connections = use_machnet_
        ? ([&]() { std::lock_guard<std::mutex> lk(machnet_connections_mu_); return machnet_connections_.size(); })()
        : connections_.size();

    LOG(INFO) << "Engine connected with " << num_connections
              << " connection(s) via " << (use_machnet_ ? "Machnet" : "TCP");
    LOG(INFO) << "Global target RPS: "
              << (target_rps == 0 ? "unlimited" : std::to_string(target_rps));
    LOG(INFO) << "";

    // Wait for workers to initialize before starting to send requests
    LOG(INFO) << "Waiting 3 seconds for workers to initialize...";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    if (use_machnet_) {
      // Machnet mode - run test from main thread
      RunMachnetStressTest(warmup_sec, duration_sec, report_interval);
      return;
    }

    // TCP mode - distribute global target RPS across connections and enable sending
    if (!connections_.empty()) {
      if (target_rps_ <= 0) {
        for (auto &conn : connections_) {
          conn->SetTargetRps(0); // unlimited per connection
        }
      } else {
        int base = target_rps_ / static_cast<int>(connections_.size());
        int rem = target_rps_ % static_cast<int>(connections_.size());
        for (size_t i = 0; i < connections_.size(); i++) {
          int per_conn = base + (static_cast<int>(i) < rem ? 1 : 0);
          connections_[i]->SetTargetRps(per_conn);
        }
        LOG(INFO) << fmt::format(
            "Per-connection target RPS: base={} (+1 for first {} conns)", base,
            rem);
      }
    }

    for (auto &conn : connections_) {
      conn->EnableSending();
    }

    test_started_ = true;

    // Warmup phase
    if (warmup_sec > 0) {
      LOG(INFO) << "Warmup phase: " << warmup_sec << " seconds...";
      std::this_thread::sleep_for(std::chrono::seconds(warmup_sec));

      for (auto &conn : connections_) {
        conn->SetWarmupDone();
      }

      LOG(INFO) << "Warmup complete. Starting measurement...";
      LOG(INFO) << "";
    } else {
      for (auto &conn : connections_) {
        conn->SetWarmupDone();
      }
    }

    start_time_ = GetMonotonicMicroTimestamp();

    // Test phase with periodic reporting
    for (int elapsed = 0; elapsed < duration_sec && !should_stop_.load();
         elapsed += report_interval) {
      std::this_thread::sleep_for(std::chrono::seconds(
          std::min(report_interval, duration_sec - elapsed)));

      if (!should_stop_.load()) {
        PrintProgress(elapsed + report_interval);
      }
    }

    end_time_ = GetMonotonicMicroTimestamp();

    // Disable sending on all connections
    for (auto &conn : connections_) {
      conn->DisableSending();
    }

    // Print final results
    LOG(INFO) << "";
    PrintFinalResults();
  }

private:
  std::string listen_addr_;
  int listen_port_;
  int listen_backlog_;
  bool use_machnet_;
  std::string machnet_ip_;
  uv_tcp_t listen_handle_;

  // Machnet support
  std::unique_ptr<machnet::MachnetListener> machnet_listener_;
  uv_prepare_t machnet_poll_prepare_;
  uv_idle_t machnet_send_idle_;  // Idle callback for sending requests
  std::vector<machnet::MachnetConnection *> machnet_connections_;
  std::mutex machnet_connections_mu_;
  // Machnet per-connection inflight tracking and selection
  std::vector<size_t> machnet_inflight_per_conn_;
  std::vector<size_t> machnet_per_conn_limit_;
  absl::flat_hash_map<machnet::MachnetConnection *, size_t> machnet_conn_index_;
  size_t machnet_rr_next_{0};
  // Machnet per-connection target and pacing
  std::vector<uint32_t> machnet_target_rps_per_conn_;
  std::vector<uint32_t> machnet_sent_in_window_per_conn_;

  // Machnet stress test state
  ConnectionStats machnet_stats_;
  std::mutex machnet_stats_mu_;
  std::atomic<bool> machnet_warmup_done_{false};
  std::atomic<bool> machnet_sending_enabled_{false};
  int64_t machnet_test_start_time_{0};
  int64_t machnet_test_end_time_{0};
  int64_t machnet_pacing_window_start_{0};
  std::atomic<uint32_t> machnet_sent_in_window_{0};
  std::atomic<uint64_t> machnet_inflight_count_{0};
  std::string machnet_input_buffer_;

  // Test parameters (set when test starts)
  int func_id_;
  int method_id_;
  int input_size_;
  int target_rps_;
  int inflight_limit_;

  std::vector<std::unique_ptr<EngineConnection>> connections_;
  std::atomic<bool> should_stop_;
  bool test_started_;
  int64_t start_time_;
  int64_t end_time_;
  std::atomic<uint32_t> next_call_id_;

  void StartInternal() override {
    if (use_machnet_) {
      CHECK(!machnet_ip_.empty())
          << "machnet_ip must be set when use_machnet=true";

      // Initialize Machnet channel
      auto *machnet_channel = machnet::MachnetChannel::Get();
      CHECK(machnet_channel->Init()) << "Failed to initialize Machnet";

      // Create Machnet listener
      machnet_listener_ =
          machnet_channel->CreateListener(machnet_ip_, listen_port_);
      CHECK(machnet_listener_ != nullptr)
          << "Failed to create Machnet listener";

      machnet_listener_->SetNewConnectionCallback(
          [this](machnet::MachnetConnection *conn) {
            OnNewMachnetConnection(conn);
          });

      // Setup aggressive polling callback (runs before every event loop iteration)
      UV_CHECK_OK(uv_prepare_init(uv_loop(), &machnet_poll_prepare_));
      machnet_poll_prepare_.data = this;
      UV_CHECK_OK(
          uv_prepare_start(&machnet_poll_prepare_, &MachnetPollCallback));

      // Setup idle callback for sending requests (runs every event loop iteration)
      UV_CHECK_OK(uv_idle_init(uv_loop(), &machnet_send_idle_));
      machnet_send_idle_.data = this;
      UV_CHECK_OK(uv_idle_start(&machnet_send_idle_, &MachnetSendIdleCallback));

      LOG(INFO) << "Listening on Machnet " << machnet_ip_ << ":"
                << listen_port_ << " for Engine connections";
    } else {
      // TCP mode
      struct sockaddr_in bind_addr;
      UV_CHECK_OK(uv_tcp_init(uv_loop(), &listen_handle_));
      listen_handle_.data = this;

      UV_CHECK_OK(uv_ip4_addr(listen_addr_.c_str(), listen_port_, &bind_addr));
      UV_CHECK_OK(
          uv_tcp_bind(&listen_handle_, (const struct sockaddr *)&bind_addr, 0));

      UV_CHECK_OK(uv_listen(UV_AS_STREAM(&listen_handle_), listen_backlog_,
                            OnNewConnection));
    }
  }

  void StopInternal() override {
    should_stop_.store(true);
    machnet_sending_enabled_.store(false);
    // Close listener and all active connections so the loop can terminate
    if (use_machnet_) {
      uv_prepare_stop(&machnet_poll_prepare_);
      uv_idle_stop(&machnet_send_idle_);
      uv_close(UV_AS_HANDLE(&machnet_poll_prepare_), nullptr);
      uv_close(UV_AS_HANDLE(&machnet_send_idle_), nullptr);

      // Clear connections before destroying listener
      {
        std::lock_guard<std::mutex> lk(machnet_connections_mu_);
        machnet_connections_.clear();
      }

      machnet_listener_.reset();
    } else {
      uv_close(UV_AS_HANDLE(&listen_handle_), nullptr);
    }
    for (auto &conn : connections_) {
      conn->DisableSending();
      conn->Close();
    }
  }

  static void OnNewConnection(uv_stream_t *server, int status) {
    auto *self = reinterpret_cast<StressClient *>(server->data);

    if (status != 0) {
      return;
    }

    uv_tcp_t *client = new uv_tcp_t();
    UV_DCHECK_OK(uv_tcp_init(server->loop, client));

    if (uv_accept(server, UV_AS_STREAM(client)) == 0) {
      client->data = self;
      UV_DCHECK_OK(
          uv_read_start(UV_AS_STREAM(client), AllocBuffer, OnHandshake));
    } else {
      uv_close(UV_AS_HANDLE(client),
               [](uv_handle_t *h) { delete reinterpret_cast<uv_tcp_t *>(h); });
    }
  }

  static void AllocBuffer(uv_handle_t *handle, size_t suggested_size,
                          uv_buf_t *buf) {
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
  }

  static void OnHandshake(uv_stream_t *stream, ssize_t nread,
                          const uv_buf_t *buf) {
    auto *self = reinterpret_cast<StressClient *>(stream->data);
    std::unique_ptr<char[]> buffer_guard(buf->base);

    if (nread < 0) {
      uv_close(UV_AS_HANDLE(stream),
               [](uv_handle_t *h) { delete reinterpret_cast<uv_tcp_t *>(h); });
      return;
    }

    if (nread < static_cast<ssize_t>(sizeof(GatewayMessage))) {
      return; // Wait for more data
    }

    const GatewayMessage *message =
        reinterpret_cast<const GatewayMessage *>(buf->base);
    if (!IsEngineHandshakeMessage(*message)) {
      uv_close(UV_AS_HANDLE(stream),
               [](uv_handle_t *h) { delete reinterpret_cast<uv_tcp_t *>(h); });
      return;
    }

    UV_DCHECK_OK(uv_read_stop(stream));

    uint16_t node_id = message->node_id;
    uint16_t conn_id = message->conn_id;

    // Extract OS fd from the accepted handle and duplicate it for a new loop
    uv_os_fd_t os_fd;
    UV_DCHECK_OK(
        uv_fileno(reinterpret_cast<const uv_handle_t *>(stream), &os_fd));
    int dup_fd = dup(static_cast<int>(os_fd));
    if (dup_fd < 0) {
      uv_close(UV_AS_HANDLE(stream),
               [](uv_handle_t *h) { delete reinterpret_cast<uv_tcp_t *>(h); });
      return;
    }
    // Ensure non-blocking
    int flags = fcntl(dup_fd, F_GETFL, 0);
    if (flags >= 0) {
      fcntl(dup_fd, F_SETFL, flags | O_NONBLOCK);
    }

    // Close and delete the temporary handle on the acceptor loop
    uv_close(UV_AS_HANDLE(stream),
             [](uv_handle_t *h) { delete reinterpret_cast<uv_tcp_t *>(h); });

    // Create connection with test parameters and start its own loop/thread
    auto connection = std::make_unique<EngineConnection>(
        &self->next_call_id_, node_id, conn_id, self->func_id_,
        self->method_id_, self->input_size_, self->target_rps_,
        self->inflight_limit_);

    connection->StartFromFd(dup_fd);

    self->connections_.push_back(std::move(connection));
  }

  void PrintProgress(int elapsed_sec) {
    ConnectionStats merged = MergeStats();

    LOG(INFO) << fmt::format(
        "[T+{}s] Sent: {} | Completed: {} | Failed: {} | Latencies: {}",
        elapsed_sec, merged.sent_count, merged.completed_count,
        merged.failed_count, merged.latencies_us.size());
  }

  void PrintFinalResults() {
    ConnectionStats merged = MergeStats();

    double duration_sec = (end_time_ - start_time_) / 1e6;
    double throughput = merged.completed_count / duration_sec;

    LOG(INFO) << "========================================";
    LOG(INFO) << "=== Stress Test Results ===";
    LOG(INFO) << "========================================";
    LOG(INFO) << fmt::format("Connections:       {}", connections_.size());
    LOG(INFO) << fmt::format("Duration:          {:.2f} seconds", duration_sec);
    LOG(INFO) << fmt::format("Total Sent:        {}", merged.sent_count);
    LOG(INFO) << fmt::format("Total Completed:   {} ({:.2f}%)",
                             merged.completed_count,
                             100.0 * merged.completed_count /
                                 std::max<uint64_t>(1, merged.sent_count));
    LOG(INFO) << fmt::format(
        "Total Failed:      {} ({:.2f}%)", merged.failed_count,
        100.0 * merged.failed_count / std::max<uint64_t>(1, merged.sent_count));
    LOG(INFO) << "";
    LOG(INFO) << fmt::format("Throughput:        {:.1f} rps", throughput);
    LOG(INFO) << "";

    if (!merged.latencies_us.empty()) {
      std::sort(merged.latencies_us.begin(), merged.latencies_us.end());

      size_t n = merged.latencies_us.size();
      int64_t min_lat = merged.latencies_us[0];
      int64_t p50_lat = merged.latencies_us[n * 50 / 100];
      int64_t p90_lat = merged.latencies_us[n * 90 / 100];
      int64_t p99_lat = merged.latencies_us[n * 99 / 100];
      int64_t p999_lat = merged.latencies_us[n * 999 / 1000];
      int64_t max_lat = merged.latencies_us[n - 1];

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

  void RunMachnetStressTest(int warmup_sec, int duration_sec,
                             int report_interval) {
    // Warmup phase
    if (warmup_sec > 0) {
      LOG(INFO) << "Warmup phase: " << warmup_sec << " seconds...";
      std::this_thread::sleep_for(std::chrono::seconds(warmup_sec));
      machnet_warmup_done_.store(true);
      LOG(INFO) << "Warmup complete. Starting measurement...";
      LOG(INFO) << "";
    } else {
      machnet_warmup_done_.store(true);
    }

    // Initialize timing for the idle callback
    start_time_ = GetMonotonicMicroTimestamp();
    machnet_test_start_time_ = start_time_;
    machnet_test_end_time_ = start_time_ + duration_sec * 1000000LL;
    machnet_pacing_window_start_ = start_time_;
    machnet_sent_in_window_.store(0);

    // Prepare reusable input buffer for Machnet sends
    machnet_input_buffer_.assign(input_size_, 'x');

    // Enable sending (idle callback will start sending)
    machnet_sending_enabled_.store(true, std::memory_order_release);

    // Test phase with periodic reporting
    int64_t next_report_time = start_time_ + report_interval * 1000000LL;

    while (GetMonotonicMicroTimestamp() < machnet_test_end_time_ &&
           !should_stop_.load()) {
      int64_t now = GetMonotonicMicroTimestamp();

      // Periodic reporting
      if (now >= next_report_time) {
        int elapsed_sec = (now - start_time_) / 1000000;
        PrintMachnetProgress(elapsed_sec);
        next_report_time =
            start_time_ + (elapsed_sec + report_interval) * 1000000LL;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    end_time_ = GetMonotonicMicroTimestamp();

    // Disable sending
    machnet_sending_enabled_.store(false, std::memory_order_release);

    // Wait a bit for inflight requests to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Print final results
    LOG(INFO) << "";
    PrintMachnetFinalResults();
  }

  void PrintMachnetProgress(int elapsed_sec) {
    std::lock_guard<std::mutex> lk(machnet_stats_mu_);
    LOG(INFO) << fmt::format(
        "[T+{}s] Sent: {} | Completed: {} | Failed: {} | Latencies: {}",
        elapsed_sec, machnet_stats_.sent_count, machnet_stats_.completed_count,
        machnet_stats_.failed_count, machnet_stats_.latencies_us.size());
  }

  void PrintMachnetFinalResults() {
    std::lock_guard<std::mutex> lk(machnet_connections_mu_);
    std::lock_guard<std::mutex> stats_lk(machnet_stats_mu_);

    double duration_sec = (end_time_ - start_time_) / 1e6;
    double throughput = machnet_stats_.completed_count / duration_sec;

    LOG(INFO) << "========================================";
    LOG(INFO) << "=== Stress Test Results ===";
    LOG(INFO) << "========================================";
    LOG(INFO) << fmt::format("Connections:       {}", machnet_connections_.size());
    LOG(INFO) << fmt::format("Duration:          {:.2f} seconds", duration_sec);
    LOG(INFO) << fmt::format("Total Sent:        {}", machnet_stats_.sent_count);
    LOG(INFO) << fmt::format(
        "Total Completed:   {} ({:.2f}%)", machnet_stats_.completed_count,
        100.0 * machnet_stats_.completed_count /
            std::max<uint64_t>(1, machnet_stats_.sent_count));
    LOG(INFO) << fmt::format(
        "Total Failed:      {} ({:.2f}%)", machnet_stats_.failed_count,
        100.0 * machnet_stats_.failed_count /
            std::max<uint64_t>(1, machnet_stats_.sent_count));
    LOG(INFO) << "";
    LOG(INFO) << fmt::format("Throughput:        {:.1f} rps", throughput);
    LOG(INFO) << "";

    if (!machnet_stats_.latencies_us.empty()) {
      std::sort(machnet_stats_.latencies_us.begin(),
               machnet_stats_.latencies_us.end());

      size_t n = machnet_stats_.latencies_us.size();
      int64_t min_lat = machnet_stats_.latencies_us[0];
      int64_t p50_lat = machnet_stats_.latencies_us[n * 50 / 100];
      int64_t p90_lat = machnet_stats_.latencies_us[n * 90 / 100];
      int64_t p99_lat = machnet_stats_.latencies_us[n * 99 / 100];
      int64_t p999_lat = machnet_stats_.latencies_us[n * 999 / 1000];
      int64_t max_lat = machnet_stats_.latencies_us[n - 1];

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

  ConnectionStats MergeStats() {
    ConnectionStats merged;
    for (const auto &conn : connections_) {
      const ConnectionStats &stats = conn->stats();
      merged.sent_count += stats.sent_count;
      merged.completed_count += stats.completed_count;
      merged.failed_count += stats.failed_count;
      merged.latencies_us.insert(merged.latencies_us.end(),
                                 stats.latencies_us.begin(),
                                 stats.latencies_us.end());
    }
    return merged;
  }

  // Machnet support methods
  void OnNewMachnetConnection(machnet::MachnetConnection *connection) {
    LOG(INFO) << "New Machnet Engine connection established";

    // Set message callback to handle incoming messages
    connection->SetMessageCallback(
        [this, connection](const GatewayMessage &msg,
                           std::span<const char> payload) {
          OnRecvMachnetEngineMessage(connection, msg, payload);
        });
  }

  void OnRecvMachnetEngineMessage(machnet::MachnetConnection *connection,
                                   const GatewayMessage &message,
                                   std::span<const char> payload) {
    // Handle handshake - just track the connection
    if (IsEngineHandshakeMessage(message)) {
      LOG(INFO) << "Machnet Engine handshake received from node_id="
                << message.node_id;
      std::lock_guard<std::mutex> lk(machnet_connections_mu_);
      machnet_connections_.push_back(connection);
      size_t idx = machnet_connections_.size() - 1;
      machnet_conn_index_[connection] = idx;
      if (machnet_inflight_per_conn_.size() < machnet_connections_.size()) {
        machnet_inflight_per_conn_.resize(machnet_connections_.size(), 0);
      }
      // Recompute per-connection inflight limits (per-connection semantics)
      machnet_per_conn_limit_.resize(machnet_connections_.size(), 0);
      size_t n = machnet_connections_.size();
      if (inflight_limit_ > 0) {
        for (size_t i = 0; i < n; i++) {
          machnet_per_conn_limit_[i] = static_cast<size_t>(inflight_limit_);
        }
      } else {
        // Safe default to avoid Machnet SHM/ring overflow under unlimited mode
        constexpr size_t kDefaultPerConnInflight = 256;
        for (size_t i = 0; i < n; i++) {
          machnet_per_conn_limit_[i] = kDefaultPerConnInflight;
        }
      }

      // Recompute per-connection target RPS distribution similar to TCP mode
      machnet_target_rps_per_conn_.resize(n, 0);
      machnet_sent_in_window_per_conn_.resize(n, 0);
      if (target_rps_ > 0) {
        uint32_t base = static_cast<uint32_t>(target_rps_ / static_cast<int>(n));
        uint32_t rem = static_cast<uint32_t>(target_rps_ % static_cast<int>(n));
        for (size_t i = 0; i < n; i++) {
          machnet_target_rps_per_conn_[i] = base + (static_cast<uint32_t>(i) < rem ? 1U : 0U);
        }
      } else {
        for (size_t i = 0; i < n; i++) {
          machnet_target_rps_per_conn_[i] = 0;  // unlimited per-connection
        }
      }
      return;
    }

    // Handle function call responses
    if (IsFuncCallCompleteMessage(message) ||
        IsFuncCallFailedMessage(message)) {
      FuncCall func_call = GetFuncCallFromMessage(message);
      int64_t recv_time = GetMonotonicMicroTimestamp();

      EngineConnection *origin = nullptr;
      int64_t send_time = 0;
      uint64_t recv_index = 0;
      if (!ConsumeInflight(func_call.full_call_id, &origin, &send_time,
                           &recv_index)) {
        return; // Unknown response
      }

      // Decrement inflight counts (global and per-connection)
      machnet_inflight_count_.fetch_sub(1, std::memory_order_acq_rel);
      {
        std::lock_guard<std::mutex> lk(machnet_connections_mu_);
        auto it_idx = machnet_conn_index_.find(connection);
        if (it_idx != machnet_conn_index_.end()) {
          size_t cidx = it_idx->second;
          if (cidx < machnet_inflight_per_conn_.size() && machnet_inflight_per_conn_[cidx] > 0) {
            machnet_inflight_per_conn_[cidx]--;
          }
        }
      }

      // Track stats for Machnet
      std::lock_guard<std::mutex> lk(machnet_stats_mu_);
      if (IsFuncCallCompleteMessage(message)) {
        machnet_stats_.completed_count++;
        if (machnet_warmup_done_.load(std::memory_order_acquire)) {
          int64_t latency_us = recv_time - send_time;
          machnet_stats_.latencies_us.push_back(latency_us);
        }
      } else if (IsFuncCallFailedMessage(message)) {
        machnet_stats_.failed_count++;
      }
    }
  }

  static void MachnetPollCallback(uv_prepare_t *handle) {
    // Poll aggressively before each event loop iteration
    machnet::MachnetChannel::Get()->Poll();
  }

  static void MachnetSendIdleCallback(uv_idle_t *handle) {
    auto *self = reinterpret_cast<StressClient *>(handle->data);

    // Fast path: check if sending is enabled (no lock needed)
    if (!self->machnet_sending_enabled_.load(std::memory_order_acquire)) {
      return;
    }

    int64_t now = GetMonotonicMicroTimestamp();

    // Check if test time is over
    if (now >= self->machnet_test_end_time_) {
      return;
    }

    // Token-bucket style pacing: reset window if needed
    int64_t elapsed_us = now - self->machnet_pacing_window_start_;
    if (elapsed_us >= 1000000) {
      self->machnet_pacing_window_start_ = now;
      self->machnet_sent_in_window_.store(0, std::memory_order_release);
      elapsed_us = 0;
    }

    // Check global inflight limit (fast check without lock)
    uint64_t current_inflight = self->machnet_inflight_count_.load(std::memory_order_acquire);

    // Now take the lock ONCE and do all the work
    std::lock_guard<std::mutex> lk(self->machnet_connections_mu_);

    if (self->machnet_connections_.empty()) {
      return;
    }

    size_t num_conns = self->machnet_connections_.size();

    // Calculate total allowed inflight
    size_t total_allowed_inflight = 0;
    if (self->inflight_limit_ > 0) {
      total_allowed_inflight = static_cast<size_t>(self->inflight_limit_) * num_conns;
    } else {
      // Unlimited mode: use default per-connection limit
      total_allowed_inflight = num_conns * 256;
    }

    if (current_inflight >= total_allowed_inflight) {
      return;  // At capacity
    }

    // Calculate how many we can send
    size_t capacity_left = total_allowed_inflight - static_cast<size_t>(current_inflight);

    // Apply rate limiting if configured
    if (self->target_rps_ > 0) {
      uint32_t sent_so_far = self->machnet_sent_in_window_.load(std::memory_order_acquire);
      if (sent_so_far >= static_cast<uint32_t>(self->target_rps_)) {
        return;  // Rate limit reached for this window
      }

      // Calculate tokens available based on elapsed time (token bucket)
      uint64_t tokens_available = (static_cast<uint64_t>(self->target_rps_) *
                                   static_cast<uint64_t>(elapsed_us)) / 1000000ULL;
      if (tokens_available <= sent_so_far) {
        return;  // Not enough tokens yet
      }

      size_t rate_capacity = tokens_available - sent_so_far;
      capacity_left = std::min(capacity_left, rate_capacity);
    }

    // Batch send: send up to capacity_left requests
    // Use round-robin across connections for load balancing
    size_t sent_count = 0;
    size_t rr_start = self->machnet_rr_next_;

    for (size_t i = 0; i < capacity_left; i++) {
      size_t conn_idx = (rr_start + i) % num_conns;
      machnet::MachnetConnection *conn = self->machnet_connections_[conn_idx];

      // Check per-connection inflight limit
      size_t per_conn_inflight = (conn_idx < self->machnet_inflight_per_conn_.size())
                                   ? self->machnet_inflight_per_conn_[conn_idx] : 0;
      size_t per_conn_limit = (conn_idx < self->machnet_per_conn_limit_.size())
                                ? self->machnet_per_conn_limit_[conn_idx] : 256;

      if (per_conn_inflight >= per_conn_limit) {
        continue;  // This connection is saturated, try next
      }

      // Create and send the request
      uint16_t client_id = 0;
      uint32_t call_id = self->next_call_id_.fetch_add(1, std::memory_order_relaxed);
      protocol::FuncCall func_call = protocol::NewFuncCall(self->func_id_, client_id, call_id);
      if (self->method_id_ > 0) {
        func_call.method_id = self->method_id_;
      }

      protocol::GatewayMessage message = protocol::NewDispatchFuncCallGatewayMessage(func_call);
      message.payload_size = self->input_size_;

      // Send via Machnet
      bool success = conn->SendMessage(
          message,
          std::span<const char>(self->machnet_input_buffer_.data(),
                                self->machnet_input_buffer_.size()));

      if (success) {
        // Track inflight
        static std::unique_ptr<EngineConnection> stats_tracker;
        if (!stats_tracker) {
          stats_tracker = std::make_unique<EngineConnection>(
              &self->next_call_id_, 0, 0, self->func_id_, self->method_id_,
              self->input_size_, self->target_rps_, self->inflight_limit_);
        }

        RegisterInflight(func_call.full_call_id, stats_tracker.get(), now, 0);

        // Update counters (inflight tracking)
        self->machnet_inflight_count_.fetch_add(1, std::memory_order_acq_rel);
        if (conn_idx < self->machnet_inflight_per_conn_.size()) {
          self->machnet_inflight_per_conn_[conn_idx]++;
        }

        // Update rate limiting counter
        self->machnet_sent_in_window_.fetch_add(1, std::memory_order_acq_rel);
        sent_count++;
      }
    }

    // Advance round-robin pointer
    if (sent_count > 0) {
      self->machnet_rr_next_ = (rr_start + sent_count) % num_conns;

      // Update stats (batch update at the end)
      std::lock_guard<std::mutex> stats_lk(self->machnet_stats_mu_);
      self->machnet_stats_.sent_count += sent_count;
    }
  }
};

} // namespace stress
} // namespace faas

static std::atomic<faas::stress::StressClient *> g_stress_client(nullptr);

void SignalHandler(int signal) {
  faas::stress::StressClient *client = g_stress_client.exchange(nullptr);
  if (client != nullptr) {
    LOG(INFO) << "Received signal, stopping...";
    client->ScheduleStop();
  }
}

int main(int argc, char *argv[]) {
  signal(SIGINT, SignalHandler);
  faas::base::InitMain(argc, argv);

  auto client = std::make_unique<faas::stress::StressClient>();
  client->set_listen_addr(absl::GetFlag(FLAGS_listen_addr));
  client->set_listen_port(absl::GetFlag(FLAGS_listen_port));
  client->set_use_machnet(absl::GetFlag(FLAGS_use_machnet));
  client->set_machnet_ip(absl::GetFlag(FLAGS_machnet_ip));

  g_stress_client.store(client.get());

  if (absl::GetFlag(FLAGS_use_machnet)) {
    LOG(INFO) << "Listening on Machnet " << absl::GetFlag(FLAGS_machnet_ip)
              << ":" << absl::GetFlag(FLAGS_listen_port)
              << " for Engine connections";
  } else {
    LOG(INFO) << "Listening on " << absl::GetFlag(FLAGS_listen_addr) << ":"
              << absl::GetFlag(FLAGS_listen_port)
              << " for Engine connections";
  }

  client->Start();

  // Run stress test directly in main thread
  std::this_thread::sleep_for(
      std::chrono::milliseconds(500)); // Let server start
  client->RunStressTest();
  client->ScheduleStop();

  client->WaitForFinish();

  return 0;
}
