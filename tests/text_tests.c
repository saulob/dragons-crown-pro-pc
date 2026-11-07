/* Focused tests of the text paths: libSceImeDialog's panel size and libSceFont's system font set, resolved the
 * way the loader binds the game's imports (runtime_resolve with the eboot's NID#library names). Game files only from
 * game/prepared, skipped when absent; no window. */
#include "runtime.h"
#include "dcp_launcher.h"
#include "latin_glyphs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static uintptr_t import(const char *name) {
    uintptr_t function = runtime_resolve(name, 0);
    if (!function) printf("unresolved import %s (%s)\n", name, runtime_import_name(name));
    return function;
}

/* ---- libSceImeDialog ---- */
#define IME_INVALID_ADDRESS ((int32_t)0x80BC0031)
#define IME_INVALID_TYPE ((int32_t)0x80BC0011)
typedef int32_t (ABI *PanelSize)(const void *param, const void *extended, uint32_t *width, uint32_t *height);

/* OrbisImeDialogParam: type @4, option @32 (the game fills the rest before sceImeDialogInit). */
static void ime_param(unsigned char param[120], uint32_t type, uint32_t option) {
    memset(param, 0, 120);
    memcpy(param + 4, &type, 4);
    memcpy(param + 32, &option, 4);
}

static void test_ime_panel_size(void) {
    const char *ime_imports[] = {"NUeBrN7hzf0#libSceImeDialog", "IADmD4tScBY#libSceImeDialog", "x01jxu+vxlc#libSceImeDialog",
                                 "gyTyVn+bXMw#libSceImeDialog", "oBmw4xrmfKs#libSceImeDialog", "CRD+jSErEJQ#libSceImeDialog"};
    for (size_t i = 0; i < sizeof(ime_imports) / sizeof(*ime_imports); ++i) CHECK(import(ime_imports[i]) != 0);
    CHECK(!strcmp(runtime_import_name("CRD+jSErEJQ#libSceImeDialog"), "sceImeDialogGetPanelSizeExtended"));
    PanelSize panel_size = (PanelSize)import("CRD+jSErEJQ#libSceImeDialog");
    if (!panel_size) return;

    unsigned char param[120];
    uint32_t width = 0, height = 0;
    ime_param(param, 0, 0);
    /* The game's call: no extended parameters. */
    CHECK(panel_size(param, NULL, &width, &height) == 0 && width == 0x319 && height == 0x210);
    width = height = 0;
    CHECK(panel_size(param, NULL, &width, &height) == 0 && width == 0x319 && height == 0x210);
    unsigned char extended[168] = {0};
    CHECK(panel_size(param, extended, &width, &height) == 0 && width == 0x319 && height == 0x210);
    ime_param(param, 0, 1); /* multiline */
    CHECK(panel_size(param, NULL, &width, &height) == 0 && width == 0x319 && height == 0x274);
    ime_param(param, 4, 0); /* number */
    CHECK(panel_size(param, NULL, &width, &height) == 0 && width == 0x172 && height == 0x20a);
    ime_param(param, 1, 0x4000); /* basic Latin, 4K coordinates */
    CHECK(panel_size(param, NULL, &width, &height) == 0 && width == 0x319 * 2 && height == 0x210 * 2);

    ime_param(param, 5, 0);
    CHECK(panel_size(param, NULL, &width, &height) == IME_INVALID_TYPE);
    ime_param(param, 0, 0);
    CHECK(panel_size(NULL, NULL, &width, &height) == IME_INVALID_ADDRESS);
    CHECK(panel_size(param, NULL, NULL, &height) == IME_INVALID_ADDRESS);
    CHECK(panel_size(param, NULL, &width, NULL) == IME_INVALID_ADDRESS);
}

/* ---- libSceFont ---- */
#define FONT_OK 0
#define ERR_INVALID_LIBRARY ((int32_t)0x80460004)
#define ERR_INVALID_FONT_HANDLE ((int32_t)0x80460005)
#define ERR_FONT_OPEN_FAILED ((int32_t)0x80460036)
#define ERR_NO_SUPPORT_GLYPH ((int32_t)0x80460042)
#define ERR_NO_SUPPORT_SURFACE ((int32_t)0x80460050)
#define FONTSET_SST_STD_EUROPEAN_JPCJK 0x1807A444u /* the set the US and Japanese builds open */
#define FONTSET_SST_STD_SCHINESE_GBCJK 0x180CAC44u
#define FONTSET_SST_STD_JAPANESE_JP 0x18080444u

typedef struct { float width, height, h_bearing_x, h_bearing_y, h_advance, v_bearing_x, v_bearing_y, v_advance; } GlyphMetrics;
typedef struct { float a, b, c; } Layout;
typedef struct { uint16_t tag, pad; uint32_t flags, subfont_index; int32_t unique_id; const void *reserved[2]; } OpenDetail;
typedef struct {
    const void *stage;
    struct { uint8_t *address; uint32_t width_byte; uint8_t pixel_size_byte, pixel_format; uint16_t pad; } image;
    struct { uint32_t x, y, w, h; } update;
    struct { float bearing_x, bearing_y, advance, stride; uint32_t width, height; } metrics;
} RenderOutput;

