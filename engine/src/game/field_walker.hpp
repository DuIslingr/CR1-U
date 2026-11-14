#pragma once
// The party leader walking on a field: FieldPCCtrl::update / checkHit from the original (CR1 2.2.0:
// 0x43450 / 0x4272c), with Cmp::CmpBasic's floor and wall queries and RectWrapper's NPC
// bodies. See docs/FORMATS.md ("Field player").

#include <vector>

#include "assets/collision.hpp"
#include "core/math.hpp"

namespace cr {

enum class Gait { Stand = 0, Walk = 1, Run = 2 }; // also the model's action index

struct FieldWalker {
    Vec3 pos;
    float heading = 0;  // radians about +Y; the model faces (sin h, 0, cos h)
    float radius = 5;   // wall sphere radius (+0x24): stage record +68 when > 0, else 5; 4 in puzzle rooms
    Gait gait = Gait::Stand;
    Vec3 velocity;      // +0x30: this frame's move (update), the pushes added (checkHit)
};

// Units per 20 Hz frame (FieldPCCtrl::update)
constexpr float kWalkSpeed = 0.5f;
constexpr float kRunSpeed = 1.65f;

// One NPC as RectWrapper sees it (the NPC table's kind word, the stage record's box)
struct WalkBody {
    Vec3 pos;                 // npc ctl +0x14, live: the record's position, plus its offset for models 0..1
    float offset[3] = {0, 0, 0}; // stage record +40
    float box[3] = {0, 0, 0};    // stage record +28 (x, height, z)
    float angle = 0;          // the angle RectWrapper::init turned the box by (the record's)
    bool wall = false;        // kind bit 2 set, bit 4 clear: its four sides are walls
    bool floor = false;       // kind bit 4: its top is a floor (getFloorHeight)
    bool shown = true;        // _ChkNpcDisp (+0x8c bit 0 clear)
    bool scriptModel = false; // +0x98: its model read by a script (__EvtNpcMdlRead) - CR1's pushes don't read it
};

// Height of the floor under (x, z) that the walker at height `y` stands on: the highest floor
// polygon at most `stepUp` above y. Returns false if there is none. (Placement helpers; the
// walker itself uses getFloor.)
bool floorHeight(const Collision& col, float x, float y, float z, float stepUp, float& out, int* polygon = nullptr);

// A floor point to start on: where the camera's line of sight meets the floor, else the
// floor point seen closest to the screen centre through `viewProj`, else the centre of the
// largest floor polygon.
bool spawnPoint(const Collision& col, Vec3 eye, Vec3 target, const Mat4& viewProj, Vec3& out);

// Cmp::CmpBase::searchNode(pos, r) (2.2.0 0x2dac0 -> the tree's slot +0xc): down the KD tree from node 0, stopping at the
// first node whose split is within r of pos (|trunc(split - p)| + header radius < r); -1 if the
// file has no tree
int searchNode(const Collision& col, Vec3 pos, float r);

// Cmp::CmpBasic::getFloor (2.2.0 0x2dcf8): the node's highest floor polygon under (x, z) within `range`
// of pos.y (|trunc(pos.y - h)| <= range); its index, or -1 (`height` -65535)
int getFloor(const Collision& col, int node, Vec3 pos, float range, float& height);

// RectWrapper::getFloorHeight (2.2.0 0x3c594): the highest top (y + offset.y + height) of a shown floor
// NPC whose unrotated box holds (x, z), within `range` of pos.y
bool bodyFloorHeight(const std::vector<WalkBody>& bodies, Vec3 pos, float range, float& height);

// RectWrapper::checkHitWallToSphere (2.2.0 0x3cf40; standing: 0x3c8e8) / Cmp::CmpBasic::checkHitWallToSphere
// (vtable +0x30; standing: +0x2c): the push that takes the sphere out of the NPC bodies / the node's walls
bool bodyWallPush(const std::vector<WalkBody>& bodies, Vec3 centre, float radius, Vec3 velocity, Vec3& push);
bool wallPush(const Collision& col, int node, Vec3 centre, float radius, Vec3 velocity, Vec3& push, int depth = 0);

// FieldPCCtrl::update: the stick (cross-key direction in screen space, x right, y down, zero = no
// input; `camYaw` = atan2(eye.x - target.x, eye.z - target.z)) turns and moves the leader and sets
// the velocity; zero velocity without input.
void moveWalker(FieldWalker& w, float stickX, float stickY, bool run, float camYaw);

// FieldPCCtrl::checkHit, every frame (standing too): the floor (getFloor 1000, then the floor NPCs),
// then the NPC bodies' and the walls' pushes - applied to the position and the velocity unless
// `pushes` is false (G+0x2c0 bit 0: a script runs). Returns the distance fldencount_walk counts
// (|velocity|), or -1 when there is no node (no collision: nothing done).
float checkWalkerHit(FieldWalker& w, const Collision& col, const std::vector<WalkBody>& bodies, bool pushes = true);

// One 20 Hz frame: moveWalker then checkWalkerHit; returns the latter's distance.
float stepWalker(FieldWalker& w, const Collision& col, const std::vector<WalkBody>& bodies, float stickX, float stickY,
                 bool run, float camYaw, bool pushes = true);
inline float stepWalker(FieldWalker& w, const Collision& col, float stickX, float stickY, bool run, float camYaw) {
    return stepWalker(w, col, {}, stickX, stickY, run, camYaw);
}

} // namespace cr
