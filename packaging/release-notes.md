# CR1-U v0.7.2

A fix release for [v0.7.1](https://github.com/DuIslingr/CR1-U/releases/tag/v0.7.1): Flydigi controllers on
Windows, take two, and battle models that could look speckled on some graphics cards. Everything in
v0.7.0's and v0.7.1's notes still applies.

## Fixed

- **Flydigi controllers on Windows, take two:** v0.7.1's fix didn't bring the Vader 4 Pro back. The
  Windows build now uses SDL's development version from 2026-11-14, which has SDL's later Flydigi
  fixes. That's the setup metastigma's fork uses, and its user confirmed it works with this
  controller, wired and through its receiver. v0.7.1's workaround is gone. Other platforms keep
  SDL 3.4.20.
- **Battle models no longer turn speckled and see-through on some graphics cards.** A battle model
  draws parts of itself twice, and the second copy only keeps pixels that are fully opaque. CR1-U
  checked that with an exact comparison, while the original allows for rounding. On GPUs that land
  a hair short, scattered pixels were dropped and the sky showed through. The check now matches the
  original's, in both the Vulkan and the OpenGL renderer. (Found in the Omega port, CRO-U #5; CR1-U
  had the same check.)
- **Android fonts:** when CR1-U reads the phone's own font list (new in v0.7.1), a font family the
  phone maker commented out is no longer used, and the font folders are searched in name order, so
  the same phone always picks the same font.

### Thanks to metastigma

[metastigma](https://github.com/metastigma) worked out the Windows SDL setup in their fork
([CRO-U-metastigma#42](https://github.com/metastigma/CRO-U-metastigma/pull/42)) and reported the
speckled battle models in the Omega port.

## Downloads

| File | Platform |
|---|---|
| `CR1-U-windows-x64.zip` | Windows 10+ x64 |
| `CR1-U-macos-universal.zip` | macOS 11+ (Apple silicon and Intel) |
| `CR1-U-x86_64.AppImage` / `CR1-U-linux-x86_64.tar.gz` | Linux x86-64 |
| `CR1-U-android.apk` | Android 7+ |
| `CR1-U-ios-unsigned.ipa` | iPhone / iPad, iOS 15+ (sideload) |

## Getting started

1. Download the file for your system and unzip it.
2. Start `chaosrings`. If it can't find your copy of the game, it asks you for one and sets it up
   (on Android and iOS too).
3. Play.

- **Windows:** if SmartScreen warns about an unsigned program, choose *More info*, *Run anyway*.
- **macOS:** right-click `chaosrings.app` and choose *Open* (it isn't signed).
- **Linux:** `chmod +x` the AppImage, then run it.
- **Android:** install the APK and start it to pick your files, or copy them over USB into
  `Android/data/local.preservation.chaosrings/files/`, in an `ios/`, `android/` or `vita/` folder.
- **iPhone / iPad:** sideload the `.ipa` and start it to pick your files. Or copy them into the
  app's folder in the Files app (or Finder's Files tab), in an `ios/`, `android/` or `vita/` folder,
  then tap *Look again*.

The README's "What you need" lists which releases of the game work and what each one needs.

## Good to know

- **No game data is included.** You need your own copy; the port reads it at run time.
- **Cheats** are saved in `settings.ini` with your other options, not in your saves.
- **Tested on:**
  - Linux: played.
  - Windows: under Wine.
  - iOS: an iPhone Duo (setup, Vita dump unpacking and decryption, both screens).
  - macOS and Android: built by CI, not yet run on real hardware. Reports are welcome.
- **DLSS** isn't in these builds (NVIDIA's library can't ship with GPL code); you can build it
  yourself (README, Building from source).
- Licence GPL-2.0-or-later (the macOS and iOS apps are GPL-3.0 because of MoltenVK). A fan
  preservation project, not affiliated with Square Enix or Media.Vision.
