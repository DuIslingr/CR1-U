#pragma once
// HI Corporation LZW decoder (reimplementation of HI_LZW_DecodeByte, see docs/FORMATS.md).
// GIF-style: min-code-size byte, length-prefixed sub-blocks, LSB-first codes up to 12 bits.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cr {

// Decodes the stream starting at data[pos]. `expected` is a size hint (0 = unknown).
// Returns false on malformed input. On success *end receives the offset past the stream.
bool lzwDecode(const uint8_t* data, size_t size, size_t pos, std::vector<uint8_t>& out,
               size_t expected = 0, size_t* end = nullptr);

} // namespace cr
