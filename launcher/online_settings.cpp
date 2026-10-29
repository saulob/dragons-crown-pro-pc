#include "online_settings.h"

#include <cstdlib>
#include <string>

#include "net/websocket.h"
#include "settings.h"

namespace dcp {

bool IsOnlineSetting(std::string_view key) {
    for (const std::string_view online : {kOnlineEnabledKey, kOnlineLobbyKey, kOnlineNameKey, kOnlineUpnpKey,
                                          kOnlineUdpPortKey, kOnlineStunKey}) {
        if (key == online) {
            return true;
        }
    }
    return false;
}

bool IsValidOnlineSetting(std::string_view key, std::string_view value) {
    if (key == kOnlineLobbyKey) {
        net::WsUrl url;
        return net::parse_ws_url(std::string(value), url, nullptr);
    }
    if (key == kOnlineUdpPortKey) {
        if (value.empty() || value.size() > 5 || value.find_first_not_of("0123456789") != std::string_view::npos) {
            return false;
        }
        return std::atoi(std::string(value).c_str()) <= 65535;
    }
    if (key == kOnlineNameKey) {
        return value.size() <= 64;
    }
    return true;
}

net::OnlineConfig ReadOnlineConfig(const std::filesystem::path& settings_file) {
    net::OnlineConfig config;
    if (settings_file.empty()) {
        return config;
    }
    const LauncherSettings settings(settings_file);
    config.enabled = settings.GetBool(kOnlineEnabledKey, false);
    config.lobby = settings.Get(kOnlineLobbyKey, net::kDefaultLobbyUrl);
    config.name = settings.Get(kOnlineNameKey);
    config.upnp = settings.GetBool(kOnlineUpnpKey, true);
    config.udp_port = static_cast<uint16_t>(std::atoi(settings.Get(kOnlineUdpPortKey, "0").c_str()));
    config.stun = settings.Get(kOnlineStunKey, net::kDefaultStunServers);
    return config;
}

} // namespace dcp
