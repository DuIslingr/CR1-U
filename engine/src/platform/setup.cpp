#include "platform/setup.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(CR_HAVE_ZLIB) || defined(CR2_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace cr {

namespace {

std::string withSlash(std::string p) {
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
    return p;
}
bool isDir(const std::string& p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}
std::string parentOf(std::string p) {
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}
std::string baseName(std::string p) {
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}
bool endsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    if (s.size() < n) return false;
    for (size_t k = 0; k < n; ++k)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + k])) != tail[k]) return false;
    return true;
}

// the folder and what's under it, gone (the port's own unpacking, when it turns out not the game)
void removeTree(const std::string& dir) {
    SDL_EnumerateDirectory(
        dir.c_str(),
        [](void*, const char* parent, const char* name) -> SDL_EnumerationResult {
            const std::string p = std::string(parent) + name;
            if (isDir(p)) removeTree(withSlash(p));
            else SDL_RemovePath(p.c_str());
            return SDL_ENUM_CONTINUE;
        },
        nullptr);
    SDL_RemovePath(dir.c_str());
}

// a folder copied (an unpacked .app chosen where it lies: into ios/)
bool copyTree(const std::string& from, const std::string& to) {
    if (!SDL_CreateDirectory(to.c_str())) return false;
    struct Ctx {
        std::string to;
        bool ok = true;
    } ctx{withSlash(to)};
    SDL_EnumerateDirectory(
        from.c_str(),
        [](void* user, const char* parent, const char* name) -> SDL_EnumerationResult {
            Ctx& c = *static_cast<Ctx*>(user);
            const std::string p = std::string(parent) + name;
            if (isDir(p)) c.ok &= copyTree(withSlash(p), c.to + name);
            else c.ok &= SDL_CopyFile(p.c_str(), (c.to + name).c_str());
            return c.ok ? SDL_ENUM_CONTINUE : SDL_ENUM_FAILURE;
        },
        &ctx);
    return ctx.ok;
}

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

// ---- the system's dialogs, waited for (they answer on their own threads)

struct Answer {
    std::mutex m;
    std::atomic<bool> done{false};
    std::vector<std::string> paths;
};
void SDLCALL answered(void* user, const char* const* list, int) {
    Answer& a = *static_cast<Answer*>(user);
    {
        std::lock_guard<std::mutex> lock(a.m);
        if (list)
            for (int k = 0; list[k]; ++k) a.paths.push_back(list[k]);
    }
    a.done = true;
}
std::vector<std::string> waitFor(Answer& a) {
    while (!a.done) {
        SDL_PumpEvents();
        SDL_Delay(15);
    }
    std::lock_guard<std::mutex> lock(a.m);
    return a.paths;
}
std::vector<std::string> pickFiles(const SDL_DialogFileFilter* filters, int n, bool many) {
    Answer a;
    SDL_ShowOpenFileDialog(answered, &a, nullptr, filters, n, nullptr, many);
    return waitFor(a);
}
std::string pickFolder() {
    Answer a;
    SDL_ShowOpenFolderDialog(answered, &a, nullptr, nullptr, false);
    const std::vector<std::string> p = waitFor(a);
    return p.empty() ? std::string() : p.front();
}

int ask(const SetupGame& g, const std::string& text, const std::vector<std::pair<int, const char*>>& buttons, SDL_MessageBoxFlags flags) {
    std::vector<SDL_MessageBoxButtonData> b;
    for (const auto& [id, label] : buttons) b.push_back({0, id, label});
    SDL_MessageBoxData box{};
    box.flags = flags | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    box.title = g.title;
    box.message = text.c_str();
    box.numbuttons = int(b.size());
    box.buttons = b.data();
    int hit = -1;
    if (!SDL_ShowMessageBox(&box, &hit)) return -1;
    return hit;
}
void tell(const SetupGame& g, const std::string& text) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, g.title, text.c_str(), nullptr); }

