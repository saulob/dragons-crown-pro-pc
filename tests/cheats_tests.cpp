// Cheat tests: the shared cheat list, the session layer, the in-game menu model, the rules the game hooks apply and
// the native hook layer. Logic and synthetic code only: no game files, no window.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <immintrin.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <vector>
#include <sstream>
#include <string>

#include "imgui.h"

#include "cheat_hooks.h"
#include "dcp_cheat_menu.h"
#include "dcp_cheats.h"
#include "guest_hook.h"
#include "runtime_cheats.h"

namespace fs = std::filesystem;
using namespace dcp::cheats;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

std::string ReadSource(const char* relative) {
    std::ifstream in(fs::path(DCP_SOURCE_DIR) / relative, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void TestCheatList() {
    CHECK(kCheatCount == 8);
    CHECK(std::size(kCheats) == kCheatCount);
    CHECK(!kCheatDefault);
    struct Expected {
        Cheat cheat;
        const char* label;
        const char* description;
    };
    const Expected expected[] = {
        {Cheat::InfiniteHealth, "Infinite Health", "Prevent normal damage from reducing player health"},
        {Cheat::InfiniteMana, "Infinite Mana", "Prevent mana from decreasing when casting spells"},
        {Cheat::InfiniteLives, "Infinite Lives", "Prevent remaining lives from decreasing"},
        {Cheat::InfiniteDurability, "Infinite Durability", "Prevent equipped gear from losing durability"},
        {Cheat::InfiniteArrows, "Infinite Arrows", "Prevent the Elf's arrows from decreasing"},
        {Cheat::InfiniteSkillPoints, "Infinite Skill Points", "Prevent skill points from decreasing when spent"},
        {Cheat::InfiniteGold, "Infinite Gold", "Prevent gold from decreasing when spent"},
        {Cheat::MovementSpeed, "Movement Speed 2x", "Move at twice the normal speed"},
    };
    for (size_t i = 0; i < kCheatCount; ++i) { // the final order, in the launcher page and in the F10 menu
        CHECK(kCheats[i].cheat == expected[i].cheat);
        CHECK(std::string(kCheats[i].label) == expected[i].label);
        CHECK(std::string(kCheats[i].description) == expected[i].description);
    }
    std::set<std::string> keys, labels;
    for (size_t i = 0; i < kCheatCount; ++i) {
        CHECK(Index(kCheats[i].cheat) == i); // the list is in enum order: rows, bits and states index by it
        CHECK(std::string(kCheats[i].key).rfind("cheat_", 0) == 0);
        keys.insert(kCheats[i].key);
        labels.insert(kCheats[i].label);
    }
    CHECK(keys.size() == kCheatCount && labels.size() == kCheatCount);
    CHECK(std::string(kLauncherPageSubtitle) == "Optional gameplay modifiers");
    CHECK(std::string(kLauncherHelp) ==
          "Press F10 while playing to open the Cheats menu. Changes made there apply only to the current session. Meant "
          "for offline and local play.");
    CHECK(Bit(Cheat::InfiniteHealth) == 1 && Bit(Cheat::InfiniteMana) == 2 && Bit(Cheat::MovementSpeed) == 128);
    CHECK(std::string(kMovementSpeedKey) == "cheat_movement_speed_2x");
}

void TestSessionOverride() {
    SessionCheats session;
    for (const bool saved : {false, true}) {
        for (const CheatInfo& cheat : kCheats) {
            CHECK(session.Effective(cheat.cheat, saved) == saved); // follows the launcher until the menu changes it
        }
    }
    // The launcher's value starts each session; a menu change is an override of it, and toggling back drops it.
    session.Toggle(Cheat::InfiniteHealth, false);
    CHECK(session.Effective(Cheat::InfiniteHealth, false));
    CHECK(!session.Effective(Cheat::InfiniteMana, false));
    session.Toggle(Cheat::InfiniteHealth, false);
    CHECK(!session.Effective(Cheat::InfiniteHealth, false));
    CHECK(session.Effective(Cheat::InfiniteHealth, true)); // back to following the saved value

    session.Toggle(Cheat::InfiniteMana, true);
    CHECK(!session.Effective(Cheat::InfiniteMana, true)); // turned off for the session although saved On
    session.Set(Cheat::InfiniteMana, true, true);
    CHECK(session.Effective(Cheat::InfiniteMana, true));

    // The runtime layer: the saved mask is the start, the menu's toggle never changes it, a new start resets it.
    runtime_cheats_start(Bit(Cheat::InfiniteHealth), 1);
    CHECK(runtime_cheat_enabled(int(Cheat::InfiniteHealth)) == 1);
    CHECK(runtime_cheat_enabled(int(Cheat::InfiniteMana)) == 0);
    runtime_cheat_toggle(int(Cheat::InfiniteHealth));
    runtime_cheat_toggle(int(Cheat::InfiniteMana));
    CHECK(runtime_cheat_enabled(int(Cheat::InfiniteHealth)) == 0);
    CHECK(runtime_cheat_enabled(int(Cheat::InfiniteMana)) == 1);
    CHECK(runtime_cheat_enabled(-1) == 0 && runtime_cheat_enabled(int(kCheatCount)) == 0);
    runtime_cheat_toggle(99); // ignored
    runtime_cheats_start(Bit(Cheat::InfiniteHealth), 1); // the next game start: the saved values win again
    CHECK(runtime_cheat_enabled(int(Cheat::InfiniteHealth)) == 1);
    CHECK(runtime_cheat_enabled(int(Cheat::InfiniteMana)) == 0);
    const std::string overlay = ReadSource("gpu/shim/dcp_overlay.cpp");
    CHECK(overlay.find("runtime_cheat_toggle") != std::string::npos);
    CHECK(ReadSource("gpu/shim/dcp_settings.cpp").find("cheat") == std::string::npos); // the menu cannot save them
}

void TestRefillTransitions() {
    const int health = int(Cheat::InfiniteHealth), mana = int(Cheat::InfiniteMana);

    // Off -> On from the menu asks for one refill; nothing else does.
    runtime_cheats_start(0, 1);
    CHECK(runtime_cheat_take_refill(health) == 0 && runtime_cheat_take_refill(mana) == 0);
    runtime_cheat_toggle(health);
    CHECK(runtime_cheat_take_refill(mana) == 0);
    CHECK(runtime_cheat_take_refill(health) == 1);
    CHECK(runtime_cheat_take_refill(health) == 0); // taken: not again, and never every frame
    runtime_cheat_toggle(health);                  // On -> Off: no refill, nothing pending
    CHECK(runtime_cheat_take_refill(health) == 0);
    runtime_cheat_toggle(health);
    runtime_cheat_toggle(health); // turned Off again before the game thread took the request: cancelled
    CHECK(runtime_cheat_take_refill(health) == 0);

    // The launcher's saved On asks once at the start, and a new start asks again.
    runtime_cheats_start(Bit(Cheat::InfiniteMana), 1);
    CHECK(runtime_cheat_take_refill(health) == 0);
    CHECK(runtime_cheat_take_refill(mana) == 1);
    CHECK(runtime_cheat_take_refill(mana) == 0);
    runtime_cheat_toggle(mana); // saved On, the menu turns it Off: nothing
    CHECK(runtime_cheat_take_refill(mana) == 0);
    runtime_cheat_toggle(mana); // and On again: a refill
    CHECK(runtime_cheat_take_refill(mana) == 1);
    runtime_cheats_start(Bit(Cheat::InfiniteMana), 1);
    CHECK(runtime_cheat_take_refill(mana) == 1);

    // Only Health, Mana and Durability fill anything; the other cheats never queue a request.
    runtime_cheats_start(0xff, 1);
    for (const CheatInfo& info : kCheats) {
        CHECK(runtime_cheat_take_refill(int(info.cheat)) == (RefillsOnEnable(info.cheat) ? 1 : 0));
    }
    runtime_cheat_toggle(int(Cheat::InfiniteGold));
    runtime_cheat_toggle(int(Cheat::InfiniteGold));
    CHECK(runtime_cheat_take_refill(int(Cheat::InfiniteGold)) == 0);
    CHECK(runtime_cheat_take_refill(-1) == 0 && runtime_cheat_take_refill(int(kCheatCount)) == 0);

    // The session layer alone: toggling reports the new state.
    SessionCheats session;
    CHECK(session.Toggle(Cheat::InfiniteHealth, false));
    CHECK(!session.Toggle(Cheat::InfiniteHealth, false));
    runtime_cheats_start(0, 1);
}

void TestMenuModel() {
    CHECK(kMenuRows.size() == 10);
    CHECK(kMenuRows.size() == kCheatCount + kActionCount + 1);
    CHECK(std::string(kMenuTitle) == "CHEATS");
    CHECK(std::string(kMenuCloseHint) == "F10 Close");
    CHECK(std::string(kMenuNote) == "Not saved to the launcher");
    const char* const order[] = {"Infinite Health",      "Infinite Mana",  "Infinite Lives",
                                 "Infinite Durability",  "Infinite Arrows", "Infinite Skill Points",
                                 "Infinite Gold",        "Add 1,000 Gold",  "Movement Speed 2x",
                                 "Experience Multiplier"};
    for (size_t i = 0; i < kMenuRows.size(); ++i) {
        CHECK(std::string(kMenuRows[i].label) == order[i]);
    }
    size_t next_cheat = 0;
    for (const MenuRow& row : kMenuRows) {
        if (row.kind == RowKind::Toggle) {
            CHECK(row.cheat == kCheats[next_cheat].cheat);
            CHECK(std::string(row.label) == kCheats[next_cheat].label); // one list: the menu repeats no label
            CHECK(row.verb == nullptr);
            ++next_cheat;
        }
    }
    CHECK(next_cheat == kCheatCount);
    CHECK(kMenuRows[6].kind == RowKind::Toggle && kMenuRows[6].cheat == Cheat::InfiniteGold);
    CHECK(kMenuRows[7].kind == RowKind::Action && kMenuRows[7].action == Action::AddGold);
    CHECK(std::string(kMenuRows[7].verb) == "Add");
    CHECK(kMenuRows[8].kind == RowKind::Toggle && kMenuRows[8].cheat == Cheat::MovementSpeed);
    CHECK(kMenuRows[9].kind == RowKind::Experience && std::string(kMenuRows[9].label) == "Experience Multiplier");
    CHECK(std::string(kActions[0].label) == "Add 1,000 Gold" && kGoldPerAdd == 1000);
    for (const CheatInfo& info : kCheats) { // an action is no cheat: nothing saved, nothing in the launcher's list
        CHECK(std::string(info.key).find("add_gold") == std::string::npos);
        CHECK(std::string(info.label) != "Add 1,000 Gold");
    }

    // The menu opens only with F10 and closes with F10, Escape or B; the game's input waits for the release.
    CHECK(!InputReleased(true, 0.2, kReleaseTimeoutSeconds));
    CHECK(InputReleased(false, 0.0, kReleaseTimeoutSeconds));
    CHECK(InputReleased(true, 1.0, kReleaseTimeoutSeconds)); // a button never reported released times out
    CloseGate gate;
    CHECK(!gate.requested() && !gate.Step(false, 0.0)); // not closing: the menu stays
    gate.Request();
    CHECK(gate.requested());
    CHECK(!gate.Step(true, 5.0));  // the closing press is still held
    CHECK(!gate.Step(true, 5.5));
    CHECK(gate.Step(false, 5.6));  // released
    CloseGate stuck;
    stuck.Request();
    CHECK(!stuck.Step(true, 10.0));
    CHECK(stuck.Step(true, 11.0)); // never released: given up after a second

    // The panel follows the display's content scale a step down (0.75x), never under 1.25x, and takes at most 45% of
    // the screen's width.
    CHECK(MenuScale(1.0f, 1920, 18) == kMenuMinScale);
    CHECK(MenuScale(0.5f, 1920, 18) == kMenuMinScale);
    CHECK(MenuScale(1.5f, 1920, 18) == kMenuMinScale);
    CHECK(MenuScale(2.0f, 1920, 18) == 1.5f);
    CHECK(MenuScale(2.25f, 1280, 13.5f) == 2.25f * kMenuScaleStep);
    CHECK(MenuScale(3.0f, 3840, 18) == 2.25f);
    CHECK(MenuScale(5.0f, 3840, 18) == kMenuMaxScale * kMenuScaleStep);
    CHECK(MenuScale(2.25f, 800, 18) < 2.25f * kMenuScaleStep && MenuScale(2.25f, 800, 18) > 1.0f);
    CHECK(MenuScale(2.0f, 200, 18) == 1.0f);
    for (const float display : {2.0f, 2.5f, 3.0f}) { // smaller than the first oversized menu, larger than the tiny one
        CHECK(MenuScale(display, 3840, 18) < display && MenuScale(display, 3840, 18) > 1.0f);
    }
    for (const float display : {1.0f, 1.5f, 2.25f, 3.0f}) {
        for (const float width : {1280.0f, 1920.0f, 2560.0f, 3840.0f}) {
            const float em = 18.0f * width / 1920.0f;
            CHECK(kMenuWidthEm * em * MenuScale(display, width, em) <= kMenuMaxWidthShare * width + 0.01f);
        }
    }

    const std::string overlay = ReadSource("gpu/shim/dcp_overlay.cpp");
    CHECK(overlay.find("MenuScale(") != std::string::npos);
    CHECK(overlay.find("FpsCounterBox(OverlayText()") != std::string::npos); // below the frame counter, not over it
    CHECK(ReadSource("gpu/shim/window.cpp").find("SetDisplayScale(SDL_GetWindowDisplayScale") != std::string::npos);
    CHECK(overlay.find("SDLK_F10") != std::string::npos);
    CHECK(overlay.find("ImGuiKey_GamepadFaceRight") != std::string::npos); // B closes
    CHECK(overlay.find("ImGuiKey_GamepadLStickUp") != std::string::npos);  // the left stick moves
    CHECK(overlay.find("kAccentRed") != std::string::npos); // the launcher's accent, not a colour of its own
}

// A unit, its owner and a damage-function frame in real memory, which the hook handlers read as the game's.
struct FakeGame {
    alignas(16) uint8_t unit[0x1000] = {};
    alignas(16) uint8_t owner[0x4000] = {};
    alignas(16) uint8_t frame[0x400] = {};

    FakeGame(bool human, int32_t slot) {
        Store<uint64_t>(unit + kUnitOwnerOffset, reinterpret_cast<uint64_t>(owner));
        Store<uint32_t>(owner + kOwnerCategoryOffset, human ? kHumanPlayerCategory : 0);
        Store<int32_t>(unit + kUnitPlayerSlotOffset, slot);
        Store<uint64_t>(frame + 0x100, reinterpret_cast<uint64_t>(unit)); // [rbp-0x200] with rbp = frame + 0x300
        Store<uint64_t>(owner + kOwnerUnitOffset, reinterpret_cast<uint64_t>(unit));
        Store<uint32_t>(owner + kOwnerClassOffset, kElfClass);
    }
    void SetClass(uint32_t class_index) { Store<uint32_t>(owner + kOwnerClassOffset, class_index); }
    template <typename T>
    static void Store(uint8_t* at, T value) {
        std::memcpy(at, &value, sizeof(value));
    }
    uint64_t Unit() const { return reinterpret_cast<uint64_t>(unit); }
    uint64_t Owner() const { return reinterpret_cast<uint64_t>(owner); }

    GuestRegs DamageRegs(int32_t amount) const {
        GuestRegs regs{};
        regs.rbp = reinterpret_cast<uint64_t>(frame) + 0x300;
        regs.r15 = Owner();
        regs.rcx = static_cast<uint32_t>(amount);
        return regs;
    }
};

struct TestMemory {
    uint64_t U64(uint64_t a) const { uint64_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 8); return v; }
    uint32_t U32(uint64_t a) const { uint32_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 4); return v; }
    int32_t S32(uint64_t a) const { int32_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 4); return v; }
};

