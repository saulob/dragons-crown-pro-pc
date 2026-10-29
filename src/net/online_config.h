// Online settings as the game and the launcher use them: dcp.ini values (launcher/online_settings.h) with the DCP_*
// environment overrides of docs/ONLINE.md applied on top.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dcp::net {

// The UDP port the game binds for its peer-to-peer socket; when it is taken (a second instance on this PC), the system
// picks a free one.
inline constexpr uint16_t kGameUdpPort = 3658;
inline constexpr char kDefaultStunServers[] = "stun.cloudflare.com:3478,stun.l.google.com:19302";
// The production lobby; only DCP_LOBBY replaces it (test instances, the development lobby).
inline constexpr char kDefaultLobbyUrl[] = "wss://dcp-lobby.saulobenigno.workers.dev";
inline constexpr char kDefaultOnlineName[] = "Player";

struct OnlineConfig {
    bool enabled = false;
    std::string lobby = kDefaultLobbyUrl;
    std::string name = kDefaultOnlineName; // as typed; net::online_name makes it the in-game id
    bool upnp = true;
    uint16_t udp_port = 0;       // 0: the game's port, or any free one
    std::string stun = kDefaultStunServers; // "" or "off": no STUN
};

// DCP_ONLINE, DCP_LOBBY, DCP_ONLINE_NAME, DCP_UPNP, DCP_UDP_PORT and DCP_STUN, when set, replace the saved values.
void apply_environment(OnlineConfig& config);

// "host:port" entries of a comma list; the port defaults to 3478.
std::vector<std::pair<std::string, uint16_t>> stun_servers(const std::string& list);

bool parse_bool(const std::string& value, bool fallback);

} // namespace dcp::net
