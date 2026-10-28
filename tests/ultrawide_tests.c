/* Focused tests of the True Ultrawide hooks: the screen-space passes, the fill rule, the world windows by layer, the
 * illustration backdrop, the widget, map and fade parts, the build records and the grouped install. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_hook.h"
#include "ultrawide_hooks.h"

static int checks;
static int failures;

#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

enum { STRIDE = 0x20 };

static void put_vertex(uint8_t *vertices, int index, float x, float y) {
    memcpy(vertices + index * STRIDE, &x, 4);
    memcpy(vertices + index * STRIDE + 4, &y, 4);
}

static float x_of(const uint8_t *vertices, int index) {
    float x;
    memcpy(&x, vertices + index * STRIDE, 4);
    return x;
}

static float y_of(const uint8_t *vertices, int index) {
    float y;
    memcpy(&y, vertices + index * STRIDE + 4, 4);
    return y;
}

static void TestScreenPasses(void) {
    for (uint32_t view = 0; view < 3; ++view) {
        for (uint32_t id = 0; id < 0x2b; ++id) {
            const int screen = (id >= 0x20 && id <= 0x22) || (id >= 0x25 && id <= 0x28);
            CHECK(ultrawide_is_screen_pass(id + 0x2b * view) == screen);
        }
    }
}

static void TestFillRule(void) {
    uint8_t quad[4 * STRIDE] = {0};
    put_vertex(quad, 0, -640.0f, -360.0f);
    put_vertex(quad, 1, 640.0f, 360.0f);
    CHECK(ultrawide_widen_fill(quad, 2, STRIDE, 1.3125f) == 1);
    CHECK(x_of(quad, 0) == -840.0f && x_of(quad, 1) == 840.0f);
    CHECK(y_of(quad, 0) == -360.0f && y_of(quad, 1) == 360.0f);

    put_vertex(quad, 0, -705.0f, 200.0f);
    put_vertex(quad, 1, 705.0f, 360.0f);
    CHECK(ultrawide_widen_fill(quad, 2, STRIDE, 2.0f) == 1);
    CHECK(x_of(quad, 0) == -1410.0f && x_of(quad, 1) == 1410.0f && y_of(quad, 0) == 200.0f);

    // A gradient quad: four corners in any order.
    put_vertex(quad, 0, 640.0f, -360.0f);
    put_vertex(quad, 1, -640.0f, -360.0f);
    put_vertex(quad, 2, 640.0f, 360.0f);
    put_vertex(quad, 3, -640.0f, 360.0f);
    CHECK(ultrawide_widen_fill(quad, 4, STRIDE, 1.3125f) == 1);
    CHECK(x_of(quad, 0) == 840.0f && x_of(quad, 1) == -840.0f && x_of(quad, 2) == 840.0f && x_of(quad, 3) == -840.0f);

    // Anything narrower than the 16:9 canvas keeps its place: panels, bars, text backing, HUD.
    const float narrow[][2] = {{-600.0f, 640.0f}, {-640.0f, 600.0f}, {-100.0f, 100.0f}, {0.0f, 1280.0f}};
    for (size_t i = 0; i < sizeof(narrow) / sizeof(narrow[0]); ++i) {
        put_vertex(quad, 0, narrow[i][0], 0.0f);
        put_vertex(quad, 1, narrow[i][1], 10.0f);
        uint8_t before[sizeof(quad)];
        memcpy(before, quad, sizeof(quad));
        CHECK(ultrawide_widen_fill(quad, 2, STRIDE, 1.3125f) == 0);
        CHECK(memcmp(before, quad, sizeof(quad)) == 0);
    }
    put_vertex(quad, 0, -640.0f, 0.0f);
    put_vertex(quad, 1, 640.0f, 0.0f);
    CHECK(ultrawide_widen_fill(quad, 0, STRIDE, 1.3125f) == 0 && ultrawide_widen_fill(quad, 5, STRIDE, 1.3125f) == 0);
    CHECK(ultrawide_widen_fill(quad, 2, STRIDE, 1.0f) == 1 && x_of(quad, 0) == -640.0f && x_of(quad, 1) == 640.0f);
}

static void TestFillHook(void) {
    ultrawide_hooks_configure(1.3125f);
    uint8_t quad[2 * STRIDE] = {0};
    GuestRegs regs;
    const struct {
        uint64_t layer, matrix, texture;
        int widened;
    } cases[] = {
        {0x26, 0, 0, 1},          // a dim on the screen canvas
        {0x27 + 0x2b, 0, 0, 1},   // a fade, second view
        {0x05, 0, 0, 0},          // a world pass: Hor+ already
        {0x29, 0, 0, 0},          // the 4:3 canvas
        {0x26, 0, 0x1000, 0},     // textured: a picture, never stretched
        {0x26, 0x2000, 0, 0},     // its own matrix: not canvas coordinates
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        put_vertex(quad, 0, -640.0f, -360.0f);
        put_vertex(quad, 1, 640.0f, 360.0f);
        memset(&regs, 0, sizeof(regs));
        regs.rdi = cases[i].layer;
        regs.rdx = (uint64_t)(uintptr_t)quad;
        regs.rcx = 2;
        regs.r8 = cases[i].matrix;
        regs.r9 = cases[i].texture;
        ultrawide_fill_hook(&regs);
        CHECK(regs.skip == 0);
        CHECK(x_of(quad, 1) == (cases[i].widened ? 840.0f : 640.0f));
        CHECK(y_of(quad, 1) == 360.0f);
    }
}

static int near(float a, float b) {
    return fabsf(a - b) < 0.01f;
}

static void TestWorldPasses(void) {
    for (uint32_t view = 0; view < 2; ++view) {
        for (uint32_t id = 0; id < 0x2b; ++id) {
            const int world = id < 0x20 || id == 0x23 || id == 0x24 || id == 0x2a;
            CHECK(ultrawide_is_world_pass(id + 0x2b * view) == world);
            CHECK(!(world && ultrawide_is_screen_pass(id)));
        }
    }
}

/* The bottom bands' share of the widening, as the approved narration uses it. */
static void TestWorldShare(void) {
    CHECK(ultrawide_world_share(812.0f, 1.0f, -3000, 3000) == 1.0f);
    CHECK(ultrawide_world_share(812.0f, 1.3125f, -3000, 3000) == 1.0f);
    CHECK(near(ultrawide_world_share(1279.0f, 1.3125f, -640, 640), 1.0f / 1.3125f));
    CHECK(near(ultrawide_world_share(1280.0f, 1.3125f, -800, 800), 1600.0f / 1680.0f));
    CHECK(ultrawide_world_share(812.0f, 1.3125f, -10000000, 10000000) == 1.0f);
}

static void TestLimitsSpan(void) {
    const float wide = 1.3125f, super = 2.0f;
    const UltrawideSpan centre = ultrawide_centre_span(wide);
    CHECK(near(centre.left * 2520.0f, 300.0f) && near(centre.right * 2520.0f, 2220.0f));
    CHECK(near(ultrawide_centre_span(super).left * 3840.0f, 960.0f));
    CHECK(ultrawide_centre_span(1.0f).left == 0.0f && ultrawide_centre_span(1.0f).right == 1.0f);
    // A wide stage, camera anywhere inside: the whole target.
    UltrawideSpan s = ultrawide_limits_span(0.0f, 812.0f, wide, -3291, 3324);
    CHECK(s.left == 0.0f && s.right == 1.0f);
    s = ultrawide_limits_span(0.0f, 812.0f, super, -3291, 3324);
    CHECK(s.left == 0.0f && s.right == 1.0f);
    // One screen wide: exactly the centred 16:9.
    s = ultrawide_limits_span(0.0f, 1280.0f, wide, -640, 640);
    CHECK(near(s.left, centre.left) && near(s.right, centre.right));
    s = ultrawide_limits_span(0.0f, 1280.0f, super, -640, 640);
    CHECK(near(s.left, 0.25f) && near(s.right, 0.75f));
    // Near the left limit: the stage ends inside the frame on that side only; the camera is not moved.
    s = ultrawide_limits_span(-2885.0f, 812.0f, wide, -3291, 3324);
    CHECK(s.left > 0.0f && near(s.left, 0.5f - 406.0f / 1065.75f) && s.right == 1.0f);
    s = ultrawide_limits_span(2918.0f, 812.0f, wide, -3291, 3324);
    CHECK(s.left == 0.0f && s.right < 1.0f);
    // Wider than 16:9, narrower than asked: the stage, as it lies.
    s = ultrawide_limits_span(0.0f, 1280.0f, wide, -800, 800);
    CHECK(near(s.left, 0.5f - 800.0f / 1680.0f) && near(s.right, 0.5f + 800.0f / 1680.0f));
    // None, or 16:9: the whole target.
    s = ultrawide_limits_span(12345.0f, 812.0f, wide, -10000000, 10000000);
    CHECK(s.left == 0.0f && s.right == 1.0f);
    s = ultrawide_limits_span(0.0f, 1280.0f, 1.0f, -640, 640);
    CHECK(s.left == 0.0f && s.right == 1.0f);
}

static void TestFrameSpan(void) {
    const float wide = 1.3125f;
    const UltrawideSpan full = {0.0f, 1.0f}, centre = ultrawide_centre_span(wide);
    // Full stage, playfield everywhere: full Hor+.
    UltrawideSpan play = {-0.2f, 1.3f};
    UltrawideSpan s = ultrawide_frame_span(full, &play, 1.0f, wide);
    CHECK(s.left == 0.0f && s.right == 1.0f);
    // The playfield layer ends inside the frame (the street's right end, the plaza's floor): cut there.
    play.left = 0.055f;
    play.right = 0.93f;
    s = ultrawide_frame_span(full, &play, 1.0f, wide);
    CHECK(s.left == 0.055f && s.right == 0.93f);
    // Never inside the 16:9 the game itself guarantees.
    play.left = 0.2f;
    play.right = 0.6f;
    s = ultrawide_frame_span(full, &play, 1.0f, wide);
    CHECK(s.left == centre.left && s.right == centre.right);
    // One-screen limits: the 16:9, whatever the playfield.
    s = ultrawide_frame_span(centre, NULL, 1.0f, wide);
    CHECK(s.left == centre.left && s.right == centre.right);
    // The town map's share at 32:9 (21:9 has none).
    s = ultrawide_frame_span(full, NULL, ultrawide_map_zoom(2.0f) / 2.0f, 2.0f);
    CHECK(near(s.right - s.left, (360.0f / 270.0f) / 2.0f) && near(s.left + s.right, 1.0f));
    s = ultrawide_frame_span(full, NULL, ultrawide_map_zoom(wide) / wide, wide);
    CHECK(s.left == 0.0f && s.right == 1.0f);
}

static void identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* A world point's pixel through a pass: clip by `vp` (row vectors), then the viewport {left, top, right, bottom}. */
static float pixel_x(const float vp[16], const float viewport[4], float x) {
    const float clip = x * vp[0] + vp[12];
    return viewport[0] + (clip + 1.0f) * 0.5f * (viewport[2] - viewport[0]);
}

