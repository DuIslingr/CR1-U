#include "app/app.hpp"

namespace crapp {

// Field collision as a flat-shaded mesh: floors green-grey (tinted by surface type),
// walls tan, so the walkable layout reads clearly.
// Shows a field by collision file. `forcedMap` / `forcedArea` select the exact map (and so
// camera and event lists) when arriving by warp; otherwise the best-framing camera is used.
void showMap(App& app, int index, int forcedMap, int forcedArea) {
    app.renderer.resetHistory(); // a cut: the upscaler starts over
    if (app.maps.empty()) return;
    app.booting = false;
    app.titleOpen = false;
    app.renderer.destroy(app.charSelectArt); // (the title's save screen showed it)
    for (cr::Texture& t : app.titleArt) app.renderer.destroy(t); // (the title's own art: TitleClr's MctTexRelease)
    app.mapIndex = wrap(index, app.maps.size());
    unloadModel(app);
    const std::string& name = app.maps[app.mapIndex];
    std::vector<uint8_t> data;
    cr::Collision col;
    std::string err;
    if (!cr::readFile(app.dataDir + name, data) || !cr::parseCollision(data, col, &err)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", name.c_str(), err.c_str());
        updateTitle(app);
        return;
    }
    // Props for this field: dXXXX.bin next to dXXXX_10.cmp
    const std::string field = name.substr(0, name.find('_'));
    auto loadLayer = [&](const std::string& file, cr::Texture& out) {
        cr::Image img;
        if (loadImage(app, file, img)) out = app.renderer.upload(img);
    };
    // Layer names vary (the game's file list): dXXXX-b00a00, tXXXX_b00a00 (towns, d0512/d0524/
    // d0525) or a plain dXXXX.mct (d0108, d0117, d0208, d0219, d0307, d0320)
    for (const char* suffix : {"-b00a00.mct", "_b00a00.mct", ".mct"})
        if (!app.background.id) loadLayer(field + suffix, app.background);
    for (const char* suffix : {"-f00a00.mct", "_f00a00.mct"})
        if (!app.foreground.id) loadLayer(field + suffix, app.foreground);
    SDL_Log("%s layers: background %dx%d (id %u), foreground %dx%d", field.c_str(), app.background.width,
            app.background.height, app.background.id, app.foreground.width, app.foreground.height);

    app.mapProps.clear(); // (the alpha model: once the stage record is known, below)

    cr::Mesh mesh;
    const float light[3] = {0.37f, 0.86f, 0.35f};
    for (const auto& poly : col.polygons) {
        const float d = poly.normal[0] * light[0] + poly.normal[1] * light[1] + poly.normal[2] * light[2];
        const float shade = 0.55f + 0.45f * std::max(0.0f, d);
        float rgb[3];
        if (poly.isFloor()) {
            const float t = float((poly.attr1 >> 8) & 7) / 4.0f; // surface type tint
            rgb[0] = 0.40f + 0.25f * t; rgb[1] = 0.62f; rgb[2] = 0.42f - 0.15f * t;
        } else {
            rgb[0] = 0.75f; rgb[1] = 0.58f; rgb[2] = 0.40f;
        }
        const uint32_t base = uint32_t(mesh.positions.size() / 3);
        for (int i = 0; i < poly.count; ++i) {
            const float* v = &col.vertices[size_t(poly.index[i]) * 3];
            mesh.positions.insert(mesh.positions.end(), {v[0], v[1], v[2]});
            mesh.uvs.insert(mesh.uvs.end(), {0.0f, 0.0f});
            for (int c = 0; c < 3; ++c) mesh.colors.push_back(uint8_t(std::min(255.0f, rgb[c] * shade * 255.0f)));
            mesh.colors.push_back(255);
        }
        for (int i = 1; i + 1 < poly.count; ++i) mesh.indices.insert(mesh.indices.end(), {base, base + i, base + i + 1});
    }
    app.collision = app.renderer.upload(mesh);

    // Cameras: a collision file can be shared by several map numbers (p0001_10 serves
    // the 16 puzzle rooms and a dungeon room). Pick the camera that frames it best;
    // M cycles through the others.
    app.stageCams.clear();
    app.stageFit.clear();
    app.stageCamBorrowed = false;
    std::vector<int> candidates = forcedMap >= 0 ? std::vector<int>{forcedMap} : cr::mapNumbersFor(name);
    auto tryCandidates = [&]() {
    for (int mapNo : candidates) {
        cr::StageCamera sc;
        if (!(forcedArea >= 0 && cr::loadStageCamera(app.dataDir, mapNo, sc, nullptr, forcedArea)) &&
            !cr::loadStageCamera(app.dataDir, mapNo, sc))
            continue;
        const float aspect = sc.viewHeight > 0 ? float(sc.viewWidth) / float(sc.viewHeight) : 1.0f;
        const cr::Mat4 vp = cr::Mat4::perspective(sc.fovDegrees * 3.14159265f / 180.0f, aspect, 10.0f, 4000.0f) *
                            cr::Mat4::lookAt({sc.eye[0], sc.eye[1], sc.eye[2]},
                                             {sc.target[0], sc.target[1], sc.target[2]}, {0, 1, 0});
        int inside = 0;
        const size_t nv = col.vertices.size() / 3;
        for (size_t v = 0; v < nv; ++v) {
            const float* q = &col.vertices[v * 3];
            const float* m = vp.m;
            const float cx = m[0] * q[0] + m[4] * q[1] + m[8] * q[2] + m[12];
            const float cy = m[1] * q[0] + m[5] * q[1] + m[9] * q[2] + m[13];
            const float cw = m[3] * q[0] + m[7] * q[1] + m[11] * q[2] + m[15];
            if (cw > 0 && std::fabs(cx / cw) <= 1.05f && std::fabs(cy / cw) <= 1.05f) ++inside;
        }
        app.stageCams.push_back(sc);
        app.stageFit.push_back(nv ? float(inside) / float(nv) : 0.0f);
    }
    };
    tryCandidates();
    if (app.stageCams.empty()) {
        // e.g. p0002/p0004/p0006-8: puzzle-room variants with no record of their own
        candidates.clear();
        for (int mapNo = 0; mapNo < cr::kNumMaps; ++mapNo)
            if (name.compare(0, 2, cr::kMapCollisionFile[mapNo], 2) == 0 && name != cr::kMapCollisionFile[mapNo])
                candidates.push_back(mapNo);
        tryCandidates();
        app.stageCamBorrowed = !app.stageCams.empty();
    }
    app.haveStageCam = !app.stageCams.empty();
    app.stageCamIndex = 0;
    for (size_t k = 1; k < app.stageFit.size(); ++k)
        if (app.stageFit[k] > app.stageFit[app.stageCamIndex] + 1e-4f) app.stageCamIndex = int(k);
    if (app.haveStageCam) app.stageCam = app.stageCams[app.stageCamIndex];
    if (app.haveStageCam && app.stageCam.scripted) {
        // its own FOV and view, the script's eye and target - until the script sets them, the best
        // framing among the maps sharing its collision's name (as a room with no record borrows)
        cr::StageCamera own = app.stageCam;
        std::vector<cr::StageCamera> mine = app.stageCams;
        std::vector<float> fit = app.stageFit;
        candidates.clear();
        for (int mapNo = 0; mapNo < cr::kNumMaps; ++mapNo)
            if (name.compare(0, 2, cr::kMapCollisionFile[mapNo], 2) == 0 && mapNo != own.mapNumber) candidates.push_back(mapNo);
        app.stageCams.clear(), app.stageFit.clear();
        tryCandidates();
        int best = -1;
        for (size_t k = 0; k < app.stageCams.size(); ++k)
            if (!app.stageCams[k].scripted && (best < 0 || app.stageFit[k] > app.stageFit[size_t(best)] + 1e-4f)) best = int(k);
        if (best >= 0)
            for (int i = 0; i < 3; ++i) own.eye[i] = app.stageCams[size_t(best)].eye[i], own.target[i] = app.stageCams[size_t(best)].target[i];
        app.stageCams = std::move(mine), app.stageFit = std::move(fit);
        app.stageCams[size_t(app.stageCamIndex)] = app.stageCam = own;
    }
    // fldread_prc (0x3e290): the alpha model (the occluders, model slot 0) the stage record's +0x40
    // names (_stgalphaname) - the rooms whose field has a model of the same name and no such entry
    // (d0102, d0405, ...: see fieldRenderSelfTest) have none -; a room without a record of its own: the
    // field's namesake
    if (app.haveStageCam && !app.stageCamBorrowed) {
        if (app.stageCam.alphaModel >= 0 && app.stageCam.alphaModel < cr::kNumStageAlphaModels)
            app.mapProps = cr::kStageAlphaModel[app.stageCam.alphaModel];
    } else if (std::find(app.models.begin(), app.models.end(), field + ".bin") != app.models.end()) {
        app.mapProps = field + ".bin";
    }
    if (!app.mapProps.empty()) {
        loadModelFile(app, app.mapProps, app.model, false);
        mdlMemRules(app.model); // (read_mdl_set -> mdl_mdl_init -> read_mdl_mem_mcm)
    }
    if (SDL_getenv("CR_STAGE_LOG")) // (debugging: which stage camera a room took)
        std::printf("stage camera for %s (map %d area %d): %zu candidates%s, took map %d area %d fov %.1f view %dx%d\n", name.c_str(), forcedMap,
                    forcedArea, app.stageCams.size(), app.stageCamBorrowed ? " (borrowed)" : "", app.haveStageCam ? app.stageCam.mapNumber : -1,
                    app.haveStageCam ? app.stageCam.area : -1, app.stageCam.fovDegrees, app.stageCam.viewWidth, app.stageCam.viewHeight);
    app.collisionPolys = int(col.polygons.size());
    // Frame the collision (props live inside it)
    const auto& b0 = col.boundsMin;
    const auto& b1 = col.boundsMax;
    app.model.center = {(b0[0] + b1[0]) / 2, (b0[1] + b1[1]) / 2, (b0[2] + b1[2]) / 2};
    const cr::Vec3 ext{b1[0] - b0[0], b1[1] - b0[1], b1[2] - b0[2]};
    app.model.radius = std::max(0.5f * std::sqrt(cr::dot(ext, ext)), 0.01f);
    app.collisionData = std::move(col);
    app.mapNumber = app.haveStageCam ? app.stageCam.mapNumber : (candidates.empty() ? -1 : candidates[0]);
    app.area = app.haveStageCam ? app.stageCam.area : cr::areaForMap(app.mapNumber, forcedArea);
    if (forcedMap >= 0) { app.mapNumber = forcedMap; app.area = forcedArea >= 0 ? forcedArea : app.area; }
    // (_fldread_prc 37795-37801: area 1's puzzle rooms scroll over the whole 1024 x 1024 painting, the
    // record's rect replaced on the map's first load - FieldCamera::calcScroll follows the head both ways)
    if (app.area == 1 && app.haveStageCam) {
        const int whole[4] = {0, 0, 1024, 1024};
        for (cr::StageCamera& c : app.stageCams) std::copy(whole, whole + 4, c.rect);
        std::copy(whole, whole + 4, app.stageCam.rect);
    }
    enterMap(app);
    app.playerByReadMdl = false; // (fldread_prc's read_mdl_set)
    placePlayer(app);
    updateTitle(app);
}

void showMap(App& app, int index) { showMap(app, index, -1, -1); }

// Kill 0 hides the party leader for a scene; Born 0 brings it back
bool playerShown(const App& app) {
    auto it = app.npcShown.find(0);
    return it == app.npcShown.end() || it->second;
}

// _ChkNpcCreateFlag: the row's show flag (if any) set and its hide flag (if any) clear
bool npcCreateFlags(const App& app, const cr::FieldNpc& n) {
    return (n.flagShow < 0 || app.game.flag(n.flagShow)) && (n.flagHide < 0 || !app.game.flag(n.flagHide));
}

// npc_setup's story actors (2.2.0 0x3b6a4: id - 400 < 50), killed as a room loads whatever their
// flags - not in the puzzle rooms (area 1)
bool npcStoryActor(const App& app, int id) { return app.area != 1 && id >= 400 && id < 450; }

// npc_setup (2.2.0 0x3b6a4, at every field load - a battle's end too: prc's load flags | 0xd): each
// NPC of the room shown (npc ctl +0x8c bit 0 clear) when its create flags pass and it isn't a story
// actor. Only Born / Kill change that until the next load - a flag set mid-room shows or hides
// nothing (_ChkNpcDisp reads the bit alone)
void npcSetup(App& app) {
    app.npcShown.clear();
    for (const cr::FieldNpc& n : app.npcs) app.npcShown[n.id] = npcCreateFlags(app, n) && !npcStoryActor(app, n.id);
    app.examineLinger.clear();
}

bool npcActive(const App& app, const cr::FieldNpc& n) { // _ChkNpcDisp
    if (auto it = app.npcShown.find(n.id); it != app.npcShown.end()) return it->second;
    return npcCreateFlags(app, n); // (an NPC npc_setup never saw)
}


// After a battle (prc: the field read back with load flags | 0xd - npc_read_setup clears the npc
// records, then npc_setup): every Born / Kill of the room undone, the NPCs a script read in
// (NpcMdlRead) gone
void resetFieldNpcs(App& app) {
    std::set<int> room;
    if (app.mapNumber >= 0 && app.area >= 0)
        for (const cr::FieldNpc& n : app.events.npcs(app.mapNumber)) room.insert(n.id);
    npcSetup(app);
    for (const cr::FieldNpc& n : app.npcs)
        if (!room.count(n.id)) app.npcShown[n.id] = false;
    buildNpcDraws(app);
}


// npcActive, but a puzzle room's objects show as the puzzle has them
bool npcVisible(const App& app, size_t index) {
    if (app.puzzle.stage >= 0)
        for (const App::Puzzle::Obj& o : app.puzzle.objs)
            if (o.npc == int(index)) return o.shown;
    return index < app.npcs.size() && npcActive(app, app.npcs[index]);
}

// npcVisible, or a red / blue block put away (npc_kill, then mdl_set_alpha keeps it drawn)
bool npcDrawn(const App& app, size_t index) {
    if (app.puzzle.stage >= 0)
        for (const App::Puzzle::Obj& o : app.puzzle.objs)
            if (o.npc == int(index)) return o.shown || o.ghost || o.fadeOut > 0;
    return npcVisible(app, index);
}

// Loads the area's event file if needed, the map's NPC list, and the trigger-box overlay.
// __bgmgo: the music command. A play request (no stop / fade bits) made on the field (G+0x2c0 bit
// 6: not in a battle) is kept in G+0x358e0 - what Uty_GetBgmNo reads and the field plays again after a
// battle; it restarts the track even when that one plays already (_setbgm_caf)
void playBgm(App& app, uint32_t v) {
    if (!(v & 0x2e000000u) && !app.battle.active) app.bgmNo = v;
    app.music.command(v);
}

// FieldScene::onTerm -> PuzzleScene::term destroys the old room's puzzle objects;
// npc_read_setup also clears its autoctl records. No task may retain an old NPC index.
void endFieldTasks(App& app) {
    app.moves.clear();
    app.rescales.clear();
    app.puzzle = App::Puzzle();
    app.puzzleBanner = App::PuzzleBanner();
    app.puzzleStartPending = false;
    app.playerAlpha = app.playerAlphaFrom = app.playerAlphaTo = 1;
    app.playerFadeFrames = app.playerFadeLeft = 0;
    app.playerFaded = false; // (fldread_prc reads the leader's model again)
}

void enterMap(App& app) {
    endFieldTasks(app);
    app.npcs.clear();
    app.fieldEffects.clear();
    app.frameEffects.clear();
    app.npcWarps.clear();
    app.renderer.destroy(app.triggerMesh);
    app.lastEvent.clear(); // (lastKick kept: checkEventKick alone clears it, the leader in no box)
    app.npcShown.clear();
    app.examineLinger.clear();
    app.placedNpcs.clear();
    app.actorSlots.clear();
    app.cam = App::ScriptCam();
    // (_fldread_prc 37738-37763: the stage record's eye and target are the live camera's on a load - in a
    // room without a painting the scene's camera from the start: FrmPanCam slides from it (18727-18751) and
    // setCamera3D draws through the doubled stage view (39553-39700) before any FrmSetCam)
    if (!app.background.id && app.haveStageCam) {
        const cr::StageCamera& sc = app.stageCam;
        app.cam.eye = {sc.eye[0], sc.eye[1], sc.eye[2]};
        app.cam.target = {sc.target[0], sc.target[1], sc.target[2]};
        app.cam.set = true;
    }
    app.crystalCam = App::CrystalCam();
    app.scroll = App::FieldScroll();
    if (app.area < 0 || app.mapNumber < 0) return;
    if (app.events.area() != app.area) {
        std::string err;
        if (!app.events.load(app.dataDir, app.area, &err)) {
            // Once per area: 2.2.0 dropped the two test areas (j001_t91 / j001_d91)
            static std::set<int> reported;
            if (reported.insert(app.area).second) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", err.c_str());
            return;
        }
    }
    app.npcs = app.events.npcs(app.mapNumber);
    app.npcNativesPending = true; // (their rows' natives on the first step: the fire walls' Uty_FireWall_Init)
    { // fldencount_init
        cr::EncounterTable table;
        const bool have = cr::encounterTable(app.events, app.mapNumber, table);
        app.encounters.enterMap(have ? &table : nullptr);
    }
    app.pendingEntry = app.area != app.entryArea ? App::Entry::Area : App::Entry::Room;
    if (app.area != app.entryArea) app.game.frmScene = app.game.frmRecord = 0; // offsets into the old file
    app.entryArea = app.area;
    // Music: none here - _fldread_prc plays a track only on the way back from a battle; a map or
    // area change leaves it to the entry scripts (their Bgm)
    // Treasure boxes (TreasureBoxManager): a dungeon's (any area but the boot area, the puzzle
    // rooms, the four Arks and t91) are drawn again each time the World Select leads into one
    // (fldread_prc -> _InitDungeon -> randomSelect), at the route's band, the draw kept in the save
    // and its boxes taken there; flags set per map (setup)
    const bool dungeonBoxes = app.area > 1 && app.area != 7 && app.area != 13 && app.area != 19 && app.area != 25 && app.area != 26;
    const int route = std::clamp(app.playerChar / 2, 0, 3);
    app.game.route = route;
    auto& pick = app.game.treasure[size_t(route)];
    if (dungeonBoxes && (app.treasureReroll || pick.empty())) {
        pick.clear();
        for (const cr::TreasureBox& b : cr::drawTreasureBoxes(app.events, [&app](int n) { return n > 0 ? app.rng.below(n) : 0; },
                                                             app.game.bands[size_t(route)], route))
            pick.push_back({b.id, b.item});
    }
    app.treasureReroll = false; // (cleared by any area's events loading)
    std::vector<cr::TreasureBox>& boxes = app.treasureBoxes[app.area];
    boxes.clear();
    if (dungeonBoxes)
        for (const auto& e : pick) boxes.push_back({e[0], e[1]});
    cr::applyTreasureFlags(app.events, app.npcs, boxes, app.game, true);
    npcSetup(app); // (npc_setup at each field load)
    app.npcDraws.clear(); // (npc_setup reads their models again: no fade carried over from the last room)
    app.npcHeld.clear();
    puzzle::enter(app); // PuzzleScene::init in a puzzle room (stage -1 elsewhere)
    app.puzzleStartPending = app.puzzle.stage >= 0;
    buildNpcDraws(app);
    cr::Mesh mesh;
    for (const cr::FieldNpc& n : app.npcs) {
        bool warps = false;
        if (n.touchTrigger()) {
            cr::GameState trial = app.game; // dry run on a copy: does touching it lead somewhere?
            warps = cr::runEvent(app.events, n.id, trial, app.area, app.mapNumber).warp.valid;
        }
        app.npcWarps.push_back(warps);
        if (!n.touchTrigger() || !npcActive(app, n)) continue;
        // Box from the floor up: magenta = exit, yellow = other touch event
        const float cx = n.boxCenterX(), cz = n.boxCenterZ(), hx = n.box[0] / 2, hz = n.box[2] / 2;
        const float y0 = n.pos[1] + n.offset[1], y1 = y0 + std::max(n.box[1], 1.0f);
        const uint8_t rgb[3] = {uint8_t(warps ? 230 : 240), uint8_t(warps ? 60 : 210), uint8_t(warps ? 220 : 40)};
        const float c[8][3] = {{cx - hx, y0, cz - hz}, {cx + hx, y0, cz - hz}, {cx + hx, y0, cz + hz}, {cx - hx, y0, cz + hz},
                               {cx - hx, y1, cz - hz}, {cx + hx, y1, cz - hz}, {cx + hx, y1, cz + hz}, {cx - hx, y1, cz + hz}};
        static const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
        for (const auto& f : faces) {
            const uint32_t base = uint32_t(mesh.positions.size() / 3);
            for (int k : f) {
                mesh.positions.insert(mesh.positions.end(), {c[k][0], c[k][1], c[k][2]});
                mesh.uvs.insert(mesh.uvs.end(), {0.0f, 0.0f});
                mesh.colors.insert(mesh.colors.end(), {rgb[0], rgb[1], rgb[2], 255});
            }
            mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
    if (!mesh.indices.empty()) app.triggerMesh = app.renderer.upload(mesh);
}

// The touch trigger box containing the player, 0 if none (RectWrapper::checkEventKick: x/z only).
int triggerAt(const App& app, const cr::Vec3& p) {
    for (const cr::FieldNpc& n : app.npcs)
        if (n.touchTrigger() && npcActive(app, n) && std::fabs(p.x - n.boxCenterX()) <= n.box[0] / 2 &&
            std::fabs(p.z - n.boxCenterZ()) <= n.box[2] / 2)
            return n.id;
    return 0;
}

// Arrives at a warp destination: loads the map if it changes, places and turns the player.
bool performWarp(App& app, const cr::WarpRequest& w, int fromEvent) {
    const int area = w.area >= 0 ? w.area : app.area;
    if (w.map < 0 || w.map >= cr::kNumMaps) return false;
    const auto it = std::find(app.maps.begin(), app.maps.end(), std::string(cr::kMapCollisionFile[w.map]));
    if (it == app.maps.end()) return false;
    const int fromMap = app.mapNumber;
    const bool reload = w.map != app.mapNumber || area != app.area;
    // (a script's Warp naming an area - its own too - for another map: load flags 0x22, the area's script 0 again)
    if (reload && fromEvent >= 0 && w.area >= 0) app.entryArea = -1;
    if (reload) {
        showMap(app, int(it - app.maps.begin()), w.map, area);
        // FieldScene::onLoading calls SetMask2(1): retain the outgoing mask's
        // colour and reveal the loaded field over 500 ms. Area-changing scripts
        // end at Warp and rely on this, notably puzzle crystals' white-out.
        app.fadeFrom = 1, app.fadeTo = 0, app.fadeFrames = 10, app.fadeT = 0;
        // A script's warp within its area goes on with the script, and no room entry script
        // (EvrStart 5) follows it: fldread_prc runs that only on the load flag's bit 3, which
        // __EvtWarp never sets (0x22 / 1 / 4) - the lost battle's return does (prc: | 0xd)
        if (fromEvent >= 0 && app.pendingEntry == App::Entry::Room) app.pendingEntry = App::Entry::None;
    }
    if (!app.havePlayer) return false;
    cr::Vec3 p{w.pos[0], w.pos[1], w.pos[2]};
    float y;
    if (cr::floorHeight(app.collisionData, p.x, p.y, p.z, 5.0f, y) ||
        cr::floorHeight(app.collisionData, p.x, p.y, p.z, 1000.0f, y)) {
        p.y = y;
    } else {
        // A few destinations sit just past a floor edge (the original's floor search snaps
        // them in); take the nearest floor point within 20 units, else keep the spawn point
        float best = 1e30f;
        cr::Vec3 q = app.walker.pos;
        for (float dx = -20; dx <= 20; dx += 0.5f)
            for (float dz = -20; dz <= 20; dz += 0.5f)
                if (dx * dx + dz * dz < best &&
                    cr::floorHeight(app.collisionData, p.x + dx, p.y, p.z + dz, 1000.0f, y)) {
                    best = dx * dx + dz * dz;
                    q = {p.x + dx, y, p.z + dz};
                }
        p = q;
    }
    app.walker.pos = p;
    // (__EvtWarp to the map in play writes the position alone - gac+0xd6c..0xd74 - the facing kept;
    // a load turns the leader as FieldPCCtrl::reset does: DegToRad)
    if (reload) app.walker.heading = w.angleDegrees * 3.14159265f / 180.0f, app.leaderYawSet = false;
    if (reload) app.loadPoint[0] = p.x, app.loadPoint[1] = p.y, app.loadPoint[2] = p.z, app.loadPoint[3] = w.angleDegrees;
    app.walker.gait = cr::Gait::Stand;
    // (lastKick as it was - the warping box's id: a box of another id where he lands kicks once the
    // scripts end)
    app.lastWarp = "warp: event " + std::to_string(fromEvent) + " map " + std::to_string(fromMap) + " -> " +
                   std::to_string(w.map);
    SDL_Log("%s (%s) at (%.1f %.1f %.1f) facing %g", app.lastWarp.c_str(), cr::kMapCollisionFile[w.map], p.x, p.y, p.z,
            w.angleDegrees);
    updateTitle(app);
    return true;
}

// An NPC whose window can be up: kind bit 3, displayed, not an opened box (FollowWindow::
// isOpenedTreasureBox, 2.2.0 0x5e2a8: its lid's, id + 1, hide flag set - no badge, no examine)
bool npcExaminable(const App& app, const cr::FieldNpc& n) {
    if (!(n.kind & 8) || !npcActive(app, n)) return false;
    if (n.pos[1] >= 900.0f || (n.pos[0] == 0 && n.pos[1] == 0 && n.pos[2] == 0)) return false; // (story actors waiting)
    if (n.treasure & 3) {
        cr::NpcRow lid;
        if (app.events.npcRow(n.id + 1, lid) && lid.flagHide >= 0 && app.game.flag(lid.flagHide)) return false;
    }
    return true;
}

// RectWrapper::checkFollowWindow / checkFlagWindow: the leader inside the NPC's box - about npc_setup's
// position plus the offset - grown by the walker radius + 1.5
bool npcInReach(const App& app, const cr::FieldNpc& n) {
    if (!npcExaminable(app, n)) return false;
    const float hx = n.box[0] / 2 + app.walker.radius + 1.5f, hz = n.box[2] / 2 + app.walker.radius + 1.5f;
    return std::fabs(app.walker.pos.x - n.boxCenterX()) <= hx && std::fabs(app.walker.pos.z - n.boxCenterZ()) <= hz;
}

// A field frame of the windows: one put up (FollowWindow::set, 2.2.0 0x5e324 / FlagWindow's) for each
// NPC in reach lasts 10 frames (FollowWindow::update counts it down), so it lingers a moment after
// the leader steps out; the checks don't run while a script holds the field
void stepExamineWindows(App& app) {
    for (auto it = app.examineLinger.begin(); it != app.examineLinger.end();)
        it = --it->second <= 0 ? app.examineLinger.erase(it) : std::next(it);
    if (!app.havePlayer || app.runner.running()) return;
    for (const cr::FieldNpc& n : app.npcs)
        if (npcInReach(app, n)) app.examineLinger[n.id] = 10;
}

// The NPC the "!" button examines or talks to. FollowWindow::update (objects: kind & 0x18 == 8) runs
// before FlagWindow::update (people: 0x18), and an examine from the first starts its script, which
// holds the second back: an object in reach wins over a person. Each takes, of its windows up (in
// reach, or lingering), the nearest by 3D distance from the box's centre to the leader.
const cr::FieldNpc* interactTarget(const App& app) {
    if (!app.havePlayer) return nullptr;
    for (int people = 0; people < 2; ++people) {
        const cr::FieldNpc* best = nullptr;
        float bestD = 1e30f;
        for (const cr::FieldNpc& n : app.npcs) {
            if (bool(n.kind & 0x10) != bool(people)) continue;
            const auto up = app.examineLinger.find(n.id);
            if (!(up != app.examineLinger.end() && npcExaminable(app, n)) && !npcInReach(app, n)) continue;
            const float dx = app.walker.pos.x - n.boxCenterX(), dy = app.walker.pos.y - (n.base(1) + n.offset[1]),
                        dz = app.walker.pos.z - n.boxCenterZ();
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < bestD) bestD = d, best = &n;
        }
        if (best) return best;
    }
    return nullptr;
}

// Examines / talks to the NPC in reach, as FollowWindow::TouchFollowWindowUtil does: a treasure
// box sets var 11 = box id, var 10 = its item, and runs the script of its lid (id + 1);
// anything else runs its own script.
bool interact(App& app, bool instant) {
    if (app.runner.running()) return false;
    const cr::FieldNpc* n = interactTarget(app);
    if (!n) return false;
    int script = n->id;
    const std::string what = n->model >= 0 && n->model < cr::kNumNpcModels ? cr::kNpcModelFile[n->model] : "trigger";
    if (n->treasure & 3) {
        cr::NpcRow row;
        app.game.vars[11] = n->id;
        app.game.vars[10] = app.events.npcRow(n->id, row) && row.item >= 0
                                ? row.item
                                : cr::treasureItem(app.treasureBoxes[app.area], n->id);
        script = n->id + 1;
    }
    const int id = n->id;
    app.examineLinger.erase(id); // (_FollowWindowOff)
    menuSound(app, se::kChoose); // TouchFollowWindowUtil (2.2.0: before the EvtStart): __effgo(2) on every examine and talk
    if (!instant) { // played over the next frames (animations, item window, fades)
        app.runnerWhat = "examined " + what.substr(0, what.size() - 4) + " (" + std::to_string(id) + ")";
        app.lastEvent = app.runnerWhat;
        app.runner.start(app.events, script, app.game, app.area, app.mapNumber);
        updateTitle(app);
        return true;
    }
    const cr::EventResult r = cr::runEvent(app.events, script, app.game, app.area, app.mapNumber);
    if (!r.error.empty()) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "event %d: %s", script, r.error.c_str());
    app.lastEvent = "examined " + what.substr(0, what.size() - 4) + " (" + std::to_string(id) + ")";
    for (int item : r.itemsGained) { // item names: sys.evt block 0 (descriptions are block 1)
        std::vector<std::vector<std::string>> name;
        app.lastEvent += ", got " + itemName(app, item);
    }
    SDL_Log("%s, script %d, %d instructions, %zu messages", app.lastEvent.c_str(), script, r.instructions,
            r.messages.size());
    showMessages(app, r.messages);
    if (r.warp.valid && performWarp(app, r.warp, script)) return true;
    // Scripts change flags (opened boxes hide their lid): refresh what is shown and solid
    buildNpcDraws(app);
    updateTitle(app);
    return true;
}

// An item's name (sys.evt block 0; descriptions are block 1)
// Whether `language` can be shown: the data has it, and Japanese / Chinese / Korean need a CJK font.
bool languageUsable(const App& app, int language) {
    if (language < 0 || language >= cr::kLanguages || !app.systemText.hasLanguage(language)) return false;
    return (language >= 1 && language <= 4) || app.fontCjk.loaded() || (language == 0 && app.fontCjkBold.loaded());
}

// FUN_000373e4 (2.2.0 0x373e4, decomp 32921): the language is Japanese, Chinese or Korean (0, 5, 6, 7) -
// the menus' 14-unit text, the message window's own margins; false for English, French, German, Spanish
bool cjkTextLanguage(const App& app) {
    const int l = app.language;
    return l >= 0 && l <= 7 && ((1 << l) & 0xe1) != 0;
}

// The language's face (2.2.0 FUN_000375bc, decomp 32980-33035: UIFont fontWithName:size:):
// Japanese HiraKakuProN-W6 - the CJK font's bold weight (Noto Sans CJK JP Bold; Regular if there is
// none), its line as Hiragino's (OS/2 ascent 0.88 em) -, every other language ArialMT - a face with
// Arial's metrics, the glyphs it lacks (Chinese, Korean) from the CJK font's regional face (SC / TC /
// KR) on its baseline, as iOS's font fallback draws them. With no Arial-metric face the CJK font
// draws everything (its Latin wider than Arial's).
void applyLanguage(App& app) {
    if (!languageUsable(app, app.language)) app.language = 1;
    static const char* const kRegion[cr::kLanguages] = {"JP", "JP", "JP", "JP", "JP", "SC", "TC", "KR"};
    app.fontCjk.selectRegion(kRegion[app.language]);
    app.fontCjkBold.selectRegion(kRegion[app.language]);
    const bool japanese = app.language == 0;
    const cr::Font& face = japanese ? (app.fontCjkBold.loaded() ? app.fontCjkBold : app.fontCjk)
                           : app.fontLatin.loaded() ? app.fontLatin : app.fontCjk;
    if (face.path() != app.font.path() || face.generation() != app.fontFaceGeneration) {
        app.font.assign(face);
        app.fontFaceGeneration = face.generation();
    }
    app.font.setFallback(&face == &app.fontLatin && app.fontCjk.loaded() ? &app.fontCjk : nullptr);
    app.font.setTypoMetrics(japanese);
}

std::string itemName(const App& app, int item) {
    std::vector<std::vector<std::string>> name;
    if (app.systemText.message(cr::itemNameId(item), app.language, nullptr, name) && !name.empty() && !name[0].empty())
        return name[0][0];
    return "item " + std::to_string(item);
}

// Turns the dialogue page (E / Enter); the last page read stays available for a following choice.
void resetTyping(App& app) {
    app.dialogueTyped = app.dialogueHold = app.dialoguePause = 0;
    app.dialogueInstant = false;
}

// PrcWin a frame: __WinOpen / __WinOpen2 take the window's alpha 0 -> 255 over 4 frames (t 4..0),
// __WinClose / __WinClose2 255 -> 0 over 5 (t 0..4), then it is gone
void stepMsgWin(App& app) { stepMsgWin(app.msgWin); }
void stepMsgWin(App::MsgWin& w) {
    switch (w.state) {
    case 1: w.t = 4, w.state = 2; break;
    case 2:
        --w.t;
        w.alpha = float(4 - w.t) / 4.0f;
        if (w.t <= 0) w.state = 3, w.alpha = 1;
        break;
    case 4: w.t = 0, w.state = 5; break;
    case 5:
        w.alpha = float(4 - w.t) / 4.0f;
        if (++w.t > 4) w.state = 6, w.alpha = 0;
        break;
    default: break;
    }
}

// `tapped`: the reader's tap turned it (PrcMsg's 0x12 / its last page's wait); a page the window
// clears or closes by itself (0x1a then 0x10 / 0x14) sets no flag
void advanceDialogue(App& app, bool tapped) {
    if (app.dialogue.empty()) return;
    // (the story line is updateVoices' to cut: _PrcMsg 25241-25247 / 25308-25313 cut it on the tap
    // unless the page's 0x1e 0xffff kept it, and a 0x13 / 0x14 end leaves it)
    app.sound.stopVoiceEffects();
    app.voiceQueue.clear();
    if (tapped) app.game.setFlag(4075, true); // PrcMsg: a page turned (or the last one closed) - MsgUty_TexDispProc's cue
    resetTyping(app);
    if (++app.dialoguePage < app.dialogue.size()) {
        // A page keeping the lines before it (0x12 without 0x10) starts with them out, typing on
        // (0x11) or out at once (anything else: _PrcMsg leaves the typing flag down)
        const cr::MessageInfo* next = app.dialoguePage < app.dialogueInfo.size() ? &app.dialogueInfo[app.dialoguePage] : nullptr;
        const int keep = next ? next->keep : 0;
        app.dialogueTyped = keep;
        app.dialogueInstant = next && next->instant;
        // (PrcMsg: the 0x12's frame, then the 0x10's, then the next page's first character; a kept
        // page's 0x11 is its first unit)
        app.dialogueHold = keep > 0 ? 1 : 2;
        return;
    }
    app.lastPage = app.dialogue.back();
    app.lastPageInfo = app.dialogueInfo.size() == app.dialogue.size() ? app.dialogueInfo.back() : cr::MessageInfo();
    app.dialogue.clear();
    app.dialoguePage = 0;
}

// A page's characters, as PrcMsg types them (UTF-8 lead bytes; not the lines' \x01 marks), and its
// line breaks: each 0x11 takes PrcMsg a frame of its own, typing or not
int pageChars(const std::vector<std::string>& page) {
    int n = page.size() > 1 ? int(page.size()) - 1 : 0;
    for (const std::string& l : page)
        for (unsigned char ch : l) n += (ch & 0xC0) != 0x80 && ch != 0x01;
    return n;
}

// Whether the page's unit `at` (as pageChars counts them) is a line break
bool pageBreakAt(const std::vector<std::string>& page, int at) {
    for (size_t k = 0; k < page.size(); ++k) {
        int n = 0;
        for (unsigned char ch : page[k]) n += (ch & 0xC0) != 0x80 && ch != 0x01;
        if (at < n) return false;
        at -= n;
        if (k + 1 < page.size() && at-- == 0) return true;
    }
    return false;
}

// The page cut to its first `limit` characters (the \x01 marks kept; a line break counts as one)
std::vector<std::string> typedPage(const std::vector<std::string>& page, int limit) {
    std::vector<std::string> out;
    for (const std::string& l : page) {
        if (!out.empty() && limit-- <= 0) break;
        std::string t;
        for (char c : l) {
            const unsigned char ch = static_cast<unsigned char>(c);
            if (ch != 0x01 && (ch & 0xC0) != 0x80 && limit-- <= 0) break;
            t += c;
        }
        out.push_back(t);
    }
    return out;
}

const cr::MessageInfo* pageInfo(const App& app) {
    const size_t pi = std::min(app.dialoguePage, app.dialogue.size() - 1);
    return pi < app.dialogueInfo.size() ? &app.dialogueInfo[pi] : nullptr;
}

// Out: every character typed and every pause over (a 0x1a before the wait holds the "next" mark too)
bool pageTyped(const App& app) {
    if (app.dialogue.empty() || !app.typeOn) return true;
    const cr::MessageInfo* info = pageInfo(app);
    return app.dialogueTyped >= pageChars(app.dialogue[std::min(app.dialoguePage, app.dialogue.size() - 1)]) &&
           (!info || app.dialoguePause >= int(info->pauses.size())) && app.dialogueHold == 0;
}

// _PrcMsg's 0x1a n (2.2.0 0x2c444): the next unit comes n frames after the one before (its counter
// G+0x33858 set to n - 1 on reaching it, down a frame at a time, on at 0 - for n 0 it starts at
// -1: two frames)
int pauseHold(int frames) { return frames == 0 ? 1 : frames - 1; }

// PrcMsg a frame: the next character, or - the typing flag down (a tap) - every character up to the
// next stop: a 0x1a pause, a line break (0x11 ends _PrcMsg's frame), the page's end. A page ending
// the message with 0x14 (or one the window clears by itself, 0x1a then 0x10) goes once out and its
// pauses are over
void stepTyping(App& app) {
    if (app.dialogue.empty()) return;
    if (app.msgWin.state == 1 || app.msgWin.state == 2) return; // PrcMsg: nothing until the window is up (state 3)
    const cr::MessageInfo* info = pageInfo(app);
    const std::vector<std::string>& page = app.dialogue[std::min(app.dialoguePage, app.dialogue.size() - 1)];
    const int total = pageChars(page);
    if (!app.typeOn) app.dialogueTyped = total, app.dialoguePause = info ? int(info->pauses.size()) : 0;
    if (app.dialogueHold > 0 && --app.dialogueHold > 0) return;
    for (;;) {
        if (info && app.dialoguePause < int(info->pauses.size()) && info->pauses[size_t(app.dialoguePause)].at <= app.dialogueTyped) {
            app.dialogueHold = pauseHold(info->pauses[size_t(app.dialoguePause++)].frames);
            if (app.dialogueHold > 0) return;
            continue;
        }
        if (app.dialogueTyped >= total) {
            if (!info || info->closeAfter < 0 || app.choiceFlag >= 0) return;
            advanceDialogue(app, false);
            // PrcMsg 0x14: the message is done (the script goes on) but its text stays in the window
            // until Close, the next message or the script's end; 0x10: the window clears, the next
            // page's first character comes the frame after
            if (app.dialogue.empty()) app.heldPage = true;
            else app.dialogueHold = 0;
            return;
        }
        const bool lineBreak = pageBreakAt(page, app.dialogueTyped);
        ++app.dialogueTyped;
        if (!app.dialogueInstant || lineBreak) return;
    }
}

// The typing flag down (2.2.0 _PrcMsg's top: a tap, or the skip corner _pad_chk_circle(460, 16,
// 20), while it types - with no 0x1b guard: the G+0x337e6 that 0x1b flips is read nowhere)
void hurryDialogue(App& app) {
    if (!app.dialogue.empty() && !pageTyped(app)) app.dialogueInstant = true;
}

// The player's tap on a page: one still typing comes out at once up to its next stop (PrcMsg
// +0x337df), one out turns; a page closing by itself takes no tap
void tapDialogue(App& app) {
    if (app.dialogue.empty()) return;
    const cr::MessageInfo* info = pageInfo(app);
    if (!pageTyped(app)) {
        hurryDialogue(app);
        return;
    }
    if (info && info->closeAfter >= 0) return;
    advanceDialogue(app);
    // (the tap comes before this frame's stepTyping, which takes one of the hold's frames on the tap's
    // own frame: _PrcMsg 25119-25480 - the tap frame's 0x12, the next frame's 0x10, then the first
    // character - wants the two empty frames after it, as the skip's turn after stepTyping gets)
    if (!app.dialogue.empty()) ++app.dialogueHold;
}

// Answers the open yes/no choice (Util 3): yes sets its flag, no clears it (_menuu_EventClr).
void answerChoice(App& app, bool yes, bool cancelled) {
    if (app.choiceFlag < 0) return;
    app.game.setFlag(app.choiceFlag, yes);
    app.choiceFlag = -1;
    // Yes and No alike (EventPrc's ComUty_YesNoChoiceProc(.., 1)); the Vita's × for No: SE 3
    // (__ComUty_YesNoChoiceProc, Vita 8104e070)
    menuSound(app, cancelled ? se::kBack : se::kChoose);
}

// Closes the world choice (_menuu_WldChoiceClr): cancelling sets flag 0 (the script returns);
// a gate leaves it clear with var 0 = gate, var 1 = level, var 2 = the party (route), flag 1 clear.
// The World Select answered at once (tests, the scripted walks): the gate at list position `pos`
// (-1 Back), its first level band, no skipping
void answerWorldChoice(App& app, int pos) {
    if (!app.worldChoiceOpen) return;
    App::WorldSelect& ws = app.worldSelect;
    ws.cancelled = pos < 0 || pos >= int(ws.worlds.size());
    if (!ws.cancelled) ws.world = ws.worlds[size_t(pos)], ws.level = 0, ws.skip = false;
    menuSound(app, ws.cancelled ? se::kBack : se::kChoose); // MuScrList: an item, or Back
    closeWorldSelect(app);
}

// Queues the messages an event showed (English text) for the dialogue box.
void showMessages(App& app, const std::vector<int>& ids) {
    app.heldPage = false;
    if (!ids.empty()) app.lastMessageId = ids.back();
    if (app.dialogue.empty()) {
        app.dialogueInfo.clear(), app.dialogueVoices.clear(), app.dialoguePage = 0;
        ++app.dialogueSerial;
        resetTyping(app);
    }
    for (int id : ids) {
        std::vector<std::vector<std::string>> pages;
        cr::MessageInfo info;
        auto var = [&](int v) { auto it = app.game.vars.find(v); return it == app.game.vars.end() ? 0 : it->second; };
        if (app.events.message(id, app.language, var, pages, &info) ||
            app.systemText.message(id, app.language, var, pages, &info))
            app.dialogue.insert(app.dialogue.end(), pages.begin(), pages.end());
        else
            pages = {{"[message " + std::to_string(id) + "]"}}, app.dialogue.push_back(pages[0]);
        app.dialogueInfo.resize(app.dialogue.size() - pages.size());
        // A code on page k at `at` - one after the last page's text belongs to it, and one past a
        // page's end (its trailing empty lines dropped) comes as it is out
        auto onPage = [&](int page, size_t k) { return page == int(k) || (k + 1 == pages.size() && page >= int(pages.size())); };
        auto clampAt = [&](int at, size_t k) { return std::min(at, pageChars(pages[k])); };
        for (size_t k = 0; k < pages.size(); ++k) { // each page its own pauses; the last the closing
            cr::MessageInfo pi = info;
            pi.pauses.clear();
            for (const auto& pz : info.pauses)
                if (onPage(pz.page, k)) pi.pauses.push_back({pz.page, clampAt(pz.at, k), pz.frames});
            if (k + 1 < pages.size()) pi.closeAfter = -1;
            for (const auto& t : info.turns) // (a page the window clears by itself after its pause turns so)
                if (t.page == int(k) && k + 1 < pages.size()) pi.closeAfter = t.frames;
            for (const auto& kp : info.kept) // (0x12 without 0x10: the lines before stay)
                if (kp.page == int(k)) pi.keep = std::min(kp.chars, pageChars(pages[k])), pi.instant = kp.instant;
            app.dialogueInfo.push_back(pi);
        }
        app.dialogueVoices.resize(app.dialogue.size() - pages.size());
        for (size_t k = 0; k < pages.size(); ++k) {
            std::vector<int> clips;
            for (const auto& v : info.voices)
                if (v.page == int(k) || (k + 1 == pages.size() && v.page >= int(pages.size()))) clips.push_back(v.clip);
            app.dialogueVoices.push_back(clips);
        }
    }
}

// Fires the touch trigger the player just stepped into. Returns true if it warped.
bool checkTriggers(App& app, bool instant) {
    if (app.runner.running()) return false;
    const int id = triggerAt(app, app.walker.pos);
    if (id == 0) { app.lastKick = 0; return false; }
    if (id == app.lastKick) return false;
    app.lastKick = id;
    if (!instant) {
        app.runnerWhat.clear();
        app.runner.start(app.events, id, app.game, app.area, app.mapNumber);
        return false;
    }
    const cr::EventResult r = cr::runEvent(app.events, id, app.game, app.area, app.mapNumber);
    if (!r.error.empty()) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "event %d: %s", id, r.error.c_str());
    showMessages(app, r.messages);
    return r.warp.valid && performWarp(app, r.warp, id);
}

