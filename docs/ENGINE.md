# CR1:U engine notes

The details behind the README: building, the tools, the asset viewer and debugging switches, and
the full status list - what the port does and from which part of the original. The README has
the quick start and the controls.

## Building

Targets: **Linux, Windows, macOS, Android**. One codebase: C++17, SDL3 (window, input,
audio, paths). Rendering is **Vulkan 1.3** (default) or OpenGL 3.3 core / OpenGL ES 3.0
(`--renderer gl`, and automatically when Vulkan can't start). The Vulkan back end needs the
Vulkan loader + headers and `glslc` at build time (shaders are compiled to SPIR-V and
embedded); without them CMake builds the OpenGL renderer only. All texture formats are
decoded to RGBA on the CPU, so no GPU-specific formats are needed.

A `Release` (or `MinSizeRel`) build leaves out the viewer's status and key help at the top of
the screen and the key hints under the menus (`CR_RELEASE`); F3 shows / hides them in any
build. `CR_CAPTURE_HUD=1` keeps the top one in a `--capture`.

Music decoding (AAC): the system's FAAD2 when pkg-config finds it (Linux), otherwise FAAD2
2.11.1's `libfaad`, bundled in `engine/third_party/faad2` (GPL-2.0; sources only, from the
knik0/faad2 release tarball) and built as a static library - the macOS / Windows path.
`-DCR_BUNDLED_FAAD2=ON` forces the bundled copy; `crtool music <dataHD>` decodes every track
(the two builds agree to within 1 LSB on 0.04% of samples: float rounding).

Temporal upscalers (Vulkan): AMD FSR 3.1 is built from `engine/third_party/fidelityfx` (AMD's
GPU passes, MIT) and always available. NVIDIA DLSS / DLAA is built when
`engine/third_party/dlss` holds the DLSS SDK's NGX headers and `libnvsdk_ngx.a` (Linux x86-64);
the DLSS runtime itself isn't kept in the project - point CMake at it and it is copied next to
the game: `cmake .. -DCR_DLSS_RUNTIME=/path/to/libnvidia-ngx-dlss.so.310.9.1`. DLSS runs only on
NVIDIA RTX GPUs; elsewhere the option says why and the frame renders without it. (The DLSS
path was written against the SDK on an AMD machine: it builds, links and backs off cleanly
there, but hasn't run on an RTX card yet.)

If SDL3 is not installed, CMake downloads and builds it automatically.

```sh
cd engine
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
build/chaosrings --data "<path to>/ChaosRingsHD.app/dataHD"
build/crtool check "<path to>/ChaosRingsHD.app/dataHD"   # decode-test every texture
```

Game data lookup order: `--data <dir>`, then `$CR_DATA`, then `dataHD/` next to the
executable (desktop) or `Android/data/<package>/files/dataHD` (Android), then (desktop)
`dataHD/`, `ChaosRingsHD.app/dataHD/` or `Payload/ChaosRingsHD.app/dataHD/` under the
current directory, so running it from the unpacked .ipa folder works.

| Platform | Status |
|---|---|
| Linux | Builds and runs (tested: GCC 16, Mesa, GL 4.6 core) |
| Windows | Code is portable; build with Visual Studio 2022 + CMake. Not yet tested |
| macOS | Code is portable (GL 3.3 core, forward-compatible); build with Xcode tools + CMake. Not yet tested |
| Android | CMake target `main` exists; needs SDL's Gradle project wrapper + SDK/NDK. Not yet set up |

## Tools

```sh
tools/dis.sh _read_mdl_mem_mct            # print one function from re/disasm.txt
python3 -I tools/mct.py <out_dir> <*.mct>  # decode textures to PNG (run from tools/ or add it to sys.path)
python3 -I tools/mcm_walk.py <file.bin>    # list a model's 12 sections and records
python3 -I tools/mca.py <file.mca>         # parse animations (run from tools/)
python3 -I tools/anim_check.py <model.bin> # skinning sanity check (edge stretch), run from tools/
python3 -I tools/descx.py re/disasm.txt <binary> <func_hex> [end_hex]   # recover a loader's field tables
python3 -I tools/evt.py <file.evt> [n ...]  # disassemble event scripts (structure only, no message text;
                                            # @<offset> for EvrStart code or a Gosub target)
python3 -I tools/evt.py --check <*.evt>     # decode every script, report errors
python3 -I tools/cmp.py <file.cmp>          # field collision summary
engine/build/crtool events <dataHD> [-v]    # run every touch trigger's script, list the warps
engine/build/crtool trace <dataHD> <area> <script> [map]  # the instructions a script runs
                                            # (CR_TRACE_HOST=1: on past warps, messages and battles)
engine/build/crtool npcloads <dataHD>       # which scripts NpcMdlRead the story NPCs, and their flags
```

To regenerate `re/` from the binary:

```sh
llvm-nm --defined-only -n ChaosRingsHD > re/symbols.txt
llvm-objdump -d --triple=armv7-apple-ios --mattr=+vfp3,+neon ChaosRingsHD > re/disasm.txt
llvm-objdump --macho --indirect-symbols ChaosRingsHD | awk '/^0x/ && NF==3 {print $1, $3}' > re/stubs.txt
python3 -I tools/annotate_stubs.py re/stubs.txt re/disasm.txt
```

## The asset viewer and debugging

`--viewer` (or `--map`, `--model`, `--models`, `--maps`) opens the asset viewer instead of the
game.

Viewer controls (bring-up stage): **Tab** switches textures/models, **Left/Right** browse,
**drag** orbits, **wheel** zooms, **U** toggles Y-up/Z-up, **N/B** next/previous animation
(the rest pose is in the cycle), **Space** pauses, **-/=** halve/double speed, **Esc** quits.
Start directly in model mode with `--models`, map mode with `--maps`, at a model with
`--model <name>`, or on a field with `--map <name>` (e.g. `--map d0108`). **Tab** cycles textures → models → maps. In map mode **C** toggles the
collision overlay: floors green-grey (tinted by surface type), walls tan, with the field's
prop models in place. **B** shows the field as the game composes it: the background, the 3D
scene through the stage camera, then the foreground. **M** picks another map that shares the
collision file.

The party leader stands on every field. **W/A/S/D** walk relative to the camera; hold
**Shift** to run. Speeds, the stand/walk/run actions and the wall radius follow
`FieldPCCtrl` (see docs/FORMATS.md, "Field player"). **P** shows or hides the player and **O**
switches between the eight party field models (f001–f008). When you browse to a map, the player starts at
the map's first entrance from the warp table (or, without one, where the camera looks at the
floor). `--no-player` starts with the player hidden.


