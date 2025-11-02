#include "common/machnet_transport.h"

#include <cstring>
#include <string>

namespace faas {
namespace machnet {

std::unique_ptr<MachnetChannel> MachnetChannel::instance_ = nullptr;

MachnetChannel* MachnetChannel::Get() {
    if (!instance_) {
        instance_ = std::unique_ptr<MachnetChannel>(new MachnetChannel());
    }
    return instance_.get();
}

MachnetChannel::MachnetChannel() : channel_(nullptr) {}

MachnetChannel::~MachnetChannel() {
    // Note: machnet doesn't provide cleanup functions
    // The channel will be cleaned up when the Machnet daemon stops
}

bool MachnetChannel::Init() {
    if (channel_ != nullptr) {
        return true;  // Already initialized
    }

    int ret = machnet_init();
    if (ret != 0) {
        fprintf(stderr, "ERROR: machnet_init() failed with error: %d\n", ret);
        return false;
    }

    channel_ = machnet_attach();
    if (channel_ == nullptr) {
        fprintf(stderr, "ERROR: machnet_attach() failed\n");
        return false;
    }

    return true;
}

std::unique_ptr<MachnetListener> MachnetChannel::CreateListener(const std::string& local_ip,
                                                                uint16_t port) {
    if (channel_ == nullptr) {
        fprintf(stderr, "ERROR: Machnet channel not initialized\n");
        return nullptr;
    }

    auto listener = std::unique_ptr<MachnetListener>(
        new MachnetListener(channel_, local_ip, port));

    return listener;
}

std::unique_ptr<MachnetConnection> MachnetChannel::CreateConnection(
    const std::string& local_ip, const std::string& remote_ip, uint16_t remote_port) {
    if (channel_ == nullptr) {
        fprintf(stderr, "ERROR: Machnet channel not initialized\n");
        return nullptr;
    }

    MachnetFlow_t flow;
    int ret = machnet_connect(channel_, local_ip.c_str(), remote_ip.c_str(),
                             remote_port, &flow);
    if (ret != 0) {
        fprintf(stderr, "ERROR: machnet_connect() failed with error: %d\n", ret);
        return nullptr;
    }

    auto connection = std::unique_ptr<MachnetConnection>(
        new MachnetConnection(channel_, flow));

    return connection;
}

void MachnetChannel::Poll() {
    // Poll all registered listeners
    for (auto* listener : listeners_) {
        listener->Poll();
    }
}

void MachnetChannel::TryPoll() {
    for (auto* listener : listeners_) {
        listener->TryPoll();
    }
}

void MachnetChannel::RegisterListener(MachnetListener* listener) {
    listeners_.push_back(listener);
}

void MachnetChannel::UnregisterListener(MachnetListener* listener) {
    listeners_.erase(
        std::remove(listeners_.begin(), listeners_.end(), listener),
        listeners_.end());
}

// ============================================================================
// MachnetListener
// ============================================================================

MachnetListener::MachnetListener(void* channel, const std::string& local_ip, uint16_t port)
    : channel_(channel), local_ip_(local_ip), port_(port) {

    int ret = machnet_listen(channel_, local_ip.c_str(), port);
    if (ret != 0) {
        fprintf(stderr, "FATAL: machnet_listen() failed on %s:%d with error: %d\n",
                local_ip.c_str(), port, ret);
        abort();
    }

    // Register with the channel for polling
    MachnetChannel::Get()->RegisterListener(this);
}

MachnetListener::~MachnetListener() {
    MachnetChannel::Get()->UnregisterListener(this);
}

void MachnetListener::TryPoll() {
    constexpr size_t kBufferSize = 65536;
    static char buffer[kBufferSize];
    MachnetFlow_t flow;
    ssize_t ret = machnet_recv(channel_, buffer, kBufferSize, &flow);
    if (ret <= 0) {
        // ret == 0: no data; ret < 0: error (silently ignore)
        return;
    }

    uint64_t flow_id = ((uint64_t)flow.src_ip << 32) |
                      ((uint64_t)flow.src_port << 16) | flow.dst_port;

    MachnetConnection* conn = nullptr;
    auto it = connections_.find(flow_id);

    if (it == connections_.end()) {
        if (single_connection_only_ && !connections_.empty()) {
            // Drop messages from additional flows in single-connection mode
            return;
        }
        // New connection from a remote peer; swap for sending
        MachnetFlow_t send_flow;
        send_flow.src_ip = flow.dst_ip;
        send_flow.src_port = flow.dst_port;
        send_flow.dst_ip = flow.src_ip;
        send_flow.dst_port = flow.src_port;

        auto new_conn = std::make_unique<MachnetConnection>(channel_, send_flow);
        conn = new_conn.get();
        connections_[flow_id] = std::move(new_conn);

        if (new_connection_callback_) {
            new_connection_callback_(conn);
        }
        std::cerr << "New Machnet connection created: " << flow.src_ip << ":" << flow.src_port << " -> " << flow.dst_ip << ":" << flow.dst_port << std::endl;
    } else {
        conn = it->second.get();
    }

    if (conn) {
        conn->read_buffer_.AppendData(buffer, ret);
        conn->ProcessMessages();
    }
}

void MachnetListener::Poll() {
    // Poll for incoming messages from any flow and drain immediately
    while (true) {
        constexpr size_t kBufferSize = 65536;
        char buffer[kBufferSize];
        MachnetFlow_t flow;
        ssize_t ret = machnet_recv(channel_, buffer, kBufferSize, &flow);
        if (ret <= 0) {
            // ret == 0: no data; ret < 0: error (silently ignore)
            break;
        }

        uint64_t flow_id = ((uint64_t)flow.src_ip << 32) |
                          ((uint64_t)flow.src_port << 16) | flow.dst_port;

        MachnetConnection* conn = nullptr;
        auto it = connections_.find(flow_id);

        if (it == connections_.end()) {
            if (single_connection_only_ && !connections_.empty()) {
                // Drop messages from additional flows in single-connection mode
                continue;
            }
            // New connection from a remote peer; swap for sending
            MachnetFlow_t send_flow;
            send_flow.src_ip = flow.dst_ip;
            send_flow.src_port = flow.dst_port;
            send_flow.dst_ip = flow.src_ip;
            send_flow.dst_port = flow.src_port;

            auto new_conn = std::make_unique<MachnetConnection>(channel_, send_flow);
            conn = new_conn.get();
            connections_[flow_id] = std::move(new_conn);
            std::cerr << "New Machnet connection created: " << flow.src_ip << ":" << flow.src_port << " -> " << flow.dst_ip << ":" << flow.dst_port << std::endl;
            if (new_connection_callback_) {
                new_connection_callback_(conn);
            }
        } else {
            conn = it->second.get();
        }

        if (conn) {
            conn->read_buffer_.AppendData(buffer, ret);
            conn->ProcessMessages();
        }
    }
}

// ============================================================================
// MachnetConnection
// ============================================================================

MachnetConnection::MachnetConnection(void* channel, const MachnetFlow_t& flow)
    : channel_(channel), flow_(flow) {
    // LOG(INFO) << fmt::format("Machnet connection created: {}:{} -> {}:{} (channel={:p})",
    //                         flow.src_ip, flow.src_port, flow.dst_ip, flow.dst_port, channel);
}

MachnetConnection::~MachnetConnection() {
    // LOG(INFO) << fmt::format("Machnet connection destroyed: {}:{} -> {}:{}",
    //                         flow_.src_ip, flow_.src_port, flow_.dst_ip, flow_.dst_port);
}

bool MachnetConnection::SendMessage(const protocol::GatewayMessage& message,
                                    std::span<const char> payload) {
    // Note: DCHECK removed - not safe on non-Nightcore threads
    // assert(message.payload_size == static_cast<int32_t>(payload.size()));

    // Optimize: send header+payload in one call to reduce overhead
    if (payload.empty()) {
        // No payload - send header only
        int ret = machnet_send(channel_, flow_, &message, sizeof(protocol::GatewayMessage));
        if (ret != 0) {
            // Silently fail - logging not safe on this thread
            std::cerr<<"Flow src: "<<flow_.src_ip<<" port: "<<flow_.src_port<<" dst_ip: "<<flow_.dst_ip<<" port: "<<flow_.dst_port<<std::endl;
            std::cerr << "ERROR: machnet_send() failed with error: " << ret << std::endl;
            return false;
        }
    } else {
        // Batch header and payload into a single buffer for one machnet_send() call
        // This significantly reduces syscall overhead
        size_t total_size = sizeof(protocol::GatewayMessage) + payload.size();

        // Use stack allocation for small messages (common case)
        constexpr size_t kStackBufferSize = 4096;
        if (total_size <= kStackBufferSize) {
            char stack_buffer[kStackBufferSize];
            memcpy(stack_buffer, &message, sizeof(protocol::GatewayMessage));
            memcpy(stack_buffer + sizeof(protocol::GatewayMessage), payload.data(), payload.size());

            int ret = machnet_send(channel_, flow_, stack_buffer, total_size);
            if (ret != 0) {
                return false;
            }
        } else {
            // Heap allocation for large messages
            std::vector<char> buffer(total_size);
            memcpy(buffer.data(), &message, sizeof(protocol::GatewayMessage));
            memcpy(buffer.data() + sizeof(protocol::GatewayMessage), payload.data(), payload.size());

            int ret = machnet_send(channel_, flow_, buffer.data(), total_size);
            if (ret != 0) {
                return false;
            }
        }
    }

    return true;
}

void MachnetConnection::Poll() {
    constexpr size_t kBufferSize = 65536;
    static char buffer[kBufferSize];

    for (;;) {
        MachnetFlow_t recv_flow;
        ssize_t ret = machnet_recv(channel_, buffer, kBufferSize, &recv_flow);
        if (ret <= 0) {
            // Silently ignore errors
            break;
        }
        read_buffer_.AppendData(buffer, ret);
        ProcessMessages();
    }
}

void MachnetConnection::ProcessMessages() {
    // Process all complete messages in the buffer
    while (read_buffer_.length() >= sizeof(protocol::GatewayMessage)) {
        auto* message = reinterpret_cast<const protocol::GatewayMessage*>(
            read_buffer_.data());

        size_t full_size = sizeof(protocol::GatewayMessage) +
                          std::max<size_t>(0, message->payload_size);

        if (read_buffer_.length() >= full_size) {
            // Complete message available
            const size_t payload_size = full_size - sizeof(protocol::GatewayMessage);

            // Copy header and payload into owning storage to avoid dangling views
            protocol::GatewayMessage header_copy = *message;
            std::string payload_copy;
            payload_copy.resize(payload_size);
            if (payload_size > 0) {
                std::memcpy(payload_copy.data(),
                            read_buffer_.data() + sizeof(protocol::GatewayMessage),
                            payload_size);
            }

            if (message_callback_) {
                std::span<const char> payload_view(payload_copy.data(), payload_copy.size());
                message_callback_(header_copy, payload_view);
            }
            // Silently ignore if no callback set

            read_buffer_.ConsumeFront(full_size);
        } else {
            // Incomplete message, wait for more data
            break;
        }
    }
}

}  // namespace machnet
}  // namespace faas
