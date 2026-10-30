/* Online player cards: the System type line names each player's PC platform and port version instead of
 * "PlayStation 4". Every copy of the game marks its own player info as PS4, so the text cannot come from the data the
 * players exchange: at the card's two calls that set the PS4 record, the hook sets the text for the Online ID the card
 * shows instead. One record per verified build; another supported build is searched for the same card code. */
#ifndef DCP_SYSTEM_TYPE_HOOK_H
#define DCP_SYSTEM_TYPE_HOOK_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYSTEM_TYPE_PS4_RECORD 0x122 /* SystemMsg "PlayStation$4"; 0x123 (PS3) and 0x124 (Vita) pass through */
#define SYSTEM_TYPE_SITES 2
#define SYSTEM_TYPE_SET_TEXT_CHECK 16

typedef struct {
    const char *build;
    GuestImageId image;
    uint32_t sites[SYSTEM_TYPE_SITES];          /* call record setter, with the widget in rdi and the record in esi */
    uint8_t calls[SYSTEM_TYPE_SITES][5];
    uint32_t set_text;                          /* (widget, UTF-8 text): what the card uses for the Online ID */
} SystemTypeRecord;

#define SYSTEM_TYPE_RECORD_COUNT 5
extern const SystemTypeRecord system_type_records[SYSTEM_TYPE_RECORD_COUNT];
extern const uint8_t system_type_set_text_prologue[SYSTEM_TYPE_SET_TEXT_CHECK];

typedef const char *(*SystemTypeTextFn)(const char *online_id);
typedef uint64_t(GUEST_HOOK_ABI *SystemTypeSetTextFn)(uint64_t widget, const char *text);

void system_type_set_functions(SystemTypeTextFn text, SystemTypeSetTextFn set_text);
void GUEST_HOOK_ABI system_type_hook(GuestRegs *regs);

size_t system_type_hook_stub_bytes(void);

/* The sites of an image: 1 from its build's record, 2 from the single match of the verified card code, 0 none. */
int system_type_locate(const uint8_t *image, uint64_t size, uint64_t entry, SystemTypeRecord *out);

/* 1 when the sites are located and every verified byte matches; 0 otherwise, the image untouched. */
int system_type_hook_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size,
                             SystemTypeTextFn text);

#ifdef __cplusplus
}
#endif

#endif
