#include "assets/obb.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

#ifdef CR_HAVE_ZLIB
#include <zlib.h>
#endif

namespace cr {

namespace {

uint16_t u16(const std::vector<uint8_t>& d, size_t at) { return at + 2 <= d.size() ? uint16_t(d[at] | d[at + 1] << 8) : 0; }
uint32_t u32(const std::vector<uint8_t>& d, size_t at) {
    return at + 4 <= d.size() ? uint32_t(d[at] | d[at + 1] << 8 | d[at + 2] << 16 | uint32_t(d[at + 3]) << 24) : 0;
}
uint32_t uleb(const std::vector<uint8_t>& d, size_t& at) {
    uint32_t v = 0;
    for (int shift = 0; shift < 35 && at < d.size(); shift += 7) {
        const uint8_t b = d[at++];
        v |= uint32_t(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
    }
    return v;
}

// Dalvik instructions' lengths in 16-bit units, by opcode (the payload pseudo-instructions apart)
int insnUnits(uint8_t op) {
    static const uint8_t kLen[256] = {
        // 0x00..0x0f
        1, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 1, 1, 1, 1, 1,
        // 0x10..0x1f
        1, 1, 1, 2, 3, 2, 2, 3, 5, 2, 2, 3, 2, 1, 1, 2,
        // 0x20..0x2f
        2, 1, 2, 2, 3, 3, 3, 1, 1, 2, 3, 3, 3, 2, 2, 2,
        // 0x30..0x3f
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,
        // 0x40..0x4f
        1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        // 0x50..0x5f
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        // 0x60..0x6f
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3,
        // 0x70..0x7f
        3, 3, 3, 1, 3, 3, 3, 3, 3, 1, 1, 1, 1, 1, 1, 1,
        // 0x80..0x8f
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        // 0x90..0x9f
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        // 0xa0..0xaf
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        // 0xb0..0xbf
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        // 0xc0..0xcf
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        // 0xd0..0xdf
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        // 0xe0..0xef
        2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        // 0xf0..0xff
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 3, 3, 2, 2,
    };
    return kLen[op];
}

std::string dexString(const std::vector<uint8_t>& d, uint32_t idx) {
    const uint32_t count = u32(d, 0x38), ids = u32(d, 0x3c);
    if (idx >= count) return std::string();
    size_t at = u32(d, ids + 4 * size_t(idx));
    uleb(d, at); // utf16 size
    std::string s;
    while (at < d.size() && d[at]) s += char(d[at++]); // (the names are ASCII)
    return s;
}

} // namespace

std::string ObbArchive::lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return o;
}

bool ObbArchive::readTable(const std::vector<uint8_t>& d, uint64_t total, std::vector<std::string>& names,
                           std::vector<uint32_t>& sizes, std::vector<uint32_t>& crcs, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (d.size() < 0x70 || std::memcmp(d.data(), "dex\n", 4) != 0) return fail("not a classes.dex");
    struct Payload { uint32_t count; size_t at; };
    std::vector<Payload> payloads;           // fill-array-data, 4-byte elements
    std::vector<std::vector<uint32_t>> strs; // each method's const-strings in order
    const uint32_t classes = u32(d, 0x60), defs = u32(d, 0x64);
    for (uint32_t c = 0; c < classes; ++c) {
        size_t at = u32(d, defs + 32 * size_t(c) + 24); // class_data_off
        if (!at) continue;
        const uint32_t sf = uleb(d, at), inf = uleb(d, at), dm = uleb(d, at), vm = uleb(d, at);
        for (uint32_t k = 0; k < 2 * (sf + inf); ++k) uleb(d, at);
        for (uint32_t m = 0; m < dm + vm; ++m) {
            uleb(d, at), uleb(d, at);
            const uint32_t code = uleb(d, at);
            if (!code) continue;
            const uint32_t units = u32(d, code + 12);
            const size_t insns = code + 16;
            std::vector<uint32_t> s;
            for (uint32_t pc = 0; pc < units;) {
                const uint16_t w = u16(d, insns + 2 * size_t(pc));
                const uint8_t op = uint8_t(w);
                if (op == 0 && (w >> 8) >= 1 && (w >> 8) <= 3) { // a payload
                    const uint16_t n16 = u16(d, insns + 2 * size_t(pc) + 2);
                    if ((w >> 8) == 1) pc += 4 + 2 * n16;
                    else if ((w >> 8) == 2) pc += 2 + 4 * n16;
                    else {
                        const uint32_t n = u32(d, insns + 2 * size_t(pc) + 4);
                        if (n16 == 4) payloads.push_back({n, insns + 2 * size_t(pc) + 8});
                        pc += 4 + uint32_t((uint64_t(n) * n16 + 1) / 2);
                    }
                    continue;
                }
                if (op == 0x1a) s.push_back(u16(d, insns + 2 * size_t(pc) + 2));
                if (op == 0x1b) s.push_back(u32(d, insns + 2 * size_t(pc) + 2));
                pc += uint32_t(insnUnits(op));
            }
            if (!s.empty()) strs.push_back(std::move(s));
        }
    }
    // The table: two payloads of one length N, the sizes adding up to the OBB, and a method with
    // exactly N const-strings - the names in the same order
    for (const Payload& a : payloads) {
        if (a.count < 16 || a.at + 4 * size_t(a.count) > d.size()) continue;
        uint64_t sum = 0;
        for (uint32_t i = 0; i < a.count; ++i) sum += u32(d, a.at + 4 * size_t(i));
        if (total && sum != total) continue;
        const Payload* crc = nullptr;
        for (const Payload& b : payloads)
            if (&b != &a && b.count == a.count && b.at + 4 * size_t(b.count) <= d.size()) crc = &b;
        const std::vector<uint32_t>* order = nullptr;
        for (const auto& s : strs)
            if (s.size() == a.count) order = &s;
        if (!crc || !order) continue;
        names.clear(), sizes.clear(), crcs.clear();
        for (uint32_t i = 0; i < a.count; ++i) {
            names.push_back(dexString(d, (*order)[i]));
            sizes.push_back(u32(d, a.at + 4 * size_t(i)));
            crcs.push_back(u32(d, crc->at + 4 * size_t(i)));
        }
        return true;
    }
    return fail(total ? "no file table in classes.dex that adds up to the OBB" : "no file table in classes.dex");
}

bool ObbArchive::readDexFromApk(const std::string& apk, std::vector<uint8_t>& dex, std::string* error) {
    auto fail = [&](const std::string& why) { if (error) *error = apk + ": " + why; return false; };
    size_t len = 0;
    void* raw = SDL_LoadFile(apk.c_str(), &len);
    if (!raw) return fail("cannot read");
    std::vector<uint8_t> z(static_cast<uint8_t*>(raw), static_cast<uint8_t*>(raw) + len);
    SDL_free(raw);
    if (z.size() >= 4 && std::memcmp(z.data(), "dex\n", 4) == 0) { dex = std::move(z); return true; } // a classes.dex itself
    size_t eocd = std::string::npos;
    for (size_t i = z.size() >= 22 ? z.size() - 22 : 0; i + 22 <= z.size() && i + 0x10016 >= z.size(); --i) {
        if (u32(z, i) == 0x06054b50) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) return fail("not a zip (APK)");
    const uint16_t count = u16(z, eocd + 10);
    size_t at = u32(z, eocd + 16);
    for (uint16_t k = 0; k < count && u32(z, at) == 0x02014b50; ++k) {
        const uint16_t method = u16(z, at + 10), nameLen = u16(z, at + 28), extra = u16(z, at + 30), comment = u16(z, at + 32);
        const uint32_t csize = u32(z, at + 20), usize = u32(z, at + 24), local = u32(z, at + 42);
        const std::string name(reinterpret_cast<const char*>(z.data()) + at + 46, std::min<size_t>(nameLen, z.size() - at - 46));
        at += 46 + size_t(nameLen) + extra + comment;
        if (name != "classes.dex") continue;
        if (u32(z, local) != 0x04034b50) return fail("bad local header");
        const size_t data = local + 30 + u16(z, local + 26) + u16(z, local + 28);
        if (data + csize > z.size()) return fail("truncated");
        if (method == 0) { dex.assign(z.begin() + std::ptrdiff_t(data), z.begin() + std::ptrdiff_t(data + csize)); return true; }
#ifdef CR_HAVE_ZLIB
        if (method != 8) return fail("classes.dex: unknown compression");
        dex.resize(usize);
        z_stream s{};
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return fail("inflate");
        s.next_in = z.data() + data, s.avail_in = csize, s.next_out = dex.data(), s.avail_out = usize;
        const int r = inflate(&s, Z_FINISH);
        inflateEnd(&s);
        return r == Z_STREAM_END ? true : fail("classes.dex: inflate failed");
#else
        return fail("built without zlib");
#endif
    }
    return fail("no classes.dex");
}

bool ObbArchive::open(const std::string& obb, const std::string& apk, std::string* error) {
    entries_.clear(), byName_.clear();
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(obb.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) {
        if (error) *error = obb + ": cannot open";
        return false;
    }
    std::vector<uint8_t> dex;
    std::vector<std::string> names;
    std::vector<uint32_t> sizes, crcs;
    if (!readDexFromApk(apk, dex, error) || !readTable(dex, info.size, names, sizes, crcs, error)) return false;
    path_ = obb;
    uint64_t off = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        byName_[lower(names[i])] = entries_.size();
        entries_.push_back({names[i], off, sizes[i]});
        off += sizes[i];
    }
    return true;
}

