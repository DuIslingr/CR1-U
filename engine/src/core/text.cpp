#include "core/text.hpp"

namespace cr {

void appendUtf8(std::string& s, uint32_t c) {
    if (c < 0x80) {
        s += char(c);
    } else if (c < 0x800) {
        s += char(0xC0 | c >> 6);
        s += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        s += char(0xE0 | c >> 12);
        s += char(0x80 | (c >> 6 & 0x3F));
        s += char(0x80 | (c & 0x3F));
    } else {
        s += char(0xF0 | c >> 18);
        s += char(0x80 | (c >> 12 & 0x3F));
        s += char(0x80 | (c >> 6 & 0x3F));
        s += char(0x80 | (c & 0x3F));
    }
}

uint32_t nextUtf8(const std::string& s, size_t& i) {
    const uint8_t b = uint8_t(s[i++]);
    if (b < 0x80) return b;
    const int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    uint32_t c = b & (0x3F >> extra);
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || (uint8_t(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        c = c << 6 | (uint8_t(s[i++]) & 0x3F);
    }
    return c;
}

namespace {

// U+00C0..U+017F as ASCII, nullptr if not a letter there
const char* foldLatin(uint32_t u) {
    static const char* const k00C0[64] = {
        "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
        "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
        "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y"};
    if (u >= 0xC0 && u < 0x100) return k00C0[u - 0xC0];
    if (u == 0x152) return "OE";
    if (u == 0x153) return "oe";
    if (u >= 0x100 && u < 0x180) { // Latin Extended-A: pairs of upper / lower case
        static const char kBase[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        static char out[2];
        out[0] = kBase[u - 0x100];
        out[1] = 0;
        return out;
    }
    return nullptr;
}

} // namespace

std::string foldAscii(const std::string& utf8) {
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        const uint32_t u = nextUtf8(utf8, i);
        if (u < 0x80) out += char(u);
        else if (u == 0x2018 || u == 0x2019) out += '\'';
        else if (u == 0x201c || u == 0x201d || u == 0xab || u == 0xbb) out += '"';
        else if (u == 0x2026) out += "...";
        else if (u == 0x2013 || u == 0x2014) out += '-';
        else if (u == 0x3000) out += ' ';
        else if (u >= 0xff01 && u <= 0xff5e) out += char(u - 0xff01 + 0x21); // full-width ASCII
        else if (u == 0xa1 || u == 0xbf) continue;                           // Spanish opening marks
        else if (const char* f = foldLatin(u)) out += f;
        else out += '?';
    }
    return out;
}

} // namespace cr
