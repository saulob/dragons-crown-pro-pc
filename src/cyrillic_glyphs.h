/* The Russian letters of the main font, drawn from the fan translation's font (localization/ru/font). The USA and European
 * fonts take them over the kana cells the fan package redraws, and their text names them by those kana; the Japanese and
 * Asia/Hong Kong fonts take them as Unicode letters in blank cells, after the Portuguese ones. */
#ifndef DCP_CYRILLIC_GLYPHS_H
#define DCP_CYRILLIC_GLYPHS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYRILLIC_GLYPH_COUNT 66

typedef enum { CYRILLIC_CELLS_NONE, CYRILLIC_CELLS_KANA, CYRILLIC_CELLS_FREE } CyrillicCells;

typedef struct {
    uint8_t width, spacing; /* bytes 2 and 3 of a glyph table entry, as the fan font sets them */
} CyrillicMetrics;

/* Letter `index` (capitals first, in the fan font's order), its Unicode code and the kana that stands for it. */
uint16_t cyrillic_letter(int index);
uint16_t cyrillic_kana(int index);
/* Its atlas cell under `cells`; -1 for CYRILLIC_CELLS_NONE. */
int cyrillic_cell(CyrillicCells cells, int index);

/* UTF-8 `text` with every Cyrillic letter written as its kana, NUL-terminated; 0 when `size` is too small. */
size_t cyrillic_kana_encode(const char *text, char *out, size_t size);

/* 0 unless every letter can be added to the main font whose page array is `pages` (font_glyph_pages). */
int cyrillic_glyphs_fit(const uint8_t *image, size_t size, const uint8_t *pages, CyrillicCells cells);
/* Writes the letters' entries with `metrics` (one per letter); 0, with nothing written, when they do not fit. */
int cyrillic_glyphs_install(uint8_t *image, size_t size, const uint8_t *pages, CyrillicCells cells,
                            const CyrillicMetrics metrics[CYRILLIC_GLYPH_COUNT]);

#ifdef __cplusplus
}
#endif

#endif
