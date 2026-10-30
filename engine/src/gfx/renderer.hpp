#pragma once
// Textured quads in a fixed 1024x768 logical space (the iPad screen), letterboxed into
// whatever window / display we get, and the original engine's meshes - on OpenGL or Vulkan
// (render_device.hpp).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "assets/model.hpp"
#include "assets/texture.hpp"
#include "core/math.hpp"
#include "gfx/render_device.hpp"

namespace cr {

struct Texture {
    unsigned id = 0;   // opaque device handle (0 = none)
    int width = 0;     // the game's size for it (pixel rectangles are in these units)
    int height = 0;
    bool alphaOnly = false;
    bool noAlpha = false; // (Image::noAlpha: a mesh's GL_REPLACE keeps the fragment's alpha)
    bool cutout = false; // (an HD texture standing in for a cut-out: see Image::cutout)
};

// A mesh uploaded to the GPU: interleaved position/uv plus a triangle index buffer.
struct GpuMesh {
    unsigned vao = 0, vbo = 0, ebo = 0; // opaque mesh handle in vao (0 = none); vbo/ebo reserved
    int indexCount = 0;
    int texture = -1;
};

class Renderer {
public:
    // The game's screen (and its 2D layout): 1024x768. In widescreen the logical screen keeps
    // the height and widens to the window's aspect (up to 21:9); the game's 2D is then drawn in
    // the middle 1024 (setOrigin(uiX())) while 3D views and fades use the whole width.
    static constexpr int kLogicalW = 1024;
    static constexpr int kLogicalH = 768;

    Renderer();
    ~Renderer();
    // Window flags `backend` needs (create the window with them, then call init)
    static uint64_t windowFlags(Backend backend);
    bool init(SDL_Window* window, Backend backend);
    void shutdown();
    bool restoreAfterContextLoss();
    Backend backend() const { return backend_; }
    std::string description() const;

    Texture upload(const Image& img);
    void destroy(Texture& tex);

    // Begin a frame for a drawable of the given pixel size; endFrame shows it when `present`.
    void beginFrame(int drawableW, int drawableH);
    void endFrame(bool present);
    // The last frame, RGBA, top row first
    bool readPixels(std::vector<uint8_t>& rgba, int& w, int& h);
    int takeErrors();
    void setVSync(bool on);
    // Draws `tex` into the logical-space rectangle (x, y, w, h), top-left origin.
    void drawQuad(const Texture& tex, float x, float y, float w, float h,
                  float r = 1, float g = 1, float b = 1, float a = 1);
    // As drawQuad, showing only the texture's pixel rectangle (sx, sy, sw, sh).
    void drawQuadUV(const Texture& tex, float x, float y, float w, float h, float sx, float sy, float sw, float sh,
                    float r = 1, float g = 1, float b = 1, float a = 1);
    // Solid-colour rectangle in logical space.
    void fillRect(float x, float y, float w, float h, float r, float g, float b, float a);
    // A plain colour over any four corners (top left, top right, bottom right, bottom left)
    void fillQuad(const float xy[8], float r, float g, float b, float a);

    // Converts window pixel coordinates to logical coordinates.
    void toLogical(float px, float py, float& lx, float& ly) const;

