#include "dcp_pointer_motion.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define BURST_GAP_NS 150000000ull
#define SUSTAIN_NS 200000000ull
#define SUSTAIN_EVENTS 8

static const char *const event_names[DCP_MOTION_EVENT_COUNT]={
    "pointer_motion_active","pointer_motion_while_button_held","pointer_motion_stall_while_button_held",
    "pointer_motion_recovered"};
static const char *const source_names[DCP_MOTION_SOURCE_COUNT]={"mouse","mouse_pointer","gamepad_axis"};

typedef struct {
    uint64_t start, last, events, stall_start;
    int active, held_reported, stalled;
} Burst;

struct DcpMotionTelemetry {
    atomic_flag lock;
    Burst burst[DCP_MOTION_SOURCE_COUNT];
    uint64_t last_read;
    uint64_t count[DCP_MOTION_EVENT_COUNT][DCP_MOTION_SOURCE_COUNT];
};

DcpMotionTelemetry *dcp_motion_telemetry_new(void) {
    DcpMotionTelemetry *t=calloc(1,sizeof(*t));
    if (t) atomic_flag_clear(&t->lock);
    return t;
}
void dcp_motion_telemetry_free(DcpMotionTelemetry *t) { free(t); }

static void lock(DcpMotionTelemetry *t) { while (atomic_flag_test_and_set_explicit(&t->lock,memory_order_acquire)) {} }
static void unlock(DcpMotionTelemetry *t) { atomic_flag_clear_explicit(&t->lock,memory_order_release); }

/* The first line and then every power of ten. */
static int line_due(uint64_t count) {
    while (count>=10 && count%10==0) count/=10;
    return count==1;
}

static const char *held_name(unsigned held) {
    switch (held & (DCP_MOTION_HELD_MOUSE|DCP_MOTION_HELD_GAMEPAD)) {
    case DCP_MOTION_HELD_MOUSE: return "mouse_button";
    case DCP_MOTION_HELD_GAMEPAD: return "gamepad_button";
    default: return "both";
    }
}

static const char *stall_bucket(uint64_t ns) {
    return ns<250000000ull ? "lt250ms" : ns<1000000000ull ? "lt1000ms" : "ge1000ms";
}

static size_t note(DcpMotionTelemetry *t, DcpMotionEvent event, DcpMotionSource source, const char *extra,
                   char lines[][DCP_MOTION_LINE_SIZE], size_t n) {
    const uint64_t count=++t->count[event][source];
    if (!line_due(count)) return n;
    snprintf(lines[n],DCP_MOTION_LINE_SIZE,"Pointer telemetry: event=%s source=%s%s count=%llu",event_names[event],
             source_names[source],extra,(unsigned long long)count);
    return n+1;
}

size_t dcp_motion_host(DcpMotionTelemetry *t, DcpMotionSource source, unsigned held, uint64_t now_ns,
                       char lines[][DCP_MOTION_LINE_SIZE]) {
    if (!t || source<0 || source>=DCP_MOTION_SOURCE_COUNT) return 0;
    size_t n=0;
    char extra[48];
    lock(t);
    Burst *b=&t->burst[source];
    if (!b->events || now_ns<b->last || now_ns-b->last>BURST_GAP_NS) {
        b->start=now_ns;
        b->events=0;
        b->active=b->held_reported=0;
    }
    b->last=now_ns;
    ++b->events;
    if (!b->active && now_ns-b->start>=SUSTAIN_NS && b->events>=SUSTAIN_EVENTS) {
        b->active=1;
        n=note(t,DCP_MOTION_EVENT_ACTIVE,source,"",lines,n);
    }
    if (b->active && held) {
        snprintf(extra,sizeof(extra)," held=%s",held_name(held));
        if (!b->held_reported) {
            b->held_reported=1;
            n=note(t,DCP_MOTION_EVENT_WHILE_HELD,source,extra,lines,n);
        }
        const int game_facing=source!=DCP_MOTION_SOURCE_MOUSE;
        if (game_facing && !b->stalled && t->last_read && now_ns>t->last_read &&
            now_ns-t->last_read>=DCP_MOTION_STALL_NS) {
            b->stalled=1;
            b->stall_start=t->last_read;
            n=note(t,DCP_MOTION_EVENT_STALL_WHILE_HELD,source,extra,lines,n);
        }
    }
    unlock(t);
    return n;
}

size_t dcp_motion_read(DcpMotionTelemetry *t, uint64_t now_ns, char lines[][DCP_MOTION_LINE_SIZE]) {
    if (!t) return 0;
    size_t n=0;
    char extra[32];
    lock(t);
    t->last_read=now_ns;
    for (int s=0;s<DCP_MOTION_SOURCE_COUNT;++s) {
        Burst *b=&t->burst[s];
        if (!b->stalled) continue;
        b->stalled=0;
        snprintf(extra,sizeof(extra)," stall=%s",stall_bucket(now_ns>b->stall_start ? now_ns-b->stall_start : 0));
        n=note(t,DCP_MOTION_EVENT_RECOVERED,(DcpMotionSource)s,extra,lines,n);
    }
    unlock(t);
    return n;
}

static DcpMotionTelemetry *global(void) {
    static _Atomic(DcpMotionTelemetry *) state;
    DcpMotionTelemetry *t=atomic_load(&state);
    if (t) return t;
    DcpMotionTelemetry *created=dcp_motion_telemetry_new(), *expected=NULL;
    if (!created) return NULL;
    if (atomic_compare_exchange_strong(&state,&expected,created)) return created;
    dcp_motion_telemetry_free(created);
    return expected;
}

static void print_lines(char lines[][DCP_MOTION_LINE_SIZE], size_t n) {
    for (size_t i=0;i<n;++i) puts(lines[i]);
}

void dcp_motion_record_host(DcpMotionSource source, unsigned held, uint64_t now_ns) {
    char lines[DCP_MOTION_MAX_LINES][DCP_MOTION_LINE_SIZE];
    print_lines(lines,dcp_motion_host(global(),source,held,now_ns,lines));
}

void dcp_motion_record_read(uint64_t now_ns) {
    char lines[DCP_MOTION_MAX_LINES][DCP_MOTION_LINE_SIZE];
    print_lines(lines,dcp_motion_read(global(),now_ns,lines));
}
