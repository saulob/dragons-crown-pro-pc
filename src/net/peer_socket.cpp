#include "net/peer_socket.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <optional>

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
// Random loss keeps halving the window; at the floor a 500 ms path still carries about 120 packets/s.
constexpr uint32_t kMaxWindow = 512, kMinWindow = 64;
// An ACK reports 32 packets past its cumulative point; nothing is known about later ones, so their timers do not fire
// until the oldest outstanding packet moves (they had almost always arrived).
constexpr uint32_t kSackSpan = 32;
constexpr uint32_t kRelayWindow = 64; // a moved link restarts below the relay budget's burst
// A CLOSE may carry one reason byte; older clients send none and ignore it.
enum : uint8_t { kCloseUnspecified = 0, kCloseByGame = 1, kCloseBySocket = 2, kCloseProtocol = 3 };
constexpr uint64_t kPingEveryMs = 1000;
constexpr double kRtoMarginMs = 50; // a steady path drives rttvar to 0; without a floor every jitter is a timeout
// Link quality: judged each second over the last five seconds of DATA (docs/ONLINE.md, "Link quality").
constexpr uint64_t kQualityPeriodMs = 1000;
constexpr uint32_t kMinJudgedPackets = 20;
constexpr double kDegradedResendShare = 0.08, kSevereResendShare = 0.18;
constexpr uint64_t kCriticalSilenceMs = 2500;
// Severe resends must outlast the 5 s window, so one outage's retransmissions alone never count as sustained. Long
// degradation escalates only when delivery itself stalled in some of those seconds: steady loss that RUDP hides stays
// direct.
constexpr int kDegradedAfter = 3, kCriticalAfter = 2, kSevereAfter = 8, kEscalateAfter = 15, kSlowSeconds = 3, kHealthyAfter = 5;
// Relay probe pings carry this bit so their PONG is told apart from the 1 s pings of the same link.
constexpr uint32_t kProbeMark = 0x80000000u;
constexpr uint64_t kRelayProofMs = 10000;
constexpr size_t kMoveBurst = 32;
// Below the lobby's per-connection relay budget (server/lobby/src/relay.js: 400 frames/s, 128 KB/s, bursts 800 and
// 256 KB): a frame it would drop counts toward closing the relay for this connection.
constexpr double kRelayFramesPerSecond = 300, kRelayFrameBurst = 600;
constexpr double kRelayBytesPerSecond = 96 * 1024, kRelayByteBurst = 192 * 1024;
// DATA leaves this much for ACK and PING/PONG, so a saturated relay keeps acknowledging and measuring.
constexpr double kRelayControlFrames = 60, kRelayControlBytes = 8 * 1024;

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

uint32_t RtoFor(double srtt, double rttvar) {
    return uint32_t(std::clamp(srtt + std::max(4 * rttvar, kRtoMarginMs), 100.0, 2000.0));
}

std::string PeerClosed(const uint8_t* data, size_t size) {
    switch (size > kHeader ? data[kHeader] : uint8_t(kCloseUnspecified)) {
    case kCloseByGame: return "the peer's game closed the connection";
    case kCloseBySocket: return "the peer's P2P socket closed";
    case kCloseProtocol: return "the peer refused our data (a message larger than the game accepts)";
    default: return "the peer closed the connection";
    }
}

const char* QualityName(LinkQuality q) {
    switch (q) {
    case LinkQuality::kHealthy: return "healthy";
    case LinkQuality::kDegraded: return "degraded";
    case LinkQuality::kCritical: return "critical";
    case LinkQuality::kRecovering: return "recovering";
    }
    return "?";
}

std::string Fixed(const char* format, double value) {
    char text[32];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

} // namespace

