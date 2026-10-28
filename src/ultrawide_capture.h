/* DEBUG_SAULO: guest side of the F9 ultrawide frame capture. While a capture is wanted, the pass bucket queue tells it
 * who queued each render command, and the pass executor where in the guest draw command buffer each command starts,
 * with its pass. Observers only: nothing the game computes changes. Same builds as ultrawide_records. */
#ifndef DCP_ULTRAWIDE_CAPTURE_H
#define DCP_ULTRAWIDE_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"
#include "ultrawide_hooks.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ULTRAWIDE_CAPTURE_HOOK_COUNT 3

typedef struct {
    uint32_t queue;    /* queue(bucket set, command, priority) */
    uint32_t executor; /* runs one bucket set's commands */
} UltrawideCaptureRecord;

/* Indexed like ultrawide_records. */
extern const UltrawideCaptureRecord ultrawide_capture_records[ULTRAWIDE_RECORD_COUNT];

void ultrawide_capture_record_hooks(const UltrawideCaptureRecord *record, GuestHook hooks[ULTRAWIDE_CAPTURE_HOOK_COUNT]);

size_t ultrawide_capture_stub_bytes(void);

/* 1 when installed; otherwise 0 with the reason in `detail`, the image untouched. */
int ultrawide_capture_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size,
                              char *detail, size_t detail_size);

#ifdef __cplusplus
}
#endif

#endif