// A model's node world matrices for `act` at `time`, under the old action while a change blends
// (mdl_actctl_prc: two action controllers weighted 1 - k / 2 and k / 2)
void poseWorld(const cr::Skeleton& skel, const std::vector<cr::Action>& actions, const cr::Action* act, float time,
               const cr::MotionBlend& blend, std::vector<cr::Mat4>& world) {
    if (const cr::Action* under = blend.under(actions))
        cr::computeBlendedWorld(skel, under, blend.fromTime, act, time, blend.weight(), world);
    else cr::computeWorld(skel, act, time, world);
}

// Advances a model's current animation and re-skins every mesh on the CPU.
void animate(App& app, LoadedModel& lm, float dt) {
    const cr::Model& m = lm.cpu;
    if (m.skeleton.nodes.empty() || lm.meshes.size() != m.meshes.size()) return;
    const cr::Action* act = lm.action >= 0 && lm.action < int(lm.actions.size()) ? &lm.actions[lm.action] : nullptr;
    if (act && (app.mode != Mode::Models || app.playing)) {
        if (app.mode == Mode::Models) lm.blend.reset(); // (the viewer shows a chosen action at once)
        if (!lm.blend.step(lm.action, lm.time, dt * kFramesPerSecond * app.speed, lm.actions))
            lm.time += dt * kFramesPerSecond * app.speed;
        const float end = lm.loopTo > 0 && lm.loopTo < act->duration ? float(lm.loopTo) : act->duration;
        if (lm.once) lm.time = std::min(lm.time, std::max(act->duration - 1, 0.0f)); // (played once, its last frame held: mdl_actctl_prc's +0x3aa)
        else if (lm.loopFrom > 0 && lm.loopFrom < end && lm.time >= end)
            lm.time = float(lm.loopFrom) + std::fmod(lm.time - float(lm.loopFrom), end - float(lm.loopFrom));
        else if (end > 0 && lm.time >= end) lm.time = std::fmod(lm.time, end);
    }
    poseWorld(m.skeleton, lm.actions, act, lm.time, lm.blend, lm.world);
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        cr::skinMesh(m, m.meshes[i], lm.world, lm.skinned);
        app.renderer.updatePositions(lm.meshes[i], lm.skinned, m.meshes[i].uvs, m.meshes[i].colors);
    }
}

void animateModel(App& app, float dt) { animate(app, app.model, dt); }


// Puts the party leader on the current field, where the stage camera's line of sight meets
// the floor (the real start point comes from the event script, which isn't interpreted yet).
void placePlayer(App& app) {
    app.havePlayer = false;
    app.leaderYawSet = false; // (_fldread_prc 37737: a load sets the drawn yaw)
    if (!app.showPlayer || app.collisionData.polygons.empty()) return;
    const std::string name = kPartyModels[app.playerChar];
    if (app.playerName != name || app.playerLoadedByReadMdl != app.playerByReadMdl) {
        unloadLoaded(app, app.player);
        loadModelFile(app, name, app.player, true);
        if (app.playerByReadMdl) readMdlRules(app.player, name);
        else mdlMemRules(app.player);
        app.playerName = name;
        app.playerLoadedByReadMdl = app.playerByReadMdl;
    }
    if (app.player.meshes.empty()) return;
    const cr::Collision& col = app.collisionData;
    cr::Vec3 eye, tgt;
    if (app.haveStageCam) {
        eye = {app.stageCam.eye[0], app.stageCam.eye[1], app.stageCam.eye[2]};
        tgt = {app.stageCam.target[0], app.stageCam.target[1], app.stageCam.target[2]};
    } else {
        const float cx = (col.boundsMin[0] + col.boundsMax[0]) / 2, cz = (col.boundsMin[2] + col.boundsMax[2]) / 2;
        eye = {cx, col.boundsMax[1] + 100, cz};
        tgt = {cx, col.boundsMin[1] - 100, cz};
    }
    const float aspect = app.haveStageCam && app.stageCam.viewHeight > 0
                             ? float(app.stageCam.viewWidth) / float(app.stageCam.viewHeight) : 1.0f;
    const float fov = app.haveStageCam ? app.stageCam.fovDegrees : 60.0f;
    const cr::Mat4 vp = cr::Mat4::perspective(fov * 3.14159265f / 180.0f, aspect, 10.0f, 4000.0f) *
                        cr::Mat4::lookAt(eye, tgt, app.haveStageCam ? cr::Vec3{0, 1, 0} : cr::Vec3{0, 0, -1}); // top-down otherwise
    // Start at the map's first entrance in the area's warp table (where the game puts you when
    // you arrive); without one, where the camera looks at the floor
    bool entrance = false;
    for (const cr::WarpEntry& w : app.events.warps()) {
        if (w.map != app.mapNumber || (w.area >= 0 && w.area != app.area)) continue;
        float y;
        if (!cr::floorHeight(col, w.pos[0], w.pos[1], w.pos[2], 5.0f, y) &&
            !cr::floorHeight(col, w.pos[0], w.pos[1], w.pos[2], 1000.0f, y))
            continue;
        app.walker.pos = {w.pos[0], y, w.pos[2]};
        app.walker.heading = float(w.angleDegrees) * 3.14159265f / 180.0f;
        entrance = true;
        break;
    }
    if (!entrance) {
        if (!cr::spawnPoint(col, eye, tgt, vp, app.walker.pos)) return;
        app.walker.heading = std::atan2(eye.x - tgt.x, eye.z - tgt.z); // face the camera
    }
    app.walker.radius = app.haveStageCam && app.stageCam.hitRadius > 0 ? float(app.stageCam.hitRadius) : 5.0f;
    app.walker.gait = cr::Gait::Stand;
    app.player.action = app.player.actions.empty() ? -1 : 0;
    app.player.time = 0;
    app.walkClock = 0;
    app.havePlayer = true;
}

// The viewer's side of a running script (see cr::EventHost).
// The NPC's models, shown or hidden: the event ops act on the model whatever its NPC's hidden bit
// (__EvtAnim / __EvtAnim2 / __EvtNpcFade / __EvtUnloadAct take npc ctl +0, 18015-19060)
std::vector<App::NpcDraw*> ViewerHost::npc(int id) {
    std::vector<App::NpcDraw*> out;
    for (auto* v : {&app.npcDraws, &app.npcHeld})
        for (auto& d : *v)
            if (d.id == id) out.push_back(&d);
    return out;
}
void ViewerHost::showMessage(int id) {
    const bool fresh = app.dialogue.empty();
    showMessages(app, {id});
    // (_gameapp_prc 1844-1845: PrcMsg runs after PrcEvnt and PrcWin, so with the window up the first
    // character is out on the Msg op's own frame; ours typed before the script ran)
    if (fresh && app.msgWin.state == 3) stepTyping(app);
}
bool ViewerHost::messageOpen() { return !app.dialogue.empty(); }
bool ViewerHost::tapped() { const bool t = app.tapPending; app.tapPending = false; return t; }
// _Uty_GetItem_Init (2.2.0 0x25434): the sound (0x10 for items 61..80, else 0xf; the Oz's 0xf), the
// icon (GetIconNoFromItemNo - 1; the Oz 0x3f) and message 6 / 10 (6 / 0x5e for item 0x1b6, 6 / 0x55
// for the Oz: "You got n/OZ!"), flag 0 cleared - the band comes down from the top (_Put)
void ViewerHost::showItemWindow(int item, bool oz) {
    app.game.setFlag(0, false); // confirmation belongs to this item window, not the previous interaction
    app.tapPending = false; // (_Uty_GetItem_Init 20367: memset(G, 0, 0xa0) - the touches cleared)
    app.itemWindowOpen = true;
    app.itemWindowItem = item;
    App::ItemWin& w = app.itemWin;
    w = App::ItemWin();
    w.oz = oz;
    const cr::ItemParam* p = oz ? nullptr : app.itemTable.item(item);
    w.icon = oz ? 0x3f : p ? p->icon : -1;
    w.message = 57000 + (oz ? 0x55 : item == 0x1b6 ? 0x5e : 10);
    const uint32_t se = !oz && item >= 0x3d && item < 0x3d + 0x14 ? 0x10 : 0xf;
    app.sound.play(se);
    app.soundLog.push_back(se);
}
// Anim (__EvtAnim -> mdl_set_act_motion_loop(mdl, slot, 2, a, b)): a new action blends in over 2
// frames (MotionBlend::step sees the change) with the loop a..b; the action already playing goes on
// as it is, its loop too. Npc 0 is the leader's model (gac + 0x83c)
void ViewerHost::playAnim(int id, int slot, int loopFrom, int loopTo) {
    int action = slot; // a slot LoadAct filled, else the model's own action of that number
    if (auto a = app.actorSlots.find(id); a != app.actorSlots.end())
        if (auto it = a->second.find(slot); it != a->second.end()) action = it->second;
    if (id == 0) {
        App::PlayerAnim& pa = app.playerAnim;
        if (pa.on && pa.action == action) return;
        if (!pa.on && app.player.action == action) pa = App::PlayerAnim{true, action, false, 0, 0}; // (the gait's, looped whole)
        else pa = App::PlayerAnim{true, action, loopFrom < 0, std::max(loopFrom, 0), loopTo};
        return;
    }
    for (auto* d : npc(id)) {
        if (d->action == action) continue; // the same action keeps running, and its loop
        d->action = action;
        d->time = 0;
        d->once = loopFrom < 0;
        d->loopFrom = loopFrom;
        d->loopTo = loopTo;
    }
}
// Anim2 (mdl_set_act_motion_loop_frm): started over from `start` whatever plays, the pose before
// blended under it over `blend` frames - a cut when 0, as the opening's are
void ViewerHost::playAnim2(int id, int slot, int loopFrom, int loopTo, int start, int blend) {
    int action = slot;
    if (auto a = app.actorSlots.find(id); a != app.actorSlots.end())
        if (auto it = a->second.find(slot); it != a->second.end()) action = it->second;
    for (auto* d : npc(id)) {
        const LoadedModel& lm = app.npcModels[d->model];
        d->action = action;
        d->time = float(std::max(start, 0));
        d->once = loopFrom < 0;
        d->loopFrom = loopFrom;
        d->loopTo = loopTo;
        d->blend.restart(action, d->time, float(std::max(blend, 0)), lm.actions);
    }
}
void ViewerHost::loadAction(int id, int action, int slot) {
    const cr::FieldNpc* n = findNpc(app, id);
    if (!n || n->model < 0 || n->model >= cr::kNumNpcModels || action < 0 || action >= cr::kNumActionFiles) {
        if (SDL_getenv("CR_OP_TRACE")) std::printf("  load action %d into npc %d slot %d: no such NPC or action\n", action, id, slot);
        return;
    }
    const std::string file = cr::kNpcModelFile[n->model];
    LoadedModel& lm = npcModel(app, file);
    const std::string mca = cr::kActionFile[action];
    auto known = lm.loaded.find(mca);
    if (known == lm.loaded.end()) {
        std::vector<uint8_t> ad;
        std::vector<cr::Action> acts;
        int index = -1;
        if (cr::readFile(app.dataDir + mca, ad) && cr::parseMca(ad.data(), ad.size(), 0, acts))
            for (auto& a : acts) // (2.2.0's __EvtLoadAct, 0x22e88, binds whatever the file holds -
                // read_act_set -> mdl_act_init, no node count checked: a motion a few segments
                // short, or short of the model's rest helpers, too)
                if (cr::actionFits(lm.cpu.skeleton, a) || cr::actionNearlyFits(lm.cpu.skeleton, a)) {
                    a.name = mca.substr(0, mca.size() - 4);
                    index = int(lm.actions.size());
                    lm.actions.push_back(std::move(a));
                    break;
                }
        known = lm.loaded.emplace(mca, index).first;
    }
    if (known->second >= 0) app.actorSlots[id][slot] = known->second;
    if (SDL_getenv("CR_OP_TRACE"))
        std::printf("  load action %s into npc %d (%s) slot %d: %s\n", mca.c_str(), id, file.c_str(), slot,
                    known->second >= 0 ? "ok" : "doesn't fit its skeleton");
}
void ViewerHost::unloadAction(int id, int slot) { // (__EvtUnloadAct: the slot freed and the model put on its action 0)
    if (auto a = app.actorSlots.find(id); a != app.actorSlots.end()) a->second.erase(slot);
    for (auto* d : npc(id)) {
        if (d->action == 0) continue;
        d->action = 0, d->time = 0, d->once = false, d->loopFrom = d->loopTo = 0;
        d->blend.restart(0, 0, 0, app.npcModels[d->model].actions);
    }
}
// Wait 0xFFFD (npc_autoctl_chk): only a scripted move keeps it waiting, not an animation
bool ViewerHost::animBusy(int id) {
    for (const auto& m : app.moves)
        if (m.npc == id) return true;
    for (const auto& r : app.rescales)
        if (r.npc == id) return true;
    return false;
}
// SetScale (frames 0) / AdjScale: an NPC's x y z scale (the player's, gac +0xd84 / prty_autoctl_set_scale,
// no script changes); one autoctl an NPC, so a rescale ends its move and a move its rescale
void ViewerHost::scaleNpc(int id, const float* s, int frames) {
    cr::FieldNpc* n = findNpc(app, id);
    if (!n || id == 0) return;
    app.rescales.erase(std::remove_if(app.rescales.begin(), app.rescales.end(), [&](const App::Rescale& o) { return o.npc == id; }),
                       app.rescales.end());
    if (frames <= 0) {
        for (int k = 0; k < 3; ++k) n->axes[k] = s[k];
        for (auto& d : app.npcDraws)
            if (d.id == id) d.world = npcWorld(*n);
        return;
    }
    app.moves.erase(std::remove_if(app.moves.begin(), app.moves.end(), [&](const App::Move& o) { return o.npc == id; }), app.moves.end());
    App::Rescale r;
    r.npc = id, r.frames = frames;
    for (int k = 0; k < 3; ++k) r.from[k] = n->axes[0] >= 0 ? n->axes[k] : n->scale, r.to[k] = s[k];
    app.rescales.push_back(r);
    if (stepRescale(app, app.rescales.back())) app.rescales.pop_back(); // (the first step this frame's)
}
// Autoadj (npc_autoctl_set / prty_autoctl_set): a straight move to (x, its y, z) at `speed` a frame -
// dist / speed frames, truncated (FPToFixed) - facing kept
void ViewerHost::autoMove(int id, float x, float z, float speed) {
    cr::Vec3 p;
    if (!actorPos(app, id, p) || speed <= 0) return;
    const float v[3] = {x, p.y, z};
    moveNpc(id, v, 0, Facing::Keep, int(std::hypot(p.x - x, p.z - z) / speed), 0);
}
// mdl_set_alpha (0x1409c): from -> to over `frames` (mdl_put's steps); 0 frames leaves the alpha
// as it is (+0x56d = 0: mdl_put keeps the colour) - either way the model's colour arrays are off
// from now on (fadedMaterial). The op's own frame already shows the first step: this frame's mdl_put
// counts +0x834 up before it colours the model (2.2.0 10156-10170) - the step loop ran before the script
void ViewerHost::fadeNpc(int id, float from, float to, int frames) {
    const float first = frames > 0 ? to + (from - to) * float(frames - 1) / float(frames) : from;
    if (id == 0) { // the player's model (NpcFade 0: __EvtNpcFade's gac + 0x83c), as mdl_set_alpha
        app.playerFaded = true;
        if (frames <= 0) { app.playerFadeLeft = 0; return; }
        app.playerAlphaFrom = from, app.playerAlphaTo = to, app.playerAlpha = first;
        app.playerFadeFrames = frames, app.playerFadeLeft = frames - 1;
        return;
    }
    bool lit = false;
    for (auto* d : npc(id)) {
        d->faded = true;
        if (frames <= 0) { d->fadeLeft = 0; continue; }
        d->alphaFrom = from; d->alphaTo = to; d->alpha = first;
        d->fadeFrames = frames, d->fadeLeft = frames - 1;
        if (d >= app.npcHeld.data() && d < app.npcHeld.data() + app.npcHeld.size()) d->lit = lit = true; // (+0x55c 0 -> 1)
    }
    if (lit) buildNpcDraws(app);
}
void ViewerHost::showNpc(int id, bool shown) {
    app.npcShown[id] = shown;
    if (id == 0) return; // the player: drawn while showPlayer and not hidden here
    if (!shown) // (npc_kill: +0x55c = 0 - a fade that lit it stops drawing it too)
        for (auto* d : npc(id)) d->lit = false;
    // Born explicitly activates the actor at its current position. Some scenes show the
    // hall loaded at the origin without a Setc (t01 event 202, the execution scene).
    if (shown) app.placedNpcs.insert(id);
    buildNpcDraws(app);
}
void ViewerHost::screenFade(float grey, float from, float to, int frames) {
    app.fadeGrey = grey; app.fadeFrom = from; app.fadeTo = to; app.fadeFrames = std::max(frames, 1); app.fadeT = 0;
}
bool ViewerHost::screenFading() { return app.fadeT < app.fadeFrames; }
void ViewerHost::backFade(float grey, float from, float to, int frames) { // _SetFade_back (PutMask: 50 ms a frame)
    app.backFade = App::BackFade{grey, from, to, std::max(frames, 1), 0};
}
bool ViewerHost::backFading() { return app.backFade.t < app.backFade.frames; } // _ChkFade_back
void ViewerHost::warp(const cr::WarpRequest& w) { performWarp(app, w, app.runner.eventNo()); }
void ViewerHost::playSound(uint32_t id) {
    const bool speech = (id & 0x400000) || app.sound.isVoice(int(id & 0xfff));
    const bool control = id & (0x20000000u | 0xe000000u | 0xe0000u);
    if (speech && !control && !app.battle.active) {
        app.sound.stopDialogueVoices();
        if (app.skipScene) return;
    }
    app.sound.play(id);
    app.soundLog.push_back(id);
}
void ViewerHost::playMusic(uint32_t v) { playBgm(app, v); app.soundLog.push_back(0x40000000u | v); }
void ViewerHost::recoverPair() { // pcparam_hpmp_recovery: the current pair's two
    const int p = std::clamp(currentPair(app) - 1, 0, 3);
    for (int c : {2 * p, 2 * p + 1}) { // (MP as a number: _pcparam_max_mp stored, 29934-29972)
        app.game.party[size_t(c)].hp = -1;
        app.game.party[size_t(c)].mp = cr::memberStat(app.game, c, cr::kMp, app.itemTable);
    }
}
int ViewerHost::bgmNo() { return app.bgmNo; }
void ViewerHost::endMenu(int kind) { // 2: the shop - SetShopBgmNoRecovery, __mc_SetMenuStat(7, 4)
    if (kind != 2 || !app.shop.open) return;
    app.shop.noBgmRecovery = true;
    closeShop(app);
}
int ViewerHost::pairNo() { return std::clamp(currentPair(app) - 1, 0, 3); }
void ViewerHost::nowLoading(bool on) { // NowLoadStart / NowLoadEnd
    if (on && !app.nowLoading.on) app.nowLoading = App::NowLoading{true, 0};
    else if (!on) app.nowLoading.on = false;
}
// Dir 2000 ("face the way it moves": __EvtSetc / __EvtFrmAdjc, 2.2.0 18226-18241): turns =
// (1 - dz / len) / 4, and 1 - that when dx < 0 - linear in the cosine, not atan2: a 45-degree
// move faces 26.4 degrees
float travelYaw(float dx, float dz) {
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len <= 0) return 0;
    float t = (1.0f - dz / len) * 0.25f;
    if (dx < 0) t = 1.0f - t;
    return t * 2.0f * 3.14159265f;
}
void ViewerHost::placeNpc(int id, const float* v, float yawDeg, Facing facing) {
    cr::Vec3 old;
    if (!actorPos(app, id, old)) return;

    const cr::Vec3 p{v[0], v[1], v[2]};
    float yaw = yawDeg * kDeg;
    if (facing == Facing::Travel) {
        if (std::fabs(p.x - old.x) + std::fabs(p.z - old.z) < 1e-4f) facing = Facing::Keep;
        else yaw = travelYaw(p.x - old.x, p.z - old.z);
    }
    setActor(app, id, p, facing == Facing::Keep ? nullptr : &yaw, false);
    if (facing == Facing::Snap) {
        if (cr::FieldNpc* n = id ? findNpc(app, id) : nullptr) n->shownSet = false; // (the model's yaw with it)
        if (id == 0) app.leaderYawSet = false;
    }
}
void ViewerHost::moveNpc(int id, const float* v, float yawDeg, Facing facing, int frames, int mode) {
    App::Move m;
    if (!actorPos(app, id, m.from)) return;
    m.npc = id;
    m.to = {v[0], v[1], v[2]};
    m.mode = mode;
    m.frames = std::max(frames, 1);
    float yawNow = id == 0 ? app.walker.heading : findNpc(app, id)->angle;
    if (facing == Facing::Travel && std::fabs(m.to.x - m.from.x) + std::fabs(m.to.z - m.from.z) > 1e-4f) {
        const float yaw = travelYaw(m.to.x - m.from.x, m.to.z - m.from.z);
        setActor(app, id, m.from, &yaw, true);
    } else if (facing == Facing::Yaw) {
        m.turn = true;
        m.yawFrom = yawNow;
        m.yawTo = yawDeg * kDeg;
    } else if (facing == Facing::Snap || facing == Facing::Set) { // (FrmAdjc2 mode 0 / FrmAdjc: the rotation set before the move starts)
        const float yaw = yawDeg * kDeg;
        setActor(app, id, m.from, &yaw, true);
        if (facing == Facing::Snap) { // (FrmAdjc2 mode 0, 19367-19372 / 19351-19353: the model's drawn yaw too; FrmAdjc's turns to it)
            if (cr::FieldNpc* n = id ? findNpc(app, id) : nullptr) n->shownSet = false;
            if (id == 0) app.leaderYawSet = false;
        }
    }
    app.moves.erase(std::remove_if(app.moves.begin(), app.moves.end(), [&](const App::Move& o) { return o.npc == id; }),
                    app.moves.end());
    app.rescales.erase(std::remove_if(app.rescales.begin(), app.rescales.end(), [&](const App::Rescale& o) { return o.npc == id; }),
                       app.rescales.end());
    app.moves.push_back(m);
    if (stepMove(app, app.moves.back())) app.moves.pop_back(); // (its first step is this frame's: npc_autoctl_prc runs after PrcEvnt)
}
// A Tex2d slot's slide, fade and grey a frame on (tex2d_img_prc: drawn at t, then t + 1)
static void stepPortraitBlend(App::Portrait& p) {
    if (p.t < p.frames) {
        ++p.t;
        const float t = float(p.t) / float(p.frames);
        p.x = p.xFrom + (p.xTo - p.xFrom) * t;
        p.y = p.yFrom + (p.yTo - p.yFrom) * t;
    }
    if (p.aT < p.aFrames) {
        ++p.aT;
        p.alpha = p.aFrom + (p.aTo - p.aFrom) * float(p.aT) / float(p.aFrames);
    }
    if (p.gT < p.gFrames) {
        ++p.gT;
        p.shade = p.gFrom + (p.gTo - p.gFrom) * float(p.gT) / float(p.gFrames);
    }
}

