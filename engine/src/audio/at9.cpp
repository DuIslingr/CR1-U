#include "audio/at9.hpp"

#include <algorithm>
#include <cstring>

#ifdef CR_HAVE_ATRAC9
#include <libatrac9.h>
#endif

namespace cr {

namespace {

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// KSDATAFORMAT_SUBTYPE_ATRAC9 {47E142D2-36BA-4D8D-88FC-61654F8C836C}
const uint8_t kAtrac9Guid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                 0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};

} // namespace

bool parseAt9(const std::vector<uint8_t>& file, At9File& out, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    const uint8_t* f = file.data();
    const size_t size = file.size();
    if (size < 12 || std::memcmp(f, "RIFF", 4) != 0 || std::memcmp(f + 8, "WAVE", 4) != 0) return fail("not a RIFF WAVE");
    out = At9File();
    bool fmt = false;
    for (size_t p = 12; p + 8 <= size;) {
        const uint32_t len = le32(f + p + 4);
        const uint8_t* c = f + p + 8;
        if (len > size - p - 8) return fail("chunk past the end");
        if (std::memcmp(f + p, "fmt ", 4) == 0) {
            // tag, channels, rate, bytes/s, block align, bits, cbSize, samples/block, channel
            // mask, sub-format GUID, version, config[4], reserved
            if (len < 52 || le16(c) != 0xfffe || std::memcmp(c + 24, kAtrac9Guid, 16) != 0) return fail("not ATRAC9");
            out.channels = le16(c + 2);
            out.rate = int(le32(c + 4));
            out.superframeBytes = le16(c + 12);
            std::memcpy(out.config, c + 44, 4);
            fmt = true;
        } else if (std::memcmp(f + p, "fact", 4) == 0 && len >= 8) {
            out.samples = le32(c);
            out.delay = int(le32(c + (len >= 12 ? 8 : 4)));
        } else if (std::memcmp(f + p, "smpl", 4) == 0 && len >= 36 + 24 && le32(c + 28) > 0) {
            out.loopStart = le32(c + 36 + 8);
            out.loopEnd = le32(c + 36 + 12);
        } else if (std::memcmp(f + p, "data", 4) == 0) {
            out.data = c;
            out.dataSize = len;
        }
        p += 8 + len + (len & 1);
    }
    if (!fmt || !out.data || out.superframeBytes <= 0) return fail("missing fmt / data");
    if (out.loopEnd >= out.samples) out.loopEnd = out.samples - 1;
    if (out.loopStart >= out.loopEnd) out.loopStart = out.loopEnd = -1;
    return true;
}

At9Decoder::At9Decoder() = default;
At9Decoder::~At9Decoder() {
#ifdef CR_HAVE_ATRAC9
    if (handle_) Atrac9ReleaseHandle(handle_);
#endif
}

bool At9Decoder::open(const At9File& f) {
#ifndef CR_HAVE_ATRAC9
    (void)f;
    return false;
#else
    if (handle_) Atrac9ReleaseHandle(handle_);
    handle_ = Atrac9GetHandle();
    file_ = &f;
    uint8_t config[4];
    std::memcpy(config, f.config, 4);
    if (!handle_ || Atrac9InitDecoder(handle_, config) != 0) return false;
    Atrac9CodecInfo info;
    if (Atrac9GetCodecInfo(handle_, &info) != 0 || info.channels != f.channels) return false;
    frames_ = info.framesInSuperframe;
    frameSamples_ = info.frameSamples;
    superframeSamples_ = frames_ * frameSamples_;
    return superframeSamples_ > 0;
#endif
}

size_t At9Decoder::superframes() const {
    return file_ && file_->superframeBytes > 0 ? file_->dataSize / size_t(file_->superframeBytes) : 0;
}

bool At9Decoder::decode(size_t i, std::vector<int16_t>& pcm) {
#ifndef CR_HAVE_ATRAC9
    (void)i, (void)pcm;
    return false;
#else
    if (!handle_ || !file_ || i >= superframes()) return false;
    pcm.resize(size_t(superframeSamples_) * size_t(file_->channels));
    const uint8_t* p = file_->data + i * size_t(file_->superframeBytes);
    const uint8_t* end = p + file_->superframeBytes;
    for (int k = 0; k < frames_; ++k) {
        int used = 0;
        if (Atrac9Decode(handle_, p, pcm.data() + size_t(k) * size_t(frameSamples_) * size_t(file_->channels), &used) != 0 ||
            used <= 0 || p + used > end)
            return false;
        p += used;
    }
    return true;
#endif
}

bool decodeAt9(const std::vector<uint8_t>& file, std::vector<int16_t>& pcm, int& rate, int& channels, std::string* error) {
    At9File f;
    if (!parseAt9(file, f, error)) return false;
    At9Decoder dec;
    if (!dec.open(f)) { if (error) *error = "no ATRAC9 decoder"; return false; }
    rate = f.rate, channels = f.channels;
    pcm.clear();
    pcm.reserve(size_t(f.samples + f.delay) * size_t(f.channels));
    std::vector<int16_t> block;
    for (size_t i = 0; i < dec.superframes() && int64_t(pcm.size()) < (f.samples + f.delay) * f.channels; ++i) {
        if (!dec.decode(i, block)) break;
        pcm.insert(pcm.end(), block.begin(), block.end());
    }
    const size_t from = std::min(pcm.size(), size_t(f.delay) * size_t(f.channels));
    const size_t to = std::min(pcm.size(), size_t(f.delay + f.samples) * size_t(f.channels));
    pcm = std::vector<int16_t>(pcm.begin() + std::ptrdiff_t(from), pcm.begin() + std::ptrdiff_t(to));
    if (pcm.empty()) { if (error) *error = "nothing decoded"; return false; }
    return true;
}

} // namespace cr
