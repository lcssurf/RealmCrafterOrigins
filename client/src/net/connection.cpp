#include "connection.h"

#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace rco::net {

// ---------------------------------------------------------------------------
// ALPN token used in the TLS handshake.  Server must advertise the same value.
// ---------------------------------------------------------------------------
static const QUIC_BUFFER kAlpn = {
    static_cast<uint32_t>(sizeof("rco") - 1),
    reinterpret_cast<uint8_t*>(const_cast<char*>("rco"))
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

Connection::Connection() = default;

Connection::~Connection() {
    Disconnect();
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

bool Connection::Connect(const std::string& host, uint16_t port) {
    if (connected_.load()) {
        return true;  // already connected
    }

    QUIC_STATUS status;

    // 1. Open the MsQuic API table.
    status = MsQuicOpen2(&api_);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[net] MsQuicOpen2 failed: 0x%x\n", status);
        return false;
    }

    // 2. Create a registration (one per application).
    QUIC_REGISTRATION_CONFIG reg_cfg{};
    reg_cfg.AppName  = "rco";
    reg_cfg.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;

    status = api_->RegistrationOpen(&reg_cfg, &registration_);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[net] RegistrationOpen failed: 0x%x\n", status);
        MsQuicClose(api_);
        api_ = nullptr;
        return false;
    }

    // 3. Create a configuration (one per connection profile).
    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs        = 30000;
    settings.IsSet.IdleTimeoutMs  = TRUE;
    settings.PeerBidiStreamCount  = 1;
    settings.IsSet.PeerBidiStreamCount = TRUE;

    status = api_->ConfigurationOpen(
        registration_,
        &kAlpn, 1,
        &settings, sizeof(settings),
        nullptr,
        &configuration_);

    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[net] ConfigurationOpen failed: 0x%x\n", status);
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
        MsQuicClose(api_);
        api_ = nullptr;
        return false;
    }

    // 4. Credential config — client, no cert validation (dev mode).
    QUIC_CREDENTIAL_CONFIG cred_cfg{};
    cred_cfg.Type  = QUIC_CREDENTIAL_TYPE_NONE;
    cred_cfg.Flags = QUIC_CREDENTIAL_FLAG_CLIENT
                   | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

    status = api_->ConfigurationLoadCredential(configuration_, &cred_cfg);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[net] ConfigurationLoadCredential failed: 0x%x\n", status);
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
        MsQuicClose(api_);
        api_ = nullptr;
        return false;
    }

    // 5. Open a connection object.
    status = api_->ConnectionOpen(
        registration_,
        &Connection::ConnectionCallback,
        this,
        &connection_);

    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[net] ConnectionOpen failed: 0x%x\n", status);
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
        MsQuicClose(api_);
        api_ = nullptr;
        return false;
    }

    // 6. Start the connection (async — ConnectionCallback fires on connected).
    status = api_->ConnectionStart(
        connection_,
        configuration_,
        QUIC_ADDRESS_FAMILY_UNSPEC,
        host.c_str(),
        port);

    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[net] ConnectionStart failed: 0x%x\n", status);
        api_->ConnectionClose(connection_);
        connection_ = nullptr;
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
        MsQuicClose(api_);
        api_ = nullptr;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Disconnect
// ---------------------------------------------------------------------------

