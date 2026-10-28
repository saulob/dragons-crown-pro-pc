/* True Ultrawide's guest hooks, on top of the widened pass projections (launcher/dcp_resolution.h). One explicit record
 * per verified build; others have none. Three groups install separately, so a mismatch disables only its own:
 * - fill: a full-screen fill of the 2D quad builders widens with the screen-space canvas;
 * - world: background layers keep the whole width, playfield and foreground layers stop where the stage and the
 *   playfield layer end; an illustration screen keeps its 16:9 with its backdrop alone covering the frame behind within
 *   its own rows and its frame lines and black matte bars reaching the edges; the route map is clipped to the centred
 *   16:9;
 * - parts: model parts of the bottom message bands, the town map, the town facility frame and the loading/saving
 *   fade. */
#ifndef DCP_ULTRAWIDE_HOOKS_H
#define DCP_ULTRAWIDE_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ULTRAWIDE_FILL_SITES 3
#define ULTRAWIDE_FADES 2
#define ULTRAWIDE_FILL_HOOKS ULTRAWIDE_FILL_SITES
#define ULTRAWIDE_WORLD_HOOKS 2
#define ULTRAWIDE_PART_HOOKS (1 + ULTRAWIDE_FADES)
#define ULTRAWIDE_HOOK_COUNT (ULTRAWIDE_FILL_HOOKS + ULTRAWIDE_WORLD_HOOKS + ULTRAWIDE_PART_HOOKS)

typedef struct {
    const char *build;
    GuestImageId image;
    uint32_t submit;                           /* the 2D primitive submit */
    uint32_t fill_calls[ULTRAWIDE_FILL_SITES]; /* its call in the colour, textured and gradient quad builders */
    uint32_t frame_commit;                     /* in the frame commit, just after a pass is copied into its set */
    uint32_t queue;                            /* the render queue */
    uint32_t part_queue_call;                  /* the model part draw's call of the queue */
    uint32_t model_draw;                       /* a model's draw, part by part */
    uint32_t widget_draw_call;                 /* the message widgets' call of the model draw */
    uint32_t map_draw_call;                    /* the town map's call of the model draw */
    uint32_t facility_draw_call;               /* the town facility frame's call of the model draw */
    uint32_t worldmap_draw;                    /* the route map widget's draw */
    uint32_t fade_jumps[ULTRAWIDE_FADES];      /* the loading and saving fades' tail jump to the model draw */
} UltrawideRecord;

#define ULTRAWIDE_RECORD_COUNT 5
extern const UltrawideRecord ultrawide_records[ULTRAWIDE_RECORD_COUNT];

/* 1 for the passes drawn on the 1280 x 720 screen-space canvas. */
int ultrawide_is_screen_pass(uint32_t layer);
/* 1 for the passes drawn on the 812 x 456 world canvas. */
int ultrawide_is_world_pass(uint32_t layer);

/* Scales the x of `count` vertices `stride` bytes apart by `scale` when they span the whole 16:9 screen-space canvas;
 * 1 when it did. */
int ultrawide_widen_fill(uint8_t *vertices, uint32_t count, size_t stride, float scale);

/* A horizontal part of the target, as fractions of its width. */
typedef struct {
    float left, right;
} UltrawideSpan;

/* The centred 16:9 part of a target widened by `scale`. */
UltrawideSpan ultrawide_centre_span(float scale);

/* The stage's horizontal limits [left, right] seen from a camera at `centre` whose 16:9 view is `view_width` wide;
 * the whole target when there are none (limits outside +-1e6 or empty). */
UltrawideSpan ultrawide_limits_span(float centre, float view_width, float scale, int32_t left, int32_t right);

/* The frame's one world window: the limits, cut to the playfield layer's extent when it drew and to the town map's
 * share; never inside the centred 16:9, never past the target. */
UltrawideSpan ultrawide_frame_span(UltrawideSpan limits, const UltrawideSpan *playfield, float map_share, float scale);

/* Narrows a pass's viewport {left, top, right, bottom} to `span` and compensates its projection and view-projection
 * (row vectors) in clip space, so every world point keeps its pixel and only clipping differs; 0 for the whole
 * width. */
int ultrawide_clip_pass(float viewport[4], float projection[16], float view_projection[16], UltrawideSpan span);

/* Layers moving with the playfield layer or faster are clipped with it; distant ones keep the whole width. */
int ultrawide_layer_follows_playfield(float parallax, float playfield);

/* One frame's world: the playfield window, each pass's layer parallax, the town map's share, and whether the route map
 * or an illustration screen (a one-screen stage without a playfield layer) is showing. */
