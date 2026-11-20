#include "dcp_mouse.h"
#include "dcp_pad_buttons.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_WIDTH 1920.0f
#define GAME_HEIGHT 1080.0f
/* The game's relative cursor scales finger movement by (Cursor Speed - 5) * 0.2 + 1: 0.6 at its default speed 3. */
#define CURSOR_GAIN 0.6f
#define WHEEL_PULSE_READS 4
/* The finger is recentred once it leaves the middle three fifths; a step is at most the fifth left on each side. */
#define MARGIN_X (DCP_TOUCH_RES_X/5)
#define MARGIN_Y (DCP_TOUCH_RES_Y/5)
#define PENDING_LIMIT 4.0f
#define MAX_LINES 3
#define LINE_SIZE 112

typedef enum { PHASE_IDLE, PHASE_DOWN, PHASE_LIFTED } Phase;

static const uint32_t target_buttons[DCP_PAD_TARGET_COUNT]={
    BTN_CROSS, BTN_CIRCLE, BTN_SQUARE, BTN_TRIANGLE, BTN_L1, BTN_R1, BTN_L2, BTN_R2, BTN_L3, BTN_R3,
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_OPTIONS, BTN_TOUCHPAD, BTN_TOUCHPAD,
};

struct DcpMouseState {
    atomic_flag lock;
    DcpMouseMap map;
    int blocked;
    int enabled;
    int down[DCP_MOUSE_BUTTON_COUNT];
    int consumed[DCP_MOUSE_BUTTON_COUNT];
    int latched; /* Hold: Pointer Control stays off until its button is released */
    int controller_surface, screen_surface; /* a controller finger or a screen contact holds the touch surface */
    int wheel_pulse[2]; /* up, down: reads left */
    float pending_x, pending_y;
    Phase phase;
    int recenter_due;
    float x, y;
    uint8_t id;
    /* Always On bookkeeping */
    int live;       /* the pointer was active at the last check */
    int suspended;  /* it was given up and has not come back */
    int used;       /* it has moved the pointer since it became active */
    int gap;        /* the next pointer read has no finger, so the game sees a landing and not a jump */
    int yield_gap;  /* the next read hides the controller's finger for the same reason */
    unsigned extended_down;
    uint64_t count[DCP_MOUSE_EVENT_COUNT][DCP_MOUSE_REASON_COUNT];
    DcpMouseLogSink sink;
    void *sink_user;
    char lines[MAX_LINES][LINE_SIZE];
    int line_count;
};

static void state_lock(DcpMouseState *s) { while (atomic_flag_test_and_set_explicit(&s->lock,memory_order_acquire)) {} }
static void state_unlock(DcpMouseState *s) { atomic_flag_clear_explicit(&s->lock,memory_order_release); }

static const char *const event_names[DCP_MOUSE_EVENT_COUNT]={
    "mouse_extended_binding_used", "pointer_control_always_on_used", "pointer_control_suspended", "pointer_control_resumed",
    "mouse_binding_invalid_fallback",
};
static const char *const reason_names[DCP_MOUSE_REASON_COUNT]={"","blocked","controller_touch","disabled"};

static int line_due(uint64_t count) {
    while (count>=10 && count%10==0) count/=10;
    return count==1;
}

static void queue_line(DcpMouseState *s, DcpMouseEvent event, DcpMouseReason reason, unsigned long long count) {
    if (s->line_count>=MAX_LINES) return;
    char *line=s->lines[s->line_count++];
    if (reason==DCP_MOUSE_REASON_NONE) snprintf(line,LINE_SIZE,"Mouse telemetry: event=%s count=%llu",event_names[event],count);
    else snprintf(line,LINE_SIZE,"Mouse telemetry: event=%s reason=%s count=%llu",event_names[event],reason_names[reason],count);
}

static void note(DcpMouseState *s, DcpMouseEvent event, DcpMouseReason reason) {
    const uint64_t count=++s->count[event][reason];
    if (line_due(count)) queue_line(s,event,reason,(unsigned long long)count);
}

