// Vulkan 1.3 device. Frames render into an offscreen colour + depth target with dynamic
// rendering, then blit to the swapchain (or stay for readPixels). GL behaviour is matched
// where the game relies on it:
// - clip space: matrices arrive GL-style; depth is remapped to 0..1 and the viewport height
//   is negative, so images and triangle winding come out as in GL;
// - meshes re-skinned between draws (several instances of one model per frame): each update
//   goes into a per-frame ring buffer, so earlier draws keep the vertices they were given;
// - textures and meshes destroyed mid-frame are released once no frame in flight uses them.
// With a temporal upscaler on, the scene renders into smaller targets (colour, depth, motion
// vectors, reactive mask) with sub-pixel jitter, is upscaled into the frame by FSR 3.1 or
// DLSS, and the UI draws over that.

#include "gfx/renderer.hpp"

#ifdef CR_HAVE_VULKAN

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "gfx/vk_dlss.hpp"
#ifdef __APPLE__
#include "gfx/vk_metalfx.hpp"
#endif
#include "gfx/vk_fsr3.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace cr {

namespace {

const uint32_t kQuadVert[] =
#include "quad.vert.inc"
    ;
const uint32_t kQuadFrag[] =
#include "quad.frag.inc"
    ;
const uint32_t kMeshVert[] =
#include "mesh.vert.inc"
    ;
const uint32_t kMeshFrag[] =
#include "mesh.frag.inc"
    ;
const uint32_t kSceneQuadVert[] =
#include "scene_quad.vert.inc"
    ;
const uint32_t kSceneQuadFrag[] =
#include "scene_quad.frag.inc"
    ;
const uint32_t kSceneMeshVert[] =
#include "scene_mesh.vert.inc"
    ;
const uint32_t kSceneMeshFrag[] =
#include "scene_mesh.frag.inc"
    ;

constexpr int kFrames = 2;                      // frames in flight
constexpr VkDeviceSize kBlockSize = 64u << 20;  // memory blocks / buffer arenas
constexpr VkDeviceSize kRingSize = 16u << 20;   // per-frame dynamic vertices, grows as needed
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT; // scene: motion vectors
constexpr VkFormat kReactiveFormat = VK_FORMAT_R8_UNORM;    // scene: reactive mask
constexpr uint32_t kScenePipeline = 1u << 16;               // pipeline key: the scene's
constexpr uint32_t kSceneDepthWrite = 1u << 2;              // scene key: the draw writes depth
constexpr uint32_t kQuadKey = 1u | 2u | 4u << 4 | 5u << 8;  // colour writes, src alpha / 1 - src alpha
// What can have written a colour target last (attachment stores, an upscaler, a blit)
constexpr VkAccessFlags kColorWrites =
    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

struct PushConstants {
    float matrix[16];
    float color[4];
    int32_t texMode;
    int32_t alphaTex;
    float alphaRef;
    float reactive;
};
static_assert(sizeof(PushConstants) == 96, "push constant layout");

// The scene variants' (common.glsl with SCENE)
struct ScenePushConstants {
    PushConstants base;
    float prevMatrix[16];
    float motion[4]; // jitter in clip units, NDC -> UV
};
static_assert(sizeof(ScenePushConstants) == 176, "scene push constant layout");

// Last frame's matrix (and re-skinned vertices) for one draw: draw n of mesh upload g
struct DrawHistory {
    float matrix[16];
    std::vector<uint8_t> vertices;
};

// First-fit free list over [0, size)
class RangeAllocator {
public:
    void init(VkDeviceSize size) { free_ = {{0, size}}; }
    bool alloc(VkDeviceSize size, VkDeviceSize align, VkDeviceSize& out) {
        for (size_t i = 0; i < free_.size(); ++i) {
            const VkDeviceSize start = (free_[i].first + align - 1) / align * align;
            const VkDeviceSize end = free_[i].first + free_[i].second;
            if (start + size > end) continue;
            const VkDeviceSize before = start - free_[i].first, after = end - (start + size);
            const VkDeviceSize first = free_[i].first;
            free_.erase(free_.begin() + long(i));
            if (after) free_.insert(free_.begin() + long(i), {start + size, after});
            if (before) free_.insert(free_.begin() + long(i), {first, before});
            out = start;
            return true;
        }
        return false;
    }
    void release(VkDeviceSize off, VkDeviceSize size) {
        auto it = std::lower_bound(free_.begin(), free_.end(), std::make_pair(off, VkDeviceSize(0)));
        it = free_.insert(it, {off, size});
        // merge with the neighbours
        if (it + 1 != free_.end() && it->first + it->second == (it + 1)->first) {
            it->second += (it + 1)->second;
            free_.erase(it + 1);
        }
        if (it != free_.begin() && (it - 1)->first + (it - 1)->second == it->first) {
            (it - 1)->second += it->second;
            free_.erase(it);
        }
    }

private:
    std::vector<std::pair<VkDeviceSize, VkDeviceSize>> free_; // (offset, size), sorted
};

struct Block {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE; // buffer arenas: one buffer over the whole block
    uint8_t* mapped = nullptr;
    uint32_t type = 0;
    VkDeviceSize size = 0;
    RangeAllocator ranges;
};

struct Alloc {
    int block = -1;
    VkDeviceSize offset = 0, size = 0;
};

struct TextureRec {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    Alloc mem;
    std::array<VkDescriptorSet, 32> sets{}; // per sampler (16+: the mipmapped variants)
    uint32_t mips = 1;
    bool live = false;
};

struct MeshRec {
    Alloc vertices, indices;
    uint32_t indexCount = 0;
    std::vector<uint8_t> dynamic; // the last updateVertices data (re-sent in later frames)
    uint64_t dynSerial = 0;       // frame whose ring holds it
    VkBuffer dynBuffer = VK_NULL_HANDLE;
    VkDeviceSize dynOffset = 0;
    uint32_t generation = 0;      // tells uploads that reuse a slot apart (draw history)
    bool live = false;
};

struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBuffer cmd2 = VK_NULL_HANDLE; // (the frame after MetalFX's scaler: vk_metalfx.hpp)
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkBuffer ring = VK_NULL_HANDLE;
    VkDeviceMemory ringMemory = VK_NULL_HANDLE;
    uint8_t* ringMapped = nullptr;
    VkDeviceSize ringSize = 0, ringUsed = 0;
};

class VkDevice_ final : public RenderDevice {
public:
    uint64_t windowFlags() const override { return SDL_WINDOW_VULKAN; }

    bool init(SDL_Window* window) override {
        window_ = window;
        return createInstance() && pickDevice() && createDevice() && createPipelines() && createSamplers() &&
               createFrames() && createWhite();
    }

    void shutdown() override {
        if (!device_) {
            destroyInstance();
            return;
        }
        vkDeviceWaitIdle(device_);
        for (auto& d : deletions_) d.second();
        deletions_.clear();
        destroyTarget();
        destroySwapchain();
        for (Frame& f : frames_) {
            vkDestroyFence(device_, f.fence, nullptr);
            vkDestroySemaphore(device_, f.acquired, nullptr);
            if (f.ring) vkDestroyBuffer(device_, f.ring, nullptr);
            if (f.ringMemory) vkFreeMemory(device_, f.ringMemory, nullptr);
        }
        for (VkSemaphore s : presentSems_) vkDestroySemaphore(device_, s, nullptr);
        presentSems_.clear();
        for (auto& t : textures_)
            if (t.live) freeTexture(t);
        if (readback_.buffer) {
            vkDestroyBuffer(device_, readback_.buffer, nullptr);
            vkFreeMemory(device_, readback_.memory, nullptr);
        }
        for (VkSampler s : samplers_) vkDestroySampler(device_, s, nullptr);
        destroyPipelines();
        destroySceneTarget();
        fsr_.shutdown();
        dlss_.shutdown();
#ifdef __APPLE__
        metalfx_.shutdown();
#endif
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (sceneLayout_) vkDestroyPipelineLayout(device_, sceneLayout_, nullptr);
        vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        vkDestroyDescriptorPool(device_, pool_, nullptr);
        for (VkShaderModule m : {quadVert_, quadFrag_, meshVert_, meshFrag_, sceneQuadVert_, sceneQuadFrag_, sceneMeshVert_,
                                 sceneMeshFrag_})
            if (m) vkDestroyShaderModule(device_, m, nullptr);
        vkDestroyFence(device_, uploadFence_, nullptr);
        vkDestroyCommandPool(device_, cmdPool_, nullptr);
        for (Block& b : blocks_) {
            if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
            vkFreeMemory(device_, b.memory, nullptr);
        }
        blocks_.clear();
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        destroyInstance();
    }

