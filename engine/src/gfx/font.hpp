#pragma once
// TrueType / OpenType text through stb_truetype, for the game's dialogue in every language
// (the iOS game draws its text with the system font; font.mct only holds the digits).
// Fonts come from the user's system (Arial / Liberation Sans, Noto Sans CJK, Hiragino, ...), never shipped.

#include <memory>
#include <string>
#include <vector>

#include "assets/texture.hpp"

namespace cr {

class Font {
public:
    Font();
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    // Reads a .ttf / .otf / .ttc; false if it isn't one stb_truetype can use.
    bool load(const std::string& path);
    // The same face as `other` (its file shared, not read again); nothing loaded if it has none.
    // The fallback and the metrics choice stay this font's own.
    void assign(const Font& other);
    bool loaded() const;
    const std::string& path() const { return path_; }
    int generation() const; // changes with the face, its fallback or its line metrics
    // In a collection, the face whose name ends with `region` ("JP", "KR", "SC", "TC");
    // keeps the current face when none does. True if the face changed.
    bool selectRegion(const char* region);
    // The font for the characters this one has no glyph for - as iOS falls back from ArialMT to a
    // Chinese / Korean system face - drawn at the same em on this font's baseline; nullptr: none.
    // Not owned: it must outlive this one.
    void setFallback(const Font* fallback);
    // The line's ascent / descent from the OS/2 table's typographic values instead of hhea's
    // (Noto Sans CJK's 0.88 / 0.12 em are Hiragino's - UIFont's ascender / descender for
    // HiraKakuProN -; its hhea's 1.16 / 0.29 em are much taller)
    void setTypoMetrics(bool on);
    // Whether every character of `utf8` has a glyph in this face or its fallback.
    bool covers(const std::string& utf8) const;
    // White-on-transparent lines: `em` pixels to the em, baselines `lineStep` apart, `pad`
    // pixels of margin; the first baseline the line's ascent below the top margin (as UIKit's
    // drawAtPoint puts a line's top at the point).
    bool rasterise(const std::vector<std::string>& lines, float em, float lineStep, int pad, Image& out) const;

private:
    struct Face;
    std::shared_ptr<const std::vector<unsigned char>> data_;
    std::unique_ptr<Face> face_;
    std::string path_;
    int faceIndex_ = -1;
    int generation_ = 0;
    const Font* fallback_ = nullptr;
    bool typo_ = false;
    bool openFace(int index);
};

// The system's fonts for the game's text (each left unloaded when none is found):
// `latin` a face with Arial's metrics - 2.2.0 draws every language but Japanese in ArialMT
// (Arial on macOS / Windows, Liberation Sans or Arimo on Linux; CR_LATIN_FONT overrides,
// CR_LATIN_FONT=none uses none); `cjk` the first usable font with CJK coverage (CR_FONT
// overrides; CR_FONT=none: no font at all - the 8x8 one); `cjkBold` a bold weight for Japanese
// (2.2.0's HiraKakuProN-W6; CR_FONT_BOLD overrides). True if `latin` or `cjk` loaded.
bool loadSystemFonts(Font& latin, Font& cjk, Font& cjkBold);

} // namespace cr
