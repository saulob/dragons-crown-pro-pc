#include "net/peer_socket.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "net/crypto.h"
#include "net/stun.h"

#ifdef _WIN32
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <poll.h>
#endif

namespace dcp::net {
namespace {

// Datagram: 'D' 'N' version type, u32 destination link tag, u32 source link tag (little endian), then the body.
constexpr uint8_t kVersion = 1;
enum : uint8_t { kHello = 1, kHelloAck = 2, kData = 3, kAck = 4, kPing = 5, kPong = 6, kClose = 7, kPunch = 8 };
enum : uint8_t { kFirst = 1, kLast = 2 };
constexpr size_t kHeader = 12;
constexpr size_t kHelloSize = kHeader + 20 + 16; // accounts, nonce, MAC
constexpr size_t kDataHeader = kHeader + 13;     // seq, ack, sack, flags
constexpr size_t kFragment = 1150;               // datagrams stay under 1200 bytes
constexpr size_t kMaxQueued = 4 * PeerSocket::kMaxMessage;
constexpr uint32_t kReceiveWindow = 2048;
constexpr uint32_t kMaxWindow = 512, kMinWindow = 16;

void Put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
void Put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t Get64(const uint8_t* p) { return uint64_t(Get32(p)) | uint64_t(Get32(p + 4)) << 32; }

void Header(uint8_t* p, uint8_t type, uint32_t dest, uint32_t src) {
    p[0] = 'D', p[1] = 'N', p[2] = kVersion, p[3] = type;
    Put32(p + 4, dest);
    Put32(p + 8, src);
}

uint32_t RandomTag() {
    uint32_t v = 0;
    while (v == 0) random_bytes(&v, sizeof(v));
    return v;
}

bool SameBytes(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i];
    return d == 0;
}

std::string Candidates(const std::vector<Endpoint>& list) {
    std::string out;
    for (const Endpoint& e : list) out += (out.empty() ? "" : ", ") + endpoint_text(e);
    return out.empty() ? "none" : out;
}

} // namespace

std::unique_ptr<PeerSocket> PeerSocket::open(const Options& options, std::string* error) {
    if (!sockets_ready()) {
        if (error) *error = "sockets are unavailable";
        return nullptr;
    }
    const socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) {
        if (error) *error = "cannot create a UDP socket (error " + std::to_string(last_socket_error()) + ")";
        return nullptr;
    }
#ifdef _WIN32
    // Without this, an ICMP "port unreachable" for any datagram fails the next receive on the whole socket.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
    const int buffer = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    sockaddr_in local = to_sockaddr({0, options.port});
    if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        local = to_sockaddr({0, 0});
        if (options.port) std::printf("P2P: UDP port %u is in use; the system picks another\n", options.port);
        if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            if (error) *error = "cannot bind a UDP port (error " + std::to_string(last_socket_error()) + ")";
            close_socket(s);
            return nullptr;
        }
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &length);
    set_nonblocking(s);
    std::unique_ptr<PeerSocket> p(new PeerSocket());
    p->socket_ = s;
    p->port_ = ntohs(bound.sin_port);
    p->options_ = options;
    if (!options.stun_servers.empty()) {
        p->stun_state_ = StunState::kRunning;
        const auto servers = options.stun_servers;
        PeerSocket* self = p.get();
        p->stun_resolver_ = std::thread([self, servers] {
            std::vector<Endpoint> addresses;
            for (const auto& [host, port] : servers)
                if (const uint32_t ip = resolve_ipv4(host)) addresses.push_back({ip, port});
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->stun_addresses_ = std::move(addresses);
            self->stun_resolved_ = true;
        });
    }
    p->thread_ = std::thread([raw = p.get()] { raw->Run(); });
    std::printf("P2P: UDP socket on port %u\n", p->port_);
    return p;
}

