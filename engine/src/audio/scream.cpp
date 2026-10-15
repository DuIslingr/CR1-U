#include "audio/scream.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hevag_coefs.h" // third_party/hevag (vgmstream's table, its licence beside it)

namespace cr {

namespace {

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// A wave's rate from its centre note / fine tune, snapped to the usual rates within 1%
int noteRate(int note, int fine) {
    const double rate = 48000.0 * std::pow(2.0, (double(note - 0xc4) + fine / 128.0) / 12.0);
    for (int r : {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000})
        if (std::fabs(rate - r) < r * 0.01) return r;
    return int(rate + 0.5);
}

} // namespace

void decodeHevag(const uint8_t* data, size_t size, int channels, std::vector<int16_t>& out) {
    const size_t frames = size / 16 / size_t(std::max(channels, 1));
    out.assign(frames * 28 * size_t(channels), 0);
    for (int c = 0; c < channels; ++c) {
        int32_t h1 = 0, h2 = 0, h3 = 0, h4 = 0;
        for (size_t f = 0; f < frames; ++f) {
            const uint8_t* fr = data + (f * size_t(channels) + size_t(c)) * 16;
            const int shift = fr[0] & 0x0f;
            int index = (fr[0] >> 4) | (fr[1] & 0xf0);
            const int flag = fr[1] & 0x0f;
            if (index > 127) index = 0;
            const float* k = hevag_coefs_f[index];
            for (int i = 0; i < 28; ++i) {
                int32_t sample = 0;
                if (flag < 7) { // (flag 7: silence)
                    const uint8_t b = fr[2 + i / 2];
                    const int nibble = i & 1 ? b >> 4 : b & 0x0f; // the low nibble first
                    const int32_t code = ((nibble >= 8 ? nibble - 16 : nibble) * 4096) >> shift;
                    sample = int32_t(float(h1) * k[0] + float(h2) * k[1] + float(h3) * k[2] + float(h4) * k[3]);
                    sample += code;
                }
                out[(f * 28 + size_t(i)) * size_t(channels) + size_t(c)] = int16_t(std::clamp(sample, -32768, 32767));
                h4 = h3, h3 = h2, h2 = h1, h1 = sample;
            }
        }
    }
}

bool parseScreamBank(const std::vector<uint8_t>& file, std::vector<PcmSound>& out, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    const uint8_t* d = file.data();
    const size_t size = file.size();
    if (size < 0x20) return fail("too short");
    const uint32_t sb = le32(d + 0x08), data = le32(d + 0x10);
    if (sb + 0x40 > size || std::memcmp(d + sb, "SBlk", 4) != 0) return fail("no SBlk block");
    if (le32(d + sb + 4) != 9) return fail("SBlk version not 9");
    const int sounds = le16(d + sb + 0x16);
    const size_t t1 = sb + le32(d + sb + 0x1c), t2 = sb + le32(d + sb + 0x20), t3 = sb + le32(d + sb + 0x34);
    out.assign(size_t(sounds), PcmSound());
    for (int s = 0; s < sounds; ++s) {
        const size_t e = t1 + 12 * size_t(s);
        if (e + 12 > size) return fail("sound table past the end");
        const int grains = d[e + 4];
        const size_t first = le32(d + e + 8);
        for (int g = 0; g < grains; ++g) { // the first grain that plays a wave
            const size_t go = t2 + first + 8 * size_t(g);
            if (go + 8 > size) break;
            const uint32_t v = le32(d + go);
            if (v >> 16 != 0x0100) continue;
            const size_t h = t3 + (v & 0xffff);
            if (h + 0x18 > size) break;
            const size_t off = data + size_t(le32(d + h + 0x10)), len = le32(d + h + 0x14);
            if (len < 0x20 || off + len > size) break;
            const uint32_t subtype = le32(d + off);
            if (subtype != 0 && subtype != 3) break; // (only mono / stereo HE-VAG here)
            PcmSound& p = out[size_t(s)];
            p.channels = subtype == 3 ? 2 : 1;
            p.rate = noteRate(d[h + 2], d[h + 3]);
            p.bits = 16;
            std::vector<int16_t> pcm;
            decodeHevag(d + off + 0x20, len - 0x20, p.channels, pcm);
            p.data.resize(pcm.size() * 2);
            for (size_t i = 0; i < pcm.size(); ++i) {
                const uint16_t u = uint16_t(pcm[i]);
                p.data[i * 2] = uint8_t(u), p.data[i * 2 + 1] = uint8_t(u >> 8);
            }
            break;
        }
    }
    return true;
}

} // namespace cr
