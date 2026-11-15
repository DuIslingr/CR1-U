// cr::androidFonts on made-up Android systems (no device): the order the port tries a phone's fonts in when
// the AOSP files (NotoSansCJK-Regular.ttc, ...) aren't where AOSP puts them.
#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>

#include "gfx/font.hpp"

namespace {
int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) ++failures, std::printf("FAIL: %s\n", what);
}

void put(const std::string& path, const std::string& text = "x") {
    const std::string dir = path.substr(0, path.rfind('/'));
    SDL_CreateDirectory(dir.c_str());
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "wb");
    if (!io) return;
    SDL_WriteIO(io, text.data(), text.size());
    SDL_CloseIO(io);
}

std::string rel(const std::string& root, const std::string& p) { return p.rfind(root, 0) == 0 ? p.substr(root.size()) : p; }

size_t at(const std::vector<std::string>& list, const std::string& root, const char* p) {
    for (size_t i = 0; i < list.size(); ++i)
        if (rel(root, list[i]) == p) return i;
    return size_t(-1);
}

void dump(const char* name, const std::string& root, const std::vector<std::string>& list) {
    std::printf("%s:", name);
    for (const std::string& p : list) std::printf(" %s", rel(root, p).c_str());
    std::printf("\n");
}
} // namespace

int main() {
    const std::string base = "android-fonts-test-root";
    std::filesystem::remove_all(base); // (an earlier run's - only its files, which this test made)

    // AOSP Android 15: font_fallback.xml, variable Roboto (one file at every weight), the CJK collection for
    // Simplified Chinese with the serif collection as its serif stand-in
    {
        const std::string r = base + "/aosp15";
        put(r + "/system/etc/font_fallback.xml", R"(<?xml version="1.0" encoding="utf-8"?>
<familyset>
    <family name="sans-serif" supportedAxes="wght,ital">
        <font weight="100" style="normal">Roboto-Regular.ttf
            <axis tag="wdth" stylevalue="100" />
            <axis tag="wght" stylevalue="100" />
        </font>
        <font weight="400" style="normal">Roboto-Regular.ttf
            <axis tag="wght" stylevalue="400" />
        </font>
        <font weight="400" style="italic">Roboto-Regular.ttf
            <axis tag="ital" stylevalue="1" />
        </font>
    </family>
    <alias name="sans-serif-thin" to="sans-serif" weight="100" />
    <family lang="und-Arab" variant="elegant">
        <font weight="400" style="normal" postScriptName="NotoNaskhArabic">NotoNaskhArabic-Regular.ttf</font>
    </family>
    <family lang="zh-Hans">
        <font weight="400" style="normal" index="2" postScriptName="NotoSansCJKjp-Regular">
            NotoSansCJK-Regular.ttc
            <axis tag="wght" stylevalue="400" />
        </font>
        <font weight="400" style="normal" index="2" fallbackFor="serif"
              postScriptName="NotoSerifCJKjp-Regular">NotoSerifCJK-Regular.ttc
        </font>
    </family>
</familyset>
)");
        for (const char* f : {"Roboto-Regular.ttf", "NotoNaskhArabic-Regular.ttf", "NotoSansCJK-Regular.ttc", "NotoSerifCJK-Regular.ttc"})
            put(r + "/system/fonts/" + f);
        const auto list = cr::androidFonts(r);
        dump("aosp15", r, list);
        check(!list.empty() && rel(r, list[0]) == "/system/fonts/NotoSansCJK-Regular.ttc", "AOSP 15: the CJK collection first");
        check(at(list, r, "/system/fonts/Roboto-Regular.ttf") == 1, "AOSP 15: the sans-serif family next");
        check(at(list, r, "/system/fonts/NotoSerifCJK-Regular.ttc") > 1, "AOSP 15: the serif stand-in only from the folder scan");
    }

    // A phone maker's system: no Noto CJK; its own families in fonts.xml, the CJK one on the product partition;
    // Korean listed before Japanese (Japanese still first), a listed file that isn't there
    {
        const std::string r = base + "/oem";
        put(r + "/system/etc/fonts.xml", R"(<familyset version="23">
<family name="sans-serif"><font weight="400" style="normal">MiSansVF.ttf</font></family>
<family lang="ko"><font weight="400" style="normal">OemKorean.ttf</font></family>
<family lang="zh-Hans zh-Hant"><font weight="400" style="normal">MiSansFallback.ttf</font></family>
<family lang="ja"><font weight="700" style="normal">OemGothic-Bold.ttf</font><font weight="400" style="normal">OemGothic.ttf</font></family>
<family lang="und-Thai"><font weight="400" style="normal">Missing.ttf</font></family>
</familyset>
)");
        put(r + "/system/fonts/MiSansVF.ttf");
        put(r + "/system/fonts/OemKorean.ttf");
        put(r + "/system/fonts/OemGothic.ttf");
        put(r + "/system/fonts/OemGothic-Bold.ttf");
        put(r + "/product/fonts/MiSansFallback.ttf");
        const auto list = cr::androidFonts(r);
        dump("oem", r, list);
        check(!list.empty() && rel(r, list[0]) == "/system/fonts/OemGothic.ttf", "OEM: Japanese's regular weight first");
        check(at(list, r, "/system/fonts/OemGothic-Bold.ttf") == 1, "OEM: Japanese's bold next");
        check(at(list, r, "/product/fonts/MiSansFallback.ttf") == 2, "OEM: Chinese on the product partition, found");
        check(at(list, r, "/system/fonts/OemKorean.ttf") == 3, "OEM: Korean after Chinese");
        check(at(list, r, "/system/fonts/MiSansVF.ttf") == 4, "OEM: the default family after the CJK ones");
    }

    // No font list at all: the folders' files, CJK-named first, sans faces next, no emoji or italics
    {
        const std::string r = base + "/bare";
        for (const char* f : {"HarmonyOS_Sans.ttf", "NotoColorEmoji.ttf", "SourceHanSansCN-Regular.otf", "Roboto-Italic.ttf",
                              "Clock.ttf", "readme.txt"})
            put(r + "/system/fonts/" + f);
        const auto list = cr::androidFonts(r);
        dump("bare", r, list);
        check(!list.empty() && rel(r, list[0]) == "/system/fonts/SourceHanSansCN-Regular.otf", "bare: the CJK-named font first");
        check(at(list, r, "/system/fonts/HarmonyOS_Sans.ttf") == 1, "bare: a sans face next");
        check(at(list, r, "/system/fonts/Clock.ttf") == 2, "bare: the rest last");
        check(at(list, r, "/system/fonts/NotoColorEmoji.ttf") == size_t(-1), "bare: no emoji font");
        check(at(list, r, "/system/fonts/Roboto-Italic.ttf") == size_t(-1), "bare: no italics");
        check(at(list, r, "/system/fonts/readme.txt") == size_t(-1), "bare: fonts only");
    }

    check(cr::androidFonts(base + "/none").empty(), "nothing there: nothing listed");
    std::filesystem::remove_all(base);
    std::printf(failures ? "%d failure(s)\n" : "android fonts: all passed\n", failures);
    return failures ? 1 : 0;
}