void ViewerHost::tex2d(int slot, uint16_t f, int image, const int16_t (*extra)[3]) {
    if (SDL_getenv("CR_TEXDISP_DEBUG")) std::printf("tex2d: slot %d flags %04x image %d\n", slot, f, image);
    if (slot < 0 || slot >= 16) return;
    App::Portrait& p = app.portraits[slot];
    if ((f & 1) && image >= 0 && image < cr::kNumTex2dImages) { // read_img_set
        app.renderer.destroy(p.tex);
        p = App::Portrait();
        cr::Image img;
        // (__EvtTex2d: image 11, the map's title card, from the language's own sheet - MAPtitle_fr,
        // _de, _es, _cs, _ct, _kr for languages 2..7 - else MAPtitle)
        static const char* const kTitleLang[6] = {"fr", "de", "es", "cs", "ct", "kr"};
        const bool own = image == 11 && app.language >= 2 && app.language <= 7 &&
                         loadImage(app, std::string("MAPtitle_") + kTitleLang[app.language - 2] + "_hd.mct", img);
        // (FUN_00006588 2161: <name>_hd.mct when it exists - slot +0x2bcca set, _tex2d_img_prc (7528) halving it -
        // else <name>.mct drawn whole: c001..c004_C00's rings, the d0xxx slides at their own size)
        std::string base = cr::kTex2dImage[image];
        if (base.size() > 4) base.resize(base.size() - 4);
        if (base.size() > 3 && base.compare(base.size() - 3, 3, "_hd") == 0) base.resize(base.size() - 3);
        p.hd = true;
        if (own || loadImage(app, base + "_hd.mct", img)) p.tex = app.renderer.upload(img);
        else if (loadImage(app, base + ".mct", img)) p.tex = app.renderer.upload(img), p.hd = false;
        p.image = image;
        // (0x8000 only sizes it - a full-screen card; a load alone shows nothing: tex2d_img_prc
        // draws a slot once 0x200 / 0x400 has turned it on - Uty_TexDisp draws loaded ones itself)
    }
    // __EvtTex2d: the flags stay in the slot (placement bits replaced); every animation is a
    // straight blend over a few 20 Hz frames (tex2d_img_prc)
    p.flags = uint16_t((p.flags & ~0x3000) | f);
    // (the HD binary's __EvtTex2d homes 0x1000 at (-32, 0), 0x2000 at (512, 0), 0x4000 at (176, 96).
    // 2.2.0's (0x20dec) put 0x1000 at tex2d (0, 0), 0x2000 at (224, 0) and 0x4000 at (176, 96) on
    // the 480 x 320 screen: in the phone layouts (logical = 2 tex2d + (32, 64)) (32, 64), (480, 64)
    // and (384, 256) - the iPad layout keeps the HD homes)
    const bool phone = app.renderer.phoneLayout();
    if (f & 0x1000) p.home = phone ? 32 : -32, p.y = phone ? 64 : 0, p.side = -1;
    if (f & 0x2000) p.home = phone ? 480 : 512, p.y = phone ? 64 : 0, p.side = 1; // drawn mirrored (+0xbd0e)
    if (f & 0x4000) {
        p.home = phone ? 384 : 176, p.y = phone ? 256 : 96, p.side = 0;
        if (f & 0x40) { p.scale = 2; p.zoomT = 0; p.zoomFrames = 4; } // 200% -> 100%
    }
    if (f & 0x7000) { p.x = p.xFrom = p.xTo = p.home; p.yFrom = p.yTo = p.y; p.frames = p.t = 0; }
    if (f & 0xC00) { // show: 3 frames of fade in and a 32 px slide in from its side; hide: a 32 px jump out
                     // at once (the slide's home moved to its start) and 3 frames of fade out; either
                     // puts the grey level back to 255
        const bool show = f & 0x400;
        const float out = 32.0f * float(p.side);
        p.aFrom = show ? 0 : 1;
        p.aTo = show ? 1 : 0;
        p.xFrom = show ? p.home + out : p.home + out;
        p.xTo = show ? p.home : p.home + out;
        p.yFrom = p.yTo = p.y;
        p.x = p.xFrom;
        p.alpha = p.aFrom;
        p.shade = p.gFrom = p.gTo = 1, p.gT = p.gFrames = 0;
        p.frames = p.aFrames = 3;
        p.t = p.aT = 0;
    }
    if (f & 0x200) { // swap in (a new expression): shown at once in place; whatever else
                     // stands on that side goes
        if (p.side)
            for (int i = 0; i < 16; ++i) {
                App::Portrait& o = app.portraits[i];
                if (i != slot && o.tex.id && (o.flags & (p.side < 0 ? 0x1000 : 0x2000))) {
                    o.alpha = o.aFrom = o.aTo = 0;
                    o.frames = o.t = o.aFrames = o.aT = 0;
                }
            }
        p.x = p.xFrom = p.xTo = p.home;
        p.yFrom = p.yTo = p.y;
        p.alpha = p.aFrom = p.aTo = 1;
        p.frames = p.t = p.aFrames = p.aT = 0;
    }
    // The low byte's extra operands, 3 shorts each: 4 a fade (from, to /255, frames), 0x20 the
    // grey level the same way, 8 a slide (dx, dy, frames) - from the offset back to where it
    // stands; fewer than 0 frames: there at once, and staying
    if (f & 2) p.dying = true;
    if (f & 4) {
        p.aFrom = float(extra[0][0]) / 255.0f, p.aTo = float(extra[0][1]) / 255.0f;
        p.aFrames = std::max<int>(1, extra[0][2]), p.aT = 0;
        p.alpha = p.aFrom;
    }
    if (f & 0x20) {
        p.gFrom = float(extra[1][0]) / 255.0f, p.gTo = float(extra[1][1]) / 255.0f;
        p.gFrames = std::max<int>(1, extra[1][2]), p.gT = 0;
        p.shade = p.gFrom;
    }
    if (f & 8) {
        const float dx = extra[2][0], dy = extra[2][1];
        const int n = extra[2][2];
        if (n < 0) { p.xTo += dx, p.yTo += dy; p.home += dx; }
        p.xFrom = p.xTo + (n < 0 ? 0 : dx), p.yFrom = p.yTo + (n < 0 ? 0 : dy);
        p.x = p.xFrom, p.y = p.yFrom;
        p.frames = std::max(1, std::abs(n)), p.t = 0;
    }
    // (__EvtTex2d 17590-17807 starts a blend's count at 1 and _gameapp_prc's tex2d_prc_2d runs after
    // PrcEvnt in the same frame: the op's own frame already shows its first step - ours runs the
    // slots before the script, so the step is taken here. The 0x40 zoom comes out the same as it is.)
    if (f & 0xC2C) stepPortraitBlend(p);
    if (p.dying && p.t >= p.frames && p.aT >= p.aFrames) { app.renderer.destroy(p.tex); p = App::Portrait(); } // nothing to wait for
}
void ViewerHost::loadParty(int pair, bool clearData) { // Uty_LoadPrty: prty_init_new(pair - 1, mode)
    if (pair >= 1 && pair <= 4) newPairParty(app, pair, clearData);
}
void ViewerHost::loadNpc(int id) {
    if (id < 0) { // (__EvtNpcMdlRead -n: an NPC npc_setup left unread - its create flags failed - read now, placed at
                  // its record with the drawn yaw set, hidden (+0x8c bit 0) at alpha 0 until a Born / the crystal camera)
        cr::FieldNpc* n = findNpc(app, -id);
        if (!n) return;
        if (n->model >= 0) n->mdlRead = true;
        n->shownSet = false;
        stopNpcAuto(-id);
        app.npcShown[-id] = false;
        const int index = int(n - app.npcs.data()); // (_read_mdl: a fresh model - no action, alpha or fade kept)
        for (auto* v : {&app.npcDraws, &app.npcHeld})
            v->erase(std::remove_if(v->begin(), v->end(), [&](const App::NpcDraw& d) { return d.index == index; }), v->end());
        buildNpcDraws(app);
        return;
    }
    if (cr::FieldNpc* had = findNpc(app, id)) { // (+0x98: marked - CR1's pushes don't read it)
        if (had->model >= 0) had->mdlRead = true;
        had->shownSet = false; // (19179: the model's drawn yaw = its yaw)
        return;
    }
    cr::NpcRow row;
    if (!app.events.npcRow(id, row)) return;
    // __EvtNpcMdlRead (2.2.0 19212-19262): at the origin, facing 0, and hidden (+0x8c bit 0, +0x55c = 0)
    // when its create flags fail, or when the map list's record at the count of NPCs loaded so far -
    // not the new NPC's own; mostly the 0xFFFF terminator or what follows it - has an id over 399
    // (19255): j007_t01's chequered floor (501, read fifth into map 113's list of two) stands shown
    if (int(app.npcs.size()) > 0x5c) return; // (19220: no room for it)
    cr::FieldNpc n;
    n.id = id;
    n.flagShow = row.flagShow;
    n.flagHide = row.flagHide;
    n.model = row.model;
    n.kind = row.kind;
    n.treasure = row.box;
    n.puzzle = row.puzzle;
    n.scale = row.scale;
    n.native = row.native;
    // (19143 keeps the row: _npc_shodow (35278) reads its +0x12 bit 15 and +0x1e..+0x24 - a read NPC casts its shadow)
    std::memcpy(n.link, row.link, sizeof n.link);
    std::memcpy(n.shadow, row.shadow, sizeof n.shadow);
    n.mdlRead = n.model >= 0;
    app.npcShown[id] = npcCreateFlags(app, n) && app.events.npcListId(app.mapNumber, int(app.npcs.size())) <= 399;
    app.npcs.push_back(n);
    app.npcWarps.push_back(false);
    buildNpcDraws(app);
}
void ViewerHost::freeCamera(bool on) { // __EvtCameraPlayer: the mode and the auto flags only - the eye stays where the last shot put it
    app.cam.free = on;
    if (!on) app.cam.t = app.cam.frames = 0;
}
void ViewerHost::setCamera(const float* eye, const float* target, int frames) {
    if (scrollingField(app)) { // a painted field: FrmAdjCam slides the painting to the shot's (x, y);
        // FrmSetCam on the field (flag word bit 64) takes the 3D path, which starts with SetScroll(0, 0)
        if (!app.cam.free) return;
        if (frames > 0) scriptFieldScroll(app, eye[0], eye[1], frames);
        else {
            app.scroll.offX = app.scroll.offY = 0;
            app.scroll.sliding = false;
        }
        return;
    }
    if (paintedField(app)) return; // shown whole: nothing moves
    const cr::Vec3 e{eye[0], eye[1], eye[2]}, t{target[0], target[1], target[2]};
    if (SDL_getenv("CR_OP_TRACE"))
        std::printf("  set camera eye (%.1f %.1f %.1f) target (%.1f %.1f %.1f) over %d\n", e.x, e.y, e.z, t.x, t.y, t.z, frames);
    if (std::fabs(e.x - t.x) + std::fabs(e.y - t.y) + std::fabs(e.z - t.z) < 1e-4f) return; // no target actor
    App::ScriptCam& c = app.cam;
    if (frames <= 0 || !c.set) { c.eye = e; c.target = t; c.frames = c.t = 0; }
    else { c.eyeFrom = c.eye; c.tgtFrom = c.target; c.eyeTo = e; c.tgtTo = t; c.frames = frames; c.t = 0; }
    c.set = true;
    stepScriptCam(app); // (the slide's first step is this frame's: updateCamera runs after PrcEvnt)
}
void ViewerHost::panCamera(const float* d, int frames) {
    if (scrollingField(app)) { // FrmPanCam's 2D path: twice the deltas on the iPad
        if (app.cam.free) { // (from GetScroll's value: past the painting's edge, as the script left it)
            const App::FieldScroll& fs = app.scroll;
            // (GetScroll as it is now - mid-slide too: FUN_00055234 writes it each frame (53283-53320), PanCam
            // reads it, asm 0x22b84-0x22c1c. 1.0.1's two hand nudges - route 3's 300 on map 86, route 2's 265
            // on map 23 - are gone in 2.2.0)
            // (curX / offX are the scaled view's pixels; scriptFieldScroll scales what it's given - so the base goes
            // back to the record's units first: only the delta is doubled, __EvtFrmPanCam 18646, 0x22b84)
            const float k = std::max(1e-4f, fieldViewScale(app));
            const float x = (fs.scripted ? fs.curX : fs.offX) / k;
            const float y = (fs.scripted ? fs.curY : fs.offY) / k;
            const float dx = d[0], dy = d[1];
            // (GetScroll's x is the painting's offset, as this scroll's is, but its y the camera's - + y
            // up -, this one's the other way: d01's 0, 400 then 0, -400 over 200 frames starts high and
            // pans down to the player, as the reference video shows, and d02's 265 brings the two on the
            // left into view, the nudge landing it on the painting's edge)
            scriptFieldScroll(app, x + 2 * dx, y - 2 * dy, std::max(frames, 1));
        }
        return;
    }
    if (paintedField(app)) return;
    App::ScriptCam& c = app.cam;
    if (!c.set) return;
    const cr::Vec3 dv{d[0], d[1], d[2]};
    c.eyeFrom = c.eye; c.tgtFrom = c.target;
    c.eyeTo = c.eye + dv; c.tgtTo = c.target + dv;
    c.frames = std::max(frames, 1); c.t = 0;
}
void ViewerHost::sceneEffectSet(int set) { // Call 2020 (FUN_000266cc 21319-21327): ScnEffectRead and ReadMapMenu(var 0)
    loadSceneEffectSet(app, set);
    if (set >= 0 && set <= 3) app.mapScreen.readGate = set;
}
void ViewerHost::quake(int amp, int from, int to, int frames) { // EvtQuake: < 0 frames starts one held, else EndQuake
    ++app.scriptQuakes;
    if (frames < 0) startQuake(app.fieldQuake, float(amp), float(from), float(to), 1, true);
    else endQuake(app.fieldQuake, float(amp), float(from), float(to), frames);
    if (!app.battle.active) stepQuake(app.fieldQuake); // (UpdateQuake runs after the script in the same frame)
}
void ViewerHost::vitaLogo(int slot, int picture) { // read_img_set(the table at 0x811122b0 [picture])
    static const char* const kLogo[8] = {"sqex", "mve", "mclogo", "title1", "title2", "chara3", "chara1", "chara2"};
    const char* name = picture == 254 ? "warning" : picture >= 0 && picture < 8 ? kLogo[picture] : nullptr;
    App::VitaLogo& l = app.vitaLogo;
    const int k = std::clamp(std::abs(slot), 0, 1);
    app.renderer.destroy(l.tex[k]);
    cr::Image img;
    const int vitaBefore = app.vitaTextures;
    if (name && loadImage(app, std::string(name) + "_hd.mct", img)) l.tex[k] = app.renderer.upload(img);
    l.fromVita[k] = app.vitaTextures > vitaBefore;
    l.warning[k] = picture == 254;
}
void ViewerHost::vitaLogoShow(int slot, int in, int hold, int out) {
    App::VitaLogo& l = app.vitaLogo;
    l.slot = std::clamp(std::abs(slot), 0, 1), l.phase = 1, l.frame = 0, l.in = in, l.hold = hold, l.out = out, l.skip = false;
    if (!l.tex[l.slot].id) l.phase = 0; // (nothing read: nothing to show)
}
bool ViewerHost::vitaLogoBusy() { return app.vitaLogo.phase != 0; }
void ViewerHost::texDisp(const int32_t* g) {
    App::TexDisp& t = app.texDisp;
    t = App::TexDisp();
    t.on = true;
    for (int k = 0; k < 11; ++k) t.g[k] = g[k];
    ++app.texDispCalls;
    if (SDL_getenv("CR_TEXDISP_DEBUG"))
        std::printf("texdisp: slot %d (%s, image %d) x %d u %d v %d %dx%d fade %d/%d/%d\n", g[0],
                    g[0] >= 0 && g[0] < 16 && app.portraits[g[0]].tex.id ? "loaded" : "EMPTY", g[0] >= 0 && g[0] < 16 ? app.portraits[g[0]].image : -1,
                    g[1], g[3], g[4], g[5], g[6], g[7], g[8], g[9]);
}
void ViewerHost::tutoTexDisp(const int32_t* g) { // MsgUty_TexDispInit(var 1, ...)
    App::TutoTex& t = app.tutoTex;
    app.renderer.destroy(t.tex);
    t = App::TutoTex();
    static const char* const kTuto[14] = { // 0xc9500: the puzzles' (0..7), the battles' (8..11, 13), the field's
        "telepo_01_hd.mct", "telepo_02_hd.mct", "psyco_01_hd.mct", "psyco_02_hd.mct", "tough_01_hd.mct",
        "tough_02_hd.mct",  "pyro_01_hd.mct",   "pyro_02_hd.mct",  "battle_01_hd.mct", "battle_02_hd.mct",
        "battle_03_hd.mct", "gene_01_hd.mct",   "puzzle_01_hd.mct", "battle_04_hd.mct"};
    const int pic = app.game.vars[1];
    cr::Image img;
    if (pic >= 0 && pic < 14) { // the iPad's _hd sheet, else the iPhone's
        std::string name = kTuto[pic];
        if (loadImage(app, name, img) || loadImage(app, name.replace(name.find("_hd"), 3, ""), img)) t.tex = app.renderer.upload(img);
    }
    t.on = true, t.state = 1;
    t.pages = g[1];
    for (int k = 0; k < 4; ++k) t.row[k] = g[2 + 2 * k], t.frames[k] = g[3 + 2 * k];
    t.x = float(g[10]), t.y = float(g[11]);
    app.game.setFlag(4075, false);
    ++app.tutoCalls;
}
void ViewerHost::exclamationMark() { app.exclamation = true; ++app.exclamationCalls; }
void ViewerHost::openWindow(int style, int lines) { // __EvtOpen -> _SetWin: fading in (at once for style 0x10), SE 6 but for 0x30
    app.msgStyle = style, app.msgLines = lines, app.heldPage = false;
    app.msgWin.state = style & 0x10 ? 3 : 1, app.msgWin.t = 0, app.msgWin.alpha = style & 0x10 ? 1.0f : 0.0f;
    if (!(style & 0x30)) app.sound.play(se::kWinOpen);
    stepMsgWin(app); // (PrcWin runs after PrcEvnt in the original's frame; ours ran before the script)
}
void ViewerHost::closeWindow() { // __EvtClose -> _ClrWin: fading out (at once for 0x110), SE 7 but for 0x30
    app.heldPage = false; // Close: a page that ended itself goes with its window
    app.msgWin.state = app.msgStyle & 0x110 ? 6 : 4, app.msgWin.t = 0;
    if (app.msgWin.state == 6) app.msgWin.alpha = 0;
    if (!(app.msgStyle & 0x30)) app.sound.play(se::kWinClose);
    stepMsgWin(app);
}
bool ViewerHost::windowClosed() { return app.msgWin.state == 0 || app.msgWin.state == 6; }
void ViewerHost::toggleFieldHidden() { app.fieldHidden = !app.fieldHidden; }
bool ViewerHost::logoSeen() { return app.logoSeen; }       // Uty_ChkLogo_flg with var 0 = 0: read alone (gac + 0x1f85e6)
void ViewerHost::setLogoSeen() { app.logoSeen = true; }    // ... with var 0 set: set alone
// Uty_FireWall_Init (var 0 = the NPC): the first dungeon's fire walls 453 (side 1) / 454 (side 0) burn -
// effect 0x12 | side at the NPC's point, scale 100, looping (eff3d_set_with_pos) - unless the route's
// flag is set: 1124 (453) / 1125 (454) + 400 x route (the table at 0xcb520)
void ViewerHost::fireWall(int npc) {
    if (npc != 0x1c5 && npc != 0x1c6) return;
    const int side = npc == 0x1c5 ? 1 : 0, route = std::clamp(app.playerChar / 2, 0, 3);
    if (app.game.flag((side ? 1124 : 1125) + 400 * route)) return;
    cr::Vec3 p;
    if (!actorPos(app, npc, p)) return;
    const int h = startEffect(app, app.fieldEffects, 0x12 | side, p, true, 1.0f, 0.0f);
    if (h)
        for (Effect& fx : app.fieldEffects)
            if (fx.handle == h) fx.faceCamera = true;
}
void ViewerHost::padArt() { app.padArt = true; }
// Uty_Btl_ParamUp01 / 02: battle fighter 1 / 0's attack, defense, speed and luck up for 6 rounds
// (+0x8b..+0x8e = 6); outside a battle, for the next one
void ViewerHost::battleParamUp(int fighter) {
    if (fighter < 0 || fighter > 1) return;
    if (app.battle.active) {
        for (int st = cr::kStatusAttackUp; st <= cr::kStatusLuckUp; ++st) app.battle.rules.f[size_t(fighter)].status[st] = 6;
    } else app.paramUp[fighter] = true;
}
void ViewerHost::puzzleStart() { app.puzzleBanner = App::PuzzleBanner(); app.puzzleBanner.on = true; } // Uty_PuzzleStart_Init
void ViewerHost::puzzlePadLock(int n) { app.padLock = n; } // Util 2038: the pad lock (Camp / Save and the pad's buttons wait)
void ViewerHost::warpCrystalCamera(int npc) { // SetFreeCamera, then the camera task
    app.crystalCam = App::CrystalCam{npc, 0};
    app.cam.free = true;
}
void ViewerHost::sceneEffect(bool on, int id) { setSceneEffect(app, on, id); }
// A field actor's node `user` (a model record's user id) as its model stands now - the leader (0) or an NPC
// drawn; false without the node (eff3d_set 61393-61411: _btlcamera_getpos on the node table's entry)
static bool fieldActorPoint(App& app, int actor, int user, cr::Vec3& out) {
    if (user <= 0) return false;
    LoadedModel* lm = nullptr;
    cr::Mat4 world;
    int action = -1;
    float time = 0;
    const cr::MotionBlend* blend = nullptr;
    if (actor == 0) {
        if (!app.havePlayer) return false;
        lm = &app.player, world = playerWorld(shownWalker(app)), action = app.player.action, time = app.player.time, blend = &app.player.blend;
    } else
        for (App::NpcDraw& d : app.npcDraws)
            if (d.id == actor) {
                auto it = app.npcModels.find(d.model);
                if (it == app.npcModels.end()) return false;
                lm = &it->second, world = d.world, action = d.action, time = d.time, blend = &d.blend;
                break;
            }
    if (!lm || lm->cpu.skeleton.nodes.empty()) return false;
    const cr::Model& m = lm->cpu;
    int node = -1;
    for (const auto& t : m.records)
        if (t.user == user)
            for (const auto& [obj, n] : m.objectNodes)
                if (obj == t.id) node = n;
    if (node < 0) return false;
    std::vector<cr::Mat4> pose;
    const cr::Action* act = action >= 0 && action < int(lm->actions.size()) ? &lm->actions[size_t(action)] : nullptr;
    poseWorld(m.skeleton, lm->actions, act, time, *blend, pose);
    if (node >= int(pose.size())) return false;
    const float* n = pose[size_t(node)].m;
    const float* w = world.m;
    out = {w[0] * n[12] + w[4] * n[13] + w[8] * n[14] + w[12], w[1] * n[12] + w[5] * n[13] + w[9] * n[14] + w[13],
           w[2] * n[12] + w[6] * n[13] + w[10] * n[14] + w[14]};
    return true;
}

