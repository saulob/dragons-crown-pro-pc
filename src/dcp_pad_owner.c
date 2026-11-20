#include "dcp_pad_owner.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dcp_pad_owners_init(DcpPadOwners *o) {
    memset(o,0,sizeof(*o));
    for (int i=0;i<DCP_PAD_OWNER_SLOTS;++i) o->active[i].previous_slot=-1;
    for (int i=0;i<DCP_PAD_OWNER_KNOWN;++i) o->known[i].slot=-1;
}

int dcp_pad_owners_slot_of(const DcpPadOwners *o, uint32_t instance) {
    for (int i=0;i<DCP_PAD_OWNER_SLOTS;++i) if (o->active[i].used && o->active[i].device.instance==instance) return i;
    return -1;
}

int dcp_pad_owners_free_slot(const DcpPadOwners *o) {
    for (int i=0;i<DCP_PAD_OWNER_SLOTS;++i) if (!o->active[i].used) return i;
    return -1;
}

void dcp_pad_owners_release(DcpPadOwners *o, int slot) {
    if (slot<0 || slot>=DCP_PAD_OWNER_SLOTS || !o->active[slot].used) return;
    o->active[slot].used=0;
    o->active[slot].previous_slot=-1;
    ++o->generation;
}

static int known_slot(const DcpPadOwners *o, uint64_t identity) {
    if (!identity) return -1;
    for (int i=0;i<DCP_PAD_OWNER_KNOWN;++i) if (o->known[i].identity==identity) return o->known[i].slot;
    return -1;
}

static void remember(DcpPadOwners *o, uint64_t identity, int slot) {
    if (!identity) return;
    int target=0;
    for (int i=0;i<DCP_PAD_OWNER_KNOWN;++i) {
        if (o->known[i].identity==identity) { target=i; break; }
        if (o->known[i].seen<o->known[target].seen) target=i;
    }
    o->known[target].identity=identity;
    o->known[target].slot=slot;
    o->known[target].seen=o->generation;
}

/* Two devices reporting the same identity cannot be told apart, so neither may claim a remembered player. */
static uint64_t usable_identity(const DcpPadOwners *o, const DcpPadDevice *devices, size_t count, size_t index) {
    const uint64_t identity=devices[index].identity;
    if (!identity) return 0;
    for (int i=0;i<DCP_PAD_OWNER_SLOTS;++i) if (o->active[i].used && o->active[i].device.identity==identity) return 0;
    for (size_t j=0;j<count;++j) if (j!=index && devices[j].identity==identity) return 0;
    return identity;
}

static void place(DcpPadOwners *o, const DcpPadDevice *device, uint64_t identity, int slot, DcpPadAssignReason reason,
                  int previous_slot, DcpPadAssignment *out) {
    ++o->generation;
    o->active[slot].used=1;
    o->active[slot].device=*device;
    o->active[slot].previous_slot=reason==DCP_PAD_ASSIGN_MOVED ? previous_slot : -1;
    remember(o,identity,slot);
    *out=(DcpPadAssignment){slot,*device,reason,o->active[slot].previous_slot,-1,o->generation};
    if (reason==DCP_PAD_ASSIGN_MOVED && o->active[previous_slot].used && o->active[previous_slot].previous_slot==slot)
        out->swapped_with=previous_slot;
}

static int comes_first(const DcpPadDevice *a, const DcpPadDevice *b) {
    if ((a->player_index>=0)!=(b->player_index>=0)) return a->player_index>=0;
    return a->player_index>=0 && a->player_index<b->player_index;
}

