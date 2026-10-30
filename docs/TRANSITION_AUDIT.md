# CR1 / Omega field-transition audit

Targeted reverse-engineering pass, 2026-10-29. This covers field loading and teardown
in CR1-U and CRO-U. It is not a fresh whole-game decompilation or a device crash trace.

## Findings and changes

| Finding | Before | Change |
|---|---|---|
| Return to Title did not terminate puzzle state | The NPC list was cleared, but the old stage, object indices, grid and pending retry/ability timers remained. A pending retry could run against the title's event file. | End puzzle tasks before clearing the old room; clear the puzzle banner and pending start too. |
| NPC scale tasks survived a room change | `moves` was cleared, but `rescales` was not. A new room could inherit an old actor ID's scale changes or report that actor busy. | Clear both kinds of actor task together. |
| Interrupted teleport opacity survived loading | Loading recreated the puzzle but reused the leader's cached model with its old alpha and fade timer. A load after the teleport fade-out left the leader invisible. | Restore full leader opacity and cancel its fade when discarding field tasks. |

`endFieldTasks` is shared by `enterMap` and `bootTitle` in both ports. A same-room
warp does not call it; scripts that continue on the same field retain their tasks.
The opacity reset is a port-side cancellation rule for the cached leader model,
verified through the normal load path, rather than a claim about an original
binary's exact alpha-reset instruction.

The previously implemented arrival fade is correct: `FieldScene::onLoading`
finishes with `SetMask2(1)`, which preserves mask colour and changes alpha from
255 to 0 over 500 ms. No new fade change was needed.

## Original-code evidence

Fresh REA/Ghidra static inspection used the local Android native libraries:

| Target | SHA-256 |
|---|---|
| CR1 Android 1.1.1 | `161576d7b857d9c536cd31412b87deb4b04e191d01aca51c4c17572a7e21a156` |
| Omega Android 1.1.6 | `340c0f2464be1ef2f73fa5896be21bf0af077e060f3f033434bc5008d0930765` |

| Target / function | Address | REA Evidence ID |
|---|---|---|
| CR1 `FieldScene::onLoading` | `0x82b6c` | `ev_184a9a6594298b058d0cff7fa71f08caa769902864e51e1ff3e5259c9572b725` |
| CR1 `SetMask2` | `0x3593c` | `ev_3da4d4c2014bd14a0e86483dd6ca366a40f6cc822920241fa7b94af717a813f8` |
| CR1 field teardown helper | `0x829ec` | `ev_184f7199b1f4240b4422bbcf0b395c6a4a632020099f1e0a4424b82d6576663c` |
| CR1 `PuzzleManager::term` | `0x8b32c` | `ev_62de2117a388f903d48f4f8a2b198becb0d2763ddcd94c9bf968f5f9ae8f9cc4` |
| CR1 `npc_read_setup` | `0x609bc` | `ev_9ddd5077a56db7bc12318f22fca637afe46af1d780a9fd01bd725f1be94c0b42` |
| Omega `FieldScene::onLoading` | `0x99ee0` | `ev_e5bdb08ebaec6b1879fcac25a4ebb237867d051402e4b40e63c8d2f72cf0e483` |
| Omega `SetMask2` | `0x387b0` | `ev_e6d42451b950439e80f449cc0d5f5ef96568c88730dd2ea3ce4d9496082a4e4d` |
| Omega `PuzzleManager::term` | `0xa2d40` | `ev_ef89c429ec8dfce22be4c263108c4bfebb22e9af5fe6a75d52e9751d17a01e08` |

The originals invalidate the puzzle selection and destroy its ability and object
instances on termination. CR1's field teardown calls `PuzzleScene::term`, which
calls `PuzzleManager::term`. `npc_read_setup` zeroes the NPC records containing
the autoctl mode/timers; `GetNpcCtl` and `npc_autoctl_set_scale` identify that
record layout in the existing decompilation.

