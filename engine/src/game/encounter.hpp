#pragma once
// Random encounters on the field (fldencount_init / _walk / _encount).

#include <functional>

#include "assets/battle_data.hpp"

namespace cr {

// What a battle starts with (the original keeps it at G+0x354c0..0x354c6).
struct BattleStart {
    int group = 0;   // 1-based enemy group
    int stage = 0;   // kBattleStageFile index
    int extra = -1;  // the encounter row's +4 (G+0x354c2)
    int strike = 0;  // 0 normal, 1 the party strikes first, 2 the enemy does (ambush)
};

class EncounterCounter {
public:
    // fldencount_init, on every room change: the walked distance restarts and the chance
    // goes back to the map's rate
    void enterMap(const EncounterTable* table);
    // fldencount_walk: the leader moved `distance` this frame
    void walked(float distance) { walked_ += distance; }
    // fldencount_encount, once a frame while the field runs: every `distance` x 10 units a check
    // succeeds when random(100) <= the chance; a miss raises the chance by 5 for the next one.
    // `random(n)` is 0..n-1. `partyLevel` is the pair's average; gene 171 / 172 on either
    // member (`firstStrikeGene` / `noAmbushGene`) make a first strike 50% / rule out ambushes.
    bool check(const std::function<int(int)>& random, int partyLevel, bool firstStrikeGene, bool noAmbushGene,
               BattleStart& out);
    bool active() const { return have_; }
    float walkedDistance() const { return walked_; }
    int chance() const { return chance_; }

private:
    EncounterTable table_;
    bool have_ = false;
    float walked_ = 0;
    int chance_ = 0;
};

} // namespace cr
