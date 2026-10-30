// OpenGL 3.3 core / GLES 3.0 device (the renderer's original back end).

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "gfx/gl_backup.hpp"

#include "gfx/gl.hpp"
#include "gfx/renderer.hpp"

namespace cr {

namespace {

const char* kVertex = R"(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uProj;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
}
)";

const char* kFragment = R"(
in vec2 vUV;
uniform sampler2D uTex;
uniform vec4 uColor;
out vec4 fragColor;
void main() {
    fragColor = texture(uTex, vUV) * uColor;
}
)";

const char* kMeshVertex = R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uMvp;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

// Unlit surface emulating GL ES 1.1 texture environment modes (lighting comes later).
// uTexMode: 0 replace, 1 add, 2 blend, 3 decal, 4 modulate, -1 untextured.
const char* kMeshFragment = R"(
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
uniform sampler2D uTex2; // texture unit 1
uniform vec4 uColor;
uniform int uTexMode;
uniform int uTexMode2;  // unit 1's environment over unit 0's result, -1 none
uniform vec4 uUv2;      // unit 1's coordinates: uv * xy + zw
uniform int uAlphaTex;  // GL_ALPHA texture: colour always comes from the fragment
uniform float uAlphaRef;
out vec4 fragColor;
vec4 texEnv(int mode, vec4 base, vec4 t) {
    if (mode == 0) return t;
    if (mode == 1) return vec4(base.rgb + t.rgb, base.a * t.a);
    if (mode == 2) return vec4(base.rgb * (vec3(1.0) - t.rgb), base.a * t.a);
    if (mode == 3) return vec4(mix(base.rgb, t.rgb, t.a), base.a);
    return base * t;
}
void main() {
    vec4 base = uColor * vColor;
    vec4 c = base;
    if (uTexMode >= 0) {
        vec4 t = texture(uTex, vUV);
        if (uAlphaTex == 1) c = vec4(base.rgb, uTexMode == 0 ? t.a : base.a * t.a);
        else c = texEnv(uTexMode, base, t);
        if (uAlphaTex == 2 && uTexMode == 0) c.a = base.a; // (GL_RGB: replace takes the colour alone)
    }
    if (uTexMode2 >= 0) c = texEnv(uTexMode2, c, texture(uTex2, vUV * uUv2.xy + uUv2.zw));
    if (uAlphaRef > 0.0 && c.a < uAlphaRef) discard;
    fragColor = c;
}
)";

unsigned compile(unsigned type, const char* body) {
    const std::string src = std::string(gl::shaderHeader()) + body;
    const char* p = src.c_str();
    const unsigned s = gl::CreateShader(type);
    gl::ShaderSource(s, 1, &p, nullptr);
    gl::CompileShader(s);
    GLint ok = 0;
    gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl::GetShaderInfoLog(s, sizeof log, nullptr, log);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "shader compile failed: %s", log);
        gl::DeleteShader(s);
        return 0;
    }
    return s;
}

unsigned link(const char* vsBody, const char* fsBody) {
    const unsigned vs = compile(GL_VERTEX_SHADER, vsBody);
    const unsigned fs = compile(GL_FRAGMENT_SHADER, fsBody);
    if (!vs || !fs) return 0;
    const unsigned p = gl::CreateProgram();
    gl::AttachShader(p, vs);
    gl::AttachShader(p, fs);
    gl::LinkProgram(p);
    gl::DeleteShader(vs);
    gl::DeleteShader(fs);
    GLint ok = 0;
    gl::GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl::GetProgramInfoLog(p, sizeof log, nullptr, log);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "program link failed: %s", log);
        gl::DeleteProgram(p);
        return 0;
    }
    return p;
}

// Blend factor codes follow the GL ES 1.1 factor lists (source allows SATURATE, destination
// allows SRC_COLOR) — 9 source and 8 destination values, as validated by setBlendMode_sd.
constexpr GLenum kBlendSrc[9] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA,
                                 GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA,
                                 GL_SRC_ALPHA_SATURATE};
constexpr GLenum kBlendDst[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                                 GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};

constexpr size_t kStride = 24;

class GlDevice final : public RenderDevice {
public:
    uint64_t windowFlags() const override { return SDL_WINDOW_OPENGL; }