    std::string description() const override {
        return "Vulkan " + std::to_string(VK_API_VERSION_MAJOR(props_.apiVersion)) + "." +
               std::to_string(VK_API_VERSION_MINOR(props_.apiVersion)) + "." +
               std::to_string(VK_API_VERSION_PATCH(props_.apiVersion)) + " / " + props_.deviceName;
    }

    // ---- resources ----

    Texture upload(const Image& img) override {
        Texture t;
        t.width = img.width;
        t.height = img.height;
        t.alphaOnly = img.alphaOnly;
        if (img.width <= 0 || img.height <= 0 || img.rgba.size() < size_t(img.width) * img.height * 4) return t;
        size_t index = textures_.size();
        for (size_t i = 0; i < textures_.size(); ++i)
            if (!textures_[i].live) {
                index = i;
                break;
            }
        if (index == textures_.size()) textures_.emplace_back();
        TextureRec& rec = textures_[index];
        rec = TextureRec();
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = kColorFormat;
        ii.extent = {uint32_t(img.width), uint32_t(img.height), 1};
        rec.mips = img.mipmaps ? 1 + uint32_t(std::floor(std::log2(float(std::max(img.width, img.height))))) : 1;
        ii.mipLevels = rec.mips;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!ok(vkCreateImage(device_, &ii, nullptr, &rec.image))) return t;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, rec.image, &req);
        if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, rec.mem) ||
            !ok(vkBindImageMemory(device_, rec.image, blocks_[size_t(rec.mem.block)].memory, rec.mem.offset))) {
            vkDestroyImage(device_, rec.image, nullptr);
            return t;
        }
        rec.view = makeView(rec.image, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, rec.mips);
        // Staging copy
        const VkDeviceSize bytes = VkDeviceSize(img.width) * img.height * 4;
        Alloc staging;
        if (!allocateBuffer(bytes, 16, staging)) return t;
        std::memcpy(blocks_[size_t(staging.block)].mapped + staging.offset, img.rgba.data(), size_t(bytes));
        VkCommandBuffer cb = beginOneTime();
        barrier(cb, rec.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, rec.mips);
        VkBufferImageCopy region{};
        region.bufferOffset = staging.offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = ii.extent;
        vkCmdCopyBufferToImage(cb, blocks_[size_t(staging.block)].buffer, rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        // The mip chain: each level filtered down from the one above
        int mw = img.width, mh = img.height;
        for (uint32_t m = 1; m < rec.mips; ++m) {
            barrierMip(cb, rec.image, m - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
            blit.srcOffsets[1] = {mw, mh, 1};
            mw = std::max(mw / 2, 1);
            mh = std::max(mh / 2, 1);
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            blit.dstOffsets[1] = {mw, mh, 1};
            vkCmdBlitImage(cb, rec.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, VK_FILTER_LINEAR);
            barrierMip(cb, rec.image, m - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        barrierMip(cb, rec.image, rec.mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        endOneTime(cb);
        freeAlloc(staging);
        rec.live = true;
        t.id = unsigned(index + 1);
        return t;
    }

    void destroy(Texture& tex) override {
        if (!tex.id || tex.id > textures_.size()) return;
        TextureRec& rec = textures_[tex.id - 1];
        if (rec.live) {
            // Recorded draws hold the descriptor sets, not the slot: the slot is free at once, the
            // GPU objects once no frame in flight can use them
            TextureRec old = rec;
            later([this, old]() mutable { freeTexture(old); });
            rec = TextureRec();
        }
        tex = {};
    }

    GpuMesh upload(const std::vector<uint8_t>& vertices, const std::vector<uint32_t>& indices, int texture) override {
        GpuMesh g;
        g.texture = texture;
        g.indexCount = int(indices.size());
        size_t index = meshes_.size();
        for (size_t i = 0; i < meshes_.size(); ++i)
            if (!meshes_[i].live) {
                index = i;
                break;
            }
        if (index == meshes_.size()) meshes_.emplace_back();
        MeshRec& rec = meshes_[index];
        rec = MeshRec();
        if (!allocateBuffer(std::max<VkDeviceSize>(vertices.size(), 4), 16, rec.vertices) ||
            !allocateBuffer(std::max<VkDeviceSize>(indices.size() * 4, 4), 16, rec.indices))
            return g;
        if (!vertices.empty())
            std::memcpy(blocks_[size_t(rec.vertices.block)].mapped + rec.vertices.offset, vertices.data(), vertices.size());
        if (!indices.empty())
            std::memcpy(blocks_[size_t(rec.indices.block)].mapped + rec.indices.offset, indices.data(), indices.size() * 4);
        rec.indexCount = uint32_t(indices.size());
        rec.generation = ++meshGeneration_;
        rec.live = true;
        g.vao = unsigned(index + 1);
        return g;
    }

    void updateVertices(GpuMesh& mesh, const std::vector<uint8_t>& vertices) override {
        if (!mesh.vao || mesh.vao > meshes_.size()) return;
        MeshRec& rec = meshes_[mesh.vao - 1];
        rec.dynamic = vertices;
        rec.dynSerial = 0; // copied into the ring when next drawn
    }

    void destroy(GpuMesh& mesh) override {
        if (!mesh.vao || mesh.vao > meshes_.size()) return;
        MeshRec& rec = meshes_[mesh.vao - 1];
        if (rec.live) {
            const Alloc v = rec.vertices, i = rec.indices;
            later([this, v, i] {
                freeAlloc(v);
                freeAlloc(i);
            });
            rec = MeshRec();
        }
        mesh = {};
    }

    // ---- frames ----

    void beginFrame(int w, int h) override {
        w = std::max(w, 1);
        h = std::max(h, 1);
        ++serial_;
        Frame& f = frames_[serial_ % kFrames];
        vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &f.fence);
        applyFiltering();
        runDeletions(serial_ >= kFrames ? serial_ - kFrames : 0);
        f.ringUsed = 0;
        const uint64_t now = SDL_GetTicksNS();
        frameMs_ = lastFrameNs_ ? std::clamp(float(double(now - lastFrameNs_) * 1e-6), 0.0f, 1000.0f) : 16.7f;
        lastFrameNs_ = now;
        // The upscaler this frame: the one asked for, where it can run (MSAA is off meanwhile)
        const Upscaler was = active_;
        active_ = upscalerProblem(upscale_.mode).empty() ? upscale_.mode : Upscaler::Off;
        if (active_ != was) historyReset_ = true;
        const int samples = active_ != Upscaler::Off ? 1 : wantSamples_;
        if (samples != samples_) {
            vkDeviceWaitIdle(device_);
            samples_ = samples;
            destroyPipelines();
            createQuadPipelines();
            destroyTarget();
        }
        if (w != targetW_ || h != targetH_) {
            vkDeviceWaitIdle(device_);
            destroyTarget();
            createTarget(w, h);
        }
        int sw = w, sh = h;
        sceneSize(w, h, sw, sh);
        if (active_ != Upscaler::Off && (sw != sceneW_ || sh != sceneH_)) {
            vkDeviceWaitIdle(device_);
            destroySceneTarget();
            createSceneTarget(sw, sh);
            historyReset_ = true;
        }
        if (active_ == Upscaler::Fsr3 && fsr_.needsResize(sw, sh, w, h)) {
            vkDeviceWaitIdle(device_);
            if (!fsr_.resize(sw, sh, w, h)) {
                upscale_.mode = active_ = Upscaler::Off; // out of memory: back to plain frames
                ++errors_;
            }
        }
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(f.cmd, 0);
        vkBeginCommandBuffer(f.cmd, &bi);
        cmd_ = f.cmd;
        if (active_ != Upscaler::Off) {
            // Sub-pixel jitter: Halton (2, 3) over the upscaler's phase count
            const int phases = Fsr3::jitterPhaseCount(sw, w);
            Fsr3::jitter(int(jitterIndex_++ % uint32_t(std::max(phases, 1))), phases, jitter_[0], jitter_[1]);
            beginScenePass();
        } else {
            jitter_[0] = jitter_[1] = 0;
            beginFramePass(true);
        }
    }

    // The frame's colour + depth (cleared, or the colour kept: the UI over the upscaled scene)
    void beginFramePass(bool clear) {
        // After everything earlier frames did with these: their reads (the blit, readback, the
        // upscaler) and their writes (attachment stores, the upscaler's output, the stretch blit) -
        // a layout change is a write, so it is ordered after those too (sync validation)
        const VkPipelineStageFlags before = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        const VkAccessFlags colorWrites = kColorWrites;
        if (clear) {
            barrier(cmd_, colorImage_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorWrites, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, before,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        if (msColorImage_)
            barrier(cmd_, msColorImage_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorWrites, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, before,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        barrier(cmd_, depthImage_, depthAspect_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, before,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = msColorImage_ ? msColorView_ : colorView_;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = msColorImage_ ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        if (msColorImage_) { // resolved into the single-sampled frame when rendering ends
            color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            color.resolveImageView = colorView_;
            color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        color.clearValue.color = {{0, 0, 0, 1}};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depthView_;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {uint32_t(targetW_), uint32_t(targetH_)}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd_, &ri);
        startPass(targetW_, targetH_);
        inScene_ = false;
    }

    // The scene at render size: colour, motion vectors, reactive mask, depth (all cleared)
    void beginScenePass() {
        const VkPipelineStageFlags before = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        for (VkImage img : {sceneColor_, sceneMotion_, sceneReactive_})
            barrier(cmd_, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    kColorWrites, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, before, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        barrier(cmd_, sceneDepth_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, before, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        VkRenderingAttachmentInfo colors[3] = {};
        const VkImageView views[3] = {sceneColorView_, sceneMotionView_, sceneReactiveView_};
        for (int i = 0; i < 3; ++i) {
            colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colors[i].imageView = views[i];
            colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colors[i].clearValue.color = {{0, 0, 0, i == 0 ? 1.0f : 0.0f}};
        }
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = sceneDepthView_;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {uint32_t(sceneW_), uint32_t(sceneH_)}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 3;
        ri.pColorAttachments = colors;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd_, &ri);
        inScene_ = true;
        startPass(sceneW_, sceneH_);
    }

    void startPass(int w, int h) {
        passW_ = w;
        passH_ = h;
        bound_ = VK_NULL_HANDLE;
        boundSet_ = VK_NULL_HANDLE;
        boundLayout_ = VK_NULL_HANDLE;
        setViewport(0, 0, float(w), float(h));
        const VkRect2D scissor{{0, 0}, {uint32_t(w), uint32_t(h)}};
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        inFrame_ = true;
    }

    void sceneSize(int w, int h, int& sw, int& sh) const override {
        sw = w;
        sh = h;
        if (active_ == Upscaler::Off) return;
        const float r = std::clamp(upscale_.ratio, 1.0f, 3.0f);
        sw = std::max(int(float(w) / r), 1);
        sh = std::max(int(float(h) / r), 1);
    }

    // Scene -> upscaler -> the frame, which the UI then draws over
    void endScene() override {
        if (!inScene_ || !cmd_) return;
        vkCmdEndRendering(cmd_);
        inScene_ = false;
        for (VkImage img : {sceneColor_, sceneMotion_, sceneReactive_})
            barrier(cmd_, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cmd_, sceneDepth_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cmd_, colorImage_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kColorWrites,
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        bool done = false;
        if (active_ == Upscaler::Fsr3) {
            Fsr3Inputs in;
            in.color = sceneColorView_;
            in.depth = sceneDepthView_;
            in.motion = sceneMotionView_;
            in.reactive = sceneReactiveView_;
            in.output = colorView_;
            in.renderW = sceneW_;
            in.renderH = sceneH_;
            in.outW = targetW_;
            in.outH = targetH_;
            in.jitterX = jitter_[0];
            in.jitterY = jitter_[1];
            in.deltaMs = frameMs_;
            in.cameraNear = cameraNear_;
            in.cameraFar = cameraFar_;
            in.fovY = cameraFovY_;
            in.sharpness = std::clamp(upscale_.sharpness, 0.0f, 1.0f);
            in.reset = historyReset_;
            fsr_.dispatch(cmd_, int(serial_ % kFrames), in);
            done = true;
        } else if (active_ == Upscaler::MetalFx) {
#ifdef __APPLE__
            done = metalFxEncode();
#endif
        } else if (active_ == Upscaler::Dlss) {
            DlssInputs in;
            in.color = {sceneColor_, sceneColorView_, kColorFormat};
            in.depth = {sceneDepth_, sceneDepthView_, VK_FORMAT_D32_SFLOAT};
            in.motion = {sceneMotion_, sceneMotionView_, kMotionFormat};
            in.output = {colorImage_, colorView_, kColorFormat};
            in.renderW = sceneW_;
            in.renderH = sceneH_;
            in.outW = targetW_;
            in.outH = targetH_;
            in.jitterX = jitter_[0];
            in.jitterY = jitter_[1];
            in.deltaMs = frameMs_;
            in.sharpness = std::clamp(upscale_.sharpness, 0.0f, 1.0f);
            in.reset = historyReset_;
            done = dlss_.evaluate(cmd_, in);
        }
        historyReset_ = false;
        if (!done) { // no upscaler after all: the scene stretched over the frame
            barrier(cmd_, sceneColor_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            barrier(cmd_, colorImage_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {sceneW_, sceneH_, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {targetW_, targetH_, 1};
            vkCmdBlitImage(cmd_, sceneColor_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colorImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            barrier(cmd_, colorImage_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        } else {
            barrier(cmd_, colorImage_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        // This frame's draws are next frame's history
        history_.swap(historyNext_);
        historyNext_.clear();
        drawCounts_.clear();
        beginFramePass(false);
    }

    std::string upscalerProblem(Upscaler m) const override {
        if (m == Upscaler::Off) return "";
        if (!upscaleCaps_) return "this GPU lacks what temporal upscaling needs";
        if (m == Upscaler::Fsr3) return fsrSupported_ ? "" : "this GPU can't run FSR 3.1";
        if (m == Upscaler::MetalFx) {
#ifdef __APPLE__
            return metalObjects_ ? metalfx_.problem() : "needs MoltenVK's VK_EXT_metal_objects";
#else
            return "MetalFX is Apple's: macOS only";
#endif
        }
        return dlss_.problem();
    }

    void setUpscaler(const UpscaleSettings& s) override {
        if (s.mode != upscale_.mode || s.ratio != upscale_.ratio) historyReset_ = true;
        upscale_ = s;
    }

    void setCamera(float zNear, float zFar, float fovY) override {
        cameraNear_ = zNear;
        cameraFar_ = zFar;
        cameraFovY_ = fovY;
    }

    void resetHistory() override {
        historyReset_ = true;
        history_.clear();
    }

    void endFrame(bool present) override {
        if (!inFrame_) return;
        endScene();
        inFrame_ = false;
        Frame& f = frames_[serial_ % kFrames];
        vkCmdEndRendering(cmd_);
        barrier(cmd_, colorImage_, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        uint32_t image = UINT32_MAX;
        if (present) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window_, &w, &h);
            if (!swapchain_ || swapchainDirty_ || w != int(swapExtent_.width) || h != int(swapExtent_.height))
                createSwapchain();
            if (swapchain_) {
                const VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
                if (r == VK_ERROR_OUT_OF_DATE_KHR) {
                    swapchainDirty_ = true;
                    image = UINT32_MAX;
                } else if (r < 0) {
                    ok(r);
                    image = UINT32_MAX;
                } else if (r == VK_SUBOPTIMAL_KHR) {
                    swapchainDirty_ = true;
                }
            }
        }
        if (image != UINT32_MAX) {
            VkImage dst = swapImages_[image];
            barrier(cmd_, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {targetW_, targetH_, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {int32_t(swapExtent_.width), int32_t(swapExtent_.height), 1};
            vkCmdBlitImage(cmd_, colorImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);
            barrier(cmd_, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        ok(vkEndCommandBuffer(cmd_));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_;
        if (image != UINT32_MAX) {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &f.acquired;
            si.pWaitDstStageMask = &waitStage;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &presentSems_[image];
        }
        ok(vkQueueSubmit(queue_, 1, &si, f.fence));
        if (image != UINT32_MAX) {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &presentSems_[image];
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain_;
            pi.pImageIndices = &image;
            const VkResult r = vkQueuePresentKHR(queue_, &pi);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;
            else ok(r);
        }
        cmd_ = VK_NULL_HANDLE;
        haveFrame_ = true;
    }

    void setViewport(float x, float y, float w, float h) override {
        if (!cmd_) return;
        if (!inScene_) { // whole pixels, as GL's
            x = float(int(x));
            y = float(int(y));
            w = float(int(w));
            h = float(int(h));
        }
        // Negative height: GL's y-up clip space lands the same way up
        const VkViewport vp{x, y + h, w, -h, 0.0f, 1.0f};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vpW_ = std::max(w, 1.0f);
        vpH_ = std::max(h, 1.0f);
    }

    void clearDepth() override {
        if (!cmd_) return;
        VkClearAttachment ca{};
        ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        ca.clearValue.depthStencil = {1.0f, 0};
        const VkClearRect rect{{{0, 0}, {uint32_t(passW_), uint32_t(passH_)}}, 0, 1};
        vkCmdClearAttachments(cmd_, 1, &ca, 1, &rect);
    }

    void setColorWrite(bool on) override { colorWrite_ = on; }

    void drawQuad(const Texture& tex, const float* v, const float* proj, const float* rgba) override {
        if (!cmd_ || !tex.id || tex.id > textures_.size() || !textures_[tex.id - 1].image) return;
        VkBuffer buf;
        VkDeviceSize off;
        if (!ringWrite(v, sizeof(float) * 24, buf, off)) return;
        bind(inScene_ ? sceneQuadPipeline_ : quadPipeline_);
        vkCmdSetCullMode(cmd_, VK_CULL_MODE_NONE);
        vkCmdSetFrontFace(cmd_, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdSetDepthTestEnable(cmd_, VK_FALSE);
        vkCmdSetDepthWriteEnable(cmd_, VK_FALSE);
        vkCmdSetDepthCompareOp(cmd_, VK_COMPARE_OP_LESS_OR_EQUAL);
        ScenePushConstants pc{};
        toVulkanClip(proj, pc.base.matrix);
        std::memcpy(pc.base.color, rgba, sizeof pc.base.color);
        pushConstants(pc);
        bindTexture(tex.id - 1, 15); // clamp, linear
        vkCmdBindVertexBuffers(cmd_, 0, 1, &buf, &off);
        vkCmdDraw(cmd_, 6, 1, 0, 0);
    }

    void drawMesh(const GpuMesh& mesh, const Texture* tex, const float* mvp, const MeshShading& sh,
                  const DrawState& st) override {
        if (!cmd_ || !mesh.vao || mesh.vao > meshes_.size()) return;
        MeshRec& rec = meshes_[mesh.vao - 1];
        if (!rec.live || !rec.indexCount) return;
        VkBuffer vb = blocks_[size_t(rec.vertices.block)].buffer;
        VkDeviceSize voff = rec.vertices.offset;
        if (!rec.dynamic.empty()) {
            if (rec.dynSerial != serial_) { // this frame's copy of the latest vertices
                if (!ringWrite(rec.dynamic.data(), rec.dynamic.size(), rec.dynBuffer, rec.dynOffset)) return;
                rec.dynSerial = serial_;
            }
            vb = rec.dynBuffer;
            voff = rec.dynOffset;
        }
        uint32_t key = colorWrite_ ? 1u : 0u;
        if (st.blend) key |= 2u | uint32_t(std::min<uint8_t>(st.blendSrc, 8)) << 4 | uint32_t(std::min<uint8_t>(st.blendDst, 7)) << 8;
        if (inScene_) key |= kScenePipeline | (st.depthWrite ? kSceneDepthWrite : 0u);
        bind(meshPipeline(key));
        vkCmdSetCullMode(cmd_, st.cull == 0 ? VK_CULL_MODE_NONE : st.cull == 1 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT);
        vkCmdSetFrontFace(cmd_, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdSetDepthTestEnable(cmd_, st.depthTest ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthWriteEnable(cmd_, st.depthWrite ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthCompareOp(cmd_, VK_COMPARE_OP_LESS_OR_EQUAL);
        ScenePushConstants pc{};
        toVulkanClip(mvp, pc.base.matrix);
        std::memcpy(pc.base.color, sh.color, sizeof pc.base.color);
        pc.base.texMode = sh.texMode;
        pc.base.alphaTex = sh.alphaTex ? 1 : 0;
        pc.base.alphaRef = sh.alphaRef;
        // Reactive mask: alpha-blended layers by their alpha, additive ones by brightness
        if (st.blend && !st.depthWrite) pc.base.reactive = st.blendDst == 1 ? -0.8f : 0.8f;
        VkBuffer prevBuf = vb;
        VkDeviceSize prevOff = voff;
        if (inScene_) {
            // Draw n of this mesh: where it was last frame (its matrix, and its vertices when
            // re-skinned), else where it is now (no motion)
            const uint64_t id = uint64_t(rec.generation) << 20 | drawCounts_[rec.generation]++;
            DrawHistory& next = historyNext_[id];
            std::memcpy(next.matrix, pc.base.matrix, sizeof next.matrix);
            next.vertices = rec.dynamic;
            const auto it = history_.find(id);
            if (it != history_.end()) {
                std::memcpy(pc.prevMatrix, it->second.matrix, sizeof pc.prevMatrix);
                if (!rec.dynamic.empty() && it->second.vertices.size() == rec.dynamic.size() &&
                    !ringWrite(it->second.vertices.data(), it->second.vertices.size(), prevBuf, prevOff)) {
                    prevBuf = vb;
                    prevOff = voff;
                }
            } else {
                std::memcpy(pc.prevMatrix, pc.base.matrix, sizeof pc.prevMatrix);
            }
        }
        pushConstants(pc);
        const bool textured = tex && tex->id && tex->id <= textures_.size() && textures_[tex->id - 1].image;
        const unsigned samplerBits = (st.clampS ? 1u : 0u) | (st.clampT ? 2u : 0u) | (st.minLinear ? 4u : 0u) |
                                     (st.magLinear ? 8u : 0u);
        bindTexture(textured ? tex->id - 1 : white_.id - 1, textured ? samplerBits : 15);
        const VkBuffer bufs[2] = {vb, prevBuf};
        const VkDeviceSize offs[2] = {voff, prevOff};
        vkCmdBindVertexBuffers(cmd_, 0, inScene_ ? 2 : 1, bufs, offs);
        vkCmdBindIndexBuffer(cmd_, blocks_[size_t(rec.indices.block)].buffer, rec.indices.offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_, rec.indexCount, 1, 0, 0, 0);
    }

    // The scene's extras (jitter, motion scale) filled in here; outside it only the first 96 bytes
    void pushConstants(ScenePushConstants& pc) {
        if (inScene_) {
            pc.motion[0] = 2.0f * jitter_[0] / vpW_;
            pc.motion[1] = -2.0f * jitter_[1] / vpH_;
            pc.motion[2] = 0.5f * vpW_ / float(sceneW_);
            pc.motion[3] = -0.5f * vpH_ / float(sceneH_);
            vkCmdPushConstants(cmd_, sceneLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        } else {
            vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc.base,
                               &pc.base);
        }
    }

    bool readPixels(std::vector<uint8_t>& rgba, int& w, int& h) override {
        if (!haveFrame_) return false;
        vkQueueWaitIdle(queue_);
        w = targetW_;
        h = targetH_;
        const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
        if (readback_.size < bytes) {
            if (readback_.buffer) {
                vkDestroyBuffer(device_, readback_.buffer, nullptr);
                vkFreeMemory(device_, readback_.memory, nullptr);
            }
            readback_ = {};
            if (!createHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback_.buffer, readback_.memory,
                                  readback_.mapped))
                return false;
            readback_.size = bytes;
        }
        VkCommandBuffer cb = beginOneTime();
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {uint32_t(w), uint32_t(h), 1};
        vkCmdCopyImageToBuffer(cb, colorImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
        endOneTime(cb);
        rgba.assign(readback_.mapped, readback_.mapped + bytes);
        return true;
    }

    int takeErrors() override {
        const int n = errors_;
        errors_ = 0;
        return n;
    }

    void setVSync(bool on) override {
        if (vsync_ != on) swapchainDirty_ = true;
        vsync_ = on;
    }

    bool offscreen() const override { return true; }

    int maxSamples() const override {
        const VkSampleCountFlags ok = props_.limits.framebufferColorSampleCounts & props_.limits.framebufferDepthSampleCounts;
        for (int n : {8, 4, 2})
            if (ok & VkSampleCountFlags(n)) return n;
        return 1;
    }

    // MSAA: the frame renders into a multisampled target resolved at the end (taken up at the
    // next frame: pipelines and target are rebuilt)
    void setSamples(int n) override {
        int want = 1;
        for (int k : {2, 4, 8})
            if (n >= k && k <= maxSamples()) want = k;
        wantSamples_ = want;
    }

private:
    bool ok(VkResult r) {
        if (r >= 0) return true;
        ++errors_;
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Vulkan call failed: %d", int(r));
        return false;
    }

    // GL clip space (depth -1..1) -> Vulkan's (0..1): z' = (z + w) / 2
    static void toVulkanClip(const float* m, float* out) {
        for (int c = 0; c < 4; ++c) {
            out[c * 4 + 0] = m[c * 4 + 0];
            out[c * 4 + 1] = m[c * 4 + 1];
            out[c * 4 + 2] = 0.5f * (m[c * 4 + 2] + m[c * 4 + 3]);
            out[c * 4 + 3] = m[c * 4 + 3];
        }
    }

    // ---- setup ----

    bool createInstance() {
        Uint32 count = 0;
        const char* const* sdlExt = SDL_Vulkan_GetInstanceExtensions(&count);
        if (!sdlExt) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Vulkan: %s", SDL_GetError());
            return false;
        }
        std::vector<const char*> ext(sdlExt, sdlExt + count);
        Dlss::instanceExtensions(ext); // NVIDIA NGX's, when its runtime is present
        // a portability driver (MoltenVK on macOS) lists its devices only when asked to
        bool portability = false;
        {
            uint32_t n = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> have(n);
            vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
            for (const VkExtensionProperties& e : have)
                if (std::strcmp(e.extensionName, "VK_KHR_portability_enumeration") == 0) portability = true;
            if (portability) ext.push_back("VK_KHR_portability_enumeration");
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Chaos Rings";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = uint32_t(ext.size());
        ci.ppEnabledExtensionNames = ext.data();
        if (portability) ci.flags |= 0x00000001; // (VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR)
#ifdef __APPLE__
        // (MoltenVK: each vkQueueSubmit committed to its Metal queue before it returns - the order
        // MetalFX's scaler, committed between two, relies on)
        setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "1", 1);
#endif
        if (!ok(vkCreateInstance(&ci, nullptr, &instance_))) return false;
        if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Vulkan surface: %s", SDL_GetError());
            return false;
        }
        return true;
    }

    void destroyInstance() {
        if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        if (instance_) vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    // A Vulkan 1.3 GPU with a queue that draws and presents; discrete GPUs first
    bool pickDevice() {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance_, &n, devs.data());
        int best = -1;
        for (VkPhysicalDevice d : devs) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(d, &p);
            if (p.apiVersion < VK_API_VERSION_1_3) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
            for (uint32_t q = 0; q < qn; ++q) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(d, q, surface_, &present);
                if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
                const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
                if (score > best) {
                    best = score;
                    gpu_ = d;
                    family_ = q;
                    props_ = p;
                }
                break;
            }
        }
        if (!gpu_) SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Vulkan: no GPU with Vulkan 1.3 that can draw to this window");
        return gpu_ != VK_NULL_HANDLE;
    }

    bool createDevice() {
        vkGetPhysicalDeviceMemoryProperties(gpu_, &memProps_);
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family_;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.pNext = &f13;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f12;
        std::vector<const char*> ext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        { // (a portability device - MoltenVK's - must have its subset enabled)
            uint32_t n = 0;
            vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> have(n);
            vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &n, have.data());
            for (const VkExtensionProperties& e : have)
                if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) ext.push_back("VK_KHR_portability_subset");
        }
#ifdef __APPLE__
        { // (MetalFX: the Metal objects under MoltenVK's)
            const size_t before = ext.size();
            MetalFx::deviceExtensions(gpu_, ext);
            metalObjects_ = ext.size() > before;
        }
#endif
        // Temporal upscaling: a scene drawn into colour + motion + reactive attachments with
        // their own blend states, depth sampled afterwards
        VkPhysicalDeviceFeatures have;
        vkGetPhysicalDeviceFeatures(gpu_, &have);
        auto attachment = [&](VkFormat f, VkFormatFeatureFlags want) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(gpu_, f, &fp);
            return (fp.optimalTilingFeatures & want) == want;
        };
        upscaleCaps_ = have.independentBlend && props_.limits.maxPushConstantsSize >= sizeof(ScenePushConstants) &&
                       attachment(kMotionFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                       attachment(kReactiveFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                       attachment(kColorFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
                       attachment(VK_FORMAT_D32_SFLOAT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        f2.features.independentBlend = upscaleCaps_ ? VK_TRUE : VK_FALSE;
        anisotropy_ = have.samplerAnisotropy ? std::min(16.0f, props_.limits.maxSamplerAnisotropy) : 1.0f;
        f2.features.samplerAnisotropy = anisotropy_ > 1.0f ? VK_TRUE : VK_FALSE;
        fsrSupported_ = upscaleCaps_ && Fsr3::supported(gpu_);
        if (fsrSupported_) {
            f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            f2.features.shaderStorageImageExtendedFormats = VK_TRUE;
            ext.push_back(Fsr3::extension());
        }
        if (upscaleCaps_) dlss_.deviceRequirements(instance_, gpu_, ext, f12); // NGX's, on NVIDIA RTX
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.pNext = &f2;
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos = &qi;
        ci.enabledExtensionCount = uint32_t(ext.size());
        ci.ppEnabledExtensionNames = ext.data();
        if (!ok(vkCreateDevice(gpu_, &ci, nullptr, &device_))) return false;
        if (fsrSupported_ && !fsr_.init(device_, gpu_, kFrames)) fsrSupported_ = false;
        if (upscaleCaps_) dlss_.init(instance_, gpu_, device_);
        vkGetDeviceQueue(device_, family_, 0, &queue_);
#ifdef __APPLE__
        if (upscaleCaps_ && metalObjects_) metalfx_.init(instance_, device_, queue_);
#endif
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = family_;
        if (!ok(vkCreateCommandPool(device_, &pi, nullptr, &cmdPool_))) return false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmdPool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!ok(vkAllocateCommandBuffers(device_, &ai, &uploadCmd_))) return false;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!ok(vkCreateFence(device_, &fi, nullptr, &uploadFence_))) return false;
        // Depth: D32 where it can be an attachment, else D24S8
        for (VkFormat fmt : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(gpu_, fmt, &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                depthFormat_ = fmt;
                depthAspect_ = fmt == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                          : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                break;
            }
        }
        // Descriptors: one combined image sampler per set, cached per (texture, sampler)
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        if (!ok(vkCreateDescriptorSetLayout(device_, &li, nullptr, &setLayout_))) return false;
        const VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpi.maxSets = 32768;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        if (!ok(vkCreateDescriptorPool(device_, &dpi, nullptr, &pool_))) return false;
        const VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        if (!ok(vkCreatePipelineLayout(device_, &pli, nullptr, &layout_))) return false;
        if (upscaleCaps_) { // the scene variants' bigger push constants
            const VkPushConstantRange spcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                           sizeof(ScenePushConstants)};
            pli.pPushConstantRanges = &spcr;
            if (!ok(vkCreatePipelineLayout(device_, &pli, nullptr, &sceneLayout_))) upscaleCaps_ = fsrSupported_ = false;
        }
        return true;
    }

    VkShaderModule module(const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        ok(vkCreateShaderModule(device_, &ci, nullptr, &m));
        return m;
    }

    bool createPipelines() {
        quadVert_ = module(kQuadVert, sizeof kQuadVert);
        quadFrag_ = module(kQuadFrag, sizeof kQuadFrag);
        meshVert_ = module(kMeshVert, sizeof kMeshVert);
        meshFrag_ = module(kMeshFrag, sizeof kMeshFrag);
        if (!quadVert_ || !quadFrag_ || !meshVert_ || !meshFrag_) return false;
        if (upscaleCaps_) {
            sceneQuadVert_ = module(kSceneQuadVert, sizeof kSceneQuadVert);
            sceneQuadFrag_ = module(kSceneQuadFrag, sizeof kSceneQuadFrag);
            sceneMeshVert_ = module(kSceneMeshVert, sizeof kSceneMeshVert);
            sceneMeshFrag_ = module(kSceneMeshFrag, sizeof kSceneMeshFrag);
            if (!sceneQuadVert_ || !sceneQuadFrag_ || !sceneMeshVert_ || !sceneMeshFrag_) upscaleCaps_ = fsrSupported_ = false;
        }
        return createQuadPipelines();
    }

    // 2D quads: alpha blending (GL's BlendFunc applies to alpha too)
    bool createQuadPipelines() {
        quadPipeline_ = makePipeline(true, kQuadKey);
        if (upscaleCaps_) sceneQuadPipeline_ = makePipeline(true, kQuadKey | kScenePipeline);
        return quadPipeline_ != VK_NULL_HANDLE;
    }

    void destroyPipelines() {
        for (auto& [k, p] : pipelines_) vkDestroyPipeline(device_, p, nullptr);
        pipelines_.clear();
        if (quadPipeline_) vkDestroyPipeline(device_, quadPipeline_, nullptr);
        if (sceneQuadPipeline_) vkDestroyPipeline(device_, sceneQuadPipeline_, nullptr);
        quadPipeline_ = sceneQuadPipeline_ = VK_NULL_HANDLE;
    }

    // key: bit 0 colour writes, bit 1 blending, bits 4-7 source factor, bits 8-11 destination,
    // kScenePipeline: the scene variant (+ motion vector and reactive attachments)
    VkPipeline makePipeline(bool quad, uint32_t key) {
        const bool scene = key & kScenePipeline;
        static const VkBlendFactor kSrc[9] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_DST_COLOR,
                                              VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
                                              VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
                                              VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
        static const VkBlendFactor kDst[8] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
                                              VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
                                              VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
                                              VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (int i = 0; i < 2; ++i) {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = scene ? (quad ? sceneQuadVert_ : sceneMeshVert_) : quad ? quadVert_ : meshVert_;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = scene ? (quad ? sceneQuadFrag_ : sceneMeshFrag_) : quad ? quadFrag_ : meshFrag_;
        // Scene meshes: binding 1 = last frame's vertices (positions)
        const VkVertexInputBindingDescription bindings[2] = {{0, quad ? 16u : 24u, VK_VERTEX_INPUT_RATE_VERTEX},
                                                             {1, 24u, VK_VERTEX_INPUT_RATE_VERTEX}};
        const VkVertexInputAttributeDescription quadAttrs[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                                                {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}};
        const VkVertexInputAttributeDescription meshAttrs[4] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                                {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
                                                                {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 20},
                                                                {3, 1, VK_FORMAT_R32G32B32_SFLOAT, 0}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = scene && !quad ? 2 : 1;
        vi.pVertexBindingDescriptions = bindings;
        vi.vertexAttributeDescriptionCount = quad ? 2 : scene ? 4 : 3;
        vi.pVertexAttributeDescriptions = quad ? quadAttrs : meshAttrs;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = scene ? VK_SAMPLE_COUNT_1_BIT : VkSampleCountFlagBits(samples_);
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = (key & 1) ? 0xF : 0;
        if (key & 2) {
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = att.srcAlphaBlendFactor = kSrc[std::min<uint32_t>(key >> 4 & 15, 8)];
            att.dstColorBlendFactor = att.dstAlphaBlendFactor = kDst[std::min<uint32_t>(key >> 8 & 15, 7)];
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        // Scene: solid surfaces - opaque, or blended but writing depth (the game blends most
        // of its models) - write their motion; see-through ones (blended, no depth) the reactive
        // mask, the most reactive layer winning; 2D in the scene writes neither (both clear to 0)
        VkPipelineColorBlendAttachmentState atts[3] = {att, {}, {}};
        if (scene && !quad && (key & 1)) {
            if ((key & 2) && !(key & kSceneDepthWrite)) {
                atts[2].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
                atts[2].blendEnable = VK_TRUE;
                atts[2].srcColorBlendFactor = atts[2].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                atts[2].srcAlphaBlendFactor = atts[2].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                atts[2].colorBlendOp = atts[2].alphaBlendOp = VK_BLEND_OP_MAX;
            } else {
                atts[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
            }
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = scene ? 3 : 1;
        cb.pAttachments = atts;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
                                      VK_DYNAMIC_STATE_CULL_MODE,         VK_DYNAMIC_STATE_FRONT_FACE,
                                      VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                      VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = uint32_t(sizeof dyn / sizeof dyn[0]);
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        const VkFormat colorFormats[3] = {kColorFormat, kMotionFormat, kReactiveFormat};
        rci.colorAttachmentCount = scene ? 3 : 1;
        rci.pColorAttachmentFormats = colorFormats;
        rci.depthAttachmentFormat = depthFormat_;
        if (depthAspect_ & VK_IMAGE_ASPECT_STENCIL_BIT) rci.stencilAttachmentFormat = depthFormat_;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.pNext = &rci;
        pi.stageCount = 2;
        pi.pStages = stages;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dy;
        pi.layout = scene ? sceneLayout_ : layout_;
        VkPipeline p = VK_NULL_HANDLE;
        ok(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &p));
        return p;
    }

    VkPipeline meshPipeline(uint32_t key) {
        auto it = pipelines_.find(key);
        if (it != pipelines_.end()) return it->second;
        const VkPipeline p = makePipeline(false, key);
        pipelines_[key] = p;
        return p;
    }

    void bind(VkPipeline p) {
        if (p == bound_ || !p) return;
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        bound_ = p;
        const VkPipelineLayout layout = inScene_ ? sceneLayout_ : layout_;
        if (layout != boundLayout_) { // sets bound through the other layout don't carry over
            boundLayout_ = layout;
            boundSet_ = VK_NULL_HANDLE;
        }
    }

    void setFiltering(const TextureFiltering& f) override { filteringWanted_ = f; }
    int maxAnisotropy() const override { return int(anisotropy_); }
    // New samplers for new filtering - between frames (beginFrame: every set this frame binds is
    // rewritten), once the GPU is done with the old ones
    void applyFiltering() {
        if (filteringWanted_ == filtering_ || !device_) return;
        vkDeviceWaitIdle(device_);
        for (VkSampler& smp : samplers_) vkDestroySampler(device_, smp, nullptr), smp = VK_NULL_HANDLE;
        filtering_ = filteringWanted_;
        if (!createSamplers()) return;
        for (TextureRec& t : textures_)
            for (size_t bits = 0; bits < t.sets.size(); ++bits) {
                if (!t.sets[bits] || !t.view) continue;
                VkDescriptorImageInfo ii{samplers_[bits], t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = t.sets[bits];
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &ii;
                vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            }
    }

    // Samplers by bits: 1 clamp S, 2 clamp T, 4 linear min, 8 linear mag, 16 blend mips (HD)
    bool createSamplers() {
        for (unsigned bits = 0; bits < 32; ++bits) {
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = (bits & 8) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.minFilter = (bits & 4) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.mipmapMode = (bits & 16) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = (bits & 1) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
            si.addressModeV = (bits & 2) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
            si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.maxLod = (bits & 16) ? VK_LOD_CLAMP_NONE : 0.0f;
            if (bits & 16) { // (the mipmapped textures: the options' filtering)
                const float aniso = std::min(float(filtering_.anisotropy), anisotropy_);
                if (aniso > 1.0f) si.anisotropyEnable = VK_TRUE, si.maxAnisotropy = aniso; // (sharp at an angle)
                if (filtering_.sharp) si.magFilter = VK_FILTER_NEAREST;
                si.mipLodBias = filtering_.lodBias;
            }
            if (!ok(vkCreateSampler(device_, &si, nullptr, &samplers_[bits]))) return false;
        }
        return true;
    }

    void bindTexture(unsigned index, unsigned samplerBits) {
        if (index >= textures_.size()) return;
        TextureRec& t = textures_[index];
        if (!t.image) return;
        samplerBits = (samplerBits & 15) | (t.mips > 1 ? 16u : 0u);
        VkDescriptorSet& set = t.sets[samplerBits];
        if (!set) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = pool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &setLayout_;
            if (!ok(vkAllocateDescriptorSets(device_, &ai, &set))) return;
            VkDescriptorImageInfo ii{samplers_[samplerBits], t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &ii;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
        if (set != boundSet_) {
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, boundLayout_, 0, 1, &set, 0, nullptr);
            boundSet_ = set;
        }
    }

    bool createFrames() {
        for (Frame& f : frames_) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = cmdPool_;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (!ok(vkAllocateCommandBuffers(device_, &ai, &f.cmd))) return false;
            if (!ok(vkAllocateCommandBuffers(device_, &ai, &f.cmd2))) return false;
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (!ok(vkCreateFence(device_, &fi, nullptr, &f.fence))) return false;
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (!ok(vkCreateSemaphore(device_, &si, nullptr, &f.acquired))) return false;
            if (!createHostBuffer(kRingSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, f.ring, f.ringMemory, f.ringMapped))
                return false;
            f.ringSize = kRingSize;
        }
        return true;
    }

    bool createWhite() {
        Image img;
        img.width = img.height = 1;
        img.rgba = {255, 255, 255, 255};
        white_ = upload(img);
        return white_.id != 0;
    }

    // Per-frame dynamic data (quad corners, re-skinned vertices); the ring doubles when full
    bool ringWrite(const void* data, size_t bytes, VkBuffer& buf, VkDeviceSize& off) {
        Frame& f = frames_[serial_ % kFrames];
        const VkDeviceSize start = (f.ringUsed + 15) & ~VkDeviceSize(15);
        if (start + bytes > f.ringSize) {
            VkDeviceSize size = f.ringSize;
            while (size < start + bytes) size *= 2;
            const VkBuffer oldBuf = f.ring;
            const VkDeviceMemory oldMem = f.ringMemory;
            later([this, oldBuf, oldMem] {
                vkDestroyBuffer(device_, oldBuf, nullptr);
                vkFreeMemory(device_, oldMem, nullptr);
            });
            // The draws already recorded this frame keep the old buffer (freed after the frame)
            if (!createHostBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, f.ring, f.ringMemory, f.ringMapped)) return false;
            f.ringSize = size;
            f.ringUsed = 0;
            return ringWrite(data, bytes, buf, off);
        }
        std::memcpy(f.ringMapped + start, data, bytes);
        f.ringUsed = start + bytes;
        buf = f.ring;
        off = start;
        return true;
    }

    // ---- memory ----

    int findType(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
            if ((bits >> i & 1) && (memProps_.memoryTypes[i].propertyFlags & want) == want) return int(i);
        return -1;
    }

    bool createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, uint8_t*& mapped) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        if (!ok(vkCreateBuffer(device_, &bi, nullptr, &buf))) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, buf, &req);
        const int type = findType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = uint32_t(type);
        if (type < 0 || !ok(vkAllocateMemory(device_, &ai, nullptr, &mem))) return false;
        vkBindBufferMemory(device_, buf, mem, 0);
        void* p = nullptr;
        if (!ok(vkMapMemory(device_, mem, 0, VK_WHOLE_SIZE, 0, &p))) return false;
        mapped = static_cast<uint8_t*>(p);
        return true;
    }

    // Host-visible buffer space (vertices, indices, staging), from 64 MB arenas
    bool allocateBuffer(VkDeviceSize size, VkDeviceSize align, Alloc& out) {
        for (size_t i = 0; i < blocks_.size(); ++i)
            if (blocks_[i].buffer && blocks_[i].ranges.alloc(size, align, out.offset)) {
                out.block = int(i);
                out.size = size;
                return true;
            }
        Block b;
        b.size = std::max(kBlockSize, size);
        if (!createHostBuffer(b.size,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              b.buffer, b.memory, b.mapped))
            return false;
        b.ranges.init(b.size);
        blocks_.push_back(b);
        Block& nb = blocks_.back();
        if (!nb.ranges.alloc(size, align, out.offset)) return false;
        out.block = int(blocks_.size() - 1);
        out.size = size;
        return true;
    }

    // Device memory for images, from 64 MB blocks per memory type
    bool allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags props, bool, Alloc& out) {
        const int type = findType(req.memoryTypeBits, props);
        if (type < 0) return false;
        for (size_t i = 0; i < blocks_.size(); ++i)
            if (!blocks_[i].buffer && blocks_[i].type == uint32_t(type) &&
                blocks_[i].ranges.alloc(req.size, req.alignment, out.offset)) {
                out.block = int(i);
                out.size = req.size;
                return true;
            }
        Block b;
        b.type = uint32_t(type);
        b.size = std::max(kBlockSize, req.size);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = b.size;
        ai.memoryTypeIndex = b.type;
        if (!ok(vkAllocateMemory(device_, &ai, nullptr, &b.memory))) return false;
        b.ranges.init(b.size);
        blocks_.push_back(b);
        if (!blocks_.back().ranges.alloc(req.size, req.alignment, out.offset)) return false;
        out.block = int(blocks_.size() - 1);
        out.size = req.size;
        return true;
    }

    void freeAlloc(const Alloc& a) {
        if (a.block >= 0 && size_t(a.block) < blocks_.size()) blocks_[size_t(a.block)].ranges.release(a.offset, a.size);
    }

    void freeTexture(TextureRec& t) {
        for (VkDescriptorSet& s : t.sets)
            if (s) vkFreeDescriptorSets(device_, pool_, 1, &s);
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.image) vkDestroyImage(device_, t.image, nullptr);
        freeAlloc(t.mem);
        t = TextureRec();
    }

    void later(std::function<void()> f) { deletions_.push_back({serial_, std::move(f)}); }

    // Releases what frames up to `done` used (their fences have signalled)
    void runDeletions(uint64_t done) {
        std::vector<std::pair<uint64_t, std::function<void()>>> keep;
        for (auto& d : deletions_) {
            if (d.first <= done) d.second();
            else keep.push_back(std::move(d));
        }
        deletions_.swap(keep);
    }

    // ---- images, targets, swapchain ----

    VkImageView makeView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mips = 1) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, mips, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        ok(vkCreateImageView(device_, &vi, nullptr, &v));
        return v;
    }

    static void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                        VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                        VkPipelineStageFlags dstStage, uint32_t mips = 1) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {aspect, 0, mips, 0, 1};
        vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // One mip level of a colour image, between transfers / to sampling
    static void barrierMip(VkCommandBuffer cb, VkImage image, uint32_t mip, VkImageLayout from, VkImageLayout to,
                           VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                                                             : VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }

    VkCommandBuffer beginOneTime() {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(uploadCmd_, 0);
        vkBeginCommandBuffer(uploadCmd_, &bi);
        return uploadCmd_;
    }

    void endOneTime(VkCommandBuffer cb) {
        ok(vkEndCommandBuffer(cb));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkResetFences(device_, 1, &uploadFence_);
        ok(vkQueueSubmit(queue_, 1, &si, uploadFence_));
        vkWaitForFences(device_, 1, &uploadFence_, VK_TRUE, UINT64_MAX);
    }

    bool createImage(int w, int h, VkFormat format, VkImageUsageFlags usage, VkImage& image, Alloc& mem, int samples = 1) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {uint32_t(w), uint32_t(h), 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VkSampleCountFlagBits(samples);
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
#ifdef __APPLE__
        if (metalObjects_) ii.pNext = MetalFx::imageExportInfo(); // (its MTLTexture handed to MetalFX)
#endif
        if (!ok(vkCreateImage(device_, &ii, nullptr, &image))) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, image, &req);
        return allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, mem) &&
               ok(vkBindImageMemory(device_, image, blocks_[size_t(mem.block)].memory, mem.offset));
    }

    void createTarget(int w, int h) {
        targetW_ = w;
        targetH_ = h;
        // The frame: also written by the upscalers (storage) or a stretched scene (transfer)
        createImage(w, h, kColorFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | (upscaleCaps_ ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
                    colorImage_, colorMem_);
        colorView_ = makeView(colorImage_, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        if (samples_ > 1) {
            createImage(w, h, kColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                        msColorImage_, msColorMem_, samples_);
            msColorView_ = makeView(msColorImage_, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        createImage(w, h, depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depthImage_, depthMem_, samples_);
        depthView_ = makeView(depthImage_, depthFormat_, depthAspect_ & VK_IMAGE_ASPECT_DEPTH_BIT);
        haveFrame_ = false;
    }

    void destroyTarget() {
        if (colorView_) vkDestroyImageView(device_, colorView_, nullptr);
        if (colorImage_) vkDestroyImage(device_, colorImage_, nullptr);
        if (depthView_) vkDestroyImageView(device_, depthView_, nullptr);
        if (depthImage_) vkDestroyImage(device_, depthImage_, nullptr);
        if (msColorView_) vkDestroyImageView(device_, msColorView_, nullptr);
        if (msColorImage_) vkDestroyImage(device_, msColorImage_, nullptr);
        freeAlloc(colorMem_);
        freeAlloc(depthMem_);
        freeAlloc(msColorMem_);
        msColorView_ = VK_NULL_HANDLE;
        msColorImage_ = VK_NULL_HANDLE;
        msColorMem_ = Alloc();
        colorView_ = depthView_ = VK_NULL_HANDLE;
        colorImage_ = depthImage_ = VK_NULL_HANDLE;
        colorMem_ = depthMem_ = Alloc();
        targetW_ = targetH_ = 0;
    }

    void createSceneTarget(int w, int h) {
        sceneW_ = w;
        sceneH_ = h;
        const VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        createImage(w, h, kColorFormat, use | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, sceneColor_, sceneColorMem_);
        createImage(w, h, kMotionFormat, use, sceneMotion_, sceneMotionMem_);
        createImage(w, h, kReactiveFormat, use, sceneReactive_, sceneReactiveMem_);
        createImage(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    sceneDepth_, sceneDepthMem_);
        sceneColorView_ = makeView(sceneColor_, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        sceneMotionView_ = makeView(sceneMotion_, kMotionFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        sceneReactiveView_ = makeView(sceneReactive_, kReactiveFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        sceneDepthView_ = makeView(sceneDepth_, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    void destroySceneTarget() {
        for (VkImageView* v : {&sceneColorView_, &sceneMotionView_, &sceneReactiveView_, &sceneDepthView_}) {
            if (*v) vkDestroyImageView(device_, *v, nullptr);
            *v = VK_NULL_HANDLE;
        }
        for (VkImage* i : {&sceneColor_, &sceneMotion_, &sceneReactive_, &sceneDepth_}) {
            if (*i) vkDestroyImage(device_, *i, nullptr);
            *i = VK_NULL_HANDLE;
        }
        for (Alloc* a : {&sceneColorMem_, &sceneMotionMem_, &sceneReactiveMem_, &sceneDepthMem_}) {
            freeAlloc(*a);
            *a = Alloc();
        }
        sceneW_ = sceneH_ = 0;
    }

    void createSwapchain() {
        vkDeviceWaitIdle(device_);
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = std::clamp(uint32_t(std::max(w, 1)), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(uint32_t(std::max(h, 1)), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) return; // minimised
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
        VkSurfaceFormatKHR format = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                                    : formats[0];
        for (const VkSurfaceFormatKHR& f : formats) // UNORM, like GL's default framebuffer (no sRGB encode)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format = f;
                break;
            }
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!vsync_)
            for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
                if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                    mode = m;
                    break;
                }
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface_;
        ci.minImageCount = std::max(caps.minImageCount, 3u);
        if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
        ci.imageFormat = format.format;
        ci.imageColorSpace = format.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = mode;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = swapchain_;
        VkSwapchainKHR sc = VK_NULL_HANDLE;
        if (!ok(vkCreateSwapchainKHR(device_, &ci, nullptr, &sc))) return;
        destroySwapchain();
        swapchain_ = sc;
        swapExtent_ = extent;
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
        swapImages_.resize(n);
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapImages_.data());
        for (uint32_t i = 0; i < n; ++i) {
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore s = VK_NULL_HANDLE;
            ok(vkCreateSemaphore(device_, &si, nullptr, &s));
            presentSems_.push_back(s);
        }
        swapchainDirty_ = false;
    }

    void destroySwapchain() {
        for (VkSemaphore s : presentSems_) vkDestroySemaphore(device_, s, nullptr);
        presentSems_.clear();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
        swapImages_.clear();
    }

    SDL_Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties memProps_{};
    uint32_t family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer uploadCmd_ = VK_NULL_HANDLE, cmd_ = VK_NULL_HANDLE;
    VkFence uploadFence_ = VK_NULL_HANDLE;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    VkImageAspectFlags depthAspect_ = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule quadVert_ = VK_NULL_HANDLE, quadFrag_ = VK_NULL_HANDLE, meshVert_ = VK_NULL_HANDLE,
                   meshFrag_ = VK_NULL_HANDLE;
    VkPipeline quadPipeline_ = VK_NULL_HANDLE, bound_ = VK_NULL_HANDLE;
    VkDescriptorSet boundSet_ = VK_NULL_HANDLE;
    std::map<uint32_t, VkPipeline> pipelines_;
    std::array<VkSampler, 32> samplers_{};
    float anisotropy_ = 1.0f;
    TextureFiltering filtering_, filteringWanted_; // (the samplers', the options')
    std::vector<Block> blocks_;
    std::vector<TextureRec> textures_;
    std::vector<MeshRec> meshes_;
    std::vector<std::pair<uint64_t, std::function<void()>>> deletions_;
    std::array<Frame, kFrames> frames_{};
    uint64_t serial_ = 0;
    bool inFrame_ = false, haveFrame_ = false, colorWrite_ = true, vsync_ = true, swapchainDirty_ = true;
    int errors_ = 0;
    // Offscreen target
    int targetW_ = 0, targetH_ = 0;
    VkImage colorImage_ = VK_NULL_HANDLE, depthImage_ = VK_NULL_HANDLE;
    VkImageView colorView_ = VK_NULL_HANDLE, depthView_ = VK_NULL_HANDLE;
    Alloc colorMem_, depthMem_;
    VkImage msColorImage_ = VK_NULL_HANDLE; // MSAA: the multisampled colour, resolved into colorImage_
    VkImageView msColorView_ = VK_NULL_HANDLE;
    Alloc msColorMem_;
    int samples_ = 1, wantSamples_ = 1;
    // Swapchain
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D swapExtent_{};
    std::vector<VkImage> swapImages_;
    std::vector<VkSemaphore> presentSems_;
    struct {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* mapped = nullptr;
        VkDeviceSize size = 0;
    } readback_;
    Texture white_;

    // ---- temporal upscaling ----
    UpscaleSettings upscale_;
    Upscaler active_ = Upscaler::Off; // this frame's (Off while not available)
    bool fsrSupported_ = false, upscaleCaps_ = false;
    Fsr3 fsr_;
    Dlss dlss_;
#ifdef __APPLE__
    MetalFx metalfx_;
    bool metalObjects_ = false; // VK_EXT_metal_objects on
    // MetalFX: the frame's work so far submitted, the scaler after it on the same Metal queue, the
    // frame going on in its second command buffer (vk_metalfx.hpp)
    bool metalFxEncode() {
        Frame& f = frames_[serial_ % kFrames];
        bool encoded = false;
        if (ok(vkEndCommandBuffer(cmd_))) {
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd_;
            if (ok(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE))) {
                MetalFxInputs in;
                in.color = sceneColor_;
                in.depth = sceneDepth_;
                in.motion = sceneMotion_;
                in.output = colorImage_;
                in.renderW = sceneW_;
                in.renderH = sceneH_;
                in.outW = targetW_;
                in.outH = targetH_;
                in.jitterX = jitter_[0];
                in.jitterY = jitter_[1];
                in.motionScaleX = float(sceneW_); // (UV offsets -> render pixels, as DLSS's)
                in.motionScaleY = float(sceneH_);
                in.reset = historyReset_;
                encoded = metalfx_.encode(in);
            }
        }
        cmd_ = f.cmd2;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cmd_, 0);
        vkBeginCommandBuffer(cmd_, &bi);
        bound_ = VK_NULL_HANDLE; // (a new command buffer: nothing bound)
        boundLayout_ = VK_NULL_HANDLE;
        boundSet_ = VK_NULL_HANDLE;
        return encoded;
    }
#endif
    VkPipelineLayout sceneLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout boundLayout_ = VK_NULL_HANDLE;
    VkShaderModule sceneQuadVert_ = VK_NULL_HANDLE, sceneQuadFrag_ = VK_NULL_HANDLE, sceneMeshVert_ = VK_NULL_HANDLE,
                   sceneMeshFrag_ = VK_NULL_HANDLE;
    VkPipeline sceneQuadPipeline_ = VK_NULL_HANDLE;
    // Scene targets (render size)
    int sceneW_ = 0, sceneH_ = 0;
    VkImage sceneColor_ = VK_NULL_HANDLE, sceneDepth_ = VK_NULL_HANDLE, sceneMotion_ = VK_NULL_HANDLE,
            sceneReactive_ = VK_NULL_HANDLE;
    VkImageView sceneColorView_ = VK_NULL_HANDLE, sceneDepthView_ = VK_NULL_HANDLE, sceneMotionView_ = VK_NULL_HANDLE,
                sceneReactiveView_ = VK_NULL_HANDLE;
    Alloc sceneColorMem_, sceneDepthMem_, sceneMotionMem_, sceneReactiveMem_;
    bool inScene_ = false, historyReset_ = true;
    int passW_ = 0, passH_ = 0;        // the render pass being recorded
    float vpW_ = 1, vpH_ = 1;          // the current viewport
    float jitter_[2] = {0, 0};         // pixels
    uint32_t jitterIndex_ = 0, meshGeneration_ = 0;
    float cameraNear_ = 10, cameraFar_ = 4000, cameraFovY_ = 0.8f;
    uint64_t lastFrameNs_ = 0;
    float frameMs_ = 16.7f;
    std::unordered_map<uint64_t, DrawHistory> history_, historyNext_;
    std::unordered_map<uint32_t, uint32_t> drawCounts_;
    std::vector<uint8_t> scratch_;
};

} // namespace

std::unique_ptr<RenderDevice> makeVkDevice() { return std::make_unique<VkDevice_>(); }

void useBundledVulkan() {
#ifdef __APPLE__
    if (const char* base = SDL_GetBasePath()) { // (a .app's: Contents/Resources/)
        const std::string loader = std::string(base) + "../Frameworks/libvulkan.1.dylib";
        if (SDL_GetPathInfo(loader.c_str(), nullptr)) SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, loader.c_str());
    }
#endif
}

} // namespace cr

#else

namespace cr {
std::unique_ptr<RenderDevice> makeVkDevice() { return nullptr; } // built without Vulkan
void useBundledVulkan() {}
} // namespace cr

#endif
