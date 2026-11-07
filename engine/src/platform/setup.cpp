#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX // (archive.h brings <windows.h>: its min / max macros would take std::min)
#endif
#include "platform/setup.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <filesystem>
#include <memory>
#include <sstream>
#ifdef CR_HAVE_PSVPFS
#include "BuiltinCryptoOperations.h"
#include "F00DNativeKeyEncryptor.h"
#include "PfsFilesystem.h"
#endif
#ifdef CR_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#if defined(CR_HAVE_ZLIB)
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

bool exists(const std::string& p) { return SDL_GetPathInfo(p.c_str(), nullptr); }
std::filesystem::path fsPath(const std::string& s) { // (UTF-8, as SDL's paths are)
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
    return std::filesystem::u8path(s);
#endif
}

// A phone (Android): no folder dialog - a folder is copied in over USB, then found where it was
// put -, and the picker's files are content:// URIs, copied into the game folder before they're
// read. (CRU_SETUP_PHONE: a test's, on a PC)
bool phone() {
#ifdef SDL_PLATFORM_ANDROID
    return true;
#else
    static const bool on = SDL_getenv("CRU_SETUP_PHONE") != nullptr;
    return on;
#endif
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

// ---- copying, its bytes counted (the progress bar's)

uint64_t fileBytes(const std::string& p) {
    SDL_PathInfo info{};
    return SDL_GetPathInfo(p.c_str(), &info) ? info.size : 0;
}
uint64_t treeBytes(const std::string& dir) {
    uint64_t total = 0;
    SDL_EnumerateDirectory(
        dir.c_str(),
        [](void* user, const char* parent, const char* name) -> SDL_EnumerationResult {
            const std::string p = std::string(parent) + name;
            *static_cast<uint64_t*>(user) += isDir(p) ? treeBytes(withSlash(p)) : fileBytes(p);
            return SDL_ENUM_CONTINUE;
        },
        &total);
    return total;
}
struct Copier {
    std::function<void(double)> progress;
    uint64_t total = 0, done = 0;
    bool file(const std::string& from, const std::string& to) {
        std::FILE* in = std::fopen(from.c_str(), "rb");
        if (!in) return false;
        std::FILE* out = std::fopen(to.c_str(), "wb");
        if (!out) return std::fclose(in), false;
        std::vector<uint8_t> buf(1 << 20);
        bool ok = true;
        for (size_t n; ok && (n = std::fread(buf.data(), 1, buf.size(), in)) > 0;) {
            ok = std::fwrite(buf.data(), 1, n, out) == n;
            done += n;
            if (progress && total) progress(double(done) / double(total));
        }
        ok = ok && !std::ferror(in);
        std::fclose(in);
        ok = std::fclose(out) == 0 && ok;
        if (!ok) SDL_RemovePath(to.c_str());
        return ok;
    }
    bool tree(const std::string& from, const std::string& to) {
        if (!isDir(to) && !SDL_CreateDirectory(to.c_str())) return false;
        struct Ctx {
            Copier* c;
            std::string to;
            bool ok = true;
        } ctx{this, withSlash(to)};
        SDL_EnumerateDirectory(
            from.c_str(),
            [](void* user, const char* parent, const char* name) -> SDL_EnumerationResult {
                Ctx& x = *static_cast<Ctx*>(user);
                const std::string p = std::string(parent) + name;
                x.ok = isDir(p) ? x.c->tree(withSlash(p), x.to + name) : x.c->file(p, x.to + name);
                return x.ok ? SDL_ENUM_CONTINUE : SDL_ENUM_FAILURE;
            },
            &ctx);
        return ctx.ok;
    }
};

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

// ---- a test's answers (CRU_SETUP_ANSWERS: no windows), else the system's dialogs, waited for
// (they answer on their own threads)

struct Script {
    bool on = false;
    std::vector<std::string> left;
};
Script& script() {
    static Script s = [] {
        Script x;
        if (const char* a = SDL_getenv("CRU_SETUP_ANSWERS")) {
            x.on = true;
            std::string all = a;
            for (size_t at = 0; at <= all.size();) {
                const size_t e = std::min(all.find(';', at), all.size());
                if (e > at) x.left.push_back(all.substr(at, e - at));
                at = e + 1;
            }
        }
        return x;
    }();
    return s;
}
std::string answer(const std::string& asked) {
    Script& s = script();
    if (s.left.empty()) {
        SDL_Log("setup (scripted): %s - no answer left: Quit", asked.c_str());
        return {};
    }
    std::string a = s.left.front();
    s.left.erase(s.left.begin());
    SDL_Log("setup (scripted): %s -> %s", asked.c_str(), a.c_str());
    return a;
}

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
std::vector<std::string> localize(const SetupGame& g, const std::vector<std::string>& picked); // (below)
std::vector<std::string> pickFiles(const SetupGame& g, const SDL_DialogFileFilter* filters, int n, bool many) {
    if (script().on) {
        std::vector<std::string> out;
        const std::string a = answer("files");
        for (size_t at = 0; at < a.size();) {
            const size_t e = std::min(a.find('|', at), a.size());
            if (e > at) out.push_back(a.substr(at, e - at));
            at = e + 1;
        }
        return localize(g, out);
    }
    if (phone()) { // (Android's picker filters by MIME type, and an .ipa, an .obb, an .apks have none: every file shown)
        static const SDL_DialogFileFilter kAny[] = {{"Any file", "*"}};
        filters = kAny, n = 1;
    }
    Answer a;
    SDL_ShowOpenFileDialog(answered, &a, nullptr, filters, n, nullptr, many);
    return localize(g, waitFor(a));
}
std::string pickFolder() {
    if (script().on) return answer("a folder");
    Answer a;
    SDL_ShowOpenFolderDialog(answered, &a, nullptr, nullptr, false);
    const std::vector<std::string> p = waitFor(a);
    return p.empty() ? std::string() : p.front();
}

int ask(const SetupGame& g, const std::string& text, const std::vector<std::pair<int, const char*>>& buttons, SDL_MessageBoxFlags flags) {
    if (script().on) {
        std::string shown = text + "\n  [";
        for (const auto& [id, label] : buttons) shown += " " + std::to_string(id) + " " + label + " ";
        SDL_Log("setup asks:\n%s]", shown.c_str());
        const std::string a = answer("the choice");
        return a.empty() ? 0 : std::atoi(a.c_str());
    }
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
void tell(const SetupGame& g, const std::string& text) {
    if (script().on) return (void)SDL_Log("setup tells: %s", text.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, g.title, text.c_str(), nullptr);
}

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
#if defined(CR_HAVE_ZLIB)
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
        if (script().on) { // (a test's: no window)
            SDL_Log("setup: %s", what.c_str());
            return;
        }
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
        // (520 x 120, centred and scaled up to the window: a phone's is the whole screen)
        int ow = 520, oh = 120;
        SDL_GetCurrentRenderOutputSize(r, &ow, &oh);
        const float k = std::max(1.0f, std::min(float(ow) / 560.0f, float(oh) / 160.0f));
        SDL_SetRenderScale(r, k, k);
        const float x = std::max(0.0f, (float(ow) / k - 520) / 2), y = std::max(0.0f, (float(oh) / k - 120) / 2);
        SDL_SetRenderDrawColor(r, 18, 22, 30, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 220, 230, 240, 255);
        SDL_RenderDebugText(r, x + 20, y + 24, label.c_str());
        const SDL_FRect track{x + 20, y + 60, 480, 22}, bar{x + 20, y + 60, float(480 * std::clamp(f, 0.0, 1.0)), 22};
        SDL_SetRenderDrawColor(r, 50, 60, 72, 255);
        SDL_RenderFillRect(r, &track);
        SDL_SetRenderDrawColor(r, 90, 200, 200, 255);
        SDL_RenderFillRect(r, &bar);
        SDL_RenderPresent(r);
    }
};

// a message: shown, or (`quiet`, a test's) logged alone
void note(const SetupGame& g, const std::string& text, bool quiet) {
    SDL_Log("setup: %s", text.c_str());
    if (script().on) tell(g, text);
    else if (!quiet) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, g.title, text.c_str(), nullptr);
}

// ---- a picked file the system hands over as a content:// URI (Android's document picker): copied
// into the game folder's .picked/ first - the unpacking reads files -, named by what it is, as the
// URI has no name of its own: an .ipa (its app's name), an .apks, an APK (the package's name), a
// .rar / .7z / .zip, an asset pack (.android.mvgl), else the OBB (its name, setup.obbHint)

std::string& staging() { // (runSetup's: <game folder>/.picked/)
    static std::string s;
    return s;
}
bool staged(const std::string& p) { return !staging().empty() && p.compare(0, staging().size(), staging()) == 0; }

