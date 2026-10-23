# CR1:U — a Chaos Rings reimplementation

A from-scratch native port of *Chaos Rings* (Square Enix / Media.Vision, 2010), built by
reverse-engineering the iPad's *Chaos Rings HD* v1.0.1. It also runs the voiced iPhone release
(2.2.0), the Android release (1.1.1) and the PlayStation Vita release (PCSG00497), alone or as an add-on for their voices, music,
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
build/chaosrings
```

C++17 and SDL3 (downloaded and built by CMake when it isn't installed); Vulkan 1.3 by default,
OpenGL 3.3 / ES 3.0 with `--renderer gl` or when Vulkan can't start. See [Platforms](#platforms) for Windows, macOS
and Android.

Your copy goes in the game folder - for this build `engine/build/`, or the folder you start it
from - as `ios/`, `android/` or `vita/` ([Your copy](#your-copy)), and it starts with nothing on
the command line.

## Your copy

Nothing has to be given on the command line: put your copy in the **game folder** and start the
program - it finds it by itself. The game folder is:

| Platform | The game folder |
|---|---|
| Windows, Linux | the folder holding `chaosrings.exe` / `chaosrings` |
| Linux AppImage | the folder holding the `.AppImage` |
| macOS | the folder holding `chaosrings.app` (its `Contents/Resources/` works too) |
| Android | `Android/data/local.preservation.chaosrings/files/` on the shared storage (copy your files there over USB) |

(then the working directory too). In it, a folder for each release you have - any one is enough:

```
<game folder>/
├── chaosrings                          (or chaosrings.exe, chaosrings.app, the .AppImage)
├── ios/
│   └── ChaosRingsHD.app/               the iPad 1.0.1's .app: its dataHD/ (sys.evt, the .evt, .mct, .bin files)
│                                       - or ChaosRings.app/ (the iPhone 2.2.0, voiced: its data/),
│                                       or Payload/ChaosRingsHD.app/ (the .ipa unpacked as it is)
├── android/
│   ├── main.100.com.square_enix.chaosrings.googleplay.obb
│   └── <the 1.1.1 APK>.apk             (or its classes.dex: the OBB's table is in it)
├── vita/
│   └── PCSG00497/                      the installed title: CR1/resources/cr1.psarc, CR1/sound/*.psarc
└── textures/
    ├── export/                         written by --export-textures: the pictures to upscale (<hash>.crraw)
    └── hd/                             read by the game: the upscaled pictures (<hash>.crhd)
```

A release's folder may hold the release itself or have it one or two folders down, as above
(`ios/Payload/....app`, `vita/PCSG00xxx/`), so a copy can be dropped in as it was unpacked or
installed; the names of the folders below `ios/`, `android/` and `vita/` don't matter. A Vita copy
has to be the installed, decrypted title (an encrypted NoNpDrm dump: install it first, e.g. with
Vita3K). The release downloads come with these folders made, each with a `README.txt` saying what
goes in it.

With several, **the iOS copy is the game and the others add to it**: the Vita's voices, ATRAC9
music, scripts in eight languages and lossless art, the Android release's sharper textures and its
own art (each switched off in Options, Game: Vita Assets / Android Assets). With no iOS copy the
Vita copy alone is the game (its 960 x 544 layout), else the Android release alone (the iPhone 2.2.0's
voiced data and layout).

If nothing is found the program says where it looked. The older places still work: `dataHD/`, `data/`, `ChaosRingsHD.app/` or `Payload/` straight in the game folder.
To use a copy elsewhere: `--data <iOS data>` (or `CR_DATA`), `--vita <PCSG00497 folder>` (`CR_VITA`), `--android <folder>` (`CR_ANDROID`) - each also a folder picked in Options, Game, kept in `settings.ini`; `--vita none` / `--android none` without. Settings and saves go to the profile folder
(`~/.local/share/preservation/ChaosRingsHD/` on Linux; the same folders under `%APPDATA%\` on Windows and
`~/Library/Application Support/` on macOS), not the game folder. All four ports (CR1:U, CRO-U, CR2-U, CR3-U) look for their copies the same way.

## HD textures

Upscaled textures you make yourself, from your own copy - waifu2x or Real-ESRGAN, drawn in place of
the originals:

1. `chaosrings --export-textures` writes every picture the game draws to `<game folder>/textures/export/`
   (`<hash>.crraw`; the profile folder's `textures/export/` if the game folder can't be written;
   `--export-textures <folder>` elsewhere). Each release exports its own pictures.
2. `python3 tools/hd_textures.py <game folder>` upscales them into `<game folder>/textures/hd/`
   (`<hash>.crhd`): waifu2x (`waifu2x-ncnn-vulkan` on the PATH, or `--waifu2x <it>`) for the
   lossless and block-compressed pictures, Real-ESRGAN (`--esrgan <the folder with
   realesrgan-ncnn-vulkan and models/>`) for the 16-bit ones - or `--engine waifu2x` for all. It
   needs Python 3 with NumPy and Pillow; `--help` lists the rest (`--scale`, `--noise`, `--only` ...).
3. Start the game: with `textures/hd/` there, HD Textures is on by itself (Options, Detail, HD Textures switches
   it; `--no-hd-textures` for a run without). One `textures/hd/` can hold every release's.

An export made with Vita Assets on takes the Vita's lossless version of each picture it has
alike and leaves `<iOS hash>.cralias` files: the tool links each to that picture's upscale, so one
upscale serves the game with or without the Vita's.

`CR_HD_TEXTURES=<folder>` reads them from elsewhere; the older `hd-textures/` beside the program or in
the profile folder is still read while there is no `textures/hd/`. The `tools/hd_textures.py` is the
same in all four ports. Personal use: the outputs are made from the game's art - keep them out of
anything shared.

## Platforms

Every push is built for Linux, Windows, macOS and Android by `.github/workflows/build.yml`; the
binaries are that run's artifacts (nothing of the game is in them): an AppImage, a Windows folder,
a universal macOS `.app` and an APK. The Windows build is checked under Wine (Vulkan and OpenGL, the
same frames as Linux's); macOS and Android are built in CI but not yet played on a real machine -
MetalFX, in particular, is compiled but untested.

| Platform | Build | Renderer | The game folder (its `ios/`, `android/`, `vita/`, `textures/`) |
|---|---|---|---|
| Linux | as above | Vulkan, OpenGL 3.3; FSR 3.1, DLSS (with the SDK) | the folder holding `chaosrings`, or the AppImage |
| Windows 10+ (x64) | MSVC and Ninja from a Developer prompt, the [Vulkan SDK](https://vulkan.lunarg.com/) for Vulkan - or from Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw): `-DCMAKE_TOOLCHAIN_FILE=engine/cmake/llvm-mingw-x86_64.cmake` (runs under Wine). SDL3 and zlib are built by CMake | Vulkan, OpenGL 3.3; FSR 3.1, DLSS (MSVC, with the SDK) | the folder holding `chaosrings.exe` |
| macOS 11+ (Apple silicon and Intel) | Xcode's command-line tools, Ninja and the [Vulkan SDK](https://vulkan.lunarg.com/) (MoltenVK, copied into the `.app`); add `"-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"` for both | Vulkan (MoltenVK), OpenGL 3.3; FSR 3.1, MetalFX (macOS 13+) | the folder holding `chaosrings.app` (or its `Contents/Resources/`) |
| Android 7+ (arm64, x86-64) | the Android SDK and NDK: `git clone -b release-3.2.0 https://github.com/libsdl-org/SDL android/SDL`, then `gradle assembleRelease` in `android/` | OpenGL ES 3.0 | `Android/data/local.preservation.chaosrings/files/` on the shared storage (copy your files there over USB) |

`-DCR_GLES=ON` builds the desktop game on OpenGL ES 3.0, as Android runs it (Mesa has it): a way
to test that path without a device. Settings and saves go to SDL's profile folder:
`%APPDATA%\preservation\ChaosRingsHD\` on Windows, `~/Library/Application Support/preservation/ChaosRingsHD/` on macOS.

DLSS: the NGX headers and library from NVIDIA's [DLSS SDK](https://github.com/NVIDIA/DLSS)
(`-DCR_DLSS_SDK=<its checkout>`, or `engine/third_party/dlss/`), its runtime copied next to the
game. CI builds and links it on Linux and Windows to prove it, but never uploads those builds: NGX
is proprietary and the port is GPL, so a DLSS build is for your own use (THIRD_PARTY.md).

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

Touch (or the mouse) plays the field as the original does: put a finger down anywhere on the field
and drag - a pad appears where it went down and the leader walks the way the finger points, running
once it's pulled further out. With something in reach, the "!" button in the bottom right corner (or a
tap on the "!" over it) examines or talks.

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
  Real-ESRGAN), smoothed models; 60 frames a second (Frame Rate: 30 / 60 / 120 / 144 / unlimited)
  over the game's own 20 - the leader, NPCs, cameras, scroll and battle actors shown moving
  between its frames
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