    bool init(SDL_Window* window) override {
#ifdef CR_GLES
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // macOS
#endif
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        window_ = window;
        ctx_ = SDL_GL_CreateContext(window);
        if (!ctx_ || !gl::load()) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "OpenGL setup failed: %s", SDL_GetError());
            return false;
        }
        return initObjects();
    }

    // SDL has already made its replacement context current. Old GL names must not
    // be deleted here: they belong to the lost context, not this new namespace.
    bool restoreAfterContextLoss() override {
        const SDL_GLContext replacement = SDL_GL_GetCurrentContext();
        if (!replacement || SDL_GL_GetCurrentWindow() != window_) return false;
        ctx_ = replacement;
        program_ = meshProgram_ = vao_ = vbo_ = 0;
        for (auto& [handle, rec] : textures_) rec.name = 0;
        for (auto& [handle, rec] : meshes_) rec.vao = rec.vbo = rec.ebo = 0;
        anisoLimit_ = -1;
        if (!gl::load() || !initObjects()) return false;
        for (auto& [handle, rec] : textures_) {
            std::vector<uint8_t> pixels;
            if (!rec.pixels.unpack(pixels)) return false;
            createTexture(rec, pixels);
        }
        for (auto& [handle, rec] : meshes_) createMesh(rec);
        const int errors = takeErrors();
        if (errors) SDL_LogError(SDL_LOG_CATEGORY_RENDER, "OpenGL recovery: %d errors", errors);
        else SDL_Log("OpenGL context restored: %zu textures, %zu meshes", textures_.size(), meshes_.size());
        return errors == 0;
    }

    void shutdown() override {
        if (!ctx_) return;
        for (auto& [handle, rec] : textures_) gl::DeleteTextures(1, &rec.name);
        for (auto& [handle, rec] : meshes_) deleteMesh(rec);
        textures_.clear();
        meshes_.clear();
        mipmapped_.clear();
        white_ = {};
        if (vbo_) gl::DeleteBuffers(1, &vbo_);
        if (vao_) gl::DeleteVertexArrays(1, &vao_);
        if (program_) gl::DeleteProgram(program_);
        if (meshProgram_) gl::DeleteProgram(meshProgram_);
        vbo_ = vao_ = program_ = meshProgram_ = 0;
        SDL_GL_DestroyContext(ctx_);
        ctx_ = nullptr;
    }

    std::string description() const override {
        return std::string("OpenGL ") + reinterpret_cast<const char*>(gl::GetString(GL_VERSION)) + " / " +
               reinterpret_cast<const char*>(gl::GetString(GL_RENDERER));
    }

    void setFiltering(const TextureFiltering& f) override {
        if (f == filtering_) return;
        filtering_ = f;
        for (unsigned id : mipmapped_) filter(textureName(id));
    }
    int maxAnisotropy() const override {
        if (anisoLimit_ < 0) {
            int n = 1;
            if (SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic") ||
                SDL_GL_ExtensionSupported("GL_ARB_texture_filter_anisotropic")) {
                float m = 1;
                gl::GetFloatv(0x84FF, &m); // (GL_MAX_TEXTURE_MAX_ANISOTROPY)
                n = std::max(1, int(m));
            }
            const_cast<GlDevice*>(this)->anisoLimit_ = n;
        }
        return anisoLimit_;
    }

    Texture upload(const Image& img) override {
        Texture t;
        t.width = img.width;
        t.height = img.height;
        t.alphaOnly = img.alphaOnly;
        t.id = nextTexture_++;
        auto& rec = textures_[t.id];
        rec.width = img.width;
        rec.height = img.height;
        rec.mipmaps = img.mipmaps;
        rec.pixels.store(img.rgba);
        createTexture(rec, img.rgba);
        if (img.mipmaps) mipmapped_.insert(t.id);
        return t;
    }

    void destroy(Texture& tex) override {
        auto it = textures_.find(tex.id);
        if (it != textures_.end()) {
            gl::DeleteTextures(1, &it->second.name);
            textures_.erase(it);
        }
        mipmapped_.erase(tex.id);
        tex = {};
    }

    GpuMesh upload(const std::vector<uint8_t>& vertices, const std::vector<uint32_t>& indices, int texture) override {
        GpuMesh g;
        g.texture = texture;
        g.indexCount = int(indices.size());
        g.vao = nextMesh_++;
        auto& rec = meshes_[g.vao];
        rec.vertices = vertices;
        rec.indices = indices;
        createMesh(rec);
        return g;
    }

    void updateVertices(GpuMesh& mesh, const std::vector<uint8_t>& vertices) override {
        auto it = meshes_.find(mesh.vao);
        if (it == meshes_.end()) return;
        auto& rec = it->second;
        const bool sameSize = rec.vertices.size() == vertices.size();
        rec.vertices = vertices; // includes the latest skinned/animated pose
        gl::BindBuffer(GL_ARRAY_BUFFER, rec.vbo);
        if (sameSize) gl::BufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(vertices.size()), vertices.data());
        else gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size()), vertices.data(), GL_DYNAMIC_DRAW);
    }

    void destroy(GpuMesh& mesh) override {
        auto it = meshes_.find(mesh.vao);
        if (it != meshes_.end()) {
            deleteMesh(it->second);
            meshes_.erase(it);
        }
        mesh = {};
    }

    void beginFrame(int w, int h) override {
        drawableW_ = w;
        drawableH_ = h;
        gl::Viewport(0, 0, w, h);
        gl::ClearColor(0, 0, 0, 1);
        gl::DepthMask(GL_TRUE);
        gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void endFrame(bool present) override {
        if (present) SDL_GL_SwapWindow(window_);
    }

    void setViewport(float fx, float fy, float fw, float fh) override {
        const int x = int(fx), y = int(fy), w = int(fw), h = int(fh);
        gl::Viewport(x, drawableH_ - y - h, w, h);
    }

    void clearDepth() override {
        gl::DepthMask(GL_TRUE);
        gl::Clear(GL_DEPTH_BUFFER_BIT);
    }

    void setColorWrite(bool on) override {
        const GLboolean v = on ? GL_TRUE : GL_FALSE;
        gl::ColorMask(v, v, v, v);
    }

    void drawQuad(const Texture& tex, const float* v, const float* proj, const float* rgba) override {
        gl::Disable(GL_DEPTH_TEST);
        gl::Disable(GL_CULL_FACE);
        gl::Enable(GL_BLEND);
        gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl::UseProgram(program_);
        gl::UniformMatrix4fv(uProj_, 1, GL_FALSE, proj);
        gl::Uniform1i(uTex_, 0);
        gl::BindVertexArray(vao_);
        gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
        gl::BufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, v, GL_DYNAMIC_DRAW);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::BindTexture(GL_TEXTURE_2D, textureName(tex.id));
        gl::Uniform4f(uColor_, rgba[0], rgba[1], rgba[2], rgba[3]);
        gl::DrawArrays(GL_TRIANGLES, 0, 6);
    }

    void drawMesh(const GpuMesh& mesh, const Texture* tex, const float* mvp, const MeshShading& sh,
                  const DrawState& st) override {
        gl::UseProgram(meshProgram_);
        gl::Uniform1i(uMeshTex_, 0);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::FrontFace(GL_CCW);
        gl::DepthFunc(GL_LEQUAL);
        gl::UniformMatrix4fv(uMvp_, 1, GL_FALSE, mvp);
        gl::Uniform4f(uMeshColor_, sh.color[0], sh.color[1], sh.color[2], sh.color[3]);
        gl::Uniform1f(uAlphaRef_, sh.alphaRef);
        gl::Uniform1i(uTexMode_, sh.texMode);
        gl::Uniform1i(uAlphaTex_, sh.alphaTex ? 1 : sh.noAlphaTex ? 2 : 0);
        const bool second = sh.tex2 && sh.tex2->id && sh.texMode2 >= 0;
        gl::Uniform1i(uTexMode2_, second ? sh.texMode2 : -1);
        gl::Uniform4f(uUv2_, sh.uv2[0], sh.uv2[1], sh.uv2[2], sh.uv2[3]);
        gl::Uniform1i(uMeshTex2_, 1);
        gl::ActiveTexture(GL_TEXTURE1);
        gl::BindTexture(GL_TEXTURE_2D, second ? textureName(sh.tex2->id) : whiteId());
        if (second) {
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            const bool mips = mipmapped_.count(sh.tex2->id) != 0;
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                              mips ? (st.minLinear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR)
                                   : st.minLinear ? GL_LINEAR : GL_NEAREST);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, st.magLinear ? GL_LINEAR : GL_NEAREST);
        }
        gl::ActiveTexture(GL_TEXTURE0);
        const bool textured = tex && tex->id;
        gl::BindTexture(GL_TEXTURE_2D, textured ? textureName(tex->id) : whiteId());
        if (textured) {
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, st.clampS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, st.clampT ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            // Mipmapped (HD) textures blend between mips whatever the base filter
            const bool mips = mipmapped_.count(tex->id) != 0;
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                              mips ? (st.minLinear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR)
                                   : st.minLinear ? GL_LINEAR : GL_NEAREST);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, st.magLinear ? GL_LINEAR : GL_NEAREST);
        }
        if (st.blend) {
            gl::Enable(GL_BLEND);
            gl::BlendFunc(kBlendSrc[st.blendSrc < 9 ? st.blendSrc : 4], kBlendDst[st.blendDst < 8 ? st.blendDst : 5]);
        } else {
            gl::Disable(GL_BLEND);
        }
        if (st.cull == 0) {
            gl::Disable(GL_CULL_FACE);
        } else {
            gl::Enable(GL_CULL_FACE);
            gl::CullFace(st.cull == 1 ? GL_FRONT : GL_BACK);
        }
        if (st.depthTest) gl::Enable(GL_DEPTH_TEST);
        else gl::Disable(GL_DEPTH_TEST);
        gl::DepthMask(st.depthWrite ? GL_TRUE : GL_FALSE);
        gl::BindVertexArray(meshName(mesh.vao));
        gl::DrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
        gl::DepthMask(GL_TRUE);
    }

    bool readPixels(std::vector<uint8_t>& rgba, int& w, int& h) override {
        w = drawableW_;
        h = drawableH_;
        std::vector<uint8_t> px(size_t(w) * h * 4);
        gl::ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        rgba.resize(px.size());
        for (int y = 0; y < h; ++y) // GL rows are bottom-up
            std::memcpy(&rgba[size_t(y) * w * 4], &px[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
        return true;
    }

    int takeErrors() override {
        int n = 0;
        while (gl::GetError() != GL_NO_ERROR) ++n;
        return n;
    }

    void setVSync(bool on) override { vsync_ = on; SDL_GL_SetSwapInterval(on ? 1 : 0); }

private:

    struct TextureRec {
        unsigned name = 0;
        int width = 0, height = 0;
        bool mipmaps = false;
        crgl::Backup pixels;
    };
    struct MeshRec {
        unsigned vao = 0, vbo = 0, ebo = 0;
        std::vector<uint8_t> vertices;
        std::vector<uint32_t> indices;
    };
    std::unordered_map<unsigned, TextureRec> textures_;
    std::unordered_map<unsigned, MeshRec> meshes_;
    unsigned nextTexture_ = 1, nextMesh_ = 1;
    bool vsync_ = true;
    unsigned textureName(unsigned handle) const {
        auto it = textures_.find(handle);
        return it == textures_.end() ? 0 : it->second.name;
    }
    unsigned meshName(unsigned handle) const {
        auto it = meshes_.find(handle);
        return it == meshes_.end() ? 0 : it->second.vao;
    }
    void createTexture(TextureRec& rec, const std::vector<uint8_t>& pixels) {
        gl::GenTextures(1, &rec.name);
        gl::BindTexture(GL_TEXTURE_2D, rec.name);
        gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rec.width, rec.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        if (rec.mipmaps) {
            gl::GenerateMipmap(GL_TEXTURE_2D);
        }
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, rec.mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        if (rec.mipmaps) filter(rec.name);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    void createMesh(MeshRec& rec) {
        gl::GenVertexArrays(1, &rec.vao);
        gl::GenBuffers(1, &rec.vbo);
        gl::GenBuffers(1, &rec.ebo);
        gl::BindVertexArray(rec.vao);
        gl::BindBuffer(GL_ARRAY_BUFFER, rec.vbo);
        gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(rec.vertices.size()), rec.vertices.data(), GL_STATIC_DRAW);
        gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, rec.ebo);
        gl::BufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(rec.indices.size() * sizeof(uint32_t)), rec.indices.data(), GL_STATIC_DRAW);
        gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride, nullptr);
        gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride, reinterpret_cast<const void*>(12));
        gl::VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, kStride, reinterpret_cast<const void*>(20));
        gl::EnableVertexAttribArray(0);
        gl::EnableVertexAttribArray(1);
        gl::EnableVertexAttribArray(2);
        gl::BindVertexArray(0);
    }
    void deleteMesh(MeshRec& rec) {
        gl::DeleteBuffers(1, &rec.ebo);
        gl::DeleteBuffers(1, &rec.vbo);
        gl::DeleteVertexArrays(1, &rec.vao);
    }
    bool initObjects() {
        program_ = link(kVertex, kFragment);
        meshProgram_ = link(kMeshVertex, kMeshFragment);
        if (!program_ || !meshProgram_) return false;
        uMvp_ = gl::GetUniformLocation(meshProgram_, "uMvp");
        uMeshTex_ = gl::GetUniformLocation(meshProgram_, "uTex");
        uAlphaRef_ = gl::GetUniformLocation(meshProgram_, "uAlphaRef");
        uMeshColor_ = gl::GetUniformLocation(meshProgram_, "uColor");
        uTexMode_ = gl::GetUniformLocation(meshProgram_, "uTexMode");
        uMeshTex2_ = gl::GetUniformLocation(meshProgram_, "uTex2");
        uTexMode2_ = gl::GetUniformLocation(meshProgram_, "uTexMode2");
        uUv2_ = gl::GetUniformLocation(meshProgram_, "uUv2");
        uAlphaTex_ = gl::GetUniformLocation(meshProgram_, "uAlphaTex");
        uProj_ = gl::GetUniformLocation(program_, "uProj");
        uColor_ = gl::GetUniformLocation(program_, "uColor");
        uTex_ = gl::GetUniformLocation(program_, "uTex");

        gl::GenVertexArrays(1, &vao_);
        gl::GenBuffers(1, &vbo_);
        gl::BindVertexArray(vao_);
        gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
        gl::BufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 6, nullptr, GL_DYNAMIC_DRAW);
        gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, nullptr);
        gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, reinterpret_cast<const void*>(sizeof(float) * 2));
        gl::EnableVertexAttribArray(0);
        gl::EnableVertexAttribArray(1);
        gl::Enable(GL_BLEND);
        gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
        return true;
    }

    // A 1x1 white texture for untextured meshes
    unsigned whiteId() {
        if (!white_.id) {
            Image img;
            img.width = img.height = 1;
            img.rgba = {255, 255, 255, 255};
            white_ = upload(img);
        }
        return textureName(white_.id);
    }

    SDL_Window* window_ = nullptr;
    SDL_GLContext ctx_ = nullptr;
    unsigned program_ = 0, vao_ = 0, vbo_ = 0;
    int uProj_ = -1, uColor_ = -1, uTex_ = -1;
    unsigned meshProgram_ = 0;
    int uMvp_ = -1, uMeshTex_ = -1, uAlphaRef_ = -1, uMeshColor_ = -1, uTexMode_ = -1, uAlphaTex_ = -1;
    int uMeshTex2_ = -1, uTexMode2_ = -1, uUv2_ = -1;
    int drawableW_ = 0, drawableH_ = 0;
    std::unordered_set<unsigned> mipmapped_; // textures uploaded with a mip chain
    TextureFiltering filtering_;
    // A mipmapped texture's filtering (bound): GL_TEXTURE_MAX_ANISOTROPY (GL 4.6 /
    // EXT_texture_filter_anisotropic; clamped to the GPU's limit), the nearest texel magnified
    void filter(unsigned id) {
        gl::BindTexture(GL_TEXTURE_2D, id);
        if (maxAnisotropy() > 1) gl::TexParameteri(GL_TEXTURE_2D, 0x84FE, std::clamp(filtering_.anisotropy, 1, maxAnisotropy()));
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtering_.sharp ? GL_NEAREST : GL_LINEAR);
    }
    int anisoLimit_ = -1;
    Texture white_;
};

} // namespace

std::unique_ptr<RenderDevice> makeGlDevice() { return std::make_unique<GlDevice>(); }

} // namespace cr