void SetCheats(bool health, bool mana) {
    runtime_cheats_start((health ? Bit(Cheat::InfiniteHealth) : 0) | (mana ? Bit(Cheat::InfiniteMana) : 0), 1);
}

void TestRules() {
    // Which unit is Player 1: a human player's, in slot 0. Players 2 to 4 and everything else are not.
    FakeGame player1(true, 0), player2(true, 1), player4(true, 3), enemy(false, 0), npc(false, 4);
    CHECK(ClassifyUnit(TestMemory{}, player1.Unit()) == UnitKind::PlayerOne);
    CHECK(ClassifyUnit(TestMemory{}, player2.Unit()) == UnitKind::OtherPlayer);
    CHECK(ClassifyUnit(TestMemory{}, player4.Unit()) == UnitKind::OtherPlayer);
    CHECK(ClassifyUnit(TestMemory{}, enemy.Unit()) == UnitKind::Other); // slot 0 by default, but not a player
    CHECK(ClassifyUnit(TestMemory{}, npc.Unit()) == UnitKind::Other);
    CHECK(ClassifyUnit(TestMemory{}, 0) == UnitKind::Other);
    FakeGame orphan(true, 0);
    FakeGame::Store<uint64_t>(orphan.unit + kUnitOwnerOffset, 0);
    CHECK(ClassifyUnit(TestMemory{}, orphan.Unit()) == UnitKind::Other);

    // Infinite Health: a loss becomes zero for Player 1; healing, other units and a disabled cheat are unchanged.
    for (const int32_t amount : {1, 40, 99999}) {
        CHECK(ProtectedDamage(true, UnitKind::PlayerOne, amount) == 0);
        CHECK(ProtectedDamage(false, UnitKind::PlayerOne, amount) == amount);
        CHECK(ProtectedDamage(true, UnitKind::OtherPlayer, amount) == amount);
        CHECK(ProtectedDamage(true, UnitKind::Other, amount) == amount);
    }
    for (const int32_t amount : {0, -1, -250}) {
        CHECK(ProtectedDamage(true, UnitKind::PlayerOne, amount) == amount); // healing is untouched
    }

    // Infinite Mana: the consume flag becomes 0 for Player 1 only; recovery (positive deltas) always goes through.
    CHECK(ProtectedConsumeFlag(true, UnitKind::PlayerOne, 1) == 0);
    CHECK(ProtectedConsumeFlag(false, UnitKind::PlayerOne, 1) == 1);
    CHECK(ProtectedConsumeFlag(true, UnitKind::OtherPlayer, 1) == 1);
    CHECK(ProtectedConsumeFlag(true, UnitKind::Other, 1) == 1);
    CHECK(SuppressesManaDelta(true, UnitKind::PlayerOne, -1));
    CHECK(!SuppressesManaDelta(true, UnitKind::PlayerOne, 0));
    CHECK(!SuppressesManaDelta(true, UnitKind::PlayerOne, 7));
    CHECK(!SuppressesManaDelta(false, UnitKind::PlayerOne, -1));
    CHECK(!SuppressesManaDelta(true, UnitKind::OtherPlayer, -1));
    CHECK(!SuppressesManaDelta(true, UnitKind::Other, -1));
}

void TestHookHandlers() {
    FakeGame player1(true, 0), player2(true, 1), enemy(false, 0);

    // Infinite Health at the damage function.
    SetCheats(false, false);
    GuestRegs regs = player1.DamageRegs(50);
    HealthDamageHook(&regs);
    CHECK(regs.rcx == 50); // off: the game's own behavior
    SetCheats(true, false);
    regs = player1.DamageRegs(50);
    HealthDamageHook(&regs);
    CHECK(regs.rcx == 0);
    regs = player1.DamageRegs(-30);
    HealthDamageHook(&regs);
    CHECK(static_cast<int32_t>(regs.rcx) == -30); // healing
    regs = player2.DamageRegs(50);
    HealthDamageHook(&regs);
    CHECK(regs.rcx == 50); // Player 2
    regs = enemy.DamageRegs(50);
    HealthDamageHook(&regs);
    CHECK(regs.rcx == 50); // an enemy
    regs = player1.DamageRegs(50);
    regs.r15 = 0x1234; // not this unit's owner: the frame is not what the hook expects
    HealthDamageHook(&regs);
    CHECK(regs.rcx == 50);
    regs = player1.DamageRegs(50);
    FakeGame::Store<uint64_t>(player1.frame + 0x100, 0);
    HealthDamageHook(&regs);
    CHECK(regs.rcx == 50);
    FakeGame::Store<uint64_t>(player1.frame + 0x100, player1.Unit());

    // The menu's toggle takes effect at once and never touches the saved value.
    runtime_cheat_toggle(int(Cheat::InfiniteHealth));
    regs = player1.DamageRegs(50);
    HealthDamageHook(&regs);
    CHECK(regs.rcx == 50);
    runtime_cheat_toggle(int(Cheat::InfiniteHealth));
    regs = player1.DamageRegs(50);
    HealthDamageHook(&regs);
    CHECK(regs.rcx == 0);

    // Infinite Mana at UseMP and AddMP; the health cheat alone changes neither.
    SetCheats(true, false);
    GuestRegs cast{};
    cast.rdi = player1.Unit();
    cast.rcx = 1;
    ManaCostHook(&cast);
    CHECK(cast.rcx == 1);
    GuestRegs drain{};
    drain.rdi = player1.Unit();
    drain.rsi = static_cast<uint32_t>(-1);
    ManaDrainHook(&drain);
    CHECK(drain.skip == 0);

    SetCheats(false, true);
    cast.rcx = 1;
    ManaCostHook(&cast);
    CHECK(cast.rcx == 0); // the cast succeeds and costs nothing
    cast.rdi = player2.Unit();
    cast.rcx = 1;
    ManaCostHook(&cast);
    CHECK(cast.rcx == 1);
    cast.rdi = enemy.Unit();
    cast.rcx = 1;
    ManaCostHook(&cast);
    CHECK(cast.rcx == 1);
    cast.rdi = 0;
    ManaCostHook(&cast);
    CHECK(cast.rcx == 1);
    ManaDrainHook(&drain);
    CHECK(drain.skip == 1); // the sustained drain returns early
    drain.skip = 0;
    drain.rsi = 5;
    ManaDrainHook(&drain);
    CHECK(drain.skip == 0); // recovery
    drain.rdi = player2.Unit();
    drain.rsi = static_cast<uint32_t>(-1);
    ManaDrainHook(&drain);
    CHECK(drain.skip == 0);
    SetCheats(false, false);
}

