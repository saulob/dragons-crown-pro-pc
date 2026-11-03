/* Focused tests of the four-row title layout patch on synthetic layouts built from the patch data. The real layouts of
 * every language are checked by scripts/title_layout_patch.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "title_layout.h"
#include "title_layout_fixture.h"

static int failures = 0;
#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                    \
        }                                                  \
    } while (0)

static const struct {
    const char *name;
    int count, master;
} kExpectedParts[TITLE_LAYOUT_PART_COUNT] = {
    {"TITLE_1", 6, 0},   {"START_0", 6, 5},   {"START_1", 12, 0},  {"START_2", 11, 0},  {"OPTION_0", 7, 6},  {"OPTION_1", 7, 6},
    {"OPTION_2", 6, 0},  {"GALLERY_0", 6, 5}, {"GALLERY_1", 9, 8}, {"GALLERY_2", 8, 0}, {"PSSTORE_0", 6, 5}};

static uint16_t u16(const uint8_t *at) {
    uint16_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static void snapshot(const FixtureLayout *layout, uint8_t *parts, uint8_t *layers, uint8_t *keyframes) {
    memcpy(parts, layout->parts, FIXTURE_PARTS * 48);
    memcpy(layers, layout->layers, FIXTURE_LAYERS * 24);
    memcpy(keyframes, layout->keyframes, FIXTURE_KEYFRAMES * 32);
}

static int unchanged(const FixtureLayout *layout, const uint8_t *parts, const uint8_t *layers, const uint8_t *keyframes) {
    return !memcmp(parts, layout->parts, FIXTURE_PARTS * 48) && !memcmp(layers, layout->layers, FIXTURE_LAYERS * 24) &&
           !memcmp(keyframes, layout->keyframes, FIXTURE_KEYFRAMES * 32);
}

static void test_patch(void) {
    FixtureLayout layout;
    CHECK(fixture_make(&layout));
    uint8_t *parts = malloc(FIXTURE_PARTS * 48), *layers = malloc(FIXTURE_LAYERS * 24), *keyframes = malloc(FIXTURE_KEYFRAMES * 32);
    CHECK(parts && layers && keyframes);
    snapshot(&layout, parts, layers, keyframes);

    CHECK(title_layout_patch(layout.header) == TITLE_LAYOUT_PATCHED);
    for (int i = 0; i < TITLE_LAYOUT_PART_COUNT; ++i) {
        const TitleLayoutPart *part = &kTitleLayoutParts[i];
        const uint8_t *record = layout.parts + part->index * 48;
        CHECK(!strcmp(part->name, kExpectedParts[i].name));
        CHECK(record[0x2a] == kExpectedParts[i].count && record[0x2b] == kExpectedParts[i].master);
        CHECK(u16(record + 0x28) == part->layer_offset);
        /* every part keeps only the layers its order names, copied from its own original layers */
        for (int pos = 0; pos < part->new_count; ++pos)
            CHECK(!memcmp(layout.layers + (part->layer_offset + pos) * 24,
                          kTitleLayoutOldLayers + part->old_layers_at + fixture_order(part)[pos] * 24, 24));
        CHECK(part->new_count == part->old_count - 2);
    }
    /* nothing outside the patched parts' layers, the listed keyframes and the parts' two bytes changes */
    int changed_layers = 0;
    for (int i = 0; i < FIXTURE_LAYERS; ++i) {
        if (memcmp(layers + i * 24, layout.layers + i * 24, 24)) {
            ++changed_layers;
            int inside = 0;
            for (int p = 0; p < TITLE_LAYOUT_PART_COUNT; ++p)
                inside |= i >= kTitleLayoutParts[p].layer_offset && i < kTitleLayoutParts[p].layer_offset + kTitleLayoutParts[p].old_count;
            CHECK(inside);
        }
    }
    CHECK(changed_layers > 0);
    for (int i = 0; i < FIXTURE_KEYFRAMES; ++i) {
        int listed = -1;
        for (int k = 0; k < TITLE_LAYOUT_KEYFRAME_COUNT; ++k)
            if (kTitleLayoutKeyframes[k].index == i) listed = k;
        const uint8_t *before = keyframes + i * 32, *after = layout.keyframes + i * 32;
        CHECK(!memcmp(before, after, 4) && !memcmp(before + 6, after + 6, 26));
        CHECK(u16(after + 4) == (listed < 0 ? u16(before + 4) : kTitleLayoutKeyframes[listed].new_transform));
    }
    for (int i = 0; i < FIXTURE_PARTS; ++i) {
        int patched = 0;
        for (int p = 0; p < TITLE_LAYOUT_PART_COUNT; ++p) patched |= kTitleLayoutParts[p].index == i;
        if (!patched) CHECK(!memcmp(parts + i * 48, layout.parts + i * 48, 48));
        else CHECK(!memcmp(parts + i * 48, layout.parts + i * 48, 0x2a) && !memcmp(parts + i * 48 + 0x2c, layout.parts + i * 48 + 0x2c, 4));
    }

    /* a layout that is already patched is recognised and left as it is */
    uint8_t *after_parts = malloc(FIXTURE_PARTS * 48), *after_layers = malloc(FIXTURE_LAYERS * 24), *after_keyframes = malloc(FIXTURE_KEYFRAMES * 32);
    CHECK(after_parts && after_layers && after_keyframes);
    snapshot(&layout, after_parts, after_layers, after_keyframes);
    CHECK(title_layout_patch(layout.header) == TITLE_LAYOUT_CURRENT);
    CHECK(unchanged(&layout, after_parts, after_layers, after_keyframes));
    free(after_parts);
    free(after_layers);
    free(after_keyframes);
    free(parts);
    free(layers);
    free(keyframes);
    fixture_free(&layout);
}

