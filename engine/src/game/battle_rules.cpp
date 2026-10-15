#include "game/battle_rules.hpp"

#include <algorithm>
#include <cstring>

#include "assets/battle_data.hpp"
#include "core/file.hpp"
#include "game/event_vm.hpp"
#include "game/genes.hpp"
#include "game/party.hpp"
#include "game/shop.hpp"

namespace cr {

namespace {

int s16(const uint8_t* p) { return int16_t(uint16_t(p[0] | p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

constexpr int kFirstBattleItem = 21, kBattleItems = 26, kItemAction = 100; // item 21 -> action 100
constexpr int kDefendAction = 26;

int monsterField(const Fighter& f, int offset) { return f.def ? s16(&f.def->raw[size_t(offset)]) : 0; }

} // namespace

bool BattleActionTable::load(const std::string& dataDir) {
    actions_.clear();
    std::vector<uint8_t> f;
    if (!readFile(dataDir + "sys.evt", f) || f.size() < 0x80) return false;
    const uint32_t at = u32(&f[0x2c]);
    if (!at || at >= f.size()) return false;
    uint32_t end = uint32_t(f.size());
    for (size_t h = 0; h < 0x80; h += 4) {
        const uint32_t o = u32(&f[h]);
        if (o > at && o < end) end = o;
    }
    // The voiced releases (sys.evt with section +0x40, the battle-voice table) keep 38-byte records:
    // six voice situations at +10, the 1.0.1 fields from there on 12 bytes later, 2 more at the end
    const bool voiced = u32(&f[0x40]) != 0;
    const uint32_t stride = voiced ? 38 : 24;
    for (uint32_t p = at; p + stride <= end; p += stride) {
        const uint8_t* base = &f[p];
        BattleActionDef a;
        a.id = int(actions_.size()) + 1;
        a.power = s16(base + 2);
        a.mp = s16(base + 4);
        a.speed = s16(base + 6);
        a.calc = s16(base + 8);
        if (voiced)
            for (int k = 0; k < 6; ++k) a.voice[k] = s16(base + 10 + 2 * k);
        const uint8_t* r = voiced ? base + 12 : base; // r[10..23]: the 1.0.1 layout
        a.icon = r[10];
        a.cls = int8_t(r[11]);
        a.scope = r[12];
        a.side = r[13];
        a.pick = r[14];
        a.alwaysHits = r[15] == 1;
        a.variance = int8_t(r[16]);
        a.motion = r[17];
        a.element = int8_t(r[18]);
        a.affinity = r[22] == 1;
        a.hostile = r[23] == 0;
        actions_.push_back(a);
    }
    return !actions_.empty();
}

const BattleActionDef* BattleActionTable::get(int id) const {
    return id >= 1 && id <= int(actions_.size()) ? &actions_[size_t(id - 1)] : nullptr;
}

int BattleRules::random(int n) { return (shared_ ? *shared_ : own_).get(n); }

void BattleRules::setupParty(const GameState& gs, const ItemTable& items, int route) {
    route = std::clamp(route, 0, 3);
    known_.clear();
    purse = gs.oz;
    for (const auto& [item, count] : gs.items)
        if (count > 0 && ((item >= 81 && item <= 186) || (item >= 479 && item <= 500))) known_.push_back(item);
    for (int k = 0; k < 2; ++k) {
        Fighter& x = f[size_t(k)];
        x = Fighter();
        x.present = true;
        x.character = 2 * route + k;
        const PartyMember& m = gs.party[size_t(x.character)];
        x.level = m.level;
        x.hpMax = memberStat(gs, x.character, kHp, items);
        x.mpMax = memberStat(gs, x.character, kMp, items);
        x.hp = x.hpMax; // btlpcparam_prty_init: every battle starts with full HP
        x.mp = m.mp < 0 ? x.mpMax : std::min(m.mp, x.mpMax);
        x.atk = memberStat(gs, x.character, kAttack, items);
        x.grd = memberStat(gs, x.character, kDefense, items);
        x.spd = memberStat(gs, x.character, kSpeed, items);
        x.lck = memberStat(gs, x.character, kLuck, items);
        for (int s = 131; s <= 186; ++s) x.skills[size_t(s - 131)] = hasSkill(gs, x.character, s);
    }
}

void BattleRules::setupSpecial(const GameState& gs, const ItemTable& items, int group, int route) {
    struct Solo { int group, route, character; };
    static const Solo kSolo[] = {{371, 0, 0}, {283, 1, 2}, {376, 2, 4}, {172, 2, 4}, {374, 2, 4},
                                 {350, 2, 5}, {377, 2, 4}, {375, 3, 6}, {372, 1, 3}};
    auto member = [&](int k, int c) {
        Fighter& x = f[size_t(k)];
        x = Fighter();
        x.present = true;
        x.character = c;
        const PartyMember& m = gs.party[size_t(c)];
        x.level = m.level;
        x.hpMax = memberStat(gs, c, kHp, items);
        x.mpMax = memberStat(gs, c, kMp, items);
        x.hp = x.hpMax;
        x.mp = m.mp < 0 ? x.mpMax : std::min(m.mp, x.mpMax);
        x.atk = memberStat(gs, c, kAttack, items);
        x.grd = memberStat(gs, c, kDefense, items);
        x.spd = memberStat(gs, c, kSpeed, items);
        x.lck = memberStat(gs, c, kLuck, items);
        for (int s = 131; s <= 186; ++s) x.skills[size_t(s - 131)] = hasSkill(gs, c, s);
    };
    if (gs.flag(4077 + route)) f[1] = Fighter(); // the partner is away
    for (const Solo& s : kSolo)
        if (s.group == group && s.route == route) {
            member(0, s.character);
            f[1] = Fighter();
        }
    struct Guest { int group, route, character, script; };
    static const Guest kGuest[] = {{370, 0, 4, 1}, {372, 1, 6, 2}, {373, 2, 6, 2}};
    for (const Guest& g : kGuest)
        if (g.group == group && g.route == route) {
            enemyLevelFrom = leaderLevel(); // (btl_prty_init: the enemies' levels come before the guest)
            Fighter& x = f[1];
            x = Fighter();
            x.present = x.guest = true;
            x.character = g.character;
            x.level = std::min(kMaxLevel, f[0].level + 5);
            // btlpcparam_prty_init_special: base + level x 50 / x 5, nothing else - and (in both
            // releases) Speed and Luck from the base Defense too
            auto at = [&](Stat s, int per) { return baseStat(g.character, s) + x.level * per; };
            x.hp = x.hpMax = at(kHp, 50);
            x.mp = x.mpMax = at(kMp, 5);
            x.atk = at(kAttack, 5);
            x.grd = x.spd = x.lck = at(kDefense, 5);
            (void)items;
            allyAuto = true;
            allyScript = g.script;
        }
}

int BattleRules::leaderLevel() const {
    // btl_enemy_init_lv: the pair's average when both are the route's own, else the leader's
    if (enemyLevelFrom >= 0) return enemyLevelFrom;
    if (f[0].present && f[1].present && f[1].character == (f[0].character | 1) && !(f[0].character & 1))
        return (f[0].level + f[1].level) / 2;
    return f[0].level;
}

void BattleRules::setupEnemies(const BattleData& data, const std::array<int, 3>& ids, int band) {
    const int L = leaderLevel();
    const int hi = L > 98 ? 100 : L + 2, lo = L < 6 ? 1 : L - 5;
    band = std::clamp(band, 0, 9);
    for (int k = 0; k < 3; ++k) {
        Fighter& x = f[size_t(2 + k)];
        x = Fighter();
        const MonsterDef* m = data.monster(ids[size_t(k)]);
        if (!m || m->model < 0) continue;
        x.present = x.enemy = true;
        x.monster = m->id;
        x.def = m;
        int lv;
        if (!m->strongGrowth) {
            if (m->level >= 1) lv = m->level;
            else { // the band's ten levels, kept near the pair's
                const int top = band * 10 + 10, bottom = band * 10 + 1;
                if (lo >= top) lv = top;
                else if (bottom >= hi) lv = bottom;
                else {
                    const int a = std::max(lo, bottom), b = std::min(hi, top);
                    lv = a + random(b - a + 1);
                }
            }
        } else
            lv = m->level > 0 ? m->level : L;
        x.level = std::clamp(lv, 1, 100);
        x.hp = x.hpMax = int(m->hpAt(x.level));
        x.atk = m->atkAt(x.level);
        x.grd = m->grdAt(x.level);
        x.spd = m->spdAt(x.level);
        x.lck = m->lckAt(x.level);
        x.exp = int(m->expAt(x.level));
        x.oz = int(m->genePointsAt(x.level));
        if (x.level >= 20) // chk_skill_btl: a monster's skills (+0xaa, five) count from level 20
            for (int k = 0; k < 5; ++k)
                if (const int s = monsterField(x, 0xaa + 2 * k); s >= 131 && s <= 186) x.skills[size_t(s - 131)] = true;
    }
}

int BattleRules::atkOf(int i) const {
    const Fighter& x = f[size_t(i)];
    return x.status[kStatusAttackUp] ? x.atk + x.atk / 4 : x.atk;
}
int BattleRules::grdOf(int i) const {
    const Fighter& x = f[size_t(i)];
    int v = x.status[kStatusDefenseUp] ? x.grd + x.grd / 4 : x.grd;
    if (i < 2 && x.guarding) v *= 2;
    return v;
}
int BattleRules::spdOf(int i) const {
    const Fighter& x = f[size_t(i)];
    return x.status[kStatusSpeedUp] ? x.spd + x.spd / 4 : x.spd;
}
int BattleRules::lckOf(int i) const {
    const Fighter& x = f[size_t(i)];
    return x.status[kStatusLuckUp] ? x.lck + x.lck / 4 : x.lck;
}

int BattleRules::attackAction(int fighter) const {
    if (fighter < 2 && pair) return f[0].character / 2 + 18;
    return f[size_t(fighter)].character + 10;
}

bool BattleRules::has(int i, int skill) const {
    if (i < 0 || i > 4 || skill < 131 || skill > 186) return false;
    if (f[size_t(i)].skills[size_t(skill - 131)]) return true;
    return i < 2 && pair && f[size_t(1 - i)].skills[size_t(skill - 131)];
}

int BattleRules::geneCost(const BattleActionDef& a, int user) const {
    return user < 2 && has(user, 132) ? a.mp / 2 : a.mp;
}

// skills 131 / 136: the target fends the whole hit off ("Miss!")
bool BattleRules::blocked(int target, int skill) {
    return has(target, skill) && random(100) < (holdsBreak(target) ? 20 : 10);
}

// btlcalc_zoku_0204: the skills' scaling of a hit
int BattleRules::zoku(int amount, const BattleActionDef& a, int user, int target, bool magic) const {
    int pct = has(target, magic ? 147 : 146) ? 75 : 100;
    if (has(user, magic ? 140 : 139)) pct += 25;
    const int e = f[size_t(user)].element;
    if (a.affinity)
        for (int k = 1; k <= 3; ++k)
            if (e == k && has(target, 147 + k)) pct -= 25; // Blaze / Aqua / Gale Resistance
    if (has(target, 151) && monsterField(f[size_t(user)], 0x38)) pct -= 25; // a herbivore's attack
    if (has(target, 152) && monsterField(f[size_t(user)], 0x36)) pct -= 25; // a carnivore's
    if (a.affinity)
        for (int k = 1; k <= 3; ++k)
            if (e == k && has(user, 140 + k)) pct += 25; // Blaze / Aqua / Gale Attack
    if (has(user, 144) && monsterField(f[size_t(target)], 0x38)) pct += 25;
    if (has(user, 145) && monsterField(f[size_t(target)], 0x36)) pct += 25;
    return pct < 0 ? 0 : pct * amount / 100;
}

std::vector<int> BattleRules::targets(const BattleActionDef& a, int user) const {
    std::vector<int> out;
    const bool party = user < 2;
    auto side = [&](bool own) {
        const bool partySide = own == party;
        for (int i = partySide ? 0 : 2; i < (partySide ? 2 : 5); ++i) out.push_back(i);
    };
    switch (a.scope) {
    case 1: out.push_back(party && pair ? 0 : user); break;
    case 2: case 3: side(true); break;
    case 4: case 5: side(false); break;
    case 6: case 7: for (int i = 0; i < 5; ++i) out.push_back(i); break;
    default: side(false); break;
    }
    const bool revive = a.id == 0x6c || a.id == 0xe6;
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](int i) { return !f[size_t(i)].present || f[size_t(i)].gone || (!revive && f[size_t(i)].hp <= 0); }),
              out.end());
    return out;
}

void BattleRules::breakEvent(int who, int event, int target) {
    const bool party = who < 2;
    if (!breakStarted) { // the battle's first event: four for whoever caused it
        breakStarted = true;
        breakTarget = party ? 4 : -4;
        if (event == 2) breakFlags |= 0x200;
        return;
    }
    int d = 0;
    switch (event) {
    case 2: // damage dealt: once an action; the holder was hit
        if (target < 2 ? breakGauge > 0 : breakGauge < 0) breakFlags &= ~0x8000u;
        d = (breakFlags & 0x200) ? 0 : 1;
        breakFlags |= 0x200;
        break;
    case 4: d = 1; break;
    case 8: { // a critical: once an action per side
        const unsigned bit = party ? 0x1000 : 0x2000;
        d = (breakFlags & bit) ? 0 : 1;
        breakFlags |= bit;
        break;
    }
    case 0x10: d = 2; break;
    case 0x20: case 0x40: d = 1; break;
    default: break;
    }
    const bool keeps = event == 0x10 || event == 0x40;
    if (party) {
        if (breakGauge > 0) d = keeps ? 1 : 0;
    } else if (breakGauge < 0)
        d = keeps ? -1 : 0;
    else
        d = -d;
    breakTarget = std::clamp(breakTarget + d, -8, 8);
    if (breakGauge < 0 && breakTarget >= 0) { // the enemies lost it
        breakTarget = 0;
        breakFlags |= 0x400 | 0x800;
    } else if (breakGauge > 0 && breakTarget <= 0) { // the pair lost it
        breakTarget = 0;
        breakFlags = (breakFlags | 0x400) & ~0x800u;
    }
    breakFlags |= unsigned(event);
}

int BattleRules::bonusFor(const int actors[2], bool magic) const {
    const int first = actors[0];
    if (first < 2) {
        int b = breakGauge > 0 ? 75 : 0;
        if (pair) b += 25;
        else if (first == (magic ? 1 : 0)) b += 50;
        return b;
    }
    return breakGauge < 0 ? 125 : 50;
}

int BattleRules::elementScale(const BattleActionDef& a, int user, int target, int& type) const {
    if (!a.affinity) return 100;
    const int ea = f[size_t(user)].element, et = f[size_t(target)].element;
    if (ea == 0 || et == 0 || ea == et) return 100;
    if ((ea == 1 && et == 3) || (ea == 2 && et == 1) || (ea == 3 && et == 2)) {
        type = kHitWeak;
        return 150;
    }
    type = kHitResist;
    return 50;
}

// A monster's resistances (+0x10 physical / +0x12 genes, +0x14.. by the user's element). Returns
// the amount; `type` becomes kHitKill for instant death, kHitNoEffect when immune
int BattleRules::resist(const BattleActionDef&, int user, int target, int amount, bool magic, int& type) const {
    if (target < 2 || amount < 1) return amount;
    const Fighter& t = f[size_t(target)];
    switch (monsterField(t, magic ? 0x12 : 0x10)) {
    case 1: case 2: amount /= 2; break;
    case 3: amount = amount * 150 / 100; break;
    default: break;
    }
    const int e = f[size_t(user)].element;
    switch (monsterField(t, e <= 1 ? 0x14 : e == 2 ? 0x16 : 0x18)) {
    case 1: type = kHitNoEffect; return 0;
    case 2: amount /= 2; break;
    case 3: amount = amount * 150 / 100; break;
    case 4: type = kHitKill; return t.hp;
    default: break;
    }
    return amount;
}

BattleHit BattleRules::physical(const BattleActionDef& a, const int actors[2], int target) {
    BattleHit h;
    h.target = target;
    int atkSum = 0, spdSum = 0, levels = 0, n = 0;
    for (int k = 0; k < 2; ++k)
        if (actors[k] >= 0) {
            atkSum += atkOf(actors[k]);
            spdSum += spdOf(actors[k]);
            levels += f[size_t(actors[k])].level;
            ++n;
        }
    int bonus = bonusFor(actors, false);
    // skill 135: a plain attack is critical the first time, when every attacker has it
    bool firstCrit = a.id >= 10 && a.id <= 21;
    for (int k = 0; k < 2; ++k)
        if (actors[k] >= 0) firstCrit &= f[size_t(actors[k])].skills[135 - 131] && f[size_t(actors[k])].firstAttack;
    // btlcalc_critical: the attackers' average level against the target's
    // (against the action's first target's level, whichever target this is)
    const int dl = (n ? levels / n : 0) - f[size_t(firstTarget_ >= 0 ? firstTarget_ : target)].level;
    const int critChance = dl < -49 ? 10 : dl < 0 ? 15 : dl < 50 ? 20 : 50;
    const bool crit = random(256) < critChance || a.id == 0xdb || firstCrit;
    if (crit) {
        bonus += 50;
        h.type = kHitCritical;
    }
    const int grd = crit && has(actors[0], 137) ? 0 : grdOf(target); // 137: a critical pierces
    int dmg = (bonus + a.power) * atkSum / 100 - grd;
    if (crit && has(actors[0], 134)) dmg *= 2;
    if (!crit && dmg > 0) {
        const int d = spdSum - lckOf(target);
        int hit = d < -100 ? 50 : d < -49 ? 60 : d < 0 ? 70 : d < 50 ? 80 : d < 101 ? 90 : 99;
        if (a.alwaysHits) hit = 100;
        if (random(100) >= hit) {
            dmg = 0;
            h.type = kHitGuard;
        }
    }
    dmg = std::max(dmg, 0);
    if (h.type != kHitGuard) dmg = dmg * elementScale(a, actors[0], target, h.type) / 100;
    if (dmg >= 1) dmg = resist(a, actors[0], target, dmg, false, h.type);
    if (dmg >= 1 && h.type != kHitKill) dmg = zoku(dmg, a, actors[0], target, false);
    if (blocked(target, 131)) dmg = 0, h.type = kHitMiss;
    h.amount = dmg;
    return h;
}

BattleHit BattleRules::magical(const BattleActionDef& a, const int actors[2], int target) {
    BattleHit h;
    h.target = target;
    int atkSum = 0;
    for (int k = 0; k < 2; ++k)
        if (actors[k] >= 0) atkSum += atkOf(actors[k]);
    int dmg = (bonusFor(actors, true) + a.power) * atkSum / 100 - grdOf(target);
    if (has(actors[0], 156)) dmg += dmg / 5; // Boost Magic Power
    if (a.id == 122 || a.id == 224) { // poison (row +0x2e: immune)
        if (!monsterField(f[size_t(target)], 0x2e)) {
            f[size_t(target)].status[kStatusPoison] = 5;
            statusTaken = true;
            notice(target, 6);
        } else
            note_ = 57074;
    }
    if (a.element > 0) // the gene's element becomes the caster's
        for (int k = 0; k < 2; ++k)
            if (actors[k] >= 0) f[size_t(actors[k])].element = a.element, elementTaken = true;
    dmg = std::max(dmg, 0);
    dmg = dmg * elementScale(a, actors[0], target, h.type) / 100;
    if (dmg >= 1) dmg = resist(a, actors[0], target, dmg, true, h.type);
    if (dmg >= 1 && h.type != kHitKill) dmg = zoku(dmg, a, actors[0], target, true);
    if (blocked(target, 136)) dmg = 0, h.type = kHitMiss;
    h.amount = dmg;
    return h;
}

BattleHit BattleRules::heal(const BattleActionDef& a, const int actors[2], int target) {
    BattleHit h;
    h.target = target;
    int lckSum = 0;
    for (int k = 0; k < 2; ++k)
        if (actors[k] >= 0) lckSum += lckOf(actors[k]);
    int bonus;
    if (target < 2) {
        bonus = breakGauge > 0 ? 100 : 0;
        if (actors[0] == 1 || actors[1] == 1) bonus += 50;
    } else
        bonus = breakGauge < 0 ? 150 : 50;
    h.amount = -((bonus + a.power) * lckSum / 100);
    if (has(actors[0], 155)) h.amount *= 2; // Boost Recovery
    return h;
}

void BattleRules::enemyChoice(int enemy, bool asPair, Entry& e, const BattleActionTable& actions, int exclude) {
    // btlai_prc: the row's lists (5 actions each, +0x46 + 10 x list; the enemy pair's 50 on):
    // 0 always (weight 100), 1 when the enemies hold the Break / 2 otherwise (50), 3 at a
    // quarter of the HP or less (100), 4 every fifth round (200); "do nothing" actions 413..429
    // weigh 10 wherever they are
    const Fighter& x = f[size_t(enemy)];
    struct Choice { int action, weight; };
    std::vector<Choice> list;
    const int base = 0x46 + (asPair ? 50 : 0);
    auto add = [&](int listNo, int weight) {
        for (int k = 0; k < 5; ++k) {
            const int a = monsterField(x, base + listNo * 10 + 2 * k);
            if (a <= 0 || a == exclude) continue;
            if (x.status[kStatusSilence] && (a - 301u > 64u) && (a - 1u > 124u) && (a - 413u > 16u)) continue;
            list.push_back({a, a - 413u < 17u ? 10 : weight});
        }
    };
    add(0, 100);
    add(breakGauge < 0 ? 1 : 2, 50);
    if (x.hp <= x.hpMax / 4) add(3, 100);
    if (round % 5 == 0) add(4, 200);
    int total = 0;
    for (const Choice& c : list) total += c.weight;
    e.action = 0;
    int r = random(total);
    for (const Choice& c : list) {
        if (r < c.weight) {
            e.action = c.action;
            break;
        }
        r -= c.weight;
    }
    if (forceAction > 0 && enemy == 2) e.action = forceAction;
    const BattleActionDef* a = actions.get(e.action);
    if (!a) {
        e.kind = kCmdNone;
        return;
    }
    const int c = a->cls + 1; // the kind: attacks 8-10 (and -1), genes 11 / 13 / 14, items 12
    e.kind = c < 16 && ((1 << c) & 0xe01) ? kCmdAttack : c < 16 && ((1 << c) & 0xd000) ? kCmdGene
           : c == 13                     ? kCmdItem
                                         : kCmdGene;
    e.target = aiTarget(*a, enemy);
    if (!exclude) e.repeats = std::max(1, int(x.def ? x.def->raw[0x3e] : 1));
}

int BattleRules::aiTarget(const BattleActionDef& a, int user) {
    auto alive = [&](int i) { return f[size_t(i)].alive(); };
    std::vector<int> pool;
    auto pickFrom = [&](int from, int to) {
        pool.clear();
        for (int i = from; i < to; ++i)
            if (alive(i)) pool.push_back(i);
        return pool.empty() ? -1 : pool[size_t(random(int(pool.size())))];
    };
    auto byHp = [&](int from, int to, bool lowest) {
        int best = -1;
        for (int i = from; i < to; ++i)
            if (alive(i) && (best < 0 || (lowest ? f[size_t(i)].hp < f[size_t(best)].hp : f[size_t(i)].hp > f[size_t(best)].hp)))
                best = i;
        return best;
    };
    switch (a.side) {
    case 1: return user;
    case 2: // its own side (the enemies)
        if (a.pick == 4) return alive(2) ? 2 : pickFrom(2, 5);
        if (a.pick == 2) return byHp(2, 5, true);
        return pickFrom(2, 5);
    case 3: // the pair
        switch (a.pick) {
        case 2: return byHp(0, 2, true);
        case 3: return byHp(0, 2, false);
        case 4: return alive(0) ? 0 : pickFrom(0, 2);
        case 5: return alive(1) ? 1 : pickFrom(0, 2);
        default: return pickFrom(0, 2);
        }
    default: return pickFrom(0, 5);
    }
}

void BattleRules::beginRound(const BattleCommand cmd[2], const BattleActionTable& actions) {
    ++round;
    queue_.clear();
    next_ = 0;
    escaped_ = false;
    // state 500: what the Break must survive, defend takes hold now
    breakFlags = 0;
    if (breakGauge != 0) {
        breakFlags |= 0x8000;
        if (breakGauge > 0) breakFlags |= 0x4000;
    }
    const bool partySkips = round == 1 && strike == 2, enemiesSkip = round == 1 && strike == 1;
    int order = 0;
    if (!partySkips)
        for (int k = 0; k < (pair ? 1 : 2); ++k) {
            const BattleCommand& c = cmd[k];
            if (!pair && !f[size_t(k)].alive()) continue;
            if (k == 1 && allyAuto) continue; // the guest chooses for itself below
            Entry e;
            e.kind = c.kind;
            e.order = order++;
            e.actors[0] = pair ? 0 : k;
            e.actors[1] = pair ? 1 : -1;
            e.target = c.target;
            switch (c.kind) {
            case kCmdAttack: e.action = attackAction(k); break;
            case kCmdGene: e.action = c.action; break;
            case kCmdItem: e.item = c.item; e.action = c.item - kFirstBattleItem + kItemAction; break;
            case kCmdDefend:
                for (int a : e.actors)
                    if (a >= 0) f[size_t(a)].guarding = true;
                continue; // done (btl_main 500)
            case kCmdEscape: e.action = 0; break;
            default: continue;
            }
            queue_.push_back(e);
        }
    // btlord_enemy_rand: the guest's rotation (370: attack, attack, Cremate; 372 / 373: Möwe,
    // attack, Asura Lotus), on a random enemy
    if (allyAuto && f[1].alive() && !partySkips) {
        const int c = allyCounter;
        allyCounter = (allyCounter + 1) % 3;
        Entry e;
        e.order = order++;
        e.actors[0] = 1;
        if (allyScript == 1) e.action = c == 0 ? 212 : 14;
        else e.action = c == 0 ? 214 : c == 1 ? 14 : 207;
        e.kind = e.action == 14 ? kCmdAttack : kCmdGene;
        std::vector<int> foes;
        for (int i = 2; i < 5; ++i) if (f[size_t(i)].alive()) foes.push_back(i);
        e.target = foes.empty() ? 2 : foes[size_t(random(int(foes.size())))];
        queue_.push_back(e);
    }
    // btlord_enemy_rand: with two enemies standing (the two-enemy formations) a third of the
    // rounds they act together
    enemyPair = false;
    if (!enemiesSkip) {
        if (f[2].alive() && f[3].alive() && !f[4].present && random(3) == 1) {
            enemyPair = true;
            Entry e;
            e.order = order++;
            e.actors[0] = 2;
            e.actors[1] = 3;
            enemyChoice(2, true, e, actions);
            if (e.kind != kCmdNone) queue_.push_back(e);
        } else
            for (int i = 2; i < 5; ++i) {
                if (!f[size_t(i)].alive()) continue;
                Entry e;
                e.order = order++;
                e.actors[0] = i;
                enemyChoice(i, false, e, actions);
                if (e.kind != kCmdNone) queue_.push_back(e);
            }
    }
    // btlord_calc_spd: the actors' average speed (+19998 acting first) and the action's
    for (Entry& e : queue_) {
        int sum = 0, n = 0;
        for (int a : e.actors)
            if (a >= 0) {
                sum += spdOf(a) + (f[size_t(a)].status[kStatusInvincible] ? 19998 : 0);
                ++n;
            }
        e.speed = (n ? sum / n : 0);
        if (const BattleActionDef* d = actions.get(e.action)) e.speed += d->speed;
    }
    for (Entry& e : queue_) // Asura Lotus strikes 2..5 times (2..3 in pair mode)
        if (e.action == 207) e.repeats = random(e.actors[0] < 2 ? (pair ? 2 : 4) : (enemyPair ? 2 : 4)) + 2;
    for (const Entry& e : queue_) // btlque_make_que +0xa4a: the party member queued last
        if (e.actors[0] >= 0 && e.actors[0] < 2) soloMember_ = e.actors[0];
    std::stable_sort(queue_.begin(), queue_.end(), [](const Entry& a, const Entry& b) { return a.speed > b.speed; });
}

std::vector<int> BattleRules::resolveTargets(const BattleActionDef& a, const Entry& e) {
    const int user = e.actors[0];
    std::vector<int> pool = targets(a, user);
    if (pool.empty()) return pool;
    std::vector<int> out;
    if (isGroupScope(a) || a.scope == 0) out = pool;
    else {
        int t = e.target;
        if (std::find(pool.begin(), pool.end(), t) == pool.end()) // fell meanwhile: someone else there
            t = pool[size_t(random(int(pool.size())))];
        out.push_back(t);
    }
    // btlque_make_que: in pair mode a single target on the pair's side is both
    if (pair && out.size() == 1 && out[0] < 2 && a.scope != 1) {
        out.clear();
        for (int i = 0; i < 2; ++i)
            if (f[size_t(i)].alive()) out.push_back(i);
    }
    return out;
}

bool BattleRules::nextTurn(const BattleActionTable& actions, BattleTurn& out) {
    while (next_ < queue_.size()) {
        if (outcome() != 0) return false;
        Entry e = queue_[next_++];
        if (e.repeats > 1 && outcome() == 0) { // btlque_make_que: it acts again straight after
            Entry again = e;
            --again.repeats;
            again.paid = true;
            if (e.actors[0] >= 2 && e.action != 207) { // a twice-acting monster chooses afresh
                const int was = e.action;
                enemyChoice(e.actors[0], e.actors[1] >= 0, again, actions, was);
                again.repeats = e.repeats - 1;
            }
            if (again.kind != kCmdNone) queue_.insert(queue_.begin() + std::ptrdiff_t(next_), again);
        }
        for (int& a : e.actors) // the fallen don't act
            if (a >= 0 && !f[size_t(a)].alive()) a = -1;
        if (e.actors[0] < 0) std::swap(e.actors[0], e.actors[1]);
        if (e.actors[0] < 0 || e.done) continue;
        out = BattleTurn();
        notices_.clear();
        note_ = 0;
        out.kind = e.kind;
        out.action = e.action;
        out.actors[0] = e.actors[0];
        out.actors[1] = e.actors[1];
        out.counter = e.counter;
        breakFlags &= ~0x3200u; // btl_main 600: per action
        if (e.kind == kCmdEscape) {
            // btlact_escape_init: certain while the pair holds the Break, else by levels
            int chance = 100;
            if (breakGauge < 1 && !has(0, 160) && !has(1, 160)) { // 160: Beast Wavelength
                int pl = 0, pn = 0, el = 0, en = 0;
                for (int i = 0; i < 2; ++i)
                    if (f[size_t(i)].alive()) pl += f[size_t(i)].level, ++pn;
                for (int i = 2; i < 5; ++i)
                    if (f[size_t(i)].alive()) el += f[size_t(i)].level, ++en;
                const int d = (pn ? pl / pn : 0) - (en ? el / en : 0);
                chance = d < -49 ? 40 : d < 0 ? 50 : d < 50 ? 60 : 70;
            }
            out.escapeChance = chance;
            out.escaped = random(100) < chance;
            escaped_ = out.escaped;
            return true;
        }
        const BattleActionDef* a = actions.get(e.action);
        if (!a) continue;
        if (e.kind == kCmdItem) out.item = e.item;
        if (e.kind == kCmdGene && e.actors[0] < 2 && !e.paid) { // the gene's MP: each member pays it in pair mode
            const int cost = geneCost(*a, e.actors[0]);
            bool enough = true;
            for (int k : e.actors)
                if (k >= 0) enough &= f[size_t(k)].mp >= cost;
            if (!enough) continue;
            for (int k : e.actors)
                if (k >= 0) f[size_t(k)].mp -= cost;
        }
        int counterBy = -1;
        const std::vector<int> hitList = resolveTargets(*a, e);
        firstTarget_ = hitList.empty() ? -1 : hitList.front(); // btlcalc_critical reads G+0x7a
        int drained = 0;
        auto atkSumOf = [&]() { int s = 0; for (int k : e.actors) if (k >= 0) s += atkOf(k); return s; };
        auto magicBoost = [&](int v) { return has(e.actors[0], 156) ? v + v / 5 : v; };
        auto recovery = [&](int v) { return has(e.actors[0], 155) ? v * 2 : v; };
        for (int t : hitList) {
            BattleHit h;
            h.target = t;
            int calc = a->calc;
            Fighter& tx = f[size_t(t)];
            if (a->hostile && calc != 12 && tx.status[kStatusBarrier] && e.kind == kCmdGene) { // the barrier takes it
                tx.status[kStatusBarrier] = 0;
                h.type = kHitNoEffect;
                out.message = 57062;
                out.hits.push_back(h);
                continue;
            }
            switch (calc) {
            case 2: h.amount = a->power * 100 / std::max<int>(1, int(hitList.size())); break; // btlcalc_0407: shared
            case 3: h.amount = a->power * 100; break;                                           // 0408: fixed
            case 4: case 6: { // 0409 / 0411: MP lost (power x 10 / power% of it)
                h.mp = true;
                h.amount = calc == 4 ? a->power * 10 : a->power * tx.mp / 100;
                if (tx.status[kStatusInvincible]) h.amount = 0;
                tx.mp = std::clamp(tx.mp - h.amount, 0, tx.mpMax);
                out.hits.push_back(h);
                continue;
            }
            case 5: case 7: case 8: { // 0410 power% of its HP / 0412 of the users' HP / 0413 of their attack
                int base = tx.hp;
                if (calc == 7) { base = 0; for (int k : e.actors) if (k >= 0) base += std::max(0, f[size_t(k)].hp - 1); }
                if (calc == 8) base = atkSumOf();
                h.amount = magicBoost(zoku(a->power * base / 100, *a, e.actors[0], t, true));
                if (blocked(t, 136)) h.amount = 0, h.type = kHitMiss;
                if (calc == 8) drained += h.amount;
                break;
            }
            case 9: h.amount = std::max(0, tx.hp - 1); break; // 0414: down to 1
            case 10: { // 0415: the purse loses power% (at least 1)
                int take = purse > 0 ? std::max(1, a->power * purse / 100) : 0;
                take = std::min(take, purse);
                purse -= take;
                ozStolen += take;
                f[size_t(e.actors[0])].stolen += take;
                out.message = take > 0 ? 57039 : 57038;
                out.messageValue = take;
                h.type = kHitQuiet;
                out.hits.push_back(h);
                continue;
            }
            case 11: { // 0416: the monster runs (sure while the enemies hold the Break)
                int chance = 100;
                if (breakGauge >= 0) {
                    int pl = 0, pn = 0;
                    for (int i = 0; i < 2; ++i) if (f[size_t(i)].alive()) pl += f[size_t(i)].level, ++pn;
                    const int d = tx.level - (pn ? pl / pn : 0);
                    chance = d < -49 ? 50 : d < 0 ? 70 : d < 50 ? 85 : 100;
                }
                if (random(100) < chance) tx.gone = out.gone = true;
                out.message = out.gone ? 57000 : 57001;
                h.type = kHitQuiet;
                out.hits.push_back(h);
                continue;
            }
            case 13: { // 0418: MP back by the luck formula
                int lck = 0;
                for (int k : e.actors) if (k >= 0) lck += lckOf(k);
                int bonus = t < 2 ? (breakGauge > 0 ? 20 : 0) + (e.actors[0] == 1 || e.actors[1] == 1 ? 5 : 0)
                                  : (breakGauge < 0 ? 25 : 5);
                int v = recovery((bonus + a->power) * lck / 100);
                if (a->variance > 0) v = (100 - a->variance + random(2 * a->variance + 1)) * v / 100;
                h.mp = true;
                h.amount = -v;
                tx.mp = std::clamp(tx.mp + v, 0, tx.mpMax);
                out.hits.push_back(h);
                continue;
            }
            case 14: // 0419: everything back
                tx.mp = tx.mpMax;
                h.amount = -tx.hpMax;
                break;
            case 15: h.amount = -recovery(tx.hpMax * a->power / 100); break; // 0420: power% of the most HP
            case 16: { // 0421: power% of the most MP
                const int v = recovery(tx.mpMax * a->power / 100);
                h.mp = true;
                h.amount = -v;
                tx.mp = std::clamp(tx.mp + v, 0, tx.mpMax);
                out.hits.push_back(h);
                continue;
            }
            case 19: // 0424: back on its feet with half (all, holding the Break) its HP
                if (tx.hp > 0) { h.type = kHitQuiet; out.hits.push_back(h); continue; }
                h.amount = -std::min(tx.hpMax, recovery(tx.hpMax * (holdsBreak(t) ? 100 : 50) / 100));
                h.revived = true;
                break;
            case 22: case 23: // 042700 poison cured / 042701 every status (and the element) gone
                if (calc == 22) tx.status[kStatusPoison] = 0;
                else { for (int& s : tx.status) s = 0; tx.element = 0; }
                h.type = kHitQuiet;
                out.hits.push_back(h);
                continue;
            case 24: { // Analyze (0428 itself does nothing): the target's weak spot, from its resistances
                out.message = 57045;
                if (tx.def)
                    for (int k = 0; k < 5; ++k)
                        if (monsterField(tx, 0x10 + 2 * k) == 3) { out.message = 57032; out.messageValue = 57040 + k; break; }
                h.type = kHitQuiet;
                out.hits.push_back(h);
                continue;
            }
            case 25: // zokusei_chr1: the target takes the action's element
                if (a->element > 0) tx.element = a->element, elementTaken = true, notice(t, 0x32 + a->element);
                h.type = kHitQuiet;
                out.hits.push_back(h);
                continue;
            case 34: tx.counterStance = true; h.type = kHitQuiet; out.hits.push_back(h); continue; // 030218
            case 35: case 36: // 0601 / 0602: this battle's experience / Oz doubled
                (calc == 35 ? doubleExp : doubleOz) = true;
                out.message = calc == 35 ? 57023 : 57024;
                h.type = kHitQuiet;
                out.hits.push_back(h);
                continue;
            case 0: case 1: case 12: {
                h = calc == 0 ? physical(*a, e.actors, t) : calc == 1 ? magical(*a, e.actors, t) : heal(*a, e.actors, t);
                if (calc == 0 && a->id == 203 && h.amount > 0) { // btlcalc_0404: Risk Breaker
                    h.amount = h.amount * (holdsBreak(e.actors[0]) ? 200 : 125) / 100;
                    if (t == hitList.front()) // the users give a quarter of their HP first
                        for (int k : e.actors) {
                            if (k < 0 || !f[size_t(k)].alive()) continue;
                            Fighter& u = f[size_t(k)];
                            BattleHit self;
                            self.target = k;
                            self.amount = u.hp / 4;
                            if (keepAlive && u.hp - self.amount < 1) self.amount = std::max(0, u.hp - 1);
                            if (self.amount > 0) breakEvent(e.actors[0], 2, t);
                            u.hp = std::max(0, u.hp - self.amount);
                            if (u.hp == 0 && u.status[kStatusEndure]) u.status[kStatusEndure] = 0, u.hp = 1;
                            if (u.hp == 0) { // btlcalc_chk_dead
                                self.fell = true;
                                for (int& st : u.status) st = 0;
                                u.guarding = false;
                                if (k < 2) pair = false;
                            }
                            out.hits.push_back(self);
                        }
                }
                if (a->variance > 0 && h.amount != 0) h.amount = (100 - a->variance + random(2 * a->variance + 1)) * h.amount / 100;
                break;
            }
            case 17: h.amount = -a->power * (has(e.actors[0], 155) ? 20 : 10); break; // btlcalc_0422: HP
            case 18: { // btlcalc_0423: MP
                Fighter& x = f[size_t(t)];
                h.mp = true;
                h.amount = -a->power * (has(e.actors[0], 155) ? 20 : 10);
                x.mp = std::clamp(x.mp - h.amount, 0, x.mpMax);
                out.hits.push_back(h);
                continue;
            }
            case 26: case 27: case 28: case 29: case 30: case 31: case 33: // btlcalc_0302xx: statuses
                h.type = kHitQuiet;
                if (calc == 31 && monsterField(tx, 0x30)) { // 030210: row +0x30 can't be silenced
                    note_ = 57075;
                    out.hits.push_back(h);
                    continue;
                }
                h.status = calc == 26 ? kStatusBarrier : calc == 31 ? kStatusSilence : calc == 33 ? kStatusInvincible
                                                                                   : kStatusAttackUp + (calc - 27);
                f[size_t(t)].status[h.status] = calc == 33 ? 2 : 5;
                statusTaken = true;
                notice(t, calc == 26 ? 0x1d : calc == 31 ? 4 : calc == 33 ? 0x24 : 0x19 + (calc - 27));
                out.hits.push_back(h);
                continue;
            case 32:
                h.status = kStatusEndure;
                f[size_t(t)].status[kStatusEndure] = 1;
                statusTaken = true;
                notice(t, 0x1e);
                h.type = kHitQuiet;
                out.hits.push_back(h);
                continue;
            case -1: case 20: case 21: h.type = kHitQuiet; out.hits.push_back(h); continue;
            default: {
                const std::string why = "calc " + std::to_string(calc);
                if (std::find(unsupported.begin(), unsupported.end(), why) == unsupported.end()) unsupported.push_back(why);
                h.type = kHitQuiet;
                out.hits.push_back(h);
                continue;
            }
            }
            Fighter& tf = f[size_t(t)];
            const int before = tf.hp;
            if (h.type == kHitKill) h.amount = tf.hp;
            if (h.amount > 0 && tf.status[kStatusInvincible]) h.amount = 0, h.type = kHitNoEffect; // Is_invincible
            if (keepAlive && h.amount > 0 && tf.hp - h.amount < 1) h.amount = std::max(0, tf.hp - 1); // chk_karaok_dmg_hp
            if (calc == 0 && round > 3 && h.amount >= 1 && a->id == 18) lateAttack = true; // btlcalc_0404: +0xa50
            if (h.amount > 0) breakEvent(e.actors[0], 2, t);
            tf.hp = std::clamp(tf.hp - h.amount, 0, tf.hpMax);
            if (tf.hp == 0 && before > 0 && tf.status[kStatusEndure]) {
                tf.status[kStatusEndure] = 0;
                tf.hp = 1;
            }
            if (h.type == kHitCritical) breakEvent(e.actors[0], 8, t);
            if (before > 0 && tf.hp == 0) { // btlcalc_chk_dead
                h.fell = true;
                for (int& s : tf.status) s = 0;
                tf.guarding = false;
                if (tf.enemy && !tf.rewarded) {
                    tf.rewarded = true;
                    // chk_dead: pair mode either member's skill, else the member +0xa4a names
                    auto member = [&](int skill) { return pair ? has(0, skill) || has(1, skill) : has(soloMember_, skill); };
                    const bool expUp = member(161), ozUp = member(162);
                    expGained += tf.exp + (expUp ? tf.exp / 5 : 0);
                    ozGained += tf.oz + (ozUp ? tf.oz / 5 : 0) + tf.stolen; // what it stole comes back
                    tf.stolen = 0;
                    fallRewards(t, e.actors);
                }
                if (t < 2) pair = false; // a fallen member ends pair mode
                breakEvent(e.actors[0], 0x10, t);
            }
            if (h.amount > 0 && tf.hp > 0 && (t < 2) != (e.actors[0] < 2)) {
                // 153: at a quarter of the HP or less the target may start guarding
                if (tf.hp <= tf.hpMax / 4 && !tf.guarding && has(t, 153) && random(100) < (holdsBreak(t) ? 20 : 10)) {
                    tf.guarding = true;
                    if (t < 2 && pair) f[size_t(1 - t)].guarding = true;
                }
                // 154 (or calc 34's stance): a physical hit may be answered with the target's attack
                if (calc == 0 && counterBy < 0 && !e.counter &&
                    (tf.counterStance || (has(t, 154) && random(100) < (holdsBreak(t) ? 20 : 10))))
                    counterBy = t;
            }
            if (calc == 0 && a->id == 201 && h.amount >= 1) { // btlcalc_0404: Trompe-L'oeil
                const bool held = holdsBreak(e.actors[0]);
                if (random(8) == 0 && held && !cancelTried_) {
                    cancelTried_ = true;
                    if (!monsterField(tx, 0x32)) // the targets' commands this round are cancelled
                        for (int c : hitList) {
                            for (size_t q = next_; q < queue_.size(); ++q)
                                if (queue_[q].actors[0] == c || queue_[q].actors[1] == c) queue_[q].done = true;
                            notice(c, 3);
                        }
                }
                if (!held && !cancelBreak_) cancelBreak_ = true, breakEvent(e.actors[0], 0x20, t);
            }
            out.hits.push_back(h);
        }
        if (a->id >= 10 && a->id <= 21)
            for (int k : e.actors)
                if (k >= 0) f[size_t(k)].firstAttack = false;
        if (a->id == 223) { // btlcalc_0504: Steal Item, when the blow landed
            int dealt = 0, mine = 0, n = 0, theirs = 0, m = 0;
            bool anyLeft = false;
            for (const BattleHit& h : out.hits) dealt += std::max(0, h.amount);
            for (int k : e.actors) if (k >= 0) mine += lckOf(k), ++n;
            for (int t : hitList) { theirs += lckOf(t), ++m; anyLeft |= !f[size_t(t)].robbed; }
            const int d = (n ? mine / n : 0) + 20 - (m ? theirs / m : 0) + a->power;
            const int chance = d < -100 ? 40 : d < -49 ? 50 : d < 0 ? 60 : d < 50 ? 70 : d < 101 ? 80 : 90;
            const bool roll = random(100) < chance;
            if (!anyLeft) out.message = 57037; // nothing to steal
            else if (dealt > 0 && roll) {
                out.message = 57038;
                for (int t : hitList) {
                    Fighter& x = f[size_t(t)];
                    if (x.robbed || !x.def) continue;
                    int item;
                    if (x.level >= 40 && random(3) == 1) item = monsterField(x, 0xca);
                    else item = x.level >= 20 && random(2) ? monsterField(x, 0xc8) : monsterField(x, 0xc6);
                    if (item <= 0) continue;
                    x.robbed = true;
                    stolen.push_back(item);
                    out.message = 0; // the catch is queued instead, one message an item
                    notice(-item, 5);
                }
            } else
                out.message = 57038; // failed
        }
        if (a->calc == 7) // 0412: the users give up their HP to 1
            for (int k : e.actors)
                if (k >= 0 && f[size_t(k)].alive()) {
                    BattleHit self;
                    self.target = k;
                    self.amount = f[size_t(k)].hp - 1;
                    f[size_t(k)].hp = 1;
                    out.hits.push_back(self);
                }
        if (a->calc == 8 && drained > 0 && !hitList.empty()) // 0413: what was drained heals the users
            for (int k : e.actors)
                if (k >= 0 && f[size_t(k)].alive()) {
                    BattleHit back;
                    back.target = k;
                    back.amount = -recovery(drained) / int(hitList.size());
                    f[size_t(k)].hp = std::clamp(f[size_t(k)].hp - back.amount, 0, f[size_t(k)].hpMax);
                    out.hits.push_back(back);
                }
        if (counterBy >= 0 && e.actors[0] >= 0) { // btlque_add_counter: next in the queue
            Entry c;
            c.counter = true;
            c.kind = kCmdAttack;
            c.actors[0] = counterBy;
            c.target = e.actors[0];
            if (counterBy < 2) c.action = attackAction(counterBy);
            else { // a monster answers with its first plain attack
                c.action = 0;
                for (int k = 0; k < 5 && !c.action; ++k)
                    if (const BattleActionDef* d = actions.get(monsterField(f[size_t(counterBy)], 0x46 + 2 * k)); d && d->calc == 0)
                        c.action = d->id;
                if (!c.action) c.action = 10;
            }
            queue_.insert(queue_.begin() + std::ptrdiff_t(next_), c);
        }
        catchUpBreak();
        firstTarget_ = -1;
        out.notices = notices_;
        if (!out.message) out.message = note_;
        return true;
    }
    return false;
}

void BattleRules::notice(int fighter, int entry) {
    for (const BattleNotice& n : notices_)
        if (n.fighter == fighter) return;
    if (notices_.size() < 5) notices_.push_back({fighter, entry}); // G+0xa14: five places
}

void BattleRules::endRound() {
    // btlord_chk: the holder wasn't hit all round
    if (outcome() == 0 && !(breakFlags & 0x400) && (breakFlags & 0x8000)) breakEvent(breakGauge < 1 ? 2 : 0, 0x40, -1);
    catchUpBreak();
    for (int i = 0; i < 5; ++i) { // btlcalc_status_prc: the regen status (10%) and skills 133 / 138 (5%)
        Fighter& x = f[size_t(i)];
        if (!x.alive()) continue;
        int hp = has(i, 133) ? x.hpMax / 20 : 0;
        x.hp = std::min(x.hpMax, x.hp + hp);
        if (x.status[kStatusPoison] > 0) // poison: 10% a round (through btlcalc_dmg_hp)
            x.hp = keepAlive ? std::max(std::min(x.hp, 1), x.hp - x.hpMax / 10) : std::max(0, x.hp - x.hpMax / 10);
        if (regenFull[size_t(i)]) x.hp = x.hpMax; // a story boss heals whole until its moment comes
        x.counterStance = false;
        if (has(i, 138)) x.mp = std::min(x.mpMax, x.mp + x.mpMax / 20);
    }
    for (Fighter& x : f) {
        x.guarding = false;
        for (int k = 0; k < kStatusCount; ++k)
            if (k != kStatusEndure && x.status[k] > 0) --x.status[k];
    }
}

bool BattleRules::knows(int item) const {
    return std::find(known_.begin(), known_.end(), item) != known_.end() ||
           std::find(learned.begin(), learned.end(), item) != learned.end() ||
           std::find(drops.begin(), drops.end(), item) != drops.end();
}

// btlcalc_0505 / 0506 / chk_dead: what a fallen monster leaves
void BattleRules::fallRewards(int enemy, const int actors[2]) {
    const Fighter& x = f[size_t(enemy)];
    if (!x.def) return;
    int sum = 0, n = 0;
    for (int k = 0; k < 2; ++k)
        if (actors[k] >= 0) sum += f[size_t(actors[k])].level, ++n;
    const int avg = n ? sum / n : 1, lv = x.level;
    auto row = [&](int off) { return monsterField(x, off); };
    if (row(0xc0) + row(0xc2) + row(0xc4) != 0) { // btlcalc_0505: an item - the roll, then which
        int d = avg + 10 - lv;
        const int chance = d < -4 ? 20 : d < 1 ? 30 : d < 6 ? 40 : 50;
        if (random(100) < chance) {
            int item;
            if (lv >= 40 && random(3) == 1) item = row(0xc4);
            else item = lv >= 20 && random(2) ? row(0xc2) : row(0xc0);
            if (item > 0 && !((item >= 81 && item <= 186) && knows(item))) drops.push_back(item);
        }
    }
    if (row(0xb4) != 0) { // a gene
        bool more = false; // 158: Get More Genes
        for (int k = 0; k < 2; ++k) more |= actors[k] >= 0 && has(actors[k], 158);
        const int d = avg - (more ? 15 : 0) - lv;
        const int chance = d < -4 ? 80 : d < 1 ? 40 : d < 6 ? 30 : 10;
        if (random(100) < chance) { // btlcalc_0506: the first not known yet of up to six (+0xb4..+0xbe:
            // its plate's abilities and skills, the later ones from levels 5 / 10 / 15 / 20 / 25)
            static const int kFrom[6] = {0, 5, 10, 15, 20, 25};
            for (int k = 0; k < 6; ++k) {
                const int g = row(0xb4 + 2 * k);
                if (lv >= kFrom[k] && g > 0 && !knows(g) &&
                    std::find(learned.begin(), learned.end(), g) == learned.end()) {
                    learned.push_back(g), learnedNew = true;
                    break;
                }
            }
        }
    }
    const int plate = plateDrop(x.def->model); // chk_dead: 481 + (model - 13) / 3
    if (plate > 0 && !knows(plate)) drops.push_back(plate);
}

int BattleRules::outcome() const {
    if (escaped_) return 1;
    if (!f[0].alive() && !f[1].alive()) return 3;
    bool any = false;
    for (int i = 2; i < 5; ++i) any |= f[size_t(i)].alive();
    return any ? 0 : 2;
}

void BattleRules::defeatEnemies() {
    const int actors[2] = {0, f[1].present ? 1 : -1};
    for (int t = 2; t < 5; ++t) {
        Fighter& tf = f[size_t(t)];
        if (!tf.present || tf.hp <= 0) continue;
        tf.hp = 0;
        for (int& s : tf.status) s = 0;
        tf.guarding = false;
        if (tf.enemy && !tf.rewarded) {
            tf.rewarded = true;
            expGained += tf.exp;
            ozGained += tf.oz + tf.stolen;
            tf.stolen = 0;
            fallRewards(t, actors);
        }
    }
}

std::vector<int> BattleRules::applyRewards(GameState& gs) const {
    std::vector<int> levelled;
    const bool won = outcome() == 2;
    for (int k = 0; k < 2; ++k) {
        const Fighter& x = f[size_t(k)];
        if (!x.present || x.guest) continue;
        PartyMember& m = gs.party[size_t(x.character)];
        m.mp = x.skills[163 - 131] && outcome() != 3 ? -1 : x.mp; // 163: MP back to full
        if (won) {
            const int gained = expGained * (doubleExp ? 2 : 1);
            m.exp += x.hp > 0 ? gained : gained / 2; // MbResultSet: a fallen member gets half
            const int before = m.level;
            while (m.level < kMaxLevel && m.exp >= expForLevel(m.level + 1)) ++m.level;
            if (m.level != before) {
                levelled.push_back(x.character);
                levelUpBoosts(gs, x.character, m.level - before);
            }
        }
    }
    if (ozStolen > 0) addOz(gs, -ozStolen);
    if (outcome() != 3)
        for (int item : stolen) gs.items[item] = std::min(kMaxItemCount, gs.items[item] + 1);
    if (won) {
        addOz(gs, ozGained * (doubleOz ? 2 : 1));
        for (int item : drops) gs.items[item] = std::min(kMaxItemCount, gs.items[item] + 1);
        for (int item : learned) gs.items[item] = std::max(1, gs.items[item]);
    }
    return levelled;
}

} // namespace cr
