// crtool — headless asset utility for the Chaos Rings reimplementation.
//
//   crtool mct-info <file.mct>...           print texture headers
//   crtool mct-raw  <file.mct> <out.rgba>   decode to raw RGBA8 (for diffing against tools/mct.py)
//   crtool check    <dataHD dir>            decode every .mct and report failures
//   crtool models   <dataHD dir> [-v]       parse every .bin model package and report stats
//   crtool anims    <dataHD dir> [model]    parse every animation; pose + skin every fitting one
//   crtool maps     <dataHD dir>            parse every field collision mesh (.cmp)

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include "assets/animation.hpp"
#include "audio/at9.hpp"
#include "audio/music.hpp"
#include "audio/scream.hpp"
#include "audio/sound.hpp"
#include "assets/battle_data.hpp"
#include "assets/collision.hpp"
#include "assets/event.hpp"
#include "game/puzzle.hpp"
#include "assets/gxt.hpp"
#include "assets/psarc.hpp"
#include "assets/obb.hpp"
#define STB_VORBIS_HEADER_ONLY
#include "stb/stb_vorbis.c"
#include "assets/model.hpp"
#include "assets/texture.hpp"
#include "core/file.hpp"
#include "core/text.hpp"
#include "game/event_vm.hpp"
#include "scene/pose.hpp"
#include "scene/smooth.hpp"
#include "game/stage_tables.hpp"
#include "scene/pose.hpp"

