#pragma once
// Skeleton posing and CPU skinning (see docs/FORMATS.md: TransformTree, Skinning).

#include <vector>

#include "assets/animation.hpp"
#include "assets/model.hpp"
#include "core/math.hpp"

namespace cr {

// World matrices of every skeleton node for `action` at `time` (frames).
// A null action gives the rest pose; an action animates its first segments' nodes.
void computeWorld(const Skeleton& skel, const Action* action, float time, std::vector<Mat4>& world);

// True if `action` animates this skeleton (one segment per node).
bool actionFits(const Skeleton& skel, const Action& action);
// One to four segments short of it: a package's motions that leave a model's last nodes at rest
// (Escher's gene casts 56 of 58, m103's staged attack 148 of 149). Motions with more segments
// than nodes (m104's 150 of 148, c109 / c111) stay out: their mapping isn't known.
bool actionNearlyFits(const Skeleton& skel, const Action& action);

// Deforms `mesh` into world space using node world matrices `world`.
// Writes xyz per render vertex into `out`.
void skinMesh(const Model& model, const Mesh& mesh, const std::vector<Mat4>& world, std::vector<float>& out);

} // namespace cr
