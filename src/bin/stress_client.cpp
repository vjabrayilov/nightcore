#include "base/init.h"
#include "base/common.h"
#include "base/thread.h"
#include "common/time.h"
#include "common/protocol.h"
#include "common/uv.h"
#include "utils/socket.h"
#include "utils/io.h"

#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <memory>
#include <algorithm>
#include <absl/flags/flag.h>
#include <fmt/format.h>

ABSL_FLAG(std::string, listen_addr, "0.0.0.0", "Address to listen for engine connections");
ABSL_FLAG(int, listen_port, 10007, "Port to listen for engine connections");
ABSL_FLAG(int, num_sender_threads, 20, "Number of request sender threads");
ABSL_FLAG(int, duration_sec, 30, "Test duration in seconds");
ABSL_FLAG(int, target_rps, 0, "Target RPS per sender thread (0 = unlimited)");
ABSL_FLAG(int, func_id, 1, "Function ID to invoke");

using namespace faas;
using protocol::GatewayMessage;
using protocol::FuncCall;
using protocol::NewFuncCall;
using protocol::NewDispatchFuncCallGatewayMessage;
using protocol::IsEngineHandshakeMessage;
using protocol::IsFuncCallCompleteMessage;
using protocol::IsFuncCallFailedMessage;
using protocol::GetFuncCallFromMessage;

struct Stats {
    std::atomic<uint64_t> requests_sent{0};
    std::atomic<uint64_t> responses_received{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> total_latency_us{0};
    std::atomic<uint64_t> min_latency_us{UINT64_MAX};
    std::atomic<uint64_t> max_latency_us{0};

    // For percentile calculations
    absl::Mutex latencies_mu;
    std::vector<uint64_t> latencies ABSL_GUARDED_BY(latencies_mu);
};

class StressGateway : public uv::Base {
public:
    StressGateway()
        : state_(kCreated),
          next_call_id_(1),
          engine_sock_fd_(-1),
          stop_sending_(false),
          stats_(nullptr) {
        UV_DCHECK_OK(uv_loop_init(&uv_loop_));
        UV_DCHECK_OK(uv_tcp_init(&uv_loop_, &uv_listen_handle_));
        uv_listen_handle_.data = this;
    }

    ~StressGateway() {
        if (engine_sock_fd_ != -1) {
            close(engine_sock_fd_);
        }
        UV_DCHECK_OK(uv_loop_close(&uv_loop_));
    }

    bool Start() {
        std::string addr = absl::GetFlag(FLAGS_listen_addr);
        int port = absl::GetFlag(FLAGS_listen_port);

        struct sockaddr_in bind_addr;
        UV_CHECK_OK(uv_ip4_addr(addr.c_str(), port, &bind_addr));
        UV_CHECK_OK(uv_tcp_bind(&uv_listen_handle_, (const struct sockaddr*)&bind_addr, 0));

        LOG(INFO) << "Listening on " << addr << ":" << port << " for engine connections";

        UV_CHECK_OK(uv_listen(UV_AS_STREAM(&uv_listen_handle_), 128,
                              &StressGateway::ConnectionCallback));

        state_ = kRunning;

        // Start event loop in separate thread using base::Thread
        event_loop_thread_.reset(new base::Thread("EventLoop", [this]() {
            LOG(INFO) << "Event loop thread started";
            uv_run(&uv_loop_, UV_RUN_DEFAULT);
            LOG(INFO) << "Event loop thread finished";
        }));
        event_loop_thread_->Start();

        // Give event loop time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        return true;
    }

