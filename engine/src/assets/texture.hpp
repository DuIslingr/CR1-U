#pragma once
// MCT texture container (see docs/FORMATS.md). Every pixel format is decoded to
// RGBA8 on the CPU, since PVRTC and GL ES 1 paletted formats are not portable.

#include <cstdint>
#include <string>
#include <vector>

namespace cr {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // top row first, 4 bytes per pixel
    bool alphaOnly = false;    // GL_ALPHA source: texture has no colour of its own
    bool mipmaps = false;      // upload with a mip chain (HD textures: minified, they'd shimmer)
    int logicalW = 0, logicalH = 0; // the size the game lays it out at, when larger (HD): 0 = its own
    bool cutout = false;       // (HD) the original's alpha all 0 or 255: drawn as a cut-out, not blended
    // How the file kept it (for tools/hd_textures.py, through the .crraw): 0 unknown, 1 lossless (8 bits
    // a channel, or 8-bit palettes / L8 / A8 / LA88 - the Vita's all), 2 PVRTC, 3 16-bit colour (RGB565,
    // RGBA5551, RGBA4444 and palettes of them: dithered or banded)
    enum Storage : uint8_t { Unknown, Lossless, Pvrtc, Bits16 } storage = Unknown;
};

struct MctInfo {
    int texId = 0;
    int width = 0;
    int height = 0;
    int format = 0;
    bool compressed = false;
};

const char* mctFormatName(int format);

// Parses and decodes an MCT file held in memory. On failure returns false and sets *error.
bool decodeMct(const std::vector<uint8_t>& file, Image& out, MctInfo* info = nullptr,
               std::string* error = nullptr);

} // namespace cr
