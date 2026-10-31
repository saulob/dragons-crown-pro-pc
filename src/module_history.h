/* Fixed-size record of the loaded modules and the most recent unloads, written from a loader notification and read by the
 * crash report: no allocation and no locks. Platform independent. */
#ifndef DCP_MODULE_HISTORY_H
#define DCP_MODULE_HISTORY_H

#include <stddef.h>
#include <stdint.h>

enum { MODULE_LOADED_MAX = 512, MODULE_UNLOADS_MAX = 64, MODULE_NAME_SIZE = 40, MODULE_PATH_SIZE = 144 };

typedef struct {
    uintptr_t base; /* 0: unused */
    size_t size;
    char name[MODULE_NAME_SIZE];
    char path[MODULE_PATH_SIZE]; /* external modules only */
    int external;
    uint64_t unloaded_ms;
    const char *unloaded_phase; /* must outlive the process */
} ModuleRecord;

typedef struct {
    ModuleRecord loaded[MODULE_LOADED_MAX];
    ModuleRecord unloads[MODULE_UNLOADS_MAX];
    unsigned long unload_total;
    unsigned long dropped;
} ModuleHistory;

typedef struct {
    const ModuleRecord *record;
    int unloaded;
    size_t offset;
} ModuleMatch;

/* `path` is NULL for a module of the Windows folder. A full table counts the module in `dropped`. */
void module_history_loaded(ModuleHistory *history, uintptr_t base, size_t size, const char *name, const char *path);

/* Moves the module at `base` to the unload ring; the other arguments describe a module that was never recorded. */
void module_history_unloaded(ModuleHistory *history, uintptr_t base, size_t size, const char *name, const char *path,
                             uint64_t now_ms, const char *phase);

/* A loaded module wins over an unloaded one at the same address, then the newest unload. */
int module_history_find(const ModuleHistory *history, uintptr_t address, ModuleMatch *match);

/* The loaded modules closest below and above `address`; either may be NULL. */
void module_history_neighbors(const ModuleHistory *history, uintptr_t address, const ModuleRecord **below,
                              const ModuleRecord **above);

/* Newest unload first; returns how many were stored. */
unsigned module_history_recent_unloads(const ModuleHistory *history, const ModuleRecord **records, unsigned max);

/* "name+0x10", or "UNLOADED name+0x10, unloaded 18 ms ago during <phase>". 0 and an empty string when no record holds
 * the address. */
int module_history_describe(const ModuleHistory *history, uintptr_t address, uint64_t now_ms, char *out, size_t size);

#endif
