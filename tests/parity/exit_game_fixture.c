#include "../../src/exit_game_hook.c"

#include "fixture.h"

/* What the installer checks beyond the located windows: the rows, the part table and the menu's jump table. */
void fixture_exit_game(Fx *fx, uint64_t row_widget, uint64_t tick, const uint64_t table_loads[2]) {
    fx_context(fx, "Exit Game");
    const uint64_t block = row_widget - EXIT_GAME_WIDGET_BLOCK_BACK;
    FX_BYTES(fx, block, kWidgetBlockHead);
    fx_call(fx, block + WIDGET_BLOCK_SIZE - 5, 16);
    for (int i = 0; i < ITEM_COUNT; ++i) {
        const uint8_t load[5] = {0xbe, kItemIds[i], 0x01, 0, 0};
        FX_BYTES(fx, block + WIDGET_BLOCK_SIZE + (uint64_t)i * ITEM_STRIDE, load);
        FX_BYTES(fx, block + WIDGET_BLOCK_SIZE + (uint64_t)i * ITEM_STRIDE + 5, kItemTail);
    }
    const uint8_t cut[5] = {0xbe, HELP_LIMIT_OLD, 0, 0, 0};
    FX_BYTES(fx, row_widget - CUT_IMMEDIATE_BACK - 1, cut);
    fx_call(fx, row_widget, 16);

    const uint64_t parts = fx_alloc(fx, 4 * (PART_TABLE_LAST + 1));
    FX_BYTES(fx, parts + 4 * PART_TABLE_FIRST, kPartTable);
    for (int i = 0; i < 2; ++i) {
        const uint8_t lea[3] = {0x48, 0x8d, 0x15};
        FX_BYTES(fx, table_loads[i], lea);
        fx_rel_to(fx, table_loads[i] + 3, table_loads[i] + 7, parts);
    }

    const uint8_t ja[2] = {0x0f, 0x87};
    FX_BYTES(fx, tick, kTickBytes);
    FX_BYTES(fx, tick + TICK_LENGTH, kTickTail);
    FX_BYTES(fx, tick + TICK_JA, ja);
    FX_BYTES(fx, tick + TICK_LEA, kTickLea);
    FX_BYTES(fx, tick + TICK_JUMP, kTickJump);
    const uint64_t fallback = fx_rel(fx, tick + TICK_JA + 2, tick + TICK_JA + 6, 16);
    const uint64_t table = fx_rel(fx, tick + TICK_LEA + 3, tick + TICK_JUMP, 4 * TABLE_ENTRIES);
    fx_entry(fx, table, 1, fallback);
    for (int k = 2; k < TABLE_ENTRIES; ++k) {
        const uint64_t target = fx_alloc(fx, 0x10);
        const uint8_t body[CASE_LENGTH] = {0x49, 0x8b, 0x06, 0xbe, 0x01, 0, 0, 0, 0xba, (uint8_t)k, 0, 0, 0};
        FX_BYTES(fx, target, body);
        fx_entry(fx, table, k, target);
    }
}