PeerSocket::~PeerSocket() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, link] : links_)
            if (link.state != LinkState::kClosed) Close(link, "socket closed", true);
        stopping_ = true;
    }
    if (thread_.joinable()) thread_.join();
    if (stun_resolver_.joinable()) stun_resolver_.join();
    close_socket(socket_);
}

void PeerSocket::set_room(const RoomKey& key, uint64_t self_account) {
    std::lock_guard<std::mutex> lock(mutex_);
    room_key_ = key;
    self_ = self_account;
}

void PeerSocket::set_peer(uint64_t account, std::vector<Endpoint> candidates) {
    std::lock_guard<std::mutex> lock(mutex_);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const Endpoint& e) { return !e.valid(); }),
                     candidates.end());
    auto& known = peers_[account];
    if (known != candidates) std::printf("P2P: peer %llu at %s\n", (unsigned long long)account, Candidates(candidates).c_str());
    known = std::move(candidates);
}

void PeerSocket::forget_peer(uint64_t account) {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.erase(account);
}

uint64_t PeerSocket::peer_at(const Endpoint& e) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [account, list] : peers_)
        if (std::find(list.begin(), list.end(), e) != list.end()) return account;
    return 0;
}

PeerSocket::Link* PeerSocket::Find(int id) {
    auto it = links_.find(id);
    return it == links_.end() ? nullptr : &it->second;
}

const PeerSocket::Link* PeerSocket::Find(int id) const {
    auto it = links_.find(id);
    return it == links_.end() ? nullptr : &it->second;
}

PeerSocket::Link* PeerSocket::ByTag(uint32_t tag) {
    for (auto& [id, link] : links_)
        if (link.tag == tag) return &link;
    return nullptr;
}

PeerSocket::Link* PeerSocket::ByAccount(uint64_t account) {
    for (auto& [id, link] : links_)
        if (link.account == account && link.state != LinkState::kClosed) return &link;
    return nullptr;
}

PeerSocket::Link& PeerSocket::NewLink(uint64_t account, uint64_t now) {
    const int id = next_link_++;
    Link& link = links_[id];
    link.id = id;
    link.account = account;
    link.tag = RandomTag();
    link.nonce = RandomTag();
    link.started_ms = link.last_rx_ms = link.last_tx_ms = now;
    link.next_hello_ms = now;
    return link;
}

int PeerSocket::connect(uint64_t account) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!peers_.count(account)) return -1;
    Link* link = ByAccount(account);
    if (!link) {
        link = &NewLink(account, now_ms());
        std::printf("P2P: link %d to peer %llu: trying %s\n", link->id, (unsigned long long)account,
                    Candidates(peers_[account]).c_str());
    }
    link->wanted = true;
    return link->id;
}

LinkState PeerSocket::state(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Link* link = Find(id);
    return link ? link->state : LinkState::kClosed;
}

Endpoint PeerSocket::remote(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Link* link = Find(id);
    return link ? link->addr : Endpoint{};
}

bool PeerSocket::send(int id, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    Link* link = Find(id);
    if (!link || link->state != LinkState::kConnected || size > kMaxMessage || link->queued + size > kMaxQueued) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    link->outbox.emplace_back(p, p + size);
    link->queued += size;
    return true;
}

size_t PeerSocket::queued_bytes(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Link* link = Find(id);
    return link ? link->queued + link->unacked.size() * kFragment : 0;
}

bool PeerSocket::receive(int id, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    Link* link = Find(id);
    if (!link || link->inbox.empty()) return false;
    out = std::move(link->inbox.front());
    link->inbox.pop_front();
    return true;
}

bool PeerSocket::readable(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Link* link = Find(id);
    return link && !link->inbox.empty();
}

void PeerSocket::close(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Link* link = Find(id);
    if (!link) return;
    if (link->state != LinkState::kClosed) Close(*link, "closed by the game", true);
    links_.erase(id);
}

LinkStats PeerSocket::stats(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Link* link = Find(id);
    return link ? link->stats : LinkStats{};
}

