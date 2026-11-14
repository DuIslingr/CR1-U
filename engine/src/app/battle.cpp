#include "app/app.hpp"

namespace crapp {

// ---- Battles (btl_main) ----

// The pair's average level (the encounter tables' minimum levels)
int partyLevel(const App& app) {
    const int route = std::clamp(app.playerChar / 2, 0, 3);
    return (app.game.party[size_t(2 * route)].level + app.game.party[size_t(2 * route + 1)].level) / 2;
}

// A system text's first line (sys.evt ids >= 51000), empty if none; `name` fills string 0
std::string sysText(const App& app, int id, const std::string& name) {
    std::vector<std::vector<std::string>> pages;
    if (app.systemText.message(id, app.language, nullptr, pages, nullptr,
                               [&](int n) { return n == 0 || n == 9 ? name : std::string(); }) &&
        !pages.empty() && !pages[0].empty())
        return pages[0][0];
    return std::string();
}

// The package action for battle action slot `slot`: the built-in action numbered slot + 1
// (named "built-in <n>.<m>"); -1 if the package has none

// _btl_anm_load: the package slots a battle fighter's model has - those _btlanmtbl marks
// (_read_mdl_mem_mca2: model * 15 + slot, flat, so past the row's end the next model's), the party's
// 1, 4, 6 and 7 always (_read_mdl_set3); the rest stay empty
void battleSlotsLoaded(LoadedModel& lm, int modelId, bool party, bool six) {
    int count = 0; // (the package's slots: its built-in actions' numbers)
    for (int i = 0; i < lm.builtin && i < int(lm.actions.size()); ++i) {
        const std::string& n = lm.actions[size_t(i)].name;
        if (n.rfind("built-in ", 0) == 0) count = std::max(count, SDL_atoi(n.c_str() + 9));
    }
    lm.slotOn.assign(size_t(std::max(count, 1)), 0);
    const signed char* table = &cr::kBattleAnimTable[0][0];
    const int total = cr::kNumBattleModels * cr::kBattleAnimTableWidth;
    for (int s = 0; s < int(lm.slotOn.size()); ++s) {
        const int at = modelId * cr::kBattleAnimTableWidth + s;
        lm.slotOn[size_t(s)] = (modelId >= 0 && at < total && table[at] != 0) || (party && (s == 1 || s == 4 || s == 6 || s == 7)) ||
                               (six && s == 6);
    }
}

// _mdl_set_act_motion_nextanm: a slot whose motion is 3 frames or less (an empty one, or a 1-frame
// stand-in) passes to the next, round the model's slots, until one has more
int realSlot(const LoadedModel& lm, int slot) {
    int count = 0;
    for (int i = 0; i < lm.builtin && i < int(lm.actions.size()); ++i) {
        const std::string& n = lm.actions[size_t(i)].name;
        if (n.rfind("built-in ", 0) == 0) count = std::max(count, SDL_atoi(n.c_str() + 9));
    }
    for (int k = 0; k < count; ++k) {
        const int s = (slot + k) % count, i = battleAction(lm, s);
        if (i >= 0 && lm.actions[size_t(i)].duration > 3) return s;
    }
    return slot;
}

int battleAction(const LoadedModel& lm, int slot) {
    if (!lm.slotOn.empty() && (slot < 0 || slot >= int(lm.slotOn.size()) || !lm.slotOn[size_t(slot)])) return -1; // (not loaded)
    const std::string want = "built-in " + std::to_string(slot + 1) + ".";
    for (int i = 0; i < lm.builtin && i < int(lm.actions.size()); ++i)
        if (lm.actions[size_t(i)].name.rfind(want, 0) == 0) return i;
    return -1;
}

App::Battle::Actor* battleActor(App& app, int fighter) {
    for (App::Battle::Actor& a : app.battle.actors)
        if (a.fighter == fighter) return &a;
    return nullptr;
}

// Plays action slot `slot` from its start; `then` >= 0 follows it once it ends (else it loops)
void playSlot(App& app, App::Battle::Actor& a, int slot, int then) {
    const LoadedModel& lm = app.battle.models[a.model];
    slot = realSlot(lm, slot);
    a.slot = slot;
    a.action = battleAction(lm, slot);
    if (a.action < 0) { // the model hasn't that one: stand
        a.slot = btlui::kSlotIdle;
        a.action = battleAction(lm, btlui::kSlotIdle);
    }
    a.time = 0;
    a.then = then;
    a.castRest = false;
    a.loopFrom = a.loopTo = -1;
}

float actorDuration(App& app, const App::Battle::Actor& a) {
    const LoadedModel& lm = app.battle.models[a.model];
    return a.action >= 0 && a.action < int(lm.actions.size()) ? lm.actions[size_t(a.action)].duration : 0.0f;
}

// Where the damage numbers start: over the actor's model
cr::Vec3 actorHead(App& app, const App::Battle::Actor& a) {
    // __dmg_set: btlcamera_getpos(actor, 0x44e) - the model's node 1102 (else over its bounds)
    const cr::Vec3 p = actorPoint(app, a, 1102);
    if (p.x != a.pos.x || p.y != a.pos.y || p.z != a.pos.z) return p;
    const LoadedModel& lm = app.battle.models[a.model];
    return a.pos + cr::Vec3{0, (lm.center.y + lm.radius * 0.6f) * a.scale, 0};
}

// btlcamera_set: camera `id` (sys.evt section 15), its values over 9000 relative to fighter `actor`
void setBattleCamera(App& app, int id, int actor, bool held) {
    // _btlcamera_set (2.2.0 0x5fd6c): the camera (+0xa80 / +0xa8e), its actor (+0xa8c), held (+0xa90). A
    // moving (raw[7]) or turning (raw[14]) one starts its count (+0xa88) again over raw[15] (+0xa8a) unless
    // the actor is -98..-1; a turning one given a negative actor makes one step (raw[15] negated, actor 0);
    // a turning one's 10000..10002 is the battle's counter (+0xae0: camHold), 10003 40
    App::Battle& b = app.battle;
    const cr::BattleCamera* c = app.battleData.camera(id);
    if (!c) return;
    const int16_t* r = c->raw;
    b.cam = id;
    b.camActor = actor;
    b.camHeld = held;
    b.camLive = true;
    ++b.camSets;
    if ((r[7] != 0 || r[14] != 0) && (actor >= 0 || actor < -98)) {
        b.camFrame = 0;
        b.camDur = r[15];
        if (r[7] == 0) {
            if (actor < 0) b.camDur = -r[15], b.camActor = 0;
            if (b.camDur >= 10000 && b.camDur <= 10002) b.camDur = b.camHold;
            if (b.camDur == 10003) b.camDur = 40;
        }
    }
}

// _btlcamera_prc (0x6007c, end of btl_main) and btl_prc's FUN_0000b4d8 / b580: the wanted eye and target -
// still (once; any negative actor), turning raw[14] degrees about the target over the count (yaw from
// mceUtil3D_atan2 less a quarter turn, the eye at the whole 3D distance), or moving from the first pair of
// points to the second; the count goes up before it's used. A camera ends when its count is over (a still
// one at once), but 10001 / 10002 hold and 0x48 turns for ever. Unless held, the shown eye and target chase
// the wanted by a fifth a frame (+0xfc / +0x100 = 5.0) for the formation (< 0x0e), victory (0x45..0x47),
// cast (0x49..0x5c) and 0x21b cameras once their count passes 1, and cut (_Clr_evcv) for the rest
void stepBattleCamera(App& app) {
    App::Battle& b = app.battle;
    b.camSnapped = false;
    if (b.camLive) {
        const cr::BattleCamera* c = app.battleData.camera(b.cam);
        if (!c) { b.camLive = false; return; }
        const int16_t* r = c->raw;
        cr::Vec3 base{0, 0, 0};
        if (b.camActor >= 0)
            for (const App::Battle::Actor& a : b.actors) // btlcamera_getpos: its node 1003 (0x3eb) as it moves
                if (a.fighter == b.camActor) base = actorPoint(app, a, 1003);
        auto rel = [](int v, float at) { return v > 9000 ? float(v - 10000) + at : float(v); };
        b.camRoll = float(r[6]); // (+0xf8 = raw[6]; a moving camera's goes on to raw[13] below)
        if ((r[7] == 0 && r[14] == 0) || b.camActor < 0) {
            b.camWantEye = {rel(r[0], base.x), rel(r[1], base.y), rel(r[2], base.z)};
            b.camWantTarget = {rel(r[3], base.x), rel(r[4], base.y), rel(r[5], base.z)};
            b.camLive = false;
        } else if (r[7] == 0) {
            const cr::Vec3 e{rel(r[0], base.x), rel(r[1], base.y), rel(r[2], base.z)};
            const cr::Vec3 t{rel(r[3], base.x), rel(r[4], base.y), rel(r[5], base.z)};
            const cr::Vec3 d = t - e;
            const float dist = std::sqrt(cr::dot(d, d)), flat = std::sqrt(d.x * d.x + d.z * d.z);
            ++b.camFrame;
            const float turn = 2.0f * 3.14159265f;
            float yaw = std::atan2(d.z, d.x) - turn * 0.25f;
            yaw += float(r[14]) * float(b.camFrame) / float(std::max(1, std::abs(b.camDur))) / 360.0f * turn;
            const float pitch = std::atan2(d.y, flat);
            b.camWantTarget = t;
            b.camWantEye = {t.x + std::sin(yaw) * dist, t.y - std::sin(pitch) * dist, t.z - std::cos(yaw) * dist};
            if (b.camFrame >= b.camDur) { // (2.2.0 0x6007c: only 0x48 starts again - 10001 / 10002 end like the rest)
                if (b.cam == 0x48) b.camFrame = 0;
                else b.camLive = false;
            }
        } else {
            ++b.camFrame;
            const float k = b.camDur != 0 ? float(b.camFrame) / float(b.camDur) : 1.0f;
            auto at = [&](int i, float o) { return rel(r[i], o) + (rel(r[i + 7], o) - rel(r[i], o)) * k; };
            b.camWantEye = {at(0, base.x), at(1, base.y), at(2, base.z)};
            b.camWantTarget = {at(3, base.x), at(4, base.y), at(5, base.z)};
            b.camRoll = float(r[6]) + float(r[13] - r[6]) * k;
            if (b.camFrame >= b.camDur) b.camLive = false;
        }
        if (!b.camHeld) {
            const int id = b.cam;
            if (b.camFrame > 1 && (id < 0x0e || (id >= 0x49 && id <= 0x5c) || (id >= 0x45 && id <= 0x47) || id == 0x21b))
                b.camEase = 5;
            else b.camSnapped = true; // (_Clr_evcv: flag 0x800, cleared after the frame)
        }
    }
    if (b.camSnapped) b.camEye = b.camWantEye, b.camTarget = b.camWantTarget;
    b.camEye = b.camEye + (b.camWantEye - b.camEye) * (1.0f / b.camEase);
    b.camTarget = b.camTarget + (b.camWantTarget - b.camTarget) * (1.0f / b.camEase);
    if (SDL_getenv("CR_BATTLE_CAMLOG")) // debugging: the shown camera each battle frame
        std::printf("camlog %d phase %d/%d cam %#x actor %d count %d/%d ease %.0f eye %.3f %.3f %.3f target %.3f %.3f %.3f roll %.1f\n", b.frames,
                    int(b.phase), b.phaseFrames, b.cam, b.camActor, b.camFrame, b.camDur, b.camEase, b.camEye.x, b.camEye.y, b.camEye.z,
                    b.camTarget.x, b.camTarget.y, b.camTarget.z, b.camRoll);
}

// The camera shown this frame: the eased eye and target, and the roll (+0xf8, not eased)
void battleCameraView(App& app, cr::Vec3& eye, cr::Vec3& target, float* roll = nullptr) {
    eye = app.battle.camEye, target = app.battle.camTarget;
    if (roll) *roll = app.battle.camRoll;
}

// The cameras the menus and actions use (menub_mainPrc, btlord_make_select, btlact_*_init)
int monsterSize(const App& app, int fighter) {
    const cr::Fighter& f = app.battle.rules.f[size_t(std::clamp(fighter, 0, 4))];
    return f.def ? f.def->size : 1;
}
// read_btlcpl's per-slot camera index (0xd3220): formation row, enemy slot
int enemyCameraSlot(const App& app, int fighter) {
    static const short kSlot[16][3] = {{0, 0, 0},   {0, 0, 0},    {23, 24, 0},  {25, 0, 0},   {0, 0, 0},    {1, 2, 0},
                                       {3, 4, 5},   {6, 0, 0},    {7, 8, 0},    {9, 10, 11},  {12, 13, 0},  {14, 0, 0},
                                       {15, 16, 0}, {17, 18, 19}, {20, 21, 0},  {22, 0, 0}};
    if (fighter < 2) return 0;
    return kSlot[std::clamp(app.battle.formation, 0, 15)][std::clamp(fighter - 2, 0, 2)];
}

// __menub_mainPrc (12411 / 12572-12579): the target cursor's camera - cut to as the targets come up,
// then, as the cursor moves, eased in by a half a frame (+0xfc / +0x100 = 2.0) and set held
// (btlcamera_set(.., 1): btlcamera_prc leaves the easing alone)
void cameraForTarget(App& app, int target, bool group, bool cursor) {
    App::Battle& b = app.battle;
    if (target < 2) setBattleCamera(app, group ? 0x26 + (b.rules.pair ? 1 : 0) : b.rules.pair ? 30 : 28 + target, 0, cursor); // (_btlord_make_select: a party-wide scope's 0x26 + pair)
    else if (group) setBattleCamera(app, b.formation + 0x26, 2, cursor);
    else {
        const int size = monsterSize(app, target);
        setBattleCamera(app, size == 1 ? target + 0x1d : size + 0x20, target, cursor);
    }
    if (cursor) b.camEase = 2;
}

// _btlact_attack_init (2.2.0 0x15548, 10587-10633): random(3) drawn for every blow (que +0xc); a blow
// at a member (target +0x8e < 2) frames the attacker's camera slot (+0xa8; an enemy pair's 0x19) x 3 +
// 0x1b0 / 0x1b1 by the member hit (the pair's 0x1b2); a blow at a monster the target's slot x 4 +
// the draw + 0x78 for member 0's (+0x84 the attacker), 0xe0 for member 1's (the pair's 0x148)
void cameraForTurn(App& app, const cr::BattleTurn& t) {
    App::Battle& b = app.battle;
    b.attackVariant = -1, b.attackCam2At = -1;
    const int user = t.actors[0], target = firstTarget(t);
    if (t.kind == cr::kCmdAttack && target >= 0) {
        const int r = b.rules.random(3);
        b.attackVariant = r;
        if (target < 2) {
            const int slot = b.rules.enemyPair ? 0x19 : enemyCameraSlot(app, user);
            setBattleCamera(app, slot * 3 + (b.rules.pair ? 0x1b2 : target == 0 ? 0x1b0 : 0x1b1));
        } else
            setBattleCamera(app, r + enemyCameraSlot(app, target) * 4 + (b.rules.pair ? 0x148 : user == 0 ? 0x78 : 0xe0));
        return;
    }
    if (t.kind == cr::kCmdGene || t.kind == cr::kCmdItem) b.camHold = 25; // (gene_init / item_init: +0xae0 = 0x19)
    if (t.kind == cr::kCmdGene || t.kind == cr::kCmdItem || t.kind == cr::kCmdAttack) castCamera(app, t.kind, user);
}

// The camera on a gene's or item's user as it casts (_btlact_gene_init 0x3ef28 / _btlact_item_init
// 0x4033c; _btlact_none_init's is the item's): a member's 0x49 / 0x53 + member (the pair's + 2, actor
// 0), a size-1 monster's + 1 + its slot (an enemy pair's + 5), a bigger one's size + 4
void castCamera(App& app, int kind, int user) {
    const App::Battle& b = app.battle;
    const int first = kind == cr::kCmdItem ? 0x53 : 0x49;
    if (user < 2) setBattleCamera(app, b.rules.pair ? first + 2 : first + user, b.rules.pair ? 0 : user);
    else if (monsterSize(app, user) == 1) setBattleCamera(app, b.rules.enemyPair ? first + 5 : user + first + 1, user);
    else setBattleCamera(app, monsterSize(app, user) + first + 4, user);
}

// _btlact_gene (38345-38411) / _btlact_item (39085-39142) at the hit (state 1000), and state 0x100e
// after a barrier's bounce (38710-38772 / 39009-39071; the users now the targets): a side-wide action
// (scope 3 / 5) frames the first target's side - 0x67 / 0x68 (the pair) for the party, 0x69 / 0x6a (an
// enemy pair) for a size-1 monster, formation + 0x67 else (actor 0); a single target its own - member
// + 0x5d (the pair's 0x5f, actor 0), monster + 0x5e (an enemy pair's 0x62) or size + 0x61; some actions
// stay on the formation's view (formation + 0xc): genes 0xd7-0xd9, 0xde, 0xe8, 0x197, 0x19a, 0x1b1
// (0x100e: not 0x1b1), items 0x76-0x78 (0x100e: 0xd7-0xd9, 0xde, 0xe0, 0xe8, 0x197, 0x19a); model 8's
// gene 0xe4 the formation + 0x67; at 1000 an action on its own user keeps the cast camera
void hitCamera(App& app, const cr::BattleTurn& t, int target, bool bounce) {
    App::Battle& b = app.battle;
    const cr::BattleActionDef* a = app.battleActions.get(t.action);
    if (!a || target < 0) return;
    if (a->scope == 3 || a->scope == 5) {
        const int id = target < 2 ? (b.rules.pair ? 0x68 : 0x67) : monsterSize(app, target) == 1 ? (b.rules.enemyPair ? 0x6a : 0x69) : b.formation + 0x67;
        return setBattleCamera(app, id, 0);
    }
    const int x = t.action;
    const bool item = t.kind == cr::kCmdItem;
    const bool genes = (x >= 0xd7 && x <= 0xd9) || x == 0xde || x == 0xe8 || x == 0x197 || x == 0x19a;
    const bool still = bounce ? genes || (item && x == 0xe0) : item ? x >= 0x76 && x <= 0x78 : genes || x == 0x1b1;
    if (still) return setBattleCamera(app, b.formation + 0xc, 0);
    if (!bounce && !item && x == 0xe4) // (the user's model 8)
        if (const App::Battle::Actor* u = battleActor(app, t.actors[0]); u && u->modelId == 8) return setBattleCamera(app, b.formation + 0x67, 0);
    if (!bounce && target == t.actors[0]) return;
    if (target < 2) {
        if (b.rules.pair) setBattleCamera(app, 0x5f, 0);
        else setBattleCamera(app, target + 0x5d, target);
    } else if (monsterSize(app, target) == 1) setBattleCamera(app, b.rules.enemyPair ? 0x62 : target + 0x5e, target);
    else setBattleCamera(app, monsterSize(app, target) + 0x61, target);
}

// _btlact_attack_init (10777-10822, 10860-10945): where an attacker steps to - the formation table
// (kBattlePosition, PTR_DAT_000c50cc: rows of 12 (x, z)): a member's blow at monster t the formation's
// row, entry 3 + (t - 2) (the pair's 6 + 2(t - 2), member 1 7 + 2(t - 2)); a monster's at member t row
// 0 (with the party's pair row 1), entry 3 + t (an enemy pair's 6 + 2t, its second 7 + 2t), z - 10 for
// a size-3 monster and + 5 for size 2 (the first only). Against the pair a lone monster's x and z are
// both row 1 entry 3 + t's x (the code reads +0x3c twice)
bool attackSpot(const App& app, int user, int k, int target, cr::Vec3& out) {
    const App::Battle& b = app.battle;
    const int formation = std::clamp(b.formation, 0, 15);
    auto at = [](int row, int entry) { return cr::Vec3{float(cr::kBattlePosition[row][entry][0]), 0, float(cr::kBattlePosition[row][entry][1])}; };
    if (user < 2) {
        if (target < 2 || target > 4) return false;
        const int t = target - 2;
        out = !b.rules.pair ? at(formation, 3 + t) : at(formation, 6 + 2 * t + (k == 1 ? 1 : 0));
        return true;
    }
    if (target < 0 || target > 1) return false;
    const int row = b.rules.pair ? 1 : 0;
    if (k == 1) { out = at(row, 7 + 2 * target); return true; }
    if (b.rules.enemyPair) out = at(row, 6 + 2 * target);
    else if (b.rules.pair) out = at(1, 3 + target), out.z = out.x;
    else out = at(0, 3 + target);
    const int size = monsterSize(app, user);
    if (size == 3) out.z -= 10;
    else if (size == 2) out.z += 5;
    return true;
}

// ---- Battle effects (eff3d_*) ----
// eff.bin is an index of 100 (u32 offset, u32 size) entries, each a model package. An effect plays
// its first action once at a point (an actor's origin, or the node of its bone whose record user
// id the table 0xd37c0 names: 1101 the body, 1102 the head, 1003 the root, 1201.. the limbs) and
// is deleted at its end (eff3d_prc). Its materials (diffuse alpha 0 at rest) take their colour
// from the package action their Appearance's refGid names (by gid; segment 1, keyType 12 RGBA).
namespace effects {
constexpr short kNode[100] = {0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                              0,    0,    0,    0,    0,    0,    0,    1204, 1203, 1101, 1101, 1102, 1003, 1101, 0,
                              0,    1101, 1206, 0,    1101, 1101, 0,    0,    1101, 1102, 1101, 1101, 1102, 1101, 1101,
                              1102, 1101, 1101, 1102, 0,    1101, 1101, 1101, 0,    0,    0,    0,    0,    0,    1101,
                              1101, 1101, 1101, 1101, 1101, 1102, 1102, 1102, 0,    1102, 1102, 1102, 1101, 1101, 1101,
                              1101, 1101, 1101, 0,    0,    1101, 1101, 1101, 1101, 1101, 1101, 1101, 1101, 1101, 1101,
                              1101, 1101, 1101, 100,  100,  100,  200,  250,  300,  0};
// What the calcs show (btlcalc_*'s eff3d_set_btl ids)
constexpr int kHit = 0x18, kCritical = 0x19, kFall = 0x1a, kFixed = 0x1c, kSacrifice = 0x22, kGene = 0x23,
              kStatus = 0x24, kDrain = 0x25, kHeal = 0x2f, kRevive = 0x31, kCast = 0x15, kBuff = 0x35, kAnalyze = 0x36;
} // namespace effects

LoadedModel* effectModel(App& app, int id) {
    if (id < 0 || id >= 100) return nullptr;
    if (auto it = app.effectModels.find(id); it != app.effectModels.end()) return it->second.meshes.empty() ? nullptr : &it->second;
    LoadedModel& out = app.effectModels[id];
    if (app.effBin.empty() && !cr::readFile(app.dataDir + "eff.bin", app.effBin)) return nullptr;
    auto u32 = [&](size_t p) {
        return p + 4 <= app.effBin.size() ? uint32_t(app.effBin[p]) | uint32_t(app.effBin[p + 1]) << 8 |
                                                 uint32_t(app.effBin[p + 2]) << 16 | uint32_t(app.effBin[p + 3]) << 24
                                           : 0u;
    };
    const uint32_t off = u32(size_t(id) * 8), size = u32(size_t(id) * 8 + 4);
    if (!size || size_t(off) + size > app.effBin.size()) return nullptr;
    const std::vector<uint8_t> part(app.effBin.begin() + off, app.effBin.begin() + off + size);
    cr::Model m;
    if (!cr::loadModelPackage(part, m)) return nullptr;
    // eff3d_set_with_pos (0x60c08) and _with_pos_and_ang (0x608c4), after read_mdl_mem: property bits 0
    // and 1 cleared - glColor colour 1 -, no alpha test (threshold 0), no depth writes (drawEffects),
    // draw order + 1000 - both alike in CR1 (Omega's angled one keeps the depth writes)
    for (cr::Material& mt : m.materials) mt.unlit = 1, mt.alphaThreshold = 0;
    for (const auto& img : m.textures) {
        cr::Image t = img;
        if (app.video.hdTextures) applyHd(app, t);
        out.textures.push_back(app.renderer.upload(t));
    }
    for (const auto& mesh : m.meshes) out.meshes.push_back(app.renderer.upload(mesh));
    out.packageActions = m.actions;
    out.materialTable = boundActionTable(m.actions);
    // Its motion: the action table's action for the transform tree - one segment a node, and not one
    // of its materials' (9 segments, only segment 1's colour and 6 / 8's texture offset and scale:
    // they can come first - 30 of Omega's effects - and with 9 nodes they'd fit) - else the first
    // that fits the skeleton (or its first nodes)
    auto materials = [](const cr::Action& a) {
        for (size_t k = 0; k < a.segments.size(); ++k)
            for (const cr::Track& t : a.segments[k])
                if (!(k == 1 && t.type == 12) && !((k == 6 || k == 8) && (t.type == 1 || t.type == 11))) return false;
        return true;
    };
    for (auto& a : m.actions)
        if (out.actions.empty() && !materials(a) && a.segments.size() == m.skeleton.nodes.size() && !a.segments.empty())
            out.actions.push_back(a);
    for (auto& a : m.actions)
        if (out.actions.empty() && a.segments.size() == m.skeleton.nodes.size() && !a.segments.empty()) out.actions.push_back(a);
    for (auto& a : m.actions)
        if (out.actions.empty() && !a.segments.empty() && a.segments.size() <= m.skeleton.nodes.size())
            out.actions.push_back(a);
    m.actions.clear();
    out.action = out.actions.empty() ? -1 : 0;
    out.cpu = std::move(m);
    return out.meshes.empty() ? nullptr : &out;
}

// btlcamera_getpos: the world position of an actor's bone by its record user id (0: the actor)
cr::Vec3 actorPoint(App& app, const App::Battle::Actor& a, int user) {
    LoadedModel& lm = app.battle.models[a.model];
    const cr::Model& m = lm.cpu;
    int node = -1;
    if (user > 0)
        for (const auto& t : m.records)
            if (t.user == user)
                for (const auto& [obj, n] : m.objectNodes)
                    if (obj == t.id) node = n;
    if (node < 0 || m.skeleton.nodes.empty()) return a.pos;
    const cr::Action* act = a.action >= 0 && a.action < int(lm.actions.size()) ? &lm.actions[size_t(a.action)] : nullptr;
    poseWorld(m.skeleton, lm.actions, act, a.time, a.blend, lm.world);
    if (node >= int(lm.world.size())) return a.pos;
    const float* w = lm.world[size_t(node)].m;
    const float c = std::cos(a.yaw), sn = std::sin(a.yaw);
    const float x = w[12] * a.scale, y = w[13] * a.scale, z = w[14] * a.scale;
    return {a.pos.x + c * x + sn * z, a.pos.y + y, a.pos.z - sn * x + c * z};
}

// eff3d_set_btl: effect `id` on fighter `fighter` (at its table node)
void spawnEffect(App& app, int id, int fighter) {
    App::Battle& b = app.battle;
    if (id < 0 || id >= 100 || b.effects.size() >= 32 || !effectModel(app, id)) return;
    const App::Battle::Actor* a = battleActor(app, fighter);
    if (!a) return;
    Effect e;
    e.id = id, e.pos = actorPoint(app, *a, effects::kNode[id]);
    // on a monster all but 22, 23 and 30 at its size class's scale (0xd387a: 100 100 100 200 250 300)
    const cr::Fighter& f = b.rules.f[size_t(std::clamp(fighter, 0, 4))];
    if (fighter > 1 && f.def && id != 22 && id != 23 && id != 30) {
        static const short kScale[6] = {100, 100, 100, 200, 250, 300};
        e.scale = float(kScale[std::clamp(f.def->size, 0, 5)]) / 100.0f;
    }
    b.effects.push_back(e);
}

// eff3d_set_with_pos into `list` (32 slots): its handle, 0 when none was free
int startEffect(App& app, std::vector<Effect>& list, int id, cr::Vec3 at, bool loop, float scale, float yaw) {
    if (id < 0 || id >= 100 || list.size() >= 32 || !effectModel(app, id)) return 0;
    if (&list == &app.fieldEffects) ++app.fieldEffectCount[size_t(id)];
    Effect e;
    e.id = id, e.pos = at, e.loop = loop, e.scale = scale, e.yaw = yaw, e.handle = ++app.effectHandles;
    list.push_back(e);
    return e.handle;
}

void spawnEffectAt(App& app, int id, cr::Vec3 at) { startEffect(app, app.battle.effects, id, at, false, 1.0f, 0.0f); }
// eff3d_set_btl / eff3d_set_with_pos, then the model's +0x56f cleared: the effect keeps its own angles instead of
// facing the camera - 2.2.0's _btlact_430_init 0x59 (decomp 70178), _btlact_430's 0x5a (70230), _btlact_434_init's
// 0x1e (75138), _btlact_435's 0x21 (75307)
void keepEffectAngles(App& app, size_t before) {
    if (app.battle.effects.size() > before) app.battle.effects.back().keepAngles = true;
}

void stopEffect(std::vector<Effect>& list, int handle) { // eff3d_delete
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Effect& e) { return handle && e.handle == handle; }), list.end());
}

