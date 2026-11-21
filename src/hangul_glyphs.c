#include "hangul_glyphs.h"

#include <string.h>

#include "latin_glyphs.h"

#define PAGE_COUNT 256
#define PAGE_ENTRY 8
#define PAGE_BYTES (256 * PAGE_ENTRY)
#define HANGUL_PAGES (HANGUL_LAST_PAGE - HANGUL_FIRST_PAGE + 1)

/* The game reads a font's pages for as long as it runs. */
static _Alignas(8) uint8_t hangul_pages[HANGUL_PAGES][PAGE_BYTES];

static uint16_t entry_cell(const uint8_t *entry) {
    return (uint16_t)(entry[0] | entry[1] << 8);
}

static int is_hangul(uint16_t code) {
    return code >= 0xAC00 && code <= 0xD7A3;
}

static int is_kanji(unsigned code) {
    return (code >= 0x3400 && code <= 0x9FFF) || (code >= 0xF900 && code <= 0xFAFF);
}

int hangul_glyphs_fit(const uint8_t *image, size_t size, const uint8_t *pages, const HangulGlyph *glyphs, size_t count) {
    if (!image || !pages || !glyphs || !count || count > HANGUL_GLYPH_LIMIT || size < PAGE_COUNT * 8 || pages < image ||
        (size_t)(pages - image) > size - PAGE_COUNT * 8)
        return 0;
    for (unsigned high = HANGUL_FIRST_PAGE; high <= HANGUL_LAST_PAGE; ++high) {
        if (font_glyph_page(image, size, pages, high)) return 0;
    }
    static uint8_t taken[65536];
    memset(taken, 0, sizeof(taken));
    for (size_t i = 0; i < count; ++i) {
        if (!is_hangul(glyphs[i].code) || !glyphs[i].cell || !glyphs[i].width || taken[glyphs[i].cell]) return 0;
        taken[glyphs[i].cell] = 1;
    }
    /* Every entry of the table that names one of the cells must be a kanji's. */
    for (unsigned high = 0; high < PAGE_COUNT; ++high) {
        const uint8_t *page = font_glyph_page(image, size, pages, high);
        if (!page) continue;
        for (unsigned low = 0; low < 256; ++low) {
            const uint16_t cell = entry_cell(page + (size_t)low * PAGE_ENTRY);
            if (cell && taken[cell] && !is_kanji(high << 8 | low)) return 0;
        }
    }
    return 1;
}

int hangul_glyphs_install(uint8_t *image, size_t size, const uint8_t *pages, const HangulGlyph *glyphs, size_t count) {
    if (!hangul_glyphs_fit(image, size, pages, glyphs, count)) return 0;
    memset(hangul_pages, 0, sizeof(hangul_pages));
    int used[HANGUL_PAGES] = {0};
    for (size_t i = 0; i < count; ++i) {
        const unsigned page = (glyphs[i].code >> 8) - HANGUL_FIRST_PAGE;
        uint8_t *slot = hangul_pages[page] + (size_t)(glyphs[i].code & 0xff) * PAGE_ENTRY;
        slot[0] = (uint8_t)glyphs[i].cell;
        slot[1] = (uint8_t)(glyphs[i].cell >> 8);
        slot[2] = glyphs[i].width;
        slot[3] = glyphs[i].spacing;
        used[page] = 1;
    }
    for (unsigned page = 0; page < HANGUL_PAGES; ++page) {
        if (!used[page]) continue;
        const uint64_t address = (uint64_t)(uintptr_t)hangul_pages[page];
        memcpy(image + (pages - image) + (size_t)(HANGUL_FIRST_PAGE + page) * 8, &address, sizeof(address));
    }
    return 1;
}