    void WaitForEngine() {
        LOG(INFO) << "Waiting for engine connection...";
        while (engine_sock_fd_ == -1 && state_ == kRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (engine_sock_fd_ != -1) {
            LOG(INFO) << "Engine connected!";
        }
    }

    void RunLoadTest(Stats* stats) {
        stats_ = stats;  // Store global stats pointer
        int num_threads = absl::GetFlag(FLAGS_num_sender_threads);

        std::vector<std::unique_ptr<base::Thread>> sender_threads;

        for (int i = 0; i < num_threads; i++) {
            auto thread = std::make_unique<base::Thread>(
                fmt::format("Sender{}", i),
                [this, stats, i]() {
                    this->SenderThread(stats, i);
                });
            thread->Start();
            sender_threads.push_back(std::move(thread));
        }

        // Wait for all senders to finish
        for (auto& t : sender_threads) {
            t->Join();
        }

        stop_sending_ = true;
    }

    void Stop() {
        stop_sending_ = true;

        // Note: We don't join receiver threads or event loop thread - they will exit naturally.
        // For a stress testing tool, clean shutdown isn't critical since the process will exit anyway.
        // Joining would hang because:
        // - Receiver threads are blocked on RecvData()
        // - Event loop thread needs time to process handle close callbacks

        // Stop the event loop (it will finish current iteration)
        uv_stop(&uv_loop_);

        state_ = kStopped;
    }

private:
    enum State { kCreated, kRunning, kStopped };

    State state_;
    uv_loop_t uv_loop_;
    uv_tcp_t uv_listen_handle_;
    std::unique_ptr<base::Thread> event_loop_thread_;

    std::atomic<uint32_t> next_call_id_;
    std::atomic<int> engine_sock_fd_;
    std::atomic<bool> stop_sending_;

    absl::Mutex mu_;
    absl::flat_hash_map<uint64_t, int64_t> pending_calls_ ABSL_GUARDED_BY(mu_);
    Stats* stats_;  // Global stats pointer
    absl::Mutex receiver_threads_mu_;
    std::vector<std::unique_ptr<base::Thread>> receiver_threads_ ABSL_GUARDED_BY(receiver_threads_mu_);

    void SenderThread(Stats* stats, int thread_id) {
        int duration_sec = absl::GetFlag(FLAGS_duration_sec);
        int target_rps = absl::GetFlag(FLAGS_target_rps);
        int func_id = absl::GetFlag(FLAGS_func_id);

        auto start_time = std::chrono::steady_clock::now();
        auto end_time = start_time + std::chrono::seconds(duration_sec);

        uint64_t sent_count = 0;
        int64_t sleep_ns = 0;
        if (target_rps > 0) {
            sleep_ns = 1000000000LL / target_rps;
        }

        while (std::chrono::steady_clock::now() < end_time && !stop_sending_) {
            int sock = engine_sock_fd_.load();
            if (sock == -1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // Create function call
            uint16_t client_id = 0;
            uint32_t call_id = next_call_id_++;
            FuncCall func_call = NewFuncCall(
                gsl::narrow_cast<uint16_t>(func_id), client_id, call_id);

            // Create dispatch message
            GatewayMessage message = NewDispatchFuncCallGatewayMessage(func_call);
            message.payload_size = 0;

            // Record send timestamp
            int64_t send_time = GetMonotonicMicroTimestamp();

            {
                absl::MutexLock lock(&mu_);
                pending_calls_[func_call.full_call_id] = send_time;
            }

            // Send to engine
            if (!io_utils::SendData(sock, reinterpret_cast<const char*>(&message),
                                    sizeof(GatewayMessage))) {
                LOG(ERROR) << "Thread " << thread_id << " failed to send message";
                stats->errors++;
                break;
            }

            stats->requests_sent++;
            sent_count++;

            // Rate limiting
            if (target_rps > 0 && sleep_ns > 0) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
            }
        }

        LOG(INFO) << "Sender thread " << thread_id << " sent " << sent_count << " requests";
    }

    void ReceiveLoop(int sock_fd, Stats* stats) {
        LOG(INFO) << "Receiver thread started with fd=" << sock_fd;

        while (!stop_sending_) {
            GatewayMessage response;
            bool eof = false;

            if (!io_utils::RecvData(sock_fd, reinterpret_cast<char*>(&response),
                                    sizeof(GatewayMessage), &eof)) {
                if (eof) {
                    LOG(INFO) << "Engine connection closed (EOF)";
                    break;
                }
                if (stop_sending_) {
                    LOG(INFO) << "Receiver stopping due to stop signal";
                    break;
                }
                LOG(ERROR) << "Failed to receive response from engine";
                stats->errors++;
                break;
            }

            if (IsFuncCallCompleteMessage(response) || IsFuncCallFailedMessage(response)) {
                int64_t recv_time = GetMonotonicMicroTimestamp();

                FuncCall func_call = GetFuncCallFromMessage(response);

                int64_t send_time = 0;
                {
                    absl::MutexLock lock(&mu_);
                    auto it = pending_calls_.find(func_call.full_call_id);
                    if (it != pending_calls_.end()) {
                        send_time = it->second;
                        pending_calls_.erase(it);
                    }
                }

                if (send_time > 0) {
                    uint64_t latency_us = recv_time - send_time;
                    stats->total_latency_us += latency_us;

                    // Update min/max
                    uint64_t current_min = stats->min_latency_us.load();
                    while (latency_us < current_min &&
                           !stats->min_latency_us.compare_exchange_weak(current_min, latency_us));

                    uint64_t current_max = stats->max_latency_us.load();
                    while (latency_us > current_max &&
                           !stats->max_latency_us.compare_exchange_weak(current_max, latency_us));

                    // Store latency for percentile calculation
                    {
                        absl::MutexLock lock(&stats->latencies_mu);
                        stats->latencies.push_back(latency_us);
                    }
                }

                stats->responses_received++;

                if (IsFuncCallFailedMessage(response)) {
                    stats->errors++;
                }
            }
        }

        LOG(INFO) << "Receiver thread stopped";
    }

    static void ConnectionCallback(uv_stream_t* server, int status) {
        StressGateway* self = reinterpret_cast<StressGateway*>(server->data);

        if (status < 0) {
            LOG(ERROR) << "Connection error: " << uv_strerror(status);
            return;
        }

        uv_tcp_t* client = reinterpret_cast<uv_tcp_t*>(malloc(sizeof(uv_tcp_t)));
        uv_tcp_init(self->uv_loop(), client);

        if (uv_accept(server, UV_AS_STREAM(client)) == 0) {
            LOG(INFO) << "Engine connected!";

            // Start reading handshake
            client->data = self;
            uv_read_start(UV_AS_STREAM(client), AllocCallback, ReadHandshakeCallback);
        } else {
            uv_close(UV_AS_HANDLE(client), CloseCallback);
        }
    }

    static void AllocCallback(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
        buf->base = reinterpret_cast<char*>(malloc(suggested_size));
        buf->len = suggested_size;
    }

    static void ReadHandshakeCallback(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
        StressGateway* self = reinterpret_cast<StressGateway*>(stream->data);

        if (nread < 0) {
            if (nread != UV_EOF) {
                LOG(ERROR) << "Read error: " << uv_strerror(nread);
            }
            free(buf->base);
            uv_close(UV_AS_HANDLE(stream), CloseCallback);
            return;
        }

        if (nread == 0) {
            free(buf->base);
            return;
        }

        // Check if it's a handshake message
        if (nread >= static_cast<ssize_t>(sizeof(GatewayMessage))) {
            const GatewayMessage* msg = reinterpret_cast<const GatewayMessage*>(buf->base);

            if (IsEngineHandshakeMessage(*msg)) {
                LOG(INFO) << "Received engine handshake: node_id=" << msg->node_id
                          << ", conn_id=" << msg->conn_id;

                // Get the underlying fd
                int fd;
                uv_fileno(UV_AS_HANDLE(stream), &fd);

                // Duplicate the fd for our use (so we can use it even after uv closes)
                int dup_fd = dup(fd);
                if (dup_fd < 0) {
                    PLOG(ERROR) << "Failed to dup fd";
                    free(buf->base);
                    uv_close(UV_AS_HANDLE(stream), CloseCallback);
                    return;
                }

                self->engine_sock_fd_ = dup_fd;

                // Stop reading on uv stream
                uv_read_stop(stream);

                // Close the uv handle (we're using dup_fd now)
                // This prevents the handle leak
                uv_close(UV_AS_HANDLE(stream), CloseCallback);

                // Start receiver thread with duplicated fd using base::Thread
                {
                    absl::MutexLock lock(&self->receiver_threads_mu_);
                    auto receiver = std::make_unique<base::Thread>(
                        fmt::format("Receiver{}", self->receiver_threads_.size()),
                        [self, dup_fd]() {
                            // Wait for stats to be set
                            while (self->stats_ == nullptr) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            }
                            self->ReceiveLoop(dup_fd, self->stats_);
                            // Close our duplicated fd when done
                            close(dup_fd);
                        });
                    receiver->Start();
                    self->receiver_threads_.push_back(std::move(receiver));
                }

                LOG(INFO) << "Engine handshake complete, receiver thread started";
                free(buf->base);
                return;  // Important: return early to avoid double-free
            }
        }

        free(buf->base);
    }

