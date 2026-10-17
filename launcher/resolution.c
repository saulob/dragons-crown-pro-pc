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