namespace {

int mctInfo(int argc, char** argv) {
    for (int i = 2; i < argc; ++i) {
        std::vector<uint8_t> data;
        cr::Image img;
        cr::MctInfo info;
        std::string err;
        if (!cr::readFile(argv[i], data)) { std::printf("FAIL %s: cannot read\n", argv[i]); continue; }
        const bool ok = cr::decodeMct(data, img, &info, &err);
        std::printf("%s %s: id=%d %dx%d %s%s %s\n", ok ? "ok  " : "FAIL", argv[i], info.texId, info.width,
                    info.height, cr::mctFormatName(info.format), info.compressed ? " lzw" : "", err.c_str());
    }
    return 0;
}

int mctRaw(const char* in, const char* out) {
    std::vector<uint8_t> data;
    cr::Image img;
    std::string err;
    if (!cr::readFile(in, data) || !cr::decodeMct(data, img, nullptr, &err)) {
        std::fprintf(stderr, "decode failed: %s\n", err.c_str());
        return 1;
    }
    if (!SDL_SaveFile(out, img.rgba.data(), img.rgba.size())) {
        std::fprintf(stderr, "write failed: %s\n", SDL_GetError());
        return 1;
    }
    std::printf("%dx%d\n", img.width, img.height);
    return 0;
}

struct CheckState {
    std::string dir;
    int ok = 0, failed = 0;
};

SDL_EnumerationResult SDLCALL checkOne(void* user, const char* dirname, const char* fname) {
    auto* st = static_cast<CheckState*>(user);
    const size_t len = std::strlen(fname);
    if (len < 4 || SDL_strcasecmp(fname + len - 4, ".mct") != 0) return SDL_ENUM_CONTINUE;
    std::vector<uint8_t> data;
    cr::Image img;
    std::string err;
    if (cr::readFile(std::string(dirname) + fname, data) && cr::decodeMct(data, img, nullptr, &err)) {
        ++st->ok;
    } else {
        ++st->failed;
        std::printf("FAIL %s: %s\n", fname, err.c_str());
    }
    return SDL_ENUM_CONTINUE;
}

int check(const char* dir) {
    CheckState st{dir};
    if (!SDL_EnumerateDirectory(dir, checkOne, &st)) {
        std::fprintf(stderr, "cannot list %s: %s\n", dir, SDL_GetError());
        return 1;
    }
    std::printf("textures: %d ok, %d failed\n", st.ok, st.failed);
    return st.failed ? 1 : 0;
}

struct ModelCheck {
    bool verbose = false;
    int packages = 0, failed = 0, notModels = 0, meshes = 0, triangles = 0, skinned = 0;
    int unsupportedPrims = 0, unpairedSkins = 0, vbIbMismatch = 0;
    int texById = 0, texByIndex = 0, texMissing = 0, untextured = 0;
};

SDL_EnumerationResult SDLCALL checkModel(void* user, const char* dirname, const char* fname) {
    auto* st = static_cast<ModelCheck*>(user);
    const size_t len = std::strlen(fname);
    if (len < 4 || SDL_strcasecmp(fname + len - 4, ".bin") != 0) return SDL_ENUM_CONTINUE;
    std::vector<uint8_t> data;
    if (!cr::readFile(std::string(dirname) + fname, data)) return SDL_ENUM_CONTINUE;
    if (!cr::isModelPackage(data)) {
        ++st->notModels;
        return SDL_ENUM_CONTINUE;
    }
    ++st->packages;
    cr::Model m;
    std::string err;
    if (!cr::loadModelPackage(data, m, &err)) {
        ++st->failed;
        std::printf("FAIL %s: %s\n", fname, err.c_str());
        return SDL_ENUM_CONTINUE;
    }
    int skinned = 0;
    for (const auto& mesh : m.meshes) skinned += mesh.skinned;
    st->meshes += int(m.meshes.size());
    st->triangles += m.stats.triangles;
    st->skinned += skinned;
    st->unsupportedPrims += m.stats.unsupportedPrims;
    st->unpairedSkins += m.stats.unpairedSkins;
    st->texById += m.stats.texById;
    st->texByIndex += m.stats.texByIndex;
    st->texMissing += m.stats.texMissing;
    st->untextured += m.stats.untextured;
    if (m.stats.vertexBuffers != m.stats.indexBuffers) ++st->vbIbMismatch;
    if (st->verbose || m.stats.unsupportedPrims || m.stats.unpairedSkins ||
        m.stats.vertexBuffers != m.stats.indexBuffers) {
        std::printf("%-20s vb=%d ib=%d skin=%d tex=%zu meshes=%zu (skinned %d) tris=%d unsupported=%d "
                    "unpairedSkin=%d bbox=(%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n",
                    fname, m.stats.vertexBuffers, m.stats.indexBuffers, m.stats.boneDeformers,
                    m.textures.size(), m.meshes.size(), skinned, m.stats.triangles, m.stats.unsupportedPrims,
                    m.stats.unpairedSkins, m.boundsMin[0], m.boundsMin[1], m.boundsMin[2], m.boundsMax[0],
                    m.boundsMax[1], m.boundsMax[2]);
    }
    return SDL_ENUM_CONTINUE;
}

// eff.bin (eff3d_set_with_pos: mmapped by read_file_memmap): (offset, size) per effect, each a model
// package; prints what each holds
int effects(const char* dir) {
    std::vector<uint8_t> f;
    if (!cr::readFile(std::string(dir) + "/eff.bin", f) || f.size() < 8) { std::fprintf(stderr, "no eff.bin\n"); return 1; }
    uint32_t first = 0;
    std::memcpy(&first, f.data(), 4);
    const int n = int(first / 8);
    int bad = 0;
    for (int i = 0; i < n; ++i) {
        uint32_t off = 0, size = 0;
        std::memcpy(&off, &f[size_t(i) * 8], 4);
        std::memcpy(&size, &f[size_t(i) * 8 + 4], 4);
        if (size == 0 || off + size > f.size()) { std::printf("effect %2d: empty\n", i); continue; }
        std::vector<uint8_t> blob(f.begin() + off, f.begin() + off + size);
        cr::Model m;
        std::string err;
        if (!cr::loadModelPackage(blob, m, &err)) { std::printf("effect %2d: %s\n", i, err.c_str()); ++bad; continue; }
        float dur = 0;
        for (const auto& a : m.actions) dur = std::max(dur, a.duration);
        int blended = 0;
        for (const auto& mat : m.materials) blended += mat.blended();
        std::printf("effect %2d: %zu meshes, %d triangles, %zu textures, %zu nodes, %zu actions (%.0f frames), %d/%zu blended, "
                    "bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n", i, m.meshes.size(), m.stats.triangles, m.textures.size(),
                    m.skeleton.nodes.size(), m.actions.size(), dur, blended, m.materials.size(), m.boundsMin[0], m.boundsMin[1],
                    m.boundsMin[2], m.boundsMax[0], m.boundsMax[1], m.boundsMax[2]);
    }
    return bad ? 1 : 0;
}

// matanim <dataHD>: model packages whose Appearances name an action (refGid) - what those actions
// animate (segment 1 colour, 6 / 8 the texture's offset / scale) - and node visibility tracks
struct MatAnim {
    int packages = 0, withRef = 0, colour = 0, uv = 0, visModels = 0, lightModels = 0;
    std::vector<std::string> names;
    std::map<std::string, int> lightKinds, matKinds;
    std::map<std::string, std::string> lightFiles;
};
SDL_EnumerationResult SDLCALL matAnimModel(void* user, const char* dirname, const char* fname) {
    auto* st = static_cast<MatAnim*>(user);
    const size_t len = std::strlen(fname);
    if (len < 4 || SDL_strcasecmp(fname + len - 4, ".bin") != 0 || std::strcmp(fname, "eff.bin") == 0) return SDL_ENUM_CONTINUE;
    std::vector<uint8_t> data;
    if (!cr::readFile(std::string(dirname) + fname, data) || !cr::isModelPackage(data)) return SDL_ENUM_CONTINUE;
    cr::Model m;
    if (!cr::loadModelPackage(data, m)) return SDL_ENUM_CONTINUE;
    ++st->packages;
    int refs = 0, colour = 0, uv = 0, vis = 0;
    std::string detail;
    for (const auto& mat : m.materials) {
        const int g = int(int32_t(mat.refGid));
        if (g < 0) continue;
        ++refs;
        if (g >= int(m.actions.size())) { detail += " ref" + std::to_string(g) + "(none)"; continue; }
        const cr::Action& a = m.actions[size_t(g)];
        bool c = false, u = false;
        for (size_t seg = 0; seg < a.segments.size(); ++seg)
            for (const cr::Track& t : a.segments[seg]) {
                if (seg == 1 && t.type == 12 && t.comps == 4) c = true;
                if ((seg == 6 || seg == 8) && t.comps == 3) u = true;
            }
        colour += c, uv += u;
        detail += " ref" + std::to_string(g) + "(" + std::to_string(a.segments.size()) + "seg " + std::to_string(int(a.duration)) +
                  "f" + (c ? " rgba" : "") + (u ? " uv" : "") + ")";
    }
    for (const auto& a : m.actions)
        for (const auto& seg : a.segments)
            for (const cr::Track& t : seg) vis += t.type == 12 && t.comps == 1;
    int lights = 0;
    for (const auto& r : m.records) lights += r.section == 3; // Light records
    if (lights) ++st->lightModels;
    for (const auto& rec : m.lightRecords) { // the light's bytes after the header
        std::string hex;
        for (size_t i = 16; i < rec.size(); ++i) { char b[4]; std::snprintf(b, sizeof b, "%02x", rec[i]); hex += b; }
        st->lightKinds[hex]++;
        st->lightFiles[hex] += std::string(" ") + fname;
    }
    for (const auto& mat : m.materials) { // what the full-bright sum changes: both a diffuse and an ambient colour
        const bool d = mat.diffuse[0] + mat.diffuse[1] + mat.diffuse[2] > 0.01f, a = mat.ambient[0] + mat.ambient[1] + mat.ambient[2] > 0.01f;
        const bool e = mat.emission[0] + mat.emission[1] + mat.emission[2] > 0.01f;
        st->matKinds[std::string(lights ? "lit " : "unlit ") + (d ? "D" : "-") + (a ? "A" : "-") + (e ? "E" : "-")]++;
    }
    if (refs) ++st->withRef;
    st->colour += colour > 0, st->uv += uv > 0, st->visModels += vis > 0;
    if ((refs || vis) && std::getenv("CR_MATANIM_VERBOSE")) std::printf("%s: %d materials with actions%s; %d visibility tracks\n", fname, refs, detail.c_str(), vis);
    return SDL_ENUM_CONTINUE;
}
int matanim(const char* dir) {
    MatAnim st;
    SDL_EnumerateDirectory(dir, matAnimModel, &st);
    std::printf("packages %d: %d with material actions (%d colour, %d texture motion), %d with visibility tracks, %d with lights\n",
                st.packages, st.withRef, st.colour, st.uv, st.visModels, st.lightModels);
    for (const auto& [k, n] : st.lightKinds)
        std::printf("light %s x%d:%s\n", k.c_str(), n, n < 40 ? st.lightFiles[k].c_str() : " (many)");
    for (const auto& [k, n] : st.matKinds) std::printf("materials %s x%d\n", k.c_str(), n);
    return 0;
}

// visdump <file.bin>: each action's visibility tracks (node, keys) and which meshes attach where
int visdump(const char* path) {
    std::vector<uint8_t> data;
    cr::Model m;
    if (!cr::readFile(path, data) || !cr::loadModelPackage(data, m)) { std::printf("can't load %s\n", path); return 1; }
    std::printf("%zu nodes, %zu meshes:", m.skeleton.nodes.size(), m.meshes.size());
    for (size_t i = 0; i < m.meshes.size(); ++i)
        std::printf(" %zu@%d%s%s", i, m.meshes[i].attachNode, m.meshes[i].skinned ? "s" : "", m.meshes[i].visible ? "" : "(hidden)");
    std::printf("\n");
    for (size_t a = 0; a < m.actions.size(); ++a) {
        const cr::Action& act = m.actions[a];
        std::string line;
        for (size_t seg = 0; seg < act.segments.size(); ++seg)
            for (const cr::Track& t : act.segments[seg]) {
                if (t.type != 12 || t.comps != 1) continue;
                line += " n" + std::to_string(seg) + "[";
                for (const cr::Key& k : t.keys) line += std::to_string(int(k.time)) + ":" + std::to_string(int(k.v[0] + 0.5f)) + " ";
                line += "]";
            }
        if (!line.empty()) std::printf("action %zu %s (%.0f f, %zu seg):%s\n", a, act.name.c_str(), act.duration, act.segments.size(), line.c_str());
    }
    return 0;
}

// propuse <dataHD>: which areas' NPC rows use the effect props (models 99..118, 158..196) and on
// which maps they stand
int propuse(const char* dirArg) {
    const std::string dir = std::string(dirArg) + (dirArg[std::strlen(dirArg) - 1] == '/' ? "" : "/");
    for (int area = 0; area < cr::kNumAreas; ++area) {
        cr::EventFile ef;
        if (!ef.load(dir, area)) continue;
        std::string rows;
        for (int id = 0; id < 4000; ++id) {
            cr::NpcRow r;
            if (!ef.npcRow(id, r)) break;
            if ((r.model >= 99 && r.model <= 118) || (r.model >= 158 && r.model <= 196))
                rows += " " + std::to_string(id) + ":" + cr::kNpcModelFile[r.model];
        }
        std::string maps;
        for (int map = 0; map < cr::kNumMaps; ++map)
            for (const cr::FieldNpc& n : ef.npcs(map))
                if ((n.model >= 99 && n.model <= 118) || (n.model >= 158 && n.model <= 196))
                    maps += " map" + std::to_string(map) + ":" + std::to_string(n.id);
        if (!rows.empty()) std::printf("area %d rows%s\n  placed%s\n", area, rows.c_str(), maps.empty() ? " nowhere" : maps.c_str());
    }
    return 0;
}

// lightdump <file.bin>: the package's Light records, as u32 / float words
int lightdump(const char* path) {
    std::vector<uint8_t> data;
    cr::Model m;
    if (!cr::readFile(path, data) || !cr::loadModelPackage(data, m)) { std::printf("can't load %s\n", path); return 1; }
    for (const auto& rec : m.lightRecords) {
        std::printf("light record, %zu bytes:\n", rec.size());
        for (size_t i = 0; i + 4 <= rec.size(); i += 4) {
            uint32_t u;
            float f;
            std::memcpy(&u, &rec[i], 4);
            std::memcpy(&f, &rec[i], 4);
            std::printf("  +%02zx %08x %g\n", i, u, f);
        }
    }
    return 0;
}

int gNativeScript = -1;
// natives <dataHD>: every script of every area run once from a new game; the native calls (op 15)
// met, by id, with how many places in the scripts call them
int natives(const char* dirArg) {
    const std::string dir = std::string(dirArg) + (dirArg[std::strlen(dirArg) - 1] == '/' ? "" : "/");
    std::map<int, std::set<std::pair<int, size_t>>> sites;
    for (int area = 0; area < cr::kNumAreas; ++area) {
        cr::EventFile ef;
        std::vector<uint8_t> raw;
        if (!ef.load(dir, area) || !cr::readFile(dir + cr::kAreaEventFile[area], raw)) continue;
        cr::setNativeTrace([&](int id, const int32_t* a) {
            static const char* want = std::getenv("CR_NATIVE_ARGS"); // e.g. "2016,2033"
            if (!want || std::string(want).find(std::to_string(id)) == std::string::npos) return;
            std::printf("  area %d script %d call %d args", area, gNativeScript, id);
            for (int k = 0; k < 12; ++k) std::printf(" %d", a[k]);
            std::printf("\n");
        });
        cr::setEventTrace([&](size_t pc, int op) {
            if (op == 25 && std::getenv("CR_TEX2D_FLAGS") && pc + 5 <= raw.size()) // Tex2d: u16 slot, u16 flags
                std::printf("  area %d script %d tex2d slot %d flags %04x\n", area, gNativeScript, int16_t(raw[pc + 1] | raw[pc + 2] << 8),
                            raw[pc + 3] | raw[pc + 4] << 8);
            if (op != 15 || pc + 3 > raw.size()) return;
            const int id = raw[pc + 1] | raw[pc + 2] << 8;
            sites[id].insert({area, pc});
        });
        for (int n = 0; n < 1000; ++n) {
            if (!ef.scriptEntry(n)) continue;
            gNativeScript = n;
            cr::GameState gs;
            cr::runEvent(ef, n, gs, area, cr::kAreaFirstMap[area]);
        }
        cr::setEventTrace(nullptr);
        cr::setNativeTrace(nullptr);
    }
    for (const auto& [id, s] : sites) {
        std::set<int> areas;
        for (const auto& [a, pc] : s) areas.insert(a);
        std::printf("call %d: %zu places in %zu areas\n", id, s.size(), areas.size());
    }
    return 0;
}

// npcloads <dataHD>: which scripts bring in the story NPCs (NpcMdlRead of rows 501..512) and the
// flags that let them - every script and EvrStart entry run with random flags (seeded, env
// CR_TRIALS per start, default 200), printing the flags read on the first run that gets there
int npcLoads(const char* dirArg) {
    const std::string dir = std::string(dirArg) + (dirArg[std::strlen(dirArg) - 1] == '/' ? "" : "/");
    const char* trialsEnv = std::getenv("CR_TRIALS");
    const int trials = trialsEnv ? std::atoi(trialsEnv) : 200;
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
    for (int area = 0; area < cr::kNumAreas; ++area) {
        cr::EventFile ef;
        std::vector<uint8_t> raw;
        if (!ef.load(dir, area) || !cr::readFile(dir + cr::kAreaEventFile[area], raw)) continue;
        std::vector<int> loads;
        std::vector<std::pair<int, bool>> reads;
        cr::setEventTrace([&](size_t pc, int op) {
            if (op != 51 || pc + 3 > raw.size()) return;
            const int id = raw[pc + 1] | raw[pc + 2] << 8;
            if (id >= 501 && id <= 512) loads.push_back(id);
        });
        std::vector<std::pair<int, int>> varReads;
        cr::setVarTrace([&](int var, int32_t v) {
            for (const auto& r : varReads)
                if (r.first == var) return;
            varReads.push_back({var, v});
        });
        cr::setFlagTrace([&](int flag, bool v) {
            for (const auto& r : reads)
                if (r.first == flag) return;
            reads.push_back({flag, v});
        });
        int nextMap = cr::kNumMaps;
        for (int a = 0; a < cr::kNumAreas; ++a)
            if (cr::kAreaFirstMap[a] > cr::kAreaFirstMap[area] && cr::kAreaFirstMap[a] < nextMap) nextMap = cr::kAreaFirstMap[a];
        std::set<std::string> seen;
        auto run = [&](const char* what, int n, uint32_t entry, int map) {
            for (int t = 0; t < trials; ++t) {
                cr::GameState gs;
                for (auto& w : gs.flags) w = rnd();
                if (std::getenv("CR_RANDOM_VARS")) // story variables 0..61 at small values too
                    for (int v = 0; v < 62; ++v) gs.vars[v] = int(rnd() >> 16) % 6;
                loads.clear();
                reads.clear();
                varReads.clear();
                cr::EventRunner r;
                if (!r.startAt(ef, entry, n, gs, area, map)) return;
                cr::EventHost host; // does nothing, so the script goes on past warps, messages and battles
                for (int k = 0; k < 20000 && r.step(&host); ++k) {}
                if (loads.empty()) continue;
                std::string key = std::string(what) + std::to_string(n);
                for (int id : loads) key += "," + std::to_string(id);
                if (!seen.insert(key).second) return;
                std::printf("%s %s %d map %d: loads", cr::kAreaEventFile[area], what, n, map);
                for (int id : loads) {
                    cr::NpcRow row;
                    std::printf(" %d (model %d)", id, ef.npcRow(id, row) ? row.model : -1);
                }
                std::printf("; flags read:");
                for (const auto& [f, v] : reads) std::printf(" %s%d", v ? "" : "!", f);
                std::printf("; vars read:");
                for (const auto& [n, v] : varReads) std::printf(" %d=%d", n, v);
                std::printf("\n");
                return;
            }
        };
        for (int n = 0; n < 1000; ++n)
            if (const uint32_t e = ef.scriptEntry(n)) run("script", n, e, cr::kAreaFirstMap[area]);
        for (int k = 0; k < 9; ++k)
            if (const uint32_t e = ef.returnEntry(k))
                for (int map = cr::kAreaFirstMap[area]; map < nextMap; ++map) run("evrstart", k, e, map);
        cr::setEventTrace(nullptr);
        cr::setFlagTrace(nullptr);
        cr::setVarTrace(nullptr);
    }
    return 0;
}

// music <dataHD>: every track decoded whole - frames and a hash of the PCM (system vs bundled FAAD2)
int music(const char* dirArg) {
    const std::string dir = std::string(dirArg) + (dirArg[std::strlen(dirArg) - 1] == '/' ? "" : "/");
    int ok = 0;
    const int only = std::getenv("CR_MUSIC_TRACK") ? std::atoi(std::getenv("CR_MUSIC_TRACK")) : -1;
    for (int t = 0; t < cr::kBgmTracks; ++t) {
        if (only >= 0 && t != only) continue;
        int64_t frames = 0;
        uint64_t hash = 0;
        std::string err;
        if (cr::decodeTrack(dir, t, frames, hash, &err)) {
            ++ok;
            std::printf("track %2d: %lld frames (%.1f s), pcm %016llx\n", t, (long long)frames, double(frames) / 44100.0,
                        (unsigned long long)hash);
        } else std::printf("track %2d: %s\n", t, err.c_str());
    }
    std::printf("%d of %d tracks decoded\n", ok, cr::kBgmTracks);
    return ok ? 0 : 1;
}

int models(const char* dir, bool verbose) {
    ModelCheck st;
    st.verbose = verbose;
    if (!SDL_EnumerateDirectory(dir, checkModel, &st)) {
        std::fprintf(stderr, "cannot list %s: %s\n", dir, SDL_GetError());
        return 1;
    }
    std::printf("model packages: %d parsed, %d failed (%d other .bin files)\n", st.packages - st.failed,
                st.failed, st.notModels);
    std::printf("meshes: %d (%d skinned), triangles: %d, unsupported index buffers: %d, unpaired skins: %d, "
                "packages with vb/ib count mismatch: %d\n",
                st.meshes, st.skinned, st.triangles, st.unsupportedPrims, st.unpairedSkins, st.vbIbMismatch);
    std::printf("mesh textures: %d by id, %d by package index, %d unresolved, %d untextured\n", st.texById,
                st.texByIndex, st.texMissing, st.untextured);
    return st.failed ? 1 : 0;
}

struct AnimCheck {
    std::string dir;
    std::vector<std::string> mcaFiles, binFiles;
};

SDL_EnumerationResult SDLCALL listAnimInputs(void* user, const char*, const char* fname) {
    auto* st = static_cast<AnimCheck*>(user);
    const size_t len = std::strlen(fname);
    if (len > 4 && SDL_strcasecmp(fname + len - 4, ".mca") == 0) st->mcaFiles.push_back(fname);
    if (len > 4 && SDL_strcasecmp(fname + len - 4, ".bin") == 0) st->binFiles.push_back(fname);
    return SDL_ENUM_CONTINUE;
}

float diagonal(const std::vector<float>& xyz) {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i + 2 < xyz.size(); i += 3)
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], xyz[i + c]);
            hi[c] = std::max(hi[c], xyz[i + c]);
        }
    const float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// 95th-percentile relative edge-length change of a mesh between rest and `posed`
