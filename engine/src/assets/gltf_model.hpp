#pragma once
// The model bridge: a model package as glTF 2.0 for editing (Blender and the like), and a glTF
// model in place of the package's meshes. See docs/ENGINE.md ("Replacement models").
//
// Export (`--export-models`): one .glb per package - the skeleton's nodes ("node_<i>", their rest
// transforms), every mesh in its rest pose ("mesh_<i>", "_hidden" for the game's hidden helpers)
// skinned to them (inverse bind matrices = the rest pose's), one material per appearance
// ("cr_mat_<i>", unlit, its texture as "tex_<k>") and the model's motions sampled at the game's
// 20 frames a second (only the channels that move).
//
// Replacement (models/hd/<package name minus .bin>.glb or .gltf): its meshes stand in for the
// package's; the package keeps its skeleton, motions, materials' render states and records. Joints
// are matched to the skeleton by name ("node_<i>"); a joint that isn't one goes with its nearest
// ancestor that is, and a mesh without a skin rides on its node's. A material named "cr_mat_<i>"
// takes appearance i's render state and texture - unless it carries a picture of its own (any
// image not named "tex_<k>"), which replaces the texture; other materials take the first
// appearance's state. Only triangle lists, JOINTS_0 / WEIGHTS_0, TEXCOORD_0 and COLOR_0 are read.

#include <string>
#include <vector>

#include "assets/animation.hpp"
#include "assets/model.hpp"

namespace cr {

struct GltfExportStats {
    int meshes = 0, vertices = 0, triangles = 0, images = 0, animations = 0, channels = 0;
    size_t bytes = 0;
};

// Writes `model` and `actions` as a .glb at `path`
bool exportGlb(const Model& model, const std::vector<Action>& actions, const std::string& path, std::string* error = nullptr,
               GltfExportStats* stats = nullptr);

struct GltfImportStats {
    int meshes = 0, vertices = 0, triangles = 0, newTextures = 0, unmatchedJoints = 0;
};

// Replaces `model`'s meshes with the glTF file's (see above); false (the model untouched) when it
// can't be read or holds no triangles
bool applyGltfReplacement(Model& model, const std::string& path, std::string* error = nullptr, GltfImportStats* stats = nullptr);

} // namespace cr
