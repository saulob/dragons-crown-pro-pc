/* Decodes the indirect call or jump that led to a bad execute address, for the crash report. Platform independent. */
#ifndef DCP_CALL_SITE_H
#define DCP_CALL_SITE_H

#include <stddef.h>
#include <stdint.h>

typedef enum { CALL_SITE_DIRECT, CALL_SITE_REGISTER, CALL_SITE_MEMORY, CALL_SITE_RIP_MEMORY } CallSiteKind;

typedef struct {
    CallSiteKind kind;
    unsigned length;
    int is_jump;
    int reg;               /* 0-15 in CONTEXT order; -1 for a SIB operand */
    int32_t displacement;  /* branch offset, memory displacement or RIP displacement */
} CallSite;

/* `before` ends with the last byte before the return address. 1 when a call or jump ends exactly there. */
int call_site_decode(const uint8_t *before, size_t size, CallSite *site);

/* "call rax", "call [rax+0x58]", "call [rip+0x1234]", "call rel 0x1000". */
void call_site_format(const CallSite *site, char *out, size_t size);

#endif