// The hook table against an image made of nothing but the original bytes: checks, installation and refusals.
void TestArrowsAndSkillPoints() {
    // Rules.
    FakeGame elf1(true, 0), elf2(true, 1), enemy(false, 0);
    CHECK(ClassifyOwner(TestMemory{}, elf1.Owner()) == UnitKind::PlayerOne);
    CHECK(ClassifyOwner(TestMemory{}, elf2.Owner()) == UnitKind::OtherPlayer);
    CHECK(ClassifyOwner(TestMemory{}, enemy.Owner()) == UnitKind::Other);
    CHECK(ClassifyOwner(TestMemory{}, 0) == UnitKind::Other);
    FakeGame stale(true, 0);
    FakeGame::Store<uint64_t>(stale.unit + kUnitOwnerOffset, 0x1234); // the unit does not point back: not trusted
    CHECK(ClassifyOwner(TestMemory{}, stale.Owner()) == UnitKind::Other);
    CHECK(IsElf(TestMemory{}, elf1.Owner()));
    elf1.SetClass(4);
    CHECK(!IsElf(TestMemory{}, elf1.Owner()));
    elf1.SetClass(kElfClass);
    CHECK(ProtectedArrowAmount(true, UnitKind::PlayerOne, true, 1) == 0);
    CHECK(ProtectedArrowAmount(false, UnitKind::PlayerOne, true, 1) == 1);
    CHECK(ProtectedArrowAmount(true, UnitKind::PlayerOne, false, 1) == 1);
    CHECK(ProtectedArrowAmount(true, UnitKind::OtherPlayer, true, 1) == 1);
    CHECK(ProtectedArrowAmount(true, UnitKind::PlayerOne, true, -3) == -3); // anything but a shot is untouched
    CHECK(ProtectedSkillPoints(true, 0, 30, 20) == 30); // spending
    CHECK(ProtectedSkillPoints(true, 0, 30, 45) == 45); // earning
    CHECK(ProtectedSkillPoints(true, 0, 30, 30) == 30);
    CHECK(ProtectedSkillPoints(false, 0, 30, 20) == 20);
    CHECK(ProtectedSkillPoints(true, 1, 30, 20) == 20); // Player 2
    CHECK(ProtectedSkillPoints(true, 3, 30, 20) == 20);

    // Handlers.
    SetCheats(false, false);
    GuestRegs shot{};
    shot.rdi = elf1.Owner();
    shot.rsi = 1;
    ArrowHook(&shot);
    CHECK(shot.rsi == 1);
    runtime_cheats_start(Bit(Cheat::InfiniteArrows), 1);
    ArrowHook(&shot);
    CHECK(shot.rsi == 0 && shot.skip == 0); // the shot still goes out
    shot.rsi = 1;
    shot.rdi = elf2.Owner();
    ArrowHook(&shot);
    CHECK(shot.rsi == 1);
    shot.rdi = enemy.Owner();
    ArrowHook(&shot);
    CHECK(shot.rsi == 1);
    elf1.SetClass(5);
    shot.rdi = elf1.Owner();
    ArrowHook(&shot);
    CHECK(shot.rsi == 1); // not an Elf
    elf1.SetClass(kElfClass);

    uint8_t record[0x40] = {};
    int16_t before = 30;
    std::memcpy(record + 0x24, &before, 2);
    GuestRegs buy{};
    buy.r12 = 0;
    buy.r13 = 0xffffffffffff0000ull | 20; // 20 left after the purchase
    buy.r14 = reinterpret_cast<uint64_t>(record);
    runtime_cheats_start(0, 1);
    SkillPointsHook(&buy);
    CHECK(static_cast<uint16_t>(buy.r13) == 20);
    runtime_cheats_start(Bit(Cheat::InfiniteSkillPoints), 1);
    SkillPointsHook(&buy);
    CHECK(static_cast<uint16_t>(buy.r13) == 30 && (buy.r13 >> 16) == 0xffffffffffffull); // only the word changes
    buy.r13 = 45;
    SkillPointsHook(&buy);
    CHECK(static_cast<uint16_t>(buy.r13) == 45);
    buy.r13 = 20;
    buy.r12 = 1;
    SkillPointsHook(&buy);
    CHECK(static_cast<uint16_t>(buy.r13) == 20); // Player 2 pays
    buy.r12 = 0;
    buy.r14 = 0;
    SkillPointsHook(&buy); // no record: nothing happens, nothing is read
    CHECK(static_cast<uint16_t>(buy.r13) == 20);
    runtime_cheats_start(0, 1);
}

struct FakeRefill {
    int calls = 0;
    uint64_t unit = 0;
    uint32_t health = 0, mana = 0, only_below_max = 0;
    bool dead = false;
};
FakeRefill g_refill;

uint8_t GUEST_HOOK_ABI FakeIsDead(uint64_t) {
    return g_refill.dead ? 1 : 0;
}

void GUEST_HOOK_ABI FakeRefillFunction(uint64_t unit, uint32_t health, uint32_t mana, uint32_t only_below_max) {
    ++g_refill.calls;
    g_refill.unit = unit;
    g_refill.health = health;
    g_refill.mana = mana;
    g_refill.only_below_max = only_below_max;
}

GuestRegs TickRegs(const FakeGame& game) {
    GuestRegs regs{};
    regs.rdi = game.Unit();
    return regs;
}

const GuestHook* FindHook(const char* name) {
    for (const GuestHook& hook : Hooks()) {
        if (std::string(hook.name) == name) {
            return &hook;
        }
    }
    return nullptr;
}

void TestMovement() {
    CHECK(ScaledWalkMultiplier(true, UnitKind::PlayerOne, 1.0f) == 2.0f);
    CHECK(ScaledWalkMultiplier(true, UnitKind::PlayerOne, 1.5f) == 3.0f); // on top of a MOVE_UP buff
    CHECK(ScaledWalkMultiplier(false, UnitKind::PlayerOne, 1.0f) == 1.0f);
    CHECK(ScaledWalkMultiplier(true, UnitKind::OtherPlayer, 1.0f) == 1.0f);
    CHECK(ScaledWalkMultiplier(true, UnitKind::Other, 1.0f) == 1.0f);

    FakeGame player1(true, 0), player2(true, 1), enemy(false, 0);
    const auto walk = [](const FakeGame& game, float multiplier) {
        // The unit sits at [rbp-0x80] and the multiplier at [rbp-0x48] of the walk function's frame.
        alignas(16) static uint8_t frame[0x200];
        std::memset(frame, 0, sizeof(frame));
        const uint64_t unit = game.Unit();
        std::memcpy(frame + 0x1c0 - 0x80, &unit, 8);
        std::memcpy(frame + 0x1c0 - 0x48, &multiplier, 4);
        GuestRegs regs{};
        regs.rbp = reinterpret_cast<uint64_t>(frame) + 0x1c0;
        MovementHook(&regs);
        float after;
        std::memcpy(&after, frame + 0x1c0 - 0x48, 4);
        return after;
    };
    SetCheats(false, false);
    CHECK(walk(player1, 1.0f) == 1.0f); // Off: the game's own movement
    runtime_cheats_start(Bit(Cheat::MovementSpeed), 1);
    CHECK(walk(player1, 1.0f) == 2.0f);
    CHECK(walk(player1, 1.5f) == 3.0f);
    CHECK(walk(player2, 1.0f) == 1.0f); // Players 2 to 4
    CHECK(walk(enemy, 1.0f) == 1.0f);   // enemies, NPC allies and everything that is not a human player
    runtime_cheat_toggle(int(Cheat::MovementSpeed)); // Off from the menu: the normal speed is back at once
    CHECK(walk(player1, 1.0f) == 1.0f);
    runtime_cheats_start(0, 1);
}

void TestLives() {
    // The respawn computes lp - 1; Player 1 hands it lp + 1 while the cheat is On.
    CHECK(ProtectedLivesInput(true, UnitKind::PlayerOne, 2) == 3);
    CHECK(ProtectedLivesInput(true, UnitKind::PlayerOne, 0) == 1);
    CHECK(ProtectedLivesInput(true, UnitKind::PlayerOne, -1) == 0);
    CHECK(ProtectedLivesInput(false, UnitKind::PlayerOne, 2) == 2);
    CHECK(ProtectedLivesInput(true, UnitKind::OtherPlayer, 2) == 2);
    CHECK(ProtectedLivesInput(true, UnitKind::Other, 2) == 2);

    FakeGame player1(true, 0), player2(true, 1);
    // What the displaced instructions then compute: `dec eax` leaves the old count, and a loss is only blocked for P1.
    const auto respawn = [](const FakeGame& game, int32_t lives) {
        GuestRegs regs{};
        regs.r13 = game.Unit();
        regs.rax = static_cast<uint32_t>(lives);
        LivesHook(&regs);
        return static_cast<int32_t>(regs.rax) - 1; // the dec eax
    };
    SetCheats(false, false);
    CHECK(respawn(player1, 2) == 1 && respawn(player1, 0) == -1);
    runtime_cheats_start(Bit(Cheat::InfiniteLives), 1);
    CHECK(respawn(player1, 2) == 2);
    CHECK(respawn(player1, 0) == 0); // the last life is not lost either: no continue prompt, no game over
    CHECK(respawn(player1, -1) == -1);
    CHECK(respawn(player2, 2) == 1); // Player 2 loses a life
    runtime_cheat_toggle(int(Cheat::InfiniteLives));
    CHECK(respawn(player1, 2) == 1); // Off: lives decrease again at once
    runtime_cheats_start(0, 1);
}

void TestGold() {
    CHECK(ProtectedGold(true, 500, 350) == 500); // spending
    CHECK(ProtectedGold(true, 500, 500) == 500);
    CHECK(ProtectedGold(true, 500, 900) == 900); // earning
    CHECK(ProtectedGold(true, 500, 0) == 500);
    CHECK(ProtectedGold(false, 500, 350) == 350);

    // Every spending path: the store's base and value registers differ, the rule is the same.
    struct Site {
        const char* name;
        uint64_t GuestRegs::*base;
        uint64_t GuestRegs::*value;
    };
    const Site sites[] = {
        {"gold shop", &GuestRegs::r12, &GuestRegs::rcx},      {"gold continue", &GuestRegs::rax, &GuestRegs::rcx},
        {"gold stable", &GuestRegs::r12, &GuestRegs::rax},    {"gold beggar", &GuestRegs::rdx, &GuestRegs::rcx},
        {"gold base", &GuestRegs::r13, &GuestRegs::rcx},      {"gold inn", &GuestRegs::r13, &GuestRegs::rax},
        {"gold inn 2", &GuestRegs::r14, &GuestRegs::rcx},     {"gold list", &GuestRegs::r14, &GuestRegs::rdx},
        {"gold list 2", &GuestRegs::r14, &GuestRegs::rsi},    {"gold equip", &GuestRegs::rdx, &GuestRegs::rax},
    };
    CHECK(std::size(sites) == 10);
    alignas(16) static uint8_t save[kGoldOffset + 64];
    for (const Site& site : sites) {
        const GuestHook* hook = FindHook(site.name);
        CHECK(hook != nullptr);
        if (!hook) {
            continue;
        }
        const auto store = [&](int32_t before, int32_t after) {
            std::memcpy(save + kGoldOffset, &before, 4);
            GuestRegs regs{};
            regs.*site.base = reinterpret_cast<uint64_t>(save);
            regs.*site.value = 0xdead00000000ull | static_cast<uint32_t>(after);
            hook->handler(&regs);
            return static_cast<int32_t>(regs.*site.value);
        };
        SetCheats(false, false);
        CHECK(store(500, 350) == 350); // Off: the purchase goes through
        runtime_cheats_start(Bit(Cheat::InfiniteGold), 1);
        CHECK(store(500, 350) == 500);
        CHECK(store(500, 0) == 500);
        CHECK(store(500, 900) == 900);
        CHECK(store(500, 500) == 500);
        runtime_cheat_toggle(int(Cheat::InfiniteGold)); // the menu turns it Off: spending resumes at once
        CHECK(store(500, 350) == 350);
        runtime_cheats_start(0, 1);
    }
}

