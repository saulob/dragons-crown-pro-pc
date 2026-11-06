// Discord Rich Presence without the host or the game: the IPC frames, the place table and its lookup, the activity text.
#ifndef DCP_DISCORD_CORE_H
#define DCP_DISCORD_CORE_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dcp::discord {

inline constexpr uint32_t kOpHandshake = 0, kOpFrame = 1, kOpClose = 2, kOpPing = 3, kOpPong = 4;
inline constexpr uint32_t kMaxReplyBytes = 1u << 20;
inline constexpr int kMaxFramesPerReply = 8;
inline constexpr int kPartyLimit = 4;
inline constexpr int kHighestLevel = 255;

// A snowflake. It also goes into JSON unescaped, so nothing but digits may pass.
inline bool ValidClientId(std::string_view id) {
    return id.size() >= 17 && id.size() <= 20 && id.find_first_not_of("0123456789") == std::string_view::npos;
}

// [opcode u32][length u32][JSON], little endian on every target.
inline std::string Frame(uint32_t op, std::string_view json) {
    const uint32_t length = static_cast<uint32_t>(json.size());
    std::string frame(sizeof(op) + sizeof(length) + json.size(), '\0');
    std::memcpy(frame.data(), &op, sizeof(op));
    std::memcpy(frame.data() + sizeof(op), &length, sizeof(length));
    std::copy(json.begin(), json.end(), frame.begin() + sizeof(op) + sizeof(length));
    return frame;
}

// `link` has int64_t Send(const void*, size_t) and int64_t Receive(void*, size_t): bytes moved, or <= 0 when broken.
template <typename Link>
bool SendAll(Link& link, std::string_view bytes) {
    while (!bytes.empty()) {
        const int64_t sent = link.Send(bytes.data(), bytes.size());
        if (sent <= 0) return false;
        bytes.remove_prefix(static_cast<size_t>(sent));
    }
    return true;
}

