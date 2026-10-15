#include "assets/animation.hpp"

#include <cmath>
#include <cstring>

#include "core/lzw.hpp"

namespace cr {

namespace {

struct Reader {
    const uint8_t* p;
    size_t size;
    size_t pos = 0;
    bool ok = true;

    bool need(size_t n) {
        if (pos + n > size) ok = false;
        return ok;
    }
    uint8_t u8() { return need(1) ? p[pos++] : 0; }
    uint16_t u16() {
        if (!need(2)) return 0;
        const uint16_t v = uint16_t(p[pos] | (p[pos + 1] << 8));
        pos += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v;
        std::memcpy(&v, p + pos, 4);
        pos += 4;
        return v;
    }
    float f32() {
        const uint32_t u = u32();
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    bool fixed = false; // header option byte: 0 = f32, 1 = s32 16.16 fixed point
    // A "dynamic"-typed real: f32 or 16.16 fixed depending on the block's option byte
    float real() {
        if (!fixed) return f32();
        return float(int32_t(u32())) / 65536.0f;
    }
    void skip(size_t n) {
        if (need(n)) pos += n;
    }
};

// Components per key for the packed encodings (getKeyTypeCompoNum(kind, 1), table 0xcb6a4)
int packedComps(int kind) { return kind >= 11 && kind <= 30 ? (kind - 11) % 4 + 1 : 0; }

constexpr int kValueSize[4] = {0, 1, 2, 4};
constexpr int kMaxComps = 4;

bool parseAction(Reader& r, Action& act, std::string& err) {
    const size_t start = r.pos;
    const uint32_t size = r.u32();
    act.objClass = r.u32(); // class, id, user
    act.objId = r.u32();
    act.objUser = r.u32();
    const size_t end = start + size;
    if (size < 16 || end > r.size) { err = "bad action record size"; return false; }

    r.u32();
    const bool compressed = r.u8() == 1;
    const uint32_t decodedSize = r.u32();
    act.duration = r.real();
    r.u8();
    r.u8();
    const int nSegments = r.u16();
    if (!r.ok) { err = "truncated action header"; return false; }

    std::vector<uint8_t> buf;
    Reader s{nullptr, 0};
    if (compressed) {
        if (!lzwDecode(r.p, end, r.pos, buf, decodedSize)) { err = "action LZW failed"; return false; }
        s = Reader{buf.data(), buf.size()};
    } else {
        s = Reader{r.p + r.pos, end - r.pos};
    }
    s.fixed = r.fixed;

    act.segments.resize(nSegments);
    for (auto& seg : act.segments) {
        const int nTracks = s.u16();
        seg.resize(nTracks);
        for (Track& tr : seg) {
            tr.type = s.u8();
            const int kind = s.u8();
            const int nKeys = s.u16();
            s.u16();
            s.u16();
            const bool usable = (kind >= 1 && kind <= kMaxComps);
            tr.comps = usable ? uint8_t(kind) : 0;
            if (usable) tr.keys.resize(nKeys);
            for (int k = 0; k < nKeys && s.ok; ++k) {
                if (kind <= 9) {
                    Key key;
                    key.mode = s.u8();
                    key.n = s.u8();
                    key.time = s.real();
                    const int count = kind * key.n;
                    for (int i = 0; i < count; ++i) {
                        const float f = s.real();
                        if (usable && i < 12) key.v[i] = f;
                    }
                    if (usable) tr.keys[k] = key;
                } else {
                    // Packed keys: not used by shipped data, consumed to stay in sync
                    s.u8();
                    const int fmt = s.u8();
                    s.f32();
                    const int comps = packedComps(kind);
                    if (fmt == 0) {
                        s.skip(size_t(comps) * 4);
                    } else {
                        for (int i = 0; i < comps; ++i) s.skip(size_t(kValueSize[(fmt >> (2 * i)) & 3]));
                    }
                }
            }
        }
        if (!s.ok) { err = "action data truncated"; return false; }
    }
    r.pos = end;
    return true;
}

} // namespace

bool parseMca(const uint8_t* data, size_t size, size_t pos, std::vector<Action>& out, std::string* error) {
    std::string err;
    Reader r{data, size, pos};
    if (!r.need(4) || std::memcmp(data + pos, "MCA_", 4) != 0) {
        if (error) *error = "not an MCA_ block";
        return false;
    }
    r.pos += 4;
    r.u32();
    r.fixed = r.u8() == 1; // option byte: real-number format of this block
    r.skip(3);
    const int n = r.u16();
    for (int i = 0; i < n; ++i) {
        Action act;
        if (!parseAction(r, act, err)) {
            if (error) *error = err;
            return false;
        }
        out.push_back(std::move(act));
    }
    return true;
}

bool evaluateTrack(const Track& tr, float time, float* out) {
    const int c = tr.comps;
    if (!c || tr.keys.empty()) return false;
    const auto& keys = tr.keys;
    const Key* k0 = &keys.front();
    const Key* k1 = nullptr;
    if (time > keys.front().time) {
        k0 = &keys.back();
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            if (time <= keys[i + 1].time) {
                k0 = &keys[i];
                k1 = &keys[i + 1];
                break;
            }
        }
    }
    if (!k1 || k0->mode == 3 || k1->time <= k0->time) {
        for (int i = 0; i < c; ++i) out[i] = k0->v[i];
    } else {
        const float s = (time - k0->time) / (k1->time - k0->time);
        if (k0->mode == 1) {
            for (int i = 0; i < c; ++i) out[i] = k0->v[i] + s * (k1->v[i] - k0->v[i]);
        } else {
            // Hermite: tangents are used unscaled (see 0x8c4b4)
            const float s2 = s * s, s3 = s2 * s;
            const float h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s;
            const float h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
            for (int i = 0; i < c; ++i) {
                const float out0 = k0->n >= 3 ? k0->v[2 * c + i] : 0.0f;
                const float in1 = k1->n >= 2 ? k1->v[c + i] : 0.0f;
                out[i] = h00 * k0->v[i] + h10 * out0 + h01 * k1->v[i] + h11 * in1;
            }
        }
    }
    for (int i = 0; i < c; ++i)
        if (!std::isfinite(out[i])) return false;
    return true;
}

} // namespace cr
