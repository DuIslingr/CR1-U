#pragma once
// NVIDIA DLSS Super Resolution (and DLAA: DLSS at an upscale ratio of 1) through the NGX
// Vulkan API. Needs the DLSS SDK at build time (third_party/dlss, CR_HAVE_DLSS) and, to run, an
// NVIDIA RTX GPU with its driver and the DLSS runtime (libnvidia-ngx-dlss.so) next to the game.
// Anywhere else problem() says why and nothing else changes.

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace cr {

struct DlssImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

struct DlssInputs {
    // Render size, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: colour, depth, motion vectors
    // (UV offsets to last frame's position); the output: upscale size, storage, GENERAL
    DlssImage color, depth, motion, output;
    int renderW = 0, renderH = 0, outW = 0, outH = 0;
    float jitterX = 0, jitterY = 0; // pixels at render size
    float deltaMs = 16.7f;
    float sharpness = 0;
    bool reset = false;
};

class Dlss {
public:
    // Before the instance / device exist: the extensions (and features) NGX needs, added only
    // when this GPU has them all
    static void instanceExtensions(std::vector<const char*>& ext);
    void deviceRequirements(VkInstance instance, VkPhysicalDevice gpu, std::vector<const char*>& ext,
                            VkPhysicalDeviceVulkan12Features& features12);
    bool init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device);
    void shutdown();
    // "" when DLSS can run, else why not
    std::string problem() const { return problem_; }
    // Records the upscale (creating DLSS for these sizes / this ratio first when needed)
    bool evaluate(VkCommandBuffer cmd, const DlssInputs& in);

private:
    VkDevice device_ = VK_NULL_HANDLE;
    bool initialised_ = false, extensionsOk_ = false;
    std::string problem_ = "built without the DLSS SDK";
    void* params_ = nullptr;  // NVSDK_NGX_Parameter*
    void* feature_ = nullptr; // NVSDK_NGX_Handle*
    int featureW_ = 0, featureH_ = 0, featureOutW_ = 0, featureOutH_ = 0, featureQuality_ = -1;
};

} // namespace cr
