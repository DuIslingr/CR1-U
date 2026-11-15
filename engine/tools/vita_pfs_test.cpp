// The Vita PFS decrypter's keys and sectors against known vectors (assets/vita_pfs). With
// CR_VITA_DUMP set to a NoNpDrm image's folder (app/<id>/ or addcont/<id>/<add-on>/), it is also
// mounted with its own work.bin's key, and with that key spoilt (the wrong-key failure); with
// CR_VITA_DUMP_OUT too, decrypted into that folder.
#include "assets/vita_pfs.hpp"
#include "crypto/crypto.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace cr;

namespace {

int problems = 0;

std::vector<uint8_t> hex(const char* h) {
    std::vector<uint8_t> out;
    for (; h[0] && h[1]; h += 2) out.push_back(uint8_t(std::stoi(std::string(h, 2), nullptr, 16)));
    return out;
}
void same(const uint8_t* got, const char* want, const char* what) {
    const std::vector<uint8_t> w = hex(want);
    if (std::memcmp(got, w.data(), w.size()) != 0) {
        ++problems;
        std::fprintf(stderr, "vita pfs FAILED: %s\n  got  ", what);
        for (size_t i = 0; i < w.size(); ++i) std::fprintf(stderr, "%02x", got[i]);
        std::fprintf(stderr, "\n  want %s\n", want);
    }
}

} // namespace