static struct {
    int32_t (ABI *memory_init)(void *memory, void *region, uint32_t size, const void *interface, void *mspace, void *destroy, void *object);
    int32_t (ABI *memory_term)(void *memory);
    const void *(ABI *select_library)(int32_t value);
    const void *(ABI *select_renderer)(int32_t value);
    int32_t (ABI *create_library)(const void *memory, const void *selection, uint64_t edition, void **out);
    int32_t (ABI *destroy_library)(void **library);
    int32_t (ABI *create_renderer)(const void *memory, const void *selection, uint64_t edition, void **out);
    int32_t (ABI *destroy_renderer)(void **renderer);
    int32_t (ABI *open_set)(void *library, uint32_t set, uint32_t mode, const OpenDetail *detail, void **out);
    int32_t (ABI *open_memory)(void *library, const void *data, uint32_t size, const OpenDetail *detail, void **out);
    int32_t (ABI *close)(void *font);
    int32_t (ABI *bind)(void *font, void *renderer);
    int32_t (ABI *unbind)(void *font);
    int32_t (ABI *set_scale)(void *font, float w, float h);
    int32_t (ABI *setup_render_scale)(void *font, float w, float h);
    int32_t (ABI *horizontal_layout)(void *font, Layout *layout);
    int32_t (ABI *vertical_layout)(void *font, Layout *layout);
    void (ABI *surface_init)(void *surface, void *buffer, int32_t width_byte, int32_t pixel_size, int32_t width, int32_t height);
    int32_t (ABI *render)(void *font, uint32_t code, void *surface, float x, float y, GlyphMetrics *metrics, RenderOutput *out);
} font;

static int resolve_font(void) {
    struct { void *slot; const char *name; } imports[] = {
        {&font.memory_init, "whrS4oksXc4#libSceFont"}, {&font.memory_term, "h6hIgxXEiEc#libSceFont"},
        {&font.select_library, "oM+XCzVG3oM#libSceFontFt"}, {&font.select_renderer, "Xx974EW-QFY#libSceFontFt"},
        {&font.create_library, "n590hj5Oe-k#libSceFont"}, {&font.destroy_library, "FXP359ygujs#libSceFont"},
        {&font.create_renderer, "WaSFJoRWXaI#libSceFont"}, {&font.destroy_renderer, "exAxkyVLt0s#libSceFont"},
        {&font.open_set, "cKYtVmeSTcw#libSceFont"}, {&font.open_memory, "KXUpebrFk1U#libSceFont"},
        {&font.close, "vzHs3C8lWJk#libSceFont"}, {&font.bind, "3OdRkSjOcog#libSceFont"},
        {&font.unbind, "1QjhKxrsOB8#libSceFont"}, {&font.set_scale, "N1EBMeGhf7E#libSceFont"},
        {&font.setup_render_scale, "6vGCkkQJOcI#libSceFont"}, {&font.horizontal_layout, "imxVx8lm+KM#libSceFont"},
        {&font.vertical_layout, "3BrWWFU+4ts#libSceFont"}, {&font.surface_init, "gdUCnU0gHdI#libSceFont"},
        {&font.render, "3G4zhgKuxE8#libSceFont"},
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(imports) / sizeof(*imports); ++i) {
        uintptr_t function = import(imports[i].name);
        CHECK(function != 0);
        if (!function) ok = 0;
        memcpy(imports[i].slot, &function, sizeof(function));
    }
    return ok;
}

enum { SURFACE = 256 };
static unsigned char surface[128], pixels[SURFACE * SURFACE];

/* Renders `code` at a fixed pen position on a cleared surface; returns the result and the inked pixel count. */
static int32_t render(void *handle, uint32_t code, int *inked) {
    GlyphMetrics metrics;
    RenderOutput out;
    memset(pixels, 0, sizeof(pixels));
    int32_t result = font.render(handle, code, surface, 16.0f, 64.0f, &metrics, &out);
    *inked = 0;
    for (size_t i = 0; i < sizeof(pixels); ++i) *inked += pixels[i] != 0;
    if (result == FONT_OK && (out.metrics.width == 0 || metrics.h_advance <= 0.0f)) return -1;
    return result;
}

/* The game sizes a glyph cell (size+2) square and passes sceFontRenderCharGlyphImage the cell's top-left
 * plus one pixel as the pen, with the render scale ceil(size * size / lineAdvance): the whole glyph,
 * accents and descenders included, must land inside its own cell. */
