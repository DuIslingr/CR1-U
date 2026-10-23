#include "assets/texture.hpp"

#include "assets/gxt.hpp"

#include <cstring>

#include "core/lzw.hpp"

namespace cr {

namespace {

// Pixel formats: index into the original engine's GL format tables (0xd4e20 / 0xd4dac).
enum Format {
    kRGB888 = 1, kRGBA8888 = 2, kRGB565 = 3, kRGBA5551 = 4, kRGBA4444 = 5,
    kPal8RGB8 = 6, kPal8RGBA8 = 7, kPal8R5G6B5 = 8, kPal8RGB5A1 = 9, kPal8RGBA4 = 10,
    kPal4RGB8 = 11, kPal4RGBA8 = 12, kPal4R5G6B5 = 13, kPal4RGB5A1 = 14, kPal4RGBA4 = 15,
    kL8 = 16, kA8 = 17, kLA88 = 18, kRGB888b = 21, kRGBA8888b = 22,
    kPvrtcRGBA4 = 25, kPvrtcRGBA2 = 26, kPvrtcRGB4 = 27, kPvrtcRGB2 = 28,
};

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

// Expands an n-bit channel to 8 bits by bit replication.
uint8_t expand(uint32_t v, int bits) {
    switch (bits) {
    case 3: return uint8_t((v << 5) | (v << 2) | (v >> 1));
    case 4: return uint8_t((v << 4) | v);
    case 5: return uint8_t((v << 3) | (v >> 2));
    case 6: return uint8_t((v << 2) | (v >> 4));
    default: return uint8_t(v);
    }
}

void unpack565(uint16_t p, uint8_t* o) {
    o[0] = expand(p >> 11, 5); o[1] = expand((p >> 5) & 63, 6); o[2] = expand(p & 31, 5); o[3] = 255;
}
void unpack5551(uint16_t p, uint8_t* o) {
    o[0] = expand(p >> 11, 5); o[1] = expand((p >> 6) & 31, 5); o[2] = expand((p >> 1) & 31, 5);
    o[3] = (p & 1) ? 255 : 0;
}
void unpack4444(uint16_t p, uint8_t* o) {
    o[0] = expand(p >> 12, 4); o[1] = expand((p >> 8) & 15, 4); o[2] = expand((p >> 4) & 15, 4);
    o[3] = expand(p & 15, 4);
}

// Bytes needed for the pixel data (excluding any palette).
size_t pixelBytes(int fmt, int w, int h) {
    const size_t n = size_t(w) * h;
    switch (fmt) {
    case kRGB888: case kRGB888b: return n * 3;
    case kRGBA8888: case kRGBA8888b: return n * 4;
    case kRGB565: case kRGBA5551: case kRGBA4444: case kLA88: return n * 2;
    case kL8: case kA8: return n;
    case kPal8RGB8: case kPal8RGBA8: case kPal8R5G6B5: case kPal8RGB5A1: case kPal8RGBA4: return n;
    case kPal4RGB8: case kPal4RGBA8: case kPal4R5G6B5: case kPal4RGB5A1: case kPal4RGBA4: return (n + 1) / 2;
    case kPvrtcRGBA4: case kPvrtcRGB4: {
        const size_t bw = w / 4 < 2 ? 2 : w / 4, bh = h / 4 < 2 ? 2 : h / 4;
        return bw * bh * 8;
    }
    default: return 0;
    }
}

// Decodes a palette of `count` entries; returns bytes consumed.
size_t readPalette(int fmt, const uint8_t* src, int count, uint8_t (*pal)[4]) {
    // Palette entry type is the same for PAL8 and PAL4 variants: RGB8, RGBA8, 565, 5551, 4444
    const int kind = (fmt >= kPal4RGB8) ? fmt - kPal4RGB8 : fmt - kPal8RGB8;
    for (int i = 0; i < count; ++i) {
        uint8_t* o = pal[i];
        switch (kind) {
        case 0: o[0] = src[i * 3]; o[1] = src[i * 3 + 1]; o[2] = src[i * 3 + 2]; o[3] = 255; break;
        case 1: std::memcpy(o, src + i * 4, 4); break;
        case 2: unpack565(rd16(src + i * 2), o); break;
        case 3: unpack5551(rd16(src + i * 2), o); break;
        case 4: unpack4444(rd16(src + i * 2), o); break;
        }
    }
    static const int kEntryBytes[5] = {3, 4, 2, 2, 2};
    return size_t(count) * kEntryBytes[kind];
}

// ------------------------------------------------------------------ PVRTC 4bpp

uint32_t twiddle(uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh) {
    const uint32_t minDim = bw < bh ? bw : bh;
    uint32_t out = 0;
    int shift = 0;
    for (uint32_t bit = 1; bit < minDim; bit <<= 1, ++shift) {
        out |= ((by >> shift) & 1) << (2 * shift);
        out |= ((bx >> shift) & 1) << (2 * shift + 1);
    }
    const uint32_t rest = (bh > bw ? by : bx) >> shift;
    return out | (rest << (2 * shift));
}

void pvrtcColours(uint32_t c, uint8_t* a, uint8_t* b) {
    if (c & 0x8000) { // colour A opaque: RGB 554
        a[0] = expand((c >> 10) & 31, 5);
        a[1] = expand((c >> 5) & 31, 5);
        a[2] = expand((((c >> 1) & 15) << 1) | ((c >> 4) & 1), 5);
        a[3] = 255;
    } else {          // colour A translucent: ARGB 3443
        a[0] = expand((c >> 8) & 15, 4);
        a[1] = expand((c >> 4) & 15, 4);
        a[2] = expand((c >> 1) & 7, 3);
        a[3] = expand((c >> 12) & 7, 3);
    }
    if (c & 0x80000000u) { // colour B opaque: RGB 555
        b[0] = expand((c >> 26) & 31, 5);
        b[1] = expand((c >> 21) & 31, 5);
        b[2] = expand((c >> 16) & 31, 5);
        b[3] = 255;
    } else {               // colour B translucent: ARGB 3444
        b[0] = expand((c >> 24) & 15, 4);
        b[1] = expand((c >> 20) & 15, 4);
        b[2] = expand((c >> 16) & 15, 4);
        b[3] = expand((c >> 28) & 7, 3);
    }
}

void decodePvrtc4(const uint8_t* src, int w, int h, uint8_t* dst) {
    const int bw = w / 4 < 2 ? 2 : w / 4, bh = h / 4 < 2 ? 2 : h / 4;
    // Per-block colour A/B and raw words, in raster order
    std::vector<uint8_t> colA(size_t(bw) * bh * 4), colB(size_t(bw) * bh * 4);
    std::vector<uint32_t> mod(size_t(bw) * bh), col(size_t(bw) * bh);
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + size_t(twiddle(bx, by, bw, bh)) * 8;
            const size_t i = size_t(by) * bw + bx;
            mod[i] = rd32(blk);
            col[i] = rd32(blk + 4);
            pvrtcColours(col[i], &colA[i * 4], &colB[i * 4]);
        }
    }
    static const int kStd[4] = {0, 3, 5, 8};
    static const int kPunch[4] = {0, 4, 4, 8};
    for (int y = 0; y < h; ++y) {
        // Block colours sit at block centres: sample between blocks y0 and y0+1
        const int fy = y - 2;
        const int y0 = ((fy >> 2) % bh + bh) % bh, y1 = (y0 + 1) % bh, ty = fy & 3;
        for (int x = 0; x < w; ++x) {
            const int fx = x - 2;
            const int x0 = ((fx >> 2) % bw + bw) % bw, x1 = (x0 + 1) % bw, tx = fx & 3;
            const int w00 = (4 - tx) * (4 - ty), w10 = tx * (4 - ty), w01 = (4 - tx) * ty, w11 = tx * ty;
            const size_t i00 = (size_t(y0) * bw + x0) * 4, i10 = (size_t(y0) * bw + x1) * 4;
            const size_t i01 = (size_t(y1) * bw + x0) * 4, i11 = (size_t(y1) * bw + x1) * 4;

            const size_t bi = size_t(y >> 2) * bw + (x >> 2);
            const int m = (mod[bi] >> (2 * ((y & 3) * 4 + (x & 3)))) & 3;
            const bool punch = col[bi] & 1;
            const int wt = punch ? kPunch[m] : kStd[m];

            uint8_t* o = dst + (size_t(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const int a = colA[i00 + c] * w00 + colA[i10 + c] * w10 + colA[i01 + c] * w01 + colA[i11 + c] * w11;
                const int b = colB[i00 + c] * w00 + colB[i10 + c] * w10 + colB[i01 + c] * w01 + colB[i11 + c] * w11;
                o[c] = uint8_t((a * (8 - wt) + b * wt + 64) / 128);
            }
            if (punch && m == 2) o[3] = 0;
        }
    }
}

bool fail(std::string* error, const char* msg) {
    if (error) *error = msg;
    return false;
}

} // namespace

