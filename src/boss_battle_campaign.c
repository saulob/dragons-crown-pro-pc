#include "boss_battle.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CONTEXT_SAVE = 0xe8,
    SAVE_USER = 0x20eb15
};

static _Atomic uint64_t play_data_table, context_in_use;
/* Game thread only. */
static uint64_t kept_save, kept_play_data;
static uint8_t *save_copy, *play_data_copy;
static atomic_int held, unrestored;

static uint64_t read_u64(uint64_t address) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

void boss_battle_campaign_configure(uint64_t table) {
    atomic_store(&play_data_table, table);
}

void boss_battle_campaign_use(uint64_t context) {
    atomic_store(&context_in_use, context);
}

/* The live play data of the save's user, as the game's own getter finds it. */
static uint64_t play_data_of(uint64_t save) {
    const uint64_t table = atomic_load(&play_data_table);
    if (!table || !save) return 0;
    return read_u64(table + 8 * (uint64_t)*(const uint8_t *)(uintptr_t)(save + SAVE_USER));
}

static uint64_t current_save(void) {
    const uint64_t context = atomic_load(&context_in_use);
    return context ? read_u64(context + CONTEXT_SAVE) : 0;
}

uint64_t boss_battle_campaign_play_data(void) {
    return play_data_of(current_save());
}

int boss_battle_campaign_snapshot(void) {
    const uint64_t save = current_save(), play_data = play_data_of(save);
    if (!save || !play_data) {
        puts("Boss Battle: the campaign save is not in memory");
        return 0;
    }
    if (!save_copy) save_copy = malloc(BOSS_BATTLE_SAVE_BYTES);
    if (!play_data_copy) play_data_copy = malloc(BOSS_BATTLE_PLAY_DATA_BYTES);
    if (!save_copy || !play_data_copy) return 0;
    memcpy(save_copy, (const void *)(uintptr_t)save, BOSS_BATTLE_SAVE_BYTES);
    memcpy(play_data_copy, (const void *)(uintptr_t)play_data, BOSS_BATTLE_PLAY_DATA_BYTES);
    kept_save = save;
    kept_play_data = play_data;
    atomic_store(&held, 1);
    return 1;
}

/* Only into the same objects the snapshot came from; anything else is left alone and every later save keeps writing the
 * campaign as it was before the fight. */
int boss_battle_campaign_restore(void) {
    if (!atomic_load(&held)) return 1;
    const uint64_t save = current_save();
    if (save != kept_save || play_data_of(save) != kept_play_data) {
        atomic_store(&unrestored, 1);
        puts("Boss Battle: the campaign save in memory is not the one kept; saves keep the state from before the fight");
        return 0;
    }
    memcpy((void *)(uintptr_t)kept_save, save_copy, BOSS_BATTLE_SAVE_BYTES);
    memcpy((void *)(uintptr_t)kept_play_data, play_data_copy, BOSS_BATTLE_PLAY_DATA_BYTES);
    atomic_store(&held, 0);
    return 1;
}

uint64_t boss_battle_campaign_save_source(uint64_t save) {
    if ((atomic_load(&held) || atomic_load(&unrestored)) && save == kept_save && save_copy)
        return (uint64_t)(uintptr_t)save_copy;
    return save;
}

int boss_battle_campaign_held(void) {
    return atomic_load(&held);
}

void boss_battle_campaign_reset(void) {
    atomic_store(&held, 0);
    atomic_store(&unrestored, 0);
    atomic_store(&context_in_use, 0);
    kept_save = kept_play_data = 0;
}