std::string kindName(const SetupGame& g, const std::string& path, int index) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    uint8_t head[8] = {};
    const size_t got = std::fread(head, 1, sizeof head, f);
    std::string name = "picked" + std::to_string(index);
    if (got >= 6 && std::memcmp(head, "Rar!\x1a\x07", 6) == 0) name += ".rar";
    else if (got >= 6 && std::memcmp(head, "7z\xbc\xaf\x27\x1c", 6) == 0) name += ".7z";
    else if (got >= 4 && std::memcmp(head, "MDB1", 4) == 0) // (an archive: CR2's OBB is one; CR3's asset packs, which have no OBB)
        name = endsWith(g.obbHint, ".obb") ? std::string(g.obbHint) : name + ".android.mvgl";
    else if (got >= 4 && std::memcmp(head, "PK\x03\x04", 4) == 0) {
        std::vector<ZipEntry> all;
        std::string why, app;
        zipEntries(f, fileBytes(path), all, &why);
        bool apks = false, apk = false;
        for (const ZipEntry& e : all) {
            if (app.empty() && e.name.compare(0, 8, "Payload/") == 0)
                if (const size_t at = e.name.find('/', 8); at != std::string::npos && endsWith(e.name.substr(0, at), ".app")) app = e.name.substr(8, at - 12);
            if (e.name.find('/') == std::string::npos && endsWith(e.name, ".apk")) apks = true;
            if (e.name == "AndroidManifest.xml") apk = true;
        }
        const std::string pkg = *g.package ? g.package : "game";
        name = !app.empty() ? app + ".ipa" : apks ? pkg + ".apks" : apk ? pkg + ".apk" : name + ".zip";
    } else name = endsWith(g.obbHint, ".obb") ? std::string(g.obbHint) : name + ".bin";
    std::fclose(f);
    return name;
}

// the picks as files: a URI (content://; a test's copy:<path>) copied into staging(), the rest as they are
std::vector<std::string> localize(const SetupGame& g, const std::vector<std::string>& picked) {
    std::vector<std::string> out;
    int index = 0;
    for (const std::string& p : picked) {
        const bool uri = p.compare(0, 10, "content://") == 0, test = script().on && p.compare(0, 5, "copy:") == 0;
        if (!uri && !test) {
            out.push_back(p);
            continue;
        }
        const std::string from = test ? p.substr(5) : p;
        SDL_IOStream* in = SDL_IOFromFile(from.c_str(), "rb");
        if (!in) {
            tell(g, "A file picked can't be read (" + std::string(SDL_GetError()) + ").");
            continue;
        }
        const Sint64 size = SDL_GetIOSize(in);
        SDL_CreateDirectory(staging().c_str());
        std::error_code ec;
        const std::filesystem::space_info room = std::filesystem::space(fsPath(staging()), ec);
        if (!ec && size > 0 && room.available < uint64_t(size) * 2 + (64u << 20)) { // (the copy, and what it unpacks to)
            SDL_CloseIO(in);
            char text[200];
            std::snprintf(text, sizeof text, "A file picked (%.1f GB) needs about %.1f GB free in %s - the copy and what it unpacks to -, and there is %.1f GB.",
                          double(size) / 1e9, double(size) * 2 / 1e9, staging().c_str(), double(room.available) / 1e9);
            tell(g, text);
            continue;
        }
        const std::string part = staging() + "picking" + std::to_string(++index);
        std::FILE* o = std::fopen(part.c_str(), "wb");
        bool ok = o != nullptr;
        {
            Progress bar(g.title, "Copying the file picked" + (picked.size() > 1 ? " (" + std::to_string(index) + " of " + std::to_string(picked.size()) + ")" : std::string()) + " ...");
            std::vector<uint8_t> buf(1 << 20);
            uint64_t done = 0;
            for (size_t n; ok && (n = SDL_ReadIO(in, buf.data(), buf.size())) > 0;) {
                ok = std::fwrite(buf.data(), 1, n, o) == n;
                done += n;
                if (size > 0) bar.show(double(done) / double(size));
            }
            ok = ok && SDL_GetIOStatus(in) != SDL_IO_STATUS_ERROR;
        }
        SDL_CloseIO(in);
        if (o) ok = std::fclose(o) == 0 && ok;
        const std::string name = ok ? kindName(g, part, index) : std::string();
        const std::string to = staging() + name;
        if (!ok || name.empty() || (exists(to) && !SDL_RemovePath(to.c_str())) || !SDL_RenamePath(part.c_str(), to.c_str())) {
            SDL_RemovePath(part.c_str());
            tell(g, "A file picked can't be copied into " + staging());
            continue;
        }
        SDL_Log("setup: picked %s - %s", from.c_str(), name.c_str());
        out.push_back(to);
    }
    return out;
}

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

bool setupScripted() { return script().on; }

namespace {

// ---- the Vita release: as Vita3K installs it (ux0/app/<id>/, the Prequel Trilogy's add-ons in
// ux0/addcont/PCSG00457/), or a NoNpDrm dump of it - its files in the Vita's PFS, encrypted, their
// key in sce_sys/package/work.bin (+0x50: the klicensee) - decrypted as Vita3K's install does
// (psvpfsparser), or such a dump packed in an archive (libarchive)


// the fan translation's own folder - rePatch's <id>/ - wherever it is in what was chosen (a download's
// folder: its .rar beside an extracted/rePatch/<id>/), not the folders around it nor the archive
std::string patchFolder(const std::string& from, const std::string& id) {
    std::error_code ec;
    std::string best;
    int bestDepth = 99;
    std::filesystem::recursive_directory_iterator it(fsPath(from), std::filesystem::directory_options::skip_permission_denied, ec), end;
    for (; it != end && !ec; it.increment(ec)) {
        if (it.depth() >= 6) it.disable_recursion_pending();
        if (it.depth() < bestDepth && it->is_directory(ec) && it->path().filename() == fsPath(id)) {
            const auto u = it->path().u8string(); // (UTF-8: std::string, or C++20's std::u8string)
            best = withSlash(std::string(u.begin(), u.end())), bestDepth = it.depth();
        }
    }
    return best.empty() ? from : best;
}
bool encryptedVita(const std::string& dir) { return exists(withSlash(dir) + "sce_sys/package/work.bin") && isDir(withSlash(dir) + "sce_pfs"); }

bool decryptVita(const std::string& from, const std::string& to, std::string* error, const std::function<void(double)>& progress) {
#ifdef CR_HAVE_PSVPFS
    uint8_t key[16];
    SDL_IOStream* w = SDL_IOFromFile((withSlash(from) + "sce_sys/package/work.bin").c_str(), "rb");
    const bool read = w && SDL_SeekIO(w, 0x50, SDL_IO_SEEK_SET) == 0x50 && SDL_ReadIO(w, key, 16) == 16;
    if (w) SDL_CloseIO(w);
    if (!read) return fail(error, "no work.bin (its licence) to decrypt it with");
    const std::filesystem::path src = fsPath(from), dst = fsPath(to);
    const auto crypto = std::make_shared<BuiltinCryptoOperations>();
    const auto f00d = std::make_shared<F00DNativeKeyEncryptor>(crypto);
    std::ostringstream log;
    PfsFilesystem pfs(crypto, f00d, log, key, src);
    if (pfs.mount() < 0) return fail(error, "its files can't be read with its own work.bin - not a whole NoNpDrm dump");
    if (pfs.decrypt_files(dst, [&](std::uint64_t done, std::uint64_t total, const std::string&) {
            if (progress && total) progress(double(done) / double(total));
        }) < 0)
        return fail(error, "its files don't decrypt (a part of the dump missing or damaged?)");
    return true;
#else
    (void)from, (void)to, (void)progress;
    return fail(error, "this build can't decrypt the Vita's files - install the dump in Vita3K, then choose Vita3K's folder");
#endif
}

#ifdef CR_HAVE_LIBARCHIVE
constexpr bool kArchives = true;
#else
constexpr bool kArchives = false;
#endif

// an archive's entries `want` takes (its paths with '/'), unpacked into `to`; `progress` by the bytes read
bool unpackArchive(const std::string& path, const std::string& to, const std::function<bool(const std::string&)>& want, std::string* error,
                   const std::function<void(double)>& progress) {
#ifdef CR_HAVE_LIBARCHIVE
    archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, path.c_str(), 1 << 20) != ARCHIVE_OK) {
        const std::string why = archive_error_string(a) ? archive_error_string(a) : "can't be opened";
        archive_read_free(a);
        return fail(error, why);
    }
    const uint64_t total = fileBytes(path);
    archive_entry* e = nullptr;
    bool ok = true;
    std::vector<char> buf(1 << 20);
    int r;
    while (ok && (r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
        const char* n = archive_entry_pathname_utf8(e);
        if (!n) n = archive_entry_pathname(e);
        std::string name = n ? n : "";
        std::replace(name.begin(), name.end(), '\\', '/');
        const bool unsafe = name.empty() || name[0] == '/' || name.find(':') != std::string::npos || ("/" + name + "/").find("/../") != std::string::npos;
        if (unsafe || !want(name)) {
            archive_read_data_skip(a);
            continue;
        }
        const std::string out = withSlash(to) + name;
        if (archive_entry_filetype(e) == AE_IFDIR) {
            SDL_CreateDirectory(out.c_str());
            continue;
        }
        if (archive_entry_filetype(e) != AE_IFREG) {
            archive_read_data_skip(a);
            continue;
        }
        SDL_CreateDirectory(parentOf(out).c_str());
        SDL_IOStream* f = SDL_IOFromFile(out.c_str(), "wb");
        if (!f) {
            ok = false, fail(error, out + ": can't be written");
            break;
        }
        for (la_ssize_t got; (got = archive_read_data(a, buf.data(), buf.size())) != 0;) {
            if (got < 0 || SDL_WriteIO(f, buf.data(), size_t(got)) != size_t(got)) {
                ok = false, fail(error, got < 0 ? name + ": " + (archive_error_string(a) ? archive_error_string(a) : "can't be read") : out + ": can't be written");
                break;
            }
            if (progress && total) progress(double(archive_filter_bytes(a, -1)) / double(total));
        }
        if (!SDL_CloseIO(f)) ok = false;
    }
    if (ok && r != ARCHIVE_EOF) ok = false, fail(error, archive_error_string(a) ? archive_error_string(a) : "a broken entry");
    archive_read_free(a);
    return ok;
#else
    (void)path, (void)to, (void)want, (void)progress;
    return fail(error, "this build can't open archives - unpack it first, then choose its folder");
#endif
}