float edgeStretchP95(const cr::Mesh& m, const std::vector<float>& posed) {
    std::set<std::pair<uint32_t, uint32_t>> edges;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
        for (int e = 0; e < 3; ++e) {
            uint32_t a = m.posOf[m.indices[t + e]], b = m.posOf[m.indices[t + (e + 1) % 3]];
            if (a != b) edges.insert({std::min(a, b), std::max(a, b)});
        }
    // Map positions back to a representative render vertex
    std::vector<uint32_t> rep(m.bindPositions.size() / 3, 0);
    for (size_t v = 0; v < m.posOf.size(); ++v) rep[m.posOf[v]] = uint32_t(v);
    std::vector<float> ratios;
    for (auto [a, b] : edges) {
        const float* ra = &m.positions[rep[a] * 3]; const float* rb = &m.positions[rep[b] * 3];
        const float* pa = &posed[rep[a] * 3]; const float* pb = &posed[rep[b] * 3];
        const float l0 = std::hypot(ra[0] - rb[0], ra[1] - rb[1], ra[2] - rb[2]);
        const float l1 = std::hypot(pa[0] - pb[0], pa[1] - pb[1], pa[2] - pb[2]);
        if (l0 > 1e-5f && l1 > 1e-4f) ratios.push_back(std::fabs(l1 / l0 - 1)); // l1≈0: hidden part
    }
    if (ratios.empty()) return 0;
    std::sort(ratios.begin(), ratios.end());
    return ratios[ratios.size() * 95 / 100];
}

