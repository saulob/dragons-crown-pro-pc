/* The graphics cards Vulkan offers the renderer, asked once by the launcher. */
#ifndef DCP_GPU_PROBE_H
#define DCP_GPU_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

#define DCP_GPU_PROBE_MAX 16

typedef struct {
    int status;            /* DcpVulkanProbe::Status (gpu/shim/dcp_vulkan_probe.h) */
    unsigned api_version;  /* the instance's Vulkan version when it was asked, else 0 */
    int code;              /* the VkResult of the step that failed, 0 otherwise */
    int count;             /* usable cards in names */
    char names[DCP_GPU_PROBE_MAX][256];
} DcpGpuProbe;

/* Never asserts and never ends the process for what Vulkan lacks: a missing loader, an old runtime, a failing driver or no
 * usable card all come back in `status` with no names. Enumerates only; no device is created and the renderer's own Vulkan
 * state is left as it was. */
void dcp_gpu_probe(DcpGpuProbe *out);

#ifdef __cplusplus
}
#endif

#endif
