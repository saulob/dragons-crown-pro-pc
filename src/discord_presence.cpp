// Discord Rich Presence over Discord's local IPC (named pipe on Windows, unix socket on Linux). No SDK: a frame is
// [opcode u32][length u32][JSON]. Opcode 0 is the handshake, 1 a command. A missing or closed Discord is retried
// quietly; closing the game closes the connection, and Discord then clears the presence itself.
#include "discord_presence.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "../gpu/shim/dcp_settings.h"
#include "cpk_patch.h"
#include "dcp_party.h"
#include "runtime_online.h"

extern "C" { // runtime.h has no C++ linkage block
int runtime_pad_slot_human(int slot);
void runtime_file_set_read_observer(void (*observe)(const char* guest, uint64_t offset, uint64_t size));
}

#ifndef DCP_DISCORD_CLIENT_ID
#define DCP_DISCORD_CLIENT_ID ""
#endif

namespace {

#ifdef _WIN32
using Handle = HANDLE;
const Handle kNone = INVALID_HANDLE_VALUE;

Handle Connect() {
    for (int i = 0; i < 10; ++i) {
        const std::string pipe = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
        Handle h = CreateFileA(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return kNone;
}
bool Write(Handle h, const void* data, uint32_t size) {
    DWORD done = 0;
    return WriteFile(h, data, size, &done, nullptr) && done == size;
}
bool Read(Handle h, void* data, uint32_t size) {
    for (uint32_t got = 0; got < size;) {
        DWORD n = 0;
        if (!ReadFile(h, static_cast<char*>(data) + got, size - got, &n, nullptr) || n == 0) return false;
        got += n;
    }
    return true;
}
void Close(Handle h) { CloseHandle(h); }
uint32_t Pid() { return GetCurrentProcessId(); }
#else
using Handle = int;
const Handle kNone = -1;

Handle Connect() {
    const char* dir = nullptr;
    for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
        if ((dir = std::getenv(var))) break;
    const std::string base = dir ? dir : "/tmp";
    // Native, Flatpak and Snap Discord.
    for (const char* sub : {"/", "/app/com.discordapp.Discord/", "/snap.discord/"}) {
        for (int i = 0; i < 10; ++i) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s%sdiscord-ipc-%d", base.c_str(), sub, i);
            const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0) return kNone;
            if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return fd;
            close(fd);
        }
    }
    return kNone;
}
bool Write(Handle h, const void* data, uint32_t size) {
    return send(h, data, size, MSG_NOSIGNAL) == static_cast<ssize_t>(size);
}
bool Read(Handle h, void* data, uint32_t size) {
    for (uint32_t got = 0; got < size;) {
        const ssize_t n = recv(h, static_cast<char*>(data) + got, size - got, 0);
        if (n <= 0) return false;
        got += static_cast<uint32_t>(n);
    }
    return true;
}
void Close(Handle h) { close(h); }
uint32_t Pid() { return static_cast<uint32_t>(getpid()); }
#endif

// Sends one frame and waits for Discord's reply, so its answers never pile up in the pipe.
bool Exchange(Handle h, uint32_t op, const std::string& json) {
    const uint32_t header[2] = {op, static_cast<uint32_t>(json.size())}; // little endian on every target
    if (!Write(h, header, sizeof(header)) || !Write(h, json.data(), header[1])) return false;
    uint32_t reply[2];
    if (!Read(h, reply, sizeof(reply)) || reply[1] > (1u << 20)) return false;
    std::string body(reply[1], '\0');
    if (!Read(h, body.data(), reply[1])) return false;
    if (body.find("\"evt\":\"ERROR\"") != std::string::npos) std::printf("Discord: %s\n", body.c_str());
    return reply[0] == 1; // 2 is Discord closing the connection
}

const char* const kClasses[] = {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"};

// Where the player is, from which file the game reads out of its CPK archives: each area has its own map file
// (round/*.brb) and each town building its own scene (stage/T_*). `key` is the Discord art asset.
struct Place {
    const char* prefix; // of the entry path in the archive
    const char* key;
    const char* name;
};
// Names are the game's own English text (msgsheet/*_US.fms). Checked in play, except the one marked *.
// ponytail: Prison is not matched to its in-game area yet. DCP_DISCORD_TRACE=1 logs every archive read,
// to find what loads where.
const Place kPlaces[] = {
    {"stage/title", "menu", "Title screen"},
    {"stage/T_ChrSelect", "inn", "Dragon's Haven Inn"},
    {"stage/T_FightersGuild", "guild", "Adventurers Guild"},
    {"stage/T_Shop", "magicshop", "Morgan's Magic Item Shop"},      // both load on one visit to Morgan's
    {"stage/T_MagesGuild", "magicshop", "Morgan's Magic Item Shop"},
    {"stage/T_MagesTower", "lucain", "Lucain's Tower"},
    {"stage/T_Temple", "temple", "Canaan Temple"},
    {"stage/T_Castle", "castle", "Castle"},
    {"stage/T_Prison", "prison", "Prison"}, // *
    {"round/Town00", "town", "Town"},
    // The town's street scenes, loaded again on every walk between them; the street one also on leaving a building.
    {"stage/bg25a", "town", "Town"},
    {"stage/bg01a", "gate", "Town Gate"},
    {"stage/bg41a", "stables", "Stables"},
    {"round/Camp_", "camp", "Camp"},
    {"stage/camp_", "camp", "Camp"},
    {"round/Tutorial", "tutorial", "Tutorial"},
    {"round/Arena", "arena", "Colosseum"},
    {"round/RandomDungeon", "labyrinth", "Labyrinth of Chaos"},
    {"round/AncientLabyrinth", "ancientlabyrinth", "Ancient Labyrinth"},
    {"round/Catacomb", "castleofthedead", "Castle of the Dead"},
    {"round/ForgottenRealms", "sanctuary", "Forgotten Sanctuary"},
    {"round/GhostshipCove", "ghostshipcove", "Ghost Ship Cove"},
    {"round/LostWoods", "lostwoods", "Lost Woods"},
    {"round/LostZone", "lostzone", "Lost Zone"},
    {"round/MageTower", "magetower", "Mage's Tower"},
    {"round/MoldyDungeon", "moldydungeon", "Moldy Dungeon"},
    {"round/MysticRuins", "templeruins", "Ancient Temple Ruins"},
    {"round/UnderCitadel", "oldcapital", "Old Capital"},
};

struct Area {
    std::string archive; // file name, upper case
    uint64_t begin, end;
    int place;
    std::string path;
};
std::vector<Area> g_areas; // sorted by begin; filled before the read observer is set, then read only
std::atomic<int> g_place{-1};

// A new place is logged, so the mapping can be checked against play. With DCP_DISCORD_TRACE=1 the other archive
// entries are tracked too (place -1) and each one read is logged, to find what a part of the game loads.
void Enter(const Area& a) {
    if (a.place < 0) {
        static std::atomic<const Area*> last{nullptr};
        if (last.exchange(&a, std::memory_order_relaxed) != &a) std::printf("Discord: read %s\n", a.path.c_str());
        return;
    }
    if (g_place.exchange(a.place, std::memory_order_relaxed) != a.place)
        std::printf("Discord: place %s (%s)\n", kPlaces[a.place].name, a.path.c_str());
}

std::string Upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

bool SameName(const std::string& upper, const char* name) {
    size_t i = 0;
    for (; name[i] && i < upper.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(name[i])) != upper[i]) return false;
    return i == upper.size() && !name[i];
}

void LoadAreas(const std::filesystem::path& app0) {
    const char* t = std::getenv("DCP_DISCORD_TRACE");
    const bool trace = t && *t == '1';
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(app0, ec)) {
        if (Upper(item.path().extension().string()) != ".CPK") continue;
        dcp::CpkArchive archive;
        if (!archive.Open(item.path())) continue;
        const std::string name = Upper(item.path().filename().string());
        for (const dcp::CpkEntry& entry : archive.entries()) {
            int place = -1;
            for (int p = 0; p < int(std::size(kPlaces)) && place < 0; ++p)
                if (entry.path.rfind(kPlaces[p].prefix, 0) == 0) place = p;
            if (place >= 0 || (trace && entry.path.rfind("snd/", 0) != 0)) // streamed sound would flood the trace
                g_areas.push_back({name, entry.offset, entry.offset + entry.size, place, entry.path});
        }
    }
    std::sort(g_areas.begin(), g_areas.end(), [](const Area& a, const Area& b) { return a.begin < b.begin; });
}