static void ortho(float m[16], float half_width) {
    identity(m);
    m[0] = 1.0f / half_width;
}

static void TestClipPass(void) {
    const float half = 532.875f; // the widened world canvas
    float viewport[4] = {0.0f, 0.0f, 2520.0f, 1080.0f}, projection[16], vp[16], before_vp[16];
    ortho(projection, half);
    ortho(vp, half);
    vp[12] = -0.1f; // a view translation folded in
    memcpy(before_vp, vp, sizeof(vp));
    const float full[4] = {0.0f, 0.0f, 2520.0f, 1080.0f};
    const float points[] = {-500.0f, -300.0f, 0.0f, 120.0f, 400.0f};
    // One-screen at 21:9: the viewport is the centred 1920 and every point keeps its pixel.
    CHECK(ultrawide_clip_pass(viewport, projection, vp, ultrawide_centre_span(1.3125f)));
    CHECK(viewport[0] == 300.0f && viewport[2] == 2220.0f && viewport[1] == 0.0f && viewport[3] == 1080.0f);
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i)
        CHECK(near(pixel_x(vp, viewport, points[i]), pixel_x(before_vp, full, points[i])));
    CHECK(near(projection[0], 1.3125f / half) && projection[5] == 1.0f); // vertical unchanged
    // An off-centre window (the playfield ends on the right): same pixels again.
    float viewport2[4] = {0.0f, 0.0f, 2520.0f, 1080.0f};
    memcpy(vp, before_vp, sizeof(vp));
    ortho(projection, half);
    const UltrawideSpan right_cut = {0.0f, 1969.0f / 2520.0f};
    CHECK(ultrawide_clip_pass(viewport2, projection, vp, right_cut));
    CHECK(viewport2[0] == 0.0f && viewport2[2] == 1969.0f);
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i)
        CHECK(near(pixel_x(vp, viewport2, points[i]), pixel_x(before_vp, full, points[i])));
    // 32:9: the centred 16:9 is 1920 of 3840.
    float viewport3[4] = {0.0f, 0.0f, 3840.0f, 1080.0f};
    ortho(projection, 812.0f);
    ortho(vp, 812.0f);
    CHECK(ultrawide_clip_pass(viewport3, projection, vp, ultrawide_centre_span(2.0f)));
    CHECK(viewport3[0] == 960.0f && viewport3[2] == 2880.0f && near(projection[0], 2.0f / 812.0f));
    // The whole width: nothing changes.
    float viewport4[4] = {0.0f, 0.0f, 2520.0f, 1080.0f};
    ortho(projection, half);
    const UltrawideSpan whole = {0.0f, 1.0f};
    CHECK(!ultrawide_clip_pass(viewport4, projection, vp, whole) && viewport4[2] == 2520.0f);
}

/* Fake frame-commit input: a pass array with three frame sets per pass. */
enum { PASS_BYTES = 0xb30, FAKE_PASSES = 0x2b };

static uint8_t *fake_passes_at(float width, float scale) {
    uint8_t *passes = calloc(FAKE_PASSES, PASS_BYTES);
    if (!passes) return NULL;
    for (int i = 0; i < FAKE_PASSES; ++i) {
        uint8_t *pass = passes + i * PASS_BYTES;
        float m[16];
        ortho(m, (ultrawide_is_world_pass((uint32_t)i) ? 406.0f : 640.0f) * scale);
        const float viewport[4] = {0.0f, 0.0f, width, 1080.0f};
        pass[0xb20] = 1;
        for (int set = 0; set < 3; ++set) {
            uint8_t *s = pass + 0x2b0 + set * 0x2d0;
            memcpy(s + 0x100, m, sizeof(m));
            memcpy(s + 0x1c0, m, sizeof(m));
            memcpy(s + 0x284, viewport, sizeof(viewport));
        }
        memcpy(pass + 0x100, m, sizeof(m));
        memcpy(pass + 0x1c0, m, sizeof(m));
        memcpy(pass + 0x284, viewport, sizeof(viewport));
    }
    return passes;
}

static uint8_t *fake_passes(void) {
    return fake_passes_at(2520.0f, 1.3125f);
}

/* As the game's frame commit: each pass's first 0x2a8 bytes are copied into its current set, then the hook runs. */
static void commit_frame(uint8_t *passes) {
    GuestRegs regs;
    for (int i = 0; i < FAKE_PASSES; ++i) {
        uint8_t *pass = passes + i * PASS_BYTES;
        memcpy(pass + 0x2b0 + pass[0xb20] * 0x2d0, pass, 0x2a8);
        memset(&regs, 0, sizeof(regs));
        regs.rbx = (uint64_t)(uintptr_t)passes;
        regs.r13 = (uint64_t)i * PASS_BYTES;
        ultrawide_commit_hook(&regs);
        CHECK(regs.skip == 0);
    }
}

static void set_viewport(const uint8_t *passes, int pass, float out[4]) {
    memcpy(out, passes + pass * PASS_BYTES + 0x2b0 + 0x2d0 + 0x284, 4 * sizeof(float));
}

static uint8_t main_stage[0x100], other_stage[0x100];

/* Stage layers 0-15 draw in passes 0x01-0x10: 0-8 distant (parallax 0.4), 9-15 on the playfield plane (1.0). */
enum { FAKE_LAYERS = 16 };
static uint8_t stage_map[0x400], stage_layers[FAKE_LAYERS][0x400];
static uint64_t layer_pointers[FAKE_LAYERS];

static void fake_stage_layers(void) {
    for (int i = 0; i < FAKE_LAYERS; ++i) {
        const float parallax = i < 9 ? 0.4f : 1.0f;
        memcpy(stage_layers[i] + 0x348, &parallax, 4);
        layer_pointers[i] = (uint64_t)(uintptr_t)stage_layers[i];
    }
    const uint64_t begin = (uint64_t)(uintptr_t)layer_pointers, end = begin + sizeof(layer_pointers),
                   map = (uint64_t)(uintptr_t)stage_map;
    memcpy(stage_map + 0x368, &begin, 8);
    memcpy(stage_map + 0x370, &end, 8);
    memcpy(main_stage + 0xb8, &map, 8);
}

static void run_camera_of(uint8_t *camera, const uint8_t *stage, float centre, float view_width, int32_t left,
                          int32_t right) {
    const int32_t other_view = 1;
    memcpy(other_stage + 0x74, &other_view, 4);
    fake_stage_layers();
    const uint64_t stage_address = (uint64_t)(uintptr_t)stage;
    memcpy(camera + 0x20, &stage_address, 8);
    const float view_left = centre - view_width * 0.5f, view_right = centre + view_width * 0.5f;
    memcpy(camera + 0x70, &centre, 4);
    memcpy(camera + 0x9c, &view_left, 4);
    memcpy(camera + 0xa4, &view_right, 4);
    memcpy(camera + 0xe4, &left, 4);
    memcpy(camera + 0xe8, &right, 4);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = (uint64_t)(uintptr_t)camera;
    ultrawide_camera_hook(&regs);
    CHECK(regs.skip == 0);
}

static void run_camera(uint8_t *camera, float centre, float view_width, int32_t left, int32_t right) {
    run_camera_of(camera, main_stage, centre, view_width, left, right);
}

/* A frame with nothing queued for any pass: resets which fixed-screen picture each pass last showed, so the next one
 * queued is never held as a change from unrelated earlier test content. */
static void neutral_frame(uint8_t *camera, uint8_t *passes) {
    run_camera(camera, 0.0f, 1280.0f, -640, 640);
    commit_frame(passes);
}

/* After an install (so the hooks are live): one window for every world pass of the frame. */
static void TestCommitHook(void) {
    uint8_t *passes = fake_passes();
    CHECK(passes != NULL);
    if (!passes) return;
    static uint8_t camera[0x100];
    float v[4], w[4];
    // A one-screen stage: every world pass gets the same centred 16:9, the screen passes and pass 0 keep theirs.
    run_camera(camera, 0.0f, 1280.0f, -640, 640);
    commit_frame(passes);
    set_viewport(passes, 0x01, v);
    CHECK(v[0] == 300.0f && v[2] == 2220.0f);
    for (int id = 1; id < FAKE_PASSES; ++id) {
        set_viewport(passes, id, w);
        if (ultrawide_is_world_pass((uint32_t)id)) CHECK(w[0] == v[0] && w[2] == v[2]);
        else CHECK(w[0] == 0.0f && w[2] == 2520.0f);
    }
    set_viewport(passes, 0x00, w);
    CHECK(w[0] == 0.0f && w[2] == 2520.0f);
    // The game's own passes are never changed.
    float own[4];
    memcpy(own, passes + 0x01 * PASS_BYTES + 0x284, sizeof(own));
    CHECK(own[0] == 0.0f && own[2] == 2520.0f);
    free(passes);

    // A wide stage, no playfield seen: the whole target, nothing clipped.
    passes = fake_passes();
    CHECK(passes != NULL);
    if (!passes) return;
    run_camera(camera, 0.0f, 812.0f, -3291, 3324);
    commit_frame(passes);
    set_viewport(passes, 0x10, v);
    CHECK(v[0] == 0.0f && v[2] == 2520.0f);
    free(passes);

    // A camera of another view does not decide the frame's window.
    passes = fake_passes();
    CHECK(passes != NULL);
    if (!passes) return;
    static uint8_t other_camera[0x100];
    run_camera_of(other_camera, other_stage, 0.0f, 1280.0f, -640, 640);
    commit_frame(passes);
    set_viewport(passes, 0x01, v);
    CHECK(v[0] == 0.0f && v[2] == 2520.0f);
    free(passes);
}

static void TestWidgetPolicy(void) {
    // The approved narration stretches wherever it is; the dialogue only as a band at the bottom (or top) edge.
    CHECK(ultrawide_widget_widens(0x26, 360.0f) && ultrawide_widget_widens(0x26, 20.0f));
    CHECK(ultrawide_widget_widens(0x25, 360.0f) && ultrawide_widget_widens(0x25, 352.0f));
    CHECK(!ultrawide_widget_widens(0x25, 40.0f) && !ultrawide_widget_widens(0x25, 300.0f));
    CHECK(!ultrawide_widget_widens(0x29, 360.0f) && !ultrawide_widget_widens(0x24, 360.0f));
    CHECK(!ultrawide_widget_widens(0x27, 360.0f) && !ultrawide_widget_widens(0x28, 360.0f));
    CHECK(ultrawide_spans_screen(-709.0f, 709.0f) && ultrawide_spans_screen(-640.0f, 640.0f));
    CHECK(!ultrawide_spans_screen(-617.0f, 617.0f) && !ultrawide_spans_screen(-709.0f, 300.0f));
    CHECK(ultrawide_widget_stretch(1.3125f, 1.0f) == 1.3125f && ultrawide_widget_stretch(2.0f, 1.0f) == 2.0f);
    CHECK(ultrawide_widget_stretch(1.0f, 1.0f) == 1.0f);
    CHECK(ultrawide_widget_stretch(1.3125f, 1.0f / 1.3125f) == 1.0f);
    float m[16];
    identity(m);
    m[12] = 37.0f;
    ultrawide_stretch_part(m, 1.3125f);
    CHECK(m[0] == 1.3125f && m[12] == 37.0f && m[5] == 1.0f);
}

