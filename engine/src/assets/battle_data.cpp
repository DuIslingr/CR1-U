#include "assets/battle_data.hpp"

#include <algorithm>
#include <cstring>

#include "assets/event.hpp"
#include "core/file.hpp"
#include "game/stage_tables.hpp"

namespace cr {

namespace {

template <typename T>
bool rd(const std::vector<uint8_t>& f, size_t pos, T& v) {
    if (pos + sizeof(T) > f.size()) return false;
    std::memcpy(&v, f.data() + pos, sizeof(T));
    return true;
}

int s16(const uint8_t* p) { return int16_t(uint16_t(p[0] | p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

constexpr size_t kMonsterSize = 204;

} // namespace

bool BattleData::load(const std::string& dataDir) {
    monsters_.clear();
    groups_.clear();
    std::vector<uint8_t> f;
    if (!readFile(dataDir + "sys.evt", f)) return false;
    uint32_t groupsAt = 0, monstersAt = 0;
    if (!rd(f, 0x28, groupsAt) || !rd(f, 0x30, monstersAt) || !groupsAt || !monstersAt) return false;
    // A section ends where the next one (by offset) starts
    auto end = [&](uint32_t at) {
        uint32_t e = uint32_t(f.size());
        for (size_t h = 0; h < 0x80; h += 4) {
            uint32_t o = 0;
            if (rd(f, h, o) && o > at && o < e) e = o;
        }
        return e;
    };
    for (uint32_t p = groupsAt; p + 6 <= end(groupsAt); p += 6) {
        std::array<int, 3> g{};
        for (int k = 0; k < 3; ++k) {
            const int id = s16(&f[p + 2 * k]);
            g[k] = id > 0 ? id : 0;
        }
        groups_.push_back(g);
    }
    for (uint32_t p = monstersAt; p + kMonsterSize <= end(monstersAt); p += kMonsterSize) {
        const uint8_t* r = &f[p];
        MonsterDef m;
        m.id = int(monsters_.size()) + 1;
        std::memcpy(m.raw.data(), r, kMonsterSize);
        m.hp = u32(r);
        m.exp = u32(r + 4);
        m.genePoints = u32(r + 8);
        m.model = s16(r + 12);
        m.size = s16(r + 26);
        m.strongGrowth = s16(r + 28) != 0;
        m.level = s16(r + 32);
        m.atk = s16(r + 38);
        m.grd = s16(r + 40);
        m.spd = s16(r + 42);
        m.lck = s16(r + 44);
        monsters_.push_back(m);
    }
    // Section 13 (+0x34): u32 file offset per battle model -> its 18-byte action records, 13 for
    // most models, 14 or 15 for the bosses (their staged actions' slots, btlact_430..436); each
    // model's run ends where the next begins (btlanm_get_* index them unchecked)
    anims_.clear();
    uint32_t animsAt = 0, camerasAt = 0;
    if (rd(f, 0x34, animsAt) && animsAt) {
        std::vector<uint32_t> starts(kNumBattleModels, 0);
        for (int model = 0; model < kNumBattleModels; ++model) rd(f, animsAt + 4 * size_t(model), starts[size_t(model)]);
        for (int model = 0; model < kNumBattleModels; ++model) {
            std::array<BattleAnim, kBattleAnimSlots> slots{};
            const uint32_t at = starts[size_t(model)];
            size_t stop = end(at);
            for (uint32_t s : starts)
                if (s > at) stop = std::min(stop, size_t(s));
            const int n = at && at < stop ? std::min<int>(kBattleAnimSlots, int((stop - at) / 18)) : 0;
            for (int k = 0; k < n; ++k) {
                const uint8_t* r = &f[at + 18 * size_t(k)];
                slots[size_t(k)] = {s16(r), s16(r + 2), s16(r + 4), s16(r + 6), s16(r + 8), s16(r + 10)};
            }
            anims_.push_back(slots);
        }
    }
    // Section 15 (+0x3c): 32-byte cameras
    cameras_.clear();
    if (rd(f, 0x3c, camerasAt) && camerasAt)
        for (uint32_t p = camerasAt; p + 32 <= end(camerasAt); p += 32) {
            BattleCamera c;
            for (int k = 0; k < 16; ++k) c.raw[k] = int16_t(s16(&f[p + 2 * size_t(k)]));
            for (int k = 0; k < 3; ++k) {
                c.eye[k] = c.raw[k];
                c.target[k] = c.raw[3 + k];
            }
            cameras_.push_back(c);
        }
    return !monsters_.empty();
}

BattleAnim BattleData::anim(int model, int slot) const {
    if (model < 0 || model >= int(anims_.size()) || slot < 0 || slot >= kBattleAnimSlots) return BattleAnim();
    return anims_[size_t(model)][size_t(slot)];
}

const BattleCamera* BattleData::camera(int id) const {
    return id >= 0 && id < int(cameras_.size()) ? &cameras_[size_t(id)] : nullptr;
}

const MonsterDef* BattleData::monster(int id) const {
    return id >= 1 && id <= int(monsters_.size()) && monsters_[size_t(id - 1)].model >= 0 ? &monsters_[size_t(id - 1)]
                                                                                        : nullptr;
}

std::array<int, 3> BattleData::group(int id) const {
    return id >= 1 && id <= int(groups_.size()) ? groups_[size_t(id - 1)] : std::array<int, 3>{};
}

int BattleData::formation(const std::array<int, 3>& ids) const {
    int code = 0, digit = 1;
    for (int id : ids)
        if (const MonsterDef* m = monster(id)) {
            code += m->size * digit;
            digit *= 10;
        }
    for (int i = 0; i < 48; ++i)
        if (kBattleFormationCode[i] == code) return i / 3;
    return -1;
}

bool encounterTable(const EventFile& events, int mapNumber, EncounterTable& out) {
    out = EncounterTable();
    const std::vector<uint8_t>& f = events.bytes();
    const int area = events.area();
    if (area < 0 || area >= kNumAreas || mapNumber < kAreaFirstMap[area]) return false;
    uint32_t table = 0, entry = 0;
    if (!rd(f, 0x14, table) || !table || table >= f.size()) return false;
    // The entries follow the table, so it ends where the first one starts (maps past it read junk)
    size_t first = f.size();
    for (size_t p = table; p < first && p + 4 <= f.size(); p += 4) {
        uint32_t v = 0;
        rd(f, p, v);
        if (v > table && v < first) first = v;
    }
    const size_t slot = table + 4 * size_t(mapNumber - kAreaFirstMap[area]);
    if (slot + 4 > first || !rd(f, slot, entry) || size_t(entry) + 8 > f.size()) return false;
    const uint8_t* e = &f[entry];
    if (uint16_t(s16(e)) == 0xFFFF) return false;
    out.stage = e[0];
    out.distance = s16(e + 2);
    out.rate = s16(e + 4);
    for (int k = 0; k < 128; ++k) {
        const size_t p = entry + 8 + 12 * size_t(k);
        if (p + 12 > f.size() || uint16_t(s16(&f[p])) == 0xFFFF) break;
        EncounterGroup g;
        g.group = uint16_t(s16(&f[p]));
        g.minLevel = s16(&f[p + 2]);
        g.extra = s16(&f[p + 4]);
        g.weight = s16(&f[p + 6]);
        g.firstStrike = s16(&f[p + 8]);
        g.ambush = s16(&f[p + 10]);
        out.groups.push_back(g);
    }
    return true;
}

} // namespace cr
