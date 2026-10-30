#include "net/addr.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#endif

namespace dcp::net {

AddressKind classify(uint32_t ip) {
    const uint32_t a = ip >> 24, b = (ip >> 16) & 0xff;
    if (ip == 0) return AddressKind::kInvalid;
    if (a == 127) return AddressKind::kLoopback;
    if (a == 10 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168)) return AddressKind::kPrivate;
    if (a == 169 && b == 254) return AddressKind::kLinkLocal;
    if (a == 100 && b >= 64 && b < 128) return AddressKind::kCarrierNat;
    if (a == 0 || a >= 224) return AddressKind::kReserved;
    return AddressKind::kPublic;
}

bool is_lan(uint32_t ip) {
    const AddressKind k = classify(ip);
    return k == AddressKind::kLoopback || k == AddressKind::kPrivate || k == AddressKind::kLinkLocal;
}

std::string ip_text(uint32_t ip) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
    return buf;
}

std::string endpoint_text(const Endpoint& e) { return ip_text(e.ip) + ":" + std::to_string(e.port); }

uint32_t parse_ip(const std::string& text) {
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        return 0;
    }
    return a << 24 | b << 16 | c << 8 | d;
}

bool sockets_ready() {
#ifdef _WIN32
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        WSADATA data;
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ready;
#else
    return true;
#endif
}

void close_socket(socket_t s) {
    if (s == kNoSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

bool set_nonblocking(socket_t s) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

int last_socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool would_block(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS;
#endif
}

sockaddr_in to_sockaddr(const Endpoint& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(e.port);
    a.sin_addr.s_addr = htonl(e.ip);
    return a;
}

Endpoint from_sockaddr(const sockaddr_in& a) { return {ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)}; }

uint32_t resolve_ipv4(const std::string& host) {
    if (const uint32_t ip = parse_ip(host)) return ip;
    if (!sockets_ready() || host.empty()) return 0;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) return 0;
    uint32_t ip = 0;
    for (const addrinfo* p = found; p && !ip; p = p->ai_next)
        if (p->ai_family == AF_INET) ip = ntohl(reinterpret_cast<const sockaddr_in*>(p->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(found);
    return ip;
}

uint32_t default_route_ip() {
    if (!sockets_ready()) return 0;
    const socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return 0;
    // Connecting a UDP socket only picks the route; nothing is sent.
    const sockaddr_in target = to_sockaddr({parse_ip("192.0.2.1"), 9});
    uint32_t ip = 0;
    if (connect(s, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == 0) {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &length) == 0) ip = ntohl(local.sin_addr.s_addr);
    }
    close_socket(s);
    return ip;
}

std::vector<uint32_t> local_ipv4_addresses() {
    std::vector<uint32_t> out;
    if (!sockets_ready()) return out;
    auto add = [&out](uint32_t ip) {
        if (ip && classify(ip) != AddressKind::kLoopback && std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
    };
    add(default_route_ip());
#ifdef _WIN32
    char name[256];
    if (gethostname(name, sizeof(name)) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* found = nullptr;
        if (getaddrinfo(name, nullptr, &hints, &found) == 0) {
            for (const addrinfo* p = found; p; p = p->ai_next)
                if (p->ai_family == AF_INET) add(ntohl(reinterpret_cast<const sockaddr_in*>(p->ai_addr)->sin_addr.s_addr));
            freeaddrinfo(found);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (const ifaddrs* p = list; p; p = p->ifa_next)
            if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && (p->ifa_flags & IFF_UP))
                add(ntohl(reinterpret_cast<const sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr));
        freeifaddrs(list);
    }
#endif
    return out;
}

uint64_t now_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace dcp::net