static float canvas_x(const float m[16], float x, float y) {
    return x * m[0] + y * m[4] + m[12];
}

static float canvas_y(const float m[16], float x, float y) {
    return x * m[1] + y * m[5] + m[13];
}

static void TestMapZoom(void) {
    CHECK(ultrawide_map_zoom(1.0f) == 1.0f);
    CHECK(ultrawide_map_zoom(1.3125f) == 1.3125f);
    CHECK(near(ultrawide_map_zoom(2.0f), 360.0f / 270.0f));
    float m[16];
    identity(m);
    ultrawide_zoom_part(m, 1.3125f);
    CHECK(canvas_x(m, 640.0f, 0.0f) == 840.0f && canvas_y(m, 0.0f, 360.0f) == 472.5f);
    const float pin_x = -300.0f, pin_y = -255.0f;
    CHECK(near(canvas_x(m, pin_x, pin_y) / canvas_y(m, pin_x, pin_y), pin_x / pin_y));
    CHECK(canvas_y(m, pin_x, pin_y) > -360.0f);
}

static void hexagon(float xy[12], float left, float right, float top, float bottom) {
    const float points[12] = {left, top, right, top, right, bottom, left, bottom, left, bottom, left, top};
    memcpy(xy, points, sizeof(points));
}

static uint32_t branch_target(const GuestHook *hook) {
    int32_t rel;
    memcpy(&rel, hook->original + 1, 4);
    return (uint32_t)((int64_t)hook->offset + 5 + rel);
}

static void TestRecords(void) {
    static const uint8_t commit[] = {0x8b, 0x45, 0xd4, 0x42, 0x88, 0x84, 0x2b, 0x20, 0x0b, 0x00, 0x00};
    for (size_t i = 0; i < ULTRAWIDE_RECORD_COUNT; ++i) {
        const UltrawideRecord *record = &ultrawide_records[i];
        GuestHook hooks[ULTRAWIDE_HOOK_COUNT];
        ultrawide_record_hooks(record, hooks);
        for (size_t h = 0; h < ULTRAWIDE_HOOK_COUNT; ++h) CHECK(hooks[h].handler != NULL);
        size_t h = 0;
        for (; h < ULTRAWIDE_FILL_HOOKS; ++h)
            CHECK(hooks[h].length == 5 && hooks[h].original[0] == 0xe8 && branch_target(&hooks[h]) == record->submit);
        CHECK(hooks[h].original[0] == 0xe8 && hooks[h].offset == 0x118e9 && branch_target(&hooks[h]) == 0x110b0);
        ++h;
        CHECK(hooks[h].offset == record->frame_commit && hooks[h].length == sizeof(commit) &&
              !memcmp(hooks[h].original, commit, sizeof(commit)));
        ++h;
        CHECK(hooks[h].original[0] == 0xe8 && branch_target(&hooks[h]) == record->queue);
        for (++h; h < ULTRAWIDE_HOOK_COUNT; ++h)
            CHECK(hooks[h].original[0] == 0xe9 && branch_target(&hooks[h]) == record->model_draw);
        // Fixed distances inside the same functions in every build.
        CHECK(record->fill_calls[0] - record->submit == 0x903);
        CHECK(record->fill_calls[1] - record->submit == 0x9f6);
        CHECK(record->fill_calls[2] - record->submit == 0x6a9);
        CHECK(record->fade_jumps[1] - record->fade_jumps[0] == 0x7c0);
        for (size_t j = 0; j < i; ++j) {
            CHECK(record->image.size != ultrawide_records[j].image.size ||
                  record->image.entry != ultrawide_records[j].image.entry);
        }
    }
}

static void put_call(uint8_t *image, uint32_t site, uint8_t opcode, uint32_t target) {
    const int32_t rel = (int32_t)((int64_t)target - ((int64_t)site + 5));
    image[site] = opcode;
    memcpy(image + site + 1, &rel, 4);
}

static void put_prologue(uint8_t *image, uint32_t at) {
    static const uint8_t prologue[] = {0x55, 0x48, 0x89, 0xe5};
    memcpy(image + at, prologue, sizeof(prologue));
}

/* A synthetic image with a record's sites and the code its handlers rely on. */
static uint8_t *synthetic_image(const UltrawideRecord *record, size_t stub_room) {
    uint8_t *memory = calloc(1, (size_t)record->image.size + stub_room);
    if (!memory) return NULL;
    GuestHook hooks[ULTRAWIDE_HOOK_COUNT];
    ultrawide_record_hooks(record, hooks);
    for (size_t h = 0; h < ULTRAWIDE_HOOK_COUNT; ++h)
        memcpy(memory + hooks[h].offset, hooks[h].original, hooks[h].length);
    put_call(memory, record->widget_draw_call, 0xe8, record->model_draw);
    put_call(memory, record->map_draw_call, 0xe8, record->model_draw);
    put_call(memory, record->facility_draw_call, 0xe8, record->model_draw);
    memory[record->model_draw + 0xe5] = 0xe8;
    put_prologue(memory, record->worldmap_draw);
    put_prologue(memory, record->worldmap_draw + 0x270);
    static const uint8_t set_from_pass[] = {0x48, 0x8d, 0xbc, 0x08, 0xb0, 0x02, 0x00, 0x00};
    memcpy(memory + record->part_queue_call - sizeof(set_from_pass), set_from_pass, sizeof(set_from_pass));
    static const uint8_t buckets[] = {0x49, 0x8b, 0x8f, 0xb0, 0x02, 0x00, 0x00,
                                      0x49, 0x8b, 0x97, 0xb8, 0x02, 0x00, 0x00};
    static const uint8_t bucket_end[] = {0x48, 0x8b, 0x44, 0x19, 0x10}, bucket_begin[] = {0x48, 0x2b, 0x44, 0x19, 0x08};
    memcpy(memory + record->queue + 0x57, buckets, sizeof(buckets));
    memcpy(memory + record->queue + 0x9b, bucket_end, sizeof(bucket_end));
    memcpy(memory + record->queue + 0xa9, bucket_begin, sizeof(bucket_begin));
    return memory;
}

static void TestInstall(void) {
    const UltrawideRecord *record = &ultrawide_records[0];
    const size_t stub_room = ultrawide_hooks_stub_bytes();
    uint8_t *memory = synthetic_image(record, stub_room);
    CHECK(memory != NULL);
    if (!memory) return;
    GuestHook hooks[ULTRAWIDE_HOOK_COUNT];
    ultrawide_record_hooks(record, hooks);
    char detail[400];

    CHECK(!ultrawide_hooks_install(memory, record->image.size - 1, record->image.entry, memory + record->image.size,
                                   stub_room, 1.3125, detail, sizeof(detail)));
    CHECK(strstr(detail, "no hook record") != NULL);

    // The frame commit differs: that group stays off with its sites untouched; the others install.
    const size_t commit = ULTRAWIDE_FILL_HOOKS + 1;
    memory[hooks[commit].offset + 4] ^= 1;
    CHECK(ultrawide_hooks_install(memory, record->image.size, record->image.entry, memory + record->image.size,
                                  stub_room, 1.3125, detail, sizeof(detail)));
    CHECK(strstr(detail, "fill hooks installed") && strstr(detail, "world window hooks off (frame commit: ") &&
          strstr(detail, "widget, map and fade hooks installed"));
    for (size_t h = 0; h < ULTRAWIDE_HOOK_COUNT; ++h) {
        const int world = h >= ULTRAWIDE_FILL_HOOKS && h < ULTRAWIDE_FILL_HOOKS + ULTRAWIDE_WORLD_HOOKS;
        CHECK(memory[hooks[h].offset] == (world ? hooks[h].original[0] : 0xe9));
    }
    free(memory);

    // The route map draw is not where the record says: the part group stays off.
    memory = synthetic_image(record, stub_room);
    CHECK(memory != NULL);
    if (!memory) return;
    memory[record->worldmap_draw] = 0x90;
    ultrawide_hooks_install(memory, record->image.size, record->image.entry, memory + record->image.size, stub_room,
                            1.3125, detail, sizeof(detail));
    CHECK(strstr(detail, "widget, map and fade hooks off (route map draw") != NULL);
    free(memory);

    // The queue's priority buckets are not laid out as the backdrop move expects: the part group stays off.
    memory = synthetic_image(record, stub_room);
    CHECK(memory != NULL);
    if (!memory) return;
    memory[record->queue + 0x9b + 4] = 0x18;
    ultrawide_hooks_install(memory, record->image.size, record->image.entry, memory + record->image.size, stub_room,
                            1.3125, detail, sizeof(detail));
    CHECK(strstr(detail, "widget, map and fade hooks off (render queue buckets") != NULL);
    free(memory);

    // The facility frame's call is not a call of the model draw: the part group stays off.
    memory = synthetic_image(record, stub_room);
    CHECK(memory != NULL);
    if (!memory) return;
    memory[record->facility_draw_call] = 0x90;
    ultrawide_hooks_install(memory, record->image.size, record->image.entry, memory + record->image.size, stub_room,
                            1.3125, detail, sizeof(detail));
    CHECK(strstr(detail, "widget, map and fade hooks off (facility frame draw") != NULL);
    free(memory);
}

/* Guest structures the part hook reads: the part draw's frame, whose caller frame returns into the owner. */
typedef struct {
    uint64_t model_frame[2];
    uint64_t part_frame[2];
    uint8_t command[0x280];
    uint8_t part[0x100];
} PartCall;

static void part_call(PartCall *call, uint64_t base, uint64_t owner_return, int16_t layer) {
    memset(call, 0, sizeof(*call));
    call->model_frame[1] = owner_return;
    call->part_frame[0] = (uint64_t)(uintptr_t)call->model_frame;
    call->part_frame[1] = base + ultrawide_records[0].model_draw + 0xea;
    memcpy(call->part + 0xc4, &layer, 2);
    float m[16];
    identity(m);
    memcpy(call->command + 0x10, m, sizeof(m));
}

static void run_part(PartCall *call, uint64_t model) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rsi = (uint64_t)(uintptr_t)call->command;
    regs.r12 = model;
    regs.r14 = (uint64_t)(uintptr_t)call->part;
    regs.rbp = (uint64_t)(uintptr_t)call->part_frame;
    ultrawide_part_hook(&regs);
    CHECK(regs.skip == 0);
}

