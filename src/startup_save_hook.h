/* First boot of a Japanese build with no save: the startup screen creates the save itself, then continues through the
 * code it runs after a successful load. One explicit record per verified build; others have none. */
#ifndef DCP_STARTUP_SAVE_HOOK_H
#define DCP_STARTUP_SAVE_HOOK_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STARTUP_SAVE_SITE_LENGTH 10
#define STARTUP_SAVE_PROLOGUE_LENGTH 8

typedef struct {
    const char *build;
    GuestImageId image;
    uint32_t step_offset; /* where the startup screen sets its next step */
    uint8_t step_original[STARTUP_SAVE_SITE_LENGTH];
    uint32_t save_offset; /* returns the save block of the loaded-save continuation */
    uint8_t save_prologue[STARTUP_SAVE_PROLOGUE_LENGTH];
    uint32_t accept_offset; /* applies a loaded save: what a successful load runs before the title */
    uint8_t accept_prologue[STARTUP_SAVE_PROLOGUE_LENGTH];
} StartupSaveRecord;

#define STARTUP_SAVE_RECORD_COUNT 3
extern const StartupSaveRecord startup_save_records[STARTUP_SAVE_RECORD_COUNT];

typedef uint64_t(GUEST_HOOK_ABI *StartupSaveBlockFn)(uint64_t owner);
typedef void(GUEST_HOOK_ABI *StartupSaveAcceptFn)(uint64_t block, uint64_t controller);

void startup_save_set_functions(StartupSaveBlockFn block, StartupSaveAcceptFn accept);
void GUEST_HOOK_ABI startup_save_step_hook(GuestRegs *regs);

size_t startup_save_hook_stub_bytes(void);

/* 1 when the image is a record's build and every verified byte matches; 0 otherwise, the image untouched. */
int startup_save_hook_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size);

#ifdef __cplusplus
}
#endif

#endif
