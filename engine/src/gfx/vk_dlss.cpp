// DLSS / DLAA through NGX (DLSS SDK 310.9.1, nvsdk_ngx_*_vk.h). NGX is initialised with a
// project ID (custom engine); the DLSS runtime is looked for next to the executable.

#include "gfx/vk_dlss.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

#ifdef CR_HAVE_DLSS
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>
#endif

namespace cr {

#ifdef CR_HAVE_DLSS

namespace {

// Any GUID identifies a custom-engine project to NGX
constexpr const char* kProjectId = "3c2a9e51-7f04-4d6b-9b8e-c4a0d17e52f3";

std::wstring widen(const char* s) {
    std::wstring w;
    for (; s && *s; ++s) w.push_back(wchar_t(static_cast<unsigned char>(*s)));
    return w;
}

// Where NGX finds the DLSS runtime and keeps its logs: the executable's directory
const std::wstring& basePath() {
    static const std::wstring path = widen(SDL_GetBasePath());
    return path;
}

NVSDK_NGX_FeatureDiscoveryInfo discovery(NVSDK_NGX_FeatureCommonInfo& common, const wchar_t* const* paths) {
    std::memset(&common, 0, sizeof common);
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    NVSDK_NGX_FeatureDiscoveryInfo info;
    std::memset(&info, 0, sizeof info);
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    info.ApplicationDataPath = basePath().c_str();
    info.FeatureInfo = &common;
    return info;
}

// Upscale ratio -> DLSS mode (1 = DLAA)
NVSDK_NGX_PerfQuality_Value quality(float ratio) {
    if (ratio < 1.25f) return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (ratio < 1.6f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ratio < 1.85f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (ratio < 2.5f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

std::vector<std::string>& instanceNames() {
    static std::vector<std::string> names;
    return names;
}

} // namespace

void Dlss::instanceExtensions(std::vector<const char*>& ext) {
    const wchar_t* paths[] = {basePath().c_str()};
    NVSDK_NGX_FeatureCommonInfo common;
    const NVSDK_NGX_FeatureDiscoveryInfo info = discovery(common, paths);
    uint32_t n = 0;
    VkExtensionProperties* props = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &n, &props)) || !props) return;
    // Only when the loader has every one (else the instance would fail to create)
    uint32_t have = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &have, nullptr);
    std::vector<VkExtensionProperties> avail(have);
    vkEnumerateInstanceExtensionProperties(nullptr, &have, avail.data());
    std::vector<std::string>& names = instanceNames();
    names.clear();
    for (uint32_t i = 0; i < n; ++i) {
        bool found = false;
        for (const VkExtensionProperties& a : avail) found = found || !std::strcmp(a.extensionName, props[i].extensionName);
        if (!found) return;
        names.emplace_back(props[i].extensionName);
    }
    for (const std::string& s : names)
        if (std::find_if(ext.begin(), ext.end(), [&](const char* e) { return s == e; }) == ext.end()) ext.push_back(s.c_str());
}

void Dlss::deviceRequirements(VkInstance instance, VkPhysicalDevice gpu, std::vector<const char*>& ext,
                              VkPhysicalDeviceVulkan12Features& features12) {
    static std::vector<std::string> names;
    names.clear();
    extensionsOk_ = false;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(gpu, &p);
    if (p.vendorID != 0x10de) {
        problem_ = "needs an NVIDIA RTX GPU";
        return;
    }
    const wchar_t* paths[] = {basePath().c_str()};
    NVSDK_NGX_FeatureCommonInfo common;
    const NVSDK_NGX_FeatureDiscoveryInfo info = discovery(common, paths);
    NVSDK_NGX_FeatureRequirement req;
    std::memset(&req, 0, sizeof req);
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, gpu, &info, &req)) || req.FeatureSupported != 0) {
        problem_ = "this NVIDIA GPU or driver can't run DLSS";
        return;
    }
    uint32_t n = 0;
    VkExtensionProperties* props = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, gpu, &info, &n, &props))) {
        problem_ = "the NVIDIA driver didn't list DLSS's Vulkan extensions";
        return;
    }
    uint32_t have = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &have, nullptr);
    std::vector<VkExtensionProperties> avail(have);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &have, avail.data());
    for (uint32_t i = 0; i < n && props; ++i) {
        bool found = false;
        for (const VkExtensionProperties& a : avail) found = found || !std::strcmp(a.extensionName, props[i].extensionName);
        if (!found) {
            problem_ = std::string("the GPU lacks ") + props[i].extensionName;
            return;
        }
        names.emplace_back(props[i].extensionName);
    }
    for (const std::string& s : names)
        if (std::find_if(ext.begin(), ext.end(), [&](const char* e) { return s == e; }) == ext.end()) ext.push_back(s.c_str());
    // NGX addresses its buffers by device address
    VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &have12;
    vkGetPhysicalDeviceFeatures2(gpu, &f2);
    if (have12.bufferDeviceAddress) features12.bufferDeviceAddress = VK_TRUE;
    extensionsOk_ = true;
}

