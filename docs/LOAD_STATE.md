# Save-load state audit

## Findings and fixes

CR2's text-save and original binary-save loaders restored `player` and the
figure's transform after loading the field, but left the collision controller
and interpolation endpoints at the entry position. The next displayed frame
could immediately replace the loaded position with the entry position.

Both loaders now use `restoreFieldPosition`: it restores the controller, clears
old movement and interpolation timing, synchronizes the displayed endpoints,
and refreshes nearby interactions and encounter-distance tracking. Events the
player is already standing inside are marked occupied, as on entering a field.
The save's rendered feet position is 0.02 below the controller's centre.

CR3's legacy field loaders contained the same defect and now use the same
restoration logic, without CR2's encounter tracking. CR3's native `loadCr3Save`
path was inspected separately: `loadArea` already restores `areaWalker`, facing,
camera and occupied event regions from `savedArea` / `savedPos`. It does not use
the defective legacy path.

The CR1/Omega load-confirmation check also caught a regression from the preceding
title-cleanup change: `applySave` cleared the menu, and its caller reopened the
result panel without restoring its Load mode or displayed save metadata. Save
progress loaded correctly, but the panel could render as a Camp save panel.
The caller now rebuilds the load result panel with the copied save, while leaving
the discarded pending confirmation cancelled.

## Regression coverage

Before the fixes, `viewer-field-restore` failed four assertions in each of CR2
and CR3: controller location, interpolation timing, first displayed frame and
subsequent horizontal physics location. The existing CR1/Omega session test was
extended to load its synthetic save through the actual confirmation key events,
check the Load-complete panel and progress, and dismiss it to the field. Its new
panel assertion failed in both games before the fix.

The asset-free field test exercises the same restoration helper used by both
text and original binary saves. The real-data CR2 check additionally runs
`--continue` against a synthetic text save in an isolated profile, verifies the
controller's position and facing, then calls the actual walking/interpolation
update and checks the first displayed position. It is enabled with
`CR_SELFTEST_LOAD_POSITION="x y z yaw"`.

All 40 CTests pass in both native and GLES ASan/UBSan builds (CR1 6, Omega 6,
CR2 13, CR3 15). The expanded CR1/Omega load check passes with seven available
data variants in both builds (14 runs), and CR2's actual text-save loading check
passes with Android and iOS data in both builds (4 runs). LeakSanitizer is
disabled for the sandbox tracing limitation. All fixtures use isolated profiles under `/tmp/cr-transition-audit`; player saves
are not modified. These desktop checks do not establish physical-device behavior.
Omega's external Vita archive remains unavailable in this pass. No commits or
releases are published by this audit.
