// The players' direct link: one UDP socket carrying reliable, ordered, whole messages to each peer of the room
// (docs/ONLINE.md, "Peer transport"). Links start from both ends at once; every candidate address of a peer is tried
// and the first one that answers is kept. The socket also asks STUN servers for its public endpoint.
#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "net/addr.h"
#include "net/relay_frame.h"

namespace dcp::net {

enum class LinkState { kConnecting, kConnected, kClosed };
enum class StunState { kIdle, kRunning, kDone, kFailed };
// Where a link stands: direct candidates only, relay offered beside them, or the path that answered.
enum class LinkPath { kConnectingDirect, kRelayOffered, kDirect, kRelay, kFailed };

// A peer reached through the lobby's relay has this address (loopback, never a candidate the lobby hands out) and its
// room member as the port, so the link layer treats it like any other endpoint.
inline constexpr uint32_t kRelayAddress = 0x7F524C59;
inline Endpoint relay_endpoint(uint16_t member) { return {kRelayAddress, member}; }
inline bool is_relay_endpoint(const Endpoint& e) { return e.ip == kRelayAddress; }
// What the game is told about a room member: its public endpoint, or the relay's for that member when it has none.
inline Endpoint signaling_endpoint(const Endpoint& address, uint16_t member) {
    return address.valid() ? address : relay_endpoint(member);
}

enum class EndpointSource { kLan, kUpnp, kStun, kLobby, kRelayFallback };
// What goes to the lobby: `ip` is the IPv4 address peers will reach, 0 when there is none (peers then use the relay).
struct EndpointChoice {
    EndpointSource source = EndpointSource::kLobby;
    uint16_t port = 0;
    uint32_t ip = 0;

    bool operator==(const EndpointChoice&) const = default;
};
// `lobby_ip` is 0 when the lobby does not see an IPv4 address, `upnp` is invalid without a mapping.
EndpointChoice choose_endpoint(uint32_t lobby_ip, bool lobby_on_lan, uint16_t local_port, StunState stun, const Endpoint& mapped,
                               const Endpoint& upnp);
std::string endpoint_choice_text(const EndpointChoice& choice, bool relay_available);
// What peers dial, with the lobby's own view beside it when the two differ.
std::string lobby_endpoint_text(const std::string& public_ip, const std::string& observed_ip);

struct RelayStats {
    uint64_t frames_out = 0, bytes_out = 0, frames_in = 0, bytes_in = 0;
    uint64_t send_failures = 0, rejected = 0;
};

struct LinkStats {
    uint64_t messages_in = 0, messages_out = 0, bytes_in = 0, bytes_out = 0;
    uint64_t resends = 0;
    uint32_t rtt_ms = 0;
    bool rtt_sampled = false;
};

struct LinkView {
    LinkState state = LinkState::kClosed;
    LinkStats stats;
};

// The in-game Ping: the worst smoothed round trip among connected links that have measured one; -1 when none.
inline int worst_rtt_ms(const std::vector<LinkView>& links) {
    int worst = -1;
    for (const LinkView& link : links) {
        if (link.state == LinkState::kConnected && link.stats.rtt_sampled && int(link.stats.rtt_ms) > worst) {
            worst = int(link.stats.rtt_ms);
        }
    }
    return worst;
}

using RoomKey = std::array<uint8_t, 16>;

class PeerSocket {
public:
    // Largest message the game may send (its Rudp receive buffer).
    static constexpr size_t kMaxMessage = 0x40000;

    struct Options {
        uint16_t port = 0; // 0 or busy: the system picks one
        uint32_t bind_ip = 0; // 0: every interface
        std::vector<std::pair<std::string, uint16_t>> stun_servers;
        int connect_timeout_ms = 15000;
        int idle_timeout_ms = 10000;
        int relay_after_ms = 2500; // direct-only head start; the direct candidates keep being tried after it
    };

    // Hands one encoded relay frame to the lobby's connection; false when it cannot take it now.
    using RelaySend = std::function<bool(std::vector<uint8_t>&&)>;

    static std::unique_ptr<PeerSocket> open(const Options& options, std::string* error);
    ~PeerSocket();
    PeerSocket(const PeerSocket&) = delete;
    PeerSocket& operator=(const PeerSocket&) = delete;

    uint16_t port() const { return port_; }

    // The room's secret from the lobby and this player's account: handshakes prove both.
    void set_room(const RoomKey& key, uint64_t self_account);
    // A peer the lobby introduced, with its endpoints in the order to try; again with new endpoints when it moves.
    // `relay_member` is its room member, which the relay needs to reach it (0: not relayable).
    void set_peer(uint64_t account, std::vector<Endpoint> candidates, uint16_t relay_member = 0);
    void forget_peer(uint64_t account);
    // The peer one of whose endpoints is `e`; 0 when none.
    uint64_t peer_at(const Endpoint& e) const;

