#pragma once
// HD textures: upscaled replacements for the game's textures, made offline (tools/hd_textures.py,
// Real-ESRGAN) and kept outside the game data. Each is found by the hash of the original's
// decoded pixels, so the same picture in several packages, or the iPad's and the Vita's
// versions of it, each get their own (or share one) without a name table.
//
// <hash>.crraw: an original as exported for upscaling ("CRRAW1", u32 width, height, flags (bit 0
//               alpha-only; bits 1-2 how the file stored it: Image::Storage), then RGBA8 rows, top first)
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
// The replacement for `original` in `dir` (with its trailing slash), if there is one: same
// original size, larger or equal size
bool loadHdImage(const std::string& dir, const Image& original, Image& out);

} // namespace cr
