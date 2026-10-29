// What the Online page shows, apart from drawing it: its sections, the Online play switch and the players the lobby lists.
#pragma once

#include <string>
#include <vector>

#include "net/json.h"

namespace dcp {

enum class OnlineSection {
    kOnlinePlay,
    kFriendRequests,
    kOnlineName,
    kFriendCode,
    kConnectionTest,
    kPingDisplay,
    kFriends
};

// With online play off only its switch is shown; friend requests come right after it.
std::vector<OnlineSection> OnlinePageSections(bool on, size_t friend_requests);

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
    // What the friend's own client advertised while online; empty or 0 when it did not say.
    std::string platform;
    std::string version;
    int online_protocol = 0;
};

// The rows of friends.list "friends" or friends.requests "incoming"/"outgoing".
std::vector<OnlinePlayer> ReadOnlinePlayers(const net::Json& rows);
const char* FriendPresence(const OnlinePlayer& player);

// The line under a friend's name: "Online, in a room · Windows · v0.8.0", and whether to add "Incompatible" (a room of
// another online protocol).
struct FriendStatus {
    std::string text;
    bool incompatible = false;
};
FriendStatus FriendStatusOf(const OnlinePlayer& player);
std::string FriendRequestText(const OnlinePlayer& sender);

} // namespace dcp