void ViewerHost::frameEffect(int actor, int effect, int scale, const float* v, bool angled) {
    ++app.frameEffectCalls;
    cr::Vec3 p;
    if (SDL_getenv("CR_OP_TRACE"))
        std::printf("  frame effect %d actor %d scale %d%s at (%.1f %.1f %.1f) angles (%.1f %.1f %.1f)\n", effect, actor, scale,
                    angled ? " angled" : "", v ? v[0] : 0.0f, v ? v[1] : 0.0f, v ? v[2] : 0.0f, v ? v[3] : 0.0f, v ? v[4] : 0.0f,
                    v ? v[5] : 0.0f);
    float pitch = 0, roll = 0;
    // (an actor's at its model's node for this effect - 1101 the body, 1102 the head, 1003 the root - as
    // eff3d_set puts it; its feet without the node)
    // (__EvtFrmPlayEff 18936 / FrmPlayEff2 19618: the NPC only when its control slot holds a model - a model-less
    // one in the room's list (600 / 627 / 658 ...) takes the record's spot, not its parked one)
    const cr::FieldNpc* fn = actor > 0 ? findNpc(app, actor) : nullptr;
    const bool modelled = actor == 0 || (fn && fn->model >= 0);
    if (modelled && actorPos(app, actor, p)) fieldActorPoint(app, actor, effectNode(effect), p);
    else { // not an NPC with a model: where the scene's record has it
        if (!v) return;
        p = {v[0], v[1], v[2]};
        // FrmPlayEff2 (eff3d_set_with_pos_and_ang) takes its angles too: X and Z stand; the Y
        // one gives way to the camera (eff3d_set_with_pos and the _and_ang variant both set the
        // model's camera-facing byte, +0x56f - an NPC's effect goes through eff3d_set to the same;
        // the scene's own eff3d_set_fld effects don't: drawEffects)
        if (angled) pitch = v[3] * kDeg, roll = v[5] * kDeg;
    }
    const int h = startEffect(app, app.fieldEffects, effect, p, true, float(scale) / 100.0f, 0.0f);
    if (h)
        for (Effect& fx : app.fieldEffects)
            if (fx.handle == h) fx.pitch = pitch, fx.roll = roll, fx.faceCamera = true;
    if (h && app.frameEffects.size() < 16) app.frameEffects.push_back({(effect + 1) * (actor + 1), effect, h});
}
void ViewerHost::stopFrameEffect(int effect, int actor) {
    for (auto it = app.frameEffects.begin(); it != app.frameEffects.end();)
        if (actor < 0 ? (*it)[1] == effect : (*it)[0] == (effect + 1) * (actor + 1)) {
            stopEffect(app.fieldEffects, (*it)[2]);
            it = app.frameEffects.erase(it);
        } else ++it;
}
void ViewerHost::npcEffect(int effect, int npc, int dy) {
    ++app.scriptEffectCalls;
    cr::Vec3 p;
    if (!actorPos(app, npc, p)) return;
    const int h = startEffect(app, app.fieldEffects, effect, p + cr::Vec3{0, float(dy), 0}, false, 1.0f, 0.0f);
    if (h) // (Uty_eff3d_set_fld goes through eff3d_set_with_pos: the camera-facing byte set)
        for (Effect& fx : app.fieldEffects)
            if (fx.handle == h) fx.faceCamera = true;
}
void ViewerHost::mapScroll(int npc, float x, float y, float rate) {
    ++app.scroll.mapScrolls;
    if (!scrollingField(app) || !app.cam.free) return; // setScroll2D / 3D only act with the camera free
    app.scroll.sliding = false;
    if (npc < 0) { scrollFieldTo(app, x, y, rate); return; }
    cr::Vec3 p;
    float vx, vy;
    if (actorPos(app, npc, p) && fieldViewPoint(app, p, vx, vy)) scrollFieldTo(app, vx, vy, rate);
}
void ViewerHost::lookAtNpc(int npc) { // SetFreeCamera, SetScroll3D(the npc's place, 1.0)
    app.cam.free = true;
    mapScroll(npc, 0, 0, 1.0f);
}
void ViewerHost::battle(int group, int stage, int flags) { // starts when this frame's script step ends
    app.battle.requested = true;
    app.bgmNo = 0; // (__EvtBattle: G+0x358e0 = 0 - after it the field plays track 0 unless var 60 says)
    app.battle.request = {group, stage, -1, 0};
    app.battle.requestFlags = flags;
}
bool ViewerHost::menu(int n) {
    if (n == 0) { // the title menu: its art alone until a tap, then the plates (the Progress Tracker's offer follows Start)
        app.titleOpen = true;
        app.titleCursor = 0;
        app.titleAnim = 0;
        app.titleStage = 0;
        return true;
    }
    if (n == 1) { openWorldSelect(app); return true; }
    if (n == 2) { openShop(app); return true; }
    if (n == 4) { openCharSelect(app); return true; } // the boot script: Character Select
    if (n == 7) { openStaffRoll(app, app.game.vars[0]); return true; } // ModeStaffRollMenu(var 0)
    if (n == 8) { // flag 0 clear: StartLastBtMenu (the rounds start over); else the pair choice
        if (!app.game.flag(0)) { app.lastBattle.count = app.lastBattle.mask = 0; return false; }
        openLastBattle(app);
        return true;
    }
    if (n == 5 || n == 6) { // SetSaveLoadMenu: at the title, the chosen pair's Load / New; else "Save data?"
        if (n == 6) checkTrophies(app); // (the Vita's Util 6: SetModeSaveLoadMenu, then the trophy check - 0x8102e226)
        openSaveScreen(app, app.booting);
        // Util 6 off the title (__EvtUtil: SetModeSaveLoadMenu(3), 4 with flag 0 - both set the menu's title mark):
        // "Save data?" on the way to the title - saved or not, SaveLoadClr returns there (MuCom_ReturnTitle);
        // a No asks about leaving unsaved (SaveLoadPrc 8 -> 0xc)
        if (n == 6 && !app.booting) app.saveScreen.toTitle = true, app.saveScreen.perfect = app.game.flag(0);
        return true;
    }
    static const char* const kMenu[] = {"title", "world choice", "shop", "yes/no", "start", "save/load", "save/load",
                                        "staff roll", "last battle"};
    app.unsupported.push_back(std::string("menu ") + (n >= 0 && n < 9 ? kMenu[n] : "?") + " on map " +
                              std::to_string(app.mapNumber));
    return false;
}
void ViewerHost::askYesNo(int flag) { app.choiceFlag = flag; app.choiceCursor = 0; app.choiceAnim = 0; }
bool ViewerHost::choiceOpen() {
    return app.choiceFlag >= 0 || app.worldChoiceOpen || app.titleOpen || app.saveScreen.open || app.charSelectOpen || app.staffRoll.open || app.lastBattle.open ||
           (app.shop.open && !app.shop.talkRunning); // (the shopkeeper's talk mustn't wait on the shop)
}
void ViewerHost::stopNpcAuto(int id) { // (the actor's npc_autoctl state cleared: a move or rescale under way stops)
    app.moves.erase(std::remove_if(app.moves.begin(), app.moves.end(), [&](const App::Move& o) { return o.npc == id; }), app.moves.end());
    app.rescales.erase(std::remove_if(app.rescales.begin(), app.rescales.end(), [&](const App::Rescale& o) { return o.npc == id; }),
                       app.rescales.end());
}
void ViewerHost::tiltNpc(int id, float xDeg, float zDeg, bool overMove) {
    cr::FieldNpc* n = id ? findNpc(app, id) : nullptr; // (the leader's model never tilts in the data)
    if (!n) return;
    const float to[2] = {xDeg * kDeg, zDeg * kDeg};
    if (overMove)
        for (App::Move& m : app.moves)
            if (m.npc == id) {
                m.tilt = true;
                for (int k = 0; k < 2; ++k) m.tiltFrom[k] = n->tilt[k], m.tiltTo[k] = to[k];
                return;
            }
    n->tilt[0] = to[0], n->tilt[1] = to[1];
    for (auto& d : app.npcDraws)
        if (d.id == id) d.world = npcWorld(*n);
}
void ViewerHost::faceNpc(int id, int target) {
    cr::Vec3 a, b;
    if (!actorPos(app, id, a) || !actorPos(app, target, b)) return;
    if (std::fabs(b.x - a.x) + std::fabs(b.z - a.z) < 1e-4f) return;
    const float yaw = std::atan2(b.x - a.x, b.z - a.z);
    setActor(app, id, a, &yaw, true);
}

// menuf_mainPut in a puzzle room (G+0x35526 == 1), in tex2d units from the anchor (0, 0): a dark
// panel (0, 12)-(80, 48) (0xb8 black), the infinity mark (792, 58, 72 x 28) at (4, 16) when the
// count is unlimited, else "Left" (484, 184, 30 x 12) at (4, 24) and cmd_number_puzzle's two digits
// ((512 + 28 d, 58), 28 x 28, from x 24 - 44 for one - by 20, y 16; swelling 1.4 .. 1.1 for the 4
// frames after a change); with the help state on, the retry button (420, 184, 44 x 28 - 2.2.0's
// __menuf_mainPut 0x364cc) at (8, 56) - blinking (5 frames of 11) at 0 once the count was above it -
// and Help (984, 64, 40 x 32) at (84, 14); the punishment mark (448, 120, 64 x 64) blinking over a
// refused touch and psychokinesis' arrows (0x366bc..; CR1 has no third pad button - Omega's 420, 216)
void drawPuzzleHud(App& app, float a) {
    cr::MenuPainter& m = app.menu;
    const App::Puzzle& pz = app.puzzle;
    auto piece = [&](float x, float y, float u, float v, float w, float h, float scale = 1) {
        m.pieceTo(x + 16 + w * (1 - scale) / 2, y + 32 + h * (1 - scale) / 2, w * scale, h * scale, u, v, w, h, a);
    };
    const float dark[4] = {0, 0, 0, float(0xb8) / 255.0f * a};
    app.renderer.setOrigin(wideLeft(app)); // (widescreen: the panel in the screen's corner)
    m.quad(16, 12 + 32, 80, 36, dark, dark);
    const int esp = pz.esp < 0 ? -1 : std::max(0, pz.esp - (pz.pkState == 2 && pz.pkMoved));
    if (esp < 0) piece(4, 16, 792, 58, 72, 28);
    else {
        static const float kSwell[5] = {1.0f, 1.1f, 1.2f, 1.3f, 1.4f};
        const char d[3] = {char('0' + esp / 10 % 10), char('0' + esp % 10), 0};
        const float scale = kSwell[std::clamp(pz.pop, 0, 4)];
        float x = esp < 10 ? 44 : 24;
        for (int k = esp < 10 ? 1 : 0; k < 2; ++k, x += 20) piece(x, 16, 512 + 28.0f * float(d[k] - '0'), 58, 28, 28, scale);
        piece(4, 24, 484, 184, 30, 12);
    }
    if (pz.help) {
        if (pz.blink < 5) piece(8, 56, 420, 184, 44, 28);
        piece(84, 14, 984, 64, 40, 32);
    }
    app.renderer.setOrigin(app.renderer.uiX()); // (the mark and the arrows: where on the field)
    // the psychokinesis arrows (2.2.0's _s_Arrows at 0xc7430: on, or fading - their colour's alpha
    // byte), 24 x 24 at their point less 12: left (432, 72), up (480, 48), right (456, 72), down (480, 72)
    static const float kArrowUV[4][2] = {{432, 72}, {480, 48}, {456, 72}, {480, 72}};
    for (int k = 0; k < 4; ++k) {
        const App::Puzzle::Arrow& ar = pz.arrows[k];
        if (ar.on != 1 && ar.timer <= 0) continue;
        const float aa = a * float(ar.alpha) / 255.0f;
        m.pieceTo(ar.x - 12 + 16, ar.y - 12 + 32, 24, 24, kArrowUV[k][0], kArrowUV[k][1], 24, 24, aa);
    }
    if (pz.punished > 0 && pz.punishPhase == 0) piece(pz.punishX, pz.punishY, 448, 120, 64, 64);
}

// menuf_mainPut's counters, a 20 Hz frame: the count's swell (set as it changes:
// SetFldMenu_puzzle_count), the retry button's blink at 0, the punishment mark's
void stepPuzzleHud(App& app) {
    App::Puzzle& pz = app.puzzle;
    if (pz.stage < 0) return;
    const int esp = pz.esp < 0 ? -1 : std::max(0, pz.esp - (pz.pkState == 2 && pz.pkMoved));
    if (esp != pz.shownEsp) {
        // (PuzzleManager::init 42715-42725 zeroes the HUD, then SetFldMenu_puzzle_count swells on any change - the
        // room's first value too, unless it is 0)
        if ((pz.shownEsp == -1000 ? 0 : pz.shownEsp) != esp) pz.pop = 4;
        pz.shownEsp = esp;
    }
    if (pz.pop > 0) --pz.pop;
    for (App::Puzzle::Arrow& ar : pz.arrows) // _ArrowOff's fade: the alpha byte down 0x50 (wrapping) / 0x33 a frame
        if (ar.timer > 0) {
            if (ar.mode == 2) ar.alpha = (ar.alpha - 0x50) & 0xff;
            else if (ar.mode == 1) ar.alpha = (ar.alpha - 0x33) & 0xff;
            --ar.timer;
        }
    if (esp > 0) pz.hadEsp = true;
    if (esp == 0 && pz.hadEsp) pz.blink = pz.blink + 1 < 11 ? pz.blink + 1 : 0;
    else pz.blink = 0;
    if (pz.punished > 0) {
        pz.punishPhase = (pz.punishPhase + 1) % 3;
        --pz.punished;
    }
}

// Uty_PuzzleStart_Put: (516, 264) 224 x 64 at (x1, y1), and (744, 264) - flag 1: (768, 900) - at
// (x2, y2), white at the banner's alpha, in front
void drawPuzzleBanner(App& app) {
    const App::PuzzleBanner& b = app.puzzleBanner;
    if (!b.on || b.alpha <= 0 || !app.windowTex.id) return;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    r.setOrigin(r.uiX());
    const float a = float(b.alpha) / 255.0f;
    m.piece(b.x1 + 16, b.y1 + 32, 516, 264, 224, 64, a);
    const bool other = app.game.flag(1);
    m.piece(b.x2 + 16, b.y2 + 32, other ? 768 : 744, other ? 900 : 264, 224, 64, a);
}

// ---- The field menu (menuf_mainPut / menuf_mainPrc) ----
// A button in the top right corner opens a bar: Camp, Save, Map (maps below 64) and, once skill
// 170 is known (owned - it needn't be worn), the switch for random encounters (this pair's
// unencountflg; flag 4076 refuses it). Laid out in tex2d coordinates; menu units are those +
// (16, 32). The buttons slide 4 frames between the corner (460, -28) and their places.
namespace fieldui {
constexpr float kCorner[2] = {460, -28};
constexpr float kPlace[4][2] = {{432, 32}, {384, -24}, {312, -24}, {240, -24}}; // Camp, Save, Map, encounters
constexpr float kSize[4] = {56, 40, 40, 40};
// window_hd pieces: the corner button closed / open, then the buttons (encounters on / off)
constexpr float kCornerPiece[2][2] = {{432, 992}, {464, 992}};
constexpr float kPiece[4][2] = {{452, 516}, {468, 576}, {512, 388}, {468, 620}};
constexpr float kEncountersOff[2] = {468, 664};
constexpr int kEncounterSkill = 170;
} // namespace fieldui

int fieldRoute(const App& app) { return std::clamp(app.playerChar / 2, 0, 3); }

// 2.2.0's iPhone layout: the bar keeps to the screen's corner, (-16, +32) (its menuf_mainPut /
// Prc and kganMfMainButtonPosTbl)
// (Android 1.1.1's SetFldMenu, 0x65ea4: the whole table - the corner button, the bar's buttons and
// their tap circles - moved by (+ox, -oy) / scale, onto the phone's top right corner)
float fieldDx(const App& app) { return (app.renderer.phoneLayout() ? -16.0f : 0.0f) + androidEdgeX(app); }
float fieldDy(const App& app) { return (app.renderer.phoneLayout() ? 32.0f : 0.0f) - androidEdgeY(app); }

bool fieldMenuWanted(const App& app) { // the field, the player free to walk
    return app.mode == Mode::Maps && app.havePlayer && !app.titleOpen && !app.booting && !app.battle.active && !app.campOpen &&
           !app.shop.open && !app.saveScreen.open && !app.optionsOpen && !app.resumePrompt && !app.mapScreen.open;
}

bool fieldSceneRunning(const App& app) { // G+0x2c0 bits 0 / 3: a script or a message holds the field
    return app.runner.running() || !app.dialogue.empty() || app.itemWindowOpen || app.choiceFlag >= 0 || app.worldChoiceOpen;
}

bool fieldEncountersOff(const App& app) { return app.game.noEncounters[size_t(fieldRoute(app))] || app.game.flag(4076); }

void stepFieldMenu(App& app) {
    App::FieldMenu& f = app.fieldMenu;
    // (__menuf_mainPut, 0x35db4-0x35dd4: nothing drawn while G+0x2c0 & 9 - a script or a message - or a
    // map load holds the field, so the bar never fades: gone at once, back at __menuf_mainSet's 320)
    f.fade = fieldSceneRunning(app) ? 0 : 320;
    if (f.anim > 0) --f.anim;
}

// A button's top-left this frame (tex2d coordinates)
void fieldButtonPos(const App& app, int k, float& x, float& y) {
    using namespace fieldui;
    const App::FieldMenu& f = app.fieldMenu;
    const float t = float(4 - f.anim) / 4; // how far along this slide is
    const float from[2] = {f.open ? kCorner[0] : kPlace[k][0], f.open ? kCorner[1] : kPlace[k][1]};
    const float to[2] = {f.open ? kPlace[k][0] : kCorner[0], f.open ? kPlace[k][1] : kCorner[1]};
    x = from[0] + (to[0] - from[0]) * t + fieldDx(app);
    y = from[1] + (to[1] - from[1]) * t + fieldDy(app);
}

bool fieldButtonShown(const App& app, int k) {
    if (k == 2) return app.mapNumber < 64;
    if (k == 3) return app.game.items.count(fieldui::kEncounterSkill) && app.game.items.at(fieldui::kEncounterSkill) > 0;
    return true;
}

void drawFieldMenu(App& app) {
    using namespace fieldui;
    const App::FieldMenu& f = app.fieldMenu;
    if (!app.windowTex.id || f.fade <= 0) return;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    m.begin(r, app.windowTex, &app.font);
    r.setOrigin(r.uiX());
    const float a = float(std::min(255, f.fade)) / 255.0f;
    if (app.puzzle.stage >= 0) drawPuzzleHud(app, a);
    r.setOrigin(wideRight(app)); // (widescreen: in the screen's corner)
    const bool bar = f.open || f.anim > 0;
    m.piece(460 + 16 + fieldDx(app), -30 + 32 + fieldDy(app), kCornerPiece[bar][0], kCornerPiece[bar][1], 32, 32, a);
    if (!bar) { r.setOrigin(r.uiX()); return; }
    for (int k = 0; k < 4; ++k) {
        if (!fieldButtonShown(app, k)) continue;
        float x, y;
        fieldButtonPos(app, k, x, y);
        const float* src = k == 3 && fieldEncountersOff(app) ? kEncountersOff : kPiece[k];
        // (the Vita's menuf_mainPut: the button under the cursor in 0xffff00 - A B G R, cyan)
        const std::vector<int> e = fieldBarPad(app) ? fieldBarEntries(app) : std::vector<int>();
        const bool lit = f.open && !e.empty() && e[size_t(std::clamp(f.cursor, 0, int(e.size()) - 1))] == k;
        if (lit) m.piece(x + 16, y + 32, src[0], src[1], kSize[k], kSize[k], a, 0.0f, 1.0f, 1.0f);
        else m.piece(x + 16, y + 32, src[0], src[1], kSize[k], kSize[k], a);
    }
    r.setOrigin(r.uiX());
}


// The Vita's bar under the buttons (its __menuf_mainPrc 0x81045cd8 / Put): the entries in its order -
// the encounters' switch (once skill 170 is held), the map (maps below 64), Save, Camp
// (_ComUty_GetBattryState's list, misnamed) - as this file's buttons (0 Camp, 1 Save, 2 Map, 3 encounters)
bool fieldBarPad(const App& app) { return vitaLayout(app); }
std::vector<int> fieldBarEntries(const App& app) {
    std::vector<int> e;
    for (int k : {3, 2, 1, 0})
        if (fieldButtonShown(app, k)) e.push_back(k);
    return e;
}

void toggleFieldMenu(App& app) {
    App::FieldMenu& f = app.fieldMenu;
    if (f.anim > 0) return;
    f.open = !f.open;
    f.anim = 4;
    if (f.open) f.cursor = int(fieldBarEntries(app).size()) - 1; // (the Vita's: the last - Camp - under the cursor)
    menuSound(app, f.open ? se::kCampOpen : se::kCampClose);
}

// The Vita's buttons on the bar (__menuf_mainPrc): ← ↑ / → ↓ move the cursor round the entries
// (FUN_810085d8 / __ScrListSelectSePlay: SE 1), ○ presses the one under it, × shuts the bar; true
// when the bar took the button
bool fieldBarButton(App& app, int b, bool confirm, bool cancel) {
    App::FieldMenu& f = app.fieldMenu;
    if (!fieldBarPad(app) || !f.open || f.anim > 0 || !fieldMenuWanted(app) || fieldSceneRunning(app)) return false;
    const std::vector<int> e = fieldBarEntries(app);
    if (e.empty()) return false;
    const int step = b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_UP      ? -1
                     : b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1
                                                                                                : 0;
    const int n = int(e.size());
    f.cursor = std::clamp(f.cursor, 0, n - 1);
    if (step) {
        const int was = f.cursor;
        f.cursor = (f.cursor + step + n) % n;
        if (f.cursor != was) menuSound(app, 1); // (the Vita's list cursor sound)
        return true;
    }
    if (confirm) { pressFieldButton(app, e[size_t(f.cursor)]); return true; }
    if (cancel) { toggleFieldMenu(app); return true; }
    return false;
}

// The bar's buttons (the bar open and still): 0 Camp, 1 Save, 2 Map, 3 encounters
void pressFieldButton(App& app, int k) {
    if (!app.fieldMenu.open || app.fieldMenu.anim > 0 || !fieldButtonShown(app, k)) return;
    if (k < 2 && app.padLock > 0) return; // (menuf_mainPrc: Camp and Save wait out the pad lock, gac + 0x3cde6: 2.2.0 0x35af0 / 0x35b2c)
    switch (k) {
    case 0:
        menuSound(app, se::kCampOpen);
        openCamp(app);
        break;
    case 1:
        menuSound(app, se::kChoose);
        app.game.vars[0] = 0;
        openSaveScreen(app, false);
        break;
    case 2: // SetMapMenu
        menuSound(app, se::kChoose);
        openMapScreen(app);
        break;
    case 3: { // the original's sounds: 3 switching off, 2 on, the buzzer while flag 4076 holds it
        if (app.game.flag(4076)) {
            menuSound(app, se::kBuzzer);
            break;
        }
        bool& off = app.game.noEncounters[size_t(fieldRoute(app))];
        off = !off;
        menuSound(app, off ? se::kBack : se::kChoose);
        break;
    }
    default: break;
    }
}

// A click in the frame's pixels; true when the field menu took it
bool fieldMenuClick(App& app, float px, float py) {
    if (!fieldMenuWanted(app) || fieldSceneRunning(app) || app.fieldMenu.fade <= 0) return false;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    const float tx = (lx - wideRight(app)) / 2 - 16 - fieldDx(app), ty = ly / 2 - 32 - fieldDy(app); // tex2d
    if (tx >= 456 && tx < 496 && ty >= -32 && ty < 8) { // _pad_chk_trg_down_box(456, -32, 40, 40) (2.2.0: 440, 0)
        toggleFieldMenu(app);
        return true;
    }
    if (!app.fieldMenu.open || app.fieldMenu.anim > 0) return false;
    static const float kCentre[4][3] = {{460, 60, 28}, {404, -4, 20}, {332, -4, 20}, {260, -4, 20}};
    for (int k = 0; k < 4; ++k) {
        const float dx = tx - kCentre[k][0], dy = ty - kCentre[k][1];
        if (fieldButtonShown(app, k) && dx * dx + dy * dy <= kCentre[k][2] * kCentre[k][2]) {
            pressFieldButton(app, k);
            return true;
        }
    }
    return false;
}

// ---- The touch pad (CrossKey, FieldPCCtrl::operate) and the "!" button ----
// A finger put down on the field where nothing else takes it holds a pad there (CrossKey::update):
// its offset from that point steers the leader as the keys do, turned by the camera's heading -
// standing under 14.5 touch units (tex2d), walking under 29, running beyond. The pad shows once the
// finger has moved or after 5 frames: its base where the finger went down, its knob after the
// finger (menuf_mainPut): the iPad's 24 x offset / max(|offset|, 72), half the way a frame; 2.2.0's
// larger art 32 x offset / max(|offset|, 48), a third. With something in reach, the "!" button
// (FollowWindow::checkTouchEasyButton: within 24 of (460, 316); 2.2.0's (444, 284)) examines it, as
// a tap within 30 of its bubble does (FollowWindow::update).
namespace touchpad {
constexpr float kWalk2 = 210.25f, kRun2 = 841.0f; // squared offsets: walking, running
constexpr int kShowAfter = 5;
struct Art { float base[2], baseSize, knob[2], knobSize, reach, limit, ease; }; // window_hd, half scale
constexpr Art kIpad = {{928, 792}, 72, {928, 724}, 48, 24, 72, 0.5f};
constexpr Art kBig = {{928, 796}, 96, {944, 724}, 64, 32, 48, 1.0f / 3}; // 2.2.0's
constexpr float kEasyPiece[2] = {464, 196};
constexpr float kEasySize = 48;
} // namespace touchpad

// The UI atlas's pad: 2.2.0's (iPhone, and the Vita's and Android's laid out as it) is larger - a
// texel of its ring (half-scale (1000, 850): the sheets are 1024 of those across) where the iPad's
// is clear
void noteWindowAtlas(App& app, const cr::Image& wi) {
    const int lw = wi.logicalW ? wi.logicalW : wi.width, lh = wi.logicalH ? wi.logicalH : wi.height;
    if (lw < 1024 || lh < 1024 || wi.rgba.empty()) return;
    const int x = 1000 * wi.width / 1024, y = 850 * wi.height / 1024;
    app.bigPad = wi.rgba[(size_t(y) * size_t(wi.width) + size_t(x)) * 4 + 3] > 64;
}

const touchpad::Art& padArt(const App& app) { return app.bigPad ? touchpad::kBig : touchpad::kIpad; }

// The "!" button's centre (tex2d, from wideRight): the iPad's, or 2.2.0's in its layout
void easyButtonCentre(const App& app, float& x, float& y) {
    x = app.renderer.phoneLayout() ? 444.0f : 460.0f, y = app.renderer.phoneLayout() ? 284.0f : 316.0f;
}

bool easyButtonShown(const App& app) {
    return fieldMenuWanted(app) && !fieldSceneRunning(app) && app.fieldMenu.fade > 0 && interactTarget(app) != nullptr;
}

// A finger down at frame pixel (px, py) on the field: the "!" button or bubble, else the pad.
// True when it took it.
bool touchPadDown(App& app, float px, float py) {
    if (!fieldMenuWanted(app) || fieldSceneRunning(app) || !app.havePlayer || app.puzzle.tpState != 0 ||
        app.puzzle.pkState != 0)
        return false;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    if (easyButtonShown(app)) {
        float ex, ey;
        easyButtonCentre(app, ex, ey);
        const float dx = (lx - wideRight(app)) / 2 - 16 - ex, dy = ly / 2 - 32 - ey;
        const float bx = (lx - app.promptX) / 2, by = (ly - app.promptY) / 2;
        if (dx * dx + dy * dy <= 24 * 24 || (app.promptX >= 0 && bx * bx + by * by <= 30 * 30)) {
            interact(app);
            return true;
        }
    }
    App::TouchPad& t = app.touchPad;
    t = App::TouchPad();
    t.down = true;
    t.ox = (lx - app.renderer.uiX()) / 2 - 16, t.oy = ly / 2 - 32;
    t.hold = touchpad::kShowAfter;
    return true;
}

void touchPadMove(App& app, float px, float py) {
    App::TouchPad& t = app.touchPad;
    if (!t.down) return;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    t.dx = (lx - app.renderer.uiX()) / 2 - 16 - t.ox, t.dy = ly / 2 - 32 - t.oy;
}

void touchPadUp(App& app) { // CheckTap: a touch let go within 5 frames (state 2) examines what the "!" stands over
    App::TouchPad& t = app.touchPad; // (FollowWindow::update: TouchFollowWindowUtil on the nearest, not while a script or a message runs)
    const bool tap = t.down && t.hold >= 0;
    t = App::TouchPad();
    if (tap && !fieldSceneRunning(app) && app.dialogue.empty() && interactTarget(app)) interact(app);
}

// The pad's stick for this step: false standing (or no finger)
bool touchPadStick(const App& app, float& sx, float& sy, bool& run) {
    const App::TouchPad& t = app.touchPad;
    const float d2 = t.dx * t.dx + t.dy * t.dy;
    if (!t.down || d2 < touchpad::kWalk2) return false;
    sx = t.dx, sy = t.dy, run = d2 >= touchpad::kRun2;
    return true;
}

// One 20 Hz frame: whether it shows (moved, or held 5 frames), the knob after the finger
void stepTouchPad(App& app) {
    App::TouchPad& t = app.touchPad;
    if (!t.down) return;
    const float d2 = t.dx * t.dx + t.dy * t.dy;
    if (--t.hold <= 0 || d2 > 1.0f) t.shown = true;
    const touchpad::Art& a = padArt(app);
    const float len = std::max(std::sqrt(d2), a.limit);
    t.knob[0] += (a.reach * t.dx / len - t.knob[0]) * a.ease;
    t.knob[1] += (a.reach * t.dy / len - t.knob[1]) * a.ease;
}

void drawTouchPad(App& app) {
    const App::TouchPad& t = app.touchPad;
    const App::FieldMenu& f = app.fieldMenu;
    if (!app.windowTex.id || f.fade <= 0) return;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    const float alpha = float(std::min(255, f.fade)) / 255.0f;
    m.begin(r, app.windowTex, &app.font);
    if (easyButtonShown(app)) {
        r.setOrigin(wideRight(app)); // (widescreen: in the screen's corner)
        float ex, ey;
        easyButtonCentre(app, ex, ey);
        const float h = touchpad::kEasySize / 2;
        m.piece(ex - h + 16, ey - h + 32, touchpad::kEasyPiece[0], touchpad::kEasyPiece[1], touchpad::kEasySize,
                touchpad::kEasySize, 1);
    }
    r.setOrigin(r.uiX());
    if (!t.down || !t.shown) return;
    const touchpad::Art& a = padArt(app);
    const float b = a.baseSize / 2, k = a.knobSize / 2;
    m.piece(t.ox - b + 16, t.oy - b + 32, a.base[0], a.base[1], a.baseSize, a.baseSize, alpha);
    m.piece(t.ox + t.knob[0] - k + 16, t.oy + t.knob[1] - k + 32, a.knob[0], a.knob[1], a.knobSize, a.knobSize, alpha);
}

