#include "app/app.hpp"

namespace crapp {

void holdSkip(App& app, unsigned source, bool held) {
    app.skipHeld = held ? app.skipHeld | source : app.skipHeld & ~source;
    app.skipScene = app.skipHeld != 0;
}
void clearSkip(App& app) {
    app.skipHeld = 0;
    app.skipScene = false;
}
// The skip button's corner, top right, in logical units
bool skipCorner(const App& app, float lx, float ly) {
    return lx >= app.renderer.layoutRight() - 76 && ly < app.renderer.layoutTop() + 72;
}

// _mdl_put: a texture of MCT id 999 (c105a's and m105a's) turns about the texture's origin -
// mceAppearance_setRotateEuler(unit 0, XYZ, (0, 0, _vEuler z)) - and z takes 0.0036 turn off for
// every model it draws, wrapping below 0 (so the more models are on screen, the faster it turns)
constexpr int kSpinTexture = 999;
bool spinsTexture(const cr::Model& m, size_t mesh) { // (its appearance's unit 0: _mceAppearance_getTexture(.., 0))
    const int ap = mesh < m.meshes.size() ? m.meshes[mesh].appearance : -1;
    return ap >= 0 && ap < int(m.materials.size()) && m.materials[size_t(ap)].textureRef == kSpinTexture;
}
void spinUvs(std::vector<float>& uv, float turns) {
    const float a = turns * 6.28318530718f, c = std::cos(a), sn = std::sin(a);
    for (size_t k = 0; k + 1 < uv.size(); k += 2) {
        const float u = uv[k], v = uv[k + 1];
        uv[k] = c * u - sn * v, uv[k + 1] = sn * u + c * v;
    }
}
void modelDrawn(App& app) {
    app.texSpin -= 0.0036f * app.drawFrames;
    while (app.texSpin < 0) app.texSpin += 1;
}

void menuSound(App& app, uint32_t id) {
    app.sound.play(id);
    if (app.menuSoundLog.size() >= 64) app.menuSoundLog.erase(app.menuSoundLog.begin());
    app.menuSoundLog.push_back(id);
}

bool endsWith(const char* s, const char* ext) {
    const size_t n = SDL_strlen(s), e = SDL_strlen(ext);
    return n > e && SDL_strcasecmp(s + n - e, ext) == 0;
}

// Checks the package table without reading the whole file (the Vita's: read whole).
bool isModelFile(const App& app, const std::string& name) {
    if (app.vitaOnly || app.androidOnly) {
        std::vector<uint8_t> f;
        if (!(app.vitaOnly ? app.vita.read(name, f) : app.android.read(name, f)) || f.size() < 8) return false;
        uint32_t off = 0;
        std::memcpy(&off, f.data(), 4);
        return off + 4 <= f.size() && std::memcmp(&f[off], "MCM_", 4) == 0;
    }
    const std::string path = app.dataDir + name;
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return false;
    Uint32 off = 0;
    char magic[4] = {};
    const bool ok = SDL_ReadU32LE(io, &off) && SDL_SeekIO(io, off, SDL_IO_SEEK_SET) == Sint64(off) &&
                    SDL_ReadIO(io, magic, 4) == 4 && SDL_memcmp(magic, "MCM_", 4) == 0;
    SDL_CloseIO(io);
    return ok;
}

SDL_EnumerationResult SDLCALL collectAssets(void* user, const char*, const char* fname) {
    App& app = *static_cast<App*>(user);
    if (endsWith(fname, ".mct")) app.textures.push_back(fname);
    else if (endsWith(fname, ".mca")) app.animFiles.push_back(fname);
    else if (endsWith(fname, ".cmp")) app.maps.push_back(fname);
    else if (endsWith(fname, ".bin") && isModelFile(app, fname)) app.models.push_back(fname);
    return SDL_ENUM_CONTINUE;
}

int wrap(int i, size_t n) { return n ? int(((i % int(n)) + int(n)) % int(n)) : 0; }

void updateTitle(App& app) {
    std::string title = "Chaos Rings — ";
    if (app.mode == Mode::Textures && !app.textures.empty()) {
        title += app.textures[app.texIndex] + "  (" + std::to_string(app.currentTex.width) + "x" +
                 std::to_string(app.currentTex.height) + " " + cr::mctFormatName(app.texInfo.format) + ")  " +
                 std::to_string(app.texIndex + 1) + "/" + std::to_string(app.textures.size());
    } else if (app.mode == Mode::Models && !app.models.empty()) {
        const LoadedModel& m = app.model;
        title += app.models[app.modelIndex] + "  (" + std::to_string(m.meshes.size()) + " meshes, " +
                 std::to_string(m.triangles) + " tris, " + (app.zUp ? "Z-up" : "Y-up") +
                 (app.smoothFilter ? ", smooth" : "") + ")  " +
                 std::to_string(app.modelIndex + 1) + "/" + std::to_string(app.models.size());
        if (m.action < 0) {
            title += "  |  rest pose (" + std::to_string(m.actions.size()) + " animations)";
        } else {
            const cr::Action& a = m.actions[m.action];
            title += "  |  anim " + std::to_string(m.action + 1) + "/" + std::to_string(m.actions.size()) + " " +
                     a.name + ", " + std::to_string(int(a.duration)) + " frames" +
                     (app.playing ? "" : " [paused]") +
                     (app.speed != 1.0f ? "  x" + std::to_string(app.speed).substr(0, 4) : "");
        }
    }
    if (app.mode == Mode::Maps && !app.maps.empty()) {
        title = "Chaos Rings — " + app.maps[app.mapIndex] + "  (" + std::to_string(app.collisionPolys) +
                " collision polygons" + (app.mapProps.empty() ? "" : ", props " + app.mapProps) +
                (app.showCollision ? "" : ", collision hidden") +
                (app.showBackground ? (app.background.id ? ", background" : ", no background") : "") +
                (app.showBackground && app.foreground.id ? " + foreground" : "") +
                (app.showNpcs ? ", " + std::to_string(app.npcDraws.size()) + " npcs" : ", npcs hidden") +
                (app.havePlayer ? ", player " + app.playerName.substr(0, 4)
                                : app.showPlayer ? "" : ", player hidden (P)") + ")  " +
                (app.area >= 0 ? std::string(cr::kAreaEventFile[app.area]).substr(0, 8) + " map " +
                                     std::to_string(app.mapNumber) + "  " : "") +
                (app.lastWarp.empty() ? "" : app.lastWarp + "  ") +
                (app.lastEvent.empty() ? "" : app.lastEvent + "  ") +
                (app.haveStageCam
                     ? "cam: map " + std::to_string(app.stageCam.mapNumber) + " (" + app.stageCam.eventFile + ", " +
                           std::to_string(app.stageCamIndex + 1) + "/" + std::to_string(app.stageCams.size()) +
                           ", fits " + std::to_string(int(app.stageFit[app.stageCamIndex] * 100 + 0.5f)) + "%" +
                           (app.stageCamBorrowed ? ", borrowed" : "") + ") fov " +
                           std::to_string(int(app.stageCam.fovDegrees + 0.5f)) + "  "
                     : "no camera  ") + std::to_string(app.mapIndex + 1) + "/" +
                std::to_string(app.maps.size());
    }
    SDL_SetWindowTitle(app.window, title.c_str());
    app.status = title.substr(std::string("Chaos Rings — ").size());
    SDL_Log("%s", title.c_str());
}

void showTexture(App& app, int index) {
    if (app.textures.empty()) return;
    app.texIndex = wrap(index, app.textures.size());
    app.renderer.destroy(app.currentTex);
    const std::string& name = app.textures[app.texIndex];
    std::vector<uint8_t> data;
    cr::Image img;
    std::string err;
    if (!cr::readFile(app.dataDir + name, data) || !cr::decodeMct(data, img, &app.texInfo, &err)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", name.c_str(), err.c_str());
        return;
    }
    app.currentTex = app.renderer.upload(img);
    if (app.mode == Mode::Textures) updateTitle(app); // startup loads a texture before --maps/--models
}

// With HD textures on: the upscaled replacement for `img` (found by its pixels' hash), drawn at
// the original's size with a mip chain
void applyHd(App& app, cr::Image& img) {
    if (!app.video.hdTextures || app.hdDir.empty() || img.width <= 0) return;
    if (const char* only = SDL_getenv("CR_HD_ONLY"); only && cr::hashName(cr::imageHash(img)) != only) return; // (debugging: one replacement alone)
    cr::Image hd;
    std::string why;
    if (!cr::loadHdImage(app.hdDir, img, hd, &why)) {
        if (!why.empty() && app.hdRefused.insert(why).second) SDL_Log("hd textures: %s - left out", why.c_str()); // (one that doesn't fit: said once)
        ++app.hdMissing;
        return;
    }
    if (SDL_getenv("CR_HD_LOG")) std::printf("hd %s %dx%d -> %dx%d\n", cr::hashName(cr::imageHash(img)).c_str(), img.width, img.height, hd.width, hd.height);
    hd.logicalW = img.width;
    hd.logicalH = img.height;
    hd.noAlpha = img.noAlpha;
    { // (a cut-out: its alpha only 0 and 255, some of it 0)
        bool binary = !img.alphaOnly, some = false;
        for (size_t i = 3; binary && i < img.rgba.size(); i += 4) {
            const uint8_t a = img.rgba[i];
            binary = a == 0 || a == 255;
            some |= a == 0;
        }
        hd.cutout = binary && some;
    }
    if (hd.cutout) cr::fillSeeThrough(hd); // (only a cut-out: soft alpha keeps its own colours)
    hd.mipmaps = true;
    img = std::move(hd);
    ++app.hdLoaded;
}


// A full-image texture by its .mct name (loadOriginalImage), its HD replacement when that's on
bool loadImage(App& app, const std::string& mct, cr::Image& img, bool* fromAndroid) {
    if (!loadOriginalImage(app, mct, img, fromAndroid)) return false;
    applyHd(app, img);
    return true;
}

// An MCT's size from its header alone (no decoding)
bool mctHeader(const std::vector<uint8_t>& f, cr::MctInfo& mi) {
    if (f.size() < 0x2b || SDL_memcmp(f.data(), "MCT_", 4) != 0) return false;
    mi.width = f[0x22] | f[0x23] << 8, mi.height = f[0x24] | f[0x25] << 8;
    return true;
}

// A full-image texture by its .mct name: the Vita's lossless .gxt of the same name when --vita is
// given (same sizes; 8 bits per channel where the iPad files use palettes, PVRTC or RGBA4444),
// else the data folder's .mct.
bool loadOriginalImage(App& app, const std::string& mct, cr::Image& img, bool* fromAndroid) {
    if (fromAndroid) *fromAndroid = false;
    // Art the Vita redrew for its own screen layout (crtool vita: > 12/255 from the iPad's) keeps
    // the iPhone / iPad version
    static const char* const kVitaRedrawn[] = {"MAPtitle_hd.mct", "chaos_hd.mct", "chara1_hd.mct", "chara2_hd.mct",
                                               "chara3_hd.mct", "daimei01_hd.mct", "daimei02_hd.mct", "title1_hd.mct",
                                               "title2_hd.mct", "t0107-b00a00.mct"};
    const bool redrawn = std::any_of(std::begin(kVitaRedrawn), std::end(kVitaRedrawn), [&](const char* n) { return mct == n; });
    if (app.vita.isOpen() && mct.size() > 4 && (!redrawn || app.vitaOnly)) { // (alone: its own screens too)
        std::vector<uint8_t> g;
        if (app.vita.read(mct.substr(0, mct.size() - 4) + ".gxt", g) && cr::decodeGxt(g, img)) { ++app.vitaTextures; return true; }
    }
    std::vector<uint8_t> data;
    // the Android copy of the same name: when the data has none, or one of the same size (the same
    // layout), as its palettes beat the iPad's PVRTC
    if (!app.vitaOnly && !app.androidOnly && app.android.isOpen() && app.android.read(mct, data)) {
        std::vector<uint8_t> mine;
        const bool own = cr::readFile(app.dataDir + mct, mine);
        cr::Image a;
        cr::MctInfo ai, mi;
        if (cr::decodeMct(data, a, &ai) && (!own || (mctHeader(mine, mi) && mi.width == ai.width && mi.height == ai.height))) {
            img = std::move(a);
            if (fromAndroid) *fromAndroid = true;
            ++app.androidTextures;
            return true;
        }
    }
    const bool ok = cr::readFile(app.dataDir + mct, data) && cr::decodeMct(data, img);
    if (fromAndroid) *fromAndroid = ok && app.androidOnly;
    return ok;
}

// An iOS data file's names in the Vita's cr1.psarc: x.mct -> x.gxt (and x.gxt for x_hd.mct, which
// the Vita names without the suffix: EndBack01, i001_C00, syuutyuusen), anything else as it is
std::vector<std::string> vitaNames(const std::string& name) {
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".mct") != 0) return {name};
    const std::string base = name.substr(0, name.size() - 4);
    std::vector<std::string> out = {base + ".gxt"};
    if (base.size() > 3 && base.compare(base.size() - 3, 3, "_hd") == 0) out.push_back(base.substr(0, base.size() - 3) + ".gxt");
    return out;
}

