// The Online page's settings in dcp.ini, read the same way by the launcher and the game (src/runtime_online.cpp).
#pragma once

#include <filesystem>
#include <string_view>

#include "net/online_config.h"

namespace dcp {

inline constexpr char kOnlineEnabledKey[] = "online_enabled";
inline constexpr char kOnlineLobbyKey[] = "online_lobby";
inline constexpr char kOnlineNameKey[] = "online_name";
inline constexpr char kOnlineUpnpKey[] = "online_upnp";
inline constexpr char kOnlineUdpPortKey[] = "online_udp_port";
inline constexpr char kOnlineStunKey[] = "online_stun";

// Kept by "Reset all settings": the player's name and lobby are not display preferences.
bool IsOnlineSetting(std::string_view key);
bool IsValidOnlineSetting(std::string_view key, std::string_view value);

// The saved values with the DCP_* environment overrides not applied (net::apply_environment does that).
net::OnlineConfig ReadOnlineConfig(const std::filesystem::path& settings_file);

} // namespace dcp