static int ink_in_cell(void *handle, int size, uint32_t code, int *top_row, int *bottom_row) {
    Layout layout = {0};
    if (font.set_scale(handle, (float)size, (float)size) || font.horizontal_layout(handle, &layout)) return 0;
    float render = ceilf(size * size / layout.b);
    if (font.setup_render_scale(handle, render, render)) return 0;
    const int cell = size + 2, row = 2, column = 2;
    font.surface_init(surface, pixels, SURFACE, 1, SURFACE, SURFACE);
    memset(pixels, 0, sizeof(pixels));
    GlyphMetrics metrics;
    RenderOutput out;
    if (font.render(handle, code, surface, (float)(column * cell + 1), (float)(row * cell + 1), &metrics, &out)) return 0;
    int first = -1, last = -1, inside = 1;
    for (int y = 0; y < SURFACE; ++y) {
        for (int x = 0; x < SURFACE; ++x) {
            if (!pixels[y * SURFACE + x]) continue;
            if (y < row * cell || y >= (row + 1) * cell || x < column * cell || x >= (column + 1) * cell) inside = 0;
            if (first < 0) first = y;
            last = y;
        }
    }
    *top_row = first - row * cell;
    *bottom_row = last - row * cell;
    return first >= 0 && inside;
}

static void test_glyph_cell(void *set) {
    const int sizes[] = {20, 28, 40};
    const struct { uint32_t code; int descends; } glyphs[] = {{'A', 0}, {'H', 0}, {'g', 1}, {0xE9, 0}, {0xC9, 0}, {0x416, 0}, {'?', 0}};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); ++s) {
        for (size_t g = 0; g < sizeof(glyphs) / sizeof(*glyphs); ++g) {
            int top = 0, bottom = 0;
            int fits = ink_in_cell(set, sizes[s], glyphs[g].code, &top, &bottom);
            CHECK(fits);
            if (!fits) { printf("  size %d U+%04X outside its cell\n", sizes[s], (unsigned)glyphs[g].code); continue; }
            /* Capitals sit on the baseline, in the lower part of the cell; only descenders pass it. */
            if (!glyphs[g].descends && glyphs[g].code < 0x300) CHECK(bottom > sizes[s] / 2);
            CHECK(top >= 0 && bottom < sizes[s] + 2);
        }
    }
}

enum { WINDOW = 96 };

static void count_ink(int x0, int y0, int x1, int y1, int *inside, int *outside) {
    *inside = *outside = 0;
    for (int y = 0; y < SURFACE; ++y)
        for (int x = 0; x < SURFACE; ++x)
            if (pixels[y * SURFACE + x]) ++*(x >= x0 && x < x1 && y >= y0 && y < y1 ? inside : outside);
}

static int output_in_surface(const RenderOutput *out, const unsigned char *buffer, int width_byte, int bpp, int width, int height) {
    return out->update.x + out->update.w <= (uint32_t)width && out->update.y + out->update.h <= (uint32_t)height &&
           out->image.address == buffer + out->update.y * width_byte + out->update.x * bpp;
}

/* SceFontRenderSurface scissor rectangle @24. */
static void set_scissor(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    const uint32_t rect[4] = {x0, y0, x1, y1};
    memcpy(surface + 24, rect, sizeof(rect));
}

/* Glyphs crossing the surface edges are clipped to it. The surface is a window of the zeroed canvas with the
 * canvas stride, so a write outside the window stays in the canvas and is counted. */