EndpointChoice choose_endpoint(uint32_t lobby_ip, bool lobby_on_lan, uint16_t local_port, StunState stun, const Endpoint& mapped,
                               const Endpoint& upnp) {
    if (lobby_on_lan) return {EndpointSource::kLan, local_port, lobby_ip};
    const bool stun_done = stun == StunState::kDone;
    uint32_t ip = lobby_ip;
    if (!ip && stun_done && classify(mapped.ip) == AddressKind::kPublic) ip = mapped.ip;
    if (!ip && classify(upnp.ip) == AddressKind::kPublic) ip = upnp.ip;
    if (!ip) return {EndpointSource::kRelayFallback, local_port, 0};
    if (upnp.valid() && upnp.ip == ip) return {EndpointSource::kUpnp, upnp.port, ip};
    if (stun_done) return {EndpointSource::kStun, mapped.port, ip};
    return {EndpointSource::kLobby, local_port, ip};
}

std::string endpoint_choice_text(const EndpointChoice& choice, bool relay_available) {
    const std::string at = endpoint_text({choice.ip, choice.port});
    switch (choice.source) {
    case EndpointSource::kLan:
        return choice.ip ? "reporting LAN endpoint " + at : "reporting LAN endpoint on port " + std::to_string(choice.port);
    case EndpointSource::kUpnp: return "reporting public UPnP endpoint " + at;
    case EndpointSource::kStun: return "reporting public STUN endpoint " + at;
    case EndpointSource::kLobby: return "reporting lobby-observed endpoint " + at;
    case EndpointSource::kRelayFallback: break;
    }
    return relay_available ? "public endpoint unavailable, relay fallback" : "public endpoint unavailable, relay not offered";
}

std::string lobby_endpoint_text(const std::string& public_ip, const std::string& observed_ip) {
    return public_ip.empty() ? observed_ip : "observed " + observed_ip + ", public " + public_ip;
}

std::unique_ptr<PeerSocket> PeerSocket::open(const Options& options, std::string* error) {
    if (options.simulated_network) {
        std::unique_ptr<PeerSocket> p(new PeerSocket());
        p->options_ = options;
        p->simulated_ = true;
        p->port_ = options.port;
        return p;
    }
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
    sockaddr_in local = to_sockaddr({options.bind_ip, options.port});
    if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        local = to_sockaddr({options.bind_ip, 0});
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
            if (link.state != LinkState::kClosed) Close(link, "socket closed", true, kCloseBySocket);
        stopping_ = true;
    }
    if (thread_.joinable()) thread_.join();
    if (stun_resolver_.joinable()) stun_resolver_.join();
    if (socket_ != kNoSocket) close_socket(socket_);
}

uint64_t PeerSocket::Now() const { return simulated_ ? simulated_now_ : now_ms(); }

void PeerSocket::simulated_clock(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    simulated_now_ = now;
}

void PeerSocket::simulated_datagram(const uint8_t* data, size_t size, const Endpoint& from) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (simulated_) Receive(data, size, from, simulated_now_);
}

void PeerSocket::simulated_tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!simulated_) return;
    StunStep(simulated_now_);
    Timers(simulated_now_);
}

void PeerSocket::set_room(const RoomKey& key, uint64_t self_account) {
    std::lock_guard<std::mutex> lock(mutex_);
    room_key_ = key;
    self_ = self_account;
}

void PeerSocket::set_peer(uint64_t account, std::vector<Endpoint> candidates, uint16_t relay_member) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (relay_member) {
        for (auto it = relay_members_.begin(); it != relay_members_.end();)
            it = it->second == relay_member && it->first != account ? relay_members_.erase(it) : std::next(it);
        relay_members_[account] = relay_member;
    } else {
        relay_members_.erase(account);
    }
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const Endpoint& e) { return !e.valid(); }),
                     candidates.end());
    auto& known = peers_[account];
    if (known != candidates) std::printf("P2P: peer %llu at %s\n", (unsigned long long)account, Candidates(candidates).c_str());
    known = std::move(candidates);
}

void PeerSocket::forget_peer(uint64_t account) {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.erase(account);
    relay_members_.erase(account);
}

uint64_t PeerSocket::peer_at(const Endpoint& e) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_relay_endpoint(e)) {
        for (const auto& [account, member] : relay_members_)
            if (member == e.port) return account;
        return 0;
    }
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
    link.stats.rto_ms = link.rto_ms;
    return link;
}