// ---- the .ipa (a zip): its central directory, then Payload/<name>.app/'s files

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16) | uint32_t(p[3]) << 24; }
uint64_t u64(const uint8_t* p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; }

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint64_t packed = 0, size = 0, local = 0;
};

bool seek(std::FILE* f, uint64_t at) {
#if defined(_WIN32)
    return _fseeki64(f, int64_t(at), SEEK_SET) == 0;
#else
    return fseeko(f, off_t(at), SEEK_SET) == 0;
#endif
}
bool readAt(std::FILE* f, uint64_t at, void* out, size_t n) { return seek(f, at) && std::fread(out, 1, n, f) == n; }

bool zipEntries(std::FILE* f, uint64_t fileSize, std::vector<ZipEntry>& out, std::string* error) {
    const uint64_t tail = std::min<uint64_t>(fileSize, 0x10000 + 22);
    std::vector<uint8_t> t(static_cast<size_t>(tail));
    if (!readAt(f, fileSize - tail, t.data(), t.size())) return fail(error, "can't be read");
    size_t eocd = std::string::npos;
    for (size_t k = t.size() - 22 + 1; k-- > 0;)
        if (u32(&t[k]) == 0x06054b50) { eocd = k; break; }
    if (eocd == std::string::npos) return fail(error, "not a zip (no central directory) - an .ipa is one");
    uint64_t count = u16(&t[eocd + 10]), cdSize = u32(&t[eocd + 12]), cdAt = u32(&t[eocd + 16]);
    if (eocd >= 20 && u32(&t[eocd - 20]) == 0x07064b50) { // (zip64: its own end record)
        uint8_t z[56];
        if (readAt(f, u64(&t[eocd - 12]), z, sizeof z) && u32(z) == 0x06064b50) count = u64(z + 32), cdSize = u64(z + 40), cdAt = u64(z + 48);
    }
    std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
    if (cdAt + cdSize > fileSize || !readAt(f, cdAt, cd.data(), cd.size())) return fail(error, "its directory is past its end");
    for (size_t p = 0, k = 0; k < count && p + 46 <= cd.size(); ++k) {
        if (u32(&cd[p]) != 0x02014b50) return fail(error, "a broken directory entry");
        ZipEntry e;
        e.method = u16(&cd[p + 10]);
        e.packed = u32(&cd[p + 20]), e.size = u32(&cd[p + 24]), e.local = u32(&cd[p + 42]);
        const size_t nl = u16(&cd[p + 28]), xl = u16(&cd[p + 30]), cl = u16(&cd[p + 32]);
        if (p + 46 + nl + xl > cd.size()) break;
        e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nl);
        for (size_t x = p + 46 + nl; x + 4 <= p + 46 + nl + xl;) { // (zip64's sizes and offset, where the plain ones are full)
            const uint16_t id = u16(&cd[x]), len = u16(&cd[x + 2]);
            if (id == 1) {
                size_t q = x + 4;
                if (e.size == 0xffffffffu && q + 8 <= x + 4 + len) e.size = u64(&cd[q]), q += 8;
                if (e.packed == 0xffffffffu && q + 8 <= x + 4 + len) e.packed = u64(&cd[q]), q += 8;
                if (e.local == 0xffffffffu && q + 8 <= x + 4 + len) e.local = u64(&cd[q]);
            }
            x += 4 + len;
        }
        out.push_back(std::move(e));
        p += 46 + nl + xl + cl;
    }
    return true;
}

