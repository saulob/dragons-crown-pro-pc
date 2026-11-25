/* The Hangul syllables of the port's Korean text (localization/ko). The USA and European fonts have none: the launcher
 * draws them into atlas cells only a kanji uses (launcher/hangul_font.cpp), as the game's official Korean font puts its
 * Hangul where the Japanese one has kanji, and this adds their code pages to the executable's glyph table. */
#ifndef DCP_HANGUL_GLYPHS_H
#define DCP_HANGUL_GLYPHS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The pages a syllable can be on: U+AC00..U+D7A3. */
#define HANGUL_FIRST_PAGE 0xAC
#define HANGUL_LAST_PAGE 0xD7
#define HANGUL_GLYPH_LIMIT 3000

typedef struct {
    uint16_t code;          /* a precomposed syllable, U+AC00..U+D7A3 */
    uint16_t cell;          /* its atlas cell */
    uint8_t width, spacing; /* bytes 2 and 3 of its glyph table entry */
} HangulGlyph;

/* 1 when the glyphs can be added to the main font whose page array is `pages` (font_glyph_pages): the table lists no
 * Hangul yet, and every atlas cell they take is listed by kanji only (U+3400..U+9FFF, U+F900..U+FAFF), never by a
 * character the Korean or English text shows. */
int hangul_glyphs_fit(const uint8_t *image, size_t size, const uint8_t *pages, const HangulGlyph *glyphs, size_t count);
/* Writes their entries in new code pages and points the page array at them; 0, with nothing written, when they do not fit. */
int hangul_glyphs_install(uint8_t *image, size_t size, const uint8_t *pages, const HangulGlyph *glyphs, size_t count);

#ifdef __cplusplus
}
#endif

#endif
