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

Font::Font() = default;
Font::~Font() = default;

bool Font::openFace(int index) {
    const int offset = stbtt_GetFontOffsetForIndex(data_.data(), index);
    if (offset < 0) return false;
    auto f = std::make_unique<Face>();
    if (!stbtt_InitFont(&f->info, data_.data(), offset)) return false;
    face_ = std::move(f);
    faceIndex_ = index;
    ++generation_;
    return true;
}

bool Font::load(const std::string& path) {
    face_.reset();
    faceIndex_ = -1;
    if (!readFile(path, data_) || data_.size() < 12 || !openFace(0)) {
        data_.clear();
        return false;
    }
    path_ = path;
    return true;
}

bool Font::loaded() const { return face_ != nullptr; }

bool Font::selectRegion(const char* region) {
    if (!loaded()) return false;
    const int n = stbtt_GetNumberOfFonts(data_.data());
    const size_t rl = std::strlen(region);
    for (int i = 0; i < n; ++i) {
        stbtt_fontinfo info;
        if (!stbtt_InitFont(&info, data_.data(), stbtt_GetFontOffsetForIndex(data_.data(), i))) continue;
        int len = 0; // full name (id 4), Windows platform, US English: UTF-16BE
        const char* name = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                                   STBTT_MS_LANG_ENGLISH, 4);
        if (!name || len < int(2 * rl)) continue;
        std::string ascii;
        for (int k = 1; k < len; k += 2) ascii += name[k];
        // "Noto Sans CJK JP" but not "Noto Sans Mono CJK JP"
        if (ascii.size() >= rl && ascii.compare(ascii.size() - rl, rl, region) == 0 &&
            ascii.find("Mono") == std::string::npos) {
            if (i == faceIndex_) return false;
            return openFace(i);
        }
    }
    return false;
}

bool Font::covers(const std::string& utf8) const {
    if (!loaded()) return false;
    for (size_t i = 0; i < utf8.size();) {
        const uint32_t c = nextUtf8(utf8, i);
        if (c > 0x20 && c != 0x3000 && !stbtt_FindGlyphIndex(&face_->info, int(c))) return false;
    }
    return true;
}

bool Font::rasterise(const std::vector<std::string>& lines, float em, float lineStep, int pad, Image& out) const {
    if (!loaded() || lines.empty()) return false;
    const stbtt_fontinfo& info = face_->info;
    const float scale = stbtt_ScaleForMappingEmToPixels(&info, em);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    const float base0 = float(pad) + float(ascent) * scale;

    // Glyph indices per line ('?' for characters the face lacks), then the widest line
    std::vector<std::vector<int>> glyphs(lines.size());
    const int fallback = stbtt_FindGlyphIndex(&info, '?');
    float widest = 1;
    for (size_t l = 0; l < lines.size(); ++l) {
        float x = 0;
        int prev = 0;
        for (size_t i = 0; i < lines[l].size();) {
            const uint32_t c = nextUtf8(lines[l], i);
            int g = stbtt_FindGlyphIndex(&info, int(c));
            if (!g && c > 0x20 && c != 0x3000) g = fallback;
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&info, g, &adv, &lsb);
            if (prev) x += float(stbtt_GetGlyphKernAdvance(&info, prev, g)) * scale;
            x += float(adv) * scale;
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
        int prev = 0;
        for (int g : glyphs[l]) {
            if (prev) x += float(stbtt_GetGlyphKernAdvance(&info, prev, g)) * scale;
            prev = g;
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&info, g, &adv, &lsb);
            const float fx = x - std::floor(x);
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(&info, g, scale, scale, fx, 0, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                bmp.assign(size_t(gw) * gh, 0);
                stbtt_MakeGlyphBitmapSubpixel(&info, bmp.data(), gw, gh, gw, scale, scale, fx, 0, g);
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
            x += float(adv) * scale;
        }
    }
    return true;
}

bool loadSystemFont(Font& font) {
    std::vector<std::string> candidates;
    if (const char* env = SDL_getenv("CR_FONT")) {
        if (SDL_strcmp(env, "none") == 0) return false; // the 8x8 font only
        candidates.push_back(env);
    }
    for (const char* p : {
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
         })
        candidates.push_back(p);
    for (const std::string& c : candidates)
        if (font.load(c)) return true;
    return false;
}

} // namespace cr
