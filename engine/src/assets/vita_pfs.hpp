#pragma once
// A PS Vita NoNpDrm dump's PFS image decrypted: the game data (app/<title>/) or one add-on
// (addcont/<title>/<add-on>/) - the read-only "unicv" images, files.db + unicv.db in sce_pfs/ -,
// with the klicensee of its sce_sys/package/work.bin (+0x50). Every directory and file files.db
// lists is written to the destination, decrypted (or copied, the ones kept in the clear), each
// sector checked against its unicv.db signature first; sce_pfs/ and sce_sys/package/ are not.
// The project's own code, written from a functional description of the format.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cr::vitapfs {

enum class Failure {
    None,
    Unsupported, // a PFS image of a kind this doesn't read (save data's, the add-ons' root, ...)
    Corrupt,     // files.db or unicv.db broken
    WrongKey,    // the klicensee isn't this image's (files.db's header doesn't check with it)
    Incomplete,  // a file or folder files.db lists isn't there, or its size isn't
    Damaged,     // a sector's signature doesn't match: the file damaged
    IoError,     // a file couldn't be read or written
};
const char* failureName(Failure f);

// (bytes done, bytes in all, the file going on: relative to the image's root, '/' separated)
using Progress = std::function<void(uint64_t done, uint64_t total, const std::string& path)>;

class Image {
public:
    // files.db and unicv.db read and checked against the key, every entry found on disk (its
    // first sector's signature checked too); nothing written
    Failure mount(const std::filesystem::path& root, const uint8_t klicensee[16]);
    // the image's tree written under `dest` (made if missing); a file it fails on is removed
    Failure decryptAll(const std::filesystem::path& dest, const Progress& progress = {});

    const std::string& detail() const { return detail_; } // (what failed, for the log)
    uint64_t totalBytes() const { return total_; }
    size_t fileCount() const;

private:
    struct Entry {
        uint32_t index = 0, parent = 0, size = 0;
        uint16_t attributes = 0;
        std::string name, path; // (path: files.db's, '/' separated, from the root)
        std::string disk;       // (the path as spelt on disk, UTF-8)
        bool directory = false, clear = false;
        uint32_t tableVersion = 0, sectors = 0;
        uint8_t seed[20] = {};
        size_t signaturesAt = 0; // (unicv.db: the offset of the first signature page)
    };
    Failure fail(Failure f, std::string why);
    Failure readTree(const std::vector<uint8_t>& db);
    Failure readTables(const std::vector<uint8_t>& unicv);
    Failure findOnDisk();
    Failure checkFirstSectors();
    Failure decryptFile(const Entry& e, const std::filesystem::path& dest, uint64_t& done, const Progress& progress);
    const uint8_t* signature(const Entry& e, uint32_t sector) const;

    std::filesystem::path root_;
    uint8_t key_[16] = {};       // (the data key: the klicensee unwrapped)
    uint8_t headerKey_[20] = {}; // (the integrity key of table salt 0)
    uint32_t imageSalt_ = 0, version_ = 0;
    std::vector<Entry> entries_;
    std::vector<uint8_t> unicv_;
    uint64_t total_ = 0;
    bool mounted_ = false;
    std::string detail_;
};

// ---- the pieces, for the tests
constexpr uint32_t kSector = 0x8000;
void dataKey(const uint8_t klicensee[16], uint8_t key[16]);
void integrityKey(const uint8_t key[16], uint32_t imageSalt, uint32_t tableSalt, uint8_t out[20]);
void ivMaskOfSalts(uint32_t imageSalt, uint32_t tableSalt, uint8_t mask[16]); // (a version-1 table's)
void ivMaskOfSeed(const uint8_t seed[20], uint8_t mask[16]);                  // (a version-2 table's)
void sectorKey(const uint8_t integrity[20], uint32_t sector, uint8_t out[20]);
void sectorSignature(const uint8_t integrity[20], uint32_t sector, const uint8_t* stored, size_t size, uint8_t out[20]);
void sectorIv(const uint8_t mask[16], uint64_t sector, uint8_t iv[16]);
void decryptSector(const uint8_t key[16], const uint8_t mask[16], uint64_t sector, uint8_t* data, size_t size);

} // namespace cr::vitapfs
