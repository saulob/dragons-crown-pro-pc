/* Boss Battle's words in every Menu & Subtitles language: each key and each boss name resolves, no language but English
 * shows the English words, the character line keeps its arguments, and Russian fits the kana-drawn fonts. */
#include <stdio.h>
#include <string.h>

#include "boss_battle_text.h"
#include "exit_game_text.h"

static int failures;
#define CHECK(x)                                                    \
    do {                                                            \
        if (!(x)) {                                                 \
            ++failures;                                             \
            printf("FAIL line %d: %s\n", __LINE__, #x);             \
        }                                                           \
    } while (0)

static const char *const kKeyNames[BOSS_BATTLE_TEXT_COUNT] = {
    "Fight All", "Fight All help", "Back to Inn help", "Attempts", "Wins",      "Deaths",           "Quits",       "Win Rate",
    "Best Time", "Runs",           "Clears",           "Best Progress", "Quit Boss Battle", "Quit confirmation", "character"};

static const char *english_text[BOSS_BATTLE_TEXT_COUNT];
static const char *english_boss[BOSS_BATTLE_TEXT_BOSSES];

/* A word the language may share with English: a boss the game itself names the same, or a format. */
static int may_match_english(int key) {
    return key == BOSS_BATTLE_TEXT_CHARACTER;
}

static int is_format(const char *text) {
    const char *first = strstr(text, "%s"), *second = first ? strstr(first + 2, "%s") : NULL;
    const char *level = second ? strstr(second + 2, "%d") : NULL;
    int percents = 0;
    for (const char *c = text; *c; ++c) percents += *c == '%';
    return level && percents == 3;
}

static int has_cyrillic(const char *text) {
    for (const unsigned char *c = (const unsigned char *)text; *c; ++c)
        if (*c == 0xd0 || *c == 0xd1) return 1;
    return 0;
}

static const ExitGameLanguage *exit_language(const char *key) {
    for (int i = 0; i < EXIT_GAME_LANGUAGE_COUNT; ++i)
        if (!strcmp(exit_game_languages[i].key, key)) return &exit_game_languages[i];
    return NULL;
}

static void test_english(void) {
    CHECK(boss_battle_text_set_language("english", 0) == 1 && !strcmp(boss_battle_text_language(), "english"));
    for (int k = 0; k < BOSS_BATTLE_TEXT_COUNT; ++k) english_text[k] = boss_battle_text((BossBattleTextKey)k);
    for (int b = 0; b < BOSS_BATTLE_TEXT_BOSSES; ++b) english_boss[b] = boss_battle_text_boss(b);
    CHECK(!strcmp(english_text[BOSS_BATTLE_TEXT_FIGHT_ALL], "Fight All"));
    CHECK(!strcmp(english_text[BOSS_BATTLE_TEXT_QUIT_ROW], "Quit Boss Battle"));
    CHECK(!strcmp(english_boss[0], "Harpy") && !strcmp(english_boss[19], "Demon Lord"));
    CHECK(!strcmp(boss_battle_text_class(0), "Fighter") && !strcmp(boss_battle_text_class(5), "Wizard"));
    CHECK(boss_battle_text_class(6) == NULL && boss_battle_text_class(-1) == NULL);
    CHECK(!strcmp(boss_battle_text_boss(20), "") && !strcmp(boss_battle_text_boss(-1), ""));
    CHECK(!strcmp(boss_battle_text(BOSS_BATTLE_TEXT_COUNT), ""));
}

/* Every in-game language the port names (the Exit Game table is the full Menu & Subtitles list) has Boss Battle words. */
static void test_every_language(void) {
    int languages = 0;
    for (int i = 0; i < EXIT_GAME_LANGUAGE_COUNT; ++i) {
        const char *key = exit_game_languages[i].key;
        CHECK(boss_battle_text_set_language(key, 0) == 1 && !strcmp(boss_battle_text_language(), key));
        const int english = !strcmp(key, "english");
        int same_bosses = 0;
        for (int k = 0; k < BOSS_BATTLE_TEXT_COUNT; ++k) {
            const char *text = boss_battle_text((BossBattleTextKey)k);
            CHECK(text && *text);
            if (!english && !may_match_english(k) && !strcmp(text, english_text[k])) {
                printf("  %s: %s is the English text\n", key, kKeyNames[k]);
                CHECK(!"a non-English language shows an English label");
            }
        }
        CHECK(is_format(boss_battle_text(BOSS_BATTLE_TEXT_CHARACTER)));
        for (int b = 0; b < BOSS_BATTLE_TEXT_BOSSES; ++b) {
            const char *name = boss_battle_text_boss(b);
            CHECK(name && *name);
            same_bosses += !strcmp(name, english_boss[b]);
        }
        for (int c = 0; c < BOSS_BATTLE_TEXT_CLASSES; ++c) CHECK(boss_battle_text_class(c) && *boss_battle_text_class(c));
        if (!english) {
            CHECK(same_bosses < BOSS_BATTLE_TEXT_BOSSES / 2);
            const ExitGameLanguage *inn = exit_language(key);
            CHECK(inn && strcmp(inn->inn_row, exit_game_languages[0].inn_row) && *inn->inn_help);
        }
        printf("%s: %d keys, %d bosses (%d named as in English), Fight All \"%s\"\n", key, BOSS_BATTLE_TEXT_COUNT,
               BOSS_BATTLE_TEXT_BOSSES, same_bosses, boss_battle_text(BOSS_BATTLE_TEXT_FIGHT_ALL));
        ++languages;
    }
    CHECK(languages == BOSS_BATTLE_TEXT_LANGUAGES);
    for (int i = 0; i < BOSS_BATTLE_TEXT_LANGUAGES; ++i) CHECK(exit_language(boss_battle_text_language_key(i)) != NULL);
    CHECK(boss_battle_text_language_key(BOSS_BATTLE_TEXT_LANGUAGES) == NULL);
}

static void test_fallback_and_kana(void) {
    CHECK(boss_battle_text_set_language("klingon", 0) == 0 && !strcmp(boss_battle_text_language(), "english"));
    CHECK(boss_battle_text_set_language(NULL, 0) == 0 && !strcmp(boss_battle_text(BOSS_BATTLE_TEXT_WINS), "Wins"));
    CHECK(boss_battle_text_set_language("russian", 0) == 1 && has_cyrillic(boss_battle_text(BOSS_BATTLE_TEXT_FIGHT_ALL)));
    CHECK(boss_battle_text_set_language("russian", 1) == 1 && !strcmp(boss_battle_text_language(), "russian"));
    for (int k = 0; k < BOSS_BATTLE_TEXT_COUNT; ++k) CHECK(!has_cyrillic(boss_battle_text((BossBattleTextKey)k)));
    for (int b = 0; b < BOSS_BATTLE_TEXT_BOSSES; ++b) CHECK(!has_cyrillic(boss_battle_text_boss(b)) && *boss_battle_text_boss(b));
    for (int c = 0; c < BOSS_BATTLE_TEXT_CLASSES; ++c) CHECK(!has_cyrillic(boss_battle_text_class(c)));
    CHECK(is_format(boss_battle_text(BOSS_BATTLE_TEXT_CHARACTER)));
    CHECK(boss_battle_text_set_language("english", 1) == 1 && !strcmp(boss_battle_text_boss(0), "Harpy"));
}

int main(void) {
    test_english();
    test_every_language();
    test_fallback_and_kana();
    boss_battle_text_set_language("english", 0);
    if (failures) {
        printf("%d Boss Battle text check(s) failed\n", failures);
        return 1;
    }
    puts("Boss Battle text tests passed");
    return 0;
}
