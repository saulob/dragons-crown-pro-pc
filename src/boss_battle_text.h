/* Boss Battle's words in the Menu & Subtitles language: the game's own boss, class and Inn names, the port's words for the
 * rest. Display only: statistics, records and logs keep the English ids. */
#ifndef DCP_BOSS_BATTLE_TEXT_H
#define DCP_BOSS_BATTLE_TEXT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOSS_BATTLE_TEXT_FIGHT_ALL,
    BOSS_BATTLE_TEXT_FIGHT_ALL_HELP,
    BOSS_BATTLE_TEXT_RETURN_HELP,
    BOSS_BATTLE_TEXT_ATTEMPTS,
    BOSS_BATTLE_TEXT_WINS,
    BOSS_BATTLE_TEXT_DEATHS,
    BOSS_BATTLE_TEXT_QUITS,
    BOSS_BATTLE_TEXT_WIN_RATE,
    BOSS_BATTLE_TEXT_BEST_TIME,
    BOSS_BATTLE_TEXT_RUNS,
    BOSS_BATTLE_TEXT_CLEARS,
    BOSS_BATTLE_TEXT_BEST_PROGRESS,
    BOSS_BATTLE_TEXT_QUIT_ROW,
    BOSS_BATTLE_TEXT_QUIT_CONFIRM,
    BOSS_BATTLE_TEXT_CHARACTER, /* format: name, class, level */
    BOSS_BATTLE_TEXT_COUNT
} BossBattleTextKey;

enum { BOSS_BATTLE_TEXT_LANGUAGES = 10, BOSS_BATTLE_TEXT_BOSSES = 20, BOSS_BATTLE_TEXT_CLASSES = 6 };

/* The language of a launcher TextLanguage key; English for any other. `kana_cyrillic`: the font draws Russian over kana.
 * Returns 1 when the key is one of the table's. */
int boss_battle_text_set_language(const char *key, int kana_cyrillic);
const char *boss_battle_text_language(void);

const char *boss_battle_text(BossBattleTextKey key);
/* By encounter index; "" past the last. */
const char *boss_battle_text_boss(int index);
/* By the game's class index; NULL past the six. */
const char *boss_battle_text_class(int class_index);

/* The table's language keys, for the tests; NULL past the last. */
const char *boss_battle_text_language_key(int index);

#ifdef __cplusplus
}
#endif

#endif