int anims(const char* dir, const char* only) {
    AnimCheck st;
    st.dir = dir;
    if (!SDL_EnumerateDirectory(dir, listAnimInputs, &st)) return 1;
    const std::string base = std::string(dir) + (dir[std::strlen(dir) - 1] == '/' ? "" : "/");

    int files = 0, fileFail = 0, actions = 0;
    for (const auto& f : st.mcaFiles) {
        std::vector<uint8_t> d;
        std::vector<cr::Action> acts;
        std::string err;
        if (!cr::readFile(base + f, d) || !cr::parseMca(d.data(), d.size(), 0, acts, &err)) {
            ++fileFail;
            std::printf("FAIL %s: %s\n", f.c_str(), err.c_str());
            continue;
        }
        ++files;
        actions += int(acts.size());
    }
    std::printf("standalone animations: %d files ok, %d failed, %d actions\n", files, fileFail, actions);

    int posed = 0, nanPoses = 0, blowups = 0, models = 0, withAnim = 0;
    for (const auto& f : st.binFiles) {
        if (only && f != std::string(only) + ".bin") continue;
        std::vector<uint8_t> d;
        cr::Model m;
        if (!cr::readFile(base + f, d) || !cr::isModelPackage(d) || !cr::loadModelPackage(d, m, nullptr, false)) continue;
        ++models;
        // Built-in actions plus standalone <name>e*.mca files
        const std::string stem = f.substr(0, f.size() - 4);
        for (const auto& a : st.mcaFiles) {
            if (a.rfind(stem + "e", 0) != 0) continue;
            std::vector<uint8_t> ad;
            if (cr::readFile(base + a, ad)) cr::parseMca(ad.data(), ad.size(), 0, m.actions);
        }
        bool any = false;
        for (const auto& act : m.actions) {
            if (!cr::actionFits(m.skeleton, act)) continue;
            any = true;
            std::vector<cr::Mat4> world;
            cr::computeWorld(m.skeleton, &act, act.duration * 0.5f, world);
            ++posed;
            for (const auto& mesh : m.meshes) {
                std::vector<float> p;
                cr::skinMesh(m, mesh, world, p);
                bool nan = false;
                for (float x : p) nan |= !std::isfinite(x);
                if (nan) { ++nanPoses; break; }
                const float d0 = diagonal(mesh.positions), d1 = diagonal(p);
                if (d0 > 0 && d1 > d0 * 3) ++blowups;
                if (only) {
                    // Motion over time: mean vertex distance from the frame-0 pose, sampled
                    // across the action, as a percentage of the rest-pose size
                    std::vector<cr::Mat4> w0, wt;
                    cr::computeWorld(m.skeleton, &act, 0.0f, w0);
                    std::vector<float> p0, pt;
                    cr::skinMesh(m, mesh, w0, p0);
                    float motion = 0;
                    for (int k = 1; k <= 8; ++k) {
                        cr::computeWorld(m.skeleton, &act, act.duration * k / 8.0f, wt);
                        cr::skinMesh(m, mesh, wt, pt);
                        double sum = 0;
                        for (size_t v = 0; v + 2 < p0.size(); v += 3)
                            sum += std::hypot(pt[v] - p0[v], pt[v + 1] - p0[v + 1], pt[v + 2] - p0[v + 2]);
                        motion = std::max(motion, float(sum / std::max<size_t>(1, p0.size() / 3)));
                    }
                    std::printf("  %s %-14s dur=%4.0f  stretch p95 %5.2f%%  bbox ratio %.2f  motion %.2f%% of size\n",
                                f.c_str(), act.name.c_str(), act.duration, edgeStretchP95(mesh, p) * 100,
                                d0 > 0 ? d1 / d0 : 0, d0 > 0 ? motion / d0 * 100 : 0);
                }
            }
        }
        withAnim += any;
    }
    std::printf("models: %d, with fitting animations: %d; poses evaluated: %d, NaN poses: %d, "
                "meshes whose bounds grew >3x: %d\n", models, withAnim, posed, nanPoses, blowups);
    return fileFail || nanPoses ? 1 : 0;
}

struct MapCheck {
    int files = 0, failed = 0, floors = 0, walls = 0, verts = 0;
};

SDL_EnumerationResult SDLCALL checkMap(void* user, const char* dirname, const char* fname) {
    auto* st = static_cast<MapCheck*>(user);
    const size_t len = std::strlen(fname);
    if (len < 4 || SDL_strcasecmp(fname + len - 4, ".cmp") != 0) return SDL_ENUM_CONTINUE;
    std::vector<uint8_t> data;
    cr::Collision c;
    std::string err;
    ++st->files;
    if (!cr::readFile(std::string(dirname) + fname, data) || !cr::parseCollision(data, c, &err)) {
        ++st->failed;
        std::printf("FAIL %s: %s\n", fname, err.c_str());
        return SDL_ENUM_CONTINUE;
    }
    st->verts += int(c.vertices.size() / 3);
    for (const auto& p : c.polygons) (p.isFloor() ? st->floors : st->walls)++;
    return SDL_ENUM_CONTINUE;
}

// Every touch trigger of every map in every area, run from a new-game state: which warp it makes.
int events(const char* dirArg, bool verbose) {
    const char* langEnv = SDL_getenv("CR_LANGUAGE"); // text language to decode (default English)
    const int lang = langEnv ? SDL_atoi(langEnv) : 1;
    const std::string dir = std::string(dirArg) + (dirArg[std::strlen(dirArg) - 1] == '/' ? "" : "/");
    int triggers = 0, warps = 0, noWarp = 0, errors = 0, badTarget = 0, scripts = 0, scriptErrors = 0;
    int msgRefs = 0, msgOk = 0, msgSystem = 0, msgSystemOk = 0, msgPages = 0, maxLines = 0, longest = 0, unknown = 0;
    cr::SystemText sys;
    sys.load(dir);
    for (int area = 0; area < cr::kNumAreas; ++area) {
        cr::EventFile ef;
        std::string err;
        if (!ef.load(dir, area, &err)) { std::printf("area %d: %s\n", area, err.c_str()); ++errors; continue; }
        // every script, for decode errors
        for (int n = 0; n < 1000; ++n) {
            if (!ef.scriptEntry(n)) continue;
            cr::GameState gs;
            const cr::EventResult r = cr::runEvent(ef, n, gs, area, cr::kAreaFirstMap[area]);
            ++scripts;
            if (!r.error.empty()) { ++scriptErrors; if (verbose) std::printf("  %s script %d: %s\n", cr::kAreaEventFile[area], n, r.error.c_str()); }
            for (int id : r.messages) {
                ++msgRefs;
                std::vector<std::vector<std::string>> pages;
                if (ef.message(id, lang, nullptr, pages)) {
                    ++msgOk;
                    msgPages += int(pages.size());
                    if (SDL_getenv("CR_MESSAGE_IDS")) // ids and sizes only, no text
                        std::printf("  message %s %d: %zu pages, %zu lines on the first\n", cr::kAreaEventFile[area], id,
                                    pages.size(), pages.empty() ? size_t(0) : pages[0].size());
                    for (const auto& pg : pages) {
                        maxLines = std::max(maxLines, int(pg.size()));
                        for (const auto& l : pg) {
                            int chars = 0;
                            for (size_t i = 0; i < l.size(); ++chars) unknown += cr::nextUtf8(l, i) == 0xFFFD;
                            longest = std::max(longest, chars);
                        }
                    }
                } else if (id >= 51000) {
                    ++msgSystem;
                    msgSystemOk += sys.message(id, lang, nullptr, pages);
                }
            }
        }
        int areaEnd = cr::kNumMaps;
        for (int a = 0; a < cr::kNumAreas; ++a)
            if (cr::kAreaFirstMap[a] > cr::kAreaFirstMap[area] && cr::kAreaFirstMap[a] < areaEnd) areaEnd = cr::kAreaFirstMap[a];
        for (int map = cr::kAreaFirstMap[area]; map < areaEnd; ++map) {
            for (const cr::FieldNpc& n : ef.npcs(map)) {
                if (!n.touchTrigger()) continue;
                ++triggers;
                cr::GameState gs;
                const cr::EventResult r = cr::runEvent(ef, n.id, gs, area, map);
                if (!r.error.empty()) ++errors;
                if (r.warp.valid) {
                    ++warps;
                    const int toArea = r.warp.area < 0 ? area : r.warp.area;
                    const bool known = r.warp.map < cr::kNumMaps && toArea < cr::kNumAreas;
                    badTarget += !known;
                    if (verbose)
                        std::printf("  %s map %3d trigger %4d -> area %2d map %3d (%s) at (%.1f %.1f %.1f) %g deg\n",
                                    cr::kAreaEventFile[area], map, n.id, toArea, r.warp.map,
                                    known ? cr::kMapCollisionFile[r.warp.map] : "?", r.warp.pos[0], r.warp.pos[1],
                                    r.warp.pos[2], r.warp.angleDegrees);
                } else {
                    ++noWarp;
                    if (verbose) std::printf("  %s map %3d trigger %4d: no warp%s%s\n", cr::kAreaEventFile[area], map, n.id,
                                             r.error.empty() ? "" : ", ", r.error.c_str());
                }
            }
        }
    }
    std::printf("scripts: %d run, %d with errors; touch triggers: %d, %d warp, %d don't (new-game flags), %d errors, "
                "%d warps to maps outside the table\n", scripts, scriptErrors, triggers, warps, noWarp, errors, badTarget);
    std::printf("messages shown by scripts (first path, new game): %d, %d decoded (%d pages, at most %d lines per page, "
                "longest line %d chars, %d malformed), %d system messages (%d decoded)\n", msgRefs, msgOk, msgPages, maxLines,
                longest, unknown, msgSystem, msgSystemOk);
    return errors || scriptErrors ? 1 : 0;
}