void stepEffects(App& app, std::vector<Effect>& list) { // eff3d_prc: each ends with its action (a loop starts again)
    for (auto& e : list) {
        e.time += 1;
        auto it = app.effectModels.find(e.id);
        const float dur = it == app.effectModels.end() || it->second.actions.empty() ? 30 : it->second.actions[0].duration;
        if (e.loop && e.time >= std::max(dur, 1.0f)) e.time = std::fmod(e.time, std::max(dur, 1.0f));
    }
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const Effect& e) {
                                  auto it = app.effectModels.find(e.id);
                                  const float dur = it == app.effectModels.end() || it->second.actions.empty()
                                                        ? 30
                                                        : it->second.actions[0].duration;
                                  return !e.loop && e.time >= std::max(dur, 1.0f);
                              }),
               list.end());
}
void stepEffects(App& app) { stepEffects(app, app.battle.effects); }

// An effect material's action (its refGid): segment 1 the colour, 6 texture unit 0's transform, 8
// unit 1's (keyType 1 the offset, 11 the scale; segment 8 only on two-unit materials). The refGid is
// an action's gid in the bound table, not its place (_mceFigure_bindAction -> _mceActionTable_findAction:
// action +0x28 == refGid); none there, the Appearance has no action and keeps its own colours
const cr::Action* effectMaterialAction(const LoadedModel& lm, const cr::Material* mt) {
    const int64_t g = mt ? int64_t(int32_t(mt->refGid)) : -1;
    if (g < 0) return nullptr;
    for (const cr::Action& a : lm.packageActions)
        if (a.table == lm.materialTable && int64_t(a.gid) == g) return &a;
    return nullptr;
}

// A texture unit's motion at `time`: uv * scale + shift (false: it has none)
bool effectUvMotion(const LoadedModel& lm, const cr::Material* mt, float time, float shift[2], float scale[2], int unit) {
    const cr::Action* ma = effectMaterialAction(lm, mt);
    const size_t seg = unit == 0 ? 6 : 8;
    bool moves = false;
    if (ma && ma->segments.size() > seg)
        for (const cr::Track& t : ma->segments[seg]) {
            float v[4] = {};
            if (t.comps != 3 || !cr::evaluateTrack(t, std::min(time, ma->duration), v)) continue;
            if (t.type == 1) shift[0] = v[0], shift[1] = v[1], moves = true;
            if (t.type == 11) scale[0] = v[0], scale[1] = v[1], moves = true;
        }
    return moves;
}

// Texture unit 1 of a mesh (its material's second texture), and where it samples: uv2 maps the
// coordinates the mesh holds - unit 0's, moved - to unit 1's (uv * xy + zw). Null: none
const cr::Texture* secondUnit(const LoadedModel& lm, size_t mesh, const cr::Material* mt, float time, float uv2[4]) {
    const cr::Model& m = lm.cpu;
    if (!mt || mt->textureRef2 < 0 || mesh >= m.meshes.size()) return nullptr;
    const int t = m.meshes[mesh].texture2;
    if (t < 0 || t >= int(lm.textures.size())) return nullptr;
    float o0[2] = {0, 0}, s0[2] = {1, 1}, o1[2] = {0, 0}, s1[2] = {1, 1};
    if (time >= 0) effectUvMotion(lm, mt, time, o0, s0, 0), effectUvMotion(lm, mt, time, o1, s1, 1);
    for (int k = 0; k < 2; ++k) {
        const float r = s1[k] / (std::fabs(s0[k]) > 1e-6f ? s0[k] : 1.0f);
        uv2[k] = r, uv2[2 + k] = o1[k] - o0[k] * r;
    }
    return &lm.textures[size_t(t)];
}

// Effects face the camera: mdl_put turns every model eff3d placed (flag 0x837) about Y to the
// camera's heading, atan2(eye - target) in x and z, each frame - so a sun is seen from the front
// whichever way the shot looks (their quads are culled from behind). Battles draw through mdl_put
// too, with the battle camera (Calc_ev / Calc_cv: gac +0xcc / +0xd8), so theirs turn the same way.
void drawEffects(App& app, const std::vector<Effect>& list, const cr::Mat4& vp, const cr::Vec3* eye, const cr::Vec3* target) {
    static const bool noScroll = SDL_getenv("CR_NO_UV_SCROLL") != nullptr; // tests: before / after
    for (const Effect& e : list) {
        LoadedModel* found = effectModel(app, e.id);
        auto it = app.effectModels.find(e.id);
        if (!found || it == app.effectModels.end()) continue;
        LoadedModel& lm = it->second;
        const cr::Model& m = lm.cpu;
        auto matOf = [&](size_t i) -> const cr::Material* {
            const int ap = i < m.meshes.size() ? m.meshes[i].appearance : -1;
            return ap >= 0 && ap < int(m.materials.size()) ? &m.materials[size_t(ap)] : nullptr;
        };
        auto materialAction = [&](const cr::Material* mt) { return effectMaterialAction(lm, mt); };
        auto uvMotion = [&](const cr::Material* mt, float shift[2], float scale[2]) {
            return effectUvMotion(lm, mt, e.time, shift, scale);
        };
        if (lm.meshes.size() == m.meshes.size()) {
            const bool posed = !m.skeleton.nodes.empty();
            if (posed) cr::computeWorld(m.skeleton, lm.actions.empty() ? nullptr : &lm.actions[0], e.time, lm.world);
            for (size_t i = 0; i < m.meshes.size(); ++i) {
                float shift[2] = {0, 0}, scale[2] = {1, 1};
                const bool moves = !noScroll && uvMotion(matOf(i), shift, scale);
                if (!posed && !moves) continue;
                cr::skinMesh(m, m.meshes[i], posed ? lm.world : std::vector<cr::Mat4>(), lm.skinned);
                if (moves) { // the texture scrolls (uv * scale + offset)
                    lm.uvScratch = m.meshes[i].uvs;
                    for (size_t k = 0; k + 1 < lm.uvScratch.size(); k += 2) {
                        lm.uvScratch[k] = lm.uvScratch[k] * scale[0] + shift[0];
                        lm.uvScratch[k + 1] = lm.uvScratch[k + 1] * scale[1] + shift[1];
                    }
                }
                app.renderer.updatePositions(lm.meshes[i], lm.skinned, moves ? lm.uvScratch : m.meshes[i].uvs, m.meshes[i].colors);
            }
        }
        // scaled, turned (Euler XYZ, mdl_put: R = Rz Ry Rx), at its point
        // (a battle's effects face the camera - not those whose +0x56f was cleared; a field's only those FrmPlayEff flagged)
        const bool face = eye && target && ((&list == &app.battle.effects && !e.keepAngles) || e.faceCamera);
        const float yaw = face ? std::atan2(eye->x - target->x, eye->z - target->z) : e.yaw;
        const float cx = std::cos(e.pitch), sx = std::sin(e.pitch), cy = std::cos(yaw), sy = std::sin(yaw);
        const float cz = std::cos(e.roll), sz = std::sin(e.roll), k = e.scale;
        cr::Mat4 world;
        world.m[0] = cy * cz * k, world.m[1] = cy * sz * k, world.m[2] = -sy * k;
        world.m[4] = (sx * sy * cz - cx * sz) * k, world.m[5] = (sx * sy * sz + cx * cz) * k, world.m[6] = sx * cy * k;
        world.m[8] = (cx * sy * cz + sx * sz) * k, world.m[9] = (cx * sy * sz - sx * cz) * k, world.m[10] = cx * cy * k;
        world.m[12] = e.pos.x, world.m[13] = e.pos.y, world.m[14] = e.pos.z;
        const cr::Mat4 mvp = vp * world;
        // drawn last, depth tested but not written (draw order + 1000, eff3d_set_with_pos_and_ang)
        std::vector<size_t> order(lm.meshes.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
            const cr::Material *a = matOf(x), *c = matOf(y);
            return (a ? a->drawOrder : 0) < (c ? c->drawOrder : 0);
        });
        for (size_t i : order) {
            if (i < m.meshes.size() && !m.meshes[i].visible) continue;
            const cr::Material* base = matOf(i);
            if (!base) continue;
            cr::Material mat = *base;
            if (const cr::Action* ma = materialAction(base); ma && ma->segments.size() > 1) // the colour track
                for (const cr::Track& t : ma->segments[1]) {
                    float v[4] = {1, 1, 1, 1};
                    if (t.type == 12 && t.comps == 4 && cr::evaluateTrack(t, std::min(e.time, ma->duration), v))
                        for (int k = 0; k < 4; ++k) mat.diffuse[k] = std::clamp(v[k], 0.0f, 1.0f);
                }
            if (mat.diffuse[3] <= 0.004f && mat.blended()) continue;
            mat.properties &= ~16u; // no depth writes
            const cr::GpuMesh& mesh = lm.meshes[i];
            const cr::Texture* tex = mesh.texture >= 0 && mesh.texture < int(lm.textures.size()) ? &lm.textures[size_t(mesh.texture)] : nullptr;
            float uv2[4];
            const cr::Texture* tex2 = secondUnit(lm, i, base, noScroll ? -1.0f : e.time, uv2);
            app.renderer.drawMesh(mesh, tex, mvp, mat, app.smoothFilter, tex2, uv2);
        }
    }
}
void drawEffects(App& app, const cr::Mat4& vp, const cr::Vec3& eye, const cr::Vec3& target) {
    drawEffects(app, app.battle.effects, vp, &eye, &target);
}

// read_btlcpl / btl_prty_init: the stage (kBattleStageFile), the pair at formation 0's first two
// places facing the enemies, the group's monsters at their formation's places; then the battle
// music (__bgmgo: var 58 when it names a track, else 8)
// ---- The battles' talks (_chk_karaok_*, EvrStart 2) ----
//
// In a battle the event script (PrcEvnt) runs only while flag 0xfff is set, and the battle waits
// meanwhile (btl_main's states check it); the HUD gives way to the portraits and the message
// box. Two kinds: an event battle's own script, waiting after its Battle, goes on when one of the
// story bosses' moments comes (by group and route, below) until it clears the flag; and in other
// battles the area's EvrStart 2 - the tutorials, keyed by var 1 - starts the first time each
// thing happens (flags 0xff1.. mark them seen), the battle's script ending with the flag.
namespace battletalk {
constexpr int kAnyRoute = -1;
struct Moment { int route, group, when, arg, seen; }; // when: 0 a round, 1 HP <= max / arg, 2 the 0x104 case
// _chk_karaok_frist: at the start
constexpr Moment kFirst[] = {{2, 0x156, 0, 0, -1}, {kAnyRoute, 0x164, 0, 0, -1}, {kAnyRoute, 0x165, 0, 0, -1},
                             {kAnyRoute, 0x166, 0, 0, -1}, {kAnyRoute, 0x167, 0, 0, -1}};
// _chk_karaok_end: as the last enemy falls
constexpr Moment kEnd[] = {{0, 0x154, 0, 0, -1}, {0, 0x158, 0, 0, -1}, {0, 0x160, 0, 0, -1}, {1, 0x155, 0, 0, -1},
                           {1, 0x11b, 0, 0, -1}, {1, 0x159, 0, 0, -1}, {1, 0x161, 0, 0, -1}, {2, 0x162, 0, 0, -1},
                           {3, 0x157, 0, 0, -1}, {3, 0x15b, 0, 0, -1}, {3, 0x163, 0, 0, -1}};
// _chk_karaok_between: after a round - its number (when 0), or enemy 2's HP at most its most / arg
// (when 1) while flag `seen` is clear
constexpr Moment kBetween[] = {
    {0, 0x104, 2, 3, 0x4d0},  {0, 0x173, 0, 3, -1},      {0, 0xad, 0, 2, -1},       {0, 0x105, 0, 3, -1},
    {0, 0x154, 0, 3, -1},     {0, 0x158, 0, 2, -1},      {0, 0x160, 1, 2, 0x4d7},   {0, 0x170, 1, 2, 0x4d9},
    {1, 0x118, 0, 3, -1},     {1, 0x119, 0, 3, -1},      {1, 0x159, 0, 2, -1},      {1, 0x161, 1, 2, 0x665},
    {1, 0x170, 1, 2, 0x667},  {2, 0x178, 0, 2, -1},      {2, 300, 0, 3, -1},        {2, 0x176, 0, 3, -1},
    {2, 0x156, 1, 5, 0x7f3},  {2, 0x17a, 0, 3, 0x7f4},   {2, 0x15e, 0, 5, 0x7f5},   {2, 0x162, 1, 2, 0x7f6},
    {2, 0x170, 1, 2, 0x7f8},  {3, 0x140, 0, 3, -1},      {3, 0x177, 0, 1, -1},      {3, 0x15b, 0, 2, -1},
    {3, 0x163, 1, 2, 0x985},  {3, 0x170, 1, 2, 0x987},   {kAnyRoute, 0x164, 1, 2, 0x4db},
    {kAnyRoute, 0x165, 1, 2, 0x4dd}, {kAnyRoute, 0x166, 1, 2, 0x4df}, {kAnyRoute, 0x167, 1, 2, 0x4e1}};
// _chk_karaok_dmg_hp: until flag `seen` is set nobody falls in these
constexpr Moment kKeepAlive[] = {
    {0, 0x104, 0, 0, 0x4d0}, {0, 0x173, 0, 0, 0x4d1}, {0, 0xad, 0, 0, 0x4a9},  {0, 0x105, 0, 0, 0x4d2},
    {0, 0x154, 0, 0, 0x4d3}, {0, 0x158, 0, 0, 0x4d5}, {0, 0x160, 0, 0, 0x4d7}, {0, 0x170, 0, 0, 0x4d9},
    {1, 0x118, 0, 0, 0x65f}, {1, 0x119, 0, 0, 0x660}, {1, 0x159, 0, 0, 0x663}, {1, 0x161, 0, 0, 0x665},
    {1, 0x170, 0, 0, 0x667}, {2, 0x178, 0, 0, 0x7ef}, {2, 300, 0, 0, 0x7f0},   {2, 0x176, 0, 0, 0x7f1},
    {2, 0x156, 0, 0, 0x7f3}, {2, 0x17a, 0, 0, 0x7f4}, {2, 0x15e, 0, 0, 0x7f5}, {2, 0x162, 0, 0, 0x7f6},
    {2, 0x170, 0, 0, 0x7f8}, {3, 0x140, 0, 0, 0x97f}, {3, 0x177, 0, 0, 0x980}, {3, 0x15b, 0, 0, 0x983},
    {3, 0x163, 0, 0, 0x985}, {3, 0x170, 0, 0, 0x987}, {kAnyRoute, 0x164, 0, 0, 0x4db},
    {kAnyRoute, 0x165, 0, 0, 0x4dd}, {kAnyRoute, 0x166, 0, 0, 0x4df}, {kAnyRoute, 0x167, 0, 0, 0x4e1}};

int route(const App& app) { return std::clamp(app.playerChar / 2, 0, 3); } // G+0xc18
bool on(const App& app) { return app.battle.active && app.game.flag(kFlag); }
template <size_t N> bool meets(const App& app, const Moment (&list)[N], const std::function<bool(const Moment&)>& more) {
    const App::Battle& b = app.battle;
    if (!b.eventBattle) return false;
    for (const Moment& m : list)
        if ((m.route == kAnyRoute || m.route == route(app)) && m.group == b.start.group && more(m)) return true;
    return false;
}
void open(App& app) { app.game.setFlag(kFlag, true); } // (__menub_karaok_on)

// The scripted rules of this battle, as they stand now (chk_karaok_dmg_hp, btlcalc_status_prc)
void rules(App& app) {
    App::Battle& b = app.battle;
    cr::BattleRules& r = b.rules;
    r.keepAlive = meets(app, kKeepAlive, [&](const Moment& m) { return !app.game.flag(m.seen); });
    r.regenFull = {};
    if (!b.eventBattle) return;
    const int g = b.start.group, rt = route(app);
    for (int i = 2; i < 5; ++i) // enemies: whole again each round until their moment
        r.regenFull[size_t(i)] = (rt == 0 && g == 0x104 && !app.game.flag(0x4d0) && !r.lateAttack) ||
                                 (rt == 2 && i == 2 && g == 0x17a && !app.game.flag(0x7f4)) ||
                                 (rt == 2 && i == 2 && g == 0x15e && !app.game.flag(0x7f5));
}

// btl_main 300 -> 400: the battle's first moment, else the first battle's tutorial (0xff1)
void first(App& app) {
    if (meets(app, kFirst, [](const Moment&) { return true; })) open(app);
    else tutorial(app, 0xff1);
}
// btl_main 899: a round over and everyone standing
void between(App& app) {
    const cr::BattleRules& r = app.battle.rules;
    const cr::Fighter& e = r.f[2];
    if (meets(app, kBetween, [&](const Moment& m) {
            if (m.when == 2) // 0x104: the third round, then the pair attack landing after it
                return r.round == 3 || (r.round > 3 && r.lateAttack && !app.game.flag(m.seen));
            if (m.seen >= 0 && app.game.flag(m.seen)) return false;
            if (m.when == 0) return r.round == m.arg;
            return e.hp <= e.hpMax / m.arg;
        }))
        open(app);
}
// btlord_chk 2: the enemies are down - a last word first? (btl_main 8000 waits for it)
bool end(App& app) {
    if (!meets(app, kEnd, [](const Moment&) { return true; })) return false;
    open(app);
    return true;
}
// EvrStart 2 with var 1 = `key` (0xff1 the first battle, 0xff2 the Break, 0xff3..0xff5 an element
// taken / a status / a gene learned, 0xfa8 / 0xfa9 the Genes / Defend commands), once each: not
// while a script runs (an event battle's waits) or a talk is on
bool tutorial(App& app, int key) {
    if (!app.battle.active || app.game.flag(kFlag) || app.game.flag(key) || app.runner.running() || app.testNoTutorials)
        return false;
    const uint32_t at = app.events.returnEntry(2);
    if (!at || !app.runner.startAt(app.events, at, 0, app.game, app.area, app.mapNumber)) return false;
    app.runnerWhat = "battle tutorial";
    app.game.vars[1] = key;
    open(app);
    app.runner.setGate(kFlag);
    return true;
}
// A battle frame with the talk on: the script's step (its pages, portraits and pictures), the
// battle held. Once the script clears the flag an event battle's waits again; another ends.
void step(App& app) {
    if (!app.runner.running()) { app.game.setFlag(kFlag, false); return; }
    stepEvents(app);
    if (app.game.flag(kFlag)) return;
    if (!app.battle.eventBattle) app.runner.stop();
}
} // namespace battletalk

// btl_main 0's FUN_0000bcc4 (2.2.0 0xbcc4): the sound a battle opens with, by its group (__effgo) -
// 0x18 for groups 9-10, 0x2a-0x2b, 0x48-0x49, 0x7a-0x7b, 0xac-0xad, 0x154-0x163, 0x168-0x16f and 0x179;
// 0x19 for 0x104-0x106, 0x118-0x11b, 0x12c-0x12e, 0x140-0x142, 0x170, 0x173, 0x176, 0x178; 0x1a for
// 0x164-0x167; else 0x17 (Omega's table, its sounds 0xa8..0xab)
int battleInSound(int g) {
    for (int base : {9, 0x2a, 0x48, 0x7a, 0xac})
        if (g == base || g == base + 1) return 0x18;
    if ((g >= 0x154 && g <= 0x163) || (g >= 0x168 && g <= 0x16f) || g == 0x179) return 0x18;
    if ((g >= 0x104 && g <= 0x106) || (g >= 0x118 && g <= 0x11b) || (g >= 0x12c && g <= 0x12e) || (g >= 0x140 && g <= 0x142) ||
        g == 0x170 || g == 0x173 || g == 0x176 || g == 0x178)
        return 0x19;
    if (g >= 0x164 && g <= 0x167) return 0x1a;
    return 0x17;
}

void startBattle(App& app, const cr::BattleStart& start, int flags) {
    app.renderer.resetHistory(); // a cut: the upscaler starts over
    App::Battle& b = app.battle;
    const std::array<int, 3> ids = app.battleData.group(start.group);
    if (!app.battleData.loaded() || !ids[0] || start.stage < 0 || start.stage >= cr::kNumBattleStages) {
        app.unsupported.push_back("battle " + std::to_string(start.group) + " on map " + std::to_string(app.mapNumber));
        return;
    }
    app.sound.stopDialogueVoices();
    app.voiceQueue.clear();
    b.active = true;
    b.start = start;
    b.flags = flags;
    b.eventBattle = false;
    b.breakFx = {};
    app.runner.setGate(battletalk::kFlag); // PrcEvnt in battle mode
    // CRView::prc's encounter frame (2.2.0 decomp 932-933): one get_random(2), then SetFade(white, 255,
    // 255, 1 ms) - solid white at once; btl_main 0 -> 10 -> 100 -> 199 -> 200 run under it, and 200
    // starts the fade in (SetFade(white, 255, 0, 1000 ms): the Intro phase, kIntroWhite steps on)
    app.rng.get(2);
    app.fadeGrey = 1, app.fadeFrom = 1, app.fadeTo = 1, app.fadeFrames = 20, app.fadeT = 0;
    b.frames = 0;
    b.clock = 0;
    b.formation = std::max(0, app.battleData.formation(ids));
    b.actors.clear();
    b.phase = App::Battle::Intro;
    b.phaseFrames = 0;
    b.introStep = 300;
    b.roundFirst = false;
    b.steps = 0;
    b.menu = App::Battle::Top;
    b.cursor = 0;
    b.member = 0;
    b.view = {};
    b.page = 0, b.viewState = 0, b.slide = battleSlideDistance(app), b.slideHold = 0, b.menuGone = 100, b.roundWait = false;
    b.turnOn = false, b.pairHeld = false;
    b.handover = 0, b.breakDue = false;
    for (App::Battle::Popup& p : b.popups) p = App::Battle::Popup();
    b.hudSlide = 0;
    b.banner.clear();
    b.bannerFrames = 0;
    b.bannerShown.clear();
    b.notices.clear();
    b.levelled.clear();
    b.settled = false;
    b.exit = 0;
    b.quake = {};
    b.voiceRng = uint32_t(SDL_GetTicks()) | 1u;
    for (int& v : b.voiceVariant) { b.voiceRng = b.voiceRng * 1103515245u + 12345u; v = int((b.voiceRng >> 16) % 3); } // reset_variants
    for (bool& c : b.commandSaid) c = false;
    b.voiceRound = -1;
    b.flash = {};
    b.stageAction = 0;
    b.breakPop = 0;
    b.gaugeShown = 0;
    b.cam = -1, b.camLive = false, b.camHeld = false, b.camEase = 1, b.camHold = 25;
    loadModelFile(app, cr::kBattleStageFile[start.stage], b.stage, false);
    auto add = [&](int modelId, const short* place, float yaw, int fighter) -> App::Battle::Actor& {
        App::Battle::Actor a;
        a.modelId = modelId;
        a.model = cr::kBattleModelFile[modelId];
        if (!b.models.count(a.model)) loadModelFile(app, a.model, b.models[a.model], true);
        if (fighter >= 0 && fighter < 5) battleSlotsLoaded(b.models[a.model], modelId, fighter < 2, false);
        a.pos = a.home = {float(place[0]), 0.0f, float(place[1])};
        a.yaw = a.homeYaw = a.wantYaw = yaw; // (+0x558 = 15: btl_main 100, read_btlcpl)
        a.fighter = fighter;
        b.actors.push_back(a);
        playSlot(app, b.actors.back(), btlui::kSlotIdle);
        return b.actors.back();
    };
    // The rules: the pair from the party, the monsters grown to their level
    const int route = std::clamp(app.playerChar / 2, 0, 3);
    cr::BattleRules& rules = b.rules;
    rules = cr::BattleRules();
    rules.useRandom(&app.rng); // the one stream, as the original's
    rules.strike = start.strike;
    // __menub_mainPrc / __menub_mainPut: Escape is dimmed and refused while a script runs (gac+0x2c0
    // bit 0 - a scene's battle); the opcode's flags byte (gac+0x354c6) is never read
    rules.escapeBlocked = app.runner.running();
    rules.formation = b.formation; // (btlord_enemy_rand: the enemy pair only in formation 2)
    (void)flags;
    rules.setupParty(app.game, app.itemTable, route);
    // btl_enemy_init_lv: a monster without a level of its own takes the route's World Select band
    // (prty+0xc10[pair], 0..9: levels band x 10 + 1..10, kept near the pair's)
    const int worldBand = app.game.bands[size_t(route)];
    rules.setupSpecial(app.game, app.itemTable, start.group, route); // the story battles' line-ups
    rules.setupEnemies(app.battleData, ids, worldBand);
    for (int k = 0; k < 2; ++k) // Uty_Btl_ParamUp01 / 02 called before this battle began
        if (app.paramUp[k]) {
            for (int st = cr::kStatusAttackUp; st <= cr::kStatusLuckUp; ++st) rules.f[size_t(k)].status[st] = 6;
            app.paramUp[k] = false;
        }
    for (int k = 0; k < 2; ++k) { // the pair (or who fights): j001..j008
        if (!rules.f[size_t(k)].present) continue;
        const int c = rules.f[size_t(k)].character;
        App::Battle::Actor& a = add(c, cr::kBattlePosition[0][k], 3.14159265f, k);
        a.name = sysText(app, 58028 + c + 1); // the name plates' character names
        a.level = rules.f[size_t(k)].level;
    }
    for (int k = 0; k < 3; ++k) {
        const cr::MonsterDef* m = app.battleData.monster(ids[size_t(k)]);
        if (!m || m->model >= cr::kNumBattleModels || !rules.f[size_t(2 + k)].present) continue;
        App::Battle::Actor& a = add(m->model, cr::kBattlePosition[b.formation][k], 0.0f, 2 + k);
        a.enemy = true;
        a.monster = m->id;
        a.level = rules.f[size_t(2 + k)].level;
        a.hp = a.hpMax = uint32_t(rules.f[size_t(2 + k)].hpMax);
        a.name = sysText(app, 53000 + m->id - 1); // font block 2: monster names
        // read_btlcpl (0xceec-0xcf14): slot 0's frame get_random(btlanm_get_max_frm(slot 0)) - the
        // monsters' idles out of step
        a.time = float(app.rng.get(int(actorDuration(app, a) - 1.0f)));
    }
    for (int i = 0; i < 5; ++i) {
        b.shownHp[i] = rules.f[size_t(i)].hp;
        b.shownMp[i] = rules.f[size_t(i)].mp;
        if (i < 2) b.rollHp[i] = 0, b.rollMp[i] = 0; // (btlpcparam_prty_init, 2.2.0 decomp 29859 / 29868: shown +0x60 /
                                                     // +0x68 from 0 - the rows count up under the opening white)
    }
    setBattleCamera(app, std::max(0, b.formation - 2), -1); // (under the white; btl_main 200 sets it again)
    stepBattleCamera(app);
    { // btl_main 0: FUN_0000bcc4(group) - the battle's opening sound (__effgo)
        const uint32_t id = uint32_t(battleInSound(start.group));
        app.sound.play(id);
        app.soundLog.push_back(id);
    }
    // btl_main 100: var 58 under 29 names the battle's track; else the route's World Select band
    // (prty+0xc10[pair], the byte btl_enemy_init_lv levels the monsters by) picks 8, or 9 from band 5
    const auto v58 = app.game.vars.find(58);
    const int track = v58 != app.game.vars.end() && v58->second >= 0 && v58->second < 29 ? v58->second : worldBand < 5 ? 8 : 9;
    playBgm(app, uint32_t(track));
    app.dialogue.clear();
    ++app.battlesFought;
    SDL_Log("battle: group %d on stage %s (formation %d, %s)", start.group, cr::kBattleStageFile[start.stage],
            b.formation, start.strike == 1 ? "first strike" : start.strike == 2 ? "ambush" : "normal");
    updateTitle(app);
}

