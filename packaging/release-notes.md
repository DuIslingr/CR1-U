# CR1-U v0.4.5

A from-scratch reimplementation of *Chaos Rings* for Windows, macOS, Linux and Android.

## What's new

- **FSR 3.1.6:** the built-in FSR is updated from 3.1.4 to AMD's FSR SDK 2.4 passes.
- **FSR 4 (Linux and Windows, AMD GPUs):** a new Upscaler choice. AMD ships FSR 4 for Vulkan only as
  its own Windows DLLs, so they aren't included: put AMD's two SDK DLLs, the driver's
  `amdvkffx64.dll` and (on Linux) ffx-vk-linux's `libffx_vk_linux.so` into `fsr4/` in the game
  folder - its `README.txt` says where each comes from. On Linux the DLLs run natively through
  ffx-vk-linux, no Wine. Without the files FSR 3.1 stays as it is.
- **Cutscenes closer to the original** (the opening checked opcode by opcode against the iOS code):
  message windows fade in over 4 frames before their text and out over 5 after it, with the
  original's open / close sounds, and a script's `Open` / `Close` / fade waits / swap-ins take the
  frames the original spends on them; line breaks and page turns type at the original's pace; the
  "next" mark no longer blinks and the page counter is gone (the original has neither); `Anim2`
  starts its action over from the frame the script names; a hidden portrait leaves at once and
  fades from full; the movement tutorial's pad art shows in the phone layout too (2.2.0 draws it);
  a hidden-field `Screen` is no longer mistaken for a 255-frame fade; a script's warp within its
  area no longer runs the room's entry script a second time; a script waiting for a tap with
  `Rept` isn't cut off after 45 s; a `Util` menu call takes its frame, and `FrmAdjc2` with mode 0 turns
  the actor to the record's facing before it moves (the sailor's turn on the deck) and mode 2 moves
  without a turn, as `__EvtFrmAdjc2` and `npc_autoctl_set_fream` have them. On the deck and after: a
  camera slide, a move, a scroll and a quake take their first step on the frame the script starts them
  (the original runs the script before the field's steps); the original's one-pixel tremors show (the
  view's corner is rounded after the shake is added); `CameraPlayer` leaves a room without a painting
  on its last shot; a scene's own effects face the camera and the room's don't, as the original flags them.
- **Resuming:** leaving the game during a cutscene no longer makes the title's "resume from last
  saved point?" put you on that cutscene's painting with nothing running. The resume point is only
  what a scene's end (or an encounter) saved, as the original's is - quitting itself saves nothing.
- **Skip:** the skip button skips while you hold it and stops when you let go, as in the original.
- **Animation:** every change of motion blends over two frames, as the original does. The warp
  crystals turn on their own again, and a few models (c109, c110, c117, c209, m206, m210) get
  motions that were missing.
- **Battles:** each monster's animation slots load as the original's table says; an empty slot
  passes to the next one instead of standing still.
- **Fixes contributed by [metastigma](https://github.com/metastigma):** the player stops walking while a scene runs; an item window
  no longer answers the question before it; turning HD Textures on in settings.ini updates the menus.
- **Windows** (hotfix, same version): the first-start setup can copy and unpack your game from a
  folder whose name uses another alphabet - a Cyrillic user name on a Russian Windows, for example.

Thanks to [metastigma](https://github.com/metastigma) for those fixes and for testing. All of it was checked against the original iPad code.

## Downloads

| File | Platform |
|---|---|
| `CR1-U-windows-x64.zip` | Windows 10+ x64 |
| `CR1-U-macos-universal.zip` | macOS 11+ (Apple silicon and Intel) |
| `CR1-U-x86_64.AppImage` / `CR1-U-linux-x86_64.tar.gz` | Linux x86-64 |
| `CR1-U-android.apk` | Android 7+ |

## Getting started

1. Download the file for your system and unzip it.
2. Start `chaosrings`. If it can't find your copy of the game, it asks you for one and sets it up.
3. Play.

- **Windows:** if SmartScreen warns about an unsigned program, choose *More info*, *Run anyway*.
- **macOS:** right-click `chaosrings.app` and choose *Open* (it isn't signed).
- **Linux:** `chmod +x` the AppImage, then run it.
- **Android:** install the APK, then copy your game files over USB into
  `Android/data/local.preservation.chaosrings/files/`, in an `ios/`, `android/` or `vita/` folder.

The README's "Your copy" section lists exactly which files each release needs.

## Good to know

- **Android, updating:** from this version on, each release installs over the one before it. Every
  earlier version was signed with a different key, so coming from one, uninstall it first - one
  last time. That deletes the game files you copied in and your saves: keep a copy of the game
  files to copy back.
- **No game data is included.** You need your own copy; the port reads it at run time.
- **Tested on:** Linux (played), Windows (under Wine). The macOS and Android builds are made by CI
  but haven't been run on real hardware yet. Reports are welcome.
- **DLSS** isn't in these builds (NVIDIA's library can't ship with GPL code); you can build it
  yourself (README, Platforms).
- Licence GPL-2.0-or-later (the macOS app is GPL-3.0 because of MoltenVK). A fan preservation
  project, not affiliated with Square Enix or Media.Vision.
