#pragma once

#include "stress_transport.h"
#include "common/protocol.h"
#include "common/time.h"
#include "common/uv.h"
#include "utils/appendable_buffer.h"
#include <absl/container/flat_hash_map.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace faas {
namespace stress {

class EngineConnection {
public:
  EngineConnection(std::atomic<uint32_t>* call_id_alloc, uint16_t node_id,
                   uint16_t conn_id, int func_id, int method_id, int input_size,
                   int target_rps, int inflight_limit);
  ~EngineConnection();

  void StartFromFd(int fd);
  void EnableSending();
  void DisableSending();
  void SetTargetRps(int target_rps);
  void SetWarmupDone();
  void Close();

  uint16_t node_id() const { return node_id_; }
  uint16_t conn_id() const { return conn_id_; }
  const ConnectionStats& stats() const { return stats_; }
  ConnectionStats SnapshotStats() const;

  void OnInflightCompletedFromAnyThread(bool success, int64_t send_time,
                                        uint64_t index, int64_t recv_time);

private:
  enum State { kCreated, kRunning, kClosed };

  std::atomic<uint32_t>* call_id_alloc_;
  uint16_t node_id_;
  uint16_t conn_id_;
  int func_id_;
  int method_id_;
  int input_size_;
  int target_rps_;
  int inflight_limit_;

  State state_;
  uv_loop_t* loop_;
  uv_tcp_t handle_;
  utils::AppendableBuffer read_buffer_;

  std::atomic<bool> warmup_done_;
  std::atomic<bool> test_enabled_;
  std::atomic<bool> should_stop_;
  std::string input_buffer_;

  ConnectionStats stats_;
  absl::flat_hash_map<uint64_t, int64_t> inflight_requests_;
  absl::flat_hash_map<uint64_t, uint64_t> inflight_indices_;

  std::mutex stats_mu_;
  std::atomic<size_t> inflight_count_{0};

  std::thread thread_;
  int owned_fd_ = -1;
  int64_t pacing_window_start_us_ = 0;
  uint32_t sent_in_window_ = 0;
  uint64_t sent_message_index_ = 0;

  void ThreadMain();
  void SendRequest(int64_t send_time);
  void SendMessage(const protocol::GatewayMessage& message,
                   std::span<const char> payload);
  void ProcessMessages();
  void HandleResponse(const protocol::GatewayMessage& message,
                      std::span<const char> payload);

  static void AllocBuffer(uv_handle_t* handle, size_t suggested_size,
                          uv_buf_t* buf);
  static void OnRecvData(uv_stream_t* stream, ssize_t nread,
                         const uv_buf_t* buf);
  static void OnSendComplete(uv_write_t* req, int status);
};

class TcpStressTransport : public IStressTransport {
public:
  TcpStressTransport(const std::string& listen_addr, int listen_port,
                     std::atomic<uint32_t>* call_id_alloc,
                     const TestConfig& config);
  ~TcpStressTransport() override;

  void Start() override;
  void Stop() override;
  void EnableSending() override;
  void DisableSending() override;
  void SetWarmupDone() override;
  ConnectionStats GetStats() const override;
  size_t GetConnectionCount() const override;
  bool HasConnections() const override;
  bool ShouldStop() const override { return should_stop_.load(); }

  // Internal methods for libuv callbacks
  void OnNewConnection(uv_stream_t* server, int status);
  void OnHandshake(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);

  uv_loop_t* uv_loop() { return &loop_; }

private:
  std::string listen_addr_;
  int listen_port_;
  int listen_backlog_;
  std::atomic<uint32_t>* call_id_alloc_;
  TestConfig config_;

  uv_loop_t loop_;
  uv_tcp_t listen_handle_;

  std::vector<std::unique_ptr<EngineConnection>> connections_;
  std::atomic<bool> should_stop_;

  static void OnNewConnectionStatic(uv_stream_t* server, int status);
  static void AllocBuffer(uv_handle_t* handle, size_t suggested_size,
                          uv_buf_t* buf);
  static void OnHandshakeStatic(uv_stream_t* stream, ssize_t nread,
                                const uv_buf_t* buf);
};

} // namespace stress
} // namespace faas
