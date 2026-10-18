// Built on Apple platforms only (engine/CMakeLists.txt), with ARC (-fobjc-arc)
#define VK_USE_PLATFORM_METAL_EXT 1
#include "gfx/vk_metalfx.hpp"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <cstring>

namespace cr {

namespace {
VkExportMetalObjectCreateInfoEXT gTextureExport{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT, nullptr,
                                                VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT};
} // namespace

struct MetalFx::Impl {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkExportMetalObjectsEXT exportObjects = nullptr;
    id<MTLDevice> mtlDevice = nil;
    id<MTLCommandQueue> mtlQueue = nil;
    id scaler = nil; // id<MTLFXTemporalScaler> (macOS 13)
    int rw = 0, rh = 0, ow = 0, oh = 0;
    MTLPixelFormat cf = MTLPixelFormatInvalid, df = MTLPixelFormatInvalid, mf = MTLPixelFormatInvalid, of = MTLPixelFormatInvalid;
    std::string problem = "not started";

    id<MTLTexture> texture(VkImage image) const {
        if (!exportObjects || !image) return nil;
        VkExportMetalTextureInfoEXT t{VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
        t.image = image;
        t.plane = VK_IMAGE_ASPECT_PLANE_0_BIT; // (a single-plane image's)
        VkExportMetalObjectsInfoEXT o{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &t};
        exportObjects(device, &o);
        return t.mtlTexture;
    }
};

MetalFx::MetalFx() : impl_(std::make_unique<Impl>()) {}
MetalFx::~MetalFx() { shutdown(); }

void MetalFx::deviceExtensions(VkPhysicalDevice gpu, std::vector<const char*>& out) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> have(n);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, have.data());
    for (const VkExtensionProperties& e : have)
        if (std::strcmp(e.extensionName, VK_EXT_METAL_OBJECTS_EXTENSION_NAME) == 0) out.push_back(VK_EXT_METAL_OBJECTS_EXTENSION_NAME);
}

const void* MetalFx::imageExportInfo() { return &gTextureExport; }

bool MetalFx::init(VkInstance instance, VkDevice device, VkQueue queue) {
    Impl& m = *impl_;
    m.device = device;
    m.exportObjects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(vkGetDeviceProcAddr(device, "vkExportMetalObjectsEXT"));
    (void)instance;
    if (!m.exportObjects) return m.problem = "needs MoltenVK's VK_EXT_metal_objects", false;
    VkExportMetalDeviceInfoEXT d{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
    VkExportMetalCommandQueueInfoEXT q{VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT, &d};
    q.queue = queue;
    VkExportMetalObjectsInfoEXT o{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &q};
    m.exportObjects(device, &o);
    m.mtlDevice = d.mtlDevice;
    m.mtlQueue = q.mtlCommandQueue;
    if (!m.mtlDevice || !m.mtlQueue) return m.problem = "MoltenVK gave no Metal device or queue", false;
    if (@available(macOS 13.0, iOS 16.0, *)) {
        if (![MTLFXTemporalScalerDescriptor supportsDevice:m.mtlDevice]) return m.problem = "this GPU can't run MetalFX", false;
    } else {
        return m.problem = "MetalFX needs macOS 13", false;
    }
    m.problem.clear();
    return true;
}

void MetalFx::shutdown() {
    if (!impl_) return;
    impl_->scaler = nil;
    impl_->mtlQueue = nil;
    impl_->mtlDevice = nil;
    impl_->problem = "not started";
}

std::string MetalFx::problem() const { return impl_->problem; }

bool MetalFx::encode(const MetalFxInputs& in) {
    Impl& m = *impl_;
    if (!m.problem.empty()) return false;
    if (@available(macOS 13.0, iOS 16.0, *)) {
        @autoreleasepool {
            id<MTLTexture> color = m.texture(in.color), depth = m.texture(in.depth), motion = m.texture(in.motion), out = m.texture(in.output);
            if (!color || !depth || !motion || !out) return false;
            bool reset = in.reset;
            if (!m.scaler || m.rw != in.renderW || m.rh != in.renderH || m.ow != in.outW || m.oh != in.outH ||
                m.cf != color.pixelFormat || m.df != depth.pixelFormat || m.mf != motion.pixelFormat || m.of != out.pixelFormat) {
                MTLFXTemporalScalerDescriptor* d = [MTLFXTemporalScalerDescriptor new];
                d.colorTextureFormat = color.pixelFormat;
                d.depthTextureFormat = depth.pixelFormat;
                d.motionTextureFormat = motion.pixelFormat;
                d.outputTextureFormat = out.pixelFormat;
                d.inputWidth = NSUInteger(in.renderW);
                d.inputHeight = NSUInteger(in.renderH);
                d.outputWidth = NSUInteger(in.outW);
                d.outputHeight = NSUInteger(in.outH);
                d.autoExposureEnabled = NO;
                m.scaler = [d newTemporalScalerWithDevice:m.mtlDevice];
                if (!m.scaler) return m.problem = "MetalFX could not make its scaler", false;
                m.rw = in.renderW, m.rh = in.renderH, m.ow = in.outW, m.oh = in.outH;
                m.cf = color.pixelFormat, m.df = depth.pixelFormat, m.mf = motion.pixelFormat, m.of = out.pixelFormat;
                reset = true;
            }
            id<MTLFXTemporalScaler> s = m.scaler;
            s.colorTexture = color;
            s.depthTexture = depth;
            s.motionTexture = motion;
            s.outputTexture = out;
            s.inputContentWidth = NSUInteger(in.renderW);
            s.inputContentHeight = NSUInteger(in.renderH);
            s.jitterOffsetX = in.jitterX;
            s.jitterOffsetY = in.jitterY;
            s.motionVectorScaleX = in.motionScaleX;
            s.motionVectorScaleY = in.motionScaleY;
            s.depthReversed = NO;
            s.reset = reset;
            id<MTLCommandBuffer> cb = [m.mtlQueue commandBuffer];
            if (!cb) return false;
            cb.label = @"MetalFX temporal upscale";
            [s encodeToCommandBuffer:cb];
            [cb commit];
            return true;
        }
    }
    return false;
}

} // namespace cr
