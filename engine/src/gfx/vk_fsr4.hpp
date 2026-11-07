#pragma once
// AMD FSR 4 (FSR Upscaling 4.1.2 - AMD's FSR SDK 2.4) through AMD's FSR API: AMD's Vulkan DLLs
// (amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll from the SDK, and the Windows
// driver's amdvkffx64.dll, which holds FSR 4 itself), put by the player in fsr4/ in the game
// folder - never shipped: AMD's licence allows them in binary form under its own terms. Windows
// loads them as they are; Linux through ffx-vk-linux (libffx_vk_linux.so beside them - MIT,
// github.com/DuIslingr/ffx-vk-linux), which maps them natively. Without the files, on other
// systems or GPUs, or where AMD's SDK offers only FSR 3.1 (no driver DLL), problem() says why and
// FSR 3.1 from source stays the game's.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cr {

struct Fsr4Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
};

struct Fsr4Inputs {
    // Render size, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: the jittered colour, depth (0 near
    // .. 1 far), motion vectors (RG16F, UV offset to last frame's position), the reactive mask
    // (R8, optional); the output: upscale size, a storage image in VK_IMAGE_LAYOUT_GENERAL
    Fsr4Image color, depth, motion, reactive, output;
    int renderW = 0, renderH = 0, outW = 0, outH = 0;
    float jitterX = 0, jitterY = 0; // pixels at render size (Fsr3::jitter's)
    float deltaMs = 16.7f;
    float cameraNear = 10, cameraFar = 4000, fovY = 1.0f;
    float sharpness = 0; // 0..1 (0: no sharpening pass)
    bool reset = false;
};

class Fsr4 {
public:
    // Where the files are looked for (main's: fsr4/ in the game folder, beside the executable,
    // CR_FSR4_DIR), and the first of them that holds them ("" none)
    static void setSearchDirs(std::vector<std::string> dirs);
    static std::string found();

    // Before the device exists: when the files are there and the GPU is AMD's, the features
    // FSR 4's shaders use (all this GPU has, as AMD's own probe enables them - a Vulkan 1.1
    // block added to the chain) and the two extensions they take when present
    void deviceRequirements(VkPhysicalDevice gpu, std::vector<const char*>& ext, VkPhysicalDeviceFeatures2& f2,
                            VkPhysicalDeviceVulkan12Features& f12, VkPhysicalDeviceVulkan13Features& f13);
    // (after deviceRequirements: the files are there and the GPU is AMD's - the device is made for it)
    bool wanted() const { return wanted_; }
    // Loads AMD's library, asks it for a context once to learn which FSR it offers here
    bool init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device);
    void shutdown();
    // "" when FSR 4 can run, else why not
    std::string problem() const { return problem_; }
    std::string version() const { return version_; } // "4.1.2" (what AMD's SDK picked)
    // Records the upscale (its context made for these sizes first when needed)
    bool evaluate(VkCommandBuffer cmd, const Fsr4Inputs& in);

private:
    bool createContext(int renderW, int renderH, int outW, int outH);
    void destroyContext();

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    std::string dir_, problem_ = "FSR 4's files aren't in fsr4/ (AMD's SDK DLLs, the driver's amdvkffx64.dll)", version_;
    bool wanted_ = false;
    void* lib_ = nullptr;     // SDL_SharedObject*
    void* fn_ = nullptr;      // the API's five entry points (ffxFunctions*)
    void* context_ = nullptr; // ffxContext
    int ctxRenderW_ = 0, ctxRenderH_ = 0, ctxOutW_ = 0, ctxOutH_ = 0;
    // (the feature blocks chained into the device's creation: they live as long as this)
    VkPhysicalDeviceVulkan11Features f11_{};
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives_{};
    VkPhysicalDeviceCoherentMemoryFeaturesAMD coherent_{};
};

} // namespace cr
