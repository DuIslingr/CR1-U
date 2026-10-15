#pragma once
// Vita textures (.gxt in cr1.psarc): an "MCT_" wrapper (9 bytes) around a Sony GXT file -
// "GXT\0", version 0x10000003, texture count, data offset/size, then per texture {data offset,
// size, palette index, flags, type, format, u16 width, u16 height, ...}. The game's are single
// swizzled textures (type 0): Morton order with the y bit lowest, square blocks of the smaller
// side in row order. Formats used: 0x0C001000 (8-bit BGRA), 0x98001000 (8-bit BGR) and, once (the
// warning screen, linear), 0x95005000: P8 with a 1RGB palette, the P8 palettes at the data's end.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/texture.hpp"

namespace cr {

bool decodeGxt(const std::vector<uint8_t>& file, Image& out, std::string* error = nullptr);

} // namespace cr
