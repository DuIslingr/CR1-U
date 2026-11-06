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

// Rotation matrix (columns) <-> unit quaternion (w, x, y, z), and the shortest-arc slerp
// (__mceTransform_getRotateQuaternion / setRotateQuaternion)
void toQuat(const float c[3][3], float q[4]) {
    const float m00 = c[0][0], m11 = c[1][1], m22 = c[2][2];
    const float m21 = c[1][2], m12 = c[2][1], m02 = c[2][0], m20 = c[0][2], m10 = c[0][1], m01 = c[1][0];
    const float tr = m00 + m11 + m22;
    if (tr > 0) {
        const float k = std::sqrt(tr + 1) * 2;
        q[0] = 0.25f * k, q[1] = (m21 - m12) / k, q[2] = (m02 - m20) / k, q[3] = (m10 - m01) / k;
    } else if (m00 > m11 && m00 > m22) {
        const float k = std::sqrt(std::fmax(0.0f, 1 + m00 - m11 - m22)) * 2;
        q[0] = (m21 - m12) / k, q[1] = 0.25f * k, q[2] = (m01 + m10) / k, q[3] = (m02 + m20) / k;
    } else if (m11 > m22) {
        const float k = std::sqrt(std::fmax(0.0f, 1 + m11 - m00 - m22)) * 2;
        q[0] = (m02 - m20) / k, q[1] = (m01 + m10) / k, q[2] = 0.25f * k, q[3] = (m12 + m21) / k;
    } else {
        const float k = std::sqrt(std::fmax(0.0f, 1 + m22 - m00 - m11)) * 2;
        q[0] = (m10 - m01) / k, q[1] = (m02 + m20) / k, q[2] = (m12 + m21) / k, q[3] = 0.25f * k;
    }
}

void fromQuat(const float in[4], float c[3][3]) {
    const float l = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2] + in[3] * in[3]);
    const float k = l > 1e-8f ? 1 / l : 0;
    const float w = l > 1e-8f ? in[0] * k : 1, x = in[1] * k, y = in[2] * k, z = in[3] * k;
    c[0][0] = 1 - 2 * (y * y + z * z), c[0][1] = 2 * (x * y + w * z), c[0][2] = 2 * (x * z - w * y);
    c[1][0] = 2 * (x * y - w * z), c[1][1] = 1 - 2 * (x * x + z * z), c[1][2] = 2 * (y * z + w * x);
    c[2][0] = 2 * (x * z + w * y), c[2][1] = 2 * (y * z - w * x), c[2][2] = 1 - 2 * (x * x + y * y);
}

void slerp(const float a[4], const float in[4], float t, float out[4]) {
    float b[4] = {in[0], in[1], in[2], in[3]};
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) d = -d, b[0] = -b[0], b[1] = -b[1], b[2] = -b[2], b[3] = -b[3];
    float wa = 1 - t, wb = t;
    if (d < 0.9995f) {
        const float th = std::acos(std::fmin(d, 1.0f)), sn = std::sin(th);
        wa = std::sin((1 - t) * th) / sn, wb = std::sin(t * th) / sn;
    }
    for (int i = 0; i < 4; ++i) out[i] = wa * a[i] + wb * b[i];
}

} // namespace

// Segments past the skeleton's nodes that key nothing (c111's warp crystal: 13 segments for 12
// nodes, its spin on segment 3 = node 3): segment i still animates node i
bool emptyTail(const Skeleton& skel, const Action& action) {
    if (skel.nodes.empty() || action.segments.size() <= skel.nodes.size()) return false;
    for (size_t i = skel.nodes.size(); i < action.segments.size(); ++i)
        if (!action.segments[i].empty()) return false;
    return true;
}

bool actionFits(const Skeleton& skel, const Action& action) {
    return !skel.nodes.empty() && action.segments.size() == skel.nodes.size();
}

bool actionNearlyFits(const Skeleton& skel, const Action& action) {
    const long d = long(skel.nodes.size()) - long(action.segments.size());
    return !action.segments.empty() && d >= 1 && d <= 4;
}

void computeLocalPoses(const Skeleton& skel, const Action* action, float time, std::vector<NodePose>& out) {
    const size_t n = skel.nodes.size();
    out.resize(n);
    // An action animates the first nodes in order (13 effects carry one or a few nodes more than
    // their action has segments, as do a few models' motions; those stay at rest) - or all of them,
    // when its segments past the last node key nothing
    const bool animate = action && !action->segments.empty() && (action->segments.size() <= n || emptyTail(skel, *action));
    for (size_t i = 0; i < n; ++i) {
        const SkeletonNode& node = skel.nodes[i];
        NodePose& p = out[i];
        std::memcpy(p.cols, node.axes, sizeof p.cols);
        std::memcpy(p.t, node.translation, sizeof p.t);
        std::memcpy(p.s, node.scale, sizeof p.s);
        if (animate && i < action->segments.size()) {
            for (const Track& tr : action->segments[i]) {
                float v[4];
                if (tr.comps != 3 || !evaluateTrack(tr, time, v)) continue;
                if (tr.type == 1) std::memcpy(p.t, v, sizeof p.t);
                else if (tr.type >= 5 && tr.type <= 10) euler(tr.type, v, p.cols);
                else if (tr.type == 11) std::memcpy(p.s, v, sizeof p.s);
            }
        }
    }
}

