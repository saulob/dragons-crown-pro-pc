#include "module_history.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void copy_text(char *out, size_t size, const char *text) {
    size_t n = 0;
    for (; text && n + 1 < size && text[n]; ++n) out[n] = text[n];
    out[n] = 0;
}

static void fill(ModuleRecord *record, size_t size, const char *name, const char *path) {
    record->size = size;
    copy_text(record->name, sizeof(record->name), name);
    copy_text(record->path, sizeof(record->path), path);
    record->external = path != NULL;
    record->unloaded_ms = 0;
    record->unloaded_phase = NULL;
}

static ModuleRecord *find_loaded(ModuleHistory *history, uintptr_t base) {
    for (size_t i = 0; i < MODULE_LOADED_MAX; ++i)
        if (history->loaded[i].base == base) return &history->loaded[i];
    return NULL;
}

void module_history_loaded(ModuleHistory *history, uintptr_t base, size_t size, const char *name, const char *path) {
    if (!base || !size) return;
    ModuleRecord *slot = find_loaded(history, base);
    if (!slot) slot = find_loaded(history, 0);
    if (!slot) { ++history->dropped; return; }
    __atomic_store_n(&slot->base, (uintptr_t)0, __ATOMIC_RELEASE);
    fill(slot, size, name, path);
    __atomic_store_n(&slot->base, base, __ATOMIC_RELEASE);
}

void module_history_unloaded(ModuleHistory *history, uintptr_t base, size_t size, const char *name, const char *path,
                             uint64_t now_ms, const char *phase) {
    if (!base) return;
    ModuleRecord *known = find_loaded(history, base);
    ModuleRecord *entry = &history->unloads[history->unload_total % MODULE_UNLOADS_MAX];
    ModuleRecord copy;
    if (known) {
        copy = *known;
        __atomic_store_n(&known->base, (uintptr_t)0, __ATOMIC_RELEASE);
    } else {
        if (!size) return;
        fill(&copy, size, name, path);
    }
    copy.base = 0;
    copy.unloaded_ms = now_ms;
    copy.unloaded_phase = phase;
    __atomic_store_n(&entry->base, (uintptr_t)0, __ATOMIC_RELEASE);
    *entry = copy;
    __atomic_store_n(&entry->base, base, __ATOMIC_RELEASE);
    ++history->unload_total;
}

static int contains(const ModuleRecord *record, uintptr_t address) {
    return record->base && address - record->base < record->size;
}

int module_history_find(const ModuleHistory *history, uintptr_t address, ModuleMatch *match) {
    for (size_t i = 0; i < MODULE_LOADED_MAX; ++i)
        if (contains(&history->loaded[i], address)) {
            match->record = &history->loaded[i];
            match->unloaded = 0;
            match->offset = address - history->loaded[i].base;
            return 1;
        }
    unsigned count = history->unload_total < MODULE_UNLOADS_MAX ? (unsigned)history->unload_total : MODULE_UNLOADS_MAX;
    for (unsigned back = 1; back <= count; ++back) {
        const ModuleRecord *record = &history->unloads[(history->unload_total - back) % MODULE_UNLOADS_MAX];
        if (contains(record, address)) {
            match->record = record;
            match->unloaded = 1;
            match->offset = address - record->base;
            return 1;
        }
    }
    return 0;
}

void module_history_neighbors(const ModuleHistory *history, uintptr_t address, const ModuleRecord **below,
                              const ModuleRecord **above) {
    *below = *above = NULL;
    for (size_t i = 0; i < MODULE_LOADED_MAX; ++i) {
        const ModuleRecord *record = &history->loaded[i];
        if (!record->base) continue;
        if (record->base + record->size <= address && (!*below || record->base > (*below)->base)) *below = record;
        if (record->base > address && (!*above || record->base < (*above)->base)) *above = record;
    }
}

unsigned module_history_recent_unloads(const ModuleHistory *history, const ModuleRecord **records, unsigned max) {
    unsigned count = history->unload_total < MODULE_UNLOADS_MAX ? (unsigned)history->unload_total : MODULE_UNLOADS_MAX;
    if (count > max) count = max;
    for (unsigned back = 1; back <= count; ++back)
        records[back - 1] = &history->unloads[(history->unload_total - back) % MODULE_UNLOADS_MAX];
    return count;
}

int module_history_describe(const ModuleHistory *history, uintptr_t address, uint64_t now_ms, char *out, size_t size) {
    ModuleMatch match;
    if (size) out[0] = 0;
    if (!module_history_find(history, address, &match)) return 0;
    if (!match.unloaded) {
        snprintf(out, size, "%s+0x%" PRIxPTR, match.record->name, (uintptr_t)match.offset);
        return 1;
    }
    const uint64_t ago = now_ms >= match.record->unloaded_ms ? now_ms - match.record->unloaded_ms : 0;
    snprintf(out, size, "UNLOADED %s+0x%" PRIxPTR ", unloaded %" PRIu64 " ms ago during %s", match.record->name,
             (uintptr_t)match.offset, ago, match.record->unloaded_phase ? match.record->unloaded_phase : "an unknown phase");
    return 1;
}
