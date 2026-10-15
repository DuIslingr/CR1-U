#include "game/genes.hpp"

#include <algorithm>

#include "game/event_vm.hpp"
#include "game/party.hpp"
#include "game/shop.hpp"

namespace cr {

namespace {

// The plates' contents (0xd3890)
constexpr std::array<int, 8> kPlate[kPlates] = {
    {81, 83, 85, 87, 131, 133, 135, 137},   {82, 84, 86, 88, 132, 134, 136, 138}, {108, 94, 113, 0, 160, 164, 156, 0},
    {89, 92, 100, 0, 170, 175, 182, 0},     {105, 90, 93, 0, 146, 176, 166, 0},   {93, 108, 110, 0, 165, 155, 147, 0},
    {89, 106, 92, 0, 184, 141, 148, 0},     {113, 103, 111, 0, 173, 157, 186, 0}, {94, 91, 100, 0, 167, 143, 150, 0},
    {105, 91, 104, 0, 160, 153, 169, 0},    {90, 93, 113, 0, 178, 142, 149, 0},   {91, 90, 89, 0, 170, 147, 158, 0},
    {105, 99, 100, 0, 181, 171, 180, 0},    {99, 107, 112, 0, 139, 179, 161, 0},  {92, 95, 107, 0, 141, 148, 154, 0},
    {93, 96, 99, 0, 168, 142, 149, 0},      {94, 97, 104, 0, 146, 143, 150, 0},   {103, 111, 109, 0, 151, 140, 172, 0},
    {106, 100, 98, 0, 145, 185, 139, 0},    {99, 106, 107, 0, 144, 183, 177, 0},  {98, 108, 109, 0, 159, 162, 174, 0},
    {102, 110, 101, 0, 152, 163, 140, 0}};
constexpr std::array<int, 8> kNone{};

int owned(const GameState& gs, int item) {
    auto it = gs.items.find(item);
    return it == gs.items.end() ? 0 : it->second;
}

// The Human Genes' abilities and skills are each character's own (81 + c, 131 + c)
bool ownEntry(int entry, int character) {
    if (entry >= 81 && entry <= 88) return entry == 81 + character;
    if (entry >= 131 && entry <= 138) return entry == 131 + character;
    return true;
}

} // namespace

const std::array<int, 8>& plateContents(int plate) { return isPlate(plate) ? kPlate[plate - kFirstPlate] : kNone; }

int plateLearned(const GameState& gs, int plate) {
    int n = 0;
    for (int e : plateContents(plate)) n += e > 0 && owned(gs, e) > 0;
    return n;
}

std::vector<int> usableAbilities(const GameState& gs, int character) {
    std::vector<int> out;
    character = std::clamp(character, 0, 7);
    for (int plate : gs.party[size_t(character)].genes)
        for (int e : plateContents(plate))
            if (isGeneAbility(e) && owned(gs, e) > 0 && ownEntry(e, character) &&
                std::find(out.begin(), out.end(), e) == out.end())
                out.push_back(e);
    return out;
}

int armorSkill(int armor) {
    const int d = armor - 222; // each character's armors 215..228, 28 items a character
    if (d < 0 || d >= 8 * 28) return 0;
    return d % 28 < 6 ? 148 + d % 28 % 3 : 0;
}

bool hasSkill(const GameState& gs, int character, int skill) {
    if (skill >= 148 && skill <= 150 && armorSkill(gs.party[size_t(std::clamp(character, 0, 7))].equip[kArmor]) == skill)
        return true; // chk_gene_btl: the resisting armors
    if (!isSkill(skill) || owned(gs, skill) <= 0 || !ownEntry(skill, character)) return false;
    for (int plate : gs.party[size_t(std::clamp(character, 0, 7))].genes) {
        const auto& p = plateContents(plate);
        if (std::find(p.begin(), p.end(), skill) != p.end()) return true;
    }
    return false;
}

int memberStat(const GameState& gs, int character, int stat, const ItemTable& items, int swap) {
    character = std::clamp(character, 0, 7);
    int v = maxStat(gs.party[size_t(character)], character, Stat(std::clamp(stat, 0, 5)), items, swap);
    for (int skill = 175; skill <= 186; ++skill)
        if (hasSkill(gs, character, skill))
            if (const ItemParam* p = items.item(skill)) v += p->stat[std::clamp(stat, 0, 5)];
    return v;
}

void levelUpBoosts(GameState& gs, int character, int levels) {
    if (levels <= 0) return;
    for (int k = 0; k < 6; ++k)
        if (hasSkill(gs, character, 164 + k)) gs.party[size_t(character)].boosts[k] += (k == 0 ? 10 : 1) * levels;
}

int plateDrop(int model) { return model >= 13 && model < 73 ? 481 + (model - 13) / 3 : 0; }

void giveStartingItems(GameState& gs) {
    ensureStartingGenes(gs);
    gs.items[1] += 3;  // Emergency Exit
    gs.items[21] += 2; // Chocolate
}

void ensureStartingGenes(GameState& gs) {
    for (int c = 0; c < 8; ++c) {
        PartyMember& m = gs.party[size_t(c)];
        if (!isPlate(m.genes[0])) m.genes[0] = humanPlate(c);
        if (owned(gs, 81 + c) <= 0) gs.items[81 + c] = 1;
        if (owned(gs, 131 + c) <= 0) gs.items[131 + c] = 1;
    }
    for (int p = kFirstPlate; p < kFirstPlate + 2; ++p)
        if (owned(gs, p) <= 0) gs.items[p] = 1;
}

} // namespace cr
