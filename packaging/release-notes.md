# CR1-U v0.7.4

The port's own menus in nine languages, the fan translation as plain files, a camera cutout fix for
Android, and fixes for the crystal puzzle rooms and the boot logos. Everything in the v0.7.0 to
v0.7.3 notes still applies.

## What's new

- **Menu Language** (*Options > Game*): the port's own text (the Options pages, the setup, the Vita
  import) in English, Japanese, French, German, Spanish, Simplified and Traditional Chinese, Korean
  or Russian. *Auto* follows the game's text language, and the fan translation's language while it
  is chosen. The Russian wording is metastigma's; the others are machine translations, not yet
  reviewed, so corrections are welcome. A `lang/<code>.po` file in the game folder or the profile
  folder is used over the built-in one, so you can fix or add a language yourself.
- **The fan translation as plain files:** besides a rePatch's `cr1.psarc`, CR1-U now takes the
  translated files themselves (`sys.evt`, the areas' `.evt`, any `.gxt` art) in
  `translation/<code>/`, for example `translation/ru/`. Dropping a `.zip` of them on the setup works
  too. A line the translation doesn't have shows the game's English instead of nothing.
- **Camera Cutout: Black Bars.** A third choice next to *Avoid* and *Ignore*: the whole picture is
  boxed inside the safe area, with black beside the notch or camera hole.

## Fixed

- **Crystal puzzle rooms:** tapping a block (to teleport, or for toughness and pyrokinesis) did
  nothing, by touch or by mouse. Any touch also started the on-screen pad, which swallowed the tap.
  Now the pad only takes over once it is moved or held, as in the original
  ([#6](https://github.com/DuIslingr/CR1-U/issues/6)).
- **The notch on Android:** on some phones the interface wasn't moved clear of the notch or camera
  hole with *Avoid* on, because Android never told SDL where the cutout was. CR1-U now asks Android
  for the cutout itself, from Android 9 on.
- **Boot logos with the fan translation:** the Square Enix and Media.Vision logos were drawn small
  and off-centre when the Russian translation was chosen. They're centred again
  ([#5](https://github.com/DuIslingr/CR1-U/issues/5)).
- **Linux, AMD:** FSR 4 is about 0.3 ms faster at 4K on RADV.

### Thanks

- [metastigma](https://github.com/metastigma) asked for the Menu Language and the plain-file
  translation ([CRO-U #10](https://github.com/DuIslingr/CRO-U/issues/10),
  [CRO-U #11](https://github.com/DuIslingr/CRO-U/issues/11)), wrote the Russian menu text, and
  reported the logos ([#5](https://github.com/DuIslingr/CR1-U/issues/5)).
- [aspendable](https://github.com/aspendable) reported the puzzle rooms
  ([#6](https://github.com/DuIslingr/CR1-U/issues/6)), and an Android player reported the notch.

### Not reproduced yet

Garrick's spear in the first cutscene was reported as drawn wrong on an Android phone, but it looks
right on every setup tried here. If you see it, please open an issue with a screenshot, your phone's
model and `chaosrings.log`.

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
