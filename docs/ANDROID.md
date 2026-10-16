# The Android release (1.1.1)

Notes from the Android APK (Chaos Rings 1.1.1), read with `tools/re_setup_android.sh <unpacked
APK>` (Ghidra; output in `re/android/`, never committed). Nothing of the APK is in this repository.

## What the APK holds

| File | What it is |
|---|---|
| `lib/armeabi/libchaosrings_native.so` | the game: the iOS code base, Thumb (GCC), **1660 functions exported by name** - ~455 of them names 1.0.1's symbols lack (the voiced release's battle voices, statics) |
| `lib/armeabi/libmc_eruption_for_android_jni.so` | MascotCapsule eruption, the 3D engine (as on iOS) |
| `lib/armeabi/libsobridge.so`, `assets/libparser.so` | a loader and an encrypted library it unpacks at run time (entropy 8.0; licence / anti-tamper, not read here) |
| `classes.dex` | the Java shell: the activity, Google Play licensing (LVL) and the APK Expansion downloader |
| `assets/00000`, `00001` | two OpenType (CFF) fonts (the same as Omega's APK) |
| `assets/loading.mct` | the loading picture |

The game's data is **not** in the APK: it comes as a Google Play expansion file
(`main.100.com.square_enix.chaosrings.googleplay.obb`, 1,018,269,756 bytes): the game's 1380 files
one after another, **with no table of its own**. The table is in `classes.dex`: a Java class's
constructor fills a `String[1380]` with the names, one const-string at a time, and two `int[1380]`
arrays (fill-array-data payloads) hold each file's size and CRC-16; a file's offset is the sizes
before it added up, and they add up to the OBB exactly. `cr::ObbArchive`
(`engine/src/assets/obb.cpp`) reads the table out of your APK at run time; `crtool obb <obb> <apk>
[name -o out]` lists, checks or extracts. `FileDownLoadAndCheckProc` is empty here.

The files: the voiced, multi-language 2.2.0 data plus 148 `_15` pictures (the 1.5x screens' art),
`window_cutin_480 .. 1366`, and the iPhone's own art; every texture as RGB / RGBA or a palette - no
PVRTC - and the music as `j001m01.mca` .. `j001m30.mca` (29: no 07), Ogg Vorbis after a 1 KB header (the other
`.mca` files are animations, as on iOS).

`--android <folder | main.*.obb>` (or `CR_ANDROID`, or Options, Game, Android Assets; saved as
`android=`) adds it as a source: a picture the data lacks, or one of the same size, comes from it,
and a model package's textures when it holds as many of the same sizes (the meshes stay the
data's); the music plays from its Ogg Vorbis tracks (stb_vorbis) before the iPad's AAC (the Vita's
ATRAC9 still first), looping at the Android build's own points (`setbgm_caf`'s table at 0x98f70,
the sample `SndBgmPlayerCallback` seeks back to; -1: bgm30 and bgm31 play once - and Android has
no bgm31, which stays the iPad's). The folder must hold the OBB and the APK (or the APK's `classes.dex`); the one whose table adds up to the OBB is the
one used.

## The screen

As Omega's Android build (`docs/ANDROID.md` of CRO-U has the details): `is_iPadDisplay` 0 (the
iPhone code path), the 480 x 320 screen at 1.0, 1.5 (`_15` art, from 720 x 480) or 2.0 (`_hd`,
from 960 x 640) and centred; the message window's cut-in strip per screen width
(`window_cutin_854`, `_1280`, ...), `window_15`, `window_char_15`.

The port's Layout: Android (Options, Display, or `--layout android`) draws the iPhone layout
centred on a 16:9 screen (the window's aspect in widescreen) as a 720-tall phone at 2x would,
and, with `--android`, the message window's strip as `draw_win` does: `window_cutin_<w>` for
the screen's width 1:1 from its left edge at y 2 y + 20 (`get_draw_win_point`), the parchment's
name streak (512, 120, 196, 40) over it at (x, y + 16) (`get_draw_namewin_point`); none above
1366 wide, where the iPhone's parchment stays.

## Other differences

- The voiced release: `btl_voice_*`, `voicebtl.bin` / `voiceevnt.bin`, `NDKGetLanguage`.
- Stereoscopic 3D (`getStereoDevice`, `setStereoMode`), the back key, pause / resume.
- Names 1.0.1's symbols lack, among them `btlcalc_attack_hit`, `btlcalc_zoku_0203`,
  `btlcalc_zoku_dmg`, `btlcalc_rand_dmg`, `btlcalc_steal_item_add`, `btlai_make`, `chk_gene2`,
  `fldencount_encount_debug`, `TreasureBoxManager::*`, the puzzle classes. Carried to 2.2.0 by
  `tools/ghidra/match_functions.py --cross`, they name 121 more of its functions than 1.0.1's
  names do (cross-compiler matching: check a name against the code before trusting it).
