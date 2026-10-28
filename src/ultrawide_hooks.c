#include "ultrawide_hooks.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "dcpgpu.h"

/* The video core provides the real one; recording it here instead keeps the guest-hook unit tests free of it while
 * still letting them see what was sent. */
static int debug_saulo_mask_wanted;

__attribute__((weak)) void dcpgpu_debug_saulo_ultrawide_mask(int wanted) {
    debug_saulo_mask_wanted = wanted;
}

/* DEBUG_SAULO: the last value sent to the final presenter's side mask, for the test build only. */
int ultrawide_debug_saulo_mask_wanted(void) {
    return debug_saulo_mask_wanted;
}

enum { PASSES_PER_VIEW = 0x2b, VERTEX_STRIDE = 0x20, MAX_FILL_VERTICES = 4 };

/* Passes 0x20-0x22 and 0x25-0x28: the ones the pass setup gives the 1280 x 720 canvas; 0x00-0x1f, 0x23, 0x24 and 0x2a
 * get the 812 x 456 world canvas. */
static const uint64_t kScreenPasses = 0x1e700000000ull;
static const uint64_t kWorldPasses = 0x418ffffffffull;
static const float kScreenHalfWidth = 640.0f, kScreenHalfHeight = 360.0f;

/* Same offset in every verified build: the camera task's per-frame update calls its view build. */
enum { CAMERA_VIEW_CALL = 0x118e9, VIEW_BUILD = 0x110b0 };
/* The frame commit's `mov -0x2c(%rbp),%eax; mov %al,0xb20(%rbx,%r13,1)`, right after it copied a pass into its set. */
static const uint8_t kCommitBytes[] = {0x8b, 0x45, 0xd4, 0x42, 0x88, 0x84, 0x2b, 0x20, 0x0b, 0x00, 0x00};

enum {
    CAMERA_CENTRE = 0x70,     /* float, the x the view is built around */
    CAMERA_VIEW_LEFT = 0x9c,  /* float, the 16:9 view's edges */
    CAMERA_VIEW_RIGHT = 0xa4,
    CAMERA_LEFT = 0xe4,       /* int32, the stage's horizontal limits for the view's edges */
    CAMERA_RIGHT = 0xe8,
    CAMERA_STAGE = 0x20,      /* its stage, whose view index (int32 at +0x74) picks the passes it sets */
    STAGE_VIEW = 0x74,
    STAGE_MAP = 0xb8,
    MAP_LAYERS = 0x368,       /* begin and end of the layer pointers */
    LAYER_PARALLAX = 0x348,   /* float, x */
    PASS_SIZE = 0xb30,
    PASS_PROJECTION = 0x100,  /* row-vector 4 x 4 */
    PASS_VIEW_PROJECTION = 0x1c0,
    PASS_VIEWPORT = 0x284,    /* float left, top, right, bottom as the executor programs it */
    PASS_SETS = 0x2b0,        /* three frame sets, each starting with a copy of the pass's first 0x2a8 bytes */
    PASS_SET_INDEX = 0xb20,
    SET_SIZE = 0x2d0,
    SET_BUCKETS = 0x2b0,      /* begin and end of the set's priority buckets, drawn in order */
    BUCKET_SIZE = 0x20,       /* begin and end of its command pointers */
    BUCKET_BEGIN = 0x8,
    BUCKET_END = 0x10,
    MAX_BUCKETS = 64,
    MAX_BAND_POLYGONS = 4096,
    PLAYFIELD_LAYER = 0x10,   /* stage layer 15, where the characters walk */
    ROUTE_MAP_PASS = 0x26,
    WORLDMAP_DRAW_SIZE = 0x270,
    MODEL_DRAW_PART_RETURN = 0xea, /* the model draw's return from each part */
    PART_LAYER = 0xc4,        /* s16 */
    COMMAND_MATRIX = 0x10,
    COMMAND_RESOURCE = 0x60,
    COMMAND_BOUNDS = 0x1a0,   /* model-local x0, y0, x1, y1 */
    COMMAND_KEYS = 0x1b0,     /* four keyframe records: first polygon s32 at +0x10, count u16 at +0x16 */
    RESOURCE_DATA = 0x158,
    DATA_POSITIONS = 0xc0,    /* six float x, y pairs per polygon */
    DATA_POLYGONS = 0xd0,     /* 16 bytes: flags at +5, position index u16 at +0xe */
    MODEL_CLIP = 0x150,       /* s16 */
    WIDGET_MODEL = 0x100,
    WIDGET_TYPE = 0xe0,
    WIDGET_DIALOGUE = 0x25,
    WIDGET_NARRATION = 0x26,
    MAP_MODEL = 0x100,
    MAP_FACE_MODEL = 0x3d0,
    FADE_MODEL = 0xb0,
};

/* The town map's highest marker sits at y -255 of the canvas's +-360. */
static const float kMapMarkerReach = 270.0f;

const UltrawideRecord ultrawide_records[ULTRAWIDE_RECORD_COUNT] = {
    {"CUSA10454 1.00 USA", {0x11f8060, 0x54c690}, 0x5811c0, {0x581ac3, 0x581bb6, 0x581869}, 0x5a209d, 0x597830,
     0x57530c, 0x579ac0, 0x4ec2ec, 0x541b89, 0x3a5055, 0x549a80, {0x44293b, 0x4430fb}},
    {"CUSA07001 1.00 Japan", {0x11e8060, 0x54a800}, 0x57f260, {0x57fb63, 0x57fc56, 0x57f909}, 0x5a012d, 0x5958d0,
     0x5733ac, 0x577b60, 0x4ea14c, 0x53fbe9, 0x3a4445, 0x547c20, {0x441b6b, 0x44232b}},
    {"CUSA07001 1.02 Japan", {0x11e8060, 0x54a660}, 0x57f190, {0x57fa93, 0x57fb86, 0x57f839}, 0x5a007d, 0x595800,
     0x5732dc, 0x577a90, 0x4ea6cc, 0x53fb69, 0x3a4885, 0x547a60, {0x441f4b, 0x44270b}},
    {"CUSA07001 1.03 Japan", {0x11e8060, 0x54a720}, 0x57f250, {0x57fb53, 0x57fc46, 0x57f8f9}, 0x5a012d, 0x5958c0,
     0x57339c, 0x577b50, 0x4ea78c, 0x53fc29, 0x3a48d5, 0x547b20, {0x44200b, 0x4427cb}},
    {"CUSA10209 1.00 Asia/Hong Kong", {0x1238060, 0x54a790}, 0x57f2c0, {0x57fbc3, 0x57fcb6, 0x57f969}, 0x5a018d,
     0x595930, 0x57340c, 0x577bc0, 0x4ea15c, 0x53fb79, 0x3a45b5, 0x547bb0, {0x441cdb, 0x44249b}},
};

static float widening = 1.0f;
static uint64_t image_base;
static const UltrawideRecord *installed;

void ultrawide_hooks_configure(float scale) {
    widening = scale;
}

int ultrawide_is_screen_pass(uint32_t layer) {
    return (int)(kScreenPasses >> (layer % PASSES_PER_VIEW) & 1);
}

int ultrawide_is_world_pass(uint32_t layer) {
    return (int)(kWorldPasses >> (layer % PASSES_PER_VIEW) & 1);
}

int ultrawide_widen_fill(uint8_t *vertices, uint32_t count, size_t stride, float scale) {
    if (count == 0 || count > MAX_FILL_VERTICES) return 0;
    float x[MAX_FILL_VERTICES];
    float left = 0.0f, right = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
        memcpy(&x[i], vertices + i * stride, sizeof(float));
        if (i == 0 || x[i] < left) left = x[i];
        if (i == 0 || x[i] > right) right = x[i];
    }
    if (left > 0.5f - kScreenHalfWidth || right < kScreenHalfWidth - 0.5f) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const float widened = x[i] * scale;
        memcpy(vertices + i * stride, &widened, sizeof(float));
    }
    return 1;
}

