// Discord Rich Presence: the parts that need neither Discord nor the game (tools/test.ps1).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "discord_core.h"

using namespace dcp::discord;

namespace {

const char* const kOfficialClientId = "1568342665970458705";

int g_checks = 0, g_failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(condition)) {                                                  \
            ++g_failures;                                                    \
            std::printf("FAIL line %d: %s\n", __LINE__, #condition);         \
        }                                                                    \
    } while (0)

// A Discord that moves at most `chunk` bytes per call and fails once its script runs out.
struct FakeLink {
    std::string incoming, outgoing;
    size_t chunk = 3;
    int sends_left = 1000;
    int64_t Send(const void* data, size_t size) {
        if (sends_left-- <= 0) return -1;
        const size_t n = std::min(size, chunk);
        outgoing.append(static_cast<const char*>(data), n);
        return int64_t(n);
    }
    int64_t Receive(void* data, size_t size) {
        const size_t n = std::min({size, chunk, incoming.size()});
        if (n == 0) return 0;
        std::memcpy(data, incoming.data(), n);
        incoming.erase(0, n);
        return int64_t(n);
    }
};

void TestClientIds() {
    CHECK(ValidClientId(kOfficialClientId));
    CHECK(ValidClientId("12345678901234567") && ValidClientId("12345678901234567890"));
    CHECK(!ValidClientId("") && !ValidClientId("1234567890123456") && !ValidClientId("123456789012345678901"));
    CHECK(!ValidClientId("15683426659704587a5") && !ValidClientId("1568342665970458705\"") &&
          !ValidClientId(" 1568342665970458705"));
}

void TestFrames() {
    const std::string frame = Frame(kOpFrame, "abc");
    CHECK(frame.size() == 11 && frame.compare(8, 3, "abc") == 0);
    CHECK(std::memcmp(frame.data(), "\x01\0\0\0\x03\0\0\0", 8) == 0);
    CHECK(Frame(kOpHandshake, "").size() == 8);
    CHECK(Handshake("123") == "{\"v\":1,\"client_id\":\"123\"}");
    CHECK(SetActivity(7, "{}") == "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"7\",\"args\":{}}");
}

void TestExchange() {
    std::string answer;
    {
        // Partial writes and reads.
        FakeLink link;
        link.incoming = Frame(kOpFrame, "ok");
        CHECK(Exchange(link, kOpFrame, "hello world", answer) == Reply::kAnswered && answer == "ok");
        CHECK(link.outgoing == Frame(kOpFrame, "hello world"));
    }
    {
        FakeLink link;
        link.sends_left = 1;
        CHECK(Exchange(link, kOpFrame, "hello world", answer) == Reply::kBroken);
    }
    {
        // Answered while Discord pings first; a stray pong is ignored.
        FakeLink link;
        link.incoming = Frame(kOpPing, "beat") + Frame(kOpPong, "") + Frame(kOpFrame, "ok");
        CHECK(Exchange(link, kOpFrame, "x", answer) == Reply::kAnswered && answer == "ok");
        CHECK(link.outgoing == Frame(kOpFrame, "x") + Frame(kOpPong, "beat"));
    }
    {
        // An invalid client id.
        FakeLink link;
        link.incoming = Frame(kOpClose, "{\"code\":4000,\"message\":\"Invalid Client ID\"}");
        CHECK(Exchange(link, kOpHandshake, "x", answer) == Reply::kClosed && answer.find("4000") != std::string::npos);
    }
    const auto broken = [&](std::string incoming) {
        FakeLink link;
        link.incoming = std::move(incoming);
        return Exchange(link, kOpFrame, "x", answer) == Reply::kBroken;
    };
    CHECK(broken(""));                                 // closed before a reply
    CHECK(broken(Frame(kOpFrame, "ok").substr(0, 5))); // inside the header
    CHECK(broken(Frame(kOpFrame, "ok").substr(0, 9))); // inside the body
    CHECK(broken(Frame(kOpFrame, "")));                // an empty reply
    CHECK(broken(Frame(77, "ok")));                    // an unknown opcode
    std::string oversized = Frame(kOpFrame, "ok");
    const uint32_t too_long = kMaxReplyBytes + 1;
    std::memcpy(oversized.data() + 4, &too_long, 4);
    CHECK(broken(oversized));
    {
        std::string pings;
        for (int i = 0; i < kMaxFramesPerReply; ++i) pings += Frame(kOpPing, "");
        CHECK(broken(pings + Frame(kOpFrame, "ok")));
    }
    {
        FakeLink link;
        link.incoming = Frame(kOpPing, "beat") + Frame(kOpFrame, "ok");
        link.sends_left = 1;
        link.chunk = 1000;
        CHECK(Exchange(link, kOpFrame, "x", answer) == Reply::kBroken);
    }
    CHECK(ReportsError("{\"cmd\":\"SET_ACTIVITY\",\"evt\":\"ERROR\"}") && !ReportsError("{\"evt\":null}") && !ReportsError("}{"));
}

