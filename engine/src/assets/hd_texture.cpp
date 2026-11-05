#include "assets/hd_texture.hpp"

#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include <algorithm>

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

#ifdef CR_HAVE_ZLIB
#include <zlib.h>
#endif

namespace cr {

namespace {

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

} // namespace

// An upscale's see-through pixels' colour: drawn filtered (the originals nearest), a cut-out's
// edge shows the colour under its part-transparent pixels - which the networks leave dark (a
// black fringe round hair and ornaments). Each pixel short of opaque takes the nearest opaque
// one's colour (a breadth-first fill from them), its alpha kept.
void fillSeeThrough(Image& img) {
    const int w = img.width, h = img.height;
    if (w <= 0 || h <= 0 || img.alphaOnly) return;
    std::vector<uint8_t>& px = img.rgba;
    constexpr uint8_t kOpaque = 250;
    std::vector<uint8_t> done(size_t(w) * size_t(h), 0);
    std::vector<int> queue;
    queue.reserve(size_t(w) * size_t(h) / 4);
    size_t solid = 0;
    for (int i = 0; i < w * h; ++i)
        if (px[size_t(i) * 4 + 3] >= kOpaque) done[size_t(i)] = 1, ++solid;
    if (solid == 0 || solid == size_t(w) * size_t(h)) return;
    for (int y = 0; y < h; ++y) // (the opaque pixels at the edge: where the fill starts)
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (!done[size_t(i)]) continue;
            if ((x > 0 && !done[size_t(i - 1)]) || (x + 1 < w && !done[size_t(i + 1)]) || (y > 0 && !done[size_t(i - w)]) ||
                (y + 1 < h && !done[size_t(i + w)]))
                queue.push_back(i);
        }
    for (size_t q = 0; q < queue.size(); ++q) {
        const int i = queue[q], x = i % w, y = i / w;
        const int next[4] = {x > 0 ? i - 1 : -1, x + 1 < w ? i + 1 : -1, y > 0 ? i - w : -1, y + 1 < h ? i + w : -1};
        for (int n : next) {
            if (n < 0 || done[size_t(n)]) continue;
            done[size_t(n)] = 1;
            for (int c = 0; c < 3; ++c) px[size_t(n) * 4 + size_t(c)] = px[size_t(i) * 4 + size_t(c)];
            queue.push_back(n);
        }
    }
}

uint64_t imageHash(const Image& img) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    const uint32_t head[3] = {uint32_t(img.width), uint32_t(img.height), img.alphaOnly ? 1u : 0u};
    mix(reinterpret_cast<const uint8_t*>(head), sizeof head);
    mix(img.rgba.data(), img.rgba.size());
    return h;
}

std::string hashName(uint64_t hash) {
    char s[17];
    SDL_snprintf(s, sizeof s, "%016llx", static_cast<unsigned long long>(hash));
    return s;
}

bool saveRawImage(const std::string& path, const Image& img) {
    std::vector<uint8_t> out = {'C', 'R', 'R', 'A', 'W', '1'};
    put32(out, uint32_t(img.width));
    put32(out, uint32_t(img.height));
    put32(out, (img.alphaOnly ? 1u : 0u) | uint32_t(img.storage) << 1);
    out.insert(out.end(), img.rgba.begin(), img.rgba.end());
    return SDL_SaveFile(path.c_str(), out.data(), out.size());
}

namespace {

// A replacement left out: why ("<hash>.png: ..."), and false
bool leftOut(std::string* why, const std::string& file, const std::string& what) {
    if (why) *why = file + ": " + what;
    return false;
}

std::string sizesText(int w, int h, int originalW, int originalH) {
    return std::to_string(w) + "x" + std::to_string(h) + " for a " + std::to_string(originalW) + "x" + std::to_string(originalH) +
           " picture (its shape, at least its size)";
}

// PNG's chunk check (CRC-32, the IEEE polynomial)
uint32_t crc32Of(const uint8_t* p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

void putBe32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i)));
}

} // namespace

