#include "assets/vita_pfs.hpp"

#include "crypto/crypto.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>

namespace cr::vitapfs {

namespace fs = std::filesystem;

namespace {

constexpr size_t kPage = 0x400;
constexpr uint32_t kGroup = 50; // (sectors per signature page)
constexpr size_t kHash = 20;

// the klicensee's unwrapping key (the console's key generator, offline: one AES block)
constexpr uint8_t kUnwrap[16] = {0xE1, 0x22, 0x13, 0xB4, 0x80, 0x16, 0xB0, 0xE9, 0x9A, 0xB8, 0x1F, 0x8E, 0xC0, 0x2A, 0xD4, 0xA2};
// HMAC-SHA1 keys: the files' IV masks, the integrity keys' material; that one's CBC IV
constexpr uint8_t kMaskKey[20] = {0xE4, 0x62, 0x25, 0x8B, 0x1F, 0x31, 0x21, 0x56, 0x07, 0x45,
                                  0xDB, 0x62, 0xB1, 0x43, 0x67, 0x23, 0xD2, 0xBF, 0x80, 0xFE};
constexpr uint8_t kIntegrityKey[20] = {0xAF, 0xE6, 0x56, 0xBB, 0x3C, 0x17, 0x25, 0x6A, 0x3C, 0x80,
                                       0x9F, 0x6E, 0x9B, 0xF1, 0x9F, 0xDD, 0x5A, 0x38, 0x85, 0x43};
constexpr uint8_t kIntegrityIv[16] = {0x74, 0xD2, 0x0C, 0xC3, 0x98, 0x81, 0xC2, 0x13, 0xEE, 0x77, 0x0B, 0x10, 0x10, 0xE4, 0xBE, 0xA7};

// files.db's attributes
constexpr uint16_t kDirectory = 0x8000, kClear = 0x4000, kNoIntegrity = 0x2000;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
bool zero(const uint8_t* p, size_t n) {
    return std::all_of(p, p + n, [](uint8_t b) { return b == 0; });
}

// the salt message: the table salt, after the image's when it has one
size_t saltMessage(uint32_t imageSalt, uint32_t tableSalt, uint8_t msg[8]) {
    if (imageSalt == 0) return put32(msg, tableSalt), 4;
    put32(msg, imageSalt), put32(msg + 4, tableSalt);
    return 8;
}

// the residual rule: a length's last partial block XORed with AES(key, the last ciphertext block)
void residual(const uint8_t key[16], const uint8_t last[16], uint8_t* tail, size_t n) {
    uint8_t pad[16];
    crypto::aesEcbEncrypt(last, pad, 16, key);
    for (size_t i = 0; i < n; ++i) tail[i] ^= pad[i];
}

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}
std::string utf8(const fs::path& p) {
    const auto u = p.generic_u8string(); // (std::string, or C++20's std::u8string)
    return std::string(u.begin(), u.end());
}
fs::path pathOf(const std::string& u) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(u.begin(), u.end()));
#else
    return fs::u8path(u);
#endif
}

bool readWhole(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n < 0) return false;
    in.seekg(0);
    out.resize(size_t(n));
    return n == 0 || bool(in.read(reinterpret_cast<char*>(out.data()), n));
}

// ---- files.db's pages: B-tree nodes
struct Node {
    const uint8_t* p;
    uint32_t parent() const { return le32(p); }
    uint32_t kind() const { return le32(p + 4); } // (0 a leaf, 1 internal)
    uint32_t count() const { return le32(p + 8); }
    uint32_t keyParent(int i) const { return le32(p + 0x10 + 0x48 * i); }
    std::string keyName(int i) const {
        const char* n = reinterpret_cast<const char*>(p + 0x14 + 0x48 * i);
        return std::string(n, strnlen(n, 0x44));
    }
    uint32_t index(int i) const { return le32(p + 0x298 + 0x10 * i); }
    uint16_t attributes(int i) const { return le16(p + 0x298 + 0x10 * i + 4); }
    uint32_t size(int i) const { return le32(p + 0x298 + 0x10 * i + 8); }
    const uint8_t* hash(int i) const { return p + 0x338 + kHash * i; }
};

