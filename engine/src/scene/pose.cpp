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

// _mceAnimatable_setFrame_i (121346): a frame at or past the duration wraps for a repeating action (2), and is
// left as it is - the last keys holding - for a clamped one (1); a loop end past the duration runs on to it
static float actionFrame(const Action* a, float t) {
    return a && a->wrap == 2 && a->duration > 0 && t >= a->duration ? std::fmod(t, a->duration) : t;
}

void computeLocalPoses(const Skeleton& skel, const Action* action, float time, std::vector<NodePose>& out) {
    time = actionFrame(action, time);
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
// the rotations as weighted sums of their axes made orthonormal again (FUN_00089d3c);
// whatever no action keys stays at rest
void computeBlendedLocalPoses(const Skeleton& skel, const Action* from, float fromTime, const Action* to, float toTime,
                              float toWeight, std::vector<NodePose>& out) {
    const size_t n = skel.nodes.size();
    out.resize(n);
    const Action* acts[2] = {from, to};
    const float times[2] = {actionFrame(from, fromTime), actionFrame(to, toTime)}, weights[2] = {1 - toWeight, toWeight};
    for (size_t i = 0; i < n; ++i) {
        const SkeletonNode& node = skel.nodes[i];
        NodePose& p = out[i];
        std::memcpy(p.cols, node.axes, sizeof p.cols);
        std::memcpy(p.t, node.translation, sizeof p.t);
        std::memcpy(p.s, node.scale, sizeof p.s);
        float t[3] = {0, 0, 0}, s[3] = {0, 0, 0}, ax[2][3] = {{0, 0, 0}, {0, 0, 0}};
        bool anyT = false, anyR = false, anyS = false, scaled[2] = {false, false}, used[2] = {false, false};
        bool moved[2] = {false, false}, turned[2] = {false, false};
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
                    anyT = moved[a] = true;
                } else if (tr.type == 11) {
                    for (int c = 0; c < 3; ++c) s[c] += weights[a] * v[c];
                    anyS = scaled[a] = true;
                } else if (tr.type >= 5 && tr.type <= 10) {
                    float cols[3][3];
                    euler(tr.type, v, cols);
                    for (int r = 0; r < 2; ++r)
                        for (int c = 0; c < 3; ++c) ax[r][c] += weights[a] * cols[r][c];
                    anyR = turned[a] = true;
                }
            }
        }
        // (FUN_00089d3c 91239-91312: an action taking part that doesn't key the node's translation adds the rest's x its
        // weight; rotations are the weighted sums of the X and Y axes - the rest's for an action not keying it - then
        // X normalised, Z = X x Y normalised, Y = Z x X)
        if (anyT) {
            for (int a = 0; a < 2; ++a)
                if (used[a] && !moved[a])
                    for (int c = 0; c < 3; ++c) t[c] += weights[a] * node.translation[c];
            std::memcpy(p.t, t, sizeof p.t);
        }
        if (anyR) {
            for (int a = 0; a < 2; ++a)
                if (used[a] && !turned[a])
                    for (int r = 0; r < 2; ++r)
                        for (int c = 0; c < 3; ++c) ax[r][c] += weights[a] * node.axes[r][c];
            float* x = ax[0];
            float* y = ax[1];
            if (x[0] == 0 && x[1] == 0 && x[2] == 0 && y[0] == 0 && y[1] == 0 && y[2] == 0) x[0] = y[1] = 1;
            auto norm = [](float* v) {
                const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
            };
            auto cross = [](const float* a, const float* b, float* o) {
                o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
            };
            float z[3], y2[3];
            norm(x);
            cross(x, y, z);
            norm(z);
            cross(z, x, y2);
            for (int c = 0; c < 3; ++c) p.cols[0][c] = x[c], p.cols[1][c] = y2[c], p.cols[2][c] = z[c];
        }
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
        // (the old action at its next frame, as mdl_actctl_prc shows it under the new - not the one already shown)
        const float d = blend ? actions[size_t(lastAction)].duration : 0;
        const float next = lastTime + frames;
        from = blend ? lastAction : -1, fromTime = d > 0 && next >= d ? std::fmod(next, d) : next, since = blend ? 0 : kFrames,
        changed = blend;
        length = kFrames, holdTime = 0;
        lastAction = action;
        if (blend) time = 0;
        lastTime = time;
        return blend;
    }
    if (since >= length) {
        lastTime = time;
        return false;
    }
    // mdl_actctl_prc: the change's frame (k = 1) the old motion under the new, which holds its first
    // frame; the next (k = length) the new alone, still at its first frame; then it runs
    const float d = from >= 0 && from < int(actions.size()) ? actions[size_t(from)].duration : 0;
    fromTime += frames;
    if (d > 0 && fromTime >= d) fromTime = std::fmod(fromTime, d);
    since += frames;
    if (since >= length) {
        from = -1;
        lastTime = time;
        return false;
    }
    time = holdTime;
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