void TestPlaces() {
    CHECK(kPlaces[PlaceOf("round/LostZone_01.brb")].name == std::string("Lost Zone"));
    CHECK(kPlaces[PlaceOf("stage/T_Prison.bin")].name == std::string("Prison"));
    CHECK(PlaceOf("msgsheet/Menu_US.fms") < 0 && PlaceOf("Round/LostZone") < 0 && PlaceOf("") < 0);
    for (const Place& place : kPlaces) {
        const std::string text = std::string(place.prefix) + place.name;
        CHECK(text.find('"') == std::string::npos && text.find('\\') == std::string::npos);
    }
}

void TestAreaLookup() {
    AreaIndex areas;
    const int town = PlaceOf("round/Town00"), zone = PlaceOf("round/LostZone"), inn = PlaceOf("stage/T_ChrSelect");
    // The two archives overlap: the same offsets name different entries.
    areas.Add("DRACRO.CPK", 1000, 100, town, "round/Town00.brb");
    areas.Add("DRACRO.CPK", 5000, 100, zone, "round/LostZone.brb");
    areas.Add("DRACRO_US.CPK", 1050, 100, inn, "stage/T_ChrSelect_US.bin");
    areas.Add("DRACRO_US.CPK", 1040, 0, zone, "empty");
    areas.Seal();
    CHECK(areas.size() == 3);
    const auto place = [&](const char* guest, uint64_t offset, uint64_t size) {
        const Area* area = areas.Find(guest, offset, size);
        return area ? area->place : -1;
    };

    // Inside an entry whose begin is not the nearest one across all archives.
    CHECK(place("/app0/DRACRO.CPK", 1060, 16) == town);
    CHECK(place("/app0/DRACRO_US.CPK", 1060, 16) == inn);
    CHECK(place("/app0/DRACRO_US.CPK", 1149, 1) == inn);
    // Out of the same offset, only the read file's entry answers.
    CHECK(place("/app0/DRACRO_US.CPK", 1000, 16) == -1);
    CHECK(place("/app0/DRACRO_US.CPK", 5000, 16) == -1);
    // Bounds of an entry, and a read that only reaches it.
    CHECK(place("/app0/DRACRO.CPK", 1000, 1) == town && place("/app0/DRACRO.CPK", 1099, 1) == town);
    CHECK(place("/app0/DRACRO.CPK", 1100, 16) == -1);
    CHECK(place("/app0/DRACRO.CPK", 900, 100) == -1 && place("/app0/DRACRO.CPK", 900, 101) == town);
    CHECK(place("/app0/DRACRO.CPK", 999, 8000) == town);
    CHECK(place("/app0/DRACRO.CPK", 4000, ~uint64_t{0}) == zone);
    CHECK(place("/app0/DRACRO.CPK", ~uint64_t{0} - 1, ~uint64_t{0}) == -1);
    // Other files, whatever the case of the path.
    CHECK(place("/app0/dracro_us.cpk", 1060, 16) == inn && place("DRACRO.CPK", 1060, 16) == town);
    CHECK(place("/app0/DRACRO.CPK.undo", 1060, 16) == -1 && place("/app0/OTHER.CPK", 1060, 16) == -1);
    CHECK(place("", 1060, 16) == -1 && place("/app0/", 1060, 16) == -1);

    AreaIndex shared;
    shared.Add("A.CPK", 100, 10, -1, "a");
    shared.Add("A.CPK", 100, 10, town, "c");
    shared.Add("A.CPK", 100, 10, zone, "b");
    shared.Add("A.CPK", 100, 10, -1, "d");
    shared.Seal();
    CHECK(shared.Find("A.CPK", 100, 1)->path == "b");

    const AreaIndex empty;
    CHECK(empty.Find("/app0/DRACRO.CPK", 0, 100) == nullptr);
}

