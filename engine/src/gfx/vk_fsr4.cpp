// AMD FSR 4 through AMD's FSR API (third_party/ffx: SDK 2.4's headers, MIT). The library is
// loaded at run time (SDL_LoadObject) from fsr4/ in the game folder: on Windows AMD's own
// amd_fidelityfx_loader_vk.dll, on Linux ffx-vk-linux's libffx_vk_linux.so, which maps AMD's DLLs
// and exports the same five functions. The API records its passes into the frame's command
// buffer and leaves the images in the states they were given in.
#include "gfx/vk_fsr4.hpp"

#include <SDL3/SDL.h>

#ifndef _WIN32
#define __declspec(x) // (the headers mark the API's entry points dllexport)
#endif
#include <ffx_api.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace cr {

namespace {

std::vector<std::string>& searchDirs() {
    static std::vector<std::string> dirs;
    return dirs;
}

struct Functions {
    PfnFfxCreateContext createContext = nullptr;
    PfnFfxDestroyContext destroyContext = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};

constexpr bool kPlatform =
#if (defined(__linux__) && !defined(__ANDROID__) && defined(__x86_64__)) || (defined(_WIN32) && defined(_WIN64))
    true;
#else
    false;
#endif

#ifdef _WIN32
const char* const kLibrary = "amd_fidelityfx_loader_vk.dll";
#else
const char* const kLibrary = "libffx_vk_linux.so";
#endif

// (the runtime's messages: errors and warnings logged)
void message(uint32_t type, const wchar_t* text) {
    std::string s;
    for (const wchar_t* p = text; p && *p; ++p) s += *p < 0x80 ? char(*p) : '?';
    SDL_Log("FSR 4 (%u): %s", unsigned(type), s.c_str());
}

// the version AMD's SDK packs into a provider id: major << 22 | minor << 12 | patch in the low word
void unpackVersion(uint64_t id, int& major, int& minor, int& patch) {
    const uint32_t v = uint32_t(id) & 0x7fffffffu;
    major = int(v >> 22), minor = int((v >> 12) & 0x3ff), patch = int(v & 0xfff);
}

// a feature block's VkBool32s (after sType and pNext) copied from the GPU's supported set
template <typename T> void copyFeatures(T& to, const T& supported) {
    std::memcpy(reinterpret_cast<char*>(&to) + 16, reinterpret_cast<const char*>(&supported) + 16, sizeof(T) - 16);
}

} // namespace

void Fsr4::setSearchDirs(std::vector<std::string> dirs) { searchDirs() = std::move(dirs); }

std::string Fsr4::found() {
    if (!kPlatform) return {};
    std::error_code ec;
    for (const std::string& d : searchDirs()) {
        if (d.empty()) continue;
        const std::filesystem::path dir = d;
        if (std::filesystem::exists(dir / kLibrary, ec) && std::filesystem::exists(dir / "amd_fidelityfx_loader_vk.dll", ec)) {
            std::string s = dir.string();
            if (s.back() != '/' && s.back() != '\\') s += '/';
            return s;
        }
    }
    return {};
}

void Fsr4::deviceRequirements(VkPhysicalDevice gpu, std::vector<const char*>& ext, VkPhysicalDeviceFeatures2& f2,
                              VkPhysicalDeviceVulkan12Features& f12, VkPhysicalDeviceVulkan13Features& f13) {
    if (!kPlatform) {
        problem_ = "FSR 4 runs on Linux and Windows (x86-64)";
        return;
    }
    dir_ = found();
    if (dir_.empty()) return; // (problem_ says where the files go)
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(gpu, &p);
    if (p.vendorID != 0x1002) {
        problem_ = "FSR 4 needs an AMD GPU (RDNA 2 or later)";
        return;
    }
    // every feature this GPU has (what FSR 4's shaders take among them: float16 / int8 storage and
    // arithmetic, subgroup size control, integer dot products ...), as AMD's probe enables them
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> have(n);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, have.data());
    auto has = [&](const char* name) {
        return std::any_of(have.begin(), have.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    VkPhysicalDeviceVulkan13Features s13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features s12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &s13};
    VkPhysicalDeviceVulkan11Features s11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &s12};
    VkPhysicalDeviceFeatures2 s2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s11};
    derivatives_ = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    coherent_ = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD};
    const bool derivatives = has(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME), coherent = has(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME);
    if (derivatives) derivatives_.pNext = s13.pNext, s13.pNext = &derivatives_;
    if (coherent) coherent_.pNext = s13.pNext, s13.pNext = &coherent_;
    vkGetPhysicalDeviceFeatures2(gpu, &s2);
    const VkBool32 robust = f2.features.robustBufferAccess; // (as the game had it: a cost on its own)
    f2.features = s2.features;
    f2.features.robustBufferAccess = robust;
    f11_ = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    copyFeatures(f11_, s11);
    copyFeatures(f12, s12);
    copyFeatures(f13, s13);
    f11_.pNext = f2.pNext; // (f2 -> f11 -> f12 -> f13 -> the extensions')
    f2.pNext = &f11_;
    derivatives_.pNext = coherent_.pNext = nullptr;
    if (derivatives) derivatives_.pNext = f13.pNext, f13.pNext = &derivatives_, ext.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
    if (coherent) coherent_.pNext = f13.pNext, f13.pNext = &coherent_, ext.push_back(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME);
    wanted_ = true;
}