// Runs on the game's file threads for every read: a binary search, no allocation.
void ObserveRead(const char* guest, uint64_t offset, uint64_t size) {
    const char* slash = std::strrchr(guest, '/');
    const char* base = slash ? slash + 1 : guest;
    auto it = std::upper_bound(g_areas.begin(), g_areas.end(), offset,
                               [](uint64_t at, const Area& a) { return at < a.begin; });
    // The area the read starts in, or one starting inside a longer read.
    if (it != g_areas.begin()) {
        const Area& a = *std::prev(it);
        if (offset < a.end && SameName(a.archive, base)) return Enter(a);
    }
    for (; it != g_areas.end() && it->begin < offset + size; ++it)
        if (SameName(it->archive, base)) return Enter(*it);
}

// This PC's player and the party size. Class and level come from the player-unit tick (dcp_party.h), so they are
// known only while a character is on screen; offline, AI allies (party slots without a controller) are not counted.
struct Status {
    bool online = false;
    int cls = -1, level = 0, players = 1, place = -1;
};

Status Current() {
    Status s;
    s.online = runtime_online_active() != 0;
    s.place = g_place.load(std::memory_order_relaxed);
    const dcp::online::RoomView room = s.online ? dcp::online::room_view() : dcp::online::RoomView{};
    const int64_t now = dcp::party::NowMs();
    const dcp::party::UnitTable& units = dcp::party::SharedUnits();
    if (const auto cls = units.ClassAt(room.slot, now)) {
        s.cls = *cls;
        s.level = *units.LevelAt(room.slot, now);
    }
    if (room.members > 0) {
        s.players = room.members;
    } else {
        s.players = 0;
        for (int slot = 0; slot < 4; ++slot) s.players += runtime_pad_slot_human(slot) ? 1 : 0;
    }
    return s;
}

