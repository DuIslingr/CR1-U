#include "platform/ui_text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <map>
#include <unordered_map>

namespace uitext {

// The catalogs built in (ui_text_catalogs.cpp, generated from lang/*.po by cmake/embed_lang.cmake)
struct BuiltIn {
    const char* code;
    const unsigned char* data;
    size_t size;
};
extern const BuiltIn kBuiltIn[];
extern const size_t kBuiltInCount;

namespace {

std::vector<std::string> folders;
std::string current;                                            // "" = English
std::unordered_map<std::string, std::string> table;             // the current language's: English -> it
std::function<bool(const std::string&)> coverage;
int coverageGeneration = -1;
std::unordered_map<std::string, bool> covered;                  // (per translation, for the font of coverageGeneration)

std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        const char c = s[++i];
        out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c; // (\" \\ and the rest as themselves)
    }
    return out;
}

// The text of one quoted .po string ("..." with the quotes), "" if the line isn't one
bool quoted(const std::string& line, size_t from, std::string& out) {
    const size_t a = line.find('"', from);
    const size_t b = line.rfind('"');
    if (a == std::string::npos || b == a) return false;
    out = unescape(line.substr(a + 1, b - a - 1));
    return true;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string headerField(const std::string& header, const char* key) {
    const std::string k = std::string(key) + ":";
    size_t at = 0;
    while ((at = header.find(k, at)) != std::string::npos) {
        if (at == 0 || header[at - 1] == '\n') {
            const size_t end = header.find('\n', at);
            return trimmed(header.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size()));
        }
        at += k.size();
    }
    return {};
}

bool readFile(const std::string& path, std::string& out) {
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) return false;
    out.assign(static_cast<const char*>(data), size);
    SDL_free(data);
    return true;
}

// The catalog's text for `code`: a loose one first, then the built-in one
bool catalogText(const std::string& code, std::string& out) {
    for (const std::string& d : folders)
        if (readFile(d + code + ".po", out)) return true;
    for (size_t i = 0; i < kBuiltInCount; ++i)
        if (code == kBuiltIn[i].code) {
            out.assign(reinterpret_cast<const char*>(kBuiltIn[i].data), kBuiltIn[i].size);
            return true;
        }
    return false;
}

const char* const kTextLanguages[8] = {"ja", "en", "fr", "de", "es", "zh-Hans", "zh-Hant", "ko"};

} // namespace

std::vector<std::pair<std::string, std::string>> parsePo(const std::string& text, std::string* name) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string id, str, header;
    int field = 0; // 0 none, 1 msgid, 2 msgstr, 3 something else (msgctxt, msgid_plural, msgstr[n]: kept out)
    bool fuzzy = false, haveEntry = false;
    auto finish = [&] {
        if (haveEntry) {
            if (id.empty()) header = str;
            else if (!str.empty() && !fuzzy) out.emplace_back(id, str);
        }
        id.clear(), str.clear();
        field = 0, fuzzy = false, haveEntry = false;
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = std::min(text.find('\n', pos), text.size());
        const std::string line = trimmed(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty()) {
            finish();
            continue;
        }
        if (line[0] == '#') {
            if (haveEntry && field == 2) finish(); // (comments start the next entry)
            if (line.compare(0, 2, "#,") == 0 && line.find("fuzzy") != std::string::npos) fuzzy = true;
            continue;
        }
        std::string part;
        if (line.compare(0, 6, "msgid ") == 0) {
            if (haveEntry && field >= 2) finish();
            if (quoted(line, 5, part)) id = part, field = 1, haveEntry = true;
        } else if (line.compare(0, 7, "msgstr ") == 0) {
            if (quoted(line, 6, part)) str = part, field = 2;
        } else if (line[0] == '"') {
            if (quoted(line, 0, part)) {
                if (field == 1) id += part;
                else if (field == 2) str += part;
            }
        } else {
            field = 3; // msgctxt, msgid_plural, msgstr[n]: not used by the ports
            if (line.compare(0, 8, "msgctxt ") == 0) fuzzy = true; // (a context would make the key ambiguous: left out)
        }
        if (end >= text.size()) break;
    }
    finish();
    if (name) *name = headerField(header, "X-Language-Name");
    return out;
}

