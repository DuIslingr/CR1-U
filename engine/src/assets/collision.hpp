#pragma once
// Field collision meshes (.cmp) — see docs/FORMATS.md (".cmp — field collision").

#include <cstdint>
#include <string>
#include <vector>

namespace cr {

struct CollisionPolygon {
    float normal[3] = {0, 1, 0};
    int16_t index[4] = {0, 0, 0, 0};
    int count = 3;          // 3 or 4 vertices
    int16_t attr0 = 0;      // probably area / event-zone id
    int16_t attr1 = 0;      // 1 = wall; floors use 0/256/512/768/1024 (surface type?)
    int16_t attr2 = 0;      // always 7 in shipped data
    uint8_t color[4] = {0, 0, 0, 0}; // +28: surface colour; picks the footstep sounds (PlayFootstepSE)
    // What the original takes it for: a KD node's floor list (node +4 count, listed first) or its wall
    // list (+6) - Cmp::CmpBasic::getFloor / checkHitWallToSphere read those, never the normal (seven
    // ramps with normal.y 0.66..0.68 are floors; the origin squares some maps have are neither)
    bool floor = false, wall = false;

    bool isFloor() const { return floor; }
    bool isWall() const { return wall; }
};

// A KD tree node (32 bytes; Cmp::TreeKD::init / searchNode). Every node, inner ones too, lists the
// polygons of its region: Cmp::CmpBase::searchNode picks one and the floor and wall queries read
// only its lists.
struct CollisionNode {
    float split = 0;        // +12: the plane's x or z
    int axis = 0;           // +20: 0 = x, else z (+16: the depth)
    int16_t child[2] = {-1, -1}; // +24: taken when split - p >= 0 (< 0: a leaf), +26: when < 0
    std::vector<int16_t> floors, walls; // the lists counted at +4 / +6 (+8 / +10: none in the data)
};

struct Collision {
    int treeType = 0;                 // 0 = KD tree, 1 = quad tree
    float radius = 0;                 // header f32 (3.0 in every file); searchNode adds it to the distance
    std::vector<float> vertices;      // xyz, world space, Y up
    std::vector<CollisionPolygon> polygons;
    std::vector<CollisionNode> nodes; // node 0 is the root; without a KD tree, one leaf with every floor / wall
    float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
};

// Parses a .cmp file. Fails unless the layout accounts for the whole file
// (allowing the up-to-15 bytes of padding every shipped file has).
bool parseCollision(const std::vector<uint8_t>& file, Collision& out, std::string* error = nullptr);

} // namespace cr
