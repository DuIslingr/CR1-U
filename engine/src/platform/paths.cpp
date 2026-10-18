#include "platform/paths.hpp"

#include <SDL3/SDL.h>

#include <cstring>

namespace cr {

namespace {

std::string withSlash(std::string p) {
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
    return p;
}

// A directory counts as game data if it has a known file from the original app.
bool looksLikeData(const std::string& dir) {
    SDL_PathInfo info;
    return SDL_GetPathInfo((dir + "sys.evt").c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

std::string platformDefault() {
#ifdef SDL_PLATFORM_ANDROID
    // e.g. /storage/emulated/0/Android/data/<package>/files/dataHD — reachable over USB
    if (const char* ext = SDL_GetAndroidExternalStoragePath()) return withSlash(ext) + "dataHD/";
    return {};
#else
    const char* base = SDL_GetBasePath();
    return base ? withSlash(base) + "dataHD/" : std::string();
#endif
}

} // namespace

std::string findDataDir(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--data") == 0) {
            const std::string d = withSlash(argv[i + 1]);
            return looksLikeData(d) ? d : std::string();
        }
    }
    if (const char* env = SDL_getenv("CR_DATA")) {
        const std::string d = withSlash(env);
        if (looksLikeData(d)) return d;
    }
    const std::string d = platformDefault();
    if (!d.empty() && looksLikeData(d)) return d;
#ifdef SDL_PLATFORM_MACOS
    // a .app: dataHD/ beside it too (its base path is its Contents/Resources/)
    if (const char* base = SDL_GetBasePath()) {
        const std::string b = base;
        const size_t app = b.rfind(".app/");
        if (app != std::string::npos) {
            const std::string beside = b.substr(0, b.rfind('/', app) + 1) + "dataHD/";
            if (looksLikeData(beside)) return beside;
        }
    }
#endif
#ifndef SDL_PLATFORM_ANDROID
    // The working directory: a copied dataHD, the .app itself, or an unpacked .ipa - the iPad HD
    // release (ChaosRingsHD.app/dataHD) or the later voiced iPhone release (ChaosRings.app/data)
    for (const char* rel : {"dataHD/", "ChaosRingsHD.app/dataHD/", "Payload/ChaosRingsHD.app/dataHD/", "data/",
                            "ChaosRings.app/data/", "Payload/ChaosRings.app/data/"}) {
        char* cwd = SDL_GetCurrentDirectory();
        const std::string c = (cwd ? withSlash(cwd) : std::string()) + rel;
        SDL_free(cwd);
        if (looksLikeData(c)) return c;
    }
#endif
    return {};
}

std::string defaultDataDirHint() { return platformDefault(); }

std::string saveDir() {
    char* pref = SDL_GetPrefPath("preservation", "ChaosRingsHD");
    std::string out = pref ? withSlash(pref) : std::string("./");
    SDL_free(pref);
    return out;
}

} // namespace cr