// crtool trace <dataHD> <area> <script> [map]: the address of every instruction a script runs
// from a new-game state (instant mode), one per line - join with tools/evt.py's listing.
int trace(const char* dirArg, int area, int script, int map) {
    const std::string dir = std::string(dirArg) + (dirArg[std::strlen(dirArg) - 1] == '/' ? "" : "/");
    cr::EventFile ef;
    std::string err;
    if (!ef.load(dir, area, &err)) { std::printf("%s\n", err.c_str()); return 1; }
    cr::setEventTrace([](size_t pc, int op) { std::printf("%zx %d\n", pc, op); });
    cr::GameState gs;
    cr::EventResult r;
    if (std::getenv("CR_TRACE_HOST")) { // with a do-nothing host: on past warps, messages and battles
        cr::EventRunner runner;
        cr::EventHost host;
        if (runner.start(ef, script, gs, area, map >= 0 ? map : cr::kAreaFirstMap[area]))
            for (int k = 0; k < 20000 && runner.step(&host); ++k) {}
        r = runner.result();
    } else
        r = cr::runEvent(ef, script, gs, area, map >= 0 ? map : cr::kAreaFirstMap[area]);
    cr::setEventTrace(nullptr);
    std::printf("# %d instructions, %zu messages, warp %s%s%s\n", r.instructions, r.messages.size(),
                r.warp.valid ? std::to_string(r.warp.map).c_str() : "none", r.error.empty() ? "" : ", error: ",
                r.error.c_str());
    return 0;
}

// crtool npcs <dataHD> <area> <map>: the room's NPC / trigger list and what their scripts do
// from a new-game state after the area's script 0.
// puzzles <dataHD>: each puzzle stage's record and the NPCs that become its objects (cell, bits)
int puzzles(const char* dirArg) {
    const std::string dir = std::string(dirArg) + (dirArg[std::strlen(dirArg) - 1] == '/' ? "" : "/");
    cr::EventFile ef;
    if (!ef.load(dir, cr::kPuzzleArea)) { std::fprintf(stderr, "no puzzle area\n"); return 1; }
    for (int s = 0; s < cr::kPuzzleStages; ++s) {
        const cr::PuzzleStage& st = cr::kPuzzleStage[s];
        const int map = cr::kFirstPuzzleMap + s;
        std::printf("stage %2d map %3d %s: ability %d, esp %2d, %dx%d", s, map, cr::kMapCollisionFile[map], st.ability, st.esp,
                    st.rows, st.cols);
        int objects = 0;
        std::string line;
        for (const cr::FieldNpc& n : ef.npcs(map)) {
            if (!n.puzzle) continue;
            ++objects;
            const int col = int(std::floor((n.pos[0] - st.x0) / cr::kPuzzleCell)), row = int(std::floor((n.pos[2] - st.z0) / cr::kPuzzleCell));
            char b[64];
            std::snprintf(b, sizeof b, " %d:%03x@%d,%d", n.id, n.puzzle, row, col);
            line += b;
        }
        std::printf(", %d objects%s\n", objects, line.c_str());
    }
    return 0;
}

int npcs(const char* dirArg, int area, int map) {
    const std::string dir = std::string(dirArg) + (dirArg[std::strlen(dirArg) - 1] == '/' ? "" : "/");
    cr::EventFile ef;
    std::string err;
    if (!ef.load(dir, area, &err)) { std::printf("%s\n", err.c_str()); return 1; }
    cr::GameState base; // after the area's entry script
    cr::runEvent(ef, 0, base, area, map);
    for (const cr::FieldNpc& n : ef.npcs(map)) {
        cr::GameState gs = base;
        const cr::EventResult r = cr::runEvent(ef, n.id, gs, area, map);
        std::printf("%4d at (%7.1f %7.1f %7.1f) box %5.1f x %5.1f kind %#06x flags %4d/%4d model %3d %-14s script %s: "
                    "%d insns, %zu msgs%s\n",
                    n.id, n.pos[0], n.pos[1], n.pos[2], n.box[0], n.box[2], n.kind, n.flagShow, n.flagHide, n.model,
                    n.model >= 0 && n.model < cr::kNumNpcModels ? cr::kNpcModelFile[n.model] : "-",
                    ef.scriptEntry(n.id) ? "yes" : "no", r.instructions, r.messages.size(), r.warp.valid ? ", warps" : "");
    }
    return 0;
}

// crtool vita <cr1.psarc> [dataHD]: decodes every .gxt texture; with the iPad data, compares each
// with the .mct of the same name (mean absolute RGBA difference where both are the same size).
// crtool obb <main.*.obb> <APK or classes.dex> [name -o out]: the Android expansion file through the
// APK's table - its files, and every texture decoded; or one file written out
int obb(int argc, char** argv) {
    cr::ObbArchive a;
    std::string err;
    if (!a.open(argv[2], argv[3], &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (argc >= 6 && std::strcmp(argv[5], "-o") == 0 && argc >= 7) {
        std::vector<uint8_t> f;
        if (!a.read(argv[4], f)) { std::fprintf(stderr, "no %s\n", argv[4]); return 1; }
        return SDL_SaveFile(argv[6], f.data(), f.size()) ? 0 : 1;
    }
    if (argc >= 5 && std::strcmp(argv[4], "--names") == 0) { // the names; with the iPad's data, what each has the other lacks
        const std::vector<std::string> listed = a.names();
        std::set<std::string> obbNames(listed.begin(), listed.end());
        if (argc < 6) {
            for (const std::string& n : a.names()) std::printf("%s\n", n.c_str());
            return 0;
        }
        std::set<std::string> ipad;
        SDL_EnumerateDirectory(argv[5], [](void* u, const char*, const char* f) {
            static_cast<std::set<std::string>*>(u)->insert(f);
            return SDL_ENUM_CONTINUE;
        }, &ipad);
        int onlyIpad = 0, onlyObb = 0;
        for (const std::string& n : ipad)
            if (!obbNames.count(n)) std::printf("iPad only: %s\n", n.c_str()), ++onlyIpad;
        for (const std::string& n : obbNames)
            if (!ipad.count(n)) std::printf("OBB only: %s\n", n.c_str()), ++onlyObb;
        std::printf("%zu in the OBB, %zu in the iPad's data: %d only in the iPad's, %d only in the OBB\n", obbNames.size(), ipad.size(), onlyIpad, onlyObb);
        return 0;
    }
    std::map<std::string, int> kinds;
    int textures = 0, decoded = 0;
    std::map<int, int> formats;
    for (const std::string& n : a.names()) {
        const size_t dot = n.rfind('.');
        ++kinds[dot == std::string::npos ? "" : n.substr(dot)];
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".mct") == 0) {
            ++textures;
            std::vector<uint8_t> f;
            cr::Image img;
            cr::MctInfo mi;
            std::string why;
            if (a.read(n, f) && cr::decodeMct(f, img, &mi, &why)) ++decoded, ++formats[mi.format];
            else std::printf("  %s: %s\n", n.c_str(), why.c_str());
        }
    }
    int tracks = 0, ogg = 0;
    double seconds = 0;
    for (const std::string& n : a.names()) { // the music: Ogg Vorbis after a 1 KB header
        if (n.compare(0, 5, "j001m") != 0) continue;
        ++tracks;
        std::vector<uint8_t> f;
        int e = 0;
        stb_vorbis* v = a.read(n, f) && f.size() > 0x400 ? stb_vorbis_open_memory(f.data() + 0x400, int(f.size() - 0x400), &e, nullptr) : nullptr;
        if (!v) { std::printf("  %s: not Ogg Vorbis (%d)\n", n.c_str(), e); continue; }
        ++ogg, seconds += double(stb_vorbis_stream_length_in_samples(v)) / stb_vorbis_get_info(v).sample_rate;
        stb_vorbis_close(v);
    }
    std::printf("music: %d of %d tracks Ogg Vorbis, %.1f minutes\n", ogg, tracks, seconds / 60);
    std::printf("%zu files:", a.size());
    for (const auto& [k, v] : kinds) std::printf(" %s %d", k.c_str(), v);
    std::printf("\ntextures: %d of %d decode; formats:", decoded, textures);
    for (const auto& [k, v] : formats) std::printf(" %s %d", cr::mctFormatName(k), v);
    std::printf("\n");
    return decoded == textures ? 0 : 1;
}