// a page's integrity value: version 5 over the page past its parent's number; 3 and 4 chained over
// its own hash slots (a leaf's keys', an internal node's children's)
void pageIcv(const uint8_t ik[20], uint32_t version, const Node& n, uint8_t out[20]) {
    if (version >= 5) return crypto::hmacSha1(n.p + 4, kPage - 4, ik, 20, out);
    std::memset(out, 0, 20);
    const uint32_t m = n.kind() == 0 ? n.count() : n.count() + 1;
    for (uint32_t i = 0; i < m; ++i) {
        uint8_t msg[40];
        std::memcpy(msg, out, 20);
        std::memcpy(msg + 20, n.hash(int(i)), 20);
        crypto::hmacSha1(msg, 40, ik, 20, out);
    }
}

} // namespace

// ---- the keys

void dataKey(const uint8_t klicensee[16], uint8_t key[16]) { crypto::aesEcbDecrypt(klicensee, key, 16, kUnwrap); }

void integrityKey(const uint8_t key[16], uint32_t imageSalt, uint32_t tableSalt, uint8_t out[20]) {
    uint8_t msg[8], pre[20], iv[16];
    crypto::hmacSha1(msg, saltMessage(imageSalt, tableSalt, msg), kIntegrityKey, 20, pre);
    std::memcpy(iv, kIntegrityIv, 16);
    crypto::aesCbcEncrypt(pre, out, 16, key, 128, iv); // (iv: now that block, the residual's)
    std::memcpy(out + 16, pre + 16, 4);
    residual(key, iv, out + 16, 4);
}

void ivMaskOfSalts(uint32_t imageSalt, uint32_t tableSalt, uint8_t mask[16]) {
    uint8_t msg[8], h[20];
    crypto::hmacSha1(msg, saltMessage(imageSalt, tableSalt, msg), kMaskKey, 20, h);
    std::memcpy(mask, h, 16);
}

void ivMaskOfSeed(const uint8_t seed[20], uint8_t mask[16]) {
    uint8_t h[20];
    crypto::hmacSha1(seed, 20, kMaskKey, 20, h);
    std::memcpy(mask, h, 16);
}

void sectorKey(const uint8_t integrity[20], uint32_t sector, uint8_t out[20]) {
    uint8_t s[4];
    put32(s, sector);
    crypto::hmacSha1(s, 4, integrity, 20, out);
}

void sectorSignature(const uint8_t integrity[20], uint32_t sector, const uint8_t* stored, size_t size, uint8_t out[20]) {
    uint8_t k[20];
    sectorKey(integrity, sector, k);
    crypto::hmacSha1(stored, size, k, 20, out);
}

void sectorIv(const uint8_t mask[16], uint64_t sector, uint8_t iv[16]) {
    const uint64_t at = sector * kSector;
    for (int i = 0; i < 16; ++i) iv[i] = uint8_t((i < 8 ? uint8_t(at >> (8 * i)) : 0) ^ mask[i]);
}

void decryptSector(const uint8_t key[16], const uint8_t mask[16], uint64_t sector, uint8_t* data, size_t size) {
    uint8_t iv[16];
    sectorIv(mask, sector, iv);
    const size_t whole = size / 16 * 16;
    crypto::aesCbcDecrypt(data, data, whole, key, 128, iv); // (iv: the last ciphertext block, or the IV)
    residual(key, iv, data + whole, size - whole);
}

const char* failureName(Failure f) {
    switch (f) {
    case Failure::None: return "none";
    case Failure::Unsupported: return "unsupported image";
    case Failure::Corrupt: return "corrupt";
    case Failure::WrongKey: return "wrong key";
    case Failure::Incomplete: return "incomplete";
    case Failure::Damaged: return "damaged";
    case Failure::IoError: return "I/O error";
    }
    return "?";
}