// What the battle leaves in the save (once): MP back to the party, and when they won the
// experience (levels) and the Oz
void settleBattle(App& app) {
    App::Battle& b = app.battle;
    if (b.settled || !b.active) return;
    b.settled = true;
    // MbResultSet: the boards from where the members were, the takings, the items grouped
    App::Battle::Result& rs = b.result;
    rs = App::Battle::Result();
    for (int k = 0; k < 2; ++k) {
        const cr::Fighter& x = b.rules.f[size_t(k)];
        if (!x.present || x.guest) continue;
        const cr::PartyMember& pm = app.game.party[size_t(x.character)];
        rs.character[k] = x.character, rs.level[k] = pm.level, rs.exp[k] = float(pm.exp), rs.hp0[k] = x.hp;
    }
    b.levelled = b.rules.applyRewards(app.game);
    int most = 0;
    for (int k = 0; k < 2; ++k) {
        if (rs.character[k] < 0) continue;
        const cr::PartyMember& pm = app.game.party[size_t(rs.character[k])];
        rs.levelTo[k] = pm.level, rs.expTo[k] = float(pm.exp), rs.levels[k] = pm.level - rs.level[k];
        rs.expStep[k] = (rs.expTo[k] - rs.exp[k]) / float((rs.levels[k] + 1) * 60);
        most = std::max(most, rs.levels[k]);
        if (rs.levels[k] > 0) rs.ups.push_back({k, rs.levels[k]});
    }
    rs.count = (most + 1) * 60;
    rs.expTotal = b.rules.expGained * (b.rules.doubleExp ? 2 : 1);
    rs.ozTotal = b.rules.ozGained * (b.rules.doubleOz ? 2 : 1);
    // (MbResultSet 58480-58527, DAT_000d2aea: the count ticks unless no HP bar refills and either every
    // member is level 100 or nothing - no EXP, no Oz - was earned)
    bool refill = false, all100 = true;
    for (int k = 0; k < 2; ++k)
        if (rs.character[k] >= 0) refill |= rs.hp0[k] != b.rules.f[size_t(k)].hpMax, all100 &= rs.level[k] == 100;
    rs.tick = refill || (!all100 && (rs.expTotal > 0 || rs.ozTotal > 0));
    rs.purseTo = app.game.oz;
    rs.shownPurse = float(std::max(0, rs.purseTo - (b.rules.outcome() == 2 ? rs.ozTotal : 0)));
    auto addItem = [&](int item) {
        for (auto& [it, n] : rs.items)
            if (it == item) { ++n; return; }
        rs.items.push_back({item, 1});
    };
    for (int item : b.rules.drops) addItem(item); // (a Steal Item catch and the abilities are in the bag already, unlisted)
}

// Back to the field: its music again, the encounter count restarted (fldencount_init), and the
// area's EvrStart 2 script (run after battles)
void endBattle(App& app, bool won) {
    app.renderer.resetHistory(); // a cut: the upscaler starts over
    app.sound.stopBattleLines(); // _btlvoice_stop_all
    App::Battle& b = app.battle;
    settleBattle(app);
    std::string names;
    for (const auto& a : b.actors)
        if (a.enemy) names += (names.empty() ? "" : ", ") + (a.name.empty() ? "monster " + std::to_string(a.monster) : a.name);
    unloadLoaded(app, b.stage);
    for (auto& [name, lm] : b.models) unloadLoaded(app, lm);
    b.models.clear();
    for (auto& [id, lm] : app.effectModels) unloadLoaded(app, lm);
    app.effectModels.clear();
    b.effects.clear();
    app.fieldEffects.clear();
    b.actors.clear();
    b.active = false;
    app.renderer.destroy(b.gameOver);
    b.lostTap = false;
    { // fldread_prc's return from a battle (the load flag's bit 3): var 60 says what plays - 998 nothing
      // (the battle's music faded out at its end), under 29 that track, else the field's last request
      // (G+0x358e0, which __EvtBattle zeroes: a scene's battle without a var 60 gets track 0)
        const auto v60 = app.game.vars.find(60);
        const int after = v60 != app.game.vars.end() ? v60->second : 999;
        if (after == 998) playBgm(app, 1u << 29);
        else if (after >= 0 && after < 29) playBgm(app, uint32_t(after));
        else playBgm(app, app.bgmNo);
    }
    cr::EncounterTable table;
    const bool have = cr::encounterTable(app.events, app.mapNumber, table);
    app.encounters.enterMap(have ? &table : nullptr);
    resetFieldNpcs(app); // (the field read back: load flags | 0xd, npc_read_setup, then npc_setup)
    app.lastEvent = (won ? "won against " : "ran from ") + names;
    // Back on the field (fldread_prc): an event battle's script goes on from its Battle; after any
    // other the room's entry script (EvrStart 5) runs again, as the field reloads
    app.runner.setGate(-1);
    app.game.setFlag(battletalk::kFlag, false);
    // FieldScene::onLoading's SetMask2(1): the field revealed out of the battle's black over 10 frames
    app.fadeGrey = 0, app.fadeFrom = 1, app.fadeTo = 0, app.fadeFrames = 10, app.fadeT = 0;
    b.ending = false, b.endFrames = b.pendingExit = 0;
    if (b.eventBattle) {
        b.eventBattle = false;
    } else {
        app.runner.stop(); // (a battle tutorial cut short)
        app.pendingEntry = App::Entry::Room;
    }
    updateTitle(app);
}

void setBanner(App& app, const std::string& text, int frames) {
    app.battle.banner = text;
    app.battle.bannerFrames = text.empty() ? 0 : frames;
}

// ---- The commands (menub_mainPrc) ----


void battleMenu(App& app, App::Battle::Menu menu) {
    App::Battle& b = app.battle;
    b.menu = menu;
    ++b.page;
    b.cursor = 0;
    b.pick = b.pickTop = 0;
    // the pages' own kept lines (the target prompt) go with them; a timed message stays until it runs out
    // or the next replaces it - 2.2.0's __menub_mainPrc (0x17920) writes slot 11 only for the escape question
    if (menu != App::Battle::Targets && b.bannerFrames > (1 << 19)) setBanner(app, "");
    // the camera: the field of battle for the first menu, the member (or the pair) choosing
    if (menu == App::Battle::Top || menu == App::Battle::EscapeAsk) setBattleCamera(app, b.formation + 12);
    else if (menu == App::Battle::Commands) {
        setBattleCamera(app, b.rules.pair ? 30 : 28 + b.member);
        btlvoice::command(app, b.rules.pair ? 0 : b.member); // __menub_mainPrc: the member's line
    }
}

// The fighters (or items) the menus offer
void battleOpenTargets(App& app, int action) {
    App::Battle& b = app.battle;
    const cr::BattleActionDef* a = app.battleActions.get(action);
    if (!a) return;
    b.picks = b.rules.targets(*a, b.member);
    if (b.picks.empty()) { menuSound(app, se::kBuzzer); return; }
    if (b.rules.isGroupScope(*a)) { // the whole side: no choosing (the plates show who)
    }
    b.menu = App::Battle::Targets;
    ++b.page;
    b.pick = 0;
    b.targetAction = action;
    cameraForTarget(app, b.picks[0], b.rules.isGroupScope(*a));
    setBanner(app, sysText(app, 57066 + a->scope), 1 << 20); // "Target: ..." while choosing
    menuSound(app, se::kChoose);
}

void battleStartRound(App& app) {
    App::Battle& b = app.battle;
    b.rules.beginRound(b.cmds, app.battleActions);
    for (int k = 0; k < 2; ++k) // defending holds the guard pose through the round
        if (b.rules.f[size_t(k)].guarding)
            if (App::Battle::Actor* a = battleActor(app, k)) playSlot(app, *a, btlui::kSlotGuard);
    b.phase = App::Battle::Acting;
    b.phaseFrames = 0;
    b.turnOn = false;
    // btl_main 500 (decomp 6241-6262): the message queue's count (+0xadc) back to 0; the banner up stays -
    // 500 / 600 don't wait for it (only 799, between actions, does) and the first action's name replaces it
    b.notices.clear();
    b.roundFirst = true;
    b.roundWait = true; // (the menu goes first: __menub_mainPrc 0x270e, then btl_main 500 / 600)
}

// A command is set: the second member's turn (solo), else the round
void battleCommandDone(App& app) {
    App::Battle& b = app.battle;
    menuSound(app, se::kChoose);
    if (!b.rules.pair && b.member == 0 && b.rules.f[1].alive() && !b.rules.allyAuto) {
        b.member = 1;
        battleMenu(app, App::Battle::Commands);
        return;
    }
    battleStartRound(app);
}

// __menub_mainPrc 3: the pair's places for the round - in pair mode side by side (_btlinitpos'
// second row), solo back where they began (the first; the second member only standing)
void placePair(App& app, bool pair) {
    App::Battle& b = app.battle;
    for (int k = 0; k < 2; ++k)
        if (App::Battle::Actor* a = battleActor(app, k); a && (pair || k == 0 || b.rules.f[size_t(k)].alive())) {
            const short* place = cr::kBattlePosition[pair ? 1 : 0][k];
            a->pos = a->home = {float(place[0]), 0.0f, float(place[1])};
        }
}

void battleChooseTop(App& app, int i) {
    App::Battle& b = app.battle;
    b.cmds[0] = b.cmds[1] = cr::BattleCommand();
    b.pendingItem = b.pendingGene = 0;
    switch (i) {
    case 0: // Solo
        b.rules.pair = false;
        placePair(app, false);
        if (!b.rules.f[0].alive() && b.rules.allyAuto) { battleStartRound(app); break; } // only the guest stands
        b.member = b.rules.f[0].alive() ? 0 : 1;
        menuSound(app, se::kChoose);
        battleMenu(app, App::Battle::Commands);
        break;
    case 1: // Pair: both standing (not with a guest)
        if (!b.rules.f[0].alive() || !b.rules.f[1].alive() || b.rules.allyAuto) { menuSound(app, se::kBuzzer); break; }
        b.rules.pair = true;
        placePair(app, true);
        b.member = 0;
        menuSound(app, se::kChoose);
        battleMenu(app, App::Battle::Commands);
        break;
    case 2: // Escape: asked first (57015)
        if (b.rules.escapeBlocked) { menuSound(app, se::kBuzzer); break; }
        menuSound(app, se::kChoose);
        battleMenu(app, App::Battle::EscapeAsk);
        b.confirm = 0;
        break;
    default: // Options (_SetOptionMenu): the Options screen over the battle, back here when it closes
        menuSound(app, se::kChoose);
        openOptions(app, 0);
        break;
    }
}

