#include "online_view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <tuple>

#include "net/online_config.h"
#include "net/online_version.h"

namespace dcp {

const char* OnlineTabLabel(OnlineTab tab) {
    switch (tab) {
    case OnlineTab::kSettings: return "Settings";
    case OnlineTab::kFriends: return "Friends";
    case OnlineTab::kLobby: return "Lobby";
    }
    return "";
}

std::vector<OnlineSection> OnlinePageSections(bool on, OnlineTab tab, size_t friend_requests, bool lobby_visible) {
    if (!on) {
        return {OnlineSection::kOnlinePlay};
    }
    std::vector<OnlineSection> sections{OnlineSection::kOnlinePlay, OnlineSection::kTabs};
    switch (tab) {
    case OnlineTab::kSettings:
        for (const OnlineSection section : {OnlineSection::kOnlineName, OnlineSection::kFriendCode,
                                            OnlineSection::kConnectionTest, OnlineSection::kPingDisplay,
                                            OnlineSection::kEarlyStoryOnline}) {
            sections.push_back(section);
        }
        break;
    case OnlineTab::kFriends:
        if (friend_requests > 0) {
            sections.push_back(OnlineSection::kFriendRequests);
        }
        sections.push_back(OnlineSection::kFriends);
        break;
    case OnlineTab::kLobby:
        sections.push_back(OnlineSection::kLobbyVisibility);
        if (lobby_visible) {
            sections.push_back(OnlineSection::kLobbyPlayers);
        }
        break;
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

const char* ConnectionTestRunningNote(bool windows) {
    return windows ? "If Windows asks whether to allow network access, choose Allow access so other players can connect "
                     "to you."
                   : nullptr;
}

ConnectionTestLook ConnectionTestLookOf(bool required) {
    return required ? ConnectionTestLook{true, true, "Connection test required"} : ConnectionTestLook{};
}

float ConnectionTestEmphasis(double seconds_since_blocked) {
    if (seconds_since_blocked < 0.0 || seconds_since_blocked >= kConnectionTestEmphasisSeconds) return 0.0f;
    const double progress = seconds_since_blocked / kConnectionTestEmphasisSeconds;
    const double pulse = 0.5 * (1.0 - std::cos(2.0 * 3.14159265358979 * 3.0 * progress));
    return static_cast<float>(pulse * (1.0 - 0.5 * progress));
}

std::vector<OnlinePlayer> ReadOnlinePlayers(const net::Json& rows) {
    std::vector<OnlinePlayer> players;
    for (const net::Json& row : rows.items()) {
        const std::string& shown = row["display_name"].as_string();
        OnlinePlayer player{row["account"].as_string(), shown.empty() ? row["name"].as_string() : shown, row["online"].as_bool(),
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

void SortLobbyPlayers(std::vector<LobbyPlayer>& players) {
    const auto rank = [](LobbyRelation relation) {
        switch (relation) {
        case LobbyRelation::kIncoming: return 0;
        case LobbyRelation::kPending: return 1;
        case LobbyRelation::kFriend: return 2;
        case LobbyRelation::kNone: break;
        }
        return 3;
    };
    const auto folded = [](std::string text) {
        for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    std::stable_sort(players.begin(), players.end(), [&](const LobbyPlayer& a, const LobbyPlayer& b) {
        return std::tuple(rank(a.relation), folded(a.name), a.player) < std::tuple(rank(b.relation), folded(b.name), b.player);
    });
}

std::vector<LobbyPlayer> ReadLobbyPlayers(const net::Json& rows) {
    std::vector<LobbyPlayer> players;
    for (const net::Json& row : rows.items()) {
        const std::string state = row["state"].as_string();
        LobbyPlayer player{row["player"].as_string(), row["name"].as_string(), row["version"].as_string()};
        if (player.player.empty() || player.name.empty()) {
            continue;
        }
        player.relation = state == "friend"     ? LobbyRelation::kFriend
                          : state == "pending"  ? LobbyRelation::kPending
                          : state == "incoming" ? LobbyRelation::kIncoming
                                                : LobbyRelation::kNone;
        players.push_back(std::move(player));
    }
    SortLobbyPlayers(players);
    return players;
}

std::string LobbyVersionLabel(const LobbyPlayer& player) {
    return player.version.empty() ? "Version unknown" : "v" + player.version;
}

std::string LobbyUpdatedText(int minutes) {
    return minutes < 1 ? "Updated just now" : "Updated " + std::to_string(minutes) + " min ago";
}

LobbyAction LobbyActionOf(LobbyRelation relation) {
    switch (relation) {
    case LobbyRelation::kNone: return {"Send friend request", true, false};
    case LobbyRelation::kPending: return {"Pending", false, false};
    case LobbyRelation::kFriend: return {"Friend", false, false};
    case LobbyRelation::kIncoming: return {"Accept", true, true, true};
    }
    return {};
}

} // namespace dcp
