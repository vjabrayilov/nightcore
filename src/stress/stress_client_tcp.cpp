#include "stress_client_tcp.h"
#include "base/logging.h"
#include "common/protocol.h"
#include "common/time.h"
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>

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

// ========== EngineConnection Implementation ==========

EngineConnection::EngineConnection(std::atomic<uint32_t>* call_id_alloc,
                                   uint16_t node_id, uint16_t conn_id,
                                   int func_id, int method_id, int input_size,
                                   int target_rps, int inflight_limit)
    : call_id_alloc_(call_id_alloc), node_id_(node_id), conn_id_(conn_id),
      func_id_(func_id), method_id_(method_id), input_size_(input_size),
      target_rps_(target_rps), inflight_limit_(inflight_limit),
      state_(kCreated), loop_(nullptr), warmup_done_(false),
      test_enabled_(false), should_stop_(false) {
  input_buffer_.resize(input_size_, 'x');
}

EngineConnection::~EngineConnection() {
  DCHECK(state_ == kCreated || state_ == kClosed);
}

void EngineConnection::StartFromFd(int fd) {
  LOG(INFO) << "Starting EngineConnection thread";
  DCHECK(state_ == kCreated);
  owned_fd_ = fd;
  thread_ = std::thread(&EngineConnection::ThreadMain, this);
}

void EngineConnection::EnableSending() {
  test_enabled_.store(true, std::memory_order_release);
}

void EngineConnection::DisableSending() {
  test_enabled_.store(false, std::memory_order_release);
}

void EngineConnection::SetTargetRps(int target_rps) { target_rps_ = target_rps; }

void EngineConnection::SetWarmupDone() {
  warmup_done_.store(true, std::memory_order_release);
}

void EngineConnection::Close() {
  if (state_ == kClosed)
    return;
  test_enabled_.store(false, std::memory_order_release);
  should_stop_.store(true, std::memory_order_release);
  if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) {
    thread_.join();
  }
  state_ = kClosed;
}