// a fan translation's archive (the .rar / .zip it comes in) unpacked, its <id>/ kept as `t`<id>/ (`t`:
// the game folder's translation/): that folder; "" not (`error` why)
// a fan translation's own folder in translation/: its language's (en/, es/ - the English one and the
// Spanish one both kept), else the Vita id's
std::string patchDir(const SetupGame& g, const std::string& patch) {
    const std::string n = g.patchName ? g.patchName(patch) : std::string();
    return n.empty() ? std::string(g.vitaId) : n;
}

std::string unpackPatch(const SetupGame& g, const std::string& archive, const std::string& t, std::string* error,
                        const std::function<void(double)>& progress) {
    const std::string u = t + ".unpack/";
    if ((!isDir(t) && !SDL_CreateDirectory(t.c_str())) || (isDir(u) && (removeTree(u), false)) || !SDL_CreateDirectory(u.c_str()))
        return fail(error, t + ": can't be written"), std::string();
    if (!unpackArchive(archive, u, [](const std::string&) { return true; }, error, progress)) return removeTree(u), std::string();
    const std::string own = patchFolder(u, g.vitaId);
    if (!g.patchOk(own)) return removeTree(u), fail(error, std::string("no fan translation in it (its rePatch/") + g.vitaId + "/)"), std::string();
    const std::string named = patchDir(g, own), done = t + named + "/", older = t + g.vitaId + "/";
    if (isDir(done)) removeTree(done);
    if (named != g.vitaId && isDir(older) && patchDir(g, older) == named) removeTree(older); // (an earlier version's translation/<id>/ of this one's language)
    const bool moved = SDL_RenamePath(own.c_str(), done.c_str());
    if (isDir(u)) removeTree(u);
    if (!moved) return fail(error, done + ": can't be made"), std::string();
    return done;
}

// a folder's entries moved into another, their folders merged (an add-on's into its game's)
bool mergeInto(const std::string& from, const std::string& to) {
    struct Ctx {
        std::string to;
        bool ok = true;
    } ctx{withSlash(to)};
    SDL_EnumerateDirectory(
        from.c_str(),
        [](void* user, const char* parent, const char* name) -> SDL_EnumerationResult {
            Ctx& c = *static_cast<Ctx*>(user);
            const std::string p = std::string(parent) + name, t = c.to + name;
            if (isDir(p) && isDir(t)) c.ok = mergeInto(withSlash(p), t);
            else {
                if (exists(t)) SDL_RemovePath(t.c_str());
                c.ok = SDL_RenamePath(p.c_str(), t.c_str());
            }
            return c.ok ? SDL_ENUM_CONTINUE : SDL_ENUM_FAILURE;
        },
        &ctx);
    return ctx.ok;
}

// ---- a Vita program (a SELF) decrypted to its ELF, as Vita3K does when it loads one (its
// packages/sce_utils.cpp, GPL-2.0): the segments' keys in its metadata - for an app, under the
// NPDRM key and its klicensee, then the metadata key (Vita3K's retail app keys, by the SELF's key
// revision and system version) -, each segment AES-128-CTR, some deflated

struct SelfKey {
    int revision;
    const char* key; // (AES-256)
    const char* iv;
    uint64_t minVersion, maxVersion;
};
const SelfKey kAppMetadataKeys[] = {
    {0, "AAA508FA5E85EAEE597ED2B27804D22287CFADF1DF32EDC7A7C58E8C9AA8BB36", "CD1BD3A59200CC67A3B804808DC2AE73", 0x00000000000, 0x16920000000},
    {0, "5661E5FB20CFD1D1DFF50C1E59A6EA977D0AA5C5770F53B9CDD4E9451FFF55CB", "23D02FF79BF430E2D123869BF0CACAA0", 0x18000000000, ~uint64_t(0)},
    {1, "4181B2DF5F5D94D3C80B7D86EACF1928533A49BA58EDE2B43CDEE7E572568BD4", "B1678C0543B6C1997B63A6F4F3C8FD33", 0, ~uint64_t(0)},
    {2, "5282582F17F068F89A260AAFB71C58928F45A8D08C681376B07FF9EAB1114226", "29672DF43E426F41AF46D42E8437D449", 0, ~uint64_t(0)},
    {3, "270CBA370061B87077672ADB5142D18844AAED352A9CCEE63602B0D740594334", "1CF2454FBF47D76221B91AFC3B608C28", 0, ~uint64_t(0)},
    {4, "A782BC5A9EDDFC49A513FF3E592C4677A8C8920F23C9F11F2558FB9D99A43868", "559B5E658559EB65EBF892C274E098A9", 0, ~uint64_t(0)},
    {5, "12D64D0172495226010A687DE245A73DE028B3561E25E69BABC325636F3CAE0A", "F149EED1757E5A915B24309795BFC380", 0, ~uint64_t(0)},
};
const char* const kNpdrmKeys[2] = {"C10368BF3D2943BC6E5BD05E46A9A7B6", "16419DD3BFBE8BDC596929B72CE237CD"}; // (key revision < 2, 2 on)

std::vector<uint8_t> hexBytes(const char* hex) {
    std::vector<uint8_t> out;
    for (size_t k = 0; hex[k] && hex[k + 1]; k += 2) out.push_back(uint8_t(std::stoi(std::string(hex + k, 2), nullptr, 16)));
    return out;
}
uint32_t le32At(const std::vector<uint8_t>& f, uint64_t at) { return at + 4 <= f.size() ? u32(&f[size_t(at)]) : 0; }
uint64_t le64At(const std::vector<uint8_t>& f, uint64_t at) { return at + 8 <= f.size() ? u64(&f[size_t(at)]) : 0; }

// whether a program's segments are encrypted (its first's "plaintext" no)
bool selfEncrypted(const std::vector<uint8_t>& f) {
    if (f.size() < 0x80 || std::memcmp(f.data(), "SCE\0", 4) != 0) return false;
    return le32At(f, le64At(f, 32 + 56) + 24) == 1;
}

