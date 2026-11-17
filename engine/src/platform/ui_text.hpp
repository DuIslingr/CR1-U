#pragma once
// The port's own interface text - its Options, the first start's setup, the Vita save import - in the
// player's language. The game's own text (menus, messages) comes from the game's data in its eight
// languages; this is only for what the port adds.
//
// English is in the code and is the key. The translations are gettext catalogs, one per language:
// lang/<code>.po in the source tree, built into the program (ui_text_catalogs.cpp, generated), and
// loose ones in a lang/ folder of a game folder or the profile folder, read first - a translation can
// be tried, or corrected, without a rebuild. tools/ui_strings.py lists the strings (lang/template.pot)
// and brings the catalogs up to date.
//
// The same file serves all four ports (CRO-U, CR1-U, CR2-U, CR3-U).
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

// Marks an English string for tools/ui_strings.py where it can't be translated at once (a static table):
// it stays as it is, tr() translates it where it is shown
#define N_(english) english

namespace uitext {

struct Language {
    std::string code; // "en", "ja", "fr", "de", "es", "zh-Hans", "zh-Hant", "ko", "ru", ...
    std::string name; // in itself: "Français", "日本語"
};

// Folders whose <code>.po (a loose catalog) is read in place of the built-in one, the first that has it first
void addFolder(const std::string& dir);
// English first, then every language with a catalog (built in or loose), by code
std::vector<Language> languages();
// The language's name in itself ("" for a code without a catalog)
std::string languageName(const std::string& code);

// "" or "en": English. False (English kept) if there's no catalog for `code`
bool setLanguage(const std::string& code);
const std::string& language();

// The translation of `english` - English where there is none, or where the font can't draw it
const char* tr(const char* english);
std::string tr(const std::string& english);
// tr(pattern) with "{0}", "{1}" ... replaced by `args` (a translation may put them in any order)
std::string trf(const char* pattern, std::initializer_list<std::string> args);

// A translation shown only if `covers` says the font has every glyph of it, else the English. Call
// again (with a new `generation`) when the font changes
void setCoverage(std::function<bool(const std::string&)> covers, int generation);

// The game's text language (0..7: ja, en, fr, de, es, zh-Hans, zh-Hant, ko) as a code
std::string codeForTextLanguage(int textLanguage);
// The system's preferred language with a catalog (SDL_GetPreferredLocales), else "en"
std::string systemLanguage();

// (tests, tools) a catalog's entries from .po text: msgid -> msgstr, untranslated and fuzzy ones left out;
// `name` gets the header's X-Language-Name
std::vector<std::pair<std::string, std::string>> parsePo(const std::string& text, std::string* name = nullptr);

} // namespace uitext
