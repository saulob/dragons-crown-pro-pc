#include "net/session_watch.h"

namespace dcp::net {
namespace {

const char* RoleName(WatchRole role) {
    switch (role) {
    case WatchRole::kHost: return "host";
    case WatchRole::kGuest: return "guest";
    case WatchRole::kNone: break;
    }
    return "none";
}

std::string Bucket(uint64_t ms) {
    if (ms < 1000) return "<1s";
    if (ms < 2000) return "1-2s";
    if (ms < 5000) return "2-5s";
    if (ms < 10000) return "5-10s";
    if (ms < 30000) return "10-30s";
    if (ms < 60000) return "30-60s";
    return ">=60s";
}

bool TimedOut(LinkEnd end) { return end == LinkEnd::kIdleTimeout || end == LinkEnd::kAckTimeout; }

const char* PeerCloseReason(LinkEnd end) {
    switch (end) {
    case LinkEnd::kPeerGame: return "game";
    case LinkEnd::kPeerSocket: return "socket";
    case LinkEnd::kPeerClosed: return "unspecified";
    case LinkEnd::kPeerRefused: return "refused";
    default: return nullptr;
    }
}

const char* RelayState(const LinkStats& s) {
    if (s.moved_to_relay) return "moved";
    if (!s.relay_probes) return "none";
    return s.relay_answers ? "answered" : "never_answered";
}

const char* LeftVerdict(const std::string& why) {
    if (why == "disconnect") return "peer_lobby_lost";
    if (why == "leave") return "peer_left_room";
    if (why == "kicked") return "peer_kicked";
    return "other";
}

} // namespace

const char* link_end_name(LinkEnd end) {
    switch (end) {
    case LinkEnd::kOpen: return "open";
    case LinkEnd::kLocal: return "local";
    case LinkEnd::kPeerGame: return "peer_game";
    case LinkEnd::kPeerSocket: return "peer_socket";
    case LinkEnd::kPeerClosed: return "peer_closed";
    case LinkEnd::kPeerRefused: return "peer_refused";
    case LinkEnd::kIdleTimeout: return "idle_timeout";
    case LinkEnd::kAckTimeout: return "ack_timeout";
    case LinkEnd::kNoAnswer: return "no_answer";
    case LinkEnd::kReplaced: return "replaced";
    case LinkEnd::kOversize: return "oversize";
    }
    return "?";
}

int SessionWatch::Count(const std::string& event) const {
    const auto it = counts_.find(event);
    return it == counts_.end() ? 0 : it->second;
}

void SessionWatch::Emit(std::vector<std::string>& out, const std::string& event, const std::string& fields) {
    const int count = ++counts_[event];
    if (count > kLinesPerEvent) return;
    std::string line = "Early Story Online telemetry: event=" + event + " " + fields + " count=" + std::to_string(count);
    if (count == kLinesPerEvent) line += " limit=reached";
    out.push_back(std::move(line));
}

std::string SessionWatch::Context(const LinkStats* stats) const {
    std::string text = std::string("role=") + RoleName(session_.role) + " step=" + std::to_string(session_.step) +
                       " transition=" + (transition_until_ ? "after_step_change" : "steady");
    if (stats) text += std::string(" transport=") + (stats->moved_to_relay ? "relay" : "direct");
    return text;
}

void SessionWatch::LinkEvents(std::vector<std::string>& out, uint64_t now, const WatchLink& link, Track& track) {
    const LinkStats& s = link.stats;
    if (track.ended) return;
    if (link.state == LinkState::kConnected) {
        if (!track.stalled && s.quality == LinkQuality::kCritical) {
            track.stalled = true;
            Emit(out, "early_story_network_stall", Context(&s) + " silence=" + Bucket(s.silence_ms));
        }
        if (track.stalled && !track.attempted && (s.relay_probes || s.moved_to_relay)) {
            track.attempted = true;
            Emit(out, "early_story_relay_fallback_attempt", Context(&s));
        }
        if (track.stalled && s.quality == LinkQuality::kHealthy) {
            Emit(out, "early_story_session_recovered", Context(&s) + " via=" + (s.moved_to_relay ? "relay" : "direct"));
            track.stalled = track.attempted = false;
        }
        return;
    }
    if (link.state != LinkState::kClosed) return;
    track.ended = true;
    if (transition_until_) transition_lost_ = true;
    if (TimedOut(s.end)) {
        timeouts_[link.peer] = now;
        Emit(out, "early_story_peer_timeout",
             Context(&s) + " timeout=" + (s.end == LinkEnd::kIdleTimeout ? "10s" : "ack") + " relay=" + RelayState(s) +
                 " local_lobby=" + (session_.lobby_up ? "up" : "down") + " local_pump=" + (pump_alive_ ? "alive" : "stalled"));
        if (s.moved_to_relay || s.relay_probes)
            Emit(out, "early_story_relay_fallback_failed",
                 Context(&s) + " reason=" + (s.moved_to_relay ? "relay_path_lost" : s.relay_answers ? "not_proven_in_time" : "never_answered") +
                     " probes=" + std::to_string(s.relay_probes));
        return;
    }
    if (const char* reason = PeerCloseReason(s.end)) Emit(out, "early_story_peer_closed", Context(&s) + " reason=" + reason);
}

void SessionWatch::CloseTransition(std::vector<std::string>& out, const char* result) {
    if (!transition_until_) return;
    transition_until_ = 0;
    Emit(out, "early_story_transition_exit",
         Context(nullptr) + " from=" + std::to_string(transition_from_) + " result=" + (transition_lost_ ? "link_lost" : result));
    transition_lost_ = false;
}

std::vector<std::string> SessionWatch::Update(uint64_t now, const WatchSession& session, const std::vector<WatchLink>& links) {
    std::vector<std::string> out;
    if (session.pump_generation != pump_seen_ || !pump_moved_ms_) {
        pump_seen_ = session.pump_generation;
        pump_moved_ms_ = now;
    }
    pump_alive_ = now - pump_moved_ms_ < kPumpStalledMs;
    std::erase_if(timeouts_, [now](const auto& entry) { return now - entry.second > kPeerLeftAfterMs; });
    const bool active = session.story && session.role != WatchRole::kNone;
    if (!active) {
        if (in_session_) CloseTransition(out, "session_ended");
        in_session_ = false;
        last_step_ = -1;
        tracks_.clear();
        session_ = session;
        return out;
    }
    if (!in_session_) {
        in_session_ = true;
        last_step_ = session.step;
        tracks_.clear();
    }
    session_ = session;
    if (session.step >= 0 && last_step_ >= 0 && session.step != last_step_) {
        transition_from_ = last_step_;
        transition_until_ = now + kTransitionMs;
        transition_lost_ = false;
        Emit(out, "early_story_transition_enter", Context(nullptr) + " from=" + std::to_string(last_step_));
    }
    if (session.step >= 0) last_step_ = session.step;
    for (const WatchLink& link : links) LinkEvents(out, now, link, tracks_[{link.peer, link.link}]);
    if (transition_until_ && now >= transition_until_) CloseTransition(out, "session_kept");
    return out;
}

std::vector<std::string> SessionWatch::PeerLeft(uint64_t now, uint64_t peer, const std::string& why) {
    std::vector<std::string> out;
    bool seated = false;
    for (auto& [key, track] : tracks_) {
        if (key.first != peer || track.left) continue;
        track.left = true;
        seated = true;
    }
    const auto timeout = timeouts_.find(peer);
    if (timeout != timeouts_.end()) {
        Emit(out, "early_story_peer_left",
             Context(nullptr) + " why=" + LeftVerdict(why) + " after_timeout=" + Bucket(now - timeout->second));
        timeouts_.erase(timeout);
    } else if (in_session_ && seated) {
        Emit(out, "early_story_peer_left", Context(nullptr) + " why=" + LeftVerdict(why));
    }
    return out;
}

} // namespace dcp::net