// ---- The map screen (__menuu_MapSet / MapPrc / MapPut, ReadMapMenu) ----
// The gate's map_D0n (slot 0x30, 512 x 512; MuTex group 1 pictures 13 the map 480 x 320 at
// (0, 0), 14 the pair's marker 48 x 48 at (0, 320), 15 a marker 16 x 16 at (0, 368)), the pair's
// marker on this field's spot (blinking, 2 frames of 6, the next 48 to the right) and the two
// markers of the story's step: the first of the pair's 25 flags (2601 + 50 x pair) set. A tap
// anywhere closes it. Coordinates are tex2d; menu units are + (16, 32).
namespace mapui {
// 0xc7b20: each field's spot on its gate's map (s16 x, y; -1 the 3D rooms)
extern const short kMapSpot[64][2] = {
    {72, 216}, {122, 196}, {82, 172}, {176, 152}, {132, 152}, {188, 108}, {327, 218}, {288, 196},
    {426, 180}, {338, 176}, {386, 155}, {350, 132}, {360, 88}, {-1, -1}, {100, 204}, {144, 204},
    {110, 160}, {66, 160}, {154, 160}, {166, 116}, {284, 228}, {334, 206}, {294, 184}, {308, 140},
    {262, 140}, {220, 140}, {354, 120}, {402, 120}, {412, 74}, {-1, -1}, {80, 176}, {124, 176},
    {168, 176}, {38, 176}, {92, 132}, {330, 200}, {292, 180}, {254, 156}, {264, 112}, {304, 136},
    {380, 180}, {430, 156}, {418, 200}, {392, 136}, {352, 112}, {364, 68}, {-1, -1}, {116, 186},
    {82, 232}, {160, 188}, {127, 144}, {172, 144}, {322, 212}, {278, 212}, {234, 212}, {334, 168},
    {288, 168}, {244, 168}, {278, 124}, {430, 132}, {442, 86}, {344, 124}, {356, 78}, {-1, -1},
};
// 0xc7c30: per gate, per story step (the first of the pair's 25 flags set), two markers (x, y, kind)
extern const short kMapMark[4][25][2][3] = {
    {
        {{68, 248, 1}, {-1, -1, 0}},
        {{75, 224, 0}, {80, 204, 1}},
        {{87, 224, 0}, {107, 224, 0}},
        {{92, 204, 0}, {112, 204, 0}},
        {{51, 192, 0}, {71, 192, 0}},
        {{133, 216, 0}, {153, 216, 0}},
        {{124, 204, 0}, {129, 184, 1}},
        {{168, 204, 1}, {173, 184, 0}},
        {{145, 172, 0}, {165, 172, 0}},
        {{180, 160, 0}, {185, 140, 1}},
        {{192, 116, 0}, {323, 252, 1}},
        {{297, 228, 1}, {317, 228, 0}},
        {{335, 252, 1}, {422, 212, 1}},
        {{257, 216, 0}, {277, 216, 0}},
        {{303, 204, 0}, {323, 204, 1}},
        {{290, 204, 0}, {338, 150, 0}},
        {{347, 208, 1}, {367, 208, 0}},
        {{353, 184, 0}, {373, 184, 1}},
        {{341, 184, 0}, {347, 164, 1}},
        {{417, 188, 0}, {397, 188, 1}},
        {{428, 188, 0}, {365, 140, 0}},
        {{353, 140, 0}, {358, 120, 1}},
        {{265, 96, 0}, {-1, -1, 0}},
        {{358, 163, 0}, {378, 163, 1}},
        {{-1, -1, -1}, {-1, -1, -1}},
    },
    {
        {{95, 236, 1}, {-1, -1, 0}},
        {{107, 236, 1}, {127, 236, 1}},
        {{113, 212, 0}, {133, 212, 0}},
        {{100, 212, 0}, {106, 192, 1}},
        {{139, 236, 1}, {134, 256, 0}},
        {{76, 191, 1}, {96, 191, 1}},
        {{82, 167, 0}, {102, 167, 0}},
        {{122, 180, 0}, {142, 180, 0}},
        {{69, 168, 0}, {74, 148, 1}},
        {{157, 168, 0}, {162, 148, 1}},
        {{169, 124, 0}, {280, 260, 1}},
        {{299, 236, 0}, {319, 236, 1}},
        {{287, 236, 0}, {292, 216, 1}},
        {{311, 192, 0}, {331, 192, 1}},
        {{299, 192, 0}, {304, 172, 1}},
        {{295, 160, 0}, {275, 160, 0}},
        {{248, 172, 1}, {228, 172, 1}},
        {{234, 148, 0}, {254, 148, 0}},
        {{215, 171, 1}, {210, 191, 0}},
        {{322, 148, 0}, {342, 148, 1}},
        {{369, 140, 0}, {389, 140, 0}},
        {{404, 128, 0}, {409, 108, 1}},
        {{416, 84, 0}, {-1, -1, 0}},
        {{-1, -1, -1}, {-1, -1, -1}},
        {{-1, -1, -1}, {-1, -1, -1}},
    },
    {
        {{77, 208, 1}, {-1, -1, 0}},
        {{88, 208, 1}, {108, 208, 1}},
        {{94, 184, 0}, {115, 184, 0}},
        {{84, 184, 0}, {89, 164, 1}},
        {{51, 184, 0}, {70, 184, 0}},
        {{44, 208, 1}, {64, 208, 1}},
        {{133, 208, 1}, {153, 208, 1}},
        {{140, 184, 0}, {159, 184, 0}},
        {{172, 184, 0}, {177, 164, 1}},
        {{40, 184, 0}, {45, 164, 1}},
        {{95, 140, 0}, {327, 232, 1}},
        {{301, 208, 1}, {321, 208, 0}},
        {{345, 208, 0}, {365, 208, 1}},
        {{262, 188, 1}, {282, 188, 0}},
        {{257, 164, 0}, {262, 144, 1}},
        {{269, 164, 0}, {289, 164, 1}},
        {{269, 120, 0}, {274, 100, 1}},
        {{395, 188, 0}, {414, 188, 1}},
        {{421, 208, 0}, {427, 188, 1}},
        {{401, 164, 1}, {421, 164, 0}},
        {{409, 252, 0}, {415, 232, 1}},
        {{363, 144, 1}, {382, 144, 0}},
        {{318, 144, 0}, {338, 144, 1}},
        {{357, 120, 0}, {362, 100, 1}},
        {{368, 76, 0}, {-1, -1, 0}},
    },
    {
        {{120, 215, 1}, {-1, -1, 0}},
        {{95, 235, 0}, {100, 215, 1}},
        {{128, 203, 0}, {148, 203, 0}},
        {{110, 190, 0}, {114, 169, 1}},
        {{50, 247, 0}, {70, 247, 0}},
        {{163, 191, 0}, {168, 171, 1}},
        {{175, 147, 0}, {180, 127, 1}},
        {{139, 159, 0}, {159, 159, 0}},
        {{131, 147, 0}, {318, 239, 1}},
        {{289, 227, 0}, {309, 227, 0}},
        {{324, 215, 0}, {330, 195, 1}},
        {{245, 227, 0}, {265, 227, 0}},
        {{236, 215, 0}, {242, 195, 1}},
        {{257, 183, 0}, {277, 183, 0}},
        {{280, 171, 0}, {286, 151, 1}},
        {{301, 183, 0}, {321, 183, 0}},
        {{270, 127, 0}, {275, 107, 1}},
        {{348, 173, 0}, {368, 173, 1}},
        {{336, 171, 0}, {342, 151, 1}},
        {{397, 155, 0}, {417, 155, 1}},
        {{434, 135, 0}, {439, 115, 1}},
        {{348, 127, 0}, {353, 107, 1}},
        {{360, 83, 0}, {-1, -1, 0}},
        {{-1, -1, -1}, {-1, -1, -1}},
        {{-1, -1, -1}, {-1, -1, -1}},
    },
};
} // namespace mapui

int mapGate(int map) { return map < 13 ? 0 : map < 29 ? 1 : map < 46 ? 2 : 3; } // split by the 3D rooms 13, 29, 46

void openMapScreen(App& app) {
    using namespace mapui;
    App::MapScreen& ms = app.mapScreen;
    if (app.mapNumber < 0 || app.mapNumber >= 64) return;
    // (the sheet ReadMapMenu read from the dungeon script's var 0 - d0150 / d0250 / d0350 show their own
    // gate's, not the next; the map number's split only before any)
    const int gate = ms.readGate >= 0 ? ms.readGate : mapGate(app.mapNumber);
    if (ms.gate != gate || !ms.tex.id) { // ReadMapMenu: only when the gate changes
        if (ms.tex.id) app.renderer.destroy(ms.tex);
        cr::Image img;
        static const char* const kFile[4] = {"map_D01.mct", "map_D02.mct", "map_D03.mct", "map_D04.mct"};
        if (loadImage(app, kFile[gate], img)) ms.tex = app.renderer.upload(img);
        ms.gate = gate;
    }
    const short* sp = kMapSpot[app.mapNumber];
    ms.spot[0] = sp[0] < 0 ? -1 : sp[0] - 24;
    ms.spot[1] = sp[1] < 0 ? -1 : sp[1] - 20;
    for (auto& m : ms.marks) m[0] = m[1] = -1;
    const int base = kFlagBase + kFlagStride * fieldRoute(app);
    for (int step = 0; step < kSteps; ++step)
        if (app.game.flag(base + step)) {
            for (int k = 0; k < 2; ++k) {
                const short* m = kMapMark[gate][step][k];
                if (m[0] - 8 > -1 && m[1] - 16 > -1) ms.marks[k][0] = m[0] - 8, ms.marks[k][1] = m[1] - 16, ms.markType[k] = m[2];
            }
            break;
        }
    ms.open = true;
    ms.closing = false;
    ms.fade = 0;
    ms.blink = 0;
    ms.clock = 0;
    // (__menuu_MapSet 76391 / 76416: the pulse starts in its 255 state - the spot full once MapPrc's 4-frame
    // wait is over, as video 12 1264.4 s shows)
    ms.wait = 4, ms.pulse = 2, ms.pulseCount = 0, ms.pulseAlpha = 0;
}

void closeMapScreen(App& app) {
    if (!app.mapScreen.open || app.mapScreen.closing || app.mapScreen.wait > 0) return; // (MapPrc 1: not for 4 frames)
    menuSound(app, se::kChoose); // __effgo(2)
    app.mapScreen.closing = true;
    app.mapScreen.closeWait = 6; // (76449-76456: DAT_000d3704 = 4, counted down until below 0)
}

void stepMapScreen(App& app) {
    App::MapScreen& ms = app.mapScreen;
    if (!ms.open) return;
    ms.blink = (ms.blink + 1) % 12;
    if (ms.closing) {
        ms.fade = std::max(0, ms.fade - 1);
        if (--ms.closeWait <= 0) ms.open = false, ms.closing = false;
        ms.pulseAlpha = 0;
        return;
    }
    ms.fade = std::min(4, ms.fade + 1);
    if (ms.wait > 0) { --ms.wait; return; }
    // __menuu_MapPrc (76487-76530): the spot's alpha 0 for 3 frames, +51 a frame for 6, 255 for 11, -51 for 6
    int& c = ms.pulseCount;
    switch (ms.pulse) {
    case 0: ms.pulseAlpha = 0; if (c > 2) ms.pulse = 1, c = 0; break;
    case 1: if (c > 5) c = 0, ms.pulse = 2, ms.pulseAlpha = 255; else ms.pulseAlpha += 51; break;
    case 2: ms.pulseAlpha = 255; if (c > 10) c = 0, ms.pulse = 3; break;
    case 3: if (c > 5) c = 0, ms.pulse = 0, ms.pulseAlpha = 0; else ms.pulseAlpha -= 51; break;
    }
    c = (c + 1) & 0xff;
}

void drawMapScreen(App& app) {
    App::MapScreen& ms = app.mapScreen;
    if (!ms.open) return;
    cr::Renderer& r = app.renderer;
    cr::MenuPainter& m = app.menu;
    const float a = float(ms.fade) / 4;
    // __menuu_MapPut (0x7a598): PutWin / CmdBase of the menu's window, then the map sheet's picture and the
    // marks; no mask - __menuu_MapSet (0x7a104) sets none - so the field shows untouched around the map's
    // own panel (the port dimmed the whole screen under it as well)
    r.setOrigin(r.uiX());
    m.begin(r, app.windowTex, &app.font);
    if (!ms.tex.id) return;
    m.imagePiece(ms.tex, 16, 32, 0, 0, 480, 320, a);
    const int frame = ms.blink / 6;
    // (tex2d's z: the map -4, a type-0 mark -3, the pair's marker -2, a type-1 mark -1 - MapSet 76403-76406)
    auto marks = [&](bool over) {
        for (int k = 0; k < 2; ++k)
            if (ms.marks[k][0] >= 0 && (ms.markType[k] == 1) == over)
                m.imagePiece(ms.tex, float(ms.marks[k][0]) + 16, float(ms.marks[k][1]) + 32, float(16 * frame), 368, 16, 16, a);
    };
    marks(false);
    // (MapPut 76565-76573: the spot's colour alpha << 24 | 0xffffff, the pulse; the story's marks opaque)
    if (ms.spot[0] >= 0)
        m.imagePiece(ms.tex, float(ms.spot[0]) + 16, float(ms.spot[1]) + 32, float(48 * frame), 320, 48, 48,
                     a * std::clamp(ms.pulseAlpha, 0.0f, 255.0f) / 255);
    marks(true);
}

// ---- Puzzle rooms (PuzzleScene, PuzzleManager, the touch abilities, the PuzzleObjects) ----
// See game/puzzle.hpp. The room's NPCs with NPC table +20 bits sit on the stage's grid; the
// pair's touch ability acts on them (teleport: tap a block or gem anywhere on the grid and swap
// places with it, one ESP each). Gems are picked up by walking onto them (+1 ESP); the gem
// crystal (WarpCrystalNG) comes when none is left, the switch crystal (WarpCrystalWS) while every
// weigh switch is pressed: the first time its NPC's script plays (flag 4000, var 1 = its event),
// which marks the room (flag 3300 + stage) - afterwards it shows. A cleared room (flag 3000 +
// stage) keeps its crystals and loses its blocks and gems. Field effects (eff3d_set_fld 2..9)
// aren't drawn yet.

// eff3d_set_fld: effect `id` where an NPC (index) or the leader (-1) stands; a looped one runs
// until stopped by its handle
int fieldEffect(App& app, int id, cr::Vec3 at, bool loop = false) {
    return startEffect(app, app.fieldEffects, id, at, loop, 1.0f, 0.0f);
}
int npcEffect(App& app, int id, int npc, bool loop) {
    if (npc < 0) return fieldEffect(app, id, app.walker.pos, loop);
    if (npc >= int(app.npcs.size())) return 0;
    const cr::FieldNpc& n = app.npcs[size_t(npc)];
    return fieldEffect(app, id, {n.pos[0], n.pos[1], n.pos[2]}, loop);
}

// A world point on the frame (logical 1024 x 768 pixels, the whole screen's width), as drawFrame
// lays out the stage view (false when it's off the view or behind the camera)
bool fieldToLogical(const App& app, cr::Vec3 p, float& lx, float& ly) {
    const float SW = app.renderer.screenW(), H = cr::Renderer::kLogicalH;
    float vx, vy, vw, vh;
    if (scrollingField(app)) { // (as drawFrame places the view: the scroll as shown, the quake's offset)
        const float fz = fieldFillZoom(app);
        vw = fieldViewW(app) * fz, vh = fieldViewH(app) * fz;
        float sx, sy;
        shownScroll(app, sx, sy);
        vx = (SW - vw) / 2 + sx * fz, vy = (H - vh) / 2 - sy * fz;
    } else {
        vw = vh = std::min(SW, H);
        vx = (SW - vw) / 2, vy = (H - vh) / 2;
    }
    vx += std::trunc(vx + app.fieldQuake.off[0]) - std::trunc(vx), vy += std::trunc(vy + app.fieldQuake.off[1]) - std::trunc(vy);
    float hx, hy;
    if (!fieldViewPoint(app, p, hx, hy)) return false;
    lx = vx + hx * vw / fieldViewW(app), ly = vy + hy * vh / fieldViewH(app);
    return true;
}
// Logical pixels <-> tex2d (the 480 x 320 layout: menu units less (16, 32), half the pixels)
void logicalToTex2d(const App& app, float lx, float ly, float& tx, float& ty) {
    tx = (lx - app.renderer.uiX()) / 2 - 16, ty = ly / 2 - 32;
}