bool hdShapeFits(int w, int h, int originalW, int originalH) {
    if (originalW <= 0 || originalH <= 0 || w < originalW || h < originalH || w > 8192 || h > 8192) return false;
    // (one scale s for both: w = originalW s + a, h = originalH s + b, each rounded, |a|, |b| <= 1/2)
    const int64_t off = int64_t(w) * originalH - int64_t(h) * originalW;
    return 2 * (off < 0 ? -off : off) <= int64_t(originalW) + originalH;
}

bool savePngImage(const std::string& path, const Image& img) {
    const size_t n = size_t(std::max(img.width, 0)) * size_t(std::max(img.height, 0)) * 4;
    if (!n || img.rgba.size() < n) return false;
    std::vector<uint8_t> rows(img.rgba.begin(), img.rgba.begin() + std::ptrdiff_t(n));
    std::vector<uint8_t> png;
    stbi_write_png_to_func(
        [](void* to, void* data, int size) {
            auto* v = static_cast<std::vector<uint8_t>*>(to);
            v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        },
        &png, img.width, img.height, 4, rows.data(), img.width * 4);
    if (png.size() < 33 || std::memcmp(png.data() + 12, "IHDR", 4) != 0) return false;
    // the .crraw's flags for tools/hd_textures.py (which upscaler), after IHDR
    const std::string text = std::string("cr-flags") + '\0' + std::to_string((img.alphaOnly ? 1u : 0u) | uint32_t(img.storage) << 1);
    std::vector<uint8_t> chunk;
    putBe32(chunk, uint32_t(text.size()));
    chunk.insert(chunk.end(), {'t', 'E', 'X', 't'});
    chunk.insert(chunk.end(), text.begin(), text.end());
    putBe32(chunk, crc32Of(chunk.data() + 4, chunk.size() - 4));
    png.insert(png.begin() + 33, chunk.begin(), chunk.end());
    return SDL_SaveFile(path.c_str(), png.data(), png.size());
}

bool loadHdImage(const std::string& dir, const Image& original, Image& out, std::string* why) {
    if (why) why->clear();
    if (dir.empty() || original.width <= 0) return false;
    const std::string name = hashName(imageHash(original));
    size_t size = 0;
#ifdef CR_HAVE_ZLIB
    if (void* data = SDL_LoadFile((dir + name + ".crhd").c_str(), &size)) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        std::string what;
        if (size <= 26 || std::memcmp(p, "CRHD1", 6) != 0) what = "not a .crhd";
        const uint32_t w = what.empty() ? get32(p + 6) : 0, h = what.empty() ? get32(p + 10) : 0;
        if (what.empty() && (get32(p + 14) != uint32_t(original.width) || get32(p + 18) != uint32_t(original.height)))
            what = "made for a picture of another size";
        if (what.empty() && !hdShapeFits(int(std::min(w, 65536u)), int(std::min(h, 65536u)), original.width, original.height))
            what = sizesText(int(std::min(w, 65536u)), int(std::min(h, 65536u)), original.width, original.height);
        if (what.empty()) {
            out.width = int(w);
            out.height = int(h);
            out.alphaOnly = (get32(p + 22) & 1) != 0;
            out.rgba.resize(size_t(w) * h * 4);
            uLongf len = uLongf(out.rgba.size());
            if (uncompress(out.rgba.data(), &len, p + 26, uLong(size - 26)) != Z_OK || len != out.rgba.size()) what = "its pixels don't unpack";
        }
        SDL_free(data);
        return what.empty() || leftOut(why, name + ".crhd", what);
    }
#endif
    if (void* data = SDL_LoadFile((dir + name + ".png").c_str(), &size)) { // (any upscale or edit: the original's shape)
        int w = 0, h = 0, channels = 0;
        stbi_uc* px = stbi_load_from_memory(static_cast<const stbi_uc*>(data), int(std::min(size, size_t(1) << 30)), &w, &h, &channels, 4);
        SDL_free(data);
        if (!px) return leftOut(why, name + ".png", "not a PNG this reads");
        if (!hdShapeFits(w, h, original.width, original.height)) {
            stbi_image_free(px);
            return leftOut(why, name + ".png", sizesText(w, h, original.width, original.height));
        }
        out.width = w;
        out.height = h;
        out.alphaOnly = original.alphaOnly;
        out.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
        stbi_image_free(px);
        return true;
    }
    return false;
}

} // namespace cr
