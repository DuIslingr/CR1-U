# CR1:U - Chaos Rings, reimplemented

CR1:U is a new, native version of *Chaos Rings* (Square Enix / Media.Vision, 2010) for Windows,
macOS, Linux and Android. It was rebuilt from scratch by studying the iPad's *Chaos Rings HD*
1.0.1, and it plays the whole game: field, story scenes, battles, menus and all 64 puzzle rooms,
checked against the original's code and a reference playthrough.

**No game data is included.** The engine is new code; it loads the game from **your own copy**
when it starts. Nothing of the game - art, audio, text, scripts or code, or anything made from
them - is in this repository or in the downloads.

- [What you need](#what-you-need)
- [Install and first start](#install-and-first-start)
- [Controls](#controls)
- [Settings worth knowing](#settings-worth-knowing)
- [Extras: Vita saves, trophies, HD textures and models](#extras)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [Credits and licence](#credits-and-licence)

## What you need

A copy of *Chaos Rings* from any of these releases. One is enough:

| Release | What to have | Notes |
|---|---|---|
| iPad *Chaos Rings HD* 1.0.1 | the `.ipa`, or its unpacked `ChaosRingsHD.app` | the release the port was built from |
| iPhone 2.2.0 | the `.ipa`, or its unpacked `ChaosRings.app` | voiced; iPhone screen layout |
| Android 1.1.1 (Google Play) | the APK and `main.100.com.square_enix.chaosrings.googleplay.obb` | sharper textures, its own art |
| PlayStation Vita (PCSG00497) | the installed game (as Vita3K installs it), or a NoNpDrm dump | voices, ATRAC9 music, scripts in eight languages, lossless art; a 960 x 544 layout |

The Vita game can also come from the Prequel Trilogy (PCSG00457): its add-on holds the game's
data and is merged in.

**Having more than one helps.** With several copies, the iOS one is the game and the others add
to it - the Vita's voices, music, languages and lossless art, the Android release's sharper
textures and art. Each add-on can be switched off in Options, Game (Vita Assets / Android
Assets). Without an iOS copy, the Vita copy alone is the game; without either, the Android
release is.

## Install and first start

1. Download the file for your system from the
   [releases](https://github.com/DuIslingr/CR1-U/releases) and unzip it.
2. Start `chaosrings`.
3. If it doesn't find your game, it asks for it. Pick your copy in the window it opens:
   - **iOS:** the `.ipa` (or any file inside the unpacked `.app`);
   - **Android:** the APK and the OBB, both at once;
   - **Vita:** Vita3K's folder with the game installed (or its `ux0`), the game's `PCSG00497`
     folder, or a NoNpDrm dump - as a folder or packed as `.zip`, `.rar`, `.7z` or `.vpk`.
     Dumps are decrypted for you, as Vita3K's install does.

   You can add several before pressing Start. The files the port needs are **copied into the
   game folder** (or the profile folder, if the game folder can't be written), so the originals
   can be moved or deleted afterwards.

- **Windows:** if SmartScreen warns about an unsigned program, choose *More info*, *Run anyway*.
- **macOS:** right-click `chaosrings.app` and choose *Open* (it isn't signed).
- **Linux:** `chmod +x` the AppImage, then run it.
- **Android:** install the APK. The first start's setup works on the phone too (it picks files;
  folders are copied in over USB). Or copy your files over USB into
  `Android/data/local.preservation.chaosrings/files/`, in an `ios/`, `android/` or `vita/` folder.

### Putting the files in by hand

Instead of the setup you can place your copy in the **game folder** yourself - the folder that
holds `chaosrings` / `chaosrings.exe`, the `.AppImage` or `chaosrings.app` (on Android, the
folder above). The release downloads already have these folders, each with a `README.txt`:

```
<game folder>/
├── ios/        ChaosRingsHD.app/ (iPad 1.0.1), ChaosRings.app/ (iPhone 2.2.0), or Payload/... as unpacked
├── android/    the OBB and the 1.1.1 APK (or the APK's classes.dex)
├── vita/       PCSG00497/ as installed - or a NoNpDrm dump's app/ (and addcont/), decrypted on the next start
├── fsr4/       optional: AMD FSR 4's files (see Settings)
├── textures/   optional: export/ and hd/ for HD textures
└── models/     optional: export/ and hd/ for HD models
```

Each release may sit one or two folders down (as it was unpacked or installed), and the folder
names below `ios/`, `android/` and `vita/` don't matter. Older places still work too: `dataHD/`,
`data/`, `ChaosRingsHD.app/` or `Payload/` straight in the game folder.

For a copy kept elsewhere, use `--data <iOS data>`, `--vita <PCSG00497 folder>` or
`--android <folder>` (or the variables `CR_DATA`, `CR_VITA`, `CR_ANDROID`), or pick the folder in
Options, Game; `--vita none` / `--android none` start without that add-on. If nothing is found,
the program tells you where it looked.

**Settings and saves** live in your profile folder, not the game folder:
`~/.local/share/preservation/ChaosRingsHD/` (Linux), `%APPDATA%\preservation\ChaosRingsHD\`
(Windows), `~/Library/Application Support/preservation/ChaosRingsHD/` (macOS).

All four ports (CR1:U, CRO-U, CR2-U, CR3-U) find their copies the same way.

## Controls

A controller plays as the Vita release does. The face buttons go by position, so on an Xbox pad
○ is B, × is A, △ is Y and □ is X. ○ confirms and × cancels, as in the Japanese release;
Options, Game, "Swap ○ and ×" turns that round. Tested with a Steam Controller (through Steam
Input).

| Button | On the field | In menus and dialogue | In puzzle rooms |
|---|---|---|---|
| Left stick / D-pad | walk (the stick pushed far runs) | move the cursor (held, it repeats) | walk |
| ○ | examine / talk | confirm, turn the page | examine |
| × | - | cancel, back | - |
| △ | open the Camp (the Vita copy: the corner bar - D-pad picks, ○ presses, × shuts) | close the Camp | open the Camp |
| □ | - | skip a scene's text (until a choice) | use the pair's ability |
| L / R | - | previous / next page | - |
| START | Options | close Options | restart the puzzle |
| SELECT | the corner bar (Camp, Save, Map, encounters) | - | the puzzle's help |
| Right stick / R3 | - | - | a touch pointer: the stick moves it, R3 held is the finger (lift and swipe blocks, tap anything) |

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
| F3 | toggle Control Hints (also in Options, Game; saved between runs) |
| Mouse | everything the original takes by touch: plates, buttons, the dialogue, the corner button; drag to push a block in puzzle rooms |

**Touch** (or the mouse) works as in the original: put a finger down anywhere on the field and
drag - a pad appears there and the leader walks the way you point, running when you pull further.
Next to something you can use, the "!" button in the bottom right corner (or a tap on the "!"
above it) examines or talks.

## Settings worth knowing

The defaults are the original's presentation, drawn at 60 frames a second; Options changes it.
A few highlights:

- **Game:** Language, Field Speed and Battle Speed (Off or 2x - 5x; walking and battles only -
  scenes, talk and menus keep their pace; `--field-speed n` / `--battle-speed n` for one run),
  Swap ○ and ×, Control Hints, Vita / Android Assets, Import Vita Saves, the Instruction Manual.
- **Display:** resolution and window mode, 4:3 or Wide (the menus stay at the screen's edges, as
  the later widescreen release does), Layout (Auto, iPad, iPhone or Android screens), **Painted
  Rooms** (Original, or Fill to zoom the painted rooms to cover a wide screen), VSync and **Frame
  Rate** (30 / 60 / 120 / 144 / unlimited - the game runs at its own 20 steps a second and the
  port draws the movement in between). "Original Presentation" puts it all back.
- **Detail:** HD Textures, HD Models, Smooth Models (rounder silhouettes), anisotropic filtering,
  Smooth or Sharp texture filtering.
- **Graphics** (Vulkan): Render Scale, anti-aliasing (MSAA, FSR Native AA, DLAA) and an
  **Upscaler** - FSR 3.1 (built in), FSR 4, DLSS or MetalFX - with Upscale Mode and Sharpness.

Notes on the upscalers:

- **FSR 4** (Linux and Windows, AMD RX 6000 series or later) needs AMD's own files in the `fsr4/`
  folder; AMD's licence doesn't let the port include them. Its `README.txt` says where each comes
  from; on Linux they run natively through [ffx-vk-linux](https://github.com/DuIslingr/ffx-vk-linux),
  no Wine. Without them you have FSR 3.1.
- **DLSS** isn't in the downloads: NVIDIA's library is proprietary and can't ship with GPL code.
  You can build it in for your own use (see [Building from source](#building-from-source)).
- **MetalFX** (macOS 13+) is built but hasn't been tried on a real Mac yet.

With Vulkan, the profile folder keeps `vulkan-pipelines.bin` so later starts and new screens don't
compile the graphics pipelines again. It is safe to delete; a new GPU or driver starts it afresh.

## Extras

**Vita saves.** Bring your Vita progress over: on the title screen, Options, Game, **Import Vita
Saves**, then choose the decrypted, exported save folder and confirm. Your current saves are
backed up first, and no Python is needed. CR1 supports level 100. Details and an optional
command-line converter: [docs/VITA_SAVES.md](docs/VITA_SAVES.md).

**Trophies.** The Vita release's 16 trophies unlock as you play, whichever copy is the game, with a
notification like the Vita's. Their names and pictures come from the Vita copy when you have it.

**HD textures.** Make upscaled textures from your own copy and the game draws them instead:

1. `chaosrings --export-textures` writes every picture to `textures/export/`.
2. `python3 tools/hd_textures.py <game folder>` upscales them into `textures/hd/` with waifu2x or
   Real-ESRGAN (needs Python 3 with NumPy and Pillow). Any upscaler or image editor works too:
   save the result under the same name.
3. Start the game: with `textures/hd/` present, HD Textures turns on (Options, Detail).

**HD models.** `chaosrings --export-models` writes every model as glTF (`.glb`) to
`models/export/`, ready for Blender, animations included. Edit one, keeping its armature, save it
to `models/hd/<package>.glb` and switch on Options, Detail, HD Models.

Both are for personal use: what you make comes from the game's art, so keep it out of anything you
share. The rules (sizes, names, the Vita's `.cralias` links, what the model loader reads) are in
[docs/HD_ASSETS.md](docs/HD_ASSETS.md).

## Troubleshooting

- **"No copy found" / the setup opens every time:** check the folder layout above; the program
  lists where it looked. A release can be one or two folders down, not deeper.
- **A Vita dump isn't picked up:** a dump in `vita/` is decrypted on the next start and needs room
  for the decrypted copy. An archive (`.zip`, `.rar`, `.7z`, `.vpk`) has to go through the setup.
- **Vulkan doesn't start:** the game falls back to OpenGL by itself; `--renderer gl` forces it.
- **FSR 4 isn't offered:** Options, Graphics says why next to the Upscaler, and the game's log
  says which FSR AMD's files picked; without AMD's driver DLL only FSR 3.1 is available.
- **An HD texture doesn't show:** a picture whose shape doesn't fit is skipped, and the log says
  which and why (see [docs/HD_ASSETS.md](docs/HD_ASSETS.md)).
- **Android, updating from v0.4.5 or earlier:** those builds were signed with a different key -
  uninstall the old version first, once. That removes the copied game files and your saves, so
  keep a copy of the game files.
- **Tested on:** Linux (played) and Windows (under Wine). The macOS and Android builds are made by
  CI but haven't been run on real hardware yet - reports are welcome.

## Building from source

```sh
cd engine
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
build/chaosrings
```

You need a C++17 compiler; CMake downloads and builds SDL3 if it isn't installed. Vulkan 1.3 is
the default renderer (OpenGL 3.3 / ES 3.0 with `--renderer gl`, or when Vulkan can't start). Put
your copy in `engine/build/` (or wherever you start it from) as described above.

Every push is built by CI (`.github/workflows/build.yml`) for Linux (AppImage), Windows, macOS
(universal `.app`) and Android (APK). The Windows, macOS and Android builds, DLSS, FSR 4,
`-DCR_GLES=ON`, the tools, the self-test (`--selftest`), the self-playing `--explore` and
`--capture` are covered in [docs/ENGINE.md](docs/ENGINE.md#building).

| Document | What's in it |
|---|---|
| [docs/ENGINE.md](docs/ENGINE.md) | building in detail, the tools, debugging switches, and the full status list of what the port does |
| [docs/HD_ASSETS.md](docs/HD_ASSETS.md) | HD textures and models in detail |
| [docs/FORMATS.md](docs/FORMATS.md) | the game's file formats and systems, as reverse-engineered |
| [docs/ANDROID.md](docs/ANDROID.md) | the Android release's APK and screen layout |
| [docs/VITA_SAVES.md](docs/VITA_SAVES.md) | importing Vita saves |

Repository layout: `engine/` (the C++17 / SDL3 engine and `crtool`), `docs/`, `tools/` (analysis
and extraction tools that work on your own copy), `android/` and `packaging/`. Local
reverse-engineering output (`re/`, `extracted/`) is never committed or shared.

## Credits and licence

- By DuIslingr, with fixes and testing by [metastigma](https://github.com/metastigma).
- *Chaos Rings* is by Square Enix and Media.Vision. This is a fan preservation project, not
  affiliated with either. The engine's source holds facts read out of the original (file names,
  flag numbers, screen positions, formula constants) where the port needs them, but no game data.
- Licence: GPL-2.0-or-later ([LICENSE](LICENSE)). The bundled libraries keep their own licences
  ([THIRD_PARTY.md](THIRD_PARTY.md)); the macOS app is GPL-3.0 because of MoltenVK. A build with
  DLSS links NVIDIA's proprietary library and is for your own use only.
