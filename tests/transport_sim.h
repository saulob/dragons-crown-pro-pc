// A deterministic two-player network for net::PeerSocket (Options::simulated_network): virtual time in 5 ms ticks, a
// direct path with a fixed round trip, seeded loss and outages, and a relay path through a stand-in for the lobby that
// enforces the lobby's per-connection budget. The game side sends numbered messages and checks what arrives.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "net/addr.h"
#include "net/peer_socket.h"
#include "net/relay_frame.h"

namespace dcp::net::sim {

class Random {
public:
    explicit Random(uint64_t seed) : state_(seed) {}
    uint64_t Next() {
        uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    bool Percent(double p) { return p > 0 && double(Next() % 1000000) < p * 10000.0; }

private:
    uint64_t state_;
};

// One direction of game traffic as the receiving end saw it.
struct Delivery {
    uint64_t sent = 0, delivered = 0, out_of_order = 0, duplicates = 0;
    uint64_t max_latency_ms = 0, max_gap_ms = 0, late = 0; // late: delivered more than 1 s after it was written
};

// The lobby's per-connection relay budget (server/lobby/src/relay.js RELAY.connection).
struct LobbyBucket {
    double frames = 800, bytes = 256 * 1024;
    uint64_t at = 0;
    bool Take(uint64_t now, size_t size) {
        const double s = double(now - at) / 1000.0;
        at = now;
        frames = std::min(800.0, frames + s * 400);
        bytes = std::min(256.0 * 1024, bytes + s * 128 * 1024);
        if (frames < 1 || bytes < double(size)) return false;
        frames -= 1, bytes -= double(size);
        return true;
    }
};

class Network {
public:
    static constexpr uint64_t kTick = 5;
    static constexpr uint32_t kToken = 0x5a5a1234;

    struct End {
        std::unique_ptr<PeerSocket> socket;
        uint64_t account = 0;
        uint16_t member = 0;
        Endpoint addr;
        int link = -1;
        uint32_t next_index = 0, expected = 0;
        uint64_t next_send_ms = 0, next_big_ms = 0, last_rx_ms = 0;
        std::deque<std::vector<uint8_t>> backlog;
        Delivery in;
        LobbyBucket lobby;
        uint64_t relay_overruns = 0;
        LinkQuality worst = LinkQuality::kHealthy;
        bool reached_degraded = false, reached_critical = false;
        uint32_t degraded_srtt = 0; // the round trip when the link was first judged degraded
        uint64_t moved_at = 0;
    };

    // Knobs a scenario may change while it runs.
    uint32_t rtt_ms;
    double loss_percent;
    uint32_t jitter_ms;
    uint32_t relay_extra_ms = 30;
    bool relay_paused = false;     // the lobby link is reconnecting: frames are refused at the source
    bool relay_forwarding = true;  // false: the lobby swallows frames
    std::vector<std::pair<uint64_t, uint64_t>> outages;
    uint64_t direct_dead_from = UINT64_MAX;
    int messages_per_second = 40;
    size_t message_bytes = 220, big_bytes = 3000;
    uint64_t big_every_ms = 2000;
    bool traffic = true;

    Network(uint32_t rtt, double loss, uint64_t seed, bool relay = true)
        : rtt_ms(rtt), loss_percent(loss), jitter_ms(rtt / 20), random_(seed), relay_(relay) {
        ends_[0].account = 1001, ends_[0].member = 0x10, ends_[0].addr = {0x0a000001, 41000};
        ends_[1].account = 2002, ends_[1].member = 0x20, ends_[1].addr = {0x0a000002, 42000};
        for (int i = 0; i < 2; ++i) {
            PeerSocket::Options options;
            options.port = ends_[i].addr.port;
            options.simulated_network = [this, i](const Endpoint& to, const uint8_t* data, size_t size) {
                Direct(i, to, data, size);
            };
            std::string error;
            ends_[i].socket = PeerSocket::open(options, &error);
        }
    }

