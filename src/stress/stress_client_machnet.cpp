#include "stress_client_machnet.h"
#include "base/logging.h"
#include "common/protocol.h"
#include "common/time.h"
#include "common/uv.h"
#include <algorithm>
#include <chrono>
#include <thread>

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

MachnetStressTransport::MachnetStressTransport(
    const std::string& machnet_ip, int listen_port,
    std::atomic<uint32_t>* call_id_alloc, const TestConfig& config)
    : machnet_ip_(machnet_ip), listen_port_(listen_port),
      call_id_alloc_(call_id_alloc), config_(config) {
  // Prepare reusable input buffer for Machnet sends
  input_buffer_.assign(config_.input_size, 'x');
}

MachnetStressTransport::~MachnetStressTransport() { Stop(); }

void MachnetStressTransport::Start() {
  CHECK(!machnet_ip_.empty()) << "machnet_ip must be set";
  // Start single event-loop thread which initializes Machnet, accepts handshake,
  // generates load and polls for responses.
  LOG(INFO) << "Starting Machnet event-loop thread...";
  loop_thread_ = std::thread(&MachnetStressTransport::LoopThreadMain, this);
}

void MachnetStressTransport::Stop() {
  should_stop_.store(true);
  sending_enabled_.store(false);

  // Join event-loop thread if it exists
  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }

  machnet_single_conn_.store(nullptr, std::memory_order_relaxed);
  machnet_listener_.reset();
}

void MachnetStressTransport::EnableSending() {
  // Initialize timing and scheduling for open-loop sending
  CHECK_GT(config_.target_rps, 0);

  test_start_time_ = uv_hrtime() / 1000; // convert ns -> us
  test_end_time_ = test_start_time_ + config_.duration_sec * 1000000LL;
  inter_send_us_ = std::max<uint64_t>(1, 1000000LL / config_.target_rps);
  next_send_time_ = test_start_time_;

  // Preallocate tracking for request timestamps and latencies
  base_call_id_start_ = call_id_alloc_->load(std::memory_order_relaxed);
  size_t expected_requests =
      static_cast<size_t>(config_.duration_sec) * static_cast<size_t>(config_.target_rps);
  send_ts_by_index_.assign(expected_requests, 0);
  {
    std::lock_guard<std::mutex> lk(latencies_mu_);
    latencies_us_.reserve(expected_requests);
  }

  // Enable sending (event-loop thread will handle send/poll)
  sending_enabled_.store(true, std::memory_order_release);
}

void MachnetStressTransport::DisableSending() {
  sending_enabled_.store(false, std::memory_order_release);
}

void MachnetStressTransport::SetWarmupDone() {
  warmup_done_.store(true, std::memory_order_release);
}

ConnectionStats MachnetStressTransport::GetStats() const {
  ConnectionStats stats;
  stats.sent_count = sent_count_.load(std::memory_order_relaxed);
  stats.completed_count = completed_count_.load(std::memory_order_relaxed);
  stats.failed_count = failed_count_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(
        const_cast<MachnetStressTransport*>(this)->latencies_mu_);
    stats.latencies_us = latencies_us_;
  }
  return stats;
}

size_t MachnetStressTransport::GetConnectionCount() const {
  return machnet_single_conn_.load(std::memory_order_acquire) != nullptr ? 1 : 0;
}

bool MachnetStressTransport::HasConnections() const {
  return machnet_single_conn_.load(std::memory_order_acquire) != nullptr;
}

void MachnetStressTransport::OnNewMachnetConnection(
    machnet::MachnetConnection* connection) {
  // std::cerr<<fmt::format("[Receive Thread {}] New Machnet connection established", std::this_thread::get_id())<<std::endl;

  connection->SetMessageCallback(
      [this, connection](const GatewayMessage& msg,
                         std::span<const char> payload) {
        OnRecvMachnetEngineMessage(connection, msg, payload);
      });
}