// ---- the image

Failure Image::fail(Failure f, std::string why) {
    detail_ = std::move(why);
    return f;
}

size_t Image::fileCount() const {
    return size_t(std::count_if(entries_.begin(), entries_.end(), [](const Entry& e) { return !e.directory; }));
}

Failure Image::mount(const fs::path& root, const uint8_t klicensee[16]) {
    mounted_ = false;
    root_ = root;
    entries_.clear(), unicv_.clear(), total_ = 0, detail_.clear();
    std::error_code ec;
    const fs::path pfs = root / "sce_pfs";
    std::vector<uint8_t> db;
    if (!readWhole(pfs / "files.db", db)) return fail(Failure::Incomplete, "no sce_pfs/files.db");

    // the header: its structure
    if (db.size() < 2 * kPage || db.size() % kPage || std::memcmp(db.data(), "SCENGPFS", 8) != 0) return fail(Failure::Corrupt, "files.db isn't one");
    const uint8_t* h = db.data();
    version_ = le32(h + 8);
    const uint16_t imageClass = le16(h + 0xC);
    if (version_ < 3 || version_ > 5) return fail(Failure::Unsupported, "files.db version " + std::to_string(version_));
    if (imageClass != 1 && imageClass != 4) return fail(Failure::Unsupported, "image class " + std::to_string(imageClass) + " (not game data or an add-on)");
    if (fs::is_directory(pfs / "icv.db", ec)) return fail(Failure::Unsupported, "a read-write (icv.db) image");
    const uint32_t pages = uint32_t(db.size() / kPage - 1);
    if (le16(h + 0xE) != 0 || le32(h + 0x10) != kPage || le32(h + 0x14) != 10 || le32(h + 0x18) >= pages || le64(h + 0x28) != db.size() - kPage ||
        !zero(h + 0x260, 0x1A0))
        return fail(Failure::Corrupt, "files.db's header");
    imageSalt_ = le32(h + 0x1C);

    // the key: files.db's header checked with it
    if (zero(klicensee, 16)) return fail(Failure::WrongKey, "work.bin's klicensee is all zero");
    dataKey(klicensee, key_);
    integrityKey(key_, imageSalt_, 0, headerKey_);
    {
        uint8_t copy[0x160], icv[20];
        std::memcpy(copy, h, sizeof copy);
        std::memset(copy + 0x4C, 0, 0x160 - 0x4C);
        crypto::hmacSha1(copy, sizeof copy, headerKey_, 20, icv);
        if (std::memcmp(icv, h + 0x4C, 20) != 0) return fail(Failure::WrongKey, "the licence doesn't match (files.db's header doesn't check with its klicensee)");
    }
    if (Failure f = readTree(db); f != Failure::None) return f;

    std::vector<uint8_t> unicv;
    if (!readWhole(pfs / "unicv.db", unicv)) return fail(Failure::Incomplete, "no sce_pfs/unicv.db");
    if (Failure f = readTables(unicv); f != Failure::None) return f;
    unicv_ = std::move(unicv);
    if (Failure f = findOnDisk(); f != Failure::None) return f;
    if (Failure f = checkFirstSectors(); f != Failure::None) return f;
    for (const Entry& e : entries_) total_ += e.directory ? 0 : e.size;
    mounted_ = true;
    return Failure::None;
}

