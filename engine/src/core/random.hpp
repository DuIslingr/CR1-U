#pragma once
// The game's random numbers (_get_random; the same in 1.0.1 and 2.2.0). One MT19937 stream
// (_InitMt_r / _NextMt_r - std::mt19937's seeding and output exactly; CRView::setup seeds it with
// the clock, CFAbsoluteTimeGetCurrent's seconds) drives the battles and the field alike.
// get_random(n) first draws one number below 10000 and throws away r % 3 + 1 more, then draws
// below n. A draw below n (FUN_00048b90, 1.0.1 inlines it) takes the top 31 bits and scales n
// up by doubling past 2^30, rejecting what falls outside: no modulo bias.

#include <cstdint>
#include <random>

namespace cr {

class GameRandom {
public:
    GameRandom() = default;
    explicit GameRandom(uint32_t seed) : mt_(seed) {}
    void seed(uint32_t s) { mt_.seed(s); }
    uint32_t next() { return uint32_t(mt_()); }

    // A draw in 0..n-1 (0 when n < 1)
    int below(int n) {
        if (n <= 0) return 0;
        const uint32_t un = uint32_t(n);
        if (range_ != un) { // the range, doubled past 2^30 (cached per n)
            range_ = un, scaled_ = un, shift_ = 0;
            if (un <= 0x40000000u)
                do scaled_ <<= 1, ++shift_; while (scaled_ <= 0x40000000u);
        }
        for (;;) {
            uint32_t r = next() >> 1, sc = scaled_;
            int sh = shift_;
            if (r < sc) return int(r >> sh);
            r -= sc;
            for (int32_t lim = int32_t(0x80000000u - sc); int32_t(n) <= lim; lim -= int32_t(sc)) {
                while (lim < int32_t(sc)) sc >>= 1, --sh;
                if (r < sc) return int(r >> sh);
                r -= sc;
            }
        }
    }

    // _get_random(n)
    int get(int n) {
        for (int k = below(10000) % 3 + 1; k > 0; --k) below(10000);
        return below(n);
    }
    int operator()(int n) { return get(n); }

private:
    std::mt19937 mt_{5489u}; // _NextMt_r's own seed when nothing seeded it (0x1571)
    uint32_t range_ = 0, scaled_ = 0;
    int shift_ = 0;
};

} // namespace cr
