# Suspended-time audit

Date: 2026-10-30. Ports: CR1-U, CRO-U, CR2-U and CR3-U.

## Findings

The ports had lifecycle timing gaps independent of the graphics recovery fix:

- CR1/Omega's music and controller-repeat delta used raw `SDL_GetTicksNS()`
  differences between app iterations. The first iteration after Android's
  blocked event pump could consequently receive the whole background interval.
  Their drawing/gameplay delta already capped long frames at 0.1 seconds, but
  still included suspended time in that first frame.
- CR1/Omega's effect fades used absolute `SDL_GetTicks()` timestamps. A fade
  could finish while its audio device was paused, rather than continue from
  where playback stopped.
- CR2/CR3's interactive viewers passed an uncapped wall-clock delta to model
  animation and `Viewer::advance`. This feeds UI, sound fades, play time and
  scene/battle/field updates. A long background interval could therefore become
  a large first update on resume. Some internal script loops cap their work,
  but that does not protect all of these other clocks.

The pinned SDL 3.2.0 implementation already handles native Android audio pause
and resume when using its default blocking-on-pause behavior. In
[`SDL_androidevents.c`](https://github.com/libsdl-org/SDL/blob/release-3.2.0/src/video/android/SDL_androidevents.c),
`Android_OnPause` pauses the OpenSL ES/AAudio devices and `Android_OnResume`
resumes them. The port's missing work was excluding suspended time from its
clocks; a second audio-device pause layer is unnecessary for that path.
This was established from local pinned SDL source, not a new original-game
binary decompilation or a physical Android trace.

## Change

`platform/lifecycle_clock.hpp` records the first background notification and
subtracts the interval through `SDL_EVENT_DID_ENTER_FOREGROUND`. The clock stays
frozen during foreground preparation. Duplicate will/did background phases and
repeated foreground notifications do not subtract the same interval twice.
Successive pauses accumulate. The first resumed frame rebases its delta, then
normal foreground updates resume. Ordinary foreground stalls retain their
previous timing policy; desktop focus loss alone does not pause the game.

A thread-safe SDL event watch records the lifecycle timing when the event is
sent, including while SDL blocks the main event pump. Watches update only clock
state, without touching gameplay or audio-device objects. Draining the event
queue does not record those timestamps a second time. Watches are detached
before their owners are destroyed.

CR1/Omega use the clock for their iteration delta, render/gameplay timestamp and
effect-fade timestamps. Their existing 0.1-second render cap is retained.
CR2/CR3 use it for the interactive viewer's frame delta. Deterministic captures
and scripted `--run` playback keep their explicitly supplied simulation steps.
Music streams, queued voices and current scenes are preserved across background
notifications. Existing page-advance speech cancellation and battle voice
policies are unchanged.

## Verification

The synthetic `audio-lifecycle` test builds a small CAF effects bank and plays
a looping entry through SDL's dummy driver. It starts a 500 ms fade, sends a
background event from a worker thread, waits 650 ms, resumes and verifies that
the sound still plays. After another 550 ms of foreground time, the same fade
must finish. The previous sound implementation fails the background-survival
assertion in **both CR1 and Omega**; the updated implementations pass. No original
assets or audible output are required.

The shared `lifecycle-clock` CTest uses deterministic timestamps to test an
hour-long background interval, both will/did phases, foreground preparation,
duplicate notifications, multiple pauses, reset, normal foreground stalls,
a blocked event pump, a real worker-thread notification and watch detachment.
CR2/CR3's additional `viewer-lifecycle-clock` CTest drives the actual viewer's
`advance` method: its UI clock remains at 0.050 seconds across the simulated
pause and reaches 0.100 after the next 50 ms foreground update. It also runs as
`cr2view --selftest-lifecycle-clock` / `cr3view --selftest-lifecycle-clock`.

All **32 CTests** pass in native desktop GL builds and again in GLES builds
under ASan/UBSan: CR1 6, Omega 6, CR2 9, CR3 11. This includes the previous
save-import, interrupted-input, graphics-context recovery, event-reset and CR3
feature checks. The GL tests execute locally using Mesa, rather than skipping.
LeakSanitizer is disabled because of the sandbox's tracing restriction; address
and undefined-behavior checks are enabled.

The existing CR1/Omega lifecycle and mobile-input checks were rerun under the
sanitizers with Android, both available iOS releases and Vita data (eight data
sets, sixteen runs). They cover rapid speech replacement, list/equipment input,
orientation persistence, field teardown and 192 crystal-room exits. These are
regressions of the available scenarios, not a reproduction of the user's
unspecified device/route crash. Rapid dialogue advance was already protected;
this pass fixes suspended-time accounting rather than changing which battle
speakers may overlap.

```sh
cmake --build engine/build
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  ctest --test-dir engine/build --output-on-failure
```

Physical Android/iOS background/resume behavior, platform audio focus and the
original user's exact failure sequence still need device testing. The
hour-long pause is simulated, and the fade comparison uses a dummy audio
device; neither is presented as physical-device evidence.