static float command_float(const PartCall *call, size_t offset) {
    float value;
    memcpy(&value, call->command + offset, 4);
    return value;
}

static void put_float(uint8_t *at, float value) {
    memcpy(at, &value, 4);
}

static void put_bounds(PartCall *call, float x0, float y0, float x1, float y1) {
    put_float(call->command + 0x1a0, x0);
    put_float(call->command + 0x1a4, y0);
    put_float(call->command + 0x1a8, x1);
    put_float(call->command + 0x1ac, y1);
}

/* Model data a part's command points at: one keyframe of polygons, each with its own six positions. */
enum { FAKE_POLYGONS = 4 };
typedef struct {
    uint8_t holder[0x160], data[0x100], polygons[FAKE_POLYGONS * 16], keys[0x20];
    float positions[FAKE_POLYGONS][12];
} FakeModel;

static void attach_model(PartCall *call, FakeModel *model) {
    const uint64_t data = (uint64_t)(uintptr_t)model->data, polygons = (uint64_t)(uintptr_t)model->polygons,
                   positions = (uint64_t)(uintptr_t)model->positions, keys = (uint64_t)(uintptr_t)model->keys,
                   holder = (uint64_t)(uintptr_t)model->holder;
    memcpy(model->holder + 0x158, &data, 8);
    memcpy(model->data + 0xd0, &polygons, 8);
    memcpy(model->data + 0xc0, &positions, 8);
    for (uint16_t i = 0; i < FAKE_POLYGONS; ++i) memcpy(model->polygons + i * 16 + 0xe, &i, 2);
    const int32_t first = 0;
    const uint16_t count = FAKE_POLYGONS;
    memcpy(model->keys + 0x10, &first, 4);
    memcpy(model->keys + 0x16, &count, 2);
    memcpy(call->command + 0x60, &holder, 8);
    memcpy(call->command + 0x1b0, &keys, 8);
}

static int same_y(const float a[12], const float b[12]) {
    for (int i = 1; i < 12; i += 2)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* Every x of the six positions at -edge or +edge, both sides present: a band across the whole frame. */
static int spans_frame(const float xy[12], float edge) {
    int left = 0, right = 0;
    for (int i = 0; i < 12; i += 2) {
        left += near(xy[i], -edge);
        right += near(xy[i], edge);
    }
    return left > 0 && right > 0 && left + right == 6;
}

/* A fixed screen's black matte: top and bottom bands across the 16:9 at the frame's top and bottom (+-1), with their
 * colours (opaque black) and polygon records. */
typedef struct {
    FakeModel model;
    uint8_t colours[FAKE_POLYGONS][24];
} FakeMatte;

static void fake_matte(FakeMatte *matte, float inner) {
    memset(matte, 0, sizeof(*matte));
    for (int i = 0; i < FAKE_POLYGONS; ++i) {
        hexagon(matte->model.positions[i], -406.5f, 406.5f, i % 2 ? 1.0f : -inner, i % 2 ? inner : -1.0f);
        for (int j = 3; j < 24; j += 4) matte->colours[i][j] = 0xff;
        const uint16_t index = (uint16_t)i;
        memcpy(matte->model.polygons + i * 16 + 0xc, &index, 2);
        memcpy(matte->model.polygons + i * 16 + 0xe, &index, 2);
    }
    const uint64_t colours = (uint64_t)(uintptr_t)matte->colours;
    memcpy(matte->model.data + 0xb0, &colours, 8);
}

static void TestPartHook(void) {
    const UltrawideRecord *record = &ultrawide_records[0];
    const size_t stub_room = ultrawide_hooks_stub_bytes();
    uint8_t *memory = synthetic_image(record, stub_room);
    CHECK(memory != NULL);
    if (!memory) return;
    char detail[400];
    ultrawide_hooks_install(memory, record->image.size, record->image.entry, memory + record->image.size, stub_room,
                            1.3125, detail, sizeof(detail));
    const uint64_t base = (uint64_t)(uintptr_t)memory;
    const uint64_t widget_owner = base + record->widget_draw_call + 5;
    static PartCall call;
    static uint8_t widget[0x300];

    // The narration (0x26) stretches as before; the bottom dialogue (0x25) too; a centred 0x25 band keeps its width.
    const struct {
        uint32_t type;
        float top, bottom, stretch;
    } widgets[] = {
        {0x26, 255.0f, 360.0f, 1.3125f},
        {0x26, -30.0f, 30.0f, 1.3125f},
        {0x25, 255.0f, 360.0f, 1.3125f},
        {0x25, -40.0f, 40.0f, 1.0f},
        {0x29, -10.0f, 10.0f, 1.0f},
    };
    for (size_t i = 0; i < sizeof(widgets) / sizeof(widgets[0]); ++i) {
        memcpy(widget + 0xe0, &widgets[i].type, 4);
        for (int animated = 0; animated < 2; ++animated) {
            part_call(&call, base, widget_owner, 0x28);
            put_bounds(&call, -709.0f, widgets[i].top, 709.0f, widgets[i].bottom);
            if (animated) put_float(call.command + 0x10, 0.2f); // opening: the matrix scale changes, the rule does not
            run_part(&call, (uint64_t)(uintptr_t)widget + 0x100);
            CHECK(near(command_float(&call, 0x10), (animated ? 0.2f : 1.0f) * widgets[i].stretch));
        }
    }
    // Narrower parts of the same widgets are never stretched.
    memcpy(widget + 0xe0, &widgets[0].type, 4);
    part_call(&call, base, widget_owner, 0x28);
    put_bounds(&call, -210.0f, 255.0f, 210.0f, 360.0f);
    run_part(&call, (uint64_t)(uintptr_t)widget + 0x100);
    CHECK(command_float(&call, 0x10) == 1.0f);

    // The town map: unchanged, uniform about the canvas centre, face included.
    static uint8_t map[0x500];
    part_call(&call, base, base + record->map_draw_call + 5, 0x26);
    put_float(call.command + 0x40, 100.0f);
    put_float(call.command + 0x44, -200.0f);
    run_part(&call, (uint64_t)(uintptr_t)map + 0x100);
    CHECK(command_float(&call, 0x10) == 1.3125f && command_float(&call, 0x24) == 1.3125f);
    CHECK(command_float(&call, 0x40) == 131.25f && command_float(&call, 0x44) == -262.5f);
    part_call(&call, base, base + 0x1234, 0x26);
    run_part(&call, (uint64_t)(uintptr_t)map + 0x3d0);
    CHECK(command_float(&call, 0x10) == 1.3125f);

    // A frame with the town map: not the route map, nothing clipped at 21:9.
    uint8_t *passes = fake_passes();
    CHECK(passes != NULL);
    if (!passes) return;
    float v[4];
    commit_frame(passes);
    set_viewport(passes, 0x26, v);
    CHECK(v[0] == 0.0f && v[2] == 2520.0f);
    free(passes);

    // A frame with the route map: its parts are left alone, the world and pass 0x26 become the centred 16:9 at commit,
    // the other screen passes (the skip prompt) keep the whole width.
    part_call(&call, base, base + record->worldmap_draw + 0x13d + 5, 0x26);
    put_bounds(&call, -900.0f, -300.0f, 900.0f, 300.0f);
    run_part(&call, 0x5000);
    CHECK(command_float(&call, 0x10) == 1.0f);
    passes = fake_passes();
    CHECK(passes != NULL);
    if (!passes) return;
    commit_frame(passes);
    set_viewport(passes, 0x26, v);
    CHECK(v[0] == 300.0f && v[2] == 2220.0f);
    set_viewport(passes, 0x24, v);
    CHECK(v[0] == 300.0f && v[2] == 2220.0f);
    set_viewport(passes, 0x28, v);
    CHECK(v[0] == 0.0f && v[2] == 2520.0f);
    // The next frame without it: back to the whole width.
    commit_frame(passes);
    free(passes);
    passes = fake_passes();
    CHECK(passes != NULL);
    if (!passes) return;
    commit_frame(passes);
    set_viewport(passes, 0x26, v);
    CHECK(v[0] == 0.0f && v[2] == 2520.0f);

    // The playfield layer ends at x 470 (world) right of centre on a wide stage: the playfield-plane layers stop there,
    // the distant ones (drawn first) keep the whole width, so nothing behind is cleared.
    static uint8_t camera[0x100];
    run_camera(camera, 0.0f, 812.0f, -3291, 3324);
    part_call(&call, base, base + 0x2222, 0x10);
    put_bounds(&call, -900.0f, -200.0f, 470.0f, 200.0f);
    run_part(&call, 0x6000);
    commit_frame(passes);
    float w[4];
    const float cut = (470.0f / 532.875f + 1.0f) * 0.5f * 2520.0f;
    for (int id = 0x0a; id <= 0x10; ++id) {
        set_viewport(passes, id, v);
        CHECK(v[0] == 0.0f && near(v[2], roundf(cut)));
    }
    for (int id = 0x01; id <= 0x09; ++id) {
        set_viewport(passes, id, w);
        CHECK(w[0] == 0.0f && w[2] == 2520.0f);
    }
    const int unknown[] = {0x11, 0x17, 0x23, 0x24, 0x00, 0x25, 0x26, 0x28};
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); ++i) {
        set_viewport(passes, unknown[i], w);
        CHECK(w[0] == 0.0f && w[2] == 2520.0f); // layers of unknown parallax and the screen UI are never clipped
    }
    free(passes);

    // The loading fade: its black backing and decorative line, both reaching the 16:9 edges, extend to the frame
    // edges like any other band; the crest stays.
    static uint8_t fade[0x200];
    const uint64_t vtable = 0xb8e1e8;
    memcpy(fade, &vtable, 8);
    const uint64_t fade_model = (uint64_t)(uintptr_t)fade + 0xb0;
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rdi = fade_model;
    ultrawide_fade_hook(&regs);
    static uint8_t holder[0x160], data[0x100], polygons[3 * 16], keys[0x20];
    static float positions[3][12];
    hexagon(positions[0], -210.0f, 210.0f, -230.0f, 270.0f); // crest, clear of the edges
    hexagon(positions[1], -640.0f, 640.0f, -360.0f, 360.0f); // black backing, the whole canvas
    hexagon(positions[2], -640.0f, 640.0f, -102.0f, -100.0f); // decorative line
    for (uint16_t i = 0; i < 3; ++i) memcpy(polygons + i * 16 + 0xe, &i, 2);
    const uint64_t data_address = (uint64_t)(uintptr_t)data, polygons_address = (uint64_t)(uintptr_t)polygons,
                   positions_address = (uint64_t)(uintptr_t)positions, keys_address = (uint64_t)(uintptr_t)keys,
                   holder_address = (uint64_t)(uintptr_t)holder;
    memcpy(holder + 0x158, &data_address, 8);
    memcpy(data + 0xd0, &polygons_address, 8);
    memcpy(data + 0xc0, &positions_address, 8);
    const int32_t first = 0;
    const uint16_t count = 3;
    memcpy(keys + 0x10, &first, 4);
    memcpy(keys + 0x16, &count, 2);
    const float edge = 640.0f * 1.3125f;
    for (int pass = 0; pass < 2; ++pass) {
        part_call(&call, base, base + 0x1234, 0x28);
        memcpy(call.command + 0x60, &holder_address, 8);
        memcpy(call.command + 0x1b8, &keys_address, 8);
        put_bounds(&call, -600.0f, -360.0f, 600.0f, 360.0f);
        run_part(&call, fade_model);
        CHECK(positions[0][0] == -210.0f && positions[0][2] == 210.0f); // the crest is untouched
        CHECK(near(positions[1][0], -edge) && near(positions[1][2], edge)); // the backing reaches both edges
        CHECK(positions[1][1] == -360.0f && positions[1][5] == 360.0f); // its height is unchanged
        CHECK(near(positions[2][0], -edge) && near(positions[2][2], edge)); // the line reaches both edges
        CHECK(positions[2][1] == -102.0f && positions[2][5] == -100.0f);
    }
    // Another object at that address later is not taken for the fade.
    const uint64_t other = 0x9999;
    memcpy(fade, &other, 8);
    part_call(&call, base, base + 0x1234, 0x28);
    put_bounds(&call, -600.0f, -360.0f, 600.0f, 360.0f);
    run_part(&call, fade_model);
    CHECK(command_float(&call, 0x1a0) == -600.0f);
    free(memory);
}

