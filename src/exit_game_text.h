/* Exit Game title text. The title menu adds the PlayStation Store record (388) as its last item and shows the help record
 * 394 for it; the loaded sheet is rewritten in memory before the game builds its string table, so the active language
 * picks the text as for every other row. The sheet keeps its size: the bytes the new strings free are given to the
 * neighbouring records (387, 395) as trailing spaces, so no record moves. */
#ifndef DCP_EXIT_GAME_TEXT_H
#define DCP_EXIT_GAME_TEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_GAME_SYSTEM_MSG_COUNT 1611
#define EXIT_GAME_INSTALL_RECORD 387
#define EXIT_GAME_ROW_RECORD 388
#define EXIT_GAME_HELP_RECORD 394
#define EXIT_GAME_STORE_HELP_RECORD 395

typedef struct {
    const char *key; /* the launcher's TextLanguage key */
    const char *language;
    const char *install_original, *help_original; /* records 387 and 394 of this language's SystemMsg */
    const char *row, *help;                        /* the new records 388 and 394 */
    const char *install_unaccented;                /* record 387 where the font could not show its letters */
} ExitGameLanguage;

#define EXIT_GAME_LANGUAGE_COUNT 9
extern const ExitGameLanguage exit_game_languages[EXIT_GAME_LANGUAGE_COUNT];

typedef enum {
    EXIT_GAME_TEXT_PATCHED,
    EXIT_GAME_TEXT_ALREADY,
    EXIT_GAME_TEXT_NOT_SYSTEM_MSG, /* another sheet: left alone */
    EXIT_GAME_TEXT_UNKNOWN,        /* a SystemMsg of a language not in the table: left alone */
    EXIT_GAME_TEXT_NO_ROOM
} ExitGameTextStatus;

/* Rewrites the rows of a loaded UTF-8 SystemMsg sheet (FMSB, `size` bytes) whose strings are a language's originals. */
ExitGameTextStatus exit_game_text_patch(uint8_t *sheet, size_t size, const ExitGameLanguage **language);

const char *exit_game_text_status_text(ExitGameTextStatus status);

#ifdef __cplusplus
}
#endif

#endif