bool unpackEntry(std::FILE* f, const ZipEntry& e, const std::string& path, std::string* error) {
    uint8_t h[30];
    if (!readAt(f, e.local, h, sizeof h) || u32(h) != 0x04034b50) return fail(error, e.name + ": no local header");
    const uint64_t data = e.local + 30 + u16(h + 26) + u16(h + 28);
    std::FILE* o = std::fopen(path.c_str(), "wb");
    if (!o) return fail(error, path + ": can't be written");
    std::vector<uint8_t> in(1 << 16), buf(1 << 16);
    bool ok = seek(f, data);
    if (e.method == 0) {
        for (uint64_t left = e.size; ok && left > 0;) {
            const size_t n = size_t(std::min<uint64_t>(left, in.size()));
            ok = std::fread(in.data(), 1, n, f) == n && std::fwrite(in.data(), 1, n, o) == n;
            left -= n;
        }
    } else if (e.method == 8) {
#if defined(CR_HAVE_ZLIB) || defined(CR2_HAVE_ZLIB)
        z_stream s{};
        ok = ok && inflateInit2(&s, -MAX_WBITS) == Z_OK;
        int r = Z_OK;
        for (uint64_t left = e.packed; ok && r != Z_STREAM_END;) {
            if (s.avail_in == 0) {
                const size_t n = size_t(std::min<uint64_t>(left, in.size()));
                if (n == 0) break;
                ok = std::fread(in.data(), 1, n, f) == n;
                s.next_in = in.data(), s.avail_in = uInt(n), left -= n;
            }
            s.next_out = buf.data(), s.avail_out = uInt(buf.size());
            r = inflate(&s, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END) ok = false;
            const size_t got = buf.size() - s.avail_out;
            ok = ok && std::fwrite(buf.data(), 1, got, o) == got;
        }
        inflateEnd(&s);
        ok = ok && r == Z_STREAM_END;
#else
        ok = false;
#endif
    } else {
        ok = false;
    }
    std::fclose(o);
    return ok || fail(error, e.name + ": can't be unpacked");
}

// a small window over the unpacking: the .ipa's name, a bar
struct Progress {
    SDL_Window* win = nullptr;
    SDL_Renderer* r = nullptr;
    std::string label;
    Uint64 last = 0;
    Progress(const char* title, const std::string& what) : label(what) {
        if (SDL_CreateWindowAndRenderer(title, 520, 120, 0, &win, &r)) show(0, true);
    }
    ~Progress() {
        if (r) SDL_DestroyRenderer(r);
        if (win) SDL_DestroyWindow(win);
    }
    void show(double f, bool now = false) {
        if (!r) return;
        const Uint64 t = SDL_GetTicks();
        if (!now && t - last < 50) return;
        last = t;
        SDL_PumpEvents();
        SDL_SetRenderDrawColor(r, 18, 22, 30, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 220, 230, 240, 255);
        SDL_RenderDebugText(r, 20, 24, label.c_str());
        const SDL_FRect track{20, 60, 480, 22}, bar{20, 60, float(480 * std::clamp(f, 0.0, 1.0)), 22};
        SDL_SetRenderDrawColor(r, 50, 60, 72, 255);
        SDL_RenderFillRect(r, &track);
        SDL_SetRenderDrawColor(r, 90, 200, 200, 255);
        SDL_RenderFillRect(r, &bar);
        SDL_RenderPresent(r);
    }
};

} // namespace