// Opens the Vita copy at `path` (its PCSG00497 folder, CR1, resources or cr1.psarc itself): the
// textures and models (loadOriginalImage, loadModelFile), the scripts through the data folder's
// overlay, and the sound folder's archives (hooked up by vitaAudio once the sound starts)
bool openVita(App& app, const std::string& path) {
    std::string err;
    std::string root = path;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    for (const char* rel : {"", "/CR1/resources/cr1.psarc", "/resources/cr1.psarc", "/cr1.psarc"})
        if (app.vita.open(root + rel, &err)) {
            const std::string p = root + rel; // the sound folder sits beside resources/
            const size_t res = p.rfind("/resources/");
            root = res == std::string::npos ? p.substr(0, p.rfind('/')) : p.substr(0, res);
            break;
        }
    if (!app.vita.isOpen()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "vita: %s (%s)", err.c_str(), path.c_str());
        return false;
    }
    app.vitaPath = app.vitaLoaded = path;
    app.vitaRoot = root;
    if (app.vitaOnly) app.dataDir = root + "/resources/"; // (the folder of cr1.psarc: the data's own paths)
    for (const char* sound : {"/sound/", "/../sound/"}) {
        if (app.vitaBgm.open(root + sound + "bgm.psarc")) {
            app.vitaSound = root + sound;
            app.vitaVoice[0].open(root + sound + "voiceevnt.psarc");
            app.vitaVoice[1].open(root + sound + "voicebtl.psarc");
            break;
        }
    }
    // Every script, its text and voice cues: 2.2.0's code to the byte but for j001_t99's boot (return
    // entry 0), the Vita's own start-up - its logos through opcodes 0x47..0x4a (played), its save
    // and trophy housekeeping (0x40..0x46, nothing here)
    // Alone (no iOS data) every file comes from it: textures by their .gxt (decodeMct takes the
    // Vita's form), an iOS "_hd" name the Vita lacks by the name without it, the boot area's script
    // too (the event runner passes its own opcodes by)
    if (app.vitaOnly)
        cr::setDataOverlay(app.dataDir, [&app](const std::string& name, std::vector<uint8_t>& out) {
            for (const std::string& n : vitaNames(name))
                if (app.vita.read(n, out)) return true;
            return false;
        });
    else
        cr::setDataOverlay(app.dataDir, [&app](const std::string& name, std::vector<uint8_t>& out) {
            return name.size() > 4 && name.compare(name.size() - 4, 4, ".evt") == 0 && app.vita.read(name, out);
        });
    SDL_Log("vita: %zu files in %s; music %s, voices %zu / %zu", app.vita.size(), path.c_str(), app.vitaBgm.isOpen() ? "yes" : "no",
            app.vitaVoice[0].size(), app.vitaVoice[1].size());
    return true;
}

