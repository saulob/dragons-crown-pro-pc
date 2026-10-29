#include "online_view.h"

#include <cstdlib>

#include "net/online_config.h"
#include "net/online_version.h"

namespace dcp {

std::vector<OnlineSection> OnlinePageSections(bool supported, bool on, size_t friend_requests) {
    if (!supported) {
        return {OnlineSection::kUnavailable, OnlineSection::kOnlinePlay};
    }
    if (!on) {
        return {OnlineSection::kOnlinePlay};
    }
    std::vector<OnlineSection> sections{OnlineSection::kOnlinePlay};
    if (friend_requests > 0) {
        sections.push_back(OnlineSection::kFriendRequests);
    }
    for (const OnlineSection section : {OnlineSection::kOnlineName, OnlineSection::kFriendCode,
                                        OnlineSection::kConnectionTest, OnlineSection::kPingDisplay,
                                        OnlineSection::kFriends}) {
        sections.push_back(section);
    }
    return sections;
}

OnlinePlaySwitch ResolveOnlinePlay(bool saved) {
    const char* forced = std::getenv("DCP_ONLINE");
    if (!forced || !*forced) {
        return {saved, false};
    }
    return {net::parse_bool(forced, saved), true};
}

std::vector<OnlinePlayer> ReadOnlinePlayers(const net::Json& rows) {
    std::vector<OnlinePlayer> players;
    for (const net::Json& row : rows.items()) {
        OnlinePlayer player{row["account"].as_string(), row["name"].as_string(), row["online"].as_bool(),
                            row["room"].as_u64() != 0, row["platform"].as_string(), row["version"].as_string(),
                            int(row["online_protocol"].as_int())};
        if (player.name.empty()) {
            player.name = net::kDefaultOnlineName;
        }
        players.push_back(std::move(player));
    }
    return players;
}

const char* FriendPresence(const OnlinePlayer& player) {
    return player.in_room ? "Online, in a room" : player.online ? "Online" : "Offline";
}

FriendStatus FriendStatusOf(const OnlinePlayer& player) {
    FriendStatus status{FriendPresence(player)};
    if (!player.online) {
        return status;
    }
    if (!player.platform.empty()) {
        status.text += " \xC2\xB7 " + player.platform;
    }
    if (!player.version.empty()) {
        status.text += " \xC2\xB7 v" + player.version;
    }
    status.incompatible = player.in_room && !net::online_compatible(player.online_protocol);
    return status;
}

std::string FriendRequestText(const OnlinePlayer& sender) {
    return sender.name + " wants to be your friend";
}

} // namespace dcp