// files.db's B-tree: every page reached once from the root, each checked against the hash its parent
// keeps of it (the root against the header's); the entries from its leaves; their paths
Failure Image::readTree(const std::vector<uint8_t>& db) {
    const uint32_t pages = uint32_t(db.size() / kPage - 1), rootPage = le32(db.data() + 0x18);
    auto node = [&](uint32_t p) { return Node{db.data() + kPage * (1 + size_t(p))}; };
    std::vector<uint8_t> reached(pages, 0);
    std::vector<uint32_t> todo = {rootPage};
    reached[rootPage] = 1;
    {
        uint8_t icv[20];
        pageIcv(headerKey_, version_, node(rootPage), icv);
        if (node(rootPage).parent() != 0xFFFFFFFF || std::memcmp(icv, db.data() + 0x38, 20) != 0) return fail(Failure::Corrupt, "files.db's root page");
    }
    std::vector<Entry> found;
    while (!todo.empty()) {
        const uint32_t p = todo.back();
        todo.pop_back();
        const Node n = node(p);
        const uint32_t count = n.count();
        if (n.kind() > 1 || count > 9 || le32(n.p + 0xC) != 0) return fail(Failure::Corrupt, "files.db page " + std::to_string(p));
        if (n.kind() == 1) {
            for (uint32_t j = 0; j <= count; ++j) {
                const uint32_t c = n.index(int(j));
                uint8_t icv[20];
                if (c >= pages || reached[c] || node(c).parent() != p) return fail(Failure::Corrupt, "files.db page " + std::to_string(p) + "'s children");
                pageIcv(headerKey_, version_, node(c), icv);
                if (std::memcmp(icv, n.hash(int(j)), 20) != 0) return fail(Failure::Corrupt, "files.db page " + std::to_string(c) + " doesn't check");
                reached[c] = 1;
                todo.push_back(c);
            }
            continue;
        }
        for (uint32_t i = 0; i < count; ++i) {
            Entry e;
            e.parent = n.keyParent(int(i));
            e.name = n.keyName(int(i));
            e.index = n.index(int(i));
            e.attributes = n.attributes(int(i));
            e.size = n.size(int(i));
            if (e.attributes == 0) {
                if (e.size) return fail(Failure::Unsupported, "an entry without attributes (" + e.name + ")");
                continue;
            }
            if (e.attributes & kNoIntegrity) return fail(Failure::Unsupported, "an entry without integrity values (" + e.name + ")");
            if (e.name.empty() || e.name == "." || e.name == ".." || e.name.find_first_of("/\\") != std::string::npos || e.index == 0)
                return fail(Failure::Corrupt, "files.db's entry \"" + e.name + "\"");
            e.directory = (e.attributes & kDirectory) != 0;
            e.clear = !e.directory && (e.attributes & kClear);
            found.push_back(std::move(e));
        }
    }
    if (std::find(reached.begin(), reached.end(), 0) != reached.end()) return fail(Failure::Corrupt, "files.db has pages its tree doesn't reach");

    // the paths: each directory's index its (parent, name)
    std::map<uint32_t, size_t> byIndex;
    for (size_t k = 0; k < found.size(); ++k)
        if (!byIndex.emplace(found[k].index, k).second) return fail(Failure::Corrupt, "files.db's index " + std::to_string(found[k].index) + " twice");
    for (Entry& e : found) {
        std::string path = e.name;
        uint32_t up = e.parent;
        for (size_t depth = 0; up != 0; ++depth) {
            const auto it = byIndex.find(up);
            if (it == byIndex.end() || !found[it->second].directory || depth > found.size())
                return fail(Failure::Corrupt, "files.db's entry \"" + e.name + "\" outside the tree");
            path = found[it->second].name + "/" + path;
            up = found[it->second].parent;
        }
        e.path = std::move(path);
    }
    std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) { return a.index < b.index; });
    entries_ = std::move(found);
    return Failure::None;
}

