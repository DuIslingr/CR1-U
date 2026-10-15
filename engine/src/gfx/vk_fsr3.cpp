// FSR 3.1 host side for Vulkan, after AMD's ffx_fsr3upscaler.cpp (FidelityFX SDK 1.1.4, MIT):
// the same internal resources, per-frame constants, clears and pass order, with AMD's GLSL
// passes. Every internal image stays in VK_IMAGE_LAYOUT_GENERAL; passes are separated by
// compute -> compute memory barriers; descriptors are pushed per dispatch.

#include "gfx/vk_fsr3.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cr {

namespace {

const uint32_t kSpvPrepareInputs[] =
#include "fsr3_prepare_inputs.inc"
    ;
const uint32_t kSpvLumaPyramid[] =
#include "fsr3_luma_pyramid.inc"
    ;
const uint32_t kShadingChangePyramid[] =
#include "fsr3_shading_change_pyramid.inc"
    ;
const uint32_t kSpvShadingChange[] =
#include "fsr3_shading_change.inc"
    ;
const uint32_t kSpvPrepareReactivity[] =
#include "fsr3_prepare_reactivity.inc"
    ;
const uint32_t kSpvLumaInstability[] =
#include "fsr3_luma_instability.inc"
    ;
const uint32_t kSpvAccumulate[] =
#include "fsr3_accumulate.inc"
    ;
const uint32_t kSpvAccumulateSharpen[] =
#include "fsr3_accumulate_sharpen.inc"
    ;
const uint32_t kSpvRcas[] =
#include "fsr3_rcas.inc"
    ;

// What a pass binding refers to this frame (ping-ponged resources resolve per frame)
enum Slot {
    sInColor, sInDepth, sInMotion, sInReactive, sTcMask, sExposure, sOutput,
    sDilatedMotion, sDilatedDepth, sReconDepth, sIntermediate, sCurLuma, sPrevLuma, sSpdAtomic, sFrameInfo,
    sSpdMips, sSpdMip0, sSpdMip1, sSpdMip2, sSpdMip3, sSpdMip4, sSpdMip5, sFarthestMip1, sShadingChange,
    sAccumSrv, sAccumUav, sNewLocks, sDilatedReactive, sLumaHistSrv, sLumaHistUav, sUpscaledSrv, sUpscaledUav,
    sLanczos, sCbFsr, sCbSpd, sCbRcas, sSlotCount
};

constexpr VkDescriptorType kSrv = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
constexpr VkDescriptorType kUav = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType kCb = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
constexpr VkDeviceSize kCbStride = 256; // per constant buffer (minUniformBufferOffsetAlignment <= 256)

// cbFSR3Upscaler (std140, as ffx_fsr3upscaler_callbacks_glsl.h)
struct Constants {
    int32_t renderSize[2], prevRenderSize[2];
    int32_t upscaleSize[2], prevUpscaleSize[2];
    int32_t maxRenderSize[2], maxUpscaleSize[2];
    float deviceToViewDepth[4];
    float jitter[2], prevJitter[2];
    float motionVectorScale[2], downscaleFactor[2];
    float motionVectorJitterCancellation[2];
    float tanHalfFOV, jitterSequenceLength;
    float deltaTime, deltaPreExposure, viewSpaceToMetersFactor, frameIndex;
    float velocityFactor, reactivenessScale, shadingChangeScale, accumulationAddedPerFrame;
    float minDisocclusionAccumulation;
};
static_assert(sizeof(Constants) == 148, "cbFSR3Upscaler layout");

struct SpdConstants {
    uint32_t mips, numWorkGroups;
    uint32_t workGroupOffset[2];
    uint32_t renderSize[2];
};

float halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    for (int i = index; i > 0; i /= base) {
        f /= float(base);
        r += f * float(i % base);
    }
    return r;
}

float lanczos2(float x) {
    const float pi = 3.14159265358979f;
    return std::fabs(x) < 1e-6f ? 1.0f : (std::sin(pi * x) / (pi * x)) * (std::sin(0.5f * pi * x) / (0.5f * pi * x));
}

uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = int((x >> 23) & 0xff) - 127 + 15;
    const uint32_t mant = x & 0x7fffffu;
    if (exp <= 0) return uint16_t(sign);
    if (exp >= 31) return uint16_t(sign | 0x7c00u);
    return uint16_t(sign | uint32_t(exp) << 10 | mant >> 13);
}

bool storageFormat(VkPhysicalDevice gpu, VkFormat f, VkFormatFeatureFlags extra = 0) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(gpu, f, &p);
    const VkFormatFeatureFlags want = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT | extra;
    return (p.optimalTilingFeatures & want) == want;
}

} // namespace

bool Fsr3::supported(VkPhysicalDevice gpu) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> ext(n);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, ext.data());
    bool push = false;
    for (const VkExtensionProperties& e : ext)
        if (!std::strcmp(e.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) push = true;
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(gpu, &f);
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(gpu, &p2);
    const bool quads = (sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                       (sg.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT);
    return push && f.shaderStorageImageWriteWithoutFormat && f.shaderStorageImageExtendedFormats && quads &&
           storageFormat(gpu, VK_FORMAT_R8_UNORM) && storageFormat(gpu, VK_FORMAT_R16_SFLOAT) &&
           storageFormat(gpu, VK_FORMAT_R16G16_SFLOAT) && storageFormat(gpu, VK_FORMAT_R16G16B16A16_SFLOAT) &&
           storageFormat(gpu, VK_FORMAT_R8G8B8A8_UNORM) && storageFormat(gpu, VK_FORMAT_R32_SFLOAT) &&
           storageFormat(gpu, VK_FORMAT_R32G32B32A32_SFLOAT) &&
           storageFormat(gpu, VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT);
}

int Fsr3::jitterPhaseCount(int renderW, int outW) {
    const float ratio = float(outW) / float(std::max(renderW, 1));
    return int(8.0f * ratio * ratio);
}

void Fsr3::jitter(int index, int phaseCount, float& x, float& y) {
    phaseCount = std::max(phaseCount, 1);
    x = halton(index % phaseCount + 1, 2) - 0.5f;
    y = halton(index % phaseCount + 1, 3) - 0.5f;
}

bool Fsr3::init(VkDevice device, VkPhysicalDevice gpu, int framesInFlight) {
    device_ = device;
    gpu_ = gpu;
    frames_ = std::max(framesInFlight, 1);
    vkGetPhysicalDeviceMemoryProperties(gpu, &memProps_);
    pushDescriptors_ = PFN_vkCmdPushDescriptorSetKHR(vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
    if (!pushDescriptors_) {
        device_ = VK_NULL_HANDLE;
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = i ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(device, &si, nullptr, i ? &linear_ : &point_) != VK_SUCCESS) {
            shutdown();
            return false;
        }
    }
    // Pass bindings, from each pass .glsl's FSR3UPSCALER_BIND_* numbers
    const std::vector<Binding> prepareInputs = {
        {0, sInMotion, kSrv}, {1, sInDepth, kSrv}, {2, sInColor, kSrv}, {3, sDilatedMotion, kUav}, {4, sDilatedDepth, kUav},
        {5, sReconDepth, kUav}, {6, sIntermediate, kUav}, {7, sCurLuma, kUav}, {8, sCbFsr, kCb}};
    const std::vector<Binding> lumaPyramid = {
        {0, sCurLuma, kSrv}, {1, sIntermediate, kSrv}, {2, sSpdAtomic, kUav}, {3, sFrameInfo, kUav},
        {4, sSpdMip0, kUav}, {5, sSpdMip1, kUav}, {6, sSpdMip2, kUav}, {7, sSpdMip3, kUav}, {8, sSpdMip4, kUav},
        {9, sSpdMip5, kUav}, {10, sFarthestMip1, kUav}, {11, sCbFsr, kCb}, {12, sCbSpd, kCb}};
    const std::vector<Binding> shadingChangePyramid = {
        {0, sCurLuma, kSrv}, {1, sPrevLuma, kSrv}, {2, sDilatedMotion, kSrv}, {3, sExposure, kSrv},
        {4, sSpdAtomic, kUav}, {5, sSpdMip0, kUav}, {6, sSpdMip1, kUav}, {7, sSpdMip2, kUav}, {8, sSpdMip3, kUav},
        {9, sSpdMip4, kUav}, {10, sSpdMip5, kUav}, {11, sCbFsr, kCb}, {12, sCbSpd, kCb}};
    const std::vector<Binding> shadingChange = {{0, sSpdMips, kSrv}, {1, sShadingChange, kUav}, {2, sCbFsr, kCb}};
    const std::vector<Binding> prepareReactivity = {
        {0, sReconDepth, kSrv}, {1, sDilatedMotion, kSrv}, {2, sDilatedDepth, kSrv}, {3, sInReactive, kSrv},
        {4, sTcMask, kSrv}, {5, sAccumSrv, kSrv}, {6, sShadingChange, kSrv}, {7, sCurLuma, kSrv}, {8, sExposure, kSrv},
        {9, sDilatedReactive, kUav}, {10, sNewLocks, kUav}, {11, sAccumUav, kUav}, {12, sCbFsr, kCb}};
    const std::vector<Binding> lumaInstability = {
        {0, sExposure, kSrv}, {1, sDilatedReactive, kSrv}, {2, sDilatedMotion, kSrv}, {3, sFrameInfo, kSrv},
        {4, sLumaHistSrv, kSrv}, {5, sFarthestMip1, kSrv}, {6, sCurLuma, kSrv}, {7, sLumaHistUav, kUav},
        {8, sIntermediate, kUav}, {9, sCbFsr, kCb}};
    const std::vector<Binding> accumulate = {
        {0, sExposure, kSrv}, {1, sDilatedReactive, kSrv}, {2, sDilatedMotion, kSrv}, {3, sUpscaledSrv, kSrv},
        {4, sLanczos, kSrv}, {5, sFarthestMip1, kSrv}, {6, sCurLuma, kSrv}, {7, sIntermediate, kSrv}, {8, sInColor, kSrv},
        {9, sUpscaledUav, kUav}, {10, sOutput, kUav}, {11, sNewLocks, kUav}, {12, sCbFsr, kCb}};
    const std::vector<Binding> rcas = {
        {0, sExposure, kSrv}, {1, sUpscaledUav, kSrv}, {2, sOutput, kUav}, {3, sCbFsr, kCb}, {4, sCbRcas, kCb}};
    passes_.resize(9);
    bool ok = createPass(passes_[0], kSpvPrepareInputs, sizeof kSpvPrepareInputs, prepareInputs) &&
              createPass(passes_[1], kSpvLumaPyramid, sizeof kSpvLumaPyramid, lumaPyramid) &&
              createPass(passes_[2], kShadingChangePyramid, sizeof kShadingChangePyramid, shadingChangePyramid) &&
              createPass(passes_[3], kSpvShadingChange, sizeof kSpvShadingChange, shadingChange) &&
              createPass(passes_[4], kSpvPrepareReactivity, sizeof kSpvPrepareReactivity, prepareReactivity) &&
              createPass(passes_[5], kSpvLumaInstability, sizeof kSpvLumaInstability, lumaInstability) &&
              createPass(passes_[6], kSpvAccumulate, sizeof kSpvAccumulate, accumulate) &&
              createPass(passes_[7], kSpvAccumulateSharpen, sizeof kSpvAccumulateSharpen, accumulate) &&
              createPass(passes_[8], kSpvRcas, sizeof kSpvRcas, rcas);
    // Constant buffers + the Lanczos table's staging copy, host-visible
    auto hostBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, void** mapped) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        if (vkCreateBuffer(device_, &bi, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
            if ((req.memoryTypeBits >> i & 1) && (memProps_.memoryTypes[i].propertyFlags & want) == want) {
                ai.memoryTypeIndex = i;
                break;
            }
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(device_, buf, mem, 0);
        return vkMapMemory(device_, mem, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS;
    };
    void* cb = nullptr;
    void* staging = nullptr;
    ok = ok && hostBuffer(kCbStride * 3 * VkDeviceSize(frames_), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, cb_, cbMemory_, &cb) &&
         hostBuffer(256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_, stagingMemory_, &staging);
    if (!ok) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "FSR 3.1: setup failed");
        shutdown();
        return false;
    }
    cbMapped_ = static_cast<uint8_t*>(cb);
    int16_t lut[128];
    for (int i = 0; i < 128; ++i) lut[i] = int16_t(std::lround(lanczos2(2.0f * float(i) / 127.0f) * 32767.0f));
    std::memcpy(staging, lut, sizeof lut);
    return true;
}