static void test_surface_bounds(void *set) {
    enum { SIZE = 48 };
    unsigned char *window = pixels + WINDOW * SURFACE + WINDOW;
    GlyphMetrics metrics;
    RenderOutput out;
    int inside, outside;
    CHECK(font.setup_render_scale(set, 24.0f, 24.0f) == FONT_OK);

    /* Pens (top of the line): inside, at the top-left corner, partly past each edge, then far outside. */
    const struct { float x, y; int visible; } pens[] = {
        {12, 8, 1}, {0, 0, 1}, {-10, 8, 1}, {12, -14, 1}, {36, 8, 1}, {12, 36, 1}, {-1000, -1000, 0}, {1000, 1000, 0}, {12, 1e5f, 0}};
    for (size_t i = 0; i < sizeof(pens) / sizeof(*pens); ++i) {
        memset(pixels, 0, sizeof(pixels));
        font.surface_init(surface, window, SURFACE, 1, SIZE, SIZE);
        int32_t result = font.render(set, 'W', surface, pens[i].x, pens[i].y, &metrics, &out);
        count_ink(WINDOW, WINDOW, WINDOW + SIZE, WINDOW + SIZE, &inside, &outside);
        int clipped = result == FONT_OK && !outside && (inside > 0) == pens[i].visible &&
                      output_in_surface(&out, window, SURFACE, 1, SIZE, SIZE);
        CHECK(clipped);
        if (!clipped)
            printf("  pen %g,%g: result %#x, %d pixels inside, %d outside\n", pens[i].x, pens[i].y, (unsigned)result, inside, outside);
    }

    /* The scissor narrows the surface; one past the surface is clamped to it, an inverted one draws nothing. */
    memset(pixels, 0, sizeof(pixels));
    font.surface_init(surface, window, SURFACE, 1, SIZE, SIZE);
    set_scissor(8, 8, 20, 20);
    CHECK(font.render(set, 'W', surface, 4, 0, &metrics, &out) == FONT_OK);
    count_ink(WINDOW + 8, WINDOW + 8, WINDOW + 20, WINDOW + 20, &inside, &outside);
    CHECK(inside > 0 && !outside && output_in_surface(&out, window, SURFACE, 1, SIZE, SIZE));
    memset(pixels, 0, sizeof(pixels));
    set_scissor(0, 0, 1000, 1000);
    CHECK(font.render(set, 'W', surface, 36, 36, &metrics, &out) == FONT_OK);
    count_ink(WINDOW, WINDOW, WINDOW + SIZE, WINDOW + SIZE, &inside, &outside);
    CHECK(inside > 0 && !outside && output_in_surface(&out, window, SURFACE, 1, SIZE, SIZE));
    memset(pixels, 0, sizeof(pixels));
    set_scissor(30, 30, 10, 10);
    CHECK(font.render(set, 'W', surface, 4, 0, &metrics, &out) == FONT_OK);
    count_ink(0, 0, SURFACE, SURFACE, &inside, &outside);
    CHECK(!inside && out.update.w == 0 && out.update.h == 0);

    /* A row narrower than width x pixel size: only what the row holds is written, as if the surface were that wide. */
    static unsigned char narrow[16 * 4 * 32];
    for (int bpp = 1; bpp <= 4; bpp += 3) {
        const int stride = 16 * bpp, height = 32, bytes = stride * height;
        memset(pixels, 0, sizeof(pixels));
        font.surface_init(surface, pixels + SURFACE, stride, bpp, 48, height);
        CHECK(font.render(set, 'W', surface, 4, 0, &metrics, &out) == FONT_OK);
        count_ink(0, 1, SURFACE, 1 + bytes / SURFACE, &inside, &outside);
        CHECK(inside > 0 && !outside && output_in_surface(&out, pixels + SURFACE, stride, bpp, 16, height));
        memcpy(narrow, pixels + SURFACE, (size_t)bytes);
        memset(pixels, 0, sizeof(pixels));
        font.surface_init(surface, pixels + SURFACE, stride, bpp, 16, height);
        CHECK(font.render(set, 'W', surface, 4, 0, &metrics, &out) == FONT_OK);
        CHECK(!memcmp(narrow, pixels + SURFACE, (size_t)bytes));
    }

    /* Empty or unsupported surfaces are refused untouched. */
    const struct { int width_byte, bpp, width, height; } invalid[] = {
        {SURFACE, 1, 0, SIZE}, {SURFACE, 1, SIZE, 0}, {0, 1, SIZE, SIZE},
        {-SURFACE, 1, SIZE, SIZE}, {SURFACE, 2, SIZE, SIZE}, {SURFACE, 1, -SIZE, SIZE}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        memset(pixels, 0, sizeof(pixels));
        font.surface_init(surface, window, invalid[i].width_byte, invalid[i].bpp, invalid[i].width, invalid[i].height);
        CHECK(font.render(set, 'W', surface, 12, 8, &metrics, &out) == ERR_NO_SUPPORT_SURFACE);
        count_ink(0, 0, SURFACE, SURFACE, &inside, &outside);
        CHECK(!inside);
    }
    font.surface_init(surface, NULL, SURFACE, 1, SIZE, SIZE);
    CHECK(font.render(set, 'W', surface, 12, 8, &metrics, &out) == ERR_NO_SUPPORT_SURFACE);

    /* A blank glyph renders nothing and reports an empty update. */
    memset(pixels, 0, sizeof(pixels));
    font.surface_init(surface, window, SURFACE, 1, SIZE, SIZE);
    CHECK(font.render(set, ' ', surface, 12, 8, &metrics, &out) == FONT_OK && metrics.h_advance > 0.0f);
    count_ink(0, 0, SURFACE, SURFACE, &inside, &outside);
    CHECK(!inside && out.update.w == 0 && out.update.h == 0 && output_in_surface(&out, window, SURFACE, 1, SIZE, SIZE));

    font.surface_init(surface, pixels, SURFACE, 1, SURFACE, SURFACE);
}

/* The game's sequence (eboot 0x80e28..0x810a6): open the set with mode 2 and an open detail
 * of tag 0xfd2 / unique id 1, bind a renderer, size it from both layouts, render into an 8-bit atlas. */