// The sound effects: the data's bank, or the Vita's when it is the whole game
bool startSound(App& app) {
    return app.sound.init(app.dataDir, app.vitaOnly && !app.vitaSound.empty() ? app.vitaSound + "sndeff.bnk" : std::string());
}

// The Vita's voices and music to the sound system and the music player
void vitaAudio(App& app) {
    for (int b = 0; b < 2; ++b) {
        if (!app.vitaVoice[b].isOpen()) continue;
        const cr::PsarcArchive* a = &app.vitaVoice[b];
        app.sound.setVitaLines(b == 1, a->names(), [a](const std::string& n, std::vector<uint8_t>& out) { return a->read(n, out); });
    }
    if (app.vitaBgm.isOpen()) {
        const cr::PsarcArchive* a = &app.vitaBgm;
        app.music.setVitaSource([a](const std::string& n, std::vector<uint8_t>& out) { return a->read(n, out); });
    }
}


// Options' Vita Assets: the folder dialog answers on its own thread; the menu takes it next frame
std::mutex vitaPickMutex;
std::string vitaPicked;
bool vitaPickedReady = false;

void chooseVitaFolder(App& app) {
    SDL_ShowOpenFolderDialog(
        [](void*, const char* const* list, int) {
            if (!list || !list[0]) return; // cancelled or failed
            std::lock_guard<std::mutex> lock(vitaPickMutex);
            vitaPicked = list[0];
            vitaPickedReady = true;
        },
        nullptr, app.window, app.vitaPath.empty() ? nullptr : app.vitaPath.c_str(), false);
}

