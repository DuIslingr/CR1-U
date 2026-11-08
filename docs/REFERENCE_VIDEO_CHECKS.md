# Reference video checks

What the port's behaviour was checked against on the reference recordings, and what was found.
Reference: the local CR1 playthrough (Archive/refvideos/cr1, `NN_<id>.mp4`, 1280 x 720 at 60 fps).
It is the Vita release running in Vita3K (the Vita's 960 x 544 scaled to 1280 x 720; Part 02 135.5 s
shows a Vita3K trophy pop-up and Yes / No with the Vita's circle / cross icons) - not iOS or Android.
Its code is re/vita/decomp.c. CR1:U has no Vita layout of its own (its layouts are iPad, iPhone and
Android), so compare content, sequence and timing, never positions against the port's layouts. The game draws 20
frames a second; grabs at `ffmpeg -vf fps=20` are one game frame each, though the recording drops
or repeats a frame now and then (a step of 30 or 32 between two of the same events). An OCR index
at 2 frames a second is in ~/.cache/cro-issue/cr1-ref/ocr-NN.

## Battles (backport of CRO-U's 2026-11-08 battle pass)

| Behaviour (source) | Video, time | Result |
|---|---|---|
| A first strike's banner names the team (btl_main 200: var 0 the lead's name, sys 0x36, 60 frames) | 02 674.5-676.6 s: "Escher's team attacks first!" from the fade | Confirmed; the port showed "'s team attacks first!" before |
| A strike's banner stays over the first page (__menub_mainPrc writes slot 11 only for the escape question; btl_main 500 only clears the queue) | 02 676.6 s: "Escher's team attacks first!" over Solo / Pair / Escape / Options | Confirmed; the port cleared it with the page before |
| The opening (the Vita's btl_main / btlcamera_prc as 2.2.0's: the 30 / 60 counter, 0x1d / 0x21b, the 5.0 easing): solid white, the 20-frame fade from btl_main 200, the commands' camera 32 frames after 200 (300's counter 30, 400, 499, the menu) | 02 673.5-677 s and 1632.5-1637 s, mean brightness per game frame: white for 8 frames, a linear fade over 19-20, the scene still after it, the cut to the commands' camera 30 and 32 frames after the fade's first step | Consistent: the port's cut comes 31 frames after its fade's first visible step. The white lasts longer in the video (the original holds it while the battle loads; the port's 4 steps are the minimum) |
| The command pages slide in a quarter of their distance a frame (__menub_mainPrc: 2.2.0 and the Vita's (vita decomp 45795..) 320 at 80; Android 1.1.1 480 at 120 on a Retina / 720 x 480 screen) | 02 (Vita) 1634.55-1635.15 s at 60 fps (3 video frames a game frame), the plates' columns profiled per frame: the camera's cut at C, nothing at C+1 and C+2, the plates cut by the screen's right edge at C+3, settled at C+4 | Ported (MenuView / stepMenuView / battleMenuReady); timing matched: on the 480-unit screen the plates (x 328 on) are off it at 240 and 160 and cut at 80, so the Vita's 320 / 80 page is in view at C+3 only if it starts at its distance on the cut's frame. The view now steps after the frame's logic and does that (self-test: 320 240 160 80 0 from the cut); stepped before the logic, as Omega's port does, it came a frame late |
| The opening's camera eased by a fifth a frame (btl_prc's FUN_0000b4d8 / b580: shown += (wanted - shown) / +0xfc; btlcamera_prc sets +0xfc 5.0 for the formation, victory, cast and 0x21b cameras once their count passes 1, else _Clr_evcv cuts) | 02 673.5-677 s and 1632.5-1637 s: the scene still for 11-12 frames before the commands' cut | Ported; consistent (the turn has settled before the cut). Not measured by phase correlation - the fade washes out the turn's first 20 frames in these clips. The self-test measures the eye's steps: rising to the turn's 15th frame, the last under 5% of the peak |
| Android layout: the Break effect's stripes and labels (btlbreak_effect_prc 0x3ccb0) | - (the videos are the Vita's; no Break effect found in 02's OCR either) | Code reading and a port capture (`break@12`): the first stripe and "Player" in the band's top-left corner, the second and "Enemy" in its bottom-right, "BREAK!!" centred |
| The rows' taps follow the rows (Android) | - | Not applicable: 1.1.1's _menub_mainPrc / _menub_mainPut test no box on the rows (their boxes are the plates and the Back bar, centred), and the port has none either |
| Android layout: the Break badge, the rows and the banner on the phone's edges (`_menub_mainPut`) | - (the videos are the Vita's: its own screen layout, not Android's) | Code reading only (Android 1.1.1: - ox - oy, - ox + oy, - oy), and a port capture (`--layout android --battle-view top@3`, the 16:9 band in a 1024 x 768 frame - no CR_CAPTURE_SIZE in CR1:U yet) with them in the band's corners. A first version of this row took video 02 for the Android release - wrong; nothing here is confirmed on video |
| Floor shadows under the fighters (_btl_shadow) | 02 676.0 s: soft dark ellipses under the pair and the monsters | Consistent: the port's captures show the same ellipses under each fighter (sizes not measured) |
| The Break ring a tick every other frame, the results' totals and purse, the boss's end, "Counter", an entry announced once, the banner a frame late, the turning, the cast loops | - | Code reading only (2.2.0 and Android 1.1.1 decomps) |

## The Camp and the menu screens (backport of CRO-U's 2026-11-07 / 08 Camp and menu passes)

Port captures: `--layout android` (1024 x 768, the 16:9 band in the middle) beside ffmpeg frames.

| Behaviour (source) | Video, time | Result |
|---|---|---|
| The field's save screen (`__menuu_SaveLoadPut` from the SAVE button): the pair's boards at (0, 24 + 88k), Yes / No, "Save data?" on the board with its mark at the lower right, no heading | 01 923 s (also 01 1690, 02 456 s) | Contradicted, fixed: the port drew the Camp's heading and no mark. Now as the video (`--save-screen save`) |
| No mask under the field's save screen (`__menuu_SaveLoadSet` masks in mode 1 - the title's - only) | 01 921 / 923 s: the hall's top (700..840 x 0..100) 50.0 / 50.1 mean before and under "Save data?" | Contradicted, fixed: the port blacked the scene to 0xa0 |
| The face boards' level right-aligned (`__ComUty_FaceBoardFaceDraw`) | 01 923 s: "LV      1", the digit at the field's right | Contradicted, fixed: the port drew it at the field's left |
| The field map with no dim of its own (`__menuu_MapPut` / `MapSet`: no mask) | - | Code reading only (CRO's finding on Omega's video; CR1's map sheet has its own panel, `--field-menu map`) |
| The manual's pages in the message window, "Option" the first time (`__menuu_OptionPrc` 0..2, 0xf..0x16) | - (no Options footage in the OCR of parts 01-03) | Code reading only |

## Trophies (backport of CRO-U's achievements, 1386052)

CR1 2.2.0 and Android 1.1.1 have no achievements (docs/ENGINE.md "Trophies"); the Vita release's
trophies are CR1's. The reference playthrough is that release on Vita3K: Vita3K's own trophy
notifications show in Parts 01, 02, 04 and 05 - so those parts at least, Part 02 among them, are
the Vita release, not Android's (which has no trophies). Found by an OCR pass at 0.5 fps
(~/.cache/cro-issue/cr1-ach/ocr/NN) and a scan for the box's grey at 2 fps (cr1-ach/scan.txt;
it misses some, e.g. 01 3075 s). None found in Parts 06-29: trophies 0 and 9..14 (the primal doors,
Qualia, the businessman, Gold) are not seen on video.
Port captures: the Vita copy as the game, `--resolution 1280x720 --widescreen`,
`CR_CAPTURE_TROPHY=n[:seconds]`.