bool decryptSelf(const std::vector<uint8_t>& f, const uint8_t* klic, std::vector<uint8_t>& elf, std::string* error) {
#ifdef CR_HAVE_PSVPFS
    if (f.size() < 0x80 || std::memcmp(f.data(), "SCE\0", 4) != 0 || (f[10] | f[11] << 8) != 1) return fail(error, "not a SELF");
    const int keyRevision = f[9];
    const uint64_t metadataAt = le32At(f, 12), headerLength = le64At(f, 16);
    const uint64_t appInfo = le64At(f, 32 + 24), elfAt = le64At(f, 32 + 32), phdrAt = le64At(f, 32 + 40), segAt = le64At(f, 32 + 56);
    const uint32_t selfType = le32At(f, appInfo + 12);
    const uint64_t sysVersion = le64At(f, appInfo + 16);
    if (headerLength > f.size() || metadataAt + 48 + 64 > headerLength || elfAt + 52 > f.size()) return fail(error, "a broken SELF");
    const int phnum = f[size_t(elfAt) + 44] | f[size_t(elfAt) + 45] << 8;
    struct Segment {
        uint64_t offset, size;
        bool compressed, encrypted;
        uint32_t fileOffset, fileSize;
    };
    std::vector<Segment> segs;
    bool any = false;
    for (int i = 0; i < phnum; ++i) {
        const uint64_t s = segAt + uint64_t(i) * 32, p = phdrAt + uint64_t(i) * 32;
        segs.push_back({le64At(f, s), le64At(f, s + 8), le32At(f, s + 16) == 2, le32At(f, s + 24) == 1, le32At(f, p + 4), le32At(f, p + 16)});
        any = any || segs.back().encrypted;
        if (segs.back().offset + segs.back().size > f.size()) return fail(error, "a broken SELF (a segment past its end)");
    }
    BuiltinCryptoOperations crypto;
    struct Keyed {
        int idx;
        std::vector<uint8_t> key, iv;
    };
    std::vector<Keyed> keyed; // (the encrypted segments, in the metadata's order)
    if (any) {
        const SelfKey* mk = nullptr;
        for (const SelfKey& k : kAppMetadataKeys)
            if (k.revision == keyRevision && sysVersion >= k.minVersion && sysVersion <= k.maxVersion) mk = &k;
        if (selfType != 8 || !mk) return fail(error, "a SELF of a kind (or key revision) the port has no key for");
        std::vector<uint8_t> dat(f.begin() + long(metadataAt + 48), f.begin() + long(headerLength));
        uint8_t in[64], info[64], zero[16] = {}, iv[16] = {};
        std::memcpy(in, dat.data(), 64);
        { // (an app's: the metadata's first block under the klicensee, itself under the NPDRM key)
            const std::vector<uint8_t> np = hexBytes(kNpdrmKeys[keyRevision >= 2 ? 1 : 0]);
            uint8_t klicKey[16];
            std::memcpy(iv, zero, 16);
            crypto.aes_cbc_decrypt(klic, klicKey, 16, np.data(), 128, iv);
            std::memcpy(iv, zero, 16);
            crypto.aes_cbc_decrypt(dat.data(), in, 64, klicKey, 128, iv);
        }
        const std::vector<uint8_t> key = hexBytes(mk->key), kiv = hexBytes(mk->iv);
        std::memcpy(iv, kiv.data(), 16);
        crypto.aes_cbc_decrypt(in, info, 64, key.data(), 256, iv);
        for (int k = 16; k < 32; ++k)
            if (info[k] || info[k + 32]) return fail(error, "its key doesn't open it (its licence isn't its own?)");
        std::vector<uint8_t> meta((dat.size() - 64) / 16 * 16);
        std::memcpy(iv, info + 32, 16);
        crypto.aes_cbc_decrypt(dat.data() + 64, meta.data(), int(meta.size()), info, 128, iv);
        if (meta.size() < 32) return fail(error, "a broken SELF (no metadata)");
        const uint32_t sections = le32At(meta, 12), keys = le32At(meta, 16);
        const uint64_t vault = 32 + uint64_t(sections) * 48;
        if (vault + uint64_t(keys) * 16 > meta.size()) return fail(error, "a broken SELF (its metadata)");
        for (uint32_t k = 0; k < sections; ++k) {
            const uint64_t m = 32 + uint64_t(k) * 48;
            const int segIdx = int(le32At(meta, m + 20)), keyIdx = int(le32At(meta, m + 36)), ivIdx = int(le32At(meta, m + 40));
            if (le32At(meta, m + 32) != 3) continue; // (AES-128-CTR)
            if (keyIdx < 0 || ivIdx < 0 || uint32_t(keyIdx) >= keys || uint32_t(ivIdx) >= keys || segIdx < 0 || segIdx >= phnum)
                return fail(error, "a broken SELF (its keys)");
            keyed.push_back({segIdx, std::vector<uint8_t>(meta.begin() + long(vault + 16 * uint64_t(keyIdx)), meta.begin() + long(vault + 16 * uint64_t(keyIdx) + 16)),
                             std::vector<uint8_t>(meta.begin() + long(vault + 16 * uint64_t(ivIdx)), meta.begin() + long(vault + 16 * uint64_t(ivIdx) + 16))});
        }
    }
    // the ELF: its header and program headers, each segment at its p_offset
    elf.assign(f.begin() + long(elfAt), f.begin() + long(elfAt + 52));
    for (int i = 0; i < phnum; ++i) elf.insert(elf.end(), f.begin() + long(phdrAt + uint64_t(i) * 32), f.begin() + long(phdrAt + uint64_t(i) * 32 + 32));
    for (int i = 0; i < phnum; ++i) {
        const int idx = keyed.empty() ? i : i < int(keyed.size()) ? keyed[size_t(i)].idx : -1;
        if (idx < 0) break;
        const Segment& s = segs[size_t(idx)];
        if (s.fileSize == 0) continue;
        if (s.fileOffset > elf.size()) elf.resize(s.fileOffset);
        std::vector<uint8_t> data(f.begin() + long(s.offset), f.begin() + long(s.offset + s.size));
        if (s.encrypted) {
            if (keyed.empty()) return fail(error, "a broken SELF (an encrypted segment without its key)");
            std::vector<uint8_t> iv = keyed[size_t(i)].iv;
            std::vector<uint8_t> plain(data.size());
            crypto.aes_ctr_decrypt(data.data(), plain.data(), int(data.size()), keyed[size_t(i)].key.data(), 128, iv.data());
            data.swap(plain);
        }
        if (s.compressed) {
#if defined(CR_HAVE_ZLIB)
            std::vector<uint8_t> out(size_t(s.fileSize));
            uLongf n = uLongf(out.size());
            if (uncompress(out.data(), &n, data.data(), uLong(data.size())) != Z_OK) return fail(error, "a segment doesn't inflate");
            out.resize(n);
            data.swap(out);
#else
            return fail(error, "built without zlib");
#endif
        }
        if (elf.size() > s.fileOffset) elf.resize(s.fileOffset); // (at its offset, over what came before)
        elf.insert(elf.end(), data.begin(), data.end());
    }
    return true;
#else
    (void)f, (void)klic, (void)elf;
    return fail(error, "this build can't decrypt the Vita's programs");
#endif
}

// a licence's klicensee: the dump's sce_sys/package/work.bin, else Vita3K's ux0/license/<id>/*.rif (+0x50)
bool klicenseeOf(const std::string& root, const std::string& id, uint8_t* key) {
    std::vector<std::string> tries = {withSlash(root) + "sce_sys/package/work.bin"};
    const std::string lic = parentOf(parentOf(withSlash(root))) + "license/" + id + "/";
    int count = 0;
    char** names = SDL_GlobDirectory(lic.c_str(), "*.rif", SDL_GLOB_CASEINSENSITIVE, &count);
    for (int k = 0; names && k < count; ++k) tries.push_back(lic + names[k]);
    SDL_free(names);
    for (const std::string& t : tries) {
        SDL_IOStream* w = SDL_IOFromFile(t.c_str(), "rb");
        const bool got = w && SDL_SeekIO(w, 0x50, SDL_IO_SEEK_SET) == 0x50 && SDL_ReadIO(w, key, 16) == 16;
        if (w) SDL_CloseIO(w);
        if (got) return true;
    }
    return false;
}