static int bounded(int32_t left, int32_t right) {
    return left > -1000000 && right < 1000000 && right > left;
}

UltrawideSpan ultrawide_centre_span(float scale) {
    const float half = scale > 1.0f ? 0.5f / scale : 0.5f;
    const UltrawideSpan span = {0.5f - half, 0.5f + half};
    return span;
}

UltrawideSpan ultrawide_limits_span(float centre, float view_width, float scale, int32_t left, int32_t right) {
    UltrawideSpan span = {0.0f, 1.0f};
    if (!(scale > 1.0f) || !(view_width > 0.0f) || !bounded(left, right)) return span;
    const float wanted = view_width * scale;
    span.left = fmaxf(0.0f, 0.5f + ((float)left - centre) / wanted);
    span.right = fminf(1.0f, 0.5f + ((float)right - centre) / wanted);
    return span;
}

UltrawideSpan ultrawide_frame_span(UltrawideSpan limits, const UltrawideSpan *playfield, float map_share, float scale) {
    UltrawideSpan span = limits;
    if (playfield) {
        span.left = fmaxf(span.left, playfield->left);
        span.right = fminf(span.right, playfield->right);
    }
    const UltrawideSpan centre = ultrawide_centre_span(scale);
    span.left = fminf(span.left, centre.left);
    span.right = fmaxf(span.right, centre.right);
    if (map_share < 1.0f) {
        span.left = fmaxf(span.left, 0.5f - map_share * 0.5f);
        span.right = fminf(span.right, 0.5f + map_share * 0.5f);
    }
    span.left = fmaxf(span.left, 0.0f);
    span.right = fminf(span.right, 1.0f);
    return span;
}

int ultrawide_clip_pass(float viewport[4], float projection[16], float view_projection[16], UltrawideSpan span) {
    const float x0 = viewport[0], x1 = viewport[2], width = x1 - x0;
    if (!(width > 0.0f) || (span.left <= 0.0f && span.right >= 1.0f)) return 0;
    /* The executor truncates the viewport to integers; the compensation must match the pixels it programs. */
    const float a = roundf(x0 + width * fmaxf(span.left, 0.0f)), b = roundf(x0 + width * fminf(span.right, 1.0f));
    if (!(b > a) || (a <= x0 && b >= x1)) return 0;
    const float gain = width / (b - a), offset = (x0 + x1 - a - b) / (b - a);
    for (int row = 0; row < 4; ++row) {
        projection[row * 4] = projection[row * 4] * gain + projection[row * 4 + 3] * offset;
        view_projection[row * 4] = view_projection[row * 4] * gain + view_projection[row * 4 + 3] * offset;
    }
    viewport[0] = a;
    viewport[2] = b;
    return 1;
}

int ultrawide_layer_follows_playfield(float parallax, float playfield) {
    return parallax >= fminf(0.999f, playfield - 0.001f);
}

UltrawideSpan ultrawide_world_pass_span(uint32_t id, const UltrawideWorldFrame *world, float scale) {
    UltrawideSpan span = {0.0f, 1.0f};
    const uint32_t pass = id % PASSES_PER_VIEW;
    if (world->route_map && (pass == ROUTE_MAP_PASS || (pass && ultrawide_is_world_pass(pass))))
        return ultrawide_centre_span(scale);
    if (!pass || !ultrawide_is_world_pass(pass)) return span;
    if (world->illustration) return ultrawide_centre_span(scale);
    const float playfield = world->parallax_known >> PLAYFIELD_LAYER & 1 ? world->parallax[PLAYFIELD_LAYER] : 1.0f;
    if (world->parallax_known >> pass & 1 && ultrawide_layer_follows_playfield(world->parallax[pass], playfield))
        span = world->playfield;
    if (world->map_share < 1.0f) {
        span.left = fmaxf(span.left, 0.5f - world->map_share * 0.5f);
        span.right = fminf(span.right, 0.5f + world->map_share * 0.5f);
    }
    return span;
}

int ultrawide_covers_view(float left, float right, float bottom, float top, float scale) {
    const float half = scale > 1.0f ? 1.0f / scale : 1.0f;
    return left <= -0.98f * half && right >= 0.98f * half && top - bottom >= 1.5f;
}

int ultrawide_clip_rows(float viewport[4], float projection[16], float view_projection[16], float reach) {
    const float y0 = viewport[1], y1 = viewport[3], height = y1 - y0;
    if (!(height > 0.0f) || !(reach > 0.0f) || reach >= 1.0f) return 0;
    const float a = roundf(y0 + height * (1.0f - reach) * 0.5f), b = roundf(y0 + height * (1.0f + reach) * 0.5f);
    if (!(b > a)) return 0;
    const float gain = height / (b - a);
    for (int row = 0; row < 4; ++row) {
        projection[row * 4 + 1] *= gain;
        view_projection[row * 4 + 1] *= gain;
    }
    viewport[1] = a;
    viewport[3] = b;
    return 1;
}

void ultrawide_cover_view(float matrix[16], float factor) {
    for (int row = 0; row < 4; ++row) {
        matrix[row * 4] *= factor;
        matrix[row * 4 + 1] *= factor;
    }
}

float ultrawide_world_share(float view_width, float scale, int32_t left, int32_t right) {
    if (!(scale > 1.0f) || !(view_width > 0.0f) || !bounded(left, right)) return 1.0f;
    const float wanted = view_width * scale, width = (float)right - (float)left;
    if (width >= wanted) return 1.0f;
    if (width <= view_width + 2.0f) return 1.0f / scale;
    return width / wanted;
}

int ultrawide_spans_screen(float left, float right) {
    return left <= 1.0f - kScreenHalfWidth && right >= kScreenHalfWidth - 1.0f;
}

int ultrawide_widget_widens(uint32_t type, float reach) {
    return type == WIDGET_NARRATION || (type == WIDGET_DIALOGUE && reach >= kScreenHalfHeight - 10.0f);
}

void ultrawide_stretch_part(float matrix[16], float factor) {
    matrix[0] *= factor;
    matrix[4] *= factor;
    matrix[8] *= factor;
}

void ultrawide_zoom_part(float matrix[16], float factor) {
    for (int row = 0; row < 4; ++row) {
        matrix[row * 4] *= factor;
        matrix[row * 4 + 1] *= factor;
    }
}

float ultrawide_widget_stretch(float scale, float window) {
    return fmaxf(1.0f, scale * window);
}

float ultrawide_map_zoom(float scale) {
    return fmaxf(1.0f, fminf(scale, 360.0f / kMapMarkerReach));
}

int ultrawide_extend_band(float u[6], const float v[6], float half, float scale) {
    if (!(scale > 1.0f) || !(half > 0.0f)) return 0;
    float left = u[0], right = u[0], bottom = v[0], top = v[0];
    for (int i = 1; i < 6; ++i) {
        left = fminf(left, u[i]);
        right = fmaxf(right, u[i]);
        bottom = fminf(bottom, v[i]);
        top = fmaxf(top, v[i]);
    }
    /* Within 0.5% of the 16:9 edge, clear of the title plates' ornaments; a side already at or past the wider frame's
     * edge was extended before. */
    const float edge = 0.995f * half, wide_edge = edge * scale;
    const int to_left = left <= -edge && left > -wide_edge, to_right = right >= edge && right < wide_edge;
    if (!to_left && !to_right) return 0;
    if (right - left < top - bottom) {
        if (to_left && to_right) return 0;
        const float shift = (to_right ? right : left) * (scale - 1.0f);
        for (int i = 0; i < 6; ++i) u[i] += shift;
        return 1;
    }
    for (int i = 0; i < 6; ++i)
        if ((to_left && u[i] <= -edge) || (to_right && u[i] >= edge)) u[i] *= scale;
    return 1;
}

