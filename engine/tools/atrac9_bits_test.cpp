// Bit-level decoder regression checks; synthetic inputs, no game assets.
#include <cstdint>
#include <cstdio>
#include <climits>
extern "C" {
#include "utility.h"
#include "bit_reader.h"
}

int main() {
    int problems = 0, checks = 0;
    uint32_t random = 0x831cb9a7u;
    for (int i = 0; i < 4096; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        for (int bits = 0; bits <= 32; ++bits) {
            uint32_t reversed = 0;
            for (int b = 0; b < bits; ++b) reversed = (reversed << 1) | ((random >> b) & 1u);
            problems += BitReverse32(random, bits) != reversed;
            const uint64_t mask = bits == 32 ? UINT32_MAX : (uint64_t(1) << bits) - 1;
            const uint64_t raw = random & mask;
            const int64_t expected = bits && (raw & (uint64_t(1) << (bits - 1))) ? int64_t(raw) - (int64_t(1) << bits) : int64_t(raw);
            problems += SignExtend32(int32_t(random), bits) != expected;
            checks += 2;
        }
        const uint8_t bytes[8] = {uint8_t(random >> 24), uint8_t(random >> 16), uint8_t(random >> 8), uint8_t(random), 0xa5, 0x69, 0xfe, 0x10};
        for (int offset = 0; offset < 8; ++offset) for (int bits = 0; bits <= 32; ++bits) {
            BitReaderCxt reader;
            InitBitReaderCxt(&reader, bytes);
            reader.Position = offset;
            uint32_t expected = 0;
            for (int b = 0; b < bits; ++b) {
                const int at = offset + b;
                expected = (expected << 1) | ((bytes[at / 8] >> (7 - at % 8)) & 1u);
            }
            problems += uint32_t(PeekInt(&reader, bits)) != expected || reader.Position != offset;
            problems += uint32_t(ReadInt(&reader, bits)) != expected || reader.Position != offset + bits;
            checks += 2;
        }
    }
    std::printf("ATRAC9 bit operations: %d checks, %d problems\n", checks, problems);
    return problems ? 1 : 0;
}