// The game as installed: <id>/ - or the Prequel Trilogy's PCSG00457/ with this game's folder in it
// (CR1-U, CRO-U and CR2-U's games' data in its add-ons, ux0/addcont/PCSG00457/<addon>/) -, found
// from Vita3K's folder (its own, or the pref path its config.yml names), ux0, ux0/app, a dump's
// folder (its app/; an archive's "<title> (JPN)/app/<id>/" a folder or two down), the install
// or the game's folder itself
struct VitaInstall {
    std::string root;  // <id>/ or PCSG00457/
    bool trilogy = false;
    std::string addon; // (the trilogy's: its add-on's folder, "" none)
};
std::string addonOf(const SetupGame& g, const std::string& root) {
    if (!*g.trilogyAddon) return {};
    const std::string a = parentOf(parentOf(root)) + "addcont/PCSG00457/" + g.trilogyAddon + "/";
    return isDir(a) ? a : std::string();
}
bool findVitaInstall(const std::string& picked, const SetupGame& g, VitaInstall& out, int depth = 0) {
    const std::string p = withSlash(picked), id = g.vitaId, tri = g.trilogyDir;
    std::vector<std::string> tops = {p, p + "app/", p + "ux0/app/", parentOf(p), parentOf(parentOf(p))};
    if (depth == 0) // (a dump's own folder or an archive's: the app/ a folder or two down)
        for (const std::string& top : {p, p + "app/"}) {
            int count = 0;
            char** names = SDL_GlobDirectory(top.c_str(), "*", 0, &count);
            for (int k = 0; names && k < count; ++k)
                if (isDir(top + names[k])) tops.push_back(withSlash(top + names[k])), tops.push_back(withSlash(top + names[k]) + "app/");
            SDL_free(names);
        }
    auto usable = [&](const std::string& root, bool trilogy) {
        if (!isDir(root)) return false;
        if (!trilogy) return encryptedVita(root) || g.vitaOk(root);
        return isDir(root + tri) && (encryptedVita(root) || g.vitaOk(root + tri + "/") || !addonOf(g, root).empty());
    };
    std::string lacking; // (the trilogy without this game's add-on: said so)
    for (const std::string& top : tops) {
        if (top.empty()) continue;
        for (const std::string& root : {top + id + "/", baseName(top) == id ? top : std::string()})
            if (!root.empty() && usable(root, false)) return out = {root, false, {}}, true;
        if (tri.empty()) continue;
        for (const std::string& root : {top + "PCSG00457/", baseName(top) == "PCSG00457" ? top : std::string()}) {
            if (!root.empty() && usable(root, true)) return out = {root, true, addonOf(g, root)}, true;
            if (!root.empty() && isDir(root + tri) && lacking.empty()) lacking = root;
        }
    }
    if (!lacking.empty()) out = {lacking, true, {}};
    // (Vita3K's own folder: its config.yml's pref-path, where its ux0 is)
    size_t size = 0;
    if (char* text = static_cast<char*>(SDL_LoadFile((p + "config.yml").c_str(), &size))) {
        const std::string all(text, size);
        SDL_free(text);
        const size_t at = all.find("pref-path:");
        if (at != std::string::npos && depth < 2) {
            std::string pref = all.substr(at + 10, all.find('\n', at) - at - 10);
            pref.erase(0, pref.find_first_not_of(" \t'\""));
            while (!pref.empty() && (pref.back() == '\r' || pref.back() == ' ' || pref.back() == '\'' || pref.back() == '"')) pref.pop_back();
            if (pref.rfind("./", 0) == 0 || pref.rfind(".\\", 0) == 0 || pref == ".") pref = p + pref.substr(std::min<size_t>(pref.size(), 2)); // (relative: Vita3K's folder's)
            if (!pref.empty() && withSlash(pref) != p) return findVitaInstall(pref, g, out, depth + 1);
        }
    }
    return false;
}

// The Vita release found (`in`) installed into <r>vita/<id>/ - the trilogy's PCSG00457/: a dump's
// decrypted, else copied (the trilogy's own files and this game's folder, not the other games'), the
// trilogy's add-on (this game's data) merged into this game's folder, its program decrypted beside it
// (vitaProgram). 1 done (`dest` this game's folder), 0 can't be written there, -1 not usable (`why`
// says, else `notGame`: not the game as the port reads it)
int installVita(const SetupGame& g, const VitaInstall& in, const std::string& r, std::string& dest, std::string& why, bool& notGame) {
    const std::string tri = g.trilogyDir, name = in.trilogy ? "PCSG00457" : g.vitaId;
    static const char* const kGames[] = {"CR1", "CRO", "CR2", "CR3"}; // (the Prequel Trilogy's - CR3 itself too)
    const std::string v = r + "vita/", part = v + name + ".part/", done = v + name + "/";
    if (!isDir(v) && !SDL_CreateDirectory(v.c_str())) return 0;
    if (isDir(part)) removeTree(part);
    { // (room for it: the decrypted copy - the add-on's too - beside what it is decrypted from)
        const uint64_t need = treeBytes(in.root) + (in.addon.empty() ? 0 : treeBytes(in.addon));
        std::error_code ec;
        const std::filesystem::space_info room = std::filesystem::space(fsPath(v), ec);
        if (!ec && room.available < need + (64u << 20)) {
            char text[160];
            std::snprintf(text, sizeof text, "it needs about %.1f GB free in %s, and there is %.1f GB", double(need) / 1e9, v.c_str(),
                          double(room.available) / 1e9);
            why = text;
            return notGame = true, -1;
        }
    }
    bool good;
    if (encryptedVita(in.root)) {
        {
            Progress bar(g.title, "Decrypting the Vita release ...");
            good = decryptVita(in.root, part, &why, [&](double x) { bar.show(x); });
        }
        if (!good) return removeTree(part), why.find("written") != std::string::npos ? 0 : (notGame = true, -1);
        if (in.trilogy)
            for (const char* other : kGames)
                if (tri != other) removeTree(part + other + "/"), SDL_RemovePath((part + other + ".self").c_str());
    } else {
        Progress bar(g.title, "Copying the Vita release ...");
        Copier copy{[&](double x) { bar.show(x); }, treeBytes(in.root)}; // (the trilogy's other games left: the bar ends short)
        good = SDL_CreateDirectory(part.c_str());
        struct Ctx {
            Copier* c;
            std::string part, only;
            bool ok = true;
        } ctx{&copy, part, in.trilogy ? tri : std::string()};
        if (good)
            SDL_EnumerateDirectory(
                in.root.c_str(),
                [](void* user, const char* parent, const char* n) -> SDL_EnumerationResult {
                    Ctx& x = *static_cast<Ctx*>(user);
                    const std::string p = std::string(parent) + n, s = n;
                    if (!x.only.empty())
                        for (const char* other : kGames)
                            if (x.only != other && (s == other || s == std::string(other) + ".self")) return SDL_ENUM_CONTINUE;
                    x.ok = isDir(p) ? x.c->tree(withSlash(p), x.part + s) : x.c->file(p, x.part + s);
                    return x.ok ? SDL_ENUM_CONTINUE : SDL_ENUM_FAILURE;
                },
                &ctx);
        good = good && ctx.ok;
        if (!good) return removeTree(part), 0;
    }
    if (!in.addon.empty()) { // (the trilogy's add-on: this game's data, into its folder)
        const std::string a = v + ".addon.part/";
        if (isDir(a)) removeTree(a);
        if (encryptedVita(in.addon)) {
            {
                Progress bar(g.title, "Decrypting the game's add-on ...");
                good = decryptVita(in.addon, a, &why, [&](double x) { bar.show(x); });
            }
            good = good && mergeInto(a + tri + "/", part + tri + "/");
        } else {
            Progress bar(g.title, "Copying the game's add-on ...");
            Copier copy{[&](double x) { bar.show(x); }, treeBytes(in.addon + tri + "/")};
            good = copy.tree(in.addon + tri + "/", part + tri);
        }
        removeTree(a);
        if (!good) return removeTree(part), why.empty() || why.find("written") != std::string::npos ? 0 : (notGame = true, -1);
    }
    if (g.vitaProgram) { // (its program, encrypted: decrypted beside it - <name>.elf)
        const std::string program = in.trilogy ? tri + ".self" : std::string("eboot.bin");
        size_t size = 0;
        std::vector<uint8_t> self;
        if (void* d = SDL_LoadFile((part + program).c_str(), &size)) self.assign(static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + size), SDL_free(d);
        const std::string elfName = part + program.substr(0, program.rfind('.')) + ".elf";
        if (selfEncrypted(self) && !exists(elfName)) { // (a copy an earlier setup made has it decrypted beside it already)
            uint8_t klic[16];
            std::vector<uint8_t> elf;
            if (!klicenseeOf(in.root, name, klic)) why = program + ": no licence to decrypt it with (the dump's work.bin, Vita3K's ux0/license)";
            else if (decryptSelf(self, klic, elf, &why)) {
                const std::string to = part + program.substr(0, program.rfind('.')) + ".elf";
                SDL_IOStream* o = SDL_IOFromFile(to.c_str(), "wb");
                const bool wrote = o && SDL_WriteIO(o, elf.data(), elf.size()) == elf.size();
                if (o) SDL_CloseIO(o);
                if (!wrote) return removeTree(part), 0;
            } else why = program + ": " + why;
            if (!why.empty()) return removeTree(part), notGame = true, -1;
        }
    }
    if (isDir(done)) removeTree(done);
    if (!SDL_RenamePath(part.c_str(), done.c_str())) return removeTree(part), 0;
    const std::string gameDir = in.trilogy ? done + tri + "/" : done;
    if (!g.vitaOk(gameDir) && !g.vitaOk(done)) return removeTree(done), notGame = true, -1;
    dest = gameDir;
    return 1;
}

// a job done in the game folder, else - it can't be written there - in the profile folder: `job`
// returns 1 done, 0 not (the next folder tried), -1 not and no use trying again
bool inGameFolder(const std::vector<std::string>& roots, const std::function<int(const std::string&)>& job) {
    for (const std::string& r : roots) {
        bool had[4];
        static const char* const kKinds[4] = {"ios", "android", "vita", "translation"};
        for (int k = 0; k < 4; ++k) had[k] = isDir(r + kKinds[k]);
        const int got = job(r);
        if (got <= 0) // (a folder it made for nothing: gone again - SDL_RemovePath takes an empty one alone)
            for (int k = 0; k < 4; ++k)
                if (!had[k]) SDL_RemovePath((r + kKinds[k]).c_str());
        if (got != 0) return got > 0;
    }
    return false;
}

