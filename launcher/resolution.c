#include "dcp_resolution.h"

#include <stdio.h>
#include <string.h>

const DcpResolution dcp_resolutions[DCP_RESOLUTION_COUNT] = {
    {"native", "Native (1080p)", 1920, 1080, 0}, {"480p", "480p", 854, 480, 0},
    {"720p", "720p", 1280, 720, 0},              {"1440p", "1440p", 2560, 1440, 1},
    {"1800p", "1800p", 3200, 1800, 1},           {"4K", "4K", 3840, 2160, 1},
};

int dcp_resolution_index(const char *key) {
    for (int i = 0; key && i < DCP_RESOLUTION_COUNT; ++i)
        if (!strcmp(key, dcp_resolutions[i].key)) return i;
    return 0;
}

const DcpAspectRatio dcp_aspect_ratios[DCP_ASPECT_COUNT] = {
    {"16:9", "16:9 Original", 16, 9}, {"16:10", "16:10", 16, 10},       {"21:9", "21:9 Ultrawide", 21, 9},
    {"32:9", "32:9 Super Ultrawide", 32, 9}, {"4:3", "4:3", 4, 3},      {"5:4", "5:4", 5, 4},
};

int dcp_aspect_index(const char *key) {
    for (int i = 0; key && i < DCP_ASPECT_COUNT; ++i)
        if (!strcmp(key, dcp_aspect_ratios[i].key)) return i;
    return 0;
}

static int aligned_width(int height, const DcpAspectRatio *aspect) {
    const long width = ((long)height * aspect->width * 2 + aspect->height) / (2 * aspect->height);
    return (int)((width + 4) / 8 * 8);
}

DcpResolution dcp_render_target(const DcpResolution *resolution, const DcpAspectRatio *aspect) {
    DcpResolution target = *resolution;
    if (aspect == &dcp_aspect_ratios[0]) return target;
    const long budget = 3840L * 2160;
    int height = resolution->height;
    while (height > 8 && (long)aligned_width(height, aspect) * height > budget) height -= 8;
    target.width = aligned_width(height, aspect);
    target.height = height;
    if (target.width > 1920 || height > 1080 || (long)target.width * height > 1920L * 1080) target.pro_memory = 1;
    return target;
}

static int fail(char *error, size_t size, const char *what, uint32_t offset) {
    snprintf(error, size, "render resolution patch: %s at image offset 0x%x", what, (unsigned)offset);
    return 0;
}

int dcp_apply_resolution(unsigned char *image, uint64_t image_size, const DcpResolutionSite *sites, size_t count,
                         const DcpResolution *resolution, char *error, size_t error_size) {
    for (size_t i = 0; i < count; ++i) {
        const DcpResolutionSite *site = &sites[i];
        const uint64_t end = (uint64_t)site->offset + site->opcode_length + 4;
        if (end > image_size) return fail(error, error_size, "site outside the image", site->offset);
        const unsigned char *at = image + site->offset;
        uint32_t current;
        memcpy(&current, at + site->opcode_length, 4);
        if (memcmp(at, site->opcode, site->opcode_length) || current != site->value)
            return fail(error, error_size, "unexpected instruction", site->offset);
    }
    for (size_t i = 0; i < count; ++i) {
        const uint32_t value = (uint32_t)(sites[i].height ? resolution->height : resolution->width);
        memcpy(image + sites[i].offset + sites[i].opcode_length, &value, 4);
    }
    return 1;
}
