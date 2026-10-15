#pragma once
// PSARC archives (the Vita release's cr1.psarc / *.psarc): read-only access by file name.
// Header (big-endian): "PSAR", version, compression ("zlib"), TOC length, entry size (30), entry
// count, block size, flags; entries {md5[16], u32 first block, u40 size, u40 offset}; then one
// compressed-size per block (2 bytes for 64 KiB blocks). Entry 0 is the manifest: names, one per
// line. Needs zlib (CR_HAVE_ZLIB); without it open() fails.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cr {

class PsarcArchive {
public:
    bool open(const std::string& path, std::string* error = nullptr);
    bool isOpen() const { return !entries_.empty(); }
    // A file by its base name (the manifest's last path component), e.g. "window_hd.gxt"
    bool read(const std::string& name, std::vector<uint8_t>& out) const;
    bool contains(const std::string& name) const { return byName_.count(name) != 0; }
    size_t size() const { return byName_.size(); }
    std::vector<std::string> names() const; // base names, sorted

private:
    struct Entry { uint32_t block; uint64_t size, offset; };
    bool readEntry(const Entry& e, std::vector<uint8_t>& out) const;
    std::string path_;
    uint32_t blockSize_ = 65536;
    std::vector<Entry> entries_;
    std::vector<uint32_t> blockSizes_;
    std::unordered_map<std::string, size_t> byName_;
};

} // namespace cr