int vita(const char* psarcPath, const char* ipadArg) {
    cr::PsarcArchive a;
    std::string err;
    if (!a.open(psarcPath, &err)) { std::printf("%s\n", err.c_str()); return 1; }
    const std::string ipad = ipadArg ? std::string(ipadArg) + (ipadArg[std::strlen(ipadArg) - 1] == '/' ? "" : "/") : "";
    std::vector<uint8_t> f, m;
    int ok = 0, failed = 0, compared = 0, close = 0;
    std::map<std::string, int> whyFailed;
    double worst = 0;
    std::string worstName;
    int buckets[5] = {};
    for (const std::string& name : a.names()) {
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".gxt") != 0) continue;
        cr::Image img;
        if (!a.read(name, f) || !cr::decodeGxt(f, img, &err)) { ++failed; ++whyFailed[err]; continue; }
        ++ok;
        if (ipad.empty()) continue;
        cr::Image ref;
        const std::string mct = name.substr(0, name.size() - 4) + ".mct";
        if (!cr::readFile(ipad + mct, m) || !cr::decodeMct(m, ref) || ref.width != img.width || ref.height != img.height) continue;
        double sum = 0;
        for (size_t i = 0; i < img.rgba.size(); ++i) sum += std::abs(int(img.rgba[i]) - int(ref.rgba[i]));
        const double mean = sum / double(img.rgba.size());
        ++compared;
        if (mean < 12) ++close;
        else std::printf("  %s %dx%d differs by %.1f\n", name.c_str(), img.width, img.height, mean);
        ++buckets[mean == 0 ? 0 : mean < 1 ? 1 : mean < 4 ? 2 : mean < 12 ? 3 : 4];
        if (mean > worst) { worst = mean; worstName = name; }
    }
    std::printf("vita textures: %d decoded, %d failed; %d compared with the iPad's, %d within 12/255 on average; "
                "largest difference %.1f (%s)\n", ok, failed, compared, close, worst, worstName.c_str());
    for (const auto& [why, n] : whyFailed) std::printf("  %s: %d\n", why.c_str(), n);
    if (compared)
        std::printf("  identical %d, under 1/255 %d, under 4 %d, under 12 %d, more %d\n", buckets[0], buckets[1],
                    buckets[2], buckets[3], buckets[4]);
    return failed ? 1 : 0;
}

int maps(const char* dir) {
    MapCheck st;
    if (!SDL_EnumerateDirectory(dir, checkMap, &st)) return 1;
    std::printf("collision meshes: %d parsed, %d failed; %d vertices, %d floor and %d wall/other polygons\n",
                st.files - st.failed, st.failed, st.verts, st.floors, st.walls);
    return st.failed ? 1 : 0;
}

// Every map's encounter table against the monster table, groups, formations and data files.
int battle(const char* dirArg, bool verbose) {
    const std::string dir = std::string(dirArg) + (dirArg[std::strlen(dirArg) - 1] == '/' ? "" : "/");
    cr::BattleData bd;
    if (!bd.load(dir)) { std::printf("sys.evt: no battle data\n"); return 1; }
    int used = 0;
    for (int id = 1; id <= bd.monsterCount(); ++id) used += bd.monster(id) != nullptr;
    std::printf("battle data: %d monster rows (%d used), %d groups\n", bd.monsterCount(), used, bd.groupCount());
    auto exists = [&](const std::string& name) {
        std::vector<uint8_t> f;
        return cr::readFile(dir + name, f);
    };
    int maps = 0, withTables = 0, refs = 0, badGroup = 0, badMonster = 0, badModel = 0, badFormation = 0,
        badStage = 0;
    std::set<int> groupsUsed, monstersUsed, stagesUsed;
    for (int area = 0; area < cr::kNumAreas; ++area) {
        cr::EventFile ef;
        if (!ef.load(dir, area)) continue;
        int areaEnd = cr::kNumMaps;
        for (int a = 0; a < cr::kNumAreas; ++a)
            if (cr::kAreaFirstMap[a] > cr::kAreaFirstMap[area] && cr::kAreaFirstMap[a] < areaEnd) areaEnd = cr::kAreaFirstMap[a];
        for (int map = cr::kAreaFirstMap[area]; map < areaEnd; ++map) {
            ++maps;
            cr::EncounterTable t;
            if (!cr::encounterTable(ef, map, t)) continue;
            ++withTables;
            stagesUsed.insert(t.stage);
            const bool stageOk = t.stage >= 0 && t.stage < cr::kNumBattleStages && exists(cr::kBattleStageFile[t.stage]);
            badStage += !stageOk;
            if (verbose)
                std::printf("  %s map %3d: stage %2d (%s), every %d0 units, %d%%, %zu groups\n", cr::kAreaEventFile[area],
                            map, t.stage, stageOk ? cr::kBattleStageFile[t.stage] : "?", t.distance, t.rate,
                            t.groups.size());
            for (const cr::EncounterGroup& g : t.groups) {
                ++refs;
                groupsUsed.insert(g.group);
                const std::array<int, 3> ids = bd.group(g.group);
                if (!ids[0]) { ++badGroup; continue; }
                for (int id : ids) {
                    if (!id) continue;
                    monstersUsed.insert(id);
                    const cr::MonsterDef* m = bd.monster(id);
                    if (!m) { ++badMonster; continue; }
                    if (m->model >= cr::kNumBattleModels || !exists(cr::kBattleModelFile[m->model])) ++badModel;
                }
                badFormation += bd.formation(ids) < 0;
            }
        }
    }
    std::printf("encounters: %d of %d maps have a table; %d group references (%zu groups, %zu monsters, %zu stages); "
                "%d bad groups, %d bad monsters, %d missing models, %d without a formation, %d bad stages\n",
                withTables, maps, refs, groupsUsed.size(), monstersUsed.size(), stagesUsed.size(), badGroup, badMonster,
                badModel, badFormation, badStage);
    return badGroup || badMonster || badModel || badFormation || badStage ? 1 : 0;
}

// crtool evtmsg <dir> <area> <language> <id>...: an area script's messages - lines, voices, pauses
int evtmsg(const char* dirArg, int area, int lang, int argc, char** argv) {
    const std::string dir = std::string(dirArg) + (dirArg[std::strlen(dirArg) - 1] == '/' ? "" : "/");
    cr::EventFile f;
    std::string err;
    if (!f.load(dir, area, &err)) { std::printf("%s\n", err.c_str()); return 1; }
    for (int i = 0; i < argc; ++i) {
        std::vector<std::vector<std::string>> pages;
        cr::MessageInfo info;
        const int id = std::atoi(argv[i]);
        const bool ok = f.message(id, lang, [](int) { return 0; }, pages, &info);
        std::printf("%d:%s %zu pages, %zu voices, %zu pauses, close after %d\n", id, ok ? "" : " (none)", pages.size(),
                    info.voices.size(), info.pauses.size(), info.closeAfter);
        for (const auto& pg : pages)
            for (const auto& l : pg) std::printf("   | %s\n", l.c_str());
    }
    return 0;
}

// System text lines (ids >= 51000) for UI labels: crtool sysmsg <dataHD> <language> <id>...
int sysmsg(const char* dirArg, int lang, int argc, char** argv) {
    const std::string dir = std::string(dirArg) + (dirArg[std::strlen(dirArg) - 1] == '/' ? "" : "/");
    cr::SystemText sys;
    if (!sys.load(dir)) return 1;
    for (int i = 0; i < argc; ++i) {
        std::vector<std::vector<std::string>> pages;
        const int id = std::atoi(argv[i]);
        std::string line;
        if (sys.message(id, lang, nullptr, pages))
            for (const auto& pg : pages)
                for (const auto& l : pg) line += (line.empty() ? "" : " / ") + l;
        std::printf("%d: %s\n", id, line.c_str());
    }
    return 0;
}

} // namespace

