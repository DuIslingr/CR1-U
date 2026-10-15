#include "scene/smooth.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>

namespace cr {

namespace {

Vec3 load3(const std::vector<float>& v, size_t i) { return {v[i * 3], v[i * 3 + 1], v[i * 3 + 2]}; }

uint64_t edgeKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return uint64_t(a) << 32 | b;
}

using StepKey = std::tuple<uint32_t, uint32_t, int>;

// One geometry space with its area-weighted face normals; points by geometric id (rest space)
// or by position id (bind space)
struct Space {
    std::vector<float> pos;
    std::vector<Vec3> faceNormal;
    Vec3 p(uint32_t id) const { return load3(pos, id); }
};

struct Patch {
    Vec3 P[3], e[3][2], c; // corners; edge i (corner i -> i+1): e[i][0] near i, e[i][1] near i+1; centre
};

Vec3 evalPatch(const Patch& pt, const float* w) {
    const float a = w[0], b = w[1], c = w[2];
    return pt.P[0] * (a * a * a) + pt.P[1] * (b * b * b) + pt.P[2] * (c * c * c) + pt.e[0][0] * (3 * a * a * b) +
           pt.e[0][1] * (3 * a * b * b) + pt.e[1][0] * (3 * b * b * c) + pt.e[1][1] * (3 * b * c * c) +
           pt.e[2][0] * (3 * c * c * a) + pt.e[2][1] * (3 * c * a * a) + pt.c * (6 * a * b * c);
}

void smoothMesh(const Model& model, Mesh& m, int L, float cosCrease, SmoothStats& st) {
    const size_t nRender = m.positions.size() / 3;
    const size_t nTri = m.indices.size() / 3;
    if (nRender == 0 || nTri == 0) return;
    for (uint32_t i : m.indices)
        if (i >= nRender) return; // malformed: leave it
    const bool haveBind = m.posOf.size() == nRender && !m.bindPositions.empty();
    const size_t nPos = haveBind ? m.bindPositions.size() / 3 : 0;
    if (haveBind)
        for (uint32_t p : m.posOf)
            if (p >= nPos) return;

    // Geometric ids: render vertices welded by rest position (the topology: duplicate points
    // with their own ids are still one point)
    std::vector<uint32_t> geo(nRender);
    std::vector<float> geoPos;
    {
        std::map<std::array<uint32_t, 3>, uint32_t> weld;
        for (size_t r = 0; r < nRender; ++r) {
            std::array<uint32_t, 3> k;
            std::memcpy(k.data(), &m.positions[r * 3], 12);
            auto it = weld.emplace(k, uint32_t(weld.size()));
            if (it.second) geoPos.insert(geoPos.end(), {m.positions[r * 3], m.positions[r * 3 + 1], m.positions[r * 3 + 2]});
            geo[r] = it.first->second;
        }
    }
    const size_t nGeo = geoPos.size() / 3;
    // Bind space: derived from rest (bind = rest in the attach node's space) where the mesh's
    // rest positions are its bind positions at rest; otherwise smoothed on its own
    const Skeleton& skel = model.skeleton;
    const int attach = m.attachNode;
    bool derivedBind = false;
    auto invertible = [](const Mat4& t) { // not a node collapsed to hide its meshes (scale 0)
        const float* x = t.m;
        const float det = x[0] * (x[5] * x[10] - x[9] * x[6]) - x[4] * (x[1] * x[10] - x[9] * x[2]) +
                          x[8] * (x[1] * x[6] - x[5] * x[2]);
        float n = 0;
        for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10}) n = std::max(n, std::fabs(x[i]));
        return n > 0 && std::fabs(det) > 1e-6f * n * n * n;
    };
    if (haveBind && attach >= 0 && attach < int(skel.restWorld.size()) && attach < int(skel.restWorldInverse.size()) &&
        invertible(skel.restWorld[attach])) {
        float size = 1e-6f;
        for (size_t i = 0; i < m.positions.size(); ++i) size = std::max(size, std::fabs(m.positions[i]));
        derivedBind = true;
        for (size_t r = 0; r < nRender && derivedBind; ++r) {
            const Vec3 v = skel.restWorld[attach].transformPoint(&m.bindPositions[m.posOf[r] * 3]);
            const Vec3 d = v - load3(m.positions, r);
            derivedBind = std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z) < 1e-4f * size;
        }
    }
    const bool bindSpace = haveBind && !derivedBind;

    std::vector<Space> spaces(bindSpace ? 2 : 1);
    spaces[0].pos = geoPos;
    if (bindSpace) spaces[1].pos = m.bindPositions;
    auto corner = [&](size_t si, size_t f, int c) -> uint32_t {
        const uint32_t r = m.indices[f * 3 + size_t(c)];
        return si == 0 ? geo[r] : m.posOf[r];
    };
    for (size_t si = 0; si < spaces.size(); ++si) {
        Space& s = spaces[si];
        s.faceNormal.resize(nTri);
        for (size_t f = 0; f < nTri; ++f) {
            const Vec3 a = s.p(corner(si, f, 0)), b = s.p(corner(si, f, 1)), c = s.p(corner(si, f, 2));
            s.faceNormal[f] = cross(b - a, c - a) * 0.5f;
        }
    }
    // Creases are judged in rest space: unit face normals, faces around each point, faces per edge
    std::vector<Vec3> unitN(nTri);
    for (size_t f = 0; f < nTri; ++f) unitN[f] = normalize(spaces[0].faceNormal[f]);
    std::vector<std::vector<uint32_t>> around(nGeo);
    std::unordered_map<uint64_t, std::vector<uint32_t>> edgeFaces;
    for (size_t f = 0; f < nTri; ++f)
        for (int c = 0; c < 3; ++c) {
            const uint32_t a = corner(0, f, c), b = corner(0, f, (c + 1) % 3);
            around[a].push_back(uint32_t(f));
            edgeFaces[edgeKey(a, b)].push_back(uint32_t(f));
        }
    auto degenerate = [&](uint32_t f) { return dot(spaces[0].faceNormal[f], spaces[0].faceNormal[f]) < 1e-12f; };
    // A face's corner normal at point g (in space si): the faces around g within the crease
    auto cornerNormal = [&](size_t si, uint32_t f, uint32_t g) {
        Vec3 n{0, 0, 0};
        for (uint32_t h : around[g])
            if (!degenerate(h) && dot(unitN[f], unitN[h]) >= cosCrease) n = n + spaces[si].faceNormal[h];
        return normalize(n);
    };
    // An edge curves when exactly two faces share it, smoothly
    auto smoothEdge = [&](uint32_t a, uint32_t b, uint32_t& f1, uint32_t& f2) {
        const auto it = edgeFaces.find(edgeKey(a, b));
        if (it == edgeFaces.end() || it->second.size() != 2) return false;
        f1 = it->second[0];
        f2 = it->second[1];
        return !degenerate(f1) && !degenerate(f2) && dot(unitN[f1], unitN[f2]) >= cosCrease;
    };

    // Output: the original vertices first (unchanged), new ones appended
    Mesh out = m;
    out.indices.clear();
    std::vector<uint32_t> outPos = haveBind ? m.posOf : std::vector<uint32_t>(); // per render vertex
    std::vector<float> outRest = m.positions;                                    // per render vertex
    size_t posCount = nPos;
    const bool skinned = haveBind && m.skinned && m.influenceCount.size() == nPos;
    // A new position (bind point + blended bone weights of the corners `p`, weights `w`)
    auto newPos = [&](Vec3 rest, Vec3 bind, const float* w, const uint32_t* p) {
        const uint32_t id = uint32_t(posCount++);
        if (derivedBind) bind = skel.restWorldInverse[attach].transformPoint(&rest.x);
        out.bindPositions.insert(out.bindPositions.end(), {bind.x, bind.y, bind.z});
        if (skinned) {
            float acc[256] = {};
            for (int c = 0; c < 3; ++c)
                for (int i = 0; i < m.influenceCount[p[c]]; ++i)
                    acc[m.influenceBone[p[c] * 4 + i]] += w[c] * m.influenceWeight[p[c] * 4 + i];
            uint8_t bones[4] = {};
            float weights[4] = {}, total = 0;
            int k = 0;
            for (; k < 4; ++k) { // the heaviest four
                int best = -1;
                for (int b = 0; b < 256; ++b)
                    if (acc[b] > 0 && (best < 0 || acc[b] > acc[best])) best = b;
                if (best < 0) break;
                bones[k] = uint8_t(best);
                weights[k] = acc[best];
                total += acc[best];
                acc[best] = 0;
            }
            out.influenceCount.push_back(uint8_t(k));
            for (int i = 0; i < 4; ++i) {
                out.influenceBone.push_back(bones[i]);
                out.influenceWeight.push_back(i < k && total > 0 ? weights[i] / total : 0.0f);
            }
        }
        return id;
    };
    auto newRender = [&](Vec3 rest, uint32_t pos, const float* w, const uint32_t* r) {
        const uint32_t id = uint32_t(outRest.size() / 3);
        outRest.insert(outRest.end(), {rest.x, rest.y, rest.z});
        if (haveBind) outPos.push_back(pos);
        for (int i = 0; i < 2; ++i) {
            float v = 0;
            for (int c = 0; c < 3; ++c) v += w[c] * (m.uvs.size() >= (r[c] + 1) * 2 ? m.uvs[r[c] * 2 + i] : 0.0f);
            out.uvs.push_back(v);
        }
        for (int i = 0; i < 4; ++i) {
            float v = 0;
            for (int c = 0; c < 3; ++c) v += w[c] * (m.colors.size() >= (r[c] + 1) * 4 ? m.colors[r[c] * 4 + i] : 255.0f);
            out.colors.push_back(uint8_t(std::clamp(std::lround(v), 0L, 255L)));
        }
        return id;
    };
    // Edge points: their geometry once per geometric edge (bit-identical for every face and
    // duplicate on it), positions per position-id pair, render vertices per render pair
    auto stepKey = [&](uint32_t a, uint32_t b, int s) { // s steps from a towards b
        if (a > b) {
            std::swap(a, b);
            s = L - s;
        }
        return StepKey(a, b, s);
    };
    std::map<StepKey, std::array<Vec3, 2>> edgeGeom;
    std::map<StepKey, uint32_t> edgePos, edgeRender;

    std::vector<uint32_t> grid(size_t((L + 1) * (L + 1)));
    for (size_t f = 0; f < nTri; ++f) {
        const uint32_t r[3] = {m.indices[f * 3], m.indices[f * 3 + 1], m.indices[f * 3 + 2]};
        const uint32_t g[3] = {geo[r[0]], geo[r[1]], geo[r[2]]};
        const uint32_t p[3] = {haveBind ? m.posOf[r[0]] : 0, haveBind ? m.posOf[r[1]] : 0, haveBind ? m.posOf[r[2]] : 0};
        Patch patch[2];
        for (size_t si = 0; si < spaces.size(); ++si) {
            Patch& pt = patch[si];
            for (int i = 0; i < 3; ++i) pt.P[i] = spaces[si].p(corner(si, f, i));
            Vec3 sum{0, 0, 0};
            for (int i = 0; i < 3; ++i) {
                const int j = (i + 1) % 3;
                uint32_t f1, f2;
                const Vec3 Pi = pt.P[i], Pj = pt.P[j];
                if (smoothEdge(g[i], g[j], f1, f2)) {
                    // The edge's own normals (both faces' corners averaged): the same from either side
                    const Vec3 Ni = normalize(cornerNormal(si, f1, g[i]) + cornerNormal(si, f2, g[i]));
                    const Vec3 Nj = normalize(cornerNormal(si, f1, g[j]) + cornerNormal(si, f2, g[j]));
                    pt.e[i][0] = (Pi * 2 + Pj - Ni * dot(Pj - Pi, Ni)) * (1.0f / 3);
                    pt.e[i][1] = (Pj * 2 + Pi - Nj * dot(Pi - Pj, Nj)) * (1.0f / 3);
                } else { // crease or border: straight
                    pt.e[i][0] = (Pi * 2 + Pj) * (1.0f / 3);
                    pt.e[i][1] = (Pj * 2 + Pi) * (1.0f / 3);
                }
                sum = sum + pt.e[i][0] + pt.e[i][1];
            }
            const Vec3 E = sum * (1.0f / 6), V = (pt.P[0] + pt.P[1] + pt.P[2]) * (1.0f / 3);
            pt.c = E + (E - V) * 0.5f;
        }
        // Grid vertex (i, j): i steps towards corner 1, j towards corner 2
        for (int i = 0; i <= L; ++i)
            for (int j = 0; i + j <= L; ++j) {
                const int k0 = L - i - j;
                const float w[3] = {float(k0) / L, float(i) / L, float(j) / L};
                uint32_t& v = grid[size_t(i * (L + 1) + j)];
                const int zeros = (k0 == 0) + (i == 0) + (j == 0);
                if (zeros == 2) { // a corner: the original vertex
                    v = k0 ? r[0] : i ? r[1] : r[2];
                    continue;
                }
                if (zeros == 1) { // on an edge: shared with the neighbours
                    int a, b, s;
                    if (j == 0) { a = 0; b = 1; s = i; }
                    else if (k0 == 0) { a = 1; b = 2; s = j; }
                    else { a = 2; b = 0; s = k0; }
                    const StepKey gk = stepKey(g[a], g[b], s);
                    auto git = edgeGeom.find(gk);
                    if (git == edgeGeom.end()) {
                        std::array<Vec3, 2> pts{evalPatch(patch[0], w), bindSpace ? evalPatch(patch[1], w) : Vec3{}};
                        git = edgeGeom.emplace(gk, pts).first;
                    }
                    const Vec3 rest = git->second[0], bind = git->second[1];
                    uint32_t pos = 0;
                    if (haveBind) {
                        const StepKey pk = stepKey(p[a], p[b], s);
                        auto pit = edgePos.find(pk);
                        pos = pit != edgePos.end() ? pit->second : (edgePos[pk] = newPos(rest, bind, w, p));
                    }
                    const StepKey rk = stepKey(r[a], r[b], s);
                    auto rit = edgeRender.find(rk);
                    v = rit != edgeRender.end() ? rit->second : (edgeRender[rk] = newRender(rest, pos, w, r));
                } else { // inside: this face's own
                    const Vec3 rest = evalPatch(patch[0], w), bind = bindSpace ? evalPatch(patch[1], w) : Vec3{};
                    v = newRender(rest, haveBind ? newPos(rest, bind, w, p) : 0, w, r);
                }
            }
        for (int i = 0; i < L; ++i)
            for (int j = 0; i + j < L; ++j) {
                const uint32_t a = grid[size_t(i * (L + 1) + j)], b = grid[size_t((i + 1) * (L + 1) + j)],
                               c = grid[size_t(i * (L + 1) + j + 1)];
                out.indices.insert(out.indices.end(), {a, b, c});
                if (i + j < L - 1) {
                    const uint32_t d = grid[size_t((i + 1) * (L + 1) + j + 1)];
                    out.indices.insert(out.indices.end(), {b, d, c});
                }
            }
    }
    out.positions = std::move(outRest);
    if (haveBind) out.posOf = std::move(outPos);
    st.trianglesBefore += int(nTri);
    st.trianglesAfter += int(out.indices.size() / 3);
    ++st.meshes;
    m = std::move(out);
}

} // namespace

SmoothStats smoothModel(Model& model, int level, float creaseDegrees) {
    SmoothStats st;
    if (level < 2) return st;
    const float cosCrease = std::cos(creaseDegrees * 3.14159265f / 180.0f);
    for (Mesh& m : model.meshes) smoothMesh(model, m, std::min(level, 4), cosCrease, st);
    model.stats.triangles = 0;
    for (const Mesh& m : model.meshes) model.stats.triangles += int(m.indices.size() / 3);
    return st;
}

} // namespace cr
