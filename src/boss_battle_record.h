/* Boss Battle's own persistent data: per character and boss, attempts, wins, deaths, quits and the best win time.
 * Kept apart from the campaign save in <user>/boss_battle.dat and replaced atomically on every write. */
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

/* The Boss Battle character's own progression, kept apart from the campaign in <user>/boss_battle_progress.dat: its
 * level (as the character record keeps it, from 0) and experience. An unreadable file is left untouched and every
 * character starts from the campaign. */
typedef struct {
    int32_t level, xp_pool, xp_progress;
} BossBattleProgress;
void boss_battle_progress_configure(const char *user_dir);
int boss_battle_progress_get(const char *character, BossBattleProgress *out);
int boss_battle_progress_put(const char *character, const BossBattleProgress *progress);
int boss_battle_progress_parse(const char *text, size_t length);
size_t boss_battle_progress_format(char *out, size_t size);

/* The file format, for the tests: parse replaces the table, format writes it. */
int boss_battle_record_parse(const char *text, size_t length);
size_t boss_battle_record_format(char *out, size_t size);
void boss_battle_record_clear(void);

#ifdef __cplusplus
}
#endif

#endif
