#pragma once
// Model smoothing (optional, Graphics > Smooth Models): PN triangles (Vlachos et al. 2001) at
// load time, on the CPU. Each triangle becomes level^2 triangles on a cubic patch through its
// corners: the original vertices stay put (feet stay on the floor, joints where they were)
// while silhouettes round out. Normals come from the faces (the game's models have none): a
// corner averages the faces around it that are within `creaseDegrees` of its own, and edges
// sharper than that - and mesh borders - stay straight, so hard edges stay hard and separate
// meshes (head / body) still meet. UVs and colours are interpolated linearly; bone weights are
// blended from the corners, so skinning and animation work as before.

#include "assets/model.hpp"

namespace cr {

struct SmoothStats {
    int meshes = 0, trianglesBefore = 0, trianglesAfter = 0;
};

// Smooths every mesh of `model` in place (level 2 = 4x triangles, 3 = 9x)
SmoothStats smoothModel(Model& model, int level, float creaseDegrees = 60.0f);

} // namespace cr
