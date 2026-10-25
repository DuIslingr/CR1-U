#include "platform/paths.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

namespace cr {

namespace {

constexpr const char* kProfile = "ChaosRingsHD"; // (the profile folder's name)

std::string withSlash(std::string p) {
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
    return p;
}

bool isDir(const std::string& p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

// A directory counts as game data if it has a known file from the original app.
bool looksLikeData(const std::string& dir) {
    SDL_PathInfo info;
    return SDL_GetPathInfo((dir + "sys.evt").c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

// the folders in `dir` (each with its separator), sorted
std::vector<std::string> subfolders(const std::string& dir) {
    std::vector<std::string> out;
    int count = 0;
    char** names = SDL_GlobDirectory(dir.c_str(), "*", 0, &count);
    for (int i = 0; names && i < count; ++i)
        if (isDir(dir + names[i])) out.push_back(withSlash(dir + names[i]));
    SDL_free(names);
    std::sort(out.begin(), out.end());
    return out;
}

// a game folder's iOS data: the folder itself, its dataHD/ (the iPad's) or data/ (the iPhone's)
std::string dataIn(const std::string& dir) {
    for (const char* sub : {"", "dataHD/", "data/"})
        if (looksLikeData(dir + sub)) return dir + sub;
    return {};
}

} // namespace

std::vector<std::string> gameFolders() {
    std::vector<std::string> out;
    auto add = [&](const std::string& p) {
        if (p.empty()) return;
        const std::string d = withSlash(p);
        if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    };
#ifdef SDL_PLATFORM_ANDROID
    if (const char* ext = SDL_GetAndroidExternalStoragePath()) add(ext);
#else
    const char* basePath = SDL_GetBasePath();
    const std::string base = basePath ? basePath : "";
#ifdef SDL_PLATFORM_LINUX
    if (const char* image = SDL_getenv("APPIMAGE")) add(std::string(image).substr(0, std::string(image).rfind('/') + 1));
#endif
#ifdef SDL_PLATFORM_MACOS
    if (const size_t app = base.rfind(".app/"); app != std::string::npos) add(base.substr(0, base.rfind('/', app) + 1));
#endif
    add(base);
    if (char* cwd = SDL_GetCurrentDirectory()) add(cwd), SDL_free(cwd);
    add(saveDir()); // (the profile folder: where the first start's setup copies the game when the game folder can't be written)
#endif
    return out;
}

std::vector<std::string> releaseFolders(const char* kind) {
    std::vector<std::string> out;
    for (const std::string& g : gameFolders()) {
        const std::string top = g + kind + "/";
        if (!isDir(top)) continue;
        out.push_back(top);
        for (const std::string& a : subfolders(top)) {
            out.push_back(a);
            for (const std::string& b : subfolders(a)) out.push_back(b);
        }
    }
    return out;
}

std::string findDataDir(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--data") == 0) {
            const std::string d = withSlash(argv[i + 1]);
            return looksLikeData(d) ? d : dataIn(d);
        }
    }
    if (const char* env = SDL_getenv("CR_DATA")) {
        const std::string d = dataIn(withSlash(env));
        if (!d.empty()) return d;
    }
    for (const std::string& r : releaseFolders("ios"))
        if (const std::string d = dataIn(r); !d.empty()) return d;
    { // (the profile folder's ios/: where the first start's setup unpacks an .ipa when the game folder can't be written)
        const std::string top = saveDir() + "ios/";
        if (isDir(top)) {
            std::vector<std::string> tries = {top};
            for (const std::string& a : subfolders(top)) {
                tries.push_back(a);
                for (const std::string& b : subfolders(a)) tries.push_back(b);
            }
            for (const std::string& t : tries)
                if (const std::string d = dataIn(t); !d.empty()) return d;
        }
    }
    // the older places: dataHD/ or data/ in a game folder, the .app there, an unpacked .ipa's Payload/
    for (const std::string& g : gameFolders()) {
        if (const std::string d = dataIn(g); !d.empty()) return d;
        std::vector<std::string> apps;
        for (const std::string& top : {g, g + "Payload/"})
            for (const std::string& s : subfolders(top))
                if (s.size() > 5 && s.compare(s.size() - 5, 5, ".app/") == 0) apps.push_back(s);
        for (const std::string& a : apps)
            if (const std::string d = dataIn(a); !d.empty()) return d;
    }
    return {};
}

std::string defaultDataDirHint() {
    const std::vector<std::string> g = gameFolders();
    return g.empty() ? std::string("ios/") : g.front() + "ios/";
}

std::string texturesFolder(const char* which) {
    const std::vector<std::string> g = gameFolders();
    return (g.empty() ? std::string() : g.front()) + "textures/" + which + "/";
}

std::string modelsFolder(const char* which) {
    const std::vector<std::string> g = gameFolders();
    return (g.empty() ? std::string() : g.front()) + "models/" + which + "/";
}

std::string saveDir() {
    char* pref = SDL_GetPrefPath("preservation", kProfile);
    std::string out = pref ? withSlash(pref) : std::string("./");
    SDL_free(pref);
    return out;
}

} // namespace cr