static void test_font_set(void) {
    if (!resolve_font()) return;
    static unsigned char region[4096];
    unsigned char memory[64];
    void *library = NULL, *renderer = NULL, *set = NULL;
    CHECK(font.memory_init(memory, region, sizeof(region), NULL, NULL, NULL, NULL) == FONT_OK);
    CHECK(font.create_library(memory, font.select_library(0), 0, &library) == FONT_OK && library);
    CHECK(font.create_renderer(memory, font.select_renderer(0), 0, &renderer) == FONT_OK && renderer);
    const OpenDetail detail = {.tag = 0xfd2, .unique_id = 1};

    CHECK(font.open_set(library, FONTSET_SST_STD_EUROPEAN_JPCJK, 2, &detail, &set) == FONT_OK && set);
    if (!set) return;
    CHECK(font.bind(set, renderer) == FONT_OK);
    CHECK(font.set_scale(set, 24.0f, 24.0f) == FONT_OK);
    Layout horizontal = {0}, vertical = {0};
    CHECK(font.horizontal_layout(set, &horizontal) == FONT_OK && horizontal.a > 0.0f && horizontal.b > horizontal.a);
    CHECK(font.vertical_layout(set, &vertical) == FONT_OK && vertical.b > 0.0f);
    CHECK(font.setup_render_scale(set, 24.0f * 24.0f / horizontal.b, 24.0f * 24.0f / horizontal.b) == FONT_OK);
    font.surface_init(surface, pixels, SURFACE, 1, SURFACE, SURFACE);

    int inked = 0;
    CHECK(render(set, 'A', &inked) == FONT_OK && inked > 0);
    const uint32_t covered[] = {'!', '?', ',', 0x2026 /* … */, 0xE9 /* é */, 0xDF /* ß */, 0x416 /* Ж */};
    for (size_t i = 0; i < sizeof(covered) / sizeof(*covered); ++i) {
        int drawn = render(set, covered[i], &inked) == FONT_OK && inked > 0;
        CHECK(drawn);
        if (!drawn) printf("  code U+%04X\n", (unsigned)covered[i]);
    }
    test_glyph_cell(set);
    test_surface_bounds(set);
    /* Kana are the game font's: the set reports them missing instead of drawing a box. */
    CHECK(render(set, 0x3042, &inked) == ERR_NO_SUPPORT_GLYPH && inked == 0);

    /* Opening and closing repeatedly keeps the shared embedded data usable. */
    for (int i = 0; i < 8; ++i) {
        void *other = NULL;
        CHECK(font.open_set(library, FONTSET_SST_STD_EUROPEAN_JPCJK, 2, &detail, &other) == FONT_OK && other);
        if (other) CHECK(font.close(other) == FONT_OK);
    }
    CHECK(render(set, 'A', &inked) == FONT_OK && inked > 0);

    const uint32_t chinese[] = {0x89D2 /* 角 */, 0x8272 /* 色 */, 0x5275 /* 創 */, 0x5EFA /* 建 */, 0x540D /* 名 */, 0x7A31 /* 稱 */};
    CHECK(render(set, chinese[0], &inked) == ERR_NO_SUPPORT_GLYPH && inked == 0);
    void *chinese_set = NULL;
    CHECK(font.open_set(library, FONTSET_SST_STD_SCHINESE_GBCJK, 2, &detail, &chinese_set) == FONT_OK && chinese_set);
    if (chinese_set) {
        CHECK(font.bind(chinese_set, renderer) == FONT_OK);
        CHECK(font.set_scale(chinese_set, 24.0f, 24.0f) == FONT_OK);
        Layout chinese_layout = {0};
        CHECK(font.horizontal_layout(chinese_set, &chinese_layout) == FONT_OK && chinese_layout.b > chinese_layout.a);
        CHECK(font.setup_render_scale(chinese_set, 24.0f * 24.0f / chinese_layout.b, 24.0f * 24.0f / chinese_layout.b) == FONT_OK);
        font.surface_init(surface, pixels, SURFACE, 1, SURFACE, SURFACE);
        CHECK(render(chinese_set, 'A', &inked) == FONT_OK && inked > 0);
        for (size_t i = 0; i < sizeof(chinese) / sizeof(*chinese); ++i) {
            GlyphMetrics metrics;
            RenderOutput out;
            memset(pixels, 0, sizeof(pixels));
            int drawn = font.render(chinese_set, chinese[i], surface, 16.0f, 64.0f, &metrics, &out) == FONT_OK &&
                        out.update.w > 0 && out.update.h > 0 && metrics.h_advance > 0.0f;
            int ink = 0;
            for (size_t j = 0; j < sizeof(pixels); ++j) ink += pixels[j] != 0;
            CHECK(drawn && ink > 0);
        }
        CHECK(render(chinese_set, 0xE000 /* private use: unmapped */, &inked) == ERR_NO_SUPPORT_GLYPH && inked == 0);
        CHECK(font.unbind(chinese_set) == FONT_OK);
        CHECK(font.close(chinese_set) == FONT_OK);
    }

    void *missing = (void *)1;
    CHECK(font.open_set(library, FONTSET_SST_STD_JAPANESE_JP, 2, &detail, &missing) == ERR_FONT_OPEN_FAILED && !missing);
    missing = (void *)1;
    CHECK(font.open_set(NULL, FONTSET_SST_STD_EUROPEAN_JPCJK, 2, &detail, &missing) == ERR_INVALID_LIBRARY && !missing);

    /* A library destroyed with fonts open stays alive until the last font closes. */
    void *second = NULL;
    CHECK(font.open_set(library, FONTSET_SST_STD_EUROPEAN_JPCJK, 2, NULL, &second) == FONT_OK && second);
    CHECK(font.destroy_library(&library) == FONT_OK && !library);
    CHECK(render(set, 'B', &inked) == FONT_OK && inked > 0);
    CHECK(font.unbind(set) == FONT_OK);
    CHECK(font.close(set) == FONT_OK);
    if (second) CHECK(font.close(second) == FONT_OK);
    CHECK(font.close(NULL) == ERR_INVALID_FONT_HANDLE);
    CHECK(font.destroy_renderer(&renderer) == FONT_OK);
    CHECK(font.memory_term(memory) == FONT_OK);
}

