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
// actor +0x24's row for anyone: a monster's, or a party member's character row (btlcalc_zoku_0204's
// herbivore / carnivore reads, 23447-23460: every character row has +0x36 = +0x38 = 1)
int rowField(const Fighter& f, int offset) {
    const MonsterDef* d = f.def ? f.def : f.row;
    return d ? s16(&d->raw[size_t(offset)]) : 0;
}


} // namespace

// Analyze's lists (2.2.0 FUN_0003f138 / FUN_0003f2b4): the elements whose resistance entry (+0x10 + 2k) is
// 3 - weak - or 1 / 2 - resisted - in element order (Omega's weak list also takes 4; CR1's doesn't)
std::vector<int> analyzeElements(const Fighter& f, bool weak) {
    std::vector<int> out;
    for (int k = 0; k < 5; ++k)
        if (const int v = monsterField(f, 0x10 + 2 * k); weak ? v == 3 : v == 1 || v == 2) out.push_back(k);
    return out;
}

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
        a.sureGene = r[20] == 1;
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
    bag_.assign(47, 0);
    purse = gs.oz;
    for (const auto& [item, count] : gs.items) {
        if (count > 0 && ((item >= 81 && item <= 186) || (item >= 479 && item <= 500))) known_.push_back(item);
        if (item >= 21 && item <= 46) bag_[size_t(item)] = std::max(count, 0);
    }
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
        x.mp = m.mp < 0 ? x.mpMax : m.mp; // (btlpcparam_prty_init 29837-29890: the stored MP as it is, unclamped; -1 an old save's "full")
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
        x.mp = m.mp < 0 ? x.mpMax : m.mp; // (btlpcparam_prty_init 29837-29890: the stored MP as it is, unclamped; -1 an old save's "full")
        x.atk = memberStat(gs, c, kAttack, items);
        x.grd = memberStat(gs, c, kDefense, items);
        x.spd = memberStat(gs, c, kSpeed, items);
        x.lck = memberStat(gs, c, kLuck, items);
        for (int s = 131; s <= 186; ++s) x.skills[size_t(s - 131)] = hasSkill(gs, c, s);
        x.abilities = usableAbilities(gs, c);
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
            x.saveLevel = gs.party[size_t(g.character)].level; // (btlcalc_0505 / 0506 read this, not the battle's)
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
    for (int k = 0; k < 2; ++k) // (the pair's rows: btl_prty_init 5647 / 5666, a guest's too)
        if (f[size_t(k)].present && f[size_t(k)].character >= 0) f[size_t(k)].row = data.monster(f[size_t(k)].character * 10 + 1);
    const int L = leaderLevel();
    const int hi = L > 98 ? 100 : L + 2, lo = L < 6 ? 1 : L - 5;
    band = std::clamp(band, -1, 9); // (-1: a Back out of World Select's band list before the last gate - top 0)
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
        x.level = std::clamp(lv, 0, 100); // (_btl_enemy_init_lv: band -1's top, 0, kept)
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
    // (chk_skill_btl 60306-60360: the pair's either member; with the enemy pair formed, enemies 2 and 3 alike)
    if (i < 2) return pair && f[size_t(1 - i)].skills[size_t(skill - 131)];
    return enemyPair && (i == 2 || i == 3) && f[size_t(5 - i)].skills[size_t(skill - 131)];
}

int BattleRules::geneCost(const BattleActionDef& a, int user) const {
    return user < 2 && has(user, 132) ? a.mp / 2 : a.mp;
}

