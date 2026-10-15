#include "assets/collision.hpp"

#include <algorithm>
#include <cstring>

namespace cr {

namespace {

template <typename T>
T rd(const std::vector<uint8_t>& f, size_t pos) {
    T v;
    std::memcpy(&v, f.data() + pos, sizeof v);
    return v;
}

constexpr size_t kHeaderSize = 32, kPolySize = 32;
constexpr size_t kNodeSize[2] = {32, 40}; // KD, QUAD

} // namespace

bool parseCollision(const std::vector<uint8_t>& f, Collision& out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (f.size() < kHeaderSize) return fail("file too small");
    out.treeType = int(rd<uint32_t>(f, 0));
    if (out.treeType != 0 && out.treeType != 1) return fail("unknown tree type");
    const int nVerts = rd<int16_t>(f, 4), nPolys = rd<int16_t>(f, 6), nNodes = rd<int16_t>(f, 8);
    out.radius = rd<float>(f, 12);
    if (nVerts < 0 || nPolys < 0 || nNodes < 0) return fail("negative counts");

    size_t p = kHeaderSize;
    const size_t nodeSize = kNodeSize[out.treeType];
    const size_t fixedEnd = p + size_t(nVerts) * 12 + size_t(nPolys) * kPolySize + size_t(nNodes) * nodeSize;
    if (fixedEnd > f.size()) return fail("truncated");

    out.vertices.resize(size_t(nVerts) * 3);
    std::memcpy(out.vertices.data(), f.data() + p, out.vertices.size() * sizeof(float));
    p += size_t(nVerts) * 12;

    out.polygons.resize(nPolys);
    for (CollisionPolygon& poly : out.polygons) {
        std::memcpy(poly.normal, f.data() + p, 12);
        std::memcpy(poly.index, f.data() + p + 12, 8);
        poly.count = rd<int16_t>(f, p + 20);
        poly.attr0 = rd<int16_t>(f, p + 22);
        poly.attr1 = rd<int16_t>(f, p + 24);
        poly.attr2 = rd<int16_t>(f, p + 26);
        for (int c = 0; c < 4; ++c) poly.color[c] = f[p + 28 + size_t(c)];
        if (poly.count != 3 && poly.count != 4) return fail("polygon vertex count not 3 or 4");
        for (int i = 0; i < poly.count; ++i)
            if (poly.index[i] < 0 || poly.index[i] >= nVerts) return fail("vertex index out of range");
        p += kPolySize;
    }

    // Nodes: each owns sum(4 counts at +4..+10) polygon indices stored after the node array
    size_t lists = 0;
    for (int i = 0; i < nNodes; ++i) {
        const size_t n = p + size_t(i) * nodeSize;
        int sum = 0;
        for (int k = 0; k < 4; ++k) sum += rd<int16_t>(f, n + 4 + 2 * k);
        if (sum > 0) lists += size_t(sum);
    }
    const size_t end = fixedEnd + lists * 2;
    if (end > f.size() || f.size() - end >= 16) return fail("layout does not match file size");

    for (int c = 0; c < 3; ++c) {
        out.boundsMin[c] = 1e30f;
        out.boundsMax[c] = -1e30f;
    }
    for (size_t i = 0; i + 2 < out.vertices.size(); i += 3)
        for (int c = 0; c < 3; ++c) {
            out.boundsMin[c] = std::min(out.boundsMin[c], out.vertices[i + c]);
            out.boundsMax[c] = std::max(out.boundsMax[c], out.vertices[i + c]);
        }
    return true;
}

} // namespace cr