/* sceFontOpenFontMemory keeps working on guest-owned font data (the game opens its own font this way). */
static void test_font_memory(const char *path) {
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    if (!file) return;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)size);
    CHECK(data && fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);

    static unsigned char region[4096];
    unsigned char memory[64];
    void *library = NULL, *renderer = NULL, *handle = NULL;
    CHECK(font.memory_init(memory, region, sizeof(region), NULL, NULL, NULL, NULL) == FONT_OK);
    CHECK(font.create_library(memory, font.select_library(0), 0, &library) == FONT_OK);
    CHECK(font.create_renderer(memory, font.select_renderer(0), 0, &renderer) == FONT_OK);
    CHECK(font.open_memory(library, data, (uint32_t)size, NULL, &handle) == FONT_OK && handle);
    if (handle) {
        int inked = 0;
        CHECK(font.bind(handle, renderer) == FONT_OK);
        CHECK(font.setup_render_scale(handle, 24.0f, 24.0f) == FONT_OK);
        font.surface_init(surface, pixels, SURFACE, 1, SURFACE, SURFACE);
        CHECK(render(handle, 'A', &inked) == FONT_OK && inked > 0);
        CHECK(font.close(handle) == FONT_OK);
    }
    CHECK(font.destroy_library(&library) == FONT_OK);
    CHECK(font.destroy_renderer(&renderer) == FONT_OK);
    free(data);
}

/* ---- PT-BR letters composed into the atlas, drawn by libSceFont ---- */
unsigned char *test_archive_entry(const char *archive, const char *entry, size_t *size);
const DcpComposedGlyph *test_compose_latin_font(unsigned char *font, size_t size, const unsigned char *marks, size_t marks_size,
                                                size_t *count, int *cell_pixels);
int test_atlas_cell(const unsigned char *font, size_t size, int cell, unsigned char *alpha);

static uint32_t fnv1a(const unsigned char *data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

typedef struct { int x0, y0, x1, y1, inked; } Ink; /* half-open, surface pixels */

static Ink ink_of(void) {
    Ink ink = {SURFACE, SURFACE, 0, 0, 0};
    for (int y = 0; y < SURFACE; ++y)
        for (int x = 0; x < SURFACE; ++x)
            if (pixels[y * SURFACE + x] > 40) {
                ink.inked = 1;
                if (x < ink.x0) ink.x0 = x;
                if (y < ink.y0) ink.y0 = y;
                if (x + 1 > ink.x1) ink.x1 = x + 1;
                if (y + 1 > ink.y1) ink.y1 = y + 1;
            }
    return ink;
}

/* The Asia/Hong Kong system font manager (eboot 0x80970 / 0x80ac0): the game font at h*h/lineAdvance*0.75 with its pen
 * 13 pixels into the cell, then the font set at h*h/lineAdvance with its pen 1 pixel in. */
typedef struct { void *game, *set; } FontPair;
enum { CELL_LEFT = 16, CELL_TOP = 32 };

static int size_like_the_game(void *handle, int size, float factor) {
    Layout layout = {0};
    if (font.set_scale(handle, (float)size, (float)size) || font.horizontal_layout(handle, &layout)) return 0;
    float render = (float)(uint32_t)((float)size * ((float)size / layout.b * factor) + 0.99999f);
    return font.setup_render_scale(handle, render, render) == FONT_OK;
}

/* Renders `code` the way the game picks its font; 1 for the game font, 2 for the set, 0 when neither draws it. */
static int render_like_the_game(const FontPair *fonts, uint32_t code, Ink *ink, float *advance) {
    GlyphMetrics metrics;
    RenderOutput out;
    memset(pixels, 0, sizeof(pixels));
    int used = 0;
    if (font.render(fonts->game, code, surface, CELL_LEFT + 1, CELL_TOP + 13, &metrics, &out) == FONT_OK) used = 1;
    else {
        memset(pixels, 0, sizeof(pixels));
        if (font.render(fonts->set, code, surface, CELL_LEFT + 1, CELL_TOP + 1, &metrics, &out) == FONT_OK) used = 2;
    }
    *ink = ink_of();
    *advance = used ? out.metrics.advance : 0.0f;
    return used;
}

static size_t utf8_codes(const char *text, uint32_t *codes, size_t capacity) {
    size_t count = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && count < capacity;) {
        uint32_t code = *p++;
        if (code >= 0xC0 && code < 0xE0 && *p) code = (code & 0x1F) << 6 | (*p++ & 0x3F);
        codes[count++] = code;
    }
    return count;
}

static const DcpComposedGlyph *composed_entry(const DcpComposedGlyph *glyphs, size_t count, uint32_t code) {
    for (size_t i = 0; i < count; ++i) if (glyphs[i].code == code) return &glyphs[i];
    return NULL;
}

static int band_width(int y0, int y1) {
    int x0 = SURFACE, x1 = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < SURFACE; ++x)
            if (pixels[y * SURFACE + x] > 40) {
                if (x < x0) x0 = x;
                if (x + 1 > x1) x1 = x + 1;
            }
    return x1 > x0 ? x1 - x0 : 0;
}

