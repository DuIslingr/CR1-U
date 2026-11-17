// platform/ui_text: .po parsing, loose catalogs over built-in ones, placeholders, the font fallback
#include "platform/ui_text.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    int problems = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) ++problems, std::fprintf(stderr, "ui_text FAILED: %s\n", what);
    };
    const std::string po = "# a translator's note\n"
                           "msgid \"\"\n"
                           "msgstr \"\"\n"
                           "\"Language: zz\\n\"\n"
                           "\"X-Language-Name: Zedish\\n\"\n"
                           "\n"
                           "#: src/x.cpp:1\n"
                           "msgid \"Menu Language\"\n"
                           "msgstr \"Zed menu\"\n"
                           "\n"
                           "msgid \"\"\n"
                           "\"Two \"\n"
                           "\"lines\\n\"\n"
                           "\"here\"\n"
                           "msgstr \"Zwei\\nzeilen \\\"quoted\\\"\"\n"
                           "\n"
                           "#, fuzzy\n"
                           "msgid \"Fuzzy\"\n"
                           "msgstr \"Not used\"\n"
                           "\n"
                           "msgid \"Untranslated\"\n"
                           "msgstr \"\"\n"
                           "\n"
                           "msgid \"{0} is off here: {1}\"\n"
                           "msgstr \"{1} - {0} ist aus\"\n"
                           "msgid \"Glyphs\"\n"
                           "msgstr \"\xd0\x93\xd0\xbb\xd0\xb8\xd1\x84\"\n";
    std::string name;
    const auto entries = uitext::parsePo(po, &name);
    check(name == "Zedish", "header X-Language-Name");
    check(entries.size() == 4, "fuzzy and untranslated entries left out");
    check(!entries.empty() && entries[0].first == "Menu Language" && entries[0].second == "Zed menu", "plain entry");
    check(entries.size() > 1 && entries[1].first == "Two lines\nhere" && entries[1].second == "Zwei\nzeilen \"quoted\"",
          "continued strings and escapes");

    // a loose catalog in a lang/ folder
    const auto dir = std::filesystem::temp_directory_path() / "cr_ui_text_test";
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "zz.po", std::ios::binary) << po; }
    uitext::addFolder(dir.string());
    bool listed = false;
    for (const auto& l : uitext::languages()) listed |= l.code == "zz" && l.name == "Zedish";
    check(listed, "a loose catalog listed with its name");
    check(uitext::languages().front().code == "en", "English first");
    check(std::string(uitext::tr("Menu Language")) == "Menu Language", "English before a language is set");
    check(uitext::setLanguage("zz") && uitext::language() == "zz", "setLanguage to a loose catalog");
    check(std::string(uitext::tr("Menu Language")) == "Zed menu", "translated");
    check(std::string(uitext::tr("Fuzzy")) == "Fuzzy", "fuzzy shows the English");
    check(std::string(uitext::tr("Untranslated")) == "Untranslated", "untranslated shows the English");
    check(std::string(uitext::tr("Not in the catalog")) == "Not in the catalog", "unknown shows the English");
    check(uitext::trf("{0} is off here: {1}", {"FSR 4", "no GPU"}) == "no GPU - FSR 4 ist aus", "placeholders reordered");
    check(uitext::trf("Nothing to fill {0} {0}", {"x"}) == "Nothing to fill x x", "a placeholder twice");
    check(uitext::trf("Path {0} then {1}", {"a{1}b", "c"}) == "Path a{1}b then c", "an argument's own braces kept");
    // the font fallback: a translation the font can't draw shows the English
    uitext::setCoverage([](const std::string& s) { return s.find("\xd0") == std::string::npos; }, 1);
    check(std::string(uitext::tr("Glyphs")) == "Glyphs", "not covered: English");
    check(std::string(uitext::tr("Menu Language")) == "Zed menu", "covered: translated");
    uitext::setCoverage(nullptr, 2);
    check(std::string(uitext::tr("Glyphs")) == "\xd0\x93\xd0\xbb\xd0\xb8\xd1\x84", "coverage off: translated");
    check(!uitext::setLanguage("no-such") && uitext::language() == "en", "an unknown language: English");
    check(uitext::setLanguage("en") && std::string(uitext::tr("Menu Language")) == "Menu Language", "back to English");
    check(uitext::codeForTextLanguage(5) == "zh-Hans" && uitext::codeForTextLanguage(7) == "ko" && uitext::codeForTextLanguage(9) == "en",
          "text language codes");
    std::filesystem::remove_all(dir);
    if (!problems) std::printf("ui_text: all checks passed\n");
    return problems ? 1 : 0;
}
