#pragma once
// The battle rules (btl_main and its btlcalc_* / btlord_* / btlai_* / btlbreak_* helpers): who
// fights, in what order, what each action does and what winning is worth. No drawing here: the
// battle scene (main.cpp) asks for a round, plays each resolved action and shows its hits.
//
// Fighters (the context's 0xb0-byte actors): 0 and 1 the pair, 2..4 the enemies.
//  - The pair (btlpcparam_prty_init): the menus' stats (level + equipment); HP full at the start of
//    every battle, MP from the save (written back afterwards).
//  - Enemies (read_btlcpl / btl_enemy_init_lv): the monster row's stats grown by level L (normal:
//    HP 30L, attack 6L, defense 4L, speed 5L, luck 4L, Oz 10L; strong growth (+28): 200L, 12L, 10L,
//    8L, 9L, 15L; experience + L squared either way). L is the row's level (+32) if set, else the
//    leader's level, or the pair's average when it's the route's own pair; ordinary monsters
//    without a level pick one in the area's band (10b+1..10b+10, kept within L-5..L+2).
//
// Actions (sys.evt +0x2c, 24 bytes, 1-based): s16 +0, power +2, MP +4, speed +6, calc +8; u8 icon
// +10, class +11 (the animation slot: 8-10 attacks, 11 genes, 12 items), scope +12 (1 self, 2/4/6
// one of own side / opposite / anyone, 3/5/7 all of them), side +13 (1 self, 2 own, 3 opposite,
// 4 any), pick +14 (the AI's target: 1 random, 2 lowest HP, 3 highest, 4 / 5 the first / second),
// always hits +15, variance % +16, motion +17, element +18 (sets the user's), affinity +22 (the
// element triangle applies).
//  - 10..17 each character's attack (character + 10; monsters use them too), 18..21 the routes'
//    pair attacks (route + 18), 26 defend, 100..125 battle items 21..46, 201.. genes.
//
// A round (btl_main 400-899): commands (the pair's, then each enemy's from its AI lists), a queue
// by speed (the actors' average + the action's +6; +19998 with the "act first" status), then the
// actions one by one. Defend takes hold at once (state 500). Pair mode: one command for both,
// attacks add up both attackers, and a single target on the pair's side becomes both.
//
// Damage (btlcalc_0404, physical; 0406 genes; 0417 healing):
//    bonus = pair side: +75 when it holds the Break, then +25 in pair mode, else +50 for actor 0
//            (0406: actor 1) / enemies: +50, +125 when they hold the Break
//    damage = (bonus + power) x (sum of the attackers' attack) / 100 - target's defense
//  - physical: a critical (+50 bonus) when random(256) < 10/15/20/50 (the attackers' average
//    level against the target's: < -49 / < 0 / < 50 / more); otherwise it hits with the speed sum
//    against the target's luck (< -100: 50%, < -49: 60, < 0: 70, < 50: 80, < 101: 90, else 99;
//    100 when the action always hits)
//  - the element triangle (1 beats 3, 2 beats 1, 3 beats 2): x1.5 ("weak"), the other way x0.5
//  - monsters' resistances (+16 physical, +18 genes, +20/22/24 elements 1-3 (0 counts as 1)):
//    1 none (immune), 2 half, 3 x1.5, 4 instant death
//  - variance: x (100 - v + random(2v + 1)) / 100
//  - healing: -(bonus + power) x (sum of luck) / 100, bonus party +100 with the Break, +50 when
//    actor 1 casts; enemies 150 with the Break, else 50
//  Attack, defense, speed and luck are x1.25 under their status (+0x8b..+0x8e); defense doubles
//  while defending.
//
// The Break (btlbreak_set): a gauge -8..8 (> 0 the pair holds it, < 0 the enemies). The first
// event of the battle puts it at 4 for whoever caused it. After that, each side's events move it
// its way: damage dealt (once an action) 1, a critical 1 (once an action), a kill 2, a round in
// which the holder wasn't hit 1 - but for the holder only kills and untouched rounds count, 1
// each. Reaching 0 from either side makes it even again.
//
// Rewards (btlcalc_chk_dead): each enemy's experience and Oz when it falls. The pair gets the
// experience (both members, as the result screen shows them), the Oz goes to the purse. A fallen
// monster may also leave an item (btlcalc_0505: row +0xc0 below level 20, +0xc2 or +0xc0 to 39,
// +0xc4 a third of the time from 40; 20 / 30 / 40 / 50% for the attackers' average level + 10 less
// its level < -4 / < 1 / < 6 / more), teach a gene (btlcalc_0506, game/genes.hpp) and leave its
// gene plate.
//
// Skills (chk_skill_btl; the pair's from their worn plates, in pair mode either member's; a
// monster's from its row's +0xaa list once it's level 20): 131 / 136 block a physical / gene hit
// (10%, 20% holding the Break: "Miss!"), 132 halves gene MP, 133 / 138 give back 5% HP / MP a
// round, 134 doubles a critical, 135 makes the first attack critical, 137 ignores defense on a
// critical, 139..152 scale damage (btlcalc_zoku_0204: resisting 75%, then +25 / -25 for each
// attack, element and herbivore / carnivore skill that applies; the monster row's +0x38 / +0x36
// mark herbivores / carnivores), 153 guards at a quarter of the HP or less, 154 counters a
// physical hit (10 / 20%), 155 doubles healing, 156 genes x1.2, 158 more genes taught, 160 sure
// escapes, 161 / 162 x1.2 experience / Oz, 163 MP back to full after the battle, 164..169 the
// level-up boosts. 157 (stealing) and 159 (more items: the original reads it and drops the result)
// do nothing here.
//
// Not kept yet: most special calcs (the boss actions, status ailments beyond the buffs).

