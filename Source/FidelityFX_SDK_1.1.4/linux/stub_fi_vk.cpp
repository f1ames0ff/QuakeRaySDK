#include <FidelityFX/host/backends/vk/ffx_vk.h>

extern "C" FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *config)
{
    (void)config;
    return FFX_ERROR_INVALID_ARGUMENT;
}