void TestPlayerCount() {
    CHECK(PlayerCount(false, 0, 1) == 1 && PlayerCount(false, 0, 3) == 3);
    CHECK(PlayerCount(false, 0, 0) == 1 && PlayerCount(false, 0, 9) == kPartyLimit);
    // Online: the room alone, pads on this PC do not make a party.
    CHECK(PlayerCount(true, 0, 2) == 1 && PlayerCount(true, 1, 1) == 1 && PlayerCount(true, 3, 1) == 3);
    CHECK(PlayerCount(true, 9, 1) == kPartyLimit);
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void TestActivity() {
    Status s;
    std::string a = Activity(s, 100, 7);
    CHECK(Has(a, "\"pid\":7,") && Has(a, "\"details\":\"In menus\"") && Has(a, "\"state\":\"Solo\""));
    CHECK(Has(a, "\"start\":100") && Has(a, "\"size\":[1,4]"));

    // No class: the place stands on its own, never "In menus" on a stage.
    s.place = PlaceOf("round/LostZone");
    a = Activity(s, 100, 7);
    CHECK(Has(a, "\"details\":\"Lost Zone\"") && Has(a, "\"state\":\"Solo\"") && !Has(a, "In menus"));

    s.cls = 2;
    s.level = 42;
    CHECK(Activity(s, 100, 7) ==
          "{\"pid\":7,\"activity\":{\"details\":\"Elf · Lv 42\",\"state\":\"Lost Zone · Solo\","
          "\"timestamps\":{\"start\":100},\"party\":{\"id\":\"dcp\",\"size\":[1,4]}}}");

    // A level the game cannot have keeps the class and drops the number.
    for (int level : {0, -1, 256}) {
        s.level = level;
        a = Activity(s, 100, 7);
        CHECK(Has(a, "\"details\":\"Elf\"") && !Has(a, "Lv"));
    }
    s.level = 255;
    CHECK(Has(Activity(s, 100, 7), "Lv 255"));

    s.place = -1;
    s.players = 2;
    a = Activity(s, 100, 7);
    CHECK(Has(a, "\"state\":\"Couch co-op\"") && Has(a, "\"size\":[2,4]"));
    s.online = true;
    CHECK(Has(Activity(s, 100, 7), "\"state\":\"Online co-op\""));
    s.players = 1;
    CHECK(Has(Activity(s, 100, 7), "\"state\":\"Online, waiting for players\""));

    // Out of range is the generic presence, not an out-of-bounds read.
    s = Status{};
    s.cls = kClassCount;
    s.place = kPlaceCount;
    CHECK(Activity(s, 100, 7) ==
          "{\"pid\":7,\"activity\":{\"details\":\"In menus\",\"state\":\"Solo\","
          "\"timestamps\":{\"start\":100},\"party\":{\"id\":\"dcp\",\"size\":[1,4]}}}");
}

bool NamesArt(const std::string& activity) {
    static const char* const kFields[] = {"assets", "large_image", "small_image", "large_text", "small_text"};
    static const char* const kKeys[] = {
        "icon", "fighter", "amazon", "elf", "dwarf", "sorceress", "wizard",
        "menu", "town", "gate", "stables", "inn", "guild", "magicshop", "lucain", "temple", "castle", "prison", "camp",
        "tutorial", "arena", "labyrinth", "ancientlabyrinth", "castleofthedead", "sanctuary", "ghostshipcove",
        "lostwoods", "lostzone", "magetower", "moldydungeon", "templeruins", "oldcapital",
    };
    for (const char* field : kFields) {
        if (Has(activity, field)) return true;
    }
    for (const char* key : kKeys) {
        if (Has(activity, "\"" + std::string(key) + "\"")) return true;
    }
    return false;
}

void TestNoArt() {
    Status s;
    s.level = 42;
    for (const bool online : {false, true}) {
        s.online = online;
        for (s.players = 1; s.players <= kPartyLimit; ++s.players) {
            for (s.cls = -1; s.cls <= kClassCount; ++s.cls) {
                for (s.place = -1; s.place <= kPlaceCount; ++s.place) CHECK(!NamesArt(Activity(s, 100, 7)));
            }
        }
    }
}

std::string Source(const char* file) {
    std::ifstream in(std::string(DCP_SOURCE_DIR) + "/" + file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void TestWiring() {
    const std::string loader = Source("src/loader.c"), presence = Source("src/discord_presence.cpp");
    // Without CACHE: an existing build folder must not keep a previous application id.
    CHECK(Has(Source("CMakeLists.txt"), "set(DCP_DISCORD_CLIENT_ID \"" + std::string(kOfficialClientId) + "\")\n"));
    CHECK(Has(loader, "const int discord_presence=!check_imports && !cpu_only && discord_presence_start(settings_file,app0);"));
    CHECK(Has(loader, "if (discord_presence) runtime_cheats_set_player_observer(party_hook_observe_unit);"));
    // Nothing thrown may reach the game: the C entry and the thread both end in a catch.
    CHECK(Has(presence, "extern \"C\" int discord_presence_start(const char* settings_file, const char* app0) {\n    try {"));
    CHECK(Has(presence, "void Worker(") && Has(presence, "noexcept {\n    LoadPlaces(app0, trace);\n    try {\n        Run("));
    CHECK(Has(presence, "void LoadPlaces(") && Has(presence, "noexcept {\n    try {\n        auto areas"));
    CHECK(Has(presence, "void ObserveRead(") && Has(presence, "size) noexcept {"));
}

} // namespace

int main() {
    TestClientIds();
    TestFrames();
    TestExchange();
    TestPlaces();
    TestAreaLookup();
    TestPlayerCount();
    TestActivity();
    TestNoArt();
    TestWiring();
    std::printf("discord tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