#include <array>

#include "core/random.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace cr {

class BattleData;
class ItemTable;
struct GameState;
struct MonsterDef;

struct BattleActionDef {
    int id = 0;
    int power = 0, mp = 0, speed = 0, calc = -1;
    int icon = 0, cls = 0, scope = 0, side = 0, pick = 0;
    bool alwaysHits = false;
    int variance = 0, motion = 0, element = 0;
    bool affinity = false;
    bool hostile = false;   // +23 == 0: a barrier (calc 26) neutralizes it
    // The voiced releases' records (2.2.0: 38 bytes, these six s16 at +10, the rest 12 later):
    // the battle-voice situations a turn's actor says - the party's for its three variants, then
    // a monster's (_btlvoice_turn); -1 none
    int voice[6] = {-1, -1, -1, -1, -1, -1};
};

class BattleActionTable {
public:
    bool load(const std::string& dataDir); // sys.evt
    bool loaded() const { return !actions_.empty(); }
    const BattleActionDef* get(int id) const; // null when out of range or empty
    int count() const { return int(actions_.size()); }

private:
    std::vector<BattleActionDef> actions_;
};

// The statuses the actors keep at +0x89..+0x93, as indices into Fighter::status
enum BattleStatus {
    kStatusPoison,     // +0x89: 10% of the most HP each round (genes 122 / 224)
    kStatusBarrier,    // +0x8a: the next hostile gene is neutralized (calc 26)
    kStatusAttackUp,   // +0x8b x1.25 (calc 27)
    kStatusDefenseUp,  // +0x8c (calc 28)
    kStatusSpeedUp,    // +0x8d (calc 29)
    kStatusLuckUp,     // +0x8e (calc 30)
    kStatusSilence,    // +0x8f: no genes (calc 31)
    kStatusEndure,     // +0x90: the next fatal hit leaves 1 HP (calc 32)
    kStatusInvincible, // +0x91: no damage, acts first (calc 33, for the next round)
    kStatusUnused92,
    kStatusUnused93,
    kStatusCount
};

