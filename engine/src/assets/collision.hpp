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

    bool isFloor() const { return normal[1] > 0.7f; }
};

struct Collision {
    int treeType = 0;                 // 0 = KD tree, 1 = quad tree
    float radius = 0;                 // header f32 (3.0 in every file)
    std::vector<float> vertices;      // xyz, world space, Y up
    std::vector<CollisionPolygon> polygons;
    float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
};

// Parses a .cmp file. Fails unless the layout accounts for the whole file
// (allowing the up-to-15 bytes of padding every shipped file has).
bool parseCollision(const std::vector<uint8_t>& file, Collision& out, std::string* error = nullptr);

} // namespace cr