Walking into an exit takes you through it, the way the game does: the exit's script runs and its
warp loads the next room. **C** also shows the trigger boxes: magenta for exits, yellow for other
touch events. The status line shows the area, the map number and the last warp. Exits fade
the screen out and back in, as in the game. NPCs, treasure boxes and field objects stand where the event file puts them (**N** hides
them). They're solid. The game's round **!** badge appears over the one in reach, and **E** or **Enter**
examines it: chests play their lid animation, show the item you got (**E** to close) and fade
their lid out, and crystals are warp points: each one sends you
to its own entry in the warp table (some to the next room, some back to the town hall). Messages an event shows appear in a dialogue box; **E**/Enter turns the page, and you
can't move while it's open. `--capture … --message <id>` renders one message for checking. Field props are invisible occluders in the background view (translucent grey in the
3D view). `--capture <map> <out.bmp>` accepts `--at x z`, `--no-player` and `--no-props` for
checks. A new-game state
is assumed: no story flags are set, so story-gated triggers behave as at the start of the game.
`--new-game` starts where a new game does (the Ark's hall) and plays the opening. Yes/no
choices and the gate list: **Up/Down** pick, **E**/Enter confirm, Esc answers no / stays.
Entering an area runs its entry script, so first visits play their scenes: actors take their
places and walk, and the speakers' portraits appear beside the dialogue box. `--capture … --frames n`
plays n frames of scripts before capturing (pages turn every 45 frames).

Known viewer limitations: lighting is approximated as full-bright (surface colour =
emission + ambient + diffuse; many materials are black-diffuse/white-ambient) until scene
Light records are decoded; effects whose colour is animated (fade in/out) are
invisible until material animation is implemented. **F** toggles smooth texture filtering
(the original uses nearest). Playback runs animations at 30 frames per second and models are Y-up — both
confirmed by eye on c101/c102/c104/m207a (2026-10-12).

## Status in detail

- [x] Binary mapped: unencrypted, unstripped ARMv7, 2,134 named functions
- [x] Compression: HI-LZW (GIF-style LZW) — `tools/hilzw.py`
- [x] Textures (`.mct`): all 441 decode, all 9 pixel formats incl. PVRTC — `tools/mct.py`
- [x] Music (`.caf`): plain AAC-LC, no RE needed
- [x] Model packages (`.bin`) and `MCM_` models: all 12 sections mapped; vertex/index
      buffers, skin data and the render-vertex→position remap decoded. 335/335 packages
      load (4,169 meshes, ~454k triangles) — `crtool models`, viewer model mode
- [x] Skeleton (TransformTree), mesh→node attachment (NodeDeformer), Figure bindings
- [x] Animation playback verified visually: characters move correctly, upright, at natural speed
- [x] Animations (`.mca` and package-embedded): all 205 files / 410 actions parse exactly;
      Hermite/linear/step keys, Euler-in-turns rotations; CPU skinning in the viewer.
      1,402 skeleton animations pose cleanly — `crtool anims`
- [x] Materials (Appearance): texture by id, blending, alpha test, culling, depth test/write,
      texture blend modes, wrap/filter, draw order — 4,109 meshes bound by id
- [x] Models, remaining items settled: every index buffer parses (0 unsupported); material colour
      / texture motion (an Appearance's refGid action: segment 1 RGBA, 6 / 8 UV) plays on NPC
      models as on effects; node visibility tracks never change what's drawn (`crtool visdump`);
      lighting stays full-bright (emission + ambient + diffuse): the packages' Light records (111,
      one each: 83 black, the rest grey / white, a white "type 1" one only in the first
      character's f001 / j001 / n001 / m001 - `crtool matanim`, `crtool lightdump`) can't be lit
      by, or that character would shade unlike the other seven. The NPC model table has 305
      entries (the scenes' bosses and battle models from 197; it was cut at 200, hiding rows
      501.. of every dungeon and town); its 99..197 props are never used by the shipped data
- [x] Field map structure: 2D background + foreground overlay + collision + props (74 fields)
- [x] Field collision (`.cmp`): 114/114 parse exactly — `crtool maps`, viewer map mode
- [x] Stage records in the event files: camera, view size and background placement rect.
      All 114 maps get a camera; backgrounds line up with the collision (incl. the d05xx
      512-wide tower fields)
- [x] Field player: walk/run on the collision floor with wall push-out (FieldPCCtrl). The
      self-test walks every map: 0 floor losses; 114 maps × 8 directions × 400 frames give 0
      wall crossings
- [x] Event scripts (`.evt`): all sections mapped; the bytecode (61 opcodes plus an
      expression language) decodes completely — `tools/evt.py --check`: 27,010/27,010
      scripts. The engine interpreter runs control flow, expressions, vars/flags and warps,
      skipping presentation opcodes — `crtool events`: 27,010 scripts run without errors
- [x] Room exits: touch-trigger boxes run their scripts, and warps load the destination map,
      position and facing. The self-test fires all 459 exits: all arrive on the right map, on a floor
- [x] Field NPCs and objects: models from the NPC table, flags, treasure-box selection
      (TreasureBoxManager) with their markers; 1,526 drawn across all maps
- [x] Field props are depth-only occluders: the player is hidden behind painted pillars and
      door frames (63–69% behind a frame, 0% in the open)
- [x] Scripts play over time: waits, screen fades, NPC animations and fades, the item window,
      dialogue; the game's "!" badge marks what you can examine
- [x] Sound effects (sndeff.bin, 253 sounds): script sounds (chests, crystals, …) and
      footsteps by floor surface; dialogue and item windows use the game's own window panels
- [x] Music: the 29 AAC tracks (FAAD2 on Linux), looping from the game's repeat points, with
      script BGM changes and fades; each area starts its usual track
- [x] Music on macOS / Windows: the bundled FAAD2 (`engine/third_party/faad2`) builds where no
      system one is found; verified on Linux by forcing it (`-DCR_BUNDLED_FAAD2=ON`: same frames,
      PCM within 1 LSB). Not yet built on an actual Mac or Windows machine
- [x] **The field camera** as the iPad build has it (`FieldCamera`, `fldread_prc`): each field's view
      is doubled, so a painting covers 2048 × 2048 screen pixels (two to a texel) and a
      screen-sized window of it follows the player (`setScrollPlayer`: the head kept centred, the
      window inside the painting's rect). With the camera free the scripts move it: `FrmAdjCam`
      slides to a shot, `FrmPanCam` slides by twice its deltas, `FrmSetCam` centres it,
      `Mapscroll` (op 31, now decoded at its real 4 / 8 bytes) centres an NPC or a point
- [x] **Scene effects** (`ScnEffectRead` / `ScnEffectStart` / `ScnEffectProc`, calls 2020 / 2009): the
      dungeons' weather over the field - the first gate's snow and fog, the second's haze and sun,
      the third's fog, the fourth's rain, p01's fog - each layer a wrapping texture moving by the
      binary's per-frame steps and against half the scroll, some breathing in and out
      (`UpdateRandom`); the tower's Space backdrop (effects 6..8, 12..20: its paintings scrolling
      up, down or still behind the 3D, fog drifting both ways over the top). They go on in
      battles too, the weather with its "(128)" copies; Options' Environment Effects Off leaves
      out the weather and the tower's fog, as the original's does. `--capture … --scene-fx <set> <effect>
      [frames]` turns one on. Not drawn: effect 1 (a blinking overlay whose texture nothing found
      loads)
- [x] The game boots as the original does: the boot area's script (j001_t99, EvrStart 0) shows the
      Square Enix, Media.Vision and Mascot Capsule logos, the title picture and music, then the
      title menu (Start / Scenarios / Options, `Util 0`). Start goes on to Character Select and the
      pair's save / load screen (below); **Scenarios** is the story flow chart (`__menuu_History*`,
      flow_hd.mct, every number from the binary's tables): a column per pair, its ten chapters in
      the order that pair meets the gates - "?" until reached, the picture once reached, a star
      when cleared, padlocks on the ways not yet open - the finale across the bottom once all four
      stories are done, the later pairs' strips silhouetted until a first story is finished, and
      Achievements % (each chapter's points). It reads every pair's saves together, as the
      original's system data; Up/Down, the wheel or a drag scroll it. Options opens the screen
      below in title mode. `--viewer` (or `--map`,
      `--model`, …) opens the asset viewer instead; `--new-game` skips the title
- [x] The Options screen (**V**, or Options on the title), rebuilt from the original's pieces
      (`gfx/menu_painter.cpp`: its label bars, Check / On-Off / Japanese-English buttons, gauges,
      page number and Back) but organised by subject on four denser pages (rows 36 apart, controls
      at 85%): **Game** - Language, Text Language (all eight), Environment Effects, Lock Screen
      Orientation, Instruction Manual, Return to Title; **Sound** - Master, BGM, Sound Effects,
      Voice, Sound Test; **Display** - Resolution, Display, Aspect, Painted Rooms, VSync, Original
      Presentation; **Graphics** - Render Scale, Anti-aliasing, Upscaler, Upscale Mode, Sharpness,
      HD Textures, Smooth Models
      (a line under the rows explains the row under the cursor, and why an option can't run on
      this machine). The original's own settings keep their labels in
      the game's language. Keys: Up/Down, Left/Right, Enter, Q/E or PgUp/PgDn for pages, Esc; the
      mouse clicks buttons, gauges, the page number and Back. Saved to settings.ini. From the title
      it is in title mode, as in the original: Reset Data instead of Return to Title. Return to
      Title works. The **Instruction Manual**: the 29 topics of system messages 56000 on (titles
      in label bars, 56007 - the manual's own introduction - first), the page beside them, wrapped
      to the panel. **Reset Data** (from the title) asks twice ("Erase all the data from your game?",
      then "This will erase all your game data. Are you sure you want to proceed?") and with Yes to
      both removes every pair's save and the resume point (`OptionPrc`: `prty_init_all`,
      `removeSaveData`, `removeContData`). The orientation lock is stored but has nothing to turn
      on a desktop
- [x] Solid NPCs (chests, crystals, blocks block the player) and examining (**E**/Enter):
      treasure boxes give their item (from the game's item pools, named from sys.evt) and lose
      their lid; crystals are warp points
- [x] Dialogue: message text decoded (area files and sys.evt, English and Japanese, control codes) and
      shown in a dialogue box when an event shows messages (**E**/Enter pages through)
- [x] Entry scripts: each area's script 0 when it loads and the room script (EvrStart 5) on every
      room change, played over time. The self-test plays all 28 area scripts and 523 room
      scripts from a new game to the end: 0 errors, 0 stuck
- [x] Cutscene staging: scene records (section 6) place, walk and turn the actors and the player
      (`FrmSet`/`FrmJmp`/`FrmSetc`/`FrmSetc2`/`FrmAdjc`/`FrmAdjc2`/`SetDir`), `Born`/`Kill` show and
      hide them; speakers' portraits slide and fade in beside the dialogue box (`Tex2d`, read
      from `__EvtTex2d` / `tex2d_img_prc`: 3-frame fade + 32 px slide, flag 0x200 swaps an
      expression in at once and clears that side, 0x40 zooms a centre image 200% -> 100% over 4
      frames; in widescreen side portraits keep to the screen's edges)
- [x] Saves, as the original keeps them (`game/save.cpp`, a small text file in the save folder):
      one per pair (each pair's story is its own: save1.dat .. save4.dat), saved from the **Camp
      menu** (K on the field, laid out as `__menuu_CampMainPut` draws it: the pair's face
      boards, the clock and the purse, Items / Equip / Genes / Status / Options, and Save) or a script's Util 5/6 - "Save data?" Yes / No, "Save complete.". The
      title's **Start** runs as the boot script has it (j001_t99: Util 0 -> Util 4 -> Util 5):
      **Character Select** (`__menuu_StartPut`: the chara1 art scaled to the screen, plates
      Escher / Eluca at (56 + 232k, 232), Back at (124, 288)), then that pair's save / load screen
      (`__menuu_SaveLoadPut`: over the Character Select art again - piece 16 of its sheet at tex2d
      (-32, -64) -, the slot's face boards, its date and play time, the pair's three
      emblems for the stories it has cleared, Load / New at (328, 136 + 48k) - and "Final" once
      every pair is cleared -, "Load data?" / "Begin new game?", or with the pair's clear data
      "Begin new game with your clear data?" then the plain new game). The choice goes to the
      script as the original's kind (var 1: 1 the last battle, 2 new, 3 new with the clear data,
      4 load) with the pair in var 0, so each pair plays its own prologue on d0550 into the Ark
      (97); `--save-screen clear` shows a cleared slot. The script's `LoadPrty` is the original's
      `prty_init_new`: the pair's flags cleared, its members back to level 1 with their
      starting gear, genes and items; with the pair's clear data the game goes on from it (its
      Oz kept), and "with your clear data" also keeps each member's stat bonuses, equipment and
      gene plates and the bag, less the story's items; `ResetFieldFlag_NewGame` clears the pair's
      field flags (its four blocks of 3000..3063, and 3300..3363). The chara2 sheet (the third and
      fourth pairs) opens once a first story is done (flag 32 or 34). The
      **Progress Tracker**: leaving the game on the field writes a resume point; the next title
      offers "You have save data from your last session. Resume game from last saved point?".
      Saved: flags, variables, items, opened boxes, area / map / position / facing, the leader,
      play time, the date. Loading arrives as a warp would (the room's entry scripts run).
      `CR_SAVE_DIR` puts the files elsewhere; `--camp`, `--save-screen save|load`, `--char-select`,
      `--scenarios` (`--at y` scrolls), `--manual` (`--at n` picks topic n) capture them, `--title-new p` goes through the title as a player would (pair 1 / 2, New)
- [x] **Shops** (Util 2, `__menuu_Shop*`; the Ark's shopkeeper, script 251 of areas 7/13/19/25):
      Talk / Buy / Sell, then Items / Weapons / Armor / Jewel, the list, how many with Yes / No,
      and "Equip it?" after buying gear - laid out from ShopPut and the ComUty_* pieces it calls
      (all its windows sit at 0, 0) and checked against the reference playthrough: the shopkeeper's
      art on the plates' screens; in equipment lists the character's board (`ComUty_StateBoardDraw`:
      face, LV, HP / MP / NEXT bars, EXP and the stats, the highlighted item's change) and
      ◀ Back ▶ to switch between the pair's two characters; the HELP board over the list; the
      message board (picture 12) for the questions. Widescreen keeps the left and right groups to
      the screen's edges, as the later widescreen release does. Stock follows
      `__GetShopItemTbl`: each character's own weapons and armor, more of everything as the
      shop's level (2..7, `MuCom_GetShopBuyLevel`, from story flags) rises; selling pays half.
      Item prices, icons and kinds come from sys.evt (+0x38, 20 bytes per item). Talk plays the
      shopkeeper's EvrStart 4 scene while the shop waits. Oz and the party (levels, experience,
      equipment; stats from `_pcparam` + level + equipment, `pcparam_max_*`) are saved. Oz comes
      from selling and battles; Sexy Pose's 10% off and the seeds' and genes' stat bonuses apply
      (see Skills).
      `--capture t0108_10.cmp <out> --shop main|category|buy|items|sell|count|equip|talk`
- [x] Message windows as the script's `Open` picks them (`__EvtOpen` / `PutWin` / `PutMsg`: its
      style byte and lines choose a rect from 0xd3d30 and the look): style 2, the speech, on the
      torn parchment (window_hd 1024,248 1024x280, the name on the blue streak painted into it,
      dark ink); 64 the grey panel (960 x 192 at its rect - one line low, else high; no "next"
      mark); 8 the tutorials' dark band with soft edges (at the top, or the middle for one line,
      the text further left); 32 a black band across the bottom, the speaker's name on its top edge
      (tex2d (16, 236), outlined); 16 the text alone, outlined in
      black. Lines marked with code 0x17 flip their alignment (centred about the layout's middle;
      the boxless style centres every line but those), as `CmdFont` does. Pages type out a character a
      frame (a centred line from where the whole line starts); a tap brings the rest out at once,
      the next turns the page; code 0x1a pauses the typing, and a message ending in 0x14 closes by
      itself (after the 0x1a before it). `CR_CAPTURE_TYPED=n` captures a page n frames into its
      typing (tests and other captures show pages whole). The page's "next" diamond, and the skip button (top right, or X) that turns the
      scene's pages by itself until it ends (choices still wait); a click turns the page.
      `--capture … --talk-portrait n` captures the n-th hall conversation moment with a portrait
      up, `--event n` starts the room's script n (`--area n` picks the event file where routes
      share a map, `--set-flags a,b` sets story flags first, `--win-battles` wins the battles it
      starts, `CR_CAPTURE_TRACE=1` prints the script's state as it changes)
- [x] New game: the opening plays as the original's script stages it (a 3D room filmed by the
      script camera: cuts and moves between shots, framed as `setCamera3D` does - the stage view
      doubled and centred, so the screen shows its middle), then the hall's characters can be talked
      to; speakers get name plates and the listener's portrait dims; yes/no choices (Util 3) and
      the gate choice (Util 1) lead into the first dungeon
- [x] Voiced 2.2.0 iPhone data (`--data …/ChaosRings.app/data`, also found automatically next to
      the executable): Japanese voices play with each voiced page (0x1e cues → voiceevnt.bin) on
      the Voice slider, and battles speak as 2.2.0's do (`_btlvoice_*`, voicebtl.bin): command
      lines as a member's menu opens, attack lines by how the targets fared, hit / down / revive /
      healed reactions, a round's gene and special lines, escaping, the victory (scripted
      battles' own) and defeat - one line per speaker at a time, three variants each. 2.2.0's
      38-byte action records are read as such (they were misread as 1.0.1's 24 before)
- [x] Both screen layouts (Options, Display, Layout: Auto / iPad / iPhone, or `--layout`): the
      iPhone's 3:2 screens as 2.2.0 draws them - the 480 × 320 space filling the screen, corner
      pieces moved to its corners, 2.2.0's own camp, save, message box, staff roll and History
      (half-size, scrolling sideways too) - with either data set; Auto picks the iPhone's for
      2.2.0's data
- [x] Text in all eight languages (Options, Language / Text Language, or `--language n`: 0 Japanese,
      1 English, 2 French, 3 German, 4 Spanish, 5/6 Chinese simplified/traditional, 7 Korean;
      1.0.1 has Japanese and English). Drawn with a system font through stb_truetype
      (`engine/third_party/stb`, public domain), as the iOS game uses the system font: Noto
      Sans CJK on Linux (its JP / SC / TC / KR face per language), Hiragino / PingFang / Apple
      SD Gothic on macOS, Yu Gothic / MS Gothic / Meiryo on Windows; `CR_FONT=<file>` picks one,
      `CR_FONT=none` keeps the 8x8 font (English and the Latin languages, accents folded)
- [x] Vita assets (Options, Game, Vita Assets: pick your copy's PCSG00497 folder - saved as
      settings.ini's `vita=`, used from the next start - or `--vita <folder | cr1.psarc>`, `CR_VITA`;
      `--vita none` for a run without): from your Vita copy the textures and model packages
      (lossless where the iPad files are PVRTC / RGBA4444: portraits, the UI sheet, many model
      textures; art the Vita redrew for its own screens keeps the iOS version), the scripts (every
      `.evt` but the boot area's, which is Vita-only code: all eight languages and the voice cues,
      also over 1.0.1's data), the voices (48 kHz
      ATRAC9, story and battle, in place of 2.2.0's 20 kHz banks - also with 1.0.1's data) and the
      music (48 kHz ATRAC9, looping sample-exactly; bgm31 stays iOS). ATRAC9 is decoded by the
      bundled LibAtrac9 (`engine/third_party/libatrac9`, MIT); `crtool vitaaudio <folder> [2.2.0
      data]` decodes it all and checks the voices against 2.2.0's
- [x] The Progress Tracker as the Vita keeps it: written as you go - at every scene's end on the field
      (but the first after a load or new game) and as an encounter starts - besides when the game is
      left (iOS's only moment)
- [x] The Vita's boot when its scripts are used (also over iOS data, so every script's text is the
      Vita's): the health warning, Square Enix and Media.Vision logos (opcodes 0x47..0x4a: faded in,
      held - a tap or confirm cuts it short - and out)
- [x] A build that carries its own data: `vita/` (a Vita copy's CR1 folder, or its PCSG00497
      folder) and `hd-textures/` beside the executable are found by themselves - the Vita copy
      when nothing else is configured (the game itself when no iOS data is found; not written to
      settings.ini), the HD textures on by default unless settings.ini turns them off
- [x] The Vita copy alone as the game (`--data` or `--vita <PCSG00497 folder>`, `CR_VITA`, or a saved
      Vita Assets folder, with no iOS data found): everything from `cr1.psarc` and `sound/` - the
      textures by their `.gxt` (an iOS `_hd` name the Vita lacks by the plain one), its own
      screens (title, chapter cards, character select: laid out as 2.2.0's), its scripts with the
      boot area's own start-up (its logos skipped), voices, music, and the sound effects from
      `sndeff.bnk` (a SCREAM bank of HE-VAG, decoded by the port; the 128 HE-VAG filters from
      vgmstream, `engine/third_party/hevag`, ISC). `crtool vitasfx <folder> [iOS data]` checks the
      253 effects against `sndeff.bin`'s
- [x] `--explore n`: plays the game by itself through to the end - from the title as a player
      starts (Character Select, the pair's save screen), talking to everyone and walking into every
      trigger not yet tried with the current story flags, answering yes, winning the battles the
      story starts, saving where the game offers, taking each gate in turn and walking back through
      the doors it knows when stuck; each pair's story to its second ending (flags 31..38), then the
      next pair from the title, and with all four done the title's last battle to the final ending
      (flag 39): 3245 actions, 142 rooms, 66 battles (the puzzle rooms taken as solved - it doesn't
      play them). Its saves go to a new folder under the profile (`CR_SAVE_DIR`'s in tests);
      `CR_EXPLORE_SAVES=<folder>` goes on from another run's. Debugging: `CR_EXPLORE_FLAGS`,
      `CR_EXPLORE_CANDS=<map>`, `CR_EXPLORE_TRACE=<step>`, `CR_EVENT_STARTS`; captures take
      `CR_CAPTURE_MAP=<n>` (maps sharing a file) and `CR_CAPTURE_NPCS`
- [x] Random encounters as the original counts them (walked distance, rising chance, groups by
      weight and party level, first strike / ambush) and script battles (`Battle`): the battle
      stage, the pair and the enemy group at their formation's places, idling, through battle
      camera 0, with the battle music. `crtool battle` checks every map's table (265 rooms, 112
      groups, 48 monsters, 9 stages). `--no-encounters` turns them off; `--capture … --battle
      <group> <stage>` renders one
- [x] Camp pages (`__MuCamp_NextProc`; its jump tables recovered with
      `FixJumpTable.java` in Ghidra): **Items** - the bag by category (Standard, Event, Weapons,
      Armor, Jewel, Gene Plates; Left/Right), the seeds (a stat point each, up to 5000, how many
      asked) and the MP items used on a member, the item's HELP; **Equip** - a member, weapon /
      armor / jewel, what the bag holds for it with the state board showing the change;
      **Status** - a member's boards (Left/Right for the other); **Genes** - the gene plates in
      the bag on the gene board (picture, n / 6 known, abilities and skills). Seed points and
      worn plates are saved. `--capture … --camp --camp-view
      items|use|equip|slots|wear|status|genes|plate` renders a page.
- [x] Genes (`game/genes.cpp`): abilities (items 81-130, battle actions +120), skills (131-186)
      and the 22 gene plates; each member wears the Human Genes and up to three plates (Equip).
      In battle **Genes** lists what the worn plates hold that's known, with its MP (pair mode:
      either member's, both pay); monsters teach genes and leave their plate and items
      (`btlcalc_0505` / `0506`), shown on the results' board. A new game starts as
      `prty_init_new` has it.
- [x] Skills (`chk_skill_btl`, `btlcalc_zoku_0204`, `pcparam_max_*`): the blocks, MP 1/2, regen,
      the critical skills, the attack / resistance / element / herbivore-carnivore scaling, auto
      guard, counters, Boost Recovery / Magic Power, sure escapes, more genes, EXP / OZ +20%,
      post-battle MP, the level-up boosts (saved), the Gates' stat points, Preemptive / Alert on
      encounters and Sexy Pose's 10% off in shops; monsters use their own from level 20; the
      Blaze / Aqua / Gale Resistance armors; Encounter (skill 170: the field menu's switch) and
      Happy Smile (174: EXP banked by walking). Secrets of Stealing and Get More Items have no
      effect in the original either (`btlcalc_0504` checks skill 157 and drops the answer).
- [x] Battles (`game/battle_rules.cpp`, from the Ghidra decompile of `btl_main` and its
      helpers): Solo / Pair commands, Attack, Items, Defend and Escape (Yes / No), targets on
      their plates, the turn order by speed, the original's damage, critical and hit rules,
      the element triangle and monsters' resistances, healing, item and status calcs, the
      enemies' AI lists, the two-enemy pair attacks, the Break gauge and its badge, first
      strikes and ambushes, experience, levels and Oz on the results screen, losing back to
      the title. The HUD is the original's (`menub_mainPut`): the badge, the members' HP / MP
      rows, the command buttons, the action banner and the damage numbers with their labels;
      the cameras are the original's per menu, target and attack. Attackers run up to their
      target and back; the fallen fade out. `--capture … --battle <group> <stage> --battle-view
      top|commands|targets|items|escape|hit<n>|results[@frames][:taps]` renders a screen of it
      (the results that many frames in, after that many taps).
- [x] Every action calc (`btlcalcTbl`, 0..36): fixed and split damage, % of HP, down to 1 HP,
      MP damage, Sacrifice, drains (Steal HP / Humor Siphon), Oz theft (won back with the thief),
      monsters running, MP / % heals, full restores, revives, poison (10% a round) and its cure,
      clearing statuses, Analyze (the enemy's weakness), enchanting an element, the counter
      stance, the magic barrier, one-turn invincibility, the Paradise Apples' double EXP / OZ; the
      boss actions use them too. Their messages show in the top bar.
- [x] Battle effects (`eff3d_*`, `eff.bin`'s 98 effect models): hit sparks, criticals, the
      elements' hits, gene bursts (single and over a side), heals, buffs, drains, revives,
      the casting glow and monsters' falls, at the original's attachment points (body, head,
      limbs) and with their materials' colour / fade animation. `--battle-view effect<id>[@frame]`
      shows one, with their textures' scrolling (the material action's UV offset / scale).
- [x] Field effects (the same models on the field): the puzzle rooms' (gems, teleports, the
      held block's glow, breaks, blocks changing over, the candles' lighting, flames and puffs,
      the crystals coming and going, squashes), the footsteps' dust, the cutscenes' looped
      effects (`FrmPlayEff` / `FrmPlayEff2` on an NPC, the leader or a record's point and angle,
      `FrmStopEff` / `FrmStopEff2`) and the scripts' call 2023 (`Uty_eff3d_set_fld`); `crtool
      effects <dataHD>` lists `eff.bin`
- [x] Story battles' line-ups (`btl_prty_init`): a member alone, a guest beside the leader
      (Olgar / Ayuta, level + 5, acting by itself in the original's rotation) or the partner away
      while the story keeps them apart; monsters that act twice a round; Asura Lotus's 2-5
      strikes; Steal Item (once per enemy, kept even when fleeing); the Options button.
- [x] The bosses' staged attacks (`btlact_430`, `432`..`436`): their own motions (the boss
      models' 14th / 15th slots), runs of cameras, effects at points and on the targets,
      sounds, white flashes over the 3D and screen quakes at the original's frames, the hit at
      the end and the wait after. Every landing hit shakes the screen (`StartQuake(3, 100, 0,
      5)`). `--battle-view stage<action>[@frame]` makes the first enemy use one.
- [x] Smaller menu pieces from the playthrough: the event Yes / No (Util 3) as the original's
      plates at (336, 104) / (336, 160) sliding in from the right (`__menuu_EventPut`), not a
      text panel; the battle's Genes list with its HELP board (target and effect); the camp's
      gene plate rows (`ComUty_ComPlate_Equip_GenePlate`: a pip per entry, the unknown ones dim,
      MASTER once all are known, the name yellow when worn, grey while nothing on it is known)
      and the gene slot page's HELP board; the World Select's lists at the shop's rows with its
      scroll bar, the bands with the gate icon (`ComUty_ComPlate_Lvl`). `--battle` captures mark
      the battle tutorials seen (`CR_CAPTURE_TUTORIALS=1` keeps them); `CR_CAPTURE_CHOICE=1` puts
      the Yes / No over `--message n`
- [x] The Break changing hands (`btlbreak_effect_*`): when a side loses the Break the battle
      waits for "BREAK!! The battle situation changed" (the stripes, "Player" / "Enemy" sliding
      along them, the original's sounds), and half way through the other side takes it at 4 - the
      port used to leave the gauge at 0 to fill again a point at a time
- [x] Checked against the 2.2.0 code (tables byte-compared, the rules read side by side): the
      levels from 20 grow every stat (`pcparam_change_lv`, with 2.2.0's fix for multi-level
      battles), seeds share that 5000; monsters teach the first unknown of all six of their
      plate's genes and leave plate 481 + (model − 13) / 3; items roll before they're chosen;
      the critical roll uses the first target's level; guests' Speed / Luck are their Defense;
      EXP / OZ +20% counts the member queued last out of pair mode; a member down at the end gets
      half the EXP; the resisting armors count as their skills; losing an ordinary battle goes
      back to the pair's Ark with a fifth of the Oz gone (a script's battle still restarts).
      Also 2.2.0's escape timing (30 + 40 frames away, 40 stayed; the line only on success) and
      the original's random numbers (`core/random.hpp`: MT19937, its bounded draw and
      `get_random`'s discards, one stream for battles and the field)
- [x] The battle's status messages (`btl_add_msgque`): "<name> was poisoned!", the stat ups,
      the barrier, endure, invincible, silence, an element given, a command cancelled and each
      stolen item queue up (one per fighter) and take the banner one at a time for 60 frames,
      the battle waiting for them; the immunities ("The poison didn't work!") show at once. Also
      Risk Breaker (203) and Trompe-L'œil (201)'s own rules, silence-immune monsters, and
      endure / an element given counting for their tutorials
- [x] The battles' talks (`_chk_karaok_*`, from `btl_main` / `PrcEvnt`): a script's `Battle`
      ends its frame and the script waits behind the battle's gate (flag 4095) - in an *event
      battle* (one a running script started) it goes on inside the battle when a story boss's
      moment comes (by group and route: the start, a round, the boss's HP, its fall) until it
      clears the flag, and after the battle it carries on from there (no room entry script).
      Meanwhile the battle waits and its HUD gives way to the portraits and the message box;
      those bosses can't fall before their last moment (`chk_karaok_dmg_hp`) and some heal whole
      each round until it (`btlcalc_status_prc`). In other battles the area's EvrStart 2 shows
      the tutorials the first time each thing happens (the first battle, the Break, an element
      taken, a status, a gene learned, the Genes and Defend commands; flags 4081..4085, 4008,
      4009), with their animated pictures (`battle_0n_hd`, `gene_01_hd`). After a battle that
      wasn't a script's, the room's entry script (EvrStart 5) runs again as the field reloads.
      The battle comes in out of white over its opening (`SetFade`). `crtool npcloads <dataHD>`
      finds the scripts that bring in the story NPCs 501..512 and the flags that let them
- [x] The field menu (`menuf_mainPut`): the corner button (or R) slides out Camp, Save, Map and
      — once skill 170 is known — the switch that turns this pair's random encounters off and
      on (J; refused while the story holds it). Skill 174 worn banks 1 EXP every 10 units
      walked, paid into the next win. Both are kept in the save. `--capture … --field-menu
      closed|open|known|off|map[step]` shows it. Map opens the gate's map (`__menuu_Map*`): the
      pair's blinking marker on its field and the story's two markers for where to go next.
- [x] Script natives: the pair's healing (2018), the music by var (2024 / 2026), the shop ended by
      its script (2025) and its own music (track 19, then the one before), the pair number
      (2027), the loading spinner (2028), the dungeons' area-name banners (`TexDisp`, 2016), the
      puzzle tutorials' animated pictures following the message's pages (`TutoTexDisp`, 2033),
      the "!" over a dimmed screen (2007), the pad's picture (2008), the logos once a run (2029),
      two story battles' stat rises (2030 / 2031); `crtool natives` lists what the scripts call.
      Every native the scripts use is handled
- [x] The World Select (Util 1, `__menuu_WldChoice*`): the title art, the focused gate's picture
      (MAP_01..06), the gates the pair has reached in its own order as plates with their four
      progress pips, "Skip the first half?" where it applies, the level bands (2 + those the pair
      has opened), the board with the gate, band and "Is this okay?", Yes / No and Back; the
      script gets the gate, band, pair and the skip as the original's Clr sets them (it used to
      get the list position, wrong for the other pairs' orders)
- [x] Event scripts' menus (`Util n`): 0 title, 1 world choice, 2 shop, 3 yes / no, 4 character
      select, 5 / 6 save / load, 7 the staff roll (the credits over the ending's picture, the
      corner button / Space to hurry, the end card, a tap to close) and 8 the last battle's pair
      choice (each pair once against the four guardians, its plate named for its leader as the
      reference shows: its camp menu, then Yes / No). `__menuu_LastBtPut` / `WldChoicePut` lay
      out as the camp does (windows at (0, 0), 1.0.1's records: the header at (8, 6) / (8, 4),
      the rules at (0, 56), the picture at (0, 64)); the pairs are list rows
      (`ComUty_ComPlate_LastBt`: plate 8 / 9, icon 4, the name at (56, 12)) at (192, 92 + 48k)
- [x] **Puzzle rooms** (area 1, maps 114..177 - the map table now has all 178): the 16 teleport
      stages play - the blocks, gems, weigh switches and crystals on their grid, a tap (or T,
      straight ahead) swapping places with a block or gem, the ESP count (at 0 an ability is
      refused with the original's mark), the crystals' scenes (`Uty_WarpCrystalCamera`), F5 the retry script. The 16
      psychokinesis stages play too: T (or a press on a block) lifts it, the arrow keys / WASD
      (or dragging and letting go, past the original's dead zone) send it along its row or column
      to the next block or wall, onto switches; T lets go for free; a block running into the
      leader resets the room. The 16 toughness stages too: a tap (or T) breaks a fragile block,
      or changes every red and blue block over (put-away ones drawn faint and walked through;
      one coming back on the leader resets the room). And the 16 pyrokinesis stages: a tap (or
      T) lights or puts out a candle switch - changing its candle blocks over (stages 48-55) or
      passing it on to its linked switches (56-63, with the original's beam); every switch lit
      brings the crystal. The original's pad: the "Left" count (or infinity) with its swell,
      the Retry button (blinking once the count runs out) and Help, both clickable, the
      punishment mark over a refused tap, and the "Puzzle Start" banner (call 2003) as a
      dungeon leads into a puzzle room; the leader's pose while holding a block and the
      landing quake. `crtool puzzles <dataHD>` lists every stage.
- [x] Field quakes (`Quake`, `FieldCamera::setOffset`): the scripts' Quake op (43) and the puzzle
      blocks' landings move the field view by the quake's whole pixels
- [x] Engine skeleton: SDL3 app callbacks, GL loader, 2D renderer (1024×768 letterboxed), data discovery
- [x] Vulkan renderer (`gfx/vk_device.cpp`) alongside OpenGL (`gfx/gl_device.cpp`) behind one
      interface: same output (captures differ in < 0.05% of pixels, single texels at nearest-
      filter boundaries); the self-test passes on both. Checked under the Khronos validation layer
      with synchronization validation (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
      VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`): no errors or hazards over the self-test and live
      runs with FSR, HD textures, smoothing, MSAA and render scale
- [x] Video options (the Options screen's Display page; saved in settings.ini). The defaults are the
      original presentation - 1024×768 window, 4:3, painted rooms at the original's 2× following
      the player, native resolution, no anti-aliasing, vsync - and "Restore the original
      presentation" brings them back.
      - Resolution 1024×768 … 3840×2160; window, fullscreen (that display mode) or borderless
      - Aspect: 4:3 (original, letterboxed) or widescreen (the window's aspect, up to 21:9):
        battles, 3D rooms and fades widen; full-screen pictures (the title, Character Select's
        and the save screen's art, the script's cards) cover the whole width, cut at the top and
        bottom; the menus keep their left-hand group (headings, boards) to the screen's left edge
        and the right-hand one (plates, lists, Yes / No, the clock and Oz) to its right, as the
        later widescreen release does - the title, Camp, both save screens, shop, battle and its
        results, World Select, Final Battle, the field's corner buttons and a puzzle room's
        counter, taps and clicks following them; the list screens (Options, the manual,
        Scenarios), Back, the message boards and dialogue stay centred. The iPhone layout keeps
        its one centred screen
      - Painted rooms: Original (2×, a screen-sized window following the player and the scripts),
        Whole (the whole painting at the screen's height, side bars) or Fill Width (top and bottom
        cut off). The C key shows the collision and trigger meshes (off by default)
      - Render scale 50–200% and MSAA 2×/4×/8× (Vulkan): the frame renders offscreen at that
        size and is scaled to the window (bilinear); 200% doubles as supersampling
      - Temporal anti-aliasing / upscaling (Vulkan, the Graphics page): **FSR 3.1** (AMD's
        passes, `gfx/vk_fsr3.cpp` reimplementing the SDK's host side) and **DLSS** (NGX,
        `gfx/vk_dlss.cpp`), at native resolution - **FSR Native AA** / **DLAA** - or upscaling
        from Quality (1.5×), Balanced (1.7×), Performance (2×) or Ultra Performance (3×) per
        side; FSR's RCAS sharpening 0–100%. The scene - paintings, 3D, battles - renders at
        render size into colour + depth + motion vectors + a reactive mask with Halton sub-pixel
        jitter (the scene pipelines: `shaders/*` built with `SCENE`); motion comes from each
        draw's last-frame matrix and, for re-skinned models, last frame's vertices; blended
        surfaces that write depth (most of the game's models) count as solid, see-through ones go
        to the reactive mask. The upscaled frame then gets the UI (dialogue, menus, fades) at full
        resolution. MSAA is off meanwhile; map changes and battles start the history over.
        Measured against a 4×4 supersampled reference (battle, 1024×768): no AA 29.1 dB, MSAA 8×
        29.7, FSR Native AA 36.5, Quality 31.2, Balanced 29.8, Performance 28.1, Ultra 25.1.
        The game renders in ~0.1 ms at 4K on a desktop GPU and FSR costs 0.7–1.7 ms, so here the
        upscaling modes are for weaker GPUs; Native AA / DLAA is the quality option
      - **HD Textures** (Graphics page, off by default): waifu2x / Real-ESRGAN upscales of every
        texture, made once on your machine and kept outside the project and the game data (they're
        derived from the game's art - personal use only). The engine finds each by the hash of the
        original's pixels and draws it at the original's layout size, with mipmaps and anisotropic
        filtering. To make them:
        `chaosrings --export-textures ~/.local/share/preservation/ChaosRingsHD/hd-source/ipad`
        (676 distinct pictures - 556 lossless, 114 PVRTC, 6 16-bit on the iPad; add `--vita
        <PCSG00497 folder>` to start from the Vita's lossless versions of every picture it has
        alike - all 676 on 1.0.1's data - with `<ios hash>.cralias` files so each upscale also
        serves the iOS picture: the game finds them with or without Vita Assets), then `tools/hd_textures.py --source <that folder>
        --out ~/.local/share/preservation/ChaosRingsHD/hd-textures [--esrgan <realesrgan-ncnn-vulkan
        folder>]` (2× by default, up to 4096 px). `--engine auto` (default) picks per picture by how
        the export says the game stored it: waifu2x CUnet (`waifu2x-ncnn-vulkan` on the PATH, or
        `--waifu2x`) for lossless ones (no denoise) and PVRTC ones (denoise 1), Real-ESRGAN x4plus for
        16-bit ones (it smooths their dithering away; waifu2x keeps it). On the Vita's lossless art
        halved and brought back, waifu2x CUnet measured 33.1 dB PSNR / 0.968 SSIM against Real-
        ESRGAN's 27.7 / 0.919 (Lanczos 30.5 / 0.945): Real-ESRGAN draws outlines and reshapes small
        details that aren't there; waifu2x is also ~15× faster. `--engine waifu2x` / `esrgan` puts
        everything through one; `--noise n` sets waifu2x's level, `--model` Real-ESRGAN's
        (`realesrgan-x4plus`, `-anime` crisper but flattens soft shading, `realesr-animevideov3`
        softest). Colour goes through the network, alpha is resampled from the original after
        see-through pixels take their neighbours' colour. `CR_HD_TEXTURES` points the engine
        elsewhere. Takes effect from the next room; pictures exported before the storage flags
        (older `.crraw`s) take waifu2x denoise 1 - export again for the per-kind choice
      - **Smooth Models** (Graphics page: Off / Smooth / Smoother): PN triangles at load time
        (`scene/smooth.cpp`) for characters, NPCs and monsters - 4× or 9× triangles on cubic
        patches through the original vertices, normals from the faces with a 60° crease, creases
        and mesh borders kept straight, UVs / colours / bone weights interpolated so animation is
        unchanged. `crtool smooth <dataHD> [2|3]` checks all 335 models: no new open edges, the
        original vertices unmoved, weights summing to 1, the rest pose skinning back onto the
        smoothed points, bounding boxes growing at most 2.3%
      - Command line: `--original`, `--resolution 1920x1080`, `--windowed` / `--fullscreen` /
        `--borderless`, `--widescreen`, `--whole-paintings`, `--zoom-paintings`, `--render-scale 150`, `--msaa 4`,
        `--upscaler off|fsr|dlss`, `--upscale-mode native|quality|balanced|performance|ultra`,
        `--sharpness 0-100`, `--hd-textures`, `--smooth-models 2|3`, `--no-vsync`; captures and the self-test start from the original
        presentation. `--capture … --settle n` draws the captured moment n times first (the
        upscaler's history converges), `--settle-moving n` draws n frames 1/30 s apart
- [x] C++ LZW + MCT/PVRTC decoders — match the Python tools on all formats, 441/441 decode in ~1 s
- [x] Engine: 3D renderer (GLES1.1 fixed-function emulation in shaders), figure system, audio
- [ ] Platform bring-up: Windows, macOS, Android packaging
- [x] Game logic: field, battle, menus, saves