static void band(float u[6], float v[6], float left, float right, float bottom, float top) {
    const float x[6] = {left, right, right, left, left, left}, y[6] = {top, top, bottom, bottom, bottom, top};
    memcpy(u, x, sizeof(x));
    memcpy(v, y, sizeof(y));
}

static void TestExtendBand(void) {
    const float wide = 1.3125f, half = 1.0f / wide;
    float u[6], v[6], before[6];
    // A bar across the 16:9 reaches both frame edges at 21:9 and at 32:9; only x changes.
    band(u, v, -half, half, 0.40f, 0.43f);
    CHECK(ultrawide_extend_band(u, v, half, wide) && near(u[0], -1.0f) && near(u[1], 1.0f) && near(u[2], 1.0f));
    CHECK(v[0] == 0.43f && v[2] == 0.40f);
    band(u, v, -0.5f, 0.5f, 0.40f, 0.43f);
    CHECK(ultrawide_extend_band(u, v, 0.5f, 2.0f) && near(u[0], -1.0f) && near(u[1], 1.0f));
    // A band already wider than the 16:9 (the dialogue backing, +-708.75 of +-640) keeps that proportion.
    const float backing = 708.75f / 640.0f * half;
    band(u, v, -backing, backing, -0.43f, -0.30f);
    CHECK(ultrawide_extend_band(u, v, half, wide) && near(u[1], backing * wide) && near(u[0], -backing * wide));
    // A line from a title plate to the right edge: only its outer end moves; the short piece on the left of the plate
    // moves only its outer end too.
    band(u, v, -0.25f, half, 0.41f, 0.412f);
    CHECK(ultrawide_extend_band(u, v, half, wide) && u[0] == -0.25f && near(u[1], 1.0f));
    band(u, v, -half, -0.98f * half, 0.41f, 0.412f);
    CHECK(ultrawide_extend_band(u, v, half, wide) && near(u[0], -1.0f) && u[1] == -0.98f * half);
    // A cap at the edge (taller than wide) moves whole to the frame edge, keeping its size.
    band(u, v, 0.70f, half, 0.30f, 0.45f);
    CHECK(ultrawide_extend_band(u, v, half, wide) && near(u[1], 1.0f) && near(u[1] - u[0], half - 0.70f));
    // Untouched: an ornament or plate clear of the edges, a band already at the frame edge, and any band at 16:9.
    const float untouched[][2] = {{-0.2f, 0.2f}, {-0.985f * half, -0.5f}, {-1.0f, 1.0f}};
    for (size_t i = 0; i < sizeof(untouched) / sizeof(untouched[0]); ++i) {
        band(u, v, untouched[i][0], untouched[i][1], 0.40f, 0.43f);
        memcpy(before, u, sizeof(before));
        CHECK(!ultrawide_extend_band(u, v, half, wide) && !memcmp(u, before, sizeof(before)));
    }
    band(u, v, -1.0f, 1.0f, 0.40f, 0.43f);
    CHECK(!ultrawide_extend_band(u, v, 1.0f, 1.0f) && u[0] == -1.0f);
}

/* The town facility frame (the inn's bars, lines, title plate and menus in one part): the bars and lines reach the
 * frame edges at 21:9 and 32:9, nothing else moves, nothing at 16:9. */
static void TestFacilityFrame(void) {
    const UltrawideRecord *record = &ultrawide_records[0];
    const size_t stub_room = ultrawide_hooks_stub_bytes();
    uint8_t *memory = synthetic_image(record, stub_room);
    CHECK(memory != NULL);
    if (!memory) return;
    const uint64_t base = (uint64_t)(uintptr_t)memory;
    static PartCall call;
    static FakeModel inn;
    char detail[400];
    const float scales[] = {1.3125f, 2.0f, 1.0f};
    for (size_t s = 0; s < sizeof(scales) / sizeof(scales[0]); ++s) {
        ultrawide_hooks_install(memory, record->image.size, record->image.entry, memory + record->image.size,
                                stub_room, scales[s], detail, sizeof(detail));
        memset(&inn, 0, sizeof(inn));
        hexagon(inn.positions[0], -640.0f, 640.0f, 360.0f, 321.0f);   // top bar
        hexagon(inn.positions[1], -640.0f, 640.0f, -321.0f, -360.0f); // bottom bar
        hexagon(inn.positions[2], -150.0f, 640.0f, 323.0f, 321.0f);   // the title's long line
        hexagon(inn.positions[3], -630.0f, -210.0f, 347.0f, 295.0f);  // the title plate, its ornament near the edge
        float original[FAKE_POLYGONS][12];
        memcpy(original, inn.positions, sizeof(original));
        for (int frame = 0; frame < 3; ++frame) { // once, then never again
            part_call(&call, base, base + record->facility_draw_call + 5, 0x26);
            attach_model(&call, &inn);
            run_part(&call, 0x7100);
            CHECK(command_float(&call, 0x10) == 1.0f && command_float(&call, 0x24) == 1.0f); // no part scales
        }
        const float edge = 640.0f * scales[s];
        CHECK(near(inn.positions[0][0], -edge) && near(inn.positions[0][2], edge));
        CHECK(near(inn.positions[1][0], -edge) && near(inn.positions[1][2], edge));
        CHECK(inn.positions[2][0] == -150.0f && near(inn.positions[2][2], edge));
        CHECK(!memcmp(inn.positions[3], original[3], sizeof(original[3])));
        for (int i = 0; i < FAKE_POLYGONS; ++i) CHECK(same_y(inn.positions[i], original[i]));
    }
    // Another owner's part with the same data (a widget or the world) is left alone.
    memset(&inn, 0, sizeof(inn));
    hexagon(inn.positions[0], -640.0f, 640.0f, 360.0f, 321.0f);
    ultrawide_hooks_install(memory, record->image.size, record->image.entry, memory + record->image.size, stub_room,
                            1.3125, detail, sizeof(detail));
    part_call(&call, base, base + 0x1234, 0x26);
    attach_model(&call, &inn);
    run_part(&call, 0x7100);
    CHECK(inn.positions[0][0] == -640.0f && inn.positions[0][2] == 640.0f);
    free(memory);
}

static void TestWorldPassSpan(void) {
    const float wide = 1.3125f;
    float parallax[0x2b] = {0};
    for (int id = 1; id <= 0x10; ++id) parallax[id] = id < 0x0a ? 0.4f : 1.0f;
    const UltrawideSpan narrow = {0.1f, 0.85f}, centre = ultrawide_centre_span(wide);
    UltrawideWorldFrame world = {narrow, parallax, 0x1fffe, 1.0f, 0, 0};
    // Background layers keep the whole Hor+ width when the playfield is narrower; the playfield and the layers
    // moving with it stop at the playfield; unknown layers, pass 0 and the screen passes are never clipped.
    UltrawideSpan s = ultrawide_world_pass_span(0x01, &world, wide);
    CHECK(s.left == 0.0f && s.right == 1.0f);
    s = ultrawide_world_pass_span(0x10, &world, wide);
    CHECK(s.left == 0.1f && s.right == 0.85f);
    s = ultrawide_world_pass_span(0x0a, &world, wide);
    CHECK(s.left == 0.1f && s.right == 0.85f);
    const uint32_t untouched[] = {0x00, 0x17, 0x23, 0x24, 0x2a, 0x25, 0x26, 0x27, 0x28, 0x29};
    for (size_t i = 0; i < sizeof(untouched) / sizeof(untouched[0]); ++i) {
        s = ultrawide_world_pass_span(untouched[i], &world, wide);
        CHECK(s.left == 0.0f && s.right == 1.0f);
    }
    CHECK(ultrawide_layer_follows_playfield(1.0f, 1.0f) && ultrawide_layer_follows_playfield(1.2f, 1.0f));
    CHECK(!ultrawide_layer_follows_playfield(0.98f, 1.0f) && !ultrawide_layer_follows_playfield(0.0f, 1.0f));
    CHECK(ultrawide_layer_follows_playfield(0.9f, 0.9f) && !ultrawide_layer_follows_playfield(0.8f, 0.9f));
    // An illustration screen: every world pass but pass 0 keeps its 16:9.
    world.illustration = 1;
    s = ultrawide_world_pass_span(0x01, &world, wide);
    CHECK(s.left == centre.left && s.right == centre.right);
    s = ultrawide_world_pass_span(0x00, &world, wide);
    CHECK(s.left == 0.0f && s.right == 1.0f);
    s = ultrawide_world_pass_span(0x28, &world, wide);
    CHECK(s.left == 0.0f && s.right == 1.0f);
    // The route map: world and pass 0x26 in the 16:9, the rest of the UI whole.
    world.illustration = 0;
    world.route_map = 1;
    s = ultrawide_world_pass_span(0x26, &world, wide);
    CHECK(s.left == centre.left && s.right == centre.right);
    s = ultrawide_world_pass_span(0x28, &world, wide);
    CHECK(s.left == 0.0f && s.right == 1.0f);
    // The town map's share at 32:9 applies to every world pass, background included; at 21:9 it is 1.
    world.route_map = 0;
    world.map_share = ultrawide_map_zoom(2.0f) / 2.0f;
    s = ultrawide_world_pass_span(0x01, &world, 2.0f);
    CHECK(near(s.right - s.left, world.map_share));
    world.map_share = ultrawide_map_zoom(wide) / wide;
    s = ultrawide_world_pass_span(0x01, &world, wide);
    CHECK(s.left == 0.0f && s.right == 1.0f);
}

