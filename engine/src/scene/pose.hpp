#pragma once
// Skeleton posing and CPU skinning (see docs/FORMATS.md: TransformTree, Skinning).

#include <vector>

#include "assets/animation.hpp"
#include "assets/model.hpp"
#include "core/math.hpp"

namespace cr {

// A node's local transform as computeWorld composes it (local = T * R * S): its rotation's basis
// columns (unscaled), translation and scale
struct NodePose {
    float cols[3][3];
    float t[3];
    float s[3];
};
// Every node's local transform for `action` at `time` (frames); a null action gives the rest pose
void computeLocalPoses(const Skeleton& skel, const Action* action, float time, std::vector<NodePose>& out);

// _mdl_set_act_motion's cross-fade: every motion change runs both actions for kFrames (2 at every
// call site) with the new one weighted k / kFrames: one frame half and half - the old motion running
// on, the new held at its first frame - then one of the new alone at its first frame; then it runs.
struct MotionBlend {
    static constexpr float kFrames = 2;
    int lastAction = -1;
    float lastTime = 0;
    int from = -1;          // the old action while it blends, else -1
    float fromTime = 0;     // its frame
    float since = kFrames;  // frames since the last change (length: none in progress)
    float length = kFrames; // the change's frames (a script's Anim2 names its own)
    float holdTime = 0;     // the frame the new action holds through the change (Anim2's start frame)
    bool changed = false;   // the current action came in through a change (and lags it)
    // Once per update, before `time` advances by `frames`: notes a change of `action` (its time reset to
    // 0), runs the old motion on, holds `time` at 0 through the change. True while held (don't advance).
    bool step(int action, float& time, float frames, const std::vector<Action>& actions);
    // A change made on purpose (mdl_set_act_motion_loop_frm): `action` from frame `start`, the one
    // before blended under it over `frames` (0: a cut), the same action started over too
    void restart(int action, float start, float frames, const std::vector<Action>& actions) {
        const bool blend = frames > 0 && lastAction >= 0 && lastAction < int(actions.size()) && action >= 0 && action < int(actions.size());
        from = blend ? lastAction : -1, fromTime = lastTime, changed = blend;
        length = blend ? frames : kFrames, holdTime = blend ? start : 0, since = blend ? 0 : length;
        lastAction = action, lastTime = start;
    }
    // The old action to blend under the current one now (the change's frames but the last), else null
    const Action* under(const std::vector<Action>& actions) const {
        return from >= 0 && from < int(actions.size()) && since < length - 1 ? &actions[size_t(from)] : nullptr;
    }
    float weight() const { return std::min(1.0f, (since + 1) / length); } // the new action's, while under() is set
    float lag() const { return !changed ? 0 : since < length ? since : length; } // frames its clock lags the change
    void reset() { *this = MotionBlend(); }
};

// Local poses / world matrices for `from` at `fromTime` and `to` at `toTime` blended with `to` weighted
// `toWeight` (__mceActionSegment_calcBlendTransform)
void computeBlendedLocalPoses(const Skeleton& skel, const Action* from, float fromTime, const Action* to, float toTime,
                              float toWeight, std::vector<NodePose>& out);
void computeBlendedWorld(const Skeleton& skel, const Action* from, float fromTime, const Action* to, float toTime, float toWeight,
                         std::vector<Mat4>& world);

// World matrices of every skeleton node for `action` at `time` (frames).
// A null action gives the rest pose; an action animates its first segments' nodes.
void computeWorld(const Skeleton& skel, const Action* action, float time, std::vector<Mat4>& world);

// True if `action` has more segments than the skeleton has nodes and those past the last node are empty.
bool emptyTail(const Skeleton& skel, const Action& action);
// True if `action` animates this skeleton (one segment per node).
bool actionFits(const Skeleton& skel, const Action& action);
// One to four segments short of it: a package's motions that leave a model's last nodes at rest
// (Escher's gene casts 56 of 58, m103's staged attack 148 of 149). Motions with more segments
// than nodes stay out unless the extra ones are empty (emptyTail; segment i is node i).
bool actionNearlyFits(const Skeleton& skel, const Action& action);

// Deforms `mesh` into world space using node world matrices `world`.
// Writes xyz per render vertex into `out`.
void skinMesh(const Model& model, const Mesh& mesh, const std::vector<Mat4>& world, std::vector<float>& out);

} // namespace cr