    ~Network() {
        for (End& e : ends_) e.socket->disable_relay();
        for (End& e : ends_) e.socket.reset();
    }

    End& a() { return ends_[0]; }
    End& b() { return ends_[1]; }
    uint64_t now() const { return now_; }

    bool Connect() {
        RoomKey key{};
        key[0] = 0x5a, key[1] = 0x5a, key[2] = 0x12, key[3] = 0x34;
        for (int i = 0; i < 2; ++i) {
            End& self = ends_[i];
            const End& other = ends_[1 - i];
            self.socket->set_room(key, self.account);
            self.socket->set_peer(other.account, {other.addr}, relay_ ? other.member : uint16_t(0));
            if (relay_) self.socket->enable_relay(kToken, [this, i](std::vector<uint8_t>&& frame) { return Relay(i, std::move(frame)); });
        }
        Clock();
        for (End& e : ends_) e.link = e.socket->connect(ends_[&e == &ends_[0] ? 1 : 0].account);
        const uint64_t deadline = now_ + 20000;
        while (now_ < deadline) {
            Step(false);
            if (Connected()) return true;
        }
        return false;
    }

    bool Connected() const {
        return ends_[0].socket->state(ends_[0].link) == LinkState::kConnected &&
               ends_[1].socket->state(ends_[1].link) == LinkState::kConnected;
    }

    bool Closed() const {
        return ends_[0].socket->state(ends_[0].link) == LinkState::kClosed ||
               ends_[1].socket->state(ends_[1].link) == LinkState::kClosed;
    }

    void Run(uint64_t ms) {
        const uint64_t until = now_ + ms;
        while (now_ < until) Step(traffic);
    }

    // Stops writing and gives what is in flight time to arrive.
    void Drain(uint64_t ms = 15000) {
        const bool was = traffic;
        traffic = false;
        const uint64_t until = now_ + ms;
        while (now_ < until && !(Backlogged() == 0 && Settled())) Step(false);
        traffic = was;
    }

    bool DirectDown(uint64_t at) const {
        if (at >= direct_dead_from) return true;
        for (const auto& [from, to] : outages)
            if (at >= from && at < to) return true;
        return false;
    }

    uint64_t direct_dropped = 0, relay_frames = 0;

private:
    struct Datagram {
        uint64_t at = 0, order = 0;
        int to = 0;
        bool relay = false;
        Endpoint from;
        std::vector<uint8_t> bytes;
    };
    struct Later {
        bool operator()(const Datagram& x, const Datagram& y) const { return x.at != y.at ? x.at > y.at : x.order > y.order; }
    };

    void Direct(int from, const Endpoint& to, const uint8_t* data, size_t size) {
        if (!(to == ends_[1 - from].addr)) return;
        if (DirectDown(now_) || random_.Percent(loss_percent)) {
            ++direct_dropped;
            return;
        }
        const uint64_t delay = rtt_ms / 2 + (jitter_ms ? random_.Next() % (jitter_ms + 1) : 0);
        queue_.push({now_ + delay, order_++, 1 - from, false, ends_[from].addr, std::vector<uint8_t>(data, data + size)});
    }

    bool Relay(int from, std::vector<uint8_t>&& frame) {
        if (relay_paused) return false;
        End& sender = ends_[from];
        if (!sender.lobby.Take(now_, frame.size())) {
            ++sender.relay_overruns;
            return true;
        }
        const auto decoded = decode_relay_frame(frame.data(), frame.size());
        if (!relay_forwarding || !decoded || decoded->token != kToken || decoded->member != ends_[1 - from].member) return true;
        ++relay_frames;
        std::vector<uint8_t> out =
            encode_relay_frame(decoded->token, sender.member, decoded->seq, decoded->payload.data(), decoded->payload.size());
        queue_.push({now_ + rtt_ms / 2 + relay_extra_ms, order_++, 1 - from, true, {}, std::move(out)});
        return true;
    }

    void Clock() {
        for (End& e : ends_) e.socket->simulated_clock(now_);
    }

