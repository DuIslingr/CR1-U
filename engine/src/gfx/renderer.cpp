#include "gfx/renderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

namespace cr {

const char* backendName(Backend b) { return b == Backend::Vulkan ? "Vulkan" : "OpenGL"; }

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

namespace {

std::unique_ptr<RenderDevice> makeDevice(Backend backend) {
    return backend == Backend::Vulkan ? makeVkDevice() : makeGlDevice();
}

// Interleaved vertex: position (3 f32), uv (2 f32), colour (4 u8 normalised) = 24 bytes
constexpr size_t kStride = 24;

void interleave(const std::vector<float>& pos, const std::vector<float>& uv, const std::vector<uint8_t>& col,
                std::vector<uint8_t>& out) {
    const size_t n = pos.size() / 3;
    out.resize(n * kStride);
    for (size_t i = 0; i < n; ++i) {
        uint8_t* v = &out[i * kStride];
        std::memcpy(v, &pos[i * 3], 12);
        if (uv.size() >= (i + 1) * 2) std::memcpy(v + 12, &uv[i * 2], 8);
        else std::memset(v + 12, 0, 8);
        if (col.size() >= (i + 1) * 4) std::memcpy(v + 20, &col[i * 4], 4);
        else std::memset(v + 20, 255, 4);
    }
}

// Mipmap modes fall back to their base filter (textures are uploaded without mipmaps):
// codes 0 nearest, 1 linear, 2 nearest-mip-nearest, 3 linear-mip-nearest, 4 nearest-mip-linear,
// 5 linear-mip-linear
bool linearFilter(uint8_t code) { return code == 1 || code == 3 || code == 5; }

} // namespace

uint64_t Renderer::windowFlags(Backend backend) {
    std::unique_ptr<RenderDevice> d = makeDevice(backend);
    return d ? d->windowFlags() : 0;
}

bool Renderer::init(SDL_Window* window, Backend backend) {
    backend_ = backend;
    dev_ = makeDevice(backend);
    if (!dev_ || !dev_->init(window)) {
        if (dev_) dev_->shutdown();
        dev_.reset();
        return false;
    }
    Image white;
    white.width = white.height = 1;
    white.rgba = {255, 255, 255, 255};
    white_ = upload(white);
    return true;
}

void Renderer::shutdown() {
    if (!dev_) return;
    destroy(white_);
    dev_->shutdown();
    dev_.reset();
}

std::string Renderer::description() const { return dev_ ? dev_->description() : std::string("no renderer"); }

GpuMesh Renderer::upload(const Mesh& mesh) {
    interleave(mesh.positions, mesh.uvs, mesh.colors, scratch_);
    return dev_->upload(scratch_, mesh.indices, mesh.texture);
}

void Renderer::updatePositions(GpuMesh& mesh, const std::vector<float>& positions, const std::vector<float>& uvs,
                               const std::vector<uint8_t>& colors) {
    interleave(positions, uvs, colors, scratch_);
    dev_->updateVertices(mesh, scratch_);
}

void Renderer::destroy(GpuMesh& mesh) {
    if (dev_) dev_->destroy(mesh);
    mesh = {};
}

Texture Renderer::upload(const Image& img) {
    Texture t = dev_->upload(img);
    t.cutout = img.cutout;
    if (img.logicalW > 0 && img.logicalH > 0) { // HD: drawn at the original's size
        t.width = img.logicalW;
        t.height = img.logicalH;
    }
    return t;
}

void Renderer::destroy(Texture& tex) {
    if (dev_ && tex.id) dev_->destroy(tex);
    tex = {};
}

void Renderer::begin3D() {
    // The letterboxed logical screen, so the projection aspect is aspect()
    dev_->setViewport(offX_, offY_, screenW_ * scaleX_, kLogicalH * scaleY_);
    dev_->clearDepth();
}

void Renderer::begin3DRect(float x, float y, float w, float h) {
    begin3D();
    x += originX_;
    if (phone_) {
        layoutToScreen(x, y);
        w *= layoutScale(), h *= layoutScale();
    }
    dev_->setViewport(offX_ + x * scaleX_, offY_ + y * scaleY_, w * scaleX_, h * scaleY_);
}

void Renderer::drawMesh(const GpuMesh& mesh, const Texture* tex, const Mat4& mvp, const Material& mat,
                        bool smoothFilter) {
    // GL ES 1.1 lighting with full-bright scene light (ambient = diffuse = 1) until scene
    // Light records are decoded: rgb = emission + ambient + diffuse, alpha = diffuse alpha.
    // 357 materials rely on this (black diffuse, white ambient), so diffuse alone is not enough.
    MeshShading sh;
    for (int i = 0; i < 3; ++i) sh.color[i] = std::min(1.0f, mat.emission[i] + mat.ambient[i] + mat.diffuse[i]);
    sh.color[3] = mat.diffuse[3];
    sh.alphaRef = mat.alphaThreshold / 255.0f;
    const bool textured = tex && tex->id;
    sh.texMode = textured ? int(mat.texBlend <= 4 ? mat.texBlend : 4) : -1;
    sh.alphaTex = textured && tex->alphaOnly;
    DrawState st;
    st.blend = mat.blended();
    st.blendSrc = mat.blendSrc;
    st.blendDst = mat.blendDst;
    st.cull = mat.culling == 0 ? 0 : mat.culling == 1 ? 1 : 2;
    st.depthTest = mat.depthTest();
    st.depthWrite = mat.depthWrite();
    // An HD texture for a cut-out (the original's alpha 0 or 255, drawn nearest): drawn filtered its
    // edges come out part-transparent, and blended there - over whatever the frame holds yet, the
    // depth written - they'd ring it in black. As the original looks: kept above half, else gone,
    // unblended (a material fading the whole thing still blends)
    if (textured && tex->cutout && st.blend && mat.blendSrc == 4 && mat.blendDst == 5 && sh.color[3] >= 0.999f)
        st.blend = false, sh.alphaRef = std::max(sh.alphaRef, 0.5f);
    st.clampS = mat.wrapS != 0;
    st.clampT = mat.wrapT != 0;
    st.minLinear = smoothFilter || linearFilter(mat.minFilter);
    st.magLinear = smoothFilter || linearFilter(mat.magFilter);
    dev_->drawMesh(mesh, textured ? tex : nullptr, mvp.m, sh, st);
}

void Renderer::end3D() {
    dev_->setViewport(0, 0, float(drawableW_), float(drawableH_));
}

void Renderer::beginFrame(int windowW, int windowH) {
    // The frame: the window's size, or render-scaled when the device renders offscreen
    const float k = scalable() ? std::clamp(renderScale_, 0.25f, 4.0f) : 1.0f;
    frameW_ = std::clamp(int(windowW * k + 0.5f), 1, 8192);
    frameH_ = std::clamp(int(windowH * k + 0.5f), 1, 8192);
    windowAspect_ = float(windowW) / float(std::max(windowH, 1));
    dev_->beginFrame(frameW_, frameH_);
    originX_ = 0;
    // The scene first: the frame's size, or smaller while an upscaler is on
    int sw = frameW_, sh = frameH_;
    dev_->sceneSize(frameW_, frameH_, sw, sh);
    inScene_ = true;
    mapScreen(sw, sh);
}

void Renderer::mapScreen(int drawableW, int drawableH) {
    drawableW_ = drawableW;
    drawableH_ = drawableH;
    // The logical screen: 768 tall, 4:3 or (widescreen) the window's aspect up to 21:9, fitted
    // inside the frame and centred
    const float least = phone_ ? 1.5f : 4.0f / 3.0f; // the iPhone's 3:2, the iPad's 4:3
    const float fixed = androidScreen() ? 16.0f / 9.0f : least; // (Android: a 1280 x 720 phone's)
    const float aspect = widescreen_ ? std::clamp(windowAspect_, least, 21.0f / 9.0f) : fixed;
    screenW_ = kLogicalH * aspect;
    const float sx = float(frameW_) / screenW_, sy = float(frameH_) / kLogicalH;
    scale_ = sx < sy ? sx : sy;
    // ... and a smaller scene (upscaling) is exactly the frame scaled down per axis, so its pixels
    // land where the upscaler puts them
    const float kx = float(drawableW) / float(frameW_), ky = float(drawableH) / float(frameH_);
    scaleX_ = scale_ * kx;
    scaleY_ = scale_ * ky;
    offX_ = (frameW_ - screenW_ * scale_) * 0.5f * kx;
    offY_ = (frameH_ - kLogicalH * scale_) * 0.5f * ky;

    // Logical -> clip space, y down
    const float l = -offX_ / scaleX_, r = (drawableW - offX_) / scaleX_;
    const float t = -offY_ / scaleY_, b = (drawableH - offY_) / scaleY_;
    const float proj[16] = {
        2 / (r - l), 0, 0, 0,
        0, 2 / (t - b), 0, 0,
        0, 0, -1, 0,
        -(r + l) / (r - l), -(t + b) / (t - b), 0, 1,
    };
    std::memcpy(proj_, proj, sizeof proj);
}

void Renderer::endScene() {
    if (!inScene_ || !dev_) return;
    inScene_ = false;
    dev_->endScene();
    mapScreen(frameW_, frameH_);
}

void Renderer::setUpscaler(const UpscaleSettings& s) {
    if (dev_) dev_->setUpscaler(s);
}

void Renderer::setCamera(float zNear, float zFar, float fovYRad) {
    if (dev_) dev_->setCamera(zNear, zFar, fovYRad);
}

void Renderer::resetHistory() {
    if (dev_) dev_->resetHistory();
}

void Renderer::endFrame(bool present) {
    endScene();
    if (phone_) { // the iPad layout's margins land outside the 3:2 screen: keep the bars black
        const float l = -offX_ / scaleX_, r = (drawableW_ - offX_) / scaleX_;
        const float t = -offY_ / scaleY_, b = (drawableH_ - offY_) / scaleY_;
        auto bar = [&](float x0, float y0, float x1, float y1) {
            if (x1 <= x0 || y1 <= y0) return;
            const float v[24] = {x0, y0, 0, 0, x1, y0, 1, 0, x1, y1, 1, 1, x0, y0, 0, 0, x1, y1, 1, 1, x0, y1, 0, 1};
            const float c[4] = {0, 0, 0, 1};
            dev_->drawQuad(white_, v, proj_, c);
        };
        bar(l, t, r, 0);
        bar(l, float(kLogicalH), r, b);
        bar(l, 0, 0, float(kLogicalH));
        bar(screenW_, 0, r, float(kLogicalH));
    }
    dev_->endFrame(present);
}

bool Renderer::readPixels(std::vector<uint8_t>& rgba, int& w, int& h) { return dev_->readPixels(rgba, w, h); }

int Renderer::takeErrors() { return dev_ ? dev_->takeErrors() : 0; }

void Renderer::setVSync(bool on) { dev_->setVSync(on); }

void Renderer::setSamples(int n) {
    if (dev_) dev_->setSamples(n);
}

void Renderer::drawQuad(const Texture& tex, float x, float y, float w, float h, float r, float g,
                        float b, float a) {
    if (!tex.id) return;
    x += originX_;
    if (phone_) {
        layoutToScreen(x, y);
        w *= layoutScale(), h *= layoutScale();
    }
    const float v[24] = {
        x, y, 0, 0,  x + w, y, 1, 0,  x + w, y + h, 1, 1,
        x, y, 0, 0,  x + w, y + h, 1, 1,  x, y + h, 0, 1,
    };
    const float c[4] = {r, g, b, a};
    dev_->drawQuad(tex, v, proj_, c);
}

void Renderer::drawQuadUV(const Texture& tex, float x, float y, float w, float h, float sx, float sy, float sw,
                          float sh, float r, float g, float b, float a) {
    if (!tex.id || tex.width <= 0 || tex.height <= 0) return;
    x += originX_;
    if (phone_) {
        layoutToScreen(x, y);
        w *= layoutScale(), h *= layoutScale();
    }
    const float u0 = sx / tex.width, v0 = sy / tex.height, u1 = (sx + sw) / tex.width, v1 = (sy + sh) / tex.height;
    const float v[24] = {
        x, y, u0, v0,  x + w, y, u1, v0,  x + w, y + h, u1, v1,
        x, y, u0, v0,  x + w, y + h, u1, v1,  x, y + h, u0, v1,
    };
    const float c[4] = {r, g, b, a};
    dev_->drawQuad(tex, v, proj_, c);
}

void Renderer::fillRect(float x, float y, float w, float h, float r, float g, float b, float a) {
    drawQuad(white_, x, y, w, h, r, g, b, a);
}

void Renderer::toLogical(float px, float py, float& lx, float& ly) const {
    lx = (px - offX_) / scaleX_;
    ly = (py - offY_) / scaleY_;
    if (phone_) screenToLayout(lx, ly);
}

// The layout's centre stays the screen's; (·, 64) - tex2d y 0 - goes to the top (Android's: the
// centre's y too)
void Renderer::layoutToScreen(float& x, float& y) const {
    if (!phone_) return;
    const float k = layoutScale(), cy = androidScreen() ? 384.0f : 64.0f, sy = androidScreen() ? 384.0f : 0.0f;
    x = screenW_ * 0.5f + (x - screenW_ * 0.5f) * k;
    y = sy + (y - cy) * k;
}

void Renderer::screenToLayout(float& x, float& y) const {
    if (!phone_) return;
    const float k = layoutScale(), cy = androidScreen() ? 384.0f : 64.0f, sy = androidScreen() ? 384.0f : 0.0f;
    x = screenW_ * 0.5f + (x - screenW_ * 0.5f) / k;
    y = cy + (y - sy) / k;
}

void Renderer::setColorWrite(bool on) { dev_->setColorWrite(on); }

} // namespace cr