const char* mctFormatName(int format) {
    switch (format) {
    case kRGB888: case kRGB888b: return "RGB888";
    case kRGBA8888: case kRGBA8888b: return "RGBA8888";
    case kRGB565: return "RGB565";
    case kRGBA5551: return "RGBA5551";
    case kRGBA4444: return "RGBA4444";
    case kPal8RGB8: return "PAL8_RGB8";
    case kPal8RGBA8: return "PAL8_RGBA8";
    case kPal8R5G6B5: return "PAL8_R5G6B5";
    case kPal8RGB5A1: return "PAL8_RGB5A1";
    case kPal8RGBA4: return "PAL8_RGBA4";
    case kPal4RGB8: return "PAL4_RGB8";
    case kPal4RGBA8: return "PAL4_RGBA8";
    case kPal4R5G6B5: return "PAL4_R5G6B5";
    case kPal4RGBA4: return "PAL4_RGBA4";
    case kPal4RGB5A1: return "PAL4_RGB5A1";
    case kL8: return "L8";
    case kA8: return "A8";
    case kLA88: return "LA88";
    case kPvrtcRGBA4: return "PVRTC_RGBA_4";
    case kPvrtcRGBA2: return "PVRTC_RGBA_2";
    case kPvrtcRGB4: return "PVRTC_RGB_4";
    case kPvrtcRGB2: return "PVRTC_RGB_2";
    default: return "unknown";
    }
}

