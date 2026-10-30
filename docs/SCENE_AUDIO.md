# Scene audio ownership audit

Audit date: 2026-10-30. Covers CR1-U, CRO-U, CR2-U and CR3-U.

## Reproduced defects

CR1/Omega cleared their dialogue window on battle entry without stopping its
speech or remaining story clips. `updateVoices` could start a queued clip after
the dialogue had been cleared, and replacing a voiced page with an unvoiced
page could leave the old story line playing. Their existing normal page-tap
and return-to-title cancellation already worked.

Both ports now discard a closed page's pending clips. A new page stops the
previous story-line channel even when the new page has no voice. Successful
battle entry stops field dialogue speech and clears its queue before loading
the battle. Separately tagged battle-speaker streams retain their existing
policy; the change does not serialize all battle voices globally.

CR2 and the CR2-compatible battle path retained in CR3 stopped current voice
and effect playback when skipping a special scene, but cleared only delayed
effects. Delayed voices could therefore play after the skip. Abandoning a
battle or returning to the title also left pending clips and delayed result
music behind. These paths now cancel both clip queues and current playback;
leaving the battle/title boundary additionally cancels its delayed music.
A rejected skip leaves the queues intact. Cancellation happens before the
remaining combat hits are applied, so new cues from those hits keep their
normal timing. Natural scene completion and normal battle results were not
changed to cancel their remaining cues indiscriminately.

CR3's native title entry now clears old voices, looping effects and delayed
requests before selecting its own music. Its native CR3 battle implementation
is distinct from the retained CR2-compatible scene path; the shared `skipScene`
regression alone is not claimed as a test of every native CR3 battle animation.

CR2/CR3 also crashed when effect/voice playback indexed their empty slot list
before audio opened, after it closed, or after opening the sound data failed.
Their main flow tolerates an unavailable sound source, making this a real
error-path defect. Playback/stops now respect the available slot count, and
closing the player clears its music state and source metadata. Silence no
longer requires accessing nonexistent slots. This reproducer does not identify
the cause of the earlier user-reported CR1 crystal-room crash.

## Original CR2 evidence

A fresh REA/Ghidra inspection used the original CR2 Vita `eboot.elf`, SHA-256
`87b3abf90fa3583987a1b225ef31478de2db5c5c9f3e5b8be4f7921a544c96e2`.

- `0x8100dfaa` calls `0x81035260` with kinds 0 and 1 before the voice/effect
  stop helpers. The local annotated export calls this `ActionAwakeSkip`.
- `0x81035260` removes matching kinds from a linked list of pending jobs. Its
  historical annotated name is `BtlSound::CancelWork`.
- `0x81035492` creates a pending job with kind 0. The historical export calls
  it `CreatePlaySEJob`.

These three bodies were inspected from the binary in this pass, using Ghidra
12.1.2. Fresh analysis exposes generic `FUN_...` names; the friendly names come
from the earlier Android/Vita correlation and are not presented as newly
recovered Vita symbols. The calls and list removal support cancellation of
pending work in addition to stopping playback. They do not constitute a trace
of an original game running, or fresh evidence for all four releases.

The immutable analysis snapshot was saved locally as
`/tmp/cr-transition-audit/audio-scene-cr2-vita.snapshot.json`, then the REA
session was closed. It contains three evidence records. No game binary or
archive was modified or added to the repository.

## Before/after checks

- CR1/Omega's `CR_SELFTEST_SCENE_AUDIO=1 chaosrings --selftest` runs with real
  data. All eight data sets failed before the fix on battle-entry cleanup.
  The six voiced data sets additionally failed closed-page replay and unvoiced
  replacement checks. All eight now pass. The older iOS releases without
  separate story banks exercise the speech-marked effects-bank path and battle
  entry, rather than claiming nonexistent story-voice coverage.
- `audio-player` is an asset-free CTest for CR2/CR3. Both previous players
  aborted on an empty-vector access. Unopened, closed and failed-open playback
  now returns safely. Running `audio-player-tests <release>` also decodes a
  real voice, looping effect and music track, checks independent voice/effect
  stops, and verifies that a closed player reports no current music or source.
  The real-data checks request SDL's dummy playback device and require it to
  open successfully, exercising stream creation/destruction without audible
  output. An earlier silent-mode control reproduced the stale music state in
  all six Android/iOS/Vita sources before the close-state fix.
- `viewer-scene-audio` is an asset-free CTest in CR2/CR3, also available as
  `<viewer> --selftest-scene-audio`. It exercises the actual skip, battle-exit,
  title and audio-scheduler methods. Both old queue implementations failed six
  assertions: stale clips could replay, and old result music could replace
  title music. Both now pass. The no-data playback bounds were repaired first
  so the skip regression could reach these assertions without aborting.
- CR3's native title was tested separately using `CR_SELFTEST_SCENE_AUDIO=1`
  with `--title --capture`, on complete Android, iOS and Vita data. A real voice
  and looping effect are started before title entry. All three previously
  retained them; all three now stop them, clear pending requests and select
  title track 33. This is a playback-state check, not a visual title comparison.

That is fourteen distinct data variants and seventeen real-data scene/player
cases: eight CR1/Omega scenes, six CR2/CR3 player cases and three CR3 native
entries. CR3 Android uses the whole `CR3_android_1.1.3` folder, including its
sound asset packs; passing only the extracted main archive's assets folder
omits those packs and is not sufficient for a voice/effect test. The complete
Android and Vita sets expose 1,033 effects and 5,877 voices; iOS has 1,033 effects
and 3,550 voices. Missing packs are reported as a test failure, not a successful
silent run.

All **36 CTests** pass in native desktop GL builds and in GLES ASan/UBSan builds
(CR1 6, Omega 6, CR2 11, CR3 13), including prior save-import, lifecycle-clock,
input-cancellation, graphics recovery, scene-reset and CR3 feature regressions.
LeakSanitizer remains disabled for the sandbox tracing limitation.

```sh
cmake --build engine/build
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  ctest --test-dir engine/build --output-on-failure
```

Real-data checks require user-supplied game files. The tests do not measure
speaker/headphone output latency, Android/iOS audio-focus behavior, or the
original user's exact device failure. Physical-device testing remains open.
Changes in this audit are local; no release or push was performed.
