# CR1-U v0.5.0

A from-scratch reimplementation of *Chaos Rings* for Windows, macOS, Linux and Android.

## What's new

The big one: the fixes made for the Omega port (CRO-U) are now in CR1-U too, each one checked
against Chaos Rings' own iPhone 2.2.0 code - and left out where Chaos Rings does it differently.
Field, battles, menus, puzzle rooms and story scenes all look and behave closer to the original.

### Story scenes and messages

- Messages page and type as the original's: a tap jumps to the next stop, pauses take the frames
  the script names, and the "next" mark and skip button are drawn as the original draws them.
- The phone layouts' parchment message window is laid out as the original's; side portraits and
  the name streak sit where 2.2.0 puts them.
- Cutscenes follow the original more closely: windows fade in and out with their sounds, waits,
  moves, scrolls and quakes start on the right frame, and the opening was checked step by step.
- A scene's warp no longer runs the room's entry script twice, and a scene waiting for a tap
  isn't cut off after 45 seconds.

### Battles

- Battles look as in 2.2.0: the opening, shadows, turning, casting, banners, the command pages
  sliding in, the eased camera and the result screen.
- The battle rules were checked again: who acts together and in what order, counters,
  Invincible, poison, the Break gauge, Steal and the Gate skills now match the original.
- Battles started by a scene: Escape is refused while the scene runs, monsters level by the
  route's World Select band, and the music, fades and defeat screen follow the original.
- The victory and every way out of a battle fade as the original's do.

### Field and menus

- Walking uses the original's collision: ramps climb smoothly, walls push only when faced, and
  chests and other solid objects block where they stand.
- Field rendering as 2.2.0's: soft shadows, model colours, see-through models, the camera's near
  plane and the 2D drawing order; the player is drawn before the NPCs, so a see-through NPC no
  longer hides him.
- The Camp, the shop, World Select, the save screen, the field map and the Instruction Manual are
  drawn and run as in 2.2.0; menu text is edged like the original's and pressed buttons glow cyan.
- The title shows its own art again (logo, "Please Touch", credits strip) before its menu slides in.
- Options: the volume gauges fill blue, the arrows on the Back bar turn the page, and Return to
  Title asks first. The "Option" notice, once seen, stays seen even before you save.
- Puzzle rooms: touches, swipes and taps as the original's, arrows on a lifted block, and the
  clouds in the right place on phone screens.
- Touch play: battle plates and Yes / No stay unlit until tapped, as on iPhone.
- **Painted Rooms** now offers Original or Fill (zoom to cover a wide screen); the old Whole and
  Fill Width choices are gone and read as the nearest of the two.

### Controller

- With the Vita copy as the game, △ opens the Vita's corner bar on the field: the D-pad moves
  round it, ○ presses, × shuts it.
- Puzzle rooms: the right stick moves a touch pointer and R3 is the finger, so blocks can be
  lifted and swiped without a mouse or touch screen.
- Holding X on the keyboard now skips a battle's talk too.

### Saves and trophies

- **Trophies whatever data is the game:** the Vita's 16 trophies unlock with the iOS or Android
  data as well, not only with the Vita copy (names and pictures from the Vita copy when present).
- **Opened treasure boxes are stored once.** A box opened again used to be added to the save
  each time, slowly growing it; existing saves shrink back the next time they're written.
- "New game with clear data" no longer stalls before the first room.
- Resuming after quitting during a cutscene puts you at the last saved point, not on a frozen
  scene.

### Graphics and performance

- **Vulkan pipeline cache:** compiled pipelines are kept between runs (`vulkan-pipelines.bin` in
  the profile folder), so starting and new screens hitch less.
- **FSR 4** (Linux and Windows, AMD GPUs) as a new Upscaler choice once AMD's files are in `fsr4/`
  (they can't be included - the folder's `README.txt` says where to get them). The built-in FSR is
  updated to 3.1.6.
- On phones the picture fills the screen under the camera hole while menus keep to the safe area.

### Setup and Android

- The first start's setup works on Android too: pick your files on the phone, or copy folders in
  over USB.
- A Vita NoNpDrm dump copied into `vita/` is decrypted on the next start, on a phone as well.
- Android builds are now signed with one release key, so future versions install over this one.
- Android: no green focus frame around the game after using a controller or keyboard.

### Fixes

- Two textures with the same id: the later one is used, as in the original (wrong textures on
  some models).
- Field events as 2.2.0's: examine windows, which NPCs show after loading, and the return to the
  Ark after a lost battle.
- Menu pictures no longer pick up a grey edge from their neighbours at window sizes that aren't a
  whole multiple of the layout.

## Thanks to metastigma

Thanks to [metastigma](https://github.com/metastigma) for code, testing and a fork that keeps
finding things:

- Contributed fixes (first shipped in v0.4.5): the player stands still while a scene runs, the
  item window no longer answers the question before it, and turning on HD Textures in
  settings.ini updates the menus.
- Pointed out by their fork and fixed here: holding X to skip a battle's talk, opened treasure
  boxes growing the save, the Vita's field bar on a controller, and puzzle-room touches from the
  right stick.

## Downloads

| File | Platform |
|---|---|
| `CR1-U-windows-x64.zip` | Windows 10+ x64 |
| `CR1-U-macos-universal.zip` | macOS 11+ (Apple silicon and Intel) |
| `CR1-U-x86_64.AppImage` / `CR1-U-linux-x86_64.tar.gz` | Linux x86-64 |
| `CR1-U-android.apk` | Android 7+ |

## Getting started

1. Download the file for your system and unzip it.
2. Start `chaosrings`. If it can't find your copy of the game, it asks you for one and sets it up
   (on Android too).
3. Play.

- **Windows:** if SmartScreen warns about an unsigned program, choose *More info*, *Run anyway*.
- **macOS:** right-click `chaosrings.app` and choose *Open* (it isn't signed).
- **Linux:** `chmod +x` the AppImage, then run it.
- **Android:** install the APK and start it to pick your files, or copy them over USB into
  `Android/data/local.preservation.chaosrings/files/`, in an `ios/`, `android/` or `vita/` folder.

The README's "What you need" lists which releases of the game work and what each one needs.

## Good to know

- **Android, updating:** earlier versions were signed with a different key. Coming from v0.4.5 or
  older, uninstall it first - one last time. That deletes the game files you copied in and your
  saves: keep a copy of the game files to copy back.
- **No game data is included.** You need your own copy; the port reads it at run time.
- **Tested on:** Linux (played), Windows (under Wine). The macOS and Android builds are made by CI
  but haven't been run on real hardware yet. Reports are welcome.
- **DLSS** isn't in these builds (NVIDIA's library can't ship with GPL code); you can build it
  yourself (README, Building from source).
- Licence GPL-2.0-or-later (the macOS app is GPL-3.0 because of MoltenVK). A fan preservation
  project, not affiliated with Square Enix or Media.Vision.