int main() {
    // the primitives the setup leans on
    {
        uint8_t d[32];
        crypto::sha1(reinterpret_cast<const uint8_t*>("abc"), 3, d);
        same(d, "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1(abc)");
        crypto::sha256(reinterpret_cast<const uint8_t*>("abc"), 3, d);
        same(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256(abc)");
        const std::vector<uint8_t> k = hex("000102030405060708090a0b0c0d0e0f"), p = hex("00112233445566778899aabbccddeeff");
        crypto::aesEcbEncrypt(p.data(), d, 16, k.data());
        same(d, "69c4e0d86a7b0430d8cdb78070b4c55a", "AES-128 (FIPS-197 C.1)");
    }

    // the keys (the spec's vectors: klicensee 00 01 .. 0F)
    uint8_t klic[16], key[16];
    for (int i = 0; i < 16; ++i) klic[i] = uint8_t(i);
    vitapfs::dataKey(klic, key);
    same(key, "c1ccac771659b49a08604278c56ef5b6", "data key");
    struct Salts {
        uint32_t image, table;
        const char *ik, *mask;
    } const salts[] = {
        {0, 0, "13d8be08a6341c7ff25eb881242c1653da0495e0", "a2e10193ff67a6d7576865687ffc753c"},
        {0, 0x0B, "a014191661d8adbc8ec01f52bb35c907059faa6a", "79aa1e9df9db3f789aedf3b5009f0749"},
        {0xF4E207AC, 0, "055a95af17c26b388ee3c8cf7c3971b0e7e3ede4", "fcdb39a6efc22f3ebff4a0907c7cc29b"},
        {0xF4E207AC, 0x87F, "3ef8df54f36fdef4d47c355eaa9e32bcdfc2abf0", "3d46bac2bb6a40d163a31e57ac659ea6"},
    };
    for (const Salts& s : salts) {
        char what[96];
        uint8_t ik[20], mask[16];
        vitapfs::integrityKey(key, s.image, s.table, ik);
        std::snprintf(what, sizeof what, "integrity key (image salt %08x, table salt %x)", s.image, s.table);
        same(ik, s.ik, what);
        vitapfs::ivMaskOfSalts(s.image, s.table, mask);
        std::snprintf(what, sizeof what, "IV mask, version 1 (image salt %08x, table salt %x)", s.image, s.table);
        same(mask, s.mask, what);
    }
    {
        uint8_t seed[20], mask[16];
        std::memset(seed, 0x11, 20);
        vitapfs::ivMaskOfSeed(seed, mask);
        same(mask, "22c8323d4e7621b1b0a3daffb200e20b", "IV mask, version 2 (seed 11 x 20)");
    }

    // a sector: image salt 0, table salt 0x0B, a version-1 table, sector 3 - 2 blocks and a 5-byte residual
    {
        uint8_t ik[20], mask[16], iv[16], sk[20], sig[20];
        vitapfs::integrityKey(key, 0, 0x0B, ik);
        vitapfs::ivMaskOfSalts(0, 0x0B, mask);
        vitapfs::sectorIv(mask, 3, iv);
        same(iv, "792a1f9df9db3f789aedf3b5009f0749", "sector 3's IV");
        vitapfs::sectorKey(ik, 3, sk);
        same(sk, "d2d67b64cfe20845b87715dc367dc73f686c3d1e", "sector 3's key");
        std::vector<uint8_t> data = hex("8f339ec2ff28af72c36610088520b3169d3f43801fac4d5e5f0761016c6d9e6c68b4e4eb34");
        vitapfs::sectorSignature(ik, 3, data.data(), data.size(), sig);
        same(sig, "7e6f056863dbfba13c00e5a6cdf511fed4866ca5", "sector 3's signature");
        vitapfs::decryptSector(key, mask, 3, data.data(), data.size());
        std::vector<uint8_t> plain(37);
        for (size_t i = 0; i < plain.size(); ++i) plain[i] = uint8_t(i);
        if (data != plain) ++problems, std::fprintf(stderr, "vita pfs FAILED: sector 3 doesn't decrypt to 00 01 .. 24\n");
        // (shorter than a block: the residual against the IV itself)
        std::vector<uint8_t> tiny = {1, 2, 3, 4, 5};
        uint8_t pad[16];
        crypto::aesEcbEncrypt(iv, pad, 16, key);
        vitapfs::decryptSector(key, mask, 3, tiny.data(), tiny.size());
        for (size_t i = 0; i < tiny.size(); ++i)
            if (tiny[i] != uint8_t((i + 1) ^ pad[i])) ++problems, std::fprintf(stderr, "vita pfs FAILED: a sector under 16 bytes\n");
    }

    // a real image, when one is given
    if (const char* dump = std::getenv("CR_VITA_DUMP"); dump && *dump) {
        const std::filesystem::path root = std::filesystem::u8path(dump);
        uint8_t own[16] = {};
        std::ifstream w(root / "sce_sys" / "package" / "work.bin", std::ios::binary);
        if (!w.seekg(0x50) || !w.read(reinterpret_cast<char*>(own), 16)) {
            ++problems, std::fprintf(stderr, "vita pfs FAILED: %s has no work.bin\n", dump);
        } else {
            vitapfs::Image image;
            const vitapfs::Failure f = image.mount(root, own);
            if (f != vitapfs::Failure::None) ++problems, std::fprintf(stderr, "vita pfs FAILED: mounting %s: %s - %s\n", dump, vitapfs::failureName(f), image.detail().c_str());
            else {
                std::printf("%s: %zu files, %llu bytes\n", dump, image.fileCount(), static_cast<unsigned long long>(image.totalBytes()));
                if (const char* to = std::getenv("CR_VITA_DUMP_OUT"); to && *to) { // (and decrypted there)
                    const vitapfs::Failure d = image.decryptAll(std::filesystem::u8path(to));
                    if (d != vitapfs::Failure::None) ++problems, std::fprintf(stderr, "vita pfs FAILED: decrypting: %s - %s\n", vitapfs::failureName(d), image.detail().c_str());
                }
            }
            own[5] ^= 0x40;
            const vitapfs::Failure g = image.mount(root, own);
            if (g != vitapfs::Failure::WrongKey) ++problems, std::fprintf(stderr, "vita pfs FAILED: a spoilt key gives \"%s\", not a wrong key\n", vitapfs::failureName(g));
        }
    }

    if (problems) return std::fprintf(stderr, "vita pfs: %d FAILED\n", problems), 1;
    std::printf("vita pfs: all passed\n");
    return 0;
}
