#pragma once
// Apple MetalFX's temporal upscaler (macOS 13+) under the Vulkan device, which runs on MoltenVK
// there: VK_EXT_metal_objects hands over the Metal device, the queue MoltenVK submits to and each
// image's MTLTexture. The device submits the frame's work up to the upscale, this encodes the
// scaler in a Metal command buffer on that same queue - after it, as MoltenVK submits in order
// (MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS) - and the frame goes on in a second command buffer.
// Elsewhere (no Apple build) it is never available.

#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <vector>

namespace cr {

struct MetalFxInputs {
    VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE, motion = VK_NULL_HANDLE, output = VK_NULL_HANDLE;
    int renderW = 0, renderH = 0, outW = 0, outH = 0;
    float jitterX = 0, jitterY = 0;          // pixels (as DLSS takes them)
    float motionScaleX = 1, motionScaleY = 1; // the motion vectors to render pixels
    bool reset = false;
};

class MetalFx {
public:
    MetalFx();
    ~MetalFx();
    // The device extension it needs, when the GPU (MoltenVK) has it
    static void deviceExtensions(VkPhysicalDevice gpu, std::vector<const char*>& out);
    // What an exported image's VkImageCreateInfo chains (null where it can't run)
    static const void* imageExportInfo();
    bool init(VkInstance instance, VkDevice device, VkQueue queue);
    void shutdown();
    std::string problem() const; // "" when it can run
    // The scaler on the queue, after everything submitted so far (false: it couldn't)
    bool encode(const MetalFxInputs& in);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cr
