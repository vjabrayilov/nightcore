#pragma once

#include "common/protocol.h"
#include "utils/appendable_buffer.h"

#include <machnet.h>

#include <functional>
#include <memory>
#include <unordered_map>

// IMPORTANT: Thread Safety Note
// ==============================
// This Machnet transport layer is designed to be used from non-Nightcore threads
// (e.g., raw std::thread). The implementation MUST NOT use LOG/VLOG/DCHECK macros
// because they rely on base::Thread::current() which is only valid for threads
// registered with Nightcore's thread system. Use fprintf(stderr) for critical errors.

namespace faas {
namespace machnet {

// Forward declarations
class MachnetChannel;
class MachnetListener;
class MachnetConnection;

// Callback type for new connections
using NewConnectionCallback = std::function<void(MachnetConnection*)>;
// Callback type for received messages
using MessageCallback = std::function<void(const protocol::GatewayMessage&, std::span<const char>)>;

/**
 * @brief Singleton Machnet channel manager
 *
 * Manages the global Machnet channel and provides interfaces for
 * creating listeners and connections.
 */
class MachnetChannel {
public:
    static MachnetChannel* Get();

    ~MachnetChannel();

    // Initialize Machnet (calls machnet_init and machnet_attach)
    bool Init();

    // Get the raw channel context
    void* channel() const { return channel_; }

    // Create a listener on a specific IP and port
    std::unique_ptr<MachnetListener> CreateListener(const std::string& local_ip, uint16_t port);

    // Create a connection to a remote endpoint
    std::unique_ptr<MachnetConnection> CreateConnection(const std::string& local_ip,
                                                        const std::string& remote_ip,
                                                        uint16_t remote_port);

    // Poll for incoming messages (should be called from event loop)
    void Poll();

    // Try to poll for incoming messages (should be called from event loop)
    void TryPoll();

    // Register/unregister listeners for polling
    void RegisterListener(MachnetListener* listener);
    void UnregisterListener(MachnetListener* listener);

private:
    MachnetChannel();

    void* channel_ = nullptr;  // Machnet channel context
    std::vector<MachnetListener*> listeners_;

    static std::unique_ptr<MachnetChannel> instance_;
};

/**
 * @brief Machnet listener
 *
 * Listens for incoming connections on a specific IP:port.
 */
class MachnetListener {
public:
    MachnetListener(void* channel, const std::string& local_ip, uint16_t port);
    ~MachnetListener();

    // Set callback for new connections
    void SetNewConnectionCallback(NewConnectionCallback callback) {
        new_connection_callback_ = callback;
    }

    // Poll for incoming connections
    void Poll();

    // Try to poll for incoming messages
    void TryPoll();

    const std::string& local_ip() const { return local_ip_; }
    uint16_t port() const { return port_; }

private:
    void* channel_;
    std::string local_ip_;
    uint16_t port_;
    NewConnectionCallback new_connection_callback_;

    // Track active connections by flow (owns the connection objects)
    std::unordered_map<uint64_t, std::unique_ptr<MachnetConnection>> connections_;

    friend class MachnetChannel;
    friend class MachnetConnection;
};

/**
 * @brief Machnet connection
 *
 * Represents a bidirectional connection between two endpoints.
 */
class MachnetConnection {
public:
    MachnetConnection(void* channel, const MachnetFlow_t& flow);
    ~MachnetConnection();

    // Send a message
    bool SendMessage(const protocol::GatewayMessage& message, std::span<const char> payload);

    // Set callback for received messages
    void SetMessageCallback(MessageCallback callback) {
        message_callback_ = callback;
    }

    // Poll for incoming messages
    void Poll();

    const MachnetFlow_t& flow() const { return flow_; }

private:
    void* channel_;
    MachnetFlow_t flow_;
    MessageCallback message_callback_;
    utils::AppendableBuffer read_buffer_;

    // Helper to process buffered messages
    void ProcessMessages();

    // Compute unique flow ID for hash maps
    uint64_t FlowId() const {
        return ((uint64_t)flow_.src_ip << 32) | ((uint64_t)flow_.src_port << 16) | flow_.dst_port;
    }

    friend class MachnetListener;
};

}  // namespace machnet
}  // namespace faas