/* Ends every public call: lines are delivered after the lock is released. */
static void leave(DcpMouseState *s) {
    if (!s->line_count) {
        state_unlock(s);
        return;
    }
    char lines[MAX_LINES][LINE_SIZE];
    const int count=s->line_count;
    memcpy(lines,s->lines,sizeof(lines));
    s->line_count=0;
    DcpMouseLogSink sink=s->sink;
    void *user=s->sink_user;
    state_unlock(s);
    for (int i=0;i<count;++i) {
        if (sink) sink(lines[i],user);
        else puts(lines[i]);
    }
}

DcpMouseState *dcp_mouse_state_new(void) {
    DcpMouseState *s=calloc(1,sizeof(*s));
    if (!s) return NULL;
    atomic_flag_clear(&s->lock);
    dcp_mouse_defaults(&s->map);
    s->enabled=1;
    return s;
}
void dcp_mouse_state_free(DcpMouseState *s) { free(s); }

DcpMouseState *dcp_mouse_global(void) {
    static _Atomic(DcpMouseState *) global;
    DcpMouseState *state=atomic_load(&global);
    if (state) return state;
    DcpMouseState *created=dcp_mouse_state_new(), *expected=NULL;
    if (!created) abort();
    if (atomic_compare_exchange_strong(&global,&expected,created)) return created;
    dcp_mouse_state_free(created);
    return expected;
}

void dcp_mouse_set_log_sink(DcpMouseState *s, DcpMouseLogSink sink, void *user) {
    state_lock(s);
    s->sink=sink;
    s->sink_user=user;
    state_unlock(s);
}

void dcp_mouse_note_invalid(DcpMouseState *s, int count) {
    if (count<1) return;
    state_lock(s);
    ++s->count[DCP_MOUSE_EVENT_BINDING_INVALID][DCP_MOUSE_REASON_NONE];
    queue_line(s,DCP_MOUSE_EVENT_BINDING_INVALID,DCP_MOUSE_REASON_NONE,(unsigned long long)(count>99 ? 99 : count));
    leave(s);
}

static int valid_button(int button) { return button>0 && button<DCP_MOUSE_BUTTON_COUNT; }
static int pointer_button(const DcpMouseState *s) { return s->map.button[DCP_MOUSE_POINTER_CONTROL]; }
static int click_button(const DcpMouseState *s) { return s->map.button[DCP_MOUSE_POINTER_INTERACT]; }
static int always_on(const DcpMouseState *s) { return s->map.pointer_mode==DCP_POINTER_MODE_ALWAYS_ON; }

static int pointer_active(const DcpMouseState *s) {
    if (!s->enabled || s->blocked) return 0;
    if (always_on(s)) return !s->controller_surface && !s->screen_surface;
    const int button=pointer_button(s);
    return !s->latched && valid_button(button) && s->down[button];
}

static DcpMouseReason loss_reason(const DcpMouseState *s) {
    if (!s->enabled) return DCP_MOUSE_REASON_DISABLED;
    if (s->blocked) return DCP_MOUSE_REASON_BLOCKED;
    return DCP_MOUSE_REASON_CONTROLLER_TOUCH;
}

/* Always On: tells when the pointer was given up or taken back, and arranges the finger-less read around each change. */
static void observe(DcpMouseState *s) {
    if (!always_on(s)) {
        s->live=s->suspended=s->used=s->gap=s->yield_gap=0;
        return;
    }
    const int now=pointer_active(s);
    if (now==s->live) return;
    s->live=now;
    s->used=0;
    if (now) {
        s->gap=1;
        if (s->suspended) {
            s->suspended=0;
            note(s,DCP_MOUSE_EVENT_POINTER_RESUMED,DCP_MOUSE_REASON_NONE);
        }
    } else {
        const DcpMouseReason reason=loss_reason(s);
        s->suspended=1;
        s->yield_gap=reason==DCP_MOUSE_REASON_CONTROLLER_TOUCH;
        note(s,DCP_MOUSE_EVENT_POINTER_SUSPENDED,reason);
    }
}