// crtool smooth <dataHD> [level]: PN-triangle smoothing of every model, checked: no new open
// edges (cracks), original vertices unmoved, bone weights summing to 1, the rest pose skinned
// back onto the smoothed positions, and how far new vertices move off the flat triangles
int smooth(const char* dirArg, int level) {
    std::string dir = dirArg;
    if (dir.back() != '/') dir += '/';
    std::vector<std::string> files;
    SDL_EnumerateDirectory(dir.c_str(), [](void* u, const char*, const char* f) {
        const size_t n = std::strlen(f);
        if (n > 4 && SDL_strcasecmp(f + n - 4, ".bin") == 0) static_cast<std::vector<std::string>*>(u)->push_back(f);
        return SDL_ENUM_CONTINUE;
    }, &files);
    std::sort(files.begin(), files.end());
    int models = 0, problems = 0;
    long before = 0, after = 0, openBefore = 0, openAfter = 0;
    double worstRest = 0, worstBulge = 0;
    auto openEdges = [](const cr::Mesh& m) { // edges (by exact position) with one face
        using P = std::array<uint32_t, 3>;
        std::map<std::pair<P, P>, int> count;
        auto key = [&](uint32_t r) {
            P b;
            std::memcpy(b.data(), &m.positions[r * 3], 12);
            return b;
        };
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
            for (int c = 0; c < 3; ++c) {
                P a = key(m.indices[t + c]), b = key(m.indices[t + (c + 1) % 3]);
                if (b < a) std::swap(a, b);
                ++count[{a, b}];
            }
        long n = 0;
        for (auto& [k, v] : count) n += v == 1;
        return n;
    };
    for (const std::string& f : files) {
        std::vector<uint8_t> data;
        cr::Model m;
        if (!cr::readFile(dir + f, data) || !cr::loadModelPackage(data, m, nullptr, false)) continue;
        ++models;
        const cr::Model orig = m;
        const cr::SmoothStats st = cr::smoothModel(m, level);
        before += st.trianglesBefore;
        after += st.trianglesAfter;
        float size = 1;
        for (int i = 0; i < 3; ++i) size = std::max(size, m.boundsMax[i] - m.boundsMin[i]);
        std::vector<cr::Mat4> world;
        cr::computeWorld(m.skeleton, nullptr, 0, world);
        for (size_t k = 0; k < m.meshes.size(); ++k) {
            const cr::Mesh &a = orig.meshes[k], &b = m.meshes[k];
            const long oa = openEdges(a), ob = openEdges(b);
            openBefore += oa * level;
            openAfter += ob;
            if (ob > oa * level) { // (fewer: zero-length edges of degenerate triangles closing up)
                std::printf("%s mesh %zu: %ld open edges, expected %ld\n", f.c_str(), k, ob, oa * level);
                ++problems;
            }
            if (std::memcmp(a.positions.data(), b.positions.data(), a.positions.size() * 4) != 0) {
                std::printf("%s mesh %zu: original vertices moved\n", f.c_str(), k);
                ++problems;
            }
            if (b.skinned)
                for (size_t p = 0; p < b.influenceCount.size(); ++p) {
                    float w = 0;
                    for (int i = 0; i < b.influenceCount[p]; ++i) w += b.influenceWeight[p * 4 + i];
                    if (b.influenceCount[p] && std::fabs(w - 1) > 1e-3f) {
                        std::printf("%s mesh %zu: weights sum to %g\n", f.c_str(), k, w);
                        ++problems;
                        break;
                    }
                }
            // At rest, skinning the smoothed bind positions should land where the original's do
            // relative to its own positions (some meshes' rest positions aren't their skinned rest)
            if (!b.posOf.empty() && !m.skeleton.nodes.empty()) {
                std::vector<float> sa, sb;
                cr::skinMesh(orig, a, world, sa);
                cr::skinMesh(m, b, world, sb);
                double offA = 0;
                for (size_t i = 0; i < sa.size(); ++i) offA = std::max(offA, double(std::fabs(sa[i] - a.positions[i])));
                if (offA < 1e-3 * size) {
                    double off = 0;
                    for (size_t i = 0; i < sb.size(); ++i) off = std::max(off, double(std::fabs(sb[i] - b.positions[i])));
                    if (off / size > 0.01 && SDL_getenv("CR_SMOOTH_DEBUG")) {
                        size_t at = 0;
                        for (size_t i = 0; i < sb.size(); ++i)
                            if (std::fabs(sb[i] - b.positions[i]) >= off * 0.999) { at = i / 3; break; }
                        std::printf("%s mesh %zu: rest off %.3g (size %.3g) at render %zu of %zu (old %zu), skinned %d, pos %u, "
                                    "skin (%g %g %g) rest (%g %g %g)\n", f.c_str(), k, off, size, at, sb.size() / 3,
                                    a.positions.size() / 3, int(b.skinned), b.posOf[at], sb[at * 3], sb[at * 3 + 1],
                                    sb[at * 3 + 2], b.positions[at * 3], b.positions[at * 3 + 1], b.positions[at * 3 + 2]);
                    }
                    worstRest = std::max(worstRest, off / size);
                }
            }
        }
        // Bulge: how far the smoothed rest pose reaches past the original's bounding box
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, lo2[3], hi2[3];
        std::memcpy(lo2, lo, 12);
        std::memcpy(hi2, hi, 12);
        for (size_t k = 0; k < m.meshes.size(); ++k) {
            const std::vector<float>&pa = orig.meshes[k].positions, &pb = m.meshes[k].positions;
            for (size_t i = 0; i < pa.size(); ++i) lo[i % 3] = std::min(lo[i % 3], pa[i]), hi[i % 3] = std::max(hi[i % 3], pa[i]);
            for (size_t i = 0; i < pb.size(); ++i) lo2[i % 3] = std::min(lo2[i % 3], pb[i]), hi2[i % 3] = std::max(hi2[i % 3], pb[i]);
        }
        for (int i = 0; i < 3; ++i)
            if (hi[i] >= lo[i]) worstBulge = std::max(worstBulge, double(std::max(lo[i] - lo2[i], hi2[i] - hi[i])) / size);
    }
    std::printf("smooth level %d: %d models, %ld -> %ld triangles (x%.2f); open edges %ld (expected %ld); "
                "rest pose off by up to %.4f%% of a model's size; bounds grow by up to %.2f%%; %d problems\n",
                level, models, before, after, before ? double(after) / double(before) : 0.0, openAfter, openBefore,
                worstRest * 100, worstBulge * 100, problems);
    return problems ? 1 : 0;
}

// crtool vitaaudio <PCSG00497 folder | its CR1/sound> [2.2.0 data]: decodes every Vita voice and
// track (.at9); with 2.2.0's data also checks the voices against its banks (sorted names = clip
// order: lengths, loudness); each track's loop seam (the samples after the loop's end against those
// at its start, as stored and moved by the encoder delay)
int vitaAudio(const char* folder, const char* data220) {
    std::string dir = folder;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    cr::PsarcArchive probe;
    for (const char* rel : {"/CR1/sound", "/sound", ""})
        if (probe.open(dir + rel + "/bgm.psarc")) { dir += rel; break; }
    int problems = 0;
    const char* banks[2] = {"voiceevnt", "voicebtl"};
    for (int b = 0; b < 2; ++b) {
        cr::PsarcArchive a;
        std::string err;
        if (!a.open(dir + "/" + banks[b] + ".psarc", &err)) { std::printf("%s: %s\n", banks[b], err.c_str()); ++problems; continue; }
        std::vector<uint8_t> table;
        std::vector<std::pair<uint32_t, uint32_t>> clips;
        if (data220 && cr::readFile(std::string(data220) + "/" + banks[b] + ".bin", table) && table.size() >= 8) {
            const uint32_t first = uint32_t(table[0]) | uint32_t(table[1]) << 8 | uint32_t(table[2]) << 16 | uint32_t(table[3]) << 24;
            for (uint32_t k = 0; k + 8 <= first && k + 8 <= table.size(); k += 8) {
                uint32_t v[2];
                std::memcpy(v, &table[k], 8);
                clips.push_back({v[0], v[1]});
            }
        }
        const std::vector<std::string> names = a.names();
        int ok = 0, lengthOff = 0;
        double vitaSq = 0, iosSq = 0, seconds = 0;
        std::vector<uint8_t> f;
        std::vector<int16_t> pcm;
        for (size_t i = 0; i < names.size(); ++i) {
            int rate = 0, ch = 0;
            if (!a.read(names[i], f) || !cr::decodeAt9(f, pcm, rate, ch, &err)) { ++problems; std::printf("  %s: %s\n", names[i].c_str(), err.c_str()); continue; }
            ++ok;
            seconds += double(pcm.size()) / ch / rate;
            if (i < clips.size()) {
                const double vs = double(pcm.size()) / ch / rate, is = clips[i].second / 2 / 20000.0;
                if (std::fabs(vs - is) > 0.05) ++lengthOff;
                if (i % 7 == 0 && clips[i].second >= 2 && uint64_t(clips[i].first) + clips[i].second <= table.size()) {
                    for (int16_t s : pcm) vitaSq += double(s) * s / double(pcm.size());
                    const int16_t* q = reinterpret_cast<const int16_t*>(&table[clips[i].first]);
                    const size_t n = clips[i].second / 2;
                    double sq = 0;
                    for (size_t k = 0; k < n; ++k) sq += double(q[k]) * q[k];
                    iosSq += sq / double(n);
                }
            }
        }
        std::printf("%s: %d of %zu decoded (%.0f s)", banks[b], ok, names.size(), seconds);
        if (!clips.empty())
            std::printf("; against 2.2.0's %zu clips: %d lengths off, loudness Vita / 2.2.0 %.3f", clips.size(), lengthOff,
                        iosSq > 0 ? std::sqrt(vitaSq / iosSq) : 0.0);
        std::printf("\n");
        problems += lengthOff + (!clips.empty() && clips.size() != names.size());
    }
    cr::PsarcArchive bgm;
    std::string err;
    if (!bgm.open(dir + "/bgm.psarc", &err)) { std::printf("bgm: %s\n", err.c_str()); return 1; }
    for (const std::string& name : bgm.names()) {
        std::vector<uint8_t> f;
        cr::At9File at9;
        std::vector<int16_t> pcm;
        int rate = 0, ch = 0;
        if (!bgm.read(name, f) || !cr::parseAt9(f, at9, &err) || !cr::decodeAt9(f, pcm, rate, ch, &err)) {
            std::printf("  %s: %s\n", name.c_str(), err.c_str());
            ++problems;
            continue;
        }
        auto seam = [&](int64_t shift) { // mean |after the end - at the start| over 0.1 s, the start moved by `shift`
            const int64_t n = rate / 10, s = at9.loopStart + shift, e = at9.loopEnd + 1 + shift;
            if (at9.loopStart < 0 || s < 0 || (e + n) * ch > int64_t(pcm.size())) return -1.0;
            double d = 0, m = 0;
            for (int64_t k = 0; k < n * ch; ++k) d += std::fabs(double(pcm[size_t(e * ch + k)]) - pcm[size_t(s * ch + k)]), m += std::fabs(double(pcm[size_t(s * ch + k)]));
            return m > 0 ? d / m : 0.0;
        };
        std::printf("  %s: %d Hz x%d, %.2f s (delay %d), loop %.3f..%.3f s, seam %.3f (as stored) / %.3f (+delay) / %.3f (-delay)\n",
                    name.c_str(), rate, ch, double(pcm.size()) / ch / rate, at9.delay, at9.loopStart / double(rate),
                    at9.loopEnd / double(rate), seam(0), seam(at9.delay), seam(-at9.delay));
    }
    std::printf("vita audio: %d problems\n", problems);
    return problems ? 1 : 0;
}