static void TestCoverHelpers(void) {
    const float wide = 1.3125f, super = 2.0f;
    CHECK(ultrawide_covers_view(-0.77f, 0.77f, -0.9f, 0.9f, wide));     // a backdrop over the whole 16:9 panel
    CHECK(ultrawide_covers_view(-0.5f, 0.5f, -0.9f, 0.9f, super));
    CHECK(!ultrawide_covers_view(-0.3f, 0.6f, -1.0f, 1.0f, wide));      // an NPC
    CHECK(!ultrawide_covers_view(-0.77f, 0.77f, 0.7f, 1.0f, wide));     // a frame line or a title box
    CHECK(!ultrawide_covers_view(-0.77f, 0.2f, -0.9f, 0.9f, wide));
    float m[16];
    identity(m);
    m[0] = 1.0f / 532.875f;
    m[12] = 0.25f;
    ultrawide_cover_view(m, wide);
    // Uniform about the frame centre: the 16:9 edge lands on the frame edge, height scales the same, depth stays.
    CHECK(near(406.0f * m[0], 1.0f) && m[5] == wide && m[10] == 1.0f && near(m[12], 0.25f * wide));
    identity(m);
    ultrawide_cover_view(m, super);
    CHECK(m[0] == 2.0f && m[5] == 2.0f && m[15] == 1.0f);
    // The rows clip keeps every point's pixel row: y = 0.5 sits at row 810 before and after.
    float viewport[4] = {0.0f, 0.0f, 2520.0f, 1080.0f}, projection[16], vp[16];
    identity(projection);
    identity(vp);
    CHECK(ultrawide_clip_rows(viewport, projection, vp, 0.9f));
    CHECK(viewport[1] == 54.0f && viewport[3] == 1026.0f && viewport[0] == 0.0f && viewport[2] == 2520.0f);
    const float clip_y = 0.5f * vp[5];
    CHECK(near(viewport[1] + (clip_y + 1.0f) * 0.5f * (viewport[3] - viewport[1]), 810.0f));
    CHECK(projection[0] == 1.0f); // x untouched
    float full[4] = {0.0f, 0.0f, 2520.0f, 1080.0f};
    CHECK(!ultrawide_clip_rows(full, projection, vp, 1.0f) && full[3] == 1080.0f);
}

/* A part of pass `id` as the queue call sees it: rax = the pass, rdi = its current set, edx = the priority bucket. */
static GuestRegs queue_model_part(PartCall *call, uint64_t base, uint8_t *passes, int16_t id, uint32_t priority,
                                  const float bounds[4], uint64_t model, FakeModel *data) {
    part_call(call, base, base + 0x2222, id);
    put_bounds(call, bounds[0], bounds[1], bounds[2], bounds[3]);
    if (data) attach_model(call, data);
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rsi = (uint64_t)(uintptr_t)call->command;
    regs.r12 = model;
    regs.r14 = (uint64_t)(uintptr_t)call->part;
    regs.rbp = (uint64_t)(uintptr_t)call->part_frame;
    regs.rax = (uint64_t)(uintptr_t)passes + (uint64_t)id * PASS_BYTES;
    regs.rdi = regs.rax + 0x2b0 + 0x2d0;
    regs.rdx = priority;
    ultrawide_part_hook(&regs);
    CHECK(regs.skip == 0);
    return regs;
}

static GuestRegs queue_part(PartCall *call, uint64_t base, uint8_t *passes, int16_t id, uint32_t priority,
                            const float bounds[4]) {
    return queue_model_part(call, base, passes, id, priority, bounds, 0x7000 + (uint64_t)id, NULL);
}

static uint64_t current_set(const uint8_t *passes, int id) {
    return (uint64_t)(uintptr_t)passes + (uint64_t)id * PASS_BYTES + 0x2b0 + 0x2d0;
}

/* An illustration screen: a part covering the 16:9 is queued in the set of the pass before its own, after what is
 * already there, and drawn there with a cover scale; the NPC and everything else keep their pass, the 16:9 and their
 * own transform. */
