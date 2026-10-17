#pragma once
// The Android release's data: its Google Play expansion file (main.<version>.<package>.obb), the
// game's files one after another with no table of its own. The table is in the APK's classes.dex:
// a Java class lists the file names in order (a String[] filled one const-string at a time) and two
// int[] arrays of the same length (fill-array-data payloads) give each file's size and CRC-16; a
// file's offset is the sizes before it added up (docs/ANDROID.md). ObbArchive reads that table
// out of your own APK (or an unpacked classes.dex) and the files out of your own OBB.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cr {

class ObbArchive {
public:
    // `apk`: the APK (a zip; its classes.dex is read) or a classes.dex itself
    bool open(const std::string& obb, const std::string& apk, std::string* error = nullptr);
    bool isOpen() const { return !entries_.empty(); }
    bool read(const std::string& name, std::vector<uint8_t>& out) const;
    bool contains(const std::string& name) const { return byName_.count(lower(name)) != 0; }
    size_t size() const { return entries_.size(); }
    std::vector<std::string> names() const; // in the OBB's order
    // where a file lies in the OBB (path()): its offset and size (for streaming it)
    bool locate(const std::string& name, uint64_t& offset, uint32_t& size) const;
    const std::string& path() const { return path_; }

    // The table alone, from a classes.dex: names in order, sizes, CRCs (all the same length);
    // `total` the OBB's size the sizes must add up to (0: not checked). For tools and tests
    static bool readTable(const std::vector<uint8_t>& dex, uint64_t total, std::vector<std::string>& names,
                          std::vector<uint32_t>& sizes, std::vector<uint32_t>& crcs, std::string* error = nullptr);
    // classes.dex out of an APK (zip: stored or deflated)
    static bool readDexFromApk(const std::string& apk, std::vector<uint8_t>& dex, std::string* error = nullptr);

private:
    struct Entry { std::string name; uint64_t offset = 0; uint32_t size = 0; };
    static std::string lower(const std::string& s);
    std::string path_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, size_t> byName_;
};

} // namespace cr