// the Android files: those picked that are packages, and beside them the OBB of its name and the
// APKs carrying its package (not every APK in a downloads folder - another game's)
std::vector<std::string> androidFiles(const SetupGame& g, const std::vector<std::string>& picked) {
    std::vector<std::string> files;
    auto add = [&](const std::string& f) {
        if (std::find(files.begin(), files.end(), f) == files.end()) files.push_back(f);
    };
    bool obb = false, apk = false;
    for (const std::string& f : picked) {
        if (endsWith(f, ".apks") || endsWith(f, ".xapk")) add(f), obb = apk = true;
        else if (endsWith(f, ".obb")) add(f), obb = true;
        else if (endsWith(f, ".apk")) add(f), apk = true;
    }
    std::vector<std::string> dirs;
    for (const std::string& f : picked)
        if (const std::string d = parentOf(f); !d.empty() && std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
    std::string pkg = g.package;
    for (char& c : pkg) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const std::string& d : dirs) {
        int count = 0;
        char** names = SDL_GlobDirectory(d.c_str(), "*", 0, &count);
        for (int k = 0; names && k < count; ++k) {
            const std::string n = names[k];
            std::string low = n;
            for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (!obb && n == g.obbHint) add(d + n);
            if (!apk && !pkg.empty() && endsWith(low, ".apk") && low.find(pkg) != std::string::npos) add(d + n);
        }
        SDL_free(names);
    }
    return files;
}

} // namespace

