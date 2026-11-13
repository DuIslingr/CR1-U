#include "game/save.hpp"
#include "game/stage_tables.hpp"
#include "platform/atomic_file.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#include "game/genes.hpp"
#include "game/shop.hpp"

namespace cr {

ItemHome itemHome(const ItemTable* t, int item) {
    if ((item >= 81 && item <= 88) || (item >= 131 && item <= 138)) return ItemHome::RouteFlag;
    if (item >= 201 && item <= 424) return ItemHome::Gear;
    int kind = -1;
    if (const ItemParam* p = t ? t->item(item) : nullptr) kind = p->kind;
    else // (2.2.0's sys.evt: kind 4 81..113, 5 131..186, 9 439..478 / 501..502, 10 479..500)
        kind = item >= 81 && item <= 113 ? 4 : item >= 131 && item <= 186 ? 5 : item >= 479 && item <= 500 ? 10 : item >= 439 && item <= 502 ? 9 : 0;
    return kind == 4 || kind == 5 || kind == 9 || kind == 10 ? ItemHome::Shared : ItemHome::Route;
}

void changeRoute(GameState& g, const ItemTable* t, int r) {
    r = std::clamp(r, 0, 3);
    const int from = std::clamp(g.bagRoute, 0, 3);
    if (r == from) return;
    std::unordered_map<int, int>& out = g.routeBags[size_t(from)];
    out.clear();
    for (auto it = g.items.begin(); it != g.items.end();) {
        const ItemHome h = itemHome(t, it->first);
        if (h == ItemHome::Route || h == ItemHome::RouteFlag) {
            if (it->second > 0) out[it->first] = it->second;
            it = g.items.erase(it);
        } else ++it;
    }
    for (const auto& [item, n] : g.routeBags[size_t(r)]) g.items[item] = n;
    g.routeBags[size_t(r)].clear();
    g.bagRoute = r;
}

void foldPairSave(SaveData& d, const SaveData& s, const ItemTable* t) {
    const int r = s.route();
    GameState& g = d.game;
    const GameState& n = s.game;
    for (size_t w = 0; w < g.flags.size() && w < n.flags.size(); ++w) g.flags[w] |= n.flags[w];
    for (const auto& [k, v] : n.vars) g.vars[k] = v;
    for (int k = 0; k < 2; ++k) g.party[size_t(2 * r + k)] = n.party[size_t(2 * r + k)];
    g.walkExp[size_t(r)] = n.walkExp[size_t(r)];
    g.noEncounters[size_t(r)] = n.noEncounters[size_t(r)];
    g.bands[size_t(r)] = n.bands[size_t(r)];
    g.treasure[size_t(r)] = n.treasure[size_t(r)];
    for (int b : n.openedBoxes)
        if (std::find(g.openedBoxes.begin(), g.openedBoxes.end(), b) == g.openedBoxes.end()) g.openedBoxes.push_back(b);
    std::sort(g.openedBoxes.begin(), g.openedBoxes.end());
    g.oz = n.oz;
    changeRoute(g, t, r);
    for (auto it = g.items.begin(); it != g.items.end();) { // the pair's own bag and its own gear: s's
        const ItemHome h = itemHome(t, it->first);
        if (h == ItemHome::Route || h == ItemHome::RouteFlag || (h == ItemHome::Gear && (it->first - 201) / 56 == r)) it = g.items.erase(it);
        else ++it;
    }
    for (const auto& [item, c] : n.items) {
        if (c <= 0) continue;
        const ItemHome h = itemHome(t, item);
        if (h == ItemHome::Shared || (h == ItemHome::Gear && (item - 201) / 56 != r)) g.items[item] = std::max(g.items[item], c);
        else g.items[item] = c;
    }
    d.places[size_t(r)] = s.placeInPlay();
    d.area = s.area, d.map = s.map, d.yaw = s.yaw, d.leader = s.leader, d.playSeconds = s.playSeconds, d.savedAt = s.savedAt;
    std::memcpy(d.pos, s.pos, sizeof d.pos);
}

bool writeSave(const std::string& path, const SaveData& in) {
    SaveData copy;
    const bool routed = in.game.bagRoute == in.route();
    if (!routed) copy = in, changeRoute(copy.game, nullptr, in.route()); // (the items as the leader's pair sees them)
    const SaveData& s = routed ? in : copy;
    std::ostringstream o;
    o.precision(9);
    o << "CRSAVE 1\n";
    o << "area " << s.area << "\nmap " << s.map << "\npos " << s.pos[0] << ' ' << s.pos[1] << ' ' << s.pos[2]
      << "\nyaw " << s.yaw << "\nleader " << s.leader << "\nplay " << s.playSeconds << "\nsaved " << s.savedAt << '\n';
    for (int r = 0; r < 4; ++r) {
        const SaveData::Place& p = s.places[size_t(r)];
        if (!p.has) continue;
        o << "place " << r << " 1 " << p.area << ' ' << p.map << ' ' << p.pos[0] << ' ' << p.pos[1] << ' ' << p.pos[2] << ' '
          << p.yaw << ' ' << p.leader << ' ' << p.playSeconds << ' ' << p.savedAt << '\n';
    }
    if (s.wholeGame) {
        o << "routebags 1\n";
        for (int r = 0; r < 4; ++r) {
            if (r == s.route()) continue;
            o << "routebag " << r;
            std::map<int, int> bag(s.game.routeBags[size_t(r)].begin(), s.game.routeBags[size_t(r)].end());
            for (auto& [k, v] : bag)
                if (v > 0) o << ' ' << k << '=' << v;
            o << '\n';
        }
    }
    o << "flags";
    char hex[16];
    for (uint32_t w : s.game.flags) {
        std::snprintf(hex, sizeof hex, " %08x", w);
        o << hex;
    }
    // Sorted, so the same state always writes the same file
    o << "\nvars";
    std::map<int, int32_t> vars(s.game.vars.begin(), s.game.vars.end());
    for (auto& [k, v] : vars) o << ' ' << k << '=' << v;
    o << "\nitems";
    std::map<int, int> items(s.game.items.begin(), s.game.items.end());
    for (auto& [k, v] : items) o << ' ' << k << '=' << v;
    o << "\noz " << s.game.oz;
    for (int c = 0; c < kCharacters; ++c) { // party c level exp hp mp weapon armor jewel
        const PartyMember& m = s.game.party[size_t(c)];
        o << "\nparty " << c << ' ' << m.level << ' ' << m.exp << ' ' << m.hp << ' ' << m.mp << ' ' << m.equip[0] << ' '
          << m.equip[1] << ' ' << m.equip[2] << ' ' << m.seeds[0] << ' ' << m.seeds[1] << ' ' << m.seeds[2] << ' '
          << m.seeds[3] << ' ' << m.genes[0] << ' ' << m.genes[1] << ' ' << m.genes[2] << ' ' << m.genes[3];
        for (int b : m.boosts) o << ' ' << b;
    }
    o << "\nnoencounters";
    for (bool b : s.game.noEncounters) o << ' ' << int(b);
    o << "\nwalkexp";
    for (int e : s.game.walkExp) o << ' ' << e;
    o << "\nboxes"; // (each box once: older saves may list one many times)
    std::vector<int> boxes = s.game.openedBoxes;
    std::sort(boxes.begin(), boxes.end());
    boxes.erase(std::unique(boxes.begin(), boxes.end()), boxes.end());
    for (int b : boxes) o << ' ' << b;
    o << "\nbands";
    for (int b : s.game.bands) o << ' ' << b;
    for (int r = 0; r < 4; ++r) {
        o << "\ntreasure " << r;
        for (const auto& e : s.game.treasure[size_t(r)]) o << ' ' << e[0] << ' ' << e[1];
    }
    o << '\n';
    const std::string text = o.str();
    return crstorage::replaceFile(path, text);
}

bool readSave(const std::string& path, SaveData& s, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    size_t size = 0;
    char* data = static_cast<char*>(SDL_LoadFile(path.c_str(), &size));
    if (!data) return fail("no save");
    const bool lastLineWhole = size > 0 && data[size - 1] == '\n';
    std::istringstream in(std::string(data, size));
    SDL_free(data);
    std::string magic;
    int version = 0;
    in >> magic >> version;
    if (magic != "CRSAVE" || version != 1) return fail("not a save (or a newer version)");
    SaveData out;
    std::string line;
    std::getline(in, line);
    int parts = 0, partyLines = 0, placeLines = 0;
    out.wholeGame = false; // until its line says so
    while (std::getline(in, line)) {
        std::istringstream l(line);
        std::string key;
        l >> key;
        parts += key == "flags" || key == "vars" || key == "items" || key == "oz" || key == "noencounters" || key == "walkexp";
        partyLines += key == "party";
        if (key == "area") l >> out.area;
        else if (key == "map") l >> out.map;
        else if (key == "pos") l >> out.pos[0] >> out.pos[1] >> out.pos[2];
        else if (key == "yaw") l >> out.yaw;
        else if (key == "leader") l >> out.leader;
        else if (key == "play") l >> out.playSeconds;
        else if (key == "saved") l >> out.savedAt;
        else if (key == "place") {
            int r = -1, has = 0;
            SaveData::Place p;
            if (l >> r >> has >> p.area >> p.map >> p.pos[0] >> p.pos[1] >> p.pos[2] >> p.yaw >> p.leader >> p.playSeconds >> p.savedAt &&
                r >= 0 && r < 4) {
                p.has = has != 0 && p.area >= 0 && p.area < kNumAreas && p.map >= 0 && p.map < kNumMaps;
                p.leader = std::clamp(p.leader, 0, 7);
                out.places[size_t(r)] = p;
                ++placeLines;
            }
        }
        else if (key == "routebags") out.wholeGame = true;
        else if (key == "routebag") {
            int r = -1;
            std::string kv;
            if (l >> r && r >= 0 && r < 4)
                while (l >> kv) {
                    const size_t eq = kv.find('=');
                    if (eq == std::string::npos) continue;
                    const int k = std::atoi(kv.c_str()), v = std::clamp(std::atoi(kv.c_str() + eq + 1), 0, 99);
                    if (v > 0) out.game.routeBags[size_t(r)][k] = v;
                }
        }
        else if (key == "oz") l >> out.game.oz;
        else if (key == "party") {
            int c = -1;
            PartyMember m;
            if (l >> c >> m.level >> m.exp >> m.hp >> m.mp >> m.equip[0] >> m.equip[1] >> m.equip[2] && c >= 0 &&
                c < kCharacters) {
                // (far past any game's, so a damaged number can't overflow the sums it goes into)
                m.level = std::clamp(m.level, 1, kMaxLevel);
                m.exp = std::clamp(m.exp, 0, 99999999);
                m.hp = std::clamp(m.hp, -1, 9999999), m.mp = std::clamp(m.mp, -1, 9999999); // (-1: full)
                for (int& e : m.equip)
                    if (e < 0 || e > 9999) e = 0;
                for (int& sd : m.seeds) // seed points and gene plates (older saves end before them)
                    if (!(l >> sd)) sd = 0;
                    else sd = std::clamp(sd, 0, kMaxSeedPoints);
                const PartyMember fresh = newParty()[size_t(c)];
                for (int k = 0; k < 4; ++k)
                    if (!(l >> m.genes[k])) m.genes[k] = fresh.genes[k];
                    else if (m.genes[k] < 0 || m.genes[k] > 9999) m.genes[k] = 0;
                for (int& b : m.boosts)
                    if (!(l >> b)) b = 0;
                    else b = std::clamp(b, -9999999, 9999999);
                out.game.party[size_t(c)] = m;
            }
        }
        else if (key == "flags") {
            std::string w;
            for (size_t i = 0; i < out.game.flags.size() && l >> w; ++i)
                out.game.flags[i] = uint32_t(std::strtoul(w.c_str(), nullptr, 16));
        } else if (key == "vars" || key == "items") {
            std::string kv;
            while (l >> kv) {
                const size_t eq = kv.find('=');
                if (eq == std::string::npos) continue;
                const int k = std::atoi(kv.c_str()), v = std::atoi(kv.c_str() + eq + 1);
                if (key == "vars") out.game.vars[k] = v;
                else out.game.items[k] = v;
            }
        } else if (key == "noencounters") {
            for (size_t r = 0; r < out.game.noEncounters.size(); ++r) {
                int v = 0;
                if (l >> v) out.game.noEncounters[r] = v != 0;
            }
        } else if (key == "walkexp") {
            for (int& e : out.game.walkExp)
                if (l >> e) e = std::clamp(e, 0, 100000);
        } else if (key == "boxes") {
            int b;
            while (l >> b) out.game.openedBoxes.push_back(b);
            std::vector<int>& boxes = out.game.openedBoxes; // (each once: older saves may repeat them)
            std::sort(boxes.begin(), boxes.end());
            boxes.erase(std::unique(boxes.begin(), boxes.end()), boxes.end());
        } else if (key == "bands") {
            for (int& b : out.game.bands)
                if (l >> b) b = std::clamp(b, 0, 19);
        } else if (key == "treasure") {
            int r = -1, id, item;
            if (l >> r && r >= 0 && r < 4)
                while (l >> id >> item) out.game.treasure[size_t(r)].push_back({id, item});
        }
    }
    // (every line ends in a newline, and a save of any version has these six and all the party, in
    // this file's order or the Vita import's: boxes and what follows it aren't in the import's)
    if (out.area < 0 || out.area >= kNumAreas || out.map < 0 || out.map >= kNumMaps || !lastLineWhole || parts < 6 || partyLines < kCharacters) return fail("incomplete save");
    ensureStartingGenes(out.game); // saves from before genes were kept
    out.leader = std::clamp(out.leader, 0, 7);
    out.game.oz = std::clamp(out.game.oz, 0, 9999999);
    out.game.bagRoute = out.game.route = out.route();
    out.game.routeBags[size_t(out.route())].clear();
    if (!placeLines) out.places[size_t(out.route())] = out.placeInPlay(); // (an older file, a resume point, an import)
    s = std::move(out);
    return true;
}

} // namespace cr