struct FakeGold {
    int32_t gold = 0;
    int calls = 0;
    int32_t last_amount = 0;
    uint64_t context = 1;
};
FakeGold g_gold;

uint64_t GUEST_HOOK_ABI FakeGoldContext(uint64_t) {
    return g_gold.context;
}

int32_t GUEST_HOOK_ABI FakeGetGold(uint64_t) {
    return g_gold.gold;
}

// The game's AddGold: it adds the amount and clamps to its own range.
void GUEST_HOOK_ABI FakeAddGold(uint64_t, int32_t amount) {
    ++g_gold.calls;
    g_gold.last_amount = amount;
    int64_t sum = int64_t(g_gold.gold) + amount;
    sum = sum > kGoldMaximum ? kGoldMaximum : (sum < 0 ? 0 : sum);
    g_gold.gold = int32_t(sum);
}

void TestAddGold() {
    CHECK(GoldIncrease(100) == 1000 && GoldIncrease(5000) == 1000 && GoldIncrease(0) == 1000);
    CHECK(GoldIncrease(kGoldMaximum - 1000) == 1000);
    CHECK(GoldIncrease(kGoldMaximum - 999) == 999); // near the maximum: only what fits
    CHECK(GoldIncrease(kGoldMaximum - 1) == 1);
    CHECK(GoldIncrease(kGoldMaximum) == 0);
    CHECK(GoldIncrease(kGoldMaximum + 1) == 0 && GoldIncrease(INT32_MAX) == 0); // never past the maximum, no overflow
    CHECK(GoldIncrease(-5) == 1000 && GoldIncrease(INT32_MIN) == 1000);

    ActionRequests requests;
    CHECK(!requests.Pending(Action::AddGold) && !requests.Take(Action::AddGold));
    requests.Request(Action::AddGold);
    CHECK(requests.Pending(Action::AddGold));
    CHECK(requests.Take(Action::AddGold) && !requests.Take(Action::AddGold)); // one request, one run
    requests.Request(Action::AddGold);
    requests.Request(Action::AddGold);
    CHECK(requests.Take(Action::AddGold) && requests.Take(Action::AddGold) && !requests.Take(Action::AddGold));
    requests.Request(Action::AddGold);
    requests.Clear();
    CHECK(!requests.Pending(Action::AddGold));

    SetGuestFunctions({FakeIsDead, FakeRefillFunction, FakeGoldContext, FakeGetGold, FakeAddGold});
    FakeGame player1(true, 0), player2(true, 1), enemy(false, 0);
    runtime_cheats_start(0, 1);
    const int add = int(Action::AddGold);
    const auto tick = [](const FakeGame& game) {
        GuestRegs regs = TickRegs(game);
        PlayerTickHook(&regs);
    };
    const auto press = [&](int32_t before) {
        g_gold = {};
        g_gold.gold = before;
        runtime_cheat_action(add);
        tick(player1);
        return g_gold.gold;
    };

    CHECK(press(100) == 1100);
    CHECK(press(5000) == 6000);
    CHECK(press(0) == 1000);
    CHECK(press(kGoldMaximum - 500) == kGoldMaximum); // near the maximum: clamped
    CHECK(press(kGoldMaximum) == kGoldMaximum && g_gold.calls == 0); // at the maximum: the game is not asked
    CHECK(press(INT32_MAX) == INT32_MAX && g_gold.calls == 0);
    CHECK(press(kGoldMaximum - 1) == kGoldMaximum);

    // Two presses, two additions: 100 -> 1100 -> 2100.
    g_gold = {};
    g_gold.gold = 100;
    runtime_cheat_action(add);
    tick(player1);
    CHECK(g_gold.gold == 1100);
    runtime_cheat_action(add);
    tick(player1);
    CHECK(g_gold.gold == 2100 && g_gold.calls == 2 && g_gold.last_amount == 1000);

    // One press is one addition however many ticks follow it (a held button files one request, see the menu input test).
    g_gold = {};
    g_gold.gold = 100;
    runtime_cheat_action(add);
    for (int i = 0; i < 120; ++i) {
        tick(player1);
    }
    CHECK(g_gold.gold == 1100 && g_gold.calls == 1);
    CHECK(runtime_cheat_action_pending(add) == 0);

    // Presses made before the game thread ticks are all carried out, one per tick.
    g_gold = {};
    runtime_cheat_action(add);
    runtime_cheat_action(add);
    runtime_cheat_action(add);
    tick(player1);
    CHECK(g_gold.gold == 1000);
    tick(player1);
    tick(player1);
    tick(player1);
    CHECK(g_gold.gold == 3000 && g_gold.calls == 3);

    // Only Player 1's unit runs it; the request waits for it.
    g_gold = {};
    runtime_cheat_action(add);
    tick(player2);
    tick(enemy);
    CHECK(g_gold.calls == 0 && runtime_cheat_action_pending(add) == 1);
    tick(player1);
    CHECK(g_gold.gold == 1000 && runtime_cheat_action_pending(add) == 0);

    // No game object to add to (a null context) or no game functions: nothing runs, the request waits.
    g_gold = {};
    g_gold.context = 0;
    runtime_cheat_action(add);
    tick(player1);
    CHECK(g_gold.calls == 0 && runtime_cheat_action_pending(add) == 1);
    g_gold.context = 1;
    tick(player1);
    CHECK(g_gold.gold == 1000);
    SetGuestFunctions({nullptr, nullptr});
    g_gold = {};
    runtime_cheat_action(add);
    tick(player1);
    CHECK(g_gold.calls == 0 && runtime_cheat_action_pending(add) == 1);
    SetGuestFunctions({FakeIsDead, FakeRefillFunction, FakeGoldContext, FakeGetGold, FakeAddGold});

    // Leaving the menu drops what has not run; starting the game does too; invalid actions are ignored.
    runtime_cheat_clear_actions();
    CHECK(runtime_cheat_action_pending(add) == 0);
    runtime_cheat_action(add);
    runtime_cheats_start(0, 1);
    CHECK(runtime_cheat_action_pending(add) == 0 && runtime_cheat_take_action(add) == 0);
    runtime_cheat_action(-1);
    runtime_cheat_action(int(kActionCount));
    CHECK(runtime_cheat_action_pending(-1) == 0 && runtime_cheat_action_pending(int(kActionCount)) == 0);

    // It is an action, not a cheat: it leaves every toggle and every refill as it was, whatever the saved value.
    runtime_cheats_start(Bit(Cheat::InfiniteHealth), 1);
    const unsigned refills = runtime_cheat_refills_pending();
    g_gold = {};
    runtime_cheat_action(add);
    tick(player1);
    CHECK(g_gold.gold == 1000);
    for (const CheatInfo& info : kCheats) {
        CHECK((runtime_cheat_enabled(int(info.cheat)) != 0) == (info.cheat == Cheat::InfiniteHealth));
    }
    CHECK(runtime_cheat_refills_pending() == 0 && refills == Bit(Cheat::InfiniteHealth)); // that refill ran on the tick

    // Infinite Gold stays what it was: spending is kept, the added gold is a gain.
    runtime_cheats_start(Bit(Cheat::InfiniteGold), 1);
    CHECK(ProtectedGold(true, 1000, 2000) == 2000 && ProtectedGold(true, 1000, 400) == 1000);
    CHECK(runtime_cheat_action_pending(add) == 0);
    runtime_cheat_action(add);
    tick(player1);
    CHECK(g_gold.gold == 2000);
    runtime_cheats_start(0, 1);
    SetGuestFunctions({nullptr, nullptr});

    const std::string overlay = ReadSource("gpu/shim/dcp_overlay.cpp");
    const size_t action_branch = overlay.find("runtime_cheat_action(int(row.action))");
    CHECK(action_branch != std::string::npos);
    CHECK(overlay.substr(action_branch, 160).find("cheat_close") == std::string::npos); // the menu stays open
    CHECK(overlay.find("runtime_cheat_clear_actions()") != std::string::npos);
    const std::string launcher = ReadSource("launcher/settings.cpp") + ReadSource("launcher/launcher.cpp");
    CHECK(launcher.find("add_gold") == std::string::npos && launcher.find("kActions") == std::string::npos);
}

// The real ImGui navigation with the overlay's flags: one press of Enter, Space or A on a row is one activation,
// however long it stays held, and so is one click.
struct MenuInputRig {
    ImGuiContext* context = ImGui::CreateContext();

