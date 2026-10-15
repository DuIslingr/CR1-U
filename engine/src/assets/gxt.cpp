#include "assets/gxt.hpp"

#include <cstring>

namespace cr {

bool decodeGxt(const std::vector<uint8_t>& f, Image& out, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    size_t g = 0;
    if (f.size() >= 9 && std::memcmp(f.data(), "MCT_", 4) == 0) g = 9;
    if (f.size() < g + 0x40 || std::memcmp(&f[g], "GXT", 3) != 0) return fail("not a GXT texture");
    auto u32 = [&](size_t p) { uint32_t v; std::memcpy(&v, &f[g + p], 4); return v; };
    auto u16 = [&](size_t p) { uint16_t v; std::memcpy(&v, &f[g + p], 2); return v; };
    if (u32(8) < 1) return fail("no texture");
    const uint32_t data = u32(0x20), type = u32(0x30), format = u32(0x34);
    const int w = u16(0x38), h = u16(0x3a);
    int bpp;
    const uint8_t* palette = nullptr;       // P8: 256 entries of 4 bytes
    if (format == 0x0C001000) bpp = 4;      // U8U8U8U8, ARGB swizzle: bytes B G R A
    else if (format == 0x98001000) bpp = 3; // U8U8U8: bytes B G R
    else if (format == 0x95005000) {        // P8, 1RGB palette (the Vita's warning screen): bytes B G R, opaque
        // the P8 palettes close the data (after any P4 ones): u32 0x18 of them, this one u32 0x28
        bpp = 1;
        const size_t end = g + size_t(u32(0x0c)) + u32(0x10), n8 = u32(0x18), idx = u32(0x28);
        if (n8 < 1 || idx >= n8 || end > f.size() || end < n8 * 1024) return fail("bad GXT palette");
        palette = &f[end - n8 * 1024 + idx * 1024];
    } else return fail("unsupported GXT format");
    if (w <= 0 || h <= 0 || g + data + size_t(w) * h * bpp > f.size()) return fail("bad GXT size");
    const bool swizzled = (type & 0xF0000000u) == 0;
    if (swizzled && ((w & (w - 1)) || (h & (h - 1)))) return fail("swizzled texture is not a power of two");
    const int n = w < h ? w : h;
    out.width = w;
    out.height = h;
    out.alphaOnly = false;
    out.storage = Image::Lossless;
    out.rgba.assign(size_t(w) * h * 4, 255);
    const uint8_t* src = &f[g + data];
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t i;
            if (swizzled) {
                const int lx = x % n, ly = y % n;
                size_t m = 0;
                for (int b = 0; (1 << b) < n; ++b)
                    m |= size_t((ly >> b) & 1) << (2 * b) | size_t((lx >> b) & 1) << (2 * b + 1);
                i = (size_t(y / n) * (w / n) + size_t(x / n)) * size_t(n) * n + m;
            } else {
                i = size_t(y) * w + x;
            }
            const uint8_t* p = palette ? palette + size_t(src[i]) * 4 : src + i * bpp;
            uint8_t* o = &out.rgba[(size_t(y) * w + x) * 4];
            o[0] = p[2]; o[1] = p[1]; o[2] = p[0];
            if (bpp == 4) o[3] = p[3];
        }
    return true;
}

} // namespace cr