/* Applies `fault` to a fresh original (or patched) layout; the patch must refuse and write nothing. */
static void refuse(void (*fault)(FixtureLayout *), int patched_first, TitleLayoutStatus expected) {
    FixtureLayout layout;
    CHECK(fixture_make(&layout));
    if (patched_first) CHECK(title_layout_patch(layout.header) == TITLE_LAYOUT_PATCHED);
    fault(&layout);
    uint8_t *parts = malloc(FIXTURE_PARTS * 48), *layers = malloc(FIXTURE_LAYERS * 24), *keyframes = malloc(FIXTURE_KEYFRAMES * 32);
    uint8_t header[FIXTURE_HEADER];
    CHECK(parts && layers && keyframes);
    snapshot(&layout, parts, layers, keyframes);
    memcpy(header, layout.header, sizeof(header));
    CHECK(title_layout_patch(layout.header) == expected);
    CHECK(unchanged(&layout, parts, layers, keyframes) && !memcmp(header, layout.header, sizeof(header)));
    free(parts);
    free(layers);
    free(keyframes);
    fixture_free(&layout);
}

static void fault_part_count(FixtureLayout *l) { fixture_put_u16(l->header + 0x64, FIXTURE_PARTS - 1); }
static void fault_anchor_name(FixtureLayout *l) { l->parts[FIXTURE_ANCHOR_PART * 48 + 0x10] = 'X'; }
static void fault_no_parts(FixtureLayout *l) { memset(l->header + 0xf8, 0, 8); }
static void fault_layer_count(FixtureLayout *l) { fixture_put_u16(l->header + 0x66, FIXTURE_LAYERS + 1); }
static void fault_keyframe_count(FixtureLayout *l) { fixture_put_u16(l->header + 0x60, FIXTURE_KEYFRAMES - 1); }
static void fault_no_layers(FixtureLayout *l) { memset(l->header + 0x100, 0, 8); }
static void fault_layer_byte(FixtureLayout *l) { l->layers[(kTitleLayoutParts[5].layer_offset + 2) * 24 + 8] ^= 1; }
static void fault_last_layer_byte(FixtureLayout *l) { l->layers[(kTitleLayoutParts[10].layer_offset + 7) * 24 + 23] ^= 0x80; }
static void fault_part_name(FixtureLayout *l) { l->parts[kTitleLayoutParts[3].index * 48 + 0x10] = 'Z'; }
static void fault_part_offset(FixtureLayout *l) { l->parts[kTitleLayoutParts[3].index * 48 + 0x28] ^= 1; }
static void fault_part_count_byte(FixtureLayout *l) { l->parts[kTitleLayoutParts[8].index * 48 + 0x2a] += 1; }
static void fault_part_master(FixtureLayout *l) { l->parts[kTitleLayoutParts[8].index * 48 + 0x2b] ^= 1; }
static void fault_keyframe(FixtureLayout *l) { l->keyframes[kTitleLayoutKeyframes[7].index * 32 + 4] ^= 1; }
static void fault_one_part_patched(FixtureLayout *l) {
    const TitleLayoutPart *part = &kTitleLayoutParts[4];
    l->parts[part->index * 48 + 0x2a] = part->new_count;
    l->parts[part->index * 48 + 0x2b] = part->new_master;
}
static void fault_patched_keyframe(FixtureLayout *l) { l->keyframes[kTitleLayoutKeyframes[0].index * 32 + 4] ^= 1; }
static void fault_patched_layer(FixtureLayout *l) { l->layers[kTitleLayoutParts[6].layer_offset * 24] ^= 1; }
static void fault_patched_part_master(FixtureLayout *l) { l->parts[kTitleLayoutParts[9].index * 48 + 0x2b] ^= 1; }
static void fault_patched_all_but_keyframes(FixtureLayout *l) {
    for (int i = 0; i < TITLE_LAYOUT_KEYFRAME_COUNT; ++i)
        fixture_put_u16(l->keyframes + kTitleLayoutKeyframes[i].index * 32 + 4, kTitleLayoutKeyframes[i].old_transform);
}

static void test_refusals(void) {
    CHECK(title_layout_patch(NULL) == TITLE_LAYOUT_NOT_TITLE);
    uint8_t blank[FIXTURE_HEADER] = {0};
    CHECK(title_layout_patch(blank) == TITLE_LAYOUT_NOT_TITLE);
    refuse(fault_part_count, 0, TITLE_LAYOUT_NOT_TITLE);
    refuse(fault_anchor_name, 0, TITLE_LAYOUT_NOT_TITLE);
    refuse(fault_no_parts, 0, TITLE_LAYOUT_NOT_TITLE);
    refuse(fault_layer_count, 0, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_keyframe_count, 0, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_no_layers, 0, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_layer_byte, 0, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_last_layer_byte, 0, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_part_name, 0, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_part_offset, 0, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_part_count_byte, 0, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_part_master, 0, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_keyframe, 0, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_one_part_patched, 0, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_patched_keyframe, 1, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_patched_layer, 1, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_patched_part_master, 1, TITLE_LAYOUT_UNSUPPORTED);
    refuse(fault_patched_all_but_keyframes, 1, TITLE_LAYOUT_UNSUPPORTED);
}

int main(void) {
    test_patch();
    test_refusals();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("title layout tests passed\n");
    return 0;
}
