#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#include "net/addr.h"
#include "net/stun.h"

namespace dcp::net {

// A loopback STUN server: answers each Binding request with `mapped`, or the sender's own address when that is invalid,
// after staying silent for `silent_ms`. It remembers the port of the last sender.
class FakeStun {
public:
    explicit FakeStun(const Endpoint& mapped = {}, uint64_t silent_ms = 0)
        : mapped_(mapped), answer_from_(std::chrono::steady_clock::now() + std::chrono::milliseconds(silent_ms)) {
        s_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const sockaddr_in local = to_sockaddr({parse_ip("127.0.0.1"), 0});
        bind(s_, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
        sockaddr_in bound{};
        socklen_t length = sizeof(bound);
        getsockname(s_, reinterpret_cast<sockaddr*>(&bound), &length);
        port_ = ntohs(bound.sin_port);
        set_nonblocking(s_);
        thread_ = std::thread([this] { Run(); });
    }
    ~FakeStun() {
        stop_ = true;
        thread_.join();
        close_socket(s_);
    }
    uint16_t port() const { return port_; }
    uint16_t last_source() const { return source_; }

private:
    void Run() {
        uint8_t buffer[1500];
        while (!stop_) {
            sockaddr_in from{};
            socklen_t length = sizeof(from);
            const int n = int(recvfrom(s_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &length));
            if (n < int(stun::kRequestSize)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const uint16_t port = ntohs(from.sin_port);
            const uint32_t ip = ntohl(from.sin_addr.s_addr);
            source_ = port;
            if (std::chrono::steady_clock::now() < answer_from_) continue;
            const Endpoint answer = mapped_.valid() ? mapped_ : Endpoint{ip, port};
            uint8_t response[32] = {0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xa4, 0x42};
            std::memcpy(response + 8, buffer + 8, 12);
            const uint16_t xport = answer.port ^ 0x2112;
            const uint32_t xip = answer.ip ^ 0x2112a442u;
            const uint8_t attr[] = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, uint8_t(xport >> 8), uint8_t(xport),
                                    uint8_t(xip >> 24), uint8_t(xip >> 16), uint8_t(xip >> 8), uint8_t(xip)};
            std::memcpy(response + 20, attr, sizeof(attr));
            sendto(s_, reinterpret_cast<const char*>(response), int(sizeof(response)), 0,
                   reinterpret_cast<const sockaddr*>(&from), sizeof(from));
        }
    }
    Endpoint mapped_;
    std::chrono::steady_clock::time_point answer_from_;
    socket_t s_;
    uint16_t port_ = 0;
    std::atomic<uint16_t> source_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace dcp::net