bool Dlss::init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device) {
    if (!extensionsOk_) return false;
    device_ = device;
    const wchar_t* paths[] = {basePath().c_str()};
    NVSDK_NGX_FeatureCommonInfo common;
    std::memset(&common, 0, sizeof common);
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                                              basePath().c_str(), instance, gpu, device, vkGetInstanceProcAddr,
                                                              vkGetDeviceProcAddr, &common);
    if (NVSDK_NGX_FAILED(r)) {
        problem_ = "NVIDIA NGX didn't start (driver too old?)";
        return false;
    }
    initialised_ = true;
    NVSDK_NGX_Parameter* params = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&params)) || !params) {
        problem_ = "NVIDIA NGX has no parameters";
        return false;
    }
    params_ = params;
    int available = 0, needsDriver = 0;
    NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    if (needsDriver) problem_ = "DLSS needs a newer NVIDIA driver";
    else if (!available) problem_ = "DLSS isn't available (libnvidia-ngx-dlss.so missing next to the game?)";
    else problem_.clear();
    if (!problem_.empty()) SDL_Log("DLSS: %s", problem_.c_str());
    return problem_.empty();
}

void Dlss::shutdown() {
    if (feature_) NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(feature_));
    feature_ = nullptr;
    if (params_) NVSDK_NGX_VULKAN_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(params_));
    params_ = nullptr;
    if (initialised_) NVSDK_NGX_VULKAN_Shutdown1(device_);
    initialised_ = false;
    device_ = VK_NULL_HANDLE;
}

bool Dlss::evaluate(VkCommandBuffer cmd, const DlssInputs& in) {
    if (!problem_.empty() || !params_) return false;
    NVSDK_NGX_Parameter* params = static_cast<NVSDK_NGX_Parameter*>(params_);
    const float ratio = float(in.outW) / float(std::max(in.renderW, 1));
    const int q = int(quality(ratio));
    if (!feature_ || featureW_ != in.renderW || featureH_ != in.renderH || featureOutW_ != in.outW ||
        featureOutH_ != in.outH || featureQuality_ != q) {
        if (feature_) {
            vkDeviceWaitIdle(device_);
            NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(feature_));
            feature_ = nullptr;
        }
        NVSDK_NGX_DLSS_Create_Params cp;
        std::memset(&cp, 0, sizeof cp);
        cp.Feature.InWidth = unsigned(in.renderW);
        cp.Feature.InHeight = unsigned(in.renderH);
        cp.Feature.InTargetWidth = unsigned(in.outW);
        cp.Feature.InTargetHeight = unsigned(in.outH);
        cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value(q);
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes; // LDR, unjittered MVs, 0 = near
        NVSDK_NGX_Handle* h = nullptr;
        if (NVSDK_NGX_FAILED(NGX_VULKAN_CREATE_DLSS_EXT1(device_, cmd, 1, 1, &h, params, &cp)) || !h) {
            problem_ = "DLSS couldn't start at this resolution";
            SDL_Log("DLSS: %s (%dx%d -> %dx%d)", problem_.c_str(), in.renderW, in.renderH, in.outW, in.outH);
            return false;
        }
        feature_ = h;
        featureW_ = in.renderW;
        featureH_ = in.renderH;
        featureOutW_ = in.outW;
        featureOutH_ = in.outH;
        featureQuality_ = q;
    }
    const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImageSubresourceRange depth{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    NVSDK_NGX_Resource_VK rColor = NVSDK_NGX_Create_ImageView_Resource_VK(in.color.view, in.color.image, color, in.color.format,
                                                                           unsigned(in.renderW), unsigned(in.renderH), false);
    NVSDK_NGX_Resource_VK rDepth = NVSDK_NGX_Create_ImageView_Resource_VK(in.depth.view, in.depth.image, depth, in.depth.format,
                                                                           unsigned(in.renderW), unsigned(in.renderH), false);
    NVSDK_NGX_Resource_VK rMotion = NVSDK_NGX_Create_ImageView_Resource_VK(in.motion.view, in.motion.image, color,
                                                                            in.motion.format, unsigned(in.renderW),
                                                                            unsigned(in.renderH), false);
    NVSDK_NGX_Resource_VK rOut = NVSDK_NGX_Create_ImageView_Resource_VK(in.output.view, in.output.image, color, in.output.format,
                                                                         unsigned(in.outW), unsigned(in.outH), true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep;
    std::memset(&ep, 0, sizeof ep);
    ep.Feature.pInColor = &rColor;
    ep.Feature.pInOutput = &rOut;
    ep.Feature.InSharpness = in.sharpness;
    ep.pInDepth = &rDepth;
    ep.pInMotionVectors = &rMotion;
    ep.InJitterOffsetX = in.jitterX;
    ep.InJitterOffsetY = in.jitterY;
    ep.InRenderSubrectDimensions.Width = unsigned(in.renderW);
    ep.InRenderSubrectDimensions.Height = unsigned(in.renderH);
    ep.InReset = in.reset ? 1 : 0;
    ep.InMVScaleX = float(in.renderW); // UV offsets -> render pixels
    ep.InMVScaleY = float(in.renderH);
    ep.InFrameTimeDeltaInMsec = in.deltaMs;
    if (NVSDK_NGX_FAILED(NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, static_cast<NVSDK_NGX_Handle*>(feature_), params, &ep))) {
        problem_ = "DLSS failed to run";
        SDL_Log("DLSS: %s", problem_.c_str());
        return false;
    }
    return true;
}

#else // built without the DLSS SDK

void Dlss::instanceExtensions(std::vector<const char*>&) {}
void Dlss::deviceRequirements(VkInstance, VkPhysicalDevice gpu, std::vector<const char*>&, VkPhysicalDeviceVulkan12Features&) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(gpu, &p);
    problem_ = p.vendorID != 0x10de ? "needs an NVIDIA RTX GPU" : "built without the DLSS SDK";
}
bool Dlss::init(VkInstance, VkPhysicalDevice, VkDevice) { return false; }
void Dlss::shutdown() {}
bool Dlss::evaluate(VkCommandBuffer, const DlssInputs&) { return false; }

#endif

} // namespace cr