bool Fsr4::init(VkInstance, VkPhysicalDevice gpu, VkDevice device) {
    if (!wanted_) return false;
    device_ = device;
    gpu_ = gpu;
    const std::string lib = dir_ + kLibrary;
    lib_ = SDL_LoadObject(lib.c_str());
    if (!lib_) {
        problem_ = "FSR 4: " + lib + " didn't load: " + SDL_GetError();
        SDL_Log("%s", problem_.c_str());
        return false;
    }
#ifndef _WIN32
    { // ffx-vk-linux: AMD's DLLs mapped from the same folder
        auto load = reinterpret_cast<int (*)(const char*)>(SDL_LoadFunction(static_cast<SDL_SharedObject*>(lib_), "ffxvkLoad"));
        auto why = reinterpret_cast<const char* (*)()>(SDL_LoadFunction(static_cast<SDL_SharedObject*>(lib_), "ffxvkError"));
        if (!load || load(dir_.c_str()) != 0) {
            problem_ = std::string("FSR 4: AMD's DLLs didn't load: ") + (why && load ? why() : "not ffx-vk-linux's library");
            SDL_Log("%s", problem_.c_str());
            return false;
        }
    }
#endif
    auto* fn = new Functions;
    auto get = [&](const char* name) { return SDL_LoadFunction(static_cast<SDL_SharedObject*>(lib_), name); };
    fn->createContext = reinterpret_cast<PfnFfxCreateContext>(get("ffxCreateContext"));
    fn->destroyContext = reinterpret_cast<PfnFfxDestroyContext>(get("ffxDestroyContext"));
    fn->configure = reinterpret_cast<PfnFfxConfigure>(get("ffxConfigure"));
    fn->query = reinterpret_cast<PfnFfxQuery>(get("ffxQuery"));
    fn->dispatch = reinterpret_cast<PfnFfxDispatch>(get("ffxDispatch"));
    if (!fn->createContext || !fn->destroyContext || !fn->configure || !fn->query || !fn->dispatch) {
        delete fn;
        problem_ = "FSR 4: " + lib + " doesn't export AMD's FSR API";
        SDL_Log("%s", problem_.c_str());
        return false;
    }
    fn_ = fn;
    // which FSR the SDK offers here: a small context's provider (4.x only with the driver's DLL)
    problem_.clear();
    if (!createContext(128, 72, 256, 144)) return false;
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const ffxReturnCode_t r = fn->query(static_cast<ffxContext*>(&context_), &pv.header);
    int major = 0, minor = 0, patch = 0;
    if (r == FFX_API_RETURN_OK) unpackVersion(pv.versionId, major, minor, patch);
    version_ = pv.versionName ? pv.versionName : std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    destroyContext();
    if (r != FFX_API_RETURN_OK || major < 4) {
        problem_ = "FSR 4 isn't offered here (AMD's SDK gives " + version_ + "): the driver's amdvkffx64.dll (AMD Software 26.10.1 or later) goes beside the SDK DLLs in " + dir_;
        SDL_Log("%s", problem_.c_str());
        return false;
    }
    SDL_Log("FSR %s: AMD's upscaler from %s", version_.c_str(), dir_.c_str());
    return true;
}