void Connection::Disconnect() {
    connected_.store(false);

    if (api_) {
        if (stream_) {
            api_->StreamShutdown(stream_, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            api_->StreamClose(stream_);
            stream_ = nullptr;
        }
        if (connection_) {
            api_->ConnectionShutdown(connection_, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            api_->ConnectionClose(connection_);
            connection_ = nullptr;
        }
        if (configuration_) {
            api_->ConfigurationClose(configuration_);
            configuration_ = nullptr;
        }
        if (registration_) {
            api_->RegistrationClose(registration_);
            registration_ = nullptr;
        }
        MsQuicClose(api_);
        api_ = nullptr;
    }

    // Drain queues
    {
        std::lock_guard<std::mutex> lk(recv_mutex_);
        while (!recv_queue_.empty()) recv_queue_.pop();
    }
    recv_buf_.clear();
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

void Connection::SendPacket(uint16_t type, const Writer& w) {
    auto frame = w.MakePacket(type);
    SendRaw(frame);
}

void Connection::SendRaw(const std::vector<uint8_t>& data) {
    if (!connected_.load() || !stream_) return;

    std::lock_guard<std::mutex> lk(send_mutex_);

    // MsQuic takes ownership of the QUIC_BUFFER until QUIC_STREAM_EVENT_SEND_COMPLETE.
    // We heap-allocate both the data copy and the buffer descriptor.
    auto* buf_data = new uint8_t[data.size()];
    std::memcpy(buf_data, data.data(), data.size());

    auto* qbuf  = new QUIC_BUFFER{};
    qbuf->Buffer = buf_data;
    qbuf->Length = static_cast<uint32_t>(data.size());

    // Context pointer: we store qbuf so we can free both in SEND_COMPLETE.
    QUIC_STATUS status = api_->StreamSend(
        stream_,
        qbuf, 1,
        QUIC_SEND_FLAG_NONE,
        qbuf  /* send context == qbuf ptr */);

    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[net] StreamSend failed: 0x%x\n", status);
        delete[] buf_data;
        delete qbuf;
    }
}

// ---------------------------------------------------------------------------
// Poll
// ---------------------------------------------------------------------------

bool Connection::Poll(InboundPacket& out) {
    std::lock_guard<std::mutex> lk(recv_mutex_);
    if (recv_queue_.empty()) return false;
    out = std::move(recv_queue_.front());
    recv_queue_.pop();
    return true;
}

// ---------------------------------------------------------------------------
// Internal: OnData / ProcessFrames
// ---------------------------------------------------------------------------

void Connection::OnData(const uint8_t* data, size_t len) {
    recv_buf_.insert(recv_buf_.end(), data, data + len);
    ProcessFrames();
}

void Connection::ProcessFrames() {
    // Frame layout: [uint16 type LE][uint32 payloadLen LE][payload...]
    constexpr size_t kHdr = 6;

    while (recv_buf_.size() >= kHdr) {
        const uint8_t* p = recv_buf_.data();

        uint16_t type =
            static_cast<uint16_t>(p[0]) |
            static_cast<uint16_t>(p[1]) << 8;

        uint32_t payload_len =
            static_cast<uint32_t>(p[2])        |
            static_cast<uint32_t>(p[3]) << 8   |
            static_cast<uint32_t>(p[4]) << 16  |
            static_cast<uint32_t>(p[5]) << 24;

        // Sanity check — 4 MB max
        if (payload_len > 4 * 1024 * 1024) {
            std::fprintf(stderr, "[net] Oversized packet (%u bytes), disconnecting\n", payload_len);
            Disconnect();
            return;
        }

        if (recv_buf_.size() < kHdr + payload_len) {
            break;  // not enough data yet
        }

        InboundPacket pkt;
        pkt.type = type;
        pkt.payload.assign(p + kHdr, p + kHdr + payload_len);

        {
            std::lock_guard<std::mutex> lk(recv_mutex_);
            recv_queue_.push(std::move(pkt));
        }

        // Consume the frame from the buffer
        recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin() + kHdr + payload_len);
    }
}

// ---------------------------------------------------------------------------
// MsQuic connection callback
// ---------------------------------------------------------------------------

QUIC_STATUS QUIC_API Connection::ConnectionCallback(
    HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev)
{
    auto* self = static_cast<Connection*>(ctx);
    (void)conn;

    switch (ev->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED: {
            std::printf("[net] QUIC connected\n");
            self->connected_.store(true);

            // Open the client-initiated bidirectional stream.
            QUIC_STATUS s = self->api_->StreamOpen(
                self->connection_,
                QUIC_STREAM_OPEN_FLAG_NONE,
                &Connection::StreamCallback,
                self,
                &self->stream_);

            if (QUIC_FAILED(s)) {
                std::fprintf(stderr, "[net] StreamOpen failed: 0x%x\n", s);
                break;
            }

            s = self->api_->StreamStart(self->stream_, QUIC_STREAM_START_FLAG_NONE);
            if (QUIC_FAILED(s)) {
                std::fprintf(stderr, "[net] StreamStart failed: 0x%x\n", s);
            }
            break;
        }

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            std::printf("[net] QUIC disconnected\n");
            self->connected_.store(false);
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            std::printf("[net] QUIC shutdown complete\n");
            self->connected_.store(false);
            break;

        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            // Server opened a stream toward us — accept it for reading.
            self->api_->SetCallbackHandler(
                ev->PEER_STREAM_STARTED.Stream,
                reinterpret_cast<void*>(&Connection::StreamCallback),
                self);
            break;

        default:
            break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// MsQuic stream callback
// ---------------------------------------------------------------------------

QUIC_STATUS QUIC_API Connection::StreamCallback(
    HQUIC stream, void* ctx, QUIC_STREAM_EVENT* ev)
{
    auto* self = static_cast<Connection*>(ctx);
    (void)stream;

    switch (ev->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
                const QUIC_BUFFER& b = ev->RECEIVE.Buffers[i];
                self->OnData(b.Buffer, b.Length);
            }
            break;
        }

        case QUIC_STREAM_EVENT_SEND_COMPLETE: {
            // Free the heap-allocated QUIC_BUFFER + data we created in SendRaw.
            auto* qbuf = static_cast<QUIC_BUFFER*>(ev->SEND_COMPLETE.ClientContext);
            if (qbuf) {
                delete[] qbuf->Buffer;
                delete qbuf;
            }
            break;
        }

        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            // Server closed its send side — gracefully close ours.
            self->api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
            break;

        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            self->connected_.store(false);
            break;

        default:
            break;
    }

    return QUIC_STATUS_SUCCESS;
}

} // namespace rco::net
