#include "dcp_touchscreen.h"
#include "dcp_pad_buttons.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAP_MAX_NS 300000000ull
#define TAP_RADIUS (DCP_TOUCH_RES_X/32)
/* The game polls the pad once per frame, so the click is held for a few reads. */
#define CLICK_READS 3

typedef enum { CONTACT_NONE, CONTACT_DOWN, CONTACT_LIFTED } Contact;

struct DcpTouchscreen {
    atomic_flag lock;
    int blocked;
    Contact contact;
    uint64_t finger;
    uint64_t down_ns;
    uint16_t x, y, start_x, start_y;
    int moved;
    int shown;       /* reads that carried this contact */
    int click_reads; /* reads left holding the tap's click */
    uint8_t id;
};

static void state_lock(DcpTouchscreen *s) { while (atomic_flag_test_and_set_explicit(&s->lock,memory_order_acquire)) {} }
static void state_unlock(DcpTouchscreen *s) { atomic_flag_clear_explicit(&s->lock,memory_order_release); }

DcpTouchscreen *dcp_touchscreen_new(void) {
    DcpTouchscreen *s=calloc(1,sizeof(*s));
    if (s) atomic_flag_clear(&s->lock);
    return s;
}
void dcp_touchscreen_free(DcpTouchscreen *s) { free(s); }

DcpTouchscreen *dcp_touchscreen_global(void) {
    static _Atomic(DcpTouchscreen *) global;
    DcpTouchscreen *state=atomic_load(&global);
    if (state) return state;
    DcpTouchscreen *created=dcp_touchscreen_new(), *expected=NULL;
    if (!created) abort();
    if (atomic_compare_exchange_strong(&global,&expected,created)) return created;
    dcp_touchscreen_free(created);
    return expected;
}

DcpTouchArea dcp_touch_area_from_pixels(int x, int y, int width, int height, int surface_width, int surface_height) {
    const DcpTouchArea whole={0.0f,0.0f,1.0f,1.0f};
    if (width<=0 || height<=0 || surface_width<=0 || surface_height<=0) return whole;
    const float sw=(float)surface_width, sh=(float)surface_height;
    return (DcpTouchArea){(float)x/sw,(float)y/sh,(float)width/sw,(float)height/sh};
}

static float clamp01(float v) { return v<0.0f ? 0.0f : v>1.0f ? 1.0f : v; }

int dcp_touch_point(DcpTouchArea area, float window_x, float window_y, uint16_t *x, uint16_t *y) {
    if (!(area.width>0.0f) || !(area.height>0.0f)) area=(DcpTouchArea){0.0f,0.0f,1.0f,1.0f};
    const float u=(window_x-area.x)/area.width, v=(window_y-area.y)/area.height;
    const int inside=u>=0.0f && u<=1.0f && v>=0.0f && v<=1.0f;
    *x=(uint16_t)lroundf(clamp01(u)*(float)DCP_TOUCH_MAX_X);
    *y=(uint16_t)lroundf(clamp01(v)*(float)DCP_TOUCH_MAX_Y);
    return inside;
}

static void clear_contact(DcpTouchscreen *s) {
    s->contact=CONTACT_NONE;
    s->click_reads=0;
}

static void follow(DcpTouchscreen *s, DcpTouchArea area, float x, float y) {
    dcp_touch_point(area,x,y,&s->x,&s->y);
    if (abs((int)s->x-(int)s->start_x)>TAP_RADIUS || abs((int)s->y-(int)s->start_y)>TAP_RADIUS) s->moved=1;
}

void dcp_touchscreen_down(DcpTouchscreen *s, uint64_t finger, DcpTouchArea area, float x, float y, uint64_t time_ns) {
    uint16_t tx, ty;
    if (!dcp_touch_point(area,x,y,&tx,&ty)) return;
    state_lock(s);
    if (!s->blocked && s->contact!=CONTACT_DOWN) {
        s->contact=CONTACT_DOWN;
        s->finger=finger;
        s->down_ns=time_ns;
        s->x=s->start_x=tx;
        s->y=s->start_y=ty;
        s->moved=0;
        s->shown=0;
        s->click_reads=0;
        s->id=(uint8_t)((s->id+1)&0x7f);
    }
    state_unlock(s);
}

