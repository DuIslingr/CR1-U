#pragma once
// Genes (chk_gene, MuCom_IsSkillEquip, GetSkillAddrFromPlate, btlcalc_0506, btlcalc_chk_dead).
//
// Everything is an item: gene abilities are items 81..130 (owning one = knowing it; its battle
// action is the item + 120, its MP that action's), skills (passive) 131..186, gene plates
// 479..500. A plate (0xd3890, 8 s16 each) lists three abilities and three skills; plates 479 /
// 480 are the men's and the women's Human Genes (each character's own ability 81 + c and skill
// 131 + c). A character wears four plates (the save's +0x350): slot 0 the Human Genes, 1..3 gene
// plates. An ability can be used, a skill counts, when it's known and on a plate the character
// wears.
//
// Learning: a fallen monster may teach the first not known yet of the six abilities / skills it
// carries (its plate's: row +0xb4 any level, +0xb6 from level 5, +0xb8 10, +0xba 15, +0xbc 20,
// +0xbe 25), 80 / 40 / 30 / 10% for the attackers' average level (less 15 with skill 158) less
// its level < -4 / < 1 / < 6 / more (btlcalc_0506); and models 13..72 leave their gene plate
// (481 + (model - 13) / 3: three models a plate) when the bag hasn't one (chk_dead).
// A new game (prty_init_new, 0xcb3f0): each character wears the Human Genes and knows its own
// ability and skill; the pair starts with 3 Emergency Exits and 2 Chocolates.

#include <array>
#include <vector>

namespace cr {

struct GameState;
class ItemTable;

constexpr int kGeneSlots = 4;
constexpr int kFirstPlate = 479, kPlates = 22;
constexpr int kFirstAbility = 81, kLastAbility = 130, kFirstSkill = 131, kLastSkill = 186;

inline bool isPlate(int item) { return item >= kFirstPlate && item < kFirstPlate + kPlates; }
inline bool isGeneAbility(int item) { return item >= kFirstAbility && item <= kLastAbility; }
inline bool isSkill(int item) { return item >= kFirstSkill && item <= kLastSkill; }
inline int geneAction(int ability) { return ability + 120; }
inline int humanPlate(int character) { return kFirstPlate + (character & 1); }

// The plate's six entries (abilities then skills, 0 = none); empty for a non-plate
const std::array<int, 8>& plateContents(int plate);
// How many of the plate's entries are known
int plateLearned(const GameState& gs, int plate);
// The abilities character c can use (known, on a worn plate; the Human Genes' only c's own)
std::vector<int> usableAbilities(const GameState& gs, int character);
bool hasSkill(const GameState& gs, int character, int skill);
// chk_gene_btl: armor that resists an element counts as its skill (148 Blaze / 149 Aqua / 150
// Gale Resistance): each character's armors 222..227 (+28 a character) - 0 none
int armorSkill(int armor);
// pcparam_max_*: the stat with equipment, seeds, boosts and the skills' (175..186, the Gates:
// each known stage worn adds its item's stat)
int memberStat(const GameState& gs, int character, int stat, const ItemTable& items, int swap = 0);
// MbResult's level-ups with skills 164..169 worn: +10 HP / +1 MP, Attack, Defense, Speed, Luck a level
void levelUpBoosts(GameState& gs, int character, int levels);
// The gene plate a monster of battle model `model` leaves (0 none)
int plateDrop(int model);
// prty_init_new's genes and items; ensureStartingGenes only adds what an older save lacks
void giveStartingItems(GameState& gs);
void ensureStartingGenes(GameState& gs);

} // namespace cr
