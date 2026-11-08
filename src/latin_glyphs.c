#include "latin_glyphs.h"

#include <string.h>

#define DESCRIPTOR_HEADER 16
#define PAGE_COUNT 256
#define PAGE_ENTRY 8
#define PAGE_BYTES (PAGE_COUNT * PAGE_ENTRY)

const LatinGlyph latin_glyphs[LATIN_GLYPH_COUNT] = {
    {0xC0, 'A', LATIN_MARK_GRAVE},      {0xC1, 'A', LATIN_MARK_ACUTE},      {0xC2, 'A', LATIN_MARK_CIRCUMFLEX},
    {0xC3, 'A', LATIN_MARK_TILDE},      {0xC7, 'C', LATIN_MARK_CEDILLA},    {0xC9, 'E', LATIN_MARK_ACUTE},
    {0xCA, 'E', LATIN_MARK_CIRCUMFLEX}, {0xCD, 'I', LATIN_MARK_ACUTE},      {0xD3, 'O', LATIN_MARK_ACUTE},
    {0xD4, 'O', LATIN_MARK_CIRCUMFLEX}, {0xD5, 'O', LATIN_MARK_TILDE},      {0xDA, 'U', LATIN_MARK_ACUTE},
    {0xDC, 'U', LATIN_MARK_DIAERESIS},  {0xE0, 'a', LATIN_MARK_GRAVE},      {0xE1, 'a', LATIN_MARK_ACUTE},
    {0xE2, 'a', LATIN_MARK_CIRCUMFLEX}, {0xE3, 'a', LATIN_MARK_TILDE},      {0xE4, 'a', LATIN_MARK_DIAERESIS},
    {0xE7, 'c', LATIN_MARK_CEDILLA},    {0xE8, 'e', LATIN_MARK_GRAVE},      {0xE9, 'e', LATIN_MARK_ACUTE},
    {0xEA, 'e', LATIN_MARK_CIRCUMFLEX}, {0xED, 'i', LATIN_MARK_ACUTE},      {0xF3, 'o', LATIN_MARK_ACUTE},
    {0xF4, 'o', LATIN_MARK_CIRCUMFLEX}, {0xF5, 'o', LATIN_MARK_TILDE},      {0xFA, 'u', LATIN_MARK_ACUTE},
    {0xFC, 'u', LATIN_MARK_DIAERESIS},
};

/* Texture 4096 x 4096, cells 60 x 60: the main font of every supported executable. */
static const uint8_t descriptor_header[DESCRIPTOR_HEADER] = {0, 0, 0, 0, 0, 0x10, 0, 0x10, 1, 0x44, 0x44, 2, 2, 0x3c, 0x3c, 0};

static uint64_t read_u64(const uint8_t *p) {
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

/* A page pointer is an image offset until the loader relocates it, and an address after. */
static const uint8_t *page_at(const uint8_t *image, size_t size, uint64_t value) {
    const uint64_t start = (uint64_t)(uintptr_t)image;
    const uint64_t offset = value >= start ? value - start : value;
    if (!value || size < PAGE_BYTES || offset > size - PAGE_BYTES) return NULL;
    return image + offset;
}

const uint8_t *font_glyph_page(const uint8_t *image, size_t size, const uint8_t *pages, unsigned high) {
    return pages && high < PAGE_COUNT ? page_at(image, size, read_u64(pages + (size_t)high * 8)) : NULL;
}

static int has_glyph(const uint8_t *image, size_t size, const uint8_t *pages, uint16_t code) {
    const uint8_t *page = font_glyph_page(image, size, pages, code >> 8);
    return page && read_u16(page + (size_t)(code & 0xff) * PAGE_ENTRY) != 0;
}

const uint8_t *font_glyph_pages(const uint8_t *image, size_t size, uint16_t probe) {
    const size_t needed = DESCRIPTOR_HEADER + PAGE_COUNT * 8;
    const uint8_t *found = NULL;
    if (!image || size < needed) return NULL;
    for (size_t at = 0; at <= size - needed; at += 8) {
        if (memcmp(image + at, descriptor_header, DESCRIPTOR_HEADER)) continue;
        const uint8_t *pages = image + at + DESCRIPTOR_HEADER;
        if (!has_glyph(image, size, pages, 'a') || !has_glyph(image, size, pages, probe)) continue;
        if (found) return NULL;
        found = pages;
    }
    return found;
}

const uint8_t *latin_glyphs_locate(const uint8_t *image, size_t size, uint16_t probe) {
    return font_glyph_page(image, size, font_glyph_pages(image, size, probe), 0);
}

int latin_glyphs_fit(const uint8_t *image, size_t size, const uint8_t *page) {
    if (!image || !page || size < PAGE_BYTES || page < image || (size_t)(page - image) > size - PAGE_BYTES) return 0;
    for (size_t i = 0; i < LATIN_GLYPH_COUNT; ++i) {
        const LatinGlyph *glyph = &latin_glyphs[i];
        const uint16_t existing = read_u16(page + (size_t)glyph->code * PAGE_ENTRY);
        if (!read_u16(page + (size_t)(uint8_t)glyph->base * PAGE_ENTRY) || (existing && existing < LATIN_FIRST_CELL)) return 0;
    }
    return 1;
}

int latin_glyphs_install(uint8_t *image, size_t size, const uint8_t *page) {
    if (!latin_glyphs_fit(image, size, page)) return 0;
    uint8_t *writable = image + (page - image);
    for (size_t i = 0; i < LATIN_GLYPH_COUNT; ++i) {
        const LatinGlyph *glyph = &latin_glyphs[i];
        uint8_t *slot = writable + (size_t)glyph->code * PAGE_ENTRY;
        const uint16_t cell = (uint16_t)(LATIN_FIRST_CELL + i);
        memcpy(slot, writable + (size_t)(uint8_t)glyph->base * PAGE_ENTRY, PAGE_ENTRY);
        slot[0] = (uint8_t)cell;
        slot[1] = (uint8_t)(cell >> 8);
    }
    return 1;
}
