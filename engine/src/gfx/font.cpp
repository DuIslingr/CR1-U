#include "gfx/font.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#include "core/file.hpp"
#include "core/text.hpp"
#include "platform/paths.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"

#ifdef SDL_PLATFORM_APPLE
#include <CoreText/CoreText.h>
#endif

namespace cr {

struct Font::Face {
    stbtt_fontinfo info;
};

namespace {
// The face's glyph for `c`, with stand-ins for what iOS's font fallback would bring: the Korean
// text's U+223C TILDE OPERATOR (sys.evt's skill lines, "2 U+223C 5") is in no Noto Sans CJK face - the
// fullwidth tilde (or the wave dash) in its place
int glyphIndex(const stbtt_fontinfo& info, uint32_t c) {
    int g = stbtt_FindGlyphIndex(&info, int(c));
    if (!g && c == 0x223C) g = stbtt_FindGlyphIndex(&info, 0xFF5E);
    if (!g && c == 0x223C) g = stbtt_FindGlyphIndex(&info, 0x301C);
    return g;
}
// Characters that need no glyph (spaces)
bool blank(uint32_t c) { return c <= 0x20 || c == 0x3000; }
} // namespace

Font::Font() = default;
Font::~Font() = default;

bool Font::openFace(int index) {
    const int offset = stbtt_GetFontOffsetForIndex(data_->data(), index);
    if (offset < 0) return false;
    auto f = std::make_unique<Face>();
    if (!stbtt_InitFont(&f->info, data_->data(), offset)) return false;
    face_ = std::move(f);
    faceIndex_ = index;
    ++generation_;
    return true;
}

bool Font::load(const std::string& path) {
    face_.reset();
    faceIndex_ = -1;
    ++generation_;
    auto bytes = std::make_shared<std::vector<unsigned char>>();
    if (!readFile(path, *bytes) || bytes->size() < 12) {
        data_.reset();
        return false;
    }
    data_ = std::move(bytes);
    if (!openFace(0)) {
        data_.reset();
        return false;
    }
    path_ = path;
    return true;
}

void Font::assign(const Font& other) {
    if (&other == this) return;
    face_.reset();
    faceIndex_ = -1;
    ++generation_;
    data_ = other.data_;
    path_ = other.path_;
    if (data_ && other.faceIndex_ >= 0 && !openFace(other.faceIndex_)) data_.reset();
}

bool Font::loaded() const { return face_ != nullptr; }

int Font::generation() const { return generation_ * 4096 + (fallback_ ? fallback_->generation() + 1 : 0); }

void Font::setFallback(const Font* fallback) {
    if (fallback == this) fallback = nullptr;
    if (fallback != fallback_) fallback_ = fallback, ++generation_;
}

void Font::setTypoMetrics(bool on) {
    if (on != typo_) typo_ = on, ++generation_;
}

bool Font::selectRegion(const char* region) {
    if (!loaded()) return false;
    const unsigned char* d = data_->data();
    const int n = stbtt_GetNumberOfFonts(d);
    const size_t rl = std::strlen(region);
    for (int i = 0; i < n; ++i) {
        stbtt_fontinfo info;
        if (!stbtt_InitFont(&info, d, stbtt_GetFontOffsetForIndex(d, i))) continue;
        int len = 0; // full name (id 4), Windows platform, US English: UTF-16BE
        const char* name = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                                   STBTT_MS_LANG_ENGLISH, 4);
        if (!name || len < int(2 * rl)) continue;
        std::string ascii;
        for (int k = 1; k < len; k += 2) ascii += name[k];
        // "Noto Sans CJK JP" but not "Noto Sans Mono CJK JP" ("Noto Sans CJK JP Bold" in the bold collection)
        const size_t at = ascii.rfind(region);
        if (at != std::string::npos && ascii.find("Mono") == std::string::npos &&
            (at + rl == ascii.size() || ascii.compare(at + rl, std::string::npos, " Bold") == 0) &&
            (at == 0 || ascii[at - 1] == ' ')) {
            if (i == faceIndex_) return false;
            return openFace(i);
        }
    }
    return false;
}

