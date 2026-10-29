// IPv4 endpoints, address classes and the sockets layer shared by the lobby link, STUN, UPnP and the peer transport.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dcp::net {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kNoSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kNoSocket = -1;
#endif

// Address and port in host order.
struct Endpoint {
    uint32_t ip = 0;
    uint16_t port = 0;

    bool valid() const { return ip != 0 && port != 0; }
    bool operator==(const Endpoint&) const = default;
};

enum class AddressKind { kInvalid, kLoopback, kPrivate, kLinkLocal, kCarrierNat, kReserved, kPublic };

AddressKind classify(uint32_t ip);
bool is_lan(uint32_t ip); // loopback, RFC 1918 or link-local
std::string ip_text(uint32_t ip);
std::string endpoint_text(const Endpoint& e);
// 0 when it is not a dotted IPv4 address.
uint32_t parse_ip(const std::string& text);

// Starts Winsock once per process (no-op elsewhere); false when it is unavailable.
bool sockets_ready();
void close_socket(socket_t s);
bool set_nonblocking(socket_t s);
int last_socket_error();
bool would_block(int error);

sockaddr_in to_sockaddr(const Endpoint& e);
Endpoint from_sockaddr(const sockaddr_in& a);

// IPv4 of a host name or dotted address; 0 when it does not resolve. Blocks.
uint32_t resolve_ipv4(const std::string& host);

// The local address the system would send Internet traffic from (no packet is sent); 0 when there is no route.
uint32_t default_route_ip();
// The machine's up IPv4 addresses that are not loopback.
std::vector<uint32_t> local_ipv4_addresses();

// Milliseconds of a monotonic clock.
uint64_t now_ms();

} // namespace dcp::net
