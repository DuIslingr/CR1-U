#include "game/encounter.hpp"

#include <vector>

namespace cr {

void EncounterCounter::enterMap(const EncounterTable* table) {
    have_ = table && table->stage >= 0 && !table->groups.empty();
    table_ = have_ ? *table : EncounterTable();
    walked_ = 0;
    chance_ = have_ ? table_.rate : 0;
}

bool EncounterCounter::check(const std::function<int(int)>& random, int partyLevel, bool firstStrikeGene,
                             bool noAmbushGene, BattleStart& out) {
    random(100); // the original draws once and discards it
    if (!have_) return false;
    const float step = float(table_.distance) * 10.0f;
    if (walked_ < step) return false;
    walked_ -= step;
    if (chance_ < random(100)) {
        chance_ += 5;
        return false;
    }
    chance_ = table_.rate;
    // A group by weight among those the party's level allows
    std::vector<const EncounterGroup*> eligible;
    int total = 0;
    for (const EncounterGroup& g : table_.groups)
        if (g.minLevel <= partyLevel) {
            eligible.push_back(&g);
            total += g.weight;
        }
    if (eligible.empty()) return false;
    int pick = random(total);
    const EncounterGroup* chosen = eligible.back();
    for (const EncounterGroup* g : eligible) {
        if (g->weight > pick) {
            chosen = g;
            break;
        }
        pick -= g->weight;
    }
    out.group = chosen->group;
    out.stage = table_.stage;
    out.extra = chosen->extra;
    out.strike = 0;
    const int first = firstStrikeGene ? 50 : chosen->firstStrike;
    if (first > random(100)) out.strike = 1;
    else if (chosen->ambush > random(100) && !noAmbushGene) out.strike = 2;
    return true;
}

} // namespace cr
