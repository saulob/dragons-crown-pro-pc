// STUN Binding (RFC 5389, IPv4): the public address and port a NAT gives a UDP socket.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "net/addr.h"

namespace dcp::net::stun {

using TransactionId = std::array<uint8_t, 12>;

inline constexpr size_t kRequestSize = 20;

void make_request(const TransactionId& id, uint8_t out[kRequestSize]);
// True for anything shaped like a STUN message (the peer transport's own datagrams never are).
bool looks_like_stun(const uint8_t* data, size_t size);
// The mapped address of a Binding success response to `id` (XOR-MAPPED-ADDRESS preferred over MAPPED-ADDRESS).
bool parse_response(const uint8_t* data, size_t size, const TransactionId& id, Endpoint& mapped);

// Asks one server from `socket` (blocking, up to `timeout_ms`); for the connection test.
bool query(socket_t socket, const Endpoint& server, int timeout_ms, Endpoint& mapped);

} // namespace dcp::net::stun