void EngineConnection::OnInflightCompletedFromAnyThread(bool success,
                                                        int64_t send_time,
                                                        uint64_t index,
                                                        int64_t recv_time) {
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

ConnectionStats EngineConnection::SnapshotStats() const {
  std::lock_guard<std::mutex> lk(
      const_cast<EngineConnection*>(this)->stats_mu_);
  return stats_;
}

void EngineConnection::AllocBuffer(uv_handle_t* handle, size_t suggested_size,
                                   uv_buf_t* buf) {
  buf->base = new char[suggested_size];
  buf->len = suggested_size;
}

void EngineConnection::OnRecvData(uv_stream_t* stream, ssize_t nread,
                                  const uv_buf_t* buf) {
  auto* conn = reinterpret_cast<EngineConnection*>(stream->data);
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

void EngineConnection::ThreadMain() {
  uv_loop_t owned_loop;
  UV_CHECK_OK(uv_loop_init(&owned_loop));
  loop_ = &owned_loop;

  UV_CHECK_OK(uv_tcp_init(loop_, &handle_));
  handle_.data = this;
  UV_CHECK_OK(uv_tcp_open(&handle_, owned_fd_));
  UV_DCHECK_OK(uv_tcp_nodelay(&handle_, 1));
  UV_DCHECK_OK(uv_tcp_keepalive(&handle_, 1, 1));
  UV_DCHECK_OK(uv_read_start(UV_AS_STREAM(&handle_), AllocBuffer, OnRecvData));

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

void EngineConnection::SendRequest(int64_t send_time) {
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

  GlobalInflightRegistry::Instance().Register(func_call.full_call_id, this,
                                               send_time, current_index);
  inflight_count_.fetch_add(1, std::memory_order_acq_rel);
  stats_.sent_count++;
}

void EngineConnection::SendMessage(const GatewayMessage& message,
                                   std::span<const char> payload) {
  if (state_ != kRunning)
    return;

  size_t total_size = sizeof(GatewayMessage) + payload.size();
  char* buffer = new char[total_size];
  memcpy(buffer, &message, sizeof(GatewayMessage));
  if (!payload.empty()) {
    memcpy(buffer + sizeof(GatewayMessage), payload.data(), payload.size());
  }

  uv_buf_t buf = uv_buf_init(buffer, total_size);
  uv_write_t* req = new uv_write_t();
  req->data = buffer;

  int status = uv_write(req, UV_AS_STREAM(&handle_), &buf, 1, OnSendComplete);
  if (status != 0) {
    delete[] buffer;
    delete req;
  }
}

void EngineConnection::OnSendComplete(uv_write_t* req, int status) {
  auto* buffer = reinterpret_cast<char*>(req->data);
  delete[] buffer;
  delete req;
}

void EngineConnection::ProcessMessages() {
  while (state_ == kRunning &&
         read_buffer_.length() >= sizeof(GatewayMessage)) {
    GatewayMessage* message =
        reinterpret_cast<GatewayMessage*>(read_buffer_.data());
    size_t full_size =
        sizeof(GatewayMessage) + std::max<size_t>(0, message->payload_size);

    if (read_buffer_.length() >= full_size) {
      std::span<const char> payload(
          read_buffer_.data() + sizeof(GatewayMessage),
          full_size - sizeof(GatewayMessage));
      HandleResponse(*message, payload);
      read_buffer_.ConsumeFront(full_size);
    } else {
      break;
    }
  }
}

void EngineConnection::HandleResponse(const GatewayMessage& message,
                                      std::span<const char> payload) {
  if (state_ != kRunning)
    return;

  int64_t recv_time = GetMonotonicMicroTimestamp();

  if (IsFuncCallCompleteMessage(message) || IsFuncCallFailedMessage(message)) {
    FuncCall func_call = GetFuncCallFromMessage(message);

    EngineConnection* origin = nullptr;
    int64_t send_time = 0;
    uint64_t recv_index = 0;
    void* origin_ptr = nullptr;
    if (!GlobalInflightRegistry::Instance().Consume(
            func_call.full_call_id, &origin_ptr, &send_time, &recv_index)) {
      return; // Unknown response
    }
    origin = reinterpret_cast<EngineConnection*>(origin_ptr);

    if (IsFuncCallCompleteMessage(message)) {
      origin->OnInflightCompletedFromAnyThread(/*success=*/true, send_time,
                                               recv_index, recv_time);
    } else if (IsFuncCallFailedMessage(message)) {
      origin->OnInflightCompletedFromAnyThread(/*success=*/false, send_time,
                                               recv_index, recv_time);
    }
  }
}

// ========== TcpStressTransport Implementation ==========

TcpStressTransport::TcpStressTransport(const std::string& listen_addr,
                                       int listen_port,
                                       std::atomic<uint32_t>* call_id_alloc,
                                       const TestConfig& config)
    : listen_addr_(listen_addr), listen_port_(listen_port), listen_backlog_(64),
      call_id_alloc_(call_id_alloc), config_(config), should_stop_(false) {}

TcpStressTransport::~TcpStressTransport() { Stop(); }

void TcpStressTransport::Start() {
  struct sockaddr_in bind_addr;
  UV_CHECK_OK(uv_loop_init(&loop_));
  UV_CHECK_OK(uv_tcp_init(&loop_, &listen_handle_));
  listen_handle_.data = this;

  UV_CHECK_OK(uv_ip4_addr(listen_addr_.c_str(), listen_port_, &bind_addr));
  UV_CHECK_OK(
      uv_tcp_bind(&listen_handle_, (const struct sockaddr*)&bind_addr, 0));

  UV_CHECK_OK(uv_listen(UV_AS_STREAM(&listen_handle_), listen_backlog_,
                        OnNewConnectionStatic));
}

void TcpStressTransport::Stop() {
  should_stop_.store(true);

  // Close listener and all active connections
  uv_close(UV_AS_HANDLE(&listen_handle_), nullptr);
  for (auto& conn : connections_) {
    conn->DisableSending();
    conn->Close();
  }
}

void TcpStressTransport::EnableSending() {
  // Distribute global target RPS across connections
  if (!connections_.empty()) {
    if (config_.target_rps <= 0) {
      for (auto& conn : connections_) {
        conn->SetTargetRps(0); // unlimited per connection
      }
    } else {
      int base = config_.target_rps / static_cast<int>(connections_.size());
      int rem = config_.target_rps % static_cast<int>(connections_.size());
      for (size_t i = 0; i < connections_.size(); i++) {
        int per_conn = base + (static_cast<int>(i) < rem ? 1 : 0);
        connections_[i]->SetTargetRps(per_conn);
      }
      LOG(INFO) << fmt::format(
          "Per-connection target RPS: base={} (+1 for first {} conns)", base,
          rem);
    }
  }

  for (auto& conn : connections_) {
    conn->EnableSending();
  }
}

void TcpStressTransport::DisableSending() {
  for (auto& conn : connections_) {
    conn->DisableSending();
  }
}

void TcpStressTransport::SetWarmupDone() {
  for (auto& conn : connections_) {
    conn->SetWarmupDone();
  }
}

ConnectionStats TcpStressTransport::GetStats() const {
  std::vector<ConnectionStats> stats_list;
  for (const auto& conn : connections_) {
    stats_list.push_back(conn->SnapshotStats());
  }
  return MergeStats(stats_list);
}

size_t TcpStressTransport::GetConnectionCount() const {
  return connections_.size();
}

bool TcpStressTransport::HasConnections() const {
  return !connections_.empty();
}

void TcpStressTransport::OnNewConnectionStatic(uv_stream_t* server,
                                               int status) {
  auto* self = reinterpret_cast<TcpStressTransport*>(server->data);
  self->OnNewConnection(server, status);
}

void TcpStressTransport::OnNewConnection(uv_stream_t* server, int status) {
  if (status != 0) {
    return;
  }

  uv_tcp_t* client = new uv_tcp_t();
  UV_DCHECK_OK(uv_tcp_init(server->loop, client));

  if (uv_accept(server, UV_AS_STREAM(client)) == 0) {
    client->data = this;
    UV_DCHECK_OK(
        uv_read_start(UV_AS_STREAM(client), AllocBuffer, OnHandshakeStatic));
  } else {
    uv_close(UV_AS_HANDLE(client),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
  }
}

void TcpStressTransport::AllocBuffer(uv_handle_t* handle, size_t suggested_size,
                                     uv_buf_t* buf) {
  buf->base = new char[suggested_size];
  buf->len = suggested_size;
}

void TcpStressTransport::OnHandshakeStatic(uv_stream_t* stream, ssize_t nread,
                                           const uv_buf_t* buf) {
  auto* self = reinterpret_cast<TcpStressTransport*>(stream->data);
  self->OnHandshake(stream, nread, buf);
}

void TcpStressTransport::OnHandshake(uv_stream_t* stream, ssize_t nread,
                                     const uv_buf_t* buf) {
  std::unique_ptr<char[]> buffer_guard(buf->base);

  if (nread < 0) {
    uv_close(UV_AS_HANDLE(stream),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
    return;
  }

  if (nread < static_cast<ssize_t>(sizeof(GatewayMessage))) {
    return; // Wait for more data
  }

  const GatewayMessage* message =
      reinterpret_cast<const GatewayMessage*>(buf->base);
  if (!IsEngineHandshakeMessage(*message)) {
    uv_close(UV_AS_HANDLE(stream),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
    return;
  }

  UV_DCHECK_OK(uv_read_stop(stream));

  uint16_t node_id = message->node_id;
  uint16_t conn_id = message->conn_id;

  // Extract OS fd from the accepted handle and duplicate it for a new loop
  uv_os_fd_t os_fd;
  UV_DCHECK_OK(uv_fileno(reinterpret_cast<const uv_handle_t*>(stream), &os_fd));
  int dup_fd = dup(static_cast<int>(os_fd));
  if (dup_fd < 0) {
    uv_close(UV_AS_HANDLE(stream),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
    return;
  }
  // Ensure non-blocking
  int flags = fcntl(dup_fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(dup_fd, F_SETFL, flags | O_NONBLOCK);
  }

  // Close and delete the temporary handle on the acceptor loop
  uv_close(UV_AS_HANDLE(stream),
           [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });

  // Create connection with test parameters and start its own loop/thread
  auto connection = std::make_unique<EngineConnection>(
      call_id_alloc_, node_id, conn_id, config_.func_id, config_.method_id,
      config_.input_size, config_.target_rps, config_.inflight_limit);

  connection->StartFromFd(dup_fd);

  connections_.push_back(std::move(connection));
}

// Factory function implementation
std::unique_ptr<IStressTransport> CreateTcpTransport(
    const std::string& listen_addr, int listen_port,
    std::atomic<uint32_t>* call_id_alloc, const TestConfig& config) {
  return std::make_unique<TcpStressTransport>(listen_addr, listen_port,
                                               call_id_alloc, config);
}

} // namespace stress
} // namespace faas
