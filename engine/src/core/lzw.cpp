#include "core/lzw.hpp"

namespace cr {

namespace {

// Reads LSB-first variable-width codes across GIF-style sub-blocks without
// first concatenating them.
struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t pos;          // next byte in the current sub-block
    size_t blockLeft = 0;
    uint32_t bits = 0;
    int nbits = 0;
    bool eof = false;

    bool fill() {
        while (blockLeft == 0) {
            if (pos >= size) return false;
            blockLeft = data[pos++];
            if (blockLeft == 0) return false; // terminator block
        }
        if (pos >= size) return false;
        bits |= uint32_t(data[pos++]) << nbits;
        nbits += 8;
        --blockLeft;
        return true;
    }

    // Returns -1 at end of data.
    int read(int width) {
        while (nbits < width) {
            if (!fill()) { eof = true; return -1; }
        }
        int code = int(bits & ((1u << width) - 1));
        bits >>= width;
        nbits -= width;
        return code;
    }

    // Skips to the end of the sub-block chain so the caller learns the stream length.
    size_t finish() {
        pos += blockLeft;
        blockLeft = 0;
        while (pos < size) {
            uint8_t n = data[pos++];
            if (n == 0) break;
            pos += n;
        }
        return pos;
    }
};

} // namespace

bool lzwDecode(const uint8_t* data, size_t size, size_t pos, std::vector<uint8_t>& out,
               size_t expected, size_t* end) {
    if (pos >= size) return false;
    const int minSize = data[pos];
    if (minSize < 2 || minSize > 11) return false;

    const int clear = 1 << minSize;
    const int eoi = clear + 1;
    constexpr int kMaxCodes = 4096;

    // Dictionary as prefix links: entry = prefix[code] + suffix[code]
    std::vector<uint16_t> prefix(kMaxCodes);
    std::vector<uint8_t> suffix(kMaxCodes), first(kMaxCodes);
    std::vector<uint16_t> length(kMaxCodes);
    for (int i = 0; i < clear; ++i) {
        suffix[i] = first[i] = uint8_t(i);
        length[i] = 1;
    }

    out.clear();
    if (expected) out.reserve(expected);

    BitReader br{data, size, pos + 1};
    int width = minSize + 1;
    int next = clear + 2;
    int prev = -1;

    auto emit = [&](int code) {
        const size_t n = length[code];
        const size_t base = out.size();
        out.resize(base + n);
        for (size_t i = n; i-- > 0;) {
            out[base + i] = suffix[code];
            code = prefix[code];
        }
    };

    for (;;) {
        const int code = br.read(width);
        if (code < 0 || code == eoi) break;
        if (code == clear) {
            width = minSize + 1;
            next = clear + 2;
            prev = -1;
            continue;
        }
        if (prev < 0) {
            if (code >= clear) return false;
            emit(code);
            prev = code;
            continue;
        }
        uint8_t head;
        if (code < next) {
            head = first[code];
        } else if (code == next) {
            head = first[prev]; // KwKwK case
        } else {
            return false;
        }
        if (next < kMaxCodes) {
            prefix[next] = uint16_t(prev);
            suffix[next] = head;
            first[next] = first[prev];
            length[next] = uint16_t(length[prev] + 1);
            ++next;
        }
        emit(code);
        prev = code;
        if (next == (1 << width) && width < 12) ++width;
    }

    if (end) *end = br.finish();
    return true;
}

} // namespace cr