int PeerSocket::connect(uint64_t account) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!peers_.count(account)) return -1;
    Link* link = ByAccount(account);
    if (!link) {
        link = &NewLink(account, Now());
        if (HasDirect(account)) {
            std::printf("P2P: link %d to peer %llu: trying %s\n", link->id, (unsigned long long)account,
                        Candidates(peers_[account]).c_str());
        } else {
            std::printf("P2P: link %d to peer %llu: no direct candidate; relay %s\n", link->id, (unsigned long long)account,
                        relay_send_ && relay_members_.count(account) ? "enabled" : "unavailable");
        }
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
    link->outbox_ms.push_back(Now());
    link->queued += size;
    return true;
}

size_t PeerSocket::queued_bytes(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Link* link = Find(id);
    return link ? link->queued + (link->unacked.size() - link->sacked) * kFragment : 0;
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
    if (link->state != LinkState::kClosed) Close(*link, "closed by the game", true, kCloseByGame);
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

void PeerSocket::enable_relay(uint32_t token, RelaySend send) {
    std::lock_guard<std::mutex> lock(mutex_);
    relay_token_ = token;
    relay_send_ = std::move(send);
    relay_bucket_ms_ = 0;
}

void PeerSocket::disable_relay() {
    std::lock_guard<std::mutex> lock(mutex_);
    relay_send_ = nullptr;
}

bool PeerSocket::relay_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bool(relay_send_);
}

RelayStats PeerSocket::relay_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return relay_stats_;
}

LinkPath PeerSocket::path(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Link* link = Find(id);
    if (!link || link->state == LinkState::kClosed) return LinkPath::kFailed;
    if (link->state == LinkState::kConnected) return is_relay_endpoint(link->addr) ? LinkPath::kRelay : LinkPath::kDirect;
    return link->relay_offered ? LinkPath::kRelayOffered : LinkPath::kConnectingDirect;
}

bool PeerSocket::deliver_relay(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::optional<RelayFrame> frame = decode_relay_frame(data, size);
    uint16_t known = 0;
    if (frame && relay_send_ && frame->token == relay_token_) {
        for (const auto& [account, member] : relay_members_)
            if (member == frame->member) known = member;
    }
    if (!known || stun::looks_like_stun(frame->payload.data(), frame->payload.size())) {
        ++relay_stats_.rejected;
        return false;
    }
    ++relay_stats_.frames_in;
    relay_stats_.bytes_in += frame->payload.size();
    Receive(frame->payload.data(), frame->payload.size(), relay_endpoint(known), Now());
    return true;
}

bool PeerSocket::HasDirect(uint64_t account) const {
    const auto peer = peers_.find(account);
    return peer != peers_.end() && !peer->second.empty();
}

bool PeerSocket::RelayReachable(const Link& link, uint64_t now) const {
    return CanRelay(link) && (!HasDirect(link.account) || now - link.started_ms >= uint64_t(options_.relay_after_ms));
}

bool PeerSocket::CanRelay(const Link& link) const { return relay_send_ && relay_members_.count(link.account); }

bool PeerSocket::FromRelayOf(const Link& link, const Endpoint& from) const {
    if (!is_relay_endpoint(from)) return false;
    const auto member = relay_members_.find(link.account);
    return member != relay_members_.end() && member->second == from.port;
}

bool PeerSocket::RelayBudget(size_t frame_bytes, uint64_t now, bool data) {
    if (!relay_bucket_ms_) {
        relay_frame_tokens_ = kRelayFrameBurst;
        relay_byte_tokens_ = kRelayByteBurst;
        relay_bucket_ms_ = now;
    }
    const double seconds = double(now - relay_bucket_ms_) / 1000.0;
    relay_bucket_ms_ = now;
    relay_frame_tokens_ = std::min(kRelayFrameBurst, relay_frame_tokens_ + seconds * kRelayFramesPerSecond);
    relay_byte_tokens_ = std::min(kRelayByteBurst, relay_byte_tokens_ + seconds * kRelayBytesPerSecond);
    const double frames_kept = data ? kRelayControlFrames : 0, bytes_kept = data ? kRelayControlBytes : 0;
    if (relay_frame_tokens_ < 1 + frames_kept || relay_byte_tokens_ < double(frame_bytes) + bytes_kept) return false;
    relay_frame_tokens_ -= 1;
    relay_byte_tokens_ -= double(frame_bytes);
    return true;
}

