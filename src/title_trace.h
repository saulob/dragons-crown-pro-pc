/* DEBUG_SAULO: logs the part sequence of the title's two layout players, one line per change. Temporary. */
#ifndef DCP_TITLE_TRACE_H
#define DCP_TITLE_TRACE_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

size_t title_trace_stub_bytes(void);

/* The part setter's entry, found by its code; 0 unless it appears exactly once. */
int title_trace_locate(const uint8_t *image, uint64_t size, uint32_t *site);
int title_trace_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size);

void GUEST_HOOK_ABI title_trace_hook(GuestRegs *regs);
void title_trace_reset(void);
int title_trace_lines(void);
const char *title_trace_last(int player);

#ifdef __cplusplus
}
#endif

#endif
