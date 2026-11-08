#include "game/field_walker.hpp"

#include <algorithm>
#include <cmath>

namespace cr {

namespace {

constexpr float kPi = 3.14159265f;

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

// ---------------------------------------------------------------------------------------------
// FieldPCCtrl::update / checkHit (CR1 2.2.0 0x43450 / 0x4272c) and the queries they make.

namespace {

constexpr float kWallHeight = 10.0f; // UNIT_WIDTH (2.2.0 *0xbc1d4): CmpBasic's sphere sits at the floor + 10 * 0.5
constexpr float kBodyHeight = 5.0f;  // BLOCK_HEIGHT (*0xbc1d8): RectWrapper's at the floor + 5 * 0.5
constexpr float kFloorRange = 1000.0f;
constexpr float kNodeRadius = 5.0f;
constexpr int kWallMask = 2;         // checkHit's mask: polygons with attr2 & 2 (all shipped walls: 7)

bool nearZero(float v) { return v < 1e-4f && v > -1e-4f; } // IsNearZero (2.2.0 0x3bd18)

// |(int)v| as a float - the vcvt.s32 / abs / vcvt.f32 the queries compare with (ARM saturates)
float truncAbs(float v) {
    if (std::isnan(v)) return 0;
    const float a = std::fabs(v);
    return a >= 2147483520.0f ? 2147483520.0f : std::trunc(a);
}

Vec3 vec(const float* v) { return {v[0], v[1], v[2]}; }

// checkInsidePolygon: n . (edge_i x (p - v_i)) never changes sign from one edge to the next
bool insidePolygon(Vec3 p, const Vec3* v, const Vec3* e, int count, Vec3 n) {
    float s[4];
    for (int i = 0; i < count; ++i) s[i] = dot(n, cross(e[i], p - v[i]));
    for (int i = 0; i < count; ++i)
        if (s[i] * s[(i + 1) % count] < 0) return false;
    return true;
}

// checkHitLineToSphere (2.2.0 0x30218): the point of the edge nearest the centre (written while it lies on
// the edge), hit when within r (3D)
bool lineToSphere(Vec3& out, Vec3 a, Vec3 e, Vec3 c, float r) {
    const float l2 = dot(e, e);
    if (nearZero(l2)) return false;
    const float t = dot(c - a, e) / l2;
    if (t < 0 || t > 1) return false;
    out = a + e * t;
    const Vec3 d = out - c;
    return dot(d, d) <= r * r;
}

// checkHitVerticesToSphere: the first corner strictly within r
bool verticesToSphere(Vec3& out, const Vec3* v, int count, Vec3 c, float r) {
    for (int i = 0; i < count; ++i) {
        const Vec3 d = c - v[i];
        if (dot(d, d) < r * r) { out = v[i]; return true; }
    }
    return false;
}

// checkHitPolygonToSphere (2.2.0 0x30378): 0 = the centre's foot on the plane lies inside the polygon (a
// face hit), 1 = an edge within r, 2 = a corner, -1 = none. `contact`: the point touched.
int polygonToSphere(Vec3& contact, const Vec3* v, const Vec3* e, int count, Vec3 n, Vec3 c, float r, float dist) {
    contact = c - n * dist;
    if (insidePolygon(contact, v, e, count, n)) return 0;
    for (int i = 0; i < count; ++i)
        if (lineToSphere(contact, v[i], e[i], c, r)) return 1;
    return verticesToSphere(contact, v, count, c, r) ? 2 : -1;
}

// checkHitVectorToPolygon (2.2.0 0x3048c): whether this frame's move (from c - w to c) crosses the
// polygon - reaching its plane (xz) at a point inside it
bool moveCrosses(const Vec3* v, const Vec3* e, int count, Vec3 n, Vec3 c, Vec3 w) {
    const Vec3 prev = c - w;
    const float num = (v[0].x - prev.x) * n.x + (v[0].z - prev.z) * n.z;
    if (num > 0) return false;
    const float den = w.z * n.z + w.x * n.x;
    if (den > 0 || den > num) return false;
    const Vec3 x = nearZero(den) ? c : prev + w * (num / den);
    return insidePolygon(x, v, e, count, n);
}

// The push an edge or corner contact gives: horizontal, from the contact point to the centre, out to r
// (the normal times r when the centre is on it)
Vec3 edgePush(Vec3 c, Vec3 contact, Vec3 n, float r) {
    const Vec3 d{c.x - contact.x, 0, c.z - contact.z};
    const float len = std::sqrt(dot(d, d));
    if (nearZero(len)) return n * r;
    return d * ((r - len) / len);
}

// A polygon's corners and edges (edge i: corner i to corner i + 1)
int polygonOf(const Collision& col, const CollisionPolygon& p, Vec3* v, Vec3* e) {
    const int count = std::clamp(p.count, 0, 4);
    for (int i = 0; i < count; ++i) v[i] = vertex(col, p.index[i]);
    for (int i = 0; i < count; ++i) e[i] = v[(i + 1) % count] - v[i];
    return count;
}

// Cmp::CmpBasic's standing form (vtable +0x2c): every listed wall within r, either side
bool wallPushStanding(const Collision& col, const CollisionNode& node, Vec3 c, float r, Vec3& out) {
    float ax = 0, az = 0;
    bool face = false, edge = false;
    Vec3 edgeOut;
    for (int16_t idx : node.walls) {
        const CollisionPolygon& p = col.polygons[size_t(idx)];
        if (!(p.attr2 & kWallMask)) continue;
        const Vec3 n = vec(p.normal);
        Vec3 v[4], e[4];
        const int count = polygonOf(col, p, v, e);
        if (count < 3) continue;
        const float dist = dot(c - v[0], n);
        if (truncAbs(dist) >= r) continue;
        Vec3 contact;
        const int k = polygonToSphere(contact, v, e, count, n, c, r, dist);
        if (k == 0) {
            const float px = n.x * (r - dist), pz = n.z * (r - dist);
            ax += px, az += pz;
            c.x += px, c.z += pz;
            face = true;
        } else if ((k == 1 || k == 2) && !edge && !face) {
            edgeOut = edgePush(c, contact, n, r);
            edge = true;
        }
    }
    if (face) out = {ax, 0, az};
    else if (edge) out = edgeOut;
    return face || edge;
}

// RectWrapper::SetupRect: side i of a body - corners i and i + 1 of its turned box, from
// y + offset.y up by the box's height - and its outward normal
void bodySide(const WalkBody& b, int i, Vec3* v, Vec3& n) {
    static const float kLocalNormal[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}}; // RectWrapper::init
    const float hx = b.box[0] * 0.5f, hz = b.box[2] * 0.5f;
    const float lx[4] = {b.offset[0] - hx, b.offset[0] + hx, b.offset[0] + hx, b.offset[0] - hx};
    const float lz[4] = {b.offset[2] - hz, b.offset[2] - hz, b.offset[2] + hz, b.offset[2] + hz};
    const float s = std::sin(b.angle), c = std::cos(b.angle);
    auto turned = [&](float x, float z, float& ox, float& oz) { ox = s * z + c * x, oz = c * z - s * x; };
    const int j = (i + 1) % 4;
    float xi, zi, xj, zj;
    turned(lx[i], lz[i], xi, zi);
    turned(lx[j], lz[j], xj, zj);
    const float y0 = b.pos.y + b.offset[1], y1 = y0 + b.box[1];
    v[0] = {b.pos.x + xi, y0, b.pos.z + zi};
    v[1] = {b.pos.x + xi, y1, b.pos.z + zi};
    v[2] = {b.pos.x + xj, y1, b.pos.z + zj};
    v[3] = {b.pos.x + xj, y0, b.pos.z + zj};
    turned(kLocalNormal[i][0], kLocalNormal[i][1], n.x, n.z);
    n.y = 0;
}

// RectWrapper::checkHitWallToSphere and its standing form (2.2.0 0x3cf40 / 0x3c8e8)
bool bodyPush(const std::vector<WalkBody>& bodies, Vec3 c, float r, Vec3 vel, bool standing, Vec3& out) {
    out = {};
    float ax = 0, az = 0;
    bool face = false, edge = false;
    for (const WalkBody& b : bodies) {
        if (!b.wall || !b.shown || (standing && b.scriptModel)) continue;
        for (int i = 0; i < 4; ++i) {
            Vec3 v[4], n;
            bodySide(b, i, v, n);
            if (!standing && vel.x * n.x + vel.z * n.z > 1e-5f) continue; // facing the move only
            const float dist = dot(c - v[0], n);
            if (std::fabs(dist) >= r) continue;
            Vec3 e[4];
            for (int k = 0; k < 4; ++k) e[k] = v[(k + 1) % 4] - v[k];
            Vec3 contact;
            const int k = polygonToSphere(contact, v, e, 4, n, c, r, dist);
            if (k == 0) {
                const float px = n.x * (r - dist), pz = n.z * (r - dist);
                ax += px, az += pz;
                c.x += px, c.z += pz;
                face = true;
            } else if ((k == 1 || k == 2) && !edge && !face) {
                out = out + edgePush(c, contact, n, r);
                edge = true;
            }
        }
    }
    if (face) out = {ax, 0, az};
    return face || edge;
}

} // namespace

int searchNode(const Collision& col, Vec3 p, float r) {
    if (col.nodes.empty()) return -1;
    int i = 0;
    for (size_t steps = 0; steps <= col.nodes.size(); ++steps) {
        const CollisionNode& n = col.nodes[size_t(i)];
        if (n.child[0] < 0) return i;
        const float d = n.split - (n.axis == 0 ? p.x : p.z);
        if (truncAbs(d) + col.radius < r) return i; // close to the split: this node's lists hold both sides
        const int next = d < 0 ? n.child[1] : n.child[0];
        if (next < 0 || size_t(next) >= col.nodes.size()) return i;
        i = next;
    }
    return i;
}

int getFloor(const Collision& col, int node, Vec3 p, float range, float& height) {
    height = -65535.0f;
    if (node < 0 || size_t(node) >= col.nodes.size()) return -1;
    int best = -1;
    for (int16_t idx : col.nodes[size_t(node)].floors) {
        const CollisionPolygon& poly = col.polygons[size_t(idx)];
        Vec3 v[4];
        const int count = std::clamp(poly.count, 0, 4);
        if (count < 1) continue;
        float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
        for (int i = 0; i < count; ++i) {
            v[i] = vertex(col, poly.index[i]);
            minX = std::min(minX, v[i].x), maxX = std::max(maxX, v[i].x);
            minZ = std::min(minZ, v[i].z), maxZ = std::max(maxZ, v[i].z);
        }
        if (p.x < minX || maxX < p.x || p.z < minZ || maxZ < p.z) continue;
        bool inside = true; // one winding: (p - a) on the inner side of every edge a -> b
        for (int i = 0; i < count && inside; ++i) {
            const Vec3 a = v[i], b = v[(i + 1) % count];
            inside = (p.z - a.z) * (b.x - a.x) - (p.x - a.x) * (b.z - a.z) <= 0;
        }
        if (!inside || nearZero(poly.normal[1])) continue;
        const float h = (poly.normal[0] * (v[0].x - p.x) + poly.normal[2] * (v[0].z - p.z)) / poly.normal[1] + v[0].y;
        if (h > height && truncAbs(p.y - h) <= range) height = h, best = idx;
    }
    return best;
}

bool bodyFloorHeight(const std::vector<WalkBody>& bodies, Vec3 p, float range, float& height) {
    height = -65535.0f;
    bool found = false;
    for (const WalkBody& b : bodies) {
        if (!b.floor || !b.shown) continue;
        const float hx = b.box[0] * 0.5f, hz = b.box[2] * 0.5f;
        const float cx = b.pos.x + b.offset[0], cz = b.pos.z + b.offset[2];
        if (p.x < cx - hx || p.x > cx + hx || p.z < cz - hz || p.z > cz + hz) continue;
        const float top = b.pos.y + b.offset[1] + b.box[1];
        if (std::fabs(p.y - top) > range) continue;
        if (top > height) height = top, found = true;
    }
    return found;
}

bool bodyWallPush(const std::vector<WalkBody>& bodies, Vec3 centre, float radius, Vec3 velocity, Vec3& push) {
    const bool standing = nearZero(velocity.x * velocity.x + velocity.z * velocity.z);
    return bodyPush(bodies, centre, radius, velocity, standing, push);
}

bool wallPush(const Collision& col, int nodeIndex, Vec3 centre, float r, Vec3 vel, Vec3& out, int depth) {
    if (nodeIndex < 0 || size_t(nodeIndex) >= col.nodes.size()) return false;
    const CollisionNode& node = col.nodes[size_t(nodeIndex)];
    if (node.walls.empty() || depth > 1) return false;
    out = {};
    if (nearZero(vel.x * vel.x + vel.z * vel.z)) return wallPushStanding(col, node, centre, r, out);
    Vec3 c = centre, w{vel.x, 0, vel.z};
    const Vec3 prev = centre - vel;
    float ax = 0, az = 0;
    bool face = false, edge = false;
    Vec3 edgeOut;
    for (int16_t idx : node.walls) {
        const CollisionPolygon& p = col.polygons[size_t(idx)];
        if (!(p.attr2 & kWallMask)) continue;
        const Vec3 n = vec(p.normal);
        if (w.x * n.x + w.z * n.z > 0) continue;      // facing the move only
        Vec3 v[4], e[4];
        const int count = polygonOf(col, p, v, e);
        if (count < 3) continue;
        if (dot(prev - v[0], n) < 0) continue;        // the centre started in front of its plane
        const float dist = dot(c - v[0], n);
        if (truncAbs(dist) >= r) continue;
        if (moveCrosses(v, e, count, n, c, w)) {      // went through it: the whole move goes back
            out = out - vel;
            return true;
        }
        Vec3 contact;
        const int k = polygonToSphere(contact, v, e, count, n, c, r, dist);
        if (k == 0) { // the face: out along the normal's x / z (not renormalised); the centre and the move follow
            const float px = n.x * (r - dist), pz = n.z * (r - dist);
            ax += px, az += pz;
            c.x += px, c.z += pz;
            w.x += px, w.z += pz;
            face = true;
        } else if ((k == 1 || k == 2) && !edge && !face) {
            edgeOut = edgePush(c, contact, n, r);
            edge = true;
        }
    }
    if (face) { // a second pass from where the faces put it (vtable +0x30 again, depth + 1)
        out = {ax, 0, az};
        Vec3 more;
        if (wallPush(col, nodeIndex, c, r, w, more, depth + 1)) out = out + more;
        return true;
    }
    if (edge) out = edgeOut;
    return edge;
}

void moveWalker(FieldWalker& w, float stickX, float stickY, bool run, float camYaw) {
    w.velocity = {};
    if (stickX * stickX + stickY * stickY < 1e-6f) {
        w.gait = Gait::Stand;
        return;
    }
    w.gait = run ? Gait::Run : Gait::Walk;
    const float speed = run ? kRunSpeed : kWalkSpeed;
    float h = std::atan2(stickX, stickY) + camYaw; // RepeatPI(atan2(x, y) + camera yaw)
    h = std::remainder(h, 2 * kPi);
    w.heading = h;
    w.velocity = {speed * std::sin(h), 0, speed * std::cos(h)};
    w.pos.x += w.velocity.x;
    w.pos.z += w.velocity.z;
}

float checkWalkerHit(FieldWalker& w, const Collision& col, const std::vector<WalkBody>& bodies, bool pushes) {
    const int node = searchNode(col, w.pos, kNodeRadius);
    if (node < 0) return -1;
    Vec3 p = w.pos;
    float h;
    if (getFloor(col, node, p, kFloorRange, h) >= 0) p.y = h; // none: y stays (x and z have moved)
    if (bodyFloorHeight(bodies, p, kFloorRange, h) && h > p.y) p.y = h;
    Vec3 push;
    if (bodyWallPush(bodies, {p.x, p.y + kBodyHeight * 0.5f, p.z}, w.radius, w.velocity, push) && pushes) {
        p.x += push.x, p.z += push.z;
        w.velocity = w.velocity + push;
    }
    if (wallPush(col, node, {p.x, p.y + kWallHeight * 0.5f, p.z}, w.radius, w.velocity, push) && pushes) {
        p.x += push.x, p.z += push.z;
        w.velocity = w.velocity + push;
    }
    w.pos = p;
    return std::sqrt(dot(w.velocity, w.velocity));
}

float stepWalker(FieldWalker& w, const Collision& col, const std::vector<WalkBody>& bodies, float stickX, float stickY,
                 bool run, float camYaw, bool pushes) {
    moveWalker(w, stickX, stickY, run, camYaw);
    return checkWalkerHit(w, col, bodies, pushes);
}

} // namespace cr