    MenuInputRig() {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~MenuInputRig() { ImGui::DestroyContext(context); }

    // One frame of three rows drawn as the menu does; returns how many were activated.
    int Frame() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
        int activated = 0;
        if (ImGui::Begin("##rig", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
            for (int i = 0; i < 3; ++i) {
                ImGui::PushID(i);
                activated += ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(0.0f, 24.0f)) ? 1 : 0;
                if (i == 0) {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::PopID();
            }
        }
        ImGui::End();
        ImGui::Render();
        return activated;
    }

    int Frames(int count) {
        int activated = 0;
        for (int i = 0; i < count; ++i) {
            activated += Frame();
        }
        return activated;
    }

    int Hold(ImGuiKey key, int count) {
        ImGui::GetIO().AddKeyEvent(key, true);
        const int activated = Frames(count);
        ImGui::GetIO().AddKeyEvent(key, false);
        return activated + Frames(3);
    }
};

void TestMenuInput() {
    for (const ImGuiKey key : {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown}) {
        MenuInputRig rig;
        rig.Frames(3);
        CHECK(rig.Hold(ImGuiKey_DownArrow, 2) == 0); // moving the selection activates nothing
        CHECK(rig.Hold(key, 120) == 1);              // two seconds held: one activation
        CHECK(rig.Hold(key, 2) == 1);                // a second press: another one
        CHECK(rig.Hold(key, 1) == 1);
        CHECK(rig.Frames(60) == 0);                  // nothing without a press
    }

    MenuInputRig rig;
    rig.Frames(3);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(40.0f, 20.0f);
    rig.Frames(2);
    io.AddMouseButtonEvent(0, true);
    int clicked = rig.Frames(90);
    io.AddMouseButtonEvent(0, false);
    clicked += rig.Frames(3);
    CHECK(clicked == 1); // a click held for a while is one activation
}

void TestDurability() {
    CHECK(ProtectedDurabilityDelta(true, UnitKind::PlayerOne, -200) == 0);
    CHECK(ProtectedDurabilityDelta(true, UnitKind::PlayerOne, -3000) == 0);
    CHECK(ProtectedDurabilityDelta(true, UnitKind::PlayerOne, 500) == 500); // repair
    CHECK(ProtectedDurabilityDelta(true, UnitKind::PlayerOne, 0) == 0);
    CHECK(ProtectedDurabilityDelta(false, UnitKind::PlayerOne, -200) == -200);
    CHECK(ProtectedDurabilityDelta(true, UnitKind::OtherPlayer, -200) == -200);
    CHECK(ProtectedDurabilityDelta(true, UnitKind::Other, -200) == -200);

    FakeGame player1(true, 0), player2(true, 1), enemy(false, 0);
    auto wear = [](const FakeGame& game, int32_t delta) {
        // The handler reads the unit at [rbp-0x308] and the delta at [rbp-0x2fc] of the buff function's frame.
        alignas(16) static uint8_t frame[0x400];
        std::memset(frame, 0, sizeof(frame));
        const uint64_t unit = game.Unit();
        std::memcpy(frame + 0x400 - 0x308 - 0x40, &unit, 8);
        std::memcpy(frame + 0x400 - 0x2fc - 0x40, &delta, 4);
        GuestRegs regs{};
        regs.rbp = reinterpret_cast<uint64_t>(frame) + 0x400 - 0x40;
        DurabilityHook(&regs);
        int32_t after;
        std::memcpy(&after, frame + 0x400 - 0x2fc - 0x40, 4);
        return after;
    };
    SetCheats(false, false);
    CHECK(wear(player1, -1000) == -1000);
    runtime_cheats_start(Bit(Cheat::InfiniteDurability), 1);
    CHECK(wear(player1, -1000) == 0);
    CHECK(wear(player1, 700) == 700);
    CHECK(wear(player2, -1000) == -1000);
    CHECK(wear(enemy, -1000) == -1000);
    runtime_cheat_toggle(int(Cheat::InfiniteDurability)); // the menu turns it Off: wear comes back at once
    CHECK(wear(player1, -1000) == -1000);
    runtime_cheats_start(0, 1);
}

// Worn gear in the fake unit's equipment list, as the game's own list functions would hand it out.
struct FakeEquipment {
    alignas(16) uint8_t bag[0x354] = {};
    uint32_t empty_mask = 0;
    uint64_t list = 0;
    int set_calls = 0;
    int32_t last_value = 0;
};
FakeEquipment g_equipment;

uint8_t GUEST_HOOK_ABI FakeEquipmentCount(uint64_t) {
    return g_equipment.bag[0x352];
}

uint64_t GUEST_HOOK_ABI FakeEquipmentItem(uint64_t list, int32_t index) {
    if (index < 0 || index >= g_equipment.bag[0x352] || (g_equipment.empty_mask & (1u << index)) != 0) {
        return 0;
    }
    return list + uint64_t(index) * 0x7c + 0x10;
}

uint8_t GUEST_HOOK_ABI FakeIsGear(uint64_t item) {
    return *reinterpret_cast<const uint8_t*>(item + 0x50);
}

void GUEST_HOOK_ABI FakeSetDurability(uint64_t list, int32_t index, int32_t value) {
    ++g_equipment.set_calls;
    g_equipment.last_value = value;
    FakeGame::Store<int32_t>(reinterpret_cast<uint8_t*>(list) + uint64_t(index) * 0x7c + 0x10 + kItemDurabilityOffset, value);
}

void UseFakeEquipment() {
    SetGuestFunctions({FakeIsDead, FakeRefillFunction, nullptr, nullptr, nullptr, FakeEquipmentCount, FakeEquipmentItem,
                       FakeIsGear, FakeSetDurability});
}

// Gives the unit an equipment list with `count` slots (owner and bag set, as once the player's gear exists).
void GiveEquipment(FakeGame& game, uint8_t count) {
    g_equipment.bag[0x352] = count;
    uint8_t* list = game.unit + kUnitEquipmentOffset;
    FakeGame::Store<uint64_t>(list + kEquipmentOwnerOffset, game.Owner());
    FakeGame::Store<uint64_t>(list + kEquipmentBagOffset, reinterpret_cast<uint64_t>(g_equipment.bag));
    g_equipment.list = reinterpret_cast<uint64_t>(list);
}

uint8_t* Slot(FakeGame& game, uint32_t slot) {
    return game.unit + kUnitEquipmentOffset + slot * 0x7c + 0x10;
}

void SetItem(FakeGame& game, uint32_t slot, int32_t current, int32_t maximum, bool equipped = true, bool gear = true) {
    uint8_t* item = Slot(game, slot);
    FakeGame::Store<uint32_t>(item + kItemFlagsOffset, equipped ? kItemEquippedFlag : 0);
    FakeGame::Store<int32_t>(item + kItemDurabilityOffset, current);
    FakeGame::Store<int32_t>(item + kItemMaxDurabilityOffset, maximum);
    item[0x50] = gear ? 1 : 0;
}

int32_t Durability(FakeGame& game, uint32_t slot) {
    int32_t value;
    std::memcpy(&value, Slot(game, slot) + kItemDurabilityOffset, sizeof(value));
    return value;
}

void TestDurabilityRefill() {
    CHECK(NeedsDurabilityRefill(kItemEquippedFlag, 835, 1000));
    CHECK(!NeedsDurabilityRefill(kItemEquippedFlag, 1000, 1000) && !NeedsDurabilityRefill(kItemEquippedFlag, 1200, 1200));
    CHECK(!NeedsDurabilityRefill(0, 835, 1000));                       // not worn
    CHECK(!NeedsDurabilityRefill(kItemEquippedFlag, 835, -1));         // no durability
    CHECK(!NeedsDurabilityRefill(kItemEquippedFlag, 835, 0));
    CHECK(!NeedsDurabilityRefill(kItemEquippedFlag, -5, 1000));        // not a durability value
    CHECK(NeedsDurabilityRefill(kItemEquippedFlag, 0, 100000));        // broken gear is restored too

    FakeGame player1(true, 0), player2(true, 1), enemy(false, 0);
    UseFakeEquipment();
    g_equipment = {};
    g_refill = {};
    const int durability = int(Cheat::InfiniteDurability);
    const auto tick = [](const FakeGame& game) {
        GuestRegs regs = TickRegs(game);
        PlayerTickHook(&regs);
    };

    // Off -> On: one piece, then several with their own maximums, an empty slot and invalid items.
    GiveEquipment(player1, 3);
    SetItem(player1, 0, 835, 1000);
    runtime_cheats_start(0, 1);
    tick(player1);
    CHECK(Durability(player1, 0) == 835 && g_equipment.set_calls == 0); // nothing asked, nothing touched
    runtime_cheat_toggle(durability);
    tick(player1);
    CHECK(Durability(player1, 0) == 1000 && g_equipment.set_calls == 1 && g_equipment.last_value == 1000);

    g_equipment = {};
    GiveEquipment(player1, 10);
    SetItem(player1, 0, 835, 1000);
    SetItem(player1, 1, 400, 500);
    SetItem(player1, 2, 1200, 1200);
    SetItem(player1, 3, 77, 100000); // a real stored value, 100 times the displayed one
    SetItem(player1, 4, 300, 900);   // empty slot below
    g_equipment.empty_mask = 1u << 4;
    SetItem(player1, 5, 300, 900, true, false);  // not gear
    SetItem(player1, 6, 300, 900, false, true);  // not worn
    SetItem(player1, 7, 300, -1);                // no durability
    SetItem(player1, 8, -4, 900);                // invalid value
    SetItem(player1, 9, 1000, 1000);
    runtime_cheats_start(0, 1);
    runtime_cheat_toggle(durability);
    tick(player1);
    CHECK(Durability(player1, 0) == 1000 && Durability(player1, 1) == 500 && Durability(player1, 2) == 1200);
    CHECK(Durability(player1, 3) == 100000);
    CHECK(Durability(player1, 4) == 300 && Durability(player1, 5) == 300 && Durability(player1, 6) == 300);
    CHECK(Durability(player1, 7) == 300 && Durability(player1, 8) == -4 && Durability(player1, 9) == 1000);
    CHECK(g_equipment.set_calls == 3); // slots 0, 1 and 3; slots 2 and 9 are already at their maximum
    CHECK(runtime_cheat_refills_pending() == 0);

    // Only once: damage after the refill stays, however many ticks follow; so does a repeated tick.
    FakeGame::Store<int32_t>(Slot(player1, 0) + kItemDurabilityOffset, 640);
    for (int i = 0; i < 60; ++i) {
        tick(player1);
    }
    CHECK(Durability(player1, 0) == 640 && g_equipment.set_calls == 3);

    // On -> Off changes nothing; Off -> On refills again.
    runtime_cheat_toggle(durability);
    tick(player1);
    CHECK(Durability(player1, 0) == 640 && g_equipment.set_calls == 3 && runtime_cheat_refills_pending() == 0);
    runtime_cheat_toggle(durability);
    tick(player1);
    CHECK(Durability(player1, 0) == 1000);

    // A piece worn later while the cheat stays On is not repaired; toggling Off and On restores it.
    FakeGame::Store<int32_t>(Slot(player1, 1) + kItemDurabilityOffset, 250);
    SetItem(player1, 4, 120, 800);
    g_equipment.empty_mask = 0;
    for (int i = 0; i < 10; ++i) {
        tick(player1);
    }
    CHECK(Durability(player1, 4) == 120 && Durability(player1, 1) == 250);
    runtime_cheat_toggle(durability);
    runtime_cheat_toggle(durability);
    tick(player1);
    CHECK(Durability(player1, 4) == 800 && Durability(player1, 1) == 500);

    // Players 2 to 4, NPCs and enemies are never refilled, and the request waits for Player 1.
    FakeGame::Store<int32_t>(Slot(player1, 0) + kItemDurabilityOffset, 600);
    GiveEquipment(player2, 2);
    SetItem(player2, 0, 100, 1000);
    runtime_cheats_start(0, 1);
    runtime_cheat_toggle(durability);
    const int calls = g_equipment.set_calls;
    tick(player2);
    tick(enemy);
    CHECK(Durability(player2, 0) == 100 && g_equipment.set_calls == calls);
    CHECK(runtime_cheat_refills_pending() == Bit(Cheat::InfiniteDurability));
    tick(player1);
    CHECK(Durability(player1, 0) == 1000 && Durability(player2, 0) == 100);

    // Enabled before Player 1's gear exists (the launcher's value, or the menu): it waits, then runs once.
    FakeGame late(true, 0);
    SetItem(late, 0, 300, 1000);
    runtime_cheats_start(Bit(Cheat::InfiniteDurability), 1);
    CHECK(runtime_cheat_refills_pending() == Bit(Cheat::InfiniteDurability));
    for (int i = 0; i < 30; ++i) {
        tick(late);
    }
    CHECK(Durability(late, 0) == 300 && runtime_cheat_refills_pending() == Bit(Cheat::InfiniteDurability));
    g_equipment = {};
    GiveEquipment(late, 1);
    tick(late);
    CHECK(Durability(late, 0) == 1000 && runtime_cheat_refills_pending() == 0);
    FakeGame::Store<int32_t>(Slot(late, 0) + kItemDurabilityOffset, 900);
    tick(late);
    CHECK(Durability(late, 0) == 900);

    // Enabled and disabled again before Player 1 exists: cancelled.
    FakeGame later(true, 0);
    GiveEquipment(later, 1);
    SetItem(later, 0, 300, 1000);
    runtime_cheats_start(0, 1);
    runtime_cheat_toggle(durability);
    runtime_cheat_toggle(durability);
    tick(later);
    CHECK(Durability(later, 0) == 300 && runtime_cheat_refills_pending() == 0);

    // Nothing is kept between ticks: a new Player 1 unit at other addresses is refilled by a new request.
    runtime_cheat_toggle(durability);
    tick(later);
    CHECK(Durability(later, 0) == 1000);
    FakeGame replacement(true, 0);
    GiveEquipment(replacement, 1);
    SetItem(replacement, 0, 10, 50);
    runtime_cheat_toggle(durability);
    runtime_cheat_toggle(durability);
    tick(replacement);
    CHECK(Durability(replacement, 0) == 50);

    // Without the game's item functions (or in a wrong build) nothing is called and the request waits.
    SetGuestFunctions({FakeIsDead, FakeRefillFunction});
    g_equipment.set_calls = 0;
    runtime_cheats_start(Bit(Cheat::InfiniteDurability), 1);
    SetItem(later, 0, 5, 50);
    tick(later);
    CHECK(g_equipment.set_calls == 0 && runtime_cheat_refills_pending() == Bit(Cheat::InfiniteDurability));
    runtime_cheats_start(0, 1);
    SetGuestFunctions({nullptr, nullptr});
}

void TestRefillOnEnable() {
    // The plan: what is filled, and what stays pending.
    RefillPlan plan = PlanRefill(true, true, kSorceressClass, false);
    CHECK(plan.fill_health && plan.fill_mana && plan.take_health && plan.take_mana);
    plan = PlanRefill(true, true, kWizardClass, false);
    CHECK(plan.fill_mana);
    plan = PlanRefill(true, true, 0, false); // a Fighter has no mana: the request is dropped, health still fills
    CHECK(plan.fill_health && !plan.fill_mana && plan.take_mana);
    plan = PlanRefill(false, true, kElfClass, false);
    CHECK(!plan.fill_health && !plan.fill_mana && plan.take_mana && !plan.take_health);
    plan = PlanRefill(true, true, kSorceressClass, true); // dead: everything waits for the revive
    CHECK(!plan.fill_health && !plan.fill_mana && !plan.take_health && !plan.take_mana);

    SetGuestFunctions({FakeIsDead, FakeRefillFunction});
    FakeGame player1(true, 0), player2(true, 1), enemy(false, 0);
    player1.SetClass(kSorceressClass);
    player2.SetClass(kSorceressClass);

    // Nothing pending: the tick does nothing, whatever the unit.
    g_refill = {};
    runtime_cheats_start(0, 1);
    GuestRegs regs = TickRegs(player1);
    PlayerTickHook(&regs);
    CHECK(g_refill.calls == 0);

    // Off -> On from the menu: one refill of Player 1's unit, health and mana together, never lowering anything.
    runtime_cheat_toggle(int(Cheat::InfiniteHealth));
    runtime_cheat_toggle(int(Cheat::InfiniteMana));
    regs = TickRegs(player2);
    PlayerTickHook(&regs);
    regs = TickRegs(enemy);
    PlayerTickHook(&regs);
    CHECK(g_refill.calls == 0); // Players 2 to 4 and everything else are never refilled
    CHECK(runtime_cheat_refills_pending() == (Bit(Cheat::InfiniteHealth) | Bit(Cheat::InfiniteMana)));
    regs = TickRegs(player1);
    PlayerTickHook(&regs);
    CHECK(g_refill.calls == 1 && g_refill.unit == player1.Unit());
    CHECK(g_refill.health == 1 && g_refill.mana == 1 && g_refill.only_below_max == 1);
    CHECK(runtime_cheat_refills_pending() == 0);
    PlayerTickHook(&regs); // every following tick: nothing
    PlayerTickHook(&regs);
    CHECK(g_refill.calls == 1);

    // On -> Off leaves everything as it is; Off -> On refills again.
    runtime_cheat_toggle(int(Cheat::InfiniteHealth));
    PlayerTickHook(&regs);
    CHECK(g_refill.calls == 1);
    runtime_cheat_toggle(int(Cheat::InfiniteHealth));
    PlayerTickHook(&regs);
    CHECK(g_refill.calls == 2 && g_refill.health == 1 && g_refill.mana == 0);

    // A launcher value that is already On refills once, when Player 1's unit first ticks.
    g_refill = {};
    runtime_cheats_start(Bit(Cheat::InfiniteMana), 1);
    FakeGame fighter(true, 0);
    fighter.SetClass(0);
    regs = TickRegs(fighter);
    PlayerTickHook(&regs);
    CHECK(g_refill.calls == 0 && runtime_cheat_refills_pending() == 0); // no mana class: dropped, nothing filled
    runtime_cheats_start(Bit(Cheat::InfiniteHealth) | Bit(Cheat::InfiniteMana), 1);
    g_refill.dead = true;
    regs = TickRegs(player1);
    PlayerTickHook(&regs);
    CHECK(g_refill.calls == 0 && runtime_cheat_refills_pending() != 0); // dead: it waits
    g_refill.dead = false;
    PlayerTickHook(&regs);
    CHECK(g_refill.calls == 1 && runtime_cheat_refills_pending() == 0);

    // The unit is validated before anything is read through it: an empty pointer or a unit without an owner is skipped.
    runtime_cheats_start(Bit(Cheat::InfiniteHealth), 1);
    g_refill.calls = 0;
    regs = TickRegs(player1);
    regs.rdi = 0;
    PlayerTickHook(&regs);
    FakeGame orphan(true, 0);
    FakeGame::Store<uint64_t>(orphan.unit + kUnitOwnerOffset, 0);
    regs = TickRegs(orphan);
    PlayerTickHook(&regs);
    CHECK(g_refill.calls == 0 && runtime_cheat_refills_pending() != 0);

    // Without the game's functions (hooks not installed) nothing is called.
    SetGuestFunctions({nullptr, nullptr});
    regs = TickRegs(player1);
    PlayerTickHook(&regs);
    CHECK(g_refill.calls == 0);
    SetGuestFunctions({FakeIsDead, FakeRefillFunction});
    runtime_cheats_start(0, 1);
    SetGuestFunctions({nullptr, nullptr});
}

void TestHookTable() {
    const std::span<const GuestHook> hooks = Hooks();
    CHECK(hooks.size() == 20);
    CHECK(hooks[19].offset == 0x27511 && hooks[19].length == 7); // the experience award, at the end of the table
    CHECK(kSupportedImage.size == 0x11f8060 && kSupportedImage.entry == 0x54c690);
    for (const GuestHook& hook : hooks) {
        CHECK(hook.length >= GUEST_HOOK_MIN_LENGTH && hook.length <= GUEST_HOOK_MAX_LENGTH);
        CHECK(hook.handler != nullptr && hook.name != nullptr);
        CHECK(hook.offset + hook.length <= kSupportedImage.size);
    }
    CHECK(hooks[0].offset == 0xd8703 && hooks[1].offset == 0xd6d70 && hooks[2].offset == 0x345380);
    CHECK(hooks[3].offset == 0x1b06f0 && hooks[4].offset == 0x537e8a && hooks[5].offset == 0x2ea420 && hooks[6].offset == 0xfe183 && hooks[7].offset == 0x2ec7cb);

    const size_t room = hooks.size() * guest_hook_stub_size();
    std::vector<uint8_t> memory(kSupportedImage.size + room, 0x90);
    uint8_t* image = memory.data();
    for (const GuestHook& hook : hooks) {
        std::memcpy(image + hook.offset, hook.original, hook.length);
    }
    size_t bad = 99;
    CHECK(guest_hooks_verify(image, kSupportedImage.size, kSupportedImage.entry, &kSupportedImage, hooks.data(),
                             hooks.size(), &bad) == GUEST_HOOK_OK);
    CHECK(guest_hooks_verify(image, kSupportedImage.size - 1, kSupportedImage.entry, &kSupportedImage, hooks.data(),
                             hooks.size(), &bad) == GUEST_HOOK_WRONG_IMAGE);
    CHECK(guest_hooks_verify(image, kSupportedImage.size, kSupportedImage.entry + 1, &kSupportedImage, hooks.data(),
                             hooks.size(), &bad) == GUEST_HOOK_WRONG_IMAGE);

    // One wrong byte anywhere in any site: refused, that hook named, nothing written.
    for (size_t i = 0; i < hooks.size(); ++i) {
        for (uint8_t at = 0; at < hooks[i].length; ++at) {
            image[hooks[i].offset + at] ^= 0x01;
            const std::vector<uint8_t> before(memory);
            CHECK(guest_hooks_install(image, kSupportedImage.size, kSupportedImage.entry, &kSupportedImage, hooks.data(),
                                      hooks.size(), image + kSupportedImage.size, room, &bad) == GUEST_HOOK_MISMATCH);
            CHECK(bad == i);
            CHECK(before == memory);
            image[hooks[i].offset + at] ^= 0x01;
        }
    }
    // The sites do not overlap each other.
    for (size_t i = 0; i < hooks.size(); ++i) {
        for (size_t j = i + 1; j < hooks.size(); ++j) {
            CHECK(hooks[i].offset + hooks[i].length <= hooks[j].offset ||
                  hooks[j].offset + hooks[j].length <= hooks[i].offset);
        }
    }

    CHECK(guest_hooks_install(image, kSupportedImage.size, kSupportedImage.entry, &kSupportedImage, hooks.data(),
                              hooks.size(), image + kSupportedImage.size, room, &bad) == GUEST_HOOK_OK);
    uint8_t* stubs = image + kSupportedImage.size;
    for (size_t i = 0; i < hooks.size(); ++i) {
        const uint8_t* site = image + hooks[i].offset;
        CHECK(site[0] == 0xe9);
        int32_t rel;
        std::memcpy(&rel, site + 1, 4);
        CHECK(site + 5 + rel == stubs + i * guest_hook_stub_size());
        for (uint8_t at = 5; at < hooks[i].length; ++at) {
            CHECK(site[at] == 0x90);
        }
        // The replaced bytes live on in the stub, followed by the jump back to the next instruction.
        const uint8_t* stub = stubs + i * guest_hook_stub_size();
        const uint8_t* found = nullptr;
        for (size_t at = 0; at + hooks[i].length <= guest_hook_stub_size() && !found; ++at) {
            if (std::memcmp(stub + at, hooks[i].original, hooks[i].length) == 0) {
                found = stub + at;
            }
        }
        CHECK(found != nullptr);
        if (found) {
            uint64_t resume;
            std::memcpy(&resume, found + hooks[i].length + 6, 8);
            CHECK(resume == reinterpret_cast<uint64_t>(site + hooks[i].length));
        }
    }
}

// A hook whose bytes do not match is skipped on its own; the others install. A wrong image installs nothing.
void TestPartialInstall() {
    const std::span<const GuestHook> hooks = Hooks();
    const size_t room = hooks.size() * guest_hook_stub_size();
    std::vector<uint8_t> memory(kSupportedImage.size + room, 0x90);
    uint8_t* image = memory.data();
    for (const GuestHook& hook : hooks) {
        std::memcpy(image + hook.offset, hook.original, hook.length);
    }
    const size_t broken = 4;
    image[hooks[broken].offset] ^= 0xff;
    CHECK(runtime_cheat_hooks_install(image, kSupportedImage.size, kSupportedImage.entry, image + kSupportedImage.size,
                                      room) == 1);
    for (size_t i = 0; i < hooks.size(); ++i) {
        const bool patched = image[hooks[i].offset] == 0xe9;
        CHECK(patched == (i != broken));
    }
    CHECK(image[hooks[broken].offset] == (hooks[broken].original[0] ^ 0xff)); // the odd site is left as it was

    std::vector<uint8_t> other(kSupportedImage.size + 4096 + room, 0x90);
    const std::vector<uint8_t> before(other);
    CHECK(runtime_cheat_hooks_install(other.data(), kSupportedImage.size + 4096, kSupportedImage.entry,
                                      other.data() + kSupportedImage.size + 4096, room) == 0);
    CHECK(other == before);

    // Too little room for the stubs installs nothing.
    std::vector<uint8_t> tight(kSupportedImage.size + room, 0x90);
    for (const GuestHook& hook : hooks) {
        std::memcpy(tight.data() + hook.offset, hook.original, hook.length);
    }
    const std::vector<uint8_t> untouched(tight);
    CHECK(runtime_cheat_hooks_install(tight.data(), kSupportedImage.size, kSupportedImage.entry,
                                      tight.data() + kSupportedImage.size, room - 1) == 0);
    CHECK(tight == untouched);
    SetGuestFunctions({nullptr, nullptr});
}

// dcp-cheats-tests --check-image <boot.bin>: the hook table against the real boot image, read-only.
int CheckImage(const char* path) {
    std::ifstream in(path, std::ios::binary);
    auto read64 = [&in]() {
        uint64_t value = 0;
        in.read(reinterpret_cast<char*>(&value), 8);
        return value;
    };
    char magic[8] = {};
    in.read(magic, 8);
    if (!in || std::memcmp(magic, "DCPBOOT1", 8) != 0) {
        std::printf("not a boot image: %s\n", path);
        return 2;
    }
    const uint64_t size = read64(), entry = read64(), segments = read64(), relocations = read64(), imports = read64();
    read64();                             // capabilities
    for (int i = 0; i < 5; ++i) read64(); // procparam, main TLS
    const uint64_t modules = read64();
    for (uint64_t i = 0; i < modules * 7; ++i) read64();
    const uint64_t bindings = read64();
    for (uint64_t i = 0; i < bindings * 3 + segments * 3; ++i) read64();
    in.seekg(static_cast<std::streamoff>(imports * 128 + relocations * 32), std::ios::cur);
    std::vector<uint8_t> image(size);
    in.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(size));
    if (!in) {
        std::printf("truncated boot image\n");
        return 2;
    }
    const std::span<const GuestHook> hooks = Hooks();
    size_t bad = 0;
    const GuestHookStatus status =
        guest_hooks_verify(image.data(), size, entry, &kSupportedImage, hooks.data(), hooks.size(), &bad);
    std::printf("image 0x%llx entry 0x%llx: %s%s%s\n", static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(entry), guest_hook_status_text(status), status ? " at " : "",
                status ? hooks[bad].name : "");
    if (status != GUEST_HOOK_OK) {
        return 1;
    }
    // The game functions the refills, Add 1,000 Gold and the durability refill call start as the runtime expects.
    const uint8_t get_gold[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x07, 0xff, 0x90, 0xd8, 0x00, 0x00, 0x00};
    const uint8_t add_gold[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb};
    if (std::memcmp(image.data() + 0x43d460, get_gold, sizeof(get_gold)) != 0 ||
        std::memcmp(image.data() + 0x43d490, add_gold, sizeof(add_gold)) != 0) {
        std::printf("the gold functions differ\n");
        return 1;
    }
    const struct {
        uint64_t offset;
        std::vector<uint8_t> bytes;
    } item_functions[] = {
        {0x281ba0, {0x48, 0x8b, 0x8f, 0xb8, 0x09, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x0b}},
        {0xe3be0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x41, 0x89, 0xd6}},
        {0x224b40, {0x48, 0x8b, 0x87, 0x10, 0x05, 0x00, 0x00}},
        {0x2251b0, {0x85, 0xf6, 0x78, 0x3b}},
        {0x21d20, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb}},
        {0x225fe0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53}},
    };
    for (const auto& function : item_functions) {
        if (std::memcmp(image.data() + function.offset, function.bytes.data(), function.bytes.size()) != 0) {
            std::printf("the game functions differ at 0x%llx\n", static_cast<unsigned long long>(function.offset));
            return 1;
        }
    }
    // The installation on a copy: every site becomes a jump to its own stub, nothing else in the image changes.
    const size_t room = hooks.size() * guest_hook_stub_size();
    std::vector<uint8_t> copy(size + room);
    std::memcpy(copy.data(), image.data(), size);
    if (guest_hooks_install(copy.data(), size, entry, &kSupportedImage, hooks.data(), hooks.size(), copy.data() + size,
                            room, &bad) != GUEST_HOOK_OK) {
        std::printf("installation failed\n");
        return 1;
    }
    size_t changed = 0;
    size_t outside = 0;
    for (size_t i = 0; i < size; ++i) {
        if (copy[i] == image[i]) {
            continue;
        }
        ++changed;
        bool in_site = false;
        for (const GuestHook& hook : hooks) {
            in_site = in_site || (i >= hook.offset && i < hook.offset + hook.length);
        }
        outside += in_site ? 0 : 1;
    }
    std::printf("installed %zu hooks on a copy: %zu bytes changed, %zu outside the sites\n", hooks.size(), changed,
                outside);
    return changed >= hooks.size() && outside == 0 ? 0 : 1;
}

