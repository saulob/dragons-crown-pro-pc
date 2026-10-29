// What a client tells the lobby about itself (docs/ONLINE.md): the online protocol that decides who can play together,
// and the port version and platform players see.
#pragma once

#include <string>

namespace dcp::net {

// Raise it when a change stops this build from playing with the previous one. Builds with the same value play together
// whatever their port version; a client that sends none (older builds) counts as 0 and is never compatible.
inline constexpr int kOnlineProtocolVersion = 1;

bool online_compatible(int remote_protocol);

// The CMake project VERSION ("0.8.0").
const std::string& port_version();

// "Windows", "Linux", or "Steam Deck" when the DMI identity is Valve's Jupiter (LCD) or Galileo (OLED).
std::string platform_name(bool windows, const std::string& dmi_vendor, const std::string& dmi_product);
const std::string& host_platform();

// "Windows · v0.8.0"; the platform alone without a version.
std::string system_type_text(const std::string& platform, const std::string& version);

} // namespace dcp::net
