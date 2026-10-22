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
    const char *key;
    const char *name;
    int width, height;
} DcpAspectRatio;

#define DCP_ASPECT_COUNT 6
extern const DcpAspectRatio dcp_aspect_ratios[DCP_ASPECT_COUNT];

/* Index of the key, 0 (16:9 Original) when it is not listed. */
int dcp_aspect_index(const char *key);

/* The width is aligned to the 8-pixel tile; a target above the 4K pixel count keeps its ratio and loses height. */
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

#define DCP_SIGNATURE_MAX 64

/* A render size site found by its instruction context: `pattern` is the window of `length` bytes that holds the site at
 * `lead`; a byte with compare 0 is a relocated displacement and is not compared. */
typedef struct {
    uint8_t length;
    uint8_t lead;
    uint8_t opcode_length;
    uint8_t opcode[2];
    uint32_t value;
    int height;
    uint8_t pattern[DCP_SIGNATURE_MAX];
    uint8_t compare[DCP_SIGNATURE_MAX];
} DcpResolutionSignature;

typedef struct {
    int matches;  /* 2 means two or more */
    uint32_t offset;
    int valid;    /* one match, and the site's bytes hold */
} DcpResolutionSignatureResult;

#define DCP_CUSA10209_102_SIGNATURE_COUNT 23
extern const DcpResolutionSignature dcp_cusa10209_102_signatures[DCP_CUSA10209_102_SIGNATURE_COUNT];

/* Finds every signature's site and checks its bytes. Returns 1 and fills `sites` only when all of them are found once,
 * valid and not overlapping; otherwise 0 and `sites` is left alone. Reads the image only. */
int dcp_find_resolution_sites(const unsigned char *image, uint64_t image_size, const DcpResolutionSignature *signatures,
                              size_t count, DcpResolutionSite *sites, DcpResolutionSignatureResult *results);

/* DEBUG_SAULO: logs the CUSA10209 1.02 signature search for `label`; writes nothing to the image. */
void dcp_probe_render_sites(const unsigned char *image, uint64_t image_size, const char *label);

#ifdef __cplusplus
}
#endif

#endif