// crtool vitasfx <PCSG00497 folder | its CR1/sound> [iOS data]: decodes the Vita's sound effects
// (sndeff.bnk, HE-VAG) and, with an iOS data folder, compares each with sndeff.bin's entry of the
// same number: length, rate, channels and the waveform (correlation at the best of +-64 samples)
int vitaSfx(const char* folder, const char* ios) {
    std::string dir = folder;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    std::vector<uint8_t> f;
    for (const char* rel : {"/CR1/sound", "/sound", ""})
        if (cr::readFile(dir + rel + "/sndeff.bnk", f)) break;
    std::vector<cr::PcmSound> vita;
    std::string err;
    if (!cr::parseScreamBank(f, vita, &err)) { std::printf("sndeff.bnk: %s\n", err.c_str()); return 1; }
    int decoded = 0;
    for (const cr::PcmSound& s : vita) decoded += !s.data.empty();
    std::printf("vita sound effects: %zu sounds, %d with a wave\n", vita.size(), decoded);
    if (!ios) return decoded ? 0 : 1;
    std::vector<uint8_t> b;
    if (!cr::readFile(std::string(ios) + "/sndeff.bin", b) || b.size() < 0x800) { std::printf("no sndeff.bin\n"); return 1; }
    int same = 0, compared = 0;
    double worst = 1;
    int worstAt = -1;
    for (size_t i = 0; i < vita.size() && i < 256; ++i) {
        uint32_t off = 0, len = 0;
        std::memcpy(&off, &b[i * 8], 4);
        std::memcpy(&len, &b[i * 8 + 4], 4);
        cr::PcmSound s;
        if (!off || !len || off + size_t(len) > b.size() || !cr::parseCafPcm(&b[off], len, s) || s.bits != 16 || vita[i].data.empty()) continue;
        ++compared;
        const cr::PcmSound& v = vita[i];
        auto at = [](const cr::PcmSound& p, size_t k) {
            int16_t x;
            std::memcpy(&x, &p.data[k * 2], 2);
            return p.bigEndian ? int16_t(uint16_t(x) >> 8 | uint16_t(x) << 8) : x;
        };
        const size_t na = s.data.size() / 2 / size_t(s.channels), nb = v.data.size() / 2 / size_t(v.channels);
        const double ratio = double(v.rate) / s.rate; // (the Vita's are often 44.1 kHz where iOS's are 22.05)
        const size_t n = std::min<size_t>(std::min(na, size_t(double(nb) / ratio)), 44100);
        double best = -1, xxAll = 0;
        for (int lag = -64; lag <= 64; ++lag) {
            double xy = 0, xx = 0, yy = 0;
            for (size_t k = 64; k + 64 < n; ++k) {
                const size_t j = size_t(std::lround(double(k) * ratio)) + size_t(lag);
                if (j >= nb) break;
                const double x = at(s, k * size_t(s.channels)), y = at(v, j * size_t(v.channels));
                xy += x * y, xx += x * x, yy += y * y;
            }
            xxAll = std::max(xxAll, xx);
            if (xx > 0 && yy > 0) best = std::max(best, xy / std::sqrt(xx * yy));
        }
        if (xxAll == 0) best = 1; // (silent in iOS's: nothing to compare)
        // (the Vita's are often stereo or 44.1 kHz where iOS's are mono / 22.05; noisy ones re-encoded
        // correlate less: 0.64 at the lowest)
        const bool ok = best > 0.6 && std::fabs(double(na) * ratio - double(nb)) < 64 * ratio;
        same += ok;
        if (best < worst) worst = best, worstAt = int(i);
        if (!ok)
            std::printf("  %zu: iOS %d Hz x%d %zu frames, Vita %d Hz x%d %zu frames, correlation %.3f\n", i, s.rate, s.channels, na,
                        v.rate, v.channels, nb, best);
    }
    std::printf("against sndeff.bin: %d of %d alike (lowest correlation %.3f, sound %d)\n", same, compared, worst, worstAt);
    return same == compared ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "vitasfx") == 0) return vitaSfx(argv[2], argc > 3 ? argv[3] : nullptr);
    if (argc >= 3 && std::strcmp(argv[1], "vitaaudio") == 0) return vitaAudio(argv[2], argc > 3 ? argv[3] : nullptr);
    if (argc == 3 && std::strcmp(argv[1], "maps") == 0) return maps(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "puzzles") == 0) return puzzles(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "effects") == 0) return effects(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "matanim") == 0) return matanim(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "visdump") == 0) return visdump(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "propuse") == 0) return propuse(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "lightdump") == 0) return lightdump(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "natives") == 0) return natives(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "music") == 0) return music(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "npcloads") == 0) return npcLoads(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "events") == 0)
        return events(argv[2], argc > 3 && std::strcmp(argv[3], "-v") == 0);
    if (argc >= 6 && std::strcmp(argv[1], "evtmsg") == 0)
        return evtmsg(argv[2], std::atoi(argv[3]), std::atoi(argv[4]), argc - 5, argv + 5);
    if (argc >= 5 && std::strcmp(argv[1], "sysmsg") == 0) return sysmsg(argv[2], std::atoi(argv[3]), argc - 4, argv + 4);
    if (argc >= 3 && std::strcmp(argv[1], "battle") == 0)
        return battle(argv[2], argc > 3 && std::strcmp(argv[3], "-v") == 0);
    if (argc >= 4 && std::strcmp(argv[1], "obb") == 0) return obb(argc, argv);
    if (argc >= 3 && std::strcmp(argv[1], "vita") == 0) return vita(argv[2], argc > 3 ? argv[3] : nullptr);
    if (argc == 5 && std::strcmp(argv[1], "npcs") == 0) return npcs(argv[2], std::atoi(argv[3]), std::atoi(argv[4]));
    if (argc >= 5 && std::strcmp(argv[1], "trace") == 0)
        return trace(argv[2], std::atoi(argv[3]), std::atoi(argv[4]), argc > 5 ? std::atoi(argv[5]) : -1);
    if (argc >= 3 && std::strcmp(argv[1], "anims") == 0) return anims(argv[2], argc > 3 ? argv[3] : nullptr);
    if (argc >= 3 && std::strcmp(argv[1], "models") == 0)
        return models(argv[2], argc > 3 && std::strcmp(argv[3], "-v") == 0);
    if (argc >= 3 && std::strcmp(argv[1], "mct-info") == 0) return mctInfo(argc, argv);
    if (argc == 4 && std::strcmp(argv[1], "mct-raw") == 0) return mctRaw(argv[2], argv[3]);
    if (argc == 3 && std::strcmp(argv[1], "check") == 0) return check(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "smooth") == 0) return smooth(argv[2], argc > 3 ? std::atoi(argv[3]) : 2);
    std::fprintf(stderr,
                 "usage:\n  crtool mct-info <file.mct>...\n  crtool mct-raw <file.mct> <out.rgba>\n"
                 "  crtool check <dataHD dir>\n  crtool models <dataHD dir> [-v]\n"
                 "  crtool anims <dataHD dir> [model]\n  crtool maps <dataHD dir>\n  crtool events <dataHD dir> [-v]\n"
                 "  crtool obb <main.*.obb> <APK or classes.dex> [name -o out | --names [dataHD]]\n");
    return 2;
}