// btlcalc_zoku_0204: the skills' scaling of a hit
int BattleRules::zoku(int amount, const BattleActionDef& a, int user, int target, bool magic) const {
    int pct = has(target, magic ? 147 : 146) ? 75 : 100;
    if (has(user, magic ? 140 : 139)) pct += 25;
    const int e = f[size_t(user)].element;
    if (a.affinity)
        for (int k = 1; k <= 3; ++k)
            if (e == k && has(target, 147 + k)) pct -= 25; // Blaze / Aqua / Gale Resistance
    if (has(target, 151) && rowField(f[size_t(user)], 0x38)) pct -= 25; // a herbivore's attack
    if (has(target, 152) && rowField(f[size_t(user)], 0x36)) pct -= 25; // a carnivore's
    if (a.affinity)
        for (int k = 1; k <= 3; ++k)
            if (e == k && has(user, 140 + k)) pct += 25; // Blaze / Aqua / Gale Attack
    if (has(user, 144) && rowField(f[size_t(target)], 0x38)) pct += 25;
    if (has(user, 145) && rowField(f[size_t(target)], 0x36)) pct += 25;
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

// btlcalc_0404 / 0406: a monster's resistances - +0x10 physical / +0x12 genes, then +0x14 / 16 /
// 18 by the user's element (none without one): 1 immune (0, the hit's type as it was), 2 half, 3
// x1.5, 4 instant death (`type` kHitKill, the amount its HP)
int BattleRules::resist(const BattleActionDef&, int user, int target, int amount, bool magic, int& type) const {
    if (target < 2) return amount;
    const Fighter& t = f[size_t(target)];
    const int e = f[size_t(user)].element;
    for (int off : {magic ? 0x12 : 0x10, e >= 1 && e <= 3 ? 0x12 + 2 * e : -1}) {
        if (off < 0) break;
        switch (monsterField(t, off)) {
        case 1: return 0;
        case 2: amount /= 2; break;
        case 3: amount = amount * 150 / 100; break;
        case 4: type = kHitKill; return t.hp;
        default: break;
        }
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
    // skill 135: a plain attack is critical the first time, when every attacker has it - chk_skill_btl's,
    // either member's in pair mode (___btlcalc_0404 23858-23866) - and none has used its first
    bool firstCrit = a.id >= 10 && a.id <= 21;
    for (int k = 0; k < 2; ++k)
        if (actors[k] >= 0) firstCrit &= has(actors[k], 135) && f[size_t(actors[k])].firstAttack;
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
    if (dodgeHit_) dmg = 0, h.type = kHitMiss, note_ = 57061; // "Neutralized physical damage!"
    // ___btlcalc_0404: que +0xbf marks the critical (cleared only on the Miss path) - _btlcalc_0404
    // fires the Break's event 8 on it, whatever the triangle or Invincible made of the hit
    h.critical = crit && !dodgeHit_;
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
    dmg = resist(a, actors[0], target, dmg, true, h.type); // (0406: even at 0)
    if (dmg >= 1 && h.type != kHitKill) dmg = zoku(dmg, a, actors[0], target, true);
    if (dodgeGene_) dmg = 0, h.type = kHitMiss, note_ = 57062; // "Neutralized magical damage!"
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
    // (btlord_enemy_rand fills every enemy's target with a random party member first, alive or
    // not; a pick btlai_prc doesn't handle leaves that, and btlque_make_que retargets the fallen)
    switch (a.side) {
    case 1: return user;
    case 2: // its own side (the enemies)
        if (a.pick == 4) return 2; // (unconditionally: fallen, make_que gives it to the actors)
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
    default: return a.pick == 1 ? pickFrom(0, 5) : random(2); // (side 4 pick 2, side 255: the prefill)
    }
}

void BattleRules::beginRound(const BattleCommand cmd[2], const BattleActionTable& actions) {
    ++round;
    pairBroken_ = false; // (btl_main 500, 6243-6244)
    queue_.clear();
    next_ = 0;
    escaped_ = false;
    soloMember_ = 0; // (btl_main 500, 6262: +0xaea back to member 0 each round)
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
    // attack, Asura Lotus), on enemy slot 2..4 at random (a fallen one: make_que retargets); the
    // count (+0x186) moves on every round
    const int allyTurn = allyCounter;
    if (allyAuto) allyCounter = (allyCounter + 1) % 3;
    if (allyAuto && f[1].alive() && !partySkips) {
        const int c = allyTurn;
        Entry e;
        e.order = order++;
        e.actors[0] = 1;
        if (allyScript == 1) e.action = c == 0 ? 212 : 14;
        else e.action = c == 0 ? 214 : c == 1 ? 14 : 207;
        e.kind = e.action == 14 ? kCmdAttack : kCmdGene;
        e.noCost = true; // (30496-30545 write no MP, +0x1c - btlord_init_all zeroed it, 6434: make_que's check and pay read 0)
        e.target = random(3) + 2;
        queue_.push_back(e);
    }
    // btlord_enemy_rand: in formation 2 (+0x10: two size-1 monsters) with both standing, a third
    // of the rounds they act together
    enemyPair = false;
    if (!enemiesSkip) {
        if (formation == 2 && f[2].alive() && f[3].alive() && random(3) == 1) {
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
    // btlord_enemy_rand: skill 153 - standing at a quarter of the HP or less, 20% holding the
    // Break (else 10) of guarding this round (in pair mode the pair both), as well as when hit
    for (int i = 0; i < 5; ++i) {
        Fighter& x = f[size_t(i)];
        if (!x.alive() || x.hp > x.hpMax / 4 || !has(i, 153)) continue;
        if (random(100) < (holdsBreak(i) ? 20 : 10)) {
            x.guarding = true;
            if (i < 2 && pair) f[size_t(1 - i)].guarding = true;
            if (enemyPair && (i == 2 || i == 3)) f[size_t(5 - i)].guarding = true;
        }
    }
    // btlord_calc_spd: the actors' average speed (+19998 acting first) and the action's - read
    // afresh before every pick (pickNext)
    for (Entry& e : queue_)
        if (const BattleActionDef* d = actions.get(e.action)) e.actionSpeed = d->speed;
    for (Entry& e : queue_) // Asura Lotus strikes 2..5 times (2..3 in pair mode)
        if (e.action == 207) e.repeats = random(e.actors[0] < 2 ? (pair ? 2 : 4) : (enemyPair ? 2 : 4)) + 2;
}

size_t BattleRules::pickNext() {
    // btlque_make_que: btlord_calc first (every record's speed from its actors as they are now -
    // a Speed Up or an Invincible taken this round moves the rest of it), then a pending counter,
    // then the escape (done == 2) whatever the speeds, then the strictly fastest - the first
    // record of equals
    size_t best = queue_.size();
    long long bestKey = 0;
    for (size_t i = next_; i < queue_.size(); ++i) {
        Entry& e = queue_[i];
        int sum = 0, n = 0;
        for (int a : e.actors) // (btlord_calc_spd 30762-30801: every listed actor, a fallen one too)
            if (a >= 0) sum += spdOf(a) + (invincible(a) ? 19998 : 0), ++n;
        e.speed = (n ? sum / n : 0) + e.actionSpeed;
        const long long key = e.counter ? (1ll << 40) : e.kind == kCmdEscape ? (1ll << 39) : e.speed;
        if (best == queue_.size() || key > bestKey || (key == bestKey && e.order < queue_[best].order)) best = i, bestKey = key;
    }
    return best;
}

std::vector<int> BattleRules::resolveTargets(const BattleActionDef& a, const Entry& e) {
    const int user = e.actors[0];
    std::vector<int> pool = targets(a, user);
    if (pool.empty()) return pool;
    std::vector<int> out;
    if (isGroupScope(a) || a.scope == 0) out = pool;
    else {
        int t = e.target;
        if (std::find(pool.begin(), pool.end(), t) == pool.end()) { // fell meanwhile: btlque_make_que's
            if (a.scope == 1) t = pool.front(); // (itself)
            else if (a.scope == 2) t = user; // own side: the actors themselves
            else { // scope 4..7: a random living opponent of the first actor
                std::vector<int> foes;
                for (int i = user < 2 ? 2 : 0; i < (user < 2 ? 5 : 2); ++i)
                    if (f[size_t(i)].alive()) foes.push_back(i);
                if (foes.empty()) return out;
                t = foes[size_t(random(int(foes.size())))];
            }
        }
        out.push_back(t);
    }
    if (e.target2 >= 0 && !isGroupScope(a) && a.scope != 0 && f[size_t(e.target2)].alive() &&
        std::find(out.begin(), out.end(), e.target2) == out.end())
        out.push_back(e.target2); // (btlque_add_counter: all the blow's actors)
    // btlque_make_que (31164-31183): in pair mode a single target on the pair's side is both; with the
    // enemy pair (+0xaec) one on the enemies' side is enemies 2 and 3 - whatever the scope (a self
    // action too: Counter Stance gives both the stance)
    if (pair && out.size() == 1 && out[0] < 2) {
        out.clear();
        for (int i = 0; i < 2; ++i)
            if (f[size_t(i)].alive()) out.push_back(i);
    }
    if (enemyPair && out.size() == 1 && out[0] >= 2) {
        out.clear();
        for (int i = 2; i < 4; ++i)
            if (f[size_t(i)].alive()) out.push_back(i);
    }
    return out;
}

bool BattleRules::nextTurn(const BattleActionTable& actions, BattleTurn& out) {
    while (next_ < queue_.size()) {
        if (outcome() != 0) return false;
        {
            const size_t i = pickNext();
            if (i == queue_.size()) return false;
            if (i != next_) std::rotate(queue_.begin() + std::ptrdiff_t(next_), queue_.begin() + std::ptrdiff_t(i), queue_.begin() + std::ptrdiff_t(i) + 1);
        }
        Entry e = queue_[next_++];
        if (e.rechoose) { // btlque_make_que: a twice-acting monster's last go is chosen as it comes up
            Entry fresh = e;
            fresh.rechoose = false;
            fresh.paid = false; // (btlai_prc clears the entry's announced flag, +0x3e0: named again)
            enemyChoice(e.actors[0], enemyPair, fresh, actions, e.action); // (btlai_prc reads +0xaec as it is now)
            fresh.repeats = 1;
            if (fresh.kind == kCmdNone) continue;
            e = fresh;
        }
        bool againQueued = false;
        if (e.repeats > 1 && outcome() == 0) { // btlque_make_que: the record stays for another go
            Entry again = e;
            --again.repeats;
            again.paid = true;
            // a monster whose row (+0x3e) has it act twice chooses afresh then (btlai_prc, the old
            // action ruled out; not Asura Lotus)
            if (e.actors[0] >= 2 && e.action != 207 && f[size_t(e.actors[0])].def && f[size_t(e.actors[0])].def->raw[0x3e] == 2) again.rechoose = true;
            queue_.insert(queue_.begin() + std::ptrdiff_t(next_), again);
            againQueued = true;
        }
        // btlque_make_que's cancels (57065 / 57003 / 57080 / 57081, LAB_00034768 31079-31083 and 31112-31117): the
        // record's count to 0 and done - its further goes (Asura Lotus's strikes, a twice-acting monster's second) go too
        auto cancel = [&](int message) {
            if (againQueued) queue_.erase(queue_.begin() + std::ptrdiff_t(next_));
            out.cancelled = true, out.message = message;
            return true;
        };
        for (int& a : e.actors) // the fallen don't act
            if (a >= 0 && !f[size_t(a)].alive()) a = -1;
        if (e.actors[0] < 0) std::swap(e.actors[0], e.actors[1]);
        if (e.actors[0] < 0 || e.done) continue;
        // btlque_make_que (31236-31258): a pair attack (18..21) with one actor left becomes that one's
        // own - a member's +0xa2 + 10 (the character's), a monster's (model - 73) / 2 + 10 (the enemy
        // pair's) - announced anew
        if (e.action >= 18 && e.action <= 21 && e.actors[1] < 0) {
            const Fighter& x = f[size_t(e.actors[0])];
            if (e.actors[0] < 2) e.action = x.character + 10;
            else if (x.def) e.action = (x.def->model - 73) / 2 + 10;
        }
        // btlque_make_que (31328-31330): +0xaea - the party member whose entry comes up, each action in
        // speed order (the victory's camera and voice, the EXP / Oz skills out of pair mode read it)
        if (e.actors[0] < 2) soloMember_ = e.actors[0];
        out = BattleTurn();
        notices_.clear();
        chimes_ = 0;
        lineBy_ = -1;
        note_ = 0, noteValue_ = 0;
        out.kind = e.kind;
        out.action = e.action;
        out.actors[0] = e.actors[0];
        out.actors[1] = e.actors[1];
        out.counter = e.counter;
        out.repeat = e.paid;
        breakFlags &= ~0x3200u; // btl_main 600: per action
        queFell_ = false;       // (make_que's memset of the que, base + 0x494, 200 bytes: +0xc4, +0xbd, +0xbe)
        bool cancelTried = false, cancelBreak = false; // que +0xbe / +0xbd: Trompe-L'oeil's cancel roll won, its Break point
        if (e.kind == kCmdEscape) {
            // btlact_escape_init: certain while the pair holds the Break, else by levels
            int chance = 100;
            if (breakGauge < 1 && !has(0, 160) && !has(1, 160)) { // 160: Beast Wavelength
                // (44927-44985 over the que record's five actors and five targets: IsDead(-1) is false,
                // so an empty slot counts with the level byte at btl - 0x20 - gac+0x35950, the visited-map
                // table's entry 99, never reached: 0. The menu fills slot 0 with member 0 alone, no
                // targets (scope 255), and slot 1 with member 1 and targets 2, 3, 4 - make_que runs slot 0,
                // or slot 1 once member 0 is down: L0 / 5 - 0, else L1 / 5 - the standing enemies' levels
                // over them + 2)
                int d;
                if (f[0].alive()) d = f[0].level / 5;
                else {
                    int el = 0, en = 0;
                    for (int i = 2; i < 5; ++i)
                        if (f[size_t(i)].alive()) el += f[size_t(i)].level, ++en;
                    d = (f[1].alive() ? f[1].level : 0) / 5 - el / (en + 2);
                }
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
        // btlque_make_que: a gene silenced since it was chosen (+0x8f) - "can't use genes" (57065)
        // in place of the action; the pair's without the MP any more - 57081
        const unsigned ac = unsigned(e.action);
        if (e.kind == kCmdGene && f[size_t(e.actors[0])].status[kStatusSilence] && ac - 301u > 64u && ac - 1u > 124u && ac - 413u > 16u)
            return cancel(57065);
        // (btlque_make_que 31096-31125: the pair broke this round - a member whose own plates lack the
        // gene can't cast it, 57003 with the first actor's name; before the MP's check)
        if (e.kind == kCmdGene && e.actors[0] < 2 && pairBroken_)
            for (int k : e.actors)
                if (k >= 0 && k < 2 && std::find(f[size_t(k)].abilities.begin(), f[size_t(k)].abilities.end(), int(e.action) - 120) == f[size_t(k)].abilities.end())
                    return cancel(57003);
        if (e.kind == kCmdItem && e.actors[0] < 2 && e.item >= 21 && e.item <= 46 && !bag_.empty()) { // none left: 57080
            if (bag_[size_t(e.item)] <= 0) return cancel(57080);
            --bag_[size_t(e.item)];
        }
        // the gene's MP (que +0xaa, the record's +0x1c - the first go's only): each member pays it in pair mode; the
        // guest's own entries carry none (31108 / 31279-31300 read 0: its genes are free)
        if (e.kind == kCmdGene && e.actors[0] < 2 && !e.paid && !e.noCost) {
            const int cost = geneCost(*a, e.actors[0]);
            bool enough = true;
            for (int k : e.actors)
                if (k >= 0) enough &= f[size_t(k)].mp >= cost;
            if (!enough) return cancel(57081);
            for (int k : e.actors)
                if (k >= 0) f[size_t(k)].mp -= cost;
        }
        int counterBy = -1;
        std::vector<int> hitList = resolveTargets(*a, e);
        firstTarget_ = hitList.empty() ? -1 : hitList.front(); // btlcalc_critical reads G+0x7a
        // btlque_make_que: each target's 131 / 136 (20% holding the Break, else 10) - one success
        // fends off the whole action ("Miss!" on every target); +20 rules both dodges out
        dodgeHit_ = dodgeGene_ = false;
        for (int t : hitList) {
            if (has(t, 131) && !dodgeHit_ && random(100) < (holdsBreak(t) ? 20 : 10)) dodgeHit_ = true;
            if (has(t, 136) && !dodgeGene_ && random(100) < (holdsBreak(t) ? 20 : 10)) dodgeGene_ = true;
        }
        if (a->sureGene) dodgeHit_ = dodgeGene_ = false; // (make_que 31419-31422: +0x20 clears que +0xb8 and +0xb9)
        // btlact_gene / btlact_item: a hostile gene or item meeting a barrier on any target takes
        // every target's barrier and comes back (0x1004 / 0x100e) - on its users, or with scope 5
        // on all of their side standing - its users' power as before; the Break counts it for the
        // barrier's side, and what it fells leaves no item or gene (+0xaf)
        int breakBy = e.actors[0];
        bool reflected = false;
        if (a->hostile && (e.kind == kCmdGene || e.kind == kCmdItem) && e.actors[0] >= 0) {
            for (int t : hitList)
                if (f[size_t(t)].status[kStatusBarrier]) f[size_t(t)].status[kStatusBarrier] = 0, reflected = true;
            if (reflected) {
                breakBy = hitList.front();
                out.barriers = hitList;
                hitList.clear();
                const bool party = e.actors[0] < 2;
                if (a->scope == 5) {
                    for (int i = party ? 0 : 2; i < (party ? 2 : 5); ++i)
                        if (f[size_t(i)].alive()) hitList.push_back(i);
                } else
                    for (int k : e.actors)
                        if (k >= 0 && f[size_t(k)].alive()) hitList.push_back(k);
                dodgeGene_ = false;
                out.reflected = true;
            }
        }
        int drained = 0;
        auto atkSumOf = [&]() { int s = 0; for (int k : e.actors) if (k >= 0) s += atkOf(k); return s; };
        auto magicBoost = [&](int v) { return has(e.actors[0], 156) ? v + v / 5 : v; };
        auto recovery = [&](int v) { return has(e.actors[0], 155) ? v * 2 : v; };
        // (_btlcalc_prc_0404 24006-24030: a physical action's every target worked out at btlact_attack_init, the
        // amounts applied at the hit - one target's fall or auto-guard doesn't change another's result)
        std::vector<BattleHit> physicalHits;
        if (a->calc == 0)
            for (int t : hitList) physicalHits.push_back(physical(*a, e.actors, t));
        size_t hitIndex = 0;
        for (int t : hitList) {
            const size_t thisHit = hitIndex++;
            BattleHit h;
            h.target = t;
            int calc = a->calc;
            Fighter& tx = f[size_t(t)];
            switch (calc) {
            case 2: h.amount = a->power * 100 / std::max<int>(1, int(hitList.size())); break; // btlcalc_0407: shared
            case 3: h.amount = a->power * 100; break;                                           // 0408: fixed
            case 4: case 6: { // 0409 / 0411: MP lost (power x 10 / power% of it)
                h.mp = true;
                h.amount = calc == 4 ? a->power * 10 : a->power * tx.mp / 100;
                if (invincible(t)) h.amount = 0; // (FUN_0002a184: Is_invincible)
                if (h.amount > 0) breakEvent(breakBy, 2, t); // (FUN_0002a184, 23694: MP lost feeds the Break too)
                tx.mp = std::clamp(tx.mp - h.amount, 0, tx.mpMax);
                out.hits.push_back(h);
                continue;
            }
            case 5: case 7: case 8: { // 0410 power% of its HP / 0412 of the users' HP / 0413 of their attack
                int base = tx.hp;
                if (calc == 7) { base = 0; for (int k : e.actors) if (k >= 0) base += std::max(0, f[size_t(k)].hp - 1); }
                if (calc == 8) base = atkSumOf();
                h.amount = magicBoost(zoku(a->power * base / 100, *a, e.actors[0], t, true));
                if (dodgeGene_) h.amount = 0, h.type = kHitMiss, note_ = 57062;
                break;
            }
            case 9: h.amount = std::max(0, tx.hp - 1); break; // 0414: down to 1
            case 10: { // 0415: the purse loses power% (at least 1) - on the first target slot only (22644: param_3 == 0;
                       // a pair's single target is both members, robbed once)
                if (t != hitList.front()) { h.type = kHitQuiet; out.hits.push_back(h); continue; }
                int take = purse > 0 ? std::max(1, a->power * purse / 100) : 0;
                take = std::min(take, purse);
                purse -= take;
                ozStolen += take;
                f[size_t(e.actors[0])].stolen += take;
                out.message = take > 0 ? 57039 : 57038;
                if (take > 0) sayLine(t, 0x16, 0x17, 0x18); else sayLine(t, 0x22, 0x23, 0x24); // (0415 22671: the robbed side)
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
            case 14: // 0419: everything back - its figure the most HP << skill 155 (FUN_00028ca8 22908), the HP itself stopping at the most
                tx.mp = tx.mpMax;
                h.amount = -recovery(tx.hpMax);
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
                // (the figure is what _btlcalc_dmg_hp was asked for, 23748-23786 - 2 x the most with 155 holding the
                // Break; the HP itself stops at the most)
                h.amount = -recovery(tx.hpMax * (holdsBreak(t) ? 100 : 50) / 100);
                h.revived = true;
                break;
            case 22: case 23: // 042700 poison cured / 042701 every status (and the element) gone
                if (calc == 22) tx.status[kStatusPoison] = 0;
                else { for (int& s : tx.status) s = 0; tx.element = 0; }
                h.type = kHitQuiet;
                out.hits.push_back(h);
                continue;
            case 24: { // Analyze (0428 itself does nothing): its lines come after the hit (btlact_gene 0xc1c..: analyzeElements)
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
                h = calc == 0 ? (thisHit < physicalHits.size() ? physicalHits[thisHit] : physical(*a, e.actors, t)) : calc == 1 ? magical(*a, e.actors, t) : heal(*a, e.actors, t);
                if (calc == 0 && a->id == 203 && h.amount > 0) { // btlcalc_0404: Risk Breaker
                    h.amount = h.amount * (holdsBreak(e.actors[0]) ? 200 : 125) / 100;
                    if (t == hitList.front()) // the users give a quarter of their HP first
                        for (int k : e.actors) {
                            if (k < 0 || !f[size_t(k)].alive()) continue;
                            Fighter& u = f[size_t(k)];
                            BattleHit self;
                            self.target = k;
                            self.own = true; // (2.2.0 _btlcalc_0404 22035-22045: chk_dead kind 99)
                            self.amount = u.hp / 4;
                            if (keepAlive && k >= 2 && u.hp - self.amount < 1) self.amount = std::max(0, u.hp - 1);
                            if (self.amount > 0) breakEvent(e.actors[0], 2, t);
                            u.hp = std::max(0, u.hp - self.amount);
                            if (u.hp == 0 && u.status[kStatusEndure]) u.status[kStatusEndure] = 0, u.hp = 1;
                            if (u.hp == 0) { // btlcalc_chk_dead
                                self.fell = true;
                                fall(k, e.actors, true);
                            }
                            out.hits.push_back(self);
                        }
                }
                // (FUN_00029f5c 23594: the roll is drawn whenever the action has a variance, a 0 too)
                if (a->variance > 0) h.amount = (100 - a->variance + random(2 * a->variance + 1)) * h.amount / 100;
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
            // (an instant death on an Invincible target: type 0 and 0 - "0", not the kill's label: 23941-23947 / 22223-22229)
            if (h.type == kHitKill && invincible(t)) h.type = kHitNormal, h.amount = 0;
            if (h.type == kHitKill) h.amount = tf.hp;
            // _btlcalc_dmg_hp (23744): Is_invincible (a pair's either member) takes the damage to 0 and
            // leaves the result's type - a "0" with the hit's own label, if any
            if (h.amount > 0 && invincible(t)) h.amount = 0;
            // (chk_karaok_dmg_hp - the story battles' last HP - only for a monster: +0xa2 > 7, 23757 / 23996 / 23768)
            if (keepAlive && t >= 2 && h.amount > 0 && tf.hp - h.amount < 1) h.amount = std::max(0, tf.hp - 1);
            if (calc == 8) drained += h.amount; // (0413: que +0x80 adds what dmg_hp took - after Invincible)
            if (calc == 0 && round > 3 && h.amount >= 1 && a->id == 18) lateAttack = true; // btlcalc_0404: +0xa50
            if (h.amount > 0) breakEvent(breakBy, 2, t);
            tf.hp = std::clamp(tf.hp - h.amount, 0, tf.hpMax);
            if (tf.hp == 0 && before > 0 && tf.status[kStatusEndure]) {
                tf.status[kStatusEndure] = 0;
                tf.hp = 1;
            }
            if (h.type == kHitCritical || h.critical) breakEvent(breakBy, 8, t); // (que +0xbf)
            if (before > 0 && tf.hp == 0) { // btlcalc_chk_dead
                h.fell = true;
                fall(t, e.actors, !reflected);
                breakEvent(breakBy, 0x10, t);
            }
            // 153 (btlord_chk_autoguard 30414-30466, from chk_dead on any hit - no side test): at a quarter of
            // the HP or less the target may start guarding; the pair, or the enemy pair (2 and 3), together
            if (h.amount > 0 && tf.hp > 0 && tf.hp <= tf.hpMax / 4 && !tf.guarding && has(t, 153) &&
                random(100) < (holdsBreak(t) ? 20 : 10)) {
                tf.guarding = true;
                if (t < 2 && pair) f[size_t(1 - t)].guarding = true;
                if ((t == 2 || t == 3) && enemyPair) f[2].guarding = f[3].guarding = true;
            }
            if (h.amount > 0 && tf.hp > 0 && (t < 2) != (e.actors[0] < 2)) {
                // 154 (or calc 34's stance): a physical hit may be answered with the target's attack
                if (calc == 0 && counterBy < 0 && !e.counter &&
                    (tf.counterStance || (has(t, 154) && random(100) < (holdsBreak(t) ? 20 : 10))))
                    counterBy = t;
            }
            if (calc == 0 && a->id == 201 && h.amount >= 1) { // btlcalc_0404: Trompe-L'oeil
                const bool held = holdsBreak(e.actors[0]);
                if (random(8) == 0 && held && !cancelTried) { // (_btlcalc_0404 22066-22090: once an action)
                    cancelTried = true;
                    if (!monsterField(tx, 0x32)) // the targets' commands this round are cancelled
                        for (int c : hitList) {
                            for (size_t q = next_; q < queue_.size(); ++q)
                                if (queue_[q].actors[0] == c || queue_[q].actors[1] == c) queue_[q].done = true;
                            notice(c, 3);
                        }
                }
                if (!held && !cancelBreak) cancelBreak = true, breakEvent(e.actors[0], 0x20, t);
            }
            out.hits.push_back(h);
        }
        if (a->id >= 10 && a->id <= 21) // (24029-24040: +0xcf marked only for an actor with 135 - chk_skill_btl, a pair's
            for (int k : e.actors)       // shared - or already marked; one without keeps its first)
                if (k >= 0 && has(k, 135)) f[size_t(k)].firstAttack = false;
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
                    if (item >= 81 && item <= 186) ++chimes_; // (FUN_0002aa8c 24052-24059: SE 0x12)
                    out.message = 0; // the catch is queued instead, one message an item
                    notice(-item, 5);
                }
            } else
                out.message = 57038; // failed
            // (0504 24186-24208: a catch 0x96, a failure 0x97 - the thief's side; "nothing left" none)
            if (out.message == 0) sayLine(e.actors[0], 0x96, 0x96, 0x96);
            else if (out.message == 57038) sayLine(e.actors[0], 0x97, 0x97, 0x97);
        }
        if (a->calc == 7) // 0412: the users give up their HP to 1
            for (int k : e.actors)
                if (k >= 0 && f[size_t(k)].alive()) {
                    // (_btlcalc_0412 22493-22510: through dmg_hp2 - Break event 2 - then chk_dead with the user's
                    // index: at 1 HP, a quarter or less, skill 153 may start the guard)
                    Fighter& u = f[size_t(k)];
                    BattleHit self;
                    self.target = k;
                    self.own = true;
                    self.amount = u.hp - 1;
                    if (self.amount > 0) breakEvent(e.actors[0], 2, hitList.empty() ? -1 : hitList.front());
                    u.hp = 1;
                    out.hits.push_back(self);
                    if (self.amount > 0 && u.hp <= u.hpMax / 4 && !u.guarding && has(k, 153) &&
                        random(100) < (holdsBreak(k) ? 20 : 10)) {
                        u.guarding = true;
                        if (k < 2 && pair) f[size_t(1 - k)].guarding = true;
                        if ((k == 2 || k == 3) && enemyPair) f[2].guarding = f[3].guarding = true;
                    }
                }
        // 0413 (22570-22597): at the last target what was drained (x2 with skill 155) heals each user by
        // its share - divided by the users (que +0xc2), not the targets
        int users = 0;
        for (int k : e.actors) users += k >= 0;
        if (a->calc == 8 && drained > 0 && !hitList.empty() && users > 0)
            for (int k : e.actors)
                if (k >= 0 && f[size_t(k)].alive()) {
                    BattleHit back;
                    back.target = k;
                    back.amount = -recovery(drained) / users;
                    f[size_t(k)].hp = std::clamp(f[size_t(k)].hp - back.amount, 0, f[size_t(k)].hpMax);
                    out.hits.push_back(back);
                }
        // btlque_add_counter: only once the striker's record is done (a repeating one on its last
        // go); one answer, played next, by the blow's targets (que +0x8e, here up to two: the pair
        // both) on its actors (+0x84), with the attack of the fighter whose roll it was
        // ... and none when someone fell in the action: chk_dead's que +0xc4 has btl_main 700 wipe the counter list
        // (6232-6235)
        if (counterBy >= 0 && e.actors[0] >= 0 && e.repeats <= 1 && !queFell_) {
            Entry c;
            c.counter = true;
            c.kind = kCmdAttack;
            c.order = -1;
            for (int t : hitList)
                if (f[size_t(t)].alive()) {
                    if (c.actors[0] < 0) c.actors[0] = t;
                    else if (c.actors[1] < 0 && t != c.actors[0]) c.actors[1] = t;
                }
            if (c.actors[0] < 0) c.actors[0] = counterBy;
            c.target = e.actors[0];
            c.target2 = e.actors[1];
            // a member: character + 10, in pair mode character / 2 + 18 (records from 8: + 0x125);
            // a monster by its model (+0xa2): below 73 itself, else (model - 73) / 2 (/ 4 + 8 with
            // the enemy pair) - + 10 under 8, else + 0x125
            int base, v;
            if (counterBy < 2) {
                base = f[size_t(counterBy)].character;
                v = pair ? base / 2 + 8 : base;
            } else {
                const int model = f[size_t(counterBy)].def ? f[size_t(counterBy)].def->model : 0;
                base = v = model;
                if (model > 72) base = v = (model - 73) / 2, v = enemyPair ? (model - 73) / 4 + 8 : base;
            }
            c.action = base < 8 ? v + 10 : v + 0x125;
            if (!actions.get(c.action)) c.action = counterBy < 2 ? attackAction(counterBy) : 10;
            queue_.insert(queue_.begin() + std::ptrdiff_t(next_), c);
        }
        catchUpBreak();
        firstTarget_ = -1;
        out.notices = notices_;
        out.chimes = chimes_;
        out.lineBy = lineBy_;
        for (int k = 0; k < 3; ++k) out.line[k] = line_[k];
        if (!out.message) out.message = note_, out.messageValue = noteValue_;
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
    // btl_main 799 (2.2.0 decomp 6161-6185): btlord_chk's 3 (the pair down) / 2 (the enemies) go to the defeat /
    // the victory at once - 800..899, and with it btlcalc_status_prc (24628, at 899's count 28), never runs: no
    // poison, regen or countdown after the battle is decided
    roundPoisoned.clear(), roundFell.clear(), roundChimes = 0;
    if (outcome() != 0) return;
    // btlord_chk: the holder wasn't hit all round
    if (!(breakFlags & 0x400) && (breakFlags & 0x8000)) breakEvent(breakGauge < 1 ? 2 : 0, 0x40, -1);
    catchUpBreak();
    // btlcalc_status_prc, each standing fighter in turn: the statuses count down (not Endure),
    // then one amount through btlcalc_dmg_hp - poison's 10% less skill 133's 5% (a story boss:
    // back to full) - with its Invincible (a pair's either member), Endure and chk_karaok rules;
    // falling to it is btlcalc_chk_dead's fall (no Break event: +0xafa)
    const int party[2] = {0, f[1].present ? 1 : -1};
    roundPoisoned.clear(), roundFell.clear();
    chimes_ = 0;
    for (int i = 0; i < 5; ++i) {
        Fighter& x = f[size_t(i)];
        if (!x.alive()) continue;
        x.counterStance = false;
        const bool poisoned = x.status[kStatusPoison] > 0;
        if (poisoned) roundPoisoned.push_back(i);
        for (int k = 0; k < kStatusCount; ++k)
            if (k != kStatusEndure && x.status[k] > 0) --x.status[k];
        if (has(i, 138)) x.mp = std::min(x.mpMax, x.mp + x.mpMax / 20);
        int d = (poisoned ? x.hpMax / 10 : 0) - (has(i, 133) ? x.hpMax / 20 : 0);
        if (regenFull[size_t(i)]) d = x.hp - x.hpMax; // a story boss heals whole until its moment comes
        if (d == 0) continue;
        if (d > 0 && invincible(i)) d = 0;
        if (keepAlive && i >= 2 && d > 0 && x.hp - d < 1) d = std::max(0, x.hp - 1);
        x.hp = std::clamp(x.hp - d, 0, x.hpMax);
        if (x.hp == 0 && x.status[kStatusEndure]) x.status[kStatusEndure] = 0, x.hp = 1;
        if (x.hp == 0) fall(i, party, true), roundFell.push_back(i);
    }
    roundChimes = chimes_;
    for (Fighter& x : f) x.guarding = false;
}

bool BattleRules::invincible(int t) const {
    if (t < 0 || t > 4) return false;
    if (t < 2 && pair) return f[0].status[kStatusInvincible] || f[1].status[kStatusInvincible];
    if ((t == 2 || t == 3) && enemyPair) return f[2].status[kStatusInvincible] || f[3].status[kStatusInvincible];
    return f[size_t(t)].status[kStatusInvincible] != 0;
}

void BattleRules::fall(int t, const int actors[2], bool itemsAndGenes) {
    Fighter& tf = f[size_t(t)];
    queFell_ = true;
    for (int& s : tf.status) s = 0;
    if (t < 2) tf.element = 0; // (chk_dead: a member's element goes with it)
    tf.guarding = false;
    if (tf.enemy && !tf.rewarded) {
        tf.rewarded = true;
        // chk_dead (24569-24600): pair mode either member's skill, else the member +0xaea names (the last to act)
        auto member = [&](int skill) { return pair ? has(0, skill) || has(1, skill) : has(soloMember_, skill); };
        const bool expUp = member(161), ozUp = member(162);
        expGained += tf.exp + (expUp ? tf.exp / 5 : 0);
        ozGained += tf.oz + (ozUp ? tf.oz / 5 : 0) + tf.stolen; // what it stole comes back
        tf.stolen = 0;
        fallRewards(t, actors, itemsAndGenes);
    }
    if (t < 2 && pair) pairBroken_ = true; // (+0xd1 on both members)
    if (t < 2) pair = false;         // a fallen member ends pair mode
    else enemyPair = false;          // (chk_dead: +0xaec cleared as a monster falls)
}

bool BattleRules::knows(int item) const {
    return std::find(known_.begin(), known_.end(), item) != known_.end() ||
           std::find(learned.begin(), learned.end(), item) != learned.end() ||
           std::find(drops.begin(), drops.end(), item) != drops.end();
}

// btlcalc_0505 / 0506 / chk_dead: what a fallen monster leaves
void BattleRules::fallRewards(int enemy, const int actors[2], bool itemsAndGenes) {
    const Fighter& x = f[size_t(enemy)];
    if (!x.def) return;
    int sum = 0, n = 0;
    for (int k = 0; k < 2; ++k)
        if (actors[k] >= 0) { // (24272 / 24395: _pcparam_lv of the actor's character - the saved level, a guest's own)
            const Fighter& a = f[size_t(actors[k])];
            sum += a.saveLevel >= 0 ? a.saveLevel : a.level, ++n;
        }
    const int avg = n ? sum / n : 1, lv = x.level;
    auto row = [&](int off) { return monsterField(x, off); };
    // (chk_dead: que +0xc3 - a reflected gene's kill - leaves out 0505 / 0506; the plate stays)
    if (itemsAndGenes && row(0xc0) + row(0xc2) + row(0xc4) != 0) { // btlcalc_0505: an item - the roll, then which
        int d = avg + 10 - lv;
        const int chance = d < -4 ? 20 : d < 1 ? 30 : d < 6 ? 40 : 50;
        if (random(100) < chance) {
            int item;
            if (lv >= 40 && random(3) == 1) item = row(0xc4);
            else item = lv >= 20 && random(2) ? row(0xc2) : row(0xc0);
            if (item >= 81 && item <= 186) { // FUN_0002ae14: an ability or skill into the bag at once, no result line
                ++chimes_; // (24221: __effgo(0x12))
                if (std::find(learned.begin(), learned.end(), item) == learned.end()) learned.push_back(item);
            } else if (item > 0 && drops.size() < 16) drops.push_back(item); // (+0xa58: 16 slots - one past them is lost)
        }
    }
    if (itemsAndGenes && row(0xb4) != 0) { // a gene
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
                    learned.push_back(g), learnedNew = true, ++chimes_; // (FUN_0002ae14, 24346: SE 0x12)
                    sayLine(actors[0], 0xa9, 0xaa, 0xab); // (0506 24357)
                    note_ = isSkill(g) ? 57050 : 57049, noteValue_ = g; // (btlcalc_0506's line, at once)
                    break;
                }
            }
        }
    }
    const int plate = plateDrop(x.def->model); // chk_dead: 481 + (model - 13) / 3
    if (plate > 0 && !knows(plate) && drops.size() < 16) drops.push_back(plate);
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
    // btlcalc_0504 / FUN_0002ae14: a Steal Item catch and an ability or skill taught (0506) or
    // dropped (0505) went into the bag the moment they happened, whatever the outcome; only the
    // other drops wait for the result screen (MbResultSet, a win). So a skill taught here counts in this
    // battle's level-ups: __menuu_MbResultPrc 5 (58770) -> _pcparam_change_lv's chk_gene 0xa4..0xa9 (29668-29697)
    for (int item : stolen) gs.items[item] = std::min(kMaxItemCount, gs.items[item] + 1);
    for (int item : learned) gs.items[item] = std::max(1, gs.items[item]);
    for (int k = 0; k < 2; ++k) {
        const Fighter& x = f[size_t(k)];
        if (!x.present || x.guest) continue;
        PartyMember& m = gs.party[size_t(x.character)];
        // 163: MP back to full - as a number (5926-5944); a defeat passes 0x238b too (0x513 -> 9000 -> 0x238b),
        // only Options' Return to Title (MuCom_BitSet(0x10000), 70743) skips it - and nothing is kept then
        m.mp = x.skills[163 - 131] ? x.mpMax : x.mp;
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
    if (won) {
        addOz(gs, ozGained * (doubleOz ? 2 : 1));
        for (int item : drops) gs.items[item] = std::min(kMaxItemCount, gs.items[item] + 1);
    }
    return levelled;
}

} // namespace cr