// The SET_ACTIVITY arguments. Large image: the class being played (the game's icon in menus). Small image: the place.
std::string Activity(const Status& s, long long start) {
    std::string large = "icon", large_text = "Dragon's Crown Pro", details = "In menus";
    if (s.cls >= 0) {
        large = kClasses[s.cls];
        for (char& c : large) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        details = large_text = std::string(kClasses[s.cls]) + " · Lv " + std::to_string(s.level);
    }
    const std::string mode = s.online ? (s.players > 1 ? "Online co-op" : "Online, waiting for players")
                                      : (s.players > 1 ? "Couch co-op" : "Solo");
    std::string state = mode, small;
    if (s.place >= 0) {
        const Place& p = kPlaces[s.place];
        state = std::string(p.name) + " · " + mode;
        small = ",\"small_image\":\"" + std::string(p.key) + "\",\"small_text\":\"" + p.name + "\"";
    }
    return "{\"pid\":" + std::to_string(Pid()) + ",\"activity\":{\"details\":\"" + details + "\",\"state\":\"" +
           state + "\",\"timestamps\":{\"start\":" + std::to_string(start) + "},\"party\":{\"id\":\"dcp\",\"size\":[" +
           std::to_string(s.players) + ",4]},\"assets\":{\"large_image\":\"" + large + "\",\"large_text\":\"" +
           large_text + "\"" + small + "}}}";
}

void Run(std::string client_id) {
    const long long start = static_cast<long long>(std::time(nullptr));
    Handle h = kNone;
    std::string sent;
    for (;; std::this_thread::sleep_for(std::chrono::seconds(5))) {
        if (h == kNone) {
            h = Connect();
            if (h == kNone) continue;
            if (!Exchange(h, 0, "{\"v\":1,\"client_id\":\"" + client_id + "\"}")) {
                Close(h);
                h = kNone;
                continue;
            }
            std::printf("Discord: connected\n");
            sent.clear();
        }
        const std::string activity = Activity(Current(), start);
        if (activity == sent) continue;
        static unsigned nonce = 0; // Discord matches replies by nonce: one per command
        if (!Exchange(h, 1, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + std::to_string(++nonce) + "\",\"args\":" +
                                activity + "}")) {
            Close(h);
            h = kNone;
            continue;
        }
        sent = activity;
    }
}

} // namespace

extern "C" void discord_presence_start(const char* settings_file, const char* app0) {
    bool enabled = true;
    std::string client_id = DCP_DISCORD_CLIENT_ID;
    std::ifstream file(settings_file ? settings_file : "");
    for (std::string line, key, value; std::getline(file, line);) {
        if (!DcpSettings::ParseLine(line, key, value)) continue;
        if (key == "discord_presence") enabled = DcpSettings::ParseBool(value, enabled);
        if (key == "discord_client_id") client_id = value;
    }
    // The id goes into JSON unescaped: digits only.
    if (!enabled || client_id.empty() || client_id.find_first_not_of("0123456789") != std::string::npos) {
        std::printf("Discord: presence off\n");
        return;
    }
    if (app0) LoadAreas(app0);
    std::printf("Discord: %zu area files tracked\n", g_areas.size());
    if (!g_areas.empty()) runtime_file_set_read_observer(ObserveRead);
    std::thread(Run, client_id).detach();
}
