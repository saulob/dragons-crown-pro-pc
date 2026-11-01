// A PeerSocket datagram carried over the lobby's WebSocket as one binary message:
// "DR", version, type, room token (u32), member (u16), sequence (u32), payload length (u16), payload; big-endian.
// Sent, `member` is the destination; delivered, the lobby has replaced it with the sender.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dcp::net {

inline constexpr uint8_t kRelayVersion = 1;
inline constexpr uint8_t kRelayData = 1;
inline constexpr size_t kRelayHeaderBytes = 16;
inline constexpr size_t kRelayMaxPayload = 1200;

struct RelayFrame {
    uint32_t token = 0;
    uint16_t member = 0;
    uint32_t seq = 0;
    std::vector<uint8_t> payload;
};

enum class RelayFrameError { kNone, kTruncated, kBadMagic, kBadVersion, kBadType, kOversized, kBadLength };

const char* relay_frame_error_name(RelayFrameError error);

// Empty for a payload that is empty or over kRelayMaxPayload.
std::vector<uint8_t> encode_relay_frame(uint32_t token, uint16_t member, uint32_t seq, const uint8_t* payload, size_t size);
std::optional<RelayFrame> decode_relay_frame(const uint8_t* data, size_t size, RelayFrameError* error = nullptr);

// The room's relay credential: the first four bytes of its key, which only its members hold.
uint32_t relay_token(const std::array<uint8_t, 16>& room_key);

} // namespace dcp::net
