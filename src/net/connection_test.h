// The launcher's Connection test: can this PC reach the lobby, and how will other players reach it?
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/identity.h"
#include "net/online_config.h"

namespace dcp::net {

struct ConnectionFacts {
    enum class Router { kNone, kMapped, kMappedBehindNat };
    bool identity_ok = true;
    bool lobby_ok = false;
    int64_t lobby_latency_ms = -1; // the sign-in's round trip to the online service, not a player's Ping
    uint16_t udp_port = 0;        // the port the test opened; 0: any free one
    bool port_open = true;
    bool stun_off = false;
    bool udp_ok = false;          // a STUN server answered
    bool public_address = false;  // the STUN address is this PC's own
    bool second_answer = false;   // a second STUN server answered
    bool same_mapping = false;    // and saw the same public port
    Router router = Router::kNone;
};

struct ConnectionReport {
    enum class Mark { kOk, kNote, kProblem };
    struct Line {
        Mark mark = Mark::kProblem;
        std::string label;
        std::string text;
    };
    std::vector<Line> lines;  // plain language, no addresses; when ready only what makes it ready
    bool ready = false;
    std::string verdict;
    std::string friend_code;           // from the lobby, when it answered
    std::vector<std::string> details;  // addresses, servers and errors, for the log
};

ConnectionReport summarize_connection(const ConnectionFacts& facts);

// The launcher's test opens the port the game will bind: the configured one, else the game's.
uint16_t connection_test_port(const OnlineConfig& config);

// Blocks for a few seconds (lobby, two STUN servers, the router); run it off the UI thread. STUN and UPnP use one
// socket bound to `udp_port`, never another port when it is taken; 0 (the in-game test, whose port the game holds)
// binds any free port and maps the game's.
ConnectionReport run_connection_test(const OnlineConfig& config, const Identity& identity, const std::string& name,
                                     const std::string& game, uint16_t udp_port = 0);

} // namespace dcp::net