Existing local iOS/Vita decompilations corroborate the termination/reset pattern:
CR1 iOS 2.2.0 `PuzzleManager::term` at `0x459b0`, Vita at `0x81064d7c`;
Omega iOS 3.1.0 at `0x4bed0`, Vita at `0x81072dda`. Their carried-over names are
inferred function matches; these builds were not freshly imported or executed.
REA bundles are kept outside the repositories under `/tmp/cr-transition-audit/`;
no original game code or data is included in the patch.

## Verification

`CR_SELFTEST_LIFECYCLE=1 chaosrings --selftest` enters real rooms and exercises:

- Return to Title during a pending Retry for each of the four puzzle abilities.
- A room change during a live NPC scale task.
- Loading captured save state after an actual Teleport has faded the leader out.

Before the fix, the check reported six failures in each port with Android data
(four title-return cases, the scale task, and teleport opacity). After the fix,
all six cases pass. These tests deliberately exercise interrupted states through
the runtime API; they do not establish that each interruption is reachable from
every normal menu or input sequence.

Validation also runs the existing `CR_SELFTEST_MOBILE=1` check: scrolling,
equipment hitboxes, orientation-setting persistence, voice replacement and real
crystal-return bytecode. CR1 covers the first puzzle on all four routes, solving
and revisiting it before/after loading (16 cases per data set). Omega covers all
16 puzzles before/after loading, including solving the first (32 cases).

All 16 asset-dependent runs passed under AddressSanitizer and UndefinedBehaviorSanitizer:

| Port | Primary data source | Lifecycle | Existing mobile / crystal checks |
|---|---|---|---|
| CR1 | Android 1.1.1 | Pass | Pass, 16 exits |
| CR1 | iOS 1.0.1 | Pass | Pass, 16 exits |
| CR1 | iOS 2.2.0 | Pass | Pass, 16 exits |
| CR1 | Vita | Pass | Pass, 16 exits |
| Omega | Android 1.1.6 | Pass | Pass, 32 exits |
| Omega | iOS 1.0.0 | Pass | Pass, 32 exits |
| Omega | iOS 3.1.0 | Pass | Pass, 32 exits |
| Omega | Vita | Pass | Pass, 32 exits |

This totals 48 lifecycle cases and 192 crystal exits. Each run used a separate
profile/save directory and disabled other asset add-ons; startup logs confirmed
the selected primary source. Both ports' CTest suites also passed (Vita importer
and ATRAC9 bit operations). LeakSanitizer was disabled because the sandbox uses
ptrace; address and undefined-behavior checks remained enabled. Native desktop
and instrumented builds succeeded. Logs and `matrix.json` are retained in
`/tmp/cr-transition-audit/`; user profiles/saves were not used.

## Remaining uncertainty

The original user's early crystal-exit crash has not been reproduced on an Android
device, and no matching stack trace is available. These are confirmed cleanup
bugs, not proof of that report's exact cause. Runtime checks execute the ports on
desktop with an offscreen OpenGL renderer and dummy audio; they do not execute
original ARM code or test Android activity/surface loss and orientation changes.
CR2/CR3 use a different field runtime and are outside this patch's scope.

## Follow-up presentation checks

A subsequent focused pass ran `CR_SELFTEST_FIELD=1` under ASan/UBSan with the same
eight primary data sets (Android, older/newer iOS and Vita for each port):

- CR1: all eight leaders walk/run with advancing poses after a viewer pause;
  seven known origin-loaded cutscene backdrops activate/hide correctly; dialogue
  parchment covers both halves in all five tested layouts, per data set.
- Omega: all four leaders walk/run correctly; the deck backdrop appears at the
  origin; parchment renders in all six tested layouts, per data set.
- An actual CR1 Android `t01` event 202 capture at frame 260 shows the reaper,
  hall backdrop, portrait and complete parchment. The screenshot and trace are
  stored outside the repo in `/tmp/cr-transition-audit/captures/`.

All eight runs passed with no sanitizer errors. These are bounded checks of the
known affected scenes and presentation paths, not an exhaustive visual audit of
every cutscene. No additional CR1/Omega presentation code change was needed.
