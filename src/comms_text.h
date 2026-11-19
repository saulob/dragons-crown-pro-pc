/* Comms Wheel words: the eight messages in the Menu & Subtitles languages. A message travels as an id only; the client
 * that shows it looks the id up here in its own language. The same text labels the wheel and fills the game's message
 * balloon. */
#ifndef DCP_COMMS_TEXT_H
#define DCP_COMMS_TEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { COMMS_TEXT_MESSAGES = 8, COMMS_TEXT_LANGUAGES = 10 };

/* The language of a launcher TextLanguage key; English for any other. `kana_cyrillic`: the font draws Russian over kana
 * (the game's balloon text only). Returns 1 when the key is one of the table's. */
int comms_text_set_language(const char *key, int kana_cyrillic);
const char *comms_text_language(void);

/* UTF-8 in the active language; "" for an id outside 0-7. */
const char *comms_text_message(int id);
/* UTF-8 in a given language; NULL for a language that is not in the table, "" for an id outside 0-7. */
const char *comms_text_lookup(const char *language_key, int id);
/* The active language as the game's balloon draws it: UTF-16 (kana-encoded Russian where the font needs it), at most
 * `capacity - 1` units and a terminating zero. Returns the units written; 0 for an id outside 0-7 or no room. */
size_t comms_text_game_utf16(int id, uint16_t *out, size_t capacity);

/* The table's language keys, for the tests; NULL past the last. */
const char *comms_text_language_key(int index);

#ifdef __cplusplus
}
#endif

#endif
