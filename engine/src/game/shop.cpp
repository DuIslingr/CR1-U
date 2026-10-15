#include "game/shop.hpp"

#include <algorithm>
#include <cstring>

#include "core/file.hpp"

namespace cr {

namespace {

int s16(const uint8_t* p) { return int16_t(uint16_t(p[0] | p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// MuCom_GetShopBuyLevel's tables (0xc8710 / 0xc8720 / 0xc8730), per route
constexpr int kAllGatesFlag[4] = {31, 33, 35, 37};       // set: level 7
constexpr int kTopLevelFlag[4] = {1104, 1504, 1904, 2304}; // set: the top level is 7, else 6
constexpr int kProgressFlag[4] = {1074, 1474, 1874, 2274}; // counted down from: one level less per clear flag

// Consumables by level (0xc86d0): {item, from level}
constexpr int kConsumables[][2] = {{1, 2},  {21, 2}, {28, 2}, {2, 3},  {22, 3}, {29, 3}, {32, 3}, {33, 4},
                                   {34, 4}, {35, 4}, {36, 5}, {37, 5}, {38, 5}, {30, 6}, {31, 6}};

// The sell lists' item ranges per category (0xc86b0)
constexpr int kSellRange[kShopCategories][2] = {{1, 60}, {201, 424}, {201, 424}, {425, 438}};

int owned(const GameState& gs, int item) {
    auto it = gs.items.find(item);
    return it == gs.items.end() ? 0 : it->second;
}

} // namespace

bool ItemTable::load(const std::string& dataDir) {
    items_.clear();
    std::vector<uint8_t> f;
    if (!readFile(dataDir + "sys.evt", f) || f.size() < 0x80) return false;
    const uint32_t at = u32(&f[0x38]);
    if (!at || at >= f.size()) return false;
    uint32_t end = uint32_t(f.size()); // up to the next section
    for (size_t h = 0; h < 0x80; h += 4) {
        const uint32_t o = u32(&f[h]);
        if (o > at && o < end) end = o;
    }
    for (uint32_t p = at; p + 20 <= end; p += 20) {
        const uint8_t* r = &f[p];
        ItemParam it;
        it.price = int(u32(r));
        for (int k = 0; k < 6; ++k) it.stat[k] = s16(r + 4 + 2 * k);
        it.icon = int8_t(r[16]);
        it.kind = r[17];
        it.owners = r[19];
        items_.push_back(it);
    }
    return !items_.empty();
}

const ItemParam* ItemTable::item(int n) const {
    return n >= 1 && n <= int(items_.size()) ? &items_[size_t(n - 1)] : nullptr;
}

int shopLevel(const GameState& gs, int route) {
    route = std::clamp(route, 0, 3);
    if (gs.flag(kAllGatesFlag[route])) return 7;
    const int top = gs.flag(kTopLevelFlag[route]) ? 7 : 6;
    int k = 0; // flags checked down from the progress flag until one is set, at most 4
    while (k < 4 && !gs.flag(kProgressFlag[route] - k)) ++k;
    const int level = top - k;
    return level > 6 ? 7 : std::max(level, 2);
}

std::vector<int> shopStock(const GameState& gs, const ItemTable& items, int character, int category, int level) {
    std::vector<int> out;
    const int c = std::clamp(character, 0, 7);
    switch (category) {
    case 0:
        for (const auto& c : kConsumables) {
            if (c[1] > level) break;
            out.push_back(c[0]);
        }
        if (gs.flag(39)) // the stat seeds
            for (int n = 3; n <= 6; ++n) out.push_back(n);
        break;
    case 1: for (int k = 0; k < level; ++k) out.push_back(201 + 28 * c + k); break;
    case 2: for (int k = 0; k < level; ++k) out.push_back(215 + 28 * c + k); break;
    case 3: for (int k = 0; k < level - 1; ++k) out.push_back(426 + k); break;
    default: break;
    }
    out.erase(std::remove_if(out.begin(), out.end(), [&](int n) { return !items.item(n); }), out.end());
    return out;
}

std::vector<int> sellableItems(const GameState& gs, const ItemTable& items, int character, int category) {
    std::vector<int> out;
    if (category < 0 || category >= kShopCategories) return out;
    const int bit = 1 << std::clamp(character, 0, 7);
    for (int n = kSellRange[category][0]; n <= kSellRange[category][1]; ++n) {
        const ItemParam* p = items.item(n);
        if (!p || p->price == 0 || owned(gs, n) == 0 || !(p->owners & bit)) continue;
        if (category == 1 && p->kind != kKindWeapon) continue;
        if (category == 2 && p->kind != kKindArmor) continue;
        out.push_back(n);
    }
    return out;
}

int maxPurchase(int price, int oz, int have) {
    if (price <= 0) return 0;
    return std::clamp(std::min(oz / price, kMaxItemCount - have), 0, kMaxItemCount);
}

void addOz(GameState& gs, int delta) {
    gs.oz = int(std::clamp<int64_t>(int64_t(gs.oz) + delta, 0, kMaxOz));
}

} // namespace cr
