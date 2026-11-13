// What the Online page shows, apart from drawing it: its tabs and sections, the Online play switch and the players the
// lobby lists.
#pragma once

#include <string>
#include <vector>

#include "net/json.h"

namespace dcp {

enum class OnlineSection {
    kOnlinePlay,
    kTabs,
    kFriendRequests,
    kOnlineName,
    kFriendCode,
    kConnectionTest,
    kPingDisplay,
    kEarlyStoryOnline,
    kFriends,
    kLobbyVisibility,
    kLobbyPlayers
};

enum class OnlineTab { kSettings, kFriends, kLobby };
inline constexpr OnlineTab kOnlineTabs[] = {OnlineTab::kSettings, OnlineTab::kFriends, OnlineTab::kLobby};
const char* OnlineTabLabel(OnlineTab tab);

// With online play off only its switch is shown; on, the tabs and the sections of the selected one follow it.
std::vector<OnlineSection> OnlinePageSections(bool on, OnlineTab tab, size_t friend_requests, bool lobby_visible);

// `on` is the effective state: DCP_ONLINE, when set, wins over the saved switch and locks it.
struct OnlinePlaySwitch {
    bool on = false;
    bool locked = false;
};
OnlinePlaySwitch ResolveOnlinePlay(bool saved);

inline constexpr char kConnectionTestRequired[] = "Run the Connection test on the Online page before playing online.";
// Under the running Connection test on Windows, whose firewall may ask about the multiplayer port's first use; none
// elsewhere.
const char* ConnectionTestRunningNote(bool windows);

// How the Connection test section is drawn: while PLAY waits for the test it is a gold panel with the Run test button as
// the primary action; otherwise the plain row.
struct ConnectionTestLook {
    bool panel = false;
    bool primary_button = false;
    const char* required_text = nullptr;
};
ConnectionTestLook ConnectionTestLookOf(bool required);

// The panel's border and tint, 0 to 1, for the seconds since a blocked PLAY pointed at it: a few soft pulses that fade
// out; 0 before and after.
inline constexpr double kConnectionTestEmphasisSeconds = 2.5;
float ConnectionTestEmphasis(double seconds_since_blocked);

// The launcher asks whether PLAY may start every frame to grey its button, and once more when PLAY is pressed.
class PlayAttemptDetector {
public:
    bool Pressed(int frame) {
        const bool again = frame == last_frame_;
        last_frame_ = frame;
        return again;
    }

private:
    int last_frame_ = -1;
};

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

// What the lobby says this player is to the one reading the list.
enum class LobbyRelation { kNone, kPending, kFriend, kIncoming };

struct LobbyPlayer {
    std::string player; // the handle of this listing; never shown
    std::string name;
    std::string version; // empty from a client that did not say
    LobbyRelation relation = LobbyRelation::kNone;
};

// Requests to answer first, then pending, friends and everyone else, by name within each.
void SortLobbyPlayers(std::vector<LobbyPlayer>& players);
// The rows of lobby.list "players", sorted.
std::vector<LobbyPlayer> ReadLobbyPlayers(const net::Json& rows);
std::string LobbyVersionLabel(const LobbyPlayer& player);
std::string LobbyUpdatedText(int minutes);

struct LobbyAction {
    const char* text = nullptr;
    bool enabled = false;
    bool primary = false;
    bool decline = false; // Decline sits beside it
};
LobbyAction LobbyActionOf(LobbyRelation relation);

// When the Lobby list is read: rarely by itself, at once for a friend event or the button, one read at a time.
class LobbyRefresh {
public:
    static constexpr double kAutoSeconds = 300.0;
    // The lobby gives back one read every 5 s.
    static constexpr double kManualGapSeconds = 5.0;
    static constexpr double kRetrySeconds = 30.0;

    bool active() const { return active_; }
    bool Due(double now) const { return !active_ && now >= next_; }
    void Start(double now) {
        active_ = true;
        started_ = now;
        next_ = now + kAutoSeconds;
    }
    void Finish() { active_ = false; }
    void Fail(double now) {
        active_ = false;
        next_ = next_ < now + kRetrySeconds ? next_ : now + kRetrySeconds;
    }
    void Soon(double now) { next_ = now; }
    bool Manual(double now) {
        if (active_ || now - started_ < kManualGapSeconds) return false;
        next_ = now;
        return true;
    }
    void Reset() {
        active_ = false;
        next_ = 0;
    }

private:
    bool active_ = false;
    double next_ = 0;
    double started_ = -1e9;
};

} // namespace dcp
