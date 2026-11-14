/* Boss Battle's own persistent data: per character and boss, attempts, wins, deaths, quits and the best win time, and per
 * character the Fight All run statistics. Kept apart from the campaign save in <user>/boss_battle.dat and replaced atomically on every write. */
#ifndef DCP_BOSS_BATTLE_RECORD_H
#define DCP_BOSS_BATTLE_RECORD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { BOSS_BATTLE_ATTEMPT, BOSS_BATTLE_WIN, BOSS_BATTLE_DEATH, BOSS_BATTLE_QUIT } BossBattleOutcome;

typedef struct {
    unsigned attempts, wins, deaths, quits;
    uint32_t best_ms; /* 0: no timed win yet */
} BossBattleStats;

/* The folder that holds boss_battle.dat; loads what is there. */
void boss_battle_record_configure(const char *user_dir);

/* Adds one outcome and writes the file; `ms` counts only for a win. A terminal outcome needs an attempt it can close
 * (wins + deaths + quits stay at most attempts). 1 when the file was written. */
int boss_battle_record_note(const char *character, const char *boss_id, BossBattleOutcome outcome, uint32_t ms);

/* What is recorded for the pair; zeros when nothing is. */
BossBattleStats boss_battle_record_stats(const char *character, const char *boss_id);

enum { BOSS_BATTLE_FIGHT_ALL_BOSSES = 20 };

typedef enum {
    BOSS_BATTLE_FIGHT_ALL_RUN,      /* the first boss became playable */
    BOSS_BATTLE_FIGHT_ALL_PROGRESS, /* value: bosses defeated so far */
    BOSS_BATTLE_FIGHT_ALL_CLEAR,    /* value: the combat time of the whole run in ms */
    BOSS_BATTLE_FIGHT_ALL_DEATH,    /* value: bosses defeated */
    BOSS_BATTLE_FIGHT_ALL_QUIT      /* value: bosses defeated */
} BossBattleFightAllEvent;

typedef struct {
    unsigned runs, clears, deaths, quits, best_progress;
    uint32_t best_ms; /* 0: no clear yet */
} BossBattleFightAllStats;

/* Adds one Fight All event for the character and writes the file. A clear, death or quit needs a run it can close
 * (clears + deaths + quits stay at most runs); progress counts only as a new best. 1 when the file is up to date. */
int boss_battle_record_fight_all_note(const char *character, BossBattleFightAllEvent event, unsigned value);
BossBattleFightAllStats boss_battle_record_fight_all_stats(const char *character);

/* The file format, for the tests: parse replaces the table, format writes it. */
int boss_battle_record_parse(const char *text, size_t length);
size_t boss_battle_record_format(char *out, size_t size);
void boss_battle_record_clear(void);

#ifdef __cplusplus
}
#endif

#endif