bool Font::selectPostScript(const char* name) {
    if (!loaded() || !name) return false;
    const unsigned char* d = data_->data();
    const int n = std::max(1, stbtt_GetNumberOfFonts(d));
    for (int i = 0; i < n; ++i) {
        stbtt_fontinfo info;
        if (!stbtt_InitFont(&info, d, stbtt_GetFontOffsetForIndex(d, i))) continue;
        std::string ps;
        int len = 0; // PostScript name (id 6): the Windows table's UTF-16BE, else the Mac table's Roman
        if (const char* u = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 6))
            for (int k = 1; k < len; k += 2) ps += u[k];
        else if (const char* m = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN, STBTT_MAC_LANG_ENGLISH, 6))
            ps.assign(m, size_t(len));
        if (ps == name) return i == faceIndex_ || openFace(i);
    }
    return false;
}

bool Font::covers(const std::string& utf8) const {
    if (!loaded()) return false;
    const stbtt_fontinfo* other = fallback_ && fallback_->loaded() ? &fallback_->face_->info : nullptr;
    for (size_t i = 0; i < utf8.size();) {
        const uint32_t c = nextUtf8(utf8, i);
        if (!blank(c) && !glyphIndex(face_->info, c) && !(other && glyphIndex(*other, c))) return false;
    }
    return true;
}

bool Font::rasterise(const std::vector<std::string>& lines, float em, float lineStep, int pad, Image& out) const {
    if (!loaded() || lines.empty()) return false;
    const stbtt_fontinfo& info = face_->info;
    const stbtt_fontinfo* other = fallback_ && fallback_->loaded() ? &fallback_->face_->info : nullptr;
    const float scale = stbtt_ScaleForMappingEmToPixels(&info, em);
    const float otherScale = other ? stbtt_ScaleForMappingEmToPixels(other, em) : 0.0f;
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    if (typo_) { // (the OS/2 table's, when the face has one)
        int ta = 0, td = 0, tg = 0;
        if (stbtt_GetFontVMetricsOS2(&info, &ta, &td, &tg) && ta > 0) ascent = ta, descent = td;
    }
    const float base0 = float(pad) + float(ascent) * scale;

    // Glyphs per line - this face's, else the fallback's ('?' for characters neither has) - then the widest line
    struct Glyph {
        const stbtt_fontinfo* face;
        float scale;
        int index;
    };
    std::vector<std::vector<Glyph>> glyphs(lines.size());
    const int unknown = stbtt_FindGlyphIndex(&info, '?');
    float widest = 1;
    for (size_t l = 0; l < lines.size(); ++l) {
        float x = 0;
        Glyph prev{nullptr, 0, 0};
        for (size_t i = 0; i < lines[l].size();) {
            const uint32_t c = nextUtf8(lines[l], i);
            if (c < 0x20) continue; // (a message's marks, such as the centred line's \x01: not drawn)
            Glyph g{&info, scale, glyphIndex(info, c)};
            if (!g.index && !blank(c) && other) {
                if (const int k = glyphIndex(*other, c)) g = {other, otherScale, k};
            }
            if (!g.index && !blank(c)) g.index = unknown;
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(g.face, g.index, &adv, &lsb);
            if (prev.face == g.face) x += float(stbtt_GetGlyphKernAdvance(g.face, prev.index, g.index)) * g.scale;
            x += float(adv) * g.scale;
            glyphs[l].push_back(g);
            prev = g;
        }
        widest = std::max(widest, x);
    }
    out.width = int(std::ceil(widest)) + 2 * pad + 1;
    out.height = int(std::ceil(float(lines.size() - 1) * lineStep + float(ascent - descent) * scale)) + 2 * pad;
    out.alphaOnly = false;
    out.rgba.assign(size_t(out.width) * out.height * 4, 0);
    std::vector<unsigned char> bmp;
    for (size_t l = 0; l < lines.size(); ++l) {
        const float baseline = base0 + float(l) * lineStep;
        float x = float(pad);
        const Glyph* prev = nullptr;
        for (const Glyph& g : glyphs[l]) { // (the vector is complete here: pointers into it hold)
            if (prev && prev->face == g.face) x += float(stbtt_GetGlyphKernAdvance(g.face, prev->index, g.index)) * g.scale;
            prev = &g;
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(g.face, g.index, &adv, &lsb);
            const float fx = x - std::floor(x);
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(g.face, g.index, g.scale, g.scale, fx, 0, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                bmp.assign(size_t(gw) * gh, 0);
                stbtt_MakeGlyphBitmapSubpixel(g.face, bmp.data(), gw, gh, gw, g.scale, g.scale, fx, 0, g.index);
                const int ox = int(std::floor(x)) + x0, oy = int(std::lround(baseline)) + y0;
                for (int y = 0; y < gh; ++y) {
                    const int ty = oy + y;
                    if (ty < 0 || ty >= out.height) continue;
                    for (int xx = 0; xx < gw; ++xx) {
                        const int tx = ox + xx;
                        if (tx < 0 || tx >= out.width) continue;
                        uint8_t* p = &out.rgba[(size_t(ty) * out.width + tx) * 4];
                        p[0] = p[1] = p[2] = 255;
                        p[3] = std::max(p[3], bmp[size_t(y) * gw + xx]);
                    }
                }
            }
            x += float(adv) * g.scale;
        }
    }
    return true;
}

