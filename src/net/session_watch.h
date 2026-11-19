// Early Story Online session telemetry: per-frame snapshots of the session and its peer links become rate-limited log
// lines (docs/ONLINE.md, "Early Story session telemetry"). Lines carry no address, account, name or room.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "net/peer_socket.h"

namespace dcp::net {

enum class WatchRole { kNone, kHost, kGuest };

struct WatchSession {
    WatchRole role = WatchRole::kNone;
    bool story = false;   // seated in an Early Story session room
    int step = -1;        // the step the session plays: the host's
    bool lobby_up = true; // our own lobby link is signed in and not reconnecting
    uint64_t pump_generation = 0; // PeerSocket::pump_generation() of our socket
};

struct WatchLink {
    uint64_t peer = 0; // with `link`, tells snapshots apart; never printed
    int link = -1;
    LinkState state = LinkState::kClosed;
    LinkStats stats;
};

class SessionWatch {
public:
    static constexpr uint64_t kTransitionMs = 60000;   // a step change and the scene work it starts
    static constexpr uint64_t kPumpStalledMs = 1500;   // our socket loop passes every 5 ms
    static constexpr uint64_t kPeerLeftAfterMs = 120000; // the lobby holds a lost seat 30 s
    static constexpr int kLinesPerEvent = 8;

    std::vector<std::string> Update(uint64_t now, const WatchSession& session, const std::vector<WatchLink>& links);
    // The lobby's room.member_left for that peer, with its reason (leave, disconnect, kicked).
    std::vector<std::string> PeerLeft(uint64_t now, uint64_t peer, const std::string& why);
    int Count(const std::string& event) const;

private:
    struct Track {
        bool stalled = false, attempted = false, ended = false, left = false;
    };

    void Emit(std::vector<std::string>& out, const std::string& event, const std::string& fields);
    std::string Context(const LinkStats* stats) const;
    void LinkEvents(std::vector<std::string>& out, uint64_t now, const WatchLink& link, Track& track);
    void CloseTransition(std::vector<std::string>& out, const char* result);

    WatchSession session_;
    bool in_session_ = false;
    int last_step_ = -1;
    uint64_t transition_until_ = 0;
    int transition_from_ = -1;
    bool transition_lost_ = false;
    bool pump_alive_ = true;
    uint64_t pump_seen_ = 0, pump_moved_ms_ = 0;
    std::map<std::pair<uint64_t, int>, Track> tracks_;
    std::map<uint64_t, uint64_t> timeouts_; // peer -> when its link timed out, kept past the session for PeerLeft
    std::map<std::string, int> counts_;
};

const char* link_end_name(LinkEnd end);

} // namespace dcp::net
