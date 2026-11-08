#include "cyrillic_glyphs.h"

#include <string.h>

#include "latin_glyphs.h"

#define PAGE_ENTRY 8
#define PAGE_BYTES (256 * PAGE_ENTRY)
#define LETTERS (CYRILLIC_GLYPH_COUNT / 2)
#define CYRILLIC_PAGE 0x04
#define CAPITAL_KANA 0x3060
#define SMALL_KANA 0x30A9
#define CAPITAL_CELL 300
#define SMALL_CELL 360
#define FREE_FIRST_CELL (LATIN_FIRST_CELL + LATIN_GLYPH_COUNT)

/* The fan font's order: the soft sign comes before the hard sign. */
static const uint16_t capitals[LETTERS] = {
    0x410, 0x411, 0x412, 0x413, 0x414, 0x415, 0x401, 0x416, 0x417, 0x418, 0x419, 0x41A, 0x41B, 0x41C, 0x41D, 0x41E, 0x41F,
    0x420, 0x421, 0x422, 0x423, 0x424, 0x425, 0x426, 0x427, 0x428, 0x429, 0x42C, 0x42A, 0x42B, 0x42D, 0x42E, 0x42F,
};

/* The game reads a font's pages for as long as it runs; the Japanese and Chinese fonts have none for these codes. */
static _Alignas(8) uint8_t cyrillic_page[PAGE_BYTES];

uint16_t cyrillic_letter(int index) {
    if (index < 0 || index >= CYRILLIC_GLYPH_COUNT) return 0;
    const uint16_t capital = capitals[index % LETTERS];
    if (index < LETTERS) return capital;
    return capital == 0x401 ? 0x451 : (uint16_t)(capital + 0x20);
}

uint16_t cyrillic_kana(int index) {
    if (index < 0 || index >= CYRILLIC_GLYPH_COUNT) return 0;
    return (uint16_t)(index < LETTERS ? CAPITAL_KANA + index : SMALL_KANA + index - LETTERS);
}

int cyrillic_cell(CyrillicCells cells, int index) {
    if (index < 0 || index >= CYRILLIC_GLYPH_COUNT) return -1;
    if (cells == CYRILLIC_CELLS_KANA) return index < LETTERS ? CAPITAL_CELL + index : SMALL_CELL + index - LETTERS;
    if (cells == CYRILLIC_CELLS_FREE) return FREE_FIRST_CELL + index;
    return -1;
}

static int letter_index(uint32_t code) {
    for (int i = 0; i < CYRILLIC_GLYPH_COUNT; ++i) {
        if (cyrillic_letter(i) == code) return i;
    }
    return -1;
}

size_t cyrillic_kana_encode(const char *text, char *out, size_t size) {
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        unsigned char kana[3];
        const unsigned char *bytes = p;
        size_t length = 1;
        const int index = (p[0] == 0xD0 || p[0] == 0xD1) && (p[1] & 0xC0) == 0x80
                              ? letter_index((uint32_t)(p[0] & 0x1F) << 6 | (uint32_t)(p[1] & 0x3F))
                              : -1;
        if (index >= 0) {
            const uint16_t code = cyrillic_kana(index);
            kana[0] = (unsigned char)(0xE0 | code >> 12);
            kana[1] = (unsigned char)(0x80 | (code >> 6 & 0x3F));
            kana[2] = (unsigned char)(0x80 | (code & 0x3F));
            bytes = kana;
            length = 3;
            p += 2;
        } else {
            ++p;
        }
        if (used + length >= size) return 0;
        memcpy(out + used, bytes, length);
        used += length;
    }
    if (used >= size) return 0;
    out[used] = 0;
    return used;
}

static uint16_t entry_cell(const uint8_t *page, unsigned low) {
    return (uint16_t)(page[low * PAGE_ENTRY] | page[low * PAGE_ENTRY + 1] << 8);
}

static uint64_t page_pointer(const uint8_t *pages, unsigned high) {
    uint64_t value;
    memcpy(&value, pages + (size_t)high * 8, sizeof(value));
    return value;
}

static int kana_fit(const uint8_t *image, size_t size, const uint8_t *pages) {
    const uint8_t *listed = font_glyph_page(image, size, pages, CYRILLIC_PAGE);
    for (int i = 0; i < CYRILLIC_GLYPH_COUNT; ++i) {
        const uint16_t kana = cyrillic_kana(i);
        const uint8_t *page = font_glyph_page(image, size, pages, kana >> 8);
        if (!page || entry_cell(page, kana & 0xff) != cyrillic_cell(CYRILLIC_CELLS_KANA, i)) return 0;
        if (listed && entry_cell(listed, cyrillic_letter(i) & 0xff)) return 0;
    }
    return 1;
}

/* The blank cells may be listed by letters the Japanese updates never draw, but never by ASCII. */
static int free_fit(const uint8_t *image, size_t size, const uint8_t *pages) {
    const uint8_t *ascii = font_glyph_page(image, size, pages, 0);
    if (page_pointer(pages, CYRILLIC_PAGE) || !ascii) return 0;
    for (unsigned code = 0x20; code < 0x7f; ++code) {
        const int cell = entry_cell(ascii, code);
        if (cell >= FREE_FIRST_CELL && cell < FREE_FIRST_CELL + CYRILLIC_GLYPH_COUNT) return 0;
    }
    return 1;
}

int cyrillic_glyphs_fit(const uint8_t *image, size_t size, const uint8_t *pages, CyrillicCells cells) {
    if (!image || !pages || size < PAGE_BYTES || pages < image || (size_t)(pages - image) > size - PAGE_BYTES) return 0;
    if (cells == CYRILLIC_CELLS_KANA) return kana_fit(image, size, pages);
    if (cells == CYRILLIC_CELLS_FREE) return free_fit(image, size, pages);
    return 0;
}

static void set_entry(uint8_t *slot, int cell, CyrillicMetrics metrics) {
    slot[0] = (uint8_t)cell;
    slot[1] = (uint8_t)(cell >> 8);
    slot[2] = metrics.width;
    slot[3] = metrics.spacing;
}

int cyrillic_glyphs_install(uint8_t *image, size_t size, const uint8_t *pages, CyrillicCells cells,
                            const CyrillicMetrics metrics[CYRILLIC_GLYPH_COUNT]) {
    if (!metrics || !cyrillic_glyphs_fit(image, size, pages, cells)) return 0;
    if (cells == CYRILLIC_CELLS_KANA) {
        for (int i = 0; i < CYRILLIC_GLYPH_COUNT; ++i) {
            const uint16_t kana = cyrillic_kana(i);
            const uint8_t *page = font_glyph_page(image, size, pages, kana >> 8);
            set_entry(image + (page - image) + (size_t)(kana & 0xff) * PAGE_ENTRY, cyrillic_cell(cells, i), metrics[i]);
        }
        return 1;
    }
    memset(cyrillic_page, 0, sizeof(cyrillic_page));
    for (int i = 0; i < CYRILLIC_GLYPH_COUNT; ++i) {
        set_entry(cyrillic_page + (size_t)(cyrillic_letter(i) & 0xff) * PAGE_ENTRY, cyrillic_cell(cells, i), metrics[i]);
    }
    const uint64_t address = (uint64_t)(uintptr_t)cyrillic_page;
    memcpy(image + (pages - image) + (size_t)CYRILLIC_PAGE * 8, &address, sizeof(address));
    return 1;
}