StunState PeerSocket::stun(Endpoint* mapped) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mapped) *mapped = stun_mapped_;
    return stun_state_;
}

void PeerSocket::punch(const Endpoint& to) {
    uint8_t packet[kHeader];
    Header(packet, kPunch, 0, 0);
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < 3; ++i) SendRaw(to, packet, sizeof(packet));
}

void PeerSocket::SendRaw(const Endpoint& to, const void* data, size_t size) {
    const sockaddr_in a = to_sockaddr(to);
    sendto(socket_, static_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
}

void PeerSocket::SendHello(Link& link, const Endpoint& to, bool reply) {
    uint8_t packet[kHelloSize];
    Header(packet, reply ? kHelloAck : kHello, reply ? link.peer_tag : 0, link.tag);
    Put64(packet + 12, self_);
    Put64(packet + 20, link.account);
    Put32(packet + 28, link.nonce);
    const Digest mac = hmac_sha256(room_key_.data(), room_key_.size(), packet, 32);
    std::memcpy(packet + 32, mac.data(), 16);
    SendRaw(to, packet, sizeof(packet));
    link.last_tx_ms = now_ms();
}

void PeerSocket::SendAck(Link& link) {
    uint8_t packet[kHeader + 8];
    Header(packet, kAck, link.peer_tag, link.tag);
    uint32_t sack = 0;
    for (uint32_t i = 0; i < 32; ++i)
        if (link.early.count(link.expected + 1 + i)) sack |= 1u << i;
    Put32(packet + 12, link.expected);
    Put32(packet + 16, sack);
    SendRaw(link.addr, packet, sizeof(packet));
    link.ack_due = false;
    link.last_tx_ms = now_ms();
}

void PeerSocket::Close(Link& link, const std::string& why, bool tell_peer) {
    if (link.state == LinkState::kClosed) return;
    if (tell_peer && link.state == LinkState::kConnected) {
        uint8_t packet[kHeader];
        Header(packet, kClose, link.peer_tag, link.tag);
        for (int i = 0; i < 3; ++i) SendRaw(link.addr, packet, sizeof(packet));
    }
    std::printf("P2P: link %d to peer %llu closed: %s; in %llu msgs / %llu B, out %llu msgs / %llu B, %llu resends, rtt %u ms\n",
                link.id, (unsigned long long)link.account, why.c_str(), (unsigned long long)link.stats.messages_in,
                (unsigned long long)link.stats.bytes_in, (unsigned long long)link.stats.messages_out,
                (unsigned long long)link.stats.bytes_out, (unsigned long long)link.stats.resends, link.stats.rtt_ms);
    link.state = LinkState::kClosed;
    link.closed_why = why;
    link.outbox.clear();
    link.unacked.clear();
    link.queued = 0;
}

void PeerSocket::Handshake(const uint8_t* data, size_t size, const Endpoint& from, uint64_t now, bool reply) {
    if (size != kHelloSize || !self_) return;
    const Digest mac = hmac_sha256(room_key_.data(), room_key_.size(), data, 32);
    if (!SameBytes(mac.data(), data + 32, 16)) return;
    const uint32_t dest = Get32(data + 4), src = Get32(data + 8);
    const uint64_t sender = Get64(data + 12), target = Get64(data + 20);
    const uint32_t nonce = Get32(data + 28);
    if (target != self_ || !peers_.count(sender) || src == 0) return;
    Link* link = reply ? ByTag(dest) : ByAccount(sender);
    if (reply && (!link || link->account != sender || link->state == LinkState::kClosed)) return;
    if (link && link->state == LinkState::kConnected && link->peer_nonce && link->peer_nonce != nonce) {
        if (reply) return;
        Close(*link, "the peer started a new connection", false);
        link = nullptr;
    }
    if (!link) link = &NewLink(sender, now);
    link->peer_tag = src;
    link->peer_nonce = nonce;
    link->last_rx_ms = now;
    if (link->state == LinkState::kConnecting) {
        link->state = LinkState::kConnected;
        link->addr = from;
        link->addr_rx_ms = now;
        std::printf("P2P: link %d to peer %llu connected via %s after %llu ms\n", link->id, (unsigned long long)sender,
                    endpoint_text(from).c_str(), (unsigned long long)(now - link->started_ms));
    }
    if (!reply) SendHello(*link, from, true);
}

void PeerSocket::Acked(Link& link, uint32_t ack, uint32_t sack, uint64_t now) {
    auto sample = [&link, now](const Packet& p) {
        if (p.tries != 1) return;
        const double s = double(now - p.sent_ms);
        if (link.srtt == 0) {
            link.srtt = s, link.rttvar = s / 2;
        } else {
            link.rttvar = 0.75 * link.rttvar + 0.25 * std::fabs(link.srtt - s);
            link.srtt = 0.875 * link.srtt + 0.125 * s;
        }
        link.rto_ms = uint32_t(std::clamp(link.srtt + 4 * link.rttvar, 100.0, 2000.0));
        link.stats.rtt_ms = uint32_t(link.srtt);
        link.stats.rtt_sampled = true;
    };
    while (!link.unacked.empty() && int32_t(link.unacked.front().seq - ack) < 0) {
        if (!link.unacked.front().acked) sample(link.unacked.front());
        link.unacked.pop_front();
        if (link.window < kMaxWindow) ++link.window;
    }
    int after = 0;
    for (Packet& p : link.unacked) {
        const uint32_t bit = p.seq - ack - 1;
        if (p.seq != ack && bit < 32 && (sack >> bit & 1) && !p.acked) {
            sample(p);
            p.acked = true;
        }
        if (p.acked) ++after;
    }
    while (!link.unacked.empty() && link.unacked.front().acked) link.unacked.pop_front();
    // Three later packets arrived without the first: it was lost; resend it now rather than at its timeout.
    if (after >= 3 && !link.unacked.empty() && link.unacked.front().tries == 1) {
        Packet& p = link.unacked.front();
        SendRaw(link.addr, p.bytes.data(), p.bytes.size());
        p.sent_ms = now;
        ++p.tries;
        ++link.stats.resends;
        link.window = std::max(kMinWindow, link.window / 2);
    }
}

void PeerSocket::Data(Link& link, const uint8_t* body, size_t size, uint64_t now) {
    if (size < 13) return;
    const uint32_t seq = Get32(body), ack = Get32(body + 4), sack = Get32(body + 8);
    const uint8_t flags = body[12];
    Acked(link, ack, sack, now);
    link.ack_due = true;
    if (int32_t(seq - link.expected) < 0 || seq - link.expected >= kReceiveWindow) return;
    link.early.emplace(seq, std::make_pair(flags, std::vector<uint8_t>(body + 13, body + size)));
    for (auto it = link.early.find(link.expected); it != link.early.end(); it = link.early.find(link.expected)) {
        const auto [fragment_flags, payload] = std::move(it->second);
        link.early.erase(it);
        ++link.expected;
        if (fragment_flags & kFirst) link.assembling.clear();
        link.assembling.insert(link.assembling.end(), payload.begin(), payload.end());
        link.stats.bytes_in += payload.size();
        if (link.assembling.size() > kMaxMessage) {
            Close(link, "the peer sent a message larger than the game accepts", true);
            return;
        }
        if (fragment_flags & kLast) {
            link.inbox.push_back(std::move(link.assembling));
            link.assembling.clear();
            ++link.stats.messages_in;
        }
    }
}

void PeerSocket::Receive(const uint8_t* data, size_t size, const Endpoint& from, uint64_t now) {
    if (stun::looks_like_stun(data, size)) {
        Endpoint mapped;
        if (stun_state_ == StunState::kRunning && stun::parse_response(data, size, stun_id_, mapped)) {
            stun_state_ = StunState::kDone;
            stun_mapped_ = mapped;
            std::printf("P2P: STUN: public endpoint %s (local port %u, server %s)\n", endpoint_text(mapped).c_str(), port_,
                        endpoint_text(from).c_str());
        }
        return;
    }
    if (size < kHeader || data[0] != 'D' || data[1] != 'N' || data[2] != kVersion) return;
    const uint8_t type = data[3];
    if (type == kHello || type == kHelloAck) return Handshake(data, size, from, now, type == kHelloAck);
    if (type == kPunch) return;
    Link* link = ByTag(Get32(data + 4));
    if (!link || link->state != LinkState::kConnected || link->peer_tag != Get32(data + 8)) return;
    link->last_rx_ms = now;
    if (from == link->addr) {
        link->addr_rx_ms = now;
    } else if (now - link->addr_rx_ms > 3000) {
        std::printf("P2P: link %d: peer moved to %s\n", link->id, endpoint_text(from).c_str());
        link->addr = from;
        link->addr_rx_ms = now;
    }
    switch (type) {
    case kData: Data(*link, data + kHeader, size - kHeader, now); break;
    case kAck:
        if (size >= kHeader + 8) Acked(*link, Get32(data + 12), Get32(data + 16), now);
        break;
    case kPing: {
        uint8_t packet[kHeader + 4];
        Header(packet, kPong, link->peer_tag, link->tag);
        std::memcpy(packet + kHeader, data + kHeader, size >= kHeader + 4 ? 4 : 0);
        SendRaw(link->addr, packet, sizeof(packet));
        break;
    }
    case kClose: Close(*link, "the peer closed the connection", false); break;
    default: break;
    }
}

void PeerSocket::Flush(Link& link, uint64_t now) {
    uint32_t sack = 0;
    for (uint32_t i = 0; i < 32; ++i)
        if (link.early.count(link.expected + 1 + i)) sack |= 1u << i;
    bool lost = false;
    for (Packet& p : link.unacked) {
        if (p.acked) continue;
        const uint64_t wait = uint64_t(link.rto_ms) << std::min(p.tries - 1, 4);
        if (now - p.sent_ms < wait) continue;
        if (p.tries >= 30) {
            Close(link, "the peer stopped acknowledging data", false);
            return;
        }
        Put32(p.bytes.data() + kHeader + 4, link.expected);
        Put32(p.bytes.data() + kHeader + 8, sack);
        SendRaw(link.addr, p.bytes.data(), p.bytes.size());
        p.sent_ms = now;
        ++p.tries;
        ++link.stats.resends;
        lost = true;
    }
    if (lost) link.window = std::max(kMinWindow, link.window / 2);
    bool sent = false;
    while (link.unacked.size() < link.window && !link.outbox.empty()) {
        std::vector<uint8_t>& message = link.outbox.front();
        const size_t offset = link.outbox_offset;
        const size_t n = std::min(kFragment, message.size() - offset);
        uint8_t flags = 0;
        if (offset == 0) flags |= kFirst;
        if (offset + n == message.size()) flags |= kLast;
        Packet p;
        p.seq = link.next_seq++;
        p.bytes.resize(kDataHeader + n);
        Header(p.bytes.data(), kData, link.peer_tag, link.tag);
        Put32(p.bytes.data() + kHeader, p.seq);
        Put32(p.bytes.data() + kHeader + 4, link.expected);
        Put32(p.bytes.data() + kHeader + 8, sack);
        p.bytes[kHeader + 12] = flags;
        if (n) std::memcpy(p.bytes.data() + kDataHeader, message.data() + offset, n);
        p.sent_ms = now;
        p.tries = 1;
        SendRaw(link.addr, p.bytes.data(), p.bytes.size());
        link.unacked.push_back(std::move(p));
        link.stats.bytes_out += n;
        sent = true;
        if (flags & kLast) {
            link.queued -= message.size();
            link.outbox.pop_front();
            link.outbox_offset = 0;
            ++link.stats.messages_out;
        } else {
            link.outbox_offset = offset + n;
        }
    }
    if (sent) {
        link.ack_due = false;
        link.last_tx_ms = now;
    }
}

void PeerSocket::Timers(uint64_t now) {
    for (auto it = links_.begin(); it != links_.end();) {
        Link& link = it->second;
        if (link.state == LinkState::kClosed && !link.wanted) {
            it = links_.erase(it);
            continue;
        }
        if (link.state == LinkState::kConnecting) {
            if (now - link.started_ms > uint64_t(options_.connect_timeout_ms)) {
                Close(link, "no answer from the peer (the networks could not be joined)", false);
            } else if (now >= link.next_hello_ms) {
                const auto peer = peers_.find(link.account);
                if (peer != peers_.end())
                    for (const Endpoint& e : peer->second) SendHello(link, e, false);
                link.next_hello_ms = now + (now - link.started_ms < 3000 ? 100 : 250);
            }
        } else if (link.state == LinkState::kConnected) {
            if (now - link.last_rx_ms > uint64_t(options_.idle_timeout_ms)) {
                Close(link, "no traffic from the peer for " + std::to_string(options_.idle_timeout_ms / 1000) + " s", false);
            } else {
                Flush(link, now);
                if (link.state == LinkState::kConnected) {
                    if (link.ack_due) SendAck(link);
                    if (now - link.last_tx_ms >= 1000) {
                        uint8_t packet[kHeader + 4];
                        Header(packet, kPing, link.peer_tag, link.tag);
                        Put32(packet + kHeader, uint32_t(now));
                        SendRaw(link.addr, packet, sizeof(packet));
                        link.last_tx_ms = now;
                    }
                }
            }
        }
        ++it;
    }
}

void PeerSocket::StunStep(uint64_t now) {
    if (stun_state_ != StunState::kRunning || !stun_resolved_) return;
    if (stun_index_ >= stun_addresses_.size()) {
        stun_state_ = StunState::kFailed;
        std::printf("P2P: STUN: no server answered; peers will use the lobby's view of this address\n");
        return;
    }
    if (stun_attempt_ == 0) stun_server_deadline_ms_ = now + 3000;
    if (now >= stun_server_deadline_ms_ && stun_attempt_ > 0) {
        ++stun_index_;
        stun_attempt_ = 0;
        return;
    }
    if (now < stun_next_ms_) return;
    if (stun_attempt_ == 0) random_bytes(stun_id_.data(), stun_id_.size());
    uint8_t request[stun::kRequestSize];
    stun::make_request(stun_id_, request);
    SendRaw(stun_addresses_[stun_index_], request, sizeof(request));
    static constexpr uint64_t kGaps[] = {250, 500, 1000, 1000};
    stun_next_ms_ = now + kGaps[std::min(stun_attempt_, 3)];
    ++stun_attempt_;
}

void PeerSocket::Run() {
    std::vector<uint8_t> buffer(2048);
    for (;;) {
#ifdef _WIN32
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket_, &read_set);
        timeval tv{0, 5000};
        select(0, &read_set, nullptr, nullptr, &tv);
#else
        pollfd pfd{socket_, POLLIN, 0};
        poll(&pfd, 1, 5);
#endif
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        const uint64_t now = now_ms();
        for (int i = 0; i < 512; ++i) {
            sockaddr_in from{};
            socklen_t length = sizeof(from);
            const int n = int(recvfrom(socket_, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
                                       reinterpret_cast<sockaddr*>(&from), &length));
            if (n < 0) {
                if (would_block(last_socket_error())) break;
                continue;
            }
            Receive(buffer.data(), size_t(n), from_sockaddr(from), now);
        }
        StunStep(now);
        Timers(now);
    }
}

} // namespace dcp::net
