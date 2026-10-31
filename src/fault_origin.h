/* Decides whether an access violation caught before any handler is this program's to report. */
#ifndef DCP_FAULT_ORIGIN_H
#define DCP_FAULT_ORIGIN_H

typedef enum {
    FAULT_ADDRESS_GUEST,
    FAULT_ADDRESS_SELF,
    FAULT_ADDRESS_OTHER_MODULE,
    FAULT_ADDRESS_UNLISTED
} FaultAddress;

/* A fault at an address in no module (a call through a null pointer) belongs to the module that made the call: it
 * may be inside its own exception handler. Unhandled, it still reaches the unhandled exception filter. */
static inline int fault_is_ours(FaultAddress at, FaultAddress caller) {
    if (at == FAULT_ADDRESS_OTHER_MODULE) return 0;
    if (at == FAULT_ADDRESS_UNLISTED) return caller != FAULT_ADDRESS_OTHER_MODULE;
    return 1;
}

#endif