struct Fighter {
    bool present = false;
    bool enemy = false;
    int character = -1;          // the pair: 0..7
    int monster = 0;             // enemies: 1-based monster id
    const MonsterDef* def = nullptr;
    int level = 1;
    int hp = 0, hpMax = 0, mp = 0, mpMax = 0;
    int atk = 0, grd = 0, spd = 0, lck = 0;
    int element = 0;             // +0x88: 0 none, 1..3 (genes set it)
    int status[kStatusCount] = {};
    bool guarding = false;       // +0x95
    int exp = 0, oz = 0;         // what falling is worth
    bool rewarded = false;
    bool guest = false;            // prty_init_special: not in the save
    bool robbed = false;           // +0xa0: Steal Item took its item already
    std::array<bool, 56> skills{}; // 131..186
    bool firstAttack = true;       // skill 135's
    bool gone = false;             // fled the battle (calc 11, +0x93)
    bool counterStance = false;    // calc 34 (+0x94): answers every physical hit this round
    int stolen = 0;                // Oz it took (calc 10, +0x9c): back when it falls
    bool alive() const { return present && hp > 0 && !gone; }
};

// What one target took: amount > 0 damage, < 0 healing. The types are __dmg_set's (its labels:
// 1 Blowout!, 2 Critical!, 3 Weak!, 5 No Effect!, 6 Guard!, 7 Miss!; numbers with 0, 2, 3, 8);
// kHitQuiet shows nothing (statuses, the do-nothing actions)
enum BattleHitType {
    kHitQuiet = -1, kHitNormal = 0, kHitKill = 1, kHitCritical = 2, kHitWeak = 3, kHitNoEffect = 5,
    kHitGuard = 6,  // the speed-against-luck check failed: fended off
    kHitMiss = 7, kHitResist = 8
};
struct BattleHit {
    int target = -1;
    int amount = 0;
    int type = kHitNormal;
    bool mp = false;      // an MP change (items 103..105)
    bool fell = false;    // this hit took the target's last HP
    int status = -1;      // a status it set (BattleStatus), -1 none
    bool revived = false; // calc 19 brought it back
};

enum BattleCommandKind { kCmdNone = 0, kCmdAttack = 1, kCmdGene = 2, kCmdItem = 3, kCmdDefend = 4, kCmdEscape = 5 };
struct BattleCommand {
    int kind = kCmdNone;
    int action = 0;       // gene action (kCmdGene)
    int item = 0;         // item number (kCmdItem)
    int target = -1;      // fighter index for single-target actions
};

// btl_add_msgque: a status message waiting for the banner - fighter (< 0: -item, its name in
// var 9 - Steal Item), entry in font block 6 (57000 + entry). One per fighter.
struct BattleNotice {
    int fighter = 0;
    int entry = 0;
};

// One resolved action, as the scene plays it
struct BattleTurn {
    int kind = kCmdNone;
    int action = 0;
    int item = 0;                // the item used (taken from the bag)
    int actors[2] = {-1, -1};
    std::vector<BattleHit> hits;
    bool escaped = false;        // kCmdEscape: it worked
    bool counter = false;        // skill 154's answer to a hit
    int message = 0;             // a system message to show for it (57xxx), 0 none
    int messageValue = 0;        // its number (Oz stolen) or the element / kind it names
    bool gone = false;           // calc 11: the enemy fled
    int escapeChance = 0;
    std::vector<BattleNotice> notices; // the calcs' queued messages (the scene shows them in turn)
};

