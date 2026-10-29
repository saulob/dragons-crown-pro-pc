// Online play on every supported game build, without the game: each local boot image (build/image/<folder>/boot.bin)
// has every online-library import the game itself binds resolved by the runtime's own resolver, the same entry points
// serve every build, every build meets in one lobby game, and Online Play keeps the cheats locked. With DCP_TEST_LOBBY,
// rooms between different builds through a development lobby.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "dcp_cheat_menu.h"
#include "dcp_cheats.h"
#include "game_profile.h"
#include "net/identity.h"
#include "net/json.h"
#include "net/lobby_link.h"
#include "net/online_version.h"
#include "runtime_cheats.h"
#include "runtime_online.h"

extern "C" {
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char* name, int is_data);
uintptr_t runtime_services_resolve(const char* name);
const char* runtime_symbol(const char* nid);
}

#define SYSV __attribute__((sysv_abi))

namespace fs = std::filesystem;
using namespace dcp;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

constexpr int32_t kNpSignedOut = int32_t(0x80550006);
constexpr char kSetDataA[] = "VzxN3tOouj8#libSceNpTus";

// What each part of online play needs from a build's imports.
const std::map<std::string, std::vector<std::string>> kPaths = {
    {"Matching2 initialization",
     {"sceNpMatching2Initialize", "sceNpMatching2CreateContextA", "sceNpMatching2RegisterContextCallback",
      "sceNpMatching2ContextStart", "sceNpMatching2GetServerId", "sceNpMatching2GetWorldInfoList"}},
    {"rooms",
     {"sceNpMatching2CreateJoinRoomA", "sceNpMatching2SearchRoom", "sceNpMatching2JoinRoomA", "sceNpMatching2LeaveRoom",
      "sceNpMatching2SetRoomDataExternal", "sceNpMatching2RegisterRoomEventCallback"}},
    {"friends and presence",
     {"_ZN3sce7Toolkit2NP2V26Friend10getFriendsERKNS3_7Request10GetFriendsEPNS2_4Core8ResponseINS3_7FriendsEEE",
      "_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_6Friend7FriendsEE3getEv",
      "_ZN3sce7Toolkit2NP2V24Core12ResponseBase8isLockedEv",
      "_ZN3sce7Toolkit2NP2V28Presence11setPresenceERKNS3_7Request11SetPresenceEPNS2_4Core8ResponseINS8_5EmptyEEE",
      "_ZN3sce7Toolkit2NP2V28Presence14deletePresenceERKNS3_7Request14DeletePresenceEPNS2_4Core8ResponseINS8_5EmptyEEE"}},
    {"P2P and Rudp",
     {"sceNpMatching2SignalingGetConnectionStatus", "sceNpMatching2SignalingGetLocalNetInfo", "sceRudpInit",
      "sceRudpCreateContext", "sceRudpBind", "sceRudpInitiate", "sceRudpWrite", "sceRudpRead", "sceRudpPollWait",
      "sceNetSocket", "sceNetBind"}},
    {"account", {"sceNpGetState", "sceNpGetNpId", "sceNpGetAccountIdA", "sceNpCheckCallback", "sceNpCheckPlus",
                 "sceNetCtlGetState", "sceNetCtlGetInfo"}},
    {"title user storage",
     {"sceNpTusCreateNpTitleCtxA", "sceNpTusCreateRequest", "sceNpTusSetDataA", "sceNpTusGetDataForCrossSave",
      "sceNpTusAbortRequest", "sceNpTusDeleteRequest", "sceNpTusDeleteNpTitleCtx"}},
};

// The imports of a boot image, and which of them the game binds itself (a code slot outside the bundled modules), as
// the loader marks them for runtime_online_check_imports.
struct BootImports {
    std::vector<std::array<char, 128>> names;
    std::vector<unsigned char> game;
    bool Check(uintptr_t (*resolve)(const char*, int)) const {
        return runtime_online_check_imports(reinterpret_cast<const char(*)[128]>(names.data()), game.data(),
                                            names.size(), resolve) != 0;
    }
};

uint64_t Read64(std::ifstream& in) {
    unsigned char b[8] = {};
    in.read(reinterpret_cast<char*>(b), 8);
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | b[i];
    return v;
}