void PeerSocket::SendRelay(uint16_t member, const void* data, size_t size) {
    std::vector<uint8_t> frame;
    if (relay_send_) {
        frame = encode_relay_frame(relay_token_, member, ++relay_seq_, static_cast<const uint8_t*>(data), size);
    }
    const bool is_data = size > 3 && static_cast<const uint8_t*>(data)[3] == kData;
    if (!frame.empty() && !RelayBudget(frame.size(), Now(), is_data)) {
        ++relay_stats_.throttled;
        return;
    }
    if (frame.empty() || !relay_send_(std::move(frame))) {
        ++relay_stats_.send_failures;
        return;
    }
    ++relay_stats_.frames_out;
    relay_stats_.bytes_out += size;
}

void PeerSocket::SendRaw(const Endpoint& to, const void* data, size_t size) {
    if (is_relay_endpoint(to)) return SendRelay(to.port, data, size);
    if (simulated_) return options_.simulated_network(to, static_cast<const uint8_t*>(data), size);
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
    link.last_tx_ms = Now();
}

void PeerSocket::SendAck(Link& link) {
    uint8_t packet[kHeader + 8];
    Header(packet, kAck, link.peer_tag, link.tag);
    Put32(packet + 12, link.expected);
    Put32(packet + 16, Sack(link));
    SendRaw(link.addr, packet, sizeof(packet));
    link.ack_due = false;
    link.last_tx_ms = Now();
}

void PeerSocket::SendPing(Link& link, const Endpoint& to, uint32_t stamp) {
    uint8_t packet[kHeader + 4];
    Header(packet, kPing, link.peer_tag, link.tag);
    Put32(packet + kHeader, stamp);
    SendRaw(to, packet, sizeof(packet));
    link.last_tx_ms = Now();
}

std::string PeerSocket::Transport(const Link& link) const {
    if (!is_relay_endpoint(link.addr)) return "direct";
    return link.health.moved ? "relay (moved from direct)" : "relay";
}

void PeerSocket::Close(Link& link, const std::string& why, bool tell_peer, uint8_t code) {
    if (link.state == LinkState::kClosed) return;
    if (tell_peer && link.state == LinkState::kConnected) {
        uint8_t packet[kHeader + 1];
        Header(packet, kClose, link.peer_tag, link.tag);
        packet[kHeader] = code;
        for (int i = 0; i < 3; ++i) SendRaw(link.addr, packet, sizeof(packet));
    }
    const LinkStats& s = link.stats;
    const double resent = s.packets_out ? 100.0 * double(s.resends) / double(s.packets_out) : 0.0;
    std::printf("P2P: link %d to peer %llu closed: %s; transport %s, in %llu msgs / %llu B, out %llu msgs / %llu B in %llu "
                "packets, %llu resends (%s%% of packets), srtt %s\n",
                link.id, (unsigned long long)link.account, why.c_str(), Transport(link).c_str(),
                (unsigned long long)s.messages_in, (unsigned long long)s.bytes_in, (unsigned long long)s.messages_out,
                (unsigned long long)s.bytes_out, (unsigned long long)s.packets_out, (unsigned long long)s.resends,
                Fixed("%.1f", resent).c_str(), s.rtt_sampled ? (std::to_string(s.rtt_ms) + " ms").c_str() : "n/a (never measured)");
    link.state = LinkState::kClosed;
    link.closed_why = why;
    link.outbox.clear();
    link.outbox_ms.clear();
    link.unacked.clear();
    link.sacked = 0;
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
        link->last_progress_ms = now;
        link->next_ping_ms = now;
        link->health.next_check_ms = now + kQualityPeriodMs;
        std::printf("P2P: link %d to peer %llu connected via %s after %llu ms\n", link->id, (unsigned long long)sender,
                    is_relay_endpoint(from) ? "the relay" : endpoint_text(from).c_str(),
                    (unsigned long long)(now - link->started_ms));
    }
    if (!reply) SendHello(*link, from, true);
}

