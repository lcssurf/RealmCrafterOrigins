#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cmath>

#include "protocol.h"

namespace rco::net {

// ---------------------------------------------------------------------------
// Writer — builds a payload buffer, then wraps it in a framed packet.
// All integers are written little-endian.
// ---------------------------------------------------------------------------
class Writer {
public:
    void WriteU8(uint8_t v) {
        buf_.push_back(v);
    }

    void WriteU16(uint16_t v) {
        buf_.push_back(static_cast<uint8_t>(v & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }

    void WriteU32(uint32_t v) {
        buf_.push_back(static_cast<uint8_t>(v & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    }

    void WriteF32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        WriteU32(bits);
    }

    void WriteBool(bool v) {
        WriteU8(v ? 1 : 0);
    }

    // String: [uint16 len][bytes] — no null terminator
    void WriteString(const std::string& s) {
        const uint16_t len = static_cast<uint16_t>(s.size());
        WriteU16(len);
        buf_.insert(buf_.end(),
                    reinterpret_cast<const uint8_t*>(s.data()),
                    reinterpret_cast<const uint8_t*>(s.data()) + len);
    }

    const uint8_t* Data() const { return buf_.data(); }
    size_t         Size() const { return buf_.size(); }
    void           Reset()      { buf_.clear(); }

    // Returns a complete framed packet: [uint16 type LE][uint32 payloadLen LE][payload]
    std::vector<uint8_t> MakePacket(uint16_t type) const {
        const uint32_t payloadLen = static_cast<uint32_t>(buf_.size());
        std::vector<uint8_t> frame;
        frame.reserve(kHeaderSize + payloadLen);

        // type (LE)
        frame.push_back(static_cast<uint8_t>(type & 0xFF));
        frame.push_back(static_cast<uint8_t>((type >> 8) & 0xFF));
        // length (LE)
        frame.push_back(static_cast<uint8_t>(payloadLen & 0xFF));
        frame.push_back(static_cast<uint8_t>((payloadLen >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>((payloadLen >> 16) & 0xFF));
        frame.push_back(static_cast<uint8_t>((payloadLen >> 24) & 0xFF));
        // payload
        frame.insert(frame.end(), buf_.begin(), buf_.end());
        return frame;
    }

private:
    std::vector<uint8_t> buf_;
};

// ---------------------------------------------------------------------------
// Reader — decodes a payload buffer.
// All integers are read little-endian.
// ---------------------------------------------------------------------------
class Reader {
public:
    Reader(const uint8_t* data, size_t size)
        : data_(data), size_(size), pos_(0), error_(false) {}

    uint8_t ReadU8() {
        if (pos_ + 1 > size_) { error_ = true; return 0; }
        return data_[pos_++];
    }

    uint16_t ReadU16() {
        if (pos_ + 2 > size_) { error_ = true; return 0; }
        uint16_t v = static_cast<uint16_t>(data_[pos_])
                   | static_cast<uint16_t>(data_[pos_ + 1]) << 8;
        pos_ += 2;
        return v;
    }

    uint32_t ReadU32() {
        if (pos_ + 4 > size_) { error_ = true; return 0; }
        uint32_t v = static_cast<uint32_t>(data_[pos_])
                   | static_cast<uint32_t>(data_[pos_ + 1]) << 8
                   | static_cast<uint32_t>(data_[pos_ + 2]) << 16
                   | static_cast<uint32_t>(data_[pos_ + 3]) << 24;
        pos_ += 4;
        return v;
    }

    float ReadF32() {
        uint32_t bits = ReadU32();
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    bool ReadBool() {
        return ReadU8() != 0;
    }

    // String: [uint16 len][bytes]
    std::string ReadString() {
        uint16_t len = ReadU16();
        if (pos_ + len > size_) { error_ = true; return {}; }
        std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return s;
    }

    bool OK()   const { return !error_; }
    bool Done() const { return pos_ >= size_; }

private:
    const uint8_t* data_;
    size_t         size_;
    size_t         pos_;
    bool           error_;
};

} // namespace rco::net