void takeVitaFolder(App& app) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(vitaPickMutex);
        if (!vitaPickedReady) return;
        vitaPickedReady = false;
        path = vitaPicked;
    }
    cr::PsarcArchive probe;
    bool found = false;
    for (const char* rel : {"", "/CR1/resources/cr1.psarc", "/resources/cr1.psarc", "/cr1.psarc"})
        if ((found = probe.open(path + rel))) break;
    if (!found) { app.vitaNote = "No Vita data (CR1/resources/cr1.psarc) in that folder"; return; }
    app.vitaPath = path;
    app.vitaNote = path == app.vitaLoaded ? "In use" : "Used from the next start: " + path;
    saveMix(app);
}

// The game's own expansion file in `path` (a folder, or the .obb itself) and the APK (or
// classes.dex) beside it whose table adds up to it - not another release's in the same folder
constexpr const char* kObbPackage = ".com.square_enix.chaosrings.googleplay.obb";
bool findAndroid(const std::string& path, cr::ObbArchive& out, std::string* error) {
    std::string dir = path, obb;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    SDL_PathInfo info;
    if (SDL_GetPathInfo(dir.c_str(), &info) && info.type == SDL_PATHTYPE_FILE) obb = dir, dir = dir.substr(0, dir.rfind('/'));
    int count = 0;
    char** files = SDL_GlobDirectory(dir.c_str(), "*", 0, &count);
    std::vector<std::string> tables;
    for (int i = 0; files && i < count; ++i) {
        const std::string n = files[i];
        const auto ends = [&](const std::string& sfx) {
            return n.size() > sfx.size() && SDL_strcasecmp(n.c_str() + n.size() - sfx.size(), sfx.c_str()) == 0;
        };
        if (obb.empty() && n.compare(0, 5, "main.") == 0 && ends(kObbPackage)) obb = dir + "/" + n;
        if (ends(".apk") || n == "classes.dex") tables.push_back(dir + "/" + n);
    }
    SDL_free(files);
    if (obb.empty()) { if (error) *error = "no main.*" + std::string(kObbPackage) + " in " + dir; return false; }
    std::string why = "no APK beside it";
    for (const std::string& t : tables)
        if (out.open(obb, t, &why)) return true;
    if (error) *error = obb + ": " + why;
    return false;
}

bool openAndroid(App& app, const std::string& path) {
    std::string err;
    if (!findAndroid(path, app.android, &err)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "android: %s", err.c_str());
        return false;
    }
    app.androidPath = app.androidLoaded = path;
    app.music.setAndroidSource([&app](const std::string& n, std::vector<uint8_t>& out) { return app.android.read(n, out); });
    if (app.androidOnly) {
        // Alone (no iOS data) every file comes from it, by its own name, or an iPad-only "_hd"
        // name by the one without (EndBack01, chara3, i001_C00, syuutyuusen, icon); the voice
        // banks streamed from inside the OBB
        const std::string obb = app.android.path();
        app.dataDir = obb.substr(0, obb.rfind('/') + 1);
        cr::setDataOverlay(app.dataDir, [&app](const std::string& name, std::vector<uint8_t>& out) {
            if (app.android.read(name, out)) return true;
            if (name.size() > 7 && name.compare(name.size() - 7, 7, "_hd.mct") == 0)
                return app.android.read(name.substr(0, name.size() - 7) + ".mct", out);
            return false;
        });
        app.sound.setLocator([&app](const std::string& name, std::string& file, uint64_t& offset, uint64_t& size) {
            uint32_t n = 0;
            if (!app.android.locate(name, offset, n)) return false;
            file = app.android.path(), size = n;
            return true;
        });
    }
    SDL_Log("android: %zu files in %s", app.android.size(), app.android.path().c_str());
    return true;
}

