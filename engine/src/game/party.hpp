#pragma once
// The eight characters' levels, experience and equipment, and the stats the menus show
// (pcparam_max_*): base (_pcparam, 0xd2cd0) + level × 50 (HP) or × 5 (the others) + the
// equipment's bonuses (param_equip_calc) + the save's per-stat bonus (+0x2fc5c HP, +0x16..+0x28):
// what the levels from 20 on add (pcparam_change_lv: leaving level L >= 20, 10 L - 190 HP and
// L - 19 to each of the others), the level-up skills' boosts and the seeds, at most 50000 / 5000.
// (1.0.1's loop runs one step short when a battle gives several levels; 2.2.0 fixed it - kept
// fixed here.) A new game's party (prty_init / _prty_init_equip):
// level 1, everyone in their own starting weapon and armor, some with jewel 426. The level
// needs _exp[level] (0xd2d30) experience in total (GetNextExp).
//
// The seeds (items 3..6, `use_item`) add a point each to Attack / Defense / Speed / Luck, into
// the same bonus: one more while it's below 5000 (`MuCom_GetUsedItemLimitNum`: 5000 - bonus;
// the save's +0x33c..+0x348).

#include <array>

namespace cr {

class ItemTable;

constexpr int kCharacters = 8;  // Escher, Musiea, Eluca, Zhamo, Olgar, Vahti, Ayuta, Mana
constexpr int kMaxLevel = 99;
enum Stat { kHp, kMp, kAttack, kDefense, kSpeed, kLuck, kStats };
enum EquipSlot { kWeapon, kArmor, kJewel, kEquipSlots };

struct PartyMember {
    int level = 1;
    int exp = 0;
    int hp = -1, mp = -1; // current; -1 = full
    int equip[kEquipSlots] = {0, 0, 0}; // item numbers, 0 = none
    int seeds[4] = {0, 0, 0, 0};        // seed points: Attack, Defense, Speed, Luck
    int genes[4] = {0, 0, 0, 0};        // gene plates worn (game/genes.hpp): Human Genes, then 3
    int boosts[6] = {0, 0, 0, 0, 0, 0}; // level-up skills' points (164..169): HP, MP, Attack.. Luck
};
constexpr int kMaxSeedPoints = 5000;
constexpr int kMaxHpBonus = 50000, kMaxStatBonus = 5000;

using Party = std::array<PartyMember, kCharacters>;

Party newParty();
int expForLevel(int level); // total experience level `level` needs (level 1 = 0)
// Stat `stat` with the member's equipment, seeds and level-up boosts; `swap` (if > 0) as if that
// item were worn in its slot. memberStat (game/genes.hpp) adds the skills' (the Gates).
int maxStat(const PartyMember& m, int character, Stat stat, const ItemTable& items, int swap = 0);
// _pcparam (0xd2cd0): the character's level-0 stat
int baseStat(int character, Stat stat);
// What levels 20..level added to `stat` (pcparam_change_lv)
int levelGrowth(int level, Stat stat);
// The per-stat bonus: growth, boosts and (Attack..Luck) seeds, capped
int statBonus(const PartyMember& m, Stat stat);
// How many more seeds of `stat` (kAttack..kLuck) the member can take
int seedRoom(const PartyMember& m, Stat stat);
// The slot an item goes in (kind 6 / 7 / 8), -1 if it isn't equipment
int equipSlot(int kind);

} // namespace cr