void PeerSocket::Sample(Link& link, double rtt_ms) {
    if (!link.stats.rtt_sampled) {
        link.srtt = rtt_ms, link.rttvar = rtt_ms / 2;
    } else {
        link.rttvar = 0.75 * link.rttvar + 0.25 * std::fabs(link.srtt - rtt_ms);
        link.srtt = 0.875 * link.srtt + 0.125 * rtt_ms;
    }
    link.rto_ms = RtoFor(link.srtt, link.rttvar);
    link.stats.rto_ms = link.rto_ms;
    link.stats.rtt_ms = uint32_t(link.srtt + 0.5);
    link.stats.rtt_sampled = true;
}

void PeerSocket::Pong(Link& link, uint32_t echo, uint64_t now) {
    Health& h = link.health;
    if (h.probe_stamp && echo == h.probe_stamp) {
        const bool first = !h.relay_ready_ms;
        h.relay_ready_ms = now;
        h.relay_rtt_ms = uint32_t(now) - (echo ^ kProbeMark);
        h.probe_stamp = 0;
        if (first)
            std::printf("P2P: link %d relay answered in %u ms; ready if the direct path fails\n", link.id, h.relay_rtt_ms);
        return;
    }
    // Unambiguous even when every DATA packet is being resent, as on a path slower than the RTO, which Karn's rule alone
    // would never measure again.
    const uint32_t rtt = uint32_t(now) - echo;
    if (rtt < 30000) Sample(link, double(rtt));
}

void PeerSocket::ShrinkWindow(Link& link, uint32_t lost_seq) {
    if (int32_t(lost_seq - link.recovery_seq) < 0) return;
    link.window = std::max(kMinWindow, link.window / 2);
    link.recovery_seq = link.next_seq;
}

// Due now: packets whose timers backed off while the path was silent, or the first ones after a path change.
void PeerSocket::Expedite(Link& link, bool everything) {
    size_t n = 0;
    for (Packet& p : link.unacked) {
        if (p.acked) continue;
        if (everything ? n++ < kMoveBurst : p.tries >= 2) p.due = true;
    }
}

uint32_t PeerSocket::Sack(const Link& link) {
    uint32_t sack = 0;
    for (uint32_t i = 0; i < kSackSpan; ++i)
        if (link.early.count(link.expected + 1 + i)) sack |= 1u << i;
    return sack;
}

void PeerSocket::Resend(Link& link, Packet& p, uint64_t now, uint32_t sack) {
    Put32(p.bytes.data() + kHeader + 4, link.expected);
    Put32(p.bytes.data() + kHeader + 8, sack);
    SendRaw(link.addr, p.bytes.data(), p.bytes.size());
    p.sent_ms = now;
    p.due = false;
    ++p.tries;
    ++link.stats.resends;
    ++link.health.resent;
}

