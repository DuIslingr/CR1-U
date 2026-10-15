#pragma once
// AMD FSR 3.1 temporal upscaler on Vulkan: AMD's GPU passes (third_party/fidelityfx, compiled
// at build time) driven by a reimplementation of the SDK's host side (ffx_fsr3upscaler.cpp:
// its resources, constants, clears and dispatch order), without the SDK's backend library.
// Native AA is the same at an upscale ratio of 1.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace cr {

struct Fsr3Inputs {
    // Render size, in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: the jittered colour, depth
    // (0 near .. 1 far), motion vectors (RG16F, UV offset to last frame's position) and the
    // reactive mask (R8)
    VkImageView color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE, motion = VK_NULL_HANDLE, reactive = VK_NULL_HANDLE;
    VkImageView output = VK_NULL_HANDLE; // upscale size, storage image in VK_IMAGE_LAYOUT_GENERAL
    int renderW = 0, renderH = 0, outW = 0, outH = 0;
    float jitterX = 0, jitterY = 0; // pixels, as Fsr3::jitter gave them
    float deltaMs = 16.7f;
    float cameraNear = 10, cameraFar = 4000, fovY = 1.0f;
    float sharpness = 0; // 0..1, 0 = no sharpening pass
    bool reset = false;
};

class Fsr3 {
public:
    // Whether this GPU can run it (the formats, features and extension it needs)
    static bool supported(VkPhysicalDevice gpu);
    // The device extension to enable when it is supported (+ the features shaderStorageImage-
    // WriteWithoutFormat and shaderStorageImageExtendedFormats)
    static const char* extension() { return VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME; }

    bool init(VkDevice device, VkPhysicalDevice gpu, int framesInFlight);
    void shutdown();
    bool ready() const { return device_ != VK_NULL_HANDLE; }
    // Records the upscale into `cmd` (frame slot `slot` < framesInFlight); recreates the
    // internal resources when the sizes change (the caller waits for the GPU to be idle first
    // when needsResize says so)
    bool needsResize(int renderW, int renderH, int outW, int outH) const;
    bool resize(int renderW, int renderH, int outW, int outH);
    void dispatch(VkCommandBuffer cmd, int slot, const Fsr3Inputs& in);

    static int jitterPhaseCount(int renderW, int outW);
    static void jitter(int index, int phaseCount, float& x, float& y);

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;              // every mip
        std::array<VkImageView, 6> mips{};              // SPD: one per written mip
        VkFormat format = VK_FORMAT_UNDEFINED;
        int w = 0, h = 0, mipCount = 1;
    };
    struct Binding {
        uint32_t binding;
        int slot;              // what is bound (Slot in vk_fsr3.cpp)
        VkDescriptorType type; // sampled image (SRV), storage image (UAV) or uniform buffer
    };
    struct Pass {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::vector<Binding> bindings;
    };

    bool createImage(Image& img, VkFormat format, int w, int h, int mips, bool storage);
    void destroyImage(Image& img);
    void destroyResources();
    bool createPass(Pass& p, const uint32_t* code, size_t bytes, const std::vector<Binding>& bindings);
    void run(VkCommandBuffer cmd, const Pass& p, const VkDescriptorImageInfo* images, const VkDescriptorBufferInfo* buffers,
             uint32_t gx, uint32_t gy);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps_{};
    PFN_vkCmdPushDescriptorSetKHR pushDescriptors_ = nullptr;
    VkSampler point_ = VK_NULL_HANDLE, linear_ = VK_NULL_HANDLE;
    std::vector<Pass> passes_;
    // Constant buffers: per frame slot, FSR / SPD / RCAS 256 bytes apart
    VkBuffer cb_ = VK_NULL_HANDLE;
    VkDeviceMemory cbMemory_ = VK_NULL_HANDLE;
    uint8_t* cbMapped_ = nullptr;
    VkBuffer staging_ = VK_NULL_HANDLE; // the Lanczos table, copied in on the first dispatch
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    int frames_ = 2;

    // Internal resources (ffx_fsr3upscaler.cpp's table)
    enum {
        kAccum1, kAccum2, kLuma1, kLuma2, kIntermediate, kShadingChange, kNewLocks, kUpscaled1, kUpscaled2, kSpdMips,
        kFarthestMip1, kLumaHistory1, kLumaHistory2, kSpdAtomic, kDilatedReactive, kLanczos, kDefaultReactivity,
        kDefaultExposure, kFrameInfo, kDilatedDepth, kDilatedMotion, kReconstructedDepth, kImageCount
    };
    std::array<Image, kImageCount> images_{};
    int renderW_ = 0, renderH_ = 0, outW_ = 0, outH_ = 0;
    bool firstExecution_ = true, lutUploaded_ = false;
    uint32_t resourceFrameIndex_ = 0;
    // Constants carried between frames
    float jitterPrev_[2] = {0, 0}, jitterCur_[2] = {0, 0};
    int prevRender_[2] = {0, 0}, prevUpscale_[2] = {0, 0};
    float jitterPhaseCount_ = 0, frameIndex_ = 0;
};

} // namespace cr