#ifdef SDL_PLATFORM_APPLE
bool loadAppleFont(Font& font, const char* postScriptName) {
    CFStringRef name = CFStringCreateWithCString(nullptr, postScriptName, kCFStringEncodingUTF8);
    if (!name) return false;
    std::string path, found;
    if (CTFontDescriptorRef desc = CTFontDescriptorCreateWithNameAndSize(name, 0)) {
        // (CoreText answers an unknown name with a stand-in: its own name checked)
        if (CFStringRef got = static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(desc, kCTFontNameAttribute))) {
            char buf[256];
            if (CFStringGetCString(got, buf, sizeof buf, kCFStringEncodingUTF8)) found = buf;
            CFRelease(got);
        }
        if (CFURLRef url = static_cast<CFURLRef>(CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute))) {
            char buf[1024];
            if (CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(buf), sizeof buf)) path = buf;
            CFRelease(url);
        }
        CFRelease(desc);
    }
    CFRelease(name);
    if (path.empty() || found != postScriptName) return false;
    if (font.loaded() && font.path() == path) return font.selectPostScript(postScriptName) || true;
    if (!font.load(path)) return false;
    font.selectPostScript(postScriptName); // (a collection's: that face)
    return true;
}
#else
bool loadAppleFont(Font&, const char*) { return false; }
#endif