bool runSetup(const SetupGame& g, const std::string& folder, const std::string& fallback, SetupResult& out) {
    std::vector<std::string> roots{withSlash(folder)};
    if (!fallback.empty() && withSlash(fallback) != roots[0]) roots.push_back(withSlash(fallback));
    staging() = roots[0] + ".picked/"; // (the picks handed over as URIs, copied in: gone when this ends)
    struct Tidy {
        ~Tidy() {
            if (isDir(staging())) removeTree(staging());
        }
    } tidy;
    for (;;) {
        std::string text = std::string(g.port) + " plays your own copy of " + g.title +
                           ". Choose where it is - any one of them; the files it needs are copied into its folder:\n\n";
        const bool obb = endsWith(g.obbHint, ".obb"); // (CR3: no OBB - its .apks, or the APK and its asset packs)
        const std::string androidLine = obb ? "  Android - the APK and its " + std::string(g.obbHint) + " (both at once)\n"
                                            : std::string("  Android - the .apks (or the APK and its asset packs, all at once)\n");
        if (phone())
            text += "  iOS - the .ipa (an unpacked .app: copy it into ios/ over USB)\n" + androidLine +
                    "  Vita - a NoNpDrm dump: packed (.zip, .rar, .vpk), or its folder copied in over USB\n\n"
                    "What is copied into its folder over USB is found with Look again.\n\n";
        else
            text += "  iOS - the .ipa (or a file in its unpacked .app)\n" + androidLine +
                    "  Vita - Vita3K's folder with the game installed (or the game's " + std::string(g.vitaId) + " folder)\n\n";
        text += "Its folder: " + roots[0];
        const bool any = !out.ios.empty() || !out.android.empty() || !out.vita.empty();
        if (any) {
            text += "\n\nIn place:";
            if (!out.ios.empty()) text += "\n  iOS: " + out.ios;
            if (!out.android.empty()) text += "\n  Android: " + out.android;
            if (!out.vita.empty()) text += "\n  Vita: " + out.vita;
            if (!out.patch.empty()) text += "\n  Its fan translation: " + out.patch;
            if (!g.oneRelease) text += "\n\nMore of them add to it (the Vita's voices and music, Android's art); or Start.";
        }
        std::vector<std::pair<int, const char*>> buttons = {{1, "iOS (.ipa)..."}, {2, obb ? "Android (APK + OBB)..." : "Android (.apks)..."}, {3, phone() ? "Vita..." : "Vita / Vita3K..."}};
        if (any) buttons.push_back({4, "Start"});
        else if (phone()) buttons.push_back({5, "Look again"}); // (copied in over USB meanwhile: found as on any start)
        buttons.push_back({0, "Quit"});
        const int choice = ask(g, text, buttons, SDL_MESSAGEBOX_INFORMATION);
        if (choice <= 0) return false;
        if (choice == 4 || choice == 5) return true;
        if (choice == 1) { // (the .ipa unpacked into ios/; an unpacked .app copied there)
            static const SDL_DialogFileFilter kIpa[] = {{"iOS app (.ipa)", "ipa"}, {"Any file (one in an unpacked .app)", "*"}};
            const std::vector<std::string> picked = pickFiles(g, kIpa, 2, false);
            if (picked.empty()) continue;
            const std::string& f = picked.front();
            if (phone() && !endsWith(f, ".ipa")) {
                tell(g, "That isn't an .ipa. Choose the .ipa - or copy the unpacked .app into " + roots[0] + "ios/ over USB, then Look again.");
                continue;
            }
            std::string app, why;
            bool ok;
            if (endsWith(f, ".ipa") || endsWith(f, ".zip")) {
                Progress bar(g.title, "Unpacking " + baseName(f) + " ...");
                ok = inGameFolder(roots, [&](const std::string& r) {
                    SDL_CreateDirectory((r + "ios").c_str());
                    if (unpackIpa(f, r + "ios/", app, &why, [&](double v) { bar.show(v); })) return 1;
                    return isDir(r + "ios") && why.find("can't be") == std::string::npos ? -1 : 0; // (a broken .ipa: the same anywhere)
                });
            } else {
                std::string dir = parentOf(f);
                while (!dir.empty() && !endsWith(dir.substr(0, dir.size() - 1), ".app")) dir = parentOf(dir);
                if (dir.empty()) dir = parentOf(f);
                if (!g.iosOk(dir)) {
                    tell(g, "No " + std::string(g.title) + " data in " + dir);
                    continue;
                }
                Progress bar(g.title, "Copying " + baseName(dir) + " ...");
                Copier copy{[&](double v) { bar.show(v); }, treeBytes(dir)};
                ok = inGameFolder(roots, [&](const std::string& r) {
                    SDL_CreateDirectory((r + "ios").c_str());
                    const std::string part = r + "ios/" + baseName(dir) + ".part/", done = r + "ios/" + baseName(dir) + "/";
                    copy.done = 0;
                    if (!copy.tree(dir, part)) return removeTree(part), 0;
                    if (isDir(done)) removeTree(done);
                    if (!SDL_RenamePath(part.c_str(), done.c_str())) return removeTree(part), 0;
                    app = done;
                    return 1;
                });
                why = "can't be written";
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
            out.ios = app;
        } else if (choice == 2) { // (the APK and the OBB - or the .apks - copied into android/)
            static const SDL_DialogFileFilter kAndroid[] = {{"Android (.apk, .obb, .apks)", "apk;obb;apks;xapk"}, {"Any file", "*"}};
            const std::vector<std::string> picked = pickFiles(g, kAndroid, 2, true);
            if (picked.empty()) continue;
            const std::vector<std::string> files = androidFiles(g, picked);
            if (files.empty()) {
                tell(g, std::string("Choose ") + g.title + "'s Android release: the APK and its " + g.obbHint + ", both at once.");
                continue;
            }
            uint64_t total = 0;
            for (const std::string& f : files) total += fileBytes(f);
            Progress bar(g.title, "Copying the Android release ...");
            Copier copy{[&](double v) { bar.show(v); }, total};
            std::string dest;
            bool notGame = false;
            const bool ok = inGameFolder(roots, [&](const std::string& r) {
                const std::string a = r + "android/";
                if (!isDir(a) && !SDL_CreateDirectory(a.c_str())) return 0;
                copy.done = 0;
                std::vector<std::string> made;
                for (const std::string& f : files) {
                    const std::string to = a + baseName(f);
                    const bool placed = staged(f) ? SDL_RenamePath(f.c_str(), (to + ".part").c_str()) : copy.file(f, to + ".part"); // (a pick copied in: moved)
                    if (!placed || (SDL_RemovePath(to.c_str()), !SDL_RenamePath((to + ".part").c_str(), to.c_str()))) {
                        SDL_RemovePath((to + ".part").c_str());
                        for (const std::string& m : made) SDL_RemovePath(m.c_str());
                        return 0;
                    }
                    made.push_back(to);
                }
                // (no APK chosen nor one by its package's name: each APK beside the picks tried in turn -
                // CR1's is named 05818-chaos-rings-1.1.1.apk -, the one that makes it whole kept)
                if (!g.androidOk(a) && std::none_of(files.begin(), files.end(), [](const std::string& f) { return endsWith(f, ".apk") || endsWith(f, ".apks") || endsWith(f, ".xapk"); }))
                    for (const std::string& d : {parentOf(files.front())}) {
                        int count = 0;
                        char** names = SDL_GlobDirectory(d.c_str(), "*.apk", SDL_GLOB_CASEINSENSITIVE, &count);
                        for (int k = 0; names && k < count && !g.androidOk(a); ++k) {
                            const std::string from = d + names[k], to = a + names[k];
                            if (fileBytes(from) > (uint64_t(256) << 20) || !copy.file(from, to)) continue; // (an APK, not a game's data)
                            if (g.androidOk(a)) made.push_back(to);
                            else SDL_RemovePath(to.c_str());
                        }
                        SDL_free(names);
                    }
                if (!g.androidOk(a)) { // (not the game's: taken out again)
                    for (const std::string& m : made) SDL_RemovePath(m.c_str());
                    notGame = true;
                    return -1;
                }
                dest = a;
                return 1;
            });
            if (!ok) {
                std::string names;
                for (const std::string& f : files) names += "\n  " + baseName(f);
                tell(g, notGame ? std::string("Not ") + g.title + "'s Android release (or not all of it):" + names + "\n\nChoose the APK and its " + g.obbHint +
                                      ", both at once."
                                : "The Android release can't be copied into " + roots[0] + "android/ (nor the profile folder's).");
                continue;
            }
            out.android = dest;
        } else if (choice == 3) { // (the Vita release copied - a NoNpDrm dump's decrypted - into vita/<id>/)
            int how = 1;
            if (phone())
                how = ask(g,
                          "Where is the Vita release?\n\n"
                          "  A folder - copied into " + roots[0] + "vita/ over USB: a NoNpDrm dump's as it was dumped (its app/, and addcont/ for the "
                          "Prequel Trilogy), Vita3K's ux0, or the game's own folder\n"
                          "  An archive - a NoNpDrm dump packed as .zip, .rar or .vpk (a .7z: unpack it on a PC)",
                          kArchives ? std::vector<std::pair<int, const char*>>{{1, "Copied in..."}, {2, "An archive..."}, {0, "Back"}}
                                    : std::vector<std::pair<int, const char*>>{{1, "Copied in..."}, {0, "Back"}},
                          SDL_MESSAGEBOX_INFORMATION);
            else if (kArchives)
                how = ask(g,
                          "Where is the Vita release?\n\n"
                          "  A folder - Vita3K's (or its ux0), the game's own, or a NoNpDrm dump's\n"
                          "  An archive - a NoNpDrm dump packed as .rar, .7z, .zip or .vpk",
                          {{1, "A folder..."}, {2, "An archive..."}, {0, "Back"}}, SDL_MESSAGEBOX_INFORMATION);
            if (how <= 0) continue;
            std::string picked, unpacked; // (an archive's: unpacked into vita/.unpack/, gone after)
            if (how == 2) {
                static const SDL_DialogFileFilter kDump[] = {{"A Vita dump (.rar, .7z, .zip, .vpk)", "rar;7z;zip;vpk"}, {"Any file", "*"}};
                const std::vector<std::string> files = pickFiles(g, kDump, 2, false);
                if (files.empty()) continue;
                // (this game's parts alone: those under its title id - the trilogy's PCSG00457 and its add-on, not the other games')
                const std::string id = g.vitaId, addon = g.trilogyAddon;
                auto want = [&](const std::string& n) {
                    const std::string s = "/" + n + "/";
                    if (s.find("/" + id + "/") != std::string::npos) return true;
                    if (!*g.trilogyDir || s.find("/PCSG00457/") == std::string::npos) return false;
                    return s.find("/addcont/") == std::string::npos || (!addon.empty() && s.find("/" + addon + "/") != std::string::npos);
                };
                Progress bar(g.title, "Unpacking " + baseName(files.front()) + " ...");
                std::string why;
                const bool ok = inGameFolder(roots, [&](const std::string& r) {
                    const std::string v = r + "vita/", u = v + ".unpack/";
                    if (!isDir(v) && !SDL_CreateDirectory(v.c_str())) return 0;
                    if (isDir(u)) removeTree(u);
                    if (!SDL_CreateDirectory(u.c_str())) return 0;
                    if (unpackArchive(files.front(), u, want, &why, [&](double x) { bar.show(x); })) return unpacked = u, 1;
                    removeTree(u);
                    return why.find("can't be written") != std::string::npos ? 0 : -1;
                });
                if (!ok) {
                    tell(g, baseName(files.front()) + ": " + why);
                    continue;
                }
                picked = unpacked;
            } else if (phone()) { // (no folder dialog: the folder it was copied into)
                const std::string v = roots[0] + "vita/";
                SDL_CreateDirectory(v.c_str());
                if (ask(g, "Copy the Vita release's folder into\n  " + v + "\nover USB from a PC - a NoNpDrm dump's as it was dumped (its app/, and addcont/ "
                           "for the Prequel Trilogy), Vita3K's ux0, or the game's own folder -, then Continue.",
                        {{1, "Continue"}, {0, "Back"}}, SDL_MESSAGEBOX_INFORMATION) != 1)
                    continue;
                picked = v;
            } else {
                picked = pickFolder();
                if (picked.empty()) continue;
            }
            VitaInstall in;
            if (!findVitaInstall(picked, g, in)) {
                if (!unpacked.empty()) removeTree(unpacked);
                if (in.trilogy) // (the Prequel Trilogy, this game's data - its add-on - not with it)
                    tell(g, std::string("The Prequel Trilogy keeps ") + g.title + "'s data in its add-on (addcont/PCSG00457/" + g.trilogyAddon + "), and it isn't beside " +
                                in.root + " - install the add-on in Vita3K too, or choose a dump with it.");
                else tell(g, std::string("No ") + g.title + " (" + g.vitaId + ") in " + (unpacked.empty() ? picked : std::string("the archive")) +
                            " - choose Vita3K's folder, its ux0, the game's own folder or a NoNpDrm dump of it.");
                continue;
            }
            // the install, into vita/<id>/: a dump's decrypted, else copied - the trilogy's own files
            // and this game's folder, not the other games' -, then the trilogy's add-on (this game's
            // data) merged into this game's folder
            const std::string tri = g.trilogyDir;
            std::string dest, why;
            bool notGame = false;
            // (copied in decrypted, already where the install would put it: read where it is)
            const std::string placed = roots[0] + "vita/" + (in.trilogy ? std::string("PCSG00457") : std::string(g.vitaId)) + "/",
                              placedGame = in.trilogy ? placed + tri + "/" : placed;
            const bool inPlace = withSlash(in.root) == placed && !encryptedVita(in.root) && in.addon.empty() && g.vitaOk(placedGame);
            const bool ok = inPlace ? (dest = placedGame, true)
                                    : inGameFolder(roots, [&](const std::string& r) { return installVita(g, in, r, dest, why, notGame); });
            if (!ok) {
                if (!unpacked.empty()) removeTree(unpacked);
                if (notGame && !why.empty()) tell(g, std::string("The Vita release in ") + in.root + ": " + why);
                else if (notGame && in.trilogy && in.addon.empty())
                    tell(g, std::string("The Prequel Trilogy keeps ") + g.title + "'s data in its add-on (addcont/PCSG00457/" + g.trilogyAddon +
                                "), and it isn't beside " + in.root + " - install it in Vita3K too, or choose the dump with it.");
                else if (notGame) tell(g, in.root + " isn't " + g.title + "'s Vita release as the port reads it.");
                else tell(g, std::string("The Vita release in ") + in.root + " can't be copied into " + roots[0] + "vita/ (nor the profile folder's).");
                continue;
            }
            out.vita = dest;
            const std::string game = in.trilogy ? in.root + tri + "/" : in.root; // (where its fan translation is looked for)
            if (g.patchOk && (!g.untranslated || g.untranslated(dest))) { // (the fan translation, over an untranslated copy: Vita3K's ux0/rePatch/<id> by itself, else asked for - into translation/<id>/)
                out.patch.clear();
                std::string from;
                const std::string t0 = roots[0] + "translation/";
                const std::string up2 = parentOf(parentOf(game)), up3 = parentOf(up2); // (<ux0>/app/<id>/ - or the trilogy's <id>/CR3/: one more)
                for (const std::string& t : {up2 + "rePatch/" + g.vitaId + "/", up3 + "rePatch/" + g.vitaId + "/"})
                    if (from.empty() && isDir(t) && g.patchOk(t)) from = t;
                while (from.empty() && out.patch.empty()) {
                    std::vector<std::pair<int, const char*>> b = {{1, phone() ? "Copied in..." : "Choose its folder..."}};
                    if (kArchives) b.push_back({2, "Its archive..."});
                    b.push_back({0, "Without it"});
                    const int a = ask(g,
                                      std::string("This Vita release is in Japanese. A fan translation puts its text over it - gbatemp's English one or "
                                                  "Joseas's Spanish one: its rePatch folder (") + g.vitaId + "), or the .rar / .zip it comes in. Is one here too?",
                                      b, SDL_MESSAGEBOX_INFORMATION);
                    if (a == 2) { // (its archive: unpacked, its <id>/ kept as translation/<id>/)
                        static const SDL_DialogFileFilter kPatch[] = {{"The fan translation (.rar, .zip, .7z)", "rar;zip;7z"}, {"Any file", "*"}};
                        const std::vector<std::string> files = pickFiles(g, kPatch, 2, false);
                        if (files.empty()) continue;
                        std::string got, why2;
                        {
                            Progress pbar(g.title, "Unpacking the fan translation ...");
                            got = unpackPatch(g, files.front(), t0, &why2, [&](double v) { pbar.show(v); });
                        }
                        if (got.empty()) tell(g, baseName(files.front()) + ": " + why2);
                        else out.patch = got;
                        continue;
                    }
                    if (a != 1) break;
                    std::string chosen;
                    if (phone()) {
                        SDL_CreateDirectory(t0.c_str());
                        if (ask(g, "Copy the fan translation's rePatch folder (or the " + std::string(g.vitaId) + " in it) into\n  " + t0 +
                                       "\nover USB from a PC, then Continue.",
                                {{1, "Continue"}, {0, "Back"}}, SDL_MESSAGEBOX_INFORMATION) != 1)
                            continue;
                        chosen = t0;
                    } else if ((chosen = pickFolder()).empty()) continue;
                    for (const std::string& t : {withSlash(chosen) + g.vitaId + "/", withSlash(chosen) + "rePatch/" + g.vitaId + "/", withSlash(chosen)}) // (its own first: translation/ may hold another already)
                        if (from.empty() && isDir(t) && g.patchOk(t)) from = t;
                    if (from.empty()) tell(g, "No fan translation in " + chosen + " - " + (phone() ? "copy" : "choose") + " its rePatch folder (or the " + g.vitaId + " in it).");
                }
                if (!from.empty()) { // (its <id>/ alone, where that is the translation)
                    if (const std::string own = patchFolder(from, g.vitaId); own != from && g.patchOk(own)) from = own;
                }
                const std::string named = from.empty() ? std::string() : patchDir(g, from);
                if (!from.empty() && withSlash(from) == t0 + named + "/") out.patch = withSlash(from); // (copied in where it belongs)
                else if (!from.empty() && !staged(from) && from.compare(0, t0.size(), t0) == 0) { // (copied in elsewhere in translation/: moved to translation/<id>/)
                    const std::string done = t0 + named + "/";
                    if (isDir(done)) removeTree(done);
                    if (SDL_RenamePath(from.c_str(), done.c_str())) out.patch = done;
                    else tell(g, "The fan translation can't be moved to " + done);
                } else if (!from.empty()) {
                    Progress pbar(g.title, "Copying the fan translation ...");
                    Copier pc{[&](double v) { pbar.show(v); }, treeBytes(from)};
                    std::string to;
                    inGameFolder(roots, [&](const std::string& r) {
                        const std::string t = r + "translation/", part = t + named + ".part/", done = t + named + "/";
                        if (!isDir(t) && !SDL_CreateDirectory(t.c_str())) return 0;
                        if (isDir(part)) removeTree(part);
                        pc.done = 0;
                        if (!pc.tree(from, part)) return removeTree(part), 0;
                        if (isDir(done)) removeTree(done);
                        if (!SDL_RenamePath(part.c_str(), done.c_str())) return removeTree(part), 0;
                        to = done;
                        return 1;
                    });
                    if (to.empty()) tell(g, "The fan translation can't be copied into " + roots[0] + "translation/");
                    out.patch = to;
                }
            }
            if (!unpacked.empty()) removeTree(unpacked);
        }
        if (g.oneRelease && (!out.ios.empty() || !out.android.empty() || !out.vita.empty())) return true;
    }
}

bool installDroppedVita(const SetupGame& g, const std::string& folder, SetupResult& out, bool quiet) {
    const std::string v = withSlash(folder) + "vita/";
    if (!isDir(v)) return false;
    VitaInstall in;
    if (!findVitaInstall(v, g, in)) return false; // (none; the trilogy without this game's add-on: the message where no copy is found)
    if (!encryptedVita(in.root) && (in.addon.empty() || !encryptedVita(in.addon))) return false; // (installed, or put there decrypted: read as it is)
    const bool video = SDL_InitSubSystem(SDL_INIT_VIDEO); // (the progress window, the messages)
    SDL_Log("setup: a Vita dump in %s, decrypting it into %s", in.root.c_str(), v.c_str());
    std::string dest, why;
    bool notGame = false;
    const int got = installVita(g, in, withSlash(folder), dest, why, notGame);
    auto say = [&](const std::string& text) { note(g, text, quiet); };
    if (got > 0) {
        out.vita = dest;
        if (withSlash(in.root) != withSlash(v + (in.trilogy ? "PCSG00457" : g.vitaId)))
            say(std::string("The Vita release is decrypted into ") + v + (in.trilogy ? "PCSG00457" : g.vitaId) + "/.\n\nThe dump you copied in (" + in.root +
                (in.addon.empty() ? std::string() : " and " + in.addon) + ") isn't needed any more - delete it to get the space back.");
    } else if (notGame && !why.empty()) {
        say(std::string("The Vita dump in ") + in.root + " can't be decrypted: " + why);
    } else if (notGame && in.trilogy && in.addon.empty()) {
        say(std::string("The Prequel Trilogy keeps ") + g.title + "'s data in its add-on (addcont/PCSG00457/" + g.trilogyAddon + "), and it isn't beside " +
            in.root + " - copy the dump's addcont folder in too, beside its app folder.");
    } else if (notGame) {
        say(in.root + " isn't " + g.title + "'s Vita release as the port reads it.");
    } else {
        say(std::string("The Vita dump can't be decrypted into ") + v + " - it can't be written there.");
    }
    if (video) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return got > 0;
}

bool installDroppedPatch(const SetupGame& g, const std::string& folder, bool quiet) {
    if (!g.patchOk) return false;
    const std::string t = withSlash(folder) + "translation/";
    if (!isDir(t)) return false;
    // each .rar / .zip / .7z there unpacked once - translation/.unpacked lists those done (name and
    // size): the English one's and the Spanish one's both, one dropped in after the other too
    const std::string listPath = t + ".unpacked";
    std::string list;
    if (std::FILE* f = std::fopen(listPath.c_str(), "rb")) {
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) list.append(buf, n);
        std::fclose(f);
    }
    std::vector<std::string> archives;
    int count = 0;
    char** names = SDL_GlobDirectory(t.c_str(), "*", 0, &count);
    for (int k = 0; names && k < count; ++k) {
        const std::string n = names[k], p = t + n;
        if (n.empty() || n[0] == '.' || isDir(p)) continue;
        if (!(endsWith(n, ".rar") || endsWith(n, ".zip") || endsWith(n, ".7z"))) continue;
        SDL_PathInfo info;
        const std::string line = n + " " + std::to_string(SDL_GetPathInfo(p.c_str(), &info) ? info.size : 0) + "\n";
        if (list.find(line) == std::string::npos) archives.push_back(p);
    }
    SDL_free(names);
    if (archives.empty()) return false;
    if (!kArchives) {
        note(g, baseName(archives.front()) + " can't be opened by this build - unpack it into " + t + " first.", quiet);
        return false;
    }
    const bool video = SDL_InitSubSystem(SDL_INIT_VIDEO);
    bool any = false;
    for (const std::string& archive : archives) {
        SDL_Log("setup: the fan translation's %s in %s, unpacking it", baseName(archive).c_str(), t.c_str());
        std::string why, got;
        {
            Progress bar(g.title, "Unpacking the fan translation ...");
            got = unpackPatch(g, archive, t, &why, [&](double v) { bar.show(v); });
        }
        if (got.empty()) note(g, baseName(archive) + " can't be unpacked: " + why, quiet);
        else note(g, "The fan translation is unpacked into " + got + " - " + baseName(archive) + " isn't needed any more: delete it to get the space back.", quiet);
        any = any || !got.empty();
        SDL_PathInfo info; // (listed either way: said once, not at every start)
        list += baseName(archive) + " " + std::to_string(SDL_GetPathInfo(archive.c_str(), &info) ? info.size : 0) + "\n";
    }
    if (std::FILE* f = std::fopen(listPath.c_str(), "wb")) {
        std::fwrite(list.data(), 1, list.size(), f);
        std::fclose(f);
    }
    if (video) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return any;
}

} // namespace cr