    // The link to a known peer, started now (or already started by the peer's own handshake); -1 when unknown.
    int connect(uint64_t account);
    LinkState state(int link) const;
    Endpoint remote(int link) const;
    // Queues one whole message; false when the link is not connected, the message is too large or the queue is full.
    bool send(int link, const void* data, size_t size);
    size_t queued_bytes(int link) const;
    bool receive(int link, std::vector<uint8_t>& out);
    bool readable(int link) const;
    void close(int link);
    LinkStats stats(int link) const;
    LinkPath path(int link) const;

    // The relay is a second path for peers given a relay member, offered once a link has tried direct alone for
    // `relay_after_ms`, or at once when the peer has no direct candidate. Nothing changes for a socket that never
    // enables it.
    void enable_relay(uint32_t token, RelaySend send);
    void disable_relay();
    bool relay_enabled() const;
    // One binary message from the lobby; false when it is not a frame of this room from a known peer.
    bool deliver_relay(const uint8_t* data, size_t size);
    RelayStats relay_stats() const;

    StunState stun(Endpoint* mapped) const;
    // Opens our NAT towards an endpoint before the handshake (throwaway datagrams).
    void punch(const Endpoint& to);

private:
    struct Packet {
        uint32_t seq = 0;
        std::vector<uint8_t> bytes;
        uint64_t sent_ms = 0;
        int tries = 0;
        bool acked = false;
    };

    struct Link {
        int id = 0;
        uint64_t account = 0;
        uint32_t tag = 0, peer_tag = 0;
        uint32_t nonce = 0, peer_nonce = 0;
        Endpoint addr;
        LinkState state = LinkState::kConnecting;
        bool wanted = false; // the game asked for it (connect()); a peer's handshake alone does not count
        uint64_t started_ms = 0, last_rx_ms = 0, last_tx_ms = 0, next_hello_ms = 0, addr_rx_ms = 0;
        bool relay_offered = false;
        std::string closed_why;
        // sending
        std::deque<std::vector<uint8_t>> outbox;
        size_t outbox_offset = 0;
        size_t queued = 0;
        uint32_t next_seq = 1;
        std::deque<Packet> unacked;
        uint32_t window = 64;
        double srtt = 0, rttvar = 0;
        uint32_t rto_ms = 300;
        // receiving
        uint32_t expected = 1;
        std::map<uint32_t, std::pair<uint8_t, std::vector<uint8_t>>> early;
        std::vector<uint8_t> assembling;
        std::deque<std::vector<uint8_t>> inbox;
        bool ack_due = false;
        LinkStats stats;
    };

    PeerSocket() = default;
    void Run();
    void Receive(const uint8_t* data, size_t size, const Endpoint& from, uint64_t now);
    void Handshake(const uint8_t* data, size_t size, const Endpoint& from, uint64_t now, bool reply);
    void Data(Link& link, const uint8_t* body, size_t size, uint64_t now);
    void Acked(Link& link, uint32_t ack, uint32_t sack, uint64_t now);
    void Timers(uint64_t now);
    void Flush(Link& link, uint64_t now);
    void SendHello(Link& link, const Endpoint& to, bool reply);
    void SendAck(Link& link);
    void SendRaw(const Endpoint& to, const void* data, size_t size);
    void SendRelay(uint16_t member, const void* data, size_t size);
    bool HasDirect(uint64_t account) const;
    bool RelayReachable(const Link& link, uint64_t now) const;
    void Close(Link& link, const std::string& why, bool tell_peer);
    void StunStep(uint64_t now);
    Link* Find(int id);
    const Link* Find(int id) const;
    Link* ByTag(uint32_t tag);
    Link* ByAccount(uint64_t account);
    Link& NewLink(uint64_t account, uint64_t now);

    socket_t socket_ = kNoSocket;
    uint16_t port_ = 0;
    Options options_;
    mutable std::mutex mutex_;
    std::thread thread_;
    bool stopping_ = false;
    RoomKey room_key_{};
    uint64_t self_ = 0;
    std::map<uint64_t, std::vector<Endpoint>> peers_;
    std::map<uint64_t, uint16_t> relay_members_;
    uint32_t relay_token_ = 0;
    RelaySend relay_send_;
    uint32_t relay_seq_ = 0;
    RelayStats relay_stats_;
    std::map<int, Link> links_;
    int next_link_ = 1;
    // STUN
    StunState stun_state_ = StunState::kIdle;
    Endpoint stun_mapped_;
    std::vector<Endpoint> stun_addresses_;
    std::thread stun_resolver_;
    bool stun_resolved_ = false;
    size_t stun_index_ = 0;
    int stun_attempt_ = 0;
    uint64_t stun_next_ms_ = 0, stun_server_deadline_ms_ = 0;
    std::array<uint8_t, 12> stun_id_{};
};

} // namespace dcp::net
