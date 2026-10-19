#include "assets/hd_texture.hpp"

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

bool loadHdImage(const std::string& dir, const Image& original, Image& out) {
#ifdef CR_HAVE_ZLIB
    if (dir.empty() || original.width <= 0) return false;
    size_t size = 0;
    void* data = SDL_LoadFile((dir + hashName(imageHash(original)) + ".crhd").c_str(), &size);
    if (!data) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bool ok = size > 26 && std::memcmp(p, "CRHD1", 6) == 0;
    const uint32_t w = ok ? get32(p + 6) : 0, h = ok ? get32(p + 10) : 0;
    ok = ok && get32(p + 14) == uint32_t(original.width) && get32(p + 18) == uint32_t(original.height) && w >= 1 &&
         h >= 1 && w <= 8192 && h <= 8192 && w >= uint32_t(original.width) && h >= uint32_t(original.height);
    if (ok) {
        out.width = int(w);
        out.height = int(h);
        out.alphaOnly = (get32(p + 22) & 1) != 0;
        out.rgba.resize(size_t(w) * h * 4);
        uLongf len = uLongf(out.rgba.size());
        ok = uncompress(out.rgba.data(), &len, p + 26, uLong(size - 26)) == Z_OK && len == out.rgba.size();
    }
    SDL_free(data);
    return ok;
#else
    (void)dir;
    (void)original;
    (void)out;
    return false; // built without zlib
#endif
}

} // namespace cr