    // 3D path. begin3D clears depth and sets the viewport to the letterboxed screen.
    GpuMesh upload(const Mesh& mesh);
    // Replaces vertex positions (xyz per render vertex) of an uploaded mesh; UVs and colours are
    // re-sent unchanged because the vertex data is interleaved.
    void updatePositions(GpuMesh& mesh, const std::vector<float>& positions, const std::vector<float>& uvs,
                         const std::vector<uint8_t>& colors);
    void destroy(GpuMesh& mesh);
    void begin3D();
    // As begin3D, but renders into the logical-space rectangle (x, y, w, h), top-left origin.
    void begin3DRect(float x, float y, float w, float h);
    // Draws with the original engine's material state: blending, alpha test, culling,
    // depth test/write, texture blend mode, wrap and filter. `smoothFilter` forces linear
    // filtering (the original uses nearest).
    // tex2: the material's texture unit 1 (its environment texBlend2), at uv * uv2.xy + uv2.zw
    void drawMesh(const GpuMesh& mesh, const Texture* tex, const Mat4& mvp, const Material& mat,
                  bool smoothFilter = false, const Texture* tex2 = nullptr, const float* uv2 = nullptr);
    void end3D();
    // Colour writes on/off (glColorMask); off draws depth only, e.g. field occluders.
    void setColorWrite(bool on);
    float aspect() const { return screenW_ / kLogicalH; }
    float screenW() const { return screenW_; }         // logical width of the whole screen
    float uiX() const { return (screenW_ - kLogicalW) * 0.5f; } // where the 1024-wide layout starts
    void setOrigin(float x) { originX_ = x; }          // added to every 2D quad's x
    void setWidescreen(bool on) { widescreen_ = on; }
    // The iPhone layout (2.2.0's): the screen is 3:2 (wider in widescreen) and the 2D layout's
    // safe area - tex2d (0, 0)..(480, 320), the 1024 layout's (32, 64)..(992, 704) - fills its
    // height (x 1.2, centred); the iPad layout's margins fall outside. 2D draws and toLogical
    // work in layout units either way; screenToLayout / layoutToScreen convert a point.
    void setPhoneLayout(bool on) { phone_ = on; }
    bool phoneLayout() const { return phone_; }
    static constexpr float kPhoneScale = 1.2f;
    // The Android release's screen (with the iPhone layout; docs/ANDROID.md): its 480 x 320 2D at
    // 2x on a 720-tall screen, centred - the layout x 16/15 about the screen's centre here - the
    // full-width parts at the screen's width; 16:9 when not widescreen
    void setAndroidScreen(bool on) { android_ = on; }
    bool androidScreen() const { return android_ && phone_; }
    static constexpr float kAndroidScale = 16.0f / 15.0f;
    float layoutScale() const { return androidScreen() ? kAndroidScale : kPhoneScale; }
    void layoutToScreen(float& x, float& y) const;
    // The visible screen's right / top edge in the layout's units (toLogical's)
    float layoutRight() const { return phone_ ? screenW_ * 0.5f + screenW_ * 0.5f / layoutScale() : screenW_; }
    float layoutTop() const { return androidScreen() ? 384.0f - 384.0f / kAndroidScale : phone_ ? 64.0f : 0.0f; }
    void screenToLayout(float& x, float& y) const;
    // Frames rendered at `scale` x the window's pixels and with `samples` x MSAA, where the
    // device renders offscreen (Vulkan); the frame is then scaled to the window
    void setRenderScale(float scale) { renderScale_ = scale; }
    void setSamples(int n);
    int maxSamples() const { return dev_ ? dev_->maxSamples() : 1; }
    void setFiltering(const TextureFiltering& f) {
        if (dev_) dev_->setFiltering(f);
    }
    int maxAnisotropy() const { return dev_ ? dev_->maxAnisotropy() : 1; }
    bool scalable() const { return dev_ && dev_->offscreen(); }
    // Temporal upscaling (FSR 3.1 / DLSS, Vulkan): the scene - everything until endScene -
    // renders at 1/ratio of the frame and is resolved to it; the UI then draws at full size.
    // upscalerProblem: "" when the mode can run, else why not
    void setUpscaler(const UpscaleSettings& s);
    std::string upscalerProblem(Upscaler m) const { return dev_ ? dev_->upscalerProblem(m) : "no renderer"; }
    void endScene();
    // The scene camera's projection (for the upscaler) and a cut (its history is dropped)
    void setCamera(float zNear, float zFar, float fovYRad);
    void resetHistory();
    int frameWidth() const { return drawableW_; }      // the frame being drawn, in pixels
    int frameHeight() const { return drawableH_; }
    // Window pixels per logical pixel (text is rasterised at this density).
    float pixelScale() const { return phone_ ? scale_ * layoutScale() : scale_; }

private:
    void mapScreen(int w, int h); // logical screen -> the current target's pixels

    std::unique_ptr<RenderDevice> dev_;
    Backend backend_ = Backend::OpenGL;
    int drawableW_ = 0, drawableH_ = 0;
    std::vector<uint8_t> scratch_;
    Texture white_;
    float proj_[16] = {};
    float scale_ = 1, offX_ = 0, offY_ = 0; // scale_: the frame's pixels per logical pixel
    float scaleX_ = 1, scaleY_ = 1;          // the current target's (the scene's, while in it)
    float screenW_ = kLogicalW, originX_ = 0, renderScale_ = 1;
    bool android_ = false;
    bool widescreen_ = false, inScene_ = false, phone_ = false;
    int frameW_ = 0, frameH_ = 0; // the full frame (drawableW_/H_ is the scene's while in it)
    float windowAspect_ = 4.0f / 3.0f;
};

} // namespace cr
