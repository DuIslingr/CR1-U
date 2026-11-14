#include "assets/gxt.hpp"

#include <cstring>

namespace cr {

namespace {
// Morton index of (x, y) in a square of side n (a power of two), the y bit lowest; squares of the
// smaller side in row order across a w x h grid
size_t swizzleIndex(int x, int y, int w, int n) {
    const int lx = x % n, ly = y % n;
    size_t m = 0;
    for (int b = 0; (1 << b) < n; ++b) m |= size_t((ly >> b) & 1) << (2 * b) | size_t((lx >> b) & 1) << (2 * b + 1);
    return (size_t(y / n) * (w / n) + size_t(x / n)) * size_t(n) * n + m;
}

// One 4x4 block of UBC1 / 2 / 3 (BC1 / 2 / 3 - DXT1 / 3 / 5): 16 RGBA texels, row by row
void decodeBlock(const uint8_t* b, int kind, uint8_t texels[16][4]) {
    const uint8_t* c = kind == 1 ? b : b + 8;
    const uint16_t c0 = uint16_t(c[0] | c[1] << 8), c1 = uint16_t(c[2] | c[3] << 8);
    uint8_t pal[4][4];
    auto rgb = [](uint16_t v, uint8_t* o) {
        o[0] = uint8_t((v >> 11 & 31) * 255 / 31), o[1] = uint8_t((v >> 5 & 63) * 255 / 63), o[2] = uint8_t((v & 31) * 255 / 31), o[3] = 255;
    };
    rgb(c0, pal[0]), rgb(c1, pal[1]);
    const bool four = kind != 1 || c0 > c1;
    for (int k = 0; k < 3; ++k) {
        pal[2][k] = uint8_t(four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2);
        pal[3][k] = uint8_t(four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0);
    }
    pal[2][3] = 255, pal[3][3] = four ? 255 : 0;
    const uint32_t bits = uint32_t(c[4] | c[5] << 8 | c[6] << 16 | uint32_t(c[7]) << 24);
    for (int t = 0; t < 16; ++t) std::memcpy(texels[t], pal[bits >> (2 * t) & 3], 4);
    if (kind == 2) // 4-bit alpha
        for (int t = 0; t < 16; ++t) texels[t][3] = uint8_t((b[t / 2] >> (4 * (t & 1)) & 15) * 17);
    else if (kind == 3) { // two alphas, 3-bit indices
        uint8_t a[8] = {b[0], b[1]};
        for (int k = 2; k < 8; ++k)
            a[k] = uint8_t(b[0] > b[1] ? ((8 - k) * b[0] + (k - 1) * b[1]) / 7 : k < 6 ? ((6 - k) * b[0] + (k - 1) * b[1]) / 5 : k == 6 ? 0 : 255);
        uint64_t ab = 0;
        for (int k = 0; k < 6; ++k) ab |= uint64_t(b[2 + k]) << (8 * k);
        for (int t = 0; t < 16; ++t) texels[t][3] = a[ab >> (3 * t) & 7];
    }
}
} // namespace

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
    } else if (format == 0x85000000 || format == 0x86000000 || format == 0x87000000) { // UBC1 / 2 / 3, ABGR
        // (no game file is one: a fan translation's own - the Russian one's window, title and map art -
        // stored as a bare GXT, its 4x4 blocks swizzled as the texels of the other formats are)
        const int kind = int(format >> 24) - 0x84, bs = kind == 1 ? 8 : 16;
        if (w <= 0 || h <= 0 || (w & 3) || (h & 3)) return fail("bad GXT size");
        const int bw = w / 4, bh = h / 4;
        if (g + data + size_t(bw) * bh * bs > f.size()) return fail("bad GXT size");
        const bool swizzled = (type & 0xF0000000u) == 0;
        if (swizzled && ((bw & (bw - 1)) || (bh & (bh - 1)))) return fail("swizzled texture is not a power of two");
        const int n = bw < bh ? bw : bh;
        out.width = w, out.height = h, out.alphaOnly = false, out.storage = Image::Lossless;
        out.rgba.assign(size_t(w) * h * 4, 255);
        uint8_t texels[16][4];
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const size_t i = swizzled ? swizzleIndex(bx, by, bw, n) : size_t(by) * bw + bx;
                decodeBlock(&f[g + data + i * bs], kind, texels);
                for (int t = 0; t < 16; ++t) std::memcpy(&out.rgba[(size_t(by * 4 + t / 4) * w + bx * 4 + t % 4) * 4], texels[t], 4);
            }
        return true;
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
                i = swizzleIndex(x, y, w, n);
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
