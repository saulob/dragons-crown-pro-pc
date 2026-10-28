/* Interface between the loader/runtime and the video core (shadPS4-derived, gpu/shim/dcpgpu.cpp). */
#ifndef DCPGPU_H
#define DCPGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* user folder: the pipeline cache lives in its cache folder */
    const char *settings_file;  /* dcp.ini */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} DcpGpuConfig;
/* 1 when a video core is linked in, 0 for the stub. */
int dcpgpu_available(void);
/* Registers kernel event queues (needed with or without graphics). */
void dcpgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor; 0 on success. */
int dcpgpu_init(const DcpGpuConfig *config);
/* Function for an imported NID ("NID#lib"), or 0 when the video core does not provide it. */
uintptr_t dcpgpu_resolve(const char *scoped_nid);
/* Called first by the loader's fault handler: 1 when a GPU page-tracking fault was handled. */
int dcpgpu_handle_fault(void *context, void *address);
void dcpgpu_dump_guest_writes(void *context);
/* Number of symbols registered by the video core (diagnostics). */
unsigned dcpgpu_symbol_count(void);
/* Keyboard text entry and list selection drawn by the video core; begin returns 0 without a window.
 * text poll: 0 typing, 1 confirmed, 2 cancelled. choice poll: -1 open, -2 cancelled, else the index. */
int dcpgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int dcpgpu_text_input_poll(char *out_utf8, uint64_t size);
int dcpgpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int dcpgpu_choice_poll(void);
/* The choice list reads PS4 pad button bits from this callback every frame while it is open. */
void dcpgpu_choice_set_input(uint32_t (*buttons)(void));
/* 1 while the settings menu is open: the game's pad input should be held neutral. */
int dcpgpu_overlay_captures_input(void);
/* DCP_INPUT_LATENCY_TRACE=1: 1 when set; the SDL_GetTicksNS time of the newest host button or key press. */
int dcpgpu_latency_trace_enabled(void);
uint64_t dcpgpu_latency_press_ns(void);

/* DEBUG_SAULO: the F9 ultrawide frame capture (gpu/shadps4/video_core/renderer_vulkan/vk_ultrawide_capture.cpp) and
 * what the guest hooks (src/ultrawide_capture.c) tell it. Image offsets are 0 outside the image. */
typedef struct {
    uint64_t command;
    uint64_t bucket_set;
    uint32_t priority;
    uint32_t callers[4];     /* return addresses as image offsets, the queueing function first */
    int32_t quad;            /* 1: a 2D primitive from the quad submit, with the fields below */
    int32_t quad_kind, quad_vertices, quad_matrix;
    float quad_left, quad_top, quad_right, quad_bottom;
    uint64_t quad_texture;
} DcpCaptureQueued;
typedef struct {
    uint64_t command_buffer; /* the guest draw command buffer's write pointer as the command starts */
    uint64_t command;
    uint64_t bucket_set;
    uint32_t vtable;         /* image offset */
    uint32_t priority;
    int32_t layer;           /* pass id + 0x2b * view; -1 unknown */
    int32_t set_index;
    float projection[4];     /* m00, m11, m30, m31 of the pass projection */
    float view_scale;
    float viewport[4];
} DcpCaptureExecuted;
void dcpgpu_debug_saulo_capture_configure(const char *logs_dir, const char *ultrawide_line, int guest_hooks);
int dcpgpu_debug_saulo_capture_wanted(void);
void dcpgpu_debug_saulo_capture_queued(const DcpCaptureQueued *queued);
void dcpgpu_debug_saulo_capture_executed(const DcpCaptureExecuted *executed);
/* 1 while the final presenter should black out the ultrawide side extensions for the frame being built: the real
 * loading/saving fade is drawn, or the fixed-screen transition fallback is active. Set once per guest frame. */
void dcpgpu_debug_saulo_ultrawide_mask(int wanted);
#ifdef __cplusplus
}
#endif
#endif