namespace puzzle {

const cr::PuzzleStage* stage(const App& app) {
    return app.puzzle.stage >= 0 ? &cr::kPuzzleStage[app.puzzle.stage] : nullptr;
}

bool cleared(const App& app) { return app.puzzle.stage >= 0 && app.game.flag(3000 + app.puzzle.stage); }

int* cellSlot(App& app, int layer, int r, int c) {
    const cr::PuzzleStage* st = stage(app);
    if (!st || layer < 0 || layer > 2 || r < 0 || c < 0 || r >= st->rows || c >= st->cols) return nullptr;
    return &app.puzzle.grid[size_t((layer * st->rows + r) * st->cols + c)];
}

int objectAt(App& app, int layer, int r, int c) {
    const int* slot = cellSlot(app, layer, r, c);
    return slot ? *slot : -1;
}

cr::Vec3 centre(const App& app, int r, int c) { // getCenterPosAtCell
    const cr::PuzzleStage* st = stage(app);
    if (!st) return {};
    return {st->x0 + cr::kPuzzleCell * (0.5f + float(c)), st->y, st->z0 + cr::kPuzzleCell * (0.5f + float(r))};
}

bool cellOf(const App& app, cr::Vec3 p, int& r, int& c) { // getCellNo
    const cr::PuzzleStage* st = stage(app);
    if (!st) return false;
    const float fr = (p.z - st->z0) / cr::kPuzzleCell, fc = (p.x - st->x0) / cr::kPuzzleCell;
    if (fr < 0 || fc < 0) return false;
    r = int(fr), c = int(fc);
    return r < st->rows && c < st->cols;
}

App::NpcDraw* drawOf(App& app, int npc) {
    for (App::NpcDraw& d : app.npcDraws)
        if (d.index == npc) return &d;
    return nullptr;
}

void fade(App& app, int npc, float from, float to, int frames) { // mdl_set_alpha
    if (App::NpcDraw* d = drawOf(app, npc)) {
        d->faded = true;
        d->alphaFrom = from, d->alphaTo = to, d->alpha = from;
        d->fadeFrames = d->fadeLeft = std::max(frames, 1);
    }
}

void show(App& app, App::Puzzle::Obj& o, bool on) { // npc_born / npc_kill
    if (o.shown == on) return;
    o.shown = on;
    buildNpcDraws(app);
}

void place(App& app, App::Puzzle::Obj& o, cr::Vec3 p) { // PuzzleObject::setPos
    if (o.npc < 0 || o.npc >= int(app.npcs.size())) return;
    cr::FieldNpc& n = app.npcs[size_t(o.npc)];
    n.pos[0] = p.x, n.pos[1] = p.y, n.pos[2] = p.z;
    if (App::NpcDraw* d = drawOf(app, o.npc)) d->world = npcWorld(n);
}

// A weigh switch below a cell: pressed (5, `silent` at the start) or let go (6)
void press(App& app, int r, int c, bool on, bool silent = false) {
    const int s = objectAt(app, 0, r, c);
    if (s < 0) return;
    App::Puzzle::Obj& sw = app.puzzle.objs[size_t(s)];
    if (sw.type != cr::kTypeWeighSwitch) return;
    if (on && !silent) app.sound.play(0x28);
    sw.on = on;
}

bool switchAt(App& app, int r, int c) {
    const int s = objectAt(app, 0, r, c);
    return s >= 0 && app.puzzle.objs[size_t(s)].type == cr::kTypeWeighSwitch;
}

bool allSwitchesOn(App& app) { // IsOnAllSwitch
    for (const App::Puzzle::Obj& o : app.puzzle.objs)
        if (o.type == cr::kTypeWeighSwitch && !o.on) return false;
    return true;
}

bool gemsLeft(App& app) {
    for (const App::Puzzle::Obj& o : app.puzzle.objs)
        if (o.type == cr::kTypeGem && o.state != 0) return true;
    return false;
}

constexpr float kGhostAlpha = 0x60 / 255.0f;

// PuzzleManager::init (PuzzleScene::init, and again on a reset): the objects from the room's NPCs
void enter(App& app) {
    App::Puzzle& pz = app.puzzle;
    pz = App::Puzzle();
    app.fieldEffects.clear(); // the objects' effects go with them
    pz.stage = cr::puzzleStage(app.area, app.mapNumber);
    const cr::PuzzleStage* st = stage(app);
    if (!st) { pz.stage = -1; return; }
    pz.leaderWas = app.walker.pos;
    // _MfMain_ClrPzlData: the count 0, Retry / Help shown; a cleared room (PuzzleManager::init) goes
    // no further - MfMain_SetPzlHelpState(0), no objects: its NPCs stay as the map has them
    pz.esp = 0;
    if (cleared(app)) {
        pz.help = false;
        buildNpcDraws(app);
        return;
    }
    pz.esp = st->esp;
    app.game.setFlag(3300 + pz.stage, false);
    app.game.vars[13] = 0; // the uses
    pz.grid.assign(size_t(3 * st->rows * st->cols), -1);
    for (size_t i = 0; i < app.npcs.size(); ++i) {
        const int type = cr::puzzleType(app.npcs[i].puzzle, st->ability);
        if (!type) continue;
        App::Puzzle::Obj o;
        o.npc = int(i);
        o.type = type;
        o.layer = cr::puzzleLayer(type);
        const cr::Vec3 p{app.npcs[i].pos[0], app.npcs[i].pos[1], app.npcs[i].pos[2]};
        const bool inGrid = cellOf(app, p, o.row, o.col);
        o.shown = npcActive(app, app.npcs[i]);
        o.baseY = app.npcs[i].pos[1];
        if ((type == cr::kTypeNormal && st->ability != cr::kAbilityPsychokinesis) || type == cr::kTypeGem) { // their init: npc_born
            o.shown = true;
            o.state = 1;
        } // (NormalBlockPK's init / postInit are PuzzleObject's - empty: as the map has it, no switch pressed)
        if (type == cr::kTypeCrystal) o.shown = false;
        if (type == cr::kTypeFragile || type == cr::kTypeRed) o.shown = true; // npc_born
        if (type == cr::kTypeBlue) o.shown = false, o.ghost = true; // npc_kill, alpha 0x60
        if (type == cr::kTypeCandle) { // lit at the start (+22)
            o.shown = app.npcs[i].link[0] != 0;
            o.ghost = !o.shown;
        }
        pz.objs.push_back(o);
        if (inGrid && type != cr::kTypeCrystal)
            if (int* slot = cellSlot(app, o.layer, o.row, o.col)) *slot = int(pz.objs.size() - 1);
    }
    for (App::Puzzle::Obj& o : pz.objs) // InitSwitchOn (NormalBlockTP's): a block standing on a switch presses it
        if (o.type == cr::kTypeNormal && st->ability != cr::kAbilityPsychokinesis && o.state == 1 && switchAt(app, o.row, o.col)) {
            o.onSwitch = true;
            press(app, o.row, o.col, true, true);
            cr::Vec3 p = centre(app, o.row, o.col);
            p.y = app.npcs[size_t(o.npc)].pos[1] + cr::kPuzzleSwitchRise;
            place(app, o, p);
        }
    for (App::Puzzle::Obj& o : pz.objs) // WarpCrystalWS::postInit
        if (o.type == cr::kTypeCrystal && (app.npcs[size_t(o.npc)].puzzle & cr::kPuzzleObject40))
            o.shown = allSwitchesOn(app);
    buildNpcDraws(app);
    for (const App::Puzzle::Obj& o : pz.objs)
        if (o.ghost) fade(app, o.npc, kGhostAlpha, kGhostAlpha, 1);
}

void stepBlock(App& app, App::Puzzle::Obj& o) { // NormalBlockTP / NormalGem (the teleported side)
    switch (o.state) {
    case 1: // onIdle: a teleport asked for - fade out
        if (o.type == cr::kTypeGem && o.pending != 8) { // picked up by walking onto it
            const cr::Vec3 c = centre(app, o.row, o.col);
            const cr::Vec3 p = app.walker.pos;
            if (std::fabs(p.x - c.x) < cr::kPuzzleCell * 0.5f && std::fabs(p.z - c.z) < cr::kPuzzleCell * 0.5f) {
                npcEffect(app, 2, o.npc); // (NormalGem::onIdle, 2.2.0's 0x6cea0: effect 2 alone as it is picked up)
                show(app, o, false);
                app.sound.play(0x27);
                if (int* slot = cellSlot(app, o.layer, o.row, o.col); slot && *slot >= 0 && &app.puzzle.objs[size_t(*slot)] == &o)
                    *slot = -1;
                if (app.puzzle.esp >= 0) ++app.puzzle.esp; // incESPCount
                o.state = 0;
                return;
            }
        }
        if (o.pending == 8) {
            npcEffect(app, 3, o.npc); // NormalBlockTP::onIdle / NormalGem::onIdle: the going-away effect
            fade(app, o.npc, 1, 0, o.outFrames);
            if (o.onSwitch) press(app, o.row, o.col, false);
            o.state = 2;
            o.pending = 0;
        }
        break;
    case 2: // onDisappear: then at its new cell
        if (--o.waitOut > 0) break;
        {
            const bool sw = switchAt(app, o.toRow, o.toCol);
            if (int* from = cellSlot(app, o.layer, o.row, o.col); from && *from >= 0 && &app.puzzle.objs[size_t(*from)] == &o) *from = -1;
            const int self = int(&o - app.puzzle.objs.data());
            o.row = o.toRow, o.col = o.toCol;
            if (int* to = cellSlot(app, o.layer, o.row, o.col)) *to = self; // replacePuzzleObject
            o.onSwitch = o.type == cr::kTypeNormal && sw;
            cr::Vec3 p = centre(app, o.row, o.col);
            // (NormalGem / NormalBlockTP's arrival, FUN_0006cd7c / FUN_0006ca58: the object's own record y -
            // getEvsTbl() + 8: a gem's 15, a block's 10 - raised 2.5 on a switch)
            p.y = o.baseY;
            if (sw) p.y += cr::kPuzzleSwitchRise;
            place(app, o, p);
            npcEffect(app, 4, o.npc); // onDisappear (the block's and the gem's)
            fade(app, o.npc, 0, 1, o.inFrames);
            o.state = 3;
        }
        break;
    case 3: // onAppear
        if (--o.waitIn > 0) break;
        if (o.onSwitch) press(app, o.row, o.col, true);
        o.state = 1;
        break;
    default: break;
    }
}

// The warp crystals: the gem one (WarpCrystalNG) once the gems are gone, the switch one
// (WarpCrystalWS) while every switch is pressed; the first time the crystal's NPC script plays

void stepCrystal(App& app, App::Puzzle::Obj& o) {
    if (o.fadeOut > 0 && --o.fadeOut == 0) buildNpcDraws(app); // (its fade out over: no longer drawn)
    const unsigned bits = app.npcs[size_t(o.npc)].puzzle;
    bool ready = false;
    if (bits & cr::kPuzzleObject100) ready = !gemsLeft(app);
    else if (bits & cr::kPuzzleObject40) {
        ready = allSwitchesOn(app);
        if (!ready && o.shown) { // a switch let go: it goes again
            npcEffect(app, 3, o.npc);
            fade(app, o.npc, 1, 0, 4);
            o.fadeOut = 4; // (WS 68156-68163 / CS 70083-70086: npc_kill, then mdl_set_alpha 0xff -> 0 over 4 frames)
            show(app, o, false);
            app.sound.play(0x29);
            return;
        }
    } else if (bits & cr::kPuzzleObject800) {
        ready = allCandlesLit(app);
        if (!ready && o.shown) { // one gone out: it goes again
            npcEffect(app, 3, o.npc);
            fade(app, o.npc, 1, 0, 4);
            o.fadeOut = 4; // (WS 68156-68163 / CS 70083-70086: npc_kill, then mdl_set_alpha 0xff -> 0 over 4 frames)
            show(app, o, false);
            app.sound.play(0x29);
            return;
        }
    } else return;
    if (!ready || o.shown) return;
    if (!app.game.flag(3300 + app.puzzle.stage)) { // the scene
        if (app.runner.running()) return;
        app.game.setFlag(4000, true);
        const int evt = app.npcs[size_t(o.npc)].id;
        app.game.vars[1] = evt;
        app.runner.start(app.events, evt, app.game, app.area, app.mapNumber);
        app.runnerWhat = "puzzle crystal";
        return;
    }
    show(app, o, true);
    npcEffect(app, 4, o.npc);
    fade(app, o.npc, 0, 1, 4);
    app.sound.play(0x2a);
}


// Teleport::onCheckTap's choice made: swap places with object `t`
void teleportTo(App& app, int t) {
    App::Puzzle& pz = app.puzzle;
    if (pz.busy || pz.tpState || t < 0 || t >= int(pz.objs.size())) return;
    App::Puzzle::Obj& o = pz.objs[size_t(t)];
    if ((o.type != cr::kTypeNormal && o.type != cr::kTypeGem) || o.state != 1) return;
    int pr, pc;
    if (!cellOf(app, app.walker.pos, pr, pc)) return;
    if (pz.esp == 0) { punish(app, t); return; } // no ESP left - only the mark
    pz.busy = true;
    ++app.game.vars[13]; // decESPCount: var 13 counts the uses; the count stops at 0
    if (pz.esp > 0) --pz.esp;
    o.pending = 8; // TeleportMessage(the player's cell, 4, 4, 6, 6)
    o.toRow = pr, o.toCol = pc, o.outFrames = 4, o.inFrames = 4, o.waitOut = 6, o.waitIn = 6;
    app.playerAlphaFrom = 1, app.playerAlphaTo = 0, app.playerFadeFrames = app.playerFadeLeft = 4, app.playerFaded = true;
    npcEffect(app, 7, -1);
    npcEffect(app, 8, -1);
    app.padLock = 12;
    app.sound.play(0x2b);
    pz.tpState = 1, pz.tpTimer = 6, pz.tpTarget = t;
}

void stepTeleport(App& app) {
    App::Puzzle& pz = app.puzzle;
    if (pz.tpState == 1 && --pz.tpTimer <= 0) { // onDisappear: to the block's (old) cell
        const App::Puzzle::Obj& o = pz.objs[size_t(pz.tpTarget)];
        setActor(app, 0, centre(app, o.row, o.col), nullptr, false);
        app.playerAlphaFrom = 0, app.playerAlphaTo = 1, app.playerFadeFrames = app.playerFadeLeft = 4;
        npcEffect(app, 9, -1);
        app.sound.play(0x2c);
        pz.tpState = 2, pz.tpTimer = 6;
    } else if (pz.tpState == 2 && --pz.tpTimer <= 0) { // onAppear
        pz.busy = false;
        pz.tpState = 0;
    }
}


// SetPunishmentDispOn: no ESP left for what was asked of object `t` - the mark (448, 120, 64 x 64)
// over the touch, blinking a frame in three for 11 frames, the count swelling, sound 4
void punish(App& app, int t) {
    App::Puzzle& pz = app.puzzle;
    float tx = pz.tapX, ty = pz.tapY;
    if (tx < 0 && t >= 0 && t < int(pz.objs.size())) { // the keyboard: over the object
        const cr::FieldNpc& n = app.npcs[size_t(pz.objs[size_t(t)].npc)];
        float lx, ly;
        if (fieldToLogical(app, {n.pos[0], n.pos[1] + 5.0f, n.pos[2]}, lx, ly)) logicalToTex2d(app, lx, ly, tx, ty);
    }
    if (tx < 0) tx = 240, ty = 160;
    pz.punishX = tx - 32, pz.punishY = ty - 32;
    pz.punished = 11, pz.punishPhase = 0;
    pz.pop = 4;
    app.sound.play(se::kBuzzer);
}

// ---- Psychokinesis (Psychokinesis, PsychokinesisDetector, NormalBlockPK) ----
// Touch a block: the leader holds still (effect 5), the block rises (sound 0x22, +2 a frame for 2
// frames, then 5 above its place) and bobs; a swipe up / down / left / right (past a dead zone of
// |dx| < 29, -35 < dy < 20 in the 480 x 320 screen) sends it along its row or column to the cell
// before the next block or wall (or the edge): sound 0x2d, 3 units a frame, then it falls (gravity
// 0.8, sound 0x23) or settles 2.5 up on a switch and presses it. A block that reaches the leader
// on its way squashes the try: effect 0x19, sound 0x54, the room resets. A slide costs one ESP
// (counted as the block lands); letting go without one costs nothing.


// IsHitPC: the leader inside the NPC's box (+0x1c / +0x24 its size, +0x28 / +0x30 its offset; x, z)
bool inBox(App& app, const cr::FieldNpc& n) {
    const cr::Vec3 p = app.walker.pos;
    return std::fabs(p.x - (n.pos[0] + n.offset[0])) <= n.box[0] * 0.5f &&
           std::fabs(p.z - (n.pos[2] + n.offset[2])) <= n.box[2] * 0.5f;
}

void squash(App& app) {
    fieldEffect(app, 0x19, app.walker.pos + cr::Vec3{0, 5, 0});
    app.sound.play(0x54);
    reset(app);
}

void pkLanded(App& app) { // the block's message 4: Psychokinesis::onWaitMove
    App::Puzzle& pz = app.puzzle;
    if (pz.pkMoved) {
        ++app.game.vars[13];
        if (pz.esp > 0) --pz.esp;
    }
    pz.busy = false;
    pz.pkState = 0, pz.pkTarget = -1, pz.pkMoved = false;
    stopEffect(app.fieldEffects, pz.pkEffect); // eff3d_delete(5)
    pz.pkEffect = 0;
    app.padLock = 2;
}

// _ArrowOn(k, x, y, z) (2.2.0 FUN_00036fd4): the arrow at the screen point of (x, y + 5, z) moved by its table's
// offset (2.2.0's 0xbb030: (-36, 0), (0, -24), (36, 0), (0, 24) iPhone points, doubled on the iPad's
// pixels - tex2d units either way); it stays there, opaque (0xffffffff)
void arrowOn(App& app, int k, cr::Vec3 p) {
    static const float kOffset[4][2] = {{-36, 0}, {0, -24}, {36, 0}, {0, 24}};
    App::Puzzle::Arrow& a = app.puzzle.arrows[k];
    float lx, ly, tx = 240, ty = 160;
    if (fieldToLogical(app, {p.x, p.y + 5.0f, p.z}, lx, ly)) logicalToTex2d(app, lx, ly, tx, ty);
    a.on = 1, a.alpha = 0xff;
    a.x = tx + kOffset[k][0], a.y = ty + kOffset[k][1];
}

// _ArrowOff(k, mode): one that's on goes - at once (0), over 5 frames (1: alpha - 0x33 a frame) or
// 10 (2: - 0x50 a frame, the byte wrapping round - the chosen arrow flickers out)
void arrowOff(App& app, int k, int mode) {
    App::Puzzle::Arrow& a = app.puzzle.arrows[k];
    if (a.on == -1) return;
    a.on = -1, a.mode = mode;
    a.timer = mode == 2 ? 10 : mode == 1 ? 5 : 0;
}
void arrowsOff(App& app, int mode) { for (int k = 0; k < 4; ++k) arrowOff(app, k, mode); } // _ArrowOffAll

// NormalBlockPK::onRise's end (ShowArrows; 2.2.0 0x54734): an arrow each way the block could go - the neighbour
// cell inside the grid and empty: 0 left (column - 1), 1 up (row - 1), 2 right, 3 down
void showArrows(App& app, const App::Puzzle::Obj& o) {
    const cr::FieldNpc& n = app.npcs[size_t(o.npc)];
    const cr::Vec3 p{n.pos[0], n.pos[1], n.pos[2]};
    static const int kStep[4][2] = {{0, -1}, {-1, 0}, {0, 1}, {1, 0}};
    for (int k = 0; k < 4; ++k) {
        const int r = o.row + kStep[k][0], c = o.col + kStep[k][1];
        if (cellSlot(app, 1, r, c) && objectAt(app, 1, r, c) < 0) arrowOn(app, k, p);
    }
}

void stepPkBlock(App& app, App::Puzzle::Obj& o) {
    cr::FieldNpc& n = app.npcs[size_t(o.npc)];
    auto setY = [&](float y) { place(app, o, {n.pos[0], y, n.pos[2]}); };
    switch (o.pk) {
    case 0: // onIdle: taken hold of
        if (o.pkMsg != 1) break;
        app.sound.play(0x22);
        if (o.onSwitch) press(app, o.row, o.col, false);
        o.onSwitch = false;
        o.pkTimer = 2;
        o.pkMsg = 0;
        o.pk = 1;
        break;
    case 1: // onRise: then the arrows, unless it was let go meanwhile
        setY(n.pos[1] + 2.0f);
        if (--o.pkTimer > 0) break;
        setY(o.baseY + 5.0f);
        if (o.pkMsg != 2) showArrows(app, o);
        o.bob = 0;
        o.pk = 2;
        break;
    case 2: { // onWaitSlide (FUN_00054250, 52564ff): a drop first (FUN_000536bc - no bob that frame), then y from
              // the angle, then the angle 9 degrees on; then a send (3)
        if (o.pkMsg == 2) {
            o.pkMsg = 0;
            o.vy = 0;
            o.pk = switchAt(app, o.row, o.col) ? 5 : 4;
            break;
        }
        setY(o.baseY + 5.0f + std::sin(float(o.bob) * 3.14159265f / 180.0f));
        o.bob = (o.bob + 9) % 360;
        if (o.pkMsg == 3) {
            o.pkMsg = 0;
            // the way it goes: its arrow over 10 frames, the others at once (onWaitSlide, 2.2.0 0x54250)
            const int chosen = o.toCol < o.col ? 0 : o.toRow < o.row ? 1 : o.toCol > o.col ? 2 : 3;
            for (int k = 0; k < 4; ++k) arrowOff(app, k, k == chosen ? 2 : 0);
            app.sound.play(0x2d);
            const cr::Vec3 from = centre(app, o.row, o.col), to = centre(app, o.toRow, o.toCol);
            const float dx = to.x - from.x, dz = to.z - from.z, d = std::sqrt(dx * dx + dz * dz);
            o.pkTimer = std::max(1, int(d / 3.0f));
            o.vx = d > 0 ? dx / d * 3.0f : 0, o.vz = d > 0 ? dz / d * 3.0f : 0;
            o.pk = 3;
        }
        break;
    }
    case 3: // onMove: the leader in the block's box squashes the try
        place(app, o, {n.pos[0] + o.vx, n.pos[1], n.pos[2] + o.vz});
        if (inBox(app, n)) { squash(app); return; }
        if (--o.pkTimer > 0) break;
        {
            if (int* from = cellSlot(app, o.layer, o.row, o.col); from && *from >= 0 && &app.puzzle.objs[size_t(*from)] == &o) *from = -1;
            const int self = int(&o - app.puzzle.objs.data());
            o.row = o.toRow, o.col = o.toCol;
            if (int* to = cellSlot(app, o.layer, o.row, o.col)) *to = self;
            const cr::Vec3 c = centre(app, o.row, o.col);
            place(app, o, {c.x, n.pos[1], c.z});
            startQuake(app.fieldQuake, 0.5f, 100, 0, 8, false); // Quake::start(0.5, 100 -> 0, 8 frames)
            o.vy = 0;
            o.pk = switchAt(app, o.row, o.col) ? 5 : 4;
        }
        break;
    case 4: case 5: { // onDown (the box tested every frame) / onDownToSwitch (never)
        const float floor = o.baseY + (o.pk == 5 ? cr::kPuzzleSwitchRise : 0.0f);
        const float y = n.pos[1] + o.vy;
        o.vy -= 0.8f;
        setY(std::max(y, floor));
        if (o.pk == 4 && inBox(app, n)) { squash(app); return; }
        if (y <= floor) {
            if (o.pk == 5) {
                o.onSwitch = true;
                press(app, o.row, o.col, true);
            } else app.sound.play(0x23);
            o.pk = 0;
            pkLanded(app);
        }
        break;
    }
    default: break;
    }
}

// Take hold of block `t` (onCheckTouch); (sx, sy) the touch (tex2d), -1e9 by key
void pkGrab(App& app, int t, float sx, float sy, float fromX, float fromY) {
    App::Puzzle& pz = app.puzzle;
    if (pz.busy || pz.pkState || t < 0 || t >= int(pz.objs.size())) return;
    App::Puzzle::Obj& o = pz.objs[size_t(t)];
    int pr, pc;
    if (o.type != cr::kTypeNormal || o.state != 1 || o.pk != 0 || !cellOf(app, app.walker.pos, pr, pc)) return;
    if (pz.esp == 0) { punish(app, t); return; } // no ESP left - only the mark
    pz.busy = true;
    o.pkMsg = 1;
    // (PsychokinesisDetector::init, 0x43dcc: from getNearestPuzzleObject's point - the block's on the
    // screen, not the finger's - so the dead zone (-29..29, -35..20) is around the block)
    pz.pkState = 1, pz.pkTarget = t, pz.pkMoved = false;
    pz.pkFromX = fromX > -1e8f ? fromX : sx, pz.pkFromY = fromX > -1e8f ? fromY : sy;
    pz.pkAtX = sx, pz.pkAtY = sy, pz.pkHeld = sx > -1e8f;
    pz.pkPointerCancelled = false;
    pz.pkEffect = npcEffect(app, 5, -1, true);
}

// Psychokinesis::onCheckSlide's choice: the direction (1 up the grid / row - 1, 2 down, 3 left /
// column - 1, 4 right) sends the block to the cell before the next block or wall (or the edge) -
// the count shown one less at once (counted as it lands), the pad locked 10 frames; 0, or no room
// that way, lets it go (every arrow over 5 frames). A block not yet taken hold of waits
void pkSend(App& app, int dir) {
    App::Puzzle& pz = app.puzzle;
    if (pz.pkState != 1 || pz.pkTarget < 0) return;
    App::Puzzle::Obj& o = pz.objs[size_t(pz.pkTarget)];
    if (o.pk == 0 && o.pkMsg == 1) return; // (the hold's message not taken yet)
    const int dr = dir == 1 ? -1 : dir == 2 ? 1 : 0, dc = dir == 3 ? -1 : dir == 4 ? 1 : 0;
    int r = o.row, c = o.col;
    if (dir)
        for (;;) {
            const int nr = r + dr, nc = c + dc;
            if (!cellSlot(app, 1, nr, nc)) break;
            const int at = objectAt(app, 1, nr, nc);
            if (at >= 0) {
                const int t = pz.objs[size_t(at)].type;
                if (t == cr::kTypeNormal || t == cr::kTypeFixed) break;
            }
            r = nr, c = nc;
        }
    if (r == o.row && c == o.col) { // nowhere to go: let go
        arrowsOff(app, 1);
        o.pkMsg = 2;
    } else {
        o.toRow = r, o.toCol = c, o.pkMsg = 3; // PsychokinesisMessage
        pz.pkMoved = true;
        app.padLock = 10;
    }
    pz.pkState = 2;
    pz.pkPointerCancelled = false;
}

// PsychokinesisDetector::update, 2.2.0 0x54d98 - decomp 53061-53090's way: a = atan2(dx, dy) in double,
// then float; strict bands of float pi / 4 - down, right, up (Repeat2PI), else left - so an exact
// diagonal to the right falls to LEFT, not to |dx| >= |dy|'s right
int pkDirection(float dx, float dy) {
    const float a = float(std::atan2(double(dx), double(dy)));
    const float pi = 3.14159274f, q = pi * 0.25f, h = pi * 0.5f, two = 6.28318548f;
    int dir = 3;
    if (a > -q && a < q) dir = 2;
    else if (h - q < a && a < h + q) dir = 4;
    else {
        const float r = a >= two ? a - two : a < 0 ? a + two : a;
        if (pi - q < r && r < pi + q) dir = 1;
    }
    return dir;
}

// PsychokinesisDetector::update (2.2.0 0x54d98), a frame while a block is held by a touch: that touch let go -
// let go of the block; still down and out of the dead zone (-29 < dx < 29, -35 < dy < 20 in
// tex2d units from where it took hold) - the way it went; else wait
void pkDetect(App& app) {
    App::Puzzle& pz = app.puzzle;
    if (pz.pkState != 1 || pz.pkFromX < -1e8f || pz.pkPointerCancelled) return;
    if (!pz.pkHeld) { pkSend(app, 0); return; }
    const float dx = pz.pkAtX - pz.pkFromX, dy = pz.pkAtY - pz.pkFromY;
    if (dx > -29 && dx < 29 && dy > -35 && dy < 20) return;
    pkSend(app, pkDirection(dx, dy));
}

// ---- Toughness (Toughness, FragileBlock, RedBlock, BlueBlock) ----
// A tap on a fragile, red or blue block that's there (isValid) costs one ESP (none left: only the
// mark) - effect 7 on the leader, sound 0x2f - and sends message 9: to the fragile block alone (it
// breaks: effect 10, sound 0x29, off the grid), or to every object on the block layer, where each
// red and blue block changes over: one put away goes solid again (alpha 0x60 to 1 in 4 frames,
// effect 4, sound 0x2a - once a frame, NoOverlapSE), squashing the try when the leader stands in
// its box (effect 0x19, sound 0x54, the room resets); a solid one is put away (1 to 0x60, effect
// 3, sound 0x29). Red blocks start solid, blue ones put away.

void toughTap(App& app, int t) {
    App::Puzzle& pz = app.puzzle;
    if (pz.busy || t < 0 || t >= int(pz.objs.size())) return;
    App::Puzzle::Obj& o = pz.objs[size_t(t)];
    int pr, pc;
    if ((o.type != cr::kTypeFragile && o.type != cr::kTypeRed && o.type != cr::kTypeBlue) || !o.shown ||
        !cellOf(app, app.walker.pos, pr, pc))
        return;
    if (pz.esp == 0) { punish(app, t); return; } // no ESP left - only the mark
    ++app.game.vars[13]; // decESPCount
    if (pz.esp > 0) --pz.esp;
    npcEffect(app, 7, -1);
    app.sound.play(0x2f);
    if (o.type == cr::kTypeFragile) o.hit = true;
    else
        for (App::Puzzle::Obj& b : pz.objs) b.hit |= b.layer == 1; // postMessage(1, 9)
}

void stepTough(App& app) {
    App::Puzzle& pz = app.puzzle;
    bool born = false, killed = false, rebuild = false, squash = false;
    for (App::Puzzle::Obj& o : pz.objs) {
        if (!o.hit) continue;
        o.hit = false;
        const cr::FieldNpc& n = app.npcs[size_t(o.npc)];
        if (o.type == cr::kTypeFragile) {
            if (!o.shown) continue;
            o.shown = false; // npc_kill, removePuzzleObject
            npcEffect(app, 10, o.npc);
            if (int* slot = cellSlot(app, o.layer, o.row, o.col); slot && *slot == int(&o - pz.objs.data())) *slot = -1;
            app.sound.play(0x29);
            rebuild = true;
        } else if (o.type == cr::kTypeRed || o.type == cr::kTypeBlue || o.type == cr::kTypeCandle) {
            if (!o.shown) {
                o.shown = true, o.ghost = false;
                born = rebuild = true;
                npcEffect(app, 4, o.npc);
                // came in on the leader: the squash - the other blocks still take their change-over this
                // frame (each object's update runs after PuzzleScene::reset, 68911-68914)
                if (inBox(app, n)) squash = true;
            } else {
                o.shown = false, o.ghost = true;
                killed = rebuild = true;
                npcEffect(app, 3, o.npc);
            }
        }
    }
    if (!rebuild) return;
    buildNpcDraws(app);
    if (born) app.sound.play(0x2a);
    if (killed) app.sound.play(0x29);
    for (const App::Puzzle::Obj& o : pz.objs) {
        if (o.type != cr::kTypeRed && o.type != cr::kTypeBlue && o.type != cr::kTypeCandle) continue;
        const App::NpcDraw* d = drawOf(app, o.npc);
        if (!d) continue;
        const bool wasGhost = d->alphaTo < 0.5f;
        if (o.ghost && !wasGhost) fade(app, o.npc, 1, kGhostAlpha, 4);
        else if (!o.ghost && wasGhost) fade(app, o.npc, kGhostAlpha, 1, 4);
    }
    if (squash) {
        fieldEffect(app, 0x19, app.walker.pos + cr::Vec3{0, 5, 0});
        app.sound.play(0x54);
        reset(app);
    }
}

// ---- Pyrokinesis (Pyrokinesis, CandleSwitch, CandleBlock, WarpCrystalCS) ----
// A tap on a candle switch (within the window; none left: only the mark) costs one ESP and sends it
// message 10. Out (0): it lights - effect 0xd, sound 0x31, SendMessage, 15 frames, then lit (3,
// flame effect 0xc). Lit: a tap puts it out - sound 0x32, the flame off, effect 0xe, SendMessage,
// 20 frames cooling (5), then out. SendMessage: in stages 48..55, message 12 with the switch's id
// to the block layer - each candle block whose +24 is that id changes over (as a red block; +22 =
// lit at the start); from stage 56, message 11 to the objects at the cells of the (up to four)
// NPCs listed at the switch's +22: a switch out catches (5 frames, effect 0xd, sound 0x31, lit 15
// later), a lit one goes out (5 frames, then as a tap) - neither passes it on - and draw3D runs a
// red beam (0x600000ff, 1.5 wide) from the sender for 20 frames. A message waits while a switch is
// lighting or cooling. WarpCrystalCS comes once every switch on the grid is lit and goes again
// when one isn't.

void sendCandle(App& app, App::Puzzle::Obj& sw) { // (anonymous)::SendMessage
    App::Puzzle& pz = app.puzzle;
    const cr::FieldNpc& n = app.npcs[size_t(sw.npc)];
    if (pz.stage < 56) {
        for (App::Puzzle::Obj& b : pz.objs)
            if (b.type == cr::kTypeCandle && app.npcs[size_t(b.npc)].link[1] == n.id) b.hit = true;
        return;
    }
    for (int k = 0; k < 4 && n.link[k] != 0; ++k)
        for (size_t i = 0; i < app.npcs.size(); ++i) {
            if (app.npcs[i].id != n.link[k]) continue;
            int r, c;
            if (cellOf(app, {app.npcs[i].pos[0], app.npcs[i].pos[1], app.npcs[i].pos[2]}, r, c)) {
                const int t = objectAt(app, 1, r, c);
                if (t >= 0 && pz.objs[size_t(t)].type == cr::kTypeCandleSwitch) {
                    App::Puzzle::Obj& to = pz.objs[size_t(t)];
                    to.csMsg = 11, to.csFrom = sw.npc, to.beamT = 0;
                }
            }
            break; // _GetNpcCtl: the first with that id
        }
}

void stepCandleSwitch(App& app, App::Puzzle::Obj& o, bool& lightSE, bool& outSE) {
    if (o.csFrom >= 0 && ++o.beamT > 20) o.csFrom = -1;
    const cr::FieldNpc& n = app.npcs[size_t(o.npc)];
    const cr::Vec3 top{n.pos[0], n.pos[1] + 4.5f, n.pos[2]}; // the effects' point
    auto goOut = [&] { // the flame off, effect 0xe
        stopEffect(app.fieldEffects, o.flame);
        o.flame = 0;
        fieldEffect(app, 0xe, top);
    };
    switch (o.cs) {
    case 0:
        if (o.csMsg == 11) o.csTimer = 5, o.cs = 1;
        else if (o.csMsg == 10) {
            fieldEffect(app, 0xd, top);
            app.sound.play(0x31);
            sendCandle(app, o);
            o.csTimer = 15, o.cs = 2;
        }
        o.csMsg = 0;
        break;
    case 1: // onIgnitionDelay
        if (--o.csTimer < 1) { fieldEffect(app, 0xd, top); lightSE = true; o.csTimer = 15, o.cs = 2; }
        break;
    case 2:
        if (--o.csTimer < 1) { o.flame = fieldEffect(app, 0xc, top, true); o.cs = 3; }
        break;
    case 3: // onLighted
        if (o.csMsg == 11) o.csTimer = 5, o.cs = 4;
        else if (o.csMsg == 10) {
            outSE = true;
            goOut();
            sendCandle(app, o);
            o.csTimer = 20, o.cs = 5;
        }
        o.csMsg = 0;
        break;
    case 4: // onExtinguishDelay
        if (--o.csTimer < 1) { outSE = true; goOut(); o.csTimer = 20, o.cs = 5; }
        break;
    case 5:
        if (--o.csTimer < 1) o.cs = 0;
        break;
    default: break;
    }
}

bool allCandlesLit(App& app) {
    for (const App::Puzzle::Obj& o : app.puzzle.objs)
        if (o.type == cr::kTypeCandleSwitch && objectAt(app, 1, o.row, o.col) == int(&o - app.puzzle.objs.data()) && o.cs != 3)
            return false;
    return true;
}

void pyroTap(App& app, int t) {
    App::Puzzle& pz = app.puzzle;
    if (t < 0 || t >= int(pz.objs.size())) return;
    App::Puzzle::Obj& o = pz.objs[size_t(t)];
    int pr, pc;
    if (o.type != cr::kTypeCandleSwitch || !cellOf(app, app.walker.pos, pr, pc)) return;
    if (pz.pyroCool > 0) return; // (FUN_0006e558: no tap looked at for 20 frames after one taken)
    if (pz.esp == 0) { punish(app, t); return; } // no ESP left - only the mark
    pz.pyroCool = 20;
    ++app.game.vars[13]; // decESPCount
    if (pz.esp > 0) --pz.esp;
    o.csMsg = 10;
}

void stepPyro(App& app) {
    bool lightSE = false, outSE = false;
    for (App::Puzzle::Obj& o : app.puzzle.objs)
        if (o.type == cr::kTypeCandleSwitch) stepCandleSwitch(app, o, lightSE, outSE);
    if (lightSE) app.sound.play(0x31); // NoOverlapSE
    if (outSE) app.sound.play(0x32);
}

// PuzzleScene: a reset fades out (sound 0x44), builds the room again, puts the leader back where
// it came in, fades in; the pad locked 24 frames
void reset(App& app) {
    App::Puzzle& pz = app.puzzle;
    if (pz.stage < 0 || pz.scene != 4) return;
    app.sound.play(0x44);
    app.fadeGrey = 0, app.fadeFrom = 0, app.fadeTo = 1, app.fadeFrames = 10, app.fadeT = 0; // (PuzzleScene::reset: _SetMask(0), 500 ms)
    app.padLock = 24;
    pz.scene = 2;
}

// What the room's ability works on: teleport - a block or gem in play; psychokinesis - a block;
// toughness - a fragile, red or blue block that's there (isValid); pyrokinesis - a candle switch
bool target(const App& app, const App::Puzzle::Obj& o) {
    const cr::PuzzleStage* st = stage(app);
    if (!st || o.layer != 1 || !o.shown) return false;
    switch (st->ability) {
    case cr::kAbilityTeleport: return (o.type == cr::kTypeNormal || o.type == cr::kTypeGem) && o.state == 1;
    case cr::kAbilityPsychokinesis: return o.type == cr::kTypeNormal && o.state == 1;
    case cr::kAbilityToughness: return o.type == cr::kTypeFragile || o.type == cr::kTypeRed || o.type == cr::kTypeBlue;
    case cr::kAbilityPyrokinesis: return o.type == cr::kTypeCandleSwitch;
    default: return false;
    }
}

// getNearestPuzzleObject (2.2.0 0x453a8: the block layer; its first best the screen's width squared - no limit
// in effect): the object whose point - (x, y + its record's +0x20 height, z) on the screen - is
// nearest the touch (tex2d), then the ability's window around that point (the touch less it): a
// gem |dx| <= 17, 7 <= dy <= 33, anything else |dx| <= 20, -16 <= dy <= 21. -1 when it's out
// of its window or isn't one the room's ability works on. Objects put away (red / blue / candle
// blocks) are still there to be nearest; broken or taken ones are gone
bool inWindow(int type, float dx, float dy) {
    return type == cr::kTypeGem ? std::fabs(dx) <= 17 && dy >= 7 && dy <= 33 : std::fabs(dx) <= 20 && dy >= -16 && dy <= 21;
}
int pick(App& app, float tx, float ty, float* atX, float* atY) {
    const App::Puzzle& pz = app.puzzle;
    int best = -1;
    float bestD = 1e30f, bx = 0, by = 0;
    for (size_t i = 0; i < pz.objs.size(); ++i) {
        const App::Puzzle::Obj& o = pz.objs[i];
        if (o.layer != 1 || (!o.shown && !o.ghost)) continue;
        const cr::FieldNpc& n = app.npcs[size_t(o.npc)];
        float lx, ly, px, py;
        if (!fieldToLogical(app, {n.pos[0], n.pos[1] + n.box[1], n.pos[2]}, lx, ly)) continue;
        logicalToTex2d(app, lx, ly, px, py);
        const float d = (px - tx) * (px - tx) + (py - ty) * (py - ty);
        if (d < bestD) bestD = d, best = int(i), bx = px, by = py;
    }
    if (best < 0) return -1;
    const App::Puzzle::Obj& o = pz.objs[size_t(best)];
    if (atX) *atX = bx;
    if (atY) *atY = by;
    return inWindow(o.type, tx - bx, ty - by) && target(app, o) ? best : -1;
}

// Teleport::onCheckTap / Toughness::update / Pyrokinesis::update (2.2.0 0x6c364 / 0x6d754 / 0x6e558;
// CheckTap::update 0x7b514): with the leader in a cell and
// not moved this frame (FieldPCCtrl + 0x28), and pyrokinesis' 20 frames over, CheckTap - the
// touch let go within 5 frames of going down is a tap, where it went down; then the object there
void stepTap(App& app) {
    App::Puzzle& pz = app.puzzle;
    const int ability = stage(app)->ability;
    if (ability == cr::kAbilityPyrokinesis && pz.pyroCool > 0 && --pz.pyroCool > 0) return;
    if ((ability == cr::kAbilityTeleport && pz.tpState) || pz.leaderMoved) return;
    int r, c;
    if (!cellOf(app, app.walker.pos, r, c)) return;
    App::Puzzle::Tap& tap = pz.tap;
    if (tap.state != 1) return;
    ++tap.frames;
    if (!tap.released) return;
    const bool tapped = tap.frames < 6; // (state 2; 3 held too long)
    const float tx = tap.x, ty = tap.y;
    tap = App::Puzzle::Tap();
    if (!tapped || pz.padHold > 0) return;
    const int t = pick(app, tx, ty);
    if (t < 0) return;
    pz.tapX = tx, pz.tapY = ty;
    if (ability == cr::kAbilityToughness) toughTap(app, t);
    else if (ability == cr::kAbilityPyrokinesis) pyroTap(app, t);
    else teleportTo(app, t);
    pz.tapX = pz.tapY = -1;
}

// A logical point -> the field view's pixels (FieldCamera::convertPos_ScreenToViewport)
void logicalToView(const App& app, float lx, float ly, float& hx, float& hy) {
    const float SW = app.renderer.screenW(), H = cr::Renderer::kLogicalH;
    float vx, vy, vw, vh; // the stage view on screen, as drawFrame lays it out
    if (scrollingField(app)) {
        const float fz = fieldFillZoom(app);
        vw = fieldViewW(app) * fz, vh = fieldViewH(app) * fz;
        vx = (SW - vw) / 2 + app.scroll.offX * fz, vy = (H - vh) / 2 - app.scroll.offY * fz;
    } else {
        vw = vh = std::min(SW, H);
        vx = (SW - vw) / 2, vy = (H - vh) / 2;
    }
    hx = (lx - vx) * fieldViewW(app) / vw, hy = (ly - vy) * fieldViewH(app) / vh;
}

// ScrollCamera::update: checkTouch (2.2.0 0x7b7c4, 77446-77483) - 5 frames on, one touch down opens 6 frames
// for a double tap (touchesBegan 640-641: tapCount 2 sets gac+0 = 2 for a frame; touch_set keeps one touch
// only - a second finger is never seen): a double tap with one touch down then scrolls the view to it
// (state 1: the camera off the leader, the ability paused) - else, or once the next single touch's frames
// pass without one, state 0 again. While scrolled, setScroll2D(the point, 0.25) every frame
void stepScroll(App& app) {
    App::Puzzle::Scroll& sc = app.puzzle.scroll;
    const App::Fingers& f = app.fingers;
    if (sc.phase == 1) {
        bool done = true;
        if (++sc.count < 6) {
            if (f.doubleTap && f.held == 1) {
                float lx, ly;
                app.renderer.toLogical(f.x, f.y, lx, ly);
                logicalToView(app, lx, ly, sc.x, sc.y);
                sc.state = 1;
            } else done = false;
        } else sc.state = 0;
        if (done) sc.phase = 0, sc.count = 0;
    } else if (++sc.count > 4 && f.held == 1) sc.phase = 1, sc.count = 0;
    if (sc.state == 1 && scrollingField(app)) scrollFieldTo(app, sc.x, sc.y, 0.25f);
}

// One 20 Hz frame (PuzzleManager::update, under PuzzleScene::update)
void step(App& app) {
    App::Puzzle& pz = app.puzzle;
    app.fingers.fresh = 0;
    struct TapGone { App& a; ~TapGone() { a.fingers.doubleTap = false; } } tapGone{app}; // (FUN_0001d044: gac+0 cleared each frame)
    if (pz.stage < 0) return;
    if (pz.scene == 2) { // fading out to the reset (PuzzleScene::onFadeOut)
        if (app.fadeT < app.fadeFrames) return;
        const cr::Vec3 start = app.puzzleStart;
        const float heading = app.puzzleStartHeading;
        const int stageNo = pz.stage;
        app.npcs = app.events.npcs(app.mapNumber); // every object back where the room has it
        enter(app);
        if (app.puzzle.stage == stageNo) { // FieldPCCtrl::reset: where and how it came in, standing
            setActor(app, 0, start, &heading, false);
            app.walker.gait = cr::Gait::Stand;
            app.leaderYawSet = false; // (FieldPCCtrl::reset 40940-40942: the drawn yaw too)
            app.scroll.followed = false; // (FieldCamera::setScrollPlayer(1.0): the view there at once)
            app.puzzle.leaderWas = start;
        }
        app.fadeFrom = 1, app.fadeTo = 0, app.fadeFrames = 10, app.fadeT = 0; // (_SetMask(1), 500 ms)
        app.puzzle.scene = 3;
        return;
    }
    if (pz.scene == 3) {
        if (app.fadeT >= app.fadeFrames) pz.scene = 4;
        return;
    }
    // FieldPCCtrl + 0x28: the leader moved this frame; gac + 0x3cde4: 3 frames after the pad was held
    pz.leaderMoved = app.walker.gait != cr::Gait::Stand || app.walker.pos.x != pz.leaderWas.x || app.walker.pos.z != pz.leaderWas.z;
    pz.leaderWas = app.walker.pos;
    if (app.touchPad.down) pz.padHold = 3;
    else if (pz.padHold > 0) --pz.padHold;
    if (cleared(app) || app.runner.running() || !app.dialogue.empty()) return;
    for (int k = 0; k < 2; ++k) // the buttons' scripts done: retry (var 0 = 1) / the other (var 1 = 666)
        if (pz.asked[k]) {
            pz.asked[k] = false;
            if (k == 0 && app.game.vars[0] == 1) { reset(app); return; }
        }
    ++pz.clock;
    if (!pz.busy) stepScroll(app); // (PuzzleManager::update 42536-42539: ScrollCamera only while no ability is busy, +0x26)
    if (pz.scroll.state != 1) { // the ability's update
        stepTeleport(app);
        if (stage(app)->ability == cr::kAbilityPsychokinesis) {
            pkDetect(app); // onCheckSlide
            if (pz.pkState == 2) app.padLock = 10; // onWaitMove
        } else stepTap(app);
    }
    if (stage(app)->ability == cr::kAbilityPyrokinesis) stepPyro(app);
    if (stage(app)->ability == cr::kAbilityToughness || stage(app)->ability == cr::kAbilityPyrokinesis) {
        stepTough(app); // the red / blue and candle blocks' changing over
        if (pz.scene != 4) return; // squashed: the room resets
    }
    const bool pk = stage(app)->ability == cr::kAbilityPsychokinesis;
    for (size_t i = 0; i < pz.objs.size(); ++i) {
        App::Puzzle::Obj& o = pz.objs[i];
        if (pk && o.type == cr::kTypeNormal) {
            stepPkBlock(app, o);
            if (app.puzzle.scene != 4) return; // squashed: the room resets
        } else if (o.type == cr::kTypeNormal || o.type == cr::kTypeGem) stepBlock(app, o);
        else if (o.type == cr::kTypeCrystal) stepCrystal(app, o);
    }
}

// The pad's two buttons (2.2.0's PuzzleManager::update 0x45700: EvrStart 6 Retry, 7 Help - no
// third, unlike Omega's), refused while the pad is locked (gac + 0x3cde6); they then wait for their
// script's answer. (A cleared room takes neither: update returns first, the port's help state off)
bool ask(App& app, int k) {
    App::Puzzle& pz = app.puzzle;
    if (pz.stage < 0 || app.padLock > 0 || app.runner.running() || !pz.help) return false;
    const uint32_t at = app.events.returnEntry(6 + k);
    if (!at || !app.runner.startAt(app.events, at, 6 + k, app.game, app.area, app.mapNumber)) return false;
    app.runnerWhat = k == 0 ? "puzzle retry" : "puzzle help";
    pz.asked[k] = true;
    return true;
}

// The keyboard's way: the first object straight ahead of the leader along its row / column (past
// put-away blocks), if the ability works on it
int ahead(App& app) {
    int r, c;
    if (!cellOf(app, app.walker.pos, r, c)) return -1;
    const float h = app.walker.heading;
    const float dx = std::sin(h), dz = std::cos(h);
    const int sr = std::fabs(dz) >= std::fabs(dx) ? (dz > 0 ? 1 : -1) : 0, sc = sr ? 0 : (dx > 0 ? 1 : -1);
    for (int k = 1; k < 12; ++k) {
        const int o = objectAt(app, 1, r + sr * k, c + sc * k);
        if (o < 0 || !app.puzzle.objs[size_t(o)].shown) continue;
        return target(app, app.puzzle.objs[size_t(o)]) ? o : -1;
    }
    return -1;
}

// Uty_PuzzleStart_Proc: flag 0 ends it; var 0 = 0 slides the pieces 8 apart a frame and fades
// them in (alpha + 0x33), 1 puts them in place (60 / 196, opaque), 2 slides them on, fading out.
// The puzzle area's script 0 runs it: 5 frames of 0, 30 of 1, 5 of 2, then flag 0 (flag 1 picks
// the second piece: the start (sound 0x43) or the other banner (sound 0x45))
void stepBanner(App& app) {
    App::PuzzleBanner& b = app.puzzleBanner;
    if (!b.on) return;
    if (app.game.flag(0)) { b.on = false; return; }
    const int v = app.game.vars[0];
    if (v == 0) b.x1 -= 8, b.x2 += 8, b.alpha = std::min(255, b.alpha + 0x33);
    else if (v == 1) b.x1 = 60, b.x2 = 196, b.alpha = 255;
    else if (v == 2) b.x1 -= 8, b.x2 += 8, b.alpha = std::max(0, b.alpha - 0x33);
}

// Uty_WarpCrystalCamera_Proc: the view slides to the crystal (0.25 a frame); there it comes in
// (fade, sound 0x2a) and var 0 = 1; once the script sets 2 the view slides back to the leader,
// var 0 = 3 and the camera is the leader's again
void stepCrystalCam(App& app) {
    App::CrystalCam& cc = app.crystalCam;
    if (cc.npc < 0) return;
    const cr::FieldNpc* n = nullptr;
    int index = -1;
    for (size_t i = 0; i < app.npcs.size(); ++i)
        if (app.npcs[i].id == cc.npc) n = &app.npcs[i], index = int(i);
    if (!n) { cc.npc = -1; app.cam.free = false; return; }
    if (cc.phase == 0 && app.game.vars[0] == 2) cc.phase = 1; // (20884-20954: aimed at the leader the frame var 0 is 2)
    const cr::Vec3 at = cc.phase == 0 ? cr::Vec3{n->pos[0], n->pos[1], n->pos[2]}
                                      : app.walker.pos + cr::Vec3{0, 7.0f + float(app.stageCam.lookHeight), 0};
    const float ox = app.scroll.offX, oy = app.scroll.offY;
    float vx, vy;
    if (scrollingField(app) && fieldViewPoint(app, at, vx, vy)) scrollFieldTo(app, vx, vy, 0.25f);
    // (Uty_WarpCrystalCamera_Proc 20889-20950: on once the scroll moves 0.01 or less a frame - GetScroll's
    // units, the record's pixels: x 2 x the view's scale in this scroll's)
    const float still = 0.01f * 2.0f * fieldViewScale(app);
    const bool settled = std::fabs(app.scroll.offX - ox) <= still && std::fabs(app.scroll.offY - oy) <= still;
    if (!settled) return;
    if (cc.phase == 0 && app.game.vars[0] == 0) { // there: the crystal comes
        // (_Uty_WarpCrystalCamera_Proc 0x25d18: any NPC call 2014 named - npc_born, mdl_set_alpha 0 -> 0xff over
        // 4 frames, eff3d_set_fld 4, SE 0x2a; a puzzle object's own state as well; j001_d05's 5 / 17 / 25 aren't)
        bool object = false;
        for (App::Puzzle::Obj& o : app.puzzle.objs)
            if (o.npc == index) show(app, o, true), object = true;
        if (!object) ViewerHost(app).showNpc(cc.npc, true);
        npcEffect(app, 4, index);
        fade(app, index, 0, 1, 4);
        app.sound.play(0x2a);
        app.game.vars[0] = 1;
    } else if (cc.phase == 1) {
        app.game.vars[0] = 3;
        cc.npc = -1;
        app.cam.free = false; // SetPlayerCamera
    }
}

// A touch going down (the frame's pixels; true when the puzzle took it): the pad's buttons -
// (8..52, 56..84) Retry (EvrStart 6), (84..124, 14..46) Help (7) - then the ability: psychokinesis takes hold of the block there at once (onCheckTouch);
// the others' CheckTap follows the touch from here, a tap once it's let go in time
bool click(App& app, float px, float py) {
    App::Puzzle& pz = app.puzzle;
    if (pz.stage < 0 || app.runner.running() || !app.dialogue.empty() || !paintedField(app)) return false;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    float tx, ty;
    logicalToTex2d(app, lx, ly, tx, ty);
    const float bx = tx + (app.renderer.uiX() - wideLeft(app)) / 2; // (widescreen: the pad in the corner)
    if (bx >= 8 && bx < 52 && ty >= 56 && ty < 84 && ask(app, 0)) return true;
    if (bx >= 84 && bx < 124 && ty >= 14 && ty < 46 && ask(app, 1)) return true;
    if (cleared(app) || pz.scene != 4 || pz.scroll.state == 1) return false;
    float ox = 0, oy = 0;
    const int t = pick(app, tx, ty, &ox, &oy);
    if (t < 0) return false;
    if (stage(app)->ability == cr::kAbilityPsychokinesis) {
        int r, c; // (onCheckTouch: the leader in a cell, not moving, the pad not held)
        if (pz.leaderMoved || pz.padHold > 0 || !cellOf(app, app.walker.pos, r, c)) return false;
        pz.tapX = tx, pz.tapY = ty;
        pkGrab(app, t, tx, ty, ox, oy);
        pz.tapX = pz.tapY = -1;
        return true;
    }
    pz.tap = App::Puzzle::Tap(); // CheckTap state 1: this touch, from here
    pz.tap.state = 1, pz.tap.x = tx, pz.tap.y = ty;
    return true;
}

// The held touch moved (the frame's pixels): where the detector sees it
void drag(App& app, float px, float py) {
    App::Puzzle& pz = app.puzzle;
    if (pz.stage < 0 || !pz.pkHeld) return;
    float lx, ly;
    app.renderer.toLogical(px, py, lx, ly);
    logicalToTex2d(app, lx, ly, pz.pkAtX, pz.pkAtY);
}

// The touch let go (the frame's pixels): CheckTap's end, the detector's let-go
void release(App& app, float px, float py) {
    App::Puzzle& pz = app.puzzle;
    if (pz.stage < 0) return;
    if (pz.tap.state == 1) pz.tap.released = true;
    if (pz.pkHeld) {
        drag(app, px, py);
        pz.pkHeld = false;
    }
}

// The ability's key: teleport to / take hold of the block ahead, or let a held one go
void abilityKey(App& app) {
    App::Puzzle& pz = app.puzzle;
    const cr::PuzzleStage* st = stage(app);
    if (!st) return;
    if (st->ability == cr::kAbilityPsychokinesis) {
        if (pz.pkState == 1) pkSend(app, 0);
        else pkGrab(app, ahead(app));
    } else if (st->ability == cr::kAbilityToughness) toughTap(app, ahead(app));
    else if (st->ability == cr::kAbilityPyrokinesis) pyroTap(app, ahead(app));
    else teleportTo(app, ahead(app));
}


// CandleSwitch::draw3D: the beam from a switch that passed a light on (message 11) - 1.5 wide,
// colour 0x600000ff, its head reaching the switch in 4 frames, its tail following from frame 8 to 12
void drawFx(App& app, const cr::Mat4& mvp) {
    App::Puzzle& pz = app.puzzle;
    const cr::PuzzleStage* st = stage(app);
    if (!st || st->ability != cr::kAbilityPyrokinesis) return;
    constexpr int kQuads = 64;
    auto make = [&](cr::GpuMesh& m) {
        if (m.vao) return;
        cr::Mesh mesh;
        mesh.positions.assign(kQuads * 4 * 3, 0.0f);
        mesh.uvs.assign(kQuads * 4 * 2, 0.0f);
        mesh.colors.assign(kQuads * 4 * 4, 0);
        for (uint32_t q = 0; q < kQuads; ++q)
            for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(q * 4 + k);
        m = app.renderer.upload(mesh);
    };
    make(app.puzzleBeams);
    struct Quads {
        std::vector<float> pos, uv;
        std::vector<uint8_t> col;
        int n = 0;
        void add(const cr::Vec3 (&v)[4], const uint8_t (&c)[4][4]) {
            if (n >= kQuads) return;
            for (int k = 0; k < 4; ++k) {
                pos.insert(pos.end(), {v[k].x, v[k].y, v[k].z});
                col.insert(col.end(), c[k], c[k] + 4);
            }
            ++n;
        }
        void finish() {
            pos.resize(kQuads * 4 * 3, 0.0f);
            col.resize(kQuads * 4 * 4, 0);
            uv.assign(kQuads * 4 * 2, 0.0f);
        }
    } beams;
    for (const App::Puzzle::Obj& o : pz.objs) {
        if (o.type != cr::kTypeCandleSwitch || !o.shown) continue;
        const cr::FieldNpc& n = app.npcs[size_t(o.npc)];
        if (o.csFrom >= 0 && o.beamT <= 20) {
            const cr::FieldNpc& f = app.npcs[size_t(o.csFrom)];
            const float dx = n.pos[0] - f.pos[0], dz = n.pos[2] - f.pos[2], dist = std::sqrt(dx * dx + dz * dz);
            const float head = std::clamp(float(o.beamT) * 2.5f, 0.0f, 10.0f) / 10.0f * dist;
            const float tail = std::clamp(float(o.beamT) * 2.5f - 20.0f, 0.0f, 10.0f) / 10.0f * dist;
            if (head > tail + 0.01f) {
                const float ang = std::atan2(dx, dz), cs = std::cos(ang), sn = std::sin(ang), y = n.pos[1] + 0.3f;
                auto at = [&](float lx, float lz) { return cr::Vec3{f.pos[0] + lx * cs + lz * sn, y, f.pos[2] + lz * cs - lx * sn}; };
                const cr::Vec3 v[4] = {at(-0.75f, tail), at(0.75f, tail), at(0.75f, head), at(-0.75f, head)};
                const uint8_t c[4][4] = {{255, 0, 0, 0x60}, {255, 0, 0, 0x60}, {255, 0, 0, 0x60}, {255, 0, 0, 0x60}};
                beams.add(v, c);
            }
        }
    }
    beams.finish();
    app.renderer.updatePositions(app.puzzleBeams, beams.pos, beams.uv, beams.col);
    cr::Material m; // untextured, double-sided, depth tested, no depth write
    m.culling = 2; // (none)
    m.properties = 8;
    m.blendSrc = 4, m.blendDst = 5; // alpha
    if (beams.n) app.renderer.drawMesh(app.puzzleBeams, nullptr, mvp, m);
}

} // namespace puzzle

