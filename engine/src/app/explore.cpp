#include "app/app.hpp"

namespace crapp {

// --selftest: renders every model through every animation and reports GL errors.
// Battles: every (stage, group) the encounter tables can start: stage, pair and monsters load,
// names resolve, a few frames draw; and one map's counter starts one by walking. Returns problems.
int battleSelfTest(App& app, int& frames, int& glErrors) {
    {
        std::set<std::pair<int, int>> starts;
        for (int area = 0; area < cr::kNumAreas; ++area) {
            cr::EventFile ef;
            if (!ef.load(app.dataDir, area)) continue;
            for (int map = 0; map < cr::kNumMaps; ++map) {
                cr::EncounterTable t;
                if (cr::encounterTable(ef, map, t))
                    for (const cr::EncounterGroup& g : t.groups) starts.insert({t.stage, g.group});
            }
        }
        if (!app.events.loaded() || app.mapNumber < 0) showMap(app, 0, 0, 2);
        int started = 0, actors = 0, unnamed = 0, noModel = 0, noAction = 0, errs = 0;
        for (const auto& [stage, group] : starts) {
            if (SDL_getenv("CR_BATTLE_TRACE")) std::printf("  battle %d on stage %d\n", group, stage), std::fflush(stdout);
            startBattle(app, {group, stage, -1, 0}, 0);
            if (!app.battle.active) continue;
            ++started;
            const std::array<int, 3> ids = app.battleData.group(group);
            const size_t want = size_t(app.battle.rules.f[0].present) + size_t(app.battle.rules.f[1].present) + // who fights
                                size_t(std::count_if(ids.begin(), ids.end(), [](int id) { return id > 0; }));
            if (app.battle.actors.size() != want) ++noModel;
            noModel += app.battle.stage.meshes.empty();
            for (const App::Battle::Actor& a : app.battle.actors) {
                ++actors;
                unnamed += a.name.empty();
                noModel += app.battle.models[a.model].meshes.empty();
                noAction += a.action < 0;
            }
            const bool trace = SDL_getenv("CR_BATTLE_TRACE") != nullptr;
            if (trace) std::printf("    started, %zu actors\n", app.battle.actors.size()), std::fflush(stdout);
            for (int f = 0; f < 3; ++f) {
                app.lastTicks = 0;
                drawFrame(app, false);
                stepBattle(app);
                ++frames;
            }
            if (trace) std::printf("    drawn\n"), std::fflush(stdout);
            errs += app.renderer.takeErrors();
            endBattle(app, true);
            if (trace) std::printf("    ended\n"), std::fflush(stdout);
            app.runner.stop();
        }
        // The first dungeon's first room: walk a unit a frame until a battle starts
        showMap(app, 0, 0, 2);
        app.rng.seed(1);
        int walkedFrames = 0;
        cr::BattleStart bs;
        auto random = [&](int n) { return app.rng.get(n); };
        bool met = false;
        for (; walkedFrames < 20000 && !met; ++walkedFrames) {
            app.encounters.walked(1.0f);
            met = app.encounters.check(random, 1, false, false, bs);
        }
        std::printf("battles: %d of %zu stage/group pairs started, %d actors (%d unnamed, %d without a model, %d without "
                    "an action), %d render API errors; walking map 0 met group %d after %d units\n",
                    started, starts.size(), actors, unnamed, noModel, noAction, errs, met ? bs.group : -1, walkedFrames);
        glErrors += errs;
        int problems = int(starts.size()) - started + unnamed + noModel + noAction + errs + !met;

        // The rules (game/battle_rules.hpp) against numbers worked out by hand
        {
            int bad = 0;
            cr::BattleRules r;
            cr::GameState gs;
            r.setupParty(gs, app.itemTable, 0);
            bad += r.f[0].hpMax != 896 || r.f[1].hpMax != 786 || r.f[0].mp != 60 || r.f[1].mp != 166; // the video's level 1
            // physical: (50 solo bonus + 150) x 200 / 100 - 100 = 300, a critical (+50) 400
            for (cr::Fighter& x : r.f) x = cr::Fighter();
            r.f[0].present = r.f[2].present = true;
            r.f[0].atk = 200, r.f[0].spd = 100, r.f[0].level = r.f[2].level = 1, r.f[0].hp = r.f[0].hpMax = 100;
            r.f[2].grd = 100, r.f[2].lck = 0, r.f[2].hp = r.f[2].hpMax = 10000, r.f[2].enemy = true;
            cr::BattleActionDef atk;
            atk.id = 10, atk.power = 150, atk.calc = 0, atk.alwaysHits = true, atk.scope = 4, atk.side = 3;
            const int who[2] = {0, -1};
            int normal = 0, crits = 0, other = 0;
            for (uint32_t seed = 1; seed <= 400; ++seed) {
                r.seed(seed);
                const cr::BattleHit h = r.physical(atk, who, 2);
                if (h.type == cr::kHitNormal && h.amount == 300) ++normal;
                else if (h.type == cr::kHitCritical && h.amount == 400) ++crits;
                else ++other;
            }
            bad += other != 0 || normal == 0 || crits == 0 || crits > 60; // 20 in 256
            r.f[0].status[cr::kStatusAttackUp] = 1; // x1.25: (200) x 250 / 100 - 100 = 400
            r.seed(7);
            for (int k = 0; k < 20; ++k) {
                const cr::BattleHit h = r.physical(atk, who, 2);
                bad += h.amount != (h.type == cr::kHitCritical ? 525 : 400);
            }
            r.f[0].status[cr::kStatusAttackUp] = 0;
            r.f[2].guarding = true; // guarding only counts for the pair
            bad += r.grdOf(2) != 100;
            r.f[0].guarding = true, r.f[0].grd = 70;
            bad += r.grdOf(0) != 140;
            // healing: -(0 + 150) x luck 80 / 100 = -120 for the pair's actor 0 casting
            cr::BattleActionDef cure;
            cure.id = 228, cure.power = 150, cure.calc = 12;
            r.f[0].lck = 80;
            bad += r.heal(cure, who, 0).amount != -120;
            // the Break: the first event puts it at 4; the enemies' damage takes 1 an action
            r.breakEvent(0, 2, 2);
            r.catchUpBreak();
            bad += r.breakGauge != 4;
            r.breakFlags &= ~0x3200u;
            r.breakEvent(2, 2, 0);
            r.catchUpBreak();
            bad += r.breakGauge != 3;
            r.breakEvent(2, 8, 0); // its critical too
            r.catchUpBreak();
            bad += r.breakGauge != 2;
            r.breakEvent(0, 2, 2); // the holder's damage doesn't count
            r.catchUpBreak();
            bad += r.breakGauge != 2;
            r.breakEvent(0, 0x10, 2); // its kill does
            r.catchUpBreak();
            bad += r.breakGauge != 3;
            r.breakEvent(2, 0x10, 0), r.breakEvent(2, 0x10, 1); // two kills by the enemies: gone, even
            r.catchUpBreak();
            bad += r.breakGauge != 0 || !(r.breakFlags & 0x400);
            // enemies: Garm (group 1) at band 0 with the pair at level 1: levels 1..3, the row's growth
            cr::BattleRules e;
            e.setupParty(gs, app.itemTable, 0);
            int lo = 99, hi = 0;
            for (uint32_t seed = 1; seed <= 50; ++seed) {
                e.seed(seed);
                e.setupEnemies(app.battleData, app.battleData.group(1), 0);
                lo = std::min(lo, e.f[2].level), hi = std::max(hi, e.f[2].level);
                const cr::MonsterDef* m = e.f[2].def;
                bad += !m || e.f[2].hpMax != int(m->hp) + 30 * e.f[2].level || e.f[2].exp != int(m->exp) + e.f[2].level * e.f[2].level;
            }
            bad += lo != 1 || hi != 3;
            // items: 21 (Chocolate) is action 100, 1000 HP
            const cr::BattleActionDef* choc = app.battleActions.get(21 + 79);
            bad += !choc || choc->calc != 17 || choc->power != 100;
            // escaping: certain while the pair holds the Break
            e.breakGauge = e.breakTarget = 2;
            cr::BattleCommand esc[2];
            esc[0].kind = cr::kCmdEscape;
            e.pair = true;
            e.beginRound(esc, app.battleActions);
            cr::BattleTurn t;
            bool escaped = false;
            while (e.nextTurn(app.battleActions, t))
                if (t.kind == cr::kCmdEscape) escaped = t.escaped;
            bad += !escaped || e.outcome() != 1;
            // genes: the new game's own, a plate's known ones, plate drops, learning, pair MP
            {
                cr::GameState g;
                cr::giveStartingItems(g);
                const std::vector<int> own = cr::usableAbilities(g, 0);
                bad += own != std::vector<int>{81} || !cr::hasSkill(g, 0, 131) || cr::hasSkill(g, 0, 133);
                g.items[481] = 1, g.items[108] = 1;
                g.party[0].genes[1] = 481;
                const std::vector<int> more = cr::usableAbilities(g, 0);
                bad += more.size() != 2 || cr::plateLearned(g, 481) != 1;
                bad += cr::plateDrop(13) != 481 || cr::plateDrop(16) != 482 || cr::plateDrop(72) != 500 || cr::plateDrop(73) != 0;
                int plates = 0, taught = 0;
                for (uint32_t seed = 1; seed <= 40; ++seed) { // monster 200 (model 13) falls to a level-1 attack
                    cr::GameState fresh;
                    cr::giveStartingItems(fresh);
                    cr::BattleRules k;
                    k.seed(seed);
                    k.setupParty(fresh, app.itemTable, 0);
                    std::array<int, 3> one{200, 0, 0};
                    k.setupEnemies(app.battleData, one, 0);
                    k.f[2].hp = 1;
                    cr::BattleCommand cmd[2];
                    cmd[0].kind = cr::kCmdAttack, cmd[0].target = 2;
                    cmd[1].kind = cr::kCmdDefend;
                    k.f[2].lck = -1000; // always hit
                    k.beginRound(cmd, app.battleActions);
                    cr::BattleTurn tt;
                    while (k.nextTurn(app.battleActions, tt)) {}
                    plates += std::count(k.drops.begin(), k.drops.end(), 481) > 0;
                    taught += !k.learned.empty();
                    if (k.outcome() == 2) {
                        k.applyRewards(fresh);
                        bad += fresh.items[481] != 1;
                    }
                }
                bad += plates != 40 || taught == 0 || taught == 40; // the plate every time, a gene 40% of the time
                cr::BattleRules pr; // a pair gene: both members pay its MP
                pr.setupParty(g, app.itemTable, 0);
                pr.setupEnemies(app.battleData, app.battleData.group(1), 0);
                pr.pair = true;
                cr::BattleCommand gc[2];
                gc[0].kind = cr::kCmdGene, gc[0].action = cr::geneAction(94), gc[0].target = 2;
                const int mp0 = pr.f[0].mp, mp1 = pr.f[1].mp;
                const int cost = pr.geneCost(*app.battleActions.get(cr::geneAction(94)), 0); // Musiea's MP 1/2 halves it
                pr.beginRound(gc, app.battleActions);
                cr::BattleTurn gt;
                bool cast = false;
                while (pr.nextTurn(app.battleActions, gt)) cast |= gt.kind == cr::kCmdGene && gt.actors[0] == 0;
                bad += !cast || pr.f[0].mp != mp0 - cost || pr.f[1].mp != mp1 - cost;
                std::printf("genes: new game knows %zu, a plate adds %zu, plates dropped %d/40, genes taught %d/40\n", own.size(),
                            more.size() - own.size(), plates, taught);
            }
            { // 1.0.1 / 2.2.0 decompile checks: levels from 20 grow every stat (pcparam_change_lv),
              // seeds share that bonus's 5000, guests' Speed / Luck are their Defense, a monster
              // teaches the first of its six genes not known (btlcalc_0506)
                int gbad = 0;
                gbad += cr::levelGrowth(20, cr::kHp) != 0 || cr::levelGrowth(21, cr::kHp) != 10 ||
                        cr::levelGrowth(22, cr::kHp) != 30 || cr::levelGrowth(40, cr::kHp) != 2100 ||
                        cr::levelGrowth(99, cr::kHp) != 31600 || cr::levelGrowth(21, cr::kAttack) != 1 ||
                        cr::levelGrowth(99, cr::kLuck) != 3160 || cr::kMaxLevel != 100 ||
                        cr::levelGrowth(100, cr::kHp) != 32400 || cr::levelGrowth(100, cr::kLuck) != 3240;
                cr::GameState cap;
                cap.party[0].level = 99;
                cap.party[0].exp = cr::expForLevel(100) - 1;
                cr::BattleRules capBattle;
                capBattle.setupParty(cap, app.itemTable, 0);
                capBattle.expGained = 1;
                capBattle.applyRewards(cap); // no living enemies: a win's rewards
                gbad += cap.party[0].level != 100 || cap.party[0].exp != cr::expForLevel(100);
                capBattle.applyRewards(cap);
                gbad += cap.party[0].level != 100;
                cr::PartyMember pm;
                pm.level = 99;
                gbad += cr::seedRoom(pm, cr::kAttack) != 5000 - 3160;
                pm.seeds[0] = 5000;
                gbad += cr::statBonus(pm, cr::kAttack) != 5000 || cr::seedRoom(pm, cr::kAttack) != 0;
                cr::GameState fresh;
                cr::giveStartingItems(fresh);
                cr::BattleRules gr;
                gr.setupSpecial(fresh, app.itemTable, 372, 1); // Ayuta's guest
                const bool quirk = gr.f[1].guest && gr.f[1].spd == gr.f[1].grd && gr.f[1].lck == gr.f[1].grd &&
                                   gr.f[1].hpMax == cr::baseStat(gr.f[1].character, cr::kHp) + 50 * gr.f[1].level;
                gbad += !quirk;
                int firstOk = 0, tries = 0; // monster 203 (row +0xb4.. 92 170 89 175 100 182) at level 30
                for (uint32_t seed = 1; seed <= 60 && firstOk == tries; ++seed) {
                    cr::GameState g2;
                    cr::giveStartingItems(g2);
                    g2.items[92] = 1; // the first already known: the second comes next
                    cr::BattleRules k;
                    k.seed(seed);
                    k.setupParty(g2, app.itemTable, 0);
                    std::array<int, 3> one{203, 0, 0};
                    k.setupEnemies(app.battleData, one, 0);
                    k.f[2].level = 30, k.f[2].hp = 1, k.f[2].lck = -1000;
                    cr::BattleCommand cmd[2];
                    cmd[0].kind = cr::kCmdAttack, cmd[0].target = 2;
                    cmd[1].kind = cr::kCmdDefend;
                    k.beginRound(cmd, app.battleActions);
                    cr::BattleTurn tt;
                    while (k.nextTurn(app.battleActions, tt)) {}
                    if (!k.learned.empty()) ++tries, firstOk += k.learned.size() == 1 && k.learned[0] == 170;
                }
                gbad += tries == 0 || firstOk != tries;
                const bool armor = cr::armorSkill(222) == 148 && cr::armorSkill(226) == 149 && cr::armorSkill(227) == 150 &&
                                   cr::armorSkill(228) == 0 && cr::armorSkill(421) == 148 && cr::armorSkill(446) == 0 &&
                                   cr::armorSkill(215) == 0;
                gbad += !armor;
                // _get_random: MT19937 (seed 5489 when unseeded) and the original's bounded draw
                cr::GameRandom gr0, gr1;
                std::mt19937 ref;
                bool mt = true;
                for (int k = 0; k < 1000; ++k) mt &= gr0.next() == uint32_t(ref());
                const int first = gr1.below(10000); // 3499211612 >> 1, less 10000 << 17, below 10000 << 16
                int hist[6] = {};
                cr::GameRandom gr2(77);
                for (int k = 0; k < 60000; ++k) ++hist[gr2.get(6)];
                bool flat = true;
                for (int h : hist) flat &= h > 9500 && h < 10500;
                gbad += !mt || first != 6696 || !flat || gr2.get(0) != 0;
                std::printf("random: MT19937 %s, first draw below 10000 %d (6696), get_random(6) even %s\n", mt ? "yes" : "NO",
                            first, flat ? "yes" : "NO");
                std::printf("level growth: +%d HP / +%d each by level 99, seeds share the 5000, guests' Speed / Luck their "
                            "Defense %s, the first unknown gene taught %d/%d, resisting armors %s\n", cr::levelGrowth(99, cr::kHp),
                            cr::levelGrowth(99, cr::kAttack), quirk ? "yes" : "NO", firstOk, tries, armor ? "yes" : "NO");
                bad += gbad;
            }
            { // btlcalc_0404's Human Genes: Risk Breaker (203) and Trompe-L'oeil (201)
                int rbBad = 0, cancels = 0, rbLanded = 0, rbPaid = 0;
                bool once = true;
                for (uint32_t seed = 1; seed <= 200; ++seed) {
                    cr::GameState fresh;
                    cr::giveStartingItems(fresh);
                    for (int pass = 0; pass < 2; ++pass) {
                        cr::BattleRules k;
                        k.seed(seed);
                        k.setupParty(fresh, app.itemTable, 0);
                        k.setupEnemies(app.battleData, app.battleData.group(1), 0);
                        for (int i = 0; i < 5; ++i)
                            if (k.f[size_t(i)].present) k.f[size_t(i)].mp = k.f[size_t(i)].mpMax = 999;
                        k.f[0].spd = 9999;            // acts first
                        k.f[2].lck = -1000;           // always hit
                        k.f[2].hp = k.f[2].hpMax = 99999;
                        k.breakStarted = true;
                        k.breakGauge = k.breakTarget = 4; // the pair holds the Break
                        cr::BattleCommand cmd[2];
                        cmd[0].kind = cr::kCmdGene, cmd[0].action = pass ? 201 : 203, cmd[0].target = 2;
                        cmd[1].kind = cr::kCmdDefend;
                        const int hp0 = k.f[0].hp;
                        k.beginRound(cmd, app.battleActions);
                        cr::BattleTurn tt;
                        bool enemyActed = false, cancelled = false;
                        while (k.nextTurn(app.battleActions, tt)) {
                            if (tt.actors[0] == 2) enemyActed = true;
                            if (tt.actors[0] != 0) continue;
                            int self = -1, dealt = 0;
                            for (const cr::BattleHit& h : tt.hits)
                                if (h.target == 0) self = h.amount;
                                else dealt = std::max(dealt, h.amount);
                            if (pass == 0 && dealt > 0) ++rbLanded, rbPaid += self == hp0 / 4 && k.f[0].hp == hp0 - hp0 / 4;
                            if (pass == 0 && dealt <= 0) rbBad += self != -1; // nothing landed: nothing paid
                            for (const cr::BattleNotice& n : tt.notices) cancelled |= n.entry == 3 && n.fighter == 2;
                        }
                        if (pass == 1 && cancelled) {
                            ++cancels;
                            once &= !enemyActed;
                            for (int r = 0; r < 20; ++r) { // never again this battle
                                k.f[2].hp = k.f[2].hpMax;
                                k.beginRound(cmd, app.battleActions);
                                while (k.nextTurn(app.battleActions, tt))
                                    for (const cr::BattleNotice& n : tt.notices) once &= n.entry != 3;
                            }
                        }
                    }
                }
                rbBad += rbLanded == 0 || rbPaid != rbLanded || cancels == 0 || cancels > 60 || !once;
                std::printf("human genes: Risk Breaker cost a quarter of the HP %d/%d times, Trompe-L'oeil cancelled %d/200 "
                            "(once a battle %s)\n", rbPaid, rbLanded, cancels, once ? "yes" : "NO");
                bad += rbBad;
            }
            // skills: the damage scaling, criticals, MP, healing, regen, the Gates, level-up boosts
            {
                int sbad = 0;
                cr::BattleRules q;
                for (cr::Fighter& x : q.f) x = cr::Fighter();
                q.f[0].present = q.f[2].present = true;
                q.f[0].atk = 200, q.f[0].spd = 100, q.f[0].level = q.f[2].level = 1, q.f[0].hp = q.f[0].hpMax = 100;
                q.f[2].grd = 100, q.f[2].lck = -1000, q.f[2].hp = q.f[2].hpMax = 10000, q.f[2].enemy = true;
                cr::BattleActionDef hit;
                hit.id = 10, hit.power = 150, hit.calc = 0, hit.alwaysHits = true;
                const int who[2] = {0, -1};
                auto plain = [&](int want, bool critOnly) { // every non-critical (or critical) hit equals `want`
                    for (uint32_t seed = 1; seed <= 300; ++seed) {
                        q.seed(seed);
                        const cr::BattleHit h = q.physical(hit, who, 2);
                        if ((h.type == cr::kHitCritical) == critOnly && h.amount != want) return false;
                    }
                    return true;
                };
                q.f[2].skills[146 - 131] = true; // Physical Resistance: 300 x 75%
                sbad += !plain(225, false);
                q.f[0].skills[139 - 131] = true; // and Physical Attack: back to 100%
                sbad += !plain(300, false);
                q.f[2].skills[146 - 131] = q.f[0].skills[139 - 131] = false;
                q.f[0].skills[137 - 131] = true; // Critical Pierce: (250) x 2 - 0
                sbad += !plain(500, true);
                q.f[0].skills[134 - 131] = true; // and Boost Critical: x2
                sbad += !plain(1000, true);
                q.f[0].skills[137 - 131] = q.f[0].skills[134 - 131] = false;
                q.f[0].skills[135 - 131] = true; // Critical First Attack: once
                q.seed(9);
                sbad += q.physical(hit, who, 2).type != cr::kHitCritical;
                q.f[0].firstAttack = false;
                q.f[0].skills[135 - 131] = false;
                cr::BattleActionDef gene;
                gene.mp = 20;
                q.f[0].skills[132 - 131] = true;
                sbad += q.geneCost(gene, 0) != 10;
                cr::BattleActionDef cure;
                cure.power = 150, cure.calc = 12;
                q.f[0].lck = 80, q.f[0].skills[155 - 131] = true;
                sbad += q.heal(cure, who, 0).amount != -240;
                q.f[0].hp = 50, q.f[0].skills[133 - 131] = true; // regen: +5 of 100
                q.endRound();
                sbad += q.f[0].hp != 55;
                // the Gates: Dragon Gate Stage 1 (175, on plate 482) adds its item's Attack
                cr::GameState g;
                cr::giveStartingItems(g);
                const int base = cr::memberStat(g, 0, cr::kAttack, app.itemTable);
                g.items[482] = 1, g.items[175] = 1;
                g.party[0].genes[1] = 482;
                const cr::ItemParam* gate = app.itemTable.item(175);
                sbad += !gate || cr::memberStat(g, 0, cr::kAttack, app.itemTable) != base + gate->stat[cr::kAttack];
                g.items[164] = 1; // HP Boost isn't on a worn plate: nothing
                cr::levelUpBoosts(g, 0, 3);
                sbad += g.party[0].boosts[0] != 0;
                g.items[166] = 1, g.items[483] = 1, g.party[0].genes[2] = 483; // Attack Boost (plate 483): +1 a level
                cr::levelUpBoosts(g, 0, 3);
                sbad += g.party[0].boosts[2] != 3;
                // the other calcs, each through a round: member 0 uses the action on its target
                auto runAs = [&](int action, int target, cr::BattleRules& rr) {
                    for (cr::Fighter& x : rr.f) x = cr::Fighter();
                    for (int i : {0, 1, 2, 3}) {
                        cr::Fighter& x = rr.f[size_t(i)];
                        x.present = true, x.enemy = i >= 2, x.level = 1, x.hp = x.hpMax = 5000, x.mp = x.mpMax = 400;
                        x.atk = 100, x.grd = 0, x.spd = i == 0 ? 10000 : 1, x.lck = 0, x.character = i < 2 ? i : -1;
                    }
                    rr.purse = 1000;
                    cr::BattleCommand cc[2];
                    cc[0].kind = cr::kCmdGene, cc[0].action = action, cc[0].target = target;
                    cc[1].kind = cr::kCmdDefend;
                    rr.beginRound(cc, app.battleActions);
                    cr::BattleTurn tt, first;
                    bool got = false;
                    while (rr.nextTurn(app.battleActions, tt))
                        if (!got && tt.actors[0] == 0) first = tt, got = true;
                    return first;
                };
                int cbad = 0;
                { cr::BattleRules rr; runAs(402, 2, rr); if (rr.f[2].hp != 4000) std::printf("  fixed: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(433, 2, rr); if (rr.f[2].hp != 3000 || rr.f[3].hp != 5000) std::printf("  shared: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(432, 2, rr); if (rr.f[2].hp != 1) std::printf("  to1: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(404, 2, rr); if (rr.f[2].hp != 3500) std::printf("  pct: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(403, 2, rr); if (rr.f[2].mp != 300) std::printf("  mp: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(409, 2, rr); if (rr.ozStolen != 100 || rr.purse != 900) std::printf("  oz: hp %d %d mp %d oz %d\n", rr.f[2].hp, rr.f[3].hp, rr.f[2].mp, rr.ozStolen), ++cbad; }
                { cr::BattleRules rr; runAs(109, 0, rr); cbad += !rr.doubleExp; }
                { // a barrier sends the next hostile gene back on its caster, then is gone
                    cr::BattleRules rr;
                    runAs(212, 2, rr);
                    const int plain = 5000 - rr.f[2].hp;
                    cr::BattleRules rc;
                    for (cr::Fighter& x : rc.f) x = cr::Fighter();
                    for (int i : {0, 1, 2, 3}) {
                        cr::Fighter& x = rc.f[size_t(i)];
                        x.present = true, x.enemy = i >= 2, x.level = 1, x.hp = x.hpMax = 5000, x.mp = x.mpMax = 400;
                        x.atk = 100, x.spd = i == 0 ? 10000 : 1, x.character = i < 2 ? i : -1;
                    }
                    rc.f[2].status[cr::kStatusBarrier] = 5;
                    cr::BattleCommand cc[2];
                    cc[0].kind = cr::kCmdGene, cc[0].action = 212, cc[0].target = 2;
                    cc[1].kind = cr::kCmdDefend;
                    rc.beginRound(cc, app.battleActions);
                    cr::BattleTurn tt;
                    int hp2 = -1, hp0 = -1;
                    bool back = false;
                    while (rc.nextTurn(app.battleActions, tt))
                        if (tt.actors[0] == 0) hp2 = rc.f[2].hp, hp0 = rc.f[0].hp, back = tt.reflected;
                    if (plain <= 0 || hp2 != 5000 || hp0 >= 5000 || !back || rc.f[2].status[cr::kStatusBarrier] != 0)
                        std::printf("  barrier: plain %d, with %d, caster %d, left %d\n", plain, hp2, hp0, rc.f[2].status[cr::kStatusBarrier]), ++cbad;
                }
                { // revive: a fallen member back with half its HP
                    cr::BattleRules rr;
                    for (cr::Fighter& x : rr.f) x = cr::Fighter();
                    for (int i : {0, 1, 2}) {
                        cr::Fighter& x = rr.f[size_t(i)];
                        x.present = true, x.enemy = i >= 2, x.level = 1, x.hp = x.hpMax = 1000, x.mp = x.mpMax = 400;
                        x.atk = 100, x.spd = i == 0 ? 10000 : 1, x.character = i < 2 ? i : -1;
                    }
                    rr.f[1].hp = 0;
                    cr::BattleCommand cc[2];
                    cc[0].kind = cr::kCmdGene, cc[0].action = 230, cc[0].target = 1;
                    rr.beginRound(cc, app.battleActions);
                    cr::BattleTurn tt;
                    while (rr.nextTurn(app.battleActions, tt)) {}
                    cbad += rr.f[1].hp != 500;
                }
                { // poison: 10% a round; invincible: no damage
                    cr::BattleRules rr;
                    for (cr::Fighter& x : rr.f) x = cr::Fighter();
                    rr.f[2].present = true, rr.f[2].hp = rr.f[2].hpMax = 1000, rr.f[2].status[cr::kStatusPoison] = 5;
                    rr.f[0].present = true, rr.f[0].hp = rr.f[0].hpMax = 1000;
                    rr.endRound();
                    cbad += rr.f[2].hp != 900;
                }
                { // story line-ups and the guest's rotation
                    cr::GameState g;
                    cr::giveStartingItems(g);
                    cr::BattleRules a;
                    a.setupParty(g, app.itemTable, 0);
                    a.setupSpecial(g, app.itemTable, 371, 0);
                    cbad += a.f[0].character != 0 || a.f[1].present;
                    cr::BattleRules gb;
                    gb.setupParty(g, app.itemTable, 0);
                    gb.setupSpecial(g, app.itemTable, 370, 0);
                    cbad += !gb.allyAuto || gb.f[1].character != 4 || gb.f[1].level != gb.f[0].level + 5 || !gb.f[1].guest;
                    gb.setupEnemies(app.battleData, app.battleData.group(370), 0);
                    { // the enemies' levels from the pair, before the guest takes the partner's place
                        cr::GameState lg = g;
                        lg.party[0].level = 10, lg.party[1].level = 20;
                        cr::BattleRules lb;
                        lb.setupParty(lg, app.itemTable, 0);
                        lb.setupSpecial(lg, app.itemTable, 370, 0);
                        cbad += lb.leaderLevel() != 15 || lb.f[1].level != 15;
                    }
                    cr::BattleCommand cc[2];
                    cc[0].kind = cr::kCmdDefend;
                    int ally[3] = {0, 0, 0};
                    for (int r = 0; r < 3; ++r) {
                        gb.f[2].hp = gb.f[2].hpMax = 99999;
                        if (gb.f[3].present) gb.f[3].hp = gb.f[3].hpMax = 99999;
                        gb.f[1].hp = gb.f[1].hpMax, gb.f[0].hp = gb.f[0].hpMax, gb.f[1].mp = gb.f[1].mpMax;
                        gb.beginRound(cc, app.battleActions);
                        cr::BattleTurn tt;
                        while (gb.nextTurn(app.battleActions, tt))
                            if (tt.actors[0] == 1 && !ally[r]) ally[r] = tt.action;
                        gb.endRound();
                    }
                    if (ally[0] != 212 || ally[1] != 14 || ally[2] != 14)
                        std::printf("  guest rotation %d %d %d\n", ally[0], ally[1], ally[2]), ++cbad;
                    cr::BattleRules zb;
                    zb.setupParty(g, app.itemTable, 1);
                    zb.setupSpecial(g, app.itemTable, 372, 1);
                    cbad += zb.f[0].character != 3 || zb.f[1].character != 6 || zb.allyScript != 2;
                }
                { // a monster with row +0x3e 2 acts twice; Asura Lotus strikes 2..5 times
                    cr::BattleRules rr;
                    cr::GameState g;
                    rr.setupParty(g, app.itemTable, 0);
                    std::array<int, 3> one{81, 0, 0};
                    rr.setupEnemies(app.battleData, one, 0);
                    rr.f[0].hp = rr.f[0].hpMax = rr.f[1].hp = rr.f[1].hpMax = 99999;
                    rr.f[2].hp = rr.f[2].hpMax = 99999;
                    cr::BattleCommand cc[2];
                    cc[0].kind = cc[1].kind = cr::kCmdDefend;
                    rr.beginRound(cc, app.battleActions);
                    cr::BattleTurn tt;
                    int turns = 0;
                    while (rr.nextTurn(app.battleActions, tt)) turns += tt.actors[0] == 2;
                    if (turns != 2) std::printf("  twice-acting monster acted %d times\n", turns), ++cbad;
                    cr::BattleRules lo;
                    int fewest = 99, most = 0;
                    for (uint32_t seed = 1; seed <= 30; ++seed) {
                        lo.seed(seed);
                        int n = 0;
                        runAs(207, 2, lo);
                        cr::BattleRules q2;
                        q2.seed(seed);
                        for (cr::Fighter& x : q2.f) x = cr::Fighter();
                        for (int i : {0, 1, 2}) {
                            cr::Fighter& x = q2.f[size_t(i)];
                            x.present = true, x.enemy = i >= 2, x.level = 1, x.hp = x.hpMax = 99999, x.mp = x.mpMax = 400;
                            x.atk = 10, x.spd = i == 0 ? 10000 : 1, x.character = i < 2 ? i : -1;
                        }
                        cr::BattleCommand c2[2];
                        c2[0].kind = cr::kCmdGene, c2[0].action = 207, c2[0].target = 2;
                        c2[1].kind = cr::kCmdDefend;
                        q2.beginRound(c2, app.battleActions);
                        while (q2.nextTurn(app.battleActions, tt)) n += tt.actors[0] == 0;
                        fewest = std::min(fewest, n), most = std::max(most, n);
                    }
                    if (fewest != 2 || most != 5) std::printf("  Asura Lotus %d..%d times\n", fewest, most), ++cbad;
                }
                { // Steal Item: monster 1's item, once
                    int got = 0, twice = 0;
                    for (uint32_t seed = 1; seed <= 30; ++seed) {
                        cr::BattleRules rr;
                        rr.seed(seed);
                        cr::GameState g;
                        rr.setupParty(g, app.itemTable, 0);
                        std::array<int, 3> one{1, 0, 0};
                        rr.setupEnemies(app.battleData, one, 0);
                        rr.f[2].hp = rr.f[2].hpMax = 99999, rr.f[2].lck = 0;
                        rr.f[0].lck = 500;
                        for (int r = 0; r < 2; ++r) {
                            cr::BattleCommand cc[2];
                            cc[0].kind = cr::kCmdGene, cc[0].action = 223, cc[0].target = 2;
                            cc[1].kind = cr::kCmdDefend;
                            rr.beginRound(cc, app.battleActions);
                            cr::BattleTurn tt;
                            while (rr.nextTurn(app.battleActions, tt)) {}
                            rr.endRound();
                        }
                        got += !rr.stolen.empty() && rr.stolen[0] == 3;
                        twice += rr.stolen.size() > 1;
                    }
                    if (got == 0 || twice) std::printf("  steal: %d got, %d twice\n", got, twice), ++cbad;
                }
                std::printf("calcs: fixed, split by targets, to 1, %%HP, MP, Oz stolen, Apples, barrier, revive, poison: %d problems\n", cbad);
                sbad += cbad;
                { // btlcalc_0404: resistance 1 is immune; an attacker without an element meets no element resistance
                    cr::MonsterDef md;
                    cr::BattleRules q;
                    for (cr::Fighter& x : q.f) x = cr::Fighter();
                    q.f[0].present = q.f[2].present = true;
                    q.f[0].atk = 200, q.f[0].spd = 100, q.f[0].level = q.f[2].level = 1, q.f[0].hp = q.f[0].hpMax = 100;
                    q.f[2].hp = q.f[2].hpMax = 10000, q.f[2].enemy = true, q.f[2].def = &md;
                    cr::BattleActionDef hit;
                    hit.id = 10, hit.power = 150, hit.calc = 0, hit.alwaysHits = true, hit.scope = 4, hit.side = 3;
                    const int who[2] = {0, -1};
                    md.raw[0x10] = 1;
                    q.seed(3);
                    int zbad = q.physical(hit, who, 2).amount != 0;
                    md.raw[0x10] = 0, md.raw[0x14] = 1;
                    q.seed(3);
                    zbad += q.physical(hit, who, 2).amount == 0;
                    if (zbad) std::printf("  resistances: %d problems\n", zbad);
                    sbad += zbad;
                }
                std::printf("skills: resistance, attack, pierce, boost critical, first attack, MP 1/2, recovery, regen, "
                            "Gate +%d, boosts: %d problems\n", gate ? gate->stat[cr::kAttack] : 0, sbad);
                bad += sbad;
            }
            std::printf("battle rules: %d physical hits (%d critical) as worked out, healing, Break, enemy levels %d..%d, "
                        "items, escape: %d problems\n", normal + crits, crits, lo, hi, bad);
            problems += bad;
        }

        // Every effect (eff.bin's 98): loaded, played on an enemy, drawn without render API errors
        {
            int loaded = 0, played = 0, ferrs = 0;
            startBattle(app, {1, 0, -1, 0}, 0);
            if (app.battle.active) {
                for (int id = 0; id < 100; ++id) {
                    if (!effectModel(app, id)) continue;
                    ++loaded;
                    spawnEffect(app, id, 2);
                    played += !app.battle.effects.empty();
                    for (int f = 0; f < 6; ++f) stepBattle(app);
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    ferrs += app.renderer.takeErrors();
                    app.battle.effects.clear();
                }
                endBattle(app, false);
                app.runner.stop();
            }
            std::printf("battle effects: %d of eff.bin's entries loaded, %d played, %d render API errors\n", loaded, played, ferrs);
            problems += (loaded < 98) + (played < loaded) + ferrs;
            glErrors += ferrs;
        }

        // The bosses' staged actions (btlact_430..436): each plays its own slot (the boss models'
        // 14th / 15th, read past the usual 13), steps through its cameras, flashes and shakes
        // where it has them, hits at its frame and ends after its wait; hits shake the screen;
        // and the effects' textures scroll with their materials' actions
        {
            // quake: one before the hit; after: the strongest from the hit on (3 = the calc's own, when a hit lands)
            struct Want { int action, group, hit, wait, cams; bool flash, quake; float after; };
            const Want wants[] = {{430, 340, 140, 40, 3, true, true, 3},    {432, 348, -1, 40, 5, false, false, 0},
                                  {433, 352, 112, 40, 0, false, false, 0}, {434, 352, 130, 65, 2, true, true, 3},
                                  {435, 356, 163, 40, 5, true, false, 3},   {436, 356, 61, 50, 4, false, false, 10}};
            int sbad = 0, rerrs = 0, ran = 0, hitQuakes = 0;
            const cr::GameState saved = app.game;
            for (const Want& w : wants) {
                app.game = saved;
                startBattle(app, {w.group, 0, -1, 0}, 0);
                App::Battle& b = app.battle;
                const cr::BattleActionDef* def = app.battleActions.get(w.action);
                if (!b.active || !def) { ++sbad; continue; }
                b.rules.forceAction = w.action;
                int hitAt = -1, endAt = -1, cams = 0, slot = -1;
                float flash = 0, quake = 0, after = 0;
                bool started = false, landed = false;
                for (int f = 0; f < 8000 && b.active && endAt < 0; ++f) {
                    if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) { // the pair defends
                        battleChooseTop(app, 0);
                        battleChooseCommand(app, 3);
                        if (b.menu == App::Battle::Commands) battleChooseCommand(app, 3);
                    }
                    const bool was = b.turnOn && b.turn.action == w.action;
                    stepBattle(app);
                    const bool on = b.turnOn && b.turn.action == w.action;
                    if (on && !started) {
                        started = true;
                        if (const App::Battle::Actor* u = battleActor(app, b.turn.actors[0])) slot = u->slot;
                    }
                    if (on) {
                        cams = std::max(cams, b.stageCams);
                        flash = std::max(flash, b.flash.on ? b.flash.alpha : 0.0f);
                        if (!b.hitsShown) quake = std::max({quake, std::fabs(b.quake.off[0]), std::fabs(b.quake.off[1])});
                        else if (b.quake.mode) after = std::max(after, b.quake.amp);
                        if (b.hitsShown && hitAt < 0) {
                            hitAt = b.turnFrame;
                            for (const cr::BattleHit& h : b.turn.hits) // btlcalc_0404 / 0406 shake for a hit that lands
                                landed |= h.type != cr::kHitGuard && h.type != cr::kHitMiss && (def->calc == 1 ? h.amount >= 0 : h.amount > 0);
                        }
                        if (b.turnFrame == 20 || b.turnFrame == hitAt) {
                            app.lastTicks = 0;
                            drawFrame(app, false);
                            rerrs += app.renderer.takeErrors();
                        }
                    }
                    if (was && !b.turnOn) endAt = b.turnFrame;
                }
                const float wantAfter = w.after > 3 ? w.after : w.after > 0 && landed ? 3.0f : 0.0f;
                const int wantHit = w.hit >= 0 ? w.hit : app.battleData.anim(battleActor(app, 2)->modelId, def->cls).loopStart + 45;
                const int bad = !started + (slot != def->cls) + (hitAt != wantHit) + (endAt != wantHit + w.wait) +
                                (cams != w.cams) + (w.flash != (flash >= 254)) + (w.quake != (quake > 0.5f)) + (after != wantAfter);
                if (bad)
                    std::printf("  staged %d: slot %d (cls %d), hit at %d (want %d), ended at %d, %d cameras, flash %.0f, "
                                "quake %.1f, after the hit %.0f\n", w.action, slot, def->cls, hitAt, wantHit, endAt, cams, flash,
                                quake, after);
                sbad += bad;
                ran += started;
                endBattle(app, false);
                app.runner.stop();
            }
            { // The battle scene as 2.2.0's (CRO's 54a299d .. 0150c2d, checked against CR1's code): the encounter's
              // white and btl_main 200 / 300 / 400 / 499 (the commands at 32, an ambush's round at 31), the strike
              // banner's name, FUN_0000bcc4's sound, read_btlcpl's idle phases, _btl_shadow's fans, the casts' loops,
              // mdl_put's turning, the gauge every other frame, the banner a frame late
                app.game = saved;
                int sp = 0;
                std::string why;
                auto check = [&](bool ok, const char* what) { if (!ok) ++sp, why += std::string(" ") + what; };
                auto near = [](float x, float y, float e = 0.01f) { return std::fabs(x - y) < e; };
                check(battleInSound(1) == 0x17 && battleInSound(9) == 0x18 && battleInSound(0x2b) == 0x18 && battleInSound(0xae) == 0x17 &&
                          battleInSound(0x105) == 0x19 && battleInSound(0x107) == 0x17 && battleInSound(0x11b) == 0x19 &&
                          battleInSound(0x15f) == 0x18 && battleInSound(0x165) == 0x1a && battleInSound(0x170) == 0x19 &&
                          battleInSound(0x171) == 0x17 && battleInSound(0x179) == 0x18,
                      "in-eff-table");
                int group = 1, idleMonsters = 0, toCommand = 0, ambushAt = 0, shadowsDrawn = 0, castFrames = 0, turnDeg = 0, turnFrames = 0;
                float peakStep = 0, lastStep = 0;
                std::string slideSeq;
                for (int g = 1; g <= app.battleData.groupCount(); ++g) { // two or more monsters, not formation 2
                    const std::array<int, 3> ids = app.battleData.group(g);
                    const cr::MonsterDef *m0 = app.battleData.monster(ids[0]), *m1 = app.battleData.monster(ids[1]);
                    if (m0 && m1 && m0->model >= 0 && m1->model >= 0 && m0->model < cr::kNumBattleModels && m1->model < cr::kNumBattleModels &&
                        app.battleData.formation(ids) != 2) {
                        group = g;
                        break;
                    }
                }
                std::string strikeBanner;
                App::Battle& b = app.battle;
                app.soundLog.clear();
                app.rng.seed(5);
                startBattle(app, {group, 0, -1, 0}, 0);
                if (b.active) {
                    check(std::find(app.soundLog.begin(), app.soundLog.end(), uint32_t(battleInSound(group))) != app.soundLog.end(), "in-eff-played");
                    std::set<int> seen;
                    bool inRange = true;
                    for (const App::Battle::Actor& a : b.actors)
                        if (a.fighter >= 2) seen.insert(int(a.time)), inRange &= a.time >= 0 && a.time < std::max(1.0f, actorDuration(app, a) - 1.0f), ++idleMonsters;
                    check(idleMonsters >= 2 && seen.size() >= 2 && inRange, "idle-phases");
                    bool white = true;
                    for (int f = 1; f < kIntroWhite; ++f) stepBattle(app), white &= app.fadeFrom == 1 && app.fadeTo == 1 && app.fadeGrey == 1;
                    check(white, "white-held");
                    const int fm = std::max(0, b.formation - 2);
                    stepBattle(app); // 200
                    check(app.fadeFrom == 1 && app.fadeTo == 0 && app.fadeT == 0 && app.fadeFrames == 20 && b.cam == fm && b.camActor == -1, "state-200");
                    stepBattle(app); // 300's first
                    const cr::BattleCamera* fc = app.battleData.camera(fm);
                    check(fc && b.cam == fm && b.camActor == 0 && b.camDur == fc->raw[15] && b.camFrame == 1 && b.camSnapped, "state-300");
                    if (fc) turnDeg = fc->raw[14], turnFrames = fc->raw[15];
                    // the turn cut to on its first frame, then eased in by a fifth a frame: the eye's steps grow to
                    // the turn's last frame and fall after, settled before the commands
                    std::vector<float> steps;
                    cr::Vec3 last = b.camEye;
                    const int sets = b.camSets;
                    toCommand = kIntroWhite + 1;
                    for (; toCommand < 100 && b.phase == App::Battle::Intro; ++toCommand) {
                        stepBattle(app);
                        if (b.phase != App::Battle::Intro) break;
                        const cr::Vec3 d = b.camEye - last;
                        steps.push_back(std::sqrt(cr::dot(d, d)));
                        last = b.camEye;
                    }
                    ++toCommand;
                    check(toCommand == kIntroWhite + 32 && b.cam == b.formation + 12 && b.camSets == sets + 1 && b.camSnapped, "commands-at-32");
                    // the first page slides in a quarter of its distance a frame, taking taps once in
                    for (int f = 0; f < 6 && !battleMenuReady(app); ++f) {
                        slideSeq += (slideSeq.empty() ? "" : " ") + std::to_string(b.slide);
                        stepBattle(app);
                    }
                    slideSeq += " " + std::to_string(b.slide);
                    check(battleMenuReady(app) && slideSeq == (battleSlideDistance(app) == 320 ? "320 240 160 80 0" : "480 360 240 120 0"), "page-slide");
                    if (turnDeg != 0 && turnFrames > 2 && int(steps.size()) > turnFrames + 8) {
                        const size_t top = size_t(turnFrames - 2);
                        bool rising = true, falling = true;
                        for (size_t i = 1; i <= top; ++i) rising &= steps[i] > steps[i - 1];
                        for (size_t i = top + 2; i < steps.size(); ++i) falling &= steps[i] <= steps[i - 1];
                        peakStep = steps[top], lastStep = steps.back();
                        check(rising && falling && lastStep < peakStep * 0.05f, "intro-ease");
                    }
                    // the shadows: a fan per fighter standing, 204, hidden none
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    shadowsDrawn = int(b.shadows.size());
                    const App::Battle::Shadow* s0 = nullptr;
                    for (const App::Battle::Shadow& sh : b.shadows)
                        if (sh.fighter == 0) s0 = &sh;
                    App::Battle::Actor* m0 = battleActor(app, 0);
                    check(s0 && near(s0->alpha, 204.0f / 255.0f, 0.001f) && s0->rx > 0 && s0->rz > 0 && m0 && near(s0->at.y, m0->pos.y + 0.1f) &&
                              b.shadowTris.size() == b.shadows.size() * 32 * 9,
                          "shadow-member");
                    if (m0) {
                        m0->hidden = true;
                        drawFrame(app, false);
                        for (const App::Battle::Shadow& sh : b.shadows) check(sh.fighter != 0, "shadow-hidden");
                        m0->hidden = false;
                    }
                    // mdl_put's turning: 15 degrees a frame; an attacker's wanted yaw, not snapped; home over 3
                    if (m0) {
                        const float y0 = m0->yaw, quarter = 3.14159265f / 2.0f, deg = 3.14159265f / 180.0f, full = 2 * 3.14159265f;
                        m0->wantYaw = y0 + quarter;
                        turnActors(app);
                        bool ok = near(std::remainder(m0->yaw - y0, full), 15 * deg, 1e-4f);
                        for (int f = 0; f < 5; ++f) turnActors(app);
                        ok &= near(std::remainder(m0->yaw - (y0 + quarter), full), 0, 1e-4f) && m0->turnStep == 15;
                        m0->yaw = m0->wantYaw = y0;
                        faceFighter(app, *m0, 2);
                        ok &= m0->yaw == y0;
                        turnOver(*m0, y0, 3);
                        for (int f = 0; f < 3; ++f) turnActors(app);
                        ok &= near(std::remainder(m0->yaw - y0, full), 0, 1e-4f);
                        check(ok, "turning");
                    }
                    // a gene's cast: the slot played to its loop's end, then that loop over and over
                    int gene = -1;
                    for (int id = 1; id < app.battleActions.count() && gene < 0; ++id)
                        if (const cr::BattleActionDef* d = app.battleActions.get(id); d && d->cls == 11 && d->motion != 1) gene = id;
                    if (m0 && gene >= 0) {
                        const int slot = realSlot(b.models[m0->model], 11);
                        const cr::BattleAnim ca = app.battleData.anim(m0->modelId, slot);
                        b.turn = cr::BattleTurn();
                        b.turn.kind = cr::kCmdGene, b.turn.action = gene, b.turn.actors[0] = 0;
                        startTurn(app);
                        bool ok = m0->slot == slot && m0->then == -1;
                        if (ca.loopEnd > ca.loopStart) {
                            ok &= m0->loopFrom == ca.loopStart && m0->loopTo == ca.loopEnd;
                            float top = 0, prev = m0->time;
                            bool wrapped = false;
                            b.phase = App::Battle::Command, b.turnOn = false; // (the actor alone steps on)
                            for (int f = 0; f < ca.loopEnd + 2 * (ca.loopEnd - ca.loopStart) + 4; ++f, ++castFrames) {
                                stepBattle(app);
                                top = std::max(top, m0->time);
                                wrapped |= m0->time < prev;
                                prev = m0->time;
                            }
                            ok &= wrapped && top < float(ca.loopEnd) && m0->slot == slot && m0->time >= float(ca.loopStart);
                        } else
                            castFrames = -1;
                        check(ok, "cast-loop");
                    }
                    // the badge's gauge: a step every other frame
                    b.phase = App::Battle::Command;
                    b.rules.breakGauge = 4, b.gaugeShown = 0;
                    for (int f = 0; f < 4; ++f) stepBattle(app);
                    check(b.gaugeShown == 2, "gauge-every-other");
                    b.rules.breakGauge = 0, b.gaugeShown = 0;
                    // the banner a frame late
                    setBanner(app, "x");
                    const bool before = b.bannerShown.empty() || b.bannerShown != "x";
                    stepBattle(app);
                    check(before && b.bannerShown == "x", "banner-late");
                    endBattle(app, false);
                    // ambushed: 400 goes to the enemies' round at 31, no menu
                    app.rng.seed(5);
                    startBattle(app, {group, 0, -1, 2}, 0);
                    for (; ambushAt < 100 && b.phase == App::Battle::Intro; ++ambushAt) stepBattle(app);
                    check(ambushAt == kIntroWhite + 31 && b.phase == App::Battle::Acting, "ambush-400");
                    endBattle(app, false);
                    // a first strike: "<the lead>'s team attacks first!" (var 0 the lead's name), kept over the first page
                    startBattle(app, {group, 0, -1, 1}, 0);
                    for (int f = 0; f < kIntroWhite; ++f) stepBattle(app);
                    const App::Battle::Actor* lead = battleActor(app, 0);
                    strikeBanner = b.banner;
                    check(lead && !lead->name.empty() && b.banner == sysText(app, 57054, lead->name) && b.banner.rfind(lead->name, 0) == 0, "strike-banner");
                    for (int f = 0; f < 40 && b.phase == App::Battle::Intro; ++f) stepBattle(app);
                    check(b.phase == App::Battle::Command && b.banner == strikeBanner && b.bannerFrames > 0, "strike-banner-kept");
                    endBattle(app, false);
                } else
                    check(false, "no-battle");
                app.runner.stop();
                app.game = saved;
                sp += app.renderer.takeErrors();
                std::printf("battle scene: the opening (white %d steps, 200's fade and still camera, 300's turning one (%d degrees over %d, "
                            "eased: eye steps up to %.2f, the last %.3f), the commands at %d (the page in: %s), an ambush's round at %d, \"%s\"), start sound, idle phases (%d monsters), shadows (%d fans at 204), cast loop (%d frames), "
                            "turning 15/frame, the gauge every other frame, the banner a frame late; %d problems%s\n",
                            kIntroWhite, turnDeg, turnFrames, peakStep, lastStep, toCommand - kIntroWhite, slideSeq.c_str(), ambushAt - kIntroWhite, strikeBanner.c_str(), idleMonsters, shadowsDrawn, castFrames, sp,
                            why.c_str());
                problems += sp;
            }
            { // The battle's cameras as 2.2.0's (CRO's 34d96cb / e7b084d, checked against CR1's code):
              // _btlact_attack_init's cameras (attacker +0x84, target +0x8e) and spots, the attack's second
              // camera, the gene / item hit frame and hit cameras, the roll, the target cursor's easing, the
              // cancelled action's camera, 10001 / 10002 ending, the boss fall's hold, the white-out's -100
                app.game = saved;
                int cp = 0;
                std::string why;
                auto check = [&](bool ok, const char* what) { if (!ok) ++cp, why += std::string(" ") + what; };
                int group = 1;
                for (int g = 1; g <= app.battleData.groupCount(); ++g) { // two or more monsters, not formation 2
                    const std::array<int, 3> ids = app.battleData.group(g);
                    const cr::MonsterDef *m0 = app.battleData.monster(ids[0]), *m1 = app.battleData.monster(ids[1]);
                    if (m0 && m1 && m0->model >= 0 && m1->model >= 0 && m0->model < cr::kNumBattleModels &&
                        m1->model < cr::kNumBattleModels && app.battleData.formation(ids) != 2) {
                        group = g;
                        break;
                    }
                }
                App::Battle& b = app.battle;
                app.rng.seed(7);
                startBattle(app, {group, 0, -1, 0}, 0);
                std::string spots;
                if (b.active) {
                    for (int f = 0; f < kIntroWhite + 60 && b.active && b.phase == App::Battle::Intro; ++f) stepBattle(app);
                    const bool keepPair = b.rules.pair, keepEPair = b.rules.enemyPair;
                    b.rules.pair = b.rules.enemyPair = false;
                    const int s2 = enemyCameraSlot(app, 2);
                    // B1: the attacker's (+0x84) and the target's (+0x8e) parts of the camera
                    cr::BattleTurn t;
                    t.kind = cr::kCmdAttack, t.action = b.rules.attackAction(0), t.actors[0] = 0;
                    t.hits.resize(1);
                    t.hits[0].target = 2;
                    auto atk = [&](int user, int target, bool pair, bool epair) {
                        b.rules.pair = pair, b.rules.enemyPair = epair;
                        t.actors[0] = user, t.actors[1] = pair && user < 2 ? 1 : epair && user >= 2 ? 3 : -1, t.hits[0].target = target;
                        cameraForTurn(app, t);
                        b.rules.pair = b.rules.enemyPair = false;
                        return b.attackVariant >= 0 && b.attackVariant < 3 ? b.cam : -1;
                    };
                    check(atk(0, 2, false, false) == s2 * 4 + 0x78 + b.attackVariant, "atk-member0");
                    check(atk(1, 2, false, false) == s2 * 4 + 0xe0 + b.attackVariant, "atk-member1");
                    check(atk(0, 2, true, false) == s2 * 4 + 0x148 + b.attackVariant, "atk-pair");
                    check(atk(2, 0, false, false) == s2 * 3 + 0x1b0, "atk-at-member0");
                    check(atk(2, 1, false, false) == s2 * 3 + 0x1b1, "atk-at-member1");
                    check(atk(2, 0, true, false) == s2 * 3 + 0x1b2, "atk-at-pair");
                    check(atk(2, 1, false, true) == 0x19 * 3 + 0x1b1, "atk-enemy-pair");
                    // B7: the formation table's spots
                    cr::Vec3 sp;
                    const short* e3 = cr::kBattlePosition[b.formation][3];
                    check(attackSpot(app, 0, 0, 2, sp) && sp.x == e3[0] && sp.z == e3[1], "spot-member");
                    b.rules.pair = true;
                    const short *e6 = cr::kBattlePosition[b.formation][6], *e7 = cr::kBattlePosition[b.formation][7];
                    check(attackSpot(app, 0, 0, 2, sp) && sp.x == e6[0] && sp.z == e6[1] && attackSpot(app, 1, 1, 2, sp) && sp.x == e7[0] &&
                              sp.z == e7[1],
                          "spot-pair");
                    const int size2 = monsterSize(app, 2);
                    const float adj = size2 == 3 ? -10.0f : size2 == 2 ? 5.0f : 0.0f; // (z - 10 size 3, + 5 size 2)
                    check(attackSpot(app, 2, 0, 0, sp) && sp.x == cr::kBattlePosition[1][3][0] && sp.z == cr::kBattlePosition[1][3][0] + adj, "spot-vs-pair");
                    b.rules.pair = false;
                    check(attackSpot(app, 2, 0, 1, sp) && sp.x == cr::kBattlePosition[0][4][0] && sp.z == cr::kBattlePosition[0][4][1] + adj, "spot-monster");
                    // B3: a member's blow drawn variant 2 cuts to slot x 4 + 0x7b as its approach ends (que +0x10)
                    {
                        b.turn = cr::BattleTurn();
                        b.turn.kind = cr::kCmdAttack, b.turn.action = b.rules.attackAction(0), b.turn.actors[0] = 0;
                        b.turn.hits.resize(1);
                        b.turn.hits[0].target = 2, b.turn.hits[0].amount = 1;
                        b.phase = App::Battle::Acting, b.turnOn = true;
                        startTurn(app);
                        const App::Battle::Actor* u = battleActor(app, 0);
                        const int first = b.cam, at = b.attackCam2At;
                        b.attackVariant = 2;
                        bool cut = false;
                        for (int f = 0; f < 200 && b.turnOn && b.turnFrame < at; ++f) stepBattle(app);
                        cut = b.turnFrame == at && b.cam == s2 * 4 + 0x7b;
                        spots = u ? std::to_string(int(u->to.x)) + "," + std::to_string(int(u->to.z)) : "?";
                        check(at > 0 && first >= s2 * 4 + 0x78 && first <= s2 * 4 + 0x7a && cut && u && u->to.x == e3[0] && u->to.z == e3[1], "atk-second-camera");
                        for (int f = 0; f < 600 && b.turnOn; ++f) stepBattle(app);
                        b.phase = App::Battle::Command, b.turnOn = false;
                    }
                    // B2 / B3: a gene lands at its cast slot's loop start + 30, cutting to its target
                    int gene = -1;
                    for (int id = 1; id < app.battleActions.count() && gene < 0; ++id)
                        if (const cr::BattleActionDef* d = app.battleActions.get(id); d && d->cls == 11 && d->motion != 1 && d->scope != 3 &&
                                                                                      d->scope != 5 && id != 0xe4 &&
                                                                                      !((id >= 0xd7 && id <= 0xd9) || id == 0xde || id == 0xe8 || id == 0x197 || id == 0x19a || id == 0x1b1))
                            gene = id;
                    if (const App::Battle::Actor* u = battleActor(app, 0); u && gene >= 0) {
                        b.turn = cr::BattleTurn();
                        b.turn.kind = cr::kCmdGene, b.turn.action = gene, b.turn.actors[0] = 0;
                        b.turn.hits.resize(1);
                        b.turn.hits[0].target = 2, b.turn.hits[0].amount = 1;
                        b.phase = App::Battle::Acting, b.turnOn = true;
                        startTurn(app);
                        const int want = app.battleData.anim(u->modelId, realSlot(b.models[u->model], 11)).loopStart + 30;
                        check(b.hitFrame == want && b.cam == 0x49 && b.camHold == 25, "gene-hit-frame");
                        // the cast camera turns over its 25 frames and ends (10001 no longer holds)
                        int ended = -1;
                        for (int f = 1; f < 40 && ended < 0; ++f) {
                            stepBattle(app);
                            if (!b.camLive) ended = f;
                        }
                        check(ended == 25 || (want <= 25 && b.cam != 0x49), "cast-10001-ends");
                        for (int f = 0; f < 200 && b.turnOn && !b.hitsShown; ++f) stepBattle(app);
                        const int sz = monsterSize(app, 2);
                        check(b.hitsShown && b.turnFrame == want && b.cam == (sz == 1 ? 0x60 : sz + 0x61) && b.camActor == 2, "gene-hit-camera");
                        for (int f = 0; f < 600 && b.turnOn; ++f) stepBattle(app);
                        b.phase = App::Battle::Command, b.turnOn = false;
                    } else
                        check(false, "no-gene");
                    { // the hit cameras' cases
                        cr::BattleTurn g;
                        g.kind = cr::kCmdGene, g.action = gene, g.actors[0] = 0;
                        setBattleCamera(app, 0x49, 0);
                        hitCamera(app, g, 0, false);
                        check(b.cam == 0x49, "hitcam-self-keeps");
                        hitCamera(app, g, 1, false);
                        check(b.cam == 0x5e && b.camActor == 1, "hitcam-member");
                        b.rules.pair = true;
                        hitCamera(app, g, 1, false);
                        check(b.cam == 0x5f && b.camActor == 0, "hitcam-pair");
                        b.rules.pair = false;
                        hitCamera(app, g, 0, true);
                        check(b.cam == 0x5d && b.camActor == 0, "hitcam-bounce");
                        g.action = 0x1b1;
                        hitCamera(app, g, 2, false);
                        check(!app.battleActions.get(0x1b1) || b.cam == b.formation + 0xc, "hitcam-still");
                        int wide = -1;
                        for (int id = 1; id < app.battleActions.count() && wide < 0; ++id)
                            if (const cr::BattleActionDef* d = app.battleActions.get(id); d && (d->scope == 3 || d->scope == 5)) wide = id;
                        g.action = wide;
                        hitCamera(app, g, 2, false);
                        check(wide >= 0 && b.cam == (size2 == 1 ? 0x69 : b.formation + 0x67), "hitcam-wide");
                    }
                    // B6: the roll - a still camera's raw[6], a moving one's raw[6] -> raw[13]
                    setBattleCamera(app, 0xc3, 0);
                    stepBattleCamera(app);
                    const cr::BattleCamera* rc = app.battleData.camera(0xc3);
                    const float stillRoll = b.camRoll;
                    check(rc && b.camRoll == rc->raw[6] && rc->raw[6] != 0, "roll-still");
                    setBattleCamera(app, 0x210, 0);
                    const cr::BattleCamera* mc = app.battleData.camera(0x210);
                    for (int f = 0; mc && f < mc->raw[15] / 2; ++f) stepBattleCamera(app);
                    check(mc && std::fabs(b.camRoll - (mc->raw[6] + (mc->raw[13] - mc->raw[6]) * float(mc->raw[15] / 2) / float(mc->raw[15]))) < 0.01f,
                          "roll-moving");
                    // B8: the target cursor eases by a half and holds it
                    setBattleCamera(app, b.formation + 12, 0);
                    stepBattleCamera(app);
                    cameraForTarget(app, 3, false, true);
                    const cr::Vec3 e0 = b.camEye;
                    stepBattleCamera(app);
                    const cr::Vec3 d1 = b.camWantEye - b.camEye, d0 = b.camWantEye - e0;
                    check(b.camHeld && b.camEase == 2 && std::fabs(std::sqrt(cr::dot(d1, d1)) * 2 - std::sqrt(cr::dot(d0, d0))) < 0.01f, "cursor-eased");
                    // B9: a cancelled action's camera: the item's cast one on its user
                    b.turn = cr::BattleTurn();
                    b.turn.kind = cr::kCmdGene, b.turn.cancelled = true, b.turn.actors[0] = 2;
                    b.phase = App::Battle::Acting, b.turnOn = true;
                    startTurn(app);
                    check(b.cam == (size2 == 1 ? 2 + 0x54 : size2 + 0x57) && b.camActor == 2 && b.camHold == 25, "cancel-monster");
                    b.turn.actors[0] = 1;
                    startTurn(app);
                    check(b.cam == 0x54 && b.camActor == 1, "cancel-member");
                    b.phase = App::Battle::Command, b.turnOn = false;
                    // B21: actor -100 - one step of a turning camera, then over
                    setBattleCamera(app, 0x45, -100);
                    const int dur = b.camDur;
                    stepBattleCamera(app);
                    check(dur == -90 && b.camActor == 0 && !b.camLive, "white-out-step");
                    (void)stillRoll;
                    b.rules.pair = keepPair, b.rules.enemyPair = keepEPair;
                    endBattle(app, false);
                } else
                    check(false, "no-battle");
                // B15: a boss's fall - +0xae0 = its slot 6's last frame + 20, the 10000 camera turning over it
                int bossGroup = -1, hold = 0, want = 0;
                for (int g = 1; g <= app.battleData.groupCount() && bossGroup < 0; ++g) {
                    const std::array<int, 3> ids = app.battleData.group(g);
                    const cr::MonsterDef* m0 = app.battleData.monster(ids[0]);
                    if (m0 && m0->strongGrowth && m0->model >= 0 && m0->model < cr::kNumBattleModels && app.battleData.formation(ids) != 2) bossGroup = g;
                }
                app.game = saved;
                if (bossGroup > 0) {
                    startBattle(app, {bossGroup, 0, -1, 0}, 0);
                    if (b.active) {
                        for (int k = 2; k < 5; ++k) b.rules.f[size_t(k)].hp = 0;
                        battleVictory(app);
                        if (const App::Battle::Actor* m2 = battleActor(app, 2)) {
                            const LoadedModel& lm = b.models[m2->model];
                            const int fall = battleAction(lm, realSlot(lm, btlui::kSlotDown));
                            want = (fall >= 0 ? int(lm.actions[size_t(fall)].duration) - 1 : 0) + 20;
                        }
                        hold = b.camHold;
                        const cr::BattleCamera* bc = app.battleData.camera(b.cam);
                        check(b.phase != App::Battle::BossDown || (hold == want && bc && (bc->raw[15] != 10000 || b.camDur == want)), "boss-hold");
                        endBattle(app, false);
                    }
                }
                app.runner.stop();
                app.game = saved;
                cp += app.renderer.takeErrors();
                std::printf("battle cameras: attacks (attacker / target, pair, enemy pair), spots (%s), the second camera, gene hit at loop start + 30 "
                            "and its cameras, roll, cursor 1/2 held, cancelled, 10001 ends, boss hold %d (want %d), white-out -100; %d problems%s\n",
                            spots.c_str(), hold, want, cp, why.c_str());
                problems += cp;
            }
            // a plain hit (btlcalc_0404) shakes the screen for 5 frames
            app.game = saved;
            startBattle(app, {1, 0, -1, 0}, 0);
            if (app.battle.active) {
                App::Battle& b = app.battle;
                for (int f = 0; f < 3000 && b.active && !hitQuakes; ++f) {
                    if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) {
                        battleChooseTop(app, 1);
                        battleChooseCommand(app, 0);
                        if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks[0]);
                    }
                    stepBattle(app);
                    if (b.turnOn && b.hitsShown && b.turn.actors[0] < 2 && !b.turn.hits.empty() && b.turn.hits[0].amount > 0)
                        hitQuakes = b.quake.mode == 1 && b.quake.frames == 5;
                }
                endBattle(app, false);
                app.runner.stop();
            }
            // effect 48's light shafts: their offset moves between frames 5 and 15
            int scrolling = 0, moved = 0;
            startBattle(app, {1, 0, -1, 0}, 0);
            if (const LoadedModel* fx = app.battle.active ? effectModel(app, 48) : nullptr)
                for (const cr::Material& mt : fx->cpu.materials) {
                    float s0[2] = {0, 0}, s1[2] = {0, 0}, k0[2] = {1, 1}, k1[2] = {1, 1};
                    if (!effectUvMotion(*fx, &mt, 5, s0, k0)) continue;
                    ++scrolling;
                    effectUvMotion(*fx, &mt, 15, s1, k1);
                    moved += s0[0] != s1[0] || s0[1] != s1[1] || k0[0] != k1[0] || k0[1] != k1[1];
                }
            if (app.battle.active) endBattle(app, false);
            app.runner.stop();
            app.game = saved;
            const int boss15 = app.battleData.anim(9, 14).loopEnd > 0 || app.battleData.anim(9, 14).hit > 0;
            std::printf("battle staging: %d of 6 boss actions staged, hit quake %s, effect 48: %d of %d scrolling materials move, "
                        "boss slots past 13 %s; %d problems, %d render API errors\n", ran, hitQuakes ? "yes" : "no", moved,
                        scrolling, boss15 ? "read" : "missing", sbad, rerrs);
            problems += sbad + rerrs + (ran != 6) + !hitQuakes + (scrolling == 0 || moved == 0);
            glErrors += rerrs;
        }

        // The weather goes on in battles with the "(128)" copies (snow: slots 62 / 65, stepped by the
        // battle's frames), and the tower's Space backdrop scrolls (Up +1, Down -1 a frame, Stop
        // still; the fog's drift wraps past 480), drawn on the field and in battles without errors
        {
            int fbad = 0, ferrs = 0;
            loadSceneEffectSet(app, 0);
            setSceneEffect(app, true, 5);
            startBattle(app, {1, 0, -1, 0}, 0);
            if (app.battle.active) {
                fbad += !app.fx.tex[62 - 49].id || !app.fx.tex[65 - 49].id || !app.fx.tex[61 - 49].id;
                const int t0 = app.fx.timerA[5];
                for (int f = 0; f < 10; ++f) stepBattle(app);
                fbad += app.fx.timerA[5] != t0 + 10;
            app.environmentEffects = false; // Options: Environment Effects off - the weather stands still, undrawn
            for (int f = 0; f < 5; ++f) stepBattle(app);
            fbad += app.fx.timerA[5] != t0 + 10;
            app.environmentEffects = true;
                app.lastTicks = 0;
                drawFrame(app, false);
                ferrs += app.renderer.takeErrors();
                endBattle(app, false);
                app.runner.stop();
            } else ++fbad;
            endSceneEffects(app);
            loadSceneEffectSet(app, 4);
            fbad += !app.fx.tex[66 - 49].id || !app.fx.tex[68 - 49].id || !app.fx.tex[72 - 49].id;
            setSceneEffect(app, true, 13); // d0507, rising
            for (int f = 0; f < 100; ++f) stepSceneEffects(app);
            const int rose = app.fx.spaceRise;
            setSceneEffect(app, false, 13);
            setSceneEffect(app, true, 17); // d0511, falling
            for (int f = 0; f < 30; ++f) stepSceneEffects(app);
            const int fell = app.fx.spaceRise;
            setSceneEffect(app, false, 17);
            setSceneEffect(app, true, 6); // d0501, still
            for (int f = 0; f < 400; ++f) stepSceneEffects(app);
            const int drift = app.fx.spaceDrift;
            fbad += rose != 100 || fell != 70 || app.fx.spaceRise != 70 || drift != 530 - 481;
            startBattle(app, {1, 0, -1, 0}, 0);
            if (app.battle.active) {
                app.lastTicks = 0;
                drawFrame(app, false);
                ferrs += app.renderer.takeErrors();
                endBattle(app, false);
                app.runner.stop();
            }
            endSceneEffects(app);
            std::printf("battle weather: snow over a battle with its (128) copies, Space backdrop rose %d, fell to %d, "
                        "drift %d; %d problems, %d render API errors\n", rose, fell, drift, fbad, ferrs);
            problems += fbad + ferrs;
            glErrors += ferrs;
        }

        // Battle voices (voiced data only): the 2.2.0 action records line up (Particle Cannon's class
        // 13, Attack's motion), the speakers come from the name ids, and a fought battle says its
        // lines - commands, attacks, hits, the victory - from the table's clips
        if (!app.battleVoiceTable.empty() && app.sound.voiceClips(true) == 0) startSound(app), vitaAudio(app); // (tests run before it)
        if (btlvoice::on(app)) {
            int vbad = 0;
            const cr::BattleActionDef* cannon = app.battleActions.get(430);
            vbad += !cannon || cannon->cls != 13 || cannon->calc != 0 || app.battleActions.count() < 450;
            startBattle(app, {1, 0, -1, 0}, 0);
            App::Battle& b = app.battle;
            if (b.active) {
                vbad += btlvoice::speaker(app, 0) != b.rules.f[0].character; // the party's own
                app.sound.battleLineLog.clear();
                for (int f = 0; f < 6000 && b.active && b.phase != App::Battle::Results && b.phase != App::Battle::Lost; ++f) {
                    if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) {
                        battleChooseTop(app, 0);
                        for (int k = 0; k < 2 && b.phase == App::Battle::Command && b.menu == App::Battle::Commands; ++k) {
                            battleChooseCommand(app, 0);
                            if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks[0]);
                        }
                    }
                    stepBattle(app);
                }
                const std::vector<int> log = app.sound.battleLineLog;
                const bool won = b.phase == App::Battle::Results;
                int inRange = 0;
                for (int c : log) inRange += c > 0 && c < app.sound.voiceClips(true);
                vbad += log.size() < 3 || inRange != int(log.size());
                if (SDL_getenv("CR_BATTLE_DEBUG"))
                    for (int c : log)
                        for (size_t i = 0; i < app.battleVoiceTable.size(); ++i)
                            if (app.battleVoiceTable[i] == c) { std::printf("  line: speaker %zu situation %zu (clip %d)\n", i / 1000, i % 1000 + 1, c); break; }
                std::printf("battle voices: %zu lines in a %s battle (%d clips in range), action 430 class %d; %d problems\n",
                            log.size(), won ? "won" : "lost", inRange, cannon ? cannon->cls : -1, vbad);
                endBattle(app, won);
                app.runner.stop();
            } else ++vbad;
            problems += vbad;
        } else
            std::printf("battle voices: none in this data (%zu situations, %d clips)\n", app.battleVoiceTable.size(),
                        app.sound.voiceClips(true));

        // Whole battles through the scene and its menus: pair or solo attacks, defending, an item,
        // escaping - every one ends (won, lost or fled) and draws without render API errors
        {
            app.rng.seed(5);
            int won = 0, lost = 0, fled = 0, stuck = 0, rerrs = 0, n = 0, levelled = 0, resultsBad = 0;
            const cr::GameState saved = app.game;
            std::vector<std::pair<int, int>> list(starts.begin(), starts.end());
            for (size_t i = 0; i < list.size() && n < 24; i += std::max<size_t>(1, list.size() / 24), ++n) {
                app.game = saved;
                app.game.items[21] = 2;
                startBattle(app, {list[i].second, list[i].first, -1, int(i % 3 == 2 ? 1 : 0)}, 0);
                App::Battle& b = app.battle;
                if (!b.active) continue;
                const int style = n % 4; // 0 pair attacks, 1 solo attacks, 2 defend then attack + an item, 3 escape
                int frame = 0, ended = 0;
                for (; frame < 30000 && b.active; ++frame) {
                    if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) {
                        if (style == 3 && b.rules.round >= 1) battleChooseTop(app, 2), battleEscapeAnswer(app, true);
                        else {
                            const bool both = b.rules.f[0].alive() && b.rules.f[1].alive();
                            battleChooseTop(app, style == 0 && both ? 1 : 0);
                            for (int k = 0; k < 2 && b.phase == App::Battle::Command; ++k) {
                                if (style == 2 && b.rules.round == 0 && k == 0) { battleChooseCommand(app, 3); continue; }
                                if (style == 2 && b.rules.round == 1 && k == 0 && app.game.items[21] > 0) {
                                    battleChooseCommand(app, 2);
                                    if (b.menu == App::Battle::Items) battleChooseItem(app, b.picks[0]);
                                    if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks[0]);
                                    continue;
                                }
                                battleChooseCommand(app, 0);
                                if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks.back());
                            }
                        }
                    }
                    if (b.phase == App::Battle::Results) { // MbResultPrc: a tap ends the counting, then a line a tap, then away
                        ++won;
                        levelled += int(b.levelled.size());
                        const size_t lines = b.result.ups.size() + b.result.items.size();
                        int taps = 0;
                        for (int g = 0; g < 3000 && b.exit != 4; ++g) {
                            stepBattle(app);
                            if (b.result.frames >= 30 && b.result.stage != 3) resultsTap(app), ++taps, b.result.frames = 4;
                        }
                        const bool through = b.exit == 4 && b.result.shownExp == float(b.result.expTotal) && size_t(taps) >= lines + 1;
                        if (!through) std::printf("  results: exit %d, %d taps for %zu lines, exp %.0f of %d\n", b.exit, taps, lines,
                                                  b.result.shownExp, b.result.expTotal), ++resultsBad;
                        b.exit = 0;
                        ended = 1;
                        endBattle(app, true);
                        break;
                    }
                    if (b.phase == App::Battle::Lost && b.phaseFrames > 5) { ++lost; ended = 1; endBattle(app, false); break; }
                    if (b.exit == 1) {
                        ++fled;
                        ended = 1;
                        if (SDL_getenv("CR_BATTLE_TRACE")) std::printf("  fled: style %d round %d\n", style, b.rules.round);
                        endBattle(app, false);
                        break;
                    }
                    stepBattle(app);
                    if (b.exit) continue; // counted above, before drawFrame would take it
                    if (frame % 40 == 0) {
                        app.lastTicks = 0;
                        drawFrame(app, false);
                        rerrs += app.renderer.takeErrors();
                    }
                }
                if (!ended) {
                    ++stuck;
                    std::printf("  battle %d on stage %d didn't end (phase %d, menu %d, round %d)\n", list[i].second,
                                list[i].first, int(b.phase), int(b.menu), b.rules.round);
                    if (b.active) endBattle(app, false);
                }
                app.runner.stop();
            }
            app.game = saved;
            std::printf("battle scenes: %d played - %d won (%d level-ups), %d lost, %d fled, %d didn't end, %d results stuck; %d render "
                        "API errors\n", n, won, levelled, lost, fled, stuck, resultsBad, rerrs);
            problems += stuck + rerrs + resultsBad + (won == 0) + (fled == 0);
            glErrors += rerrs;
        }
        return problems;
    }
}

// The last drawn frame to a .bmp (self-test looks)
void saveFrame(App& app, const char* path) {
    int sw = 0, sh = 0;
    std::vector<uint8_t> px;
    if (!app.renderer.readPixels(px, sw, sh)) return;
    SDL_Surface* surf = SDL_CreateSurface(sw, sh, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < sh; ++y)
        std::memcpy(static_cast<uint8_t*>(surf->pixels) + size_t(y) * surf->pitch, &px[size_t(y) * sw * 4], size_t(sw) * 4);
    SDL_SaveBMP(surf, path);
    SDL_DestroySurface(surf);
}

// Trophies (game/trophies.hpp): each condition unlocks exactly its trophy, once, any pair's flag for
// the shared ones and only that pair's for its primal door; several at once in FUN_8106ae3a's order;
// 0 once the fifteen are in; the shop's purchase; nothing without the Vita copy as the game; the
// notification's timing and queue; kept across a restart (settings.ini); the trophy pack's names
int trophiesSelfTest(App& app) {
    namespace tr = cr::trophies;
    int problems = 0;
    const App::Trophies keepTrophy = app.trophy;
    const cr::GameState keepGame = app.game;
    const App::Shop keepShop = app.shop;
    const bool keepPersist = app.persist, keepVitaOnly = app.vitaOnly;
    app.persist = false;
    auto fresh = [&] {
        app.trophy.flags = tr::Flags(), app.trophy.queue.clear(), app.trophy.earnedLog.clear(), app.trophy.notice = tr::Notice();
        app.trophy.live = true, app.trophy.force = true, app.trophy.hold = false;
        app.game = cr::GameState();
    };
    struct Case { const char* what; std::vector<int> expect; std::function<void()> set; };
    const std::vector<Case> cases = {
        {"flag 0x462 (A Dryg Goch, Escher's)", {1}, [&] { app.game.setFlag(0x462, true); }},
        {"flag 0x912 (A Dryg Goch, Ayuta's)", {1}, [&] { app.game.setFlag(0x912, true); }},
        {"flag 0x7bf (the Prototype Agent, Olgar's)", {8}, [&] { app.game.setFlag(0x7bf, true); }},
        {"flag 0x455 (Escher's primal door)", {9}, [&] { app.game.setFlag(0x455, true); }},
        {"flag 0x5e5 (Zhamo's primal door)", {10}, [&] { app.game.setFlag(0x5e5, true); }},
        {"flag 0x905 (Ayuta's primal door)", {12}, [&] { app.game.setFlag(0x905, true); }},
        {"flag 0x649 (Qualia, Zhamo's)", {13}, [&] { app.game.setFlag(0x649, true); }},
        {"flag 0x425 (the businessman)", {14}, [&] { app.game.setFlag(0x425, true); }},
        {"a purchase (0x2f)", {15}, [&] { app.trophy.flags.set(tr::kBought); }},
        {"three at once", {1, 5, 14}, [&] { app.game.setFlag(0x425, true), app.game.setFlag(0x48a, true), app.game.setFlag(0x5f2, true); }},
        {"fourteen of fifteen", {}, [&] { for (int n = 1; n < 15; ++n) app.trophy.flags.set(n); }},
        {"the fifteenth", {15, 0}, [&] { for (int n = 1; n < 15; ++n) app.trophy.flags.set(n); app.trophy.flags.set(tr::kBought); }},
        {"another flag (0x456)", {}, [&] { app.game.setFlag(0x456, true); }},
    };
    int conditions = 0;
    for (const Case& c : cases) {
        fresh();
        c.set();
        checkTrophies(app);
        const bool once = app.trophy.earnedLog == c.expect && app.trophy.queue == c.expect;
        bool kept = true;
        for (int n : c.expect) kept &= app.trophy.flags.has(n);
        checkTrophies(app); // (again: nothing more)
        const bool again = app.trophy.earnedLog.size() == c.expect.size();
        if (once && kept && again) ++conditions;
        else std::printf("trophies: %s unlocked %zu (want %zu)\n", c.what, app.trophy.earnedLog.size(), c.expect.size()), ++problems;
    }
    // the shop's purchase (__menuu_ShopPrc 7): the bit, then the check
    fresh();
    app.shop = App::Shop();
    app.shop.list = {1};
    app.shop.listCursor = 0, app.shop.count = 1, app.shop.selling = false;
    app.game.oz = 100000;
    shopPay(app);
    const bool shop = app.trophy.flags.has(tr::kBought) && app.trophy.earnedLog == std::vector<int>{15};
    problems += !shop;
    app.shop = keepShop;
    // with the iOS (or Android) data as the game too (the owner's choice: 2.2.0 has none of its own)
    fresh();
    app.trophy.force = false, app.vitaOnly = false;
    app.game.setFlag(0x462, true);
    checkTrophies(app);
    const bool onIos = app.trophy.earnedLog == std::vector<int>{1};
    fresh();
    app.trophy.force = false, app.vitaOnly = true;
    app.game.setFlag(0x462, true);
    checkTrophies(app);
    const bool onVita = app.trophy.earnedLog == std::vector<int>{1};
    app.vitaOnly = keepVitaOnly;
    problems += !onIos + !onVita;
    // the notification: in over kIn, held, out; one at a time, in order
    fresh();
    app.game.setFlag(0x462, true), app.game.setFlag(0x463, true);
    checkTrophies(app);
    std::vector<int> shownOrder;
    float firstFull = -1, firstGone = -1, clock = 0;
    for (int f = 0; f < 400 && shownOrder.size() < 2; ++f) {
        const bool was = app.trophy.notice.on;
        stepTrophies(app, 0.01f);
        clock += 0.01f;
        if (!was && app.trophy.notice.on) shownOrder.push_back(app.trophy.notice.id);
        if (shownOrder.size() == 1 && firstFull < 0 && app.trophy.notice.shown() >= 1) firstFull = clock;
        if (shownOrder.size() == 1 && was && !app.trophy.notice.on && firstGone < 0) firstGone = clock;
    }
    const bool notice = shownOrder == std::vector<int>({1, 2}) && std::abs(firstFull - 0.24f) < 0.025f &&
                        std::abs(firstGone - (tr::Notice::kIn + tr::Notice::kHold + tr::Notice::kOut)) < 0.025f;
    problems += !notice;
    // kept across a restart: settings.ini's trophies= (an isolated folder, as controlHintsSelfTest's)
    bool kept = false;
    {
        const char* oldEnv = SDL_getenv("CR_SAVE_DIR");
        const std::string keepEnv = oldEnv ? oldEnv : "";
        const auto testDir = std::filesystem::temp_directory_path() / ("cr-trophy-test-" + std::to_string(SDL_GetTicksNS()));
        if (std::filesystem::create_directory(testDir)) {
            SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "CR_SAVE_DIR", testDir.u8string().c_str(), true);
            const App::Mix keepMix = app.mix;
            const int keepLanguage = app.language;
            fresh();
            app.persist = true;
            app.game.setFlag(0x4b9, true);
            checkTrophies(app); // (13: written as it is unlocked)
            setTrophyBit(app, tr::kBought);
            const tr::Flags before = app.trophy.flags;
            app.trophy.flags = tr::Flags();
            loadMix(app);
            kept = before.has(13) && app.trophy.flags.bits == before.bits && app.trophy.flags.earned() == 1 && app.trophy.flags.has(tr::kBought);
            app.persist = false;
            app.mix = keepMix, app.language = keepLanguage;
            applyLanguage(app), applyMix(app);
            SDL_RemovePath(settingsPath().c_str());
            std::filesystem::remove(testDir);
            if (oldEnv) SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "CR_SAVE_DIR", keepEnv.c_str(), true);
            else SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "CR_SAVE_DIR");
        }
    }
    problems += !kept;
    tr::Flags hex;
    hex.set(0), hex.set(15), hex.set(tr::kBought);
    tr::Flags back;
    const bool hexOk = back.fromHex(hex.hex()) && back.bits == hex.bits && hex.hex().size() == 16 && !back.fromHex("zz");
    problems += !hexOk;
    // the names: the pack's (with the Vita copy beside) as the built-in ones, else the built-in ones
    app.trophy = keepTrophy;
    loadTrophyPack(app);
    bool names = true;
    int icons = 0;
    for (int n = 0; n < tr::kCount; ++n) {
        names &= !trophyName(app, n).empty() && (!app.trophy.packRead || trophyName(app, n) == tr::builtinName(n)) &&
                 trophyGrade(app, n) == tr::builtinGrade(n);
        icons += app.trophy.icons[size_t(n)].id != 0;
    }
    if (app.trophy.packRead) names &= icons == tr::kCount;
    problems += !names;
    std::printf("trophies: %d of %zu conditions unlock their own once, the shop's purchase %s, with the iOS data as the game too %s "
                "(with it %s), notification %s (in %.2f s, gone %.2f s, one at a time %s), kept across a restart %s, flags text %s, "
                "names %s (pack %s, %d pictures); %d problems\n",
                conditions, cases.size(), shop ? "yes" : "NO", onIos ? "yes" : "NO", onVita ? "yes" : "NO", notice ? "yes" : "NO",
                firstFull, firstGone, shownOrder.size() == 2 ? "yes" : "NO", kept ? "yes" : "NO", hexOk ? "yes" : "NO",
                names ? "yes" : "NO", app.trophy.packRead ? "read" : "not beside the data", icons, problems);
    if (!keepTrophy.packTried)
        for (cr::Texture& t : app.trophy.icons) app.renderer.destroy(t);
    app.trophy = keepTrophy;
    app.game = keepGame;
    app.persist = keepPersist;
    return problems;
}

int controlHintsSelfTest(App& app) {
    const bool keepHud = app.showHud, keepPersist = app.persist;
    // settingsPath uses CR_SAVE_DIR, not the game's saveDirOverride. Give this
    // persistence test its own directory even when invoked in a real profile.
    const char* oldEnv = SDL_getenv("CR_SAVE_DIR");
    const bool hadEnv = oldEnv != nullptr;
    const std::string keepEnv = oldEnv ? oldEnv : "";
    const auto testDir = std::filesystem::temp_directory_path() /
        ("cr-settings-test-" + std::to_string(SDL_GetTicksNS()));
    if (!std::filesystem::create_directory(testDir)) return 1;
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "CR_SAVE_DIR", testDir.u8string().c_str(), true);
    app.persist = true; // an isolated settings file: exercise the same save / load as a restart
    int problems = 0;
    bool off = false, on = false, shortcut = false;
    const auto rows = optionRows(app, 0);
    const auto row = std::find_if(rows.begin(), rows.end(), [](const OptRow& r) { return r.label == "Control Hints"; });
    if (row == rows.end() || !row->act) ++problems;
    else {
        row->act(1); // Off, as a tap on its settings button
        const bool hidden = !app.showHud && !keyHints(app);
        app.showHud = true;
        loadMix(app);
        off = hidden && !app.showHud && !keyHints(app);
        problems += !off;
        row->act(0); // On
        const bool shown = app.showHud && keyHints(app);
        app.showHud = false;
        loadMix(app);
        on = shown && app.showHud && keyHints(app);
        problems += !on;
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.key = SDLK_F3;
        key.key.down = true;
        SDL_AppEvent(&app, &key);
        const bool toggled = !app.showHud;
        app.showHud = true;
        loadMix(app);
        shortcut = toggled && !app.showHud;
        problems += !shortcut;
    }
    SDL_RemovePath(settingsPath().c_str());
    std::filesystem::remove(testDir);
    if (hadEnv) SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "CR_SAVE_DIR", keepEnv.c_str(), true);
    else SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "CR_SAVE_DIR");
    app.showHud = keepHud;
    app.persist = keepPersist;
    std::printf("control hints: settings Off persists %s, On persists %s, F3 persists %s; %d problems\n",
                off ? "yes" : "NO", on ? "yes" : "NO", shortcut ? "yes" : "NO", problems);
    return problems;
}

// The field's rendering as CR1 2.2.0 draws it (Omega's 8e8c370 ported): npc_shodow's shadows, the
// stage record's alpha model, the loaders' colours, the effects' depth writes, the fades, the front
// fade's place, the planes and (2.2.0's data) the Options' Language row
int fieldRenderSelfTest(App& app) {
    int problems = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) std::printf("field render: %s FAILED\n", what);
        problems += !ok;
    };
    const bool keepBg = app.showBackground, keepNoEnc = app.noEncounters;
    const int keepLang = app.language, keepChar = app.playerChar;
    app.showBackground = true, app.noEncounters = true;
    app.mode = Mode::Maps;
    auto settle = [&] {
        app.runner.stop();
        app.pendingEntry = App::Entry::None;
        app.dialogue.clear();
    };
    auto frame = [&] {
        app.lastTicks = 0;
        drawFrame(app, false);
    };
    // The alpha model: the stage record's +0x40 against the field's namesake (the old rule)
    int withModel = 0, differ = 0, recordNone = 0;
    std::string differences;
    for (size_t m = 0; m < app.maps.size(); ++m) {
        showMap(app, int(m));
        settle();
        const std::string field = app.maps[m].substr(0, app.maps[m].find('_'));
        const bool named = std::find(app.models.begin(), app.models.end(), field + ".bin") != app.models.end();
        const std::string old = named ? field + ".bin" : std::string();
        withModel += !app.mapProps.empty();
        if (app.mapProps != old) {
            ++differ;
            recordNone += app.mapProps.empty();
            if (differences.size() < 900)
                differences += " " + app.maps[m] + ":" + (old.empty() ? "-" : old) + "->" + (app.mapProps.empty() ? "-" : app.mapProps);
        }
    }
    std::printf("field render: alpha models in %d of %zu rooms; %d differ from the field's namesake (%d to none):%s\n", withModel,
                app.maps.size(), differ, recordNone, differences.c_str());
    check(withModel > 0, "alpha models");
    // Shadows: the leader's (80%: 204) and every flagged NPC's - the first room with one
    int flagged = 0, playerA = -1, npcA = -1, npcWant = -1, fadedA = -1;
    for (size_t m = 0; m < app.maps.size() && !flagged; ++m) {
        showMap(app, int(m));
        settle();
        for (const App::NpcDraw& d : app.npcDraws) {
            const bool on = d.index >= 0 && d.index < int(app.npcs.size()) && app.npcs[size_t(d.index)].hasShadow();
            if (!on || app.npcs[size_t(d.index)].shadow[1] <= 0 || app.npcs[size_t(d.index)].shadow[2] <= 0 || d.alpha <= 0) continue;
            if (!flagged) npcWant = std::abs(int(app.npcs[size_t(d.index)].shadow[3])) * 255 / 100;
            ++flagged;
        }
    }
    if (flagged && app.havePlayer) {
        int drawnBefore = app.shadowsDrawn;
        frame();
        const int drawn = app.shadowsDrawn - drawnBefore;
        if (!app.fieldShadowAlpha.empty()) playerA = app.fieldShadowAlpha[0];
        if (app.fieldShadowAlpha.size() >= 2 * 96) npcA = app.fieldShadowAlpha[96];
        check(drawn == 1 + flagged && playerA == 204 && npcA == npcWant, "shadows (the leader's 204, the NPC's by its percent)");
        ViewerHost host(app); // NpcFade 0 to half: CR1's shadow keeps its own alpha (mdl_shadow: 255 x percent)
        host.fadeNpc(0, 128.0f / 255.0f, 128.0f / 255.0f, 1);
        frame();
        if (!app.fieldShadowAlpha.empty()) fadedA = app.fieldShadowAlpha[0];
        check(fadedA == 204 && app.playerFaded, "the leader's shadow at half alpha");
        host.fadeNpc(0, 0, 0, 1); // ... and none at 0 (+0x55c = 0)
        frame();
        const bool noneAtZero = app.fieldShadowAlpha.size() == size_t(96 * flagged);
        host.fadeNpc(0, 1, 1, 1);
        frame();
        check(noneAtZero, "no leader's shadow at alpha 0");
        app.fieldHidden = true; // Screen's toggle: no models, the shadows all the same
        drawnBefore = app.shadowsDrawn;
        frame();
        check(app.shadowsDrawn - drawnBefore == 1 + flagged, "shadows with the field hidden");
        app.fieldHidden = false;
        // mdl_set_alpha over 0 frames: the alpha as it was, the colour arrays off from then on
        const int id = app.npcDraws.front().id;
        const float before = app.npcDraws.front().alpha;
        host.fadeNpc(id, 0.0f, 0.0f, 0);
        const App::NpcDraw& d1 = app.npcDraws.front();
        check(d1.id == id && d1.faded && d1.alpha == before && d1.fadeLeft == 0, "NpcFade over 0 frames");
    } else check(false, "a room with a shadowed NPC");
    std::printf("field render: shadows: %d flagged NPCs in %s, the leader's alpha %d, the NPC's %d (want %d), the leader at half %d\n",
                flagged, app.maps[size_t(app.mapIndex)].c_str(), playerA, npcA, npcWant, fadedA);
    // The colours: read_mdl's models unlit with their own colour 1, alpha test 1 (0 for no-alpha textures
    // and c302..c306); effects unlit, no alpha test
    {
        int npcMats = 0, notUnlit = 0, badTest = 0, darker = 0, effMats = 0, effBad = 0;
        std::string darkList;
        for (int k = 0; k < cr::kNumNpcModels; ++k) npcModel(app, cr::kNpcModelFile[k]); // (every NPC table model, as read_mdl reads it)
        for (const auto& [name, lm] : app.npcModels) {
            int dk = 0;
            for (const cr::Material& mt : lm.cpu.materials) {
                ++npcMats, notUnlit += !mt.unlit, badTest += mt.alphaThreshold > 1;
                float old = 0, now = 0; // (the old approximation's brightness against colour 1's)
                for (int k = 0; k < 3; ++k) old += std::min(1.0f, mt.emission[k] + mt.ambient[k] + mt.diffuse[k]), now += mt.diffuse[k];
                dk += now < old - 0.05f && !(mt.textureRef >= 0 && mt.texBlend == 0); // (a replaced texture takes no colour)
                if (SDL_getenv("CR_DUMP_COLOURS") && now < old - 0.05f)
                    std::printf("colour %s: diffuse %.2f %.2f %.2f %.2f ambient %.2f %.2f %.2f emission %.2f %.2f %.2f tex %d blend %d props %x\n",
                                name.c_str(), mt.diffuse[0], mt.diffuse[1], mt.diffuse[2], mt.diffuse[3], mt.ambient[0], mt.ambient[1],
                                mt.ambient[2], mt.emission[0], mt.emission[1], mt.emission[2], mt.textureRef, mt.texBlend, mt.properties);
            }
            darker += dk;
            if (dk && darkList.size() < 400 && name[0] != 'b') darkList += " " + name + ":" + std::to_string(dk) + "/" + std::to_string(lm.cpu.materials.size());
        }
        for (int id = 0; id < 100; ++id) {
            const LoadedModel* lm = effectModel(app, id);
            if (!lm) continue;
            for (const cr::Material& mt : lm->cpu.materials) ++effMats, effBad += !mt.unlit || mt.alphaThreshold != 0;
        }
        check(npcMats > 0 && notUnlit == 0 && badTest == 0 && effMats > 0 && effBad == 0, "the loaders' colours");
        std::printf("field render: %d read_mdl materials (%d not unlit, %d alpha tests above 1, %d darker than the old "
                    "approximation:%s); %d effect materials (%d not as eff3d leaves them)\n", npcMats, notUnlit, badTest, darker,
                    darkList.c_str(), effMats, effBad);
    }
    // The 2D order: the front fade over a portrait and under the message's text; over the portrait alone
    if (app.windowTex.id) {
        App::Portrait& p = app.portraits[15];
        const App::Portrait keep = p;
        p = App::Portrait();
        p.tex = app.windowTex, p.alpha = 1;
        app.fadeGrey = 0, app.fadeFrom = 0.5f, app.fadeTo = 0.5f, app.fadeFrames = 10, app.fadeT = 0;
        app.dialogue = {{"Fade order"}};
        app.dialoguePage = 0;
        frame();
        const App::Order2D withText = app.order2d;
        app.dialogue.clear();
        const int keepWin = app.msgWin.state;
        app.msgWin.state = 6;
        frame();
        const App::Order2D alone = app.order2d;
        app.msgWin.state = keepWin;
        p = keep;
        app.fadeFrames = 0, app.fadeT = 0;
        check(withText.portrait > 0 && withText.frontFade > withText.portrait && withText.text > withText.frontFade &&
                  alone.portrait > 0 && alone.frontFade > alone.portrait && alone.text == 0,
              "the front fade's place");
        std::printf("field render: 2D order with a message: portrait %d, front fade %d, text %d; without: portrait %d, fade %d\n",
                    withText.portrait, withText.frontFade, withText.text, alone.portrait, alone.frontFade);
    }
    { // setCamera3D's planes: near 1 + the map's offset, far 2000 (maps 86 / 87: 1000)
        float n19, f19, n86, f86, n42, f42, n0, f0;
        const int keepMap = app.mapNumber;
        const cr::Vec3 eye{0, 50, 100}, tgt{0, 0, 0};
        app.mapNumber = 19, fieldDepthRange(app, eye, tgt, n19, f19);
        app.mapNumber = 86, fieldDepthRange(app, eye, tgt, n86, f86);
        app.playerChar = 2, app.mapNumber = 42, fieldDepthRange(app, eye, tgt, n42, f42);
        app.playerChar = 0, fieldDepthRange(app, eye, tgt, n0, f0);
        app.playerChar = keepChar;
        app.mapNumber = keepMap;
        const float far0 = 1 + std::sqrt(50.0f * 50.0f + 100.0f * 100.0f) / 10; // (distance over 100, heights 50 apart)
        check(n19 == 1 && f19 == 2000 && n86 == 11 && f86 == 1000 && n42 == 501 && std::fabs(n0 - far0) < 1e-3f && f0 == 2000, "near / far");
    }
    // Options (2.2.0's data): one Language row, its arrows through the languages the data has
    if (languageSheet(app)) {
        app.language = 1;
        std::vector<OptRow> rows = optionRows(app, 0);
        bool ok = rows.size() > 1 && rows[0].kind == OptControl::Value && rows[0].valueTex && rows[1].label != "Text Language";
        std::set<int> seen;
        for (int k = 0; ok && k < cr::kLanguages; ++k) {
            optionRows(app, 0)[0].act(1);
            seen.insert(app.language);
        }
        int usable = 0;
        for (int l = 0; l < cr::kLanguages; ++l) usable += languageUsable(app, l);
        ok = ok && app.language == 1 && int(seen.size()) == usable;
        app.language = 1;
        optionRows(app, 0)[0].act(-1);
        const int back = app.language;
        ok = ok && back == (languageUsable(app, 0) ? 0 : 7);
        rows = optionRows(app, 0);
        ok = ok && rows[0].valueUv[0] == 88.0f * float(app.language);
        check(ok, "the Language row (2.2.0)");
        std::printf("field render: the Language row cycles %zu languages, left from English to %s\n", seen.size(), cr::kLanguageName[back]);
    } else {
        const std::vector<OptRow> rows = optionRows(app, 0);
        check(rows.size() > 1 && rows[0].kind == OptControl::Pair && rows[0].words[0] == 37 && rows[1].label == "Text Language",
              "the Language pair (1.0.1)");
        std::printf("field render: Options keep 1.0.1's Japanese / English pair\n");
    }
    app.language = keepLang;
    applyLanguage(app);
    app.showBackground = keepBg, app.noEncounters = keepNoEnc;
    problems += app.renderer.takeErrors();
    std::printf("field render: %d problems\n", problems);
    return problems;
}

int fieldPresentationSelfTest(App& app) {
    const auto at = std::find(app.maps.begin(), app.maps.end(), "d0101_10.cmp");
    if (at == app.maps.end()) return 1;
    app.mode = Mode::Maps;
    showMap(app, int(at - app.maps.begin()), 0, 2);
    app.runner.stop();
    app.pendingEntry = App::Entry::None;
    app.dialogue.clear();
    const cr::FieldWalker start = app.walker;
    int problems = 0, moving = 0;
    // A pause left in the model viewer must not stop field animation. Exercise the actual
    // player update with both touch-pad gaits, including each party member's loaded actions.
    // (No encounters meanwhile: a battle started by the walk took the Space below - about one run in
    // three failed "Space confirms")
    const bool keepEncounters = app.noEncounters;
    app.noEncounters = true;
    app.playing = false;
    for (int c = 0; c < int(SDL_arraysize(kPartyModels)); ++c) {
        app.playerChar = c;
        placePlayer(app);
        for (int gait = 1; gait <= 2; ++gait) {
            app.walker = start;
            app.walkClock = 0;
            app.touchPad = App::TouchPad();
            app.touchPad.down = true;
            app.touchPad.dx = gait == 1 ? 20.0f : 40.0f;
            updatePlayer(app, 1.0f / kFramesPerSecond, 0);
            updatePlayer(app, 1.0f / kFramesPerSecond, 0); // (the gait's change: its 2-frame cross-fade)
            const auto first = app.player.world;
            updatePlayer(app, 1.0f / kFramesPerSecond, 0);
            bool changed = false;
            for (size_t i = 0; i < first.size() && i < app.player.world.size(); ++i)
                changed |= !same(first[i], app.player.world[i]);
            const bool ok = app.havePlayer && int(app.walker.gait) == gait && app.player.action == gait &&
                            app.player.time > 0 && changed;
            moving += ok;
            problems += !ok;
        }
    }
    app.touchPad = App::TouchPad();
    app.playing = true;
    app.dialogue = {{"Confirm"}};
    app.dialoguePage = 0;
    SDL_Event confirm{};
    confirm.type = SDL_EVENT_KEY_DOWN;
    confirm.key.key = SDLK_SPACE;
    confirm.key.down = true;
    SDL_AppEvent(&app, &confirm);
    const bool confirmed = app.dialogue.empty() && app.playing;
    problems += !confirmed;
    app.dialogue.clear();
    app.playerChar = 0;
    app.playing = true;
    app.noEncounters = keepEncounters;
    problems += app.renderer.takeErrors();
    std::printf("field animation: %d/16 walk/run poses advance after viewer pause, Space confirms %s; %d problems\n",
                moving, confirmed ? "yes" : "NO", problems);
    // These scripts show a loaded backdrop at the origin without a separate Setc. Cover the
    // Ark across all routes, the fourth gate and both affected tower backdrops.
    struct Backdrop { int area, map, npc; const char* model; };
    static const Backdrop backdrops[] = {
        {7, 113, 501, "t0152bt.bin"}, {13, 113, 501, "t0152bt.bin"},
        {19, 113, 501, "t0152bt.bin"}, {25, 113, 501, "t0152bt.bin"},
        {5, 63, 508, "d0451bt.bin"}, {6, 88, 515, "d0555bt.bin"},
        {18, 88, 511, "d0551bt.bin"}
    };
    for (const Backdrop& b : backdrops) {
        const auto map = std::find(app.maps.begin(), app.maps.end(), cr::kMapCollisionFile[b.map]);
        if (map == app.maps.end()) { ++problems; continue; }
        showMap(app, int(map - app.maps.begin()), b.map, b.area);
        app.runner.stop();
        app.pendingEntry = App::Entry::None;
        ViewerHost host(app);
        host.loadNpc(b.npc);
        host.showNpc(b.npc, false);
        const bool hidden = host.npc(b.npc).empty();
        host.showNpc(b.npc, true);
        const auto draws = host.npc(b.npc);
        const bool shown = draws.size() == 1 && draws[0]->model == b.model &&
                           draws[0]->world.m[12] == 0 && draws[0]->world.m[13] == 0 && draws[0]->world.m[14] == 0;
        host.showNpc(b.npc, false);
        const bool killed = host.npc(b.npc).empty();
        const int sceneProblems = !hidden + !shown + !killed + app.renderer.takeErrors();
        std::printf("cutscene backdrop %s area %d: hidden on Kill %s, shown at origin on Born %s, hidden again %s; %d problems\n",
                    b.model, b.area, hidden ? "yes" : "NO", shown ? "yes" : "NO", killed ? "yes" : "NO", sceneProblems);
        problems += sceneProblems;
    }
    {
        // Render the loaded atlas's panel over magenta and check both halves in every layout.
        // This also guards the iOS / Vita art when changing Android's separate parchment path.
        const char* source = app.androidWindow ? "Android" : app.vita.isOpen() ? "Vita" : "iOS";
        const App::Video keepVideo = app.video;
        const int keepStyle = app.msgStyle, keepLines = app.msgLines;
        int panels = 0;
        app.msgStyle = 2, app.msgLines = 2;
        for (int layout = 0; layout < 5; ++layout) {
            app.video.layout = layout % 4;
            app.video.widescreen = layout == 4;
            applyVideo(app);
            // A smaller viewport exercises the wide aspect without resizing the GL surface.
            int fw = 1024, fh = layout == 4 ? 448 : 768;
            auto& r = app.renderer;
            r.beginFrame(fw, fh);
            r.setOrigin(r.uiX());
            r.fillRect(-r.uiX(), 0, r.screenW(), cr::Renderer::kLogicalH, 1, 0, 1, 1);
            drawMessageBox(app, {}, "Speaker", false, "");
            r.endFrame(false);
            std::vector<uint8_t> pixels;
            bool paper = r.readPixels(pixels, fw, fh);
            float x0, y0, x1, y1;
            r.toLogical(0, 0, x0, y0);
            r.toLogical(float(fw), float(fh), x1, y1);
            for (float x : {200.0f, 800.0f}) {
                const int px = int((x + r.uiX() - x0) * fw / (x1 - x0));
                const int py = int((620 - y0) * fh / (y1 - y0));
                if (!paper || px < 0 || px >= fw || py < 0 || py >= fh) { paper = false; break; }
                paper = pixels[(size_t(py) * fw + px) * 4 + 1] > 100; // parchment, not magenta
            }
            panels += paper;
            if (!paper) std::printf("%s dialogue: parchment missing in layout %d\n", source, layout);
        }
        const int panelProblems = 5 - panels + app.renderer.takeErrors();
        std::printf("%s dialogue: parchment covers both halves in %d/5 layouts; %d problems\n", source, panels, panelProblems);
        problems += panelProblems;
        app.msgStyle = keepStyle, app.msgLines = keepLines;
        app.video = keepVideo;
        applyVideo(app);
    }
    return problems;
}

// Exercise the SDL input path, including window/frame scaling and the phone projection.
int mobileInputSelfTest(App& app) {
    int problems = 0, gestures = 0, slots = 0;
    app.runner.stop();
    app.pendingEntry = App::Entry::None;
    app.titleOpen = app.optionsOpen = app.charSelectOpen = false;
    app.saveScreen.open = false;
    app.game = cr::GameState();
    app.playerChar = 0;
    for (int n = 1; n <= 500; ++n) app.game.items[n] = 10;
    const auto keepVideo = app.video;
    auto pointer = [&](SDL_EventType type, float mx, float my) {
        float x0, y0, x1, y1;
        app.renderer.toLogical(0, 0, x0, y0);
        app.renderer.toLogical(1000, 1000, x1, y1);
        const bool phone = app.renderer.phoneLayout();
        const float lx = wideRight(app) + 2 * (mx + (phone ? 16 : 0));
        const float ly = 2 * (my + (phone ? 32 : 0));
        int ww, wh;
        SDL_GetWindowSize(app.window, &ww, &wh);
        const float x = (lx - x0) * 1000 / (x1 - x0) * ww / app.renderer.frameWidth();
        const float y = (ly - y0) * 1000 / (y1 - y0) * wh / app.renderer.frameHeight();
        SDL_Event e{};
        e.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) e.motion.x = x, e.motion.y = y;
        else e.button.x = x, e.button.y = y, e.button.button = SDL_BUTTON_LEFT;
        SDL_AppEvent(&app, &e);
    };
    auto tap = [&](float x, float y) {
        pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        pointer(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
    };
    for (int layout = 1; layout <= 3; ++layout) for (bool wide : {false, true}) {
        app.video.layout = layout, app.video.widescreen = wide;
        applyVideo(app);
        app.renderer.beginFrame(1280, 720);
        app.renderer.endFrame(false);
        // Item, gene, equipment-slot and equipment-candidate lists use the same gesture (MuScrList mode 0: the
        // focus row on the line, the rows moving under it)
        app.game.setFlag(0x38, true), app.game.setFlag(0x39, true); // (the Camp's once-only messages read)
        for (int view = 0; view < 4; ++view) {
            openCamp(app);
            auto& c = app.camp;
            campOpenPage(app, view == 0 ? 0 : view == 1 ? 2 : 1);
            c.anim = 8;
            if (view >= 2) {
                c.phase = App::Camp::Slot;
                c.member = 0, c.slot = c.slotTop = 0;
                if (view == 3) campChoose(app);
            }
            const auto phase = c.phase;
            const int n = campScrollCount(app);
            if (n <= 6) { ++problems; std::printf("mobile input: view %d has %d rows\n", view, n); continue; }
            pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, campListX(app) + 60, 250);
            pointer(SDL_EVENT_MOUSE_MOTION, campListX(app) + 60, 150);
            pointer(SDL_EVENT_MOUSE_BUTTON_UP, campListX(app) + 60, 150);
            bool ok = campFocus(app) == 2 && c.phase == phase;
            SDL_Event wheel{};
            wheel.type = SDL_EVENT_MOUSE_WHEEL, wheel.wheel.y = -1;
            SDL_AppEvent(&app, &wheel);
            ok &= campFocus(app) == 3;
            pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, 468, 140);
            pointer(SDL_EVENT_MOUSE_MOTION, 468, 284);
            pointer(SDL_EVENT_MOUSE_BUTTON_UP, 468, 284);
            ok &= campFocus(app) == n - 1; // (the gauge's foot: the last row on the line)
            pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, 468, 250);
            pointer(SDL_EVENT_MOUSE_MOTION, 468, 92);
            pointer(SDL_EVENT_MOUSE_BUTTON_UP, 468, 92);
            ok &= campFocus(app) == 0;
            tap(468, 292);
            ok &= campFocus(app) == 1;
            tap(468, 82);
            ok &= campFocus(app) == 0;
            // (a row under the line comes to it; the row on it is the one a tap chooses)
            tap(campListX(app) + 60, campui::kFocusY + campui::kRowH + 20);
            ok &= campFocus(app) == 1 && c.phase == phase;
            if (!ok) std::printf("mobile input: layout %d wide %d view %d FAILED (focus %d slot %d phase %d)\n",
                                      layout, wide, view, c.listCursor, c.slot, int(c.phase));
            problems += !ok;
            ++gestures;
        }
        for (int anim : {4, 8}) for (int slot = 0; slot < campui::kEquipSlots; ++slot) {
            openCamp(app);
            auto& c = app.camp;
            c.page = App::Camp::Equip, c.phase = App::Camp::Slot, c.anim = anim;
            c.slot = c.slotTop = slot; // (the slot on the line)
            tap(campListX(app) + 60, campui::kFocusY + 20);
            const bool ok = c.slot == slot && (slot == 3 ? c.phase == App::Camp::Slot : c.phase == App::Camp::List);
            problems += !ok;
            if (!ok) std::printf("mobile equipment: layout %d wide %d anim %d slot %d FAILED\n", layout, wide, anim, slot);
            ++slots;
        }
    }
    app.campOpen = false;
    app.video = keepVideo;
    applyVideo(app);
    // The settings file must restore both the lock and its chosen landscape direction.
    app.persist = true;
    const bool keepLock = app.lockOrientation, keepFlip = app.lockLandscapeFlipped;
    for (bool locked : {false, true}) for (bool flipped : {false, true}) {
        app.lockOrientation = locked, app.lockLandscapeFlipped = flipped;
        applyOrientation(app);
        saveMix(app);
        app.lockOrientation = !locked, app.lockLandscapeFlipped = !flipped;
        loadMix(app);
        applyVideo(app);
        const char* expected = !locked ? "LandscapeLeft LandscapeRight" : flipped ? "LandscapeRight" : "LandscapeLeft";
        problems += app.lockOrientation != locked || app.lockLandscapeFlipped != flipped ||
                    std::string(SDL_GetHint(SDL_HINT_ORIENTATIONS)) != expected;
    }
    app.lockOrientation = keepLock, app.lockLandscapeFlipped = keepFlip;
    app.persist = false;
    applyOrientation(app);
    startSound(app);
    vitaAudio(app);
    ViewerHost host(app);
    int clip = -1;
    for (int i = 0; i < app.sound.entries(); ++i) {
        host.playSound(0x400000u | uint32_t(i));
        if (app.sound.dialogueVoiceCount() == 1) { clip = i; break; }
    }
    if (clip >= 0) {
        for (int i = 0; i < 10; ++i) host.playSound(0x400000u | uint32_t(clip));
        problems += app.sound.dialogueVoiceCount() != 1;
        app.dialogue = {{"First page"}, {"Silent page"}};
        app.dialoguePage = 0;
        advanceDialogue(app);
        problems += app.sound.dialogueVoiceCount() != 0 || app.dialoguePage != 1;
        host.playSound(0x400000u | uint32_t(clip));
        advanceDialogue(app);
        problems += app.sound.dialogueVoiceCount() != 0 || !app.dialogue.empty();
    } else ++problems;
    if (app.sound.hasVoices()) {
        app.sound.playLine(1);
        app.dialogue = {{"Voiced page"}};
        app.dialoguePage = 0;
        advanceDialogue(app);
        problems += app.sound.dialogueVoiceCount() != 0;
    }
    app.sound.stopAll();
    problems += app.renderer.takeErrors();
    std::printf("mobile input: %d list gesture/tap checks, %d equipment hitboxes; orientation persists both directions, rapid speech replacement (clip %d); %d problems\n",
                gestures, slots, clip, problems);
    return problems;
}

// Exercise teardown while native field tasks are still active. No player saves are written.
int inputCancelSelfTest(App& app) {
    int problems = 0;
    for (Uint32 type : {Uint32(SDL_EVENT_WINDOW_FOCUS_LOST), Uint32(SDL_EVENT_FINGER_CANCELED), Uint32(SDL_EVENT_WILL_ENTER_BACKGROUND)}) {
        app.touchPad.down = app.touchPad.shown = true;
        app.campPointer.down = app.optionsBarDrag = app.dragging = true;
        holdSkip(app, kSkipPointer, true), app.tapPending = true;
        SDL_Event e{};
        e.type = type;
        if (type == SDL_EVENT_FINGER_CANCELED) e.tfinger.windowID = SDL_GetWindowID(app.window);
        else e.window.windowID = SDL_GetWindowID(app.window);
        SDL_AppEvent(&app, &e);
        e.type = SDL_EVENT_USER; // main-thread checkpoint after a lifecycle callback
        SDL_AppEvent(&app, &e);
        const bool clean = !app.touchPad.down && !app.touchPad.shown && !app.campPointer.down &&
                           !app.optionsBarDrag && !app.dragging && !app.skipScene && !app.tapPending;
        problems += !clean;
        if (!clean) std::printf("input cancel FAILED: event %u retained an active gesture\n", type);
        if (type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
            e.type = SDL_EVENT_DID_ENTER_FOREGROUND;
            SDL_AppEvent(&app, &e);
        }
    }
    // Use a real psychokinesis room: cancellation keeps its lifted block and ESP;
    // a fresh drag can then drop it in place, without sending it toward the old release.
    int pkStage = -1;
    for (int i = 0; i < cr::kPuzzleStages; ++i)
        if (cr::kPuzzleStage[i].ability == cr::kAbilityPsychokinesis) { pkStage = i; break; }
    app.mode = Mode::Maps;
    app.runner.stop();
    app.game = cr::GameState();
    app.playerChar = 0, app.mapNumber = app.entryArea = -1;
    cr::WarpRequest w;
    w.valid = true, w.area = cr::kPuzzleArea, w.map = cr::kFirstPuzzleMap + pkStage;
    const bool entered = pkStage >= 0 && performWarp(app, w, -1);
    app.runner.stop(), app.pendingEntry = App::Entry::None;
    int target = -1;
    for (size_t i = 0; i < app.puzzle.objs.size(); ++i)
        if (app.puzzle.objs[i].type == cr::kTypeNormal && app.puzzle.objs[i].shown) { target = int(i); break; }
    if (!entered || target < 0) ++problems;
    else {
        const auto& at = app.puzzle.objs[size_t(target)];
        const auto& layout = cr::kPuzzleStage[pkStage];
        setActor(app, 0, puzzle::centre(app, at.row == 0 ? layout.rows - 1 : 0,
                                       at.col == 0 ? layout.cols - 1 : 0), nullptr, false);
        puzzle::pkGrab(app, target, 200, 200);
        for (int i = 0; i < 4; ++i) stepEvents(app);
        const auto& block = app.puzzle.objs[size_t(target)];
        const int row = block.row, col = block.col, esp = app.puzzle.esp, uses = app.game.vars[13];
        problems += app.puzzle.pkState != 1 || block.pk != 2;
        SDL_Event e{};
        e.type = SDL_EVENT_WINDOW_FOCUS_LOST, e.window.windowID = SDL_GetWindowID(app.window);
        SDL_AppEvent(&app, &e);
        e.type = SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.x = 900, e.button.y = 100;
        SDL_AppEvent(&app, &e);
        for (int i = 0; i < 3; ++i) stepEvents(app);
        const bool held = app.puzzle.pkState == 1 && app.puzzle.pkPointerCancelled && block.row == row && block.col == col;
        problems += !held;
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.x = 100, e.button.y = 200;
        SDL_AppEvent(&app, &e);
        problems += app.puzzle.pkState != 1 || app.puzzle.pkPointerCancelled || app.puzzle.pkMoved;
        e.type = SDL_EVENT_MOUSE_BUTTON_UP; // fresh press/release in place, not the old swipe
        SDL_AppEvent(&app, &e);
        for (int i = 0; i < 60; ++i) stepEvents(app);
        const bool dropped = app.puzzle.pkState == 0 && block.row == row && block.col == col &&
                             app.puzzle.esp == esp && app.game.vars[13] == uses;
        problems += !dropped;
        std::printf("puzzle input cancel: block held %s, fresh release drops in place without ESP %s\n",
                    held ? "yes" : "NO", dropped ? "yes" : "NO");
    }
    app.trackerArmed = false;
    std::printf("viewer input cancellation: %d problems\n", problems);
    return problems;
}

int fieldLifecycleSelfTest(App& app) {
    int problems = 0, titles = 0;
    app.persist = false;
    app.noEncounters = true;
    auto enter = [&](int stage) {
        app.runner.stop();
        app.pendingEntry = App::Entry::None;
        app.game = cr::GameState();
        app.playerChar = 0;
        app.mapNumber = app.entryArea = -1;
        cr::WarpRequest w;
        w.valid = true, w.area = cr::kPuzzleArea, w.map = cr::kFirstPuzzleMap + stage;
        const bool ok = performWarp(app, w, -1) && app.puzzle.stage == stage;
        app.pendingEntry = App::Entry::None;
        return ok;
    };
    // Every ability owns different objects/timers. A pending Retry must not rebuild a puzzle
    // from the title's event file after its old NPC list has been released.
    int ability = -1;
    for (int stage = 0; stage < cr::kPuzzleStages; ++stage) {
        if (cr::kPuzzleStage[stage].ability == ability) continue;
        ability = cr::kPuzzleStage[stage].ability;
        if (!enter(stage)) { ++problems; continue; }
        puzzle::reset(app);
        bootTitle(app);
        const bool clean = app.puzzle.stage == -1 && app.puzzle.objs.empty() && app.puzzle.grid.empty() &&
                           !app.puzzleStartPending && app.moves.empty() && app.rescales.empty();
        std::printf("field lifecycle: ability %d return to title %s (stage %d, %zu objects)\n",
                    ability, clean ? "ok" : "FAILED", app.puzzle.stage, app.puzzle.objs.size());
        problems += !clean;
        // Keep stepping past the cancelled retry's eight-frame delay.
        if (clean) {
            for (int f = 0; f < 12; ++f) stepEvents(app);
            problems += app.puzzle.stage != -1;
        }
        ++titles;
    }
    if (enter(0)) {
        ViewerHost host(app);
        auto n = std::find_if(app.npcs.begin(), app.npcs.end(), [](const cr::FieldNpc& n) { return n.id > 0; });
        if (n == app.npcs.end()) ++problems;
        else {
            const int id = n->id;
            const float scale[3] = {2, 2, 2};
            host.scaleNpc(id, scale, 100);
            const bool active = host.animBusy(id) && !app.rescales.empty();
            cr::WarpRequest w;
            w.valid = true, w.area = cr::kPuzzleArea, w.map = cr::kFirstPuzzleMap + 1;
            const bool clean = performWarp(app, w, -1) && app.rescales.empty() && !host.animBusy(id);
            std::printf("field lifecycle: NPC scale task active %d, next room %s\n", active, clean ? "ok" : "FAILED");
            problems += !active + !clean;
        }
    } else ++problems;
    // Interrupt an actual Teleport after its fade-out, then reload the captured room.
    int tpStage = -1;
    for (int stage = 0; stage < cr::kPuzzleStages; ++stage)
        if (cr::kPuzzleStage[stage].ability == cr::kAbilityTeleport && cr::kPuzzleStage[stage].esp != 0) {
            tpStage = stage; break;
        }
    if (tpStage >= 0 && enter(tpStage)) {
        const auto saved = captureSave(app);
        int target = -1;
        for (size_t i = 0; i < app.puzzle.objs.size(); ++i)
            if (puzzle::target(app, app.puzzle.objs[i])) { target = int(i); break; }
        if (target < 0) ++problems;
        else {
            setActor(app, 0, puzzle::centre(app, 0, 0), nullptr, false);
            puzzle::teleportTo(app, target);
            for (int f = 0; f < 4; ++f) stepEvents(app);
            const bool faded = app.puzzle.tpState != 0 && app.playerAlpha < 0.01f;
            const bool loaded = applySave(app, saved);
            const bool visible = loaded && app.playerAlpha == 1 && app.playerFadeLeft == 0 && !app.puzzle.tpState;
            std::printf("field lifecycle: interrupted teleport faded %d, reload visible %s\n", faded, visible ? "ok" : "FAILED");
            problems += !faded + !visible;
        }
    } else ++problems;
    problems += app.renderer.takeErrors();
    std::printf("field lifecycle: %d abilities, %d problems\n", titles, problems);
    app.trackerArmed = false;
    return problems;
}

// Real puzzle exit bytecode across area-file changes, both before and after loading a save.
int mobileTransitionSelfTest(App& app) {
    int problems = 0;
    auto settle = [&] {
        for (int quiet = 0, f = 0; f < 5000 && quiet < 10; ++f) {
            stepEvents(app);
            if (!app.dialogue.empty()) advanceDialogue(app);
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
            if (f % 64 == 0) { app.lastTicks = 0; drawFrame(app, false); }
            quiet = app.runner.running() || app.pendingEntry != App::Entry::None || app.tutoTex.on ||
                    app.fadeT < app.fadeFrames ? 0 : quiet + 1;
        }
    };
    for (int route = 0; route < 4; ++route) for (bool reload : {false, true}) for (bool solved : {false, true}) {
        app.runner.stop();
        app.game = cr::GameState();
        app.playerChar = 2 * route;
        app.game.route = route;
        app.game.setFlag(3070, true);
        app.game.setFlag(3000, !solved); // revisit a cleared room, or solve it from scratch
        app.mapNumber = app.entryArea = -1;
        cr::WarpRequest w;
        w.valid = true, w.area = cr::kPuzzleArea, w.map = cr::kFirstPuzzleMap;
        performWarp(app, w, -1);
        settle();
        if (solved) {
            std::vector<cr::Vec3> gems;
            for (const auto& o : app.puzzle.objs)
                if (o.type == cr::kTypeGem) gems.push_back(puzzle::centre(app, o.row, o.col));
            for (const auto& pos : gems) {
                setActor(app, 0, pos, nullptr, true);
                for (int f = 0; f < 3; ++f) stepEvents(app);
            }
            settle();
            problems += puzzle::gemsLeft(app) || !app.game.flag(3300);
        }
        if (reload) {
            const auto save = captureSave(app);
            // Loading must discard a previous scene's mask/camera before the arrival fade.
            app.fadeGrey = app.fadeFrom = app.fadeTo = 1;
            app.fadeT = app.fadeFrames = 1;
            problems += !applySave(app, save);
            settle();
        }
        // Stage 0 has a return for each route; use the real trigger's warp-table entry.
        const int event = 1 + 8 * route;
        cr::WarpEntry dest;
        const bool have = app.events.warpEntry(event, dest);
        problems += !have || !app.runner.start(app.events, event, app.game, app.area, app.mapNumber);
        app.runnerWhat = "mobile regression: crystal exit";
        settle();
        const bool ok = have && app.area == dest.area && app.mapNumber == dest.map && app.puzzle.stage == -1 &&
                        !app.runner.running() && screenFadeAlpha(app) < 0.01f;
        std::printf("mobile transition: route %d reload %d solved %d -> area %d map %d fade %.2f; %s\n",
                    route, reload, solved, app.area, app.mapNumber, screenFadeAlpha(app), ok ? "ok" : "FAILED");
        problems += !ok + app.renderer.takeErrors();
        app.lastTicks = 0;
        drawFrame(app, false);
    }
    app.trackerArmed = false;
    std::printf("mobile transitions: %d problems\n", problems);
    return problems;
}

int graphicsRecoverySelfTest(App& app) {
    const auto at = std::find(app.maps.begin(), app.maps.end(), "d0101_10.cmp");
    if (at == app.maps.end()) return 1;
    app.mode = Mode::Maps;
    showMap(app, int(at - app.maps.begin()), 0, 2);
    app.runner.stop();
    app.pendingEntry = App::Entry::None;
    app.showBackground = true;
    app.fadeFrames = 0;
    app.optionsOpen = app.charSelectOpen = app.saveScreen.open = false;
    app.dialogue = {{"Graphics recovery"}};
    app.dialoguePage = 0;
    app.dialogueInfo.resize(1);
    app.dialogueInfo[0].closeAfter = 0; // static panel, without the wall-clock "next" blink
    app.showHud = false;
    auto frame = [&] {
        app.lastTicks = 0; // freeze the scene during pixel comparisons
        drawFrame(app, false);
        std::vector<uint8_t> pixels;
        int w = 0, h = 0;
        app.renderer.readPixels(pixels, w, h);
        return pixels;
    };
    frame(); // populate lazy UI resources
    const auto before = frame();
    if (const char* file = SDL_getenv("CR_SELFTEST_GRAPHICS_CAPTURE")) saveFrame(app, file);
    // Require visible field pixels above the dialogue, not just matching blank frames.
    size_t visible = 0;
    for (size_t i = 0; i + 3 < before.size() / 2; i += 4)
        visible += int(before[i]) + before[i + 1] + before[i + 2] > 32;
    int problems = app.renderer.takeErrors() + (visible < before.size() / 8 / 10);
    std::printf("graphics recovery: visible field pixels %zu\n", visible);
    for (int cycle = 0; cycle < 3; ++cycle) {
        SDL_GL_DestroyContext(SDL_GL_GetCurrentContext());
        if (!SDL_GL_CreateContext(app.window)) return 1;
        SDL_Event event{};
        event.type = SDL_EVENT_RENDER_DEVICE_RESET;
        event.render.windowID = SDL_GetWindowID(app.window);
        if (SDL_AppEvent(&app, &event) != SDL_APP_CONTINUE) return 1;
        event.type = SDL_EVENT_USER; // next game-thread event consumes the queued reset
        if (SDL_AppEvent(&app, &event) != SDL_APP_CONTINUE) return 1;
        const auto after = frame();
        const bool sameFrame = !before.empty() && before == after;
        problems += !sameFrame + app.renderer.takeErrors();
        std::printf("graphics recovery: cycle %d, identical field/dialogue frame %s\n", cycle + 1, sameFrame ? "yes" : "NO");
    }
    std::printf("graphics recovery: %d problems\n", problems);
    return problems;
}

int sceneAudioSelfTest(App& app) {
    app.mode = Mode::Maps;
    const auto at = std::find(app.maps.begin(), app.maps.end(), "d0101_10.cmp");
    if (at == app.maps.end() || !startSound(app)) return 1;
    vitaAudio(app);
    showMap(app, int(at - app.maps.begin()), 0, 2);
    app.runner.stop();
    app.pendingEntry = App::Entry::None;
    int problems = 0, clip = -1;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { ++problems; std::printf("scene audio FAILED: %s\n", what); }
    };
    if (app.sound.hasVoices()) {
        for (int i = 0; i < app.sound.voiceClips(); ++i) {
            app.sound.playLine(i);
            if (app.sound.linePlaying()) { clip = i; break; }
        }
        check(clip >= 0, "a real story clip can play");
        if (clip >= 0) {
            app.dialogue = {{"Old page"}};
            app.dialogueVoices = {{clip, clip}};
            ++app.dialogueSerial;
            app.dialoguePage = 0;
            updateVoices(app);
            check(app.voiceQueue.size() == 1, "a page queues its second clip");
            app.dialogue.clear(); // a scene closes the window directly, without a page tap
            app.sound.stopLine(); // the first clip has finished
            const int played = app.voicesPlayed;
            updateVoices(app);
            check(app.voiceQueue.empty() && app.voicesPlayed == played && !app.sound.linePlaying(),
                  "a closed page cannot restart its remaining clip");
            app.sound.playLine(clip);
            app.dialogue = {{"Silent replacement"}};
            app.dialogueVoices = {{}};
            ++app.dialogueSerial;
            updateVoices(app);
            check(!app.sound.linePlaying(), "an unvoiced replacement stops the previous page's line");
            app.dialogue = {{"Kept"}, {"Next"}}; // _PrcMsg 0x1e 0xffff: the clip goes on over the next tap
            app.dialogueVoices = {{clip, -1}, {}};
            ++app.dialogueSerial;
            app.dialoguePage = 0;
            updateVoices(app);
            app.dialoguePage = 1;
            updateVoices(app);
            check(app.sound.linePlaying() && !app.voiceKeep, "0x1e 0xffff keeps the clip over the page turn");
            app.dialogue.clear();
            updateVoices(app);
            check(!app.sound.linePlaying(), "the closing tap cuts an unkept clip");
            app.voiceQueue = {clip};
        }
    }
    app.dialogue = {{"Interrupted by battle"}};
    // The Android speech-in-effects path exists in every data variant too.
    app.sound.play(0xc00000);
    check(app.sound.dialogueVoiceCount() > 0, "speech-marked looping sample is playing before battle");
    startBattle(app, {1, 0, -1, 0}, 0);
    check(app.battle.active, "real battle starts");
    app.sound.update(); // refill an old looping stream if battle entry failed to stop it
    check(app.dialogue.empty() && app.voiceQueue.empty() && app.sound.dialogueVoiceCount() == 0,
          "battle entry clears the field dialogue's audio and queued clips");
    if (app.battle.active) endBattle(app, false);
    app.runner.stop();
    app.sound.stopAll();
    bootTitle(app);
    check(app.voiceQueue.empty() && app.sound.dialogueVoiceCount() == 0, "title cleanup remains silent");
    problems += app.renderer.takeErrors();
    std::printf("scene audio: story clip %d, battle entry and title; %d problems\n", clip, problems);
    return problems;
}

int sessionUiSelfTest(App& app) {
    int problems = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { ++problems; std::printf("session UI FAILED: %s\n", what); }
    };
    const auto at = std::find(app.maps.begin(), app.maps.end(), "d0101_10.cmp");
    if (at == app.maps.end()) return 1;
    auto field = [&] {
        app.mode = Mode::Maps;
        app.playerChar = 0;
        showMap(app, int(at-app.maps.begin()), 0, 2);
        app.booting = app.titleOpen = false;
        app.game.setFlag(0x43, true); // Options tutorial already read in this fixture
        app.runner.stop();
        app.pendingEntry = App::Entry::None;
    };
    auto key = [&](SDL_Keycode code) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        SDL_AppEvent(&app, &event);
    };
    field();
    openCamp(app);
    campOpenPage(app, 4); // Camp -> Options, the real Return to Title path
    app.playing = false;
    key(SDLK_END); key(SDLK_RETURN);
    check(app.booting && !app.campOpen && !app.optionsOpen, "Camp cannot cover or capture input on the title");
    check(app.playing, "return to title resumes normal game updates");
    // This directory contains only this test's synthetic save, never player progress.
    const std::string keepDir = app.saveDirOverride;
    app.saveDirOverride = userDir()+"session-ui-test-"+std::to_string(SDL_GetTicksNS())+"/";
    SDL_CreateDirectory(app.saveDirOverride.c_str());
    field();
    cr::SaveData save = captureSave(app);
    save.game.oz = 54321;
    const std::string path = dataPath(app);
    check(cr::writeSave(path, save), "synthetic save created");
    { // a save cut short is refused, here and in the Vita import's order (the party last, no boxes)
        std::vector<uint8_t> whole;
        cr::readFile(path, whole);
        const std::string text(whole.begin(), whole.end());
        const auto cutsRefused = [&](const std::string& file, const std::string& last) {
            const size_t end = file.find('\n', file.rfind('\n' + last) + 1) + 1;
            bool refused = file.rfind('\n' + last) != std::string::npos;
            cr::SaveData got;
            for (size_t cut = 0; cut < file.size(); ++cut) {
                if (cut >= end && file[cut - 1] == '\n') continue; // (a save that ends here is an older one)
                SDL_SaveFile(path.c_str(), file.data(), cut);
                refused = refused && !cr::readSave(path, got);
            }
            return refused;
        };
        std::string imported, partyLines;
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);) {
            if (line.rfind("boxes", 0) == 0 || line.rfind("treasure", 0) == 0) continue;
            (line.rfind("party", 0) == 0 ? partyLines : imported) += line + '\n';
        }
        imported += partyLines;
        check(cutsRefused(text, "walkexp"), "a save cut short is refused");
        check(cutsRefused(imported, "party"), "a Vita import cut short is refused");
        cr::SaveData got;
        const auto valueRefused = [&](const std::string& key, int value) { // a whole save, a place no game has
            const size_t at = text.find('\n' + key + ' ') + 1 + key.size() + 1;
            const std::string file = text.substr(0, at) + std::to_string(value) + text.substr(text.find('\n', at));
            SDL_SaveFile(path.c_str(), file.data(), file.size());
            return !cr::readSave(path, got);
        };
        check(valueRefused("area", cr::kNumAreas) && valueRefused("map", cr::kNumMaps), "a save's place out of range is refused");
        SDL_SaveFile(path.c_str(), imported.data(), imported.size());
        check(cr::readSave(path, got) && got.game.oz == 54321, "a whole Vita import reads");
        SDL_SaveFile(path.c_str(), text.data(), text.size());
        check(cr::readSave(path, got) && got.game.oz == 54321, "a whole save still reads");
    }
    std::vector<uint8_t> before, after;
    cr::readFile(path, before);
    openSaveScreen(app, false);
    key(SDLK_V); key(SDLK_END); key(SDLK_RETURN);
    check(app.booting && !app.saveScreen.open && app.saveScreen.pending == 0,
          "return to title cancels a pending save confirmation");
    key(SDLK_RETURN); // a late confirmation must not overwrite the test's existing file
    cr::readFile(path, after);
    check(!before.empty() && after == before, "late Enter cannot overwrite the previous save from the title");
    // Clear broader overlays and detach an asynchronous picker from the old session.
    app.campOpen = app.manualOpen = app.scenariosOpen = app.charSelectOpen = true;
    app.itemWindowOpen = app.shop.open = app.shop.talkRunning = true;
    app.mapScreen.open = app.mapScreen.closing = app.staffRoll.open = app.lastBattle.open = true;
    app.resumePrompt = 1;
    app.menuMask.to = 160;
    app.touchPad.down = app.touchPad.shown = app.campPointer.down = true;
    app.optionsBarDrag = app.dragging = app.tapPending = true;
    holdSkip(app, kSkipKey, true);
    app.padHeld = 1;
    app.heldPage = true, app.lastPage = {"Old page"};
    app.saveImport.state = vitaimport::Session::Picking;
    app.saveImport.choice = std::make_shared<vitaimport::Session::Choice>();
    const auto oldPicker = app.saveImport.choice;
    const int fieldSpeed = app.fieldSpeed, volume = app.mix.master;
    bootTitle(app);
    check(!app.campOpen && !app.manualOpen && !app.scenariosOpen && !app.charSelectOpen &&
          !app.shop.open && !app.shop.talkRunning && !app.mapScreen.open && !app.mapScreen.closing &&
          !app.staffRoll.open && !app.lastBattle.open && !app.itemWindowOpen && !app.resumePrompt,
          "old overlays are closed at the title boundary");
    check(!app.touchPad.down && !app.campPointer.down && !app.dragging && !app.optionsBarDrag &&
          !app.tapPending && !app.skipScene && app.padHeld == -1 && !app.heldPage && app.lastPage.empty() && app.menuMask.to == 0,
          "old gestures, pending dialogue and menu fade are cleared");
    { std::lock_guard<std::mutex> lock(oldPicker->mutex); oldPicker->path = "/no-such-late-import-after-title"; oldPicker->ready = true; }
    app.saveImport.poll();
    check(!app.saveImport.active() && !app.saveImport.choice, "late folder-picker completion cannot reopen import over the title");
    check(app.fieldSpeed == fieldSpeed && app.mix.master == volume, "user preferences survive the session boundary");
    // Loading through the actual confirmation must retain the load-complete panel.
    app.chosenPair = 1;
    app.titleOpen = true;
    openSaveScreen(app, true);
    key(SDLK_RETURN); key(SDLK_RETURN);
    check(app.game.oz == 54321, "confirmed load restores the synthetic save's progress");
    check(app.saveScreen.open && app.saveScreen.phase == 2 && app.saveScreen.load &&
          app.saveScreen.haveSave && app.saveScreen.data.game.oz == 54321,
          "load-complete panel retains its mode and loaded save after session cleanup");
    for (int i = 0; i < 40; ++i) stepSaveScreen(app);
    check(!app.saveScreen.open && !app.titleOpen, "load-complete panel dismisses to the field");
    app.playSeconds = 1234.5;
    startNewGame(app);
    check(app.playSeconds == 0, "a completely fresh game resets the previous session's playtime");
    app.runner.stop();
    app.pendingEntry = App::Entry::None;
    app.playSeconds = 4321.5;
    app.resetAsk = 1;
    resetAnswer(app, false);
    check(app.playSeconds == 4321.5 && SDL_GetPathInfo(path.c_str(), nullptr), "declining reset keeps playtime and saves");
    save.playSeconds = 777;
    check(cr::writeSave(path, save), "synthetic save rewritten");
    app.playSeconds = 9999;
    bootTitle(app);
    check(app.playSeconds == 777, "the title goes back to the newest save's playtime");
    app.runner.stop();
    app.pendingEntry = App::Entry::None;
    app.playSeconds = 4321.5;
    app.resetAsk = 1;
    resetAnswer(app, true);
    check(app.resetAsk == 2 && app.playSeconds == 4321.5 && SDL_GetPathInfo(path.c_str(), nullptr), "first reset confirmation keeps playtime and saves");
    resetAnswer(app, true);
    check(app.playSeconds == 0 && !SDL_GetPathInfo(path.c_str(), nullptr), "confirmed data reset clears playtime with saved progress");
    app.playSeconds = 55;
    bootTitle(app);
    check(app.playSeconds == 0, "the title with no save starts the playtime from zero");
    app.runner.stop();
    app.pendingEntry = App::Entry::None;
    app.saveImport.cancel();
    SDL_RemovePath(path.c_str());
    SDL_RemovePath(app.saveDirOverride.c_str());
    app.saveDirOverride = keepDir;
    problems += app.renderer.takeErrors();
    std::printf("session UI: Camp/Options, save confirmation, late picker and title; %d problems\n", problems);
    return problems;
}

// New with the clear data from the title, as a player begins it (pair 1 cleared, its flag 40): the
// cleared save's own work - var 0, var 1 as a played game leaves them - must not take the boot
// script's over (prty_init_new leaves the work alone); its New goes on to the opening, map 88, as a
// plain one does, with var 1 = 3 and the pair's story flags cleared
int clearDataNewGameSelfTest(App& app, const cr::SaveData& base) {
    cr::SaveData sd = base;
    for (int f : {31, 32, 40, 1100}) sd.game.setFlag(f, true);
    sd.game.vars[0] = 7, sd.game.vars[1] = 4;
    sd.leader = 0;
    cr::writeSave(dataPath(app), sd);
    app.game = cr::GameState();
    bootTitle(app);
    for (int f = 0; f < 3000 && !app.titleOpen; ++f) stepEvents(app);
    titleChoose(app, 0);
    for (int f = 0; f < 300 && !app.charSelectOpen; ++f) stepEvents(app);
    if (app.charSelectPage != 0) charSelectTurn(app);
    app.charSelectCursor = 0;
    charSelectKey(app, SDLK_RETURN);
    for (int f = 0; f < 300 && !app.saveScreen.open; ++f) stepEvents(app);
    const std::vector<int> plates = savePlates(app);
    const auto newPlate = std::find(plates.begin(), plates.end(), 2);
    int opening = -1, var1 = -1;
    if (app.saveScreen.open && newPlate != plates.end()) {
        saveScreenChoose(app, int(newPlate - plates.begin())); // New: with the clear data?
        const bool clearQuestion = app.saveScreen.kind == 3;
        saveScreenYes(app);
        var1 = clearQuestion ? app.game.vars[1] : -2;
        for (int f = 0; f < 1500 && opening < 0; ++f) {
            stepEvents(app);
            if (!app.booting && app.mapNumber >= 0) opening = app.mapNumber;
        }
    }
    app.runner.stop();
    const int problems = (var1 != 3) + (opening != 88) + app.game.flag(1100) + !app.game.flag(40);
    std::printf("new game with clear data from the title: var 1 = %d, opening map %d; %d problems\n", var1, opening, problems);
    return problems;
}

// The field's collision as FieldPCCtrl::checkHit has it: the leader walked round every map (finite,
// on a floor, gets somewhere), walked into every solid NPC from four sides (never inside its box),
// up d0207's ramp (polygons 49 / 50, normal.y 0.68: floors by the KD lists, climbed on their plane)
// and onto a j001_p01 plate (a floor NPC: RectWrapper::getFloorHeight lifts it by the box's 3).
// Returns the walk-ins that ended inside a box; problems go to `mapErrors`.
int walkCollisionSelfTest(App& app, int& spawned, int& walked, int& mapErrors) {
    setMode(app, Mode::Maps);
    // On a floor: one within a unit of y (checkHit takes the floor before the pushes move x / z, so on a
    // slope y can trail by a push's rise), or a floor NPC's top; with no floor under it at all, y must
    // stay as it was (getFloor finds none: x and z move on - past a floor's edge with no wall there)
    auto floorUnder = [&](const cr::Vec3& p, float& fy) {
        return cr::floorHeight(app.collisionData, p.x, p.y + 1000.0f, p.z, 2000.0f, fy) ||
               cr::bodyFloorHeight(walkBodies(app), p, 1000.0f, fy);
    };
    auto onFloor = [&](const cr::Vec3& p) {
        float fy;
        return (cr::floorHeight(app.collisionData, p.x, p.y + 1.0f, p.z, 0.0f, fy) && fy > p.y - 1.0f) ||
               (cr::bodyFloorHeight(walkBodies(app), p, 1000.0f, fy) && std::fabs(fy - p.y) < 0.02f);
    };
    // Walk the leader around for 6 seconds of game time (stick turning a full circle) - then stand 20
    // frames, the floor and the pushes still running
    int offEdge = 0;
    for (size_t i = 0; i < app.maps.size(); ++i) {
        showMap(app, int(i));
        if (!app.havePlayer) continue;
        ++spawned;
        const cr::Vec3 start = app.walker.pos;
        const std::vector<cr::WalkBody> bodies = walkBodies(app);
        float far = 0;
        bool ok = true, off = false;
        for (int f = 0; f < 140; ++f) {
            const float a = float(f) / 120.0f * 6.2831853f;
            const bool still = f >= 120;
            const float wasY = app.walker.pos.y;
            cr::stepWalker(app.walker, app.collisionData, bodies, still ? 0.0f : std::sin(a), still ? 0.0f : -std::cos(a),
                           f % 40 < 20, 0.0f);
            const cr::Vec3 p = app.walker.pos;
            float fy;
            const bool none = !floorUnder(p, fy);
            off = off || none;
            const bool good = std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && (none ? p.y == wasY : onFloor(p));
            if (ok && !good && SDL_getenv("CR_WALK_DEBUG"))
                std::printf("  off the floor at frame %d: (%.2f %.2f %.2f), was y %.2f\n", f, p.x, p.y, p.z, wasY);
            ok = ok && good;
            const cr::Vec3 d = p - start;
            far = std::max(far, std::sqrt(cr::dot(d, d)));
        }
        if (!ok) {
            ++mapErrors;
            std::printf("player left the floor in %s\n", app.maps[i].c_str());
        }
        offEdge += off;
        walked += far > 5.0f;
    }
    std::printf("walk: on %d maps the leader went past a floor's edge (no wall there), y held\n", offEdge);
    // Walk into every solid NPC from outside its box: the player must never end up inside it
    auto wallBetween = [&](cr::Vec3 a, cr::Vec3 b) { // a wall polygon (either side) the segment a-b crosses
        const cr::Collision& col = app.collisionData;
        for (const cr::CollisionPolygon& w : col.polygons) {
            if (!w.isWall()) continue;
            const cr::Vec3 n{w.normal[0], w.normal[1], w.normal[2]};
            auto vert = [&](int k) { const float* v = &col.vertices[size_t(w.index[k]) * 3]; return cr::Vec3{v[0], v[1], v[2]}; };
            const float da = cr::dot(a - vert(0), n), db = cr::dot(b - vert(0), n);
            if ((da > 0) == (db > 0)) continue;
            const cr::Vec3 x = a + (b - a) * (da / (da - db));
            float sgn = 0;
            bool inside = true;
            for (int k = 0; k < w.count && inside; ++k) {
                const float sk = cr::dot(n, cr::cross(vert((k + 1) % w.count) - vert(k), x - vert(k)));
                inside = sk * sgn >= 0;
                if (sk != 0) sgn = sk;
            }
            if (inside) return true;
        }
        return false;
    };
    int solidTests = 0, entered = 0;
    for (int area0 = 0; area0 < cr::kNumAreas; ++area0)
        for (int map0 = 0; map0 < cr::kNumMaps; ++map0) {
            if (cr::areaForMap(map0, area0) != area0) continue;
            const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map0]));
            if (at == app.maps.end()) continue;
            showMap(app, int(at - app.maps.begin()), map0, area0);
            if (!app.havePlayer) continue;
            const std::vector<cr::WalkBody> bodies = walkBodies(app);
            for (const cr::WalkBody& b : bodies) {
                if (!b.wall || !b.shown || b.box[0] <= 0 || b.box[2] <= 0) continue;
                // the box as RectWrapper turns it: corners (offset +- size / 2) about the position
                const float s = std::sin(b.angle), c = std::cos(b.angle);
                float px[4], pz[4], cx = 0, cz = 0, reach = 0;
                for (int q = 0; q < 4; ++q) {
                    const float lx = b.offset[0] + (q == 1 || q == 2 ? 0.5f : -0.5f) * b.box[0];
                    const float lz = b.offset[2] + (q >= 2 ? 0.5f : -0.5f) * b.box[2];
                    px[q] = b.pos.x + s * lz + c * lx, pz[q] = b.pos.z + c * lz - s * lx;
                    cx += px[q] / 4, cz += pz[q] / 4;
                }
                for (int q = 0; q < 4; ++q) reach = std::max(reach, std::hypot(px[q] - cx, pz[q] - cz));
                reach += app.walker.radius + 2; // start outside the box
                for (int dir = 0; dir < 4; ++dir) {
                    const float a = dir * 1.5707963f;
                    const cr::Vec3 from{cx + reach * std::sin(a), b.pos.y + 5, cz + reach * std::cos(a)};
                    float fy;
                    if (!cr::floorHeight(app.collisionData, from.x, from.y, from.z, 3.0f, fy)) continue;
                    // (RectWrapper's sphere sits at y + 2.5: a box above or below that passes over / under it;
                    // and a start behind a one-sided wall can be pushed through it)
                    const float y0 = b.pos.y + b.offset[1], centreY = fy + 2.5f;
                    if (centreY < y0 || centreY > y0 + b.box[1] || wallBetween({from.x, centreY + 2.5f, from.z}, {cx, centreY + 2.5f, cz}))
                        continue;
                    app.walker.pos = {from.x, fy, from.z};
                    ++solidTests;
                    bool inside = false;
                    for (int f = 0; f < 120 && !inside; ++f) {
                        const cr::Vec3 d{cx - app.walker.pos.x, 0, cz - app.walker.pos.z}; // camYaw 0: the stick is (world x, world z)
                        cr::stepWalker(app.walker, app.collisionData, bodies, d.x, d.z, false, 0.0f);
                        int in = 0; // on the inner side of all four sides
                        for (int q = 0; q < 4; ++q) {
                            const int r = (q + 1) % 4;
                            const float ex = px[r] - px[q], ez = pz[r] - pz[q];
                            in += ex * (app.walker.pos.z - pz[q]) - ez * (app.walker.pos.x - px[q]) > 0;
                        }
                        inside = in == 4;
                    }
                    if (inside && (entered < 8 || SDL_getenv("CR_WALK_DEBUG")))
                        std::printf("  inside: %s map %d box centre (%.1f %.1f) reach %.1f, from dir %d, now (%.1f %.1f) y %.1f; "
                                    "body y %.1f box %.1f %.1f %.1f offset %.1f %.1f %.1f angle %.3f r %.1f\n",
                                    cr::kAreaEventFile[area0], map0, cx, cz, reach, dir, app.walker.pos.x, app.walker.pos.z,
                                    app.walker.pos.y, b.pos.y, b.box[0], b.box[1], b.box[2], b.offset[0], b.offset[1], b.offset[2], b.angle, app.walker.radius);
                    entered += inside;
                }
            }
        }
    std::printf("solid npcs: %d walk-ins, %d ended inside a box\n", solidTests, entered);
    auto showByName = [&](const char* cmp, const char* evt) {
        for (int area0 = 0; area0 < cr::kNumAreas; ++area0)
            for (int map0 = 0; map0 < cr::kNumMaps; ++map0) {
                if (cr::areaForMap(map0, area0) != area0 || std::string(cr::kMapCollisionFile[map0]) != cmp) continue;
                if (evt && std::string(cr::kAreaEventFile[area0]) != evt) continue;
                const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map0]));
                if (at == app.maps.end()) continue;
                showMap(app, int(at - app.maps.begin()), map0, area0);
                if (app.havePlayer) return true;
            }
        return false;
    };
    // d0207's ramp: up from its foot (y 30, z -60.9) to its top (y 54.1, z -83.0), x 168..188
    int rampProblems = 0;
    float rampTop = -1e9f, rampWorst = 0;
    int rampSteps = 0;
    if (showByName("d0207_10.cmp", nullptr)) {
        const std::vector<cr::WalkBody> bodies = walkBodies(app);
        app.walker.pos = {178.0f, 30.0f, -58.0f};
        app.walker.velocity = {};
        cr::checkWalkerHit(app.walker, app.collisionData, bodies);
        for (int f = 0; f < 70; ++f) {
            cr::stepWalker(app.walker, app.collisionData, bodies, 0.0f, -1.0f, false, 0.0f); // walk -z
            const cr::Vec3 p = app.walker.pos;
            if (p.z < -61.5f && p.z > -82.5f) { // on the ramp: on its plane
                ++rampSteps;
                rampWorst = std::max(rampWorst, std::fabs(p.y - (30.0f + (-60.9f - p.z) * (24.1f / 22.1f))));
            }
            rampTop = std::max(rampTop, p.y);
        }
        rampProblems = rampSteps < 30 || rampWorst > 0.2f || rampTop < 54.0f;
    } else rampProblems = 1;
    std::printf("ramp (d0207 49 / 50): %d steps on it, %.3f off its plane at worst, up to y %.1f; %d problems\n", rampSteps,
                rampWorst, rampTop, rampProblems);
    // j001_p01's plates (NPCs 153..207, model 32, kind bit 4, box 10 x 3 x 10): the leader stands on top
    int plateProblems = 1, plateId = -1;
    float plateY = 0, plateWant = 0, besideY = 0, besideWant = 0;
    for (int area0 = 0; area0 < cr::kNumAreas && plateProblems; ++area0) {
        if (std::string(cr::kAreaEventFile[area0]) != "j001_p01.evt") continue;
        for (int map0 = 0; map0 < cr::kNumMaps && plateProblems; ++map0) {
            if (cr::areaForMap(map0, area0) != area0) continue;
            const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map0]));
            if (at == app.maps.end()) continue;
            showMap(app, int(at - app.maps.begin()), map0, area0);
            if (!app.havePlayer) continue;
            for (const cr::FieldNpc& n : app.npcs) {
                if (n.id < 153 || n.id > 207 || !(n.kind & 4) || n.box[1] <= 0) continue;
                app.npcShown[n.id] = true; // (shown whatever the room's flags say)
                const std::vector<cr::WalkBody> bodies = walkBodies(app);
                const float cx = n.pos[0] + n.offset[0], cz = n.pos[2] + n.offset[2];
                float floorY;
                const int node = cr::searchNode(app.collisionData, {cx, n.pos[1], cz}, 5.0f);
                if (cr::getFloor(app.collisionData, node, {cx, n.pos[1], cz}, 1000.0f, floorY) < 0) continue;
                plateId = n.id;
                plateWant = n.pos[1] + n.offset[1] + n.box[1];
                app.walker.pos = {cx, floorY, cz};
                app.walker.velocity = {};
                cr::checkWalkerHit(app.walker, app.collisionData, bodies);
                plateY = app.walker.pos.y;
                // off its box (just past the side): back on the floor below
                const cr::Vec3 beside{cx + n.box[0] * 0.5f + 0.5f, plateY, cz};
                const int node2 = cr::searchNode(app.collisionData, beside, 5.0f);
                if (cr::getFloor(app.collisionData, node2, beside, 1000.0f, besideWant) < 0) continue;
                app.walker.pos = beside;
                bool otherPlate = false;
                for (const cr::WalkBody& b : bodies)
                    otherPlate = otherPlate || (b.floor && b.shown && std::fabs(beside.x - b.pos.x - b.offset[0]) <= b.box[0] * 0.5f &&
                                                std::fabs(beside.z - b.pos.z - b.offset[2]) <= b.box[2] * 0.5f);
                if (otherPlate) continue;
                cr::checkWalkerHit(app.walker, app.collisionData, bodies);
                besideY = app.walker.pos.y;
                plateProblems = std::fabs(plateY - plateWant) > 0.01f || plateWant < floorY + 2.9f || std::fabs(besideY - besideWant) > 0.01f;
                break;
            }
        }
    }
    std::printf("plate (j001_p01 npc %d): stands at y %.2f (top %.2f), beside it %.2f (floor %.2f); %d problems\n", plateId,
                plateY, plateWant, besideY, besideWant, plateProblems);
    mapErrors += rampProblems + plateProblems;
    return entered;
}

// The 2026-11-13 audit's menu items, as 2.2.0 has them: the Sexy Pose price (__menuu_ShopPrc 6:
// price - price / 10), the Camp's healing on the home maps (CampMainSet), the Equip list's grey empty
// slots (__MuScrList_ScrListEquip_Main), the held-back member's spare jewel (MuComMakeItemData), and
// the Android staff roll's measures (InitStaffRollMenu)
int menusAuditSelfTest(App& app) {
    const cr::GameState keepGame = app.game;
    const int keepChar = app.playerChar, keepMap = app.mapNumber;
    int problems = 0;
    // F8: a price that isn't a multiple of 10 loses its tenth rounded down
    app.game = cr::GameState();
    app.playerChar = 0;
    int priced = 0, posePrice = -1, plainPrice = -1;
    for (int it = 1; it <= 438 && !priced; ++it)
        if (const cr::ItemParam* ip = app.itemTable.item(it); ip && ip->price % 10 && ip->price > 10) priced = it;
    for (int c = 0; c < 2 && posePrice < 0; ++c)
        for (int pl = cr::kFirstPlate; pl < cr::kFirstPlate + cr::kPlates && posePrice < 0; ++pl) {
            const auto& e = cr::plateContents(pl);
            if (std::find(e.begin(), e.end(), 173) == e.end()) continue;
            app.game.items[173] = 1, app.game.items[pl] = 1, app.game.party[size_t(c)].genes[1] = pl;
            if (cr::hasSkill(app.game, c, 173)) posePrice = priced ? shopUnitPrice(app, priced) : -1;
            else app.game.party[size_t(c)].genes[1] = 0;
        }
    app.game = cr::GameState();
    plainPrice = priced ? shopUnitPrice(app, priced) : -1;
    const bool f8 = priced && plainPrice > 0 && posePrice == plainPrice - plainPrice / 10;
    problems += !f8;
    // S7: the Camp opened on a home map (0x61..0x71, 0x53) heals the pair; elsewhere not
    app.game = cr::GameState();
    app.game.setFlag(0x38, true);
    app.game.party[0].hp = 5, app.game.party[1].mp = 3, app.game.party[2].hp = 7;
    app.mapNumber = 97;
    openCamp(app);
    const bool healed = app.game.party[0].hp < 0 && app.game.party[1].mp < 0 && app.game.party[2].hp == 7;
    app.campOpen = false;
    app.game.party[0].hp = 5;
    app.mapNumber = 42;
    openCamp(app);
    const bool notHealed = app.game.party[0].hp == 5;
    app.campOpen = false;
    problems += !healed + !notHealed;
    // S8: the empty jewel slot grey without a free jewel, the empty plate slots without a plate to wear
    app.game = cr::GameState();
    const bool grey = !campFreeJewel(app) && !campCommonSkill(app);
    app.game.items[426] = 1;
    int entry = 0;
    for (int e : cr::plateContents(cr::kFirstPlate + 2))
        if ((e >= 0x59 && e <= 0x71) || (e >= 0x8b && e <= 0xba)) { entry = e; break; }
    app.game.items[cr::kFirstPlate + 2] = 1, app.game.items[entry] = 1;
    const bool white = campFreeJewel(app) && entry && campCommonSkill(app);
    problems += !grey + !white;
    // S9: Musiea held back (flag 0xfed) wearing jewel 426, a spare 426 in the bag: listed for Escher
    app.game = cr::GameState();
    app.game.setFlag(0xfed, true);
    app.game.party[1].equip[cr::kJewel] = 426;
    app.game.items[426] = 1;
    bool spare = false;
    for (int k = 0; k < campui::kCategories; ++k)
        if (campui::kCategoryKind[k] == 8) {
            const std::vector<int> l = campItems(app, k);
            spare = std::find(l.begin(), l.end(), 426) != l.end();
        }
    const std::vector<int> eq = campEquipList(app, 0, cr::kJewel);
    spare = spare && std::find(eq.begin(), eq.end(), 426) != eq.end();
    problems += !spare;
    // S10: the Android phone's staff roll (oy 20: a 4:3 screen) - the picture's end, the lines shown
    App::StaffRoll st;
    st.phone = true;
    const bool iphone = staffPicEnd(st) == -575 && staffShown(st) == 17;
    st.edgeY = 20;
    const bool android = staffPicEnd(st) == -615 && staffShown(st) == 19;
    problems += !iphone + !android;
    std::printf("menus audit: Sexy Pose %d -> %d %s, Camp heals at home %s and not away %s, empty slots grey %s / white %s, "
                "held-back spare jewel listed %s, Android staff roll %s / %s; %d problems\n",
                plainPrice, posePrice, f8 ? "ok" : "WRONG", healed ? "ok" : "NO", notHealed ? "ok" : "NO", grey ? "ok" : "NO",
                white ? "ok" : "NO", spare ? "ok" : "NO", iphone ? "ok" : "WRONG", android ? "ok" : "WRONG", problems);
    app.game = keepGame;
    app.playerChar = keepChar, app.mapNumber = keepMap;
    return problems;
}

int runSelfTest(App& app) {
    app.persist = false;
    app.trophy.live = false; // (none unlocked by the other checks - trophiesSelfTest turns them on for its own)
    app.seenFlags.clear(); // (the prompts seen: none - the tests expect each first showing)
    app.typeOn = false; // pages whole (the typing has its own test)
    app.video.layout = 1; // the iPad layout the tests measure (the iPhone one has its own)
    applyVideo(app);
    app.testNoTutorials = true; // the battles' tutorials only where a test asks for them
    app.renderer.setVSync(false); // as fast as it renders, not at the display's rate
    if (const char* dir = SDL_getenv("CR_SELFTEST_FOLD")) { // a COPY of a folder of per-pair saves (save1..4.dat, no data.dat):
        // folded into data.dat once, the files left as they were, each pair loaded from its own place
        app.saveDirOverride = std::string(dir) + (dir[0] && dir[std::strlen(dir) - 1] == '/' ? "" : "/");
        if (SDL_GetPathInfo(dataPath(app).c_str(), nullptr)) { std::fprintf(stderr, "fold: %s has a data.dat already\n", dir); return 1; }
        int problems = 0, pairs = 0;
        if (const char* vita = SDL_getenv("CR_SELFTEST_FOLD_VITA")) { // first a Vita export imported there (its files only read)
            try {
                const vitaimport::Plan plan = vitaimport::prepare(vita, "cr1");
                vitaimport::install(plan, app.saveDirOverride);
                std::vector<std::string> names;
                for (const auto& f : plan.files) names.push_back(f.first);
                problems += !foldVitaImport(app, names);
                std::printf("fold: Vita import of %d slot(s) installed and folded\n", plan.slots);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "fold: Vita import failed: %s\n", e.what());
                return 1;
            }
        }
        std::vector<uint8_t> was[4], now[4];
        cr::SaveData own[4];
        bool have[4] = {};
        for (int p = 0; p < 4; ++p)
            if ((have[p] = cr::readSave(slotPath(app, p + 1), own[p]))) cr::readFile(slotPath(app, p + 1), was[p]), ++pairs;
        cr::SaveData d;
        problems += !readData(app, d);
        cr::SaveData written;
        problems += !cr::readSave(dataPath(app), written);
        for (int p = 0; p < 4; ++p) {
            if (!have[p]) { problems += d.places[size_t(p)].has; continue; }
            cr::readFile(slotPath(app, p + 1), now[p]);
            problems += now[p] != was[p];
            const cr::SaveData& o = own[size_t(p)];
            cr::SaveData l = d;
            l.takePlace(p), cr::changeRoute(l.game, &app.itemTable, p);
            int bad = !d.places[size_t(p)].has + (l.area != o.area) + (l.map != o.map) + (l.leader != o.leader);
            for (int k = 0; k < 2; ++k) {
                const cr::PartyMember &a = l.game.party[size_t(2 * p + k)], &b = o.game.party[size_t(2 * p + k)];
                bad += a.level != b.level || a.exp != b.exp || !std::equal(std::begin(a.equip), std::end(a.equip), std::begin(b.equip));
            }
            for (const auto& [item, n] : o.game.items) { // the pair's own items as its file had them; the shared at least as many
                const cr::ItemHome h = cr::itemHome(&app.itemTable, item);
                const int got = l.game.items.count(item) ? l.game.items.at(item) : 0;
                if (n > 0 && (h == cr::ItemHome::Shared || h == cr::ItemHome::Gear ? got < n : got != n)) ++bad;
            }
            for (size_t w = 0; w < o.game.flags.size(); ++w) bad += (l.game.flags[w] & o.game.flags[w]) != o.game.flags[w];
            if (!applySave(app, l)) ++bad;
            else for (int i = 0; i < 2; ++i) drawFrame(app, false);
            bad += !app.havePlayer || app.mapNumber != o.map;
            std::printf("fold: pair %d - area %d map %d leader %d, members %d / %d, %zu items in view, Oz %d: %s\n", p + 1, l.area, l.map, l.leader,
                        l.game.party[size_t(2 * p)].level, l.game.party[size_t(2 * p + 1)].level, l.game.items.size(), l.game.oz, bad ? "WRONG" : "ok");
            problems += bad;
        }
        std::printf("fold: %d per-pair saves into data.dat (in play pair %d, Oz %d); %d problems\n", pairs, d.route() + 1, d.game.oz, problems);
        return problems ? 1 : 0;
    }
    if (const char* file = SDL_getenv("CR_SELFTEST_SAVE")) {
        cr::SaveData save;
        std::string error;
        if (!cr::readSave(file, save, &error) || !applySave(app, save)) {
            std::fprintf(stderr, "imported save: %s\n", error.c_str());
            return 1;
        }
        for (int i = 0; i < 3; ++i) drawFrame(app, false);
        const int problems = !app.havePlayer || app.mapNumber != save.map || app.renderer.takeErrors() != 0;
        if (SDL_getenv("CR_SELFTEST_SAVE_DEEP")) { // the save's numbers in use: the Camp's pages, then a battle
            const auto key = [&](SDL_Keycode k) {
                SDL_Event e{};
                e.type = SDL_EVENT_KEY_DOWN, e.key.key = k, e.key.down = true;
                SDL_AppEvent(&app, &e);
                drawFrame(app, false);
            };
            for (int plate = 0; plate < 4; ++plate) {
                openCamp(app);
                app.camp.cursor = plate;
                key(SDLK_RETURN);
                for (SDL_Keycode k : {SDLK_RIGHT, SDLK_DOWN, SDLK_RETURN, SDLK_DOWN, SDLK_RIGHT, SDLK_RETURN}) key(k);
                for (int i = 0; i < 5 && app.campOpen; ++i) key(SDLK_ESCAPE); // (Esc on the field quits)
            }
            startBattle(app, {1, 0, -1, 0}, 0);
            for (int f = 0; f < 400; ++f) {
                drawFrame(app, false);
                stepBattle(app);
                if (f % 25 == 0) key(SDLK_RETURN);
            }
        }
        std::printf("imported save: area %d map %d, leader %d, level %d; %d problems\n", app.area, app.mapNumber,
                    app.playerChar, app.game.party[size_t(app.playerChar)].level, problems);
        return problems ? 1 : 0;
    }
    if (SDL_getenv("CR_SELFTEST_SESSION_UI")) return sessionUiSelfTest(app) ? 1 : 0;
    if (SDL_getenv("CR_SELFTEST_SCENE_AUDIO")) return sceneAudioSelfTest(app) ? 1 : 0;
    if (SDL_getenv("CR_SELFTEST_GRAPHICS")) return graphicsRecoverySelfTest(app) ? 1 : 0;
    if (SDL_getenv("CR_SELFTEST_CLEAR_NEW")) { // only New with the clear data from the title, in a folder of its own
        const std::string keep = app.saveDirOverride;
        app.saveDirOverride = userDir() + "clear-new-test-" + std::to_string(SDL_GetTicksNS()) + "/";
        SDL_CreateDirectory(app.saveDirOverride.c_str());
        setMode(app, Mode::Maps);
        startNewGame(app);
        const int problems = clearDataNewGameSelfTest(app, captureSave(app));
        for (const char* f : {"save1.dat", "save2.dat", "save3.dat", "save4.dat", "data.dat", "resume.dat"}) SDL_RemovePath(savePath(app, f).c_str());
        SDL_RemovePath(app.saveDirOverride.c_str());
        app.saveDirOverride = keep;
        return problems ? 1 : 0;
    }
    if (SDL_getenv("CR_SELFTEST_CANCEL")) return inputCancelSelfTest(app) ? 1 : 0;
    if (SDL_getenv("CR_SELFTEST_LIFECYCLE")) return fieldLifecycleSelfTest(app) ? 1 : 0;
    if (SDL_getenv("CR_SELFTEST_MOBILE")) return mobileInputSelfTest(app) + mobileTransitionSelfTest(app) ? 1 : 0;
    if (SDL_getenv("CR_SELFTEST_CONTROLS")) return controlHintsSelfTest(app) ? 1 : 0;
    if (SDL_getenv("CR_SELFTEST_TROPHIES")) return trophiesSelfTest(app) ? 1 : 0;
    if (SDL_getenv("CR_SELFTEST_FIELD")) return fieldPresentationSelfTest(app) ? 1 : 0;
    if (SDL_getenv("CR_SELFTEST_FIELD_RENDER")) return fieldRenderSelfTest(app) ? 1 : 0;
    if (SDL_getenv("CR_SELFTEST_WALK")) { // only the walking collision pass
        int spawned = 0, walked = 0, problems = 0;
        problems += walkCollisionSelfTest(app, spawned, walked, problems);
        std::printf("walk: player spawned on %d maps, walked > 5 units on %d; %d problems\n", spawned, walked, problems);
        return problems ? 1 : 0;
    }
    if (SDL_getenv("CR_SELFTEST_BATTLES")) { // only the battle pass
        int frames = 0, glErrors = 0;
        setMode(app, Mode::Maps);
        return battleSelfTest(app, frames, glErrors) ? 1 : 0;
    }
    int glErrors = controlHintsSelfTest(app) + trophiesSelfTest(app) + fieldPresentationSelfTest(app) + fieldRenderSelfTest(app), frames = 0, failedModels = 0;
    setMode(app, Mode::Models);
    for (size_t i = 0; i < app.models.size(); ++i) {
        showModel(app, int(i));
        int errs = 0;
        const int steps = int(app.model.actions.size()) + 1;
        for (int a = 0; a < steps; ++a) {
            for (int f = 0; f < 3; ++f) {
                animateModel(app, 0.25f);
                drawFrame(app);
                ++frames;
            }
            errs += app.renderer.takeErrors();
            selectAction(app, 1);
        }
        if (errs) {
            ++failedModels;
            glErrors += errs;
            std::printf("GL errors in %s: %d\n", app.models[i].c_str(), errs);
        }
    }
    // Every field map (collision + props)
    setMode(app, Mode::Maps);
    int mapErrors = 0, camMaps = 0, spawned = 0, walked = 0;
    for (size_t i = 0; i < app.maps.size(); ++i) {
        showMap(app, int(i));
        camMaps += app.haveStageCam;
        for (int f = 0; f < 2; ++f) { // 3D view, then the composite (background + stage camera)
            app.showBackground = f == 1;
            app.lastTicks = 0; // no game time passes here, however long the load took
            drawFrame(app);
            ++frames;
        }
        app.showBackground = false;
        int errs = 0;
        errs += app.renderer.takeErrors();
        if (errs || app.collisionPolys == 0) {
            ++mapErrors;
            std::printf("map problem in %s: %d GL errors, %d polygons\n", app.maps[i].c_str(), errs, app.collisionPolys);
        }
        glErrors += errs;
    }
    // Every exit of every map: stand in its box, fire it, check where the player arrives
    int exits = 0, arrived = 0, onFloor = 0, exitFailures = 0;
    for (int area0 = 0; area0 < cr::kNumAreas; ++area0)
    for (int map0 = 0; map0 < cr::kNumMaps; ++map0) {
        if (cr::areaForMap(map0, area0) != area0) continue;
        const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map0]));
        if (at == app.maps.end()) continue;
        const int i = int(at - app.maps.begin());
        showMap(app, i, map0, area0);
        const int area = app.area, map = app.mapNumber;
        const std::vector<cr::FieldNpc> npcs = app.npcs;
        const std::vector<bool> warps = app.npcWarps;
        for (size_t k = 0; k < npcs.size(); ++k) {
            if (!warps[k] || !npcActive(app, npcs[k])) continue;
            ++exits;
            if (app.mapNumber != map || app.area != area) showMap(app, i, map, area);
            cr::GameState trial = app.game;
            const cr::EventResult r = cr::runEvent(app.events, npcs[k].id, trial, area, map);
            if (!app.havePlayer) { ++exitFailures; continue; }
            app.walker.pos = {npcs[k].boxCenterX(), npcs[k].pos[1], npcs[k].boxCenterZ()};
            app.lastKick = 0;
            const bool warped = checkTriggers(app, true);
            const int wantArea = r.warp.area >= 0 ? r.warp.area : area;
            if (!warped || app.mapNumber != r.warp.map || app.area != wantArea) {
                ++exitFailures;
                std::printf("exit problem: %s map %d event %d -> map %d: %s\n", cr::kAreaEventFile[area], map,
                            npcs[k].id, r.warp.map, warped ? "arrived elsewhere" : "did not warp");
                continue;
            }
            ++arrived;
            float fy;
            const cr::Vec3 p = app.walker.pos;
            if (cr::floorHeight(app.collisionData, p.x, p.y + 0.01f, p.z, 0.0f, fy) && std::fabs(fy - p.y) < 0.05f) ++onFloor;
            else std::printf("exit lands off the floor: %s map %d event %d -> map %d at (%.1f %.1f %.1f)\n",
                             cr::kAreaEventFile[area], map, npcs[k].id, app.mapNumber, p.x, p.y, p.z);
        }
    }
    std::printf("exits: %d fired, %d arrived on the right map, %d on a floor, %d problems\n", exits, arrived, onFloor,
                exitFailures);
    // Every visible NPC of every (area, map) should stand on a floor
    int npcsDrawn = 0, npcsOnFloor = 0, npcMaps = 0;
    std::map<std::string, int> offFloor;
    for (int area0 = 0; area0 < cr::kNumAreas; ++area0)
        for (int map0 = 0; map0 < cr::kNumMaps; ++map0) {
            if (cr::areaForMap(map0, area0) != area0) continue;
            const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map0]));
            if (at == app.maps.end()) continue;
            showMap(app, int(at - app.maps.begin()), map0, area0);
            npcMaps += !app.npcDraws.empty();
            for (const auto& d : app.npcDraws) {
                ++npcsDrawn;
                const float x = d.world.m[12], y = d.world.m[13], z = d.world.m[14];
                float fy;
                if (cr::floorHeight(app.collisionData, x, y, z, 2.0f, fy) && std::fabs(fy - y) < 2.0f) ++npcsOnFloor;
                else ++offFloor[d.model];
            }
        }
    const int entered = walkCollisionSelfTest(app, spawned, walked, mapErrors);
    // Examine everything examinable on every (area, map)
    int examined = 0, gotItems = 0, lidsHidden = 0, examineWarps = 0, examineErrors = 0;
    std::map<std::string, int> examinedByModel;
    for (int area0 = 0; area0 < cr::kNumAreas; ++area0)
        for (int map0 = 0; map0 < cr::kNumMaps; ++map0) {
            if (cr::areaForMap(map0, area0) != area0) continue;
            const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map0]));
            if (at == app.maps.end()) continue;
            showMap(app, int(at - app.maps.begin()), map0, area0);
            std::vector<cr::FieldNpc> targets;
            for (const cr::FieldNpc& n : app.npcs)
                if ((n.kind & 8) && npcActive(app, n) && n.pos[1] < 900.0f &&
                    !(n.pos[0] == 0 && n.pos[1] == 0 && n.pos[2] == 0))
                    targets.push_back(n);
            for (const cr::FieldNpc& n : targets) {
                if (app.mapNumber != map0 || app.area != area0) showMap(app, int(at - app.maps.begin()), map0, area0);
                if (!app.havePlayer) break;
                const float k = n.model == 0 || n.model == 1 ? 1.0f : 0.0f;
                const float cx = n.pos[0] + k * n.offset[0] + n.offset[0], cz = n.pos[2] + k * n.offset[2] + n.offset[2];
                app.walker.pos = {cx + n.box[0] / 2 + app.walker.radius, n.pos[1], cz}; // just beside it
                const cr::FieldNpc* t = interactTarget(app);
                if (!t) continue;
                const size_t drawsBefore = app.npcDraws.size();
                ++examined;
                ++examinedByModel[n.model >= 0 ? cr::kNpcModelFile[n.model] : "none"];
                const bool warped = interact(app, true);
                if (app.mapNumber != map0) { ++examineWarps; continue; }
                const bool gave = app.lastEvent.find(", got ") != std::string::npos;
                gotItems += gave;
                lidsHidden += (n.treasure & 3) && app.npcDraws.size() < drawsBefore;
                if ((n.treasure & 3) && !gave)
                    std::printf("  box without item: %s map %d box %d (lid script %d)\n", cr::kAreaEventFile[area0], map0, n.id, n.id + 1);
                (void)warped;
            }
        }
    std::printf("examine: %d examined, %d gave items (%d different, item 0 x%d), %d boxes lost their lid, %d warped; "
                "by model:", examined, gotItems, int(app.game.items.size()),
                app.game.items.count(0) ? app.game.items[0] : 0, lidsHidden, examineWarps);
    for (const auto& [m, c] : examinedByModel) std::printf(" %s x%d", m.c_str(), c);
    std::printf("\n");
    (void)examineErrors;
    mapErrors += entered;
    std::printf("npcs: %d drawn on %d maps, %d stand on a floor; off the floor by model:", npcsDrawn, npcMaps, npcsOnFloor);
    for (const auto& [m, c] : offFloor) std::printf(" %s x%d", m.c_str(), c);
    std::printf("\n");
    mapErrors += exitFailures;
    { // a gold box (j001_d01's boxes 60..98, lid scripts 61..99): at band 2 on the draw puts pool
      // slots 9..14's items into them (randomSelect's gold step); Uty_ChkItem counts key 2 into var
      // 0 - none, a message; with one, a Yes opens it and DelItem takes the key
        int gotWithout = -1, gotWith = -1, keysLeft = -1, boxId = -1, boxMap = -1, item = -1, draws = 0;
        for (int attempt = 0; attempt < 40 && boxId < 0; ++attempt)
        for (int m = 0, first = 1; m < cr::kNumMaps && boxId < 0; ++m) {
            if (cr::areaForMap(m, 2) != 2) continue;
            const auto at = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[m]));
            if (at == app.maps.end()) continue;
            if (first) { // a new draw at band 4 for the first pair, as the World Select would make
                app.game = cr::GameState();
                app.game.bands[0] = 4;
                app.playerChar = 0;
                app.treasureReroll = true;
                first = 0, ++draws;
            }
            showMap(app, int(at - app.maps.begin()), m, 2);
            app.pendingEntry = App::Entry::None;
            app.runner.stop();
            for (const cr::FieldNpc& n : app.npcs)
                if ((n.treasure & 3) && n.id >= 60 && n.id <= 98 && n.id % 2 == 0 && npcActive(app, n) && app.havePlayer) {
                    boxId = n.id, boxMap = m;
                    const cr::FieldNpc b = n;
                    for (int withKey = 0; withKey < 2; ++withKey) {
                        if (item > 0) app.game.items.erase(item);
                        if (withKey) app.game.items[2] = 1;
                        app.walker.pos = {b.pos[0] + b.box[0] / 2 + app.walker.radius, b.pos[1], b.pos[2]};
                        app.runner.stop();
                        interact(app);
                        item = app.game.vars[10];
                        for (int f = 0; f < 900 && (app.runner.running() || app.itemWindowOpen); ++f) {
                            stepEvents(app);
                            if (app.choiceFlag >= 0) answerChoice(app, true);
                            if (app.itemWindowOpen) app.tapPending = true;
                            if (!app.dialogue.empty()) app.dialogue.clear(), app.tapPending = true;
                        }
                        (withKey ? gotWith : gotWithout) = item > 0 && app.game.items.count(item) ? app.game.items[item] : 0;
                    }
                    keysLeft = app.game.items.count(2) ? app.game.items[2] : 0;
                    app.runner.stop();
                    app.dialogue.clear();
                    break;
                }
        }
        const bool ok = boxId >= 0 && gotWithout == 0 && gotWith >= 1 && keysLeft == 0;
        std::printf("gold box %d (map %d, draw %d at band 4): item %d - without a key %d, with one %d (keys left %d) %s\n", boxId,
                    boxMap, draws, item, gotWithout, gotWith, keysLeft, ok ? "ok" : "WRONG");
        mapErrors += !ok;
    }
    // Stepped scripts: open a chest frame by frame, then walk into an exit
    {
        app.game = cr::GameState(); // fresh: the examine pass above opened every box
        app.treasureBoxes.clear();
        const auto at = std::find(app.maps.begin(), app.maps.end(), std::string("d0101_10.cmp"));
        showMap(app, int(at - app.maps.begin()), 0, cr::areaForMap(0, 2));
        app.pendingEntry = App::Entry::None; // these tests play one script each
        app.dialogue.clear(); // (a page left by the tests before: it would hold the field)
        if (app.havePlayer) { // the touch pad steers as the keys do: pushed up, then right - running, a quarter turn apart
            const cr::FieldWalker start = app.walker;
            float heading[2] = {0, 0}, moved[2] = {0, 0};
            bool ran = true;
            for (int k = 0; k < 2; ++k) {
                app.walker = start;
                app.walkClock = 0;
                App::TouchPad& t = app.touchPad;
                t = App::TouchPad();
                t.down = true, t.dx = k ? 40.0f : 0.0f, t.dy = k ? 0.0f : -40.0f;
                updatePlayer(app, 1.0f / kFramesPerSecond, 0.0f);
                heading[k] = app.walker.heading;
                const cr::Vec3 d = app.walker.pos - start.pos;
                moved[k] = std::sqrt(d.x * d.x + d.z * d.z);
                ran = ran && app.walker.gait == cr::Gait::Run;
            }
            touchPadUp(app);
            app.walker = start;
            const float turn = std::remainder(heading[1] - heading[0], 2 * SDL_PI_F);
            const bool ok = ran && std::fabs(turn + SDL_PI_F / 2) < 1e-3f && moved[0] > 1.0f && moved[1] > 1.0f;
            std::printf("touch pad: up and right %s, headings %.3f / %.3f (turn %.3f), moved %.2f / %.2f\n",
                        ran ? "run" : "DON'T RUN", heading[0], heading[1], turn, moved[0], moved[1]);
            if (!ok) ++mapErrors;
        }
        int lidAnimFrame = -1, windowFrame = -1, fadedFrame = -1, endFrame = -1, frames = 0;
        bool staleItemFlagCleared = false, itemWindowClosedEarly = false;
        bool itemWindowHeldForRead = false, playerStoppedAtChest = false;
        int bandDown = -1, flagFrame = -1, closeFrame = -1; // (_Uty_GetItem_Proc: down in 2 frames, out 6 after flag 0)
        const cr::FieldNpc* box = nullptr;
        for (const cr::FieldNpc& n : app.npcs)
            if ((n.treasure & 3) && npcActive(app, n)) { box = &n; break; }
        if (box && app.havePlayer) {
            const int boxId = box->id;
            app.walker.pos = {box->pos[0] + box->box[0] / 2 + app.walker.radius, box->pos[1], box->pos[2]};
            app.tapPending = false;
            app.game.setFlag(0, true); // simulate a stale confirmation left by the previous interaction
            { // opened by touch, as on the iPad: the "!" button in the corner (FollowWindow::checkTouchEasyButton)
                float l0x, l0y, l1x, l1y, ex, ey;
                app.renderer.toLogical(0, 0, l0x, l0y);
                app.renderer.toLogical(1000, 1000, l1x, l1y);
                easyButtonCentre(app, ex, ey);
                const float lx = wideRight(app) + 2 * (ex + 16), ly = 2 * (ey + 32);
                app.fieldMenu.fade = 320;
                const bool shown = easyButtonShown(app);
                touchPadDown(app, (lx - l0x) * 1000 / (l1x - l0x), (ly - l0y) * 1000 / (l1y - l0y));
                touchPadUp(app);
                std::printf("touch: the \"!\" button %s, the chest %s\n", shown ? "shown" : "NOT SHOWN",
                            app.runner.running() ? "opening" : "NOT OPENED");
                if (!shown || !app.runner.running()) ++mapErrors;
            }
            app.soundLog.clear();
            const size_t itemsBefore = app.game.items.size();
            for (frames = 0; frames < 900 && (app.runner.running() || app.itemWindowOpen); ++frames) {
                stepEvents(app);
                for (const auto& d : app.npcDraws)
                    if (d.id == boxId + 1) {
                        if (lidAnimFrame < 0 && d.action == 1) lidAnimFrame = frames;
                        if (fadedFrame < 0 && d.alpha < 0.1f) fadedFrame = frames;
                    }
                if (windowFrame < 0 && app.itemWindowOpen) {
                    windowFrame = frames;
                    staleItemFlagCleared = !app.game.flag(0);
                }
                if (windowFrame >= 0 && frames - windowFrame < 30 && !app.itemWindowOpen) itemWindowClosedEarly = true;
                if (windowFrame >= 0 && frames - windowFrame == 5) bandDown = app.itemWin.slide == 2 && app.itemWin.fade == 0;
                if (flagFrame < 0 && app.itemWindowOpen && app.game.flag(0)) flagFrame = frames;
                if (flagFrame >= 0 && closeFrame < 0 && !app.itemWindowOpen) closeFrame = frames;
                if (app.itemWindowOpen && frames - windowFrame == 10 && app.havePlayer && app.player.actions.size() > 2) {
                    app.player.action = 2; // simulate reaching the chest while the run action is active
                    app.player.time = 1;
                    app.walker.gait = cr::Gait::Run;
                    updatePlayer(app, 0, 0);
                    playerStoppedAtChest = app.walker.gait == cr::Gait::Stand && app.player.action == 0;
                }
                if (app.itemWindowOpen && frames - windowFrame == 30) {
                    itemWindowHeldForRead = true;
                    app.tapPending = true; // the player confirms after reading the item
                }
                if (!app.dialogue.empty()) app.dialogue.clear();
                if (!app.runner.running() && endFrame < 0) endFrame = frames;
            }
            const bool lidGone = std::none_of(app.npcDraws.begin(), app.npcDraws.end(),
                                              [&](const App::NpcDraw& d) { return d.id == boxId + 1; });
            std::printf("stepped chest sounds:");
            for (uint32_t id : app.soundLog) std::printf(" %u", id);
            std::printf("\n");
            std::printf("stepped chest %d: lid anim %d, item window %d (flag reset %s, held for read %s, closed early %s), "
                        "player stopped %s, lid faded %d, done %d, item %s, lid %s\n",
                        boxId, lidAnimFrame, windowFrame, staleItemFlagCleared ? "yes" : "NO",
                        itemWindowHeldForRead ? "yes" : "NO", itemWindowClosedEarly ? "YES" : "no",
                        playerStoppedAtChest ? "yes" : "NO", fadedFrame, endFrame,
                        app.game.items.size() > itemsBefore || !app.game.items.empty() ? "gained" : "MISSING",
                        lidGone ? "removed" : "STILL THERE");
            const bool getSound = std::any_of(app.soundLog.begin(), app.soundLog.end(), [](uint32_t id) { return id == 0xf || id == 0x10; });
            std::printf("stepped chest band: down %s, out %d frames after flag 0, SE 0xf / 0x10 %s\n", bandDown == 1 ? "yes" : "NO",
                        closeFrame - flagFrame, getSound ? "yes" : "NO");
            if (lidAnimFrame < 0 || windowFrame < 0 || endFrame < 0 || !lidGone ||
                !staleItemFlagCleared || itemWindowClosedEarly || !itemWindowHeldForRead || !playerStoppedAtChest ||
                bandDown != 1 || closeFrame - flagFrame < 5 || closeFrame - flagFrame > 7 || !getSound)
                ++mapErrors;
        }
        // exit: walk into trigger 2's box and let the script play
        for (const cr::FieldNpc& n : app.npcs)
            if (n.id == 2 && n.touchTrigger()) {
                app.walker.pos = {n.boxCenterX(), n.pos[1], n.boxCenterZ()};
                app.lastKick = 0;
                app.soundLog.clear();
                checkTriggers(app);
                int warpFrame = -1, maxFade = 0;
                for (frames = 0; frames < 600 && app.runner.running(); ++frames) {
                    stepEvents(app);
                    if (warpFrame < 0 && app.mapNumber != 0) warpFrame = frames;
                    maxFade = std::max(maxFade, int(screenFadeAlpha(app) * 100));
                }
                std::printf("stepped exit sounds:");
                for (uint32_t id : app.soundLog) std::printf(" %#x", id);
                std::printf("\n");
                std::printf("stepped exit: warped to map %d at frame %d, script done at frame %d, peak fade %d%%, final fade %d%%\n",
                            app.mapNumber, warpFrame, frames, maxFade, int(screenFadeAlpha(app) * 100));
                if (warpFrame < 0 || screenFadeAlpha(app) > 0.01f) ++mapErrors;
                break;
            }
    }
    // Entry scripts on a new game: each area's script 0 on its first room, then every room's
    // EvrStart 5, played frame by frame to the end (dialogue paged, taps given)
    {
        int areasRun = 0, roomsRun = 0, stuck = 0, errors = 0, pages = 0, warps = 0, longest = 0, choices = 0, battles = 0;
        auto play = [&](const char* what, int area, int map) {
            const int startMap = app.mapNumber;
            int f = 0;
            app.tapPending = false;
            for (; f < 20000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
                stepEvents(app);
                if (!app.dialogue.empty()) { pages += int(app.dialogue.size()); app.dialogue.clear(); }
                if (app.choiceFlag >= 0) { ++choices; answerChoice(app, true); }
                if (app.worldChoiceOpen) { ++choices; answerWorldChoice(app, 0); }
                app.tapPending = true;
                if (app.itemWindowOpen) app.game.setFlag(0, true);
                if (app.battle.active) { ++battles; endBattle(app, true); } // a script's battle: won (else every room after it runs gated)
            }
            longest = std::max(longest, f);
            if (app.mapNumber != startMap) ++warps;
            if (!app.runner.result().error.empty()) {
                ++errors;
                std::printf("  %s area %d map %d: %s\n", what, area, map, app.runner.result().error.c_str());
            }
            if (app.runner.running()) {
                ++stuck;
                std::printf("  %s area %d map %d: still running after %d frames (%s, event %d)\n", what, area, map, f,
                            app.runner.where().c_str(), app.runner.eventNo());
                if (SDL_getenv("CR_STUCK_TRACE")) { // (debugging) the loop it's in
                    std::vector<std::pair<size_t, int>> ring;
                    cr::setEventTrace([&](size_t pc, int op) { if (ring.size() < 64) ring.push_back({pc, op}); });
                    for (int k = 0; k < 3; ++k) stepEvents(app);
                    cr::setEventTrace(nullptr);
                    for (const auto& [pc, op] : ring) std::printf("    %#zx op %d\n", pc, op);
                }
                app.runner.stop();
            }
            return f;
        };
        for (int area = 0; area < cr::kNumAreas; ++area) {
            const int map = cr::kAreaFirstMap[area];
            const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map]));
            if (it == app.maps.end()) continue;
            app.game = cr::GameState();
            app.entryArea = -1;
            showMap(app, int(it - app.maps.begin()), map, area);
            const int f = play("area script", area, map);
            ++areasRun;
            std::printf("  area %2d %s: script 0 on map %d took %d frames\n", area, cr::kAreaEventFile[area], map, f);
        }
        for (int area = 0; area < cr::kNumAreas; ++area)
            for (int map = 0; map < cr::kNumMaps; ++map) {
                if (cr::areaForMap(map, area) != area) continue;
                const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map]));
                if (it == app.maps.end()) continue;
                app.game = cr::GameState();
                app.entryArea = area; // arriving from another room of the area
                showMap(app, int(it - app.maps.begin()), map, area);
                play("room script", area, map);
                ++roomsRun;
            }
        std::printf("entry scripts: %d areas, %d rooms; %d dialogue pages, %d choices, %d battles, %d warped, %d errors, %d stuck, "
                    "longest %d frames\n", areasRun, roomsRun, pages, choices, battles, warps, errors, stuck, longest);
        mapErrors += errors + stuck;
    }
    // New game: talk to everyone in the Ark's hall, frame by frame (pages turned, choices answered yes)
    {
        startNewGame(app);
        int openingFrames = 0, openingPages = 0;
        for (; openingFrames < 20000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++openingFrames) {
            stepEvents(app);
            if (!app.dialogue.empty()) {
                openingPages += int(app.dialogue.size());
                for (const auto& v : app.dialogueVoices) app.voicesPlayed += int(v.size());
                app.dialogue.clear();
            }
            if (app.choiceFlag >= 0) answerChoice(app, true);
            if (app.worldChoiceOpen) answerWorldChoice(app, 0);
            app.tapPending = true;
        }
        std::printf("new game: the opening took %d frames, %d pages, %d voice lines, ends on map %d%s\n", openingFrames,
                    openingPages, app.voicesPlayed, app.mapNumber, app.runner.running() ? " (STILL RUNNING)" : "");
        const int voicesBefore = app.voicesPlayed;
        if (app.runner.running() || app.mapNumber != 97) ++mapErrors;
        int talked = 0, pages = 0, choices = 0, maxPortraits = 0, plates = 0, unfinished = 0;
        std::vector<int> ids;
        for (const cr::FieldNpc& n : app.npcs)
            if ((n.kind & 8) && npcActive(app, n) && n.pos[1] < 900.0f) ids.push_back(n.id);
        for (int id : ids) {
            const cr::FieldNpc* n = findNpc(app, id);
            if (!n || !app.havePlayer) continue;
            app.walker.pos = {n->pos[0] + n->box[0] / 2 + app.walker.radius, n->pos[1], n->pos[2]};
            if (!interact(app)) continue;
            ++talked;
            int f = 0;
            for (; f < 6000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
                stepEvents(app);
                int shown = 0;
                for (const App::Portrait& p : app.portraits) shown += p.tex.id && p.alpha > 0.5f;
                maxPortraits = std::max(maxPortraits, shown);
                if (!app.dialogue.empty()) {
                    for (const auto& mi : app.dialogueInfo) plates += mi.nameId >= 0;
                    pages += int(app.dialogue.size());
                    for (const auto& v : app.dialogueVoices) app.voicesPlayed += int(v.size());
                    app.dialogue.clear();
                }
                if (app.choiceFlag >= 0) { ++choices; answerChoice(app, true); }
                if (app.worldChoiceOpen) { ++choices; answerWorldChoice(app, 0); }
                app.tapPending = true;
            }
            if (app.runner.running()) {
                ++unfinished;
                std::printf("  talk to %d: still running (%s)\n", id, app.runner.where().c_str());
                app.runner.stop();
            }
        }
        std::printf("new game: talked to %d of %zu in the hall; %d pages (%d with a name plate), %d voice lines, %d choices, "
                    "up to %d portraits at once, %d unfinished\n", talked, ids.size(), pages, plates,
                    app.voicesPlayed - voicesBefore, choices, maxPortraits, unfinished);
        mapErrors += unfinished;
    }
    mapErrors += battleSelfTest(app, frames, glErrors);
    // The menus' sounds since the last call against the original's __effgo ids (2 choose, 3 back, 4 buzzer /
    // keyboard cursor, 5 page, 11 saved, 12 loaded, 22 paid, 40 SE gauge): 1 if they differ
    auto expectSounds = [&app](const char* what, std::initializer_list<uint32_t> want) {
        const bool same = std::equal(app.menuSoundLog.begin(), app.menuSoundLog.end(), want.begin(), want.end());
        if (!same) {
            std::printf("menu sounds, %s:", what);
            for (uint32_t id : app.menuSoundLog) std::printf(" %u", id);
            std::printf(" (want");
            for (uint32_t id : want) std::printf(" %u", id);
            std::printf(")\n");
        }
        app.menuSoundLog.clear();
        return same ? 0 : 1;
    };
    { // The Options screen: every page builds and draws; keys change what they should
        app.game.setFlag(0x43, true); // ("Option", the first time's page, is the manual block's)
        const App::Mix mix = app.mix;
        const App::Video video = app.video;
        const int language = app.language;
        int rows = 0, problems = 0;
        for (int page = 0; page < kOptionPages; ++page) {
            openOptions(app, page);
            rows += int(optionRows(app, page).size());
            app.lastTicks = 0;
            drawFrame(app, false);
            ++frames;
        }
        app.menuSoundLog.clear();
        openOptions(app, 1);         // Sound
        optionsKey(app, SDLK_DOWN);  // BGM
        const int music = app.mix.music;
        optionsKey(app, SDLK_LEFT);
        problems += app.mix.music != std::max(0, (music + 5) / 10 * 10 - 10); // (steps of 10)
        optionsKey(app, SDLK_DOWN);  // Sound Effects
        const int effects = app.mix.effects;
        optionsKey(app, SDLK_LEFT);
        problems += app.mix.effects != std::max(0, (effects + 5) / 10 * 10 - 10);
        problems += expectSounds("options gauges", {4, 4, 40}); // the BGM's is silent
        { // the < Back > bar's parts: the arrows turn the page, Back alone closes (ComUty_CmdBack 2)
            const bool parts = optionsBackPart(200, kOptBackY + 12) == -1 && optionsBackPart(300, kOptBackY + 12) == 0 &&
                               optionsBackPart(400, kOptBackY + 12) == 1 && optionsBackPart(300, kOptBackY - 40) == 2;
            if (!parts) std::printf("  options: the Back bar's arrows don't turn the page\n");
            problems += !parts;
        }
        openOptions(app, 0);         // Game: Language
        optionsKey(app, SDLK_LEFT);  // Japanese
        problems += app.language != 0;
        optionsKey(app, SDLK_RIGHT); // English
        problems += app.language != 1;
        optionsKey(app, SDLK_Q);     // wraps to the last page (Graphics)
        problems += app.optionsPage != kOptionPages - 1;
        optionsKey(app, SDLK_UP);    // from row 0 to Back
        problems += app.optionsRow != kOptionsBackRow;
        problems += expectSounds("options buttons, page", {2, 2, 3, 4});
        // Graphics: Anti-aliasing steps through MSAA to FSR Native AA and DLAA (the upscaler at
        // native size), the Upscaler row on to its other modes
        if (app.renderer.scalable()) { // (OpenGL: the row is off)
            app.optionsRow = 1;
            int aaSteps = 0;
            while (app.video.upscaler != 1 && aaSteps++ < 6) optionsKey(app, SDLK_RIGHT);
            problems += app.video.upscaler != 1 || app.video.upscaleMode != 0;
            optionsKey(app, SDLK_RIGHT);
            problems += app.video.upscaler != 2 || app.video.upscaleMode != 0;
            optionsKey(app, SDLK_RIGHT); // wraps to Off
            problems += app.video.upscaler != 0 || app.video.msaa != 1;
        }
        // FSR 3.1 where it runs (Vulkan): frames come out the frame's size, not black, error free
        int upscaled = 0;
        if (app.renderer.upscalerProblem(cr::Upscaler::Fsr3).empty()) {
            for (int mode : {0, 1, 3}) {
                app.video.upscaler = 1;
                app.video.upscaleMode = mode;
                applyVideo(app);
                app.optionsOpen = false;
                for (int f = 0; f < 8; ++f) {
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    ++frames;
                }
                std::vector<uint8_t> px;
                int w = 0, h = 0;
                uint64_t sum = 0;
                if (app.renderer.readPixels(px, w, h))
                    for (size_t i = 0; i < px.size(); i += 4) sum += px[i] + px[i + 1] + px[i + 2];
                const int errs = app.renderer.takeErrors();
                if (!sum || errs || w != app.renderer.frameWidth()) {
                    std::printf("FSR 3.1 %s: %d errors, %dx%d, %s\n", kUpscaleModes[mode].name, errs, w, h, sum ? "drawn" : "black");
                    ++problems;
                }
                ++upscaled;
            }
        }
        { // Smooth Models + HD textures: battles load and draw with them, HD textures keep the
          // original's layout size
            app.video.smoothModels = 3;
            app.video.hdTextures = true;
            const int hdBefore = app.hdLoaded;
            int drawn = 0;
            for (int g : {1, 5, 9}) {
                startBattle(app, {g, g % 9, -1, 0}, 0);
                for (int f = 0; f < 4; ++f) {
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    ++frames;
                }
                drawn += app.battle.active;
                const int errs = app.renderer.takeErrors();
                if (errs) std::printf("smooth/hd battle %d: %d render API errors\n", g, errs);
                problems += errs;
                endBattle(app, true);
            }
            cr::Image wi;
            if (loadImage(app, "window_hd.mct", wi) && wi.logicalW) {
                const cr::Texture t = app.renderer.upload(wi);
                problems += t.width != wi.logicalW || t.height != wi.logicalH;
                cr::Texture tt = t;
                app.renderer.destroy(tt);
            }
            std::printf("smooth models + hd textures: %d battles drawn, %d HD textures used (%s)\n", drawn,
                        app.hdLoaded - hdBefore, app.hdDir.c_str());
            problems += drawn != 3;
            app.video.smoothModels = 0;
            app.video.hdTextures = false;
        }
        openOptions(app, 2);         // Display
        app.video.widescreen = true; // "Original Presentation" restores the defaults (upscaler off)
        app.video.upscaler = 1;
        app.optionsRow = int(optionRows(app, 2).size()) - 1;
        app.menuSoundLog.clear();
        optionsKey(app, SDLK_RETURN);
        problems += app.video.widescreen || app.video.width != 1024 || app.video.upscaler != 0;
        optionsKey(app, SDLK_ESCAPE);
        problems += app.optionsOpen;
        problems += expectSounds("options Enter, Back", {2, 3});
        // The title: the boot script reaches the title menu; its Options are title mode (Reset
        // Data); Start begins a new game; in a game, Return to Title boots again
        bootTitle(app);
        int bootFrames = 0;
        for (; bootFrames < 1000 && !app.titleOpen; ++bootFrames) stepEvents(app);
        problems += !app.titleOpen;
        openOptions(app, 0);
        const std::vector<OptRow> titlePage = optionRows(app, 0);
        problems += titlePage.size() != size_t(13 - languageSheet(app)) || // (2.2.0: one Language row)
                    titlePage.back().label != sysLabel(app, 58122, "Reset Data");
        { // Reset Data: asked twice; No keeps the saves, Yes to both removes them (in a test folder)
            const std::string keepDir = app.saveDirOverride;
            app.saveDirOverride = userDir() + "selftest-reset/";
            SDL_CreateDirectory(app.saveDirOverride.c_str());
            const cr::SaveData sd = captureSave(app);
            cr::writeSave(dataPath(app), sd);
            cr::writeSave(slotPath(app, 1), sd); // (an earlier release's)
            cr::writeSave(savePath(app, "resume.dat"), sd);
            auto saves = [&] {
                int n = int(SDL_GetPathInfo(dataPath(app).c_str(), nullptr));
                for (int pair = 1; pair <= 4; ++pair) n += SDL_GetPathInfo(slotPath(app, pair).c_str(), nullptr);
                return n + int(SDL_GetPathInfo(savePath(app, "resume.dat").c_str(), nullptr));
            };
            app.optionsRow = int(titlePage.size()) - 1;
            optionsKey(app, SDLK_RETURN);                 // "Erase all the data from your game?"
            int bad = app.resetAsk != 1 || app.resetConfirm != 1;
            optionsKey(app, SDLK_RETURN);                 // No (under the cursor)
            bad += app.resetAsk != 0 || saves() != 3;
            optionsKey(app, SDLK_RETURN);
            optionsKey(app, SDLK_LEFT), optionsKey(app, SDLK_RETURN); // Yes: "Are you sure ...?"
            bad += app.resetAsk != 2 || saves() != 3;
            optionsKey(app, SDLK_LEFT), optionsKey(app, SDLK_RETURN); // Yes
            bad += app.resetAsk != 0 || saves() != 0;
            std::printf("reset data: asked twice, No kept 3 saves, Yes removed them: %s\n", bad ? "WRONG" : "ok");
            problems += bad;
            app.saveDirOverride = keepDir;
        }
        optionsKey(app, SDLK_ESCAPE);
        problems += !app.titleOpen; // back on the title menu
        app.menuSoundLog.clear();
        int openingMap = -1;
        {
            const std::string keepDir = app.saveDirOverride; // (no save there: New alone)
            app.saveDirOverride = userDir() + "selftest-empty/";
            titleChoose(app, 0);    // Start: the boot script goes on to Character Select (Util 4)
            for (int f = 0; f < 300 && !app.charSelectOpen; ++f) stepEvents(app);
            problems += !app.charSelectOpen;
            app.charSelectCursor = 0, app.charSelectPage = 0; // Escher & Musiea
            charSelectKey(app, SDLK_RETURN); // Escher & Musiea -> the pair's save / load screen (Util 5)
            for (int f = 0; f < 300 && !app.saveScreen.open; ++f) stepEvents(app);
            problems += !app.saveScreen.open || app.saveScreen.haveSave || !app.saveScreen.load;
            saveScreenKey(app, SDLK_RETURN); // New -> "Begin new game?"
            problems += app.saveScreen.message != 57057;
            saveScreenKey(app, SDLK_RETURN); // Yes: the script plays the pair's opening
            problems += expectSounds("title Start, pair, New, Yes", {2, 2, 2, 12});
            for (int f = 0; f < 600 && app.booting; ++f) stepEvents(app);
            openingMap = app.mapNumber;
            app.saveDirOverride = keepDir;
        }
        problems += app.booting || openingMap != 88 || app.playerChar != 0; // the opening (d0550); 82 would be the last battle
        if (problems) std::printf("title flow: map %d, leader %d, booting %d\n", openingMap, app.playerChar, int(app.booting));
        app.runner.stop();
        startNewGame(app); // (the hall, for the in-game Options checks below)
        openOptions(app, 0);
        std::vector<OptRow> gamePage = optionRows(app, 0);
        problems += gamePage.size() != size_t(13 - languageSheet(app)) || gamePage.back().label != sysLabel(app, 58123, "Return to Title") || !gamePage.back().enabled;
        if (gamePage.size() == size_t(13 - languageSheet(app)) && gamePage.back().act) gamePage.back().act(0);
        problems += app.resetAsk != 3; // "return to the title?" (57060)
        resetAnswer(app, true);
        for (int f = 0; f < 16 && app.titleReturnIn > 0; ++f) stepEvents(app); // (the black's frames)
        problems += !app.booting;
        app.runner.stop();
        { // touch play's look: no battle plate lit while a page is in after a click or touch, the one
          // chosen as it goes; the cursor's for keys and pads
            const bool keep = app.pointerLast;
            const int keepState = app.battle.viewState;
            app.battle.viewState = 2;
            app.pointerLast = false;
            const bool keyLit = battleCursorLit(app);
            app.pointerLast = true;
            const bool touchDark = !battleCursorLit(app);
            app.battle.viewState = 3;
            const bool goingLit = battleCursorLit(app);
            app.battle.viewState = keepState;
            app.pointerLast = keep;
            if (!keyLit || !touchDark || !goingLit)
                std::printf("  touch play: the battle's cursor plate lit %s after keys, %s after a tap, %s as the page goes\n",
                            keyLit ? "yes" : "NO", touchDark ? "not" : "STILL", goingLit ? "lit" : "NOT");
            problems += !keyLit + !touchDark + !goingLit;
        }
        std::printf("options: %d pages, %d rows; FSR 3.1 modes drawn: %d; title menu after %d frames; %d problems\n",
                    kOptionPages, rows, upscaled, bootFrames, problems);
        mapErrors += problems;
        app.mix = mix; // put the settings back as they were
        app.video = video;
        app.language = language;
        applyLanguage(app);
        applyMix(app);
        applyVideo(app);
    }
    { // Saves: a field's state written, changed, read back and restored; the title's Start shows it
        int problems = 0;
        const std::string keepDir = app.saveDirOverride;
        app.saveDirOverride = userDir() + "selftest-saves/";
        SDL_CreateDirectory(app.saveDirOverride.c_str());
        startNewGame(app);
        for (int f = 0; f < 3000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        app.game.setFlag(3333, true);
        app.game.items[7] = 3;
        app.game.vars[100] = -42;
        app.game.oz = 777;
        app.game.party[1].level = 5;
        app.game.party[1].exp = 600;
        app.game.party[0].equip[cr::kJewel] = 426;
        app.game.party[0].seeds[2] = 7;
        app.game.party[0].genes[2] = 485;
        app.playSeconds = 1234.5;
        app.walker.pos.x += 3;
        const cr::SaveData before = captureSave(app);
        app.menuSoundLog.clear();
        openSaveScreen(app, false);
        saveScreenKey(app, SDLK_RETURN); // Yes
        problems += app.saveScreen.message != 57082;
        problems += expectSounds("save", {11});
        app.saveScreen.open = false;
        app.game = cr::GameState();
        app.playSeconds = 0;
        bootTitle(app);
        for (int f = 0; f < 1000 && !app.titleOpen; ++f) stepEvents(app);
        titleChoose(app, 0); // Start -> Character Select -> the pair's Load / New
        for (int f = 0; f < 300 && !app.charSelectOpen; ++f) stepEvents(app);
        app.charSelectCursor = 0, app.charSelectPage = 0; // Escher & Musiea
        charSelectKey(app, SDLK_RETURN);
        for (int f = 0; f < 300 && !app.saveScreen.open; ++f) stepEvents(app);
        problems += !app.saveScreen.open || !app.saveScreen.haveSave;
        saveScreenKey(app, SDLK_RETURN); // Load
        problems += app.saveScreen.message != 57013;
        saveScreenKey(app, SDLK_RETURN); // Yes
        problems += expectSounds("title Start, pair, Load, Yes", {2, 2, 2, 12});
        const bool loaded = app.saveScreen.message == 57083;
        problems += !loaded;
        const cr::SaveData after = captureSave(app);
        problems += after.map != before.map || after.area != before.area || !app.game.flag(3333) ||
                    app.game.items[7] != 3 || app.game.vars[100] != -42 || app.game.oz != 777 ||
                    app.game.party[1].level != 5 || app.game.party[1].exp != 600 || app.game.party[0].equip[cr::kJewel] != 426 || app.game.party[0].seeds[2] != 7 || app.game.party[0].genes[2] != 485 ||
                    app.game.party[2].equip[cr::kWeapon] != 257 ||
                    std::fabs(app.playSeconds - 1234.5) > 1e-6 ||
                    std::fabs(after.pos[0] - before.pos[0]) > 1e-3f || app.titleOpen || app.booting;
        for (int f = 0; f < 60; ++f) stepSaveScreen(app);
        problems += app.saveScreen.open;
        for (int f = 0; f < 3; ++f) {
            app.lastTicks = 0;
            drawFrame(app, false);
            ++frames;
        }
        // The Progress Tracker
        app.persist = true;
        writeResume(app);
        app.persist = false;
        cr::SaveData resume;
        problems += !cr::readSave(savePath(app, "resume.dat"), resume) || resume.map != before.map;
        // ... offered after the title's Start (TitlePrc 4 -> 0xf), and used up by the answer
        app.persist = true;
        bootTitle(app);
        for (int f = 0; f < 1000 && !app.titleOpen; ++f) stepEvents(app);
        problems += app.resumePrompt != 0; // (not before the tap and Start)
        app.menuSoundLog.clear();
        titleKey(app, SDLK_RETURN); // the tap: SE 0, the plates
        for (int f = 0; f < 8; ++f) stepEvents(app);
        app.interruptShown = false; // (a new launch)
        titleKey(app, SDLK_RETURN); // Start
        // S3 (__menuu_TitlePrc 0xf..0x18): the first offer a launch with flag 0x44 clear reads "About
        // Progress Tracker" (56036) through first - flags 0x44 and 3 set while it is up -, then
        // windows 4..7 come in with the question; the answer sends them out before the load
        int rbad = 0;
        const size_t noticePages = app.dialogue.size();
        rbad += app.resumeNotice != 1 || app.resumePrompt || !app.game.flag(0x44) || !app.game.flag(3) || noticePages < 1 ||
                !app.interruptShown || !app.titleOpen;
        problems += expectSounds("title tap, Start", {0, 2});
        for (int f = 0; f < 3; ++f) { app.lastTicks = 0; drawFrame(app, false); ++frames; }
        for (int f = 0; f < 800 && app.resumeNotice == 1; ++f) {
            stepEvents(app);
            if (f % 8 == 7) titleKey(app, SDLK_RETURN); // (a page out, then turned)
        }
        for (int f = 0; f < 6; ++f) stepEvents(app);
        rbad += app.game.flag(3) || app.resumeNotice || app.resumePrompt != 1 || app.resumeWin < 1 || app.resumeCursor != 0;
        for (int f = 0; f < 3; ++f) { app.lastTicks = 0; drawFrame(app, false); ++frames; }
        const bool offered = app.resumePrompt == 1;
        problems += !offered;
        app.menuSoundLog.clear();
        answerResume(app, true);
        rbad += app.resumePrompt != 2 || !app.resumeYes;
        for (int f = 0; f < 4; ++f) stepEvents(app);
        rbad += app.resumeWin > 0 || !app.titleOpen; // (0x15: the windows out, the black still coming)
        for (int f = 0; f < 20 && app.resumeIn > 0; ++f) stepEvents(app); // (the black, then the load)
        rbad += !app.game.flag(0x44) || app.resumePrompt; // (ContData_memcpy: the message seen this launch)
        problems += expectSounds("resume", {2});
        problems += app.titleOpen || app.mapNumber != before.map || SDL_GetPathInfo(savePath(app, "resume.dat").c_str(), nullptr);
        // ... and saved as you go (the Vita's): a scene's end after the load arms it, the next writes it
        app.trackerArmed = false; // (a script that ended during the load's frames above armed it already)
        trackerSceneEnd(app);
        const bool armedOnly = !SDL_GetPathInfo(savePath(app, "resume.dat").c_str(), nullptr);
        trackerSceneEnd(app);
        const bool asYouGo = armedOnly && cr::readSave(savePath(app, "resume.dat"), resume) && resume.map == before.map;
        problems += !asYouGo;
        { // ... a second offer this launch: no message, the question at once; No sends the windows out and
          // Start goes on (the boot script on to Character Select)
            bootTitle(app);
            for (int f = 0; f < 1000 && !app.titleOpen; ++f) stepEvents(app);
            titleKey(app, SDLK_RETURN);
            for (int f = 0; f < 8; ++f) stepEvents(app);
            titleKey(app, SDLK_RETURN); // Start
            rbad += app.resumeNotice || app.resumePrompt != 1 || !app.dialogue.empty();
            for (int f = 0; f < 4; ++f) stepEvents(app);
            answerResume(app, false);
            rbad += app.resumePrompt != 2 || app.resumeYes;
            for (int f = 0; f < 20 && app.resumeIn > 0; ++f) stepEvents(app);
            rbad += app.titleOpen || app.resumePrompt || app.game.flag(0) || SDL_GetPathInfo(savePath(app, "resume.dat").c_str(), nullptr);
            app.runner.stop();
            // ... and none while the save holds flag 0x44, a new launch or not
            cr::SaveData d;
            if (readData(app, d)) d.game.setFlag(0x44, true), cr::writeSave(dataPath(app), d);
            else rbad += 1;
            cr::writeSave(savePath(app, "resume.dat"), resume);
            app.interruptShown = false;
            bootTitle(app);
            for (int f = 0; f < 1000 && !app.titleOpen; ++f) stepEvents(app);
            titleKey(app, SDLK_RETURN);
            for (int f = 0; f < 8; ++f) stepEvents(app);
            titleKey(app, SDLK_RETURN); // Start
            rbad += app.resumeNotice || app.resumePrompt != 1 || app.interruptShown;
            bootTitle(app);
            rbad += app.resumePrompt || app.resumeNotice || app.resumeIn;
            app.runner.stop();
            SDL_RemovePath(savePath(app, "resume.dat").c_str());
            std::printf("title resume: the message %zu page(s) once a launch (flag 0x44), the windows in and out, Yes loads, No starts: %d problems\n",
                        noticePages, rbad);
            problems += rbad;
        }
        SDL_RemovePath(savePath(app, "resume.dat").c_str());
        app.persist = false;
        // The title's New (pair 1, no save): New alone, at 232; Yes hands var 1 = 2 to the boot
        // script, which goes to the opening (map 88) - var 1 = 1 would be the last battle (map 82)
        for (const char* f : {"save1.dat", "data.dat", "resume.dat"}) SDL_RemovePath(savePath(app, f).c_str());
        app.game = cr::GameState();
        bootTitle(app);
        for (int f = 0; f < 1000 && !app.titleOpen; ++f) stepEvents(app);
        titleChoose(app, 0);
        for (int f = 0; f < 300 && !app.charSelectOpen; ++f) stepEvents(app);
        app.charSelectCursor = 0;
        charSelectKey(app, SDLK_RETURN);
        for (int f = 0; f < 300 && !app.saveScreen.open; ++f) stepEvents(app);
        problems += !app.saveScreen.open || app.saveScreen.haveSave || savePlates(app) != std::vector<int>{2} ||
                    app.saveScreen.emblems != 0;
        saveScreenKey(app, SDLK_RETURN); // New
        problems += app.saveScreen.message != 57057 || app.saveScreen.kind != 2;
        saveScreenKey(app, SDLK_RETURN); // Yes
        const int var1 = app.game.vars[1];
        int openingMap = -1;
        for (int f = 0; f < 1500 && openingMap < 0; ++f) {
            stepEvents(app);
            if (!app.booting && app.mapNumber >= 0) openingMap = app.mapNumber;
        }
        problems += var1 != 2 || openingMap != 88;
        app.runner.stop();
        // Every story cleared: the emblems, the Final plate, New asking about the clear data first
        {
            cr::SaveData sd = before;
            for (int f : {31, 32, 34, 36, 38, 39, 40}) sd.game.setFlag(f, true);
            sd.leader = 0;
            cr::writeSave(dataPath(app), sd);
        }
        app.game = cr::GameState();
        app.chosenPair = 1;
        openSaveScreen(app, true);
        App::SaveScreen& ssc = app.saveScreen;
        problems += ssc.emblems != 7 || !ssc.allClear || !ssc.cleared || savePlates(app) != std::vector<int>{0, 1, 2};
        saveScreenChoose(app, 2); // New: with the clear data?
        problems += ssc.kind != 3 || ssc.message != 57086 || ssc.lines[0] != 57087 || ssc.lines[1] != 57088;
        saveScreenKey(app, SDLK_ESCAPE); // No: a plain new game, then?
        problems += ssc.phase != 1 || ssc.kind != 2 || ssc.message != 57089 || ssc.lines[1] != 57091;
        saveScreenKey(app, SDLK_ESCAPE); // No again: back to the plates
        problems += ssc.phase != 0;
        saveScreenChoose(app, 0); // the last battle
        problems += ssc.kind != 1 || ssc.message != 57056;
        // Uty_LoadPrty with the clear data: mode 3 keeps the bonuses, gear, plates and bag (less the
        // story's kind 3 / 9 items) and Oz; mode 2 keeps Oz only; either clears the pair's flags
        int storyItem = 0;
        for (int it = 1; it < 500 && !storyItem; ++it)
            if (const cr::ItemParam* ip = app.itemTable.item(it); ip && (ip->kind == 3 || ip->kind == 9)) storyItem = it;
        {
            cr::SaveData sd = before;
            for (int f : {31, 32, 34, 36, 38, 39, 40, 1100, 2000, 3000, 3020, 3004, 3310}) sd.game.setFlag(f, true);
            sd.game.items[storyItem] = 1;
            sd.game.items[95] = 1;                  // a gene (kind 4): the shared bag
            sd.game.routeBags[1] = {{7, 5}, {22, 2}}; // pair 2's own bag
            cr::writeSave(dataPath(app), sd);
        }
        ViewerHost host(app);
        app.game = cr::GameState();
        host.loadParty(1, true);
        const cr::PartyMember& e0 = app.game.party[0];
        int carry = (e0.level == 1) + (e0.seeds[2] == 7) + (e0.genes[2] == 485) + (e0.equip[cr::kJewel] == 426) +
                    (app.game.party[1].level == 1) + (app.game.items[7] == 3) + !app.game.items.count(storyItem) +
                    (app.game.oz == 777) + !app.game.flag(1100) + app.game.flag(2000) + (app.playerChar == 0);
        // ResetFieldFlag_NewGame: pair 1's blocks 0 and 5 (3000, 3020) and 3310 go, block 1 (pair 2's) stays
        problems += (carry != 11) + (storyItem == 0) + app.game.flag(3000) + app.game.flag(3020) + app.game.flag(3310) +
                    !app.game.flag(3004);
        app.game = cr::GameState();
        host.loadParty(1, false);
        // (F4, _prty_init_new 67059: the shared bag G+0x30640 - kinds 4 / 5 / 9 / 10 - is not the pair's;
        // the other pairs' bags stay as they are)
        const bool sharedKept = app.game.items[95] == 1 && app.game.routeBags[1][7] == 5 && app.game.routeBags[1][22] == 2;
        problems += app.game.party[0].seeds[2] != 0 || app.game.items.count(7) || app.game.items[1] != 3 ||
                    app.game.oz != 777 || app.game.flag(1100) || !app.game.flag(2000) || !sharedKept;
        // another pair's new game on the one save: its own bag and records start over, the Oz, the
        // shared bag and pair 1's records go on (pair 2: Eluca's leader 2)
        app.game = cr::GameState();
        host.loadParty(2, false);
        const bool pairFresh = app.game.items[21] == 2 && !app.game.items.count(7) && !app.game.items.count(22) &&
                               app.game.party[2].level == 1 && app.playerChar == 2;
        const bool restKept = app.game.oz == 777 && app.game.items[95] == 1 && app.game.party[0].seeds[2] == 7 &&
                              app.game.routeBags[0][7] == 3 && app.game.flag(1100);
        problems += !pairFresh + !restKept;
        std::printf("new game with clear data: mode 3 carried %d of 11, mode 2 keeps Oz and the shared bag (%s), "
                    "another pair's new game: its own fresh %s, the rest kept %s\n",
                    carry, sharedKept ? "yes" : "NO", pairFresh ? "yes" : "NO", restKept ? "yes" : "NO");
        problems += clearDataNewGameSelfTest(app, before);
        app.game = cr::GameState();
        app.playerChar = 0;
        for (int f = 0; f < 2; ++f) {
            app.lastTicks = 0;
            drawFrame(app, false);
            ++frames;
        }
        problems += app.renderer.takeErrors();
        { // S1: one data.dat (2.2.0 SaveData_Save 0x725d0). The earlier releases' save1 .. save4 fold into
          // it once and stay as they were; each pair loads its own place and bag; a save keeps the
          // other pairs' places; Oz and the shared bag are one
            int fbad = 0;
            for (const char* f : {"save1.dat", "save2.dat", "save3.dat", "save4.dat", "data.dat", "resume.dat"}) SDL_RemovePath(savePath(app, f).c_str());
            auto legacy = [&](int pair, int64_t when, int oz, int area, int map) { // an earlier release's file: no places, no other bags
                cr::SaveData s = before;
                s.wholeGame = false, s.places = {}, s.game.routeBags = {};
                s.leader = 2 * (pair - 1), s.game.bagRoute = pair - 1, s.savedAt = when, s.game.oz = oz, s.area = area, s.map = map;
                s.game.items.clear();
                s.game.items[95] = pair; // a gene (shared)
                s.game.items[21] = 10 + pair; // the pair's own Chocolates
                s.game.items[201 + 56 * (pair - 1)] = 1; // its first member's first weapon (its gear)
                s.game.party[size_t(2 * (pair - 1))].level = 10 + pair;
                s.game.setFlag(1100 + 400 * (pair - 1), true);
                return s;
            };
            const cr::SaveData s1 = legacy(1, 300, 111, 7, 97), s2 = legacy(2, 500, 222, 13, 97), s4 = legacy(4, 400, 444, 25, 105);
            cr::writeSave(slotPath(app, 1), s1), cr::writeSave(slotPath(app, 2), s2), cr::writeSave(slotPath(app, 4), s4);
            std::vector<uint8_t> was[3], now[3];
            for (int k = 0; k < 3; ++k) cr::readFile(slotPath(app, k == 2 ? 4 : k + 1), was[k]);
            cr::SaveData d;
            fbad += !readData(app, d) || !SDL_GetPathInfo(dataPath(app).c_str(), nullptr);
            for (int k = 0; k < 3; ++k) cr::readFile(slotPath(app, k == 2 ? 4 : k + 1), now[k]), fbad += was[k].empty() || now[k] != was[k];
            const bool places = d.places[0].has && d.places[1].has && !d.places[2].has && d.places[3].has && d.places[3].area == 25 &&
                                d.places[1].area == 13 && d.route() == 1; // (the newest, pair 2's, in play)
            const bool records = d.game.party[0].level == 11 && d.game.party[2].level == 12 && d.game.party[6].level == 14 &&
                                 d.game.flag(1100) && d.game.flag(1500) && d.game.flag(2300) && d.game.oz == 222;
            const bool bags = d.game.items[21] == 12 && d.game.routeBags[0][21] == 11 && d.game.routeBags[3][21] == 14 &&
                              d.game.items[95] == 4 && d.game.items[201] == 1 && d.game.items[257] == 1 && d.game.items[369] == 1;
            fbad += !places + !records + !bags;
            // the title's Load of pair 4: its own place, its own Chocolates, the one Oz
            app.game = cr::GameState();
            app.chosenPair = 4;
            openSaveScreen(app, true);
            const bool load4 = app.saveScreen.haveSave && app.saveScreen.data.map == 105 && app.saveScreen.data.game.items[21] == 14 &&
                               app.saveScreen.data.game.oz == 222 && app.saveScreen.data.leader == 6;
            app.chosenPair = 3;
            openSaveScreen(app, true);
            const bool none3 = !app.saveScreen.haveSave; // (no mark: New alone)
            app.saveScreen.open = false;
            // a save from pair 4's place: its own place and bag written, pair 2's kept
            app.chosenPair = 4;
            openSaveScreen(app, true);
            cr::SaveData p4 = app.saveScreen.data;
            app.saveScreen.open = false;
            app.game = p4.game, app.playerChar = p4.leader, app.area = p4.area, app.mapNumber = 98;
            app.game.items[21] = 30, app.game.oz = 999;
            fbad += !saveGame(app);
            cr::SaveData back;
            const bool saved = readData(app, back) && back.places[3].map == 98 && back.places[1].map == 97 && back.places[1].area == 13 &&
                               back.game.items[21] == 30 && back.game.routeBags[1][21] == 12 && back.game.oz == 999 && back.route() == 3;
            app.mapNumber = -1;
            fbad += !load4 + !none3 + !saved;
            // a second read takes data.dat as it is (the old files are folded once only)
            cr::writeSave(slotPath(app, 1), legacy(1, 900, 5, 7, 97));
            cr::SaveData again;
            fbad += !readData(app, again) || again.game.oz != 999;
            std::printf("one save (data.dat): save1 + save2 + save4 folded, kept as they were; places %s, records %s, bags %s; "
                        "pair 4 loads its own %s, pair 3 none %s, a save keeps the others %s; %d problems\n",
                        places ? "ok" : "WRONG", records ? "ok" : "WRONG", bags ? "ok" : "WRONG", load4 ? "ok" : "WRONG",
                        none3 ? "ok" : "WRONG", saved ? "ok" : "WRONG", fbad);
            problems += fbad;
            for (const char* f : {"save1.dat", "save2.dat", "save3.dat", "save4.dat", "data.dat"}) SDL_RemovePath(savePath(app, f).c_str());
            app.game = cr::GameState();
            app.playerChar = 0;
        }
        std::printf("save screen: the title's New -> var 1 = %d, opening map %d; cleared: emblems %u, %zu plates; %d problems so far\n",
                    var1, openingMap, ssc.emblems, savePlates(app).size(), problems);
        ssc.open = false;
        app.menuSoundLog.clear();
        for (const char* f : {"save1.dat", "data.dat", "resume.dat"}) SDL_RemovePath(savePath(app, f).c_str());
        SDL_RemovePath(app.saveDirOverride.c_str());
        app.saveDirOverride = keepDir;
        // The Instruction Manual (Options; OptionPrc 0xf..0x16): the manual items held (439..478), a topic's
        // page 56000 + item - 439 in the message window, paged by tap, the five of MsgUty_SetMsg's table with
        // their pictures and the window at _mwin[4]. The first Options of a game open on 56035 "Option" (flag
        // 0x43), once. Then Scenarios (the title)
        {
            const cr::GameState keepGame = app.game;
            const bool keepType = app.typeOn, keepTitle = app.titleOpen, keepOptions = app.optionsOpen;
            app.typeOn = false;
            app.optionsOpen = false;
            for (int item = kManualFirstItem; item <= kManualLastItem; ++item) app.game.items.erase(item);
            for (int item : {439, 440, 441, 448, 451, 473}) app.game.items[item] = 1;
            app.game.setFlag(0x43, false);
            const std::vector<int> keepSeen = app.seenFlags;
            app.seenFlags.clear();
            app.titleOpen = false;
            App::ManualMsg& mm = app.manualMsg;
            openOptions(app, 0);
            problems += !app.manualOpen || app.manualIntro != kOptionIntroPage || manualPage(app, kOptionIntroPage).empty() ||
                        !app.game.flag(0x43) || mm.state != 2 || mm.id != kOptionIntroPage || mm.picture;
            // OptionPrc 0..2: no menu mask, the page held back until the window has shut - from the title too
            int aboutHeld = 0;
            auto held = [&] {
                bool atOnce = false;
                stepOptions(app);
                aboutHeld += menuMaskWanted(app, atOnce) < 0 && app.optionsAnim == 0;
            };
            held();
            for (int f = 0; f < 6; ++f) stepManual(app);
            problems += mm.win.state != 3;
            manualKey(app, SDLK_ESCAPE); // (the port's Back: the window closes, then on to Options)
            for (int f = 0; f < 8; ++f) stepManual(app);
            problems += app.manualOpen;
            stepOptions(app);
            aboutHeld += app.optionsAnim == 1;
            app.optionsOpen = false;
            openOptions(app, 0); // (seen: not again)
            problems += app.manualOpen;
            app.optionsOpen = false;
            app.game.setFlag(0x43, false);
            // seen once, it stays seen: a game without the flag (a new launch, another save) opens on the page
            problems += !seenFlag(app, 0x43);
            openOptions(app, 0);
            problems += app.manualOpen;
            app.optionsOpen = false;
            app.seenFlags.clear(); // (a fresh install's title)
            app.titleOpen = true;
            openOptions(app, 0);
            aboutHeld += app.manualOpen && app.manualIntro == kOptionIntroPage && mm.state == 2;
            held();
            for (int f = 0; f < 6; ++f) stepManual(app);
            manualKey(app, SDLK_ESCAPE);
            for (int f = 0; f < 8; ++f) stepManual(app);
            aboutHeld += !app.manualOpen && app.game.flag(0x43);
            app.optionsOpen = false;
            app.titleOpen = false;
            app.seenFlags = keepSeen;
            problems += aboutHeld != 5;
            std::printf("options: \"Option\" (56035) alone in the message window, the page after it, from the field and "
                        "the title: %d/5\n", aboutHeld);
            openManual(app, 0);
            const std::vector<int> topics = manualTopics(app);
            manualKey(app, SDLK_DOWN);
            manualKey(app, SDLK_DOWN);
            problems += topics.size() != 6 || app.manualCursor != 2 || topics[2] != 441 || manualPage(app, 56002).empty();
            manualKey(app, SDLK_PAGEDOWN); // (to the last, 473)
            manualKey(app, SDLK_UP);       // 451, "Solo and Pair"
            manualKey(app, SDLK_RETURN);
            problems += mm.state != 1;
            for (int f = 0; f < 5; ++f) stepManual(app); // 0x12's wait, then the window
            const size_t textPages = mm.pages.size();
            problems += mm.state != 2 || mm.id != 56012 || !mm.picture || mm.pic.page != -1 || textPages < 2;
            manualKey(app, SDLK_RETURN); // (opening: no tap yet)
            for (int f = 0; f < 6; ++f) stepManual(app);
            problems += mm.page != 0 || mm.win.state != 3;
            int lastPicPage = -1, framesSeen = 0, picturePages = 0, turned = 0;
            for (int turn = 0; turn < 12 && mm.state == 2; ++turn) {
                ++turned;
                manualKey(app, SDLK_RETURN);
                if (mm.state != 2) break;
                if (mm.pic.on) {
                    problems += mm.pic.page != lastPicPage + 1;
                    lastPicPage = mm.pic.page;
                    ++picturePages;
                    for (int f = 0; f < 70; ++f) { stepManual(app); framesSeen = std::max(framesSeen, mm.pic.frame + 1); }
                } else stepManual(app);
                app.lastTicks = 0;
                drawFrame(app, false);
                ++frames;
            }
            const int wantPictures = std::min(4, int(textPages) - 1); // (battle_01: 4 pages, from the first turn)
            problems += mm.state != 3 || mm.pic.on || picturePages != wantPictures || turned != int(textPages) || framesSeen < 4;
            for (int f = 0; f < 8; ++f) stepManual(app);
            problems += !app.manualOpen || mm.state != 0 || app.manualCursor != 4; // the list back at 451
            manualKey(app, SDLK_ESCAPE);
            problems += app.manualOpen || app.renderer.takeErrors();
            std::printf("manual: %zu topics held, 56012 %zu pages, %d with battle_01's picture (frames up to %d)\n", topics.size(),
                        textPages, picturePages, framesSeen);
            app.typeOn = keepType;
            app.titleOpen = keepTitle;
            app.optionsOpen = keepOptions;
            app.game = keepGame;
        }
        problems += expectSounds("manual", {6, 7, 6, 7, 1, 1, 5, 1, 2, 6, 7, 3}); // (the topics' MuScrList: __effgo(1) a focus change)
        cr::GameState chart;
        chart.setFlag(1092, true);
        chart.setFlag(1093, true);
        problems += scenarioState(chart, 0) != 3 || scenarioState(chart, 1) != 1 || scenarioState(chart, 2) != 0 ||
                    scenarioState(chart, 40) != 0 || scenarioPercent(chart) != 3;
        openScenarios(app);
        problems += !app.scenariosOpen || !app.scenariosSheet.id;
        app.lastTicks = 0;
        drawFrame(app, false);
        scenariosKey(app, SDLK_DOWN);
        problems += app.scenariosScroll >= 0;
        scenariosKey(app, SDLK_ESCAPE);
        problems += app.scenariosOpen;
        problems += expectSounds("scenarios", {3}); // scrolling is silent
        std::printf("scenarios chart drawn\n");
        std::printf("saves: save -> title Start -> load %s (map %d, flags, items, vars, Oz, party, play time, position); "
                    "resume point written, offered at the title and resumed, saved as you go %s; %d problems\n", loaded ? "ok" : "FAILED",
                    after.map, asYouGo ? "yes" : "NO", problems);
        mapErrors += problems;
        bootTitle(app);
        app.runner.stop();
    }
    { // Shops: the item table, stock and level rules, then the Ark's shop (area 7) opened by its
      // shopkeeper's script: buy, sell, Talk (EvrStart 4 while the script waits parked), leave
        int problems = 0;
        const cr::ItemParam* sword = app.itemTable.item(201);
        problems += app.itemTable.count() < 438 || !sword || sword->price != 450 || sword->kind != cr::kKindWeapon ||
                    sword->stat[2] != 3 || itemName(app, 201) == "item 201" || itemName(app, 1) == itemName(app, 2);
        cr::GameState g;
        problems += cr::shopLevel(g, 0) != 2;
        problems += cr::shopStock(g, app.itemTable, 0, 1, 2) != std::vector<int>{201, 202};
        problems += cr::shopStock(g, app.itemTable, 1, 2, 3) != std::vector<int>{243, 244, 245};
        problems += cr::shopStock(g, app.itemTable, 0, 0, 2) != std::vector<int>{1, 21, 28};
        problems += cr::shopStock(g, app.itemTable, 0, 3, 2) != std::vector<int>{426};
        g.setFlag(1072, true);
        problems += cr::shopLevel(g, 0) != 4; // two flags short of the progress flag
        g.setFlag(1074, true);
        problems += cr::shopLevel(g, 0) != 6;
        g.setFlag(1104, true);
        problems += cr::shopLevel(g, 0) != 7 || cr::shopLevel(g, 1) != 2;
        problems += cr::maxPurchase(450, 1000, 0) != 2 || cr::maxPurchase(10, 100000, 95) != 4;
        // The Ark's shop room (t0108, area 7) and NPC 251's script
        app.runner.stop();
        app.game = cr::GameState();
        app.mapNumber = -1;
        cr::WarpRequest w;
        w.valid = true;
        w.area = 7;
        w.map = 103;
        performWarp(app, w, -1);
        for (int f = 0; f < 3000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        problems += app.area != 7 || app.mapNumber != 103;
        app.runner.start(app.events, 251, app.game, app.area, app.mapNumber);
        bool stoodDuringShopScript = false;
        if (app.havePlayer && app.player.actions.size() > 2) {
            app.player.action = 2; // simulate entering the interaction during the run clip
            app.player.time = 1;
            app.walker.gait = cr::Gait::Run;
            updatePlayer(app, 1.0f / kFramesPerSecond, 0);
            stoodDuringShopScript = app.walker.gait == cr::Gait::Stand && app.player.action == 0;
        }
        problems += !stoodDuringShopScript;
        for (int f = 0; f < 600 && app.runner.running() && !app.shop.open; ++f) {
            stepEvents(app);
            app.dialogue.clear();
            app.tapPending = true;
        }
        const bool opened = app.shop.open && app.runner.running();
        problems += !opened || app.shop.level != 2;
        app.game.oz = 1000;
        shopKey(app, SDLK_DOWN);   // Buy
        shopKey(app, SDLK_RETURN);
        shopKey(app, SDLK_DOWN);   // Weapons
        shopKey(app, SDLK_RETURN);
        problems += app.shop.phase != App::Shop::Member; // (__Buy_CharChoiceProc: whose list)
        shopKey(app, SDLK_RETURN); // Escher's
        problems += app.shop.phase != App::Shop::List || app.shop.list != std::vector<int>{201, 202};
        app.menuSoundLog.clear();
        shopKey(app, SDLK_RETURN); // Trusty Longsword, 450: how many, with Yes / No beside
        for (int k = 0; k < 3; ++k) shopKey(app, SDLK_RIGHT); // as many as the Oz buy: 2, then past the end
        problems += app.shop.phase != App::Shop::Count || app.shop.count != 2 ||
                    shopMessage(app).find(itemName(app, 201)) == std::string::npos;
        shopKey(app, SDLK_RETURN); // Yes (Escher wears one already: no "Equip it?")
        problems += app.game.oz != 100 || app.game.items[201] != 2 || app.shop.phase != App::Shop::List;
        shopKey(app, SDLK_DOWN);   // Bastard Sword: 1800, too dear
        shopKey(app, SDLK_RETURN);
        problems += app.shop.phase != App::Shop::List;
        problems += expectSounds("shop: item, count 2 then its end twice, Yes, too dear", {2, 2, 4, 4, 22, 1, 4}); // (the list: __effgo(1))
        // Armor for Escher, worn at once: what he wore goes in the bag
        app.game.oz = 2000;
        shopKey(app, SDLK_ESCAPE); // the pair again
        shopKey(app, SDLK_ESCAPE);
        shopKey(app, SDLK_DOWN);   // Armor
        shopKey(app, SDLK_RETURN);
        shopKey(app, SDLK_RETURN); // Escher's
        problems += app.shop.list != std::vector<int>{215, 216};
        shopKey(app, SDLK_DOWN);
        shopKey(app, SDLK_RETURN); // 216, 1350
        shopKey(app, SDLK_RETURN); // Yes
        const bool asked = app.shop.phase == App::Shop::EquipAsk && shopMessage(app).find(itemName(app, 216)) != std::string::npos;
        shopKey(app, SDLK_RETURN); // Yes, equip it
        const cr::PartyMember& escher = app.game.party[0];
        problems += !asked || app.game.oz != 650 || escher.equip[cr::kArmor] != 216 || app.game.items[216] != 0 ||
                    app.game.items[215] != 1 || app.shop.phase != App::Shop::List;
        problems += cr::maxStat(escher, 0, cr::kHp, app.itemTable) != 842 + 50 + 24 ||
                    cr::maxStat(escher, 0, cr::kAttack, app.itemTable) != 180 + 5 + 3 ||
                    cr::maxStat(escher, 0, cr::kDefense, app.itemTable, 215) != 135 + 5 + 5;
        shopKey(app, SDLK_RIGHT);  // Musiea: her own armor
        problems += app.shop.member != 1 || app.shop.list != std::vector<int>{243, 244};
        shopKey(app, SDLK_LEFT);
        app.lastTicks = 0;
        drawFrame(app, false); // the board, the list
        ++frames;
        app.menuSoundLog.clear();
        shopKey(app, SDLK_ESCAPE);
        shopKey(app, SDLK_ESCAPE);
        shopKey(app, SDLK_ESCAPE); // back to Talk / Buy / Sell
        shopKey(app, SDLK_DOWN);   // Sell
        shopKey(app, SDLK_RETURN);
        shopKey(app, SDLK_UP);     // Weapons
        shopKey(app, SDLK_RETURN);
        shopKey(app, SDLK_RETURN); // Escher's
        problems += app.shop.list != std::vector<int>{201};
        shopKey(app, SDLK_RETURN);
        shopKey(app, SDLK_RETURN); // one, for half the price
        problems += app.game.oz != 875 || app.game.items[201] != 1 || app.shop.phase != App::Shop::List;
        problems += expectSounds("shop: Back, Back, Back, Sell, Weapons, Escher, the sword, Yes", {3, 3, 3, 4, 2, 4, 2, 2, 2, 22});
        app.lastTicks = 0;
        drawFrame(app, false);
        ++frames;
        shopKey(app, SDLK_ESCAPE);
        shopKey(app, SDLK_ESCAPE);
        shopKey(app, SDLK_ESCAPE);
        shopKey(app, SDLK_UP);
        shopKey(app, SDLK_UP);     // Talk
        shopKey(app, SDLK_RETURN);
        problems += app.shop.phase != App::Shop::Talk;
        int talkFrames = 0;
        for (; talkFrames < 3000 && app.shop.phase == App::Shop::Talk; ++talkFrames) {
            stepShop(app);
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        const size_t talkMessages = app.parkedRunner.result().messages.size();
        problems += app.shop.phase != App::Shop::Main || app.game.flag(4090) || !app.runner.running() || talkMessages == 0;
        shopKey(app, SDLK_ESCAPE); // leave: the shopkeeper's script goes on to its end
        for (int f = 0; f < 600 && app.runner.running(); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            app.tapPending = true;
        }
        problems += app.shop.open || app.runner.running();
        std::printf("shops: %d items; Ark shop opened by its script %s, gait stopped during its script %s, "
                    "bought 2 and sold 1 (%d Oz left), Talk played %zu messages in %d frames; %d problems\n",
                    app.itemTable.count(), opened ? "ok" : "FAILED", stoodDuringShopScript ? "yes" : "NO",
                    app.game.oz, talkMessages, talkFrames, problems);
        mapErrors += problems;
        bootTitle(app);
        app.runner.stop();
    }
    { // The Camp menu's pages: seeds and an MP item used on a member, a weapon worn, drawn without errors
        int problems = 0;
        app.game = cr::GameState();
        app.playerChar = 0;
        app.game.items = {{3, 3}, {24, 1}, {21, 1}, {202, 1}};
        app.game.party[0].mp = 10;
        app.game.items[0x59] = 1; // (an ability: CampMainSet's 56010, once)
        app.menuSoundLog.clear();
        const int campMap = app.mapNumber;
        app.mapNumber = 42; // (away from the home maps: CampMainSet heals there, and the Plum below wants MP to fill)
        openCamp(app);
        auto key = [&](SDL_Keycode k) { campKey(app, k); };
        auto steps = [&](int n) { for (int f = 0; f < n; ++f) stepCamp(app); };
        App::Camp& c = app.camp;
        problems += c.notice != 56010 || !app.game.flag(0x38);
        app.dialogue.clear();
        steps(1);
        problems += c.notice != 0;
        const int atk0 = cr::maxStat(app.game.party[0], 0, cr::kAttack, app.itemTable);
        c.cursor = 0, key(SDLK_RETURN);                       // Items, the first time: 56016 first (flag 0x39)
        problems += c.notice != 56016 || c.page != App::Camp::Main;
        app.dialogue.clear();
        steps(1);                                             // the message read: the page (CampMain 2 -> 6)
        problems += c.page != App::Camp::Items || c.list.empty() || c.list[0] != 3;
        steps(8);
        key(SDLK_RETURN), key(SDLK_RETURN);                   // on Escher: how many?
        key(SDLK_RIGHT), key(SDLK_RIGHT), key(SDLK_RIGHT);    // 3 (the bag's all)
        problems += c.count != 3;
        key(SDLK_RETURN);                                     // Yes: the board (CampMainPrc 8, 9)
        problems += c.phase != App::Camp::Result;
        key(SDLK_RETURN);                                     // the tap: the list again
        const int atk1 = cr::maxStat(app.game.party[0], 0, cr::kAttack, app.itemTable);
        problems += atk1 != atk0 + 3 || app.game.party[0].seeds[0] != 3 || ownedCount(app, 3) != 0;
        const auto plum = std::find(c.list.begin(), c.list.end(), 24); // Plum: MP + 100
        problems += plum == c.list.end();
        if (plum != c.list.end()) c.listCursor = int(plum - c.list.begin());
        key(SDLK_RETURN), key(SDLK_RETURN), key(SDLK_RETURN), key(SDLK_RETURN);
        problems += app.game.party[0].mp != 60 || ownedCount(app, 24) != 0; // 10 + 100, capped at 60
        const auto choc = std::find(c.list.begin(), c.list.end(), 21); // HP items do nothing on the field
        if (choc != c.list.end()) c.listCursor = int(choc - c.list.begin()), key(SDLK_RETURN);
        problems += c.phase != App::Camp::List || ownedCount(app, 21) != 1;
        int frames = 0;
        for (int f = 0; f < 3; ++f) { app.lastTicks = 0; drawFrame(app, false); ++frames; }
        key(SDLK_ESCAPE);                                     // Back: the page's windows go, then the main page's come
        problems += c.pageOut == 0;
        steps(8);
        problems += c.page != App::Camp::Main || c.pageOut != 0;
        c.cursor = 1, key(SDLK_RETURN), key(SDLK_RETURN);   // Equip: Escher's weapon
        key(SDLK_RETURN);
        problems += c.phase != App::Camp::List || c.list.empty() || c.list[0] != 202;
        for (int f = 0; f < 3; ++f) { app.lastTicks = 0; drawFrame(app, false); ++frames; }
        key(SDLK_RETURN);                                     // wear the Bastard Sword
        problems += app.game.party[0].equip[0] != 202 || ownedCount(app, 201) != 1 || ownedCount(app, 202) != 0;
        key(SDLK_ESCAPE), key(SDLK_ESCAPE);
        steps(8);
        problems += c.page != App::Camp::Main;
        c.cursor = 3, key(SDLK_RETURN), key(SDLK_DOWN), key(SDLK_RETURN); // Status: Musiea's
        problems += c.page != App::Camp::Status || c.member != 1 || c.phase != App::Camp::Slot;
        key(SDLK_RIGHT);                                      // her equipment
        problems += c.statusView != 1 || c.list.size() != size_t(campui::kEquipSlots);
        for (int f = 0; f < 3; ++f) { app.lastTicks = 0; drawFrame(app, false); ++frames; }
        const int errs = app.renderer.takeErrors();
        key(SDLK_ESCAPE), key(SDLK_ESCAPE);
        steps(8);
        key(SDLK_ESCAPE);                                     // Back on the main page: the windows go, then it closes
        problems += !app.campOpen;
        steps(5);
        problems += app.campOpen + errs;
        problems += expectSounds("camp", {2, 2, 2, 2, 2, 4, 20, 2, 2, 2, 20, 2, 4, 3, 2, 2, 2, 21, 3, 3, 2, 4, 2, 5, 3, 3, 3, 9});
        app.mapNumber = campMap;
        std::printf("camp: 56010 / 56016 first, 3 seeds +%d attack, Plum to MP %d, Bastard Sword worn, status page and list; %d "
                    "frames, %d render API errors; %d problems\n", atk1 - atk0, app.game.party[0].mp, frames, errs, problems);
        mapErrors += problems;
        glErrors += errs;
        app.game = cr::GameState();
    }
    { // The field menu (menuf_mainPrc): the corner button slides the bar out and back in 4 frames;
      // skill 170 known adds the encounter switch (per pair, refused under flag 4076), and while
      // it's off the walked distance doesn't count; skill 174 worn banks 1 EXP every 10 units,
      // paid into the next win; both are kept in the save
        int problems = 0;
        app.game = cr::GameState();
        app.playerChar = 0;
        app.menuSoundLog.clear();
        App::FieldMenu& fm = app.fieldMenu;
        fm = App::FieldMenu();
        toggleFieldMenu(app);
        float x0, y0, x4, y4;
        fieldButtonPos(app, 0, x0, y0);
        for (int f = 0; f < 4; ++f) stepFieldMenu(app);
        fieldButtonPos(app, 0, x4, y4);
        problems += !fm.open || fm.anim != 0 || x0 != 460 || y0 != -28 || x4 != 432 || y4 != 32;
        problems += fieldButtonShown(app, 3); // not known yet
        pressFieldButton(app, 3);
        problems += app.game.noEncounters[0];
        app.game.items[170] = 1;
        pressFieldButton(app, 3); // off
        const float d0 = app.encounters.walkedDistance();
        fieldWalked(app, 50);
        problems += !app.game.noEncounters[0] || app.encounters.walkedDistance() != d0;
        app.playerChar = 2; // the next pair's switch is its own
        problems += fieldEncountersOff(app);
        app.playerChar = 0;
        app.game.setFlag(4076, true);
        pressFieldButton(app, 3); // refused
        problems += !app.game.noEncounters[0];
        app.game.setFlag(4076, false);
        pressFieldButton(app, 3); // on again
        fieldWalked(app, 50);
        problems += app.game.noEncounters[0] || app.encounters.walkedDistance() != d0 + 50;
        problems += expectSounds("field menu, encounter switch", {8, 3, 4, 2});
        toggleFieldMenu(app);
        for (int f = 0; f < 4; ++f) stepFieldMenu(app);
        fieldButtonPos(app, 3, x4, y4);
        problems += fm.open || x4 != 460 || y4 != -28;
        app.menuSoundLog.clear();
        // skill 174 on Escher (plate 499): 100 one-unit steps and a 25-unit one
        app.game.items[174] = 1, app.game.items[499] = 1;
        app.game.party[0].genes[1] = 499;
        app.walkExpDistance = 0;
        for (int k = 0; k < 100; ++k) fieldWalked(app, 1);
        const int bank100 = app.game.walkExp[0];
        fieldWalked(app, 25);
        fieldWalked(app, 0.5f);
        const int bank = app.game.walkExp[0];
        problems += bank100 != 10 || bank != 12;
        // kept in the save
        cr::SaveData sd = captureSave(app);
        const std::string path = savePath(app, "fieldmenu-test.dat");
        cr::SaveData back;
        problems += !cr::writeSave(path, sd) || !cr::readSave(path, back) || back.game.walkExp[0] != bank ||
                    back.game.noEncounters != sd.game.noEncounters;
        sd.game.noEncounters[3] = true;
        sd.game.party[0].level = 100;
        sd.game.party[0].exp = cr::expForLevel(100);
        problems += !cr::writeSave(path, sd) || !cr::readSave(path, back) || !back.game.noEncounters[3] ||
                    back.game.party[0].level != 100 || back.game.party[0].exp != cr::expForLevel(100);
        SDL_RemovePath(path.c_str());
        // paid into a win, before the Apples double it
        int paid = -1;
        startBattle(app, {1, 0, -1, 0}, 0);
        if (app.battle.active) {
            App::Battle& b = app.battle;
            for (int i = 2; i < 5; ++i) b.rules.f[size_t(i)].hp = 0;
            b.phase = App::Battle::Acting;
            b.turnOn = false;
            const int before = b.rules.expGained;
            endRound(app);
            for (int f = 0; f < 10 && b.phase == App::Battle::Won; ++f) stepBattle(app); // (1000: ten frames, then 0x3e9 settles the win)
            paid = b.rules.expGained - before;
            endBattle(app, true);
            app.runner.stop();
        }
        problems += paid != bank || app.game.walkExp[0] != 0;
        // The map screen: every field has its spot (the 3D rooms 13 / 29 / 46 / 63 none) and every
        // marker lies on its gate's 480 x 320 map; map 0 with the story at step 3
        int spots = 0, offMap = 0;
        for (int mp = 0; mp < 64; ++mp) {
            const bool room3d = mp == 13 || mp == 29 || mp == 46 || mp == 63;
            spots += mapui::kMapSpot[mp][0] >= 0;
            offMap += room3d != (mapui::kMapSpot[mp][0] < 0) ||
                      (!room3d && (mapui::kMapSpot[mp][0] - 24 < 0 || mapui::kMapSpot[mp][0] + 24 > 480 ||
                                   mapui::kMapSpot[mp][1] - 20 < 0 || mapui::kMapSpot[mp][1] + 28 > 320));
        }
        for (const auto& gate : mapui::kMapMark)
            for (const auto& step : gate)
                for (const auto& mk : step)
                    offMap += mk[0] >= 0 && (mk[0] - 8 < 0 || mk[0] + 8 > 480 || mk[1] - 16 < 0 || mk[1] > 320);
        const int keepMap = app.mapNumber;
        app.mapNumber = 0;
        app.game.setFlag(mapui::kFlagBase + 3, true);
        app.menuSoundLog.clear();
        fm.open = true, fm.anim = 0;
        pressFieldButton(app, 2);
        const App::MapScreen& ms = app.mapScreen;
        problems += !ms.open || ms.gate != 0 || !ms.tex.id || ms.spot[0] != 48 || ms.spot[1] != 196 ||
                    ms.marks[0][0] != 84 || ms.marks[1][0] != 104;
        for (int f = 0; f < 6; ++f) stepMapScreen(app);
        app.lastTicks = 0;
        drawFrame(app, false);
        const int mapErrs = app.renderer.takeErrors();
        closeMapScreen(app);
        for (int f = 0; f < 4; ++f) stepMapScreen(app);
        problems += ms.open + mapErrs + expectSounds("map screen", {2, 2}) + offMap + (spots != 60);
        app.mapNumber = keepMap;
        std::printf("field menu: bar slides out and back, encounter switch per pair (flag 4076 refuses it), skill 174 "
                    "banked %d EXP, %d paid into a win; map screen: %d spots, %d off the map; %d problems\n", bank, paid,
                    spots, offMap, problems);
        mapErrors += problems;
        app.game = cr::GameState();
        fm = App::FieldMenu();
    }
    { // A puzzle room (stage 0, map 114: teleport): its objects on the grid, a swap with a block,
      // both gems picked up (+1 ESP each), then the gem crystal's scene, and the room cleared
        int problems = 0;
        app.runner.stop();
        app.game = cr::GameState();
        app.mapNumber = -1;
        cr::WarpRequest w;
        w.valid = true, w.area = cr::kPuzzleArea, w.map = cr::kFirstPuzzleMap;
        app.game.setFlag(3070, true); // as a dungeon's way into a puzzle room sets it: the area's script 0 shows the banner
        performWarp(app, w, -1);
        int bannerFrames = 0, bannerPeak = 0; // Uty_PuzzleStart: up, held, gone
        float bannerX1 = 0;
        for (int f = 0; f < 3000 && (app.runner.running() || app.pendingEntry != App::Entry::None || app.puzzleBanner.on); ++f) {
            stepEvents(app);
            if (app.puzzleBanner.on) {
                ++bannerFrames;
                if (app.puzzleBanner.alpha > bannerPeak) bannerPeak = app.puzzleBanner.alpha, bannerX1 = app.puzzleBanner.x1;
                if (bannerFrames == 20) { // a look at it held (CR_PUZZLE_SHOT_BANNER: saved)
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    if (const char* path = SDL_getenv("CR_PUZZLE_SHOT_BANNER")) saveFrame(app, path);
                }
            }
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        const bool banner = bannerFrames >= 38 && bannerFrames <= 44 && bannerPeak == 255 && bannerX1 == 60 && !app.puzzleBanner.on &&
                            !app.game.flag(3070);
        problems += !banner;
        std::printf("puzzle room: \"Puzzle Start\" banner %s (%d frames, alpha up to %d, at x %.0f)\n",
                    banner ? "shown" : "WRONG", bannerFrames, bannerPeak, bannerX1);
        App::Puzzle& pz = app.puzzle;
        int blocks = 0, gems = 0;
        for (const App::Puzzle::Obj& o : pz.objs) blocks += o.type == cr::kTypeNormal, gems += o.type == cr::kTypeGem;
        problems += pz.stage != 0 || blocks != 2 || gems != 2 || pz.esp != 0;
        // no ESP yet: a swap is refused (only the mark)
        setActor(app, 0, puzzle::centre(app, 2, 2), nullptr, false);
        const int block = puzzle::objectAt(app, 1, 2, 1);
        problems += block < 0;
        if (block >= 0) puzzle::teleportTo(app, block);
        const bool refused = !pz.busy && pz.tpState == 0 && app.game.vars[13] == 0 && pz.punished > 0;
        problems += !refused;
        // the gem at (0, 0), walked to (ESP 1): effect 2 there (NormalGem::onIdle: 3 is a teleported gem's)
        const int gemFx = app.fieldEffectCount[2] + app.fieldEffectCount[3];
        setActor(app, 0, puzzle::centre(app, 0, 0), nullptr, true);
        for (int f = 0; f < 3; ++f) stepEvents(app);
        const bool firstGem = pz.esp == 1 && app.fieldEffectCount[2] + app.fieldEffectCount[3] == gemFx + 1;
        stepPuzzleHud(app); // the HUD has shown a count above 0
        const bool popped = pz.pop > 0;
        // stand at the centre (2, 2) and swap with the block at (2, 1) (ESP 0)
        setActor(app, 0, puzzle::centre(app, 2, 2), nullptr, false);
        app.menuSoundLog.clear();
        if (block >= 0) puzzle::teleportTo(app, block);
        for (int f = 0; f < 30; ++f) stepEvents(app);
        int pr = -1, pc = -1;
        puzzle::cellOf(app, app.walker.pos, pr, pc);
        for (int f = 0; f < 3; ++f) stepPuzzleHud(app);
        if (const char* path = SDL_getenv("CR_PUZZLE_SHOT_HUD")) { // the pad at 0 ESP (CR_PUZZLE_SHOT_HUD: saved)
            pz.blink = 0;
            const int keepFade = app.fieldMenu.fade;
            app.fieldMenu.fade = 320;
            app.lastTicks = 0;
            drawFrame(app, false);
            saveFrame(app, path);
            app.fieldMenu.fade = keepFade;
        }
        const bool hudBlink = popped && pz.hadEsp && pz.blink > 0 && pz.shownEsp == 0;
        problems += !hudBlink;
        const bool swapped = firstGem && block >= 0 && pz.objs[size_t(block)].row == 2 && pz.objs[size_t(block)].col == 2 &&
                             pr == 2 && pc == 1 && puzzle::objectAt(app, 1, 2, 2) == block && app.game.vars[13] == 1 && pz.esp == 0;
        // the gem at (4, 4) (ESP 1, none left)
        setActor(app, 0, puzzle::centre(app, 4, 4), nullptr, true);
        for (int f = 0; f < 3; ++f) stepEvents(app);
        const int espAfterGems = pz.esp;
        problems += espAfterGems != 1;
        problems += !swapped || puzzle::gemsLeft(app);
        // the crystal's scene starts by itself, sets the room's flag, then the crystal shows
        bool sceneRan = false;
        for (int f = 0; f < 6000; ++f) {
            stepEvents(app);
            sceneRan |= app.runner.running();
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
            if (sceneRan && !app.runner.running() && app.pendingEntry == App::Entry::None) break;
        }
        for (int f = 0; f < 3; ++f) stepEvents(app); // the crystal shows the frame after
        bool crystal = false;
        for (const App::Puzzle::Obj& o : pz.objs) crystal |= o.type == cr::kTypeCrystal && o.shown;
        const bool roomFlag = app.game.flag(3300), clearFlag = app.game.flag(3000);
        problems += !sceneRan || (!crystal && !clearFlag);
        app.lastTicks = 0;
        drawFrame(app, false);
        const int errs = app.renderer.takeErrors();
        problems += errs;
        // stage 8 (map 122): the block at (4, 4) teleported onto the switch at (0, 0) presses it - the switch crystal's scene
        app.runner.stop();
        app.game = cr::GameState();
        app.mapNumber = -1;
        w.map = cr::kFirstPuzzleMap + 8;
        performWarp(app, w, -1);
        for (int f = 0; f < 3000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) { // the room's entry script, to its end
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
            quiet = app.runner.running() || app.pendingEntry != App::Entry::None ? 0 : quiet + 1;
        }
        auto crystalShown = [&] {
            for (const App::Puzzle::Obj& o : pz.objs)
                if (o.type == cr::kTypeCrystal && o.shown) return true;
            return false;
        };
        for (int f = 0; f < 3; ++f) stepEvents(app);
        const bool hiddenBefore = !crystalShown() && !app.runner.running(); // the room's script done, the switch up
        setActor(app, 0, puzzle::centre(app, 0, 0), nullptr, false);
        const int b8 = puzzle::objectAt(app, 1, 4, 4);
        if (b8 >= 0) puzzle::teleportTo(app, b8);
        for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
            quiet = app.runner.running() || app.puzzle.tpState ? 0 : quiet + 1;
        }
        const bool switchScene = hiddenBefore && crystalShown();
        const bool pressed = puzzle::allSwitchesOn(app);
        if (SDL_getenv("CR_PUZZLE_DEBUG"))
            for (const App::Puzzle::Obj& o : pz.objs)
                if (o.type == cr::kTypeCrystal || o.type == cr::kTypeWeighSwitch)
                    std::printf("  obj npc %d id %d type %d bits %03x shown %d on %d; flag 3308 %d 4000 %d, runner %s, last %s\n", o.npc,
                                app.npcs[size_t(o.npc)].id, o.type, app.npcs[size_t(o.npc)].puzzle, o.shown, o.on, app.game.flag(3308),
                                app.game.flag(4000), app.runner.running() ? app.runner.where().c_str() : "idle", app.lastEvent.c_str());
        problems += b8 < 0 || !pressed || !switchScene;
        app.runner.stop();
        std::printf("puzzle room: stage 8 - the block teleported onto the switch %s, the switch crystal %s\n",
                    pressed ? "presses it" : "DIDN'T press it", switchScene ? "came" : "DIDN'T come");
        // stage 24 (map 138, psychokinesis): (3, 3) right to (3, 4), up to (1, 4) under the fixed
        // block at (0, 4), then (1, 1) right onto the switch at (1, 3); a letting go costs nothing;
        // then (3, 1) sent right into the leader at (3, 2) squashes the try
        {
            app.game = cr::GameState();
            app.mapNumber = -1;
            w.map = cr::kFirstPuzzleMap + 24;
            performWarp(app, w, -1);
            for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) {
                stepEvents(app);
                app.dialogue.clear();
                if (app.choiceFlag >= 0) answerChoice(app, true);
                app.tapPending = true;
                quiet = app.runner.running() || app.pendingEntry != App::Entry::None ? 0 : quiet + 1;
            }
            setActor(app, 0, puzzle::centre(app, 2, 2), nullptr, false);
            int poseOk = 0, quakes = 0, slides = 0;
            auto slide = [&](int r, int c, int dir) { // the block's cell after the slide (-1 none)
                const int t = puzzle::objectAt(app, 1, r, c);
                if (t < 0) return std::pair<int, int>{-1, -1};
                puzzle::pkGrab(app, t);
                for (int f = 0; f < 20 && pz.objs[size_t(t)].pk != 2; ++f) stepEvents(app);
                updatePlayer(app, 0, 0); // the leader's pose while it's held
                poseOk += app.player.action == 3 && app.player.loopFrom == 2;
                puzzle::pkSend(app, dir);
                bool quaked = false;
                for (int f = 0; f < 200 && pz.pkState && pz.scene == 4; ++f) {
                    stepEvents(app);
                    quaked |= app.fieldQuake.mode == 1 && app.fieldQuake.frames == 8 && app.fieldQuake.amp == 0.5f;
                }
                ++slides, quakes += quaked;
                updatePlayer(app, 0, 0);
                poseOk -= app.player.action == 3; // standing again
                return std::pair<int, int>{pz.objs[size_t(t)].row, pz.objs[size_t(t)].col};
            };
            const bool pkRoom = pz.stage == 24 && pz.esp == -1;
            const auto drop = slide(3, 3, 2);  // down: (4, 3) free, then the edge
            const auto drop2 = slide(4, 3, 2); // at the edge: nowhere to go - let go, no use counted
            const bool letGo = drop2 == std::pair<int, int>{4, 3} && app.game.vars[13] == 1 && !pz.busy;
            // the solution, from the room as it began
            app.runner.stop();
            app.npcs = app.events.npcs(app.mapNumber);
            puzzle::enter(app);
            pz.scene = 4;
            setActor(app, 0, puzzle::centre(app, 2, 2), nullptr, false);
            const auto s1 = slide(3, 3, 4), s2 = slide(3, 4, 1), s3 = slide(1, 1, 4);
            const bool solved = s1 == std::pair<int, int>{3, 4} && s2 == std::pair<int, int>{1, 4} &&
                                s3 == std::pair<int, int>{1, 3} && puzzle::allSwitchesOn(app) && app.game.vars[13] == 3;
            for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) { // the switch crystal's scene
                stepEvents(app);
                app.dialogue.clear();
                if (app.choiceFlag >= 0) answerChoice(app, true);
                app.tapPending = true;
                quiet = app.runner.running() ? 0 : quiet + 1;
            }
            // the squash
            setActor(app, 0, puzzle::centre(app, 3, 2), nullptr, false);
            app.menuSoundLog.clear();
            slide(3, 1, 4);
            const bool squashed = pz.scene == 2;
            if (SDL_getenv("CR_PUZZLE_DEBUG"))
                std::printf("  squash: scene %d pkState %d busy %d runner %s cleared %d dialogue %d, player %.1f %.1f\n", pz.scene,
                            pz.pkState, pz.busy, app.runner.running() ? app.runner.where().c_str() : "idle", puzzle::cleared(app),
                            int(app.dialogue.size()), app.walker.pos.x, app.walker.pos.z);
            for (int f = 0; f < 40 && pz.scene != 4; ++f) stepEvents(app);
            const bool restored = pz.scene == 4 && !puzzle::allSwitchesOn(app) && puzzle::objectAt(app, 1, 1, 1) >= 0 &&
                                  puzzle::objectAt(app, 1, 3, 3) >= 0 && app.game.vars[13] == 0;
            problems += poseOk < 3 || quakes < 3;
            std::printf("puzzle room: stage 24 - the holding pose %d of %d, landing quakes %d\n", poseOk, slides, quakes);
            problems += !pkRoom || drop != std::pair<int, int>{4, 3} || !letGo ||
                        !solved || !squashed || !restored;
            std::printf("puzzle room: stage 24 (psychokinesis) - slides %s, letting go %s, the switch %s in 3, squash %s, "
                        "room %s\n", drop == std::pair<int, int>{4, 3} ? "ok" : "FAILED", letGo ? "free" : "COUNTED", solved ? "pressed" : "NOT pressed",
                        squashed ? "resets" : "DIDN'T reset", restored ? "restored" : "NOT restored");
            app.runner.stop();
        }
        // stage 32 (map 146, toughness, ESP 2): from (1, 1) the fragile block at (0, 1) breaks
        // (ESP 1), the gem at (0, 0) is reached through its cell (ESP 2); at 0 ESP a tap is refused.
        // Stage 40 (map 154, ESP 0): the gem at (1, 1) (ESP 1), a tap on a red block puts both
        // away and brings both blue ones; the gem at (1, 3) (ESP 1), then from the red block's cell
        // (0, 2) a tap on a blue one brings the reds back onto the leader - the room resets
        {
            auto room = [&](int stageNo) {
                app.runner.stop();
                app.game = cr::GameState();
                app.mapNumber = -1;
                w.map = cr::kFirstPuzzleMap + stageNo;
                performWarp(app, w, -1);
                for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) {
                    stepEvents(app);
                    app.dialogue.clear();
                    if (app.choiceFlag >= 0) answerChoice(app, true);
                    app.tapPending = true;
                    quiet = app.runner.running() || app.pendingEntry != App::Entry::None ? 0 : quiet + 1;
                }
            };
            auto walkTo = [&](int r, int c) {
                setActor(app, 0, puzzle::centre(app, r, c), nullptr, true);
                for (int f = 0; f < 3; ++f) stepEvents(app);
            };
            room(32);
            walkTo(1, 1);
            const int frag = puzzle::objectAt(app, 1, 0, 1);
            const bool fragOk0 = pz.stage == 32 && pz.esp == 2 && frag >= 0 && pz.objs[size_t(frag)].type == cr::kTypeFragile &&
                                 pz.objs[size_t(frag)].shown;
            app.walker.heading = 3.14159265f; // facing row - 1
            puzzle::abilityKey(app);
            for (int f = 0; f < 3; ++f) stepEvents(app);
            const bool broke = frag >= 0 && !pz.objs[size_t(frag)].shown && puzzle::objectAt(app, 1, 0, 1) < 0 && pz.esp == 1 &&
                               app.game.vars[13] == 1 && !npcVisible(app, size_t(pz.objs[size_t(frag)].npc));
            walkTo(0, 1);
            walkTo(0, 0);
            const bool gem = pz.esp == 2;
            pz.esp = 0;
            const int frag2 = puzzle::objectAt(app, 1, 1, 0);
            puzzle::toughTap(app, frag2);
            for (int f = 0; f < 3; ++f) stepEvents(app);
            const bool refusedT = frag2 >= 0 && pz.objs[size_t(frag2)].shown && app.game.vars[13] == 1 && pz.punished > 0;
            room(40);
            walkTo(1, 1);
            const int red = puzzle::objectAt(app, 1, 0, 2), blue = puzzle::objectAt(app, 1, 2, 0);
            auto solid = [&](int t) { return t >= 0 && pz.objs[size_t(t)].shown && !pz.objs[size_t(t)].ghost; };
            auto ghostOf = [&](int t) {
                const App::NpcDraw* d = t >= 0 ? puzzle::drawOf(app, pz.objs[size_t(t)].npc) : nullptr;
                return t >= 0 && pz.objs[size_t(t)].ghost && d && d->alpha < 0.4f;
            };
            const bool start = pz.stage == 40 && solid(red) && ghostOf(blue) && pz.esp == 1;
            puzzle::toughTap(app, red);
            for (int f = 0; f < 8; ++f) stepEvents(app);
            const int red2 = puzzle::objectAt(app, 1, 4, 2), blue2 = puzzle::objectAt(app, 1, 2, 4);
            const bool swappedRB = ghostOf(red) && ghostOf(red2) && solid(blue) && solid(blue2) && pz.esp == 0;
            if (const char* shot = SDL_getenv("CR_PUZZLE_SHOT")) { // a look at the changed-over blocks
                app.lastTicks = 0;
                drawFrame(app, false);
                int sw = 0, sh = 0;
                std::vector<uint8_t> px;
                if (app.renderer.readPixels(px, sw, sh)) {
                    SDL_Surface* surf = SDL_CreateSurface(sw, sh, SDL_PIXELFORMAT_RGBA32);
                    for (int y = 0; y < sh; ++y)
                        std::memcpy(static_cast<uint8_t*>(surf->pixels) + size_t(y) * surf->pitch, &px[size_t(y) * sw * 4], size_t(sw) * 4);
                    SDL_SaveBMP(surf, shot);
                    SDL_DestroySurface(surf);
                }
            }
            walkTo(1, 3);
            setActor(app, 0, puzzle::centre(app, 0, 2), nullptr, false);
            puzzle::toughTap(app, blue);
            stepEvents(app);
            const bool squashedRB = pz.scene == 2;
            for (int f = 0; f < 40 && pz.scene != 4; ++f) stepEvents(app);
            const bool restoredRB = pz.scene == 4 && solid(puzzle::objectAt(app, 1, 0, 2)) && ghostOf(puzzle::objectAt(app, 1, 2, 0));
            problems += !fragOk0 || !broke || !gem || !refusedT || !start || !swappedRB || !squashedRB || !restoredRB;
            std::printf("puzzle room: stage 32 (toughness) - fragile block %s, gem %s, at 0 ESP %s; stage 40 - red / blue %s, "
                        "%s, squash %s, room %s\n", fragOk0 && broke ? "breaks" : "DIDN'T break", gem ? "ok" : "MISSED",
                        refusedT ? "refused" : "NOT refused", start ? "start right" : "start WRONG", swappedRB ? "change over" : "DON'T change",
                        squashedRB ? "resets" : "DIDN'T reset", restoredRB ? "restored" : "NOT restored");
            app.runner.stop();
        }
        // stage 48 (map 162, pyrokinesis, ESP 0): the gem at (0, 2) (ESP 1), then the candle switch
        // at (2, 2) lights (lit 15 frames on) and changes its candle blocks over - the three lit ones
        // (+22 = 1) put away, the three out lit. Stage 56 (map 170): switches linked in pairs (1, 2)
        // - (3, 2) and (2, 1) - (2, 3): one lit lights the other (5 + 15 frames), with a beam; one put
        // out puts the other out; all four lit bring the crystal (its scene)
        {
            auto room = [&](int stageNo) {
                app.runner.stop();
                app.game = cr::GameState();
                app.mapNumber = -1;
                w.map = cr::kFirstPuzzleMap + stageNo;
                performWarp(app, w, -1);
                for (int quiet = 0, f = 0; f < 3000 && quiet < 10; ++f) {
                    stepEvents(app);
                    app.dialogue.clear();
                    if (app.choiceFlag >= 0) answerChoice(app, true);
                    app.tapPending = true;
                    quiet = app.runner.running() || app.pendingEntry != App::Entry::None ? 0 : quiet + 1;
                }
            };
            auto steps = [&](int n) { for (int f = 0; f < n; ++f) stepEvents(app); };
            room(48);
            setActor(app, 0, puzzle::centre(app, 0, 2), nullptr, true);
            steps(3);
            const int sw = puzzle::objectAt(app, 1, 2, 2);
            int litBlocks = 0, outBlocks = 0;
            for (const App::Puzzle::Obj& o : pz.objs)
                if (o.type == cr::kTypeCandle) (o.shown ? litBlocks : outBlocks) += 1;
            const bool start48 = pz.stage == 48 && pz.esp == 1 && sw >= 0 && pz.objs[size_t(sw)].type == cr::kTypeCandleSwitch &&
                                 litBlocks == 3 && outBlocks == 3;
            const int b01 = puzzle::objectAt(app, 1, 0, 1), b03 = puzzle::objectAt(app, 1, 0, 3);
            setActor(app, 0, puzzle::centre(app, 1, 1), nullptr, false);
            puzzle::pyroTap(app, sw);
            steps(2);
            const bool lighting = sw >= 0 && pz.objs[size_t(sw)].cs == 2 && pz.esp == 0 && app.game.vars[13] == 1;
            steps(16);
            const bool lit48 = sw >= 0 && pz.objs[size_t(sw)].cs == 3;
            const bool changed = b01 >= 0 && b03 >= 0 && !pz.objs[size_t(b01)].shown && pz.objs[size_t(b01)].ghost &&
                                 pz.objs[size_t(b03)].shown && !npcVisible(app, size_t(pz.objs[size_t(b01)].npc));
            steps(3); // (past the 20 frames after a tap in which none counts)
            puzzle::pyroTap(app, sw); // no ESP: refused
            steps(2);
            const bool refusedP = sw >= 0 && pz.objs[size_t(sw)].cs == 3 && pz.punished > 0;
            room(56);
            const int s12 = puzzle::objectAt(app, 1, 1, 2), s32 = puzzle::objectAt(app, 1, 3, 2), s21 = puzzle::objectAt(app, 1, 2, 1),
                      s23 = puzzle::objectAt(app, 1, 2, 3);
            auto csOf = [&](int t) { return t >= 0 ? pz.objs[size_t(t)].cs : -1; };
            const bool start56 = pz.stage == 56 && csOf(s12) == 0 && csOf(s32) == 0 && csOf(s21) == 0 && csOf(s23) == 0;
            setActor(app, 0, puzzle::centre(app, 0, 0), nullptr, false);
            puzzle::pyroTap(app, s12);
            steps(3);
            const bool beam = s32 >= 0 && pz.objs[size_t(s32)].csFrom == pz.objs[size_t(s12)].npc;
            auto shot = [&](const char* var) { // CR_PUZZLE_SHOT56A / B: a look at the stand-ins
                const char* path = SDL_getenv(var);
                if (!path) return;
                app.lastTicks = 0;
                drawFrame(app, false);
                int sw = 0, sh = 0;
                std::vector<uint8_t> px;
                if (!app.renderer.readPixels(px, sw, sh)) return;
                SDL_Surface* surf = SDL_CreateSurface(sw, sh, SDL_PIXELFORMAT_RGBA32);
                for (int y = 0; y < sh; ++y)
                    std::memcpy(static_cast<uint8_t*>(surf->pixels) + size_t(y) * surf->pitch, &px[size_t(y) * sw * 4], size_t(sw) * 4);
                SDL_SaveBMP(surf, path);
                SDL_DestroySurface(surf);
            };
            app.lastTicks = 0;
            drawFrame(app, false); // the beam and the first flame drawn
            const int fxErrs = app.renderer.takeErrors();
            shot("CR_PUZZLE_SHOT56A");
            steps(25);
            shot("CR_PUZZLE_SHOT56B");
            auto flames = [&] {
                int n = 0;
                for (const Effect& e : app.fieldEffects) n += e.id == 0xc && e.loop;
                return n;
            };
            const bool pair = csOf(s12) == 3 && csOf(s32) == 3 && csOf(s21) == 0 && flames() == 2;
            puzzle::pyroTap(app, s32); // out again, and its pair
            steps(40);
            const bool outPair = csOf(s12) == 0 && csOf(s32) == 0 && flames() == 0;
            const int tapsBefore = app.game.vars[13];
            puzzle::pyroTap(app, s12);
            puzzle::pyroTap(app, s21); // (inside the 20 frames: not counted)
            const bool cooled = app.game.vars[13] == tapsBefore + 1;
            steps(20);
            puzzle::pyroTap(app, s21);
            bool crystal56 = false;
            for (int quiet = 0, f = 0; f < 3000 && (quiet < 10 || f < 40); ++f) {
                stepEvents(app);
                app.dialogue.clear();
                if (app.choiceFlag >= 0) answerChoice(app, true);
                app.tapPending = true;
                quiet = app.runner.running() ? 0 : quiet + 1;
            }
            steps(3);
            for (const App::Puzzle::Obj& o : pz.objs) crystal56 |= o.type == cr::kTypeCrystal && o.shown;
            const bool allLit = puzzle::allCandlesLit(app);
            problems += !start48 || !lighting || !lit48 || !changed || !refusedP || !start56 || !beam || fxErrs || !pair || !outPair ||
                        !allLit || !crystal56 || !cooled;
            glErrors += fxErrs;
            std::printf("puzzle room: stage 48 (pyrokinesis) - %s, switch %s, candle blocks %s, at 0 ESP %s; stage 56 - %s, beam %s, "
                        "pair %s, out %s, all lit %s, crystal %s\n", start48 ? "start right" : "start WRONG",
                        lighting && lit48 ? "lights" : "DOESN'T light", changed ? "change over" : "DON'T change",
                        refusedP ? "refused" : "NOT refused", start56 ? "start right" : "start WRONG", beam ? "drawn" : "MISSING",
                        pair ? "lit" : "NOT lit", outPair ? "together" : "NOT together", allLit ? "yes" : "no",
                        crystal56 ? "came" : "DIDN'T come");
            app.runner.stop();
        }
        { // The touches as 2.2.0 reads them (CRO's 90a9652): psychokinesis' arrows and its swipe
          // decided while held, CheckTap and the windows, the pad lock, ScrollCamera, a Retry's entry
          // angle, cleared rooms (no objects) and the PK blocks' empty init
            int tp = 0;
            auto settle = [&](int most) {
                for (int quiet = 0, f = 0; f < most && quiet < 10; ++f) {
                    stepEvents(app);
                    app.dialogue.clear();
                    if (app.choiceFlag >= 0) answerChoice(app, true);
                    app.tapPending = true;
                    quiet = app.runner.running() || app.pendingEntry != App::Entry::None || pz.tpState ? 0 : quiet + 1;
                }
            };
            auto room = [&](int stageNo) {
                app.runner.stop();
                app.game = cr::GameState();
                app.mapNumber = -1;
                w.map = cr::kFirstPuzzleMap + stageNo;
                performWarp(app, w, -1);
            };
            auto steps = [&](int n) { for (int f = 0; f < n; ++f) stepEvents(app); };
            auto pointOf = [&](int t, float dy, float& x, float& y) { // an object's point (tex2d), or its top + dy
                const cr::FieldNpc& n = app.npcs[size_t(pz.objs[size_t(t)].npc)];
                float lx = 0, ly = 0;
                x = y = -1000;
                if (fieldToLogical(app, {n.pos[0], n.pos[1] + dy, n.pos[2]}, lx, ly)) logicalToTex2d(app, lx, ly, x, y);
            };
            // Stage 24 (psychokinesis): block A at (3, 3), the leader at (2, 2); slid left it stops at
            // (3, 2), against the block at (3, 1)
            room(24);
            settle(3000);
            setActor(app, 0, puzzle::centre(app, 2, 2), nullptr, false);
            steps(2);
            int pkInit = 0, pkBlocks = 0;
            for (const App::Puzzle::Obj& o : pz.objs)
                if (o.type == cr::kTypeNormal) ++pkBlocks, pkInit += o.shown == npcActive(app, app.npcs[size_t(o.npc)]) && !o.onSwitch;
            const int a = puzzle::objectAt(app, 1, 3, 3);
            float bx = 0, by = 0;
            if (a >= 0) pointOf(a, app.npcs[size_t(pz.objs[size_t(a)].npc)].box[1], bx, by);
            const bool window = a >= 0 && puzzle::pick(app, bx, by) == a && puzzle::pick(app, bx + 4, by + 6) == a &&
                                puzzle::inWindow(cr::kTypeNormal, 20, 21) && puzzle::inWindow(cr::kTypeNormal, -20, -16) &&
                                !puzzle::inWindow(cr::kTypeNormal, 21, 0) && !puzzle::inWindow(cr::kTypeNormal, 0, 22) &&
                                !puzzle::inWindow(cr::kTypeNormal, 0, -17);
            bool arrowsOn = false, arrowsAt = false, decided = false, flicker = false, letGo = false, fade5 = false, locked = false;
            if (a >= 0) {
                puzzle::pkGrab(app, a, bx, by);
                for (int f = 0; f < 10 && pz.objs[size_t(a)].pk != 2; ++f) steps(1);
                static const int kStep[4][2] = {{0, -1}, {-1, 0}, {0, 1}, {1, 0}};
                arrowsOn = true, arrowsAt = true;
                float ax = 0, ay = 0;
                const App::Puzzle::Obj& A = pz.objs[size_t(a)];
                pointOf(a, 5 - (app.npcs[size_t(A.npc)].pos[1] - (A.baseY + 5)), ax, ay); // (x, rise's y + 5, z)
                static const float kOff[4][2] = {{-36, 0}, {0, -24}, {36, 0}, {0, 24}};
                for (int k = 0; k < 4; ++k) {
                    const int r = 3 + kStep[k][0], c = 3 + kStep[k][1];
                    const bool want = puzzle::cellSlot(app, 1, r, c) && puzzle::objectAt(app, 1, r, c) < 0;
                    arrowsOn &= (pz.arrows[k].on == 1) == want;
                    if (want) arrowsAt &= std::fabs(pz.arrows[k].x - (ax + kOff[k][0])) < 1.5f && std::fabs(pz.arrows[k].y - (ay + kOff[k][1])) < 1.5f;
                }
                // still held, the touch 40 to the left: sent at once (onCheckSlide), the pad locked
                pz.pkAtX = bx - 40, pz.pkAtY = by;
                steps(1);
                decided = pz.pkState == 2 && pz.pkHeld && pz.objs[size_t(a)].toCol == 2;
                steps(1);
                for (int k = 1; k < 4; ++k) flicker |= pz.arrows[k].timer > 0;
                flicker = !flicker && pz.arrows[0].on == -1 && pz.arrows[0].timer == 10 && pz.arrows[0].mode == 2;
                stepPuzzleHud(app);
                flicker &= pz.arrows[0].alpha == 0xaf;
                for (int f = 0; f < 3; ++f) stepPuzzleHud(app);
                flicker &= pz.arrows[0].alpha == 0xbf; // (0xff - 4 x 0x50, the byte wrapped)
                for (int f = 0; f < 6; ++f) stepPuzzleHud(app);
                flicker &= pz.arrows[0].timer == 0; // (gone after its 10)
                locked = app.padLock == 10 && !puzzle::ask(app, 0);
                pz.pkHeld = false;
                for (int f = 0; f < 200 && pz.pkState; ++f) steps(1);
                locked &= app.padLock == 2;
                // taken hold of again and let go inside the dead zone: down where it was, nothing counted
                const int uses = app.game.vars[13], ar = pz.objs[size_t(a)].row, ac = pz.objs[size_t(a)].col;
                pointOf(a, app.npcs[size_t(pz.objs[size_t(a)].npc)].box[1], bx, by);
                puzzle::pkGrab(app, a, bx, by);
                const bool regrab = pz.pkState == 1;
                for (int f = 0; f < 10 && pz.objs[size_t(a)].pk != 2; ++f) steps(1);
                int shown = 0;
                for (const App::Puzzle::Arrow& w : pz.arrows) shown += w.on == 1;
                if (!regrab || !shown) std::printf("puzzle touches: the second hold at (%d, %d): taken %d, block %d, arrows %d\n", ar, ac, regrab, pz.objs[size_t(a)].pk, shown);
                pz.pkAtX = bx + 20, pz.pkAtY = by + 10;
                steps(1);
                pz.pkHeld = false;
                steps(1);
                bool anyOn = false;
                for (const App::Puzzle::Arrow& w : pz.arrows) anyOn |= w.timer == 5 && w.mode == 1;
                stepPuzzleHud(app);
                fade5 = anyOn;
                for (const App::Puzzle::Arrow& w : pz.arrows) fade5 &= w.timer == 0 || w.alpha == 0xcc;
                for (int f = 0; f < 60 && pz.pkState; ++f) steps(1);
                letGo = pz.pkState == 0 && app.game.vars[13] == uses && pz.objs[size_t(a)].row == ar && pz.objs[size_t(a)].col == ac;
            }
            tp += !window || !arrowsOn || !arrowsAt || !decided || !flicker || !locked || !letGo || !fade5 || pkInit != pkBlocks;
            std::printf("puzzle touches: psychokinesis - window %s, arrows %s at %s, swipe decided while held %s, the chosen arrow "
                        "flickers out %s, pad lock %s, a let-go fades them %s and costs nothing %s, blocks as the map has them %d/%d\n",
                        window ? "ok" : "WRONG", arrowsOn ? "on" : "WRONG", arrowsAt ? "their points" : "WRONG POINTS", decided ? "yes" : "NO",
                        flicker ? "yes" : "NO", locked ? "10 then 2" : "WRONG", fade5 ? "over 5" : "NO", letGo ? "yes" : "NO", pkInit, pkBlocks);
            // Stage 0: CheckTap - held 7 frames no tap; the leader moving holds it; let go in time, a
            // teleport (pad lock 12). (CR1's stage 0 starts at 0 ESP: three given)
            room(0);
            settle(3000);
            setActor(app, 0, puzzle::centre(app, 0, 2), nullptr, false);
            pz.esp = 3;
            steps(2);
            const int b = puzzle::objectAt(app, 1, 2, 1);
            bool longNo = false, movingWaits = false, tapped = false, gemWindow = true;
            if (b >= 0) {
                pointOf(b, app.npcs[size_t(pz.objs[size_t(b)].npc)].box[1], bx, by);
                auto touch = [&] { pz.tap = App::Puzzle::Tap(); pz.tap.state = 1, pz.tap.x = bx, pz.tap.y = by + 10; };
                touch();
                steps(7);
                pz.tap.released = true;
                steps(1);
                longNo = pz.tpState == 0 && pz.tap.state == 0;
                touch();
                app.walker.gait = cr::Gait::Walk;
                steps(3);
                movingWaits = pz.tap.frames == 0 && pz.tap.state == 1;
                app.walker.gait = cr::Gait::Stand;
                steps(2);
                pz.tap.released = true;
                steps(1);
                tapped = pz.tpState == 1 && app.padLock == 12;
                settle(3000);
            }
            for (size_t i = 0; i < pz.objs.size(); ++i) // a gem's window: 7..33 below its point, |dx| <= 17
                if (pz.objs[i].type == cr::kTypeGem && puzzle::target(app, pz.objs[i])) {
                    float gx, gy;
                    pointOf(int(i), app.npcs[size_t(pz.objs[i].npc)].box[1], gx, gy);
                    gemWindow &= puzzle::pick(app, gx, gy + 20) == int(i) && puzzle::pick(app, gx, gy + 6) < 0 &&
                                 puzzle::inWindow(cr::kTypeGem, 17, 7) && puzzle::inWindow(cr::kTypeGem, -17, 33) &&
                                 !puzzle::inWindow(cr::kTypeGem, 18, 20) && !puzzle::inWindow(cr::kTypeGem, 0, 34);
                }
            // the pad's buttons refused while the pad is locked (PuzzleManager::update's gac + 0x3cde6)
            app.padLock = 5;
            const bool lockRefuses = !puzzle::ask(app, 0) && !puzzle::ask(app, 1);
            app.padLock = 0;
            app.runner.stop();
            // ScrollCamera: one finger 5 frames, a second within 6 - the view goes to it, the ability waits
            app.fingers = App::Fingers();
            app.fingers.held = 1;
            steps(5);
            app.fingers.held = 2, app.fingers.fresh = 1;
            app.fingers.x = float(app.renderer.frameWidth()) / 4, app.fingers.y = float(app.renderer.frameHeight()) / 4;
            steps(1);
            const bool scrolled = pz.scroll.state == 1;
            pz.tap = App::Puzzle::Tap();
            pz.tap.state = 1;
            steps(2);
            const bool paused = pz.tap.frames == 0;
            app.fingers.held = 1;
            steps(12);
            const bool back = pz.scroll.state == 0;
            app.fingers = App::Fingers();
            pz.tap = App::Puzzle::Tap();
            // a Retry: the leader back where and as it came in, standing; the pad locked 24
            const cr::Vec3 start = app.puzzleStart;
            const float heading = app.puzzleStartHeading;
            setActor(app, 0, puzzle::centre(app, 4, 4), nullptr, false);
            app.walker.heading = heading + 1.5f, app.walker.gait = cr::Gait::Walk;
            puzzle::reset(app);
            const bool lock24 = app.padLock == 24;
            for (int f = 0; f < 40 && pz.scene != 4; ++f) steps(1);
            const bool retry = lock24 && pz.scene == 4 && std::fabs(app.walker.heading - heading) < 1e-4f &&
                               app.walker.gait == cr::Gait::Stand && std::fabs(app.walker.pos.x - start.x) < 0.01f &&
                               std::fabs(app.walker.pos.z - start.z) < 0.01f;
            // cleared rooms: no objects, no Retry / Help, the count 0 (MfMain_ClrPzlData)
            int clearedOk = 0, crystals = 0;
            for (int st = 0; st < cr::kPuzzleStages; ++st) {
                room(st);
                settle(3000);
                app.game.setFlag(3000 + st, true);
                app.npcs = app.events.npcs(app.mapNumber);
                puzzle::enter(app);
                clearedOk += pz.stage == st && pz.objs.empty() && !pz.help && pz.esp == 0 && !puzzle::ask(app, 0);
                int have = 0, shown = 0;
                for (size_t i = 0; i < app.npcs.size(); ++i)
                    if (app.npcs[i].puzzle & (cr::kPuzzleObject40 | cr::kPuzzleObject100 | cr::kPuzzleObject800)) {
                        ++have;
                        shown += npcVisible(app, i);
                    }
                crystals += shown > 0;
                clearedOk -= have && !shown; // (the way on: a crystal the map shows)
                if (have && !shown) std::printf("puzzle touches: cleared stage %d - %d crystal NPCs, none shown by the map\n", st, have);
            }
            tp += !longNo || !movingWaits || !tapped || !gemWindow || !lockRefuses || !scrolled || !paused || !back || !retry || clearedOk != cr::kPuzzleStages;
            std::printf("puzzle touches: CheckTap - held 7 frames %s, the leader moving %s, a tap %s; gem window %s; pad lock %s; "
                        "ScrollCamera %s, the ability %s, %s; Retry %s; cleared rooms bare %d/%d (a crystal shown in %d); %d problems\n",
                        longNo ? "no tap" : "TAPPED", movingWaits ? "waits" : "DOESN'T WAIT", tapped ? "teleports (pad lock 12)" : "DOESN'T",
                        gemWindow ? "ok" : "WRONG", lockRefuses ? "refuses Retry / Help" : "WRONG", scrolled ? "scrolls" : "DOESN'T SCROLL", paused ? "paused" : "NOT PAUSED",
                        back ? "let go" : "STUCK", retry ? "back as it came in" : "WRONG", clearedOk, cr::kPuzzleStages, crystals, tp);
            problems += tp;
        }
        std::printf("puzzle room: stage 0 - %d blocks, %d gems, swap %s, ESP %d after the gems, crystal scene %s (flag 3300 %d, "
                    "3000 %d, crystal %s); %d problems\n", blocks, gems, refused ? (swapped ? "refused at 0 ESP, then ok" : "FAILED") : "NOT refused at 0 ESP", espAfterGems,
                    sceneRan ? "ran" : "didn't run", roomFlag, clearFlag, crystal ? "shown" : "hidden", problems);
        mapErrors += problems;
        glErrors += errs;
        app.runner.stop();
        app.game = cr::GameState();
        app.puzzle = App::Puzzle();
    }
    { // The field camera: the doubled view follows the player within the painting, the scripts pan it
        int problems = 0;
        const int keepPaintings = app.video.paintings;
        app.video.paintings = 0;
        app.runner.stop();
        app.game = cr::GameState();
        app.mapNumber = -1;
        cr::WarpRequest w;
        w.valid = true;
        w.area = 2;
        w.map = 0; // d0101, the first gate's first field
        performWarp(app, w, -1);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        app.cam = App::ScriptCam();
        const float SW = app.renderer.screenW(), H = cr::Renderer::kLogicalH;
        auto view = [&](float& x, float& y) { // the view's top-left on screen
            x = (SW - fieldViewW(app)) / 2 + app.scroll.offX;
            y = (H - fieldViewH(app)) / 2 - app.scroll.offY;
        };
        auto inside = [&] { // the screen stays inside the doubled view
            float x, y;
            view(x, y);
            return x <= 0.5f && y <= 0.5f && x + fieldViewW(app) >= SW - 0.5f && y + fieldViewH(app) >= H - 0.5f;
        };
        problems += !scrollingField(app) || fieldViewW(app) != 2048 || fieldViewH(app) != 2048;
        int followed = 0;
        for (const cr::Vec3 p : {cr::Vec3{120.4f, 20.0f, 69.5f}, cr::Vec3{100, 20, 60}, cr::Vec3{80, 20, 50}}) {
            app.walker.pos = p;
            followPlayerScroll(app);
            float hx, hy, x, y;
            fieldViewPoint(app, p + cr::Vec3{0, 7, 0}, hx, hy);
            view(x, y);
            const bool onScreen = x + hx >= 0 && x + hx <= SW && y + hy >= 0 && y + hy <= H;
            followed += onScreen && inside();
        }
        problems += followed != 3;
        // CameraFree: FrmPanCam moves twice its deltas over its frames, kept in the painting (the
        // camera's way - the painting the other way)
        ViewerHost host(app);
        host.freeCamera(true);
        const float startY = app.scroll.offY;
        const float pan[3] = {0, -100, 0};
        host.panCamera(pan, 10); // (its first step is the pan's own frame: ScrollAutoCtl runs after PrcEvnt)
        for (int f = 0; f < 4; ++f) stepFieldScroll(app);
        const float halfway = app.scroll.offY;
        for (int f = 0; f < 10; ++f) stepFieldScroll(app);
        const float wantY = std::clamp(startY + 200, -(fieldViewH(app) - H) / 2, (fieldViewH(app) - H) / 2);
        const float panned = app.scroll.offY;
        problems += std::fabs(halfway - (startY + wantY) / 2) > 1.0f || std::fabs(panned - wantY) > 0.5f ||
                    app.scroll.sliding || !inside();
        // FrmAdjCam: slides to a shot's (x, y); FrmSetCam: back to the centre; Mapscroll: centre on an NPC (the player)
        const float shot[3] = {-200, 150, 0};
        host.setCamera(shot, shot, 4);
        for (int f = 0; f < 4; ++f) stepFieldScroll(app);
        problems += std::fabs(app.scroll.offX + 200) > 0.5f || std::fabs(app.scroll.offY - 150) > 0.5f;
        host.setCamera(shot, shot, 0);
        problems += app.scroll.offX != 0 || app.scroll.offY != 0;
        host.mapScroll(0, 0, 0, 1.0f);
        float hx, hy, x, y;
        fieldViewPoint(app, app.walker.pos, hx, hy);
        view(x, y);
        problems += !inside() || x + hx < 0 || x + hx > SW;
        // CameraPlayer: following again
        host.freeCamera(false);
        followPlayerScroll(app);
        problems += !inside();
        app.lastTicks = 0;
        drawFrame(app, false);
        ++frames;
        std::printf("field camera: d0101's view %gx%g follows the player (%d of 3 spots), pans %g -> %g, shots and "
                    "Mapscroll; %d problems\n",
                    fieldViewW(app), fieldViewH(app), followed, startY, panned, problems);
        // Scene effects: the first gate's entry script loads set 0, turns everything off and starts the snow (5);
        // d0117's fog (3) is another room's
        int fxProblems = 0;
        endSceneEffects(app);
        app.runner.start(app.events, 0, app.game, app.area, app.mapNumber);
        for (int f = 0; f < 3000 && app.runner.running(); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        int loaded = 0;
        for (const cr::Texture& t : app.fx.tex) loaded += t.id != 0;
        fxProblems += app.fx.set != 0 || loaded != 6 || app.fx.on[3] || !app.fx.on[5]; // 3 and their "(128)" copies
        setSceneEffect(app, true, 3);
        int lo = 255, hi = 0;
        for (int f = 0; f < 600; ++f) {
            stepSceneEffects(app);
            lo = std::min(lo, app.fx.ch[0].alpha);
            hi = std::max(hi, app.fx.ch[0].alpha);
            fxProblems += app.fx.timerA[5] < 0 || app.fx.timerA[5] > 79 || app.fx.timerA[3] > 400 || app.fx.timerB[3] > 440;
        }
        fxProblems += lo != 55 || hi != 255 || app.fx.ch[2].alpha != 135; // UpdateRandom: 55 <-> 255; channel 2 stays
        app.lastTicks = 0;
        drawFrame(app, false);
        ++frames;
        setSceneEffect(app, false, 5);
        fxProblems += app.fx.on[5];
        std::printf("scene effects: set %d (%d textures), snow on, fog added, snow's fog alpha %d..%d; %d problems\n", app.fx.set,
                    loaded, lo, hi, fxProblems);
        problems += fxProblems;
        endSceneEffects(app);
        app.video.paintings = keepPaintings;
        mapErrors += problems;
        bootTitle(app);
        app.runner.stop();
    }
    { // The puzzle tutorial (Uty_TutoTexDisp): entering stage 0 from a dungeon, its pages turned as a
      // player would (advanceDialogue): the picture's pages follow; the frames step
        int problems = 0, maxPage = -1, framesSeen = 0, calls0 = app.tutoCalls;
        bool shot = false;
        app.runner.stop();
        app.game = cr::GameState();
        app.mapNumber = -1;
        app.game.setFlag(3070, true);
        cr::WarpRequest w;
        w.valid = true, w.area = cr::kPuzzleArea, w.map = cr::kFirstPuzzleMap;
        performWarp(app, w, -1);
        for (int f = 0; f < 4000 && (app.runner.running() || app.pendingEntry != App::Entry::None || app.tutoTex.on); ++f) {
            stepEvents(app);
            if (app.tutoTex.on) {
                maxPage = std::max(maxPage, app.tutoTex.page);
                framesSeen = std::max(framesSeen, app.tutoTex.frame + 1);
                if (!shot && app.tutoTex.page >= 0 && app.tutoTex.frame >= 1) {
                    shot = true;
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    if (const char* path = SDL_getenv("CR_TUTO_SHOT")) saveFrame(app, path);
                }
            }
            if (f % 60 == 59 && !app.dialogue.empty()) advanceDialogue(app); // a page read, then turned
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        const bool ran = app.tutoCalls > calls0;
        problems += !ran || maxPage < 1 || framesSeen < 2 || app.tutoTex.on || app.renderer.takeErrors();
        std::printf("puzzle tutorial: %s, pages shown up to %d, frames stepped %d, ended %s; %d problems\n",
                    ran ? "started" : "NOT started", maxPage, framesSeen, app.tutoTex.on ? "NO" : "yes", problems);
        mapErrors += problems;
        app.runner.stop();
        app.game = cr::GameState();
        app.puzzle = App::Puzzle();
    }
    { // Music: the AAC decoder (the system's FAAD2 or the bundled copy) decodes a whole track
        int64_t frames = 0;
        uint64_t hash = 0;
        std::string err;
#ifdef CR_HAVE_FAAD2
        if (app.vitaOnly || app.androidOnly) std::printf("music: the %s's (no AAC tracks)\n", app.vitaOnly ? "Vita" : "Android release");
        else {
        const bool ok = cr::decodeTrack(app.dataDir, 10, frames, hash, &err) && frames > 1000000;
        mapErrors += !ok;
        std::printf("music: track 10 decoded %s (%lld frames)\n", ok ? "ok" : ("FAILED: " + err).c_str(), (long long)frames);
        }
#else
        std::printf("music: built without an AAC decoder\n");
#endif
    }
    if (app.android.isOpen()) { // --android: its j001mNN.mca for bgmNN (Ogg Vorbis, looping at its own sample)
        int problems = 0, decoded = 0, loops = 0;
        double minutes = 0;
        const auto read = [&app](const std::string& n, std::vector<uint8_t>& out) { return app.android.read(n, out); };
        for (int t = 0; t < cr::kBgmTracks; ++t) {
            int64_t samples = 0;
            bool loopOk = false;
            if (cr::checkAndroidTrack(read, t, samples, loopOk)) ++decoded, loops += loopOk, minutes += double(samples) / 44100 / 60;
        }
        problems += decoded < cr::kBgmTracks - 1 || loops != decoded; // (CR1's: no j001m31)
        // the layout's message strip (window_cutin_<w>) by the width of a 720-tall screen; each in the data
        int strips = 0;
        for (const int w : {480, 800, 854, 960, 1024, 1280, 1366}) strips += app.android.contains("window_cutin_" + std::to_string(w) + ".mct");
        problems += strips != 7 || androidCutinWidth(1080) != 1280 || androidCutinWidth(1280.0f * 0.9999f) != 1280 ||
                    androidCutinWidth(1281) != 1366 || androidCutinWidth(1680) != 0;
        std::printf("android music: %d of %d tracks decode (%.1f minutes), %d loop points inside them; %d problems\n", decoded,
                    cr::kBgmTracks, minutes, loops, problems);
        mapErrors += problems;
    }
    if (app.vita.isOpen()) { // --vita: the Vita's scripts (eight languages), voices (sorted names = 2.2.0's
                             // clips) and music (cr_mNN.at9 for bgmNN, looping between its smpl points)
        int problems = 0;
        int languages = 0;
        for (int l = 0; l < cr::kLanguages; ++l) languages += app.systemText.hasLanguage(l);
        problems += languages != cr::kLanguages;
        if (app.sound.voiceClips(false) == 0 && app.vitaVoice[0].isOpen()) startSound(app), vitaAudio(app); // (tests run before it)
        const bool voices = app.sound.vitaVoices() && app.sound.voiceClips(false) == 2900 && app.sound.voiceClips(true) == 1220;
        problems += !voices;
        int decoded = 0, rate = 0, ch = 0;
        std::vector<uint8_t> f;
        std::vector<int16_t> pcm;
        for (int b = 0; b < 2; ++b) {
            const std::vector<std::string> names = app.vitaVoice[b].names();
            if (names.size() > 1 && app.vitaVoice[b].read(names[1], f) && cr::decodeAt9(f, pcm, rate, ch) && rate == 48000 && ch == 1 && pcm.size() > 4800)
                ++decoded;
        }
        problems += decoded != 2;
        cr::At9File track;
        int64_t frames = 0;
        const bool music = app.vitaBgm.read("cr_m01.at9", f) && cr::parseAt9(f, track) && track.loopStart > 0 &&
                           track.loopEnd > track.loopStart && cr::decodeAt9(f, pcm, rate, ch) && (frames = int64_t(pcm.size()) / std::max(ch, 1)) == track.samples;
        problems += !music;
        std::printf("vita assets: %d languages, voices %s (%d / 2 clips decoded), track 1 %s (%lld frames, loop %.3f..%.3f s); %d problems\n",
                    languages, voices ? "yes" : "NO", decoded, music ? "decoded" : "NOT decoded", (long long)frames,
                    double(track.loopStart) / 48000, double(track.loopEnd) / 48000, problems);
        mapErrors += problems;
    }
    { // The scripts' small natives: 2018 the pair's HP / MP, 2024 / 2026 the music by var, 2025 the shop
      // ended without its music put back, 2027 the pair, 2028 the loading spinner; the shop's music
        int problems = 0;
        ViewerHost host(app);
        const cr::GameState keepGame = app.game;
        const int keepChar = app.playerChar;
        app.playerChar = 2; // the second pair
        for (auto& m : app.game.party) m.hp = 1, m.mp = 1;
        host.recoverPair();
        const bool healed = app.game.party[2].hp < 0 && app.game.party[3].mp < 0 && app.game.party[0].hp == 1;
        const bool pair = host.pairNo() == 1;
        host.playMusic(7);
        const bool bgm = host.bgmNo() == 7;
        host.playMusic(1u << 27 | 7); // a fade-out isn't a track
        const bool fadeKept = host.bgmNo() == 7;
        app.game.vars[0] = 7;
        openShop(app);
        const bool shopTune = host.bgmNo() == 0x13 && app.shop.bgmBack == 7;
        closeShop(app);
        const bool back = host.bgmNo() == 7;
        openShop(app);
        host.endMenu(2);
        const bool ended = !app.shop.open && host.bgmNo() == 0x13; // SetShopBgmNoRecovery
        // 2030 / 2031: queued outside a battle, the next battle's fighter 1 / 0 starts with the four ups
        host.battleParamUp(1);
        const bool queued = app.paramUp[1] && !app.paramUp[0];
        app.game = keepGame;
        startBattle(app, {1, 0, -1, 0}, 0);
        bool boosted = false;
        if (app.battle.active) {
            const cr::Fighter& f1 = app.battle.rules.f[1];
            boosted = f1.status[cr::kStatusAttackUp] == 6 && f1.status[cr::kStatusDefenseUp] == 6 &&
                      f1.status[cr::kStatusSpeedUp] == 6 && f1.status[cr::kStatusLuckUp] == 6 &&
                      app.battle.rules.f[0].status[cr::kStatusAttackUp] == 0 && !app.paramUp[1];
            host.battleParamUp(0); // in a battle: at once
            boosted &= app.battle.rules.f[0].status[cr::kStatusSpeedUp] == 6;
            endBattle(app, false);
        }
        problems += !queued || !boosted;
        // 2029 / 2008: the logos once a run; the pad's picture while flag 0 is set
        app.logoSeen = false;
        host.setLogoSeen(); // (var 0 set: the logos shown this run)
        const bool logoOnce = host.logoSeen();
        app.game.setFlag(0, true);
        host.padArt();
        app.lastTicks = 0;
        drawFrame(app, false);
        if (const char* path = SDL_getenv("CR_PAD_SHOT")) saveFrame(app, path);
        app.game.setFlag(0, false);
        drawFrame(app, false);
        const bool padGone = !app.padArt;
        problems += !logoOnce || !padGone || app.renderer.takeErrors();
        std::printf("natives: battle stat ups (2030 / 2031) %s\n", queued && boosted ? "ok" : "WRONG");
        app.runner.stop(); // (a script of the run before might turn it off: flag 4086 clear)
        host.nowLoading(true);
        const int frame0 = app.nowLoading.frame;
        for (int f = 0; f < 3; ++f) stepEvents(app);
        const bool spin = app.nowLoading.on && app.nowLoading.frame == ((frame0 + 3) & 15);
        app.lastTicks = 0;
        drawFrame(app, false);
        host.nowLoading(false);
        problems += !healed || !pair || !bgm || !fadeKept || !shopTune || !back || !ended || !spin || app.renderer.takeErrors();
        std::printf("natives: pair recovery %s, pair number %s, music by number %s, shop music %s / put back %s / kept by EndMenu %s, "
                    "loading spinner %s; %d problems\n", healed ? "ok" : "WRONG", pair ? "ok" : "WRONG", bgm && fadeKept ? "ok" : "WRONG",
                    shopTune ? "on" : "OFF", back ? "yes" : "NO", ended ? "yes" : "NO", spin ? "turns" : "STILL", problems);
        mapErrors += problems;
        app.game = keepGame;
        app.playerChar = keepChar;
        playBgm(app, 1u << 29);
    }
    { // The staff roll (Util 7), fast: the credits run out, the picture reaches its bottom, the panel
      // slides away, the end card waits for a tap, then it all closes
        int problems = 0;
        openStaffRoll(app, 0);
        App::StaffRoll& st = app.staffRoll;
        int lines = 0;
        for (int e = 0; e < staffLines(app); ++e) lines += staffLine(app, e).size() >= 3;
        const bool tex = st.tex.id != 0;
        st.fast = true;
        int frames = 0, shot = 0;
        while (st.open && st.state != 7 && frames < 3000) {
            st.fast = true; // (drawFrame reads the keys)
            stepStaffRoll(app);
            ++frames;
            if (frames == 150) { // a look mid-way
                app.lastTicks = 0;
                drawFrame(app, false);
                const char* path = SDL_getenv(shot == 0 ? "CR_STAFF_SHOT_A" : "CR_STAFF_SHOT_B");
                if (path && shot < 2) saveFrame(app, path);
                ++shot;
            }
        }
        for (int f = 0; f < 4; ++f) stepStaffRoll(app); // the end card faded in, waiting
        if (const char* path = SDL_getenv("CR_STAFF_SHOT_B")) {
            app.lastTicks = 0;
            drawFrame(app, false);
            saveFrame(app, path);
        }
        const bool ended = st.state == 7 && st.picY == staffPicEnd(st) && st.panel < staffroll::kPanelEnd && st.next > staffLines(app) &&
                           st.win[4] == 1.0f;
        staffRollTap(app);
        for (int f = 0; f < 20 && st.open; ++f) stepStaffRoll(app);
        problems += !tex || lines < 100 || !ended || st.open || app.renderer.takeErrors();
        std::printf("staff roll: picture %s, %d of block 9's %d lines with text, end card after %d fast frames %s, closed %s; %d problems\n",
                    tex ? "read" : "MISSING", lines, staffLines(app), frames, ended ? "reached" : "NOT reached",
                    st.open ? "NO" : "yes", problems);
        mapErrors += problems;
    }
    mapErrors += menusAuditSelfTest(app);
    { // The last battle's pair choice (Util 8): four rounds, each pair once; a No goes back to the
      // plates; the camp menu opens for the chosen pair; flag 1 clears after the fourth
        int problems = 0;
        const cr::GameState keepGame = app.game;
        const int keepChar = app.playerChar;
        app.game.setFlag(1, true);
        App::LastBattle& lb = app.lastBattle;
        lb.count = lb.mask = 0;
        auto stepTo = [&](int state) { for (int f = 0; f < 60 && lb.open && lb.state != state; ++f) stepLastBattle(app); };
        auto look = [&](const char* var) {
            if (const char* path = SDL_getenv(var)) {
                for (int f = 0; f < 4; ++f) stepLastBattle(app);
                app.lastTicks = 0;
                drawFrame(app, false);
                saveFrame(app, path);
            }
        };
        std::vector<int> order;
        bool campFor = true, backOk = true, texOk = true, healed = true;
        for (int round = 0; round < 4; ++round) {
            app.game.party[3].hp = 1;
            openLastBattle(app);
            texOk &= lb.tex.id != 0;
            healed &= app.game.party[3].hp < 0;
            stepTo(3);
            if (round == 0) look("CR_LASTBT_SHOT_A");
            const int left = int(lb.pairs.size());
            backOk &= left == 4 - round;
            lastBattleChoose(app, left - 1); // the last one listed
            stepTo(7);
            campFor &= app.campOpen && app.playerChar == 2 * lb.chosen;
            app.campOpen = false;
            stepTo(10);
            if (round == 0) {
                look("CR_LASTBT_SHOT_B");
                lastBattleAnswer(app, false); // No: the plates again, the same pairs
                stepTo(3);
                backOk &= lb.state == 3 && int(lb.pairs.size()) == left;
                lastBattleChoose(app, left - 1);
                stepTo(7);
                app.campOpen = false;
                stepTo(10);
            }
            const int chosen = lb.chosen;
            lastBattleAnswer(app, true);
            stepTo(-1);
            order.push_back(chosen);
            problems += lb.open || app.game.vars[0] != chosen;
        }
        bool leaders = true; // the plates name the leaders, not the partners (entry 0x1e + 2p)
        for (int p = 0; p < 4; ++p)
            leaders &= !lastBattlePlate(app, p).empty() && lastBattlePlate(app, p) != sysText(app, 58030 + 2 * p);
        problems += !leaders;
        const bool reset = lb.count == 0 && lb.mask == 0 && !app.game.flag(1);
        std::sort(order.begin(), order.end());
        problems += !texOk || !healed || !campFor || !backOk || !reset || order != std::vector<int>{0, 1, 2, 3} || app.renderer.takeErrors();
        std::printf("last battle: 4 rounds, each pair once %s, its camp menu %s, No back to the plates %s, HP / MP restored %s, "
                    "flag 1 and the rounds clear %s, plates name the leaders %s; %d problems\n", order == std::vector<int>{0, 1, 2, 3} ? "yes" : "NO",
                    campFor ? "opens" : "DOESN'T open", backOk ? "yes" : "NO", healed ? "yes" : "NO", reset ? "yes" : "NO", leaders ? "yes" : "NO", problems);
        mapErrors += problems;
        app.game = keepGame;
        app.playerChar = keepChar;
    }
    { // The Break changing hands (btlord_chk -1, btlbreak_effect_*): the effect plays 41 frames,
      // the battle waiting; half way the pair takes the Break at 4
        int problems = 0;
        const cr::GameState keepGame = app.game;
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        startBattle(app, {1, 0, -1, 0}, 0);
        App::Battle& b = app.battle;
        int frames = 0, gaugeAt = -1;
        bool waited = true;
        if (b.active) {
            for (int f = 0; f < 60 && b.phase != App::Battle::Command; ++f) stepBattle(app);
            b.phase = App::Battle::Acting, b.turnOn = false;
            b.rules.breakStarted = true;
            b.rules.breakGauge = b.rules.breakTarget = 0;
            b.rules.breakFlags |= 0x400 | 0x800; // the enemies lost it
            startBreakFx(app);
            for (; frames < 200 && b.breakFx.on; ++frames) {
                const int round = b.rules.round;
                stepBattle(app);
                waited &= !b.turnOn && b.rules.round == round;
                if (gaugeAt < 0 && b.rules.breakGauge == 4) gaugeAt = frames;
                if (frames == 20 && SDL_getenv("CR_BREAK_SHOT")) {
                    app.lastTicks = 0;
                    drawFrame(app, false);
                    saveFrame(app, SDL_getenv("CR_BREAK_SHOT"));
                }
            }
            endBattle(app, false);
        }
        problems += frames != 41 || gaugeAt != 19 || !waited || b.rules.breakChanging();
        std::printf("break effect: %d frames, the pair holds 4 from frame %d, the battle waited %s; %d problems\n", frames,
                    gaugeAt, waited ? "yes" : "NO", problems);
        mapErrors += problems;
        app.runner.stop();
        app.game = keepGame;
    }
    { // The iPhone layout (2.2.0's screens): a 3:2 screen, the layout's safe area filling it, taps
      // mapped back; the field menu's corner button where 2.2.0 has it, and the converted screens
      // drawing without render API errors
        int problems = 0;
        const cr::GameState keepGame = app.game;
        app.video.layout = 2;
        applyVideo(app);
        cr::Renderer& r = app.renderer;
        drawFrame(app, false); // (the frame maps the 3:2 screen)
        float x0 = 32, y0 = 64, x1 = 992 + r.uiX(), y1 = 704;
        x0 += r.uiX();
        r.layoutToScreen(x0, y0);
        r.layoutToScreen(x1, y1);
        const bool frame = r.phoneLayout() && std::fabs(r.screenW() / cr::Renderer::kLogicalH - 1.5f) < 0.01f;
        const bool fills = std::fabs(x0) < 0.5f && std::fabs(y0) < 0.5f && std::fabs(x1 - r.screenW()) < 0.5f &&
                           std::fabs(y1 - cr::Renderer::kLogicalH) < 0.5f;
        float bx = 500, by = 300;
        r.layoutToScreen(bx, by);
        r.screenToLayout(bx, by);
        const bool inverse = std::fabs(bx - 500) < 0.01f && std::fabs(by - 300) < 0.01f;
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        int errs = 0;
        // the corner button: 2.2.0's box (440, 0, 40, 40) - tex2d -> layout -> screen -> pixels
        float cx = 2.0f * (440 + 20) + 32 + r.uiX(), cy = 2.0f * 20 + 64;
        r.layoutToScreen(cx, cy);
        const float kx = float(r.frameWidth()) / r.screenW();
        app.fieldMenu = App::FieldMenu();
        app.fieldMenu.fade = 320;
        const float ox = (float(r.frameWidth()) - r.screenW() * std::min(kx, float(r.frameHeight()) / cr::Renderer::kLogicalH)) / 2;
        const float sc = std::min(kx, float(r.frameHeight()) / cr::Renderer::kLogicalH);
        const float oy = (float(r.frameHeight()) - cr::Renderer::kLogicalH * sc) / 2;
        const bool corner = fieldMenuClick(app, ox + cx * sc, oy + cy * sc) && app.fieldMenu.open;
        app.fieldMenu = App::FieldMenu();
        openCamp(app);
        const int entries = campEntries(app);
        app.lastTicks = 0;
        drawFrame(app, false);
        errs += r.takeErrors();
        app.campOpen = false;
        startBattle(app, {1, 0, -1, 0}, 0);
        for (int f = 0; f < 40 && app.battle.active; ++f) stepBattle(app);
        app.lastTicks = 0;
        drawFrame(app, false);
        errs += r.takeErrors();
        if (app.battle.active) endBattle(app, false);
        showMessages(app, {57006});
        drawFrame(app, false);
        errs += r.takeErrors();
        app.dialogue.clear();
        openScenarios(app); // 2.2.0's chart scrolls sideways too, to -120, and down to its own end
        for (int k = 0; k < 5; ++k) scenariosKey(app, SDLK_RIGHT);
        for (int k = 0; k < 8; ++k) scenariosKey(app, SDLK_PAGEDOWN);
        const bool chart = app.scenariosHScroll == -120 && app.scenariosScroll == 2 * kPhoneHistoryLowest;
        drawFrame(app, false);
        errs += r.takeErrors();
        closeScenarios(app);
        problems += !frame || !fills || !inverse || !corner || entries != 5 || !chart || errs;
        std::printf("iphone layout: 3:2 screen %s, the safe area fills it %s, taps map back %s, the corner button %s, "
                    "camp %d plates, History scrolls both ways %s, %d render API errors; %d problems\n", frame ? "yes" : "NO",
                    fills ? "yes" : "NO", inverse ? "yes" : "NO", corner ? "opens" : "DOESN'T open", entries,
                    chart ? "yes" : "NO", errs, problems);
        mapErrors += problems;
        app.video.layout = 1;
        applyVideo(app);
        app.runner.stop();
        app.game = keepGame;
    }
    { // Widescreen (16:9): the screens' left- and right-hand groups at the screen's edges, taps
      // following them - the field's corner button, the Camp's plates, the title's, the save
      // screen's - and full-screen pictures covering the whole width
        int problems = 0;
        const cr::GameState keepGame = app.game;
        const App::Video keepVideo = app.video;
        app.video.widescreen = true, app.video.layout = 1, app.video.display = 0;
        app.video.width = 1280, app.video.height = 720;
        applyVideo(app);
        if (app.window) SDL_SyncWindow(app.window);
        cr::Renderer& r = app.renderer;
        app.lastTicks = 0;
        drawFrame(app, false);
        const float ux = r.uiX();
        float l0x, l0y, l1x, l1y; // logical <-> the frame's pixels (straight lines off the iPhone layout)
        r.toLogical(0, 0, l0x, l0y);
        r.toLogical(1000, 1000, l1x, l1y);
        auto pixel = [&](float lx, float ly, float& px, float& py) {
            px = (lx - l0x) * 1000 / (l1x - l0x), py = (ly - l0y) * 1000 / (l1y - l0y);
        };
        auto tap = [&](float origin, float mx, float my, float& px, float& py) { pixel(origin + 2 * mx, 2 * my, px, py); };
        float px, py;
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        app.fieldMenu = App::FieldMenu();
        app.fieldMenu.fade = 320;
        tap(wideRight(app), 456 + 20 + 16, -32 + 20 + 32, px, py); // the corner button's box (tex2d + (16, 32))
        const bool corner = fieldMenuClick(app, px, py) && app.fieldMenu.open;
        app.fieldMenu = App::FieldMenu();
        app.game.setFlag(0x38, true), app.game.setFlag(0x39, true); // (the Camp's once-only messages read)
        openCamp(app);
        app.camp.anim = 100;
        tap(wideRight(app), campui::kPlateX + 60, campui::kPlateY + 20, px, py); // Items
        campClick(app, px, py);
        const bool camp = app.camp.page == App::Camp::Items;
        app.lastTicks = 0;
        drawFrame(app, false);
        int errs = r.takeErrors();
        app.campOpen = false;
        openSaveScreen(app, false);
        tap(wideRight(app), 328 + 60, 136 + 48.0f * float(savePlates(app).empty() ? 0 : savePlates(app)[0]) + 20, px, py);
        saveScreenClick(app, px, py);
        const bool save = app.saveScreen.phase == 1; // asking "Save data?"
        drawFrame(app, false);
        errs += r.takeErrors();
        app.saveScreen.open = false;
        app.titleOpen = true;
        app.titleStage = 1, app.titleAnim = 10; // (past the title's tap, the plates in)
        app.titleCursor = 2;
        tap(wideRight(app), 328 + 60, 114 + 20, px, py); // Start's plate
        const int before = app.titleCursor;
        titleClick(app, px, py);
        const bool title = app.titleCursor == 0 && before == 2;
        app.titleOpen = false;
        app.saveScreen.open = false; // (Start went on to the save / load screen)
        if (app.charSelectOpen) closeCharSelect(app, 0);
        app.optionsOpen = false;
        problems += ux <= 0 || !corner || !camp || !save || !title || errs;
        std::printf("widescreen: the screen %.0f wide, the corner button %s, Camp's Items %s, the save plate %s, the title's Start %s, "
                    "%d render API errors; %d problems\n", r.screenW(), corner ? "opens" : "DOESN'T open", camp ? "yes" : "NO",
                    save ? "yes" : "NO", title ? "yes" : "NO", errs, problems);
        mapErrors += problems;
        app.video = keepVideo;
        applyVideo(app);
        if (app.window) SDL_SyncWindow(app.window);
        app.runner.stop();
        app.game = keepGame;
    }
    { // A controller, as the Vita's buttons: a virtual one - the stick walks the leader, △ opens the
      // Camp and closes it, ○ confirms, × goes back (and does nothing on the field), the D-pad
      // moves the cursor; ×-confirm swaps the two
        int problems = 0;
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = 6, desc.nbuttons = 15;
        desc.axis_mask = (1u << 6) - 1, desc.button_mask = (1u << 15) - 1; // (the gamepad's own order)
        desc.name = "selftest pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        SDL_Gamepad* pad = id ? SDL_OpenGamepad(id) : nullptr;
        SDL_Joystick* js = pad ? SDL_GetGamepadJoystick(pad) : nullptr;
        if (!pad) {
            std::printf("controller: no virtual controller here (%s), skipped\n", SDL_GetError());
        } else {
            const cr::GameState keepGame = app.game;
            SDL_Gamepad* keepPad = app.pad;
            app.pad = pad;
            padLive(app); // (a frame with nothing held)
            app.game = cr::GameState();
            cr::giveStartingItems(app.game);
            app.game.setFlag(0x38, true), app.game.setFlag(0x39, true); // (the Camp's once-only messages read)
            showMap(app, 0, 0, 2);
            app.pendingEntry = App::Entry::None;
            app.runner.stop();
            app.dialogue.clear();
            const cr::Vec3 from = app.walker.pos;
            SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 32767);
            SDL_UpdateJoysticks();
            for (int f = 0; f < 15; ++f) updatePlayer(app, 1.0f / kFramesPerSecond, 0);
            SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_UpdateJoysticks();
            const cr::Vec3 d = app.walker.pos - from;
            const bool walked = std::sqrt(cr::dot(d, d)) > 1.0f;
            app.walker.pos = from;
            app.runner.stop(); // (anything the walk set off)
            app.dialogue.clear();
            app.pendingEntry = App::Entry::None;
            padButton(app, SDL_GAMEPAD_BUTTON_SOUTH); // × on the field: nothing
            const bool fieldQuiet = !app.campOpen && app.havePlayer;
            padButton(app, SDL_GAMEPAD_BUTTON_NORTH); // △: the Camp
            const bool opened = app.campOpen;

            const int c0 = app.camp.cursor;
            padButton(app, SDL_GAMEPAD_BUTTON_DPAD_UP);
            const bool moved = app.camp.cursor != c0;
            app.camp.cursor = 0;
            padButton(app, SDL_GAMEPAD_BUTTON_EAST); // ○: Items
            const bool confirmed = app.camp.page == App::Camp::Items;
            padButton(app, SDL_GAMEPAD_BUTTON_SOUTH); // ×: back to the plates
            for (int f = 0; f < 8; ++f) stepCamp(app); // (the page's windows go, then the main page's come)
            const bool backed = app.camp.page == App::Camp::Main && app.campOpen;
            bool swapRow = false; // Options, Game: "Swap ○ and ×" On - the other way round
            for (const OptRow& o : optionRows(app, 0))
                if (o.label.rfind("Swap", 0) == 0 && o.act) { o.act(0); swapRow = true; }
            swapRow = swapRow && !app.padConfirmEast;
            app.camp.cursor = 0;
            padButton(app, SDL_GAMEPAD_BUTTON_SOUTH);
            const bool swapped = swapRow && app.camp.page == App::Camp::Items;
            for (const OptRow& o : optionRows(app, 0))
                if (o.label.rfind("Swap", 0) == 0 && o.act) o.act(1); // Off: as on the Vita
            app.padConfirmEast = true;
            const auto held = [&](int button, bool down) { SDL_SetJoystickVirtualButton(js, button, down); SDL_UpdateJoysticks(); };
            const auto repeats = [&] { // (half a second of a D-pad direction held in the Camp: the cursor's moves)
                app.camp.cursor = 0;
                for (int f = 0; f < 30; ++f) padStep(app, 1.0f / 60.0f);
                return app.camp.cursor != 0;
            };
            const auto focusLost = [&] {
                SDL_Event e{};
                e.type = SDL_EVENT_WINDOW_FOCUS_LOST, e.window.windowID = SDL_GetWindowID(app.window);
                SDL_AppEvent(&app, &e);
                e.type = SDL_EVENT_USER; // (the next event finds it)
                SDL_AppEvent(&app, &e);
            };
            app.camp.page = App::Camp::Main;
            held(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
            const bool repeated = repeats();
            focusLost();
            const bool repeatStopped = !repeats();
            held(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
            padStep(app, 1.0f / 60.0f);
            held(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
            const bool repeatFresh = repeats();
            held(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
            padStep(app, 1.0f / 60.0f);
            app.camp.cursor = 0;
            problems += !repeated || !repeatStopped || !repeatFresh;
            std::printf("controller: a held direction repeats %s, stops with the focus %s, and repeats again once let go %s\n",
                        repeated ? "yes" : "NO", repeatStopped ? "yes" : "NO", repeatFresh ? "yes" : "NO");
            padButton(app, SDL_GAMEPAD_BUTTON_NORTH); // △ again: closed, from any page
            const bool closed = !app.campOpen;
            { // The Vita copy (its __menuf_mainPrc / Put): △ the bar, the cursor on its last entry (Camp),
              // ← ↑ / → ↓ round the entries, ○ the one under it, × the bar shut
                const bool keepVita = app.vitaOnly;
                const int keepLayout = app.video.layout;
                const bool keepEast = app.padConfirmEast;
                app.vitaOnly = true, app.video.layout = 0, app.padConfirmEast = true;
                app.fieldMenu = App::FieldMenu();
                app.fieldMenu.fade = 320;
                const std::vector<int> e = fieldBarEntries(app);
                auto settle = [&] { for (int f = 0; f < 8 && app.fieldMenu.anim > 0; ++f) --app.fieldMenu.anim; };
                padButton(app, SDL_GAMEPAD_BUTTON_NORTH);
                settle();
                const bool barOpen = app.fieldMenu.open && !app.campOpen && !e.empty() && e.back() == 0 &&
                                     app.fieldMenu.cursor == int(e.size()) - 1;
                padButton(app, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
                const bool stepped = e.size() > 1 && e[size_t(app.fieldMenu.cursor)] == e[e.size() - 2];
                padButton(app, SDL_GAMEPAD_BUTTON_DPAD_DOWN), padButton(app, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
                const bool wrapped = app.fieldMenu.cursor == (e.size() > 1 ? 0 : -1);
                padButton(app, SDL_GAMEPAD_BUTTON_SOUTH); // × (○ confirms on the Vita)
                settle();
                const bool shut = !app.fieldMenu.open;
                padButton(app, SDL_GAMEPAD_BUTTON_NORTH);
                settle();
                padButton(app, SDL_GAMEPAD_BUTTON_EAST); // ○ on Camp
                const bool camp = app.campOpen;
                padButton(app, SDL_GAMEPAD_BUTTON_NORTH);
                app.fieldMenu = App::FieldMenu();
                app.vitaOnly = keepVita, app.video.layout = keepLayout, app.padConfirmEast = keepEast;
                const bool ok = barOpen && stepped && wrapped && shut && camp && !app.campOpen;
                problems += !ok;
                std::printf("controller, the Vita's bar: △ opens it on Camp %s, the cursor steps %s and wraps %s, × shuts it %s, ○ opens Camp %s\n",
                            barOpen ? "yes" : "NO", stepped ? "yes" : "NO", wrapped ? "yes" : "NO", shut ? "yes" : "NO", camp ? "yes" : "NO");
            }
            { // A puzzle room's touch from the controller: R3 on a psychokinesis block lifts it, the right
              // stick slides the finger, R3 let go sends it; outside a puzzle room the pointer stays away
                const bool fieldAway = !stickPointerWanted(app);
                const bool keepPointerLast = app.pointerLast, keepPointerCancelled = app.pointerCancelled;
                int pkStage = -1;
                for (int i = 0; i < cr::kPuzzleStages; ++i)
                    if (cr::kPuzzleStage[i].ability == cr::kAbilityPsychokinesis) { pkStage = i; break; }
                const cr::GameState keepRoomGame = app.game;
                cr::WarpRequest w;
                w.valid = true, w.area = cr::kPuzzleArea, w.map = cr::kFirstPuzzleMap + pkStage;
                const bool entered = pkStage >= 0 && performWarp(app, w, -1);
                app.runner.stop(), app.pendingEntry = App::Entry::None, app.dialogue.clear();
                for (int f = 0; f < 30; ++f) stepEvents(app), stepFieldMenu(app);
                app.runner.stop(), app.pendingEntry = App::Entry::None, app.dialogue.clear();
                int target = -1;
                for (size_t i = 0; entered && i < app.puzzle.objs.size(); ++i)
                    if (app.puzzle.objs[i].type == cr::kTypeNormal && app.puzzle.objs[i].shown) { target = int(i); break; }
                bool lifted = false, followed = false, sent = false;
                if (target >= 0) {
                    const App::Puzzle::Obj& o = app.puzzle.objs[size_t(target)];
                    const cr::FieldNpc& n = app.npcs[size_t(o.npc)];
                    const int row = o.row, col = o.col;
                    float lx = 0, ly = 0, ax, ay, bx, by;
                    fieldToLogical(app, {n.pos[0], n.pos[1] + n.box[1], n.pos[2]}, lx, ly);
                    app.renderer.toLogical(0, 0, ax, ay), app.renderer.toLogical(1000, 1000, bx, by); // (the frame's pixels back)
                    int ww = 0, wh = 0;
                    SDL_GetWindowSize(app.window, &ww, &wh);
                    App::StickPointer& sp = app.stickPointer;
                    sp = App::StickPointer();
                    sp.x = (lx - ax) * 1000 / (bx - ax) * float(ww) / float(app.renderer.frameWidth());
                    sp.y = (ly - ay) * 1000 / (by - ay) * float(wh) / float(app.renderer.frameHeight());
                    sp.shown = true, sp.stage = app.puzzle.stage;
                    const float px = sp.x, py = sp.y;
                    SDL_Event pe{};
                    pe.gbutton.which = SDL_GetGamepadID(pad), pe.gbutton.button = SDL_GAMEPAD_BUTTON_RIGHT_STICK;
                    // (each way in turn until the block goes: a wall or another block may stand in one)
                    static const int kWay[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto& wv : kWay) {
                        sp.x = px, sp.y = py;
                        pe.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        SDL_AppEvent(&app, &pe);
                        lifted = lifted || (sp.down && app.puzzle.pkState == 1 && app.puzzle.pkTarget == target);
                        for (int f = 0; f < 6; ++f) stepEvents(app);
                        const float x0 = sp.x, y0 = sp.y;
                        SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTX, Sint16(wv[0] * 32767));
                        SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTY, Sint16(wv[1] * 32767));
                        SDL_UpdateJoysticks();
                        for (int f = 0; f < 4; ++f) stickPointerStep(app, 0.05f), stepEvents(app);
                        SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTX, 0);
                        SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTY, 0);
                        SDL_UpdateJoysticks();
                        followed = followed || std::fabs(sp.x - x0) + std::fabs(sp.y - y0) > 20;
                        pe.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
                        SDL_AppEvent(&app, &pe);
                        for (int f = 0; f < 90; ++f) stepEvents(app);
                        sent = !sp.down && (o.row != row || o.col != col);
                        if (sent) break;
                    }
                }
                stickPointerRelease(app);
                app.stickPointer = App::StickPointer();
                app.pointerLast = keepPointerLast, app.pointerCancelled = keepPointerCancelled;
                app.game = keepRoomGame;
                showMap(app, 0, 0, 2);
                app.pendingEntry = App::Entry::None, app.runner.stop(), app.dialogue.clear();
                const bool ok = fieldAway && entered && lifted && followed && sent;
                problems += !ok;
                std::printf("controller, a puzzle room's touch: away from puzzles %s, R3 lifts a block %s, the right stick slides %s, let go sends it %s\n",
                            fieldAway ? "yes" : "NO", lifted ? "yes" : "NO", followed ? "yes" : "NO", sent ? "yes" : "NO");
            }
            app.runner.stop();
            app.dialogue.clear();
            app.pendingEntry = App::Entry::None;
            const cr::Vec3 heldFrom = app.walker.pos;
            SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 32767);
            SDL_UpdateJoysticks();
            focusLost();
            for (int f = 0; f < 15; ++f) updatePlayer(app, 1.0f / kFramesPerSecond, 0);
            const cr::Vec3 heldMove = app.walker.pos - heldFrom;
            const bool walkStopped = std::sqrt(cr::dot(heldMove, heldMove)) < 0.01f;
            SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_UpdateJoysticks();
            padStep(app, 1.0f / 60.0f);
            app.walker.pos = heldFrom;
            app.runner.stop();
            app.dialogue.clear();
            app.pendingEntry = App::Entry::None;
            problems += !walkStopped;
            std::printf("controller: a stick held through a lost focus stops walking %s\n", walkStopped ? "yes" : "NO");
            problems += !walked || !fieldQuiet || !opened || !moved || !confirmed || !backed || !swapped || !closed;
            std::printf("controller: the stick walks %s, △ opens / closes the Camp %s / %s, the D-pad moves %s, ○ confirms %s, × back %s "
                        "(on the field nothing %s), ×-confirm %s; %d problems\n", walked ? "yes" : "NO", opened ? "yes" : "NO",
                        closed ? "yes" : "NO", moved ? "yes" : "NO", confirmed ? "yes" : "NO", backed ? "yes" : "NO",
                        fieldQuiet ? "yes" : "NO", swapped ? "yes" : "NO", problems);
            app.campOpen = false;
            app.pad = keepPad;
            app.game = keepGame;
            SDL_CloseGamepad(pad);
        }
        if (id) SDL_DetachVirtualJoystick(id);
        mapErrors += problems;
    }
    { // btl_lose: an ordinary battle lost goes back to the pair's Ark, a fifth of the Oz gone
        int problems = 0;
        const cr::GameState keepGame = app.game;
        const int keepChar = app.playerChar;
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        app.game.oz = 1005;
        app.playerChar = 2; // Zhamo and Eluca: area 13
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        startBattle(app, {1, 0, -1, 0}, 0);
        const bool ordinary = app.battle.active && !app.battle.eventBattle;
        if (app.battle.active) loseToArk(app);
        const bool ark = app.area == 13 && app.mapNumber == 103 && !app.battle.active;
        const bool oz = app.game.oz == 1005 - 201;
        problems += !ordinary || !ark || !oz;
        std::printf("battle lost: back to the Ark (area %d, room %d) %s, Oz %d (a fifth gone %s); %d problems\n", app.area,
                    app.mapNumber, ark ? "yes" : "NO", app.game.oz, oz ? "yes" : "NO", problems);
        mapErrors += problems;
        app.runner.stop();
        app.pendingEntry = App::Entry::None;
        app.game = keepGame;
        app.playerChar = keepChar;
    }
    { // PrcMsg's typing: a character a frame, a tap brings the page out (up to its next stop) and the
      // next turns it; 0x1a pauses it; 0x14 closes the message by itself (j005_d05's 5203: five
      // pauses, then closes after the last)
        int problems = 0;
        const cr::EventFile keepEvents = app.events;
        const bool keepType = app.typeOn;
        app.typeOn = true;
        app.dialogue.clear();
        showMessages(app, {57006}); // a page with no codes
        const int chars = app.dialogue.empty() ? 0 : pageChars(app.dialogue[0]);
        for (int f = 0; f < 10; ++f) stepTyping(app);
        const bool rate = chars > 10 && app.dialogueTyped == 10 && !pageTyped(app);
        tapDialogue(app);
        for (size_t f = 0; f < 8 && !app.dialogue.empty() && !pageTyped(app); ++f) stepTyping(app); // (a frame a line)
        const bool whole = pageTyped(app) && !app.dialogue.empty();
        tapDialogue(app);
        const bool turned = app.dialogue.empty();
        problems += !rate || !whole || !turned;
        int frames = 0, taps = 0, pauses = 0, pauseFrames = 0, total = 0, closeAfter = -2, turnsSeen = 0;
        bool closed = false;
        if (app.events.load(app.dataDir, 18)) {
            showMessages(app, {5203});
            for (size_t k = 0; k < app.dialogueInfo.size() && k < app.dialogue.size(); ++k) // (those at a page's end
                for (const auto& pz : app.dialogueInfo[k].pauses)                           // hold its turn or closing)
                    ++pauses, pauseFrames += pauseHold(pz.frames);
            for (const auto& pg : app.dialogue) total += pageChars(pg);
            closeAfter = app.dialogueInfo.empty() ? -2 : app.dialogueInfo.back().closeAfter;
            const size_t pages = app.dialogue.size();
            for (size_t k = 0; k + 1 < app.dialogueInfo.size() && k + 1 < pages; ++k) // pages the window clears by itself
                turnsSeen += app.dialogueInfo[k].closeAfter >= 0;
            for (; frames < 4000 && !app.dialogue.empty(); ++frames) {
                stepTyping(app);
                const cr::MessageInfo* pinfo = pageInfo(app);
                if (!app.dialogue.empty() && pageTyped(app) && app.dialoguePage + 1 < app.dialogue.size() &&
                    !(pinfo && pinfo->closeAfter >= 0)) {
                    tapDialogue(app); // the reader turns each page once it's out
                    ++taps;
                }
            }
            closed = app.dialogue.empty() && size_t(taps + turnsSeen) + 1 == pages;
        }
        const int least = total + pauseFrames;
        problems += pauses == 0 || closeAfter <= 0 || !closed || frames < least || frames > least + 2 * (taps + turnsSeen + 2);
        std::printf("message typing: a character a frame %s, a tap brings it out %s, the next turns it %s; 5203: %d "
                    "characters, %d pauses (%d frames), then closes by itself (%d frames after the last) %s in %d frames; %d problems\n",
                    rate ? "yes" : "NO", whole ? "yes" : "NO", turned ? "yes" : "NO", total, pauses, pauseFrames, closeAfter,
                    closed ? "yes" : "NO", frames, problems);
        mapErrors += problems;
        app.dialogue.clear();
        app.events = keepEvents;
        app.typeOn = keepType;
    }
    { // 2.2.0 _PrcMsg's pages (0x2c444): 0x1a's length, a tap stopping at the next pause / line
      // break, 0x12 keeping the lines (0x11 after it) or going on along the same line out at once
      // (anything else after it)
        int problems = 0;
        auto check = [&](bool ok, const char* what) {
            if (!ok) ++problems, std::printf("message pages FAILED: %s\n", what);
        };
        const cr::EventFile keepEvents = app.events;
        const bool keepType = app.typeOn;
        const int keepLanguage = app.language;
        const App::MsgWin keepWin = app.msgWin;
        app.typeOn = true, app.language = 1, app.msgWin = App::MsgWin();
        auto page = [&](const std::vector<std::string>& lines, const std::vector<cr::MessageInfo::Pause>& pz) {
            app.dialogue = {lines};
            app.dialoguePage = 0;
            cr::MessageInfo mi;
            mi.pauses = pz;
            app.dialogueInfo = {mi};
            app.dialogueVoices = {{}};
            ++app.dialogueSerial;
            resetTyping(app);
        };
        page({"abcdef"}, {{0, 2, 0}, {0, 4, 5}});
        std::vector<int> at; // the frame each character comes out
        for (int f = 0; f < 20 && !pageTyped(app); ++f) {
            const int before = app.dialogueTyped;
            stepTyping(app);
            if (app.dialogueTyped > before) at.push_back(f);
        }
        check(at == std::vector<int>{0, 1, 3, 4, 9, 10}, "0x1a n: the next character n frames after the one before (2 for n = 0)");
        page({"abcdef", "ghij"}, {{0, 3, 10}});
        stepTyping(app);
        tapDialogue(app);
        stepTyping(app);
        const bool atPause = app.dialogueTyped == 3 && app.dialogueInstant && !pageTyped(app);
        for (int f = 0; f < 9; ++f) stepTyping(app);
        const bool atBreak = app.dialogueTyped == 7 && !pageTyped(app);
        stepTyping(app);
        check(atPause && atBreak && pageTyped(app), "a tap: out at once up to the next pause, after it up to the line break, then the rest");
        page({"abc"}, {});
        app.dialogue = {{"abc"}, {"abcdef"}};
        app.dialogueInfo.resize(2);
        app.dialogueInfo[1].keep = 3, app.dialogueInfo[1].instant = true;
        for (int f = 0; f < 5; ++f) stepTyping(app);
        tapDialogue(app);
        stepTyping(app);
        check(app.dialoguePage == 1 && app.dialogueTyped == 6 && pageTyped(app), "a kept page going on along the line: out at once");
        auto typeOut = [&] { for (int f = 0; f < 400 && !app.dialogue.empty() && !pageTyped(app); ++f) stepTyping(app); };
        // j001_d02 2623: "Yes, I'm fine." / "It's nothing." 0x12 0x11 "I'm just... a little scared to
        // fight..." - the second page keeps the first two lines (reference 02 1240 s)
        bool keptLines = false, keptTyping = false;
        if (app.events.load(app.dataDir, 3)) {
            app.dialogue.clear();
            showMessages(app, {2623});
            if (app.dialogue.size() == 2 && app.dialogueInfo.size() == 2) {
                const std::vector<std::string> first = app.dialogue[0];
                const int keep = pageChars(first);
                typeOut();
                tapDialogue(app);
                keptLines = app.dialoguePage == 1 && app.dialogue[1].size() == first.size() + 1 &&
                            app.dialogueTyped == keep && typedPage(app.dialogue[1], app.dialogueTyped) == first;
                stepTyping(app); // (the line break)
                stepTyping(app);
                keptTyping = !app.dialogueInstant && app.dialogueTyped == keep + 2;
            }
        }
        check(keptLines && keptTyping, "2623: 0x12 0x11 keeps the lines above and types on from the next");
        std::printf("message pages: 0x1a's frames, a tap out to the next pause / line break, 0x12 0x11 keeping the lines %s; %d problems\n",
                    keptLines && keptTyping ? "yes" : "NO", problems);
        mapErrors += problems;
        app.dialogue.clear();
        app.dialogueInfo.clear();
        app.dialogueVoices.clear();
        resetTyping(app);
        app.events = keepEvents;
        app.typeOn = keepType, app.language = keepLanguage, app.msgWin = keepWin;
    }
    { // Scripts as 2.2.0's (audit 2026-11-13): op 58's back mask and Wait 0xFFFC, FrmAdjc's facing,
      // _add_item's 99, Uty_GetItem's Oz credit and band, Anim keeping a playing action's loop, a
      // same-map warp keeping the facing, Uty_GetBgmNo's field request (__EvtBattle zeroes it)
        int problems = 0;
        auto check = [&](bool ok, const char* what) {
            if (!ok) ++problems, std::printf("scripts 2.2.0 FAILED: %s\n", what);
        };
        const cr::EventFile keepEvents = app.events;
        const cr::GameState keepGame = app.game;
        ViewerHost host(app);
        // j001_d05 (area 6) 0xab44: Screen(58) 0, 0, 0 -> 68 over 20 / Wait 0xFFFC - the back mask, the
        // front fade untouched, the script waiting until the back fade is over
        int backWait = -1;
        bool backSet = false;
        if (app.events.load(app.dataDir, 6)) {
            app.backFade = App::BackFade();
            const int frontFrames = app.fadeFrames, frontT = app.fadeT;
            const float frontTo = app.fadeTo;
            cr::EventRunner r;
            if (r.startAt(app.events, 0xab44, 0, app.game, 6, app.mapNumber)) {
                r.step(&host);
                backSet = std::fabs(app.backFade.to - 68.0f / 255.0f) < 1e-4f && app.backFade.frames == 20 &&
                          app.fadeFrames == frontFrames && app.fadeT == frontT && app.fadeTo == frontTo;
                for (backWait = 0; backWait < 40 && r.running() && r.where().find("back fade") != std::string::npos; ++backWait) {
                    if (app.backFade.t < app.backFade.frames) ++app.backFade.t;
                    r.step(&host);
                }
                r.stop();
            }
            app.backFade = App::BackFade();
        }
        check(backSet, "op 58 sets the back mask (68 over 20 frames), not the front fade");
        check(backWait == 20, "Wait 0xFFFC holds until the back fade is over");
        // j001_d01 (area 2) 0x1d76, the chest's end: AddItem var 9 (a stack stops at 99) and Uty_GetItem
        int stack99 = -1, stack6 = -1;
        if (app.events.load(app.dataDir, 2)) {
            for (const int had : {99, 5}) {
                cr::GameState gs = keepGame;
                gs.vars[9] = 21, gs.items[21] = had;
                cr::EventRunner r;
                if (r.startAt(app.events, 0x1d76, 0, gs, 2, 0))
                    while (r.step(nullptr)) {}
                (had == 99 ? stack99 : stack6) = gs.items[21];
            }
        }
        check(stack99 == 99 && stack6 == 6, "_add_item: a stack stops at 99");
        // j001_p01 (area 1) 0x1cfb: flag 10, Uty_GetItem - var 9 Oz into the purse (9,999,999 at most),
        // SE 0xf, the Oz's icon 0x3f and "You got 2000/OZ!" (6 / 0x55) in the band (reference 01 1040 s)
        int ozAfter = -1, ozCapped = -1;
        bool ozBand = false;
        std::string ozText;
        if (app.events.load(app.dataDir, 1)) {
            for (const int purse : {100, 9999000}) {
                app.game = keepGame;
                app.game.vars[9] = 2000, app.game.oz = purse;
                app.soundLog.clear();
                cr::EventRunner r;
                if (r.startAt(app.events, 0x1cfb, 0, app.game, 1, 0)) r.step(&host);
                (purse == 100 ? ozAfter : ozCapped) = app.game.oz;
                if (purse == 100) {
                    ozBand = app.itemWindowOpen && app.itemWin.oz && app.itemWin.icon == 0x3f && app.itemWin.message == 57085 &&
                             std::find(app.soundLog.begin(), app.soundLog.end(), 0xfu) != app.soundLog.end();
                    ozText = itemBandText(app);
                }
                r.stop();
                app.itemWindowOpen = false;
            }
        }
        check(ozAfter == 2100 && ozCapped == 9999999, "Uty_GetItem with flag 10: var 9 Oz into the purse, capped");
        check(ozBand && ozText.find("2000") != std::string::npos, "the Oz band: SE 0xf, icon 0x3f, 6 / 0x55 with the amount");
        // FrmAdjc's dir: j003_d01's 211 moves npc 401 to frame 230 with dir 90 - the angle set as the
        // move starts (no turn over it); dir 2000's way as _NpcSetDirConv computes it
        struct Mover : cr::EventHost {
            int facing = -1;
            float yaw = 0;
            void moveNpc(int npc, const float*, float y, Facing f, int, int) override {
                if (npc == 401 && y == 90) facing = int(f), yaw = y; // (its dir-2000 walks before)
            }
            bool tapped() override { return true; }
        } mover;
        if (app.events.load(app.dataDir, 8)) {
            cr::GameState gs = keepGame;
            cr::EventRunner r;
            if (const uint32_t at = app.events.scriptEntry(211); at && r.startAt(app.events, at, 211, gs, 8, 0))
                for (int f = 0; f < 6000 && r.step(&mover) && mover.facing < 0; ++f) {}
        }
        check(mover.facing == int(cr::EventHost::Facing::Snap) && mover.yaw == 90, "FrmAdjc's degree: set at once");
        const float k = 180.0f / SDL_PI_F;
        check(std::fabs(travelYaw(1, 1) * k - 26.36f) < 0.05f && std::fabs(travelYaw(0, 1)) < 1e-4f &&
                  std::fabs(travelYaw(1, 0) * k - 90) < 1e-3f && std::fabs(travelYaw(-1, 0) * k - 270) < 1e-3f,
              "dir 2000: turns = (1 - dz / len) / 4 (a 45-degree move faces 26.4)");
        app.events = keepEvents;
        app.game = keepGame;
        // Anim on an action already playing keeps its loop; another one takes the new loop; npc 0 the leader
        bool keptLoop = true, newLoop = true;
        if (!app.npcDraws.empty()) {
            App::NpcDraw& d = app.npcDraws[0];
            const App::NpcDraw keep = d;
            int slot = d.action; // (a slot no LoadAct changed: the model's own action of that number)
            if (auto a = app.actorSlots.find(d.id); a != app.actorSlots.end() && a->second.count(slot)) slot = -1;
            if (slot >= 0) {
                d.loopFrom = 2, d.loopTo = 8, d.once = false;
                host.playAnim(d.id, slot, 0, 0);
                keptLoop = d.loopFrom == 2 && d.loopTo == 8;
                host.playAnim(d.id, slot + 1, 3, 5);
                newLoop = d.action != keep.action && d.loopFrom == 3 && d.loopTo == 5 && d.time == 0;
            }
            for (App::NpcDraw& o : app.npcDraws)
                if (o.id == keep.id) o = keep;
        }
        check(keptLoop && newLoop, "Anim: the playing action keeps its loop, another starts with its own");
        const App::PlayerAnim keepAnim = app.playerAnim;
        app.playerAnim = App::PlayerAnim();
        host.playAnim(0, 1, 0, 0);
        check(app.playerAnim.on && app.playerAnim.action == 1, "Anim on npc 0: the leader's model");
        app.playerAnim = keepAnim;
        // A warp to the map in play: the position alone (__EvtWarp writes gac+0xd6c..0xd74)
        if (app.havePlayer && app.mapNumber >= 0) {
            const cr::FieldWalker keepWalker = app.walker;
            app.walker.heading = 1.0f;
            cr::WarpRequest w;
            w.valid = true, w.area = -1, w.map = app.mapNumber, w.angleDegrees = 180;
            w.pos[0] = app.walker.pos.x, w.pos[1] = app.walker.pos.y, w.pos[2] = app.walker.pos.z;
            performWarp(app, w, -1);
            check(app.walker.heading == 1.0f, "a same-map warp keeps the facing");
            app.walker = keepWalker;
        }
        // Uty_GetBgmNo: the field's last play request; __EvtBattle zeroes it
        const uint32_t keepBgm = app.bgmNo;
        playBgm(app, 7);
        const bool asked = app.bgmNo == 7;
        host.battle(1, 0, 0);
        check(asked && app.bgmNo == 0 && host.bgmNo() == 0, "Uty_GetBgmNo: the field's request, zeroed by __EvtBattle");
        app.battle.requested = false;
        app.bgmNo = keepBgm;
        playBgm(app, 1u << 29);
        std::printf("scripts 2.2.0: back mask %s (Wait 0xFFFC %d frames), stacks %d / %d, Oz %d / %d \"%s\", FrmAdjc facing %d at %.0f; "
                    "%d problems\n", backSet ? "yes" : "NO", backWait, stack99, stack6, ozAfter, ozCapped, ozText.c_str(),
                    mover.facing, mover.yaw, problems);
        mapErrors += problems;
    }
    { // The calcs' queued messages (btl_add_msgque / menub_mainPut): a monster's poison queues
      // "<name> was poisoned!" for its target (one per fighter); the banner shows each 60 frames
      // once free and the battle waits for the queue before the next action
        int problems = 0;
        const cr::GameState keepGame = app.game;
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        startBattle(app, {1, 0, -1, 0}, 0);
        App::Battle& b = app.battle;
        bool queued = false, oneEach = false, waited = true, shown = false;
        int held = 0, nextAt = -1;
        if (b.active) {
            for (int f = 0; f < 60 && b.phase != App::Battle::Command; ++f) stepBattle(app);
            b.rules.forceAction = 122; // enemy 2 casts poison
            cr::BattleCommand cmd[2];
            cmd[0].kind = cmd[1].kind = cr::kCmdDefend;
            b.rules.beginRound(cmd, app.battleActions);
            cr::BattleTurn t;
            while (b.rules.nextTurn(app.battleActions, t) && t.action != 122) {}
            int target = -1;
            for (const cr::BattleHit& h : t.hits) target = h.target;
            queued = t.action == 122 && target >= 0 && t.notices.size() == 1 && t.notices[0].fighter == target &&
                     t.notices[0].entry == 6 && b.rules.f[size_t(target)].status[cr::kStatusPoison] == 5;
            t.notices.push_back({target, 0x19}); // the same fighter again: dropped
            t.notices.push_back({2, 0x1d});
            b.phase = App::Battle::Acting;
            b.turn = t;
            b.turnOn = false;
            b.bannerFrames = 0;
            showHits(app);
            oneEach = b.notices.size() == 2;
            const std::string want = target >= 0 ? sysText(app, 57006, battleActor(app, target) ? battleActor(app, target)->name : "") : "";
            for (int f = 0; f < 400 && nextAt < 0; ++f) {
                const bool before = !b.notices.empty() || b.bannerFrames > 1;
                stepBattle(app);
                if (b.banner == want && b.bannerFrames > 0) shown = true, ++held;
                if (b.turnOn || b.phase != App::Battle::Acting) nextAt = f;
                else if (!before) break;
                waited &= !(b.turnOn && before);
            }
            endBattle(app, false);
        }
        problems += !queued || !oneEach || !shown || held != btlui::kMessageFrames || !waited || nextAt < 2 * btlui::kMessageFrames;
        std::printf("battle messages: poison queued for its target %s, one per fighter %s, shown %d frames, the next action after "
                    "frame %d, waited %s; %d problems\n", queued ? "yes" : "NO", oneEach ? "yes" : "NO", held, nextAt,
                    waited ? "yes" : "NO", problems);
        mapErrors += problems;
        app.runner.stop();
        app.game = keepGame;
    }
    { // Battle presentation as 2.2.0's: __dmg_prc's labels, slots and places, the rows' rolling numbers,
      // _btlcalc_chk_dead's reactions, _btlcalc_0404's gating, the invincible's "0", +0xaea (the member
      // whose action came up last), the talk's HUD slide, an enemy pair's pair attack with one left
        int problems = 0;
        std::string what;
        auto fail = [&](bool bad, const char* name) {
            if (bad) ++problems, what += std::string(" ") + name;
        };
        const cr::GameState keepGame = app.game;
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        fail(hitLabel(1) != 0 || hitLabel(3) != 2 || hitLabel(5) != 5 || hitLabel(6) != 6 || hitLabel(7) != 7, "labels");
        startBattle(app, {1, 0, -1, 0}, 0);
        App::Battle& b = app.battle;
        if (b.active) {
            for (int f = 0; f < 60 && b.phase != App::Battle::Command; ++f) stepBattle(app);
            // the places (480 x 320 units)
            for (App::Battle::Popup& p : b.popups) p = App::Battle::Popup();
            b.popups[0].on = true, b.popups[0].amount = 50;
            float x = 10, y = 300;
            placePopup(b, 0, x, y, 0);
            fail(x != 64 || y != 272, "slot0-clamp");
            b.popups[1].on = true, b.popups[1].amount = 123;
            x = 470, y = 100;
            placePopup(b, 1, x, y, 0);
            fail(x != 420 || y != 148, "slot1-under-slot0");
            b.popups[3].on = true, b.popups[3].amount = -5;
            x = 200, y = 10;
            placePopup(b, 3, x, y, 0);
            fail(y != 72 + 28, "slot3-heal");
            b.popups[2].on = true, b.popups[2].amount = -5;
            x = 200, y = 280;
            placePopup(b, 2, x, y, 0);
            fail(y != 242 || b.popups[2].y != 272, "slot2-heal-near-slot0");
            // five slots, a sixth dropped; nothing taken - no reaction, still jolting under 0404
            for (App::Battle::Popup& p : b.popups) p = App::Battle::Popup();
            const cr::BattleActionDef* atk = app.battleActions.get(14);
            fail(!atk || atk->calc != 0, "action14-calc");
            App::Battle::Actor* m2 = battleActor(app, 2);
            App::Battle::Actor* u0 = battleActor(app, 0);
            App::Battle::Actor* u1 = battleActor(app, 1);
            b.turn = cr::BattleTurn();
            b.turn.action = 14;
            b.turn.actors[0] = 0;
            for (int i = 0; i < 6; ++i) {
                cr::BattleHit h;
                h.target = 2, h.amount = 0, h.type = cr::kHitMiss;
                b.turn.hits.push_back(h);
            }
            if (m2) playSlot(app, *m2, btlui::kSlotIdle);
            b.quake = {};
            showHits(app);
            int on = 0;
            for (const App::Battle::Popup& p : b.popups) on += p.on;
            fail(on != 5, "five-slots");
            fail(m2 && m2->slot != btlui::kSlotIdle, "no-reaction-for-0");
            fail(b.quake.mode != 1, "0404-miss-jolts");
            b.turn.hits.assign(1, b.turn.hits[0]);
            b.turn.hits[0].type = cr::kHitGuard;
            b.quake = {};
            for (App::Battle::Popup& p : b.popups) p = App::Battle::Popup();
            showHits(app);
            fail(b.quake.mode != 0, "0404-fended-off-still");
            // a blow on a defending member: no flinch; on one standing: 5 then 0; a revive: 7 then 0; a
            // monster's fall: 5 then standing as it fades
            if (u0 && u1 && m2) {
                playSlot(app, *u0, btlui::kSlotGuard);
                const bool guards = u0->slot == btlui::kSlotGuard;
                playSlot(app, *u1, btlui::kSlotIdle);
                b.turn.hits.clear();
                cr::BattleHit h;
                h.target = 0, h.amount = 10;
                b.turn.hits.push_back(h);
                h.target = 1;
                b.turn.hits.push_back(h);
                for (App::Battle::Popup& p : b.popups) p = App::Battle::Popup();
                showHits(app);
                fail(guards && u0->slot != btlui::kSlotGuard, "defending-no-flinch");
                fail(u1->slot != btlui::kSlotHit || u1->then != btlui::kSlotIdle, "hit-5-then-0");
                b.turn.hits.clear();
                h = cr::BattleHit();
                h.target = 1, h.amount = -100, h.revived = true;
                b.turn.hits.push_back(h);
                for (App::Battle::Popup& p : b.popups) p = App::Battle::Popup();
                showHits(app);
                fail(u1->then != btlui::kSlotIdle || (u1->slot != btlui::kSlotRise && u1->slot != btlui::kSlotIdle), "revive-7-then-0");
                if (b.rules.f[2].def && !b.rules.f[2].def->strongGrowth) {
                    b.turn.hits.clear();
                    h = cr::BattleHit();
                    h.target = 2, h.amount = 10, h.fell = true;
                    b.turn.hits.push_back(h);
                    for (App::Battle::Popup& p : b.popups) p = App::Battle::Popup();
                    showHits(app);
                    fail(m2->slot != btlui::kSlotHit || m2->then != btlui::kSlotIdle || !m2->fading, "monster-fall-5-then-0");
                }
            }
            // the rows roll: a fifth of the gap a frame, 1 within 4
            b.rollHp[0] = 1000, b.shownHp[0] = 0, b.rollMp[0] = 3, b.shownMp[0] = 0;
            rollPartyNumbers(b);
            fail(b.rollHp[0] != 800 || b.rollMp[0] != 2, "roll");
            // the talk's slide: 120 a frame to 480 and back
            app.game.setFlag(battletalk::kFlag, true);
            std::string slide;
            for (int f = 0; f < 5; ++f) stepMenuView(app), slide += std::to_string(b.hudSlide) + " ";
            app.game.setFlag(battletalk::kFlag, false);
            for (int f = 0; f < 2; ++f) stepMenuView(app), slide += std::to_string(b.hudSlide) + " ";
            fail(slide != "120 240 360 480 480 360 240 ", "talk-slide");
            endBattle(app, false);
        } else
            fail(true, "no-battle");
        { // the rules: Is_invincible's 0 keeps the type; +0xaea follows the members as they act; a lone
          // monster's pair attack (18) becomes its own ((model - 73) / 2 + 10)
            cr::GameState fresh;
            cr::giveStartingItems(fresh);
            cr::BattleRules k;
            k.seed(3);
            k.setupParty(fresh, app.itemTable, 0);
            k.setupEnemies(app.battleData, app.battleData.group(1), 0);
            k.f[2].hp = k.f[2].hpMax = 99999;
            k.f[2].lck = -1000;
            k.f[2].status[cr::kStatusInvincible] = 3;
            cr::MonsterDef paired = k.f[2].def ? *k.f[2].def : cr::MonsterDef();
            paired.model = 77;
            k.f[2].def = &paired;
            k.forceAction = 18;
            cr::BattleCommand cmd[2];
            cmd[0].kind = cr::kCmdAttack, cmd[0].target = 2;
            cmd[1].kind = cr::kCmdAttack, cmd[1].target = 2;
            k.beginRound(cmd, app.battleActions);
            cr::BattleTurn tt;
            int members = 0, followed = 0, zero = 0, lone = 0;
            while (k.nextTurn(app.battleActions, tt)) {
                if (tt.actors[0] >= 0 && tt.actors[0] < 2) ++members, followed += k.soloMember() == tt.actors[0];
                for (const cr::BattleHit& hh : tt.hits)
                    if (hh.target == 2 && tt.actors[0] < 2) zero += hh.amount == 0 && hh.type != cr::kHitNoEffect;
                if (tt.actors[0] == 2) lone += tt.action == (77 - 73) / 2 + 10;
            }
            fail(members < 1 || followed != members, "0xaea");
            fail(zero < 1, "invincible-0");
            fail(lone != 1, "enemy-pair-attack-alone");
        }
        std::printf("battle presentation (2.2.0): labels, slots, places, reactions, 0404 gating, rows' roll, talk slide, "
                    "invincible 0, +0xaea, lone pair attack; %d problems%s\n", problems, what.c_str());
        mapErrors += problems;
        app.runner.stop();
        app.game = keepGame;
    }
    { // The World Select (Util 1): the gates the pair has reached in its order, the level bands,
      // "Skip the first half?" where its flag is set, Back, and the sixth gate without bands
        int problems = 0;
        const cr::GameState keepGame = app.game;
        const int keepChar = app.playerChar;
        App::WorldSelect& ws = app.worldSelect;
        auto look = [&](const char* var) {
            if (const char* path = SDL_getenv(var)) {
                app.lastTicks = 0;
                drawFrame(app, false);
                saveFrame(app, path);
            }
        };
        auto settle = [&] { for (int f = 0; f < 12 && app.worldChoiceOpen; ++f) stepWorldSelect(app); };
        auto finish = [&] { for (int f = 0; f < 30 && app.worldChoiceOpen; ++f) stepWorldSelect(app); };
        app.game = cr::GameState();
        app.playerChar = 0;
        for (int f : {1076, 1077, 1123, 1052}) app.game.setFlag(f, true); // gates 0, 1; one band more; a pip
        openWorldSelect(app);
        settle();
        look("CR_WORLD_SHOT_A");
        const bool offered = ws.worlds == std::vector<int>{0, 1} && ws.levels == 3;
        ws.cursor = 1;
        worldSelectChoose(app, 1);
        settle();
        const bool bands = ws.state == 6;
        look("CR_WORLD_SHOT_B");
        worldSelectLevel(app, 2);
        settle();
        const bool asked = ws.state == 8;
        look("CR_WORLD_SHOT_C");
        worldSelectAnswer(app, true);
        finish();
        const bool chosen = !app.worldChoiceOpen && app.game.vars[0] == 1 && app.game.vars[1] == 2 && app.game.vars[2] == 0 &&
                            !app.game.flag(0) && !app.game.flag(1);
        openWorldSelect(app); // Back
        settle();
        worldSelectChoose(app, -1);
        finish();
        const bool back = !app.worldChoiceOpen && app.game.flag(0);
        app.game.setFlag(1122, true); // the first gate asks to skip its first half
        openWorldSelect(app);
        settle();
        worldSelectChoose(app, 0);
        settle();
        const bool skipAsked = ws.state == 4 && ws.asking;
        worldSelectAnswer(app, true);
        settle();
        worldSelectLevel(app, 0);
        settle();
        worldSelectAnswer(app, true);
        finish();
        const bool skipped = app.game.vars[0] == 0 && app.game.flag(1) && !app.game.flag(0);
        app.game.setFlag(1104, true); // the sixth gate
        openWorldSelect(app);
        settle();
        const int last = int(ws.worlds.size()) - 1;
        worldSelectChoose(app, last);
        settle();
        const bool sixthAsked = ws.state == 8;
        worldSelectAnswer(app, true);
        finish();
        const bool sixth = last >= 0 && app.game.vars[0] == 5 && app.game.vars[1] == 0 && !app.game.flag(1);
        app.game = cr::GameState(); // the second pair's order: gate 1 first
        app.playerChar = 2;
        for (int f : {1476, 1477}) app.game.setFlag(f, true);
        openWorldSelect(app);
        const bool order = ws.worlds == std::vector<int>{1, 0};
        answerWorldChoice(app, 0);
        const bool second = app.game.vars[0] == 1 && app.game.vars[2] == 1;
        problems += !offered + !bands + !asked + !chosen + !back + !skipAsked + !skipped + !sixthAsked + !sixth + !order + !second;
        problems += app.renderer.takeErrors();
        std::printf("world select: gates offered %s, bands %s, okay? %s, chosen %s, Back %s, skip asked %s and kept %s, "
                    "the sixth %s / %s, the second pair's order %s / %s; %d problems\n",
                    offered ? "yes" : "NO", bands ? "yes" : "NO", asked ? "yes" : "NO", chosen ? "yes" : "NO",
                    back ? "yes" : "NO", skipAsked ? "yes" : "NO", skipped ? "yes" : "NO", sixthAsked ? "yes" : "NO",
                    sixth ? "yes" : "NO", order ? "yes" : "NO", second ? "yes" : "NO", problems);
        mapErrors += problems;
        app.game = keepGame;
        app.playerChar = keepChar;
    }
    { // A script's camera in a 3D room (setCamera3D): map 113's 1024 x 1024 stage view, doubled,
      // centred on the 768-row screen - the target at the screen's centre, the frustum's top edge
      // 640 rows above the screen, its sides 512 past the 1024 layout's edges
        int problems = 0;
        const cr::StageCamera keep = app.stageCam;
        app.stageCam.fovDegrees = 53.9f, app.stageCam.viewWidth = app.stageCam.viewHeight = 1024;
        const cr::Vec3 eye{0, 10, 50}, tgt{0, 10, 0};
        const cr::Mat4 vp = scriptCameraView(app, eye, tgt);
        auto toScreen = [&](const cr::Vec3& p, float& x, float& y) {
            const float* m = vp.m;
            const float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
            const float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
            x = (cx / cw * 0.5f + 0.5f) * app.renderer.screenW();
            y = (0.5f - cy / cw * 0.5f) * cr::Renderer::kLogicalH;
        };
        const float t = std::tan(53.9f * 3.14159265f / 360.0f);
        float x0, y0, x1, y1, x2, y2;
        toScreen(tgt, x0, y0);
        toScreen({0, 10 + 50 * t, 0}, x1, y1);   // the frustum's top edge at the target's distance
        toScreen({50 * t, 10, 0}, x2, y2);       // its right edge
        const float cx = app.renderer.uiX() + 512;
        problems += std::fabs(x0 - cx) > 0.5f || std::fabs(y0 - 384) > 0.5f;
        problems += std::fabs(y1 - (-640)) > 0.5f || std::fabs(x2 - (cx + 1024)) > 0.5f;
        std::printf("script camera: target at (%.1f, %.1f), top edge at row %.1f, right edge at %.1f; %d problems\n", x0 - app.renderer.uiX(),
                    y0, y1, x2 - app.renderer.uiX(), problems);
        app.stageCam = keep;
        mapErrors += problems;
    }
    { // The battles' talks: the first battle's tutorial (EvrStart 2, var 1 = 0xff1) holds the
      // battle and hides its HUD until its pages are read; an event battle's script waits behind
      // the gate and goes on after it (j001_d04 script 201: on to map 63, NpcMdlRead 508 and its Born);
      // the 1st Gate boss (group 260, j001_d05 script 201) talks after round 3 and again once a
      // pair attack lands after it, can't fall before that and heals whole each round meanwhile
        int problems = 0;
        const cr::GameState keepGame = app.game;
        const int keepChar = app.playerChar;
        App::Battle& b = app.battle;
        auto look = [&](const char* var) {
            if (const char* path = SDL_getenv(var)) {
                app.lastTicks = 0;
                drawFrame(app, false);
                saveFrame(app, path);
            }
        };
        auto mapIndex = [&](const char* name) {
            const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(name));
            return it == app.maps.end() ? -1 : int(it - app.maps.begin());
        };
        auto playScript = [&](int area, int map, int script) {
            app.battle.active = false;
            app.dialogue.clear();
            showMap(app, mapIndex(cr::kMapCollisionFile[map]), map, area);
            app.pendingEntry = App::Entry::None;
            app.runner.stop();
            app.runner.setGate(-1);
            return app.runner.start(app.events, script, app.game, app.area, app.mapNumber);
        };
        auto readOn = [&](int f) { // pages turn, item windows close
            if (!app.dialogue.empty() && f % 3 == 2) advanceDialogue(app);
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        };
        // 1. The first battle's tutorial
        app.testNoTutorials = false;
        app.game = cr::GameState();
        app.playerChar = 0;
        cr::giveStartingItems(app.game);
        showMap(app, 0, 0, 2);
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        app.puzzleBanner = App::PuzzleBanner(); // (an earlier test's)
        startBattle(app, {1, 0, -1, 0}, 0);
        int talkFrames = 0, pagesSeen = 0, pageFrames = 0;
        bool tutorialSeen = false;
        for (int f = 0; f < 2000 && b.active; ++f) {
            stepBattle(app);
            if (battletalk::on(app)) {
                ++talkFrames;
                pagesSeen = std::max(pagesSeen, int(app.dialogue.size()));
                if (!app.dialogue.empty() && ++pageFrames == 12) look("CR_TALK_SHOT_A"); // the first page and its picture
                if (pageFrames >= 12) readOn(f);
            } else if (talkFrames > 0) { // (the talk's end: btl_main 400, then the menu a frame on)
                for (int k = 0; k < 2 && b.phase == App::Battle::Intro; ++k) stepBattle(app);
                tutorialSeen = app.game.flag(0xff1) && !app.runner.running() && b.phase == App::Battle::Command;
                break;
            }
        }
        problems += !tutorialSeen || pagesSeen == 0;
        if (b.active) endBattle(app, false);
        // 2. An event battle: the script goes on after it
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        app.game.setFlag(0xff1, true);
        bool waited = true, continued = false, bossIn = false, gated = false;
        if (playScript(5, 47, 201)) {
            int f = 0;
            for (; f < 2000 && !b.active && app.runner.running(); ++f) stepEvents(app), readOn(f);
            gated = b.active && b.eventBattle && app.runner.running() && app.runner.gate() == battletalk::kFlag;
            const std::string where = app.runner.where();
            for (int k = 0; k < 90 && b.active; ++k) stepBattle(app);
            waited = app.runner.running() && app.runner.where() == where;
            if (b.active) {
                for (int k = 2; k < 5; ++k) b.rules.f[size_t(k)].hp = 0;
                endBattle(app, true);
            }
            for (f = 0; f < 4000 && app.runner.running() && !b.active; ++f) {
                stepEvents(app);
                animateNpcs(app, 1.0f / kFramesPerSecond);
                readOn(f);
                if (const cr::FieldNpc* n = findNpc(app, 508); n && npcActive(app, *n) && app.mapNumber == 63 && !bossIn) {
                    bossIn = true;
                    look("CR_TALK_SHOT_B");
                }
                continued |= app.mapNumber == 63;
            }
        }
        problems += !gated + !waited + !continued + !bossIn;
        if (b.active) endBattle(app, false);
        app.runner.stop();
        // 3. The 1st Gate boss's talks
        app.game = cr::GameState();
        cr::giveStartingItems(app.game);
        app.game.setFlag(0xff1, true);
        int talk1 = 0, talk2 = 0, heldAt1 = 0, healedWhole = 0, lateRound = 0;
        bool bossWon = false, scriptEnded = false;
        if (playScript(6, 64, 201)) {
            for (int f = 0; f < 4000 && !b.active && app.runner.running(); ++f) stepEvents(app), readOn(f);
            if (b.active && b.start.group == 260) {
                for (int k = 0; k < 2; ++k) b.rules.f[size_t(k)].hp = b.rules.f[size_t(k)].hpMax = 999999;
                bool shot = false;
                for (int f = 0; f < 30000 && b.active; ++f) {
                    if (battletalk::on(app)) {
                        bool up = false;
                        for (const App::Portrait& pt : app.portraits) up |= pt.tex.id && pt.alpha > 0.99f;
                        if (!shot && !app.dialogue.empty() && up) look("CR_TALK_SHOT_C"), shot = true;
                        readOn(f);
                    } else if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) { // the pair attacks
                        battleChooseTop(app, 1);
                        battleChooseCommand(app, 0);
                        if (b.menu == App::Battle::Targets && !b.picks.empty()) battleChooseTarget(app, b.picks[0]);
                    } else if (b.phase == App::Battle::Results) {
                        bossWon = true;
                        endBattle(app, true);
                        break;
                    }
                    const int hpBefore = b.rules.f[2].hp, roundBefore = b.rules.round;
                    stepBattle(app);
                    if (!talk1 && app.game.flag(1231)) talk1 = b.rules.round;
                    if (!talk2 && app.game.flag(1232)) talk2 = b.rules.round;
                    if (b.rules.lateAttack && !lateRound) lateRound = b.rules.round;
                    if (!app.game.flag(1232) && b.rules.f[2].hp == 1) ++heldAt1;
                    if (roundBefore == b.rules.round && (b.phase == App::Battle::Command || b.phase == App::Battle::RoundEnd) && // (the round's
                        hpBefore < b.rules.f[2].hpMax && b.rules.f[2].hp == b.rules.f[2].hpMax && !app.game.flag(1232))         // end heals: 899's 30 frames)
                        ++healedWhole;
                }
                for (int f = 0; f < 3000 && app.runner.running() && !b.active; ++f) stepEvents(app), readOn(f);
                scriptEnded = !app.runner.running();
            }
        }
        problems += talk1 != 3 || talk2 <= 3 || lateRound <= 3 || !bossWon || !scriptEnded || healedWhole == 0;
        if (b.active) endBattle(app, false);
        app.runner.stop();
        app.runner.setGate(-1);
        problems += app.renderer.takeErrors();
        std::printf("battle talks: tutorial %s (%d pages, %d frames held), event battle %s, waits %s, goes on to map 63 %s, "
                    "NPC 508 read and born %s; group 260 talks after rounds %d and %d (pair attack late in round %d), held at 1 HP "
                    "%d frames, healed whole %d times, won %s, script ended %s; %d problems\n",
                    tutorialSeen ? "shown" : "NOT shown", pagesSeen, talkFrames, gated ? "gated" : "NOT gated",
                    waited ? "yes" : "NO", continued ? "yes" : "NO", bossIn ? "yes" : "NO", talk1, talk2, lateRound, heldAt1,
                    healedWhole, bossWon ? "yes" : "NO", scriptEnded ? "yes" : "NO", problems);
        mapErrors += problems;
        app.testNoTutorials = true;
        app.game = keepGame;
        app.playerChar = keepChar;
    }
    { // The NPC model table's story models (197..304: the scenes' bosses and the party's battle
      // models - rows 501.. of every dungeon and town use 198, 200..214): each loads
        int loaded = 0, failed = 0, animated = 0, absent = 0;
        for (int k = 197; k < cr::kNumNpcModels; ++k) {
            const std::string file = cr::kNpcModelFile[k];
            if (app.npcModels.count(file)) { ++loaded; continue; }
            if (app.vitaOnly ? !app.vita.contains(file) : app.androidOnly ? !app.android.contains(file) : !SDL_GetPathInfo((app.dataDir + file).c_str(), nullptr)) { ++absent; continue; } // 197: not shipped, unused
            LoadedModel lm;
            loadModelFile(app, file, lm, true);
            if (lm.meshes.empty()) ++failed;
            else ++loaded, animated += !lm.actions.empty();
            unloadLoaded(app, lm);
        }
        mapErrors += failed;
        std::printf("NPC model table: %d story models (197..%d) load, %d with fitting actions, %d not in the data, %d failed\n",
                    loaded, cr::kNumNpcModels - 1, animated, absent, failed);
    }
    { // eff3d on the field over the whole run: the puzzle rooms', the footsteps' dust, the scripts'
        // The dust: walk the leader on the maps in turn until a step lands on a floor with a sound
        int stepMap = -1, dust = 0, steps = 0;
        for (size_t m = 0; m < app.maps.size() && stepMap < 0; ++m) {
            app.runner.stop();
            showMap(app, int(m));
            app.runner.stop();
            app.pendingEntry = App::Entry::None;
            if (!app.havePlayer || app.player.actions.size() < 2) continue;
            const int before = app.fieldEffectCount[0];
            const size_t sounds = app.soundLog.size();
            app.walker.gait = cr::Gait::Walk;
            for (int f = 0; f < 60; ++f) {
                const int want = std::min(int(app.walker.gait), int(app.player.actions.size()) - 1);
                if (want != app.player.action) app.player.action = want, app.player.time = 0, app.footPrev = 0;
                animate(app, app.player, 1.0f / kFramesPerSecond);
                playFootsteps(app);
                stepEffects(app, app.fieldEffects);
            }
            app.walker.gait = cr::Gait::Stand;
            dust = app.fieldEffectCount[0] - before;
            steps = int(app.soundLog.size() - sounds);
            if (dust > 0) stepMap = int(m);
        }
        mapErrors += dust == 0 || dust != steps;
        std::printf("footstep dust: %d puffs for %d step sounds on %s\n", dust, steps,
                    stepMap >= 0 ? app.maps[size_t(stepMap)].c_str() : "no map");
        std::string ids;
        int kinds = 0;
        for (int i = 0; i < 100; ++i)
            if (app.fieldEffectCount[size_t(i)]) ids += (kinds++ ? " " : "") + std::to_string(i) + "x" + std::to_string(app.fieldEffectCount[size_t(i)]);
        const int fxProblems = !app.fieldEffectCount[0] + !app.fieldEffectCount[0xc] + !app.fieldEffectCount[5];
        std::printf("field effects: %d kinds started (%s); FrmPlayEff %d, Uty_eff3d_set_fld %d, script quakes %d; %d problems\n",
                    kinds, ids.c_str(), app.frameEffectCalls, app.scriptEffectCalls, app.scriptQuakes, fxProblems);
        std::printf("pictures: TexDisp %d, TutoTexDisp %d (pages up to %d), the \"!\" %d\n", app.texDispCalls, app.tutoCalls,
                    app.tutoPage, app.exclamationCalls);
        std::string props;
        for (const auto& [name, n] : app.matAnimDraws) props += " " + name + "x" + std::to_string(n);
        std::printf("props with animated materials drawn:%s\n", props.empty() ? " none" : props.c_str());
        mapErrors += fxProblems;
    }
    std::printf("selftest: %zu models, %zu maps (%d with a stage camera), %d frames, %d render API errors in %d models, "
                "%d map problems; player spawned on %d maps, walked > 5 units on %d\n",
                app.models.size(), app.maps.size(), camMaps, frames, glErrors, failedModels, mapErrors, spawned, walked);
    glErrors += mapErrors;
    return glErrors ? 1 : 0;
}

// A new game (route 1): the Ark's hall (j001_t01, map 97) with only the new-game flag set; its
// script 0 plays the opening.
// CR_CAPTURE_VARS="k:v,...": script variables a capture sets (before the map's entry, and before --event's script)
void captureVars(App& app) {
    if (const char* cv = SDL_getenv("CR_CAPTURE_VARS"))
        for (const char* p = cv; *p;) {
            int k = 0, v = 0, used = 0;
            if (std::sscanf(p, "%d:%d%n", &k, &v, &used) != 2) break;
            app.game.vars[k] = v;
            p += used;
            if (*p == ',') ++p;
        }
}

void startNewGame(App& app) {
    app.game = cr::GameState();
    app.playSeconds = 0;
    // Set by the title's new-game path: t01's script 0 tests it to play the opening (event 201's
    // scene, gosub 0x5f98) instead of treating the opening as seen
    app.game.setFlag(4089, true);
    cr::giveStartingItems(app.game); // prty_init_new: the Human Genes, 3 Emergency Exits, 2 Chocolates
    app.treasureBoxes.clear();
    app.entryArea = -1;
    const int area = 7, map = 97;
    const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[map]));
    if (it != app.maps.end()) showMap(app, int(it - app.maps.begin()), map, area);
}

// __menuu_TitleClr's Load_SaveData: the title goes back to the saved block, and the play time is in
// it - the one save the game keeps for all four pairs, data.dat (none: prty_init_all's 0). A pair's
// new game (prty_init_new) leaves it, so it counts on across pairs
double savedPlaySeconds(const App& app) {
    cr::SaveData d;
    return readData(app, d) ? d.playSeconds : 0;
}

// The title, as the game boots: area 0 (j001_t99) and its EvrStart 0 - the logos, the title
// picture and music, then the title menu. Also "Return to Title".
void bootTitle(App& app) {
    endFieldTasks(app); // stop puzzle/reset/actor timers before releasing their NPCs
    app.trackerArmed = false; // (SaveData_Load_init: a load or a new game starts it again)
    app.playSeconds = savedPlaySeconds(app);
    endSceneEffects(app); // MuCom_PreReturnTitle: ScnEffectEnd
    app.renderer.resetHistory(); // a cut: the upscaler starts over
    app.runner.stop();
    app.sound.stopDialogueVoices();
    app.voiceQueue.clear();
    app.fadeGrey = app.fadeFrom = app.fadeTo = 0;
    app.fadeT = app.fadeFrames = 0;
    app.crystalCam = App::CrystalCam();
    unloadModel(app);
    if (app.battle.active) endBattle(app, false);
    app.runner.stop();
    // Discard the previous session's UI without executing pending confirmations.
    // In particular, a save prompt hidden behind Options must never survive this cut.
    app.campOpen = app.manualOpen = app.scenariosOpen = app.charSelectOpen = false;
    app.itemWindowOpen = app.optionsOpen = app.optionsFromTitle = false;
    app.resetAsk = 0;
    app.saveScreen = App::SaveScreen();
    app.resumePrompt = app.resumeCursor = app.resumeNotice = app.resumeIn = 0; // (interruptShown: once a launch)
    app.resumeWin = 0;
    app.saveImport.cancel();
    app.parkedRunner.stop();
    app.renderer.destroy(app.shop.keeper);
    app.shop = App::Shop();
    app.renderer.destroy(app.mapScreen.tex);
    app.mapScreen = App::MapScreen();
    app.renderer.destroy(app.staffRoll.tex);
    app.staffRoll = App::StaffRoll();
    app.renderer.destroy(app.lastBattle.tex);
    app.lastBattle = App::LastBattle();
    app.fieldMenu = App::FieldMenu();
    app.touchPad = App::TouchPad();
    app.menuMask = App::MenuMask();
    app.campPointer = App::CampPointer();
    app.dragging = app.optionsBarDrag = app.tapPending = false;
    clearSkip(app);
    app.padHeld = -1;
    app.padHeldFor = 0;
    app.padHeldOver.cancel();
    app.pointerCancelled = true;
    app.heldPage = false;
    app.lastPage.clear();
    app.dialogueVoices.clear();
    resetTyping(app);
    app.playing = true;
    app.lastTicks = 0;
    app.haveStageCam = false;
    app.havePlayer = false;
    app.npcs.clear();
    app.npcDraws.clear();
    app.dialogue.clear();
    app.dialogueInfo.clear();
    app.choiceFlag = -1;
    app.worldChoiceOpen = false;
    app.optionsOpen = false;
    for (App::Portrait& p : app.portraits) {
        app.renderer.destroy(p.tex);
        p = App::Portrait();
    }
    app.game = cr::GameState();
    app.treasureBoxes.clear();
    app.mode = Mode::Maps;
    app.showBackground = true;
    app.area = 0;
    app.mapNumber = cr::kAreaFirstMap[0];
    app.entryArea = 0;
    app.pendingEntry = App::Entry::None;
    app.fieldHidden = false; // (the boot script toggles it on at its start and off before the warp out)
    app.msgWin = App::MsgWin();
    playBgm(app, 1u << 29); // the script starts the title music
    std::string err;
    if (!app.events.load(app.dataDir, 0, &err)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "title: %s", err.c_str());
        return;
    }
    app.booting = true;
    app.bootClock = 0;
    app.titleOpen = false;
    if (const uint32_t at = app.events.returnEntry(0))
        if (app.runner.startAt(app.events, at, 0, app.game, 0, app.mapNumber)) app.runnerWhat = "title";
    updateTitle(app);
}

// --explore n: plays a new game by itself - talks to everyone and walks into every trigger not yet
// tried with the current story flags (people first, exits last), answering yes and winning
// battles - and logs how far the story gets and what it asked for that isn't there yet.
int runExplore(App& app, int maxActions) {
    app.persist = false;
    app.typeOn = false;
    setMode(app, Mode::Maps);
    // Its own saves (the pairs' stories carry over through them, as a player's do): a new folder
    // under the profile (CR_SAVE_DIR's in tests), never the player's own
    app.saveDirOverride = userDir() + "explore-" + std::to_string(SDL_GetTicksNS()) + "/";
    if (const char* sd = SDL_getenv("CR_EXPLORE_SAVES"); sd && *sd) // (go on from another run's saves)
        app.saveDirOverride = std::string(sd) + (sd[SDL_strlen(sd) - 1] == '/' ? "" : "/");
    SDL_CreateDirectory(app.saveDirOverride.c_str());
    auto stateKey = [&] {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t w : app.game.flags) h = (h ^ w) * 1099511628211ull;
        return h;
    };
    std::map<uint64_t, std::set<int>> gatesTried; // story state -> World Select positions chosen in it
    int worldCount = 0;
    uint64_t gateKey = 0;      // the state the last World Select opened in
    int totalFrames = 0, pagesRead = 0;
    bool worldAsked = false;                  // the last action opened the World Select
    std::set<std::pair<int, int>> deadExits;           // ways that didn't lead where the dry run said
    bool hopping = false;                               // the step taken is the way back's
    std::map<int, std::map<int, int>> roomExits;          // map -> (id x 2 + examine -> the room it leads to)
    std::map<int, std::pair<bool, uint64_t>> roomLeft;  // map -> (something untried there, in that state)
    std::set<std::pair<int, int>> triedEver;  // (map, id x 2 + examine): done before, in any state
    std::set<std::pair<int, uint64_t>> escaped; // (map, state) where the way out to another area was taken
    std::set<std::pair<int, int>> worldDoors; // (map, id) that do: tried last, once the rooms about are done
    auto settle = [&](int limit) {
        int f = 0;
        for (; f < limit && (app.runner.running() || app.pendingEntry != App::Entry::None || app.battle.active || app.lastBattle.open); ++f) {
            if (app.battle.active) { // its talks play, then it's won at its first command
                stepBattle(app);
                if (!battletalk::on(app) && app.battle.phase == App::Battle::Command) {
                    app.battle.rules.defeatEnemies(); // (with its EXP, Oz and drops)
                    std::printf("     battle %d won\n", app.battle.start.group);
                    endBattle(app, true);
                }
                if (!app.dialogue.empty()) { pagesRead += int(app.dialogue.size()); advanceDialogue(app); }
                continue;
            }
            stepEvents(app);
            if (!app.dialogue.empty()) { pagesRead += int(app.dialogue.size()); app.dialogue.clear(); }
            if (app.choiceFlag >= 0) answerChoice(app, true);
            if (app.worldChoiceOpen) { // the newest gate not yet taken in this state (then the newest)
                const int n = int(app.worldSelect.worlds.size());
                gateKey = stateKey();
                std::set<int>& done = gatesTried[gateKey];
                int pos = n - 1;
                while (pos >= 0 && done.count(pos)) --pos;
                if (pos < 0) pos = n - 1;
                done.insert(pos);
                worldCount = n;
                answerWorldChoice(app, pos);
                worldAsked = true;
            }
            if (app.lastBattle.open) { // the last battle's rounds: the last pair listed, the Camp left, Yes
                App::LastBattle& lb = app.lastBattle;
                stepLastBattle(app);
                if (lb.state == 3) lastBattleChoose(app, int(lb.pairs.size()) - 1);
                if (app.campOpen) app.campOpen = false;
                if (lb.state == 10) lastBattleAnswer(app, true);
            }
            if (app.shop.open && !app.shop.talkRunning) closeShop(app); // (it buys nothing)
            if (app.staffRoll.open) { // the ending's credits, fast, the end card tapped away
                if (!app.staffRoll.fast) std::printf("     staff roll (var 0 = %d)\n", app.game.vars[0]);
                app.staffRoll.fast = true;
                stepStaffRoll(app);
                if (app.staffRoll.state >= 7) staffRollTap(app);
            }
            if (app.saveScreen.open && !app.booting) { // "Save data?": yes, to the pair's slot
                if (!app.saveScreen.load && app.saveScreen.phase == 1 && app.saveScreen.pending == 1) saveScreenYes(app);
                closeSaveScreen(app);
            }
            if (app.itemWindowOpen) app.game.setFlag(0, true);
            app.tapPending = true;
        }
        totalFrames += f;
        if (SDL_getenv("CR_EXPLORE_DRAW")) drawFrame(app, false); // (a frame drawn: the device's deferred frees run, for measuring what a play holds)
        return f;
    };
    auto flagCount = [&] {
        int c = 0;
        for (uint32_t w : app.game.flags) c += int(std::bitset<32>(w).count());
        return c;
    };
    // Through the title as a player goes: Character Select's pair, its save screen's New (with the
    // clear data when it has some) - or, every pair's story done, the first pair's last battle
    auto fromTitle = [&](int pair, bool lastBattle) {
        bootTitle(app);
        for (int f = 0; f < 3000 && !app.titleOpen; ++f) stepEvents(app);
        titleChoose(app, 0);
        for (int f = 0; f < 300 && !app.charSelectOpen; ++f) stepEvents(app);
        if (!app.charSelectOpen) return false;
        if ((pair >= 3) != (app.charSelectPage == 1)) charSelectTurn(app);
        app.charSelectCursor = (pair - 1) & 1;
        charSelectKey(app, SDLK_RETURN);
        for (int f = 0; f < 300 && !app.saveScreen.open; ++f) stepEvents(app);
        if (!app.saveScreen.open) return false;
        const std::vector<int> plates = savePlates(app);
        const auto it = std::find(plates.begin(), plates.end(), lastBattle ? 0 : 2);
        if (it == plates.end()) return false;
        saveScreenChoose(app, int(it - plates.begin()));
        saveScreenYes(app);
        std::printf("     title: pair %d, %s\n", pair, lastBattle ? "the last battle" : app.saveScreen.kind == 3 ? "new game with the clear data" : "new game");
        return true;
    };
    // the first pair whose story isn't done (the saves' flags), else the last battle
    auto nextPair = [&] {
        const cr::GameState all = allPairsFlags(app);
        for (int q = 0; q < 4; ++q)
            if (!all.flag(32 + 2 * q)) return q + 1;
        return 0;
    };
    const int first = nextPair();
    bool finale = first == 0; // the last battle begun from the title
    if (!fromTitle(first ? first : 1, finale)) { std::printf("explore: the title didn't start a new game\n"); return 1; }
    settle(20000);
    std::set<std::tuple<int, int, uint64_t>> tried;
    std::set<int> mapsSeen{app.mapNumber};
    std::map<int, int> visits{{app.mapNumber, 1}}; // (rooms seen: the less visited first)
    int step = 0, stuckReason = 0;
    for (; step < maxActions; ++step) {
        { // a pair's story done (its second ending, 32 + 2 pair): saved, and the next pair's begins
            const int p = std::clamp(app.playerChar / 2, 0, 3);
            if (app.game.flag(39)) { stuckReason = 3; break; } // the last battle's ending: the end
            if (!finale && app.game.flag(32 + 2 * p) && !app.runner.running()) {
                saveGame(app);
                const cr::GameState all = allPairsFlags(app);
                const int next = nextPair();
                std::printf("     pair %d's story done (flags 31..38:%s%s%s%s%s%s%s%s)\n", p + 1,
                            all.flag(31) ? " 31" : "", all.flag(32) ? " 32" : "", all.flag(33) ? " 33" : "", all.flag(34) ? " 34" : "",
                            all.flag(35) ? " 35" : "", all.flag(36) ? " 36" : "", all.flag(37) ? " 37" : "", all.flag(38) ? " 38" : "");
                finale = next == 0;
                if (!fromTitle(next ? next : 1, finale)) { stuckReason = 4; break; }
                settle(20000);
                tried.clear(), triedEver.clear(), escaped.clear(), roomExits.clear(), roomLeft.clear(), deadExits.clear();
                worldDoors.clear(), gatesTried.clear();
                mapsSeen.insert(app.mapNumber);
                continue;
            }
        }
        // candidates: people / objects (kind bit 3), then touch boxes that don't warp, then exits
        struct Cand { int id; bool examine; int rank; };
        std::vector<Cand> cands;
        // a dry run on a copy of the state tells where a script leads: new rooms first, back last
        int lastTarget = -1;
        auto rankOf = [&](int id, int base) {
            cr::GameState trial = app.game;
            const cr::EventResult r = cr::runEvent(app.events, id, trial, app.area, app.mapNumber);
            // (the puzzle rooms - area 1, j001_p01 - are the dungeon's own: neither way is "away")
            const bool local = r.warp.area < 0 || r.warp.area == app.area || r.warp.area == 1 || app.area == 1;
            lastTarget = r.warp.valid && local ? r.warp.map : -1;
            if (!r.warp.valid) return base;
            if (!mapsSeen.count(r.warp.map)) return 2;
            const bool away = !local; // (another area: the Ark - not held back by its many visits)
            return away ? 25 : 5 + std::min(visits[r.warp.map], 50); // (away: once the rooms about are seen ~20 times, or when stuck)
        };
        for (size_t i = 0; i < app.npcs.size(); ++i) {
            const cr::FieldNpc& n = app.npcs[i];
            if (!npcActive(app, n)) continue;
            const bool parked = (n.pos[1] >= 900.0f || (n.pos[0] == 0 && n.pos[1] == 0 && n.pos[2] == 0)) &&
                                !app.placedNpcs.count(n.id);
            if ((n.kind & 8) && !parked) {
                lastTarget = -1;
                cands.push_back({n.id, true, worldDoors.count({app.mapNumber, n.id}) ? 60 : rankOf(n.treasure & 3 ? n.id + 1 : n.id, 0)});
                if (lastTarget >= 0 && !deadExits.count({app.mapNumber, n.id * 2 + 1})) roomExits[app.mapNumber][n.id * 2 + 1] = lastTarget;
            }
            if (n.touchTrigger()) {
                cands.push_back({n.id, false, rankOf(n.id, 1)});
                if (lastTarget >= 0 && !deadExits.count({app.mapNumber, n.id * 2})) roomExits[app.mapNumber][n.id * 2] = lastTarget;
            }
        }
        // a talk or touch that leads nowhere, done here before (a lever, often): after the ways into new rooms
        for (Cand& c : cands)
            if (c.rank < 2 && triedEver.count({app.mapNumber, c.id * 2 + c.examine})) c.rank = 3;
        // (at the same rank walk-in triggers first: they're the rooms' ways on more often than doors)
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return a.rank != b.rank ? a.rank < b.rank : (!a.examine && b.examine);
        });
        if (app.puzzle.stage >= 0 && !app.game.flag(3000 + app.puzzle.stage) && !app.runner.running()) {
            // a puzzle room it can't play: solved on the spot (its clear flag), so the room's ways on open
            std::printf("     puzzle stage %d taken as solved (flag %d)\n", app.puzzle.stage, 3000 + app.puzzle.stage);
            app.game.setFlag(3000 + app.puzzle.stage, true);
            puzzle::enter(app); // the room again as solved: its crystal out (the way on)
            buildNpcDraws(app);
            tried.clear();
            continue;
        }
        const uint64_t key = stateKey();
        { // what's left here, for the way back when stuck elsewhere
            bool left = false;
            for (const Cand& c : cands) left |= !tried.count({app.mapNumber, c.id * 2 + c.examine, key}) && c.rank < 60;
            roomLeft[app.mapNumber] = {left, key};
        }
        if (const char* cm = SDL_getenv("CR_EXPLORE_CANDS"); cm && SDL_atoi(cm) == app.mapNumber) { // (debugging)
            std::printf("     candidates on map %d:", app.mapNumber);
            for (const Cand& c : cands)
                std::printf(" %s%d(r%d%s)", c.examine ? "t" : "x", c.id, c.rank, tried.count({app.mapNumber, c.id * 2 + c.examine, key}) ? ",tried" : "");
            std::printf("\n");
        }
        const Cand* pick = nullptr;
        for (const Cand& c : cands)
            if (!tried.count({app.mapNumber, c.id * 2 + c.examine, key})) { pick = &c; break; }
        if (!pick && app.puzzle.stage >= 0 && !app.game.flag(3000 + app.puzzle.stage)) {
            // a puzzle it can't solve: as if its crystal were reached (the clear flag), then on
            std::printf("     puzzle stage %d taken as solved (flag %d)\n", app.puzzle.stage, 3000 + app.puzzle.stage);
            app.game.setFlag(3000 + app.puzzle.stage, true);
            tried.clear();
            continue;
        }
        if (!pick && !escaped.count({app.mapNumber, key})) {
            // all tried here: once for this story state, a way out to another area (the Ark, most
            // often - least wanted by the ranking, as the room seen most), whatever was tried
            // ... else any way out (the ranking's best): back towards rooms with something untried
            const Cand* anyExit = nullptr;
            for (const Cand& c : cands) {
                cr::GameState trial = app.game;
                const cr::EventResult r = cr::runEvent(app.events, c.examine && (findNpc(app, c.id) && (findNpc(app, c.id)->treasure & 3))
                                                                      ? c.id + 1 : c.id, trial, app.area, app.mapNumber);
                if (!r.warp.valid || worldDoors.count({app.mapNumber, c.id})) continue;
                if (r.warp.area >= 0 && r.warp.area != app.area && r.warp.area != 1 && app.area != 1) { pick = &c; break; }
                if (!anyExit) anyExit = &c;
            }
            if (!pick) pick = anyExit;
            escaped.insert({app.mapNumber, key});
            if (pick) std::printf("     nothing new here: out again\n");
        }
        if (!pick) { // the nearest room (by the doors known) with something left - or seen in another state
            std::map<int, int> firstHop{{app.mapNumber, -1}};
            std::vector<int> queue{app.mapNumber};
            int goal = -1;
            for (size_t qi = 0; qi < queue.size() && goal < 0; ++qi) {
                const int at = queue[qi];
                for (const auto& [ck, to] : roomExits[at]) {
                    if (firstHop.count(to)) continue;
                    firstHop[to] = at == app.mapNumber ? ck : firstHop[at];
                    queue.push_back(to);
                    const auto rl = roomLeft.find(to);
                    if (rl == roomLeft.end() || rl->second.first || rl->second.second != key) { goal = to; break; }
                }
            }
            if (goal >= 0)
                for (const Cand& c : cands)
                    if (c.id * 2 + c.examine == firstHop[goal]) { pick = &c; break; }
            if (pick) std::printf("     towards map %d\n", goal), hopping = true;
        }
        if (!pick) { stuckReason = 1; break; }
        const int hopKey = pick->id * 2 + pick->examine;
        tried.insert({app.mapNumber, pick->id * 2 + pick->examine, key});
        triedEver.insert({app.mapNumber, pick->id * 2 + pick->examine});
        const int pickMap = app.mapNumber, pickId = pick->id;
        worldAsked = false;
        const int fromMap = app.mapNumber, flagsBefore = flagCount(), pagesBefore = pagesRead;
        const cr::GameState flagsWere = app.game;
        const size_t unsupportedBefore = app.unsupported.size();
        const cr::FieldNpc* n = findNpc(app, pick->id);
        if (!n || !app.havePlayer) continue;
        const char* traceStep = SDL_getenv("CR_EXPLORE_TRACE"); // debugging: every instruction of one step
        if (traceStep && SDL_atoi(traceStep) == step)
            cr::setEventTrace([](size_t pc, int op) { std::printf("    pc %#zx op %d\n", pc, op); });
        bool started;
        if (pick->examine) { // from each side of its box, facing it, until one starts it
            started = false;
            const float dx = n->box[0] / 2 + app.walker.radius, dz = n->box[2] / 2 + app.walker.radius;
            const float side[4][3] = {{dx, 0, 3.14159265f * 1.5f}, {-dx, 0, 3.14159265f * 0.5f}, {0, dz, 3.14159265f}, {0, -dz, 0}};
            // (only where it's the one in reach: two people side by side would answer for each other)
            for (int k = 0; k < 5 && !started; ++k) {
                app.walker.pos = k < 4 ? cr::Vec3{n->pos[0] + side[k][0], n->pos[1], n->pos[2] + side[k][1]}
                                       : cr::Vec3{n->pos[0], n->pos[1], n->pos[2]};
                if (k && k < 4) app.walker.heading = side[k][2];
                if (interactTarget(app) == n) started = interact(app);
            }
        } else {
            app.walker.pos = {n->boxCenterX(), n->pos[1], n->boxCenterZ()};
            app.lastKick = 0;
            checkTriggers(app);
            started = app.runner.running();
        }
        const int frames = settle(20000);
        cr::setEventTrace(nullptr);
        if (!started) {
            if (pick->examine) std::printf("     %d on map %d: out of reach\n", pick->id, app.mapNumber);
            continue;
        }
        if (app.mapNumber != fromMap) ++visits[app.mapNumber];
        mapsSeen.insert(app.mapNumber);
        std::string extra;
        for (size_t u = unsupportedBefore; u < app.unsupported.size(); ++u) extra += ", " + app.unsupported[u];
        if (app.runner.running()) extra += ", STILL RUNNING (" + app.runner.where() + ")";
        if (worldAsked) { // a terminal: tried last, and again while it has gates not taken in this state
            worldDoors.insert({pickMap, pickId});
            if (int(gatesTried[gateKey].size()) < worldCount) tried.erase({pickMap, hopKey, key});
        }
        if (hopping && app.mapNumber == pickMap) roomExits[pickMap].erase(hopKey), deadExits.insert({pickMap, hopKey}); // (it didn't lead there: forgotten)
        hopping = false;
        std::printf("%4d map %3d %s %4d: %+d flags, %d pages, %d frames%s%s\n", step, fromMap,
                    pick->examine ? "talk " : "touch", pick->id, flagCount() - flagsBefore, pagesRead - pagesBefore,
                    frames, app.mapNumber != fromMap ? (" -> map " + std::to_string(app.mapNumber)).c_str() : "",
                    extra.c_str());
        if (flagCount() != flagsBefore && SDL_getenv("CR_EXPLORE_FLAGS")) { // (debugging: which)
            std::printf("     flags:");
            for (int fl = 0; fl < int(app.game.flags.size()) * 32; ++fl)
                if (app.game.flag(fl) != flagsWere.flag(fl)) std::printf(" %s%d", app.game.flag(fl) ? "+" : "-", fl);
            std::printf("\n");
        }
        if (app.runner.running()) { app.runner.stop(); stuckReason = 2; break; }
    }
    std::printf("explore: area %d, puzzle stage %d\n", app.area, app.puzzle.stage);
    std::printf("explore: %d actions, %d frames, %d pages, %zu maps seen, %d flags set, ends on map %d (%s)\n", step,
                totalFrames, pagesRead, mapsSeen.size(), flagCount(), app.mapNumber,
                stuckReason == 1   ? "nothing new to try"
                : stuckReason == 2 ? "a script never finished"
                : stuckReason == 3 ? "THE END: the last battle's ending (flag 39)"
                : stuckReason == 4 ? "the title didn't start the next pair"
                                   : "action limit");
    std::printf("explore: flags set:");
    for (int fl = 0; fl < int(app.game.flags.size()) * 32; ++fl)
        if (app.game.flag(fl)) std::printf(" %d", fl);
    std::printf("\n");
    std::map<std::string, int> counts;
    for (const std::string& u : app.unsupported) counts[u.substr(0, u.find(" on map"))]++;
    for (const auto& [what, c] : counts) std::printf("  not implemented: %s x%d\n", what.c_str(), c);
    return 0;
}

// --capture <map.cmp> <out.bmp>: renders that map's composite (background + collision through
// the stage camera, no HUD) and saves the frame — for checking alignment without a person.
// --export-textures [dir] (without one, textures/export/ in the game folder): every distinct picture
// the game draws (its .mct files and the textures inside model packages) as <hash>.png (upright), for tools/hd_textures.py to upscale - or any editor - into textures/hd/.
// With --vita, the Vita's lossless version of each picture it has alike (same size, alpha kept, the
// same picture: within 12/255 a channel on average - not the art it redrew) is exported instead,
// and <ios hash>.cralias (the Vita's hash, 16 hex digits) lets the one upscale serve the iOS
// version too: the game finds it whichever it decoded
// --export-models [dir] (without one, models/export/ in the game folder): every model package as
// <name>.glb - its skeleton, meshes at rest skinned to it, materials, textures and motions (the
// package's and its <name>e*.mca files'), for editing and models/hd/ (assets/gltf_model.hpp)
int runExportModels(App& app, const char* dirArg) {
    std::string dir = dirArg && *dirArg ? dirArg : cr::modelsFolder("export");
    if (dir.back() != '/') dir += '/';
    if (!SDL_CreateDirectory(dir.c_str()) && !(dirArg && *dirArg)) dir = cr::saveDir() + "models/export/", SDL_CreateDirectory(dir.c_str());
    std::printf("exporting to %s\n", dir.c_str());
    int written = 0, failed = 0;
    size_t bytes = 0;
    for (const std::string& name : app.models) {
        std::vector<uint8_t> data;
        cr::Model m;
        std::string err;
        const bool fromVita = app.vita.isOpen() && app.vita.read(name, data) && cr::loadModelPackage(data, m, &err);
        if (!fromVita && (!cr::readFile(app.dataDir + name, data) || !cr::loadModelPackage(data, m, &err))) {
            std::printf("  %s: %s\n", name.c_str(), err.c_str());
            ++failed;
            continue;
        }
        std::vector<cr::Action> actions; // (as loadModelFile keeps them)
        const std::vector<bool> kept = packageActionsKept(m);
        for (size_t k = 0; k < m.actions.size(); ++k)
            if (kept[k]) actions.push_back(m.actions[k]);
        const std::string stem = name.substr(0, name.size() - 4);
        for (const auto& file : app.animFiles) {
            if (file.rfind(stem + "e", 0) != 0) continue;
            std::vector<uint8_t> ad;
            std::vector<cr::Action> acts;
            if (!cr::readFile(app.dataDir + file, ad) || !cr::parseMca(ad.data(), ad.size(), 0, acts)) continue;
            for (size_t i = 0; i < acts.size(); ++i)
                if (cr::actionFits(m.skeleton, acts[i])) {
                    acts[i].name = file.substr(0, file.size() - 4) + "." + std::to_string(i + 1);
                    actions.push_back(std::move(acts[i]));
                }
        }
        cr::GltfExportStats gs;
        if (cr::exportGlb(m, actions, dir + stem + ".glb", &err, &gs)) {
            ++written;
            bytes += gs.bytes;
            if (SDL_getenv("CR_EXPORT_VERBOSE"))
                std::printf("  %s: %d meshes, %d triangles, %d pictures, %d motions (%d channels), %zu KB\n", name.c_str(), gs.meshes,
                            gs.triangles, gs.images, gs.animations, gs.channels, gs.bytes / 1024);
        } else {
            std::printf("  %s: %s\n", name.c_str(), err.c_str());
            ++failed;
        }
    }
    std::printf("%d models written (%zu MB), %d failed\n", written, bytes >> 20, failed);
    return failed && !written ? 1 : 0;
}

int runExportTextures(App& app, const char* dirArg) {
    // (no folder given: textures/export/ in the game folder, else the profile folder's when that can't be written)
    std::string dir = dirArg && *dirArg ? dirArg : cr::texturesFolder("export");
    if (dir.back() != '/') dir += '/';
    if (!SDL_CreateDirectory(dir.c_str()) && !(dirArg && *dirArg)) dir = cr::saveDir() + "textures/export/", SDL_CreateDirectory(dir.c_str());
    std::printf("exporting to %s\n", dir.c_str());
    std::set<uint64_t> seen;
    int written = 0, failed = 0, dupes = 0, fromVita = 0, aliases = 0;
    auto put = [&](const cr::Image& img) {
        const uint64_t h = cr::imageHash(img);
        if (!seen.insert(h).second) {
            ++dupes;
            return h;
        }
        const std::string path = dir + cr::hashName(h) + ".png";
        if (SDL_GetPathInfo(path.c_str(), nullptr)) return h; // already exported
        if (cr::savePngImage(path, img)) ++written;
        else ++failed;
        return h;
    };
    // The Vita's picture for the iOS one, when it is the same picture losslessly
    auto alike = [](const cr::Image& ios, const cr::Image& vita) {
        if (ios.alphaOnly || ios.width != vita.width || ios.height != vita.height || ios.rgba.size() != vita.rgba.size()) return false;
        double sum = 0;
        for (size_t i = 0; i < ios.rgba.size(); ++i) sum += std::abs(int(ios.rgba[i]) - int(vita.rgba[i]));
        return sum / double(ios.rgba.size()) < 12;
    };
    auto pair = [&](const cr::Image& ios, const cr::Image* vita) {
        if (!vita || !alike(ios, *vita)) { // (the art the Vita redrew: both, each for its own)
            put(ios);
            if (vita) put(*vita);
            return;
        }
        const uint64_t v = put(*vita), i = cr::imageHash(ios);
        ++fromVita;
        if (i == v) return;
        const std::string path = dir + cr::hashName(i) + ".cralias", target = cr::hashName(v);
        if (SDL_SaveFile(path.c_str(), target.data(), target.size())) ++aliases;
    };
    for (const std::string& name : app.textures) {
        cr::Image ios, vita;
        std::vector<uint8_t> data;
        const bool haveIos = !app.vitaOnly && cr::readFile(app.dataDir + name, data) && cr::decodeMct(data, ios);
        bool haveVita = false;
        if (app.vita.isOpen() && name.size() > 4) {
            std::vector<uint8_t> g;
            haveVita = app.vita.read(name.substr(0, name.size() - 4) + ".gxt", g) && cr::decodeGxt(g, vita);
        }
        if (haveIos) pair(ios, haveVita ? &vita : nullptr);
        else if (haveVita) put(vita);
        else ++failed;
    }
    if (app.vita.isOpen() && !app.vitaOnly) // the Vita's own pictures (its warning screen, the titles in other languages ...)
        for (const std::string& n : app.vita.names()) {
            if (n.size() < 5 || n.compare(n.size() - 4, 4, ".gxt") != 0) continue;
            if (std::find(app.textures.begin(), app.textures.end(), n.substr(0, n.size() - 4) + ".mct") != app.textures.end()) continue;
            std::vector<uint8_t> g;
            cr::Image img;
            if (app.vita.read(n, g) && cr::decodeGxt(g, img)) put(img);
        }
    for (const std::string& name : app.models) {
        std::vector<uint8_t> data;
        cr::Model ios, vita;
        const bool haveIos = !app.vitaOnly && cr::readFile(app.dataDir + name, data) && cr::loadModelPackage(data, ios, nullptr);
        const bool haveVita = app.vita.isOpen() && app.vita.read(name, data) && cr::loadModelPackage(data, vita, nullptr);
        if (haveIos) {
            const bool paired = haveVita && vita.textures.size() == ios.textures.size();
            for (size_t k = 0; k < ios.textures.size(); ++k) pair(ios.textures[k], paired ? &vita.textures[k] : nullptr);
            if (haveVita && !paired)
                for (const cr::Image& img : vita.textures) put(img);
        } else if (haveVita)
            for (const cr::Image& img : vita.textures) put(img);
    }
    if (app.vita.isOpen() && !app.vitaOnly) // the Vita's own model packages (t0101_1..3.bin)
        for (const std::string& n : app.vita.names()) {
            if (n.size() < 5 || n.compare(n.size() - 4, 4, ".bin") != 0) continue;
            if (std::find(app.models.begin(), app.models.end(), n) != app.models.end()) continue;
            std::vector<uint8_t> data;
            cr::Model m;
            if (app.vita.read(n, data) && cr::loadModelPackage(data, m, nullptr))
                for (const cr::Image& img : m.textures) put(img);
        }
    std::printf("export: %zu distinct pictures (%d written, %d already there, %d repeats, %d failed; %d from the Vita, %d "
                "iOS hashes aliased to them) -> %s\n", seen.size(), written, int(seen.size()) - written - failed, dupes,
                failed, fromVita, aliases, dir.c_str());
    return failed ? 1 : 0;
}

int runCapture(App& app, const char* mapName, const char* outPath) {
    app.persist = false;
    app.trophy.live = false; // (no notifications of their own in captures: CR_CAPTURE_TROPHY shows one)
    if (const char* sz = SDL_getenv("CR_CAPTURE_SIZE")) { // CR_CAPTURE_SIZE=WxH: the window's size (e.g. 1280x720, a phone's 16:9)
        int cw = 0, ch = 0;
        if (std::sscanf(sz, "%dx%d", &cw, &ch) == 2 && cw >= 320 && ch >= 240) {
            SDL_SetWindowSize(app.window, cw, ch);
            SDL_SyncWindow(app.window);
        }
    }
    { // the logical screen as the first frame will have it (--frames runs step before drawing)
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(app.window, &ww, &wh);
        if (ww > 0 && wh > 0) app.renderer.fitScreen(ww, wh);
    }
    app.typeOn = SDL_getenv("CR_CAPTURE_TYPED") != nullptr; // CR_CAPTURE_TYPED=n: n frames of typing
    setMode(app, Mode::Maps);
    int idx = -1;
    for (size_t i = 0; i < app.maps.size(); ++i)
        if (app.maps[i] == mapName) idx = int(i);
    if (idx < 0) {
        std::printf("unknown map %s\n", mapName);
        return 1;
    }
    if (app.captureTitlePair) { // --title-new p: through the title as a player would
        app.saveDirOverride = userDir() + "capture-empty/"; // (no saves: New)
        bootTitle(app);
        for (int f = 0; f < 1000 && !app.titleOpen; ++f) stepEvents(app);
        // (CR_TITLE_NEW_STOP=chars / save: stays on Character Select / the save screen)
        const char* stop = SDL_getenv("CR_TITLE_NEW_STOP");
        const std::string stopAt = stop ? stop : "";
        titleChoose(app, 0);
        for (int f = 0; f < 300 && !app.charSelectOpen; ++f) stepEvents(app);
        app.charSelectCursor = app.captureTitlePair - 1;
        if (stopAt != "chars") {
            charSelectKey(app, SDLK_RETURN);
            for (int f = 0; f < 300 && !app.saveScreen.open; ++f) stepEvents(app);
            if (stopAt != "save") {
                saveScreenKey(app, SDLK_RETURN);
                saveScreenKey(app, SDLK_RETURN);
                app.saveDirOverride.clear();
            } else app.saveScreen.anim = 100; // (faded in: a capture draws one frame)
        } else app.charSelectAnim = 100;
    } else if (app.captureTitle) bootTitle(app); // --title: the boot script, --frames n into it
    else if (app.newGame) startNewGame(app);
    else {
        for (int f : app.captureFlags) app.game.setFlag(f, true);
        captureVars(app);
        std::vector<int> numbers = cr::mapNumbersFor(mapName);
        if (const char* cm = SDL_getenv("CR_CAPTURE_MAP")) numbers.insert(numbers.begin(), SDL_atoi(cm)); // (maps sharing a file)
        if (app.captureArea >= 0 && !numbers.empty()) showMap(app, idx, numbers[0], app.captureArea);
        else showMap(app, idx);
    }
    app.showBackground = true;
    app.hideHud = SDL_getenv("CR_CAPTURE_HUD") == nullptr; // (CR_CAPTURE_HUD=1: as the build shows it)
    if (app.captureEvent >= 0) { // --event n: as if something in the room had started it
        app.pendingEntry = App::Entry::None;
        app.runner.stop();
        captureVars(app); // (again: the map's own entry may have used them as counters)
        if (const char* at = SDL_getenv("CR_CAPTURE_AT")) { // (an address in the area's file instead: a branch reached by state)
            if (!app.runner.startAt(app.events, uint32_t(std::strtoul(at, nullptr, 0)), app.captureEvent, app.game, app.area, app.mapNumber))
                std::printf("area %d: no code at %s\n", app.area, at);
        } else if (!app.runner.start(app.events, app.captureEvent, app.game, app.area, app.mapNumber))
            std::printf("area %d has no script %d\n", app.area, app.captureEvent);
    }
    const bool traceRunner = SDL_getenv("CR_CAPTURE_TRACE") != nullptr; // the script's state each frame it changes
    std::string lastWhere;
    int lastMsgShown = -1;
    // CR_CAPTURE_UNTIL_MSG="id[:n[:p[:area]]]": stop the --frames run n frames (60 if not given) after
    // message id comes up, with no other taps meanwhile (p of its pages turned first, 45 frames apart;
    // only in that area - ids repeat across the areas' files)
    int untilMsg = -1, untilAfter = 60, untilPages = 0, untilArea = -1, untilLeft = -1;
    if (const char* um = SDL_getenv("CR_CAPTURE_UNTIL_MSG"))
        std::sscanf(um, "%d:%d:%d:%d", &untilMsg, &untilAfter, &untilPages, &untilArea);
    for (int f = 0; f < app.captureFrames; ++f) { // --frames: play the scripts, reading each page for 45 frames
        if (untilMsg >= 0 && untilLeft < 0 && app.lastMessageId == untilMsg && !app.dialogue.empty() &&
            (untilArea < 0 || app.area == untilArea)) {
            untilLeft = untilAfter + 45 * untilPages;
            std::printf("frame %d: message %d up\n", f, untilMsg);
        }
        if (untilLeft > untilAfter && (untilLeft - untilAfter) % 45 == 0 && !app.dialogue.empty()) advanceDialogue(app);
        if (untilLeft >= 0 && untilLeft-- == 0) break;
        if (app.battle.active && app.captureWinBattles) { // its talks play; then it's won
            stepBattle(app);
            if (traceRunner && f % 100 == 0)
                std::printf("frame %d: battle phase %d frames %d flag %d\n", f, int(app.battle.phase), app.battle.phaseFrames,
                            int(app.game.flag(battletalk::kFlag)));
            if (traceRunner && app.battle.phaseFrames == 1)
                std::printf("frame %d: battle %d phase %d%s\n", f, app.battle.start.group, int(app.battle.phase),
                            battletalk::on(app) ? " talk" : "");
            if (!battletalk::on(app) && app.battle.phase == App::Battle::Command) {
                for (int k = 2; k < 5; ++k) app.battle.rules.f[size_t(k)].hp = 0;
                endBattle(app, true);
            }
            if (f % 45 == 44 && !app.dialogue.empty()) advanceDialogue(app);
            continue;
        }
        stepEvents(app);
        if (traceRunner) {
            const std::string where = (app.runner.running() ? app.runner.where()
                                                             : "done (" + std::to_string(app.runner.result().instructions) +
                                                                   " instructions" +
                                                                   (app.runner.result().error.empty() ? "" : ", " + app.runner.result().error) +
                                                                   ")") + " map " +
                                      std::to_string(app.mapNumber) + " (" + app.runnerWhat + ")" +
                                      (app.battle.active ? " battle " + std::to_string(app.battle.start.group) + " phase " +
                                                               std::to_string(int(app.battle.phase)) +
                                                               (battletalk::on(app) ? " talk" : "")
                                                         : std::string()) +
                                      " gate " + std::to_string(app.runner.gate());
            if (where != lastWhere) std::printf("frame %d: %s\n", f, where.c_str());
            if (app.lastMessageId != lastMsgShown) std::printf("frame %d: message %d (area %d)\n", f, app.lastMessageId, app.area);
            lastMsgShown = app.lastMessageId;
            lastWhere = where;
        }
        animateNpcs(app, 1.0f / kFramesPerSecond);
        followPlayerScroll(app); // (drawFrame's, every frame the game shows: a capture draws only its last)
        if (f % 45 == 44 && f + 1 < app.captureFrames && !SDL_getenv("CR_CAPTURE_NOTAP") && untilLeft < 0) { // (CR_CAPTURE_NOTAP: no taps)
            if (app.choiceFlag >= 0) answerChoice(app, true);
            else if (app.worldChoiceOpen) answerWorldChoice(app, 0);
            else advanceDialogue(app);
            app.tapPending = true;
            if (app.itemWindowOpen) app.game.setFlag(0, true);
        }
    }
    if (app.captureTalk >= 0) { // the opening to its end, then the hall's people until a portrait is up
        for (int f = 0; f < 20000 && (app.runner.running() || app.pendingEntry != App::Entry::None); ++f) {
            stepEvents(app);
            app.dialogue.clear();
            if (app.choiceFlag >= 0) answerChoice(app, true);
            app.tapPending = true;
        }
        int found = -1;
        std::vector<int> ids;
        for (const cr::FieldNpc& n : app.npcs)
            if ((n.kind & 8) && npcActive(app, n) && n.pos[1] < 900.0f) ids.push_back(n.id);
        for (int id : ids) {
            const cr::FieldNpc* n = findNpc(app, id);
            if (!n || !app.havePlayer) continue;
            app.walker.pos = {n->pos[0] + n->box[0] / 2 + app.walker.radius, n->pos[1], n->pos[2]};
            if (!interact(app)) continue;
            for (int f = 0; f < 6000 && app.runner.running(); ++f) {
                stepEvents(app);
                bool up = false;
                for (const App::Portrait& p : app.portraits) up = up || (p.tex.id && p.alpha > 0.99f && p.t >= p.frames && p.aT >= p.aFrames);
                if (up && !app.dialogue.empty() && ++found == app.captureTalk) break;
                if (!app.dialogue.empty()) advanceDialogue(app);
                if (app.choiceFlag >= 0) answerChoice(app, true);
                app.tapPending = true;
            }
            if (found >= app.captureTalk) {
                std::printf("talk-portrait: npc %d\n", id);
                break;
            }
            app.runner.stop();
        }
        if (found < app.captureTalk) std::printf("talk-portrait: only %d moments with a portrait\n", found + 1);
    }
    if (app.vita.isOpen()) std::printf("vita textures used: %d\n", app.vitaTextures);
    if (app.android.isOpen()) std::printf("android textures used: %d\n", app.androidTextures);
    if (app.video.hdTextures) std::printf("hd textures: %d replaced, %d without one (%s)\n", app.hdLoaded, app.hdMissing, app.hdDir.c_str());
    if (app.captureTitle) {
        std::printf("title: script %s, title menu %s, fade grey %.2f alpha %.2f", app.runner.running() ? app.runner.where().c_str() : "done",
                    app.titleOpen ? "open" : "closed", app.fadeGrey, screenFadeAlpha(app));
        for (int i = 0; i < 16; ++i)
            if (app.portraits[i].tex.id) std::printf(", slot %d image %d (%d x %d) alpha %.2f at (%.0f %.0f)", i, app.portraits[i].image, app.portraits[i].tex.width,
                                                    app.portraits[i].tex.height, app.portraits[i].alpha, app.portraits[i].x, app.portraits[i].y);
        std::printf("\n");
    }
    if (app.captureFrames) {
        const size_t pi = std::min(app.dialoguePage, app.dialogue.size() ? app.dialogue.size() - 1 : 0);
        const cr::MessageInfo mi = pi < app.dialogueInfo.size() ? app.dialogueInfo[pi] : cr::MessageInfo();
        int shown = 0;
        for (const App::Portrait& p : app.portraits) shown += p.tex.id && p.alpha > 0.5f;
        std::printf("after %d frames: area %d, map %d, %s, %zu dialogue pages open (name plate %d, side %d), %d portraits shown, "
                    "fade %.0f%%\n", app.captureFrames, app.area, app.mapNumber,
                    app.runner.running() ? app.runner.where().c_str() : "scripts done", app.dialogue.size(), mi.nameId,
                    mi.side, shown, screenFadeAlpha(app) * 100);
        std::printf("field: %s, background %dx%d, %zu stage cameras (%s), scroll (%.0f, %.0f)%s\n",
                    app.mapIndex >= 0 && app.mapIndex < int(app.maps.size()) ? app.maps[size_t(app.mapIndex)].c_str() : "?",
                    app.background.width, app.background.height, app.stageCams.size(),
                    scrollingField(app) ? "a scrolling painting" : paintedField(app) ? "a painting shown whole" : "3D",
                    app.scroll.offX, app.scroll.offY, app.cam.free ? ", camera free" : "");
        if (SDL_getenv("CR_CAPTURE_NPCS")) // (debugging) the room's NPCs as the scripts left them
            for (size_t i = 0; i < app.npcs.size(); ++i) {
                const cr::FieldNpc& n = app.npcs[i];
                std::printf("npc %d kind %#x at (%.1f %.1f %.1f) active %d visible %d\n", n.id, n.kind, n.pos[0], n.pos[1], n.pos[2],
                            int(npcActive(app, n)), int(npcVisible(app, i)));
            }
        if (SDL_getenv("CR_CAPTURE_TRACE")) {
            const cr::StageCamera& sc = app.stageCam;
            std::printf("camera: stage %s fov %.1f view %dx%d; script %s eye (%.1f %.1f %.1f) target (%.1f %.1f %.1f)\n",
                        app.haveStageCam ? "yes" : "no", sc.fovDegrees, sc.viewWidth, sc.viewHeight,
                        app.cam.free && app.cam.set ? "on" : "off", app.cam.eye.x, app.cam.eye.y, app.cam.eye.z,
                        app.cam.target.x, app.cam.target.y, app.cam.target.z);
        }
    }
    if (app.captureSceneFx[0] >= 0) { // --scene-fx set effect [frames]: that weather / backdrop on
        loadSceneEffectSet(app, app.captureSceneFx[0]);
        setSceneEffect(app, true, app.captureSceneFx[1]);
        for (int f = 0; f < app.captureSceneFx[2]; ++f) stepSceneEffects(app);
        std::printf("scene fx: set %d, effect %d on, %d frames, drift %d rise %d\n", app.fx.set, app.captureSceneFx[1],
                    app.captureSceneFx[2], app.fx.spaceDrift, app.fx.spaceRise);
    }
    if (app.captureBattle[0] > 0) { // --battle group stage: that battle, a second in
        cr::ensureStartingGenes(app.game);
        if (app.captureBattleView == "genes" || app.captureBattleView == "genehit" || app.captureBattleView.rfind("reflect", 0) == 0) { // a plate with known genes
            app.game.items[481] = 1, app.game.items[108] = 1, app.game.items[94] = 1;
            app.game.party[0].genes[1] = 481;
        }
        if (!SDL_getenv("CR_CAPTURE_TUTORIALS")) // the battles' tutorials seen (else the first one holds the view)
            for (int f : {0xff1, 0xff2, 0xff3, 0xff4, 0xff5, 0xfa8, 0xfa9}) app.game.setFlag(f, true);
        // (CR_BATTLE_STRIKE: 1 the party strikes first, 2 an ambush)
        startBattle(app, {app.captureBattle[0], app.captureBattle[1], -1, SDL_getenv("CR_BATTLE_STRIKE") ? SDL_atoi(SDL_getenv("CR_BATTLE_STRIKE")) : 0}, 0);
        // --battle-view top|commands|targets|items|escape|hit|results: a screen of it; intro@n: the
        // opening n battle frames in (btl_main 200 at kIntroWhite)
        const std::string& v = app.captureBattleView;
        App::Battle& b = app.battle;
        const int introAt = v.rfind("intro@", 0) == 0 ? SDL_atoi(v.c_str() + 6) : -1;
        // top@n: n frames after the first page starts in (0: set at its distance, not drawn yet)
        const int topAt = v.rfind("top@", 0) == 0 ? SDL_atoi(v.c_str() + 4) : -1;
        if (topAt >= 0) {
            for (int f = 0; f < 600 && b.active && b.viewState != 1; ++f) stepBattle(app);
            for (int f = 0; f < topAt && b.active; ++f) stepBattle(app);
            std::printf("battle view %s: slide %d of %d, state %d\n", v.c_str(), b.slide, battleSlideDistance(app), b.viewState);
        }
        for (int f = 0; topAt < 0 && f < (introAt >= 0 ? introAt : 30) && app.battle.active; ++f) stepBattle(app);
        for (int f = 0; introAt < 0 && topAt < 0 && f < 10 && b.active && b.phase != App::Battle::Command; ++f) stepBattle(app);
        // (the page in, as a player would have it before tapping)
        for (int f = 0; introAt < 0 && topAt < 0 && f < 10 && b.active && b.phase == App::Battle::Command && !battleMenuReady(app); ++f) stepBattle(app);
        if (v == "items") app.game.items[21] = 3, app.game.items[22] = 1, app.game.items[24] = 2;
        if (v == "commands" || v == "targets" || v == "items") battleChooseTop(app, 0);
        if (v == "targets") battleChooseCommand(app, 0);
        if (v == "items") battleChooseCommand(app, 2);
        if (v == "escape") battleChooseTop(app, 2);
        if (v == "options") battleChooseTop(app, 3), app.optionsAnim = 100;
        if (v.rfind("break@", 0) == 0) { // break@n: the Break effect (btlbreak_effect_prc), its n-th frame
            startBreakFx(app);
            for (int f = 0, n = SDL_atoi(v.c_str() + 6); f < n; ++f) stepBreakFx(app);
        }
        if (v.rfind("effect", 0) == 0) { // effect<id>[@frame]: that effect on the first enemy, some frames in
            const int id = SDL_atoi(v.c_str() + 6);
            const size_t at = v.find('@');
            const int frames = at == std::string::npos ? 8 : SDL_atoi(v.c_str() + at + 1);
            setBattleCamera(app, b.formation + 12);
            spawnEffect(app, id, 2);
            for (int f = 0; f < frames; ++f) stepBattle(app);
            std::printf("battle view %s: %zu effects playing\n", v.c_str(), b.effects.size());
        }
        if (v.rfind("stage", 0) == 0) { // stage<action>[@frame]: the first enemy's staged action, that frame of it
            const int action = SDL_atoi(v.c_str() + 5);
            const size_t at = v.find('@');
            const int frame = at == std::string::npos ? -1 : SDL_atoi(v.c_str() + at + 1);
            b.rules.forceAction = action;
            for (int f = 0; f < 6000 && b.active; ++f) {
                if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) { // the pair defends
                    battleChooseTop(app, 0);
                    battleChooseCommand(app, 3);
                    if (b.menu == App::Battle::Commands) battleChooseCommand(app, 3);
                }
                if (b.turnOn && b.turn.action == action && b.turnFrame >= (frame >= 0 ? frame : b.hitFrame + 6)) break;
                if (b.phase == App::Battle::Won || b.phase == App::Battle::Lost) break;
                stepBattle(app);
            }
            std::printf("battle view %s: action %d frame %d (hit %d), camera %d (%d so far), flash %.0f, quake %.1f %.1f, "
                        "%zu effects, hp %d/%d %d/%d, user slot %d\n", v.c_str(), b.turn.action, b.turnFrame, b.hitFrame, b.cam, b.stageCams,
                        b.flash.on ? b.flash.alpha : 0.0f, b.quake.off[0], b.quake.off[1], b.effects.size(), b.rules.f[0].hp,
                        b.rules.f[0].hpMax, b.rules.f[1].hp, b.rules.f[1].hpMax,
                        battleActor(app, b.turn.actors[0]) ? battleActor(app, b.turn.actors[0])->slot : -1);
            if (SDL_getenv("CR_BATTLE_DEBUG"))
                if (const App::Battle::Actor* u = battleActor(app, b.turn.actors[0])) {
                    const LoadedModel& lm = b.models[u->model];
                    std::printf("user %s: %d built-in of %zu actions:", u->model.c_str(), lm.builtin, lm.actions.size());
                    for (const auto& ac : lm.actions) std::printf(" '%s' (%.0f)", ac.name.c_str(), ac.duration);
                    std::printf("\n");
                }
        }
        // reflect@n: genehit against a barrier on every enemy, n frames into the gene's turn
        const bool reflect = v.rfind("reflect", 0) == 0;
        const int reflectAt = reflect && v.find('@') != std::string::npos ? SDL_atoi(v.c_str() + v.find('@') + 1) : 60;
        if (reflect)
            for (int i = 2; i < 5; ++i) b.rules.f[size_t(i)].status[cr::kStatusBarrier] = 5;
        if (v == "genes" || v == "genehit" || reflect) battleChooseTop(app, 0), battleChooseCommand(app, 1);
        if (reflect) {
            for (size_t i = 0; i < b.picks.size(); ++i)
                if (b.picks[i] == 94) b.pick = int(i);
            battleChooseGene(app, b.picks[size_t(b.pick)]);
            if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks[0]);
            if (b.menu == App::Battle::Commands) battleChooseCommand(app, 3);
            for (int f = 0; f < 2000 && b.active; ++f) {
                if (b.turnOn && b.turn.kind == cr::kCmdGene && b.turnFrame >= reflectAt) break;
                stepBattle(app);
            }
            std::printf("battle view reflect: frame %d, reflected %d, back at %d, hits at %d (shown %d), escher hp %d/%d, enemy hp %d/%d\n",
                        b.turnFrame, int(b.turn.reflected), b.bounceAt, b.hitFrame, int(b.hitsShown), b.rules.f[0].hp,
                        b.rules.f[0].hpMax, b.rules.f[2].hp, b.rules.f[2].hpMax);
        }
        if (v == "genehit") {
            for (size_t i = 0; i < b.picks.size(); ++i)
                if (b.picks[i] == 94) b.pick = int(i);
            battleChooseGene(app, b.picks[size_t(b.pick)]);
            if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks[0]);
            if (b.menu == App::Battle::Commands) battleChooseCommand(app, 3); // Musiea defends
            for (int f = 0; f < 2000 && b.active; ++f) {
                if (b.turnOn && b.turn.kind == cr::kCmdGene && b.hitsShown && b.turnFrame >= b.hitFrame + 6) break;
                stepBattle(app);
            }
            std::printf("battle view genehit: escher mp %d, enemy hp %d/%d\n", b.rules.f[0].mp, b.rules.f[2].hp, b.rules.f[2].hpMax);
        }
        const int hitWanted = v.rfind("hit", 0) == 0 ? std::max(1, SDL_atoi(v.c_str() + 3)) : 0;
        int hitsSeen = 0;
        bool wasShown = false;
        // results[@frames][:taps]: the results that many frames in, after that many taps
        // (bossdown@n: n frames into the boss's end; won@n: into the victory)
        const bool won = v.rfind("won", 0) == 0;
        const bool results = v.rfind("results", 0) == 0 || v.rfind("bossdown", 0) == 0 || won;
        const bool bossDown = v.rfind("bossdown", 0) == 0 || won;
        int resultFrames = 10, resultTaps = 0;
        if (results) {
            if (const size_t at = v.find('@'); at != std::string::npos) resultFrames = SDL_atoi(v.c_str() + at + 1);
            if (const size_t c = v.find(':'); c != std::string::npos) resultTaps = SDL_atoi(v.c_str() + c + 1);
        }
        if (hitWanted || results) {
            // pair attacks on the first enemy standing until the first hit lands (or the battle's won)
            for (int f = 0; f < 6000 && b.active; ++f) {
                if (b.phase == App::Battle::Command && b.menu == App::Battle::Top) {
                    battleChooseTop(app, b.rules.f[0].alive() && b.rules.f[1].alive() ? 1 : 0);
                    battleChooseCommand(app, 0);
                    if (b.menu == App::Battle::Targets) battleChooseTarget(app, b.picks[0]);
                    if (b.menu == App::Battle::Commands) { battleChooseCommand(app, 0); battleChooseTarget(app, b.picks[0]); }
                }
                if (bossDown && (b.phase == App::Battle::Command || b.phase == App::Battle::Acting)) // (the test: the pair holds, the monsters fall to a blow)
                    for (int k = 0; k < 5; ++k)
                        if (b.rules.f[size_t(k)].alive()) {
                            b.rules.f[size_t(k)].hp = k < 2 ? b.rules.f[size_t(k)].hpMax : 1;
                            if (k < 2) b.rules.f[size_t(k)].status[cr::kStatusInvincible] = 9;
                        }
                if (results) {
                    if (bossDown && !won && b.phase == App::Battle::BossDown && b.phaseFrames >= resultFrames) break;
                    if (bossDown && !won && b.phase == App::Battle::Won) break;
                    if (won && b.phase == App::Battle::Won && b.phaseFrames >= resultFrames) break;
                    if (b.phase == App::Battle::Results && b.result.frames >= resultFrames) break;
                    if (b.phase == App::Battle::Lost) break;
                } else {
                    if (b.turnOn && b.hitsShown && !wasShown) ++hitsSeen;
                    wasShown = b.turnOn && b.hitsShown;
                    if (hitsSeen >= hitWanted && b.hitsShown && b.turnFrame >= b.hitFrame + 6) break;
                    if (b.phase == App::Battle::Won || b.phase == App::Battle::Lost) break;
                }
                stepBattle(app);
            }
            for (int t = 0; t < resultTaps && b.phase == App::Battle::Results; ++t) {
                resultsTap(app);
                for (int f = 0; f < 6; ++f) stepBattle(app);
            }
            if (results)
                std::printf("results: stage %d, line %d, exp shown %.0f of %d, oz %.0f of %d, %zu level-ups, %zu items\n",
                            b.result.stage, b.result.message, b.result.shownExp, b.result.expTotal, b.result.shownOz,
                            b.result.ozTotal, b.result.ups.size(), b.result.items.size());
            std::printf("battle view %s: phase %d (frame %d, boss model %d, camera %d), round %d, break %d, hp", v.c_str(), int(b.phase),
                        b.phaseFrames, b.bossModel, b.cam, b.rules.round, b.rules.breakGauge);
            for (int i = 0; i < 5; ++i) std::printf(" %d/%d", b.rules.f[size_t(i)].hp, b.rules.f[size_t(i)].hpMax);
            std::printf(", exp %d oz %d\n", b.rules.expGained, b.rules.ozGained);
        }
        if (SDL_getenv("CR_BATTLE_DEBUG"))
            for (const App::Battle::Actor& a : app.battle.actors) {
                LoadedModel& lm = app.battle.models[a.model];
                const cr::Action* act = a.action >= 0 && a.action < int(lm.actions.size()) ? &lm.actions[size_t(a.action)] : nullptr;
                cr::computeWorld(lm.cpu.skeleton, act, a.time, lm.world);
                cr::Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
                for (const auto& mesh : lm.cpu.meshes) {
                    cr::skinMesh(lm.cpu, mesh, lm.world, lm.skinned);
                    for (size_t v = 0; v + 2 < lm.skinned.size(); v += 3) {
                        lo = {std::min(lo.x, lm.skinned[v]), std::min(lo.y, lm.skinned[v + 1]), std::min(lo.z, lm.skinned[v + 2])};
                        hi = {std::max(hi.x, lm.skinned[v]), std::max(hi.y, lm.skinned[v + 1]), std::max(hi.z, lm.skinned[v + 2])};
                    }
                }
                std::printf("actor %s at (%.0f %.0f) action %d '%s' t=%.0f: posed bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n",
                            a.model.c_str(), a.pos.x, a.pos.z, a.action, act ? act->name.c_str() : "-", a.time, lo.x, lo.y,
                            lo.z, hi.x, hi.y, hi.z);
            }
    }
    if (app.captureMessage > 0) showMessages(app, {app.captureMessage}); // --message <id>
    if (const char* st = SDL_getenv("CR_CAPTURE_STYLE")) app.msgStyle = SDL_atoi(st), app.msgLines = 2; // Open's style
    if (const char* pg = SDL_getenv("CR_CAPTURE_PAGE"))
        app.dialoguePage = size_t(std::clamp(SDL_atoi(pg), 0, std::max(0, int(app.dialogue.size()) - 1)));
    if (SDL_getenv("CR_CAPTURE_LINES") && !app.dialogue.empty()) // the page's lines' first bytes (tests)
        for (const std::string& l : app.dialogue[std::min(app.dialoguePage, app.dialogue.size() - 1)])
            std::printf("line: %02x %02x %02x len %zu\n", l.size() > 0 ? (unsigned char)l[0] : 0,
                        l.size() > 1 ? (unsigned char)l[1] : 0, l.size() > 2 ? (unsigned char)l[2] : 0, l.size());
    if (const char* ty = SDL_getenv("CR_CAPTURE_TYPED")) { // the page n frames into its typing
        resetTyping(app);
        if (const cr::MessageInfo* info = app.dialogue.empty() ? nullptr : pageInfo(app)) // (a page keeping the lines before it)
            app.dialogueTyped = info->keep, app.dialogueInstant = info->instant;
        for (int f = SDL_atoi(ty); f > 0 && !app.dialogue.empty(); --f) stepTyping(app);
        std::printf("typed: %d of %d characters\n", app.dialogueTyped,
                    app.dialogue.empty() ? 0 : pageChars(app.dialogue[std::min(app.dialoguePage, app.dialogue.size() - 1)]));
    }
    if (const char* tr = SDL_getenv("CR_CAPTURE_TROPHY")) { // "n[:seconds]": trophy n's notification, seconds in (1 = held)
        int n = 1;
        float at = 1;
        std::sscanf(tr, "%d:%f", &n, &at);
        loadTrophyPack(app);
        app.trophy.notice.start(std::clamp(n, 0, cr::trophies::kCount - 1));
        app.trophy.notice.t = std::max(0.0f, at);
        app.trophy.hold = true; // (as it is: the capture's own frame doesn't move it)
        std::printf("trophy notification: %d, %.2f s, in %.2f, pack %s, picture %s\n", n, at, app.trophy.notice.shown(),
                    app.trophy.packRead ? "read" : "not found", app.trophy.icons[size_t(app.trophy.notice.id)].id ? "yes" : "no");
    }
    if (const char* ch = SDL_getenv("CR_CAPTURE_CHOICE")) { // the event Yes / No over it (Util 3), settled
        app.msgStyle = 64, app.msgLines = 1;
        app.choiceFlag = 0, app.choiceCursor = 0, app.choiceAnim = 4;
        if (SDL_strcmp(ch, "touch") == 0) app.pointerLast = true; // (touch play's look)
    }
    if (!app.captureShop.empty()) { // the Ark's shop (area 7), with some Oz and things to sell
        if (app.area != 7 && app.mapNumber >= 97) {
            const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[app.mapNumber]));
            if (it != app.maps.end()) showMap(app, int(it - app.maps.begin()), app.mapNumber, 7);
        }
        app.game.oz = 45070;
        app.game.items[21] = 3;
        app.game.items[1] = 1;
        app.game.items[202] = 1;
        openShop(app);
        App::Shop& s = app.shop;
        const std::string& p = app.captureShop; // main category buy items sell count equip talk
        if (p == "talk") {
            shopChooseMain(app, 0);
            for (int f = 0; f < 40; ++f) { stepShop(app); stepEvents(app); }
        } else if (p != "main") {
            shopChooseMain(app, p == "sell" ? 2 : 1);
            if (p != "category") shopChooseCategory(app, p == "sell" || p == "items" ? 0 : 1);
            if (s.phase == App::Shop::Member && p != "member") shopChooseMember(app, 0); // (member: the pair's plates)
            if (p == "count" || p == "equip") {
                shopChooseItem(app, 1);
                if (p == "count") s.count = 2;
                else { s.count = 1; shopPay(app); }
            } else if (p == "buy") {
                s.listCursor = 1; // Bastard Sword: Attack up
            }
        }
        s.anim = 100;
        for (int f = 0; f < 12; ++f) stepShop(app), s.anim = 100; // (the list settled on its line)
        std::printf("shop: level %d, %zu in the list, phase %d, %d oz\n", s.level, s.list.size(), int(s.phase), app.game.oz);
    }
    if (app.puzzle.stage >= 0) {
        int counts[11] = {};
        for (const App::Puzzle::Obj& o : app.puzzle.objs) ++counts[std::clamp(o.type, 0, 10)];
        std::printf("puzzle: stage %d (map %d), ability %d, ESP %d, %zu objects: %d blocks, %d fixed, %d gems, %d switches, %d crystals\n",
                    app.puzzle.stage, app.mapNumber, cr::kPuzzleStage[app.puzzle.stage].ability, app.puzzle.esp,
                    app.puzzle.objs.size(), counts[1], counts[2], counts[9], counts[8], counts[10]);
    }
    if (!app.captureTouchPad.empty()) { // --touch-pad dx,dy[,chest]: the pad held from the screen's middle (tex2d)
        float dx = 0, dy = 0;
        SDL_sscanf(app.captureTouchPad.c_str(), "%f,%f", &dx, &dy);
        app.fieldMenu.fade = 320;
        if (app.captureTouchPad.find("chest") != std::string::npos) // (beside the first chest: the "!" button)
            for (const cr::FieldNpc& n : app.npcs)
                if ((n.treasure & 3) && npcActive(app, n)) {
                    app.walker.pos = {n.pos[0] + n.box[0] / 2 + app.walker.radius, n.pos[1], n.pos[2]};
                    break;
                }
        App::TouchPad& t = app.touchPad;
        t = App::TouchPad();
        t.down = t.shown = true, t.ox = 240, t.oy = 200, t.dx = dx, t.dy = dy;
        for (int f = 0; f < 12; ++f) stepTouchPad(app);
        float sx = 0, sy = 0;
        bool run = false;
        const bool moving = touchPadStick(app, sx, sy, run);
        std::printf("touch pad: %s, offset (%g %g), knob (%.1f %.1f), %s pad art, \"!\" button %s\n",
                    moving ? (run ? "running" : "walking") : "standing", dx, dy, t.knob[0], t.knob[1],
                    app.bigPad ? "the larger" : "the small", easyButtonShown(app) ? "shown" : "hidden");
    }
    if (!app.captureFieldMenu.empty()) { // --field-menu closed|open|known|off: the corner button, the bar
        const std::string& v = app.captureFieldMenu;
        if (v != "closed") app.fieldMenu.open = true;
        if (v == "known" || v == "off") app.game.items[170] = 1;
        if (v == "off") app.game.noEncounters[size_t(fieldRoute(app))] = true;
        app.fieldMenu.fade = 320;
        if (v.rfind("map", 0) == 0) { // map[step]: the map screen, the story at that step
            if (v.size() > 3) app.game.setFlag(mapui::kFlagBase + mapui::kFlagStride * fieldRoute(app) + SDL_atoi(v.c_str() + 3), true);
            openMapScreen(app);
            app.mapScreen.fade = 4;
            const App::MapScreen& ms = app.mapScreen;
            std::printf("map screen: map %d, gate %d, texture %dx%d, spot (%d %d), marks (%d %d) (%d %d)\n", app.mapNumber, ms.gate,
                        ms.tex.width, ms.tex.height, ms.spot[0], ms.spot[1], ms.marks[0][0], ms.marks[0][1], ms.marks[1][0],
                        ms.marks[1][1]);
        }
        std::printf("field menu: %s, %s, route %d encounters %s\n", fieldMenuWanted(app) ? "shown" : "hidden",
                    app.fieldMenu.open ? "open" : "closed", fieldRoute(app), fieldEncountersOff(app) ? "off" : "on");
    }
    if (app.captureMenu == 1) {
        // --camp-view items|use|equip|slots|wear|status|genes|plate|entries: a page, with a bag to show
        // (entries: the first plate's known entries, Genes 4); open@n / back@n: n frames after the main page's
        // Items is chosen / after Back from it
        const std::string& v0 = app.captureCampView;
        const size_t at = v0.find('@');
        const std::string v = v0.substr(0, at);
        const int frames = at == std::string::npos ? -1 : SDL_atoi(v0.c_str() + at + 1);
        if (!v.empty()) app.game.setFlag(0x38, true), app.game.setFlag(0x39, true); // (the once-only messages read)
        openCamp(app);
        app.camp.anim = 100;
        if (v == "open" || v == "back") {
            app.game.items[3] = 4, app.game.items[24] = 2, app.game.items[21] = 3;
            app.camp.cursor = 0;
            campKey(app, SDLK_RETURN);
            if (v == "back") {
                for (int f = 0; f < 12; ++f) stepCamp(app);
                campKey(app, SDLK_ESCAPE);
            }
            for (int f = 0; f < frames; ++f) stepCamp(app);
            std::printf("camp: %s@%d, page %d, anim %d, pageOut %d, ease %.2f\n", v.c_str(), frames, int(app.camp.page), app.camp.anim,
                        app.camp.pageOut, campEase(app));
        } else if (!v.empty()) {
            app.game.items[3] = 4, app.game.items[24] = 2, app.game.items[21] = 3, app.game.items[27] = 1;
            app.game.items[202] = 1, app.game.items[216] = 1, app.game.items[427] = 1;
            app.game.party[size_t(campCharacter(app, 0))].mp = 10;
            auto key = [&](SDL_Keycode k) { campKey(app, k); };
            if (v == "items" || v == "use") app.camp.cursor = 0, key(SDLK_RETURN);
            if (v == "use") key(SDLK_RIGHT), key(SDLK_LEFT), key(SDLK_RETURN), key(SDLK_RETURN), key(SDLK_RIGHT), key(SDLK_RIGHT);
            if (v == "equip" || v == "slots" || v == "wear") app.camp.cursor = 1, key(SDLK_RETURN);
            if (v == "slots" || v == "wear") key(SDLK_RETURN);
            if (v == "wear") key(SDLK_RETURN);
            if (v == "status") app.camp.cursor = 3, key(SDLK_RETURN), key(SDLK_RETURN);
            if (v == "genes" || v == "plate" || v == "entries") app.game.items[481] = 1, app.game.items[108] = 1, app.game.items[94] = 1;
            if (v == "genes") app.camp.cursor = 2, key(SDLK_RETURN), key(SDLK_DOWN), key(SDLK_DOWN);
            if (v == "entries") app.camp.cursor = 2, key(SDLK_RETURN), key(SDLK_RETURN);
            if (v == "human") { // Equip > the Human Genes slot's list (Part 01 1190 s)
                app.game.items[479] = app.game.items[480] = 1;
                app.camp.cursor = 1, key(SDLK_RETURN), key(SDLK_RETURN);
                for (int k = 0; k < 3; ++k) key(SDLK_DOWN);
                key(SDLK_RETURN);
            }
            if (v == "plate") {
                app.camp.cursor = 1, key(SDLK_RETURN), key(SDLK_RETURN);
                for (int k = 0; k < 4; ++k) key(SDLK_DOWN); // Gene Plate 1
                key(SDLK_RETURN);
            }
            app.camp.anim = 100;
            for (int f = 0; f < 12; ++f) stepCamp(app); // (the list settled on its line)
        }
    }
    if (app.captureMenu == 7) { // --save-screen clear: the title's, pair 1 with every story cleared (a save of its own)
        app.saveDirOverride = userDir() + "capture-clear/";
        SDL_CreateDirectory(app.saveDirOverride.c_str());
        cr::SaveData sd = captureSave(app);
        for (int f : {31, 32, 34, 36, 38, 39, 40}) sd.game.setFlag(f, true);
        sd.leader = 0;
        sd.playSeconds = 3600 * 23 + 60 * 7;
        cr::writeSave(dataPath(app), sd);
        app.chosenPair = 1;
        app.captureMenu = 3;
    }
    if (app.captureMenu == 2 || app.captureMenu == 3) { openSaveScreen(app, app.captureMenu == 3); app.saveScreen.anim = 100; }
    if (app.captureMenu == 4) { openCharSelect(app); app.charSelectAnim = 100; }
    if (app.captureMenu == 8) { // (stepped in: a capture draws one frame)
        openWorldSelect(app);
        for (int f = 0; f < 40 && app.worldChoiceOpen; ++f) stepWorldSelect(app);
    }
    if (app.captureMenu == 9) {
        openLastBattle(app);
        for (int f = 0; f < 40 && app.lastBattle.open; ++f) stepLastBattle(app);
    }
    if (app.captureMenu == 5) { openScenarios(app); app.scenariosAnim = 100; app.scenariosScroll = app.captureAt[0] == app.captureAt[0] ? -app.captureAt[0] : 0;
        if (app.captureAt[1] == app.captureAt[1]) scenariosScrollSideways(app, -app.captureAt[1]); }
    if (app.captureMenu == 6) {
        openManual(app, 0);
        app.manualAnim = 100;
        if (app.captureAt[0] == app.captureAt[0]) app.manualCursor = int(app.captureAt[0]);
        if (manualTopics(app).empty()) // (none held yet: every manual item, the topic the item's number - 439)
            for (int item = kManualFirstItem; item <= kManualLastItem; ++item) app.game.items[item] = 1;
        if (app.captureAt[1] == app.captureAt[1]) { // --at topic page: that topic's page open (its picture 30 frames on)
            app.typeOn = false;
            manualChoose(app);
            for (int f = 0; f < 12; ++f) stepManual(app);
            for (int k = 0; k < int(app.captureAt[1]); ++k) manualTap(app);
            for (int f = 0; f < 30; ++f) stepManual(app);
        }
        std::printf("manual: %zu topics\n", manualTopics(app).size());
    }
    if (app.captureOptions) { // --volume-menu (page 1) / --video-menu (page 4)
        loadMix(app);
        const int page = app.optionsPage;
        openOptions(app, page);
        app.optionsAnim = 100;
        if (const char* ra = SDL_getenv("CR_CAPTURE_RESET")) app.resetAsk = SDL_atoi(ra); // (Reset Data's question 1 / 2)
        if (const char* row = SDL_getenv("CR_CAPTURE_OPTIONS_ROW")) { // (that row in focus, the page scrolled to it)
            app.optionsRow = SDL_atoi(row);
            optionsScrollFix(app, int(optionRows(app, app.optionsPage).size()), true);
        }
        // Exercise the actual native Options handlers in automated captures.
        if (const char* keys = SDL_getenv("CR_CAPTURE_OPTIONS_KEYS")) {
            std::istringstream sequence(keys);
            for (std::string key; sequence >> key;) {
                if (key == "enter") optionsKey(app, SDLK_RETURN);
                else if (key == "left") optionsKey(app, SDLK_LEFT);
                else if (key == "right") optionsKey(app, SDLK_RIGHT);
                else if (key == "back") optionsKey(app, SDLK_ESCAPE);
            }
            std::printf("import UI: state %d, succeeded %d, options %d, title %d\n", int(app.saveImport.state),
                        int(app.saveImport.succeeded), int(app.optionsOpen), int(app.titleOpen));
        }
    }
    if (app.havePlayer && app.captureAt[0] == app.captureAt[0]) { // --at x z: put the player there
        float y;
        if (cr::floorHeight(app.collisionData, app.captureAt[0], 1000.0f, app.captureAt[1], 0.0f, y))
            app.walker.pos = {app.captureAt[0], y, app.captureAt[1]};
    }
    settleMenuMask(app);
    // The capture shows exactly the frames stepped above; --settle draws that moment first (an
    // upscaler's history converges on it)
    const Uint64 frame = Uint64(SDL_NS_PER_SECOND / (app.captureSettleFps > 0 ? float(app.captureSettleFps) : kFramesPerSecond));
    const char* every = SDL_getenv("CR_CAPTURE_EVERY"); // (debugging: each --settle-moving frame as <dir>/sNNNN.bmp)
    for (int f = 0; f < app.captureSettle; ++f) {
        app.lastTicks = app.captureSettleMoving ? SDL_GetTicksNS() - frame : 0;
        drawFrame(app, false);
        int fw = 0, fh = 0;
        std::vector<uint8_t> fp;
        if (every && app.renderer.readPixels(fp, fw, fh))
            if (SDL_Surface* fs = SDL_CreateSurface(fw, fh, SDL_PIXELFORMAT_RGBA32)) {
                for (int y = 0; y < fh; ++y)
                    std::memcpy(static_cast<uint8_t*>(fs->pixels) + size_t(y) * fs->pitch, &fp[size_t(y) * fw * 4], size_t(fw) * 4);
                char name[32];
                SDL_snprintf(name, sizeof name, "/s%04d.bmp", f);
                SDL_SaveBMP(fs, (std::string(every) + name).c_str());
                SDL_DestroySurface(fs);
            }
    }
    app.lastTicks = app.captureSettleMoving && app.captureSettle ? SDL_GetTicksNS() - frame : 0;
    drawFrame(app, false);
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    if (!app.renderer.readPixels(px, w, h)) {
        std::printf("FAILED to read the frame back\n");
        return 1;
    }
    SDL_Surface* surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(surf->pixels) + size_t(y) * surf->pitch, &px[size_t(y) * w * 4], size_t(w) * 4);
    const bool ok = SDL_SaveBMP(surf, outPath);
    SDL_DestroySurface(surf);
    std::printf("%s %s -> %s (%dx%d)\n", ok ? "captured" : "FAILED", mapName, outPath, w, h);
    if (app.hdModelsLoaded) std::printf("hd models: %d replaced (%s)\n", app.hdModelsLoaded, app.modelsDir.c_str());
    if (app.battle.active) std::printf("battle: %d steps (speed %d)\n", app.battle.frames, app.battleSpeed);
    if (app.havePlayer && !app.battle.active) {
        const int action = app.player.action;
        const char* name = action >= 0 && action < int(app.player.actions.size()) ? app.player.actions[action].name.c_str() : "-";
        std::printf("player: at %.1f %.1f (field speed %d), gait %d, action %s, frame %.2f\n", app.walker.pos.x,
                    app.walker.pos.z, app.fieldSpeed, int(app.walker.gait), name, app.player.time);
    }
    if (app.haveStageCam) {
        const cr::StageCamera& sc = app.stageCam;
        const cr::Mat4 vp = cr::Mat4::perspective(sc.fovDegrees * 3.14159265f / 180.0f, 1.0f, 10.0f, 4000.0f) *
                            cr::Mat4::lookAt({sc.eye[0], sc.eye[1], sc.eye[2]}, {sc.target[0], sc.target[1], sc.target[2]},
                                             {0, 1, 0});
        for (const auto& d : app.npcDraws) {
            const float o[3] = {0, 0, 0};
            const cr::Vec3 p = d.world.transformPoint(o);
            const float* m = vp.m;
            const float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
            const float sx = (m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12]) / cw * 512 + 512;
            const float sy = 512 - (m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13]) / cw * 512;
            const LoadedModel& lm = app.npcModels[d.model];
            const std::string act = d.action >= 0 && d.action < int(lm.actions.size())
                                        ? lm.actions[d.action].name + (d.action < lm.builtin ? " (own)" : "") : "-";
            std::printf("npc %-16s at (%.1f %.1f %.1f) view (%.0f, %.0f) action %s t %.0f\n", d.model.c_str(), p.x, p.y,
                        p.z, sx, sy, act.c_str(), d.time);
        }
    }
    if (app.havePlayer && app.haveStageCam) {
        // Where the leader's bounding box lands in the 1024x1024 stage view
        const cr::StageCamera& sc = app.stageCam;
        const float aspect = sc.viewHeight > 0 ? float(sc.viewWidth) / float(sc.viewHeight) : 1.0f;
        const cr::Mat4 mvp = cr::Mat4::perspective(sc.fovDegrees * 3.14159265f / 180.0f, aspect, 10.0f, 4000.0f) *
                             cr::Mat4::lookAt({sc.eye[0], sc.eye[1], sc.eye[2]}, {sc.target[0], sc.target[1], sc.target[2]},
                                              {0, 1, 0}) * playerWorld(app.walker);
        const auto& b0 = app.player.cpu.boundsMin;
        const auto& b1 = app.player.cpu.boundsMax;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int c = 0; c < 8; ++c) {
            const float p[3] = {c & 1 ? b1[0] : b0[0], c & 2 ? b1[1] : b0[1], c & 4 ? b1[2] : b0[2]};
            const float* m = mvp.m;
            const float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
            const float sx = (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / cw * 512 + 512;
            const float sy = 512 - (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / cw * 512;
            x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy);
        }
        std::printf("player %s at (%.1f %.1f %.1f) r=%.0f: view box x %.0f..%.0f y %.0f..%.0f (%.0f px tall)\n",
                    app.playerName.c_str(), app.walker.pos.x, app.walker.pos.y, app.walker.pos.z, app.walker.radius,
                    x0, x1, y0, y1, y1 - y0);
    }
    return ok ? 0 : 1;
}


} // namespace crapp