static void TestBackdrop(void) {
    const UltrawideRecord *record = &ultrawide_records[0];
    const size_t stub_room = ultrawide_hooks_stub_bytes();
    uint8_t *memory = synthetic_image(record, stub_room);
    uint8_t *passes = fake_passes();
    CHECK(memory != NULL && passes != NULL);
    if (!memory || !passes) return;
    char detail[400];
    ultrawide_hooks_install(memory, record->image.size, record->image.entry, memory + record->image.size, stub_room,
                            1.3125, detail, sizeof(detail));
    const uint64_t base = (uint64_t)(uintptr_t)memory;
    static uint8_t camera[0x100];
    static PartCall call;
    const uint64_t set0 = current_set(passes, 0), set1 = current_set(passes, 1);
    const float room[4] = {-406.5f, -0.9f, 406.5f, 0.9f}, npc[4] = {-150.0f, -1.0f, 300.0f, 1.0f},
                title[4] = {-400.0f, 0.8f, 0.0f, 1.0f};
    run_camera(camera, 0.0f, 1280.0f, -640, 640); // the previous frame: one screen, no playfield layer
    commit_frame(passes);
    // One stage layer (the guild knight, Morgan): the room goes to pass 0, the NPC and the title box stay.
    const float *const parts[] = {room, npc, title};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        const GuestRegs regs = queue_part(&call, base, passes, 0x01, 1, parts[i]);
        CHECK(regs.rdi == (i == 0 ? set0 : set1) && regs.rdx == 1);
        CHECK(command_float(&call, 0x10) == 1.0f && command_float(&call, 0x24) == 1.0f); // no part is rescaled
    }
    run_camera(camera, 0.0f, 1280.0f, -640, 640);
    commit_frame(passes);
    float projection[16], v[4];
    memcpy(projection, (const void *)(uintptr_t)(set0 + 0x100), sizeof(projection));
    CHECK(near(projection[0] * 532.875f, 1.3125f) && near(projection[5], 1.3125f * 1080.0f / 972.0f));
    memcpy(v, (const void *)(uintptr_t)(set0 + 0x284), sizeof(v));
    // The whole width, the backdrop's own rows (+-0.9: 54..1026): the top and bottom framing stays.
    CHECK(v[0] == 0.0f && v[2] == 2520.0f && v[1] == 54.0f && v[3] == 1026.0f);
    set_viewport(passes, 0x01, v);
    CHECK(v[0] == 300.0f && v[2] == 2220.0f); // the NPC's pass keeps the 16:9
    set_viewport(passes, 0x25, v);
    CHECK(v[0] == 0.0f && v[2] == 2520.0f); // titles and text untouched
    memcpy(projection, (const void *)(uintptr_t)(passes + PASS_BYTES + 0x100), sizeof(projection));
    CHECK(projection[0] * 532.875f == 1.0f); // the game's own pass is never changed

    // The guild knight (backdrop y -0.89..0.89) and Morgan (y -0.90..0.89), the same path. The screen's own black
    // bands (the backdrop's model, drawn after the NPC) and what follows them in pass 0x01 (the plate's body) go to
    // pass 0x02's set, unused, which draws pass 0x01's view unzoomed at the whole width: the bands reach the frame
    // edges, only their x changing. Morgan's glow goes with the backdrop; the frame (another model: title ornaments and
    // long lines, drawn last) goes to the same set, its lines reaching the frame edges, its plate and ornaments
    // unchanged. The NPC and every part keep their transform. The backdrop line prints once per screen.
    const uint64_t set2 = current_set(passes, 2);
    static FakeModel lines;
    static FakeMatte mattes;
    const float bars[4] = {-406.5f, -1.0f, 406.5f, 1.0f}, frame_bounds[4] = {-406.5f, -0.9f, 406.5f, 0.97f};
    const float backdrops[2][4] = {{-406.5f, -0.89f, 406.5f, 0.89f}, {-406.5f, -0.90f, 406.5f, 0.89f}};
    for (int screen = 0; screen < 2; ++screen) {
        const uint64_t backdrop_model = 0x7a00 + (uint64_t)screen, frame_model = 0x7c00 + (uint64_t)screen;
        memset(&lines, 0, sizeof(lines));
        hexagon(lines.positions[0], -100.0f, 406.5f, 0.895f, 0.89f);   // top line, from the plate to the edge
        hexagon(lines.positions[1], -406.5f, 406.5f, -0.89f, -0.895f); // bottom line
        hexagon(lines.positions[2], -380.0f, -120.0f, 0.97f, 0.85f);   // title plate
        hexagon(lines.positions[3], -397.0f, -380.0f, 0.97f, 0.85f);   // its left ornament, near the 16:9 edge
        fake_matte(&mattes, backdrops[screen][3]);
        float original[FAKE_POLYGONS][12], matte_original[FAKE_POLYGONS][12];
        uint8_t colours[sizeof(mattes.colours)], polygons[sizeof(mattes.model.polygons)];
        memcpy(original, lines.positions, sizeof(original));
        memcpy(matte_original, mattes.model.positions, sizeof(matte_original));
        memcpy(colours, mattes.colours, sizeof(colours));
        memcpy(polygons, mattes.model.polygons, sizeof(polygons));
        const unsigned printed = ultrawide_debug_saulo_backdrop_lines();
        for (int frame_index = 0; frame_index < 3; ++frame_index) {
            GuestRegs regs = queue_model_part(&call, base, passes, 0x01, 1, backdrops[screen], backdrop_model, NULL);
            CHECK(regs.rdi == set0);
            regs = queue_model_part(&call, base, passes, 0x01, 1, npc, 0x7e00, NULL);
            CHECK(regs.rdi == set1 && regs.rdx == 1);
            CHECK(command_float(&call, 0x10) == 1.0f && command_float(&call, 0x24) == 1.0f);
            if (screen == 1) {
                regs = queue_model_part(&call, base, passes, 0x01, 1, room, backdrop_model, NULL); // Morgan's glow
                CHECK(regs.rdi == set0);
            }
            regs = queue_model_part(&call, base, passes, 0x01, 1, bars, backdrop_model, &mattes.model);
            CHECK(regs.rdi == set2 && regs.rdx == 1);
            CHECK(command_float(&call, 0x10) == 1.0f && command_float(&call, 0x24) == 1.0f &&
                  command_float(&call, 0x40) == 0.0f);
            regs = queue_model_part(&call, base, passes, 0x01, 1, title, 0x7d00, NULL); // the plate's body
            CHECK(regs.rdi == set2 && regs.rdx == 1);
            CHECK(command_float(&call, 0x10) == 1.0f && command_float(&call, 0x24) == 1.0f);
            regs = queue_model_part(&call, base, passes, 0x01, 1, frame_bounds, frame_model, &lines);
            CHECK(regs.rdi == set2 && regs.rdx == 1);
            CHECK(command_float(&call, 0x10) == 1.0f && command_float(&call, 0x24) == 1.0f);
            run_camera(camera, 0.0f, 1280.0f, -640, 640);
            commit_frame(passes);
        }
        CHECK(ultrawide_debug_saulo_backdrop_lines() == printed + 1);
        const float edge = 406.5f * 1.3125f;
        // Top (odd) and bottom (even) bars: the whole frame width at 21:9; y, height, colour and alpha as they were.
        for (int i = 0; i < FAKE_POLYGONS; ++i)
            CHECK(spans_frame(mattes.model.positions[i], edge) && same_y(mattes.model.positions[i], matte_original[i]));
        CHECK(!memcmp(mattes.colours, colours, sizeof(colours)));
        CHECK(!memcmp(mattes.model.polygons, polygons, sizeof(polygons)));
        CHECK(lines.positions[0][0] == -100.0f && near(lines.positions[0][2], edge));
        CHECK(near(lines.positions[1][0], -edge) && near(lines.positions[1][2], edge));
        CHECK(!memcmp(lines.positions[2], original[2], sizeof(original[2])));
        CHECK(!memcmp(lines.positions[3], original[3], sizeof(original[3])));
        for (int i = 0; i < FAKE_POLYGONS; ++i) CHECK(same_y(lines.positions[i], original[i]));
        memcpy(projection, (const void *)(uintptr_t)(set2 + 0x1c0), sizeof(projection));
        CHECK(projection[0] * 532.875f == 1.0f && projection[5] == 1.0f); // pass 0x01's view, unzoomed
        memcpy(v, (const void *)(uintptr_t)(set2 + 0x284), sizeof(v));
        CHECK(v[0] == 0.0f && v[2] == 2520.0f && v[1] == 0.0f && v[3] == 1080.0f);
        // The zoomed backdrop's set as before: the whole width, rows 59..1021 (its y +-0.89 before the cover).
        memcpy(projection, (const void *)(uintptr_t)(set0 + 0x100), sizeof(projection));
        CHECK(near(projection[0] * 532.875f, 1.3125f) && near(projection[5], 1.3125f * 1080.0f / 962.0f));
        memcpy(v, (const void *)(uintptr_t)(set0 + 0x284), sizeof(v));
        CHECK(v[0] == 0.0f && v[2] == 2520.0f && v[1] == 59.0f && v[3] == 1021.0f);
        set_viewport(passes, 0x01, v);
        CHECK(v[0] == 300.0f && v[2] == 2220.0f && v[1] == 0.0f && v[3] == 1080.0f);
    }

    // Their dialogue: the backing and its border (wider than the 16:9 already) reach the frame edges; the centre
    // ornament, the matrix (no stretch) and the text part stay.
    static uint8_t widget[0x300];
    static FakeModel backing;
    const uint32_t narration = 0x26;
    memcpy(widget + 0xe0, &narration, 4);
    memset(&backing, 0, sizeof(backing));
    hexagon(backing.positions[0], -708.75f, 708.75f, 255.0f, 360.0f); // the gradient
    hexagon(backing.positions[1], -708.75f, 708.75f, 255.0f, 262.0f); // its border line
    hexagon(backing.positions[2], -210.0f, 210.0f, 245.0f, 262.0f);   // the ornament on it
    float backing_before[FAKE_POLYGONS][12];
    memcpy(backing_before, backing.positions, sizeof(backing_before));
    part_call(&call, base, base + record->widget_draw_call + 5, 0x28);
    put_bounds(&call, -708.75f, 245.0f, 708.75f, 360.0f);
    attach_model(&call, &backing);
    run_part(&call, (uint64_t)(uintptr_t)widget + 0x100);
    CHECK(command_float(&call, 0x10) == 1.0f && command_float(&call, 0x24) == 1.0f);
    CHECK(near(backing.positions[0][0], -708.75f * 1.3125f) && near(backing.positions[0][2], 708.75f * 1.3125f));
    CHECK(near(backing.positions[1][0], -708.75f * 1.3125f) && near(backing.positions[1][2], 708.75f * 1.3125f));
    CHECK(!memcmp(backing.positions[2], backing_before[2], sizeof(backing_before[2])));
    for (int i = 0; i < FAKE_POLYGONS; ++i) CHECK(same_y(backing.positions[i], backing_before[i]));
    part_call(&call, base, base + record->widget_draw_call + 5, 0x28);
    put_bounds(&call, -470.0f, 270.0f, 470.0f, 330.0f); // a line of text
    run_part(&call, (uint64_t)(uintptr_t)widget + 0x100);
    CHECK(command_float(&call, 0x10) == 1.0f && command_float(&call, 0x30) == 0.0f);

    neutral_frame(camera, passes); // the inn follows Morgan directly in this test; a real visit passes through gameplay
    // Two layers (the inn): a full picture with figures in pass 0x01, hidden under the room in pass 0x03; pass 0x02
    // already holds a part in bucket 6. The room is drawn last in pass 0x02's set, after that part.
    static uint8_t buckets[7][0x20];
    static uint64_t queued[1];
    const uint64_t queued_begin = (uint64_t)(uintptr_t)queued, queued_end = queued_begin + sizeof(queued);
    memcpy(buckets[6] + 0x8, &queued_begin, 8);
    memcpy(buckets[6] + 0x10, &queued_end, 8);
    const uint64_t set3 = current_set(passes, 3);
    const uint64_t buckets_begin = (uint64_t)(uintptr_t)buckets, buckets_end = buckets_begin + sizeof(buckets);
    memcpy((void *)(uintptr_t)(set2 + 0x2b0), &buckets_begin, 8);
    memcpy((void *)(uintptr_t)(set2 + 0x2b8), &buckets_end, 8);
    const float full[4] = {-406.5f, -1.0f, 406.5f, 1.0f}, table[4] = {100.0f, -1.0f, 406.5f, -0.3f};
    // Its picture holds its own bars among the tiles: not a matte, so nothing is extended or follows it.
    static FakeModel picture;
    memset(&picture, 0, sizeof(picture));
    hexagon(picture.positions[0], -406.5f, 406.5f, 1.0f, 0.93f);
    hexagon(picture.positions[1], -406.5f, 406.5f, -0.93f, -1.0f);
    hexagon(picture.positions[2], -406.5f, 0.0f, 1.0f, -1.0f);
    hexagon(picture.positions[3], 0.0f, 406.5f, 1.0f, -1.0f);
    float picture_before[FAKE_POLYGONS][12];
    memcpy(picture_before, picture.positions, sizeof(picture_before));
    GuestRegs regs = queue_model_part(&call, base, passes, 0x01, 1, full, 0x7b00, &picture);
    CHECK(regs.rdi == set0 && regs.rdx == 1);
    CHECK(!memcmp(picture.positions, picture_before, sizeof(picture_before)));
    regs = queue_part(&call, base, passes, 0x01, 1, npc);
    CHECK(regs.rdi == set1);
    regs = queue_part(&call, base, passes, 0x01, 1, title);
    CHECK(regs.rdi == set1);
    regs = queue_part(&call, base, passes, 0x03, 1, full);
    CHECK(regs.rdi == set2 && regs.rdx == 7);
    regs = queue_part(&call, base, passes, 0x03, 1, table);
    CHECK(regs.rdi == set3 && regs.rdx == 1);
    float shifted[16];
    memcpy(shifted, passes + 3 * PASS_BYTES + 0x1c0, sizeof(shifted));
    shifted[12] = 0.1f; // pass 0x03's own view, so its set can be told apart
    memcpy(passes + 3 * PASS_BYTES + 0x1c0, shifted, sizeof(shifted));
    run_camera(camera, 0.0f, 1280.0f, -640, 640);
    commit_frame(passes);
    memcpy(projection, (const void *)(uintptr_t)(set2 + 0x1c0), sizeof(projection));
    CHECK(near(projection[0] * 532.875f, 1.3125f) && near(projection[12], 0.1f * 1.3125f));
    memcpy(v, (const void *)(uintptr_t)(set2 + 0x284), sizeof(v));
    CHECK(v[0] == 0.0f && v[2] == 2520.0f && v[1] == 0.0f && v[3] == 1080.0f); // the room reaches the frame's rows
    memcpy(projection, (const void *)(uintptr_t)(set0 + 0x100), sizeof(projection));
    CHECK(near(projection[0] * 532.875f, 1.3125f) && near(projection[5], 1.3125f));
    memcpy(v, (const void *)(uintptr_t)(set0 + 0x284), sizeof(v));
    CHECK(v[1] == 0.0f && v[3] == 1080.0f); // full-height backdrops: no black matte rows
    set_viewport(passes, 0x01, v);
    CHECK(v[0] == 300.0f && v[2] == 2220.0f);
    set_viewport(passes, 0x03, v);
    CHECK(v[0] == 300.0f && v[2] == 2220.0f); // the table keeps the 16:9
    shifted[12] = 0.0f;
    memcpy(passes + 3 * PASS_BYTES + 0x1c0, shifted, sizeof(shifted));

    // Buckets that do not read as a list: the part stays where the game queued it.
    memcpy((void *)(uintptr_t)(set2 + 0x2b0), &buckets_end, 8);
    memcpy((void *)(uintptr_t)(set2 + 0x2b8), &buckets_begin, 8);
    regs = queue_part(&call, base, passes, 0x03, 1, full);
    CHECK(regs.rdi == set3 && regs.rdx == 1);
    memset((void *)(uintptr_t)(set2 + 0x2b0), 0, 16);
    // A world pass after a screen pass (0x23 after 0x22) never moves.
    regs = queue_part(&call, base, passes, 0x23, 1, full);
    CHECK(regs.rdi == current_set(passes, 0x23));
    run_camera(camera, 0.0f, 1280.0f, -640, 640);
    commit_frame(passes);

    // In gameplay (a wide stage), the same part stays where the game queued it.
    run_camera(camera, 0.0f, 812.0f, -3291, 3324);
    commit_frame(passes);
    regs = queue_part(&call, base, passes, 0x01, 1, room);
    CHECK(regs.rdi == set1);
    free(passes);
    free(memory);
}

/* The real transition state: illustration with no stable fixed backdrop yet (entering) or no longer (leaving). Every
 * world pass stays at the centred 16:9, the same frame the picture changes, with the side mask wanted; no pass is
 * ever cover-relocated while that state holds. */