std::mutex androidPickMutex;
std::string androidPicked;
bool androidPickedReady = false;

void chooseAndroidFolder(App& app) {
    SDL_ShowOpenFolderDialog(
        [](void*, const char* const* list, int) {
            if (!list || !list[0]) return;
            std::lock_guard<std::mutex> lock(androidPickMutex);
            androidPicked = list[0];
            androidPickedReady = true;
        },
        nullptr, app.window, app.androidPath.empty() ? nullptr : app.androidPath.c_str(), false);
}

void takeAndroidFolder(App& app) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(androidPickMutex);
        if (!androidPickedReady) return;
        androidPickedReady = false;
        path = androidPicked;
    }
    cr::ObbArchive probe;
    std::string err;
    if (!findAndroid(path, probe, &err)) { app.androidNote = err; return; }
    app.androidPath = path;
    app.androidNote = path == app.androidLoaded ? "In use" : "Used from the next start: " + path;
    saveMix(app);
}


// settings.ini's vita= / android= (the whole line: a path), read before the data loads
std::string storedVitaPath() { return storedSetting("vita="); }
// settings.ini's line `key` (with its '=') set to `value`, the others kept (the first start's setup)
void storeSetting(const char* key, const std::string& value) {
    size_t size = 0;
    char* text = static_cast<char*>(SDL_LoadFile(settingsPath().c_str(), &size));
    std::string all = text ? std::string(text, size) : std::string(), out;
    SDL_free(text);
    const size_t kl = SDL_strlen(key);
    bool set = false;
    for (size_t at = 0; at < all.size();) {
        const size_t e = std::min(all.find('\n', at), all.size());
        if (all.compare(at, kl, key) == 0) out += key + value + "\n", set = true;
        else out += all.substr(at, e - at) + "\n";
        at = e + 1;
    }
    if (!set) out += key + value + "\n";
    std::string error;
    if (!crstorage::replaceFile(settingsPath(), out, &error)) SDL_Log("settings: %s", error.c_str());
}
std::string storedSetting(const char* key) {
    size_t size = 0;
    char* text = static_cast<char*>(SDL_LoadFile(settingsPath().c_str(), &size));
    if (!text) return {};
    std::string all(text, size), path;
    SDL_free(text);
    const size_t kl = SDL_strlen(key);
    for (size_t at = 0; at < all.size();) {
        const size_t e = std::min(all.find('\n', at), all.size());
        if (all.compare(at, kl, key) == 0) path = all.substr(at + kl, e - at - kl);
        at = e + 1;
    }
    while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) path.pop_back();
    return path;
}

void unloadLoaded(App& app, LoadedModel& lm) {
    for (auto& m : lm.meshes) app.renderer.destroy(m);
    for (auto& t : lm.textures) app.renderer.destroy(t);
    lm = {};
}

void unloadModel(App& app) {
    app.renderer.destroy(app.background);
    app.renderer.destroy(app.foreground);
    app.renderer.destroy(app.collision);
    app.renderer.destroy(app.triggerMesh);
    app.renderer.destroy(app.puzzleBeams);
    app.renderer.destroy(app.battleShadows);
    app.renderer.destroy(app.fieldShadows);
    app.collisionPolys = 0;
    unloadLoaded(app, app.model);
}

// Loads a model package (and its fitting animations) into `out`.
// The package's actions a model keeps: those with one segment per node; a list's motion ("N.1") a few
// segments short; and one with empty segments past the last node, segment i still on node i (c111's
// warp crystal spin: 13 for 12 nodes, segment 3 the crystal). (m104's, m209's and m220's 149..150-
// segment lists don't fit that way; their battle slots are ones _btlanmtbl never loads.)
std::vector<bool> packageActionsKept(const cr::Model& m) {
    std::vector<bool> keep(m.actions.size());
    for (size_t i = 0; i < m.actions.size(); ++i) {
        const cr::Action& a = m.actions[i];
        const bool motion = a.name.size() > 2 && a.name.compare(a.name.size() - 2, 2, ".1") == 0;
        keep[i] = cr::actionFits(m.skeleton, a) || (motion && cr::actionNearlyFits(m.skeleton, a)) ||
                  cr::emptyTail(m.skeleton, a);
    }
    return keep;
}