bool unpackIpa(const std::string& ipa, const std::string& folder, std::string& app, std::string* error,
               const std::function<void(double)>& progress) {
    std::FILE* f = std::fopen(ipa.c_str(), "rb");
    if (!f) return fail(error, "can't be opened");
    SDL_PathInfo info{};
    SDL_GetPathInfo(ipa.c_str(), &info);
    std::vector<ZipEntry> all;
    if (!zipEntries(f, info.size, all, error)) return std::fclose(f), false;
    // (Payload/<name>.app/...: the first .app's)
    std::string appName;
    for (const ZipEntry& e : all)
        if (e.name.compare(0, 8, "Payload/") == 0)
            if (const size_t s = e.name.find('/', 8); s != std::string::npos && endsWith(e.name.substr(0, s), ".app")) {
                appName = e.name.substr(8, s - 8);
                break;
            }
    if (appName.empty()) return std::fclose(f), fail(error, "no Payload/<name>.app in it - not an iOS app");
    const std::string prefix = "Payload/" + appName + "/";
    const std::string part = withSlash(folder) + appName + ".part/", done = withSlash(folder) + appName + "/";
    if (isDir(part)) removeTree(part);
    if (!SDL_CreateDirectory(part.c_str())) return std::fclose(f), fail(error, folder + ": can't be written");
    uint64_t total = 0, written = 0;
    for (const ZipEntry& e : all)
        if (e.name.compare(0, prefix.size(), prefix) == 0) total += e.size;
    bool ok = true;
    for (const ZipEntry& e : all) {
        if (e.name.compare(0, prefix.size(), prefix) != 0 || e.name.size() == prefix.size()) continue;
        const std::string rel = e.name.substr(prefix.size());
        if (rel.find("..") != std::string::npos || rel[0] == '/' || rel[0] == '\\') continue; // (nothing outside the folder)
        const std::string path = part + rel;
        if (rel.back() == '/') {
            SDL_CreateDirectory(path.c_str());
            continue;
        }
        SDL_CreateDirectory(parentOf(path).c_str());
        if (!(ok = unpackEntry(f, e, path, error))) break;
        written += e.size;
        if (progress && total) progress(double(written) / double(total));
    }
    std::fclose(f);
    if (!ok) return removeTree(part), false;
    if (isDir(done)) removeTree(done); // (an earlier unpacking of the same: replaced)
    if (!SDL_RenamePath(part.c_str(), done.c_str())) return removeTree(part), fail(error, done + ": can't be made");
    app = done;
    return true;
}

std::string findVitaGame(const std::string& picked, const SetupGame& g) {
    const std::string p = withSlash(picked);
    std::vector<std::string> tries; // (the game's own folders first, the folder chosen last - a reader that looks deep would take any game under it)
    const std::string id = g.vitaId, tri = g.trilogyDir;
    for (const std::string& top : {p, p + "app/", p + "ux0/app/"}) {
        tries.push_back(top + id + "/");
        if (!tri.empty()) tries.push_back(top + "PCSG00457/" + tri + "/");
    }
    if (!tri.empty()) tries.push_back(p + tri + "/"); // (the trilogy's own folder chosen)
    tries.push_back(p);
    for (const std::string& t : tries)
        if (isDir(t) && g.vitaOk(t)) return t;
    // (Vita3K's own folder: its config.yml's pref-path, where its ux0 is)
    size_t size = 0;
    if (char* text = static_cast<char*>(SDL_LoadFile((p + "config.yml").c_str(), &size))) {
        const std::string all(text, size);
        SDL_free(text);
        const size_t at = all.find("pref-path:");
        if (at != std::string::npos) {
            std::string pref = all.substr(at + 10, all.find('\n', at) - at - 10);
            pref.erase(0, pref.find_first_not_of(" \t'\""));
            while (!pref.empty() && (pref.back() == '\r' || pref.back() == ' ' || pref.back() == '\'' || pref.back() == '"')) pref.pop_back();
            if (!pref.empty() && withSlash(pref) != p) return findVitaGame(pref, g);
        }
    }
    return {};
}