| Behaviour (source) | Video, time | Result |
|---|---|---|
| The trophies in the playthrough, in its order: 1 A Dryg Goch, 2 Cerberus, 15 the first purchase, 3 Gnomus, 4 Airavata, 5 Ondine, 6 Old One, 7 Ra, 8 the Prototype Agent (FUN_8106ae3a's conditions) | 01 1850.7 s, 01 3075 s, 02 135.5 s, 02 1551.5 s, 02 2788 s, 04 863 s, 04 2146 s, 05 1040 s, 05 1939 s | Consistent: each after its boss's scene (the gate bosses' flags), none twice |
| The first purchase: unlocked in the shop as the purchase is paid (__menuu_ShopPrc 7: FUN_8106ad28(0x2f), FUN_8106ae3a) | 02 135.5 s: Oz 45482 -> 43862 (Favorite Black Tunic, 1620) and "(Bronze) 初めてのお買い物" with "Equip Favorite Black Tunic?" | Confirmed: the port's shopPay unlocks it as it pays, before the equip question |
| The box: translucent light grey with rounded corners at the screen's top right, the trophy's picture, "(Bronze) <name>" (cut at the box's edge) and "You have earned a trophy!" in dark grey (Vita3K's; on the Vita the system draws its own) | 01 1851.6 s (A Dryg Goch): box x 720..1251, y 26..107; picture 736..800 x 34..97; glyph rows 42..66 and 72..91, widths 402 / 256 px; colours (182, 181, 186) over the dark snow, (192, 182, 178) over 01 3075 s's orange, the ink (54, 52, 56) | Port (`CR_CAPTURE_TROPHY=1:1`, Vita copy as the game): box 721..1250 x 26..107, picture 736..800 x 34..97, rows 42..66 / 72..93, widths 406 / 250 px - the line font a little different (the system's CJK font); the box (190, 188, 193) at 0.94 (fitted to both backgrounds) gives (179, 177, 182) over the hall's near-black |
| The motion: in from the screen's right edge, held, out to the right; one at a time | 01 1850.67-1850.80 s in (left edge 929, 849, 769, 721 at 30 fps: 80 px a frame), held to 1852.50 s, out by 1852.77 s (753, 834, 914, 993) | Port: in over 0.23 s, held 1.73 s, out over 0.23 s, linear (self-test `trophies:`: in 0.23 s, gone 2.20 s) |
| When: at the script's end after the boss's battle (FUN_8106ae3a from __EvtReturn / __EvtAbort) | 01 1850.4 s the field back from black after the battle's results (1847-1848 s), 1850.67 s the notification | Consistent: the port checks as the script ends (not measured against a port run of that scene) |
| The names: TROP.SFM's (Japanese only, even with the game in English) | 01 1851 s 『ア・ドライグ・ゴッホ』, 01 3075 s 『ケルベロス』, 02 1552 s 『ゲノムス』, 04 863 s 『オンディーヌ』, 04 2146 s 『オールド・ワン』 ... "を突破した！" | Confirmed: the pack's names (built in the same) |
