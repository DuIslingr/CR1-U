# CR1-U v0.7.0

A from-scratch reimplementation of *Chaos Rings* for Windows, macOS, Linux, Android and now iPhone /
iPad.

## What's new

The big ones: an **iPhone / iPad build**, a **Cheats page**, and another long run of fidelity rounds:
battles, the field, menus, scenes, sounds and timings checked against Chaos Rings' own iPhone 2.2.0
code, one part of the game at a time.

### iPhone and iPad

- **A new download:** `CR1-U-ios-unsigned.ipa` (iOS 15+). It's unsigned: sideload it with AltStore,
  Sideloadly or the like, which sign it with your Apple ID.
- It does everything the Android build does. The first start's setup offers a file picker with
  progress bars. A Vita NoNpDrm dump (even still in its `.zip` / `.rar`) is unpacked and decrypted.
  A hand-placed `.ipa` is unpacked.
- The app's folder shows in the Files app (*On My iPhone / iPad > Chaos Rings*) and in Finder, so
  you can copy your game in, add fonts or textures, and read `chaosrings.log`.
- Text uses the system's own fonts, as 2.2.0 does (ArialMT and Hiragino; PingFang or Apple SD
  Gothic for Chinese and Korean). The app has its own icon.
- **Foldables (the iPhone Duo):** the game fills the inner and the outer screen and changes when you
  fold or open the phone. Built with Xcode 27.1, because an app built with an older SDK is shown in
  a box on the Duo's screens.
- **The camera's punch hole:** the picture fills the whole screen, and only the buttons, windows and
  text keep clear of it. Options' *Camera Cutout* (Avoid / Ignore) turns that off.
- Phones (iOS and Android) have no Resolution, Fullscreen or Aspect options: the game takes the
  screen's own shape.
- Switching apps and coming back no longer leaves a frozen picture.

### Cheats

A new Options page, all off until you turn them on:

- **Infinite HP**, **Infinite MP**, **Max Oz** (9,999,999)
- **EXP Multiplier** 1x, 2x, 3x, 5x or 10x
- **Always First Strike**, **No Random Battles** (story battles still come)
- **One-Hit Kills**, **Full Break Gauge** (the Break is yours and full from the start)

### Battles

- Cameras, attack spots, hit labels, number slots, bars, reactions, voices and effects as 2.2.0's.
- The rules were checked again, action by action. That covers targets worked out together,
  counters, Invincible, Risk Breaker, the Oz theft, revives, MP, dodges, the Break, and the win or
  loss before a round ends.
- Battle menus as 2.2.0's: the gene and item lists scroll, flick and snap like the original's. The
  escape question sits in the banner, and Back and Defend behave as the original's.
- Monster attacks run as long as 2.2.0's counters say, not as long as their animation clips.

### Field, scenes and menus

- Paintings, the "!" bubbles, NPC floors, shadows and turns, the camera's ease and the story
  scripts follow 2.2.0's code more closely.
- The Camp, the shop, World Select, Options, Scenarios, the map and the Instruction Manual change
  pages, slide and wait as 2.2.0's do. Buttons act as a touch lifts and stay lit while held.
- Menu and message text is sized and placed as 2.2.0 draws it.
- Puzzle rooms: double taps, psychokinesis swipes, sliding blocks and the tap's window as the
  original's.
- Sounds as 2.2.0's: gene chimes, poison ticks, area sounds, the cursor.
- Wide screens: the Ark hall's door model sits in its arch again (the painting was drawn off its 3D
  view). Menus and windows no longer overlap the camera cutout or each other.

### Controller

- **Hot-plug:** a controller connected or disconnected mid-game is picked up at once.
- The Vita release's controls: L / R keep the field's map, × never quits, battle cursors are kept,
  and the Scenarios chart pans while the stick is held.

### Setup

- **Unpacked with a progress window, on the same start:** a hand-placed `.ipa` in `ios/`, an
  `.xapk` / `.apks` / OBB `.zip` in `android/`, or a Vita dump archive in `vita/`. *Look again* checks
  without restarting.
- **libarchive 3.8.11:** Vita dump `.rar` files that 3.7.7 called "a broken entry" now unpack.
- **A clean-room Vita PFS decrypter** (from CR3-U) replaces psvpfsparser, which has no licence. It
  gives the same files, and a fix to its AES makes decryption work on Apple's compiler (iOS, macOS).
- **`font/` folder:** fonts you put there are used instead of the system's (a CJK font for Japanese,
  Chinese and Korean, a "bold" one for bold text).
- **The Russian fan translation** by metastigma is a language of its own. Put its archive into
  `translation/` and pick it in Options. English stays as it is. The README's "Fan translation"
  section explains.
- Vita textures in UBC1/2/3 (DXT) and bare GXT files are read.
- Desktop: SDL's log is written to `chaosrings.log` in the profile folder (the last start's kept).

### Desktop

- **F11** and **Alt+Enter** switch fullscreen.
- SDL 3.4.20, Vulkan SDK 1.4.363.1 and MoltenVK 1.4.2.

## Thanks to metastigma

Thanks to [metastigma](https://github.com/metastigma), whose work runs through this release:

- **Their Russian translation**
  ([the ZoG thread](https://forum.zoneofgames.ru/topic/72457-chaos-rings/)) is now a language of its
  own: [bfbdfbf](https://github.com/DuIslingr/CR1-U/commit/bfbdfbf). Their redrawn art is read too,
  stored in Vita texture formats the port couldn't decode before:
  [dc8e8fe](https://github.com/DuIslingr/CR1-U/commit/dc8e8fe).
- **From reviewing their CRO-U fork** ([CRO-U-metastigma](https://github.com/metastigma/CRO-U-metastigma)):
  the controller hot-plug, after their Windows XInput work, and F11 / Alt+Enter, which their fork
  had first. Both came here with [7ffec48](https://github.com/DuIslingr/CR1-U/commit/7ffec48).

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