// __mceActionSegment_calcBlendTransform: per node, from the actions that have its segment - the keyed
// translations and scales summed by weight (a scale left unkeyed counts as 1, once any action keys one),
// the rotations as quaternions, the first action's taken and each later one slerped in by its weight;
// whatever no action keys stays at rest
void computeBlendedLocalPoses(const Skeleton& skel, const Action* from, float fromTime, const Action* to, float toTime,
                              float toWeight, std::vector<NodePose>& out) {
    const size_t n = skel.nodes.size();
    out.resize(n);
    const Action* acts[2] = {from, to};
    const float times[2] = {fromTime, toTime}, weights[2] = {1 - toWeight, toWeight};
    for (size_t i = 0; i < n; ++i) {
        const SkeletonNode& node = skel.nodes[i];
        NodePose& p = out[i];
        std::memcpy(p.cols, node.axes, sizeof p.cols);
        std::memcpy(p.t, node.translation, sizeof p.t);
        std::memcpy(p.s, node.scale, sizeof p.s);
        float t[3] = {0, 0, 0}, s[3] = {0, 0, 0}, q[4] = {1, 0, 0, 0};
        bool anyT = false, anyR = false, anyS = false, scaled[2] = {false, false}, used[2] = {false, false};
        int k = 0; // the actions taking part so far
        for (int a = 0; a < 2; ++a) {
            const Action* act = acts[a];
            if (!act || act->segments.empty() || (act->segments.size() > n && !emptyTail(skel, *act)) || i >= act->segments.size())
                continue;
            used[a] = true;
            for (const Track& tr : act->segments[i]) {
                float v[4];
                if (tr.comps != 3 || !evaluateTrack(tr, times[a], v)) continue;
                if (tr.type == 1) {
                    for (int c = 0; c < 3; ++c) t[c] += weights[a] * v[c];
                    anyT = true;
                } else if (tr.type == 11) {
                    for (int c = 0; c < 3; ++c) s[c] += weights[a] * v[c];
                    anyS = scaled[a] = true;
                } else if (tr.type >= 5 && tr.type <= 10) {
                    float cols[3][3], qa[4];
                    euler(tr.type, v, cols);
                    toQuat(cols, qa);
                    if (k == 0) std::memcpy(q, qa, sizeof q);
                    else slerp(q, qa, weights[a], q);
                    anyR = true;
                }
            }
            ++k;
        }
        if (anyT) std::memcpy(p.t, t, sizeof p.t);
        if (anyR) fromQuat(q, p.cols);
        if (anyS) {
            for (int a = 0; a < 2; ++a)
                if (used[a] && !scaled[a])
                    for (int c = 0; c < 3; ++c) s[c] += weights[a];
            std::memcpy(p.s, s, sizeof p.s);
        }
    }
}

void computeBlendedWorld(const Skeleton& skel, const Action* from, float fromTime, const Action* to, float toTime, float toWeight,
                         std::vector<Mat4>& world) {
    std::vector<NodePose> poses;
    computeBlendedLocalPoses(skel, from, fromTime, to, toTime, toWeight, poses);
    std::vector<Mat4> local(poses.size());
    for (size_t i = 0; i < poses.size(); ++i) local[i] = localMatrix(poses[i].cols, poses[i].t, poses[i].s);
    resolveWorld(skel, local, world);
}

bool MotionBlend::step(int action, float& time, float frames, const std::vector<Action>& actions) {
    if (action != lastAction) {
        const bool blend = lastAction >= 0 && lastAction < int(actions.size()) && action >= 0 && action < int(actions.size());
        from = blend ? lastAction : -1, fromTime = lastTime, since = blend ? 0 : kFrames, changed = blend;
        lastAction = action;
        if (blend) time = 0;
        lastTime = time;
        return blend;
    }
    if (since >= kFrames) {
        lastTime = time;
        return false;
    }
    // mdl_actctl_prc: the change's frame (k = 1) the old motion under the new, which holds its first
    // frame; the next (k = kFrames) the new alone, still at its first frame; then it runs
    const float d = from >= 0 && from < int(actions.size()) ? actions[size_t(from)].duration : 0;
    fromTime += frames;
    if (d > 0 && fromTime >= d) fromTime = std::fmod(fromTime, d);
    since += frames;
    if (since >= kFrames) {
        from = -1;
        lastTime = time;
        return false;
    }
    time = 0;
    lastTime = time;
    return true;
}

void computeWorld(const Skeleton& skel, const Action* action, float time, std::vector<Mat4>& world) {
    std::vector<NodePose> poses;
    computeLocalPoses(skel, action, time, poses);
    std::vector<Mat4> local(poses.size());
    for (size_t i = 0; i < poses.size(); ++i) local[i] = localMatrix(poses[i].cols, poses[i].t, poses[i].s);
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