// A fake game function the hook tests run: int f(int a, int b) { return a + b; } with two whole instructions to take.
struct CodeBuffer {
    uint8_t* base = nullptr;
    size_t size = 0;

    explicit CodeBuffer(size_t bytes) : size(bytes) {
#ifdef _WIN32
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
    }
    ~CodeBuffer() {
#ifdef _WIN32
        if (base) VirtualFree(base, 0, MEM_RELEASE);
#else
        if (base) munmap(base, size);
#endif
    }
};

using AddFunction = int(GUEST_HOOK_ABI*)(int, int);
using ScaleFunction = float(GUEST_HOOK_ABI*)(float, float);
using SumFunction = int(GUEST_HOOK_ABI*)(int, int, int, int, int, int);
using StoreFunction = int(GUEST_HOOK_ABI*)(int*);
using RbxFunction = int(GUEST_HOOK_ABI*)();

int g_hook_calls = 0;
GuestRegs g_seen{};

void GUEST_HOOK_ABI ReplaceSecondOperand(GuestRegs* regs) {
    ++g_hook_calls;
    g_seen = *regs;
    if (static_cast<uint32_t>(regs->rsi) == 7) {
        regs->rsi = 100;
    }
    // Wide vector work with aligned stack slots: it fails unless the stub left the stack 16-byte aligned, and it
    // overwrites xmm0-xmm7, which the stub must give back.
    volatile __m128 a = _mm_set1_ps(1.0f), b = _mm_set1_ps(2.0f), c = _mm_set1_ps(3.0f), d = _mm_set1_ps(4.0f);
    volatile __m128 sum = _mm_add_ps(_mm_add_ps(a, b), _mm_add_ps(c, d));
    (void)sum;
}

