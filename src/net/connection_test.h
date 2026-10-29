// The launcher's Connection test: can this PC reach the lobby, and how will other players reach it?
#pragma once

#include <string>
#include <vector>

#include "net/identity.h"
#include "net/online_config.h"

namespace dcp::net {

struct ConnectionReport {
    struct Line {
        bool ok = false;
        std::string label;
        std::string text;
    };
    std::vector<Line> lines;
    bool ready = false;
    std::string verdict;
    std::string friend_code; // from the lobby, when it answered
};

// Blocks for a few seconds (lobby, two STUN servers, the router); run it off the UI thread.
ConnectionReport run_connection_test(const OnlineConfig& config, const Identity& identity, const std::string& name,
                                     const std::string& game);

} // namespace dcp::net
