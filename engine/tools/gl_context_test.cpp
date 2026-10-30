// Exercise a real replacement context, including quad/mesh programs, texture
// units, generated mips, dynamic vertices and the CR2/3 material UBO.
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include "gfx/renderer.hpp"
#include "gfx/gl_backup.hpp"
namespace game = cr;
constexpr size_t stride = 24;

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
#ifdef CR_GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_Window* window = SDL_CreateWindow("GL recovery test", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    auto dev = game::makeGlDevice();
    if (!window || !dev->init(window)) {
        std::cerr << "No GL context: " << SDL_GetError() << '\n';
        dev->shutdown();
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;
    }
    int problems = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++problems; }
    };
    // Compression and raw fallback must round-trip exactly, including nonuniform data.
    for (size_t size : {size_t(0), size_t(4), size_t(65536)}) {
        std::vector<uint8_t> original(size), restored;
        for (size_t i=0; i<size; ++i) original[i] = uint8_t((i / 100) % 251);
        crgl::Backup backup;
        backup.store(original);
        check(backup.unpack(restored) && restored == original, "pixel backup round-trip");
    }
    game::Image image;
    image.width = image.height = 32;
    image.rgba.resize(32 * 32 * 4);
    for (size_t i=0; i<image.rgba.size(); i+=4) {
        image.rgba[i]=220; image.rgba[i+1]=40; image.rgba[i+2]=20; image.rgba[i+3]=255;
    }
    image.mipmaps = true;
    auto red = dev->upload(image);
    image.width = image.height = 1;
    image.rgba = {20, 210, 40, 255};
    image.mipmaps = false;
    auto green = dev->upload(image);
    auto deadTexture = dev->upload(image);
    const unsigned deadTextureId = deadTexture.id;
    dev->destroy(deadTexture);
    std::vector<uint8_t> vertices(3 * stride, 0);
    const float positions[] = {0,-1,0, 1,-1,0, 1,1,0};
    for (int i=0; i<3; ++i) {
        std::memcpy(vertices.data() + i*stride, positions+i*3, 12);
        std::memset(vertices.data() + i*stride + 20, 255, 4);
    }
    auto mesh = dev->upload(vertices, {0,1,2}, 0);
    auto deadMesh = dev->upload(vertices, {0,1,2}, 0);
    const unsigned deadMeshId = deadMesh.vao;
    dev->destroy(deadMesh);
    // Change geometry AFTER upload; recovery must use the current animated pose.
    const float moved = -1;
    std::memcpy(vertices.data() + 2*stride, &moved, 4);
    dev->updateVertices(mesh, vertices);
    const unsigned textureId=red.id, meshId=mesh.vao;
    const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float quad[24] = {-1,-1,0,0, 0,-1,1,0, 0,1,1,1, -1,-1,0,0, 0,1,1,1, -1,1,0,1};
    const float white[4] = {1,1,1,1};
    game::MeshShading shading;
    shading.texMode = 0;
    game::DrawState state;
    state.depthTest = false;
    dev->setVSync(false);
    game::TextureFiltering filtering;
    filtering.anisotropy = 10000; // must clamp on both upload and restore
    filtering.sharp = true;
    dev->setFiltering(filtering);
    auto frame = [&] {
        dev->beginFrame(64,64);
        dev->drawQuad(red,quad,identity,white);
        dev->drawMesh(mesh,&green,identity,shading,state);
        // Also create/use the untextured white fallback and secondary texture units.
        game::MeshShading untextured;
        untextured.color[0]=0.2f;
        untextured.color[1]=0.3f;
        untextured.color[2]=0.8f;
        float shifted[16];
        std::memcpy(shifted, identity, sizeof shifted);
        shifted[12] = 1.0f;
        dev->drawMesh(mesh,nullptr,shifted,untextured,state);
        std::vector<uint8_t> pixels;
        int w=0,h=0;
        check(dev->readPixels(pixels,w,h) && w==64 && h==64,"readback dimensions");
        dev->endFrame(false);
        check(dev->takeErrors()==0,"GL errors");
        return pixels;
    };
    const auto expected = frame();
    check(expected.size()==64*64*4 && expected[(32*64+8)*4]>150,"quad has visible red pixels");
    check(expected[(40*64+36)*4+1]>150,"mesh has visible green texture pixels");
    check(std::any_of(expected.begin(),expected.end(),[](uint8_t c){return c==204;}),"mesh has visible blue pixels");
    // Multiple losses: no old names may survive or collide with newly uploaded objects.
    for (int cycle=0; cycle<3; ++cycle) {
        SDL_GL_DestroyContext(SDL_GL_GetCurrentContext());
        SDL_GLContext replacement=SDL_GL_CreateContext(window);
        check(replacement != nullptr,"replacement context");
        if (!replacement) break;
        if (!dev->restoreAfterContextLoss()) {
            check(false,"resource restoration");
            check(frame()==expected,"rendering after failed recovery");
            // The previous implementation may still hold the destroyed context.
            // Dispose CPU state and the known current context without using that pointer.
            dev.reset();
            SDL_GL_DestroyContext(replacement);
            SDL_DestroyWindow(window);
            SDL_Quit();
            std::cout << "OpenGL context recovery: " << problems << " problems\n";
            return 1;
        }
        check(red.id==textureId && mesh.vao==meshId,"stable client handles");
        check(frame()==expected,"identical rendering after context replacement");
        auto freshTexture=dev->upload(image);
        auto freshMesh=dev->upload(vertices,{0,1,2},0);
        check(freshTexture.id!=deadTextureId && freshTexture.id!=red.id,"texture handle collision");
        check(freshMesh.vao!=deadMeshId && freshMesh.vao!=mesh.vao,"mesh handle collision");
        dev->destroy(freshTexture);
        dev->destroy(freshMesh);
        check(frame()==expected,"live resources survive transient resource destruction");
    }
    dev->destroy(red);
    dev->destroy(green);
    dev->destroy(mesh);
    dev->shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::cout << "OpenGL context recovery: " << problems << " problems\n";
    return problems ? 1 : 0;
}