void GUEST_HOOK_ABI ClobberRegisters(GuestRegs*) {
    ++g_hook_calls;
    asm volatile("xor %%r8d, %%r8d; xor %%r9d, %%r9d; xor %%r10d, %%r10d; stc" ::: "r8", "r9", "r10", "cc");
}

void GUEST_HOOK_ABI SkipWhenFive(GuestRegs* regs) {
    ++g_hook_calls;
    g_seen = *regs;
    if (*reinterpret_cast<int*>(regs->rdi) == 5) {
        regs->skip = 1;
        regs->rax = 7;
    }
}

void TestNativeHooks() {
    constexpr uint32_t kImageSize = 0x1000;
    CodeBuffer memory(0x4000);
    CHECK(memory.base != nullptr);
    if (!memory.base) {
        return;
    }
    uint8_t* image = memory.base;
    uint8_t* stubs = memory.base + 0x2000;
    const uint8_t add_function[] = {0x8d, 0x04, 0x37, 0x83, 0xc0, 0x00, 0xc3}; // lea eax,[rdi+rsi]; add eax,0; ret
    const uint8_t scale_function[] = {0xf3, 0x0f, 0x59, 0xc1, 0x90, 0xc3};    // mulss xmm0,xmm1; nop; ret
    std::memcpy(image + 0x100, add_function, sizeof(add_function));
    std::memcpy(image + 0x200, scale_function, sizeof(scale_function));
    const uint8_t pristine_add[sizeof(add_function)] = {0x8d, 0x04, 0x37, 0x83, 0xc0, 0x00, 0xc3};

    const uint8_t sum_code[] = {0x44, 0x89, 0xc0, 0x44, 0x01, 0xc8, 0xc3}; // mov eax,r8d; add eax,r9d; ret
    std::memcpy(image + 0x300, sum_code, sizeof(sum_code));
    const SumFunction sum_function = reinterpret_cast<SumFunction>(image + 0x300);
    CHECK(sum_function(0, 0, 0, 0, 20, 22) == 42);
    const uint8_t store_code[] = {0xc7, 0x07, 0x01, 0x00, 0x00, 0x00, 0x31, 0xc0, 0xc3}; // mov [rdi],1; xor eax,eax; ret
    std::memcpy(image + 0x400, store_code, sizeof(store_code));
    const StoreFunction store = reinterpret_cast<StoreFunction>(image + 0x400);
    const uint8_t rbx_code[] = {0x89, 0xd8, 0x90, 0x90, 0x90, 0xc3}; // mov eax,ebx; nop x3; ret
    std::memcpy(image + 0x500, rbx_code, sizeof(rbx_code));
    const RbxFunction read_rbx = reinterpret_cast<RbxFunction>(image + 0x500);
    const AddFunction add = reinterpret_cast<AddFunction>(image + 0x100);
    const ScaleFunction scale = reinterpret_cast<ScaleFunction>(image + 0x200);
    CHECK(add(1, 7) == 8);

    GuestHook hooks[5] = {};
    hooks[0].name = "add";
    hooks[0].offset = 0x100;
    hooks[0].length = 6;
    std::memcpy(hooks[0].original, add_function, 6);
    hooks[0].handler = ReplaceSecondOperand;
    hooks[1].name = "scale";
    hooks[1].offset = 0x200;
    hooks[1].length = 5;
    std::memcpy(hooks[1].original, scale_function, 5);
    hooks[1].handler = ReplaceSecondOperand;
    hooks[2].name = "sum";
    hooks[2].offset = 0x300;
    hooks[2].length = 6;
    std::memcpy(hooks[2].original, sum_code, 6);
    hooks[2].handler = ClobberRegisters;
    hooks[3].name = "store";
    hooks[3].offset = 0x400;
    hooks[3].length = 6;
    std::memcpy(hooks[3].original, store_code, 6);
    hooks[3].handler = SkipWhenFive;
    hooks[4].name = "rbx";
    hooks[4].offset = 0x500;
    hooks[4].length = 5;
    std::memcpy(hooks[4].original, rbx_code, 5);
    hooks[4].handler = ReplaceSecondOperand;
    const GuestImageId id{kImageSize, 0x40};
    size_t bad = 99;

    // Wrong version: size or entry differs; nothing is written.
    CHECK(guest_hooks_verify(image, kImageSize + 1, 0x40, &id, hooks, 5, &bad) == GUEST_HOOK_WRONG_IMAGE);
    CHECK(guest_hooks_verify(image, kImageSize, 0x41, &id, hooks, 5, &bad) == GUEST_HOOK_WRONG_IMAGE);
    CHECK(guest_hooks_verify(image, kImageSize, 0x40, &id, hooks, 5, &bad) == GUEST_HOOK_OK);

    // Original bytes that do not match: refused, naming the hook, and the image is left alone.
    {
        GuestHook broken[5] = {hooks[0], hooks[1], hooks[2], hooks[3], hooks[4]};
        broken[1].original[2] ^= 0xff;
        uint8_t before[0x300];
        std::memcpy(before, image, sizeof(before));
        CHECK(guest_hooks_install(image, kImageSize, 0x40, &id, broken, 5, stubs, 0x1000, &bad) == GUEST_HOOK_MISMATCH);
        CHECK(bad == 1);
        CHECK(std::memcmp(before, image, sizeof(before)) == 0);
        CHECK(add(1, 7) == 8);
    }
    // Overlapping sites, a site outside the image, a patch too short or without a handler, a stub area too small.
    {
        GuestHook overlap[2] = {hooks[0], hooks[0]};
        overlap[1].offset = 0x103;
        std::memcpy(overlap[1].original, add_function + 3, 4);
        overlap[1].length = 5;
        CHECK(guest_hooks_verify(image, kImageSize, 0x40, &id, overlap, 2, &bad) == GUEST_HOOK_MISMATCH ||
              guest_hooks_verify(image, kImageSize, 0x40, &id, overlap, 2, &bad) == GUEST_HOOK_OVERLAP);
        GuestHook same[2] = {hooks[0], hooks[0]};
        CHECK(guest_hooks_verify(image, kImageSize, 0x40, &id, same, 2, &bad) == GUEST_HOOK_OVERLAP && bad == 1);
        GuestHook outside = hooks[0];
        outside.offset = kImageSize - 2;
        CHECK(guest_hooks_verify(image, kImageSize, 0x40, &id, &outside, 1, &bad) == GUEST_HOOK_OUT_OF_RANGE);
        GuestHook shorter = hooks[0];
        shorter.length = 4;
        CHECK(guest_hooks_verify(image, kImageSize, 0x40, &id, &shorter, 1, &bad) == GUEST_HOOK_BAD_HOOK);
        GuestHook longer = hooks[0];
        longer.length = GUEST_HOOK_MAX_LENGTH + 1;
        CHECK(guest_hooks_verify(image, kImageSize, 0x40, &id, &longer, 1, &bad) == GUEST_HOOK_BAD_HOOK);
        GuestHook idle = hooks[0];
        idle.handler = nullptr;
        CHECK(guest_hooks_verify(image, kImageSize, 0x40, &id, &idle, 1, &bad) == GUEST_HOOK_BAD_HOOK);
        CHECK(guest_hooks_install(image, kImageSize, 0x40, &id, hooks, 5, stubs, guest_hook_stub_size() * 2, &bad) ==
              GUEST_HOOK_NO_ROOM);
        CHECK(std::memcmp(image + 0x100, pristine_add, sizeof(pristine_add)) == 0);
    }

    // The installed hooks: the jump, the nops, and the replaced bytes in the stub.
    CHECK(guest_hooks_install(image, kImageSize, 0x40, &id, hooks, 5, stubs, 0x1000, &bad) == GUEST_HOOK_OK);
    CHECK(image[0x100] == 0xe9);
    CHECK(image[0x105] == 0x90 && image[0x106] == 0xc3); // the sixth byte is a nop, the return is untouched
    int32_t rel;
    std::memcpy(&rel, image + 0x101, 4);
    CHECK(image + 0x105 + rel == stubs);
    CHECK(image[0x200] == 0xe9 && image[0x205] == 0xc3);
    CHECK(std::memcmp(stubs + guest_hook_stub_size() - 1, "\xcc", 1) == 0); // unused stub bytes trap

    // The behavior: the handler runs, may change the arguments, and everything else survives.
    g_hook_calls = 0;
    CHECK(add(1, 2) == 3);
    CHECK(g_hook_calls == 1);
    CHECK(static_cast<uint32_t>(g_seen.rdi) == 1 && static_cast<uint32_t>(g_seen.rsi) == 2);
    CHECK(add(5, 7) == 105); // the handler replaced the second argument
    CHECK(g_hook_calls == 2);
    CHECK(add(-3, 3) == 0);
    CHECK(scale(1.5f, 4.0f) == 6.0f); // the float arguments survived the handler's vector work
    CHECK(g_hook_calls == 4);

    // Caller-saved registers the handler overwrites come back as they were.
    CHECK(sum_function(1, 2, 3, 4, 20, 22) == 42);
    CHECK(g_hook_calls == 5);

    // A handler at a function's first instruction may return for it, or let it run: the replaced instruction runs.
    int cell = 0;
    CHECK(store(&cell) == 0 && cell == 1);
    cell = 5;
    CHECK(store(&cell) == 7 && cell == 5);
    CHECK(g_hook_calls == 7);

    // The callee-saved registers reach the handler too (the hooked code keeps its locals behind rbp).
    const int rbx_value = read_rbx();
    CHECK(static_cast<int>(g_seen.rbx) == rbx_value);
    CHECK(g_hook_calls == 8);
}

