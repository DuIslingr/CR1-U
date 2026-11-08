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

    // Nodes: each owns sum(4 counts at +4..+10) polygon indices stored after the node array, in node
    // order - its floors (+4) first, then its walls (+6), then the rest
    size_t lists = 0;
    for (int i = 0; i < nNodes; ++i) {
        const size_t n = p + size_t(i) * nodeSize;
        int sum = 0;
        for (int k = 0; k < 4; ++k) sum += rd<int16_t>(f, n + 4 + 2 * k);
        if (sum > 0) lists += size_t(sum);
    }
    const size_t end = fixedEnd + lists * 2;
    if (end > f.size() || f.size() - end >= 16) return fail("layout does not match file size");
    bool listed = false;
    out.nodes.clear();
    if (out.treeType == 0) out.nodes.resize(size_t(nNodes));
    for (int i = 0, at = 0; i < nNodes; ++i) {
        const size_t n = p + size_t(i) * nodeSize;
        int count[4];
        for (int k = 0; k < 4; ++k) count[k] = std::max(0, int(rd<int16_t>(f, n + 4 + 2 * k)));
        CollisionNode* node = out.treeType == 0 ? &out.nodes[size_t(i)] : nullptr;
        if (node) {
            node->split = rd<float>(f, n + 12);
            node->axis = rd<int32_t>(f, n + 20);
            node->child[0] = rd<int16_t>(f, n + 24);
            node->child[1] = rd<int16_t>(f, n + 26);
            if (node->child[0] >= nNodes || node->child[1] >= nNodes) return fail("KD child out of range");
        }
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < count[k]; ++j, ++at) {
                const int poly = rd<int16_t>(f, fixedEnd + size_t(at) * 2);
                if (poly < 0 || poly >= nPolys || k > 1) continue;
                if (node) (k == 0 ? node->floors : node->walls).push_back(int16_t(poly));
                CollisionPolygon& q = out.polygons[size_t(poly)];
                if (k == 1) q.wall = true;
                else if (q.normal[1] > 0) q.floor = true; // (d0406's one ceiling, listed as a floor, is no ground)
                listed = true;
            }
    }
    if (!listed) // (no lists: by the normal, as before)
        for (auto& poly : out.polygons) poly.floor = poly.normal[1] > 0.7f, poly.wall = !poly.floor && poly.normal[1] > -0.7f;
    if (out.nodes.empty() || !listed) { // (no KD tree: one leaf holding them all)
        out.nodes.assign(1, CollisionNode());
        for (int i = 0; i < nPolys; ++i) {
            if (out.polygons[size_t(i)].floor) out.nodes[0].floors.push_back(int16_t(i));
            if (out.polygons[size_t(i)].wall) out.nodes[0].walls.push_back(int16_t(i));
        }
    }

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
