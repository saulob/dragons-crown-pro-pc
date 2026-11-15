// Focused tests of the Boss Battle screen on the native Options page (order, rows, choices, Fight All, Return to tavern), the
// single quit path behind F4, and what the redesigns removed: no ImGui selector, no boss list on the Inn's own list,
// no Boss Battle on the title menu.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "boss_battle.h"
#include "exit_game_hook.h"

namespace {

int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);  \
            ++failures;                                          \
        }                                                        \
    } while (0)

std::string Source(const char* path) {
    std::ifstream in(std::string(DCP_SOURCE_DIR) + "/" + path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

bool Exists(const char* path) {
    return std::ifstream(std::string(DCP_SOURCE_DIR) + "/" + path).good();
}

void TestOrder() {
    static const char* const kLabels[BOSS_BATTLE_ENCOUNTERS] = {
        "01 Harpy",         "02 Wyvern",   "03 Doom Beetle",   "04 Golem",        "05 Gazer",
        "06 Pirates",       "07 Minotaur", "08 Vampire",       "09 Warlock",      "10 Medusa",
        "11 Wraith",        "12 Killer Rabbit", "13 Kraken",   "14 Cyclops",      "15 Chimera",
        "16 Gargoyle Gate", "17 Red Dragon", "18 Arch Demon",  "19 Ancient Dragon", "20 Demon Lord"};
    char label[64];
    CHECK(BOSS_BATTLE_SCREEN_ROWS == 8 && BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW == 0 && BOSS_BATTLE_SCREEN_BOSS_ROWS == 6 &&
          BOSS_BATTLE_SCREEN_RETURN_ROW == 7 && BOSS_BATTLE_INN_OPTIONS_ROW == 3);
    for (int i = 0; i < BOSS_BATTLE_ENCOUNTERS; ++i) CHECK(boss_battle_label(i, label, sizeof(label)) && !std::strcmp(label, kLabels[i]));
    CHECK(!boss_battle_label(BOSS_BATTLE_ENCOUNTERS, label, sizeof(label)));
    CHECK(BOSS_BATTLE_RETURN_TO_TAVERN_RECORD == 1120);

    // the screen, walked down from its top: Fight All, 01-20, Return to tavern
    boss_battle_reset();
    boss_battle_screen_request();
    CHECK(boss_battle_screen_built() == 1);
    std::string order;
    int row = BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW;
    for (int entry = BOSS_BATTLE_ENTRY_FIGHT_ALL; entry < BOSS_BATTLE_ENTRIES; ++entry) {
        CHECK(boss_battle_screen_entry(row) == entry);
        boss_battle_screen_label(row, label, sizeof(label));
        order += entry == BOSS_BATTLE_ENTRY_RETURN_TO_TAVERN ? "Return to tavern" : label;
        order += '|';
        if (!boss_battle_screen_move(row, 1)) ++row;
    }
    std::string expected = "Fight All|";
    for (const char* boss : kLabels) expected += std::string(boss) + "|";
    CHECK(order == expected + "Return to tavern|");
    boss_battle_reset();
}

// Every boss can be reached by scrolling the six boss rows and starts from its row; Fight All is its own choice, never
// a boss; the bottom action closes; nothing else acts, and a screen that is not shown never answers.
void TestChoices() {
    boss_battle_reset();
    int index = -1;
    CHECK(boss_battle_screen_choose(1, &index) == BOSS_BATTLE_SCREEN_NONE && index == -1);
    boss_battle_screen_request();
    CHECK(boss_battle_screen_built() == 1);
    int reached[BOSS_BATTLE_ENCOUNTERS] = {0};
    for (int step = 0; step < BOSS_BATTLE_ENCOUNTERS; ++step) {
        for (int row = 1; row <= BOSS_BATTLE_SCREEN_BOSS_ROWS; ++row) {
            index = -1;
            CHECK(boss_battle_screen_choose(row, &index) == BOSS_BATTLE_SCREEN_START &&
                  index == boss_battle_screen_boss(row));
            reached[index] = 1;
        }
        boss_battle_screen_move(BOSS_BATTLE_SCREEN_BOSS_ROWS, 1);
    }
    for (int i = 0; i < BOSS_BATTLE_ENCOUNTERS; ++i) CHECK(reached[i]);
    CHECK(boss_battle_screen_choose(BOSS_BATTLE_SCREEN_RETURN_ROW, &index) == BOSS_BATTLE_SCREEN_CLOSE);
    index = -1;
    CHECK(boss_battle_screen_choose(BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW, &index) == BOSS_BATTLE_SCREEN_FIGHT_ALL && index == -1);
    CHECK(boss_battle_screen_boss(BOSS_BATTLE_SCREEN_FIGHT_ALL_ROW) == -1 &&
          boss_battle_entry_boss(BOSS_BATTLE_ENTRY_FIGHT_ALL) == -1);
    CHECK(boss_battle_screen_choose(-1, &index) == BOSS_BATTLE_SCREEN_NONE);
    CHECK(boss_battle_screen_choose(-2, &index) == BOSS_BATTLE_SCREEN_NONE);
    boss_battle_reset();
}

// Every boss is confirmed: none carries a pending state or text, the Demon King included.
void TestNoPendingBoss() {
    for (int i = 0; i < BOSS_BATTLE_ENCOUNTERS; ++i) CHECK(boss_battle_encounter(i)->unavailable == nullptr);
    for (const char* path : {"src/boss_battle.c", "src/boss_battle.h", "src/boss_battle_hook.c"})
        CHECK(!Contains(Source(path), "not yet confirmed") && !Contains(Source(path), "Pending:"));
    CHECK(boss_battle_mode_pending(BOSS_BATTLE_MODE_OFFLINE) == nullptr &&
          boss_battle_mode_pending(BOSS_BATTLE_MODE_ONLINE) != nullptr && boss_battle_mode() == BOSS_BATTLE_MODE_OFFLINE);
}

// The screen is the game's Options page: no ImGui screen exists or opens, the Inn's own list stays the Inn menu, and
// F4 reaches the one quit function.
void TestNoImGuiSelector() {
    CHECK(!Exists("gpu/shim/dcp_boss_battle_menu.cpp") && !Exists("gpu/shim/dcp_boss_battle_menu.h"));
    const std::string overlay = Source("gpu/shim/dcp_overlay.cpp");
    CHECK(!Contains(overlay, "DcpBossBattleMenu") && !Contains(overlay, "boss_open") &&
          !Contains(overlay, "selector_request") && !Contains(overlay, "boss_battle_request("));
    CHECK(Contains(overlay, "boss_battle_quit_key(event.key.repeat, (event.key.mod & kBossBattleQuitModifiers) != 0,"));
    const std::string core = Source("src/boss_battle.c");
    CHECK(Contains(core, "return boss_battle_request_quit(BOSS_BATTLE_QUIT_KEY);"));
    const std::string hook = Source("src/boss_battle_hook.c");
    CHECK(!Contains(hook, "open_boss_menu") && !Contains(hook, "LIST_VTABLE_ADD_TEXT") && !Contains(hook, "close_boss_menu"));
    CHECK(Contains(hook, "regs->rax = BOSS_BATTLE_INN_OPTIONS_ROW;") && Contains(hook, "BOSS_BATTLE_RETURN_TO_TAVERN_RECORD") &&
          Contains(hook, "boss_battle_options_built_hook"));
}

void TestTitleRestored() {
    CHECK(EXIT_GAME_MENU_ITEMS == 4 && EXIT_GAME_MENU_WIDGETS == 4 && EXIT_GAME_ROW_INDEX == 3);
    for (const char* path : {"src/exit_game_hook.c", "src/exit_game_hook.h", "src/title_layout.h", "src/loader.c",
                             "scripts/title_layout_patch.py"})
        CHECK(!Contains(Source(path), "boss_battle_title") && !Contains(Source(path), "TITLE_BOSS_BATTLE_ROW") &&
              !Contains(Source(path), "Boss Battle"));
    CHECK(Contains(Source("scripts/title_layout_patch.py"), "KEEP_ROWS = 4\n"));
}

} // namespace

int main() {
    TestOrder();
    TestChoices();
    TestNoPendingBoss();
    TestNoImGuiSelector();
    TestTitleRestored();
    if (failures) {
        std::printf("%d Boss Battle menu check(s) failed\n", failures);
        return 1;
    }
    std::puts("Boss Battle menu tests passed");
    return 0;
}