/* The buttons Pointer Control uses stay out of the normal actions while it owns them: Hold, its own button and the
 * interaction; Always On, the interaction only. */
static void update_consumed(DcpMouseState *s) {
    if (!pointer_active(s)) return;
    const int pointer=pointer_button(s), click=click_button(s);
    if (!always_on(s) && valid_button(pointer) && s->down[pointer]) s->consumed[pointer]=1;
    if (valid_button(click) && s->down[click]) s->consumed[click]=1;
}

static void clear_held(DcpMouseState *s) {
    memset(s->down,0,sizeof(s->down));
    memset(s->consumed,0,sizeof(s->consumed));
    memset(s->wheel_pulse,0,sizeof(s->wheel_pulse));
    s->latched=0;
    s->pending_x=s->pending_y=0;
    s->phase=PHASE_IDLE;
    s->recenter_due=0;
    s->extended_down=0;
}

void dcp_mouse_configure(DcpMouseState *s, const DcpMouseMap *map) {
    state_lock(s);
    s->map=*map;
    s->enabled=map->controls!=0;
    dcp_mouse_normalize(&s->map);
    if (!dcp_pointer_sensitivity_valid(s->map.sensitivity)) s->map.sensitivity=DCP_POINTER_SENSITIVITY_DEFAULT;
    if (s->map.pointer_mode<0 || s->map.pointer_mode>=DCP_POINTER_MODE_COUNT) s->map.pointer_mode=DCP_POINTER_MODE_HOLD;
    clear_held(s);
    s->live=s->suspended=s->used=s->gap=s->yield_gap=0;
    state_unlock(s);
}

void dcp_mouse_button(DcpMouseState *s, int button, int down) {
    if (!valid_button(button)) return;
    state_lock(s);
    if (s->blocked || !s->enabled) {
        s->down[button]=0;
        s->consumed[button]=0;
    } else {
        s->down[button]=down!=0;
        if (!down) {
            s->consumed[button]=0;
            if (button==pointer_button(s)) s->latched=0;
        }
        update_consumed(s);
    }
    leave(s);
}

void dcp_mouse_to_touch_units(int percent, float game_dx, float game_dy, float *touch_dx, float *touch_dy) {
    const float sensitivity=(float)percent/100.0f;
    *touch_dx=game_dx*sensitivity/(CURSOR_GAIN*(GAME_WIDTH/(float)DCP_TOUCH_RES_X));
    *touch_dy=game_dy*sensitivity/(CURSOR_GAIN*(GAME_HEIGHT/(float)DCP_TOUCH_RES_Y));
}

static float clampf(float v, float lo, float hi) { return v<lo ? lo : v>hi ? hi : v; }

void dcp_mouse_motion(DcpMouseState *s, float game_dx, float game_dy) {
    state_lock(s);
    observe(s);
    if (pointer_active(s)) {
        float dx, dy;
        dcp_mouse_to_touch_units(s->map.sensitivity,game_dx,game_dy,&dx,&dy);
        s->pending_x=clampf(s->pending_x+dx,-PENDING_LIMIT*DCP_TOUCH_RES_X,PENDING_LIMIT*DCP_TOUCH_RES_X);
        s->pending_y=clampf(s->pending_y+dy,-PENDING_LIMIT*DCP_TOUCH_RES_Y,PENDING_LIMIT*DCP_TOUCH_RES_Y);
        if (always_on(s) && !s->used && (game_dx!=0.0f || game_dy!=0.0f)) {
            s->used=1;
            note(s,DCP_MOUSE_EVENT_ALWAYS_ON_USED,DCP_MOUSE_REASON_NONE);
        }
    }
    leave(s);
}

