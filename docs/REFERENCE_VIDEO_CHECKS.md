# Reference video checks

What the port's behaviour was checked against on the reference recordings, and what was found.
Reference: the local CR1 playthrough (Archive/refvideos/cr1, `NN_<id>.mp4`, 1280 x 720 at 60 fps).
Part 02 is the Android release (its 16:9 layout: the Break badge and the rows on the phone's own
edges). Compare content, sequence and timing, and layout only within a screen. The game draws 20
frames a second; grabs at `ffmpeg -vf fps=20` are one game frame each, though the recording drops
or repeats a frame now and then (a step of 30 or 32 between two of the same events). An OCR index
at 2 frames a second is in ~/.cache/cro-issue/cr1-ref/ocr-NN.

## Battles (backport of CRO-U's 2026-11-08 battle pass)

| Behaviour (source) | Video, time | Result |
|---|---|---|
| A first strike's banner names the team (btl_main 200: var 0 the lead's name, sys 0x36, 60 frames) | 02 674.5-676.6 s: "Escher's team attacks first!" from the fade | Confirmed; the port showed "'s team attacks first!" before |
| A strike's banner stays over the first page (__menub_mainPrc writes slot 11 only for the escape question; btl_main 500 only clears the queue) | 02 676.6 s: "Escher's team attacks first!" over Solo / Pair / Escape / Options | Confirmed; the port cleared it with the page before |
| The opening: solid white, the 20-frame fade from btl_main 200, the commands' camera 32 frames after 200 (300's counter 30, 400, 499, the menu) | 02 673.5-677 s and 1632.5-1637 s, mean brightness per game frame: white for 8 frames, a linear fade over 19-20, the scene still after it, the cut to the commands' camera 30 and 32 frames after the fade's first step | Consistent: the port's cut comes 31 frames after its fade's first visible step. The white lasts longer in the video (the original holds it while the battle loads; the port's 4 steps are the minimum) |
| The command pages slide in over a few frames (menub_mainPrc) | 02 1634.9-1635.0 s: the plates arrive 3 frames after the camera's cut | Differs: CR1:U has no page slide yet (Omega's 893363e / fc672d9 not ported). Open |
| Android layout: the Break badge, the rows and the banner on the phone's edges (`_menub_mainPut`) | 02 676.0 s: the badge in the phone's top-left corner, the rows on its bottom-left edge, the banner at the top | Consistent with 1.1.1's code (- ox - oy, - ox + oy, - oy), which the port's Android layout now applies; not measured to the pixel |
| Floor shadows under the fighters (_btl_shadow) | 02 676.0 s: soft dark ellipses under the pair and the monsters | Consistent (not measured) |
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
