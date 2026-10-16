# CR1:U — a Chaos Rings reimplementation

A from-scratch native port of *Chaos Rings* (Square Enix / Media.Vision, 2010), built by
reverse-engineering the iPad's *Chaos Rings HD* v1.0.1. It also runs the voiced iPhone release
(2.2.0) and the PlayStation Vita release (PCSG00497), alone or as an add-on for their voices, music,
languages and lossless art. The engine code here is new; it loads the game from **your own copy**
at runtime.

**No game data is in this repository or distributed with it** - no art, audio, text, scripts or
code of the game, and nothing generated from them. You need your own copy of the game. The
engine's source does hold facts read out of the original (file names, flag numbers, screen
positions, formula constants) where the port needs them.

Licence: GPL-2.0-or-later (`LICENSE`); the bundled libraries keep their own (`THIRD_PARTY.md`).

## Quick start

```sh
cd engine
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
build/chaosrings --data "<path to>/ChaosRingsHD.app/dataHD"
```

C++17 and SDL3 (downloaded and built by CMake when it isn't installed); Vulkan 1.3 by default,
OpenGL 3.3 / ES 3.0 with `--renderer gl` or when Vulkan can't start. Builds and runs on Linux; the
code is portable to Windows, macOS and Android but not yet tested there.

The game's data: `--data <dir>`, else `$CR_DATA`, else `dataHD/` beside the executable, else
`dataHD/`, `ChaosRingsHD.app/dataHD/` or `Payload/ChaosRingsHD.app/dataHD/` under the current
directory (so it runs from an unpacked .ipa). Any of these works as the game:

| Your copy | How |
|---|---|
| iPad *Chaos Rings HD* 1.0.1 | `--data …/ChaosRingsHD.app/dataHD` |
| iPhone *Chaos Rings* 2.2.0 (Japanese voices, eight languages, the iPhone layout) | `--data …/ChaosRings.app/data` |
| Vita PCSG00497, alone | `--data <PCSG00497 folder>` |
| Vita as an add-on (voices, ATRAC9 music, scripts in eight languages, lossless art) | `--vita <PCSG00497 folder>`, or Options, Game, Vita Assets |
| Android as an add-on (its expansion file's sharper textures and its own art) | `--android <folder with main.*.obb and the APK>`, or Options, Game, Android Assets |

Settings and saves go to the profile folder (`~/.local/share/preservation/ChaosRingsHD/` on
Linux); `CR_SAVE_DIR` moves the settings.

## Controls

A controller plays as the Vita does (its manual). The face buttons are by position, so on an Xbox
pad ○ is B, × is A, △ is Y and □ is X. ○ confirms and × cancels, as in the Japanese release;
Options, Game, "Swap ○ and ×" turns that round. Tested with a
Steam Controller (through Steam Input).

| Button | On the field | In menus and dialogue | In puzzle rooms |
|---|---|---|---|
| Left stick / D-pad | walk (the stick pushed far runs) | move the cursor (held, it repeats) | walk |
| ○ | examine / talk | confirm, turn the page | examine |
| × | - | cancel, back | - |
| △ | open the Camp | close the Camp | open the Camp |
| □ | - | skip a scene's text (until a choice) | use the pair's ability |
| L / R | - | previous / next page | - |
| START | Options | close Options | restart the puzzle |
| SELECT | the corner bar (Camp, Save, Map, encounters) | - | the puzzle's help |

| Keyboard / mouse | |
|---|---|
| W A S D (Shift runs) | walk; in menus, the cursor as the arrow keys |
| E / Enter / Space | examine, confirm, turn the page |
| Esc / Backspace | cancel, back, answer No (on the field itself Esc quits) |
| K | the Camp |
| R | the corner bar; J switches this pair's encounters once skill 170 is known |
| X | skip a scene's text |
| V | Options |
| PgUp / PgDn (Q / E in Options) | pages |
| T, F5, F6 | puzzle rooms: the ability straight ahead, retry, help |
| F3 | the status line and key help (hidden in Release builds) |
| Mouse | everything the original takes by touch: plates, buttons, the dialogue, the corner button; drag to push a block in puzzle rooms |

## What it does

The whole game plays, checked against the original's code (Ghidra decompiles of 1.0.1, 2.2.0 and
the Vita build) and a reference playthrough:

- **Field**: the painted rooms at the iPad's 2× with the window following the player, the 3D rooms,
  walking and collision, exits, NPCs and treasure, the event scripts (all 27,010 decode and run)
  with their cutscenes, portraits, message windows, choices, scene effects and quakes
- **Battles**: the original's rules (damage, criticals, elements, the Break, genes and skills,
  every action calc, the AI), the bosses' staged attacks, the story battles' talks and line-ups,
  effects, cameras and the HUD; random encounters as the original counts them
- **Menus**: title, Character Select, save / load (a slot per pair, clear data), Camp (items,
  equipment, genes, status), shops, the World Select, Scenarios, the field menu and map, the
  staff roll and the last battle's pair choice, the instruction manual, Options
- **Puzzle rooms**: all 64 stages (teleport, psychokinesis, toughness, pyrokinesis)
- **The other releases**: 2.2.0's voices, languages and iPhone layout; the Vita's voices, music,
  scripts, sound bank and art, as an add-on or as the game
- **Presentation options** (the defaults are the original's): resolution, fullscreen, 4:3 or
  widescreen (the menus' groups kept to the screen's edges, as the later widescreen release does),
  render scale, MSAA, FSR 3.1 / DLSS, optional HD textures you make yourself (waifu2x /
  Real-ESRGAN), smoothed models
- **Testing**: `--selftest` checks every subsystem against the original's rules and data;
  `--explore n` plays the game by itself from the title through all four pairs' stories and the
  last battle to the final ending; `--capture` renders any room, screen or battle moment

Not yet: Windows, macOS and Android packaging.

## Documentation

| | |
|---|---|
| `docs/ENGINE.md` | building in detail (renderers, music decoding, upscalers, platforms), the tools, the asset viewer and debugging switches, and the full status list |
| `docs/FORMATS.md` | the game's file formats and the original's systems as reverse-engineered |
| `docs/ANDROID.md` | the Android release's APK: what it holds, its screen layout (`tools/re_setup_android.sh`) |

## Layout

| Path | Purpose |
|---|---|
| `engine/` | the C++17 / SDL3 engine and `crtool` |
| `docs/` | engine and format notes |
| `tools/` | analysis / extraction tools; they work on your own copy |
| `re/`, `extracted/` | (not in the repository) local RE output and decoded assets - never commit or share |
