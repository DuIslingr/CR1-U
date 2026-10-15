#include "game/party.hpp"

#include <algorithm>

#include "game/shop.hpp"

namespace cr {

namespace {

// _pcparam: HP, MP, Attack, Defense, Speed, Luck
constexpr int kBase[kCharacters][kStats] = {
    {842, 52, 180, 135, 120, 114}, {720, 157, 134, 119, 180, 98}, {695, 85, 164, 115, 144, 105},
    {658, 181, 120, 99, 163, 98},  {900, 26, 195, 152, 73, 78},   {578, 230, 88, 65, 225, 125},
    {710, 110, 140, 110, 200, 90}, {645, 155, 135, 105, 105, 115}};

// _prty_init_equip: weapon, armor, jewel
constexpr int kStartEquip[kCharacters][kEquipSlots] = {{201, 215, 0}, {229, 243, 426}, {257, 271, 426}, {285, 299, 0},
                                                       {313, 327, 0},  {341, 355, 426}, {369, 383, 0},   {397, 411, 426}};

// _exp: total experience for levels 1..100
constexpr int kExp[100] = {
    0, 40, 130, 290, 540, 900, 1390, 2030, 2840, 3840,
    5050, 6490, 8180, 10140, 12390, 14950, 17840, 21080, 24690, 28690,
    33100, 37940, 43230, 48990, 55240, 62000, 69290, 77130, 85540, 94540,
    104150, 114390, 125280, 136840, 149090, 162050, 175740, 190180, 205390, 221390,
    238200, 255840, 274330, 293690, 313940, 335100, 357190, 380230, 404240, 429240,
    455250, 482290, 510380, 539540, 569790, 601150, 633640, 667280, 702090, 738090,
    775300, 813740, 853430, 894120, 936370, 979930, 1024820, 1071060, 1118670, 1167670,
    1218080, 1269920, 1323210, 1377970, 1434220, 1491980, 1551270, 1612110, 1674520, 1738520,
    1804130, 1871370, 1940260, 2010820, 2083070, 2157030, 2232720, 2310160, 2389370, 2470370,
    2553180, 2637820, 2724310, 2812670, 2902920, 2995080, 3089170, 3185210, 3283220, 3500000,
};

} // namespace

Party newParty() {
    Party p;
    for (int c = 0; c < kCharacters; ++c)
        for (int s = 0; s < kEquipSlots; ++s) p[size_t(c)].equip[s] = kStartEquip[c][s];
    for (int c = 0; c < kCharacters; ++c) p[size_t(c)].genes[0] = 479 + (c & 1); // the Human Genes
    return p;
}

int expForLevel(int level) { return kExp[std::clamp(level, 1, 100) - 1]; }

int baseStat(int character, Stat stat) { return kBase[std::clamp(character, 0, kCharacters - 1)][stat]; }

int levelGrowth(int level, Stat stat) {
    const int n = std::max(0, std::clamp(level, 1, kMaxLevel) - 20); // the level-ups from 20
    return stat == kHp ? 5 * n * (n + 1) : n * (n + 1) / 2;         // 10, 20, 30.. / 1, 2, 3..
}

int statBonus(const PartyMember& m, Stat stat) {
    int v = levelGrowth(m.level, stat) + m.boosts[stat];
    if (stat >= kAttack) v += m.seeds[stat - kAttack];
    return std::min(v, stat == kHp ? kMaxHpBonus : kMaxStatBonus);
}

int seedRoom(const PartyMember& m, Stat stat) {
    return stat >= kAttack ? std::max(0, kMaxStatBonus - statBonus(m, stat)) : 0;
}

int equipSlot(int kind) {
    return kind == kKindWeapon ? kWeapon : kind == kKindArmor ? kArmor : kind == kKindJewel ? kJewel : -1;
}

int maxStat(const PartyMember& m, int character, Stat stat, const ItemTable& items, int swap) {
    character = std::clamp(character, 0, kCharacters - 1);
    int equip[kEquipSlots] = {m.equip[0], m.equip[1], m.equip[2]};
    if (const ItemParam* p = items.item(swap); p && equipSlot(p->kind) >= 0) equip[equipSlot(p->kind)] = swap;
    int v = kBase[character][stat] + std::clamp(m.level, 1, kMaxLevel) * (stat == kHp ? 50 : 5);
    for (int e : equip)
        if (const ItemParam* p = items.item(e)) v += p->stat[stat];
    return v + statBonus(m, stat);
}

} // namespace cr
