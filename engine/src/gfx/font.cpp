#include "gfx/font.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/file.hpp"
#include "core/text.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"

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

namespace {
// The first of `paths` that loads (an environment variable's file first; its value "none": none)
bool loadFirst(Font& font, const char* env, std::initializer_list<const char*> paths) {
    if (const char* e = SDL_getenv(env)) {
        if (SDL_strcmp(e, "none") == 0) return false;
        if (font.load(e)) return true;
    }
    for (const char* p : paths)
        if (font.load(p)) return true;
    return false;
}
} // namespace

bool loadSystemFonts(Font& latin, Font& cjk, Font& cjkBold) {
    if (const char* env = SDL_getenv("CR_FONT"); env && SDL_strcmp(env, "none") == 0) return false; // the 8x8 font only
    loadFirst(cjk, "CR_FONT",
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
    loadFirst(cjkBold, "CR_FONT_BOLD",
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
    loadFirst(latin, "CR_LATIN_FONT",
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
    return latin.loaded() || cjk.loaded();
}

} // namespace cr