typedef struct {
    UltrawideSpan playfield;
    const float *parallax;
    uint64_t parallax_known; /* one bit per pass id */
    float map_share;
    int route_map, illustration;
} UltrawideWorldFrame;

/* The horizontal window of pass `id` in that frame; the whole width for the screen passes and pass 0. */
UltrawideSpan ultrawide_world_pass_span(uint32_t id, const UltrawideWorldFrame *world, float scale);

/* 1 when a part's clip-space extent covers the whole centred 16:9 width and most of the height: a backdrop. */
int ultrawide_covers_view(float left, float right, float bottom, float top, float scale);
/* 1 while the screen is a one-screen illustration with no stable fixed backdrop yet or no longer: entering from
 * gameplay, or leaving a stable illustration, with no mixed cover/native mapping in between. */
int ultrawide_fixed_transition_active(void);
/* Narrows a pass's viewport to the rows within clip y +-`reach` and compensates clip y, symmetric about the centre so
 * the target's y direction does not matter; 0 for the whole height. */
int ultrawide_clip_rows(float viewport[4], float projection[16], float view_projection[16], float reach);
/* Scales clip x and y of a row-vector projection by `factor` about the centre: the backdrop's cover. */
void ultrawide_cover_view(float matrix[16], float factor);

/* The share of the widened width the stage allows, for the bottom message bands: 1, 1 / scale on a one-screen stage,
 * or the stage width over the widened view in between. */
float ultrawide_world_share(float view_width, float scale, int32_t left, int32_t right);

/* 1 when [left, right] reaches both edges of the 16:9 screen-space canvas. */
int ultrawide_spans_screen(float left, float right);
/* Which message widget parts stretch: the narration (0x26) as it was, and the dialogue (0x25) when its band is anchored
 * at the top or bottom edge; `reach` is the largest |y| of its polygons. */
int ultrawide_widget_widens(uint32_t type, float reach);
/* `matrix` is a model part's world matrix as its draw command holds it: row vectors, rows 0-2 linear, row 3
 * translation. Stretches the part horizontally about its own origin. */
void ultrawide_stretch_part(float matrix[16], float factor);
/* Scales the part uniformly about the canvas centre. */
void ultrawide_zoom_part(float matrix[16], float factor);

/* The stretch of a bottom band: the screen-space widening times the share the world shows, at least 1. */
float ultrawide_widget_stretch(float scale, float window);

/* The town map's uniform zoom: the widening, while its markers stay on screen. */
float ultrawide_map_zoom(float scale);

/* For a polygon's six screen x (`u`) and y (`v`), in units where the centred 16:9 spans +-`half`: a piece that reaches
 * a 16:9 edge continues to the same place in a frame `scale` times wider. Wide pieces (bands, lines) move only their
 * outer vertices; pieces taller than wide (end caps) move whole. 1 when it moved. */
int ultrawide_extend_band(float u[6], const float v[6], float half, float scale);
/* 1 for a polygon (six clip x `u` and y `y`) that is a thin band across the whole centred 16:9, +-`half`, at the top or
 * bottom of the frame: a fixed screen's black matte. */
int ultrawide_is_matte_band(const float u[6], const float y[6], float half);

/* The horizontal widening of the pass projections. */
void ultrawide_hooks_configure(float scale);

void GUEST_HOOK_ABI ultrawide_fill_hook(GuestRegs *regs);
void GUEST_HOOK_ABI ultrawide_camera_hook(GuestRegs *regs);
void GUEST_HOOK_ABI ultrawide_commit_hook(GuestRegs *regs);
void GUEST_HOOK_ABI ultrawide_part_hook(GuestRegs *regs);
void GUEST_HOOK_ABI ultrawide_fade_hook(GuestRegs *regs);

/* DEBUG_SAULO: how many fixed backdrop lines were printed. */
unsigned ultrawide_debug_saulo_backdrop_lines(void);
/* DEBUG_SAULO: how many fixed-screen transition state changes (fallback entered, stable resumed, gameplay resumed)
 * were printed. */
unsigned ultrawide_debug_saulo_transition_lines(void);
/* DEBUG_SAULO: the last value sent towards the final presenter's side mask (fade active or the fallback active),
 * for the test build only. */
int ultrawide_debug_saulo_mask_wanted(void);

void ultrawide_record_hooks(const UltrawideRecord *record, GuestHook hooks[ULTRAWIDE_HOOK_COUNT]);

size_t ultrawide_hooks_stub_bytes(void);

/* Installs the hook groups of the record whose image this is, with `scale` the horizontal widening of the pass
 * projections. 1 when any group installed; `detail` says which. A group that fails leaves its sites untouched. */
int ultrawide_hooks_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size,
                            double scale, char *detail, size_t detail_size);

#ifdef __cplusplus
}
#endif

#endif