    void Step(bool write) {
        now_ += kTick;
        Clock();
        while (!queue_.empty() && queue_.top().at <= now_) {
            const Datagram d = queue_.top();
            queue_.pop();
            End& to = ends_[d.to];
            if (d.relay) to.socket->deliver_relay(d.bytes.data(), d.bytes.size());
            else to.socket->simulated_datagram(d.bytes.data(), d.bytes.size(), d.from);
        }
        for (End& e : ends_) {
            if (write) Write(e);
            Flush(e);
            e.socket->simulated_tick();
            Read(e);
            Observe(e);
        }
    }

    void Write(End& e) {
        if (e.socket->state(e.link) != LinkState::kConnected) return;
        if (!e.next_send_ms) e.next_send_ms = e.next_big_ms = now_;
        while (now_ >= e.next_send_ms) {
            e.backlog.push_back(Message(e, message_bytes));
            e.next_send_ms += 1000 / uint64_t(std::max(1, messages_per_second));
        }
        if (big_every_ms && now_ >= e.next_big_ms) {
            e.backlog.push_back(Message(e, big_bytes));
            e.next_big_ms += big_every_ms;
        }
    }

    std::vector<uint8_t> Message(End& e, size_t size) {
        std::vector<uint8_t> m(std::max<size_t>(size, 12));
        const uint32_t index = e.next_index++;
        std::memcpy(m.data(), &index, 4);
        std::memcpy(m.data() + 4, &now_, 8);
        for (size_t i = 12; i < m.size(); ++i) m[i] = uint8_t(index + i);
        return m;
    }

    void Flush(End& e) {
        while (!e.backlog.empty() && e.socket->send(e.link, e.backlog.front().data(), e.backlog.front().size())) {
            e.backlog.pop_front();
            ++ends_[&e == &ends_[0] ? 1 : 0].in.sent;
        }
    }

    void Read(End& e) {
        std::vector<uint8_t> m;
        while (e.socket->receive(e.link, m)) {
            if (m.size() < 12) continue;
            uint32_t index;
            uint64_t sent_at;
            std::memcpy(&index, m.data(), 4);
            std::memcpy(&sent_at, m.data() + 4, 8);
            bool intact = true;
            for (size_t i = 12; i < m.size(); ++i) intact = intact && m[i] == uint8_t(index + i);
            if (!intact || index > e.expected) ++e.in.out_of_order;
            if (index < e.expected) {
                ++e.in.duplicates;
                continue;
            }
            e.expected = index + 1;
            ++e.in.delivered;
            const uint64_t latency = now_ - sent_at;
            e.in.max_latency_ms = std::max(e.in.max_latency_ms, latency);
            if (latency > 1000) ++e.in.late;
            if (e.last_rx_ms) e.in.max_gap_ms = std::max(e.in.max_gap_ms, now_ - e.last_rx_ms);
            e.last_rx_ms = now_;
        }
    }

    void Observe(End& e) {
        const LinkStats s = e.socket->stats(e.link);
        const bool degraded = s.quality == LinkQuality::kDegraded || s.quality == LinkQuality::kCritical;
        if (degraded && !e.reached_degraded) e.degraded_srtt = s.rtt_ms;
        e.reached_degraded = e.reached_degraded || degraded;
        e.reached_critical = e.reached_critical || s.quality == LinkQuality::kCritical;
        if (s.moved_to_relay && !e.moved_at) e.moved_at = now_;
    }

    size_t Backlogged() const { return ends_[0].backlog.size() + ends_[1].backlog.size(); }
    bool Settled() const {
        return ends_[0].in.delivered == ends_[0].in.sent && ends_[1].in.delivered == ends_[1].in.sent;
    }

    End ends_[2];
    Random random_;
    bool relay_;
    uint64_t now_ = 1000000;
    uint64_t order_ = 0;
    std::priority_queue<Datagram, std::vector<Datagram>, Later> queue_;
};

} // namespace dcp::net::sim
