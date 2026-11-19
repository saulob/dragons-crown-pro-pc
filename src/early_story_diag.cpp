// DEBUG_SAULO: see early_story_diag.h.
#include "early_story_diag.h"

#include <cstdio>
#include <string>

using namespace dcp::net;

namespace {

std::string g_last;
uint64_t g_last_pump_ms = 0;

const char* StateName(LinkState state) {
    switch (state) {
    case LinkState::kConnected: return "connected";
    case LinkState::kConnecting: return "connecting";
    case LinkState::kClosed: break;
    }
    return "closed";
}

} // namespace

void early_story_diag(uint64_t now, const WatchSession& session, const std::vector<WatchLink>& links, const EarlyStoryDiagRoom& room) {
    const uint64_t paused = g_last_pump_ms ? now - g_last_pump_ms : 0;
    g_last_pump_ms = now;
    if (session.role == WatchRole::kNone) return;
    if (session.story && paused >= 2000)
        std::printf("DEBUG_SAULO Early Story diag: game pump paused %llu ms (step %d)\n", (unsigned long long)paused, session.step);
    std::string text = std::string("role=") + (session.role == WatchRole::kHost ? "host" : "guest") + " story=" +
                       (session.story ? "1" : "0") + " step=" + std::to_string(session.step) + " own=" + std::to_string(room.own_step) +
                       " room=" + (room.room ? "1" : "0") + " members=" + std::to_string(room.members) + " lobby=" + room.lobby +
                       " relay=" + (room.relay_on ? "on" : "off");
    for (const WatchLink& link : links) {
        const LinkStats& s = link.stats;
        text += " [link " + std::to_string(link.link) + " " + StateName(link.state) + " q" + std::to_string(int(s.quality)) +
                (s.moved_to_relay ? " relay" : " direct") + " end=" + link_end_name(s.end) + " probes=" +
                std::to_string(s.relay_probes) + " answers=" + std::to_string(s.relay_answers) + "]";
    }
    if (text == g_last) return;
    g_last = text;
    std::printf("DEBUG_SAULO Early Story diag: %s socket_pump=%llu silence=%llu ms relay_frames out=%llu in=%llu fail=%llu "
                "throttled=%llu rejected=%llu\n",
                text.c_str(), (unsigned long long)session.pump_generation,
                (unsigned long long)(links.empty() ? 0 : links.front().stats.silence_ms), (unsigned long long)room.relay.frames_out,
                (unsigned long long)room.relay.frames_in, (unsigned long long)room.relay.send_failures,
                (unsigned long long)room.relay.throttled, (unsigned long long)room.relay.rejected);
}
