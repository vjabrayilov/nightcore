#include "common/machnet_transport.h"

#include <cstring>

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
        LOG(ERROR) << "machnet_init() failed with error: " << ret;
        return false;
    }

    channel_ = machnet_attach();
    if (channel_ == nullptr) {
        LOG(ERROR) << "machnet_attach() failed";
        return false;
    }

    LOG(INFO) << "Machnet channel initialized successfully";
    return true;
}

std::unique_ptr<MachnetListener> MachnetChannel::CreateListener(const std::string& local_ip,
                                                                uint16_t port) {
    if (channel_ == nullptr) {
        LOG(ERROR) << "Machnet channel not initialized";
        return nullptr;
    }

    auto listener = std::unique_ptr<MachnetListener>(
        new MachnetListener(channel_, local_ip, port));

    return listener;
}

std::unique_ptr<MachnetConnection> MachnetChannel::CreateConnection(
    const std::string& local_ip, const std::string& remote_ip, uint16_t remote_port) {
    if (channel_ == nullptr) {
        LOG(ERROR) << "Machnet channel not initialized";
        return nullptr;
    }

    MachnetFlow_t flow;
    int ret = machnet_connect(channel_, local_ip.c_str(), remote_ip.c_str(),
                             remote_port, &flow);
    if (ret != 0) {
        LOG(ERROR) << "machnet_connect() failed with error: " << ret;
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
        LOG(FATAL) << "machnet_listen() failed on " << local_ip << ":" << port
                   << " with error: " << ret;
    }

    LOG(INFO) << "Machnet listening on " << local_ip << ":" << port;

    // Register with the channel for polling
    MachnetChannel::Get()->RegisterListener(this);
}

MachnetListener::~MachnetListener() {
    MachnetChannel::Get()->UnregisterListener(this);
}

void MachnetListener::Poll() {
    // Poll for incoming messages from any flow
    constexpr size_t kBufferSize = 65536;
    static char buffer[kBufferSize];

    MachnetFlow_t flow;
    ssize_t ret = machnet_recv(channel_, buffer, kBufferSize, &flow);

    if (ret > 0) {
        LOG(INFO) << fmt::format("Listener received {} bytes from flow {}:{} -> {}:{}",
                                ret, flow.src_ip, flow.src_port, flow.dst_ip, flow.dst_port);
        // New message received
        uint64_t flow_id = ((uint64_t)flow.src_ip << 32) |
                          ((uint64_t)flow.src_port << 16) | flow.dst_port;

        MachnetConnection* conn = nullptr;
        auto it = connections_.find(flow_id);

        if (it == connections_.end()) {
            // New connection from a remote peer
            // IMPORTANT: The received flow has src=remote, dst=local
            // For sending responses, we need dst=remote, so SWAP src/dst
            MachnetFlow_t send_flow;
            send_flow.src_ip = flow.dst_ip;      // local IP becomes src
            send_flow.src_port = flow.dst_port;  // local port becomes src
            send_flow.dst_ip = flow.src_ip;      // remote IP becomes dst
            send_flow.dst_port = flow.src_port;  // remote port becomes dst

            LOG(INFO) << fmt::format("Creating connection with swapped flow: {}:{} -> {}:{}",
                                    send_flow.src_ip, send_flow.src_port,
                                    send_flow.dst_ip, send_flow.dst_port);

            auto new_conn = std::make_unique<MachnetConnection>(channel_, send_flow);
            conn = new_conn.get();
            connections_[flow_id] = std::move(new_conn);

            if (new_connection_callback_) {
                new_connection_callback_(conn);
            }
        } else {
            conn = it->second.get();
        }

        // Append data to connection's read buffer
        if (conn) {
            conn->read_buffer_.AppendData(buffer, ret);
            conn->ProcessMessages();
        }
    } else if (ret == 0) {
        // No data available, this is normal
    } else {
        // Error
        LOG(ERROR) << "machnet_recv() failed with error: " << ret;
    }
}

// ============================================================================
// MachnetConnection
// ============================================================================

MachnetConnection::MachnetConnection(void* channel, const MachnetFlow_t& flow)
    : channel_(channel), flow_(flow) {
    LOG(INFO) << fmt::format("Machnet connection created: {}:{} -> {}:{} (channel={:p})",
                            flow.src_ip, flow.src_port, flow.dst_ip, flow.dst_port, channel);
}

MachnetConnection::~MachnetConnection() {
    LOG(INFO) << fmt::format("Machnet connection destroyed: {}:{} -> {}:{}",
                            flow_.src_ip, flow_.src_port, flow_.dst_ip, flow_.dst_port);
}

bool MachnetConnection::SendMessage(const protocol::GatewayMessage& message,
                                    std::span<const char> payload) {
    DCHECK_EQ(message.payload_size, gsl::narrow_cast<int32_t>(payload.size()));

    LOG(INFO) << fmt::format("SendMessage: using flow {}:{} -> {}:{}, channel={:p}",
                            flow_.src_ip, flow_.src_port, flow_.dst_ip, flow_.dst_port, channel_);

    // Send header
    int ret = machnet_send(channel_, flow_, &message, sizeof(protocol::GatewayMessage));
    if (ret != 0) {
        LOG(ERROR) << "machnet_send() failed for header with error: " << ret;
        return false;
    }

    // Send payload if present
    if (payload.size() > 0) {
        ret = machnet_send(channel_, flow_, payload.data(), payload.size());
        if (ret != 0) {
            LOG(ERROR) << "machnet_send() failed for payload with error: " << ret;
            return false;
        }
    }

    return true;
}

void MachnetConnection::Poll() {
    constexpr size_t kBufferSize = 65536;
    static char buffer[kBufferSize];

    MachnetFlow_t recv_flow;
    ssize_t ret = machnet_recv(channel_, buffer, kBufferSize, &recv_flow);

    if (ret > 0) {
        LOG(INFO) << fmt::format("Machnet received {} bytes from flow {}:{} -> {}:{}",
                                ret, recv_flow.src_ip, recv_flow.src_port, recv_flow.dst_ip, recv_flow.dst_port);
        read_buffer_.AppendData(buffer, ret);
        ProcessMessages();
    } else if (ret < 0) {
        LOG(ERROR) << fmt::format("machnet_recv() failed with error: {}", ret);
    }
    // ret == 0 means no data, which is normal - don't log
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
            std::span<const char> payload(
                read_buffer_.data() + sizeof(protocol::GatewayMessage),
                full_size - sizeof(protocol::GatewayMessage));

            LOG(INFO) << fmt::format("Machnet message complete: header + {} bytes payload", payload.size());
            if (message_callback_) {
                message_callback_(*message, payload);
            } else {
                LOG(WARNING) << "No message callback set!";
            }

            read_buffer_.ConsumeFront(full_size);
        } else {
            // Incomplete message, wait for more data
            VLOG(1) << fmt::format("Incomplete message: have {}, need {} bytes",
                                   read_buffer_.length(), full_size);
            break;
        }
    }
}

}  // namespace machnet
}  // namespace faas
