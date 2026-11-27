#include "gfx/font_outline.hpp"

#include <cstdint>
#include <iostream>

namespace {

uint8_t alphaAt(const cr::Image& image, int x, int y) {
    return image.rgba[(size_t(y) * size_t(image.width) + size_t(x)) * 4 + 3];
}

bool fail(const char* message) {
    std::cerr << message << '\n';
    return false;
}

} // namespace

int main() {
    cr::Image glyph;
    glyph.width = glyph.height = 7;
    glyph.rgba.assign(7u * 7u * 4u, 0);
    const size_t solid = (3u * 7u + 3u) * 4u;
    glyph.rgba[solid] = glyph.rgba[solid + 1] = glyph.rgba[solid + 2] = 255;
    glyph.rgba[solid + 3] = 255;
    const size_t antialias = (3u * 7u + 4u) * 4u;
    glyph.rgba[antialias] = glyph.rgba[antialias + 1] = glyph.rgba[antialias + 2] = 255;
    glyph.rgba[antialias + 3] = 128;

    cr::Image edge;
    if (!cr::makeGlyphOutline(glyph, 2, edge)) return fail("outline generation failed") ? 0 : 1;
    if (edge.width != glyph.width || edge.height != glyph.height) return fail("outline dimensions changed") ? 0 : 1;
    if (alphaAt(edge, 3, 3) != 0) return fail("outline filled the glyph interior") ? 0 : 1;
    if (alphaAt(edge, 4, 3) != 127) return fail("antialiased edge coverage was not preserved") ? 0 : 1;
    if (alphaAt(edge, 5, 3) != 255) return fail("outline did not reach the requested radius") ? 0 : 1;
    if (alphaAt(edge, 5, 5) != 0) return fail("outline exceeded its circular radius") ? 0 : 1;
    // The antialiased source pixel at (4, 3) is exactly two pixels from (6, 3),
    // so its 128 coverage must extend to that radius; the diagonal is outside it.
    if (alphaAt(edge, 6, 3) != 128) return fail("antialiased coverage did not reach the outline radius") ? 0 : 1;
    if (alphaAt(edge, 6, 5) != 0) return fail("outline extended beyond its circular radius") ? 0 : 1;
    return 0;
}