// Android's fonts beyond the AOSP file names: a phone maker's system may name or place its CJK font otherwise.
// The system's own font list - /system/etc/fonts.xml, font_fallback.xml from Android 15, the product / vendor
// partitions' - names each family's files and languages: Japanese's first, then Chinese (Simplified,
// Traditional) and Korean, then the default sans-serif family; after them, any font in the font folders,
// CJK-named ones first. In the order to try
std::vector<std::string> androidFonts(const std::string& root) {
    static const char* const kDirs[] = {"/system/fonts/", "/product/fonts/", "/system_ext/fonts/", "/vendor/fonts/"};
    auto locate = [&root](const std::string& name) -> std::string {
        if (!name.empty() && name[0] == '/') return SDL_GetPathInfo((root + name).c_str(), nullptr) ? root + name : std::string();
        for (const char* d : kDirs)
            if (const std::string p = root + d + name; SDL_GetPathInfo(p.c_str(), nullptr)) return p;
        return {};
    };
    auto attr = [](const std::string& tag, const char* key) -> std::string {
        const std::string k = std::string(" ") + key + "=\"";
        const size_t at = tag.find(k);
        if (at == std::string::npos) return {};
        const size_t b = at + k.size(), e = tag.find('"', b);
        return e == std::string::npos ? std::string() : tag.substr(b, e - b);
    };
    // (a family's rank: 0 Japanese, 1 Simplified Chinese, 2 Traditional, 3 Korean, 4 the default family, -1 none)
    auto rank = [](const std::string& lang, const std::string& name) {
        int best = -1;
        size_t i = 0;
        while (i < lang.size()) {
            size_t e = lang.find_first_of(" ,", i);
            if (e == std::string::npos) e = lang.size();
            const std::string t = lang.substr(i, e - i);
            i = e + 1;
            int r = -1;
            if (t.rfind("ja", 0) == 0 || t.find("Jpan") != std::string::npos) r = 0;
            else if (t.rfind("zh-Hans", 0) == 0 || t.find("Hans") != std::string::npos || t == "zh" || t == "zh-CN") r = 1;
            else if (t.rfind("zh-Hant", 0) == 0 || t.find("Hant") != std::string::npos || t == "zh-TW") r = 2;
            else if (t.rfind("ko", 0) == 0 || t.find("Kore") != std::string::npos) r = 3;
            if (r >= 0 && (best < 0 || r < best)) best = r;
        }
        if (best < 0 && name == "sans-serif") best = 4;
        return best;
    };
    std::vector<std::string> ranked[5];
    for (const char* xml : {"/product/etc/fonts_customization.xml", "/system/etc/font_fallback.xml", "/system/etc/fonts.xml",
                            "/vendor/etc/fonts.xml", "/system/etc/fallback_fonts.xml", "/system/etc/system_fonts.xml"}) {
        std::vector<uint8_t> data;
        if (!readFile(root + xml, data)) continue;
        const std::string s(data.begin(), data.end());
        for (size_t f = s.find("<family"); f != std::string::npos; f = s.find("<family", f + 1)) {
            const size_t tagEnd = s.find('>', f), end = s.find("</family>", f);
            if (tagEnd == std::string::npos || end == std::string::npos) break;
            const std::string tag = s.substr(f, tagEnd - f);
            const int r = rank(attr(tag, "lang"), attr(tag, "name"));
            if (r < 0) continue;
            std::vector<std::string> regular, other; // (the 400 weight first)
            for (size_t t = s.find("<font", tagEnd); t != std::string::npos && t < end; t = s.find("<font", t + 1)) {
                const size_t te = s.find('>', t);
                if (te == std::string::npos || te > end) break;
                if (std::isalpha(static_cast<unsigned char>(s[t + 5])) || s[t + 5] == '-') continue; // (<fontVariation...>)
                const std::string ftag = s.substr(t, te - t);
                std::string file;
                if (s[te - 1] != '/') {
                    size_t a = te + 1, b = s.find('<', a);
                    if (b == std::string::npos) break;
                    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
                    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
                    file = s.substr(a, b - a);
                }
                if (file.empty()) file = attr(ftag, "path");
                // (not italics, nor a serif family's stand-in - fallbackFor="serif")
                if (file.empty() || attr(ftag, "style") == "italic" || !attr(ftag, "fallbackFor").empty()) continue;
                const std::string w = attr(ftag, "weight");
                (w == "400" || w.empty() ? regular : other).push_back(file);
            }
            for (auto* list : {&regular, &other})
                for (const std::string& file : *list)
                    if (const std::string p = locate(file); !p.empty()) ranked[r].push_back(p);
        }
    }
    std::vector<std::string> out;
    auto add = [&out](const std::string& p) {
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    for (const auto& list : ranked)
        for (const std::string& p : list) add(p);
    // The font folders themselves (a list that names none of these): CJK-named first, then sans faces, the rest
    std::vector<std::string> cjk, sans, rest;
    for (const char* d : kDirs) {
        int n = 0;
        char** names = SDL_GlobDirectory((root + d).c_str(), "*", 0, &n);
        for (int i = 0; names && i < n; ++i) {
            const std::string name = names[i];
            std::string lower = name;
            for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
            const std::string ext = lower.size() > 4 ? lower.substr(lower.size() - 4) : std::string();
            if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;
            auto has = [&lower](const char* k) { return lower.find(k) != std::string::npos; };
            if (has("emoji") || has("symbol") || has("italic")) continue;
            const std::string path = root + d + name;
            if (has("cjk") || has("jp") || has("sc-") || has("tc-") || has("kr") || has("han")) cjk.push_back(path);
            else if (has("roboto") || has("sans")) sans.push_back(path);
            else rest.push_back(path);
        }
        SDL_free(names);
    }
    for (auto* list : {&cjk, &sans, &rest}) {
        std::stable_sort(list->begin(), list->end(), [](const std::string& a, const std::string& b) {
            return a.find("Regular") != std::string::npos && b.find("Regular") == std::string::npos;
        });
        for (const std::string& p : *list) add(p);
    }
    return out;
}

namespace {
// The first of `paths` that loads (an environment variable's file first; its value "none": none -,
// then `own`: the font/ folder's)
bool loadFirst(Font& font, const char* env, const std::string& own, std::initializer_list<const char*> paths) {
    if (const char* e = SDL_getenv(env)) {
        if (SDL_strcmp(e, "none") == 0) return false;
        if (font.load(e)) return true;
    }
    if (!own.empty() && font.load(own)) return SDL_Log("font/: %s", own.c_str()), true;
    for (const char* p : paths)
        if (font.load(p)) return true;
    return false;
}

// Your own fonts: the .ttf / .otf / .ttc files in a game folder's font/ (the first game folder
// holding one wins, its files by name). One with Japanese / Chinese / Korean glyphs takes the CJK
// font's place - its bold's too, unless a file named "...bold..." is there for it -; one without
// takes the Latin (ArialMT) one's. Nothing there: the system's fonts, as before.
struct OwnFonts {
    std::string latin, cjk, cjkBold;
};
OwnFonts ownFonts() {
    OwnFonts own;
    for (const std::string& folder : gameFolders()) {
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(folder + "font"), ec)) {
            std::string ext = entry.path().extension().u8string();
            for (char& c : ext) c = char(std::tolower((unsigned char)c));
            if (entry.is_regular_file(ec) && (ext == ".ttf" || ext == ".otf" || ext == ".ttc")) files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            Font font;
            const std::string path = file.u8string();
            if (!font.load(path)) {
                SDL_Log("font/: %s isn't a font this can read", path.c_str());
                continue;
            }
            std::string name = file.filename().u8string();
            for (char& c : name) c = char(std::tolower((unsigned char)c));
            std::string& slot = !font.covers("\xe3\x81\x82\xe6\xbc\xa2") ? own.latin // (あ漢)
                                : name.find("bold") != std::string::npos ? own.cjkBold : own.cjk;
            if (slot.empty()) slot = path;
        }
        if (!files.empty()) break;
    }
    if (own.cjkBold.empty()) own.cjkBold = own.cjk;
    return own;
}
} // namespace

