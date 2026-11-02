// Focused tests of the online party snapshot: what the observer takes from a unit, how a snapshot is built and when it is
// reported, the hook on a synthetic image, and the client's sources (the report's fields, its gates and the sites it
// needs). No game, no lobby.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "dcp_party.h"
#include "guest_hook.h"
#include "party_hook.h"

namespace fs = std::filesystem;
using namespace dcp::party;
namespace cheats = dcp::cheats;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                             \
    do {                                                        \
        ++g_checks;                                             \
        if (!(cond)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

// The game's memory as a map of little-endian cells.
class FakeMemory {
public:
    void Put32(uint64_t address, uint32_t value) { cells_[address] = value; }
    void Put64(uint64_t address, uint64_t value) {
        cells_[address] = static_cast<uint32_t>(value);
        cells_[address + 4] = static_cast<uint32_t>(value >> 32);
    }
    uint64_t U64(uint64_t address) const { return U32(address) | static_cast<uint64_t>(U32(address + 4)) << 32; }
    uint32_t U32(uint64_t address) const {
        const auto it = cells_.find(address);
        return it == cells_.end() ? 0 : it->second;
    }
    uint8_t U8(uint64_t address) const {
        const uint64_t base = address & ~uint64_t{3};
        return static_cast<uint8_t>(U32(base) >> (8 * (address & 3)));
    }
    void PutByte(uint64_t address, uint8_t value) {
        const uint64_t base = address & ~uint64_t{3};
        const unsigned shift = 8 * (address & 3);
        cells_[base] = (U32(base) & ~(0xffu << shift)) | (uint32_t{value} << shift);
    }

private:
    std::map<uint64_t, uint32_t> cells_;
};

// A player unit at `unit` with its owner at `unit + 0x100000`.
void PutUnit(FakeMemory& memory, uint64_t unit, uint32_t category, int32_t slot, uint8_t class_index) {
    const uint64_t owner = unit + 0x100000;
    memory.Put64(unit + cheats::kUnitOwnerOffset, owner);
    memory.Put32(owner + cheats::kOwnerCategoryOffset, category);
    memory.Put32(unit + cheats::kUnitPlayerSlotOffset, static_cast<uint32_t>(slot));
    memory.PutByte(unit + cheats::kUnitClassOffset, class_index);
}

std::string Source(const char* relative) {
    std::ifstream in(fs::path(DCP_SOURCE_DIR) / relative, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
    const size_t begin = text.find(from);
    const size_t end = begin == std::string::npos ? begin : text.find(to, begin);
    return end == std::string::npos ? std::string() : text.substr(begin, end - begin);
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

Snapshot Of(std::initializer_list<uint8_t> classes) {
    Snapshot snapshot;
    for (const uint8_t c : classes) snapshot.classes[snapshot.size++] = c;
    std::sort(snapshot.classes.begin(), snapshot.classes.begin() + snapshot.size);
    return snapshot;
}

void TestClassesAndSlots() {
    for (uint32_t c = 0; c < 6; ++c) CHECK(ValidClass(c));
    for (const uint32_t c : {6u, 7u, 255u, 0xffffffffu}) CHECK(!ValidClass(c));
    CHECK(kClassCount == 6 && kSlots == 4);
    for (int slot = 0; slot < 4; ++slot) CHECK(ValidSlot(slot));
    for (const int slot : {-1, 4, 255}) CHECK(!ValidSlot(slot));

    CHECK(MemberSlot(0x10) == 0 && MemberSlot(0x20) == 1 && MemberSlot(0x30) == 2 && MemberSlot(0x40) == 3);
    CHECK(MemberSlot(0x1f) == 0 && MemberSlot(0x4f) == 3);
    for (const uint32_t id : {0u, 0x0fu, 0x50u, 0x60u, 0xffffu}) CHECK(MemberSlot(id) == -1);
}

void TestObserver() {
    FakeMemory memory;
    UnitTable table;
    PutUnit(memory, 0x1000, cheats::kHumanPlayerCategory, 0, 3);
    PutUnit(memory, 0x2000, cheats::kHumanPlayerCategory | 0x10, 2, 5);
    Observe(memory, 0x1000, 100, table);
    Observe(memory, 0x2000, 100, table);
    CHECK(table.ClassAt(0, 100) == 3 && table.ClassAt(2, 100) == 5);
    CHECK(!table.ClassAt(1, 100) && !table.ClassAt(3, 100));

    // Not a human player, an unknown class, a slot outside the party, no owner, no unit: nothing is recorded.
    UnitTable ignored;
    PutUnit(memory, 0x3000, 0, 1, 2);
    PutUnit(memory, 0x4000, cheats::kHumanPlayerCategory, 1, 255);
    PutUnit(memory, 0x5000, cheats::kHumanPlayerCategory, 1, 6);
    PutUnit(memory, 0x6000, cheats::kHumanPlayerCategory, 4, 2);
    PutUnit(memory, 0x7000, cheats::kHumanPlayerCategory, -1, 2);
    PutUnit(memory, 0x8000, cheats::kHumanPlayerCategory, 1, 2);
    memory.Put64(0x8000 + cheats::kUnitOwnerOffset, 0);
    for (const uint64_t unit : {uint64_t{0x3000}, uint64_t{0x4000}, uint64_t{0x5000}, uint64_t{0x6000}, uint64_t{0x7000},
                                uint64_t{0x8000}, uint64_t{0}}) {
        Observe(memory, unit, 100, ignored);
    }
    for (int slot = 0; slot < 4; ++slot) CHECK(!ignored.ClassAt(slot, 100));

    // A unit not seen for a while is gone; seen again it is back.
    CHECK(table.ClassAt(0, 100 + kFreshMs) == 3);
    CHECK(!table.ClassAt(0, 101 + kFreshMs));
    Observe(memory, 0x1000, 5000, table);
    CHECK(table.ClassAt(0, 5000) == 3);
    table.Clear();
    CHECK(!table.ClassAt(0, 5000));

    // Every one of the six classes is carried through.
    for (uint8_t c = 0; c < 6; ++c) {
        PutUnit(memory, 0x9000, cheats::kHumanPlayerCategory, 3, c);
        Observe(memory, 0x9000, 7000, table);
        CHECK(table.ClassAt(3, 7000) == c);
    }
}

std::unique_ptr<UnitTable> Party(std::initializer_list<std::pair<int, uint8_t>> units, int64_t now = 100) {
    auto table = std::make_unique<UnitTable>();
    for (const auto& [slot, class_index] : units) table->Seen(slot, class_index, now);
    return table;
}

void TestSnapshots() {
    const std::vector<uint16_t> two = {0x10, 0x20};
    const auto fighter_elf = Party({{0, 0}, {1, 2}});
    const std::optional<Snapshot> snapshot = BuildSnapshot(two, *fighter_elf, 100);
    CHECK(snapshot && *snapshot == Of({0, 2}) && snapshot->size == 2);

    // The same party in any order, from any machine, is one snapshot; repeated classes stay.
    CHECK(BuildSnapshot({0x20, 0x10}, *Party({{0, 2}, {1, 0}}), 100) == snapshot);
    const std::optional<Snapshot> twins = BuildSnapshot({0x10, 0x20, 0x30}, *Party({{0, 2}, {1, 0}, {2, 2}}), 100);
    CHECK(twins && *twins == Of({0, 2, 2}) && twins->size == 3);
    CHECK(!(*twins == Of({0, 2})));
    const std::optional<Snapshot> four = BuildSnapshot({0x10, 0x20, 0x30, 0x40}, *Party({{0, 5}, {1, 4}, {2, 3}, {3, 5}}), 100);
    CHECK(four && four->size == 4 && *four == Of({3, 4, 5, 5}));

    // Sparse seats are fine: the slot comes from the member id.
    CHECK(BuildSnapshot({0x10, 0x30}, *Party({{0, 1}, {2, 4}}), 100) == Of({1, 4}));

    // Computer-controlled allies have units in slots that are not members and are not part of the party.
    CHECK(BuildSnapshot(two, *Party({{0, 0}, {1, 2}, {2, 3}, {3, 1}}), 100) == snapshot);

    // Not a party: one member, none, five, a repeated or unknown member id, a member without a live unit.
    CHECK(!BuildSnapshot({0x10}, *Party({{0, 0}}), 100));
    CHECK(!BuildSnapshot({}, *Party({}), 100));
    CHECK(!BuildSnapshot({0x10, 0x20, 0x30, 0x40, 0x50}, *Party({{0, 0}, {1, 0}, {2, 0}, {3, 0}}), 100));
    CHECK(!BuildSnapshot({0x10, 0x10}, *Party({{0, 0}}), 100));
    CHECK(!BuildSnapshot({0x10, 0x50}, *Party({{0, 0}, {1, 0}}), 100));
    CHECK(!BuildSnapshot({0x10, 0x00}, *Party({{0, 0}, {1, 0}}), 100));
    CHECK(!BuildSnapshot(two, *Party({{0, 0}}), 100));
    CHECK(!BuildSnapshot(two, *Party({{1, 2}}), 100));
    CHECK(!BuildSnapshot(two, *fighter_elf, 100 + kFreshMs + 1));
    // A unit in the wrong slot for a sparse room does not stand in for the member.
    CHECK(!BuildSnapshot({0x10, 0x30}, *Party({{0, 1}, {1, 4}}), 100));
}

void TestReporter() {
    Reporter reporter;
    const Snapshot a = Of({0, 2}), b = Of({0, 2, 5});
    int64_t now = 1000;

    CHECK(reporter.Due(now) && !reporter.Due(now + kPollMs - 1) && reporter.Due(now + kPollMs));

    // Nothing before the party has been the same for kStableMs, and no snapshot ever starts the clock.
    CHECK(!reporter.Poll(7, a, now));
    CHECK(!reporter.Poll(7, a, now + kStableMs - 1));
    CHECK(!reporter.Poll(7, std::nullopt, now + kStableMs));
    CHECK(!reporter.Poll(7, a, now + kStableMs + 1));
    now += kStableMs + 1;
    CHECK(!reporter.Poll(7, a, now + kStableMs - 1));
    const std::optional<Snapshot> first = reporter.Poll(7, a, now + kStableMs);
    CHECK(first && *first == a);
    reporter.Sent(7, a, now + kStableMs);
    now += kStableMs;

    // The same party is not sent again, however long it plays, nor after the link or the room's report came back.
    for (int i = 1; i <= 100; ++i) CHECK(!reporter.Poll(7, a, now + i * kPollMs));
    now += 100 * kPollMs;
    reporter.Idle();
    CHECK(!reporter.Poll(7, a, now + 1));
    CHECK(!reporter.Poll(7, a, now + 1 + kStableMs));

    // A changed party is sent once it is stable and the minimum gap has passed.
    now += kMinGapMs;
    CHECK(!reporter.Poll(7, b, now));
    const std::optional<Snapshot> second = reporter.Poll(7, b, now + kStableMs);
    CHECK(second && *second == b);
    reporter.Sent(7, b, now + kStableMs);
    now += kStableMs;

    // A party that changes back and forth is not reported faster than the gap.
    CHECK(!reporter.Poll(7, a, now + 1));
    CHECK(!reporter.Poll(7, a, now + 1 + kStableMs));
    CHECK(!reporter.Poll(7, a, now + kMinGapMs - 1));
    CHECK(reporter.Poll(7, a, now + kMinGapMs));
    reporter.Sent(7, a, now + kMinGapMs);
    now += kMinGapMs;

    // The same party in another room is another report, also after the gap.
    CHECK(!reporter.Poll(8, a, now + 1));
    CHECK(!reporter.Poll(8, a, now + 1 + kStableMs));
    CHECK(reporter.Poll(8, a, now + kMinGapMs));
}

void TestGates() {
    CHECK(CanReport(true, true, 4096, 0x10, 0x10));
    CHECK(!CanReport(false, true, 4096, 0x10, 0x10)); // Online Play off, or offline
    CHECK(!CanReport(true, false, 4096, 0x10, 0x10)); // no lobby link, or not signed in
    CHECK(!CanReport(true, true, 0, 0x10, 0x10));     // no room
    CHECK(!CanReport(true, true, 4096, 0, 0));        // no seat
    CHECK(!CanReport(true, true, 4096, 0x20, 0x10));  // a guest
}

std::vector<uint8_t> SyntheticImage(uint64_t size, uint32_t site, size_t stub_bytes, uint64_t& stubs) {
    stubs = (size + 0xfff) & ~uint64_t{0xfff};
    std::vector<uint8_t> image(stubs + stub_bytes, 0x90);
    const uint8_t prologue[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    std::memcpy(&image[site], prologue, sizeof(prologue));
    return image;
}

DcpPartySites Sites(uint32_t tick) {
    return {tick, cheats::kUnitOwnerOffset, cheats::kUnitPlayerSlotOffset, cheats::kUnitClassOffset,
            cheats::kOwnerCategoryOffset};
}

void TestHookInstall() {
    const uint64_t size = 0x10000, entry = 0x1230;
    const uint32_t site = 0x4400;
    const size_t stub_bytes = party_hook_stub_bytes();
    CHECK(stub_bytes == guest_hook_stub_size());
    uint64_t stubs = 0;
    std::vector<uint8_t> image = SyntheticImage(size, site, stub_bytes, stubs);
    const std::vector<uint8_t> before = image;

    // The game's displacements must be the handler's, the bytes the entry of the per-tick update, the stub in reach.
    DcpPartySites moved = Sites(site);
    moved.unit_class += 1;
    CHECK(!party_hook_install_located(image.data(), size, entry, &moved, image.data() + stubs, stub_bytes));
    moved = Sites(site);
    moved.owner_category += 4;
    CHECK(!party_hook_install_located(image.data(), size, entry, &moved, image.data() + stubs, stub_bytes));
    CHECK(!party_hook_install_located(image.data(), size, entry, nullptr, image.data() + stubs, stub_bytes));
    const DcpPartySites elsewhere = Sites(site + 0x100);
    CHECK(!party_hook_install_located(image.data(), size, entry, &elsewhere, image.data() + stubs, stub_bytes));
    const DcpPartySites outside = Sites(static_cast<uint32_t>(size));
    CHECK(!party_hook_install_located(image.data(), size, entry, &outside, image.data() + stubs, stub_bytes));
    CHECK(!party_hook_install_located(image.data(), size, entry, &elsewhere, image.data() + stubs, 0));
    const DcpPartySites good = Sites(site);
    CHECK(!party_hook_install_located(image.data(), size, entry, &good, image.data() + stubs, stub_bytes - 1));
    CHECK(image == before);

    CHECK(party_hook_install_located(image.data(), size, entry, &good, image.data() + stubs, stub_bytes));
    CHECK(image[site] == 0xe9);
    // Only the six bytes of the one site were touched.
    size_t outside_site = 0;
    for (uint64_t i = 0; i < size; ++i) outside_site += (i < site || i >= site + 6) && image[i] != before[i];
    CHECK(outside_site == 0);
}

void TestSources() {
    const std::string hook = Source("src/party_hook.cpp");
    const std::string header = Source("src/dcp_party.h");
    const std::string matching = Source("src/runtime_matching2.cpp");
    const std::string loader = Source("src/loader.c");
    const std::string cheats_source = Source("src/cheat_hooks.cpp");
    const std::string cmake = Source("CMakeLists.txt");
    CHECK(!hook.empty() && !header.empty() && !matching.empty() && !loader.empty() && !cheats_source.empty());

    // Independent of the Cheats: no cheat state, no cheat setting, and it links without the cheat runtime.
    for (const std::string& text : {hook, header}) {
        CHECK(text.find("runtime_cheat") == std::string::npos);
        CHECK(text.find("Enabled(") == std::string::npos);
        CHECK(text.find("Store<") == std::string::npos && text.find("memcpy(reinterpret_cast<void*>") == std::string::npos);
    }
    CHECK(hook.find("#include \"runtime_cheats.h\"") == std::string::npos);
    CHECK(cmake.find("add_executable(dcp-party-tests EXCLUDE_FROM_ALL tests/party_tests.cpp src/guest_hook.c src/party_hook.cpp)") !=
          std::string::npos);

    // The hook only observes: its handler is one Observe call and nothing writes through the registers.
    const std::string handler = Between(hook, "void GUEST_HOOK_ABI PartyTickHook", "// The entry");
    CHECK(!handler.empty() && Count(handler, "Observe(") == 1 && handler.find("regs->skip") == std::string::npos &&
          handler.find("regs->r") == handler.rfind("regs->r") && handler.find("= ") == std::string::npos);

    // It goes in only while the game is online, never beside the cheat hooks, and the cheat install is as it was.
    const std::string install = Between(loader, "if (known_build && runtime_online_active() && !runtime_cheats_hooks_wanted(1))",
                                        "const unsigned skip_applied");
    CHECK(!install.empty() && install.find("party_hook_install_located") != std::string::npos);
    CHECK(Count(loader, "party_hook_install_located") == 1);
    CHECK(loader.find("if (runtime_cheats_hooks_wanted(known_build && game_build.cheats)) {") != std::string::npos);
    CHECK(loader.find("party_located=dcp_resolve_party(&game_build,") != std::string::npos);
    CHECK(loader.find("if (known_build && runtime_online_active()) {\n            party_located") != std::string::npos);
    for (const char* file : {"src/cheat_hooks.cpp", "src/runtime_cheats.cpp", "src/runtime_cheats.h", "src/dcp_cheats.h"}) {
        const std::string text = Source(file);
        CHECK(!text.empty() && text.find("dcp_party.h") == std::string::npos && text.find("party_hook") == std::string::npos);
    }

    // The report: one request on the game's lobby link, class numbers only, no host, behind the gates.
    const std::string report = Between(matching, "void ReportParty(uint64_t now) {", "// Our endpoint goes to the lobby");
    CHECK(!report.empty());
    CHECK(Count(matching, "\"party.report\"") == 1 && Count(report, "g.link->request(\"party.report\"") == 1);
    CHECK(Count(report, ".set(") == 1 && report.find("body.set(\"classes\"") != std::string::npos);
    CHECK(report.find("\"host\"") == std::string::npos && report.find("account") == std::string::npos &&
          report.find("name") == std::string::npos && report.find("g.key") == std::string::npos &&
          report.find("g.room,") != std::string::npos);
    CHECK(report.find("party::CanReport(settings().enabled, LinkUsable() && g.up && !g.lost, g.room, g.me, g.owner)") !=
          std::string::npos);
    CHECK(report.find("Kind::kInternal") != std::string::npos && report.find("Complete(") == std::string::npos &&
          report.find("g.notice") == std::string::npos && report.find("exit") == std::string::npos &&
          report.find("abort") == std::string::npos);
    CHECK(Count(matching, "ReportParty(now);") == 1);
    CHECK(Count(Source("src/net/lobby_link.cpp"), "party") == 0 && Count(Source("src/net/websocket.cpp"), "party") == 0);

    // Only the game's own lobby link carries it: not the Connection test, the launcher's link or the in-game Options page.
    for (const char* file : {"src/net/connection_test.cpp", "launcher/online_client.cpp", "launcher/online_page.cpp",
                             "src/runtime_online.cpp", "src/runtime_rudp.cpp"}) {
        CHECK(Source(file).find("party.report") == std::string::npos);
    }
    // No telemetry-style words reach the client's network sources.
    CHECK(Source("src/dcp_party.h").find("telemetry") == std::string::npos && matching.find("telemetry") == std::string::npos);
}

} // namespace

int main() {
    TestClassesAndSlots();
    TestObserver();
    TestSnapshots();
    TestReporter();
    TestGates();
    TestHookInstall();
    TestSources();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
