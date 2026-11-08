/* The accented letters of Brazilian Portuguese on executables whose main font has none (Japan, Asia/Hong Kong). Their
 * glyph table is completed in the loaded image and launcher/latin_font.cpp draws the letters into free atlas cells. */
#ifndef DCP_LATIN_GLYPHS_H
#define DCP_LATIN_GLYPHS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LATIN_MARK_GRAVE,
    LATIN_MARK_ACUTE,
    LATIN_MARK_CIRCUMFLEX,
    LATIN_MARK_TILDE,
    LATIN_MARK_DIAERESIS,
    LATIN_MARK_CEDILLA
} LatinMark;

typedef struct {
    uint16_t code;
    char base; /* the ASCII letter it is built from, whose metrics it keeps */
    uint8_t mark;
} LatinGlyph;

#define LATIN_GLYPH_COUNT 28
extern const LatinGlyph latin_glyphs[LATIN_GLYPH_COUNT];

/* Atlas cells: 60 columns and 60 rows. Rows 58 and 59 are blank in the Japanese font and the Chinese one. */
#define LATIN_ATLAS_COLUMNS 60
#define LATIN_FIRST_CELL (58 * LATIN_ATLAS_COLUMNS)

/* The 256 code page pointers of the main font holding `probe`, a character only that font has; NULL unless exactly one
 * does. A page is 256 entries of 8 bytes: the atlas cell (u16), then the glyph's width and spacing bytes. */
const uint8_t *font_glyph_pages(const uint8_t *image, size_t size, uint16_t probe);
/* Code page `high` of that font; NULL when the font has none. */
const uint8_t *font_glyph_page(const uint8_t *image, size_t size, const uint8_t *pages, unsigned high);
/* Its first code page. */
const uint8_t *latin_glyphs_locate(const uint8_t *image, size_t size, uint16_t probe);

/* 0 when a base letter is missing or a slot holds a real glyph. The Japanese updates list these letters at cells of the
 * free rows, which their font leaves blank. */
int latin_glyphs_fit(const uint8_t *image, size_t size, const uint8_t *page);

/* Adds every letter to the page latin_glyphs_locate found; 0, with nothing written, when they do not fit. */
int latin_glyphs_install(uint8_t *image, size_t size, const uint8_t *page);

#ifdef __cplusplus
}
#endif

#endif
