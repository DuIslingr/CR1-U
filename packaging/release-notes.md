# CR1-U v0.7.1

A fix release for [v0.7.0](https://github.com/DuIslingr/CR1-U/releases/tag/v0.7.0): missing text on
some Android phones, and a controller Windows stopped finding. Everything in v0.7.0's notes still
applies.

## Fixed

- **No text on some Android phones:** phones whose maker ships its own fonts, without Android's usual
  Noto fonts where CR1-U looked, showed no text at all. CR1-U now reads the phone's own font list and
  uses the fonts it names for Japanese, Chinese and Korean, then the phone's default font.
- **Flydigi controllers on Windows:** a Vader 4 Pro, wired or through its receiver, wasn't found
  since v0.7.0's SDL update. SDL 3.4 added its own Flydigi driver, which takes the controller and then
  gets no input from it. CR1-U now leaves that driver off on Windows, so the controller is read
  through XInput as before. Setting the environment variable `SDL_JOYSTICK_HIDAPI_FLYDIGI=1` turns
  SDL's driver back on. Every controller SDL finds is now written to `chaosrings.log`, so if one still
  isn't picked up, the log shows why.

### Thanks to metastigma

[metastigma](https://github.com/metastigma) reported the Flydigi problem and investigated it in their
fork ([CRO-U-metastigma#42](https://github.com/metastigma/CRO-U-metastigma/pull/42),
[#43](https://github.com/metastigma/CRO-U-metastigma/pull/43)). This release takes a narrower fix
than their development build of SDL.

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
