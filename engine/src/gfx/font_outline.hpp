#pragma once

// Builds a one-pass glyph outline from a rasterised white-on-transparent font image.
// The caller draws this texture once underneath the original glyph; unlike eight offset
// draw calls, overlapping outline pixels are combined before blending.
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "assets/texture.hpp"

namespace cr {

inline bool makeGlyphOutline(const Image& glyph, int radius, Image& out) {
    if (glyph.width <= 0 || glyph.height <= 0 || radius <= 0 || radius > 64) return false;
    const size_t bytes = size_t(glyph.width) * size_t(glyph.height) * 4;
    if (glyph.rgba.size() < bytes) return false;

    out = {};
    out.width = glyph.width;
    out.height = glyph.height;
    out.rgba.assign(bytes, 0);
    const int r2 = radius * radius;

    for (int y = 0; y < glyph.height; ++y) {
        for (int x = 0; x < glyph.width; ++x) {
            const size_t dst = (size_t(y) * size_t(glyph.width) + size_t(x)) * 4;
            const uint8_t face = glyph.rgba[dst + 3];
            uint8_t expanded = face;
            for (int dy = -radius; dy <= radius; ++dy) {
                const int yy = y + dy;
                if (yy < 0 || yy >= glyph.height) continue;
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (dx * dx + dy * dy > r2) continue;
                    const int xx = x + dx;
                    if (xx < 0 || xx >= glyph.width) continue;
                    const size_t src = (size_t(yy) * size_t(glyph.width) + size_t(xx)) * 4 + 3;
                    expanded = std::max(expanded, glyph.rgba[src]);
                }
            }
            const uint8_t edge = expanded > face ? uint8_t(expanded - face) : uint8_t(0);
            out.rgba[dst] = out.rgba[dst + 1] = out.rgba[dst + 2] = 255;
            out.rgba[dst + 3] = edge;
        }
    }
    return true;
}

} // namespace cr