// unicv.db: each entry's table - its header page at index - 1, its signature pages after it
Failure Image::readTables(const std::vector<uint8_t>& u) {
    if (u.size() < kPage || u.size() % kPage || std::memcmp(u.data(), "SCEIRODB", 8) != 0) return fail(Failure::Corrupt, "unicv.db isn't one");
    const uint32_t v = le32(u.data() + 8);
    if (v != 1 && v != 2) return fail(Failure::Unsupported, "unicv.db version " + std::to_string(v));
    if (le32(u.data() + 0xC) != kPage || le32(u.data() + 0x10) != 0xFFFFFFFF || le32(u.data() + 0x14) != 0xFFFFFFFF ||
        le64(u.data() + 0x18) != u.size() - kPage || !zero(u.data() + 0x20, kPage - 0x20))
        return fail(Failure::Corrupt, "unicv.db's header");
    const size_t pages = u.size() / kPage - 1;
    for (Entry& e : entries_) {
        const size_t q = size_t(e.index) - 1;
        const std::string what = "unicv.db's table of " + e.path;
        if (q >= pages) return fail(Failure::Corrupt, what + ": missing");
        const uint8_t* t = u.data() + kPage * (1 + q);
        const uint32_t tv = le32(t + 8), sectors = le32(t + 0x14);
        if (std::memcmp(t, "SCEIFTBL", 8) != 0 || (tv != 1 && tv != 2) || le32(t + 0xC) != kPage || le32(t + 0x10) != kGroup ||
            le32(t + 0x18) != kSector || le32(t + 0x1C) != 0 || !zero(t + 0x48, kPage - 0x48))
            return fail(Failure::Corrupt, what);
        const uint32_t want = e.directory ? 0 : uint32_t((uint64_t(e.size) + kSector - 1) / kSector);
        if (sectors != want) return fail(Failure::Corrupt, what + ": its sectors aren't its size's");
        const size_t groups = (sectors + kGroup - 1) / kGroup;
        if (q + groups >= pages) return fail(Failure::Corrupt, what + ": past unicv.db's end");
        for (size_t g = 0; g < groups; ++g) {
            const uint8_t* s = t + kPage * (1 + g);
            const uint32_t c = g + 1 < groups ? kGroup : sectors - kGroup * uint32_t(g);
            if (le32(s) != 0x10 + kGroup * kHash || le32(s + 4) != kHash || le32(s + 8) != c || le32(s + 0xC) != 0) return fail(Failure::Corrupt, what + "'s signatures");
        }
        e.tableVersion = tv, e.sectors = sectors;
        std::memcpy(e.seed, t + 0x34, 20);
        e.signaturesAt = kPage * (2 + q);
    }
    return Failure::None;
}

const uint8_t* Image::signature(const Entry& e, uint32_t sector) const {
    return unicv_.data() + e.signaturesAt + kPage * (sector / kGroup) + 0x10 + kHash * (sector % kGroup);
}

// each entry's file or folder on disk, matched without regard to case (sce_pfs/, sce_sys/package/ not)
Failure Image::findOnDisk() {
    struct OnDisk {
        std::string path;
        bool directory;
        uint64_t size;
        bool ambiguous;
    };
    std::map<std::string, OnDisk> disk;
    std::error_code ec;
    fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return fail(Failure::IoError, "the dump's folder can't be read");
    for (; it != end; it.increment(ec)) {
        if (ec) return fail(Failure::IoError, "the dump's folder can't be read");
        const std::string rel = utf8(it->path().lexically_relative(root_)), key = upper(rel);
        const bool dir = it->is_directory(ec);
        if (key == "SCE_PFS" || key == "SCE_SYS/PACKAGE") {
            if (dir) it.disable_recursion_pending();
            continue;
        }
        const uint64_t size = dir ? 0 : uint64_t(it->file_size(ec));
        const auto [at, fresh] = disk.emplace(key, OnDisk{rel, dir, size, false});
        if (!fresh) at->second.ambiguous = true;
    }
    for (Entry& e : entries_) {
        const auto at = disk.find(upper(e.path));
        if (at == disk.end() || at->second.directory != e.directory) return fail(Failure::Incomplete, e.path + " missing");
        if (at->second.ambiguous) return fail(Failure::Corrupt, e.path + " is on disk twice, in different cases");
        if (!e.directory && at->second.size != e.size) return fail(Failure::Incomplete, e.path + " isn't its size");
        e.disk = at->second.path;
    }
    return Failure::None;
}