size_t dcp_pad_owners_assign(DcpPadOwners *o, const DcpPadDevice *devices, size_t count, DcpPadAssignment *out,
                             size_t capacity) {
    enum { MAX_CANDIDATES=16 };
    size_t order[MAX_CANDIDATES], candidates=0;
    for (size_t j=0;j<count && candidates<MAX_CANDIDATES;++j) {
        int seen=dcp_pad_owners_slot_of(o,devices[j].instance)>=0;
        for (size_t k=0;k<candidates && !seen;++k) seen=devices[order[k]].instance==devices[j].instance;
        if (seen) continue;
        size_t at=candidates++;
        while (at>0 && comes_first(&devices[j],&devices[order[at-1]])) { order[at]=order[at-1]; --at; }
        order[at]=j;
    }
    int placed[MAX_CANDIDATES]={0};
    size_t written=0;
    for (size_t k=0;k<candidates && written<capacity;++k) {
        const uint64_t identity=usable_identity(o,devices,count,order[k]);
        const int slot=known_slot(o,identity);
        if (slot<0 || o->active[slot].used) continue;
        place(o,&devices[order[k]],identity,slot,DCP_PAD_ASSIGN_RECLAIM,-1,&out[written++]);
        placed[k]=1;
    }
    for (size_t k=0;k<candidates && written<capacity;++k) {
        if (placed[k]) continue;
        const int slot=dcp_pad_owners_free_slot(o);
        if (slot<0) break;
        const uint64_t identity=usable_identity(o,devices,count,order[k]);
        const int previous=known_slot(o,identity);
        const DcpPadAssignReason reason=previous>=0 && previous!=slot ? DCP_PAD_ASSIGN_MOVED : DCP_PAD_ASSIGN_NEW;
        place(o,&devices[order[k]],identity,slot,reason,previous,&out[written++]);
    }
    return written;
}

static const char *const telemetry_names[DCP_PAD_EVENT_COUNT]={
    "local_controller_assignment","local_controller_reassignment","local_controller_swap_detected"};

struct DcpPadTelemetry { _Atomic uint64_t count[DCP_PAD_EVENT_COUNT][DCP_PAD_OWNER_SLOTS]; };

DcpPadTelemetry *dcp_pad_telemetry_new(void) { return calloc(1,sizeof(DcpPadTelemetry)); }
void dcp_pad_telemetry_free(DcpPadTelemetry *t) { free(t); }

static int line_due(uint64_t count) {
    while (count>=10 && count%10==0) count/=10;
    return count==1;
}

int dcp_pad_telemetry_note(DcpPadTelemetry *t, DcpPadEvent event, int player, char *line, size_t size) {
    if (event<0 || event>=DCP_PAD_EVENT_COUNT || player<1 || player>DCP_PAD_OWNER_SLOTS) return 0;
    const uint64_t count=atomic_fetch_add(&t->count[event][player-1],1)+1;
    if (!line_due(count)) return 0;
    snprintf(line,size,"Local controller telemetry: event=%s player=%d count=%llu",telemetry_names[event],player,
             (unsigned long long)count);
    return 1;
}

size_t dcp_pad_telemetry_lines(DcpPadTelemetry *t, const DcpPadAssignment *a, char (*lines)[DCP_PAD_TELEMETRY_LINE]) {
    size_t n=0;
    n+=(size_t)dcp_pad_telemetry_note(t,DCP_PAD_EVENT_ASSIGNMENT,a->slot+1,lines[n],DCP_PAD_TELEMETRY_LINE);
    if (a->reason==DCP_PAD_ASSIGN_MOVED)
        n+=(size_t)dcp_pad_telemetry_note(t,DCP_PAD_EVENT_REASSIGNMENT,a->slot+1,lines[n],DCP_PAD_TELEMETRY_LINE);
    if (a->swapped_with>=0) {
        const int player=(a->slot<a->swapped_with ? a->slot : a->swapped_with)+1;
        n+=(size_t)dcp_pad_telemetry_note(t,DCP_PAD_EVENT_SWAP,player,lines[n],DCP_PAD_TELEMETRY_LINE);
    }
    return n;
}

void dcp_pad_telemetry_record(const DcpPadAssignment *a) {
    static _Atomic(DcpPadTelemetry *) global;
    DcpPadTelemetry *t=atomic_load(&global);
    if (!t) {
        DcpPadTelemetry *created=dcp_pad_telemetry_new(), *expected=NULL;
        if (!created) return;
        if (atomic_compare_exchange_strong(&global,&expected,created)) t=created;
        else { dcp_pad_telemetry_free(created); t=expected; }
    }
    char lines[DCP_PAD_EVENT_COUNT][DCP_PAD_TELEMETRY_LINE];
    const size_t n=dcp_pad_telemetry_lines(t,a,lines);
    for (size_t i=0;i<n;++i) puts(lines[i]);
}
