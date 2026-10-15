#include "assets/psarc.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

#ifdef CR_HAVE_ZLIB
#include <zlib.h>
#endif

namespace cr {

namespace {
uint64_t be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | p[i];
    return v;
}
} // namespace

bool PsarcArchive::open(const std::string& path, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = path + ": " + why; entries_.clear(); return false; };
#ifndef CR_HAVE_ZLIB
    (void)path;
    return fail("built without zlib");
#else
    path_ = path;
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return fail("cannot open");
    uint8_t h[32];
    if (SDL_ReadIO(io, h, 32) != 32 || SDL_memcmp(h, "PSAR", 4) != 0 || SDL_memcmp(h + 8, "zlib", 4) != 0) {
        SDL_CloseIO(io);
        return fail("not a zlib PSARC");
    }
    const uint32_t tocLen = uint32_t(be(h + 12, 4)), entSize = uint32_t(be(h + 16, 4)), count = uint32_t(be(h + 20, 4));
    blockSize_ = uint32_t(be(h + 24, 4));
    std::vector<uint8_t> toc(tocLen > 32 ? tocLen - 32 : 0);
    const bool ok = SDL_ReadIO(io, toc.data(), toc.size()) == toc.size();
    SDL_CloseIO(io);
    if (!ok || entSize < 30 || size_t(count) * entSize > toc.size()) return fail("bad table of contents");
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = &toc[size_t(i) * entSize];
        entries_.push_back({uint32_t(be(e + 16, 4)), be(e + 20, 5), be(e + 25, 5)});
    }
    const int nb = blockSize_ == 65536 ? 2 : blockSize_ == 16777216 ? 4 : 3;
    for (size_t p = size_t(count) * entSize; p + nb <= toc.size(); p += nb) blockSizes_.push_back(uint32_t(be(&toc[p], nb)));
    std::vector<uint8_t> manifest;
    if (!readEntry(entries_[0], manifest)) return fail("unreadable manifest");
    size_t start = 0, index = 1;
    for (size_t i = 0; i <= manifest.size(); ++i)
        if (i == manifest.size() || manifest[i] == '\n') {
            std::string name(manifest.begin() + long(start), manifest.begin() + long(i));
            const size_t slash = name.find_last_of('/');
            if (!name.empty() && index < entries_.size()) byName_[slash == std::string::npos ? name : name.substr(slash + 1)] = index;
            ++index;
            start = i + 1;
        }
    return true;
#endif
}

bool PsarcArchive::readEntry(const Entry& e, std::vector<uint8_t>& out) const {
#ifndef CR_HAVE_ZLIB
    (void)e; (void)out;
    return false;
#else
    out.clear();
    SDL_IOStream* io = SDL_IOFromFile(path_.c_str(), "rb");
    if (!io) return false;
    bool ok = SDL_SeekIO(io, Sint64(e.offset), SDL_IO_SEEK_SET) >= 0;
    std::vector<uint8_t> chunk;
    for (uint32_t b = e.block; ok && out.size() < e.size; ++b) {
        if (b >= blockSizes_.size()) { ok = false; break; }
        const uint32_t z = blockSizes_[b] ? blockSizes_[b] : blockSize_;
        const size_t want = size_t(std::min<uint64_t>(blockSize_, e.size - out.size()));
        chunk.resize(z);
        if (SDL_ReadIO(io, chunk.data(), z) != z) { ok = false; break; }
        if (z == want) { out.insert(out.end(), chunk.begin(), chunk.end()); continue; } // stored
        const size_t at = out.size();
        out.resize(at + want);
        uLongf len = uLongf(want);
        if (uncompress(&out[at], &len, chunk.data(), z) != Z_OK || len != want) ok = false;
    }
    SDL_CloseIO(io);
    return ok && out.size() == e.size;
#endif
}

std::vector<std::string> PsarcArchive::names() const {
    std::vector<std::string> out;
    for (const auto& kv : byName_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

bool PsarcArchive::read(const std::string& name, std::vector<uint8_t>& out) const {
    const auto it = byName_.find(name);
    return it != byName_.end() && readEntry(entries_[it->second], out);
}

} // namespace cr