bool Fsr4::createContext(int renderW, int renderH, int outW, int outH) {
    auto* fn = static_cast<Functions*>(fn_);
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.header.pNext = &version.header;
    backend.deviceContext = {device_, gpu_, vkGetDeviceProcAddr};
    ffxCreateContextDescUpscale up{};
    up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up.header.pNext = &backend.header;
    up.maxRenderSize = {uint32_t(renderW), uint32_t(renderH)};
    up.maxUpscaleSize = {uint32_t(outW), uint32_t(outH)};
    // (the game's colour is display-ready 8-bit: perceptual; its exposure the upscaler's own)
    up.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    up.fpMessage = &message;
    ffxContext ctx = nullptr;
    const ffxReturnCode_t r = fn->createContext(&ctx, &up.header, nullptr);
    if (r != FFX_API_RETURN_OK || !ctx) {
        problem_ = "FSR 4 couldn't start (code " + std::to_string(r) + ") at " + std::to_string(renderW) + "x" + std::to_string(renderH) + " -> " +
                   std::to_string(outW) + "x" + std::to_string(outH);
        SDL_Log("%s", problem_.c_str());
        return false;
    }
    context_ = ctx;
    ctxRenderW_ = renderW, ctxRenderH_ = renderH, ctxOutW_ = outW, ctxOutH_ = outH;
    return true;
}

void Fsr4::destroyContext() {
    if (!context_ || !fn_) return;
    static_cast<Functions*>(fn_)->destroyContext(static_cast<ffxContext*>(&context_), nullptr);
    context_ = nullptr;
}

void Fsr4::shutdown() {
    destroyContext();
    delete static_cast<Functions*>(fn_);
    fn_ = nullptr;
    // (the library stays loaded: AMD's DLLs have threads of their own)
    device_ = VK_NULL_HANDLE;
}

bool Fsr4::evaluate(VkCommandBuffer cmd, const Fsr4Inputs& in) {
    if (!problem_.empty() || !fn_) return false;
    if (!context_ || ctxRenderW_ != in.renderW || ctxRenderH_ != in.renderH || ctxOutW_ != in.outW || ctxOutH_ != in.outH) {
        if (context_) {
            vkDeviceWaitIdle(device_);
            destroyContext();
        }
        if (!createContext(in.renderW, in.renderH, in.outW, in.outH)) return false;
    }
    auto resource = [](const Fsr4Image& im, int w, int h, uint32_t state) {
        FfxApiResourceDescription d{};
        d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        d.format = ffxApiGetSurfaceFormatVK(im.format);
        d.width = uint32_t(w), d.height = uint32_t(h), d.depth = 1, d.mipCount = 1;
        d.flags = FFX_API_RESOURCE_FLAGS_NONE;
        d.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
        if (ffxApiIsDepthFormat(im.format)) d.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
        if (im.usage & VK_IMAGE_USAGE_STORAGE_BIT) d.usage |= FFX_API_RESOURCE_USAGE_UAV;
        return ffxApiGetResourceVK(reinterpret_cast<void*>(im.image), d, state);
    };
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd;
    d.color = resource(in.color, in.renderW, in.renderH, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = resource(in.depth, in.renderW, in.renderH, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = resource(in.motion, in.renderW, in.renderH, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (in.reactive.image) d.reactive = resource(in.reactive, in.renderW, in.renderH, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = resource(in.output, in.outW, in.outH, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {in.jitterX, in.jitterY};
    d.motionVectorScale = {float(in.renderW), float(in.renderH)}; // (UV offsets -> render pixels)
    d.renderSize = {uint32_t(in.renderW), uint32_t(in.renderH)};
    d.upscaleSize = {uint32_t(in.outW), uint32_t(in.outH)};
    d.enableSharpening = in.sharpness > 0;
    d.sharpness = in.sharpness;
    d.frameTimeDelta = in.deltaMs;
    d.preExposure = 1.0f;
    d.reset = in.reset;
    d.cameraNear = in.cameraNear;
    d.cameraFar = in.cameraFar;
    d.cameraFovAngleVertical = in.fovY;
    d.viewSpaceToMetersFactor = 1.0f;
    const ffxReturnCode_t r = static_cast<Functions*>(fn_)->dispatch(static_cast<ffxContext*>(&context_), &d.header);
    if (r != FFX_API_RETURN_OK) {
        problem_ = "FSR 4 failed to run (code " + std::to_string(r) + ")";
        SDL_Log("%s", problem_.c_str());
        return false;
    }
    return true;
}

void setFsr4Dirs(std::vector<std::string> dirs) { Fsr4::setSearchDirs(std::move(dirs)); }

} // namespace cr
