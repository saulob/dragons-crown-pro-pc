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

int dcp_true_ultrawide_applies(const DcpAspectRatio *aspect) {
    return aspect->width * 9 > aspect->height * 16;
}

double dcp_true_ultrawide_scale(const DcpAspectRatio *aspect, const DcpResolution *target) {
    if (!dcp_true_ultrawide_applies(aspect)) return 1.0;
    return (double)target->width * 9 / ((double)target->height * 16);
}

const DcpOrthoSite dcp_pass_ortho_sites[DCP_PASS_ORTHO_SITE_COUNT] = {
    {0x10686, -640.0f}, {0x106bb, -640.0f}, {0x10699, 640.0f},
    {0x106d5, 640.0f},  {0x106f7, -406.0f}, {0x10703, 406.0f},
};

static const unsigned char ortho_opcode[4] = {0xc5, 0xfa, 0x10, 0x05};

static int ultrawide_fail(char *error, size_t size, const char *what, uint64_t offset) {
    snprintf(error, size, "true ultrawide patch: %s at image offset 0x%llx", what, (unsigned long long)offset);
    return 0;
}

static int check_ortho_site(const unsigned char *image, uint64_t image_size, const DcpOrthoSite *site, char *error,
                            size_t error_size) {
    if ((uint64_t)site->offset + 8 > image_size)
        return ultrawide_fail(error, error_size, "site outside the image", site->offset);
    int32_t displacement;
    memcpy(&displacement, image + site->offset + 4, 4);
    const int64_t constant = (int64_t)site->offset + 8 + displacement;
    if (memcmp(image + site->offset, ortho_opcode, sizeof(ortho_opcode)))
        return ultrawide_fail(error, error_size, "unexpected instruction", site->offset);
    if (constant < 0 || (uint64_t)constant + 4 > image_size)
        return ultrawide_fail(error, error_size, "constant outside the image", site->offset);
    float value;
    memcpy(&value, image + constant, 4);
    if (value != site->value) return ultrawide_fail(error, error_size, "unexpected constant", (uint64_t)constant);
    return 1;
}

int dcp_apply_true_ultrawide(unsigned char *image, uint64_t image_size, uint64_t slot_offset, const DcpOrthoSite *sites,
                             size_t count, double scale, char *error, size_t error_size) {
    if (slot_offset < image_size) return ultrawide_fail(error, error_size, "slots inside the image", slot_offset);
    for (size_t i = 0; i < count; ++i) {
        if (!check_ortho_site(image, image_size, &sites[i], error, error_size)) return 0;
        const int64_t reach = (int64_t)(slot_offset + 4 * i) - ((int64_t)sites[i].offset + 8);
        if (reach < INT32_MIN || reach > INT32_MAX)
            return ultrawide_fail(error, error_size, "slots out of reach", sites[i].offset);
    }
    for (size_t i = 0; i < count; ++i) {
        const float value = (float)(sites[i].value * scale);
        const int32_t displacement = (int32_t)((int64_t)(slot_offset + 4 * i) - ((int64_t)sites[i].offset + 8));
        memcpy(image + slot_offset + 4 * i, &value, 4);
        memcpy(image + sites[i].offset + 4, &displacement, 4);
    }
    return 1;
}
