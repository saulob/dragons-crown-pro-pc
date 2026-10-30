#include "net/stun.h"

#include <algorithm>
#include <cstring>

#include "net/crypto.h"

#ifndef _WIN32
#include <poll.h>
#endif

namespace dcp::net::stun {
namespace {

constexpr uint32_t kCookie = 0x2112a442;
constexpr uint16_t kBindingRequest = 0x0001, kBindingSuccess = 0x0101;
constexpr uint16_t kMappedAddress = 0x0001, kXorMappedAddress = 0x0020;

uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

} // namespace

void make_request(const TransactionId& id, uint8_t out[kRequestSize]) {
    out[0] = kBindingRequest >> 8, out[1] = kBindingRequest & 0xff;
    out[2] = out[3] = 0;
    out[4] = kCookie >> 24, out[5] = (kCookie >> 16) & 0xff, out[6] = (kCookie >> 8) & 0xff, out[7] = kCookie & 0xff;
    std::memcpy(out + 8, id.data(), id.size());
}

bool looks_like_stun(const uint8_t* data, size_t size) {
    return size >= 20 && (data[0] & 0xc0) == 0 && Be32(data + 4) == kCookie && (Be16(data + 2) & 3) == 0;
}

bool parse_response(const uint8_t* data, size_t size, const TransactionId& id, Endpoint& mapped) {
    if (!looks_like_stun(data, size) || Be16(data) != kBindingSuccess || std::memcmp(data + 8, id.data(), id.size())) {
        return false;
    }
    const size_t length = Be16(data + 2);
    if (20 + length > size) return false;
    bool found_plain = false, found_xor = false;
    Endpoint plain, xored;
    for (size_t at = 20; at + 4 <= 20 + length;) {
        const uint16_t type = Be16(data + at), attr = Be16(data + at + 2);
        const uint8_t* value = data + at + 4;
        if (at + 4 + attr > 20 + length) return false;
        if ((type == kMappedAddress || type == kXorMappedAddress) && attr >= 8 && value[1] == 0x01) {
            uint16_t port = Be16(value + 2);
            uint32_t ip = Be32(value + 4);
            if (type == kXorMappedAddress) {
                port ^= uint16_t(kCookie >> 16);
                ip ^= kCookie;
                xored = {ip, port}, found_xor = true;
            } else {
                plain = {ip, port}, found_plain = true;
            }
        }
        at += 4 + ((attr + 3u) & ~3u);
    }
    if (found_xor) mapped = xored;
    else if (found_plain) mapped = plain;
    return (found_xor || found_plain) && mapped.valid();
}

bool query(socket_t socket, const Endpoint& server, int timeout_ms, Endpoint& mapped) {
    TransactionId id;
    random_bytes(id.data(), id.size());
    uint8_t request[kRequestSize];
    make_request(id, request);
    const sockaddr_in to = to_sockaddr(server);
    const uint64_t deadline = now_ms() + uint64_t(timeout_ms);
    uint64_t next_send = 0;
    int attempt = 0;
    while (now_ms() < deadline) {
        if (now_ms() >= next_send) {
            sendto(socket, reinterpret_cast<const char*>(request), int(sizeof(request)), 0,
                   reinterpret_cast<const sockaddr*>(&to), sizeof(to));
            next_send = now_ms() + uint64_t(250 << std::min(attempt++, 3));
        }
        const uint64_t now = now_ms(), until = std::min(next_send, deadline);
        const int wait = until > now ? int(until - now) : 0;
#ifdef _WIN32
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket, &read_set);
        timeval tv{wait / 1000, (wait % 1000) * 1000};
        if (select(0, &read_set, nullptr, nullptr, &tv) <= 0) continue;
#else
        pollfd pfd{socket, POLLIN, 0};
        if (poll(&pfd, 1, wait) <= 0) continue;
#endif
        uint8_t buffer[1500];
        sockaddr_in from{};
        socklen_t length = sizeof(from);
        const int n = int(recvfrom(socket, reinterpret_cast<char*>(buffer), int(sizeof(buffer)), 0,
                                   reinterpret_cast<sockaddr*>(&from), &length));
        if (n > 0 && parse_response(buffer, size_t(n), id, mapped)) return true;
    }
    return false;
}

} // namespace dcp::net::stun