// each file's first sector against its signature: the key, the file, its table, all at once
Failure Image::checkFirstSectors() {
    std::vector<uint8_t> buf(kSector);
    for (const Entry& e : entries_) {
        if (e.directory || e.sectors == 0) continue;
        const size_t n = std::min<size_t>(e.size, kSector);
        std::ifstream in(root_ / pathOf(e.disk), std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(n))) return fail(Failure::IoError, e.path + " can't be read");
        uint8_t ik[20], sig[20];
        integrityKey(key_, imageSalt_, e.index, ik);
        sectorSignature(ik, 0, buf.data(), n, sig);
        if (std::memcmp(sig, signature(e, 0), 20) != 0) return fail(Failure::Damaged, e.path + "'s first sector doesn't match its signature");
    }
    return Failure::None;
}

Failure Image::decryptAll(const fs::path& dest, const Progress& progress) {
    if (!mounted_) return fail(Failure::IoError, "not mounted");
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (!fs::is_directory(dest, ec)) return fail(Failure::IoError, "the destination can't be made");
    for (const Entry& e : entries_) {
        if (!e.directory) continue;
        const fs::path d = dest / pathOf(e.disk);
        fs::create_directories(d, ec);
        if (!fs::is_directory(d, ec)) return fail(Failure::IoError, e.path + " can't be made");
    }
    uint64_t done = 0;
    for (const Entry& e : entries_) {
        if (e.directory) continue;
        if (progress) progress(done, total_, e.disk);
        const fs::path out = dest / pathOf(e.disk);
        if (Failure f = decryptFile(e, out, done, progress); f != Failure::None) {
            fs::remove(out, ec);
            return f;
        }
        if (progress) progress(done, total_, e.disk);
    }
    return Failure::None;
}

// a file's sectors, 50 at a time (a signature page's): each checked as stored, then decrypted
Failure Image::decryptFile(const Entry& e, const fs::path& to, uint64_t& done, const Progress& progress) {
    std::ifstream in(root_ / pathOf(e.disk), std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in) return fail(Failure::IoError, e.path + " can't be read");
    if (!out) return fail(Failure::IoError, e.path + " can't be written");
    uint8_t ik[20], mask[16];
    integrityKey(key_, imageSalt_, e.index, ik);
    if (e.tableVersion >= 2) ivMaskOfSeed(e.seed, mask);
    else ivMaskOfSalts(imageSalt_, e.index, mask);
    std::vector<uint8_t> buf(size_t(kGroup) * kSector);
    for (uint32_t first = 0; first < e.sectors; first += kGroup) {
        const uint64_t at = uint64_t(first) * kSector;
        const size_t n = size_t(std::min<uint64_t>(uint64_t(kGroup) * kSector, e.size - at));
        if (!in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(n))) return fail(Failure::IoError, e.path + " can't be read");
        for (size_t off = 0; off < n; off += kSector) {
            const uint32_t s = first + uint32_t(off / kSector);
            const size_t len = std::min<size_t>(kSector, n - off);
            uint8_t sig[20];
            sectorSignature(ik, s, buf.data() + off, len, sig);
            if (std::memcmp(sig, signature(e, s), 20) != 0) return fail(Failure::Damaged, e.path + ": sector " + std::to_string(s) + " doesn't match its signature");
            if (!e.clear) decryptSector(key_, mask, s, buf.data() + off, len);
        }
        if (!out.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(n))) return fail(Failure::IoError, e.path + " can't be written");
        done += n;
        if (progress) progress(done, total_, e.disk);
    }
    out.close();
    if (!out) return fail(Failure::IoError, e.path + " can't be written");
    return Failure::None;
}

} // namespace cr::vitapfs