int ultrawide_is_matte_band(const float u[6], const float y[6], float half) {
    float left = u[0], right = u[0], bottom = y[0], top = y[0];
    for (int i = 1; i < 6; ++i) {
        left = fminf(left, u[i]);
        right = fmaxf(right, u[i]);
        bottom = fminf(bottom, y[i]);
        top = fmaxf(top, y[i]);
    }
    const float edge = 0.995f * half;
    return left <= -edge && right >= edge && top - bottom <= 0.5f && (top >= 0.98f || bottom <= -0.98f);
}

static void load(void *to, const void *from, size_t bytes) {
    memcpy(to, from, bytes);
}

static uint64_t pointer_at(uint64_t address) {
    uint64_t value;
    load(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

/* DEBUG_SAULO: the first widened fill of each pass, for the manual test. */
static atomic_ullong debug_saulo_fill_passes;

static void debug_saulo_fill(uint32_t layer) {
    const unsigned long long bit = 1ull << (layer % PASSES_PER_VIEW);
    if (!(atomic_fetch_or(&debug_saulo_fill_passes, bit) & bit))
        printf("DEBUG_SAULO true ultrawide fill: pass 0x%x\n", layer % PASSES_PER_VIEW);
}

/* At a quad builder's call to the submit: rdi = layer, rdx = the built vertices, ecx = their count, r8 = a model
 * matrix, r9 = a texture. Only untextured canvas-space quads are fills. */
void GUEST_HOOK_ABI ultrawide_fill_hook(GuestRegs *regs) {
    if (regs->r8 || regs->r9 || !ultrawide_is_screen_pass((uint32_t)regs->rdi)) return;
    if (ultrawide_widen_fill((uint8_t *)(uintptr_t)regs->rdx, (uint32_t)regs->rcx, VERTEX_STRIDE, widening))
        debug_saulo_fill((uint32_t)regs->rdi);
}

/* What the frame commit needs from the task walk that built the frame; the walk and the commit run on the game's
 * task thread. */
static struct {
    uint64_t camera;
    float centre, view_width;
    int32_t left, right;
    float parallax[PASSES_PER_VIEW];
    uint64_t parallax_known; /* one bit per pass id */
    int playfield;
    float playfield_left, playfield_right; /* world x */
    int route_map, town_map;
    uint64_t backdrop_models[PASSES_PER_VIEW]; /* by pass id: the model of its first backdrop */
    float backdrop_y[PASSES_PER_VIEW][2];      /* DEBUG_SAULO: its clip y bottom and top */
    uint64_t backdrop_sets[PASSES_PER_VIEW];   /* by pass id: the set a backdrop of the next pass was moved into */
    float backdrop_reach[PASSES_PER_VIEW];     /* its |clip y| reach before the cover */
    uint64_t frame_sets[PASSES_PER_VIEW];      /* by pass id: the set the screen frame of the pass before went to */
    uint64_t used_passes;                      /* one bit per pass id the game queued a part in */
    uint64_t matte_passes;                     /* one bit per pass id whose black matte went to the frame set */
    int fade_active;                           /* the loading or saving fade drew this frame */
} frame;

static struct {
    UltrawideSpan playfield;
    float parallax[PASSES_PER_VIEW];
    uint64_t parallax_known;
    float map_share;
    int route_map, illustration;
    uint64_t backdrop_sets[PASSES_PER_VIEW];
    float backdrop_reach[PASSES_PER_VIEW];
    uint64_t frame_sets[PASSES_PER_VIEW];
    uint64_t used_passes;
} committed = {{0.0f, 1.0f}, {0.0f}, 0, 1.0f, 0, 0, {0}, {0.0f}, {0}, 0};

static float world_window = 1.0f; /* the bottom bands' share of the widening */
static uint32_t camera_frames, map_frame; /* map_frame: camera_frames + 1 when the map last drew, 0 before */

/* Stage layer i draws in pass i + 1, moving with the camera by its parallax. */
static void read_layers(uint64_t stage) {
    const uint64_t map = pointer_at(stage + STAGE_MAP);
    const uint64_t begin = map ? pointer_at(map + MAP_LAYERS) : 0, end = map ? pointer_at(map + MAP_LAYERS + 8) : 0;
    if (!begin || end < begin) return;
    for (uint64_t i = 0; i < (end - begin) / 8 && i + 1 < PASSES_PER_VIEW; ++i) {
        const uint64_t layer = pointer_at(begin + i * 8);
        if (!layer) continue;
        load(&frame.parallax[i + 1], (const void *)(uintptr_t)(layer + LAYER_PARALLAX), sizeof(float));
        frame.parallax_known |= 1ull << (i + 1);
    }
}

/* At the camera update's call of its view build: rdi = the camera task. Reads the stage limits and its layers'
 * parallax; nothing changes. The frame's windows follow the camera of the main view only. */
void GUEST_HOOK_ABI ultrawide_camera_hook(GuestRegs *regs) {
    const uint8_t *camera = (const uint8_t *)(uintptr_t)regs->rdi;
    if (!installed || !camera) return;
    float centre, view_left, view_right;
    int32_t left, right;
    load(&centre, camera + CAMERA_CENTRE, 4);
    load(&view_left, camera + CAMERA_VIEW_LEFT, 4);
    load(&view_right, camera + CAMERA_VIEW_RIGHT, 4);
    load(&left, camera + CAMERA_LEFT, 4);
    load(&right, camera + CAMERA_RIGHT, 4);
    float share = ultrawide_world_share(view_right - view_left, widening, left, right);
    if (map_frame && camera_frames + 1 - map_frame <= 1) share = fminf(share, ultrawide_map_zoom(widening) / widening);
    ++camera_frames;
    world_window = share;
    const uint64_t stage = pointer_at(regs->rdi + CAMERA_STAGE);
    int32_t view = -1;
    if (stage) load(&view, (const void *)(uintptr_t)(stage + STAGE_VIEW), sizeof(view));
    if (view != 0) return;
    frame.camera = regs->rdi;
    frame.centre = centre;
    frame.view_width = view_right - view_left;
    frame.left = left;
    frame.right = right;
    read_layers(stage);
}

static int clipped_passes(void) {
    int count = 0;
    for (uint32_t id = 1; id < PASSES_PER_VIEW; ++id) {
        const UltrawideWorldFrame world = {committed.playfield, committed.parallax, committed.parallax_known,
                                           committed.map_share, committed.route_map, committed.illustration};
        const UltrawideSpan span = ultrawide_world_pass_span(id, &world, widening);
        count += ultrawide_is_world_pass(id) && (span.left > 0.0f || span.right < 1.0f);
    }
    return count;
}

/* DEBUG_SAULO: one line whenever the frame's world policy changes, for the manual test. */
static void debug_saulo_world(const UltrawideSpan *playfield, uint64_t passes) {
    static int last = -1;
    const int clipped = clipped_passes();
    const int mode = committed.route_map ? 0 : committed.illustration ? 1 : clipped ? 2 : 3;
    if (mode == last) return;
    last = mode;
    static const char *const names[] = {"route map 16:9", "fixed screen 16:9, backdrop behind", "background full",
                                        "full"};
    float viewport[4];
    load(viewport, (const void *)(uintptr_t)(passes + PASS_SIZE + PASS_VIEWPORT), sizeof(viewport));
    const float width = viewport[2] - viewport[0];
    printf("DEBUG_SAULO true ultrawide world: %s, camera %#llx, limits %d..%d", names[mode],
           (unsigned long long)frame.camera, frame.left, frame.right);
    if (mode == 2)
        printf(", playfield and foreground clipped to x %.0f..%.0f (%d passes)%s",
               viewport[0] + width * committed.playfield.left, viewport[0] + width * committed.playfield.right, clipped,
               playfield ? "" : ", no playfield layer");
    if (mode >= 2 && committed.parallax_known) {
        printf(", parallax");
        for (uint32_t id = 1; id < PASSES_PER_VIEW; ++id)
            if (committed.parallax_known >> id & 1) printf(" 0x%02x:%.2f", id, committed.parallax[id]);
    }
    printf("\n");
}

enum { FIXED_GAMEPLAY, FIXED_FALLBACK, FIXED_STABLE };

static unsigned debug_saulo_transition_lines; /* DEBUG_SAULO */

unsigned ultrawide_debug_saulo_transition_lines(void) {
    return debug_saulo_transition_lines;
}

/* DEBUG_SAULO: once whenever the fixed-screen transition state changes, for the manual test. */
static void debug_saulo_fixed_transition(int state) {
    static int last = FIXED_GAMEPLAY;
    static const char *const messages[] = {"gameplay resumed", "centred 16:9 fallback entered",
                                            "stable ultrawide resumed"};
    if (state == last) return;
    last = state;
    ++debug_saulo_transition_lines;
    printf("DEBUG_SAULO true ultrawide fixed transition: %s\n", messages[state]);
}

/* DEBUG_SAULO: once when the final presenter's side mask turns on, for the manual test. */
static void debug_saulo_side_mask(int fade_active) {
    static int last;
    if (fade_active == last) return;
    last = fade_active;
    if (fade_active) printf("DEBUG_SAULO true ultrawide final side mask: fade active\n");
}

static int fixed_transition_active;

/* 1 while the screen is a one-screen illustration with no stable fixed backdrop yet (entering from gameplay) or no
 * longer (leaving a stable illustration): the centred 16:9 fallback with black sides belongs on these frames. */
int ultrawide_fixed_transition_active(void) {
    return fixed_transition_active;
}

/* The first pass of a frame commit: the frame's windows from everything its walk saw. */
static void settle_frame(uint64_t passes) {
    UltrawideSpan limits = {0.0f, 1.0f}, playfield;
    if (frame.camera)
        limits = ultrawide_limits_span(frame.centre, frame.view_width, widening, frame.left, frame.right);
    if (frame.playfield) {
        float vp[16];
        load(vp, (const void *)(uintptr_t)(passes + PLAYFIELD_LAYER * PASS_SIZE + PASS_VIEW_PROJECTION), sizeof(vp));
        const float a = (frame.playfield_left * vp[0] + vp[12] + 1.0f) * 0.5f;
        const float b = (frame.playfield_right * vp[0] + vp[12] + 1.0f) * 0.5f;
        playfield.left = fminf(a, b);
        playfield.right = fmaxf(a, b);
    }
    committed.playfield = ultrawide_frame_span(limits, frame.playfield ? &playfield : NULL, 1.0f, widening);
    memcpy(committed.parallax, frame.parallax, sizeof(committed.parallax));
    committed.parallax_known = frame.parallax_known;
    committed.map_share = frame.town_map ? ultrawide_map_zoom(widening) / widening : 1.0f;
    committed.route_map = frame.route_map;
    committed.illustration =
        frame.camera && !frame.playfield &&
        ultrawide_world_share(frame.view_width, widening, frame.left, frame.right) <= 1.0f / widening + 1e-4f;
    memcpy(committed.backdrop_sets, frame.backdrop_sets, sizeof(committed.backdrop_sets));
    memcpy(committed.backdrop_reach, frame.backdrop_reach, sizeof(committed.backdrop_reach));
    memcpy(committed.frame_sets, frame.frame_sets, sizeof(committed.frame_sets));
    committed.used_passes = frame.used_passes;
    debug_saulo_world(frame.playfield ? &playfield : NULL, passes);
    if (committed.route_map) {
        static int debug_saulo_route; /* DEBUG_SAULO */
        if (!debug_saulo_route++) printf("DEBUG_SAULO true ultrawide route map: clipped to the centred 16:9\n");
    }
    int any_backdrop = 0;
    for (uint32_t id = 0; id < PASSES_PER_VIEW; ++id) any_backdrop |= frame.backdrop_sets[id] != 0;
    fixed_transition_active = committed.illustration && !any_backdrop;
    debug_saulo_fixed_transition(!committed.illustration ? FIXED_GAMEPLAY
                                 : any_backdrop            ? FIXED_STABLE
                                                            : FIXED_FALLBACK);
    debug_saulo_side_mask(frame.fade_active);
    dcpgpu_debug_saulo_ultrawide_mask(frame.fade_active || fixed_transition_active);
    memset(&frame, 0, sizeof(frame));
}

static void copy_matrices(uint8_t *set, uint64_t source) {
    load(set + PASS_PROJECTION, (const void *)(uintptr_t)(source + PASS_PROJECTION),
         PASS_VIEWPORT - PASS_PROJECTION - 4);
}

/* The set that took a backdrop draws just before the backdrop's own pass: it gets that pass's view and a cover scale,
 * kept to the backdrop's rows so the screen's top and bottom framing stays; the backdrop's pass itself keeps the
 * illustration's 16:9. */
static void place_backdrop(uint8_t *set, uint64_t source, float reach) {
    float projection[16], view_projection[16];
    copy_matrices(set, source);
    load(projection, set + PASS_PROJECTION, sizeof(projection));
    load(view_projection, set + PASS_VIEW_PROJECTION, sizeof(view_projection));
    float viewport[4];
    load(viewport, set + PASS_VIEWPORT, sizeof(viewport));
    ultrawide_cover_view(projection, widening);
    ultrawide_cover_view(view_projection, widening);
    ultrawide_clip_rows(viewport, projection, view_projection, reach);
    memcpy(set + PASS_VIEWPORT, viewport, sizeof(viewport));
    memcpy(set + PASS_PROJECTION, projection, sizeof(projection));
    memcpy(set + PASS_VIEW_PROJECTION, view_projection, sizeof(view_projection));
}

/* In the frame commit, just after pass rbx + r13 was copied into its current set: that copy is what the executor
 * draws from, so the windows change this frame only and never the game's own pass. */
void GUEST_HOOK_ABI ultrawide_commit_hook(GuestRegs *regs) {
    const uint64_t passes = regs->rbx, offset = regs->r13;
    if (!installed || !passes || offset % PASS_SIZE) return;
    const uint32_t layer = (uint32_t)(offset / PASS_SIZE), id = layer % PASSES_PER_VIEW;
    if (layer == 0) settle_frame(passes);
    uint8_t *pass = (uint8_t *)(uintptr_t)(passes + offset);
    if (pass[PASS_SET_INDEX] > 2) return;
    uint8_t *set = pass + PASS_SETS + pass[PASS_SET_INDEX] * SET_SIZE;
    if (id + 1 < PASSES_PER_VIEW && committed.backdrop_sets[id] == (uint64_t)(uintptr_t)set) {
        place_backdrop(set, (uint64_t)(uintptr_t)pass + PASS_SIZE, committed.backdrop_reach[id]);
        return;
    }
    if (id > 0 && committed.frame_sets[id] == (uint64_t)(uintptr_t)set) {
        copy_matrices(set, (uint64_t)(uintptr_t)pass - PASS_SIZE); /* unzoomed, the whole frame */
        return;
    }
    const UltrawideWorldFrame world = {committed.playfield, committed.parallax, committed.parallax_known,
                                       committed.map_share, committed.route_map, committed.illustration};
    const UltrawideSpan span = ultrawide_world_pass_span(id, &world, widening);
    float viewport[4], projection[16], view_projection[16];
    load(viewport, set + PASS_VIEWPORT, sizeof(viewport));
    load(projection, set + PASS_PROJECTION, sizeof(projection));
    load(view_projection, set + PASS_VIEW_PROJECTION, sizeof(view_projection));
    if (!ultrawide_clip_pass(viewport, projection, view_projection, span)) return;
    memcpy(set + PASS_VIEWPORT, viewport, sizeof(viewport));
    memcpy(set + PASS_PROJECTION, projection, sizeof(projection));
    memcpy(set + PASS_VIEW_PROJECTION, view_projection, sizeof(view_projection));
}

/* Parts. */
static uint64_t town_map;

typedef struct {
    uint64_t model, vtable;
} FadeModel;

static FadeModel fades[ULTRAWIDE_FADES];

/* Model positions the band rule wrote are never extended twice, whatever the part's matrix by then; a reloaded model no
 * longer holds what was written and is extended again. */
typedef struct {
    float *xy;
    float written[12];
} BandRecord;

static BandRecord band_records[256];
static size_t band_record_count;

/* Model-local x, y to clip x, and to clip y in clip x units. */
typedef struct {
    float x[3], y[3];
} ScreenMap;

static const float kIdentity[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

static ScreenMap canvas_map(const float matrix[16]) {
    const float unit = 1.0f / (kScreenHalfWidth * widening);
    const ScreenMap map = {{matrix[0] * unit, matrix[4] * unit, matrix[12] * unit},
                           {matrix[1] * unit, matrix[5] * unit, matrix[13] * unit}};
    return map;
}

static ScreenMap world_map(const float matrix[16], const float vp[16]) {
    const float y_unit = 9.0f / (16.0f * widening);
    const ScreenMap map = {
        {matrix[0] * vp[0] + matrix[1] * vp[4], matrix[4] * vp[0] + matrix[5] * vp[4],
         matrix[12] * vp[0] + matrix[13] * vp[4] + vp[12]},
        {(matrix[0] * vp[1] + matrix[1] * vp[5]) * y_unit, (matrix[4] * vp[1] + matrix[5] * vp[5]) * y_unit,
         (matrix[12] * vp[1] + matrix[13] * vp[5] + vp[13]) * y_unit}};
    return map;
}

static BandRecord *band_record(const float *xy) {
    for (size_t i = 0; i < band_record_count; ++i)
        if (band_records[i].xy == xy) return &band_records[i];
    return NULL;
}

/* The six positions of polygon `i` of a part's keyframe `key`; NULL past its last polygon or without model data. */
static float *polygon_positions(const uint8_t *command, int key, uint32_t i) {
    const uint64_t holder = pointer_at((uint64_t)(uintptr_t)command + COMMAND_RESOURCE);
    const uint64_t data = holder ? pointer_at(holder + RESOURCE_DATA) : 0;
    const uint64_t polygons = data ? pointer_at(data + DATA_POLYGONS) : 0;
    const uint64_t positions = data ? pointer_at(data + DATA_POSITIONS) : 0;
    const uint64_t keys = pointer_at((uint64_t)(uintptr_t)command + COMMAND_KEYS + key * 8u);
    if (!polygons || !positions || !keys) return NULL;
    int32_t first;
    uint16_t count, index;
    load(&first, (const void *)(uintptr_t)(keys + 0x10), sizeof(first));
    load(&count, (const void *)(uintptr_t)(keys + 0x16), sizeof(count));
    if (first < 0 || count > MAX_BAND_POLYGONS || i >= count) return NULL;
    load(&index, (const void *)(uintptr_t)(polygons + ((uint64_t)first + i) * 16u + 0xe), sizeof(index));
    return (float *)(uintptr_t)(positions + index * 48u);
}

static void map_vertices(const ScreenMap *map, const float vertices[12], float u[6], float v[6]) {
    for (int j = 0; j < 6; ++j) {
        const float lx = vertices[j * 2], ly = vertices[j * 2 + 1];
        u[j] = map->x[0] * lx + map->x[1] * ly + map->x[2];
        v[j] = map->y[0] * lx + map->y[1] * ly + map->y[2];
    }
}

/* The band pieces of a part's polygons, every keyframe, continue to the frame edges in the model data; how many moved.
 * Only x changes. */
static uint32_t extend_bands(const uint8_t *command, const ScreenMap *map) {
    if (fabsf(map->x[0]) < 1e-9f) return 0;
    uint32_t moved = 0;
    for (int key = 0; key < 4; ++key) {
        float *xy;
        for (uint32_t i = 0; (xy = polygon_positions(command, key, i)) != NULL; ++i) {
            float vertices[12], u[6], v[6], extended[6];
            load(vertices, xy, sizeof(vertices));
            map_vertices(map, vertices, u, v);
            memcpy(extended, u, sizeof(extended));
            if (!ultrawide_extend_band(extended, v, 1.0f / widening, widening)) continue;
            BandRecord *record = band_record(xy);
            if (record && !memcmp(record->written, vertices, sizeof(vertices))) continue;
            if (!record && band_record_count == sizeof(band_records) / sizeof(band_records[0])) continue;
            if (!record) record = &band_records[band_record_count++];
            for (int j = 0; j < 6; ++j) vertices[j * 2] += (extended[j] - u[j]) / map->x[0];
            memcpy(xy, vertices, sizeof(vertices));
            record->xy = xy;
            memcpy(record->written, vertices, sizeof(vertices));
            ++moved;
        }
    }
    return moved;
}

/* 1 when every polygon of a part (and there is one) is a black matte band; `map` gives clip x and y. */
static int is_matte(const uint8_t *command, const ScreenMap *map) {
    const float to_clip_y = 16.0f * widening / 9.0f;
    uint32_t bands = 0;
    for (int key = 0; key < 4; ++key) {
        float *xy;
        for (uint32_t i = 0; (xy = polygon_positions(command, key, i)) != NULL; ++i, ++bands) {
            float vertices[12], u[6], y[6];
            load(vertices, xy, sizeof(vertices));
            map_vertices(map, vertices, u, y);
            for (int j = 0; j < 6; ++j) y[j] *= to_clip_y;
            if (!ultrawide_is_matte_band(u, y, 1.0f / widening)) return 0;
        }
    }
    return bands > 0;
}

/* DEBUG_SAULO: once per model data a band rule changed, for the manual test. */
static void debug_saulo_band(const char *what, uint32_t moved) {
    if (moved) printf("DEBUG_SAULO true ultrawide horizontal band: %s extended (%u pieces)\n", what, moved);
}

static int backdrop_fills(void) {
    if (!committed.illustration) return 0;
    for (uint32_t id = 0; id < PASSES_PER_VIEW; ++id)
        if (committed.backdrop_sets[id]) return 1;
    return 0;
}

/* The town facility frame (the inn's) draws its top and bottom bars, its lines, its title plate and its menus as one
 * part: the bars and lines reach the frame edges, the rest keeps its place and size. */
static void extend_facility_frame(const uint8_t *command) {
    float matrix[16];
    load(matrix, command + COMMAND_MATRIX, sizeof(matrix));
    const ScreenMap map = canvas_map(matrix);
    debug_saulo_band("town facility bars and lines", extend_bands(command, &map));
}

/* A widget part is full-width when its polygons, canvas units at the model's rest, reach both 16:9 edges; the open
 * and close animations scale it through its matrix, so the test holds all through them. */
static void widen_widget(uint8_t *command, uint64_t model) {
    float matrix[16], bounds[4];
    load(matrix, command + COMMAND_MATRIX, sizeof(matrix));
    load(bounds, command + COMMAND_BOUNDS, sizeof(bounds));
    if (!ultrawide_spans_screen(fminf(bounds[0], bounds[2]), fmaxf(bounds[0], bounds[2]))) return;
    uint32_t type;
    load(&type, (const void *)(uintptr_t)(model - WIDGET_MODEL + WIDGET_TYPE), sizeof(type));
    const float reach = fmaxf(fabsf(bounds[1]), fabsf(bounds[3]));
    const int widens = ultrawide_widget_widens(type, reach);
    const float factor = ultrawide_widget_stretch(widening, world_window);
    /* DEBUG_SAULO: each widget type's first full-width part and what happened to it, for the manual test. */
    static uint64_t debug_saulo_seen[2];
    if (!(debug_saulo_seen[widens] & 1ull << (type & 63))) {
        debug_saulo_seen[widens] |= 1ull << (type & 63);
        int16_t clip;
        load(&clip, (const void *)(uintptr_t)(model + MODEL_CLIP), sizeof(clip));
        printf("DEBUG_SAULO true ultrawide widget: type 0x%x clip %d reach y %.0f %s x%.4f\n", type, clip, reach,
               widens ? "stretched" : "kept at its 16:9 width", widens ? factor : 1.0f);
    }
    if (!widens) return;
    if (backdrop_fills()) {
        /* Behind a fixed screen's full backdrop the backing and its border reach the edges; its ornament stays. */
        const ScreenMap rest = canvas_map(kIdentity);
        debug_saulo_band("dialogue backing", extend_bands(command, &rest));
        return;
    }
    if (factor <= 1.0f) return;
    ultrawide_stretch_part(matrix, factor);
    memcpy(command + COMMAND_MATRIX, matrix, sizeof(matrix));
}

static void zoom_map(uint8_t *command) {
    const float factor = ultrawide_map_zoom(widening);
    map_frame = camera_frames + 1;
    frame.town_map = 1;
    if (factor <= 1.0f) return;
    float matrix[16];
    load(matrix, command + COMMAND_MATRIX, sizeof(matrix));
    ultrawide_zoom_part(matrix, factor);
    memcpy(command + COMMAND_MATRIX, matrix, sizeof(matrix));
    static int debug_saulo_map; /* DEBUG_SAULO */
    if (!debug_saulo_map++) printf("DEBUG_SAULO true ultrawide map: x%.4f\n", factor);
}

static int is_fade(uint64_t model) {
    for (size_t i = 0; i < ULTRAWIDE_FADES; ++i)
        if (fades[i].model && fades[i].model == model && pointer_at(model - FADE_MODEL) == fades[i].vtable) return 1;
    return 0;
}

/* The fade (loading and saving) draws its crest, its decorative line and its black backing as model parts. Whichever
 * polygons reach the 16:9 edges extend to the frame edges, the same rule as the facility frame and the fixed-screen
 * bars; the crest and anything clear of the edges stay as they are, and the part's bounds follow what moved. */
static void extend_fade(uint8_t *command) {
    float matrix[16];
    load(matrix, command + COMMAND_MATRIX, sizeof(matrix));
    if (fabsf(matrix[0]) < 0.01f) return;
    const ScreenMap map = canvas_map(matrix);
    debug_saulo_band("fade backing", extend_bands(command, &map));
}

/* The highest priority bucket of a set that holds commands: -1 when none, -2 when the buckets do not read as a list. */
static int last_filled_bucket(uint64_t set) {
    const uint64_t begin = pointer_at(set + SET_BUCKETS), end = pointer_at(set + SET_BUCKETS + 8);
    if (end < begin || (end - begin) % BUCKET_SIZE || (end - begin) / BUCKET_SIZE > MAX_BUCKETS) return -2;
    for (int i = (int)((end - begin) / BUCKET_SIZE) - 1; i >= 0; --i) {
        const uint64_t bucket = begin + (uint64_t)i * BUCKET_SIZE;
        if (pointer_at(bucket + BUCKET_END) != pointer_at(bucket + BUCKET_BEGIN)) return i;
    }
    return -1;
}

static unsigned debug_saulo_backdrop_lines; /* DEBUG_SAULO */

unsigned ultrawide_debug_saulo_backdrop_lines(void) {
    return debug_saulo_backdrop_lines;
}

/* DEBUG_SAULO: once per backdrop whose screen has a black matte, for the manual test. */
static void debug_saulo_matte(uint32_t id) {
    static uint64_t printed[PASSES_PER_VIEW];
    const uint64_t backdrop = frame.backdrop_models[id];
    if (!backdrop || printed[id] == backdrop) return;
    printed[id] = backdrop;
    printf("DEBUG_SAULO true ultrawide fixed matte: backdrop y %.2f..%.2f in pass 0x%02x; black bars %s\n",
           frame.backdrop_y[id][0], frame.backdrop_y[id][1], id,
           frame.matte_passes >> id & 1 ? "extended across full canvas width, drawn with the parts after them from "
                                          "the next pass's set"
                                        : "kept at 16:9, the next pass's set is not free");
}

static uint64_t set_of(uint64_t pass) {
    const uint8_t index = *(const uint8_t *)(uintptr_t)(pass + PASS_SET_INDEX);
    return index > 2 ? 0 : pass + PASS_SETS + index * SET_SIZE;
}

/* The set of the world pass after `id` when the game leaves that pass unused: the commit gives it pass id's view at the
 * whole width, so what is queued there still draws over pass id, unzoomed. 0 when it is not free. */
static uint64_t frame_set(uint64_t pass, uint32_t id) {
    if (id + 1 >= PASSES_PER_VIEW || !ultrawide_is_world_pass(id + 1) || committed.used_passes >> (id + 1) & 1 ||
        committed.backdrop_sets[id + 1])
        return 0;
    const uint64_t target = set_of(pass + PASS_SIZE);
    if (!target || (frame.frame_sets[id + 1] != target && last_filled_bucket(target) != -1)) return 0;
    frame.frame_sets[id + 1] = target;
    return target;
}

/* The black matte's pass keeps the 16:9, so its bars reach the frame edges only from the frame set; the parts of its
 * pass queued after it (the title plate, choice windows) go there too and still draw over it. */
static void move_matte(GuestRegs *regs, const uint8_t *command, const ScreenMap *map, uint32_t id) {
    const uint64_t target = frame_set(regs->rax, id);
    if (target) {
        regs->rdi = target;
        frame.matte_passes |= 1ull << id;
        extend_bands(command, map);
    }
    debug_saulo_matte(id);
}

/* A full-view part of another model after the pass's backdrop is the screen's frame (the title plate's ornaments and
 * long lines): in the frame set it is unzoomed and its lines reach the frame edges. */
static void move_screen_frame(GuestRegs *regs, const uint8_t *command, uint64_t model, uint32_t id) {
    const uint64_t pass = regs->rax, target = frame_set(pass, id);
    if (!target) return;
    regs->rdi = target;
    float matrix[16], vp[16];
    load(matrix, command + COMMAND_MATRIX, sizeof(matrix));
    load(vp, (const void *)(uintptr_t)(pass + PASS_VIEW_PROJECTION), sizeof(vp));
    const ScreenMap map = world_map(matrix, vp);
    debug_saulo_band("fixed screen frame lines", extend_bands(command, &map));
    static uint64_t debug_saulo_frame[PASSES_PER_VIEW]; /* DEBUG_SAULO */
    if (debug_saulo_frame[id] != model) {
        debug_saulo_frame[id] = model;
        printf("DEBUG_SAULO true ultrawide fixed screen frame: model %#llx, pass 0x%02x part drawn unzoomed at the "
               "whole width in pass 0x%02x\n",
               (unsigned long long)model, id, id + 1);
    }
}

/* On an illustration screen, the first part of a pass that covers the whole 16:9 view is its backdrop, and so are the
 * model's other full-view parts but the black matte: each is queued in the set of the pass before, in a bucket after
 * everything already there, so it alone fills the frame behind the 16:9 rest of its pass and still covers what it
 * covered. rax = its pass, rdi = its set, edx = its priority bucket. */
static void move_backdrop(GuestRegs *regs, const uint8_t *command, uint64_t model, uint32_t id) {
    const uint64_t pass = regs->rax, set = regs->rdi;
    if (!pass || set < pass + PASS_SETS || set >= pass + PASS_SETS + 3 * SET_SIZE ||
        (set - pass - PASS_SETS) % SET_SIZE)
        return;
    float matrix[16], bounds[4], vp[16], left = 0.0f, right = 0.0f, bottom = 0.0f, top = 0.0f;
    load(matrix, command + COMMAND_MATRIX, sizeof(matrix));
    load(bounds, command + COMMAND_BOUNDS, sizeof(bounds));
    load(vp, (const void *)(uintptr_t)(pass + PASS_VIEW_PROJECTION), sizeof(vp));
    for (int i = 0; i < 4; ++i) {
        const float lx = bounds[(i & 1) ? 2 : 0], ly = bounds[(i & 2) ? 3 : 1];
        const float wx = lx * matrix[0] + ly * matrix[4] + matrix[12];
        const float wy = lx * matrix[1] + ly * matrix[5] + matrix[13];
        const float x = wx * vp[0] + wy * vp[4] + vp[12], y = wx * vp[1] + wy * vp[5] + vp[13];
        left = i ? fminf(left, x) : x;
        right = i ? fmaxf(right, x) : x;
        bottom = i ? fminf(bottom, y) : y;
        top = i ? fmaxf(top, y) : y;
    }
    if (!ultrawide_covers_view(left, right, bottom, top, widening)) {
        if (frame.matte_passes >> id & 1) regs->rdi = frame.frame_sets[id + 1];
        return;
    }
    const ScreenMap map = world_map(matrix, vp);
    if (is_matte(command, &map)) {
        move_matte(regs, command, &map, id);
        return;
    }
    if (frame.backdrop_models[id] && frame.backdrop_models[id] != model) {
        move_screen_frame(regs, command, model, id);
        return;
    }
    const uint64_t target = set_of(pass - PASS_SIZE);
    const int last = target ? last_filled_bucket(target) : -2;
    if (last < -1) return;
    if (last >= (int32_t)(uint32_t)regs->rdx) regs->rdx = (uint32_t)(last + 1);
    regs->rdi = target;
    frame.backdrop_sets[id - 1] = target;
    if (frame.backdrop_models[id]) return;
    frame.backdrop_models[id] = model;
    /* Only the backdrop's own rows: the frame above and below it stays the black it was at 16:9. */
    frame.backdrop_reach[id - 1] = fminf(-bottom, top);
    frame.backdrop_y[id][0] = bottom;
    frame.backdrop_y[id][1] = top;
    static uint64_t debug_saulo_backdrop[PASSES_PER_VIEW]; /* DEBUG_SAULO */
    if (debug_saulo_backdrop[id] != model) {
        debug_saulo_backdrop[id] = model;
        ++debug_saulo_backdrop_lines;
        printf("DEBUG_SAULO true ultrawide fixed backdrop: model %#llx, pass 0x%02x part over x %.2f..%.2f "
               "y %.2f..%.2f, drawn at cover x%.4f in pass 0x%02x bucket %u; foreground and UI unchanged\n",
               (unsigned long long)model, id, left, right, bottom, top, widening, id - 1, (uint32_t)regs->rdx);
    }
}

/* At the model part draw's call of the queue, the command built: rsi = it, r12 = the model, r14 = the part, rbp = the
 * part draw's frame, whose caller (the model draw) returns into the owner's draw. */
void GUEST_HOOK_ABI ultrawide_part_hook(GuestRegs *regs) {
    uint8_t *command = (uint8_t *)(uintptr_t)regs->rsi;
    const uint64_t model = regs->r12, part = regs->r14, frame_pointer = regs->rbp;
    if (!installed || !command || !model || !part || !frame_pointer ||
        pointer_at(frame_pointer + 8) != image_base + installed->model_draw + MODEL_DRAW_PART_RETURN)
        return;
    const uint64_t model_frame = pointer_at(frame_pointer);
    const uint64_t owner = model_frame ? pointer_at(model_frame + 8) : 0;
    int16_t layer;
    load(&layer, (const void *)(uintptr_t)(part + PART_LAYER), sizeof(layer));
    if (layer == PLAYFIELD_LAYER) {
        float matrix[16], bounds[4];
        load(matrix, command + COMMAND_MATRIX, sizeof(matrix));
        load(bounds, command + COMMAND_BOUNDS, sizeof(bounds));
        for (int i = 0; i < 4; ++i) {
            const float x = bounds[(i & 1) ? 2 : 0] * matrix[0] + bounds[(i & 2) ? 3 : 1] * matrix[4] + matrix[12];
            if (!frame.playfield || x < frame.playfield_left) frame.playfield_left = x;
            if (!frame.playfield || x > frame.playfield_right) frame.playfield_right = x;
            frame.playfield = 1;
        }
    }
    if (layer >= 0 && layer < PASSES_PER_VIEW) frame.used_passes |= 1ull << layer;
    if (committed.illustration && !committed.route_map && layer > 0 && layer < PASSES_PER_VIEW &&
        ultrawide_is_world_pass((uint32_t)layer) && ultrawide_is_world_pass((uint32_t)layer - 1))
        move_backdrop(regs, command, model, (uint32_t)layer);
    const uint64_t worldmap = image_base + installed->worldmap_draw;
    if (owner >= worldmap && owner < worldmap + WORLDMAP_DRAW_SIZE) {
        frame.route_map = 1;
    } else if (owner == image_base + installed->widget_draw_call + 5) {
        widen_widget(command, model);
    } else if (owner == image_base + installed->map_draw_call + 5) {
        town_map = model - MAP_MODEL;
        zoom_map(command);
    } else if (town_map && model == town_map + MAP_FACE_MODEL) {
        zoom_map(command);
    } else if (owner == image_base + installed->facility_draw_call + 5) {
        extend_facility_frame(command);
    } else if (is_fade(model)) {
        extend_fade(command);
    }
}

/* At a fade draw's tail jump to the model draw: rdi = the fade's model. Fires every frame the fade actually draws. */
void GUEST_HOOK_ABI ultrawide_fade_hook(GuestRegs *regs) {
    const uint64_t model = regs->rdi;
    if (!model) return;
    frame.fade_active = 1;
    const FadeModel fade = {model, pointer_at(model - FADE_MODEL)};
    for (size_t i = 0; i < ULTRAWIDE_FADES; ++i)
        if (fades[i].model == model) {
            fades[i] = fade;
            return;
        }
    memmove(&fades[1], &fades[0], sizeof(fades) - sizeof(fades[0]));
    fades[0] = fade;
    static int debug_saulo_fade_seen; /* DEBUG_SAULO */
    if (!debug_saulo_fade_seen++) printf("DEBUG_SAULO true ultrawide fade: drawn\n");
}

static void branch_hook(GuestHook *hook, const char *name, uint8_t opcode, uint32_t site, uint32_t target,
                        GuestHookHandler handler) {
    const int32_t rel = (int32_t)((int64_t)target - ((int64_t)site + 5));
    hook->name = name;
    hook->offset = site;
    hook->length = 5;
    memset(hook->original, 0, sizeof(hook->original));
    hook->original[0] = opcode;
    memcpy(hook->original + 1, &rel, sizeof(rel));
    hook->handler = handler;
}

void ultrawide_record_hooks(const UltrawideRecord *record, GuestHook hooks[ULTRAWIDE_HOOK_COUNT]) {
    static const char *const fills[ULTRAWIDE_FILL_SITES] = {"colour fill", "textured fill", "gradient fill"};
    static const char *const fade_names[ULTRAWIDE_FADES] = {"loading fade", "saving fade"};
    GuestHook *hook = hooks;
    for (uint32_t i = 0; i < ULTRAWIDE_FILL_SITES; ++i)
        branch_hook(hook++, fills[i], 0xe8, record->fill_calls[i], record->submit, ultrawide_fill_hook);
    branch_hook(hook++, "camera view", 0xe8, CAMERA_VIEW_CALL, VIEW_BUILD, ultrawide_camera_hook);
    hook->name = "frame commit";
    hook->offset = record->frame_commit;
    hook->length = sizeof(kCommitBytes);
    memset(hook->original, 0, sizeof(hook->original));
    memcpy(hook->original, kCommitBytes, sizeof(kCommitBytes));
    hook->handler = ultrawide_commit_hook;
    ++hook;
    branch_hook(hook++, "model part", 0xe8, record->part_queue_call, record->queue, ultrawide_part_hook);
    for (uint32_t i = 0; i < ULTRAWIDE_FADES; ++i)
        branch_hook(hook++, fade_names[i], 0xe9, record->fade_jumps[i], record->model_draw, ultrawide_fade_hook);
}

size_t ultrawide_hooks_stub_bytes(void) {
    return ULTRAWIDE_HOOK_COUNT * guest_hook_stub_size();
}

static int calls(const uint8_t *image, uint64_t size, uint32_t site, uint32_t target) {
    if ((uint64_t)site + 5 > size || image[site] != 0xe8) return 0;
    int32_t rel;
    memcpy(&rel, image + site + 1, sizeof(rel));
    return (int64_t)site + 5 + rel == (int64_t)target;
}

static int function_at(const uint8_t *image, uint64_t size, uint64_t offset) {
    static const uint8_t prologue[] = {0x55, 0x48, 0x89, 0xe5}; /* push %rbp; mov %rsp,%rbp */
    return offset + sizeof(prologue) <= size && !memcmp(image + offset, prologue, sizeof(prologue));
}

static int bytes_at(const uint8_t *image, uint64_t size, uint64_t offset, const uint8_t *bytes, size_t count) {
    return offset + count <= size && !memcmp(image + offset, bytes, count);
}

/* What the handlers rely on beyond their own sites: the owner calls, the draw they compare with and the queue's
 * buckets. */
static const char *group_precondition(int group, const uint8_t *image, uint64_t size, const UltrawideRecord *record) {
    /* lea 0x2b0(%rax,%rcx),%rdi: rax still holds the part's pass at the queue call. */
    static const uint8_t set_from_pass[] = {0x48, 0x8d, 0xbc, 0x08, 0xb0, 0x02, 0x00, 0x00};
    /* The queue appends to bucket max(edx, 0): mov 0x2b0(%r15),%rcx; mov 0x2b8(%r15),%rdx, then the bucket's end and
     * begin, mov 0x10(%rcx,%rbx),%rax and sub 0x8(%rcx,%rbx),%rax. */
    static const uint8_t buckets[] = {0x49, 0x8b, 0x8f, 0xb0, 0x02, 0x00, 0x00,
                                      0x49, 0x8b, 0x97, 0xb8, 0x02, 0x00, 0x00};
    static const uint8_t bucket_end[] = {0x48, 0x8b, 0x44, 0x19, 0x10}, bucket_begin[] = {0x48, 0x2b, 0x44, 0x19, 0x08};
    if (group != 2) return NULL;
    if (record->part_queue_call < sizeof(set_from_pass) ||
        memcmp(image + record->part_queue_call - sizeof(set_from_pass), set_from_pass, sizeof(set_from_pass)))
        return "model part queue";
    if (!bytes_at(image, size, (uint64_t)record->queue + 0x57, buckets, sizeof(buckets)) ||
        !bytes_at(image, size, (uint64_t)record->queue + 0x9b, bucket_end, sizeof(bucket_end)) ||
        !bytes_at(image, size, (uint64_t)record->queue + 0xa9, bucket_begin, sizeof(bucket_begin)))
        return "render queue buckets";
    if (!calls(image, size, record->widget_draw_call, record->model_draw)) return "widget draw";
    if (!calls(image, size, record->map_draw_call, record->model_draw)) return "map draw";
    if (!calls(image, size, record->facility_draw_call, record->model_draw)) return "facility frame draw";
    if ((uint64_t)record->model_draw + MODEL_DRAW_PART_RETURN > size ||
        image[record->model_draw + MODEL_DRAW_PART_RETURN - 5] != 0xe8)
        return "model part draw";
    if (!function_at(image, size, record->worldmap_draw) ||
        !function_at(image, size, (uint64_t)record->worldmap_draw + WORLDMAP_DRAW_SIZE))
        return "route map draw";
    return NULL;
}

int ultrawide_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size,
                            double scale, char *detail, size_t detail_size) {
    static const char *const group_names[] = {"fill", "world window", "widget, map and fade"};
    static const size_t group_sizes[] = {ULTRAWIDE_FILL_HOOKS, ULTRAWIDE_WORLD_HOOKS, ULTRAWIDE_PART_HOOKS};
    for (size_t i = 0; i < ULTRAWIDE_RECORD_COUNT; ++i) {
        const UltrawideRecord *record = &ultrawide_records[i];
        if (size != record->image.size || entry != record->image.entry) continue;
        GuestHook hooks[ULTRAWIDE_HOOK_COUNT];
        ultrawide_record_hooks(record, hooks);
        ultrawide_hooks_configure((float)scale);
        image_base = (uint64_t)(uintptr_t)image;
        installed = record;
        size_t first = 0, used = 0, written = 0;
        int any = 0;
        detail[0] = 0;
        for (int group = 0; group < 3; ++group) {
            const size_t count = group_sizes[group], room = count * guest_hook_stub_size();
            const char *precondition = group_precondition(group, image, size, record);
            size_t bad = 0;
            const GuestHookStatus status =
                precondition ? GUEST_HOOK_MISMATCH
                : used + room > stubs_size
                    ? GUEST_HOOK_NO_ROOM
                    : guest_hooks_install(image, size, entry, &record->image, hooks + first, count, stubs + used,
                                          room, &bad);
            if (status == GUEST_HOOK_OK) {
                used += room;
                any = 1;
                written += (size_t)snprintf(detail + written, detail_size - written, "%s%s hooks installed",
                                            written ? "; " : "", group_names[group]);
            } else {
                written += (size_t)snprintf(detail + written, detail_size - written, "%s%s hooks off (%s: %s)",
                                            written ? "; " : "", group_names[group],
                                            precondition ? precondition : hooks[first + bad].name,
                                            guest_hook_status_text(status));
            }
            if (written >= detail_size) written = detail_size - 1;
            first += count;
        }
        return any;
    }
    snprintf(detail, detail_size, "no hook record for this image");
    return 0;
}
