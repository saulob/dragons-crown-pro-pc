#include "dcp_mouse.h"
#include "dcp_pad_buttons.h"
#include <math.h>
#include <stdatomic.h>
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

typedef enum { PHASE_IDLE, PHASE_DOWN, PHASE_LIFTED } Phase;

struct DcpMouseState {
    atomic_flag lock;
    DcpMouseMap map;
    int blocked;
    int enabled;
    int down[DCP_MOUSE_BUTTON_COUNT];
    int consumed[DCP_MOUSE_BUTTON_COUNT];
    int latched; /* Pointer Control stays off until its button is released */
    int wheel_pulse[2]; /* up, down: reads left */
    float pending_x, pending_y;
    Phase phase;
    int recenter_due;
    float x, y;
    uint8_t id;
};

static void state_lock(DcpMouseState *s) { while (atomic_flag_test_and_set_explicit(&s->lock,memory_order_acquire)) {} }
static void state_unlock(DcpMouseState *s) { atomic_flag_clear_explicit(&s->lock,memory_order_release); }

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

static int valid_button(int button) { return button>0 && button<DCP_MOUSE_BUTTON_COUNT; }
static int pointer_button(const DcpMouseState *s) { return s->map.button[DCP_MOUSE_POINTER_CONTROL]; }
static int click_button(const DcpMouseState *s) { return s->map.button[DCP_MOUSE_POINTER_INTERACT]; }

static int pointer_active(const DcpMouseState *s) {
    const int button=pointer_button(s);
    return s->enabled && !s->blocked && !s->latched && valid_button(button) && s->down[button];
}

/* A button used by Pointer Control stays out of the normal actions until it is released. */
static void update_consumed(DcpMouseState *s) {
    if (!pointer_active(s)) return;
    const int pointer=pointer_button(s), click=click_button(s);
    if (valid_button(pointer) && s->down[pointer]) s->consumed[pointer]=1;
    if (valid_button(click) && s->down[click]) s->consumed[click]=1;
}

static void clear_held(DcpMouseState *s) {
    memset(s->down,0,sizeof(s->down));
    memset(s->consumed,0,sizeof(s->consumed));
    memset(s->wheel_pulse,0,sizeof(s->wheel_pulse));
    s->latched=0;
    s->pending_x=s->pending_y=0;
}

void dcp_mouse_configure(DcpMouseState *s, const DcpMouseMap *map) {
    state_lock(s);
    s->map=*map;
    s->enabled=map->controls!=0;
    dcp_mouse_normalize(&s->map);
    if (!dcp_pointer_sensitivity_valid(s->map.sensitivity)) s->map.sensitivity=DCP_POINTER_SENSITIVITY_DEFAULT;
    clear_held(s);
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
    state_unlock(s);
}

void dcp_mouse_to_touch_units(int percent, float game_dx, float game_dy, float *touch_dx, float *touch_dy) {
    const float sensitivity=(float)percent/100.0f;
    *touch_dx=game_dx*sensitivity/(CURSOR_GAIN*(GAME_WIDTH/(float)DCP_TOUCH_RES_X));
    *touch_dy=game_dy*sensitivity/(CURSOR_GAIN*(GAME_HEIGHT/(float)DCP_TOUCH_RES_Y));
}

static float clampf(float v, float lo, float hi) { return v<lo ? lo : v>hi ? hi : v; }

void dcp_mouse_motion(DcpMouseState *s, float game_dx, float game_dy) {
    state_lock(s);
    if (pointer_active(s)) {
        float dx, dy;
        dcp_mouse_to_touch_units(s->map.sensitivity,game_dx,game_dy,&dx,&dy);
        s->pending_x=clampf(s->pending_x+dx,-PENDING_LIMIT*DCP_TOUCH_RES_X,PENDING_LIMIT*DCP_TOUCH_RES_X);
        s->pending_y=clampf(s->pending_y+dy,-PENDING_LIMIT*DCP_TOUCH_RES_Y,PENDING_LIMIT*DCP_TOUCH_RES_Y);
    }
    state_unlock(s);
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
    state_unlock(s);
}

void dcp_mouse_set_sensitivity(DcpMouseState *s, int percent) {
    state_lock(s);
    s->map.sensitivity=dcp_pointer_sensitivity_normalize(percent);
    state_unlock(s);
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
    state_unlock(s);
}

void dcp_mouse_release_all(DcpMouseState *s) {
    state_lock(s);
    clear_held(s);
    state_unlock(s);
}

void dcp_mouse_controller_activity(DcpMouseState *s) {
    state_lock(s);
    if (pointer_active(s)) {
        s->latched=1;
        s->pending_x=s->pending_y=0;
    }
    state_unlock(s);
}

int dcp_mouse_pointer_active(DcpMouseState *s) {
    state_lock(s);
    const int active=pointer_active(s);
    state_unlock(s);
    return active;
}

static uint32_t wheel_bits(int target) {
    switch (target) {
    case DCP_WHEEL_CROSS: return BTN_CROSS;
    case DCP_WHEEL_CIRCLE: return BTN_CIRCLE;
    case DCP_WHEEL_SQUARE: return BTN_SQUARE;
    case DCP_WHEEL_TRIANGLE: return BTN_TRIANGLE;
    case DCP_WHEEL_L1: return BTN_L1;
    case DCP_WHEEL_R1: return BTN_R1;
    case DCP_WHEEL_L2: return BTN_L2;
    case DCP_WHEEL_R2: return BTN_R2;
    default: return 0;
    }
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

void dcp_mouse_sample(DcpMouseState *s, DcpMouseSample *out) {
    memset(out,0,sizeof(*out));
    state_lock(s);
    update_consumed(s);
    const int active=pointer_active(s);
    if (s->map.wheel_up!=DCP_WHEEL_NONE && s->wheel_pulse[0]>0) { out->buttons|=wheel_bits(s->map.wheel_up); --s->wheel_pulse[0]; }
    if (s->map.wheel_down!=DCP_WHEEL_NONE && s->wheel_pulse[1]>0) { out->buttons|=wheel_bits(s->map.wheel_down); --s->wheel_pulse[1]; }
    const int square=s->map.button[DCP_MOUSE_SQUARE], circle=s->map.button[DCP_MOUSE_CIRCLE];
    if (valid_button(square) && s->down[square] && !s->consumed[square]) out->buttons|=BTN_SQUARE;
    if (valid_button(circle) && s->down[circle] && !s->consumed[circle]) out->buttons|=BTN_CIRCLE;
    if (!active) {
        s->phase=PHASE_IDLE;
        s->pending_x=s->pending_y=0;
    } else {
        out->pointer=1;
        const int click=click_button(s);
        if (valid_button(click) && s->down[click]) out->buttons|=BTN_TOUCHPAD;
        if (s->phase==PHASE_DOWN && s->recenter_due) {
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
    state_unlock(s);
}

float dcp_mouse_game_scale(int window_width, int window_height) {
    if (window_width<=0 || window_height<=0) return 1.0f;
    float shown=(float)window_height;
    const float fitted=(float)window_width*9.0f/16.0f;
    if (fitted<shown) shown=fitted;
    return GAME_HEIGHT/shown;
}