void Fsr3::shutdown() {
    if (!device_) return;
    destroyResources();
    for (Pass& p : passes_) {
        if (p.pipeline) vkDestroyPipeline(device_, p.pipeline, nullptr);
        if (p.layout) vkDestroyPipelineLayout(device_, p.layout, nullptr);
        if (p.setLayout) vkDestroyDescriptorSetLayout(device_, p.setLayout, nullptr);
    }
    passes_.clear();
    if (cb_) vkDestroyBuffer(device_, cb_, nullptr);
    if (cbMemory_) vkFreeMemory(device_, cbMemory_, nullptr);
    if (staging_) vkDestroyBuffer(device_, staging_, nullptr);
    if (stagingMemory_) vkFreeMemory(device_, stagingMemory_, nullptr);
    if (point_) vkDestroySampler(device_, point_, nullptr);
    if (linear_) vkDestroySampler(device_, linear_, nullptr);
    cb_ = staging_ = VK_NULL_HANDLE;
    cbMemory_ = stagingMemory_ = VK_NULL_HANDLE;
    point_ = linear_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

bool Fsr3::createPass(Pass& p, const uint32_t* code, size_t bytes, const std::vector<Binding>& bindings) {
    p.bindings = bindings;
    std::vector<VkDescriptorSetLayoutBinding> lb;
    for (const Binding& b : bindings) {
        VkDescriptorSetLayoutBinding x{};
        x.binding = b.binding;
        x.descriptorType = b.type;
        x.descriptorCount = 1;
        x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        lb.push_back(x);
    }
    for (uint32_t i = 0; i < 2; ++i) { // s_PointClamp, s_LinearClamp
        VkDescriptorSetLayoutBinding x{};
        x.binding = 1000 + i;
        x.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        x.descriptorCount = 1;
        x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        x.pImmutableSamplers = i ? &linear_ : &point_;
        lb.push_back(x);
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = uint32_t(lb.size());
    li.pBindings = lb.data();
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &p.setLayout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &p.setLayout;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &p.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = p.layout;
    const VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline);
    vkDestroyShaderModule(device_, m, nullptr);
    return r == VK_SUCCESS;
}

bool Fsr3::createImage(Image& img, VkFormat format, int w, int h, int mips, bool storage) {
    img.format = format;
    img.w = std::max(w, 1);
    img.h = std::max(h, 1);
    img.mipCount = mips > 0 ? mips : 1 + int(std::floor(std::log2(float(std::max(img.w, img.h)))));
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {uint32_t(img.w), uint32_t(img.h), 1};
    ii.mipLevels = uint32_t(img.mipCount);
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    if (vkCreateImage(device_, &ii, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
        if ((req.memoryTypeBits >> i & 1) && (memProps_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            ai.memoryTypeIndex = i;
            break;
        }
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ai, nullptr, &img.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(img.mipCount), 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &img.view) != VK_SUCCESS) return false;
    if (img.mipCount > 1)
        for (int m = 0; m < 6; ++m) { // SPD writes mips 0..5 (the last real mip stands in past the end)
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(std::min(m, img.mipCount - 1)), 1, 0, 1};
            if (vkCreateImageView(device_, &vi, nullptr, &img.mips[size_t(m)]) != VK_SUCCESS) return false;
        }
    return true;
}

void Fsr3::destroyImage(Image& img) {
    for (VkImageView& v : img.mips)
        if (v) vkDestroyImageView(device_, v, nullptr);
    if (img.view) vkDestroyImageView(device_, img.view, nullptr);
    if (img.image) vkDestroyImage(device_, img.image, nullptr);
    if (img.memory) vkFreeMemory(device_, img.memory, nullptr);
    img = Image();
}

void Fsr3::destroyResources() {
    for (Image& img : images_) destroyImage(img);
    renderW_ = renderH_ = outW_ = outH_ = 0;
}

bool Fsr3::needsResize(int renderW, int renderH, int outW, int outH) const {
    return renderW != renderW_ || renderH != renderH_ || outW != outW_ || outH != outH_;
}

bool Fsr3::resize(int rw, int rh, int ow, int oh) {
    destroyResources();
    const int hw = std::max(rw / 2, 1), hh = std::max(rh / 2, 1);
    struct Desc {
        int id;
        VkFormat format;
        int w, h, mips;
    };
    // ffx_fsr3upscaler.cpp's internal surfaces (max render size = render size: resized here)
    const Desc table[] = {
        {kAccum1, VK_FORMAT_R8_UNORM, rw, rh, 1},
        {kAccum2, VK_FORMAT_R8_UNORM, rw, rh, 1},
        {kLuma1, VK_FORMAT_R16_SFLOAT, rw, rh, 1},
        {kLuma2, VK_FORMAT_R16_SFLOAT, rw, rh, 1},
        {kIntermediate, VK_FORMAT_R16_SFLOAT, rw, rh, 1},
        {kShadingChange, VK_FORMAT_R8_UNORM, hw, hh, 1},
        {kNewLocks, VK_FORMAT_R8_UNORM, ow, oh, 1},
        {kUpscaled1, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, 1},
        {kUpscaled2, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, 1},
        {kSpdMips, VK_FORMAT_R16G16_SFLOAT, hw, hh, 0},
        {kFarthestMip1, VK_FORMAT_R16_SFLOAT, hw, hh, 1},
        // The SDK asks for RGBA16F but its shader stores through an rgba8 image: RGBA8 matches
        {kLumaHistory1, VK_FORMAT_R8G8B8A8_UNORM, rw, rh, 1},
        {kLumaHistory2, VK_FORMAT_R8G8B8A8_UNORM, rw, rh, 1},
        {kSpdAtomic, VK_FORMAT_R32_UINT, 1, 1, 1},
        {kDilatedReactive, VK_FORMAT_R8G8B8A8_UNORM, rw, rh, 1},
        {kLanczos, VK_FORMAT_R16_SNORM, 128, 1, 1},
        {kDefaultReactivity, VK_FORMAT_R8_UNORM, 1, 1, 1},
        {kDefaultExposure, VK_FORMAT_R32G32_SFLOAT, 1, 1, 1},
        {kFrameInfo, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 1},
        {kDilatedDepth, VK_FORMAT_R32_SFLOAT, rw, rh, 1},
        {kDilatedMotion, VK_FORMAT_R16G16_SFLOAT, rw, rh, 1},
        {kReconstructedDepth, VK_FORMAT_R32_UINT, rw, rh, 1},
    };
    for (const Desc& d : table) {
        const bool readOnly = d.id == kLanczos || d.id == kDefaultReactivity || d.id == kDefaultExposure;
        if (!createImage(images_[size_t(d.id)], d.format, d.w, d.h, d.mips, !readOnly)) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "FSR 3.1: out of video memory for %dx%d -> %dx%d", rw, rh, ow, oh);
            destroyResources();
            return false;
        }
    }
    renderW_ = rw;
    renderH_ = rh;
    outW_ = ow;
    outH_ = oh;
    firstExecution_ = true;
    lutUploaded_ = false;
    resourceFrameIndex_ = 0;
    jitterPhaseCount_ = 0;
    return true;
}

