/* Character creation in a translated language: an empty message row shows the default the character will receive, not "no message". The
 * game picks one DefaultBone variant per character when it is added, so the variant previewed for a class is held and
 * given to that pick. The guest's rand still runs once, and nothing is written to the character before it is added. */
#ifndef DCP_DEFAULT_MESSAGE_HOOK_H
#define DCP_DEFAULT_MESSAGE_HOOK_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DEFAULT_MESSAGE_CLASSES = 6,
    DEFAULT_MESSAGE_VARIANTS = 15,
    DEFAULT_MESSAGE_MAX_UNITS = 30 /* UTF-16 units a character's message field keeps */
};

/* The order of the rows. */
typedef enum {
    DEFAULT_MESSAGE_GREETING,
    DEFAULT_MESSAGE_SELECTION,
    DEFAULT_MESSAGE_RESURRECTION,
    DEFAULT_MESSAGE_DEATH,
    DEFAULT_MESSAGE_CATEGORIES
} DefaultMessageCategory;

/* The DefaultBone record of a class (the byte at character +0x27), or -1 out of range. */
int default_message_record(int character_class, DefaultMessageCategory category, int variant);

typedef struct {
    uint64_t rows[DEFAULT_MESSAGE_CATEGORIES]; /* the row's empty-field text is loaded here */
    uint64_t pick;                             /* the picker reduces rand's result here */
    uint64_t picker_return;                    /* where the picker returns to in the character defaults */
} DefaultMessageSites;

/* 1 when the row code, the picker, its one call from the character defaults and the record tables all match. */
int default_message_locate(const unsigned char *image, uint64_t size, DefaultMessageSites *sites);

size_t default_message_stub_bytes(void);

/* Takes every message from record_text (the served sheet) and hooks the rows and the pick; returns 1 when installed.
 * Writes nothing and says why when a record, the code or the stub space does not fit. */
int default_message_install(unsigned char *image, uint64_t size, uint64_t entry, const char *(*record_text)(unsigned),
                            unsigned char *stubs, size_t stubs_size);

/* From guest code at a row: rsi becomes the class's default for that row. */
void default_message_preview(GuestRegs *regs, DefaultMessageCategory category);

/* From guest code at the pick: rax becomes the variant held for the class, when the character being added has an empty
 * message. */
void GUEST_HOOK_ABI default_message_pin(GuestRegs *regs);

#ifdef __cplusplus
}
#endif

#endif