bool ObbArchive::read(const std::string& name, std::vector<uint8_t>& out) const {
    const auto it = byName_.find(lower(name));
    if (it == byName_.end()) return false;
    const Entry& e = entries_[it->second];
    SDL_IOStream* io = SDL_IOFromFile(path_.c_str(), "rb");
    if (!io) return false;
    out.resize(e.size);
    const bool ok = SDL_SeekIO(io, Sint64(e.offset), SDL_IO_SEEK_SET) == Sint64(e.offset) &&
                    SDL_ReadIO(io, out.data(), e.size) == e.size;
    SDL_CloseIO(io);
    // The event files (the areas' and sys.evt) are stored scrambled: file_load_malloc (0x39980:
    // fldread_evt_init2, sys.evt) XORs every other byte from 0x1000 with a 21-byte key, in turn
    const std::string& n = e.name;
    if (ok && n.size() > 4 && SDL_strcasecmp(n.c_str() + n.size() - 4, ".evt") == 0) {
        static const uint8_t key[21] = {0x61, 0x19, 0x4a, 0xed, 0x04, 0xa7, 0xa1, 0x48, 0x5a, 0x6f, 0x44,
                                        0xb9, 0xd9, 0xed, 0x74, 0xda, 0x75, 0x9c, 0xd8, 0xf5, 0x3f};
        for (size_t i = 0x1000, k = 0; i < out.size(); i += 2, k = (k + 1) % 21) out[i] ^= key[k];
    }
    return ok;
}

std::vector<std::string> ObbArchive::names() const {
    std::vector<std::string> n;
    n.reserve(entries_.size());
    for (const Entry& e : entries_) n.push_back(e.name);
    return n;
}

bool ObbArchive::locate(const std::string& name, uint64_t& offset, uint32_t& size) const {
    const auto it = byName_.find(lower(name));
    if (it == byName_.end()) return false;
    offset = entries_[it->second].offset, size = entries_[it->second].size;
    return true;
}

} // namespace cr