// mdl_put (2.2.0 10202-10212, outside battles): each field frame a model's drawn yaw turns toward its
// actor's yaw (copied each frame, 35339-35344) by at most 1/16 turn (FUN_00013734, step 0x3d800000), the
// short way round - a SetDir about-face takes 8 frames. Runs after the frame's script, as mdl_put does.
static void stepNpcTurns(App& app) {
    if (app.battle.active) return;
    constexpr float kTurn = 6.28318531f, kStep = kTurn / 16.0f;
    for (cr::FieldNpc& n : app.npcs) {
        if (!n.shownSet) { n.shownAngle = n.angle, n.shownSet = true; continue; }
        const float d = std::remainder(n.angle - n.shownAngle, kTurn);
        if (std::fabs(d) < 1e-6f) { n.shownAngle = n.angle; continue; }
        n.shownAngle = std::fabs(d) <= kStep ? n.angle : n.shownAngle + (d > 0 ? kStep : -kStep);
        for (auto& dr : app.npcDraws)
            if (dr.id == n.id) dr.world = npcWorld(n);
    }
}

void stepEvents(App& app) {
    ++app.mdlFrame; // (gac+0x3c6a8: the main loop's count, _mdl_uv's clock)
    struct TurnAfter { App& a; ~TurnAfter() { stepNpcTurns(a); } } turnAfter{app}; // (every way out of the frame)
    // (touch_up_chk / _pad_chk_trg_up 14519: a tap counts in the frame it came - FUN_0001d044 (14607) clears the
    // touches at the frame's end; a tap no Call 2001 read that frame is gone, not held for the next)
    struct TapGone { App& a; ~TapGone() { a.tapPending = false; } } tapGone{app};
    if (app.npcNativesPending) { // npc_setup: SetVar(0, id); UtyPrcTbl[row + 8 - 1].init() for every created NPC with one
        app.npcNativesPending = false;
        ViewerHost natives(app);
        for (const cr::FieldNpc& n : app.npcs)
            if (n.native == 15 && npcActive(app, n)) app.game.vars[0] = n.id, natives.fireWall(n.id); // (15, Uty_FireWall_Init: the one handled)
    }
    // fast-forward while the skip is held (_PrcMsg: the corner _pad_chk_circle(460, 16, 20) as a tap
    // every frame): the typing out at once and each page turned the frame it is out (choices still
    // wait for an answer; pauses and line breaks still take their frames)
    const bool skipping = app.skipScene && app.runner.running() && !app.dialogue.empty();
    if (skipping) hurryDialogue(app);
    stepTyping(app);
    if (skipping && !app.dialogue.empty() && pageTyped(app) && !(pageInfo(app) && pageInfo(app)->closeAfter >= 0))
        advanceDialogue(app);
    if (app.titleOpen) ++app.titleAnim; // the title menu fades in
    stepResume(app); // the title's Progress Tracker offer: its message, windows, and the load / Start after the answer
    if (app.titleReturnIn > 0 && --app.titleReturnIn == 0) { app.optionsOpen = false; bootTitle(app); } // Options' Return to Title, after its black
    if (app.choiceFlag >= 0 && app.choiceAnim < 4) ++app.choiceAnim;
    updateVoices(app);
    if (!app.battle.active) stepEffects(app, app.fieldEffects), stepQuake(app.fieldQuake); // eff3d_prc, UpdateQuake
    if (app.nowLoading.on) app.nowLoading.frame = (app.nowLoading.frame + 1) & 15; // NowLoadProc
    stepTexDisp(app);
    stepVitaLogo(app);
    stepTutoTex(app);
    {
        const bool opening = app.msgWin.state == 2;
        stepMsgWin(app);
        // (PrcWin then PrcMsg in one frame: the first character comes the frame the window is up)
        if (opening && app.msgWin.state == 3) stepTyping(app);
    }
    if (app.fadeT < app.fadeFrames) ++app.fadeT;
    if (app.backFade.t < app.backFade.frames) ++app.backFade.t;
    bool unlit = false; // (only the drawn models' fades step: mdl_put's)
    for (auto& d : app.npcDraws)
        if (d.fadeLeft > 0) {
            --d.fadeLeft;
            d.alpha = d.alphaTo + (d.alphaFrom - d.alphaTo) * float(d.fadeLeft) / float(d.fadeFrames);
            if (d.fadeLeft == 0 && d.lit && d.alpha <= 0) d.lit = false, unlit = true; // (+0x55c 0 again: 10189)
        }
    if (unlit) buildNpcDraws(app);
    if (app.playerFadeLeft > 0) {
        --app.playerFadeLeft;
        app.playerAlpha = app.playerAlphaTo + (app.playerAlphaFrom - app.playerAlphaTo) * float(app.playerFadeLeft) /
                                                  float(std::max(1, app.playerFadeFrames));
    }
    if (app.puzzleStartPending && app.havePlayer)
        app.puzzleStart = app.walker.pos, app.puzzleStartHeading = app.walker.heading, app.puzzleStartPending = false;
    if (app.padLock > 0) --app.padLock; // (the main frame's: gac + 0x3cde6 counts down before _gameapp_prc: 2.2.0 0x43d4)
    puzzle::step(app);
    puzzle::stepCrystalCam(app);
    puzzle::stepBanner(app);
    // Script moves (npc_autoctl_prc): straight lines, y on the floor unless mode 2. (The original's
    // frame runs the script first, then Field_Main's steps: a move, scale, scroll or camera slide a
    // script starts takes its first step on that same frame - the starters step once themselves)
    for (size_t i = 0; i < app.moves.size();) {
        if (stepMove(app, app.moves[i])) app.moves.erase(app.moves.begin() + long(i));
        else ++i;
    }
    for (size_t i = 0; i < app.rescales.size();) { // (npc_autoctl_prc mode 3: the scale in a straight line)
        if (stepRescale(app, app.rescales[i])) app.rescales.erase(app.rescales.begin() + long(i));
        else ++i;
    }
    stepFieldScroll(app);
    stepSceneEffects(app);
    stepScriptCam(app);
    for (App::Portrait& p : app.portraits) // Tex2d slides and fades (tex2d_img_prc)
    {
        stepPortraitBlend(p);
        if (p.dying && p.t >= p.frames && p.aT >= p.aFrames) {
            app.renderer.destroy(p.tex);
            p = App::Portrait();
        }
        if (p.zoomT < p.zoomFrames) {
            ++p.zoomT;
            p.scale = 2.0f - float(p.zoomT) / float(p.zoomFrames);
        }
    }
    // The item band (_Uty_GetItem_Proc): down from the top in two frames; once the script raises
    // flag 0 (after the tap) it slides on down and fades for five frames more, then clears flag 0
    if (app.itemWindowOpen) {
        App::ItemWin& w = app.itemWin;
        if (app.game.flag(0)) {
            if (w.slide > 6) { app.itemWindowOpen = false; app.game.setFlag(0, false); }
            else ++w.slide, ++w.fade;
        } else if (w.slide <= 1) ++w.slide;
    }
    if (!app.runner.running() && app.pendingEntry != App::Entry::None) {
        const bool area = app.pendingEntry == App::Entry::Area;
        app.pendingEntry = App::Entry::None;
        const bool started = area ? app.runner.start(app.events, 0, app.game, app.area, app.mapNumber)
                                  : app.runner.startAt(app.events, app.events.returnEntry(5), 0, app.game, app.area,
                                                       app.mapNumber);
        if (started) {
            app.runnerWhat = area ? "area entry script" : "room entry script";
            SDL_Log("%s: area %d map %d", app.runnerWhat.c_str(), app.area, app.mapNumber);
        }
    }
    if (!app.runner.running()) return;
    ViewerHost host(app);
    const bool more = app.runner.step(&host);
    if (app.battle.requested) { // G+0x2c0 bit 7: the field gives way to the battle
        app.battle.requested = false;
        // btl_main: a script still running (G+0x2c0 bit 0) makes it an event battle (G+0xa4e) - the
        // script waits behind the battle's gate and goes on later; else nothing's left of it
        const bool event = more && app.runner.running();
        if (!event) app.runner.stop();
        startBattle(app, app.battle.request, app.battle.requestFlags);
        app.battle.eventBattle = event && app.battle.active;
        return;
    }
    if (more) return;
    // Finished: report, and refresh what the script's flags changed (an opened box loses its lid)
    trackerSceneEnd(app);
    checkTrophies(app); // (the Vita's __EvtReturn / __EvtAbort: FUN_8106ae3a - 0x8102d416 / 0x8102d528)
    const cr::EventResult& r = app.runner.result();
    if (!r.error.empty())
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "event (%s, area %d map %d): %s", app.runnerWhat.c_str(), app.area,
                    app.mapNumber, r.error.c_str());
    if (!app.runnerWhat.empty()) {
        app.lastEvent = app.runnerWhat;
        for (int item : r.itemsGained) {
            std::vector<std::vector<std::string>> name;
            app.lastEvent += ", got " + itemName(app, item);
        }
        SDL_Log("%s, %d instructions, %zu messages", app.lastEvent.c_str(), r.instructions, r.messages.size());
    }
    app.itemWindowOpen = false;
    app.tapPending = false;
    buildNpcDraws(app);
    updateTitle(app);
}

// fldencount_walk, for a step of `dist` units: the encounter distance counts unless encounters
// are off (flag 4076, the pair's unencountflg); with skill 174 on either member every 10 units
// banks 1 EXP for the pair (to 100000; one a step at most)
void fieldWalked(App& app, float dist) {
    if (!app.noEncounters && !fieldEncountersOff(app)) app.encounters.walked(dist);
    const int pr = fieldRoute(app) * 2;
    if (!cr::hasSkill(app.game, pr, 174) && !cr::hasSkill(app.game, pr + 1, 174)) return;
    app.walkExpDistance += dist;
    if (app.walkExpDistance >= 10) {
        int& bank = app.game.walkExp[size_t(fieldRoute(app))];
        bank = std::min(100000, bank + 1);
        app.walkExpDistance -= 10;
    }
}

void playFootsteps(App& app) {
    // PlayFootstepSE: a step sound when the walk / run animation passes one of its two step
    // frames, chosen (one of three) by the colour of the floor polygon underfoot, and effect 0 (dust)
    if (app.walker.gait != cr::Gait::Stand) {
        const int route = std::clamp(app.playerChar / 2, 0, 3);
        const short* frames = app.walker.gait == cr::Gait::Run ? cr::kFootstepFrameRun[route] : cr::kFootstepFrameWalk[route];
        const float prev = app.footPrev, now = app.player.time;
        for (int k = 0; k < 2; ++k) {
            const float f = frames[k];
            const bool crossed = prev <= now ? (prev < f && f <= now) : (f > prev || f <= now); // wrapped
            if (!crossed) continue;
            float y;
            int poly = -1;
            if (!cr::floorHeight(app.collisionData, app.walker.pos.x, app.walker.pos.y + 1, app.walker.pos.z, 2.0f, y, &poly))
                break;
            static const uint8_t kSurface[5][3] = {{254, 0, 0}, {175, 175, 175}, {125, 185, 175}, {205, 220, 230}, {140, 190, 190}};
            const uint8_t* c = app.collisionData.polygons[size_t(poly)].color;
            for (int t = 0; t < 5; ++t)
                if (c[0] == kSurface[t][0] && c[1] == kSurface[t][1] && c[2] == kSurface[t][2])
                {
                    npcEffect(app, 0, -1); // the dust (eff3d_set_fld 0 on the leader)
                    const uint32_t id = uint32_t(cr::kFootstepSound[t][SDL_rand(3)]);
                    app.sound.play(id);
                    app.soundLog.push_back(id);
                }
        }
    }
    app.footPrev = app.player.time;
}

// WASD moves the leader relative to the camera (camYaw), as the original's cross key does.
// A field step's start and end (App::Between): what the step moves, recorded on either side of it
void betweenRecord(App& app, int side) {
    App::Between& b = app.between;
    b.walker[side] = app.walker.pos, b.heading[side] = drawnHeading(app), b.playerAlpha[side] = app.playerAlpha;
    b.eye[side] = app.cam.eye, b.target[side] = app.cam.target;
    b.scroll[side][0] = app.scroll.offX, b.scroll[side][1] = app.scroll.offY;
    if (side == 0) {
        b.npcs.clear();
        for (const App::NpcDraw& d : app.npcDraws) {
            App::Between::Npc n;
            n.index = d.index, n.id = d.id, n.world[0] = n.world[1] = d.world, n.alpha[0] = n.alpha[1] = d.alpha;
            b.npcs.push_back(n);
        }
        b.valid = false;
        return;
    }
    // (the NPCs the step began with that are still drawn: matched by their place and id)
    std::vector<App::Between::Npc> kept;
    for (App::Between::Npc n : b.npcs)
        for (const App::NpcDraw& d : app.npcDraws)
            if (d.index == n.index && d.id == n.id) {
                n.world[1] = d.world, n.alpha[1] = d.alpha;
                kept.push_back(n);
                break;
            }
    b.npcs.swap(kept);
    b.valid = true;
}

struct BetweenStep { // betweenRecord on either side of a step, however the step's body leaves
    App& app;
    explicit BetweenStep(App& a) : app(a) { betweenRecord(app, 0); }
    ~BetweenStep() { betweenRecord(app, 1); }
};