void addFolder(const std::string& dir) {
    if (dir.empty()) return;
    const std::string d = dir.back() == '/' || dir.back() == '\\' ? dir : dir + "/";
    if (std::find(folders.begin(), folders.end(), d) == folders.end()) folders.push_back(d);
}

std::string languageName(const std::string& code) {
    if (code.empty() || code == "en") return "English";
    std::string text, name;
    if (!catalogText(code, text)) return {};
    parsePo(text, &name);
    return name.empty() ? code : name;
}

std::vector<Language> languages() {
    std::map<std::string, std::string> found; // code -> name
    for (size_t i = 0; i < kBuiltInCount; ++i) found[kBuiltIn[i].code];
    for (const std::string& d : folders) {
        int count = 0;
        char** names = SDL_GlobDirectory(d.c_str(), "*.po", 0, &count);
        for (int i = 0; names && i < count; ++i) {
            std::string f = names[i];
            const size_t slash = f.find_last_of("/\\");
            if (slash != std::string::npos) f = f.substr(slash + 1);
            if (f.size() > 3 && f != "template.po") found[f.substr(0, f.size() - 3)];
        }
        SDL_free(names);
    }
    found.erase("en");
    std::vector<Language> out{{"en", "English"}};
    for (auto& [code, name] : found) out.push_back({code, languageName(code)});
    return out;
}

bool setLanguage(const std::string& code) {
    if (code == current || (code == "en" && current.empty())) return true;
    table.clear();
    covered.clear();
    current.clear();
    if (code.empty() || code == "en") return true;
    std::string text;
    if (!catalogText(code, text)) return false;
    for (auto& [id, str] : parsePo(text)) table[id] = str;
    current = code;
    return true;
}

const std::string& language() {
    static const std::string en = "en";
    return current.empty() ? en : current;
}

const char* tr(const char* english) {
    if (!english || table.empty()) return english;
    const auto it = table.find(english);
    if (it == table.end()) return english;
    if (coverage) {
        auto c = covered.find(it->second);
        if (c == covered.end()) c = covered.emplace(it->second, coverage(it->second)).first;
        if (!c->second) return english;
    }
    return it->second.c_str();
}

std::string tr(const std::string& english) { return tr(english.c_str()); }

std::string trf(const char* pattern, std::initializer_list<std::string> args) {
    const std::string p = tr(pattern);
    const std::vector<std::string> a(args);
    std::string out;
    out.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i) { // one pass: an argument's own "{1}" stays as it is
        if (p[i] == '{') {
            size_t j = i + 1;
            while (j < p.size() && p[j] >= '0' && p[j] <= '9') ++j;
            if (j > i + 1 && j < p.size() && p[j] == '}') {
                const size_t k = size_t(std::stoul(p.substr(i + 1, j - i - 1)));
                if (k < a.size()) {
                    out += a[k];
                    i = j;
                    continue;
                }
            }
        }
        out += p[i];
    }
    return out;
}

void setCoverage(std::function<bool(const std::string&)> covers, int generation) {
    if (generation == coverageGeneration && static_cast<bool>(covers) == static_cast<bool>(coverage)) return;
    coverage = std::move(covers);
    coverageGeneration = generation;
    covered.clear();
}

std::string codeForTextLanguage(int textLanguage) {
    return textLanguage >= 0 && textLanguage < 8 ? kTextLanguages[textLanguage] : "en";
}

std::string systemLanguage() {
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    std::string out = "en";
    for (int i = 0; locales && i < count; ++i) {
        const std::string lang = locales[i]->language ? locales[i]->language : "";
        const std::string country = locales[i]->country ? locales[i]->country : "";
        std::string code = lang;
        if (lang == "zh") code = country == "TW" || country == "HK" || country == "MO" ? "zh-Hant" : "zh-Hans";
        if (code == "en") break;
        std::string text;
        if (catalogText(code, text)) {
            out = code;
            break;
        }
    }
    SDL_free(locales);
    return out;
}

} // namespace uitext
