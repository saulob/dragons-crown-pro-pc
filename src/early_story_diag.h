// DEBUG_SAULO: temporary DCP-7G8RZV diagnostics of an Early Story session's network picture; delete this file and its
// one call in runtime_matching2.cpp after the next field report.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "net/peer_socket.h"
#include "net/session_watch.h"

struct EarlyStoryDiagRoom {
    bool room = false;
    size_t members = 0;
    const char* lobby = "";
    bool relay_on = false;
    dcp::net::RelayStats relay;
    int own_step = -1;
};

void early_story_diag(uint64_t now, const dcp::net::WatchSession& session, const std::vector<dcp::net::WatchLink>& links,
                      const EarlyStoryDiagRoom& room);
