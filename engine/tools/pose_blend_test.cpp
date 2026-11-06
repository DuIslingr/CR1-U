// The motion cross-fade (mdl_set_act_motion / mdl_actctl_prc / __mceActionSegment_calcBlendTransform)
// and actions with empty segments past the last node (c111's warp crystal)
#include "scene/pose.hpp"
#include <cmath>
#include <cstdio>

namespace {
bool near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

// One node; a constant translation x and a Z turn (Euler XYZ, turns) keyed at frames 0 and 10
cr::Action action(float tx, float turn, size_t segments = 1) {
    cr::Action a;
    a.duration = 10;
    a.segments.resize(segments);
    cr::Track t, r;
    t.type = 1, t.comps = 3, r.type = 5, r.comps = 3;
    for (float f : {0.0f, 10.0f}) {
        cr::Key k;
        k.time = f, k.mode = 1, k.n = 1;
        k.v[0] = tx;
        t.keys.push_back(k);
        k.v[0] = 0, k.v[2] = turn;
        r.keys.push_back(k);
    }
    a.segments[0] = {t, r};
    return a;
}

float zTurn(const cr::NodePose& p) { return std::atan2(p.cols[0][1], p.cols[0][0]) / 6.28318530718f; }
} // namespace

int main() {
    int bad = 0;
    cr::Skeleton sk;
    sk.nodes.resize(1);
    const std::vector<cr::Action> acts = {action(2, 0), action(6, 0.25f)};
    std::vector<cr::NodePose> p;
    // half and half: translations summed by weight, rotations slerped
    cr::computeBlendedLocalPoses(sk, &acts[0], 0, &acts[1], 0, 0.5f, p);
    bad += !near(p[0].t[0], 4) || !near(zTurn(p[0]), 0.125f) || !near(p[0].s[0], 1);
    // the step sequence: change -> one frame blended (time held at 0), one frame of the new at 0, then on
    cr::MotionBlend mb;
    float time = 3;
    bad += mb.step(0, time, 1, acts) || mb.under(acts) || mb.lag() != 0; // the first action: no blend
    time = 4;
    bad += mb.step(0, time, 1, acts);                   // runs
    time = 0;                                           // (a change: the caller resets the time)
    bad += !mb.step(1, time, 1, acts) || mb.under(acts) != &acts[0] || !near(mb.fromTime, 4) || !near(mb.weight(), 0.5f);
    bad += !mb.step(1, time, 1, acts) || mb.under(acts) || time != 0; // the new alone, still at its first frame
    bad += mb.step(1, time, 1, acts) || !near(mb.lag(), 2);          // then it runs (the caller advances it)
    // sub-frame updates: blended for the first frame's length only
    cr::MotionBlend fine;
    time = 0;
    fine.step(0, time, 0.25f, acts);
    fine.step(1, time, 0.25f, acts);
    int blended = 0, heldUpdates = 0;
    for (int i = 0; i < 12; ++i) {
        if (fine.under(acts)) ++blended;
        if (fine.step(1, time, 0.25f, acts)) ++heldUpdates;
    }
    bad += blended != 4 || heldUpdates != 7;
    // c111: 13 segments for 12 nodes, the last empty - segment i on node i
    cr::Skeleton s12;
    s12.nodes.resize(12);
    cr::Action spin = action(0, 0.5f, 13);
    spin.segments[3] = spin.segments[0], spin.segments[0].clear();
    bad += !cr::emptyTail(s12, spin);
    cr::computeLocalPoses(s12, &spin, 5, p);
    bad += !near(std::fabs(zTurn(p[3])), 0.5f) || !near(zTurn(p[0]), 0); // node 3 turned, node 0 at rest
    cr::Action keyedTail = spin;
    keyedTail.segments[12] = keyedTail.segments[3];
    bad += cr::emptyTail(s12, keyedTail);
    std::printf("pose blend: weighted sum, slerp, 2-frame change, sub-frame steps, empty tail; %d problems\n", bad);
    return bad ? 1 : 0;
}