// The genes the member (in pair mode, either of the pair) can use, and the MP to pay with
std::vector<int> battleGeneList(const App& app) {
    const App::Battle& b = app.battle;
    std::vector<int> out = cr::usableAbilities(app.game, b.rules.f[size_t(b.rules.pair ? 0 : b.member)].character);
    if (b.rules.pair)
        for (int g : cr::usableAbilities(app.game, b.rules.f[1].character))
            if (std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
    std::sort(out.begin(), out.end());
    return out;
}
int battleGeneMp(const App& app) { // __GetBtlGeneMp: the lower of the pair's
    const App::Battle& b = app.battle;
    return b.rules.pair ? std::min(b.rules.f[0].mp, b.rules.f[1].mp) : b.rules.f[size_t(b.member)].mp;
}
int geneMpCost(const App& app, int ability) { // skill 132 halves it
    const cr::BattleActionDef* a = app.battleActions.get(cr::geneAction(ability));
    return a ? app.battle.rules.geneCost(*a, app.battle.rules.pair ? 0 : app.battle.member) : 0;
}
bool battleGenesSilenced(const App& app) {
    const App::Battle& b = app.battle;
    if (b.rules.pair) return b.rules.f[0].status[cr::kStatusSilence] || b.rules.f[1].status[cr::kStatusSilence];
    return b.rules.f[size_t(b.member)].status[cr::kStatusSilence] > 0;
}

void battleChooseGene(App& app, int ability) {
    App::Battle& b = app.battle;
    const cr::BattleActionDef* a = app.battleActions.get(cr::geneAction(ability));
    if (!a) return;
    if (battleGeneMp(app) < geneMpCost(app, ability)) { // 57081
        setBanner(app, sysText(app, 57081));
        menuSound(app, se::kBuzzer);
        return;
    }
    b.pendingGene = ability;
    cr::BattleCommand& c = b.cmds[b.rules.pair ? 0 : b.member];
    c = cr::BattleCommand();
    c.kind = cr::kCmdGene;
    c.action = a->id;
    if (b.rules.isGroupScope(*a) || a->scope == 1) {
        c.target = b.member;
        b.pendingGene = 0;
        battleCommandDone(app);
        return;
    }
    battleOpenTargets(app, a->id);
}

void battleChooseCommand(App& app, int i) {
    App::Battle& b = app.battle;
    cr::BattleCommand& c = b.cmds[b.rules.pair ? 0 : b.member];
    c = cr::BattleCommand();
    b.pendingItem = 0;
    switch (i) {
    case 0: battleOpenTargets(app, b.rules.attackAction(b.member)); break;
    case 1: // Genes: what the worn plates hold that's known
        b.picks = battleGeneList(app);
        if (b.picks.empty() || battleGenesSilenced(app)) { menuSound(app, se::kBuzzer); break; }
        menuSound(app, se::kChoose);
        b.menu = App::Battle::Genes;
        ++b.page;
        b.pick = b.pickTop = 0;
        battletalk::tutorial(app, 0xfa8); // the first time: what genes cost
        break;
    case 2: { // Items: what the bag holds of the battle items (21..46)
        b.picks.clear();
        for (int n = 21; n <= 46; ++n)
            if (auto it = app.game.items.find(n); it != app.game.items.end() && it->second > 0 && app.battleActions.get(n + 79))
                b.picks.push_back(n);
        if (b.picks.empty()) {
            setBanner(app, sysText(app, 57080));
            menuSound(app, se::kBuzzer);
            break;
        }
        menuSound(app, se::kChoose);
        b.menu = App::Battle::Items;
        ++b.page;
        b.pick = b.pickTop = 0;
        break;
    }
    case 3: // Defend: the guard pose at once (__menub_mainPrc: the loop motion (slot 4) starts as it is chosen - both of a pair's)
        c.kind = cr::kCmdDefend;
        for (int k = 0; k < 2; ++k)
            if (App::Battle::Actor* a = (b.rules.pair || k == b.member) ? battleActor(app, k) : nullptr) playSlot(app, *a, btlui::kSlotGuard);
        if (b.viewState == 1 || b.viewState == 2) b.slideHold = 20; // (the page goes, then 20 frames: 0x66 -> 199, +0x48 0x14)
        battletalk::tutorial(app, 0xfa9); // the first time: what it does
        battleCommandDone(app);
        break;
    default: break;
    }
}

void battleChooseItem(App& app, int item) {
    App::Battle& b = app.battle;
    const cr::BattleActionDef* a = app.battleActions.get(item + 79);
    if (!a) return;
    b.pendingItem = item;
    cr::BattleCommand& c = b.cmds[b.rules.pair ? 0 : b.member];
    c = cr::BattleCommand();
    c.kind = cr::kCmdItem;
    c.item = item;
    if (b.rules.isGroupScope(*a) || a->scope == 1) {
        c.target = b.member;
        battleCommandDone(app);
        return;
    }
    battleOpenTargets(app, item + 79);
}

void battleChooseTarget(App& app, int fighter) {
    App::Battle& b = app.battle;
    cr::BattleCommand& c = b.cmds[b.rules.pair ? 0 : b.member];
    if (b.pendingItem) {
        c.kind = cr::kCmdItem;
        c.item = b.pendingItem;
    } else if (b.pendingGene) {
        c.kind = cr::kCmdGene;
        c.action = cr::geneAction(b.pendingGene);
        b.pendingGene = 0;
    } else
        c.kind = cr::kCmdAttack;
    c.target = fighter;
    setBanner(app, "");
    battleCommandDone(app);
}

void battleBack(App& app) {
    App::Battle& b = app.battle;
    switch (b.menu) {
    case App::Battle::Commands:
        if (!b.rules.pair && b.member == 1 && b.rules.f[0].alive()) {
            b.member = 0;
            setBattleCamera(app, 28);
        } else
            battleMenu(app, App::Battle::Top);
        break;
    case App::Battle::Targets: {
        const bool item = b.pendingItem != 0, gene = b.pendingGene != 0;
        battleMenu(app, item ? App::Battle::Items : gene ? App::Battle::Genes : App::Battle::Commands);
        if (item) b.pendingItem = 0;
        if (gene) b.pendingGene = 0, b.picks = battleGeneList(app);
        if (item) {
            b.picks.clear();
            for (int n = 21; n <= 46; ++n)
                if (auto it = app.game.items.find(n); it != app.game.items.end() && it->second > 0 && app.battleActions.get(n + 79))
                    b.picks.push_back(n);
        }
        break;
    }
    case App::Battle::Items: case App::Battle::Genes: battleMenu(app, App::Battle::Commands); break;
    case App::Battle::EscapeAsk: battleMenu(app, App::Battle::Top); break;
    default: return;
    }
    menuSound(app, se::kBack);
}

void battleEscapeAnswer(App& app, bool yes) {
    App::Battle& b = app.battle;
    if (!yes) {
        battleBack(app);
        return;
    }
    b.rules.pair = false;
    b.cmds[0] = b.cmds[1] = cr::BattleCommand();
    b.cmds[0].kind = cr::kCmdEscape;
    menuSound(app, se::kChoose);
    // the escape is the pair's one action this round (both members' commands become it)
    b.rules.pair = true;
    battleStartRound(app);
    b.rules.pair = false;
}

// ---- Playing the round (btlact_*) ----

// _btlact_attack_init's mdl_ang_y (2.2.0 0x15548: into +0xd7c, the wanted yaw only): the attacker's
// wanted yaw toward `target` - turned to at 15 degrees a frame by mdl_put, not snapped
void faceFighter(App& app, App::Battle::Actor& a, int target) {
    if (const App::Battle::Actor* t = battleActor(app, target))
        if (t != &a) a.wantYaw = std::atan2(t->pos.x - a.pos.x, t->pos.z - a.pos.z);
}

// _mdl_put (0x14a70, FUN_00013734 in battle mode): each model's shown yaw toward its wanted one, by
// +0x558 degrees a frame (_GetNewMdlDir2: the shorter way round), the step back to 15 once it's there
void turnActors(App& app) {
    constexpr float kTurn = 2.0f * 3.14159265f;
    for (App::Battle::Actor& a : app.battle.actors) {
        const float d = std::remainder(a.wantYaw - a.yaw, kTurn);
        const float step = a.turnStep / 360.0f * kTurn;
        if (std::fabs(d) <= step) {
            a.yaw = a.wantYaw;
            a.turnStep = 15;
        } else
            a.yaw = std::remainder(a.yaw + (d > 0 ? step : -step), kTurn);
    }
}

// A turn timed over `frames` (btlmove_prc, _btlact_attack's return and end: the gap x 360 / frames)
void turnOver(App::Battle::Actor& a, float want, float frames) {
    const float d = std::fabs(std::remainder(want - a.wantYaw, 2.0f * 3.14159265f));
    a.wantYaw = want;
    a.turnStep = d * 180.0f / 3.14159265f / std::max(1.0f, frames);
}

// ---- Screen quakes and flashes (Quake, SetFade_back) ----

// StartQuake(pixels, from %, to %, frames, hold): the offset's size runs from -> to over the
// frames; a held quake goes on at its end until EndQuake, and a passing one doesn't replace it
void startQuake(App::Battle::Quake& q, float amp, float from, float to, int frames, bool hold) {
    if (!hold && q.mode == 2) return;
    q.amp = std::fabs(amp);
    q.now = q.from = std::clamp(from, 0.0f, 100.0f);
    q.to = std::clamp(to, 0.0f, 100.0f);
    q.frame = 0;
    q.frames = std::max(1, frames);
    q.mode = hold ? 2 : 1;
}

void startQuake(App& app, float amp, float from, float to, int frames, bool hold) {
    startQuake(app.battle.quake, amp, from, to, frames, hold);
}

void endQuake(App::Battle::Quake& q, float amp, float from, float to, int frames) { // EndQuake: dies away from -> to
    startQuake(q, amp, from, to, frames, true);
    q.mode = 1;
}
void endQuake(App& app, float amp, float from, float to, int frames) { endQuake(app.battle.quake, amp, from, to, frames); }

// Quake::update, once a frame: each axis 75..100% of the size now, against its last direction
// (and turned again one time in 11)
void stepQuake(App::Battle::Quake& q) {
    if (!q.mode) {
        q.off[0] = q.off[1] = 0;
        return;
    }
    for (float& o : q.off) {
        q.rng = q.rng * 1103515245u + 12345u;
        const int r = int((q.rng >> 16) & 0x7fff);
        float v = float(r % 26 + 75) * 0.01f * q.amp * q.now * 0.01f;
        if (o * v >= 0) v = -v;
        if (r % 11 == 0) v = -v;
        o = v;
    }
    q.now = q.from + (q.to - q.from) * float(q.frame) / float(q.frames);
    if (++q.frame >= q.frames) {
        q.frame = q.frames;
        if (q.mode == 1 || std::fabs(q.now) < 1e-4f) q.mode = 0;
    }
}

void stepQuake(App& app) { stepQuake(app.battle.quake); }

// SetFade_back(colour, from, to, ms): white over the 3D, 50 a frame of the time (PutMask)
void setFlash(App& app, float from, float to, int ms) {
    App::Battle::Flash& f = app.battle.flash;
    f.on = true;
    f.from = from;
    f.to = to;
    f.at = 0;
    f.total = std::max(1, ms);
}

void stepFlash(App& app) {
    App::Battle::Flash& f = app.battle.flash;
    if (!f.on) return;
    f.alpha = f.from + (f.to - f.from) * float(f.at) / float(f.total);
    f.at += 50;
    if (f.at >= f.total) {
        f.at = f.total;
        if (f.alpha <= 0) f.on = false;
    }
}

// ---- The bosses' staged actions (btlact_430..436) ----
// Each runs its own timeline instead of the gene's: the user's motion, a run of cameras (the
// next set as the last one ends), effects at points or on the targets, sounds, quakes and
// flashes at fixed frames, the hit (btlcalc_prc) at the end of it, then a wait.
bool isStagedAction(int action) { return action == 430 || (action >= 432 && action <= 436); }

bool cameraDone(const App& app) { return !app.battle.camLive; } // btlcamera_prc clears +0xa80 when its frames are over

std::vector<int> turnTargets(const cr::BattleTurn& t) { // the action's targets (+0x7a), each once
    std::vector<int> out; // (not the users' own losses - Risk Breaker's quarter, 0412's give-up)
    for (const cr::BattleHit& h : t.hits)
        if (!h.own && std::find(out.begin(), out.end(), h.target) == out.end()) out.push_back(h.target);
    return out;
}

// que +0x8e: the action's first target - _btlact_attack_init's camera, approach and facing key on it (2.2.0
// decomp 10588-10635), not on a user's own loss pushed first (Risk Breaker: the pair runs in at the monster)
int firstTarget(const cr::BattleTurn& t) {
    for (const cr::BattleHit& h : t.hits)
        if (!h.own) return h.target;
    return t.hits.empty() ? -1 : t.hits[0].target;
}

void startStaged(App& app, const cr::BattleActionDef* a) {
    App::Battle& b = app.battle;
    const cr::BattleTurn& t = b.turn;
    const int user = t.actors[0];
    App::Battle::Actor* u = battleActor(app, user);
    const int cls = a ? a->cls : 8;
    b.stageAction = t.action;
    b.stageCams = 0;
    b.stageWait = 40;
    if (u && !t.hits.empty()) faceFighter(app, *u, firstTarget(t));
    switch (t.action) {
    case 430: // Particle Cannon: the charge, the shot, two flashes
        if (u) playSlot(app, *u, cls, btlui::kSlotIdle);
        b.hitFrame = 140;
        { const size_t n = b.effects.size(); spawnEffectAt(app, 0x59, {500, -4, 500}); keepEffectAngles(app, n); }
        app.sound.play(0x7b);
        setBattleCamera(app, 0x207);
        break;
    case 432: { // the slot's loop, 45 frames past its start
        if (u) playSlot(app, *u, cls, -1);
        const cr::BattleAnim an = app.battleData.anim(u ? u->modelId : 0, cls);
        b.hitFrame = an.loopStart + 45;
        app.sound.play(0xbd);
        setBattleCamera(app, 0x1fe);
        break;
    }
    case 433: // the item cameras, frames 82..83 held
        if (u) playSlot(app, *u, cls, -1), u->loopFrom = 82, u->loopTo = 83;
        b.hitFrame = 112;
        if (user < 2) setBattleCamera(app, b.rules.pair ? 0x55 : user + 0x53);
        else if (monsterSize(app, user) == 1) setBattleCamera(app, b.rules.enemyPair ? 0x58 : user + 0x54, user);
        else setBattleCamera(app, monsterSize(app, user) + 0x57, user);
        spawnEffect(app, effects::kCast, user);
        break;
    case 434:
        if (u) playSlot(app, *u, cls, btlui::kSlotIdle);
        b.hitFrame = 130;
        b.stageWait = 65;
        { const size_t n = b.effects.size(); spawnEffect(app, 0x1e, user); keepEffectAngles(app, n); }
        app.sound.play(0x62);
        setBattleCamera(app, 0x20c);
        break;
    case 435: // frames 103..113 looped
        if (u) playSlot(app, *u, cls, -1), u->loopFrom = 103, u->loopTo = 113;
        b.hitFrame = 163;
        app.sound.play(0xf0);
        app.sound.play(0xf1);
        setBattleCamera(app, 0x215);
        break;
    case 436:
        if (u) playSlot(app, *u, cls, btlui::kSlotIdle);
        b.hitFrame = 61;
        b.stageWait = 50;
        app.sound.play(0x83);
        app.sound.play(0xf0);
        setBattleCamera(app, 0x210);
        break;
    default: break;
    }
}

// One frame of it before the hit (b.turnFrame already counted)
void stepStaged(App& app) {
    App::Battle& b = app.battle;
    const cr::BattleTurn& t = b.turn;
    const int f = b.turnFrame, user = t.actors[0];
    auto nextCamera = [&](int most, int first) {
        if (b.stageCams < most && cameraDone(app)) setBattleCamera(app, first + ++b.stageCams);
    };
    auto onTargets = [&](int id) {
        for (int k : turnTargets(t)) spawnEffect(app, id, k);
    };
    switch (b.stageAction) {
    case 430:
        nextCamera(3, 0x207);
        if (f == 70) {
            { const size_t n = b.effects.size(); spawnEffectAt(app, 0x5a, {500, -2, 500}); keepEffectAngles(app, n); }
            app.sound.play(0x7c);
            startQuake(app, 3, 0, 100, 2, true);
        }
        if (f == 133) endQuake(app, 3, 100, 0, 2);
        if (f == 79) setFlash(app, 0, 255, 150);
        if (f == 82) setFlash(app, 255, 0, 150);
        if (f == 110) setFlash(app, 0, 255, 500);
        if (f == 130) setFlash(app, 255, 0, 500);
        break;
    case 432:
        if (f == 14) app.sound.play(0xc2);
        if (f == 36) {
            if (const App::Battle::Actor* u = battleActor(app, user)) spawnEffectAt(app, 0x19, actorPoint(app, *u, 1206));
            onTargets(0x5b);
            app.sound.play(0x7d);
        }
        break;
    case 434:
        nextCamera(2, 0x20c);
        if (f == 84) app.sound.play(0xd4);
        if (f == 86) setFlash(app, 0, 255, 150);
        if (f == 89) setFlash(app, 255, 0, 150);
        if (f == 100) {
            onTargets(0x22);
            app.sound.play(0x6d);
            startQuake(app, 5, 0, 100, 2, true);
        }
        if (f == 128) endQuake(app, 5, 100, 0, 2);
        break;
    case 435:
        if (f == 103) {
            { const size_t n = b.effects.size(); spawnEffectAt(app, 0x21, {500, 0, 523}); keepEffectAngles(app, n); }
            app.sound.play(0x65);
            setFlash(app, 0, 255, 150);
        }
        if (f == 106) setFlash(app, 255, 0, 150);
        if (f == 133) setFlash(app, 0, 255, 1500);
        if (f == 163) setFlash(app, 255, 0, 500);
        break;
    default: break;
    }
}

// Each frame of the whole action: the cameras that go on past the hit (432, 435, 436); 432's
// fifth looks at its target (0x203, 0x204) or the pair (0x205)
void stepStagedCameras(App& app) {
    App::Battle& b = app.battle;
    if (b.stageAction == 432 && b.stageCams < 5 && cameraDone(app)) {
        ++b.stageCams;
        const int target = b.turn.hits.empty() ? 0 : std::clamp(firstTarget(b.turn), 0, 1);
        setBattleCamera(app, b.stageCams < 5 ? 0x1fe + b.stageCams : b.rules.pair ? 0x205 : target + 0x203);
    }
    if (b.stageAction == 435 && b.stageCams < 5 && cameraDone(app)) setBattleCamera(app, 0x215 + ++b.stageCams);
    if (b.stageAction == 436 && b.stageCams < 4 && cameraDone(app)) setBattleCamera(app, 0x210 + ++b.stageCams);
}

// The hit's own extras, after showHits
void stagedHit(App& app) {
    App::Battle& b = app.battle;
    App::Battle::Actor* u = battleActor(app, b.turn.actors[0]);
    switch (b.stageAction) {
    case 432:
        for (int k : turnTargets(b.turn)) spawnEffect(app, 0x5c, k);
        app.sound.play(0x7e);
        if (u) u->then = btlui::kSlotIdle; // mdl_set_act_nextanm: the loop plays out, then stands
        break;
    case 433: setBattleCamera(app, b.formation + 12); break;
    case 435:
        if (u) u->then = btlui::kSlotIdle;
        break;
    case 436: startQuake(app, 10, 100, 0, 10, false); break;
    default: break;
    }
}

// ---- Battle voices (the voiced releases: 2.2.0's _btlvoice_*, its voicebtl.bin) ----
// A line is the clip table[speaker x 1000 + situation - 1] (sys.evt section +0x40), played with
// tag speaker x 1000: that speaker's previous line stops first. Each fighter cycles through three
// variants of a situation. Nothing here plays without the table (the iPad's 1.0.1 has none).
namespace btlvoice {

bool on(const App& app) { return !app.battleVoiceTable.empty() && app.sound.voiceClips(true) > 0; }

int rnd(App& app, int n) {
    App::Battle& b = app.battle;
    b.voiceRng = b.voiceRng * 1103515245u + 12345u;
    return n > 1 ? int((b.voiceRng >> 16) % uint32_t(n)) : 0;
}

// _btlvoice_speaker: from the fighter's name id (the party: its character x 10, +3 for
// character 7 under flag 0x997 or flag 0x996 on the fourth pair; monsters: id - 1, ids 71..73
// as id + 2 under the same flags): 0..7 the eight characters (x0..x3), 80 -> 11, 90..93 -> 8,
// 94..98 -> 9, 110..113 -> 10, 120..123 -> 12; battle models 7, 87, 88 are always speaker 7
int speaker(const App& app, int k) {
    const App::Battle& b = app.battle;
    if (k < 0 || k > 4) return -1;
    const cr::Fighter& f = b.rules.f[size_t(k)];
    if (!f.present) return -1;
    const bool alt = (app.game.flag(0x996) && app.playerChar / 2 == 3) || app.game.flag(0x997);
    int name, model = -1;
    if (k < 2) {
        name = f.character * 10;
        if (name >= 70 && name < 73 && alt) name += 3;
        for (const App::Battle::Actor& a : b.actors)
            if (a.fighter == k) model = a.modelId;
    } else {
        name = f.monster - 1;
        if (f.monster >= 0x47 && f.monster <= 0x49 && alt) name = f.monster + 2;
        model = f.def ? f.def->model : -1;
    }
    int sp = -1;
    static const int kSpan[8] = {4, 4, 4, 3, 4, 3, 4, 3}; // name ids per character (x0 .. x3)
    if (name >= 0 && name < 80) {
        if (name % 10 < kSpan[name / 10]) sp = name / 10;
    } else if (name >= 80) {
        const int u = name - 80;
        sp = u == 0 ? 11 : u >= 10 && u <= 13 ? 8 : u >= 14 && u <= 18 ? 9 : u >= 30 && u <= 33 ? 10 : u >= 40 && u <= 43 ? 12 : -1;
    }
    if (model == 7 || model == 87 || model == 88) sp = 7;
    return sp;
}

int clipFor(const App& app, int sp, int situation) {
    const size_t i = size_t(sp * 1000 + situation - 1);
    return sp >= 0 && situation > 0 && i < app.battleVoiceTable.size() ? app.battleVoiceTable[i] : 0;
}

// _btlvoice_play: fighter k says the next of its three variants (-1: nothing)
void play(App& app, int k, int a, int b2, int c) {
    if (!on(app)) return;
    const int sp = speaker(app, k);
    if (sp < 0) return;
    int& v = app.battle.voiceVariant[size_t(k)];
    const int s = v == 0 ? a : v == 1 ? b2 : c;
    v = (v + 1) % 3;
    if (s == -1) return;
    const int clip = clipFor(app, sp, s);
    if (clip > 0) app.sound.playBattleLine(clip, sp * 1000);
}

bool living(const App& app, int k) { return k >= 0 && k < 5 && app.battle.rules.f[size_t(k)].alive(); }

// One of two at random among the living (the first if neither)
int eitherOf(App& app, int x, int y) {
    const bool lx = living(app, x), ly = living(app, y);
    if (lx && ly) return rnd(app, 2) ? y : x;
    return ly ? y : x;
}

// _btlvoice_choose_speaker: 99 either of the party; a side acting as a pair, either of it
int chooseSpeaker(App& app, int k) {
    const cr::BattleRules& r = app.battle.rules;
    if (k == 99) return r.f[1].present ? eitherOf(app, 0, 1) : 0;
    if (k >= 2 && r.enemyPair && (k == 2 || k == 3)) return eitherOf(app, 2, 3);
    if (k < 2 && r.pair) return eitherOf(app, 0, 1);
    return k;
}

void playSide(App& app, int k, int a, int b2, int c) { play(app, chooseSpeaker(app, k), a, b2, c); }

// _btlvoice_pick_speakers, each round: who of each side speaks for it
void pickSpeakers(App& app) {
    App::Battle& b = app.battle;
    for (bool& s : b.voiceSpeaks) s = false;
    b.voiceSpeaks[size_t(eitherOf(app, 0, 1))] = true;
    std::vector<int> foes;
    for (int k = 2; k < 5; ++k)
        if (living(app, k)) foes.push_back(k);
    if (!foes.empty()) b.voiceSpeaks[size_t(foes[size_t(rnd(app, int(foes.size())))])] = true;
}

// _btlvoice_side_is_pair (79035-79057): a side acting as a pair - or any side under an action over a
// whole side (the action's scope 3 / 5) - reacts through its picked speaker only
bool mayReact(const App& app, int k) {
    const App::Battle& b = app.battle;
    const bool pairSide = k < 2 ? b.rules.pair : b.rules.enemyPair;
    const cr::BattleActionDef* act = app.battleActions.get(b.turn.action);
    const bool allOf = act && (act->scope == 3 || act->scope == 5);
    return (!pairSide && !allOf) || b.voiceSpeaks[size_t(k)];
}

// FUN_00026e5c: how many of fighter k's side are still on the field - _IsDead counts the left (+0x93)
// and those whose models are gone, not the HP, so the ones an action fells still count while it lands
int sideStanding(const App& app, int k) {
    const App::Battle& b = app.battle;
    int n = 0;
    for (const App::Battle::Actor& a : b.actors)
        if (a.fighter >= 0 && a.fighter < 5 && (a.fighter < 2) == (k < 2) && b.rules.f[size_t(a.fighter)].present && !a.fading) ++n;
    return n;
}
void markStanding(App& app) { // (taken before an action's blows start the fallen fading)
    app.battle.voiceStanding[0] = sideStanding(app, 0);
    app.battle.voiceStanding[1] = sideStanding(app, 2);
}

// _btlvoice_listed_battle: in these battles the last monster standing says 0x1f..0x21 as it falls
// (FUN_00026e5c(k) == 1: the side's last)
bool listedBattle(const App& app, int k) {
    static const int kList[] = {260, 261, 262, 280, 281, 282, 300, 374, 302, 320, 321, 322};
    return k >= 2 && app.battle.voiceStanding[1] == 1 &&
           std::find(std::begin(kList), std::end(kList), app.battle.start.group) != std::end(kList);
}

// _btlvoice_hp_change (79291-79364, from btlcalc_chk_dead): fighter k took a blow of `damage` (the
// damage, not the HP: 0 for a miss, a guard, the invincible): nothing taken 0x22..0x24, felled
// 0x1c..0x1e (0x1f..0x21 the listed battles' last foe), standing above a quarter of its HP 0x16..0x18,
// else 0x19..0x1b. Action 0x1b5 only while the HP left equals the damage
void hpChange(App& app, int k, int damage) {
    if (!on(app) || speaker(app, k) < 0 || !mayReact(app, k)) return;
    const cr::Fighter& f = app.battle.rules.f[size_t(k)];
    if (app.battle.turn.action == 0x1b5 && f.hp != damage) return;
    if (damage == 0) play(app, k, 0x22, 0x23, 0x24);
    else if (f.hp <= 0) {
        if (listedBattle(app, k)) play(app, k, 0x1f, 0x20, 0x21);
        else play(app, k, 0x1c, 0x1d, 0x1e);
    } else if (f.hp > f.hpMax / 4) play(app, k, 0x16, 0x17, 0x18);
    else play(app, k, 0x19, 0x1a, 0x1b);
}

void healed(App& app, int k) { // _btlvoice_healed: to full, or not
    if (!on(app) || speaker(app, k) < 0 || !mayReact(app, k)) return;
    const cr::Fighter& f = app.battle.rules.f[size_t(k)];
    if (f.hp >= f.hpMax) play(app, k, 0x28, 0x29, 0x2a);
    else play(app, k, 0x25, 0x26, 0x27);
}

// _btlvoice_attack (79620-79725): only when the action has no line of its own (the record's party slot
// +10 for a member, the monster slot +0x10 for a monster, is -1 - else _btlvoice_turn's line was it):
// how the targets fared - 0 struck, 1 a critical, 2 one fell, 3 nothing taken or fended off (type 6),
// 4 one fell that was the last of its side (FUN_00026e5c(target) == 1)
void attack(App& app, const cr::BattleTurn& t) {
    if (!on(app) || speaker(app, t.actors[0]) < 0) return;
    if (const cr::BattleActionDef* a = app.battleActions.get(t.action); a && a->voice[t.actors[0] < 2 ? 0 : 3] != -1) return;
    int how = 0;
    for (const cr::BattleHit& h : t.hits) {
        const cr::Fighter& f = app.battle.rules.f[size_t(std::clamp(h.target, 0, 4))];
        if (h.amount == 0 || h.type == cr::kHitGuard) how = std::max(how, 3);
        if (f.hp <= 0) how = std::max(how, sideStanding(app, h.target) == 1 ? 4 : 2);
        else if (h.type == cr::kHitCritical && how == 0) how = 1;
    }
    static const int kLine[5] = {0x2e, 0x31, 0x34, 0x37, 0x3a};
    playSide(app, t.actors[0], kLine[how], kLine[how] + 1, kLine[how] + 2);
}

int halfOrLess(const App& app, int k) {
    const cr::Fighter& f = app.battle.rules.f[size_t(k)];
    return f.hp <= f.hpMax / 2;
}

// _btlvoice_command_hurt: the pair's command, one of it at half HP or less (10..12) or not (7..9)
void commandHurt(App& app, int member) {
    const bool hurt = (living(app, 0) && halfOrLess(app, 0)) || (living(app, 1) && halfOrLess(app, 1));
    playSide(app, member, hurt ? 10 : 7, hurt ? 11 : 8, hurt ? 12 : 9);
}

// _btlvoice_command: a member's commands open (once a round): 1..3, at half HP or less 4..6,
// the partner down 13..15 / 16..18; battles 356..359 the pair's lines instead
void command(App& app, int member) {
    App::Battle& b = app.battle;
    if (!on(app) || member < 0 || member > 1 || b.commandSaid[size_t(member)]) return;
    b.commandSaid[size_t(member)] = true;
    if (b.rules.pair) {
        b.commandSaid[0] = b.commandSaid[1] = true;
        commandHurt(app, member);
        return;
    }
    const int partner = member ^ 1;
    const int idx = halfOrLess(app, member) | ((b.rules.f[size_t(partner)].present && !living(app, partner)) ? 2 : 0);
    if (idx < 2 && b.start.group >= 0x164 && b.start.group <= 0x167) { commandHurt(app, member); return; }
    static const int kLine[4] = {1, 4, 13, 16};
    playSide(app, member, kLine[idx], kLine[idx] + 1, kLine[idx] + 2);
}

// _btlvoice_turn (from btlque_make_que, an entry's first go): the actor's action lines (the record's
// +10.. for the party and the voiced characters, +16.. for the other monsters)
void turn(App& app, int k, int action) {
    if (!on(app)) return;
    const cr::BattleActionDef* a = app.battleActions.get(action);
    const int sp = speaker(app, k);
    if (!a || sp < 0) return;
    const bool own = sp < 8 || k < 2;
    const int who = own ? chooseSpeaker(app, k) : (app.battle.rules.enemyPair && (k == 2 || k == 3) ? eitherOf(app, 2, 3) : k);
    const int* v = own ? a->voice : a->voice + 3;
    play(app, who, v[0], v[1], v[2]);
}

// _btlvoice_victory (79844-79988): scripted battles' own lines (0xc93d0, by the pair), else said by
// the member who acted last (+0xaea; either of the pair in pair mode): 500..502, "hurt" (that member
// down or at a quarter of its HP or less - in pair mode either member) 503..505, +6 when a strong-
// growth monster was in the battle (+0xafb: 506..511)
void victory(App& app) {
    if (!on(app)) return;
    App::Battle& b = app.battle;
    static const short kScripted[4][13][2] = {
        {{260, 621}, {261, 622}, {340, 623}, {348, 624}, {344, 625}, {352, 626}, {356, 627}, {357, 627}, {358, 627}, {359, 627}, {368, 628}, {262, 629}, {-1, 0}},
        {{280, 621}, {281, 622}, {341, 623}, {283, 624}, {349, 625}, {345, 626}, {353, 627}, {356, 628}, {357, 628}, {358, 628}, {359, 628}, {368, 629}, {282, 630}},
        {{300, 621}, {374, 622}, {342, 623}, {346, 624}, {377, 625}, {354, 626}, {356, 627}, {357, 627}, {358, 627}, {359, 627}, {368, 628}, {302, 629}, {-1, 0}},
        {{320, 621}, {321, 622}, {343, 623}, {351, 624}, {347, 625}, {355, 626}, {356, 627}, {357, 627}, {358, 627}, {359, 627}, {368, 628}, {322, 629}, {-1, 0}}};
    const int route = std::clamp(app.playerChar / 2, 0, 3);
    int line = 0;
    for (const auto& e : kScripted[route])
        if (e[0] == b.start.group) line = e[1];
    const int leader = std::clamp(b.rules.soloMember(), 0, 1);
    const int who = chooseSpeaker(app, leader);
    const int sp = speaker(app, who);
    if (sp < 0) return;
    if (!line) {
        bool hurt = false, strong = false;
        for (int k = 0; k < 2; ++k)
            if (b.rules.f[size_t(k)].present && (b.rules.pair || k == leader))
                hurt |= !living(app, k) || b.rules.f[size_t(k)].hp <= b.rules.f[size_t(k)].hpMax / 4;
        for (int k = 2; k < 5; ++k)
            if (const cr::Fighter& f = b.rules.f[size_t(k)]; f.present && f.def && f.def->strongGrowth) strong = true;
        int& v = b.voiceVariant[size_t(who)];
        line = 500 + (strong ? 6 : 0) + (hurt ? 3 : 0) + v;
        v = (v + 1) % 3;
    }
    const int clip = clipFor(app, sp, line);
    if (clip > 0) app.sound.playBattleLine(clip, sp * 1000);
}

} // namespace btlvoice

// _btlact_attack_init (2.2.0 0x15548; 10669-10760, ARM 0x15964..0x15b48; 10833-10960): an attack's
// swing slot. A user over size 3 (its def +0x1a) swings 9 where it stands (state 0x76c); a member of
// model 7 (+0xa6) or with a class-10 action (+0x17), or an enemy of model 0x28..0x2a / 0x57 / 0x58,
// 10; model 6 in pair mode (+0xae8) or 0x55 / 0x56 in an enemy pair (+0xaec) 0xd; anyone else 8 -
// a monster's class-10 action and 302 / 303's class 9 aren't read. The pair's second (unit 1 / 3)
// swings 8 at its own spot, or with model 7 / 0x57 / 0x58 the joint 0xd beside the first's.
int attackSlot(const App& app, const cr::BattleTurn& t, int k, int model, int cls) {
    const App::Battle& b = app.battle;
    const int user = t.actors[0];
    if (k == 1) return (user < 2 ? model == 7 : model == 0x57 || model == 0x58) ? 0xd : 8;
    if (user >= 2 && monsterSize(app, user) > 3) return 9;
    if (user < 2 ? model == 7 || cls == 10 : (model >= 0x28 && model <= 0x2a) || model == 0x57 || model == 0x58) return 10;
    if ((model == 6 && b.rules.pair) || ((model == 0x55 || model == 0x56) && b.rules.enemyPair)) return 0xd;
    return 8;
}

void startTurn(App& app) {
    App::Battle& b = app.battle;
    const cr::BattleTurn& t = b.turn;
    if (!t.cancelled) { // btlque_make_que (31461-31465), each action: an entry's first go (not a counter's)
        // says its action line, and the sides' speakers are picked afresh
        b.voiceRound = b.rules.round;
        if (!t.repeat && !t.counter) btlvoice::turn(app, t.actors[0], t.action);
        btlvoice::pickSpeakers(app);
    }
    // btlact_escape_init (44998-45002): the pair flag set for the call - either member says it
    if (t.kind == cr::kCmdEscape && t.escaped) btlvoice::playSide(app, 99, 0x13, 0x14, 0x15);
    else if (t.kind == cr::kCmdAttack) btlvoice::attack(app, t);                 // btlact_attack_init
    if (t.kind == cr::kCmdAttack) { // btlact_attack_init's __effgo: actions 302 / 303's own, each attacker's swing
        if (t.action == 0x12e) app.sound.play(0xbb);
        else if (t.action == 0x12f) app.sound.play(0xbd);
        for (int k : t.actors) {
            const App::Battle::Actor* u = k >= 0 ? battleActor(app, k) : nullptr;
            if (!u) continue;
            const int m = u->modelId; // (G+0xa2: kBattleModelFile's index; the pair's j001..j008 their +0xa6 too)
            if (k < 2) {
                if (m != 7) app.sound.play(0x4a + uint32_t(m));
            } else if ((m < 8 || m > 10) && m != 0x57 && m != 0x58) // (m101 .. m103 sound their own; m008a / b none)
                app.sound.play(uint32_t(m < 0xd ? m + 0x77 : m < 0x49 ? (m - 0xd) / 3 + 0x84 : (m - 0x49) / 2 + 0x4a));
        }
    }
    b.turnFrame = 0;
    b.nodeFx = -1;
    b.hitsShown = false;
    b.hitFrame = 20;
    b.bounceAt = -1;
    if (t.kind == cr::kCmdEscape) {
        setBanner(app, sysText(app, t.escaped ? 57000 : 57001));
        b.hitFrame = 0; // btlact_escape: away - 30 frames (2.2.0; 1.0.1 10, no line to wait for)
        return;          // then 40; stayed - 40
    }
    b.idleSince = -1;
    if (t.cancelled) { // btlact_none: its message for 40 frames in place of the action, sixty frames on the user's
        // camera (_btlact_none_init 0x6d504: +0xae0 = 25, the item's cast camera - 0x53 + member on it, the
        // pair's 0x55, a size-1 monster's its slot + 0x54 (an enemy pair's 0x58), a bigger one's size + 0x57)
        setBanner(app, sysText(app, b.turn.message), 40);
        b.hitFrame = 0;
        const int u = t.actors[0];
        b.camHold = 25;
        if (u >= 0) castCamera(app, cr::kCmdItem, u);
        return;
    }
    const cr::BattleActionDef* a = app.battleActions.get(t.action);
    // _btlque_make_que (0x342d8): a counter is announced as action 0xdc's name, "Counter", whatever blow it
    // is (font_set_utf16(11, 3, 0xdb, 40)); any other entry once, while its announced flag (+0x3e0) is
    // clear - a repeated go of the same entry (Asura Lotus's strikes) leaves the banner alone; btlai_prc's
    // re-choice clears the flag (named afresh)
    if (!t.repeat || t.counter) setBanner(app, sysText(app, 54000 + (t.counter ? 0xdb : t.action - 1)));
    for (int k : t.actors) // the gene's MP leaves as it's cast
        if (k >= 0) b.shownMp[k] = b.rules.f[size_t(k)].mp;
    b.stageAction = 0;
    if (isStagedAction(t.action) && t.actors[0] >= 2) { // btlact_430_init.. instead of btlact_gene_init
        startStaged(app, a);
        return;
    }
    cameraForTurn(app, t);
    if (t.kind == cr::kCmdGene || t.kind == cr::kCmdItem) // btlact_gene_init / item_init: the casting glow
        for (int k : t.actors)
            if (k >= 0) spawnEffect(app, effects::kCast, k);
    if (t.kind == cr::kCmdGene) { // btlact_gene_init: __effgo 0x5b (m103's own 0xbb first)
        const App::Battle::Actor* u = battleActor(app, t.actors[0]);
        if (u && u->modelId == 10) app.sound.play(0xbb);
        app.sound.play(0x5b);
    } else if (t.kind == cr::kCmdItem) app.sound.play(0x5c); // btlact_item_init
    if (t.kind == cr::kCmdItem && t.item > 0) { // the bag gives one up
        auto it = app.game.items.find(t.item);
        if (it != app.game.items.end() && --it->second <= 0) app.game.items.erase(it);
    }
    const int target = !t.barriers.empty() ? t.barriers[0] : firstTarget(t); // (reflected: at the barrier)
    // (a monster over size 3 swings where it stands, unturned: _btlact_attack_init's 0x76c branch)
    const bool inPlace = t.kind == cr::kCmdAttack && t.actors[0] >= 2 && monsterSize(app, t.actors[0]) > 3;
    const bool melee = a && a->motion == 1 && target >= 0 && (target < 2) != (t.actors[0] < 2) && !inPlace;
    for (int k = 0; k < 2; ++k) {
        App::Battle::Actor* u = t.actors[k] >= 0 ? battleActor(app, t.actors[k]) : nullptr;
        if (!u) continue;
        int slot = a && a->cls >= 8 && a->cls < cr::kBattleAnimSlots ? a->cls : 8;
        if (t.kind == cr::kCmdAttack) slot = attackSlot(app, t, k, u->modelId, a ? a->cls : -1);
        // (the joint 0xd's second stays at home, turned to the target: mdl_ang_y(it, target), no setpos)
        const bool joint = t.kind == cr::kCmdAttack && k == 1 && slot == 0xd;
        const bool moves = melee && !joint;
        if (target >= 0 && t.kind == cr::kCmdAttack && !moves && !inPlace) faceFighter(app, *u, target); // (gene_init / item_init turn no one)
        u->turnHome = false, u->approached = false;
        if ((t.kind == cr::kCmdGene || t.kind == cr::kCmdItem) && !melee) {
            // _btlact_gene_init / _btlact_item_init (0x3ef28 / 0x4033c): _mdl_set_act_motion_loop(slot, 2,
            // loop start, loop end) - played to the loop's end, then its loop over and over (the whole
            // slot when it has none) till another motion is set (the round's end, a blow)
            playSlot(app, *u, slot, -1);
            const cr::BattleAnim ca = app.battleData.anim(u->modelId, u->slot);
            if (ca.loopEnd > ca.loopStart) u->loopFrom = ca.loopStart, u->loopTo = ca.loopEnd;
        } else
            playSlot(app, *u, slot, moves ? btlui::kSlotReturn : btlui::kSlotIdle);
        const cr::BattleAnim an = app.battleData.anim(u->modelId, slot);
        // (_btlact_gene_init / _btlact_item_init: the hit at the cast slot's loop start + 30 - the item's
        // slot 12, the gene's its class)
        const bool cast = (t.kind == cr::kCmdGene || t.kind == cr::kCmdItem) && !melee;
        if (k == 0)
            b.hitFrame = cast ? app.battleData.anim(u->modelId, t.kind == cr::kCmdItem ? 12 : slot).loopStart + 30
                         : an.hit > 0 ? an.hit
                         : t.kind == cr::kCmdAttack ? 18
                                                    : 30;
        u->moveFrom = u->moveTo = 0;
        if (moves) { // an attack: _btlact_attack_init's btlmove_setpos_angy to the formation table's spot (attackSpot)
            const App::Battle::Actor* tv = battleActor(app, target);
            cr::Vec3 spot;
            if (tv && t.kind == cr::kCmdAttack && attackSpot(app, t.actors[k], k, target, spot)) {
                u->from = u->pos;
                u->to = {spot.x, u->pos.y, spot.z};
                // (que +0x10 = FUN_00041b90: the slot's move end - the pair's second member's when it moves)
                if (an.moveEnd > 0) b.attackCam2At = an.moveEnd;
            } else if (tv) { // (the port's own for a melee gene: up to the target, a step short of it)
                const LoadedModel &lu = b.models[u->model], &lt = b.models[tv->model];
                cr::Vec3 d = u->home - tv->home;
                const float len = std::max(1.0f, std::sqrt(d.x * d.x + d.z * d.z));
                const float reach = 0.6f * (lu.radius * u->scale + lt.radius * tv->scale) + (k ? 18.0f : 0.0f);
                u->from = u->pos;
                u->to = tv->home + cr::Vec3{d.x / len * reach, 0, d.z / len * reach};
                if (k == 1) u->to = u->to + cr::Vec3{d.z / len * 18, 0, -d.x / len * 18}; // beside the first
            }
            if (tv) {
                u->moveFrom = an.moveEnd > an.moveStart ? an.moveStart : 0;
                u->moveTo = an.moveEnd > an.moveStart ? an.moveEnd : std::max(1, b.hitFrame - 4);
                // (_btlact_attack_init 0x15548: btlmove_setpos_angy, then mdl_ang_y(from, to) into both the
                // wanted and the shown yaw - kept for the way back - and btlmove_prc turns it home over the move)
                if (u->to.x != u->from.x || u->to.z != u->from.z) u->yaw = u->wantYaw = std::atan2(u->to.x - u->from.x, u->to.z - u->from.z);
                u->approachYaw = u->wantYaw, u->approached = true, u->turnHome = true;
            }
        }
    }
    // (_btlact_attack_init 10838-10900 / 10920-10958: the pair's blow lands - que +0xb0 - at the second's
    // hit frame when theirs is the joint 0xd or the second is model 1 (an enemy pair's 0x4b / 0x4c))
    if (t.kind == cr::kCmdAttack && t.actors[1] >= 0)
        if (const App::Battle::Actor* u2 = battleActor(app, t.actors[1])) {
            const int m = u2->modelId;
            const int slot2 = attackSlot(app, t, 1, m, a ? a->cls : -1);
            const bool second = slot2 == 0xd || (t.actors[0] < 2 ? m == 1 : m == 0x4b || m == 0x4c);
            if (const int h = app.battleData.anim(m, slot2).hit; second && h > 0) b.hitFrame = h;
        }
}

// The hits land: the numbers, the targets' reactions, the HP the HUD shows
void releasePair(App::Battle& b) { // the calc's pair flags, as its hit comes (see pairHeld)
    if (b.pairHeld) b.rules.pair = b.pairAfterHit, b.rules.enemyPair = b.enemyPairAfterHit;
    b.pairHeld = false;
}

void showHits(App& app) {
    releasePair(app.battle); // (2.2.0 _btlcalc_chk_dead 24534 / 24613 at the hit: _btlact_attack calls _btlcalc_prc there)
    App::Battle& b = app.battle;
    b.hitsShown = true;
    if (b.turn.message > 0) { // stolen Oz, a monster running, Analyze, the barrier, the Apples
        std::string name = b.turn.message == 57032 ? sysText(app, b.turn.messageValue)
                           : b.turn.message == 57005 || b.turn.message == 57049 || b.turn.message == 57050
                               ? itemName(app, b.turn.messageValue)
                               : std::string();
        if (b.turn.message == 57000 || b.turn.message == 57001)
            if (App::Battle::Actor* u = battleActor(app, b.turn.actors[0])) name = u->name;
        std::vector<std::vector<std::string>> pages;
        const int value = b.turn.messageValue;
        if (app.systemText.message(b.turn.message, app.language, [&](int) { return value; }, pages, nullptr,
                                   [&](int) { return name; }) &&
            !pages.empty() && !pages[0].empty())
            setBanner(app, pages[0][0], btlui::kMessageFrames);
    }
    for (const cr::BattleNotice& n : b.turn.notices) // btl_add_msgque: one waiting per fighter
        if (std::none_of(b.notices.begin(), b.notices.end(), [&](const cr::BattleNotice& q) { return q.fighter == n.fighter; }) &&
            b.notices.size() < 5)
            b.notices.push_back(n);
    if (b.turn.gone) // the monster leaves
        if (App::Battle::Actor* u = battleActor(app, b.turn.actors[0])) u->fading = true;
    btlvoice::markStanding(app); // (FUN_00026e5c's count as the blows land)
    for (const cr::BattleHit& h : b.turn.hits) {
        App::Battle::Actor* a = battleActor(app, h.target);
        const cr::Fighter& f = b.rules.f[size_t(h.target)];
        if (h.revived) btlvoice::playSide(app, h.target, 0x2b, 0x2c, 0x2d); // btlcalc_0424
        else if (!h.mp && h.type != cr::kHitQuiet) {
            if (h.amount < 0) btlvoice::healed(app, h.target);           // btlcalc_dmg_hp
            else btlvoice::hpChange(app, h.target, h.amount);            // btlcalc_chk_dead (a miss's 0 too)
        }
        b.shownHp[h.target] = f.hp;
        b.shownMp[h.target] = f.mp;
        if (!a || h.type == cr::kHitQuiet) continue;
        for (App::Battle::Popup& p : b.popups) { // __dmg_set: the first free of five slots, else none
            if (p.on) continue;
            p = App::Battle::Popup();
            p.on = true;
            p.at = actorHead(app, *a);
            p.amount = h.amount;
            p.type = h.type;
            p.mp = h.mp;
            p.timer = btlui::kPopupTime;
            break;
        }
        // _btlcalc_chk_dead (24466-24560) / _btlcalc_0424 (23084): a revive gets up (7, then 0); a fall -
        // a member hit, then down looping (5, 6), a monster hit, then standing as it fades (5, 0), a
        // strong-growth one down (5, 6) when of size 1, else standing (5, 0); a blow on one standing -
        // 5 then 0, unless nothing was taken (a miss, fended off, the invincible's 0) or its model is
        // defending (slot 4 playing)
        if (h.revived) playSlot(app, *a, btlui::kSlotRise, btlui::kSlotIdle);
        else if (h.fell) {
            const cr::MonsterDef* def = a->enemy ? f.def : nullptr;
            if (def && def->strongGrowth)
                playSlot(app, *a, btlui::kSlotHit, def->size == 1 ? btlui::kSlotDown : btlui::kSlotIdle);
            else {
                if (a->enemy) a->fading = true; // monsters fade out over 20 frames (mdl_set_alpha(.., 0, 0x14))
                playSlot(app, *a, btlui::kSlotHit, a->enemy ? btlui::kSlotIdle : btlui::kSlotDown);
            }
        } else if (h.amount > 0 && a->slot != btlui::kSlotGuard)
            playSlot(app, *a, btlui::kSlotHit, btlui::kSlotIdle);
    }
    // The calcs' sounds (__effgo in btlcalc_*, a target at a time; each heard once an action)
    std::vector<uint32_t> sounds;
    auto sound = [&](uint32_t id) {
        if (std::find(sounds.begin(), sounds.end(), id) == sounds.end()) sounds.push_back(id);
    };
    auto healSound = [&](int healed) { sound(healed < 2000 ? 0x57 : healed < 5000 ? 0x58 : 0x59); }; // (by the HP healed)
    bool areaSounded = false;
    auto calcSound = [&](int calc, const cr::BattleHit& h, int user, int ue, bool area, int element) {
        const int t = h.target;
        switch (calc) {
        case 0: // 0404 (22104-22132): fended off (type 6) 0x5d; any other result - a miss, no damage, the
            // invincible's 0 too - a critical 0x56, the weapon's element 0x74 + it, else 0x53..0x55
            if (h.type == cr::kHitGuard) sound(0x5d);
            else sound(h.type == cr::kHitCritical ? 0x56 : ue > 0 ? 0x74 + uint32_t(ue) : 0x53 + uint32_t(b.rules.random(3)));
            break;
        case 1: // 0406 (22252-22273): whenever the damage is 0 or more - an element's gene 0x64 + 2e a target,
            // its burst over a side (scope 5) 0x65 + 2e once; actions 0x1b2 / 0x1b3 none
            if (element > 0 && h.amount >= 0 && b.turn.action != 0x1b2 && b.turn.action != 0x1b3) {
                if (!area) sound(0x64 + 2 * uint32_t(element));
                else if (!areaSounded) sound(0x65 + 2 * uint32_t(element)), areaSounded = true;
            }
            break;
        case 2: case 3: case 4: case 5: case 6: sound(0x60); break; // 0407 .. 0411
        case 7: if (t != user) sound(0x6d); break;                   // 0412
        case 8: // 0413: the one drained 0x6f, the drinker as healed
            if ((t < 2) != (user < 2)) sound(0x6f);
            else if (h.amount < 0) healSound(-h.amount);
            break;
        case 12: case 15: case 17: if (h.amount < 0) healSound(-h.amount); break; // 0417, 0420, 0422
        case 13: case 16: case 18: case 22: sound(0x6c); break;      // 0418, 0421, 0423, 042700
        case 14: sound(0x59); break;                                 // 0419
        case 19: sound(0x71); break;                                 // 0424
        case 23: sound(b.turn.action == 0xe8 ? 0x70 : 0x6c); break;  // 042701
        case 24: sound(0x79); break;                                 // 0428
        case 26: sound(0x78); break;                                 // 030205
        case 27: case 28: case 29: case 30: case 32: case 33: case 34: case 35: case 36: sound(0x6e); break; // 030206 .. 0602
        case 31: sound(0x6f); break;                                 // 030210
        default: break;
        }
    };
    // The calcs' effects (eff3d_set_btl in btlcalc_*)
    const cr::BattleActionDef* calcAct = app.battleActions.get(b.turn.action);
    if (calcAct && calcAct->calc == 10) sound(0x53 + uint32_t(b.rules.random(3))); // 0415: once, after its effects
    if (const cr::BattleActionDef* act = calcAct) {
        const int user = b.turn.actors[0];
        const int ue = user >= 0 ? b.rules.f[size_t(user)].element : 0;
        bool area = false;
        for (const cr::BattleHit& h : b.turn.hits) {
            using namespace effects;
            const int t = h.target;
            int id = -1;
            switch (act->calc) {
            case 0: // btlcalc_0404 (22104-22132): the element's hit, or the plain / critical one - for every
                // result but the fended off (type 6: its sound only); a miss, no damage or the invincible's 0
                // still jolt and show it
                if (h.type == cr::kHitGuard) break;
                id = h.type == cr::kHitCritical ? kCritical : ue > 0 ? 0x25 + 3 * ue : kHit;
                startQuake(app, 3, 100, 0, 5, false); // and the screen jolts (StartQuake(3, 100, 0, 5))
                break;
            case 1: // 0406 (22252-22273): elemental genes - one burst over a whole side (scope 5), else on each
                // target; the jolt, the effect and the sound whenever the damage is 0 or more (0x1b2 / 0x1b3 the jolt only)
                if (h.amount < 0) break;
                startQuake(app, 3, 100, 0, 5, false);
                if (act->element <= 0 || b.turn.action == 0x1b2 || b.turn.action == 0x1b3) break;
                if (act->scope == 5) {
                    if (!area) spawnEffectAt(app, 0x24 + 3 * act->element, {500, 0, t < 2 ? 523.0f : 472.0f});
                    area = true;
                } else
                    id = kGene + 3 * act->element;
                break;
            case 2: case 3: case 4: case 5: case 6: case 9: case 10: id = kFixed; break;
            case 7: id = t == user ? -1 : kSacrifice; break;
            case 8: id = (t < 2) == (user < 2) ? kHeal : kDrain; break;
            case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 22: case 23: case 35: case 36: id = kHeal; break;
            case 19: id = kRevive; break;
            case 24: id = kAnalyze; break;
            case 25: id = act->element > 0 ? 0x31 + act->element : -1; break;
            case 26: id = kBuff; break;
            case 27: case 28: case 29: case 30: case 32: case 33: case 34: id = kStatus; break;
            case 31: id = kDrain; break;
            default: break;
            }
            if (id >= 0) spawnEffect(app, id, t);
            calcSound(act->calc, h, user, ue, act->scope == 5, act->element);
            if (h.fell && t >= 2) { // btlcalc_chk_dead: monsters without strong growth burst
                const cr::Fighter& f = b.rules.f[size_t(t)];
                if (f.def && !f.def->strongGrowth) spawnEffect(app, kFall, t), app.sound.play(0x5e);
            }
        }
    }
    for (uint32_t id : sounds) app.sound.play(id);
    if (b.rules.breakGauge != b.gaugeShown && b.rules.breakGauge != 0 && (b.gaugeShown == 0 || (b.gaugeShown > 0) != (b.rules.breakGauge > 0)))
        b.breakPop = 5.0f; // btlbreak_set: the label pops when a side takes the Break
}

// btlbreak_effect_init / _prc: two white stripes at 30 degrees (alpha up to 152), "Player" and
// "Enemy" sliding along them (window_hd (704, 328) / (864, 328), 160 x 104), "BREAK!!" (512, 432,
// 512 x 72) and "The battle situation changed" (512, 504, 512 x 32) sliding in from opposite
// sides on 480 sin(t): 5 frames in, 30 held (at 15 the other side takes the Break, sound 0x7a),
// 5 out (sound 0x48, or 0x49 when the enemies hold it). Sound 0x47 as it starts.
void startBreakFx(App& app) {
    App::Battle::BreakFx& fx = app.battle.breakFx;
    fx = App::Battle::BreakFx();
    fx.on = true;
    app.sound.play(0x47);
}
bool stepBreakFx(App& app) {
    App::Battle& b = app.battle;
    App::Battle::BreakFx& fx = b.breakFx;
    const float deg = 3.14159265f / 180.0f;
    if (fx.state == 0) {
        fx.t += 18;
        fx.slide = int(480.0f * std::sin(float(fx.t) * deg));
        fx.alpha = std::min(152, fx.alpha + 64);
        fx.pos += 96;
        if (fx.t >= fx.limit) fx.t = 0, fx.limit = 30, fx.state = 10;
    } else if (fx.state == 10) {
        ++fx.t;
        if (fx.t == fx.limit / 2) {
            b.rules.takeBreak();
            b.breakPop = 5.0f;
            app.sound.play(0x7a);
        }
        if (fx.t >= fx.limit) {
            fx.t = 0, fx.limit = 90, fx.state = 0x3e6;
            app.sound.play(b.rules.breakGauge < 1 ? 0x49 : 0x48);
        }
    } else if (fx.state == 0x3e6) {
        fx.t += 18;
        fx.slide = int(480.0f * std::sin(float(fx.t) * deg) + 480.0f);
        fx.alpha = std::max(0, fx.alpha - 64);
        fx.pos += 96;
        if (fx.t >= fx.limit) fx.state = 999;
    } else
        return true;
    return false;
}
void drawBreakFx(App& app, cr::MenuPainter& m) {
    const App::Battle::BreakFx& fx = app.battle.breakFx;
    if (!fx.on) return;
    const float a = float(fx.alpha) / 255.0f, white[4] = {1, 1, 1, a};
    if (app.renderer.phoneLayout()) { // 2.2.0 (the Retina path): the stripes into the iPhone screen's
        // corners ((0, 160)-(280, 0)-(0, 216)-(376, 0), (160, 320)-(480, 140)-(256, 320)-(480, 192)),
        // "Player" at (pos cos 30 + 130, 10 - pos sin 30), "Enemy" at (234 - pos cos 30, pos sin 30
        // + 208), "BREAK!!" (528, 432, 480 x 72) at (slide - 480, 120), the line (528, 504, 480 x 32)
        // at (480 - slide, 192). Android 1.1.1's btlbreak_effect_prc (0x3ccb0): the first stripe and "Player"
        // (- ex, - ey) into the phone's top left, the second and "Enemy" (+ ex, + ey) into its bottom right
        // (decomp 22999-23027; the labels' fsub / fadd at 0x3ce44 / 0x3ce90); "BREAK!!" and the line centred
        const float ex = androidEdgeX(app), ey = androidEdgeY(app);
        for (float x = 0; x < 480; x += 2) {
            const float top1 = 160 - 160 * x / 280, bot1 = 216 - 216 * x / 376;
            if (bot1 > top1 && bot1 > 0) m.quad(x + 16 - ex, std::max(top1, 0.0f) + 32 - ey, 2, bot1 - std::max(top1, 0.0f), white, white);
            if (x >= 160) {
                const float top2 = std::max(140.0f, 320 - 180 * (x - 160) / 320);
                const float bot2 = x < 256 ? 320 : std::max(192.0f, 320 - 128 * (x - 256) / 224);
                if (bot2 > top2) m.quad(x + 16 + ex, top2 + 32 + ey, 2, bot2 - top2, white, white);
            }
        }
        const float c = std::cos(30 * 3.14159265f / 180), s = std::sin(30 * 3.14159265f / 180), pos = float(fx.pos);
        m.piece(pos * c + 130 + 16 - ex, 10 - pos * s + 32 - ey, 704, 328, 160, 104);
        m.piece(234 - pos * c + 16 + ex, pos * s + 208 + 32 + ey, 864, 328, 160, 104);
        m.piece(float(fx.slide) - 480 + 16, 120 + 32, 528, 432, 480, 72);
        m.piece(480 - float(fx.slide) + 16, 192 + 32, 528, 504, 480, 32);
        return;
    }
    // tex2d -> menu units: + (16, 32). The stripes as 2-unit columns between their edges
    for (float x = -16; x < 496; x += 2) {
        const float top1 = 128 - 160 * (x + 16) / 280, bot1 = 184 - 216 * (x + 16) / 376;
        if (bot1 > top1 && bot1 > -32) m.quad(x + 16, std::max(top1, -32.0f) + 32, 2, bot1 - std::max(top1, -32.0f), white, white);
        if (x >= 176) {
            const float top2 = std::max(172.0f, 352 - 180 * (x - 176) / 320);
            const float bot2 = x < 272 ? 352 : std::max(224.0f, 352 - 128 * (x - 272) / 224);
            if (bot2 > top2) m.quad(x + 16, top2 + 32, 2, bot2 - top2, white, white);
        }
    }
    const float c = std::cos(30 * 3.14159265f / 180), s = std::sin(30 * 3.14159265f / 180), pos = float(fx.pos);
    m.piece(pos * c + 130 - 16 + 16, 10 - pos * s - 32 + 32, 704, 328, 160, 104);
    m.piece(234 - pos * c + 16 + 16, pos * s + 208 + 32 + 32, 864, 328, 160, 104);
    m.piece(float(fx.slide) - 496 + 16, 114 + 32, 512, 432, 512, 72);
    m.piece(464 - float(fx.slide) + 16, 186 + 32, 512, 504, 512, 32);
}

// The enemies are down (btl_main 1000): the victory pose and tune, then the results
// btl_main 1000 / 0x3e8: ten frames first, the battle's music fading out over them unless the
// victory tune (var 59) is the battle's own (var 58); then 0x3e9 - victoryShown
void battleWon(App& app) {
    App::Battle& b = app.battle;
    b.phase = App::Battle::Won;
    b.phaseFrames = 0;
    b.wonTap = false;
    const auto v58 = app.game.vars.find(58), v59 = app.game.vars.find(59);
    const int battleTune = v58 != app.game.vars.end() ? v58->second : 999, tune = v59 != app.game.vars.end() ? v59->second : 10;
    if (battleTune != tune) playBgm(app, 1u << 26); // (__bgmgo(0x4000000): out over 10 frames)
}
void victoryShown(App& app) {
    App::Battle& b = app.battle;
    btlvoice::victory(app); // btl_main 0x3e9
    int& bank = app.game.walkExp[size_t(fieldRoute(app))]; // skill 174's walking EXP joins the battle's
    if (!b.settled) b.rules.expGained += bank, bank = 0;   // (before the Apples double it)
    settleBattle(app);
    { // var 59: the victory tune (10 for random encounters) - none when it's the battle's own (var 58)
        // or 998, the battle's music played on (btl_main 0x3e9)
        const auto v58 = app.game.vars.find(58), v59 = app.game.vars.find(59);
        const int battleTune = v58 != app.game.vars.end() ? v58->second : 999, tune = v59 != app.game.vars.end() ? v59->second : 10;
        if (battleTune != tune && tune != 998) playBgm(app, uint32_t(tune >= 0 && tune < 29 ? tune : 10));
    }
    for (int k = 0; k < 2; ++k)
        if (App::Battle::Actor* a = battleActor(app, k); a && b.rules.f[size_t(k)].alive()) {
            a->yaw = a->wantYaw = a->homeYaw;
            a->hidden = false;
            playSlot(app, *a, btlui::kSlotWin);
        }
    setBattleCamera(app, b.rules.pair ? 0x47 : b.rules.soloMember() + 0x45); // (btl_main 0x3e9)
    setBanner(app, sysText(app, 57007), 60);                                    // font_set_utf16(11, 6, 7, 60)
}

// btl_main 8000 -> 1000: a battle with a strong-growth monster (G+0xa4b), won in any formation but
// 2, shows the boss's end first - the camera on it (the formation's 0x35th, m105a's 0x34th), the
// fallen monsters' down motion (btlanm_boss_dead_anim), the pair out of sight. m102 .. m105a
// (0x3e3 .. 0x3e6): a quake and white, the boss gone under it, the pair back; any other (0x3de):
// its down motion played, then it fades (effect 0x1b, sound 0x5f) and 40 frames pass (999). Then
// the victory (battleWon: the Won phase is 1000, its 10 frames)
void battleVictory(App& app) {
    App::Battle& b = app.battle;
    bool boss = false;
    for (int k = 2; k < 5; ++k)
        if (const cr::Fighter& f = b.rules.f[size_t(k)]; f.present && f.def && f.def->strongGrowth) boss = true;
    App::Battle::Actor* m2 = battleActor(app, 2);
    if (!boss || b.formation == 2 || !m2) return battleWon(app);
    b.phase = App::Battle::BossDown;
    b.phaseFrames = 0;
    b.bossModel = m2->modelId;
    // (btl_main 8000, 5970-5974: +0xae0 = btlanm_get_max_frm(2, its model, 6) + 20 before the camera - its
    // 10000 turns over the fall)
    const LoadedModel& lm2 = b.models[m2->model];
    const int fall = battleAction(lm2, realSlot(lm2, btlui::kSlotDown));
    b.camHold = (fall >= 0 ? int(lm2.actions[size_t(fall)].duration) - 1 : 0) + 20;
    setBattleCamera(app, b.formation + (b.bossModel == 12 ? 0x34 : 0x35));
    for (App::Battle::Actor& a : b.actors) {
        if (a.fighter >= 2 && !b.rules.f[size_t(a.fighter)].alive() && a.slot != btlui::kSlotDown) playSlot(app, a, btlui::kSlotDown, -3);
        if (a.fighter >= 0 && a.fighter < 2 && b.rules.f[size_t(a.fighter)].alive()) a.hidden = true;
    }
    b.bossWait = int(actorDuration(app, *m2)) - 1 + 20; // (btlanm_get_max_frm(2, .., 6) + 20)
}

void stepBossDown(App& app) {
    App::Battle& b = app.battle;
    const int f = b.phaseFrames, m = b.bossModel;
    App::Battle::Actor* boss = battleActor(app, 2);
    if (m < 9 || m > 12) { // 0x3de, then 999's 40 frames and 1000's 10
        if (b.bossWait > 0 && --b.bossWait < 2) {
            b.bossWait = -40; // (999's +0xae0 40; 1000's 10 frames are the Won phase's own)
            if (boss) boss->fading = true, spawnEffect(app, 0x1b, 2);
            app.sound.play(0x5f);
        } else if (b.bossWait < 0 && ++b.bossWait == 0) battleWon(app);
        return;
    }
    // m102, m103, m104, m105a: the white's frames in and out, when the boss goes, the end
    static const int kWhite[4] = {60, 66, 104, 60}, kWhiteMs[4] = {1000, 700, 1000, 3500}, kGone[4] = {90, 90, 130, 135},
                     kBack[4] = {100, 100, 140, 140}, kEnd[4] = {130, 130, 170, 170};
    const int k = m - 9;
    if (f == 1) {
        app.sound.play(0xbe);
        if (m != 12) app.sound.play(0x62);
        startQuake(app, 3, 0, 100, 2, true);
    }
    if (f == kWhite[k]) setFlash(app, 0, 255, kWhiteMs[k]);
    if (f == kGone[k]) {
        if (boss) boss->hidden = true;
        endQuake(app, 3, 100, 0, 2);
    }
    if (f == kBack[k]) {
        app.sound.play(0xeb);
        setFlash(app, 255, 0, 2000);
        setBattleCamera(app, b.rules.pair ? 0x47 : b.rules.soloMember() + 0x45, -100); // (btl_main 6503: actor -100, one step of its turn)
        for (App::Battle::Actor& a : b.actors)
            if (a.fighter >= 0 && a.fighter < 2 && b.rules.f[size_t(a.fighter)].alive()) a.hidden = false;
    }
    if (f == kEnd[k] + 1) battleWon(app); // (999's frame - +0xae0 1 - then 1000, whose 10 frames are the Won phase's)
}

void endRound(App& app) {
    App::Battle& b = app.battle;
    if (b.rules.outcome() == 1) { // _btlact_escape's 99: btl_main 700 goes to 9000 at once (2.2.0 decomp 6191-6223 ->
        b.phase = App::Battle::Fled; // 6037) - no round's end (statuses, poison), no wait before the black
        b.phaseFrames = 0;
        settleBattle(app);
        b.exit = 1;
        return;
    }
    battletalk::rules(app); // (status_prc's boss rules as the round ends)
    b.rules.endRound();
    for (int k = 0; k < 2; ++k) // the guard pose ends with the round
        if (App::Battle::Actor* a = battleActor(app, k); a && a->slot == btlui::kSlotGuard) playSlot(app, *a, btlui::kSlotIdle);
    for (int i = 0; i < 5; ++i) b.shownHp[i] = b.rules.f[size_t(i)].hp, b.shownMp[i] = b.rules.f[size_t(i)].mp;
    switch (b.rules.outcome()) {
    case 2: // won: the victory pose and tune, then the results (btl_main 0x44b) - a story boss's
        // last word first (8000)
        if (battletalk::end(app)) b.phase = App::Battle::WinTalk, b.phaseFrames = 0;
        else battleVictory(app);
        break;
    case 3: { // lost (0x4b0): the battle's music out over 10 frames, then (0x4b1) the defeat tune and "... lost the battle"
        b.phase = App::Battle::Lost;
        b.phaseFrames = 0;
        b.lostTap = false;
        playBgm(app, 1u << 26); // (btl_main 0x4b0: __bgmgo(0x4000000); 0x4b1 plays 0x1a)
        break;
    }
    case 1:
        b.phase = App::Battle::Fled;
        b.phaseFrames = 0;
        settleBattle(app);
        break;
    default: // btl_main 800 -> 899: thirty frames on the formation's camera (btlcamera_set(formation - 2)), then
        // the commands (400) and chk_karaok_between
        // (0x320, decomp 6131-6154: the pair standing - not getting up, slot 7 - and formation 2's first two
        // monsters back to their idles, mdl_set_act_nextanm 0: a cast's loop ends here)
        for (int k = 0; k < (b.formation == 2 ? 4 : 2); ++k)
            if (App::Battle::Actor* a = battleActor(app, k); a && b.rules.f[size_t(k)].alive() && a->slot != btlui::kSlotRise && a->slot != btlui::kSlotIdle)
                playSlot(app, *a, btlui::kSlotIdle);
        b.phase = App::Battle::RoundEnd;
        b.phaseFrames = 0;
        b.member = 0;
        b.commandSaid[0] = b.commandSaid[1] = false; // +0xafc / +0xafd
        setBattleCamera(app, std::max(0, b.formation - 2));
        break;
    }
}

// How far a page slides: 320 in iOS 2.2.0 (__menub_mainPrc 0x17920: +0x38 by 0x50 to 0x140); Android
// 1.1.1's _menub_mainPrc takes 480 (a quarter a frame) on a Retina, iPad or 720 x 480 screen, 320 below -
// its iPhone screen is centred on a wider phone. The Android layout is a 720-tall phone's: 480, 120 a frame
int battleSlideDistance(const App& app) { return app.renderer.androidScreen() ? 480 : 320; }

// __menub_mainPrc's slides: the page shown follows the logic's - it goes out (a quarter a frame), is held
// out a while (Defend's 20 frames), and the next comes in (state 1's sound 0x22 for the first page); with
// none to show (a talk, the round, Options) it stays out. The left HUD (+0x3c, 11767-11775) goes 480 to
// the left, 120 a frame, while a talk runs (__menub_karaok_on) and comes back the same way after
void stepMenuView(App& app) {
    App::Battle& b = app.battle;
    App::Battle::MenuView& v = b.view;
    // (and as the results open: btl_main 0x44b's _menub_main_set_result, 2.2.0 decomp 13180-13195, sends the two
    // windows from x 0 to -480 over 4 frames)
    const bool hudOut = battletalk::on(app) || b.phase == App::Battle::Results;
    b.hudSlide = hudOut ? std::min(480, b.hudSlide + 120) : std::max(0, b.hudSlide - 120);
    const int dist = battleSlideDistance(app);
    const int want = b.phase == App::Battle::Command && !battletalk::on(app) && !app.optionsOpen ? b.page : -1;
    auto live = [&]() { // the page as the logic has it
        v.menu = b.menu, v.cursor = b.cursor, v.member = b.member, v.pick = b.pick, v.pickTop = b.pickTop;
        v.confirm = b.confirm, v.targetAction = b.targetAction, v.pair = b.rules.pair, v.picks = b.picks;
    };
    if ((b.viewState == 1 || b.viewState == 2) && v.page != want) b.viewState = 3;
    switch (b.viewState) {
    case 1: // coming in
        live();
        b.slide = std::max(0, b.slide - dist / 4);
        if (b.slide == 0) {
            b.viewState = 2;
            if (v.menu == App::Battle::Top) app.sound.play(0x22); // (__menub_mainPrc 1 -> 2: the top menu in place)
        }
        return;
    case 2: live(); return;
    case 3: // going out
        b.slide = std::min(dist, b.slide + dist / 4);
        if (b.slide < dist) return;
        b.viewState = b.slideHold > 0 ? 4 : 0;
        if (b.viewState == 4) return;
        break;
    case 4: // held out
        if (--b.slideHold > 0) return;
        b.viewState = 0;
        break;
    default:
        if (v.page == want || want < 0) { // nothing new: out
            v.page = -1;
            ++b.menuGone;
            return;
        }
        break;
    }
    b.slide = dist; // out: the next page (or none)
    b.slideHold = 0;
    b.menuGone = 0;
    v.page = want;
    if (want < 0) return;
    live();
    b.viewState = 1;
}

// Taps and keys are taken once the page is in (and the logic's page is the one shown)
bool battleMenuReady(const App& app) {
    const App::Battle& b = app.battle;
    return b.viewState == 2 && b.view.page == b.page;
}

// One 20 Hz battle frame: the actors' motions and moves, the round, the HUD's counters

// FUN_00031820 / FUN_000318f4 (29228-29330, from __menub_mainPut every frame): the party's shown HP / MP
// step toward the value by a fifth of the gap, or by 1 within 4
void rollPartyNumbers(App::Battle& b) {
    auto roll = [](int& shown, int want) {
        const int d = want - shown;
        if (d == 0) return;
        shown += std::abs(d) <= 4 ? (d > 0 ? 1 : -1) : d / 5;
    };
    for (int k = 0; k < 2; ++k) roll(b.rollHp[k], b.shownHp[k]), roll(b.rollMp[k], b.shownMp[k]);
}

void stepBattle(App& app) {
    rollPartyNumbers(app.battle);
    App::Battle& b = app.battle;
    if (SDL_getenv("CR_BATTLE_BANNERLOG")) { // the top banner's changes, by battle frame (the last frame's state)
        static std::string shown;
        static int shownPhase = -1;
        if (b.banner != shown || int(b.phase) != shownPhase)
            std::printf("battle frame %d: phase %d, banner '%s' (%d left), camera %d\n", b.frames, int(b.phase), b.banner.c_str(), b.bannerFrames, b.cam);
        shown = b.banner, shownPhase = int(b.phase);
    }
    b.bannerShown = b.bannerFrames > 0 ? b.banner : std::string(); // (what the last frame set shows from this one)
    ++b.frames;
    ++b.phaseFrames;
    stepEffects(app);
    stepSceneEffects(app);
    for (App::Battle::Actor& a : b.actors) {
        const float dur = actorDuration(app, a);
        const cr::BattleAnim an = app.battleData.anim(a.modelId, a.slot);
        // (a new slot: its first frame held through the cross-fade - the model's frame lags the change
        // by it, the move along its window doesn't: btlmove_prc counts on its own)
        if (!a.blend.step(a.action, a.time, 1.0f, b.models[a.model].actions)) a.time += 1.0f;
        if (a.then == -2) { // held on its last frame (a fallen monster)
            a.time = std::max(dur - 1, 0.0f);
        } else if (a.then == -3) { // played once, then held (mdl_set_act_motion_loop mode 2: a boss's end)
            a.time = std::min(a.time, std::max(dur - 1, 0.0f));
        } else if (a.then >= 0) {
            if (a.time >= std::max(dur, 1.0f)) {
                const int next = a.then;
                const bool back = next == btlui::kSlotReturn;
                playSlot(app, a, next, back ? btlui::kSlotIdle : -1);
                if (back) { // and walks back to its place meanwhile
                    const cr::BattleAnim ra = app.battleData.anim(a.modelId, btlui::kSlotReturn);
                    a.from = a.pos;
                    a.to = a.home;
                    a.moveFrom = ra.moveEnd > ra.moveStart ? ra.moveStart : 0;
                    a.moveTo = ra.moveEnd > ra.moveStart ? ra.moveEnd : std::max(1, int(actorDuration(app, a)));
                    // (_btlact_attack 0x406: the wanted yaw the heading out again, turned to over the way back)
                    if (a.approached) turnOver(a, a.approachYaw, float(a.moveTo - a.moveFrom));
                    a.turnHome = false;
                } else if (next == btlui::kSlotIdle) { // (the yaw is left: the attack's end turns it home)
                    a.pos = a.home;
                    a.moveFrom = a.moveTo = 0;
                }
            }
        } else if (a.loopTo > a.loopFrom && a.time >= float(a.loopTo))
            a.time = float(a.loopFrom) + (a.time - float(a.loopTo));
        else if (a.loopTo <= a.loopFrom && an.loopEnd > an.loopStart && a.time >= float(an.loopEnd))
            a.time = float(an.loopStart) + (a.time - float(an.loopEnd));
        else if (dur > 0 && a.time >= dur)
            a.time = std::fmod(a.time, dur);
        if (a.moveTo > a.moveFrom) {
            const float t = std::clamp((a.time + a.blend.lag() - float(a.moveFrom)) / float(a.moveTo - a.moveFrom), 0.0f, 1.0f);
            a.pos = a.from + (a.to - a.from) * t;
            // btlmove_prc (0x3544c): a setpos_angy move, its wait over, turns the wanted yaw home over the move
            if (a.turnHome && a.time + a.blend.lag() >= float(a.moveFrom)) {
                turnOver(a, a.homeYaw, float(a.moveTo - a.moveFrom));
                a.turnHome = false;
            }
        }
        if (a.fading) a.alpha = std::max(0.0f, a.alpha - 1.0f / 20.0f);
    }
    // The badge's gauge follows the rules' a step every other frame (__menub_mainPut 0x19728, decomp
    // 12797-12805: only while CRView::prc's frame count +0x3c6a8 is even), the pop shrinks
    if (b.gaugeShown != b.rules.breakGauge && (b.steps & 1) == 0) b.gaugeShown += b.gaugeShown < b.rules.breakGauge ? 1 : -1;
    ++b.steps;
    if (b.breakPop >= 1.0f) b.breakPop -= 4.0f / 7.0f;
    if (b.bannerFrames > 0) --b.bannerFrames;
    if (b.bannerFrames == 0 && !b.notices.empty()) { // menub_mainPut: the next queued message
        const cr::BattleNotice n = b.notices.front();
        b.notices.erase(b.notices.begin());
        std::string name; // var 9 the item (Steal Item), else var 0 the fighter
        if (n.fighter < 0) name = itemName(app, -n.fighter);
        else if (const App::Battle::Actor* u = battleActor(app, n.fighter)) name = u->name;
        setBanner(app, sysText(app, 57000 + n.entry, name), btlui::kMessageFrames);
    }
    for (App::Battle::Popup& p : b.popups) // (__dmg_prc: 16 off a frame, the slot freed below 0)
        if (p.on && (p.timer -= 16) < 0) p.on = false;

    battletalk::rules(app);
    if (app.game.flag(battletalk::kFlag)) { // a talk: the script runs (its fades with it), the battle waits
        battletalk::step(app);
        stepMenuView(app);
        turnActors(app);
        stepBattleCamera(app);
        stepQuake(app);
        stepFlash(app);
        return;
    }
    if (app.fadeT < app.fadeFrames) ++app.fadeT; // the screen's fade (stepEvents' while a talk runs)
    switch (b.phase) {
    case App::Battle::Intro: // btl_main 10 / 100 / 199 under the encounter's white, then 200, 300 and 400
        if (b.phaseFrames == kIntroWhite) {
            // 200 (2.2.0 btl_main 0xd34c, decomp 6372-6389): the fade in over 20 frames, the formation's
            // camera held still (actor -1), the 300 counter (+0xae0: 30, formation 2's 60), a strike's or
            // ambush's announcement - var 0 the lead's name (+0xaa), sys 0x36 / 0x37 for 60 frames
            app.fadeGrey = 1, app.fadeFrom = 1, app.fadeTo = 0, app.fadeFrames = 20, app.fadeT = 0;
            setBattleCamera(app, std::max(0, b.formation - 2), -1);
            if (b.start.strike == 1 || b.start.strike == 2) {
                const App::Battle::Actor* lead = battleActor(app, 0);
                setBanner(app, sysText(app, 57053 + b.start.strike, lead ? lead->name : std::string()), 60);
            }
        }
        // 300's first frame (6393-6405): the counter at 29 sets the formation's camera again with actor 0;
        // formation 2's at 59 camera 0x21b instead
        if (b.phaseFrames == kIntroWhite + 1) setBattleCamera(app, b.formation == 2 ? 0x21b : std::max(0, b.formation - 2), 0);
        if (b.introStep == 300 && b.phaseFrames >= kIntroWhite + (b.formation == 2 ? 60 : 30)) {
            // the counter below 1 (and the fade done): 400 next, _chk_karaok_frist - a story battle's talk
            // (flag 0xfff) or the first battle's tutorial holds 400 till it ends
            b.introStep = 400;
            battletalk::first(app);
        } else if (b.introStep == 400) {
            // 400 (6422-6459): ambushed (+0x35874 2) - 500, the enemies' round, no menu and no camera of its
            // own; else +0x2c0 |= 0x200 and 499
            if (b.start.strike == 2) {
                battleStartRound(app);
                b.roundFirst = true;
            } else
                b.introStep = 499;
        } else if (b.introStep == 499) {
            // __menub_mainPrc 0 the frame after: the top menu and its camera (formation + 0xc)
            b.phase = App::Battle::Command;
            b.phaseFrames = 0;
            battleMenu(app, App::Battle::Top);
        }
        break;
    case App::Battle::WinTalk: // btl_main 8000: the last word said, the victory
        battleVictory(app);
        break;
    case App::Battle::BossDown: stepBossDown(app); break;
    case App::Battle::Acting:
        if (b.breakFx.on) { // the battle waits for it; then the Break's tutorial (0xff2)
            if (stepBreakFx(app)) b.breakFx.on = false, battletalk::tutorial(app, 0xff2);
            break;
        }
        if (!b.turnOn) {
            // btl_main after an action's end (btlact_main's 1): 700 goes to 799 that frame, 799's checks run
            // the next (btlord_chk) and 600 starts the next action on the third (2.2.0 decomp 6190-6240) -
            // two frames more between one action and the next
            if (b.handover > 0) {
                --b.handover;
                break;
            }
            // btl_main 799: no next action while a message is up or waiting (the round's first comes from
            // 500 / 600 straight)
            if (!b.roundFirst && (!b.notices.empty() || (b.bannerFrames > 0 && b.bannerFrames <= btlui::kMessageFrames))) break;
            // btlord_chk in 799: a side lost the Break - its effect (0x5dc: init in 799, its frames from the next)
            if (b.breakDue) {
                b.breakDue = false;
                startBreakFx(app);
                if (stepBreakFx(app)) b.breakFx.on = false, battletalk::tutorial(app, 0xff2);
                break;
            }
            if (b.roundWait && (b.viewState != 0 || b.menuGone < 2)) break; // the menu goes first
            b.roundWait = false;
            b.roundFirst = false;
            const bool pairWas = b.rules.pair, enemyPairWas = b.rules.enemyPair;
            if (b.rules.nextTurn(app.battleActions, b.turn)) {
                // (the calc ran ahead of the scene: what its chk_dead ended stays until the hit - showHits)
                b.pairHeld = true, b.pairAfterHit = b.rules.pair, b.enemyPairAfterHit = b.rules.enemyPair;
                b.rules.pair = pairWas, b.rules.enemyPair = enemyPairWas;
                b.turnOn = true;
                startTurn(app);
            } else if (std::none_of(std::begin(b.popups), std::end(b.popups), [](const App::Battle::Popup& p) { return p.on; }))
                endRound(app);
            break;
        }
        ++b.turnFrame;
        if (b.stageAction) {
            stepStagedCameras(app);
            if (!b.hitsShown) stepStaged(app);
        } else if (b.turn.kind == cr::kCmdAttack && b.turn.actors[0] >= 2) { // btlact_attack 0x76c: m101 .. m103's blows
            const App::Battle::Actor* u = battleActor(app, b.turn.actors[0]);
            const int m = u ? u->modelId : -1, f = b.turnFrame;
            if ((m == 8 && f == 22) || (m == 9 && f == 7) || (m == 10 && f == 30)) app.sound.play(m == 8 ? 0x7f : m == 9 ? 0x80 : 0xcc);
            // (2.2.0 _btlact_attack 0x76c, 11476-11505: m103's frame 0x12 sets effect 0x3f at its node 1204
            // - eff3d_set_with_pos(.., 50%), its own angles (+0x56f 0) - and keeps it on that node while it lasts)
            if (u && m == 10 && f == 18) {
                const size_t n = b.effects.size();
                startEffect(app, b.effects, 0x3f, actorPoint(app, *u, 1204), false, 0.5f, 0.0f);
                keepEffectAngles(app, n);
                b.nodeFx = n < b.effects.size() ? int(n) : -1;
            } else if (u && m == 10 && f > 18 && b.nodeFx >= 0 && b.nodeFx < int(b.effects.size()) && b.effects[size_t(b.nodeFx)].id == 0x3f)
                b.effects[size_t(b.nodeFx)].pos = actorPoint(app, *u, 1204);
        } else if (b.turn.kind == cr::kCmdGene && !b.hitsShown) { // btlact_gene 1000: the bosses' casting sounds
            const App::Battle::Actor* u = battleActor(app, b.turn.actors[0]);
            const int m = u ? u->modelId : -1, f = b.turnFrame; // (m102 .. m105a)
            if ((m == 9 && f == 18) || (m == 10 && f == 30) || (m == 11 && f == (b.turn.action == 433 ? 36 : 44)) || (m == 12 && f == 28))
                app.sound.play(m == 9 ? 0xbd : m == 10 ? 0xdd : m == 11 ? 0xd4 : 0xf1);
        }
        // _btlact_attack 1000 (11318-11338): a member's blow whose init drew variant 2 cuts, as the approach
        // ends (count == que +0x10), to the target's fourth camera - slot x 4 + 0x7b / 0xe3 (the pair's 0x14b)
        if (!b.stageAction && b.turn.kind == cr::kCmdAttack && b.turn.actors[0] >= 0 && b.turn.actors[0] < 2 && b.attackVariant == 2 &&
            b.attackCam2At > 0 && b.turnFrame == b.attackCam2At && !b.turn.hits.empty()) {
            const int slot = enemyCameraSlot(app, firstTarget(b.turn));
            setBattleCamera(app, slot * 4 + (b.rules.pair ? 0x14b : b.turn.actors[0] == 0 ? 0x7b : 0xe3), 0);
        }
        if (!b.hitsShown && b.turnFrame >= b.hitFrame) {
            if (!b.turn.barriers.empty() && b.bounceAt < 0) {
                // btlact_gene / btlact_item 0x1004: the barriers light up, the users stop; 40 frames
                // on (0x100e) the barrier's side sends it back, and it lands on them
                for (int k : b.turn.barriers) spawnEffect(app, effects::kBuff, k);
                app.sound.play(0x78);
                for (int k : b.turn.actors)
                    if (App::Battle::Actor* u = k >= 0 ? battleActor(app, k) : nullptr) playSlot(app, *u, btlui::kSlotIdle, btlui::kSlotIdle);
                b.bounceAt = b.turnFrame + 40;
                b.hitFrame = b.bounceAt + 1; // (0x1004 counts 40 and swaps the targets; 0x100e the next frame sets the camera
                                             // and calculates - no glow, no wait: 2.2.0 _btlact_gene decomp 38655-38778)
                hitCamera(app, b.turn, b.turn.barriers[0], false); // (state 1000's camera, on the target, before the barrier check)
            } else {
                if ((b.turn.kind == cr::kCmdGene || b.turn.kind == cr::kCmdItem) && !b.stageAction && !b.turn.cancelled) {
                    // state 1000's camera at the hit (0x100e's on the users, now the targets, after a barrier)
                    const std::vector<int> ts = turnTargets(b.turn);
                    hitCamera(app, b.turn, b.turn.barriers.empty() ? (ts.empty() ? -1 : ts[0]) : b.turn.actors[0], !b.turn.barriers.empty());
                }
                showHits(app);
                if (b.stageAction) stagedHit(app);
            }
        }
        // _btlact_gene / _btlact_item 2000's first frame (2.2.0 decomp 38480-38497): each user (que +0x98, the
        // users' copy), alive and not guarding (_btlact_autoguard), gets _mdl_set_act_nextanm(0) - its cast
        // loop let go: the clip plays on to its end, then the idle
        if (b.hitsShown && (b.turn.kind == cr::kCmdGene || b.turn.kind == cr::kCmdItem) && !b.stageAction && !b.turn.cancelled &&
            b.turnFrame == b.hitFrame + 1)
            for (int k = 0; k < 2; ++k) {
                const int who = b.turn.actors[k];
                App::Battle::Actor* u = who >= 0 ? battleActor(app, who) : nullptr;
                if (!u || u->then >= 0 || u->slot == btlui::kSlotIdle) continue; // (a mover's own way)
                const cr::Fighter& fw = b.rules.f[size_t(std::clamp(who, 0, 4))];
                if (fw.guarding || (b.turn.kind == cr::kCmdGene && !fw.alive())) continue; // (the gene's _IsDead; the item's none)
                u->then = btlui::kSlotIdle, u->castRest = true;
                u->loopFrom = u->loopTo = -1;
            }
        if (b.hitsShown) {
            bool busy = false;
            for (int k = 0; k < 2; ++k)
                if (const App::Battle::Actor* u = b.turn.actors[k] >= 0 ? battleActor(app, b.turn.actors[k]) : nullptr)
                    busy |= u->then >= 0 && u->slot != btlui::kSlotHit && !u->castRest;
            // (the tail after the action: an attack's ten frames once its actors are back (btlact_attack 1999,
            // count 10), a gene's or item's forty after its hit (btlact_gene / item 2000, count 0x28), a
            // cancelled command's sixty on its user's camera (btlact_none_init: count 0x3c))
            if (!busy && b.turnFrame >= b.hitFrame && b.idleSince < 0) b.idleSince = b.turnFrame;
            const bool over = b.turn.kind == cr::kCmdEscape ? b.turnFrame >= (b.turn.escaped ? 30 + 40 : 40)
                              : b.stageAction ? b.turnFrame >= b.hitFrame + b.stageWait // the staged ones' wait
                              : b.turn.cancelled ? b.turnFrame >= 60
                              : b.turn.kind == cr::kCmdAttack ? (b.idleSince >= 0 && b.turnFrame >= b.idleSince + 10) || b.turnFrame > 400
                                                              : (!busy && b.turnFrame >= b.hitFrame + 40) || b.turnFrame > 400;
            if (over) {
                if (b.stageAction == 433) // 9999: mdl_set_act_motion(0), it stands
                    if (App::Battle::Actor* u = battleActor(app, b.turn.actors[0])) playSlot(app, *u, btlui::kSlotIdle);
                b.stageAction = 0;
                b.turnOn = false;
                releasePair(b); // (a turn without a hit shown)
                b.handover = b.turn.kind == cr::kCmdEscape && b.turn.escaped ? 0 : 2; // (an escape's 99 goes to 9000 from 700)
                // btlord_chk: a side lost the Break - its effect first (btl_main 0x5dc), from 799
                b.breakDue = b.rules.breakChanging();
                // btl_main 700: the action's tutorials - an element taken, a status, a gene learned
                if (b.rules.elementTaken) battletalk::tutorial(app, 0xff3);
                else if (b.rules.statusTaken) battletalk::tutorial(app, 0xff4);
                else if (b.rules.learnedNew) battletalk::tutorial(app, 0xff5);
                // (_btlact_attack's end, 2.2.0 _btlact_attack 0x164a8: each attacker's wanted yaw home, turned over 3
                // frames - a size-3 monster's 7; the other actions don't turn their actors)
                for (int k = 0; k < 2; ++k)
                    if (App::Battle::Actor* u = b.turn.actors[k] >= 0 ? battleActor(app, b.turn.actors[k]) : nullptr) {
                        u->pos = u->home;
                        u->moveFrom = u->moveTo = 0;
                        u->turnHome = false, u->approached = false;
                        if (u->slot != btlui::kSlotGuard)
                            turnOver(*u, u->homeYaw, b.turn.kind == cr::kCmdAttack && b.turn.actors[k] >= 2 && monsterSize(app, b.turn.actors[k]) == 3 ? 7.0f : 3.0f);
                    }
            }
        }
        break;
    case App::Battle::RoundEnd: // (899: thirty frames, then 400 - the menu - and _chk_karaok_between)
        if (b.phaseFrames >= 30) {
            b.phase = App::Battle::Command;
            b.phaseFrames = 0;
            battleMenu(app, App::Battle::Top);
            battletalk::between(app); // btl_main 899 -> 400
        }
        break;
    case App::Battle::Won: // 0x3e8's ten frames, then 0x3e9 (the poses, tune and banner) and 1099's 140 or a tap
        if (b.phaseFrames == 10) victoryShown(app);
        if (b.phaseFrames < 10) { b.wonTap = false; break; } // (a tap in the fade counts for nothing)
        if (b.phaseFrames >= 150 || b.wonTap) {
            b.wonTap = false;
            b.phase = App::Battle::Results;
            b.phaseFrames = 0;
            b.result.stage = 0, b.result.frames = 0, b.result.message = 57007;
        }
        break;
    case App::Battle::Results: stepResults(app); break;
    case App::Battle::Lost: // 0x4b0 ten frames, 0x4b1 the defeat tune, 0x513 a tap, then (9000) btl_lose: a story
        // battle starts over from the title, any other goes back to the pair's Ark (drawFrame does either,
        // between frames). An event battle's: gameover.mct read at 0x4b0, fading in over 20 frames from
        // 0x4b1, the tap taken after 40; any other's tap at once (the port goes on by itself after 120)
        if (b.phaseFrames == 10) { // 0x4b1: the tune, "... lost the battle" for 60 frames (font_set_utf16(0xb, 6, 0x10, 0x3c))
            // 0x4b0 -> 0x4b1 (2.2.0 decomp 6044-6074): the defeat voice only for a battle a script started (+0xaee,
            // as for a pair: +0xae8 set round it); then the camera 0x48 and the monsters still standing out of sight (+0x55c)
            if (b.eventBattle) btlvoice::playSide(app, 99, 0x1f, 0x20, 0x21);
            setBattleCamera(app, 0x48);
            for (App::Battle::Actor& a : b.actors)
                if (a.fighter >= 2 && b.rules.f[size_t(a.fighter)].alive()) a.hidden = true;
            playBgm(app, 0x1a);
            const App::Battle::Actor* lead = battleActor(app, 0);
            setBanner(app, sysText(app, 57016, lead ? lead->name : std::string()), 60);
            if (b.eventBattle && !b.gameOver.id) {
                cr::Image img;
                if (loadImage(app, "gameover.mct", img)) b.gameOver = app.renderer.upload(img);
            }
        }
        // 0x513: a tap (pad_chk_trg_up) once +0xa46 reads 40 - at once for any battle but an event's, whose count
        // starts at 0 with the picture; nothing else ends it
        if (b.phaseFrames >= (b.eventBattle ? 50 : 10) && b.lostTap) b.exit = b.eventBattle ? 2 : 3;
        b.lostTap = false;
        break;
    case App::Battle::Fled:
        if (b.phaseFrames >= 30) b.exit = 1;
        break;
    default: break;
    }
    // btl_main 9000: every way out (the results done, a flight, a defeat) first masks the screen to
    // black over 10 frames (_SetMask(0): 500 ms) with the music fading out, and 0x238b leaves 12 frames
    // later - the field then comes back out of the black (FieldScene::onLoading's SetMask2(1), endBattle)
    if (b.exit && !b.ending) {
        b.pendingExit = b.exit, b.exit = 0;
        b.ending = true, b.endFrames = 0;
        app.fadeGrey = 0, app.fadeFrom = screenFadeAlpha(app), app.fadeTo = 1, app.fadeFrames = 10, app.fadeT = 0;
        playBgm(app, 1u << 26);
    } else if (b.ending && ++b.endFrames >= 12) {
        b.exit = b.pendingExit;
        b.ending = false, b.endFrames = b.pendingExit = 0;
    }
    // the pages after the frame's logic: a page set this frame starts at its distance with its camera's cut
    // (video 02 1634.70-1634.90 s: the plates partly in 3 frames after the cut, settled at 4)
    stepMenuView(app);
    turnActors(app); // (btl_prc's mdl_put)
    stepBattleCamera(app); // (btl_main's end: btlcamera_prc; btl_prc: the eye, the easing)
    stepQuake(app); // gameapp_prc: UpdateQuake and PutMask after the battle's frame
    stepFlash(app);
}

// _btl_shadow (2.2.0 0xbb80, after mdl_put in btl_prc) with _mdl_shadow (0x138ac): a 32-triangle black
// ellipse fan under each fighter standing (_IsDead clear), into tex2d slot 0x4e - centred on the x / z of
// the model's node 1102 (btlcamera_getpos2) at its y + 0.1, radii its row's +0x42 (x) and +0x44 (z)
// turned by its shown yaw (+0x554), alpha 0x50 = 80% of 255 for all (Omega fades a monster's with it);
// a monster's none when +0x42 is 0, none while a model is hidden (+0x55c clear). The pair's row is the
// character's (btl_prty_init: +0x24 = rows + c x 0x7f8, monster row c x 10 + 1)
void addBattleShadow(App& app, const App::Battle::Actor& a) {
    App::Battle& b = app.battle;
    if (a.fighter < 0 || a.fighter > 4 || a.hidden || !b.rules.f[size_t(a.fighter)].alive()) return;
    const cr::Fighter& f = b.rules.f[size_t(a.fighter)];
    const cr::MonsterDef* row = a.fighter < 2 ? app.battleData.monster(f.character * 10 + 1) : f.def;
    if (!row) return;
    const int rx = int16_t(row->raw[0x42] | row->raw[0x43] << 8), rz = int16_t(row->raw[0x44] | row->raw[0x45] << 8);
    if (a.fighter >= 2 && rx == 0) return;
    App::Battle::Shadow sh;
    sh.fighter = a.fighter;
    sh.alpha = float(80 * 255 / 100) / 255.0f; // (|0x50| x 255 / 100: 204)
    const cr::Vec3 c = actorPoint(app, a, 1102);
    sh.at = {c.x, a.pos.y + 0.1f, c.z};
    sh.rx = float(rx), sh.rz = float(rz);
    // the fan's triangles: (centre, point i, point i + 1), i / 31 turns round, the last back to 0
    const float cy = std::cos(a.yaw), sy = std::sin(a.yaw);
    auto point = [&](int i) {
        const float t = float(i == 32 ? 0 : i) / 31.0f * 2.0f * 3.14159265f;
        const float x = sh.rx * std::sin(t), z = sh.rz * std::cos(t);
        return cr::Vec3{sh.at.x + x * cy + z * sy, sh.at.y, sh.at.z + z * cy - sy * x};
    };
    for (int i = 0; i < 32; ++i) {
        const cr::Vec3 p0 = point(i), p1 = point(i + 1);
        b.shadowTris.insert(b.shadowTris.end(), {sh.at.x, sh.at.y, sh.at.z, p0.x, p0.y, p0.z, p1.x, p1.y, p1.z});
    }
    b.shadows.push_back(sh);
}

void drawBattleShadows(App& app, const cr::Mat4& vp) {
    App::Battle& b = app.battle;
    constexpr size_t kVerts = 5 * 32 * 3;
    if (b.shadows.empty()) return;
    if (!app.battleShadows.vao) {
        cr::Mesh mesh;
        mesh.positions.assign(kVerts * 3, 0.0f);
        mesh.uvs.assign(kVerts * 2, 0.0f);
        mesh.colors.assign(kVerts * 4, 0);
        for (uint32_t i = 0; i < kVerts; ++i) mesh.indices.push_back(i);
        app.battleShadows = app.renderer.upload(mesh);
    }
    std::vector<float> pos = b.shadowTris, uv(kVerts * 2, 0.0f);
    pos.resize(kVerts * 3, 0.0f);
    std::vector<uint8_t> col(kVerts * 4, 0);
    // (mdl_shadow's tex2d_add_tri colours: the centre's alpha << 24, the rim's 0 - 0x13a90..0x13afc)
    for (size_t v = 0; v < b.shadows.size() * 32 * 3 && v < kVerts; v += 3) col[v * 4 + 3] = uint8_t(std::lround(b.shadows[v / 96].alpha * 255.0f));
    app.renderer.updatePositions(app.battleShadows, pos, uv, col);
    cr::Material m; // untextured, double-sided, depth tested, not written
    m.culling = 2; // (none)
    m.properties = 8;
    m.blendSrc = 4, m.blendDst = 5;
    app.renderer.drawMesh(app.battleShadows, nullptr, vp, m);
}

// btl_prc: the whole screen through the formation's camera (btl_main: camera formation - 2;
// perspective 0.0792 turns = 28.5 degrees vertical, 4:3, near 9, far 2000 - 2.2.0's btl_prc 0xf7f8
// passes 0x41100000 / 0x44fa0000: Omega's 1 / 3500 are its own)
void drawBattle(App& app, float dt) {
    App::Battle& b = app.battle;
    auto& r = app.renderer;
    const float W = r.screenW(), H = cr::Renderer::kLogicalH; // 3D across the whole screen
    auto record = [&](int side) { // (the step's start or end: Battle::Between)
        b.between.actors[side] = b.actors;
        b.between.eye[side] = {488, 12, 555}, b.between.target[side] = {500, 4, 498};
        battleCameraView(app, b.between.eye[side], b.between.target[side], &b.between.roll[side]);
        b.between.cam[side] = b.cam;
        b.between.valid = side == 1;
    };
    for (b.clock += dt; b.clock >= 1.0f / kFramesPerSecond && b.active; b.clock -= 1.0f / kFramesPerSecond) {
        record(0);
        stepBattle(app);
        record(1);
    }
    b.between.alpha = std::clamp(b.clock * kFramesPerSecond, 0.0f, 1.0f);
    if (!b.active) return;
    const App::Battle::Between& bt = b.between;
    const float bk = bt.valid ? bt.alpha : 1.0f;
    r.fillRect(0, 0, W, H, 0, 0, 0, 1);
    drawSpaceBackdrop(app, true); // ScnEffectProc runs in battles too: the Space backdrop at the back
    r.setOrigin(0);
    cr::Vec3 eye{488, 12, 555}, target{500, 4, 498};
    float roll = 0;
    battleCameraView(app, eye, target, &roll);
    // (between steps: where the camera is by then, unless the step cut to another)
    const bool camBetween = bt.valid && bt.cam[0] == bt.cam[1] && bt.cam[1] == b.cam && same(eye, bt.eye[1]) && same(target, bt.target[1]);
    if (camBetween) {
        eye = lerp(bt.eye[0], bt.eye[1], bk);
        target = lerp(bt.target[0], bt.target[1], bk);
        roll = bt.roll[0] + (bt.roll[1] - bt.roll[0]) * bk;
    }
    if (SDL_getenv("CR_BETWEEN_DEBUG"))
        std::printf("battle frame: valid %d alpha %.2f camera %s (cam %d %d now %d)\n", int(bt.valid), bt.alpha, camBetween ? "between" : "as is",
                    bt.cam[0], bt.cam[1], b.cam);
    cr::Mat4 proj = cr::Mat4::perspective(28.5f * 3.14159265f / 180.0f, W / H, 9.0f, 2000.0f);
    // btl_prc: the quake shifts the frustum by its offset in the 1024 x 768 screen's pixels
    proj.m[8] -= 2.0f * b.quake.off[0] / W;
    proj.m[9] -= 2.0f * b.quake.off[1] / H;
    // (btl_prc sets flag 0x100: _gluLookAt turns the camera's frame +0xf8 degrees about its own z -
    // the world the other way in the view)
    cr::Mat4 view = cr::Mat4::lookAt(eye, target, {0, 1, 0});
    if (roll != 0) {
        const float ang = -roll * 3.14159265f / 180.0f;
        cr::Mat4 rz;
        rz.m[0] = std::cos(ang), rz.m[1] = std::sin(ang), rz.m[4] = -std::sin(ang), rz.m[5] = std::cos(ang);
        view = rz * view;
    }
    const cr::Mat4 vp = proj * view;
    b.vp = vp;
    r.setCamera(9.0f, 2000.0f, 28.5f * 3.14159265f / 180.0f);
    r.begin3DRect(0, 0, W, H);
    drawLoaded(app, b.stage, vp);
    b.shadows.clear(), b.shadowTris.clear();
    for (size_t ai = 0; ai < b.actors.size(); ++ai) {
        // the actor as a frame between steps shows it: moved, turned, faded and its animation
        // played on by then (an animation that looped, changed or jumped is shown as it is)
        App::Battle::Actor a = b.actors[ai];
        if (bt.valid && ai < bt.actors[0].size() && ai < bt.actors[1].size()) {
            const App::Battle::Actor& p0 = bt.actors[0][ai], &p1 = bt.actors[1][ai];
            if (p0.model == a.model && p1.model == a.model && same(a.pos, p1.pos) && a.yaw == p1.yaw && a.time == p1.time &&
                a.action == p1.action) {
                const cr::Vec3 d = p1.pos - p0.pos;
                if (cr::dot(d, d) < 40.0f * 40.0f) a.pos = lerp(p0.pos, p1.pos, bk), a.yaw = lerpAngle(p0.yaw, p1.yaw, bk);
                if (a.alpha == p1.alpha) a.alpha = p0.alpha + (p1.alpha - p0.alpha) * bk;
                if (p0.action == p1.action && p1.time >= p0.time && p1.time - p0.time <= 1.0f) a.time = p0.time + (p1.time - p0.time) * bk;
            }
        }
        if (a.alpha <= 0.001f || a.hidden) continue;
        LoadedModel& lm = b.models[a.model];
        const cr::Model& m = lm.cpu;
        if (!m.skeleton.nodes.empty() && lm.meshes.size() == m.meshes.size()) { // skin this instance
            const cr::Action* act = a.action >= 0 && a.action < int(lm.actions.size()) ? &lm.actions[size_t(a.action)] : nullptr;
            poseWorld(m.skeleton, lm.actions, act, a.time, a.blend, lm.world);
            for (size_t i = 0; i < m.meshes.size(); ++i) {
                cr::skinMesh(m, m.meshes[i], lm.world, lm.skinned);
                const bool spins = spinsTexture(m, i);
                if (spins) lm.uvScratch = m.meshes[i].uvs, spinUvs(lm.uvScratch, app.texSpin);
                r.updatePositions(lm.meshes[i], lm.skinned, spins ? lm.uvScratch : m.meshes[i].uvs, m.meshes[i].colors);
            }
        }
        modelDrawn(app);
        cr::Mat4 world;
        const float c = std::cos(a.yaw), sn = std::sin(a.yaw);
        world.m[0] = c * a.scale; world.m[2] = -sn * a.scale; world.m[5] = a.scale;
        world.m[8] = sn * a.scale; world.m[10] = c * a.scale;
        world.m[12] = a.pos.x; world.m[13] = a.pos.y; world.m[14] = a.pos.z;
        drawLoaded(app, lm, vp * world, nullptr, a.alpha);
    }
    for (const App::Battle::Actor& a : b.actors) addBattleShadow(app, a); // (as the step left them)
    drawBattleShadows(app, vp); // (_btl_shadow after mdl_put)
    drawEffects(app, vp, eye, target);
    r.end3D();
    drawSceneEffects(app, true); // the field's weather goes on over the battle
    r.setOrigin(0);
    if (b.flash.on && b.flash.alpha > 0) r.fillRect(0, 0, W, H, 1, 1, 1, b.flash.alpha / 255.0f); // PutMask
}

// ---- The HUD (menub_mainPut, __dmg_prc, MbResultPut) ----

// __dmg_prc's label (51790): the piece type - 1 for types 1..3 (Blowout! Critical! Weak!), the type
// itself from 4 - 5 Guard!, 6 Miss!, 7 Block! (the fended off is 6, the neutralized 7)
int hitLabel(int type) { return type < 4 ? type - 1 : type; }

// __dmg_prc's place for slot `slot` (x, y in the 480 x 320 screen's units; `wide` the widescreen margin
// beyond the 1024 layout, which the screen's clamps move out by): y 72..272, x 64 and over; slots 1 and 3
// 48 lower (28 for a heal) while slot 0 is up; x at most 460 - 20 a digit past the first; y kept as put
// (+0x99c); slot 2's heal within 20 of slot 0's kept y drawn 30 higher
void placePopup(App::Battle& b, int slot, float& x, float& y, float wide) {
    App::Battle::Popup& p = b.popups[slot];
    y = std::clamp(y, 72.0f, 272.0f);
    x = std::max(x, 64.0f - wide);
    if ((slot == 1 || slot == 3) && b.popups[0].on) y += p.amount < 0 ? 28 : 48;
    int right = 460;
    for (int v = std::abs(p.amount); v > 9; v /= 10) right -= 20;
    x = std::min(x, float(right) + wide);
    p.y = y;
    if (slot == 2 && p.amount < 0 && y - 20 <= b.popups[0].y && b.popups[0].y <= y + 20) y -= 30;
}

// cmd_number: `digits` places 10 apart, right-aligned; the HP's red under a quarter, the
// level's orange style (width 3)
void drawBattleNumber(cr::MenuPainter& m, int value, float x, float y, int digits, bool low, float a, bool level = false) {
    const std::string t = std::to_string(std::max(0, value));
    const int n = int(t.size());
    for (int k = 0; k < n; ++k) {
        const int d = t[size_t(k)] - '0';
        const float u = level ? 588 + 14.0f * d : 14.0f * d + (low ? 140 : 0);
        m.piece(x + 10.0f * float(digits - n + k), y, u, 0, 14, 14, a);
    }
}

// The command pages' cursor plate lit: for keys and pads; not after a touch or click. 2.2.0 is
// touch-only: __menub_mainPut (0x19728) lights a plate while a finger is in its box (pad_chk_box0)
// and __menub_mainPrc (0x17920) state 2 sets +0x92 to -1 every frame - no cursor. The Vita's
// __menub_mainPut (0x81043646) keeps one (DAT_81120298 the top page's, DAT_8112029c[member] the
// commands'; a touch moves it): the Vita copy as the game keeps it lit after a tap too. After a tap
// the plate just chosen shows lit as its page slides out (as Omega's port)
bool battleCursorLit(const App& app) { return !app.pointerLast || vitaLayout(app) || app.battle.viewState == 3; }

// cmd_bottom: a battle button (`x` and `y` its corner): base, face (lit when selected), icon, word
void drawBattleButton(cr::MenuPainter& m, float x, float y, int icon, int word, bool selected, bool dimmed, float a) {
    const float c = dimmed ? 0.25f : 1.0f;
    m.piece(x, y, 0, 184, 24, 40, a, c, c, c);
    m.pieceTo(x + 24, y, 84, 40, 28, 184, 4, 40, a, c, c, c);
    m.piece(x + 108, y, 28, 184, 24, 40, a, c, c, c);
    const float face = selected && !dimmed ? 96 : 52;
    m.piece(x + 28, y, face, 184, 24, 40, a, c, c, c);
    m.pieceTo(x + 52, y, 68, 40, face + 24, 184, 4, 40, a, c, c, c);
    m.piece(x + 120, y, face + 28, 184, 16, 40, a, c, c, c);
    m.piece(x + 8, y + 4, float(icon % 16 * 32), float(icon / 16 * 32 + 376), 32, 32, a, c, c, c);
    m.piece(x + 40, y + 8, float(word % 5 * 88), float(word / 5 * 24 + 512), 88, 24, a, c, c, c);
}

// The red -> orange HP bar of the plates and rows (FUN_0001b840 / FUN_0001b6c4, 13472-13560: tex2d_add4
// on slot 45), `w` long when full: a parallelogram 3 high leaning right by 3 - the top (x + 3, y)..(x + 3 +
// fill), the bottom (x, y + 3)..(x + fill) - fill = w x value / most (at least 1, none at 0), its right
// end coloured that far to the full colour; under it the black one (value 1 of 1) 1 right and 1 down
void drawBattleHpBar(cr::MenuPainter& m, float x, float y, float w, int value, int most, bool mp, float a) {
    const float black[4] = {0, 0, 0, a};
    m.slant(x + 1, y + 1, w, 3, 3, black, black);
    if (value <= 0 || most <= 0) return;
    const float f = std::clamp(float(value) / float(most), 0.0f, 1.0f);
    const float fill = float(std::max(1, int(w) * std::min(value, most) / most));
    const float left[4] = {mp ? 0x07 / 255.f : 0xdd / 255.f, mp ? 0xaf / 255.f : 0x02 / 255.f, mp ? 0x73 / 255.f : 0, a};
    const float right[4] = {left[0] + (1 - left[0]) * f, left[1] + (0xc3 / 255.f - left[1]) * f,
                            left[2] + (0x73 / 255.f - left[2]) * f, a};
    m.slant(x, y, fill, 3, 3, left, right);
}

// menub_param_put: the element, then each status on, 20 apart
void drawBattleStatus(cr::MenuPainter& m, const cr::Fighter& f, float x, float y, float a) {
    m.piece(x, y, float(f.element * 20), 924, 20, 20, a);
    float at = x + 20;
    for (int k = 0; k < cr::kStatusCount; ++k)
        if (f.status[k] > 0 && k != cr::kStatusUnused92) {
            m.piece(at, y, float(140 + 20 * k), 924, 20, 20, a);
            at += 20;
        }
}

// cmd_bottom_teki: a target's plate at (x, y): its level, name and HP bar
void drawBattlePlate(App& app, cr::MenuPainter& m, float x, float y, int fighter, bool selected, float a) {
    const App::Battle& b = app.battle;
    const cr::Fighter& f = b.rules.f[size_t(fighter)];
    m.piece(x, y, 0, 184, 24, 40, a);
    m.pieceTo(x + 24, y, 200, 40, 28, 184, 4, 40, a);
    m.piece(x + 224, y, 28, 184, 24, 40, a);
    const float face = selected ? 96 : 52;
    m.piece(x + 36, y, face, 184, 24, 40, a);
    m.pieceTo(x + 60, y, 180, 40, face + 24, 184, 4, 40, a);
    m.piece(x + 240, y, face + 28, 184, 16, 40, a);
    m.piece(x + 6, y + 25, 116, 14, 38, 10, a); // "Lv"
    drawBattleNumber(m, f.level, x + 6, y + 12, 3, false, a, true);
    drawBattleHpBar(m, x + 96, y + 25, 96, b.shownHp[fighter], f.hpMax, false, a); // (_cmd_bottom_teki 13718-13720: 0x60 wide)
    std::string name;
    for (const App::Battle::Actor& ac : b.actors)
        if (ac.fighter == fighter) name = ac.name;
    m.textCentred(name, x + 146 - m.textWidth(name, 13) / 2, y + 15, selected, a, 13);
    drawBattleStatus(m, f, x + 56, y - 8, a);
}

// A member's row: portrait, HP and MP numbers and bars, statuses (actor 1 at the bottom, actor
// 0 40 to the right and 24 up)
void drawBattleRow(App& app, cr::MenuPainter& m, int k, bool dimmed, float a) {
    const App::Battle& b = app.battle;
    const cr::Fighter& f = b.rules.f[size_t(k)];
    if (!f.present) return;
    const bool phone = app.renderer.phoneLayout(); // 2.2.0: (+16, -32), into the iPhone's corner
    // (Android 1.1.1's _menub_mainPut: - ex, + ey - the phone's bottom left)
    const float ex = androidEdgeX(app), ey = androidEdgeY(app);
    const float dx = (k == 0 ? 40 : 0) + (phone ? 16 - ex : 0), dy = (k == 0 ? -24 : 0) + (phone ? -32 + ey : 0);
    const float c = dimmed ? 0.25f : 1.0f;
    m.piece((k == 0 ? 52 : 0) + (phone ? 16 - ex : 0), 328 + dy, float(f.character * 48), 440, 48, 48, a, c, c, c);
    m.piece(2 + dx, 370 + dy, 0, 14, 22, 10, a); // "HP"
    m.piece(86 + dx, 370 + dy, 22, 14, 22, 10, a); // "MP"
    // the numbers and bars as they roll (+0x60 / +0x68); cmd_number(hp, max, 5) red at a quarter or less,
    // cmd_number(mp, mp, 4) red at 0
    drawBattleNumber(m, b.rollHp[k], 24 + dx, 364 + dy, 5, b.rollHp[k] <= f.hpMax / 4, a);
    drawBattleNumber(m, b.rollMp[k], 108 + dx, 364 + dy, 4, b.rollMp[k] <= 0, a);
    drawBattleHpBar(m, 20 + dx, 376 + dy, 59, b.rollHp[k], f.hpMax, false, a);
    drawBattleHpBar(m, 100 + dx, 376 + dy, 49, b.rollMp[k], f.mpMax, true, a);
    drawBattleStatus(m, f, 152 + dx, 364 + dy, a);
}

// The results (__menuu_MbResultSet / Prc / Put). Prc 3: for (most levels + 1) x 60 frames the
// boards' EXP climbs (each member's gain over its own (levels + 1) x 60, the chime 0xa2 at a level)
// and HP fills over 60 frames, EXP Gained, OZ Earned and the purse count up a sixtieth a frame,
// a tick (0xa4) every second frame while anything came; a tap (0x93) puts everything at its end.
// Then a tap at a time: each member's level-up line (0xa2: 57008, or 57034 for several), each
// item's (0xa0: 57010, or 57035 with how many), and 0x93 as the windows go (4 frames)
void resultsNext(App& app) {
    App::Battle::Result& rs = app.battle.result;
    rs.frames = 0;
    if (rs.upNext < rs.ups.size()) {
        const auto [k, n] = rs.ups[rs.upNext++];
        menuSound(app, se::kMenuBase + 0x11);
        rs.stage = 1, rs.message = n == 1 ? 57008 : 57034, rs.value = n;
        const App::Battle::Actor* u = battleActor(app, k);
        rs.name = u ? u->name : std::string();
        rs.level[k] = rs.levelTo[k], rs.exp[k] = rs.expTo[k]; // (pcparam_change_lv)
        return;
    }
    if (rs.itemNext < rs.items.size()) {
        const auto [item, n] = rs.items[rs.itemNext++];
        menuSound(app, se::kMenuBase + 0xf);
        rs.stage = 2, rs.message = n == 1 ? 57010 : 57035, rs.value = n, rs.name = itemName(app, item);
        return;
    }
    menuSound(app, se::kChoose);
    rs.stage = 3, rs.count = 4;
}

void resultsSettle(App::Battle::Result& rs) { // __RestoreParam
    for (int k = 0; k < 2; ++k) rs.level[k] = rs.levelTo[k], rs.exp[k] = rs.expTo[k];
    rs.shownExp = float(rs.expTotal), rs.shownOz = float(rs.ozTotal), rs.shownPurse = float(rs.purseTo);
    rs.count = 0;
}

void resultsTap(App& app) {
    App::Battle::Result& rs = app.battle.result;
    if (rs.frames < 4 || rs.stage == 3) return; // (the windows coming in, or going)
    if (rs.stage == 0 && rs.count > 0) {
        menuSound(app, se::kChoose);
        resultsSettle(rs);
        return;
    }
    resultsNext(app);
}

void stepResults(App& app) {
    App::Battle& b = app.battle;
    App::Battle::Result& rs = b.result;
    ++rs.frames;
    if (rs.stage == 3) {
        if (--rs.count < 0) b.exit = 4;
        return;
    }
    if (rs.stage != 0 || rs.frames < 4 || rs.count <= 0) return;
    for (int k = 0; k < 2; ++k) {
        if (rs.character[k] < 0 || rs.exp[k] >= rs.expTo[k]) continue;
        rs.exp[k] = std::min(rs.expTo[k], rs.exp[k] + rs.expStep[k]);
        if (rs.level[k] < rs.levelTo[k] && rs.exp[k] >= float(cr::expForLevel(rs.level[k] + 1)))
            ++rs.level[k], menuSound(app, se::kMenuBase + 0x11);
    }
    auto climb = [](float& v, float to, float step) { if (v < to) v = std::min(to, v + step); };
    climb(rs.shownExp, float(rs.expTotal), float(rs.expTotal / 60));
    climb(rs.shownOz, float(rs.ozTotal), float(rs.ozTotal / 60));
    // the purse climbs by the earned OZ / 60 only while it is below the OZ earned (__menuu_MbResultPrc
    // compares DAT_000d2db8 with DAT_000d2d2c, not the new purse): a fuller purse holds until the settle
    if (rs.shownPurse < float(rs.ozTotal)) rs.shownPurse = std::min(9999999.0f, rs.shownPurse + float(rs.ozTotal / 60));
    --rs.count;
    if (rs.count % 3 == 0 && rs.tick) app.sound.play(se::kMenuBase + 0x13); // (__effgo 0x13 every third)
    if (rs.count < 1) resultsSettle(rs);
}

int pictAnimFrame(int draws, int ticks, int frames); // (menus.cpp)

// MbResultPut: "Results", the pair's boards (ComUty_FaceBoardDraw, 88 apart), EXP Gained and OZ
// Earned (ComUty_ExpAndOzDraw at (288, 76)), the play time and the purse, the message board at
// (0, 220) with its one line; windows 1 / 2 come from the left / right, 3 / 4 up from below
void drawBattleResults(App& app, cr::MenuPainter& m, float a) {
    App::Battle& b = app.battle;
    const App::Battle::Result& rs = b.result;
    const PhoneMenu phoneMenu(app, m); // MbResultPut: the iPhone screen's menu coordinates
    cr::Renderer& r = app.renderer;
    const float in = rs.stage == 3 ? std::max(0.0f, float(rs.count) / 4) : std::min(1.0f, float(rs.frames) / 4);
    const float away = 1 - in;
    r.setOrigin(wideLeft(app)); // (widescreen: the heading and boards left, the takings right)
    m.picture(19, 8, 4, a * in); // "Results"
    for (int k = 0; k < 2; ++k) {
        const cr::Fighter& f = b.rules.f[size_t(k)];
        if (!f.present) continue;
        const float y = 24 + 88.0f * float(k);
        // the board as it counts: its level and EXP, HP filling over 60 frames
        cr::PartyMember& pm = app.game.party[size_t(f.character)];
        const int keepLevel = pm.level, keepExp = pm.exp;
        if (rs.character[k] >= 0) pm.level = rs.level[k], pm.exp = int(rs.exp[k]);
        const float fill = rs.stage == 0 && rs.count > 0 ? std::min(1.0f, float(rs.frames) / 60) : 1;
        const int hp = rs.character[k] >= 0 ? rs.hp0[k] + int(float(f.hpMax - rs.hp0[k]) * fill) : f.hp;
        drawFaceBoard(app, m, -320 * away, y, f.character, hp, f.hpMax, f.mp, f.mpMax, a);
        pm.level = keepLevel, pm.exp = keepExp;
    }
    r.setOrigin(wideRight(app));
    const float rx = 320 * away;
    m.picture(67, 304 + rx, 48, a);
    m.picture(68, 288 + rx, 76, a); // EXP Gained
    m.picture(70, 340 + rx, 96, a);
    drawShopNumber(m, rs.expTotal, 358 + rx, 98, 6, 12, a); // (__menuu_MbResultPut: the totals, DAT_000d2d28 / 2d2c - not the counters)
    m.picture(69, 288 + rx, 132, a); // OZ Earned
    m.picture(70, 340 + rx, 152, a);
    drawShopNumber(m, rs.ozTotal, 358 + rx, 154, 6, 12, a);
    const int keepOz = app.game.oz;
    app.game.oz = int(rs.shownPurse); // (ComUty_BattryAndRealTimeAndOzDraw: the purse as it counts)
    drawClockAndOz(app, m, a * in);
    app.game.oz = keepOz;
    r.setOrigin(r.uiX());
    const float by = 160 * away;
    // ComUty_MsgBoardDraw(.., 3, .., 0, 0xdc) (2.2.0 0x4d23c, decomp 47977-48045): picture 12, the mark (picture 13
    // out and back) at (458, y + 78), the line only while the window stands still (!MuComIsWinCtlMoveCt)
    m.picture(12, 0, 220 + by, a);
    const short* mk = cr::kMenuPicture[13];
    m.piece(458, 298 + by, float(mk[0] + 20 * pictAnimFrame(rs.frames, 1, 4)), mk[1], mk[2], mk[3], a);
    std::vector<std::vector<std::string>> pages;
    const int value = rs.value;
    if (away <= 0.0f && app.systemText.message(rs.message, app.language, [&](int) { return value; }, pages, nullptr,
                               [&](int) { return rs.name; }) &&
        !pages.empty())
        for (size_t i = 0; i < pages[0].size() && i < 4; ++i) m.text(pages[0][i], 24, 234 + by + 16.0f * float(i), false, a, 14);
    r.setOrigin(wideLeft(app));
    if (keyHints(app)) // (our key hints)
        m.text("Enter: continue", 20, 372, false, a * 0.8f * in, 9);
    r.setOrigin(r.uiX());
}

void drawBattleHud(App& app, float W, float H) {
    (void)W;
    (void)H;
    using namespace btlui;
    App::Battle& b = app.battle;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    // a talk: the portraits and the message box instead - the left HUD sliding out first (+0x3c); the
    // damage numbers go on (__dmg_prc runs in btl_prc whatever the menu does)
    const bool talk = battletalk::on(app);
    m.begin(r, app.windowTex, &app.font);
    const bool results = b.phase == App::Battle::Results && !talk;
    if (results) {
        drawBattleResults(app, m, std::min(1.0f, float(b.phaseFrames + 1) / 8.0f));
        r.setOrigin(r.uiX());
        if (b.hudSlide >= 480) return; // (the badge and the rows still on their way out)
    }
    if (b.phase == App::Battle::Lost && b.eventBattle && b.gameOver.id && b.phaseFrames >= 10 && !talk) {
        // btl_main's put: tex2d_add(1, 112, 32, -10, 0, 0, 256, 256) - the screen's (256, 128), 512 x 512 -
        // at (frames x 255 / 20) alpha, full from the 20th
        r.setOrigin(r.uiX());
        const float ga = std::min(1.0f, float(b.phaseFrames - 10) / 20.0f);
        const float gw = float(b.gameOver.width), gh = float(b.gameOver.height);
        r.drawQuadUV(b.gameOver, 2.0f * 112 + 32, 2.0f * 32 + 64, 512, 512, 0, 0, gw, gh, 1, 1, 1, ga);
        return;
    }
    const float a = std::min(1.0f, float(b.frames) / 10.0f);
    // The iPhone layout (2.2.0's __menub_mainPut): the badge and the rows into its screen's
    // corners - (+16, +32) / (+16, -32) - the banner 32 lower, the menus where they are
    const bool phone = r.phoneLayout();
    const float edge = wideLeft(app); // the left-hand pieces' origin (the screen's edge inside the safe area)
    // Android 1.1.1's _menub_mainPut puts them on the phone's edges: the badge (- ex, - ey) (top left), the
    // rows (- ex, + ey) (bottom left), the banner - ey (top); the menus stay centred
    const float ex = androidEdgeX(app), ey = androidEdgeY(app);

    // Damage numbers (__dmg_prc 51605-51870): over their targets, projected from the 3D across the screen
    // and put in the 480 x 320 screen's units (tex2d's: the 1024 layout's (32, 64) + 2x) - y kept to
    // 72..272, x to 64 and over and at most 460 less 20 a digit past the first (widescreen: the margins
    // beyond the 1024 layout added); slots 1 and 3 48 lower (a heal 28) while slot 0 shows, slot 2's heal
    // within 20 of slot 0's put 30 higher. Drawn opaque (tex2d_add's own colour, not the HUD's fade)
    r.setOrigin(0);
    for (int slot = 0; slot < 5; ++slot) {
        App::Battle::Popup& p = b.popups[slot];
        if (!p.on) continue;
        const float* v = b.vp.m;
        const float cx = v[0] * p.at.x + v[4] * p.at.y + v[8] * p.at.z + v[12];
        const float cy = v[1] * p.at.x + v[5] * p.at.y + v[9] * p.at.z + v[13];
        float cw = v[3] * p.at.x + v[7] * p.at.y + v[11] * p.at.z + v[15];
        if (std::fabs(cw) < 0.01f) cw = 0.01f;
        const std::string t = std::to_string(std::abs(p.amount));
        float px = (cx / cw * 0.5f + 0.5f) * r.screenW(), py = (1 - (cy / cw * 0.5f + 0.5f)) * cr::Renderer::kLogicalH;
        r.screenToLayout(px, py); // (the iPhone layout draws 2D scaled; the projection is the screen's)
        float tx = (px - r.uiX() - 32) / 2, ty = (py - 64) / 2;
        placePopup(b, slot, tx, ty, r.uiX() / 2);
        const float x = tx + 16 + r.uiX() / 2, y = ty + 32, a = 1;
        const int step = (kPopupTime - p.timer) >> 4;
        if (p.type >= 1 && p.type <= 7) { // the label: Blowout! Critical! Weak! / Guard! Miss! Block!
            float lx = x - 72, ly = y - 24, la = 1;
            const int idx = hitLabel(p.type);
            if (p.type < 4) { // __dmg_prc: Blowout / Critical / Weak slide in from the right (x + 18k while the timer is
                // over 0x140) and out to the left (x - 18k under 0x40); the hop table is the others'
                if (p.timer >= 320) { const int k = 4 - (step); lx += 18.0f * float(std::max(0, k)); la = 1 - 0.25f * float(std::max(0, k)); }
                else if (p.timer < 64) { const int k = 4 - (p.timer >> 4); lx -= 18.0f * float(k); la = 1 - 0.25f * float(k); }
            } else if (step < 8)
                ly -= float(kLabelHop[step]);
            m.piece(lx, ly, float(idx % 4 * 144), float(idx / 4 * 24 + 72), 144, 24, a * la);
        }
        const bool number = p.type == cr::kHitNormal || p.type == cr::kHitCritical || p.type == cr::kHitWeak ||
                            p.type == cr::kHitResist;
        if (!number) continue;
        const int style = p.amount < 0 ? 3 : p.type == cr::kHitResist ? 2 : p.type == cr::kHitWeak ? 1 : 0;
        float dx = x - 10.0f * float(t.size());
        const float dy = y - (p.amount < 0 ? 24 : 40);
        for (size_t i = 0; i < t.size(); ++i, dx += 19) {
            const int s = step - int(i);
            const float hop = s >= 0 && s < 8 ? float(kDigitHop[s]) : 0;
            m.piece(dx, dy - hop, float((t[i] - '0') * 24 + style % 2 * 240), float(style / 2 * 24 + 24), 24, 24, a);
        }
    }

    // Top left: the Break badge - the ring, a piece per point, the holder's name (at x - +0x3c: a talk's slide)
    const int g = b.gaugeShown;
    const float hud = -float(b.hudSlide);
    if ((talk || results) && b.hudSlide >= 480) {
        r.setOrigin(r.uiX());
        return;
    }
    r.setOrigin(edge);
    m.setOffset((phone ? 16.0f - ex : 0.0f) + hud, phone ? 32.0f - ey : 0.0f);
    m.piece(0, 0, 0, 120, 64, 64, a);
    for (int i = 0; i < std::min(8, std::abs(g)); ++i) {
        const short* t = kBreakTick[i];
        if (g > 0) m.piece(t[0], t[1], t[2], t[3], 32, 32, a, 0x38 / 255.f, 1, 0xe3 / 255.f);
        else m.piece(t[0], t[1], t[2], t[3], 32, 32, a, 1, 0x14 / 255.f, 0x23 / 255.f);
    }
    if (g != 0 && b.breakPop > 1.0f) { // the label pops in large (12840-12902: tex2d_add_sca at the badge +
        // (32 (s - 1), 10 (s - 1)), x s about its centre, opaque)
        const float s = b.breakPop, sz = 64 * s, cx = 32 + (s - 1) * 32, cy = 32 + (s - 1) * 10;
        m.pieceTo(cx - sz / 2, cy - sz / 2, sz, sz, g < 0 ? 512 : 384, 240, 128, 128, a);
    } else
        m.piece(0, 0, g == 0 ? 320 : g < 0 ? 256 : 192, 120, 64, 64, a);
    m.setOffset(0, 0);

    r.setOrigin(r.uiX());
    if (!talk && !results) drawBreakFx(app, m); // btl_main 0x5dc
    r.setOrigin(edge);
    // Bottom left: the pair's rows (the one not choosing dimmed)
    const bool choosing = b.phase == App::Battle::Command && b.menu != App::Battle::Top && b.menu != App::Battle::EscapeAsk;
    m.setOffset(hud, 0);
    for (int k = 1; k >= 0; --k)
        drawBattleRow(app, m, k, choosing && !b.rules.pair && b.member != k, a);
    m.setOffset(0, 0);
    if (talk || results) { // (the banner and the menus are the talk's to cover; the results' own)
        r.setOrigin(r.uiX());
        return;
    }

    // The banner: the action's name, the target prompt, the messages
    r.setOrigin(r.uiX());
    if (!b.bannerShown.empty()) { // (font_set_utf16 fills slot 11, drawfont_btl draws it a frame on)
        const float clear[4] = {0, 0, 0, 0}, dark[4] = {0, 0, 0, 0x98 / 255.f};
        const float by = phone ? 32.0f - ey : 0.0f;
        m.quad(88, 4 + by, 24, 24, clear, dark);
        m.quad(112, 4 + by, 351, 24, dark, dark);
        m.quad(463, 4 + by, 24, 24, dark, clear);
        m.setOutline(false); // (the banner's text plain: __menub_mainPut's 0x3783c, drawfont_btl's twin, has no edge)
        m.textCentred(b.bannerShown, 287 - m.textWidth(b.bannerShown, 14) / 2, 16 + by, false, 1, 14);
        m.setOutline(true);
    }

    // Right: the menus
    r.setOrigin(wideRight(app));
    // the page as shown (b.view), slid right by b.slide (__menub_mainPut: x + +0x38) - the logic's state swapped
    // for the view's while it draws
    const App::Battle::MenuView& v = b.view;
    struct Keep { App::Battle::Menu menu; int cursor, member, pick, pickTop, confirm, targetAction; bool pair; std::vector<int> picks; };
    const Keep keep{b.menu, b.cursor, b.member, b.pick, b.pickTop, b.confirm, b.targetAction, b.rules.pair, b.picks};
    const bool shown = v.page >= 0 && b.slide < battleSlideDistance(app);
    if (shown) {
        b.menu = v.menu, b.cursor = v.cursor, b.member = v.member, b.pick = v.pick, b.pickTop = v.pickTop;
        b.confirm = v.confirm, b.targetAction = v.targetAction, b.rules.pair = v.pair, b.picks = v.picks;
        m.setOffset(float(b.slide), 0);
    }
    if (shown && !app.optionsOpen) { // (Options covers them while it's up)
        // the HELP board across the top (ComUty_HelpWindowDraw): the item's or gene's target and effect
        auto helpBoard = [&](int item) { drawHelpWindow(app, m, item, a); };
        switch (b.menu) {
        case App::Battle::Top:
            for (int i = 0; i < 4; ++i) {
                const bool dim = (i == 1 && (!(b.rules.f[0].alive() && b.rules.f[1].alive()) || b.rules.allyAuto)) ||
                                 (i == 2 && b.rules.escapeBlocked);
                drawBattleButton(m, kButtonX, kButtonY + kButtonStep * float(i), kTopIcon[i], kTopWord[i], b.cursor == i && battleCursorLit(app), dim, a);
            }
            break;
        case App::Battle::Commands:
            for (int i = 0; i < 4; ++i) {
                const bool dim = i == 1 && (battleGeneList(app).empty() || battleGenesSilenced(app));
                drawBattleButton(m, kButtonX, kButtonY + kButtonStep * float(i), kCmdIcon[i], kCmdWord[i], b.cursor == i && battleCursorLit(app), dim, a);
            }
            m.backBar(kBackX, kBackY, false, a);
            break;
        case App::Battle::Targets: {
            const int n = int(b.picks.size());
            const cr::BattleActionDef* act = app.battleActions.get(b.targetAction);
            const bool all = act && b.rules.isGroupScope(*act);
            for (int i = 0; i < n; ++i)
                drawBattlePlate(app, m, kPlateX, kPlateBottom - 48.0f * float(n - i), b.picks[size_t(i)], all || b.pick == i, a);
            m.backBar(kBackX, kBackY, false, a);
            break;
        }
        case App::Battle::Items: { // MuScrList: the battle items in the bag, how many of each
            const int n = int(b.picks.size());
            for (int k = 0; k < kItemRows && b.pickTop + k < n; ++k) {
                const int index = b.pickTop + k, item = b.picks[size_t(index)];
                const float y = 112 + btlui::kItemPitch * float(k);
                // ComUty_ComPlate_Item: the plate (8 / 9 picked), icon, name, how many at (222, 30)
                const auto it = app.game.items.find(item);
                drawCampRow(app, m, 232, y, item, it == app.game.items.end() ? 0 : it->second, index == b.pick, a);
            }
            m.backBar(kBackX, kBackY, false, a);
            if (b.pick >= 0 && b.pick < n) helpBoard(b.picks[size_t(b.pick)]); // (MuScrList_SetIdInfo's help: the items' too)
            break;
        }
        case App::Battle::Genes: { // the gene list: name, MP / cost (dim when the MP won't pay)
            const int n = int(b.picks.size()), have = battleGeneMp(app);
            for (int k = 0; k < kItemRows && b.pickTop + k < n; ++k) {
                const int index = b.pickTop + k, g = b.picks[size_t(index)], cost = geneMpCost(app, g);
                const float y = 112 + btlui::kItemPitch * float(k), ra = cost <= have ? a : a * 0.5f;
                // ComUty_ComPlate_Gene: the plate, icon, name, "MP/" (picture 27 at (188, 34)) and the cost
                // (three digits to (218, 30)), dim when the MP won't pay
                drawCampRow(app, m, 232, y, g, 0, index == b.pick, ra);
                m.picture(27, 232 + 188, y + 34, ra);
                drawShopNumber(m, cost, 232 + 218, y + 30, 3, 10, ra, 2); // (ComUty_ComPlate_Gene: PictNumDraw's field from 0xda, right-aligned)
            }
            m.backBar(kBackX, kBackY, false, a);
            if (b.pick >= 0 && b.pick < n) helpBoard(b.picks[size_t(b.pick)]); // the gene's target and effect
            break;
        }
        case App::Battle::EscapeAsk: { // 57015 on the board, Yes / No beneath
            const float mx = 96, my = 200;
            r.setOrigin(r.uiX());
            drawWinPiece(app, kWinSmallPanel, mx * 2, my * 2, 320 * 2, 48 * 2);
            m.textCentred(sysText(app, 57015), mx + 20, my + 24, false, a, 15);
            m.button(128, 264, 0, "", b.confirm == 0, a, b.confirm == 0); // Yes
            m.button(272, 264, 1, "", b.confirm == 1, a, b.confirm == 1); // No
            break;
        }
        }
    }
    if (shown) {
        m.setOffset(0, 0);
        b.menu = keep.menu, b.cursor = keep.cursor, b.member = keep.member, b.pick = keep.pick, b.pickTop = keep.pickTop;
        b.confirm = keep.confirm, b.targetAction = keep.targetAction, b.rules.pair = keep.pair, b.picks = keep.picks;
    }
    r.setOrigin(r.uiX());
    const char* help = b.phase != App::Battle::Command ? ""
                       : b.menu == App::Battle::Top ? "Up/Down  Enter"
                       : b.menu == App::Battle::EscapeAsk ? "Left/Right  Enter  Esc: no"
                                                          : "Up/Down  Enter  Esc: back";
    if (app.showHud) m.text(help, 300, 368, false, a * 0.6f, 9); // (our key hints)
}

// ---- Battle input ----

void battleKey(App& app, SDL_Keycode key) {
    App::Battle& b = app.battle;
    const bool back = key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_AC_BACK;
    const bool enter = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_E;
    if (battletalk::on(app)) { // a talk: the pages turn as on the field
        if (enter) {
            if (!app.dialogue.empty()) tapDialogue(app);
            else app.tapPending = true;
        }
        return;
    }
    const bool up = key == SDLK_UP || key == SDLK_W, down = key == SDLK_DOWN || key == SDLK_S;
    const bool left = key == SDLK_LEFT || key == SDLK_A, right = key == SDLK_RIGHT || key == SDLK_D;
    if (b.phase == App::Battle::Results) {
        if (enter || back) resultsTap(app);
        return;
    }
    if (b.phase == App::Battle::Won) {
        if (enter || back) b.wonTap = true;
        return;
    }
    if (b.phase == App::Battle::Lost) {
        if (enter || back) b.lostTap = true;
        return;
    }
    if (b.phase != App::Battle::Command || !battleMenuReady(app)) return;
    switch (b.menu) {
    case App::Battle::Top:
    case App::Battle::Commands:
        if (up || down) { b.cursor = (b.cursor + (up ? 3 : 1)) % 4; menuSound(app, se::kCursor); }
        else if (enter) { if (b.menu == App::Battle::Top) battleChooseTop(app, b.cursor); else battleChooseCommand(app, b.cursor); }
        else if (back && b.menu == App::Battle::Commands) battleBack(app);
        break;
    case App::Battle::Targets:
    case App::Battle::Items:
    case App::Battle::Genes: {
        const int n = int(b.picks.size());
        const bool targets = b.menu == App::Battle::Targets;
        if ((up || down || (targets && (left || right))) && n > 0) {
            b.pick = (b.pick + ((up || left) ? n - 1 : 1)) % n;
            b.pickTop = std::clamp(b.pickTop, std::max(0, b.pick - btlui::kItemRows + 1), b.pick);
            if (targets) {
                const cr::BattleActionDef* act = app.battleActions.get(b.targetAction);
                cameraForTarget(app, b.picks[size_t(b.pick)], act && b.rules.isGroupScope(*act), true); // (eased 1/2, held)
            }
            menuSound(app, targets ? se::kCursor : se::kListFocus); // (the items' / genes' MuScrList: __effgo(1))
        } else if (enter && n > 0) {
            if (targets) battleChooseTarget(app, b.picks[size_t(b.pick)]);
            else if (b.menu == App::Battle::Genes) battleChooseGene(app, b.picks[size_t(b.pick)]);
            else battleChooseItem(app, b.picks[size_t(b.pick)]);
        } else if (back)
            battleBack(app);
        break;
    }
    case App::Battle::EscapeAsk:
        if (up || down || left || right) { b.confirm ^= 1; menuSound(app, se::kCursor); }
        else if (enter) battleEscapeAnswer(app, b.confirm == 0);
        else if (back) battleEscapeAnswer(app, false);
        break;
    }
}

void battleClick(App& app, float px, float py) {
    using namespace btlui;
    App::Battle& b = app.battle;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    const float uiX = app.renderer.uiX(), rightX = app.renderer.phoneLayout() ? uiX : 2 * uiX;
    const float cx = (lx - uiX) / 2, rx = (lx - rightX) / 2, my = ly / 2;
    if (battletalk::on(app)) { // a talk: the skip corner, else the next page
        if (skipCorner(app, lx, ly)) holdSkip(app, kSkipPointer, true);
        else if (!app.dialogue.empty()) tapDialogue(app);
        else app.tapPending = true;
        return;
    }
    if (b.phase == App::Battle::Results) { // __MbResult_IsTouchChack: (0, 0, 480, 320)
        resultsTap(app);
        return;
    }
    if (b.phase == App::Battle::Lost) { // (anywhere: the defeat goes on with a tap)
        b.lostTap = true;
        return;
    }
    if (b.phase == App::Battle::Won) { // (anywhere ends the victory's hold)
        b.wonTap = true;
        return;
    }
    if (b.phase != App::Battle::Command || !battleMenuReady(app)) return;
    const bool onBack = my >= kBackY && my < kBackY + 24 && rx >= kBackX && rx < kBackX + 232;
    auto buttonAt = [&]() {
        for (int i = 0; i < 4; ++i)
            if (rx >= kButtonX && rx < kButtonX + 136 && my >= kButtonY + kButtonStep * i && my < kButtonY + kButtonStep * i + 40) return i;
        return -1;
    };
    switch (b.menu) {
    case App::Battle::Top:
        if (const int i = buttonAt(); i >= 0) { b.cursor = b.view.cursor = i; battleChooseTop(app, i); } // (the page as shown lights the tapped plate as it goes)
        break;
    case App::Battle::Commands:
        if (const int i = buttonAt(); i >= 0) { b.cursor = b.view.cursor = i; battleChooseCommand(app, i); }
        else if (onBack) battleBack(app);
        break;
    case App::Battle::Targets: {
        const int n = int(b.picks.size());
        for (int i = 0; i < n; ++i) {
            const float y = kPlateBottom - 48.0f * float(n - i);
            if (rx >= kPlateX && rx < kPlateX + 256 && my >= y && my < y + 40) {
                b.pick = i;
                battleChooseTarget(app, b.picks[size_t(i)]);
                return;
            }
        }
        if (onBack) battleBack(app);
        break;
    }
    case App::Battle::Items:
    case App::Battle::Genes:
        if (rx >= 232 && rx < 492 && my >= 112 && my < 112 + kItemPitch * kItemRows) {
            const int index = b.pickTop + int((my - 112) / kItemPitch);
            if (index < int(b.picks.size())) {
                b.pick = index;
                if (b.menu == App::Battle::Genes) battleChooseGene(app, b.picks[size_t(index)]);
                else battleChooseItem(app, b.picks[size_t(index)]);
            }
        } else if (onBack)
            battleBack(app);
        break;
    case App::Battle::EscapeAsk:
        if (my >= 264 && my < 304 && cx >= 128 && cx < 240) battleEscapeAnswer(app, true);
        else if (my >= 264 && my < 304 && cx >= 272 && cx < 384) battleEscapeAnswer(app, false);
        break;
    }
}


} // namespace crapp