void loadModelFile(App& app, const std::string& name, LoadedModel& out, bool character) {
    std::vector<uint8_t> data;
    cr::Model m;
    std::string err;
    // --vita: the Vita's package of the same name (same meshes, lossless textures)
    const bool fromVita = app.vita.isOpen() && app.vita.read(name, data) && cr::loadModelPackage(data, m, &err);
    if (fromVita) ++app.vitaTextures;
    else if (!cr::readFile(app.dataDir + name, data) || !cr::loadModelPackage(data, m, &err)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", name.c_str(), err.c_str());
        updateTitle(app);
        return;
    }
    // --android: the Android package's textures (8-bit palettes where the iPad's are PVRTC) when
    // it holds as many of the same sizes - the meshes stay the data's
    if (!fromVita && app.android.isOpen()) {
        std::vector<uint8_t> ad;
        cr::Model am;
        if (app.android.read(name, ad) && cr::loadModelPackage(ad, am, nullptr) && am.textures.size() == m.textures.size() &&
            !m.textures.empty()) {
            bool same = true;
            for (size_t i = 0; i < m.textures.size(); ++i)
                same &= am.textures[i].width == m.textures[i].width && am.textures[i].height == m.textures[i].height;
            if (same) m.textures = std::move(am.textures), ++app.androidTextures;
        }
    }
    // HD Models: models/hd/<name>.glb (or .gltf) in place of the package's meshes - its skeleton,
    // motions and materials kept (assets/gltf_model.hpp)
    bool replaced = false;
    if (app.video.hdModels && !app.modelsDir.empty()) {
        const std::string base = app.modelsDir + name.substr(0, name.size() - 4);
        for (const char* ext : {".glb", ".gltf"}) {
            const std::string path = base + ext;
            if (!SDL_GetPathInfo(path.c_str(), nullptr)) continue;
            std::string why;
            cr::GltfImportStats gs;
            if (cr::applyGltfReplacement(m, path, &why, &gs)) {
                replaced = true;
                ++app.hdModelsLoaded;
                SDL_Log("%s: replaced by %s (%d meshes, %d triangles%s%s)", name.c_str(), path.c_str(), gs.meshes, gs.triangles,
                        gs.newTextures ? ", pictures of its own" : "", gs.unmatchedJoints ? ", joints not in the skeleton on its root" : "");
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s left out: %s", name.c_str(), path.c_str(), why.c_str());
            }
            break;
        }
    }
    // Smooth Models: every model - characters, NPCs, monsters, field props and battle stages (a
    // replacement as its maker built it)
    (void)character;
    if (!replaced && app.video.smoothModels >= 2) cr::smoothModel(m, app.video.smoothModels);
    for (const auto& img : m.textures) {
        if (app.video.hdTextures) {
            cr::Image hd = img;
            applyHd(app, hd);
            out.textures.push_back(app.renderer.upload(hd));
        } else {
            out.textures.push_back(app.renderer.upload(img));
        }
    }
    for (const auto& mesh : m.meshes) out.meshes.push_back(app.renderer.upload(mesh));
    out.triangles = m.stats.triangles;

    // Appearances that name a package action (refGid: segment 1 colour, 6 / 8 texture motion) -
    // the scenes' effect props (ee*, fe*): keep every package action for them, as effects do
    for (const auto& mat : m.materials)
        if (int32_t(mat.refGid) >= 0) { out.packageActions = m.actions; break; }
    // Animations: those stored in the package plus standalone <name>e*.mca files,
    // keeping only actions that animate this skeleton (one segment per node)
    // (or, for each list's motion - its first action, "N.1" - one a few segments off)
    const std::vector<bool> keep = packageActionsKept(m);
    for (size_t ai = 0; ai < m.actions.size(); ++ai) {
        auto& a = m.actions[ai];
        const bool fits = keep[ai];
        if (SDL_getenv("CR_FIT_DEBUG") && !cr::actionFits(m.skeleton, a)) {
            float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            if (fits) { // its pose halfway through: the bounds stay the model's size when the nodes line up
                std::vector<cr::Mat4> w;
                std::vector<float> sk;
                cr::computeWorld(m.skeleton, &a, a.duration / 2, w);
                for (const auto& mesh : m.meshes) {
                    cr::skinMesh(m, mesh, w, sk);
                    for (size_t k = 0; k + 2 < sk.size(); k += 3)
                        for (int c = 0; c < 3; ++c) lo[c] = std::min(lo[c], sk[k + c]), hi[c] = std::max(hi[c], sk[k + c]);
                }
            }
            std::printf("%s: %s (%zu segments, %zu nodes) %s", name.c_str(), a.name.c_str(), a.segments.size(),
                        m.skeleton.nodes.size(), fits ? "kept" : "left out");
            if (fits) std::printf(", halfway (%.1f %.1f %.1f)-(%.1f %.1f %.1f)", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            std::printf("\n");
        }
        if (fits) out.actions.push_back(std::move(a));
    }
    out.builtin = int(out.actions.size());
    const std::string stem = name.substr(0, name.size() - 4);
    for (const auto& file : app.animFiles) {
        if (file.rfind(stem + "e", 0) != 0) continue;
        std::vector<uint8_t> ad;
        std::vector<cr::Action> acts;
        if (!cr::readFile(app.dataDir + file, ad) || !cr::parseMca(ad.data(), ad.size(), 0, acts)) continue;
        for (size_t i = 0; i < acts.size(); ++i) {
            if (!cr::actionFits(m.skeleton, acts[i])) continue;
            acts[i].name = file.substr(0, file.size() - 4) + "." + std::to_string(i + 1);
            out.actions.push_back(std::move(acts[i]));
        }
    }
    m.actions.clear();
    out.action = out.actions.empty() ? -1 : 0;
    out.cpu = std::move(m);
    const cr::Model& cpu = out.cpu;
    const auto& b0 = cpu.boundsMin;
    const auto& b1 = cpu.boundsMax;
    out.center = {(b0[0] + b1[0]) / 2, (b0[1] + b1[1]) / 2, (b0[2] + b1[2]) / 2};
    const cr::Vec3 ext{b1[0] - b0[0], b1[1] - b0[1], b1[2] - b0[2]};
    out.radius = std::max(0.5f * std::sqrt(cr::dot(ext, ext)), 0.01f);
}

// What CR1's loaders do to a model's Appearances, by the path that reads it (2.2.0, the same in 1.0.1):
// read_mdl_mem_mcm (0x7664: fldread_prc's read_mdl_set -> mdl_mdl_init - the alpha model, and the
// leader on a map's load) clears property bit 1 alone and sets every alpha test to 1: lighting (bit
// 0) stays as the file has it - unlit (hwalSetAppearanceColor_ES1: glColor = colour 1) where it is
// clear; the port has no field lights, so a lit one keeps the old approximation
void mdlMemRules(LoadedModel& lm) {
    for (cr::Material& mt : lm.cpu.materials) {
        mt.unlit = (mt.properties & 1) ? 0 : 1;
        mt.alphaThreshold = 1;
    }
}

// read_mdl (0x813c: npc_setup, Set_Prtyno, __EvtNpcMdlRead) clears bits 0 and 1 - unlit, glColor =
// colour 1 as the file has it (CR1 sets no colour, unlike Omega's white) -; d0552bt's draw order 900
// -> 0, others above 0 less 2000; d0451bt's 400 -> -400; d0555bt left as it is; c302..c306
// alpha-blended (blend mode 0), no alpha test, no depth writes; every other model's alpha test 1; a
// texture in pixel format 1 or 6 (RGB888, PAL8_RGB8: no alpha) no alpha test; c321's textured ones
// no depth writes
void readMdlRules(LoadedModel& lm, const std::string& name) {
    cr::Model& m = lm.cpu;
    const bool glass = name == "c302.bin" || name == "c303.bin" || name == "c304.bin" || name == "c305.bin" || name == "c306.bin";
    for (cr::Material& mt : m.materials) {
        mt.unlit = 1;
        if (name == "d0552bt.bin") mt.drawOrder = mt.drawOrder == 900 ? 0 : mt.drawOrder > 0 ? int16_t(mt.drawOrder - 2000) : mt.drawOrder;
        else if (name == "d0451bt.bin") mt.drawOrder = mt.drawOrder == 400 ? -400 : mt.drawOrder;
        else if (name == "d0555bt.bin") {}
        else if (glass) mt.alphaThreshold = 0, mt.blendSrc = 4, mt.blendDst = 5, mt.properties &= ~16u;
        else mt.alphaThreshold = 1;
        if (mt.textureRef < 0) continue; // (mceAppearance_getTexture: unit 0's)
        for (size_t k = 0; k < m.textureIds.size() && k < m.textures.size(); ++k)
            if (m.textureIds[k] == mt.textureRef && (m.textures[k].format == 1 || m.textures[k].format == 6)) mt.alphaThreshold = 0;
        if (name == "c321.bin") mt.properties &= ~16u;
    }
}

// An NPC table model as npc_setup / EvtNpcMdlRead read it (read_mdl), cached by file
LoadedModel& npcModel(App& app, const std::string& file) {
    auto it = app.npcModels.find(file);
    if (it == app.npcModels.end()) {
        LoadedModel lm;
        loadModelFile(app, file, lm, true);
        readMdlRules(lm, file);
        it = app.npcModels.emplace(file, std::move(lm)).first;
    }
    return it->second;
}

void showModel(App& app, int index) {
    app.renderer.resetHistory(); // a cut: the upscaler starts over
    if (app.models.empty()) return;
    app.modelIndex = wrap(index, app.models.size());
    unloadModel(app);
    loadModelFile(app, app.models[app.modelIndex], app.model, true);
    updateTitle(app);
}

// Mixer: master scales the three channels; saved to settings.ini in the user's save folder.
// _sound_init (2.2.0 0xab6c): the BGM plays at 0.2 of its option (G+0x3a428 = 20), sound effects at
// 0.3 (G+0x3a42c = 30), voices at 1.0 (__voice_resume) - the original's balance, scaled up together
// so the loudest channel the data has (the voices, else the effects) reaches full gain
void applyMix(App& app) {
    const float m = app.mix.master / 100.0f, k = app.sound.hasVoices() ? 1.0f : 1.0f / 0.3f;
    app.music.setVolume(m * k * 0.2f * app.mix.music / 100.0f);
    app.sound.setVolumes(m * k * 0.3f * app.mix.effects / 100.0f, m * k * app.mix.voice / 100.0f);
    app.sound.setVoiceStandIns(app.mix.voice == 0 || !app.sound.hasVoices()); // (__effplay's 0x10000 sounds)
}

// The user's folder - or CR_SAVE_DIR's (tests and captures keep everything of theirs there)
std::string userDir() {
    if (const char* sd = SDL_getenv("CR_SAVE_DIR"); sd && *sd) return std::string(sd) + (sd[SDL_strlen(sd) - 1] == '/' ? "" : "/");
    return cr::saveDir();
}
std::string settingsPath() { return userDir() + "settings.ini"; }


void loadMix(App& app) {
    size_t size = 0;
    char* text = static_cast<char*>(SDL_LoadFile(settingsPath().c_str(), &size));
    if (text) {
        int* fields[4] = {&app.mix.master, &app.mix.music, &app.mix.effects, &app.mix.voice};
        const char* keys[4] = {"master=", "music=", "effects=", "voice="};
        for (int i = 0; i < 4; ++i)
            if (const char* at = SDL_strstr(text, keys[i])) // (the gauges' steps of 10: an older file's 5s rounded up)
                *fields[i] = std::clamp((SDL_atoi(at + SDL_strlen(keys[i])) + 5) / 10 * 10, 0, 100);
        if (const char* at = SDL_strstr(text, "language=")) app.language = std::clamp(SDL_atoi(at + 9), 0, cr::kLanguages - 1);
        // the Vita's system data's trophy bits (G+0x34e38..; the system's own record on the Vita)
        if (const char* at = SDL_strstr(text, "trophies=")) app.trophy.flags.fromHex(std::string(at + 9, SDL_strnlen(at + 9, 16)));
        app.seenFlags.clear();
        if (const char* at = SDL_strstr(text, "seen_flags="))
            for (const char* q = at + 11; *q && *q != '\n' && *q != '\r';) {
                const int f = SDL_atoi(q);
                if (f > 0) app.seenFlags.push_back(f);
                while (*q && *q != ',' && *q != '\n') ++q;
                if (*q == ',') ++q;
            }
        applyLanguage(app);
        loadVideo(app, text);
        app.voiceEntries.clear();
        if (const char* at = SDL_strstr(text, "voice_entries=")) { // comma-separated bank entries
            at += 14;
            while (*at >= '0' && *at <= '9') {
                app.voiceEntries.push_back(SDL_atoi(at));
                while (*at >= '0' && *at <= '9') ++at;
                if (*at == ',') ++at;
            }
        }
        SDL_free(text);
    }
    SDL_Log("mix: master %d, music %d, effects %d, voice %d, %zu voice entries (%s)", app.mix.master, app.mix.music,
            app.mix.effects, app.mix.voice, app.voiceEntries.size(), settingsPath().c_str());
    app.sound.setVoiceEntries(app.voiceEntries);
    applyMix(app);
}

bool seenFlag(const App& app, int f) { return std::find(app.seenFlags.begin(), app.seenFlags.end(), f) != app.seenFlags.end(); }
void markSeen(App& app, int f) {
    if (seenFlag(app, f)) return;
    app.seenFlags.push_back(f);
    saveMix(app);
}

void saveMix(const App& app) {
    if (!app.persist) return;
    const std::string t = "master=" + std::to_string(app.mix.master) + "\nmusic=" + std::to_string(app.mix.music) +
                          "\neffects=" + std::to_string(app.mix.effects) + "\nvoice=" + std::to_string(app.mix.voice) + "\n";
    std::string list;
    for (int e : app.voiceEntries) list += (list.empty() ? "" : ",") + std::to_string(e);
    const App::Video& v = app.video;
    const std::string video = "width=" + std::to_string(v.width) + "\nheight=" + std::to_string(v.height) +
                              "\ndisplay=" + std::to_string(v.display) + "\nwidescreen=" + std::to_string(int(v.widescreen)) +
                              "\npainted_fill=" + std::to_string(v.paintings) + "\nrender_scale=" +
                              std::to_string(v.renderScale) + "\nmsaa=" + std::to_string(v.msaa) +
                              "\nvsync=" + std::to_string(int(v.vsync)) + "\nframe_limit=" + std::to_string(v.frameLimit) + "\nupscaler=" + std::to_string(v.upscaler) +
                              "\nupscale_mode=" + std::to_string(v.upscaleMode) + "\nsharpness=" +
                              std::to_string(v.sharpness) + "\nhd_textures=" + std::to_string(int(v.hdTextures)) +
                              "\nsmooth_models=" + std::to_string(v.smoothModels) + "\nhd_models=" + std::to_string(int(v.hdModels)) +
                              "\nanisotropy=" + std::to_string(v.anisotropy) + "\nsharp_textures=" + std::to_string(int(v.sharpTextures)) +
                              "\nlayout=" + std::to_string(v.layout) +
                              "\ncontrol_hints=" + std::to_string(int(app.showHud)) +
                              "\nenvironment_effects=" +
                              std::to_string(int(app.environmentEffects)) + "\nlock_orientation=" +
                              std::to_string(int(app.lockOrientation)) + "\nlock_landscape_flipped=" +
                              std::to_string(int(app.lockLandscapeFlipped)) + "\n" + "vita=" + app.vitaPath + "\n" + "android=" + app.androidPath + "\n" + "pad_confirm=" +
                              (app.padConfirmEast ? "east" : "south") + "\n" + "field_speed=" + std::to_string(app.fieldSpeed) +
                              "\nbattle_speed=" + std::to_string(app.battleSpeed) + "\n";
    std::string seen;
    for (int f : app.seenFlags) seen += (seen.empty() ? "" : ",") + std::to_string(f);
    const std::string all = t + "language=" + std::to_string(app.language) + "\nvoice_entries=" + list + "\nseen_flags=" + seen + "\n" + video +
                            "trophies=" + app.trophy.flags.hex() + "\n";
    std::string error;
    if (!crstorage::replaceFile(settingsPath(), all, &error)) SDL_Log("settings: %s", error.c_str());
}


} // namespace crapp