void MachnetStressTransport::OnRecvMachnetEngineMessage(
    machnet::MachnetConnection* connection, const GatewayMessage& message,
    std::span<const char> payload) {
  // Handle handshake - single connection
  if (IsEngineHandshakeMessage(message)) {
    // Pin the sending connection on first handshake; ignore subsequent ones
    machnet::MachnetConnection* expected = nullptr;
    if (machnet_single_conn_.compare_exchange_strong(
            expected, connection, std::memory_order_acq_rel)) {
      const auto& f = connection->flow();
      std::cerr << "[Receive Thread " << std::this_thread::get_id()
                << "] Handshake: pinned send flow "
                << f.src_ip << ":" << f.src_port << " -> "
                << f.dst_ip << ":" << f.dst_port << std::endl;
    } else {
      const auto& f_new = connection->flow();
      const auto& f_old = expected->flow();
      std::cerr << "[Receive Thread " << std::this_thread::get_id()
                << "] Handshake: ignoring new flow "
                << f_new.src_ip << ":" << f_new.src_port << " -> "
                << f_new.dst_ip << ":" << f_new.dst_port
                << "; already pinned to "
                << f_old.src_ip << ":" << f_old.src_port << " -> "
                << f_old.dst_ip << ":" << f_old.dst_port << std::endl;
    }
    return;
  }

  // Handle function call responses
  if (IsFuncCallCompleteMessage(message) || IsFuncCallFailedMessage(message)) {
    FuncCall func_call = GetFuncCallFromMessage(message);
    int64_t recv_time = GetMonotonicMicroTimestamp();

    size_t idx = static_cast<size_t>(func_call.call_id - base_call_id_start_);
    int64_t send_time = 0;
    if (idx < send_ts_by_index_.size()) {
      send_time = send_ts_by_index_[idx];
    }

    // Track stats for Machnet using lock-free atomics
    if (IsFuncCallCompleteMessage(message)) {
      completed_count_.fetch_add(1, std::memory_order_relaxed);
      if (warmup_done_.load(std::memory_order_acquire) && send_time > 0) {
        int64_t latency_us = recv_time - send_time;
        // Only lock for latencies vector append
        std::lock_guard<std::mutex> lk(latencies_mu_);
        latencies_us_.push_back(latency_us);
      }
    } else if (IsFuncCallFailedMessage(message)) {
      failed_count_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void MachnetStressTransport::LoopThreadMain() {
  // Initialize Machnet and create listener on this thread
  std::cerr << "[Loop Thread " << std::this_thread::get_id()
            << "] Machnet event-loop thread started" << std::endl;
  auto* chan = machnet::MachnetChannel::Get();
  if (!chan->Init()) {
    std::cerr << "[Loop Thread " << std::this_thread::get_id()
              << "] Failed to initialize Machnet" << std::endl;
    return;
  }

  machnet_listener_ = chan->CreateListener(machnet_ip_, listen_port_);
  if (machnet_listener_ == nullptr) {
    std::cerr << "[Loop Thread " << std::this_thread::get_id()
              << "] Failed to create Machnet listener" << std::endl;
    return;
  }
  std::cerr << "Listening on Machnet " << machnet_ip_ << ":" << listen_port_
            << " for Engine connections";
  std::cerr << "[Loop Thread " << std::this_thread::get_id()
            << "] Machnet listener created" << std::endl;

  // Enforce single-connection behavior: after first flow, ignore any new ones
  machnet_listener_->SetSingleConnectionOnly(true);
  machnet_listener_->SetNewConnectionCallback(
      [this](machnet::MachnetConnection* conn) { OnNewMachnetConnection(conn); });

  machnet::MachnetConnection* last_conn = nullptr;

  while (!should_stop_.load(std::memory_order_acquire)) {
    // Always poll first to prioritize handshake and response processing
    chan->Poll();

    // Wait for handshake to pin the single connection
    machnet::MachnetConnection* conn =
        machnet_single_conn_.load(std::memory_order_acquire);
    if (conn == nullptr) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      continue;
    }

    if (conn != last_conn) {
      const auto& f = conn->flow();
      std::cerr << "[Loop Thread " << std::this_thread::get_id()
                << "] Now sending on flow "
                << f.src_ip << ":" << f.src_port << " -> "
                << f.dst_ip << ":" << f.dst_port << std::endl;
      last_conn = conn;
    }

    if (!sending_enabled_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      continue;
    }

    uint64_t now = uv_hrtime() / 1000; // convert ns -> us
    if (now >= test_end_time_) {
      sending_enabled_.store(false, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    if (now < next_send_time_) {
      chan->Poll();
      continue;
    }

    uint16_t client_id = 0;
    uint32_t call_id = call_id_alloc_->fetch_add(1, std::memory_order_relaxed);
    FuncCall func_call = NewFuncCall(config_.func_id, client_id, call_id);
    if (config_.method_id > 0) {
      func_call.method_id = config_.method_id;
    }
    GatewayMessage message = NewDispatchFuncCallGatewayMessage(func_call);

    size_t idx = static_cast<size_t>(call_id - base_call_id_start_);
    if (idx < send_ts_by_index_.size()) {
      send_ts_by_index_[idx] = now;
    }

    bool success = conn->SendMessage(message, {});
    if (success) {
      sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Advance schedule (open-loop pacing with catch-up)
    int64_t delta = (int64_t)now - (int64_t)next_send_time_;
    int64_t intervals = 1 + (delta >= 0 ? (delta / (int64_t)inter_send_us_) : 0);
    if (intervals < 1) intervals = 1;
    next_send_time_ += (uint64_t)intervals * inter_send_us_;

    chan->Poll();
  }

  std::cerr << "[Loop Thread " << std::this_thread::get_id()
            << "] Machnet event-loop thread finished" << std::endl;
}

// Factory function implementation
std::unique_ptr<IStressTransport> CreateMachnetTransport(
    const std::string& machnet_ip, int listen_port,
    std::atomic<uint32_t>* call_id_alloc, const TestConfig& config) {
  return std::make_unique<MachnetStressTransport>(machnet_ip, listen_port,
                                                   call_id_alloc, config);
}

} // namespace stress
} // namespace faas
