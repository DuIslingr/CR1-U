#include "scene/pose.hpp"

#include <cmath>
#include <cstring>

namespace cr {

namespace {

constexpr float kTwoPi = 6.28318530718f;

// Rotation from Euler angles in turns, written as three columns. keyTypes 5..10 are the orders
// XYZ, XZY, YXZ, YZX, ZXY, ZYX (the track table at 0xd0078 -> __mceTransform_setRotateEuler*),
// the first axis turning first: XYZ is R = Rz * Ry * Rx, XZY R = Ry * Rz * Rx (as their matrices).
void euler(int type, const float* turns, float cols[3][3]) {
    static const int kOrder[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; // row-major
    for (int axis : kOrder[type - 5]) { // r = R_axis * r
        const float a = turns[axis] * kTwoPi, c = std::cos(a), sn = std::sin(a);
        const int i = (axis + 1) % 3, j = (axis + 2) % 3; // the plane it turns: i -> j
        for (int k = 0; k < 3; ++k) {
            const float ri = r[i][k], rj = r[j][k];
            r[i][k] = c * ri - sn * rj;
            r[j][k] = sn * ri + c * rj;
        }
    }
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k) cols[c][k] = r[k][c];
}

Mat4 localMatrix(const float cols[3][3], const float* t, const float* s) {
    Mat4 m;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) m.m[c * 4 + r] = cols[c][r] * s[c];
    m.m[12] = t[0];
    m.m[13] = t[1];
    m.m[14] = t[2];
    return m;
}

void resolveWorld(const Skeleton& skel, const std::vector<Mat4>& local, std::vector<Mat4>& world) {
    const size_t n = skel.nodes.size();
    world.assign(n, Mat4{});
    std::vector<uint8_t> state(n, 0); // 0 todo, 1 in progress, 2 done
    std::vector<int> stack;
    for (size_t i = 0; i < n; ++i) {
        if (state[i] == 2) continue;
        // Walk up to the first resolved ancestor, then resolve downwards
        stack.clear();
        for (int j = int(i); j >= 0 && state[j] == 0; j = skel.nodes[j].parent) {
            state[j] = 1;
            stack.push_back(j);
            if (skel.nodes[j].parent >= int(n)) break;
        }
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            const int j = *it, p = skel.nodes[j].parent;
            world[j] = (p >= 0 && p < int(n) && state[p] == 2) ? world[p] * local[j] : local[j];
            state[j] = 2;
        }
    }
}

} // namespace

bool actionFits(const Skeleton& skel, const Action& action) {
    return !skel.nodes.empty() && action.segments.size() == skel.nodes.size();
}

bool actionNearlyFits(const Skeleton& skel, const Action& action) {
    const long d = long(skel.nodes.size()) - long(action.segments.size());
    return !action.segments.empty() && d >= 1 && d <= 4;
}

void computeWorld(const Skeleton& skel, const Action* action, float time, std::vector<Mat4>& world) {
    const size_t n = skel.nodes.size();
    std::vector<Mat4> local(n);
    // An action animates the first nodes in order (13 effects carry one or a few nodes more than
    // their action has segments, as do a few models' motions; those stay at rest)
    const bool animate = action && !action->segments.empty() && action->segments.size() <= n;
    for (size_t i = 0; i < n; ++i) {
        const SkeletonNode& node = skel.nodes[i];
        float cols[3][3], t[3], s[3];
        std::memcpy(cols, node.axes, sizeof cols);
        std::memcpy(t, node.translation, sizeof t);
        std::memcpy(s, node.scale, sizeof s);
        if (animate && i < action->segments.size()) {
            for (const Track& tr : action->segments[i]) {
                float v[4];
                if (tr.comps != 3 || !evaluateTrack(tr, time, v)) continue;
                if (tr.type == 1) std::memcpy(t, v, sizeof t);
                else if (tr.type >= 5 && tr.type <= 10) euler(tr.type, v, cols);
                else if (tr.type == 11) std::memcpy(s, v, sizeof s);
            }
        }
        local[i] = localMatrix(cols, t, s);
    }
    resolveWorld(skel, local, world);
}

void skinMesh(const Model& model, const Mesh& mesh, const std::vector<Mat4>& world, std::vector<float>& out) {
    const Skeleton& skel = model.skeleton;
    const size_t nRender = mesh.posOf.size();
    out.resize(nRender * 3);
    const int attach = mesh.attachNode;
    const bool haveAttach = attach >= 0 && attach < int(world.size());
    const Mat4 attachAnim = haveAttach ? world[attach] : Mat4{};

    // Per-position deformed positions first, then expand to render vertices
    const size_t nPos = mesh.bindPositions.size() / 3;
    std::vector<float> deformed(nPos * 3);
    if (mesh.skinned && haveAttach) {
        // skin(node) = W_anim[node] * W_rest[node]^-1 * W_rest[attach]
        std::vector<Mat4> palette(mesh.boneNodes.size());
        for (size_t b = 0; b < palette.size(); ++b) {
            const int node = mesh.boneNodes[b];
            palette[b] = (node >= 0 && node < int(world.size()))
                             ? world[node] * skel.restWorldInverse[node] * skel.restWorld[attach]
                             : attachAnim;
        }
        for (size_t p = 0; p < nPos; ++p) {
            const float* src = &mesh.bindPositions[p * 3];
            const int k = mesh.influenceCount[p];
            float acc[3] = {0, 0, 0};
            if (k == 0) {
                const Vec3 v = attachAnim.transformPoint(src);
                acc[0] = v.x; acc[1] = v.y; acc[2] = v.z;
            }
            for (int i = 0; i < k; ++i) {
                const int b = mesh.influenceBone[p * 4 + i];
                const float w = mesh.influenceWeight[p * 4 + i];
                const Vec3 v = (b < int(palette.size()) ? palette[b] : attachAnim).transformPoint(src);
                acc[0] += w * v.x; acc[1] += w * v.y; acc[2] += w * v.z;
            }
            std::memcpy(&deformed[p * 3], acc, sizeof acc);
        }
    } else {
        // Rigid: vertices are in the attach node's local space
        for (size_t p = 0; p < nPos; ++p) {
            const Vec3 v = attachAnim.transformPoint(&mesh.bindPositions[p * 3]);
            deformed[p * 3] = v.x; deformed[p * 3 + 1] = v.y; deformed[p * 3 + 2] = v.z;
        }
    }
    for (size_t v = 0; v < nRender; ++v) std::memcpy(&out[v * 3], &deformed[mesh.posOf[v] * 3], 3 * sizeof(float));
}

} // namespace cr
