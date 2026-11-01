// The Online page's settings in dcp.ini, read the same way by the launcher and the game (src/runtime_online.cpp).
#pragma once

#include <filesystem>
#include <string_view>

#include "net/online_config.h"

namespace trg {
class SettingsStore;
}

namespace dcp {

class LauncherSettings;

inline constexpr char kOnlineEnabledKey[] = "online_enabled";
inline constexpr char kOnlineNameKey[] = "online_name";
inline constexpr char kOnlineUdpPortKey[] = "online_udp_port";
inline constexpr char kOnlineStunKey[] = "online_stun";
// The UDP port of the last Connection test finished with Online play on; 0: none since Online play was turned on.
inline constexpr char kConnectionTestPortKey[] = "online_connection_test_port";
// Whether the in-game Ping panel starts shown (gpu/shim/dcp_ping.h); a display preference, reset like the others.
inline constexpr char kPingDisplayKey[] = "show_ping";

// Kept by "Reset all settings": the player's name is not a display preference.
bool IsOnlineSetting(std::string_view key);
bool IsValidOnlineSetting(std::string_view key, std::string_view value);

// The saved values with the DCP_* environment overrides not applied (net::apply_environment does that). The lobby and
// UPnP have no saved setting: only DCP_LOBBY and DCP_UPNP change them.
net::OnlineConfig ReadOnlineConfig(const std::filesystem::path& settings_file);
net::OnlineConfig OnlineConfigOf(const trg::SettingsStore& settings);

// With Online play on, PLAY waits for a Connection test finished on the UDP port the game will use, whatever its
// result. Another port (online_udp_port or DCP_UDP_PORT) asks for a new test.
bool ConnectionTestNeeded(const trg::SettingsStore& settings, bool online_on, bool testing, uint16_t port);
// Saved at once: closing the launcher without Save keeps it.
bool RecordConnectionTest(LauncherSettings& settings, uint16_t port);
// Online play off forgets the test (pending until Save), so turning it on again asks for a new one.
void ForgetConnectionTestWhenOff(trg::SettingsStore& settings, bool online_on);

} // namespace dcp
