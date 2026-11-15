#include "inn_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LAYOUT_FLAGS = 0x17,
    LAYOUT_RELOCATED = 0x04,
    LAYOUT_HEADER = 0x120,
    LAYOUT_TRANSFORM_COUNT = 0x5e,
    LAYOUT_KEYFRAME_COUNT = 0x60,
    LAYOUT_PART_COUNT = 0x64,
    LAYOUT_LAYER_COUNT = 0x66,
    LAYOUT_TRANSFORMS = 0xe8,
    LAYOUT_KEYFRAMES = 0xf0,
    LAYOUT_PARTS = 0xf8,
    LAYOUT_LAYERS = 0x100,
    MENU_TRANSFORMS = 1049,
    MENU_KEYFRAMES = 6600,
    MENU_PARTS = 471,
    MENU_LAYERS = 2046,
    ANCHOR_PART = 3,
    PART_SIZE = 48,
    PART_NAME = 0x10,
    PART_NAME_SIZE = 24,
    PART_LAYER_OFFSET = 0x28,
    PART_LAYER_COUNT = 0x2a,
    PART_MASTER = 0x2b,
    LAYER_SIZE = 24,
    LAYER_KEY_START = 0,
    LAYER_KEY_COUNT = 2,
    KEYFRAME_SIZE = 32,
    KEYFRAME_TRANSFORM = 4,
    TRANSFORM_SIZE = 36,
    TRANSFORM_X = 0,
    TRANSFORM_Y = 4,
    TRANSFORM_SCALE_Y = 28,
    NO_ROW = -1,
    NO_DECIDED = -1,
    MAX_OPTIONS_KEYS = 64,
    MAX_LAYOUTS = 4,
    INSERTED_ROW = 3,
    OPTIONS_NATIVE_ROW = 3
};

/* The row backings sit on the menu column's centre line. */
static const float kRowX = -420.0f;

static const char *const kInnParts[INN_LAYOUT_PARTS] = {
    "INN_0", "INN_1", "INN_2", "INN_A0", "INN_C0", "INN_C2", "INN_D0", "INN_D2",
    "INN_E0", "INN_E1", "INN_F0", "INN_G0", "INN_G0_1", "INN_G2", "INN_G2_1", "INN_H0"};

/* A key of the Options part that follows the confirmed row: its lit strip and flash, or the plain strip it trades with. */
typedef struct {
    uint16_t transform, original;
    int8_t template_row, lit;
} OptionsKey;

typedef struct {
    uint8_t *layout;
    uint8_t *own_tables[4];
    uint16_t own_counts[3];
    uint8_t *block;
    uint8_t *tables[4];
    uint16_t counts[3];
    float centers[INN_LAYOUT_ROWS];
    float rows[INN_LAYOUT_VANILLA_ROWS];
    float scale;
    uint16_t parts[INN_LAYOUT_PARTS], rows_end[INN_LAYOUT_PARTS], layer_counts[INN_LAYOUT_PARTS];
    int options_count;
    OptionsKey options[MAX_OPTIONS_KEYS];
} Patch;

static Patch patches[MAX_LAYOUTS];

static const int kTableAt[4] = {LAYOUT_TRANSFORMS, LAYOUT_KEYFRAMES, LAYOUT_PARTS, LAYOUT_LAYERS};
static const int kCountAt[3] = {LAYOUT_TRANSFORM_COUNT, LAYOUT_KEYFRAME_COUNT, LAYOUT_LAYER_COUNT};
enum { T_TRANSFORMS, T_KEYFRAMES, T_PARTS, T_LAYERS };