bool loadSystemFonts(Font& latin, Font& cjk, Font& cjkBold) {
    if (const char* env = SDL_getenv("CR_FONT"); env && SDL_strcmp(env, "none") == 0) return false; // the 8x8 font only
    const OwnFonts own = ownFonts();
    loadFirst(cjk, "CR_FONT", own.cjk,
              {
                  // Linux (Noto CJK under several distributions' paths, then other CJK families)
                  "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                  "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                  "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
                  "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc",
                  "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
                  "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
                  "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                  "/usr/share/fonts/TTF/VL-Gothic-Regular.ttf",
                  // macOS
                  "/System/Library/Fonts/Hiragino Sans GB.ttc",
                  "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
                  "/System/Library/Fonts/PingFang.ttc",
                  "/System/Library/Fonts/AppleSDGothicNeo.ttc",
                  // Windows
                  "C:/Windows/Fonts/YuGothM.ttc",
                  "C:/Windows/Fonts/msgothic.ttc",
                  "C:/Windows/Fonts/meiryo.ttc",
                  "C:/Windows/Fonts/msyh.ttc",
                  // Android
                  "/system/fonts/NotoSansCJK-Regular.ttc",
                  "/system/fonts/NotoSansSC-Regular.otf",
              });
    // Japanese: 2.2.0's FUN_000375bc picks HiraKakuProN-W6, a bold Gothic
    loadFirst(cjkBold, "CR_FONT_BOLD", own.cjkBold,
              {
                  "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
                  "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
                  "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Bold.ttc",
                  "/usr/share/fonts/noto/NotoSansCJK-Bold.ttc",
                  "/usr/share/fonts/truetype/noto/NotoSansCJK-Bold.ttc",
                  "/System/Library/Fonts/ヒラギノ角ゴシック W6.ttc",
                  "C:/Windows/Fonts/YuGothB.ttc",
                  "C:/Windows/Fonts/meiryob.ttc",
                  "/system/fonts/NotoSansCJK-Bold.ttc",
              });
    // Every other language: ArialMT - Arial itself, or a face made to its metrics (the same advance widths)
    loadFirst(latin, "CR_LATIN_FONT", own.latin,
              {
                  // Linux: Liberation Sans, Arimo (both Arial's metrics), Arial from the core fonts
                  "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                  "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                  "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
                  "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
                  "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
                  "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
                  "/usr/share/fonts/truetype/croscore/Arimo-Regular.ttf",
                  "/usr/share/fonts/croscore/Arimo-Regular.ttf",
                  "/usr/share/fonts/TTF/Arimo-Regular.ttf",
                  "/usr/share/fonts/truetype/msttcorefonts/Arial.ttf",
                  "/usr/share/fonts/TTF/arial.ttf",
                  "/usr/share/fonts/corefonts/arial.ttf",
                  // macOS
                  "/System/Library/Fonts/Supplemental/Arial.ttf",
                  "/Library/Fonts/Arial.ttf",
                  // Windows
                  "C:/Windows/Fonts/arial.ttf",
                  // Android (ChromeOS-derived builds ship Arimo)
                  "/system/fonts/Arimo-Regular.ttf",
              });
#ifdef SDL_PLATFORM_ANDROID
    // (a system without the AOSP files - a phone maker's own names and partitions -: its own font list's, the
    // first that draws Latin text, in the CJK font's place and its bold's)
    if (!cjk.loaded() && !SDL_getenv("CR_FONT")) {
        std::string found;
        for (const std::string& c : androidFonts(""))
            if (cjk.load(c) && cjk.covers("Aa")) {
                found = c;
                break;
            }
        if (found.empty()) cjk.load(std::string()); // (none: no face left loaded)
        else {
            SDL_Log("font: %s from the system's font list", found.c_str());
            if (!cjkBold.loaded() && !SDL_getenv("CR_FONT_BOLD")) cjkBold.load(found);
        }
    }
#endif
#ifdef SDL_PLATFORM_IOS
    // iOS: none of the paths above (macOS's) - the system's own faces through CoreText, as 2.2.0 asks UIKit
    // for them: ArialMT, Hiragino Kaku Gothic ProN W6 for Japanese, W3 the CJK fallback (applyLanguage
    // puts PingFang SC / TC or Apple SD Gothic Neo there for Chinese and Korean)
    if (!cjk.loaded() && !SDL_getenv("CR_FONT")) loadAppleFont(cjk, "HiraKakuProN-W3");
    if (!cjkBold.loaded() && !SDL_getenv("CR_FONT_BOLD")) loadAppleFont(cjkBold, "HiraKakuProN-W6");
    if (!latin.loaded() && !SDL_getenv("CR_LATIN_FONT")) loadAppleFont(latin, "ArialMT");
#endif
    return latin.loaded() || cjk.loaded();
}

} // namespace cr