// One RTT sample per ACK, from the newest packet it covers, and none when it also covers a retransmission: that ACK
// may have been released by a filled hole, so the packets it acknowledges waited behind the loss, not on the path.
// Only packets sent once date the loss check below: an ACK of a resent packet may be for its first copy.
void PeerSocket::Acked(Link& link, uint32_t ack, uint32_t sack, uint64_t now) {
    uint64_t newest_sent = 0;
    bool progress = false, covers_resend = false;
    auto delivered = [&](const Packet& p) {
        const bool clean = p.tries == 1 && !p.due;
        progress = true;
        covers_resend = covers_resend || !clean;
        if (clean) newest_sent = std::max(newest_sent, p.sent_ms);
    };
    while (!link.unacked.empty() && int32_t(link.unacked.front().seq - ack) < 0) {
        if (link.unacked.front().acked) --link.sacked;
        else delivered(link.unacked.front());
        link.unacked.pop_front();
        if (link.window < kMaxWindow) ++link.window;
    }
    for (Packet& p : link.unacked) {
        const uint32_t bit = p.seq - ack - 1;
        if (p.seq != ack && bit < kSackSpan && (sack >> bit & 1) && !p.acked) {
            delivered(p);
            p.acked = true;
            ++link.sacked;
        }
    }
    while (!link.unacked.empty() && link.unacked.front().acked) {
        link.unacked.pop_front();
        --link.sacked;
    }
    if (!progress) return;
    link.last_progress_ms = now;
    if (!newest_sent) return;
    if (!covers_resend) Sample(link, double(now - newest_sent));
    // Lost: a packet sent a quarter round trip later was delivered. This also catches a retransmission lost again.
    const double reorder = link.stats.rtt_sampled ? std::max(10.0, link.srtt / 4) : 50.0;
    if (link.unacked.empty()) return;
    const uint32_t front = link.unacked.front().seq, current_sack = Sack(link);
    uint32_t lost_seq = 0;
    bool lost = false;
    for (Packet& p : link.unacked) {
        if (p.seq - front > kSackSpan) break;
        if (p.acked || double(p.sent_ms) + reorder >= double(newest_sent)) continue;
        Resend(link, p, now, current_sack);
        lost = true;
        lost_seq = p.seq;
    }
    if (lost) ShrinkWindow(link, lost_seq);
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
            Close(link, "the peer sent a message larger than the game accepts", true, kCloseProtocol);
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
    const uint64_t silent_ms = now - link->last_rx_ms;
    link->last_rx_ms = now;
    if (silent_ms >= std::max<uint64_t>(link->rto_ms, 500)) Expedite(*link, false);
    const bool via_relay = FromRelayOf(*link, from);
    if (from == link->addr) {
        link->addr_rx_ms = now;
    } else if (!is_relay_endpoint(from) && !is_relay_endpoint(link->addr) && now - link->addr_rx_ms > 3000) {
        std::printf("P2P: link %d: peer moved to %s\n", link->id, endpoint_text(from).c_str());
        link->addr = from;
        link->addr_rx_ms = now;
    } else if (via_relay && !is_relay_endpoint(link->addr) && (type == kData || type == kAck) && CanRelay(*link)) {
        MoveToRelay(*link, now, "the peer moved this link to the relay");
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
        SendRaw(via_relay ? from : link->addr, packet, sizeof(packet));
        break;
    }
    case kPong:
        if (size >= kHeader + 4) Pong(*link, Get32(data + kHeader), now);
        break;
    case kClose: Close(*link, PeerClosed(data, size), false); break;
    default: break;
    }
}

