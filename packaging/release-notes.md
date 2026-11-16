# CR1-U v0.7.3

A fix release for [v0.7.2](https://github.com/DuIslingr/CR1-U/releases/tag/v0.7.2): Flydigi controllers,
take three. Everything in v0.7.0's, v0.7.1's and v0.7.2's notes still applies.

## Fixed

- **Flydigi controllers, take three:** with v0.7.2's SDL update the Vader 4 Pro was found, but it
  shows up as two "Xbox 360 Controller" devices, and its input comes on the second one, which
  CR1-U never opened. Now every connected controller is opened, and the one you press a button on
  or move a stick on takes over. This also helps any other controller that shows up twice, and
  lets you switch between two controllers just by picking one up.

### Thanks to metastigma

[metastigma](https://github.com/metastigma) sent the log that showed the controller listed twice
([CRO-U #4](https://github.com/DuIslingr/CRO-U/issues/4)).

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