template <typename Link>
bool ReceiveAll(Link& link, void* data, size_t size) {
    for (size_t got = 0; got < size;) {
        const int64_t n = link.Receive(static_cast<char*>(data) + got, size - got);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

enum class Reply {
    kAnswered, // `answer` is Discord's reply
    kClosed,   // Discord refused or ended the connection; `answer` says why
    kBroken,   // the connection failed or the reply makes no sense
};

// Sends one frame and waits for Discord's reply, so its answers never pile up in the pipe.
template <typename Link>
Reply Exchange(Link& link, uint32_t op, std::string_view json, std::string& answer) {
    if (!SendAll(link, Frame(op, json))) return Reply::kBroken;
    for (int frame = 0; frame < kMaxFramesPerReply; ++frame) {
        uint32_t header[2];
        if (!ReceiveAll(link, header, sizeof(header)) || header[1] > kMaxReplyBytes) return Reply::kBroken;
        std::string body(header[1], '\0');
        if (!ReceiveAll(link, body.data(), body.size())) return Reply::kBroken;
        switch (header[0]) {
        case kOpFrame:
            if (body.empty()) return Reply::kBroken;
            answer = std::move(body);
            return Reply::kAnswered;
        case kOpClose:
            answer = std::move(body);
            return Reply::kClosed;
        case kOpPing:
            if (!SendAll(link, Frame(kOpPong, body))) return Reply::kBroken;
            break;
        case kOpPong:
            break;
        default:
            return Reply::kBroken;
        }
    }
    return Reply::kBroken;
}

inline bool ReportsError(std::string_view answer) {
    return answer.find("\"evt\":\"ERROR\"") != std::string_view::npos;
}

// Where the player is, from which file the game reads out of its CPK archives: each area has its own map file
// (round/*.brb) and each town building its own scene (stage/T_*).
struct Place {
    const char* prefix; // of the entry path in the archive
    const char* name;
};

// Names are the game's own English text (msgsheet/*_US.fms).
inline constexpr Place kPlaces[] = {
    {"stage/title", "Title screen"},
    {"stage/T_ChrSelect", "Dragon's Haven Inn"},
    {"stage/T_FightersGuild", "Adventurers Guild"},
    {"stage/T_Shop", "Morgan's Magic Item Shop"}, // both load on one visit to Morgan's
    {"stage/T_MagesGuild", "Morgan's Magic Item Shop"},
    {"stage/T_MagesTower", "Lucain's Tower"},
    {"stage/T_Temple", "Canaan Temple"},
    {"stage/T_Castle", "Castle"},
    {"stage/T_Prison", "Prison"}, // not confirmed in play
    {"round/Town00", "Town"},
    // The town's street scenes, loaded again on every walk between them; the street one also on leaving a building.
    {"stage/bg25a", "Town"},
    {"stage/bg01a", "Town Gate"},
    {"stage/bg41a", "Stables"},
    {"round/Camp_", "Camp"},
    {"stage/camp_", "Camp"},
    {"round/Tutorial", "Tutorial"},
    {"round/Arena", "Colosseum"},
    {"round/RandomDungeon", "Labyrinth of Chaos"},
    {"round/AncientLabyrinth", "Ancient Labyrinth"},
    {"round/Catacomb", "Castle of the Dead"},
    {"round/ForgottenRealms", "Forgotten Sanctuary"},
    {"round/GhostshipCove", "Ghost Ship Cove"},
    {"round/LostWoods", "Lost Woods"},
    {"round/LostZone", "Lost Zone"},
    {"round/MageTower", "Mage's Tower"},
    {"round/MoldyDungeon", "Moldy Dungeon"},
    {"round/MysticRuins", "Ancient Temple Ruins"},
    {"round/UnderCitadel", "Old Capital"},
};
inline constexpr int kPlaceCount = static_cast<int>(std::size(kPlaces));

inline constexpr const char* kClasses[] = {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"};
inline constexpr int kClassCount = static_cast<int>(std::size(kClasses));

// The index in kPlaces of an archive entry, -1 when it says nothing about where the player is.
inline int PlaceOf(std::string_view entry_path) {
    for (int place = 0; place < kPlaceCount; ++place) {
        if (entry_path.rfind(kPlaces[place].prefix, 0) == 0) return place;
    }
    return -1;
}

struct Area {
    uint64_t begin = 0, end = 0;
    int place = -1;
    std::string path;
};

// The tracked entries of each archive. Offsets only mean something inside one archive, so the archive is found first.
class AreaIndex {
public:
    void Add(std::string_view archive, uint64_t offset, uint64_t size, int place, std::string path) {
        if (size == 0) return;
        ArchiveAreas* found = Named(archive);
        if (!found) {
            archives_.push_back({std::string(archive), {}});
            found = &archives_.back();
        }
        found->areas.push_back({offset, offset + size, place, std::move(path)});
    }

    // Once, after the last Add. Entries sharing their bytes resolve to the place, then to the first path.
    void Seal() {
        for (ArchiveAreas& archive : archives_) {
            std::sort(archive.areas.begin(), archive.areas.end(), [](const Area& a, const Area& b) {
                if (a.begin != b.begin) return a.begin < b.begin;
                if ((a.place >= 0) != (b.place >= 0)) return b.place >= 0;
                return a.path > b.path;
            });
        }
    }

    size_t size() const {
        size_t total = 0;
        for (const ArchiveAreas& archive : archives_) total += archive.areas.size();
        return total;
    }

    // The area a read starts in, or the first one starting inside it. Runs on the game's file threads for every read:
    // no allocation, and nothing that can fail.
    const Area* Find(const char* guest_path, uint64_t offset, uint64_t size) const noexcept {
        const char* slash = std::strrchr(guest_path, '/');
        const ArchiveAreas* archive = Named(slash ? slash + 1 : guest_path);
        if (!archive) return nullptr;
        const std::vector<Area>& areas = archive->areas;
        const auto next = std::upper_bound(areas.begin(), areas.end(), offset,
                                           [](uint64_t at, const Area& area) { return at < area.begin; });
        if (next != areas.begin() && offset < std::prev(next)->end) return &*std::prev(next);
        return next != areas.end() && next->begin - offset < size ? &*next : nullptr;
    }

private:
    struct ArchiveAreas {
        std::string name;
        std::vector<Area> areas; // by begin
    };

    static bool SameName(std::string_view a, std::string_view b) noexcept {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                   return std::toupper(x) == std::toupper(y);
               });
    }

    const ArchiveAreas* Named(std::string_view archive) const noexcept {
        for (const ArchiveAreas& candidate : archives_) {
            if (SameName(candidate.name, archive)) return &candidate;
        }
        return nullptr;
    }
    ArchiveAreas* Named(std::string_view archive) noexcept {
        return const_cast<ArchiveAreas*>(std::as_const(*this).Named(archive));
    }

    std::vector<ArchiveAreas> archives_;
};

struct Status {
    bool online = false;
    int cls = -1, level = 0, players = 1, place = -1;
};

// Online: the room's players, AI allies and this PC's other pads are not members. Offline: `local_humans`.
inline int PlayerCount(bool online, int room_members, int local_humans) {
    return std::clamp(online ? room_members : local_humans, 1, kPartyLimit);
}

// The SET_ACTIVITY arguments. Text only: the Discord application has no Rich Presence art.
inline std::string Activity(const Status& s, long long start, uint32_t pid) {
    const Place* place = s.place >= 0 && s.place < kPlaceCount ? &kPlaces[s.place] : nullptr;
    const std::string mode = s.online ? (s.players > 1 ? "Online co-op" : "Online, waiting for players")
                                      : (s.players > 1 ? "Couch co-op" : "Solo");
    std::string details = place ? place->name : "In menus", state = mode;
    if (s.cls >= 0 && s.cls < kClassCount) {
        details = kClasses[s.cls];
        if (s.level >= 1 && s.level <= kHighestLevel) details += " · Lv " + std::to_string(s.level);
        if (place) state = std::string(place->name) + " · " + mode;
    }
    return "{\"pid\":" + std::to_string(pid) + ",\"activity\":{\"details\":\"" + details + "\",\"state\":\"" + state +
           "\",\"timestamps\":{\"start\":" + std::to_string(start) + "},\"party\":{\"id\":\"dcp\",\"size\":[" +
           std::to_string(s.players) + "," + std::to_string(kPartyLimit) + "]}}}";
}

inline std::string Handshake(std::string_view client_id) {
    return "{\"v\":1,\"client_id\":\"" + std::string(client_id) + "\"}";
}

inline std::string SetActivity(unsigned nonce, std::string_view activity) {
    return "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + std::to_string(nonce) + "\",\"args\":" + std::string(activity) + "}";
}

} // namespace dcp::discord

#endif
