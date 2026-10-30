# Interrupted input handling

Follow-up audit, 2026-10-29, covering CR1-U, CRO-U, CR2-U and CR3-U.

## Problem and behavior

All four ports retained active gestures when focus or a touch was interrupted.
Without the final release event, walking drags, menu swipes, scrollbars or held
fast-forward could remain active. A late release could also finish an action
that belonged to the interrupted gesture.

`platform/input_cancel.hpp` now watches for the owning window losing focus or
being minimized, a cancelled finger, and application background notifications.
It only records an atomic cancellation request. Cleanup runs on the game thread;
it does not modify gameplay/UI objects from SDL's lifecycle callback thread.
The watch is detached before its owner is destroyed. Events from another window
are ignored. The implementation uses APIs present in the pinned SDL 3.2.0 headers.

- CR1/Omega clear the walking pad, Camp pointer, options-scrollbar drag, viewer drag,
  pending tap/skip and menu-controller repeat. A psychokinesis block already held
  stays held; a new press re-anchors its drag, and only the fresh release can
  send or drop it. Cancellation itself does not move the block or spend ESP.
- CR2/CR3 clear the walking drag, menu swipe and fast-forward input. Pressed slash,
  aim and lock gestures return to awaiting fresh input, preserving their prompts.
  CR3 also releases town/world/My Page drags.
- An interrupted pointer release is ignored until a new pointer press arrives.
  Cancellation does not call a menu's normal click/release path or finish a
  gameplay action. Normal continuous movement across map boundaries is retained.

The original Android `touchesCancel` routines also clear pad/touch queues:
CR1 at `0x2f398`, Omega at `0x319dc` in the existing local decompilations. Those are
historical static references; this pass does not claim a new original-device trace.
The held-block re-grab policy is the port's way to preserve an interrupted action.

## Checks

Before cleanup was added, the focused viewer checks retained active gestures for
all three tested interruption types (focus loss, cancelled finger, background)
in each port. After the fix they pass.

`input-cancel` is an asset-free CTest in all four repos. It checks owning-window
filtering, one-shot request consumption, cancelled touch, a background notification
pushed from a worker thread, and detaching the event watch. CR2/CR3 additionally
have an asset-free `viewer-input-cancel` CTest, also runnable as
`cr2view --selftest-input-cancel` / `cr3view --selftest-input-cancel`. It exercises
the actual viewer cleanup and verifies action prompts remain pending.

CR1/Omega use `CR_SELFTEST_CANCEL=1 chaosrings --selftest` with local game data.
The check feeds interruptions through `SDL_AppEvent` and tests a real lifted
psychokinesis block: a late release is ignored, a fresh gesture can drop the block
in place, and the ESP/use counters do not change.

Logs and the isolated test profiles are under `/tmp/cr-transition-audit/`.
No user saves or settings are used.

Final validation passed:

- All 20 CTests across the four ports (3 CR1, 3 Omega, 6 CR2, 8 CR3), both native
  and with AddressSanitizer/UndefinedBehaviorSanitizer instrumentation.
- CR1/Omega's actual SDL cancellation and real held-block/re-grab checks with
  Android, older/newer iOS and Vita data (eight primary data sets). Every block
  remained held after cancellation and dropped in place on a fresh release,
  with no ESP/use-counter change.
- The existing CR1/Omega mobile checks with those same eight data sets, including
  menu scrolling, equipment hitboxes and 192 crystal exits in total.

LeakSanitizer is disabled because the sandbox uses ptrace; ASan/UBSan remain
active. The callback thread test checks atomic request delivery, not a TSan run.

## Limits

These tests run on desktop and inject SDL events; they do not replace tests on a
physical Android device. This fix cancels interrupted input. It does not implement
application pausing, graphics-context recovery or a fix for the exact earlier
crystal-room crash, which remains unreproduced.

## Follow-up: controllers held through a cancel (2026-10-30)

Clearing the repeat state was not enough. No port pauses on focus loss, and
controller state is read each frame, while SDL keeps it unchanged out of focus. A
direction held at the cancel looked like a new press one frame later. In CR2, 18
more Down presses reached the unfocused game in two seconds; a held stick also
kept walking, and CR2/CR3's held square kept fast-forwarding.

`crinput::HeldOver` holds the controller's direction input (and CR2/CR3's held
square) dead after a cancel until it is let go. Button presses still arrive as
events. The title and new-session boundaries use it too, so a direction held
there does not carry into the next screen. Ordinary map changes do not, so
walking across a map boundary continues. The player has to release the controller
once after an interruption.

Tests: `input-cancel` covers the latch. CR2/CR3's `viewer-input-cancel` covers the
cleanup, and `CR2_PAD_SCRIPT`/`CR3_PAD_SCRIPT` take a `blur` step and log each
press. CR1/Omega's `--selftest` holds a virtual controller through a focus loss.