void Fsr3::run(VkCommandBuffer cmd, const Pass& p, const VkDescriptorImageInfo* images, const VkDescriptorBufferInfo* buffers,
               uint32_t gx, uint32_t gy) {
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(p.bindings.size());
    for (const Binding& b : p.bindings) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = b.binding;
        w.descriptorCount = 1;
        w.descriptorType = b.type;
        if (b.type == kCb) w.pBufferInfo = &buffers[b.slot];
        else w.pImageInfo = &images[b.slot];
        writes.push_back(w);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    pushDescriptors_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, uint32_t(writes.size()), writes.data());
    vkCmdDispatch(cmd, std::max(gx, 1u), std::max(gy, 1u), 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

void Fsr3::dispatch(VkCommandBuffer cmd, int slot, const Fsr3Inputs& in) {
    if (!device_ || !renderW_) return;
    const bool odd = resourceFrameIndex_ & 1;
    const bool reset = in.reset || firstExecution_;

    // ---- clears (and, the first time, layouts and the Lanczos table) ----
    // Each image is cleared once per dispatch: two clears of one image back to back would race
    // (sync validation), so the first one's value stands (the reset values come first)
    std::array<bool, kImageCount> cleared{};
    auto clear = [&](int id, float r, float g, float b, float a) {
        if (cleared[size_t(id)]) return;
        cleared[size_t(id)] = true;
        VkClearColorValue c;
        c.float32[0] = r;
        c.float32[1] = g;
        c.float32[2] = b;
        c.float32[3] = a;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
        vkCmdClearColorImage(cmd, images_[size_t(id)].image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
    };
    auto clearUint = [&](int id, uint32_t v) {
        if (cleared[size_t(id)]) return;
        cleared[size_t(id)] = true;
        VkClearColorValue c{};
        c.uint32[0] = c.uint32[1] = c.uint32[2] = c.uint32[3] = v;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, images_[size_t(id)].image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
    };
    {
        // Earlier compute (last frame's FSR) is done with what is cleared now
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
    }
    if (firstExecution_) {
        std::vector<VkImageMemoryBarrier> bs;
        for (Image& img : images_) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
            bs.push_back(b);
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             uint32_t(bs.size()), bs.data());
        clear(kAccum1, 0, 0, 0, 0);
        clear(kAccum2, 0, 0, 0, 0);
        clear(kLuma1, 0, 0, 0, 0);
        clear(kLuma2, 0, 0, 0, 0);
        clear(kDefaultReactivity, 0, 0, 0, 0);
        clear(kDefaultExposure, 0, 0, 0, 0);
        // Never-written history reads as black rather than garbage
        for (int id : {kUpscaled1, kUpscaled2, kLumaHistory1, kLumaHistory2, kIntermediate, kShadingChange, kNewLocks,
                       kFarthestMip1, kDilatedReactive, kDilatedDepth, kDilatedMotion})
            clear(id, 0, 0, 0, 0);
    }
    if (!lutUploaded_) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {128, 1, 1};
        vkCmdCopyBufferToImage(cmd, staging_, images_[kLanczos].image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        lutUploaded_ = true;
    }
    const int accumSrv = odd ? kAccum2 : kAccum1, accumUav = odd ? kAccum1 : kAccum2;
    const int upscaledSrv = odd ? kUpscaled2 : kUpscaled1, upscaledUav = odd ? kUpscaled1 : kUpscaled2;
    const int lumaHistSrv = odd ? kLumaHistory2 : kLumaHistory1, lumaHistUav = odd ? kLumaHistory1 : kLumaHistory2;
    const int curLuma = odd ? kLuma2 : kLuma1, prevLuma = odd ? kLuma1 : kLuma2;
    if (reset) {
        clear(accumSrv, 0, 0, 0, 0);
        clear(kSpdMips, 0, 0, 0, 0);
        clear(kFrameInfo, -1, 1, 0, 0); // auto exposure tracks luma changes for the locks
    }
    clearUint(kReconstructedDepth, 0x3f800000u); // 1.0f: farthest (min depth is stored as bits)
    clearUint(kSpdAtomic, 0);
    clear(kSpdMips, 0, 0, 0, 0);
    firstExecution_ = false;
    {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
    }

    // ---- constants ----
    Constants c{};
    jitterPrev_[0] = jitterCur_[0];
    jitterPrev_[1] = jitterCur_[1];
    jitterCur_[0] = in.jitterX;
    jitterCur_[1] = in.jitterY;
    c.jitter[0] = jitterCur_[0];
    c.jitter[1] = jitterCur_[1];
    c.prevJitter[0] = jitterPrev_[0];
    c.prevJitter[1] = jitterPrev_[1];
    c.renderSize[0] = in.renderW;
    c.renderSize[1] = in.renderH;
    c.prevRenderSize[0] = prevRender_[0] ? prevRender_[0] : in.renderW;
    c.prevRenderSize[1] = prevRender_[1] ? prevRender_[1] : in.renderH;
    c.maxRenderSize[0] = renderW_;
    c.maxRenderSize[1] = renderH_;
    c.upscaleSize[0] = in.outW;
    c.upscaleSize[1] = in.outH;
    c.prevUpscaleSize[0] = prevUpscale_[0] ? prevUpscale_[0] : in.outW;
    c.prevUpscaleSize[1] = prevUpscale_[1] ? prevUpscale_[1] : in.outH;
    c.maxUpscaleSize[0] = outW_;
    c.maxUpscaleSize[1] = outH_;
    prevRender_[0] = in.renderW;
    prevRender_[1] = in.renderH;
    prevUpscale_[0] = in.outW;
    prevUpscale_[1] = in.outH;
    const float aspect = float(in.renderW) / float(in.renderH);
    const float fovX = std::atan(std::tan(in.fovY * 0.5f) * aspect) * 2.0f;
    c.tanHalfFOV = std::tan(fovX * 0.5f);
    c.viewSpaceToMetersFactor = 1.0f;
    {   // device depth -> view depth (non-inverted, finite)
        const float n = std::min(in.cameraNear, in.cameraFar), f = std::max(in.cameraNear, in.cameraFar);
        const float q = f / (n - f);
        c.deviceToViewDepth[0] = -1.0f * q;
        c.deviceToViewDepth[1] = q * n;
        const float cot = std::cos(0.5f * in.fovY) / std::sin(0.5f * in.fovY);
        c.deviceToViewDepth[2] = 1.0f / (cot / aspect);
        c.deviceToViewDepth[3] = 1.0f / cot;
    }
    c.downscaleFactor[0] = float(in.renderW) / float(in.outW);
    c.downscaleFactor[1] = float(in.renderH) / float(in.outH);
    c.deltaPreExposure = 1.0f;
    // Motion vectors arrive in UV units already (scale = render size / render size)
    c.motionVectorScale[0] = 1.0f;
    c.motionVectorScale[1] = 1.0f;
    const int phases = jitterPhaseCount(in.renderW, in.outW);
    if (reset || jitterPhaseCount_ == 0) jitterPhaseCount_ = float(phases);
    else if (float(phases) > jitterPhaseCount_) jitterPhaseCount_ += 1;
    else if (float(phases) < jitterPhaseCount_) jitterPhaseCount_ -= 1;
    c.jitterSequenceLength = jitterPhaseCount_;
    c.deltaTime = std::clamp(in.deltaMs / 1000.0f, 0.0f, 1.0f);
    frameIndex_ = reset ? 0.0f : frameIndex_ + 1.0f;
    c.frameIndex = frameIndex_;
    c.velocityFactor = 1.0f;
    c.reactivenessScale = 1.0f;
    c.shadingChangeScale = 1.0f;
    c.accumulationAddedPerFrame = 1.0f / 3.0f;
    c.minDisocclusionAccumulation = -1.0f / 3.0f;

    // SPD over the render size (ffxSpdSetup with mips from the size)
    SpdConstants spd{};
    const uint32_t gx = uint32_t((in.renderW - 1) / 64 + 1), gy = uint32_t((in.renderH - 1) / 64 + 1);
    spd.numWorkGroups = gx * gy;
    spd.mips = uint32_t(std::min(std::floor(std::log2(float(std::max(in.renderW, in.renderH)))), 12.0f));
    spd.renderSize[0] = uint32_t(in.renderW);
    spd.renderSize[1] = uint32_t(in.renderH);

    // RCAS: sharpness 0..1 -> stops 2..0
    uint32_t rcas[4] = {};
    {
        const float stops = -2.0f * in.sharpness + 2.0f;
        const float sharp = std::exp2(-stops);
        std::memcpy(&rcas[0], &sharp, 4);
        const uint16_t h = toHalf(sharp);
        rcas[1] = uint32_t(h) | uint32_t(h) << 16;
    }
    const VkDeviceSize base = kCbStride * 3 * VkDeviceSize(slot % frames_);
    std::memcpy(cbMapped_ + base, &c, sizeof c);
    std::memcpy(cbMapped_ + base + kCbStride, &spd, sizeof spd);
    std::memcpy(cbMapped_ + base + 2 * kCbStride, rcas, sizeof rcas);

    // ---- bindings this frame ----
    VkDescriptorImageInfo im[sSlotCount] = {};
    VkDescriptorBufferInfo bu[sSlotCount] = {};
    auto internal = [&](int s, int id) { im[s] = {VK_NULL_HANDLE, images_[size_t(id)].view, VK_IMAGE_LAYOUT_GENERAL}; };
    im[sInColor] = {VK_NULL_HANDLE, in.color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    im[sInDepth] = {VK_NULL_HANDLE, in.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    im[sInMotion] = {VK_NULL_HANDLE, in.motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    if (in.reactive) im[sInReactive] = {VK_NULL_HANDLE, in.reactive, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    else internal(sInReactive, kDefaultReactivity);
    internal(sTcMask, kDefaultReactivity);
    internal(sExposure, kDefaultExposure);
    im[sOutput] = {VK_NULL_HANDLE, in.output, VK_IMAGE_LAYOUT_GENERAL};
    internal(sDilatedMotion, kDilatedMotion);
    internal(sDilatedDepth, kDilatedDepth);
    internal(sReconDepth, kReconstructedDepth);
    internal(sIntermediate, kIntermediate);
    internal(sCurLuma, curLuma);
    internal(sPrevLuma, prevLuma);
    internal(sSpdAtomic, kSpdAtomic);
    internal(sFrameInfo, kFrameInfo);
    internal(sSpdMips, kSpdMips);
    for (int m = 0; m < 6; ++m)
        im[sSpdMip0 + m] = {VK_NULL_HANDLE, images_[kSpdMips].mips[size_t(m)], VK_IMAGE_LAYOUT_GENERAL};
    internal(sFarthestMip1, kFarthestMip1);
    internal(sShadingChange, kShadingChange);
    internal(sAccumSrv, accumSrv);
    internal(sAccumUav, accumUav);
    internal(sNewLocks, kNewLocks);
    internal(sDilatedReactive, kDilatedReactive);
    internal(sLumaHistSrv, lumaHistSrv);
    internal(sLumaHistUav, lumaHistUav);
    internal(sUpscaledSrv, upscaledSrv);
    internal(sUpscaledUav, upscaledUav);
    internal(sLanczos, kLanczos);
    bu[sCbFsr] = {cb_, base, sizeof(Constants)};
    bu[sCbSpd] = {cb_, base + kCbStride, sizeof(SpdConstants)};
    bu[sCbRcas] = {cb_, base + 2 * kCbStride, sizeof rcas};

    // ---- passes (ffx_fsr3upscaler.cpp's order and group counts) ----
    const uint32_t srcX = uint32_t((in.renderW + 7) / 8), srcY = uint32_t((in.renderH + 7) / 8);
    const uint32_t dstX = uint32_t((in.outW + 7) / 8), dstY = uint32_t((in.outH + 7) / 8);
    const uint32_t scX = uint32_t((int(in.renderW * 0.5f) + 7) / 8), scY = uint32_t((int(in.renderH * 0.5f) + 7) / 8);
    const bool sharpen = in.sharpness > 0.0f;
    run(cmd, passes_[0], im, bu, srcX, srcY); // prepare inputs
    run(cmd, passes_[1], im, bu, gx, gy);     // luma pyramid
    run(cmd, passes_[2], im, bu, gx, gy);     // shading change pyramid
    run(cmd, passes_[3], im, bu, scX, scY);   // shading change
    run(cmd, passes_[4], im, bu, srcX, srcY); // prepare reactivity
    run(cmd, passes_[5], im, bu, srcX, srcY); // luma instability
    run(cmd, passes_[sharpen ? 7 : 6], im, bu, dstX, dstY);
    if (sharpen) run(cmd, passes_[8], im, bu, uint32_t((in.outW + 15) / 16), uint32_t((in.outH + 15) / 16));
    resourceFrameIndex_ = (resourceFrameIndex_ + 1) % 16;
}

} // namespace cr
