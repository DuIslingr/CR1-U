#pragma once
// Native Vita save conversion, shared by the four ports. No game/renderer types
// or subprocesses: prepare validates everything before install touches the
// profile.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
// Some Android zlib headers define the old near/far compatibility macros.
// Keep those implementation details out of the engine's camera/geometry code.
#pragma push_macro("near")
#pragma push_macro("far")
#include <zlib.h>
#pragma pop_macro("far")
#pragma pop_macro("near")

namespace vitaimport {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using Bag = std::map<int, int>;
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Plan {
    std::map<std::string, std::string> files;
    std::vector<std::string> warnings;
    int slots = 0;
    std::string summary() const {
        std::string s = "Found " + std::to_string(slots) + " save slot" + (slots == 1 ? "." : "s.");
        if (!warnings.empty())
            s += " An invalid suspend save was skipped.";
        return s;
    }
};
inline Bytes read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f)
        throw Error("Cannot read " + p.filename().u8string());
    const auto n = f.tellg();
    if (n < 0 || n > 16 * 1024 * 1024)
        throw Error("Invalid save file size.");
    Bytes b(static_cast<size_t>(n));
    f.seekg(0);
    if (!b.empty() && !f.read(reinterpret_cast<char *>(b.data()), n))
        throw Error("Cannot read the save completely.");
    return b;
}
inline uint32_t u32(const Bytes &b, size_t o) {
    if (o > b.size() || b.size() - o < 4)
        throw Error("Truncated save.");
    return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24;
}
inline int32_t i32(const Bytes &b, size_t o) { return int32_t(u32(b, o)); }
inline uint16_t u16(const Bytes &b, size_t o) {
    if (o > b.size() || b.size() - o < 2)
        throw Error("Truncated save.");
    return uint16_t(b[o] | uint16_t(b[o + 1]) << 8);
}
inline float real(const Bytes &b, size_t o) {
    const uint32_t v = u32(b, o);
    float f;
    std::memcpy(&f, &v, 4);
    if (!std::isfinite(f))
        throw Error("Invalid field position or rotation.");
    return f;
}
inline uint16_t crc(const Bytes &b) {
    uint16_t c = 0xffff;
    for (size_t i = 2; i < b.size(); ++i) {
        c ^= uint16_t(b[i]) << 8;
        for (int k = 0; k < 8; ++k)
            c = uint16_t(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
    }
    return uint16_t(~c);
}
inline Bytes binary(const fs::path &p, size_t size) {
    Bytes b = read(p);
    if (b.size() != size || u16(b, 2) != 0x3f6)
        throw Error(p.filename().u8string() + ": wrong size or version.");
    if (u16(b, 0) != crc(b))
        throw Error(p.filename().u8string() + ": checksum mismatch.");
    return b;
}
inline std::ostringstream text() {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::setprecision(9);
    return o;
}
inline int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
inline bool flag(const Bytes &b, size_t base, int n) { return (b.at(base + size_t(n / 8)) & (1 << (n % 8))) != 0; }
inline void flags(std::ostream &o, const Bytes &b, size_t base, int words, bool hex) {
    o << "flags";
    if (hex) {
        for (int i = 0; i < words; ++i)
            o << ' ' << std::hex << std::setfill('0') << std::setw(8) << u32(b, base + size_t(i) * 4);
        o << std::dec << std::setfill(' ');
    } else
        for (int n = 0; n < words * 32; ++n)
            if (flag(b, base, n))
                o << ' ' << n;
    o << '\n';
}
inline void bagLine(std::ostream &o, const char *key, const Bag &b) {
    o << key;
    for (const auto &[id, n] : b)
        if (n > 0)
            o << ' ' << id << '=' << n;
    o << '\n';
}
inline Bag bag(const Bytes &b, size_t base, size_t size, bool omega = false) {
    Bag result;
    for (size_t i = base; i < base + size; i += omega ? 4 : 2) {
        const int v = u16(b, i), id = omega ? v : v & 511, count = omega ? u16(b, i + 2) : v >> 9;
        if (id && count) {
            result[id] += count;
            if (result[id] > 99)
                throw Error("Invalid inventory count.");
        }
    }
    return result;
}
inline Bag add(Bag a, const Bag &b) {
    for (const auto &[id, n] : b)
        a[id] += n;
    return a;
}
inline std::vector<std::string> party(const Bytes &b, size_t base, int count, bool omega) {
    std::vector<std::string> result;
    for (int c = 0; c < count; ++c) {
        const size_t p = base + size_t(c) * 0x4c;
        int level = u16(b, p + 0x48);
        if (!level && omega && c == 3)
            level = 1;
        if (level < 1 || level > (omega ? 255 : 100))
            throw Error("Invalid character level.");
        auto o = text();
        o << level << ' ' << i32(b, p) << ' ' << i32(b, p + 8) << ' ' << u16(b, p + 0x14);
        for (int i = 0; i < 3; ++i)
            o << ' ' << u16(b, p + 0x2a + size_t(i) * 2);
        o << " 0 0 0 0";
        for (int i = 0; i < 4; ++i)
            o << ' ' << u16(b, p + 0x30 + size_t(i) * 2);
        const int n = std::max(level - 20, 0);
        const int grow = omega ? n : n * (n + 1) / 2;
        o << ' ' << i32(b, p + 0xc) - grow * 10;
        for (int offset : {0x16, 0x1c, 0x20, 0x24, 0x28})
            o << ' ' << int(u16(b, p + size_t(offset))) - grow;
        result.push_back(o.str());
    }
    return result;
}
inline std::map<std::string, std::string> firstGames(const Bytes &b, bool omega) {
    const int routes = omega ? 2 : 4, active = b.at(omega ? 0x1550 : 0xc18);
    if (active >= routes)
        throw Error("Invalid active scenario.");
    std::vector<Bag> bags;
    for (int r = 0; r < routes; ++r)
        bags.push_back(
            bag(b, (omega ? 0x79c : 0x580) + size_t(r) * (omega ? 0x500 : 0x100), omega ? 0x500 : 0x100, omega));
    Bag common = bag(b, omega ? 0x119c : 0x980, omega ? 0x300 : 0x140, omega), gear;
    if (omega) {
        static const int own[7][8] = {{85, 114, 116, 0, 135, 187, 189, 0}, {86, 82, 117, 0, 136, 188, 190, 0},
                                      {119, 0, 0, 0, 0, 0, 0, 0},          {85, 87, 83, 0, 135, 137, 131, 0},
                                      {86, 82, 84, 0, 136, 190, 132, 0},   {114, 81, 116, 0, 187, 189, 133, 0},
                                      {82, 88, 117, 0, 188, 134, 138, 0}};
        for (int r = 0; r < 2; ++r)
            for (int j = 0; j < (r ? 4 : 3); ++j)
                for (int k = 0; k < 8; ++k) {
                    const int item = own[j + (r ? 3 : 0)][k];
                    if (item && flag(b, 0x24, (r ? 0x1024 : 0x1004) + j * 8 + k))
                        bags[size_t(r)][item] = 1;
                }
    } else
        for (const Bag &a : bags)
            for (const auto &[id, n] : a)
                if (id >= 201 && id <= 424)
                    gear[id] = n;
    auto members = party(b, omega ? 0x4a4 : 0x320, omega ? 10 : 8, omega);
    if (omega)
        for (int k = 0; k < 3; ++k)
            members[size_t(4 + active * 3 + k)] = members[size_t(k)];
    auto base = text();
    if (omega)
        base << "wornbag 1\nscenario_genes 1\n";
    flags(base, b, omega ? 0x24 : 0x20, omega ? 160 : 128, true);
    base << "vars";
    for (int k = 0; k < (omega ? 128 : 64); ++k)
        if (const int n = i32(b, (omega ? 0x2a4 : 0x220) + size_t(k) * 4))
            base << ' ' << k << '=' << n;
    base << "\nwalkexp";
    for (int r = 0; r < 4; ++r)
        base << ' ' << (r < routes ? i32(b, (omega ? 0x149c : 0xac0) + size_t(r) * 4) : 0);
    base << "\nnoencounters";
    for (int r = 0; r < 4; ++r)
        base << ' ' << (r < routes ? int(b.at((omega ? 0x1548 : 0xc1b) + size_t(r))) : 0);
    base << '\n';
    if (!omega)
        base << "bands " << int(b[0xc10]) << ' ' << int(b[0xc11]) << ' ' << int(b[0xc12]) << ' ' << int(b[0xc13])
             << '\n';
    for (int r = 0; r < routes; ++r) {
        base << "treasure " << r;
        for (int k = 0; k < 30; ++k)
            base << ' ' << int16_t(u16(b, (omega ? 0x14cc : 0xb20) + size_t(r) * 0x3c + size_t(k) * 2));
        base << '\n';
    }
    std::map<std::string, std::string> saves;
    for (int r = 0; r < routes; ++r) {
        if (!b.at((omega ? 0x1546 : 0xc14) + size_t(r)))
            continue;
        const size_t pos = (omega ? 0x14a4 : 0xad0) + size_t(r) * 0x14;
        Bag items = add(common, bags[size_t(r)]);
        if (!omega) {
            for (auto it = items.begin(); it != items.end();)
                if (it->first >= 201 && it->first <= 424)
                    it = items.erase(it);
                else
                    ++it;
            items = add(items, gear);
            for (int id = 81; id <= 88; ++id)
                if (flag(b, 0x20, id + r * 16 + 0xf59))
                    items[id] = 1;
            for (int id = 131; id <= 138; ++id)
                if (flag(b, 0x20, id + r * 16 + 0xf2f))
                    items[id] = 1;
        }
        auto o = text();
        o << "CRSAVE 1\narea " << u16(b, pos + 16) << "\nmap " << u16(b, pos + 18) << "\npos " << real(b, pos) << ' '
          << real(b, pos + 4) << ' ' << real(b, pos + 8) << "\nyaw " << real(b, pos + 12) * 0.017453292519943295
          << "\nleader " << (omega ? int(b[0x154e + size_t(r)]) : r * 2) << "\nplay "
          << i32(b, 4) * 3600.0 + i32(b, 8) / 1000.0 << "\nsaved " << now() << '\n';
        bagLine(o, "items", items);
        o << "oz " << i32(b, 0xc + (omega ? size_t(r) * 4 : 0)) << '\n';
        if (omega) {
            o << "scenario " << r;
            for (int k = 0; k < 4; ++k)
                o << ' ' << int(int8_t(b[0x154a + size_t(k)]));
            o << ' ' << int(b[0x154e]) << ' ' << int(b[0x154f]) << ' ' << i32(b, 0xc + size_t(1 - r) * 4) << ' '
              << int(b[0x1544]) << ' ' << int(b[0x1545]) << '\n';
            bagLine(o, "otherbag", bags[size_t(1 - r)]);
        }
        o << base.str();
        for (size_t c = 0; c < members.size(); ++c)
            o << "party " << c << ' ' << members[omega && c < 3 ? size_t(4 + r * 3) + c : c] << '\n';
        saves["save" + std::to_string(r + 1) + ".dat"] = o.str();
    }
    const auto at = saves.find("save" + std::to_string(active + 1) + ".dat");
    if (at == saves.end())
        throw Error("The active route has no saved position.");
    saves["resume.dat"] = at->second;
    return saves;
}
inline std::string secondGame(const Bytes &b, const Bytes &sys) {
    auto o = text();
    o << "CR2-U save 1\nflags";
    for (int n = 0; n < 992; ++n)
        if (flag(sys, 4, n))
            o << ' ' << n;
    for (int n = 1001; n < 4000; ++n)
        if (flag(b, 0x630, n))
            o << ' ' << n;
    const int dungeon = i32(b, 0x5f0), area = i32(b, 0x5f4);
    if (dungeon < 0 || dungeon > 99 || area < 0 || area > 98)
        throw Error("Invalid CR2 map.");
    o << "\nparty " << int(int8_t(b[0x37])) << ' ' << int(int8_t(b[0x38])) << "\nactive " << i32(b, 0x5f8) << ' '
      << int(b[0x622]) << ' ' << int(b[0x623]) << ' ' << int(b[0x620]) << '\n';
    for (int k = 0; k < 3; ++k)
        o << "pairs " << k << ' ' << int(int8_t(b[0x39 + size_t(k) * 2])) << ' ' << int(int8_t(b[0x3a + size_t(k) * 2]))
          << '\n';
    for (int id = 1; id < 300; ++id)
        if (const int n = int16_t(u16(b, 0x40 + size_t(id) * 2)))
            o << "item " << id << ' ' << n << '\n';
    o << "oz " << i32(b, 0x298) << '\n';
    for (int c = 0; c < 9; ++c) {
        const size_t p = 0x340 + size_t(c) * 0x4c;
        const int mp = int16_t(u16(b, p + 0x10)), most = int16_t(u16(b, p + 0x12));
        o << "chara " << c << ' ' << i32(b, p) << ' ' << i32(b, p + 4) << ' ' << (mp >= most ? -1 : mp);
        for (int k = 0; k < 8; ++k)
            o << ' ' << int16_t(u16(b, p + 0x3c + size_t(k) * 2));
        for (int k = 0; k < 5; ++k)
            o << ' ' << int16_t(u16(b, p + 0x1e + size_t(k) * 2));
        o << '\n';
    }
    for (int k = 0; k < 32; ++k)
        o << "sopia " << k << ' ' << int(b[0x2a0 + size_t(k)]) << ' ' << i32(b, 0x2c0 + size_t(k) * 4) << '\n';
    o << "owned " << u32(b, 0x29c) << "\ndifficulty " << int(b[0x34]) << "\nboxes " << i32(b, 0x8a4);
    for (int kind = 0; kind < 3; ++kind)
        for (int k = 0; k < 10; ++k) {
            const size_t p = (kind == 0 ? 0x8a8 : kind == 1 ? 0x8bc : 0x8c2) + size_t(k) * 2;
            o << ' ' << (k < (kind ? 3 : 10) ? int(b[p] != 0) + 2 * int(b[p + 1] != 0) : 0);
        }
    o << "\nglobal";
    for (int k = 0; k < 32; ++k)
        o << ' ' << i32(b, 0x824 + size_t(k) * 4);
    o << "\nmap ";
    if (dungeon == 0)
        o << "t01";
    else
        o << 'd' << std::setfill('0') << std::setw(2) << dungeon;
    o << std::setfill('0') << std::setw(2) << area + 1 << std::setfill(' ') << "\nplace " << real(b, 0x600) << ' '
      << real(b, 0x604) << ' ' << real(b, 0x608) << ' ' << 2 * std::atan2(real(b, 0x614), real(b, 0x61c)) << "\nsaved "
      << now() << "\nplaytime " << u32(b, 0x10) << "\noptions";
    for (int k = 0; k < 5; ++k)
        o << ' ' << i32(sys, 0x80 + size_t(k) * 4);
    o << '\n';
    return o.str();
}

// Small strict JSON DOM for CR3's save documents; numbers retain 64-bit
// timestamps.
struct Json {
    enum Kind { Null, Number, String, Object, Array, Boolean } kind = Null;
    double number = 0;
    std::string string;
    std::map<std::string, Json> object;
    std::vector<Json> array;
    const Json &operator[](const std::string &k) const {
        static const Json none;
        auto it = object.find(k);
        return it == object.end() ? none : it->second;
    }
    const Json &operator[](size_t k) const {
        if (k >= array.size())
            throw Error("Incomplete CR3 record.");
        return array[k];
    }
    int64_t n(int64_t fallback = 0) const {
        if (kind != Number && kind != Boolean)
            return fallback;
        if (number < -9007199254740991.0 || number > 9007199254740991.0)
            throw Error("CR3 number out of range.");
        return int64_t(number);
    }
    std::string s() const { return string; }
    bool has(const std::string &k) const { return object.count(k) != 0; }
};
class JsonParser {
    const std::string &text_;
    size_t p_ = 0;
    void ws() {
        while (p_ < text_.size() && (text_[p_] == ' ' || text_[p_] == '\n' || text_[p_] == '\r' || text_[p_] == '\t'))
            ++p_;
    }
    char take() {
        if (p_ >= text_.size())
            throw Error("Truncated CR3 JSON.");
        return text_[p_++];
    }
    void expect(char c) {
        ws();
        if (take() != c)
            throw Error("Invalid CR3 JSON.");
    }
    uint32_t hex() {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = take();
            int n = c >= '0' && c <= '9'   ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
            if (n < 0)
                throw Error("Invalid Unicode escape.");
            v = v * 16 + uint32_t(n);
        }
        return v;
    }
    static void utf8(std::string &s, uint32_t c) {
        if (c < 0x80)
            s += char(c);
        else if (c < 0x800) {
            s += char(0xc0 | (c >> 6));
            s += char(0x80 | (c & 63));
        } else if (c < 0x10000) {
            s += char(0xe0 | (c >> 12));
            s += char(0x80 | ((c >> 6) & 63));
            s += char(0x80 | (c & 63));
        } else {
            s += char(0xf0 | (c >> 18));
            s += char(0x80 | ((c >> 12) & 63));
            s += char(0x80 | ((c >> 6) & 63));
            s += char(0x80 | (c & 63));
        }
    }
    std::string str() {
        expect('"');
        std::string s;
        for (;;) {
            char c = take();
            if (c == '"')
                return s;
            if (uint8_t(c) < 32)
                throw Error("Control character in JSON string.");
            if (c != '\\') {
                s += c;
                continue;
            }
            c = take();
            switch (c) {
            case '"':
            case '\\':
            case '/':
                s += c;
                break;
            case 'b':
                s += '\b';
                break;
            case 'f':
                s += '\f';
                break;
            case 'n':
                s += '\n';
                break;
            case 'r':
                s += '\r';
                break;
            case 't':
                s += '\t';
                break;
            case 'u': {
                uint32_t a = hex();
                if (a >= 0xd800 && a <= 0xdbff) {
                    if (take() != '\\' || take() != 'u')
                        throw Error("Invalid Unicode pair.");
                    uint32_t b = hex();
                    if (b < 0xdc00 || b > 0xdfff)
                        throw Error("Invalid Unicode pair.");
                    a = 0x10000 + ((a - 0xd800) << 10) + (b - 0xdc00);
                } else if (a >= 0xdc00 && a <= 0xdfff)
                    throw Error("Invalid Unicode pair.");
                utf8(s, a);
                break;
            }
            default:
                throw Error("Invalid JSON escape.");
            }
        }
    }
    Json value(int depth) {
        if (depth > 64)
            throw Error("CR3 JSON nesting too deep.");
        ws();
        if (p_ >= text_.size())
            throw Error("Missing CR3 JSON value.");
        Json j;
        char c = text_[p_];
        if (c == '{') {
            ++p_;
            j.kind = Json::Object;
            ws();
            if (p_ < text_.size() && text_[p_] == '}') {
                ++p_;
                return j;
            }
            for (;;) {
                ws();
                std::string k = str();
                expect(':');
                j.object[k] = value(depth + 1); // The original writer repeats vipTime/vipTrial;
                                                // the last value wins.
                ws();
                c = take();
                if (c == '}')
                    return j;
                if (c != ',')
                    throw Error("Invalid JSON object.");
            }
        }
        if (c == '[') {
            ++p_;
            j.kind = Json::Array;
            ws();
            if (p_ < text_.size() && text_[p_] == ']') {
                ++p_;
                return j;
            }
            for (;;) {
                j.array.push_back(value(depth + 1));
                ws();
                c = take();
                if (c == ']')
                    return j;
                if (c != ',')
                    throw Error("Invalid JSON array.");
            }
        }
        if (c == '"') {
            j.kind = Json::String;
            j.string = str();
            return j;
        }
        for (const char *word : {"true", "false", "null"}) {
            size_t n = std::strlen(word);
            if (text_.compare(p_, n, word) == 0) {
                p_ += n;
                j.kind = word[0] == 'n' ? Json::Null : Json::Boolean;
                j.number = word[0] == 't' ? 1 : 0;
                return j;
            }
        }
        const size_t start = p_;
        if (c == '-')
            ++p_;
        if (p_ >= text_.size())
            throw Error("Invalid JSON number.");
        if (text_[p_] == '0')
            ++p_;
        else {
            if (text_[p_] < '1' || text_[p_] > '9')
                throw Error("Invalid JSON number.");
            while (p_ < text_.size() && text_[p_] >= '0' && text_[p_] <= '9')
                ++p_;
        }
        if (p_ < text_.size() && text_[p_] == '.') {
            ++p_;
            size_t a = p_;
            while (p_ < text_.size() && text_[p_] >= '0' && text_[p_] <= '9')
                ++p_;
            if (a == p_)
                throw Error("Invalid JSON fraction.");
        }
        if (p_ < text_.size() && (text_[p_] == 'e' || text_[p_] == 'E')) {
            ++p_;
            if (p_ < text_.size() && (text_[p_] == '+' || text_[p_] == '-'))
                ++p_;
            size_t a = p_;
            while (p_ < text_.size() && text_[p_] >= '0' && text_[p_] <= '9')
                ++p_;
            if (a == p_)
                throw Error("Invalid JSON exponent.");
        }
        std::istringstream in(text_.substr(start, p_ - start));
        in.imbue(std::locale::classic());
        in >> j.number;
        if (!in || !std::isfinite(j.number))
            throw Error("Invalid JSON number.");
        j.kind = Json::Number;
        return j;
    }

  public:
    explicit JsonParser(const std::string &s) : text_(s) {}
    Json parse() {
        Json j = value(0);
        ws();
        if (p_ != text_.size() || j.kind != Json::Object)
            throw Error("Invalid CR3 JSON document.");
        return j;
    }
};
inline std::string inflate(const std::string &data) {
    z_stream z{};
    z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
    z.avail_in = uInt(data.size());
    if (inflateInit(&z) != Z_OK)
        throw Error("Cannot initialize save decompression.");
    std::string result;
    int status = Z_OK;
    char buf[8192];
    do {
        z.next_out = reinterpret_cast<Bytef *>(buf);
        z.avail_out = sizeof(buf);
        status = ::inflate(&z, Z_NO_FLUSH);
        result.append(buf, sizeof(buf) - z.avail_out);
        if (result.size() > 16 * 1024 * 1024) {
            inflateEnd(&z);
            throw Error("Decompressed save too large.");
        }
    } while (status == Z_OK);
    const bool good = status == Z_STREAM_END && z.avail_in == 0;
    inflateEnd(&z);
    if (!good)
        throw Error("Invalid compressed save.");
    return result;
}
inline std::string base64(const std::string &s) {
    const std::string table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (s.empty() || s.size() % 4)
        throw Error("Invalid CR3 base64.");
    std::string out;
    for (size_t i = 0; i < s.size(); i += 4) {
        uint32_t v = 0;
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i + size_t(k)];
            if (c == '=') {
                if (i + 4 != s.size() || k < 2)
                    throw Error("Invalid base64 padding.");
                ++pad;
                v <<= 6;
            } else {
                size_t at = table.find(c);
                if (at == std::string::npos || pad)
                    throw Error("Invalid CR3 base64.");
                v = (v << 6) | uint32_t(at);
            }
        }
        out += char(v >> 16);
        if (pad < 2)
            out += char(v >> 8);
        if (!pad)
            out += char(v);
    }
    return out;
}
inline std::string cr3Document(const Bytes &b) {
    const size_t n = u32(b, 0);
    if (!n || n > b.size() - 4)
        throw Error("Invalid CR3 payload length.");
    std::string s(reinterpret_cast<const char *>(b.data() + 4), n);
    if (s.compare(0, 16, "^q7@#=0TmLx3MC5$") == 0) {
        if (s.size() < 20 || u32(b, 20) != s.size() - 20)
            throw Error("Invalid CR3 compressed length.");
        s = inflate(s.substr(20));
    }
    if (s.size() % 2)
        throw Error("Invalid CR3 encoded length.");
    std::string d;
    const char *key = "mediavision";
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = ((uint8_t(s[i]) + 1) & 255) ^ key[i % 11], c = ((uint8_t(s[i + 1]) + 1) & 255) ^ key[(i + 1) % 11];
        if (a > 15 || c > 15)
            throw Error("Invalid CR3 encoded nibble.");
        d += char(a * 16 + c);
    }
    if (d.empty())
        throw Error("Empty CR3 save.");
    if (d[0] != '{')
        d = inflate(base64(d));
    return d;
}
inline void numbers(std::ostream &o, const Json &j) {
    for (const Json &v : j.array)
        o << ' ' << v.n();
}
inline void gene(std::ostream &o, const Json &g, int uid, bool stocked) {
    if (g["commands"].array.size() != 16 || g["add"].array.size() != 4 || g["masterId"].n() <= 0)
        throw Error("Incomplete CR3 gene.");
    o << g["masterId"].n() << ' ' << g["exp"].n() << ' ' << (g["locking"].n() != 0) << ' ' << stocked << " 0";
    numbers(o, g["commands"]);
    numbers(o, g["add"]);
    o << ' ' << uid;
}
inline std::string thirdGame(const Json &d) {
    const Json &sd = d["savedata"], &profile = d["profile"], &f = sd["field"];
    if (sd.kind != Json::Object || profile.kind != Json::Object || f.kind != Json::Object || !d.has("flags"))
        throw Error("Not a CR3 gameplay save.");
    static const char *prefix[] = {"t01", "t02", "t03", "t04", "t05", "d01", "d02", "d03", "d04", "d05", "d06", "d07",
                                   "d08", "d09", "d10", "d11", "d12", "d13", "d14", "d15", "d16", "d17", "e00"};
    static const char *names[] = {"", "NA", "LE", "DA", "EL", "AL", "JO"};
    int dungeon = int(f["nDungeonNum"].n(-1)), area = int(f["nAreaNum"].n(-1));
    if (dungeon < 0 || dungeon > 22 || area < 0 || area > 98 || f["vPosition"].array.size() != 3 ||
        f["qRotation"].array.size() != 4 || sd["main_parties"].array.size() != 5)
        throw Error("Invalid CR3 field or party.");
    auto o = text();
    o << "CR3-U save 1\nflags";
    numbers(o, d["flags"]);
    o << "\ncr3party";
    numbers(o, sd["main_parties"]);
    o << "\nactive " << f["nActivePC"].n() << ' ' << f["bChangeCharacterEnable"].n(1) << ' ' << f["bTabMenuEnable"].n(1)
      << ' ' << f["bEncountEnable"].n(1) << "\nselect " << dungeon << ' ' << f["nDungeonLevel"].n() << "\narea "
      << prefix[dungeon] << std::setfill('0') << std::setw(2) << area + 1 << std::setfill(' ') << "\nplace";
    for (const Json &p : f["vPosition"].array)
        o << ' ' << p.number;
    o << ' ' << 2 * std::atan2(f["qRotation"][1].number, f["qRotation"][3].number) << "\nglobal";
    for (int k = 0; k < 100; ++k)
        o << ' ' << d["workFlags"][std::to_string(k)].n();
    o << "\nsaved " << d["clientTime"].n() / 1000 << "\nplaytime " << profile["playtime"].n() * 20 << "\ncr3team "
      << profile["teamLevel"].n(1) << ' ' << profile["value"].n() << '\n';
    for (const auto &p : std::map<std::string, std::string>{
             {"name", "mainChar"}, {"team", "teamName"}, {"pet", "petName"}, {"food", "foodName"}}) {
        const std::string s = profile[p.second].s();
        if (s.find_first_of(std::string("\r\n\0", 3)) != std::string::npos)
            throw Error("Invalid player name.");
        o << "cr3name " << p.first << ' ' << s << '\n';
    }
    Bag counts;
    for (const Json &item : d["items"].array) {
        int master = int(item["masterId"].n()), cat = master / 100000;
        counts[master] += int(item["count"].n());
        const std::string tag = item["tag"].s();
        if (cat >= 1 && cat <= 3)
            for (int ch = 1; ch <= 6; ++ch)
                if (tag.find(std::string("-M") + names[ch]) != std::string::npos)
                    o << (cat == 1   ? "cr3weapon "
                          : cat == 2 ? "cr3armour "
                                     : "cr3accessory ")
                      << ch << ' ' << master % 100000 << '\n';
        if (item["enforceRemain"].n() > 0)
            o << "cr3attach " << master << ' ' << item["enforceItem"].n() << ' ' << item["enforceRemain"].n() << '\n';
    }
    for (const auto &[id, n] : counts)
        if (n)
            o << "cr3item " << id << ' ' << n << '\n';
    int uid = 0;
    for (int stock = 0; stock < 2; ++stock)
        for (const Json &g : d[stock ? "geneStocks" : "genes"].array) {
            o << "cr3gene ";
            gene(o, g, ++uid, stock != 0);
            o << '\n';
            if (!stock)
                for (int ch = 1; ch <= 6; ++ch)
                    if (g["tag"].s().find(std::string("-M") + names[ch]) != std::string::npos)
                        o << "cr3geneof " << ch << ' ' << uid << '\n';
        }
    o << "cr3geneuid " << uid << "\ncr3book";
    numbers(o, d["geneCollections"]);
    o << "\ncr3seen";
    std::set<int64_t> marks;
    for (const Json &v : d["geneCollectionMarks"].array)
        marks.insert(v.n());
    for (const Json &v : d["geneCollections"].array)
        if (!marks.count(v.n()))
            o << ' ' << v.n();
    o << "\ncr3money " << d["gold"].n() << ' ' << d["coin"].n() << ' ' << d["usedCoin"].n() << ' ' << d["GP"].n() << ' '
      << d["usedGP"].n() << ' ' << d["geneStock"].n() << "\ncr3days 0 " << d["vipDays"].n() << ' ' << d["vipTrial"].n()
      << ' ' << d["lastTime"].n() << ' ' << d["cheer"].n() << '\n';
    for (const Json &s : d["itemsheets"].array) {
        o << "cr3sheet " << s["_id"].s() << ' ' << s["full"].n() << " 0 1 " << s["items"].array.size();
        for (const Json &v : s["items"].array)
            o << ' ' << v.n() << ' ' << (v.n() > 0);
        o << '\n';
    }
    for (const Json &ch : d["characters"].array)
        o << "cr3mp " << ch["chara_id"].n() << ' ' << ch["MP"].n() << '\n';
    for (const Json &p : d["presents"].array) {
        bool g = p["category"].n() == 1;
        o << "cr3present " << p["masterId"].n() << ' ' << p["count"].n(1) << ' ' << p["from"].n() << ' ' << g;
        if (g) {
            o << ' ';
            gene(o, p.has("gene") ? p["gene"] : p, 0, false);
        }
        o << '\n';
    }
    const Json &q = d["quest"];
    o << "cr3begun";
    numbers(o, q["accepted"]);
    o << "\ncr3ended";
    numbers(o, q["completed"]);
    o << "\ncr3offered";
    for (const Json &v : q["orders"].array)
        if (v.n())
            o << ' ' << v.n();
    o << "\ncr3quests 0 " << profile["questCount"].n() << " -100\n";
    const Json &v = d["vs"], &m = v["mission"], &season = v["season"];
    o << "cr3vs " << v["score"].n() << ' ' << v["totalScore"].n() << ' ' << v["rewardsTotalScore"].n() << ' '
      << v["resetTime"].n() << ' ' << m["_id"].n() << ' ' << m["score"].n() << ' ' << m["totalScore"].n() << ' '
      << m["goal"].n() << ' ' << m["level"].n() << ' ' << m["type"].n() << ' ' << m["reward"].n() << ' '
      << m["earned"].n() << ' ' << m["geneLevel"].n(1) << ' ' << m["teamLevel"].n(1) << ' ' << m["geneLevelMax"].n(1)
      << ' ' << m["teamLevelMax"].n(1) << ' ' << m["count"].n() << ' ' << season["_id"].n(-1) << ' '
      << season["totalScore"].n() << ' ' << season["count"].n() << " 0 " << (v["state"].n() != 0) << ' '
      << (v["earned"].n() != 0) << ' ' << v["tournament"].n() << ' ' << d["vs_prev_normal_tournament"].n(-1) << ' '
      << d["vs_prev_special_tournament"].n(-1) << "\ncr3vsboosts";
    for (const Json &x : v["items"].array)
        o << ' ' << x["masterId"].n() << ' ' << x["count"].n();
    o << "\ncr3vscleared";
    numbers(o, v["completed"]);
    o << "\ncr3records";
    for (const char *k : {"playtime", "killCount", "maxDamage", "vsTournamentCount", "acquiredGeneCount",
                          "mergedGeneCount", "mergedGiftCount"})
        o << ' ' << sd["records"][k].n();
    o << '\n';
    for (const char *k : {"tboxes", "gboxes"})
        for (const Json &box : d[k].array) {
            std::string name = box["place"].s();
            if (name.compare(0, 8, "ragdoll_") == 0)
                name.erase(0, 8);
            if (name.find_first_of(" \t\r\n") != std::string::npos)
                throw Error("Invalid treasure box name.");
            o << "cr3box " << box["area_no"].n() << ' ' << name << ' ' << box["box_type"].n() << ' ' << (k[0] == 'g')
              << ' ' << box["item_type"].n(-1) << ' ' << box["item_no"].n(-1) << ' ' << box["is_open"].n() << '\n';
        }
    const Json &op = sd["option"];
    o << "options " << op["language"].n() << ' ' << op["graphLevel"].n(1);
    for (const char *k : {"volumeBGM", "volumeSE", "volumeVoice"})
        o << ' ' << op[k].n(100);
    o << '\n';
    return o.str();
}
inline Plan prepare(const std::string &folder, const std::string &game) {
    const std::map<std::string, std::pair<std::string, int>> titles = {{"cr1", {"PCSG00497", 0}},
                                                                       {"omega", {"PCSG00499", 10}},
                                                                       {"cr2", {"PCSG00498", 20}},
                                                                       {"cr3", {"PCSG00457", 30}}};
    auto entry = titles.find(game);
    if (entry == titles.end())
        throw Error("Unknown game.");
    fs::path root = fs::u8path(folder);
    if (!fs::is_directory(root))
        throw Error("Choose an exported save folder.");
    if (fs::is_directory(root / entry->second.first))
        root /= entry->second.first;
    const int slot = entry->second.second;
    auto file = [&](int n) {
        auto o = text();
        o << "SAVEDATA" << std::setfill('0') << std::setw(4) << slot + n << ".bin";
        return root / o.str();
    };
    Plan plan;
    if (game == "cr3") {
        for (int k = 1; k <= 3; ++k)
            if (fs::exists(file(k))) {
                std::string doc = cr3Document(read(file(k)));
                Json parsed = JsonParser(doc).parse();
                plan.files["save" + std::to_string(k) + ".txt"] = thirdGame(parsed);
                plan.files["vita-slot" + std::to_string(k) + ".json"] = doc;
                ++plan.slots;
            }
    } else {
        const bool omega = game == "omega", second = game == "cr2";
        Bytes sys;
        const size_t size = second ? 0x910 : omega ? 0x1558 : 0xc24;
        if (second) {
            sys = read(file(0));
            if (sys.size() != 0x98 && sys.size() != 0xb8)
                throw Error("Invalid CR2 system save size.");
            sys = binary(file(0), sys.size());
            auto o = text();
            o << "CR2-U system 1\n";
            flags(o, sys, 4, 31, false);
            o << "options";
            for (int k = 0; k < 5; ++k)
                o << ' ' << i32(sys, 0x80 + size_t(k) * 4);
            o << '\n';
            plan.files["system.txt"] = o.str();
        }
        Bytes manual = binary(file(1), size);
        if (second) {
            plan.files["save1.txt"] = secondGame(manual, sys);
            plan.files["cont.txt"] = plan.files["save1.txt"];
            plan.slots = 1;
        } else {
            plan.files = firstGames(manual, omega);
            plan.slots = int(plan.files.size()) - 1;
        }
        if (fs::exists(file(2)))
            try {
                Bytes b = binary(file(2), size);
                if (second)
                    plan.files["cont.txt"] = secondGame(b, sys);
                else
                    plan.files["resume.dat"] = firstGames(b, omega).at("resume.dat");
            } catch (const Error &e) {
                plan.warnings.push_back(e.what());
            }
    }
    if (!plan.slots)
        throw Error("No gameplay saves for this game were found.");
    return plan;
}
inline std::string unique() {
    static std::atomic<unsigned> n{0};
    return std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + "-" +
           std::to_string(n++);
}
inline void write(const fs::path &p, const std::string &s) {
    std::ofstream f(p, std::ios::binary);
    if (!f || !f.write(s.data(), std::streamsize(s.size())) || !f.flush())
        throw Error("Cannot write imported saves.");
}
inline std::string install(const Plan &plan, const std::string &destination) {
    if (plan.files.empty())
        throw Error("No saves prepared.");
    const fs::path dest = fs::u8path(destination);
    fs::create_directories(dest);
    const fs::path stage = dest / (".vita-import-" + unique()), backup = dest / ("vita-backup-" + unique());
    fs::create_directory(stage);
    std::set<std::string> existing;
    std::vector<std::string> written;
    try {
        for (const auto &[name, data] : plan.files) {
            if (fs::path(name).filename().u8string() != name)
                throw Error("Invalid output filename.");
            write(stage / fs::u8path(name), data);
            if (fs::exists(dest / fs::u8path(name)))
                existing.insert(name);
        }
        if (!existing.empty()) {
            fs::create_directory(backup);
            for (const auto &name : existing)
                fs::copy_file(dest / fs::u8path(name), backup / fs::u8path(name));
        }
        for (const auto &[name, data] : plan.files) {
            (void)data;
            written.push_back(name);
            const auto target = dest / fs::u8path(name);
            if (existing.count(name))
                fs::remove(target);
            fs::rename(stage / fs::u8path(name), target);
        }
        fs::remove_all(stage);
        return existing.empty() ? std::string() : backup.u8string();
    } catch (...) {
        bool restored = true;
        for (const auto &name : written)
            try {
                auto target = dest / fs::u8path(name);
                if (existing.count(name))
                    fs::copy_file(backup / fs::u8path(name), target, fs::copy_options::overwrite_existing);
                else
                    fs::remove(target);
            } catch (...) {
                restored = false;
            }
        std::error_code ignored;
        fs::remove_all(stage, ignored);
        if (!restored)
            throw Error("Import failed. Restore your previous saves from " + backup.u8string());
        throw;
    }
}
} // namespace vitaimport
