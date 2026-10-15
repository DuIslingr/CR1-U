#pragma once
// UTF-8 helpers for the game's text (stored as UTF-16 in the event files).

#include <cstdint>
#include <string>

namespace cr {

void appendUtf8(std::string& s, uint32_t codePoint);
// The code point at `i`, moving `i` past it; U+FFFD for a malformed sequence.
uint32_t nextUtf8(const std::string& s, size_t& i);
// For the 8x8 fallback font: accented Latin letters and typographic punctuation as plain ASCII,
// anything else outside ASCII as '?'.
std::string foldAscii(const std::string& utf8);

} // namespace cr
