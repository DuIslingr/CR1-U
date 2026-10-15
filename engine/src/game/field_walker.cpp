#include "game/field_walker.hpp"

#include <algorithm>
#include <cmath>

namespace cr {

namespace {

constexpr float kStepUp = 5.0f;       // searchNode probe height in checkHit
constexpr float kBodyHeight = 10.0f;  // party models are 9-12 units tall
constexpr float kPi = 3.14159265f;

bool isWall(const CollisionPolygon& p) { return !p.isFloor() && p.normal[1] > -0.7f; }

Vec3 vertex(const Collision& col, int i) {
    const float* v = &col.vertices[size_t(i) * 3];
    return {v[0], v[1], v[2]};
}

// Height of the polygon's plane at (x, z) if (x, z) lies inside its XZ projection.
bool heightOnPolygon(const Collision& col, const CollisionPolygon& p, float x, float z, float& y) {
    if (std::fabs(p.normal[1]) < 1e-4f) return false;
    bool inside = false;
    for (int i = 0, j = p.count - 1; i < p.count; j = i++) {
        const Vec3 a = vertex(col, p.index[i]), b = vertex(col, p.index[j]);
        if ((a.z > z) != (b.z > z) && x < (b.x - a.x) * (z - a.z) / (b.z - a.z) + a.x) inside = !inside;
    }
    if (!inside) return false;
    const Vec3 a = vertex(col, p.index[0]);
    y = a.y - (p.normal[0] * (x - a.x) + p.normal[2] * (z - a.z)) / p.normal[1];
    return true;
}

Vec3 closestOnSegment(Vec3 p, Vec3 a, Vec3 b) {
    const Vec3 ab = b - a;
    const float l = dot(ab, ab);
    const float t = l > 0 ? std::clamp(dot(p - a, ab) / l, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

// Closest point to `p` on a convex polygon.
Vec3 closestOnPolygon(const Collision& col, const CollisionPolygon& poly, Vec3 p) {
    const Vec3 n{poly.normal[0], poly.normal[1], poly.normal[2]};
    const Vec3 a = vertex(col, poly.index[0]);
    const Vec3 q = p - n * dot(p - a, n);
    bool inside = true;
    for (int i = 0; i < poly.count; ++i) {
        const Vec3 u = vertex(col, poly.index[i]), v = vertex(col, poly.index[(i + 1) % poly.count]);
        if (dot(cross(v - u, q - u), n) < 0) inside = false;
    }
    if (inside) return q;
    Vec3 best = a;
    float bd = 1e30f;
    for (int i = 0; i < poly.count; ++i) {
        const Vec3 c = closestOnSegment(p, vertex(col, poly.index[i]), vertex(col, poly.index[(i + 1) % poly.count]));
        const Vec3 d = c - p;
        if (dot(d, d) < bd) { bd = dot(d, d); best = c; }
    }
    return best;
}

// checkHitWallToSphere: push the body sphere horizontally out of every wall it overlaps.
void pushOutOfWalls(const Collision& col, Vec3& pos, float radius) {
    for (int pass = 0; pass < 3; ++pass) {
        bool moved = false;
        const Vec3 c{pos.x, pos.y + kBodyHeight * 0.5f, pos.z};
        for (const auto& poly : col.polygons) {
            if (!isWall(poly)) continue;
            const Vec3 h = closestOnPolygon(col, poly, c);
            Vec3 d{c.x - h.x, 0, c.z - h.z};
            if (std::fabs(h.y - c.y) > kBodyHeight * 0.5f) continue; // above the head / below the feet
            float len = std::sqrt(dot(d, d));
            if (len >= radius) continue;
            if (len < 1e-4f) { // centre on the wall: push along the wall's horizontal normal
                d = {poly.normal[0], 0, poly.normal[2]};
                len = std::sqrt(dot(d, d));
                if (len < 1e-4f) continue;
                d = d * (1.0f / len);
                len = 0;
            } else {
                d = d * (1.0f / len);
            }
            pos = pos + d * (radius - len);
            moved = true;
        }
        if (!moved) break;
    }
}

// Whether moving the body centre from p to q passes through a wall polygon.
bool crossesWall(const Collision& col, Vec3 p, Vec3 q) {
    p.y += kBodyHeight * 0.5f;
    q.y += kBodyHeight * 0.5f;
    for (const auto& w : col.polygons) {
        if (!isWall(w)) continue;
        const Vec3 n{w.normal[0], w.normal[1], w.normal[2]};
        const Vec3 a = vertex(col, w.index[0]);
        const float dp = dot(p - a, n), dq = dot(q - a, n);
        if ((dp > 0) == (dq > 0)) continue;
        const Vec3 x = p + (q - p) * (dp / (dp - dq));
        bool inside = true;
        for (int i = 0; i < w.count && inside; ++i) {
            const Vec3 u = vertex(col, w.index[i]), v = vertex(col, w.index[(i + 1) % w.count]);
            inside = dot(cross(v - u, x - u), n) >= -1e-4f;
        }
        if (inside) return true;
    }
    return false;
}

} // namespace

bool floorHeight(const Collision& col, float x, float y, float z, float stepUp, float& out, int* polygon) {
    bool found = false;
    for (size_t i = 0; i < col.polygons.size(); ++i) {
        const auto& p = col.polygons[i];
        if (!p.isFloor()) continue;
        float h;
        if (!heightOnPolygon(col, p, x, z, h) || h > y + stepUp) continue;
        if (!found || h > out) {
            out = h;
            if (polygon) *polygon = int(i);
        }
        found = true;
    }
    return found;
}

bool spawnPoint(const Collision& col, Vec3 eye, Vec3 target, const Mat4& viewProj, Vec3& out) {
    const Vec3 dir = target - eye;
    float bestT = 1e30f;
    for (const auto& p : col.polygons) {
        if (!p.isFloor()) continue;
        const Vec3 n{p.normal[0], p.normal[1], p.normal[2]};
        const float den = dot(dir, n);
        if (den >= 0) continue;
        const float t = dot(vertex(col, p.index[0]) - eye, n) / den;
        if (t <= 0 || t >= bestT) continue;
        const Vec3 hit = eye + dir * t;
        float h;
        if (heightOnPolygon(col, p, hit.x, hit.z, h)) { bestT = t; out = {hit.x, h, hit.z}; }
    }
    if (bestT < 1e30f) return true;

    // Otherwise the floor point (polygon centroids and vertices) seen closest to the screen centre
    float best = 1e30f;
    auto consider = [&](Vec3 q) {
        const float* m = viewProj.m;
        const float cw = m[3] * q.x + m[7] * q.y + m[11] * q.z + m[15];
        if (cw <= 0) return;
        const float sx = (m[0] * q.x + m[4] * q.y + m[8] * q.z + m[12]) / cw;
        const float sy = (m[1] * q.x + m[5] * q.y + m[9] * q.z + m[13]) / cw;
        if (std::fabs(sx) > 0.8f || std::fabs(sy) > 0.8f) return;
        const float d = sx * sx + sy * sy;
        if (d < best) { best = d; out = q; }
    };
    for (const auto& p : col.polygons) {
        if (!p.isFloor()) continue;
        Vec3 c{0, 0, 0};
        for (int i = 0; i < p.count; ++i) c = c + vertex(col, p.index[i]);
        c = c * (1.0f / float(p.count));
        consider(c);
        for (int i = 0; i < p.count; ++i) consider(vertex(col, p.index[i]) * 0.8f + c * 0.2f);
    }
    if (best < 1e30f) return true;

    float bestArea = -1;
    for (const auto& p : col.polygons) {
        if (!p.isFloor()) continue;
        Vec3 c{0, 0, 0};
        float area = 0;
        const Vec3 a = vertex(col, p.index[0]);
        for (int i = 0; i < p.count; ++i) c = c + vertex(col, p.index[i]);
        for (int i = 1; i + 1 < p.count; ++i) {
            const Vec3 n = cross(vertex(col, p.index[i]) - a, vertex(col, p.index[i + 1]) - a);
            area += std::sqrt(dot(n, n));
        }
        if (area > bestArea) { bestArea = area; out = c * (1.0f / float(p.count)); }
    }
    return bestArea >= 0;
}

void stepWalker(FieldWalker& w, const Collision& col, float stickX, float stickY, bool run, float camYaw) {
    if (stickX * stickX + stickY * stickY < 1e-6f) {
        w.gait = Gait::Stand;
        return;
    }
    w.gait = run ? Gait::Run : Gait::Walk;
    const float speed = run ? kRunSpeed : kWalkSpeed;
    float h = std::atan2(stickX, stickY) + camYaw; // RepeatPI(atan2(x, y) + camera yaw)
    h = std::remainder(h, 2 * kPi);
    w.heading = h;

    const Vec3 raw{w.pos.x + speed * std::sin(h), w.pos.y, w.pos.z + speed * std::cos(h)};
    Vec3 next = raw;
    pushOutOfWalls(col, next, w.radius);
    // The push can go through a thin or sloped wall at a corner; never pass through one
    if (crossesWall(col, w.pos, next)) {
        next = raw;
        if (crossesWall(col, w.pos, next)) return;
    }
    float y;
    if (!floorHeight(col, next.x, w.pos.y, next.z, kStepUp, y)) return; // no floor there: stay put
    next.y = y;
    w.pos = next;
}

} // namespace cr