static void TestFixedScreenTransition(void) {
    const UltrawideRecord *record = &ultrawide_records[0];
    const size_t stub_room = ultrawide_hooks_stub_bytes();
    uint8_t *memory = synthetic_image(record, stub_room);
    uint8_t *passes = fake_passes();
    CHECK(memory != NULL && passes != NULL);
    if (!memory || !passes) return;
    char detail[400];
    ultrawide_hooks_install(memory, record->image.size, record->image.entry, memory + record->image.size, stub_room,
                            1.3125, detail, sizeof(detail));
    const uint64_t base = (uint64_t)(uintptr_t)memory;
    static uint8_t camera[0x100];
    static PartCall call;
    const float knight[4] = {-406.5f, -0.89f, 406.5f, 0.89f};
    float v[4];

    // Gameplay: a wide stage, nothing fixed about the screen, no mask.
    run_camera(camera, 0.0f, 812.0f, -3291, 3324);
    commit_frame(passes);
    CHECK(!ultrawide_fixed_transition_active() && !ultrawide_debug_saulo_mask_wanted());
    unsigned lines = ultrawide_debug_saulo_transition_lines(), backdrops = ultrawide_debug_saulo_backdrop_lines();

    // Entering the guild with no backdrop part queued yet: the fallback, on this very frame, both world passes
    // tried (0x01, where the backdrop belongs, and 0x05, an unrelated one) at the same centred 16:9, masked.
    run_camera(camera, 0.0f, 1280.0f, -640, 640);
    commit_frame(passes);
    CHECK(ultrawide_fixed_transition_active() && ultrawide_debug_saulo_mask_wanted());
    CHECK(ultrawide_debug_saulo_transition_lines() == lines + 1);
    CHECK(ultrawide_debug_saulo_backdrop_lines() == backdrops); // no cover relocation while held
    set_viewport(passes, 0x01, v);
    CHECK(v[0] == 300.0f && v[2] == 2220.0f);
    set_viewport(passes, 0x05, v);
    CHECK(v[0] == 300.0f && v[2] == 2220.0f); // one mapping for every world pass
    lines = ultrawide_debug_saulo_transition_lines();

    // The backdrop appears: stable, cover relocation resumes, the mask lifts.
    queue_model_part(&call, base, passes, 0x01, 1, knight, 0x8a00, NULL);
    run_camera(camera, 0.0f, 1280.0f, -640, 640);
    commit_frame(passes);
    CHECK(!ultrawide_fixed_transition_active() && !ultrawide_debug_saulo_mask_wanted());
    CHECK(ultrawide_debug_saulo_transition_lines() == lines + 1);
    CHECK(ultrawide_debug_saulo_backdrop_lines() == backdrops + 1);
    lines = ultrawide_debug_saulo_transition_lines();

    // Leaving: the backdrop stops being queued while still illustration: the fallback again, same frame.
    run_camera(camera, 0.0f, 1280.0f, -640, 640);
    commit_frame(passes);
    CHECK(ultrawide_fixed_transition_active() && ultrawide_debug_saulo_mask_wanted());
    CHECK(ultrawide_debug_saulo_transition_lines() == lines + 1);
    lines = ultrawide_debug_saulo_transition_lines();

    // Gameplay resumes.
    run_camera(camera, 0.0f, 812.0f, -3291, 3324);
    commit_frame(passes);
    CHECK(!ultrawide_fixed_transition_active() && !ultrawide_debug_saulo_mask_wanted());
    CHECK(ultrawide_debug_saulo_transition_lines() == lines + 1);
    free(passes);
    free(memory);
}

/* The loading or saving fade wants the same final side mask, independently of any fixed-screen transition. */
static void TestFadeSideMask(void) {
    const UltrawideRecord *record = &ultrawide_records[0];
    const size_t stub_room = ultrawide_hooks_stub_bytes();
    uint8_t *memory = synthetic_image(record, stub_room);
    uint8_t *passes = fake_passes();
    CHECK(memory != NULL && passes != NULL);
    if (!memory || !passes) return;
    char detail[400];
    ultrawide_hooks_install(memory, record->image.size, record->image.entry, memory + record->image.size, stub_room,
                            1.3125, detail, sizeof(detail));
    static uint8_t camera[0x100], fade[0x200];
    const uint64_t vtable = 0xb8e1e8;
    memcpy(fade, &vtable, 8);
    const uint64_t fade_model = (uint64_t)(uintptr_t)fade + 0xb0;
    GuestRegs regs;

    run_camera(camera, 0.0f, 812.0f, -3291, 3324); // plain gameplay: no fixed-screen transition either
    commit_frame(passes);
    CHECK(!ultrawide_debug_saulo_mask_wanted());

    memset(&regs, 0, sizeof(regs));
    regs.rdi = fade_model;
    ultrawide_fade_hook(&regs);
    run_camera(camera, 0.0f, 812.0f, -3291, 3324);
    commit_frame(passes);
    CHECK(!ultrawide_fixed_transition_active() && ultrawide_debug_saulo_mask_wanted());

    run_camera(camera, 0.0f, 812.0f, -3291, 3324); // the fade stops drawing: the mask follows, next frame
    commit_frame(passes);
    CHECK(!ultrawide_debug_saulo_mask_wanted());
    free(passes);
    free(memory);
}

static void TestMatteBand(void) {
    const float half21 = 1.0f / 1.3125f;
    float u[6], y[6];
    // The black bands across the 16:9 at the frame's top and bottom, at 21:9 and 32:9.
    band(u, y, -half21, half21, 0.89f, 1.0f);
    CHECK(ultrawide_is_matte_band(u, y, half21));
    band(u, y, -half21, half21, -1.0f, -0.89f);
    CHECK(ultrawide_is_matte_band(u, y, half21));
    band(u, y, -0.5f, 0.5f, 0.89f, 1.0f);
    CHECK(ultrawide_is_matte_band(u, y, 0.5f));
    // Already extended to the frame edges, still the matte.
    band(u, y, -1.0f, 1.0f, 0.89f, 1.0f);
    CHECK(ultrawide_is_matte_band(u, y, half21));
    // Not a matte: a backdrop tile, a band clear of the frame's top and bottom (a title line), one not across the 16:9.
    band(u, y, -half21, half21, -0.89f, 0.89f);
    CHECK(!ultrawide_is_matte_band(u, y, half21));
    band(u, y, -half21, half21, 0.885f, 0.895f);
    CHECK(!ultrawide_is_matte_band(u, y, half21));
    band(u, y, -0.25f, half21, 0.89f, 1.0f);
    CHECK(!ultrawide_is_matte_band(u, y, half21));
}

/* The guild knight's and Morgan's black bars at 32:9 (3840 x 1080) reach the whole width from pass 0x02's set, which
 * draws pass 0x01's view; at 16:9 nothing extends. */
static void TestMatteWidths(void) {
    const UltrawideRecord *record = &ultrawide_records[0];
    const size_t stub_room = ultrawide_hooks_stub_bytes();
    uint8_t *memory = synthetic_image(record, stub_room);
    CHECK(memory != NULL);
    if (!memory) return;
    const uint64_t base = (uint64_t)(uintptr_t)memory;
    static uint8_t camera[0x100];
    static PartCall call;
    static FakeMatte mattes;
    const float npc[4] = {-150.0f, -1.0f, 300.0f, 1.0f}, bars[4] = {-406.5f, -1.0f, 406.5f, 1.0f};
    const float backdrops[2][4] = {{-406.5f, -0.89f, 406.5f, 0.89f}, {-406.5f, -0.90f, 406.5f, 0.89f}};
    const float widths[] = {3840.0f, 1920.0f}, scales[] = {2.0f, 1.0f};
    for (size_t s = 0; s < sizeof(scales) / sizeof(scales[0]); ++s) {
        uint8_t *passes = fake_passes_at(widths[s], scales[s]);
        CHECK(passes != NULL);
        if (!passes) break;
        char detail[400];
        ultrawide_hooks_install(memory, record->image.size, record->image.entry, memory + record->image.size,
                                stub_room, scales[s], detail, sizeof(detail));
        for (int screen = 0; screen < 2; ++screen) {
            const uint64_t backdrop_model = 0x7f00 + (uint64_t)screen * 2;
            fake_matte(&mattes, backdrops[screen][3]);
            float before[FAKE_POLYGONS][12];
            uint8_t colours[sizeof(mattes.colours)];
            memcpy(before, mattes.model.positions, sizeof(before));
            memcpy(colours, mattes.colours, sizeof(colours));
            run_camera(camera, 0.0f, 1280.0f, -640, 640);
            commit_frame(passes);
            const GuestRegs backdrop = queue_model_part(&call, base, passes, 0x01, 1, backdrops[screen],
                                                        backdrop_model, NULL);
            const GuestRegs character = queue_model_part(&call, base, passes, 0x01, 1, npc, backdrop_model + 1, NULL);
            const GuestRegs regs = queue_model_part(&call, base, passes, 0x01, 1, bars, backdrop_model, &mattes.model);
            for (int i = 0; i < FAKE_POLYGONS; ++i) CHECK(same_y(mattes.model.positions[i], before[i]));
            CHECK(!memcmp(mattes.colours, colours, sizeof(colours)));
            if (scales[s] == 1.0f) { // the loader never installs at 16:9; even so, no x changes
                CHECK(!memcmp(mattes.model.positions, before, sizeof(before)));
                continue;
            }
            CHECK(backdrop.rdi == current_set(passes, 0) && character.rdi == current_set(passes, 1));
            CHECK(regs.rdi == current_set(passes, 2) && regs.rdx == 1);
            for (int i = 0; i < FAKE_POLYGONS; ++i) CHECK(spans_frame(mattes.model.positions[i], 406.5f * 2.0f));
            run_camera(camera, 0.0f, 1280.0f, -640, 640);
            commit_frame(passes);
            float v[4], projection[16];
            memcpy(v, (const void *)(uintptr_t)(current_set(passes, 2) + 0x284), sizeof(v));
            CHECK(v[0] == 0.0f && v[2] == 3840.0f && v[1] == 0.0f && v[3] == 1080.0f);
            memcpy(projection, (const void *)(uintptr_t)(current_set(passes, 2) + 0x1c0), sizeof(projection));
            CHECK(near(projection[0] * 812.0f, 1.0f) && projection[5] == 1.0f); // pass 0x01's view, unzoomed
            memcpy(v, (const void *)(uintptr_t)(current_set(passes, 0) + 0x284), sizeof(v));
            CHECK(v[0] == 0.0f && v[2] == 3840.0f && v[1] == 59.0f && v[3] == 1021.0f);
            memcpy(projection, (const void *)(uintptr_t)(current_set(passes, 0) + 0x100), sizeof(projection));
            CHECK(near(projection[0] * 812.0f, 2.0f));
            set_viewport(passes, 0x01, v);
            CHECK(v[0] == 960.0f && v[2] == 2880.0f);
        }
        free(passes);
    }
    free(memory);
}

int main(void) {
    TestScreenPasses();
    TestWorldPasses();
    TestFillRule();
    TestFillHook();
    TestWorldShare();
    TestLimitsSpan();
    TestFrameSpan();
    TestClipPass();
    TestWidgetPolicy();
    TestMapZoom();
    TestExtendBand();
    TestRecords();
    TestInstall();
    TestPartHook();
    TestFacilityFrame();
    TestCommitHook();
    TestWorldPassSpan();
    TestCoverHelpers();
    TestBackdrop();
    TestFixedScreenTransition();
    TestFadeSideMask();
    TestMatteBand();
    TestMatteWidths();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
