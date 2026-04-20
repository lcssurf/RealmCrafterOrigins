#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>

#include <msquic.h>

#include "codec.h"

namespace rco::net {

// ---------------------------------------------------------------------------
// A single decoded inbound packet (header already stripped).
// ---------------------------------------------------------------------------
struct InboundPacket {
    uint16_t             type;
    std::vector<uint8_t> payload;
};

using PacketHandler = std::function<void(const InboundPacket&)>;

// ---------------------------------------------------------------------------
// Connection — wraps a single MsQuic client connection + bidirectional stream.
//
// Thread model:
//   MsQuic callbacks fire on its internal thread pool.
//   Received data is pushed into recv_queue_ under recv_mutex_.
//   The main/game thread calls Poll() each frame to drain the queue.
//   SendPacket() is thread-safe (locks send_mutex_).
// ---------------------------------------------------------------------------
class Connection {
public:
    Connection();
    ~Connection();

    // Opens a QUIC connection to host:port.  Returns true if the handshake
    // was started successfully (actual connectivity checked via IsConnected).
    bool Connect(const std::string& host, uint16_t port);

    // Gracefully close stream, connection and registration.
    void Disconnect();

    bool IsConnected() const { return connected_.load(); }

    // Build a framed packet from Writer contents and send it.
    void SendPacket(uint16_t type, const Writer& w);

    // Send a pre-framed raw buffer (used internally and for testing).
    void SendRaw(const std::vector<uint8_t>& data);

    // Dequeue one inbound packet.  Returns false if the queue is empty.
    bool Poll(InboundPacket& out);

    // Optional: install a handler that is called synchronously inside Poll().
    void SetPacketHandler(PacketHandler h) { handler_ = std::move(h); }

private:
    // msquic handles
    const QUIC_API_TABLE* api_           = nullptr;
    HQUIC                 registration_  = nullptr;
    HQUIC                 configuration_ = nullptr;
    HQUIC                 connection_    = nullptr;
    HQUIC                 stream_        = nullptr;

    std::atomic<bool> connected_{false};

    // Inbound packet queue (producer: MsQuic thread; consumer: game thread)
    std::mutex                 recv_mutex_;
    std::queue<InboundPacket>  recv_queue_;

    // Serialises concurrent sends
    std::mutex send_mutex_;

    // Reassembly buffer — accumulates raw bytes from MsQuic receive events
    std::vector<uint8_t> recv_buf_;

    PacketHandler handler_;

    // ---------------------------------------------------------------------------
    // MsQuic static callbacks
    // ---------------------------------------------------------------------------
    static QUIC_STATUS QUIC_API ConnectionCallback(
        HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev);

    static QUIC_STATUS QUIC_API StreamCallback(
        HQUIC stream, void* ctx, QUIC_STREAM_EVENT* ev);

    // ---------------------------------------------------------------------------
    // Internal helpers
    // ---------------------------------------------------------------------------

    // Append raw bytes from the transport and attempt to parse complete frames.
    void OnData(const uint8_t* data, size_t len);

    // Parse as many complete [type:u16][len:u32][payload] frames as possible
    // from recv_buf_, pushing each into recv_queue_.
    void ProcessFrames();
};

} // namespace rco::net