// build/image/<folder>/boot.bin, laid out by launcher/boot_image.cpp.
bool ReadBootImports(const fs::path& path, BootImports& out) {
    std::ifstream in(path, std::ios::binary);
    char magic[8] = {};
    if (!in.read(magic, 8) || std::memcmp(magic, "DCPBOOT1", 8)) return false;
    Read64(in), Read64(in);
    const uint64_t segments = Read64(in), relocs = Read64(in), names = Read64(in);
    for (int i = 0; i < 2 + 4; ++i) Read64(in);
    std::vector<std::pair<uint64_t, uint64_t>> modules(Read64(in));
    for (auto& [base, size] : modules) {
        base = Read64(in), size = Read64(in);
        for (int i = 0; i < 5; ++i) Read64(in);
    }
    const uint64_t bindings = Read64(in);
    in.seekg(std::streamoff(bindings * 24 + segments * 24), std::ios::cur);
    out.names.assign(names, {});
    for (auto& name : out.names) in.read(name.data(), 128);
    out.game.assign(names, 0);
    for (uint64_t i = 0; i < relocs; ++i) {
        const uint64_t target = Read64(in), kind = Read64(in), value = Read64(in);
        Read64(in);
        bool in_module = false;
        for (const auto& [base, size] : modules) in_module = in_module || (target >= base && target - base < size);
        if (kind == 1 && value < names && !in_module) out.game[value] = 1;
    }
    return bool(in);
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

std::string Library(const char* name) { return std::strchr(name, '#') + 1; }
std::string Key(const GameBuild& build) { return std::string(build.title_id) + "-" + build.version; }

void Configure(const fs::path& base, const char* build, bool online) {
    const fs::path file = base / "dcp.ini";
    std::ofstream(file, std::ios::binary) << (online ? "online_enabled = 1\n" : "online_enabled = 0\n");
    runtime_online_configure(file.string().c_str(), base.string().c_str(), build);
}

uintptr_t ResolveWithoutSetDataA(const char* name, int is_data) {
    return std::strcmp(name, kSetDataA) ? runtime_resolve(name, is_data) : 0;
}

// sceNpTusSetDataA(reqId, accountId, slotId, totalSize, sendSize, data, info, infoStructSize, lastChangedAuthor,
// lastChangedDate, option), called the way the game's cross-save writer calls it: the result is not checked and the
// request is deleted next, so the call must return without touching the game's buffers.
void TestSetDataA(uintptr_t function) {
    using SetDataA = int32_t SYSV (*)(int32_t, uint64_t, int32_t, uint64_t, uint64_t, const void*, const void*, uint64_t,
                                      void*, void*, void*);
    const auto set = reinterpret_cast<SetDataA>(function);
    uint8_t data[0x100], info[0x188], author[8], date[8];
    std::memset(data, 0x5a, sizeof(data)), std::memset(info, 0x11, sizeof(info));
    std::memset(author, 0x22, sizeof(author)), std::memset(date, 0x33, sizeof(date));
    CHECK(set(5, 0x1234, 4, sizeof(data), sizeof(data), data, info, sizeof(info), author, date, nullptr) == kNpSignedOut);
    CHECK(data[0] == 0x5a && info[0x187] == 0x11 && author[0] == 0x22 && date[7] == 0x33);
    CHECK(set(5, 0x1234, 4, 0, 0, nullptr, nullptr, 0, nullptr, nullptr, nullptr) == kNpSignedOut);
}

void TestBuild(const fs::path& base, const GameBuild& build, std::map<std::string, uintptr_t>& reference) {
    BootImports boot;
    if (!ReadBootImports(fs::path(DCP_SOURCE_DIR) / "build" / "image" / build.folder / "boot.bin", boot)) {
        std::printf("%s: no boot image (build/image/%s): skipped\n", build.label, build.folder);
        return;
    }
    const std::string key = Key(build);
    Configure(base, key.c_str(), true);
    CHECK(runtime_online_active() == 1 && runtime_online_play_enabled() == 1);
    CHECK(online::settings().game == net::kOnlineGame);

    // Every online-library import the game binds: named, and resolved by the loader's resolver.
    std::map<std::string, size_t> per_library;
    std::map<std::string, uintptr_t> points;
    std::string tus;
    size_t unresolved = 0;
    for (size_t i = 0; i < boot.names.size(); ++i) {
        const char* name = boot.names[i].data();
        if (!boot.game[i] || !runtime_online_library(name)) continue;
        ++per_library[Library(name)];
        const char* symbol = runtime_symbol(name);
        const uintptr_t resolved = runtime_resolve(name, 0);
        if (!symbol || !resolved) {
            ++unresolved;
            std::printf("%s: %s (%s) does not resolve\n", build.label, name, symbol ? symbol : "unnamed");
            continue;
        }
        if (Library(name) == "libSceNpToolkit2") CHECK(runtime_online_resolve(name) == resolved);
        if (Library(name) == "libSceNpTus") tus += std::string(" ") + symbol;
        points[symbol] = resolved;
    }
    for (size_t i = 0; i < boot.names.size(); ++i) {
        if (!boot.game[i] || Library(boot.names[i].data()) != "libSceRtc") continue;
        ++per_library["libSceRtc"];
        if (!runtime_resolve(boot.names[i].data(), 0)) {
            ++unresolved;
            std::printf("%s: %s does not resolve\n", build.label, boot.names[i].data());
        }
    }
    CHECK(unresolved == 0);
    for (const char* library : {"libSceNpManager", "libSceNpToolkit2", "libSceNpMatching2", "libSceNpTus",
                                "libSceNetCtl", "libSceNet", "libSceRudp", "libSceRtc"})
        CHECK(per_library[library] > 0);
    CHECK(boot.Check(runtime_resolve));
    CHECK(runtime_online_active() == 1);
    std::printf("%s: %zu game online imports resolve; libSceNpTus:%s\n", build.label, points.size(), tus.c_str());

    for (const auto& [path, symbols] : kPaths) {
        for (const std::string& symbol : symbols) {
            const auto found = points.find(symbol);
            CHECK(found != points.end());
            if (found == points.end()) {
                std::printf("%s: %s: %s is not a resolved game import\n", build.label, path.c_str(), symbol.c_str());
                continue;
            }
            // One implementation for every build: the same entry point as the first build's.
            const auto [first, added] = reference.emplace(symbol, found->second);
            CHECK(added || first->second == found->second);
        }
    }

    // The cross-save writer's import: the libSceNpTus entry, callable with the game's arguments.
    size_t set_data = boot.names.size();
    for (size_t i = 0; i < boot.names.size(); ++i)
        if (!std::strcmp(boot.names[i].data(), kSetDataA)) set_data = i;
    CHECK(set_data < boot.names.size() && boot.game[set_data]);
    CHECK(runtime_symbol(kSetDataA) && !std::strcmp(runtime_symbol(kSetDataA), "sceNpTusSetDataA"));
    const uintptr_t set = runtime_resolve(kSetDataA, 0);
    CHECK(set != 0 && set == runtime_services_resolve(kSetDataA) && runtime_online_resolve(kSetDataA) == 0);
    if (set) TestSetDataA(set);

    // Without that implementation the check fails, and online play stays off while the cheats stay locked.
    CHECK(!boot.Check(ResolveWithoutSetDataA));
    CHECK(runtime_online_active() == 0 && runtime_online_play_enabled() == 1);

    // An unknown Toolkit2 import of the game cannot be left to the bundled module; one only a module binds is not the
    // game's.
    Configure(base, key.c_str(), true);
    BootImports unknown = boot;
    unknown.names.push_back({});
    std::snprintf(unknown.names.back().data(), 128, "AAAAAAAAAAA#libSceNpToolkit2");
    unknown.game.push_back(1);
    CHECK(!unknown.Check(runtime_resolve));
    CHECK(runtime_online_active() == 0 && runtime_online_play_enabled() == 1);
    Configure(base, key.c_str(), true);
    unknown.game.back() = 0;
    CHECK(unknown.Check(runtime_resolve) && runtime_online_active() == 1);

    Configure(base, key.c_str(), false);
    CHECK(runtime_online_active() == 0 && runtime_online_play_enabled() == 0);
    CHECK(boot.Check(ResolveWithoutSetDataA));
}

// What the loader does with the cheats once Online Play is read (src/loader.c): the lock from the effective setting,
// the saved values, the log line and the hook decision; then what the menu, F10 and the getters allow.
struct CheatRun {
    int log_state;
    bool hooks;
};
CheatRun StartCheats(const GameBuild& build) {
    runtime_cheats_set_online(runtime_online_play_enabled());
    unsigned all = 0;
    for (const cheats::CheatInfo& info : cheats::kCheats) all |= cheats::Bit(info.cheat);
    runtime_cheats_start(all, 10, 4);
    return {runtime_cheats_log_state(build.cheats), runtime_cheats_hooks_wanted(build.cheats) != 0};
}

void CheckCheatsLocked(const GameBuild& build, const CheatRun& run) {
    CHECK(runtime_cheats_online_locked() == 1);
    CHECK(run.log_state == RUNTIME_CHEATS_ONLINE_LOCKED && !run.hooks);
    for (const cheats::CheatInfo& info : cheats::kCheats) {
        CHECK(runtime_cheat_enabled(int(info.cheat)) == 0);
        runtime_cheat_toggle(int(info.cheat));
        CHECK(runtime_cheat_enabled(int(info.cheat)) == 0 && runtime_cheat_take_refill(int(info.cheat)) == 0);
    }
    for (size_t action = 0; action < cheats::kActionCount; ++action) {
        runtime_cheat_action(int(action));
        CHECK(runtime_cheat_action_pending(int(action)) == 0 && runtime_cheat_take_action(int(action)) == 0);
    }
    CHECK(runtime_cheat_refills_pending() == 0);
    CHECK(runtime_cheat_experience_multiplier() == cheats::kExperienceMultiplierOff &&
          runtime_cheat_experience_cycle() == cheats::kExperienceMultiplierOff);
    CHECK(runtime_cheat_movement_speed() == cheats::kMovementSpeedOff &&
          runtime_cheat_movement_cycle() == cheats::kMovementSpeedOff);
    CHECK(cheats::MenuKeyPress(false, runtime_cheats_online_locked() != 0, false, false) ==
          cheats::MenuKeyAction::OnlineHint);
    CHECK(cheats::MenuKeyPress(false, runtime_cheats_online_locked() != 0, false, true) ==
          cheats::MenuKeyAction::OnlineHint);
    std::printf("%s: cheats locked for Online Play\n", build.label);
}

// Online Play locks the cheats on every supported build, from dcp.ini or DCP_ONLINE; with it off each build keeps its
// own cheat support (native hooks on CUSA10454 1.00 only).
void TestCheatLockout(const fs::path& base) {
    for (const GameBuild& build : SupportedBuilds()) {
        const std::string key = Key(build);
        Configure(base, key.c_str(), false);
        CHECK(runtime_online_play_enabled() == 0);
        const CheatRun off = StartCheats(build);
        CHECK(runtime_cheats_online_locked() == 0);
        CHECK(off.log_state == (build.cheats ? RUNTIME_CHEATS_LISTED : RUNTIME_CHEATS_UNAVAILABLE));
        CHECK(off.hooks == build.cheats);
        CHECK(runtime_cheat_enabled(int(cheats::Cheat::InfiniteHealth)) == 1 && runtime_cheat_experience_multiplier() == 10);
        CHECK(cheats::MenuKeyPress(false, runtime_cheats_online_locked() != 0, false, false) == cheats::MenuKeyAction::Open);

        Configure(base, key.c_str(), true);
        CHECK(runtime_online_play_enabled() == 1 && runtime_online_active() == 1);
        CheckCheatsLocked(build, StartCheats(build));

        set_env("DCP_ONLINE", "1");
        Configure(base, key.c_str(), false);
        CHECK(runtime_online_play_enabled() == 1);
        CheckCheatsLocked(build, StartCheats(build));
        set_env("DCP_ONLINE", nullptr);

        Configure(base, key.c_str(), false);
        CHECK(StartCheats(build).log_state != RUNTIME_CHEATS_ONLINE_LOCKED && runtime_cheats_online_locked() == 0);
        CHECK(cheats::MenuKeyPress(false, false, false, false) == cheats::MenuKeyAction::Open);
    }

    // Asia/Hong Kong 1.00 has no cheat hooks; the log used to be chosen by that alone, so it said "enabled: none"
    // while Online Play had locked the cheats.
    const GameBuild& asia = SupportedBuilds()[5];
    CHECK(Key(asia) == "CUSA10209-1.00" && !asia.cheats);
    Configure(base, "CUSA10209-1.00", true);
    StartCheats(asia);
    const int by_build_only = asia.cheats ? RUNTIME_CHEATS_LISTED : RUNTIME_CHEATS_UNAVAILABLE;
    CHECK(runtime_cheats_online_locked() == 1 && by_build_only != RUNTIME_CHEATS_ONLINE_LOCKED);
    CHECK(runtime_cheats_log_state(asia.cheats) == RUNTIME_CHEATS_ONLINE_LOCKED);
    Configure(base, "CUSA10209-1.00", false);
    runtime_cheats_set_online(0);
}

// The lobby keeps games apart by this key only, so every pair of supported builds meets; the online protocol, sent
// in every hello, is what separates incompatible ports.
void TestOneLobbyGame(const fs::path& base) {
    for (const GameBuild& build : SupportedBuilds()) {
        Configure(base, Key(build).c_str(), true);
        CHECK(runtime_online_active() == 1);
        CHECK(online::settings().game == net::kOnlineGame);
    }
    Configure(base, nullptr, true);
    CHECK(runtime_online_active() == 0 && runtime_online_play_enabled() == 1);
    CHECK(net::online_compatible(net::kOnlineProtocolVersion));
    CHECK(!net::online_compatible(net::kOnlineProtocolVersion + 1) && !net::online_compatible(0));
}

const GameBuild* BuildNamed(const char* key) {
    for (const GameBuild& build : SupportedBuilds())
        if (Key(build) == key) return &build;
    return nullptr;
}

// What a build's game says in its lobby hello.
net::Json HelloOf(const fs::path& base, const char* build, const char* who) {
    Configure(base, build, true);
    const net::Identity identity = net::load_identity(base / who);
    return net::hello_body(identity, who, online::settings().game, false);
}

// Rooms between different supported builds through a development lobby (DCP_TEST_LOBBY): found and joined with the
// same online protocol, hidden and refused with another.
void TestCrossBuildRooms(const fs::path& base) {
    const char* url = std::getenv("DCP_TEST_LOBBY");
    if (!url || !*url) {
        std::printf("cross-build rooms skipped: set DCP_TEST_LOBBY to a running development lobby\n");
        return;
    }
    const struct {
        const char *host, *guest;
    } kPairs[] = {
        {"CUSA10454-1.00", "CUSA07001-1.03"}, {"CUSA10454-1.00", "CUSA10209-1.00"}, {"CUSA07001-1.00", "CUSA07001-1.03"},
        {"CUSA07001-1.02", "CUSA10209-1.02"}, {"CUSA10454-1.01", "CUSA07001-1.00"},
    };
    for (const auto& pair : kPairs) {
        CHECK(BuildNamed(pair.host) && BuildNamed(pair.guest));
        for (const bool same_protocol : {true, false}) {
            net::Json hello = HelloOf(base, pair.host, "Host");
            net::LobbySession host, guest;
            net::Json reply;
            std::string error, code;
            CHECK(host.open(url, hello, &reply, &error));
            net::Json room = net::Json::Object();
            room.set("world", 1);
            room.set("slots", 4);
            const auto created = host.call("room.create", room, &code);
            CHECK(created && code == "ok");
            const std::string room_code = created ? (*created)["room"]["code"].as_string() : "";

            hello = HelloOf(base, pair.guest, "Guest");
            if (!same_protocol) hello.set("online_protocol", net::kOnlineProtocolVersion + 1);
            CHECK(guest.open(url, hello, &reply, &error));
            net::Json search = net::Json::Object();
            search.set("code", room_code);
            const auto found = guest.call("room.search", search, &code);
            CHECK(found && code == "ok");
            CHECK(found && (*found)["total"].as_int() == (same_protocol ? 1 : 0));
            CHECK(found && (*found)["incompatible"].as_int() == (same_protocol ? 0 : 1));
            net::Json join = net::Json::Object();
            join.set("code", room_code);
            const auto joined = guest.call("room.join", join, &code);
            CHECK(same_protocol ? joined && code == "ok" : !joined && code == "incompatible");
            std::printf("%s host, %s guest, %s protocol: search %s, join %s\n", pair.host, pair.guest,
                        same_protocol ? "same" : "other", found ? std::to_string((*found)["total"].as_int()).c_str() : "-",
                        code.c_str());
            guest.close();
            host.close();
        }
    }
}

}  // namespace

int main() {
    runtime_start(1);
    const fs::path base = fs::temp_directory_path() / "dcp-online-builds-tests";
    fs::remove_all(base);
    fs::create_directories(base);
    std::map<std::string, uintptr_t> reference;
    for (const GameBuild& build : SupportedBuilds()) TestBuild(base, build, reference);
    TestOneLobbyGame(base);
    TestCheatLockout(base);
    TestCrossBuildRooms(base);
    fs::remove_all(base);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