    static void CloseCallback(uv_handle_t* handle) {
        free(handle);
    }

    uv_loop_t* uv_loop() { return &uv_loop_; }
};

void PrintStats(Stats* stats, int duration_sec) {
    uint64_t total_sent = stats->requests_sent.load();
    uint64_t total_received = stats->responses_received.load();
    uint64_t total_errors = stats->errors.load();
    uint64_t total_latency = stats->total_latency_us.load();
    uint64_t min_latency = stats->min_latency_us.load();
    uint64_t max_latency = stats->max_latency_us.load();

    double avg_latency = 0;
    if (total_received > 0) {
        avg_latency = static_cast<double>(total_latency) / total_received;
    }

    double throughput = static_cast<double>(total_received) / duration_sec;

    // Calculate 99th percentile
    uint64_t p99_latency = 0;
    {
        absl::MutexLock lock(&stats->latencies_mu);
        if (!stats->latencies.empty()) {
            std::vector<uint64_t> sorted_latencies = stats->latencies;
            std::sort(sorted_latencies.begin(), sorted_latencies.end());
            size_t p99_index = (sorted_latencies.size() * 99) / 100;
            if (p99_index >= sorted_latencies.size()) {
                p99_index = sorted_latencies.size() - 1;
            }
            p99_latency = sorted_latencies[p99_index];
        }
    }

    LOG(INFO) << "=== Benchmark Results ===";
    LOG(INFO) << "Duration: " << duration_sec << " seconds";
    LOG(INFO) << "Total requests sent: " << total_sent;
    LOG(INFO) << "Total responses received: " << total_received;
    LOG(INFO) << "Total errors: " << total_errors;
    LOG(INFO) << "Throughput: " << throughput << " req/s";
    LOG(INFO) << "Average latency: " << avg_latency << " μs";
    if (min_latency != UINT64_MAX) {
        LOG(INFO) << "Min latency: " << min_latency << " μs";
    }
    LOG(INFO) << "P99 latency: " << p99_latency << " μs";
    LOG(INFO) << "Max latency: " << max_latency << " μs";
    if (total_sent > 0) {
        LOG(INFO) << "Success rate: "
                  << (100.0 * (total_received - total_errors) / total_sent) << "%";
    }
}

int main(int argc, char* argv[]) {
    faas::base::InitMain(argc, argv);

    int duration_sec = absl::GetFlag(FLAGS_duration_sec);

    LOG(INFO) << "Starting Nightcore stress test (Gateway mode)";
    LOG(INFO) << "Test duration: " << duration_sec << " seconds";
    LOG(INFO) << "Sender threads: " << absl::GetFlag(FLAGS_num_sender_threads);
    LOG(INFO) << "Target RPS per thread: "
              << (absl::GetFlag(FLAGS_target_rps) == 0 ? "unlimited" :
                  std::to_string(absl::GetFlag(FLAGS_target_rps)));

    Stats stats;
    StressGateway gateway;

    if (!gateway.Start()) {
        LOG(FATAL) << "Failed to start gateway";
    }

    // Wait for engine to connect
    gateway.WaitForEngine();

    // Give engine time to stabilize
    std::this_thread::sleep_for(std::chrono::seconds(2));

    LOG(INFO) << "Starting load test...";
    auto start_time = std::chrono::steady_clock::now();

    // Run load test
    gateway.RunLoadTest(&stats);

    auto end_time = std::chrono::steady_clock::now();
    int actual_duration = std::chrono::duration_cast<std::chrono::seconds>(
        end_time - start_time).count();

    // Give time for remaining responses
    std::this_thread::sleep_for(std::chrono::seconds(2));

    gateway.Stop();

    PrintStats(&stats, actual_duration);

    return 0;
}