bool decodeMct(const std::vector<uint8_t>& file, Image& out, MctInfo* info, std::string* error) {
    if (file.size() >= 12 && std::memcmp(file.data(), "MCT_", 4) == 0 && std::memcmp(file.data() + 9, "GXT", 3) == 0) {
        // the Vita's form of the same texture ("MCT_", id, a byte, then a GXT; gxt.hpp)
        if (!decodeGxt(file, out, error)) return false;
        if (info) {
            *info = MctInfo();
            info->texId = rd16(file.data() + 4);
            info->width = out.width, info->height = out.height;
        }
        return true;
    }
    if (file.size() < 0x2b || std::memcmp(file.data(), "MCT_", 4) != 0)
        return fail(error, "not an MCT file");

    const uint8_t* h = file.data();
    MctInfo mi;
    mi.texId = rd16(h + 0x1a);
    const uint32_t decodedSize = rd32(h + 0x1e);
    mi.width = rd16(h + 0x22);
    mi.height = rd16(h + 0x24);
    mi.compressed = rd16(h + 0x26) != 0;
    mi.format = rd16(h + 0x28);
    if (info) *info = mi;
    switch (mi.format) {
    case kPvrtcRGBA4: case kPvrtcRGBA2: case kPvrtcRGB4: case kPvrtcRGB2: out.storage = Image::Pvrtc; break;
    case kRGB565: case kRGBA5551: case kRGBA4444: case kPal8R5G6B5: case kPal8RGB5A1: case kPal8RGBA4:
    case kPal4R5G6B5: case kPal4RGBA4: case kPal4RGB5A1: out.storage = Image::Bits16; break;
    default: out.storage = Image::Lossless; break;
    }

    const int w = mi.width, ht = mi.height, fmt = mi.format;
    if (w <= 0 || ht <= 0) return fail(error, "bad dimensions");
    const size_t pix = pixelBytes(fmt, w, ht);
    if (!pix) return fail(error, "unsupported pixel format");

    std::vector<uint8_t> payloadBuf;
    const uint8_t* p;
    size_t psize;
    if (mi.compressed) {
        if (!lzwDecode(file.data(), file.size(), 0x2a, payloadBuf, decodedSize))
            return fail(error, "LZW decode failed");
        p = payloadBuf.data();
        psize = payloadBuf.size();
    } else {
        p = file.data() + 0x2a;
        psize = file.size() - 0x2a;
    }

    const bool pal8 = fmt >= kPal8RGB8 && fmt <= kPal8RGBA4;
    const bool pal4 = fmt >= kPal4RGB8 && fmt <= kPal4RGBA4;
    uint8_t palette[256][4];
    size_t palBytes = 0;
    if (pal8 || pal4) {
        static const int kEntryBytes[5] = {3, 4, 2, 2, 2};
        const int count = pal8 ? 256 : 16;
        const int kind = pal4 ? fmt - kPal4RGB8 : fmt - kPal8RGB8;
        if (psize < size_t(count) * kEntryBytes[kind]) return fail(error, "truncated palette");
        palBytes = readPalette(fmt, p, count, palette);
    }
    if (psize < palBytes + pix) return fail(error, "truncated pixel data");
    p += palBytes;

    out.width = w;
    out.height = ht;
    out.rgba.assign(size_t(w) * ht * 4, 0);
    uint8_t* o = out.rgba.data();
    const size_t n = size_t(w) * ht;

    // (no alpha of their own: RGB, RGB565, L8, the RGB palettes and RGB PVRTC)
    out.noAlpha = fmt == kRGB888 || fmt == kRGB888b || fmt == kRGB565 || fmt == kL8 || fmt == kPal8RGB8 ||
                  fmt == kPal8R5G6B5 || fmt == kPal4RGB8 || fmt == kPal4R5G6B5 || fmt == kPvrtcRGB4 || fmt == kPvrtcRGB2;
    switch (fmt) {
    case kRGB888: case kRGB888b:
        for (size_t i = 0; i < n; ++i) { o[i*4] = p[i*3]; o[i*4+1] = p[i*3+1]; o[i*4+2] = p[i*3+2]; o[i*4+3] = 255; }
        break;
    case kRGBA8888: case kRGBA8888b:
        std::memcpy(o, p, n * 4);
        break;
    case kRGB565: for (size_t i = 0; i < n; ++i) unpack565(rd16(p + i * 2), o + i * 4); break;
    case kRGBA5551: for (size_t i = 0; i < n; ++i) unpack5551(rd16(p + i * 2), o + i * 4); break;
    case kRGBA4444: for (size_t i = 0; i < n; ++i) unpack4444(rd16(p + i * 2), o + i * 4); break;
    case kL8: for (size_t i = 0; i < n; ++i) { o[i*4] = o[i*4+1] = o[i*4+2] = p[i]; o[i*4+3] = 255; } break;
    case kA8:
        for (size_t i = 0; i < n; ++i) { o[i*4] = o[i*4+1] = o[i*4+2] = 255; o[i*4+3] = p[i]; }
        out.alphaOnly = true;
        break;
    case kLA88: for (size_t i = 0; i < n; ++i) { o[i*4] = o[i*4+1] = o[i*4+2] = p[i*2]; o[i*4+3] = p[i*2+1]; } break;
    case kPvrtcRGBA4: case kPvrtcRGB4:
        decodePvrtc4(p, w, ht, o);
        break;
    default:
        if (pal8) {
            for (size_t i = 0; i < n; ++i) std::memcpy(o + i * 4, palette[p[i]], 4);
        } else if (pal4) {
            // OES_compressed_paletted_texture: first texel in the high nibble
            for (size_t i = 0; i < n; ++i) {
                const uint8_t b = p[i >> 1];
                std::memcpy(o + i * 4, palette[(i & 1) ? (b & 15) : (b >> 4)], 4);
            }
        } else {
            return fail(error, "unsupported pixel format");
        }
    }
    return true;
}

} // namespace cr
