// Vita GXT textures: a bare GXT (a fan translation's) in UBC3 - DXT5 - with its 4x4 blocks swizzled
// (Morton, the y bit lowest), a UBC1 block's transparent texel, the same through decodeMct
#include "assets/gxt.hpp"
#include "assets/texture.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
std::vector<uint8_t> gxt(uint32_t format, uint32_t type, int w, int h, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> f(0x40, 0);
    auto put32 = [&](size_t p, uint32_t v) { std::memcpy(&f[p], &v, 4); };
    auto put16 = [&](size_t p, uint16_t v) { std::memcpy(&f[p], &v, 2); };
    std::memcpy(f.data(), "GXT\0", 4);
    put32(4, 0x10000003), put32(8, 1), put32(0x0c, 0x40), put32(0x10, uint32_t(data.size()));
    put32(0x20, 0x40), put32(0x24, uint32_t(data.size())), put32(0x28, 0xffffffffu);
    put32(0x30, type), put32(0x34, format), put16(0x38, uint16_t(w)), put16(0x3a, uint16_t(h)), put16(0x3c, 1);
    f.insert(f.end(), data.begin(), data.end());
    return f;
}
// A DXT5 block of one colour (RGB565 `c`) at one alpha
void solid5(std::vector<uint8_t>& d, uint16_t c, uint8_t a) {
    const uint8_t b[16] = {a, a, 0, 0, 0, 0, 0, 0, uint8_t(c), uint8_t(c >> 8), uint8_t(c), uint8_t(c >> 8), 0, 0, 0, 0};
    d.insert(d.end(), b, b + 16);
}
} // namespace

int main() {
    int problems = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) ++problems, std::fprintf(stderr, "gxt FAILED: %s\n", what);
    };
    // 8x8: blocks (0,0) red, (0,1) green, (1,0) blue, (1,1) white - stored in swizzled order
    std::vector<uint8_t> d;
    solid5(d, 0xF800, 255), solid5(d, 0x07E0, 128), solid5(d, 0x001F, 64), solid5(d, 0xFFFF, 0);
    cr::Image img;
    std::string err;
    check(cr::decodeGxt(gxt(0x87000000, 0, 8, 8, d), img, &err) && img.width == 8 && img.height == 8, "UBC3 decodes");
    auto px = [&](int x, int y) { return &img.rgba[(size_t(y) * img.width + x) * 4]; };
    if (img.rgba.size() == 256) {
        check(px(1, 1)[0] == 255 && px(1, 1)[1] == 0 && px(1, 1)[3] == 255, "block (0,0) red, opaque");
        check(px(2, 6)[1] == 255 && px(2, 6)[0] == 0 && px(2, 6)[3] == 128, "block (0,1) green, half alpha (the y bit lowest)");
        check(px(6, 2)[2] == 255 && px(6, 2)[0] == 0 && px(6, 2)[3] == 64, "block (1,0) blue");
        check(px(7, 7)[0] == 255 && px(7, 7)[1] == 255 && px(7, 7)[3] == 0, "block (1,1) white, clear");
    }
    // linear (type 0x60000000): the blocks in row order
    check(cr::decodeGxt(gxt(0x87000000, 0x60000000, 8, 8, d), img) && img.rgba.size() == 256 && px(6, 2)[1] == 255,
          "linear UBC3: block (1,0) the second stored");
    // DXT5's interpolated alpha: a0 200, a1 100, every texel index 2 -> (6*200 + 1*100) / 7
    std::vector<uint8_t> a = {200, 100, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0, 0, 0, 0, 0, 0, 0, 0};
    check(cr::decodeGxt(gxt(0x87000000, 0, 4, 4, a), img) && img.rgba.size() == 64 && img.rgba[3] == 185, "DXT5 alpha step");
    // UBC1 with c0 <= c1: index 3 transparent
    std::vector<uint8_t> b1 = {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    check(cr::decodeGxt(gxt(0x85000000, 0, 4, 4, b1), img) && img.rgba.size() == 64 && img.rgba[3] == 0, "UBC1 transparent texel");
    // a bare GXT through decodeMct (the overlay's path for the Vita copy alone)
    cr::MctInfo info;
    check(cr::decodeMct(gxt(0x87000000, 0, 8, 8, d), img, &info) && info.width == 8 && info.texId == 0, "decodeMct takes a bare GXT");
    // a block format whose sides aren't multiples of 4 is refused, not read past its end
    check(!cr::decodeGxt(gxt(0x87000000, 0, 6, 8, d), img), "6 wide refused");
    if (!problems) std::printf("gxt: all passed\n");
    return problems ? 1 : 0;
}
