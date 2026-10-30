# CR1-U v0.4.3

A from-scratch reimplementation of *Chaos Rings* for Windows, macOS, Linux and Android.

## What's new

- **Saves:** written safely. A save cut short, or naming an area that doesn't exist, is refused instead of crashing, and damaged party numbers are held in range.
- **Play time:** resets at the title, Reset Data and a new game.
- **Controller:** a button held while the window loses focus, or as a session ends, does nothing until it's let go.
- **Background:** sound and graphics survive the app going to the background.
- **Windows:** a PC without Vulkan no longer refuses to start the game; it goes on to OpenGL.

The game was played to its final ending by the built-in autoplayer. A real Windows PC without Vulkan and physical Android devices are still untested.

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

- **No game data is included.** You need your own copy; the port reads it at run time.
- **Tested on:** Linux (played), Windows (under Wine). The macOS and Android builds are made by CI
  but haven't been run on real hardware yet. Reports are welcome.
- **DLSS** isn't in these builds (NVIDIA's library can't ship with GPL code); you can build it
  yourself (README, Platforms).
- Licence GPL-2.0-or-later (the macOS app is GPL-3.0 because of MoltenVK). A fan preservation
  project, not affiliated with Square Enix or Media.Vision.
