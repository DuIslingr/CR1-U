#pragma once
// The party leader walking on a field: FieldPCCtrl::operate / checkHit from the original.
// See docs/FORMATS.md ("Field player").

#include "assets/collision.hpp"
#include "core/math.hpp"

namespace cr {

enum class Gait { Stand = 0, Walk = 1, Run = 2 }; // also the model's action index

struct FieldWalker {
    Vec3 pos;
    float heading = 0;  // radians about +Y; the model faces (sin h, 0, cos h)
    float radius = 5;   // wall sphere radius: stage record +68 when > 0, else 5
    Gait gait = Gait::Stand;
};

// Units per 30 Hz frame (FieldPCCtrl::operate)
constexpr float kWalkSpeed = 0.5f;
constexpr float kRunSpeed = 1.65f;

// Height of the floor under (x, z) that the walker at height `y` stands on: the highest floor
// polygon at most `stepUp` above y. Returns false if there is none.
bool floorHeight(const Collision& col, float x, float y, float z, float stepUp, float& out, int* polygon = nullptr);

// A floor point to start on: where the camera's line of sight meets the floor, else the
// floor point seen closest to the screen centre through `viewProj`, else the centre of the
// largest floor polygon.
bool spawnPoint(const Collision& col, Vec3 eye, Vec3 target, const Mat4& viewProj, Vec3& out);

// One 30 Hz frame. `stick` is the cross-key direction in screen space (x right, y down,
// zero = no input); `camYaw` is atan2(eye.x - target.x, eye.z - target.z).
void stepWalker(FieldWalker& w, const Collision& col, float stickX, float stickY, bool run, float camYaw);

} // namespace cr