class BattleRules {
public:
    std::array<Fighter, 5> f;
    bool pair = false;           // +0xa48: the pair acts as one this round
    bool enemyPair = false;      // +0xa4c: enemies 2 and 3 act together this round
    int breakGauge = 0;          // +0x12, -8..8
    int breakTarget = 0;         // +0x14 (the menu moves the gauge to it a step a frame)
    bool breakStarted = false;   // G+0x2c0 bit 15
    unsigned breakFlags = 0;     // +0x18
    int round = 0;               // +0x1a
    int strike = 0;              // BattleStart::strike, for the first round
    bool escapeBlocked = false;  // G+0x2c0 bit 0 (script battles)
    bool allyAuto = false;       // +0xa4f: member 1 is a guest who acts by itself (btlord_enemy_rand)
    int allyScript = 0;          // its rotation: 1 (group 370), 2 (groups 372 / 373)
    int allyCounter = 0;         // +0x17c
    int forceAction = 0;         // tests (--battle-view stage<n>): enemy 2 always chooses this action
    int expGained = 0, ozGained = 0;
    int purse = 0;               // the party's Oz at the start (calc 10 takes from it)
    int ozStolen = 0;            // what the enemies took
    bool doubleExp = false, doubleOz = false; // calcs 35 / 36 (Paradise Apples)
    std::vector<int> drops;      // items (btlcalc_0505), gene plates (chk_dead), in the order won
    std::vector<int> learned;    // abilities and skills taught (btlcalc_0506)
    std::vector<int> stolen;     // Steal Item's catch (btlcalc_0504: kept whatever the outcome)
    std::vector<std::string> unsupported; // calcs it met that aren't implemented
    // What the battle tutorials (EvrStart 2, var 1 = 0xff3..0xff5) look for, set for the rest of
    // the battle: G+0xa6 a gene gave its users (0406) or its targets (zokusei_chr1) its
    // element, +0xa7 a status took hold (0406's poison, 0302 05..12), +0xa8 an ability or
    // skill was learned (0506)
    bool elementTaken = false, statusTaken = false, learnedNew = false;
    // The story battles' scripted rules (set by the scene from the group, the route and flags):
    // chk_karaok_dmg_hp - no damage takes a fighter below 1 HP; btlcalc_status_prc - these
    // fighters are back to full HP as the round ends; +0xa50 - a pair attack (action 18) landed
    // after the third round (btlcalc_0404)
    bool keepAlive = false;
    std::array<bool, 5> regenFull{};
    bool lateAttack = false;

    // _get_random: the game's one stream (useRandom: the app's), or a stream of its own (seed: tests)
    void seed(uint32_t s) { own_.seed(s), shared_ = nullptr; }
    void useRandom(GameRandom* r) { shared_ = r; }
    int random(int n); // 0..n-1

    // The pair `route` (characters 2 route, 2 route + 1) from the party
    void setupParty(const GameState& gs, const ItemTable& items, int route);
    // btl_prty_init: the story battles' own line-ups - one member alone, a guest beside the
    // leader (prty_init_special: level + 5, base stats), or the partner away (flag 4077 + route)
    void setupSpecial(const GameState& gs, const ItemTable& items, int group, int route);
    // The group's monsters; `band` is the area's progress 0..9 (the original's per-route byte)
    void setupEnemies(const BattleData& data, const std::array<int, 3>& ids, int band);
    int leaderLevel() const;

    // The fighters `action` may be aimed at by `user` (btlord_make_select), the living ones
    std::vector<int> targets(const BattleActionDef& a, int user) const;
    bool isGroupScope(const BattleActionDef& a) const { return a.scope == 3 || a.scope == 5 || a.scope == 7; }
    // The attack the command menu's Attack gives (solo: character + 10, pair: route + 18)
    int attackAction(int fighter) const;

    // Round: the pair's commands (pair mode: cmd[0] for both), enemies choose, the queue is made
    void beginRound(const BattleCommand cmd[2], const BattleActionTable& actions);
    // The next action of the round; false when the round is over (or the battle is)
    bool nextTurn(const BattleActionTable& actions, BattleTurn& out);
    // Statuses count down, defending ends, the untouched holder gains (btlord_chk)
    void endRound();
    // 0 still going, 1 escaped, 2 won, 3 lost
    int outcome() const;
    // Applies the battle to the save: MP, and when they won the experience, Oz, drops and genes
    // learned. Returns the party members that levelled up (character numbers).
    std::vector<int> applyRewards(GameState& gs) const;