bool runSetup(const SetupGame& g, const std::string& iosFolder, const std::string& fallbackIos, SetupResult& out) {
    for (;;) {
        std::string text = std::string(g.port) + " plays your own copy of " + g.title +
                           ". Choose where it is - any one of them; the port takes the files from there:\n\n"
                           "  iOS - the .ipa (or a file in its unpacked .app)\n"
                           "  Android - the APK and its " + g.obbHint + " (in one folder)\n"
                           "  Vita - Vita3K's folder (or the game's " + g.vitaId + " folder)";
        const bool any = !out.ios.empty() || !out.android.empty() || !out.vita.empty();
        if (any) {
            text += "\n\nGiven so far:";
            if (!out.ios.empty()) text += "\n  iOS: " + out.ios;
            if (!out.android.empty()) text += "\n  Android: " + out.android;
            if (!out.vita.empty()) text += "\n  Vita: " + out.vita;
            if (!g.oneRelease) text += "\n\nMore of them add to it (the Vita's voices and music, Android's art); or Start.";
        }
        std::vector<std::pair<int, const char*>> buttons = {{1, "iOS (.ipa)..."}, {2, "Android (APK + OBB)..."}, {3, "Vita / Vita3K..."}};
        if (any) buttons.push_back({4, "Start"});
        buttons.push_back({0, "Quit"});
        const int choice = ask(g, text, buttons, SDL_MESSAGEBOX_INFORMATION);
        if (choice <= 0) return false;
        if (choice == 4) return true;
        if (choice == 1) {
            static const SDL_DialogFileFilter kIpa[] = {{"iOS app (.ipa)", "ipa"}, {"Any file (one in an unpacked .app)", "*"}};
            const std::vector<std::string> picked = pickFiles(kIpa, 2, false);
            if (picked.empty()) continue;
            const std::string& f = picked.front();
            std::string app, why;
            if (endsWith(f, ".ipa") || endsWith(f, ".zip")) {
                std::string where = iosFolder;
                SDL_CreateDirectory(where.c_str());
                bool ok;
                {
                    Progress bar(g.title, "Unpacking " + baseName(f) + " ...");
                    ok = unpackIpa(f, where, app, &why, [&](double v) { bar.show(v); });
                    if (!ok && !fallbackIos.empty() && fallbackIos != iosFolder) { // (the game folder can't be written: the profile's)
                        where = fallbackIos;
                        SDL_CreateDirectory(where.c_str());
                        ok = unpackIpa(f, where, app, &why, [&](double v) { bar.show(v); });
                    }
                }
                if (!ok) {
                    tell(g, baseName(f) + ": " + why);
                    continue;
                }
                if (!g.iosOk(app)) {
                    removeTree(app);
                    tell(g, baseName(f) + " isn't " + g.title + "'s iOS release.");
                    continue;
                }
            } else { // (a file in an unpacked .app: that .app, copied into ios/)
                std::string dir = parentOf(f);
                while (!dir.empty() && !endsWith(dir.substr(0, dir.size() - 1), ".app")) dir = parentOf(dir);
                if (dir.empty()) dir = parentOf(f);
                if (!g.iosOk(dir)) {
                    tell(g, "No " + std::string(g.title) + " data in " + dir);
                    continue;
                }
                app = withSlash(iosFolder) + baseName(dir) + "/";
                SDL_CreateDirectory(iosFolder.c_str());
                Progress bar(g.title, "Copying " + baseName(dir) + " ...");
                bool ok = copyTree(dir, app);
                if (!ok && !fallbackIos.empty()) { // (the game folder can't be written: the profile's)
                    removeTree(app);
                    SDL_CreateDirectory(fallbackIos.c_str());
                    ok = copyTree(dir, app = withSlash(fallbackIos) + baseName(dir) + "/");
                }
                if (!ok) {
                    tell(g, app + ": can't be written");
                    continue;
                }
            }
            out.ios = app;
        } else if (choice == 2) {
            static const SDL_DialogFileFilter kAndroid[] = {{"Android (.obb, .apk)", "obb;apk"}, {"Any file", "*"}};
            const std::vector<std::string> picked = pickFiles(kAndroid, 2, true);
            if (picked.empty()) continue;
            std::string found;
            for (const std::string& f : picked)
                for (const std::string& t : {f, parentOf(f)})
                    if (found.empty() && g.androidOk(t)) found = t;
            if (found.empty()) {
                tell(g, std::string("No ") + g.title + " Android release there: the APK and its " + g.obbHint + " go in one folder.");
                continue;
            }
            out.android = found;
        } else if (choice == 3) {
            const std::string picked = pickFolder();
            if (picked.empty()) continue;
            const std::string game = findVitaGame(picked, g);
            if (game.empty()) {
                tell(g, std::string("No ") + g.title + " (" + g.vitaId + ") in " + picked +
                            " - choose Vita3K's folder, its ux0, or the game's own folder.");
                continue;
            }
            out.vita = game;
        }
        if (g.oneRelease) return true;
    }
}

} // namespace cr