void dcp_mouse_wheel(DcpMouseState *s, float steps) {
    if (steps==0.0f) return;
    state_lock(s);
    if (!s->blocked && s->enabled) {
        const int up=steps>0.0f;
        if ((up ? s->map.wheel_up : s->map.wheel_down)!=DCP_WHEEL_NONE) s->wheel_pulse[up ? 0 : 1]=WHEEL_PULSE_READS;
    }
    state_unlock(s);
}

void dcp_mouse_set_enabled(DcpMouseState *s, int enabled) {
    state_lock(s);
    s->enabled=enabled!=0;
    if (!s->enabled) clear_held(s);
    observe(s);
    leave(s);
}

void dcp_mouse_set_sensitivity(DcpMouseState *s, int percent) {
    state_lock(s);
    s->map.sensitivity=dcp_pointer_sensitivity_normalize(percent);
    state_unlock(s);
}

void dcp_mouse_set_pointer_mode(DcpMouseState *s, int mode) {
    if (mode<0 || mode>=DCP_POINTER_MODE_COUNT) return;
    state_lock(s);
    if (mode!=s->map.pointer_mode) {
        s->map.pointer_mode=mode;
        s->latched=0;
        s->pending_x=s->pending_y=0;
        update_consumed(s);
        observe(s);
    }
    leave(s);
}

int dcp_mouse_enabled(DcpMouseState *s) {
    state_lock(s);
    const int enabled=s->enabled;
    state_unlock(s);
    return enabled;
}

void dcp_mouse_block(DcpMouseState *s, int blocked) {
    state_lock(s);
    s->blocked=blocked!=0;
    if (s->blocked) clear_held(s);
    observe(s);
    leave(s);
}

void dcp_mouse_release_all(DcpMouseState *s) {
    state_lock(s);
    clear_held(s);
    if (always_on(s) && s->live) s->gap=1;
    observe(s);
    leave(s);
}

void dcp_mouse_controller_touch(DcpMouseState *s) {
    state_lock(s);
    if (always_on(s)) {
        s->controller_surface=1;
        s->pending_x=s->pending_y=0;
    } else if (pointer_active(s)) {
        s->latched=1;
        s->pending_x=s->pending_y=0;
    }
    observe(s);
    leave(s);
}

void dcp_mouse_controller_surface(DcpMouseState *s, int touching) {
    state_lock(s);
    s->controller_surface=touching!=0;
    if (touching && always_on(s)) s->pending_x=s->pending_y=0;
    observe(s);
    leave(s);
}

void dcp_mouse_screen_surface(DcpMouseState *s, int touching) {
    state_lock(s);
    s->screen_surface=touching!=0;
    if (touching && always_on(s)) s->pending_x=s->pending_y=0;
    observe(s);
    leave(s);
}

int dcp_mouse_pointer_active(DcpMouseState *s) {
    state_lock(s);
    const int active=pointer_active(s);
    state_unlock(s);
    return active;
}

int dcp_mouse_capture_wanted(DcpMouseState *s) {
    state_lock(s);
    const int wanted=s->enabled && (always_on(s) || pointer_active(s));
    state_unlock(s);
    return wanted;
}

int dcp_mouse_any_held(DcpMouseState *s) {
    state_lock(s);
    int held=0;
    for (int i=1;i<DCP_MOUSE_BUTTON_COUNT && !held;++i) held=s->down[i];
    state_unlock(s);
    return held;
}

static void press_target(DcpMouseSample *out, int target) {
    out->buttons|=target_buttons[target];
    if (target==DCP_PAD_TARGET_TOUCHPAD_LEFT && !out->touchpad_side) out->touchpad_side=1;
    if (target==DCP_PAD_TARGET_TOUCHPAD_RIGHT) out->touchpad_side=2;
}

static int touch_coordinate(float v, int max) { return (int)clampf(lroundf(v),0.0f,(float)max); }