bool same(const cr::Vec3& a, const cr::Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool same(const cr::Mat4& a, const cr::Mat4& b) { return std::memcmp(a.m, b.m, sizeof a.m) == 0; }

// how far between the last step's start and end a frame shows (1: no step to show between)
float betweenK(const App& app) { return app.between.valid ? std::clamp(app.between.alpha, 0.0f, 1.0f) : 1.0f; }

cr::Vec3 lerp(const cr::Vec3& a, const cr::Vec3& b, float k) { return a + (b - a) * k; }
float lerpAngle(float a, float b, float k) {
    float d = std::fmod(b - a, 6.2831853f);
    if (d > 3.14159265f) d -= 6.2831853f;
    if (d < -3.14159265f) d += 6.2831853f;
    return a + d * k;
}

// The leader as a frame between steps shows it (a step's warp - further than it can walk - is
// shown at once)
float drawnHeading(const App& app) { return app.leaderYawSet ? app.leaderYaw : app.walker.heading; }

// mdl_put's turn for the leader (slot 1: +0xd7c the heading FieldPCCtrl::operate writes, +0xd90 the drawn
// yaw nothing copies - 40810-40820): 1/16 turn a field frame the short way round, after the frame's move
static void stepLeaderTurn(App& app) {
    if (app.battle.active || !app.havePlayer) return;
    constexpr float kTurn = 6.28318531f, kStep = kTurn / 16.0f;
    if (!app.leaderYawSet) { app.leaderYaw = app.walker.heading, app.leaderYawSet = true; return; }
    const float d = std::remainder(app.walker.heading - app.leaderYaw, kTurn);
    app.leaderYaw = std::fabs(d) <= kStep ? app.walker.heading : app.leaderYaw + (d > 0 ? kStep : -kStep);
}

cr::FieldWalker shownWalker(const App& app) {
    cr::FieldWalker w = app.walker;
    w.heading = drawnHeading(app);
    const App::Between& b = app.between;
    if (!b.valid || !same(w.pos, b.walker[1]) || w.heading != b.heading[1]) return w;
    const cr::Vec3 d = b.walker[1] - b.walker[0];
    if (cr::dot(d, d) > 40.0f * 40.0f) return w;
    const float k = betweenK(app);
    w.pos = lerp(b.walker[0], b.walker[1], k);
    w.heading = lerpAngle(b.heading[0], b.heading[1], k);
    return w;
}

float shownPlayerAlpha(const App& app) {
    const App::Between& b = app.between;
    if (!b.valid || app.playerAlpha != b.playerAlpha[1]) return app.playerAlpha;
    return b.playerAlpha[0] + (b.playerAlpha[1] - b.playerAlpha[0]) * betweenK(app);
}

// An NPC's model matrix and opacity as a frame between steps shows them (a turn of more than
// about 45 degrees or a jump in one step is shown at once)
void shownNpc(const App& app, const App::NpcDraw& d, cr::Mat4& world, float& alpha) {
    world = d.world, alpha = d.alpha;
    for (const App::Between::Npc& n : app.between.npcs) {
        if (!app.between.valid || n.index != d.index || n.id != d.id) continue;
        const float k = betweenK(app);
        if (d.alpha == n.alpha[1]) alpha = n.alpha[0] + (n.alpha[1] - n.alpha[0]) * k;
        if (!same(d.world, n.world[1])) return;
        const float* a = n.world[0].m, *c = n.world[1].m;
        const float turn = (a[0] * c[0] + a[1] * c[1] + a[2] * c[2]) /
                           std::max(std::sqrt((a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) * (c[0] * c[0] + c[1] * c[1] + c[2] * c[2])), 1e-6f);
        const float jx = c[12] - a[12], jy = c[13] - a[13], jz = c[14] - a[14];
        if (turn < 0.7f || jx * jx + jy * jy + jz * jz > 40.0f * 40.0f) return;
        for (int i = 0; i < 16; ++i) world.m[i] = a[i] + (c[i] - a[i]) * k;
        return;
    }
}

// The script's free camera as a frame between steps shows it
void shownCamera(const App& app, cr::Vec3& eye, cr::Vec3& target) {
    eye = app.cam.eye, target = app.cam.target;
    const App::Between& b = app.between;
    if (!b.valid || !same(eye, b.eye[1]) || !same(target, b.target[1])) return;
    const float k = betweenK(app);
    eye = lerp(b.eye[0], b.eye[1], k), target = lerp(b.target[0], b.target[1], k);
}

// The painted field's scroll as a frame between steps shows it (a script's slide; otherwise it
// follows the leader as shown)
void shownScroll(const App& app, float& x, float& y) {
    x = app.scroll.offX, y = app.scroll.offY;
    const App::Between& b = app.between;
    if (!b.valid || !app.cam.free || x != b.scroll[1][0] || y != b.scroll[1][1]) return;
    const float k = betweenK(app);
    x = b.scroll[0][0] + (b.scroll[1][0] - b.scroll[0][0]) * k;
    y = b.scroll[0][1] + (b.scroll[1][1] - b.scroll[0][1]) * k;
}

void updatePlayer(App& app, float dt, float camYaw) {
    if (!app.havePlayer) return;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    // A running script, its dialogue box or item window hold the player in place
    const bool talking = fieldSceneRunning(app) || app.optionsOpen || app.campOpen || app.saveScreen.open ||
                         app.shop.open || app.mapScreen.open ||
                         app.puzzle.tpState != 0 || app.puzzle.pkState != 0 ||
                         (app.puzzle.stage >= 0 && app.puzzle.scene != 4);
    if (talking) app.walker.gait = cr::Gait::Stand; // scripts skip stepWalker, so stop the animation here too
    float sx = talking ? 0.0f : float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]);
    float sy = talking ? 0.0f : float(keys[SDL_SCANCODE_S]) - float(keys[SDL_SCANCODE_W]);
    bool run = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    // A controller as the Vita's CrossKey::update (81034fcc): each stick axis zeroed below 0.2549 (the
    // pad reader, FUN_8106bafc: +0xc 0x3e828284), then x 50 and held to +-50; with the stick at rest
    // the D-pad sets its axes to +-50 - a full push. The length walks from 14.5 and runs from 29
    // (squared 210.25 / 841): the stick walks past about 0.29 and runs past 0.58, the D-pad always
    // runs. The field bar open (FUN_81036ed0, gac + 0x38600) the leader stands
    if (padLive(app) && !talking && sx == 0 && sy == 0 && !(fieldBarPad(app) && app.fieldMenu.open)) {
        auto axis = [&](SDL_GamepadAxis a) {
            const float v = float(SDL_GetGamepadAxis(app.pad, a)) / 32767.0f;
            return std::fabs(v) < 0.2549f ? 0.0f : std::clamp(v * 50.0f, -50.0f, 50.0f);
        };
        float lx = axis(SDL_GAMEPAD_AXIS_LEFTX), ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
        if (lx == 0 && ly == 0) {
            lx = 50.0f * (float(SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) - float(SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)));
            ly = 50.0f * (float(SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) - float(SDL_GetGamepadButton(app.pad, SDL_GAMEPAD_BUTTON_DPAD_UP)));
        }
        const float len2 = lx * lx + ly * ly;
        if (len2 >= 210.25f) {
            run = run || len2 >= 841.0f;
            sx = std::clamp(lx / 50.0f, -1.0f, 1.0f), sy = std::clamp(ly / 50.0f, -1.0f, 1.0f);
        }
    }
    if (!talking && sx == 0 && sy == 0) { // the touch pad (CrossKey): its offset steers, far out runs
        float tx, ty;
        bool trun;
        if (touchPadStick(app, tx, ty, trun)) sx = tx, sy = ty, run = trun;
    }
    if (const char* w = SDL_getenv("CR_CAPTURE_WALK"); w && !talking && sx == 0 && sy == 0) // (tests: x,y held - a --settle-moving walk)
        if (SDL_sscanf(w, "%f,%f", &sx, &sy) != 2) sx = sy = 0;
    constexpr float kStep = 1.0f / kFramesPerSecond;
    app.walkClock += dt;
    while (app.walkClock >= kStep) {
        app.walkClock -= kStep;
        BetweenStep between(app);
        struct LeaderTurn { App& a; ~LeaderTurn() { stepLeaderTurn(a); } } leaderTurn{app}; // (after the move, before the step's record)
        stepEvents(app);
        if (app.battle.active) break;
        if (!app.havePlayer) continue;
        // FieldScene::onUpdate: FieldPCCtrl's update and checkHit (2.2.0 0x4272c) every frame - a script
        // holding the field (G+0x2c0 bit 0) stops the stick, not the floor; the pushes wait for its end
        const std::vector<cr::WalkBody> bodies = walkBodies(app);
        if (fieldSceneRunning(app)) { // (a touch trigger may start a script this step)
            stepExamineWindows(app); // (the windows count down, none put up)
            app.walker.gait = cr::Gait::Stand;
            app.walker.velocity = {};
            cr::checkWalkerHit(app.walker, app.collisionData, bodies, !app.runner.running());
            continue;
        }
        const float walkedDist = cr::stepWalker(app.walker, app.collisionData, bodies, sx, sy, run, camYaw, !app.runner.running());
        if (sx == 0 && sy == 0) // FieldPCCtrl::operate's state 3 (FollowWindow::update's receiveMessage 3): standing in reach,
            if (const cr::FieldNpc* n = interactTarget(app)) { // the leader turns to the target by 0.35 of the angle left each frame
                // (from the drawn yaw, both written: 2.2.0 0x433a0-0x43418)
                const float cx = n->boxCenterX(), cz = n->boxCenterZ(), from = drawnHeading(app);
                float d = std::atan2(cx - app.walker.pos.x, cz - app.walker.pos.z) - from;
                while (d > 3.14159265f) d -= 2 * 3.14159265f;
                while (d < -3.14159265f) d += 2 * 3.14159265f;
                app.walker.heading = app.leaderYaw = from + 0.35f * d;
                app.leaderYawSet = true;
            }
        if (walkedDist >= 0) fieldWalked(app, walkedDist); // fldencount_walk(|velocity + pushes|)
        stepExamineWindows(app);
        checkTriggers(app);
        if (fieldSceneRunning(app)) app.walker.gait = cr::Gait::Stand;
        if (fieldSceneRunning(app) || !app.havePlayer) continue;
        cr::BattleStart start; // fldencount_encount
        auto random = [&](int n) { return app.rng.get(n); };
        const int pr = std::clamp(app.playerChar / 2, 0, 3) * 2; // 171 Preemptive / 172 Alert on either member
        const bool first = cr::hasSkill(app.game, pr, 171) || cr::hasSkill(app.game, pr + 1, 171);
        const bool alert = cr::hasSkill(app.game, pr, 172) || cr::hasSkill(app.game, pr + 1, 172);
        if (app.encounters.check(random, partyLevel(app), first, alert, start)) {
            writeResume(app); // (fldencount_encount: the Progress Tracker at the fight's start)
            app.game.vars[58] = 999; // the usual battle music, victory tune 10
            app.game.vars[59] = 10;
            app.game.vars[60] = 999;
            startBattle(app, start, 0);
            break;
        }
    }
    app.between.alpha = app.walkClock / kStep; // (how far into the next step this frame is)
    // Actions 0/1/2 = stand / walk / run (mdl_set_act_motion in FieldPCCtrl::operate); while a
    // block is held, slot 3's frames 2..3 looped (Psychokinesis::onCheckTouch's
    // mdl_set_act_motion_loop(leader, 3, 2, 2, 4) - 2.2.0 0x43d80..0x43da8: loop start 2, end 4, the end
    // not shown: mdl_actctl_prc goes back to the start when the next frame reaches it)
    // A scene's Anim on npc 0 holds the leader's action while the scene holds the field
    const bool hold = app.puzzle.pkState != 0 && app.player.actions.size() > 3;
    App::PlayerAnim& pa = app.playerAnim;
    if (pa.on && !fieldSceneRunning(app)) pa.on = false;
    const bool scripted = pa.on && !hold && pa.action >= 0 && pa.action < int(app.player.actions.size());
    const int want = hold ? 3 : scripted ? pa.action : std::min(int(app.walker.gait), int(app.player.actions.size()) - 1);
    if (want != app.player.action) {
        app.player.action = want;
        app.player.time = 0;
        app.footPrev = 0;
    }
    app.player.loopFrom = hold ? 2 : scripted ? pa.loopFrom : -1;
    app.player.loopTo = hold ? 4 : scripted ? pa.loopTo : 0;
    app.player.once = scripted && pa.once;
    animate(app, app.player, dt);
    playFootsteps(app);
}

cr::Mat4 playerWorld(const cr::FieldWalker& w) {
    cr::Mat4 m;
    const float c = std::cos(w.heading), s = std::sin(w.heading);
    m.m[0] = c; m.m[2] = -s; m.m[8] = s; m.m[10] = c;
    m.m[12] = w.pos.x; m.m[13] = w.pos.y; m.m[14] = w.pos.z;
    return m;
}

// The map's visible NPCs (npc_setup): model from the NPC table, position (plus the offset
// for models 0/1), turned by the record's angle about Y, scaled by the table's percentage.
// An NPC's model matrix: position (plus the offset for models 0/1), turned about Y, scaled
cr::Mat4 npcWorld(const cr::FieldNpc& n) {
    const float k = n.model == 0 || n.model == 1 ? 1.0f : 0.0f;
    const float yaw = n.shownSet ? n.shownAngle : n.angle;
    const bool set = n.axes[0] >= 0;
    const float sx = set ? n.axes[0] : n.scale, sy = set ? n.axes[1] : n.scale, sz = set ? n.axes[2] : n.scale;
    // (mdl_put 10214-10215: mceTransform_setRotateEuler (x, yaw, z) - R = Rz Ry Rx, as a battle effect's)
    const float cx = std::cos(n.tilt[0]), snx = std::sin(n.tilt[0]), cy = std::cos(yaw), sy0 = std::sin(yaw);
    const float cz = std::cos(n.tilt[1]), snz = std::sin(n.tilt[1]);
    cr::Mat4 w;
    w.m[0] = cy * cz * sx, w.m[1] = cy * snz * sx, w.m[2] = -sy0 * sx;
    w.m[4] = (snx * sy0 * cz - cx * snz) * sy, w.m[5] = (snx * sy0 * snz + cx * cz) * sy, w.m[6] = snx * cy * sy;
    w.m[8] = (cx * sy0 * cz + snx * snz) * sz, w.m[9] = (cx * sy0 * snz - snx * cz) * sz, w.m[10] = cx * cy * sz;
    w.m[12] = n.pos[0] + k * n.offset[0];
    w.m[13] = n.pos[1] + k * n.offset[1];
    w.m[14] = n.pos[2] + k * n.offset[2];
    return w;
}

cr::FieldNpc* findNpc(App& app, int id) {
    for (cr::FieldNpc& n : app.npcs)
        if (n.id == id) return &n;
    return nullptr;
}

// Where a script actor is: npc 0 is the party leader
bool actorPos(App& app, int id, cr::Vec3& out) {
    if (id == 0) { if (!app.havePlayer) return false; out = app.walker.pos; return true; }
    const cr::FieldNpc* n = findNpc(app, id);
    if (!n) return false;
    out = {n->pos[0], n->pos[1], n->pos[2]};
    return true;
}

// Moves / turns a script actor and its drawn model (get_floor snaps y unless keepY)
void setActor(App& app, int id, cr::Vec3 p, const float* yaw, bool keepY) {
    float y;
    if (!keepY && (cr::floorHeight(app.collisionData, p.x, p.y, p.z, 1000.0f, y) ||
                   cr::floorHeight(app.collisionData, p.x, p.y + 1000.0f, p.z, 2000.0f, y)))
        p.y = y;
    if (id == 0) {
        app.walker.pos = p;
        if (yaw) app.walker.heading = *yaw;
        return;
    }
    cr::FieldNpc* n = findNpc(app, id);
    if (!n) return;
    const bool wasParked = !app.placedNpcs.count(id);
    app.placedNpcs.insert(id);
    n->pos[0] = p.x; n->pos[1] = p.y; n->pos[2] = p.z;
    if (yaw) n->angle = *yaw;
    bool drawn = false;
    for (auto& d : app.npcDraws)
        if (d.id == id) { d.world = npcWorld(*n); drawn = true; }
    if (wasParked || !drawn) buildNpcDraws(app); // a parked actor enters the scene
}

void buildNpcDraws(App& app) {
    std::vector<App::NpcDraw> old;
    old.swap(app.npcDraws);
    old.insert(old.end(), std::make_move_iterator(app.npcHeld.begin()), std::make_move_iterator(app.npcHeld.end()));
    app.npcHeld.clear();
    if (!app.showNpcs) { app.npcHeld = std::move(old); return; } // (kept for when they're shown again)
    for (size_t i = 0; i < app.npcs.size(); ++i) {
        const cr::FieldNpc& n = app.npcs[i];
        if (n.model < 0 || n.model >= cr::kNumNpcModels) continue;
        const std::string file = cr::kNpcModelFile[n.model];
        // (the same NPC: its model as it was left - action, time, blend, alpha and fade - shown or hidden)
        const App::NpcDraw* kept = nullptr;
        for (const App::NpcDraw& o : old)
            if (o.index == int(i) && o.model == file) { kept = &o; break; }
        bool drawn = npcDrawn(app, i) || (kept && kept->lit);
        // Cutscene actor slots wait parked at y 995..1000 or at the origin until a scene places
        // them (FrmSetc2) or explicitly shows them (Born). Not a model a script read in (NpcMdlRead):
        // that one stands at the origin as 2.2.0 reads it (19226-19253) - j007_t01's chequered hall floor
        if ((n.pos[1] >= 900.0f || (!n.mdlRead && n.pos[0] == 0 && n.pos[1] == 0 && n.pos[2] == 0)) && !app.placedNpcs.count(n.id))
            drawn = false;
        // (a hidden one whose model npc_setup didn't read - its create flags failed - and no script read: none)
        if (!drawn && !kept && !n.mdlRead && !npcCreateFlags(app, n)) continue;
        if (npcModel(app, file).meshes.empty()) continue;
        App::NpcDraw d;
        if (kept) d = *kept;
        d.model = file;
        d.world = npcWorld(n);
        d.id = n.id;
        d.index = int(i);
        (drawn ? app.npcDraws : app.npcHeld).push_back(d);
    }
}

// RectWrapper's NPCs, read live each frame (SetupRect / getFloorHeight take the npc ctl's position as
// it is): solid ones (kind bit 2 set, bit 4 clear) wall the leader in with their box's four sides,
// floor ones (bit 4) lift it onto their top; both only while shown (_ChkNpcDisp). The box turns by
// the record's angle (RectWrapper::init), the position is the record's plus, for models 0..1, its
// offset (npc_setup).
std::vector<cr::WalkBody> walkBodies(const App& app) {
    std::vector<cr::WalkBody> out;
    for (size_t idx = 0; idx < app.npcs.size(); ++idx) {
        const cr::FieldNpc& n = app.npcs[idx];
        cr::WalkBody b;
        b.wall = (n.kind & 2) && !(n.kind & 4);
        b.floor = (n.kind & 4) != 0;
        if (!b.wall && !b.floor) continue;
        b.shown = npcVisible(app, idx) && // (actors waiting off stage)
                  !(n.pos[1] >= 900.0f || (n.pos[0] == 0 && n.pos[1] == 0 && n.pos[2] == 0));
        const float k = n.model == 0 || n.model == 1 ? 1.0f : 0.0f;
        b.pos = {n.pos[0] + k * n.offset[0], n.pos[1] + k * n.offset[1], n.pos[2] + k * n.offset[2]};
        for (int c = 0; c < 3; ++c) b.offset[c] = n.offset[c], b.box[c] = n.box[c];
        b.angle = n.rectAngle;
        b.scriptModel = n.mdlRead;
        out.push_back(b);
    }
    return out;
}

// Each NPC instance has its own action and time: action 0 (idle) loops, others play once and
// hold their last frame (a chest lid stays open).
void animateNpcs(App& app, float dt) {
    for (auto& d : app.npcDraws) {
        const LoadedModel& lm = app.npcModels[d.model];
        if ((d.action < 0 || d.action >= int(lm.actions.size())) && !lm.packageActions.empty()) {
            float dur = 0; // only its materials move: they loop
            for (const auto& a : lm.packageActions) dur = std::max(dur, a.duration);
            d.time += dt * kFramesPerSecond;
            if (dur > 0) d.time = std::fmod(d.time, dur);
            continue;
        }
        if (d.action < 0 || d.action >= int(lm.actions.size())) continue;
        const float dur = lm.actions[d.action].duration;
        if (d.blend.step(d.action, d.time, dt * kFramesPerSecond, lm.actions)) continue; // (held at its first frame)
        d.time += dt * kFramesPerSecond;
        if (const char* tr = SDL_getenv("CR_NPC_TRACE"); tr && (SDL_atoi(tr) < 0 || d.id == SDL_atoi(tr))) // (debugging: an NPC's animation clock; -1 every one)
            std::printf("npc %d action %d time %.3f dur %.1f once %d loop %d..%d\n", d.id, d.action, d.time, dur, int(d.once), d.loopFrom, d.loopTo);
        if (dur <= 0) continue;
        if (d.once) { d.time = std::min(d.time, std::max(dur - 1, 0.0f)); continue; } // loop from -1: play once, hold the last frame (duration - 1: mdl_actctl_prc)
        const float end = d.loopTo > 0 && d.loopTo < dur ? float(d.loopTo) : dur;
        if (d.time >= end) { // then repeat frames loopFrom..loopTo
            const float start = std::clamp(float(d.loopFrom), 0.0f, end), len = end - start;
            d.time = len > 0 ? start + std::fmod(d.time - start, len) : start;
        }
    }
}


void drawNpcs(App& app, const cr::Mat4& vp) {
    for (auto& d : app.npcDraws) {
        if (d.alpha <= 0.001f) continue;
        LoadedModel& lm = app.npcModels[d.model];
        const cr::Model& m = lm.cpu;
        const bool matAnim = !lm.packageActions.empty();
        auto matOf = [&](size_t i) -> const cr::Material* {
            const int ap = i < m.meshes.size() ? m.meshes[i].appearance : -1;
            return ap >= 0 && ap < int(m.materials.size()) ? &m.materials[size_t(ap)] : nullptr;
        };
        const bool posed = !m.skeleton.nodes.empty() && lm.meshes.size() == m.meshes.size();
        static const bool noScroll = SDL_getenv("CR_NO_UV_SCROLL") != nullptr; // tests: before / after
        if (posed || lm.meshes.size() == m.meshes.size()) { // skin this instance; scroll its textures
            const cr::Action* act = d.action >= 0 && d.action < int(lm.actions.size()) ? &lm.actions[d.action] : nullptr;
            if (posed) poseWorld(m.skeleton, lm.actions, act, d.time, d.blend, lm.world);
            for (size_t i = 0; i < m.meshes.size(); ++i) {
                float shift[2] = {0, 0}, scale[2] = {1, 1};
                const bool moves = matAnim && effectUvMotion(lm, matOf(i), d.time, shift, scale);
                const bool spins = spinsTexture(m, i);
                float group[2] = {0, 0}; // (_mdl_uv: d0555bt's groups, read_mdl's too)
                const bool scrolls = !noScroll && uvScroll(matOf(i), app.mdlFrame, group);
                if (!posed && !moves && !spins && !scrolls) continue;
                if (posed) cr::skinMesh(m, m.meshes[i], lm.world, lm.skinned);
                const std::vector<float>& pos = posed ? lm.skinned : m.meshes[i].positions;
                const bool uvs = moves || spins || scrolls;
                if (uvs) { // uv * scale + offset (texture unit 0's: the material action's segment 6)
                    lm.uvScratch = m.meshes[i].uvs;
                    for (size_t k = 0; moves && k + 1 < lm.uvScratch.size(); k += 2) {
                        lm.uvScratch[k] = lm.uvScratch[k] * scale[0] + shift[0];
                        lm.uvScratch[k + 1] = lm.uvScratch[k + 1] * scale[1] + shift[1];
                    }
                    for (size_t k = 0; scrolls && k + 1 < lm.uvScratch.size(); k += 2) lm.uvScratch[k] += group[0], lm.uvScratch[k + 1] += group[1];
                    if (spins) spinUvs(lm.uvScratch, app.texSpin);
                }
                app.renderer.updatePositions(lm.meshes[i], pos, uvs ? lm.uvScratch : m.meshes[i].uvs, m.meshes[i].colors);
            }
        }
        if (matAnim) ++app.matAnimDraws[d.model];
        cr::Mat4 world; // (as a frame between the game's steps shows it)
        float alpha;
        shownNpc(app, d, world, alpha);
        drawLoaded(app, lm, vp * world, nullptr, alpha, matAnim ? d.time : -1.0f, d.faded);
        modelDrawn(app);
    }
}

// A model mdl_set_alpha (0x1409c) has run on, as it stays for good: property bit 6 cleared (no colour
// array: glColor alone) and - 2.2.0 only - its alpha test at most 1 (1.0.1's leaves the test); mdl_put
// (0x14a70): texture unit 0 modulated, colour 1's alpha replaced by the model's. CR1's mdl_set_alpha
// sets no blend mode (Omega's does): a model under 255 is blended as the port always has
cr::Material fadedMaterial(const App& app, cr::Material mt, float alpha) {
    mt.properties &= ~0x40u;
    if (app.phoneData) mt.alphaThreshold = std::min<uint8_t>(mt.alphaThreshold, 1);
    mt.texBlend = 4;
    mt.diffuse[3] = std::clamp(alpha, 0.0f, 1.0f);
    if (alpha < 0.999f && !mt.blended()) mt.blendSrc = 4, mt.blendDst = 5;
    return mt;
}

// Draws a model's visible meshes: opaque materials first, then blended ones by draw order.
// `override`, if given, replaces every material (texture unbound); `alpha` < 1 fades it - `faded`:
// as mdl_set_alpha / mdl_put leave it (fadedMaterial), else the old way (the alpha multiplied in).
void drawLoaded(App& app, const LoadedModel& m, const cr::Mat4& mvp, const cr::Material* override, float alpha, float matTime,
                bool faded) {
    const cr::Model& cpu = m.cpu;
    static const cr::Material kDefaultMaterial;
    auto baseOf = [&](size_t i) -> const cr::Material& {
        const int a = i < cpu.meshes.size() ? cpu.meshes[i].appearance : -1;
        return a >= 0 && a < int(cpu.materials.size()) ? cpu.materials[a] : kDefaultMaterial;
    };
    // `matTime` >= 0: the Appearances' own actions colour them at that time (keyType-12 RGBA in
    // segment 1, as effects); a blended one gone fully clear isn't drawn
    std::vector<cr::Material> animated;
    if (matTime >= 0 && !m.packageActions.empty()) {
        animated.resize(m.meshes.size());
        for (size_t i = 0; i < m.meshes.size(); ++i) {
            animated[i] = baseOf(i);
            const cr::Action* ma = effectMaterialAction(m, &baseOf(i));
            if (!ma || ma->segments.size() < 2) continue;
            for (const cr::Track& t : ma->segments[1]) {
                float v[4] = {1, 1, 1, 1};
                if (t.type == 12 && t.comps == 4 && cr::evaluateTrack(t, std::min(matTime, ma->duration), v))
                    for (int k = 0; k < 4; ++k) animated[i].diffuse[k] = std::clamp(v[k], 0.0f, 1.0f);
            }
        }
    }
    auto materialOf = [&](size_t i) -> const cr::Material& { return i < animated.size() ? animated[i] : baseOf(i); };
    std::vector<size_t> order(m.meshes.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const cr::Material &ma = materialOf(a), &mb = materialOf(b);
        if (ma.blended() != mb.blended()) return !ma.blended();
        return ma.drawOrder < mb.drawOrder;
    });
    for (size_t i : order) {
        if (i < cpu.meshes.size() && !cpu.meshes[i].visible) continue;
        if (i < animated.size() && animated[i].blended() && animated[i].diffuse[3] <= 0.004f) continue;
        const cr::GpuMesh& mesh = m.meshes[i];
        const cr::Texture* tex =
            mesh.texture >= 0 && mesh.texture < int(m.textures.size()) ? &m.textures[mesh.texture] : nullptr;
        if (override) {
            app.renderer.drawMesh(mesh, nullptr, mvp, *override);
        } else if (faded) {
            app.renderer.drawMesh(mesh, tex, mvp, fadedMaterial(app, materialOf(i), alpha), app.smoothFilter);
        } else if (alpha < 0.999f) {
            // (mdl_put: a fading model's texture unit 0 modulated, colour 1's alpha the model's)
            cr::Material faded = materialOf(i);
            faded.diffuse[3] *= alpha;
            faded.texBlend = 4;
            if (!faded.blended()) { faded.blendSrc = 4; faded.blendDst = 5; }
            app.renderer.drawMesh(mesh, tex, mvp, faded, app.smoothFilter);
        } else {
            app.renderer.drawMesh(mesh, tex, mvp, materialOf(i), app.smoothFilter);
        }
    }
}

// ---- Shadows (npc_shodow 0x3b3fc -> mdl_shadow 0x138ac; 1.0.1's 0x453fc / 0x68268 the same) ----

// A model's node by its record user id (findObject3D type 0x13: 1102 the head, 1101 the body), -1 none
int nodeOfUser(const cr::Model& m, int user) {
    for (const auto& t : m.records)
        if (t.user == user)
            for (const auto& [obj, n] : m.objectNodes)
                if (obj == t.id) return n;
    return -1;
}

// A shadow's fan (mdl_shadow): 32 tex2d_add_tri (centre, point i, point i + 1) of a black ellipse - the
// points i / 31 turns round, the last back to 0 - with radii rx (the model's x) and rz (its z), turned
// by its yaw (+0x554), at `at`: the centre's colour (alpha `a`) << 24, the rim's 0 (0x13af0..0x13afc:
// tex2d_add_tri's three colours r4, 0, 0) - a soft blob, clear at its edge
void addShadowFan(std::vector<float>& tris, std::vector<uint8_t>& alpha, cr::Vec3 at, float yaw, float rx, float rz, float a) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    auto point = [&](int i) {
        const float t = float(i == 32 ? 0 : i) / 31.0f * 2.0f * 3.14159265f;
        const float x = rx * std::sin(t), z = rz * std::cos(t);
        return cr::Vec3{at.x + x * cy + z * sy, at.y, at.z + z * cy - sy * x};
    };
    tris.reserve(tris.size() + 32 * 9);
    for (int i = 0; i < 32; ++i) {
        const cr::Vec3 p0 = point(i), p1 = point(i + 1);
        tris.insert(tris.end(), {at.x, at.y, at.z, p0.x, p0.y, p0.z, p1.x, p1.y, p1.z});
    }
    const uint8_t centre = uint8_t(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
    for (int i = 0; i < 32; ++i) alpha.insert(alpha.end(), {centre, uint8_t(0), uint8_t(0)});
}

// The fans into tex2d slot 0x4e (tex2d_setup 0x120f0: draw order 990 - after the models -, property
// bits 4 and 6: no depth writes, vertex colours): one dynamic mesh of black vertices - untextured,
// double-sided, depth tested, not written. `mesh` grows to hold them all
void drawShadowFans(App& app, cr::GpuMesh& mesh, const std::vector<float>& tris, const std::vector<uint8_t>& alpha, const cr::Mat4& vp) {
    if (alpha.empty()) return;
    const size_t verts = alpha.size(), room = std::max<size_t>(verts, 5 * 32 * 3);
    if (!mesh.vao || size_t(mesh.indexCount) < verts) {
        app.renderer.destroy(mesh);
        cr::Mesh m;
        m.positions.assign(room * 3, 0.0f);
        m.uvs.assign(room * 2, 0.0f);
        m.colors.assign(room * 4, 0);
        for (uint32_t i = 0; i < room; ++i) m.indices.push_back(i);
        mesh = app.renderer.upload(m);
    }
    const size_t cap = size_t(mesh.indexCount);
    std::vector<float> pos = tris, uv(cap * 2, 0.0f);
    pos.resize(cap * 3, 0.0f);
    std::vector<uint8_t> col(cap * 4, 0);
    for (size_t v = 0; v < verts && v < cap; ++v) col[v * 4 + 3] = alpha[v];
    app.renderer.updatePositions(mesh, pos, uv, col);
    cr::Material m;
    m.culling = 2; // (none)
    m.properties = 8;
    m.blendSrc = 4, m.blendDst = 5;
    m.drawOrder = 990;
    app.renderer.drawMesh(mesh, nullptr, vp, m);
}

// mdl_shadow(model, _, rx, rz, percent): drawn while the model's colour word (+0x55c) is not 0 - its
// alpha above 0 -; percent < 0 at the model's position (y - 0.3 + 0.1), else under its node 1102
// (FUN_00032cc0: that node's x and z - the model's own place when it has none) at the model's height
// + 0.1; alpha |percent| x 255 / 100 - not the model's alpha (Omega's scales it by that)
void modelShadow(App& app, const LoadedModel& lm, const std::vector<cr::Mat4>& pose, const cr::Mat4& world,
                 float rx, float rz, int percent, float modelAlpha) {
    if (int(std::lround(std::clamp(modelAlpha, 0.0f, 1.0f) * 255.0f)) == 0) return; // (+0x55c = 0: none)
    cr::Vec3 at{world.m[12], world.m[13], world.m[14]};
    if (percent < 0) at.y += -0.3f + 0.1f;
    else {
        const int node = nodeOfUser(lm.cpu, 1102);
        if (node >= 0 && node < int(pose.size())) {
            const cr::Mat4 nw = world * pose[size_t(node)];
            at.x = nw.m[12], at.z = nw.m[14];
        }
        at.y += 0.1f;
    }
    const float yaw = std::atan2(world.m[8], world.m[10]);
    const int alpha = std::abs(percent) * 255 / 100; // (((|percent| x 255) / 100) << 24)
    if (alpha <= 0 || rx <= 0 || rz <= 0) return;
    addShadowFan(app.fieldShadowTris, app.fieldShadowAlpha, at, yaw, rx, rz, float(std::min(alpha, 255)) / 255.0f);
    ++app.shadowsDrawn;
}

// npc_shodow (FUN_0001d68c's draw3D, after mdl_put): the leader's (radii 3 / 3, 80%) when its model's
// colour is not 0 (gac + 0xd98), then each NPC whose table row's +18 has bit 15 (+30..+36: radii and
// percent)
void drawFieldShadows(App& app, const cr::Mat4& vp) {
    app.fieldShadowTris.clear(), app.fieldShadowAlpha.clear();
    if (app.havePlayer && playerShown(app) && !app.player.meshes.empty())
        modelShadow(app, app.player, app.player.world, playerWorld(shownWalker(app)), 3, 3, 80, shownPlayerAlpha(app));
    for (const App::NpcDraw& d : app.npcDraws) {
        if (d.index < 0 || d.index >= int(app.npcs.size()) || !app.npcs[size_t(d.index)].hasShadow()) continue;
        const cr::FieldNpc& n = app.npcs[size_t(d.index)];
        auto it = app.npcModels.find(d.model);
        if (it == app.npcModels.end() || it->second.meshes.empty()) continue;
        LoadedModel& lm = it->second;
        cr::Mat4 world;
        float alpha;
        shownNpc(app, d, world, alpha);
        if (n.shadow[3] >= 0 && !lm.cpu.skeleton.nodes.empty()) { // (the head where this frame's pose has it)
            const cr::Action* act = d.action >= 0 && d.action < int(lm.actions.size()) ? &lm.actions[size_t(d.action)] : nullptr;
            poseWorld(lm.cpu.skeleton, lm.actions, act, d.time, d.blend, lm.world);
        }
        modelShadow(app, lm, lm.world, world, n.shadow[1], n.shadow[2], n.shadow[3], alpha);
    }
    drawShadowFans(app, app.fieldShadows, app.fieldShadowTris, app.fieldShadowAlpha, vp);
}

} // namespace crapp
