#pragma once
// HD textures: upscaled replacements for the game's textures, made offline (tools/hd_textures.py,
// Real-ESRGAN) and kept outside the game data. Each is found by the hash of the original's
// decoded pixels, so the same picture in several packages, or the iPad's and the Vita's
// versions of it, each get their own (or share one) without a name table.
//
// <hash>.crraw: an original as exported before .png ("CRRAW1", u32 width, height, flags (bit 0
//               alpha-only; bits 1-2 how the file stored it: Image::Storage), then RGBA8 rows, top first)
// <hash>.png:   an original exported upright (savePngImage), or a replacement made from one
// <hash>.crhd:  its replacement ("CRHD1\0", u32 width, height, original width, original height,
//               flags, zlib-compressed RGBA8 rows, top first)

#include "assets/texture.hpp"

#include <cstdint>
#include <string>

namespace cr {

// 64-bit FNV-1a over the size, the alpha-only flag and the pixels
uint64_t imageHash(const Image& img);
std::string hashName(uint64_t hash); // 16 hex digits

bool saveRawImage(const std::string& path, const Image& img);
// A replacement's size against its original's: at least as large, no side over 8192, and the same
// shape - one scale across and down, each side rounded to whole pixels
bool hdShapeFits(int w, int h, int originalW, int originalH);
// <hash>.png: an original exported for upscaling or editing - upright, the top row first, the
// .crraw's flags in a tEXt chunk ("cr-flags") for tools/hd_textures.py
bool savePngImage(const std::string& path, const Image& img);
// The replacement for `original` in `dir` (with its trailing slash), if there is one -
// <hash>.crhd, else <hash>.png (any upscale or edit, upright) - of a size hdShapeFits; one
// that doesn't fit is left out, `why` saying why
bool loadHdImage(const std::string& dir, const Image& original, Image& out, std::string* why = nullptr);

// A cut-out's see-through pixels (alpha under 250) take the nearest opaque pixel's colour, so
// filtering at its edges doesn't pull in the upscaler's dark fringe
void fillSeeThrough(Image& img);

} // namespace cr
