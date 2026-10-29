// What the Online page shows, apart from drawing it: its sections, the Online play switch and the players the lobby lists.
#pragma once

#include <string>
#include <vector>

#include "net/json.h"

namespace dcp {

enum class OnlineSection { kUnavailable, kOnlinePlay, kFriendRequests, kOnlineName, kFriendCode, kConnectionTest, kFriends };

// With online play off only its switch is shown; friend requests come right after it.
std::vector<OnlineSection> OnlinePageSections(bool supported, bool on, size_t friend_requests);

// `on` is the effective state: DCP_ONLINE, when set, wins over the saved switch and locks it.
struct OnlinePlaySwitch {
    bool on = false;
    bool locked = false;
};
OnlinePlaySwitch ResolveOnlinePlay(bool saved);

struct OnlinePlayer {
    std::string account; // names the player in lobby requests; never shown
    std::string name;
    bool online = false;
    bool in_room = false;
};

// The rows of friends.list "friends" or friends.requests "incoming"/"outgoing".
std::vector<OnlinePlayer> ReadOnlinePlayers(const net::Json& rows);
const char* FriendPresence(const OnlinePlayer& player);
std::string FriendRequestText(const OnlinePlayer& sender);

} // namespace dcp