/* An accented letter keeps its base letter's scale: the same width over the half of the base away from the mark, the
 * same baseline (the cedilla: the same top) and the same advance. */
static int same_scale_as_base(const FontPair *fonts, uint32_t code, uint32_t base, int *used) {
    Ink letter, plain;
    float letter_advance, plain_advance;
    const int cedilla = code == 0xE7 || code == 0xC7;
    if (!render_like_the_game(fonts, base, &plain, &plain_advance) || !plain.inked) return 0;
    const int half = (plain.y1 - plain.y0) / 2;
    const int band0 = cedilla ? plain.y0 : plain.y1 - half, band1 = cedilla ? plain.y0 + half : plain.y1;
    const int base_width = band_width(band0, band1);
    *used = render_like_the_game(fonts, code, &letter, &letter_advance);
    if (*used != 1 || !letter.inked) return 0;
    const int aligned = cedilla ? abs(letter.y0 - plain.y0) <= 1 : abs(letter.y1 - plain.y1) <= 1;
    return abs(band_width(band0, band1) - base_width) <= 2 && aligned && letter_advance == plain_advance;
}

static void test_composed_letters(void) {
    size_t font_size = 0, marks_size = 0, song_size = 0, skip_size = 0;
    unsigned char *atlas = test_archive_entry("CUSA10209-1.00/DRACRO_CK.CPK", "other/CT_fontUTF16.ftx", &font_size);
    unsigned char *marks = test_archive_entry("CUSA10209-1.00/DRACRO.CPK", "other/fontUTF16.ftx", &marks_size);
    unsigned char *song = test_archive_entry("CUSA10209-1.00/DRACRO_CK.CPK", "other/FOTC-arstdsongb5heavy.OTF", &song_size);
    unsigned char *skip = test_archive_entry("CUSA10209-1.00/DRACRO.CPK", "other/FOT-SKIPSTD-B.OTF", &skip_size);
    if (!atlas || !marks || !song || !skip) {
        printf("  composed letters: no local Asia/Hong Kong 1.00 archives, skipped\n");
        free(atlas), free(marks), free(song), free(skip);
        return;
    }
    size_t count = 0;
    int cell = 0;
    const DcpComposedGlyph *glyphs = test_compose_latin_font(atlas, font_size, marks, marks_size, &count, &cell);
    CHECK(glyphs && count == LATIN_GLYPH_COUNT && cell == 68);
    if (!glyphs || cell != 68) goto done;

    /* Both consumers read one cell: the atlas the main font samples and the cell libSceFont is given. */
    static unsigned char cell_pixels[68 * 68];
    const uint32_t sampled[] = {0xF3, 0xE3, 0xE7};
    for (size_t i = 0; i < sizeof(sampled) / sizeof(*sampled); ++i) {
        const DcpComposedGlyph *glyph = composed_entry(glyphs, count, sampled[i]);
        int index = -1;
        for (int g = 0; g < LATIN_GLYPH_COUNT; ++g) if (latin_glyphs[g].code == sampled[i]) index = g;
        CHECK(glyph && index >= 0 && test_atlas_cell(atlas, font_size, LATIN_FIRST_CELL + index, cell_pixels));
        if (!glyph || index < 0) continue;
        const uint32_t atlas_hash = fnv1a(cell_pixels, sizeof(cell_pixels)), served_hash = fnv1a(glyph->alpha, sizeof(cell_pixels));
        CHECK(atlas_hash == served_hash);
        printf("  COMPOSED-CELL|U+%04X|cell %d|atlas %08x|libSceFont %08x\n", (unsigned)sampled[i], LATIN_FIRST_CELL + index,
               (unsigned)atlas_hash, (unsigned)served_hash);
    }

    static unsigned char region[4096];
    unsigned char memory[64];
    void *library = NULL, *renderer = NULL, *native = NULL;
    FontPair fonts = {NULL, NULL};
    const OpenDetail detail = {.tag = 0xfd2, .unique_id = 1};
    CHECK(font.memory_init(memory, region, sizeof(region), NULL, NULL, NULL, NULL) == FONT_OK);
    CHECK(font.create_library(memory, font.select_library(0), 0, &library) == FONT_OK);
    CHECK(font.create_renderer(memory, font.select_renderer(0), 0, &renderer) == FONT_OK);
    CHECK(font.open_memory(library, song, (uint32_t)song_size, &detail, &fonts.game) == FONT_OK && fonts.game);
    CHECK(font.open_set(library, FONTSET_SST_STD_SCHINESE_GBCJK, 2, &detail, &fonts.set) == FONT_OK && fonts.set);
    CHECK(font.open_memory(library, skip, (uint32_t)skip_size, &detail, &native) == FONT_OK && native);
    if (!fonts.game || !fonts.set || !native) goto close;
    CHECK(font.bind(fonts.game, renderer) == FONT_OK && font.bind(fonts.set, renderer) == FONT_OK &&
          font.bind(native, renderer) == FONT_OK);
    font.surface_init(surface, pixels, SURFACE, 1, SURFACE, SURFACE);

    const int sizes[] = {24, 32, 40};
    uint32_t before_set[3], before_native[3];
    for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); ++s) {
        CHECK(size_like_the_game(fonts.game, sizes[s], 0.75f) && size_like_the_game(fonts.set, sizes[s], 1.0f) &&
              size_like_the_game(native, sizes[s], 1.0f));
        /* Without the composed cells the game font has none of the letters: the set draws them at its own scale. */
        Ink letter, plain;
        float advance;
        int used = render_like_the_game(&fonts, 0xE3, &letter, &advance);
        render_like_the_game(&fonts, 'a', &plain, &advance);
        CHECK(used == 2);
        printf("  size %d without the cells: U+00E3 from the font set, ink %dx%d against a %dx%d\n", sizes[s],
               letter.x1 - letter.x0, letter.y1 - letter.y0, plain.x1 - plain.x0, plain.y1 - plain.y0);
        GlyphMetrics metrics;
        RenderOutput out;
        memset(pixels, 0, sizeof(pixels));
        CHECK(font.render(fonts.set, 0xE3, surface, 17, 33, &metrics, &out) == FONT_OK);
        before_set[s] = fnv1a(pixels, sizeof(pixels));
        memset(pixels, 0, sizeof(pixels));
        CHECK(font.render(native, 0xE3, surface, 17, 45, &metrics, &out) == FONT_OK);
        before_native[s] = fnv1a(pixels, sizeof(pixels));
    }

    RuntimeComposedGlyph shared[LATIN_GLYPH_COUNT];
    for (size_t i = 0; i < count; ++i) shared[i] = (RuntimeComposedGlyph){glyphs[i].code, glyphs[i].base, glyphs[i].alpha, glyphs[i].base_alpha};
    runtime_font_set_composed_glyphs(shared, count, cell);

    for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); ++s) {
        CHECK(size_like_the_game(fonts.game, sizes[s], 0.75f) && size_like_the_game(fonts.set, sizes[s], 1.0f) &&
              size_like_the_game(native, sizes[s], 1.0f));
        for (size_t i = 0; i < count; ++i) {
            int used = 0;
            const int same = same_scale_as_base(&fonts, glyphs[i].code, glyphs[i].base, &used);
            CHECK(same);
            if (!same) printf("  size %d U+%04X: font %d, not at its base letter's scale\n", sizes[s], (unsigned)glyphs[i].code, used);
        }
        /* System font sets and fonts that have the letters draw them as before. */
        GlyphMetrics metrics;
        RenderOutput out;
        memset(pixels, 0, sizeof(pixels));
        CHECK(font.render(fonts.set, 0xE3, surface, 17, 33, &metrics, &out) == FONT_OK && fnv1a(pixels, sizeof(pixels)) == before_set[s]);
        memset(pixels, 0, sizeof(pixels));
        CHECK(font.render(native, 0xE3, surface, 17, 45, &metrics, &out) == FONT_OK && fnv1a(pixels, sizeof(pixels)) == before_native[s]);
    }

    /* The character-message phrases: every letter from the game font, accented ones at their base letter's scale. */
    CHECK(size_like_the_game(fonts.game, 32, 0.75f) && size_like_the_game(fonts.set, 32, 1.0f));
    const char *phrases[] = {"ação", "ótima", "não", "Não", "Graças à deusa!", "Situação sem saída..."};
    for (size_t p = 0; p < sizeof(phrases) / sizeof(*phrases); ++p) {
        uint32_t codes[64];
        const size_t length = utf8_codes(phrases[p], codes, 64);
        int ok = 1;
        for (size_t i = 0; i < length; ++i) {
            const DcpComposedGlyph *glyph = composed_entry(glyphs, count, codes[i]);
            int used = 0;
            if (glyph) ok &= same_scale_as_base(&fonts, codes[i], glyph->base, &used);
            else if (codes[i] != ' ') {
                Ink ink;
                float advance;
                ok &= render_like_the_game(&fonts, codes[i], &ink, &advance) == 1;
            }
        }
        CHECK(ok);
        printf("  PT-BR-MESSAGE|%s|%s\n", phrases[p], ok ? "every letter at the game font's scale" : "MISMATCH");
    }

close:
    runtime_font_set_composed_glyphs(NULL, 0, 0);
    if (fonts.game) CHECK(font.close(fonts.game) == FONT_OK);
    if (fonts.set) CHECK(font.close(fonts.set) == FONT_OK);
    if (native) CHECK(font.close(native) == FONT_OK);
    CHECK(font.destroy_library(&library) == FONT_OK);
    CHECK(font.destroy_renderer(&renderer) == FONT_OK);
done:
    free(atlas), free(marks), free(song), free(skip);
}

int main(void) {
    runtime_start(1);
    test_ime_panel_size();
    test_font_set();
    test_font_memory(DCP_SOURCE_DIR "/gpu/third_party/fonts/DejaVuSans.ttf");
    test_composed_letters();
    printf("text tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