    // btlbreak_set: `who` caused event `event` (2 damage dealt, 8 critical, 0x10 kill, 0x20, 0x40 untouched)
    void breakEvent(int who, int event, int target);
    void catchUpBreak() { breakGauge = breakTarget; }
    // btlord_chk: a side lost the Break (+0x18 bit 10) - the effect plays, and half way through
    // (btlbreak_effect_prc) the other side takes it at 4 (bit 11: the pair)
    bool breakChanging() const { return breakFlags & 0x400; }
    void takeBreak() {
        breakTarget = breakGauge = (breakFlags & 0x800) ? 4 : -4;
        breakFlags &= ~(0x400u | 0x800u);
    }

    // The calcs, exposed for the self-test. `actors` are the attackers (-1 = none)
    BattleHit physical(const BattleActionDef& a, const int actors[2], int target);
    BattleHit magical(const BattleActionDef& a, const int actors[2], int target);
    BattleHit heal(const BattleActionDef& a, const int actors[2], int target);
    // chk_skill_btl: fighter i has skill `skill` (the pair: either member's in pair mode)
    bool has(int i, int skill) const;
    // __GetBtlGeneMp: what an ability costs fighter `user` (skill 132 halves it)
    int geneCost(const BattleActionDef& a, int user) const;
    int atkOf(int i) const;
    int grdOf(int i) const;
    int spdOf(int i) const;
    int lckOf(int i) const;

private:
    struct Entry {
        bool counter = false;
        int repeats = 1;          // +0xb: monsters with row +0x3e 2 act twice; Asura Lotus 2..5 times
        bool paid = false;        // a repeat: the MP went with the first
        bool done = false;        // +0x399: acted, or cancelled (Trompe-L'oeil)
        int kind = kCmdNone;
        int action = 0, item = 0;
        int actors[2] = {-1, -1};
        int target = -1;
        int speed = 0;
        int order = 0;
    };
    std::vector<Entry> queue_;
    size_t next_ = 0;
    std::vector<BattleNotice> notices_; // this action's, handed to BattleTurn::notices
    int note_ = 0;                      // a message the calcs show at once (an immunity), 0 none
    bool cancelTried_ = false;          // G+0xaa: Trompe-L'oeil's cancel came up (once a battle)
    bool cancelBreak_ = false;          // G+0xa9: its Break event, the once it isn't held
    int firstTarget_ = -1;              // G+0x7a: the action's first target (btlcalc_critical)
    int soloMember_ = 0;                // G+0xa4a: whose EXP / Oz skills count out of pair mode
    void notice(int fighter, int entry);
    GameRandom own_{1};
    GameRandom* shared_ = nullptr;
    bool escaped_ = false;
    std::vector<int> known_;     // abilities, skills and plates the bag held at the start

    void fallRewards(int enemy, const int actors[2]);
    int zoku(int amount, const BattleActionDef& a, int user, int target, bool magic) const;
    bool holdsBreak(int i) const { return i < 2 ? breakGauge > 0 : breakGauge < 0; }
    bool blocked(int target, int skill);
    bool knows(int item) const;

    int bonusFor(const int actors[2], bool magic) const;
    int elementScale(const BattleActionDef& a, int user, int target, int& type) const;
    int resist(const BattleActionDef& a, int user, int target, int amount, bool magic, int& type) const;
    void enemyChoice(int enemy, bool asPair, Entry& e, const BattleActionTable& actions, int exclude = 0);
    int aiTarget(const BattleActionDef& a, int user);
    std::vector<int> resolveTargets(const BattleActionDef& a, const Entry& e);
};

} // namespace cr