void dcp_touchscreen_motion(DcpTouchscreen *s, uint64_t finger, DcpTouchArea area, float x, float y) {
    state_lock(s);
    if (s->contact==CONTACT_DOWN && s->finger==finger) follow(s,area,x,y);
    state_unlock(s);
}

void dcp_touchscreen_up(DcpTouchscreen *s, uint64_t finger, DcpTouchArea area, float x, float y, uint64_t time_ns) {
    state_lock(s);
    if (s->contact==CONTACT_DOWN && s->finger==finger) {
        follow(s,area,x,y);
        const int tap=!s->moved && time_ns>=s->down_ns && time_ns-s->down_ns<=TAP_MAX_NS;
        s->contact=CONTACT_LIFTED;
        s->click_reads=tap ? CLICK_READS : 0;
    }
    state_unlock(s);
}

void dcp_touchscreen_cancel(DcpTouchscreen *s, uint64_t finger) {
    state_lock(s);
    if (s->contact!=CONTACT_NONE && s->finger==finger) clear_contact(s);
    state_unlock(s);
}

void dcp_touchscreen_block(DcpTouchscreen *s, int blocked) {
    state_lock(s);
    s->blocked=blocked!=0;
    if (s->blocked) clear_contact(s);
    state_unlock(s);
}

void dcp_touchscreen_release_all(DcpTouchscreen *s) {
    state_lock(s);
    clear_contact(s);
    state_unlock(s);
}

/* A lifted contact is still shown once if no read saw it, so a tap shorter than a frame moves the cursor before the
 * click lands. */
void dcp_touchscreen_sample(DcpTouchscreen *s, DcpTouchSample *out) {
    memset(out,0,sizeof(*out));
    state_lock(s);
    if (s->contact==CONTACT_LIFTED && s->shown>0) {
        if (s->click_reads>0) {
            --s->click_reads;
            out->buttons=BTN_TOUCHPAD;
        } else {
            clear_contact(s);
        }
    }
    if (s->contact!=CONTACT_NONE) {
        out->new_contact=s->shown==0;
        ++s->shown;
        out->touch=1;
        out->x=s->x;
        out->y=s->y;
        out->id=s->id;
    }
    state_unlock(s);
}

static const char *const telemetry_names[DCP_TOUCH_EVENT_COUNT]={
    "touchscreen_detected","touchscreen_touchpad_used","touchscreen_synthetic_mouse_suppressed"};

struct DcpTouchTelemetry { _Atomic uint64_t count[DCP_TOUCH_EVENT_COUNT]; };

DcpTouchTelemetry *dcp_touch_telemetry_new(void) { return calloc(1,sizeof(DcpTouchTelemetry)); }
void dcp_touch_telemetry_free(DcpTouchTelemetry *t) { free(t); }

static int line_due(uint64_t count) {
    while (count>=10 && count%10==0) count/=10;
    return count==1;
}

int dcp_touch_telemetry_note(DcpTouchTelemetry *t, DcpTouchEvent event, char *line, size_t size) {
    if (event<0 || event>=DCP_TOUCH_EVENT_COUNT) return 0;
    const uint64_t count=atomic_fetch_add(&t->count[event],1)+1;
    if (event==DCP_TOUCH_EVENT_DETECTED && count>1) return 0;
    if (!line_due(count)) return 0;
    snprintf(line,size,"Touchscreen telemetry: event=%s count=%llu",telemetry_names[event],(unsigned long long)count);
    return 1;
}

void dcp_touch_telemetry_record(DcpTouchEvent event) {
    static _Atomic(DcpTouchTelemetry *) global;
    DcpTouchTelemetry *t=atomic_load(&global);
    if (!t) {
        DcpTouchTelemetry *created=dcp_touch_telemetry_new(), *expected=NULL;
        if (!created) return;
        if (atomic_compare_exchange_strong(&global,&expected,created)) t=created;
        else { dcp_touch_telemetry_free(created); t=expected; }
    }
    char line[96];
    if (dcp_touch_telemetry_note(t,event,line,sizeof(line))) puts(line);
}