static void place_finger(DcpMouseState *s, DcpMouseSample *out) {
    s->x=(float)(DCP_TOUCH_MAX_X/2);
    s->y=(float)(DCP_TOUCH_MAX_Y/2);
    s->id=(uint8_t)((s->id+1)&0x7f);
    s->phase=PHASE_DOWN;
    s->recenter_due=0;
    out->touch=1;
}

static void move_finger(DcpMouseState *s, DcpMouseSample *out) {
    const float step_x=clampf(s->pending_x,-(float)MARGIN_X,(float)MARGIN_X);
    const float step_y=clampf(s->pending_y,-(float)MARGIN_Y,(float)MARGIN_Y);
    s->pending_x-=step_x;
    s->pending_y-=step_y;
    s->x=clampf(s->x+step_x,0.0f,(float)DCP_TOUCH_MAX_X);
    s->y=clampf(s->y+step_y,0.0f,(float)DCP_TOUCH_MAX_Y);
    s->recenter_due=s->x<(float)MARGIN_X || s->x>(float)(DCP_TOUCH_MAX_X-MARGIN_X) ||
                    s->y<(float)MARGIN_Y || s->y>(float)(DCP_TOUCH_MAX_Y-MARGIN_Y);
    out->touch=1;
}

static void press_gameplay_buttons(DcpMouseState *s, DcpMouseSample *out) {
    unsigned extended=0;
    for (int action=0;action<DCP_MOUSE_ACTION_COUNT;++action) {
        const int target=dcp_mouse_actions[action].target, button=s->map.button[action];
        if (target<0 || !valid_button(button) || !s->down[button] || s->consumed[button]) continue;
        press_target(out,target);
        if (action>=DCP_MOUSE_FIRST_EXTENDED) extended|=1u<<action;
    }
    if (extended & ~s->extended_down) note(s,DCP_MOUSE_EVENT_EXTENDED_BINDING_USED,DCP_MOUSE_REASON_NONE);
    s->extended_down=extended;
}

void dcp_mouse_sample(DcpMouseState *s, DcpMouseSample *out) {
    memset(out,0,sizeof(*out));
    state_lock(s);
    observe(s);
    update_consumed(s);
    const int active=pointer_active(s);
    if (s->map.wheel_up!=DCP_WHEEL_NONE && s->wheel_pulse[0]>0) { press_target(out,s->map.wheel_up-1); --s->wheel_pulse[0]; }
    if (s->map.wheel_down!=DCP_WHEEL_NONE && s->wheel_pulse[1]>0) { press_target(out,s->map.wheel_down-1); --s->wheel_pulse[1]; }
    press_gameplay_buttons(s,out);
    if (!active) {
        s->phase=PHASE_IDLE;
        s->pending_x=s->pending_y=0;
        if (s->yield_gap) {
            s->yield_gap=0;
            out->pointer=1;
        }
    } else {
        out->pointer=1;
        const int click=click_button(s);
        if (valid_button(click) && s->down[click]) out->buttons|=BTN_TOUCHPAD;
        if (s->gap) {
            s->gap=0;
            s->phase=PHASE_LIFTED;
            s->recenter_due=0;
        } else if (s->phase==PHASE_DOWN && s->recenter_due) {
            s->phase=PHASE_LIFTED;
            s->recenter_due=0;
        } else if (s->phase==PHASE_DOWN) {
            move_finger(s,out);
        } else {
            place_finger(s,out);
        }
        if (out->touch) {
            out->x=(uint16_t)touch_coordinate(s->x,DCP_TOUCH_MAX_X);
            out->y=(uint16_t)touch_coordinate(s->y,DCP_TOUCH_MAX_Y);
            out->id=s->id;
        }
    }
    leave(s);
}

float dcp_mouse_game_scale(int window_width, int window_height) {
    if (window_width<=0 || window_height<=0) return 1.0f;
    float shown=(float)window_height;
    const float fitted=(float)window_width*9.0f/16.0f;
    if (fitted<shown) shown=fitted;
    return GAME_HEIGHT/shown;
}
