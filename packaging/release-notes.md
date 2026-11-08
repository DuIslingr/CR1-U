# CR1-U v0.4.5

A from-scratch reimplementation of *Chaos Rings* for Windows, macOS, Linux and Android.

## What's new

- **Puzzle rooms' touches as the original's (iOS 2.2.0):** a lifted block shows arrows the ways it can
  go (the chosen one flickers out as it slides, the rest fade on a let-go); a swipe is decided as soon
  as the finger leaves the dead zone; teleport / toughness / pyrokinesis taps are taps (let go within
  5 frames, not while the leader moves) with the original's per-object windows; a second finger
  scrolls the view; Retry / Help and the field menu's Camp / Save wait out the original's pad lock;
  a Retry puts the leader back facing as it came in; a cleared room has no objects or buttons; the
  clouds sit at the screen's bottom in the phone layouts.
- **Vulkan: fewer hitches.** The renderer keeps the driver's compiled pipelines between runs
  (`vulkan-pipelines.bin` in the profile folder), so the start and each new screen don't compile
  them again. Measured here: about 6.5 ms of pipeline compiling the first time, 0.1 ms the next;
  slower drivers save more. A cache from another GPU or driver is ignored and rebuilt.
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
- **Battles started by a scene** (checked against the iOS code's `btl_main` and battle menu): Escape is
  dimmed and refused while a scene runs, as the original's menu has it; monsters without a level of their
  own take the route's World Select band (the dungeon's number stood in for it) and a random
  battle's track follows that band (8, or 9 from band 5) and a scene's the track it names in var 58;
  the music after a battle follows the scene's var 60 (silence after the deck's and the first
  dungeon's fights, as the original); the battle comes in out of white over the original's 20
  frames; a lost battle fades its music out over 10 frames before the defeat tune, a lost scene
  battle shows `gameover` fading in and waits for a tap before the title, and any defeat goes on
  with a tap; a victory holds ten frames while the battle's music fades before the poses and the
  tune (`btl_main` 0x3e8 / 0x3e9); every way out of a battle masks to black over ten frames with the
  music fading and leaves twelve frames later, and the field comes back out of that black over ten
  (`btl_main` 9000 / 0x238b, `FieldScene::onLoading`). (Omega's "skip off" scene command, its `battle_start` tutorial picture and its
  100 ms tutorial dim don't exist in Chaos Rings: its tutorial dim is 400 ms, as the port has it.)
- **On the field and in the Ark** (checked against the iOS code's `CheckTap`, `FieldPCCtrl`, `FollowWindow`,
  `SaveLoadPrc` and `ShopPrc`): a quick tap anywhere (let go within 5 frames) examines or talks, as the
  original's CheckTap; standing by something, the leader turns to face it (0.35 of the way each frame);
  the "!" bubble is the original's piece, set over the object's top; a scene's "Save data?" (`Util 6`)
  leads to the title whether you save or not, asking "return to the title without saving?" on a No,
  as the original - it no longer dropped you back on the field - and "Save complete." stands its 20
  frames; a sale's No in the shop plays the original's sound. (Omega's eased camera, turned trigger
  boxes, puzzle-room radius, one-at-a-time shop stock and World Select discount pair don't exist in
  Chaos Rings: its camera is bolted to the player, its trigger boxes are tested upright, and its shop
  sells any count and reads the route's own pair.)
- **Battle UI, Camp and scripts' natives** (checked against the iOS code's `btl_main`, `btlact_*`, `__menub_mainPrc`,
  `_btlord_make_select`, `__dmg_prc`, `__MuCamp_NextProc`, `npc_setup` and the `Uty_*` natives): a round ends with
  thirty frames on the formation's camera before the commands; an attack waits ten frames once its actors are
  back, a gene or item forty after its hit, a cancelled command sixty on its user's camera; Defend takes the
  guard pose as it is chosen; a party-wide target's camera is the original's; Blowout / Critical / Weak slide in
  from the right and out to the left; the Camp's item-used sound is the original's, and a No to using an item
  goes back to the list. Scripts: `NpcFade 0` fades the player's model (the materialisation in the first
  dungeon), an NPC row's native runs as the room is set up - the first dungeon's two fire walls burn until
  their puzzles are cleared (`Uty_FireWall_Init`, also callable from scripts) - and `Uty_ChkLogo_flg` sets the
  flag when var 0 is set. (Omega's top-menu sound, its Down / Ward icons, its "doubled" battle param-ups and
  its teleport countdown don't exist in Chaos Rings: its param-ups are the four Ups, as the port has them.)
- **The title, Options and the puzzle rooms** (checked against the 2.2.0 code's `__menuu_TitlePrc` / `TitlePut`,
  `__menuu_OptionPrc`, `PuzzleScene` and the puzzle objects): the title shows its own art again - the CHAOS
  RINGS logo, "Please Touch" and the credits strip, which the port never drew - alone until a tap or key
  (its sound), then the logo goes and Start / Scenarios / Options slide in; the "resume from last saved
  point?" offer comes after Start, not as the title opens, and its Yes stops the music and fades to black
  before the load; the store's picture answers a tap with its sound (there is no store). Options' Return
  to Title asks first ("return to the title?") and leaves through the original's black fade with the music
  stopping. Puzzle rooms: picking up a gem shows the original's one effect (the second belongs to a gem
  teleported away, as for a block); pyrokinesis counts a tap only twenty frames after the last; a Retry
  fades out and in over 500 ms. (Omega's "Load complete." timing, sound-effects gauge sound, last battle
  header, candle stage numbers and lifted-block sounds already were Chaos Rings' own here; Chaos Rings has
  no second scenario to give a tune back to.)
- **The battle rules** (checked against the 2.2.0 code's `btlord_*`, `btlque_*`, `btlai_prc`, `btlcalc_*`,
  `Is_invincible` and `pcparam_*`): two monsters act together only in the two-small-monster formation;
  Escape resolves before any other action of the round; speeds are read afresh for every action (a Speed
  Up or an Invincible taken mid-round moves the rest of it); a twice-acting monster chooses its second
  action when it comes up; the guest's target and turn count are the original's; an AI pick the original
  doesn't handle falls back as it does, and a fallen target is replaced as the original replaces it (an own-side
  action goes to its user, the rest to a random living opponent); with the enemy pair a blow on one of them
  hits both, as with your pair; a pair attack whose partner has fallen becomes the survivor's own attack.
  Invincible covers a pair's either member (for damage, MP loss, poison and acting first); poison and the
  HP-regaining skill are settled as one amount at the round's end with Invincible and Endure holding against it, and a
  monster poisoned to death gives its experience, Oz and drops; a fallen member loses his element; a critical
  moves the Break gauge even when the element triangle or Invincible changes the hit; a counter is the
  original's (everyone the blow hit answers everyone who struck, with the counterer's own attack; a monster's by
  its model; never against a monster's first of two actions). The Gate skills raise only their own stat (the
  Defense Gates no longer add max HP and MP); a Steal Item catch and an ability or skill a monster teaches or
  drops go into the bag at once (kept after an escape or a defeat, not listed on the result screen); a gene
  reflected back for the kill leaves no item or gene; a new game "with your clear data" keeps the levels' growth
  in the bonuses, as the original's. (Omega's pair attack ids, warrior / sorcerer bonus, 234 / 239 / 243 repeats,
  sure-escape group, escape frames, Ward refusals, stat clamps, first-scenario level cap and start-level growth
  aren't Chaos Rings': its rules already stood here. Omega 3.1.0's sure Counter skill isn't either - Chaos Rings'
  newest release still rolls 10 % / 20 %.)
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
