#include "gfx/gl.hpp"

#include <SDL3/SDL.h>

#ifndef APIENTRY
#define APIENTRY
#endif

namespace gl {

#ifdef CR_GLES
// GLES 3 is linked directly; point the table at the real entry points.
#define CR_GL_DEFINE(ret, name, args) ret(APIENTRY* name) args = ::gl##name;
CR_GL_FUNCTIONS(CR_GL_DEFINE)
#undef CR_GL_DEFINE

bool load() { return true; }
const char* shaderHeader() { return "#version 300 es\nprecision mediump float;\n"; }

#else

#define CR_GL_DEFINE(ret, name, args) ret(APIENTRY* name) args = nullptr;
CR_GL_FUNCTIONS(CR_GL_DEFINE)
#undef CR_GL_DEFINE

bool load() {
    bool ok = true;
#define CR_GL_LOAD(ret, name, args)                                                     \
    name = reinterpret_cast<ret(APIENTRY*) args>(SDL_GL_GetProcAddress("gl" #name));    \
    if (!name) {                                                                        \
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "missing GL function gl%s", #name);       \
        ok = false;                                                                     \
    }
    CR_GL_FUNCTIONS(CR_GL_LOAD)
#undef CR_GL_LOAD
    return ok;
}

const char* shaderHeader() { return "#version 330 core\n"; }

#endif

} // namespace gl
