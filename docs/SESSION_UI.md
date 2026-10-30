# Title transition and session UI audit

Returning to the title must abandon the old session's menu confirmations,
input targets and asynchronous import picker. It must not confirm a save or
import, leave the game paused, or leave an old field/tutorial runtime receiving
input behind the title. Settings are retained.

## CR1 and Omega

`bootTitle` now closes Camp, Options, manual/scenario/character-selection and
field overlays, discards pending save/load and resume confirmations, stops the
parked shop script, releases temporary overlay textures and clears old pointer,
repeat, menu-mask and held-dialogue state. Cached Camp artwork is retained.
The importer is cancelled by detaching its shared picker result; a late callback
can complete safely but cannot reopen the importer in the new session.

Previously, a save confirmation could remain behind Options. Choosing Return to
Title and then pressing Enter can execute that stale confirmation against the
old slot. The regression uses its own temporary directory and a synthetic save,
compares the file bytes after the late Enter, and removes only that fixture.
It never uses a player's save directory for this write.

`CR_SELFTEST_SESSION_UI=1 chaosrings --data <game data> --selftest` exercises the
actual keyboard event path through Camp -> Options -> Return to Title, the
hidden save confirmation and late Enter, broader overlays, late picker
completion, and preference preservation. The fixture has already read Omega's
first-time Options tutorial so it reaches the requested Options action.

## CR2 and CR3

A shared title-entry helper closes legacy menus, clears temporary pause/input
and ability controls, detaches field and tutorial runtimes, and cancels the
import picker. It keeps game progress until the player chooses a file or new
game. CR3's native title also uses the helper after preparing its title assets;
it discards native Camp/text-entry/area/battle runtime state and pending save
sync, while preserving its saved one/two-handed control preference.

`viewer-session-ui` is an asset-free CTest (`--selftest-session-ui`) covering the
legacy title. `CR_SELFTEST_SESSION_UI=1 cr3view --data <game data> --title
--capture <image.bmp> --renderer opengl` also exercises the real native CR3 title
and checks the same invariants after the actual title initializer.

## Validation for this pass

Native builds and GLES ASan/UBSan builds each pass all 38 CTests
(CR1 6, Omega 6, CR2 12, CR3 14). LeakSanitizer is disabled for the
sandbox tracing limitation.
Real-data checks pass for CR1 Android, iOS HD, iOS phone and Vita; Omega Android
and both iOS versions; and CR3 native title with Android, iOS and Japanese Vita
data. All ten checks also pass under GLES ASan/UBSan (twenty runs total),
using isolated profiles under `/tmp/cr-transition-audit`.
Omega's Vita archive on the external volume is not currently mounted, so it was
not included in this pass. CR2's cleanup regression is asset-free.

These are desktop runtime checks using the platform data sets. Physical Android,
iOS and Vita device behavior is not established by them. Changes remain local;
this pass does not push commits or publish releases.

## Follow-up: play time (2026-10-30)

In the original, the play time is part of the `prty` block (G+0x2f930; hours at
+4, milliseconds at +8). `_prty_init_all` clears it for Reset Data and when no
save exists, and `__menuu_TitleClr` calls `_Load_SaveData`, so each visit to the
title puts the saved block back. `_prty_init_new` leaves the timer running across
pairs. This is from the iOS decompilations of both games.

CR1/Omega kept counting instead. They now clear it on Reset Data and at a new
game, and the title goes back to the newest save's time, or zero with no save.
`CR_SELFTEST_SESSION_UI=1` checks both.
