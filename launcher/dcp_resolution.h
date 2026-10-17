/* The render resolutions the launcher offers and the game code that selects them. */
#ifndef DCP_RESOLUTION_H
#define DCP_RESOLUTION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;  /* render_resolution in dcp.ini */
    const char *name; /* launcher label */
    int width, height;
    int pro_memory;   /* the game's PS4 Pro path: its larger graphics memory, sized for 3840 x 2160 */
} DcpResolution;

#define DCP_RESOLUTION_COUNT 6
extern const DcpResolution dcp_resolutions[DCP_RESOLUTION_COUNT];

/* Index of the key, 0 (native) when it is not listed. */
int dcp_resolution_index(const char *key);

typedef struct {
    const char *key;  /* aspect_ratio in dcp.ini */
    const char *name; /* launcher label */
    int width, height; /* the ratio's terms */
} DcpAspectRatio;

#define DCP_ASPECT_COUNT 6
extern const DcpAspectRatio dcp_aspect_ratios[DCP_ASPECT_COUNT];

/* Index of the key, 0 (16:9 Original) when it is not listed. */
int dcp_aspect_index(const char *key);

/* The render target of a resolution in an aspect ratio: the resolution's height, the width that ratio gives it aligned
 * to the 8-pixel tile, and the PS4 Pro memory path when the target is bigger than 1080p. 16:9 returns the resolution
 * unchanged. A target above the 4K pixel count keeps its ratio and loses height until it fits. */
DcpResolution dcp_render_target(const DcpResolution *resolution, const DcpAspectRatio *aspect);

/* One `mov reg, imm32` in the game that holds a render width or height. */
typedef struct {
    uint32_t offset; /* image offset of the opcode */
    uint8_t opcode_length;
    uint8_t opcode[2];
    uint32_t value;  /* the original immediate */
    int height;
} DcpResolutionSite;

/* Writes the resolution's width and height into every site after checking each one still holds the original
 * instruction. Returns 0 and a message in `error` when the image does not match; nothing is written then. */
int dcp_apply_resolution(unsigned char *image, uint64_t image_size, const DcpResolutionSite *sites, size_t count,
                         const DcpResolution *resolution, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
