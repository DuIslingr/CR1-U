#include "game/trophies.hpp"

#include "assets/texture.hpp"
#include "core/file.hpp"
#include "stb/stb_image.h"

#include <cstdlib>
#include <cstring>

namespace cr::trophies {

bool decodeIcon(const Info& info, Image& out) {
    if (info.icon.empty() || info.icon.size() > (size_t(1) << 30)) return false;
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load_from_memory(info.icon.data(), int(info.icon.size()), &w, &h, &ch, 4);
    if (!px) return false;
    out = Image();
    out.width = w, out.height = h;
    out.mipmaps = true; // (drawn at a fraction of its size)
    out.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    return w > 0 && h > 0;
}

std::string Flags::hex() const {
    static const char* const kDigits = "0123456789abcdef";
    std::string s;
    for (uint8_t b : bits) s += kDigits[b >> 4], s += kDigits[b & 15];
    return s;
}

bool Flags::fromHex(const std::string& s) {
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    if (s.size() < bits.size() * 2) return false;
    std::array<uint8_t, 8> b{};
    for (size_t i = 0; i < b.size(); ++i) {
        const int hi = digit(s[2 * i]), lo = digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        b[i] = uint8_t(hi << 4 | lo);
    }
    bits = b;
    return true;
}

std::vector<int> check(const GameState& gs, Flags& f) {
    std::vector<int> out;
    auto unlock = [&](int n) { out.push_back(n), f.set(n); }; // FUN_8106add4: queued, its bit set at once
    // Each pair's story keeps its flags 400 (0x190) apart: any pair's flag counts
    auto anyPair = [&](int flag) {
        for (int p = 0; p < 4; ++p)
            if (gs.flag(flag + 0x190 * p)) return true;
        return false;
    };
    // 1..8: the eight gate bosses (A Dryg Goch, Cerberus, Gnomus, Airavata, Ondine, Old One, Ra, the
    // Prototype Agent - TROP.SFM's names), 9..12: Escher's / Zhamo's / Olgar's / Ayuta's own story
    // through the primal door, 13: Qualia, 14: the businessman (Piu-Piu), 15: the first purchase
    static const int kBoss[8] = {0x462, 0x463, 0x476, 0x477, 0x48a, 0x48b, 0x49e, 0x49f};
    for (int i = 0; i < 8; ++i)
        if (anyPair(kBoss[i]) && !f.has(1 + i)) unlock(1 + i);
    for (int p = 0; p < 4; ++p)
        if (gs.flag(0x455 + 0x190 * p) && !f.has(9 + p)) unlock(9 + p);
    if (anyPair(0x4b9) && !f.has(13)) unlock(13);
    if (anyPair(0x425) && !f.has(14)) unlock(14);
    if (f.has(kBought) && !f.has(15)) unlock(15);
    if (!f.has(0)) { // all fifteen: trophy 0 (Gold)
        bool all = true;
        for (int n = 1; n < kCount && all; ++n) all = f.has(n);
        if (all) unlock(0);
    }
    return out;
}

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

std::string unescape(std::string s) {
    static const char* const kFrom[] = {"&lt;", "&gt;", "&quot;", "&apos;", "&amp;"};
    static const char* const kTo[] = {"<", ">", "\"", "'", "&"};
    for (int k = 0; k < 5; ++k)
        for (size_t at = 0; (at = s.find(kFrom[k], at)) != std::string::npos; at += std::strlen(kTo[k]))
            s.replace(at, std::strlen(kFrom[k]), kTo[k]);
    return s;
}

std::string between(const std::string& s, size_t from, size_t end, const char* open, const char* close) {
    const size_t a = s.find(open, from);
    if (a == std::string::npos || a >= end) return {};
    const size_t b = s.find(close, a);
    if (b == std::string::npos || b > end) return {};
    return unescape(s.substr(a + std::strlen(open), b - a - std::strlen(open)));
}

} // namespace

bool readPack(const std::string& path, std::array<Info, kCount>& out) {
    std::vector<uint8_t> d;
    if (!readFile(path, d) || d.size() < 0x40 || be32(d.data()) != 0xdca24d00u) return false;
    const uint32_t count = be32(&d[0x10]), size = be32(&d[0x14]);
    if (size < 0x30 || count > 256 || 0x40 + size_t(count) * size > d.size()) return false;
    bool named = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = &d[0x40 + size_t(i) * size];
        const std::string name(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 32));
        const uint64_t off = be64(e + 0x20), len = be64(e + 0x28);
        if (off > d.size() || len > d.size() - off) return false;
        const uint8_t* body = d.data() + off;
        if (name == "TROP.SFM") { // the names (the system's language's TROP_nn.SFM would follow; this pack has Japanese only)
            const std::string xml(reinterpret_cast<const char*>(body), size_t(len));
            for (size_t at = 0; (at = xml.find("<trophy id=\"", at)) != std::string::npos;) {
                const int id = std::atoi(xml.c_str() + at + 12);
                size_t end = xml.find("</trophy>", at);
                if (end == std::string::npos) end = xml.size();
                if (id >= 0 && id < kCount) {
                    Info& t = out[size_t(id)];
                    const size_t tag = xml.find('>', at);
                    const std::string head = xml.substr(at, tag == std::string::npos ? 0 : tag - at);
                    const size_t tt = head.find("ttype=\"");
                    if (tt != std::string::npos && tt + 7 < head.size()) t.grade = head[tt + 7];
                    t.hidden = head.find("hidden=\"yes\"") != std::string::npos;
                    t.name = between(xml, at, end, "<name>", "</name>");
                    t.detail = between(xml, at, end, "<detail>", "</detail>");
                    named |= !t.name.empty();
                }
                at = end;
            }
        } else if (name.size() == 11 && name.compare(0, 4, "TROP") == 0 && name.compare(7, 4, ".PNG") == 0) {
            const int id = std::atoi(name.c_str() + 4);
            if (id >= 0 && id < kCount) out[size_t(id)].icon.assign(body, body + len);
        }
    }
    return named;
}

const char* builtinName(int n) {
    // TROP.SFM of NPWR08182_00 (the pack has no other language)
    static const char* const kNames[kCount] = {
        "輪廻の果て",
        "『ア・ドライグ・ゴッホ』を突破した！",
        "『ケルベロス』を突破した！",
        "『ゲノムス』を突破した！",
        "『アイラーヴァタ』を突破した！",
        "『オンディーヌ』を突破した！",
        "『オールド・ワン』を突破した！",
        "『ラー』を突破した！",
        "『プロトタイプ代弁者』を突破した！",
        "エッシャー編で原初の扉を通過した！",
        "シャモ編で原初の扉を通過した！",
        "オーガ編で原初の扉を通過した！",
        "アユタ編で原初の扉を通過した！",
        "『クオリア』を突破した！",
        "ビジネスマンとの戦い",
        "初めてのお買い物"};
    return n >= 0 && n < kCount ? kNames[n] : "";
}

char builtinGrade(int n) { return n == 0 ? 'G' : 'B'; }

const char* gradeWord(char grade) {
    switch (grade) {
    case 'P': return "(Platinum)";
    case 'G': return "(Gold)";
    case 'S': return "(Silver)";
    default: return "(Bronze)";
    }
}

} // namespace cr::trophies