void PeerSocket::Flush(Link& link, uint64_t now) {
    const uint32_t sack = Sack(link);
    bool lost = false;
    uint32_t lost_seq = 0;
    const uint32_t front = link.unacked.empty() ? 0 : link.unacked.front().seq;
    // Backoff is for a silent or congested path. While the peer keeps talking, a packet lost again is random loss and
    // waits at most two RTOs, so one unlucky packet cannot hold every later message for seconds.
    const int max_backoff = now - link.last_rx_ms < link.rto_ms ? 1 : 4;
    for (Packet& p : link.unacked) {
        if (p.seq - front > kSackSpan) break;
        if (p.acked) continue;
        const uint64_t wait = uint64_t(link.rto_ms) << std::min(p.tries - 1, max_backoff);
        if (!p.due && now - p.sent_ms < wait) continue;
        if (p.tries >= 30) {
            Close(link, Transport(link) + " path failed: the peer stopped acknowledging data", false);
            return;
        }
        Resend(link, p, now, sack);
        lost = true;
        lost_seq = p.seq;
    }
    if (lost) ShrinkWindow(link, lost_seq);
    bool sent = false;
    // The window counts packets not yet confirmed; ones SACKed behind a hole are already with the peer. The deque stays
    // well inside the peer's receive window either way.
    while (link.unacked.size() - link.sacked < link.window && link.unacked.size() < kReceiveWindow / 2 && !link.outbox.empty()) {
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
        if (link.unacked.empty()) link.outstanding_since_ms = now;
        link.unacked.push_back(std::move(p));
        link.stats.bytes_out += n;
        ++link.stats.packets_out;
        ++link.health.sent;
        sent = true;
        if (flags & kLast) {
            link.queued -= message.size();
            link.outbox.pop_front();
            link.outbox_ms.pop_front();
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

PeerSocket::Assessment PeerSocket::Measure(const Link& link, uint64_t now) const {
    Assessment a;
    for (const auto& [sent, resent] : link.health.seconds) a.sent += sent, a.resent += resent;
    if (a.sent >= kMinJudgedPackets) a.resend_share = double(a.resent) / double(a.sent + a.resent);
    if (!link.unacked.empty()) a.stall_ms = now - std::max(link.last_progress_ms, link.outstanding_since_ms);
    a.silence_ms = now - link.last_rx_ms;
    if (!link.outbox_ms.empty()) a.backlog_ms = now - link.outbox_ms.front();
    return a;
}

std::string PeerSocket::QualityText(const Link& link, const Assessment& a) const {
    std::string text = "transport " + Transport(link) + ", srtt ";
    text += link.stats.rtt_sampled ? std::to_string(link.stats.rtt_ms) + " ms" : "n/a";
    text += ", rto " + std::to_string(link.rto_ms) + " ms, resends ";
    text += a.resend_share >= 0 ? Fixed("%.1f", 100.0 * a.resend_share) + "%" : "n/a";
    text += " of " + std::to_string(a.sent + a.resent) + " sends in 5 s, ack stall " + std::to_string(a.stall_ms) +
            " ms, rx silence " + std::to_string(a.silence_ms) + " ms, send queue " + std::to_string(a.backlog_ms) + " ms";
    return text;
}

// One judged second. Thresholds and their reasons: docs/ONLINE.md, "Link quality".
void PeerSocket::Assess(Link& link, uint64_t now) {
    Health& h = link.health;
    h.next_check_ms = now + kQualityPeriodMs;
    h.seconds[h.slot] = {h.sent, h.resent};
    h.slot = (h.slot + 1) % h.seconds.size();
    h.sent = h.resent = 0;
    const Assessment a = Measure(link, now);
    const double srtt = link.stats.rtt_sampled ? link.srtt : 0;
    // Waits measured against the round trip, so a slow but steady path is not judged by its latency.
    const uint64_t long_wait = uint64_t(std::max(2000.0, 4 * srtt)), wait = uint64_t(std::max(1000.0, 2 * srtt));
    const bool critical = a.stall_ms >= long_wait || a.backlog_ms >= long_wait || a.silence_ms >= kCriticalSilenceMs;
    const bool severe = a.resend_share >= kSevereResendShare;
    const bool slow = a.stall_ms >= wait || a.backlog_ms >= wait;
    const bool degraded = critical || severe || slow || a.resend_share >= kDegradedResendShare;
    h.bad = degraded ? h.bad + 1 : 0;
    h.critical = critical ? h.critical + 1 : 0;
    h.severe = severe ? h.severe + 1 : 0;
    h.slow = degraded ? h.slow + slow : 0;
    h.good = degraded ? 0 : h.good + 1;
    if (h.moved && !h.move_confirmed && link.last_progress_ms > h.moved_ms) {
        h.move_confirmed = true;
        std::printf("P2P: link %d migrated to the relay; session preserved (%s)\n", link.id, QualityText(link, a).c_str());
    }
    const LinkQuality before = h.state;
    if (h.state == LinkQuality::kHealthy && h.bad >= kDegradedAfter) h.state = LinkQuality::kDegraded;
    if ((h.state == LinkQuality::kDegraded || h.state == LinkQuality::kRecovering) &&
        (h.critical >= kCriticalAfter || h.severe >= kSevereAfter || (h.bad >= kEscalateAfter && h.slow >= kSlowSeconds)))
        h.state = LinkQuality::kCritical;
    if (h.state != LinkQuality::kHealthy && h.good >= kHealthyAfter) h.state = LinkQuality::kHealthy;
    link.stats.quality = h.state;
    if (h.state != before) {
        std::printf("P2P: link %d quality %s: %s\n", link.id,
                    h.state == LinkQuality::kHealthy ? "recovered" : QualityName(h.state), QualityText(link, a).c_str());
        h.stuck_logged = false;
    }
    if (h.state != LinkQuality::kCritical || is_relay_endpoint(link.addr)) return;
    if (CanRelay(link) && h.relay_ready_ms && now - h.relay_ready_ms <= kRelayProofMs) {
        MoveToRelay(link, now, "direct quality critical");
    } else if (!h.stuck_logged) {
        h.stuck_logged = true;
        std::printf("P2P: link %d direct quality critical; %s\n", link.id,
                    CanRelay(link) ? "the relay has not answered yet, staying direct" : "no relay for this peer, staying direct");
    }
}

// While a direct link shows trouble, a PING through the relay each second proves that path before it is needed. Its
// PONG may come back either way (older clients answer on the direct path): either proves the peer hears the relay.
void PeerSocket::PrepareRelay(Link& link, uint64_t now) {
    Health& h = link.health;
    if (is_relay_endpoint(link.addr) || !CanRelay(link) || (h.state == LinkQuality::kHealthy && h.bad == 0)) return;
    if (now < h.probe_due_ms || (h.relay_ready_ms && now - h.relay_ready_ms < kRelayProofMs / 2)) return;
    h.probe_due_ms = now + kPingEveryMs;
    h.probe_stamp = uint32_t(now) ^ kProbeMark;
    SendPing(link, relay_endpoint(relay_members_[link.account]), h.probe_stamp);
}

// The link's tags, sequence numbers, acknowledgements and buffers stay as they are: only the address changes, so
// nothing the game sees changes. Packets still in flight on the old path are told apart by sequence number.
void PeerSocket::MoveToRelay(Link& link, uint64_t now, const std::string& why) {
    Health& h = link.health;
    std::printf("P2P: link %d %s; switching to the relay (%s)\n", link.id, why.c_str(), QualityText(link, Measure(link, now)).c_str());
    link.addr = relay_endpoint(relay_members_[link.account]);
    link.addr_rx_ms = now;
    h.moved = true;
    h.moved_ms = now;
    h.state = LinkQuality::kRecovering;
    h.bad = h.critical = h.severe = h.slow = h.good = 0;
    h.seconds = {};
    h.sent = h.resent = 0;
    link.stats.quality = h.state;
    link.stats.moved_to_relay = true;
    if (h.relay_rtt_ms) {
        link.srtt = h.relay_rtt_ms;
        link.rttvar = h.relay_rtt_ms / 2.0;
        link.rto_ms = link.stats.rto_ms = RtoFor(link.srtt, link.rttvar);
        link.stats.rtt_ms = h.relay_rtt_ms;
    }
    link.recovery_seq = link.next_seq;
    link.window = std::min(link.window, kRelayWindow);
    link.next_ping_ms = now;
    Expedite(link, true);
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
                if (RelayReachable(link, now)) {
                    if (!link.relay_offered) {
                        link.relay_offered = true;
                        if (HasDirect(link.account)) {
                            std::printf("P2P: link %d to peer %llu: no direct answer after %d ms; relay offered beside direct\n",
                                        link.id, (unsigned long long)link.account, options_.relay_after_ms);
                        } else {
                            std::printf("P2P: link %d to peer %llu: no direct candidate; relay offered at once\n", link.id,
                                        (unsigned long long)link.account);
                        }
                    }
                    SendHello(link, relay_endpoint(relay_members_[link.account]), false);
                }
                link.next_hello_ms = now + (now - link.started_ms < 3000 ? 100 : 250);
            }
        } else if (link.state == LinkState::kConnected) {
            if (now - link.last_rx_ms > uint64_t(options_.idle_timeout_ms)) {
                std::string why = Transport(link) + " path timed out: no traffic from the peer for " +
                                  std::to_string(options_.idle_timeout_ms / 1000) + " s";
                if (!is_relay_endpoint(link.addr) && link.health.state == LinkQuality::kCritical)
                    why += CanRelay(link) ? " (relay fallback failed: the relay never answered)" : " (no relay for this peer)";
                Close(link, why, false);
            } else {
                Flush(link, now);
                if (link.state == LinkState::kConnected) {
                    if (link.ack_due) SendAck(link);
                    if (now >= link.next_ping_ms) {
                        SendPing(link, link.addr, uint32_t(now));
                        link.next_ping_ms = now + kPingEveryMs;
                    }
                    if (now >= link.health.next_check_ms) Assess(link, now);
                    PrepareRelay(link, now);
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