static uint16_t read_u16(const uint8_t *at) {
    uint16_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static void write_u16(uint8_t *at, uint16_t value) {
    memcpy(at, &value, sizeof(value));
}

static float read_f32(const uint8_t *at) {
    float value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static void write_f32(uint8_t *at, float value) {
    memcpy(at, &value, sizeof(value));
}

static uint8_t *table(const uint8_t *layout, int which) {
    uint64_t value;
    memcpy(&value, layout + kTableAt[which], sizeof(value));
    return (uint8_t *)(uintptr_t)value;
}

static void set_table(uint8_t *layout, int which, const uint8_t *value) {
    const uint64_t pointer = (uint64_t)(uintptr_t)value;
    memcpy(layout + kTableAt[which], &pointer, sizeof(pointer));
}

/* A view of a layout's four tables. */
typedef struct {
    uint8_t *transforms, *keyframes, *parts, *layers;
} Tables;

static Tables tables_of(const uint8_t *layout) {
    return (Tables){table(layout, T_TRANSFORMS), table(layout, T_KEYFRAMES), table(layout, T_PARTS), table(layout, T_LAYERS)};
}

static const uint8_t *part_record(const Tables *t, int part) {
    return t->parts + (size_t)part * PART_SIZE;
}

static const uint8_t *key_transform(const Tables *t, int key) {
    return t->transforms + (size_t)read_u16(t->keyframes + (size_t)key * KEYFRAME_SIZE + KEYFRAME_TRANSFORM) * TRANSFORM_SIZE;
}

static int part_named(const Tables *t, const char *name) {
    for (int i = 0; i < MENU_PARTS; ++i)
        if (!strncmp((const char *)part_record(t, i) + PART_NAME, name, PART_NAME_SIZE)) return i;
    return -1;
}

/* The vanilla row a layer backs: one of its keys sits on the column at that row; NO_ROW for none, -2 for several. */
static int layer_row(const Tables *t, int layer, const float rows[INN_LAYOUT_VANILLA_ROWS]) {
    const uint8_t *record = t->layers + (size_t)layer * LAYER_SIZE;
    const int start = read_u16(record + LAYER_KEY_START), count = read_u16(record + LAYER_KEY_COUNT);
    int row = NO_ROW;
    for (int k = start; k < start + count; ++k) {
        const uint8_t *transform = key_transform(t, k);
        if (read_f32(transform + TRANSFORM_X) != kRowX) continue;
        for (int r = 0; r < INN_LAYOUT_VANILLA_ROWS; ++r)
            if (read_f32(transform + TRANSFORM_Y) == rows[r]) {
                if (row != NO_ROW && row != r) return -2;
                row = r;
            }
    }
    return row;
}

/* The game loads a layout in place: its table offsets become addresses past the header, and the flag says so. */
static int tables_loaded(const uint8_t *layout, const Tables *t) {
    const uint8_t *const tables[4] = {t->transforms, t->keyframes, t->parts, t->layers};
    if (!(layout[LAYOUT_FLAGS] & LAYOUT_RELOCATED)) return 0;
    for (int i = 0; i < 4; ++i)
        if (!tables[i] || tables[i] < layout + LAYOUT_HEADER) return 0;
    return 1;
}

static const char *menu_layout_failure(const uint8_t *layout, Tables *t) {
    if (read_u16(layout + LAYOUT_PART_COUNT) != MENU_PARTS || read_u16(layout + LAYOUT_TRANSFORM_COUNT) != MENU_TRANSFORMS ||
        read_u16(layout + LAYOUT_KEYFRAME_COUNT) != MENU_KEYFRAMES || read_u16(layout + LAYOUT_LAYER_COUNT) != MENU_LAYERS)
        return "not the known menu layout";
    *t = tables_of(layout);
    if (!tables_loaded(layout, t)) return "runtime tables unavailable";
    if (strncmp((const char *)part_record(t, ANCHOR_PART) + PART_NAME, "INN_1", PART_NAME_SIZE)) return "INN_1 is not part 3";
    return NULL;
}

static int is_menu_layout(const uint8_t *layout, Tables *t) {
    return !menu_layout_failure(layout, t);
}

/* The rows of the idle menu part: six backings evenly spaced down the column. */
static int vanilla_rows(const Tables *t, float rows[INN_LAYOUT_VANILLA_ROWS]) {
    const uint8_t *part = part_record(t, ANCHOR_PART);
    const int first = read_u16(part + PART_LAYER_OFFSET), count = part[PART_LAYER_COUNT];
    int found = 0;
    for (int layer = first; layer < first + count; ++layer) {
        const uint8_t *record = t->layers + (size_t)layer * LAYER_SIZE;
        if (read_u16(record + LAYER_KEY_COUNT) != 1) continue;
        const uint8_t *transform = key_transform(t, read_u16(record + LAYER_KEY_START));
        if (read_f32(transform + TRANSFORM_X) != kRowX) continue;
        if (found == INN_LAYOUT_VANILLA_ROWS) return 0;
        rows[found++] = read_f32(transform + TRANSFORM_Y);
    }
    if (found != INN_LAYOUT_VANILLA_ROWS) return 0;
    const float pitch = rows[1] - rows[0];
    for (int r = 1; r < INN_LAYOUT_VANILLA_ROWS; ++r)
        if (!(pitch > 0.0f) || rows[r] - rows[r - 1] != pitch) return 0;
    return 1;
}

/* A menu part: six consecutive row layers, one per row, and at most one more on the row the menu confirmed. */
typedef struct {
    int part, first, count, run, decided_at, decided_row;
    int8_t run_rows[INN_LAYOUT_VANILLA_ROWS];
} MenuPart;

static int menu_part(const Tables *t, int part, const float rows[INN_LAYOUT_VANILLA_ROWS], MenuPart *out) {
    const uint8_t *record = part_record(t, part);
    *out = (MenuPart){part, read_u16(record + PART_LAYER_OFFSET), record[PART_LAYER_COUNT], -1, NO_DECIDED, NO_ROW, {0}};
    int seen = 0, run_length = 0;
    for (int at = 0; at < out->count; ++at) {
        const int row = layer_row(t, out->first + at, rows);
        if (row == -2) return 0;
        if (row == NO_ROW) continue;
        if (out->run < 0 || (run_length < INN_LAYOUT_VANILLA_ROWS && at == out->run + run_length)) {
            if (out->run < 0) out->run = at;
            if (seen & (1 << row)) return 0;
            seen |= 1 << row;
            out->run_rows[run_length++] = (int8_t)row;
        } else if (out->decided_at == NO_DECIDED) {
            out->decided_at = at;
            out->decided_row = row;
        } else {
            return 0;
        }
    }
    return run_length == INN_LAYOUT_VANILLA_ROWS && seen == (1 << INN_LAYOUT_VANILLA_ROWS) - 1;
}

static int scan_failed(InnLayoutScan *scan, const char *why, const char *part) {
    snprintf(scan->failure, sizeof(scan->failure), "%s%s%s", part ? part : "", part ? ": " : "", why);
    return 0;
}

int inn_layout_scan(const uint8_t *layout, InnLayoutScan *scan) {
    Tables t;
    memset(scan, 0, sizeof(*scan));
    if (!layout) return scan_failed(scan, "no runtime layout", NULL);
    const char *why = menu_layout_failure(layout, &t);
    if (why) return scan_failed(scan, why, NULL);
    if (!vanilla_rows(&t, scan->rows)) return scan_failed(scan, "row layers fingerprint mismatch", "INN_1");
    for (int i = 0; i < INN_LAYOUT_PARTS; ++i) {
        MenuPart part;
        const int index = part_named(&t, kInnParts[i]);
        if (index < 0) return scan_failed(scan, "missing", kInnParts[i]);
        if (!menu_part(&t, index, scan->rows, &part)) return scan_failed(scan, "expected 6 row layers", kInnParts[i]);
        scan->part[i] = (uint16_t)index;
        scan->first_row_layer[i] = (uint16_t)(part.first + part.run);
    }
    scan->count = INN_LAYOUT_PARTS;
    return 1;
}

/* The native row a custom row copies: its own, and for the inserted row a neighbour the part does not light. */
static int template_row(int row, int decided_row) {
    if (row < INSERTED_ROW) return row;
    if (row > INSERTED_ROW) return row - 1;
    return decided_row == INSERTED_ROW ? INSERTED_ROW - 1 : INSERTED_ROW;
}

static int displayed_row(int native_row) {
    return native_row < INSERTED_ROW ? native_row : native_row + 1;
}

typedef struct {
    Tables from, to;
    int layers, keyframes, transforms;
    const float *rows, *centers;
    float scale;
    Patch *patch;
} Builder;

static void move_transform(uint8_t *transform, float dy, float scale) {
    write_f32(transform + TRANSFORM_Y, read_f32(transform + TRANSFORM_Y) + dy);
    if (read_f32(transform + TRANSFORM_X) == kRowX)
        write_f32(transform + TRANSFORM_SCALE_Y, read_f32(transform + TRANSFORM_SCALE_Y) * scale);
}

static int run_layer(const MenuPart *part, int native_row) {
    int at = 0;
    while (part->run_rows[at] != native_row) ++at;
    return part->first + part->run + at;
}

/* Appends a copy of `layer` whose keys use new transforms moved by `dy`; the Options part's keys that follow the
 * confirmed row are remembered with `template_row` and whether they are lit. */
static void add_moved_layer(Builder *b, int layer, float dy, int template_row, int lit) {
    uint8_t *record = b->to.layers + (size_t)b->layers * LAYER_SIZE;
    memcpy(record, b->from.layers + (size_t)layer * LAYER_SIZE, LAYER_SIZE);
    const int start = read_u16(record + LAYER_KEY_START), count = read_u16(record + LAYER_KEY_COUNT);
    write_u16(record + LAYER_KEY_START, (uint16_t)b->keyframes);
    for (int k = start; k < start + count; ++k) {
        uint8_t *key = b->to.keyframes + (size_t)b->keyframes * KEYFRAME_SIZE;
        memcpy(key, b->from.keyframes + (size_t)k * KEYFRAME_SIZE, KEYFRAME_SIZE);
        const uint16_t original = read_u16(key + KEYFRAME_TRANSFORM);
        uint8_t *transform = b->to.transforms + (size_t)b->transforms * TRANSFORM_SIZE;
        memcpy(transform, b->from.transforms + (size_t)original * TRANSFORM_SIZE, TRANSFORM_SIZE);
        move_transform(transform, dy, b->scale);
        write_u16(key + KEYFRAME_TRANSFORM, (uint16_t)b->transforms);
        if (template_row != NO_ROW && b->patch->options_count < MAX_OPTIONS_KEYS)
            b->patch->options[b->patch->options_count++] =
                (OptionsKey){(uint16_t)b->transforms, original, (int8_t)template_row, (int8_t)lit};
        ++b->keyframes;
        ++b->transforms;
    }
    ++b->layers;
}

static void rebuild_part(Builder *b, const MenuPart *part) {
    uint8_t *record = b->to.parts + (size_t)part->part * PART_SIZE;
    const int start = b->layers, master = record[PART_MASTER];
    const int options = part->decided_row == OPTIONS_NATIVE_ROW;
    int new_master = -1;
    for (int at = 0; at < part->count; ++at) {
        const int layer = part->first + at;
        if (at == part->run) {
            for (int row = 0; row < INN_LAYOUT_ROWS; ++row) {
                const int from = template_row(row, part->decided_row);
                const int source = run_layer(part, from);
                if (master == source - part->first && new_master < 0) new_master = b->layers - start;
                const int follows = options && (row == INSERTED_ROW || from == OPTIONS_NATIVE_ROW);
                add_moved_layer(b, source, b->centers[row] - b->rows[from], follows ? from : NO_ROW, from == OPTIONS_NATIVE_ROW);
            }
            at += INN_LAYOUT_VANILLA_ROWS - 1;
            continue;
        }
        if (at == master) new_master = b->layers - start;
        if (at == part->decided_at) {
            const int row = part->decided_row;
            add_moved_layer(b, layer, b->centers[displayed_row(row)] - b->rows[row], options ? row : NO_ROW, 1);
        } else {
            memcpy(b->to.layers + (size_t)b->layers * LAYER_SIZE, b->from.layers + (size_t)layer * LAYER_SIZE, LAYER_SIZE);
            ++b->layers;
        }
    }
    write_u16(record + PART_LAYER_OFFSET, (uint16_t)start);
    record[PART_LAYER_COUNT] = (uint8_t)(part->count + 1);
    record[PART_MASTER] = (uint8_t)(new_master < 0 ? master : new_master);
}

static int key_count(const Tables *t, int layer) {
    return read_u16(t->layers + (size_t)layer * LAYER_SIZE + LAYER_KEY_COUNT);
}

static InnLayoutStatus build(Patch *patch, uint8_t *layout, const Tables *own, const float centers[INN_LAYOUT_ROWS], float scale) {
    float rows[INN_LAYOUT_VANILLA_ROWS];
    MenuPart parts[INN_LAYOUT_PARTS];
    if (!vanilla_rows(own, rows)) return INN_LAYOUT_UNSUPPORTED;
    int added_layers = 0, added_keys = 0;
    for (int i = 0; i < INN_LAYOUT_PARTS; ++i) {
        const int index = part_named(own, kInnParts[i]);
        if (index < 0 || !menu_part(own, index, rows, &parts[i])) return INN_LAYOUT_UNSUPPORTED;
        added_layers += parts[i].count + 1;
        for (int row = 0; row < INN_LAYOUT_ROWS; ++row)
            added_keys += key_count(own, run_layer(&parts[i], template_row(row, parts[i].decided_row)));
        if (parts[i].decided_at != NO_DECIDED) added_keys += key_count(own, parts[i].first + parts[i].decided_at);
    }
    if (MENU_LAYERS + added_layers > UINT16_MAX || MENU_KEYFRAMES + added_keys > UINT16_MAX ||
        MENU_TRANSFORMS + added_keys > UINT16_MAX)
        return INN_LAYOUT_UNSUPPORTED;
    const size_t sizes[4] = {(size_t)(MENU_TRANSFORMS + added_keys) * TRANSFORM_SIZE, (size_t)(MENU_KEYFRAMES + added_keys) * KEYFRAME_SIZE,
                             (size_t)MENU_PARTS * PART_SIZE, (size_t)(MENU_LAYERS + added_layers) * LAYER_SIZE};
    uint8_t *block = malloc(sizes[0] + sizes[1] + sizes[2] + sizes[3]);
    if (!block) return INN_LAYOUT_NO_MEMORY;
    memset(patch, 0, sizeof(*patch));
    patch->block = block;
    size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        patch->tables[i] = block + at;
        at += sizes[i];
    }
    Builder b = {*own, {patch->tables[T_TRANSFORMS], patch->tables[T_KEYFRAMES], patch->tables[T_PARTS], patch->tables[T_LAYERS]},
                 MENU_LAYERS, MENU_KEYFRAMES, MENU_TRANSFORMS, rows, centers, scale, patch};
    memcpy(b.to.transforms, own->transforms, (size_t)MENU_TRANSFORMS * TRANSFORM_SIZE);
    memcpy(b.to.keyframes, own->keyframes, (size_t)MENU_KEYFRAMES * KEYFRAME_SIZE);
    memcpy(b.to.parts, own->parts, (size_t)MENU_PARTS * PART_SIZE);
    memcpy(b.to.layers, own->layers, (size_t)MENU_LAYERS * LAYER_SIZE);
    for (int i = 0; i < INN_LAYOUT_PARTS; ++i) {
        rebuild_part(&b, &parts[i]);
        patch->parts[i] = (uint16_t)parts[i].part;
        patch->rows_end[i] = (uint16_t)(parts[i].run + INN_LAYOUT_VANILLA_ROWS);
        patch->layer_counts[i] = (uint16_t)(parts[i].count + 1);
    }
    patch->layout = layout;
    for (int i = 0; i < 4; ++i) patch->own_tables[i] = table(layout, i);
    for (int i = 0; i < 3; ++i) patch->own_counts[i] = read_u16(layout + kCountAt[i]);
    patch->counts[0] = (uint16_t)b.transforms;
    patch->counts[1] = (uint16_t)b.keyframes;
    patch->counts[2] = (uint16_t)b.layers;
    memcpy(patch->centers, centers, sizeof(patch->centers));
    memcpy(patch->rows, rows, sizeof(patch->rows));
    patch->scale = scale;
    return INN_LAYOUT_APPLIED;
}

static Patch *patch_of(const uint8_t *layout) {
    for (int i = 0; i < MAX_LAYOUTS; ++i)
        if (patches[i].layout && patches[i].layout == layout) return &patches[i];
    return NULL;
}

int inn_layout_applied(const uint8_t *layout) {
    const Patch *patch = layout ? patch_of(layout) : NULL;
    return patch && table(layout, T_PARTS) == patch->tables[T_PARTS];
}

static void point_at(uint8_t *layout, uint8_t *const tables[4], const uint16_t counts[3]) {
    for (int i = 0; i < 4; ++i) set_table(layout, i, tables[i]);
    for (int i = 0; i < 3; ++i) write_u16(layout + kCountAt[i], counts[i]);
}

/* A layout seen before keeps its clone for as long as it uses the same tables. */
static int clone_fits(const Patch *patch, const uint8_t *layout, const float centers[INN_LAYOUT_ROWS], float scale) {
    for (int i = 0; i < 4; ++i)
        if (table(layout, i) != patch->own_tables[i]) return 0;
    return patch->scale == scale && !memcmp(patch->centers, centers, sizeof(patch->centers));
}

InnLayoutStatus inn_layout_apply(uint8_t *layout, const float centers[INN_LAYOUT_ROWS], float row_scale) {
    if (!layout) return INN_LAYOUT_OTHER;
    if (inn_layout_applied(layout)) return INN_LAYOUT_APPLIED;
    Tables own;
    if (!is_menu_layout(layout, &own)) return INN_LAYOUT_OTHER;
    Patch *patch = patch_of(layout);
    if (!patch || !clone_fits(patch, layout, centers, row_scale)) {
        if (!patch)
            for (int i = 0; i < MAX_LAYOUTS && !patch; ++i)
                if (!patches[i].layout) patch = &patches[i];
        if (!patch) return INN_LAYOUT_NO_MEMORY;
        uint8_t *old = patch->block;
        const InnLayoutStatus status = build(patch, layout, &own, centers, row_scale);
        if (status != INN_LAYOUT_APPLIED) return status;
        free(old);
    }
    point_at(layout, patch->tables, patch->counts);
    return INN_LAYOUT_APPLIED;
}

int inn_layout_restore(uint8_t *layout) {
    if (!inn_layout_applied(layout)) return 0;
    const Patch *patch = patch_of(layout);
    point_at(layout, patch->own_tables, patch->own_counts);
    return 1;
}

int inn_layout_position(const uint8_t *layout, int part, int position, int instances) {
    const Patch *patch = layout ? patch_of(layout) : NULL;
    if (!patch) return position;
    for (int i = 0; i < INN_LAYOUT_PARTS; ++i)
        if (patch->parts[i] == part)
            return instances == patch->layer_counts[i] && position >= patch->rows_end[i] ? position + 1 : position;
    return position;
}

void inn_layout_reset(void) {
    for (int i = 0; i < MAX_LAYOUTS; ++i) free(patches[i].block);
    memset(patches, 0, sizeof(patches));
}

void inn_layout_options_row(uint8_t *layout, int row) {
    Patch *patch = layout ? patch_of(layout) : NULL;
    if (!patch || (row != INSERTED_ROW && row != displayed_row(OPTIONS_NATIVE_ROW))) return;
    const int other = row == INSERTED_ROW ? displayed_row(OPTIONS_NATIVE_ROW) : INSERTED_ROW;
    for (int i = 0; i < patch->options_count; ++i) {
        const OptionsKey *key = &patch->options[i];
        uint8_t *transform = patch->tables[T_TRANSFORMS] + (size_t)key->transform * TRANSFORM_SIZE;
        memcpy(transform, patch->own_tables[T_TRANSFORMS] + (size_t)key->original * TRANSFORM_SIZE, TRANSFORM_SIZE);
        move_transform(transform, patch->centers[key->lit ? row : other] - patch->rows[key->template_row], patch->scale);
    }
}