// Infinite Arrows holds Player 1's Elf at 10 from the next tick on, whatever the count was; Off lets the count go.
void TestArrowHold() {
    FakeGame elf(true, 0), fighter(true, 0), player2(true, 1), enemy(false, 0);
    fighter.SetClass(0);
    const auto tick = [](const FakeGame& game) {
        GuestRegs regs = TickRegs(game);
        PlayerTickHook(&regs);
    };
    const auto set = [](FakeGame& game, int32_t value) { FakeGame::Store<int32_t>(game.owner + kArrowCountOffset, value); };
    const auto count = [](const FakeGame& game) { return TestMemory{}.S32(game.Owner() + kArrowCountOffset); };

    SetGuestFunctions({nullptr, nullptr});
    runtime_cheats_start(Bit(Cheat::InfiniteArrows), 1); // the launcher's On: from the first tick
    for (const int32_t start : {0, 1, 5, 10}) {
        set(elf, start);
        tick(elf);
        CHECK(count(elf) == 10);
    }
    set(elf, 15); // a pickup while the cheat is On does not stay
    tick(elf);
    CHECK(count(elf) == 10);

    runtime_cheats_start(0, 1); // the menu's On: the same at the next tick
    set(elf, 1);
    tick(elf);
    CHECK(count(elf) == 1);
    runtime_cheat_toggle(int(Cheat::InfiniteArrows));
    tick(elf);
    CHECK(count(elf) == 10);
    runtime_cheat_toggle(int(Cheat::InfiniteArrows)); // Off: nothing is forced, the game's count stands
    set(elf, 3);
    tick(elf);
    CHECK(count(elf) == 3);

    runtime_cheats_start(Bit(Cheat::InfiniteArrows), 1); // only Player 1's Elf: other players, classes and enemies keep theirs
    set(player2, 1);
    set(fighter, 2);
    set(enemy, 4);
    tick(player2);
    tick(fighter);
    tick(enemy);
    CHECK(count(player2) == 1 && count(fighter) == 2 && count(enemy) == 4);
    runtime_cheats_start(0, 1);
}

// Experience Multiplier: the saved value, the menu's cycle, the award rule and the hook on the award.
void TestExperienceMultiplier() {
    CHECK(kExperienceMultiplierOff == 1);
    CHECK(std::string(kExperienceMultiplierKey) == "cheat_experience_multiplier");
    CHECK(std::string(kExperienceMultiplierName) == "Experience Multiplier");

    // The saved value is the multiplier; 1, or a missing or unknown value, is Off.
    CHECK(ExperienceMultiplierFromSaved("1") == 1 && ExperienceMultiplierFromSaved("2") == 2);
    CHECK(ExperienceMultiplierFromSaved("5") == 5 && ExperienceMultiplierFromSaved("10") == 10);
    for (const char* saved : {"", "0", "3", "Off", "2x", "-5"}) {
        CHECK(ExperienceMultiplierFromSaved(saved) == 1);
    }
    CHECK(ListedExperienceMultiplier(5) == 5 && ListedExperienceMultiplier(3) == 1);

    // The menu's order: Off, 2x, 5x, 10x, and Off again.
    CHECK(NextExperienceMultiplier(1) == 2 && NextExperienceMultiplier(2) == 5);
    CHECK(NextExperienceMultiplier(5) == 10 && NextExperienceMultiplier(10) == 1);
    CHECK(NextExperienceMultiplier(7) == 2); // unlisted: from Off
    CHECK(std::string(ExperienceMultiplierLabel(1)) == "Off" && std::string(ExperienceMultiplierLabel(2)) == "2x");
    CHECK(std::string(ExperienceMultiplierLabel(5)) == "5x" && std::string(ExperienceMultiplierLabel(10)) == "10x");

    // The award: the gain is multiplied once, with no wrap; a zero or negative gain is left as it is.
    CHECK(MultipliedExperience(1, 100) == 100);
    CHECK(MultipliedExperience(2, 100) == 200);
    CHECK(MultipliedExperience(5, 100) == 500);
    CHECK(MultipliedExperience(10, 100) == 1000);
    CHECK(MultipliedExperience(10, 0) == 0 && MultipliedExperience(10, -40) == -40);
    CHECK(MultipliedExperience(2, INT32_MAX / 2) == int64_t{INT32_MAX / 2} * 2);
    CHECK(MultipliedExperience(2, INT32_MAX / 2 + 1) == INT32_MAX);
    CHECK(MultipliedExperience(10, int64_t{1} << 40) == INT32_MAX);

    // The menu's change is a difference from the saved value: cycling back to it follows the launcher again.
    SessionExperience session;
    CHECK(session.Effective(5) == 5);
    CHECK(session.Cycle(5) == 10 && session.Effective(5) == 10);
    CHECK(session.Cycle(5) == 1 && session.Cycle(5) == 2 && session.Cycle(5) == 5);
    CHECK(session.Effective(5) == 5);

    // The runtime: the saved value starts each session, the menu's cycle changes it, an unlisted saved value is Off.
    runtime_cheats_start(0, 5);
    CHECK(runtime_cheat_experience_multiplier() == 5);
    CHECK(runtime_cheat_experience_cycle() == 10 && runtime_cheat_experience_multiplier() == 10);
    runtime_cheats_start(0, 2);
    CHECK(runtime_cheat_experience_multiplier() == 2);
    runtime_cheats_start(0, 3);
    CHECK(runtime_cheat_experience_multiplier() == 1);

    // The award hook changes only the gain the function returns (rax); the character's total is never touched.
    const GuestHook* hook = FindHook("experience");
    CHECK(hook != nullptr && hook->offset == 0x27511 && hook->length == 7);
    GuestRegs award{};
    award.rax = 100;
    award.r12 = 4000;
    runtime_cheats_start(0, 1);
    ExperienceHook(&award);
    CHECK(award.rax == 100);
    for (const int saved : {2, 5, 10}) {
        runtime_cheats_start(0, saved);
        award = {};
        award.rax = 100;
        award.r12 = 4000;
        ExperienceHook(&award);
        CHECK(award.rax == uint64_t(saved) * 100 && award.r12 == 4000);
    }
    // Each award is multiplied on its own: the handler keeps nothing between awards.
    runtime_cheats_start(0, 10);
    award = {};
    award.rax = 100;
    ExperienceHook(&award);
    award.rax = 100;
    ExperienceHook(&award);
    CHECK(award.rax == 1000);
    runtime_cheats_start(0, 1);

    const std::string overlay = ReadSource("gpu/shim/dcp_overlay.cpp");
    CHECK(overlay.find("runtime_cheat_experience_cycle()") != std::string::npos);
    CHECK(ReadSource("launcher/launcher.cpp").find("kExperienceChoices") != std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--check-image") {
        return CheckImage(argv[2]);
    }
    TestCheatList();
    TestSessionOverride();
    TestRefillTransitions();
    TestMenuModel();
    TestRules();
    TestHookHandlers();
    TestArrowsAndSkillPoints();
    TestRefillOnEnable();
    TestArrowHold();
    TestExperienceMultiplier();
    TestDurability();
    TestDurabilityRefill();
    TestLives();
    TestGold();
    TestAddGold();
    TestMenuInput();
    TestMovement();
    TestHookTable();
    TestPartialInstall();
    TestNativeHooks();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
