/* Local controller ownership: which connected gamepad drives player N (pad handle N). A connected gamepad keeps its
 * player until it disconnects; a returning gamepad takes its old player back when that player is free; any other
 * gamepad takes the lowest free player. */
#ifndef DCP_PAD_OWNER_H
#define DCP_PAD_OWNER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DCP_PAD_OWNER_SLOTS 4
#define DCP_PAD_OWNER_KNOWN 8

typedef struct {
    uint32_t instance;  /* SDL instance id: unique while connected, new on every reconnect */
    uint64_t identity;  /* the same device across reconnects when the platform tells; 0 = unknown */
    int player_index;   /* SDL player index (the XInput user index), -1 = none */
} DcpPadDevice;

typedef enum {
    DCP_PAD_ASSIGN_NEW,     /* first time this device is seen, or unknown identity */
    DCP_PAD_ASSIGN_RECLAIM, /* a returning device took its previous player back */
    DCP_PAD_ASSIGN_MOVED,   /* a returning device found its previous player taken */
} DcpPadAssignReason;

typedef struct {
    int slot;
    DcpPadDevice device;
    DcpPadAssignReason reason;
    int previous_slot;  /* -1 unless reason is MOVED */
    int swapped_with;   /* the slot of the controller that took the other side of a proven exchange, else -1 */
    uint32_t generation;
} DcpPadAssignment;

typedef struct {
    struct {
        int used;
        DcpPadDevice device;
        int previous_slot;
    } active[DCP_PAD_OWNER_SLOTS];
    struct {
        uint64_t identity;
        int slot;
        uint32_t seen;
    } known[DCP_PAD_OWNER_KNOWN];
    uint32_t generation;
} DcpPadOwners;

void dcp_pad_owners_init(DcpPadOwners *owners);
int dcp_pad_owners_slot_of(const DcpPadOwners *owners, uint32_t instance);
int dcp_pad_owners_free_slot(const DcpPadOwners *owners);
void dcp_pad_owners_release(DcpPadOwners *owners, int slot);
/* Gives the devices of one scan that have no player yet a free player; returns how many assignments it wrote. */
size_t dcp_pad_owners_assign(DcpPadOwners *owners, const DcpPadDevice *devices, size_t count, DcpPadAssignment *out,
                             size_t capacity);

typedef enum {
    DCP_PAD_EVENT_ASSIGNMENT,   /* local_controller_assignment */
    DCP_PAD_EVENT_REASSIGNMENT, /* local_controller_reassignment */
    DCP_PAD_EVENT_SWAP,         /* local_controller_swap_detected */
    DCP_PAD_EVENT_COUNT
} DcpPadEvent;

typedef struct DcpPadTelemetry DcpPadTelemetry;

DcpPadTelemetry *dcp_pad_telemetry_new(void);
void dcp_pad_telemetry_free(DcpPadTelemetry *telemetry);
/* Counts the event for a player (1-4). Returns whether a log line is due (per event and player: the first, then every
 * tenth power) and writes it to `line`. */
int dcp_pad_telemetry_note(DcpPadTelemetry *telemetry, DcpPadEvent event, int player, char *line, size_t size);
#define DCP_PAD_TELEMETRY_LINE 112
/* Counts the assignment's events; writes the due lines (at most DCP_PAD_EVENT_COUNT) and returns how many. */
size_t dcp_pad_telemetry_lines(DcpPadTelemetry *telemetry, const DcpPadAssignment *assignment,
                               char (*lines)[DCP_PAD_TELEMETRY_LINE]);
/* The same in the process-wide telemetry, printing the due lines. */
void dcp_pad_telemetry_record(const DcpPadAssignment *assignment);

#ifdef __cplusplus
}
#endif

#endif
